#include <stdlib.h>
#include "error.h"
#include "hashtable.h"
#include "pool.h"
#include "typed_array.h"
#include "vm.h"

/* Generational GC -- write barrier and remembered set                   */

/* Every pooled cell's gc_state byte lives at the cell itself (offset 0, see pool.h), not in a
   side table -- so unlike a lookup keyed by which pool a value belongs to, checking any of these
   bits needs nothing beyond the value's own pointer. value_has_cell (below) gates out
   null/boolean/integer/float, which have no cell to point at all. */
static bool value_has_cell(AerVal v);

/* True if v's own pooled cell is young; null/boolean/real (and inline integers) have no cell, so they're
   trivially "not young". */
static bool value_is_young(AerVal v) {
    return value_has_cell(v) && pool_is_young(v.as.ptr);
}

/* Every RememberedKind (ARRAY/DICT/STRUCT) names a pooled type, so its own gc_state byte's
   REMEMBERED bit is always available -- no separate scan of remembered_set needed to dedup. */
static void gc_remember(VmHeap* heap, void* ptr, RememberedKind kind) {
    (void)kind;
    if (pool_is_remembered(ptr))
        return; /* already remembered */
    pool_mark_remembered(ptr);
    if (heap->remembered_count >= heap->remembered_cap) {
        heap->remembered_cap = heap->remembered_cap ? heap->remembered_cap * 2 : 64;
        heap->remembered_set = xrealloc(heap->remembered_set, sizeof(RememberedEntry) * heap->remembered_cap);
    }
    heap->remembered_set[heap->remembered_count].ptr = ptr;
    heap->remembered_set[heap->remembered_count].kind = kind;
    heap->remembered_count++;
}

/* Card marking: grows *dirty_cards to cover `index`, sets that bit, and widens
   [*dirty_min_byte, *dirty_max_byte). Fields are passed by pointer so gc_barrier_array and
   gc_barrier_dict share this. Called on every qualifying write, not just the one that first
   remembers the container. The min/max range is what bounds gc_collect's rescan by how much
   changed rather than by the container's total size. */
static void mark_card_dirty(unsigned char** dirty_cards, unsigned int* dirty_cards_bytes,
                            unsigned int* dirty_min_byte, unsigned int* dirty_max_byte, unsigned int index) {
    unsigned int needed_bytes = index / 8 + 1;
    if (needed_bytes > *dirty_cards_bytes) {
        *dirty_cards = xrealloc(*dirty_cards, needed_bytes);
        memset(*dirty_cards + *dirty_cards_bytes, 0, needed_bytes - *dirty_cards_bytes);
        *dirty_cards_bytes = needed_bytes;
    }
    (*dirty_cards)[index / 8] |= (unsigned char)(1u << (index % 8));
    unsigned int byte_i = index / 8;
    if (byte_i < *dirty_min_byte)
        *dirty_min_byte = byte_i;
    if (byte_i + 1 > *dirty_max_byte)
        *dirty_max_byte = byte_i + 1;
}

/* An old container gaining a young value at `index`: mark that index's card and remember the
   container, so the next minor re-traces only what changed. Nothing can be old before the first
   collection, and a young container is re-traced anyway. */
static void barrier_with_cards(VM* vm, void* cell, RememberedKind kind, unsigned char** cards,
                               unsigned int* cards_bytes, unsigned int* min_byte, unsigned int* max_byte,
                               unsigned int index, AerVal new_value) {
    VmHeap* heap = &vm->heap;
    if (!heap->gc_ever_collected || pool_is_young(cell) || !value_is_young(new_value))
        return;
    mark_card_dirty(cards, cards_bytes, min_byte, max_byte, index);
    gc_remember(heap, cell, kind);
}

/* Array write barrier (index-assign, append, insert) -- `index` is the exact slot new_value lands
   in. collection.delete/insert/sort set dirty_all instead when a write also shifts other elements. */
void gc_barrier_array(VM* vm, AerArray* a, unsigned int index, AerVal new_value) {
    barrier_with_cards(vm, a, REMEMBERED_ARRAY, &a->dirty_cards, &a->dirty_cards_bytes, &a->dirty_min_byte,
                       &a->dirty_max_byte, index, new_value);
}

/* Struct field-set write barrier -- AerStruct has its own pool, so it needs its own barrier. No
   card marking: a struct's field count is small and fixed, so a full rescan is already bounded.
   The O(n^2) problem cards solve is specific to unbounded containers. */
void gc_barrier_struct(VM* vm, AerStruct* s, AerVal new_value) {
    VmHeap* heap = &vm->heap;
    if (!heap->gc_ever_collected)
        return;
    if (pool_is_young(s))
        return;
    if (!value_is_young(new_value))
        return;
    gc_remember(heap, s, REMEMBERED_STRUCT);
}

/* Dict entry write barrier -- `index` is the entry's DENSE index, which the caller resolves before the
   put: an update reuses the entry's stable index, a new key lands at the current count.
   hashtable_remove's swap-compaction invalidates it, which is why collection.delete sets dirty_all. */
void gc_barrier_dict(VM* vm, AerDict* d, unsigned int index, AerVal new_value) {
    barrier_with_cards(vm, d, REMEMBERED_DICT, &d->dirty_cards, &d->dirty_cards_bytes, &d->dirty_min_byte,
                       &d->dirty_max_byte, index, new_value);
}

/* Generational GC -- mark phase                                        */

/* null/boolean/integer/float reference no heap cell (integers are never boxed under the tagged
   representation) -- filtering them out here, once, means every caller (register/stack roots,
   array/dict/result contents) skips the push+later pop-and-dispatch for them, instead of each
   caller needing its own check. Matters most for a large numeric-valued dict/array: without this,
   every entry gets pushed and popped every single GC cycle for nothing. */
static bool value_has_cell(AerVal v) {
    switch (aer_type(v)) {
        case TYPE_STRING:
        case TYPE_ARRAY:
        case TYPE_STRUCT:
        case TYPE_DICT:
        case TYPE_FUNCTION:
        case TYPE_PACKED_ARRAY:
        case TYPE_TYPED_ARRAY:
        case TYPE_RESULT: return true;
        default: return false;
    }
}

static void worklist_push(VmHeap* heap, AerVal v, bool minor) {
    if (!value_has_cell(v))
        return;
    /* An old cell is frozen during a minor cycle: pool_sweep's young_only skip already presumes it
       alive regardless of mark state, and the only legitimate old -> young edge (a write reaching it
       through gc_barrier_array/struct/dict) is captured by the remembered set and replayed
       separately in gc_collect's own `if (minor)` block below -- so there is nothing left for the
       ordinary mark phase to find by recursing into an old object's own children here. This is also
       why no old cell ever carries a mark bit going into a minor sweep (pool.c's pool_sweep). */
    if (minor && !value_is_young(v))
        return;
    MarkWorklist* wl = &heap->gc_worklist;
    if (wl->count >= wl->cap) {
        wl->cap = wl->cap ? wl->cap * 2 : 256;
        wl->items = xrealloc(wl->items, sizeof(AerVal) * wl->cap);
    }
    wl->items[wl->count++] = v;
}

/* Wider containers are traced a chunk at a time, so marking one never copies every element onto
   the worklist at once. */
#define MARK_CHUNK 256

static unsigned int container_count(void* container, bool is_dict) {
    return is_dict ? ((AerDict*)container)->map.count : ((AerArray*)container)->count;
}

static AerVal container_element(void* container, bool is_dict, unsigned int i) {
    return is_dict ? ((AerDict*)container)->map.dense[i].payload : ((AerArray*)container)->items[i];
}

static void worklist_push_elements(VmHeap* heap, void* container, bool is_dict, bool minor) {
    unsigned int count = container_count(container, is_dict);
    if (count <= MARK_CHUNK) {
        for (unsigned int i = 0; i < count; i++)
            worklist_push(heap, container_element(container, is_dict, i), minor);
        return;
    }
    MarkWorklist* wl = &heap->gc_worklist;
    if (wl->range_count >= wl->range_cap) {
        wl->range_cap = wl->range_cap ? wl->range_cap * 2 : 16;
        wl->ranges = xrealloc(wl->ranges, sizeof(MarkRange) * wl->range_cap);
    }
    wl->ranges[wl->range_count++] = (MarkRange){container, 0, is_dict};
}

/* Re-traces a remembered container's elements whose cards were marked since the last minor -- or all of
   them after an operation that shifted indices -- then clears the marks. Bounding the scan and the clear
   to [min_byte, max_byte) is what keeps a pure-growth append loop O(n) rather than O(n^2).
   always_inline: as its own function it moves the code after gc_collect, costing small_dict_bench ~6%. */
static inline __attribute__((always_inline)) void rescan_dirty_cards(VmHeap* heap, void* container,
                                                                     bool is_dict, unsigned char* cards,
                                                                     unsigned int* min_byte,
                                                                     unsigned int* max_byte, bool* all,
                                                                     bool minor) {
    if (*all || !cards) {
        worklist_push_elements(heap, container, is_dict, minor);
    } else {
        unsigned int count = container_count(container, is_dict);
        for (unsigned int byte_i = *min_byte; byte_i < *max_byte; byte_i++) {
            unsigned char byte = cards[byte_i];
            if (!byte)
                continue;
            for (unsigned int bit = 0; bit < 8; bit++) {
                if (!(byte & (1u << bit)))
                    continue;
                unsigned int idx = byte_i * 8 + bit;
                if (idx < count)
                    worklist_push(heap, container_element(container, is_dict, idx), minor);
            }
        }
    }
    if (*max_byte > *min_byte)
        memset(cards + *min_byte, 0, *max_byte - *min_byte);
    *min_byte = (unsigned int)-1;
    *max_byte = 0;
    *all = false;
}

/* Pushes the next chunk of the most recently started container; false once none is left. */
static bool worklist_refill(VmHeap* heap, bool minor) {
    MarkWorklist* wl = &heap->gc_worklist;
    if (wl->range_count == 0)
        return false;
    MarkRange r = wl->ranges[wl->range_count - 1];
    unsigned int count = container_count(r.container, r.is_dict);
    unsigned int end = count - r.next > MARK_CHUNK ? r.next + MARK_CHUNK : count;
    if (end == count)
        wl->range_count--;
    else
        wl->ranges[wl->range_count - 1].next = end;
    for (unsigned int i = r.next; i < end; i++)
        worklist_push(heap, container_element(r.container, r.is_dict, i), minor);
    return true;
}

/* Shared by TYPE_FUNCTION marking and CallFrame root marking (a frame's executing function is a raw
   AerFunction*, not a wrapped AerVal). */
static void mark_function(AerFunction* f) {
    pool_mark(f);
}

static void mark_value(VmHeap* heap, AerVal v, bool minor) {
    switch (aer_type(v)) {
        case TYPE_STRING:
            pool_mark(aer_as_string(v)); /* a leaf -- data owns no other Values */
            break;
        case TYPE_ARRAY: {
            AerArray* a = aer_as_array(v);
            if (!pool_mark(a))
                worklist_push_elements(heap, a, false, minor);
            break;
        }
        case TYPE_STRUCT: {
            /* A struct instance's own type/pool now, not a shaped AerArray -- see AerStruct's own
               comment (vm.h) for why. Only TYPE_ANY fields ever need pushing: a typed (raw) field
               has no tag and is never a GC cell, so treating it as an AerVal here would be a real
               memory-safety bug (reading raw bytes as a fake tagged pointer during mark). */
            AerStruct* s = aer_as_struct(v);
            if (!pool_mark(s)) {
                for (unsigned int i = 0; i < s->shape->field_count; i++)
                    if (s->shape->field_types[i] == TYPE_ANY)
                        worklist_push(heap, vm_struct_field_read(s, i), minor);
            }
            break;
        }
        case TYPE_DICT:
            /* Keys are plain owned char*, not Values -- nothing to push. */
            if (!pool_mark(aer_as_dict(v)))
                worklist_push_elements(heap, aer_as_dict(v), true, minor);
            break;
        case TYPE_FUNCTION: mark_function(aer_as_function(v)); break;
        case TYPE_PACKED_ARRAY:
            /* A GC leaf -- every field is a fixed primitive, never a heap reference. */
            pool_mark(aer_as_packed_array(v));
            break;
        case TYPE_TYPED_ARRAY:
            /* A GC leaf, same reasoning as TYPE_PACKED_ARRAY above -- every element is a fixed
               numeric primitive. */
            pool_mark(aer_as_typed_array(v));
            break;
        case TYPE_RESULT: {
            AerResult* r = aer_as_result(v);
            if (!pool_mark(r)) {
                worklist_push(heap, r->value, minor);
                worklist_push(heap, r->err, minor);
            }
            break;
        }
        default:
            break; /* null/boolean/integer/float reference no heap cell -- integers are never boxed under the tagged representation */
    }
}

static void mark_drain(VmHeap* heap, bool minor) {
    MarkWorklist* wl = &heap->gc_worklist;
    do {
        while (wl->count > 0)
            mark_value(heap, wl->items[--wl->count], minor);
    } while (worklist_refill(heap, minor));
}

/* Pushes every live root in vm's own heap -- vm's stack/call-frame registers only, now that each
   VM collects only itself (no more cross-VM fan-out, see gc_collect). */
static void mark_vm_roots(VmHeap* heap, VM* vm, bool minor) {
    for (int i = 0; i < vm->stack_top; i++)
        worklist_push(heap, vm->stack[i], minor);
    worklist_push(heap, vm->kept, minor); /* actor.keep's value, live between calls */

    /* Scanned unconditionally (zero-init decodes as harmless TYPE_NULL), bounded to the live call
       chain (0..call_depth) and, within a frame, to the slots that can hold a reference at all --
       the frame answers that rather than the GC assuming it, so merging the raw banks into the same
       array later changes the answer and not this loop. */
    for (int f = 0; f <= vm->call_depth; f++) {
        CallFrame* frame = &vm->call_stack[f];
        AerVal* refs;
        unsigned int ref_count = frame_ref_slots(frame, &refs);
        for (unsigned int i = 0; i < ref_count; i++)
            worklist_push(heap, refs[i], minor);
    }
}

/* Chunk.pool and every Shape's field_defaults are permanent roots, walked fresh every cycle since mark
   bits are cleared each sweep. */
static void mark_chunk_roots(VmHeap* heap, Chunk* chunk, bool minor) {
    for (unsigned int i = 0; i < chunk->pool_count; i++)
        worklist_push(heap, chunk->pool[i], minor);
    for (unsigned int s = 0; s < chunk->shape_count; s++) {
        Shape* shape = chunk->shapes[s];
        for (unsigned int i = 0; i < shape->field_count; i++)
            worklist_push(heap, shape->field_defaults[i], minor);
    }
}

/* Generational GC -- sweep finalizers                                  */

/* An inline string owns nothing separate; anything longer holds a payload from
   vm_string_payload_alloc, which re-derives the same size class from `length`. current_heap is this
   cell's own heap for the same reason free_typed_array below can rely on it. */
static void free_string(void* cell) {
    AerString* s = (AerString*)cell;
    if (s->data != s->inline_buf)
        vm_string_payload_free(vm_current_heap(), s->data, s->length);
}
static void free_array(void* cell) {
    AerArray* a = (AerArray*)cell;
    free(a->items);
    free(a->dirty_cards);
}
static void free_dict(void* cell) {
    AerDict* d = (AerDict*)cell;
    hashtable_free(&d->map);
    free(d->dirty_cards);
} /* hashtable_free already frees every entry's key */
static void free_function(void* cell) {
    (void)cell;
} /* nothing to free -- no closure upvalues array anymore */
static void free_struct(void* cell) {
    (void)cell;
} /* items lives inline in this same cell -- nothing separate to free */
static void free_packed_array(void* cell) {
    free(((AerPackedArray*)cell)->data);
}
/* Stashes the data buffer into current_heap's free-cache rather than freeing it, when a slot is
   free and the buffer qualifies -- vm_new_typed_array checks that cache before xmalloc, so a
   repeatedly-rebuilt same-size array reuses the buffer. current_heap is guaranteed to be this
   cell's own heap: vm_run_slice sets it for normal GC, and vm_free brackets teardown with it. */
static void free_typed_array(void* cell) {
    AerTypedArray* ta = (AerTypedArray*)cell;
    if (!ta->data)
        return;
    size_t size = (size_t)ta->count * vm_typed_elem_width(ta->elem_kind);
    VmHeap* heap = vm_current_heap();
    if (heap && size > 0 &&
        heap->typed_array_free_cache_bytes + size <= TYPED_ARRAY_FREE_CACHE_MAX_TOTAL_BYTES) {
        for (unsigned int i = 0; i < TYPED_ARRAY_FREE_CACHE_SLOTS; i++) {
            if (heap->typed_array_free_cache[i].size == 0) {
                heap->typed_array_free_cache[i].size = size;
                heap->typed_array_free_cache[i].ptr = ta->data;
                heap->typed_array_free_cache_bytes += size;
                return;
            }
        }
    }
    free(ta->data);
}
static void free_result(void* cell) {
    (void)cell;
} /* both fields are plain AerVals -- nothing separately owned */

/* Each owning pool's payload, sized from the same fields its finalizer above frees. */
static size_t payload_bytes_string(void* cell) {
    AerString* s = (AerString*)cell;
    return s->data != s->inline_buf ? (size_t)s->length + 1 : 0;
}
static size_t payload_bytes_array(void* cell) {
    return (size_t)((AerArray*)cell)->capacity * sizeof(AerVal);
}
static size_t payload_bytes_dict(void* cell) {
    HashTable* map = &((AerDict*)cell)->map;
    return (size_t)map->dense_capacity * sizeof(HashTableEntry) +
           (size_t)map->capacity * sizeof(unsigned int);
}
static size_t payload_bytes_packed_array(void* cell) {
    AerPackedArray* pa = (AerPackedArray*)cell;
    return (size_t)pa->count * pa->shape->instance_bytes;
}
static size_t payload_bytes_typed_array(void* cell) {
    AerTypedArray* ta = (AerTypedArray*)cell;
    return (size_t)ta->count * vm_typed_elem_width(ta->elem_kind);
}

/* Every pool a VmHeap owns, by field offset rather than raw pointer, paired with its finalizer.
   The single place all pools are listed; gc_finalize_all_pools, pool_sweep and gc_count_live_cells
   all walk this instead of hand-written lists -- which is how gc_count_live_cells once silently
   omitted result_pool. struct_pools contributes one entry per size-class tier. */
typedef struct {
    size_t offset;
    void (*on_free)(void* cell);
    size_t (*payload_bytes)(void* cell); /* NULL when the cell owns nothing separately */
} PoolEntry;
static const PoolEntry pool_table[] = {
    {offsetof(VmHeap, string_pool), free_string, payload_bytes_string},
    {offsetof(VmHeap, array_pool), free_array, payload_bytes_array},
    {offsetof(VmHeap, dict_pool), free_dict, payload_bytes_dict},
    {offsetof(VmHeap, function_pool), free_function, NULL},
    {offsetof(VmHeap, struct_pools[0]), free_struct, NULL},
    {offsetof(VmHeap, struct_pools[1]), free_struct, NULL},
    {offsetof(VmHeap, struct_pools[2]), free_struct, NULL},
    {offsetof(VmHeap, struct_pools[3]), free_struct, NULL},
    {offsetof(VmHeap, struct_pools[4]), free_struct, NULL},
    {offsetof(VmHeap, packed_array_pool), free_packed_array, payload_bytes_packed_array},
    {offsetof(VmHeap, typed_array_pool), free_typed_array, payload_bytes_typed_array},
    {offsetof(VmHeap, result_pool), free_result, NULL},
};
#define POOL_TABLE_COUNT (sizeof(pool_table) / sizeof(pool_table[0]))
_Static_assert(STRUCT_PAYLOAD_TIER_COUNT == 5,
               "pool_table above hardcodes 5 struct_pools[] entries -- update both together");

static inline Pool* pool_at(VmHeap* heap, size_t offset) {
    return (Pool*)((char*)heap + offset);
}

/* vm_free's own teardown call (vm.c) -- every cell finalized regardless of mark/generation state,
   since the whole heap is going away, not just the garbage since the last cycle (contrast
   gc_collect's pool_sweep calls below, which only finalize actual garbage). */
void gc_finalize_all_pools(VmHeap* heap) {
    for (size_t i = 0; i < POOL_TABLE_COUNT; i++)
        pool_finalize_all(pool_at(heap, pool_table[i].offset), pool_table[i].on_free);
}

/* Generational GC -- collection                                        */

/* Collects only vm's own heap, against only vm's own roots -- each VM owns an independent heap, so
   there is no other VM's state to fan out into. File-modules and actors each collect themselves the
   same way, whenever THEIR OWN gc_maybe_collect fires. */
static void gc_collect(VM* vm, bool minor, PoolSweepTally* tally) {
    VmHeap* heap = &vm->heap;

    mark_vm_roots(heap, vm, minor);
    mark_chunk_roots(heap, vm->chunk, minor);

    if (minor) {
        /* Remembered old objects, traced as extra roots (a minor pass skips old cells otherwise). A
           remembered entry can outlive its object (never proactively removed), so check liveness
           before dereferencing and compact in place. */
        unsigned int kept = 0;
        for (unsigned int i = 0; i < heap->remembered_count; i++) {
            RememberedEntry* e = &heap->remembered_set[i];
            if (pool_is_freed(e->ptr))
                continue; /* e->kind is irrelevant -- the byte lives on e->ptr's own cell regardless of which pool */

            switch (e->kind) {
                case REMEMBERED_ARRAY: {
                    AerArray* a = (AerArray*)e->ptr;
                    rescan_dirty_cards(heap, a, false, a->dirty_cards, &a->dirty_min_byte, &a->dirty_max_byte,
                                       &a->dirty_all, minor);
                    break;
                }
                case REMEMBERED_STRUCT: {
                    /* Only TYPE_ANY fields -- see mark_value's TYPE_STRUCT case for why a raw
                       field must never be pushed as if it were a tagged AerVal. No card marking
                       here -- see gc_barrier_struct's own comment for why a struct's small, fixed
                       field count doesn't need it. */
                    AerStruct* s = (AerStruct*)e->ptr;
                    for (unsigned int j = 0; j < s->shape->field_count; j++)
                        if (s->shape->field_types[j] == TYPE_ANY)
                            worklist_push(heap, vm_struct_field_read(s, j), minor);
                    break;
                }
                case REMEMBERED_DICT: {
                    AerDict* d = (AerDict*)e->ptr;
                    rescan_dirty_cards(heap, d, true, d->dirty_cards, &d->dirty_min_byte, &d->dirty_max_byte,
                                       &d->dirty_all, minor);
                    break;
                }
            }
            heap->remembered_set[kept++] = *e;
        }
        heap->remembered_count = kept;
    }

    mark_drain(heap, minor);

    for (size_t i = 0; i < POOL_TABLE_COUNT; i++)
        pool_sweep(pool_at(heap, pool_table[i].offset), minor, pool_table[i].on_free,
                   pool_table[i].payload_bytes, tally);
}

/* Generational GC -- trigger                                           */

/* Tuning defaults, overridable via aer_gc_configure(), are applied in vm_heap_init (heap.c). */

/* A full walk of every pool. A major sweep reports its own live count as it goes, so this is only
   for callers with no collection to piggyback on: aer_gc_stats, and the ceiling check after a minor. */
unsigned int gc_count_live_cells(VmHeap* heap) {
    unsigned int total = 0;
    for (size_t t = 0; t < POOL_TABLE_COUNT; t++) {
        Pool* pool = pool_at(heap, pool_table[t].offset);
        for (unsigned int i = 0; i < pool->slab_count; i++) {
            unsigned int count = (i == pool->slab_count - 1) ? pool->next_index : pool->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                char* cell = pool->slabs[i] + (size_t)j * pool->stride;
                if (!(*(unsigned char*)cell & POOL_FREE))
                    total++;
            }
        }
    }
    return total;
}

/* Returns the live cell count, for the ceiling; the old generation is sized in bytes. */
static unsigned int gc_major(VM* vm) {
    VmHeap* heap = &vm->heap;
    PoolSweepTally live = {0, 0};
    gc_collect(vm, false, &live);
    heap->major_collections_run++;
    heap->old_bytes = heap->old_bytes_after_major = live.bytes;
    return live.cells;
}

/* Floored at the nursery, or a nearly empty heap would run a major after every minor. */
static bool gc_old_generation_grown(VmHeap* heap) {
    size_t base = heap->old_bytes_after_major > heap->nursery_bytes ? heap->old_bytes_after_major
                                                                    : heap->nursery_bytes;
    return (uint64_t)heap->old_bytes >= (uint64_t)base * heap->growth_factor;
}

/* Runs only between complete opcodes, where stack/scope/frame invariants are consistent. Called
   from gc_maybe_collect (vm_internal.h, always_inline, checked once per DISPATCH()) once the rare
   threshold-crossing case actually happens -- the common case never reaches this file at all. */
void gc_run_collection_cycle(VM* vm) {
    VmHeap* heap = &vm->heap;
    heap->gc_ever_collected = true;
    PoolSweepTally promoted = {0, 0};
    gc_collect(vm, true, &promoted);
    heap->minor_collections_run++;
    heap->young_bytes = 0;
    heap->old_bytes += promoted.bytes;

    bool major_ran = false;
    unsigned int live = 0;
    if (gc_old_generation_grown(heap)) {
        live = gc_major(vm);
        major_ran = true;
    }

    /* Checked once per opcode, not per allocation -- a ceiling'd host can slip slightly past it. */
    if (heap->gc_live_cell_ceiling == 0)
        return;
    if (!major_ran)
        live = gc_count_live_cells(heap); /* a minor did not visit the old cells, so it owes a full walk */
    if (live <= heap->gc_live_cell_ceiling)
        return;
    if (!major_ran) {
        live = gc_major(vm);
        if (live <= heap->gc_live_cell_ceiling)
            return;
    }
    error("Memory ceiling exceeded: %u live cells (limit %u)", live, heap->gc_live_cell_ceiling);
}

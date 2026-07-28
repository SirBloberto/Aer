#include <stdlib.h>
#include "error.h"
#include "hashtable.h"
#include "pool.h"
#include "vm.h"

/* ------------------------------------------------------------------ */
/* Generational GC — write barrier and remembered set                   */
/* ------------------------------------------------------------------ */

/* True if v's own pooled cell is young; null/boolean/real (and inline integers) have no cell, so they're trivially "not young". */
static bool value_is_young(VmHeap* heap, AerVal v) {
    switch (aer_type(v)) {
        case TYPE_STRING:   return pool_is_young(&heap->string_pool,   aer_as_string(v));
        case TYPE_ARRAY:    return pool_is_young(&heap->array_pool,    aer_as_array(v));
        case TYPE_STRUCT:   return pool_is_young(&heap->struct_pool,   aer_as_struct(v));
        case TYPE_DICT:     return pool_is_young(&heap->dict_pool,     aer_as_dict(v));
        case TYPE_FUNCTION: return pool_is_young(&heap->function_pool, aer_as_function(v));
        case TYPE_PACKED_ARRAY: return pool_is_young(&heap->packed_array_pool, aer_as_packed_array(v));
        case TYPE_TYPED_ARRAY: return pool_is_young(&heap->typed_array_pool, aer_as_typed_array(v));
        case TYPE_RESULT:   return pool_is_young(&heap->result_pool,   aer_as_result(v));
        default:            return false;   /* null/boolean/integer/float have no heap cell — integers are never boxed under the tagged representation */
    }
}

/* Pool for a remembered pointer's kind, so gc_remember can dedup via its cell's REMEMBERED bit (O(1)) instead of scanning remembered_set. */
static Pool* remembered_pool_for(VmHeap* heap, void* ptr, RememberedKind kind) {
    (void)ptr;
    switch (kind) {
        case REMEMBERED_ARRAY:  return &heap->array_pool;
        case REMEMBERED_DICT:   return &heap->dict_pool;
        case REMEMBERED_STRUCT: return &heap->struct_pool;
    }
    return NULL;
}

static void gc_remember(VmHeap* heap, void* ptr, RememberedKind kind) {
    Pool* p = remembered_pool_for(heap, ptr, kind);
    if (p) {
        if (pool_is_remembered(p, ptr)) return;   /* already remembered */
        pool_mark_remembered(p, ptr);
    } else {
        for (unsigned int i = 0; i < heap->remembered_count; i++)
            if (heap->remembered_set[i].ptr == ptr) return;   /* already remembered */
    }
    if (heap->remembered_count >= heap->remembered_cap) {
        heap->remembered_cap = heap->remembered_cap ? heap->remembered_cap * 2 : 64;
        heap->remembered_set = xrealloc(heap->remembered_set, sizeof(RememberedEntry) * heap->remembered_cap);
    }
    heap->remembered_set[heap->remembered_count].ptr  = ptr;
    heap->remembered_set[heap->remembered_count].kind = kind;
    heap->remembered_count++;
}

/* Card marking: grows *dirty_cards (if needed) to cover `index`, then sets that bit. Shared by
   gc_barrier_array/gc_barrier_dict -- AerArray and AerDict aren't a common type in C, so the two
   relevant fields are passed by pointer instead of duplicating this logic twice. Called on EVERY
   qualifying write, not just the one that first adds the container to remembered_set (gc_remember's
   own dedup only governs remembered_set membership, not which indices need rescanning). */
static void mark_card_dirty(unsigned char** dirty_cards, unsigned int* dirty_cards_bytes, unsigned int index) {
    unsigned int needed_bytes = index / 8 + 1;
    if (needed_bytes > *dirty_cards_bytes) {
        *dirty_cards = xrealloc(*dirty_cards, needed_bytes);
        memset(*dirty_cards + *dirty_cards_bytes, 0, needed_bytes - *dirty_cards_bytes);
        *dirty_cards_bytes = needed_bytes;
    }
    (*dirty_cards)[index / 8] |= (unsigned char)(1u << (index % 8));
}

/* Array write barrier (index-assign, append, insert) -- `index` is the exact slot new_value lands
   in, so the next minor GC's REMEMBERED_ARRAY rescan (gc_collect below) only has to revisit that
   one slot instead of the whole array. See AerArray.dirty_cards's own comment (value.h) for the
   dirty_all fallback collection.delete/insert/sort use instead, when a write also shifts OTHER
   elements' indices. */
void gc_barrier_array(VM* vm, AerArray* a, unsigned int index, AerVal new_value) {
    VmHeap* heap = &vm->heap;
    if (!heap->gc_ever_collected) return;   /* nothing can be old yet — see gc_ever_collected's own comment */
    if (pool_is_young(&heap->array_pool, a)) return;   /* young containers are re-traced normally next cycle */
    if (!value_is_young(heap, new_value)) return;
    mark_card_dirty(&a->dirty_cards, &a->dirty_cards_bytes, index);
    gc_remember(heap, a, REMEMBERED_ARRAY);
}

/* Struct field-set write barrier -- AerStruct is its own type/pool now, not a shaped AerArray, so
   it needs its own barrier instead of gc_barrier_array's old shape-ternary dispatch. Only ever
   needs to consider TYPE_ANY fields in practice (a raw typed field can never hold a reference the
   GC must trace), but the caller doesn't need to know that -- value_is_young already returns false
   for a primitive regardless. No card marking here -- a struct's field count is small and fixed
   (MAX_STRUCT_FIELDS), so a full per-struct rescan is already bounded, unlike an unboundedly
   growing array/dict; the O(n^2) problem this fixes is specific to unbounded containers. */
void gc_barrier_struct(VM* vm, AerStruct* s, AerVal new_value) {
    VmHeap* heap = &vm->heap;
    if (!heap->gc_ever_collected) return;
    if (pool_is_young(&heap->struct_pool, s)) return;
    if (!value_is_young(heap, new_value)) return;
    gc_remember(heap, s, REMEMBERED_STRUCT);
}

/* Dict entry write barrier (update-in-place and new-entry paths) -- `index` is the entry's DENSE
   index (map.dense[index]), which the caller must resolve BEFORE the actual hashtable_get_hashed/
   hashtable_put_hashed call: an update reuses an existing entry's stable dense index, while a fresh
   key always lands at the table's current count (hashtable_get_index_hashed, hashtable.c). Stable
   across ordinary insert/update; hashtable_remove's swap-compaction invalidates it, which is why
   collection.delete sets dirty_all instead (see AerDict.dirty_cards's own comment, vm.h). */
void gc_barrier_dict(VM* vm, AerDict* d, unsigned int index, AerVal new_value) {
    VmHeap* heap = &vm->heap;
    if (!heap->gc_ever_collected) return;   /* nothing can be old yet — see gc_ever_collected's own comment */
    if (pool_is_young(&heap->dict_pool, d)) return;
    if (!value_is_young(heap, new_value)) return;
    mark_card_dirty(&d->dirty_cards, &d->dirty_cards_bytes, index);
    gc_remember(heap, d, REMEMBERED_DICT);
}

/* ------------------------------------------------------------------ */
/* Generational GC — mark phase                                        */
/* ------------------------------------------------------------------ */

/* null/boolean/integer/float reference no heap cell (integers are never boxed under the tagged
   representation) -- filtering them out here, once, means every caller (register/stack roots,
   array/dict/result contents) skips the push+later pop-and-dispatch for them, instead of each
   caller needing its own check. Matters most for a large numeric-valued dict/array: without this,
   every entry gets pushed and popped every single GC cycle for nothing. */
static bool value_has_cell(AerVal v) {
    switch (aer_type(v)) {
        case TYPE_STRING: case TYPE_ARRAY: case TYPE_STRUCT: case TYPE_DICT: case TYPE_FUNCTION:
        case TYPE_PACKED_ARRAY: case TYPE_TYPED_ARRAY: case TYPE_RESULT:
            return true;
        default:
            return false;
    }
}

static void worklist_push(VmHeap* heap, AerVal v, bool minor) {
    if (!value_has_cell(v)) return;
    /* An old cell is frozen during a minor cycle: pool_sweep's young_only skip already presumes it
       alive regardless of mark state, and the only legitimate old -> young edge (a write reaching it
       through gc_barrier_array/struct/dict) is captured by the remembered set and replayed
       separately in gc_collect's own `if (minor)` block below -- so there is nothing left for the
       ordinary mark phase to find by recursing into an old object's own children here. This is what
       makes it safe for pool_clear_marks/pool_sweep to also skip old cells during a minor cycle (see
       their own comments, pool.c/pool.h): if nothing ever marks an old cell here, nothing needs to
       unmark or sweep-check it either. */
    if (minor && !value_is_young(heap, v)) return;
    MarkWorklist* wl = &heap->gc_worklist;
    if (wl->count >= wl->cap) {
        wl->cap   = wl->cap ? wl->cap * 2 : 256;
        wl->items = xrealloc(wl->items, sizeof(AerVal) * wl->cap);
    }
    wl->items[wl->count++] = v;
}

/* Shared by TYPE_FUNCTION marking and CallFrame root marking (a frame's executing function is a raw AerFunction*, not a wrapped AerVal). */
static void mark_function(VmHeap* heap, AerFunction* f) {
    pool_mark(&heap->function_pool, f);
}

static void mark_value(VmHeap* heap, AerVal v, bool minor) {
    switch (aer_type(v)) {
        case TYPE_STRING:
            pool_mark(&heap->string_pool, aer_as_string(v));   /* a leaf — data owns no other Values */
            break;
        case TYPE_ARRAY: {
            AerArray* a = aer_as_array(v);
            if (!pool_mark(&heap->array_pool, a)) {
                for (unsigned int i = 0; i < a->count; i++)
                    worklist_push(heap, a->items[i], minor);
            }
            break;
        }
        case TYPE_STRUCT: {
            /* A struct instance's own type/pool now, not a shaped AerArray -- see AerStruct's own
               comment (vm.h) for why. Only TYPE_ANY fields ever need pushing: a typed (raw) field
               has no tag and is never a GC cell, so treating it as an AerVal here would be a real
               memory-safety bug (reading raw bytes as a fake tagged pointer during mark). */
            AerStruct* s = aer_as_struct(v);
            if (!pool_mark(&heap->struct_pool, s)) {
                for (unsigned int i = 0; i < s->shape->field_count; i++)
                    if (s->shape->field_types[i] == TYPE_ANY)
                        worklist_push(heap, vm_struct_field_read(s, i), minor);
            }
            break;
        }
        case TYPE_DICT:
            if (!pool_mark(&heap->dict_pool, aer_as_dict(v))) {
                HashTable* map = &aer_as_dict(v)->map;
                for (unsigned int i = 0; i < map->count; i++)
                    worklist_push(heap, map->dense[i].payload, minor);
                /* Keys are plain owned char*, not Values — nothing to push. */
            }
            break;
        case TYPE_FUNCTION:
            mark_function(heap, aer_as_function(v));
            break;
        case TYPE_PACKED_ARRAY:
            /* A GC leaf -- every field is a fixed primitive, never a heap reference. */
            pool_mark(&heap->packed_array_pool, aer_as_packed_array(v));
            break;
        case TYPE_TYPED_ARRAY:
            /* A GC leaf, same reasoning as TYPE_PACKED_ARRAY above -- every element is a fixed
               numeric primitive. */
            pool_mark(&heap->typed_array_pool, aer_as_typed_array(v));
            break;
        case TYPE_RESULT: {
            AerResult* r = aer_as_result(v);
            if (!pool_mark(&heap->result_pool, r)) {
                worklist_push(heap, r->value, minor);
                worklist_push(heap, r->err, minor);
            }
            break;
        }
        default:
            break;   /* null/boolean/integer/float reference no heap cell — integers are never boxed under the tagged representation */
    }
}

static void mark_drain(VmHeap* heap, bool minor) {
    MarkWorklist* wl = &heap->gc_worklist;
    while (wl->count > 0)
        mark_value(heap, wl->items[--wl->count], minor);
}

/* Pushes every live root in vm's own heap -- vm's stack/call-frame registers only, now that each
   VM collects only itself (no more cross-VM fan-out, see gc_collect). */
static void mark_vm_roots(VmHeap* heap, VM* vm, bool minor) {
    for (int i = 0; i < vm->stack_top; i++)
        worklist_push(heap, vm->stack[i], minor);

    /* Scanned unconditionally (zero-init decodes as harmless TYPE_NULL), bounded to the live call
       chain (0..call_depth, by each frame's real frame_size) -- a blanket scan over every frame was
       a measured cache-miss hotspot, and frames past call_depth are already dead. */
    for (int f = 0; f <= vm->call_depth; f++)
        for (unsigned int i = 0; i < vm->call_stack[f].frame_size; i++)
            worklist_push(heap, vm->call_stack[f].registers[i], minor);
}

/* Chunk.pool and every Shape's field_defaults are permanent roots, walked fresh every cycle since mark bits are cleared each sweep. */
static void mark_chunk_roots(VmHeap* heap, Chunk* chunk, bool minor) {
    for (unsigned int i = 0; i < chunk->pool_count; i++)
        worklist_push(heap, chunk->pool[i], minor);
    for (unsigned int s = 0; s < chunk->shape_count; s++) {
        Shape* shape = chunk->shapes[s];
        for (unsigned int i = 0; i < shape->field_count; i++)
            worklist_push(heap, shape->field_defaults[i], minor);
    }
}

/* ------------------------------------------------------------------ */
/* Generational GC — sweep finalizers                                  */
/* ------------------------------------------------------------------ */

static void free_string(void* cell)   { free(((AerString*)cell)->data); }
static void free_array(void* cell)    { AerArray* a = (AerArray*)cell; free(a->items); free(a->dirty_cards); }
static void free_dict(void* cell)     { AerDict* d = (AerDict*)cell; hashtable_free(&d->map); free(d->dirty_cards); }   /* hashtable_free already frees every entry's key */
static void free_function(void* cell) { (void)cell; }   /* nothing to free — no closure upvalues array anymore */
static void free_struct(void* cell)   { (void)cell; }   /* items lives inline in this same cell — nothing separate to free */
static void free_packed_array(void* cell) { free(((AerPackedArray*)cell)->data); }
static void free_typed_array(void* cell)  { free(((AerTypedArray*)cell)->data); }
static void free_result(void* cell)   { (void)cell; }   /* both fields are plain AerVals — nothing separately owned */

/* vm_free's own teardown call (vm.c) -- every cell finalized regardless of mark/generation state,
   since the whole heap is going away, not just the garbage since the last cycle (contrast
   gc_collect's pool_sweep calls below, which only finalize actual garbage). One cohesive call
   rather than exposing all 7 individual finalizer functions just for vm_free's one use. */
void gc_finalize_all_pools(VmHeap* heap) {
    pool_finalize_all(&heap->string_pool,       free_string);
    pool_finalize_all(&heap->array_pool,        free_array);
    pool_finalize_all(&heap->dict_pool,         free_dict);
    pool_finalize_all(&heap->function_pool,     free_function);
    pool_finalize_all(&heap->struct_pool,       free_struct);
    pool_finalize_all(&heap->packed_array_pool, free_packed_array);
    pool_finalize_all(&heap->typed_array_pool,  free_typed_array);
    pool_finalize_all(&heap->result_pool,       free_result);
}

/* ------------------------------------------------------------------ */
/* Generational GC — collection                                        */
/* ------------------------------------------------------------------ */

/* Collects only vm's own heap, against only vm's own roots -- each VM now owns an independent
   heap, so there is no other VM's state to fan out into (file-modules and actors used to be marked
   here too, since they all shared one heap; each now collects itself the same way, whenever ITS
   OWN gc_maybe_collect fires). */
static void gc_collect(VM* vm, bool minor) {
    VmHeap* heap = &vm->heap;
    /* Must run before marking every cycle. With young_only (minor), old cells are skipped entirely --
       correct because the mark phase itself now never sets an old cell's mark bit during a minor
       cycle either (worklist_push's own skip, above), so there is nothing to clear for them. */
    pool_clear_marks(&heap->string_pool, minor);
    pool_clear_marks(&heap->array_pool, minor);
    pool_clear_marks(&heap->dict_pool, minor);
    pool_clear_marks(&heap->function_pool, minor);
    pool_clear_marks(&heap->struct_pool, minor);
    pool_clear_marks(&heap->packed_array_pool, minor);
    pool_clear_marks(&heap->typed_array_pool, minor);
    pool_clear_marks(&heap->result_pool, minor);

    mark_vm_roots(heap, vm, minor);
    mark_chunk_roots(heap, vm->chunk, minor);

    if (minor) {
        /* Remembered old objects, traced as extra roots (a minor pass skips old cells otherwise). A
           remembered entry can outlive its object (never proactively removed), so check liveness
           before dereferencing and compact in place. */
        unsigned int kept = 0;
        for (unsigned int i = 0; i < heap->remembered_count; i++) {
            RememberedEntry* e = &heap->remembered_set[i];
            bool alive;
            switch (e->kind) {
                case REMEMBERED_ARRAY:  alive = !pool_is_freed(&heap->array_pool,  e->ptr); break;
                case REMEMBERED_STRUCT: alive = !pool_is_freed(&heap->struct_pool, e->ptr); break;
                case REMEMBERED_DICT:   alive = !pool_is_freed(&heap->dict_pool,   e->ptr); break;
                default: alive = false; break;
            }
            if (!alive) continue;

            switch (e->kind) {
                case REMEMBERED_ARRAY: {
                    /* Card-marked: only the indices actually dirtied since the last rescan get
                       revisited (turning a pure-growth "build a huge array via many appends"
                       pattern into true O(n) total instead of O(n^2)) -- dirty_all is the fallback
                       for an operation that shifts element-to-index correspondence instead
                       (collection.delete/insert/sort, aer_collection.c), and !dirty_cards is a
                       defensive fallback that should never actually trigger (every write reaching
                       this array via gc_barrier_array already dirties a card before remembering
                       it), kept anyway rather than assumed. */
                    AerArray* a = (AerArray*)e->ptr;
                    if (a->dirty_all || !a->dirty_cards) {
                        for (unsigned int j = 0; j < a->count; j++) worklist_push(heap, a->items[j], minor);
                    } else {
                        for (unsigned int byte_i = 0; byte_i < a->dirty_cards_bytes; byte_i++) {
                            unsigned char byte = a->dirty_cards[byte_i];
                            if (!byte) continue;
                            for (unsigned int bit = 0; bit < 8; bit++) {
                                if (!(byte & (1u << bit))) continue;
                                unsigned int idx = byte_i * 8 + bit;
                                if (idx < a->count) worklist_push(heap, a->items[idx], minor);
                            }
                        }
                    }
                    if (a->dirty_cards) memset(a->dirty_cards, 0, a->dirty_cards_bytes);
                    a->dirty_all = false;
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
                    /* Same card-marked scan as REMEMBERED_ARRAY above, indexed by dense slot. */
                    AerDict* d = (AerDict*)e->ptr;
                    HashTable* map = &d->map;
                    if (d->dirty_all || !d->dirty_cards) {
                        for (unsigned int j = 0; j < map->count; j++)
                            worklist_push(heap, map->dense[j].payload, minor);
                    } else {
                        for (unsigned int byte_i = 0; byte_i < d->dirty_cards_bytes; byte_i++) {
                            unsigned char byte = d->dirty_cards[byte_i];
                            if (!byte) continue;
                            for (unsigned int bit = 0; bit < 8; bit++) {
                                if (!(byte & (1u << bit))) continue;
                                unsigned int idx = byte_i * 8 + bit;
                                if (idx < map->count) worklist_push(heap, map->dense[idx].payload, minor);
                            }
                        }
                    }
                    if (d->dirty_cards) memset(d->dirty_cards, 0, d->dirty_cards_bytes);
                    d->dirty_all = false;
                    break;
                }
            }
            heap->remembered_set[kept++] = *e;
        }
        heap->remembered_count = kept;
    }

    mark_drain(heap, minor);

    pool_sweep(&heap->string_pool,   minor, free_string);
    pool_sweep(&heap->array_pool,    minor, free_array);
    pool_sweep(&heap->dict_pool,     minor, free_dict);
    pool_sweep(&heap->function_pool, minor, free_function);
    pool_sweep(&heap->struct_pool,   minor, free_struct);
    pool_sweep(&heap->packed_array_pool, minor, free_packed_array);
    pool_sweep(&heap->typed_array_pool, minor, free_typed_array);
    pool_sweep(&heap->result_pool,   minor, free_result);
}

/* ------------------------------------------------------------------ */
/* Generational GC — trigger                                           */
/* ------------------------------------------------------------------ */

/* Tuning defaults (DEFAULT_MINOR_GC_THRESHOLD/DEFAULT_MAJOR_GC_EVERY_N_MINOR, overridable per-heap
   via aer_gc_configure()) are applied in vm_heap_init (vm.c) -- VmHeap's zero-init obviously can't
   carry these non-zero defaults itself. */

static void gc_reset_alloc_counts(VmHeap* heap) {
    heap->pool_alloc_count = 0;
}

/* Shared by aer_gc_stats (vm.c) and gc_run_collection_cycle's own ceiling check below — one place walking all pools' cell state, not two. */
unsigned int gc_count_live_cells(VmHeap* heap) {
    unsigned int total = 0;
    Pool* pools[] = { &heap->string_pool, &heap->array_pool, &heap->dict_pool, &heap->function_pool,
                      &heap->struct_pool, &heap->packed_array_pool, &heap->typed_array_pool };
    for (unsigned int p = 0; p < sizeof(pools) / sizeof(pools[0]); p++) {
        Pool* pool = pools[p];
        for (unsigned int i = 0; i < pool->slab_count; i++) {
            unsigned int count = (i == pool->slab_count - 1) ? pool->next_index : pool->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                char* cell = pool->slabs[i] + (size_t)j * pool->stride;
                if (!(*(unsigned char*)cell & POOL_FREE)) total++;
            }
        }
    }
    return total;
}

/* Runs only between complete opcodes, where stack/scope/frame invariants are consistent. Called
   from gc_maybe_collect (vm.c, always_inline, checked once per DISPATCH()) once the rare
   threshold-crossing case actually happens -- the common case never reaches this file at all. */
void gc_run_collection_cycle(VM* vm) {
    VmHeap* heap = &vm->heap;
    heap->gc_ever_collected = true;
    gc_collect(vm, true);
    heap->minor_collections_run++;
    gc_reset_alloc_counts(heap);

    bool major_ran = false;
    if (++heap->minor_since_major >= heap->major_gc_every_n_minor) {
        gc_collect(vm, false);
        heap->major_collections_run++;
        heap->minor_since_major = 0;
        major_ran = true;
    }

    /* Checked once per opcode, not per allocation -- a ceiling'd host can slip slightly past it. */
    if (heap->gc_live_cell_ceiling == 0) return;
    unsigned int live = gc_count_live_cells(heap);
    if (live <= heap->gc_live_cell_ceiling) return;
    if (!major_ran) {
        gc_collect(vm, false);
        heap->major_collections_run++;
        heap->minor_since_major = 0;
        live = gc_count_live_cells(heap);
        if (live <= heap->gc_live_cell_ceiling) return;
    }
    error("Memory ceiling exceeded: %u live cells (limit %u)", live, heap->gc_live_cell_ceiling);
}

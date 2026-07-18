#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer_host.h"
#include "aer_module.h"
#include "aer_stdlib.h"
#include "error.h"
#include "pool.h"
#include "strbuf.h"
#include "vm.h"

/* Slab pools for heap types confirmed (via every free() site) to never be freed individually — alloc-speed only. Guarded since vm_init() reruns per VM/module import and would otherwise leak slabs. */
static Pool string_pool, array_pool, dict_pool, function_pool, struct_pool, packed_array_pool, result_pool;
static bool pools_initialized = false;

/* struct_pool holds struct instances (AerArray with shape != NULL) as ONE allocation instead of
   the usual header-plus-separate-items-buffer two-allocation layout ordinary arrays use — see the
   struct-construction sites below for why this is sound (a struct's field count never changes
   after construction, so there's no reallocation to support, unlike a plain array's items[]).
   Sized for MAX_STRUCT_FIELDS (the worst case) since Pool requires uniform cell size — wastes some
   space for shapes with fewer fields, a bounded, deliberate trade for collapsing two dependent
   pointer dereferences (header, then its separately-allocated items[]) into one on every struct
   field access. */
/* Test-only accessor (tests/smoke_test.c is the only intended caller) — reads back a register's
   final value after a chunk has run to OP_HALT. Reads whichever frame is currently active, which
   is frame 0 (the top level) once a chunk has run to completion with every call balanced by a
   return. See CallFrame's own comment in vm.h for the call_stack/registers/
   call_depth fields this reads — per-VM-instance so a nested file-module VM (aer_module.c) gets
   its own isolated call chain instead of sharing/corrupting the calling VM's in-progress one. */
AerVal register_get(VM* vm, int slot) {
    return vm->registers[slot];
}

/* Decodes an ordinary (32-bit, RK_CONST_FLAG-at-bit-30) RK operand — see RK_CONST_FLAG's comment
   in vm.h. Used by every opcode except the packed-single-word OP_BINARY below, which has its own
   narrower RK20 scheme instead. */
static inline AerVal vm_rk_value(VM* vm, Chunk* c, int rk) {
    if (rk & RK_CONST_FLAG) return c->pool[rk & ~RK_CONST_FLAG];
    return vm->registers[rk];
}

/* Decodes one of the wider 20-bit RK operands (RK20_CONST_FLAG at bit 19, vm.h) still used by
   every packed opcode other than OP_BINARY's own family (OP_FIELD_SET, OP_INDEX_GET, the fused
   field-arithmetic ops, etc. — see vm_rk_ptr9 below for OP_BINARY's narrower scheme). Returns a
   pointer straight into the pool/register array (hoisted `const_pool` — see vm_run's top — rather
   than re-deriving c->pool on every call) instead of copying the 16-byte AerVal out by value:
   every call site either dereferences once at its single point of use, or forwards the pointer
   into an always_inline consumer (vm_binary_fast, vm_truthy, vm_index_get/set_compute), so the value
   is fetched from its home location without an intermediate copy sitting in between. */
static inline AerVal* vm_rk_ptr20(VM* vm, AerVal* const_pool, uint64_t rk) {
    if (rk & RK20_CONST_FLAG) return &const_pool[rk & RK20_INDEX_MASK];
    return &vm->registers[rk & RK20_INDEX_MASK];
}

/* Decodes one of PACK_BINARY's compact 9-bit RK operands (RK9_CONST_FLAG at bit 8, vm.h) — the
   arithmetic/comparison/bitwise/OP_IN family's own narrower RK scheme, sized so the whole packed
   word fits in the low 32 bits (see PACK_BINARY's own comment in vm.h for why that matters on this
   32-bit ARM target). Same pointer-return shape as vm_rk_ptr20 above, same reasoning.
     A branch-free variant of this (reading a per-frame copy of the constant pool instead of
   const_pool directly, so the flag bit could fold into arithmetic) was tried and reverted: it
   required giving every call frame its own pool-allocated window, which made calls themselves
   slower. Lua/V8 don't try to eliminate this branch either — they accept it and instead make calls
   themselves free (a bump-pointer register stack, see VM.register_stack), which is the direction
   this codebase went instead. */
static inline AerVal* vm_rk_ptr9(VM* vm, AerVal* const_pool, uint32_t rk) {
    if (rk & RK9_CONST_FLAG) return &const_pool[rk & RK9_INDEX_MASK];
    return &vm->registers[rk & RK9_INDEX_MASK];
}

static void vm_pools_init_once(void) {
    if (pools_initialized) return;
    pool_init(&string_pool,   sizeof(AerString),   256);
    pool_init(&array_pool,    sizeof(AerArray),    256);
    pool_init(&dict_pool,     sizeof(AerDict),      64);
    pool_init(&function_pool, sizeof(AerFunction),  64);
    pool_init(&struct_pool,   sizeof(AerArray) + MAX_STRUCT_FIELDS * sizeof(AerVal), 64);
    pool_init(&packed_array_pool, sizeof(AerPackedArray), 64);
    pool_init(&result_pool,   sizeof(AerResult),   64);
    hashtable_pools_init_once();
    pools_initialized = true;
}

/* ------------------------------------------------------------------ */
/* Generational GC — write barrier and remembered set                   */
/* ------------------------------------------------------------------ */

/* True if v's own pooled cell is young; null/boolean/real (and inline integers) have no cell, so they're trivially "not young". */
static bool value_is_young(AerVal v) {
    switch (aer_type(v)) {
        case TYPE_STRING:   return pool_is_young(&string_pool,   aer_as_string(v));
        case TYPE_ARRAY:    return pool_is_young(&array_pool,    aer_as_array(v));
        case TYPE_DICT:     return pool_is_young(&dict_pool,     aer_as_dict(v));
        case TYPE_FUNCTION: return pool_is_young(&function_pool, aer_as_function(v));
        case TYPE_PACKED_ARRAY: return pool_is_young(&packed_array_pool, aer_as_packed_array(v));
        case TYPE_RESULT:   return pool_is_young(&result_pool,   aer_as_result(v));
        default:            return false;   /* null/boolean/integer/real have no heap cell — integers are never boxed under the tagged representation */
    }
}

typedef enum { REMEMBERED_ARRAY, REMEMBERED_DICT, REMEMBERED_STRUCT } RememberedKind;
typedef struct { void* ptr; RememberedKind kind; } RememberedEntry;

/* Old objects a write barrier caught holding a young reference; entries are only ever added/deduped, never removed, and re-traced as extra roots on every minor collection thereafter. */
static RememberedEntry* remembered_set   = NULL;
static unsigned int     remembered_count = 0;
static unsigned int     remembered_cap   = 0;

/* Pool for a remembered pointer's kind, so gc_remember can dedup via its cell's REMEMBERED bit (O(1)) instead of scanning remembered_set. */
static Pool* remembered_pool_for(void* ptr, RememberedKind kind) {
    (void)ptr;
    switch (kind) {
        case REMEMBERED_ARRAY:  return &array_pool;
        case REMEMBERED_DICT:   return &dict_pool;
        case REMEMBERED_STRUCT: return &struct_pool;
    }
    return NULL;
}

static void gc_remember(void* ptr, RememberedKind kind) {
    Pool* p = remembered_pool_for(ptr, kind);
    if (p) {
        if (pool_is_remembered(p, ptr)) return;   /* already remembered */
        pool_mark_remembered(p, ptr);
    } else {
        for (unsigned int i = 0; i < remembered_count; i++)
            if (remembered_set[i].ptr == ptr) return;   /* already remembered */
    }
    if (remembered_count >= remembered_cap) {
        remembered_cap = remembered_cap ? remembered_cap * 2 : 64;
        remembered_set = xrealloc(remembered_set, sizeof(RememberedEntry) * remembered_cap);
    }
    remembered_set[remembered_count].ptr  = ptr;
    remembered_set[remembered_count].kind = kind;
    remembered_count++;
}

/* Set once, forever, the first time gc_run_collection_cycle actually runs — never cleared. Guards
   gc_barrier_array/gc_barrier_dict: POOL_OLD is only ever set by pool_sweep's promotion step, which
   only ever runs inside a collection cycle, so before this flag is true nothing in any pool can be
   old — the write barrier is provably a no-op for every write until then, not just usually one. */
static bool gc_ever_collected = false;

/* Write barrier for array item writes (index-assign, append, struct field-set via lbl_field_set —
   a struct is an array with a shape, now backed by struct_pool instead of array_pool — see
   vm_pools_init_once). Branches on a->shape to pick the right pool/remembered-kind pair, mirroring
   gc_barrier_dict's single-pool pattern but for whichever of the two pools actually owns `a`. */
static void gc_barrier_array(AerArray* a, AerVal new_value) {
    if (!gc_ever_collected) return;   /* nothing can be old yet — see gc_ever_collected's own comment */
    Pool* p = a->shape ? &struct_pool : &array_pool;
    if (pool_is_young(p, a)) return;   /* young containers are already
                                          re-traced normally next cycle */
    if (!value_is_young(new_value)) return;
    gc_remember(a, a->shape ? REMEMBERED_STRUCT : REMEMBERED_ARRAY);
}

/* Write barrier for dict entry writes (both the update-in-place and
   new-entry paths in lbl_index_set). `d` is always dict_pool-tracked. */
static void gc_barrier_dict(AerDict* d, AerVal new_value) {
    if (!gc_ever_collected) return;   /* nothing can be old yet — see gc_ever_collected's own comment */
    if (pool_is_young(&dict_pool, d)) return;
    if (!value_is_young(new_value)) return;
    gc_remember(d, REMEMBERED_DICT);
}

/* ------------------------------------------------------------------ */
/* Generational GC — mark phase                                        */
/* ------------------------------------------------------------------ */

/* Explicit growable worklist, not C recursion, since user data structures have no depth limit; pool_mark's "already marked" return terminates cycles correctly. */
typedef struct {
    AerVal*      items;
    unsigned int count, cap;
} MarkWorklist;

static MarkWorklist gc_worklist = {0};

static void worklist_push(AerVal v) {
    if (gc_worklist.count >= gc_worklist.cap) {
        gc_worklist.cap   = gc_worklist.cap ? gc_worklist.cap * 2 : 256;
        gc_worklist.items = xrealloc(gc_worklist.items, sizeof(AerVal) * gc_worklist.cap);
    }
    gc_worklist.items[gc_worklist.count++] = v;
}

/* Shared by TYPE_FUNCTION marking and CallFrame root marking (a frame's executing function is a raw AerFunction*, not a wrapped AerVal). */
static void mark_function(AerFunction* f) {
    pool_mark(&function_pool, f);
}

static void mark_value(AerVal v) {
    switch (aer_type(v)) {
        case TYPE_STRING:
            pool_mark(&string_pool, aer_as_string(v));   /* a leaf — data owns no other Values */
            break;
        case TYPE_ARRAY: {
            AerArray* a = aer_as_array(v);
            Pool* p = a->shape ? &struct_pool : &array_pool;   /* see vm_pools_init_once */
            if (!pool_mark(p, a)) {
                for (unsigned int i = 0; i < a->count; i++)
                    worklist_push(a->items[i]);
            }
            break;
        }
        case TYPE_DICT:
            if (!pool_mark(&dict_pool, aer_as_dict(v))) {
                HashTable* map = &aer_as_dict(v)->map;
                for (unsigned int i = 0; i < map->capacity; i++)
                    if (map->buckets[i].key)
                        worklist_push(map->buckets[i].payload);
                /* Bucket keys are plain owned char*, not Values — nothing to push. */
            }
            break;
        case TYPE_FUNCTION:
            mark_function(aer_as_function(v));
            break;
        case TYPE_PACKED_ARRAY:
            /* A leaf, unlike TYPE_ARRAY — every field is a fixed primitive (integer/float/boolean,
               enforced at construction), never a heap reference, so there's nothing to push onto
               the worklist. */
            pool_mark(&packed_array_pool, aer_as_packed_array(v));
            break;
        case TYPE_RESULT: {
            AerResult* r = aer_as_result(v);
            if (!pool_mark(&result_pool, r)) {
                worklist_push(r->value);
                worklist_push(r->err);
            }
            break;
        }
        default:
            break;   /* null/boolean/integer/real reference no heap cell — integers are never boxed under the tagged representation */
    }
}

static void mark_drain(void) {
    while (gc_worklist.count > 0)
        mark_value(gc_worklist.items[--gc_worklist.count]);
}

/* Pushes every live root in one VM; called once for the calling VM and once per file-module VM (aer_module_get) — the five pools are one shared heap fed by N independent root sets, not N separate collectors. */
static void mark_vm_roots(VM* vm) {
    for (int i = 0; i < vm->stack_top; i++)
        worklist_push(vm->stack[i]);

    /* Registers can hold heap references (arrays/dicts/strings/functions). Scanned
       unconditionally, not tracked for liveness: an idle register is zero-init, which decodes as
       TYPE_NULL (value.h — the tag field defaults to 0 on zero-init) and is a harmless no-op leaf
       in mark_value's default case. Scanned per-frame, bounded to the live call chain
       (0..call_depth) rather than all VM_CALL_MAX frames regardless of depth — a blanket scan
       over every frame was a real, measured cache-miss hotspot, since even shallow recursion was
       walking every frame's worth of cold, mostly-zeroed memory on every GC pass. Frames beyond
       call_depth are dead (already returned), so bounding the scan to the live call chain can't
       under-collect. frame_size, not FRAME_REGISTERS — frames pack contiguously in
       vm->register_stack, so scanning a flat 128 per frame would re-visit deeper frames'
       overlapping windows and mark stale values left by already-returned calls. */
    for (int f = 0; f <= vm->call_depth; f++)
        for (unsigned int i = 0; i < vm->call_stack[f].frame_size; i++)
            worklist_push(vm->call_stack[f].registers[i]);
}

/* Chunk.pool and every Shape's field_defaults are permanent roots, walked fresh every cycle since mark bits are cleared each sweep. */
static void mark_chunk_roots(Chunk* chunk) {
    for (unsigned int i = 0; i < chunk->pool_count; i++)
        worklist_push(chunk->pool[i]);
    for (unsigned int s = 0; s < chunk->shape_count; s++) {
        Shape* shape = chunk->shapes[s];
        for (unsigned int i = 0; i < shape->field_count; i++)
            worklist_push(shape->field_defaults[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Generational GC — sweep finalizers                                  */
/* ------------------------------------------------------------------ */

static void free_string(void* cell)   { free(((AerString*)cell)->data); }
static void free_array(void* cell)    { free(((AerArray*)cell)->items); }
static void free_dict(void* cell)     { hashtable_free(&((AerDict*)cell)->map); }   /* already frees every entry's key */
static void free_function(void* cell) { (void)cell; }   /* nothing to free — no closure upvalues array anymore */
static void free_struct(void* cell)   { (void)cell; }   /* items lives inline in this same cell — nothing separate to free */
static void free_packed_array(void* cell) { free(((AerPackedArray*)cell)->data); }
static void free_result(void* cell)   { (void)cell; }   /* both fields are plain AerVals — nothing separately owned */

/* ------------------------------------------------------------------ */
/* Generational GC — collection                                        */
/* ------------------------------------------------------------------ */

static void gc_collect(VM* vm, bool minor) {
    /* Must run before marking every cycle: a minor sweep never visits old cells, so without this an old cell's mark bit would stay set forever and never be re-traced. */
    pool_clear_marks(&string_pool);
    pool_clear_marks(&array_pool);
    pool_clear_marks(&dict_pool);
    pool_clear_marks(&function_pool);
    pool_clear_marks(&struct_pool);
    pool_clear_marks(&packed_array_pool);
    pool_clear_marks(&result_pool);

    mark_vm_roots(vm);
    mark_chunk_roots(vm->chunk);
    for (unsigned int i = 0; ; i++) {
        VM* mvm; Chunk* mchunk;
        if (!aer_module_get(i, &mvm, &mchunk)) break;
        mark_vm_roots(mvm);
        mark_chunk_roots(mchunk);
    }

    if (minor) {
        /* Old objects the write barrier caught holding a young reference, traced as extra
           roots since a minor pass never looks at old cells otherwise. A MAJOR pass can
           legitimately free a remembered object between when it was added and now (entries
           are never proactively removed), so check liveness before dereferencing each one
           and compact the array in place rather than leaving a dangling pointer. */
        unsigned int kept = 0;
        for (unsigned int i = 0; i < remembered_count; i++) {
            RememberedEntry* e = &remembered_set[i];
            bool alive;
            switch (e->kind) {
                case REMEMBERED_ARRAY:  alive = !pool_is_freed(&array_pool,  e->ptr); break;
                case REMEMBERED_STRUCT: alive = !pool_is_freed(&struct_pool, e->ptr); break;
                case REMEMBERED_DICT:   alive = !pool_is_freed(&dict_pool,   e->ptr); break;
                default: alive = false; break;
            }
            if (!alive) continue;

            switch (e->kind) {
                case REMEMBERED_ARRAY:
                case REMEMBERED_STRUCT: {
                    AerArray* a = (AerArray*)e->ptr;
                    for (unsigned int j = 0; j < a->count; j++) worklist_push(a->items[j]);
                    break;
                }
                case REMEMBERED_DICT: {
                    HashTable* map = &((AerDict*)e->ptr)->map;
                    for (unsigned int j = 0; j < map->capacity; j++)
                        if (map->buckets[j].key) worklist_push(map->buckets[j].payload);
                    break;
                }
            }
            remembered_set[kept++] = *e;
        }
        remembered_count = kept;
    }

    mark_drain();

    pool_sweep(&string_pool,   minor, free_string);
    pool_sweep(&array_pool,    minor, free_array);
    pool_sweep(&dict_pool,     minor, free_dict);
    pool_sweep(&function_pool, minor, free_function);
    pool_sweep(&struct_pool,   minor, free_struct);
    pool_sweep(&packed_array_pool, minor, free_packed_array);
    pool_sweep(&result_pool,   minor, free_result);
}

/* ------------------------------------------------------------------ */
/* Generational GC — trigger                                           */
/* ------------------------------------------------------------------ */

/* Tuning defaults (overridable via aer_gc_configure()): minor_gc_threshold is total cells allocated across all pools since the last minor GC; major_gc_every_n_minor runs a major pass after that many minor ones. */
static unsigned int minor_gc_threshold     = 2048;
static unsigned int major_gc_every_n_minor = 10;

/* 0 (the default) means unlimited — see aer_gc_set_ceiling. */
static unsigned int gc_live_cell_ceiling = 0;

static unsigned int minor_collections_run = 0;
static unsigned int major_collections_run = 0;
static unsigned int minor_since_major     = 0;

static void gc_reset_alloc_counts(void) {
    pool_total_alloc_count = 0;
}

/* Shared by aer_gc_stats and gc_maybe_collect's ceiling check — one place walking all pools' cell state, not two. */
static unsigned int gc_count_live_cells(void) {
    unsigned int total = 0;
    Pool* pools[] = { &string_pool, &array_pool, &dict_pool, &function_pool, &struct_pool,
                      &packed_array_pool };
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

static int gc_suppress_depth = 0;

void vm_gc_suppress(void)   { gc_suppress_depth++; }
void vm_gc_unsuppress(void) { if (gc_suppress_depth > 0) gc_suppress_depth--; }

void aer_gc_configure(unsigned int minor_threshold, unsigned int major_every_n_minor) {
    if (minor_threshold)    minor_gc_threshold     = minor_threshold;
    if (major_every_n_minor) major_gc_every_n_minor = major_every_n_minor;
}

void aer_gc_set_ceiling(unsigned int max_live_cells) {
    gc_live_cell_ceiling = max_live_cells;
}

/* Split out from gc_maybe_collect so that stays small enough to inline into DISPATCH(). Only
   ever runs between two complete opcodes, where stack/scope/call-frame invariants are
   self-consistent (no handler leaves those half-updated across its own DISPATCH() call). */
static void gc_run_collection_cycle(VM* vm) {
    gc_ever_collected = true;
    gc_collect(vm, true);
    minor_collections_run++;
    gc_reset_alloc_counts();

    bool major_ran = false;
    if (++minor_since_major >= major_gc_every_n_minor) {
        gc_collect(vm, false);
        major_collections_run++;
        minor_since_major = 0;
        major_ran = true;
    }

    /* Ceiling check runs AFTER the normal trigger logic so a ceiling'd host still gets the
       usual cheap minor/major rhythm; checked once per opcode (not per-allocation), so one
       opcode's worth of allocation can slip past the ceiling before the abort fires — a
       deliberate, minor looseness. */
    if (gc_live_cell_ceiling == 0) return;
    unsigned int live = gc_count_live_cells();
    if (live <= gc_live_cell_ceiling) return;
    if (!major_ran) {
        gc_collect(vm, false);
        major_collections_run++;
        minor_since_major = 0;
        live = gc_count_live_cells();
        if (live <= gc_live_cell_ceiling) return;
    }
    error("Memory ceiling exceeded: %u live cells (limit %u)", live, gc_live_cell_ceiling);
}

/* Checked once per opcode from DISPATCH(); kept tiny and always_inline so the common case (nowhere near threshold) costs nothing beyond what's already inlined into the dispatch loop. */
static inline __attribute__((always_inline)) void gc_maybe_collect(VM* vm) {
    if (gc_suppress_depth > 0) return;
    if (pool_total_alloc_count < minor_gc_threshold) return;
    gc_run_collection_cycle(vm);
}

/* Embedding-facing introspection (include/aer.h); live_cells is a bookkeeping snapshot, not a fresh trace, so it undercounts unswept-but-garbage cells since the last cycle. */
void aer_gc_stats(unsigned int* live_cells, unsigned int* minor_collections,
                  unsigned int* major_collections) {
    if (live_cells)         *live_cells         = gc_count_live_cells();
    if (minor_collections)  *minor_collections  = minor_collections_run;
    if (major_collections)  *major_collections  = major_collections_run;
}

#ifdef AER_DEBUG_TOOLS
/* Byte-accurate memory report — aer_gc_stats() only gives a live *cell* count, which understates
   real usage for string/array/dict: their pool cell is a fixed-size header only, the actual
   payload (string bytes, array items[], dict hash buckets) is a separate xmalloc'd/xrealloc'd
   allocation the pool system doesn't track at all. Walks each pool the same way
   gc_count_live_cells does, but reads each live cell's own size fields instead of just counting. */
void aer_debug_memory_report(FILE* out) {
    fprintf(out, "\n--- memory ---\n");

    uint64_t str_hdr = 0, str_payload = 0;
    {
        Pool* p = &string_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerString* s = (AerString*)(p->slabs[i] + (size_t)j * p->stride);
                if (s->gc_state & POOL_FREE) continue;
                str_hdr += sizeof(AerString);
                str_payload += s->length;
            }
        }
    }
    fprintf(out, "  string   header %10llu B  payload %10llu B\n", str_hdr, str_payload);

    uint64_t arr_hdr = 0, arr_payload = 0;
    {
        Pool* p = &array_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerArray* a = (AerArray*)(p->slabs[i] + (size_t)j * p->stride);
                if (a->gc_state & POOL_FREE) continue;
                arr_hdr += sizeof(AerArray);
                arr_payload += (uint64_t)a->capacity * sizeof(AerVal);
            }
        }
    }
    fprintf(out, "  array    header %10llu B  payload %10llu B\n", arr_hdr, arr_payload);

    uint64_t dict_hdr = 0, dict_payload = 0;
    {
        Pool* p = &dict_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerDict* d = (AerDict*)(p->slabs[i] + (size_t)j * p->stride);
                if (d->gc_state & POOL_FREE) continue;
                dict_hdr += sizeof(AerDict);
                dict_payload += (uint64_t)d->map.capacity * sizeof(HashTableEntry);
                for (unsigned int b = 0; b < d->map.capacity; b++)
                    if (d->map.buckets[b].key) dict_payload += d->map.buckets[b].length + 1;
            }
        }
    }
    fprintf(out, "  dict     header %10llu B  payload %10llu B\n", dict_hdr, dict_payload);

    uint64_t fn_hdr = 0, fn_payload = 0;
    {
        Pool* p = &function_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerFunction* f = (AerFunction*)(p->slabs[i] + (size_t)j * p->stride);
                if (f->gc_state & POOL_FREE) continue;
                fn_hdr += sizeof(AerFunction);
                if (f->defaults) fn_payload += (uint64_t)(f->arity - f->min_arity) * sizeof(AerVal);
            }
        }
    }
    fprintf(out, "  function header %10llu B  payload %10llu B\n", fn_hdr, fn_payload);

    /* struct_pool cells are fixed-size (sizeof(AerArray) + MAX_STRUCT_FIELDS*sizeof(AerVal)) —
       "header" here is the fixed per-cell reservation, "payload" is the sum of each live
       instance's ACTUAL field_count*sizeof(AerVal), so the gap between the two is exactly the
       over-provisioning cost the MAX_STRUCT_FIELDS-sized single pool trades for one allocation
       instead of two — see vm_pools_init_once. */
    uint64_t struct_hdr = 0, struct_payload = 0;
    {
        Pool* p = &struct_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerArray* a = (AerArray*)(p->slabs[i] + (size_t)j * p->stride);
                if (a->gc_state & POOL_FREE) continue;
                struct_hdr += p->stride;
                struct_payload += (uint64_t)a->count * sizeof(AerVal);
            }
        }
    }
    fprintf(out, "  struct   reserved %9llu B  used %10llu B\n", struct_hdr, struct_payload);

    unsigned int live, minor, major;
    aer_gc_stats(&live, &minor, &major);
    fprintf(out, "  %u live cells, %u minor collections, %u major collections\n", live, minor, major);
}
#endif

/* ------------------------------------------------------------------ */
/* Chunk management                                                     */
/* ------------------------------------------------------------------ */

void chunk_init(Chunk* c) {
    memset(c, 0, sizeof(*c));
}

void chunk_free(Chunk* c) {
    free(c->code);
    for (unsigned int i = 0; i < c->pool_count; i++)
        if (aer_type(c->pool[i]) == TYPE_STRING) free(aer_as_string(c->pool[i])->data);
    free(c->pool);
    hashtable_free(&c->name_index);
    free(c->line_mark_offsets);
    free(c->line_mark_lines);
    for (unsigned int i = 0; i < c->import_count; i++) free(c->imported_modules[i]);
    free(c->imported_modules);
    /* functions[]/shapes[] are otherwise deliberately left unfreed for a chunk's whole life (see
       their own comments, vm.h) since a Chunk normally lives for the process's life anyway — but
       chunk_free itself is only ever reached via aer_module_free_all(), the one path that exists
       specifically so an embedding host can reclaim memory WITHOUT exiting the process, so it must
       actually free everything rather than rely on process exit to do it. */
    for (unsigned int i = 0; i < c->function_count; i++) free(c->functions[i].defaults);
    free(c->functions);
    for (unsigned int i = 0; i < c->shape_count; i++) free(c->shapes[i]);
    free(c->shapes);
    /* Not each entry's shape — every populated slot's Shape* is owned by c->shapes, never separately owned. */
    free(c->field_cache);
#ifdef AER_DEBUG_TOOLS
    free(c->debug_hits);
#endif
    memset(c, 0, sizeof(*c));
}

void chunk_mark_line(Chunk* c, unsigned int offset, unsigned int line) {
    if (c->line_mark_count > 0 && c->line_mark_offsets[c->line_mark_count - 1] >= offset) return;
    if (c->line_mark_count >= c->line_mark_cap) {
        c->line_mark_cap = c->line_mark_cap ? c->line_mark_cap * 2 : 64;
        c->line_mark_offsets = xrealloc(c->line_mark_offsets, sizeof(unsigned int) * c->line_mark_cap);
        c->line_mark_lines   = xrealloc(c->line_mark_lines,   sizeof(unsigned int) * c->line_mark_cap);
    }
    c->line_mark_offsets[c->line_mark_count] = offset;
    c->line_mark_lines[c->line_mark_count]   = line;
    c->line_mark_count++;
}

unsigned int chunk_line_for_offset(Chunk* c, unsigned int offset) {
    if (c->line_mark_count == 0) return 0;
    unsigned int lo = 0, hi = c->line_mark_count;   /* find first mark with offset > target */
    while (lo < hi) {
        unsigned int mid = lo + (hi - lo) / 2;
        if (c->line_mark_offsets[mid] <= offset) lo = mid + 1;
        else                                     hi = mid;
    }
    return lo == 0 ? 0 : c->line_mark_lines[lo - 1];
}

/* Whichever VM is currently dispatching, kept fresh by DISPATCH() each opcode; self-corrects after a nested module call's vm_run() returns since the outer VM reasserts itself next dispatch. */
static VM* active_vm_for_errors = NULL;

static unsigned int lookup_runtime_line(void) {
    if (!active_vm_for_errors) return 0;
    return chunk_line_for_offset(active_vm_for_errors->chunk, active_vm_for_errors->ip);
}

/* Wraps (data, length) — caller must already exclusively own data — in a fresh heap box; never
   allocates or copies the character data itself. vm_pools_init_once() is normally reached via
   vm_init() before anything compiles, but the lexer can call this (via emit_string_token, for
   every identifier/string token) during compilation itself, before any VM exists yet. Without this
   guard, string_pool is still the zero-initialized static (elem_size=0, elems_per_slab=0), so
   pool_alloc's slab xmalloc(0*0) hands back a ~1-byte allocation that this function then writes a
   pointer into — a real heap-buffer-overflow (confirmed via ASAN). */
AerVal aer_make_string(char* data, unsigned int length) {
    vm_pools_init_once();
    AerString* s = pool_alloc(&string_pool);
    s->data = data;
    s->length = length;
    return aer_string_val(s);
}

void chunk_emit(Chunk* c, uint64_t word) {
    if (c->count >= c->capacity) {
        c->capacity = c->capacity ? c->capacity * 2 : 64;
        c->code = xrealloc(c->code, sizeof(uint64_t) * c->capacity);
    }
    c->code[c->count++] = word;
}

/* Appends v to the pool and returns its index; shared tail for both paths of chunk_add_pool. */
static unsigned int chunk_pool_append(Chunk* c, AerVal v) {
    if (c->pool_count >= c->pool_cap) {
        c->pool_cap = c->pool_cap ? c->pool_cap * 2 : 16;
        c->pool = xrealloc(c->pool, sizeof(AerVal) * c->pool_cap);
    }
    c->pool[c->pool_count] = v;
    return c->pool_count++;
}

unsigned int chunk_add_pool(Chunk* c, AerVal v) {
    /* Strings dominate call volume and the REPL never resets the pool between lines, so dedup them via name_index (O(1)) instead of the O(n) linear scan below, kept for rarer non-string literals. */
    if (aer_type(v) == TYPE_STRING) {
        /* Tokens are substrings of the source buffer, not NUL-terminated — build an owned copy first. */
        AerString* vs = aer_as_string(v);
        char* key = xmalloc(vs->length + 1);
        memcpy(key, vs->data, vs->length);
        key[vs->length] = '\0';

        AerVal* existing = hashtable_get(&c->name_index, key);
        if (existing) { free(key); return (unsigned int)aer_as_int(*existing); }

        /* vs->data was already an owned, single-reference buffer at every call site (e.g. the
           lexer's emit_string_token freshly xmalloc's one per token) — free it before replacing
           it with `key`, or it's orphaned with nothing left pointing to it. Confirmed as a real
           leak via LeakSanitizer (Raspberry Pi ASAN build) once the unrelated pool-initialization
           crash that had been masking it was fixed. */
        char* old_data = vs->data;
        vs->data = key;   /* pool entry takes ownership of `key` */
        free(old_data);
        unsigned int idx = chunk_pool_append(c, v);

        /* Independent copy, not an alias of c->pool[idx]'s, so both can be freed independently without a double-free. */
        char* index_key = hashtable_key_dup(key, (unsigned int)strlen(key), NULL);
        hashtable_put(&c->name_index, index_key, aer_int((int64_t)idx));
        return idx;
    }

    for (unsigned int i = 0; i < c->pool_count; i++) {
        AerVal* e = &c->pool[i];
        if (aer_type(*e) != aer_type(v)) continue;
        if (aer_type(v) == TYPE_NULL)                                                  return i;
        if (aer_type(v) == TYPE_INTEGER && aer_as_int(*e)  == aer_as_int(v))  return i;
        if (aer_type(v) == TYPE_REAL    && aer_as_real(*e) == aer_as_real(v)) return i;
        if (aer_type(v) == TYPE_BOOLEAN && aer_as_bool(*e) == aer_as_bool(v)) return i;
        if (aer_type(v) == TYPE_FUNCTION &&
            aer_as_function(*e)->code_offset == aer_as_function(v)->code_offset &&
            aer_as_function(*e)->arity       == aer_as_function(v)->arity) return i;
    }
    return chunk_pool_append(c, v);
}

/* Newest-first so a redeclared struct (e.g. re-running a REPL block) shadows the old one for new lookups, without invalidating instances still pointing at the old Shape. */
Shape* chunk_find_shape(Chunk* c, const char* name) {
    for (unsigned int i = c->shape_count; i > 0; i--) {
        Shape* s = c->shapes[i - 1];
        if (strcmp(aer_as_string(c->pool[s->name])->data, name) == 0) return s;
    }
    return NULL;
}

/* See ChunkFunction's own comment in vm.h. Appended by func_register (parser.c) at the same
   moment it updates its own parse-time-only lookup tables. */
void chunk_add_function(Chunk* c, unsigned int name_idx, unsigned int code_offset,
                         unsigned int arity, unsigned int min_arity, AerVal* defaults) {
    if (c->function_count >= c->function_cap) {
        c->function_cap = c->function_cap ? c->function_cap * 2 : 8;
        c->functions = xrealloc(c->functions, sizeof(ChunkFunction) * c->function_cap);
    }
    ChunkFunction* f = &c->functions[c->function_count++];
    f->name          = name_idx;
    f->code_offset   = code_offset;
    f->arity         = arity;
    f->min_arity     = min_arity;
    f->defaults      = defaults;
    /* Safe placeholder until parse_function patches in the real captured peak after the body
       finishes compiling — see ChunkFunction's own comment. Never an under-allocation even for the
       one case that can read it before the patch runs (self-reference from within this same
       function's own body). */
    f->max_registers = FRAME_REGISTERS;
}

/* Newest-first, same convention as chunk_find_shape. */
ChunkFunction* chunk_find_function(Chunk* c, const char* name) {
    for (unsigned int i = c->function_count; i > 0; i--) {
        ChunkFunction* f = &c->functions[i - 1];
        if (strcmp(aer_as_string(c->pool[f->name])->data, name) == 0) return f;
    }
    return NULL;
}

/* Parse-time lookup — name_idx is a dedup'd pool index (chunk_add_pool), so this is a plain int
   compare, no strcmp. Newest-first, same convention as chunk_find_function. */
ChunkFunction* chunk_find_function_by_name_idx(Chunk* c, unsigned int name_idx) {
    for (unsigned int i = c->function_count; i > 0; i--) {
        ChunkFunction* f = &c->functions[i - 1];
        if (f->name == name_idx) return f;
    }
    return NULL;
}

bool chunk_is_imported(Chunk* c, const char* name, unsigned int len) {
    for (unsigned int i = 0; i < c->import_count; i++)
        if (strlen(c->imported_modules[i]) == len && strncmp(c->imported_modules[i], name, len) == 0)
            return true;
    return false;
}

bool chunk_add_import(Chunk* c, const char* name, unsigned int len,
                       const char* path_name, unsigned int path_len) {
    /* Anything not a native/host module is attempted as a file-based import; aer_module_load() reports its own errors for that path. */
    if (!aer_stdlib_is_native_module(name, len) && !aer_host_is_module(name, len) &&
        !aer_module_load(name, len, path_name, path_len)) {
        return false;
    }
    if (chunk_is_imported(c, name, len)) return true;   /* re-importing is harmless, not an error */
    if (c->import_count >= c->import_cap) {
        c->import_cap = c->import_cap ? c->import_cap * 2 : 8;
        c->imported_modules = xrealloc(c->imported_modules, sizeof(char*) * c->import_cap);
    }
    char* copy = xmalloc(len + 1);
    memcpy(copy, name, len);
    copy[len] = '\0';
    c->imported_modules[c->import_count++] = copy;
    return true;
}

/* ------------------------------------------------------------------ */
/* VM lifecycle                                                         */
/* ------------------------------------------------------------------ */

void vm_init(VM* vm, Chunk* chunk) {
    memset(vm, 0, sizeof(*vm));
    vm->chunk = chunk;
    aer_stdlib_init();
    vm_pools_init_once();
    runtime_line_lookup = lookup_runtime_line;
    /* Resets this VM's own call stack for a fresh run — a chunk that ended mid-call (a bug, or
       a deliberately unbalanced test) must not leak into this VM's next run. Frame 0's registers
       base never moves again after this — the top level's own usage is open-ended (globals persist
       and can grow across REPL statements), so unlike a real function call it always gets a flat
       FRAME_REGISTERS reservation. */
    vm->call_depth = 0;
    vm->call_stack[0].registers  = &vm->register_stack[0];
    vm->call_stack[0].frame_size = FRAME_REGISTERS;
    vm->registers  = vm->call_stack[0].registers;
    vm->raw_ints   = vm->call_stack[0].raw_ints;
    vm->raw_reals  = vm->call_stack[0].raw_reals;
}

void vm_free(VM* vm) {
    (void)vm;   /* nothing to free — every allocation a VM makes lives in the shared pools */
}

/* ------------------------------------------------------------------ */
/* Type helpers                                                         */
/* ------------------------------------------------------------------ */

/* Struct instances report their declared name (e.g. "Player") instead of "array" — used by type() and OP_CHECK_SHAPE's error message. A packed array reports "Player[]" — distinct from a single instance's own "Player". type_names[] is indexed directly by ValueType, so it must stay exactly as long as the enum's non-specially-handled entries (value.h) — TYPE_PACKED_ARRAY is handled specially, just like TYPE_ARRAY+shape, so it's never used to index this array. */
static const char* vm_type_name(Chunk* c, AerVal v) {
    static const char* type_names[] = {
        "null", "boolean", "integer", "real", "string", "function", "array", "dict"
    };
    if (aer_type(v) == TYPE_ARRAY && aer_as_array(v)->shape)
        return aer_as_string(c->pool[aer_as_array(v)->shape->name])->data;
    if (aer_type(v) == TYPE_PACKED_ARRAY) {
        /* static buf is safe only because every call site consumes the result immediately (copies
           or formats it) before this function could be called again — never hold this return value
           across a second call. */
        AerPackedArray* pa = aer_as_packed_array(v);
        static char buf[128];
        snprintf(buf, sizeof(buf), "%s[]", aer_as_string(c->pool[pa->shape->name])->data);
        return buf;
    }
    if (aer_type(v) == TYPE_RESULT) return "Result";
    return type_names[aer_type(v)];
}

void aer_format_real(double d, char* buf, size_t bufsize) {
    snprintf(buf, bufsize, "%g", d);
    /* '.'/'e'/'E' already mark an ordinary real; 'n'/'N'/'i'/'I' cover every case spelling of
       "nan"/"inf"/"-inf" — none of those need (or should get) a trailing ".0" appended. */
    if (!strpbrk(buf, ".eEnNiI")) {
        size_t len = strlen(buf);
        if (len + 3 <= bufsize) { buf[len] = '.'; buf[len + 1] = '0'; buf[len + 2] = '\0'; }
    }
}

/* ------------------------------------------------------------------ */
/* Value formatting — shared by print() and vm_to_str() (interpolation, +, etc.) for one consistent recursive rendering, not a terse "<array[3]>" fallback. */
/* ------------------------------------------------------------------ */

static void vm_format_value(Chunk* c, AerVal v, bool in_collection, StrBuf* sb);

static void vm_format_value(Chunk* c, AerVal v, bool in_collection, StrBuf* sb) {
    char tmp[64];
    switch (aer_type(v)) {
        case TYPE_NULL:     strbuf_append(sb, "null"); break;
        case TYPE_INTEGER:  snprintf(tmp, sizeof(tmp), "%lld", aer_as_int(v));  strbuf_append(sb, tmp); break;
        case TYPE_REAL:     aer_format_real(aer_as_real(v), tmp, sizeof(tmp)); strbuf_append(sb, tmp); break;
        case TYPE_BOOLEAN:  strbuf_append(sb, aer_as_bool(v) ? "true" : "false"); break;
        case TYPE_FUNCTION: strbuf_append(sb, "<function>"); break;
        case TYPE_STRING: {
            AerString* s = aer_as_string(v);
            if (in_collection) strbuf_append(sb, "\"");
            strbuf_append_n(sb, s->data, s->length);
            if (in_collection) strbuf_append(sb, "\"");
            break;
        }
        case TYPE_ARRAY: {
            AerArray* a = aer_as_array(v);
            if (a->shape) {
                Shape* shape = a->shape;
                strbuf_append(sb, aer_as_string(c->pool[shape->name])->data);
                strbuf_append(sb, "{");
                for (unsigned int i = 0; i < shape->field_count; i++) {
                    if (i > 0) strbuf_append(sb, ", ");
                    strbuf_append(sb, aer_as_string(c->pool[shape->field_names[i]])->data);
                    strbuf_append(sb, ": ");
                    vm_format_value(c, a->items[i], true, sb);
                }
                strbuf_append(sb, "}");
                break;
            }
            strbuf_append(sb, "[");
            for (unsigned int i = 0; i < a->count; i++) {
                if (i > 0) strbuf_append(sb, ", ");
                vm_format_value(c, a->items[i], true, sb);
            }
            strbuf_append(sb, "]");
            break;
        }
        case TYPE_PACKED_ARRAY: {
            AerPackedArray* pa = aer_as_packed_array(v);
            strbuf_append(sb, aer_as_string(c->pool[pa->shape->name])->data);
            strbuf_append(sb, "[");
            snprintf(tmp, sizeof(tmp), "%u", pa->count);
            strbuf_append(sb, tmp);
            strbuf_append(sb, "]");
            break;
        }
        case TYPE_DICT: {
            AerDict* d = aer_as_dict(v);
            strbuf_append(sb, "{");
            bool first = true;
            for (unsigned int i = 0; i < d->map.capacity; i++) {
                HashTableEntry* e = &d->map.buckets[i];
                if (!e->key) continue;
                if (!first) strbuf_append(sb, ", ");
                first = false;
                strbuf_append(sb, "\"");
                strbuf_append(sb, e->key);
                strbuf_append(sb, "\": ");
                vm_format_value(c, e->payload, true, sb);
            }
            strbuf_append(sb, "}");
            break;
        }
        case TYPE_RESULT: {
            AerResult* r = aer_as_result(v);
            strbuf_append(sb, "Result(");
            vm_format_value(c, r->value, true, sb);
            strbuf_append(sb, ", ");
            vm_format_value(c, r->err, true, sb);
            strbuf_append(sb, ")");
            break;
        }
        case TYPE_ANY: break;   /* never a real AerVal's tag — only Shape.field_types[] uses it */
    }
}

static void vm_print_value(Chunk* c, AerVal v, bool in_collection) {
    StrBuf sb;
    strbuf_init(&sb);
    vm_format_value(c, v, in_collection, &sb);
    printf("%s", sb.buf);
    free(sb.buf);
}

/* ------------------------------------------------------------------ */

static inline __attribute__((always_inline)) bool vm_truthy(AerVal v) {
    switch (aer_type(v)) {
        case TYPE_NULL:     return false;
        case TYPE_BOOLEAN:  return aer_as_bool(v);
        case TYPE_INTEGER:  return aer_as_int(v) != 0;
        case TYPE_REAL:     return aer_as_real(v) != 0.0;
        case TYPE_STRING:   return aer_as_string(v)->length > 0;
        case TYPE_FUNCTION: return true;
        case TYPE_ARRAY:    return aer_as_array(v)->count > 0;
        case TYPE_DICT:     return aer_as_dict(v)->map.count > 0;
        case TYPE_PACKED_ARRAY: return aer_as_packed_array(v)->count > 0;
        /* "Did this succeed" — `if result { ... }` reads the same way `if err == null` does, just
           inverted, without needing to destructure first. */
        case TYPE_RESULT:   return aer_type(aer_as_result(v)->err) == TYPE_NULL;
        case TYPE_ANY:      break;   /* never a real AerVal's tag — only Shape.field_types[] uses it */
    }
    return false;
}

static inline __attribute__((always_inline)) AerVal vm_promote_real(AerVal v) {
    if (aer_type(v) == TYPE_INTEGER) v = aer_real((double)aer_as_int(v));
    return v;
}

/* ------------------------------------------------------------------ */
/* Binary operation dispatch                                            */
/* ------------------------------------------------------------------ */

/* Structural/reference equality with no error path — unlike OP_EQ, a type mismatch here just means "not this one, keep looking." Used by OP_IN's array scan. */
static bool values_equal(AerVal a, AerVal b) {
    if (aer_type(a) != aer_type(b)) return false;
    switch (aer_type(a)) {
        case TYPE_NULL:     return true;
        case TYPE_BOOLEAN:  return aer_as_bool(a) == aer_as_bool(b);
        case TYPE_INTEGER:  return aer_as_int(a) == aer_as_int(b);
        case TYPE_REAL:     return aer_as_real(a) == aer_as_real(b);
        case TYPE_STRING:   return aer_as_string(a)->length == aer_as_string(b)->length &&
                                    strncmp(aer_as_string(a)->data, aer_as_string(b)->data, aer_as_string(a)->length) == 0;
        case TYPE_FUNCTION: return aer_as_function(a)->code_offset == aer_as_function(b)->code_offset;
        case TYPE_ARRAY:    return aer_as_array(a) == aer_as_array(b);
        case TYPE_DICT:     return aer_as_dict(a) == aer_as_dict(b);
        case TYPE_PACKED_ARRAY: return aer_as_packed_array(a) == aer_as_packed_array(b);
        case TYPE_RESULT:   return aer_as_result(a) == aer_as_result(b);
        case TYPE_ANY:      break;   /* never a real AerVal's tag — only Shape.field_types[] uses it */
    }
    return false;
}

/* Int/int and real/real only — the two type-pairs common enough in arithmetic-heavy code to be
   worth an inlined fast path at a call site, and small enough (no string/array/dict/bool/null/
   AND-OR-IN handling) that always_inline-ing this at its two call sites (the OP_BINARY_FIELD/
   OP_FIELD_BINARY field-fusion opcodes below) doesn't bloat the icache the way inlining a full
   binary-op dispatch would. Everything else falls through to vm_binary_cold() (a real,
   non-inlined function, just below) — *handled is set false and the caller must call that instead.
   ta/tb are passed in rather than recomputed since every caller already computed them for its own
   dispatch. */
static inline __attribute__((always_inline)) AerVal vm_binary_fast(AerVal a, AerVal b, Opcode op, ValueType ta, ValueType tb, bool* handled) {
    *handled = true;
    if (ta == TYPE_INTEGER && tb == TYPE_INTEGER) {
        int64_t l = aer_as_int(a), rv = aer_as_int(b);
        switch (op) {
            case OP_ADD:         return aer_int(l + rv);
            case OP_SUB:         return aer_int(l - rv);
            case OP_MUL:         return aer_int(l * rv);
            case OP_DIV:
                if (rv == 0) { error("Division by zero"); return aer_int(0); }
                return aer_real((double)l / (double)rv);
            case OP_FLOOR_DIV:
                if (rv == 0) { error("Division by zero"); return aer_int(0); }
                return aer_int((int64_t)floor((double)l / (double)rv));
            case OP_MOD:
                if (rv == 0) { error("Modulo by zero"); return aer_int(0); }
                return aer_int(l % rv);
            case OP_LSHIFT:      return aer_int(l << rv);
            case OP_RSHIFT:      return aer_int(l >> rv);
            case OP_BITWISE_AND: return aer_int(l &  rv);
            case OP_BITWISE_OR:  return aer_int(l |  rv);
            case OP_BITWISE_XOR: return aer_int(l ^  rv);
            case OP_EQ:  return aer_bool(l == rv);
            case OP_NEQ: return aer_bool(l != rv);
            case OP_LT:  return aer_bool(l <  rv);
            case OP_GT:  return aer_bool(l >  rv);
            case OP_LTE: return aer_bool(l <= rv);
            case OP_GTE: return aer_bool(l >= rv);
            default: error("Operator not valid for integers"); return aer_int(0);
        }
    }

    if (ta == TYPE_REAL && tb == TYPE_REAL) {
        double l = aer_as_real(a), rv = aer_as_real(b);
        switch (op) {
            case OP_ADD: return aer_real(l + rv);
            case OP_SUB: return aer_real(l - rv);
            case OP_MUL: return aer_real(l * rv);
            case OP_DIV:
                if (rv == 0.0) { error("Division by zero"); return aer_real(0.0); }
                return aer_real(l / rv);
            case OP_FLOOR_DIV:
                if (rv == 0.0) { error("Division by zero"); return aer_real(0.0); }
                return aer_real(floor(l / rv));
            case OP_MOD: return aer_real(fmod(l, rv));
            case OP_EQ:  return aer_bool(l == rv);
            case OP_NEQ: return aer_bool(l != rv);
            case OP_LT:  return aer_bool(l <  rv);
            case OP_GT:  return aer_bool(l >  rv);
            case OP_LTE: return aer_bool(l <= rv);
            case OP_GTE: return aer_bool(l >= rv);
            default: error("Operator not valid for reals"); return aer_real(0.0);
        }
    }

    *handled = false;
    return aer_bool(false);   /* unused by the caller when *handled is false */
}

/* Cold path for the per-operator OP_ADD/OP_SUB/.../OP_GTE labels in vm_run() (see PACK_BINARY's
   own comment, vm.h) and for the OP_BINARY_FIELD/OP_FIELD_BINARY field-fusion opcodes —
   everything vm_binary_fast() (just above) doesn't handle: IN, null, promoted-real, boolean,
   string, array, dict, and the final type-mismatch error. Deliberately a real, non-inlined
   function rather than always_inline like vm_binary_fast(): this path only runs for the rare
   case, and NOT inlining it avoids duplicating this whole body across every call site. ta/tb are
   passed in rather than recomputed since every caller already computed them for its own
   fast-path check. */
static AerVal vm_binary_cold(AerVal a, AerVal b, Opcode op, ValueType ta, ValueType tb) {
    /* Checked before null-handling below so `null in arr` isn't intercepted by the "null op anything-else errors" rule, which is about direct comparison, not container search. */
    if (op == OP_IN) {
        if (aer_type(b) == TYPE_DICT) {
            if (aer_type(a) != TYPE_STRING) { error("Left side of 'in' must be a string when testing dict membership"); return aer_bool(false); }
            AerString* as = aer_as_string(a);
            unsigned int klen = as->length;
            if (klen > VM_KEY_MAX) { error("Dict key too long (max %d bytes)", VM_KEY_MAX); return aer_bool(false); }
            char kbuf[VM_KEY_MAX + 1];
            memcpy(kbuf, as->data, klen);
            kbuf[klen] = '\0';
            return aer_bool(hashtable_get(&aer_as_dict(b)->map, kbuf) != NULL);
        }
        if (aer_type(b) == TYPE_ARRAY) {
            AerArray* arr = aer_as_array(b);
            for (unsigned int i = 0; i < arr->count; i++) {
                if (values_equal(a, arr->items[i])) return aer_bool(true);
            }
            return aer_bool(false);
        }
        if (aer_type(b) == TYPE_STRING) {
            /* Substring search — mirrors string.contains() (aer_string.c) exactly; kept as its
               own small loop rather than shared, since the two live in different modules with
               different (stack-based vs. binary-op) calling conventions. */
            if (aer_type(a) != TYPE_STRING) { error("Left side of 'in' must be a string when testing string membership"); return aer_bool(false); }
            AerString* needle = aer_as_string(a);
            AerString* hay    = aer_as_string(b);
            bool found = needle->length == 0;
            for (unsigned int i = 0; !found && i + needle->length <= hay->length; i++)
                if (memcmp(hay->data + i, needle->data, needle->length) == 0) found = true;
            return aer_bool(found);
        }
        error("Right side of 'in' must be a dict, array, or string");
        return aer_bool(false);
    }

    /* null equality: null == null is true; null op anything-else errors */
    if (ta == TYPE_NULL || tb == TYPE_NULL) {
        if (op == OP_EQ)  return aer_bool(ta == TYPE_NULL && tb == TYPE_NULL);
        if (op == OP_NEQ) return aer_bool(!(ta == TYPE_NULL && tb == TYPE_NULL));
        error("Operator not valid for null"); return aer_bool(false);
    }

    if (ta == TYPE_REAL || tb == TYPE_REAL) {
        a = vm_promote_real(a);
        b = vm_promote_real(b);
    }

    if (aer_type(a) == TYPE_REAL && aer_type(b) == TYPE_REAL) {
        double l = aer_as_real(a), rv = aer_as_real(b);
        switch (op) {
            case OP_ADD: return aer_real(l + rv);
            case OP_SUB: return aer_real(l - rv);
            case OP_MUL: return aer_real(l * rv);
            case OP_DIV:
                if (rv == 0.0) { error("Division by zero"); return aer_real(0.0); }
                return aer_real(l / rv);
            case OP_FLOOR_DIV:
                if (rv == 0.0) { error("Division by zero"); return aer_real(0.0); }
                return aer_real(floor(l / rv));
            case OP_MOD: return aer_real(fmod(l, rv));
            case OP_EQ:  return aer_bool(l == rv);
            case OP_NEQ: return aer_bool(l != rv);
            case OP_LT:  return aer_bool(l <  rv);
            case OP_GT:  return aer_bool(l >  rv);
            case OP_LTE: return aer_bool(l <= rv);
            case OP_GTE: return aer_bool(l >= rv);
            default: error("Operator not valid for reals"); return aer_real(0.0);
        }
    }

    if (aer_type(a) == TYPE_BOOLEAN && aer_type(b) == TYPE_BOOLEAN) {
        if (op == OP_EQ)  return aer_bool(aer_as_bool(a) == aer_as_bool(b));
        if (op == OP_NEQ) return aer_bool(aer_as_bool(a) != aer_as_bool(b));
        error("Operator not valid for booleans"); return aer_bool(false);
    }

    if (aer_type(a) == TYPE_STRING && aer_type(b) == TYPE_STRING) {
        AerString* as = aer_as_string(a);
        AerString* bs = aer_as_string(b);
        bool eq = as->length == bs->length &&
                  strncmp(as->data, bs->data, as->length) == 0;
        if (op == OP_EQ)  return aer_bool(eq);
        if (op == OP_NEQ) return aer_bool(!eq);
        if (op == OP_ADD) {
            unsigned int len = as->length + bs->length;
            char* buf = xmalloc(len + 1);
            memcpy(buf, as->data, as->length);
            memcpy(buf + as->length, bs->data, bs->length);
            buf[len] = '\0';
            /* aer_make_string takes ownership of buf directly; no pool interning needed since this string is used once, right here (see vm_to_str's comment). */
            return aer_make_string(buf, len);
        }
        if (op == OP_LT || op == OP_GT || op == OP_LTE || op == OP_GTE) {
            /* Same total order math.sort() already assumes and implements for strings
               (aer_math.c's sort_cmp) — exposed here as the ordinary comparison operators
               instead of only being reachable indirectly through sort(). */
            unsigned int n = as->length < bs->length ? as->length : bs->length;
            int cmp = n > 0 ? memcmp(as->data, bs->data, n) : 0;
            if (cmp == 0) cmp = (int)as->length - (int)bs->length;
            switch (op) {
                case OP_LT:  return aer_bool(cmp < 0);
                case OP_GT:  return aer_bool(cmp > 0);
                case OP_LTE: return aer_bool(cmp <= 0);
                default:     return aer_bool(cmp >= 0);   /* OP_GTE */
            }
        }
        error("Operator not valid for strings"); return aer_bool(false);
    }

    if (aer_type(a) == TYPE_ARRAY && aer_type(b) == TYPE_ARRAY) {
        if (op == OP_EQ)  return aer_bool(aer_as_array(a) == aer_as_array(b));
        if (op == OP_NEQ) return aer_bool(aer_as_array(a) != aer_as_array(b));
        error("Operator not valid for arrays"); return aer_bool(false);
    }

    if (aer_type(a) == TYPE_DICT && aer_type(b) == TYPE_DICT) {
        if (op == OP_EQ)  return aer_bool(aer_as_dict(a) == aer_as_dict(b));
        if (op == OP_NEQ) return aer_bool(aer_as_dict(a) != aer_as_dict(b));
        error("Operator not valid for dicts"); return aer_bool(false);
    }

    if (aer_type(a) == TYPE_RESULT && aer_type(b) == TYPE_RESULT) {
        if (op == OP_EQ)  return aer_bool(aer_as_result(a) == aer_as_result(b));
        if (op == OP_NEQ) return aer_bool(aer_as_result(a) != aer_as_result(b));
        error("Operator not valid for Results"); return aer_bool(false);
    }

    error("Type mismatch in binary expression");
    return aer_bool(false);
}

static AerVal vm_to_str(VM* vm, AerVal v) {
    if (aer_type(v) == TYPE_STRING) return v;

    char*        owned;
    unsigned int len;

    if (aer_type(v) == TYPE_ARRAY || aer_type(v) == TYPE_DICT || aer_type(v) == TYPE_PACKED_ARRAY || aer_type(v) == TYPE_RESULT) {
        /* Unbounded recursive content doesn't fit the fixed buffer below, so reuse print()'s formatter; sb.buf is already a fresh allocation, handed to aer_make_string as-is. */
        StrBuf sb;
        strbuf_init(&sb);
        vm_format_value(vm->chunk, v, false, &sb);
        owned = sb.buf;
        len   = (unsigned int)sb.len;
    } else {
        /* Copies into a fresh owned buffer since AerString always owns its data, and buf is a stack array that can't be handed to aer_make_string directly. */
        char buf[64];
        switch (aer_type(v)) {
            case TYPE_NULL:     snprintf(buf, sizeof(buf), "null");                              break;
            case TYPE_INTEGER:  snprintf(buf, sizeof(buf), "%lld", aer_as_int(v));               break;
            case TYPE_REAL:     aer_format_real(aer_as_real(v), buf, sizeof(buf));                 break;
            case TYPE_BOOLEAN:  snprintf(buf, sizeof(buf), "%s",   aer_as_bool(v) ? "true" : "false"); break;
            case TYPE_FUNCTION: snprintf(buf, sizeof(buf), "<function>");                        break;
            case TYPE_ARRAY: case TYPE_DICT: case TYPE_STRING: case TYPE_PACKED_ARRAY: case TYPE_RESULT: break;   /* handled above */
            case TYPE_ANY: break;   /* never a real AerVal's tag — only Shape.field_types[] uses it */
        }
        len   = (unsigned int)strlen(buf);
        owned = xmalloc(len + 1);
        memcpy(owned, buf, len + 1);
    }
    /* No chunk_add_pool interning: this string is used once and never looked up by pool index again. Interning would grow the pool/name_index forever per unique value — measured 7x slower for 100k unique casts vs. 10 distinct ones. */
    return aer_make_string(owned, len);
}

/* Resolves a[start:end] bounds against length `len`; either bound may be TYPE_NULL (defaults to 0/len). Clamps out-of-range bounds instead of erroring, Python-slice style. */
static bool vm_slice_bounds(AerVal start_v, AerVal end_v, int64_t len,
                             int64_t* out_start, int64_t* out_end) {
    if (aer_type(start_v) != TYPE_NULL && aer_type(start_v) != TYPE_INTEGER) { error("Slice bounds must be integers"); return false; }
    if (aer_type(end_v)   != TYPE_NULL && aer_type(end_v)   != TYPE_INTEGER) { error("Slice bounds must be integers"); return false; }
    int64_t start = (aer_type(start_v) == TYPE_NULL) ? 0   : aer_as_int(start_v);
    int64_t end   = (aer_type(end_v)   == TYPE_NULL) ? len : aer_as_int(end_v);
    if (start < 0) start += len;
    if (end   < 0) end   += len;
    if (start < 0)   start = 0;
    if (end   > len) end   = len;
    if (end   < start) end = start;
    *out_start = start;
    *out_end   = end;
    return true;
}

/* A baked default value — a struct field's (Shape.field_defaults) or a function parameter's
   (AerFunction.defaults) — needs one of two treatments when it's actually applied: a primitive
   (int/real/bool/null/string) is safe to copy as-is — AerVal for those is either inline or (for a
   string) an immutable shared buffer. An array or dict is NOT safe to copy as-is: every struct
   instance, or every call that omits that argument, would then alias the exact same AerArray/
   AerDict the default is stored as, so append()ing to one's default field/argument would silently
   show up on every other one ever constructed/called (and on the stored default itself) —
   Python's mutable-default-argument bug, reproduced. parser.c's default-value grammar only ever
   bakes an EMPTY array/dict literal ('[]' / '{}') as such a default (see parse_literal_default),
   so a fresh empty one is always the correct replacement here — never a deep copy of arbitrary
   contents, since there aren't any. */
static AerVal vm_default_value(AerVal dflt) {
    if (aer_type(dflt) == TYPE_ARRAY && !aer_as_array(dflt)->shape) {
        AerArray* a = pool_alloc(&array_pool);
        a->count = a->capacity = 0;
        a->items = NULL;
        a->shape = NULL;
        return aer_array_val(a);
    }
    if (aer_type(dflt) == TYPE_DICT) {
        AerDict* d = pool_alloc(&dict_pool);
        memset(&d->map, 0, sizeof(d->map));
        return aer_dict_val(d);
    }
    return dflt;
}

/* Used only by aer_module_call for a cross-module call into a v3-compiled file's exported
   function (see ChunkFunction's own comment, vm.h) — mirrors lbl_call_value's frame-push
   exactly (arity check, default-filling), just as a standalone function since
   aer_module_call isn't inside vm_run's computed-goto dispatch loop. dest_reg is fixed at 0: this
   always sets up the SECOND frame (index target->call_depth + 1) above target's own top-level
   (frame 0), so once vm_run(target) drains back to depth 0 on return_ip's OP_HALT, the result is
   sitting in target->call_stack[0].registers[0] — a fixed, known slot the caller
   (aer_module_call) can read without needing any of target's own variables' registers to stay
   predictable. */
bool setup_call(VM* target, ChunkFunction* fn, int arg_count,
                    AerVal* args, unsigned int return_ip) {
    if (arg_count < (int)fn->min_arity || arg_count > (int)fn->arity) {
        if (fn->min_arity == fn->arity)
            error("Function expects %u arguments, got %d", fn->arity, arg_count);
        else
            error("Function expects between %u and %u arguments, got %d", fn->min_arity, fn->arity, arg_count);
        return false;
    }
    if (target->call_depth + 1 >= VM_CALL_MAX) { error("v3 call stack overflow"); return false; }
    CallFrame* caller = &target->call_stack[target->call_depth];
    CallFrame* callee = &target->call_stack[target->call_depth + 1];
    callee->registers  = caller->registers + caller->frame_size;
    callee->frame_size  = fn->max_registers;
    for (int i = 0; i < arg_count; i++)
        callee->registers[i] = args[i];
    for (int i = arg_count; i < (int)fn->arity; i++)
        callee->registers[i] = vm_default_value(fn->defaults[i - fn->min_arity]);
    callee->return_ip   = return_ip;
    callee->dest_reg    = 0;
    target->call_depth++;   /* same rooting rule as vm_call_value's non-tail branch (above) — the defaults loop wrote into callee->registers[] before this point */
    gc_maybe_collect(target);
    target->registers = target->call_stack[target->call_depth].registers;
    target->raw_ints  = target->call_stack[target->call_depth].raw_ints;
    target->raw_reals = target->call_stack[target->call_depth].raw_reals;
    target->ip = fn->code_offset;
    return true;
}

/* Used by lbl_call_value (vm_run, below), factored out into a plain function since DISPATCH()'s
   computed-goto only needs to run in the caller, after this returns (a plain C function can't
   itself jump to a vm_run-local label, but it doesn't need to: it just does the work and lets
   the caller DISPATCH() once it's back). `dest_reg`/`arg_reg_base`/`arg_count` are the caller's
   own operands; `is_tail_call` is whether the calling label's own opcode was its
   OP_TAIL_CALL_* counterpart. `return_ip` is the caller's own local `ip` (the resume address,
   already past this instruction's operands) — passed explicitly rather than read from vm->ip so
   this function has no dependency on that field being kept in sync for anything but error
   reporting. */
static void vm_call_value(VM* vm, AerVal fv, int dest_reg, int arg_reg_base, int arg_count,
                              bool is_tail_call, unsigned int return_ip) {
    if (aer_type(fv) != TYPE_FUNCTION) { error("Value is not callable"); return; }
    AerFunction* f = aer_as_function(fv);
    if (arg_count < (int)f->min_arity || arg_count > (int)f->arity) {
        if (f->min_arity == f->arity)
            error("Function expects %u arguments, got %d", (unsigned int)f->arity, arg_count);
        else
            error("Function expects between %u and %u arguments, got %d", (unsigned int)f->min_arity, (unsigned int)f->arity, arg_count);
        return;
    }
    /* Tail-call reuse, see OP_TAIL_CALL's own comment in vm.h. `f` is already a plain pointer by
       this point, so overwriting its source register during the copy below — entirely possible
       when the callee names a LOCAL variable's own permanent register, not a temp, e.g.
       `f = some_fn; return f(x)` — can't invalidate anything already read out of it. The two
       loops below are the same always-safe-forward-shift copy lbl_call's own comment explains,
       done in this order (args, then defaults) specifically so every SOURCE register the arg-copy
       loop reads is read before the defaults-fill loop can possibly overwrite it. */
    if (is_tail_call) {
        for (int i = 0; i < arg_count; i++)
            vm->registers[i] = vm->registers[arg_reg_base + i];
        for (int i = arg_count; i < (int)f->arity; i++)
            vm->registers[i] = vm_default_value(f->defaults[i - f->min_arity]);
        gc_maybe_collect(vm);   /* defaults just written into the CURRENT frame (tail call, call_depth unchanged) — already rooted */
        vm->ip = f->code_offset;
        return;
    }
    if (vm->call_depth + 1 >= VM_CALL_MAX) { error("v3 call stack overflow"); return; }
    CallFrame* caller = &vm->call_stack[vm->call_depth];
    CallFrame* callee = &vm->call_stack[vm->call_depth + 1];
    callee->registers  = caller->registers + caller->frame_size;
    callee->frame_size  = f->max_registers;
    for (int i = 0; i < arg_count; i++)
        callee->registers[i] = caller->registers[arg_reg_base + i];
    for (int i = arg_count; i < (int)f->arity; i++)
        callee->registers[i] = vm_default_value(f->defaults[i - f->min_arity]);
    callee->return_ip   = return_ip;
    callee->dest_reg    = dest_reg;
    vm->call_depth++;   /* the defaults loop above wrote into callee->registers[] BEFORE this point, when mark_vm_roots's 0..call_depth scan didn't yet cover that frame — gc_maybe_collect() must run AFTER this increment, not before, or a collection could reclaim a fresh default array/dict as unreachable */
    gc_maybe_collect(vm);
    vm->registers = vm->call_stack[vm->call_depth].registers;
    vm->raw_ints  = vm->call_stack[vm->call_depth].raw_ints;
    vm->raw_reals = vm->call_stack[vm->call_depth].raw_reals;
    vm->ip = f->code_offset;
}

/* Resolves field_idx to a slot within shape, checking the per-callsite inline cache
   (field_cache, keyed by `site` — this instruction's own bytecode offset) first. The shape-only
   half of vm_resolve_field's lookup, split out so a caller that already has a Shape* without
   going through a register/AerArray (OP_INDEX_FIELD_GET/SET's packed branch, keyed off
   AerPackedArray.shape directly) can share the same cache — safe to share: the cache only ever
   stores (shape, slot) pairs, and a given field name's slot within a given Shape is identical
   whether that shape backs a boxed struct instance or a packed array. Returns false (error
   already reported) if shape has no such field. */
static inline __attribute__((always_inline)) bool vm_resolve_field_by_shape(Chunk* c, unsigned int site, Shape* shape, int field_idx, int* out_slot) {
    FieldCacheEntry* entry = &c->field_cache[site];
    if (entry->shape == shape) {
        *out_slot = entry->slot;
        return true;
    }
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == (unsigned int)field_idx) {
            entry->shape = shape;
            entry->slot  = (int)i;
            *out_slot = (int)i;
            return true;
        }
    }
    error("'%s' has no field '%s'", aer_as_string(c->pool[shape->name])->data,
          aer_as_string(c->pool[field_idx])->data);
    return false;
}

/* Resolves struct_reg's field (by field_idx) to an (AerArray*, item slot) pair — the register/
   struct-instance-validating half, delegating the shape+cache lookup to vm_resolve_field_by_shape
   above. Shared by every field-access opcode: lbl_field_get, the fused lbl_binary_field/
   lbl_field_binary (both operand orders), and lbl_field_set. Returns false (error already
   reported) if struct_reg isn't a struct instance or has no such field. */
static inline __attribute__((always_inline)) bool vm_resolve_field(VM* vm, Chunk* c, unsigned int site, int struct_reg, int field_idx,
                                 AerArray** out_oa, int* out_slot) {
    AerVal* obj = &vm->registers[struct_reg];
    if (obj->tag != TYPE_ARRAY || !((AerArray*)obj->as.ptr)->shape) {
        error("'.' field access requires a struct instance");
        return false;
    }
    AerArray* oa = (AerArray*)obj->as.ptr;
    *out_oa = oa;
    return vm_resolve_field_by_shape(c, site, oa->shape, field_idx, out_slot);
}

/* Reads an AerVal out of one 8-byte packed slot per its declared field type — shared by the
   packed branches of lbl_index_field_get/set and lbl_packed_array_new's own default-fill.
     No switch on ftype: AerVal.as is exactly 8 bytes, and for TYPE_INTEGER/TYPE_REAL the packed
   slot's raw bytes already ARE that union's bit pattern. TYPE_BOOLEAN's slot holds an 8-byte 0/1
   (see vm_packed_slot_write) whose low byte is a little-endian machine's first byte — the same
   byte .as.b reads — so one branchless memcpy is correct for all three eligible field types. A
   3-way switch here regresses packed arrays slower than plain struct arrays on ARM (weaker branch
   prediction than x86) — keep this branchless. */
static inline AerVal vm_packed_slot_read(unsigned char* slot, ValueType ftype) {
    AerVal v;
    v.tag = ftype;
    memcpy(&v.as, slot, 8);
    return v;
}

/* Inverse of vm_packed_slot_read — writes v's raw 8-byte payload into one packed slot. Caller
   must already have type-checked v against the field's declared type. */
static inline void vm_packed_slot_write(unsigned char* slot, ValueType ftype, AerVal v) {
    (void)ftype;
    memcpy(slot, &v.as, 8);
}

/* Scans a dict's bucket array forward from *idx, skipping empty buckets, and returns an owned
   copy of the first live key found as an AerVal (aer_make_string) — shared by lbl_iter_next_array's
   dict branch and lbl_iter_next_pair, both of which iterate dict keys this way. *idx is left at
   the found bucket (caller advances it by 1 after also reading the value, if needed). Returns
   false (nothing written) once *idx reaches d->map.capacity with no more live buckets. */
static bool vm_dict_next_key(AerDict* d, int64_t* idx, AerVal* out_key) {
    while ((uint64_t)*idx < d->map.capacity && !d->map.buckets[*idx].key) (*idx)++;
    if ((uint64_t)*idx >= d->map.capacity) return false;
    unsigned int key_len = d->map.buckets[*idx].length;
    char* key_buf = xmalloc(key_len + 1);
    memcpy(key_buf, d->map.buckets[*idx].key, key_len);
    key_buf[key_len] = '\0';
    *out_key = aer_make_string(key_buf, key_len);   /* no chunk_add_pool interning — see vm_to_str's comment */
    return true;
}

AerArray* vm_new_array(void) {
    return pool_alloc(&array_pool);
}

AerDict* vm_new_dict(void) {
    return pool_alloc(&dict_pool);
}

AerVal aer_make_result(AerVal value, AerVal err) {
    AerResult* r = pool_alloc(&result_pool);
    r->value = value;
    r->err   = err;
    return aer_result_val(r);
}

AerFunction* vm_new_function(void) {
    return pool_alloc(&function_pool);
}

/* builtin_id is resolved at parse time (builtin_call_id) — always one of the cases below. Returns
   true if arg_count matched, result in *out. */
static bool vm_call_builtin(Chunk* c, int builtin_id, AerVal* args, int arg_count, AerVal* out) {
    *out = aer_null();

    switch (builtin_id) {
        case CALL_BUILTIN_LENGTH: {
            if (arg_count != 1) return false;
            AerVal a = args[0];
            if      (aer_type(a) == TYPE_ARRAY)  *out = aer_int((int64_t)aer_as_array(a)->count);
            else if (aer_type(a) == TYPE_STRING) *out = aer_int((int64_t)aer_as_string(a)->length);
            else if (aer_type(a) == TYPE_DICT)   *out = aer_int((int64_t)aer_as_dict(a)->map.count);
            else if (aer_type(a) == TYPE_PACKED_ARRAY) *out = aer_int((int64_t)aer_as_packed_array(a)->count);
            else error("length() requires an array, dict, string, or packed array");
            return true;
        }
        case CALL_BUILTIN_DELETE: {
            if (arg_count != 2) return false;
            AerVal obj = args[0], key = args[1];
            if (aer_type(obj) == TYPE_DICT) {
                if (aer_type(key) != TYPE_STRING) { error("delete() key must be a string"); return true; }
                AerString* ks = aer_as_string(key);
                unsigned int klen = ks->length;
                if (klen > VM_KEY_MAX) { error("Dict key too long (max %d bytes)", VM_KEY_MAX); return true; }
                char kbuf[VM_KEY_MAX + 1];
                memcpy(kbuf, ks->data, klen);
                kbuf[klen] = '\0';
                hashtable_remove(&aer_as_dict(obj)->map, kbuf);
                *out = obj;
                return true;
            }
            if (aer_type(obj) == TYPE_ARRAY) {
                AerArray* a = aer_as_array(obj);
                if (a->shape) { error("delete() cannot remove fields from a struct instance — structs have a fixed shape"); return true; }
                if (aer_type(key) != TYPE_INTEGER) { error("Array delete() index must be an integer"); return true; }
                int64_t i = aer_as_int(key);
                if (i < 0) i += (int64_t)a->count;
                if (i < 0 || (uint64_t)i >= a->count) { error("Array index %lld out of bounds (len %u)", aer_as_int(key), a->count); return true; }
                memmove(&a->items[i], &a->items[i + 1], (size_t)(a->count - (uint64_t)i - 1) * sizeof(AerVal));
                a->count--;
                *out = obj;
                return true;
            }
            error("delete() requires a dict or array");
            return true;
        }
        case CALL_BUILTIN_APPEND: {
            if (arg_count != 2) return false;
            AerVal arr = args[0], val = args[1];
            if (aer_type(arr) != TYPE_ARRAY) { error("append() requires an array"); return true; }
            AerArray* a = aer_as_array(arr);
            if (a->shape) { error("append() cannot add fields to a struct instance — structs have a fixed shape"); return true; }
            if (a->count >= a->capacity) {
                a->capacity = a->capacity ? a->capacity * 2 : 4;
                a->items = xrealloc(a->items, sizeof(AerVal) * a->capacity);
            }
            gc_barrier_array(a, val);
            a->items[a->count++] = val;
            *out = arr;
            return true;
        }
        case CALL_BUILTIN_PRINT: {
            if (arg_count != 1) return false;
            vm_print_value(c, args[0], false);
            printf("\n");
            return true;
        }
        case CALL_BUILTIN_TYPE: {
            if (arg_count != 1) return false;
            const char* tn = vm_type_name(c, args[0]);
            /* Copies rather than pointing at a static literal or the chunk's pool data — AerString always owns its data, no exceptions. */
            unsigned int tn_len = (unsigned int)strlen(tn);
            char* tn_buf = xmalloc(tn_len + 1);
            memcpy(tn_buf, tn, tn_len + 1);
            *out = aer_make_string(tn_buf, tn_len);   /* no chunk_add_pool interning — see vm_to_str's comment */
            return true;
        }
        case CALL_BUILTIN_ASSERT: {
            if (arg_count != 2) return false;
            AerVal cond = args[0], msg = args[1];
            if (aer_type(msg) != TYPE_STRING) { error("assert() requires a string message as its second argument"); return true; }
            if (!vm_truthy(cond)) {
                assert_failure_count++;
                AerString* ms = aer_as_string(msg);
                printf("ASSERT FAILED: %.*s\n", (int)ms->length, ms->data);
            }
            return true;
        }
        case CALL_BUILTIN_PANIC: {
            if (arg_count != 1) return false;
            AerVal msg = args[0];
            if (aer_type(msg) != TYPE_STRING) { error("panic() requires a string message"); return true; }
            AerString* ms = aer_as_string(msg);
            error("panic: %.*s", (int)ms->length, ms->data);
            return true;
        }
        case CALL_BUILTIN_RESULT: {
            if (arg_count != 2) return false;
            bool value_is_null = aer_type(args[0]) == TYPE_NULL;
            bool err_is_null   = aer_type(args[1]) == TYPE_NULL;
            if (value_is_null == err_is_null) {
                error("Result() requires exactly one of its two arguments to be null (the value on success, the err on failure)");
                return true;
            }
            *out = aer_make_result(args[0], args[1]);
            return true;
        }
    }
    return false;
}

/* Shared by lbl_index_get and the OP_INDEX_GET_*_* fused handlers (see
   Part 2 of the array-index-get fusion comment in vm.h) — identical
   type dispatch, bounds/negative-index handling, and error messages as
   the original inline body, just returning the value instead of
   PUSH()ing it so callers can fuse it into a stack-neutral read. On any
   error path this calls error() (setting runtime_had_error) and returns
   aer_null(); every caller must PUSH/DISPATCH exactly as before — the
   returned null is never actually used once DISPATCH() bails. */
/* Writes through `out` instead of returning AerVal by value — its one call site (lbl_index_get,
   below) assigns straight into vm->registers[dest_reg], and returning by value across this many
   branches meant the compiler couldn't avoid materializing the result on the stack first (same
   16-byte ldmia/stmia round trip found and fixed in BINARY_OP_INT_REAL/INT_ONLY just above; see
   that comment for the full story). Safe even if `out` aliases obj's or idx's own register: both
   are function parameters, already copied by value, before `*out` is ever touched. */
static inline void vm_index_get_compute(AerVal obj, AerVal idx, AerVal* out) {
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) { error("Struct fields are accessed with '.', not '[]'"); *out = aer_null(); return; }
        if (aer_type(idx) != TYPE_INTEGER) { error("Array index must be an integer"); *out = aer_null(); return; }
        int64_t i = aer_as_int(idx);
        if (i < 0) i += (int64_t)a->count;
        if (i < 0 || (uint64_t)i >= a->count) { error("Array index %lld out of bounds (len %u)", aer_as_int(idx), a->count); *out = aer_null(); return; }
        *out = a->items[i]; return;
    } else if (aer_type(obj) == TYPE_DICT) {
        if (aer_type(idx) != TYPE_STRING) { error("Dict key must be a string"); *out = aer_null(); return; }
        AerString* is = aer_as_string(idx);
        unsigned int klen = is->length;
        if (klen > VM_KEY_MAX) { error("Dict key too long (max %d bytes)", VM_KEY_MAX); *out = aer_null(); return; }
        char kbuf[VM_KEY_MAX + 1];
        memcpy(kbuf, is->data, klen);
        kbuf[klen] = '\0';
        AerVal* found = hashtable_get(&aer_as_dict(obj)->map, kbuf);
        if (!found) { *out = aer_null(); return; }
        *out = *found; return;
    } else if (aer_type(obj) == TYPE_STRING) {
        AerString* os = aer_as_string(obj);
        if (aer_type(idx) != TYPE_INTEGER) { error("String index must be an integer"); *out = aer_null(); return; }
        int64_t i = aer_as_int(idx);
        int64_t len = (int64_t)os->length;
        if (i < 0) i += len;
        if (i < 0 || i >= len) { error("String index %lld out of bounds (len %lld)", aer_as_int(idx), len); *out = aer_null(); return; }
        /* A single character is a length-1 string (AER has no char type); copies the byte since AerString must always own its data, even after obj is later collected. */
        char* ch_buf = xmalloc(2);
        ch_buf[0] = os->data[i];
        ch_buf[1] = '\0';
        *out = aer_make_string(ch_buf, 1); return;   /* no chunk_add_pool interning — see vm_to_str's comment */
    } else if (aer_type(obj) == TYPE_RESULT) {
        /* `result[0]` is the value, `result[1]` is the err — the same (value, err) order every
           stdlib fallible function returns, so destructuring (`a, b = io.read(path)`, which
           desugars to exactly this indexing) reads them out in the expected order. */
        if (aer_type(idx) != TYPE_INTEGER) { error("Result index must be an integer"); *out = aer_null(); return; }
        int64_t i = aer_as_int(idx);
        AerResult* r = aer_as_result(obj);
        if (i == 0) { *out = r->value; return; }
        if (i == 1) { *out = r->err;   return; }
        error("Result index %lld out of bounds (a Result only has indices 0 and 1)", aer_as_int(idx));
        *out = aer_null();
    } else {
        error("Cannot index type");
        *out = aer_null();
    }
}

/* Shared by lbl_index_set and the OP_INDEX_SET_*_* fused handlers (see the array-index-set
   fusion comment in vm.h) — identical type dispatch, bounds/negative-index handling, and error
   messages as the original inline body. Every caller must DISPATCH() immediately after (same
   discipline as vm_index_get_compute): on an error path this calls error() and simply returns,
   leaving runtime_had_error set for DISPATCH() to catch. */
static inline void vm_index_set_compute(AerVal obj, AerVal idx, AerVal val) {
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) { error("Struct fields are assigned with '.', not '[]'"); return; }
        if (aer_type(idx) != TYPE_INTEGER) { error("Array index must be an integer"); return; }
        int64_t i = aer_as_int(idx);
        if (i < 0) i += (int64_t)a->count;
        if (i < 0 || (uint64_t)i >= a->count) { error("Array index %lld out of bounds (len %u)", aer_as_int(idx), a->count); return; }
        gc_barrier_array(a, val);
        a->items[i] = val;
    } else if (aer_type(obj) == TYPE_DICT) {
        if (aer_type(idx) != TYPE_STRING) { error("Dict key must be a string"); return; }
        AerString* is = aer_as_string(idx);
        unsigned int klen = is->length;
        if (klen > VM_KEY_MAX) { error("Dict key too long (max %d bytes)", VM_KEY_MAX); return; }
        char kbuf[VM_KEY_MAX + 1];
        memcpy(kbuf, is->data, klen);
        kbuf[klen] = '\0';
        gc_barrier_dict(aer_as_dict(obj), val);
        AerVal* existing = hashtable_get(&aer_as_dict(obj)->map, kbuf);
        if (existing) {
            *existing = val;   /* update in place — no allocation */
        } else {
            char* k = hashtable_key_dup(is->data, klen, NULL);
            hashtable_put(&aer_as_dict(obj)->map, k, val);
        }
    } else if (aer_type(obj) == TYPE_STRING) {
        error("Strings are immutable — cannot assign to an index");
    } else {
        error("Cannot index type");
    }
}

/* `x as integer/float/boolean/string` conversion rules, shared by OP_CAST's handler below. */
static AerVal vm_cast(AerVal v, int cast_type) {
    AerVal r = aer_null();
    /* atoll()/atof() only consume a leading sign/digits(/./exponent), so truncating to a fixed buffer (instead of a length-sized VLA) can't change the parsed value for a real number. */
    char buf[64];
    switch (cast_type) {
        case CAST_INTEGER:
            switch (aer_type(v)) {
                case TYPE_INTEGER: r = v; break;
                case TYPE_REAL:    r = aer_int((int64_t)aer_as_real(v)); break;
                case TYPE_BOOLEAN: r = aer_int(aer_as_bool(v) ? 1 : 0); break;
                case TYPE_STRING: {
                    AerString* vs = aer_as_string(v);
                    unsigned int n = vs->length < sizeof(buf) - 1
                                          ? vs->length : sizeof(buf) - 1;
                    memcpy(buf, vs->data, n);
                    buf[n] = '\0';
                    /* strtoll, not atoll — atoll returns 0 for a non-numeric string with no way
                       to tell "parsed as zero" apart from "wasn't a number at all"; checking end
                       against the buffer's own end (after skipping the same leading whitespace/
                       sign atoll would) catches that case as a real error instead of fabricating
                       a plausible-looking wrong number. */
                    char* end;
                    long long parsed = strtoll(buf, &end, 10);
                    while (*end == ' ' || *end == '\t') end++;   /* tolerate trailing whitespace, same as leading */
                    if (end == buf || *end != '\0') {
                        error("'%.*s' as integer: not a valid integer", (int)n, buf);
                        r = aer_int(0);
                        break;
                    }
                    r = aer_int(parsed); break;
                }
                default: error("Cannot convert this type to integer"); r = aer_int(0);
            }
            break;
        case CAST_FLOAT:
            switch (aer_type(v)) {
                case TYPE_REAL:    r = v; break;
                case TYPE_INTEGER: r = aer_real((double)aer_as_int(v)); break;
                case TYPE_BOOLEAN: r = aer_real(aer_as_bool(v) ? 1.0 : 0.0); break;
                case TYPE_STRING: {
                    AerString* vs = aer_as_string(v);
                    unsigned int n = vs->length < sizeof(buf) - 1
                                          ? vs->length : sizeof(buf) - 1;
                    memcpy(buf, vs->data, n);
                    buf[n] = '\0';
                    /* strtod, not atof — same "distinguish a real 0 from not-a-number-at-all"
                       reasoning as CAST_INTEGER above. */
                    char* end;
                    double parsed = strtod(buf, &end);
                    while (*end == ' ' || *end == '\t') end++;
                    if (end == buf || *end != '\0') {
                        error("'%.*s' as float: not a valid number", (int)n, buf);
                        r = aer_real(0.0);
                        break;
                    }
                    r = aer_real(parsed); break;
                }
                default: error("Cannot convert this type to float"); r = aer_real(0.0);
            }
            break;
        case CAST_BOOLEAN:
            r = aer_bool(vm_truthy(v));
            break;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Dispatch loop — computed goto (GCC direct-threaded dispatch); each instruction jumps straight to the next handler, letting the branch predictor learn per-instruction patterns. */
/* ------------------------------------------------------------------ */

#ifdef AER_DEBUG_TOOLS
/* Grows Chunk.debug_hits to cover every word currently in c->code, zero-filling the new region —
   called once at the top of vm_run so DISPATCH() can index it unconditionally. Safe to call every
   run() (REPL appends code across calls): a no-op once debug_hits_cap already covers c->count. */
static void chunk_ensure_debug_hits(Chunk* c) {
    if (c->count <= c->debug_hits_cap) return;
    unsigned int old_cap = c->debug_hits_cap;
    c->debug_hits_cap = c->count;
    c->debug_hits = xrealloc(c->debug_hits, sizeof(uint64_t) * c->debug_hits_cap);
    memset(c->debug_hits + old_cap, 0, sizeof(uint64_t) * (c->debug_hits_cap - old_cap));
}
#endif

/* Grows Chunk.field_cache to cover every word currently in c->code, zero-filling the new region
   (NULL shape = not cached) — same growth idiom as chunk_ensure_debug_hits above, but
   unconditional: this is a real always-on perf feature, not a debug tool. Called once at the top
   of vm_run; a no-op once field_cache_cap already covers c->count (REPL appends code across
   vm_run calls, same as debug_hits). */
static void chunk_ensure_field_cache(Chunk* c) {
    if (c->count <= c->field_cache_cap) return;
    unsigned int old_cap = c->field_cache_cap;
    c->field_cache_cap = c->count;
    c->field_cache = xrealloc(c->field_cache, sizeof(FieldCacheEntry) * c->field_cache_cap);
    memset(c->field_cache + old_cap, 0, sizeof(FieldCacheEntry) * (c->field_cache_cap - old_cap));
}

bool vm_run(VM* vm) {
    Chunk* c = vm->chunk;
    /* Hoisted once here instead of re-deriving c->pool inside vm_rk_ptr20 on every call — c->pool
       is only ever mutated by chunk_add_pool, which is parse-time only (parser.c), never called
       during vm_run's own execution, so this is stable for the whole call. */
    AerVal* const_pool = c->pool;
    /* Installs this call's own catch point for error()/error_at() to longjmp back to, saving
       whatever was previously active so a nested vm_run() (cross-module calls, via setup_call/
       aer_module_call) catches its own errors and unwinds no further than here — restored before
       every return below, so the caller's own catch point (if any) is exactly as it was. Nothing
       about aer_module_load/aer_module_call's own post-call `if (runtime_had_error)` cascade
       logic needed to change: runtime_had_error is still set by error() exactly as before, still
       read by those call sites exactly where they already read it; only the per-instruction
       DISPATCH() flag check is gone, replaced by jumping directly here the moment an error fires. */
    AerJmpBuf  catch_point;
    AerJmpBuf* saved_unwind_target = runtime_error_unwind_target;
    runtime_error_unwind_target    = &catch_point;
    /* Was set on every single DISPATCH() before — this call's own VM never changes for the
       whole run (a nested module/stdlib call either runs on a different VM's own vm_run(),
       which does this same save/restore, or never touches active_vm_for_errors at all), so one
       store here plus one restore at each of this call's two exits (below, and lbl_halt) is
       exactly equivalent, at a fraction of the cost. */
    VM* saved_active_vm  = active_vm_for_errors;
    active_vm_for_errors = vm;
    if (AER_SETJMP(catch_point) != 0) {
        runtime_error_unwind_target = saved_unwind_target;
        active_vm_for_errors        = saved_active_vm;
        return false;
    }
    Opcode cur_op;
    /* The just-fetched instruction word, opcode and all — outer-scope for the same reason cur_op
       is: it must still be readable inside the handler body the goto jumps to, past the end of
       DISPATCH()'s own do-while block. For a plain (unpacked) instruction this is just cur_op's
       own value with zero upper bits, unused by that handler. For a packed OP_* instruction (see
       PACK3's comment in vm.h) the upper bits hold that instruction's narrow operands, read via
       UNPACK_A/B/C — every opcode value is < 256, so masking this word with & 0xFF to get cur_op
       is a complete no-op for every unpacked instruction and correctly extracts the opcode from a
       packed one too; one shared DISPATCH() handles both without needing to know in advance which
       kind of instruction it's about to fetch — which matters because a handful of opcodes
       (OP_JUMP, OP_DEFINE_STRUCT, OP_HALT) are reached from both packed and unpacked contexts
       through the exact same handler code. Kept the full 64 bits wide (not truncated to 32) so
       OP_BINARY's packed RK operands (bits 24-63 — see PACK_BINARY, vm.h) survive; every other
       opcode's packed fields still live in the low 32 bits exactly as before. */
    uint64_t op_word;
    /* Hoisted out of vm->ip, which every jump/call/return/READ() would otherwise reload from and
       store back to on every single touch — cheap individually since vm is already register-
       resident, but READ() alone does it on every dispatch, and some opcodes (OP_DEFINE_STRUCT's
       field loop, trailing jump-target words) call READ() several times per dispatch, each paying
       a fresh load+store. Kept in sync with vm->ip at exactly two kinds of point: the end of every
       DISPATCH() (so active_vm_for_errors->ip stays correct for any error() call the next opcode's
       body makes, identical to today's behavior) and immediately around the vm_call_value() call
       below (the only place outside this function that writes the SAME vm's ip — module/stdlib
       calls operate on a different VM or never touch ip at all, confirmed by inspection, so they
       need no such sync; vm_call_value's own return-address computation takes `ip` as an explicit
       parameter now, not a read of vm->ip, so this reload is purely to pick up the new entry point
       vm_call_value wrote on a successful call). Every other jump/call/return site in this
       function only ever reads/writes ip using data already local to it, so using the hoisted
       local instead of vm->ip directly is a pure substitution there — no additional sync needed. */
    unsigned int ip = vm->ip;
#ifdef AER_DEBUG_TOOLS
    chunk_ensure_debug_hits(c);
#endif
    chunk_ensure_field_cache(c);

#define READ()     (c->code[ip++])
#define PUSH(v)    do { if (vm->stack_top >= VM_STACK_MAX) { error("Stack overflow"); return false; } vm->stack[vm->stack_top++] = (v); } while(0)
#define POP()      (vm->stack_top > 0 ? vm->stack[--vm->stack_top] : (error("Stack underflow"), aer_null()))
#ifdef AER_DEBUG_TOOLS
/* gc_maybe_collect() is no longer called from here — see each allocating label's own call,
   placed by hand right after its result is stored into a VM-visible root (a register, or for
   OP_CALL_VALUE's non-tail path, after call_depth++ makes the new frame
   part of mark_vm_roots's 0..call_depth scan). Labels that can never reach pool_alloc (MOVE,
   JUMP, FIELD_GET/SET, OP_CALL/OP_TAIL_CALL — confirmed by direct inspection,
   not assumed) have no call at all, not a skipped one — a real Lua-style zero-cost dispatch for
   the common case, unlike the opcode_can_allocate[] gate this replaces (which still paid a
   lookup+branch on every dispatch, including the allocating ones, and measured as a net loss). */
/* No error check here anymore — error()/error_at() longjmp straight back to this call's own
   catch_point (see vm_run's preamble, above) the moment a fault fires, instead of setting a flag
   for the next DISPATCH() to notice. That was the last unconditional per-instruction cost left in
   this macro after the GC-check relocation (see gc_maybe_collect's own comment, DISPATCH()'s
   longtime neighbor) — this closes the other half of the gap explained to the user against Lua's
   longjmp-based error propagation. */
#define DISPATCH() do { unsigned int op_ip = ip; op_word = READ(); vm->ip = ip; cur_op = (Opcode)(op_word & 0x7F); c->debug_hits[op_ip]++; goto *dt[cur_op]; } while(0)
#else
/* Masked to 7 bits (0x7F), not 8 — 62 Opcode values fit with 66 to spare (room for future opcodes,
   e.g. concurrency primitives, without a second redesign). Every existing packed format (PACK3,
   PACK_REG4, ...) already starts its next field at bit 8, so bit 7 was already unused padding for
   them; narrowing the mask changes nothing for those. It exists so newly designed compact formats
   (PACK_BINARY, see vm.h) can start their own next field at bit 7 instead of being forced to
   reserve all of bit 7 for no reason. */
#define DISPATCH() do { op_word = READ(); vm->ip = ip; cur_op = (Opcode)(op_word & 0x7F); goto *dt[cur_op]; } while(0)
#endif

    static const void* const dt[] = {
        /* OP_NEGATE/OP_NOT/OP_BITWISE_NOT/OP_TO_STR have no entries here at all — they're never
           dispatched as a standalone instruction, only ever embedded as a unary_op TAG inside an
           OP_UNARY instruction's packed word (see PACK3's comment above). OP_ADD..OP_RSHIFT/OP_IN
           below, by contrast, ARE real top-level dispatch targets now — true single-level dispatch
           for binary operators (see PACK_BINARY's own comment, vm.h): each one is dispatched
           directly by DISPATCH()'s computed-goto instead of riding along as a second-level
           bin_op tag re-dispatched via a shared runtime switch (now vm_binary_cold(), see its own
           comment above). OP_AND/OP_OR/OP_PIPE still
           have no entries — still genuinely never dispatched (see their own comment, vm.h). */
        [OP_ADD]         = &&lbl_add,
        [OP_SUB]         = &&lbl_sub,
        [OP_MUL]         = &&lbl_mul,
        [OP_DIV]         = &&lbl_div,
        [OP_MOD]         = &&lbl_mod,
        [OP_FLOOR_DIV]   = &&lbl_floor_div,
        [OP_EQ]          = &&lbl_eq,
        [OP_NEQ]         = &&lbl_neq,
        [OP_LT]          = &&lbl_lt,
        [OP_GT]          = &&lbl_gt,
        [OP_LTE]         = &&lbl_lte,
        [OP_GTE]         = &&lbl_gte,
        [OP_IN]          = &&lbl_in,
        [OP_BITWISE_AND] = &&lbl_bitwise_and,
        [OP_BITWISE_OR]  = &&lbl_bitwise_or,
        [OP_BITWISE_XOR] = &&lbl_bitwise_xor,
        [OP_LSHIFT]      = &&lbl_lshift,
        [OP_RSHIFT]      = &&lbl_rshift,
        [OP_JUMP]           = &&lbl_jump,
        [OP_DEFINE_STRUCT]  = &&lbl_define_struct,
        [OP_HALT]           = &&lbl_halt,
        [OP_LOADK]       = &&lbl_loadk,
        [OP_MOVE]        = &&lbl_move,
        [OP_IS_RESULT]   = &&lbl_is_result,
        [OP_JUMP_IF_FALSE_REG] = &&lbl_jump_if_false_reg,
        [OP_CALL]              = &&lbl_call,
        [OP_CALL_VALUE]        = &&lbl_call_value,
        [OP_TAIL_CALL]         = &&lbl_call,
        [OP_TAIL_CALL_VALUE]   = &&lbl_call_value,
        [OP_CALL_MODULE]       = &&lbl_call_module,
        [OP_CALL_BUILTIN]      = &&lbl_call_builtin,
        [OP_RETURN]            = &&lbl_return,
        [OP_ARRAY_NEW]         = &&lbl_array_new,
        [OP_INDEX_GET]         = &&lbl_index_get,
        [OP_INDEX_SET]         = &&lbl_index_set,
        [OP_SLICE_GET]         = &&lbl_slice_get,
        [OP_CHECK_SHAPE]       = &&lbl_check_shape,
        [OP_DICT_NEW]          = &&lbl_dict_new,
        [OP_ITER_NEXT_ARRAY]   = &&lbl_iter_next_array,
        [OP_ITER_NEXT_PAIR]    = &&lbl_iter_next_pair,
        [OP_ITER_RANGE_PREP]      = &&lbl_iter_range_prep,
        [OP_ITER_RANGE_LOOP]      = &&lbl_iter_range_loop,
        [OP_STRUCT_NEW]        = &&lbl_struct_new,
        [OP_FIELD_GET]         = &&lbl_field_get,
        [OP_FIELD_SET]         = &&lbl_field_set,
        [OP_PACKED_ARRAY_NEW]  = &&lbl_packed_array_new,
        [OP_INDEX_FIELD_GET]   = &&lbl_index_field_get,
        [OP_INDEX_FIELD_SET]   = &&lbl_index_field_set,
        [OP_UNARY]             = &&lbl_unary,
        [OP_CAST]              = &&lbl_cast,
        [OP_BINARY_FIELD]      = &&lbl_binary_field,
        [OP_FIELD_BINARY]      = &&lbl_field_binary,
        [OP_PRINT_REPL]        = &&lbl_print_repl,

        /* "Primitive pass" raw-arithmetic family (vm.h's OP_RAW_LOAD_INT comment) — see the
           lbl_raw_* labels themselves, below lbl_halt, for why they need no vm_rk_ptr9/tag-check
           at all. */
        [OP_RAW_LOAD_INT]      = &&lbl_raw_load_int,
        [OP_RAW_LOAD_REAL]     = &&lbl_raw_load_real,
        [OP_RAW_ADD_INT]       = &&lbl_raw_add_int,
        [OP_RAW_SUB_INT]       = &&lbl_raw_sub_int,
        [OP_RAW_MUL_INT]       = &&lbl_raw_mul_int,
        [OP_RAW_DIV_INT]       = &&lbl_raw_div_int,
        [OP_RAW_MOD_INT]       = &&lbl_raw_mod_int,
        [OP_RAW_FLOOR_DIV_INT] = &&lbl_raw_floor_div_int,
        [OP_RAW_ADD_REAL]      = &&lbl_raw_add_real,
        [OP_RAW_SUB_REAL]      = &&lbl_raw_sub_real,
        [OP_RAW_MUL_REAL]      = &&lbl_raw_mul_real,
        [OP_RAW_DIV_REAL]      = &&lbl_raw_div_real,
        [OP_RAW_LT_INT]        = &&lbl_raw_lt_int,
        [OP_RAW_GT_INT]        = &&lbl_raw_gt_int,
        [OP_RAW_LTE_INT]       = &&lbl_raw_lte_int,
        [OP_RAW_GTE_INT]       = &&lbl_raw_gte_int,
        [OP_RAW_LT_REAL]       = &&lbl_raw_lt_real,
        [OP_RAW_GT_REAL]       = &&lbl_raw_gt_real,
        [OP_RAW_LTE_REAL]      = &&lbl_raw_lte_real,
        [OP_RAW_GTE_REAL]      = &&lbl_raw_gte_real,
        [OP_BOX_INT]           = &&lbl_box_int,
        [OP_BOX_REAL]          = &&lbl_box_real,
        [OP_RAW_MOVE_INT]      = &&lbl_raw_move_int,
        [OP_RAW_MOVE_REAL]     = &&lbl_raw_move_real,
        [OP_RAW_ADD_INT_BOXED]  = &&lbl_raw_add_int_boxed,
        [OP_RAW_SUB_INT_BOXED]  = &&lbl_raw_sub_int_boxed,
        [OP_RAW_MUL_INT_BOXED]  = &&lbl_raw_mul_int_boxed,
        [OP_RAW_ADD_REAL_BOXED] = &&lbl_raw_add_real_boxed,
        [OP_RAW_SUB_REAL_BOXED] = &&lbl_raw_sub_real_boxed,
        [OP_RAW_MUL_REAL_BOXED] = &&lbl_raw_mul_real_boxed,
        [OP_RAW_LOAD_INT_POOL]  = &&lbl_raw_load_int_pool,
        [OP_RAW_LT_INT_BOXED]   = &&lbl_raw_lt_int_boxed,
        [OP_RAW_GT_INT_BOXED]   = &&lbl_raw_gt_int_boxed,
        [OP_RAW_LTE_INT_BOXED]  = &&lbl_raw_lte_int_boxed,
        [OP_RAW_GTE_INT_BOXED]  = &&lbl_raw_gte_int_boxed,
        [OP_RAW_LT_REAL_BOXED]  = &&lbl_raw_lt_real_boxed,
        [OP_RAW_GT_REAL_BOXED]  = &&lbl_raw_gt_real_boxed,
        [OP_RAW_LTE_REAL_BOXED] = &&lbl_raw_lte_real_boxed,
        [OP_RAW_GTE_REAL_BOXED] = &&lbl_raw_gte_real_boxed,
    };

    DISPATCH();

lbl_jump: {
    int target = READ();
    ip = (unsigned int)target;
    DISPATCH();
}

lbl_define_struct: {
    int name_idx    = READ();
    int field_count = READ();
    Shape* shape = xmalloc(sizeof(Shape));
    shape->name        = (unsigned int)name_idx;
    shape->field_count = (unsigned int)field_count;
    for (int i = 0; i < field_count; i++) {
        shape->field_names[i]    = (unsigned int)READ();
        shape->field_defaults[i] = c->pool[READ()];
        shape->field_types[i]    = (ValueType)READ();
    }
    if (c->shape_count >= c->shape_cap) {
        c->shape_cap = c->shape_cap ? c->shape_cap * 2 : 4;
        c->shapes = xrealloc(c->shapes, sizeof(Shape*) * c->shape_cap);
    }
    c->shapes[c->shape_count++] = shape;
    DISPATCH();
}

/* See OP_LOADK/OP_MOVE/OP_BINARY's own comments in vm.h. */
lbl_loadk: {
    int dest = (int)UNPACK_A(op_word);
    int pool_idx = READ();
    vm->registers[dest] = c->pool[pool_idx];
    DISPATCH();
}

lbl_move: {
    int dest = (int)UNPACK_A(op_word);
    int src  = (int)UNPACK_B(op_word);
    vm->registers[dest] = vm->registers[src];
    DISPATCH();
}

lbl_is_result: {
    int dest = (int)UNPACK_A(op_word);
    int src  = (int)UNPACK_B(op_word);
    vm->registers[dest] = aer_bool(aer_type(vm->registers[src]) == TYPE_RESULT);
    DISPATCH();
}

/* Dest + both RK operands packed into the single op_word DISPATCH() already fetched (see
   PACK_BINARY's comment in vm.h) — no further READ() at all. Each operator below is its own
   top-level dispatch target (true single-level dispatch, matching Lua's per-operator opcodes)
   instead of a shared OP_BINARY re-dispatching via a runtime switch (now vm_binary_cold()): DISPATCH()'s
   computed-goto already picked the exact right label, so there's no second jump to make. Each
   label checks its own int/int (and, where it applies, real/real) fast path locally — the common
   case for arithmetic-heavy code — and falls back to vm_binary_cold() (a real, non-inlined
   function; see its own comment above) only for anything else: nulls, strings, booleans, arrays,
   dicts, or a genuinely mixed int/real pair needing promotion. BINARY_OP_INT_REAL is for operators
   with both a fast int/int and fast real/real case; BINARY_OP_INT_ONLY is for the five bitwise/
   shift operators, which have no real/real meaning (vm_binary_cold's real/real switch already
   errors "Operator not valid for reals" for these, same as vm_binary_fast's always has). */
/* `result` is a pointer straight at the destination register, not a separate local — each
   INT_STMT/REAL_STMT below writes through it directly (`*result = ...`). Found via perf annotate:
   building a standalone AerVal local and copying it into vm->registers[dest] afterward compiled to
   a 16-byte ldmia/stmia round trip through the stack that was, on its own, the single hottest
   instruction in the entire nbody.aer profile (~3.6% of all cycles) — bigger than the RK9-decode
   branch it sits next to. Writing directly at the final address instead gives the compiler a real
   shot at folding each op's 1-2 field writes straight into vm->registers[dest], no intermediate.
   Safe even when dest aliases ra's or rb's register (e.g. `x = x + 1`): l and rv already snapshotted
   ra->as.i and rb->as.i by value before result is ever touched, and vm_binary_cold's (*ra, *rb) args
   are likewise passed by value at the call, before *result is written. */
/* gc_maybe_collect() lives ONLY in the vm_binary_cold() branch below, not after the whole if/else —
   confirmed by direct inspection of vm_binary_cold's own body that the ONLY allocation reachable
   from ANY instantiation of this macro (add/sub/mul/div/floor_div/eq/neq/lt/gt/lte/gte) is OP_ADD's
   string-concatenation case (an xmalloc + aer_make_string). Every int/int and real/real fast-path
   result (aer_int()/aer_real()) is a plain tagged-union construction, never heap allocation, so it
   never needs a GC checkpoint — this call's own two comparisons (gc_suppress_depth/
   pool_total_alloc_count) would otherwise run on literally every dispatch of the single most
   common opcode family regardless of operator or operand type, when 10 of these 11 operators
   (everything except OP_ADD) could never possibly need it in any branch at all. */
#define BINARY_OP_INT_REAL(NAME, OPENUM, INT_STMT, REAL_STMT) \
lbl_##NAME: { \
    int dest = (int)UNPACK_BINARY_DEST(op_word); \
    AerVal* ra = vm_rk_ptr9(vm, const_pool, UNPACK_RK_B9(op_word)); \
    AerVal* rb = vm_rk_ptr9(vm, const_pool, UNPACK_RK_C9(op_word)); \
    ValueType ta = ra->tag, tb = rb->tag; \
    AerVal* result = &vm->registers[dest]; \
    if (ta == TYPE_INTEGER && tb == TYPE_INTEGER) { \
        int64_t l = ra->as.i, rv = rb->as.i; \
        INT_STMT \
    } else if (ta == TYPE_REAL && tb == TYPE_REAL) { \
        double l = ra->as.d, rv = rb->as.d; \
        REAL_STMT \
    } else { \
        *result = vm_binary_cold(*ra, *rb, OPENUM, ta, tb); \
        gc_maybe_collect(vm); \
    } \
    DISPATCH(); \
}
/* Bitwise family (AND/OR/XOR/LSHIFT/RSHIFT) — int-only, and vm_binary_cold's error path for a
   non-integer operand never allocates either, so gc_maybe_collect() is never reachable from here at
   all, in any branch — removed outright rather than kept as unreachable-but-harmless insurance. */
#define BINARY_OP_INT_ONLY(NAME, OPENUM, INT_STMT) \
lbl_##NAME: { \
    int dest = (int)UNPACK_BINARY_DEST(op_word); \
    AerVal* ra = vm_rk_ptr9(vm, const_pool, UNPACK_RK_B9(op_word)); \
    AerVal* rb = vm_rk_ptr9(vm, const_pool, UNPACK_RK_C9(op_word)); \
    ValueType ta = ra->tag, tb = rb->tag; \
    AerVal* result = &vm->registers[dest]; \
    if (ta == TYPE_INTEGER && tb == TYPE_INTEGER) { \
        int64_t l = ra->as.i, rv = rb->as.i; \
        INT_STMT \
    } else { \
        *result = vm_binary_cold(*ra, *rb, OPENUM, ta, tb); \
    } \
    DISPATCH(); \
}

BINARY_OP_INT_REAL(add, OP_ADD, { *result = aer_int(l + rv); }, { *result = aer_real(l + rv); })
BINARY_OP_INT_REAL(sub, OP_SUB, { *result = aer_int(l - rv); }, { *result = aer_real(l - rv); })
BINARY_OP_INT_REAL(mul, OP_MUL, { *result = aer_int(l * rv); }, { *result = aer_real(l * rv); })
BINARY_OP_INT_REAL(div, OP_DIV,
    { if (rv == 0) { error("Division by zero"); *result = aer_int(0); } else { *result = aer_real((double)l / (double)rv); } },
    { if (rv == 0.0) { error("Division by zero"); *result = aer_real(0.0); } else { *result = aer_real(l / rv); } })
BINARY_OP_INT_REAL(floor_div, OP_FLOOR_DIV,
    { if (rv == 0) { error("Division by zero"); *result = aer_int(0); } else { *result = aer_int((int64_t)floor((double)l / (double)rv)); } },
    { if (rv == 0.0) { error("Division by zero"); *result = aer_real(0.0); } else { *result = aer_real(floor(l / rv)); } })
BINARY_OP_INT_REAL(mod, OP_MOD,
    { if (rv == 0) { error("Modulo by zero"); *result = aer_int(0); } else { *result = aer_int(l % rv); } },
    { *result = aer_real(fmod(l, rv)); })
BINARY_OP_INT_REAL(eq,  OP_EQ,  { *result = aer_bool(l == rv); }, { *result = aer_bool(l == rv); })
BINARY_OP_INT_REAL(neq, OP_NEQ, { *result = aer_bool(l != rv); }, { *result = aer_bool(l != rv); })
BINARY_OP_INT_REAL(lt,  OP_LT,  { *result = aer_bool(l <  rv); }, { *result = aer_bool(l <  rv); })
BINARY_OP_INT_REAL(gt,  OP_GT,  { *result = aer_bool(l >  rv); }, { *result = aer_bool(l >  rv); })
BINARY_OP_INT_REAL(lte, OP_LTE, { *result = aer_bool(l <= rv); }, { *result = aer_bool(l <= rv); })
BINARY_OP_INT_REAL(gte, OP_GTE, { *result = aer_bool(l >= rv); }, { *result = aer_bool(l >= rv); })
BINARY_OP_INT_ONLY(bitwise_and, OP_BITWISE_AND, { *result = aer_int(l & rv); })
BINARY_OP_INT_ONLY(bitwise_or,  OP_BITWISE_OR,  { *result = aer_int(l | rv); })
BINARY_OP_INT_ONLY(bitwise_xor, OP_BITWISE_XOR, { *result = aer_int(l ^ rv); })
BINARY_OP_INT_ONLY(lshift, OP_LSHIFT, { *result = aer_int(l << rv); })
BINARY_OP_INT_ONLY(rshift, OP_RSHIFT, { *result = aer_int(l >> rv); })

#undef BINARY_OP_INT_REAL
#undef BINARY_OP_INT_ONLY

/* `in` has no int/int or real/real fast path of its own — dict-key lookup or an array element
   scan either way — so it's just its own label with the same dict/array logic vm_binary_cold() uses
   for every other operator that reaches it, verbatim. */
lbl_in: {
    int dest = (int)UNPACK_BINARY_DEST(op_word);
    AerVal a = *vm_rk_ptr9(vm, const_pool, UNPACK_RK_B9(op_word));
    AerVal b = *vm_rk_ptr9(vm, const_pool, UNPACK_RK_C9(op_word));
    AerVal* result = &vm->registers[dest];
    if (aer_type(b) == TYPE_DICT) {
        if (aer_type(a) != TYPE_STRING) {
            error("Left side of 'in' must be a string when testing dict membership");
            *result = aer_bool(false);
        } else {
            AerString* as = aer_as_string(a);
            unsigned int klen = as->length;
            if (klen > VM_KEY_MAX) {
                error("Dict key too long (max %d bytes)", VM_KEY_MAX);
                *result = aer_bool(false);
            } else {
                char kbuf[VM_KEY_MAX + 1];
                memcpy(kbuf, as->data, klen);
                kbuf[klen] = '\0';
                *result = aer_bool(hashtable_get(&aer_as_dict(b)->map, kbuf) != NULL);
            }
        }
    } else if (aer_type(b) == TYPE_ARRAY) {
        AerArray* arr = aer_as_array(b);
        bool found = false;
        for (unsigned int i = 0; i < arr->count; i++) {
            if (values_equal(a, arr->items[i])) { found = true; break; }
        }
        *result = aer_bool(found);
    } else if (aer_type(b) == TYPE_STRING) {
        /* Substring search — same as vm_binary_cold's own OP_IN case and string.contains() (aer_string.c). */
        if (aer_type(a) != TYPE_STRING) {
            error("Left side of 'in' must be a string when testing string membership");
            *result = aer_bool(false);
        } else {
            AerString* needle = aer_as_string(a);
            AerString* hay    = aer_as_string(b);
            bool found = needle->length == 0;
            for (unsigned int i = 0; !found && i + needle->length <= hay->length; i++)
                if (memcmp(hay->data + i, needle->data, needle->length) == 0) found = true;
            *result = aer_bool(found);
        }
    } else {
        error("Right side of 'in' must be a dict, array, or string");
        *result = aer_bool(false);
    }
    DISPATCH();
}

/* M2 — control flow. Stack-neutral, same as the M1 opcodes above — reads vm->registers[]/the chunk
   pool only, never pops/pushes anything, and OP_JUMP (reused as-is for unconditional jumps) is
   already stack-neutral too. */
lbl_jump_if_false_reg: {
    int reg    = (int)UNPACK_A(op_word);
    int target = READ();
    if (!vm_truthy(vm->registers[reg])) ip = (unsigned int)target;
    DISPATCH();
}

/* M5 — real per-call register windowing. Bulk-copies arg_reg_base..+arg_count from the CALLER's
   bank into the NEW callee frame's bank (always starting at its own register 0 — no more fixed
   shared offset to agree on) in one dispatch, same bulk-copy mechanism M3 proved, now landing in
   an isolated frame instead of a shared fixed range. Mirrors the stack VM's own overflow check
   (vm.c's vm_setup_call: `if (target->call_depth >= VM_CALL_MAX) { error("Call stack overflow");
   ... }`) almost verbatim — same ceiling, same error-then-DISPATCH() discipline. */
lbl_call: {
    int dest_reg      = (int)UNPACK_A(op_word);
    int arg_reg_base  = (int)UNPACK_B(op_word);
    int arg_count     = (int)UNPACK_C(op_word);
    int callee_offset = READ();
    /* tail-call reuse, see OP_TAIL_CALL's own comment in vm.h. The copy below iterates
       i in INCREASING order with dest_i (== i) always <= source_i (== arg_reg_base + i, and
       arg_reg_base is never negative) — the same "shift toward a lower-or-equal address" pattern
       memmove(dest, src, n) uses when dest <= src, which is always safe to do element-by-element
       forward regardless of how much the two ranges overlap (each register is read as a source at
       most once, always before whichever later iteration — if any — overwrites it as a
       destination). So this needs no special-casing for overlap, whether or not it aliases the
       CALLEE's own parameter registers (which may have nothing to do with the CALLER's own
       reserved_floor — a genuinely different function's arity). */
    if (cur_op == OP_TAIL_CALL) {
        for (int i = 0; i < arg_count; i++)
            vm->registers[i] = vm->registers[arg_reg_base + i];
        ip = (unsigned int)callee_offset;
        DISPATCH();
    }
    if (vm->call_depth + 1 >= VM_CALL_MAX) { error("v3 call stack overflow"); DISPATCH(); }
    CallFrame* caller = &vm->call_stack[vm->call_depth];
    CallFrame* callee = &vm->call_stack[vm->call_depth + 1];
    /* FRAME_REGISTERS, not the callee's own real max_registers: OP_CALL only carries a raw code
       offset, not a stable function reference the way setup_call/vm_call_value's ChunkFunction and
       AerFunction pointers already do — getting the real per-function count here would need
       OP_CALL to also carry a function index, patched the same way callee_offset itself is for a
       forward reference (parser.c's pending_call_add), which self-recursive calls make non-trivial
       (this function's own max_registers isn't captured until its body finishes — see
       parse_function's comment). Deliberately not built: this still gets the zero-allocation
       bump-pointer property (the actual regression fix), just not the cache-locality bonus, for
       this one call path. */
    callee->registers  = caller->registers + caller->frame_size;
    callee->frame_size  = FRAME_REGISTERS;
    for (int i = 0; i < arg_count; i++)
        callee->registers[i] = caller->registers[arg_reg_base + i];
    callee->return_ip   = ip;   /* already past this instruction's operands — the correct resume point */
    callee->dest_reg    = dest_reg;
    vm->call_depth++;
    vm->registers = vm->call_stack[vm->call_depth].registers;
    vm->raw_ints  = vm->call_stack[vm->call_depth].raw_ints;
    vm->raw_reals = vm->call_stack[vm->call_depth].raw_reals;
    ip = (unsigned int)callee_offset;
    DISPATCH();
}

/* Functions as values. Same frame-push/bulk-copy shape as lbl_call, but the target is read from
   a REGISTER at dispatch time (a runtime AerFunction, built by build_function_value at the point
   a function name was referenced as a value) instead of a compile-time callee_offset. */
lbl_call_value: {
    int dest_reg     = (int)UNPACK_REG4_A(op_word);
    int arg_reg_base = (int)UNPACK_REG4_B(op_word);
    int arg_count    = (int)UNPACK_REG4_C(op_word);
    int callee_reg   = (int)UNPACK_REG4_D(op_word);
    /* vm_call_value writes a new vm->ip internally (function entry) or leaves it untouched (an
       error return) — either way, ip must be reloaded from it before the next READ(), since
       DISPATCH() only writes vm->ip, it doesn't read it back. OP_CALL_VALUE is a single packed
       word with no trailing operand (PACK_REG4), so the local `ip` already holds the correct
       resume address (past this instruction, nothing else to skip) to pass as return_ip. */
    vm_call_value(vm, vm->registers[callee_reg], dest_reg, arg_reg_base, arg_count,
                      cur_op == OP_TAIL_CALL_VALUE, ip);
    ip = vm->ip;
    DISPATCH();
}

/* src_reg is a plain 0-based index into the CALLEE's own frame. return_ip/dest_reg live in the
   callee's own frame (not a single shared global), which is exactly what makes nested/recursive
   calls safe: an outer call's return info can't be clobbered by an inner one. */
lbl_return: {
    int src_reg = (int)UNPACK_A(op_word);
    CallFrame* callee = &vm->call_stack[vm->call_depth];

    AerVal result = callee->registers[src_reg];
    unsigned int return_ip = callee->return_ip;
    int dest_reg = callee->dest_reg;
    vm->call_depth--;
    vm->registers = vm->call_stack[vm->call_depth].registers;
    vm->raw_ints  = vm->call_stack[vm->call_depth].raw_ints;
    vm->raw_reals = vm->call_stack[vm->call_depth].raw_reals;
    vm->registers[dest_reg] = result;
    ip = return_ip;
    DISPATCH();
}

/* See OP_CALL_MODULE's comment in vm.h — bridges to the exact same stack-based stdlib dispatch
   lbl_call_module uses, since reimplementing every stdlib function for registers would be pure
   duplication. */
lbl_call_module: {
    int dest_reg     = (int)UNPACK_CALL_MODULE_DEST(op_word);
    int arg_reg_base = (int)UNPACK_CALL_MODULE_ARG_BASE(op_word);
    int arg_count    = (int)UNPACK_CALL_MODULE_ARG_COUNT(op_word);
    int module_idx   = (int)UNPACK_CALL_MODULE_MODULE(op_word);
    int fn_idx       = (int)UNPACK_CALL_MODULE_FN(op_word);
    int module_id    = READ();
    int fn_id        = READ();
    for (int i = 0; i < arg_count; i++) PUSH(vm->registers[arg_reg_base + i]);
    bool handled = false;
    /* module_id/fn_id were resolved once, at parse time (module_call_id/module_fn_id, parser.c)
       — a switch on two small ints instead of a strcmp chain against every known built-in
       module's name, then another against every one of that module's function names, on every
       single call. Only CALL_MODULE_DYNAMIC (a host-registered module or a user file import —
       never a fixed core built-in) still needs the original by-name resolution, since those are
       genuinely only knowable at runtime; module/fn names are resolved below only on that path
       and on the error path — the happy path here never touches the constant pool. */
    switch (module_id) {
        case CALL_MODULE_MATH:   handled = aer_math_call(vm, fn_id, arg_count);   break;
        case CALL_MODULE_RANDOM: handled = aer_random_call(vm, fn_id, arg_count); break;
        case CALL_MODULE_STRING: handled = aer_string_call(vm, fn_id, arg_count); break;
        case CALL_MODULE_TIME:   handled = aer_time_call(vm, fn_id, arg_count);   break;
        case CALL_MODULE_JSON:   handled = aer_json_call(vm, c, fn_id, arg_count);   break;
        default: {
            const char* module = aer_as_string(c->pool[module_idx])->data;
            const char* fn     = aer_as_string(c->pool[fn_idx])->data;
            if (aer_host_is_module(module, (unsigned int)strlen(module)))
                handled = aer_host_call(vm, module, fn, arg_count);
            else
                handled = aer_module_call(vm, module, fn, arg_count);
            break;
        }
    }
    if (handled) { vm->registers[dest_reg] = POP(); gc_maybe_collect(vm); DISPATCH(); }   /* stdlib/module functions routinely allocate (new strings/arrays/etc.) */
    error("'%s' has no function '%s'", aer_as_string(c->pool[module_idx])->data, aer_as_string(c->pool[fn_idx])->data);
    for (int i = 0; i < arg_count; i++) POP();
    vm->registers[dest_reg] = aer_null();
    DISPATCH();
}

/* See OP_CALL_BUILTIN's comment in vm.h — vm_call_builtin() already takes a plain AerVal*
   array, so unlike OP_CALL_MODULE this needs no push/pop bridge to vm->stack at all. 4 local
   slots is headroom over every builtin's real max arity (2 — delete/append/assert). */
lbl_call_builtin: {
    int dest_reg     = (int)UNPACK_CALL_BUILTIN_DEST(op_word);
    int arg_reg_base = (int)UNPACK_CALL_BUILTIN_ARG_BASE(op_word);
    int arg_count    = (int)UNPACK_CALL_BUILTIN_ARG_COUNT(op_word);
    int name_idx     = (int)UNPACK_CALL_BUILTIN_NAME(op_word);
    int builtin_id   = (int)READ();
    /* name is only resolved on the error paths — the happy path never needs it. */
    if (arg_count > 4) {
        error("Too many arguments to '%s'", aer_as_string(c->pool[name_idx])->data);
        vm->registers[dest_reg] = aer_null();
        DISPATCH();
    }
    AerVal args[4];
    for (int i = 0; i < arg_count; i++) args[i] = vm->registers[arg_reg_base + i];
    bool handled = vm_call_builtin(c, builtin_id, args, arg_count, &vm->registers[dest_reg]);
    if (!handled) error("'%s' is not defined, or was called with the wrong number of arguments",
                        aer_as_string(c->pool[name_idx])->data);
    gc_maybe_collect(vm);   /* vm_call_builtin: struct_pool site + aer_make_string (type()) */
    DISPATCH();
}

/* Builds a new array from an already-in-order register range (pool_alloc/capacity/items/shape
   setup) — this and every opcode below that can put a heap pointer into vm->registers[] rely on
   mark_vm_roots's registers scan to keep it alive across a GC cycle. */
lbl_array_new: {
    int dest_reg      = (int)UNPACK_A(op_word);
    int item_reg_base = (int)UNPACK_B(op_word);
    int item_count    = (int)UNPACK_C(op_word);
    AerArray* a = pool_alloc(&array_pool);
    a->capacity = item_count > 0 ? (unsigned int)item_count : 4;
    a->count    = (unsigned int)item_count;
    a->items    = xmalloc(sizeof(AerVal) * a->capacity);
    a->shape    = NULL;
    for (int i = 0; i < item_count; i++)
        a->items[i] = vm->registers[item_reg_base + i];
    vm->registers[dest_reg] = aer_array_val(a);
    gc_maybe_collect(vm);   /* pool_alloc(&array_pool) above; result already rooted */
    DISPATCH();
}

/* Reuses vm_index_get_compute (above) as-is — already type-generic (array/dict/string) and
   already has all bounds/negative-index logic, so nothing about indexing itself needed
   reimplementing for the register path. */
lbl_index_get: {
    int dest_reg = (int)UNPACK_INDEX_GET_DEST(op_word);
    int arr_reg  = (int)UNPACK_INDEX_GET_ARR(op_word);
    AerVal* idx = vm_rk_ptr9(vm, const_pool, UNPACK_INDEX_GET_RK(op_word));
    AerVal obj = vm->registers[arr_reg];
    vm_index_get_compute(obj, *idx, &vm->registers[dest_reg]);
    /* Only single-char string indexing allocates (a fresh 1-char string, vm_index_get_compute) —
       array/dict indexing just copies an existing value, never touching the heap. Checked here
       instead of calling unconditionally so the array/dict common case (e.g. nbody.aer's own
       struct-field arrays, dominant in real code) never pays for a GC check it can't need. */
    if (aer_type(obj) == TYPE_STRING) gc_maybe_collect(vm);
    DISPATCH();
}

/* Reuses vm_index_set_compute (above) as-is — including its internal gc_barrier_array/
   gc_barrier_dict call, which this is the first v3 opcode to exercise against a register-held
   reference rather than a stack-held one. */
lbl_index_set: {
    int arr_reg = (int)UNPACK_INDEX_SET_ARR(op_word);
    AerVal idx = *vm_rk_ptr20(vm, const_pool, UNPACK_INDEX_SET_IDX(op_word));
    AerVal val = *vm_rk_ptr20(vm, const_pool, UNPACK_INDEX_SET_VAL(op_word));
    vm_index_set_compute(vm->registers[arr_reg], idx, val);
    DISPATCH();
}

/* `arr[a:b]` — vm_slice_bounds() resolves/clamps the bounds; a slice is always a fresh copy. */
lbl_slice_get: {
    int dest_reg = (int)UNPACK_SLICE_GET_DEST(op_word);
    int arr_reg  = (int)UNPACK_SLICE_GET_ARR(op_word);
    AerVal start_v = *vm_rk_ptr20(vm, const_pool, UNPACK_SLICE_GET_START(op_word));
    AerVal end_v   = *vm_rk_ptr20(vm, const_pool, UNPACK_SLICE_GET_END(op_word));
    AerVal obj = vm->registers[arr_reg];
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) { error("Structs cannot be sliced"); vm->registers[dest_reg] = aer_null(); DISPATCH(); }
        int64_t start, end;
        if (!vm_slice_bounds(start_v, end_v, (int64_t)a->count, &start, &end)) { vm->registers[dest_reg] = aer_null(); DISPATCH(); }
        unsigned int n = (unsigned int)(end - start);
        AerArray* r = pool_alloc(&array_pool);
        r->count    = n;
        r->capacity = n > 0 ? n : 4;
        r->items    = xmalloc(sizeof(AerVal) * r->capacity);
        r->shape    = NULL;   /* a slice is always a plain array, even of a struct */
        for (unsigned int i = 0; i < n; i++) r->items[i] = a->items[start + i];
        vm->registers[dest_reg] = aer_array_val(r);
    } else if (aer_type(obj) == TYPE_STRING) {
        AerString* os = aer_as_string(obj);
        int64_t start, end;
        if (!vm_slice_bounds(start_v, end_v, (int64_t)os->length, &start, &end)) { vm->registers[dest_reg] = aer_null(); DISPATCH(); }
        unsigned int sub_len = (unsigned int)(end - start);
        char* sub_buf = xmalloc(sub_len + 1);
        memcpy(sub_buf, os->data + start, sub_len);
        sub_buf[sub_len] = '\0';
        vm->registers[dest_reg] = aer_make_string(sub_buf, sub_len);   /* no chunk_add_pool interning — see vm_to_str's comment */
    } else {
        error("Cannot slice this type");
        vm->registers[dest_reg] = aer_null();
    }
    gc_maybe_collect(vm);   /* array branch: pool_alloc(&array_pool); string branch: aer_make_string; the null-result branches above are harmless no-ops here too */
    DISPATCH();
}

/* `x as Point` where Point is a known struct type: errors unless src_reg holds exactly that
   struct type, else passes the value through unchanged (never converts). */
lbl_check_shape: {
    int dest_reg = (int)UNPACK_CHECK_SHAPE_DEST(op_word);
    int src_reg  = (int)UNPACK_CHECK_SHAPE_LHS(op_word);
    int name_idx = (int)UNPACK_CHECK_SHAPE_NAME(op_word);
    AerVal v = vm->registers[src_reg];
    if (aer_type(v) != TYPE_ARRAY || !aer_as_array(v)->shape || aer_as_array(v)->shape->name != (unsigned int)name_idx) {
        error("Expected a '%s', got a '%s'", aer_as_string(c->pool[name_idx])->data, vm_type_name(c, v));
        vm->registers[dest_reg] = aer_null();
        DISPATCH();
    }
    vm->registers[dest_reg] = v;
    DISPATCH();
}

/* Dict literal — keys must be strings; each key is stored as an owned copy (hashtable_key_dup),
   never an alias into the source string. */
lbl_dict_new: {
    int dest_reg      = (int)UNPACK_A(op_word);
    int pair_reg_base = (int)UNPACK_B(op_word);
    int pair_count    = (int)UNPACK_C(op_word);
    AerDict* d = pool_alloc(&dict_pool);
    memset(&d->map, 0, sizeof(d->map));
    for (int i = 0; i < pair_count; i++) {
        AerVal key = vm->registers[pair_reg_base + 2 * i];
        AerVal val = vm->registers[pair_reg_base + 2 * i + 1];
        if (aer_type(key) != TYPE_STRING) { error("Dict keys must be strings"); continue; }
        AerString* ks = aer_as_string(key);
        char* k = hashtable_key_dup(ks->data, ks->length, NULL);
        hashtable_put(&d->map, k, val);
    }
    vm->registers[dest_reg] = aer_dict_val(d);
    gc_maybe_collect(vm);   /* pool_alloc(&dict_pool) above; result already rooted */
    DISPATCH();
}

/* `for x in collection:` — arrays yield items, dicts yield keys, strings yield 1-char strings
   (see OP_ITER_NEXT_ARRAY's comment, vm.h). */
lbl_iter_next_array: {
    int col_reg       = (int)UNPACK_A(op_word);
    int idx_reg       = (int)UNPACK_B(op_word);
    int item_dest_reg = (int)UNPACK_C(op_word);
    int end_target    = READ();
    AerVal col = vm->registers[col_reg];
    int64_t idx = aer_as_int(vm->registers[idx_reg]);
    if (aer_type(col) == TYPE_DICT) {
        /* Single-variable `for k in dict:` yields keys. */
        AerVal key;
        if (!vm_dict_next_key(aer_as_dict(col), &idx, &key)) {
            ip = (unsigned int)end_target;
            DISPATCH();
        }
        vm->registers[item_dest_reg] = key;
        vm->registers[idx_reg]       = aer_int(idx + 1);
        gc_maybe_collect(vm);   /* vm_dict_next_key's owned-copy key string allocates */
        DISPATCH();
    }
    if (aer_type(col) == TYPE_STRING) {
        /* `for c in string:` yields one-character strings, same as `for k in dict:` yields keys
           above rather than a dedicated opcode. */
        AerString* cs = aer_as_string(col);
        if ((uint64_t)idx >= cs->length) {
            ip = (unsigned int)end_target;
            DISPATCH();
        }
        char* ch_buf = xmalloc(2);
        ch_buf[0] = cs->data[idx];
        ch_buf[1] = '\0';
        vm->registers[item_dest_reg] = aer_make_string(ch_buf, 1);   /* no chunk_add_pool interning — see vm_to_str's comment */
        vm->registers[idx_reg]       = aer_int(idx + 1);
        gc_maybe_collect(vm);
        DISPATCH();
    }
    if (aer_type(col) != TYPE_ARRAY) {
        error("'for x in ...' only supports arrays, dicts, and strings");
        DISPATCH();
    }
    AerArray* a = aer_as_array(col);
    if ((uint64_t)idx >= a->count) {
        ip = (unsigned int)end_target;
        DISPATCH();
    }
    vm->registers[item_dest_reg] = a->items[idx];
    vm->registers[idx_reg]       = aer_int(idx + 1);
    DISPATCH();
}

/* `for k, v in dict:` — see vm_dict_next_key (above) for the shared bucket-scan/copy-key logic. */
lbl_iter_next_pair: {
    int col_reg       = (int)UNPACK_REG4_A(op_word);
    int idx_reg       = (int)UNPACK_REG4_B(op_word);
    int key_dest_reg  = (int)UNPACK_REG4_C(op_word);
    int val_dest_reg  = (int)UNPACK_REG4_D(op_word);
    int end_target    = READ();
    AerVal col = vm->registers[col_reg];
    if (aer_type(col) != TYPE_DICT) {
        error("for k, v requires a dict");
        ip = (unsigned int)end_target;
        DISPATCH();
    }
    AerDict* d = aer_as_dict(col);
    int64_t idx = aer_as_int(vm->registers[idx_reg]);
    AerVal key;
    if (!vm_dict_next_key(d, &idx, &key)) {
        ip = (unsigned int)end_target;
        DISPATCH();
    }
    vm->registers[key_dest_reg] = key;
    vm->registers[val_dest_reg] = d->map.buckets[idx].payload;
    vm->registers[idx_reg]      = aer_int(idx + 1);
    gc_maybe_collect(vm);   /* vm_dict_next_key's owned-copy key string allocates */
    DISPATCH();
}

/* Runs ONCE, before a rotated range-for loop (see OP_ITER_RANGE_PREP's own comment, vm.h) — same
   TYPE_INTEGER checks/direction-inference/exit-condition shape as OP_ITER_RANGE_LOOP below, just
   never advances cur_reg (there is no "next iteration" to prepare for yet; OP_ITER_RANGE_LOOP owns
   advancing). */
lbl_iter_range_prep: {
    int cur_reg       = (int)UNPACK_REG4_A(op_word);
    int end_reg       = (int)UNPACK_REG4_B(op_word);
    int step_reg      = (int)UNPACK_REG4_C(op_word);
    int item_dest_reg = (int)UNPACK_REG4_D(op_word);
    int empty_target  = READ();
    AerVal cur_v  = vm->registers[cur_reg];
    AerVal end_v  = vm->registers[end_reg];
    AerVal step_v = vm->registers[step_reg];
    if (aer_type(cur_v) != TYPE_INTEGER || aer_type(end_v) != TYPE_INTEGER || aer_type(step_v) != TYPE_INTEGER) {
        error("Range bounds and step must be integers");
        ip = (unsigned int)empty_target;
        DISPATCH();
    }
    int64_t cur = aer_as_int(cur_v), rng_end = aer_as_int(end_v), step = aer_as_int(step_v);
    if (step <= 0) {
        error("Range step must be a positive integer (direction is inferred from the bounds, not the step's sign)");
        ip = (unsigned int)empty_target;
        DISPATCH();
    }
    /* Precomputes a total iteration count ONCE, instead of re-deriving "still in range" from a
       direction-dependent comparison against the original limit on every single dispatch of
       OP_ITER_RANGE_LOOP — matching Lua's own FORLOOP design (its FORPREP does the equivalent
       count computation). Ceiling division so a step that doesn't evenly divide the range still
       gets the correct final count (e.g. 0..10..3 must stop after 0,3,6,9 — 4 iterations, not 3
       or 4.33). count==0 means an empty range, same exit as before. */
    bool ascending = cur < rng_end;
    int64_t diff  = ascending ? (rng_end - cur) : (cur - rng_end);
    /* step == 1 (the overwhelmingly common case — no explicit `..step` in the source) skips the
       division entirely: count is just diff. This target has no fast hardware integer divide, and
       short, frequently-re-entered range-for loops (nbody.aer's inner loops, not a long-running
       counting loop) pay PREP's one-time division cost often enough that skipping it here is a
       real win, not just a theoretical one. */
    int64_t count = (step == 1) ? diff : (diff + step - 1) / step;
    if (count == 0) {
        ip = (unsigned int)empty_target;
        DISPATCH();
    }
    /* end_reg/step_reg are repurposed from here on, for the rest of this loop's life —
       parse_for_in's arg_materialize snapshot already guarantees they're fresh, loop-owned
       registers nothing else in the program ever reads, so overwriting their ORIGINAL
       bound/step values with derived bookkeeping (a countdown, and a direction-adjusted step) is
       safe. OP_ITER_RANGE_LOOP reads them back under their new meaning — see its own comment. */
    vm->registers[end_reg]       = aer_int(count - 1);                 /* iterations remaining AFTER this one */
    vm->registers[step_reg]      = aer_int(ascending ? step : -step);  /* direction baked in once, not re-inferred every iteration */
    vm->registers[item_dest_reg] = cur_v;
    DISPATCH();
}

/* Runs once per iteration, at the BOTTOM of a rotated range-for loop's body (see
   OP_ITER_RANGE_LOOP's own comment, vm.h). end_reg/step_reg no longer hold the range's original
   bound/step here — OP_ITER_RANGE_PREP repurposes them into a countdown ("remaining_reg") and a
   direction-adjusted step ("signed_step_reg") the first time it runs, so this handler never needs
   to re-derive "still in range" from a fresh comparison against the original limit, matching how
   Lua's own FORLOOP works (its FORPREP does the equivalent countdown setup) — found by comparing
   AER's actual per-iteration cost against Lua's real algorithm, not just its opcode name. cur_reg
   holds the value the body just used (written by lbl_iter_range_prep for the first iteration, or
   by this same label for every iteration after) — advances it by the already-signed step (no
   direction ternary needed here at all now), and only writes back (to cur_reg, item_dest_reg, and
   the countdown) and branches backward if iterations remain; otherwise leaves cur_reg/item_dest_reg
   untouched and falls through to the exit code right after this instruction. body_target is always
   a plain, already-resolved address — never a patch_jump placeholder, unlike every other loop
   form's back-edge (see parse_for_in, parser.c). No type/step re-validation here — parse_for_in
   snapshots cur/end/step once via arg_materialize before the loop starts, so they can never be an
   alias to a mutable variable, and OP_ITER_RANGE_PREP already validated them once; see this
   opcode's own top comment, vm.h. No gc_maybe_collect() — aer_int() is a plain tagged-union
   construction (value.h), never heap allocation, under the current AerVal representation; an
   earlier "overflow-box path" concern applied to this project's prior NaN-boxing representation,
   eliminated by the later tagged-union migration but never cleaned out of this comment until now. */
lbl_iter_range_loop: {
    int cur_reg         = (int)UNPACK_REG4_A(op_word);
    int remaining_reg   = (int)UNPACK_REG4_B(op_word);
    int signed_step_reg = (int)UNPACK_REG4_C(op_word);
    int item_dest_reg   = (int)UNPACK_REG4_D(op_word);
    int body_target     = READ();
    int64_t remaining = aer_as_int(vm->registers[remaining_reg]);
    if (remaining == 0) {
        DISPATCH();   /* exhausted — fall through to the exit code, cur_reg/item_dest_reg untouched */
    }
    int64_t signed_step = aer_as_int(vm->registers[signed_step_reg]);
    int64_t new_cur      = aer_as_int(vm->registers[cur_reg]) + signed_step;
    AerVal new_cur_v = aer_int(new_cur);
    vm->registers[cur_reg]       = new_cur_v;
    vm->registers[item_dest_reg] = new_cur_v;
    vm->registers[remaining_reg] = aer_int(remaining - 1);
    ip = (unsigned int)body_target;
    DISPATCH();
}

/* Struct instantiation — chunk_find_shape() by name, arity check, one struct_pool allocation
   with items inline after the header, omitted trailing fields default-filled. */
lbl_struct_new: {
    int dest_reg           = (int)UNPACK_STRUCT_NEW_DEST(op_word);
    int arg_reg_base       = (int)UNPACK_STRUCT_NEW_ARG_BASE(op_word);
    int arg_count          = (int)UNPACK_STRUCT_NEW_ARG_COUNT(op_word);
    int type_name_pool_idx = (int)UNPACK_STRUCT_NEW_NAME(op_word);
    const char* name = aer_as_string(c->pool[type_name_pool_idx])->data;
    Shape* shape = chunk_find_shape(c, name);
    if (!shape) { error("'%s' is not defined", name); DISPATCH(); }
    if ((unsigned int)arg_count > shape->field_count) {
        error("'%s' takes at most %u argument%s, got %d",
              name, shape->field_count, shape->field_count == 1 ? "" : "s", arg_count);
        DISPATCH();
    }
    /* Positional constructor args must match a typed field's declared type too — OP_FIELD_SET
       isn't the only way a field's value is ever set, and the fused field-arithmetic opcodes'
       typed fast path trusts every field unconditionally, not just ones reached via `.field =`.
       Checked before allocating anything, so a mismatch never leaves a half-built struct behind. */
    for (int i = 0; i < arg_count; i++) {
        ValueType declared = shape->field_types[i];
        if (declared != TYPE_ANY && vm->registers[arg_reg_base + i].tag != declared) {
            error("'%s' field '%s' is declared as a fixed type and cannot be constructed with a different type",
                  name, aer_as_string(c->pool[shape->field_names[i]])->data);
            DISPATCH();
        }
    }
    AerArray* a = pool_alloc(&struct_pool);
    a->count = a->capacity = shape->field_count;
    a->items = (AerVal*)((char*)a + sizeof(AerArray));
    a->shape = shape;
    for (int i = 0; i < arg_count; i++)
        a->items[i] = vm->registers[arg_reg_base + i];
    for (unsigned int i = (unsigned int)arg_count; i < shape->field_count; i++)
        a->items[i] = vm_default_value(shape->field_defaults[i]);
    vm->registers[dest_reg] = aer_array_val(a);
    gc_maybe_collect(vm);   /* pool_alloc(&struct_pool) above, plus any vm_default_value array/dict defaults — all rooted now that the struct itself is stored */
    DISPATCH();
}

/* Reading struct_reg from a register instead of popping the stack; see vm_resolve_field (above)
   for the shared field-slot lookup/cache. */
lbl_field_get: {
    unsigned int site = ip - 1;
    int dest_reg   = (int)UNPACK_FIELD_GET_DEST(op_word);
    int struct_reg = (int)UNPACK_FIELD_GET_STRUCT(op_word);
    int field_idx  = (int)UNPACK_FIELD_GET_FIELD(op_word);
    AerArray* oa; int slot;
    if (!vm_resolve_field(vm, c, site, struct_reg, field_idx, &oa, &slot)) DISPATCH();
    vm->registers[dest_reg] = oa->items[slot];
    DISPATCH();
}

/* Fusion opcode — see its own comment in vm.h and vm_resolve_field (above) for the shared
   field-slot lookup/cache. Feeds the field value straight into the binary op instead of writing
   it to a register first. bin_op is a runtime value here (unlike lbl_add/lbl_sub/etc., which each
   get their own statically-known opcode), so a per-operator switch can't be avoided the way it was
   for the standalone case — but the int/int and real/real cases (the common ones for arithmetic-
   heavy code) are still handled by the small always_inline vm_binary_fast() below, falling back to
   the non-inlined vm_binary_cold() only for anything else. */
lbl_binary_field: {
    unsigned int site   = ip - 1;
    int dest_reg   = (int)UNPACK_BINARY_FIELD_DEST(op_word);
    int struct_reg = (int)UNPACK_BINARY_FIELD_STRUCT(op_word);
    Opcode bin_op  = (Opcode)UNPACK_BINARY_FIELD_OP(op_word);
    int field_idx  = (int)UNPACK_BINARY_FIELD_NAME(op_word);
    AerVal* lhs = vm_rk_ptr20(vm, const_pool, UNPACK_BINARY_FIELD_RK(op_word));
    AerArray* oa; int slot;
    if (!vm_resolve_field(vm, c, site, struct_reg, field_idx, &oa, &slot)) DISPATCH();
    AerVal rhs = oa->items[slot];
    ValueType ta = aer_type(*lhs), tb = aer_type(rhs);
    bool handled;
    vm->registers[dest_reg] = vm_binary_fast(*lhs, rhs, bin_op, ta, tb, &handled);
    /* gc_maybe_collect only ever needed on the cold path (string concat) — see BINARY_OP_INT_REAL's
       own comment above for why the fast path (vm_binary_fast's int/int and real/real cases) can
       never allocate. */
    if (!handled) {
        vm->registers[dest_reg] = vm_binary_cold(*lhs, rhs, bin_op, ta, tb);
        gc_maybe_collect(vm);
    }
    DISPATCH();
}

/* Mirror of lbl_binary_field for the other operand order — see OP_FIELD_BINARY's own comment in
   vm.h. */
lbl_field_binary: {
    unsigned int site   = ip - 1;
    int dest_reg   = (int)UNPACK_FIELD_BINARY_DEST(op_word);
    int struct_reg = (int)UNPACK_FIELD_BINARY_STRUCT(op_word);
    Opcode bin_op  = (Opcode)UNPACK_FIELD_BINARY_OP(op_word);
    int field_idx  = (int)UNPACK_FIELD_BINARY_NAME(op_word);
    AerVal* rhs = vm_rk_ptr20(vm, const_pool, UNPACK_FIELD_BINARY_RK(op_word));
    AerArray* oa; int slot;
    if (!vm_resolve_field(vm, c, site, struct_reg, field_idx, &oa, &slot)) DISPATCH();
    AerVal lhs = oa->items[slot];
    ValueType ta = aer_type(lhs), tb = aer_type(*rhs);
    bool handled;
    vm->registers[dest_reg] = vm_binary_fast(lhs, *rhs, bin_op, ta, tb, &handled);
    if (!handled) {
        vm->registers[dest_reg] = vm_binary_cold(lhs, *rhs, bin_op, ta, tb);
        gc_maybe_collect(vm);
    }
    DISPATCH();
}

/* Shell mode auto-print: a bare statement's result is printed unless null. */
lbl_print_repl: {
    int src_reg = (int)UNPACK_A(op_word);
    AerVal v = vm->registers[src_reg];
    if (aer_type(v) != TYPE_NULL) {
        vm_print_value(c, v, false);
        printf("\n");
    }
    DISPATCH();
}

/* See vm_resolve_field (above) for the shared field-slot lookup/cache. gc_barrier_array is the
   write barrier every mutating struct/array/dict write needs. */
lbl_field_set: {
    unsigned int site = ip - 1;
    int struct_reg = (int)UNPACK_FIELD_SET_STRUCT(op_word);
    int field_idx  = (int)UNPACK_FIELD_SET_FIELD(op_word);
    AerVal* val = vm_rk_ptr9(vm, const_pool, UNPACK_FIELD_SET_RK(op_word));
    AerArray* oa; int slot;
    if (!vm_resolve_field(vm, c, site, struct_reg, field_idx, &oa, &slot)) DISPATCH();
    /* Enforced once here, at the only place a field's value ever changes — trusted everywhere
       else (including the fused field-arithmetic opcodes' typed fast path below), never
       re-checked on read. TYPE_ANY means the field is untyped, same as today. */
    ValueType declared = oa->shape->field_types[slot];
    if (declared != TYPE_ANY && val->tag != declared) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    gc_barrier_array(oa, *val);
    oa->items[slot] = *val;
    DISPATCH();
}

/* `Type[count]` — see TYPE_PACKED_ARRAY's own comment, value.h. Eligibility (every field is a
   fixed primitive) is checked here, at runtime, for the same reason OP_STRUCT_NEW's positional-arg
   type check is: a Shape is only fully known once its OP_DEFINE_STRUCT has actually run, not at
   parse time. */
lbl_packed_array_new: {
    int dest_reg            = (int)UNPACK_PACKED_ARRAY_NEW_DEST(op_word);
    int type_name_pool_idx  = (int)UNPACK_PACKED_ARRAY_NEW_NAME(op_word);
    AerVal* count_v = vm_rk_ptr9(vm, const_pool, (uint32_t)UNPACK_PACKED_ARRAY_NEW_COUNT(op_word));
    const char* name = aer_as_string(c->pool[type_name_pool_idx])->data;
    Shape* shape = chunk_find_shape(c, name);
    if (!shape) { error("'%s' is not defined", name); DISPATCH(); }
    for (unsigned int i = 0; i < shape->field_count; i++) {
        ValueType ft = shape->field_types[i];
        if (ft != TYPE_INTEGER && ft != TYPE_REAL && ft != TYPE_BOOLEAN) {
            error("'%s' cannot be packed into an array: field '%s' must be integer/float/boolean, not %s",
                  name, aer_as_string(c->pool[shape->field_names[i]])->data,
                  ft == TYPE_ANY ? "any" : "string");
            DISPATCH();
        }
    }
    if (aer_type(*count_v) != TYPE_INTEGER) { error("Packed array count must be an integer"); DISPATCH(); }
    int64_t count = aer_as_int(*count_v);
    if (count < 0) { error("Packed array count must not be negative"); DISPATCH(); }
    unsigned int element_size = shape->field_count * 8;
    AerPackedArray* pa = pool_alloc(&packed_array_pool);
    pa->count = (unsigned int)count;
    pa->shape = shape;
    /* malloc(0)'s return value is implementation-defined (NULL is allowed) — xmalloc would
       misreport that as out-of-memory, so a zero-count packed array skips the call entirely; every
       later access is already rejected by the bounds check regardless. */
    pa->data  = count > 0 ? xmalloc((size_t)count * (size_t)element_size) : NULL;
    for (int64_t e = 0; e < count; e++) {
        unsigned char* elem = pa->data + (size_t)e * element_size;
        for (unsigned int f = 0; f < shape->field_count; f++)
            vm_packed_slot_write(elem + f * 8, shape->field_types[f], shape->field_defaults[f]);
    }
    vm->registers[dest_reg] = aer_packed_array_val(pa);
    gc_maybe_collect(vm);
    DISPATCH();
}

/* The fused `obj[index].field` read — see OP_INDEX_FIELD_GET's own comment, vm.h, for why this
   handles BOTH a packed array and an ordinary struct array in one opcode (the parser can't know
   which at compile time — functions are untyped, so a packed array flows through a parameter
   exactly like any other value). The non-packed branch reproduces vm_index_get_compute() +
   vm_resolve_field() exactly, just without needing a register for the intermediate value (no
   dest_reg is free to stash it in ahead of the final write, the way the old two-opcode sequence
   used one temp register for both steps). */
lbl_index_field_get: {
    unsigned int site = ip - 1;
    int dest_reg  = (int)UNPACK_INDEX_FIELD_GET_DEST(op_word);
    int obj_reg   = (int)UNPACK_INDEX_FIELD_GET_OBJ(op_word);
    int field_idx = (int)UNPACK_INDEX_FIELD_GET_FIELD(op_word);
    AerVal* idx = vm_rk_ptr20(vm, const_pool, UNPACK_INDEX_FIELD_GET_RK(op_word));
    AerVal obj = vm->registers[obj_reg];
    if (aer_type(obj) == TYPE_PACKED_ARRAY) {
        AerPackedArray* pa = aer_as_packed_array(obj);
        if (aer_type(*idx) != TYPE_INTEGER) { error("Array index must be an integer"); DISPATCH(); }
        int64_t i = aer_as_int(*idx);
        if (i < 0) i += (int64_t)pa->count;
        if (i < 0 || (uint64_t)i >= pa->count) {
            error("Array index %lld out of bounds (len %u)", aer_as_int(*idx), pa->count);
            DISPATCH();
        }
        int slot;
        if (!vm_resolve_field_by_shape(c, site, pa->shape, field_idx, &slot)) DISPATCH();
        unsigned int element_size = pa->shape->field_count * 8;
        unsigned char* elem = pa->data + (size_t)i * element_size + (size_t)slot * 8;
        vm->registers[dest_reg] = vm_packed_slot_read(elem, pa->shape->field_types[slot]);
        DISPATCH();
    }
    AerVal tmp;
    vm_index_get_compute(obj, *idx, &tmp);
    if (aer_type(obj) == TYPE_STRING) gc_maybe_collect(vm);   /* single-char string indexing allocates */
    if (aer_type(tmp) != TYPE_ARRAY || !aer_as_array(tmp)->shape) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerArray* oa = aer_as_array(tmp);
    int slot;
    if (!vm_resolve_field_by_shape(c, site, oa->shape, field_idx, &slot)) DISPATCH();
    vm->registers[dest_reg] = oa->items[slot];
    DISPATCH();
}

/* The fused `obj[index].field = value` / compound-assign write — mirror of lbl_index_field_get
   above, same dual dispatch, same reason it needs no scratch register for the non-packed
   intermediate (vm_resolve_field_by_shape works off the intermediate's own ->shape directly). */
lbl_index_field_set: {
    unsigned int site   = ip - 1;
    int obj_reg         = (int)UNPACK_INDEX_FIELD_SET_OBJ(op_word);
    int field_idx       = (int)UNPACK_INDEX_FIELD_SET_FIELD(op_word);
    AerVal* idx = vm_rk_ptr9(vm, const_pool, (uint32_t)UNPACK_INDEX_FIELD_SET_IDX(op_word));
    AerVal* val = vm_rk_ptr9(vm, const_pool, (uint32_t)UNPACK_INDEX_FIELD_SET_VAL(op_word));
    AerVal obj = vm->registers[obj_reg];
    if (aer_type(obj) == TYPE_PACKED_ARRAY) {
        AerPackedArray* pa = aer_as_packed_array(obj);
        if (aer_type(*idx) != TYPE_INTEGER) { error("Array index must be an integer"); DISPATCH(); }
        int64_t i = aer_as_int(*idx);
        if (i < 0) i += (int64_t)pa->count;
        if (i < 0 || (uint64_t)i >= pa->count) {
            error("Array index %lld out of bounds (len %u)", aer_as_int(*idx), pa->count);
            DISPATCH();
        }
        int slot;
        if (!vm_resolve_field_by_shape(c, site, pa->shape, field_idx, &slot)) DISPATCH();
        ValueType declared = pa->shape->field_types[slot];
        if (val->tag != declared) {
            error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
                  aer_as_string(c->pool[field_idx])->data);
            DISPATCH();
        }
        unsigned int element_size = pa->shape->field_count * 8;
        unsigned char* elem = pa->data + (size_t)i * element_size + (size_t)slot * 8;
        vm_packed_slot_write(elem, declared, *val);
        DISPATCH();
    }
    AerVal tmp;
    vm_index_get_compute(obj, *idx, &tmp);
    if (aer_type(obj) == TYPE_STRING) gc_maybe_collect(vm);   /* single-char string indexing allocates */
    if (aer_type(tmp) != TYPE_ARRAY || !aer_as_array(tmp)->shape) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerArray* oa = aer_as_array(tmp);
    int slot;
    if (!vm_resolve_field_by_shape(c, site, oa->shape, field_idx, &slot)) DISPATCH();
    ValueType declared = oa->shape->field_types[slot];
    if (declared != TYPE_ANY && val->tag != declared) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    gc_barrier_array(oa, *val);
    oa->items[slot] = *val;
    DISPATCH();
}

/* One handler for negate/not/bitwise-not, keyed by unary_op — same "reuse the Opcode value as an
   operand tag" convention lbl_binary uses. Also folds in OP_TO_STR (string interpolation's
   "{name}" -> string conversion), calling the existing vm_to_str() helper (shared with
   print()/lbl_to_str above) rather than reimplementing value formatting. */
lbl_unary: {
    int dest        = (int)UNPACK_UNARY_DEST(op_word);
    Opcode unary_op = (Opcode)UNPACK_UNARY_OP(op_word);
    AerVal v = *vm_rk_ptr9(vm, const_pool, UNPACK_UNARY_RK(op_word));
    AerVal* result = &vm->registers[dest];
    switch (unary_op) {
        case OP_NEGATE:
            if      (aer_type(v) == TYPE_INTEGER) *result = aer_int(-aer_as_int(v));
            else if (aer_type(v) == TYPE_REAL)     *result = aer_real(-aer_as_real(v));
            else { error("Negation requires a numeric type"); *result = aer_null(); }
            break;
        case OP_NOT:
            *result = aer_bool(!vm_truthy(v));
            break;
        case OP_BITWISE_NOT:
            if (aer_type(v) != TYPE_INTEGER) { error("Bitwise NOT requires an integer"); *result = aer_null(); }
            else *result = aer_int(~aer_as_int(v));
            break;
        case OP_TO_STR:
            /* The only case here that can ever allocate (vm_to_str/aer_make_string) — NEGATE/NOT/
               BITWISE_NOT only ever produce aer_int/aer_real/aer_bool, plain tagged-union
               constructions (value.h) with no heap involvement at all under the current AerVal
               representation, so gc_maybe_collect is scoped to just this case, not the whole
               opcode. */
            *result = vm_to_str(vm, v);
            gc_maybe_collect(vm);
            break;
        default:
            *result = aer_null();
            break;
    }
    DISPATCH();
}

/* `x as integer/float/boolean` — see vm_cast() above. No gc_maybe_collect: every cast_type only
   ever produces aer_null/aer_int/aer_real/aer_bool (confirmed by direct inspection of vm_cast's
   body, including its string-parsing sub-cases, which only ever populate a local stack buffer),
   never heap allocation. */
lbl_cast: {
    int dest      = (int)UNPACK_CAST_DEST(op_word);
    int cast_type = (int)UNPACK_CAST_TYPE(op_word);
    AerVal v = *vm_rk_ptr9(vm, const_pool, UNPACK_CAST_RK(op_word));
    vm->registers[dest] = vm_cast(v, cast_type);
    DISPATCH();
}

/* "Primitive pass" raw-arithmetic handlers (vm.h's OP_RAW_LOAD_INT comment, CallFrame.raw_ints/
   raw_reals) — skip both vm_rk_ptr9's RK-flag check and the usual tag check entirely, since the
   parser has already proven, at compile time, that every operand here is the stated type. None of
   these call gc_maybe_collect(): integers/reals/booleans never have heap cells (see
   value_is_young's default case, above), so nothing in this whole family — arithmetic, comparison,
   or boxing — ever allocates pool memory, unlike the boxed BINARY_OP_INT_REAL family (whose
   gc_maybe_collect call is a general periodic safepoint, not a response to that specific op
   allocating). */
lbl_raw_load_int: {
    int dest = (int)UNPACK_RAW_LOAD_INT_DEST(op_word);
    vm->raw_ints[dest] = UNPACK_RAW_LOAD_INT_IMM(op_word);
    DISPATCH();
}

lbl_raw_load_real: {
    int dest = (int)UNPACK_RAW_LOAD_REAL_DEST(op_word);
    unsigned int pool_idx = UNPACK_RAW_LOAD_REAL_POOL(op_word);
    vm->raw_reals[dest] = const_pool[pool_idx].as.d;
    DISPATCH();
}

lbl_raw_add_int: {
    int dest = (int)UNPACK_RAW_ARITH_RR_DEST(op_word);
    int a    = (int)UNPACK_RAW_ARITH_RR_A(op_word);
    int b    = (int)UNPACK_RAW_ARITH_RR_B(op_word);
    vm->raw_ints[dest] = vm->raw_ints[a] + vm->raw_ints[b];
    DISPATCH();
}

lbl_raw_sub_int: {
    int dest = (int)UNPACK_RAW_ARITH_RR_DEST(op_word);
    int a    = (int)UNPACK_RAW_ARITH_RR_A(op_word);
    int b    = (int)UNPACK_RAW_ARITH_RR_B(op_word);
    vm->raw_ints[dest] = vm->raw_ints[a] - vm->raw_ints[b];
    DISPATCH();
}

lbl_raw_mul_int: {
    int dest = (int)UNPACK_RAW_ARITH_RR_DEST(op_word);
    int a    = (int)UNPACK_RAW_ARITH_RR_A(op_word);
    int b    = (int)UNPACK_RAW_ARITH_RR_B(op_word);
    vm->raw_ints[dest] = vm->raw_ints[a] * vm->raw_ints[b];
    DISPATCH();
}

/* Matches OP_DIV's own boxed semantics exactly: integer division always promotes to a real result
   — AER has no truncating "/" (see BINARY_OP_INT_REAL(div, ...) above, which produces aer_real()
   even for two integer operands). So, uniquely among the OP_RAW_*_INT family, this opcode's dest
   is a raw_reals[] slot, not raw_ints[] — the parser's var_kind classification must know this:
   `x = a / b` for raw-int a/b makes x RAW_REAL, never RAW_INT. */
lbl_raw_div_int: {
    int dest = (int)UNPACK_RAW_ARITH_RR_DEST(op_word);
    int a    = (int)UNPACK_RAW_ARITH_RR_A(op_word);
    int b    = (int)UNPACK_RAW_ARITH_RR_B(op_word);
    int64_t rv = vm->raw_ints[b];
    if (rv == 0) { error("Division by zero"); vm->raw_reals[dest] = 0.0; }
    else vm->raw_reals[dest] = (double)vm->raw_ints[a] / (double)rv;
    DISPATCH();
}

lbl_raw_mod_int: {
    int dest = (int)UNPACK_RAW_ARITH_RR_DEST(op_word);
    int a    = (int)UNPACK_RAW_ARITH_RR_A(op_word);
    int b    = (int)UNPACK_RAW_ARITH_RR_B(op_word);
    int64_t rv = vm->raw_ints[b];
    if (rv == 0) { error("Modulo by zero"); vm->raw_ints[dest] = 0; }
    else vm->raw_ints[dest] = vm->raw_ints[a] % rv;
    DISPATCH();
}

lbl_raw_floor_div_int: {
    int dest = (int)UNPACK_RAW_ARITH_RR_DEST(op_word);
    int a    = (int)UNPACK_RAW_ARITH_RR_A(op_word);
    int b    = (int)UNPACK_RAW_ARITH_RR_B(op_word);
    int64_t rv = vm->raw_ints[b];
    if (rv == 0) { error("Division by zero"); vm->raw_ints[dest] = 0; }
    else vm->raw_ints[dest] = (int64_t)floor((double)vm->raw_ints[a] / (double)rv);
    DISPATCH();
}

lbl_raw_add_real: {
    int dest = (int)UNPACK_RAW_ARITH_RR_DEST(op_word);
    int a    = (int)UNPACK_RAW_ARITH_RR_A(op_word);
    int b    = (int)UNPACK_RAW_ARITH_RR_B(op_word);
    vm->raw_reals[dest] = vm->raw_reals[a] + vm->raw_reals[b];
    DISPATCH();
}

lbl_raw_sub_real: {
    int dest = (int)UNPACK_RAW_ARITH_RR_DEST(op_word);
    int a    = (int)UNPACK_RAW_ARITH_RR_A(op_word);
    int b    = (int)UNPACK_RAW_ARITH_RR_B(op_word);
    vm->raw_reals[dest] = vm->raw_reals[a] - vm->raw_reals[b];
    DISPATCH();
}

lbl_raw_mul_real: {
    int dest = (int)UNPACK_RAW_ARITH_RR_DEST(op_word);
    int a    = (int)UNPACK_RAW_ARITH_RR_A(op_word);
    int b    = (int)UNPACK_RAW_ARITH_RR_B(op_word);
    vm->raw_reals[dest] = vm->raw_reals[a] * vm->raw_reals[b];
    DISPATCH();
}

lbl_raw_div_real: {
    int dest = (int)UNPACK_RAW_ARITH_RR_DEST(op_word);
    int a    = (int)UNPACK_RAW_ARITH_RR_A(op_word);
    int b    = (int)UNPACK_RAW_ARITH_RR_B(op_word);
    double rv = vm->raw_reals[b];
    if (rv == 0.0) { error("Division by zero"); vm->raw_reals[dest] = 0.0; }
    else vm->raw_reals[dest] = vm->raw_reals[a] / rv;
    DISPATCH();
}

/* Comparisons produce an ordinary BOXED boolean, in a normal registers[] slot (dest here is 7
   bits, not 5 — see PACK_RAW_CMP's own comment, vm.h) — they feed a branch, not further raw
   arithmetic, so there's no raw boolean type to keep this result in. */
lbl_raw_lt_int: {
    int dest = (int)UNPACK_RAW_CMP_DEST(op_word);
    int a    = (int)UNPACK_RAW_CMP_A(op_word);
    int b    = (int)UNPACK_RAW_CMP_B(op_word);
    vm->registers[dest] = aer_bool(vm->raw_ints[a] < vm->raw_ints[b]);
    DISPATCH();
}

lbl_raw_gt_int: {
    int dest = (int)UNPACK_RAW_CMP_DEST(op_word);
    int a    = (int)UNPACK_RAW_CMP_A(op_word);
    int b    = (int)UNPACK_RAW_CMP_B(op_word);
    vm->registers[dest] = aer_bool(vm->raw_ints[a] > vm->raw_ints[b]);
    DISPATCH();
}

lbl_raw_lte_int: {
    int dest = (int)UNPACK_RAW_CMP_DEST(op_word);
    int a    = (int)UNPACK_RAW_CMP_A(op_word);
    int b    = (int)UNPACK_RAW_CMP_B(op_word);
    vm->registers[dest] = aer_bool(vm->raw_ints[a] <= vm->raw_ints[b]);
    DISPATCH();
}

lbl_raw_gte_int: {
    int dest = (int)UNPACK_RAW_CMP_DEST(op_word);
    int a    = (int)UNPACK_RAW_CMP_A(op_word);
    int b    = (int)UNPACK_RAW_CMP_B(op_word);
    vm->registers[dest] = aer_bool(vm->raw_ints[a] >= vm->raw_ints[b]);
    DISPATCH();
}

lbl_raw_lt_real: {
    int dest = (int)UNPACK_RAW_CMP_DEST(op_word);
    int a    = (int)UNPACK_RAW_CMP_A(op_word);
    int b    = (int)UNPACK_RAW_CMP_B(op_word);
    vm->registers[dest] = aer_bool(vm->raw_reals[a] < vm->raw_reals[b]);
    DISPATCH();
}

lbl_raw_gt_real: {
    int dest = (int)UNPACK_RAW_CMP_DEST(op_word);
    int a    = (int)UNPACK_RAW_CMP_A(op_word);
    int b    = (int)UNPACK_RAW_CMP_B(op_word);
    vm->registers[dest] = aer_bool(vm->raw_reals[a] > vm->raw_reals[b]);
    DISPATCH();
}

lbl_raw_lte_real: {
    int dest = (int)UNPACK_RAW_CMP_DEST(op_word);
    int a    = (int)UNPACK_RAW_CMP_A(op_word);
    int b    = (int)UNPACK_RAW_CMP_B(op_word);
    vm->registers[dest] = aer_bool(vm->raw_reals[a] <= vm->raw_reals[b]);
    DISPATCH();
}

lbl_raw_gte_real: {
    int dest = (int)UNPACK_RAW_CMP_DEST(op_word);
    int a    = (int)UNPACK_RAW_CMP_A(op_word);
    int b    = (int)UNPACK_RAW_CMP_B(op_word);
    vm->registers[dest] = aer_bool(vm->raw_reals[a] >= vm->raw_reals[b]);
    DISPATCH();
}

/* The only bridge from raw storage back to a normal tagged AerVal register — see PACK_BOX's own
   comment, vm.h. */
lbl_box_int: {
    int dest = (int)UNPACK_BOX_DEST(op_word);
    int src  = (int)UNPACK_BOX_SRC(op_word);
    vm->registers[dest] = aer_int(vm->raw_ints[src]);
    DISPATCH();
}

lbl_box_real: {
    int dest = (int)UNPACK_BOX_DEST(op_word);
    int src  = (int)UNPACK_BOX_SRC(op_word);
    vm->registers[dest] = aer_real(vm->raw_reals[src]);
    DISPATCH();
}

lbl_raw_move_int: {
    int dest = (int)UNPACK_RAW_MOVE_DEST(op_word);
    int src  = (int)UNPACK_RAW_MOVE_SRC(op_word);
    vm->raw_ints[dest] = vm->raw_ints[src];
    DISPATCH();
}

lbl_raw_move_real: {
    int dest = (int)UNPACK_RAW_MOVE_DEST(op_word);
    int src  = (int)UNPACK_RAW_MOVE_SRC(op_word);
    vm->raw_reals[dest] = vm->raw_reals[src];
    DISPATCH();
}

/* Compound-assign accumulation of a boxed value into a raw slot, in place — see the opcode's own
   comment in vm.h. A runtime tag check decides: matching type accumulates directly into the raw
   slot (no shadow, no allocation — safe to repeat every loop iteration since the slot's identity
   never changes); mismatched type is the same "Type mismatch in binary expression" runtime error
   vm_binary_cold already gives for this, not a crash or silent corruption. */
lbl_raw_add_int_boxed: {
    int slot = (int)UNPACK_RAW_ARITH_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_ARITH_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag != TYPE_INTEGER) { error("Type mismatch in binary expression"); DISPATCH(); }
    vm->raw_ints[slot] += rhs->as.i;
    DISPATCH();
}

lbl_raw_sub_int_boxed: {
    int slot = (int)UNPACK_RAW_ARITH_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_ARITH_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag != TYPE_INTEGER) { error("Type mismatch in binary expression"); DISPATCH(); }
    vm->raw_ints[slot] -= rhs->as.i;
    DISPATCH();
}

lbl_raw_mul_int_boxed: {
    int slot = (int)UNPACK_RAW_ARITH_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_ARITH_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag != TYPE_INTEGER) { error("Type mismatch in binary expression"); DISPATCH(); }
    vm->raw_ints[slot] *= rhs->as.i;
    DISPATCH();
}

lbl_raw_add_real_boxed: {
    int slot = (int)UNPACK_RAW_ARITH_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_ARITH_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag != TYPE_REAL) { error("Type mismatch in binary expression"); DISPATCH(); }
    vm->raw_reals[slot] += rhs->as.d;
    DISPATCH();
}

lbl_raw_sub_real_boxed: {
    int slot = (int)UNPACK_RAW_ARITH_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_ARITH_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag != TYPE_REAL) { error("Type mismatch in binary expression"); DISPATCH(); }
    vm->raw_reals[slot] -= rhs->as.d;
    DISPATCH();
}

lbl_raw_mul_real_boxed: {
    int slot = (int)UNPACK_RAW_ARITH_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_ARITH_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag != TYPE_REAL) { error("Type mismatch in binary expression"); DISPATCH(); }
    vm->raw_reals[slot] *= rhs->as.d;
    DISPATCH();
}

lbl_raw_load_int_pool: {
    int dest = (int)UNPACK_RAW_LOAD_INT_POOL_DEST(op_word);
    unsigned int pool_idx = UNPACK_RAW_LOAD_INT_POOL_POOL(op_word);
    vm->raw_ints[dest] = const_pool[pool_idx].as.i;
    DISPATCH();
}

/* rhs may legitimately be TYPE_REAL even though the raw slot is int (e.g. `time.now() > 1700000000`
   — the literal is raw-composable int, time.now()'s boxed result is real) — the ordinary boxed
   comparison this replaces silently promotes int<->real for exactly this reason (vm_binary_fast's
   own int/real promotion), so a hard type-check here would be a real regression, not just a missed
   optimization. Only a genuinely non-numeric boxed type still errors. */
lbl_raw_lt_int_boxed: {
    int dest = (int)UNPACK_RAW_CMP_BOXED_DEST(op_word);
    int slot = (int)UNPACK_RAW_CMP_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_CMP_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_ints[slot] < rhs->as.i);
    else if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool((double)vm->raw_ints[slot] < rhs->as.d);
    else error("Type mismatch in binary expression");
    DISPATCH();
}

lbl_raw_gt_int_boxed: {
    int dest = (int)UNPACK_RAW_CMP_BOXED_DEST(op_word);
    int slot = (int)UNPACK_RAW_CMP_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_CMP_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_ints[slot] > rhs->as.i);
    else if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool((double)vm->raw_ints[slot] > rhs->as.d);
    else error("Type mismatch in binary expression");
    DISPATCH();
}

lbl_raw_lte_int_boxed: {
    int dest = (int)UNPACK_RAW_CMP_BOXED_DEST(op_word);
    int slot = (int)UNPACK_RAW_CMP_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_CMP_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_ints[slot] <= rhs->as.i);
    else if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool((double)vm->raw_ints[slot] <= rhs->as.d);
    else error("Type mismatch in binary expression");
    DISPATCH();
}

lbl_raw_gte_int_boxed: {
    int dest = (int)UNPACK_RAW_CMP_BOXED_DEST(op_word);
    int slot = (int)UNPACK_RAW_CMP_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_CMP_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_ints[slot] >= rhs->as.i);
    else if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool((double)vm->raw_ints[slot] >= rhs->as.d);
    else error("Type mismatch in binary expression");
    DISPATCH();
}

lbl_raw_lt_real_boxed: {
    int dest = (int)UNPACK_RAW_CMP_BOXED_DEST(op_word);
    int slot = (int)UNPACK_RAW_CMP_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_CMP_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool(vm->raw_reals[slot] < rhs->as.d);
    else if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_reals[slot] < (double)rhs->as.i);
    else error("Type mismatch in binary expression");
    DISPATCH();
}

lbl_raw_gt_real_boxed: {
    int dest = (int)UNPACK_RAW_CMP_BOXED_DEST(op_word);
    int slot = (int)UNPACK_RAW_CMP_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_CMP_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool(vm->raw_reals[slot] > rhs->as.d);
    else if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_reals[slot] > (double)rhs->as.i);
    else error("Type mismatch in binary expression");
    DISPATCH();
}

lbl_raw_lte_real_boxed: {
    int dest = (int)UNPACK_RAW_CMP_BOXED_DEST(op_word);
    int slot = (int)UNPACK_RAW_CMP_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_CMP_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool(vm->raw_reals[slot] <= rhs->as.d);
    else if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_reals[slot] <= (double)rhs->as.i);
    else error("Type mismatch in binary expression");
    DISPATCH();
}

lbl_raw_gte_real_boxed: {
    int dest = (int)UNPACK_RAW_CMP_BOXED_DEST(op_word);
    int slot = (int)UNPACK_RAW_CMP_BOXED_SLOT(op_word);
    int reg  = (int)UNPACK_RAW_CMP_BOXED_REG(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool(vm->raw_reals[slot] >= rhs->as.d);
    else if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_reals[slot] >= (double)rhs->as.i);
    else error("Type mismatch in binary expression");
    DISPATCH();
}

lbl_halt:
    runtime_error_unwind_target = saved_unwind_target;
    active_vm_for_errors        = saved_active_vm;
    return true;
}

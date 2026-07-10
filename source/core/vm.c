#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer_host.h"
#include "aer_json.h"
#include "aer_module.h"
#include "aer_stdlib.h"
#include "error.h"
#include "pool.h"
#include "vm.h"
#include "value_box.h"

/* Slab pools for heap types confirmed (via every free() site) to never be freed individually — alloc-speed only. Guarded since vm_init() reruns per VM/module import and would otherwise leak slabs. */
/* long_pool: heap fallback for a TYPE_INTEGER outside AerVal's 47-bit inline range (value_box.h's AER_BIGFLAG_BIT); one boxed value is immutable, so no write barrier needed. */
static Pool string_pool, array_pool, dict_pool, function_pool, long_pool, struct_pool;
static bool pools_initialized = false;

/* struct_pool holds struct instances (AerArray with shape != NULL) as ONE allocation instead of
   the usual header-plus-separate-items-buffer two-allocation layout ordinary arrays use — see the
   struct-construction sites below for why this is sound (a struct's field count never changes
   after construction, so there's no reallocation to support, unlike a plain array's items[]).
   Sized for MAX_STRUCT_FIELDS (the worst case) since Pool requires uniform cell size — wastes some
   space for shapes with fewer fields, a bounded, deliberate trade for collapsing two dependent
   pointer dereferences (header, then its separately-allocated items[]) into one on every struct
   field access. */
#ifdef AER_V3
/* v3 register-VM prototype — see OP_V3_*'s comment in vm.h. Entirely separate from
   vm->stack/vm->scopes so the prototype cannot interact with or destabilize the existing
   stack-based interpreter no matter what it's given to run.

   M5 — real per-call register windowing, superseding M3's single fixed callee bank
   (`V3_CALLEE_FRAME_BASE`, deleted) and its two bare globals (`v3_return_ip`/`v3_call_dest_reg`,
   also deleted): those supported exactly one call level, since a second OP_V3_CALL before the
   first returned would clobber them and corrupt the outer call's return. Now every active call
   gets its own isolated V3_FRAME_REGISTERS-sized bank, mirroring vm->call_stack[VM_CALL_MAX]'s
   existing role for the stack VM — nested/recursive calls can no longer clobber each other.
   (V3_FRAME_REGISTERS itself now lives in vm.h, not here — parser_v3.c's real-source variable
   table needs to see it too.) */

/* M5 slice 12 — a `defer name(args)` statement. Unlike the stack VM's DeferredCall (whose
   name_idx re-resolves via scope lookup at replay time, since functions there are first-class
   values), v3 resolves the target at COMPILE time via v3_func_lookup — consistent with v3 already
   resolving every other call site's target at compile time (functions aren't first-class values
   in this prototype at all). `args` are snapshotted at the defer statement, same as the stack
   VM's version — a deferred call's arguments are evaluated once, now, not re-evaluated at replay
   time. */
typedef struct {
    unsigned int callee_offset;
    AerVal       args[MAX_DEFER_ARGS];
    int          arg_count;
} V3DeferredCall;

typedef struct {
    AerVal       registers[V3_FRAME_REGISTERS];
    unsigned int return_ip;   /* where to resume in the CALLER */
    int          dest_reg;    /* which of the CALLER's registers gets the return value */

    /* Pending `defer` calls, drained LIFO by lbl_v3_return before the frame unwinds — mirrors
       CallFrame's own defers/defer_count exactly, including the lazy-xmalloc-once-and-keep
       rationale (defer is rare, V3_FRAME_REGISTERS-sized frames are not). Unlike CallFrame, no
       pending_return_value/defers_draining bookkeeping is needed: each v3 call gets its own
       isolated register bank (V3CallFrame.registers), so a frame's real return value just sits in
       its own src_reg untouched while nested deferred calls run in deeper, separate frames — there
       is no shared mutable stack slot for a deferred call's own result to collide with. */
    V3DeferredCall* defers;
    int             defer_count;
} V3CallFrame;

static V3CallFrame v3_call_stack[VM_CALL_MAX];   /* reuses the stack VM's own recursion ceiling */
static int         v3_call_depth = 0;            /* 0 = the top-level/main frame */

/* Every M1-M4 opcode handler indexes this exactly as before (`v3_registers[dest]`, etc.) — only
   its type changed, from a fixed array to a pointer repointed at the active frame's bank on every
   call/return (lbl_v3_call/lbl_v3_return, below). This is what lets every opcode handler EXCEPT
   those two stay completely unchanged: they transparently operate on whichever frame is currently
   executing without needing to know call depth exists. */
static AerVal* v3_registers = v3_call_stack[0].registers;

/* Test-only accessor (source/compiler/parser_v3.c's hand-driven compiler and tests/v3_smoke_test.c
   are the only intended callers) — reads back a v3 register's final value after a v3-only chunk
   has run to OP_HALT. Reads whichever frame is currently active, which is frame 0 (the top level)
   once a chunk has run to completion with every call balanced by a return. */
AerVal v3_register_get(int slot) {
    return v3_registers[slot];
}

/* Decodes an OP_V3_BINARY RK operand — see V3_RK_CONST_FLAG's comment in vm.h. */
static inline AerVal vm_v3_rk_value(Chunk* c, int rk) {
    if (rk & V3_RK_CONST_FLAG) return c->pool[rk & ~V3_RK_CONST_FLAG];
    return v3_registers[rk];
}
#endif

static void vm_pools_init_once(void) {
    if (pools_initialized) return;
    pool_init(&string_pool,   sizeof(AerString),   256);
    pool_init(&array_pool,    sizeof(AerArray),    256);
    pool_init(&dict_pool,     sizeof(AerDict),      64);
    pool_init(&function_pool, sizeof(AerFunction),  64);
    pool_init(&long_pool,     sizeof(long long),    64);
    pool_init(&struct_pool,   sizeof(AerArray) + MAX_STRUCT_FIELDS * sizeof(AerVal), 64);
    pools_initialized = true;
}

/* AerVal's factory for TYPE_INTEGER (defined here, not value_box.h, since it needs long_pool): in-range values encode inline, out-of-range values get one long_pool cell. */
AerVal aer_int(long long n) {
    if (n >= AER_INT47_MIN && n <= AER_INT47_MAX) {
        AerVal v;
        v.bits = AER_BOXED_TEST | ((uint64_t)TYPE_INTEGER << AER_TAG_SHIFT) |
                 ((uint64_t)n & AER_INT47_MASK);
        return v;
    }
    long long* box = pool_alloc(&long_pool);
    *box = n;
    AerVal v;
    v.bits = AER_BOXED_TEST | ((uint64_t)TYPE_INTEGER << AER_TAG_SHIFT) |
             AER_BIGFLAG_BIT | ((uint64_t)(uintptr_t)box & AER_INT47_MASK);
    return v;
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
        case TYPE_INTEGER:  return aer_int_is_boxed(v) && pool_is_young(&long_pool, aer_int_box_ptr(v));
        default:            return false;
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

/* Write barrier for array item writes (index-assign, append, struct field-set via lbl_field_set —
   a struct is an array with a shape, now backed by struct_pool instead of array_pool — see
   vm_pools_init_once). Branches on a->shape to pick the right pool/remembered-kind pair, mirroring
   gc_barrier_dict's single-pool pattern but for whichever of the two pools actually owns `a`. */
static void gc_barrier_array(AerArray* a, AerVal new_value) {
    Pool* p = a->shape ? &struct_pool : &array_pool;
    if (pool_is_young(p, a)) return;   /* young containers are already
                                          re-traced normally next cycle */
    if (!value_is_young(new_value)) return;
    gc_remember(a, a->shape ? REMEMBERED_STRUCT : REMEMBERED_ARRAY);
}

/* Write barrier for dict entry writes (both the update-in-place and
   new-entry paths in lbl_index_set). `d` is always dict_pool-tracked. */
static void gc_barrier_dict(AerDict* d, AerVal new_value) {
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

/* Shared by TYPE_FUNCTION marking and CallFrame root marking (a frame's executing function is a raw AerFunction*, not a wrapped Value). */
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
                DictMap* map = &aer_as_dict(v)->map;
                for (unsigned int i = 0; i < map->capacity; i++)
                    if (map->buckets[i].key)
                        worklist_push(map->buckets[i].payload.inline_val);
                /* Bucket keys are plain dictmap-owned char*, not Values — nothing to push. */
            }
            break;
        case TYPE_FUNCTION:
            mark_function(aer_as_function(v));
            break;
        case TYPE_INTEGER:
            /* Only an out-of-range integer has a heap cell (AER_BIGFLAG_BIT); a leaf like TYPE_STRING. */
            if (aer_int_is_boxed(v)) pool_mark(&long_pool, aer_int_box_ptr(v));
            break;
        default:
            break;   /* null/boolean/real (and an inline integer) reference no heap cell */
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

#ifdef AER_V3
    /* M4 — v3 registers can hold heap references (arrays/dicts), unlike M1-M3's scalars-only
       registers. Scanned unconditionally, same as vm->stack above, not tracked for liveness: an
       idle register is 0-bits, which decodes as TYPE_REAL 0.0 under NaN-boxing (value_box.h) and
       is a harmless no-op leaf in mark_value's default case, so there's no cost to scanning past
       whatever's actually live.
       M5 — reshaped from a single flat array to every frame's bank (v3_call_stack[VM_CALL_MAX],
       V3_FRAME_REGISTERS each). Originally blanket-scanned over all VM_CALL_MAX frames regardless
       of v3_call_depth on the theory that over-scanning dead frames is a harmless no-op; measured
       on real hardware (Pi, nbody.aer) this was NOT free — it was the dominant source of cache
       misses in the whole VM (10x the stack VM's), because a GC pass that runs at recursion depth
       ~2-3 was still walking all 64 frames' worth of cold, mostly-zeroed memory every time. Frames
       beyond v3_call_depth are dead (already returned, defers already drained by lbl_v3_return
       before unwind), so bounding the scan to the live call chain can't under-collect — it only
       stops retaining garbage from frames nothing can reach anymore. */
    for (int f = 0; f <= v3_call_depth; f++)
        for (int i = 0; i < V3_FRAME_REGISTERS; i++)
            worklist_push(v3_call_stack[f].registers[i]);

    /* M5 slice 12 — a deferred call's snapshotted args live outside v3_registers[], in each frame's
       own defers[] side array (mirroring CallFrame's identical defer-args root below), so they need
       their own scan; bounded to the live call chain for the same reason as the registers loop
       just above. */
    for (int f = 0; f <= v3_call_depth; f++) {
        V3CallFrame* frame = &v3_call_stack[f];
        for (int d = 0; d < frame->defer_count; d++)
            for (int a = 0; a < frame->defers[d].arg_count; a++)
                worklist_push(frame->defers[d].args[a]);
    }
#endif

    for (int s = 0; s < vm->scope_depth; s++) {
        AerScope* scope = &vm->scopes[s];
        for (int i = 0; i < scope->count; i++)
            worklist_push(scope->slots[i].val);
        if (scope->overflow) {
            HashMap* map = &scope->map;
            for (unsigned int i = 0; i < map->capacity; i++)
                if (map->buckets[i].key)
                    worklist_push(*(AerVal*)map->buckets[i].payload.boxed);
        }
    }

    for (int f = 0; f < vm->call_depth; f++) {
        CallFrame* frame = &vm->call_stack[f];
        if (frame->function) mark_function(frame->function);
        for (int d = 0; d < frame->defer_count; d++)
            for (int a = 0; a < frame->defers[d].arg_count; a++)
                worklist_push(frame->defers[d].args[a]);
        if (frame->defers_draining) worklist_push(frame->pending_return_value);
    }
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
static void free_dict(void* cell)     { dictmap_free(&((AerDict*)cell)->map); }   /* already frees every entry's key */
static void free_function(void* cell) { (void)cell; }   /* nothing to free — no closure upvalues array anymore */
static void free_long(void* cell)     { (void)cell; }   /* a bare long long — no heap references, immutable once created */
static void free_struct(void* cell)   { (void)cell; }   /* items lives inline in this same cell — nothing separate to free */

/* ------------------------------------------------------------------ */
/* Generational GC — collection                                        */
/* ------------------------------------------------------------------ */

static void gc_collect(VM* vm, bool minor) {
    /* Must run before marking every cycle: a minor sweep never visits old cells, so without this an old cell's mark bit would stay set forever and never be re-traced. */
    pool_clear_marks(&string_pool);
    pool_clear_marks(&array_pool);
    pool_clear_marks(&dict_pool);
    pool_clear_marks(&function_pool);
    pool_clear_marks(&long_pool);
    pool_clear_marks(&struct_pool);

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
                    DictMap* map = &((AerDict*)e->ptr)->map;
                    for (unsigned int j = 0; j < map->capacity; j++)
                        if (map->buckets[j].key) worklist_push(map->buckets[j].payload.inline_val);
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
    pool_sweep(&long_pool,     minor, free_long);
    pool_sweep(&struct_pool,   minor, free_struct);
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

/* Shared by aer_gc_stats and gc_maybe_collect's ceiling check — one place walking all pools' cell_state, not two. */
static unsigned int gc_count_live_cells(void) {
    unsigned int total = 0;
    Pool* pools[] = { &string_pool, &array_pool, &dict_pool, &function_pool, &long_pool, &struct_pool };
    for (unsigned int p = 0; p < 6; p++) {
        Pool* pool = pools[p];
        for (unsigned int i = 0; i < pool->slab_count; i++) {
            unsigned int count = (i == pool->slab_count - 1) ? pool->next_index : pool->elems_per_slab;
            for (unsigned int j = 0; j < count; j++)
                if (!(pool->cell_state[i][j] & POOL_FREE)) total++;
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
   allocation the pool system doesn't track at all. Walks each pool's cell_state the same way
   gc_count_live_cells does, but reads each live cell's own size fields instead of just counting. */
void aer_debug_memory_report(FILE* out) {
    fprintf(out, "\n--- memory ---\n");

    unsigned long long str_hdr = 0, str_payload = 0;
    {
        Pool* p = &string_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                if (p->cell_state[i][j] & POOL_FREE) continue;
                AerString* s = (AerString*)(p->slabs[i] + (size_t)j * p->stride);
                str_hdr += sizeof(AerString);
                str_payload += s->length;
            }
        }
    }
    fprintf(out, "  string   header %10llu B  payload %10llu B\n", str_hdr, str_payload);

    unsigned long long arr_hdr = 0, arr_payload = 0;
    {
        Pool* p = &array_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                if (p->cell_state[i][j] & POOL_FREE) continue;
                AerArray* a = (AerArray*)(p->slabs[i] + (size_t)j * p->stride);
                arr_hdr += sizeof(AerArray);
                arr_payload += (unsigned long long)a->capacity * sizeof(AerVal);
            }
        }
    }
    fprintf(out, "  array    header %10llu B  payload %10llu B\n", arr_hdr, arr_payload);

    unsigned long long dict_hdr = 0, dict_payload = 0;
    {
        Pool* p = &dict_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                if (p->cell_state[i][j] & POOL_FREE) continue;
                AerDict* d = (AerDict*)(p->slabs[i] + (size_t)j * p->stride);
                dict_hdr += sizeof(AerDict);
                dict_payload += (unsigned long long)d->map.capacity * sizeof(HashTableEntry);
                for (unsigned int b = 0; b < d->map.capacity; b++)
                    if (d->map.buckets[b].key) dict_payload += d->map.buckets[b].length + 1;
            }
        }
    }
    fprintf(out, "  dict     header %10llu B  payload %10llu B\n", dict_hdr, dict_payload);

    unsigned long long fn_hdr = 0, fn_payload = 0;
    {
        Pool* p = &function_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                if (p->cell_state[i][j] & POOL_FREE) continue;
                AerFunction* f = (AerFunction*)(p->slabs[i] + (size_t)j * p->stride);
                fn_hdr += sizeof(AerFunction);
                if (f->defaults) fn_payload += (unsigned long long)(f->arity - f->min_arity) * sizeof(AerVal);
            }
        }
    }
    fprintf(out, "  function header %10llu B  payload %10llu B\n", fn_hdr, fn_payload);

    unsigned long long long_hdr = (unsigned long long)long_pool.slab_count * long_pool.elems_per_slab * long_pool.stride;
    fprintf(out, "  long     reserved %9llu B (no separate payload)\n", long_hdr);

    /* struct_pool cells are fixed-size (sizeof(AerArray) + MAX_STRUCT_FIELDS*sizeof(AerVal)) —
       "header" here is the fixed per-cell reservation, "payload" is the sum of each live
       instance's ACTUAL field_count*sizeof(AerVal), so the gap between the two is exactly the
       over-provisioning cost the MAX_STRUCT_FIELDS-sized single pool trades for one allocation
       instead of two — see vm_pools_init_once. */
    unsigned long long struct_hdr = 0, struct_payload = 0;
    {
        Pool* p = &struct_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                if (p->cell_state[i][j] & POOL_FREE) continue;
                struct_hdr += p->stride;
                AerArray* a = (AerArray*)(p->slabs[i] + (size_t)j * p->stride);
                struct_payload += (unsigned long long)a->count * sizeof(AerVal);
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
    hashmap_free(&c->name_index);
    free(c->line_mark_offsets);
    free(c->line_mark_lines);
    for (unsigned int i = 0; i < c->import_count; i++) free(c->imported_modules[i]);
    free(c->imported_modules);
    /* Not each entry — every populated slot is a pointer INTO vm->scopes, never separately owned. */
    free(c->addr_cache);
    /* Not each entry — every populated slot is a Shape* owned by c->shapes, never separately owned. */
    free(c->field_cache_shape);
    free(c->field_cache_slot);
#ifdef AER_DEBUG_TOOLS
    free(c->debug_hits);
#endif
    memset(c, 0, sizeof(*c));
}

unsigned int chunk_add_addr_cache(Chunk* c) {
    if (c->addr_cache_count >= c->addr_cache_cap) {
        c->addr_cache_cap = c->addr_cache_cap ? c->addr_cache_cap * 2 : 32;
        c->addr_cache = xrealloc(c->addr_cache, sizeof(AerVal*) * c->addr_cache_cap);
    }
    c->addr_cache[c->addr_cache_count] = NULL;
    return c->addr_cache_count++;
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

/* Wraps (data, length) — caller must already exclusively own data — in a fresh heap box; never allocates or copies the character data itself.
   vm_pools_init_once() is normally reached via vm_init() before anything compiles, but the lexer
   can call this (via emit_string_token, for every identifier/string token) during compilation
   itself — e.g. v3_parse() legitimately runs lex()/v3_parse() before any VM exists yet. Without
   this guard, string_pool is still the zero-initialized static (elem_size=0, elems_per_slab=0),
   so pool_alloc's slab xmalloc(0*0) hands back a ~1-byte allocation that this function then
   writes a pointer into — a real heap-buffer-overflow, confirmed via ASAN (only reachable when
   nothing else already initialized the pools first, which every existing caller in main.c/
   tests/v3_smoke_test.c's early tests happened to already guarantee by accident of ordering). */
AerVal aer_make_string(char* data, unsigned int length) {
    vm_pools_init_once();
    AerString* s = pool_alloc(&string_pool);
    s->data = data;
    s->length = length;
    return aer_string_val(s);
}

void chunk_emit(Chunk* c, int word) {
    if (c->count >= c->capacity) {
        c->capacity = c->capacity ? c->capacity * 2 : 64;
        c->code = xrealloc(c->code, sizeof(int) * c->capacity);
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

        unsigned int* existing = (unsigned int*)hashmap_get(&c->name_index, key);
        if (existing) { free(key); return *existing; }

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
        char* index_key = xstrdup(key);
        unsigned int* idx_box = xmalloc(sizeof(unsigned int));
        *idx_box = idx;
        hashmap_put(&c->name_index, index_key, idx_box);
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
    vm->chunk       = chunk;
    vm->scope_depth = 1;
    aer_stdlib_init();
    vm_pools_init_once();
    runtime_line_lookup = lookup_runtime_line;
#ifdef AER_V3
    /* Resets the v3 call stack for a fresh run — needed since v3_call_depth/v3_registers are
       file-scope statics outside VM, shared across every vm_init() call in a process (e.g. a test
       harness running several independent chunks back to back); a chunk that ended mid-call
       (a bug, or a deliberately unbalanced test) must not leak into the next chunk's run. */
    v3_call_depth = 0;
    v3_registers  = v3_call_stack[0].registers;
    v3_call_stack[0].defer_count = 0;
#endif
}

void vm_free(VM* vm) {
    for (int i = 0; i < vm->scope_depth; i++)
        if (vm->scopes[i].overflow)
            hashmap_free(&vm->scopes[i].map);
    /* Every call_stack slot, not just up to call_depth — a slot's lazily allocated defers array stays allocated across reuse, so any slot ever used may still hold one. */
    for (int i = 0; i < VM_CALL_MAX; i++)
        free(vm->call_stack[i].defers);
#ifdef AER_V3
    /* Same reasoning as the stack VM's loop just above — a v3 call_stack slot's lazily allocated
       defers array persists across reuse, so any slot ever used may still hold one. */
    for (int i = 0; i < VM_CALL_MAX; i++)
        free(v3_call_stack[i].defers);
#endif
}

/* ------------------------------------------------------------------ */
/* Scope helpers                                                        */
/* ------------------------------------------------------------------ */

/* The current call's own (only) scope index. No closures and no per-block scoping left (see
   parser.c's current_locals), so every local name in a function resolves to OP_LOAD_LOCAL/
   STORE_LOCAL at parse time instead of a name-based scope walk — this only exists to locate that
   one flat frame, never to walk multiple levels. scope_depth - 1 always equals it directly while
   any function's own bytecode is executing (nothing pushes a scope except a call, and a call this
   function itself makes is popped back off before control returns here), which is what
   vm_read_local_slot/vm_write_local_slot rely on. */
static inline int vm_scope_floor(VM* vm) {
    return vm->scope_depth - 1;
}

/* Global scope only — scopes[0]. Used directly by OP_LOAD/OP_STORE (see OP_LOAD_LOCAL's comment
   in vm.h: the parser only ever emits those for a true global), and as the fallback half of
   vm_scope_get below. */
static inline __attribute__((always_inline)) AerVal* vm_global_get(VM* vm, unsigned int name) {
    AerScope* g = &vm->scopes[0];
    for (int j = 0; j < g->count; j++)
        if (g->slots[j].name == name) return &g->slots[j].val;
    if (g->overflow) {
        AerVal* v = (AerVal*)hashmap_get(&g->map, aer_as_string(vm->chunk->pool[name])->data);
        if (v) return v;
    }
    return NULL;
}

/* Checks the current call's own local slots by name first, then falls back to vm_global_get.
   Needed specifically by OP_CALL and defer's replay (lbl_return): unlike a plain variable
   reference, a call site isn't resolved to LOCAL-vs-GLOBAL at parse time (see OP_DEFINE_LOCAL's
   comment in vm.h), so calling a function value held in a parameter or local — `f = fn; f()` —
   still needs a runtime name lookup. Only one level to check now (no per-block scoping left),
   so this is a single linear scan, not a chain walk. */
static inline AerVal* vm_scope_get(VM* vm, unsigned int name) {
    AerScope* s = &vm->scopes[vm_scope_floor(vm)];
    for (int j = 0; j < s->count; j++)
        if (s->slots[j].name == name) return &s->slots[j].val;
    return vm_global_get(vm, name);
}

/* For the inline address cache: is `name` a global whose address is stable for the VM's whole
   lifetime? Only the plain slots array qualifies (vm->scopes[0] never reallocates) — an
   overflowed global's hashmap bucket array can move on rehash, so that case just falls back to
   vm_global_get, uncached, same as today. */
static AerVal* vm_global_slot_address(VM* vm, unsigned int name) {
    AerScope* g = &vm->scopes[0];
    for (int j = 0; j < g->count; j++)
        if (g->slots[j].name == name) return &g->slots[j].val;
    return NULL;
}

/* Shared by lbl_load, lbl_store, and the OP_COMPOUND_NAME_*-and-OP_INDEX_*_NAME fused handlers —
   resolves name_idx (always a true global — see vm_global_get's comment) via Chunk.addr_cache,
   falling back to a fresh global-scope lookup on a cold site. Returns NULL if undefined (caller
   decides what that means: error, or "define fresh"). Does NOT itself call error() — see
   vm_resolve_name_cached below for the erroring wrapper lbl_load and the compound/index handlers
   actually use. No invalidation logic needed: the cache holds an address, not a value. */
static inline AerVal* vm_resolve_name_full(VM* vm, Chunk* c, unsigned int name_idx, int cache_idx) {
    AerVal* found = c->addr_cache[cache_idx];
    if (found) return found;

    found = vm_global_get(vm, name_idx);
    if (!found) return NULL;

    AerVal* cacheable = vm_global_slot_address(vm, name_idx);
    if (cacheable == found) c->addr_cache[cache_idx] = cacheable;
    return found;
}

/* Shared by lbl_load and the OP_COMPOUND_NAME_* fused handlers — same resolution as
   vm_resolve_name_full, but calls error() if undefined. Caller must check and bail via
   DISPATCH(), same as any fallible sub-step in a fused handler. */
static inline AerVal* vm_resolve_name_cached(VM* vm, Chunk* c, int name_idx, int cache_idx) {
    AerVal* found = vm_resolve_name_full(vm, c, (unsigned int)name_idx, cache_idx);
    if (!found) error("'%s' is not defined", aer_as_string(c->pool[name_idx])->data);
    return found;
}

/* Shared by lbl_load_local and any fused handler reading a LOCAL-kind operand. */
static inline AerVal vm_read_local_slot(VM* vm, int slot) {
    return vm->scopes[vm_scope_floor(vm)].slots[slot].val;
}

/* Write counterpart of vm_read_local_slot; shared by lbl_store_local and the fused handlers' write-back. */
static inline void vm_write_local_slot(VM* vm, int slot, AerVal val) {
    vm->scopes[vm_scope_floor(vm)].slots[slot].val = val;
}

/* Always create/update in the global scope — used for top-level assignment and top-level loop vars
   (the only remaining OP_DEFINE/OP_STORE-fallback callers; every in-function local goes through
   OP_DEFINE_LOCAL instead — see parser.c's current_locals). */
static inline __attribute__((always_inline)) void vm_scope_define(VM* vm, unsigned int name, AerVal val) {
    AerScope* s = &vm->scopes[0];
    for (int j = 0; j < s->count; j++) {
        if (s->slots[j].name == name) { s->slots[j].val = val; return; }
    }
    if (s->count < SCOPE_SLOT_MAX) {
        s->slots[s->count].name = name;
        s->slots[s->count].val  = val;
        s->count++;
        return;
    }
    /* Spill to hashmap for scopes with more than SCOPE_SLOT_MAX variables. */
    s->overflow = true;
    const char* key = aer_as_string(vm->chunk->pool[name])->data;
    AerVal* existing = (AerVal*)hashmap_get(&s->map, key);
    if (existing) { *existing = val; return; }
    char* k = xmalloc(strlen(key) + 1);
    AerVal* vp = xmalloc(sizeof(AerVal));
    strcpy(k, key);
    *vp = val;
    hashmap_put(&s->map, k, vp);
}

/* Update the nearest global binding if one exists, else create one — used by OP_STORE, which
   (per vm_global_get's comment) only ever targets scopes[0] now. */
static inline __attribute__((always_inline)) void vm_scope_set(VM* vm, unsigned int name, AerVal val) {
    AerVal* found = vm_global_get(vm, name);
    if (found) { *found = val; return; }
    vm_scope_define(vm, name, val);
}

/* ------------------------------------------------------------------ */
/* Type helpers                                                         */
/* ------------------------------------------------------------------ */

/* Struct instances report their declared name (e.g. "Player") instead of "array" — used by type() and OP_CHECK_SHAPE's error message. */
static const char* vm_type_name(Chunk* c, AerVal v) {
    static const char* type_names[] = {
        "null", "boolean", "integer", "real", "string", "function", "array", "dict"
    };
    if (aer_type(v) == TYPE_ARRAY && aer_as_array(v)->shape)
        return aer_as_string(c->pool[aer_as_array(v)->shape->name])->data;
    return type_names[aer_type(v)];
}

/* ------------------------------------------------------------------ */
/* Value formatting — shared by print() and vm_to_str() (interpolation, +, etc.) for one consistent recursive rendering, not a terse "<array[3]>" fallback. */
/* ------------------------------------------------------------------ */

typedef struct { char* buf; size_t len; size_t cap; } StrBuilder;

static void sb_init(StrBuilder* sb) {
    sb->cap = 64;
    sb->buf = xmalloc(sb->cap);
    sb->len = 0;
    sb->buf[0] = '\0';
}

static void sb_append_n(StrBuilder* sb, const char* s, size_t n) {
    if (sb->len + n + 1 > sb->cap) {
        while (sb->len + n + 1 > sb->cap) sb->cap *= 2;
        sb->buf = xrealloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_append(StrBuilder* sb, const char* s) { sb_append_n(sb, s, strlen(s)); }

static void vm_format_value(Chunk* c, AerVal v, bool in_collection, StrBuilder* sb);

static void vm_format_value(Chunk* c, AerVal v, bool in_collection, StrBuilder* sb) {
    char tmp[64];
    switch (aer_type(v)) {
        case TYPE_NULL:     sb_append(sb, "null"); break;
        case TYPE_INTEGER:  snprintf(tmp, sizeof(tmp), "%lld", aer_as_int(v));  sb_append(sb, tmp); break;
        case TYPE_REAL:     snprintf(tmp, sizeof(tmp), "%g",   aer_as_real(v)); sb_append(sb, tmp); break;
        case TYPE_BOOLEAN:  sb_append(sb, aer_as_bool(v) ? "true" : "false"); break;
        case TYPE_FUNCTION: sb_append(sb, "<function>"); break;
        case TYPE_STRING: {
            AerString* s = aer_as_string(v);
            if (in_collection) sb_append(sb, "\"");
            sb_append_n(sb, s->data, s->length);
            if (in_collection) sb_append(sb, "\"");
            break;
        }
        case TYPE_ARRAY: {
            AerArray* a = aer_as_array(v);
            if (a->shape) {
                Shape* shape = a->shape;
                sb_append(sb, aer_as_string(c->pool[shape->name])->data);
                sb_append(sb, "{");
                for (unsigned int i = 0; i < shape->field_count; i++) {
                    if (i > 0) sb_append(sb, ", ");
                    sb_append(sb, aer_as_string(c->pool[shape->field_names[i]])->data);
                    sb_append(sb, ": ");
                    vm_format_value(c, a->items[i], true, sb);
                }
                sb_append(sb, "}");
                break;
            }
            sb_append(sb, "[");
            for (unsigned int i = 0; i < a->count; i++) {
                if (i > 0) sb_append(sb, ", ");
                vm_format_value(c, a->items[i], true, sb);
            }
            sb_append(sb, "]");
            break;
        }
        case TYPE_DICT: {
            AerDict* d = aer_as_dict(v);
            sb_append(sb, "{");
            bool first = true;
            for (unsigned int i = 0; i < d->map.capacity; i++) {
                HashTableEntry* e = &d->map.buckets[i];
                if (!e->key) continue;
                if (!first) sb_append(sb, ", ");
                first = false;
                sb_append(sb, "\"");
                sb_append(sb, e->key);
                sb_append(sb, "\": ");
                vm_format_value(c, e->payload.inline_val, true, sb);
            }
            sb_append(sb, "}");
            break;
        }
    }
}

static void vm_print_value(Chunk* c, AerVal v, bool in_collection) {
    StrBuilder sb;
    sb_init(&sb);
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
    }
    return false;
}

static inline __attribute__((always_inline)) AerVal vm_binary(AerVal a, AerVal b, Opcode op) {
    /* Logical ops work on any type via truthiness */
    if (op == OP_AND || op == OP_OR) {
        return aer_bool((op == OP_AND) ? (vm_truthy(a) && vm_truthy(b))
                                        : (vm_truthy(a) || vm_truthy(b)));
    }

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
            return aer_bool(dictmap_get(&aer_as_dict(b)->map, kbuf) != NULL);
        }
        if (aer_type(b) == TYPE_ARRAY) {
            AerArray* arr = aer_as_array(b);
            for (unsigned int i = 0; i < arr->count; i++) {
                if (values_equal(a, arr->items[i])) return aer_bool(true);
            }
            return aer_bool(false);
        }
        error("Right side of 'in' must be a dict or array");
        return aer_bool(false);
    }

    /* Integer-vs-integer fast path, checked before null/real-promotion below — the common case for arithmetic-heavy code, and neither branch applies once both are integers. */
    if (aer_type(a) == TYPE_INTEGER && aer_type(b) == TYPE_INTEGER) {
        long long l = aer_as_int(a), rv = aer_as_int(b);
        switch (op) {
            case OP_ADD:         return aer_int(l + rv);
            case OP_SUB:         return aer_int(l - rv);
            case OP_MUL:         return aer_int(l * rv);
            case OP_DIV:
                if (rv == 0) { error("Division by zero"); return aer_int(0); }
                return aer_real((double)l / (double)rv);
            case OP_FLOOR_DIV:
                if (rv == 0) { error("Division by zero"); return aer_int(0); }
                return aer_int((long long)floor((double)l / (double)rv));
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

    /* Real-vs-real fast path, mirrored right after the int/int one above for the same reason —
       nbody-style arithmetic-heavy code is dominated by this case, but until now it fell through
       the AND/OR check, the IN check, the (failing) int/int check, a null check, and real-
       promotion logic that's a no-op when both operands are already real, before ever reaching
       the real/real switch below (which still has to stay — this is not a replacement for it, just
       a fast exit before the checks a promoted-from-int operand still needs). Same case bodies as
       that switch, duplicated rather than shared, since jumping into the middle of the block below
       isn't simpler than just checking here first. Measured neutral in isolation on nbody.aer
       (~2.41s vs ~2.42s baseline, within noise) — kept anyway since it's a correct simplification
       with no added opcodes/call sites and no downside found, not because it proved a win. */
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

    /* null equality: null == null is true; null op anything-else errors */
    if (aer_type(a) == TYPE_NULL || aer_type(b) == TYPE_NULL) {
        if (op == OP_EQ)  return aer_bool(aer_type(a) == TYPE_NULL && aer_type(b) == TYPE_NULL);
        if (op == OP_NEQ) return aer_bool(!(aer_type(a) == TYPE_NULL && aer_type(b) == TYPE_NULL));
        error("Operator not valid for null"); return aer_bool(false);
    }

    if (aer_type(a) == TYPE_REAL || aer_type(b) == TYPE_REAL) {
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

    error("Type mismatch in binary expression");
    return aer_bool(false);
}

static AerVal vm_to_str(VM* vm, AerVal v) {
    if (aer_type(v) == TYPE_STRING) return v;

    char*        owned;
    unsigned int len;

    if (aer_type(v) == TYPE_ARRAY || aer_type(v) == TYPE_DICT) {
        /* Unbounded recursive content doesn't fit the fixed buffer below, so reuse print()'s formatter; sb.buf is already a fresh allocation, handed to aer_make_string as-is. */
        StrBuilder sb;
        sb_init(&sb);
        vm_format_value(vm->chunk, v, false, &sb);
        owned = sb.buf;
        len   = (unsigned int)sb.len;
    } else {
        /* Copies into a fresh owned buffer since AerString always owns its data, and buf is a stack array that can't be handed to aer_make_string directly. */
        char buf[64];
        switch (aer_type(v)) {
            case TYPE_NULL:     snprintf(buf, sizeof(buf), "null");                              break;
            case TYPE_INTEGER:  snprintf(buf, sizeof(buf), "%lld", aer_as_int(v));               break;
            case TYPE_REAL:     snprintf(buf, sizeof(buf), "%g",   aer_as_real(v));               break;
            case TYPE_BOOLEAN:  snprintf(buf, sizeof(buf), "%s",   aer_as_bool(v) ? "true" : "false"); break;
            case TYPE_FUNCTION: snprintf(buf, sizeof(buf), "<function>");                        break;
            case TYPE_ARRAY: case TYPE_DICT: case TYPE_STRING: break;   /* handled above */
        }
        len   = (unsigned int)strlen(buf);
        owned = xmalloc(len + 1);
        memcpy(owned, buf, len + 1);
    }
    /* No chunk_add_pool interning: this string is used once and never looked up by pool index again. Interning would grow the pool/name_index forever per unique value — measured 7x slower for 100k unique casts vs. 10 distinct ones. */
    return aer_make_string(owned, len);
}

/* Resolves a[start:end] bounds against length `len`; either bound may be TYPE_NULL (defaults to 0/len). Clamps out-of-range bounds instead of erroring, Python-slice style. */
static bool vm_slice_bounds(AerVal start_v, AerVal end_v, long long len,
                             long long* out_start, long long* out_end) {
    if (aer_type(start_v) != TYPE_NULL && aer_type(start_v) != TYPE_INTEGER) { error("Slice bounds must be integers"); return false; }
    if (aer_type(end_v)   != TYPE_NULL && aer_type(end_v)   != TYPE_INTEGER) { error("Slice bounds must be integers"); return false; }
    long long start = (aer_type(start_v) == TYPE_NULL) ? 0   : aer_as_int(start_v);
    long long end   = (aer_type(end_v)   == TYPE_NULL) ? len : aer_as_int(end_v);
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
        d->map.is_inline = true;
        return aer_dict_val(d);
    }
    return dflt;
}

/* Shared by lbl_call_value and aer_module_call — arity/receiver-type check and call-frame
   construction, identical whether the call stays in one VM or crosses into a module's own.
   Doesn't touch the argument stack; `fn_chunk` is the function's own chunk (for error
   messages), not necessarily target's. Not static: aer_module.c calls this too. */
bool vm_setup_call(VM* target, Chunk* fn_chunk, AerVal fv, int arg_count,
                    AerVal* args, unsigned int return_ip) {
    if (aer_type(fv) != TYPE_FUNCTION) { error("Value is not callable"); return false; }
    AerFunction* f = aer_as_function(fv);
    if (arg_count < (int)f->min_arity || arg_count > (int)f->arity) {
        if (f->min_arity == f->arity)
            error("Function expects %u arguments, got %d", (unsigned int)f->arity, arg_count);
        else
            error("Function expects between %u and %u arguments, got %d", (unsigned int)f->min_arity, (unsigned int)f->arity, arg_count);
        return false;
    }
    if (f->has_receiver) {
        AerVal arg0 = args[0];
        if (aer_type(arg0) != TYPE_ARRAY || !aer_as_array(arg0)->shape ||
            aer_as_array(arg0)->shape->name != f->receiver_type) {
            error("Function expects its first argument to be a %s",
                  aer_as_string(fn_chunk->pool[f->receiver_type])->data);
            return false;
        }
    }
    if (target->call_depth >= VM_CALL_MAX) { error("Call stack overflow"); return false; }
    /* Only pushed after every failure check above — a partial push here would leave stray values a caller's own cleanup wouldn't know to pop. */
    int missing = (int)f->arity - arg_count;
    if (missing > 0) {
        if (target->stack_top + missing > VM_STACK_MAX) { error("Stack overflow"); return false; }
        for (int i = arg_count; i < (int)f->arity; i++)
            target->stack[target->stack_top++] = vm_default_value(f->defaults[i - f->min_arity]);
    }
    CallFrame* frame = &target->call_stack[target->call_depth];
    frame->return_ip          = return_ip;
    frame->return_scope_depth = target->scope_depth;
    frame->function           = f;
    /* call_stack is a fixed, reused array — a slot's previous occupant may have left pending defers, which a fresh call must never inherit. */
    frame->defer_count        = 0;
    frame->defers_draining    = false;
    target->call_depth++;
    target->ip = f->code_offset;
    return true;
}

AerArray* vm_new_array(void) {
    return pool_alloc(&array_pool);
}

AerDict* vm_new_dict(void) {
    return pool_alloc(&dict_pool);
}

AerFunction* vm_new_function(void) {
    return pool_alloc(&function_pool);
}

/* Resolves name as a builtin or struct call against a plain args[] array (not POP()ing from
   the operand stack) — used only by deferred-call replay in lbl_return, whose args are already
   staged in a bounded DeferredCall; lbl_call keeps its own POP()-based version since its
   arg_count is parser-unbounded. Returns true if `name` was recognized, result in *out. */
static bool vm_call_builtin(Chunk* c, const char* name, AerVal* args, int arg_count, AerVal* out) {
    *out = aer_null();

    if (strcmp(name, "length") == 0 && arg_count == 1) {
        AerVal a = args[0];
        if      (aer_type(a) == TYPE_ARRAY)  *out = aer_int((long long)aer_as_array(a)->count);
        else if (aer_type(a) == TYPE_STRING) *out = aer_int((long long)aer_as_string(a)->length);
        else if (aer_type(a) == TYPE_DICT)   *out = aer_int((long long)aer_as_dict(a)->map.count);
        else error("length() requires an array, dict, or string");
        return true;
    }
    if (strcmp(name, "delete") == 0 && arg_count == 2) {
        AerVal obj = args[0], key = args[1];
        if (aer_type(obj) == TYPE_DICT) {
            if (aer_type(key) != TYPE_STRING) { error("delete() key must be a string"); return true; }
            AerString* ks = aer_as_string(key);
            unsigned int klen = ks->length;
            if (klen > VM_KEY_MAX) { error("Dict key too long (max %d bytes)", VM_KEY_MAX); return true; }
            char kbuf[VM_KEY_MAX + 1];
            memcpy(kbuf, ks->data, klen);
            kbuf[klen] = '\0';
            dictmap_remove(&aer_as_dict(obj)->map, kbuf);
            *out = obj;
            return true;
        }
        if (aer_type(obj) == TYPE_ARRAY) {
            AerArray* a = aer_as_array(obj);
            if (a->shape) { error("delete() cannot remove fields from a struct instance — structs have a fixed shape"); return true; }
            if (aer_type(key) != TYPE_INTEGER) { error("Array delete() index must be an integer"); return true; }
            long long i = aer_as_int(key);
            if (i < 0) i += (long long)a->count;
            if (i < 0 || (unsigned long long)i >= a->count) { error("Array index %lld out of bounds (len %u)", aer_as_int(key), a->count); return true; }
            memmove(&a->items[i], &a->items[i + 1], (size_t)(a->count - (unsigned long long)i - 1) * sizeof(AerVal));
            a->count--;
            *out = obj;
            return true;
        }
        error("delete() requires a dict or array");
        return true;
    }
    if (strcmp(name, "append") == 0 && arg_count == 2) {
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
    if (strcmp(name, "print") == 0 && arg_count == 1) {
        vm_print_value(c, args[0], false);
        printf("\n");
        return true;
    }
    if (strcmp(name, "type") == 0 && arg_count == 1) {
        const char* tn = vm_type_name(c, args[0]);
        /* Copies rather than pointing at a static literal or the chunk's pool data — AerString always owns its data, no exceptions. */
        unsigned int tn_len = (unsigned int)strlen(tn);
        char* tn_buf = xmalloc(tn_len + 1);
        memcpy(tn_buf, tn, tn_len + 1);
        *out = aer_make_string(tn_buf, tn_len);   /* no chunk_add_pool interning — see vm_to_str's comment */
        return true;
    }
    if (strcmp(name, "assert") == 0 && arg_count == 2) {
        AerVal cond = args[0], msg = args[1];
        if (aer_type(msg) != TYPE_STRING) { error("assert() requires a string message as its second argument"); return true; }
        if (!vm_truthy(cond)) {
            assert_failure_count++;
            AerString* ms = aer_as_string(msg);
            printf("ASSERT FAILED: %.*s\n", (int)ms->length, ms->data);
        }
        return true;
    }
    if (strcmp(name, "panic") == 0 && arg_count == 1) {
        AerVal msg = args[0];
        if (aer_type(msg) != TYPE_STRING) { error("panic() requires a string message"); return true; }
        AerString* ms = aer_as_string(msg);
        error("panic: %.*s", (int)ms->length, ms->data);
        return true;
    }

    Shape* shape = chunk_find_shape(c, name);
    if (shape) {
        if ((unsigned int)arg_count > shape->field_count) {
            error("'%s' takes at most %u argument%s, got %d",
                  name, shape->field_count, shape->field_count == 1 ? "" : "s", arg_count);
            return true;
        }
        /* struct_pool cell holds the AerArray header AND its field storage in one allocation —
           items points right after the header instead of a separate xmalloc (see vm_pools_init_once). */
        AerArray* a = pool_alloc(&struct_pool);
        a->count = a->capacity = shape->field_count;
        a->items = (AerVal*)((char*)a + sizeof(AerArray));
        a->shape = shape;
        for (int i = 0; i < arg_count; i++) a->items[i] = args[i];
        for (unsigned int i = (unsigned int)arg_count; i < shape->field_count; i++)
            a->items[i] = vm_default_value(shape->field_defaults[i]);
        *out = aer_array_val(a);
        return true;
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
static inline AerVal vm_index_get_compute(AerVal obj, AerVal idx) {
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) { error("Struct fields are accessed with '.', not '[]'"); return aer_null(); }
        if (aer_type(idx) != TYPE_INTEGER) { error("Array index must be an integer"); return aer_null(); }
        long long i = aer_as_int(idx);
        if (i < 0) i += (long long)a->count;
        if (i < 0 || (unsigned long long)i >= a->count) { error("Array index %lld out of bounds (len %u)", aer_as_int(idx), a->count); return aer_null(); }
        return a->items[i];
    } else if (aer_type(obj) == TYPE_DICT) {
        if (aer_type(idx) != TYPE_STRING) { error("Dict key must be a string"); return aer_null(); }
        AerString* is = aer_as_string(idx);
        unsigned int klen = is->length;
        if (klen > VM_KEY_MAX) { error("Dict key too long (max %d bytes)", VM_KEY_MAX); return aer_null(); }
        char kbuf[VM_KEY_MAX + 1];
        memcpy(kbuf, is->data, klen);
        kbuf[klen] = '\0';
        AerVal* found = dictmap_get(&aer_as_dict(obj)->map, kbuf);
        if (!found) return aer_null();
        return *found;
    } else if (aer_type(obj) == TYPE_STRING) {
        AerString* os = aer_as_string(obj);
        if (aer_type(idx) != TYPE_INTEGER) { error("String index must be an integer"); return aer_null(); }
        long long i = aer_as_int(idx);
        long long len = (long long)os->length;
        if (i < 0) i += len;
        if (i < 0 || i >= len) { error("String index %lld out of bounds (len %lld)", aer_as_int(idx), len); return aer_null(); }
        /* A single character is a length-1 string (AER has no char type); copies the byte since AerString must always own its data, even after obj is later collected. */
        char* ch_buf = xmalloc(2);
        ch_buf[0] = os->data[i];
        ch_buf[1] = '\0';
        return aer_make_string(ch_buf, 1);   /* no chunk_add_pool interning — see vm_to_str's comment */
    } else {
        error("Cannot index type");
        return aer_null();
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
        long long i = aer_as_int(idx);
        if (i < 0) i += (long long)a->count;
        if (i < 0 || (unsigned long long)i >= a->count) { error("Array index %lld out of bounds (len %u)", aer_as_int(idx), a->count); return; }
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
        AerVal* existing = dictmap_get(&aer_as_dict(obj)->map, kbuf);
        if (existing) {
            *existing = val;   /* update in place — no allocation */
        } else {
            char* k = xmalloc(klen + 1);
            memcpy(k, is->data, klen);
            k[klen] = '\0';
            dictmap_put(&aer_as_dict(obj)->map, k, val);
        }
    } else if (aer_type(obj) == TYPE_STRING) {
        error("Strings are immutable — cannot assign to an index");
    } else {
        error("Cannot index type");
    }
}

/* Shared by lbl_cast and (M5 slice 9) lbl_v3_cast — extracted so the v3 register path can reuse
   the exact same conversion rules without duplicating this switch. */
static AerVal vm_cast(AerVal v, int cast_type) {
    AerVal r = aer_null();
    /* atoll()/atof() only consume a leading sign/digits(/./exponent), so truncating to a fixed buffer (instead of a length-sized VLA) can't change the parsed value for a real number. */
    char buf[64];
    switch (cast_type) {
        case CAST_INTEGER:
            switch (aer_type(v)) {
                case TYPE_INTEGER: r = v; break;
                case TYPE_REAL:    r = aer_int((long long)aer_as_real(v)); break;
                case TYPE_BOOLEAN: r = aer_int(aer_as_bool(v) ? 1 : 0); break;
                case TYPE_STRING: {
                    AerString* vs = aer_as_string(v);
                    unsigned int n = vs->length < sizeof(buf) - 1
                                          ? vs->length : sizeof(buf) - 1;
                    memcpy(buf, vs->data, n);
                    buf[n] = '\0';
                    r = aer_int(atoll(buf)); break;
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
                    r = aer_real(atof(buf)); break;
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
    c->debug_hits = xrealloc(c->debug_hits, sizeof(unsigned long long) * c->debug_hits_cap);
    memset(c->debug_hits + old_cap, 0, sizeof(unsigned long long) * (c->debug_hits_cap - old_cap));
}
#endif

/* Grows Chunk.field_cache_shape/slot to cover every word currently in c->code, zero-filling the
   new region (NULL shape = not cached) — same growth idiom as chunk_ensure_debug_hits above, but
   unconditional: this is a real always-on perf feature, not a debug tool. Called once at the top
   of vm_run; a no-op once field_cache_cap already covers c->count (REPL appends code across
   vm_run calls, same as debug_hits). */
static void chunk_ensure_field_cache(Chunk* c) {
    if (c->count <= c->field_cache_cap) return;
    unsigned int old_cap = c->field_cache_cap;
    c->field_cache_cap = c->count;
    c->field_cache_shape = xrealloc(c->field_cache_shape, sizeof(Shape*) * c->field_cache_cap);
    c->field_cache_slot  = xrealloc(c->field_cache_slot,  sizeof(int)    * c->field_cache_cap);
    memset(c->field_cache_shape + old_cap, 0, sizeof(Shape*) * (c->field_cache_cap - old_cap));
}

bool vm_run(VM* vm) {
    Chunk* c = vm->chunk;
    Opcode cur_op;
    /* The just-fetched instruction word, opcode and all — outer-scope for the same reason cur_op
       is: it must still be readable inside the handler body the goto jumps to, past the end of
       DISPATCH()'s own do-while block. For a plain (unpacked) instruction this is just cur_op's
       own value with zero upper bits, unused by that handler. For a packed OP_V3_* instruction
       (see V3_PACK3's comment in vm.h) the upper bits hold that instruction's narrow operands,
       read via V3_UNPACK_A/B/C — every opcode value is < 256 (94 total), so masking this word
       with & 0xFF to get cur_op is a complete no-op for every existing, unpacked instruction (its
       own upper bits are already zero) and correctly extracts the opcode from a packed one too;
       one shared DISPATCH() handles both without needing to know in advance which kind of
       instruction it's about to fetch — which matters because a handful of opcodes (OP_JUMP,
       OP_DEFINE_STRUCT, OP_HALT) are reused verbatim inside v3-compiled bytecode, reached from
       both packed and unpacked contexts through the exact same handler code. */
    unsigned int op_word;
#ifdef AER_DEBUG_TOOLS
    chunk_ensure_debug_hits(c);
#endif
    chunk_ensure_field_cache(c);

#define READ()     (c->code[vm->ip++])
#define PUSH(v)    do { if (vm->stack_top >= VM_STACK_MAX) { error("Stack overflow"); return false; } vm->stack[vm->stack_top++] = (v); } while(0)
#define POP()      (vm->stack_top > 0 ? vm->stack[--vm->stack_top] : (error("Stack underflow"), aer_null()))
/* active_vm_for_errors = vm is just a pointer store; the line-lookup binary search only runs inside error() when a fault fires, not per-opcode as an earlier version did. */
#ifdef AER_DEBUG_TOOLS
#define DISPATCH() do { if (runtime_had_error) return false; gc_maybe_collect(vm); active_vm_for_errors = vm; unsigned int op_ip = vm->ip; op_word = (unsigned int)READ(); cur_op = (Opcode)(op_word & 0xFF); c->debug_hits[op_ip]++; goto *dt[cur_op]; } while(0)
#else
#define DISPATCH() do { if (runtime_had_error) return false; gc_maybe_collect(vm); active_vm_for_errors = vm; op_word = (unsigned int)READ(); cur_op = (Opcode)(op_word & 0xFF); goto *dt[cur_op]; } while(0)
#endif

    static const void* const dt[] = {
        [OP_PUSH]           = &&lbl_push,
        [OP_DUP_N]          = &&lbl_dup_n,
        [OP_LOAD]           = &&lbl_load,
        [OP_STORE]          = &&lbl_store,
        [OP_DEFINE]         = &&lbl_define,
        [OP_LOAD_LOCAL]     = &&lbl_load_local,
        [OP_STORE_LOCAL]    = &&lbl_store_local,
        [OP_DEFINE_LOCAL]   = &&lbl_define_local,
        [OP_COMPOUND_NAME_CONST] = &&lbl_compound_name_const,
        [OP_COMPOUND_NAME_NAME]  = &&lbl_compound_name_name,
        [OP_COMPOUND_LOCAL_CONST] = &&lbl_compound_local_const,
        [OP_COMPOUND_LOCAL_LOCAL] = &&lbl_compound_local_local,
        [OP_COMPOUND_LOCAL_NAME]  = &&lbl_compound_local_name,
        [OP_COMPOUND_NAME_LOCAL]  = &&lbl_compound_name_local,
        [OP_BINARY_LOCAL_LOCAL] = &&lbl_binary_local_local,
        [OP_BINARY_LOCAL_CONST] = &&lbl_binary_local_const,
        [OP_BINARY_LOCAL_NAME]  = &&lbl_binary_local_name,
        [OP_BINARY_NAME_LOCAL]  = &&lbl_binary_name_local,
        [OP_BINARY_NAME_CONST]  = &&lbl_binary_name_const,
        [OP_BINARY_NAME_NAME]   = &&lbl_binary_name_name,
        [OP_COMPOUND_INDEXED_FIELD_LOCAL_LOCAL] = &&lbl_compound_indexed_field_local_local,
        [OP_ADD]            = &&lbl_binary,
        [OP_SUB]            = &&lbl_binary,
        [OP_MUL]            = &&lbl_binary,
        [OP_DIV]            = &&lbl_binary,
        [OP_MOD]            = &&lbl_binary,
        [OP_FLOOR_DIV]      = &&lbl_binary,
        [OP_EQ]             = &&lbl_binary,
        [OP_NEQ]            = &&lbl_binary,
        [OP_LT]             = &&lbl_binary,
        [OP_GT]             = &&lbl_binary,
        [OP_LTE]            = &&lbl_binary,
        [OP_GTE]            = &&lbl_binary,
        [OP_IN]             = &&lbl_binary,
        [OP_AND]            = &&lbl_binary,
        [OP_OR]             = &&lbl_binary,
        [OP_PIPE]           = &&lbl_binary,
        [OP_BITWISE_AND]    = &&lbl_binary,
        [OP_BITWISE_OR]     = &&lbl_binary,
        [OP_BITWISE_XOR]    = &&lbl_binary,
        [OP_LSHIFT]         = &&lbl_binary,
        [OP_RSHIFT]         = &&lbl_binary,
        [OP_NEGATE]         = &&lbl_negate,
        [OP_NOT]            = &&lbl_not,
        [OP_BITWISE_NOT]    = &&lbl_bitwise_not,
        [OP_JUMP]           = &&lbl_jump,
        [OP_JUMP_IF_FALSE]  = &&lbl_jump_if_false,
        [OP_JUMP_IF_TRUE]   = &&lbl_jump_if_true,
        [OP_CMP_JUMP_FALSE] = &&lbl_cmp_jump_false,
        [OP_PUSH_SCOPE]     = &&lbl_push_scope,
        [OP_POP_SCOPE]      = &&lbl_pop_scope,
        [OP_CALL]           = &&lbl_call,
        [OP_TAIL_CALL]      = &&lbl_call,
        [OP_CALL_VALUE]     = &&lbl_call_value,
        [OP_CALL_MODULE]    = &&lbl_call_module,
        [OP_RETURN]         = &&lbl_return,
        [OP_DEFER_PUSH]     = &&lbl_defer_push,
        [OP_ARRAY_NEW]      = &&lbl_array_new,
        [OP_DICT_NEW]       = &&lbl_dict_new,
        [OP_INDEX_GET]      = &&lbl_index_get,
        [OP_INDEX_GET_LOCAL_CONST] = &&lbl_index_get_local_const,
        [OP_INDEX_GET_LOCAL_LOCAL] = &&lbl_index_get_local_local,
        [OP_INDEX_GET_LOCAL_NAME]  = &&lbl_index_get_local_name,
        [OP_INDEX_GET_NAME_CONST]  = &&lbl_index_get_name_const,
        [OP_INDEX_GET_NAME_LOCAL]  = &&lbl_index_get_name_local,
        [OP_INDEX_GET_NAME_NAME]   = &&lbl_index_get_name_name,
        [OP_INDEX_SET]      = &&lbl_index_set,
        [OP_INDEX_SET_LOCAL_CONST] = &&lbl_index_set_local_const,
        [OP_INDEX_SET_LOCAL_LOCAL] = &&lbl_index_set_local_local,
        [OP_INDEX_SET_LOCAL_NAME]  = &&lbl_index_set_local_name,
        [OP_INDEX_SET_NAME_CONST]  = &&lbl_index_set_name_const,
        [OP_INDEX_SET_NAME_LOCAL]  = &&lbl_index_set_name_local,
        [OP_INDEX_SET_NAME_NAME]   = &&lbl_index_set_name_name,
        [OP_SLICE_GET]      = &&lbl_slice_get,
        [OP_DEFINE_STRUCT]  = &&lbl_define_struct,
        [OP_FIELD_GET]      = &&lbl_field_get,
        [OP_FIELD_SET]      = &&lbl_field_set,
        [OP_CHECK_SHAPE]    = &&lbl_check_shape,
        [OP_UNPACK]         = &&lbl_unpack,
        [OP_ITER_NEXT]      = &&lbl_iter_next,
        [OP_ITER_NEXT_PAIR] = &&lbl_iter_next_pair,
        [OP_ITER_RANGE]     = &&lbl_iter_range,
        [OP_PRINT_REPL]     = &&lbl_print_repl,
        [OP_POP]            = &&lbl_pop,
        [OP_TO_STR]         = &&lbl_to_str,
        [OP_CAST]           = &&lbl_cast,
        [OP_HALT]           = &&lbl_halt,
#ifdef AER_V3
        [OP_V3_LOADK]       = &&lbl_v3_loadk,
        [OP_V3_MOVE]        = &&lbl_v3_move,
        [OP_V3_BINARY]      = &&lbl_v3_binary,
        [OP_V3_JUMP_IF_FALSE_REG] = &&lbl_v3_jump_if_false_reg,
        [OP_V3_CMP_JUMP_FALSE]    = &&lbl_v3_cmp_jump_false,
        [OP_V3_CALL]              = &&lbl_v3_call,
        [OP_V3_CALL_MODULE]       = &&lbl_v3_call_module,
        [OP_V3_CALL_BUILTIN]      = &&lbl_v3_call_builtin,
        [OP_V3_LOAD_GLOBAL]       = &&lbl_v3_load_global,
        [OP_V3_DEFER_PUSH]        = &&lbl_v3_defer_push,
        [OP_V3_RETURN]            = &&lbl_v3_return,
        [OP_V3_ARRAY_NEW]         = &&lbl_v3_array_new,
        [OP_V3_INDEX_GET]         = &&lbl_v3_index_get,
        [OP_V3_INDEX_SET]         = &&lbl_v3_index_set,
        [OP_V3_DICT_NEW]          = &&lbl_v3_dict_new,
        [OP_V3_ITER_NEXT_ARRAY]   = &&lbl_v3_iter_next_array,
        [OP_V3_ITER_RANGE]        = &&lbl_v3_iter_range,
        [OP_V3_STRUCT_NEW]        = &&lbl_v3_struct_new,
        [OP_V3_FIELD_GET]         = &&lbl_v3_field_get,
        [OP_V3_FIELD_SET]         = &&lbl_v3_field_set,
        [OP_V3_UNARY]             = &&lbl_v3_unary,
        [OP_V3_CAST]              = &&lbl_v3_cast,
        [OP_V3_BINARY_FIELD]      = &&lbl_v3_binary_field,
        [OP_V3_FIELD_BINARY]      = &&lbl_v3_field_binary,
#endif
    };

    DISPATCH();

lbl_push:
    PUSH(c->pool[READ()]);
    DISPATCH();

lbl_dup_n: {
    int n = READ();
    /* Bounds check (not just trusting the parser): this stays reachable from arbitrary chain
       depth, so a corrupt/adversarial chunk shouldn't be able to read below the stack base. */
    if (vm->stack_top < n) { error("Stack underflow"); DISPATCH(); }
    int base = vm->stack_top - n;
    for (int i = 0; i < n; i++) PUSH(vm->stack[base + i]);
    DISPATCH();
}

lbl_load: {
    int idx       = READ();
    int cache_idx = READ();
    /* vm_resolve_name_cached tries the global absolute-pointer cache, then a fresh global lookup — see its comment. */
    AerVal* found = vm_resolve_name_cached(vm, c, idx, cache_idx);
    PUSH(found ? *found : aer_null());
    DISPATCH();
}

lbl_store: {
    int idx       = READ();
    int cache_idx = READ();
    AerVal val = POP();
    AerVal* found = vm_resolve_name_full(vm, c, (unsigned int)idx, cache_idx);
    if (found) *found = val;
    else       vm_scope_define(vm, (unsigned int)idx, val);
    DISPATCH();
}

lbl_define: {
    int idx = READ();
    AerVal val = POP();
    vm_scope_define(vm, (unsigned int)idx, val);
    DISPATCH();
}

lbl_load_local: {
    int slot = READ();
    PUSH(vm_read_local_slot(vm, slot));
    DISPATCH();
}

lbl_store_local: {
    int slot = READ();
    AerVal val = POP();
    vm_write_local_slot(vm, slot, val);
    DISPATCH();
}

/* First assignment to a name in this function — allocates/reinitializes its slot. Runs at the
   current call's own base scope, same as lbl_load_local/store_local (see vm_scope_floor). Sets
   .name (not just .val) so a call site or defer replay can still find this local by name —
   see OP_DEFINE_LOCAL's comment in vm.h. */
lbl_define_local: {
    int slot = READ();
    int name_idx = READ();
    AerScope* s = &vm->scopes[vm_scope_floor(vm)];
    s->slots[slot].val  = POP();
    s->slots[slot].name = (unsigned int)name_idx;
    if (slot >= (int)s->count) s->count = (unsigned int)slot + 1;
    DISPATCH();
}

/* Fused `name OP= const` (e.g. `i += 1`) — see OP_COMPOUND_*'s comment in vm.h. Stack-neutral,
   matching the unfused 4-opcode form's net-zero effect. The runtime_had_error check after each
   fallible step replaces the intervening DISPATCH() the unfused form used to abort cleanly. */
lbl_compound_name_const: {
    int lhs_name_idx  = READ();
    int lhs_cache_idx = READ();
    Opcode bin_op     = (Opcode)READ();
    int rhs_pool_idx  = READ();
    AerVal* lhs_addr = vm_resolve_name_cached(vm, c, lhs_name_idx, lhs_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal result = vm_binary(*lhs_addr, c->pool[rhs_pool_idx], bin_op);
    if (runtime_had_error) DISPATCH();
    *lhs_addr = result;
    DISPATCH();
}

/* Fused `name OP= name` (e.g. `total += i`) — same discipline as lbl_compound_name_const, RHS resolved by name; reads both operands before writing, correct for self-referential `x += x`. */
lbl_compound_name_name: {
    int lhs_name_idx  = READ();
    int lhs_cache_idx = READ();
    Opcode bin_op     = (Opcode)READ();
    int rhs_name_idx  = READ();
    int rhs_cache_idx = READ();
    AerVal* lhs_addr = vm_resolve_name_cached(vm, c, lhs_name_idx, lhs_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal* rhs_addr = vm_resolve_name_cached(vm, c, rhs_name_idx, rhs_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal result = vm_binary(*lhs_addr, *rhs_addr, bin_op);
    if (runtime_had_error) DISPATCH();
    *lhs_addr = result;
    DISPATCH();
}

/* Phase 2 — the 4 remaining compound-assignment shapes. A LOCAL operand can't fail to resolve;
   reads/writes go through vm_read_local_slot/vm_write_local_slot, matching lbl_load_local/
   lbl_store_local. */
lbl_compound_local_const: {
    int lhs_slot     = READ();
    Opcode bin_op    = (Opcode)READ();
    int rhs_pool_idx = READ();
    AerVal lhs_val = vm_read_local_slot(vm, lhs_slot);
    AerVal result = vm_binary(lhs_val, c->pool[rhs_pool_idx], bin_op);
    if (runtime_had_error) DISPATCH();
    vm_write_local_slot(vm, lhs_slot, result);
    DISPATCH();
}

lbl_compound_local_local: {
    int lhs_slot  = READ();
    Opcode bin_op = (Opcode)READ();
    int rhs_slot  = READ();
    /* Both read before either is written — matches the self-referential `p += p` case. */
    AerVal lhs_val = vm_read_local_slot(vm, lhs_slot);
    AerVal rhs_val = vm_read_local_slot(vm, rhs_slot);
    AerVal result = vm_binary(lhs_val, rhs_val, bin_op);
    if (runtime_had_error) DISPATCH();
    vm_write_local_slot(vm, lhs_slot, result);
    DISPATCH();
}

lbl_compound_local_name: {
    int lhs_slot      = READ();
    Opcode bin_op     = (Opcode)READ();
    int rhs_name_idx  = READ();
    int rhs_cache_idx = READ();
    AerVal lhs_val = vm_read_local_slot(vm, lhs_slot);
    AerVal* rhs_addr = vm_resolve_name_cached(vm, c, rhs_name_idx, rhs_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal result = vm_binary(lhs_val, *rhs_addr, bin_op);
    if (runtime_had_error) DISPATCH();
    vm_write_local_slot(vm, lhs_slot, result);
    DISPATCH();
}

lbl_compound_name_local: {
    int lhs_name_idx  = READ();
    int lhs_cache_idx = READ();
    Opcode bin_op     = (Opcode)READ();
    int rhs_slot      = READ();
    AerVal* lhs_addr = vm_resolve_name_cached(vm, c, lhs_name_idx, lhs_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal rhs_val = vm_read_local_slot(vm, rhs_slot);
    AerVal result = vm_binary(*lhs_addr, rhs_val, bin_op);
    if (runtime_had_error) DISPATCH();
    *lhs_addr = result;
    DISPATCH();
}

/* Fused plain binary expression (`a + b`, not an assignment) — see OP_BINARY_*'s comment in vm.h.
   No write-back (unlike OP_COMPOUND_*), just pushes the result, so this follows lbl_binary's
   discipline (below) rather than the compound handlers': no runtime_had_error check needed before
   the push itself, since a transient stack value about to be discarded when the statement aborts
   at the next DISPATCH() isn't corrupting any lasting state. A NAME operand's resolution can still
   fail outright (undefined name), which *does* need checking before dereferencing it. */
lbl_binary_local_local: {
    int lhs_slot  = READ();
    Opcode bin_op = (Opcode)READ();
    int rhs_slot  = READ();
    AerVal lhs_val = vm_read_local_slot(vm, lhs_slot);
    AerVal rhs_val = vm_read_local_slot(vm, rhs_slot);
    PUSH(vm_binary(lhs_val, rhs_val, bin_op));
    DISPATCH();
}

lbl_binary_local_const: {
    int lhs_slot     = READ();
    Opcode bin_op    = (Opcode)READ();
    int rhs_pool_idx = READ();
    AerVal lhs_val = vm_read_local_slot(vm, lhs_slot);
    PUSH(vm_binary(lhs_val, c->pool[rhs_pool_idx], bin_op));
    DISPATCH();
}

lbl_binary_local_name: {
    int lhs_slot      = READ();
    Opcode bin_op     = (Opcode)READ();
    int rhs_name_idx  = READ();
    int rhs_cache_idx = READ();
    AerVal lhs_val = vm_read_local_slot(vm, lhs_slot);
    AerVal* rhs_addr = vm_resolve_name_cached(vm, c, rhs_name_idx, rhs_cache_idx);
    if (runtime_had_error) DISPATCH();
    PUSH(vm_binary(lhs_val, *rhs_addr, bin_op));
    DISPATCH();
}

lbl_binary_name_local: {
    int lhs_name_idx  = READ();
    int lhs_cache_idx = READ();
    Opcode bin_op     = (Opcode)READ();
    int rhs_slot      = READ();
    AerVal* lhs_addr = vm_resolve_name_cached(vm, c, lhs_name_idx, lhs_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal rhs_val = vm_read_local_slot(vm, rhs_slot);
    PUSH(vm_binary(*lhs_addr, rhs_val, bin_op));
    DISPATCH();
}

lbl_binary_name_const: {
    int lhs_name_idx  = READ();
    int lhs_cache_idx = READ();
    Opcode bin_op     = (Opcode)READ();
    int rhs_pool_idx  = READ();
    AerVal* lhs_addr = vm_resolve_name_cached(vm, c, lhs_name_idx, lhs_cache_idx);
    if (runtime_had_error) DISPATCH();
    PUSH(vm_binary(*lhs_addr, c->pool[rhs_pool_idx], bin_op));
    DISPATCH();
}

lbl_binary_name_name: {
    int lhs_name_idx  = READ();
    int lhs_cache_idx = READ();
    Opcode bin_op     = (Opcode)READ();
    int rhs_name_idx  = READ();
    int rhs_cache_idx = READ();
    AerVal* lhs_addr = vm_resolve_name_cached(vm, c, lhs_name_idx, lhs_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal* rhs_addr = vm_resolve_name_cached(vm, c, rhs_name_idx, rhs_cache_idx);
    if (runtime_had_error) DISPATCH();
    PUSH(vm_binary(*lhs_addr, *rhs_addr, bin_op));
    DISPATCH();
}

/* Compound-assignment fusion for `arr[idx].field OP= rhs` — see OP_COMPOUND_INDEXED_FIELD_LOCAL_
   LOCAL's comment in vm.h. rhs is already computed and on the stack (parse_assignment emits the
   rhs expression before this opcode, same as every OP_COMPOUND_* handler); arr/idx are re-read
   directly from their local slots rather than duplicated on the stack ahead of time — no DUP_N.
   Shares vm_index_get_compute with lbl_index_get (to resolve arr[idx] into the struct instance)
   and the same field linear-scan lbl_field_get/lbl_field_set use (pool-index equality, no strcmp). */
lbl_compound_indexed_field_local_local: {
    int arr_slot      = READ();
    int idx_slot      = READ();
    int field_idx     = READ();
    Opcode bin_op     = (Opcode)READ();
    AerVal rhs_val = POP();
    AerVal arr_val = vm_read_local_slot(vm, arr_slot);
    AerVal idx_val = vm_read_local_slot(vm, idx_slot);
    AerVal obj = vm_index_get_compute(arr_val, idx_val);
    if (runtime_had_error) DISPATCH();
    if (aer_type(obj) != TYPE_ARRAY || !aer_as_array(obj)->shape) {
        error("'.' field access requires a struct instance"); DISPATCH();
    }
    AerArray* oa = aer_as_array(obj);
    Shape* shape = oa->shape;
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == (unsigned int)field_idx) {
            AerVal result = vm_binary(oa->items[i], rhs_val, bin_op);
            if (runtime_had_error) DISPATCH();
            gc_barrier_array(oa, result);
            oa->items[i] = result;
            DISPATCH();
        }
    }
    error("'%s' has no field '%s'", aer_as_string(c->pool[shape->name])->data,
          aer_as_string(c->pool[field_idx])->data);
    DISPATCH();
}

/* All 18 binary/comparison/logical/bitwise ops share one handler.
   cur_op holds whichever opcode was dispatched here. */
lbl_binary: {
    AerVal b = POP(), a = POP();
    PUSH(vm_binary(a, b, cur_op));
    DISPATCH();
}

lbl_negate: {
    AerVal v = POP();
    if      (aer_type(v) == TYPE_INTEGER) PUSH(aer_int(-aer_as_int(v)));
    else if (aer_type(v) == TYPE_REAL)    PUSH(aer_real(-aer_as_real(v)));
    else { error("Negation requires a numeric type"); PUSH(aer_null()); }
    DISPATCH();
}

lbl_not: {
    AerVal v = POP();
    PUSH(aer_bool(!vm_truthy(v)));
    DISPATCH();
}

lbl_bitwise_not: {
    AerVal v = POP();
    if (aer_type(v) != TYPE_INTEGER) { error("Bitwise NOT requires an integer"); PUSH(aer_null()); }
    else PUSH(aer_int(~aer_as_int(v)));
    DISPATCH();
}

lbl_jump: {
    int target = READ();
    vm->ip = (unsigned int)target;
    DISPATCH();
}

lbl_jump_if_false: {
    int target = READ();
    if (!vm_truthy(POP())) vm->ip = (unsigned int)target;
    DISPATCH();
}

lbl_jump_if_true: {
    int target = READ();
    if (vm_truthy(POP())) vm->ip = (unsigned int)target;
    DISPATCH();
}

/* Fused comparison + conditional branch — see OP_CMP_JUMP_FALSE's comment in vm.h. Same
   type-dispatch and error behavior as lbl_binary + lbl_jump_if_false's unfused pair: on an
   error (vm_binary calls error()), vm_truthy of whatever dummy value it returned decides an
   irrelevant, never-observed branch, and DISPATCH() aborts before that matters — identical
   discipline to lbl_binary's own PUSH-then-DISPATCH. */
lbl_cmp_jump_false: {
    int cmp_op = READ();
    int target = READ();
    AerVal b = POP(), a = POP();
    if (!vm_truthy(vm_binary(a, b, (Opcode)cmp_op))) vm->ip = (unsigned int)target;
    DISPATCH();
}

/* One per call now, not one per if/for block (see parser.c's current_locals) — VM_SCOPE_MAX is
   sized off VM_CALL_MAX accordingly, so this can only run out of room alongside the call stack. */
lbl_push_scope: {
    if (vm->scope_depth >= VM_SCOPE_MAX) { error("Scope stack overflow"); return false; }
    AerScope* ns = &vm->scopes[vm->scope_depth++];
    ns->count    = 0;
    ns->overflow = false;
    ns->map      = (HashMap){0};
    DISPATCH();
}

lbl_pop_scope:
    if (vm->scope_depth <= 1) { error("Scope stack underflow"); return false; }
    --vm->scope_depth;
    if (vm->scopes[vm->scope_depth].overflow)
        hashmap_free(&vm->scopes[vm->scope_depth].map);
    DISPATCH();

lbl_print_repl: {
    AerVal v = POP();
    if (aer_type(v) != TYPE_NULL) {
        vm_print_value(c, v, false);
        printf("\n");
    }
    DISPATCH();
}

lbl_call: {
    int name_idx   = READ();
    int arg_count  = READ();
    int cache_idx  = READ();
    AerVal* fv = c->addr_cache[cache_idx];
    if (!fv) {
        fv = vm_scope_get(vm, (unsigned int)name_idx);
        /* Only cache when the global IS the winning resolution — a local binding shadowing a same-named global must keep winning every call, not just before the cache populated. */
        AerVal* cacheable = vm_global_slot_address(vm, (unsigned int)name_idx);
        if (cacheable && cacheable == fv) c->addr_cache[cache_idx] = cacheable;
    }
    const char* name = aer_as_string(c->pool[name_idx])->data;

    /* Built-in functions */
    if (!fv) {
        if (strcmp(name, "length") == 0 && arg_count == 1) {
            AerVal a = POP();
            AerVal r;
            if      (aer_type(a) == TYPE_ARRAY)  r = aer_int((long long)aer_as_array(a)->count);
            else if (aer_type(a) == TYPE_STRING) r = aer_int((long long)aer_as_string(a)->length);
            else if (aer_type(a) == TYPE_DICT)   r = aer_int((long long)aer_as_dict(a)->map.count);
            else { error("length() requires an array, dict, or string"); r = aer_int(0); }
            PUSH(r); DISPATCH();
        }
        if (strcmp(name, "delete") == 0 && arg_count == 2) {
            AerVal key = POP();
            AerVal obj = POP();
            if (aer_type(obj) == TYPE_DICT) {
                if (aer_type(key) != TYPE_STRING) { error("delete() key must be a string"); PUSH(aer_null()); DISPATCH(); }
                AerString* ks = aer_as_string(key);
                unsigned int klen = ks->length;
                if (klen > VM_KEY_MAX) { error("Dict key too long (max %d bytes)", VM_KEY_MAX); PUSH(aer_null()); DISPATCH(); }
                char kbuf[VM_KEY_MAX + 1];
                memcpy(kbuf, ks->data, klen);
                kbuf[klen] = '\0';
                dictmap_remove(&aer_as_dict(obj)->map, kbuf);
                PUSH(obj); DISPATCH();
            }
            if (aer_type(obj) == TYPE_ARRAY) {
                AerArray* a = aer_as_array(obj);
                if (a->shape) { error("delete() cannot remove fields from a struct instance — structs have a fixed shape"); PUSH(aer_null()); DISPATCH(); }
                if (aer_type(key) != TYPE_INTEGER) { error("Array delete() index must be an integer"); PUSH(aer_null()); DISPATCH(); }
                long long i = aer_as_int(key);
                if (i < 0) i += (long long)a->count;
                if (i < 0 || (unsigned long long)i >= a->count) { error("Array index %lld out of bounds (len %u)", aer_as_int(key), a->count); PUSH(aer_null()); DISPATCH(); }
                /* Shift-based, preserves order; overlapping ranges need memmove not memcpy (deleting the last element is a harmless zero-length copy). */
                memmove(&a->items[i], &a->items[i + 1], (size_t)(a->count - (unsigned long long)i - 1) * sizeof(AerVal));
                a->count--;
                PUSH(obj); DISPATCH();
            }
            error("delete() requires a dict or array");
            PUSH(aer_null()); DISPATCH();
        }
        if (strcmp(name, "append") == 0 && arg_count == 2) {
            AerVal val = POP();
            AerVal arr = POP();
            if (aer_type(arr) != TYPE_ARRAY) { error("append() requires an array"); PUSH(aer_null()); DISPATCH(); }
            AerArray* a = aer_as_array(arr);
            if (a->shape) { error("append() cannot add fields to a struct instance — structs have a fixed shape"); PUSH(aer_null()); DISPATCH(); }
            if (a->count >= a->capacity) {
                a->capacity = a->capacity ? a->capacity * 2 : 4;
                a->items = xrealloc(a->items, sizeof(AerVal) * a->capacity);
            }
            gc_barrier_array(a, val);
            a->items[a->count++] = val;
            PUSH(arr); DISPATCH();
        }
        if (strcmp(name, "print") == 0 && arg_count == 1) {
            vm_print_value(c, POP(), false);
            printf("\n");
            PUSH(aer_null()); DISPATCH();
        }
        if (strcmp(name, "type") == 0 && arg_count == 1) {
            AerVal v = POP();
            const char* tn = vm_type_name(c, v);
            /* Copies — see vm_call_builtin's identical type() case. */
            unsigned int tn_len = (unsigned int)strlen(tn);
            char* tn_buf = xmalloc(tn_len + 1);
            memcpy(tn_buf, tn, tn_len + 1);
            PUSH(aer_make_string(tn_buf, tn_len)); DISPATCH();   /* no chunk_add_pool interning — see vm_to_str's comment */
        }
        if (strcmp(name, "assert") == 0 && arg_count == 2) {
            AerVal msg  = POP();
            AerVal cond = POP();
            if (aer_type(msg) != TYPE_STRING) {
                error("assert() requires a string message as its second argument");
                PUSH(aer_null()); DISPATCH();
            }
            /* Deliberately skips error()/runtime_had_error — a failed assertion reports and execution keeps running, unlike every other runtime fault. */
            if (!vm_truthy(cond)) {
                assert_failure_count++;
                AerString* ms = aer_as_string(msg);
                printf("ASSERT FAILED: %.*s\n", (int)ms->length, ms->data);
            }
            PUSH(aer_null()); DISPATCH();
        }
        if (strcmp(name, "panic") == 0 && arg_count == 1) {
            AerVal msg = POP();
            if (aer_type(msg) != TYPE_STRING) {
                error("panic() requires a string message");
                PUSH(aer_null()); DISPATCH();
            }
            /* error() sets runtime_had_error — DISPATCH()'s check aborts on the next dispatch, same path as any other runtime fault. */
            {
                AerString* ms = aer_as_string(msg);
                error("panic: %.*s", (int)ms->length, ms->data);
            }
            PUSH(aer_null()); DISPATCH();
        }

        /* Not a variable, not a builtin — instantiation: Point(1, 2). Fewer args than fields is allowed (trailing fields take defaults); more is not. */
        Shape* shape = chunk_find_shape(c, name);
        if (shape) {
            if ((unsigned int)arg_count > shape->field_count) {
                error("'%s' takes at most %u argument%s, got %d",
                      name, shape->field_count, shape->field_count == 1 ? "" : "s", arg_count);
                for (int i = 0; i < arg_count; i++) POP();
                PUSH(aer_null()); DISPATCH();
            }
            /* struct_pool cell holds the AerArray header AND its field storage in one allocation —
               items points right after the header instead of a separate xmalloc (see vm_pools_init_once). */
            AerArray* a = pool_alloc(&struct_pool);
            a->count = a->capacity = shape->field_count;
            a->items = (AerVal*)((char*)a + sizeof(AerArray));
            a->shape = shape;
            for (int i = arg_count - 1; i >= 0; i--) a->items[i] = POP();
            for (unsigned int i = (unsigned int)arg_count; i < shape->field_count; i++)
                a->items[i] = vm_default_value(shape->field_defaults[i]);
            PUSH(aer_array_val(a)); DISPATCH();
        }

        error("'%s' is not defined", name);
        for (int i = 0; i < arg_count; i++) POP();
        PUSH(aer_null()); DISPATCH();
    }

    if (aer_type(*fv) != TYPE_FUNCTION) {
        error("'%s' is not a function", name);
        for (int i = 0; i < arg_count; i++) POP();
        PUSH(aer_null()); DISPATCH();
    }
    {
    AerFunction* fvf = aer_as_function(*fv);
    if (arg_count < (int)fvf->min_arity || arg_count > (int)fvf->arity) {
        if (fvf->min_arity == fvf->arity)
            error("'%s' expects %u arguments, got %d", name, (unsigned int)fvf->arity, arg_count);
        else
            error("'%s' expects between %u and %u arguments, got %d", name, (unsigned int)fvf->min_arity, (unsigned int)fvf->arity, arg_count);
        for (int i = 0; i < arg_count; i++) POP();
        PUSH(aer_null()); DISPATCH();
    }
    /* Omitted trailing args get pushed here, so the callee's OP_DEFINE_LOCAL sequence still sees exactly `arity` values. */
    for (int i = arg_count; i < (int)fvf->arity; i++) PUSH(vm_default_value(fvf->defaults[i - fvf->min_arity]));
    arg_count = (int)fvf->arity;
    if (fvf->has_receiver) {
        AerVal arg0 = vm->stack[vm->stack_top - arg_count];
        if (aer_type(arg0) != TYPE_ARRAY || !aer_as_array(arg0)->shape ||
            aer_as_array(arg0)->shape->name != fvf->receiver_type) {
            error("'%s' expects its first argument to be a %s",
                  name, aer_as_string(c->pool[fvf->receiver_type])->data);
            for (int i = 0; i < arg_count; i++) POP();
            PUSH(aer_null()); DISPATCH();
        }
    }
    /* OP_TAIL_CALL shares this whole handler with OP_CALL and diverges only past this point,
       once fv is confirmed callable with matching arity/receiver. Reusing the current frame is
       only safe with no pending defers (they'd need to run before this frame is discarded, and
       there's no return value yet to hand back mid-call), so a tail call with pending defers
       just falls through to the normal stack-growing path below. */
    if (cur_op == OP_TAIL_CALL) {
        CallFrame* caller = &vm->call_stack[vm->call_depth - 1];
        if (caller->defer_count == 0) {
            int base = caller->return_scope_depth;
            while (vm->scope_depth > base) {
                --vm->scope_depth;
                if (vm->scopes[vm->scope_depth].overflow)
                    hashmap_free(&vm->scopes[vm->scope_depth].map);
            }
            caller->function = fvf;
            vm->ip = fvf->code_offset;
            DISPATCH();
        }
    }
    if (vm->call_depth >= VM_CALL_MAX) { error("Call stack overflow"); return false; }
    {
        CallFrame* frame = &vm->call_stack[vm->call_depth];
        frame->return_ip          = vm->ip;
        frame->return_scope_depth = vm->scope_depth;
        frame->function           = fvf;
        /* See vm_setup_call's identical reset — reused call_stack slots must never inherit a previous occupant's pending defers. */
        frame->defer_count        = 0;
        frame->defers_draining    = false;
    }
    vm->call_depth++;
    vm->ip = fvf->code_offset;
    DISPATCH();
    }
}

lbl_return: {
    /* Address of this OP_RETURN itself (vm->ip already advanced past it); used as a deferred call's return_ip so its own OP_RETURN lands back here to check for further pending defers. */
    unsigned int return_addr = vm->ip - 1;
    CallFrame* frame = &vm->call_stack[vm->call_depth - 1];
    AerVal popped = POP();
    if (!frame->defers_draining) {
        /* First visit for this call: `popped` is the real return value. */
        frame->pending_return_value = popped;
        frame->defers_draining      = true;
    }
    /* Otherwise `popped` is a just-finished deferred call's own result, discarded here. */

    while (frame->defer_count > 0) {
        DeferredCall dc = frame->defers[--frame->defer_count];
        const char* dname = aer_as_string(c->pool[dc.name_idx])->data;
        AerVal* dfv = vm_scope_get(vm, dc.name_idx);

        if (dfv) {
            if (aer_type(*dfv) != TYPE_FUNCTION) {
                error("'%s' is not a function", dname);
            } else {
                for (int i = 0; i < dc.arg_count; i++) PUSH(dc.args[i]);
                AerVal* dargs = &vm->stack[vm->stack_top - dc.arg_count];
                if (vm_setup_call(vm, c, *dfv, dc.arg_count, dargs, return_addr)) {
                    /* Resumes inside the deferred call's bytecode; when it returns, execution re-enters lbl_return for THIS frame, rechecking defer_count. */
                    DISPATCH();
                }
                for (int i = 0; i < dc.arg_count; i++) POP();   /* vm_setup_call failed; error() already called */
            }
        } else {
            AerVal result;
            bool handled = vm_call_builtin(c, dname, dc.args, dc.arg_count, &result);
            if (!handled) error("'%s' is not defined", dname);
        }
        if (runtime_had_error) DISPATCH();   /* uniform abort, same as every other error path */
    }

    int base = frame->return_scope_depth;
    while (vm->scope_depth > base) {
        --vm->scope_depth;
        if (vm->scopes[vm->scope_depth].overflow)
            hashmap_free(&vm->scopes[vm->scope_depth].map);
    }
    AerVal ret = frame->pending_return_value;
    frame->defers_draining = false;
    vm->ip = vm->call_stack[--vm->call_depth].return_ip;
    PUSH(ret);
    DISPATCH();
}

lbl_defer_push: {
    int name_idx  = READ();
    int arg_count = READ();
    CallFrame* frame = &vm->call_stack[vm->call_depth - 1];
    /* arg_count > MAX_DEFER_ARGS is already rejected at parse time; this is just a defensive backstop against dc->args[] overflow. */
    if (arg_count > MAX_DEFER_ARGS) {
        error("Too many arguments to a deferred call (max %d)", MAX_DEFER_ARGS);
        for (int i = 0; i < arg_count; i++) POP();
        DISPATCH();
    }
    if (frame->defer_count >= MAX_DEFERS_PER_CALL) {
        error("Too many deferred calls in one function (max %d)", MAX_DEFERS_PER_CALL);
        for (int i = 0; i < arg_count; i++) POP();
        DISPATCH();
    }
    /* Lazily allocated, kept (not freed) once allocated, so a later call reusing this call_stack slot finds it already there. */
    if (!frame->defers) frame->defers = xmalloc(sizeof(DeferredCall) * MAX_DEFERS_PER_CALL);
    DeferredCall* dc = &frame->defers[frame->defer_count++];
    dc->name_idx  = (unsigned int)name_idx;
    dc->arg_count = arg_count;
    for (int i = arg_count - 1; i >= 0; i--) dc->args[i] = POP();
    DISPATCH();
}

lbl_call_value: {
    int arg_count = READ();
    AerVal fv = vm->stack[vm->stack_top - arg_count - 1];
    AerVal* args = &vm->stack[vm->stack_top - arg_count];
    if (!vm_setup_call(vm, c, fv, arg_count, args, vm->ip)) {
        for (int i = 0; i <= arg_count; i++) POP();
        PUSH(aer_null()); DISPATCH();
    }
    /* vm_setup_call may have pushed default values for omitted trailing params, so the
       true count now on the stack is the function's own arity, not the original arg_count. */
    int final_count = (int)aer_as_function(fv)->arity;
    /* Shift args down one slot to overwrite the function value — done after vm_setup_call since it reads args[0] (the receiver) from its original position. */
    memmove(&vm->stack[vm->stack_top - final_count - 1], args, (size_t)final_count * sizeof(AerVal));
    vm->stack_top--;
    DISPATCH();
}

lbl_call_module: {
    int module_idx = READ();
    int fn_idx     = READ();
    int arg_count  = READ();
    const char* module = aer_as_string(c->pool[module_idx])->data;
    const char* fn     = aer_as_string(c->pool[fn_idx])->data;
    bool handled = false;
    if      (strcmp(module, "math")   == 0) handled = aer_math_call(vm, c, fn, arg_count);
    else if (strcmp(module, "random") == 0) handled = aer_random_call(vm, c, fn, arg_count);
    else if (strcmp(module, "string") == 0) handled = aer_string_call(vm, c, fn, arg_count);
    else if (strcmp(module, "time")   == 0) handled = aer_time_call(vm, c, fn, arg_count);
    else if (strcmp(module, "json")   == 0) handled = aer_json_call(vm, c, fn, arg_count);
    else if (aer_host_is_module(module, (unsigned int)strlen(module)))
                                             handled = aer_host_call(vm, module, fn, arg_count);
    else                                     handled = aer_module_call(vm, module, fn, arg_count);
    if (handled) DISPATCH();
    /* The module matched but `fn` wasn't one of its functions — e.g. wrong module, or undefined at the file-module's top level. */
    error("'%s' has no function '%s'", module, fn);
    for (int i = 0; i < arg_count; i++) POP();
    PUSH(aer_null()); DISPATCH();
}

lbl_array_new: {
    int count = READ();
    AerArray* a = pool_alloc(&array_pool);
    a->capacity = count > 0 ? (unsigned int)count : 4;
    a->count    = (unsigned int)count;
    a->items    = xmalloc(sizeof(AerVal) * a->capacity);
    a->shape    = NULL;
    for (int i = count - 1; i >= 0; i--)
        a->items[i] = POP();
    PUSH(aer_array_val(a));
    DISPATCH();
}

lbl_unpack: {
    int idx = READ();
    AerVal arr = vm->stack[vm->stack_top - 1];   /* peek — do not pop */
    if (aer_type(arr) != TYPE_ARRAY) { error("Cannot unpack a non-array"); PUSH(aer_null()); DISPATCH(); }
    AerArray* a = aer_as_array(arr);
    if ((unsigned int)idx >= a->count) {
        error("Cannot unpack index %d: array has %u element%s",
              idx, a->count, a->count == 1 ? "" : "s");
        PUSH(aer_null()); DISPATCH();
    }
    PUSH(a->items[idx]);
    DISPATCH();
}

lbl_dict_new: {
    int count = READ();
    AerDict* d = pool_alloc(&dict_pool);
    memset(&d->map, 0, sizeof(d->map));
    d->map.is_inline = true;   /* AerDict stores AerVal inline, not boxed — see hashtable.h */
    /* Stack: key0, val0, key1, val1, ..., keyN-1, valN-1 (valN-1 on top) */
    for (int i = count - 1; i >= 0; i--) {
        AerVal val = POP();
        AerVal key = POP();
        if (aer_type(key) != TYPE_STRING) { error("Dict keys must be strings"); continue; }
        AerString* ks = aer_as_string(key);
        unsigned int klen = ks->length;
        char* k = xmalloc(klen + 1);
        memcpy(k, ks->data, klen);
        k[klen] = '\0';
        dictmap_put(&d->map, k, val);
    }
    PUSH(aer_dict_val(d));
    DISPATCH();
}

lbl_index_get: {
    AerVal idx = POP();
    AerVal obj = POP();
    PUSH(vm_index_get_compute(obj, idx));
    DISPATCH();
}

/* Fused `name[index]` reads. Reads both operands directly, computes via the shared
   vm_index_get_compute, then pushes the result — unlike Part 1's compound-assignment family
   this isn't stack-neutral. A NAME-kind operand can fail to resolve; check runtime_had_error
   and DISPATCH() immediately rather than indexing with garbage, same as OP_COMPOUND_NAME_*. */
lbl_index_get_local_const: {
    int arr_slot = READ();
    int idx_pool = READ();
    AerVal obj = vm_read_local_slot(vm, arr_slot);
    AerVal idx = c->pool[idx_pool];
    PUSH(vm_index_get_compute(obj, idx));
    DISPATCH();
}

lbl_index_get_local_local: {
    int arr_slot = READ();
    int idx_slot = READ();
    AerVal obj = vm_read_local_slot(vm, arr_slot);
    AerVal idx = vm_read_local_slot(vm, idx_slot);
    PUSH(vm_index_get_compute(obj, idx));
    DISPATCH();
}

lbl_index_get_local_name: {
    int arr_slot      = READ();
    int idx_name_idx  = READ();
    int idx_cache_idx = READ();
    AerVal obj = vm_read_local_slot(vm, arr_slot);
    AerVal* idx_addr = vm_resolve_name_cached(vm, c, idx_name_idx, idx_cache_idx);
    if (runtime_had_error) DISPATCH();
    PUSH(vm_index_get_compute(obj, *idx_addr));
    DISPATCH();
}

lbl_index_get_name_const: {
    int arr_name_idx  = READ();
    int arr_cache_idx = READ();
    int idx_pool      = READ();
    AerVal* obj_addr = vm_resolve_name_cached(vm, c, arr_name_idx, arr_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal idx = c->pool[idx_pool];
    PUSH(vm_index_get_compute(*obj_addr, idx));
    DISPATCH();
}

lbl_index_get_name_local: {
    int arr_name_idx  = READ();
    int arr_cache_idx = READ();
    int idx_slot      = READ();
    AerVal* obj_addr = vm_resolve_name_cached(vm, c, arr_name_idx, arr_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal idx = vm_read_local_slot(vm, idx_slot);
    PUSH(vm_index_get_compute(*obj_addr, idx));
    DISPATCH();
}

lbl_index_get_name_name: {
    int arr_name_idx  = READ();
    int arr_cache_idx = READ();
    int idx_name_idx  = READ();
    int idx_cache_idx = READ();
    AerVal* obj_addr = vm_resolve_name_cached(vm, c, arr_name_idx, arr_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal* idx_addr = vm_resolve_name_cached(vm, c, idx_name_idx, idx_cache_idx);
    if (runtime_had_error) DISPATCH();
    PUSH(vm_index_get_compute(*obj_addr, *idx_addr));
    DISPATCH();
}

lbl_index_set: {
    AerVal val = POP();
    AerVal idx = POP();
    AerVal obj = POP();
    vm_index_set_compute(obj, idx, val);
    DISPATCH();
}

/* Compile-time-fused `name[index] = <literal>` writes — mirrors OP_INDEX_GET_*'s scheme
   (see that comment in vm.h), just for the write side. All three of array/index/value are
   resolved directly from this opcode's own operands, not the stack — no separate LOAD/PUSH
   bytecode runs for any of them, so there's no intermediate point where something else could
   run and see array/index/value in a half-updated state. The value is restricted to a bare
   CONST (parser.c's parse_assignment) — see its comment for why that restriction specifically
   is what makes resolving array/index "after" the value (in this opcode, unavoidably, since it's
   one dispatch) safe. Recognized only for a single, unchained bracket assignment; a chained write
   (a.b[i] = v) or a struct field write keeps using the generic OP_INDEX_SET/OP_FIELD_SET. */
lbl_index_set_local_const: {
    int arr_slot = READ();
    int idx_pool = READ();
    int val_pool = READ();
    AerVal obj = vm_read_local_slot(vm, arr_slot);
    AerVal idx = c->pool[idx_pool];
    vm_index_set_compute(obj, idx, c->pool[val_pool]);
    DISPATCH();
}

lbl_index_set_local_local: {
    int arr_slot = READ();
    int idx_slot = READ();
    int val_pool = READ();
    AerVal obj = vm_read_local_slot(vm, arr_slot);
    AerVal idx = vm_read_local_slot(vm, idx_slot);
    vm_index_set_compute(obj, idx, c->pool[val_pool]);
    DISPATCH();
}

lbl_index_set_local_name: {
    int arr_slot      = READ();
    int idx_name_idx  = READ();
    int idx_cache_idx = READ();
    int val_pool      = READ();
    AerVal obj = vm_read_local_slot(vm, arr_slot);
    AerVal* idx_addr = vm_resolve_name_cached(vm, c, idx_name_idx, idx_cache_idx);
    if (runtime_had_error) DISPATCH();
    vm_index_set_compute(obj, *idx_addr, c->pool[val_pool]);
    DISPATCH();
}

lbl_index_set_name_const: {
    int arr_name_idx  = READ();
    int arr_cache_idx = READ();
    int idx_pool      = READ();
    int val_pool      = READ();
    AerVal* obj_addr = vm_resolve_name_cached(vm, c, arr_name_idx, arr_cache_idx);
    if (runtime_had_error) DISPATCH();
    vm_index_set_compute(*obj_addr, c->pool[idx_pool], c->pool[val_pool]);
    DISPATCH();
}

lbl_index_set_name_local: {
    int arr_name_idx  = READ();
    int arr_cache_idx = READ();
    int idx_slot      = READ();
    int val_pool      = READ();
    AerVal* obj_addr = vm_resolve_name_cached(vm, c, arr_name_idx, arr_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal idx = vm_read_local_slot(vm, idx_slot);
    vm_index_set_compute(*obj_addr, idx, c->pool[val_pool]);
    DISPATCH();
}

lbl_index_set_name_name: {
    int arr_name_idx  = READ();
    int arr_cache_idx = READ();
    int idx_name_idx  = READ();
    int idx_cache_idx = READ();
    int val_pool      = READ();
    AerVal* obj_addr = vm_resolve_name_cached(vm, c, arr_name_idx, arr_cache_idx);
    if (runtime_had_error) DISPATCH();
    AerVal* idx_addr = vm_resolve_name_cached(vm, c, idx_name_idx, idx_cache_idx);
    if (runtime_had_error) DISPATCH();
    vm_index_set_compute(*obj_addr, *idx_addr, c->pool[val_pool]);
    DISPATCH();
}

lbl_slice_get: {
    AerVal end_v   = POP();
    AerVal start_v = POP();
    AerVal obj     = POP();
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) { error("Structs cannot be sliced"); PUSH(aer_null()); DISPATCH(); }
        long long start, end;
        if (!vm_slice_bounds(start_v, end_v, (long long)a->count, &start, &end)) { PUSH(aer_null()); DISPATCH(); }
        unsigned int n = (unsigned int)(end - start);
        AerArray* r = pool_alloc(&array_pool);
        r->count    = n;
        r->capacity = n > 0 ? n : 4;
        r->items    = xmalloc(sizeof(AerVal) * r->capacity);
        r->shape    = NULL;   /* a slice is always a plain array, even of a struct */
        for (unsigned int i = 0; i < n; i++) r->items[i] = a->items[start + i];
        PUSH(aer_array_val(r));
    } else if (aer_type(obj) == TYPE_STRING) {
        AerString* os = aer_as_string(obj);
        long long start, end;
        if (!vm_slice_bounds(start_v, end_v, (long long)os->length, &start, &end)) { PUSH(aer_null()); DISPATCH(); }
        /* Copies rather than pointing into obj's own buffer, since obj could be collected later while this slice is still alive. */
        unsigned int sub_len = (unsigned int)(end - start);
        char* sub_buf = xmalloc(sub_len + 1);
        memcpy(sub_buf, os->data + start, sub_len);
        sub_buf[sub_len] = '\0';
        PUSH(aer_make_string(sub_buf, sub_len));   /* no chunk_add_pool interning — see vm_to_str's comment */
    } else {
        error("Cannot slice this type");
        PUSH(aer_null());
    }
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
    }
    if (c->shape_count >= c->shape_cap) {
        c->shape_cap = c->shape_cap ? c->shape_cap * 2 : 4;
        c->shapes = xrealloc(c->shapes, sizeof(Shape*) * c->shape_cap);
    }
    c->shapes[c->shape_count++] = shape;
    DISPATCH();
}

lbl_field_get: {
    unsigned int site = vm->ip - 1;   /* the opcode's own word — see field_cache_shape's comment */
    int field_idx = READ();
    AerVal obj = POP();
    if (aer_type(obj) != TYPE_ARRAY || !aer_as_array(obj)->shape) {
        error("'.' field access requires a struct instance");
        PUSH(aer_null()); DISPATCH();
    }
    AerArray* oa = aer_as_array(obj);
    Shape* shape = oa->shape;
    if (c->field_cache_shape[site] == shape) {
        PUSH(oa->items[c->field_cache_slot[site]]); DISPATCH();
    }
    /* field_idx and shape->field_names[i] are chunk_add_pool-deduped indices, so an identical field name always yields the identical index — comparing indices is equivalent to strcmp, without one. */
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == (unsigned int)field_idx) {
            c->field_cache_shape[site] = shape;
            c->field_cache_slot[site]  = (int)i;
            PUSH(oa->items[i]); DISPATCH();
        }
    }
    error("'%s' has no field '%s'", aer_as_string(c->pool[shape->name])->data,
          aer_as_string(c->pool[field_idx])->data);
    PUSH(aer_null()); DISPATCH();
}

lbl_field_set: {
    unsigned int site = vm->ip - 1;
    int field_idx = READ();
    AerVal val = POP();
    AerVal obj = POP();
    if (aer_type(obj) != TYPE_ARRAY || !aer_as_array(obj)->shape) {
        error("'.' field access requires a struct instance"); DISPATCH();
    }
    AerArray* oa = aer_as_array(obj);
    Shape* shape = oa->shape;
    if (c->field_cache_shape[site] == shape) {
        gc_barrier_array(oa, val);
        oa->items[c->field_cache_slot[site]] = val; DISPATCH();
    }
    /* See lbl_field_get's comment — same pool-index equivalence. */
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == (unsigned int)field_idx) {
            c->field_cache_shape[site] = shape;
            c->field_cache_slot[site]  = (int)i;
            gc_barrier_array(oa, val);
            oa->items[i] = val; DISPATCH();
        }
    }
    error("'%s' has no field '%s'", aer_as_string(c->pool[shape->name])->data,
          aer_as_string(c->pool[field_idx])->data);
    DISPATCH();
}

lbl_check_shape: {
    int name_idx = READ();
    AerVal v = POP();
    if (aer_type(v) != TYPE_ARRAY || !aer_as_array(v)->shape || aer_as_array(v)->shape->name != (unsigned int)name_idx) {
        error("Expected a '%s', got a '%s'", aer_as_string(c->pool[name_idx])->data, vm_type_name(c, v));
        PUSH(aer_null()); DISPATCH();
    }
    PUSH(v);
    DISPATCH();
}

lbl_iter_next: {
    int end_addr = READ();
    AerVal* idx_v = &vm->stack[vm->stack_top - 1];
    AerVal* col   = &vm->stack[vm->stack_top - 2];
    long long idx = aer_as_int(*idx_v);
    if (aer_type(*col) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(*col);
        if ((unsigned long long)idx >= a->count) {
            vm->stack_top -= 2; vm->ip = (unsigned int)end_addr;
        } else {
            *idx_v = aer_int(idx + 1);
            PUSH(a->items[idx]);
        }
    } else if (aer_type(*col) == TYPE_DICT) {
        AerDict* d = aer_as_dict(*col);
        while ((unsigned long long)idx < d->map.capacity && !d->map.buckets[idx].key)
            idx++;
        if ((unsigned long long)idx >= d->map.capacity) {
            vm->stack_top -= 2; vm->ip = (unsigned int)end_addr;
        } else {
            *idx_v = aer_int(idx + 1);
            /* Copies the key rather than pointing into the hashmap's buffer, since the entry could be removed/overwritten while this string is still alive. */
            unsigned int key_len = d->map.buckets[idx].length;
            char* key_buf = xmalloc(key_len + 1);
            memcpy(key_buf, d->map.buckets[idx].key, key_len);
            key_buf[key_len] = '\0';
            PUSH(aer_make_string(key_buf, key_len));   /* no chunk_add_pool interning — see vm_to_str's comment */
        }
    } else if (aer_type(*col) == TYPE_STRING) {
        AerString* cs = aer_as_string(*col);
        if ((unsigned long long)idx >= cs->length) {
            vm->stack_top -= 2; vm->ip = (unsigned int)end_addr;
        } else {
            *idx_v = aer_int(idx + 1);
            char* ch_buf = xmalloc(2);
            ch_buf[0] = cs->data[idx];
            ch_buf[1] = '\0';
            PUSH(aer_make_string(ch_buf, 1));   /* no chunk_add_pool interning — see vm_to_str's comment */
        }
    } else { error("Cannot iterate over this type"); vm->stack_top -= 2; vm->ip = (unsigned int)end_addr; }
    DISPATCH();
}

lbl_iter_next_pair: {
    int end_addr = READ();
    AerVal* idx_v = &vm->stack[vm->stack_top - 1];
    AerVal* col   = &vm->stack[vm->stack_top - 2];
    long long idx = aer_as_int(*idx_v);
    if (aer_type(*col) != TYPE_DICT) { error("for k, v requires a dict"); vm->stack_top -= 2; vm->ip = (unsigned int)end_addr; DISPATCH(); }
    AerDict* d = aer_as_dict(*col);
    while ((unsigned long long)idx < d->map.capacity && !d->map.buckets[idx].key)
        idx++;
    if ((unsigned long long)idx >= d->map.capacity) {
        vm->stack_top -= 2; vm->ip = (unsigned int)end_addr;
    } else {
        *idx_v = aer_int(idx + 1);
        /* Copies the key — see lbl_iter_next's identical comment on this pattern. */
        unsigned int key_len = d->map.buckets[idx].length;
        char* key_buf = xmalloc(key_len + 1);
        memcpy(key_buf, d->map.buckets[idx].key, key_len);
        key_buf[key_len] = '\0';
        PUSH(aer_make_string(key_buf, key_len));   /* key — no chunk_add_pool interning, see vm_to_str's comment */
        PUSH(d->map.buckets[idx].payload.inline_val);           /* value */
    }
    DISPATCH();
}

lbl_iter_range: {
    int end_addr = READ();
    /* Stack: [cur, end, step] — step is always present (parser defaults to 1), so no branch is needed. Direction is inferred from cur vs end, not the step's sign, so `10..0..2` still descends. */
    AerVal* cur_v  = &vm->stack[vm->stack_top - 3];
    AerVal* end_v  = &vm->stack[vm->stack_top - 2];
    AerVal* step_v = &vm->stack[vm->stack_top - 1];
    if (aer_type(*cur_v) != TYPE_INTEGER || aer_type(*end_v) != TYPE_INTEGER || aer_type(*step_v) != TYPE_INTEGER) {
        error("Range bounds and step must be integers"); vm->stack_top -= 3; vm->ip = (unsigned int)end_addr; DISPATCH();
    }
    long long cur = aer_as_int(*cur_v), rng_end = aer_as_int(*end_v), step = aer_as_int(*step_v);
    if (step <= 0) {
        error("Range step must be a positive integer (direction is inferred from the bounds, not the step's sign)");
        vm->stack_top -= 3; vm->ip = (unsigned int)end_addr; DISPATCH();
    }
    bool ascending = cur < rng_end;
    /* >= / <= rather than == : a step that doesn't evenly divide the range must still stop cleanly instead of stepping past rng_end and never hitting it exactly. */
    if (ascending ? (cur >= rng_end) : (cur <= rng_end)) {
        vm->stack_top -= 3; vm->ip = (unsigned int)end_addr;
    } else {
        PUSH(*cur_v);
        *cur_v = aer_int(cur + (ascending ? step : -step));
    }
    DISPATCH();
}

lbl_to_str: {
    /* Not PUSH(vm_to_str(vm, POP())) directly — PUSH's macro body and POP() would both modify vm->stack_top within one unsequenced expression (a real -Wsequence-point issue). */
    AerVal v = POP();
    PUSH(vm_to_str(vm, v));
    DISPATCH();
}

lbl_cast: {
    int cast_type = READ();
    AerVal v = POP();
    PUSH(vm_cast(v, cast_type));
    DISPATCH();
}

lbl_pop:
    POP();
    DISPATCH();

#ifdef AER_V3
/* v3 register-VM prototype, M1 — see OP_V3_*'s comment in vm.h. Stack-neutral: none of these three
   touch vm->stack/vm->scopes/PUSH/POP at all, only v3_registers[] and (for LOADK) the chunk pool —
   the whole point of keeping this genuinely isolated from the live interpreter. */
lbl_v3_loadk: {
    int dest = (int)V3_UNPACK_A(op_word);
    int pool_idx = READ();
    v3_registers[dest] = c->pool[pool_idx];
    DISPATCH();
}

lbl_v3_move: {
    int dest = (int)V3_UNPACK_A(op_word);
    int src  = (int)V3_UNPACK_B(op_word);
    v3_registers[dest] = v3_registers[src];
    DISPATCH();
}

/* dest/bin_op are packed into op_word (see V3_PACK3's comment in vm.h); rk_b/rk_c are wide
   RK-encoded operands (register or constant), each still its own dedicated word, read via
   READ() exactly as before this opcode was packed. */
lbl_v3_binary: {
    int dest      = (int)V3_UNPACK_A(op_word);
    Opcode bin_op = (Opcode)V3_UNPACK_B(op_word);
    int rk_b      = READ();
    int rk_c      = READ();
    AerVal b = vm_v3_rk_value(c, rk_b);
    AerVal cc = vm_v3_rk_value(c, rk_c);
    v3_registers[dest] = vm_binary(b, cc, bin_op);
    DISPATCH();
}

/* M2 — control flow. Stack-neutral, same as the M1 opcodes above — reads v3_registers[]/the chunk
   pool only, never pops/pushes anything, and OP_JUMP (reused as-is for unconditional jumps) is
   already stack-neutral too. */
lbl_v3_jump_if_false_reg: {
    int reg    = (int)V3_UNPACK_A(op_word);
    int target = READ();
    if (!vm_truthy(v3_registers[reg])) vm->ip = (unsigned int)target;
    DISPATCH();
}

lbl_v3_cmp_jump_false: {
    Opcode cmp_op = (Opcode)V3_UNPACK_A(op_word);
    int rk_a      = READ();
    int rk_b      = READ();
    int target    = READ();
    AerVal a = vm_v3_rk_value(c, rk_a);
    AerVal b = vm_v3_rk_value(c, rk_b);
    if (!vm_truthy(vm_binary(a, b, cmp_op))) vm->ip = (unsigned int)target;
    DISPATCH();
}

/* M5 — real per-call register windowing. Bulk-copies arg_reg_base..+arg_count from the CALLER's
   bank into the NEW callee frame's bank (always starting at its own register 0 — no more fixed
   shared offset to agree on) in one dispatch, same bulk-copy mechanism M3 proved, now landing in
   an isolated frame instead of a shared fixed range. Mirrors the stack VM's own overflow check
   (vm.c's vm_setup_call: `if (target->call_depth >= VM_CALL_MAX) { error("Call stack overflow");
   ... }`) almost verbatim — same ceiling, same error-then-DISPATCH() discipline. */
lbl_v3_call: {
    int dest_reg      = (int)V3_UNPACK_A(op_word);
    int arg_reg_base  = (int)V3_UNPACK_B(op_word);
    int arg_count     = (int)V3_UNPACK_C(op_word);
    int callee_offset = READ();
    if (v3_call_depth + 1 >= VM_CALL_MAX) { error("v3 call stack overflow"); DISPATCH(); }
    V3CallFrame* caller = &v3_call_stack[v3_call_depth];
    V3CallFrame* callee = &v3_call_stack[v3_call_depth + 1];
    for (int i = 0; i < arg_count; i++)
        callee->registers[i] = caller->registers[arg_reg_base + i];
    callee->return_ip   = vm->ip;   /* already past this instruction's operands — the correct resume point */
    callee->dest_reg    = dest_reg;
    callee->defer_count = 0;   /* reused call_stack slots must never inherit a previous occupant's pending defers */
    v3_call_depth++;
    v3_registers = v3_call_stack[v3_call_depth].registers;
    vm->ip = (unsigned int)callee_offset;
    DISPATCH();
}

/* src_reg is a plain 0-based index into the CALLEE's own frame — no more "absolute index into a
   shared fixed bank" caveat, since every call now owns an isolated bank. return_ip/dest_reg live
   in the callee's own frame (not a single shared global), which is exactly what makes nested/
   recursive calls safe: an outer call's return info can't be clobbered by an inner one.
     M5 slice 12 — before actually returning, drains this frame's pending defers LIFO, one per
   visit: each is set up as an inline call into a fresh, deeper v3_call_stack slot whose return_ip
   points back at THIS SAME OP_V3_RETURN instruction (reenter_addr, computed the same way the
   stack VM's lbl_return computes its own self-re-entry address), so the deferred call's own
   eventual OP_V3_RETURN re-dispatches here and rechecks defer_count, same LIFO-drain-before-
   real-return shape as lbl_return's while loop. Simpler than the stack VM's version: since every
   v3 call already owns an isolated register bank, this frame's `result` (once finally read, after
   defer_count reaches 0) can't be clobbered by a deferred call's own return value the way a
   shared value-stack slot could — no pending_return_value/defers_draining flag needed. */
lbl_v3_return: {
    int src_reg = (int)V3_UNPACK_A(op_word);
    unsigned int reenter_addr = vm->ip - 1;   /* this OP_V3_RETURN's own address: now one packed word (opcode + src_reg together) instead of two */
    V3CallFrame* callee = &v3_call_stack[v3_call_depth];

    if (callee->defer_count > 0) {
        if (v3_call_depth + 1 >= VM_CALL_MAX) { error("v3 call stack overflow"); DISPATCH(); }
        V3DeferredCall dc = callee->defers[--callee->defer_count];
        V3CallFrame* next = &v3_call_stack[v3_call_depth + 1];
        for (int i = 0; i < dc.arg_count; i++) next->registers[i] = dc.args[i];
        next->return_ip   = reenter_addr;
        next->dest_reg    = -1;   /* sentinel: a deferred call's own return value is always discarded, never written anywhere (register 0 may be a live variable in the frame it would otherwise land in) */
        next->defer_count = 0;
        v3_call_depth++;
        v3_registers = v3_call_stack[v3_call_depth].registers;
        vm->ip = dc.callee_offset;
        DISPATCH();
    }

    AerVal result = callee->registers[src_reg];
    unsigned int return_ip = callee->return_ip;
    int dest_reg = callee->dest_reg;
    v3_call_depth--;
    v3_registers = v3_call_stack[v3_call_depth].registers;
    if (dest_reg >= 0) v3_registers[dest_reg] = result;   /* dest_reg == -1: a deferred call's discarded result */
    vm->ip = return_ip;
    DISPATCH();
}

/* See OP_V3_CALL_MODULE's comment in vm.h — bridges to the exact same stack-based stdlib dispatch
   lbl_call_module uses, since reimplementing every stdlib function for registers would be pure
   duplication. */
lbl_v3_call_module: {
    int dest_reg     = (int)V3_UNPACK_A(op_word);
    int arg_reg_base = (int)V3_UNPACK_B(op_word);
    int arg_count    = (int)V3_UNPACK_C(op_word);
    int module_idx   = READ();
    int fn_idx       = READ();
    const char* module = aer_as_string(c->pool[module_idx])->data;
    const char* fn     = aer_as_string(c->pool[fn_idx])->data;
    for (int i = 0; i < arg_count; i++) PUSH(v3_registers[arg_reg_base + i]);
    bool handled = false;
    if      (strcmp(module, "math")   == 0) handled = aer_math_call(vm, c, fn, arg_count);
    else if (strcmp(module, "random") == 0) handled = aer_random_call(vm, c, fn, arg_count);
    else if (strcmp(module, "string") == 0) handled = aer_string_call(vm, c, fn, arg_count);
    else if (strcmp(module, "time")   == 0) handled = aer_time_call(vm, c, fn, arg_count);
    else if (strcmp(module, "json")   == 0) handled = aer_json_call(vm, c, fn, arg_count);
    else if (aer_host_is_module(module, (unsigned int)strlen(module)))
                                             handled = aer_host_call(vm, module, fn, arg_count);
    else                                     handled = aer_module_call(vm, module, fn, arg_count);
    if (handled) { v3_registers[dest_reg] = POP(); DISPATCH(); }
    error("'%s' has no function '%s'", module, fn);
    for (int i = 0; i < arg_count; i++) POP();
    v3_registers[dest_reg] = aer_null();
    DISPATCH();
}

/* See OP_V3_CALL_BUILTIN's comment in vm.h — vm_call_builtin() already takes a plain AerVal*
   array (built for deferred-call replay, vm.c above), so unlike OP_V3_CALL_MODULE this needs no
   push/pop bridge to vm->stack at all. 4 local slots is headroom over every builtin's real max
   arity (2 — delete/append/assert). */
lbl_v3_call_builtin: {
    int dest_reg     = (int)V3_UNPACK_A(op_word);
    int arg_reg_base = (int)V3_UNPACK_B(op_word);
    int arg_count    = (int)V3_UNPACK_C(op_word);
    int name_idx     = READ();
    const char* name = aer_as_string(c->pool[name_idx])->data;
    if (arg_count > 4) {
        error("Too many arguments to '%s'", name);
        v3_registers[dest_reg] = aer_null();
        DISPATCH();
    }
    AerVal args[4];
    for (int i = 0; i < arg_count; i++) args[i] = v3_registers[arg_reg_base + i];
    AerVal out;
    bool handled = vm_call_builtin(c, name, args, arg_count, &out);
    if (!handled) error("'%s' is not defined, or was called with the wrong number of arguments", name);
    v3_registers[dest_reg] = out;
    DISPATCH();
}

/* See OP_V3_LOAD_GLOBAL's comment in vm.h — always reads frame 0 directly, never the currently
   active frame, since a function's own registers are a completely separate bank from the
   top-level's. */
lbl_v3_load_global: {
    int dest_reg   = (int)V3_UNPACK_A(op_word);
    int global_reg = (int)V3_UNPACK_B(op_word);
    v3_registers[dest_reg] = v3_call_stack[0].registers[global_reg];
    DISPATCH();
}

/* See V3DeferredCall's comment (above) and OP_V3_DEFER_PUSH's (vm.h) — snapshots arg_count
   register values into the CURRENT frame's own deferred-call list now; lbl_v3_return drains this
   list LIFO before the frame actually returns. Overflow checks mirror the stack VM's
   lbl_defer_push exactly (arg_count is already rejected at parse time by v3_parse_defer, this is
   just a defensive backstop). */
lbl_v3_defer_push: {
    int arg_reg_base  = (int)V3_UNPACK_A(op_word);
    int arg_count     = (int)V3_UNPACK_B(op_word);
    int callee_offset = READ();
    V3CallFrame* frame = &v3_call_stack[v3_call_depth];
    if (arg_count > MAX_DEFER_ARGS) {
        error("Too many arguments to a deferred call (max %d)", MAX_DEFER_ARGS);
        DISPATCH();
    }
    if (frame->defer_count >= MAX_DEFERS_PER_CALL) {
        error("Too many deferred calls in one function (max %d)", MAX_DEFERS_PER_CALL);
        DISPATCH();
    }
    if (!frame->defers) frame->defers = xmalloc(sizeof(V3DeferredCall) * MAX_DEFERS_PER_CALL);
    V3DeferredCall* dc = &frame->defers[frame->defer_count++];
    dc->callee_offset = (unsigned int)callee_offset;
    dc->arg_count      = arg_count;
    for (int i = 0; i < arg_count; i++) dc->args[i] = v3_registers[arg_reg_base + i];
    DISPATCH();
}

/* M4 — the register-VM analog of lbl_array_new (above): same pool_alloc/capacity/items/shape
   setup, but the source is an already-in-order register range instead of N stack pops, so the
   copy runs forward (lbl_array_new copies backward specifically to undo the stack's LIFO pop
   order — registers have no such inversion to undo). This is the first v3 opcode that puts a
   heap pointer in v3_registers[] — see mark_vm_roots's new AER_V3 block, required before this
   opcode could be safe to use across a GC cycle. */
lbl_v3_array_new: {
    int dest_reg      = (int)V3_UNPACK_A(op_word);
    int item_reg_base = (int)V3_UNPACK_B(op_word);
    int item_count    = (int)V3_UNPACK_C(op_word);
    AerArray* a = pool_alloc(&array_pool);
    a->capacity = item_count > 0 ? (unsigned int)item_count : 4;
    a->count    = (unsigned int)item_count;
    a->items    = xmalloc(sizeof(AerVal) * a->capacity);
    a->shape    = NULL;
    for (int i = 0; i < item_count; i++)
        a->items[i] = v3_registers[item_reg_base + i];
    v3_registers[dest_reg] = aer_array_val(a);
    DISPATCH();
}

/* Reuses vm_index_get_compute (above) as-is — already type-generic (array/dict/string) and
   already has all bounds/negative-index logic, so nothing about indexing itself needed
   reimplementing for the register path. */
lbl_v3_index_get: {
    int dest_reg = (int)V3_UNPACK_A(op_word);
    int arr_reg  = (int)V3_UNPACK_B(op_word);
    int rk_idx   = READ();
    AerVal idx = vm_v3_rk_value(c, rk_idx);
    v3_registers[dest_reg] = vm_index_get_compute(v3_registers[arr_reg], idx);
    DISPATCH();
}

/* Reuses vm_index_set_compute (above) as-is — including its internal gc_barrier_array/
   gc_barrier_dict call, which this is the first v3 opcode to exercise against a register-held
   reference rather than a stack-held one. */
lbl_v3_index_set: {
    int arr_reg = (int)V3_UNPACK_A(op_word);
    int rk_idx  = READ();
    int rk_val  = READ();
    AerVal idx = vm_v3_rk_value(c, rk_idx);
    AerVal val = vm_v3_rk_value(c, rk_val);
    vm_index_set_compute(v3_registers[arr_reg], idx, val);
    DISPATCH();
}

/* Mirrors lbl_dict_new (above) exactly — same pool_alloc/memset/is_inline setup, same
   key-must-be-string validation and owned-copy-of-the-key discipline — reading pairs from an
   already-in-order register range instead of popping them off the stack in reverse. */
lbl_v3_dict_new: {
    int dest_reg      = (int)V3_UNPACK_A(op_word);
    int pair_reg_base = (int)V3_UNPACK_B(op_word);
    int pair_count    = (int)V3_UNPACK_C(op_word);
    AerDict* d = pool_alloc(&dict_pool);
    memset(&d->map, 0, sizeof(d->map));
    d->map.is_inline = true;   /* AerDict stores AerVal inline, not boxed — see hashtable.h */
    for (int i = 0; i < pair_count; i++) {
        AerVal key = v3_registers[pair_reg_base + 2 * i];
        AerVal val = v3_registers[pair_reg_base + 2 * i + 1];
        if (aer_type(key) != TYPE_STRING) { error("Dict keys must be strings"); continue; }
        AerString* ks = aer_as_string(key);
        unsigned int klen = ks->length;
        char* k = xmalloc(klen + 1);
        memcpy(k, ks->data, klen);
        k[klen] = '\0';
        dictmap_put(&d->map, k, val);
    }
    v3_registers[dest_reg] = aer_dict_val(d);
    DISPATCH();
}

/* Mirrors lbl_iter_next's TYPE_ARRAY branch (above) exactly — same bounds check/advance/fetch —
   reading col/idx from registers instead of peeking the stack, and with no stack slots to pop on
   exit (see this opcode's own comment in vm.h for why). */
lbl_v3_iter_next_array: {
    int col_reg       = (int)V3_UNPACK_A(op_word);
    int idx_reg       = (int)V3_UNPACK_B(op_word);
    int item_dest_reg = (int)V3_UNPACK_C(op_word);
    int end_target    = READ();
    AerVal col = v3_registers[col_reg];
    if (aer_type(col) != TYPE_ARRAY) {
        error("v3 M4 slice only supports iterating arrays");
        DISPATCH();
    }
    AerArray* a = aer_as_array(col);
    long long idx = aer_as_int(v3_registers[idx_reg]);
    if ((unsigned long long)idx >= a->count) {
        vm->ip = (unsigned int)end_target;
        DISPATCH();
    }
    v3_registers[item_dest_reg] = a->items[idx];
    v3_registers[idx_reg]       = aer_int(idx + 1);
    DISPATCH();
}

/* Feature-completeness follow-up — mirrors lbl_iter_range's TYPE_INTEGER checks/direction-inference/
   exit-condition exactly (vm.c, above), reading/writing three plain registers instead of three
   un-popped stack slots; no stack cleanup needed on exit since registers aren't a shared LIFO
   structure the way vm->stack is. */
lbl_v3_iter_range: {
    int cur_reg       = (int)V3_UNPACK_A(op_word);
    int end_reg       = (int)V3_UNPACK_B(op_word);
    int step_reg      = (int)V3_UNPACK_C(op_word);
    int item_dest_reg = READ();
    int end_target    = READ();
    AerVal cur_v  = v3_registers[cur_reg];
    AerVal end_v  = v3_registers[end_reg];
    AerVal step_v = v3_registers[step_reg];
    if (aer_type(cur_v) != TYPE_INTEGER || aer_type(end_v) != TYPE_INTEGER || aer_type(step_v) != TYPE_INTEGER) {
        error("Range bounds and step must be integers");
        vm->ip = (unsigned int)end_target;
        DISPATCH();
    }
    long long cur = aer_as_int(cur_v), rng_end = aer_as_int(end_v), step = aer_as_int(step_v);
    if (step <= 0) {
        error("Range step must be a positive integer (direction is inferred from the bounds, not the step's sign)");
        vm->ip = (unsigned int)end_target;
        DISPATCH();
    }
    bool ascending = cur < rng_end;
    if (ascending ? (cur >= rng_end) : (cur <= rng_end)) {
        vm->ip = (unsigned int)end_target;
        DISPATCH();
    }
    v3_registers[item_dest_reg] = cur_v;
    v3_registers[cur_reg]       = aer_int(cur + (ascending ? step : -step));
    DISPATCH();
}

/* Mirrors lbl_call's struct-instantiation fallback (above, around line 2137) almost verbatim —
   same chunk_find_shape() lookup, same arity check, same single-allocation struct_pool layout —
   reading args from a register range instead of popping them off the stack in reverse. */
lbl_v3_struct_new: {
    int dest_reg           = (int)V3_UNPACK_A(op_word);
    int arg_reg_base       = (int)V3_UNPACK_B(op_word);
    int arg_count          = (int)V3_UNPACK_C(op_word);
    int type_name_pool_idx = READ();
    const char* name = aer_as_string(c->pool[type_name_pool_idx])->data;
    Shape* shape = chunk_find_shape(c, name);
    if (!shape) { error("'%s' is not defined", name); DISPATCH(); }
    if ((unsigned int)arg_count > shape->field_count) {
        error("'%s' takes at most %u argument%s, got %d",
              name, shape->field_count, shape->field_count == 1 ? "" : "s", arg_count);
        DISPATCH();
    }
    AerArray* a = pool_alloc(&struct_pool);
    a->count = a->capacity = shape->field_count;
    a->items = (AerVal*)((char*)a + sizeof(AerArray));
    a->shape = shape;
    for (int i = 0; i < arg_count; i++)
        a->items[i] = v3_registers[arg_reg_base + i];
    for (unsigned int i = (unsigned int)arg_count; i < shape->field_count; i++)
        a->items[i] = vm_default_value(shape->field_defaults[i]);
    v3_registers[dest_reg] = aer_array_val(a);
    DISPATCH();
}

/* Mirrors lbl_field_get (above) exactly — same pool-index field-name scan — reading struct_reg
   from a register instead of popping the stack. */
lbl_v3_field_get: {
    unsigned int site = vm->ip - 1;
    int dest_reg   = (int)V3_UNPACK_A(op_word);
    int struct_reg = (int)V3_UNPACK_B(op_word);
    int field_idx  = READ();
    AerVal obj = v3_registers[struct_reg];
    if (aer_type(obj) != TYPE_ARRAY || !aer_as_array(obj)->shape) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerArray* oa = aer_as_array(obj);
    Shape* shape = oa->shape;
    if (c->field_cache_shape[site] == shape) {
        v3_registers[dest_reg] = oa->items[c->field_cache_slot[site]];
        DISPATCH();
    }
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == (unsigned int)field_idx) {
            c->field_cache_shape[site] = shape;
            c->field_cache_slot[site]  = (int)i;
            v3_registers[dest_reg] = oa->items[i];
            DISPATCH();
        }
    }
    error("'%s' has no field '%s'", aer_as_string(c->pool[shape->name])->data,
          aer_as_string(c->pool[field_idx])->data);
    DISPATCH();
}

/* Fusion opcode — see its own comment in vm.h. Same inline-cache mechanism as lbl_v3_field_get
   (a distinct site, since this instruction lives at its own, different bytecode offset), just
   feeding the field value straight into vm_binary instead of writing it to a register first. */
lbl_v3_binary_field: {
    unsigned int site   = vm->ip - 1;
    int dest_reg   = (int)V3_UNPACK_A(op_word);
    int struct_reg = (int)V3_UNPACK_B(op_word);
    int bin_op     = (int)V3_UNPACK_C(op_word);
    int rk_lhs     = READ();
    int field_idx  = READ();
    AerVal lhs = vm_v3_rk_value(c, rk_lhs);
    AerVal obj = v3_registers[struct_reg];
    if (aer_type(obj) != TYPE_ARRAY || !aer_as_array(obj)->shape) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerArray* oa = aer_as_array(obj);
    Shape* shape = oa->shape;
    if (c->field_cache_shape[site] == shape) {
        v3_registers[dest_reg] = vm_binary(lhs, oa->items[c->field_cache_slot[site]], (Opcode)bin_op);
        DISPATCH();
    }
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == (unsigned int)field_idx) {
            c->field_cache_shape[site] = shape;
            c->field_cache_slot[site]  = (int)i;
            v3_registers[dest_reg] = vm_binary(lhs, oa->items[i], (Opcode)bin_op);
            DISPATCH();
        }
    }
    error("'%s' has no field '%s'", aer_as_string(c->pool[shape->name])->data,
          aer_as_string(c->pool[field_idx])->data);
    DISPATCH();
}

/* Mirror of lbl_v3_binary_field for the other operand order — see OP_V3_FIELD_BINARY's own
   comment in vm.h. Own field_cache site, same as every other field-access opcode. */
lbl_v3_field_binary: {
    unsigned int site   = vm->ip - 1;
    int dest_reg   = (int)V3_UNPACK_A(op_word);
    int struct_reg = (int)V3_UNPACK_B(op_word);
    int bin_op     = (int)V3_UNPACK_C(op_word);
    int field_idx  = READ();
    int rk_rhs     = READ();
    AerVal rhs = vm_v3_rk_value(c, rk_rhs);
    AerVal obj = v3_registers[struct_reg];
    if (aer_type(obj) != TYPE_ARRAY || !aer_as_array(obj)->shape) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerArray* oa = aer_as_array(obj);
    Shape* shape = oa->shape;
    if (c->field_cache_shape[site] == shape) {
        v3_registers[dest_reg] = vm_binary(oa->items[c->field_cache_slot[site]], rhs, (Opcode)bin_op);
        DISPATCH();
    }
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == (unsigned int)field_idx) {
            c->field_cache_shape[site] = shape;
            c->field_cache_slot[site]  = (int)i;
            v3_registers[dest_reg] = vm_binary(oa->items[i], rhs, (Opcode)bin_op);
            DISPATCH();
        }
    }
    error("'%s' has no field '%s'", aer_as_string(c->pool[shape->name])->data,
          aer_as_string(c->pool[field_idx])->data);
    DISPATCH();
}

/* Mirrors lbl_field_set (above) exactly, including its gc_barrier_array call — the first v3
   opcode to exercise that barrier for a struct instance rather than an array/dict. */
lbl_v3_field_set: {
    unsigned int site = vm->ip - 1;
    int struct_reg = (int)V3_UNPACK_A(op_word);
    int field_idx  = READ();
    int rk_val     = READ();
    AerVal obj = v3_registers[struct_reg];
    AerVal val = vm_v3_rk_value(c, rk_val);
    if (aer_type(obj) != TYPE_ARRAY || !aer_as_array(obj)->shape) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerArray* oa = aer_as_array(obj);
    Shape* shape = oa->shape;
    if (c->field_cache_shape[site] == shape) {
        gc_barrier_array(oa, val);
        oa->items[c->field_cache_slot[site]] = val;
        DISPATCH();
    }
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == (unsigned int)field_idx) {
            c->field_cache_shape[site] = shape;
            c->field_cache_slot[site]  = (int)i;
            gc_barrier_array(oa, val);
            oa->items[i] = val;
            DISPATCH();
        }
    }
    error("'%s' has no field '%s'", aer_as_string(c->pool[shape->name])->data,
          aer_as_string(c->pool[field_idx])->data);
    DISPATCH();
}

/* Merges lbl_negate/lbl_not/lbl_bitwise_not (above, ~line 1967) into one handler keyed by
   unary_op, same "reuse the stack VM's Opcode value as an operand tag" convention lbl_v3_binary
   already uses — reading an RK operand (register or constant) instead of popping the stack.
   M5 slice 9 folded OP_TO_STR in too (string interpolation's "{name}" -> string conversion) —
   same one-opcode-per-mechanism-family shape, calling the existing vm_to_str() helper (shared
   with print()/lbl_to_str above) rather than reimplementing value formatting. */
lbl_v3_unary: {
    int dest        = (int)V3_UNPACK_A(op_word);
    Opcode unary_op = (Opcode)V3_UNPACK_B(op_word);
    int rk          = READ();
    AerVal v = vm_v3_rk_value(c, rk);
    AerVal result;
    switch (unary_op) {
        case OP_NEGATE:
            if      (aer_type(v) == TYPE_INTEGER) result = aer_int(-aer_as_int(v));
            else if (aer_type(v) == TYPE_REAL)     result = aer_real(-aer_as_real(v));
            else { error("Negation requires a numeric type"); result = aer_null(); }
            break;
        case OP_NOT:
            result = aer_bool(!vm_truthy(v));
            break;
        case OP_BITWISE_NOT:
            if (aer_type(v) != TYPE_INTEGER) { error("Bitwise NOT requires an integer"); result = aer_null(); }
            else result = aer_int(~aer_as_int(v));
            break;
        case OP_TO_STR:
            result = vm_to_str(vm, v);
            break;
        default:
            result = aer_null();
            break;
    }
    v3_registers[dest] = result;
    DISPATCH();
}

/* `x as integer/float/boolean` — reuses the extracted vm_cast() helper (above, shared with
   lbl_cast) directly, no logic duplicated. */
lbl_v3_cast: {
    int dest      = (int)V3_UNPACK_A(op_word);
    int cast_type = (int)V3_UNPACK_B(op_word);
    int rk        = READ();
    AerVal v = vm_v3_rk_value(c, rk);
    v3_registers[dest] = vm_cast(v, cast_type);
    DISPATCH();
}
#endif

lbl_halt:
    return true;
}

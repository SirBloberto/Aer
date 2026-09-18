#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "aer_host.h"
#include "aer_module.h"
#include "aer_stdlib.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "pool.h"
#include "strbuf.h"
#include "vm.h"

/* Tuning defaults every freshly-initialized heap inherits -- process-wide mutable state, not
   hardcoded constants, specifically so aer_gc_configure()/aer_gc_set_ceiling() work when called
   before any VM exists yet (configure once, then create VMs that pick it up). aer_gc_configure/
   set_ceiling update these AND current_heap's own live fields, so both "configure ahead of time"
   and "reconfigure an already-running VM" work. */
static unsigned int default_nursery_bytes = 1024u * 1024;
static unsigned int default_growth_factor = 2;
static unsigned int default_gc_live_cell_ceiling = 0; /* 0 = unlimited */

/* Tier sizes for the size-classed struct pools (VmHeap.struct_pools, vm.h's own comment on why).
   Powers of two, same spirit as hashtable.c's KEY_TIER_SIZE -- the largest tier (256 = MAX_STRUCT_
   FIELDS * sizeof(AerVal)) exactly covers the worst case, so every shape's instance_bytes fits
   some tier; no malloc-fallback tier needed. pool_init itself floors any stride below 16 bytes
   (pool.c, for the free-list pointer), so the smallest tier needs no special-casing here either. */
static const size_t STRUCT_PAYLOAD_TIER_SIZE[STRUCT_PAYLOAD_TIER_COUNT] = {16, 32, 64, 128, 256};
static const unsigned int STRUCT_TIER_ELEMS_PER_SLAB[STRUCT_PAYLOAD_TIER_COUNT] = {64, 64, 64, 32, 16};

/* Smallest tier that fits instance_bytes -- ceiling, never floor, since a cell smaller than the
   shape's own fields buffer would let AerStruct.fields (set to right after the header, in the
   same cell) run past the cell's actual allocation. */
static Pool* struct_pool_for_size(VmHeap* heap, unsigned int instance_bytes) {
    for (unsigned int i = 0; i < STRUCT_PAYLOAD_TIER_COUNT; i++)
        if (instance_bytes <= STRUCT_PAYLOAD_TIER_SIZE[i])
            return &heap->struct_pools[i];
    return &heap->struct_pools
                [STRUCT_PAYLOAD_TIER_COUNT -
                 1]; /* unreachable given the 256 ceiling above, but fail safe rather than out-of-bounds */
}

/* Smallest tier is 32, not 16: a payload only exists above AER_STRING_INLINE_MAX, so the smallest
   buffer this ever allocates is 17 bytes and a 16-byte tier could never be selected. */
static const size_t STRING_PAYLOAD_TIER_SIZE[STRING_PAYLOAD_TIER_COUNT] = {32, 64, 128, 256};
static const unsigned int STRING_PAYLOAD_TIER_ELEMS_PER_SLAB[STRING_PAYLOAD_TIER_COUNT] = {128, 64, 32, 16};

static void vm_heap_init(VmHeap* heap); /* defined below; the payload allocator needs it first */

/* Tier owning a `length`-character payload, or -1 for the plain-malloc fallback. Both alloc and
   free derive the class through this one function, so they cannot disagree. */
static int string_payload_tier(unsigned int length) {
    size_t need = (size_t)length + 1; /* + NUL, exactly what the allocation must hold */
    for (int i = 0; i < STRING_PAYLOAD_TIER_COUNT; i++)
        if (need <= STRING_PAYLOAD_TIER_SIZE[i])
            return i;
    return -1;
}

char* vm_string_payload_alloc(VmHeap* heap, unsigned int length) {
    int tier = string_payload_tier(length);
    if (tier < 0 || !heap)
        return xmalloc((size_t)length + 1);
    vm_heap_init(heap);
    return pool_alloc(&heap->string_payload_pools[tier]);
}

void vm_string_payload_free(VmHeap* heap, char* payload, unsigned int length) {
    int tier = string_payload_tier(length);
    if (tier < 0) {
        free(payload);
        return;
    }
    /* No heap means the payload outlived its own pools, which vm_free prevents by bracketing
       gc_finalize_all_pools with current_heap. Leave it to the slab teardown rather than guess. */
    if (!heap)
        return;
    pool_free(&heap->string_payload_pools[tier], payload);
}

/* Initializes one heap's pools -- called once per VM (vm_init), not once per process, since every
   VM now owns its own. */
static void vm_heap_init(VmHeap* heap) {
    if (heap->pools_initialized)
        return;
    pool_init(&heap->string_pool, sizeof(AerString), 256);
    pool_init(&heap->array_pool, sizeof(AerArray), 256);
    pool_init(&heap->dict_pool, sizeof(AerDict), 64);
    pool_init(&heap->function_pool, sizeof(AerFunction), 64);
    for (unsigned int i = 0; i < STRUCT_PAYLOAD_TIER_COUNT; i++)
        pool_init(&heap->struct_pools[i], sizeof(AerStruct) + STRUCT_PAYLOAD_TIER_SIZE[i],
                  STRUCT_TIER_ELEMS_PER_SLAB[i]);
    for (unsigned int i = 0; i < STRING_PAYLOAD_TIER_COUNT; i++)
        pool_init(&heap->string_payload_pools[i], STRING_PAYLOAD_TIER_SIZE[i],
                  STRING_PAYLOAD_TIER_ELEMS_PER_SLAB[i]);
    pool_init(&heap->packed_array_pool, sizeof(AerPackedArray), 64);
    pool_init(&heap->typed_array_pool, sizeof(AerTypedArray), 64);
    pool_init(&heap->result_pool, sizeof(AerResult), 64);
    hashtable_pools_init(&heap->dict_hash_pools);
    for (unsigned int i = 0; i < TYPED_ARRAY_FREE_CACHE_SLOTS; i++) {
        heap->typed_array_free_cache[i].size = 0;
        heap->typed_array_free_cache[i].ptr = NULL;
    }
    heap->typed_array_free_cache_bytes = 0;
    heap->nursery_bytes = default_nursery_bytes;
    heap->growth_factor = default_growth_factor;
    heap->gc_live_cell_ceiling = default_gc_live_cell_ceiling;
    heap->pools_initialized = true;
}

/* Every allocation from a heap's GC-managed pools goes through here, so young_bytes (the minor
   collection's trigger) counts each cell exactly once. */
static void* heap_alloc(VmHeap* heap, Pool* p) {
    heap->young_bytes += p->stride;
    return pool_alloc(p);
}

/* A struct instance's field count never changes, so header+items are one allocation, sized
   for the MAX_STRUCT_FIELDS worst case (Pool needs a uniform cell size). */
/* Test-only (tests/smoke_test.c) -- reads a register from whichever frame is active, per VM
   instance so a nested module VM stays isolated. */
AerVal register_get(VM* vm, int slot) {
    return vm->call_stack[vm->call_depth].registers[slot];
}

/* RK16: 1 flag + 15 index bits, the wire form most RK operands use. Returns a pointer into the
   hoisted const_pool/registers, not a copy. Takes registers directly rather than re-deriving from
   a VM*: that re-fetch, through a pointer the compiler cannot prove is unaliased, measured ~4.5%
   more instructions on fib_bench. */
static inline AerVal* vm_rk_ptr16(AerVal* registers, AerVal* pool, uint32_t rk16) {
    if (rk16 & RK16_CONST_FLAG)
        return &pool[rk16 & RK16_INDEX_MASK];
    return &registers[rk16 & RK16_INDEX_MASK];
}

/* RK8 (1 flag + 7 index bits) -- the narrow wire form used only where two RK operands must share
   one word alongside a dest register (the OP_ADD..OP_RSHIFT/OP_IN family, OP_INDEX_GET/SET,
   OP_UNARY, OP_CAST). A branch-free variant (per-frame pool window) was tried and reverted -- it
   made calls slower; Lua/V8 accept this branch too and make calls free instead (register_stack).
   Takes registers directly -- see vm_rk_ptr16's own comment just above for why. */
static inline AerVal* vm_rk_ptr8(AerVal* registers, AerVal* pool, uint32_t rk8) {
    if (rk8 & RK8_CONST_FLAG)
        return &pool[rk8 & RK8_INDEX_MASK];
    return &registers[rk8 & RK8_INDEX_MASK];
}

/* GC policy -- suppression, tuning, and the collection trigger         */

/* Young only if v has a pooled cell at all -- numbers and booleans have none. */
/* None of these take a VM* -- adding one would break every existing embedder. Suppress/unsuppress
   and aer_gc_stats act on the current heap. configure/set_ceiling also update process-wide
   defaults, since real usage calls them before any VM exists. */
void vm_gc_suppress(void) {
    vm_require_current_heap()->gc_suppress_depth++;
}
void vm_gc_unsuppress(void) {
    VmHeap* h = vm_require_current_heap();
    if (h->gc_suppress_depth > 0)
        h->gc_suppress_depth--;
}

void aer_gc_configure(unsigned int nursery_bytes, unsigned int growth_factor) {
    if (nursery_bytes)
        default_nursery_bytes = nursery_bytes;
    if (growth_factor)
        default_growth_factor = growth_factor;
    /* Also apply immediately to whichever heap is already current, if one exists -- so
       reconfiguring an already-running VM takes effect right away, not just for the next one. */
    VmHeap* heap = vm_require_current_heap();
    if (nursery_bytes)
        heap->nursery_bytes = nursery_bytes;
    if (growth_factor)
        heap->growth_factor = growth_factor;
}

void aer_gc_set_ceiling(unsigned int max_live_cells) {
    default_gc_live_cell_ceiling = max_live_cells;
    vm_require_current_heap()->gc_live_cell_ceiling = max_live_cells;
}

/* Split from gc_maybe_collect so the dispatch loop can put its own work (syncing vm->ip for error
   reporting) on the collection path without paying for it on the far commoner "nowhere near
   threshold" one -- see vm_run_slice's gc_maybe_collect shadow. */
static inline __attribute__((always_inline)) bool gc_should_collect(VM* vm) {
    VmHeap* heap = &vm->heap;
    if (heap->gc_suppress_depth > 0)
        return false;
    return heap->young_bytes >= heap->nursery_bytes;
}

/* Tiny and always_inline, so the common case costs nothing. gc.c holds the collection itself. */
static inline __attribute__((always_inline)) void gc_maybe_collect(VM* vm) {
    if (gc_should_collect(vm))
        gc_run_collection_cycle(vm);
}

/* live_cells is bookkeeping, not a fresh trace, so it counts garbage not yet swept. */
void aer_gc_stats(unsigned int* live_cells, unsigned int* minor_collections,
                  unsigned int* major_collections) {
    VmHeap* heap = vm_require_current_heap();
    if (live_cells)
        *live_cells = gc_count_live_cells(heap);
    if (minor_collections)
        *minor_collections = heap->minor_collections_run;
    if (major_collections)
        *major_collections = heap->major_collections_run;
}

/* Runtime error context -- the callbacks error.c installs              */

/* The line of the instruction that ends just before `offset`. A handler's recorded pc and a frame's return
   address both sit past the instruction they belong to -- the next statement's first word when that
   instruction ends its own -- so one word back is always inside it. */
static unsigned int line_before(Chunk* c, unsigned int offset) {
    return chunk_line_for_offset(c, offset > 0 ? offset - 1 : 0);
}

static unsigned int lookup_runtime_line(void) {
    VM* vm = vm_active_error_vm();
    if (!vm)
        return 0;
    /* error_pc is only ever set while that same chunk is executing, so resolving it here is safe --
       and this path is cold, so reloading chunk->code costs nothing. Falls back to ip for a VM that
       has not entered vm_run_slice yet. */
    if (!vm->error_pc || !vm->chunk)
        return chunk_line_for_offset(vm->chunk, vm->ip);
    return line_before(vm->chunk, (unsigned int)(vm->error_pc - vm->chunk->code));
}

static const char* lookup_runtime_filename(void) {
    if (!vm_active_error_vm())
        return NULL;
    return vm_active_error_vm()->chunk->source_filename;
}

static const char* lookup_runtime_function(void) {
    if (!vm_active_error_vm())
        return NULL;
    VM* vm = vm_active_error_vm();
    if (vm->call_depth == 0)
        return NULL;
    ChunkFunction* fn = chunk_find_function_by_offset(vm->chunk, vm->call_stack[vm->call_depth].code_offset);
    return fn ? aer_as_string(vm->chunk->pool[fn->name])->data : NULL;
}

static const char* tail_call_note(unsigned int collapsed, char* buf, size_t bufsize) {
    if (collapsed == 0)
        return "";
    snprintf(buf, bufsize, " (+%u tail call%s not shown)", collapsed, collapsed == 1 ? "" : "s");
    return buf;
}

static unsigned int lookup_runtime_stack_trace(char* out, unsigned int out_size) {
    if (!vm_active_error_vm())
        return 0;
    VM* vm = vm_active_error_vm();
    if (vm->call_depth == 0)
        return 0;
    Chunk* c = vm->chunk;
    const unsigned int max_frames = 20;
    unsigned int pos = 0, shown = 0;
    bool hit_synthetic_boundary = false;
    char note_buf[64];

    unsigned int innermost_collapsed = vm->call_stack[vm->call_depth].tail_calls_collapsed;
    if (innermost_collapsed > 0) {
        int n = snprintf(out + pos, out_size - pos, "\n %s",
                         tail_call_note(innermost_collapsed, note_buf, sizeof(note_buf)));
        if (n > 0 && (unsigned int)n < out_size - pos)
            pos += (unsigned int)n;
    }

    for (int depth = (int)vm->call_depth; depth >= 1 && shown < max_frames; depth--) {
        if (vm->call_stack[depth].synthetic_entry) {
            hit_synthetic_boundary = true;
            break;
        }
        unsigned int line = line_before(c, vm->call_stack[depth].return_ip);
        const char* note =
            tail_call_note(vm->call_stack[depth - 1].tail_calls_collapsed, note_buf, sizeof(note_buf));
        int n;
        if (depth - 1 == 0) {
            n = snprintf(out + pos, out_size - pos, "\n  called from line %u, at top level%s", line, note);
        } else {
            ChunkFunction* fn = chunk_find_function_by_offset(c, vm->call_stack[depth - 1].code_offset);
            const char* fname = fn ? aer_as_string(c->pool[fn->name])->data : "?";
            n = snprintf(out + pos, out_size - pos, "\n  called from line %u, in %s()%s", line, fname, note);
        }
        if (n < 0 || (unsigned int)n >= out_size - pos)
            break;
        pos += (unsigned int)n;
        shown++;
    }
    if (!hit_synthetic_boundary && shown < (unsigned int)vm->call_depth) {
        int n =
            snprintf(out + pos, out_size - pos, "\n  ... and %u more", (unsigned int)vm->call_depth - shown);
        if (n > 0 && (unsigned int)n < out_size - pos)
            pos += (unsigned int)n;
    }
    return pos;
}

/* String construction                                              */

/* Wraps an exclusively-owned (data, length) in a fresh heap box; never copies. Routes to
   current_heap, guarded via vm_require_current_heap() because the lexer can call this
   (emit_string_token) before any VM/pool exists -- without the guard, an uninitialized heap's
   zero elem_size makes pool_alloc hand back a ~1-byte allocation (ASAN heap-buffer-overflow). */
/* always_inline: counting the text made LTO stop inlining it, putting a call on every string built. */
static inline __attribute__((always_inline)) AerString* aer_string_alloc(unsigned int length) {
    VmHeap* heap = vm_require_current_heap();
    vm_heap_init(heap);
    AerString* s = heap_alloc(heap, &heap->string_pool);
    if (length > AER_STRING_INLINE_MAX)
        heap->young_bytes += (size_t)length + 1; /* the text, which a cell's own size says nothing about */
    s->length = length;
    s->hash = 0;
    return s;
}

AerVal aer_make_string(char* data, unsigned int length) {
    AerString* s = aer_string_alloc(length);
    if (length <= AER_STRING_INLINE_MAX) {
        /* Small-string optimization (see AerString's own comment, value.h): copy into this cell's
           own inline_buf and drop the caller's separately-allocated buffer. This exists only to keep
           every EXISTING aer_make_string call site correct without editing it -- it does NOT avoid
           an allocation by itself (the caller already paid for `data`'s malloc before calling this);
           aer_make_string_copy, below, is the version that actually avoids one. */
        memcpy(s->inline_buf, data, length);
        s->inline_buf[length] = '\0';
        s->data = s->inline_buf;
        free(data);
    } else {
        s->data = data;
    }
    return aer_string_val(s);
}

AerVal aer_make_string_copy(const char* src, unsigned int length) {
    AerString* s = aer_string_alloc(length);
    if (length <= AER_STRING_INLINE_MAX) {
        memcpy(s->inline_buf, src, length);
        s->inline_buf[length] = '\0';
        s->data = s->inline_buf;
    } else {
        char* buf = vm_string_payload_alloc(vm_require_current_heap(), length);
        memcpy(buf, src, length);
        buf[length] = '\0';
        s->data = buf;
    }
    return aer_string_val(s);
}

/* VM lifecycle                                                         */

/* Registered here, once per process regardless of how many VMs get created (module loading spins
   up a fresh one per import), so io is exactly as always-on as collection/math/string/etc. with no
   host action needed. aer_register_function() has no dedup check of its own, so calling
   aer_io_register() more than once would silently grow host_functions[] on every import. */
static bool io_registered = false;
static void ensure_io_registered(void) {
    if (io_registered)
        return;
    aer_io_register();
    io_registered = true;
}

bool aer_io_enabled = true;
bool aer_net_enabled = true;
bool aer_import_enabled = true;

void aer_set_io_enabled(bool enabled) {
    aer_io_enabled = enabled;
}
void aer_set_net_enabled(bool enabled) {
    aer_net_enabled = enabled;
}
void aer_set_import_enabled(bool enabled) {
    aer_import_enabled = enabled;
}

void vm_init(VM* vm, Chunk* chunk) {
    memset(vm, 0, sizeof(*vm));
    vm->chunk = chunk;
    vm->io_enabled = aer_io_enabled;
    vm->net_enabled = aer_net_enabled;
    vm_heap_init(&vm->heap);
    /* Every VM now owns its own heap -- this VM's is the active allocation target from here on,
       for both its own execution and any parsing that immediately follows for its Chunk (see
       current_heap's own comment). Saved/restored around vm_run_slice for nested/reentrant runs,
       exactly like vm_active_error_vm() just below. */
    vm_set_current_heap(&vm->heap);
    ensure_io_registered();
    runtime_line_lookup = lookup_runtime_line;
    runtime_filename_lookup = lookup_runtime_filename;
    runtime_function_lookup = lookup_runtime_function;
    runtime_stack_trace_lookup = lookup_runtime_stack_trace;
    /* Not only by DISPATCH(): parsing happens before any opcode runs, and its errors need a filename
       too. A nested module overwrites this while it parses; DISPATCH() reasserts the outer VM. */
    vm_set_active_error_vm(vm);
    /* Frame 0's register window only ever needs linking once, for the life of the VM (top-level
       usage is open-ended, so it always gets a flat FRAME_REGISTERS reservation) -- everything
       else a fresh run needs is exactly what aer_vm_reset_for_reuse() already does. */
    vm->register_stack =
        xmalloc(sizeof(AerVal) * REGISTER_STACK_INITIAL_FRAMES * FRAME_REGISTERS);
    vm->call_stack = xcalloc(REGISTER_STACK_INITIAL_FRAMES, sizeof(CallFrame));
    vm->call_depth_limit = REGISTER_STACK_INITIAL_FRAMES;
    vm->register_capacity = (size_t)REGISTER_STACK_INITIAL_FRAMES * FRAME_REGISTERS;
    vm->push_base_limit = vm->register_stack + (vm->register_capacity - FRAME_REGISTERS);
    vm->call_stack[0].registers = &vm->register_stack[0];
    vm->call_stack[0].frame_size = FRAME_REGISTERS;
    /* Top level gets no gap: its real slots come off the top of the same bank the collector has to
       trace for top-level variables, and there is no per-call cost here to save by narrowing it. */
    vm->call_stack[0].frame_bounds = FRAME_BOUNDS(FRAME_REGISTERS, FRAME_REGISTERS);
    /* Top level has no caller to tag its frame, so it is done once here. Its real slots come off the top
       and the parser never places one on a register it has used for anything else, so the tags stay
       right for the VM's life -- retagging on a later run would overwrite live top-level variables. */
    for (unsigned int i = 0; i < FRAME_REGISTERS; i++)
        vm->call_stack[0].registers[i].tag = TYPE_REAL;
    aer_vm_reset_for_reuse(vm);
}

void vm_free(VM* vm) {
    VmHeap* heap = &vm->heap;
    /* Every live cell's separately-owned payload must be freed before its pool's slabs go away;
       pool_destroy alone would leak them all. Unlike a normal sweep this finalizes regardless of
       mark state -- the whole heap is going, not just recent garbage.
       current_heap is saved/set/restored for free_typed_array, which stashes buffers into a
       specific heap's cache; without it a stash can land in another live VM's heap. */
    VmHeap* saved_current_heap = vm_current_heap();
    vm_set_current_heap(heap);
    gc_finalize_all_pools(heap);
    vm_set_current_heap(saved_current_heap);

    /* The free cache holds buffers belonging to no live cell, so the sweep above never reaches
       them. */
    for (unsigned int i = 0; i < TYPED_ARRAY_FREE_CACHE_SLOTS; i++)
        free(heap->typed_array_free_cache[i].ptr);
    heap->typed_array_free_cache_bytes = 0;

    pool_destroy(&heap->string_pool);
    pool_destroy(&heap->array_pool);
    pool_destroy(&heap->dict_pool);
    pool_destroy(&heap->function_pool);
    for (unsigned int i = 0; i < STRUCT_PAYLOAD_TIER_COUNT; i++)
        pool_destroy(&heap->struct_pools[i]);
    for (unsigned int i = 0; i < STRING_PAYLOAD_TIER_COUNT; i++)
        pool_destroy(&heap->string_payload_pools[i]);
    pool_destroy(&heap->packed_array_pool);
    pool_destroy(&heap->typed_array_pool);
    pool_destroy(&heap->result_pool);
    /* free_dict (above, via pool_finalize_all) already freed every live AerDict's own hashtable
       entries back into heap->dict_hash_pools, so every key it ever handed out has already been
       returned by the time these tiers are torn down. */
    for (unsigned int i = 0; i < HASH_KEY_TIER_COUNT; i++)
        pool_destroy(&heap->dict_hash_pools.key_pools[i]);
    for (unsigned int i = 0; i < HASH_SPARSE_TIER_COUNT; i++)
        pool_destroy(&heap->dict_hash_pools.sparse_pools[i]);
    free(heap->remembered_set);
    free(heap->gc_worklist.items);
    free(heap->gc_worklist.ranges);
    /* If this VM's heap was the active allocation target, it no longer exists -- leaving
       current_heap dangling would be a use-after-free the moment anything allocates next. */
    if (vm_current_heap() == heap)
        vm_set_current_heap(NULL);
    *heap = (VmHeap){0};

    free(vm->register_stack);
    vm->register_stack = NULL;
    free(vm->call_stack);
    vm->call_stack = NULL;
    vm->call_depth_limit = 0;
    vm->register_capacity = 0;
    vm->push_base_limit = NULL;
}

void aer_vm_reset_for_reuse(VM* vm) {
    vm->stack_top = 0;
    vm->call_depth = 0;
}

bool aer_run_source(VM* vm, Chunk* chunk, const char* source) {
    /* parse() below runs BEFORE vm_run(vm) -- vm_run_slice's own current_heap save/restore only
       wraps the run, not this function's own parse step, and vm_init already ran for `vm` (this
       is the "run more code into an already-initialized VM" entry point, REPL-style), so nothing
       else sets current_heap here. Without this, a parse-time allocation (a string literal, an
       interned token) lands in whatever heap some OTHER, unrelated VM last left active instead of
       this one's own -- exactly the cross-heap contamination per-VM heaps exist to prevent. */
    vm_set_current_heap(&vm->heap);
    /* Re-seeded every call, not just at vm_init: the documented toggle pattern flips the global,
       runs one thing, then flips it back, all on an EXISTING vm (embed_smoke_test.c) -- which
       seeding once would silently ignore. */
    vm->io_enabled = aer_io_enabled;
    vm->net_enabled = aer_net_enabled;
    aer_vm_reset_for_reuse(vm);
    vm->ip = chunk->count;
    shell((char*)source); /* shell() strdup()s its own copy -- never mutates through this pointer */
    lex();
    parse(chunk);
    chunk_emit(chunk, OP_HALT);
    runtime_had_error = false;
    return vm_run(vm);
}

/* Type helpers                                                         */

/* A struct reports its declared name, a packed array that name plus "[]". */
static const char* vm_type_name(Chunk* c, AerVal v) {
    if (aer_type(v) == TYPE_STRUCT)
        return aer_as_string(c->pool[aer_as_struct(v)->shape->name])->data;
    if (aer_type(v) == TYPE_PACKED_ARRAY) {
        /* static buf is safe only because every caller consumes the result immediately. */
        AerPackedArray* pa = aer_as_packed_array(v);
        static char buf[128];
        snprintf(buf, sizeof(buf), "%s[]", aer_as_string(c->pool[pa->shape->name])->data);
        return buf;
    }
    if (aer_type(v) == TYPE_TYPED_ARRAY) {
        static char buf[32];
        snprintf(buf, sizeof(buf), "%s[]", aer_typed_elem_names[aer_as_typed_array(v)->elem_kind]);
        return buf;
    }
    if (aer_type(v) == TYPE_RESULT)
        return "Result";
    return aer_value_type_names[aer_type(v)];
}

static inline __attribute__((always_inline)) bool vm_truthy(AerVal v) {
    switch (aer_type(v)) {
        case TYPE_NULL: return false;
        case TYPE_BOOLEAN: return aer_as_bool(v);
        case TYPE_INTEGER: return aer_as_int(v) != 0;
        case TYPE_REAL: return aer_as_real(v) != 0.0;
        case TYPE_STRING: return aer_as_string(v)->length > 0;
        case TYPE_FUNCTION: return true;
        case TYPE_ARRAY: return aer_as_array(v)->count > 0;
        case TYPE_DICT: return aer_as_dict(v)->map.count > 0;
        case TYPE_STRUCT: return true; /* a struct can never have zero fields, enforced at parse time */
        case TYPE_PACKED_ARRAY: return aer_as_packed_array(v)->count > 0;
        case TYPE_TYPED_ARRAY: return aer_as_typed_array(v)->count > 0;
        /* `if result:` reads like `if err == null:`, without destructuring first. */
        case TYPE_RESULT: return aer_type(aer_as_result(v)->err) == TYPE_NULL;
        case TYPE_ANY: break; /* never a real AerVal's tag -- only Shape.field_types[] uses it */
    }
    return false;
}

static inline __attribute__((always_inline)) AerVal vm_promote_real(AerVal v) {
    if (aer_type(v) == TYPE_INTEGER)
        v = aer_real((double)aer_as_int(v));
    return v;
}

/* Floored, taking the divisor's sign, so `(a // b) * b + (a % b) == a` holds for negatives -- `//`
   already floors and C's truncating % disagrees with it. Matches Lua and Python.
   The int32 narrowing is a real win on 32-bit ARM, where a 64-bit modulo is a libgcc call. rv != -1
   is required, not incidental: INT32_MIN % -1 overflows at 32-bit width but not at 64. */
static inline int64_t aer_mod_int64(int64_t l, int64_t rv) {
    /* Both operands non-negative is the overwhelmingly common shape, and it is worth its own path
       twice over on 32-bit ARM: the unsigned divide skips the sign handling libgcc's signed one does
       around it, and a non-negative remainder never needs the flooring correction below. */
    if (l >= 0 && rv > 0 && l <= (int64_t)UINT32_MAX && rv <= (int64_t)UINT32_MAX)
        return (int64_t)((uint32_t)l % (uint32_t)rv);
    int64_t r;
    if (rv != -1 && l >= INT32_MIN && l <= INT32_MAX && rv >= INT32_MIN && rv <= INT32_MAX)
        r = (int32_t)l % (int32_t)rv;
    else
        r = l % rv;
    if (r != 0 && ((r < 0) != (rv < 0)))
        r += rv;
    return r;
}

/* Same flooring for reals, so `%` means one thing regardless of operand type. */
static inline double aer_mod_double(double l, double rv) {
    double r = fmod(l, rv);
    if (r != 0.0 && ((r < 0.0) != (rv < 0.0)))
        r += rv;
    return r;
}

/* Operator semantics -- what +, ==, in, ... actually do                */

/* Int/int and real/real fast path shared by every struct-field-fusion opcode. A plain `inline`
   hint, not always_inline: with 7 call sites, forcing it made h_binary_field/h_field_binary
   among the largest handlers in vm_run_slice. Sets *handled = false for anything else, and the
   caller falls back to vm_binary_cold(). */
static inline AerVal vm_binary_fast(AerVal a, AerVal b, Opcode op, ValueType ta, ValueType tb,
                                    bool* handled) {
    *handled = true;
    if (ta == TYPE_INTEGER && tb == TYPE_INTEGER) {
        int64_t l = aer_as_int(a), rv = aer_as_int(b);
        switch (op) {
            case OP_ADD: return aer_int(l + rv);
            case OP_SUB: return aer_int(l - rv);
            case OP_MUL: return aer_int(l * rv);
            case OP_DIV:
                if (rv == 0) {
                    error("Division by zero");
                    return aer_int(0);
                }
                return aer_real((double)l / (double)rv);
            case OP_FLOOR_DIV:
                if (rv == 0) {
                    error("Division by zero");
                    return aer_int(0);
                }
                return aer_int((int64_t)floor((double)l / (double)rv));
            case OP_MOD:
                if (rv == 0) {
                    error("Modulo by zero");
                    return aer_int(0);
                }
                return aer_int(aer_mod_int64(l, rv));
            case OP_LSHIFT: return aer_int(l << rv);
            case OP_RSHIFT: return aer_int(l >> rv);
            case OP_BITWISE_AND: return aer_int(l & rv);
            case OP_BITWISE_OR: return aer_int(l | rv);
            case OP_BITWISE_XOR: return aer_int(l ^ rv);
            case OP_EQ: return aer_bool(l == rv);
            case OP_NEQ: return aer_bool(l != rv);
            case OP_LT: return aer_bool(l < rv);
            case OP_GT: return aer_bool(l > rv);
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
                if (rv == 0.0) {
                    error("Division by zero");
                    return aer_real(0.0);
                }
                return aer_real(l / rv);
            case OP_FLOOR_DIV:
                if (rv == 0.0) {
                    error("Division by zero");
                    return aer_real(0.0);
                }
                return aer_real(floor(l / rv));
            case OP_MOD:
                if (rv == 0.0) {
                    error("Modulo by zero");
                    return aer_real(0.0);
                }
                return aer_real(aer_mod_double(l, rv));
            case OP_EQ: return aer_bool(l == rv);
            case OP_NEQ: return aer_bool(l != rv);
            case OP_LT: return aer_bool(l < rv);
            case OP_GT: return aer_bool(l > rv);
            case OP_LTE: return aer_bool(l <= rv);
            case OP_GTE: return aer_bool(l >= rv);
            default: error("Operator not valid for reals"); return aer_real(0.0);
        }
    }

    *handled = false;
    return aer_bool(false); /* unused by the caller when *handled is false */
}

/* Cold path for the per-operator labels and the field-fusion opcodes -- everything
   vm_binary_fast() doesn't handle. Real, non-inlined: this path is rare, and not inlining it
   avoids duplicating the body across every call site. */
/* The one `in` implementation -- called by h_in and by vm_binary_cold (reached when a fused
   opcode carries OP_IN as its runtime bin_op). */
static AerVal vm_in(AerVal a, AerVal b) {
    if (aer_type(b) == TYPE_DICT) {
        if (aer_type(a) != TYPE_STRING) {
            error("Left side of 'in' must be a string when testing dict membership");
            return aer_bool(false);
        }
        AerString* as = aer_as_string(a);
        unsigned int klen = hashtable_key_true_len(as->data, as->length);
        return aer_bool(hashtable_get_hashed(&aer_as_dict(b)->map, as->data, klen,
                                             hashtable_string_hash(as, klen)) != NULL);
    }
    if (aer_type(b) == TYPE_ARRAY) {
        AerArray* arr = aer_as_array(b);
        for (unsigned int i = 0; i < arr->count; i++) {
            if (values_equal(a, arr->items[i]))
                return aer_bool(true);
        }
        return aer_bool(false);
    }
    if (aer_type(b) == TYPE_STRING) {
        if (aer_type(a) != TYPE_STRING) {
            error("Left side of 'in' must be a string when testing string membership");
            return aer_bool(false);
        }
        return aer_bool(aer_string_find(aer_as_string(b), aer_as_string(a)) >= 0);
    }
    error("Right side of 'in' must be a dict, array, or string");
    return aer_bool(false);
}

/* Only used to name the operator in a type-mismatch message -- never on a path that already has
   its own more specific error (e.g. 'in' has vm_in()'s own messages above). */
static const char* binop_symbol(Opcode op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_DIV: return "/";
        case OP_FLOOR_DIV: return "//";
        case OP_MOD: return "%";
        case OP_EQ: return "==";
        case OP_NEQ: return "!=";
        case OP_LT: return "<";
        case OP_GT: return ">";
        case OP_LTE: return "<=";
        case OP_GTE: return ">=";
        case OP_IN: return "in";
        default: return "that operator";
    }
}

/* Defined below vm_typed_elem_width/read/write, which it needs; forward-declared here since
   vm_binary_cold (this function) is defined first in the file. */
static AerVal vm_typed_array_binary_op(AerTypedArray* ta, AerTypedArray* tb, Opcode op);
static AerVal vm_typed_array_scalar_op(AerTypedArray* a, AerVal scalar, Opcode op, bool flip);

static AerVal vm_binary_cold(Chunk* c, AerVal a, AerVal b, Opcode op, ValueType ta, ValueType tb) {
    /* Before the null handling below, so `null in arr` is a container search rather than a
       comparison error. */
    if (op == OP_IN)
        return vm_in(a, b);

    /* null equality: null == null is true; null op anything-else errors */
    if (ta == TYPE_NULL || tb == TYPE_NULL) {
        if (op == OP_EQ)
            return aer_bool(ta == TYPE_NULL && tb == TYPE_NULL);
        if (op == OP_NEQ)
            return aer_bool(!(ta == TYPE_NULL && tb == TYPE_NULL));
        error("Operator not valid for null");
        return aer_bool(false);
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
                if (rv == 0.0) {
                    error("Division by zero");
                    return aer_real(0.0);
                }
                return aer_real(l / rv);
            case OP_FLOOR_DIV:
                if (rv == 0.0) {
                    error("Division by zero");
                    return aer_real(0.0);
                }
                return aer_real(floor(l / rv));
            case OP_MOD:
                if (rv == 0.0) {
                    error("Modulo by zero");
                    return aer_real(0.0);
                }
                return aer_real(aer_mod_double(l, rv));
            case OP_EQ: return aer_bool(l == rv);
            case OP_NEQ: return aer_bool(l != rv);
            case OP_LT: return aer_bool(l < rv);
            case OP_GT: return aer_bool(l > rv);
            case OP_LTE: return aer_bool(l <= rv);
            case OP_GTE: return aer_bool(l >= rv);
            default: error("Operator not valid for reals"); return aer_real(0.0);
        }
    }

    if (aer_type(a) == TYPE_BOOLEAN && aer_type(b) == TYPE_BOOLEAN) {
        if (op == OP_EQ)
            return aer_bool(aer_as_bool(a) == aer_as_bool(b));
        if (op == OP_NEQ)
            return aer_bool(aer_as_bool(a) != aer_as_bool(b));
        error("Operator not valid for booleans");
        return aer_bool(false);
    }

    if (aer_type(a) == TYPE_STRING && aer_type(b) == TYPE_STRING) {
        AerString* as = aer_as_string(a);
        AerString* bs = aer_as_string(b);
        bool eq = as->length == bs->length && strncmp(as->data, bs->data, as->length) == 0;
        if (op == OP_EQ)
            return aer_bool(eq);
        if (op == OP_NEQ)
            return aer_bool(!eq);
        if (op == OP_ADD) {
            unsigned int len = as->length + bs->length;
            if (len <= AER_STRING_INLINE_MAX) {
                /* Assembled on the stack, not the heap -- the concat result is short enough to land
                   entirely inline in the new AerString cell, so there's nothing to allocate at all. */
                char stackbuf[AER_STRING_INLINE_MAX + 1];
                memcpy(stackbuf, as->data, as->length);
                memcpy(stackbuf + as->length, bs->data, bs->length);
                return aer_make_string_copy(stackbuf, len);
            }
            char* buf = vm_string_payload_alloc(vm_require_current_heap(), len);
            memcpy(buf, as->data, as->length);
            memcpy(buf + as->length, bs->data, bs->length);
            buf[len] = '\0';
            /* aer_make_string takes ownership of buf. Used once here, so no interning. */
            return aer_make_string(buf, len);
        }
        if (op == OP_LT || op == OP_GT || op == OP_LTE || op == OP_GTE) {
            /* Same total order collection.sort() uses for strings -- one shared helper (value.h). */
            int cmp = aer_string_compare(as, bs);
            switch (op) {
                case OP_LT: return aer_bool(cmp < 0);
                case OP_GT: return aer_bool(cmp > 0);
                case OP_LTE: return aer_bool(cmp <= 0);
                default: return aer_bool(cmp >= 0); /* OP_GTE */
            }
        }
        error("Operator not valid for strings");
        return aer_bool(false);
    }

    /* Compared by identity only. Tested on the values' own tags: a struct-field caller passes the
       field's declared type as ta, which is TYPE_ANY for an untyped field. */
    if (aer_type(a) == aer_type(b) &&
        (aer_type(a) == TYPE_ARRAY || aer_type(a) == TYPE_DICT || aer_type(a) == TYPE_RESULT)) {
        if (op == OP_EQ || op == OP_NEQ)
            return aer_bool((a.as.ptr == b.as.ptr) == (op == OP_EQ));
        error("Operator not valid for %s",
              aer_type(a) == TYPE_ARRAY ? "arrays" : aer_type(a) == TYPE_DICT ? "dicts" : "Results");
        return aer_bool(false);
    }

    if (aer_type(a) == TYPE_TYPED_ARRAY && aer_type(b) == TYPE_TYPED_ARRAY) {
        AerTypedArray* tta = aer_as_typed_array(a);
        AerTypedArray* ttb = aer_as_typed_array(b);
        if (op == OP_EQ)
            return aer_bool(tta == ttb);
        if (op == OP_NEQ)
            return aer_bool(tta != ttb);
        if (op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV || op == OP_LT || op == OP_LTE ||
            op == OP_GT || op == OP_GTE)
            return vm_typed_array_binary_op(tta, ttb, op);
        error("'%s' is not defined between two columns — columns support + - * / and the comparisons "
              "(== and != between two columns ask whether they are the same column, not elementwise)",
              binop_symbol(op));
        return aer_bool(false);
    }

    /* One side a typed array, the other a plain number: broadcast it across the array. */
    if (op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV || op == OP_LT || op == OP_LTE ||
        op == OP_GT || op == OP_GTE || op == OP_EQ || op == OP_NEQ) {
        bool a_arr = aer_type(a) == TYPE_TYPED_ARRAY, b_arr = aer_type(b) == TYPE_TYPED_ARRAY;
        bool a_num = ta == TYPE_INTEGER || ta == TYPE_REAL, b_num = tb == TYPE_INTEGER || tb == TYPE_REAL;
        if (a_arr && b_num)
            return vm_typed_array_scalar_op(aer_as_typed_array(a), b, op, false);
        if (b_arr && a_num)
            return vm_typed_array_scalar_op(aer_as_typed_array(b), a, op, true);
    }

    /* A column reaching here failed for a reason the generic message hides: either the operator is
       not one columns have, or the other side is not a number to broadcast. Saying which saves the
       reader looking for a type error that isn't there. */
    if (aer_type(a) == TYPE_TYPED_ARRAY || aer_type(b) == TYPE_TYPED_ARRAY) {
        bool other_is_num = (aer_type(a) == TYPE_TYPED_ARRAY ? tb : ta) == TYPE_INTEGER ||
                            (aer_type(a) == TYPE_TYPED_ARRAY ? tb : ta) == TYPE_REAL;
        if (other_is_num)
            error("'%s' is not defined on a column — columns support + - * / and the comparisons",
                  binop_symbol(op));
        else
            error("'%s' needs a number or another column on the other side, not %s", binop_symbol(op),
                  vm_type_name(c, aer_type(a) == TYPE_TYPED_ARRAY ? b : a));
        return aer_bool(false);
    }

    error("Cannot apply '%s' to %s and %s", binop_symbol(op), vm_type_name(c, a), vm_type_name(c, b));
    return aer_bool(false);
}

/* Value stringification                                            */

static AerVal vm_to_str(VM* vm, AerVal v) {
    if (aer_type(v) == TYPE_STRING)
        return v;

    char buf[64];
    const char* text;
    unsigned int len;
    /* Not interned: a runtime string is used once, and interning would grow the pool forever -- 7x
       slower for 100k unique casts than for 10 distinct ones. */
    if (aer_format_scalar(v, buf, sizeof(buf), &text, &len))
        return aer_make_string_copy(text, len);
    /* Recursive content has no bounded size, so this reuses print()'s formatter and hands over its
       buffer as-is. */
    StrBuf sb;
    strbuf_init(&sb);
    vm_format_value(vm->chunk, v, false, &sb);
    return aer_make_string(sb.buf, (unsigned int)sb.len);
}

/* OP_INTERP's builder. A part is rendered into `scratch` only if it isn't already a string;
   unbounded content defers to vm_to_str. */
/* Formats an interpolated dict key into a stack buffer and probes with the bytes, never building the
   AerString. Returns false (leaving *out alone) when that cannot work, and the caller falls back.
   Scalars render through aer_format_scalar, as in vm_interp_build, so a key written through OP_INTERP
   is found here; tests/test_interp_dict_keys.aer guards that. */
static bool vm_dict_get_interp(AerDict* d, const uint32_t* rks, unsigned int count,
                                                         AerVal* registers, AerVal* pool, AerVal* out) {
    char key[INTERP_KEY_MAX];
    unsigned int at = 0;
    for (unsigned int i = 0; i < count; i++) {
        AerVal v = *vm_rk_ptr16(registers, pool, rks[i]);
        const char* piece;
        unsigned int piece_len;
        char scratch[32];
        if (aer_type(v) == TYPE_STRING) {
            piece = aer_as_string(v)->data;
            piece_len = aer_as_string(v)->length;
        } else if (!aer_format_scalar(v, scratch, sizeof(scratch), &piece, &piece_len)) {
            return false; /* a collection part needs vm_to_str, which allocates */
        }
        if (at + piece_len > INTERP_KEY_MAX)
            return false;
        memcpy(key + at, piece, piece_len);
        at += piece_len;
    }
    unsigned int klen = hashtable_key_true_len(key, at);
    AerVal* found = hashtable_get_hashed(&d->map, key, klen, hashtable_hash_bytes(key, klen));
    *out = found ? *found : aer_null();
    return true;
}

static AerVal vm_interp_build(VM* vm, const AerVal* parts, unsigned int count) {
    const char* piece[INTERP_MAX_PARTS];
    unsigned int piece_len[INTERP_MAX_PARTS];
    char scratch[INTERP_MAX_PARTS][32];
    /* Only for parts that had to become a real string first; kept alive until the copy is done. */
    AerVal spilled[INTERP_MAX_PARTS];
    unsigned int spill_count = 0;
    unsigned int total = 0;

    for (unsigned int i = 0; i < count; i++) {
        AerVal v = parts[i];
        if (aer_type(v) == TYPE_STRING) {
            piece[i] = aer_as_string(v)->data;
            piece_len[i] = aer_as_string(v)->length;
        } else if (!aer_format_scalar(v, scratch[i], sizeof(scratch[i]), &piece[i], &piece_len[i])) {
            /* vm_to_str allocates; hold the result so a collection triggered by a later part cannot
               reclaim bytes this one still points at. */
            AerVal s = vm_to_str(vm, v);
            spilled[spill_count++] = s;
            piece[i] = aer_as_string(s)->data;
            piece_len[i] = aer_as_string(s)->length;
        }
        total += piece_len[i];
    }

    AerString* out = aer_string_alloc(total);
    char* buf;
    if (total <= AER_STRING_INLINE_MAX) {
        buf = out->inline_buf;
        out->data = buf;
    } else {
        buf = vm_string_payload_alloc(vm_require_current_heap(), total);
        out->data = buf;
    }
    unsigned int at = 0;
    for (unsigned int i = 0; i < count; i++) {
        memcpy(buf + at, piece[i], piece_len[i]);
        at += piece_len[i];
    }
    buf[total] = '\0';
    (void)spilled;
    return aer_string_val(out);
}

/* Either bound may be null, meaning 0 or len. Out-of-range clamps rather than errors, Python-style. */
/* Slices and default values                                        */

static bool vm_slice_bounds(AerVal start_v, AerVal end_v, int64_t len, int64_t* out_start, int64_t* out_end) {
    if (aer_type(start_v) != TYPE_NULL && aer_type(start_v) != TYPE_INTEGER) {
        error("Slice bounds must be integers");
        return false;
    }
    if (aer_type(end_v) != TYPE_NULL && aer_type(end_v) != TYPE_INTEGER) {
        error("Slice bounds must be integers");
        return false;
    }
    int64_t start = (aer_type(start_v) == TYPE_NULL) ? 0 : aer_as_int(start_v);
    int64_t end = (aer_type(end_v) == TYPE_NULL) ? len : aer_as_int(end_v);
    if (start < 0)
        start += len;
    if (end < 0)
        end += len;
    if (start < 0)
        start = 0;
    if (end > len)
        end = len;
    if (end < start)
        end = start;
    *out_start = start;
    *out_end = end;
    return true;
}

/* An empty plain array with room for `capacity` items; 0 leaves items NULL. Every field is set, since
   pool_alloc zeroes only gc_state and a reused cell's stale dirty_cards pointer would be unsafe. */
static AerArray* heap_new_array(VmHeap* heap, unsigned int capacity) {
    AerArray* a = heap_alloc(heap, &heap->array_pool);
    a->count = 0;
    a->shape = NULL;
    a->generation = 0;
    a->dirty_cards = NULL;
    a->dirty_cards_bytes = 0;
    a->dirty_min_byte = (unsigned int)-1;
    a->dirty_max_byte = 0;
    a->dirty_all = false;
    if (capacity > 0) {
        vm_array_alloc_items(a, capacity);
    } else {
        a->items = NULL;
        a->capacity = 0;
    }
    return a;
}

/* A baked default (struct field or function parameter): primitives copy as-is; an array/dict
   default must become a FRESH empty container, or every omitted call/instance would alias the
   same one (Python's mutable-default bug). parser.c only ever bakes an empty '[]'/'{}' as such
   a default, so a fresh empty one is always correct -- no deep copy needed. */
static AerVal vm_default_value(VM* vm, AerVal dflt) {
    if (aer_type(dflt) == TYPE_ARRAY && !aer_as_array(dflt)->shape)
        return aer_array_val(heap_new_array(&vm->heap, 0));
    if (aer_type(dflt) == TYPE_DICT) {
        AerDict* d = heap_alloc(&vm->heap, &vm->heap.dict_pool);
        memset(&d->map, 0, sizeof(d->map));
        d->map.pools = &vm->heap.dict_hash_pools;
        d->dirty_cards = NULL;
        d->dirty_cards_bytes = 0;
        d->dirty_min_byte = (unsigned int)-1;
        d->dirty_max_byte = 0;
        d->dirty_all = false;
        return aer_dict_val(d);
    }
    return dflt;
}

/* Cross-module call setup (aer_module_call): mirrors h_call_value's frame-push, standalone
   since this isn't inside vm_run's dispatch loop. dest_reg fixed at 0 sets up the frame right
   above target's own frame 0, so once vm_run(target) drains back to depth 0, the result sits
   in target->call_stack[0].registers[0]. */
/* Call setup                                                       */

/* Makes room for a frame starting at base: a free call-stack entry, and FRAME_REGISTERS slots of bank
   from base. False only when the depth ceiling is reached, the caller's overflow error. Growing the
   bank moves every frame's registers, so the caller recomputes anything it derived from them. */
static bool vm_grow_for_push(VM* vm, AerVal* base) {
    if (vm->call_depth + 1 >= vm->call_depth_limit) {
        if (vm->call_depth_limit >= VM_CALL_MAX)
            return false;
        int old_frames = vm->call_depth_limit;
        int frames = old_frames * 2 > VM_CALL_MAX ? VM_CALL_MAX : old_frames * 2;
        vm->call_stack = xrealloc(vm->call_stack, (size_t)frames * sizeof(CallFrame));
        memset(vm->call_stack + old_frames, 0, (size_t)(frames - old_frames) * sizeof(CallFrame));
        vm->call_depth_limit = frames;
    }
    size_t needed = (size_t)(base - vm->register_stack) + FRAME_REGISTERS;
    if (needed > vm->register_capacity) {
        size_t capacity = vm->register_capacity * 2;
        while (capacity < needed)
            capacity *= 2;
        AerVal* old_bank = vm->register_stack;
        AerVal* bank = xmalloc(capacity * sizeof(AerVal));
        memcpy(bank, old_bank, vm->register_capacity * sizeof(AerVal));
        /* Rebased against the old bank while it is still allocated, so no offset has to be stashed. */
        for (int i = 0; i <= vm->call_depth; i++)
            vm->call_stack[i].registers = bank + (vm->call_stack[i].registers - old_bank);
        free(old_bank);
        vm->register_stack = bank;
        vm->register_capacity = capacity;
        vm->push_base_limit = bank + (capacity - FRAME_REGISTERS);
    }
    return true;
}

bool setup_call(VM* target, ChunkFunction* fn, int arg_count, AerVal* args, unsigned int return_ip) {
    if (arg_count < (int)fn->min_arity || arg_count > (int)fn->arity) {
        if (fn->min_arity == fn->arity)
            error("Function expects %u arguments, got %d", fn->arity, arg_count);
        else
            error("Function expects between %u and %u arguments, got %d", fn->min_arity, fn->arity,
                  arg_count);
        return false;
    }
    CallFrame* caller = &target->call_stack[target->call_depth];
    AerVal* callee_regs = caller->registers + caller->frame_size;
    if (target->call_depth + 1 >= target->call_depth_limit || callee_regs > target->push_base_limit) {
        if (!vm_grow_for_push(target, callee_regs)) {
            error("Call stack overflow (max %d frames); tail calls do not consume one", VM_CALL_MAX);
            return false;
        }
        caller = &target->call_stack[target->call_depth];
        callee_regs = caller->registers + caller->frame_size;
    }
    CallFrame* callee = caller + 1;
    callee->registers = callee_regs;
    callee->frame_size = fn->max_registers;
    for (int i = 0; i < arg_count; i++)
        callee_regs[i] = args[i];
    for (int i = arg_count; i < (int)fn->arity; i++)
        callee_regs[i] = vm_default_value(target, fn->defaults[i - fn->min_arity]);
    /* Same reason as h_call's own -- everything below frame_size gets traced. */
    frame_init_tags(callee_regs, fn->arity, fn->frame_bounds);
    callee->frame_bounds = fn->frame_bounds;
    callee->return_ip = return_ip;
    callee->dest_reg = 0;
    callee->code_offset = fn->code_offset;
    callee->tail_calls_collapsed = 0;
    callee->synthetic_entry = true;
    target
        ->call_depth++; /* same rooting rule as vm_call_value's non-tail branch (above) -- the defaults loop wrote into callee->registers[] before this point */
    gc_maybe_collect(target);
    target->ip = fn->code_offset;
    return true;
}

/* Factored out of h_call_value since a plain function can't itself jump to a vm_run-local
   label -- it does the work and lets the caller DISPATCH(). return_ip is the caller's own
   resume address, passed explicitly rather than read from vm->ip. */
static void vm_call_value(VM* vm, AerVal fv, int dest_reg, int arg_reg_base, int arg_count, bool is_tail_call,
                          unsigned int return_ip) {
    if (aer_type(fv) != TYPE_FUNCTION) {
        return error("Value of type '%s' is not callable", vm_type_name(vm->chunk, fv));
    }
    AerFunction* f = aer_as_function(fv);
    if (arg_count < (int)f->min_arity || arg_count > (int)f->arity) {
        if (f->min_arity == f->arity)
            error("Function expects %u arguments, got %d", (unsigned int)f->arity, arg_count);
        else
            return error("Function expects between %u and %u arguments, got %d", (unsigned int)f->min_arity,
                         (unsigned int)f->arity, arg_count);
    }
    /* Tail-call reuse -- `f` is already a plain pointer, so overwriting its source register during
       the copy can't invalidate it. Args copied before defaults, so no source register is
       overwritten before it's read. */
    if (is_tail_call) {
        AerVal* cur = vm->call_stack[vm->call_depth].registers;
        for (int i = 0; i < arg_count; i++)
            cur[i] = cur[arg_reg_base + i];
        for (int i = arg_count; i < (int)f->arity; i++)
            cur[i] = vm_default_value(vm, f->defaults[i - f->min_arity]);
        gc_maybe_collect(
            vm); /* defaults just written into the CURRENT frame (tail call, call_depth unchanged) -- already rooted */
        vm->ip = f->code_offset;
        CallFrame* reused = &vm->call_stack[vm->call_depth];
        reused->code_offset = f->code_offset; /* reused frame now runs a different function */
        /* Same stale-sizing hazard as h_call's OP_TAIL_CALL branch (see its own comment) -- `f`
           may need a different max_registers than whatever function last occupied this frame.
           Must be refreshed here too, not just on the non-tail push path above. */
        reused->frame_size = f->max_registers;
        /* Growing the reused frame exposes registers the previous occupant never wrote, which
           mark_vm_roots would still trace -- same retagging as the two push paths. */
        frame_init_tags(reused->registers, f->arity, f->frame_bounds);
        reused->frame_bounds = f->frame_bounds;
        reused->tail_calls_collapsed++;
        return;
    }
    CallFrame* caller = &vm->call_stack[vm->call_depth];
    AerVal* callee_regs = caller->registers + caller->frame_size;
    if (vm->call_depth + 1 >= vm->call_depth_limit || callee_regs > vm->push_base_limit) {
        if (!vm_grow_for_push(vm, callee_regs))
            return error("Call stack overflow (max %d frames); tail calls do not consume one", VM_CALL_MAX);
        caller = &vm->call_stack[vm->call_depth];
        callee_regs = caller->registers + caller->frame_size;
    }
    CallFrame* callee = caller + 1;
    callee->registers = callee_regs;
    callee->frame_size = f->max_registers;
    for (int i = 0; i < arg_count; i++)
        callee_regs[i] = caller->registers[arg_reg_base + i];
    for (int i = arg_count; i < (int)f->arity; i++)
        callee_regs[i] = vm_default_value(vm, f->defaults[i - f->min_arity]);
    /* Same reason as h_call's own -- everything below frame_size gets traced. */
    frame_init_tags(callee_regs, f->arity, f->frame_bounds);
    callee->frame_bounds = f->frame_bounds;
    callee->return_ip = return_ip;
    callee->dest_reg = dest_reg;
    callee->code_offset = f->code_offset;
    callee->tail_calls_collapsed = 0;
    callee->synthetic_entry = false;
    vm->call_depth++; /* the defaults loop above wrote into callee->registers[] BEFORE this point, when mark_vm_roots's 0..call_depth scan didn't yet cover that frame -- gc_maybe_collect() must run AFTER this increment, not before, or a collection could reclaim a fresh default array/dict as unreachable */
    gc_maybe_collect(vm);
    vm->ip = f->code_offset;
}

/* Resolves field_idx within shape via the per-site inline cache (keyed by bytecode offset) --
   shared by both boxed-struct and packed-array callers, since a field's slot within a given
   Shape is identical either way. False (error reported) if shape has no such field. */
/* Field and element access -- struct, packed, typed                */

static inline __attribute__((always_inline)) bool
vm_resolve_field_by_shape(Chunk* c, unsigned int site, Shape* shape, int field_idx, int* out_slot,
                          unsigned int* out_offset, ValueType* out_ftype, bool* out_narrow) {
    FieldCacheEntry* entry = &c->field_cache[site];
    if (entry->shape == shape) {
        *out_slot = entry->slot;
        *out_offset = entry->offset;
        *out_ftype = entry->ftype;
        *out_narrow = entry->narrow;
        return true;
    }
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == (unsigned int)field_idx) {
            entry->shape = shape;
            entry->slot = (int)i;
            entry->offset = shape->field_offsets[i];
            entry->ftype = shape->field_types[i];
            entry->narrow = shape->field_narrow[i];
            *out_slot = (int)i;
            *out_offset = entry->offset;
            *out_ftype = entry->ftype;
            *out_narrow = entry->narrow;
            return true;
        }
    }
    error("'%s' has no field '%s'", aer_as_string(c->pool[shape->name])->data,
          aer_as_string(c->pool[field_idx])->data);
    return false;
}

/* Resolves struct_reg's field to an (AerStruct*, slot, offset, ftype, narrow) tuple, delegating the
   shape+cache lookup above. Shared by every field-access opcode. False (error reported) if not a
   struct instance or no such field. */
static inline __attribute__((always_inline)) bool vm_resolve_field(AerVal* registers, Chunk* c,
                                                                   unsigned int site, int struct_reg,
                                                                   int field_idx, AerStruct** out_s,
                                                                   int* out_slot, unsigned int* out_offset,
                                                                   ValueType* out_ftype, bool* out_narrow) {
    AerVal* obj = &registers[struct_reg];
    if (obj->tag != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        return false;
    }
    AerStruct* s = (AerStruct*)obj->as.ptr;
    *out_s = s;
    return vm_resolve_field_by_shape(c, site, s->shape, field_idx, out_slot, out_offset, out_ftype,
                                     out_narrow);
}

/* Reads one 8-byte packed slot as AerVal -- no switch on field type needed since AerVal.as is
   exactly 8 bytes and every eligible type's bit pattern matches directly. A 3-way switch here
   measurably regressed packed arrays on ARM -- keep this branchless. */
static inline AerVal vm_packed_slot_read(unsigned char* slot, ValueType ftype) {
    AerVal v;
    v.tag = ftype;
    memcpy(&v.as, slot, 8);
    return v;
}

/* Inverse of vm_packed_slot_read. Caller must already have type-checked v. */
static inline void vm_packed_slot_write(unsigned char* slot, ValueType ftype, AerVal v) {
    (void)ftype;
    memcpy(slot, &v.as, 8);
}

/* Deliberately not merged with vm_packed_slot_read/write above: that function's own comment records
   a measured ARM regression from adding exactly this kind of branch to its hot path. Not
   file-static because gc.c needs it; -flto still inlines it at vm.c's own call sites. */
unsigned int vm_typed_elem_width(TypedArrayElemKind kind) {
    if (kind == TYPED_ELEM_INT32 || kind == TYPED_ELEM_FLOAT32)
        return 4;
    return kind == TYPED_ELEM_BOOL ? 1 : 8;
}

static inline AerVal vm_typed_elem_read(unsigned char* slot, TypedArrayElemKind kind) {
    switch (kind) {
        case TYPED_ELEM_INT32: {
            int32_t v;
            memcpy(&v, slot, 4);
            return aer_int(v);
        }
        case TYPED_ELEM_FLOAT32: {
            float v;
            memcpy(&v, slot, 4);
            return aer_real((double)v);
        }
        case TYPED_ELEM_INT64: {
            int64_t v;
            memcpy(&v, slot, 8);
            return aer_int(v);
        }
        case TYPED_ELEM_FLOAT64: {
            double v;
            memcpy(&v, slot, 8);
            return aer_real(v);
        }
        case TYPED_ELEM_BOOL: return aer_bool(*slot != 0);
    }
    return aer_null();
}

/* Caller must already have validated v against kind -- see vm_typed_array_check. int32 in
   particular is never silently wrapped: an out-of-range value is rejected there, not truncated
   here. */
static inline void vm_typed_elem_write(unsigned char* slot, TypedArrayElemKind kind, AerVal v) {
    switch (kind) {
        case TYPED_ELEM_INT32: {
            int32_t iv = (int32_t)aer_as_int(v);
            memcpy(slot, &iv, 4);
            break;
        }
        case TYPED_ELEM_FLOAT32: {
            float fv = (float)(aer_type(v) == TYPE_INTEGER ? (double)aer_as_int(v) : aer_as_real(v));
            memcpy(slot, &fv, 4);
            break;
        }
        case TYPED_ELEM_INT64: {
            int64_t iv = aer_as_int(v);
            memcpy(slot, &iv, 8);
            break;
        }
        case TYPED_ELEM_FLOAT64: {
            double dv = (aer_type(v) == TYPE_INTEGER ? (double)aer_as_int(v) : aer_as_real(v));
            memcpy(slot, &dv, 8);
            break;
        }
        case TYPED_ELEM_BOOL: *slot = aer_as_bool(v) ? 1 : 0; break;
    }
}

static inline AerVal vm_narrow_field_read(unsigned char* p, ValueType ftype) {
    if (ftype == TYPE_INTEGER) {
        int32_t v;
        memcpy(&v, p, 4);
        return aer_int(v);
    }
    float v;
    memcpy(&v, p, 4);
    return aer_real((double)v);
}

/* The field's declared type is already enforced on v, so a float32 field only ever receives a real. */
static inline void vm_narrow_field_write(unsigned char* p, ValueType ftype, AerVal v) {
    if (ftype == TYPE_INTEGER) {
        int32_t iv = (int32_t)aer_as_int(v);
        memcpy(p, &iv, 4);
        return;
    }
    float fv = (float)aer_as_real(v);
    memcpy(p, &fv, 4);
}

/* -O2 vectorizes none of these and -O3 alone skips the float kinds, hence the per-function
   attribute; fast-math is on the float32 pair only, so every other float op keeps strict IEEE 754.
   Worth ~2.2-2.4x while cache-resident, nothing past it -- the loop is bandwidth-bound there. */
#define AER_TYPED_ELEMENTWISE(name, ctype, op_expr)                                                          \
    static __attribute__((optimize("O3", "tree-vectorize"))) void name(                                      \
        ctype* restrict c, const ctype* restrict a, const ctype* restrict b, unsigned int n) {               \
        for (unsigned int i = 0; i < n; i++)                                                                 \
            c[i] = op_expr;                                                                                  \
    }
#define AER_TYPED_ELEMENTWISE_FASTMATH(name, ctype, op_expr)                                                 \
    static __attribute__((optimize("O3", "tree-vectorize", "fast-math"))) void name(                         \
        ctype* restrict c, const ctype* restrict a, const ctype* restrict b, unsigned int n) {               \
        for (unsigned int i = 0; i < n; i++)                                                                 \
            c[i] = op_expr;                                                                                  \
    }

AER_TYPED_ELEMENTWISE(typed_add_i32, int32_t, a[i] + b[i])
AER_TYPED_ELEMENTWISE(typed_sub_i32, int32_t, a[i] - b[i])
AER_TYPED_ELEMENTWISE(typed_mul_i32, int32_t, a[i] * b[i])
AER_TYPED_ELEMENTWISE(typed_add_i64, int64_t, a[i] + b[i])
AER_TYPED_ELEMENTWISE(typed_sub_i64, int64_t, a[i] - b[i])
AER_TYPED_ELEMENTWISE(typed_mul_i64, int64_t, a[i] * b[i])
AER_TYPED_ELEMENTWISE(typed_add_f64, double, a[i] + b[i])
AER_TYPED_ELEMENTWISE(typed_sub_f64, double, a[i] - b[i])
AER_TYPED_ELEMENTWISE(typed_mul_f64, double, a[i] * b[i])
AER_TYPED_ELEMENTWISE_FASTMATH(typed_add_f32, float, a[i] + b[i])
AER_TYPED_ELEMENTWISE_FASTMATH(typed_sub_f32, float, a[i] - b[i])
AER_TYPED_ELEMENTWISE_FASTMATH(typed_mul_f32, float, a[i] * b[i])

/* The same kernels against one broadcast value rather than a second array. `s` is passed already
   narrowed to the element type, so the loop body is the element type throughout and vectorizes as
   the two-array form does. Subtraction needs both orders; add and multiply commute. */
#define AER_TYPED_SCALAR(name, ctype, op_expr)                                                               \
    static __attribute__((optimize("O3", "tree-vectorize"))) void name(                                      \
        ctype* restrict c, const ctype* restrict a, ctype s, unsigned int n) {                               \
        for (unsigned int i = 0; i < n; i++)                                                                 \
            c[i] = op_expr;                                                                                  \
    }
#define AER_TYPED_SCALAR_FASTMATH(name, ctype, op_expr)                                                      \
    static __attribute__((optimize("O3", "tree-vectorize", "fast-math"))) void name(                         \
        ctype* restrict c, const ctype* restrict a, ctype s, unsigned int n) {                               \
        for (unsigned int i = 0; i < n; i++)                                                                 \
            c[i] = op_expr;                                                                                  \
    }

#define AER_TYPED_SCALAR_SET(sfx, ctype, DEF)                                                                \
    DEF(typed_adds_##sfx, ctype, a[i] + s)                                                                   \
    DEF(typed_subs_##sfx, ctype, a[i] - s)                                                                   \
    DEF(typed_rsubs_##sfx, ctype, s - a[i])                                                                  \
    DEF(typed_muls_##sfx, ctype, a[i] * s)

AER_TYPED_SCALAR_SET(i32, int32_t, AER_TYPED_SCALAR)
AER_TYPED_SCALAR_SET(i64, int64_t, AER_TYPED_SCALAR)
AER_TYPED_SCALAR_SET(f64, double, AER_TYPED_SCALAR)
AER_TYPED_SCALAR_SET(f32, float, AER_TYPED_SCALAR_FASTMATH)

/* Division answers in float64 whatever it divides, because `5 / 2` is 2.5 everywhere else in the
   language and a column should not be the exception. That makes it the one operator whose result
   kind is not its operands', so it is dispatched apart from the kinds-preserving set above.
   A zero divisor is collected as a flag rather than branched on, which keeps the loop vectorizing;
   the caller reports it with the same message the scalar path uses. */
#define AER_TYPED_DIV(name, ctype)                                                                           \
    static __attribute__((optimize("O3", "tree-vectorize"))) int name(                                       \
        double* restrict c, const ctype* restrict a, const ctype* restrict b, unsigned int n) {              \
        int zero = 0;                                                                                        \
        for (unsigned int i = 0; i < n; i++) {                                                               \
            zero |= (b[i] == 0);                                                                             \
            c[i] = (double)a[i] / (double)b[i];                                                              \
        }                                                                                                    \
        return zero;                                                                                         \
    }
#define AER_TYPED_DIV_SCALAR(name, ctype, num, den)                                                          \
    static __attribute__((optimize("O3", "tree-vectorize"))) void name(                                      \
        double* restrict c, const ctype* restrict a, double s, unsigned int n) {                             \
        for (unsigned int i = 0; i < n; i++)                                                                 \
            c[i] = (num) / (den);                                                                            \
    }

#define AER_TYPED_DIV_SET(sfx, ctype)                                                                        \
    AER_TYPED_DIV(typed_div_##sfx, ctype)                                                                    \
    AER_TYPED_DIV_SCALAR(typed_divs_##sfx, ctype, (double)a[i], s)                                           \
    AER_TYPED_DIV_SCALAR(typed_rdivs_##sfx, ctype, s, (double)a[i])

AER_TYPED_DIV_SET(i32, int32_t)
AER_TYPED_DIV_SET(i64, int64_t)
AER_TYPED_DIV_SET(f32, float)
AER_TYPED_DIV_SET(f64, double)

/* Both directions over any element kind, so the two dispatch sites read as one line each. */
#define TYPED_DIV_CALL(sfx, ctype)                                                                           \
    return typed_div_##sfx(out, (const ctype*)a->data, (const ctype*)b->data, a->count)

static int typed_div_run(double* out, const AerTypedArray* a, const AerTypedArray* b) {
    switch (a->elem_kind) {
        case TYPED_ELEM_INT32: TYPED_DIV_CALL(i32, int32_t);
        case TYPED_ELEM_INT64: TYPED_DIV_CALL(i64, int64_t);
        case TYPED_ELEM_FLOAT32: TYPED_DIV_CALL(f32, float);
        case TYPED_ELEM_FLOAT64: TYPED_DIV_CALL(f64, double);
        case TYPED_ELEM_BOOL: break;
    }
    return 0;
}
#undef TYPED_DIV_CALL

#define TYPED_DIVS_CALL(sfx, ctype)                                                                          \
    if (flip)                                                                                                \
        typed_rdivs_##sfx(out, (const ctype*)a->data, s, a->count);                                          \
    else                                                                                                     \
        typed_divs_##sfx(out, (const ctype*)a->data, s, a->count);                                           \
    break

static void typed_divs_run(double* out, const AerTypedArray* a, double s, bool flip) {
    switch (a->elem_kind) {
        case TYPED_ELEM_INT32: TYPED_DIVS_CALL(i32, int32_t);
        case TYPED_ELEM_INT64: TYPED_DIVS_CALL(i64, int64_t);
        case TYPED_ELEM_FLOAT32: TYPED_DIVS_CALL(f32, float);
        case TYPED_ELEM_FLOAT64: TYPED_DIVS_CALL(f64, double);
        case TYPED_ELEM_BOOL: break;
    }
}
#undef TYPED_DIVS_CALL

/* Comparisons answer 1 or 0 in the ELEMENT's own type rather than a separate boolean array, which
   is what lets a mask compose with the arithmetic already here: `sum(price * (price < 400.0))` is a
   filtered total with no new reduction and no new opcode. Fast-math is deliberately NOT applied --
   it lets the compiler assume no NaN, and a comparison is exactly where that shows. */
#define AER_TYPED_CMP(name, ctype, op)                                                                       \
    static __attribute__((optimize("O3", "tree-vectorize"))) void name(                                      \
        ctype* restrict r, const ctype* restrict a, const ctype* restrict b, unsigned int n) {               \
        for (unsigned int i = 0; i < n; i++)                                                                 \
            r[i] = (ctype)(a[i] op b[i] ? 1 : 0);                                                            \
    }
#define AER_TYPED_CMP_SCALAR(name, ctype, op)                                                                \
    static __attribute__((optimize("O3", "tree-vectorize"))) void name(                                      \
        ctype* restrict r, const ctype* restrict a, ctype s, unsigned int n) {                               \
        for (unsigned int i = 0; i < n; i++)                                                                 \
            r[i] = (ctype)(a[i] op s ? 1 : 0);                                                               \
    }

#define AER_TYPED_CMP_SET(sfx, ctype)                                                                        \
    AER_TYPED_CMP(typed_lt_##sfx, ctype, <)                                                                  \
    AER_TYPED_CMP(typed_lte_##sfx, ctype, <=)                                                                \
    AER_TYPED_CMP(typed_gt_##sfx, ctype, >)                                                                  \
    AER_TYPED_CMP(typed_gte_##sfx, ctype, >=)                                                                \
    AER_TYPED_CMP(typed_eq_##sfx, ctype, ==)                                                                 \
    AER_TYPED_CMP(typed_ne_##sfx, ctype, !=)                                                                 \
    AER_TYPED_CMP_SCALAR(typed_lts_##sfx, ctype, <)                                                          \
    AER_TYPED_CMP_SCALAR(typed_ltes_##sfx, ctype, <=)                                                        \
    AER_TYPED_CMP_SCALAR(typed_gts_##sfx, ctype, >)                                                          \
    AER_TYPED_CMP_SCALAR(typed_gtes_##sfx, ctype, >=)                                                        \
    AER_TYPED_CMP_SCALAR(typed_eqs_##sfx, ctype, ==)                                                         \
    AER_TYPED_CMP_SCALAR(typed_nes_##sfx, ctype, !=)

AER_TYPED_CMP_SET(i32, int32_t)
AER_TYPED_CMP_SET(i64, int64_t)
AER_TYPED_CMP_SET(f32, float)
AER_TYPED_CMP_SET(f64, double)

#undef AER_TYPED_CMP
#undef AER_TYPED_CMP_SCALAR
#undef AER_TYPED_CMP_SET
#undef AER_TYPED_ELEMENTWISE
#undef AER_TYPED_ELEMENTWISE_FASTMATH
#undef AER_TYPED_SCALAR
#undef AER_TYPED_SCALAR_FASTMATH
#undef AER_TYPED_SCALAR_SET

/* Checks the free-cache (vm.h's own comment on TypedArrayFreeSlot) for a buffer of EXACTLY this
   size before falling back to xmalloc -- linear scan over a handful of slots, cheap regardless of
   hit or miss. Claimed slots are cleared (size = 0) so a later free_typed_array (gc.c) can reuse
   them for a different buffer. */
/* Typed arrays -- allocation and elementwise ops                   */

static unsigned char* typed_array_data_alloc(VmHeap* heap, size_t size) {
    for (unsigned int i = 0; i < TYPED_ARRAY_FREE_CACHE_SLOTS; i++) {
        if (heap->typed_array_free_cache[i].size == size) {
            unsigned char* p = heap->typed_array_free_cache[i].ptr;
            heap->typed_array_free_cache[i].size = 0;
            heap->typed_array_free_cache[i].ptr = NULL;
            heap->typed_array_free_cache_bytes -= size;
            return p;
        }
    }
    /* A hit above allocated nothing, so only a miss counts toward the next collection. */
    heap->young_bytes += size;
    return xmalloc(size);
}

static AerTypedArray* vm_new_typed_array(TypedArrayElemKind kind, unsigned int count) {
    VmHeap* heap = vm_require_current_heap();
    AerTypedArray* ta = heap_alloc(heap, &heap->typed_array_pool);
    ta->count = count;
    ta->elem_kind = kind;
    unsigned int width = vm_typed_elem_width(kind);
    ta->data = count > 0 ? typed_array_data_alloc(heap, (size_t)count * width) : NULL;
    return ta;
}

/* A zeroed typed array as a value -- exposed for actor.receive(), which rebuilds one from the raw
   bytes a message carried. */
AerVal vm_new_typed_array_val(TypedArrayElemKind kind, unsigned int count) {
    return aer_typed_array_val(vm_new_typed_array(kind, count));
}

/* Fuses (A op1 B) op2 C over three typed arrays into one pass, materializing no intermediate.
   Emitted only when the parser sees the whole expression at once. The ~1.65x win is from memory
   traffic -- two passes over 3 arrays become one over 4 -- not from vectorization. ADD/SUB/MUL
   only; any other operator, or operands that aren't all matching typed arrays at runtime, falls
   back to the unfused computation below. */
#define AER_TYPED_CHAIN2(name, ctype, expr)                                                                  \
    static __attribute__((optimize("O3", "tree-vectorize"))) void name(                                      \
        ctype* restrict r, const ctype* restrict a, const ctype* restrict b, const ctype* restrict cc,       \
        unsigned int n) {                                                                                    \
        for (unsigned int i = 0; i < n; i++)                                                                 \
            r[i] = expr;                                                                                     \
    }
#define AER_TYPED_CHAIN2_FASTMATH(name, ctype, expr)                                                         \
    static __attribute__((optimize("O3", "tree-vectorize", "fast-math"))) void name(                         \
        ctype* restrict r, const ctype* restrict a, const ctype* restrict b, const ctype* restrict cc,       \
        unsigned int n) {                                                                                    \
        for (unsigned int i = 0; i < n; i++)                                                                 \
            r[i] = expr;                                                                                     \
    }

AER_TYPED_CHAIN2(typed_chain_f64_add_add, double, (a[i] + b[i]) + cc[i])
AER_TYPED_CHAIN2(typed_chain_f64_add_sub, double, (a[i] + b[i]) - cc[i])
AER_TYPED_CHAIN2(typed_chain_f64_add_mul, double, (a[i] + b[i]) * cc[i])
AER_TYPED_CHAIN2(typed_chain_f64_sub_add, double, (a[i] - b[i]) + cc[i])
AER_TYPED_CHAIN2(typed_chain_f64_sub_sub, double, (a[i] - b[i]) - cc[i])
AER_TYPED_CHAIN2(typed_chain_f64_sub_mul, double, (a[i] - b[i]) * cc[i])
AER_TYPED_CHAIN2(typed_chain_f64_mul_add, double, (a[i] * b[i]) + cc[i])
AER_TYPED_CHAIN2(typed_chain_f64_mul_sub, double, (a[i] * b[i]) - cc[i])
AER_TYPED_CHAIN2(typed_chain_f64_mul_mul, double, (a[i] * b[i]) * cc[i])
AER_TYPED_CHAIN2_FASTMATH(typed_chain_f32_add_add, float, (a[i] + b[i]) + cc[i])
AER_TYPED_CHAIN2_FASTMATH(typed_chain_f32_add_sub, float, (a[i] + b[i]) - cc[i])
AER_TYPED_CHAIN2_FASTMATH(typed_chain_f32_add_mul, float, (a[i] + b[i]) * cc[i])
AER_TYPED_CHAIN2_FASTMATH(typed_chain_f32_sub_add, float, (a[i] - b[i]) + cc[i])
AER_TYPED_CHAIN2_FASTMATH(typed_chain_f32_sub_sub, float, (a[i] - b[i]) - cc[i])
AER_TYPED_CHAIN2_FASTMATH(typed_chain_f32_sub_mul, float, (a[i] - b[i]) * cc[i])
AER_TYPED_CHAIN2_FASTMATH(typed_chain_f32_mul_add, float, (a[i] * b[i]) + cc[i])
AER_TYPED_CHAIN2_FASTMATH(typed_chain_f32_mul_sub, float, (a[i] * b[i]) - cc[i])
AER_TYPED_CHAIN2_FASTMATH(typed_chain_f32_mul_mul, float, (a[i] * b[i]) * cc[i])

#undef AER_TYPED_CHAIN2
#undef AER_TYPED_CHAIN2_FASTMATH

/* Dispatch table for the 9 op1xop2 combinations above, keyed by (op1,op2) -- built once, checked
   by row/col index rather than a 9-way if/else chain. Row/col order matches op_chain2_index's own
   mapping (ADD=0, SUB=1, MUL=2). */
typedef void (*TypedChain2FnF64)(double*, const double*, const double*, const double*, unsigned int);
typedef void (*TypedChain2FnF32)(float*, const float*, const float*, const float*, unsigned int);
static const TypedChain2FnF64 typed_chain2_f64[3][3] = {
    {typed_chain_f64_add_add, typed_chain_f64_add_sub, typed_chain_f64_add_mul},
    {typed_chain_f64_sub_add, typed_chain_f64_sub_sub, typed_chain_f64_sub_mul},
    {typed_chain_f64_mul_add, typed_chain_f64_mul_sub, typed_chain_f64_mul_mul},
};
static const TypedChain2FnF32 typed_chain2_f32[3][3] = {
    {typed_chain_f32_add_add, typed_chain_f32_add_sub, typed_chain_f32_add_mul},
    {typed_chain_f32_sub_add, typed_chain_f32_sub_sub, typed_chain_f32_sub_mul},
    {typed_chain_f32_mul_add, typed_chain_f32_mul_sub, typed_chain_f32_mul_mul},
};

/* -1 if op isn't one of the 3 fusable arithmetic operators -- caller treats that as "can't fuse,
   fall back to the exact unfused computation" rather than an error (a chain using, say, OP_DIV or
   a comparison is still perfectly valid AER code, just not one this fast path covers). */
static inline int op_chain2_index(Opcode op) {
    switch (op) {
        case OP_ADD: return 0;
        case OP_SUB: return 1;
        case OP_MUL: return 2;
        default: return -1;
    }
}

/* The caller has already checked that a, b and cc share an elem_kind and count, and that the kind
   is float. */
static AerVal vm_typed_array_chain2(AerTypedArray* a, AerTypedArray* b, AerTypedArray* cc, Opcode op1,
                                    Opcode op2) {
    int i1 = op_chain2_index(op1), i2 = op_chain2_index(op2);
    AerTypedArray* r = vm_new_typed_array(a->elem_kind, a->count);
    if (a->elem_kind == TYPED_ELEM_FLOAT64) {
        typed_chain2_f64[i1][i2]((double*)r->data, (const double*)a->data, (const double*)b->data,
                                 (const double*)cc->data, a->count);
    } else {
        typed_chain2_f32[i1][i2]((float*)r->data, (const float*)a->data, (const float*)b->data,
                                 (const float*)cc->data, a->count);
    }
    return aer_typed_array_val(r);
}

/* a and b must already have the same elem_kind and count -- checked by the caller (vm_binary_cold),
   since the error message there names the actual operator, which this function doesn't have. */
static AerVal vm_reject_boolean_column(Opcode op) {
    error("'%s' is not defined on a boolean[] -- it holds true and false, not numbers", binop_symbol(op));
    return aer_bool(false);
}

static AerVal vm_typed_array_binary_op(AerTypedArray* a, AerTypedArray* b, Opcode op) {
    if (a->elem_kind == TYPED_ELEM_BOOL || b->elem_kind == TYPED_ELEM_BOOL)
        return vm_reject_boolean_column(op);
    if (a->elem_kind != b->elem_kind) {
        error("Cannot combine typed arrays of different element kinds");
        return aer_bool(false);
    }
    if (a->count != b->count) {
        error("Typed arrays must have the same length (got %u and %u)", a->count, b->count);
        return aer_bool(false);
    }
    if (op == OP_DIV) {
        AerVal out = aer_typed_array_val(vm_new_typed_array(TYPED_ELEM_FLOAT64, a->count));
        if (a->count > 0 && typed_div_run((double*)aer_as_typed_array(out)->data, a, b)) {
            error("Division by zero");
            return aer_bool(false);
        }
        return out;
    }
    AerTypedArray* r = vm_new_typed_array(a->elem_kind, a->count);
/* A comparison answers in the element type, so it slots into the same per-kind branch. */
#define AER_CMP_DISPATCH(sfx, rc, ra, rb, n)                                                                 \
    if (op == OP_LT) {                                                                                       \
        typed_lt_##sfx(rc, ra, rb, n);                                                                       \
        break;                                                                                               \
    } else if (op == OP_LTE) {                                                                               \
        typed_lte_##sfx(rc, ra, rb, n);                                                                      \
        break;                                                                                               \
    } else if (op == OP_GT) {                                                                                \
        typed_gt_##sfx(rc, ra, rb, n);                                                                       \
        break;                                                                                               \
    } else if (op == OP_GTE) {                                                                               \
        typed_gte_##sfx(rc, ra, rb, n);                                                                      \
        break;                                                                                               \
    } else if (op == OP_EQ) {                                                                                \
        typed_eq_##sfx(rc, ra, rb, n);                                                                       \
        break;                                                                                               \
    } else if (op == OP_NEQ) {                                                                               \
        typed_ne_##sfx(rc, ra, rb, n);                                                                       \
        break;                                                                                               \
    }
    switch (a->elem_kind) {
        case TYPED_ELEM_INT32: {
            int32_t* rc = (int32_t*)r->data;
            const int32_t* ra = (const int32_t*)a->data;
            const int32_t* rb = (const int32_t*)b->data;
            AER_CMP_DISPATCH(i32, rc, ra, rb, a->count)
            if (op == OP_ADD)
                typed_add_i32(rc, ra, rb, a->count);
            else if (op == OP_SUB)
                typed_sub_i32(rc, ra, rb, a->count);
            else
                typed_mul_i32(rc, ra, rb, a->count);
            break;
        }
        case TYPED_ELEM_INT64: {
            int64_t* rc = (int64_t*)r->data;
            const int64_t* ra = (const int64_t*)a->data;
            const int64_t* rb = (const int64_t*)b->data;
            AER_CMP_DISPATCH(i64, rc, ra, rb, a->count)
            if (op == OP_ADD)
                typed_add_i64(rc, ra, rb, a->count);
            else if (op == OP_SUB)
                typed_sub_i64(rc, ra, rb, a->count);
            else
                typed_mul_i64(rc, ra, rb, a->count);
            break;
        }
        case TYPED_ELEM_FLOAT32: {
            float* rc = (float*)r->data;
            const float* ra = (const float*)a->data;
            const float* rb = (const float*)b->data;
            AER_CMP_DISPATCH(f32, rc, ra, rb, a->count)
            if (op == OP_ADD)
                typed_add_f32(rc, ra, rb, a->count);
            else if (op == OP_SUB)
                typed_sub_f32(rc, ra, rb, a->count);
            else
                typed_mul_f32(rc, ra, rb, a->count);
            break;
        }
        case TYPED_ELEM_FLOAT64: {
            double* rc = (double*)r->data;
            const double* ra = (const double*)a->data;
            const double* rb = (const double*)b->data;
            AER_CMP_DISPATCH(f64, rc, ra, rb, a->count)
            if (op == OP_ADD)
                typed_add_f64(rc, ra, rb, a->count);
            else if (op == OP_SUB)
                typed_sub_f64(rc, ra, rb, a->count);
            else
                typed_mul_f64(rc, ra, rb, a->count);
            break;
        }
        case TYPED_ELEM_BOOL: break;
    }
#undef AER_CMP_DISPATCH
    return aer_typed_array_val(r);
}

/* `a * 2.0` and `2.0 * a`. Broadcasting the scalar rather than requiring a second array of it is
   what lets a whole-array expression carry a constant at all -- without it any loop body with a
   literal in it has no array-level form to be written as. `flip` is set when the scalar was the
   left operand, which only changes subtraction. */
static AerVal vm_typed_array_scalar_op(AerTypedArray* a, AerVal scalar, Opcode op, bool flip) {
    if (a->elem_kind == TYPED_ELEM_BOOL)
        return vm_reject_boolean_column(op);
    double s = aer_type(scalar) == TYPE_INTEGER ? (double)aer_as_int(scalar) : aer_as_real(scalar);
    if (op == OP_DIV) {
        /* Only the scalar can be a zero divisor here, so it is checked once rather than per row. */
        if (!flip && s == 0.0) {
            error("Division by zero");
            return aer_bool(false);
        }
        AerVal out = aer_typed_array_val(vm_new_typed_array(TYPED_ELEM_FLOAT64, a->count));
        typed_divs_run((double*)aer_as_typed_array(out)->data, a, s, flip);
        return out;
    }
    AerTypedArray* r = vm_new_typed_array(a->elem_kind, a->count);
    unsigned int n = a->count;
    bool rsub = (op == OP_SUB && flip);
    /* With the scalar on the left the comparison reads the other way round: `400.0 > price` asks
       what `price < 400.0` asks. */
    Opcode cmp = op;
    if (flip) {
        if (op == OP_LT)
            cmp = OP_GT;
        else if (op == OP_GT)
            cmp = OP_LT;
        else if (op == OP_LTE)
            cmp = OP_GTE;
        else if (op == OP_GTE)
            cmp = OP_LTE;
    }
#define AER_CMPS_DISPATCH(sfx, rc, ra, sv, n)                                                                \
    if (cmp == OP_LT) {                                                                                      \
        typed_lts_##sfx(rc, ra, sv, n);                                                                      \
        break;                                                                                               \
    } else if (cmp == OP_LTE) {                                                                              \
        typed_ltes_##sfx(rc, ra, sv, n);                                                                     \
        break;                                                                                               \
    } else if (cmp == OP_GT) {                                                                               \
        typed_gts_##sfx(rc, ra, sv, n);                                                                      \
        break;                                                                                               \
    } else if (cmp == OP_GTE) {                                                                              \
        typed_gtes_##sfx(rc, ra, sv, n);                                                                     \
        break;                                                                                               \
    } else if (cmp == OP_EQ) {                                                                               \
        typed_eqs_##sfx(rc, ra, sv, n);                                                                      \
        break;                                                                                               \
    } else if (cmp == OP_NEQ) {                                                                              \
        typed_nes_##sfx(rc, ra, sv, n);                                                                      \
        break;                                                                                               \
    }
    switch (a->elem_kind) {
#define AER_SCALAR_CASE(KIND, sfx, ctype)                                                                    \
    case KIND: {                                                                                             \
        ctype* rc = (ctype*)r->data;                                                                         \
        const ctype* ra = (const ctype*)a->data;                                                             \
        ctype sv = (ctype)s;                                                                                 \
        AER_CMPS_DISPATCH(sfx, rc, ra, sv, n)                                                                \
        if (op == OP_ADD)                                                                                    \
            typed_adds_##sfx(rc, ra, sv, n);                                                                 \
        else if (op == OP_MUL)                                                                               \
            typed_muls_##sfx(rc, ra, sv, n);                                                                 \
        else if (rsub)                                                                                       \
            typed_rsubs_##sfx(rc, ra, sv, n);                                                                \
        else                                                                                                 \
            typed_subs_##sfx(rc, ra, sv, n);                                                                 \
        break;                                                                                               \
    }
        AER_SCALAR_CASE(TYPED_ELEM_INT32, i32, int32_t)
        AER_SCALAR_CASE(TYPED_ELEM_INT64, i64, int64_t)
        AER_SCALAR_CASE(TYPED_ELEM_FLOAT32, f32, float)
        AER_SCALAR_CASE(TYPED_ELEM_FLOAT64, f64, double)
#undef AER_SCALAR_CASE
        case TYPED_ELEM_BOOL: break;
    }
    return aer_typed_array_val(r);
}

/* One struct field at its Shape-computed offset: packed-slot access for a typed 8-byte field, the
   narrow reader/writer for a 4-byte one, a 16-byte memcpy for TYPE_ANY. Declared in vm.h because
   aer_json.c needs it too. Looks offset/ftype/narrow up fresh, so opcode call sites should use
   vm_struct_field_read_at below once the field cache has already resolved them. */
/* Struct field read/write -- boxed and narrow                      */

AerVal vm_struct_field_read(AerStruct* s, unsigned int slot) {
    ValueType ftype = s->shape->field_types[slot];
    unsigned int offset = s->shape->field_offsets[slot];
    unsigned char* p = s->fields + offset;
    if (ftype == TYPE_ANY) {
        AerVal v;
        memcpy(&v, p, sizeof(AerVal));
        return v;
    }
    if (s->shape->field_narrow[slot])
        return vm_narrow_field_read(p, ftype);
    return vm_packed_slot_read(p, ftype);
}

/* Natural literal shape, not exact-tag matching (unlike a typed struct field's OP_FIELD_SET) --
   `arr[i] = 5` into a float32 array shouldn't require writing `5.0`. int32/int64 variants still
   require TYPE_INTEGER; int32 additionally range-checks (erroring rather than silently wrapping,
   this project's established "fail loudly" convention -- see OP_CAST's own integer-overflow
   handling). float32/float64 accept TYPE_INTEGER or TYPE_REAL, promoted, matching the existing
   promotion convention in the raw-arithmetic `_boxed` handlers. */
static inline bool vm_typed_array_accepts(TypedArrayElemKind kind, AerVal val) {
    switch (kind) {
        case TYPED_ELEM_INT64: return aer_type(val) == TYPE_INTEGER;
        case TYPED_ELEM_INT32:
            return aer_type(val) == TYPE_INTEGER && aer_as_int(val) >= INT32_MIN &&
                   aer_as_int(val) <= INT32_MAX;
        case TYPED_ELEM_FLOAT32:
        case TYPED_ELEM_FLOAT64: return aer_type(val) == TYPE_INTEGER || aer_type(val) == TYPE_REAL;
        case TYPED_ELEM_BOOL: return aer_type(val) == TYPE_BOOLEAN;
    }
    return false;
}

/* The indefinite article for a type or element name: "an integer", "an int32", "a float", "a Point". */
static const char* article_for(const char* name) {
    return name[0] != '\0' && strchr("aeiouAEIOU", name[0]) ? "an" : "a";
}

static bool vm_typed_array_check(Chunk* c, TypedArrayElemKind kind, AerVal val) {
    if (vm_typed_array_accepts(kind, val))
        return true;
    const char* elem = aer_typed_elem_names[kind];
    const char* got = vm_type_name(c, val);
    if (kind == TYPED_ELEM_INT32 && aer_type(val) == TYPE_INTEGER)
        error("Value %lld out of range for an int32[] array", (long long)aer_as_int(val));
    else
        error("Cannot assign %s %s into %s %s[] array", article_for(got), got, article_for(elem), elem);
    return false;
}

/* Same "fail loudly, don't silently truncate" convention as vm_typed_array_check -- a narrow
   (int32) struct field additionally range-checks; a non-narrow field's ordinary
   `declared != TYPE_ANY && val->tag != declared` check (every OP_FIELD_SET-family handler's own)
   already confirms val really is an integer whenever ftype/declared is TYPE_INTEGER, so this only
   needs to add the extra range check on top of that, not re-verify the type itself. */
static inline bool vm_fits_narrow_field(ValueType ftype, bool narrow, AerVal val) {
    return !narrow || ftype != TYPE_INTEGER || (aer_as_int(val) >= INT32_MIN && aer_as_int(val) <= INT32_MAX);
}

static bool vm_check_narrow_field_write(ValueType ftype, bool narrow, AerVal val) {
    if (vm_fits_narrow_field(ftype, narrow, val))
        return true;
    error("Value %lld out of range for a narrow (int32) field", (long long)aer_as_int(val));
    return false;
}

/* Narrow (4-byte) counterparts of the raw-slot memcpy's the wide RAW opcode family (vm_run_slice's
   own OP_FIELD_GET_RAW_INT/REAL etc. handlers) uses -- widen into an ordinary int64_t/double
   registers[].as.i/registers[].as.d slot on read, narrow back on write. No range check on the int32 write side
   -- see the narrow RAW opcode family's own comment (vm.h) for why that's the intentional,
   consistent-with-every-other-raw-opcode tradeoff here. */
static inline int64_t vm_raw_read_int32(unsigned char* p) {
    int32_t v;
    memcpy(&v, p, 4);
    return (int64_t)v;
}
static inline void vm_raw_write_int32(unsigned char* p, int64_t v) {
    int32_t iv = (int32_t)v;
    memcpy(p, &iv, 4);
}
static inline double vm_raw_read_float32(unsigned char* p) {
    float v;
    memcpy(&v, p, 4);
    return (double)v;
}
static inline void vm_raw_write_float32(unsigned char* p, double v) {
    float fv = (float)v;
    memcpy(p, &fv, 4);
}

/* As above, but offset/ftype/narrow come from vm_resolve_field's cache instead of being re-derived
   from s->shape. The branches themselves are free at any monomorphic site; what this avoids is the
   extra pointer-chase through s->shape on every access. */
static inline AerVal vm_struct_field_read_at(AerStruct* s, unsigned int offset, ValueType ftype,
                                             bool narrow) {
    unsigned char* p = s->fields + offset;
    if (ftype == TYPE_ANY) {
        AerVal v;
        memcpy(&v, p, sizeof(AerVal));
        return v;
    }
    if (narrow)
        return vm_narrow_field_read(p, ftype);
    return vm_packed_slot_read(p, ftype);
}

static inline void vm_struct_field_write_at(AerStruct* s, unsigned int offset, ValueType ftype, bool narrow,
                                            AerVal v) {
    unsigned char* p = s->fields + offset;
    if (ftype == TYPE_ANY) {
        memcpy(p, &v, sizeof(AerVal));
        return;
    }
    if (narrow) {
        vm_narrow_field_write(p, ftype, v);
        return;
    }
    vm_packed_slot_write(p, ftype, v);
}

/* Returns an owned copy of dense[*idx]'s key. Shared by array-iteration's dict branch and
   pair-iteration. False once exhausted -- the dense array has no holes, so this is a plain
   bounds check, not a scan. */
/* Dict iteration                                                   */

static bool vm_dict_next_key(AerDict* d, int64_t* idx, AerVal* out_key) {
    if ((uint64_t)*idx >= d->map.count)
        return false;
    unsigned int key_len = d->map.dense[*idx].length;
    *out_key = aer_make_string_copy(d->map.dense[*idx].key,
                                    key_len); /* no chunk_add_pool interning -- see vm_to_str's comment */
    return true;
}

/* Value constructors                                               */

void vm_array_alloc_items(AerArray* a, unsigned int capacity) {
    vm_require_current_heap()->young_bytes += (size_t)capacity * sizeof(AerVal);
    a->items = xmalloc(sizeof(AerVal) * capacity);
    a->capacity = capacity;
}

void vm_array_grow_items(AerArray* a, unsigned int capacity) {
    vm_require_current_heap()->young_bytes += (size_t)(capacity - a->capacity) * sizeof(AerVal);
    a->items = xrealloc(a->items, sizeof(AerVal) * capacity);
    a->capacity = capacity;
}

AerArray* vm_new_array(unsigned int capacity) {
    return heap_new_array(vm_require_current_heap(), capacity);
}

AerDict* vm_new_dict(void) {
    VmHeap* heap = vm_require_current_heap();
    AerDict* d = heap_alloc(heap, &heap->dict_pool);
    /* Zeroed here, not left to each caller, so setting .pools below can't be wiped out by a
       caller's own zeroing running afterward. */
    memset(&d->map, 0, sizeof(d->map));
    d->map.pools = &heap->dict_hash_pools;
    /* pool_alloc only zeroes gc_state (byte 0) -- a reused cell's previous occupant's dirty_cards
       pointer would otherwise survive as garbage; see vm_new_array's identical reasoning. */
    d->dirty_cards = NULL;
    d->dirty_cards_bytes = 0;
    d->dirty_min_byte = (unsigned int)-1;
    d->dirty_max_byte = 0;
    d->dirty_all = false;
    return d;
}

AerVal aer_make_result(AerVal value, AerVal err) {
    VmHeap* heap = vm_require_current_heap();
    AerResult* r = heap_alloc(heap, &heap->result_pool);
    r->value = value;
    r->err = err;
    return aer_result_val(r);
}

AerVal aer_make_error(const char* msg) {
    return aer_make_string_copy(msg, (unsigned int)strlen(msg));
}

AerFunction* vm_new_function(void) {
    VmHeap* heap = vm_require_current_heap();
    return heap_alloc(heap, &heap->function_pool);
}

/* builtin_id resolved at parse time. Returns true if arg_count matched, result in *out. */
/* Builtin function calls                                           */

static bool vm_call_builtin(Chunk* c, int builtin_id, AerVal* args, int arg_count, AerVal* out) {
    *out = aer_null();

    switch (builtin_id) {
        case CALL_BUILTIN_LENGTH: {
            if (arg_count != 1)
                return false;
            AerVal a = args[0];
            if (aer_type(a) == TYPE_ARRAY)
                *out = aer_int((int64_t)aer_as_array(a)->count);
            else if (aer_type(a) == TYPE_STRING)
                *out = aer_int((int64_t)aer_as_string(a)->length);
            else if (aer_type(a) == TYPE_DICT)
                *out = aer_int((int64_t)aer_as_dict(a)->map.count);
            else if (aer_type(a) == TYPE_PACKED_ARRAY)
                *out = aer_int((int64_t)aer_as_packed_array(a)->count);
            else if (aer_type(a) == TYPE_TYPED_ARRAY)
                *out = aer_int((int64_t)aer_as_typed_array(a)->count);
            else
                error("length() requires an array, dict, string, packed array, or typed array");
            return true;
        }
        case CALL_BUILTIN_PRINT: {
            if (arg_count != 1)
                return false;
            vm_print_value(c, args[0], false);
            printf("\n");
            return true;
        }
        case CALL_BUILTIN_TYPE: {
            if (arg_count != 1)
                return false;
            const char* tn = vm_type_name(c, args[0]);
            /* Copies: an AerString always owns its data, with no exceptions. */
            *out = aer_make_string_copy(
                tn, (unsigned int)strlen(tn)); /* no chunk_add_pool interning -- see vm_to_str's comment */
            return true;
        }
        case CALL_BUILTIN_ASSERT: {
            if (arg_count != 2)
                return false;
            AerVal cond = args[0], msg = args[1];
            if (aer_type(msg) != TYPE_STRING) {
                error("assert() requires a string message as its second argument");
                return true;
            }
            if (!vm_truthy(cond)) {
                assert_failure_count++;
                AerString* ms = aer_as_string(msg);
                printf("ASSERT FAILED: %.*s\n", (int)ms->length, ms->data);
            }
            return true;
        }
        case CALL_BUILTIN_PANIC: {
            if (arg_count != 1)
                return false;
            AerVal msg = args[0];
            if (aer_type(msg) != TYPE_STRING) {
                error("panic() requires a string message");
                return true;
            }
            AerString* ms = aer_as_string(msg);
            error("panic: %.*s", (int)ms->length, ms->data);
            return true;
        }
        case CALL_BUILTIN_RESULT: {
            if (arg_count != 2)
                return false;
            bool value_is_null = aer_type(args[0]) == TYPE_NULL;
            bool err_is_null = aer_type(args[1]) == TYPE_NULL;
            if (value_is_null == err_is_null) {
                error("Result() requires exactly one of its two arguments to be null (the value on success, "
                      "the err on failure)");
                return true;
            }
            *out = aer_make_result(args[0], args[1]);
            return true;
        }
    }
    return false;
}

/* Where an array index lands in [0, count), counting from the end when negative. */
static inline bool vm_array_position(AerVal idx, unsigned int count, uint64_t* out) {
    if (aer_type(idx) != TYPE_INTEGER)
        return false;
    int64_t i = aer_as_int(idx);
    if (i < 0)
        i += (int64_t)count;
    if (i < 0 || (uint64_t)i >= count)
        return false;
    *out = (uint64_t)i;
    return true;
}

static void vm_array_index_error(AerVal idx, unsigned int count) {
    if (aer_type(idx) != TYPE_INTEGER)
        error("Array index must be an integer");
    else
        error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(idx), count);
}

/* Shared by h_index_get and the fused index-get handlers -- same dispatch, bounds, and
   errors as the original inline body, returning the value instead of pushing it. */
/* Writes through `out` (avoids a 16-byte stack round-trip returning by value -- see
   BINARY_OP_INT_REAL's comment). Safe even if `out` aliases obj's/idx's register. */
static inline void vm_index_get_compute(AerVal obj, AerVal idx, AerVal* out) {
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) {
            error("Struct fields are accessed with '.', not '[]'");
            *out = aer_null();
            return;
        }
        uint64_t i;
        if (!vm_array_position(idx, a->count, &i)) {
            vm_array_index_error(idx, a->count);
            *out = aer_null();
            return;
        }
        *out = a->items[i];
        return;
    } else if (aer_type(obj) == TYPE_DICT) {
        if (aer_type(idx) != TYPE_STRING) {
            error("Hashtable key must be a string");
            *out = aer_null();
            return;
        }
        AerString* is = aer_as_string(idx);
        unsigned int klen = hashtable_key_true_len(is->data, is->length);
        AerVal* found =
            hashtable_get_hashed(&aer_as_dict(obj)->map, is->data, klen, hashtable_string_hash(is, klen));
        if (!found) {
            *out = aer_null();
            return;
        }
        *out = *found;
        return;
    } else if (aer_type(obj) == TYPE_STRING) {
        AerString* os = aer_as_string(obj);
        if (aer_type(idx) != TYPE_INTEGER) {
            error("String index must be an integer");
            *out = aer_null();
            return;
        }
        int64_t i = aer_as_int(idx);
        int64_t len = (int64_t)os->length;
        if (i < 0)
            i += len;
        if (i < 0 || i >= len) {
            error("String index %lld out of bounds (len %lld)", (long long)aer_as_int(idx), (long long)len);
            *out = aer_null();
            return;
        }
        /* A character is a length-1 string; the byte is copied, since obj may be collected later. */
        *out = aer_make_string_copy(os->data + i, 1);
        return; /* no chunk_add_pool interning -- see vm_to_str's comment */
    } else if (aer_type(obj) == TYPE_RESULT) {
        /* result[0] is the value, result[1] is the err -- the same order every stdlib fallible
           function returns. */
        if (aer_type(idx) != TYPE_INTEGER) {
            error("Result index must be an integer");
            *out = aer_null();
            return;
        }
        int64_t i = aer_as_int(idx);
        AerResult* r = aer_as_result(obj);
        if (i == 0) {
            *out = r->value;
            return;
        }
        if (i == 1) {
            *out = r->err;
            return;
        }
        error("Result index %lld out of bounds (a Result only has indices 0 and 1)",
              (long long)aer_as_int(idx));
        *out = aer_null();
    } else if (aer_type(obj) == TYPE_TYPED_ARRAY) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        uint64_t i;
        if (!vm_array_position(idx, ta->count, &i)) {
            vm_array_index_error(idx, ta->count);
            *out = aer_null();
            return;
        }
        unsigned int width = vm_typed_elem_width(ta->elem_kind);
        *out = vm_typed_elem_read(ta->data + (size_t)i * width, ta->elem_kind);
    } else {
        error("This value cannot be indexed — indexing reads from an array, a column, a hashtable, "
              "a string, or a Result (0 for the value, 1 for the error)");
        *out = aer_null();
    }
}

/* vm_dict_get_interp's fallback: builds the key string for real, then takes the general index
   path. */
static AerVal vm_index_get_interp_slow(VM* vm, AerVal obj, const uint32_t* rks,
                                                                 unsigned int count, AerVal* registers,
                                                                 AerVal* pool) {
    AerVal parts[INTERP_MAX_PARTS];
    for (unsigned int i = 0; i < count; i++)
        parts[i] = *vm_rk_ptr16(registers, pool, rks[i]);
    AerVal out;
    vm_index_get_compute(obj, vm_interp_build(vm, parts, count), &out);
    return out;
}

/* `a, b = expr` -- a genuine Result unpacks to (value, err); a plain 2-element array (not a
   struct/packed array, both dot-only) unpacks positionally; anything else is treated as (that
   value, null), the same "not a real Result? just a plain value" duck-typing |> already applies on
   its own left operand. Lets a function that can never fail just `return value` without fabricating
   a second one to satisfy this shape. Never allocates -- both branches only copy existing AerVals. */
static inline void vm_destructure_compute(AerVal src, AerVal* out0, AerVal* out1) {
    if (aer_type(src) == TYPE_RESULT) {
        AerResult* r = aer_as_result(src);
        *out0 = r->value;
        *out1 = r->err;
        return;
    }
    if (aer_type(src) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(src);
        if (!a->shape && a->count == 2) {
            *out0 = a->items[0];
            *out1 = a->items[1];
            return;
        }
    }
    *out0 = src;
    *out1 = aer_null();
}

/* Shared by h_index_set and the fused index-set handlers -- same dispatch/bounds/errors.
   Caller must DISPATCH() immediately after. */
static inline void vm_index_set_compute(VM* vm, AerVal obj, AerVal idx, AerVal val) {
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) {
            return error("Struct fields are assigned with '.', not '[]'");
        }
        uint64_t i;
        if (!vm_array_position(idx, a->count, &i))
            return vm_array_index_error(idx, a->count);
        gc_barrier_array(vm, a, (unsigned int)i, val);
        a->items[i] = val;
        /* Replacing an element can change whether this array is uniformly one struct shape --
           invalidates h_call's SPEC_KIND_ARRAY_OF_STRUCTS "already verified homogeneous"
           per-call-site cache (vm.c), which is keyed on (array pointer, generation, shape). */
        a->generation++;
    } else if (aer_type(obj) == TYPE_DICT) {
        if (aer_type(idx) != TYPE_STRING) {
            return error("Hashtable key must be a string");
        }
        AerString* is = aer_as_string(idx);
        unsigned int klen = hashtable_key_true_len(is->data, is->length);
        HashValue khash = hashtable_string_hash(is, klen);
        AerDict* d = aer_as_dict(obj);
        /* Resolved BEFORE the write, not after -- an update reuses this exact dense index, a fresh
           key always lands at d->map.count (see gc_barrier_dict's own comment, gc.c). */
        int existing_idx = hashtable_get_index_hashed(&d->map, is->data, klen, khash);
        unsigned int write_idx = existing_idx >= 0 ? (unsigned int)existing_idx : d->map.count;
        gc_barrier_dict(vm, d, write_idx, val);
        if (existing_idx >= 0) {
            d->map.dense[existing_idx].payload = val; /* update in place -- no allocation */
        } else {
            char* k = hashtable_key_dup_known(d->map.pools, is->data, klen);
            hashtable_put_hashed(&d->map, k, klen, khash, val);
        }
    } else if (aer_type(obj) == TYPE_STRING) {
        error("Strings are immutable — cannot assign to an index");
    } else if (aer_type(obj) == TYPE_TYPED_ARRAY) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        uint64_t i;
        if (!vm_array_position(idx, ta->count, &i))
            return vm_array_index_error(idx, ta->count);
        if (!vm_typed_array_check(vm->chunk, ta->elem_kind, val))
            return;
        unsigned int width = vm_typed_elem_width(ta->elem_kind);
        vm_typed_elem_write(ta->data + (size_t)i * width, ta->elem_kind, val);
    } else {
        error("This value cannot be assigned to by index — an array, a column or a hashtable can");
    }
}

/* integer(x)/float(x)/boolean(x)/string(x) conversion rules, shared by OP_CAST's handler below. */
static AerVal vm_cast(AerVal v, int cast_type) {
    AerVal r = aer_null();
    switch (cast_type) {
        case CAST_INTEGER:
            switch (aer_type(v)) {
                case TYPE_INTEGER: r = v; break;
                case TYPE_REAL: r = aer_int((int64_t)aer_as_real(v)); break;
                case TYPE_BOOLEAN: r = aer_int(aer_as_bool(v) ? 1 : 0); break;
                case TYPE_STRING:
                    error("integer() converts between number types, which cannot fail; parse a "
                          "string with string.to_integer(), which returns (value, err)");
                    r = aer_int(0);
                    break;
                default:
                    error("integer() accepts a number or a boolean");
                    r = aer_int(0);
                    break;
            }
            break;
        case CAST_FLOAT:
            switch (aer_type(v)) {
                case TYPE_REAL: r = v; break;
                case TYPE_INTEGER: r = aer_real((double)aer_as_int(v)); break;
                case TYPE_BOOLEAN: r = aer_real(aer_as_bool(v) ? 1.0 : 0.0); break;
                case TYPE_STRING:
                    error("float() converts between number types, which cannot fail; parse a "
                          "string with string.to_float(), which returns (value, err)");
                    r = aer_real(0.0);
                    break;
                default:
                    error("float() accepts a number or a boolean");
                    r = aer_real(0.0);
                    break;
            }
            break;
        case CAST_BOOLEAN: r = aer_bool(vm_truthy(v)); break;
    }
    return r;
}

/* Dispatch loop -- tail calls                                          */

/* GCC direct-threaded dispatch: each instruction jumps straight to the next handler, so the branch
   predictor learns per-instruction patterns instead of funnelling every opcode through one switch.
   This section cannot move to another file, and that is a language constraint, not inertia:
   `goto *dispatch_table[op]` needs every `h_*` label in the same function, and a label's address
   is only takeable within the function declaring it. Everything above this banner can move freely. */

/* Only ever true in an AER_PROFILE build; elsewhere it keeps Chunk.debug_hits NULL, which is
   what stops the disassembler printing counts nobody collected. */
static bool profiling = false;

void aer_profile_enable(void) {
#ifdef AER_PROFILE
    profiling = true;
#endif
}

bool aer_profile_is_enabled(void) {
    return profiling;
}

/* Grows debug_hits to cover c->code, zero-filling the new region; a no-op once already covered
   (REPL appends code across calls) and while profiling is off, which is what keeps the pointer
   NULL and the dispatch check free. */
static void chunk_ensure_debug_hits(Chunk* c) {
    if (!profiling || c->count <= c->debug_hits_cap)
        return;
    unsigned int old_cap = c->debug_hits_cap;
    c->debug_hits_cap = c->count;
    c->debug_hits = xrealloc(c->debug_hits, sizeof(uint64_t) * c->debug_hits_cap);
    memset(c->debug_hits + old_cap, 0, sizeof(uint64_t) * (c->debug_hits_cap - old_cap));
}

/* Same growth idiom as chunk_ensure_debug_hits, but unconditional -- a perf feature, not a probe. */
static void chunk_ensure_field_cache(Chunk* c) {
    if (c->count <= c->field_cache_cap)
        return;
    unsigned int old_cap = c->field_cache_cap;
    c->field_cache_cap = c->count;
    c->field_cache = xrealloc(c->field_cache, sizeof(FieldCacheEntry) * c->field_cache_cap);
    memset(c->field_cache + old_cap, 0, sizeof(FieldCacheEntry) * (c->field_cache_cap - old_cap));
}

/* Resolves a struct name to its Shape, memoised on the name's pool index -- see Chunk.shape_by_name.
   A miss falls through to the same newest-first scan as before, so a redeclared struct still wins. */
static Shape* chunk_shape_for_pool_idx(Chunk* c, unsigned int pool_idx) {
    if (pool_idx < c->shape_by_name_cap && c->shape_by_name[pool_idx])
        return c->shape_by_name[pool_idx];
    Shape* s = chunk_find_shape(c, aer_as_string(c->pool[pool_idx])->data);
    if (!s)
        return NULL;
    if (pool_idx >= c->shape_by_name_cap) {
        unsigned int old = c->shape_by_name_cap;
        c->shape_by_name_cap = c->pool_count > pool_idx + 1 ? c->pool_count : pool_idx + 1;
        c->shape_by_name = xrealloc(c->shape_by_name, sizeof(Shape*) * c->shape_by_name_cap);
        memset(c->shape_by_name + old, 0, sizeof(Shape*) * (c->shape_by_name_cap - old));
    }
    c->shape_by_name[pool_idx] = s;
    return s;
}

/* Same idiom, for h_call's shape-specialization per-site dispatch cache (see its own comment). */
static void chunk_ensure_call_spec_cache(Chunk* c) {
    if (c->count <= c->call_spec_cache_cap)
        return;
    unsigned int old_cap = c->call_spec_cache_cap;
    c->call_spec_cache_cap = c->count;
    c->call_spec_cache = xrealloc(c->call_spec_cache, sizeof(CallSpecCacheEntry) * c->call_spec_cache_cap);
    memset(c->call_spec_cache + old_cap, 0, sizeof(CallSpecCacheEntry) * (c->call_spec_cache_cap - old_cap));
}

/* h_call's cold path. noinline is required: a static function with one call site is a prime
   candidate for LTO to inline right back in, which grows h_call from 848 bytes to 3KB
   for a path an ordinary call never takes. */
static void __attribute__((noinline))
vm_call_resolve_specialization_full(Chunk* c, ChunkFunction* target_f, AerVal* registers, int arg_reg_base,
                                    unsigned int ip, unsigned int* chosen_offset,
                                    unsigned int* chosen_max_registers, unsigned short* chosen_frame_bounds) {
    unsigned int site =
        ip - 3; /* this instruction's own word0 offset -- ip already advanced past all 3 words by now */
    /* Which argument register keys specialization. Only the first shape-sensitive parameter does;
       the others still work, they just never specialize. */
    int param_index = __builtin_ctz(target_f->shape_sensitive_mask);
    AerVal arg = registers[arg_reg_base + param_index];
    Shape* observed = NULL;
    SpecKind kind = SPEC_KIND_STRUCT;
    if (aer_type(arg) == TYPE_PACKED_ARRAY) {
        observed = aer_as_packed_array(arg)->shape;
        kind = SPEC_KIND_PACKED_ARRAY;
    } else if (aer_type(arg) == TYPE_STRUCT) {
        observed = aer_as_struct(arg)->shape;
        kind = SPEC_KIND_STRUCT;
    } else if (aer_type(arg) == TYPE_ARRAY) {
        /* A plain array carries no structural shape guarantee of its own (AerArray.shape is
           dead, always NULL) -- peek element 0 as a CANDIDATE shape only; the homogeneity scan
           just below is what actually earns the right to trust it, every single call, since
           unlike a packed array or a lone struct instance, a plain array's contents can differ
           from call to call (or even be heterogeneous outright). */
        AerArray* arr = aer_as_array(arg);
        if (arr->count > 0 && aer_type(arr->items[0]) == TYPE_STRUCT) {
            observed = aer_as_struct(arr->items[0])->shape;
            kind = SPEC_KIND_ARRAY_OF_STRUCTS;
        }
    }

    /* SPEC_KIND_ARRAY_OF_STRUCTS has no structural guarantee, so it re-verifies every call: the
       same array can be mutated to hold a different shape between calls. A mismatch falls back to
       the generic body for that call only, leaving the cache untouched.
       The scan is skipped when this site already verified this exact array, by pointer, at its
       current AerArray.generation -- generation only changes on restructuring, so a prior
       verification still holds. That is what makes a repeatedly-scanned array pay O(n) once. */
    if (kind == SPEC_KIND_ARRAY_OF_STRUCTS) {
        AerArray* arr = aer_as_array(arg);
        chunk_ensure_call_spec_cache(c);
        CallSpecCacheEntry* site_entry = &c->call_spec_cache[site];
        bool already_verified = site_entry->last_verified_array == arr &&
                                site_entry->last_verified_generation == arr->generation &&
                                site_entry->last_shape == observed;
        if (!already_verified) {
            for (unsigned int idx = 0; idx < arr->count; idx++) {
                AerVal item = arr->items[idx];
                if (aer_type(item) != TYPE_STRUCT || aer_as_struct(item)->shape != observed) {
                    observed = NULL;
                    break;
                }
            }
            if (observed) {
                site_entry->last_verified_array = arr;
                site_entry->last_verified_generation = arr->generation;
            }
        }
    }

    if (observed) {
        chunk_ensure_call_spec_cache(c);
        CallSpecCacheEntry* site_entry = &c->call_spec_cache[site];
        SpecEntry* entry = NULL;
        if (site_entry->last_shape == observed) {
            *chosen_offset = site_entry->last_code_offset;
            *chosen_max_registers = site_entry->last_max_registers;
            *chosen_frame_bounds = site_entry->last_frame_bounds;
            entry = site_entry->last_entry;
        } else {
            SpecEntry* found = NULL;
            for (int i = 0; i < target_f->specialization_count; i++)
                if (target_f->specializations[i].shape == observed) {
                    found = &target_f->specializations[i];
                    break;
                }
            if (!found && target_f->specialization_count < SPEC_MAX && !target_f->specializations)
                target_f->specializations = xcalloc(SPEC_MAX, sizeof(SpecEntry));
            if (!found && target_f->specialization_count < SPEC_MAX) {
                SpecEntry fresh;
                if (parser_specialize_function(c, target_f, observed, kind, param_index, &fresh, NULL, NULL,
                                               0)) {
                    /* The recompile appended code to this running chunk, so every per-word cache
                       must cover the new size before any site dispatches. debug_hits[] included:
                       DISPATCH() writes it per opcode while profiling, so skipping the resize is a
                       silent out-of-bounds write under --debug-path. */
                    chunk_ensure_debug_hits(c);
                    chunk_ensure_field_cache(c);
                    chunk_ensure_call_spec_cache(c);
                    site_entry =
                        &c->call_spec_cache
                             [site]; /* re-fetch: chunk_ensure_call_spec_cache may have reallocated the array */
                    target_f->specializations[target_f->specialization_count++] = fresh;
                    found = &target_f->specializations[target_f->specialization_count - 1];
                } else {
                    target_f->megamorphic =
                        true; /* treat a recompile failure like exhausting the table -- stop retrying every call */
                }
            } else if (!found) {
                target_f->megamorphic =
                    true; /* table full and still a new shape -- stop specializing this function */
            }
            if (found) {
                site_entry->last_shape = found->shape;
                site_entry->last_code_offset = found->code_offset;
                site_entry->last_max_registers = found->max_registers;
                site_entry->last_frame_bounds = found->frame_bounds;
                site_entry->last_entry = found;
                *chosen_offset = found->code_offset;
                *chosen_max_registers = found->max_registers;
                *chosen_frame_bounds = found->frame_bounds;
                entry = found;
            }
        }

        /* An axis independent of the shape lookup above, re-derived each call rather than cached --
           at most SPEC_MAX_RAW_PARAMS aer_type() reads, and `entry` already exposes the relevant
           raw_variant_* fields. See SpecEntry (vm.h) for why it is a separate variant. */
        /* SPEC_KIND_ARRAY_OF_STRUCTS was once excluded here over an apparent regression that perf
           counters did not reproduce -- cycles, instructions and branch-misses were all lower with
           the variant enabled, and the guarded build's own branch-misses varied 2x between
           identical runs. The guard was removed. */
        if (entry && entry->raw_param_count >= 0) {
            int cand_regs[SPEC_MAX_RAW_PARAMS];
            ValueType cand_types[SPEC_MAX_RAW_PARAMS];
            int cand_count = 0;
            for (unsigned int pi = 0; pi < target_f->arity && cand_count < SPEC_MAX_RAW_PARAMS; pi++) {
                if ((int)pi == param_index)
                    continue;
                AerVal pv = registers[arg_reg_base + pi];
                ValueType pt = aer_type(pv);
                if (pt != TYPE_INTEGER && pt != TYPE_REAL) {
                    cand_count = 0;
                    break;
                } /* not all-numeric -- no raw variant applies this call */
                cand_regs[cand_count] = (int)pi;
                cand_types[cand_count] = pt;
                cand_count++;
            }

            if (cand_count > 0) {
                bool matches_existing = entry->raw_param_count == cand_count;
                if (matches_existing) {
                    for (int k = 0; k < cand_count; k++)
                        if (entry->raw_param_regs[k] != cand_regs[k] ||
                            entry->raw_param_types[k] != cand_types[k]) {
                            matches_existing = false;
                            break;
                        }
                }
                if (matches_existing) {
                    *chosen_offset = entry->raw_variant_code_offset;
                    *chosen_max_registers = entry->raw_variant_max_registers;
                    *chosen_frame_bounds = entry->raw_variant_frame_bounds;
                    *chosen_frame_bounds = entry->raw_variant_frame_bounds;
                } else if (entry->raw_param_count == 0) {
                    /* Never attempted for THIS entry -- try to compile it now. A failure here
                       (raw_ints/raw_reals budget exhausted -- realistic, since this shape's own
                       raw field usage already competes for the same 32-slot budget) sets
                       raw_param_count to -1, a PERMANENT bailout for this one SpecEntry only --
                       never retried every call, but also never affecting `megamorphic` or the
                       shape table, since this axis is fully decoupled from those. */
                    SpecEntry variant;
                    if (parser_specialize_function(c, target_f, observed, kind, param_index, &variant,
                                                   cand_regs, cand_types, cand_count)) {
                        chunk_ensure_debug_hits(c);
                        chunk_ensure_field_cache(c);
                        chunk_ensure_call_spec_cache(c);
                        entry->raw_variant_code_offset = variant.code_offset;
                        entry->raw_variant_max_registers = variant.max_registers;
                        entry->raw_variant_frame_bounds = variant.frame_bounds;
                        entry->raw_variant_frame_bounds = variant.frame_bounds;
                        for (int k = 0; k < cand_count; k++) {
                            entry->raw_param_regs[k] = cand_regs[k];
                            entry->raw_param_types[k] = cand_types[k];
                        }
                        entry->raw_param_count = cand_count;
                        *chosen_offset = entry->raw_variant_code_offset;
                        *chosen_max_registers = entry->raw_variant_max_registers;
                        *chosen_frame_bounds = entry->raw_variant_frame_bounds;
                        *chosen_frame_bounds = entry->raw_variant_frame_bounds;
                    } else {
                        entry->raw_param_count = -1;
                    }
                }
                /* else: entry already has a DIFFERENT raw-numeric variant than this call's
                   observed types -- don't attempt a second one, just use the baseline
                   offset/sizes already chosen above (still fully correct, just boxed dt again
                   for this one call). */
            }
        }
    }
}

/* A numeric-only function has no shape to key on, so the whole decision is whether every argument
   arrived with the types the variant was compiled for -- read straight from the argument registers,
   needing neither the site cache nor the shape table. specializations[0] holds it; shape stays NULL
   because nothing ever looks this entry up by shape. */
static void __attribute__((noinline)) vm_call_resolve_numeric(Chunk* c, ChunkFunction* target_f,
                                                              AerVal* registers, int arg_reg_base,
                                                              unsigned int* chosen_offset,
                                                              unsigned int* chosen_max_registers,
                                                              unsigned short* chosen_frame_bounds) {
    int cand_regs[SPEC_MAX_RAW_PARAMS];
    ValueType cand_types[SPEC_MAX_RAW_PARAMS];
    int cand_count = 0;
    /* Binds whichever parameters arrived numeric and leaves the rest boxed -- a collection
       parameter alongside a scalar one (scale_and_accumulate(nums, factor)) is an ordinary shape,
       and bailing on it left the scalar boxed through the whole body. */
    for (unsigned int pi = 0; pi < target_f->arity && cand_count < SPEC_MAX_RAW_PARAMS; pi++) {
        ValueType pt = aer_type(registers[arg_reg_base + pi]);
        if (pt != TYPE_INTEGER && pt != TYPE_REAL)
            continue;
        cand_regs[cand_count] = (int)pi;
        cand_types[cand_count] = pt;
        cand_count++;
    }
    if (cand_count == 0)
        return;

    if (!target_f->specializations)
        target_f->specializations = xcalloc(SPEC_MAX, sizeof(SpecEntry));
    SpecEntry* entry = &target_f->specializations[0];
    if (entry->raw_param_count < 0)
        return; /* permanently declined for this function */

    if (entry->raw_param_count == cand_count) {
        for (int k = 0; k < cand_count; k++)
            if (entry->raw_param_regs[k] != cand_regs[k] || entry->raw_param_types[k] != cand_types[k])
                return; /* a different numeric signature than the one compiled -- stay boxed */
        *chosen_offset = entry->raw_variant_code_offset;
        *chosen_max_registers = entry->raw_variant_max_registers;
        *chosen_frame_bounds = entry->raw_variant_frame_bounds;
        return;
    }
    if (entry->raw_param_count != 0)
        return;
    /* Warm up first -- see numeric_call_count (vm.h). Counted here rather than in h_call so an
       ordinary call pays nothing for it. */
    if (++target_f->numeric_call_count < NUMERIC_SPECIALIZE_AFTER)
        return;

    SpecEntry variant;
    if (!parser_specialize_function(c, target_f, NULL, SPEC_KIND_STRUCT, -1, &variant, cand_regs, cand_types,
                                    cand_count)) {
        entry->raw_param_count = -1;
        return;
    }
    chunk_ensure_debug_hits(c);
    chunk_ensure_field_cache(c);
    chunk_ensure_call_spec_cache(c);
    entry->raw_variant_code_offset = variant.code_offset;
    entry->raw_variant_max_registers = variant.max_registers;
    entry->raw_variant_frame_bounds = variant.frame_bounds;
    for (int k = 0; k < cand_count; k++) {
        entry->raw_param_regs[k] = cand_regs[k];
        entry->raw_param_types[k] = cand_types[k];
    }
    entry->raw_param_count = cand_count;
    *chosen_offset = entry->raw_variant_code_offset;
    *chosen_max_registers = entry->raw_variant_max_registers;
    *chosen_frame_bounds = entry->raw_variant_frame_bounds;
    *chosen_frame_bounds = entry->raw_variant_frame_bounds;
}

/* The monomorphic case, split off so it does not pay for the full resolver's frame: that one is
   sized for the raw-variant block's candidate arrays and so also carries a stack-protector canary,
   both on every call regardless of which path runs. */
static void __attribute__((noinline))
vm_call_resolve_specialization(Chunk* c, ChunkFunction* target_f, AerVal* registers, int arg_reg_base,
                               unsigned int ip, unsigned int* chosen_offset,
                               unsigned int* chosen_max_registers, unsigned short* chosen_frame_bounds) {
    if (target_f->shape_sensitive_mask == SHAPE_MASK_NUMERIC_ONLY) {
        vm_call_resolve_numeric(c, target_f, registers, arg_reg_base, chosen_offset, chosen_max_registers,
                                chosen_frame_bounds);
        return;
    }
    unsigned int site = ip - 3;
    /* arity 1 with a non-zero mask puts the shape-sensitive parameter at index 0, and leaves the
       raw-variant block inert (it skips that one parameter and there is no other), so a site-cache
       hit is the whole answer. */
    if (target_f->arity == 1 && site < c->call_spec_cache_cap &&
        aer_type(registers[arg_reg_base]) == TYPE_STRUCT &&
        c->call_spec_cache[site].last_shape == aer_as_struct(registers[arg_reg_base])->shape) {
        const CallSpecCacheEntry* hit = &c->call_spec_cache[site];
        *chosen_offset = hit->last_code_offset;
        *chosen_max_registers = hit->last_max_registers;
        *chosen_frame_bounds = hit->last_frame_bounds;
        return;
    }
    vm_call_resolve_specialization_full(c, target_f, registers, arg_reg_base, ip, chosen_offset,
                                        chosen_max_registers, chosen_frame_bounds);
}

/* h_call_module's cold path, split for the same reason as vm_call_resolve_specialization: letting
   LTO inline it grows h_call_module from 336 bytes to 13KB, all of it the ~10 external calls'
   argument shuffling. POP is safe here (no early-return, unlike PUSH), and the
   io-disabled error() longjmps before anything after it runs. Always returns true by the time it
   returns; the compiler just cannot prove it. */
static bool __attribute__((noinline)) vm_call_module_dispatch(VM* vm, Chunk* c, int module_idx, int fn_idx,
                                                              int module_id, int fn_id, int arg_count) {
    bool handled;
    /* module_id/fn_id resolved at parse time -- a switch on two ints instead of a strcmp chain.
       CALL_MODULE_DYNAMIC (host/file module) still resolves by name at runtime. */
    switch (module_id) {
#define AER_MODULE_DISPATCH(id, str, call)                                                                   \
    case CALL_MODULE_##id: handled = call; break;
        AER_NATIVE_MODULES(AER_MODULE_DISPATCH)
#undef AER_MODULE_DISPATCH
        default: {
            const char* module = aer_as_string(c->pool[module_idx])->data;
            const char* fn = aer_as_string(c->pool[fn_idx])->data;
            /* io is the one module still reached through the generic host-call path
               (aer_host_call) rather than a fixed CALL_MODULE_* case -- gated here by name
               specifically so --no-io never touches a real embedding host's own custom modules,
               which go through this exact same path. */
            if (!vm->io_enabled && strcmp(module, "io") == 0) {
                /* POP itself is defined inside vm_run_slice (needs its DISPATCH-loop-local
                   `vm` binding for the overflow/underflow checks the macro shares with PUSH) --
                   this early-drain has no early-return hazard PUSH's own version does, so
                   inlining its one-line body directly here is exactly as safe as calling the
                   macro would be. */
                for (int i = 0; i < arg_count; i++) {
                    if (vm->stack_top > 0)
                        --vm->stack_top;
                    else
                        error("Stack underflow");
                }
                error("io is disabled for this run (--no-io)"); /* never returns -- unwinds instead */
            }
            if (aer_host_is_module(module, (unsigned int)strlen(module)))
                handled = aer_host_call(vm, module, fn, arg_count);
            else
                handled = aer_module_call(vm, module, fn, arg_count);
            break;
        }
    }
    if (!handled)
        error("'%s' has no function '%s'", aer_as_string(c->pool[module_idx])->data,
              aer_as_string(c->pool[fn_idx])->data);
    return handled;
}

/* Measurement-only: shifts every following function's address, so a benchmark can be run across
   several deliberately different code layouts instead of the single one a build happens to produce.
   Interpreter cycle counts swing several percent purely on how vm_run_slice's ~153 dispatch sites
   alias in the branch-target buffer (5.44), which is not attributable to any source change -- and
   comparing one layout against one layout silently folds that in. Never defined by a normal build. */
#ifdef AER_LAYOUT_PAD
#define AER_PAD_STR2(x) #x
#define AER_PAD_STR(x) AER_PAD_STR2(x)
__attribute__((used, noinline)) static void aer_layout_pad_fn(void) {
    __asm__ volatile(".space " AER_PAD_STR(AER_LAYOUT_PAD));
}
#endif

/* max_instructions == 0 means unlimited (every existing caller via the vm_run() wrapper below) --
   the budget decrement only happens at loop-back-edge and call opcodes (the only places a script
   can spend unbounded time), not on every DISPATCH(), so the check costs nothing on the common
   unlimited path and stays cheap even when a budget is active. See aer_scheduler.c for the caller
   that actually uses a nonzero budget. */
/* Opcodes that legitimately have no dispatch label: OP_AND/OP_OR/OP_PIPE are parser tags (&&, ||
   and |> compile to jumps and desugared calls), and the four unary ops only ever appear as
   OP_UNARY's operand tag. Anything else missing an entry is a bug -- see vm_run_slice. */
static bool opcode_is_tag_only(Opcode op) {
    switch (op) {
        case OP_AND:
        case OP_OR:
        case OP_PIPE:
        case OP_NEGATE:
        case OP_NOT:
        case OP_BITWISE_NOT:
        case OP_TO_STR: return true;
        default: return false;
    }
}

/* Every opcode is its own function, tail-calling the next. That makes the handler the unit the
   compiler allocates registers for and the unit perf reports, rather than one body whose live
   ranges span 150 blocks. The four parameters are the only values measured to earn a register;
   everything else is reached through vm or c. */
typedef VmSliceResult (*OpHandler)(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c);
static const OpHandler aer_handlers[256];

/* Deliberately not a hoisted local. Every use is on a boxed path, while keeping it in a
   register costs the raw paths one they need more -- the same trade that made hoisting
   rawk_i/rawk_d expensive. `c` is live regardless, and reading through it also means a
   specialization recompile that moves the pool needs no refresh here. */
#define const_pool (c->pool)

/* pc points past word0 on entry, and op_word is captured before any handler moves pc. */
#define HANDLER(name)                                                                                        \
    static VmSliceResult h_##name(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {                 \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;

/* noinline, or the compiler folds it back into its tail caller along with the registers its calls save. */
#define SEPARATE_HANDLER(name) __attribute__((noinline)) HANDLER(name)

#define READ() (*pc++)
#define PUSH(v)                                                                                              \
    do {                                                                                                     \
        if (vm->stack_top >= VM_STACK_MAX) {                                                                 \
            error("Stack overflow");                                                                         \
            return VM_SLICE_ERROR;                                                                           \
        }                                                                                                    \
        vm->stack[vm->stack_top++] = (v);                                                                    \
    } while (0)
#define POP() (vm->stack_top > 0 ? vm->stack[--vm->stack_top] : (error("Stack underflow"), aer_null()))
/* Neither a GC nor an error poll here: allocating labels call gc_maybe_collect themselves right
   after storing their result into a VM-visible root, and error() longjmps straight to this call's
   catch_point rather than setting a flag every dispatch would have to test. */
/* Full 8-bit mask -- opcode is unambiguously its own byte now (OP_OPCODE_COUNT_MARKER's static
   assert guarantees <=256), no reason to ever mask narrower. */
/* Replicated on purpose: every handler ends with its own indirect branch, so the predictor learns
   per-opcode successor patterns instead of one shared branch guessing among 150 targets.
   musttail is required rather than preferred -- an ordinary call here would grow the stack once
   per instruction executed. */
#define DISPATCH()                                                                                           \
    do {                                                                                                     \
        AER_PROFILE_HIT();                                                                                   \
        uint32_t next_word = *pc++;                                                                          \
        __attribute__((musttail)) return aer_handlers[next_word & 0xFF](vm, pc, registers, c);               \
    } while (0)

#ifdef AER_PROFILE
#define AER_PROFILE_HIT()                                                                                    \
    do {                                                                                                     \
        if (__builtin_expect(c->debug_hits != NULL, 0))                                                      \
            c->debug_hits[pc - c->code]++;                                                                   \
    } while (0)
#else
#define AER_PROFILE_HIT() ((void)0)
#endif

#define SYNC_IP() ((void)(vm->error_pc = pc))
#define error(...) (SYNC_IP(), (error)(__VA_ARGS__))
#define vm_binary_cold(...) (SYNC_IP(), (vm_binary_cold)(__VA_ARGS__))
#define vm_index_get_compute(...) (SYNC_IP(), (vm_index_get_compute)(__VA_ARGS__))
#define vm_index_set_compute(...) (SYNC_IP(), (vm_index_set_compute)(__VA_ARGS__))
#define vm_array_index_error(...) (SYNC_IP(), (vm_array_index_error)(__VA_ARGS__))
#define vm_in(...) (SYNC_IP(), (vm_in)(__VA_ARGS__))
#define vm_cast(...) (SYNC_IP(), (vm_cast)(__VA_ARGS__))
#define vm_to_str(...) (SYNC_IP(), (vm_to_str)(__VA_ARGS__))
#define vm_slice_bounds(...) (SYNC_IP(), (vm_slice_bounds)(__VA_ARGS__))
#define vm_call_builtin(...) (SYNC_IP(), (vm_call_builtin)(__VA_ARGS__))
#define vm_call_value(...) (SYNC_IP(), (vm_call_value)(__VA_ARGS__))
#define vm_call_module_dispatch(...) (SYNC_IP(), (vm_call_module_dispatch)(__VA_ARGS__))
#define vm_call_resolve_specialization(...) (SYNC_IP(), (vm_call_resolve_specialization)(__VA_ARGS__))
#define vm_resolve_field(...) (SYNC_IP(), (vm_resolve_field)(__VA_ARGS__))
#define vm_resolve_field_by_shape(...) (SYNC_IP(), (vm_resolve_field_by_shape)(__VA_ARGS__))
#define vm_check_narrow_field_write(...) (SYNC_IP(), (vm_check_narrow_field_write)(__VA_ARGS__))
#define vm_typed_array_check(...) (SYNC_IP(), (vm_typed_array_check)(__VA_ARGS__))
#define vm_default_value(...) (SYNC_IP(), (vm_default_value)(__VA_ARGS__))
#define aer_make_string_copy(...) (SYNC_IP(), (aer_make_string_copy)(__VA_ARGS__))
/* Only the collection path syncs: the threshold test runs on every allocating opcode. */
#define gc_maybe_collect(v)                                                                                  \
    do {                                                                                                     \
        if (gc_should_collect(v)) {                                                                          \
            SYNC_IP();                                                                                       \
            gc_run_collection_cycle(v);                                                                      \
        }                                                                                                    \
    } while (0)


HANDLER(jump)
    int target = READ();
    pc += (int32_t)target;
    /* Every loop's back-edge (while/for/plain jump alike) goes through here -- the one checkpoint
       that bounds an AER-level loop's slice length. pc already points at a complete instruction
       (this jump's own operand is fully consumed), so yielding here is always resumable. */
    if (vm->slice_max && --vm->slice_budget == 0) {
        vm->ip = (unsigned int)(pc - c->code);
        return VM_SLICE_YIELDED;
    }
    DISPATCH();
}

HANDLER(define_struct)
    int name_idx = (int)UNPACK_STRUCT_HEADER_NAME(op_word);
    int field_count = (int)UNPACK_STRUCT_HEADER_COUNT(op_word);
    Shape* shape = xmalloc(sizeof(Shape));
    shape->name = (unsigned int)name_idx;
    shape->field_count = (unsigned int)field_count;
    for (int i = 0; i < field_count; i++) {
        uint32_t name_default_word = READ();
        shape->field_names[i] = UNPACK_2X16_HI(name_default_word);
        shape->field_defaults[i] = c->pool[UNPACK_2X16_LO(name_default_word)];
        /* Low byte is the ValueType tag, bit 0x100 is the narrow (`i`/`f`-suffixed-literal) marker
           -- see emit_struct_field_type/parse_struct's own comment (parser.c). */
        uint32_t ftype_word = READ();
        shape->field_types[i] = (ValueType)(ftype_word & 0xFF);
        shape->field_narrow[i] = (ftype_word & 0x100) != 0;
    }
    /* Typed fields get 8 raw bytes (no tag -- the type is this Shape's own static knowledge), or 4
       if narrow (int32/float32); TYPE_ANY fields get a full boxed AerVal (16 bytes), since they can
       hold a reference type the GC must trace. See vm_struct_field_read/write. */
    {
        unsigned int offset = 0;
        for (unsigned int i = 0; i < shape->field_count; i++) {
            shape->field_offsets[i] = offset;
            offset += (shape->field_types[i] == TYPE_ANY) ? sizeof(AerVal) : (shape->field_narrow[i] ? 4 : 8);
        }
        shape->instance_bytes = offset;
    }
    if (c->shape_count >= c->shape_cap) {
        c->shape_cap = c->shape_cap ? c->shape_cap * 2 : 4;
        c->shapes = xrealloc(c->shapes, sizeof(Shape*) * c->shape_cap);
    }
    c->shapes[c->shape_count++] = shape;
    /* A redeclare must not keep resolving to the Shape it shadows -- see Chunk.shape_by_name. */
    if (c->shape_by_name)
        memset(c->shape_by_name, 0, sizeof(Shape*) * c->shape_by_name_cap);
    DISPATCH();
}

/* dest+pool_idx both fit in word0 now (op(8)+dest(8)+pool_idx(16)) -- no trailing word needed,
   down from the original design's 2-word form. */
HANDLER(loadk)
    int dest = (int)UNPACK_A(op_word);
    unsigned int pool_idx = UNPACK_W16(op_word);
    registers[dest] = c->pool[pool_idx];
    DISPATCH();
}

HANDLER(move)
    int dest = (int)UNPACK_A(op_word);
    int src = (int)UNPACK_B(op_word);
    registers[dest] = registers[src];
    DISPATCH();
}

HANDLER(is_result)
    int dest = (int)UNPACK_A(op_word);
    int src = (int)UNPACK_B(op_word);
    registers[dest] = aer_bool(aer_type(registers[src]) == TYPE_RESULT);
    DISPATCH();
}

/* Dest + both RK operands already in op_word -- no further READ(). Each operator is its own
   top-level dispatch target, checks its own fast path, and falls back to vm_binary_cold() for
   anything else. BINARY_OP_INT_REAL has both fast paths; BINARY_OP_INT_ONLY (bitwise/shift) has
   no real/real meaning. */
/* `result` points straight at the destination register -- building a local AerVal and copying
   it measured as a 16-byte stack round-trip that was the single hottest instruction in the
   nbody profile. Safe even when dest aliases ra/rb (l/rv already snapshotted by value). */
/* Only the _any_values handlers collect: an int/int or real/real result never allocates. */
#define BINARY_OP_ANY_VALUES(NAME, OPENUM)                                                                   \
    SEPARATE_HANDLER(NAME##_any_values)                                                                      \
        AerVal* ra = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));                                   \
        AerVal* rb = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));                                   \
        registers[UNPACK_A(op_word)] = vm_binary_cold(c, *ra, *rb, OPENUM, ra->tag, rb->tag);                \
        gc_maybe_collect(vm);                                                                                \
        DISPATCH();                                                                                          \
    }
#define BINARY_OP_INT_REAL(NAME, OPENUM, INT_STMT, REAL_STMT)                                                \
    BINARY_OP_ANY_VALUES(NAME, OPENUM)                                                                       \
    static VmSliceResult h_##NAME(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {                 \
        const uint32_t op_word = pc[-1];                                                                     \
        int dest = (int)UNPACK_A(op_word);                                                                   \
        AerVal* ra = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));                                   \
        AerVal* rb = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));                                   \
        ValueType ta = ra->tag, tb = rb->tag;                                                                \
        AerVal* result = &registers[dest];                                                                   \
        if (ta == TYPE_INTEGER && tb == TYPE_INTEGER) {                                                      \
            int64_t l = ra->as.i, rv = rb->as.i;                                                             \
            INT_STMT                                                                                         \
        } else if (ta == TYPE_REAL && tb == TYPE_REAL) {                                                     \
            double l = ra->as.d, rv = rb->as.d;                                                              \
            REAL_STMT                                                                                        \
        } else {                                                                                             \
            __attribute__((musttail)) return h_##NAME##_any_values(vm, pc, registers, c);                    \
        }                                                                                                    \
        DISPATCH();                                                                                          \
    }
#define BINARY_OP_INT_ONLY(NAME, OPENUM, INT_STMT)                                                           \
    BINARY_OP_ANY_VALUES(NAME, OPENUM)                                                                       \
    static VmSliceResult h_##NAME(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {                 \
        const uint32_t op_word = pc[-1];                                                                     \
        int dest = (int)UNPACK_A(op_word);                                                                   \
        AerVal* ra = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));                                   \
        AerVal* rb = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));                                   \
        AerVal* result = &registers[dest];                                                                   \
        if (ra->tag == TYPE_INTEGER && rb->tag == TYPE_INTEGER) {                                            \
            int64_t l = ra->as.i, rv = rb->as.i;                                                             \
            INT_STMT                                                                                         \
        } else {                                                                                             \
            __attribute__((musttail)) return h_##NAME##_any_values(vm, pc, registers, c);                    \
        }                                                                                                    \
        DISPATCH();                                                                                          \
    }

    BINARY_OP_INT_REAL(add, OP_ADD, { *result = aer_int(l + rv); }, { *result = aer_real(l + rv); })
    BINARY_OP_INT_REAL(sub, OP_SUB, { *result = aer_int(l - rv); }, { *result = aer_real(l - rv); })
    BINARY_OP_INT_REAL(mul, OP_MUL, { *result = aer_int(l * rv); }, { *result = aer_real(l * rv); })
    BINARY_OP_INT_REAL(
        div, OP_DIV,
        {
            if (rv == 0) {
                error("Division by zero");
                *result = aer_int(0);
            } else {
                *result = aer_real((double)l / (double)rv);
            }
        },
        {
            if (rv == 0.0) {
                error("Division by zero");
                *result = aer_real(0.0);
            } else {
                *result = aer_real(l / rv);
            }
        })
    BINARY_OP_INT_REAL(
        floor_div, OP_FLOOR_DIV,
        {
            if (rv == 0) {
                error("Division by zero");
                *result = aer_int(0);
            } else {
                *result = aer_int((int64_t)floor((double)l / (double)rv));
            }
        },
        {
            if (rv == 0.0) {
                error("Division by zero");
                *result = aer_real(0.0);
            } else {
                *result = aer_real(floor(l / rv));
            }
        })
    BINARY_OP_INT_REAL(
        mod, OP_MOD,
        {
            if (rv == 0) {
                error("Modulo by zero");
                *result = aer_int(0);
            } else {
                *result = aer_int(aer_mod_int64(l, rv));
            }
        },
        {
            if (rv == 0.0) {
                error("Modulo by zero");
                *result = aer_real(0.0);
            } else {
                *result = aer_real(aer_mod_double(l, rv));
            }
        })
    BINARY_OP_INT_REAL(eq, OP_EQ, { *result = aer_bool(l == rv); }, { *result = aer_bool(l == rv); })
    BINARY_OP_INT_REAL(neq, OP_NEQ, { *result = aer_bool(l != rv); }, { *result = aer_bool(l != rv); })
    BINARY_OP_INT_REAL(lt, OP_LT, { *result = aer_bool(l < rv); }, { *result = aer_bool(l < rv); })
    BINARY_OP_INT_REAL(gt, OP_GT, { *result = aer_bool(l > rv); }, { *result = aer_bool(l > rv); })
    BINARY_OP_INT_REAL(lte, OP_LTE, { *result = aer_bool(l <= rv); }, { *result = aer_bool(l <= rv); })
    BINARY_OP_INT_REAL(gte, OP_GTE, { *result = aer_bool(l >= rv); }, { *result = aer_bool(l >= rv); })
    BINARY_OP_INT_ONLY(bitwise_and, OP_BITWISE_AND, { *result = aer_int(l & rv); })
    BINARY_OP_INT_ONLY(bitwise_or, OP_BITWISE_OR, { *result = aer_int(l | rv); })
    BINARY_OP_INT_ONLY(bitwise_xor, OP_BITWISE_XOR, { *result = aer_int(l ^ rv); })
    BINARY_OP_INT_ONLY(lshift, OP_LSHIFT, { *result = aer_int(l << rv); })
    BINARY_OP_INT_ONLY(rshift, OP_RSHIFT, { *result = aer_int(l >> rv); })

#undef BINARY_OP_INT_REAL
#undef BINARY_OP_INT_ONLY
#undef BINARY_OP_ANY_VALUES

/* Fused comparison-and-branch (vm.h's own comment on OP_LT_JUMP_IF_FALSE et al. has the full
   rationale) -- no destination register at all, the truth value is consumed immediately by the
   branch decision below instead of being written out and read back. The cold (non-int/non-real)
   path skips gc_maybe_collect() unlike BINARY_OP_INT_REAL's shared cold path above -- a boolean
   result never allocates, on any operand type, so there is nothing here for a collection to ever
   need to run for. */
#define CMP_JUMP_IF_FALSE(NAME, OPENUM, INT_CMP, REAL_CMP)                                                   \
    SEPARATE_HANDLER(NAME##_jump_if_false_any_values)                                                        \
        AerVal* ra = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));                                   \
        AerVal* rb = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));                                   \
        int target = READ();                                                                                 \
        if (!vm_truthy(vm_binary_cold(c, *ra, *rb, OPENUM, ra->tag, rb->tag)))                               \
            pc += (int32_t)target;                                                                           \
        DISPATCH();                                                                                          \
    }                                                                                                        \
    static VmSliceResult h_##NAME##_jump_if_false(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) { \
        const uint32_t op_word = pc[-1];                                                                     \
        AerVal* ra = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));                                   \
        AerVal* rb = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));                                   \
        ValueType ta = ra->tag, tb = rb->tag;                                                                \
        bool cond;                                                                                           \
        if (ta == TYPE_INTEGER && tb == TYPE_INTEGER) {                                                      \
            int64_t l = ra->as.i, rv = rb->as.i;                                                             \
            cond = (INT_CMP);                                                                                \
        } else if (ta == TYPE_REAL && tb == TYPE_REAL) {                                                     \
            double l = ra->as.d, rv = rb->as.d;                                                              \
            cond = (REAL_CMP);                                                                               \
        } else {                                                                                             \
            __attribute__((musttail)) return h_##NAME##_jump_if_false_any_values(vm, pc, registers, c);      \
        }                                                                                                    \
        int target = READ();                                                                                 \
        if (!cond)                                                                                           \
            pc += (int32_t)target;                                                                           \
        DISPATCH();                                                                                          \
    }

    CMP_JUMP_IF_FALSE(eq, OP_EQ, l == rv, l == rv)
    CMP_JUMP_IF_FALSE(neq, OP_NEQ, l != rv, l != rv)
    CMP_JUMP_IF_FALSE(lt, OP_LT, l < rv, l < rv)
    CMP_JUMP_IF_FALSE(gt, OP_GT, l > rv, l > rv)
    CMP_JUMP_IF_FALSE(lte, OP_LTE, l <= rv, l <= rv)
    CMP_JUMP_IF_FALSE(gte, OP_GTE, l >= rv, l >= rv)
#undef CMP_JUMP_IF_FALSE

/* No int/int or real/real fast path -- dispatches straight to the shared vm_in(). */
HANDLER(in)
    int dest = (int)UNPACK_A(op_word);
    AerVal a = *vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    AerVal b = *vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    registers[dest] = vm_in(a, b);
    DISPATCH();
}

/* Control flow -- stack-neutral, reads only registers[]/the pool. */
HANDLER(jump_if_false_reg)
    int reg = (int)UNPACK_A(op_word);
    int target = READ();
    if (!vm_truthy(registers[reg]))
        pc += (int32_t)target;
    DISPATCH();
}

/* Bulk-copies args from the caller's bank into the new frame's bank at register 0; overflow
   check mirrors vm_setup_call's own ceiling. */
HANDLER(call)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int callee_offset = READ();
    /* The target's BYTE offset into c->functions[] -- always present (emit_call always emits it,
       patched in later by func_register for a forward reference), letting this size the callee's
       frame from its real max_registers peak instead of a flat,
       function-agnostic ceiling. A byte offset rather than an index because ChunkFunction is not a
       power of two, so indexing cost a multiply on every call. */
    unsigned int func_byte_offset = (unsigned int)READ();
    ChunkFunction* target_f = (ChunkFunction*)((char*)c->functions + func_byte_offset);
    /* `registers` is the hoisted copy of this same frame's base -- every site that changes frames
       reassigns it -- so reading it back out of the frame is a redundant dependent load on the
       hottest path in the interpreter. Resolution below never touches the bank, so the base is
       final here. */
    AerVal* callee_regs = registers + vm->call_stack[vm->call_depth].frame_size;
    if (vm->call_depth + 1 >= vm->call_depth_limit || callee_regs > vm->push_base_limit) {
        if (!vm_grow_for_push(vm, callee_regs)) {
            error("Call stack overflow (max %d frames); tail calls do not consume one", VM_CALL_MAX);
            DISPATCH();
        }
        /* Nothing is written yet, so the call reruns from its first word against the grown bank.
           Rebasing locals here instead keeps them live through the handler, and every call pays
           for that in spilled registers. */
        __attribute__((musttail)) return h_call(vm, pc - 2, vm->call_stack[vm->call_depth].registers, c);
    }

    /* Gated on shape_sensitive_mask (zero for most functions, one already-fetched field) rather than
       on a distinct call opcode: shape-sensitivity is only known once the whole body has compiled,
       but forward-referenced and self-recursive calls need their opcode chosen before that. */
    unsigned int chosen_offset = (unsigned int)callee_offset;
    unsigned int chosen_max_registers = target_f->max_registers;
    unsigned short chosen_frame_bounds = target_f->frame_bounds;

    if (!target_f->megamorphic && target_f->shape_sensitive_mask != 0) {
        /* The out-params are scoped to this branch on purpose. Taking the address of the chosen_*
           locals themselves forces both into memory for the WHOLE handler -- an address that escapes
           cannot live in a register -- so every ordinary call paid stores and address computations
           for a path it never takes. */
        unsigned int spec_offset = chosen_offset, spec_registers = chosen_max_registers;
        unsigned short spec_bounds = chosen_frame_bounds;
        unsigned int resume_at = (unsigned int)(pc - c->code);
        vm_call_resolve_specialization(c, target_f, registers, arg_reg_base, resume_at, &spec_offset,
                                       &spec_registers, &spec_bounds);
        chosen_offset = spec_offset;
        chosen_max_registers = spec_registers;
        chosen_frame_bounds = spec_bounds;
        /* Compiling a specialized body can realloc any of the chunk's growable arrays, so pc is
           rebuilt from the chunk rather than kept across the call. Everything else the loop reads
           already goes through c, so a move costs nothing to follow. */
        pc = c->code + resume_at;
    }

    CallFrame* caller = &vm->call_stack[vm->call_depth];
    CallFrame* callee = caller + 1;
    /* Every scalar field first, so pc, c, dest_reg and chosen_offset stop being live across the
       two loops below. The compiler cannot sink them itself: call_stack and the register bank are
       separate allocations, but nothing in the types says so. */
    callee->registers = callee_regs;
    callee->frame_size = chosen_max_registers;
    callee->frame_bounds = chosen_frame_bounds;
    callee->return_ip =
        (unsigned int)(pc - c->code); /* already past this instruction's operands -- the correct resume point */
    callee->dest_reg = dest_reg;
    callee->code_offset = chosen_offset;
    callee->tail_calls_collapsed = 0;
    callee->synthetic_entry = false;
    for (int i = 0; i < arg_count; i++)
        callee_regs[i] = registers[arg_reg_base + i];
    /* mark_vm_roots traces every register below frame_size, so an unfilled one would still hold a
       popped frame's pointer. Only the tag matters -- value_has_cell reads nothing else. */
    frame_init_tags(callee_regs, (unsigned int)arg_count, chosen_frame_bounds);
    vm->call_depth++;
    registers = callee_regs;
    pc = c->code + chosen_offset;
    if (vm->slice_max && --vm->slice_budget == 0) {
        vm->ip = (unsigned int)(pc - c->code);
        return VM_SLICE_YIELDED;
    }
    DISPATCH();
}

/* Its own label, so an ordinary call carries none of this in its live range. Reuses the current
   frame rather than pushing, so none of h_call's sizing work applies. */
HANDLER(tail_call)
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int callee_offset = READ();
    /* Same byte-offset operand as h_call's -- emit_call produces both. */
    ChunkFunction* target_f = (ChunkFunction*)((char*)c->functions + (unsigned int)READ());
    /* The copy iterates with dest_i always <= source_i, the same always-safe-forward-shift pattern
       memmove uses when dest <= src, so no overlap special-casing is needed. */
    for (int i = 0; i < arg_count; i++)
        registers[i] = registers[arg_reg_base + i];
    pc = c->code + callee_offset;
    CallFrame* reused = &vm->call_stack[vm->call_depth];
    reused->code_offset = (unsigned int)callee_offset; /* reused frame now runs a different function */
    /* The reused frame's sizing belongs to whatever function last occupied it, and a tail call can
       land on one with a different peak. Left stale, a later real call from inside this frame
       computes its child's base from the wrong frame_size and overlaps still-live slots. The base
       pointers are untouched -- same frame, same memory, only the claim changes. */
    reused->frame_size = target_f->max_registers;
    /* Same as the non-tail push: the block's tags belong to the function now running here. */
    frame_init_tags(registers, (unsigned int)arg_count, target_f->frame_bounds);
    reused->frame_bounds = target_f->frame_bounds;
    reused->tail_calls_collapsed++;
    /* Every call (tail or not) is the other place a script can spend unbounded time (recursion
       instead of a loop) -- checked once pc already points at the callee's real entry point, so a
       yield here always resumes at a valid instruction boundary. */
    if (vm->slice_max && --vm->slice_budget == 0) {
        vm->ip = (unsigned int)(pc - c->code);
        return VM_SLICE_YIELDED;
    }
    DISPATCH();
}

/* Same frame-push shape as h_call, but the target is a runtime AerFunction read from a register.
   3 registers fit word0 (PACK3); callee_reg is the 4th and gets its own trailing word (no room
   left in a 32-bit word0 for a 4th 8-bit field alongside the opcode). */
HANDLER(call_value)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int callee_reg = (int)READ();
    /* vm_call_value writes vm->ip on success (or leaves it untouched on error) -- reload before
       the next READ(). pc is already past callee_reg's word, the correct resume address. */
    vm_call_value(vm, registers[callee_reg], dest_reg, arg_reg_base, arg_count, (op_word & 0xFF) == OP_TAIL_CALL_VALUE,
                  (unsigned int)(pc - c->code));
    pc = c->code + vm->ip;
    /* vm_call_value may have pushed a frame (non-tail) or reused this one (tail) -- refresh from
       whichever is now current, either way. */
    registers = vm->call_stack[vm->call_depth].registers;
    DISPATCH();
}

/* return_ip/dest_reg live in the callee's own frame, not a shared global -- what makes
   nested/recursive calls safe. */
HANDLER(return)
    int src_reg = (int)UNPACK_A(op_word);
    CallFrame* callee = &vm->call_stack[vm->call_depth];

    AerVal result = callee->registers[src_reg];
    unsigned int return_ip = callee->return_ip;
    int dest_reg = callee->dest_reg;
    vm->call_depth--;
    /* Refreshed BEFORE the write below -- registers still pointed at the callee's (now-popped)
       frame otherwise, corrupting whichever register of the CALLER's frame happens to share
       dest_reg's index instead of writing the return value where the caller expects it. */
    CallFrame* caller = &vm->call_stack[vm->call_depth];
    registers = caller->registers;
    registers[dest_reg] = result;
    pc = c->code + return_ip;
    DISPATCH();
}

/* See OP_CALL_SELF (vm.h). The frame is the caller's own size and entry point, both read from the
   caller frame rather than resolved, so this is a push and nothing else. */
HANDLER(call_self)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    CallFrame* caller = &vm->call_stack[vm->call_depth];
    CallFrame* callee = caller + 1;
    unsigned int fsz = caller->frame_size, entry = caller->code_offset;
    unsigned short bounds = caller->frame_bounds;
    AerVal* callee_regs = registers + fsz;
    if (vm->call_depth + 1 >= vm->call_depth_limit || callee_regs > vm->push_base_limit) {
        if (!vm_grow_for_push(vm, callee_regs)) {
            error("Call stack overflow (max %d frames); tail calls do not consume one", VM_CALL_MAX);
            DISPATCH();
        }
        /* Same rerun as h_call's, and for the same reason. */
        __attribute__((musttail)) return h_call_self(vm, pc, vm->call_stack[vm->call_depth].registers, c);
    }
    /* Every scalar field first, so pc, c, dest_reg and entry stop being live across the two loops
       below. The compiler cannot sink them itself: call_stack and the register bank are separate
       allocations, but nothing in the types says so. */
    callee->registers = callee_regs;
    callee->frame_size = fsz;
    callee->frame_bounds = bounds;
    callee->return_ip = (unsigned int)(pc - c->code);
    callee->dest_reg = dest_reg;
    callee->code_offset = entry;
    callee->tail_calls_collapsed = 0;
    callee->synthetic_entry = false;
    for (int i = 0; i < arg_count; i++)
        callee_regs[i] = registers[arg_reg_base + i];
    /* mark_vm_roots traces every slot below frame_size, so the ones this call does not fill must not
       keep a popped frame's stale references. Self-call, so the callee's block layout is the
       caller's. */
    frame_init_tags(callee_regs, (unsigned int)arg_count, bounds);
    vm->call_depth++;
    registers = callee_regs;
    pc = c->code + entry;
    DISPATCH();
}

/* OP_CALL_SELF in tail position. The frame being reused is already this variant's own, so its
   size, bounds and entry stay as they are -- OP_TAIL_CALL would install the generic function's. */
HANDLER(tail_call_self)
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    CallFrame* frame = &vm->call_stack[vm->call_depth];
    for (int i = 0; i < arg_count; i++)
        registers[i] = registers[arg_reg_base + i];
    frame_init_tags(registers, (unsigned int)arg_count, frame->frame_bounds);
    frame->tail_calls_collapsed++;
    pc = c->code + frame->code_offset;
    if (vm->slice_max && --vm->slice_budget == 0) {
        vm->ip = (unsigned int)(pc - c->code);
        return VM_SLICE_YIELDED;
    }
    DISPATCH();
}

/* On a domain error aer_math_unary_raw has already raised it and longjmped, so the store is only
   reached with a real result. */
HANDLER(raw_math_real)
    int dest = (int)UNPACK_A(op_word);
    double x = registers[UNPACK_B(op_word)].as.d;
    /* sqrt is one machine instruction and the overwhelming majority of the traffic. A real call in
       the dispatch loop costs far more than the call itself -- every hoisted pointer becomes
       call-clobbered and the register allocator pessimizes accordingly, which on nbody was worth
       10% of cycles against a 13% instruction saving. The rest are rare enough to keep the call. */
    if (UNPACK_C(op_word) == FN_MATH_SQRT && x >= 0) {
        registers[dest] = aer_real(sqrt(x));
        DISPATCH();
    }
    double out;
    SYNC_IP();
    if (aer_math_unary_raw((int)UNPACK_C(op_word), x, &out))
        registers[dest] = aer_real(out);
    DISPATCH();
}

#undef CALL_RAW

/* Bridges to the same stack-based stdlib dispatch h_call_module uses. */
HANDLER(call_module)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int module_idx = (int)READ();
    int fn_idx = (int)READ();
    uint32_t ids_word = READ();
    int module_id = (int)UNPACK_2X16_HI(ids_word);
    int fn_id = (int16_t)UNPACK_2X16_LO(ids_word); /* sign-extend -- FN_ID_UNKNOWN is -1 */
    for (int i = 0; i < arg_count; i++)
        PUSH(registers[arg_reg_base + i]);
    /* vm_call_module_dispatch (below) never actually returns with the call unresolved -- it calls
       error() itself in that case, which unwinds before coming back here -- so by the time control
       reaches this line, the module function's result is already sitting on top of the value stack. */
    vm_call_module_dispatch(vm, c, module_idx, fn_idx, module_id, fn_id, arg_count);
    registers[dest_reg] = POP();
    gc_maybe_collect(vm); /* stdlib/module functions routinely allocate (new strings/arrays/etc.) */
    DISPATCH();
}

/* vm_call_builtin takes a plain AerVal* array -- no push/pop bridge needed. */
HANDLER(call_builtin)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int name_idx = (int)READ();
    int builtin_id = (int)READ();
    /* name is only resolved on the error paths -- the happy path never needs it. */
    if (arg_count > 4) {
        error("Too many arguments to '%s'", aer_as_string(c->pool[name_idx])->data);
        registers[dest_reg] = aer_null();
        DISPATCH();
    }
    AerVal args[4];
    for (int i = 0; i < arg_count; i++)
        args[i] = registers[arg_reg_base + i];
    bool handled = vm_call_builtin(c, builtin_id, args, arg_count, &registers[dest_reg]);
    if (!handled)
        error("'%s' is not defined, or was called with the wrong number of arguments",
              aer_as_string(c->pool[name_idx])->data);
    gc_maybe_collect(vm); /* vm_call_builtin: a struct_pools tier site + aer_make_string (type()) */
    DISPATCH();
}

/* Builds an array from an in-order register range -- mark_vm_roots's registers scan keeps the
   result alive across GC. */
HANDLER(array_new)
    int dest_reg = (int)UNPACK_A(op_word);
    int item_reg_base = (int)UNPACK_B(op_word);
    int item_count = (int)UNPACK_C(op_word);
    AerArray* a = heap_new_array(&vm->heap, item_count > 0 ? (unsigned int)item_count : 4);
    a->count = (unsigned int)item_count;
    for (int i = 0; i < item_count; i++)
        a->items[i] = registers[item_reg_base + i];
    registers[dest_reg] = aer_array_val(a);
    gc_maybe_collect(vm); /* pool_alloc(&array_pool) above; result already rooted */
    DISPATCH();
}

/* Already type-generic (array/dict/string) with all bounds/negative-index logic. */
HANDLER(index_get)
    int dest_reg = (int)UNPACK_A(op_word);
    int arr_reg = (int)UNPACK_B(op_word);
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal obj = registers[arr_reg];
    /* A negative index wraps to a huge unsigned value and falls through to vm_array_position, which
       counts it from the end -- so one compare covers both bounds and this skips nothing a plain
       in-range read needs. */
    if (aer_type(obj) == TYPE_ARRAY && aer_type(*idx) == TYPE_INTEGER) {
        AerArray* a = aer_as_array(obj);
        uint64_t at = (uint64_t)aer_as_int(*idx);
        if (!a->shape && at < (uint64_t)a->count) {
            registers[dest_reg] = a->items[at];
            DISPATCH();
        }
    }
    vm_index_get_compute(obj, *idx, &registers[dest_reg]);
    /* Only single-char string indexing allocates -- array/dict indexing never touches the heap, so
       skip the check for the dominant common case. */
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm);
    DISPATCH();
}

/* a, b = expr -- see vm_destructure_compute. Never allocates, unlike h_index_get, so no
   gc_maybe_collect needed. */
HANDLER(destructure)
    int t0 = (int)UNPACK_A(op_word);
    int t1 = (int)UNPACK_B(op_word);
    int src_reg = (int)UNPACK_C(op_word);
    vm_destructure_compute(registers[src_reg], &registers[t0], &registers[t1]);
    DISPATCH();
}

/* Includes the internal write barrier, now exercised against a register-held reference. */
HANDLER(index_set)
    int arr_reg = (int)UNPACK_A(op_word);
    AerVal idx = *vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    AerVal val = *vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    vm_index_set_compute(vm, registers[arr_reg], idx, val);
    DISPATCH();
}

/* Loop-bound-hoisting counterparts of h_index_get/set, same word layout as OP_INDEX_GET/SET.
   This opcode carries only an index-safety proof, never a container-identity one, so the
   TYPE_TYPED_ARRAY check below is not a defensive net -- it is the only thing deciding whether the
   fast path applies. A miss must fall through to the fully generic behavior for any other
   container, not error: the parser's loop-safety proof knows nothing about container type. */
HANDLER(typed_index_get_unchecked)
    int dest_reg = (int)UNPACK_A(op_word);
    int arr_reg = (int)UNPACK_B(op_word);
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal obj = registers[arr_reg];
    if (aer_type(obj) == TYPE_ARRAY) {
        /* A plain array can shrink inside the loop, so the proof covers only a typed one's index. */
        AerArray* a = aer_as_array(obj);
        if (a->shape || idx->tag != TYPE_INTEGER || (uint64_t)idx->as.i >= a->count)
            __attribute__((musttail)) return h_index_get(vm, pc, registers, c);
        registers[dest_reg] = a->items[idx->as.i];
        DISPATCH();
    }
    if (aer_type(obj) != TYPE_TYPED_ARRAY)
        __attribute__((musttail)) return h_index_get(vm, pc, registers, c);
    AerTypedArray* ta = aer_as_typed_array(obj);
    int64_t i = aer_as_int(*idx);
    unsigned int width = vm_typed_elem_width(ta->elem_kind);
    registers[dest_reg] = vm_typed_elem_read(ta->data + (size_t)i * width, ta->elem_kind);
    DISPATCH();
}

SEPARATE_HANDLER(typed_index_set_reject_value)
    AerTypedArray* ta = aer_as_typed_array(registers[(int)UNPACK_A(op_word)]);
    vm_typed_array_check(c, ta->elem_kind, *vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word)));
    DISPATCH();
}

HANDLER(typed_index_set_unchecked)
    int arr_reg = (int)UNPACK_A(op_word);
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    AerVal* val = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal obj = registers[arr_reg];
    if (aer_type(obj) != TYPE_TYPED_ARRAY)
        __attribute__((musttail)) return h_index_set(vm, pc, registers, c);
    AerTypedArray* ta = aer_as_typed_array(obj);
    /* The loop proves the index, not the value. */
    if (!vm_typed_array_accepts(ta->elem_kind, *val))
        __attribute__((musttail)) return h_typed_index_set_reject_value(vm, pc, registers, c);
    int64_t i = aer_as_int(*idx);
    unsigned int width = vm_typed_elem_width(ta->elem_kind);
    vm_typed_elem_write(ta->data + (size_t)i * width, ta->elem_kind, *val);
    DISPATCH();
}

SEPARATE_HANDLER(index_set_any_from_int)
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    vm_index_set_compute(vm, registers[(int)UNPACK_A(op_word)], *idx,
                         aer_int(registers[UNPACK_C(op_word)].as.i));
    DISPATCH();
}

SEPARATE_HANDLER(index_set_any_from_real)
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    vm_index_set_compute(vm, registers[(int)UNPACK_A(op_word)], *idx,
                         aer_real(registers[UNPACK_C(op_word)].as.d));
    DISPATCH();
}

SEPARATE_HANDLER(index_get_any_as_int)
    int dest = (int)UNPACK_A(op_word);
    AerVal obj = registers[(int)UNPACK_B(op_word)];
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal v;
    vm_index_get_compute(obj, *idx, &v);
    if (v.tag == TYPE_INTEGER)
        registers[dest] = aer_int(v.as.i);
    else
        error("Expected an integer from this index, got %s", vm_type_name(c, v));
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm);
    DISPATCH();
}

SEPARATE_HANDLER(index_get_any_as_real)
    int dest = (int)UNPACK_A(op_word);
    AerVal obj = registers[(int)UNPACK_B(op_word)];
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal v;
    vm_index_get_compute(obj, *idx, &v);
    if (v.tag == TYPE_REAL)
        registers[dest] = aer_real(v.as.d);
    else if (v.tag == TYPE_INTEGER)
        registers[dest] = aer_real((double)v.as.i);
    else
        error("Expected a number from this index, got %s", vm_type_name(c, v));
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm);
    DISPATCH();
}

/* The non-typed-array receiver goes through the ordinary boxed compute and then unboxes, so these
   stay correct on any value -- a plain array of reals, a string index, a dict -- and only the
   typed-array case skips the AerVal entirely. A non-numeric result is the same error the raw
   arithmetic that consumes this slot would have raised one opcode later. */
HANDLER(index_set_raw_int)
    AerVal obj = registers[(int)UNPACK_A(op_word)];
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    int64_t v = registers[UNPACK_C(op_word)].as.i;
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count) {
            if (ta->elem_kind == TYPED_ELEM_INT64) {
                memcpy(ta->data + (size_t)idx->as.i * 8, &v, 8);
                DISPATCH();
            }
            if (ta->elem_kind == TYPED_ELEM_INT32 && v >= INT32_MIN && v <= INT32_MAX) {
                int32_t narrow = (int32_t)v;
                memcpy(ta->data + (size_t)idx->as.i * 4, &narrow, 4);
                DISPATCH();
            }
        }
    }
    __attribute__((musttail)) return h_index_set_any_from_int(vm, pc, registers, c);
}

HANDLER(index_set_raw_real)
    AerVal obj = registers[(int)UNPACK_A(op_word)];
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_B(op_word));
    double v = registers[UNPACK_C(op_word)].as.d;
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count) {
            if (ta->elem_kind == TYPED_ELEM_FLOAT64) {
                memcpy(ta->data + (size_t)idx->as.i * 8, &v, 8);
                DISPATCH();
            }
            if (ta->elem_kind == TYPED_ELEM_FLOAT32) {
                float narrow = (float)v;
                memcpy(ta->data + (size_t)idx->as.i * 4, &narrow, 4);
                DISPATCH();
            }
        }
    }
    __attribute__((musttail)) return h_index_set_any_from_real(vm, pc, registers, c);
}

HANDLER(index_get_raw_int)
    int dest = (int)UNPACK_A(op_word);
    AerVal obj = registers[(int)UNPACK_B(op_word)];
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count) {
            /* Only two kinds can reach the read, so decode it here rather than through the shared
               helpers, which each re-decide among five on an element read. */
            if (ta->elem_kind == TYPED_ELEM_INT64) {
                int64_t v;
                memcpy(&v, ta->data + (size_t)idx->as.i * 8, 8);
                registers[dest] = aer_int(v);
                DISPATCH();
            }
            if (ta->elem_kind == TYPED_ELEM_INT32) {
                int32_t v;
                memcpy(&v, ta->data + (size_t)idx->as.i * 4, 4);
                registers[dest] = aer_int(v);
                DISPATCH();
            }
        }
    }
    __attribute__((musttail)) return h_index_get_any_as_int(vm, pc, registers, c);
}

HANDLER(index_get_raw_real)
    int dest = (int)UNPACK_A(op_word);
    AerVal obj = registers[(int)UNPACK_B(op_word)];
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count) {
            /* Same two-kind decode as index_get_raw_int above. */
            if (ta->elem_kind == TYPED_ELEM_FLOAT64) {
                double v;
                memcpy(&v, ta->data + (size_t)idx->as.i * 8, 8);
                registers[dest] = aer_real(v);
                DISPATCH();
            }
            if (ta->elem_kind == TYPED_ELEM_FLOAT32) {
                float v;
                memcpy(&v, ta->data + (size_t)idx->as.i * 4, 4);
                registers[dest] = aer_real((double)v);
                DISPATCH();
            }
        }
    }
    __attribute__((musttail)) return h_index_get_any_as_real(vm, pc, registers, c);
}

/* Any receiver the fused compound below does not cover -- a dict, a plain array, a narrow element --
   as the ordinary boxed read-modify-write its three-opcode expansion would have done. */
static void vm_index_compound_slow(VM* vm, const uint32_t* pc, Chunk* c, AerVal obj, AerVal idx,
                                   AerVal rhs, Opcode bin) {
    /* An existing dict entry is read and written through ONE resolution -- the whole reason the read
       and the write are in the same handler. A missing key falls through so its error is the one the
       separate read and write would have raised. */
    if (aer_type(obj) == TYPE_DICT && aer_type(idx) == TYPE_STRING) {
        AerDict* d = aer_as_dict(obj);
        AerString* is = aer_as_string(idx);
        unsigned int klen = hashtable_key_true_len(is->data, is->length);
        HashValue khash = hashtable_string_hash(is, klen);
        int at = hashtable_get_index_hashed(&d->map, is->data, klen, khash);
        if (at >= 0) {
            AerVal held = d->map.dense[at].payload;
            bool done;
            AerVal updated = vm_binary_fast(held, rhs, bin, aer_type(held), aer_type(rhs), &done);
            if (!done)
                updated = vm_binary_cold(c, held, rhs, bin, aer_type(held), aer_type(rhs));
            gc_barrier_dict(vm, d, (unsigned int)at, updated);
            d->map.dense[at].payload = updated;
            return;
        }
    }
    AerVal cur;
    vm_index_get_compute(obj, idx, &cur);
    bool handled;
    AerVal res = vm_binary_fast(cur, rhs, bin, aer_type(cur), aer_type(rhs), &handled);
    if (!handled)
        res = vm_binary_cold(c, cur, rhs, bin, aer_type(cur), aer_type(rhs));
    vm_index_set_compute(vm, obj, idx, res);
}

/* `arr[rk] OP= rhs` on any receiver: the same read-modify-write the three-opcode expansion did, in
   one dispatch, and without boxing the element into a temp register in between. The read and the
   write still resolve separately, so a dict still hashes its key twice. */
HANDLER(index_compound)
    AerVal obj = registers[(int)UNPACK_A(op_word)];
    Opcode bin = (Opcode)UNPACK_B(op_word);
    uint32_t idx_rhs_word = READ();
    AerVal idx = *vm_rk_ptr16(registers, const_pool, UNPACK_2X16_HI(idx_rhs_word));
    AerVal rhs = *vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(idx_rhs_word));
    vm_index_compound_slow(vm, pc, c, obj, idx, rhs, bin);
    gc_maybe_collect(vm);
    DISPATCH();
}

/* Only ADD/SUB/MUL reach here (parser.c), so the three-way choice is exhaustive. */
HANDLER(index_compound_raw_int)
    AerVal obj = registers[(int)UNPACK_A(op_word)];
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    Opcode bin = (Opcode)UNPACK_B(op_word);
    int64_t rhs = registers[READ()].as.i;
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count && ta->elem_kind == TYPED_ELEM_INT64) {
            unsigned char* at = ta->data + (size_t)idx->as.i * 8;
            int64_t cur;
            memcpy(&cur, at, 8);
            cur = bin == OP_ADD ? cur + rhs : bin == OP_SUB ? cur - rhs : cur * rhs;
            memcpy(at, &cur, 8);
            DISPATCH();
        }
    }
    vm_index_compound_slow(vm, pc, c, obj, *idx, aer_int(rhs), bin);
    DISPATCH();
}

HANDLER(index_compound_raw_real)
    AerVal obj = registers[(int)UNPACK_A(op_word)];
    AerVal* idx = vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    Opcode bin = (Opcode)UNPACK_B(op_word);
    double rhs = registers[READ()].as.d;
    if (aer_type(obj) == TYPE_TYPED_ARRAY && idx->tag == TYPE_INTEGER) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if ((uint64_t)idx->as.i < (uint64_t)ta->count && ta->elem_kind == TYPED_ELEM_FLOAT64) {
            unsigned char* at = ta->data + (size_t)idx->as.i * 8;
            double cur;
            memcpy(&cur, at, 8);
            cur = bin == OP_ADD ? cur + rhs : bin == OP_SUB ? cur - rhs : cur * rhs;
            memcpy(at, &cur, 8);
            DISPATCH();
        }
    }
    vm_index_compound_slow(vm, pc, c, obj, *idx, aer_real(rhs), bin);
    DISPATCH();
}

/* `arr[a:b]` -- vm_slice_bounds() resolves/clamps the bounds; a slice is always a fresh copy. */
HANDLER(slice_get)
    int dest_reg = (int)UNPACK_A(op_word);
    int arr_reg = (int)UNPACK_B(op_word);
    uint32_t bounds_word = READ();
    AerVal start_v = *vm_rk_ptr16(registers, const_pool, UNPACK_2X16_HI(bounds_word));
    AerVal end_v = *vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(bounds_word));
    AerVal obj = registers[arr_reg];
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) {
            error("Structs cannot be sliced");
            registers[dest_reg] = aer_null();
            DISPATCH();
        }
        int64_t start, end;
        if (!vm_slice_bounds(start_v, end_v, (int64_t)a->count, &start, &end)) {
            registers[dest_reg] = aer_null();
            DISPATCH();
        }
        unsigned int n = (unsigned int)(end - start);
        /* a slice is always a plain array, even of a struct */
        AerArray* r = heap_new_array(&vm->heap, n > 0 ? n : 4);
        r->count = n;
        for (unsigned int i = 0; i < n; i++)
            r->items[i] = a->items[start + i];
        registers[dest_reg] = aer_array_val(r);
    } else if (aer_type(obj) == TYPE_STRING) {
        AerString* os = aer_as_string(obj);
        int64_t start, end;
        if (!vm_slice_bounds(start_v, end_v, (int64_t)os->length, &start, &end)) {
            registers[dest_reg] = aer_null();
            DISPATCH();
        }
        unsigned int sub_len = (unsigned int)(end - start);
        registers[dest_reg] = aer_make_string_copy(
            os->data + start, sub_len); /* no chunk_add_pool interning -- see vm_to_str's comment */
    } else if (aer_type(obj) == TYPE_TYPED_ARRAY) {
        /* A copy, like the array and string cases -- not a view. A view would have to keep its
           parent alive and know not to free a buffer it borrowed, and slicing is the same spelling
           on all three types, so it should not mean something structurally different on one. */
        AerTypedArray* ta = aer_as_typed_array(obj);
        int64_t start, end;
        if (!vm_slice_bounds(start_v, end_v, (int64_t)ta->count, &start, &end)) {
            registers[dest_reg] = aer_null();
            DISPATCH();
        }
        unsigned int n = (unsigned int)(end - start);
        AerTypedArray* r = vm_new_typed_array(ta->elem_kind, n);
        unsigned int w = vm_typed_elem_width(ta->elem_kind);
        if (n > 0)
            memcpy(r->data, ta->data + (size_t)start * w, (size_t)n * w);
        registers[dest_reg] = aer_typed_array_val(r);
    } else {
        error("Cannot slice this type");
        registers[dest_reg] = aer_null();
    }
    gc_maybe_collect(
        vm); /* array branch: pool_alloc(&array_pool); string branch: aer_make_string; the null-result branches above are harmless no-ops here too */
    DISPATCH();
}

/* Dict literal -- each key stored as an owned copy, never an alias into the source string. */
HANDLER(dict_new)
    int dest_reg = (int)UNPACK_A(op_word);
    int pair_reg_base = (int)UNPACK_B(op_word);
    int pair_count = (int)UNPACK_C(op_word);
    AerDict* d = heap_alloc(&vm->heap, &vm->heap.dict_pool);
    memset(&d->map, 0, sizeof(d->map));
    d->map.pools = &vm->heap.dict_hash_pools;
    d->dirty_cards = NULL;
    d->dirty_cards_bytes = 0;
    d->dirty_min_byte = (unsigned int)-1;
    d->dirty_max_byte = 0;
    d->dirty_all = false;
    if (pair_count > 0)
        hashtable_reserve(&d->map, (unsigned int)pair_count);
    for (int i = 0; i < pair_count; i++) {
        AerVal key = registers[pair_reg_base + 2 * i];
        AerVal val = registers[pair_reg_base + 2 * i + 1];
        if (aer_type(key) != TYPE_STRING) {
            error("Hashtable keys must be strings");
            continue;
        }
        AerString* ks = aer_as_string(key);
        unsigned int klen = hashtable_key_true_len(ks->data, ks->length);
        HashValue khash = hashtable_string_hash(ks, klen);
        char* k = hashtable_key_dup_known(d->map.pools, ks->data, klen);
        hashtable_put_hashed(&d->map, k, klen, khash, val);
    }
    registers[dest_reg] = aer_dict_val(d);
    gc_maybe_collect(vm); /* pool_alloc(&dict_pool) above; result already rooted */
    DISPATCH();
}

/* Arrays yield items, dicts yield keys, strings yield 1-char strings. */
HANDLER(iter_next_array)
    int col_reg = (int)UNPACK_A(op_word);
    int idx_reg = (int)UNPACK_B(op_word);
    int item_dest_reg = (int)UNPACK_C(op_word);
    int end_target = READ();
    AerVal col = registers[col_reg];
    int64_t idx = aer_as_int(registers[idx_reg]);
    if (aer_type(col) == TYPE_DICT) {
        /* Single-variable `for k in dict:` yields keys. */
        AerVal key;
        if (!vm_dict_next_key(aer_as_dict(col), &idx, &key)) {
            pc += (int32_t)end_target;
            DISPATCH();
        }
        registers[item_dest_reg] = key;
        registers[idx_reg] = aer_int(idx + 1);
        gc_maybe_collect(vm); /* vm_dict_next_key's owned-copy key string allocates */
        DISPATCH();
    }
    if (aer_type(col) == TYPE_STRING) {
        /* Yields one-character strings, same shape as dict-key iteration. */
        AerString* cs = aer_as_string(col);
        if ((uint64_t)idx >= cs->length) {
            pc += (int32_t)end_target;
            DISPATCH();
        }
        registers[item_dest_reg] = aer_make_string_copy(
            cs->data + idx, 1); /* no chunk_add_pool interning -- see vm_to_str's comment */
        registers[idx_reg] = aer_int(idx + 1);
        gc_maybe_collect(vm);
        DISPATCH();
    }
    if (aer_type(col) == TYPE_TYPED_ARRAY) {
        /* Unlike a packed array's struct elements (no standalone `arr[i]` reference, so no
           iteration either -- see README), a typed array's elements ARE plain scalar values, so
           iteration works exactly like an ordinary array's. */
        AerTypedArray* ta = aer_as_typed_array(col);
        if ((uint64_t)idx >= ta->count) {
            pc += (int32_t)end_target;
            DISPATCH();
        }
        unsigned int width = vm_typed_elem_width(ta->elem_kind);
        registers[item_dest_reg] = vm_typed_elem_read(ta->data + (size_t)idx * width, ta->elem_kind);
        registers[idx_reg] = aer_int(idx + 1);
        DISPATCH();
    }
    if (aer_type(col) != TYPE_ARRAY) {
        error("'for x in ...' only supports arrays, dicts, strings, and typed arrays");
        DISPATCH();
    }
    AerArray* a = aer_as_array(col);
    if ((uint64_t)idx >= a->count) {
        pc += (int32_t)end_target;
        DISPATCH();
    }
    registers[item_dest_reg] = a->items[idx];
    registers[idx_reg] = aer_int(idx + 1);
    DISPATCH();
}

/* `for k, v in dict:` -- see vm_dict_next_key (above) for the shared bucket-scan/copy-key logic. */
HANDLER(iter_next_pair)
    int col_reg = (int)UNPACK_A(op_word);
    int idx_reg = (int)UNPACK_B(op_word);
    int key_dest_reg = (int)UNPACK_C(op_word);
    int val_dest_reg = (int)READ();
    int end_target = READ();
    AerVal col = registers[col_reg];
    if (aer_type(col) != TYPE_DICT) {
        error("for k, v requires a hashtable");
        pc += (int32_t)end_target;
        DISPATCH();
    }
    AerDict* d = aer_as_dict(col);
    int64_t idx = aer_as_int(registers[idx_reg]);
    AerVal key;
    if (!vm_dict_next_key(d, &idx, &key)) {
        pc += (int32_t)end_target;
        DISPATCH();
    }
    registers[key_dest_reg] = key;
    registers[val_dest_reg] = d->map.dense[idx].payload;
    registers[idx_reg] = aer_int(idx + 1);
    gc_maybe_collect(vm); /* vm_dict_next_key's owned-copy key string allocates */
    DISPATCH();
}

/* Runs once before the loop -- same checks as OP_ITER_RANGE_LOOP below, but never advances
   cur_reg (nothing to prepare yet). */
HANDLER(iter_range_prep)
    int cur_reg = (int)UNPACK_A(op_word);
    int end_reg = (int)UNPACK_B(op_word);
    int step_reg = (int)UNPACK_C(op_word);
    uint32_t item_word = READ();
    int item_dest_reg = RANGE_PREP_ITEM_REG(item_word);
    int empty_target = READ();
    AerVal cur_v = registers[cur_reg];
    AerVal end_v = registers[end_reg];
    AerVal step_v = registers[step_reg];
    if (aer_type(cur_v) != TYPE_INTEGER || aer_type(end_v) != TYPE_INTEGER ||
        aer_type(step_v) != TYPE_INTEGER) {
        error("Range bounds and step must be integers");
        pc += (int32_t)empty_target;
        DISPATCH();
    }
    int64_t cur = aer_as_int(cur_v), rng_end = aer_as_int(end_v), step = aer_as_int(step_v);
    if (step <= 0) {
        error("Range step must be a positive integer (direction is inferred from the bounds, not the step's "
              "sign)");
        pc += (int32_t)empty_target;
        DISPATCH();
    }
    /* The body indexes unchecked on a proof that every value lands in [0, length), which holds only
       while the loop ascends from a non-negative start -- so verify that precondition once here
       rather than per iteration. Neither half is hypothetical: direction is inferred from the
       bounds, so `for i in 200..length(a)` on a short array descends off the end (this segfaulted
       before the check existed), and overflow in a computed start can wrap it negative. */
    /* One unsigned compare covers both halves: rng_end is a length so it is never negative, and a
       negative cur reinterprets as a huge unsigned, failing the same test. Splitting the cases
       costs nothing because it only happens on the way to raising. */
    if ((item_word & RANGE_PREP_GUARD_NONNEG) && (uint64_t)cur > (uint64_t)rng_end) {
        if (cur < 0)
            error("Range start is negative (%lld) -- an index computation overflowed", (long long)cur);
        else
            error("Range start %lld is past its end %lld, so this loop would count downwards out of "
                  "the collection it indexes",
                  (long long)cur, (long long)rng_end);
        pc += (int32_t)empty_target;
        DISPATCH();
    }
    /* Precomputes the iteration count once (ceiling division, matching Lua's FORLOOP) instead of
       re-deriving it every dispatch. count==0 means empty range. */
    bool ascending = cur < rng_end;
    int64_t diff = ascending ? (rng_end - cur) : (cur - rng_end);
    /* step==1 (the common case) skips the division -- this target has no fast hardware divide,
       and short re-entered loops pay that cost often enough for it to be a real win. */
    int64_t count = (step == 1) ? diff : (diff + step - 1) / step;
    if (count == 0) {
        pc += (int32_t)empty_target;
        DISPATCH();
    }
    /* end_reg/step_reg repurposed for this loop's life -- arg_materialize's snapshot guarantees
       they're fresh, loop-owned registers nothing else reads. */
    registers[end_reg] = aer_int(count - 1); /* iterations remaining AFTER this one */
    registers[step_reg] =
        aer_int(ascending ? step : -step); /* direction baked in once, not re-inferred every iteration */
    registers[item_dest_reg] = cur_v;
    DISPATCH();
}

/* Runs at the bottom of the loop body. end_reg/step_reg hold PREP's repurposed countdown/signed
   step, not the original bound, so no comparison against the original limit is ever needed (Lua's
   FORLOOP shape); on exhaustion it falls through leaving cur_reg/item_dest_reg at their last value.
   cur/remaining/step are written value-only -- PREP validated all three as integers and they are
   loop-owned snapshots, so nothing can have retagged them. body_target is always a resolved
   address, never a patch placeholder. No gc_maybe_collect -- nothing here allocates. */
HANDLER(iter_range_loop)
    int cur_reg = (int)UNPACK_A(op_word);
    int remaining_reg = (int)UNPACK_B(op_word);
    int signed_step_reg = (int)UNPACK_C(op_word);
    int item_dest_reg = (int)READ();
    int body_target = READ();
    int64_t remaining = registers[remaining_reg].as.i;
    if (remaining == 0) {
        DISPATCH(); /* exhausted -- fall through to the exit code, cur_reg/item_dest_reg untouched */
    }
    int64_t new_cur = registers[cur_reg].as.i + registers[signed_step_reg].as.i;
    registers[cur_reg].as.i = new_cur;
    registers[remaining_reg].as.i = remaining - 1;
    /* Equal when the parser proved this loop's body never writes the loop variable, so cur_reg IS
       item_dest_reg and the store above already published this iteration's value. */
    if (cur_reg != item_dest_reg)
        registers[item_dest_reg] = aer_int(new_cur);
    pc += (int32_t)body_target;
    /* range-for's own dedicated back-edge -- h_jump's check doesn't cover this loop shape since
       it never goes through a plain OP_JUMP. */
    if (vm->slice_max && --vm->slice_budget == 0) {
        vm->ip = (unsigned int)(pc - c->code);
        return VM_SLICE_YIELDED;
    }
    DISPATCH();
}

/* chunk_find_shape() by name, arity check, one struct_pools[] allocation (sized to the tier this
   shape's instance_bytes fits), trailing fields default-filled. */
HANDLER(struct_new)
    int dest_reg = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count = (int)UNPACK_C(op_word);
    int type_name_pool_idx = (int)READ();
    const char* name = aer_as_string(c->pool[type_name_pool_idx])->data;
    Shape* shape = chunk_shape_for_pool_idx(c, (unsigned int)type_name_pool_idx);
    if (!shape) {
        error("'%s' is not defined", name);
        DISPATCH();
    }
    if ((unsigned int)arg_count > shape->field_count) {
        error("'%s' takes at most %u argument%s, got %d", name, shape->field_count,
              shape->field_count == 1 ? "" : "s", arg_count);
        DISPATCH();
    }
    /* Positional args must match a typed field too -- the fused field-arithmetic fast path trusts
       every field unconditionally, not just ones set via `.field =`. Checked before allocating. */
    for (int i = 0; i < arg_count; i++) {
        ValueType declared = shape->field_types[i];
        if (declared != TYPE_ANY && registers[arg_reg_base + i].tag != declared) {
            error(
                "'%s' field '%s' is declared as a fixed type and cannot be constructed with a different type",
                name, aer_as_string(c->pool[shape->field_names[i]])->data);
            DISPATCH();
        }
        if (!vm_check_narrow_field_write(declared, shape->field_narrow[i], registers[arg_reg_base + i]))
            DISPATCH();
    }
    AerStruct* s = heap_alloc(&vm->heap, struct_pool_for_size(&vm->heap, shape->instance_bytes));
    s->shape = shape;
    s->fields = (unsigned char*)s + sizeof(AerStruct);
    /* The _at form, with the shape already in hand: vm_struct_field_write re-reads s->shape twice
       per field through a non-inlinable external call, and this loop runs for every field of every
       construction -- 8.19% of bench/binary_trees.aer sat in it. */
    for (unsigned int i = 0; i < (unsigned int)arg_count; i++)
        vm_struct_field_write_at(s, shape->field_offsets[i], shape->field_types[i], shape->field_narrow[i],
                                 registers[arg_reg_base + i]);
    for (unsigned int i = (unsigned int)arg_count; i < shape->field_count; i++)
        vm_struct_field_write_at(s, shape->field_offsets[i], shape->field_types[i], shape->field_narrow[i],
                                 vm_default_value(vm, shape->field_defaults[i]));
    registers[dest_reg] = aer_struct_val(s);
    gc_maybe_collect(
        vm); /* pool_alloc(&struct_pools[tier]) above, plus any vm_default_value array/dict defaults -- all rooted now that the struct itself is stored */
    DISPATCH();
}

SEPARATE_HANDLER(field_get_by_name)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int dest_reg = (int)UNPACK_A(op_word);
    int struct_reg = (int)UNPACK_B(op_word);
    int field_idx = (int)READ();
    AerStruct* oa;
    int slot;
    unsigned int foffset;
    ValueType ftype;
    bool narrow;
    if (!vm_resolve_field(registers, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &ftype, &narrow))
        DISPATCH();
    registers[dest_reg] = vm_struct_field_read_at(oa, foffset, ftype, narrow);
    DISPATCH();
}

HANDLER(field_get)
    AerVal* obj = &registers[(int)UNPACK_B(op_word)];
    FieldCacheEntry* entry = &c->field_cache[(unsigned int)(pc - c->code) - 1];
    if (obj->tag != TYPE_STRUCT || entry->shape != ((AerStruct*)obj->as.ptr)->shape)
        __attribute__((musttail)) return h_field_get_by_name(vm, pc, registers, c);
    (void)READ();
    registers[(int)UNPACK_A(op_word)] =
        vm_struct_field_read_at((AerStruct*)obj->as.ptr, entry->offset, entry->ftype, entry->narrow);
    DISPATCH();
}

/* `x OP y.field` reaches here too: parse_binary_ops canonicalizes it into this opcode's `field OP' x`
   form at compile time (see that function's own comment, and OP_FIELD_BINARY's, vm.h, for why that
   is exact, not an approximation, for every operator that reaches here). */
HANDLER(field_binary)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int dest_reg = (int)UNPACK_A(op_word);
    int struct_reg = (int)UNPACK_B(op_word);
    Opcode bin_op = (Opcode)UNPACK_C(op_word);
    uint32_t field_rk_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_rk_word);
    AerVal* rhs = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerStruct* oa;
    int slot;
    unsigned int foffset;
    ValueType ftype;
    bool narrow;
    if (!vm_resolve_field(registers, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &ftype, &narrow))
        DISPATCH();
    AerVal lhs = vm_struct_field_read_at(oa, foffset, ftype, narrow);
    ValueType ta = aer_type(lhs), tb = aer_type(*rhs);
    bool handled;
    registers[dest_reg] = vm_binary_fast(lhs, *rhs, bin_op, ta, tb, &handled);
    if (!handled) {
        registers[dest_reg] = vm_binary_cold(c, lhs, *rhs, bin_op, ta, tb);
        gc_maybe_collect(vm);
    }
    DISPATCH();
}

/* `struct.field OP= rhs` -- one vm_resolve_field/cache lookup covers the read, the compute, the
   type check and the write back, where a separate read and write would each pay their own. */
HANDLER(field_compound)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int struct_reg = (int)UNPACK_A(op_word);
    Opcode bin_op = (Opcode)UNPACK_B(op_word);
    uint32_t field_rk_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_rk_word);
    AerVal* rhs = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerStruct* oa;
    int slot;
    unsigned int foffset;
    ValueType ftype;
    bool narrow;
    if (!vm_resolve_field(registers, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &ftype, &narrow))
        DISPATCH();
    AerVal lhs = vm_struct_field_read_at(oa, foffset, ftype, narrow);
    ValueType ta = aer_type(lhs), tb = aer_type(*rhs);
    bool handled;
    AerVal result = vm_binary_fast(lhs, *rhs, bin_op, ta, tb, &handled);
    if (!handled) {
        result = vm_binary_cold(c, lhs, *rhs, bin_op, ta, tb);
        gc_maybe_collect(vm);
    }
    /* Same enforcement as OP_FIELD_SET's own -- the only other place a field's value changes. */
    if (ftype != TYPE_ANY && result.tag != ftype) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    if (!vm_check_narrow_field_write(ftype, narrow, result))
        DISPATCH();
    gc_barrier_struct(vm, oa, result);
    vm_struct_field_write_at(oa, foffset, ftype, narrow, result);
    DISPATCH();
}

/* Fuses `(A op1 B) op2 C` into one pass when A/B/C are all matching-shape typed arrays --
   vm_typed_array_chain2's own comment has the full "why" and the measured win. Any other operand
   shape (a plain int/real, a mismatched typed array, a different-length one) falls back to
   computing the EXACT unfused result: op1 first, then op2 on that intermediate -- the same value
   this would have produced as two separate statements, just without the fusion win for this one
   call. Correctness never depends on which path runs. */
HANDLER(typed_array_chain2)
    int dest_reg = (int)UNPACK_A(op_word);
    int a_reg = (int)UNPACK_B(op_word);
    int b_reg = (int)UNPACK_C(op_word);
    uint32_t word1 = READ();
    Opcode op1 = (Opcode)UNPACK_2X16_HI(word1);
    int c_reg = (int)UNPACK_2X16_LO(word1);
    Opcode op2 = (Opcode)READ();

    AerVal av = registers[a_reg], bv = registers[b_reg], cv = registers[c_reg];
    if (aer_type(av) == TYPE_TYPED_ARRAY && aer_type(bv) == TYPE_TYPED_ARRAY &&
        aer_type(cv) == TYPE_TYPED_ARRAY) {
        AerTypedArray* ta = aer_as_typed_array(av);
        AerTypedArray* tb = aer_as_typed_array(bv);
        AerTypedArray* tcc = aer_as_typed_array(cv);
        int i1 = op_chain2_index(op1), i2 = op_chain2_index(op2);
        if (i1 >= 0 && i2 >= 0 &&
            (ta->elem_kind == TYPED_ELEM_FLOAT32 || ta->elem_kind == TYPED_ELEM_FLOAT64) &&
            ta->elem_kind == tb->elem_kind && ta->elem_kind == tcc->elem_kind && ta->count == tb->count &&
            ta->count == tcc->count) {
            registers[dest_reg] = vm_typed_array_chain2(ta, tb, tcc, op1, op2);
            gc_maybe_collect(vm); /* vm_new_typed_array (inside) allocates */
            DISPATCH();
        }
    }
    /* Fallback: exact unfused computation, still fully correct. */
    bool handled;
    ValueType ta_ = aer_type(av), tb_ = aer_type(bv);
    AerVal tmp = vm_binary_fast(av, bv, op1, ta_, tb_, &handled);
    if (!handled) {
        tmp = vm_binary_cold(c, av, bv, op1, ta_, tb_);
        gc_maybe_collect(vm);
    }
    ValueType ttmp = aer_type(tmp), tc_ = aer_type(cv);
    AerVal result = vm_binary_fast(tmp, cv, op2, ttmp, tc_, &handled);
    if (!handled) {
        result = vm_binary_cold(c, tmp, cv, op2, ttmp, tc_);
        gc_maybe_collect(vm);
    }
    registers[dest_reg] = result;
    DISPATCH();
}

/* Shell mode auto-print: a bare statement's result is printed unless null. */
HANDLER(print_repl)
    int src_reg = (int)UNPACK_A(op_word);
    AerVal v = registers[src_reg];
    if (aer_type(v) != TYPE_NULL) {
        vm_print_value(c, v, false);
        printf("\n");
    }
    DISPATCH();
}

/* gc_barrier_struct is the write barrier every mutating struct field-set needs. */
SEPARATE_HANDLER(field_set_by_name)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int struct_reg = (int)UNPACK_A(op_word);
    AerVal* val = vm_rk_ptr16(registers, const_pool, UNPACK_W16(op_word));
    int field_idx = (int)READ();
    AerStruct* oa;
    int slot;
    unsigned int foffset;
    ValueType declared;
    bool narrow;
    if (!vm_resolve_field(registers, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &declared,
                          &narrow))
        DISPATCH();
    /* Enforced once here (the only place a field's value changes), trusted everywhere else
       including the fused fast path. TYPE_ANY means untyped. */
    if (declared != TYPE_ANY && val->tag != declared) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    if (!vm_check_narrow_field_write(declared, narrow, *val))
        DISPATCH();
    gc_barrier_struct(vm, oa, *val);
    vm_struct_field_write_at(oa, foffset, declared, narrow, *val);
    DISPATCH();
}

HANDLER(field_set)
    AerVal* obj = &registers[(int)UNPACK_A(op_word)];
    AerVal* val = vm_rk_ptr16(registers, const_pool, UNPACK_W16(op_word));
    FieldCacheEntry* entry = &c->field_cache[(unsigned int)(pc - c->code) - 1];
    if (obj->tag != TYPE_STRUCT || entry->shape != ((AerStruct*)obj->as.ptr)->shape ||
        (entry->ftype != TYPE_ANY && val->tag != entry->ftype) ||
        !vm_fits_narrow_field(entry->ftype, entry->narrow, *val))
        __attribute__((musttail)) return h_field_set_by_name(vm, pc, registers, c);
    (void)READ();
    AerStruct* s = (AerStruct*)obj->as.ptr;
    gc_barrier_struct(vm, s, *val);
    vm_struct_field_write_at(s, entry->offset, entry->ftype, entry->narrow, *val);
    DISPATCH();
}

/* Shape-specialized raw field access -- only in a specialized body, where the parser resolved the
   field offset against a proven Shape, so there is no runtime lookup and neither side is boxed.
   The aer_type() check is a safety net: the reg_known_shape[] audit is not proven exhaustive, and a
   gap should be loud rather than silent corruption. Storage kind stays in the opcode rather than
   becoming an operand -- see OPTIMIZATION_HISTORY.md for the measurement. */
/* Which storage a specialized field uses. Every handler below passes a literal, so each switch here
   folds to the single load or store that field needs -- the kind never reaches a runtime branch. */
typedef enum { RAWW_INT, RAWW_REAL, RAWW_INT32, RAWW_FLOAT32 } RawWidth;

static inline __attribute__((always_inline)) void raw_field_read(AerVal* dst, unsigned char* src, RawWidth w) {
    switch (w) {
        case RAWW_INT:
            dst->tag = TYPE_INTEGER;
            memcpy(&dst->as.i, src, 8);
            break;
        case RAWW_REAL:
            dst->tag = TYPE_REAL;
            memcpy(&dst->as.d, src, 8);
            break;
        case RAWW_INT32: *dst = aer_int(vm_raw_read_int32(src)); break;
        case RAWW_FLOAT32: *dst = aer_real(vm_raw_read_float32(src)); break;
    }
}

static inline __attribute__((always_inline)) void raw_field_write(VM* vm, const uint32_t* pc,
                                                                  unsigned char* dst, AerVal src,
                                                                  RawWidth w) {
    switch (w) {
        case RAWW_INT: memcpy(dst, &src.as.i, 8); break;
        case RAWW_REAL: memcpy(dst, &src.as.d, 8); break;
        case RAWW_INT32:
            if (vm_fits_narrow_field(TYPE_INTEGER, true, src))
                vm_raw_write_int32(dst, src.as.i);
            else
                vm_check_narrow_field_write(TYPE_INTEGER, true, src); /* reports */
            break;
        case RAWW_FLOAT32: vm_raw_write_float32(dst, src.as.d); break;
    }
}

/* The struct a specialized access names, or NULL once it has raised -- callers dispatch either way.
   The check is a safety net rather than a real possibility: a gap in the reg_known_shape[] audit
   should be loud instead of silently reinterpreting some other value's bits as a field. */
static inline __attribute__((always_inline)) AerStruct* raw_field_struct(VM* vm, const uint32_t* pc, AerVal obj) {
    if (aer_type(obj) == TYPE_STRUCT)
        return aer_as_struct(obj);
    error("internal error: specialized struct field access on a non-struct value");
    return NULL;
}

/* The raw field bytes a specialized packed-array access names, or NULL once it has raised. Below the
   `error` macro, so a raised error records this instruction rather than the caller's. */
static inline __attribute__((always_inline)) unsigned char* vm_packed_raw_elem(VM* vm, const uint32_t* pc,
                                                                               AerVal obj, AerVal* idx,
                                                                               unsigned int foffset) {
    if (aer_type(obj) != TYPE_PACKED_ARRAY) {
        error("internal error: specialized packed-array field access on a non-packed-array value");
        return NULL;
    }
    AerPackedArray* pa = aer_as_packed_array(obj);
    uint64_t i;
    if (!vm_array_position(*idx, pa->count, &i)) {
        vm_array_index_error(*idx, pa->count);
        return NULL;
    }
    return pa->data + (size_t)i * pa->shape->instance_bytes + foffset;
}

/* vm_packed_raw_elem for an index a loop proved in range (parser.c's index_safe_unchecked): only the array's
   own type check remains, as a defensive net. */
static inline __attribute__((always_inline)) unsigned char*
vm_packed_raw_elem_unchecked(VM* vm, const uint32_t* pc, AerVal obj, AerVal* idx, unsigned int foffset) {
    if (aer_type(obj) != TYPE_PACKED_ARRAY) {
        error("internal error: specialized packed-array field access on a non-packed-array value");
        return NULL;
    }
    AerPackedArray* pa = aer_as_packed_array(obj);
    int64_t i = aer_as_int(*idx);
    return pa->data + (size_t)i * pa->shape->instance_bytes + foffset;
}

static inline __attribute__((always_inline)) unsigned char* raw_elem(VM* vm, const uint32_t* pc, AerVal arr,
                                                                     AerVal* idx, unsigned int foffset,
                                                                     bool checked) {
    return checked ? vm_packed_raw_elem(vm, pc, arr, idx, foffset)
                   : vm_packed_raw_elem_unchecked(vm, pc, arr, idx, foffset);
}

/* `+=`, `-=` and `*=` are the whole specialized compound set; anything else raises and leaves the
   field alone. Int and real never share a field, so the width picks the arithmetic too. */
static inline __attribute__((always_inline)) bool raw_field_compound(VM* vm, const uint32_t* pc, unsigned char* elem,
                                                 AerVal rhs, Opcode bin_op, RawWidth w) {
    if (w == RAWW_INT || w == RAWW_INT32) {
        int64_t lhs, result;
        if (w == RAWW_INT)
            memcpy(&lhs, elem, 8);
        else
            lhs = vm_raw_read_int32(elem);
        switch (bin_op) {
            case OP_ADD: result = lhs + rhs.as.i; break;
            case OP_SUB: result = lhs - rhs.as.i; break;
            case OP_MUL: result = lhs * rhs.as.i; break;
            default: error("internal error: unsupported raw compound-assign op"); return false;
        }
        if (w == RAWW_INT) {
            memcpy(elem, &result, 8);
        } else if (vm_fits_narrow_field(TYPE_INTEGER, true, aer_int(result))) {
            vm_raw_write_int32(elem, result);
        } else {
            vm_check_narrow_field_write(TYPE_INTEGER, true, aer_int(result));
            return false;
        }
    } else {
        double lhs, result;
        if (w == RAWW_REAL)
            memcpy(&lhs, elem, 8);
        else
            lhs = vm_raw_read_float32(elem);
        switch (bin_op) {
            case OP_ADD: result = lhs + rhs.as.d; break;
            case OP_SUB: result = lhs - rhs.as.d; break;
            case OP_MUL: result = lhs * rhs.as.d; break;
            default: error("internal error: unsupported raw compound-assign op"); return false;
        }
        if (w == RAWW_REAL)
            memcpy(elem, &result, 8);
        else
            vm_raw_write_float32(elem, result);
    }
    return true;
}

static inline __attribute__((always_inline)) void field_get_raw(VM* vm, const uint32_t* pc, AerVal* registers,
                                            uint32_t op_word, unsigned int foffset, RawWidth w) {
    AerStruct* s = raw_field_struct(vm, pc, registers[UNPACK_B(op_word)]);
    if (s)
        raw_field_read(&registers[UNPACK_A(op_word)], s->fields + foffset, w);
}

static inline __attribute__((always_inline)) void field_set_raw(VM* vm, const uint32_t* pc, AerVal* registers,
                                            uint32_t op_word, unsigned int foffset, int src_slot,
                                            RawWidth w) {
    AerStruct* s = raw_field_struct(vm, pc, registers[UNPACK_A(op_word)]);
    if (s)
        raw_field_write(vm, pc, s->fields + foffset, registers[src_slot], w);
}

static inline __attribute__((always_inline)) void field_compound_raw(VM* vm, const uint32_t* pc, AerVal* registers,
                                                 uint32_t op_word, unsigned int foffset,
                                                 int rhs_slot, RawWidth w) {
    AerStruct* s = raw_field_struct(vm, pc, registers[UNPACK_A(op_word)]);
    if (s)
        raw_field_compound(vm, pc, s->fields + foffset, registers[rhs_slot],
                           (Opcode)UNPACK_B(op_word), w);
}

static inline __attribute__((always_inline)) void index_field_get_raw(VM* vm, const uint32_t* pc,
                                                                      AerVal* registers, Chunk* c,
                                                                      uint32_t op_word,
                                                                      uint32_t field_rk_word, RawWidth w,
                                                                      bool checked) {
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    unsigned char* elem = raw_elem(vm, pc, registers[UNPACK_B(op_word)], idx, foffset, checked);
    if (elem)
        raw_field_read(&registers[UNPACK_A(op_word)], elem, w);
}

static inline __attribute__((always_inline)) void index_field_set_raw(VM* vm, const uint32_t* pc,
                                                                      AerVal* registers, Chunk* c,
                                                                      uint32_t op_word,
                                                                      uint32_t off_slot_word, RawWidth w,
                                                                      bool checked) {
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_W16(op_word));
    unsigned int foffset = UNPACK_2X16_HI(off_slot_word);
    unsigned char* elem = raw_elem(vm, pc, registers[UNPACK_A(op_word)], idx, foffset, checked);
    if (elem)
        raw_field_write(vm, pc, elem, registers[UNPACK_2X16_LO(off_slot_word)], w);
}

static inline __attribute__((always_inline)) void index_field_compound_raw(VM* vm, const uint32_t* pc, AerVal* registers,
                                                       Chunk* c, uint32_t op_word,
                                                       uint32_t field_rk_word, int rhs_slot,
                                                       RawWidth w, bool checked) {
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    unsigned char* elem = raw_elem(vm, pc, registers[UNPACK_A(op_word)], idx, foffset, checked);
    if (elem)
        raw_field_compound(vm, pc, elem, registers[rhs_slot], (Opcode)UNPACK_B(op_word), w);
}

/* `+=` with bin_op already known at compile time, so it skips the switch the general form runs. */
static inline __attribute__((always_inline)) void index_field_add_raw(VM* vm, const uint32_t* pc,
                                                                      AerVal* registers, Chunk* c,
                                                                      uint32_t op_word,
                                                                      uint32_t field_rk_word, int rhs_slot,
                                                                      RawWidth w, bool checked) {
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    unsigned char* elem = raw_elem(vm, pc, registers[UNPACK_A(op_word)], idx, foffset, checked);
    if (!elem)
        return;
    if (w == RAWW_REAL) {
        double lhs;
        memcpy(&lhs, elem, 8);
        lhs += registers[rhs_slot].as.d;
        memcpy(elem, &lhs, 8);
    } else {
        vm_raw_write_float32(elem, vm_raw_read_float32(elem) + registers[rhs_slot].as.d);
    }
}


HANDLER(index_field_get_raw_int)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_INT, true);
    DISPATCH();
}

HANDLER(index_field_get_raw_real)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_REAL, true);
    DISPATCH();
}

HANDLER(field_get_raw_int)
    unsigned int foffset = READ();
    field_get_raw(vm, pc, registers, op_word, foffset, RAWW_INT);
    DISPATCH();
}

HANDLER(field_get_raw_real)
    unsigned int foffset = READ();
    field_get_raw(vm, pc, registers, op_word, foffset, RAWW_REAL);
    DISPATCH();
}

HANDLER(index_field_get_raw_int32)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_INT32, true);
    DISPATCH();
}

HANDLER(index_field_get_raw_float32)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_FLOAT32, true);
    DISPATCH();
}

HANDLER(field_get_raw_int32)
    unsigned int foffset = READ();
    field_get_raw(vm, pc, registers, op_word, foffset, RAWW_INT32);
    DISPATCH();
}

HANDLER(field_get_raw_float32)
    unsigned int foffset = READ();
    field_get_raw(vm, pc, registers, op_word, foffset, RAWW_FLOAT32);
    DISPATCH();
}

HANDLER(index_field_set_raw_int)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_INT, true);
    DISPATCH();
}

HANDLER(index_field_set_raw_real)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_REAL, true);
    DISPATCH();
}

HANDLER(field_set_raw_int)
    unsigned int foffset = READ();
    int src_slot = (int)READ();
    field_set_raw(vm, pc, registers, op_word, foffset, src_slot, RAWW_INT);
    DISPATCH();
}

HANDLER(field_set_raw_real)
    unsigned int foffset = READ();
    int src_slot = (int)READ();
    field_set_raw(vm, pc, registers, op_word, foffset, src_slot, RAWW_REAL);
    DISPATCH();
}

HANDLER(index_field_set_raw_int32)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_INT32, true);
    DISPATCH();
}

HANDLER(index_field_set_raw_float32)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_FLOAT32, true);
    DISPATCH();
}

HANDLER(field_set_raw_int32)
    unsigned int foffset = READ();
    int src_slot = (int)READ();
    field_set_raw(vm, pc, registers, op_word, foffset, src_slot, RAWW_INT32);
    DISPATCH();
}

HANDLER(field_set_raw_float32)
    unsigned int foffset = READ();
    int src_slot = (int)READ();
    field_set_raw(vm, pc, registers, op_word, foffset, src_slot, RAWW_FLOAT32);
    DISPATCH();
}

HANDLER(field_compound_raw_int)
    unsigned int foffset = READ();
    int rhs_slot = (int)READ();
    field_compound_raw(vm, pc, registers, op_word, foffset, rhs_slot, RAWW_INT);
    DISPATCH();
}

HANDLER(field_compound_raw_real)
    unsigned int foffset = READ();
    int rhs_slot = (int)READ();
    field_compound_raw(vm, pc, registers, op_word, foffset, rhs_slot, RAWW_REAL);
    DISPATCH();
}

HANDLER(index_field_compound_raw_int)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_INT, true);
    DISPATCH();
}

HANDLER(index_field_compound_raw_real)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_REAL, true);
    DISPATCH();
}

HANDLER(index_field_get_raw_int_unchecked)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_INT, false);
    DISPATCH();
}

HANDLER(index_field_get_raw_real_unchecked)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_REAL, false);
    DISPATCH();
}

HANDLER(index_field_set_raw_int_unchecked)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_INT, false);
    DISPATCH();
}

HANDLER(index_field_set_raw_real_unchecked)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_REAL, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_int_unchecked)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_INT, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_real_unchecked)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_REAL, false);
    DISPATCH();
}

HANDLER(field_compound_raw_int32)
    unsigned int foffset = READ();
    int rhs_slot = (int)READ();
    field_compound_raw(vm, pc, registers, op_word, foffset, rhs_slot, RAWW_INT32);
    DISPATCH();
}

HANDLER(field_compound_raw_float32)
    unsigned int foffset = READ();
    int rhs_slot = (int)READ();
    field_compound_raw(vm, pc, registers, op_word, foffset, rhs_slot, RAWW_FLOAT32);
    DISPATCH();
}

HANDLER(index_field_compound_raw_int32)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_INT32, true);
    DISPATCH();
}

HANDLER(index_field_compound_raw_float32)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_FLOAT32, true);
    DISPATCH();
}

HANDLER(index_field_get_raw_int32_unchecked)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_INT32, false);
    DISPATCH();
}

HANDLER(index_field_get_raw_float32_unchecked)
    uint32_t field_rk_word = READ();
    index_field_get_raw(vm, pc, registers, c, op_word, field_rk_word, RAWW_FLOAT32, false);
    DISPATCH();
}

HANDLER(index_field_set_raw_int32_unchecked)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_INT32, false);
    DISPATCH();
}

HANDLER(index_field_set_raw_float32_unchecked)
    uint32_t off_slot_word = READ();
    index_field_set_raw(vm, pc, registers, c, op_word, off_slot_word, RAWW_FLOAT32, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_int32_unchecked)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_INT32, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_float32_unchecked)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_compound_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot,
                            RAWW_FLOAT32, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_real_unchecked_add)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_add_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot, RAWW_REAL, false);
    DISPATCH();
}

HANDLER(index_field_compound_raw_float32_unchecked_add)
    uint32_t field_rk_word = READ();
    int rhs_slot = (int)READ();
    index_field_add_raw(vm, pc, registers, c, op_word, field_rk_word, rhs_slot, RAWW_FLOAT32, false);
    DISPATCH();
}


/* `field += a*b` in one dispatch -- see OP_FIELD_COMPOUND_RAW_FLOAT32_FMA (vm.h). The multiply is
   parenthesised to keep the same rounding the unfused pair had. */
HANDLER(field_compound_raw_float32_fma)
    int struct_reg = (int)UNPACK_A(op_word);
    int a = (int)UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    unsigned int foffset = READ();
    AerVal obj = registers[struct_reg];
    if (aer_type(obj) != TYPE_STRUCT) {
        error("internal error: specialized struct field access on a non-struct value");
        DISPATCH();
    }
    unsigned char* slot = aer_as_struct(obj)->fields + foffset;
    vm_raw_write_float32(slot, vm_raw_read_float32(slot) + (registers[a].as.d * registers[b].as.d));
    DISPATCH();
}

HANDLER(index_field_compound_raw_real_unchecked_fma)
    int arr_reg = (int)UNPACK_A(op_word);
    int a = (int)UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    uint32_t field_rk_word = READ();
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    unsigned char* elem = vm_packed_raw_elem_unchecked(vm, pc, registers[arr_reg], idx, foffset);
    if (!elem)
        DISPATCH();
    double lhs;
    memcpy(&lhs, elem, 8);
    lhs = lhs + (registers[a].as.d * registers[b].as.d);
    memcpy(elem, &lhs, 8);
    DISPATCH();
}

/* `[value; count]` -- fill_reg is already evaluated exactly once by the parser, so this branches on
   its RUNTIME type. Eligibility (every field a fixed primitive) has to be checked here rather than
   at compile time: a Shape is only fully known once its OP_DEFINE_STRUCT has run. */
HANDLER(array_repeat)
    int dest_reg = (int)UNPACK_A(op_word);
    int fill_reg = (int)UNPACK_B(op_word);
    int narrow_flag = (int)UNPACK_C(op_word);
    uint32_t count_word = READ();
    AerVal* count_v = vm_rk_ptr16(registers, const_pool, (uint16_t)count_word);
    if (aer_type(*count_v) != TYPE_INTEGER) {
        error("Repeat-literal array count must be an integer");
        DISPATCH();
    }
    int64_t count = aer_as_int(*count_v);
    if (count < 0) {
        error("Repeat-literal array count must not be negative");
        DISPATCH();
    }

    AerVal fill = registers[fill_reg];
    if (aer_type(fill) == TYPE_STRUCT) {
        AerStruct* src = aer_as_struct(fill);
        Shape* shape = src->shape;
        for (unsigned int i = 0; i < shape->field_count; i++) {
            ValueType ft = shape->field_types[i];
            if (ft != TYPE_INTEGER && ft != TYPE_REAL && ft != TYPE_BOOLEAN) {
                const char* got = ft == TYPE_ANY ? "untyped (no annotation)" : aer_value_type_names[ft];
                error("'%s' cannot be packed into an array: field '%s' must be integer/float/boolean, not %s",
                      aer_as_string(c->pool[shape->name])->data,
                      aer_as_string(c->pool[shape->field_names[i]])->data, got);
                DISPATCH();
            }
        }
        /* A narrow (int32/float32) field packs fine here -- every packed-array element read/write
           (this handler and OP_INDEX_FIELD_GET/SET/COMPOUND's packed branches) uses
           shape->instance_bytes as the real per-element stride, not a hardcoded 8-bytes-per-field
           assumption, so a mix of narrow and wide fields lays out correctly either way. */
        unsigned int element_size = shape->instance_bytes;
        AerPackedArray* pa = heap_alloc(&vm->heap, &vm->heap.packed_array_pool);
        pa->count = (unsigned int)count;
        pa->shape = shape;
        /* malloc(0) is implementation-defined -- skip it for a zero-count array; bounds checks
           reject every later access anyway. */
        pa->data = count > 0 ? xmalloc((size_t)count * (size_t)element_size) : NULL;
        vm->heap.young_bytes += (size_t)count * (size_t)element_size;
        /* Every eligible field is raw -- 8 bytes, or 4 if narrow (TYPE_ANY, the only field kind
           needing a full boxed AerVal, was already rejected above) -- so src->fields IS one
           element's worth of bytes at exactly instance_bytes, laid out identically to a packed
           element: a straight memcpy per element, not a field-by-field copy. src's OWN field values
           are replicated, not the Shape's defaults, so `[Particle(1.0, 2.0); n]` differs from
           `[Particle(); n]`. */
        for (int64_t e = 0; e < count; e++)
            memcpy(pa->data + (size_t)e * element_size, src->fields, element_size);
        registers[dest_reg] = aer_packed_array_val(pa);
    } else if (aer_type(fill) == TYPE_INTEGER || aer_type(fill) == TYPE_REAL ||
               aer_type(fill) == TYPE_BOOLEAN) {
        /* narrow_flag is a pure parse-time decision (was the fill expression written as an
           `i`/`f`-suffixed literal directly in this position?) -- by construction, that always
           agrees with fill's own runtime tag (an `i`-suffixed literal is always a TYPE_INTEGER
           token, `f` always TYPE_REAL), so no cross-check against narrow_flag is needed here. */
        TypedArrayElemKind kind;
        if (narrow_flag == 1)
            kind = TYPED_ELEM_INT32;
        else if (narrow_flag == 2)
            kind = TYPED_ELEM_FLOAT32;
        else if (aer_type(fill) == TYPE_BOOLEAN)
            kind = TYPED_ELEM_BOOL;
        else
            kind = (aer_type(fill) == TYPE_INTEGER) ? TYPED_ELEM_INT64 : TYPED_ELEM_FLOAT64;
        unsigned int width = vm_typed_elem_width(kind);
        AerTypedArray* ta = heap_alloc(&vm->heap, &vm->heap.typed_array_pool);
        ta->count = (unsigned int)count;
        ta->elem_kind = kind;
        ta->data = count > 0 ? typed_array_data_alloc(&vm->heap, (size_t)count * width) : NULL;
        for (int64_t e = 0; e < count; e++)
            vm_typed_elem_write(ta->data + (size_t)e * width, kind, fill);
        registers[dest_reg] = aer_typed_array_val(ta);
    } else if (aer_type(fill) == TYPE_TYPED_ARRAY) {
        /* `[[0.0; cols]; rows]`. Each row is its own COPY of the fill, never one array referenced
           `count` times -- sharing it would make `m[0][0] = x` write every row, which is the trap
           Python's `[[0] * cols] * rows` is famous for. Nothing here collects, so the rows are safe
           to build before the outer array roots them (gc_maybe_collect runs after). */
        AerTypedArray* src = aer_as_typed_array(fill);
        unsigned int width = vm_typed_elem_width(src->elem_kind);
        AerArray* rows = heap_new_array(&vm->heap, count > 0 ? (unsigned int)count : 4);
        rows->count = (unsigned int)count;
        for (int64_t e = 0; e < count; e++) {
            AerTypedArray* row = vm_new_typed_array(src->elem_kind, src->count);
            if (src->count > 0)
                memcpy(row->data, src->data, (size_t)src->count * width);
            rows->items[e] = aer_typed_array_val(row);
        }
        registers[dest_reg] = aer_array_val(rows);
    } else {
        error("Cannot build a repeat-literal array from a %s value -- the fill value must be a "
              "number, a boolean, a struct instance, or a typed array",
              vm_type_name(c, fill));
        DISPATCH();
    }
    gc_maybe_collect(vm);
    DISPATCH();
}

/* Handles both a packed array and an ordinary struct array (the parser can't know which --
   functions are untyped). The non-packed branch reproduces the plain index+field path exactly,
   just without needing a scratch register for the intermediate. */
SEPARATE_HANDLER(index_field_get_any_container)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int dest_reg = (int)UNPACK_A(op_word);
    int obj_reg = (int)UNPACK_B(op_word);
    uint32_t field_rk_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerVal obj = registers[obj_reg];
    if (aer_type(obj) == TYPE_PACKED_ARRAY) {
        AerPackedArray* pa = aer_as_packed_array(obj);
        uint64_t i;
        if (!vm_array_position(*idx, pa->count, &i)) {
            vm_array_index_error(*idx, pa->count);
            DISPATCH();
        }
        int slot;
        unsigned int foffset;
        ValueType ftype;
        bool narrow;
        if (!vm_resolve_field_by_shape(c, site, pa->shape, field_idx, &slot, &foffset, &ftype, &narrow))
            DISPATCH();
        unsigned int element_size = pa->shape->instance_bytes;
        unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
        registers[dest_reg] =
            narrow ? vm_narrow_field_read(elem, ftype)
                   : vm_packed_slot_read(elem, ftype);
        DISPATCH();
    }
    AerVal tmp;
    vm_index_get_compute(obj, *idx, &tmp);
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm); /* single-char string indexing allocates */
    if (aer_type(tmp) != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerStruct* oa = aer_as_struct(tmp);
    int slot;
    unsigned int foffset;
    ValueType ftype;
    bool narrow;
    if (!vm_resolve_field_by_shape(c, site, oa->shape, field_idx, &slot, &foffset, &ftype, &narrow))
        DISPATCH();
    registers[dest_reg] = vm_struct_field_read_at(oa, foffset, ftype, narrow);
    DISPATCH();
}

HANDLER(index_field_get)
    AerVal obj = registers[(int)UNPACK_B(op_word)];
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(pc[0]));
    FieldCacheEntry* entry = &c->field_cache[(unsigned int)(pc - c->code) - 1];
    if (aer_type(obj) != TYPE_PACKED_ARRAY || idx->tag != TYPE_INTEGER)
        __attribute__((musttail)) return h_index_field_get_any_container(vm, pc, registers, c);
    AerPackedArray* pa = aer_as_packed_array(obj);
    if ((uint64_t)idx->as.i >= pa->count || entry->shape != pa->shape)
        __attribute__((musttail)) return h_index_field_get_any_container(vm, pc, registers, c);
    (void)READ();
    unsigned char* elem = pa->data + (size_t)idx->as.i * pa->shape->instance_bytes + entry->offset;
    registers[(int)UNPACK_A(op_word)] =
        entry->narrow
            ? vm_narrow_field_read(elem, entry->ftype)
            : vm_packed_slot_read(elem, entry->ftype);
    DISPATCH();
}

/* Mirror of h_index_field_get -- same dual dispatch, same reason no scratch register is needed. */
SEPARATE_HANDLER(index_field_set_any_container)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int obj_reg = (int)UNPACK_A(op_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_W16(op_word));
    uint32_t field_val_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_val_word);
    AerVal* val = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_val_word));
    AerVal obj = registers[obj_reg];
    if (aer_type(obj) == TYPE_PACKED_ARRAY) {
        AerPackedArray* pa = aer_as_packed_array(obj);
        uint64_t i;
        if (!vm_array_position(*idx, pa->count, &i)) {
            vm_array_index_error(*idx, pa->count);
            DISPATCH();
        }
        int slot;
        unsigned int foffset;
        ValueType declared;
        bool narrow;
        if (!vm_resolve_field_by_shape(c, site, pa->shape, field_idx, &slot, &foffset, &declared, &narrow))
            DISPATCH();
        if (val->tag != declared) {
            error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
                  aer_as_string(c->pool[field_idx])->data);
            DISPATCH();
        }
        if (!vm_check_narrow_field_write(declared, narrow, *val))
            DISPATCH();
        unsigned int element_size = pa->shape->instance_bytes;
        unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
        if (narrow)
            vm_narrow_field_write(elem, declared, *val);
        else
            vm_packed_slot_write(elem, declared, *val);
        DISPATCH();
    }
    AerVal tmp;
    vm_index_get_compute(obj, *idx, &tmp);
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm); /* single-char string indexing allocates */
    if (aer_type(tmp) != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerStruct* oa = aer_as_struct(tmp);
    int slot;
    unsigned int foffset;
    ValueType declared;
    bool narrow;
    if (!vm_resolve_field_by_shape(c, site, oa->shape, field_idx, &slot, &foffset, &declared, &narrow))
        DISPATCH();
    if (declared != TYPE_ANY && val->tag != declared) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    if (!vm_check_narrow_field_write(declared, narrow, *val))
        DISPATCH();
    gc_barrier_struct(vm, oa, *val);
    vm_struct_field_write_at(oa, foffset, declared, narrow, *val);
    DISPATCH();
}

HANDLER(index_field_set)
    AerVal obj = registers[(int)UNPACK_A(op_word)];
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_W16(op_word));
    AerVal* val = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(pc[0]));
    FieldCacheEntry* entry = &c->field_cache[(unsigned int)(pc - c->code) - 1];
    if (aer_type(obj) != TYPE_PACKED_ARRAY || idx->tag != TYPE_INTEGER)
        __attribute__((musttail)) return h_index_field_set_any_container(vm, pc, registers, c);
    AerPackedArray* pa = aer_as_packed_array(obj);
    if ((uint64_t)idx->as.i >= pa->count || entry->shape != pa->shape || val->tag != entry->ftype ||
        !vm_fits_narrow_field(entry->ftype, entry->narrow, *val))
        __attribute__((musttail)) return h_index_field_set_any_container(vm, pc, registers, c);
    (void)READ();
    unsigned char* elem = pa->data + (size_t)idx->as.i * pa->shape->instance_bytes + entry->offset;
    if (entry->narrow)
        vm_narrow_field_write(elem, entry->ftype, *val);
    else
        vm_packed_slot_write(elem, entry->ftype, *val);
    DISPATCH();
}

/* `obj[index].field OP= rk_rhs` -- resolves the index+field exactly once (same dual packed-array/
   struct-instance dispatch as h_index_field_get/set), reads, computes, type-checks, and writes
   back in one dispatch. One resolution, not the two a separate read and write would each pay --
   the nbody-style `bodies[j].vx += dx * mi` pattern. */
HANDLER(index_field_compound)
    unsigned int site = (unsigned int)(pc - c->code) - 1;
    int obj_reg = (int)UNPACK_A(op_word);
    Opcode bin_op = (Opcode)UNPACK_B(op_word);
    uint32_t field_idx_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_idx_word);
    AerVal* idx = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(field_idx_word));
    uint32_t rhs_word = READ();
    AerVal* rhs = vm_rk_ptr16(registers, const_pool, UNPACK_2X16_LO(rhs_word));
    AerVal obj = registers[obj_reg];
    if (aer_type(obj) == TYPE_PACKED_ARRAY) {
        AerPackedArray* pa = aer_as_packed_array(obj);
        uint64_t i;
        if (!vm_array_position(*idx, pa->count, &i)) {
            vm_array_index_error(*idx, pa->count);
            DISPATCH();
        }
        int slot;
        unsigned int foffset;
        ValueType ftype;
        bool narrow;
        if (!vm_resolve_field_by_shape(c, site, pa->shape, field_idx, &slot, &foffset, &ftype, &narrow))
            DISPATCH();
        unsigned int element_size = pa->shape->instance_bytes;
        unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
        AerVal lhs =
            narrow ? vm_narrow_field_read(elem, ftype)
                   : vm_packed_slot_read(elem, ftype);
        bool handled;
        AerVal result = vm_binary_fast(lhs, *rhs, bin_op, ftype, aer_type(*rhs), &handled);
        if (!handled) {
            result = vm_binary_cold(c, lhs, *rhs, bin_op, ftype, aer_type(*rhs));
            gc_maybe_collect(vm);
        }
        if (result.tag != ftype) {
            error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
                  aer_as_string(c->pool[field_idx])->data);
            DISPATCH();
        }
        if (!vm_check_narrow_field_write(ftype, narrow, result))
            DISPATCH();
        if (narrow)
            vm_narrow_field_write(elem, ftype, result);
        else
            vm_packed_slot_write(elem, ftype, result);
        DISPATCH();
    }
    AerVal tmp;
    vm_index_get_compute(obj, *idx, &tmp);
    if (aer_type(obj) == TYPE_STRING)
        gc_maybe_collect(vm); /* single-char string indexing allocates */
    if (aer_type(tmp) != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerStruct* oa = aer_as_struct(tmp);
    int slot;
    unsigned int foffset;
    ValueType ftype;
    bool narrow;
    if (!vm_resolve_field_by_shape(c, site, oa->shape, field_idx, &slot, &foffset, &ftype, &narrow))
        DISPATCH();
    AerVal lhs = vm_struct_field_read_at(oa, foffset, ftype, narrow);
    ValueType ta = aer_type(lhs), tb = aer_type(*rhs);
    bool handled;
    AerVal result = vm_binary_fast(lhs, *rhs, bin_op, ta, tb, &handled);
    if (!handled) {
        result = vm_binary_cold(c, lhs, *rhs, bin_op, ta, tb);
        gc_maybe_collect(vm);
    }
    if (ftype != TYPE_ANY && result.tag != ftype) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    if (!vm_check_narrow_field_write(ftype, narrow, result))
        DISPATCH();
    gc_barrier_struct(vm, oa, result);
    vm_struct_field_write_at(oa, foffset, ftype, narrow, result);
    DISPATCH();
}

/* Keyed by unary_op, same tag convention as h_binary. Also folds in OP_TO_STR
   (interpolation's string conversion) via the shared vm_to_str(). */
HANDLER(interp)
    int dest = (int)UNPACK_A(op_word);
    unsigned int count = UNPACK_B(op_word);
    AerVal parts[INTERP_MAX_PARTS];
    for (unsigned int i = 0; i < count; i++)
        parts[i] = *vm_rk_ptr16(registers, const_pool, READ());
    registers[dest] = vm_interp_build(vm, parts, count);
    gc_maybe_collect(vm);
    DISPATCH();
}

HANDLER(unary)
    int dest = (int)UNPACK_A(op_word);
    Opcode unary_op = (Opcode)UNPACK_B(op_word);
    AerVal v = *vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    AerVal* result = &registers[dest];
    switch (unary_op) {
        case OP_NEGATE:
            if (aer_type(v) == TYPE_INTEGER)
                *result = aer_int(-aer_as_int(v));
            else if (aer_type(v) == TYPE_REAL)
                *result = aer_real(-aer_as_real(v));
            else {
                error("Negation requires a numeric type");
                *result = aer_null();
            }
            break;
        case OP_NOT: *result = aer_bool(!vm_truthy(v)); break;
        case OP_BITWISE_NOT:
            if (aer_type(v) != TYPE_INTEGER) {
                error("Bitwise NOT requires an integer");
                *result = aer_null();
            } else
                *result = aer_int(~aer_as_int(v));
            break;
        case OP_TO_STR:
            /* The only allocating case -- NEGATE/NOT/BITWISE_NOT only ever produce plain tagged-union
               values. */
            *result = vm_to_str(vm, v);
            gc_maybe_collect(vm);
            break;
        default: *result = aer_null(); break;
    }
    DISPATCH();
}

/* No gc_maybe_collect -- every cast_type only ever produces a plain tagged-union value
   (confirmed by inspection, including vm_cast's string-parsing sub-cases). */
HANDLER(cast)
    int dest = (int)UNPACK_A(op_word);
    int cast_type = (int)UNPACK_B(op_word);
    AerVal v = *vm_rk_ptr8(registers, const_pool, UNPACK_C(op_word));
    registers[dest] = vm_cast(v, cast_type);
    DISPATCH();
}

HANDLER(raw_int_to_real)
    registers[UNPACK_A(op_word)] = aer_real((double)registers[UNPACK_B(op_word)].as.i);
    DISPATCH();
}

HANDLER(raw_real_to_int)
    registers[UNPACK_A(op_word)] = aer_int((int64_t)registers[UNPACK_B(op_word)].as.d);
    DISPATCH();
}

/* Skip both the RK-flag check and the tag check -- the parser already proved every operand's
   type at compile time. None allocate: integers/reals/booleans never have heap cells. */
HANDLER(raw_load_int)
    int dest = (int)UNPACK_A(op_word);
    /* Full 32-bit signed immediate in its own dedicated word, so no literal reaching here can
       truncate -- a narrower field silently turned a 20M-iteration bound into 77056. */
    registers[dest] = aer_int((int32_t)READ());
    DISPATCH();
}

HANDLER(raw_load_real)
    int dest = (int)UNPACK_A(op_word);
    unsigned int idx = (unsigned int)READ();
    registers[dest] = aer_real(c->rawk_d[idx]);
    DISPATCH();
}

/* Source-level dedup only: each invocation still expands to its own full label and body. NOT a
   shared noinline function, which would put a call in the dispatch loop for a code-size win these
   workloads do not need. #undef'd after the last family that uses them.

   RAW_I/RAW_D: the RK8 const flag picks the raw constant table, so a literal needs no slot. A slot
   index is at most FRAME_REGISTERS-1 (127) and the flag is 0x80, so the two never collide. */
#define RAW_I(x) (RK8_IS_CONST(x) ? c->rawk_i[RK8_INDEX(x)] : registers[x].as.i)
#define RAW_D(x) (RK8_IS_CONST(x) ? c->rawk_d[RK8_INDEX(x)] : registers[x].as.d)

#define RAW_ARITH_INT(name, op)                                                                              \
    static VmSliceResult h_raw_##name##_int(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {       \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        int dest = (int)UNPACK_A(op_word);                                                                   \
        int a = (int)UNPACK_B(op_word);                                                                      \
        unsigned int b = UNPACK_C(op_word);                                                                  \
        registers[dest] = aer_int(registers[a].as.i op registers[b].as.i);                                   \
        DISPATCH();                                                                                          \
    }
/* The C field is a bare rawk_i index, not an RK -- the opcode itself already says "constant", so
   all 8 bits are index and no flag needs testing. */
#define RAW_ARITH_INT_K(name, op)                                                                            \
    static VmSliceResult h_raw_##name##_int_k(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {     \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        int dest = (int)UNPACK_A(op_word);                                                                   \
        int a = (int)UNPACK_B(op_word);                                                                      \
        unsigned int k = UNPACK_C(op_word);                                                                  \
        registers[dest] = aer_int(registers[a].as.i op c->rawk_i[k]);                                        \
        DISPATCH();                                                                                          \
    }
/* Writes the payload and leaves the tag: the destination is a real slot, whose tag frame entry
   already set and nothing since can have changed (frame_init_tags, vm.h). One 8-byte store instead
   of a 16-byte one, on the instruction a numeric loop spends most of its dispatches in. */
#define RAW_ARITH_REAL(name, op)                                                                             \
    static VmSliceResult h_raw_##name##_real(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {      \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        int dest = (int)UNPACK_A(op_word);                                                                   \
        int a = (int)UNPACK_B(op_word);                                                                      \
        unsigned int b = UNPACK_C(op_word);                                                                  \
        registers[dest].as.d = registers[a].as.d op registers[b].as.d;                                       \
        DISPATCH();                                                                                          \
    }
#define RAW_CMP_INT(name, op)                                                                                \
    static VmSliceResult h_raw_##name##_int(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {       \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        int dest = (int)UNPACK_A(op_word);                                                                   \
        int a = (int)UNPACK_B(op_word);                                                                      \
        unsigned int b = UNPACK_C(op_word);                                                                  \
        registers[dest] = aer_bool(registers[a].as.i op RAW_I(b));                                           \
        DISPATCH();                                                                                          \
    }
#define RAW_CMP_REAL(name, op)                                                                               \
    static VmSliceResult h_raw_##name##_real(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {      \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        int dest = (int)UNPACK_A(op_word);                                                                   \
        int a = (int)UNPACK_B(op_word);                                                                      \
        unsigned int b = UNPACK_C(op_word);                                                                  \
        registers[dest] = aer_bool(registers[a].as.d op RAW_D(b));                                           \
        DISPATCH();                                                                                          \
    }
    RAW_ARITH_INT(add, +)
    RAW_ARITH_INT(sub, -)
    RAW_ARITH_INT(mul, *)
    RAW_ARITH_INT_K(add, +)
    RAW_ARITH_INT_K(sub, -)

/* Matches OP_DIV's own semantics: int/int division always promotes to float, so this is the one
   OP_RAW_*_INT opcode whose dest is registers[].as.d, not registers[].as.i. */
HANDLER(raw_div_int)
    int dest = (int)UNPACK_A(op_word);
    int a = (int)UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    int64_t rv = registers[b].as.i;
    if (rv == 0) {
        error("Division by zero");
        registers[dest] = aer_real(0.0);
    } else
        registers[dest] = aer_real((double)registers[a].as.i / (double)rv);
    DISPATCH();
}

HANDLER(raw_mod_int)
    int dest = (int)UNPACK_A(op_word);
    int a = (int)UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    int64_t rv = registers[b].as.i;
    if (rv == 0) {
        error("Modulo by zero");
        registers[dest] = aer_int(0);
    } else
        registers[dest] = aer_int(aer_mod_int64(registers[a].as.i, rv));
    DISPATCH();
}

HANDLER(raw_floor_div_int)
    int dest = (int)UNPACK_A(op_word);
    int a = (int)UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    int64_t rv = registers[b].as.i;
    if (rv == 0) {
        error("Division by zero");
        registers[dest] = aer_int(0);
    } else
        registers[dest] = aer_int((int64_t)floor((double)registers[a].as.i / (double)rv));
    DISPATCH();
}

    RAW_ARITH_REAL(add, +)
    RAW_ARITH_REAL(sub, -)
    RAW_ARITH_REAL(mul, *)

/* Superinstruction for `x += a*b` / `x -= a*b` on raw real locals -- see OP_RAW_FMA_REAL's own
   comment (vm.h) for what emits this and why it's still bit-identical to the unfused two-opcode
   form (two roundings, not a hardware single-rounding FMA). dest doubles as the accumulator's
   read source and write destination, same in-place convention RAW_ARITH_REAL's own dest==a
   callers already rely on. */
#define RAW_FUSED_MULACC_REAL(name, op)                                                                      \
    static VmSliceResult h_raw_##name##_real(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {      \
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        int dest = (int)UNPACK_A(op_word);                                                                   \
        int a = (int)UNPACK_B(op_word);                                                                      \
        unsigned int b = UNPACK_C(op_word);                                                                  \
        registers[dest].as.d = registers[dest].as.d op(registers[a].as.d * registers[b].as.d);               \
        DISPATCH();                                                                                          \
    }
    RAW_FUSED_MULACC_REAL(fma, +)
    RAW_FUSED_MULACC_REAL(fms, -)
#undef RAW_FUSED_MULACC_REAL

HANDLER(raw_div_real)
    int dest = (int)UNPACK_A(op_word);
    int a = (int)UNPACK_B(op_word);
    unsigned int b = UNPACK_C(op_word);
    double rv = registers[b].as.d;
    if (rv == 0.0) {
        error("Division by zero");
        registers[dest] = aer_real(0.0);
    } else
        registers[dest] = aer_real(registers[a].as.d / rv);
    DISPATCH();
}

    /* Comparisons produce a boxed boolean (no raw boolean type exists) -- dest is 7 bits, not 5. */
    RAW_CMP_INT(lt, <)
    RAW_CMP_INT(lte, <=)
    RAW_CMP_INT(eq, ==)
    RAW_CMP_INT(neq, !=)
    RAW_CMP_REAL(lt, <)
    RAW_CMP_REAL(lte, <=)
    RAW_CMP_REAL(eq, ==)
    RAW_CMP_REAL(neq, !=)

/* Checks a slot whose type nothing proved, so every opcode after it can skip the check. The error
   wording names the operator, not "unbox", because that is what the source line reads as. */
HANDLER(unbox_int)
    int dest = (int)UNPACK_A(op_word);
    AerVal* v = &registers[UNPACK_B(op_word)];
    if (v->tag == TYPE_INTEGER)
        registers[dest] = aer_int(v->as.i);
    else
        error("Cannot apply this operator to integer and %s", vm_type_name(c, *v));
    DISPATCH();
}

HANDLER(unbox_real)
    int dest = (int)UNPACK_A(op_word);
    AerVal* v = &registers[UNPACK_B(op_word)];
    if (v->tag == TYPE_REAL)
        registers[dest] = aer_real(v->as.d);
    else if (v->tag == TYPE_INTEGER)
        registers[dest] = aer_real((double)v->as.i);
    else
        error("Cannot apply this operator to float and %s", vm_type_name(c, *v));
    DISPATCH();
}

HANDLER(raw_move_int)
    int dest = (int)UNPACK_A(op_word);
    int src = (int)UNPACK_B(op_word);
    registers[dest] = aer_int(registers[src].as.i);
    DISPATCH();
}

HANDLER(raw_move_real)
    int dest = (int)UNPACK_A(op_word);
    int src = (int)UNPACK_B(op_word);
    registers[dest] = aer_real(registers[src].as.d);
    DISPATCH();
}

#undef RAW_ARITH_INT
#undef RAW_ARITH_REAL
#undef RAW_CMP_INT
#undef RAW_CMP_REAL

HANDLER(raw_load_int_pool)
    int dest = (int)UNPACK_A(op_word);
    unsigned int idx = (unsigned int)READ();
    registers[dest] = aer_int(c->rawk_i[idx]);
    DISPATCH();
}

/* Both operands raw: no tag, no table, no error path -- the comparison is the two loads the
   hardware would do anyway. */
#define RAW_CMP_JUMP_IF_FALSE(name, member, rhs, op)                                                         \
    static VmSliceResult h_raw_##name##_jump_if_false(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {\
        const uint32_t op_word = pc[-1];                                                                     \
        (void)op_word;                                                                                       \
        int a = (int)UNPACK_B(op_word);                                                                      \
        unsigned int b = UNPACK_C(op_word);                                                                  \
        int target = READ();                                                                                 \
        if (!(registers[a].as.member op rhs(b)))                                                             \
            pc += (int32_t)target;                                                                           \
        DISPATCH();                                                                                          \
    }

    RAW_CMP_JUMP_IF_FALSE(lt_int, i, RAW_I, <)
    RAW_CMP_JUMP_IF_FALSE(lte_int, i, RAW_I, <=)
    RAW_CMP_JUMP_IF_FALSE(eq_int, i, RAW_I, ==)
    RAW_CMP_JUMP_IF_FALSE(neq_int, i, RAW_I, !=)
    RAW_CMP_JUMP_IF_FALSE(lt_real, d, RAW_D, <)
    RAW_CMP_JUMP_IF_FALSE(lte_real, d, RAW_D, <=)
    RAW_CMP_JUMP_IF_FALSE(eq_real, d, RAW_D, ==)
    RAW_CMP_JUMP_IF_FALSE(neq_real, d, RAW_D, !=)

#undef RAW_CMP_JUMP_IF_FALSE

/* The operand words are read by the helpers straight out of the instruction stream rather than
   into a local array. Both helpers read every part before dest is written, so dest may alias a
   part's register, which emit_interp deliberately arranges. */
HANDLER(index_get_interp)
    int dest_reg = (int)UNPACK_A(op_word);
    int obj_reg = (int)UNPACK_B(op_word);
    unsigned int count = UNPACK_C(op_word);
    const uint32_t* rks = pc;
    pc += count;
    AerVal obj = registers[obj_reg];
    if (aer_type(obj) == TYPE_DICT &&
        vm_dict_get_interp(aer_as_dict(obj), rks, count, registers, const_pool, &registers[dest_reg]))
        DISPATCH();
    registers[dest_reg] = vm_index_get_interp_slow(vm, obj, rks, count, registers, const_pool);
    gc_maybe_collect(vm);
    DISPATCH();
}

/* The only handler that ends the chain rather than continuing it, so it is the only one that
   touches none of the four. */
static VmSliceResult h_halt(VM* vm, const uint32_t* pc, AerVal* registers, Chunk* c) {
    (void)vm;
    (void)pc;
    (void)registers;
    (void)c;
    return VM_SLICE_DONE;
}

#undef SYNC_IP
#undef error
#undef vm_binary_cold
#undef vm_index_get_compute
#undef vm_index_set_compute
#undef vm_array_index_error
#undef vm_in
#undef vm_cast
#undef vm_to_str
#undef vm_slice_bounds
#undef vm_call_builtin
#undef vm_call_value
#undef vm_call_module_dispatch
#undef vm_call_resolve_specialization
#undef vm_resolve_field
#undef vm_resolve_field_by_shape
#undef vm_check_narrow_field_write
#undef vm_typed_array_check
#undef vm_default_value
#undef aer_make_string_copy
#undef gc_maybe_collect
#undef HANDLER
#undef READ
#undef PUSH
#undef POP
#undef DISPATCH
#undef AER_PROFILE_HIT
#undef const_pool

static const OpHandler aer_handlers[256] = {
        /* Unary ops have no entries -- only ever embedded as a tag inside OP_UNARY. OP_ADD..OP_IN ARE
           real top-level dispatch targets (true single-level dispatch, PACK_BINARY). */
        [OP_ADD] = h_add,
        [OP_SUB] = h_sub,
        [OP_MUL] = h_mul,
        [OP_DIV] = h_div,
        [OP_MOD] = h_mod,
        [OP_FLOOR_DIV] = h_floor_div,
        [OP_EQ] = h_eq,
        [OP_NEQ] = h_neq,
        [OP_LT] = h_lt,
        [OP_GT] = h_gt,
        [OP_LTE] = h_lte,
        [OP_GTE] = h_gte,
        [OP_IN] = h_in,
        [OP_BITWISE_AND] = h_bitwise_and,
        [OP_BITWISE_OR] = h_bitwise_or,
        [OP_BITWISE_XOR] = h_bitwise_xor,
        [OP_LSHIFT] = h_lshift,
        [OP_RSHIFT] = h_rshift,
        [OP_JUMP] = h_jump,
        [OP_DEFINE_STRUCT] = h_define_struct,
        [OP_HALT] = h_halt,
        [OP_LOADK] = h_loadk,
        [OP_MOVE] = h_move,
        [OP_IS_RESULT] = h_is_result,
        [OP_JUMP_IF_FALSE_REG] = h_jump_if_false_reg,
        [OP_CALL] = h_call,
        [OP_CALL_VALUE] = h_call_value,
        [OP_TAIL_CALL] = h_tail_call,
        [OP_TAIL_CALL_VALUE] = h_call_value,
        [OP_CALL_MODULE] = h_call_module,
        [OP_CALL_BUILTIN] = h_call_builtin,
        [OP_RETURN] = h_return,
        [OP_ARRAY_NEW] = h_array_new,
        [OP_INDEX_GET] = h_index_get,
        [OP_INDEX_SET] = h_index_set,
        [OP_TYPED_INDEX_GET_UNCHECKED] = h_typed_index_get_unchecked,
        [OP_TYPED_INDEX_SET_UNCHECKED] = h_typed_index_set_unchecked,
        [OP_INDEX_GET_RAW_INT] = h_index_get_raw_int,
        [OP_INDEX_SET_RAW_INT] = h_index_set_raw_int,
        [OP_INDEX_SET_RAW_REAL] = h_index_set_raw_real,
        [OP_INDEX_GET_RAW_REAL] = h_index_get_raw_real,
        [OP_INDEX_COMPOUND_RAW_INT] = h_index_compound_raw_int,
        [OP_INDEX_COMPOUND_RAW_REAL] = h_index_compound_raw_real,
        [OP_CALL_SELF] = h_call_self,
        [OP_TAIL_CALL_SELF] = h_tail_call_self,
        [OP_RAW_MATH_REAL] = h_raw_math_real,
        [OP_RAW_INT_TO_REAL] = h_raw_int_to_real,
        [OP_RAW_REAL_TO_INT] = h_raw_real_to_int,
        [OP_DESTRUCTURE] = h_destructure,
        [OP_SLICE_GET] = h_slice_get,
        [OP_DICT_NEW] = h_dict_new,
        [OP_ITER_NEXT_ARRAY] = h_iter_next_array,
        [OP_ITER_NEXT_PAIR] = h_iter_next_pair,
        [OP_ITER_RANGE_PREP] = h_iter_range_prep,
        [OP_ITER_RANGE_LOOP] = h_iter_range_loop,
        [OP_STRUCT_NEW] = h_struct_new,
        [OP_FIELD_GET] = h_field_get,
        [OP_FIELD_SET] = h_field_set,
        [OP_ARRAY_REPEAT] = h_array_repeat,
        [OP_INDEX_FIELD_GET] = h_index_field_get,
        [OP_INDEX_FIELD_SET] = h_index_field_set,
        [OP_INDEX_FIELD_COMPOUND] = h_index_field_compound,
        [OP_UNARY] = h_unary,
        [OP_CAST] = h_cast,
        [OP_FIELD_BINARY] = h_field_binary,
        [OP_FIELD_COMPOUND] = h_field_compound,
        [OP_INDEX_COMPOUND] = h_index_compound,
        [OP_TYPED_ARRAY_CHAIN2] = h_typed_array_chain2,
        [OP_PRINT_REPL] = h_print_repl,

        /* Raw-arithmetic family -- see the h_raw_* labels below for why no vm_rk_ptr8/tag-check
           is needed. */
        [OP_RAW_LOAD_INT] = h_raw_load_int,
        [OP_RAW_LOAD_REAL] = h_raw_load_real,
        [OP_RAW_ADD_INT] = h_raw_add_int,
        [OP_RAW_SUB_INT] = h_raw_sub_int,
        [OP_RAW_ADD_INT_K] = h_raw_add_int_k,
        [OP_RAW_SUB_INT_K] = h_raw_sub_int_k,
        [OP_RAW_MUL_INT] = h_raw_mul_int,
        [OP_RAW_DIV_INT] = h_raw_div_int,
        [OP_RAW_MOD_INT] = h_raw_mod_int,
        [OP_RAW_FLOOR_DIV_INT] = h_raw_floor_div_int,
        [OP_RAW_ADD_REAL] = h_raw_add_real,
        [OP_RAW_SUB_REAL] = h_raw_sub_real,
        [OP_RAW_MUL_REAL] = h_raw_mul_real,
        [OP_RAW_DIV_REAL] = h_raw_div_real,
        [OP_RAW_FMA_REAL] = h_raw_fma_real,
        [OP_RAW_FMS_REAL] = h_raw_fms_real,
        [OP_RAW_LT_INT] = h_raw_lt_int,
        [OP_RAW_LTE_INT] = h_raw_lte_int,
        [OP_RAW_LT_REAL] = h_raw_lt_real,
        [OP_RAW_LTE_REAL] = h_raw_lte_real,
        [OP_RAW_EQ_INT] = h_raw_eq_int,
        [OP_RAW_NEQ_INT] = h_raw_neq_int,
        [OP_RAW_EQ_REAL] = h_raw_eq_real,
        [OP_RAW_NEQ_REAL] = h_raw_neq_real,
        [OP_UNBOX_INT] = h_unbox_int,
        [OP_UNBOX_REAL] = h_unbox_real,
        [OP_RAW_MOVE_INT] = h_raw_move_int,
        [OP_RAW_MOVE_REAL] = h_raw_move_real,
        [OP_RAW_LOAD_INT_POOL] = h_raw_load_int_pool,

        [OP_INDEX_FIELD_GET_RAW_INT] = h_index_field_get_raw_int,
        [OP_INDEX_FIELD_GET_RAW_REAL] = h_index_field_get_raw_real,
        [OP_FIELD_GET_RAW_INT] = h_field_get_raw_int,
        [OP_FIELD_GET_RAW_REAL] = h_field_get_raw_real,
        [OP_INDEX_FIELD_SET_RAW_INT] = h_index_field_set_raw_int,
        [OP_INDEX_FIELD_SET_RAW_REAL] = h_index_field_set_raw_real,
        [OP_FIELD_SET_RAW_INT] = h_field_set_raw_int,
        [OP_FIELD_SET_RAW_REAL] = h_field_set_raw_real,
        [OP_FIELD_COMPOUND_RAW_INT] = h_field_compound_raw_int,
        [OP_FIELD_COMPOUND_RAW_REAL] = h_field_compound_raw_real,
        [OP_INDEX_FIELD_COMPOUND_RAW_INT] = h_index_field_compound_raw_int,
        [OP_INDEX_FIELD_COMPOUND_RAW_REAL] = h_index_field_compound_raw_real,

        [OP_INDEX_FIELD_GET_RAW_INT_UNCHECKED] = h_index_field_get_raw_int_unchecked,
        [OP_INDEX_FIELD_GET_RAW_REAL_UNCHECKED] = h_index_field_get_raw_real_unchecked,
        [OP_INDEX_FIELD_SET_RAW_INT_UNCHECKED] = h_index_field_set_raw_int_unchecked,
        [OP_INDEX_FIELD_SET_RAW_REAL_UNCHECKED] = h_index_field_set_raw_real_unchecked,
        [OP_INDEX_FIELD_COMPOUND_RAW_INT_UNCHECKED] = h_index_field_compound_raw_int_unchecked,
        [OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED] = h_index_field_compound_raw_real_unchecked,

        [OP_INDEX_FIELD_GET_RAW_INT32] = h_index_field_get_raw_int32,
        [OP_INDEX_FIELD_GET_RAW_FLOAT32] = h_index_field_get_raw_float32,
        [OP_FIELD_GET_RAW_INT32] = h_field_get_raw_int32,
        [OP_FIELD_GET_RAW_FLOAT32] = h_field_get_raw_float32,
        [OP_INDEX_FIELD_SET_RAW_INT32] = h_index_field_set_raw_int32,
        [OP_INDEX_FIELD_SET_RAW_FLOAT32] = h_index_field_set_raw_float32,
        [OP_FIELD_SET_RAW_INT32] = h_field_set_raw_int32,
        [OP_FIELD_SET_RAW_FLOAT32] = h_field_set_raw_float32,
        [OP_FIELD_COMPOUND_RAW_INT32] = h_field_compound_raw_int32,
        [OP_FIELD_COMPOUND_RAW_FLOAT32] = h_field_compound_raw_float32,
        [OP_INDEX_FIELD_COMPOUND_RAW_INT32] = h_index_field_compound_raw_int32,
        [OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32] = h_index_field_compound_raw_float32,

        [OP_INDEX_FIELD_GET_RAW_INT32_UNCHECKED] = h_index_field_get_raw_int32_unchecked,
        [OP_INDEX_FIELD_GET_RAW_FLOAT32_UNCHECKED] = h_index_field_get_raw_float32_unchecked,
        [OP_INDEX_FIELD_SET_RAW_INT32_UNCHECKED] = h_index_field_set_raw_int32_unchecked,
        [OP_INDEX_FIELD_SET_RAW_FLOAT32_UNCHECKED] = h_index_field_set_raw_float32_unchecked,
        [OP_INDEX_FIELD_COMPOUND_RAW_INT32_UNCHECKED] = h_index_field_compound_raw_int32_unchecked,
        [OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED] = h_index_field_compound_raw_float32_unchecked,
        [OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED_ADD] = h_index_field_compound_raw_real_unchecked_add,
        [OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED_ADD] = h_index_field_compound_raw_float32_unchecked_add,
        [OP_FIELD_COMPOUND_RAW_FLOAT32_FMA] = h_field_compound_raw_float32_fma,
        [OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED_FMA] =
            h_index_field_compound_raw_real_unchecked_fma,

        [OP_EQ_JUMP_IF_FALSE] = h_eq_jump_if_false,
        [OP_NEQ_JUMP_IF_FALSE] = h_neq_jump_if_false,
        [OP_LT_JUMP_IF_FALSE] = h_lt_jump_if_false,
        [OP_GT_JUMP_IF_FALSE] = h_gt_jump_if_false,
        [OP_LTE_JUMP_IF_FALSE] = h_lte_jump_if_false,
        [OP_GTE_JUMP_IF_FALSE] = h_gte_jump_if_false,

        [OP_RAW_LT_INT_JUMP_IF_FALSE] = h_raw_lt_int_jump_if_false,
        [OP_RAW_LTE_INT_JUMP_IF_FALSE] = h_raw_lte_int_jump_if_false,
        [OP_RAW_LT_REAL_JUMP_IF_FALSE] = h_raw_lt_real_jump_if_false,
        [OP_RAW_LTE_REAL_JUMP_IF_FALSE] = h_raw_lte_real_jump_if_false,
        [OP_RAW_EQ_INT_JUMP_IF_FALSE] = h_raw_eq_int_jump_if_false,
        [OP_RAW_NEQ_INT_JUMP_IF_FALSE] = h_raw_neq_int_jump_if_false,
        [OP_RAW_EQ_REAL_JUMP_IF_FALSE] = h_raw_eq_real_jump_if_false,
        [OP_RAW_NEQ_REAL_JUMP_IF_FALSE] = h_raw_neq_real_jump_if_false,
        [OP_INTERP] = h_interp,
        [OP_INDEX_GET_INTERP] = h_index_get_interp,
};

VmSliceResult vm_run_slice(VM* vm, unsigned int max_instructions) {
    Chunk* c = vm->chunk;
    /* Hoisted once -- c->pool is only mutated at parse time, stable for the whole call. */
    /* Installs this call's error catch point, saving the previous one so nested vm_run calls
       catch their own errors and unwind no further than here; restored on every return. */
    AerJmpBuf catch_point;
    AerJmpBuf* saved_unwind_target = runtime_error_unwind_target;
    runtime_error_unwind_target = &catch_point;
    /* One store here + one restore at each exit, instead of every DISPATCH() -- the VM never
       changes mid-call. */
    VM* saved_active_vm = vm_active_error_vm();
    vm_set_active_error_vm(vm);
    /* Same save/restore shape as vm_active_error_vm() just above -- a nested vm_run_slice (module
       instantiation, actor.call) must allocate into ITS OWN heap while it runs, then hand
       allocation back to whichever heap was active before it, once it returns. */
    VmHeap* saved_current_heap = vm_current_heap();
    vm_set_current_heap(&vm->heap);
/* Every exit restores all three, yields included: a stale unwind target longjmps into dead stack,
   and a stale heap sends the next allocation somewhere this call no longer owns. */
#define SLICE_RETURN(result)                                                                                 \
    do {                                                                                                     \
        runtime_error_unwind_target = saved_unwind_target;                                                   \
        vm_set_active_error_vm(saved_active_vm);                                                             \
        vm_set_current_heap(saved_current_heap);                                                             \
        return (result);                                                                                     \
    } while (0)
    if (AER_SETJMP(catch_point) != 0)
        SLICE_RETURN(VM_SLICE_ERROR);
    /* The program counter is a moving pointer, not a `code` + offset pair: READ() is then a single
       post-indexed load instead of a base reload, an index-scaled load and a separate increment.
       Only vm_call_resolve_specialization can realloc `code` mid-slice, and it converts to an
       offset and back across that one call. vm->ip stays an offset, which is what everything
       outside this function -- error lines, return addresses, yields -- expects. */
    const uint32_t* pc = c->code + vm->ip;
    /* The active frame's three windows, hoisted: stable for the whole call, refreshed only at the
       3 call/return sites below. Reloading them per opcode was among the hottest instructions in
       the dispatch loop (perf annotate, nbody). The backing stacks are fixed-size inline VM
       arrays, never reallocated, so caching them across dispatches is safe. */
    AerVal* registers = vm->call_stack[vm->call_depth].registers;
    vm->slice_max = max_instructions;
    vm->slice_budget = max_instructions;
    chunk_ensure_debug_hits(c);
    chunk_ensure_field_cache(c);

    /* A designated-initializer table leaves an opcode with no entry as NULL, so emitting one calls
       through a null pointer instead of failing near the mistake -- OP_BINARY sat in the enum in
       exactly that state. Once per process, not per call. */
    static bool dispatch_table_checked = false;
    if (!dispatch_table_checked) {
        dispatch_table_checked = true;
        for (int op = 0; op < (int)OP_OPCODE_COUNT_MARKER; op++) {
            if (aer_handlers[op] == NULL && !opcode_is_tag_only((Opcode)op)) {
                fprintf(stderr, "aer: internal error: opcode %d has no handler\n", op);
                abort();
            }
        }
    }

    /* An ordinary call, not a tail call: this frame owns catch_point, so it has to outlive the
       whole chain -- every handler longjmps back here. */
    uint32_t first_word = *pc++;
    VmSliceResult result = aer_handlers[first_word & 0xFF](vm, pc, registers, c);
    SLICE_RETURN(result);
}

bool vm_run(VM* vm) {
    return vm_run_slice(vm, 0) == VM_SLICE_DONE;
}

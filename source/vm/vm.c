#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "aer_stdlib.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "pool.h"
#include "strbuf.h"
#include "typed_array.h"
#include "value_ops.h"
#include "vm.h"
#include "vm_internal.h"

/* Test-only (tests/smoke_test.c) -- reads a register from whichever frame is active, per VM
   instance so a nested module VM stays isolated. */
AerVal register_get(VM* vm, int slot) {
    return vm->call_stack[vm->call_depth].registers[slot];
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
void chunk_ensure_debug_hits(Chunk* c) {
    if (!profiling || c->count <= c->debug_hits_cap)
        return;
    unsigned int old_cap = c->debug_hits_cap;
    c->debug_hits_cap = c->count;
    c->debug_hits = xrealloc(c->debug_hits, sizeof(uint64_t) * c->debug_hits_cap);
    memset(c->debug_hits + old_cap, 0, sizeof(uint64_t) * (c->debug_hits_cap - old_cap));
}

/* Same growth idiom as chunk_ensure_debug_hits, but unconditional -- a perf feature, not a probe. */
void chunk_ensure_field_cache(Chunk* c) {
    if (c->count <= c->field_cache_cap)
        return;
    unsigned int old_cap = c->field_cache_cap;
    c->field_cache_cap = c->count;
    c->field_cache = xrealloc(c->field_cache, sizeof(FieldCacheEntry) * c->field_cache_cap);
    memset(c->field_cache + old_cap, 0, sizeof(FieldCacheEntry) * (c->field_cache_cap - old_cap));
}

/* Resolves a struct name to its Shape, memoised on the name's pool index -- see Chunk.shape_by_name.
   A miss falls through to the same newest-first scan as before, so a redeclared struct still wins. */
Shape* chunk_shape_for_pool_idx(Chunk* c, unsigned int pool_idx) {
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
void chunk_ensure_call_spec_cache(Chunk* c) {
    if (c->count <= c->call_spec_cache_cap)
        return;
    unsigned int old_cap = c->call_spec_cache_cap;
    c->call_spec_cache_cap = c->count;
    c->call_spec_cache = xrealloc(c->call_spec_cache, sizeof(CallSpecCacheEntry) * c->call_spec_cache_cap);
    memset(c->call_spec_cache + old_cap, 0, sizeof(CallSpecCacheEntry) * (c->call_spec_cache_cap - old_cap));
}

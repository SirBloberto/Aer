#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "aer_host.h"
#include "aer_actor.h"
#include "aer_module.h"
#include "aer_scheduler.h"
#include "aer_stdlib.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "pool.h"
#include "strbuf.h"
#include "vm.h"

/* Whichever VM's heap is currently active -- set by vm_init(), saved/restored around vm_run_slice
   exactly like active_vm_for_errors below (same nested-call shape, same fix). Routes every
   allocation call that has no VM* in scope (the lexer, parts of the parser, which build pooled
   values before/while a Chunk's own VM exists) to the right heap with no signature changes to the
   lexer/parser themselves, since vm_init always runs before parsing starts for the Chunk it owns
   (confirmed: aer_module.c's aer_vm_instantiate_from_file calls vm_init before parse()). */
static VmHeap* current_heap = NULL;

/* Fallback for the one case with no VM at all yet in the whole process (e.g. a host calling
   aer_gc_configure() before ever creating a VM) -- lazily promoted to current_heap so nothing
   dereferences NULL. Mirrors the defensiveness the old vm_pools_init_once() guard already had. */
static VmHeap bootstrap_heap = {0};

static VmHeap* require_current_heap(void) {
    if (!current_heap) current_heap = &bootstrap_heap;
    return current_heap;
}

/* Exposed so a caller that's about to vm_init() a NESTED VM while its own execution is paused on
   the C call stack (aer_vm_instantiate_from_file: a file-module import, or an actor spawn) can
   save the heap that was active before that nested vm_init unconditionally overwrites it, and
   restore it once the nested VM's compile+run cycle is done -- vm_run_slice's own save/restore
   only brackets vm_run() itself, not the vm_init()+parse() that happens before it, which is where
   current_heap first gets clobbered. Without this, allocations made by the OUTER VM after a nested
   import returns would keep landing in the nested VM's heap: a live object only reachable from the
   outer VM's registers, invisible to the nested VM's own (now-independent) GC roots, silently
   collected out from under it -- the exact heap corruption this was written to prevent. */
VmHeap* vm_current_heap(void)          { return current_heap; }
void    vm_set_current_heap(VmHeap* h) { current_heap = h; }

/* Tuning defaults every freshly-initialized heap inherits -- process-wide mutable state, not
   hardcoded constants, specifically so aer_gc_configure()/aer_gc_set_ceiling() keep working when
   called BEFORE any VM exists yet (a real, previously-supported pattern: configure once, then
   create VMs that pick it up). aer_gc_configure/set_ceiling update these AND current_heap's own
   live fields, so both "configure ahead of time" and "reconfigure an already-running VM" work. */
static unsigned int default_minor_gc_threshold     = 2048;
static unsigned int default_major_gc_every_n_minor = 10;
static unsigned int default_gc_live_cell_ceiling   = 0;   /* 0 = unlimited */

/* Initializes one heap's pools -- called once per VM (vm_init), not once per process, since every
   VM now owns its own. */
static void vm_heap_init(VmHeap* heap) {
    if (heap->pools_initialized) return;
    pool_init(&heap->string_pool,   sizeof(AerString),   256);
    pool_init(&heap->array_pool,    sizeof(AerArray),    256);
    pool_init(&heap->dict_pool,     sizeof(AerDict),      64);
    pool_init(&heap->function_pool, sizeof(AerFunction),  64);
    pool_init(&heap->struct_pool,   sizeof(AerStruct) + MAX_STRUCT_FIELDS * sizeof(AerVal), 64);
    pool_init(&heap->packed_array_pool, sizeof(AerPackedArray), 64);
    pool_init(&heap->typed_array_pool, sizeof(AerTypedArray), 64);
    pool_init(&heap->result_pool,   sizeof(AerResult),   64);
    hashtable_pools_init(&heap->dict_hash_pools);
    heap->minor_gc_threshold     = default_minor_gc_threshold;
    heap->major_gc_every_n_minor = default_major_gc_every_n_minor;
    heap->gc_live_cell_ceiling   = default_gc_live_cell_ceiling;
    heap->pools_initialized = true;
}

/* Every allocation from one of a heap's 7 GC-managed pools goes through here instead of calling
   pool_alloc directly, so heap->pool_alloc_count (gc_maybe_collect's trigger) stays accurate --
   this replaces the single process-global counter pool_alloc itself used to keep before pools were
   per-VM. Centralized here rather than at each of the ~10 call sites so there's exactly one place
   that can get this wrong, not ten. */
static void* heap_alloc(VmHeap* heap, Pool* p) {
    heap->pool_alloc_count++;
    return pool_alloc(p);
}

/* A struct instance's field count never changes, so header+items are one allocation, sized
   for the MAX_STRUCT_FIELDS worst case (Pool needs a uniform cell size). */
/* Test-only (tests/smoke_test.c) -- reads a register from whichever frame is active, per VM
   instance so a nested module VM stays isolated. */
AerVal register_get(VM* vm, int slot) {
    return vm->registers[slot];
}

/* Ordinary RK operand as the parser's own compile-time tagged int (RK_CONST_FLAG at bit 30) --
   used only in the two or three spots still working with that representation directly rather
   than a wire-encoded RK8/RK16 byte/halfword. */
static inline AerVal vm_rk_value(VM* vm, Chunk* c, int rk) {
    if (rk & RK_CONST_FLAG) return c->pool[rk & ~RK_CONST_FLAG];
    return vm->registers[rk];
}

/* RK16 (1 flag + 15 index bits) -- the wire form most RK operands use now (a whole halfword to
   itself, or paired with one other 16-bit field). Returns a pointer into the hoisted
   const_pool/registers, not a copy, so callers dereference once or forward it into an
   always_inline consumer. */
static inline AerVal* vm_rk_ptr16(VM* vm, AerVal* const_pool, uint32_t rk16) {
    if (rk16 & RK16_CONST_FLAG) return &const_pool[rk16 & RK16_INDEX_MASK];
    return &vm->registers[rk16 & RK16_INDEX_MASK];
}

/* RK8 (1 flag + 7 index bits) -- the narrow wire form used only where two RK operands must share
   one word alongside a dest register (the OP_ADD..OP_RSHIFT/OP_IN family, OP_INDEX_GET/SET,
   OP_UNARY, OP_CAST). A branch-free variant (per-frame pool window) was tried and reverted -- it
   made calls slower; Lua/V8 accept this branch too and make calls free instead (register_stack). */
static inline AerVal* vm_rk_ptr8(VM* vm, AerVal* const_pool, uint32_t rk8) {
    if (rk8 & RK8_CONST_FLAG) return &const_pool[rk8 & RK8_INDEX_MASK];
    return &vm->registers[rk8 & RK8_INDEX_MASK];
}

/* ------------------------------------------------------------------ */
/* Generational GC — write barrier and remembered set                   */
/* ------------------------------------------------------------------ */

/* True if v's own pooled cell is young; null/boolean/real (and inline integers) have no cell, so they're trivially "not young". */
/* Embedding-facing (vm_gc_suppress/unsuppress here; aer_gc_configure/aer_gc_set_ceiling below) —
   none of these gained a VM* parameter: changing their signatures would break every existing
   embedder. Suppress/unsuppress and aer_gc_stats operate on whichever heap is current (there's no
   "before any VM" case that makes sense for a nesting counter or a stats snapshot). configure/
   set_ceiling are different: real usage calls them BEFORE creating a VM (this project's own
   smoke_test.c does), so they also update process-wide defaults every freshly-initialized heap
   picks up (vm_heap_init), not just current_heap -- see their own comments. */
void vm_gc_suppress(void)   { require_current_heap()->gc_suppress_depth++; }
void vm_gc_unsuppress(void) { VmHeap* h = require_current_heap(); if (h->gc_suppress_depth > 0) h->gc_suppress_depth--; }

void aer_gc_configure(unsigned int minor_threshold, unsigned int major_every_n_minor) {
    if (minor_threshold)     default_minor_gc_threshold     = minor_threshold;
    if (major_every_n_minor) default_major_gc_every_n_minor = major_every_n_minor;
    /* Also apply immediately to whichever heap is already current, if one exists -- so
       reconfiguring an already-running VM takes effect right away, not just for the next one. */
    VmHeap* heap = require_current_heap();
    if (minor_threshold)     heap->minor_gc_threshold     = minor_threshold;
    if (major_every_n_minor) heap->major_gc_every_n_minor = major_every_n_minor;
}

void aer_gc_set_ceiling(unsigned int max_live_cells) {
    default_gc_live_cell_ceiling = max_live_cells;
    require_current_heap()->gc_live_cell_ceiling = max_live_cells;
}

/* Checked once per opcode from DISPATCH(); kept tiny and always_inline so the common case (nowhere near threshold) costs nothing beyond what's already inlined into the dispatch loop. gc_run_collection_cycle (the rare, actual-collection path) lives in gc.c. */
static inline __attribute__((always_inline)) void gc_maybe_collect(VM* vm) {
    VmHeap* heap = &vm->heap;
    if (heap->gc_suppress_depth > 0) return;
    if (heap->pool_alloc_count < heap->minor_gc_threshold) return;
    gc_run_collection_cycle(vm);
}

/* Embedding-facing introspection (include/aer.h); live_cells is a bookkeeping snapshot, not a fresh trace, so it undercounts unswept-but-garbage cells since the last cycle. Reads whichever heap is current -- see vm_gc_suppress's comment. */
void aer_gc_stats(unsigned int* live_cells, unsigned int* minor_collections,
                  unsigned int* major_collections) {
    VmHeap* heap = require_current_heap();
    if (live_cells)         *live_cells         = gc_count_live_cells(heap);
    if (minor_collections)  *minor_collections  = heap->minor_collections_run;
    if (major_collections)  *major_collections  = heap->major_collections_run;
}

#ifdef AER_DEBUG_TOOLS
/* aer_gc_stats() only counts live cells, which understates real usage -- string/array/dict
   payloads are separate xmalloc'd allocations the pool doesn't track. */
void aer_debug_memory_report(FILE* out) {
    VmHeap* heap = require_current_heap();
    fprintf(out, "\n--- memory ---\n");

    uint64_t str_hdr = 0, str_payload = 0;
    {
        Pool* p = &heap->string_pool;
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
        Pool* p = &heap->array_pool;
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
        Pool* p = &heap->dict_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerDict* d = (AerDict*)(p->slabs[i] + (size_t)j * p->stride);
                if (d->gc_state & POOL_FREE) continue;
                dict_hdr += sizeof(AerDict);
                dict_payload += (uint64_t)d->map.capacity * sizeof(unsigned int) +
                                (uint64_t)d->map.dense_capacity * sizeof(HashTableEntry);
                for (unsigned int b = 0; b < d->map.count; b++)
                    dict_payload += d->map.dense[b].length + 1;
            }
        }
    }
    fprintf(out, "  dict     header %10llu B  payload %10llu B\n", dict_hdr, dict_payload);

    uint64_t fn_hdr = 0, fn_payload = 0;
    {
        Pool* p = &heap->function_pool;
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

    /* header = the fixed per-cell reservation; payload = each instance's own Shape.instance_bytes
       (mixed raw/boxed per field now, not a uniform field_count * sizeof(AerVal)) -- the gap is
       MAX_STRUCT_FIELDS's over-provisioning cost plus whatever typed fields saved by being raw. */
    uint64_t struct_hdr = 0, struct_payload = 0;
    {
        Pool* p = &heap->struct_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerStruct* s = (AerStruct*)(p->slabs[i] + (size_t)j * p->stride);
                if (s->gc_state & POOL_FREE) continue;
                struct_hdr += p->stride;
                struct_payload += s->shape->instance_bytes;
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

/* Whichever VM is currently dispatching, kept fresh by DISPATCH() each opcode; self-corrects after a nested module call's vm_run() returns since the outer VM reasserts itself next dispatch. */
static VM* active_vm_for_errors = NULL;

/* Exposed so a caller doing a nested compile+run cycle on its own VM+Chunk (aer_module.c's
   aer_vm_instantiate_from_file) can save/restore this around the WHOLE cycle, not just the
   vm_run() call vm_run_slice's own save/restore already brackets -- vm_init() unconditionally
   repoints this at the new VM the moment it's called, before that VM ever runs, so without an
   explicit save/restore around the entire cycle a failed nested compile leaves this dangling at
   an already-freed VM. Same shape as vm_current_heap()/vm_set_current_heap(). */
VM* vm_active_error_vm(void) { return active_vm_for_errors; }
void vm_set_active_error_vm(VM* vm) { active_vm_for_errors = vm; }

static unsigned int lookup_runtime_line(void) {
    if (!active_vm_for_errors) return 0;
    return chunk_line_for_offset(active_vm_for_errors->chunk, active_vm_for_errors->ip);
}

static const char* lookup_runtime_filename(void) {
    if (!active_vm_for_errors) return NULL;
    return active_vm_for_errors->chunk->source_filename;
}

static const char* lookup_runtime_function(void) {
    if (!active_vm_for_errors) return NULL;
    VM* vm = active_vm_for_errors;
    if (vm->call_depth == 0) return NULL;
    ChunkFunction* fn = chunk_find_function_by_offset(vm->chunk, vm->call_stack[vm->call_depth].code_offset);
    return fn ? aer_as_string(vm->chunk->pool[fn->name])->data : NULL;
}

static const char* tail_call_note(unsigned int collapsed, char* buf, size_t bufsize) {
    if (collapsed == 0) return "";
    snprintf(buf, bufsize, " (+%u tail call%s not shown)", collapsed, collapsed == 1 ? "" : "s");
    return buf;
}

static unsigned int lookup_runtime_stack_trace(char* out, unsigned int out_size) {
    if (!active_vm_for_errors) return 0;
    VM* vm = active_vm_for_errors;
    if (vm->call_depth == 0) return 0;
    Chunk* c = vm->chunk;
    const unsigned int max_frames = 20;
    unsigned int pos = 0, shown = 0;
    bool hit_synthetic_boundary = false;
    char note_buf[64];

    unsigned int innermost_collapsed = vm->call_stack[vm->call_depth].tail_calls_collapsed;
    if (innermost_collapsed > 0) {
        int n = snprintf(out + pos, out_size - pos, "\n %s", tail_call_note(innermost_collapsed, note_buf, sizeof(note_buf)));
        if (n > 0 && (unsigned int)n < out_size - pos) pos += (unsigned int)n;
    }

    for (int depth = (int)vm->call_depth; depth >= 1 && shown < max_frames; depth--) {
        if (vm->call_stack[depth].synthetic_entry) { hit_synthetic_boundary = true; break; }
        unsigned int line = chunk_line_for_offset(c, vm->call_stack[depth].return_ip);
        const char* note = tail_call_note(vm->call_stack[depth - 1].tail_calls_collapsed, note_buf, sizeof(note_buf));
        int n;
        if (depth - 1 == 0) {
            n = snprintf(out + pos, out_size - pos, "\n  called from line %u, at top level%s", line, note);
        } else {
            ChunkFunction* fn = chunk_find_function_by_offset(c, vm->call_stack[depth - 1].code_offset);
            const char* fname = fn ? aer_as_string(c->pool[fn->name])->data : "?";
            n = snprintf(out + pos, out_size - pos, "\n  called from line %u, in %s()%s", line, fname, note);
        }
        if (n < 0 || (unsigned int)n >= out_size - pos) break;
        pos += (unsigned int)n;
        shown++;
    }
    if (!hit_synthetic_boundary && shown < (unsigned int)vm->call_depth) {
        int n = snprintf(out + pos, out_size - pos, "\n  ... and %u more", (unsigned int)vm->call_depth - shown);
        if (n > 0 && (unsigned int)n < out_size - pos) pos += (unsigned int)n;
    }
    return pos;
}

/* Wraps an exclusively-owned (data, length) in a fresh heap box; never copies. Routes to
   current_heap, guarded via require_current_heap() because the lexer can call this
   (emit_string_token) before any VM/pool exists -- without the guard, an uninitialized heap's
   zero elem_size makes pool_alloc hand back a ~1-byte allocation (confirmed heap-buffer-overflow
   via ASAN, back when this was a single process-global pool). */
AerVal aer_make_string(char* data, unsigned int length) {
    VmHeap* heap = require_current_heap();
    vm_heap_init(heap);
    AerString* s = heap_alloc(heap, &heap->string_pool);
    s->data = data;
    s->length = length;
    return aer_string_val(s);
}

/* ------------------------------------------------------------------ */
/* VM lifecycle                                                         */
/* ------------------------------------------------------------------ */

/* Registered here, once per process regardless of how many VMs get created (module loading spins
   up a fresh one per import), so io is exactly as always-on as collection/math/string/etc. with no
   host action needed. aer_register_function() has no dedup check of its own, so calling
   aer_io_register() more than once would silently grow host_functions[] on every import. */
static bool io_registered = false;
static void ensure_io_registered(void) {
    if (io_registered) return;
    aer_io_register();
    io_registered = true;
}

bool aer_io_enabled     = true;
bool aer_net_enabled    = true;
bool aer_import_enabled = true;

void aer_set_io_enabled(bool enabled)     { aer_io_enabled     = enabled; }
void aer_set_net_enabled(bool enabled)    { aer_net_enabled    = enabled; }
void aer_set_import_enabled(bool enabled) { aer_import_enabled = enabled; }

void vm_init(VM* vm, Chunk* chunk) {
    memset(vm, 0, sizeof(*vm));
    vm->chunk = chunk;
    vm->io_enabled  = aer_io_enabled;
    vm->net_enabled = aer_net_enabled;
    vm_heap_init(&vm->heap);
    /* Every VM now owns its own heap -- this VM's is the active allocation target from here on,
       for both its own execution and any parsing that immediately follows for its Chunk (see
       current_heap's own comment). Saved/restored around vm_run_slice for nested/reentrant runs,
       exactly like active_vm_for_errors just below. */
    current_heap = &vm->heap;
    ensure_io_registered();
    runtime_line_lookup        = lookup_runtime_line;
    runtime_filename_lookup    = lookup_runtime_filename;
    runtime_function_lookup    = lookup_runtime_function;
    runtime_stack_trace_lookup = lookup_runtime_stack_trace;
    /* Set here too, not just by DISPATCH() -- makes filename/line lookups correct during parsing
       as well as running (parse happens before vm_run ever dispatches a single opcode). A nested
       module's own vm_init (aer_vm_instantiate_from_file) correctly overwrites this to itself
       while it parses/runs; DISPATCH() reasserts the outer VM on the first opcode after control
       returns, exactly as it already did before this addition. */
    active_vm_for_errors = vm;
    /* Frame 0's register window only ever needs linking once, for the life of the VM (top-level
       usage is open-ended, so it always gets a flat FRAME_REGISTERS reservation) -- everything
       else a fresh run needs is exactly what aer_vm_reset_for_reuse() already does. */
    vm->call_stack[0].registers  = &vm->register_stack[0];
    vm->call_stack[0].frame_size = FRAME_REGISTERS;
    vm->call_stack[0].raw_ints            = &vm->raw_int_stack[0];
    vm->call_stack[0].raw_reals           = &vm->raw_real_stack[0];
    vm->call_stack[0].raw_int_frame_size  = RAW_REGISTERS_INT;
    vm->call_stack[0].raw_real_frame_size = RAW_REGISTERS_REAL;
    aer_vm_reset_for_reuse(vm);
}

void vm_free(VM* vm) {
    VmHeap* heap = &vm->heap;
    /* Each live cell's own separately-owned payload (a string's data buffer, an array's items,
       a dict's whole hashtable, a packed array's data buffer) must be freed before the pool's own
       slab memory goes away -- pool_destroy alone would leak every one of them. Every cell gets
       finalized here regardless of mark/generation state, unlike a normal sweep: the whole heap is
       going away, not just the garbage since the last cycle. Delegates to gc.c (gc_finalize_all_
       pools) rather than exposing all 7 individual finalizer functions just for this one call site. */
    gc_finalize_all_pools(heap);

    pool_destroy(&heap->string_pool);
    pool_destroy(&heap->array_pool);
    pool_destroy(&heap->dict_pool);
    pool_destroy(&heap->function_pool);
    pool_destroy(&heap->struct_pool);
    pool_destroy(&heap->packed_array_pool);
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
    /* If this VM's heap was the active allocation target, it no longer exists -- leaving
       current_heap dangling would be a use-after-free the moment anything allocates next. */
    if (current_heap == heap) current_heap = NULL;
    *heap = (VmHeap){0};
}

void aer_vm_reset_for_reuse(VM* vm) {
    vm->stack_top  = 0;
    vm->call_depth = 0;
    vm->registers  = vm->call_stack[0].registers;
    vm->raw_ints   = vm->call_stack[0].raw_ints;
    vm->raw_reals  = vm->call_stack[0].raw_reals;
}

bool aer_run_source(VM* vm, Chunk* chunk, const char* source) {
    /* parse() below runs BEFORE vm_run(vm) -- vm_run_slice's own current_heap save/restore only
       wraps the run, not this function's own parse step, and vm_init already ran for `vm` (this
       is the "run more code into an already-initialized VM" entry point, REPL-style), so nothing
       else sets current_heap here. Without this, a parse-time allocation (a string literal, an
       interned token) lands in whatever heap some OTHER, unrelated VM last left active instead of
       this one's own -- exactly the cross-heap contamination per-VM heaps exist to prevent. */
    vm_set_current_heap(&vm->heap);
    /* Re-seeded on every call, not just at vm_init: aer_run_source is the "run more code into an
       already-initialized VM" entry point (REPL, embedding), and the documented capability-toggle
       pattern is to flip aer_set_io_enabled/net_enabled(false), run one thing, then flip it back —
       on an existing vm, not a freshly created one (see embed_smoke_test.c). Without this, that
       pattern would silently do nothing once the vm's own fields were seeded once at vm_init. */
    vm->io_enabled  = aer_io_enabled;
    vm->net_enabled = aer_net_enabled;
    aer_vm_reset_for_reuse(vm);
    vm->ip = chunk->count;
    shell((char*)source);   /* shell() strdup()s its own copy — never mutates through this pointer */
    lex();
    parse(chunk);
    chunk_emit(chunk, OP_HALT);
    runtime_had_error = false;
    return vm_run(vm);
}

/* ------------------------------------------------------------------ */
/* Type helpers                                                         */
/* ------------------------------------------------------------------ */

/* Struct instances report their declared name (e.g. "Player") instead of "array" — used by type(),
   the one way to check a struct's shape now (type(x) == "Player") since OP_CHECK_SHAPE was removed.
   A packed array reports "Player[]" — distinct from a single instance's own "Player". type_names[]
   is indexed directly by ValueType, so it must stay exactly as long as the enum's non-specially-
   handled entries (value.h) — TYPE_STRUCT/TYPE_PACKED_ARRAY/TYPE_TYPED_ARRAY/TYPE_RESULT are all
   handled specially, so none of them is ever used to index this array. */
static const char* vm_type_name(Chunk* c, AerVal v) {
    static const char* type_names[] = {
        "null", "boolean", "integer", "float", "string", "function", "array", "hashtable"
    };
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
        static const char* elem_names[] = { "int32", "float32", "integer", "float" };
        static char buf[32];
        snprintf(buf, sizeof(buf), "%s[]", elem_names[aer_as_typed_array(v)->elem_kind]);
        return buf;
    }
    if (aer_type(v) == TYPE_RESULT) return "Result";
    return type_names[aer_type(v)];
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
        case TYPE_STRUCT:   return true;   /* a struct can never have zero fields, enforced at parse time */
        case TYPE_PACKED_ARRAY: return aer_as_packed_array(v)->count > 0;
        case TYPE_TYPED_ARRAY:  return aer_as_typed_array(v)->count > 0;
        /* `if result:` reads like `if err == null:`, without destructuring first. */
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

/* Int/int and real/real fast path only -- small enough to always_inline at its two callers
   (the field-fusion opcodes) without icache bloat. Sets *handled = false for anything else;
   caller falls back to vm_binary_cold(). */
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

/* Cold path for the per-operator labels and the field-fusion opcodes -- everything
   vm_binary_fast() doesn't handle. Real, non-inlined: this path is rare, and not inlining it
   avoids duplicating the body across every call site. */
/* The one `in` implementation -- called by lbl_in and by vm_binary_cold (reached when a fused
   opcode carries OP_IN as its runtime bin_op). */
static AerVal vm_in(AerVal a, AerVal b) {
    if (aer_type(b) == TYPE_DICT) {
        if (aer_type(a) != TYPE_STRING) { error("Left side of 'in' must be a string when testing dict membership"); return aer_bool(false); }
        AerString* as = aer_as_string(a);
        if (as->length > VM_KEY_MAX) { error("Hashtable key too long (max %d bytes)", VM_KEY_MAX); return aer_bool(false); }
        unsigned int klen = hashtable_key_true_len(as->data, as->length);
        char kbuf[VM_KEY_MAX + 1];
        memcpy(kbuf, as->data, klen);
        kbuf[klen] = '\0';
        return aer_bool(hashtable_get_hashed(&aer_as_dict(b)->map, kbuf, klen, hashtable_hash_bytes(as->data, klen)) != NULL);
    }
    if (aer_type(b) == TYPE_ARRAY) {
        AerArray* arr = aer_as_array(b);
        for (unsigned int i = 0; i < arr->count; i++) {
            if (values_equal(a, arr->items[i])) return aer_bool(true);
        }
        return aer_bool(false);
    }
    if (aer_type(b) == TYPE_STRING) {
        if (aer_type(a) != TYPE_STRING) { error("Left side of 'in' must be a string when testing string membership"); return aer_bool(false); }
        return aer_bool(aer_string_find(aer_as_string(b), aer_as_string(a)) >= 0);
    }
    error("Right side of 'in' must be a dict, array, or string");
    return aer_bool(false);
}

/* Only used to name the operator in a type-mismatch message -- never on a path that already has
   its own more specific error (e.g. 'in' has vm_in()'s own messages above). */
static const char* binop_symbol(Opcode op) {
    switch (op) {
        case OP_ADD:       return "+";
        case OP_SUB:       return "-";
        case OP_MUL:       return "*";
        case OP_DIV:       return "/";
        case OP_FLOOR_DIV: return "//";
        case OP_MOD:       return "%";
        case OP_EQ:        return "==";
        case OP_NEQ:       return "!=";
        case OP_LT:        return "<";
        case OP_GT:        return ">";
        case OP_LTE:       return "<=";
        case OP_GTE:       return ">=";
        case OP_IN:        return "in";
        default:           return "that operator";
    }
}

static AerVal vm_binary_cold(Chunk* c, AerVal a, AerVal b, Opcode op, ValueType ta, ValueType tb) {
    /* Checked before null-handling below so `null in arr` isn't intercepted by the "null op anything-else errors" rule, which is about direct comparison, not container search. */
    if (op == OP_IN) return vm_in(a, b);

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
            /* Same total order collection.sort() uses for strings — one shared helper (value.h). */
            int cmp = aer_string_compare(as, bs);
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

    error("Cannot apply '%s' to %s and %s", binop_symbol(op), vm_type_name(c, a), vm_type_name(c, b));
    return aer_bool(false);
}

static AerVal vm_to_str(VM* vm, AerVal v) {
    if (aer_type(v) == TYPE_STRING) return v;

    char*        owned;
    unsigned int len;

    if (aer_type(v) == TYPE_ARRAY || aer_type(v) == TYPE_DICT || aer_type(v) == TYPE_STRUCT || aer_type(v) == TYPE_PACKED_ARRAY || aer_type(v) == TYPE_TYPED_ARRAY || aer_type(v) == TYPE_RESULT) {
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
            case TYPE_INTEGER:  snprintf(buf, sizeof(buf), "%lld", (long long)aer_as_int(v));               break;
            case TYPE_REAL:     aer_format_real(aer_as_real(v), buf, sizeof(buf));                 break;
            case TYPE_BOOLEAN:  snprintf(buf, sizeof(buf), "%s",   aer_as_bool(v) ? "true" : "false"); break;
            case TYPE_FUNCTION: snprintf(buf, sizeof(buf), "<function>");                        break;
            case TYPE_ARRAY: case TYPE_DICT: case TYPE_STRUCT: case TYPE_STRING: case TYPE_PACKED_ARRAY: case TYPE_TYPED_ARRAY: case TYPE_RESULT: break;   /* handled above */
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

/* A baked default (struct field or function parameter): primitives copy as-is; an array/dict
   default must become a FRESH empty container, or every omitted call/instance would alias the
   same one (Python's mutable-default bug). parser.c only ever bakes an empty '[]'/'{}' as such
   a default, so a fresh empty one is always correct -- no deep copy needed. */
static AerVal vm_default_value(VM* vm, AerVal dflt) {
    if (aer_type(dflt) == TYPE_ARRAY && !aer_as_array(dflt)->shape) {
        AerArray* a = heap_alloc(&vm->heap, &vm->heap.array_pool);
        a->count = a->capacity = 0;
        a->items = NULL;
        a->shape = NULL;
        a->generation = 0;
        return aer_array_val(a);
    }
    if (aer_type(dflt) == TYPE_DICT) {
        AerDict* d = heap_alloc(&vm->heap, &vm->heap.dict_pool);
        memset(&d->map, 0, sizeof(d->map));
        d->map.pools = &vm->heap.dict_hash_pools;
        return aer_dict_val(d);
    }
    return dflt;
}

/* Cross-module call setup (aer_module_call): mirrors lbl_call_value's frame-push, standalone
   since this isn't inside vm_run's dispatch loop. dest_reg fixed at 0 sets up the frame right
   above target's own frame 0, so once vm_run(target) drains back to depth 0, the result sits
   in target->call_stack[0].registers[0]. */
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
    callee->raw_ints   = caller->raw_ints  + caller->raw_int_frame_size;
    callee->raw_reals  = caller->raw_reals + caller->raw_real_frame_size;
    callee->raw_int_frame_size  = fn->max_raw_ints;
    callee->raw_real_frame_size = fn->max_raw_reals;
    for (int i = 0; i < arg_count; i++)
        callee->registers[i] = args[i];
    for (int i = arg_count; i < (int)fn->arity; i++)
        callee->registers[i] = vm_default_value(target, fn->defaults[i - fn->min_arity]);
    callee->return_ip   = return_ip;
    callee->dest_reg    = 0;
    callee->code_offset = fn->code_offset;
    callee->tail_calls_collapsed = 0;
    callee->synthetic_entry = true;
    target->call_depth++;   /* same rooting rule as vm_call_value's non-tail branch (above) — the defaults loop wrote into callee->registers[] before this point */
    gc_maybe_collect(target);
    target->registers = target->call_stack[target->call_depth].registers;
    target->raw_ints  = target->call_stack[target->call_depth].raw_ints;
    target->raw_reals = target->call_stack[target->call_depth].raw_reals;
    target->ip = fn->code_offset;
    return true;
}

/* Factored out of lbl_call_value since a plain function can't itself jump to a vm_run-local
   label -- it does the work and lets the caller DISPATCH(). return_ip is the caller's own
   resume address, passed explicitly rather than read from vm->ip. */
static void vm_call_value(VM* vm, AerVal fv, int dest_reg, int arg_reg_base, int arg_count,
                              bool is_tail_call, unsigned int return_ip) {
    if (aer_type(fv) != TYPE_FUNCTION) { error("Value of type '%s' is not callable", vm_type_name(vm->chunk, fv)); return; }
    AerFunction* f = aer_as_function(fv);
    if (arg_count < (int)f->min_arity || arg_count > (int)f->arity) {
        if (f->min_arity == f->arity)
            error("Function expects %u arguments, got %d", (unsigned int)f->arity, arg_count);
        else
            error("Function expects between %u and %u arguments, got %d", (unsigned int)f->min_arity, (unsigned int)f->arity, arg_count);
        return;
    }
    /* Tail-call reuse -- `f` is already a plain pointer, so overwriting its source register during
       the copy can't invalidate it. Args copied before defaults, so no source register is
       overwritten before it's read. */
    if (is_tail_call) {
        for (int i = 0; i < arg_count; i++)
            vm->registers[i] = vm->registers[arg_reg_base + i];
        for (int i = arg_count; i < (int)f->arity; i++)
            vm->registers[i] = vm_default_value(vm, f->defaults[i - f->min_arity]);
        gc_maybe_collect(vm);   /* defaults just written into the CURRENT frame (tail call, call_depth unchanged) — already rooted */
        vm->ip = f->code_offset;
        CallFrame* reused = &vm->call_stack[vm->call_depth];
        reused->code_offset = f->code_offset;   /* reused frame now runs a different function */
        /* Same stale-sizing hazard as lbl_call's OP_TAIL_CALL branch (see its own comment) -- `f`
           may need a different max_registers/max_raw_ints/max_raw_reals than whatever function
           last occupied this frame. Must be refreshed here too, not just on the non-tail push path
           above (which already sizes from f->max_registers/max_raw_ints/max_raw_reals). */
        reused->frame_size          = f->max_registers;
        reused->raw_int_frame_size  = f->max_raw_ints;
        reused->raw_real_frame_size = f->max_raw_reals;
        reused->tail_calls_collapsed++;
        return;
    }
    if (vm->call_depth + 1 >= VM_CALL_MAX) { error("v3 call stack overflow"); return; }
    CallFrame* caller = &vm->call_stack[vm->call_depth];
    CallFrame* callee = &vm->call_stack[vm->call_depth + 1];
    callee->registers  = caller->registers + caller->frame_size;
    callee->frame_size  = f->max_registers;
    callee->raw_ints   = caller->raw_ints  + caller->raw_int_frame_size;
    callee->raw_reals  = caller->raw_reals + caller->raw_real_frame_size;
    callee->raw_int_frame_size  = f->max_raw_ints;
    callee->raw_real_frame_size = f->max_raw_reals;
    for (int i = 0; i < arg_count; i++)
        callee->registers[i] = caller->registers[arg_reg_base + i];
    for (int i = arg_count; i < (int)f->arity; i++)
        callee->registers[i] = vm_default_value(vm, f->defaults[i - f->min_arity]);
    callee->return_ip   = return_ip;
    callee->dest_reg    = dest_reg;
    callee->code_offset = f->code_offset;
    callee->tail_calls_collapsed = 0;
    callee->synthetic_entry = false;
    vm->call_depth++;   /* the defaults loop above wrote into callee->registers[] BEFORE this point, when mark_vm_roots's 0..call_depth scan didn't yet cover that frame — gc_maybe_collect() must run AFTER this increment, not before, or a collection could reclaim a fresh default array/dict as unreachable */
    gc_maybe_collect(vm);
    vm->registers = vm->call_stack[vm->call_depth].registers;
    vm->raw_ints  = vm->call_stack[vm->call_depth].raw_ints;
    vm->raw_reals = vm->call_stack[vm->call_depth].raw_reals;
    vm->ip = f->code_offset;
}

/* Resolves field_idx within shape via the per-site inline cache (keyed by bytecode offset) --
   shared by both boxed-struct and packed-array callers, since a field's slot within a given
   Shape is identical either way. False (error reported) if shape has no such field. */
static inline __attribute__((always_inline)) bool vm_resolve_field_by_shape(Chunk* c, unsigned int site, Shape* shape, int field_idx,
                                 int* out_slot, unsigned int* out_offset, ValueType* out_ftype) {
    FieldCacheEntry* entry = &c->field_cache[site];
    if (entry->shape == shape) {
        *out_slot   = entry->slot;
        *out_offset = entry->offset;
        *out_ftype  = entry->ftype;
        return true;
    }
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == (unsigned int)field_idx) {
            entry->shape  = shape;
            entry->slot   = (int)i;
            entry->offset = shape->field_offsets[i];
            entry->ftype  = shape->field_types[i];
            *out_slot   = (int)i;
            *out_offset = entry->offset;
            *out_ftype  = entry->ftype;
            return true;
        }
    }
    error("'%s' has no field '%s'", aer_as_string(c->pool[shape->name])->data,
          aer_as_string(c->pool[field_idx])->data);
    return false;
}

/* Resolves struct_reg's field to an (AerStruct*, slot, offset, ftype) tuple, delegating the
   shape+cache lookup above. Shared by every field-access opcode. False (error reported) if not a
   struct instance or no such field. */
static inline __attribute__((always_inline)) bool vm_resolve_field(VM* vm, Chunk* c, unsigned int site, int struct_reg, int field_idx,
                                 AerStruct** out_s, int* out_slot, unsigned int* out_offset, ValueType* out_ftype) {
    AerVal* obj = &vm->registers[struct_reg];
    if (obj->tag != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        return false;
    }
    AerStruct* s = (AerStruct*)obj->as.ptr;
    *out_s = s;
    return vm_resolve_field_by_shape(c, site, s->shape, field_idx, out_slot, out_offset, out_ftype);
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

/* One struct field, at its own Shape-computed byte offset -- raw via vm_packed_slot_read/write for
   a typed field, a plain 16-byte memcpy for TYPE_ANY (it isn't a fixed-width 8-byte payload, so the
   packed-slot helpers don't apply). Declared in vm.h since aer_json.c's struct-serialization branch
   needs these too, not just vm.c's own opcodes. Iterates every field with no per-site cache to draw
   on (print/json.encode), so it looks offset/ftype up fresh -- vm_struct_field_read_at below is the
   one every opcode call site should use instead, once it already has them from the field cache. */
AerVal vm_struct_field_read(AerStruct* s, unsigned int slot) {
    ValueType ftype = s->shape->field_types[slot];
    unsigned int offset = s->shape->field_offsets[slot];
    unsigned char* p = s->fields + offset;
    if (ftype == TYPE_ANY) { AerVal v; memcpy(&v, p, sizeof(AerVal)); return v; }
    return vm_packed_slot_read(p, ftype);
}

void vm_struct_field_write(AerStruct* s, unsigned int slot, AerVal v) {
    ValueType ftype = s->shape->field_types[slot];
    unsigned int offset = s->shape->field_offsets[slot];
    unsigned char* p = s->fields + offset;
    if (ftype == TYPE_ANY) memcpy(p, &v, sizeof(AerVal));
    else vm_packed_slot_write(p, ftype, v);
}

/* Same contract as vm_struct_field_read/write above, but offset/ftype are already in hand (from
   vm_resolve_field's cache output) instead of being re-derived from s->shape here -- every opcode
   call site uses these, not the by-slot versions above. The `ftype == TYPE_ANY` branch itself is
   not the cost this avoids: for any real (monomorphic) call site it's the same outcome every single
   time, so branch prediction makes it free after the first iteration. What was real and worth
   removing was the extra pointer-chase through s->shape to re-fetch offset/ftype on every access
   even after the cache already proved the shape matched. */
static inline AerVal vm_struct_field_read_at(AerStruct* s, unsigned int offset, ValueType ftype) {
    unsigned char* p = s->fields + offset;
    if (ftype == TYPE_ANY) { AerVal v; memcpy(&v, p, sizeof(AerVal)); return v; }
    return vm_packed_slot_read(p, ftype);
}

static inline void vm_struct_field_write_at(AerStruct* s, unsigned int offset, ValueType ftype, AerVal v) {
    unsigned char* p = s->fields + offset;
    if (ftype == TYPE_ANY) memcpy(p, &v, sizeof(AerVal));
    else vm_packed_slot_write(p, ftype, v);
}

/* One element of a TYPE_TYPED_ARRAY (AerTypedArray) -- a completely separate function from
   vm_packed_slot_read/write above, deliberately NOT merged into one wider-switch helper: that
   function's own comment documents a real, measured ARM regression from adding exactly this kind of
   branch to its hot path, and struct-field access (already tuned, already fusion-opcode-heavy) has
   nothing to gain from sharing code with a brand new, unrelated value kind. */
static inline unsigned int vm_typed_elem_width(TypedArrayElemKind kind) {
    return (kind == TYPED_ELEM_INT32 || kind == TYPED_ELEM_FLOAT32) ? 4 : 8;
}

static inline AerVal vm_typed_elem_read(unsigned char* slot, TypedArrayElemKind kind) {
    switch (kind) {
        case TYPED_ELEM_INT32:   { int32_t v; memcpy(&v, slot, 4); return aer_int(v); }
        case TYPED_ELEM_FLOAT32: { float    v; memcpy(&v, slot, 4); return aer_real((double)v); }
        case TYPED_ELEM_INT64:   { int64_t v; memcpy(&v, slot, 8); return aer_int(v); }
        case TYPED_ELEM_FLOAT64: { double   v; memcpy(&v, slot, 8); return aer_real(v); }
    }
    return aer_null();
}

/* Caller must already have validated v against kind -- see vm_typed_array_check. int32 in
   particular is never silently wrapped: an out-of-range value is rejected there, not truncated
   here. */
static inline void vm_typed_elem_write(unsigned char* slot, TypedArrayElemKind kind, AerVal v) {
    switch (kind) {
        case TYPED_ELEM_INT32:   { int32_t iv = (int32_t)aer_as_int(v); memcpy(slot, &iv, 4); break; }
        case TYPED_ELEM_FLOAT32: { float    fv = (float)(aer_type(v) == TYPE_INTEGER ? (double)aer_as_int(v) : aer_as_real(v)); memcpy(slot, &fv, 4); break; }
        case TYPED_ELEM_INT64:   { int64_t iv = aer_as_int(v); memcpy(slot, &iv, 8); break; }
        case TYPED_ELEM_FLOAT64: { double   dv = (aer_type(v) == TYPE_INTEGER ? (double)aer_as_int(v) : aer_as_real(v)); memcpy(slot, &dv, 8); break; }
    }
}

/* Natural literal shape, not exact-tag matching (unlike a typed struct field's OP_FIELD_SET) --
   `arr[i] = 5` into a float32 array shouldn't require writing `5.0`. int32/int64 variants still
   require TYPE_INTEGER; int32 additionally range-checks (erroring rather than silently wrapping,
   this project's established "fail loudly" convention -- see OP_CAST's own integer-overflow
   handling). float32/float64 accept TYPE_INTEGER or TYPE_REAL, promoted, matching the existing
   promotion convention in the raw-arithmetic `_boxed` handlers. */
static bool vm_typed_array_check(Chunk* c, TypedArrayElemKind kind, AerVal val) {
    switch (kind) {
        case TYPED_ELEM_INT64:
            if (aer_type(val) != TYPE_INTEGER) {
                error("Cannot assign a %s into an integer[] array", vm_type_name(c, val));
                return false;
            }
            return true;
        case TYPED_ELEM_INT32:
            if (aer_type(val) != TYPE_INTEGER) {
                error("Cannot assign a %s into an int32[] array", vm_type_name(c, val));
                return false;
            }
            if (aer_as_int(val) < INT32_MIN || aer_as_int(val) > INT32_MAX) {
                error("Value %lld out of range for an int32[] array", (long long)aer_as_int(val));
                return false;
            }
            return true;
        case TYPED_ELEM_FLOAT64:
        case TYPED_ELEM_FLOAT32:
            if (aer_type(val) != TYPE_INTEGER && aer_type(val) != TYPE_REAL) {
                error("Cannot assign a %s into a %s array", vm_type_name(c, val),
                      kind == TYPED_ELEM_FLOAT32 ? "float32[]" : "float[]");
                return false;
            }
            return true;
    }
    return false;
}

/* Returns an owned copy of dense[*idx]'s key. Shared by array-iteration's dict branch and
   pair-iteration. False once exhausted -- the dense array has no holes, so this is a plain
   bounds check, not a scan. */
static bool vm_dict_next_key(AerDict* d, int64_t* idx, AerVal* out_key) {
    if ((uint64_t)*idx >= d->map.count) return false;
    unsigned int key_len = d->map.dense[*idx].length;
    char* key_buf = xmalloc(key_len + 1);
    memcpy(key_buf, d->map.dense[*idx].key, key_len);
    key_buf[key_len] = '\0';
    *out_key = aer_make_string(key_buf, key_len);   /* no chunk_add_pool interning — see vm_to_str's comment */
    return true;
}

AerArray* vm_new_array(void) {
    VmHeap* heap = require_current_heap();
    return heap_alloc(heap, &heap->array_pool);
}

AerDict* vm_new_dict(void) {
    VmHeap* heap = require_current_heap();
    AerDict* d = heap_alloc(heap, &heap->dict_pool);
    /* Every caller used to memset(&d->map, 0, sizeof(d->map)) itself right after this call --
       centralized here instead so setting .pools below can't be wiped out by a caller's own
       zeroing running afterward. */
    memset(&d->map, 0, sizeof(d->map));
    d->map.pools = &heap->dict_hash_pools;
    return d;
}

AerVal aer_make_result(AerVal value, AerVal err) {
    VmHeap* heap = require_current_heap();
    AerResult* r = heap_alloc(heap, &heap->result_pool);
    r->value = value;
    r->err   = err;
    return aer_result_val(r);
}

AerVal aer_make_error(const char* msg) {
    size_t n   = strlen(msg);
    char*  buf = xmalloc(n + 1);
    memcpy(buf, msg, n + 1);
    return aer_make_string(buf, (unsigned int)n);
}

AerFunction* vm_new_function(void) {
    VmHeap* heap = require_current_heap();
    return heap_alloc(heap, &heap->function_pool);
}

/* builtin_id resolved at parse time. Returns true if arg_count matched, result in *out. */
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
            else if (aer_type(a) == TYPE_TYPED_ARRAY) *out = aer_int((int64_t)aer_as_typed_array(a)->count);
            else error("length() requires an array, dict, string, packed array, or typed array");
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

/* Shared by lbl_index_get and the fused index-get handlers -- same dispatch, bounds, and
   errors as the original inline body, returning the value instead of pushing it. */
/* Writes through `out` (avoids a 16-byte stack round-trip returning by value -- see
   BINARY_OP_INT_REAL's comment). Safe even if `out` aliases obj's/idx's register. */
static inline void vm_index_get_compute(AerVal obj, AerVal idx, AerVal* out) {
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) { error("Struct fields are accessed with '.', not '[]'"); *out = aer_null(); return; }
        if (aer_type(idx) != TYPE_INTEGER) { error("Array index must be an integer"); *out = aer_null(); return; }
        int64_t i = aer_as_int(idx);
        if (i < 0) i += (int64_t)a->count;
        if (i < 0 || (uint64_t)i >= a->count) { error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(idx), a->count); *out = aer_null(); return; }
        *out = a->items[i]; return;
    } else if (aer_type(obj) == TYPE_DICT) {
        if (aer_type(idx) != TYPE_STRING) { error("Hashtable key must be a string"); *out = aer_null(); return; }
        AerString* is = aer_as_string(idx);
        if (is->length > VM_KEY_MAX) { error("Hashtable key too long (max %d bytes)", VM_KEY_MAX); *out = aer_null(); return; }
        unsigned int klen = hashtable_key_true_len(is->data, is->length);
        char kbuf[VM_KEY_MAX + 1];
        memcpy(kbuf, is->data, klen);
        kbuf[klen] = '\0';
        AerVal* found = hashtable_get_hashed(&aer_as_dict(obj)->map, kbuf, klen, hashtable_hash_bytes(is->data, klen));
        if (!found) { *out = aer_null(); return; }
        *out = *found; return;
    } else if (aer_type(obj) == TYPE_STRING) {
        AerString* os = aer_as_string(obj);
        if (aer_type(idx) != TYPE_INTEGER) { error("String index must be an integer"); *out = aer_null(); return; }
        int64_t i = aer_as_int(idx);
        int64_t len = (int64_t)os->length;
        if (i < 0) i += len;
        if (i < 0 || i >= len) { error("String index %lld out of bounds (len %lld)", (long long)aer_as_int(idx), (long long)len); *out = aer_null(); return; }
        /* A single character is a length-1 string (AER has no char type); copies the byte since AerString must always own its data, even after obj is later collected. */
        char* ch_buf = xmalloc(2);
        ch_buf[0] = os->data[i];
        ch_buf[1] = '\0';
        *out = aer_make_string(ch_buf, 1); return;   /* no chunk_add_pool interning — see vm_to_str's comment */
    } else if (aer_type(obj) == TYPE_RESULT) {
        /* result[0] is the value, result[1] is the err -- the same order every stdlib fallible
           function returns. */
        if (aer_type(idx) != TYPE_INTEGER) { error("Result index must be an integer"); *out = aer_null(); return; }
        int64_t i = aer_as_int(idx);
        AerResult* r = aer_as_result(obj);
        if (i == 0) { *out = r->value; return; }
        if (i == 1) { *out = r->err;   return; }
        error("Result index %lld out of bounds (a Result only has indices 0 and 1)", (long long)aer_as_int(idx));
        *out = aer_null();
    } else if (aer_type(obj) == TYPE_TYPED_ARRAY) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if (aer_type(idx) != TYPE_INTEGER) { error("Array index must be an integer"); *out = aer_null(); return; }
        int64_t i = aer_as_int(idx);
        if (i < 0) i += (int64_t)ta->count;
        if (i < 0 || (uint64_t)i >= ta->count) { error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(idx), ta->count); *out = aer_null(); return; }
        unsigned int width = vm_typed_elem_width(ta->elem_kind);
        *out = vm_typed_elem_read(ta->data + (size_t)i * width, ta->elem_kind);
    } else {
        error("Cannot index type");
        *out = aer_null();
    }
}

/* `a, b = expr` — a genuine Result unpacks to (value, err); a plain 2-element array (not a
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

/* Shared by lbl_index_set and the fused index-set handlers -- same dispatch/bounds/errors.
   Caller must DISPATCH() immediately after. */
static inline void vm_index_set_compute(VM* vm, AerVal obj, AerVal idx, AerVal val) {
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) { error("Struct fields are assigned with '.', not '[]'"); return; }
        if (aer_type(idx) != TYPE_INTEGER) { error("Array index must be an integer"); return; }
        int64_t i = aer_as_int(idx);
        if (i < 0) i += (int64_t)a->count;
        if (i < 0 || (uint64_t)i >= a->count) { error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(idx), a->count); return; }
        gc_barrier_array(vm, a, val);
        a->items[i] = val;
        /* Replacing an element can change whether this array is uniformly one struct shape --
           invalidates lbl_call's SPEC_KIND_ARRAY_OF_STRUCTS "already verified homogeneous"
           per-call-site cache (vm.c), which is keyed on (array pointer, generation, shape). */
        a->generation++;
    } else if (aer_type(obj) == TYPE_DICT) {
        if (aer_type(idx) != TYPE_STRING) { error("Hashtable key must be a string"); return; }
        AerString* is = aer_as_string(idx);
        if (is->length > VM_KEY_MAX) { error("Hashtable key too long (max %d bytes)", VM_KEY_MAX); return; }
        unsigned int klen = hashtable_key_true_len(is->data, is->length);
        uint64_t khash = hashtable_hash_bytes(is->data, klen);
        char kbuf[VM_KEY_MAX + 1];
        memcpy(kbuf, is->data, klen);
        kbuf[klen] = '\0';
        gc_barrier_dict(vm, aer_as_dict(obj), val);
        AerVal* existing = hashtable_get_hashed(&aer_as_dict(obj)->map, kbuf, klen, khash);
        if (existing) {
            *existing = val;   /* update in place — no allocation */
        } else {
            char* k = hashtable_key_dup(aer_as_dict(obj)->map.pools, is->data, klen, NULL);   /* klen already true length */
            hashtable_put_hashed(&aer_as_dict(obj)->map, k, klen, khash, val);
        }
    } else if (aer_type(obj) == TYPE_STRING) {
        error("Strings are immutable — cannot assign to an index");
    } else if (aer_type(obj) == TYPE_TYPED_ARRAY) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        if (aer_type(idx) != TYPE_INTEGER) { error("Array index must be an integer"); return; }
        int64_t i = aer_as_int(idx);
        if (i < 0) i += (int64_t)ta->count;
        if (i < 0 || (uint64_t)i >= ta->count) { error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(idx), ta->count); return; }
        if (!vm_typed_array_check(vm->chunk, ta->elem_kind, val)) return;
        unsigned int width = vm_typed_elem_width(ta->elem_kind);
        vm_typed_elem_write(ta->data + (size_t)i * width, ta->elem_kind, val);
    } else {
        error("Cannot index type");
    }
}

/* integer(x)/float(x)/boolean(x)/string(x) conversion rules, shared by OP_CAST's handler below. */
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
                    /* strtoll, not atoll -- atoll can't distinguish "parsed as zero" from "not a number". */
                    char* end;
                    long long parsed = strtoll(buf, &end, 10);
                    while (*end == ' ' || *end == '\t') end++;   /* tolerate trailing whitespace, same as leading */
                    if (end == buf || *end != '\0') {
                        error("integer('%.*s'): not a valid integer", (int)n, buf);
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
                    /* strtod, not atof -- same reasoning as CAST_INTEGER above. */
                    char* end;
                    double parsed = strtod(buf, &end);
                    while (*end == ' ' || *end == '\t') end++;
                    if (end == buf || *end != '\0') {
                        error("float('%.*s'): not a valid number", (int)n, buf);
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
/* Grows debug_hits to cover c->code, zero-filling the new region; a no-op once already covered
   (REPL appends code across calls). */
static void chunk_ensure_debug_hits(Chunk* c) {
    if (c->count <= c->debug_hits_cap) return;
    unsigned int old_cap = c->debug_hits_cap;
    c->debug_hits_cap = c->count;
    c->debug_hits = xrealloc(c->debug_hits, sizeof(uint64_t) * c->debug_hits_cap);
    memset(c->debug_hits + old_cap, 0, sizeof(uint64_t) * (c->debug_hits_cap - old_cap));
}
#endif

/* Same growth idiom as chunk_ensure_debug_hits, but always-on (a real perf feature, not debug). */
static void chunk_ensure_field_cache(Chunk* c) {
    if (c->count <= c->field_cache_cap) return;
    unsigned int old_cap = c->field_cache_cap;
    c->field_cache_cap = c->count;
    c->field_cache = xrealloc(c->field_cache, sizeof(FieldCacheEntry) * c->field_cache_cap);
    memset(c->field_cache + old_cap, 0, sizeof(FieldCacheEntry) * (c->field_cache_cap - old_cap));
}

/* Same idiom, for lbl_call's shape-specialization per-site dispatch cache (see its own comment). */
static void chunk_ensure_call_spec_cache(Chunk* c) {
    if (c->count <= c->call_spec_cache_cap) return;
    unsigned int old_cap = c->call_spec_cache_cap;
    c->call_spec_cache_cap = c->count;
    c->call_spec_cache = xrealloc(c->call_spec_cache, sizeof(CallSpecCacheEntry) * c->call_spec_cache_cap);
    memset(c->call_spec_cache + old_cap, 0, sizeof(CallSpecCacheEntry) * (c->call_spec_cache_cap - old_cap));
}

/* max_instructions == 0 means unlimited (every existing caller via the vm_run() wrapper below) --
   the budget decrement only happens at loop-back-edge and call opcodes (the only places a script
   can spend unbounded time), not on every DISPATCH(), so the check costs nothing on the common
   unlimited path and stays cheap even when a budget is active. See aer_scheduler.c for the caller
   that actually uses a nonzero budget. */
VmSliceResult vm_run_slice(VM* vm, unsigned int max_instructions) {
    Chunk* c = vm->chunk;
    /* Hoisted once -- c->pool is only mutated at parse time, stable for the whole call. */
    AerVal* const_pool = c->pool;
    /* Installs this call's error catch point, saving the previous one so nested vm_run calls
       catch their own errors and unwind no further than here; restored on every return. */
    AerJmpBuf  catch_point;
    AerJmpBuf* saved_unwind_target = runtime_error_unwind_target;
    runtime_error_unwind_target    = &catch_point;
    /* One store here + one restore at each exit, instead of every DISPATCH() -- the VM never
       changes mid-call. */
    VM* saved_active_vm  = active_vm_for_errors;
    active_vm_for_errors = vm;
    /* Same save/restore shape as active_vm_for_errors just above -- a nested vm_run_slice (module
       instantiation, actor.call) must allocate into ITS OWN heap while it runs, then hand
       allocation back to whichever heap was active before it, once it returns. */
    VmHeap* saved_current_heap = current_heap;
    current_heap = &vm->heap;
    if (AER_SETJMP(catch_point) != 0) {
        runtime_error_unwind_target = saved_unwind_target;
        active_vm_for_errors        = saved_active_vm;
        current_heap                = saved_current_heap;
        return VM_SLICE_ERROR;
    }
    Opcode cur_op;
    /* word0 -- opcode(8) plus up to 3 narrow packed fields (PACK3) or one 16-bit field
       (PACK_OP_A_W16), depending on cur_op's own fixed shape. Must survive past DISPATCH()'s own
       do-while into the handler body the goto jumps to. Any further words the opcode's shape
       needs are read via READ() directly in the handler body, same as the original design's own
       trailing-word convention. */
    uint32_t op_word;
    /* Hoisted local for vm->ip -- every jump/call/READ() would otherwise reload/store it on every
       touch. Synced at the end of every DISPATCH() and around vm_call_value (the only other
       write to this VM's ip). */
    unsigned int ip = vm->ip;
    /* Only ever read/decremented at the handful of yield-checkpoints below; never touched when
       max_instructions is 0. */
    unsigned int slice_budget = max_instructions;
#ifdef AER_DEBUG_TOOLS
    chunk_ensure_debug_hits(c);
#endif
    chunk_ensure_field_cache(c);

#define READ()     (c->code[ip++])
#define PUSH(v)    do { if (vm->stack_top >= VM_STACK_MAX) { error("Stack overflow"); return VM_SLICE_ERROR; } vm->stack[vm->stack_top++] = (v); } while(0)
#define POP()      (vm->stack_top > 0 ? vm->stack[--vm->stack_top] : (error("Stack underflow"), aer_null()))
#ifdef AER_DEBUG_TOOLS
/* Not called from DISPATCH() -- each allocating label calls it right after storing its result
   into a VM-visible root. Labels that can never allocate (confirmed by inspection) have no call
   at all, a real zero-cost dispatch for the common case. */
/* No error check here -- error()/error_at() longjmp straight to this call's catch_point on
   fault, so DISPATCH() itself never needs to poll anything. */
#define DISPATCH() do { unsigned int op_ip = ip; op_word = READ(); vm->ip = ip; cur_op = (Opcode)(op_word & 0xFF); c->debug_hits[op_ip]++; goto *dt[cur_op]; } while(0)
#else
/* Full 8-bit mask -- opcode is unambiguously its own byte now (OP_OPCODE_COUNT_MARKER's static
   assert guarantees <=256), no reason to ever mask narrower. */
#define DISPATCH() do { op_word = READ(); vm->ip = ip; cur_op = (Opcode)(op_word & 0xFF); goto *dt[cur_op]; } while(0)
#endif

    static const void* const dt[] = {
        /* Unary ops have no entries -- only ever embedded as a tag inside OP_UNARY. OP_ADD..OP_IN ARE
           real top-level dispatch targets (true single-level dispatch, PACK_BINARY). */
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
        [OP_DESTRUCTURE]       = &&lbl_destructure,
        [OP_SLICE_GET]         = &&lbl_slice_get,
        [OP_DICT_NEW]          = &&lbl_dict_new,
        [OP_ITER_NEXT_ARRAY]   = &&lbl_iter_next_array,
        [OP_ITER_NEXT_PAIR]    = &&lbl_iter_next_pair,
        [OP_ITER_RANGE_PREP]      = &&lbl_iter_range_prep,
        [OP_ITER_RANGE_LOOP]      = &&lbl_iter_range_loop,
        [OP_STRUCT_NEW]        = &&lbl_struct_new,
        [OP_FIELD_GET]         = &&lbl_field_get,
        [OP_FIELD_SET]         = &&lbl_field_set,
        [OP_ARRAY_REPEAT]      = &&lbl_array_repeat,
        [OP_INDEX_FIELD_GET]   = &&lbl_index_field_get,
        [OP_INDEX_FIELD_SET]   = &&lbl_index_field_set,
        [OP_INDEX_FIELD_COMPOUND] = &&lbl_index_field_compound,
        [OP_UNARY]             = &&lbl_unary,
        [OP_CAST]              = &&lbl_cast,
        [OP_BINARY_FIELD]      = &&lbl_binary_field,
        [OP_FIELD_BINARY]      = &&lbl_field_binary,
        [OP_FIELD_COMPOUND]    = &&lbl_field_compound,
        [OP_PRINT_REPL]        = &&lbl_print_repl,

        /* Raw-arithmetic family -- see the lbl_raw_* labels below for why no vm_rk_ptr8/tag-check
           is needed. */
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
        [OP_RAW_ADD_REAL_BOXED_TO] = &&lbl_raw_add_real_boxed_to,
        [OP_RAW_MUL_REAL_BOXED_TO] = &&lbl_raw_mul_real_boxed_to,
        [OP_RAW_LOAD_INT_POOL]  = &&lbl_raw_load_int_pool,
        [OP_RAW_LT_INT_BOXED]   = &&lbl_raw_lt_int_boxed,
        [OP_RAW_GT_INT_BOXED]   = &&lbl_raw_gt_int_boxed,
        [OP_RAW_LTE_INT_BOXED]  = &&lbl_raw_lte_int_boxed,
        [OP_RAW_GTE_INT_BOXED]  = &&lbl_raw_gte_int_boxed,
        [OP_RAW_LT_REAL_BOXED]  = &&lbl_raw_lt_real_boxed,
        [OP_RAW_GT_REAL_BOXED]  = &&lbl_raw_gt_real_boxed,
        [OP_RAW_LTE_REAL_BOXED] = &&lbl_raw_lte_real_boxed,
        [OP_RAW_GTE_REAL_BOXED] = &&lbl_raw_gte_real_boxed,

        [OP_INDEX_FIELD_GET_RAW_INT]  = &&lbl_index_field_get_raw_int,
        [OP_INDEX_FIELD_GET_RAW_REAL] = &&lbl_index_field_get_raw_real,
        [OP_FIELD_GET_RAW_INT]        = &&lbl_field_get_raw_int,
        [OP_FIELD_GET_RAW_REAL]       = &&lbl_field_get_raw_real,
        [OP_INDEX_FIELD_SET_RAW_INT]  = &&lbl_index_field_set_raw_int,
        [OP_INDEX_FIELD_SET_RAW_REAL] = &&lbl_index_field_set_raw_real,
        [OP_FIELD_SET_RAW_INT]        = &&lbl_field_set_raw_int,
        [OP_FIELD_SET_RAW_REAL]       = &&lbl_field_set_raw_real,
        [OP_FIELD_COMPOUND_RAW_INT]        = &&lbl_field_compound_raw_int,
        [OP_FIELD_COMPOUND_RAW_REAL]        = &&lbl_field_compound_raw_real,
        [OP_INDEX_FIELD_COMPOUND_RAW_INT]  = &&lbl_index_field_compound_raw_int,
        [OP_INDEX_FIELD_COMPOUND_RAW_REAL] = &&lbl_index_field_compound_raw_real,
        [OP_UNBOX_PARAM_INT]  = &&lbl_unbox_param_int,
        [OP_UNBOX_PARAM_REAL] = &&lbl_unbox_param_real,
    };

    DISPATCH();

lbl_jump: {
    int target = READ();
    ip = (unsigned int)target;
    /* Every loop's back-edge (while/for/plain jump alike) goes through here -- the one checkpoint
       that bounds an AER-level loop's slice length. ip already points at a complete instruction
       (this jump's own operand is fully consumed), so yielding here is always resumable. */
    if (max_instructions && --slice_budget == 0) { vm->ip = ip; return VM_SLICE_YIELDED; }
    DISPATCH();
}

lbl_define_struct: {
    int name_idx    = (int)UNPACK_STRUCT_HEADER_NAME(op_word);
    int field_count = (int)UNPACK_STRUCT_HEADER_COUNT(op_word);
    Shape* shape = xmalloc(sizeof(Shape));
    shape->name        = (unsigned int)name_idx;
    shape->field_count = (unsigned int)field_count;
    for (int i = 0; i < field_count; i++) {
        uint32_t name_default_word = READ();
        shape->field_names[i]    = UNPACK_2X16_HI(name_default_word);
        shape->field_defaults[i] = c->pool[UNPACK_2X16_LO(name_default_word)];
        shape->field_types[i]    = (ValueType)READ();
    }
    /* Typed fields get 8 raw bytes (no tag -- the type is this Shape's own static knowledge);
       TYPE_ANY fields get a full boxed AerVal (16 bytes), since they can hold a reference type the
       GC must trace. See vm_struct_field_read/write. */
    {
        unsigned int offset = 0;
        for (unsigned int i = 0; i < shape->field_count; i++) {
            shape->field_offsets[i] = offset;
            offset += (shape->field_types[i] == TYPE_ANY) ? sizeof(AerVal) : 8;
        }
        shape->instance_bytes = offset;
    }
    if (c->shape_count >= c->shape_cap) {
        c->shape_cap = c->shape_cap ? c->shape_cap * 2 : 4;
        c->shapes = xrealloc(c->shapes, sizeof(Shape*) * c->shape_cap);
    }
    c->shapes[c->shape_count++] = shape;
    DISPATCH();
}

/* dest+pool_idx both fit in word0 now (op(8)+dest(8)+pool_idx(16)) -- no trailing word needed,
   down from the original design's 2-word form. */
lbl_loadk: {
    int dest = (int)UNPACK_A(op_word);
    unsigned int pool_idx = UNPACK_W16(op_word);
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

/* Dest + both RK operands already in op_word -- no further READ(). Each operator is its own
   top-level dispatch target, checks its own fast path, and falls back to vm_binary_cold() for
   anything else. BINARY_OP_INT_REAL has both fast paths; BINARY_OP_INT_ONLY (bitwise/shift) has
   no real/real meaning. */
/* `result` points straight at the destination register -- building a local AerVal and copying
   it measured as a 16-byte stack round-trip that was the single hottest instruction in the
   nbody profile. Safe even when dest aliases ra/rb (l/rv already snapshotted by value). */
/* Only in the vm_binary_cold() branch -- every int/int and real/real fast-path result is a
   plain tagged-union construction, never an allocation. */
#define BINARY_OP_INT_REAL(NAME, OPENUM, INT_STMT, REAL_STMT) \
lbl_##NAME: { \
    int dest = (int)UNPACK_A(op_word); \
    AerVal* ra = vm_rk_ptr8(vm, const_pool, UNPACK_B(op_word)); \
    AerVal* rb = vm_rk_ptr8(vm, const_pool, UNPACK_C(op_word)); \
    ValueType ta = ra->tag, tb = rb->tag; \
    AerVal* result = &vm->registers[dest]; \
    if (ta == TYPE_INTEGER && tb == TYPE_INTEGER) { \
        int64_t l = ra->as.i, rv = rb->as.i; \
        INT_STMT \
    } else if (ta == TYPE_REAL && tb == TYPE_REAL) { \
        double l = ra->as.d, rv = rb->as.d; \
        REAL_STMT \
    } else { \
        *result = vm_binary_cold(c, *ra, *rb, OPENUM, ta, tb); \
        gc_maybe_collect(vm); \
    } \
    DISPATCH(); \
}
/* Bitwise family never allocates in any branch, so no gc_maybe_collect at all. */
#define BINARY_OP_INT_ONLY(NAME, OPENUM, INT_STMT) \
lbl_##NAME: { \
    int dest = (int)UNPACK_A(op_word); \
    AerVal* ra = vm_rk_ptr8(vm, const_pool, UNPACK_B(op_word)); \
    AerVal* rb = vm_rk_ptr8(vm, const_pool, UNPACK_C(op_word)); \
    ValueType ta = ra->tag, tb = rb->tag; \
    AerVal* result = &vm->registers[dest]; \
    if (ta == TYPE_INTEGER && tb == TYPE_INTEGER) { \
        int64_t l = ra->as.i, rv = rb->as.i; \
        INT_STMT \
    } else { \
        *result = vm_binary_cold(c, *ra, *rb, OPENUM, ta, tb); \
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

/* No int/int or real/real fast path -- dispatches straight to the shared vm_in(). */
lbl_in: {
    int dest = (int)UNPACK_A(op_word);
    AerVal a = *vm_rk_ptr8(vm, const_pool, UNPACK_B(op_word));
    AerVal b = *vm_rk_ptr8(vm, const_pool, UNPACK_C(op_word));
    vm->registers[dest] = vm_in(a, b);
    DISPATCH();
}

/* Control flow -- stack-neutral, reads only registers[]/the pool. */
lbl_jump_if_false_reg: {
    int reg    = (int)UNPACK_A(op_word);
    int target = READ();
    if (!vm_truthy(vm->registers[reg])) ip = (unsigned int)target;
    DISPATCH();
}

/* Bulk-copies args from the caller's bank into the new frame's bank at register 0; overflow
   check mirrors vm_setup_call's own ceiling. */
lbl_call: {
    int dest_reg      = (int)UNPACK_A(op_word);
    int arg_reg_base  = (int)UNPACK_B(op_word);
    int arg_count     = (int)UNPACK_C(op_word);
    int callee_offset = READ();
    /* func_index is the target's index into c->functions[] -- always present (emit_call always
       emits it, patched in later by func_register for a forward reference), letting this size the
       callee's frame from its real max_registers/max_raw_ints/max_raw_reals peaks instead of a
       flat, function-agnostic ceiling. */
    unsigned int func_index = (unsigned int)READ();
    ChunkFunction* target_f = &c->functions[func_index];
    /* Tail-call reuse -- the copy iterates with dest_i always <= source_i, the same
       always-safe-forward-shift pattern memmove uses when dest <= src, so no overlap
       special-casing is needed. */
    if (cur_op == OP_TAIL_CALL) {
        for (int i = 0; i < arg_count; i++)
            vm->registers[i] = vm->registers[arg_reg_base + i];
        ip = (unsigned int)callee_offset;
        CallFrame* reused = &vm->call_stack[vm->call_depth];
        reused->code_offset = (unsigned int)callee_offset;   /* reused frame now runs a different function */
        /* The reused frame's own sizing was set for WHATEVER function last occupied it -- a tail
           call can jump to a completely different function with a different max_registers/
           max_raw_ints/max_raw_reals peak. Left stale, a later real (non-tail) call pushed from
           inside this reused frame would compute its child's base from the WRONG (possibly too
           small) frame_size, silently overlapping this frame's own still-live registers/raw
           slots. base pointers (registers/raw_ints/raw_reals) are untouched -- same frame, same
           backing memory, only how much of it this frame claims for itself changes. */
        reused->frame_size          = target_f->max_registers;
        reused->raw_int_frame_size  = target_f->max_raw_ints;
        reused->raw_real_frame_size = target_f->max_raw_reals;
        reused->tail_calls_collapsed++;
        /* Every call (tail or not) is the other place a script can spend unbounded time
           (recursion instead of a loop) -- checked once ip already points at the callee's real
           entry point, so a yield here always resumes at a valid instruction boundary. */
        if (max_instructions && --slice_budget == 0) { vm->ip = ip; return VM_SLICE_YIELDED; }
        DISPATCH();
    }
    if (vm->call_depth + 1 >= VM_CALL_MAX) { error("v3 call stack overflow"); DISPATCH(); }

    /* Shape-based specialization dispatch -- gated on shape_sensitive_mask (zero for the
       overwhelming majority of functions, a single already-fetched field check) rather than a
       separate opcode. A separate OP_CALL_SPEC, chosen at EMISSION time, can't work: shape-
       sensitivity is only known once a function's ENTIRE body has finished compiling, but a
       forward-referenced call and a self-recursive call both need their opcode decided before
       that -- doing it here instead means every call site just sees the final, fully-compiled
       truth regardless of when or how it was originally compiled, no forward-reference or
       recompile-timing problem to solve at all. */
    unsigned int chosen_offset        = (unsigned int)callee_offset;
    unsigned int chosen_max_registers = target_f->max_registers;
    unsigned int chosen_max_raw_ints  = target_f->max_raw_ints;
    unsigned int chosen_max_raw_reals = target_f->max_raw_reals;

    if (!target_f->megamorphic && target_f->shape_sensitive_mask != 0) {
        unsigned int site = ip - 3;   /* this instruction's own word0 offset -- ip already advanced past all 3 words by now */
        /* Lowest set bit -- which argument register carries the shape-sensitive parameter. Only
           the first such parameter is ever used to key specialization; a function using more than
           one parameter for field access still compiles and runs correctly, it just never
           specializes on the others. */
        int param_index = __builtin_ctz(target_f->shape_sensitive_mask);
        AerVal arg = vm->registers[arg_reg_base + param_index];
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

        /* SPEC_KIND_ARRAY_OF_STRUCTS gets no structural guarantee, so -- unlike the other two
           kinds, which are safe forever once observed -- this must re-verify EVERY call, not just
           on a shape/cache miss: same array reused across two calls could be mutated to hold a
           different shape in between, and nothing else here would notice. O(n), same order as
           advance_pass's own per-call work, so this is the exact cost the design accepted for this
           case (see linear-rolling-dream.md's Part B). A single mismatched element silently falls
           back to the generic body for THIS call only -- observed is left in place conceptually,
           but the cache/table are simply never touched below, so a later, uniform call still
           specializes normally.

           The scan itself is skippable, though: if THIS call site already fully verified this EXACT
           array (by pointer) at its CURRENT AerArray.generation against this same shape on some
           earlier call, items[] provably hasn't been restructured since (generation only changes on
           index-assignment/append/delete/insert -- see its own comment, value.h) -- trusting that
           prior verification is exactly as safe as the structural guarantee the other two kinds get
           for free, just re-derived per generation instead of assumed forever. This is what turns
           struct_array_scan.aer's real pattern (same 2M-particle array, 50 calls, only field VALUES
           change between calls) from paying the O(n) scan on every one of the 50 into paying it once. */
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
                    site_entry->last_verified_array      = arr;
                    site_entry->last_verified_generation  = arr->generation;
                }
            }
        }

        if (observed) {
            chunk_ensure_call_spec_cache(c);
            CallSpecCacheEntry* site_entry = &c->call_spec_cache[site];
            SpecEntry* entry = NULL;
            if (site_entry->last_shape == observed) {
                chosen_offset        = site_entry->last_code_offset;
                chosen_max_registers = site_entry->last_max_registers;
                chosen_max_raw_ints  = site_entry->last_max_raw_ints;
                chosen_max_raw_reals = site_entry->last_max_raw_reals;
                entry = site_entry->last_entry;
            } else {
                SpecEntry* found = NULL;
                for (int i = 0; i < target_f->specialization_count; i++)
                    if (target_f->specializations[i].shape == observed) { found = &target_f->specializations[i]; break; }
                if (!found && target_f->specialization_count < SPEC_MAX) {
                    SpecEntry fresh;
                    if (parser_specialize_function(c, target_f, observed, kind, param_index, &fresh, NULL, NULL, 0)) {
                        /* The recompile just appended new code to THIS running chunk -- every
                           per-word cache must cover the new size before any of its sites dispatch.
                           debug_hits[] is the same idiom (chunk_ensure_debug_hits) but debug-tools-
                           only -- DISPATCH() increments c->debug_hits[offset] for every opcode word
                           when built with AER_DEBUG_TOOLS, so skipping this resize here is a real,
                           silent out-of-bounds write the moment the specialized body's own code
                           (now beyond the ORIGINAL debug_hits_cap) executes, in that build only —
                           found via a real Windows heap-corruption crash inside a LATER, unrelated
                           malloc, exactly the kind of delayed symptom this class of bug produces. */
#ifdef AER_DEBUG_TOOLS
                        chunk_ensure_debug_hits(c);
#endif
                        chunk_ensure_field_cache(c);
                        chunk_ensure_call_spec_cache(c);
                        site_entry = &c->call_spec_cache[site];   /* re-fetch: chunk_ensure_call_spec_cache may have reallocated the array */
                        target_f->specializations[target_f->specialization_count++] = fresh;
                        found = &target_f->specializations[target_f->specialization_count - 1];
                    } else {
                        target_f->megamorphic = true;   /* treat a recompile failure like exhausting the table -- stop retrying every call */
                    }
                } else if (!found) {
                    target_f->megamorphic = true;   /* table full and still a new shape -- stop specializing this function */
                }
                if (found) {
                    site_entry->last_shape          = found->shape;
                    site_entry->last_code_offset    = found->code_offset;
                    site_entry->last_max_registers  = found->max_registers;
                    site_entry->last_max_raw_ints   = found->max_raw_ints;
                    site_entry->last_max_raw_reals  = found->max_raw_reals;
                    site_entry->last_entry          = found;
                    chosen_offset        = found->code_offset;
                    chosen_max_registers = found->max_registers;
                    chosen_max_raw_ints  = found->max_raw_ints;
                    chosen_max_raw_reals = found->max_raw_reals;
                    entry = found;
                }
            }

            /* Raw-numeric-variant check -- an independent axis from the shape cache/table lookup
               above, re-derived fresh every call (cheap: at most SPEC_MAX_RAW_PARAMS aer_type()
               reads, nowhere near the cost of a shape/homogeneity miss) rather than mirrored into
               its own site-cache fields, since `entry` (from either the cache hit or the table
               lookup just above) already gives direct access to whichever SpecEntry's own
               raw_variant_* fields are relevant. See SpecEntry's own comment, vm.h, for why this is
               a SEPARATE per-entry variant rather than folding raw-param-kind into the shape axis
               itself. */
            /* SPEC_KIND_ARRAY_OF_STRUCTS was previously excluded here, based on an apparent ~20%
               regression on struct_array_scan.aer. Re-investigated with real hardware performance
               counters (perf stat, Raspberry Pi 4) rather than wall-clock alone: dispatch counts and
               GC stats were already known to be identical or better with the variant enabled: this
               time cycles, instructions, AND branch-misses were all lower too (branch-misses ~18-40x
               lower: 289-292K vs 5.3-11.8M across repeated runs) -- and the ORIGINAL guarded build's
               own branch-misses varied more than 2x between two back-to-back runs of the identical
               binary, meaning that build was unstable on its own before any comparison even started.
               That instability is consistent with the original "~20% regression" having been a noisy
               single-sample wall-clock artifact, not a real effect. Confirmed at both a reduced scale
               (N=100,000, isolated from an unrelated O(n^2) GC bug in unrelated benchmark setup code)
               and the real N=2,000,000 scale (19.01s enabled vs 20.32s excluded, ~6.4% faster,
               consistent with the reduced-scale ~7% figure) -- guard removed. */
            if (entry && entry->raw_param_count >= 0) {
                int cand_regs[SPEC_MAX_RAW_PARAMS];
                ValueType cand_types[SPEC_MAX_RAW_PARAMS];
                int cand_count = 0;
                for (unsigned int pi = 0; pi < target_f->arity && cand_count < SPEC_MAX_RAW_PARAMS; pi++) {
                    if ((int)pi == param_index) continue;
                    AerVal pv = vm->registers[arg_reg_base + pi];
                    ValueType pt = aer_type(pv);
                    if (pt != TYPE_INTEGER && pt != TYPE_REAL) { cand_count = 0; break; }   /* not all-numeric -- no raw variant applies this call */
                    cand_regs[cand_count]  = (int)pi;
                    cand_types[cand_count] = pt;
                    cand_count++;
                }

                if (cand_count > 0) {
                    bool matches_existing = entry->raw_param_count == cand_count;
                    if (matches_existing) {
                        for (int k = 0; k < cand_count; k++)
                            if (entry->raw_param_regs[k] != cand_regs[k] || entry->raw_param_types[k] != cand_types[k]) { matches_existing = false; break; }
                    }
                    if (matches_existing) {
                        chosen_offset        = entry->raw_variant_code_offset;
                        chosen_max_registers = entry->raw_variant_max_registers;
                        chosen_max_raw_ints  = entry->raw_variant_max_raw_ints;
                        chosen_max_raw_reals = entry->raw_variant_max_raw_reals;
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
#ifdef AER_DEBUG_TOOLS
                            chunk_ensure_debug_hits(c);
#endif
                            chunk_ensure_field_cache(c);
                            chunk_ensure_call_spec_cache(c);
                            entry->raw_variant_code_offset     = variant.code_offset;
                            entry->raw_variant_max_registers   = variant.max_registers;
                            entry->raw_variant_max_raw_ints    = variant.max_raw_ints;
                            entry->raw_variant_max_raw_reals   = variant.max_raw_reals;
                            for (int k = 0; k < cand_count; k++) {
                                entry->raw_param_regs[k]  = cand_regs[k];
                                entry->raw_param_types[k] = cand_types[k];
                            }
                            entry->raw_param_count = cand_count;
                            chosen_offset        = entry->raw_variant_code_offset;
                            chosen_max_registers = entry->raw_variant_max_registers;
                            chosen_max_raw_ints  = entry->raw_variant_max_raw_ints;
                            chosen_max_raw_reals = entry->raw_variant_max_raw_reals;
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

    CallFrame* caller = &vm->call_stack[vm->call_depth];
    CallFrame* callee = &vm->call_stack[vm->call_depth + 1];
    callee->registers  = caller->registers + caller->frame_size;
    callee->frame_size  = chosen_max_registers;
    callee->raw_ints   = caller->raw_ints  + caller->raw_int_frame_size;
    callee->raw_reals  = caller->raw_reals + caller->raw_real_frame_size;
    callee->raw_int_frame_size  = chosen_max_raw_ints;
    callee->raw_real_frame_size = chosen_max_raw_reals;
    for (int i = 0; i < arg_count; i++)
        callee->registers[i] = caller->registers[arg_reg_base + i];
    callee->return_ip   = ip;   /* already past this instruction's operands — the correct resume point */
    callee->dest_reg    = dest_reg;
    callee->code_offset = chosen_offset;
    callee->tail_calls_collapsed = 0;
    callee->synthetic_entry = false;
    vm->call_depth++;
    vm->registers = vm->call_stack[vm->call_depth].registers;
    vm->raw_ints  = vm->call_stack[vm->call_depth].raw_ints;
    vm->raw_reals = vm->call_stack[vm->call_depth].raw_reals;
    ip = chosen_offset;
    if (max_instructions && --slice_budget == 0) { vm->ip = ip; return VM_SLICE_YIELDED; }
    DISPATCH();
}

/* Same frame-push shape as lbl_call, but the target is a runtime AerFunction read from a register.
   3 registers fit word0 (PACK3); callee_reg is the 4th and gets its own trailing word (no room
   left in a 32-bit word0 for a 4th 8-bit field alongside the opcode). */
lbl_call_value: {
    int dest_reg     = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count    = (int)UNPACK_C(op_word);
    int callee_reg   = (int)READ();
    /* vm_call_value writes vm->ip on success (or leaves it untouched on error) -- reload before
       the next READ(). ip is already past callee_reg's word, the correct resume address. */
    vm_call_value(vm, vm->registers[callee_reg], dest_reg, arg_reg_base, arg_count,
                      cur_op == OP_TAIL_CALL_VALUE, ip);
    ip = vm->ip;
    DISPATCH();
}

/* return_ip/dest_reg live in the callee's own frame, not a shared global -- what makes
   nested/recursive calls safe. */
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

/* Bridges to the same stack-based stdlib dispatch lbl_call_module uses. */
lbl_call_module: {
    int dest_reg     = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count    = (int)UNPACK_C(op_word);
    int module_idx   = (int)READ();
    int fn_idx       = (int)READ();
    uint32_t ids_word = READ();
    int module_id    = (int)UNPACK_2X16_HI(ids_word);
    int fn_id        = (int16_t)UNPACK_2X16_LO(ids_word);   /* sign-extend -- FN_ID_UNKNOWN is -1 */
    for (int i = 0; i < arg_count; i++) PUSH(vm->registers[arg_reg_base + i]);
    bool handled = false;
    /* module_id/fn_id resolved at parse time -- a switch on two ints instead of a strcmp chain.
       CALL_MODULE_DYNAMIC (host/file module) still resolves by name at runtime. */
    switch (module_id) {
        case CALL_MODULE_MATH:   handled = aer_math_call(vm, fn_id, arg_count);   break;
        case CALL_MODULE_RANDOM: handled = aer_random_call(vm, fn_id, arg_count); break;
        case CALL_MODULE_STRING: handled = aer_string_call(vm, fn_id, arg_count); break;
        case CALL_MODULE_TIME:   handled = aer_time_call(vm, fn_id, arg_count);   break;
        case CALL_MODULE_JSON:   handled = aer_json_call(vm, c, fn_id, arg_count);   break;
        case CALL_MODULE_COLLECTION: handled = aer_collection_call(vm, fn_id, arg_count); break;
        case CALL_MODULE_NET:    handled = aer_net_call(vm, fn_id, arg_count);   break;
        case CALL_MODULE_REGEX:  handled = aer_regex_call(vm, fn_id, arg_count); break;
        case CALL_MODULE_ACTOR:     handled = aer_actor_module_call(vm, fn_id, arg_count);     break;
        case CALL_MODULE_SCHEDULER: handled = aer_scheduler_module_call(vm, fn_id, arg_count); break;
        default: {
            const char* module = aer_as_string(c->pool[module_idx])->data;
            const char* fn     = aer_as_string(c->pool[fn_idx])->data;
            /* io is the one module still reached through the generic host-call path
               (aer_host_call) rather than a fixed CALL_MODULE_* case -- gated here by name
               specifically so --no-io never touches a real embedding host's own custom modules,
               which go through this exact same path. */
            if (!vm->io_enabled && strcmp(module, "io") == 0) {
                for (int i = 0; i < arg_count; i++) POP();
                error("io is disabled for this run (--no-io)");
                PUSH(aer_null());
                handled = true;
                break;
            }
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

/* vm_call_builtin takes a plain AerVal* array -- no push/pop bridge needed. */
lbl_call_builtin: {
    int dest_reg     = (int)UNPACK_A(op_word);
    int arg_reg_base = (int)UNPACK_B(op_word);
    int arg_count    = (int)UNPACK_C(op_word);
    int name_idx     = (int)READ();
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

/* Builds an array from an in-order register range -- mark_vm_roots's registers scan keeps the
   result alive across GC. */
lbl_array_new: {
    int dest_reg      = (int)UNPACK_A(op_word);
    int item_reg_base = (int)UNPACK_B(op_word);
    int item_count    = (int)UNPACK_C(op_word);
    AerArray* a = heap_alloc(&vm->heap, &vm->heap.array_pool);
    a->capacity = item_count > 0 ? (unsigned int)item_count : 4;
    a->count    = (unsigned int)item_count;
    a->items    = xmalloc(sizeof(AerVal) * a->capacity);
    a->shape    = NULL;
    a->generation = 0;
    for (int i = 0; i < item_count; i++)
        a->items[i] = vm->registers[item_reg_base + i];
    vm->registers[dest_reg] = aer_array_val(a);
    gc_maybe_collect(vm);   /* pool_alloc(&array_pool) above; result already rooted */
    DISPATCH();
}

/* Already type-generic (array/dict/string) with all bounds/negative-index logic. */
lbl_index_get: {
    int dest_reg = (int)UNPACK_A(op_word);
    int arr_reg  = (int)UNPACK_B(op_word);
    AerVal* idx = vm_rk_ptr8(vm, const_pool, UNPACK_C(op_word));
    AerVal obj = vm->registers[arr_reg];
    vm_index_get_compute(obj, *idx, &vm->registers[dest_reg]);
    /* Only single-char string indexing allocates -- array/dict indexing never touches the heap, so
       skip the check for the dominant common case. */
    if (aer_type(obj) == TYPE_STRING) gc_maybe_collect(vm);
    DISPATCH();
}

/* a, b = expr — see vm_destructure_compute. Never allocates, unlike lbl_index_get, so no
   gc_maybe_collect needed. */
lbl_destructure: {
    int t0      = (int)UNPACK_A(op_word);
    int t1      = (int)UNPACK_B(op_word);
    int src_reg = (int)UNPACK_C(op_word);
    vm_destructure_compute(vm->registers[src_reg], &vm->registers[t0], &vm->registers[t1]);
    DISPATCH();
}

/* Includes the internal write barrier, now exercised against a register-held reference. */
lbl_index_set: {
    int arr_reg = (int)UNPACK_A(op_word);
    AerVal idx = *vm_rk_ptr8(vm, const_pool, UNPACK_B(op_word));
    AerVal val = *vm_rk_ptr8(vm, const_pool, UNPACK_C(op_word));
    vm_index_set_compute(vm, vm->registers[arr_reg], idx, val);
    DISPATCH();
}

/* `arr[a:b]` — vm_slice_bounds() resolves/clamps the bounds; a slice is always a fresh copy. */
lbl_slice_get: {
    int dest_reg = (int)UNPACK_A(op_word);
    int arr_reg  = (int)UNPACK_B(op_word);
    uint32_t bounds_word = READ();
    AerVal start_v = *vm_rk_ptr16(vm, const_pool, UNPACK_2X16_HI(bounds_word));
    AerVal end_v   = *vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(bounds_word));
    AerVal obj = vm->registers[arr_reg];
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) { error("Structs cannot be sliced"); vm->registers[dest_reg] = aer_null(); DISPATCH(); }
        int64_t start, end;
        if (!vm_slice_bounds(start_v, end_v, (int64_t)a->count, &start, &end)) { vm->registers[dest_reg] = aer_null(); DISPATCH(); }
        unsigned int n = (unsigned int)(end - start);
        AerArray* r = heap_alloc(&vm->heap, &vm->heap.array_pool);
        r->count    = n;
        r->capacity = n > 0 ? n : 4;
        r->items    = xmalloc(sizeof(AerVal) * r->capacity);
        r->shape    = NULL;   /* a slice is always a plain array, even of a struct */
        r->generation = 0;
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

/* Errors unless src_reg holds exactly that struct type; never converts. */
/* Dict literal -- each key stored as an owned copy, never an alias into the source string. */
lbl_dict_new: {
    int dest_reg      = (int)UNPACK_A(op_word);
    int pair_reg_base = (int)UNPACK_B(op_word);
    int pair_count    = (int)UNPACK_C(op_word);
    AerDict* d = heap_alloc(&vm->heap, &vm->heap.dict_pool);
    memset(&d->map, 0, sizeof(d->map));
    d->map.pools = &vm->heap.dict_hash_pools;
    if (pair_count > 0) hashtable_reserve(&d->map, (unsigned int)pair_count);
    for (int i = 0; i < pair_count; i++) {
        AerVal key = vm->registers[pair_reg_base + 2 * i];
        AerVal val = vm->registers[pair_reg_base + 2 * i + 1];
        if (aer_type(key) != TYPE_STRING) { error("Hashtable keys must be strings"); continue; }
        AerString* ks = aer_as_string(key);
        unsigned int klen = hashtable_key_true_len(ks->data, ks->length);
        uint64_t khash = hashtable_hash_bytes(ks->data, klen);
        char* k = hashtable_key_dup(d->map.pools, ks->data, klen, NULL);
        hashtable_put_hashed(&d->map, k, klen, khash, val);
    }
    vm->registers[dest_reg] = aer_dict_val(d);
    gc_maybe_collect(vm);   /* pool_alloc(&dict_pool) above; result already rooted */
    DISPATCH();
}

/* Arrays yield items, dicts yield keys, strings yield 1-char strings. */
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
        /* Yields one-character strings, same shape as dict-key iteration. */
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
    if (aer_type(col) == TYPE_TYPED_ARRAY) {
        /* Unlike a packed array's struct elements (no standalone `arr[i]` reference, so no
           iteration either -- see README), a typed array's elements ARE plain scalar values, so
           iteration works exactly like an ordinary array's. */
        AerTypedArray* ta = aer_as_typed_array(col);
        if ((uint64_t)idx >= ta->count) {
            ip = (unsigned int)end_target;
            DISPATCH();
        }
        unsigned int width = vm_typed_elem_width(ta->elem_kind);
        vm->registers[item_dest_reg] = vm_typed_elem_read(ta->data + (size_t)idx * width, ta->elem_kind);
        vm->registers[idx_reg]       = aer_int(idx + 1);
        DISPATCH();
    }
    if (aer_type(col) != TYPE_ARRAY) {
        error("'for x in ...' only supports arrays, dicts, strings, and typed arrays");
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
    int col_reg       = (int)UNPACK_A(op_word);
    int idx_reg       = (int)UNPACK_B(op_word);
    int key_dest_reg  = (int)UNPACK_C(op_word);
    int val_dest_reg  = (int)READ();
    int end_target    = READ();
    AerVal col = vm->registers[col_reg];
    if (aer_type(col) != TYPE_DICT) {
        error("for k, v requires a hashtable");
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
    vm->registers[val_dest_reg] = d->map.dense[idx].payload;
    vm->registers[idx_reg]      = aer_int(idx + 1);
    gc_maybe_collect(vm);   /* vm_dict_next_key's owned-copy key string allocates */
    DISPATCH();
}

/* Runs once before the loop -- same checks as OP_ITER_RANGE_LOOP below, but never advances
   cur_reg (nothing to prepare yet). */
lbl_iter_range_prep: {
    int cur_reg       = (int)UNPACK_A(op_word);
    int end_reg       = (int)UNPACK_B(op_word);
    int step_reg      = (int)UNPACK_C(op_word);
    int item_dest_reg = (int)READ();
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
    /* Precomputes the iteration count once (ceiling division, matching Lua's FORLOOP) instead of
       re-deriving it every dispatch. count==0 means empty range. */
    bool ascending = cur < rng_end;
    int64_t diff  = ascending ? (rng_end - cur) : (cur - rng_end);
    /* step==1 (the common case) skips the division -- this target has no fast hardware divide,
       and short re-entered loops pay that cost often enough for it to be a real win. */
    int64_t count = (step == 1) ? diff : (diff + step - 1) / step;
    if (count == 0) {
        ip = (unsigned int)empty_target;
        DISPATCH();
    }
    /* end_reg/step_reg repurposed for this loop's life -- arg_materialize's snapshot guarantees
       they're fresh, loop-owned registers nothing else reads. */
    vm->registers[end_reg]       = aer_int(count - 1);                 /* iterations remaining AFTER this one */
    vm->registers[step_reg]      = aer_int(ascending ? step : -step);  /* direction baked in once, not re-inferred every iteration */
    vm->registers[item_dest_reg] = cur_v;
    DISPATCH();
}

/* Runs at the bottom of the loop body. end_reg/step_reg hold PREP's repurposed countdown/signed
   step, not the original bound -- so no fresh comparison against the original limit is ever
   needed (Lua's FORLOOP shape). Advances cur_reg by the already-signed step; on exhaustion,
   falls through leaving cur_reg/item_dest_reg at their last value. body_target is always a
   resolved address, never a patch placeholder. No re-validation -- PREP already checked once and
   the snapshot can't have changed. No gc_maybe_collect -- aer_int is a plain construction. */
lbl_iter_range_loop: {
    int cur_reg         = (int)UNPACK_A(op_word);
    int remaining_reg   = (int)UNPACK_B(op_word);
    int signed_step_reg = (int)UNPACK_C(op_word);
    int item_dest_reg   = (int)READ();
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
    /* range-for's own dedicated back-edge -- lbl_jump's check doesn't cover this loop shape since
       it never goes through a plain OP_JUMP. */
    if (max_instructions && --slice_budget == 0) { vm->ip = ip; return VM_SLICE_YIELDED; }
    DISPATCH();
}

/* chunk_find_shape() by name, arity check, one struct_pool allocation, trailing fields
   default-filled. */
lbl_struct_new: {
    int dest_reg           = (int)UNPACK_A(op_word);
    int arg_reg_base       = (int)UNPACK_B(op_word);
    int arg_count          = (int)UNPACK_C(op_word);
    int type_name_pool_idx = (int)READ();
    const char* name = aer_as_string(c->pool[type_name_pool_idx])->data;
    Shape* shape = chunk_find_shape(c, name);
    if (!shape) { error("'%s' is not defined", name); DISPATCH(); }
    if ((unsigned int)arg_count > shape->field_count) {
        error("'%s' takes at most %u argument%s, got %d",
              name, shape->field_count, shape->field_count == 1 ? "" : "s", arg_count);
        DISPATCH();
    }
    /* Positional args must match a typed field too -- the fused field-arithmetic fast path trusts
       every field unconditionally, not just ones set via `.field =`. Checked before allocating. */
    for (int i = 0; i < arg_count; i++) {
        ValueType declared = shape->field_types[i];
        if (declared != TYPE_ANY && vm->registers[arg_reg_base + i].tag != declared) {
            error("'%s' field '%s' is declared as a fixed type and cannot be constructed with a different type",
                  name, aer_as_string(c->pool[shape->field_names[i]])->data);
            DISPATCH();
        }
    }
    AerStruct* s = heap_alloc(&vm->heap, &vm->heap.struct_pool);
    s->shape  = shape;
    s->fields = (unsigned char*)s + sizeof(AerStruct);
    for (int i = 0; i < arg_count; i++)
        vm_struct_field_write(s, (unsigned int)i, vm->registers[arg_reg_base + i]);
    for (unsigned int i = (unsigned int)arg_count; i < shape->field_count; i++)
        vm_struct_field_write(s, i, vm_default_value(vm, shape->field_defaults[i]));
    vm->registers[dest_reg] = aer_struct_val(s);
    gc_maybe_collect(vm);   /* pool_alloc(&struct_pool) above, plus any vm_default_value array/dict defaults — all rooted now that the struct itself is stored */
    DISPATCH();
}

/* Reads struct_reg from a register instead of popping the stack. */
lbl_field_get: {
    unsigned int site = ip - 1;
    int dest_reg   = (int)UNPACK_A(op_word);
    int struct_reg = (int)UNPACK_B(op_word);
    int field_idx  = (int)READ();
    AerStruct* oa; int slot; unsigned int foffset; ValueType ftype;
    if (!vm_resolve_field(vm, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &ftype)) DISPATCH();
    vm->registers[dest_reg] = vm_struct_field_read_at(oa, foffset, ftype);
    DISPATCH();
}

/* Feeds the field value straight into the binary op instead of a register first. bin_op is a
   runtime value here, so the int/int and real/real cases still route through vm_binary_fast,
   falling back to vm_binary_cold for anything else. */
lbl_binary_field: {
    unsigned int site   = ip - 1;
    int dest_reg   = (int)UNPACK_A(op_word);
    int struct_reg = (int)UNPACK_B(op_word);
    Opcode bin_op  = (Opcode)UNPACK_C(op_word);
    uint32_t field_rk_word = READ();
    int field_idx  = (int)UNPACK_2X16_HI(field_rk_word);
    AerVal* lhs = vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerStruct* oa; int slot; unsigned int foffset; ValueType ftype;
    if (!vm_resolve_field(vm, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &ftype)) DISPATCH();
    AerVal rhs = vm_struct_field_read_at(oa, foffset, ftype);
    ValueType ta = aer_type(*lhs), tb = aer_type(rhs);
    bool handled;
    vm->registers[dest_reg] = vm_binary_fast(*lhs, rhs, bin_op, ta, tb, &handled);
    /* Only needed on the cold path (string concat) -- the fast path never allocates. */
    if (!handled) {
        vm->registers[dest_reg] = vm_binary_cold(c, *lhs, rhs, bin_op, ta, tb);
        gc_maybe_collect(vm);
    }
    DISPATCH();
}

/* Mirror of lbl_binary_field for the other operand order. */
lbl_field_binary: {
    unsigned int site   = ip - 1;
    int dest_reg   = (int)UNPACK_A(op_word);
    int struct_reg = (int)UNPACK_B(op_word);
    Opcode bin_op  = (Opcode)UNPACK_C(op_word);
    uint32_t field_rk_word = READ();
    int field_idx  = (int)UNPACK_2X16_HI(field_rk_word);
    AerVal* rhs = vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerStruct* oa; int slot; unsigned int foffset; ValueType ftype;
    if (!vm_resolve_field(vm, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &ftype)) DISPATCH();
    AerVal lhs = vm_struct_field_read_at(oa, foffset, ftype);
    ValueType ta = aer_type(lhs), tb = aer_type(*rhs);
    bool handled;
    vm->registers[dest_reg] = vm_binary_fast(lhs, *rhs, bin_op, ta, tb, &handled);
    if (!handled) {
        vm->registers[dest_reg] = vm_binary_cold(c, lhs, *rhs, bin_op, ta, tb);
        gc_maybe_collect(vm);
    }
    DISPATCH();
}

/* `struct.field OP= rhs` -- resolves the field exactly ONCE (one vm_resolve_field/cache lookup),
   reads it, computes, type-checks, and writes back, instead of the two full field resolutions
   (OP_FIELD_BINARY's read + a separate OP_FIELD_SET's write) this used to compile to. */
lbl_field_compound: {
    unsigned int site   = ip - 1;
    int struct_reg = (int)UNPACK_A(op_word);
    Opcode bin_op  = (Opcode)UNPACK_B(op_word);
    uint32_t field_rk_word = READ();
    int field_idx  = (int)UNPACK_2X16_HI(field_rk_word);
    AerVal* rhs = vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerStruct* oa; int slot; unsigned int foffset; ValueType ftype;
    if (!vm_resolve_field(vm, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &ftype)) DISPATCH();
    AerVal lhs = vm_struct_field_read_at(oa, foffset, ftype);
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
    gc_barrier_struct(vm, oa, result);
    vm_struct_field_write_at(oa, foffset, ftype, result);
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

/* gc_barrier_struct is the write barrier every mutating struct field-set needs. */
lbl_field_set: {
    unsigned int site = ip - 1;
    int struct_reg = (int)UNPACK_A(op_word);
    AerVal* val = vm_rk_ptr16(vm, const_pool, UNPACK_W16(op_word));
    int field_idx  = (int)READ();
    AerStruct* oa; int slot; unsigned int foffset; ValueType declared;
    if (!vm_resolve_field(vm, c, site, struct_reg, field_idx, &oa, &slot, &foffset, &declared)) DISPATCH();
    /* Enforced once here (the only place a field's value changes), trusted everywhere else
       including the fused fast path. TYPE_ANY means untyped. */
    if (declared != TYPE_ANY && val->tag != declared) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    gc_barrier_struct(vm, oa, *val);
    vm_struct_field_write_at(oa, foffset, declared, *val);
    DISPATCH();
}

/* Shape-specialized field access (OP_INDEX_FIELD_GET_RAW_INT/REAL, OP_FIELD_GET_RAW_INT/REAL,
   OP_INDEX_FIELD_SET_RAW_INT/REAL, OP_FIELD_SET_RAW_INT/REAL) -- only ever appears in a
   ChunkFunction's SPECIALIZED body (see ChunkFunction.specializations, lbl_call's dispatch above), where
   the parser already resolved the field's byte offset at COMPILE time against a Shape it proved
   for one parameter. No vm_resolve_field_by_shape call, no inline-cache lookup, no boxed AerVal
   ever constructed on the read side -- the 8 raw bytes at the field's offset are memcpy'd straight
   into/out of a raw_ints[]/raw_reals[] slot, since a typed field's storage (vm_packed_slot_read/
   write's own layout) is already bit-for-bit identical to a raw slot's.

   The aer_type() check on the container below is a deliberate defensive safety net, not a cost
   the design is supposed to need: the parser clears reg_known_shape[] on every reassignment path
   it currently knows to guard (plain and compound assignment to an existing name, parser.c), but
   that audit isn't yet proven exhaustive against every way a name's value can change (destructuring,
   for-loop rebinding, etc.) -- until it is, trusting the compile-time proof unconditionally would
   turn a compiler coverage gap into silent memory corruption instead of a clean, loud error. Revisit
   once that audit is complete. */
lbl_index_field_get_raw_int: {
    int dest_slot = (int)UNPACK_A(op_word);
    int arr_reg   = (int)UNPACK_B(op_word);
    uint32_t field_rk_word = READ();
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerVal obj = vm->registers[arr_reg];
    if (aer_type(obj) != TYPE_PACKED_ARRAY) { error("internal error: specialized packed-array field access on a non-packed-array value"); DISPATCH(); }
    AerPackedArray* pa = aer_as_packed_array(obj);
    if (aer_type(*idx) != TYPE_INTEGER) { error("Array index must be an integer"); DISPATCH(); }
    int64_t i = aer_as_int(*idx);
    if (i < 0) i += (int64_t)pa->count;
    if (i < 0 || (uint64_t)i >= pa->count) {
        error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(*idx), pa->count);
        DISPATCH();
    }
    unsigned int element_size = pa->shape->field_count * 8;
    unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
    memcpy(&vm->raw_ints[dest_slot], elem, 8);
    DISPATCH();
}

lbl_index_field_get_raw_real: {
    int dest_slot = (int)UNPACK_A(op_word);
    int arr_reg   = (int)UNPACK_B(op_word);
    uint32_t field_rk_word = READ();
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerVal obj = vm->registers[arr_reg];
    if (aer_type(obj) != TYPE_PACKED_ARRAY) { error("internal error: specialized packed-array field access on a non-packed-array value"); DISPATCH(); }
    AerPackedArray* pa = aer_as_packed_array(obj);
    if (aer_type(*idx) != TYPE_INTEGER) { error("Array index must be an integer"); DISPATCH(); }
    int64_t i = aer_as_int(*idx);
    if (i < 0) i += (int64_t)pa->count;
    if (i < 0 || (uint64_t)i >= pa->count) {
        error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(*idx), pa->count);
        DISPATCH();
    }
    unsigned int element_size = pa->shape->field_count * 8;
    unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
    memcpy(&vm->raw_reals[dest_slot], elem, 8);
    DISPATCH();
}

lbl_field_get_raw_int: {
    int dest_slot  = (int)UNPACK_A(op_word);
    int struct_reg = (int)UNPACK_B(op_word);
    unsigned int foffset = READ();
    AerVal obj = vm->registers[struct_reg];
    if (aer_type(obj) != TYPE_STRUCT) { error("internal error: specialized struct field access on a non-struct value"); DISPATCH(); }
    AerStruct* oa = aer_as_struct(obj);
    memcpy(&vm->raw_ints[dest_slot], oa->fields + foffset, 8);
    DISPATCH();
}

lbl_field_get_raw_real: {
    int dest_slot  = (int)UNPACK_A(op_word);
    int struct_reg = (int)UNPACK_B(op_word);
    unsigned int foffset = READ();
    AerVal obj = vm->registers[struct_reg];
    if (aer_type(obj) != TYPE_STRUCT) { error("internal error: specialized struct field access on a non-struct value"); DISPATCH(); }
    AerStruct* oa = aer_as_struct(obj);
    memcpy(&vm->raw_reals[dest_slot], oa->fields + foffset, 8);
    DISPATCH();
}

lbl_index_field_set_raw_int: {
    int obj_reg = (int)UNPACK_A(op_word);
    AerVal* idx = vm_rk_ptr16(vm, const_pool, UNPACK_W16(op_word));
    uint32_t off_slot_word = READ();
    unsigned int foffset = UNPACK_2X16_HI(off_slot_word);
    int src_slot          = (int)UNPACK_2X16_LO(off_slot_word);
    AerVal obj = vm->registers[obj_reg];
    if (aer_type(obj) != TYPE_PACKED_ARRAY) { error("internal error: specialized packed-array field access on a non-packed-array value"); DISPATCH(); }
    AerPackedArray* pa = aer_as_packed_array(obj);
    if (aer_type(*idx) != TYPE_INTEGER) { error("Array index must be an integer"); DISPATCH(); }
    int64_t i = aer_as_int(*idx);
    if (i < 0) i += (int64_t)pa->count;
    if (i < 0 || (uint64_t)i >= pa->count) {
        error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(*idx), pa->count);
        DISPATCH();
    }
    unsigned int element_size = pa->shape->field_count * 8;
    unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
    memcpy(elem, &vm->raw_ints[src_slot], 8);
    DISPATCH();
}

lbl_index_field_set_raw_real: {
    int obj_reg = (int)UNPACK_A(op_word);
    AerVal* idx = vm_rk_ptr16(vm, const_pool, UNPACK_W16(op_word));
    uint32_t off_slot_word = READ();
    unsigned int foffset = UNPACK_2X16_HI(off_slot_word);
    int src_slot          = (int)UNPACK_2X16_LO(off_slot_word);
    AerVal obj = vm->registers[obj_reg];
    if (aer_type(obj) != TYPE_PACKED_ARRAY) { error("internal error: specialized packed-array field access on a non-packed-array value"); DISPATCH(); }
    AerPackedArray* pa = aer_as_packed_array(obj);
    if (aer_type(*idx) != TYPE_INTEGER) { error("Array index must be an integer"); DISPATCH(); }
    int64_t i = aer_as_int(*idx);
    if (i < 0) i += (int64_t)pa->count;
    if (i < 0 || (uint64_t)i >= pa->count) {
        error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(*idx), pa->count);
        DISPATCH();
    }
    unsigned int element_size = pa->shape->field_count * 8;
    unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
    memcpy(elem, &vm->raw_reals[src_slot], 8);
    DISPATCH();
}

lbl_field_set_raw_int: {
    int struct_reg = (int)UNPACK_A(op_word);
    unsigned int foffset = READ();
    int src_slot = (int)READ();
    AerVal obj = vm->registers[struct_reg];
    if (aer_type(obj) != TYPE_STRUCT) { error("internal error: specialized struct field access on a non-struct value"); DISPATCH(); }
    AerStruct* oa = aer_as_struct(obj);
    memcpy(oa->fields + foffset, &vm->raw_ints[src_slot], 8);
    DISPATCH();
}

lbl_field_set_raw_real: {
    int struct_reg = (int)UNPACK_A(op_word);
    unsigned int foffset = READ();
    int src_slot = (int)READ();
    AerVal obj = vm->registers[struct_reg];
    if (aer_type(obj) != TYPE_STRUCT) { error("internal error: specialized struct field access on a non-struct value"); DISPATCH(); }
    AerStruct* oa = aer_as_struct(obj);
    memcpy(oa->fields + foffset, &vm->raw_reals[src_slot], 8);
    DISPATCH();
}

/* Raw-rhs compound assignment (`field += <already-raw expr>`) for the 4 combinations of
   {bare struct, packed-array-fused index} x {int, real} -- same specialized-body-only contract
   and same defensive aer_type() safety net as the GET/SET family above, but folding read + compute
   + write into one dispatch with NEITHER side ever boxed: the field's current value is read
   straight from its raw bytes, combined with rhs_slot's raw value via a 3-way switch (ADD/SUB/MUL
   only -- matching the _BOXED compound family's own restriction; the parser never emits this
   opcode for /=, %=, //=), and written back raw. Reachable only when the parser proved the RHS was
   ALREADY a raw value at the compound-assignment site (a raw local, or the result of
   try_emit_arith_raw_boxed) -- see parse_chain_assignment's compound-assignment branches. */
lbl_field_compound_raw_int: {
    int struct_reg = (int)UNPACK_A(op_word);
    Opcode bin_op  = (Opcode)UNPACK_B(op_word);
    unsigned int foffset = READ();
    int rhs_slot = (int)READ();
    AerVal obj = vm->registers[struct_reg];
    if (aer_type(obj) != TYPE_STRUCT) { error("internal error: specialized struct field access on a non-struct value"); DISPATCH(); }
    AerStruct* oa = aer_as_struct(obj);
    int64_t lhs; memcpy(&lhs, oa->fields + foffset, 8);
    int64_t rhs = vm->raw_ints[rhs_slot];
    int64_t result;
    switch (bin_op) {
        case OP_ADD: result = lhs + rhs; break;
        case OP_SUB: result = lhs - rhs; break;
        case OP_MUL: result = lhs * rhs; break;
        default: error("internal error: unsupported raw compound-assign op"); DISPATCH();
    }
    memcpy(oa->fields + foffset, &result, 8);
    DISPATCH();
}

lbl_field_compound_raw_real: {
    int struct_reg = (int)UNPACK_A(op_word);
    Opcode bin_op  = (Opcode)UNPACK_B(op_word);
    unsigned int foffset = READ();
    int rhs_slot = (int)READ();
    AerVal obj = vm->registers[struct_reg];
    if (aer_type(obj) != TYPE_STRUCT) { error("internal error: specialized struct field access on a non-struct value"); DISPATCH(); }
    AerStruct* oa = aer_as_struct(obj);
    double lhs; memcpy(&lhs, oa->fields + foffset, 8);
    double rhs = vm->raw_reals[rhs_slot];
    double result;
    switch (bin_op) {
        case OP_ADD: result = lhs + rhs; break;
        case OP_SUB: result = lhs - rhs; break;
        case OP_MUL: result = lhs * rhs; break;
        default: error("internal error: unsupported raw compound-assign op"); DISPATCH();
    }
    memcpy(oa->fields + foffset, &result, 8);
    DISPATCH();
}

lbl_index_field_compound_raw_int: {
    int arr_reg   = (int)UNPACK_A(op_word);
    Opcode bin_op = (Opcode)UNPACK_B(op_word);
    uint32_t field_rk_word = READ();
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(field_rk_word));
    int rhs_slot = (int)READ();
    AerVal obj = vm->registers[arr_reg];
    if (aer_type(obj) != TYPE_PACKED_ARRAY) { error("internal error: specialized packed-array field access on a non-packed-array value"); DISPATCH(); }
    AerPackedArray* pa = aer_as_packed_array(obj);
    if (aer_type(*idx) != TYPE_INTEGER) { error("Array index must be an integer"); DISPATCH(); }
    int64_t i = aer_as_int(*idx);
    if (i < 0) i += (int64_t)pa->count;
    if (i < 0 || (uint64_t)i >= pa->count) {
        error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(*idx), pa->count);
        DISPATCH();
    }
    unsigned int element_size = pa->shape->field_count * 8;
    unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
    int64_t lhs; memcpy(&lhs, elem, 8);
    int64_t rhs = vm->raw_ints[rhs_slot];
    int64_t result;
    switch (bin_op) {
        case OP_ADD: result = lhs + rhs; break;
        case OP_SUB: result = lhs - rhs; break;
        case OP_MUL: result = lhs * rhs; break;
        default: error("internal error: unsupported raw compound-assign op"); DISPATCH();
    }
    memcpy(elem, &result, 8);
    DISPATCH();
}

lbl_index_field_compound_raw_real: {
    int arr_reg   = (int)UNPACK_A(op_word);
    Opcode bin_op = (Opcode)UNPACK_B(op_word);
    uint32_t field_rk_word = READ();
    unsigned int foffset = UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(field_rk_word));
    int rhs_slot = (int)READ();
    AerVal obj = vm->registers[arr_reg];
    if (aer_type(obj) != TYPE_PACKED_ARRAY) { error("internal error: specialized packed-array field access on a non-packed-array value"); DISPATCH(); }
    AerPackedArray* pa = aer_as_packed_array(obj);
    if (aer_type(*idx) != TYPE_INTEGER) { error("Array index must be an integer"); DISPATCH(); }
    int64_t i = aer_as_int(*idx);
    if (i < 0) i += (int64_t)pa->count;
    if (i < 0 || (uint64_t)i >= pa->count) {
        error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(*idx), pa->count);
        DISPATCH();
    }
    unsigned int element_size = pa->shape->field_count * 8;
    unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
    double lhs; memcpy(&lhs, elem, 8);
    double rhs = vm->raw_reals[rhs_slot];
    double result;
    switch (bin_op) {
        case OP_ADD: result = lhs + rhs; break;
        case OP_SUB: result = lhs - rhs; break;
        case OP_MUL: result = lhs * rhs; break;
        default: error("internal error: unsupported raw compound-assign op"); DISPATCH();
    }
    memcpy(elem, &result, 8);
    DISPATCH();
}

/* Unconditional, untagged unbox -- see this opcode's own comment, vm.h. Only ever appears once per
   raw-bound parameter, at the very start of a specialized body's "raw-numeric variant"; lbl_call
   already proved the argument's runtime type before choosing to jump here, so there's nothing left
   to check. */
lbl_unbox_param_int: {
    int slot   = (int)UNPACK_A(op_word);
    int reg    = (int)UNPACK_B(op_word);
    vm->raw_ints[slot] = aer_as_int(vm->registers[reg]);
    DISPATCH();
}

lbl_unbox_param_real: {
    int slot   = (int)UNPACK_A(op_word);
    int reg    = (int)UNPACK_B(op_word);
    vm->raw_reals[slot] = aer_as_real(vm->registers[reg]);
    DISPATCH();
}

/* `[value; count]` -- replaces the old `Type[count]` (OP_PACKED_ARRAY_NEW) entirely. fill_reg has
   already been evaluated exactly once by the parser's own codegen; this handler just branches on
   its RUNTIME type. Eligibility (every field a fixed primitive) is checked here for the same reason
   the old opcode checked it here -- a Shape is only fully known once its OP_DEFINE_STRUCT has run. */
lbl_array_repeat: {
    int dest_reg    = (int)UNPACK_A(op_word);
    int fill_reg    = (int)UNPACK_B(op_word);
    int narrow_flag = (int)UNPACK_C(op_word);
    uint32_t count_word = READ();
    AerVal* count_v = vm_rk_ptr16(vm, const_pool, (uint16_t)count_word);
    if (aer_type(*count_v) != TYPE_INTEGER) { error("Repeat-literal array count must be an integer"); DISPATCH(); }
    int64_t count = aer_as_int(*count_v);
    if (count < 0) { error("Repeat-literal array count must not be negative"); DISPATCH(); }

    AerVal fill = vm->registers[fill_reg];
    if (aer_type(fill) == TYPE_STRUCT) {
        AerStruct* src = aer_as_struct(fill);
        Shape* shape = src->shape;
        for (unsigned int i = 0; i < shape->field_count; i++) {
            ValueType ft = shape->field_types[i];
            if (ft != TYPE_INTEGER && ft != TYPE_REAL && ft != TYPE_BOOLEAN) {
                const char* got = ft == TYPE_ANY ? "untyped (no annotation)"
                                : ft == TYPE_ARRAY ? "array"
                                : ft == TYPE_DICT ? "hashtable" : "string";
                error("'%s' cannot be packed into an array: field '%s' must be integer/float/boolean, not %s",
                      aer_as_string(c->pool[shape->name])->data,
                      aer_as_string(c->pool[shape->field_names[i]])->data, got);
                DISPATCH();
            }
        }
        unsigned int element_size = shape->field_count * 8;
        AerPackedArray* pa = heap_alloc(&vm->heap, &vm->heap.packed_array_pool);
        pa->count = (unsigned int)count;
        pa->shape = shape;
        /* malloc(0) is implementation-defined -- skip it for a zero-count array; bounds checks
           reject every later access anyway. */
        pa->data  = count > 0 ? xmalloc((size_t)count * (size_t)element_size) : NULL;
        /* Every eligible field is raw 8 bytes (TYPE_ANY, the only wider field kind, was already
           rejected above), so src->fields IS one element's worth of bytes, laid out identically --
           a straight memcpy per element, not a field-by-field copy. This is also the real capability
           gain over the old opcode: src's OWN field values are replicated, not the Shape's static
           defaults, so `[Particle(1.0, 2.0); n]` now differs from `[Particle(); n]`. */
        for (int64_t e = 0; e < count; e++)
            memcpy(pa->data + (size_t)e * element_size, src->fields, element_size);
        vm->registers[dest_reg] = aer_packed_array_val(pa);
    } else if (aer_type(fill) == TYPE_INTEGER || aer_type(fill) == TYPE_REAL) {
        /* narrow_flag is a pure parse-time decision (was the fill expression written as an
           `i`/`f`-suffixed literal directly in this position?) -- by construction, that always
           agrees with fill's own runtime tag (an `i`-suffixed literal is always a TYPE_INTEGER
           token, `f` always TYPE_REAL), so no cross-check against narrow_flag is needed here. */
        TypedArrayElemKind kind;
        if (narrow_flag == 1)      kind = TYPED_ELEM_INT32;
        else if (narrow_flag == 2) kind = TYPED_ELEM_FLOAT32;
        else                       kind = (aer_type(fill) == TYPE_INTEGER) ? TYPED_ELEM_INT64 : TYPED_ELEM_FLOAT64;
        unsigned int width = vm_typed_elem_width(kind);
        AerTypedArray* ta = heap_alloc(&vm->heap, &vm->heap.typed_array_pool);
        ta->count     = (unsigned int)count;
        ta->elem_kind = kind;
        ta->data      = count > 0 ? xmalloc((size_t)count * width) : NULL;
        for (int64_t e = 0; e < count; e++)
            vm_typed_elem_write(ta->data + (size_t)e * width, kind, fill);
        vm->registers[dest_reg] = aer_typed_array_val(ta);
    } else {
        error("Cannot build a repeat-literal array from a %s value -- the fill value must be a struct instance or a number", vm_type_name(c, fill));
        DISPATCH();
    }
    gc_maybe_collect(vm);
    DISPATCH();
}

/* Handles both a packed array and an ordinary struct array (the parser can't know which --
   functions are untyped). The non-packed branch reproduces the plain index+field path exactly,
   just without needing a scratch register for the intermediate. */
lbl_index_field_get: {
    unsigned int site = ip - 1;
    int dest_reg  = (int)UNPACK_A(op_word);
    int obj_reg   = (int)UNPACK_B(op_word);
    uint32_t field_rk_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_rk_word);
    AerVal* idx = vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(field_rk_word));
    AerVal obj = vm->registers[obj_reg];
    if (aer_type(obj) == TYPE_PACKED_ARRAY) {
        AerPackedArray* pa = aer_as_packed_array(obj);
        if (aer_type(*idx) != TYPE_INTEGER) { error("Array index must be an integer"); DISPATCH(); }
        int64_t i = aer_as_int(*idx);
        if (i < 0) i += (int64_t)pa->count;
        if (i < 0 || (uint64_t)i >= pa->count) {
            error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(*idx), pa->count);
            DISPATCH();
        }
        int slot; unsigned int foffset; ValueType ftype;
        if (!vm_resolve_field_by_shape(c, site, pa->shape, field_idx, &slot, &foffset, &ftype)) DISPATCH();
        unsigned int element_size = pa->shape->field_count * 8;
        unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
        vm->registers[dest_reg] = vm_packed_slot_read(elem, ftype);
        DISPATCH();
    }
    AerVal tmp;
    vm_index_get_compute(obj, *idx, &tmp);
    if (aer_type(obj) == TYPE_STRING) gc_maybe_collect(vm);   /* single-char string indexing allocates */
    if (aer_type(tmp) != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerStruct* oa = aer_as_struct(tmp);
    int slot; unsigned int foffset; ValueType ftype;
    if (!vm_resolve_field_by_shape(c, site, oa->shape, field_idx, &slot, &foffset, &ftype)) DISPATCH();
    vm->registers[dest_reg] = vm_struct_field_read_at(oa, foffset, ftype);
    DISPATCH();
}

/* Mirror of lbl_index_field_get -- same dual dispatch, same reason no scratch register is needed. */
lbl_index_field_set: {
    unsigned int site   = ip - 1;
    int obj_reg         = (int)UNPACK_A(op_word);
    AerVal* idx = vm_rk_ptr16(vm, const_pool, UNPACK_W16(op_word));
    uint32_t field_val_word = READ();
    int field_idx       = (int)UNPACK_2X16_HI(field_val_word);
    AerVal* val = vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(field_val_word));
    AerVal obj = vm->registers[obj_reg];
    if (aer_type(obj) == TYPE_PACKED_ARRAY) {
        AerPackedArray* pa = aer_as_packed_array(obj);
        if (aer_type(*idx) != TYPE_INTEGER) { error("Array index must be an integer"); DISPATCH(); }
        int64_t i = aer_as_int(*idx);
        if (i < 0) i += (int64_t)pa->count;
        if (i < 0 || (uint64_t)i >= pa->count) {
            error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(*idx), pa->count);
            DISPATCH();
        }
        int slot; unsigned int foffset; ValueType declared;
        if (!vm_resolve_field_by_shape(c, site, pa->shape, field_idx, &slot, &foffset, &declared)) DISPATCH();
        if (val->tag != declared) {
            error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
                  aer_as_string(c->pool[field_idx])->data);
            DISPATCH();
        }
        unsigned int element_size = pa->shape->field_count * 8;
        unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
        vm_packed_slot_write(elem, declared, *val);
        DISPATCH();
    }
    AerVal tmp;
    vm_index_get_compute(obj, *idx, &tmp);
    if (aer_type(obj) == TYPE_STRING) gc_maybe_collect(vm);   /* single-char string indexing allocates */
    if (aer_type(tmp) != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerStruct* oa = aer_as_struct(tmp);
    int slot; unsigned int foffset; ValueType declared;
    if (!vm_resolve_field_by_shape(c, site, oa->shape, field_idx, &slot, &foffset, &declared)) DISPATCH();
    if (declared != TYPE_ANY && val->tag != declared) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    gc_barrier_struct(vm, oa, *val);
    vm_struct_field_write_at(oa, foffset, declared, *val);
    DISPATCH();
}

/* `obj[index].field OP= rk_rhs` -- resolves the index+field exactly once (same dual packed-array/
   struct-instance dispatch as lbl_index_field_get/set), reads, computes, type-checks, and writes
   back in one dispatch. Before this existed, the parser emitted OP_INDEX_FIELD_GET (read) followed
   by a separate OP_INDEX_FIELD_SET (a second index+field resolution just to write the same slot
   back) -- exactly the nbody-style `bodies[j].vx += dx * mi` pattern, twice resolved for one
   logical operation. */
lbl_index_field_compound: {
    unsigned int site   = ip - 1;
    int obj_reg   = (int)UNPACK_A(op_word);
    Opcode bin_op = (Opcode)UNPACK_B(op_word);
    uint32_t field_idx_word = READ();
    int field_idx = (int)UNPACK_2X16_HI(field_idx_word);
    AerVal* idx = vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(field_idx_word));
    uint32_t rhs_word = READ();
    AerVal* rhs = vm_rk_ptr16(vm, const_pool, UNPACK_2X16_LO(rhs_word));
    AerVal obj = vm->registers[obj_reg];
    if (aer_type(obj) == TYPE_PACKED_ARRAY) {
        AerPackedArray* pa = aer_as_packed_array(obj);
        if (aer_type(*idx) != TYPE_INTEGER) { error("Array index must be an integer"); DISPATCH(); }
        int64_t i = aer_as_int(*idx);
        if (i < 0) i += (int64_t)pa->count;
        if (i < 0 || (uint64_t)i >= pa->count) {
            error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(*idx), pa->count);
            DISPATCH();
        }
        int slot; unsigned int foffset; ValueType ftype;
        if (!vm_resolve_field_by_shape(c, site, pa->shape, field_idx, &slot, &foffset, &ftype)) DISPATCH();
        unsigned int element_size = pa->shape->field_count * 8;
        unsigned char* elem = pa->data + (size_t)i * element_size + foffset;
        AerVal lhs = vm_packed_slot_read(elem, ftype);
        bool handled;
        AerVal result = vm_binary_fast(lhs, *rhs, bin_op, ftype, aer_type(*rhs), &handled);
        if (!handled) { result = vm_binary_cold(c, lhs, *rhs, bin_op, ftype, aer_type(*rhs)); gc_maybe_collect(vm); }
        if (result.tag != ftype) {
            error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
                  aer_as_string(c->pool[field_idx])->data);
            DISPATCH();
        }
        vm_packed_slot_write(elem, ftype, result);
        DISPATCH();
    }
    AerVal tmp;
    vm_index_get_compute(obj, *idx, &tmp);
    if (aer_type(obj) == TYPE_STRING) gc_maybe_collect(vm);   /* single-char string indexing allocates */
    if (aer_type(tmp) != TYPE_STRUCT) {
        error("'.' field access requires a struct instance");
        DISPATCH();
    }
    AerStruct* oa = aer_as_struct(tmp);
    int slot; unsigned int foffset; ValueType ftype;
    if (!vm_resolve_field_by_shape(c, site, oa->shape, field_idx, &slot, &foffset, &ftype)) DISPATCH();
    AerVal lhs = vm_struct_field_read_at(oa, foffset, ftype);
    ValueType ta = aer_type(lhs), tb = aer_type(*rhs);
    bool handled;
    AerVal result = vm_binary_fast(lhs, *rhs, bin_op, ta, tb, &handled);
    if (!handled) { result = vm_binary_cold(c, lhs, *rhs, bin_op, ta, tb); gc_maybe_collect(vm); }
    if (ftype != TYPE_ANY && result.tag != ftype) {
        error("Field '%s' is declared as a fixed type and cannot be assigned a different type",
              aer_as_string(c->pool[field_idx])->data);
        DISPATCH();
    }
    gc_barrier_struct(vm, oa, result);
    vm_struct_field_write_at(oa, foffset, ftype, result);
    DISPATCH();
}

/* Keyed by unary_op, same tag convention as lbl_binary. Also folds in OP_TO_STR
   (interpolation's string conversion) via the shared vm_to_str(). */
lbl_unary: {
    int dest        = (int)UNPACK_A(op_word);
    Opcode unary_op = (Opcode)UNPACK_B(op_word);
    AerVal v = *vm_rk_ptr8(vm, const_pool, UNPACK_C(op_word));
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
            /* The only allocating case -- NEGATE/NOT/BITWISE_NOT only ever produce plain tagged-union
               values. */
            *result = vm_to_str(vm, v);
            gc_maybe_collect(vm);
            break;
        default:
            *result = aer_null();
            break;
    }
    DISPATCH();
}

/* No gc_maybe_collect -- every cast_type only ever produces a plain tagged-union value
   (confirmed by inspection, including vm_cast's string-parsing sub-cases). */
lbl_cast: {
    int dest      = (int)UNPACK_A(op_word);
    int cast_type = (int)UNPACK_B(op_word);
    AerVal v = *vm_rk_ptr8(vm, const_pool, UNPACK_C(op_word));
    vm->registers[dest] = vm_cast(v, cast_type);
    DISPATCH();
}

/* Skip both the RK-flag check and the tag check -- the parser already proved every operand's
   type at compile time. None allocate: integers/reals/booleans never have heap cells. */
lbl_raw_load_int: {
    int dest = (int)UNPACK_A(op_word);
    /* Full 32-bit signed immediate, its own dedicated word -- closes the old 20-bit-immediate
       truncation bug outright (a 20M-iteration bound once silently became 77056) rather than
       just widening it again. */
    vm->raw_ints[dest] = (int32_t)READ();
    DISPATCH();
}

lbl_raw_load_real: {
    int dest = (int)UNPACK_A(op_word);
    unsigned int pool_idx = (unsigned int)READ();
    vm->raw_reals[dest] = const_pool[pool_idx].as.d;
    DISPATCH();
}

/* Source-level dedup only, zero behavior/codegen change (each invocation below still expands to
   its own full label + body, byte-for-byte what was hand-written here before) -- NOT a NOINLINE
   shared-function refactor, which would trade a real per-dispatch call+branch cost on these
   extremely hot opcodes for a code-size win already confirmed not to matter (icache misses are
   16-140x rarer than dcache misses on every workload measured this session). #undef'd right after
   the last family that needs them. */
#define RAW_ARITH_INT(name, op) \
lbl_raw_##name##_int: { \
    int dest = (int)UNPACK_A(op_word); \
    int a    = (int)UNPACK_B(op_word); \
    int b    = (int)UNPACK_C(op_word); \
    vm->raw_ints[dest] = vm->raw_ints[a] op vm->raw_ints[b]; \
    DISPATCH(); \
}
#define RAW_ARITH_REAL(name, op) \
lbl_raw_##name##_real: { \
    int dest = (int)UNPACK_A(op_word); \
    int a    = (int)UNPACK_B(op_word); \
    int b    = (int)UNPACK_C(op_word); \
    vm->raw_reals[dest] = vm->raw_reals[a] op vm->raw_reals[b]; \
    DISPATCH(); \
}
#define RAW_CMP_INT(name, op) \
lbl_raw_##name##_int: { \
    int dest = (int)UNPACK_A(op_word); \
    int a    = (int)UNPACK_B(op_word); \
    int b    = (int)UNPACK_C(op_word); \
    vm->registers[dest] = aer_bool(vm->raw_ints[a] op vm->raw_ints[b]); \
    DISPATCH(); \
}
#define RAW_CMP_REAL(name, op) \
lbl_raw_##name##_real: { \
    int dest = (int)UNPACK_A(op_word); \
    int a    = (int)UNPACK_B(op_word); \
    int b    = (int)UNPACK_C(op_word); \
    vm->registers[dest] = aer_bool(vm->raw_reals[a] op vm->raw_reals[b]); \
    DISPATCH(); \
}
/* A runtime tag check decides: matching type accumulates in place (safe every iteration, the
   slot's identity never changes); mismatched type is the same runtime error vm_binary_cold gives. */
#define RAW_ARITH_INT_BOXED(name, op, opstr) \
lbl_raw_##name##_int_boxed: { \
    int slot = (int)UNPACK_A(op_word); \
    int reg  = (int)UNPACK_B(op_word); \
    AerVal* rhs = &vm->registers[reg]; \
    if (rhs->tag != TYPE_INTEGER) { error("Cannot apply '" opstr "' to integer and %s", vm_type_name(c, *rhs)); DISPATCH(); } \
    vm->raw_ints[slot] op##= rhs->as.i; \
    DISPATCH(); \
}
/* An integer rhs promotes to real here (vm_promote_real's own rule) instead of erroring -- matches
   the fully-boxed path's own int/real mixing semantics (vm_binary_cold/fast), which this opcode is
   otherwise a drop-in replacement for. Without this, `real_value OP boxed_int_expr` (extremely
   common -- any loop counter or other plain int composed with a real, e.g. `i * 0.10`) would
   incorrectly reject a completely ordinary mixed-numeric expression the boxed path already
   supports. Only int/real mix; any other type still errors exactly as before. */
#define RAW_ARITH_REAL_BOXED(name, op, opstr) \
lbl_raw_##name##_real_boxed: { \
    int slot = (int)UNPACK_A(op_word); \
    int reg  = (int)UNPACK_B(op_word); \
    AerVal* rhs = &vm->registers[reg]; \
    if (rhs->tag == TYPE_REAL) vm->raw_reals[slot] op##= rhs->as.d; \
    else if (rhs->tag == TYPE_INTEGER) vm->raw_reals[slot] op##= (double)rhs->as.i; \
    else error("Cannot apply '" opstr "' to float and %s", vm_type_name(c, *rhs)); \
    DISPATCH(); \
}
/* Non-destructive counterpart of RAW_ARITH_REAL_BOXED -- (dest, src_raw, boxed_reg) instead of
   (slot, reg) in-place. src_raw is read-only here, letting a caller compose a permanent raw local
   with a boxed value into a FRESH slot without a defensive OP_RAW_MOVE_REAL first (see
   try_emit_arith_raw_boxed, parser.c). Error messages say '+'/'*' rather than the compound forms
   since this is always a general expression, never a compound assignment -- opstr is passed
   un-suffixed for exactly that reason (contrast RAW_ARITH_REAL_BOXED's "+="/"-="/"*="). */
#define RAW_ARITH_REAL_BOXED_TO(name, op, opstr) \
lbl_raw_##name##_real_boxed_to: { \
    int dest = (int)UNPACK_A(op_word); \
    int src  = (int)UNPACK_B(op_word); \
    int reg  = (int)UNPACK_C(op_word); \
    AerVal* rhs = &vm->registers[reg]; \
    if (rhs->tag == TYPE_REAL) vm->raw_reals[dest] = vm->raw_reals[src] op rhs->as.d; \
    else if (rhs->tag == TYPE_INTEGER) vm->raw_reals[dest] = vm->raw_reals[src] op (double)rhs->as.i; \
    else error("Cannot apply '" opstr "' to float and %s", vm_type_name(c, *rhs)); \
    DISPATCH(); \
}

RAW_ARITH_INT(add, +)
RAW_ARITH_INT(sub, -)
RAW_ARITH_INT(mul, *)

/* Matches OP_DIV's own semantics: int/int division always promotes to float, so this is the one
   OP_RAW_*_INT opcode whose dest is raw_reals[], not raw_ints[]. */
lbl_raw_div_int: {
    int dest = (int)UNPACK_A(op_word);
    int a    = (int)UNPACK_B(op_word);
    int b    = (int)UNPACK_C(op_word);
    int64_t rv = vm->raw_ints[b];
    if (rv == 0) { error("Division by zero"); vm->raw_reals[dest] = 0.0; }
    else vm->raw_reals[dest] = (double)vm->raw_ints[a] / (double)rv;
    DISPATCH();
}

lbl_raw_mod_int: {
    int dest = (int)UNPACK_A(op_word);
    int a    = (int)UNPACK_B(op_word);
    int b    = (int)UNPACK_C(op_word);
    int64_t rv = vm->raw_ints[b];
    if (rv == 0) { error("Modulo by zero"); vm->raw_ints[dest] = 0; }
    else vm->raw_ints[dest] = vm->raw_ints[a] % rv;
    DISPATCH();
}

lbl_raw_floor_div_int: {
    int dest = (int)UNPACK_A(op_word);
    int a    = (int)UNPACK_B(op_word);
    int b    = (int)UNPACK_C(op_word);
    int64_t rv = vm->raw_ints[b];
    if (rv == 0) { error("Division by zero"); vm->raw_ints[dest] = 0; }
    else vm->raw_ints[dest] = (int64_t)floor((double)vm->raw_ints[a] / (double)rv);
    DISPATCH();
}

RAW_ARITH_REAL(add, +)
RAW_ARITH_REAL(sub, -)
RAW_ARITH_REAL(mul, *)

lbl_raw_div_real: {
    int dest = (int)UNPACK_A(op_word);
    int a    = (int)UNPACK_B(op_word);
    int b    = (int)UNPACK_C(op_word);
    double rv = vm->raw_reals[b];
    if (rv == 0.0) { error("Division by zero"); vm->raw_reals[dest] = 0.0; }
    else vm->raw_reals[dest] = vm->raw_reals[a] / rv;
    DISPATCH();
}

/* Comparisons produce a boxed boolean (no raw boolean type exists) -- dest is 7 bits, not 5. */
RAW_CMP_INT(lt,  <)
RAW_CMP_INT(gt,  >)
RAW_CMP_INT(lte, <=)
RAW_CMP_INT(gte, >=)
RAW_CMP_REAL(lt,  <)
RAW_CMP_REAL(gt,  >)
RAW_CMP_REAL(lte, <=)
RAW_CMP_REAL(gte, >=)

/* The only bridge from raw storage back to a tagged AerVal register. */
lbl_box_int: {
    int dest = (int)UNPACK_A(op_word);
    int src  = (int)UNPACK_B(op_word);
    vm->registers[dest] = aer_int(vm->raw_ints[src]);
    DISPATCH();
}

lbl_box_real: {
    int dest = (int)UNPACK_A(op_word);
    int src  = (int)UNPACK_B(op_word);
    vm->registers[dest] = aer_real(vm->raw_reals[src]);
    DISPATCH();
}

lbl_raw_move_int: {
    int dest = (int)UNPACK_A(op_word);
    int src  = (int)UNPACK_B(op_word);
    vm->raw_ints[dest] = vm->raw_ints[src];
    DISPATCH();
}

lbl_raw_move_real: {
    int dest = (int)UNPACK_A(op_word);
    int src  = (int)UNPACK_B(op_word);
    vm->raw_reals[dest] = vm->raw_reals[src];
    DISPATCH();
}

RAW_ARITH_INT_BOXED(add, +, "+=")
RAW_ARITH_INT_BOXED(sub, -, "-=")
RAW_ARITH_INT_BOXED(mul, *, "*=")

RAW_ARITH_REAL_BOXED(add, +, "+=")
RAW_ARITH_REAL_BOXED(sub, -, "-=")
RAW_ARITH_REAL_BOXED(mul, *, "*=")

RAW_ARITH_REAL_BOXED_TO(add, +, "+")
RAW_ARITH_REAL_BOXED_TO(mul, *, "*")

#undef RAW_ARITH_INT
#undef RAW_ARITH_REAL
#undef RAW_CMP_INT
#undef RAW_CMP_REAL
#undef RAW_ARITH_INT_BOXED
#undef RAW_ARITH_REAL_BOXED
#undef RAW_ARITH_REAL_BOXED_TO

lbl_raw_load_int_pool: {
    int dest = (int)UNPACK_A(op_word);
    unsigned int pool_idx = (unsigned int)READ();
    vm->raw_ints[dest] = const_pool[pool_idx].as.i;
    DISPATCH();
}

/* rhs may legitimately be TYPE_REAL against an int raw slot (e.g. comparing against
   time.now()'s real result) -- the ordinary boxed comparison this replaces promotes int<->real
   too, so a hard type check here would be a real regression. */
lbl_raw_lt_int_boxed: {
    int dest = (int)UNPACK_A(op_word);
    int slot = (int)UNPACK_B(op_word);
    int reg  = (int)UNPACK_C(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_ints[slot] < rhs->as.i);
    else if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool((double)vm->raw_ints[slot] < rhs->as.d);
    else error("Cannot apply '<' to integer and %s", vm_type_name(c, *rhs));
    DISPATCH();
}

lbl_raw_gt_int_boxed: {
    int dest = (int)UNPACK_A(op_word);
    int slot = (int)UNPACK_B(op_word);
    int reg  = (int)UNPACK_C(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_ints[slot] > rhs->as.i);
    else if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool((double)vm->raw_ints[slot] > rhs->as.d);
    else error("Cannot apply '>' to integer and %s", vm_type_name(c, *rhs));
    DISPATCH();
}

lbl_raw_lte_int_boxed: {
    int dest = (int)UNPACK_A(op_word);
    int slot = (int)UNPACK_B(op_word);
    int reg  = (int)UNPACK_C(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_ints[slot] <= rhs->as.i);
    else if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool((double)vm->raw_ints[slot] <= rhs->as.d);
    else error("Cannot apply '<=' to integer and %s", vm_type_name(c, *rhs));
    DISPATCH();
}

lbl_raw_gte_int_boxed: {
    int dest = (int)UNPACK_A(op_word);
    int slot = (int)UNPACK_B(op_word);
    int reg  = (int)UNPACK_C(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_ints[slot] >= rhs->as.i);
    else if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool((double)vm->raw_ints[slot] >= rhs->as.d);
    else error("Cannot apply '>=' to integer and %s", vm_type_name(c, *rhs));
    DISPATCH();
}

lbl_raw_lt_real_boxed: {
    int dest = (int)UNPACK_A(op_word);
    int slot = (int)UNPACK_B(op_word);
    int reg  = (int)UNPACK_C(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool(vm->raw_reals[slot] < rhs->as.d);
    else if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_reals[slot] < (double)rhs->as.i);
    else error("Cannot apply '<' to float and %s", vm_type_name(c, *rhs));
    DISPATCH();
}

lbl_raw_gt_real_boxed: {
    int dest = (int)UNPACK_A(op_word);
    int slot = (int)UNPACK_B(op_word);
    int reg  = (int)UNPACK_C(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool(vm->raw_reals[slot] > rhs->as.d);
    else if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_reals[slot] > (double)rhs->as.i);
    else error("Cannot apply '>' to float and %s", vm_type_name(c, *rhs));
    DISPATCH();
}

lbl_raw_lte_real_boxed: {
    int dest = (int)UNPACK_A(op_word);
    int slot = (int)UNPACK_B(op_word);
    int reg  = (int)UNPACK_C(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool(vm->raw_reals[slot] <= rhs->as.d);
    else if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_reals[slot] <= (double)rhs->as.i);
    else error("Cannot apply '<=' to float and %s", vm_type_name(c, *rhs));
    DISPATCH();
}

lbl_raw_gte_real_boxed: {
    int dest = (int)UNPACK_A(op_word);
    int slot = (int)UNPACK_B(op_word);
    int reg  = (int)UNPACK_C(op_word);
    AerVal* rhs = &vm->registers[reg];
    if (rhs->tag == TYPE_REAL) vm->registers[dest] = aer_bool(vm->raw_reals[slot] >= rhs->as.d);
    else if (rhs->tag == TYPE_INTEGER) vm->registers[dest] = aer_bool(vm->raw_reals[slot] >= (double)rhs->as.i);
    else error("Cannot apply '>=' to float and %s", vm_type_name(c, *rhs));
    DISPATCH();
}

lbl_halt:
    runtime_error_unwind_target = saved_unwind_target;
    active_vm_for_errors        = saved_active_vm;
    current_heap                = saved_current_heap;
    return VM_SLICE_DONE;
}

bool vm_run(VM* vm) {
    return vm_run_slice(vm, 0) == VM_SLICE_DONE;
}

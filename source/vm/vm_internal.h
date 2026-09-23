#ifndef AER_VM_INTERNAL_H
#define AER_VM_INTERNAL_H

/* Shared by vm.c, call.c and handlers.c; nothing outside the VM includes it. */

#include <stdbool.h>
#include <stdint.h>
#include "vm.h"

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

/* Lazily sized per-chunk tables: hit counters (profiling only), the field inline cache, the call-site
   specialization cache, and the shape a struct-name constant names. */
void chunk_ensure_debug_hits(Chunk* c);
void chunk_ensure_field_cache(Chunk* c);
Shape* chunk_shape_for_pool_idx(Chunk* c, unsigned int pool_idx);
void chunk_ensure_call_spec_cache(Chunk* c);

/* Calls (call.c): a parameter or field default as a fresh value, the register-bank grow path, calls
   through a function value, builtins, the specialization a call site should enter, and native or
   file-module calls. */
AerVal vm_default_value(VM* vm, AerVal dflt);
bool vm_grow_for_push(VM* vm, AerVal* base);
void vm_call_value(VM* vm, AerVal fv, int dest_reg, int arg_reg_base, int arg_count, bool is_tail_call,
                   unsigned int return_ip);
bool vm_call_builtin(Chunk* c, int builtin_id, AerVal* args, int arg_count, AerVal* out);
void vm_call_resolve_specialization(Chunk* c, ChunkFunction* target_f, AerVal* registers, int arg_reg_base,
                                    unsigned int ip, unsigned int* chosen_offset,
                                    unsigned int* chosen_max_registers, unsigned short* chosen_frame_bounds);
bool vm_call_module_dispatch(VM* vm, Chunk* c, int module_idx, int fn_idx, int module_id, int fn_id,
                             int arg_count);

#endif

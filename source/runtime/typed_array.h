#ifndef AER_TYPED_ARRAY_H
#define AER_TYPED_ARRAY_H

#include <stdbool.h>
#include <string.h>
#include "heap.h"
#include "opcodes.h"
#include "value.h"

typedef struct Chunk Chunk;

/* 1 for boolean, 4 for int32/float32, 8 for int64/float64. Kept out of vm_packed_slot_read/write, whose
   hot path measured an ARM regression from exactly this branch. */
static inline unsigned int vm_typed_elem_width(TypedArrayElemKind kind) {
    if (kind == TYPED_ELEM_INT32 || kind == TYPED_ELEM_FLOAT32)
        return 4;
    return kind == TYPED_ELEM_BOOL ? 1 : 8;
}

/* A zeroed typed array. vm_new_typed_array_val wraps it as a value, for actor.receive(). */
AerTypedArray* vm_new_typed_array(TypedArrayElemKind kind, unsigned int count);
AerVal vm_new_typed_array_val(TypedArrayElemKind kind, unsigned int count);

/* A data buffer of exactly `size` bytes, from the heap's free-cache when one fits. */
unsigned char* typed_array_data_alloc(VmHeap* heap, size_t size);

/* (a op1 b) op2 cc in one pass over three same-kind, same-count arrays. */
AerVal vm_typed_array_chain2(AerTypedArray* a, AerTypedArray* b, AerTypedArray* cc, Opcode op1, Opcode op2);

/* A binary operator with a column on either side: elementwise, broadcast, or an error naming why not. */
AerVal vm_binary_column(Chunk* c, AerVal a, AerVal b, Opcode op, ValueType ta, ValueType tb);

/* True when the array can hold val; false after raising the error saying why not. */
bool vm_typed_array_check(Chunk* c, TypedArrayElemKind kind, AerVal val);

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

#endif

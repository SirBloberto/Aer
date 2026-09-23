#ifndef AER_VALUE_OPS_H
#define AER_VALUE_OPS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "objects.h"
#include "opcodes.h"
#include "value.h"

typedef struct Chunk Chunk;

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

/* `a in b`: a dict key, an array element, or a substring. */
AerVal vm_in(AerVal a, AerVal b);

/* Everything vm_binary_fast does not handle, for the per-operator handlers and the field-fusion
   opcodes. Out of line: the path is rare, and inlining it would copy the body into every caller. */
AerVal vm_binary_cold(Chunk* c, AerVal a, AerVal b, Opcode op, ValueType ta, ValueType tb);

/* Resolves a slice's optional bounds against len; false after raising the error for a bad one. */
bool vm_slice_bounds(AerVal start_v, AerVal end_v, int64_t len, int64_t* out_start, int64_t* out_end);

/* integer(x), float(x), boolean(x) -- see the conversion rules at the definition. */
AerVal vm_cast(AerVal v, int cast_type);

#endif

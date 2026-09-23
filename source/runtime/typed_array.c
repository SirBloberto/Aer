#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"
#include "heap.h"
#include "typed_array.h"
#include "value_format.h"

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

/* Checks the free-cache (heap.h's own comment on TypedArrayFreeSlot) for a buffer of EXACTLY this
   size before falling back to xmalloc -- linear scan over a handful of slots, cheap regardless of
   hit or miss. Claimed slots are cleared (size = 0) so a later free_typed_array (gc.c) can reuse
   them for a different buffer. */
unsigned char* typed_array_data_alloc(VmHeap* heap, size_t size) {
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

AerTypedArray* vm_new_typed_array(TypedArrayElemKind kind, unsigned int count) {
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

/* The caller has already checked that a, b and cc share an elem_kind and count, and that the kind
   is float. */
AerVal vm_typed_array_chain2(AerTypedArray* a, AerTypedArray* b, AerTypedArray* cc, Opcode op1,
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

/* Either side a column: elementwise between two, broadcast against a number, or an error that
   says which of those it was not. */
AerVal vm_binary_column(Chunk* c, AerVal a, AerVal b, Opcode op, ValueType ta, ValueType tb) {
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

    ValueType other = aer_type(a) == TYPE_TYPED_ARRAY ? tb : ta;
    if (other == TYPE_INTEGER || other == TYPE_REAL)
        error("'%s' is not defined on a column — columns support + - * / and the comparisons",
              binop_symbol(op));
    else
        error("'%s' needs a number or another column on the other side, not %s", binop_symbol(op),
              vm_type_name(c, aer_type(a) == TYPE_TYPED_ARRAY ? b : a));
    return aer_bool(false);
}

/* The indefinite article for a type or element name: "an integer", "an int32", "a float", "a Point". */
static const char* article_for(const char* name) {
    return name[0] != '\0' && strchr("aeiouAEIOU", name[0]) ? "an" : "a";
}

bool vm_typed_array_check(Chunk* c, TypedArrayElemKind kind, AerVal val) {
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

#include <stdlib.h>
#include <string.h>
#include "aer_stdlib.h"
#include "error.h"
#include "hashtable.h"

/* qsort() comparator for sort() -- only called once the caller has verified every element is TYPE_STRING
   or every element is numeric, so no type-mismatch case needs handling here. */
static int sort_cmp(const void* pa, const void* pb) {
    const AerVal* a = (const AerVal*)pa;
    const AerVal* b = (const AerVal*)pb;
    if (aer_type(*a) == TYPE_STRING)
        return aer_string_compare(aer_as_string(*a), aer_as_string(*b));
    double da = aer_type(*a) == TYPE_INTEGER ? (double)aer_as_int(*a) : aer_as_real(*a);
    double db = aer_type(*b) == TYPE_INTEGER ? (double)aer_as_int(*b) : aer_as_real(*b);
    return da < db ? -1 : (da > db ? 1 : 0);
}

/* Same treatment vm.c's typed-array kernels get: -O3 with vectorization on these loops only, rather
   than raising it for the whole file. */
#define REDUCE_ATTR __attribute__((optimize("O3", "tree-vectorize")))

/* Four running totals rather than one, so the loop has four independent dependency chains and can
   vectorize. That is a throughput trick, not an accuracy one -- what protects the answer is the
   WIDTH of the total. Summing float32 elements into a float32 total is 66% wrong by 1e8 elements,
   because the total outgrows the values still being added and they round away to nothing; the same
   loop accumulating into a double lands within 2e-12, and measures faster besides. */
#define DEFINE_SUM(name, ctype, acctype)                                                                     \
    REDUCE_ATTR static acctype name(const ctype* restrict a, unsigned int n) {                               \
        acctype t0 = 0, t1 = 0, t2 = 0, t3 = 0;                                                              \
        unsigned int i = 0;                                                                                  \
        for (; i + 4 <= n; i += 4) {                                                                         \
            t0 += a[i];                                                                                      \
            t1 += a[i + 1];                                                                                  \
            t2 += a[i + 2];                                                                                  \
            t3 += a[i + 3];                                                                                  \
        }                                                                                                    \
        for (; i < n; i++)                                                                                   \
            t0 += a[i];                                                                                      \
        return (t0 + t1) + (t2 + t3);                                                                        \
    }

DEFINE_SUM(sum_i32, int32_t, int64_t)
DEFINE_SUM(sum_i64, int64_t, int64_t)
DEFINE_SUM(sum_f32, float, double)
DEFINE_SUM(sum_f64, double, double)

/* n == 0 is rejected before these are reached -- an empty range has no least or greatest element. */
#define DEFINE_EXTREME(name, ctype, cmp)                                                                     \
    REDUCE_ATTR static ctype name(const ctype* restrict a, unsigned int n) {                                 \
        ctype best = a[0];                                                                                   \
        for (unsigned int i = 1; i < n; i++)                                                                 \
            if (a[i] cmp best)                                                                               \
                best = a[i];                                                                                 \
        return best;                                                                                         \
    }

DEFINE_EXTREME(min_i32, int32_t, <)
DEFINE_EXTREME(max_i32, int32_t, >)
DEFINE_EXTREME(min_i64, int64_t, <)
DEFINE_EXTREME(max_i64, int64_t, >)
DEFINE_EXTREME(min_f32, float, <)
DEFINE_EXTREME(max_f32, float, >)
DEFINE_EXTREME(min_f64, double, <)
DEFINE_EXTREME(max_f64, double, >)

/* FN_COLLECTION_SUM/MIN/MAX over a typed array. Integer elements answer as an integer and float
   elements as a real, so the result matches what indexing the same array would have given. */
static AerVal typed_reduce(AerTypedArray* ta, int fn_id) {
    const void* d = ta->data;
    unsigned int n = ta->count;
    switch (ta->elem_kind) {
        case TYPED_ELEM_INT32:
            if (fn_id == FN_COLLECTION_SUM)
                return aer_int(sum_i32(d, n));
            return aer_int(fn_id == FN_COLLECTION_MIN ? min_i32(d, n) : max_i32(d, n));
        case TYPED_ELEM_INT64:
            if (fn_id == FN_COLLECTION_SUM)
                return aer_int(sum_i64(d, n));
            return aer_int(fn_id == FN_COLLECTION_MIN ? min_i64(d, n) : max_i64(d, n));
        case TYPED_ELEM_FLOAT32:
            if (fn_id == FN_COLLECTION_SUM)
                return aer_real(sum_f32(d, n));
            return aer_real((double)(fn_id == FN_COLLECTION_MIN ? min_f32(d, n) : max_f32(d, n)));
        case TYPED_ELEM_FLOAT64:
        default:
            if (fn_id == FN_COLLECTION_SUM)
                return aer_real(sum_f64(d, n));
            return aer_real(fn_id == FN_COLLECTION_MIN ? min_f64(d, n) : max_f64(d, n));
    }
}

static double typed_elem(const AerTypedArray* t, unsigned int i) {
    switch (t->elem_kind) {
        case TYPED_ELEM_INT32: return (double)((const int32_t*)t->data)[i];
        case TYPED_ELEM_INT64: return (double)((const int64_t*)t->data)[i];
        case TYPED_ELEM_FLOAT32: return (double)((const float*)t->data)[i];
        default: return ((const double*)t->data)[i];
    }
}

/* GROUP BY in one pass. What whole-array arithmetic cannot do is scatter -- send each element to an
   accumulator chosen by another column -- so expressed with masks it costs one pass PER GROUP, which
   turned a 2.9x win into a 0.72x loss (bench/columnar_scale.aer measures both forms). Deliberately
   no vectorization attribute: a scatter does not vectorize; the win is the pass count. A filter
   SKIPS its rejected rows rather than adding zero for them -- a row the loop's `if` never reached
   may hold a group number outside the target, and even a masked-off zero has to index by it. */
#define DEFINE_GROUP_SUM(name, vt, gt)                                                                       \
    static bool name(vt* restrict out, const vt* restrict v, const gt* restrict g, const uint8_t* keep,      \
                     unsigned int n, unsigned int ngroups) {                                                 \
        for (unsigned int i = 0; i < n; i++) {                                                               \
            if (keep && !keep[i])                                                                            \
                continue;                                                                                    \
            long long k = (long long)g[i];                                                                   \
            if (k < 0 || (unsigned long long)k >= (unsigned long long)ngroups)                               \
                return false;                                                                                \
            out[k] += v[i];                                                                                  \
        }                                                                                                    \
        return true;                                                                                         \
    }

#define DEFINE_GROUP_SUM_SET(vsfx, vt)                                                                       \
    DEFINE_GROUP_SUM(gsum_##vsfx##_i32, vt, int32_t)                                                         \
    DEFINE_GROUP_SUM(gsum_##vsfx##_i64, vt, int64_t)                                                         \
    DEFINE_GROUP_SUM(gsum_##vsfx##_f32, vt, float)                                                           \
    DEFINE_GROUP_SUM(gsum_##vsfx##_f64, vt, double)

DEFINE_GROUP_SUM_SET(i32, int32_t)
DEFINE_GROUP_SUM_SET(i64, int64_t)
DEFINE_GROUP_SUM_SET(f32, float)
DEFINE_GROUP_SUM_SET(f64, double)

/* Dispatches on both element kinds -- the values decide the result's kind, the groups only supply
   an index. Returns false on a group index outside 0..ngroups, which the caller reports. */
static bool group_sum_run(AerTypedArray* val, AerTypedArray* grp, AerTypedArray* out, const uint8_t* keep,
                          unsigned int ngroups) {
    unsigned int n = val->count;
#define GS_BY_GROUP(vsfx, vt)                                                                                \
    switch (grp->elem_kind) {                                                                                \
        case TYPED_ELEM_INT32:                                                                               \
            return gsum_##vsfx##_i32((vt*)out->data, (const vt*)val->data, (const int32_t*)grp->data, keep,  \
                                     n, ngroups);                                                            \
        case TYPED_ELEM_INT64:                                                                               \
            return gsum_##vsfx##_i64((vt*)out->data, (const vt*)val->data, (const int64_t*)grp->data, keep,  \
                                     n, ngroups);                                                            \
        case TYPED_ELEM_FLOAT32:                                                                             \
            return gsum_##vsfx##_f32((vt*)out->data, (const vt*)val->data, (const float*)grp->data, keep, n, \
                                     ngroups);                                                               \
        default:                                                                                             \
            return gsum_##vsfx##_f64((vt*)out->data, (const vt*)val->data, (const double*)grp->data, keep,   \
                                     n, ngroups);                                                            \
    }
    switch (val->elem_kind) {
        case TYPED_ELEM_INT32: GS_BY_GROUP(i32, int32_t)
        case TYPED_ELEM_INT64: GS_BY_GROUP(i64, int64_t)
        case TYPED_ELEM_FLOAT32: GS_BY_GROUP(f32, float)
        default: GS_BY_GROUP(f64, double)
    }
#undef GS_BY_GROUP
}

/* `collection.sum(a * b * c)` in one pass rather than one whole-array pass per operator, each of
   which writes a full-size intermediate the next reads straight back. Tiled rather than enumerated:
   a kernel per operator combination is 27 shapes before element kinds are counted, while a block at
   a time keeps every intermediate in L1 and covers any chain. */
#define CHAIN_TILE 1024u
#define CHAIN_MAX_OPERANDS 8
/* A binary tree over k leaves has k-1 operators, so the token count follows from the operand count
   and needs no separate argument. */
#define CHAIN_MAX_TOKENS (2 * CHAIN_MAX_OPERANDS - 1)

/* Four bits per token, in postfix order: 0 pushes the next operand, anything else applies an
   operator to the top two. Postfix rather than a straight run of operators because the expressions
   worth fusing are trees -- `price * quantity * (1 - discount) * mask` is four operators and no two
   of them are adjacent in the run sense, which is why a left-associative recogniser never fired on
   the one query that most wanted it. */
#define CHAIN_TOK_PUSH 0u
#define CHAIN_TOK_ADD 1u
#define CHAIN_TOK_SUB 2u
#define CHAIN_TOK_MUL 3u
#define CHAIN_TOK_LT 4u
#define CHAIN_TOK_LTE 5u
#define CHAIN_TOK_GT 6u
#define CHAIN_TOK_GTE 7u

static double as_number(AerVal v);

static void chain_load_tile(double* restrict dst, AerVal v, unsigned int at, unsigned int len) {
    if (aer_type(v) != TYPE_TYPED_ARRAY) {
        double s = as_number(v);
        for (unsigned int i = 0; i < len; i++)
            dst[i] = s;
        return;
    }
    AerTypedArray* t = aer_as_typed_array(v);
    if (t->elem_kind == TYPED_ELEM_FLOAT64) {
        memcpy(dst, (const double*)t->data + at, (size_t)len * sizeof(double));
    } else {
        const float* f = (const float*)t->data + at;
        for (unsigned int i = 0; i < len; i++)
            dst[i] = (double)f[i];
    }
}

/* Comparisons answer 1.0/0.0 in the operand's own type, matching what the whole-array form builds a
   mask out of -- so a filter can live inside a fused expression rather than costing its own pass. */
REDUCE_ATTR static void chain_apply(double* restrict acc, const double* restrict rhs, unsigned int tok,
                                    unsigned int len) {
    switch (tok) {
        case CHAIN_TOK_ADD:
            for (unsigned int i = 0; i < len; i++)
                acc[i] += rhs[i];
            break;
        case CHAIN_TOK_SUB:
            for (unsigned int i = 0; i < len; i++)
                acc[i] -= rhs[i];
            break;
        case CHAIN_TOK_MUL:
            for (unsigned int i = 0; i < len; i++)
                acc[i] *= rhs[i];
            break;
        case CHAIN_TOK_LT:
            for (unsigned int i = 0; i < len; i++)
                acc[i] = acc[i] < rhs[i] ? 1.0 : 0.0;
            break;
        case CHAIN_TOK_LTE:
            for (unsigned int i = 0; i < len; i++)
                acc[i] = acc[i] <= rhs[i] ? 1.0 : 0.0;
            break;
        case CHAIN_TOK_GT:
            for (unsigned int i = 0; i < len; i++)
                acc[i] = acc[i] > rhs[i] ? 1.0 : 0.0;
            break;
        default:
            for (unsigned int i = 0; i < len; i++)
                acc[i] = acc[i] >= rhs[i] ? 1.0 : 0.0;
            break;
    }
}

static double as_number(AerVal v) {
    return aer_type(v) == TYPE_INTEGER ? (double)aer_as_int(v) : aer_as_real(v);
}

/* A typed array's length is fixed at construction, so append/insert/delete/reserve can never apply
   to one. Worth saying outright: "requires an array" on a value that plainly is an array sends the
   reader looking for the wrong mistake. */
static bool reject_fixed_length(VM* vm, AerVal v, const char* fname) {
    if (aer_type(v) != TYPE_TYPED_ARRAY)
        return false;
    error("%s() cannot resize a typed array -- its length is fixed when it is created", fname);
    vm_stack_push(vm, aer_null());
    return true;
}

/* Sorting a column, specialised per element kind so the comparison inlines -- qsort's indirect call
   per comparison measured 4.6x slower than NumPy over 16M elements. Three-way partitioning, because
   a column is where duplicates concentrate: 4M rows over a thousand distinct prices sends every
   equal element to the middle in one pass rather than recursing through them. Recursion is always
   into the smaller side, so the depth stays logarithmic without an explicit stack. */
#define TYPED_SORT_SMALL 24
#define TYPED_MED3(x, y, z)                                                                                  \
    ((x) < (y) ? ((y) < (z) ? (y) : ((x) < (z) ? (z) : (x))) : ((x) < (z) ? (x) : ((y) < (z) ? (z) : (y))))

#define DEFINE_TYPED_SORT(name, ctype)                                                                       \
    static void name##_heap(ctype* a, size_t n) {                                                            \
        for (size_t start = n / 2; start > 0;) {                                                             \
            start--;                                                                                         \
            for (size_t r = start;;) {                                                                       \
                size_t c = 2 * r + 1;                                                                        \
                if (c >= n)                                                                                  \
                    break;                                                                                   \
                if (c + 1 < n && a[c] < a[c + 1])                                                            \
                    c++;                                                                                     \
                if (!(a[r] < a[c]))                                                                          \
                    break;                                                                                   \
                ctype t = a[r];                                                                              \
                a[r] = a[c];                                                                                 \
                a[c] = t;                                                                                    \
                r = c;                                                                                       \
            }                                                                                                \
        }                                                                                                    \
        for (size_t end = n; end > 1;) {                                                                     \
            end--;                                                                                           \
            ctype t = a[0];                                                                                  \
            a[0] = a[end];                                                                                   \
            a[end] = t;                                                                                      \
            for (size_t r = 0;;) {                                                                           \
                size_t c = 2 * r + 1;                                                                        \
                if (c >= end)                                                                                \
                    break;                                                                                   \
                if (c + 1 < end && a[c] < a[c + 1])                                                          \
                    c++;                                                                                     \
                if (!(a[r] < a[c]))                                                                          \
                    break;                                                                                   \
                ctype u = a[r];                                                                              \
                a[r] = a[c];                                                                                 \
                a[c] = u;                                                                                    \
                r = c;                                                                                       \
            }                                                                                                \
        }                                                                                                    \
    }                                                                                                        \
    static void name##_run(ctype* a, size_t n, int depth) {                                                  \
        while (n > TYPED_SORT_SMALL) {                                                                       \
            if (depth-- <= 0) {                                                                              \
                name##_heap(a, n);                                                                           \
                return;                                                                                      \
            }                                                                                                \
            /* Sample positions have to be irregular. Any fixed stride aliases against a column that      \
               repeats with a period -- `(i % 1000) * 0.5` against a stride of n/8 lands on the same       \
               phase nine times out of nine and hands back the minimum, which is how a median-of-nine      \
               measured 2.3x SLOWER than the qsort it replaced. The seed is derived from the range         \
               itself so the choice stays deterministic and needs no shared state. */ \
            size_t r = (size_t)((uintptr_t)a >> 3) ^ (n * (size_t)0x9E3779B97F4A7C15ULL);                    \
            ctype p;                                                                                         \
            if (n > 128) {                                                                                   \
                ctype v[3];                                                                                  \
                for (int k = 0; k < 3; k++) {                                                                \
                    r ^= r >> 30;                                                                            \
                    r *= (size_t)0xBF58476D1CE4E5B9ULL;                                                      \
                    r ^= r >> 27;                                                                            \
                    v[k] = a[r % n];                                                                         \
                }                                                                                            \
                p = TYPED_MED3(v[0], v[1], v[2]);                                                            \
            } else {                                                                                         \
                p = TYPED_MED3(a[0], a[n / 2], a[n - 1]);                                                    \
            }                                                                                                \
            size_t lt = 0, i = 0, gt = n;                                                                    \
            while (i < gt) {                                                                                 \
                if (a[i] < p) {                                                                              \
                    ctype t = a[lt];                                                                         \
                    a[lt++] = a[i];                                                                          \
                    a[i++] = t;                                                                              \
                } else if (p < a[i]) {                                                                       \
                    ctype t = a[--gt];                                                                       \
                    a[gt] = a[i];                                                                            \
                    a[i] = t;                                                                                \
                } else {                                                                                     \
                    i++; /* equal to the pivot, and already where it belongs */                              \
                }                                                                                            \
            }                                                                                                \
            if (lt < n - gt) {                                                                               \
                name##_run(a, lt, depth);                                                                    \
                a += gt;                                                                                     \
                n -= gt;                                                                                     \
            } else {                                                                                         \
                name##_run(a + gt, n - gt, depth);                                                           \
                n = lt;                                                                                      \
            }                                                                                                \
        }                                                                                                    \
        for (size_t k = 1; k < n; k++) {                                                                     \
            ctype v = a[k];                                                                                  \
            size_t j = k;                                                                                    \
            while (j > 0 && v < a[j - 1]) {                                                                  \
                a[j] = a[j - 1];                                                                             \
                j--;                                                                                         \
            }                                                                                                \
            a[j] = v;                                                                                        \
        }                                                                                                    \
    }                                                                                                        \
    static void name(ctype* a, size_t n) {                                                                   \
        int depth = 0;                                                                                       \
        for (size_t m = n; m > 1; m >>= 1)                                                                   \
            depth++;                                                                                         \
        name##_run(a, n, 2 * depth);                                                                         \
    }

DEFINE_TYPED_SORT(sort_i32, int32_t)
DEFINE_TYPED_SORT(sort_i64, int64_t)
DEFINE_TYPED_SORT(sort_f32, float)
DEFINE_TYPED_SORT(sort_f64, double)

static void typed_sort(AerTypedArray* t) {
    switch (t->elem_kind) {
        case TYPED_ELEM_INT32: sort_i32((int32_t*)t->data, t->count); break;
        case TYPED_ELEM_INT64: sort_i64((int64_t*)t->data, t->count); break;
        case TYPED_ELEM_FLOAT32: sort_f32((float*)t->data, t->count); break;
        default: sort_f64((double*)t->data, t->count); break;
    }
}

/* Matched in the element's own type where it can be: widening an int64 element to double to compare
   it would start reporting false matches past 2^53. */
#define DEFINE_TYPED_FIND(name, ctype, integral)                                                             \
    static int64_t name(const unsigned char* d, unsigned int n, AerVal want) {                               \
        bool exact = (integral) && aer_type(want) == TYPE_INTEGER;                                           \
        int64_t wi = exact ? aer_as_int(want) : 0;                                                           \
        double wd = as_number(want);                                                                         \
        for (unsigned int i = 0; i < n; i++) {                                                               \
            ctype v;                                                                                         \
            memcpy(&v, d + (size_t)i * sizeof(ctype), sizeof(ctype));                                        \
            if (exact ? ((int64_t)v == wi) : ((double)v == wd))                                              \
                return (int64_t)i;                                                                           \
        }                                                                                                    \
        return -1;                                                                                           \
    }

DEFINE_TYPED_FIND(find_i32, int32_t, true)
DEFINE_TYPED_FIND(find_i64, int64_t, true)
DEFINE_TYPED_FIND(find_f32, float, false)
DEFINE_TYPED_FIND(find_f64, double, false)

static int64_t typed_index_of(AerTypedArray* t, AerVal want) {
    if (aer_type(want) != TYPE_INTEGER && aer_type(want) != TYPE_REAL)
        return -1;
    switch (t->elem_kind) {
        case TYPED_ELEM_INT32: return find_i32(t->data, t->count, want);
        case TYPED_ELEM_INT64: return find_i64(t->data, t->count, want);
        case TYPED_ELEM_FLOAT32: return find_f32(t->data, t->count, want);
        case TYPED_ELEM_FLOAT64:
        default: return find_f64(t->data, t->count, want);
    }
}

/* The same three over an ordinary array. It stays integer-exact while every element is an integer,
   and widens to double the moment one is not -- so summing whole numbers cannot drift, and the
   float case still gets the wide total the typed kernels use. */
static bool boxed_reduce(AerArray* a, int fn_id, const char* fname, AerVal* out) {
    for (unsigned int i = 0; i < a->count; i++) {
        ValueType t = aer_type(a->items[i]);
        if (t != TYPE_INTEGER && t != TYPE_REAL) {
            error("%s() requires every element to be a number", fname);
            return false;
        }
    }
    bool all_int = true;
    for (unsigned int i = 0; i < a->count; i++)
        if (aer_type(a->items[i]) != TYPE_INTEGER)
            all_int = false;

    if (all_int) {
        int64_t acc = aer_as_int(a->items[0]);
        for (unsigned int i = 1; i < a->count; i++) {
            int64_t v = aer_as_int(a->items[i]);
            if (fn_id == FN_COLLECTION_SUM)
                acc += v;
            else if (fn_id == FN_COLLECTION_MIN)
                acc = v < acc ? v : acc;
            else
                acc = v > acc ? v : acc;
        }
        *out = aer_int(acc);
        return true;
    }
    double acc = as_number(a->items[0]);
    for (unsigned int i = 1; i < a->count; i++) {
        double v = as_number(a->items[i]);
        if (fn_id == FN_COLLECTION_SUM)
            acc += v;
        else if (fn_id == FN_COLLECTION_MIN)
            acc = v < acc ? v : acc;
        else
            acc = v > acc ? v : acc;
    }
    *out = aer_real(acc);
    return true;
}

static bool push_null(VM* vm) {
    vm_stack_push(vm, aer_null());
    return true;
}

/* Reports and answers null in one statement -- these handlers reject on several counts each, and
   three lines per rejection buried what was actually being checked. */
static bool push_error(VM* vm, const char* msg) {
    error("%s", msg);
    return push_null(vm);
}

/* Every operand is either a float column or a scalar to broadcast, and the columns agree on length.
   Float only: an integer expression would have to answer as an integer, which the double the tile
   evaluator carries cannot promise. */
static bool chain_operands_ok(const AerVal* operand, int n, unsigned int* count) {
    bool sized = false;
    *count = 0;
    for (int i = 0; i < n; i++) {
        if (aer_type(operand[i]) == TYPE_TYPED_ARRAY) {
            AerTypedArray* t = aer_as_typed_array(operand[i]);
            if (t->elem_kind != TYPED_ELEM_FLOAT32 && t->elem_kind != TYPED_ELEM_FLOAT64) {
                error("a fused array expression covers float columns only");
                return false;
            }
            if (sized && t->count != *count) {
                error("Typed arrays must have the same length (got %u and %u)", *count, t->count);
                return false;
            }
            *count = t->count;
            sized = true;
        } else if (aer_type(operand[i]) != TYPE_INTEGER && aer_type(operand[i]) != TYPE_REAL) {
            error("a fused array expression needs numbers and typed arrays");
            return false;
        }
    }
    if (!sized)
        *count = 0;
    return true;
}

/* Evaluates the postfix program over rows [at, at+len), leaving the block's values in stack[0]. The
   stack never exceeds one entry per operand, and a tile is small enough that the whole stack stays in
   L1 -- which is the entire point: no intermediate is ever written at full size. */
static void chain_eval_tile(double stack[][CHAIN_TILE], const AerVal* operand, uint64_t prog, int ntok,
                            unsigned int at, unsigned int len) {
    int sp = 0, next_leaf = 0;
    for (int t = 0; t < ntok; t++) {
        unsigned int tok = (unsigned int)((prog >> (4 * t)) & 0xFu);
        if (tok == CHAIN_TOK_PUSH) {
            chain_load_tile(stack[sp++], operand[next_leaf++], at, len);
        } else {
            sp--;
            chain_apply(stack[sp - 1], stack[sp], tok, len);
        }
    }
}

/* One tiled pass over the whole expression. Float operands only -- an integer chain would have to
   answer as an integer, which a double accumulator cannot promise. */
static bool sum_chain_eval(AerVal* operand, int n, uint64_t prog, unsigned int count, double* out) {
    double stack[CHAIN_MAX_OPERANDS][CHAIN_TILE];
    int ntok = 2 * n - 1;
    double t0 = 0, t1 = 0;
    for (unsigned int at = 0; at < count; at += CHAIN_TILE) {
        unsigned int len = count - at < CHAIN_TILE ? count - at : CHAIN_TILE;
        chain_eval_tile(stack, operand, prog, ntok, at, len);
        /* Two partial sums, matching collection.sum's own reassociation rather than inventing a
           different one -- the fused and unfused spellings must agree. */
        unsigned int i = 0;
        for (; i + 2 <= len; i += 2) {
            t0 += stack[0][i];
            t1 += stack[0][i + 1];
        }
        for (; i < len; i++)
            t0 += stack[0][i];
    }
    *out = t0 + t1;
    return true;
}

/* The same tiled evaluation, scattered into per-group totals instead of summed. This is the shape a
   filtered GROUP BY actually has, and it is where the fusion pays most: the query that motivated it
   wrote seven full-size intermediates to compute values the scatter consumed once. */
static bool group_sum_chain_eval(AerVal* operand, int n, uint64_t prog, AerTypedArray* grp,
                                 double* restrict out, unsigned int ngroups, unsigned int count) {
    double stack[CHAIN_MAX_OPERANDS][CHAIN_TILE];
    int ntok = 2 * n - 1;
    for (unsigned int at = 0; at < count; at += CHAIN_TILE) {
        unsigned int len = count - at < CHAIN_TILE ? count - at : CHAIN_TILE;
        chain_eval_tile(stack, operand, prog, ntok, at, len);
        for (unsigned int i = 0; i < len; i++) {
            long long k = (long long)typed_elem(grp, at + i);
            if (k < 0 || (unsigned long long)k >= (unsigned long long)ngroups)
                return false;
            out[k] += stack[0][i];
        }
    }
    return true;
}

bool aer_collection_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_COLLECTION_GROUP_SUM_INTO && (arg_count == 3 || arg_count == 4)) {
        AerVal keep_v = arg_count == 4 ? vm_stack_pop(vm) : aer_null();
        AerVal grp_v = vm_stack_pop(vm);
        AerVal val_v = vm_stack_pop(vm);
        AerVal tgt_v = vm_stack_pop(vm);
        if (aer_type(tgt_v) != TYPE_TYPED_ARRAY || aer_type(val_v) != TYPE_TYPED_ARRAY ||
            aer_type(grp_v) != TYPE_TYPED_ARRAY || (arg_count == 4 && aer_type(keep_v) != TYPE_TYPED_ARRAY))
            return push_error(vm, "group accumulation needs typed arrays");
        AerTypedArray* tgt = aer_as_typed_array(tgt_v);
        AerTypedArray* val = aer_as_typed_array(val_v);
        AerTypedArray* grp = aer_as_typed_array(grp_v);
        AerTypedArray* keep = arg_count == 4 ? aer_as_typed_array(keep_v) : NULL;
        if (val->count != grp->count || tgt->elem_kind != val->elem_kind ||
            (keep && keep->count != val->count))
            return push_error(vm, "group accumulation needs matching lengths and element kinds");
        /* The filter arrives as 1/0 in whichever element kind the comparison produced. Flattening it
           to a byte per row costs a pass but keeps the scatter kernels from needing a variant per
           mask kind on top of the value and group kinds they already carry. */
        uint8_t* flags = NULL;
        if (keep && val->count > 0) {
            flags = (uint8_t*)malloc(val->count);
            if (!flags)
                return push_error(vm, "out of memory building a group filter");
            for (unsigned int i = 0; i < val->count; i++)
                flags[i] = (uint8_t)(typed_elem(keep, i) != 0.0);
        }
        /* Adds into what the target already holds, exactly as the loop's `t[g] = t[g] + v` did --
           no zeroing, and the array's own length is the group count. */
        bool ok = val->count == 0 || group_sum_run(val, grp, tgt, flags, tgt->count);
        free(flags);
        if (!ok) {
            error("group index outside 0..%u", tgt->count - 1);
            return push_null(vm);
        }
        vm_stack_push(vm, tgt_v);
        return true;
    }
    /* `group_sum(price * quantity * (1 - discount) * mask, region, G)` -- the values never exist as
       an array. Answers in float64 regardless of what the columns were, since the tile evaluator
       carries doubles; group_sum's unfused form takes its result kind from the values, and a fused
       expression's values are always the evaluator's. */
    if (fn_id == FN_COLLECTION_GROUP_SUM_CHAIN && arg_count >= 5) {
        AerVal operand[CHAIN_MAX_OPERANDS];
        int n = arg_count - 3;
        if (n > CHAIN_MAX_OPERANDS) {
            error("group_sum() chain too long to fuse");
            return push_null(vm);
        }
        for (int i = n - 1; i >= 0; i--)
            operand[i] = vm_stack_pop(vm);
        AerVal ngroups_v = vm_stack_pop(vm);
        AerVal grp_v = vm_stack_pop(vm);
        AerVal prog_v = vm_stack_pop(vm);
        if (aer_type(grp_v) != TYPE_TYPED_ARRAY) {
            error("group_sum() needs a typed array of group numbers");
            return push_null(vm);
        }
        if (aer_type(ngroups_v) != TYPE_INTEGER || aer_as_int(ngroups_v) <= 0) {
            error("group_sum() group count must be a positive integer");
            return push_null(vm);
        }
        unsigned int count = 0;
        if (!chain_operands_ok(operand, n, &count))
            return push_null(vm);
        AerTypedArray* grp = aer_as_typed_array(grp_v);
        if (count != grp->count) {
            error("group_sum() values and groups must be the same length (got %u and %u)", count, grp->count);
            return push_null(vm);
        }
        unsigned int ngroups = (unsigned int)aer_as_int(ngroups_v);
        AerVal out_v = vm_new_typed_array_val(TYPED_ELEM_FLOAT64, ngroups);
        double* out = (double*)aer_as_typed_array(out_v)->data;
        memset(out, 0, (size_t)ngroups * sizeof(double));
        if (count > 0 &&
            !group_sum_chain_eval(operand, n, (uint64_t)aer_as_int(prog_v), grp, out, ngroups, count)) {
            error("group_sum() found a group number outside 0..%u", ngroups - 1);
            return push_null(vm);
        }
        vm_stack_push(vm, out_v);
        return true;
    }
    if (fn_id == FN_COLLECTION_SUM_CHAIN && arg_count >= 3) {
        AerVal operand[CHAIN_MAX_OPERANDS];
        int n = arg_count - 1;
        if (n > CHAIN_MAX_OPERANDS) {
            error("sum() chain too long to fuse");
            vm_stack_push(vm, aer_null());
            return true;
        }
        for (int i = n - 1; i >= 0; i--)
            operand[i] = vm_stack_pop(vm);
        AerVal packed_v = vm_stack_pop(vm);
        unsigned int count = 0;
        if (!chain_operands_ok(operand, n, &count))
            return push_null(vm);
        double total = 0.0;
        if (count > 0)
            sum_chain_eval(operand, n, (uint64_t)aer_as_int(packed_v), count, &total);
        vm_stack_push(vm, aer_real(total));
        return true;
    }
    if (fn_id == FN_COLLECTION_GROUP_SUM && arg_count == 3) {
        AerVal ngroups_v = vm_stack_pop(vm);
        AerVal grp_v = vm_stack_pop(vm);
        AerVal val_v = vm_stack_pop(vm);
        if (aer_type(val_v) != TYPE_TYPED_ARRAY || aer_type(grp_v) != TYPE_TYPED_ARRAY) {
            error("group_sum() needs a typed array of values and a typed array of group numbers");
            vm_stack_push(vm, aer_null());
            return true;
        }
        if (aer_type(ngroups_v) != TYPE_INTEGER || aer_as_int(ngroups_v) <= 0) {
            error("group_sum() group count must be a positive integer");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerTypedArray* val = aer_as_typed_array(val_v);
        AerTypedArray* grp = aer_as_typed_array(grp_v);
        if (val->count != grp->count) {
            error("group_sum() values and groups must be the same length (got %u and %u)", val->count,
                  grp->count);
            vm_stack_push(vm, aer_null());
            return true;
        }
        unsigned int ngroups = (unsigned int)aer_as_int(ngroups_v);
        AerVal out_v = vm_new_typed_array_val(val->elem_kind, ngroups);
        AerTypedArray* out = aer_as_typed_array(out_v);
        memset(out->data, 0, (size_t)ngroups * vm_typed_elem_width(val->elem_kind));
        if (val->count > 0 && !group_sum_run(val, grp, out, NULL, ngroups)) {
            error("group_sum() found a group number outside 0..%u", ngroups - 1);
            vm_stack_push(vm, aer_null());
            return true;
        }
        vm_stack_push(vm, out_v);
        return true;
    }
    if ((fn_id == FN_COLLECTION_SUM || fn_id == FN_COLLECTION_MIN || fn_id == FN_COLLECTION_MAX) &&
        arg_count == 1) {
        const char* fname = fn_id == FN_COLLECTION_SUM ? "sum" : (fn_id == FN_COLLECTION_MIN ? "min" : "max");
        AerVal src = vm_stack_pop(vm);
        unsigned int count;
        if (aer_type(src) == TYPE_TYPED_ARRAY)
            count = aer_as_typed_array(src)->count;
        else if (aer_type(src) == TYPE_ARRAY && !aer_as_array(src)->shape)
            count = aer_as_array(src)->count;
        else {
            error("%s() requires an array of numbers", fname);
            vm_stack_push(vm, aer_null());
            return true;
        }
        if (count == 0) {
            /* Summing nothing is 0; there is no honest least or greatest of nothing. */
            if (fn_id != FN_COLLECTION_SUM) {
                error("%s() of an empty array has no answer", fname);
                vm_stack_push(vm, aer_null());
                return true;
            }
            bool floaty = aer_type(src) == TYPE_TYPED_ARRAY &&
                          (aer_as_typed_array(src)->elem_kind == TYPED_ELEM_FLOAT32 ||
                           aer_as_typed_array(src)->elem_kind == TYPED_ELEM_FLOAT64);
            vm_stack_push(vm, floaty ? aer_real(0.0) : aer_int(0));
            return true;
        }
        if (aer_type(src) == TYPE_TYPED_ARRAY) {
            vm_stack_push(vm, typed_reduce(aer_as_typed_array(src), fn_id));
            return true;
        }
        AerVal out;
        if (!boxed_reduce(aer_as_array(src), fn_id, fname, &out))
            out = aer_null();
        vm_stack_push(vm, out);
        return true;
    }
    if (fn_id == FN_COLLECTION_APPEND && arg_count == 2) {
        AerVal val = vm_stack_pop(vm);
        AerVal arr = vm_stack_pop(vm);
        if (reject_fixed_length(vm, arr, "append"))
            return true;
        if (aer_type(arr) != TYPE_ARRAY) {
            error("append() requires an array");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerArray* a = aer_as_array(arr);
        if (a->shape) {
            error("append() cannot add fields to a struct instance — structs have a fixed shape");
            vm_stack_push(vm, aer_null());
            return true;
        }
        if (a->count >= a->capacity) {
            a->capacity = a->capacity ? a->capacity * 2 : 4;
            a->items = xrealloc(a->items, sizeof(AerVal) * a->capacity);
        }
        gc_barrier_array(vm, a, a->count, val);
        a->items[a->count++] = val;
        a->generation++; /* see AerArray.generation's own comment, value.h */
        vm_stack_push(vm, arr);
        return true;
    }
    if (fn_id == FN_COLLECTION_RESERVE && arg_count == 2) {
        AerVal n_v = vm_stack_pop(vm);
        AerVal arr = vm_stack_pop(vm);
        if (reject_fixed_length(vm, arr, "reserve"))
            return true;
        if (aer_type(arr) != TYPE_ARRAY) {
            error("reserve() requires an array");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerArray* a = aer_as_array(arr);
        if (a->shape) {
            error("reserve() cannot resize a struct instance — structs have a fixed shape");
            vm_stack_push(vm, aer_null());
            return true;
        }
        if (aer_type(n_v) != TYPE_INTEGER) {
            error("reserve() count must be an integer");
            vm_stack_push(vm, aer_null());
            return true;
        }
        int64_t n = aer_as_int(n_v);
        if (n < 0) {
            error("reserve() count cannot be negative");
            vm_stack_push(vm, aer_null());
            return true;
        }
        /* Pre-sizes items[] once, up front, to skip append()'s incremental double-on-overflow
           growth for the common "build a huge array via many appends" pattern -- mirrors
           hashtable_reserve's own contract (hashtable.c): never shrinks, a no-op if already big
           enough. Doesn't touch count -- unlike a packed array's fixed-size construction, this is
           purely a capacity hint; elements still only exist once actually appended/assigned. */
        if ((unsigned int)n > a->capacity) {
            a->capacity = (unsigned int)n;
            a->items = xrealloc(a->items, sizeof(AerVal) * a->capacity);
        }
        vm_stack_push(vm, arr);
        return true;
    }
    if (fn_id == FN_COLLECTION_DELETE && arg_count == 2) {
        AerVal key = vm_stack_pop(vm);
        AerVal obj = vm_stack_pop(vm);
        if (reject_fixed_length(vm, obj, "delete"))
            return true;
        if (aer_type(obj) == TYPE_DICT) {
            if (aer_type(key) != TYPE_STRING) {
                error("delete() key must be a string");
                vm_stack_push(vm, aer_null());
                return true;
            }
            AerString* ks = aer_as_string(key);
            unsigned int klen = hashtable_key_true_len(ks->data, ks->length);
            /* hashtable_remove swap-compacts the dense array (moves the last entry into the
               vacated slot), which invalidates any existing per-index dirty-card state -- rather
               than fix up the one moved entry's card (real complexity for a rare path), dirty_all
               just forces a full rescan next cycle if this dict is remembered (harmless, cheap,
               no-op otherwise). See AerDict.dirty_cards's own comment, vm.h. */
            aer_as_dict(obj)->dirty_all = true;
            hashtable_remove(&aer_as_dict(obj)->map, ks->data, klen);
            vm_stack_push(vm, obj);
            return true;
        }
        if (aer_type(obj) == TYPE_ARRAY) {
            AerArray* a = aer_as_array(obj);
            if (a->shape) {
                error("delete() cannot remove fields from a struct instance — structs have a fixed shape");
                vm_stack_push(vm, aer_null());
                return true;
            }
            if (aer_type(key) != TYPE_INTEGER) {
                error("Array delete() index must be an integer");
                vm_stack_push(vm, aer_null());
                return true;
            }
            int64_t i = aer_as_int(key);
            if (i < 0)
                i += (int64_t)a->count;
            if (i < 0 || (uint64_t)i >= a->count) {
                error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(key), a->count);
                vm_stack_push(vm, aer_null());
                return true;
            }
            /* Shifts every element after i down by one -- same dirty_all reasoning as the dict
               branch above (see AerArray.dirty_cards's own comment, value.h). */
            a->dirty_all = true;
            memmove(&a->items[i], &a->items[i + 1], (size_t)(a->count - (uint64_t)i - 1) * sizeof(AerVal));
            a->count--;
            a->generation++; /* see AerArray.generation's own comment, value.h */
            vm_stack_push(vm, obj);
            return true;
        }
        error("delete() requires a hashtable or array");
        vm_stack_push(vm, aer_null());
        return true;
    }
    if (fn_id == FN_COLLECTION_COPY && arg_count == 1) {
        AerVal src = vm_stack_pop(vm);
        if (aer_type(src) == TYPE_ARRAY && !aer_as_array(src)->shape) {
            AerArray* a = aer_as_array(src);
            AerArray* r = vm_new_array();
            r->count = a->count;
            r->capacity = a->count ? a->count : 4;
            r->items = xmalloc(sizeof(AerVal) * r->capacity);
            r->shape = NULL;
            r->generation = 0;
            memcpy(r->items, a->items, sizeof(AerVal) * a->count);
            vm_stack_push(vm, aer_array_val(r));
            return true;
        }
        if (aer_type(src) == TYPE_DICT) {
            AerDict* d = aer_as_dict(src);
            AerDict* r = vm_new_dict();
            if (d->map.count > 0)
                hashtable_reserve(&r->map, d->map.count);
            for (unsigned int i = 0; i < d->map.count; i++) {
                /* Duped into r's own pools, not d's -- the copy owns r->map, not the original.
                   Each source entry's own .hash was already computed once at its original
                   insertion (cached right there in the dense array, not on any AerString) --
                   reused here instead of hashing the same bytes again. */
                char* k = hashtable_key_dup_known(r->map.pools, d->map.dense[i].key, d->map.dense[i].length);
                hashtable_put_hashed(&r->map, k, d->map.dense[i].length, d->map.dense[i].hash,
                                     d->map.dense[i].payload);
            }
            vm_stack_push(vm, aer_dict_val(r));
            return true;
        }
        if (aer_type(src) == TYPE_TYPED_ARRAY) {
            AerTypedArray* t = aer_as_typed_array(src);
            AerVal out = vm_new_typed_array_val(t->elem_kind, t->count);
            if (t->count > 0)
                memcpy(aer_as_typed_array(out)->data, t->data,
                       (size_t)t->count * vm_typed_elem_width(t->elem_kind));
            vm_stack_push(vm, out);
            return true;
        }
        /* A struct instance is deliberately excluded -- construct a fresh one instead (a shaped
           copy would also land in the wrong GC pool, see gc_barrier_array's pool split, vm.c). */
        error("copy() requires an array or dict");
        vm_stack_push(vm, aer_null());
        return true;
    }
    if (fn_id == FN_COLLECTION_INSERT && arg_count == 3) {
        AerVal val = vm_stack_pop(vm);
        AerVal idx = vm_stack_pop(vm);
        AerVal arr = vm_stack_pop(vm);
        if (reject_fixed_length(vm, arr, "insert"))
            return true;
        if (aer_type(arr) != TYPE_ARRAY || aer_as_array(arr)->shape) {
            error("insert() requires an array");
            vm_stack_push(vm, aer_null());
            return true;
        }
        if (aer_type(idx) != TYPE_INTEGER) {
            error("insert() index must be an integer");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerArray* a = aer_as_array(arr);
        int64_t i = aer_as_int(idx);
        if (i < 0)
            i += (int64_t)a->count;
        /* i == count is valid: insert at the end, same as append(). */
        if (i < 0 || (uint64_t)i > a->count) {
            error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(idx), a->count);
            vm_stack_push(vm, aer_null());
            return true;
        }
        if (a->count >= a->capacity) {
            a->capacity = a->capacity ? a->capacity * 2 : 4;
            a->items = xrealloc(a->items, sizeof(AerVal) * a->capacity);
        }
        /* Shifts every element from i onward up by one -- same dirty_all reasoning as delete's own
           (see AerArray.dirty_cards's own comment, value.h); gc_barrier_array's own per-index card
           for the new value at i is harmless but redundant once dirty_all forces a full rescan. */
        a->dirty_all = true;
        gc_barrier_array(vm, a, (unsigned int)i, val);
        memmove(&a->items[i + 1], &a->items[i], (size_t)(a->count - (uint64_t)i) * sizeof(AerVal));
        a->items[i] = val;
        a->count++;
        a->generation++; /* see AerArray.generation's own comment, value.h */
        vm_stack_push(vm, arr);
        return true;
    }
    if (fn_id == FN_COLLECTION_INDEX_OF && arg_count == 2) {
        AerVal val = vm_stack_pop(vm);
        AerVal arr = vm_stack_pop(vm);
        if (aer_type(arr) == TYPE_TYPED_ARRAY) {
            vm_stack_push(vm, aer_int(typed_index_of(aer_as_typed_array(arr), val)));
            return true;
        }
        if (aer_type(arr) != TYPE_ARRAY || aer_as_array(arr)->shape) {
            error("index_of() requires an array");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerArray* a = aer_as_array(arr);
        int64_t found = -1;
        for (unsigned int i = 0; i < a->count; i++)
            if (values_equal(val, a->items[i])) {
                found = (int64_t)i;
                break;
            }
        vm_stack_push(vm, aer_int(found));
        return true;
    }
    if (fn_id == FN_COLLECTION_KEYS && arg_count == 1) {
        AerVal src = vm_stack_pop(vm);
        if (aer_type(src) != TYPE_DICT) {
            error("keys() requires a hashtable");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerDict* d = aer_as_dict(src);
        AerArray* r = vm_new_array();
        r->count = 0;
        r->capacity = d->map.count ? d->map.count : 4;
        r->items = xmalloc(sizeof(AerVal) * r->capacity);
        r->shape = NULL;
        r->generation = 0;
        for (unsigned int i = 0; i < d->map.count; i++) {
            unsigned int n = d->map.dense[i].length;
            r->items[r->count++] = aer_make_string_copy(d->map.dense[i].key, n);
        }
        vm_stack_push(vm, aer_array_val(r));
        return true;
    }
    if (fn_id == FN_COLLECTION_SORT && arg_count == 1) {
        AerVal arr = vm_stack_pop(vm);
        if (aer_type(arr) == TYPE_TYPED_ARRAY) {
            AerTypedArray* t = aer_as_typed_array(arr);
            typed_sort(t);
            vm_stack_push(vm, arr);
            return true;
        }
        if (aer_type(arr) != TYPE_ARRAY) {
            error("sort() requires an array");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerArray* a = aer_as_array(arr);
        if (a->shape) {
            error("sort() cannot sort a struct instance");
            vm_stack_push(vm, aer_null());
            return true;
        }
        /* Ordering across mixed types has no sensible answer, so it's rejected up front rather than
           falling back to an arbitrary tie-break. */
        bool numeric = true, stringy = true;
        for (unsigned int i = 0; i < a->count; i++) {
            if (aer_type(a->items[i]) != TYPE_INTEGER && aer_type(a->items[i]) != TYPE_REAL)
                numeric = false;
            if (aer_type(a->items[i]) != TYPE_STRING)
                stringy = false;
        }
        if (a->count > 0 && !numeric && !stringy) {
            error("sort() requires all elements to be numbers, or all to be strings");
            vm_stack_push(vm, aer_null());
            return true;
        }
        /* Arbitrary reorder -- same dirty_all reasoning as delete/insert above (see
           AerArray.dirty_cards's own comment, value.h); a string element is a real heap reference
           (unlike a number), so this matters even though sort() never replaces a value. */
        a->dirty_all = true;
        qsort(a->items, a->count, sizeof(AerVal), sort_cmp);
        vm_stack_push(vm, arr);
        return true;
    }

    return false;
}

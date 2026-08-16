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

#define DEFINE_TYPED_CMP(name, ctype)                                                                        \
    static int name(const void* pa, const void* pb) {                                                        \
        ctype x = *(const ctype*)pa, y = *(const ctype*)pb;                                                  \
        return x < y ? -1 : (x > y ? 1 : 0);                                                                 \
    }

DEFINE_TYPED_CMP(cmp_i32, int32_t)
DEFINE_TYPED_CMP(cmp_i64, int64_t)
DEFINE_TYPED_CMP(cmp_f32, float)
DEFINE_TYPED_CMP(cmp_f64, double)

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

static int (*typed_cmp_for(TypedArrayElemKind k))(const void*, const void*) {
    switch (k) {
        case TYPED_ELEM_INT32: return cmp_i32;
        case TYPED_ELEM_INT64: return cmp_i64;
        case TYPED_ELEM_FLOAT32: return cmp_f32;
        case TYPED_ELEM_FLOAT64:
        default: return cmp_f64;
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

bool aer_collection_call(VM* vm, int fn_id, int arg_count) {
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
            qsort(t->data, t->count, vm_typed_elem_width(t->elem_kind), typed_cmp_for(t->elem_kind));
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

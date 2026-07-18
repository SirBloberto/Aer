#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "aer_stdlib.h"
#include "error.h"

/* Pops one argument and coerces it to a double via aer_as_double(); on failure, reports "<name>()
   requires a number", pushes null, and returns false — every single-arg math function below
   follows this exact pop-check-report shape. Caller should `return true` immediately when this
   returns false (the error's already been reported and the null result already pushed). */
static bool math_pop_double(VM* vm, const char* name, double* out) {
    AerVal a = vm_stack_pop(vm);
    if (aer_as_double(a, out)) return true;
    error("%s() requires a number", name);
    vm_stack_push(vm, aer_null());
    return false;
}

/* qsort() comparator for sort() — only called once the caller has verified every element is TYPE_STRING or every element is numeric, so no type-mismatch case needs handling here. */
static int sort_cmp(const void* pa, const void* pb) {
    const AerVal* a = (const AerVal*)pa;
    const AerVal* b = (const AerVal*)pb;
    if (aer_type(*a) == TYPE_STRING) {
        AerString* as = aer_as_string(*a);
        AerString* bs = aer_as_string(*b);
        unsigned int n = as->length < bs->length ? as->length : bs->length;
        int c = memcmp(as->data, bs->data, n);
        if (c != 0) return c;
        return (int)as->length - (int)bs->length;
    }
    double da = aer_type(*a) == TYPE_INTEGER ? (double)aer_as_int(*a) : aer_as_real(*a);
    double db = aer_type(*b) == TYPE_INTEGER ? (double)aer_as_int(*b) : aer_as_real(*b);
    return da < db ? -1 : (da > db ? 1 : 0);
}

bool aer_math_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_MATH_SQRT && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "sqrt", &x)) return true;
        vm_stack_push(vm, aer_real(sqrt(x))); return true;
    }
    if (fn_id == FN_MATH_POW && arg_count == 2) {
        AerVal ey = vm_stack_pop(vm); AerVal ex = vm_stack_pop(vm);
        double x, y;
        if (!aer_as_double(ex, &x) || !aer_as_double(ey, &y)) { error("pow() requires two numbers"); vm_stack_push(vm, aer_null()); return true; }
        vm_stack_push(vm, aer_real(pow(x, y))); return true;
    }
    if (fn_id == FN_MATH_FLOOR && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "floor", &x)) return true;
        vm_stack_push(vm, aer_int((int64_t)floor(x))); return true;
    }
    if (fn_id == FN_MATH_CEIL && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "ceil", &x)) return true;
        vm_stack_push(vm, aer_int((int64_t)ceil(x))); return true;
    }
    if (fn_id == FN_MATH_ABS && arg_count == 1) {
        AerVal a = vm_stack_pop(vm);
        if (aer_type(a) == TYPE_INTEGER) {
            int64_t n = aer_as_int(a);
            vm_stack_push(vm, aer_int(n < 0 ? -n : n)); return true;
        }
        if (aer_type(a) == TYPE_REAL) {
            double d = aer_as_real(a);
            vm_stack_push(vm, aer_real(d < 0 ? -d : d)); return true;
        }
        error("abs() requires a number"); vm_stack_push(vm, aer_null()); return true;
    }
    if (fn_id == FN_MATH_MIN && arg_count == 2) {
        AerVal b = vm_stack_pop(vm); AerVal a = vm_stack_pop(vm);
        double da, db;
        if (!aer_as_double(a, &da) || !aer_as_double(b, &db)) { error("min() requires two numbers"); vm_stack_push(vm, aer_null()); return true; }
        vm_stack_push(vm, da <= db ? a : b); return true;
    }
    if (fn_id == FN_MATH_MAX && arg_count == 2) {
        AerVal b = vm_stack_pop(vm); AerVal a = vm_stack_pop(vm);
        double da, db;
        if (!aer_as_double(a, &da) || !aer_as_double(b, &db)) { error("max() requires two numbers"); vm_stack_push(vm, aer_null()); return true; }
        vm_stack_push(vm, da >= db ? a : b); return true;
    }
    if (fn_id == FN_MATH_SIN && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "sin", &x)) return true;
        vm_stack_push(vm, aer_real(sin(x))); return true;
    }
    if (fn_id == FN_MATH_COS && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "cos", &x)) return true;
        vm_stack_push(vm, aer_real(cos(x))); return true;
    }
    if (fn_id == FN_MATH_LOG && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "log", &x)) return true;
        if (x <= 0) { error("log() requires a positive number"); vm_stack_push(vm, aer_null()); return true; }
        vm_stack_push(vm, aer_real(log(x))); return true;
    }
    if (fn_id == FN_MATH_LOG2 && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "log2", &x)) return true;
        if (x <= 0) { error("log2() requires a positive number"); vm_stack_push(vm, aer_null()); return true; }
        vm_stack_push(vm, aer_real(log2(x))); return true;
    }
    if (fn_id == FN_MATH_LOG10 && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "log10", &x)) return true;
        if (x <= 0) { error("log10() requires a positive number"); vm_stack_push(vm, aer_null()); return true; }
        vm_stack_push(vm, aer_real(log10(x))); return true;
    }
    if (fn_id == FN_MATH_PI && arg_count == 0) {
        /* A function, not a bare module value, for consistency with every other native module (none expose non-function bindings yet); literal digits rather than M_PI, which isn't guaranteed defined on every target toolchain. */
        vm_stack_push(vm, aer_real(3.14159265358979323846)); return true;
    }
    if (fn_id == FN_MATH_SORT && arg_count == 1) {
        AerVal arr = vm_stack_pop(vm);
        if (aer_type(arr) != TYPE_ARRAY) { error("sort() requires an array"); vm_stack_push(vm, aer_null()); return true; }
        AerArray* a = aer_as_array(arr);
        if (a->shape) { error("sort() cannot sort a struct instance"); vm_stack_push(vm, aer_null()); return true; }
        /* Ordering across mixed types has no sensible answer, so it's rejected up front rather than falling back to an arbitrary tie-break. */
        bool numeric = true, stringy = true;
        for (unsigned int i = 0; i < a->count; i++) {
            if (aer_type(a->items[i]) != TYPE_INTEGER && aer_type(a->items[i]) != TYPE_REAL) numeric = false;
            if (aer_type(a->items[i]) != TYPE_STRING) stringy = false;
        }
        if (a->count > 0 && !numeric && !stringy) {
            error("sort() requires all elements to be numbers, or all to be strings");
            vm_stack_push(vm, aer_null()); return true;
        }
        qsort(a->items, a->count, sizeof(AerVal), sort_cmp);
        vm_stack_push(vm, arr); return true;
    }

    return false;
}

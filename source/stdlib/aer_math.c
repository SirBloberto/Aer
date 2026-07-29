#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "aer_stdlib.h"
#include "error.h"

/* Pop-and-coerce for single-arg functions; on false the error is already reported and null pushed. */
static bool math_pop_double(VM* vm, const char* name, double* out) {
    AerVal a = vm_stack_pop(vm);
    if (aer_as_double(a, out)) return true;
    error("%s() requires a number", name);
    vm_stack_push(vm, aer_null());
    return false;
}

bool aer_math_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_MATH_SQRT && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "sqrt", &x)) return true;
        if (x < 0) { error("sqrt() requires a non-negative number"); vm_stack_push(vm, aer_null()); return true; }
        vm_stack_push(vm, aer_real(sqrt(x))); return true;
    }
    if (fn_id == FN_MATH_POW && arg_count == 2) {
        AerVal ey = vm_stack_pop(vm); AerVal ex = vm_stack_pop(vm);
        double x, y;
        if (!aer_as_double(ex, &x) || !aer_as_double(ey, &y)) { error("pow() requires two numbers"); vm_stack_push(vm, aer_null()); return true; }
        /* No real result exists for a negative base with a fractional exponent -- error, not nan */
        if (x < 0 && floor(y) != y) { error("pow() with a negative base requires a whole-number exponent"); vm_stack_push(vm, aer_null()); return true; }
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
    if (fn_id == FN_MATH_ROUND && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "round", &x)) return true;
        /* llround, not (int64_t)(x + 0.5) -- the latter mis-rounds negatives (-2.5 -> -1). */
        vm_stack_push(vm, aer_int((int64_t)llround(x))); return true;
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
    if (fn_id == FN_MATH_TAN && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "tan", &x)) return true;
        vm_stack_push(vm, aer_real(tan(x))); return true;
    }
    if (fn_id == FN_MATH_EXP && arg_count == 1) {
        double x;
        if (!math_pop_double(vm, "exp", &x)) return true;
        vm_stack_push(vm, aer_real(exp(x))); return true;
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
        /* Literal digits -- M_PI isn't guaranteed by every toolchain */
        vm_stack_push(vm, aer_real(3.14159265358979323846)); return true;
    }

    return false;
}

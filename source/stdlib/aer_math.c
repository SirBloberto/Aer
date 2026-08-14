#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "aer_stdlib.h"
#include "error.h"

/* Pop-and-coerce for single-arg functions; on false the error is already reported and null pushed. */
static bool math_pop_double(VM* vm, const char* name, double* out) {
    AerVal a = vm_stack_pop(vm);
    if (aer_as_double(a, out))
        return true;
    error("%s() requires a number", name);
    vm_stack_push(vm, aer_null());
    return false;
}

/* Computation and domain check for every single-argument real-in math function. One place knows
   what each computes and what its domain allows, rather than eleven near-identical blocks that can
   drift apart. Raises its own error and returns false on a domain violation; *out is meaningful
   only on true. abs and the 0-/2-arg functions aren't this shape and stay inline below. */
/* The real-returning half of math_unary, without the AerVal. Only these: floor/ceil/round yield an
   integer, so they have no place to land in a real slot. Same domain errors, same results. */
bool aer_math_unary_raw(int fn_id, double x, double* out) {
    switch (fn_id) {
        case FN_MATH_SQRT:
            if (x < 0) {
                error("sqrt() requires a non-negative number");
                return false;
            }
            *out = sqrt(x);
            return true;
        case FN_MATH_SIN: *out = sin(x); return true;
        case FN_MATH_COS: *out = cos(x); return true;
        case FN_MATH_TAN: *out = tan(x); return true;
        case FN_MATH_EXP: *out = exp(x); return true;
        case FN_MATH_LOG:
            if (x <= 0) {
                error("log() requires a positive number");
                return false;
            }
            *out = log(x);
            return true;
        case FN_MATH_LOG2:
            if (x <= 0) {
                error("log2() requires a positive number");
                return false;
            }
            *out = log2(x);
            return true;
        case FN_MATH_LOG10:
            if (x <= 0) {
                error("log10() requires a positive number");
                return false;
            }
            *out = log10(x);
            return true;
        default: return false;
    }
}

/* Which fn_ids aer_math_unary_raw handles -- the parser's test before it may emit OP_RAW_MATH_REAL. */
bool aer_math_fn_is_raw_real(int fn_id) {
    return fn_id == FN_MATH_SQRT || fn_id == FN_MATH_SIN || fn_id == FN_MATH_COS || fn_id == FN_MATH_TAN ||
           fn_id == FN_MATH_EXP || fn_id == FN_MATH_LOG || fn_id == FN_MATH_LOG2 || fn_id == FN_MATH_LOG10;
}

static bool math_unary(int fn_id, double x, AerVal* out) {
    switch (fn_id) {
        case FN_MATH_SQRT:
            if (x < 0) {
                error("sqrt() requires a non-negative number");
                return false;
            }
            *out = aer_real(sqrt(x));
            return true;
        case FN_MATH_FLOOR: *out = aer_int((int64_t)floor(x)); return true;
        case FN_MATH_CEIL: *out = aer_int((int64_t)ceil(x)); return true;
        /* llround, not (int64_t)(x + 0.5) -- the latter mis-rounds negatives (-2.5 -> -1). */
        case FN_MATH_ROUND: *out = aer_int((int64_t)llround(x)); return true;
        case FN_MATH_SIN: *out = aer_real(sin(x)); return true;
        case FN_MATH_COS: *out = aer_real(cos(x)); return true;
        case FN_MATH_TAN: *out = aer_real(tan(x)); return true;
        case FN_MATH_EXP: *out = aer_real(exp(x)); return true;
        case FN_MATH_LOG:
            if (x <= 0) {
                error("log() requires a positive number");
                return false;
            }
            *out = aer_real(log(x));
            return true;
        case FN_MATH_LOG2:
            if (x <= 0) {
                error("log2() requires a positive number");
                return false;
            }
            *out = aer_real(log2(x));
            return true;
        case FN_MATH_LOG10:
            if (x <= 0) {
                error("log10() requires a positive number");
                return false;
            }
            *out = aer_real(log10(x));
            return true;
        default: return false; /* not one of this shape's functions -- not reached today */
    }
}

/* Pop+coerce (NAME supplies the "requires a number" error text), compute via math_unary, push the
   result. A domain violation inside math_unary has already raised its own error and unwound. */
#define MATH_UNARY_CASE(FN_ID, NAME)                                                                         \
    if (fn_id == (FN_ID) && arg_count == 1) {                                                                \
        double x;                                                                                            \
        AerVal result;                                                                                       \
        if (!math_pop_double(vm, (NAME), &x))                                                                \
            return true;                                                                                     \
        if (!math_unary(fn_id, x, &result)) {                                                                \
            vm_stack_push(vm, aer_null());                                                                   \
            return true;                                                                                     \
        }                                                                                                    \
        vm_stack_push(vm, result);                                                                           \
        return true;                                                                                         \
    }

bool aer_math_call(VM* vm, int fn_id, int arg_count) {
    MATH_UNARY_CASE(FN_MATH_SQRT, "sqrt")
    MATH_UNARY_CASE(FN_MATH_FLOOR, "floor")
    MATH_UNARY_CASE(FN_MATH_CEIL, "ceil")
    MATH_UNARY_CASE(FN_MATH_ROUND, "round")
    MATH_UNARY_CASE(FN_MATH_SIN, "sin")
    MATH_UNARY_CASE(FN_MATH_COS, "cos")
    MATH_UNARY_CASE(FN_MATH_TAN, "tan")
    MATH_UNARY_CASE(FN_MATH_EXP, "exp")
    MATH_UNARY_CASE(FN_MATH_LOG, "log")
    MATH_UNARY_CASE(FN_MATH_LOG2, "log2")
    MATH_UNARY_CASE(FN_MATH_LOG10, "log10")
    if (fn_id == FN_MATH_POW && arg_count == 2) {
        AerVal ey = vm_stack_pop(vm);
        AerVal ex = vm_stack_pop(vm);
        double x, y;
        if (!aer_as_double(ex, &x) || !aer_as_double(ey, &y)) {
            error("pow() requires two numbers");
            vm_stack_push(vm, aer_null());
            return true;
        }
        /* No real result exists for a negative base with a fractional exponent -- error, not nan */
        if (x < 0 && floor(y) != y) {
            error("pow() with a negative base requires a whole-number exponent");
            vm_stack_push(vm, aer_null());
            return true;
        }
        vm_stack_push(vm, aer_real(pow(x, y)));
        return true;
    }
    if (fn_id == FN_MATH_ABS && arg_count == 1) {
        AerVal a = vm_stack_pop(vm);
        if (aer_type(a) == TYPE_INTEGER) {
            int64_t n = aer_as_int(a);
            vm_stack_push(vm, aer_int(n < 0 ? -n : n));
            return true;
        }
        if (aer_type(a) == TYPE_REAL) {
            double d = aer_as_real(a);
            vm_stack_push(vm, aer_real(d < 0 ? -d : d));
            return true;
        }
        error("abs() requires a number");
        vm_stack_push(vm, aer_null());
        return true;
    }
    if (fn_id == FN_MATH_MIN && arg_count == 2) {
        AerVal b = vm_stack_pop(vm);
        AerVal a = vm_stack_pop(vm);
        double da, db;
        if (!aer_as_double(a, &da) || !aer_as_double(b, &db)) {
            error("min() requires two numbers");
            vm_stack_push(vm, aer_null());
            return true;
        }
        vm_stack_push(vm, da <= db ? a : b);
        return true;
    }
    if (fn_id == FN_MATH_MAX && arg_count == 2) {
        AerVal b = vm_stack_pop(vm);
        AerVal a = vm_stack_pop(vm);
        double da, db;
        if (!aer_as_double(a, &da) || !aer_as_double(b, &db)) {
            error("max() requires two numbers");
            vm_stack_push(vm, aer_null());
            return true;
        }
        vm_stack_push(vm, da >= db ? a : b);
        return true;
    }
    if (fn_id == FN_MATH_PI && arg_count == 0) {
        /* Literal digits -- M_PI isn't guaranteed by every toolchain */
        vm_stack_push(vm, aer_real(3.14159265358979323846));
        return true;
    }

    return false;
}
#undef MATH_UNARY_CASE

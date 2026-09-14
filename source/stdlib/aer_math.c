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

/* What a real-in, real-out function accepts; the error text names the rule. */
typedef enum { DOMAIN_ANY, DOMAIN_NON_NEGATIVE, DOMAIN_POSITIVE, DOMAIN_UNIT } MathDomain;

/* Every single-argument math function that takes a real and gives a real -- OP_RAW_MATH_REAL's set. */
static const struct {
    int fn_id;
    const char* name;
    double (*fn)(double);
    MathDomain domain;
} real_unary[] = {
    {FN_MATH_SQRT, "sqrt", sqrt, DOMAIN_NON_NEGATIVE},
    {FN_MATH_SIN, "sin", sin, DOMAIN_ANY},
    {FN_MATH_COS, "cos", cos, DOMAIN_ANY},
    {FN_MATH_TAN, "tan", tan, DOMAIN_ANY},
    {FN_MATH_ASIN, "asin", asin, DOMAIN_UNIT},
    {FN_MATH_ACOS, "acos", acos, DOMAIN_UNIT},
    {FN_MATH_ATAN, "atan", atan, DOMAIN_ANY},
    {FN_MATH_EXP, "exp", exp, DOMAIN_ANY},
    {FN_MATH_LOG, "log", log, DOMAIN_POSITIVE},
    {FN_MATH_LOG2, "log2", log2, DOMAIN_POSITIVE},
    {FN_MATH_LOG10, "log10", log10, DOMAIN_POSITIVE},
};

static int real_unary_index(int fn_id) {
    for (int i = 0; i < (int)(sizeof(real_unary) / sizeof(real_unary[0])); i++)
        if (real_unary[i].fn_id == fn_id)
            return i;
    return -1;
}

bool aer_math_fn_is_raw_real(int fn_id) {
    return real_unary_index(fn_id) >= 0;
}

bool aer_math_unary_raw(int fn_id, double x, double* out) {
    int i = real_unary_index(fn_id);
    if (i < 0)
        return false;
    const char* name = real_unary[i].name;
    switch (real_unary[i].domain) {
        case DOMAIN_ANY: break;
        case DOMAIN_NON_NEGATIVE:
            if (x < 0) {
                error("%s() requires a non-negative number", name);
                return false;
            }
            break;
        case DOMAIN_POSITIVE:
            if (x <= 0) {
                error("%s() requires a positive number", name);
                return false;
            }
            break;
        case DOMAIN_UNIT:
            if (x < -1 || x > 1) {
                error("%s() requires a number between -1 and 1", name);
                return false;
            }
            break;
    }
    *out = real_unary[i].fn(x);
    return true;
}

/* A single-argument function on a number: one of real_unary, or floor/ceil/round, which give an
   integer and so have no raw-real form. False, with the stack untouched, for any other fn_id. */
static bool math_unary_call(VM* vm, int fn_id) {
    int ri = real_unary_index(fn_id);
    const char* name = ri >= 0                  ? real_unary[ri].name
                       : fn_id == FN_MATH_FLOOR ? "floor"
                       : fn_id == FN_MATH_CEIL  ? "ceil"
                       : fn_id == FN_MATH_ROUND ? "round"
                                                : NULL;
    if (!name)
        return false;
    double x;
    if (!math_pop_double(vm, name, &x))
        return true;
    double real;
    switch (fn_id) {
        case FN_MATH_FLOOR: vm_stack_push(vm, aer_int((int64_t)floor(x))); break;
        case FN_MATH_CEIL: vm_stack_push(vm, aer_int((int64_t)ceil(x))); break;
        /* llround, not (int64_t)(x + 0.5) -- the latter mis-rounds negatives (-2.5 -> -1). */
        case FN_MATH_ROUND: vm_stack_push(vm, aer_int((int64_t)llround(x))); break;
        default: vm_stack_push(vm, aer_math_unary_raw(fn_id, x, &real) ? aer_real(real) : aer_null()); break;
    }
    return true;
}

bool aer_math_call(VM* vm, int fn_id, int arg_count) {
    if (arg_count == 1 && math_unary_call(vm, fn_id))
        return true;
    /* Two arguments, so it gets the quadrant right where atan(y / x) cannot -- and unlike the
       unary functions it has no raw-slot form, since OP_RAW_MATH_REAL carries one operand. */
    if (fn_id == FN_MATH_ATAN2 && arg_count == 2) {
        AerVal ax = vm_stack_pop(vm);
        AerVal ay = vm_stack_pop(vm);
        double y, x;
        if (!aer_as_double(ay, &y) || !aer_as_double(ax, &x)) {
            error("atan2() requires two numbers");
            vm_stack_push(vm, aer_null());
            return true;
        }
        vm_stack_push(vm, aer_real(atan2(y, x)));
        return true;
    }
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

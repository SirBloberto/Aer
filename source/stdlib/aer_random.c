#include <stdlib.h>
#include <string.h>
#include "aer_stdlib.h"
#include "error.h"

bool aer_random_call(VM* vm, Chunk* c, const char* name, int arg_count) {
    (void)c;

    if (strcmp(name, "random") == 0 && arg_count == 0) {
        vm_stack_push(vm, aer_real((double)rand() / ((double)RAND_MAX + 1.0))); return true;
    }
    if (strcmp(name, "randint") == 0 && arg_count == 2) {
        AerVal hi = vm_stack_pop(vm); AerVal lo = vm_stack_pop(vm);
        if (aer_type(lo) != TYPE_INTEGER || aer_type(hi) != TYPE_INTEGER) { error("randint() requires two integers"); vm_stack_push(vm, aer_null()); return true; }
        int64_t lo_n = aer_as_int(lo), hi_n = aer_as_int(hi);
        if (hi_n < lo_n) { error("randint() requires min <= max"); vm_stack_push(vm, aer_null()); return true; }
        int64_t span = hi_n - lo_n + 1;
        /* rand() % span is slightly biased toward the low end for spans that don't evenly divide RAND_MAX+1 — a known simplification; rejection sampling would fix it but isn't worth the complexity. */
        vm_stack_push(vm, aer_int(lo_n + (int64_t)(rand() % span))); return true;
    }
    if (strcmp(name, "seed") == 0 && arg_count == 1) {
        AerVal a = vm_stack_pop(vm);
        if (aer_type(a) != TYPE_INTEGER) { error("seed() requires an integer"); vm_stack_push(vm, aer_null()); return true; }
        srand((unsigned int)aer_as_int(a));
        vm_stack_push(vm, aer_null()); return true;
    }

    return false;
}

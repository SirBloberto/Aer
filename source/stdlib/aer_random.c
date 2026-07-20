#include <time.h>
#include "aer_stdlib.h"
#include "error.h"

/* Own xoshiro256** generator, not C rand() — RAND_MAX is 32767 on MinGW (capping wide randint
   spans), and seed() must reproduce the same sequence on every platform. */
static uint64_t rng_state[4];
static bool     rng_seeded = false;

static uint64_t splitmix64(uint64_t* x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void rng_seed(uint64_t seed) {
    for (int i = 0; i < 4; i++) rng_state[i] = splitmix64(&seed);
    rng_seeded = true;
}

static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t rng_next(void) {
    if (!rng_seeded) rng_seed((uint64_t)time(NULL) ^ ((uint64_t)clock() << 32));
    uint64_t result = rotl(rng_state[1] * 5, 7) * 9;
    uint64_t t = rng_state[1] << 17;
    rng_state[2] ^= rng_state[0];
    rng_state[3] ^= rng_state[1];
    rng_state[1] ^= rng_state[2];
    rng_state[0] ^= rng_state[3];
    rng_state[2] ^= t;
    rng_state[3] = rotl(rng_state[3], 45);
    return result;
}

/* Uniform in [0, n) without modulo bias — rejects the tail region that doesn't divide evenly. */
static uint64_t rng_below(uint64_t n) {
    uint64_t limit = UINT64_MAX - UINT64_MAX % n;
    uint64_t r;
    do { r = rng_next(); } while (r >= limit);
    return r % n;
}

bool aer_random_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_RANDOM_RANDOM && arg_count == 0) {
        /* Top 53 bits — the full precision a double's mantissa can hold, uniform in [0, 1). */
        vm_stack_push(vm, aer_real((double)(rng_next() >> 11) * (1.0 / 9007199254740992.0))); return true;
    }
    if (fn_id == FN_RANDOM_RANDINT && arg_count == 2) {
        AerVal hi = vm_stack_pop(vm); AerVal lo = vm_stack_pop(vm);
        if (aer_type(lo) != TYPE_INTEGER || aer_type(hi) != TYPE_INTEGER) { error("randint() requires two integers"); vm_stack_push(vm, aer_null()); return true; }
        int64_t lo_n = aer_as_int(lo), hi_n = aer_as_int(hi);
        if (hi_n < lo_n) { error("randint() requires min <= max"); vm_stack_push(vm, aer_null()); return true; }
        uint64_t span = (uint64_t)(hi_n - lo_n) + 1;
        /* span 0 means the full 64-bit range (hi-lo overflowed to UINT64_MAX) — rng_next() itself is already uniform there. */
        uint64_t r = span == 0 ? rng_next() : rng_below(span);
        vm_stack_push(vm, aer_int(lo_n + (int64_t)r)); return true;
    }
    if (fn_id == FN_RANDOM_SEED && arg_count == 1) {
        AerVal a = vm_stack_pop(vm);
        if (aer_type(a) != TYPE_INTEGER) { error("seed() requires an integer"); vm_stack_push(vm, aer_null()); return true; }
        rng_seed((uint64_t)aer_as_int(a));
        vm_stack_push(vm, aer_null()); return true;
    }
    if (fn_id == FN_RANDOM_CHOICE && arg_count == 1) {
        AerVal arr = vm_stack_pop(vm);
        if (aer_type(arr) != TYPE_ARRAY || aer_as_array(arr)->shape) { error("choice() requires an array"); vm_stack_push(vm, aer_null()); return true; }
        AerArray* a = aer_as_array(arr);
        if (a->count == 0) { error("choice() requires a non-empty array"); vm_stack_push(vm, aer_null()); return true; }
        vm_stack_push(vm, a->items[rng_below(a->count)]); return true;
    }
    if (fn_id == FN_RANDOM_SHUFFLE && arg_count == 1) {
        AerVal arr = vm_stack_pop(vm);
        if (aer_type(arr) != TYPE_ARRAY || aer_as_array(arr)->shape) { error("shuffle() requires an array"); vm_stack_push(vm, aer_null()); return true; }
        AerArray* a = aer_as_array(arr);
        /* Fisher-Yates, in place; swapping within one array never needs a GC write barrier — no value becomes newly reachable from it. */
        for (unsigned int i = a->count; i > 1; i--) {
            uint64_t j = rng_below(i);
            AerVal tmp = a->items[i - 1];
            a->items[i - 1] = a->items[j];
            a->items[j] = tmp;
        }
        vm_stack_push(vm, arr); return true;
    }

    return false;
}

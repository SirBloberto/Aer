#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aer_stdlib.h"
#include "error.h"
#include "value_box.h"

void aer_stdlib_init(void) {
    srand((unsigned int)time(NULL));
}

bool aer_stdlib_is_native_module(const char* name, unsigned int len) {
    return (len == 4 && strncmp(name, "math",   4) == 0) ||
           (len == 6 && strncmp(name, "random", 6) == 0) ||
           (len == 6 && strncmp(name, "string", 6) == 0) ||
           (len == 4 && strncmp(name, "time",   4) == 0) ||
           (len == 4 && strncmp(name, "json",   4) == 0);
}

/* Stack helpers — not vm.c's PUSH()/POP() macros, which are scoped to vm_run's locals and DISPATCH(); same bounds-checked semantics, as plain functions instead. */
static bool stdlib_push(VM* vm, AerVal v) {
    if (vm->stack_top >= VM_STACK_MAX) { error("Stack overflow"); return false; }
    vm->stack[vm->stack_top++] = v;
    return true;
}

static AerVal stdlib_pop(VM* vm) {
    if (vm->stack_top <= 0) { error("Stack underflow"); return aer_null(); }
    return vm->stack[--vm->stack_top];
}

/* Integer and real are both "a number" as far as sqrt/pow/floor/ceil/min/max are concerned. */
static bool as_double(AerVal v, double* out) {
    if (aer_type(v) == TYPE_INTEGER) { *out = (double)aer_as_int(v);  return true; }
    if (aer_type(v) == TYPE_REAL)    { *out = aer_as_real(v);         return true; }
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

bool aer_math_call(VM* vm, Chunk* c, const char* name, int arg_count) {
    (void)c;   /* not needed yet — kept for parity with core builtins and in case a future stdlib function needs it */

    if (strcmp(name, "sqrt") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        double x;
        if (!as_double(a, &x)) { error("sqrt() requires a number"); stdlib_push(vm, aer_null()); return true; }
        stdlib_push(vm, aer_real(sqrt(x))); return true;
    }
    if (strcmp(name, "pow") == 0 && arg_count == 2) {
        AerVal ey = stdlib_pop(vm); AerVal ex = stdlib_pop(vm);
        double x, y;
        if (!as_double(ex, &x) || !as_double(ey, &y)) { error("pow() requires two numbers"); stdlib_push(vm, aer_null()); return true; }
        stdlib_push(vm, aer_real(pow(x, y))); return true;
    }
    if (strcmp(name, "floor") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        double x;
        if (!as_double(a, &x)) { error("floor() requires a number"); stdlib_push(vm, aer_null()); return true; }
        stdlib_push(vm, aer_int((long long)floor(x))); return true;
    }
    if (strcmp(name, "ceil") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        double x;
        if (!as_double(a, &x)) { error("ceil() requires a number"); stdlib_push(vm, aer_null()); return true; }
        stdlib_push(vm, aer_int((long long)ceil(x))); return true;
    }
    if (strcmp(name, "abs") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        if (aer_type(a) == TYPE_INTEGER) {
            long long n = aer_as_int(a);
            stdlib_push(vm, aer_int(n < 0 ? -n : n)); return true;
        }
        if (aer_type(a) == TYPE_REAL) {
            double d = aer_as_real(a);
            stdlib_push(vm, aer_real(d < 0 ? -d : d)); return true;
        }
        error("abs() requires a number"); stdlib_push(vm, aer_null()); return true;
    }
    if (strcmp(name, "min") == 0 && arg_count == 2) {
        AerVal b = stdlib_pop(vm); AerVal a = stdlib_pop(vm);
        double da, db;
        if (!as_double(a, &da) || !as_double(b, &db)) { error("min() requires two numbers"); stdlib_push(vm, aer_null()); return true; }
        stdlib_push(vm, da <= db ? a : b); return true;
    }
    if (strcmp(name, "max") == 0 && arg_count == 2) {
        AerVal b = stdlib_pop(vm); AerVal a = stdlib_pop(vm);
        double da, db;
        if (!as_double(a, &da) || !as_double(b, &db)) { error("max() requires two numbers"); stdlib_push(vm, aer_null()); return true; }
        stdlib_push(vm, da >= db ? a : b); return true;
    }
    if (strcmp(name, "sin") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        double x;
        if (!as_double(a, &x)) { error("sin() requires a number"); stdlib_push(vm, aer_null()); return true; }
        stdlib_push(vm, aer_real(sin(x))); return true;
    }
    if (strcmp(name, "cos") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        double x;
        if (!as_double(a, &x)) { error("cos() requires a number"); stdlib_push(vm, aer_null()); return true; }
        stdlib_push(vm, aer_real(cos(x))); return true;
    }
    if (strcmp(name, "log") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        double x;
        if (!as_double(a, &x)) { error("log() requires a number"); stdlib_push(vm, aer_null()); return true; }
        if (x <= 0) { error("log() requires a positive number"); stdlib_push(vm, aer_null()); return true; }
        stdlib_push(vm, aer_real(log(x))); return true;
    }
    if (strcmp(name, "log2") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        double x;
        if (!as_double(a, &x)) { error("log2() requires a number"); stdlib_push(vm, aer_null()); return true; }
        if (x <= 0) { error("log2() requires a positive number"); stdlib_push(vm, aer_null()); return true; }
        stdlib_push(vm, aer_real(log2(x))); return true;
    }
    if (strcmp(name, "log10") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        double x;
        if (!as_double(a, &x)) { error("log10() requires a number"); stdlib_push(vm, aer_null()); return true; }
        if (x <= 0) { error("log10() requires a positive number"); stdlib_push(vm, aer_null()); return true; }
        stdlib_push(vm, aer_real(log10(x))); return true;
    }
    if (strcmp(name, "pi") == 0 && arg_count == 0) {
        /* A function, not a bare module value, for consistency with every other native module (none expose non-function bindings yet); literal digits rather than M_PI, which isn't guaranteed defined on every target toolchain. */
        stdlib_push(vm, aer_real(3.14159265358979323846)); return true;
    }
    if (strcmp(name, "sort") == 0 && arg_count == 1) {
        AerVal arr = stdlib_pop(vm);
        if (aer_type(arr) != TYPE_ARRAY) { error("sort() requires an array"); stdlib_push(vm, aer_null()); return true; }
        AerArray* a = aer_as_array(arr);
        if (a->shape) { error("sort() cannot sort a struct instance"); stdlib_push(vm, aer_null()); return true; }
        /* Ordering across mixed types has no sensible answer, so it's rejected up front rather than falling back to an arbitrary tie-break. */
        bool numeric = true, stringy = true;
        for (unsigned int i = 0; i < a->count; i++) {
            if (aer_type(a->items[i]) != TYPE_INTEGER && aer_type(a->items[i]) != TYPE_REAL) numeric = false;
            if (aer_type(a->items[i]) != TYPE_STRING) stringy = false;
        }
        if (a->count > 0 && !numeric && !stringy) {
            error("sort() requires all elements to be numbers, or all to be strings");
            stdlib_push(vm, aer_null()); return true;
        }
        qsort(a->items, a->count, sizeof(AerVal), sort_cmp);
        stdlib_push(vm, arr); return true;
    }

    return false;
}

bool aer_random_call(VM* vm, Chunk* c, const char* name, int arg_count) {
    (void)c;

    if (strcmp(name, "random") == 0 && arg_count == 0) {
        stdlib_push(vm, aer_real((double)rand() / ((double)RAND_MAX + 1.0))); return true;
    }
    if (strcmp(name, "randint") == 0 && arg_count == 2) {
        AerVal hi = stdlib_pop(vm); AerVal lo = stdlib_pop(vm);
        if (aer_type(lo) != TYPE_INTEGER || aer_type(hi) != TYPE_INTEGER) { error("randint() requires two integers"); stdlib_push(vm, aer_null()); return true; }
        long long lo_n = aer_as_int(lo), hi_n = aer_as_int(hi);
        if (hi_n < lo_n) { error("randint() requires min <= max"); stdlib_push(vm, aer_null()); return true; }
        long long span = hi_n - lo_n + 1;
        /* rand() % span is slightly biased toward the low end for spans that don't evenly divide RAND_MAX+1 — a known simplification; rejection sampling would fix it but isn't worth the complexity. */
        stdlib_push(vm, aer_int(lo_n + (long long)(rand() % span))); return true;
    }
    if (strcmp(name, "seed") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        if (aer_type(a) != TYPE_INTEGER) { error("seed() requires an integer"); stdlib_push(vm, aer_null()); return true; }
        srand((unsigned int)aer_as_int(a));
        stdlib_push(vm, aer_null()); return true;
    }

    return false;
}

bool aer_string_call(VM* vm, Chunk* c, const char* name, int arg_count) {
    (void)c;

    if (strcmp(name, "upper") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        if (aer_type(a) != TYPE_STRING) { error("string.upper() requires a string"); stdlib_push(vm, aer_null()); return true; }
        AerString* as = aer_as_string(a);
        unsigned int len = as->length;
        char* buf = xmalloc(len + 1);
        for (unsigned int i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)as->data[i]);
        buf[len] = '\0';
        stdlib_push(vm, aer_make_string(buf, len)); return true;
    }
    if (strcmp(name, "lower") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        if (aer_type(a) != TYPE_STRING) { error("string.lower() requires a string"); stdlib_push(vm, aer_null()); return true; }
        AerString* as = aer_as_string(a);
        unsigned int len = as->length;
        char* buf = xmalloc(len + 1);
        for (unsigned int i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)as->data[i]);
        buf[len] = '\0';
        stdlib_push(vm, aer_make_string(buf, len)); return true;
    }
    if (strcmp(name, "trim") == 0 && arg_count == 1) {
        AerVal a = stdlib_pop(vm);
        if (aer_type(a) != TYPE_STRING) { error("string.trim() requires a string"); stdlib_push(vm, aer_null()); return true; }
        AerString* as = aer_as_string(a);
        const char* data = as->data;
        unsigned int start = 0, end = as->length;
        while (start < end && isspace((unsigned char)data[start]))     start++;
        while (end > start && isspace((unsigned char)data[end - 1]))   end--;
        unsigned int n = end - start;
        char* buf = xmalloc(n + 1);
        memcpy(buf, data + start, n);
        buf[n] = '\0';
        stdlib_push(vm, aer_make_string(buf, n)); return true;
    }
    if (strcmp(name, "contains") == 0 && arg_count == 2) {
        AerVal needle = stdlib_pop(vm); AerVal hay = stdlib_pop(vm);
        if (aer_type(hay) != TYPE_STRING || aer_type(needle) != TYPE_STRING) { error("string.contains() requires two strings"); stdlib_push(vm, aer_null()); return true; }
        AerString* hs = aer_as_string(hay);
        AerString* ns = aer_as_string(needle);
        unsigned int hlen = hs->length, nlen = ns->length;
        bool found = nlen == 0;
        for (unsigned int i = 0; !found && i + nlen <= hlen; i++)
            if (memcmp(hs->data + i, ns->data, nlen) == 0) found = true;
        stdlib_push(vm, aer_bool(found)); return true;
    }
    if (strcmp(name, "split") == 0 && arg_count == 2) {
        AerVal sep_v = stdlib_pop(vm); AerVal s_v = stdlib_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(sep_v) != TYPE_STRING) { error("string.split() requires two strings"); stdlib_push(vm, aer_null()); return true; }
        AerString* ss = aer_as_string(s_v);
        AerString* seps = aer_as_string(sep_v);
        unsigned int slen = ss->length, seplen = seps->length;
        if (seplen == 0) { error("string.split() separator cannot be empty"); stdlib_push(vm, aer_null()); return true; }
        const char* s = ss->data;
        const char* sep = seps->data;

        AerArray* r = vm_new_array();
        r->count = 0;
        r->capacity = 4;
        r->items = xmalloc(sizeof(AerVal) * r->capacity);
        r->shape = NULL;

        unsigned int seg_start = 0, i = 0;
        while (i <= slen) {
            bool at_sep = i + seplen <= slen && memcmp(s + i, sep, seplen) == 0;
            if (at_sep || i == slen) {
                unsigned int n = i - seg_start;
                char* buf = xmalloc(n + 1);
                memcpy(buf, s + seg_start, n);
                buf[n] = '\0';
                if (r->count >= r->capacity) {
                    r->capacity *= 2;
                    r->items = xrealloc(r->items, sizeof(AerVal) * r->capacity);
                }
                r->items[r->count++] = aer_make_string(buf, n);
                if (i == slen) break;
                i += seplen;
                seg_start = i;
            } else {
                i++;
            }
        }
        stdlib_push(vm, aer_array_val(r)); return true;
    }
    if (strcmp(name, "starts_with") == 0 && arg_count == 2) {
        AerVal prefix_v = stdlib_pop(vm); AerVal s_v = stdlib_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(prefix_v) != TYPE_STRING) { error("string.starts_with() requires two strings"); stdlib_push(vm, aer_null()); return true; }
        AerString* ss = aer_as_string(s_v);
        AerString* ps = aer_as_string(prefix_v);
        bool matches = ps->length <= ss->length && memcmp(ss->data, ps->data, ps->length) == 0;
        stdlib_push(vm, aer_bool(matches)); return true;
    }
    if (strcmp(name, "ends_with") == 0 && arg_count == 2) {
        AerVal suffix_v = stdlib_pop(vm); AerVal s_v = stdlib_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(suffix_v) != TYPE_STRING) { error("string.ends_with() requires two strings"); stdlib_push(vm, aer_null()); return true; }
        AerString* ss = aer_as_string(s_v);
        AerString* fs = aer_as_string(suffix_v);
        bool matches = fs->length <= ss->length && memcmp(ss->data + (ss->length - fs->length), fs->data, fs->length) == 0;
        stdlib_push(vm, aer_bool(matches)); return true;
    }
    if (strcmp(name, "repeat") == 0 && arg_count == 2) {
        AerVal n_v = stdlib_pop(vm); AerVal s_v = stdlib_pop(vm);
        if (aer_type(s_v) != TYPE_STRING) { error("string.repeat() requires a string"); stdlib_push(vm, aer_null()); return true; }
        if (aer_type(n_v) != TYPE_INTEGER) { error("string.repeat() count must be an integer"); stdlib_push(vm, aer_null()); return true; }
        long long n = aer_as_int(n_v);
        if (n < 0) { error("string.repeat() count must not be negative"); stdlib_push(vm, aer_null()); return true; }
        AerString* ss = aer_as_string(s_v);
        unsigned int total = ss->length * (unsigned int)n;
        char* buf = xmalloc(total + 1);
        for (long long i = 0; i < n; i++) memcpy(buf + (unsigned int)i * ss->length, ss->data, ss->length);
        buf[total] = '\0';
        stdlib_push(vm, aer_make_string(buf, total)); return true;
    }
    if (strcmp(name, "replace") == 0 && arg_count == 3) {
        AerVal new_v = stdlib_pop(vm); AerVal old_v = stdlib_pop(vm); AerVal s_v = stdlib_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(old_v) != TYPE_STRING || aer_type(new_v) != TYPE_STRING) {
            error("string.replace() requires three strings"); stdlib_push(vm, aer_null()); return true;
        }
        AerString* ss  = aer_as_string(s_v);
        AerString* os  = aer_as_string(old_v);
        AerString* nsv = aer_as_string(new_v);
        if (os->length == 0) { error("string.replace() 'old' argument cannot be empty"); stdlib_push(vm, aer_null()); return true; }

        /* Two passes — count matches first so the output buffer is sized exactly once, same discipline string.join uses, rather than a growable buffer. */
        unsigned int matches = 0;
        for (unsigned int i = 0; i + os->length <= ss->length; ) {
            if (memcmp(ss->data + i, os->data, os->length) == 0) { matches++; i += os->length; }
            else i++;
        }
        unsigned int total = ss->length - matches * os->length + matches * nsv->length;
        char* buf = xmalloc(total + 1);
        unsigned int pos = 0;
        for (unsigned int i = 0; i < ss->length; ) {
            if (i + os->length <= ss->length && memcmp(ss->data + i, os->data, os->length) == 0) {
                memcpy(buf + pos, nsv->data, nsv->length);
                pos += nsv->length;
                i   += os->length;
            } else {
                buf[pos++] = ss->data[i++];
            }
        }
        buf[total] = '\0';
        stdlib_push(vm, aer_make_string(buf, total)); return true;
    }
    if (strcmp(name, "join") == 0 && arg_count == 2) {
        AerVal sep_v = stdlib_pop(vm); AerVal arr_v = stdlib_pop(vm);
        if (aer_type(arr_v) != TYPE_ARRAY) { error("string.join() requires an array"); stdlib_push(vm, aer_null()); return true; }
        if (aer_type(sep_v) != TYPE_STRING) { error("string.join() separator must be a string"); stdlib_push(vm, aer_null()); return true; }
        AerArray* arr = aer_as_array(arr_v);
        for (unsigned int i = 0; i < arr->count; i++)
            if (aer_type(arr->items[i]) != TYPE_STRING) { error("string.join() requires an array of strings"); stdlib_push(vm, aer_null()); return true; }

        AerString* seps = aer_as_string(sep_v);
        unsigned int seplen = seps->length;
        unsigned int total = arr->count > 0 ? (arr->count - 1) * seplen : 0;
        for (unsigned int i = 0; i < arr->count; i++) total += aer_as_string(arr->items[i])->length;

        char* buf = xmalloc(total + 1);
        unsigned int pos = 0;
        for (unsigned int i = 0; i < arr->count; i++) {
            if (i > 0) { memcpy(buf + pos, seps->data, seplen); pos += seplen; }
            AerString* item = aer_as_string(arr->items[i]);
            unsigned int n = item->length;
            memcpy(buf + pos, item->data, n);
            pos += n;
        }
        buf[total] = '\0';
        stdlib_push(vm, aer_make_string(buf, total)); return true;
    }

    return false;
}

bool aer_time_call(VM* vm, Chunk* c, const char* name, int arg_count) {
    (void)c;

    if (strcmp(name, "now") == 0 && arg_count == 0) {
        /* Sub-second epoch time via clock_gettime(CLOCK_REALTIME), not time()'s whole seconds, so scripts can measure short durations; used since C11's timespec_get() isn't available on this project's MinGW-w64 target. */
        struct timespec ts = {0};
        clock_gettime(CLOCK_REALTIME, &ts);
        stdlib_push(vm, aer_real((double)ts.tv_sec + (double)ts.tv_nsec / 1e9)); return true;
    }

    if (strcmp(name, "strftime") == 0 && arg_count == 2) {
        AerVal fmt_v = stdlib_pop(vm); AerVal ts_v = stdlib_pop(vm);
        if (aer_type(fmt_v) != TYPE_STRING) { error("time.strftime() requires a format string"); stdlib_push(vm, aer_null()); return true; }
        double ts_num;
        if (!as_double(ts_v, &ts_num)) { error("time.strftime() requires a numeric timestamp (e.g. from time.now())"); stdlib_push(vm, aer_null()); return true; }
        AerString* fs = aer_as_string(fmt_v);
        unsigned int flen = fs->length;
        if (flen > 255) { error("time.strftime() format string too long (max 255 bytes)"); stdlib_push(vm, aer_null()); return true; }
        char fmt_buf[256];
        memcpy(fmt_buf, fs->data, flen);
        fmt_buf[flen] = '\0';

        time_t t = (time_t)ts_num;
        struct tm* tmv = localtime(&t);
        char out[256];
        size_t n = strftime(out, sizeof(out), fmt_buf, tmv);
        char* buf = xmalloc(n + 1);
        memcpy(buf, out, n + 1);
        stdlib_push(vm, aer_make_string(buf, (unsigned int)n)); return true;
    }

    return false;
}

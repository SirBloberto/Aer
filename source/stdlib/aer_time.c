#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>   /* Sleep() -- MinGW's nanosleep needs winpthreads, which the static CLI build doesn't link */
#endif
#include "aer_stdlib.h"
#include "error.h"

/* Hand-rolled rather than strptime(): unlike strftime(), it isn't reliably present on the MinGW
   target (confirmed absent -- implicit-declaration error at compile time), so a minimal parser
   for the common format codes stands in, same pattern as time.sleep()'s own platform split. */
static bool parse_digits(const char** s, int max_digits, int* out) {
    int n = 0, count = 0;
    while (count < max_digits && **s >= '0' && **s <= '9') {
        n = n * 10 + (**s - '0');
        (*s)++;
        count++;
    }
    if (count == 0) return false;
    *out = n;
    return true;
}

/* %Y %m %d %H %M %S %% only; every other format char must match the input literally. Requires
   the whole input to be consumed -- a trailing mismatch is a parse failure, not a partial match. */
static bool time_parse_impl(const char* input, const char* fmt, struct tm* tm) {
    memset(tm, 0, sizeof(*tm));
    tm->tm_mday = 1;
    const char* s = input;
    const char* f = fmt;
    while (*f) {
        if (*f == '%') {
            f++;
            int val;
            switch (*f) {
                case 'Y': if (!parse_digits(&s, 4, &val)) return false; tm->tm_year = val - 1900; break;
                case 'm': if (!parse_digits(&s, 2, &val)) return false; tm->tm_mon  = val - 1;    break;
                case 'd': if (!parse_digits(&s, 2, &val)) return false; tm->tm_mday = val;         break;
                case 'H': if (!parse_digits(&s, 2, &val)) return false; tm->tm_hour = val;         break;
                case 'M': if (!parse_digits(&s, 2, &val)) return false; tm->tm_min  = val;         break;
                case 'S': if (!parse_digits(&s, 2, &val)) return false; tm->tm_sec  = val;         break;
                case '%': if (*s != '%') return false; s++; break;
                default: return false;   /* unsupported format code */
            }
            f++;
        } else {
            if (*s != *f) return false;
            s++; f++;
        }
    }
    return *s == '\0';
}

bool aer_time_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_TIME_NOW && arg_count == 0) {
        /* Sub-second epoch time via clock_gettime(CLOCK_REALTIME), not time()'s whole seconds, so scripts can measure short durations; used since C11's timespec_get() isn't available on this project's MinGW-w64 target. */
        struct timespec ts = {0};
        clock_gettime(CLOCK_REALTIME, &ts);
        vm_stack_push(vm, aer_real((double)ts.tv_sec + (double)ts.tv_nsec / 1e9)); return true;
    }

    if (fn_id == FN_TIME_SLEEP && arg_count == 1) {
        AerVal a = vm_stack_pop(vm);
        double secs;
        if (!aer_as_double(a, &secs) || secs < 0) { error("time.sleep() requires a non-negative number of seconds"); vm_stack_push(vm, aer_null()); return true; }
#ifdef _WIN32
        Sleep((DWORD)(secs * 1000.0));
#else
        struct timespec req;
        req.tv_sec  = (time_t)secs;
        req.tv_nsec = (long)((secs - (double)req.tv_sec) * 1e9);
        nanosleep(&req, NULL);
#endif
        vm_stack_push(vm, aer_null()); return true;
    }

    if (fn_id == FN_TIME_STRFTIME && arg_count == 2) {
        AerVal fmt_v = vm_stack_pop(vm); AerVal ts_v = vm_stack_pop(vm);
        if (aer_type(fmt_v) != TYPE_STRING) { error("time.strftime() requires a format string"); vm_stack_push(vm, aer_null()); return true; }
        double ts_num;
        if (!aer_as_double(ts_v, &ts_num)) { error("time.strftime() requires a numeric timestamp (e.g. from time.now())"); vm_stack_push(vm, aer_null()); return true; }
        AerString* fs = aer_as_string(fmt_v);
        unsigned int flen = fs->length;
        if (flen > 255) { error("time.strftime() format string too long (max 255 bytes)"); vm_stack_push(vm, aer_null()); return true; }
        char fmt_buf[256];
        memcpy(fmt_buf, fs->data, flen);
        fmt_buf[flen] = '\0';

        time_t t = (time_t)ts_num;
        struct tm* tmv = localtime(&t);
        char out[256];
        size_t n = strftime(out, sizeof(out), fmt_buf, tmv);
        char* buf = xmalloc(n + 1);
        memcpy(buf, out, n + 1);
        vm_stack_push(vm, aer_make_string(buf, (unsigned int)n)); return true;
    }

    if (fn_id == FN_TIME_PARSE && arg_count == 2) {
        AerVal fmt_v = vm_stack_pop(vm); AerVal str_v = vm_stack_pop(vm);
        if (aer_type(str_v) != TYPE_STRING || aer_type(fmt_v) != TYPE_STRING) {
            error("time.parse() requires a date string and a format string");
            vm_stack_push(vm, aer_null()); return true;
        }
        struct tm tm;
        if (!time_parse_impl(aer_as_string(str_v)->data, aer_as_string(fmt_v)->data, &tm)) {
            error("time.parse(): '%s' does not match format '%s'", aer_as_string(str_v)->data, aer_as_string(fmt_v)->data);
            vm_stack_push(vm, aer_null()); return true;
        }
        tm.tm_isdst = -1;   /* let mktime figure out DST */
        time_t t = mktime(&tm);
        if (t == (time_t)-1) {
            error("time.parse(): the parsed date/time is not representable");
            vm_stack_push(vm, aer_null()); return true;
        }
        vm_stack_push(vm, aer_real((double)t)); return true;
    }

    if (fn_id == FN_TIME_TO_PARTS && arg_count == 1) {
        AerVal ts_v = vm_stack_pop(vm);
        double ts_num;
        if (!aer_as_double(ts_v, &ts_num)) { error("time.to_parts() requires a numeric timestamp (e.g. from time.now())"); vm_stack_push(vm, aer_null()); return true; }
        time_t t = (time_t)ts_num;
        struct tm* tmv = localtime(&t);
        AerDict* d = vm_new_dict();
        const char* names[7]  = { "year", "month", "day", "hour", "min", "sec", "weekday" };
        int         values[7] = { tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday, tmv->tm_hour, tmv->tm_min, tmv->tm_sec, tmv->tm_wday };
        for (int i = 0; i < 7; i++) {
            unsigned int klen = (unsigned int)strlen(names[i]);
            char* k = hashtable_key_dup(d->map.pools, names[i], klen, NULL);
            hashtable_put(&d->map, k, klen, aer_int(values[i]));
        }
        vm_stack_push(vm, aer_dict_val(d)); return true;
    }

    if (fn_id == FN_TIME_FROM_PARTS && arg_count == 1) {
        AerVal d_v = vm_stack_pop(vm);
        if (aer_type(d_v) != TYPE_DICT) { error("time.from_parts() requires a dict (e.g. from time.to_parts())"); vm_stack_push(vm, aer_null()); return true; }
        AerDict* d = aer_as_dict(d_v);
        const char* required[6] = { "year", "month", "day", "hour", "min", "sec" };
        int64_t parts[6];
        for (int i = 0; i < 6; i++) {
            AerVal* found = hashtable_get(&d->map, required[i], (unsigned int)strlen(required[i]));
            if (!found || aer_type(*found) != TYPE_INTEGER) {
                error("time.from_parts(): dict must have an integer '%s' field", required[i]);
                vm_stack_push(vm, aer_null()); return true;
            }
            parts[i] = aer_as_int(*found);
        }
        struct tm tm = {0};
        tm.tm_year  = (int)parts[0] - 1900;
        tm.tm_mon   = (int)parts[1] - 1;
        tm.tm_mday  = (int)parts[2];
        tm.tm_hour  = (int)parts[3];
        tm.tm_min   = (int)parts[4];
        tm.tm_sec   = (int)parts[5];
        tm.tm_isdst = -1;
        time_t t = mktime(&tm);
        if (t == (time_t)-1) {
            error("time.from_parts(): the given date/time is not representable");
            vm_stack_push(vm, aer_null()); return true;
        }
        vm_stack_push(vm, aer_real((double)t)); return true;
    }

    return false;
}

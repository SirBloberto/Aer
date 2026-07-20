#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>   /* Sleep() — MinGW's nanosleep needs winpthreads, which the static CLI build doesn't link */
#endif
#include "aer_stdlib.h"
#include "error.h"

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

    return false;
}

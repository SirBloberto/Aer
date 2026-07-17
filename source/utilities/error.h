#ifndef ERROR_H
#define ERROR_H

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MinGW's longjmp() goes through SEH-validating _setjmpex(); unwinding into vm_run's computed-goto
   dispatch loop crashes the process (STATUS_BAD_STACK) instead of unwinding. __builtin_setjmp/
   __builtin_longjmp are GCC's own low-level primitives — no SEH involvement, so this works there.
   They skip signal-mask save/restore compared to the real setjmp/longjmp, but nothing in this
   codebase's error unwinding touches signals, so that's not a loss here. Linux/macOS never hit the
   SEH problem, so they keep the standard, fully-portable setjmp/longjmp. */
#ifdef __MINGW32__
typedef intptr_t AerJmpBuf[5];
#define AER_SETJMP(buf)       __builtin_setjmp(buf)
#define AER_LONGJMP(buf, val) __builtin_longjmp(buf, 1)   /* __builtin_longjmp's val must be the literal 1 */
#else
typedef jmp_buf AerJmpBuf;
#define AER_SETJMP(buf)       setjmp(buf)
#define AER_LONGJMP(buf, val) longjmp(buf, val)
#endif

typedef enum Mode {
    MODE_SHELL,
    MODE_RUN
} Mode;

extern Mode         mode;
extern bool         parse_had_error;
extern bool         runtime_had_error;

/* NULL outside any vm_run() call (e.g. while parsing) — error()/error_at() only unwind when this
   is set, so a parse-time error keeps its old "set flags, return normally" behavior and the
   parser's own recursive-descent recovery still runs unchanged. Set by vm_run() itself (vm.c) to
   the address of an AerJmpBuf local to that call, saving/restoring whatever was there before, so
   nested vm_run() calls (cross-module calls) each catch their own errors locally — nothing about
   the flag-based cascade aer_module_call/aer_module_load already do (checking runtime_had_error
   right after their own nested vm_run() returns) needed to change. */
extern AerJmpBuf* runtime_error_unwind_target;

/* Incremented by assert() on failure; deliberately not runtime_had_error, since DISPATCH() aborts vm_run on that flag but a failed assertion should report and keep going. */
extern unsigned int assert_failure_count;

/* Set once (vm.c) to look up the executing instruction's source line for error()'s runtime faults; called lazily only inside error() (not every dispatch), so error-free runs pay nothing; NULL or a 0 return both mean "unknown," and error() omits the "Line N: " prefix rather than print a misleading "Line 0". */
extern unsigned int (*runtime_line_lookup)(void);

void error(const char* format, ...) __attribute__((cold));
void error_at(const char* format, ...) __attribute__((cold));

/* Terminates the process for a condition error recovery doesn't apply to (OOM) — routes through the same sink as error()/error_at() first; the only thing here still allowed to call exit(). */
void aer_report_fatal(const char* msg);

/* malloc/calloc/realloc/strdup, but fatal (via aer_report_fatal) on failure instead of returning NULL — every call site is spared its own check. */
void* xmalloc(size_t size);
void* xcalloc(size_t count, size_t size);
void* xrealloc(void* ptr, size_t size);
char* xstrdup(const char* s);

#endif

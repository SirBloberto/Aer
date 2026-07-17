#ifndef ERROR_H
#define ERROR_H

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum Mode {
    MODE_SHELL,
    MODE_RUN
} Mode;

extern Mode         mode;
extern bool         parse_had_error;
extern bool         runtime_had_error;

/* NULL outside any vm_run() call (e.g. while parsing) — error()/error_at() only longjmp when this
   is set, so a parse-time error keeps its old "set flags, return normally" behavior and the
   parser's own recursive-descent recovery still runs unchanged. Set by vm_run() itself (vm.c) to
   the address of a jmp_buf local to that call, saving/restoring whatever was there before, so
   nested vm_run() calls (cross-module calls) each catch their own errors locally — nothing about
   the flag-based cascade aer_module_call/aer_module_load already do (checking runtime_had_error
   right after their own nested vm_run() returns) needed to change. */
extern jmp_buf* runtime_error_unwind_target;

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

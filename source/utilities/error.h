#ifndef ERROR_H
#define ERROR_H

#include <stdbool.h>
#include <stddef.h>

typedef enum Mode {
    MODE_SHELL,
    MODE_RUN
} Mode;

extern Mode         mode;
extern bool         parse_had_error;
extern bool         runtime_had_error;

/* Incremented by assert() on failure; deliberately not runtime_had_error, since DISPATCH() aborts vm_run on that flag but a failed assertion should report and keep going. */
extern unsigned int assert_failure_count;

/* Set once (vm.c) to look up the executing instruction's source line for error()'s runtime faults; called lazily only inside error() (not every dispatch), so error-free runs pay nothing; NULL or a 0 return both mean "unknown," and error() omits the "Line N: " prefix rather than print a misleading "Line 0". */
extern unsigned int (*runtime_line_lookup)(void);

void error(char* format, ...);
void error_at(char* format, ...);

/* Terminates the process for a condition error recovery doesn't apply to (OOM) — routes through the same sink as error()/error_at() first; the only thing here still allowed to call exit(). */
void aer_report_fatal(const char* msg);

/* malloc/calloc/realloc/strdup, but fatal (via aer_report_fatal) on failure instead of returning NULL — every call site is spared its own check. */
void* xmalloc(size_t size);
void* xcalloc(size_t count, size_t size);
void* xrealloc(void* ptr, size_t size);
char* xstrdup(const char* s);

#endif

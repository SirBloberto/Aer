#ifndef ERROR_H
#define ERROR_H

/* Per-thread once actors run in parallel, so one worker's error is not read as another's. */
#ifdef AER_HEAP_REF_TLS
#define AER_TLS _Thread_local
#else
#define AER_TLS
#endif

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MinGW's longjmp is SEH-validated and crashes (STATUS_BAD_STACK) unwinding into vm_run's
   computed-goto loop; GCC's __builtin_ primitives skip SEH entirely. */
#ifdef __MINGW32__
typedef intptr_t AerJmpBuf[5];
#define AER_SETJMP(buf) __builtin_setjmp(buf)
#define AER_LONGJMP(buf, val) __builtin_longjmp(buf, 1) /* __builtin_longjmp's val must be the literal 1 */
#else
typedef jmp_buf AerJmpBuf;
#define AER_SETJMP(buf) setjmp(buf)
#define AER_LONGJMP(buf, val) longjmp(buf, val)
#endif

typedef enum Mode { MODE_SHELL, MODE_RUN } Mode;

extern Mode mode;
extern AER_TLS bool parse_had_error;
extern AER_TLS bool runtime_had_error;

/* error() unwinds to this when set; NULL (outside any vm_run) means "set flags, return normally"
   so parse-time recovery runs unchanged. Nested vm_run calls save/restore it. */
extern AerJmpBuf* runtime_error_unwind_target;

/* Deliberately not runtime_had_error: a failed assertion reports and keeps going. */
extern AER_TLS unsigned int assert_failure_count;

/* Source-line lookup for runtime errors; NULL or a 0 return means "unknown", no line prefix. */
extern unsigned int (*runtime_line_lookup)(void);

/* Source filename for the currently-running chunk; NULL means unknown. */
extern const char* (*runtime_filename_lookup)(void);

/* Name of the function the current frame is executing; NULL at top level. */
extern const char* (*runtime_function_lookup)(void);

/* Appends "called from ..." trace lines into `out`; returns bytes written. */
extern unsigned int (*runtime_stack_trace_lookup)(char* out, unsigned int out_size);

void error(const char* format, ...) __attribute__((cold));
void error_at(const char* format, ...) __attribute__((cold));

/* Structured alongside the plain-text sink above (aer_set_error_callback, include/aer.h) -- for a
   tool that wants (line, column, message) fields directly instead of scraping them back out of a
   formatted "N | source line\n    ^\nError: msg" string. line/col are 1-based; col 0 means
   "unknown position" (the plain error() path, which has no source cursor to measure from unless
   runtime_line_lookup is active). */
typedef void (*AerDiagnosticCallback)(unsigned int line, unsigned int col, const char* message,
                                      void* userdata);
void aer_set_diagnostic_callback(AerDiagnosticCallback callback, void* userdata);

/* For a condition recovery cannot apply to, such as OOM. The only thing here that calls exit(). */
void aer_report_fatal(const char* msg);

/* Fatal on failure rather than returning NULL, so no call site needs its own check. */
void* xmalloc(size_t size);
void* xcalloc(size_t count, size_t size);
void* xrealloc(void* ptr, size_t size);
char* xstrdup(const char* s);

#endif

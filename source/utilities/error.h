#ifndef ERROR_H
#define ERROR_H

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
extern bool parse_had_error;
extern bool runtime_had_error;

/* error() unwinds to this when set; NULL (outside any vm_run) means "set flags, return normally"
   so parse-time recovery runs unchanged. Nested vm_run calls save/restore it. */
extern AerJmpBuf* runtime_error_unwind_target;

/* Incremented by assert() on failure; deliberately not runtime_had_error, since DISPATCH() aborts vm_run on that flag but a failed assertion should report and keep going. */
extern unsigned int assert_failure_count;

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

/* Terminates the process for a condition error recovery doesn't apply to (OOM) -- routes through the same sink as error()/error_at() first; the only thing here still allowed to call exit(). */
void aer_report_fatal(const char* msg);

/* malloc/calloc/realloc/strdup, but fatal (via aer_report_fatal) on failure instead of returning NULL -- every call site is spared its own check. */
void* xmalloc(size_t size);
void* xcalloc(size_t count, size_t size);
void* xrealloc(void* ptr, size_t size);
char* xstrdup(const char* s);

/* The interpreter's dispatch table base, pinned -- under PIC its address is no link-time constant,
   so otherwise it is rebuilt from `pc` on every opcode (5.16s). It belongs in this header, not
   beside its use, because that IS the correctness condition: every translation unit LTO'd together
   must see it. Declaring it only in vm.c let -flto inline parser.c code that still thought r8 was
   free, and a parser pointer took the dispatch base's value, freeing &dt (5.16t). r8 is
   callee-saved under AAPCS, so libc preserves it across calls. */
#if defined(__arm__) && defined(__GNUC__) && !defined(__clang__)
#define AER_PINNED_DISPATCH 1
register const void* const* aer_dispatch_base asm("r8");
#endif

#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "error.h"
#include "lexer.h"

bool         parse_had_error    = false;
bool         runtime_had_error  = false;
unsigned int assert_failure_count = 0;
unsigned int (*runtime_line_lookup)(void) = NULL;
AerJmpBuf*   runtime_error_unwind_target  = NULL;

#define ERROR_MSG_MAX 2048

static char             last_error_msg[ERROR_MSG_MAX] = "";
static AerErrorCallback error_callback                = NULL;
static void*            error_callback_userdata       = NULL;

/* Appends a formatted piece to buf[*pos..], clamped to bufsize, so error formatting can never overrun its buffer regardless of source-line or message length. */
static void append_fmt(char* buf, size_t bufsize, size_t* pos, const char* fmt, ...) {
    if (*pos >= bufsize) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *pos, bufsize - *pos, fmt, args);
    va_end(args);
    if (n < 0) return;
    *pos += (size_t)n;
    if (*pos >= bufsize) *pos = bufsize - 1;
}

/* Sends a fully-formatted message to whichever sink is active — a host-registered callback, or stderr by default (unchanged CLI/REPL behavior) — and updates aer_last_error() either way. */
static void emit_error(const char* msg) {
    strncpy(last_error_msg, msg, ERROR_MSG_MAX - 1);
    last_error_msg[ERROR_MSG_MAX - 1] = '\0';
    if (error_callback) error_callback(last_error_msg, error_callback_userdata);
    else                fprintf(stderr, "%s", last_error_msg);
}

void aer_set_error_callback(AerErrorCallback callback, void* userdata) {
    error_callback          = callback;
    error_callback_userdata = userdata;
}

const char* aer_last_error(void) { return last_error_msg; }

bool aer_had_error(void) { return parse_had_error || runtime_had_error; }

unsigned int aer_assert_failure_count(void) { return assert_failure_count; }

void aer_clear_error(void) {
    parse_had_error     = false;
    runtime_had_error   = false;
    assert_failure_count = 0;
    last_error_msg[0]   = '\0';
}

/* The only thing left in this codebase allowed to exit() — for OOM; doesn't solve recoverable OOM, just guarantees a host-registered sink sees the message before the process goes down. */
void aer_report_fatal(const char* msg) {
    char buf[ERROR_MSG_MAX];
    snprintf(buf, sizeof(buf), "Error: %s\n", msg);
    emit_error(buf);
    exit(1);
}

/* Allocation wrappers — aer_report_fatal() never returns, so these never return NULL; every call site is spared its own "if (!p) ..." check. */
void* xmalloc(size_t size) {
    void* p = malloc(size);
    if (!p) aer_report_fatal("Out of memory");
    return p;
}

void* xcalloc(size_t count, size_t size) {
    void* p = calloc(count, size);
    if (!p) aer_report_fatal("Out of memory");
    return p;
}

void* xrealloc(void* ptr, size_t size) {
    void* p = realloc(ptr, size);
    if (!p) aer_report_fatal("Out of memory");
    return p;
}

char* xstrdup(const char* s) {
    char* p = strdup(s);
    if (!p) aer_report_fatal("Out of memory");
    return p;
}

void error(const char* format, ...) {
    char   buf[ERROR_MSG_MAX];
    size_t pos = 0;
    unsigned int line = runtime_line_lookup ? runtime_line_lookup() : 0;
    if (line > 0) append_fmt(buf, sizeof(buf), &pos, "Line %u: ", line);
    append_fmt(buf, sizeof(buf), &pos, "Error: ");

    va_list args;
    va_start(args, format);
    if (pos < sizeof(buf)) {
        int n = vsnprintf(buf + pos, sizeof(buf) - pos, format, args);
        if (n > 0) pos += (size_t)n;
        if (pos >= sizeof(buf)) pos = sizeof(buf) - 1;
    }
    va_end(args);

    append_fmt(buf, sizeof(buf), &pos, "\n");
    emit_error(buf);

    parse_had_error   = true;
    runtime_had_error = true;

    /* Jumps straight back to the currently-executing vm_run() call's own dispatch loop instead of
       waiting for its next DISPATCH() to notice the flag (there is no such per-instruction check
       anymore — see DISPATCH()'s own comment, vm.c). NULL here means no vm_run() call is active
       (a parse-time error, or error()/error_at() called from parser.c) — falls through to the
       same "set flags, return normally" behavior this always had, letting the parser's own
       recursive-descent recovery run unchanged. */
    if (runtime_error_unwind_target) AER_LONGJMP(*runtime_error_unwind_target, 1);
}

/* Print a message pinpointing the current token in the source. */
void error_at(const char* format, ...) {
    const char* start  = current_source_start();
    const char* cursor = current_source_cursor();

    unsigned int line_number = 1;
    const char*  line_start  = start;

    for (const char* p = start; p < cursor; p++) {
        if (*p == '\n') {
            line_number++;
            line_start = p + 1;
        }
    }

    /* Find end of the current line */
    const char* line_end = cursor;
    while (*line_end != '\n' && *line_end != '\0')
        line_end++;

    unsigned int line_len = (unsigned int)(line_end - line_start);
    unsigned int col      = (unsigned int)(cursor - line_start);

    char   buf[ERROR_MSG_MAX];
    size_t pos = 0;
    append_fmt(buf, sizeof(buf), &pos, "%u | %.*s\n    ", line_number, (int)line_len, line_start);
    for (unsigned int i = 0; i < col && pos < sizeof(buf) - 1; i++) buf[pos++] = ' ';
    append_fmt(buf, sizeof(buf), &pos, "^\nError: ");

    va_list args;
    va_start(args, format);
    if (pos < sizeof(buf)) {
        int n = vsnprintf(buf + pos, sizeof(buf) - pos, format, args);
        if (n > 0) pos += (size_t)n;
        if (pos >= sizeof(buf)) pos = sizeof(buf) - 1;
    }
    va_end(args);

    append_fmt(buf, sizeof(buf), &pos, "\n");
    emit_error(buf);

    parse_had_error   = true;
    runtime_had_error = true;

    /* Jumps straight back to the currently-executing vm_run() call's own dispatch loop instead of
       waiting for its next DISPATCH() to notice the flag (there is no such per-instruction check
       anymore — see DISPATCH()'s own comment, vm.c). NULL here means no vm_run() call is active
       (a parse-time error, or error()/error_at() called from parser.c) — falls through to the
       same "set flags, return normally" behavior this always had, letting the parser's own
       recursive-descent recovery run unchanged. */
    if (runtime_error_unwind_target) AER_LONGJMP(*runtime_error_unwind_target, 1);
}

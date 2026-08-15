#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "error.h"
#include "lexer.h"

AER_TLS bool parse_had_error = false;
AER_TLS bool runtime_had_error = false;
AER_TLS unsigned int assert_failure_count = 0;
unsigned int (*runtime_line_lookup)(void) = NULL;
const char* (*runtime_filename_lookup)(void) = NULL;
const char* (*runtime_function_lookup)(void) = NULL;
unsigned int (*runtime_stack_trace_lookup)(char* out, unsigned int out_size) = NULL;
AerJmpBuf* runtime_error_unwind_target = NULL;

#define ERROR_MSG_MAX 2048

static AER_TLS char last_error_msg[ERROR_MSG_MAX] = "";
static AerErrorCallback error_callback = NULL;
static void* error_callback_userdata = NULL;
static AerDiagnosticCallback diagnostic_callback = NULL;
static void* diagnostic_callback_userdata = NULL;

void aer_set_diagnostic_callback(AerDiagnosticCallback callback, void* userdata) {
    diagnostic_callback = callback;
    diagnostic_callback_userdata = userdata;
}

static void append_fmt(char* buf, size_t bufsize, size_t* pos, const char* fmt, ...) {
    if (*pos >= bufsize)
        return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *pos, bufsize - *pos, fmt, args);
    va_end(args);
    if (n < 0)
        return;
    *pos += (size_t)n;
    if (*pos >= bufsize)
        *pos = bufsize - 1;
}

/* Routes to the host-registered callback if any, else stderr; updates aer_last_error() either way. */
static void emit_error(const char* msg) {
    strncpy(last_error_msg, msg, ERROR_MSG_MAX - 1);
    last_error_msg[ERROR_MSG_MAX - 1] = '\0';
    if (error_callback)
        error_callback(last_error_msg, error_callback_userdata);
    else
        fprintf(stderr, "%s", last_error_msg);
}

void aer_set_error_callback(AerErrorCallback callback, void* userdata) {
    error_callback = callback;
    error_callback_userdata = userdata;
}

const char* aer_last_error(void) {
    return last_error_msg;
}

bool aer_had_error(void) {
    return parse_had_error || runtime_had_error;
}

unsigned int aer_assert_failure_count(void) {
    return assert_failure_count;
}

void aer_clear_error(void) {
    parse_had_error = false;
    runtime_had_error = false;
    assert_failure_count = 0;
    last_error_msg[0] = '\0';
}

/* The only thing left in this codebase allowed to exit() -- for OOM; doesn't solve recoverable OOM, just
   guarantees a host-registered sink sees the message before the process goes down. */
void aer_report_fatal(const char* msg) {
    char buf[ERROR_MSG_MAX];
    snprintf(buf, sizeof(buf), "Error: %s\n", msg);
    emit_error(buf);
    exit(1);
}

void* xmalloc(size_t size) {
    void* p = malloc(size);
    if (!p)
        aer_report_fatal("Out of memory");
    return p;
}

void* xcalloc(size_t count, size_t size) {
    void* p = calloc(count, size);
    if (!p)
        aer_report_fatal("Out of memory");
    return p;
}

void* xrealloc(void* ptr, size_t size) {
    void* p = realloc(ptr, size);
    if (!p)
        aer_report_fatal("Out of memory");
    return p;
}

char* xstrdup(const char* s) {
    char* p = strdup(s);
    if (!p)
        aer_report_fatal("Out of memory");
    return p;
}

void error(const char* format, ...) {
    char buf[ERROR_MSG_MAX];
    size_t pos = 0;
    unsigned int line = runtime_line_lookup ? runtime_line_lookup() : 0;
    const char* filename = runtime_filename_lookup ? runtime_filename_lookup() : NULL;
    const char* function_name = runtime_function_lookup ? runtime_function_lookup() : NULL;

    if (filename && line > 0) {
        if (function_name)
            append_fmt(buf, sizeof(buf), &pos, "%s:%u, in %s(): ", filename, line, function_name);
        else
            append_fmt(buf, sizeof(buf), &pos, "%s:%u: ", filename, line);
    } else if (line > 0) {
        append_fmt(buf, sizeof(buf), &pos, "Line %u: ", line);
    }
    append_fmt(buf, sizeof(buf), &pos, "Error: ");

    char msg_only[ERROR_MSG_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(msg_only, sizeof(msg_only), format, args);
    va_end(args);
    append_fmt(buf, sizeof(buf), &pos, "%s", msg_only);

    if (runtime_stack_trace_lookup) {
        char trace[1024];
        unsigned int n = runtime_stack_trace_lookup(trace, sizeof(trace));
        if (n > 0)
            append_fmt(buf, sizeof(buf), &pos, "%s", trace);
    }

    append_fmt(buf, sizeof(buf), &pos, "\n");
    emit_error(buf);
    if (diagnostic_callback)
        diagnostic_callback(line, 0, msg_only, diagnostic_callback_userdata);

    parse_had_error = true;
    runtime_had_error = true;

    if (runtime_error_unwind_target)
        AER_LONGJMP(*runtime_error_unwind_target, 1);
}

/* Print a message pinpointing the current token in the source. */
void error_at(const char* format, ...) {
    const char* start = current_source_start();
    const char* cursor = current_source_cursor();

    unsigned int line_number = 1;
    const char* line_start = start;

    for (const char* p = start; p < cursor; p++) {
        if (*p == '\n') {
            line_number++;
            line_start = p + 1;
        }
    }

    const char* line_end = cursor;
    while (*line_end != '\n' && *line_end != '\0')
        line_end++;

    unsigned int line_len = (unsigned int)(line_end - line_start);
    unsigned int col = (unsigned int)(cursor - line_start);

    char buf[ERROR_MSG_MAX];
    size_t pos = 0;
    const char* filename = runtime_filename_lookup ? runtime_filename_lookup() : NULL;
    if (filename)
        append_fmt(buf, sizeof(buf), &pos, "%s:", filename);
    append_fmt(buf, sizeof(buf), &pos, "%u | %.*s\n    ", line_number, (int)line_len, line_start);
    for (unsigned int i = 0; i < col && pos < sizeof(buf) - 1; i++)
        buf[pos++] = ' ';
    append_fmt(buf, sizeof(buf), &pos, "^\nError: ");

    char msg_only[ERROR_MSG_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(msg_only, sizeof(msg_only), format, args);
    va_end(args);
    append_fmt(buf, sizeof(buf), &pos, "%s", msg_only);

    append_fmt(buf, sizeof(buf), &pos, "\n");
    emit_error(buf);
    /* col is 0-based here (measured from line_start); diagnostic consumers get 1-based like
       line_number, so callers don't need to know this function's own internal convention. */
    if (diagnostic_callback)
        diagnostic_callback(line_number, col + 1, msg_only, diagnostic_callback_userdata);

    parse_had_error = true;
    runtime_had_error = true;

    if (runtime_error_unwind_target)
        AER_LONGJMP(*runtime_error_unwind_target, 1);
}

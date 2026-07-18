#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "aer_stdlib.h"
#include "aer_host.h"
#include "error.h"

/* io is a host-registered module, not hardcoded like math/random/string — see aer_stdlib.h; registered once by main.c, and embed_smoke_test.c deliberately never registers it, proving file access is opt-in. */

/* io.stdin()'s handle — the only "handle" concept left in this API. Every other operation is
   one-shot and path-based (open, do the thing, close, all inside the native call), so there's no
   persistent file table to manage anymore. */
#define STDIN_HANDLE 0
static FILE* stdin_file = NULL;

static AerVal make_error(const char* msg) {
    size_t n   = strlen(msg);
    char*  buf = xmalloc(n + 1);
    memcpy(buf, msg, n + 1);
    return aer_make_string(buf, (unsigned int)n);
}

/* stdin (or any non-seekable stream) can't be pre-sized via fseek/ftell, so read until EOF into a growing buffer. */
static AerVal io_read_until_eof(FILE* fp) {
    size_t cap = 4096, len = 0;
    char*  buf = xmalloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = xrealloc(buf, cap); }
        size_t n = fread(buf + len, 1, cap - len, fp);
        len += n;
        if (n == 0) break;   /* EOF or a read error either way */
    }
    buf = xrealloc(buf, len + 1);
    buf[len] = '\0';
    return aer_make_result(aer_make_string(buf, (unsigned int)len), aer_null());
}

/* Shared by the path-open and stdin branches of io_read — tries the fast seek-and-presize path
   first, falling back to io_read_until_eof for a non-seekable stream (fseek fails on a pipe). */
static AerVal io_read_fp(FILE* fp) {
    if (fseek(fp, 0, SEEK_END) != 0) return io_read_until_eof(fp);
    long size = ftell(fp);
    if (size < 0) return aer_make_result(aer_null(), make_error(strerror(errno)));
    fseek(fp, 0, SEEK_SET);

    char*  buf   = xmalloc((size_t)size + 1);
    size_t nread = fread(buf, 1, (size_t)size, fp);
    buf[nread] = '\0';
    return aer_make_result(aer_make_string(buf, (unsigned int)nread), aer_null());
}

/* io.read(path) opens, reads the whole file, and closes it in one call; io.read(io.stdin())
   reads the already-open stdin stream instead — the only handle-shaped value this API still
   produces, since there's no path for piped input. */
static AerVal io_read(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm; (void)userdata;
    if (arg_count != 1) {
        error("io.read() requires a path string or io.stdin()'s handle");
        return aer_null();
    }

    if (aer_type(args[0]) == TYPE_STRING) {
        const char* path = aer_as_string(args[0])->data;
        FILE* fp = fopen(path, "r");
        if (!fp) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s: %s", path, strerror(errno));
            return aer_make_result(aer_null(), make_error(buf));
        }
        AerVal result = io_read_fp(fp);
        fclose(fp);
        return result;
    }
    if (aer_type(args[0]) == TYPE_INTEGER && aer_as_int(args[0]) == STDIN_HANDLE) {
        return io_read_fp(stdin_file);
    }
    error("io.read() requires a path string or io.stdin()'s handle");
    return aer_null();
}

/* Shared by io_write/io_append — opens `path` in `mode`, writes the whole string, closes it. */
static AerVal io_write_mode(AerVal path_v, AerVal data_v, const char* mode) {
    const char* path = aer_as_string(path_v)->data;
    FILE* fp = fopen(path, mode);
    if (!fp) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: %s", path, strerror(errno));
        return aer_make_result(aer_null(), make_error(buf));
    }
    AerString* s = aer_as_string(data_v);
    size_t written = fwrite(s->data, 1, s->length, fp);
    fclose(fp);
    if (written != s->length) return aer_make_result(aer_null(), make_error(strerror(errno)));
    return aer_make_result(aer_null(), aer_null());
}

static AerVal io_write(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm; (void)userdata;
    if (arg_count != 2 || aer_type(args[0]) != TYPE_STRING || aer_type(args[1]) != TYPE_STRING) {
        error("io.write() requires a path and a string");
        return aer_null();
    }
    return io_write_mode(args[0], args[1], "w");
}

static AerVal io_append(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm; (void)userdata;
    if (arg_count != 2 || aer_type(args[0]) != TYPE_STRING || aer_type(args[1]) != TYPE_STRING) {
        error("io.append() requires a path and a string");
        return aer_null();
    }
    return io_write_mode(args[0], args[1], "a");
}

/* Always handle 0 — the one reserved handle. Same call site as every other io function; kept
   since there's no module-constant mechanism, and it's the only way to reach piped stdin data. */
static AerVal io_stdin(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm; (void)args; (void)userdata;
    if (arg_count != 0) {
        error("io.stdin() takes no arguments");
        return aer_null();
    }
    return aer_int(STDIN_HANDLE);
}

void aer_io_register(void) {
    stdin_file = stdin;
    aer_register_function("io", "read",   io_read,   NULL);
    aer_register_function("io", "write",  io_write,  NULL);
    aer_register_function("io", "append", io_append, NULL);
    aer_register_function("io", "stdin",  io_stdin,  NULL);
}

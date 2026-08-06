#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "aer_stdlib.h"
#include "aer_host.h"
#include "error.h"

/* io is host-registered, not a native module -- file access is opt-in per host (see aer_stdlib.h). */

/* io.stdin()'s handle -- the only handle in this API; everything else is one-shot and path-based. */
#define STDIN_HANDLE 0

/* Borrowed argv slice for io.args() -- see aer_io_set_args (aer_stdlib.h). */
static int io_argc = 0;
static char** io_argv = NULL;

/* stdin (or any non-seekable stream) can't be pre-sized via fseek/ftell, so read until EOF into a growing buffer. */
static AerVal io_read_until_eof(FILE* fp) {
    size_t cap = 4096, len = 0;
    char* buf = xmalloc(cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        size_t n = fread(buf + len, 1, cap - len, fp);
        len += n;
        if (n == 0) break; /* EOF or a read error either way */
    }
    buf = xrealloc(buf, len + 1);
    buf[len] = '\0';
    return aer_make_result(aer_make_string(buf, (unsigned int)len), aer_null());
}

/* Seek-and-presize when possible; io_read_until_eof for non-seekable streams (pipes). */
static AerVal io_read_fp(FILE* fp) {
    if (fseek(fp, 0, SEEK_END) != 0) return io_read_until_eof(fp);
    long size = ftell(fp);
    if (size < 0) return aer_make_result(aer_null(), aer_make_error(strerror(errno)));
    fseek(fp, 0, SEEK_SET);

    char* buf = xmalloc((size_t)size + 1);
    size_t nread = fread(buf, 1, (size_t)size, fp);
    buf[nread] = '\0';
    return aer_make_result(aer_make_string(buf, (unsigned int)nread), aer_null());
}

static AerVal io_read(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm;
    (void)userdata;
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
            return aer_make_result(aer_null(), aer_make_error(buf));
        }
        AerVal result = io_read_fp(fp);
        fclose(fp);
        return result;
    }
    if (aer_type(args[0]) == TYPE_INTEGER && aer_as_int(args[0]) == STDIN_HANDLE) {
        return io_read_fp(stdin);
    }
    error("io.read() requires a path string or io.stdin()'s handle");
    return aer_null();
}

static AerVal io_write_mode(AerVal path_v, AerVal data_v, const char* mode) {
    const char* path = aer_as_string(path_v)->data;
    FILE* fp = fopen(path, mode);
    if (!fp) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: %s", path, strerror(errno));
        return aer_make_result(aer_null(), aer_make_error(buf));
    }
    AerString* s = aer_as_string(data_v);
    size_t written = fwrite(s->data, 1, s->length, fp);
    fclose(fp);
    if (written != s->length) return aer_make_result(aer_null(), aer_make_error(strerror(errno)));
    return aer_make_result(aer_null(), aer_null());
}

static AerVal io_write(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm;
    (void)userdata;
    if (arg_count != 2 || aer_type(args[0]) != TYPE_STRING || aer_type(args[1]) != TYPE_STRING) {
        error("io.write() requires a path and a string");
        return aer_null();
    }
    return io_write_mode(args[0], args[1], "w");
}

static AerVal io_append(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm;
    (void)userdata;
    if (arg_count != 2 || aer_type(args[0]) != TYPE_STRING || aer_type(args[1]) != TYPE_STRING) {
        error("io.append() requires a path and a string");
        return aer_null();
    }
    return io_write_mode(args[0], args[1], "a");
}

/* A plain boolean, not a Result -- "no" is an answer here, never an error. */
static AerVal io_exists(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm;
    (void)userdata;
    if (arg_count != 1 || aer_type(args[0]) != TYPE_STRING) {
        error("io.exists() requires a path string");
        return aer_null();
    }
    struct stat st;
    return aer_bool(stat(aer_as_string(args[0])->data, &st) == 0);
}

static AerVal io_remove(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm;
    (void)userdata;
    if (arg_count != 1 || aer_type(args[0]) != TYPE_STRING) {
        error("io.remove() requires a path string");
        return aer_null();
    }
    const char* path = aer_as_string(args[0])->data;
    if (remove(path) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: %s", path, strerror(errno));
        return aer_make_result(aer_null(), aer_make_error(buf));
    }
    return aer_make_result(aer_null(), aer_null());
}

static AerVal io_args(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm;
    (void)args;
    (void)userdata;
    if (arg_count != 0) {
        error("io.args() takes no arguments");
        return aer_null();
    }
    AerArray* r = vm_new_array();
    r->count = 0;
    r->capacity = io_argc > 0 ? (unsigned int)io_argc : 4;
    r->items = xmalloc(sizeof(AerVal) * r->capacity);
    r->shape = NULL;
    r->generation = 0;
    for (int i = 0; i < io_argc; i++) {
        size_t n = strlen(io_argv[i]);
        char* buf = xmalloc(n + 1);
        memcpy(buf, io_argv[i], n + 1);
        r->items[r->count++] = aer_make_string(buf, (unsigned int)n);
    }
    return aer_array_val(r);
}

static AerVal io_stdin(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm;
    (void)args;
    (void)userdata;
    if (arg_count != 0) {
        error("io.stdin() takes no arguments");
        return aer_null();
    }
    return aer_int(STDIN_HANDLE);
}

/* Handles both '/' and '\\' -- a path from io.args() may be OS-native on Windows. */
static const char* io_last_sep(const char* s, unsigned int len) {
    for (unsigned int i = len; i-- > 0;) {
        if (s[i] == '/' || s[i] == '\\') return s + i;
    }
    return NULL;
}

static AerVal io_basename(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm;
    (void)userdata;
    if (arg_count != 1 || aer_type(args[0]) != TYPE_STRING) {
        error("io.basename() requires a path string");
        return aer_null();
    }
    AerString* s = aer_as_string(args[0]);
    const char* sep = io_last_sep(s->data, s->length);
    const char* start = sep ? sep + 1 : s->data;
    unsigned int n = (unsigned int)((s->data + s->length) - start);
    char* buf = xmalloc((size_t)n + 1);
    memcpy(buf, start, n);
    buf[n] = '\0';
    return aer_make_string(buf, n);
}

static AerVal io_dirname(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm;
    (void)userdata;
    if (arg_count != 1 || aer_type(args[0]) != TYPE_STRING) {
        error("io.dirname() requires a path string");
        return aer_null();
    }
    AerString* s = aer_as_string(args[0]);
    const char* sep = io_last_sep(s->data, s->length);
    if (!sep) {
        char* buf = xmalloc(2);
        buf[0] = '.';
        buf[1] = '\0';
        return aer_make_string(buf, 1);
    }
    /* A separator at position 0 ("/etc") means the directory is "/" itself, not "". */
    unsigned int n = (unsigned int)(sep - s->data);
    if (n == 0) n = 1;
    char* buf = xmalloc((size_t)n + 1);
    memcpy(buf, s->data, n);
    buf[n] = '\0';
    return aer_make_string(buf, n);
}

static AerVal io_join(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm;
    (void)userdata;
    if (arg_count != 2 || aer_type(args[0]) != TYPE_STRING || aer_type(args[1]) != TYPE_STRING) {
        error("io.join() requires two path strings");
        return aer_null();
    }
    AerString* a = aer_as_string(args[0]);
    AerString* b = aer_as_string(args[1]);
    bool need_sep = a->length > 0 && a->data[a->length - 1] != '/' && a->data[a->length - 1] != '\\';
    unsigned int n = a->length + (need_sep ? 1u : 0u) + b->length;
    char* buf = xmalloc((size_t)n + 1);
    memcpy(buf, a->data, a->length);
    unsigned int pos = a->length;
    if (need_sep) buf[pos++] = '/';
    memcpy(buf + pos, b->data, b->length);
    buf[n] = '\0';
    return aer_make_string(buf, n);
}

void aer_io_set_args(int argc, char** argv) {
    io_argc = argc;
    io_argv = argv;
}

void aer_io_register(void) {
    aer_register_function("io", "read", io_read, NULL);
    aer_register_function("io", "write", io_write, NULL);
    aer_register_function("io", "append", io_append, NULL);
    aer_register_function("io", "exists", io_exists, NULL);
    aer_register_function("io", "remove", io_remove, NULL);
    aer_register_function("io", "stdin", io_stdin, NULL);
    aer_register_function("io", "args", io_args, NULL);
    aer_register_function("io", "basename", io_basename, NULL);
    aer_register_function("io", "dirname", io_dirname, NULL);
    aer_register_function("io", "join", io_join, NULL);
}

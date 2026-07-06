#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "aer_io.h"
#include "aer_host.h"
#include "error.h"
#include "vm.h"

/* io is a host-registered module, not hardcoded like math/random/string (aer_stdlib.c) — see aer_io.h; registered once by main.c, and embed_smoke_test.c deliberately never registers it, proving file access is opt-in. */

#define MAX_OPEN_FILES 16

static FILE* open_files[MAX_OPEN_FILES];

/* Builds a Go-style (ok, err) pair the same way string.split does — a "multi-return" is just a TYPE_ARRAY Value (see parse_return/OP_UNPACK), so that's all `value, err = io.open(...)` needs. */
static Value make_pair(Value ok, Value err) {
    AerArray* r = vm_new_array();
    r->count    = 2;
    r->capacity = 2;
    r->items    = xmalloc(sizeof(AerVal) * 2);
    r->items[0] = aer_val_from_public(ok);
    r->items[1] = aer_val_from_public(err);
    r->shape    = NULL;
    return aer_val_to_public(aer_array_val(r));
}

static Value make_error(const char* msg) {
    size_t n   = strlen(msg);
    char*  buf = xmalloc(n + 1);
    memcpy(buf, msg, n + 1);
    return aer_val_to_public(aer_make_string(buf, (unsigned int)n));
}

static Value io_open(VM* vm, int arg_count, Value* args, void* userdata) {
    (void)vm; (void)userdata;
    if (arg_count != 2 || args[0].type != TYPE_STRING || args[1].type != TYPE_STRING) {
        error("io.open() requires a path and a mode string");
        return (Value){0};
    }
    const char* path = args[0].data.string->data;
    const char* mode = args[1].data.string->data;
    if (strcmp(mode, "r") != 0 && strcmp(mode, "w") != 0 && strcmp(mode, "a") != 0) {
        error("io.open() mode must be \"r\", \"w\", or \"a\"");
        return (Value){0};
    }

    int slot = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i]) { slot = i; break; }
    }
    if (slot < 0) return make_pair((Value){0}, make_error("Too many open files"));

    FILE* fp = fopen(path, mode);
    if (!fp) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: %s", path, strerror(errno));
        return make_pair((Value){0}, make_error(buf));
    }
    open_files[slot] = fp;

    Value handle = {0}; handle.type = TYPE_INTEGER; handle.data.integer = slot;
    return make_pair(handle, (Value){0});
}

/* Shared by io_read/io_write/io_close — a handle is a bounds-checked index into open_files; anything else is a wrong-type arg (caller's job to reject) or a stale/closed handle (this function's job). */
static FILE* handle_file(Value h) {
    if (h.type != TYPE_INTEGER) return NULL;
    long long i = h.data.integer;
    if (i < 0 || i >= MAX_OPEN_FILES) return NULL;
    return open_files[i];
}

/* stdin (or any non-seekable stream) can't be pre-sized via fseek/ftell, so read until EOF into a growing buffer — the same io.read(handle) call site the caller can't tell apart from a normal file. */
static Value io_read_until_eof(FILE* fp) {
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
    return make_pair(aer_val_to_public(aer_make_string(buf, (unsigned int)len)), (Value){0});
}

static Value io_read(VM* vm, int arg_count, Value* args, void* userdata) {
    (void)vm; (void)userdata;
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) {
        error("io.read() requires a handle");
        return (Value){0};
    }
    FILE* fp = handle_file(args[0]);
    if (!fp) return make_pair((Value){0}, make_error("Invalid or closed file handle"));

    if (fseek(fp, 0, SEEK_END) != 0) return io_read_until_eof(fp);
    long size = ftell(fp);
    if (size < 0) return make_pair((Value){0}, make_error(strerror(errno)));
    fseek(fp, 0, SEEK_SET);

    char*  buf   = xmalloc((size_t)size + 1);
    size_t nread = fread(buf, 1, (size_t)size, fp);
    buf[nread] = '\0';

    return make_pair(aer_val_to_public(aer_make_string(buf, (unsigned int)nread)), (Value){0});
}

static Value io_write(VM* vm, int arg_count, Value* args, void* userdata) {
    (void)vm; (void)userdata;
    if (arg_count != 2 || args[0].type != TYPE_INTEGER || args[1].type != TYPE_STRING) {
        error("io.write() requires a handle and a string");
        return (Value){0};
    }
    FILE* fp = handle_file(args[0]);
    if (!fp) return make_pair((Value){0}, make_error("Invalid or closed file handle"));

    unsigned int n       = args[1].data.string->length;
    size_t       written = fwrite(args[1].data.string->data, 1, n, fp);
    if (written != n) return make_pair((Value){0}, make_error(strerror(errno)));

    return make_pair((Value){0}, (Value){0});
}

static Value io_close(VM* vm, int arg_count, Value* args, void* userdata) {
    (void)vm; (void)userdata;
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) {
        error("io.close() requires a handle");
        return (Value){0};
    }
    FILE* fp = handle_file(args[0]);
    if (!fp) return make_pair((Value){0}, make_error("Invalid or already-closed file handle"));

    fclose(fp);
    open_files[args[0].data.integer] = NULL;
    return make_pair((Value){0}, (Value){0});
}

/* Always handle 0 — reserved in aer_io_register() before any io.open() call could claim it (io_open scans from index 0); a call like every other io function since there's no module-constant mechanism, and closing stdin's handle works like any other. */
static Value io_stdin(VM* vm, int arg_count, Value* args, void* userdata) {
    (void)vm; (void)args; (void)userdata;
    if (arg_count != 0) {
        error("io.stdin() takes no arguments");
        return (Value){0};
    }
    Value handle = {0}; handle.type = TYPE_INTEGER; handle.data.integer = 0;
    return handle;
}

void aer_io_register(void) {
    open_files[0] = stdin;
    aer_register_function("io", "open",  io_open,  NULL);
    aer_register_function("io", "read",  io_read,  NULL);
    aer_register_function("io", "write", io_write, NULL);
    aer_register_function("io", "close", io_close, NULL);
    aer_register_function("io", "stdin", io_stdin, NULL);
}

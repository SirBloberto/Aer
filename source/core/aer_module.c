#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer_module.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "value_box.h"

#ifdef _WIN32
#define PATH_LIST_SEP ';'
#else
#define PATH_LIST_SEP ':'
#endif

typedef struct {
    char*        name;    /* the import name, e.g. "helpers" — keyed by this alone, process-wide, not by resolved path; two same-named imports from different dirs collide */
    Chunk*       chunk;
    VM*          vm;
    unsigned int halt_addr;   /* return address for the cross-VM call trampoline in aer_module_call() */
} FileModule;

/* Both arrays below grow the same way append() does (start small, double on overflow) rather than capping at a fixed file count, since there's no principled limit on how many files a program imports. */
static FileModule*  modules          = NULL;
static int          module_count     = 0;
static int          module_capacity  = 0;

/* "Currently loading" stack — detects A-imports-B-imports-A cycles instead of recursing until the process runs out of file slots or stack space. */
static char** loading_stack    = NULL;
static int    loading_depth    = 0;
static int    loading_capacity = 0;

static FileModule* find_module(const char* name, unsigned int len) {
    for (int i = 0; i < module_count; i++)
        if (strlen(modules[i].name) == len && strncmp(modules[i].name, name, len) == 0)
            return &modules[i];
    return NULL;
}

static char* join_path(const char* dir, size_t dir_len, const char* name, unsigned int len) {
    char* path = xmalloc(dir_len + len + 4 /* ".aer" */ + 1 /* NUL */);
    memcpy(path, dir, dir_len);
    memcpy(path + dir_len, name, len);
    memcpy(path + dir_len + len, ".aer", 4);
    path[dir_len + len + 4] = '\0';
    return path;
}

static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* Resolves `name`.aer relative to the currently-lexed file's directory (dots become dir separators, e.g. "sub.mid" -> "sub/mid.aer"); if missing, falls back to AER_PATH, a PATH_LIST_SEP-separated list searched in order (like PYTHONPATH); if still not found, returns the same-directory candidate anyway so aer_module_load's read_file() produces the usual "Cannot open file" error. */
static char* resolve_path(const char* dotted_name, unsigned int len) {
    char* name = xmalloc(len);
    for (unsigned int i = 0; i < len; i++) name[i] = dotted_name[i] == '.' ? '/' : dotted_name[i];

    const char* base = current_source_name();
    const char* slash = NULL;
    for (const char* p = base; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    size_t dir_len = slash ? (size_t)(slash - base + 1) : 0;

    char* same_dir_path = join_path(base, dir_len, name, len);
    if (file_exists(same_dir_path)) { free(name); return same_dir_path; }

    const char* aer_path = getenv("AER_PATH");
    if (aer_path) {
        const char* start = aer_path;
        while (*start) {
            const char* end = start;
            while (*end && *end != PATH_LIST_SEP) end++;
            size_t entry_len = (size_t)(end - start);
            if (entry_len > 0) {
                bool needs_sep = start[entry_len - 1] != '/' && start[entry_len - 1] != '\\';
                size_t joined_dir_len = entry_len + (needs_sep ? 1 : 0);
                char* dirbuf = xmalloc(joined_dir_len);
                memcpy(dirbuf, start, entry_len);
                if (needs_sep) dirbuf[entry_len] = '/';
                char* candidate = join_path(dirbuf, joined_dir_len, name, len);
                free(dirbuf);
                if (file_exists(candidate)) { free(same_dir_path); free(name); return candidate; }
                free(candidate);
            }
            start = *end ? end + 1 : end;
        }
    }
    free(name);
    return same_dir_path;
}

bool aer_module_get(unsigned int index, VM** out_vm, Chunk** out_chunk) {
    if ((int)index >= module_count) return false;
    *out_vm    = modules[index].vm;
    *out_chunk = modules[index].chunk;
    return true;
}

void aer_module_free_all(void) {
    for (int i = 0; i < module_count; i++) {
        vm_free(modules[i].vm);
        chunk_free(modules[i].chunk);
        free(modules[i].vm);
        free(modules[i].chunk);
        free(modules[i].name);
    }
    free(modules);
    modules = NULL;
    module_count = module_capacity = 0;
    free(loading_stack);
    loading_stack = NULL;
    loading_depth = loading_capacity = 0;
}

/* Pushes a null placeholder onto the CALLING vm's stack — every failure path in aer_module_call reports its error and still needs to leave exactly one result behind, matching the core builtins' convention. */
static void push_null_result(VM* vm) {
    if (vm->stack_top < VM_STACK_MAX) vm->stack[vm->stack_top++] = aer_null();
}

bool aer_module_load(const char* name, unsigned int len,
                      const char* path_name, unsigned int path_len) {
    if (find_module(name, len)) return true;   /* already loaded, not an error */

    char* path = resolve_path(path_name, path_len);

    for (int i = 0; i < loading_depth; i++) {
        if (strcmp(loading_stack[i], path) == 0) {
            error_at("Circular import: '%s' is already being loaded", path);
            free(path);
            return false;
        }
    }
    if (loading_depth >= loading_capacity) {
        loading_capacity = loading_capacity ? loading_capacity * 2 : 8;
        loading_stack = xrealloc(loading_stack, sizeof(char*) * loading_capacity);
    }
    loading_stack[loading_depth++] = path;

    /* Save the importing file's lexer position and lookahead token so parsing can resume exactly where it left off once this nested read+lex+parse+run cycle (which reuses the same global lexer/parser state) completes. */
    LexerState* saved       = lexer_save_state();
    Token       saved_token = token;

    Chunk* mchunk = xmalloc(sizeof(Chunk));
    VM*    mvm    = xmalloc(sizeof(VM));
    chunk_init(mchunk);
    vm_init(mvm, mchunk);

    read_file(path);
    bool ok = !parse_had_error;
    if (ok) {
        lex();
        parse(mchunk);
        ok = !parse_had_error;
    }
    unsigned int halt_addr = mchunk->count;
    chunk_emit(mchunk, OP_HALT);

    if (ok) {
        runtime_had_error = false;
        mvm->ip = 0;
        /* The outer (importing) chunk/VM isn't registered as a GC root yet (that happens once this function returns and the FileModule entry is added), so a collection triggered by this nested run could sweep something the outer file still needs — see vm_gc_suppress's comment in vm.h. */
        vm_gc_suppress();
        vm_run(mvm);
        vm_gc_unsuppress();
        if (runtime_had_error) {
            error_at("Error while loading module '%.*s'", (int)len, name);
            ok = false;
        }
    }
    /* Whatever happened inside the imported file must not leak into the importing program's own later execution — it hasn't even finished parsing yet, let alone started running. */
    runtime_had_error = false;

    lexer_restore_state(saved);
    token = saved_token;
    loading_depth--;

    if (!ok) {
        free(path);
        return false;
    }

    if (module_count >= module_capacity) {
        module_capacity = module_capacity ? module_capacity * 2 : 8;
        modules = xrealloc(modules, sizeof(FileModule) * module_capacity);
    }
    FileModule* m = &modules[module_count++];
    m->name      = xmalloc(len + 1);
    memcpy(m->name, name, len);
    m->name[len] = '\0';
    m->chunk     = mchunk;
    m->vm        = mvm;
    m->halt_addr = halt_addr;
    free(path);
    return true;
}

/* Finds a TYPE_FUNCTION value named `fn` among the module's top-level bindings — inline scope slots, then the hashmap-overflow path if it spilled past SCOPE_SLOT_MAX. Mirrors vm_scope_get but only scopes[0], since a file-module's own code always runs at call_depth 0. */
static AerVal* find_module_function(FileModule* m, const char* fn) {
    AerScope* g = &m->vm->scopes[0];
    for (int j = 0; j < g->count; j++) {
        unsigned int name_idx = g->slots[j].name;
        if (strcmp(aer_as_string(m->chunk->pool[name_idx])->data, fn) != 0) continue;
        AerVal* v = &g->slots[j].val;
        return aer_type(*v) == TYPE_FUNCTION ? v : NULL;
    }
    if (g->overflow) {
        AerVal* v = (AerVal*)hashmap_get(&g->map, fn);
        if (v && aer_type(*v) == TYPE_FUNCTION) return v;
    }
    return NULL;
}

bool aer_module_call(VM* vm, const char* module, const char* fn, int arg_count) {
    FileModule* m = find_module(module, (unsigned int)strlen(module));
    if (!m) return false;

    AerVal* fv = find_module_function(m, fn);
    if (!fv) return false;

    VM* mv = m->vm;

    /* Copy args left-to-right onto the module's own isolated stack before validating, so vm_setup_call can read args[0] (the receiver, if any) exactly like lbl_call_value's in-VM call does. */
    if (mv->stack_top + arg_count > VM_STACK_MAX) {
        error("Stack overflow");
        vm->stack_top -= arg_count;
        push_null_result(vm);
        return true;
    }
    AerVal* args = &mv->stack[mv->stack_top];
    for (int i = 0; i < arg_count; i++) mv->stack[mv->stack_top++] = vm->stack[vm->stack_top - arg_count + i];
    vm->stack_top -= arg_count;

    /* Trampoline: vm_setup_call (shared with lbl_call_value) sets up a call frame whose return address is this module's own top-level HALT, so vm_run(mv) executes exactly one call and stops. */
    if (!vm_setup_call(mv, m->chunk, *fv, arg_count, args, m->halt_addr)) {
        mv->stack_top -= arg_count;   /* undo the copy above — validation failed */
        push_null_result(vm);
        return true;
    }

    /* The calling vm isn't reachable from mv's own roots, and isn't in aer_module_get's list either (vm might be the main VM, never a file-module one) — same missing-root hazard as aer_module_load's nested run; see vm_gc_suppress's comment in vm.h. */
    vm_gc_suppress();
    vm_run(mv);
    vm_gc_unsuppress();
    /* runtime_had_error deliberately stays true on failure here (unlike aer_module_load's parse-time path) — DISPATCH()'s next check aborts the calling vm's execution too, exactly like a same-VM call error. */
    if (runtime_had_error || mv->stack_top <= 0) {
        push_null_result(vm);
        return true;
    }

    AerVal ret = mv->stack[--mv->stack_top];
    if (vm->stack_top < VM_STACK_MAX) vm->stack[vm->stack_top++] = ret;
    return true;
}

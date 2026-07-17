#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer_module.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"

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

/* True if `name` already ends in ".aer" — quoted import paths may spell the extension out
   explicitly (`import "../utils.aer"`), dotted-identifier imports never do. */
static bool has_aer_ext(const char* name, unsigned int len) {
    return len >= 4 && strncmp(name + len - 4, ".aer", 4) == 0;
}

static char* join_path(const char* dir, size_t dir_len, const char* name, unsigned int len) {
    bool ext = has_aer_ext(name, len);
    char* path = xmalloc(dir_len + len + (ext ? 0 : 4) + 1 /* NUL */);
    memcpy(path, dir, dir_len);
    memcpy(path + dir_len, name, len);
    if (!ext) memcpy(path + dir_len + len, ".aer", 4);
    path[dir_len + len + (ext ? 0 : 4)] = '\0';
    return path;
}

static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* True for a path that's already fully qualified and shouldn't be joined against the importing
   file's directory or searched for on AER_PATH: a leading '/' or '\' (Unix-style, and also how
   Windows accepts a rooted path on the current drive), or a drive letter like "C:/" / "C:\". */
static bool is_absolute_path(const char* p, unsigned int len) {
    if (len == 0) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    if (len >= 2 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') return true;
    return false;
}

/* Resolves an import path to a real file on disk. `path_name` is used exactly as written —
   dotted-identifier imports (`import a.b`) arrive here with dots already turned into '/' by the
   parser; quoted-path imports (`import "../a/b"`) arrive with whatever separators/relative
   components the user wrote, untouched (so a literal ".." is never mistaken for the dotted-name
   convention and mangled into extra separators).

   An absolute path (leading '/' or a drive letter) is used as-is, with no directory-joining or
   AER_PATH search — the caller already said exactly where to look. Otherwise this resolves
   relative to the currently-lexed file's directory; if missing there, falls back to AER_PATH, a
   PATH_LIST_SEP-separated list searched in order (like PYTHONPATH); if still not found, returns
   the same-directory candidate anyway so aer_module_load's read_file() produces the usual "Cannot
   open file" error. */
static char* resolve_path(const char* path_name, unsigned int len) {
    if (is_absolute_path(path_name, len)) return join_path("", 0, path_name, len);

    const char* base = current_source_name();
    const char* slash = NULL;
    for (const char* p = base; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    size_t dir_len = slash ? (size_t)(slash - base + 1) : 0;

    char* same_dir_path = join_path(base, dir_len, path_name, len);
    if (file_exists(same_dir_path)) return same_dir_path;

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
                char* candidate = join_path(dirbuf, joined_dir_len, path_name, len);
                free(dirbuf);
                if (file_exists(candidate)) { free(same_dir_path); return candidate; }
                free(candidate);
            }
            start = *end ? end + 1 : end;
        }
    }
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
    /* Same idea for the parser's own compile-time tables (function/variable/struct registries, the
       pending-forward-reference list, etc.) — all file-scope statics in parser.c shared by
       whichever parse() call is innermost. Without this, compiling the imported file here would
       corrupt the importing file's own still-in-progress compile the moment this call returns. See
       parser_save_state's own comment in parser.c/.h. */
    ParserState* saved_parser = parser_save_state();

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
    parser_restore_state(saved_parser);
    loading_depth--;

    if (!ok) {
        /* mchunk/mvm were already fully allocated and initialized above (chunk_init/vm_init, plus
           whatever the failed parse/run itself emitted) — never registered into modules[], so
           aer_module_free_all() has no way to ever reach them; free everything here instead,
           mirroring aer_module_free_all's own per-module cleanup. */
        vm_free(mvm);
        chunk_free(mchunk);
        free(mvm);
        free(mchunk);
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

/* Finds a compiled TYPE_FUNCTION registration named `fn` among the module's exported functions
   — see ChunkFunction's own comment (vm.h) for why this is a Chunk-level registry rather than a
   runtime scope-by-name scan: the register VM never writes named bindings into any scope at all. */
static ChunkFunction* find_module_function(FileModule* m, const char* fn) {
    return chunk_find_function(m->chunk, fn);
}

bool aer_module_call(VM* vm, const char* module, const char* fn, int arg_count) {
    FileModule* m = find_module(module, (unsigned int)strlen(module));
    if (!m) return false;

    ChunkFunction* fnreg = find_module_function(m, fn);
    if (!fnreg) return false;

    VM* mv = m->vm;

    /* Args are already sitting in the CALLING vm's own stack (lbl_call_module pushed them there
       before calling here) — no need to copy them anywhere first, setup_call reads straight out
       of this pointer and copies each one into the callee's own register bank immediately, with no
       allocation in between. */
    AerVal* args = &vm->stack[vm->stack_top - arg_count];
    vm->stack_top -= arg_count;

    /* mv is reused across every future call into this module for the process's whole life — a
       PRIOR call's runtime error unwinds via longjmp straight past OP_RETURN's normal
       call_depth-- (vm.c), so a failed call can leave mv->call_depth stuck above 0. setup_call
       always pushes its new frame at mv->call_depth+1 and hardcodes the callee's dest_reg to 0,
       meaning the result of THIS call would land in mv->call_stack[mv->call_depth].registers[0] —
       not mv->call_stack[0].registers[0], which is what the read below always assumes. Left
       unreset, this isn't just an eventual "Call stack overflow" after enough failures (bounded by
       VM_CALL_MAX) — the very next call after any single failed one silently reads whatever stale
       value already sits in frame 0's register 0, returning a wrong result with no error at all.
       Mirrors main.c's run()'s own defensive reset before every top-level statement — unconditional,
       not just after a detected failure, since a successful call already restores call_depth to 0
       itself (OP_RETURN), so resetting here is always safe and costs nothing extra. */
    mv->call_depth = 0;
    mv->stack_top  = 0;
    mv->registers  = mv->call_stack[0].registers;
    mv->raw_ints   = mv->call_stack[0].raw_ints;
    mv->raw_reals  = mv->call_stack[0].raw_reals;

    /* Trampoline: setup_call pushes a real call frame whose return address is this module's
       own top-level HALT, so vm_run(mv) executes exactly one call and stops — see its own comment
       (vm.c) for why the result always lands in mv->call_stack[0].registers[0]. */
    if (!setup_call(mv, fnreg, arg_count, args, m->halt_addr)) {
        push_null_result(vm);
        return true;
    }

    /* The calling vm isn't reachable from mv's own roots, and isn't in aer_module_get's list either (vm might be the main VM, never a file-module one) — same missing-root hazard as aer_module_load's nested run; see vm_gc_suppress's comment in vm.h. */
    vm_gc_suppress();
    vm_run(mv);
    vm_gc_unsuppress();
    /* runtime_had_error deliberately stays true on failure here (unlike aer_module_load's parse-time path) — the called module's vm_run(mv) already caught its own error locally (its own catch point, installed and restored inside vm_run itself) and returned cleanly, so nothing automatically aborts the CALLING vm too anymore now that DISPATCH() no longer polls this flag every instruction. Propagate explicitly: longjmp to whatever vm_run() call is now the current unwind target (the calling vm's own, since vm_run(mv)'s return already restored it) — exactly like a same-VM call error, just raised here instead of noticed passively. */
    if (runtime_had_error) {
        push_null_result(vm);
        if (runtime_error_unwind_target) longjmp(*runtime_error_unwind_target, 1);
        return true;
    }

    AerVal ret = mv->call_stack[0].registers[0];
    if (vm->stack_top < VM_STACK_MAX) vm->stack[vm->stack_top++] = ret;
    return true;
}

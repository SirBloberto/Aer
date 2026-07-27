#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
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

/* Quoted import paths may spell out the .aer extension; dotted imports never do. */
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

/* Fully qualified (leading '/', '', or drive letter) — never dir-joined or AER_PATH-searched. */
static bool is_absolute_path(const char* p, unsigned int len) {
    if (len == 0) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    if (len >= 2 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') return true;
    return false;
}

/* Resolves an import path to a real file: absolute paths as-is; otherwise relative to the
   importing file's directory, then each AER_PATH entry. Dots in dotted imports were already
   turned into '/' by the parser; quoted paths arrive untouched. */
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

/* Shared by aer_module_load below and aer_actor_spawn (aer_actor.c) -- both need "make a
   brand-new, independent VM+Chunk, load+compile+run its top-level code exactly once, without
   corrupting whatever compile is already in progress on the caller's side (a nested import, or
   an actor spawned mid-script)" and nothing else; each caller layers its own specific concerns
   (circular-import detection, module-name registration, contextual error wording) on top. */
InstantiateResult aer_vm_instantiate_from_file(char* path, VM** out_vm, Chunk** out_chunk, unsigned int* out_halt_addr) {
    /* Save the caller's lexer position and lookahead token so parsing can resume exactly where it
       left off once this nested read+lex+parse+run cycle (which reuses the same global
       lexer/parser state) completes. */
    LexerState* saved       = lexer_save_state();
    Token       saved_token = token;
    /* Save the parser's file-scope tables too, or compiling this corrupts the caller's own
       still-in-progress compile. */
    ParserState* saved_parser = parser_save_state();
    /* vm_init(mvm, ...) below unconditionally repoints current_heap at mvm's own heap so parsing
       (which starts right after, before mvm ever runs) allocates into the right place -- but that
       happens outside vm_run_slice's own save/restore, which only brackets vm_run(mvm) itself, not
       this whole compile+run cycle. Without this, every allocation the CALLER makes after this
       function returns would keep landing in mvm's heap instead of its own. */
    VmHeap* saved_heap = vm_current_heap();
    /* Same reasoning as saved_heap just above, for the OTHER file-scope global vm_init() clobbers
       unconditionally: active_vm_for_errors. Without this, a failed nested compile (ok == false
       below) frees mvm while active_vm_for_errors still points at it -- the next error() call
       anywhere in the caller's context then dereferences a freed VM. */
    VM* saved_active_vm = vm_active_error_vm();

    Chunk* mchunk = xmalloc(sizeof(Chunk));
    VM*    mvm    = xmalloc(sizeof(VM));
    chunk_init(mchunk);
    mchunk->source_filename = xstrdup(path);
    vm_init(mvm, mchunk);

    read_file(path);
    bool ok = !parse_had_error;
    if (ok) {
        lex();
        parse(mchunk);
        ok = !parse_had_error;
    }
    InstantiateResult result = ok ? INSTANTIATE_OK : INSTANTIATE_PARSE_FAILED;
    unsigned int halt_addr = mchunk->count;
    chunk_emit(mchunk, OP_HALT);

    if (ok) {
        runtime_had_error = false;
        mvm->ip = 0;
        /* mvm now collects only its own independent heap (see vm.c's VmHeap), so a collection
           triggered by this nested run can no longer reach anything belonging to the caller's
           heap at all — this suppress/unsuppress pairing predates that split, from when every VM
           shared one heap and the caller's own chunk/VM (not yet registered as a root at this
           point) could be swept by mistake. Left in place as a harmless, still-correct no-op
           rather than removed speculatively; see vm_gc_suppress's comment in vm.h. */
        vm_gc_suppress();
        vm_run(mvm);
        vm_gc_unsuppress();
        if (runtime_had_error) {
            ok = false;
            result = INSTANTIATE_RUNTIME_FAILED;
        }
    }
    /* Whatever happened during this nested run must not leak into the caller's own later
       execution — it hasn't even finished parsing yet, let alone started running. */
    runtime_had_error = false;

    lexer_restore_state(saved);
    token = saved_token;
    parser_restore_state(saved_parser);
    /* Restored on every path, success or failure -- mvm's own heap stays valid and independently
       reachable whenever mvm itself runs later (vm_run_slice's save/restore handles that case
       correctly on its own); this only fixes where allocations land in the CALLER's own code from
       here on. */
    vm_set_current_heap(saved_heap);
    vm_set_active_error_vm(saved_active_vm);

    if (!ok) {
        /* Never handed back to the caller, so nothing else can reach these — free here. */
        vm_free(mvm);
        chunk_free(mchunk);
        free(mvm);
        free(mchunk);
        return result;
    }
    *out_vm        = mvm;
    *out_chunk     = mchunk;
    *out_halt_addr = halt_addr;
    return INSTANTIATE_OK;
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

    VM* mvm = NULL; Chunk* mchunk = NULL; unsigned int halt_addr = 0;
    InstantiateResult r = aer_vm_instantiate_from_file(path, &mvm, &mchunk, &halt_addr);
    loading_depth--;

    if (r != INSTANTIATE_OK) {
        if (r == INSTANTIATE_RUNTIME_FAILED) error_at("Error while loading module '%.*s'", (int)len, name);
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

/* Chunk-level function registry — the register VM never writes named bindings into any scope. */
static ChunkFunction* find_module_function(FileModule* m, const char* fn) {
    return chunk_find_function(m->chunk, fn);
}

bool aer_module_call(VM* vm, const char* module, const char* fn, int arg_count) {
    FileModule* m = find_module(module, (unsigned int)strlen(module));
    if (!m) return false;

    ChunkFunction* fnreg = find_module_function(m, fn);
    if (!fnreg) return false;

    VM* mv = m->vm;

    /* Args are already on the calling vm's stack; setup_call copies them straight into the
       callee's registers. */
    AerVal* args = &vm->stack[vm->stack_top - arg_count];
    vm->stack_top -= arg_count;

    /* A prior call's error longjmp can leave mv->call_depth stuck above 0 — left unreset, this
       call's result would land in the wrong frame's registers (and enough failures overflow the
       call stack). The stack is dead space between calls. */
    aer_vm_reset_for_reuse(mv);

    /* Trampoline: the frame's return address is this module's top-level HALT, so vm_run executes
       exactly one call and the result lands in call_stack[0].registers[0]. */
    if (!setup_call(mv, fnreg, arg_count, args, m->halt_addr)) {
        push_null_result(vm);
        return true;
    }

    /* mv now collects only its own independent heap (see vm.c's VmHeap), so mv's own GC can no
       longer reach anything belonging to vm's heap at all — this suppress/unsuppress pairing
       predates that split, from when every VM shared one heap and mv's collection had to be kept
       from sweeping vm's not-yet-rooted state. Left in place as a harmless, still-correct no-op
       rather than removed speculatively; see vm_gc_suppress's comment in vm.h. */
    vm_gc_suppress();
    vm_run(mv);
    vm_gc_unsuppress();
    /* runtime_had_error deliberately stays true on failure here (unlike aer_module_load's parse-time path) — the called module's vm_run(mv) already caught its own error locally (its own catch point, installed and restored inside vm_run itself) and returned cleanly, so nothing automatically aborts the CALLING vm too anymore now that DISPATCH() no longer polls this flag every instruction. Propagate explicitly: longjmp to whatever vm_run() call is now the current unwind target (the calling vm's own, since vm_run(mv)'s return already restored it) — exactly like a same-VM call error, just raised here instead of noticed passively. */
    if (runtime_had_error) {
        push_null_result(vm);
        if (runtime_error_unwind_target) AER_LONGJMP(*runtime_error_unwind_target, 1);
        return true;
    }

    AerVal ret = mv->call_stack[0].registers[0];
    if (vm->stack_top < VM_STACK_MAX) vm->stack[vm->stack_top++] = ret;
    return true;
}

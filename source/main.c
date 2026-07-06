#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "aer_io.h"
#include "aer_module.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "terminal.h"
#include "vm.h"

Token token;
Mode  mode;

static Chunk chunk;
static VM    vm;

static void run();
static void run_shell();
static bool run_file(char* path);
static void help();

int main(int argc, char** argv) {
    chunk_init(&chunk);
    vm_init(&vm, &chunk);
    /* Opt-in host capability, not a built-in module (see aer_io.h) — the reference CLI grants file access; an embedding host that wants a sandboxed script simply doesn't call this. */
    aer_io_register();

    int status = 0;
    if (argc == 1) {
        run_shell();
    } else if (strcmp(argv[1], "help") == 0) {
        help();
    } else if (strcmp(argv[1], "version") == 0) {
        printf("Aer %s\n", VERSION);
    } else {
        status = run_file(argv[1]) ? 0 : 1;
    }

    /* Not load-bearing at process exit — exercises the same teardown path an embedding host would use. */
    aer_module_free_all();
    return status;
}

/* Parse and run all statements in the current source buffer; resets bytecode each call, but name/constant pools persist so indices stay stable across REPL calls. */

static void run() {
    /* Append new code after any previous bytecode — preserves function bodies compiled in earlier REPL calls. */
    unsigned int start = chunk.count;
    vm.ip         = start;
    vm.stack_top  = 0;
    vm.call_depth = 0;
    while (vm.scope_depth > 1) {
        AerScope* s = &vm.scopes[--vm.scope_depth];
        /* overflow_has_captures guard — same as vm.c's lbl_pop_scope/lbl_return/vm_free: a scope a runtime error aborted mid-call may still have a live closure holding a box into its hashmap, so freeing it here would use-after-free that closure. */
        if (s->overflow && !s->overflow_has_captures) hashmap_free(&s->map);
    }
    lex();
    parse(&chunk);
    chunk_emit(&chunk, OP_HALT);
    runtime_had_error = false;
    vm_run(&vm);
}

/* Interactive REPL */

static void run_shell() {
    mode = MODE_SHELL;
    start_terminal("aer");

    char*  block_buf  = NULL;
    size_t block_size = 0;
    bool   in_block   = false;

    while (1) {
        char* line = handle_terminal();
        if (!line) continue;

        size_t len = strlen(line);

        bool ends_with_colon = false;
        for (size_t i = len; i > 0; i--) {
            char ch = line[i - 1];
            if (ch == '\n' || ch == ' ' || ch == '\t') continue;
            ends_with_colon = (ch == ':');
            break;
        }

        if (in_block || ends_with_colon) {
            block_buf = xrealloc(block_buf, block_size + len + 1);
            memcpy(block_buf + block_size, line, len);
            block_size += len;
            block_buf[block_size] = '\0';

            if (ends_with_colon) {
                in_block = true;
                set_terminal_prompt("... ");
                continue;
            }

            bool blank = true;
            for (size_t i = 0; i < len; i++) {
                if (line[i] != '\n' && line[i] != ' ' && line[i] != '\t') {
                    blank = false;
                    break;
                }
            }
            if (!blank) continue;

            set_terminal_prompt(">>> ");
            shell(block_buf);
            free(block_buf);
            block_buf  = NULL;
            block_size = 0;
            in_block   = false;
        } else {
            shell(line);
        }

        run();
    }
}

/* File execution */

static bool run_file(char* path) {
    mode = MODE_RUN;
    read_file(path);
    run();
    /* A runtime error no longer terminates the process (see error.c) — the CLI decides to exit nonzero here; a failed assert() doesn't set aer_had_error() on purpose (see error.h), so it's checked separately. */
    return !aer_had_error() && aer_assert_failure_count() == 0;
}

static void help() {
    printf("Usage: aer [command|file]\n");
    printf("  help      Show this message\n");
    printf("  version   Show Aer version\n");
    printf("  <file>    Execute an Aer source file\n");
    printf("  (no args) Start interactive shell\n");
}

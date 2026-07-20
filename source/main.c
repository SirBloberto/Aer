#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "aer_stdlib.h"
#include "aer_module.h"
#include "disasm.h"
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

#ifdef AER_DEBUG_TOOLS
static const char* debug_dump_path = NULL;
#endif

int main(int argc, char** argv) {
    chunk_init(&chunk);
    vm_init(&vm, &chunk);   /* registers io, same as every other stdlib module */
    /* Once per process — parser state persists across parse() calls, which is what makes REPL
       variable/function persistence work */
    parser_reset();

#ifdef AER_DEBUG_TOOLS
    /* Filter --debug-path=<path> out of argv before the help/version/file dispatch sees it */
    int    real_argc = 1;
    char** real_argv = xmalloc(sizeof(char*) * (size_t)argc);
    real_argv[0] = argv[0];
    for (int i = 1; i < argc; i++) {
        static const char prefix[] = "--debug-path=";
        if (strncmp(argv[i], prefix, sizeof(prefix) - 1) == 0) {
            debug_dump_path = argv[i] + sizeof(prefix) - 1;
        } else {
            real_argv[real_argc++] = argv[i];
        }
    }
    argc = real_argc;
    argv = real_argv;
#endif

    int status = 0;
    if (argc == 1) {
        run_shell();
    } else if (strcmp(argv[1], "help") == 0) {
        help();
    } else if (strcmp(argv[1], "version") == 0) {
        printf("Aer %s\n", VERSION);
    } else {
        /* Everything after the script path belongs to the script, via io.args(). */
        aer_io_set_args(argc - 2, argv + 2);
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
    vm.ip            = start;
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    lex();
    /* parse() never resets its own tables (parser_reset, called once in main() above, already did
       that) and has its own per-statement rollback/recovery, so it's safe to call repeatedly —
       once per REPL line, or once for a whole file. */
    parse(&chunk);
    chunk_emit(&chunk, OP_HALT);
    runtime_had_error = false;
    /* A rolled-back statement compiles to a no-op, which is harmless for one bad REPL line but not
       for a file: a corrupted loop body (e.g. an increment that failed to compile) silently becomes
       an infinite loop instead of the syntax error it actually is. File mode refuses to run at all
       once parse() has flagged any statement as invalid; the REPL still runs the rest of the line. */
    if (mode == MODE_RUN && parse_had_error) return;
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
        if (!line) {
            /* Ctrl-C — abandon any in-progress multi-line block, like Python's REPL */
            free(block_buf);
            block_buf  = NULL;
            block_size = 0;
            in_block   = false;
            set_terminal_prompt(">>> ");
            continue;
        }

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
    /* read_file() can fail (missing file, unreadable, embedded NUL) without ever setting up the
       lexer's current file — calling run() anyway would lex/parse a null or stale File*. */
    if (!aer_had_error()) run();
#ifdef AER_DEBUG_TOOLS
    /* After run() so the dump has both the bytecode and the run's hit counts; "-" means stderr */
    const char* dump_path = debug_dump_path;
    if (dump_path) {
        FILE* dump_out = strcmp(dump_path, "-") == 0 ? stderr : fopen(dump_path, "w");
        if (!dump_out) { fprintf(stderr, "--debug-path: could not open '%s' for writing\n", dump_path); dump_out = stderr; }
        aer_disassemble(&chunk, dump_out);
        aer_debug_memory_report(dump_out);
        if (dump_out != stderr) fclose(dump_out);
    }
#endif
    /* A runtime error no longer terminates the process (see error.c) — the CLI decides to exit nonzero here; a failed assert() doesn't set aer_had_error() on purpose (see error.h), so it's checked separately. */
    return !aer_had_error() && aer_assert_failure_count() == 0;
}

static void help() {
    printf("Usage: aer [command|file]\n");
    printf("  help      Show this message\n");
    printf("  version   Show Aer version\n");
    printf("  <file> [args...]   Execute an Aer source file; extra arguments reach the script via io.args()\n");
    printf("  (no args) Start interactive shell\n");
#ifdef AER_DEBUG_TOOLS
    printf("  --debug-path=<path>  Write a disassembly + hit-count/memory dump here after running\n");
    printf("                       <file> (\"-\" for stderr)\n");
#endif
}

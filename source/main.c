#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "aer_actor.h"
#include "aer_stdlib.h"
#include "aer_module.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "terminal.h"
#include "vm.h"

Token token;
Mode mode;

static Chunk chunk;
static VM vm;

static void run();
static void run_shell();
static bool run_file(char* path);
static void help();

#ifdef AER_DEBUG_TOOLS
static const char* debug_dump_path = NULL;
#endif

/* --max-instructions: 0 means unbounded, the default for every ordinary run. A budget turns "this
   program does not terminate" into an exact, machine-independent fact instead of a wall-clock
   guess -- see tests/fuzz.py, which is the only caller that sets one. */
static unsigned int max_instructions = 0;
static bool budget_exhausted = false;
/* vm_run_slice's budget is per-call, so a large total is spent in chunks rather than passed whole. */
#define INSTRUCTION_SLICE 1000000u
#define EXIT_BUDGET_EXHAUSTED 3

/* Interprets a K/M/G suffix as a cell-count multiplier (1,000 / 1,000,000 / 1,000,000,000) --
   aer_gc_set_ceiling() counts live *cells*, not bytes, and cell sizes differ per pool (a string
   cell isn't the size of a dict cell), so there is no accurate bytes-to-cells conversion available
   without a much bigger per-allocation byte-accounting subsystem this project doesn't have. This
   is a cell-count ceiling with a familiar-looking suffix, not a byte-accurate memory limit --
   documented as such in help() below and the README, not silently implied. */
static unsigned int parse_memory_size(const char* s) {
    char* end;
    double n = strtod(s, &end);
    unsigned int multiplier = 1;
    if (*end == 'K' || *end == 'k')
        multiplier = 1000u;
    else if (*end == 'M' || *end == 'm')
        multiplier = 1000000u;
    else if (*end == 'G' || *end == 'g')
        multiplier = 1000000000u;
    return (unsigned int)(n * multiplier);
}

int main(int argc, char** argv) {
    chunk_init(&chunk);
    /* Once per process -- parser state persists across parse() calls, which is what makes REPL
       variable/function persistence work */
    parser_reset();

    /* Filter recognized global flags out of argv before the help/version/file dispatch sees them
       -- everything after the script path still reaches the script untouched, via io.args(). Must
       run BEFORE vm_init below: vm_init seeds vm.io_enabled/net_enabled from the current
       aer_io_enabled/aer_net_enabled globals once, so --no-io/--no-net need to already be applied
       to those globals by the time it runs, or the flag would silently do nothing for this vm. */
    int real_argc = 1;
    char** real_argv = xmalloc(sizeof(char*) * (size_t)argc);
    real_argv[0] = argv[0];
    for (int i = 1; i < argc; i++) {
        static const char no_io[] = "--no-io";
        static const char no_net[] = "--no-net";
        static const char no_import[] = "--no-import";
        static const char mem_size[] = "--memory-size=";
        static const char max_instr[] = "--max-instructions=";
#ifdef AER_DEBUG_TOOLS
        static const char debug_path[] = "--debug-path=";
#endif
        if (strcmp(argv[i], no_io) == 0) {
            aer_set_io_enabled(false);
        } else if (strcmp(argv[i], no_net) == 0) {
            aer_set_net_enabled(false);
        } else if (strcmp(argv[i], no_import) == 0) {
            aer_set_import_enabled(false);
        } else if (strncmp(argv[i], mem_size, sizeof(mem_size) - 1) == 0) {
            aer_gc_set_ceiling(parse_memory_size(argv[i] + sizeof(mem_size) - 1));
        } else if (strncmp(argv[i], max_instr, sizeof(max_instr) - 1) == 0) {
            max_instructions = (unsigned int)strtoul(argv[i] + sizeof(max_instr) - 1, NULL, 10);
#ifdef AER_DEBUG_TOOLS
        } else if (strncmp(argv[i], debug_path, sizeof(debug_path) - 1) == 0) {
            debug_dump_path = argv[i] + sizeof(debug_path) - 1;
#endif
        } else {
            real_argv[real_argc++] = argv[i];
        }
    }
    argc = real_argc;
    argv = real_argv;

    vm_init(&vm, &chunk); /* registers io, same as every other stdlib module */

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
        /* Distinct from a runtime error: the program was still running correctly, it just had not
           finished. Its own exit code stays available for a program that both errored and ran long. */
        if (budget_exhausted) {
            fprintf(stderr, "aer: instruction budget of %u exhausted\n", max_instructions);
            status = EXIT_BUDGET_EXHAUSTED;
        }
    }

    /* Not load-bearing at process exit -- exercises the same teardown path an embedding host would use.
       real_argv is freed here rather than left to the OS because io.args() borrows a slice of it, so it
       has to outlive the script -- and because a report LeakSanitizer never prints clean is a report
       nobody reads. */
    aer_module_free_all();
    free(real_argv);
    aer_actor_free_all();
    return status;
}

/* Parse and run all statements in the current source buffer; resets bytecode each call, but name/constant pools persist so indices stay stable across REPL calls. */

static void run() {
    /* Append new code after any previous bytecode -- preserves function bodies compiled in earlier REPL calls. */
    aer_vm_reset_for_reuse(&vm);
    vm.ip = chunk.count;
    lex();
    /* parse() never resets its own tables (parser_reset, called once in main() above, already did
       that) and has its own per-statement rollback/recovery, so it's safe to call repeatedly --
       once per REPL line, or once for a whole file. */
    parse(&chunk);
    chunk_emit(&chunk, OP_HALT);
    runtime_had_error = false;
    /* A rolled-back statement compiles to a no-op, which is harmless for one bad REPL line but not
       for a file: a corrupted loop body (e.g. an increment that failed to compile) silently becomes
       an infinite loop instead of the syntax error it actually is. File mode refuses to run at all
       once parse() has flagged any statement as invalid; the REPL still runs the rest of the line. */
    if (mode == MODE_RUN && parse_had_error)
        return;
    if (max_instructions == 0) {
        vm_run(&vm);
        return;
    }
    /* Budgeted run (--max-instructions). A slice yields at loop back-edges and calls, so resuming
       in a loop is equivalent to an unbounded run until the budget is genuinely spent. */
    unsigned int remaining = max_instructions;
    for (;;) {
        unsigned int step = remaining < INSTRUCTION_SLICE ? remaining : INSTRUCTION_SLICE;
        VmSliceResult r = vm_run_slice(&vm, step);
        if (r != VM_SLICE_YIELDED)
            return;
        remaining -= step;
        if (remaining == 0) {
            budget_exhausted = true;
            return;
        }
    }
}

/* Interactive REPL */

static void run_shell() {
    mode = MODE_SHELL;
    start_terminal("aer");

    char* block_buf = NULL;
    size_t block_size = 0;
    bool in_block = false;

    while (1) {
        char* line = handle_terminal();
        if (!line) {
            /* Ctrl-C -- abandon any in-progress multi-line block, like Python's REPL */
            free(block_buf);
            block_buf = NULL;
            block_size = 0;
            in_block = false;
            set_terminal_prompt(">>> ");
            continue;
        }

        size_t len = strlen(line);

        bool ends_with_colon = false;
        for (size_t i = len; i > 0; i--) {
            char ch = line[i - 1];
            if (ch == '\n' || ch == ' ' || ch == '\t')
                continue;
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
            if (!blank)
                continue;

            set_terminal_prompt(">>> ");
            aer_run_source(&vm, &chunk, block_buf);
            free(block_buf);
            block_buf = NULL;
            block_size = 0;
            in_block = false;
            continue;
        }

        aer_run_source(&vm, &chunk, line);
    }
}

/* File execution */

static bool run_file(char* path) {
    mode = MODE_RUN;
    chunk.source_filename = xstrdup(path);
    read_file(path);
    /* read_file() can fail (missing file, unreadable, embedded NUL) without ever setting up the
       lexer's current file -- calling run() anyway would lex/parse a null or stale File*. */
    if (!aer_had_error())
        run();
#ifdef AER_DEBUG_TOOLS
    /* After run() so the dump has both the bytecode and the run's hit counts; "-" means stderr */
    const char* dump_path = debug_dump_path;
    if (dump_path) {
        FILE* dump_out = strcmp(dump_path, "-") == 0 ? stderr : fopen(dump_path, "w");
        if (!dump_out) {
            fprintf(stderr, "--debug-path: could not open '%s' for writing\n", dump_path);
            dump_out = stderr;
        }
        aer_disassemble(&chunk, dump_out);
        aer_debug_memory_report(dump_out);
        if (dump_out != stderr)
            fclose(dump_out);
    }
#endif
    /* A runtime error doesn't terminate the process (see error.c) -- the CLI decides to exit nonzero here; a failed assert() doesn't set aer_had_error() on purpose (see error.h), so it's checked separately. */
    return !aer_had_error() && aer_assert_failure_count() == 0;
}

static void help() {
    printf("Usage: aer [command|file]\n");
    printf("  help      Show this message\n");
    printf("  version   Show Aer version\n");
    printf(
        "  <file> [args...]   Execute an Aer source file; extra arguments reach the script via io.args()\n");
    printf("  (no args) Start interactive shell\n");
    printf("  --no-io              Disable the io module for this run\n");
    printf("  --no-net             Disable the net module for this run\n");
    printf(
        "  --no-import          Disable file-based import for this run (fixed stdlib modules still work)\n");
    printf(
        "  --memory-size=<N>[K|M|G]  Cap live GC cells (not bytes) at N; suffix multiplies by 1e3/1e6/1e9\n"
        "  --max-instructions=<N>    Stop after N instructions and exit 3; 0 (default) is unbounded\n");
#ifdef AER_DEBUG_TOOLS
    printf("  --debug-path=<path>  Write a disassembly + hit-count/memory dump here after running\n");
    printf("                       <file> (\"-\" for stderr)\n");
#endif
}

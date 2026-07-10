/* Standalone CLI runner for the v3 register-VM prototype — takes a single .aer file path and runs
   it through v3_parse()/vm_run(), mirroring main.c's run_file()/run() but targeting v3_parse()
   instead of parser.c's parse(). Exists purely so a real .aer file (e.g. nbody.aer) can actually
   be run through v3 for manual testing/benchmarking — v3 itself remains entirely absent from the
   normal aer.exe build (see the makefile's own comment on why), so this is a separate,
   always-opt-in binary, same isolation precedent as tests/v3_smoke_test.c and
   tests/embed_smoke_test.c.

   Build and run: make run-v3 FILE=nbody.aer
*/
#include <stdio.h>
#include "aer.h"
#include "aer_io.h"
#include "error.h"
#include "lexer.h"
#include "parser_v3.h"
#include "vm.h"

Token token;
Mode  mode;

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file.aer>\n", argv[0]);
        return 1;
    }

    mode = MODE_RUN;
    aer_io_register();   /* opt-in file-I/O host capability, same as main.c's own CLI */

    Chunk chunk;
    chunk_init(&chunk);

    read_file(argv[1]);
    lex();
    v3_parse(&chunk);
    if (parse_had_error) return 1;
    chunk_emit(&chunk, OP_HALT);

    VM vm;
    vm_init(&vm, &chunk);
    runtime_had_error = false;
    vm_run(&vm);

    return (!aer_had_error() && aer_assert_failure_count() == 0) ? 0 : 1;
}

/* embedding_example.c — a minimal, from-scratch demonstration of embedding AER in a host C
   program. This is the short version, meant to be read top to bottom; tests/embed_smoke_test.c is
   the exhaustive version this project's own test suite relies on for actual coverage.

   Build and run with:
     make example-embed
*/
#include <stdio.h>
#include "aer.h"
#include "error.h"    /* Mode */
#include "lexer.h"    /* Token */
#include "vm.h"

/* Owned by main.c in the normal CLI build (source/main.c) -- a host that doesn't link main.c, like
   this one, provides these globals itself. */
Token token;
Mode  mode;

/* A host function an AER script reaches exactly like a built-in module -- registered once, below,
   as "game.double_damage". */
static AerVal host_double_damage(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm; (void)userdata;
    if (arg_count == 1 && aer_type(args[0]) == TYPE_INTEGER)
        return aer_int(aer_as_int(args[0]) * 2);
    return aer_int(0);
}

static void on_script_error(const char* message, void* userdata) {
    (void)userdata;
    fprintf(stderr, "[script error] %s\n", message);
}

int main(void) {
    /* Every parse/runtime error goes through this instead of stderr by default -- a real host
       would route it to a game console, a log file, wherever. */
    aer_set_error_callback(on_script_error, NULL);

    /* Registration is process-global and must happen before any script referencing "game" is
       parsed. */
    aer_register_function("game", "double_damage", host_double_damage, NULL);

    Chunk chunk;
    VM    vm;
    chunk_init(&chunk);
    vm_init(&vm, &chunk);

    bool ok = aer_run_source(&vm, &chunk,
        "import game\n"
        "base = 10\n"
        "boosted = game.double_damage(base)\n"
        "print(\"boosted damage: {boosted}\")\n");

    if (!ok) {
        fprintf(stderr, "script failed: %s\n", aer_last_error());
        return 1;
    }

    /* Reusing the same VM for a second call needs no manual bookkeeping -- aer_run_source() resets
       stack/call-frame state itself every time, in case the *previous* call errored mid-execution
       (this one deliberately does, to prove the process survives it). */
    ok = aer_run_source(&vm, &chunk, "print(1 / 0)\n");
    printf("second call ok = %s (the process is still running either way)\n", ok ? "true" : "false");

    vm_free(&vm);
    chunk_free(&chunk);
    return 0;
}

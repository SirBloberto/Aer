/* Embedding smoke test — proves the thing this phase exists to prove: a
   runtime error no longer terminates the process. Links directly against
   the AER library sources (everything except source/main.c, which has
   its own conflicting main()) — this program IS a minimal embedding
   host, not a wrapper around the CLI.

   Build and run: make test-embed
*/
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "aer_host.h"
#include "aer_module.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"

/* setenv() isn't part of MSVC's/MinGW's standard C runtime the way it is on
   POSIX systems — _putenv_s is the Windows equivalent. Only used below, to
   exercise AER_PATH, which resolve_path() (aer_module.c) reads via plain
   getenv() on both platforms. */
#ifdef _WIN32
static void set_aer_path(const char* path) { _putenv_s("AER_PATH", path); }
#else
static void set_aer_path(const char* path) { setenv("AER_PATH", path, 1); }
#endif

/* Owned by main.c in the normal binary/aer build — this program doesn't
   link main.c, so it provides these globals itself, exactly as any host
   embedding AER would need to. */
Token token;
Mode  mode;

static char last_callback_msg[512] = "";
static int  callback_calls         = 0;

static void on_error(const char* message, void* userdata) {
    (void)userdata;
    callback_calls++;
    strncpy(last_callback_msg, message, sizeof(last_callback_msg) - 1);
    last_callback_msg[sizeof(last_callback_msg) - 1] = '\0';
}

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) printf("PASS: %s\n", what);
    else      { printf("FAIL: %s\n", what); failures++; }
}

/* A minimal host-registered function — everything a real one needs:
   read args, compute, return a Value. Registered below as "game.add". */
static Value host_add(VM* vm, int arg_count, Value* args, void* userdata) {
    (void)vm; (void)userdata;
    Value r = {0};
    r.type = TYPE_INTEGER;
    if (arg_count == 2 && args[0].type == TYPE_INTEGER && args[1].type == TYPE_INTEGER)
        r.data.integer = args[0].data.integer + args[1].data.integer;
    return r;
}

/* Mirrors main.c's run() — parse+run whatever text shell() was just
   handed, appended after any existing bytecode. Not calling into main.c
   itself; this is what a host's own equivalent of run() looks like.
     error_at()/error() (error.c) set runtime_had_error alongside parse_had_error
   unconditionally — even a compile error that parse()'s own per-statement recovery
   fully recovers from leaves runtime_had_error stuck true, and DISPATCH() (vm.c) aborts
   vm_run() before its first instruction whenever that flag is set. main.c's run() resets
   it right before vm_run() for exactly this reason; without the same reset here, a
   recovered parse error in one shell() segment silently no-ops every later run_appended()
   call's execution too, not just the segment that actually errored. */
static bool run_appended(Chunk* chunk, VM* vm) {
    unsigned int start = chunk->count;
    vm->ip = start;
    lex();
    parse(chunk);
    chunk_emit(chunk, OP_HALT);
    runtime_had_error = false;
    return vm_run(vm);
}

int main(void) {
    aer_set_error_callback(on_error, NULL);

    Chunk chunk;
    VM    vm;
    chunk_init(&chunk);
    vm_init(&vm, &chunk);
    parser_reset();
    mode = MODE_RUN;   /* the mode that used to exit(1) on any runtime error */

    /* A script that deliberately errors at runtime. Before this phase,
       reaching this line at all in MODE_RUN was impossible — the process
       would already be dead. */
    shell("print(1 / 0)\n");
    bool ok = run_appended(&chunk, &vm);

    check(!ok, "vm_run() returns false on a runtime error");
    check(aer_had_error(), "aer_had_error() is true after the error");
    check(callback_calls == 1, "the registered error callback fired exactly once");
    check(strstr(last_callback_msg, "Division by zero") != NULL,
          "the callback received the actual error message");
    check(strcmp(aer_last_error(), last_callback_msg) == 0,
          "aer_last_error() matches what the callback received");

    /* The embedding contract documented on vm_run() in vm.h: a host
       reusing the same VM* after an error must reset stack/call/scope
       state itself, applied here by hand, exactly like main.c's run()
       does between REPL statements. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();

    shell("print(2 + 2)\n");
    ok = run_appended(&chunk, &vm);

    check(ok, "the same VM runs a second, valid script successfully after the reset");
    check(!aer_had_error(), "no error state remains after a clean run");
    check(callback_calls == 1, "the callback did not fire again for the clean run");

    /* Runtime errors now report the source line they happened on (parse
       errors already did, via error_at() and its own source-context scan
       — this is the separate error()/current_runtime_line path used by
       every runtime fault, added because a runtime error deep in a script
       used to give no way to locate it at all). Line 4 here is "x = 1"
       plus two blank lines plus "bad = x.y" — chunk_line_for_offset() and
       current_runtime_line (source/core/vm.c, source/utilities/error.c)
       are what make this work. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("x = 1\n\n\nbad = x.y\n");
    ok = run_appended(&chunk, &vm);

    check(!ok, "field access on a non-struct still fails");
    check(strstr(aer_last_error(), "Line 4:") != NULL,
          "the runtime error names the actual source line, not just the message");

    /* Custom native-function registration: a host function reachable
       exactly like math/random/string, via import + dot-call. Uses
       assert() (checked below via aer_assert_failure_count()) rather than
       print()+eyeballing to verify the value actually round-tripped
       correctly through the C call, not just that the script ran. */
    aer_clear_error();
    aer_register_function("game", "add", host_add, NULL);
    shell("import game\nassert(game.add(3, 4) == 7, \"game.add returns the sum\")\n");
    ok = run_appended(&chunk, &vm);

    check(ok, "a script calling a host-registered function runs without error");
    check(aer_assert_failure_count() == 0,
          "the host function's return value round-tripped correctly (verified via assert())");

    /* panic(msg) — a user-invokable entry into the same abort path a
       VM-internal fault already takes. Can't be exercised from tests/test.aer
       itself (it would abort that whole file, same as any other runtime
       error) — here, a host can inspect vm_run()'s return value directly. */
    aer_clear_error();
    shell("panic(\"something went wrong\")\n");
    ok = run_appended(&chunk, &vm);

    check(!ok, "panic() aborts the call, just like any other runtime error");
    check(aer_had_error(), "aer_had_error() is true after panic()");
    check(strstr(aer_last_error(), "something went wrong") != NULL,
          "the panic message is reachable via aer_last_error()");

    /* Tail-call optimization only reuses the frame for a bare `return
       name(args)` — a call wrapped by another operator, like `+ 0` here,
       is deliberately excluded (see parse_return, parser.c), so deep
       *non*-tail recursion still correctly exhausts VM_CALL_MAX (64) and
       aborts with a runtime error. Proves the detection doesn't
       over-apply; tests/test.aer proves the true-tail-call case reuses
       the frame (completes far past 64 levels) — this is the negative
       case that can't run there, since it deliberately aborts the file. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("function not_tail(n):\n    if n <= 0:\n        return 0\n    return not_tail(n - 1) + 0\n\nnot_tail(1000)\n");
    ok = run_appended(&chunk, &vm);

    check(!ok, "deep non-tail recursion still overflows the call stack — tail-call detection did not over-apply");
    check(strstr(aer_last_error(), "overflow") != NULL,
          "the failure is specifically a call stack overflow, not some other error");

    /* AER_PATH search paths — resolve_path() (aer_module.c) only reaches its
       AER_PATH fallback once the same-directory candidate misses. shell()
       names its source "shell" (no directory component), so the
       same-directory candidate resolves relative to this process's cwd —
       the repo root when run via `make test-embed` — where
       searchpath_helper.aer does not exist; it only exists under tests/.
       Setting AER_PATH=tests here is what makes the import succeed at all. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    set_aer_path("tests");
    shell("import searchpath_helper\nassert(searchpath_helper.quadruple(5) == 20, \"quadruple via AER_PATH-resolved import\")\n");
    ok = run_appended(&chunk, &vm);

    check(ok, "a module found only via an AER_PATH directory still imports and runs successfully");
    check(aer_assert_failure_count() == 0,
          "the AER_PATH-resolved module's function returned the correct value");

    /* io is a host-registered module (source/stdlib/aer_io.h), not a hardcoded
       native one — see aer_io_register(). This test binary deliberately
       never calls it (unlike source/main.c, which does), proving file
       access really is opt-in per embedding host: `import io` here falls
       through to the generic file-import path, which fails to find an
       "io.aer" file on disk, exactly as if "io" were any other unknown
       name. import itself emits no bytecode either way, so vm_run() alone
       can't observe this — the failure only shows up as a parse-time error
       (aer_had_error()), not a false `ok`. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("import io\n");
    run_appended(&chunk, &vm);

    check(aer_had_error(),
          "import io fails in a host that never called aer_io_register() — file access is opt-in per host, not ambient");

    /* aer_module_call's mv (a file-module's own reused VM) didn't reset call_depth/stack_top after
       a runtime error inside a module function — the error unwinds via longjmp straight past
       OP_RETURN's normal call_depth--, so setup_call's next call pushes its frame at the wrong
       depth and its hardcoded dest_reg=0 write lands in the wrong frame's register 0, not
       mv->call_stack[0].registers[0] where aer_module_call always reads the result from. Not just
       an eventual "Call stack overflow" after enough failures — the very next call after a single
       failure silently returns whatever stale value already sat there, no error at all. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("import module_call_helper\n");
    run_appended(&chunk, &vm);

    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("module_call_helper.boom()\n");
    run_appended(&chunk, &vm);
    check(aer_had_error(), "a runtime error inside a module function is reported, not silently swallowed");

    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("assert(module_call_helper.good(5) == 50, \"a call after a prior failed call still returns the correct value, not a stale one from the wrong call frame\")\n");
    bool ok_after_module_error = run_appended(&chunk, &vm);
    check(ok_after_module_error, "the call after a prior module-function failure runs without error");
    check(aer_assert_failure_count() == 0,
          "good(5) returns 50, not null or any other stale value left over from boom()'s failed call");

    /* Generational GC — aer_gc_stats() introspection. Allocates far more
       short-lived arrays than MINOR_GC_THRESHOLD (2048, vm.c) — each loop
       iteration overwrites `temp`, so only the last one stays reachable,
       and everything before it is immediately collectible garbage.
       Asserting a collection actually ran and that the live-cell count
       stayed far below the iteration count is the only real proof
       reclamation happened, not just that nothing crashed. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("for i in 0..5000:\n    temp = [i, i * 2, i * 3]\n");
    ok = run_appended(&chunk, &vm);

    unsigned int live_cells, minor_collections, major_collections;
    aer_gc_stats(&live_cells, &minor_collections, &major_collections);

    check(ok, "a script allocating heavily in a loop still runs to completion");
    check(minor_collections > 0, "at least one minor collection actually ran under allocation pressure");
    /* live_cells counts every currently-live pool cell in the whole shared VM, not just this
       loop's own allocations — it also includes every object still held by earlier tests in this
       same file (module registrations, function registries, etc.), so this bound isn't "this
       loop's own leftovers," it's "still far below the 5000 iterations that ran," with headroom for
       the file's own accumulated baseline growing over time as more tests get added. */
    check(live_cells < 2000,
          "live cell count stayed far below the 5000 iterations that ran — reclamation, not just non-crashing");

    /* aer_gc_configure — a much smaller minor threshold should trigger far
       more collections than the default for the same workload. Restored
       to the defaults immediately after, so it doesn't affect the ceiling
       tests below. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    aer_gc_configure(20, 0);   /* tiny minor threshold; 0 leaves the major cadence alone */
    unsigned int minors_before;
    aer_gc_stats(NULL, &minors_before, NULL);
    shell("for i in 0..2000:\n    temp = [i, i * 2, i * 3]\n");
    ok = run_appended(&chunk, &vm);
    unsigned int minors_after;
    aer_gc_stats(NULL, &minors_after, NULL);
    aer_gc_configure(2048, 10);   /* restore defaults before the ceiling tests below */

    check(ok, "a script still runs correctly with an aggressively small GC threshold configured");
    check(minors_after - minors_before > 10,
          "aer_gc_configure's smaller minor threshold triggers many more collections for the same workload");

    /* aer_gc_set_ceiling — a deliberately tiny cap forces a script that
       keeps genuinely-live memory growing (not throwaway garbage) to
       abort with a normal, recoverable runtime error instead of growing
       forever. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    aer_gc_set_ceiling(50);
    shell("permanent = []\nfor i in 0..5000:\n    append(permanent, [i, i * 2, i * 3])\n");
    ok = run_appended(&chunk, &vm);

    check(!ok, "a script whose live memory keeps growing hits the ceiling and aborts");
    check(aer_had_error(), "aer_had_error() is true after the ceiling is exceeded");
    check(strstr(aer_last_error(), "ceiling") != NULL,
          "the error message identifies the memory ceiling as the cause");

    /* Disabling the ceiling (0) lets the same shape of script succeed —
       proves it doesn't wrongly reject once unset, not just that a tiny
       one rejects. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    aer_gc_set_ceiling(0);
    shell("permanent2 = []\nfor i in 0..5000:\n    append(permanent2, [i, i * 2, i * 3])\n");
    ok = run_appended(&chunk, &vm);

    check(ok, "the same shape of script succeeds once the ceiling is disabled (0)");

    /* A parse error inside a function body used to corrupt compilation of
       whatever top-level statement came next: the error-recovery skip-loop
       (parser.c's parse()) assumed the lexer was still mid-line when the
       error surfaced, but a failure inside a function/if/for body already
       leaves the lexer positioned at the START of the next statement (the
       nested parse_compound() call already recovered past its own DEDENT).
       The stale skip-loop then silently ate that next statement's tokens
       too, so e.g. "n1 = 42" right after a broken function definition never
       got compiled at all, and a later reference to n1 failed with a
       confusing, seemingly unrelated "'n1' is not defined" — see
       recovered_at_boundary in parser.c. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    /* x.y += 1 used to be this test's broken statement, back when compound
       field/index assignment (parser.c's parse_assignment) wasn't supported
       at all — now that it is, `x.` with no field name after the dot is the
       still-genuinely-invalid construct, unrelated to that feature. */
    shell("function f(x):\n    x. += 1\n\n"
          "n1 = 42\nassert(n1 == 42, \"a statement after a broken function body still compiles and runs\")\n");
    ok = run_appended(&chunk, &vm);

    check(aer_had_error(), "the malformed field access inside f() still reports its own error");
    check(aer_assert_failure_count() == 0,
          "n1 is defined and correct — the earlier error did not corrupt the next top-level statement");

    /* Opcode-fusion correctness: the fused OP_COMPOUND_NAME_* handlers
       (vm.c) must abort BEFORE writing the LHS back whenever a fallible
       sub-step fails — the unfused 4-opcode form relied on the
       *intervening* DISPATCH() between opcodes to skip the final STORE on
       an error partway through (e.g. an undefined RHS name), and a fused
       handler has no such intervening point unless it checks
       runtime_had_error explicitly after each sub-step. This can only be
       observed by inspecting VM state *after* an aborted statement, which
       needs the REPL-style statement-level abort this embedding harness
       already exercises above — file mode (MODE_RUN) would just exit. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("compound_x = 5\n");
    run_appended(&chunk, &vm);
    shell("compound_x += undefined_thing\n");
    ok = run_appended(&chunk, &vm);

    check(!ok, "compound assignment with an undefined RHS name fails, just like the unfused form did");

    aer_clear_error();
    shell("assert(compound_x == 5, \"the fused handler did not write back after the RHS failed to resolve\")\n");
    run_appended(&chunk, &vm);
    check(aer_assert_failure_count() == 0,
          "a failed OP_COMPOUND_NAME_NAME left its LHS completely unchanged, matching the unfused form");

    /* Array-index-get fusion (Part 2): an out-of-bounds/wrong-type index
       through a fused OP_INDEX_GET_*_* path must error and abort exactly
       like the unfused OP_INDEX_GET does — same reason this can't be
       checked from tests/test.aer (MODE_RUN exits on the first runtime
       error, so there's no way to observe "this expression errored, then
       execution continued" from inside that file). */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("ig_smoke_arr = [1, 2, 3]\n");
    run_appended(&chunk, &vm);
    shell("ig_smoke_oob = ig_smoke_arr[99]\n");
    ok = run_appended(&chunk, &vm);

    check(!ok, "an out-of-bounds index through a fused OP_INDEX_GET_NAME_CONST path fails, just like the unfused form did");
    check(aer_had_error(), "the out-of-bounds fused index read reports its own error");

    aer_clear_error();
    shell("assert(ig_smoke_arr[0] == 1, \"the array itself is untouched after the failed fused index read\")\n");
    run_appended(&chunk, &vm);
    check(aer_assert_failure_count() == 0,
          "a failed fused index-get did not corrupt the array or leave the VM in a bad state");

    /* A malformed `for` while-condition (found by tests/fuzz.py) used to still compile into
       a real, infinite back-edge loop despite the reported error. Can't be tested from a
       normal .aer file (the hang IS the bug) — here we just confirm the call returns. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("malformed_for_x = 1\nfor malformed_for_x !  print(\"body\")\n    malformed_for_x = 2\nmalformed_for_after = \"reached\"\n");
    run_appended(&chunk, &vm);

    check(aer_had_error(), "a malformed while-condition (missing ':') reports a parse error");

    aer_clear_error();
    shell("assert(malformed_for_after == \"reached\", \"the statement after a malformed for-loop still compiles and runs — recovery, not a stuck loop\")\n");
    run_appended(&chunk, &vm);
    check(aer_assert_failure_count() == 0,
          "execution continued past the malformed for-loop instead of looping forever");

    /* "Primitive pass" — a compound assignment that would change a raw-tracked local's own
       type (here: int /= promoting to real) is a compile error when it's textually inside a
       loop, not silently miscompiled — see project_aer_primitive_pass_step2 memory for why:
       the shadow-to-boxed transition needs the variable's OLD value, but the box instruction
       that reads it is bytecode that would re-execute every loop iteration, discarding
       whatever the boxed register had accumulated. Can't be tested from a normal .aer file the
       same way the malformed-for-loop case above can't — the compile error would abort the
       whole script before any assert() runs. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("function loop_shadow_div():\n    p_raw = 4\n    for k in 0..3:\n        if k == 1:\n            p_raw /= 2\n    return p_raw\n");
    run_appended(&chunk, &vm);
    check(aer_had_error(),
          "compound-assigning a raw-tracked local to a different type inside a loop is a compile error, not silent corruption");

    /* Same hazard, but via a PLAIN self-referential assignment (`total = total + x`), not `+=` —
       found live: this path's shadow used to be treated as unconditionally safe on the theory that
       a plain assignment always overwrites with a brand-new value, missing that the RHS itself can
       read the variable's own OLD (raw) value before the shadow, and that read is bytecode that
       re-executes every loop iteration, always seeing the same frozen pre-loop value instead of
       accumulating. Same fix, same reasoning as the compound case above: a compile error, not a
       silent wrong answer. */
    vm.stack_top     = 0;
    vm.call_depth = 0;
    vm.registers  = vm.call_stack[0].registers;
    vm.raw_ints   = vm.call_stack[0].raw_ints;
    vm.raw_reals  = vm.call_stack[0].raw_reals;
    aer_clear_error();
    shell("function loop_shadow_plain():\n    total_raw = 0\n    k = 0\n    for k < 3:\n        x = length(\"ab\")\n        total_raw = total_raw + x\n        k = k + 1\n    return total_raw\n");
    run_appended(&chunk, &vm);
    check(aer_had_error(),
          "plain-assigning a raw-tracked local to a boxed value inside a loop is a compile error, not silent corruption");

    /* aer_module_free_all — searchpath_helper (loaded earlier via AER_PATH) proves there's
       something in the registry to tear down; aer_module_get(0, ...) going from true to
       false is the actual proof of reclamation, not just that the process exits cleanly. */
    VM* mod_vm; Chunk* mod_chunk;
    check(aer_module_get(0, &mod_vm, &mod_chunk), "a file-module is registered before teardown");
    aer_module_free_all();
    check(!aer_module_get(0, &mod_vm, &mod_chunk), "aer_module_free_all() actually clears the module registry");

    vm_free(&vm);
    chunk_free(&chunk);

    if (failures == 0) printf("\nAll embedding smoke tests passed.\n");
    else                printf("\n%d embedding smoke test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}

/* Embedding smoke test — proves a runtime error no longer terminates the process. Links directly
   against the AER library sources (everything except source/main.c, which has its own conflicting
   main()) — this program IS a minimal embedding host, not a wrapper around the CLI.

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
   read args, compute, return an AerVal. Registered below as "game.add". */
static AerVal host_add(VM* vm, int arg_count, AerVal* args, void* userdata) {
    (void)vm; (void)userdata;
    if (arg_count == 2 && aer_type(args[0]) == TYPE_INTEGER && aer_type(args[1]) == TYPE_INTEGER)
        return aer_int(aer_as_int(args[0]) + aer_as_int(args[1]));
    return aer_int(0);
}

int main(void) {
    aer_set_error_callback(on_error, NULL);

    Chunk chunk;
    VM    vm;
    chunk_init(&chunk);
    vm_init(&vm, &chunk);
    parser_reset();
    mode = MODE_RUN;

    /* A script that deliberately errors at runtime; MODE_RUN survives it without terminating. */
    bool ok = aer_run_source(&vm, &chunk, "print(1 / 0)\n");

    check(!ok, "vm_run() returns false on a runtime error");
    check(aer_had_error(), "aer_had_error() is true after the error");
    check(callback_calls == 1, "the registered error callback fired exactly once");
    check(strstr(last_callback_msg, "Division by zero") != NULL,
          "the callback received the actual error message");
    check(strcmp(aer_last_error(), last_callback_msg) == 0,
          "aer_last_error() matches what the callback received");

    /* aer_run_source() resets the VM for reuse itself (aer_vm_reset_for_reuse) — a host driving
       vm_run()/vm_run_slice() directly on its own VM would need to call that itself between calls,
       same contract vm_run() has always documented. */
    aer_clear_error();

    ok = aer_run_source(&vm, &chunk, "print(2 + 2)\n");

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
    aer_clear_error();
    ok = aer_run_source(&vm, &chunk, "x = 1\n\n\nbad = x.y\n");

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
    ok = aer_run_source(&vm, &chunk, "import game\nassert(game.add(3, 4) == 7, \"game.add returns the sum\")\n");

    check(ok, "a script calling a host-registered function runs without error");
    check(aer_assert_failure_count() == 0,
          "the host function's return value round-tripped correctly (verified via assert())");

    /* panic(msg) — a user-invokable entry into the same abort path a
       VM-internal fault already takes. Can't be exercised from tests/test.aer
       itself (it would abort that whole file, same as any other runtime
       error) — here, a host can inspect vm_run()'s return value directly. */
    aer_clear_error();
    ok = aer_run_source(&vm, &chunk, "panic(\"something went wrong\")\n");

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
    aer_clear_error();
    ok = aer_run_source(&vm, &chunk,
        "function not_tail(n):\n    if n <= 0:\n        return 0\n    return not_tail(n - 1) + 0\n\nnot_tail(1000)\n");

    check(!ok, "deep non-tail recursion still overflows the call stack — tail-call detection did not over-apply");
    check(strstr(aer_last_error(), "overflow") != NULL,
          "the failure is specifically a call stack overflow, not some other error");

    /* AER_PATH search paths — resolve_path() (aer_module.c) only reaches its
       AER_PATH fallback once the same-directory candidate misses. aer_run_source()'s
       internal shell() call names its source "shell" (no directory component), so the
       same-directory candidate resolves relative to this process's cwd —
       the repo root when run via `make test-embed` — where
       searchpath_helper.aer does not exist; it only exists under tests/.
       Setting AER_PATH=tests here is what makes the import succeed at all. */
    aer_clear_error();
    set_aer_path("tests");
    ok = aer_run_source(&vm, &chunk,
        "import searchpath_helper\nassert(searchpath_helper.quadruple(5) == 20, \"quadruple via AER_PATH-resolved import\")\n");

    check(ok, "a module found only via an AER_PATH directory still imports and runs successfully");
    check(aer_assert_failure_count() == 0,
          "the AER_PATH-resolved module's function returned the correct value");

    /* io is registered by vm_init() itself now (source/core/vm.c's ensure_io_registered), the
       same always-on status as every other stdlib module — this test binary never calls
       aer_io_register() itself (unlike source/main.c, which used to be the only thing that did),
       and io.exists() still works, proving io is no longer a host opt-in. */
    aer_clear_error();
    ok = aer_run_source(&vm, &chunk, "import io\nassert(io.exists(\"tests\") == true, \"io.exists() finds the real tests/ directory\")\n");

    check(ok && !aer_had_error(),
          "import io succeeds with no host action at all — io is unconditionally available, same as every other stdlib module");
    check(aer_assert_failure_count() == 0, "io.exists() actually works through this unconditional registration, not just a successful import");

    /* aer_module_call's mv (a file-module's own reused VM) didn't reset call_depth/stack_top after
       a runtime error inside a module function — the error unwinds via longjmp straight past
       OP_RETURN's normal call_depth--, so setup_call's next call pushes its frame at the wrong
       depth and its hardcoded dest_reg=0 write lands in the wrong frame's register 0, not
       mv->call_stack[0].registers[0] where aer_module_call always reads the result from. Not just
       an eventual "Call stack overflow" after enough failures — the very next call after a single
       failure silently returns whatever stale value already sat there, no error at all. */
    aer_clear_error();
    aer_run_source(&vm, &chunk, "import module_call_helper\n");

    aer_clear_error();
    aer_run_source(&vm, &chunk, "module_call_helper.boom()\n");
    check(aer_had_error(), "a runtime error inside a module function is reported, not silently swallowed");

    aer_clear_error();
    bool ok_after_module_error = aer_run_source(&vm, &chunk,
        "assert(module_call_helper.good(5) == 50, \"a call after a prior failed call still returns the correct value, not a stale one from the wrong call frame\")\n");
    check(ok_after_module_error, "the call after a prior module-function failure runs without error");
    check(aer_assert_failure_count() == 0,
          "good(5) returns 50, not null or any other stale value left over from boom()'s failed call");

    /* setup_call (aer_module_call's own frame-push path, separate from lbl_call's OP_CALL) sizes
       raw_ints/raw_reals from fn->max_raw_ints/max_raw_reals -- a bump-pointer bug there (wrong
       offset, overlapping a sibling frame's raw slots) would corrupt raw_calc's own accumulator
       silently rather than crash, since raw storage is never GC-scanned or otherwise validated. */
    aer_clear_error();
    ok = aer_run_source(&vm, &chunk,
        "assert(module_call_helper.raw_calc(5) == 10, \"raw locals inside a module-called function (setup_call's frame-push path) compute correctly\")\n");
    check(ok && !aer_had_error(), "a module function using raw int locals runs without error through setup_call");
    check(aer_assert_failure_count() == 0, "raw_calc(5) == 10 (0+1+2+3+4), proving setup_call's raw_ints/raw_reals bump-pointer sizing is correct");

    /* Generational GC — aer_gc_stats() introspection. Allocates far more
       short-lived arrays than MINOR_GC_THRESHOLD (2048, vm.c) — each loop
       iteration overwrites `temp`, so only the last one stays reachable,
       and everything before it is immediately collectible garbage.
       Asserting a collection actually ran and that the live-cell count
       stayed far below the iteration count is the only real proof
       reclamation happened, not just that nothing crashed. */
    aer_clear_error();
    ok = aer_run_source(&vm, &chunk, "for i in 0..5000:\n    temp = [i, i * 2, i * 3]\n");

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
    aer_clear_error();
    aer_gc_configure(20, 0);   /* tiny minor threshold; 0 leaves the major cadence alone */
    unsigned int minors_before;
    aer_gc_stats(NULL, &minors_before, NULL);
    ok = aer_run_source(&vm, &chunk, "for i in 0..2000:\n    temp = [i, i * 2, i * 3]\n");
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
    aer_clear_error();
    aer_gc_set_ceiling(50);
    ok = aer_run_source(&vm, &chunk, "import collection\npermanent = []\nfor i in 0..5000:\n    collection.append(permanent, [i, i * 2, i * 3])\n");

    check(!ok, "a script whose live memory keeps growing hits the ceiling and aborts");
    check(aer_had_error(), "aer_had_error() is true after the ceiling is exceeded");
    check(strstr(aer_last_error(), "ceiling") != NULL,
          "the error message identifies the memory ceiling as the cause");

    /* Disabling the ceiling (0) lets the same shape of script succeed —
       proves it doesn't wrongly reject once unset, not just that a tiny
       one rejects. */
    aer_clear_error();
    aer_gc_set_ceiling(0);
    ok = aer_run_source(&vm, &chunk, "permanent2 = []\nfor i in 0..5000:\n    collection.append(permanent2, [i, i * 2, i * 3])\n");

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
    aer_clear_error();
    /* x.y += 1 used to be this test's broken statement, back when compound
       field/index assignment (parser.c's parse_assignment) wasn't supported
       at all — now that it is, `x.` with no field name after the dot is the
       still-genuinely-invalid construct, unrelated to that feature. */
    ok = aer_run_source(&vm, &chunk,
        "function f(x):\n    x. += 1\n\n"
        "n1 = 42\nassert(n1 == 42, \"a statement after a broken function body still compiles and runs\")\n");

    check(aer_had_error(), "the malformed field access inside f() still reports its own error");
    check(aer_assert_failure_count() == 0,
          "n1 is defined and correct — the earlier error did not corrupt the next top-level statement");

    /* Opcode-fusion correctness: the fused OP_COMPOUND_NAME_* handlers
       (vm.c) must abort BEFORE writing the LHS back whenever a fallible
       sub-step fails — the unfused 4-opcode form relied on the
       *intervening* DISPATCH() between opcodes to skip the final STORE on
       an error partway through (e.g. a type-mismatched RHS), and a fused
       handler has no such intervening point unless it checks
       runtime_had_error explicitly after each sub-step. This can only be
       observed by inspecting VM state *after* an aborted statement, which
       needs the REPL-style statement-level abort this embedding harness
       already exercises above — file mode (MODE_RUN) would just exit.
         The RHS must be an already-DEFINED name of the wrong type (not an
       undefined one) to land in OP_COMPOUND_NAME_NAME specifically: an
       undefined name is now a parse-time error (see parser.c's
       report_if_shadowed_global/"is not defined" fallback), so it never
       reaches this fused runtime opcode at all. */
    aer_clear_error();
    aer_run_source(&vm, &chunk, "compound_x = 5\n");
    aer_run_source(&vm, &chunk, "wrong_type = \"abc\"\n");
    ok = aer_run_source(&vm, &chunk, "compound_x += wrong_type\n");

    check(!ok, "compound assignment with a type-mismatched RHS fails, just like the unfused form did");

    aer_clear_error();
    aer_run_source(&vm, &chunk, "assert(compound_x == 5, \"the fused handler did not write back after the RHS failed to resolve\")\n");
    check(aer_assert_failure_count() == 0,
          "a failed OP_COMPOUND_NAME_NAME left its LHS completely unchanged, matching the unfused form");

    /* Array-index-get fusion (Part 2): an out-of-bounds/wrong-type index
       through a fused OP_INDEX_GET_*_* path must error and abort exactly
       like the unfused OP_INDEX_GET does — same reason this can't be
       checked from tests/test.aer (MODE_RUN exits on the first runtime
       error, so there's no way to observe "this expression errored, then
       execution continued" from inside that file). */
    aer_clear_error();
    aer_run_source(&vm, &chunk, "ig_smoke_arr = [1, 2, 3]\n");
    ok = aer_run_source(&vm, &chunk, "ig_smoke_oob = ig_smoke_arr[99]\n");

    check(!ok, "an out-of-bounds index through a fused OP_INDEX_GET_NAME_CONST path fails, just like the unfused form did");
    check(aer_had_error(), "the out-of-bounds fused index read reports its own error");

    aer_clear_error();
    aer_run_source(&vm, &chunk, "assert(ig_smoke_arr[0] == 1, \"the array itself is untouched after the failed fused index read\")\n");
    check(aer_assert_failure_count() == 0,
          "a failed fused index-get did not corrupt the array or leave the VM in a bad state");

    /* TYPE_STRUCT split: `for x in <struct>:` has no bracket/slice/append/delete-style guard of
       its own -- worth a direct check that it's a clean rejection ("only supports arrays, dicts,
       and strings", the same message every other unsupported-collection-type error already uses)
       rather than silently iterating a struct's fields the way it did before the split. Can't be
       checked from a .aer test file for the same MODE_RUN-aborts-on-error reason as the fused
       index-get case just above. */
    aer_clear_error();
    ok = aer_run_source(&vm, &chunk,
        "struct ForInStruct:\n    a = 1\n    b = 2\nfis_s = ForInStruct()\nfor fis_x in fis_s:\n    fis_x = fis_x\n");
    check(!ok && aer_had_error() && strstr(aer_last_error(), "only supports arrays, dicts, strings, and typed arrays") != NULL,
          "'for x in <struct>:' is a clean, reported error, not a silent iteration over its fields");

    /* A malformed `for` while-condition (found by tests/fuzz.py) used to still compile into
       a real, infinite back-edge loop despite the reported error. Can't be tested from a
       normal .aer file (the hang IS the bug) — here we just confirm the call returns. */
    aer_clear_error();
    aer_run_source(&vm, &chunk,
        "malformed_for_x = 1\nfor malformed_for_x !  print(\"body\")\n    malformed_for_x = 2\nmalformed_for_after = \"reached\"\n");

    check(aer_had_error(), "a malformed while-condition (missing ':') reports a parse error");

    aer_clear_error();
    aer_run_source(&vm, &chunk,
        "assert(malformed_for_after == \"reached\", \"the statement after a malformed for-loop still compiles and runs — recovery, not a stuck loop\")\n");
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
    aer_clear_error();
    aer_run_source(&vm, &chunk,
        "function loop_shadow_div():\n    p_raw = 4\n    for k in 0..3:\n        if k == 1:\n            p_raw /= 2\n    return p_raw\n");
    check(aer_had_error(),
          "compound-assigning a raw-tracked local to a different type inside a loop is a compile error, not silent corruption");

    /* Same hazard, but via a PLAIN self-referential assignment (`total = total + x`), not `+=` —
       found live: this path's shadow used to be treated as unconditionally safe on the theory that
       a plain assignment always overwrites with a brand-new value, missing that the RHS itself can
       read the variable's own OLD (raw) value before the shadow, and that read is bytecode that
       re-executes every loop iteration, always seeing the same frozen pre-loop value instead of
       accumulating. Same fix, same reasoning as the compound case above: a compile error, not a
       silent wrong answer. */
    aer_clear_error();
    aer_run_source(&vm, &chunk,
        "function loop_shadow_plain():\n    total_raw = 0\n    k = 0\n    for k < 3:\n        x = length(\"ab\")\n        total_raw = total_raw + x\n        k = k + 1\n    return total_raw\n");
    check(aer_had_error(),
          "plain-assigning a raw-tracked local to a boxed value inside a loop is a compile error, not silent corruption");

    /* net handles are resolved through a registry (aer_net.c), never a raw socket cast through an
       int — a bad or stale handle must be a clean, reported error, not silently operate on
       whatever OS handle that integer happens to collide with (net.close(0) used to mean "close
       real stdin" before this fix). Can't be checked from tests/test_net.aer: MODE_RUN aborts the
       whole file on this class of error, same reason wrong-argument-type checks live here too. */
    aer_clear_error();
    ok = aer_run_source(&vm, &chunk, "import net\nnet.close(999999)\n");
    check(!ok && strstr(aer_last_error(), "no such connection handle") != NULL,
          "net.close() on a handle that was never connected fails cleanly, not silently or on a real fd");

    /* Coarse capability toggles (aer_set_io_enabled/net_enabled/import_enabled, --no-io/--no-net/
       --no-import at the CLI). Each is a process-wide flag, not something a running .aer script
       can flip on itself — this embedding-level check is the natural place for coverage, the same
       reason aer_gc_set_ceiling() is tested here rather than from tests/test_*.aer. Every check
       restores the default (true) immediately after, so no later test in this file is affected. */
    aer_clear_error();
    aer_set_net_enabled(false);
    ok = aer_run_source(&vm, &chunk, "import net\nnet.connect(\"127.0.0.1\", 1)\n");
    aer_set_net_enabled(true);
    check(!ok && strstr(aer_last_error(), "--no-net") != NULL,
          "net.connect() reports a clean, specific error when net is disabled, not a crash or a silent no-op");

    aer_clear_error();
    aer_set_io_enabled(false);
    ok = aer_run_source(&vm, &chunk, "io.exists(\"tests\")\n");
    aer_set_io_enabled(true);
    check(!ok && strstr(aer_last_error(), "--no-io") != NULL,
          "io.exists() reports a clean, specific error when io is disabled");

    aer_clear_error();
    aer_set_import_enabled(false);
    /* The import failure is a parse-time error (chunk_add_import/error_at()), not a runtime one —
       aer_run_source()'s own return value only reflects vm_run()'s outcome, and a single failed
       import statement rolls back to a no-op that vm_run() then trivially succeeds on. Check
       aer_had_error() (which error_at() does set), not aer_run_source()'s return value, to observe
       a pure parse-time failure correctly — the same distinction "field access on a non-struct"
       above draws between parse and runtime errors. */
    aer_run_source(&vm, &chunk, "import searchpath_helper\n");
    aer_set_import_enabled(true);
    check(aer_had_error() && strstr(aer_last_error(), "--no-import") != NULL,
          "file-based import reports a clean, specific error when import is disabled");

    aer_clear_error();
    ok = aer_run_source(&vm, &chunk, "import math\nassert(math.sqrt(4.0) == 2.0, \"math still works\")\n");
    check(ok && aer_assert_failure_count() == 0,
          "fixed-dispatch modules like math are unaffected by --no-import — only file-based import is gated");

    /* aer_module_free_all — searchpath_helper (loaded earlier via AER_PATH) proves there's
       something in the registry to tear down; a real aer_module_call() succeeding, then failing
       to resolve at all once the registry is cleared, is the actual proof of reclamation, not
       just that the process exits cleanly. */
    vm_stack_push(&vm, aer_int(5));
    bool call_ok = aer_module_call(&vm, "searchpath_helper", "quadruple", 1);
    AerVal call_result = call_ok ? vm_stack_pop(&vm) : aer_null();
    check(call_ok && aer_type(call_result) == TYPE_INTEGER && aer_as_int(call_result) == 20,
          "a file-module is registered before teardown");
    aer_module_free_all();
    vm_stack_push(&vm, aer_int(5));
    check(!aer_module_call(&vm, "searchpath_helper", "quadruple", 1),
          "aer_module_free_all() actually clears the module registry");
    vm_stack_pop(&vm);   /* aer_module_call left the pushed argument on the stack when it returned false */

    vm_free(&vm);
    chunk_free(&chunk);

    /* Per-VM heap isolation — two independent VM/Chunk pairs; heavy allocation in one must not
       inflate the other's own live-cell count. This is the actual proof heaps are independent,
       not just that nothing crashes: under the old shared-heap design both VMs' live_cells would
       have risen together, since there was only ever one shared set of pools. */
    {
        Chunk chunk_a, chunk_b;
        VM vm_a, vm_b;
        chunk_init(&chunk_a); vm_init(&vm_a, &chunk_a);
        chunk_init(&chunk_b); vm_init(&vm_b, &chunk_b);

        vm_set_current_heap(&vm_b.heap);
        unsigned int live_b_before;
        aer_gc_stats(&live_b_before, NULL, NULL);

        aer_clear_error();
        bool iso_ok = aer_run_source(&vm_a, &chunk_a, "for i in 0..5000:\n    temp = [i, i * 2, i * 3]\n");

        vm_set_current_heap(&vm_a.heap);
        unsigned int live_a;
        aer_gc_stats(&live_a, NULL, NULL);

        vm_set_current_heap(&vm_b.heap);
        unsigned int live_b_after;
        aer_gc_stats(&live_b_after, NULL, NULL);

        check(iso_ok, "the heavy-allocation script on vm_a ran to completion without error");
        /* vm_a's own heap actually did the work (a real collection ran, keeping this bounded --
           same assertion shape as the earlier single-VM GC test). */
        check(live_a < 2000, "vm_a's own live cell count reflects its allocation, collected down same as any single VM");
        check(live_b_before == live_b_after,
              "vm_b's live cell count is completely unchanged by vm_a's allocation — the two heaps never touched each other");

        vm_free(&vm_a);
        chunk_free(&chunk_a);
        vm_free(&vm_b);
        chunk_free(&chunk_b);
    }

    /* A runtime error INSIDE a shape-specializing function's recompiled body used to report a line
       number relative to the retained source span's own start (lexer_begin_span re-lexes an
       isolated copy of just the function's text, and current_source_line() counted newlines from
       THAT copy's own beginning) instead of the true absolute line in the original source -- found
       by inspecting the disassembler's own --debug-path=- output. `compute`'s division sits on
       line 6 of this script; before the fix this would have reported "Line 3:" instead (span-
       relative: line 1 of the span is `(p, divisor):`, the parameter list, on line 4 of the real
       file). `p.value` (line 5) is what makes `p` shape-sensitive and triggers the recompile on
       the very first call.

       A FRESH Chunk+VM, not the shared vm/chunk above -- the shared pair is torn down for good
       several tests up (vm_free(&vm); chunk_free(&chunk);, proving aer_module_free_all() actually
       clears the module registry) and never touched again after that point; this test needs its
       own independent, still-live pair, same as vm_a/vm_b and the 500-cycle block just below. (An
       earlier version of this comment blamed a "pool corruption" bug in Chunk.name_index's
       string-interning pool for a segfault at this exact spot -- confirmed via a gdb watchpoint on
       chunk.name_index.pools that the real cause was simpler: an earlier draft of this test
       mistakenly reused the ALREADY-FREED shared vm/chunk here, so chunk_add_pool's string-
       interning dereferenced a HashPools pointer that chunk_free had already zeroed. No library bug
       at all -- ordinary use-after-free in this test file, fixed by using a fresh pair like this.) */
    {
        Chunk spec_chunk;
        VM    spec_vm;
        chunk_init(&spec_chunk);
        vm_init(&spec_vm, &spec_chunk);
        /* parser_reset()'s own contract: "ONCE per independent program, never between statements of
           the same session" -- this fresh chunk starts a genuinely independent program, not a
           continuation of the shared vm/chunk's ongoing REPL-like session above, so it needs its own
           reset (matches vm_a/vm_b's own fresh-chunk block, which happens not to need this only
           because its script defines no functions/structs -- the global, non-per-chunk parser tables
           that function/struct definitions populate are what actually need resetting here). Safe to
           call here: nothing later in this file uses the shared vm/chunk again. */
        parser_reset();
        aer_clear_error();
        bool spec_ok = aer_run_source(&spec_vm, &spec_chunk,
            "struct Divisor:\n"
            "    value = 0\n"
            "\n"
            "function compute(p, divisor):\n"
            "    x = p.value\n"
            "    y = x / divisor\n"
            "    return y\n"
            "\n"
            "compute(Divisor(10), 0)\n");
        check(!spec_ok, "division by zero inside a specialized function body still fails");
        check(strstr(aer_last_error(), "Line 7:") != NULL,
              "the error names the TRUE source line (a pre-existing, separate off-by-one attributes "
              "it to the trailing return statement's line rather than the division's own line 6 -- "
              "see this test's own follow-up comment), not one relative to the retained span");
        check(strstr(aer_last_error(), "Line 3:") == NULL,
              "the error does NOT report the old, buggy span-relative line number");
        vm_free(&spec_vm);
        chunk_free(&spec_chunk);
    }

    /* Repeated vm_init()+run+vm_free() cycles, mirroring source/tools/aer_lsp.c's
       run_diagnostics() — a fresh Chunk+VM per request, used once, then discarded. vm_free() used
       to be a complete no-op, so every one of these cycles permanently grew the shared pools
       forever in a long-running host process (the LSP server). It's real work now
       (pool_finalize_all + pool_destroy per heap) — this proves hundreds of real init+run+free
       cycles complete cleanly rather than crashing or corrupting, the actual risk surface a
       previously-untested vm_free() introduces. */
    {
        bool cycles_ok = true;
        for (int iter = 0; iter < 500 && cycles_ok; iter++) {
            Chunk c;
            VM v;
            chunk_init(&c);
            vm_init(&v, &c);
            aer_clear_error();
            cycles_ok = aer_run_source(&v, &c,
                "arr = [1, 2, 3]\nd = {\"a\": 1, \"b\": 2}\ns = \"a fresh string literal each cycle\"\n")
                && !aer_had_error();
            vm_free(&v);
            chunk_free(&c);
        }
        check(cycles_ok, "500 repeated vm_init()+run+vm_free() cycles (aer_lsp.c's own usage pattern) all complete cleanly");
    }

    if (failures == 0) printf("\nAll embedding smoke tests passed.\n");
    else                printf("\n%d embedding smoke test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}

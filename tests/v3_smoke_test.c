/* v3 register-VM prototype, M1 smoke test — proves the register allocator (parser_v3.c) and the
   three OP_V3_* opcodes (vm.c) work correctly together for genuinely nested expressions, entirely
   independent of the real .aer lexer/parser/interpreter (which this test never touches). See the
   register-based bytecode plan for the full M1 scope and why this is deliberately isolated.

   Build and run: make test-v3
*/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "aer.h"
#include "error.h"
#include "lexer.h"
#include "parser_v3.h"
#include "vm.h"

/* Owned by main.c in the normal binary/aer build — this program doesn't link main.c (LIBOBJECT
   excludes it, same as embed_smoke_test.c), so parser.c/lexer.c's extern references to these need
   a definition here even though this test never calls into the real lexer/parser. */
Token token;
Mode  mode;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) printf("PASS: %s\n", what);
    else      { printf("FAIL: %s\n", what); failures++; }
}

/* M5 slice 9 — compares a v3 register's string value against a C string, for interpolation tests. */
static bool v3_string_eq(AerVal v, const char* expected) {
    if (aer_type(v) != TYPE_STRING) return false;
    AerString* s = aer_as_string(v);
    size_t elen = strlen(expected);
    return s->length == elen && memcmp(s->data, expected, elen) == 0;
}

/* Runs `c` (already ending in OP_HALT) on a fresh VM, returning true on a clean finish.
   runtime_had_error is a global DISPATCH() checks on every single instruction (see error.h) — the
   normal interpreter resets it once per REPL statement (main.c's run()); a test harness invoking
   vm_run() directly, separately, per test case must do the same, or an earlier test's deliberate
   error (Test 3's division by zero) silently poisons every later test's very first dispatch. */
static bool run_chunk(Chunk* c) {
    VM vm;
    vm_init(&vm, c);
    runtime_had_error = false;
    return vm_run(&vm);
}

/* M5 slice 2 — real .aer source through v3, mirroring embed_smoke_test.c's run_appended() but
   targeting v3_parse() instead of the real parser.c's parse(). Variables are read back by
   encounter order afterward (v3_var_names[i]'s register is always i — the first name assigned in
   `src` lands in register 0, the second in register 1, etc.), since there's no name-based lookup
   exposed; each test's own comment documents which name is which register. */
static bool v3_run_source(Chunk* c, const char* src) {
    shell((char*)src);
    lex();
    v3_parse(c);
    if (parse_had_error) return false;   /* don't run on possibly-incomplete bytecode */
    chunk_emit(c, OP_HALT);
    VM vm;
    vm_init(&vm, c);
    runtime_had_error = false;
    return vm_run(&vm);
}

int main(void) {
    mode = MODE_RUN;   /* real-source tests (11+) go through the real lexer; not REPL semantics */

    /* Test 1: (2+3) * (4+5), all constant leaves — no locals in play, purely exercising the
       allocator's compact-reuse behavior (see parser_v3.c's free-then-allocate comment) across a
       tree deep enough that the naive "never reuse a slot" approach would need 3 registers, not 1. */
    {
        Chunk c;
        chunk_init(&c);
        v3_reg_reset();

        V3Node two   = { .kind = V3_NODE_CONST, .const_value = aer_int(2) };
        V3Node three = { .kind = V3_NODE_CONST, .const_value = aer_int(3) };
        V3Node four  = { .kind = V3_NODE_CONST, .const_value = aer_int(4) };
        V3Node five  = { .kind = V3_NODE_CONST, .const_value = aer_int(5) };
        V3Node add_l = { .kind = V3_NODE_BINARY, .bin_op = OP_ADD, .lhs = &two,  .rhs = &three };
        V3Node add_r = { .kind = V3_NODE_BINARY, .bin_op = OP_ADD, .lhs = &four, .rhs = &five  };
        V3Node mul   = { .kind = V3_NODE_BINARY, .bin_op = OP_MUL, .lhs = &add_l, .rhs = &add_r };

        int result_reg = v3_compile_node(&c, &mul);
        chunk_emit(&c, OP_HALT);

        check(run_chunk(&c), "(2+3)*(4+5): chunk ran to completion without error");
        AerVal result = v3_register_get(result_reg);
        check(aer_type(result) == TYPE_INTEGER && aer_as_int(result) == 45,
              "(2+3)*(4+5) == 45, computed via nested OP_V3_BINARY with register reuse");
        check(result_reg == 0,
              "the allocator reused register 0 for the final result instead of growing to register 2");

        chunk_free(&c);
    }

    /* Test 2: a*a + b*b with a=3, b=4 — "locals" set up via OP_V3_LOADK into reserved registers,
       proving V3_NODE_REG leaves (an already-live register, not a fresh constant) compile
       correctly and are never freed by the allocator. */
    {
        Chunk c;
        chunk_init(&c);
        v3_reg_reset();

        unsigned int pool_a = chunk_add_pool(&c, aer_int(3));
        unsigned int pool_b = chunk_add_pool(&c, aer_int(4));
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 0); chunk_emit(&c, (int)pool_a);  /* reg 0 = 3 */
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 1); chunk_emit(&c, (int)pool_b);  /* reg 1 = 4 */
        v3_reg_reserve(2);   /* registers 0,1 are now "locals" — never freed/reallocated below */

        V3Node a1 = { .kind = V3_NODE_REG, .reg = 0 };
        V3Node a2 = { .kind = V3_NODE_REG, .reg = 0 };
        V3Node b1 = { .kind = V3_NODE_REG, .reg = 1 };
        V3Node b2 = { .kind = V3_NODE_REG, .reg = 1 };
        V3Node a_sq = { .kind = V3_NODE_BINARY, .bin_op = OP_MUL, .lhs = &a1, .rhs = &a2 };
        V3Node b_sq = { .kind = V3_NODE_BINARY, .bin_op = OP_MUL, .lhs = &b1, .rhs = &b2 };
        V3Node sum  = { .kind = V3_NODE_BINARY, .bin_op = OP_ADD, .lhs = &a_sq, .rhs = &b_sq };

        int result_reg = v3_compile_node(&c, &sum);
        chunk_emit(&c, OP_HALT);

        check(run_chunk(&c), "a*a+b*b (a=3,b=4): chunk ran to completion without error");
        AerVal result = v3_register_get(result_reg);
        check(aer_type(result) == TYPE_INTEGER && aer_as_int(result) == 25,
              "a*a+b*b == 25 with a,b read from reserved 'local' registers via OP_V3_LOADK");
        check(aer_as_int(v3_register_get(0)) == 3 && aer_as_int(v3_register_get(1)) == 4,
              "the reserved 'local' registers were never clobbered by the temp-register allocator");

        chunk_free(&c);
    }

    /* Test 3: division by zero still errors correctly through OP_V3_BINARY, matching vm_binary's
       normal error behavior — proves this path isn't silently swallowing errors just because it's
       a new, isolated opcode family. */
    {
        Chunk c;
        chunk_init(&c);
        v3_reg_reset();

        V3Node one  = { .kind = V3_NODE_CONST, .const_value = aer_int(1) };
        V3Node zero = { .kind = V3_NODE_CONST, .const_value = aer_int(0) };
        V3Node div  = { .kind = V3_NODE_BINARY, .bin_op = OP_DIV, .lhs = &one, .rhs = &zero };

        v3_compile_node(&c, &div);
        chunk_emit(&c, OP_HALT);

        check(!run_chunk(&c), "1/0 through OP_V3_BINARY reports a runtime error, same as the stack VM's OP_DIV would");

        chunk_free(&c);
    }

    /* Test 4 (M2): a hand-driven while-loop — sum = 0; i = 0; while i < 5: sum += i; i += 1 —
       proving OP_V3_CMP_JUMP_FALSE (the fused register-operand comparison+branch) and the reused,
       stack-neutral OP_JUMP work correctly together for real control flow, not just straight-line
       arithmetic. sum/i are "locals" in reserved registers 0/1; the loop body writes directly into
       them (a compound-assignment shape, not a fresh temp via v3_compile_node — parser_v3.c's tree
       compiler is for general expressions feeding a *new* result, this is the simpler "assign back
       into an already-live register" shape, same distinction the real parser draws between
       OP_BINARY_* and OP_COMPOUND_*). Expected: sum == 0+1+2+3+4 == 10. */
    {
        Chunk c;
        chunk_init(&c);
        v3_reg_reset();

        unsigned int pool_zero = chunk_add_pool(&c, aer_int(0));
        unsigned int pool_one  = chunk_add_pool(&c, aer_int(1));
        unsigned int pool_five = chunk_add_pool(&c, aer_int(5));

        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 0); chunk_emit(&c, (int)pool_zero);  /* sum = 0 */
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 1); chunk_emit(&c, (int)pool_zero);  /* i = 0 */
        v3_reg_reserve(2);   /* registers 0 (sum), 1 (i) are now "locals" */

        unsigned int loop_start = c.count;
        unsigned int exit_patch = v3_emit_cmp_jump_false(&c, /*rk_i=*/1, OP_LT, /*rk_5=*/(int)pool_five | V3_RK_CONST_FLAG);

        /* sum = sum + i — writes directly into register 0, no new allocation. */
        chunk_emit(&c, OP_V3_BINARY);
        chunk_emit(&c, 0); chunk_emit(&c, 0); chunk_emit(&c, (int)OP_ADD); chunk_emit(&c, 1);

        /* i = i + 1 — writes directly into register 1, no new allocation. */
        chunk_emit(&c, OP_V3_BINARY);
        chunk_emit(&c, 1); chunk_emit(&c, 1); chunk_emit(&c, (int)OP_ADD);
        chunk_emit(&c, (int)pool_one | V3_RK_CONST_FLAG);

        chunk_emit(&c, OP_JUMP); chunk_emit(&c, (int)loop_start);

        v3_patch_jump(&c, exit_patch, c.count);
        chunk_emit(&c, OP_HALT);

        check(run_chunk(&c), "while i<5: sum+=i; i+=1 — chunk ran to completion without error");
        check(aer_as_int(v3_register_get(0)) == 10,
              "sum == 10 after the loop, computed via OP_V3_CMP_JUMP_FALSE + reused OP_JUMP");
        check(aer_as_int(v3_register_get(1)) == 5,
              "i == 5 after the loop — the fused comparison correctly stopped iteration at the boundary");

        chunk_free(&c);
    }

    /* Test 5 (M5: real per-call windowing): a single call — square(x) = x*x, called with x=6,
       expecting 36. Proves OP_V3_CALL's bulk arg-copy into a fresh, isolated callee frame and
       OP_V3_RETURN's write-back into the caller's saved dest_reg work together, end to end.
       Simpler than M3's version: the callee just reads its argument from its own register 0
       directly — no more disjoint-range reservation dance (V3_CALLEE_FRAME_BASE is gone), since
       every call now gets an isolated bank and caller/callee register numbers can't collide.

       Layout, since v3_emit_call needs callee_offset already known (no patch step, unlike a jump):
       an initial OP_JUMP skips over the callee body so it's never reached by fall-through, the
       callee body is compiled first (recording its start offset), then the jump is patched to land
       on the caller code that follows it. Matches M2's "record the offset, patch after" idiom, just
       with the body/patch order flipped since here the *target* must be known before the *jumper*
       (the call site) can be emitted, not the other way around. */
    {
        Chunk c;
        chunk_init(&c);

        chunk_emit(&c, OP_JUMP);
        unsigned int skip_callee_patch = c.count;
        chunk_emit(&c, 0);   /* placeholder — patched below once the caller's start offset is known */

        /* Callee body: x*x, with x read from its own frame's register 0 — no reservation needed. */
        v3_reg_reset();
        v3_reg_reserve(1);   /* register 0 is the callee's "x" argument */

        unsigned int callee_offset = c.count;
        V3Node x1 = { .kind = V3_NODE_REG, .reg = 0 };
        V3Node x2 = { .kind = V3_NODE_REG, .reg = 0 };
        V3Node x_sq = { .kind = V3_NODE_BINARY, .bin_op = OP_MUL, .lhs = &x1, .rhs = &x2 };
        int result_reg = v3_compile_node(&c, &x_sq);
        v3_emit_return(&c, result_reg);

        v3_patch_jump(&c, skip_callee_patch, c.count);   /* caller code starts right here */

        /* Caller: reg 0 = 6 (the argument), call the callee, result lands in reg 1. */
        v3_reg_reset();
        v3_reg_reserve(1);   /* register 0 holds the argument being passed */

        unsigned int pool_six = chunk_add_pool(&c, aer_int(6));
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 0); chunk_emit(&c, (int)pool_six);
        v3_emit_call(&c, /*dest_reg=*/1, callee_offset, /*arg_reg_base=*/0, /*arg_count=*/1);
        chunk_emit(&c, OP_HALT);

        check(run_chunk(&c), "square(6) via OP_V3_CALL into an isolated callee frame: chunk ran to completion without error");
        check(aer_as_int(v3_register_get(1)) == 36,
              "square(6) == 36 — OP_V3_RETURN wrote the callee's result into the caller's saved dest_reg");
        check(aer_as_int(v3_register_get(0)) == 6,
              "the caller's own argument register (0) was untouched by the callee's execution");

        chunk_free(&c);
    }

    /* Test 9 (M5): recursive factorial(5) == 120 — the callee's own body contains an OP_V3_CALL
       targeting its own callee_offset. The real proof this milestone exists for: register 0 ("n")
       must survive across the nested recursive call and still be correctly readable afterward
       (n * factorial(n-1)) — if frame isolation were broken (M3's single shared bank), the nested
       call would have clobbered it. Exercises 5 genuinely distinct, simultaneously-live frames. */
    {
        Chunk c;
        chunk_init(&c);

        chunk_emit(&c, OP_JUMP);
        unsigned int skip_callee_patch = c.count;
        chunk_emit(&c, 0);

        unsigned int pool_1 = chunk_add_pool(&c, aer_int(1));

        v3_reg_reset();
        v3_reg_reserve(1);   /* register 0: "n", the callee's argument */

        unsigned int callee_offset = c.count;

        /* if !(n > 1) goto base_case: return 1 */
        unsigned int base_case_patch = v3_emit_cmp_jump_false(&c, /*rk_n=*/0, OP_GT,
                                                                /*rk_1=*/(int)pool_1 | V3_RK_CONST_FLAG);

        /* Recursive case (n > 1): reg1 = n - 1; reg2 = factorial(reg1); reg3 = n * reg2; return reg3 */
        chunk_emit(&c, OP_V3_BINARY);
        chunk_emit(&c, 1); chunk_emit(&c, 0); chunk_emit(&c, (int)OP_SUB);
        chunk_emit(&c, (int)pool_1 | V3_RK_CONST_FLAG);

        v3_emit_call(&c, /*dest_reg=*/2, callee_offset, /*arg_reg_base=*/1, /*arg_count=*/1);

        chunk_emit(&c, OP_V3_BINARY);
        chunk_emit(&c, 3); chunk_emit(&c, 0); chunk_emit(&c, (int)OP_MUL); chunk_emit(&c, 2);
        v3_emit_return(&c, 3);

        /* Base case (n <= 1): return 1. OP_V3_RETURN above jumps away, so this is only ever
           reached via the patched branch, never by fall-through. */
        v3_patch_jump(&c, base_case_patch, c.count);
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 4); chunk_emit(&c, (int)pool_1);
        v3_emit_return(&c, 4);

        v3_patch_jump(&c, skip_callee_patch, c.count);   /* caller code starts right here */

        /* Caller: reg 0 = 5, call factorial, result lands in reg 1. */
        v3_reg_reset();
        v3_reg_reserve(1);

        unsigned int pool_5 = chunk_add_pool(&c, aer_int(5));
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 0); chunk_emit(&c, (int)pool_5);
        v3_emit_call(&c, /*dest_reg=*/1, callee_offset, /*arg_reg_base=*/0, /*arg_count=*/1);
        chunk_emit(&c, OP_HALT);

        check(run_chunk(&c), "recursive factorial(5) via OP_V3_CALL targeting its own callee_offset: chunk ran to completion without error");
        check(aer_as_int(v3_register_get(1)) == 120,
              "factorial(5) == 120 — 5 nested, simultaneously-live frames each correctly isolated");

        chunk_free(&c);
    }

    /* Test 10 (M5): unconditional self-recursion (no base case) must hit the v3 call stack
       overflow guard cleanly — run_chunk returns false, runtime_had_error is set, no crash or
       stack corruption. Proves the overflow guard itself, not just the recursion happy path. */
    {
        Chunk c;
        chunk_init(&c);

        chunk_emit(&c, OP_JUMP);
        unsigned int skip_callee_patch = c.count;
        chunk_emit(&c, 0);

        v3_reg_reset();
        v3_reg_reserve(1);

        unsigned int callee_offset = c.count;
        v3_emit_call(&c, /*dest_reg=*/1, callee_offset, /*arg_reg_base=*/0, /*arg_count=*/1);
        v3_emit_return(&c, 1);   /* never reached — the call above never returns before overflowing */

        v3_patch_jump(&c, skip_callee_patch, c.count);

        v3_reg_reset();
        v3_reg_reserve(1);
        unsigned int pool_zero = chunk_add_pool(&c, aer_int(0));
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 0); chunk_emit(&c, (int)pool_zero);
        v3_emit_call(&c, /*dest_reg=*/1, callee_offset, /*arg_reg_base=*/0, /*arg_count=*/1);
        chunk_emit(&c, OP_HALT);

        check(!run_chunk(&c),
              "unconditional self-recursion hits the v3 call stack overflow guard and reports a clean runtime error, not a crash");

        chunk_free(&c);
    }

    /* Test 6 (M4): a register-resident array survives real GC pressure it did not itself cause.
       Builds one array into reg 3 from regs 0-2, then runs a 200-iteration while-loop that rebuilds
       a second, throwaway array into reg 5 every pass (from the same source regs, discarding the
       previous iteration's array each time) — real allocation churn, forced to actually trigger
       minor collections via a tiny aer_gc_configure threshold, exactly mirroring
       embed_smoke_test.c's stack-VM GC-pressure test but through registers instead of a scope
       local. Before the v3_registers[] scan was added to mark_vm_roots (see this milestone's plan),
       reg 3's array had no root at all during those collections — this test is the one that would
       have caught that, not just documentation of the fix. */
    {
        Chunk c;
        chunk_init(&c);
        v3_reg_reset();

        unsigned int pool_10   = chunk_add_pool(&c, aer_int(10));
        unsigned int pool_20   = chunk_add_pool(&c, aer_int(20));
        unsigned int pool_30   = chunk_add_pool(&c, aer_int(30));
        unsigned int pool_zero = chunk_add_pool(&c, aer_int(0));
        unsigned int pool_one  = chunk_add_pool(&c, aer_int(1));
        unsigned int pool_200  = chunk_add_pool(&c, aer_int(200));
        unsigned int pool_99   = chunk_add_pool(&c, aer_int(99));

        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 0); chunk_emit(&c, (int)pool_10);
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 1); chunk_emit(&c, (int)pool_20);
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 2); chunk_emit(&c, (int)pool_30);
        v3_reg_reserve(3);   /* regs 0-2: source items, read by both arrays below */

        v3_emit_array_new(&c, /*dest=*/3, /*item_reg_base=*/0, /*item_count=*/3);
        v3_reg_reserve(1);   /* reg 3: the array whose survival across GC pressure this test proves */

        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 4); chunk_emit(&c, (int)pool_zero);  /* i = 0 */
        v3_reg_reserve(1);   /* reg 4: loop counter */

        unsigned int loop_start = c.count;
        unsigned int exit_patch = v3_emit_cmp_jump_false(&c, /*rk_i=*/4, OP_LT,
                                                           /*rk_200=*/(int)pool_200 | V3_RK_CONST_FLAG);

        /* Throwaway array, rebuilt fresh into reg 5 every iteration — each pass's array_pool cell +
           xmalloc'd items buffer becomes garbage the instant the next iteration overwrites reg 5,
           generating the allocation pressure needed to force real minor collections while reg 3's
           array is never touched. */
        v3_emit_array_new(&c, /*dest=*/5, /*item_reg_base=*/0, /*item_count=*/3);

        /* i = i + 1 */
        chunk_emit(&c, OP_V3_BINARY);
        chunk_emit(&c, 4); chunk_emit(&c, 4); chunk_emit(&c, (int)OP_ADD);
        chunk_emit(&c, (int)pool_one | V3_RK_CONST_FLAG);

        chunk_emit(&c, OP_JUMP); chunk_emit(&c, (int)loop_start);
        v3_patch_jump(&c, exit_patch, c.count);

        /* After 200 throwaway allocations, read reg 3's surviving array back out, then mutate and
           re-read it, to prove it's still the same, correct, uncorrupted array — not a
           dangling/reused cell a collection swept out from under a register. */
        v3_emit_index_get(&c, /*dest=*/6, /*arr_reg=*/3, /*rk_idx=*/(int)pool_one | V3_RK_CONST_FLAG);
        v3_emit_index_set(&c, /*arr_reg=*/3, /*rk_idx=*/(int)pool_zero | V3_RK_CONST_FLAG,
                           /*rk_val=*/(int)pool_99 | V3_RK_CONST_FLAG);
        v3_emit_index_get(&c, /*dest=*/7, /*arr_reg=*/3, /*rk_idx=*/(int)pool_zero | V3_RK_CONST_FLAG);

        chunk_emit(&c, OP_HALT);

        /* Tiny minor threshold so 200 iterations' worth of allocation actually triggers several
           real collections, not zero — same technique embed_smoke_test.c uses for its own GC test. */
        aer_gc_configure(20, 0);
        unsigned int minors_before;
        aer_gc_stats(NULL, &minors_before, NULL);

        check(run_chunk(&c),
              "200-iteration array-allocation loop with one register-resident survivor ran to completion without error");

        unsigned int minors_after;
        aer_gc_stats(NULL, &minors_after, NULL);
        aer_gc_configure(2048, 10);   /* restore defaults */

        check(minors_after > minors_before,
              "the throwaway-array loop actually triggered real minor collections, not just ran without crashing");
        check(aer_as_int(v3_register_get(4)) == 200,
              "loop counter reached 200 — the while-loop itself ran to completion");
        check(aer_as_int(v3_register_get(6)) == 20,
              "reg 3's array read back correctly (index 1 == 20) after surviving GC pressure — proves "
              "mark_vm_roots's new v3_registers[] scan keeps a register-only array reference alive");
        check(aer_as_int(v3_register_get(7)) == 99,
              "OP_V3_INDEX_SET's mutation of reg 3's array is visible on the very same surviving array, not a stale/reallocated one");

        chunk_free(&c);
    }

    /* Test 7 (M4 follow-up): {"a": 10, "b": 20} built via OP_V3_DICT_NEW from register pairs,
       then read/mutated via the SAME OP_V3_INDEX_GET/OP_V3_INDEX_SET opcodes Test 6 used for
       arrays — proving they really are type-generic (vm_index_get_compute/vm_index_set_compute
       already dispatch on TYPE_DICT) rather than needing any dict-specific get/set opcode. Index
       keys are passed as RK constants (a pool-interned string), the same RK convention an integer
       array index already uses. */
    {
        Chunk c;
        chunk_init(&c);
        v3_reg_reset();

        char* key_a = xmalloc(2); memcpy(key_a, "a", 1); key_a[1] = '\0';
        char* key_b = xmalloc(2); memcpy(key_b, "b", 1); key_b[1] = '\0';
        unsigned int pool_key_a = chunk_add_pool(&c, aer_make_string(key_a, 1));
        unsigned int pool_key_b = chunk_add_pool(&c, aer_make_string(key_b, 1));
        unsigned int pool_10    = chunk_add_pool(&c, aer_int(10));
        unsigned int pool_20    = chunk_add_pool(&c, aer_int(20));
        unsigned int pool_99    = chunk_add_pool(&c, aer_int(99));

        /* reg0/reg1 = key "a", val 10; reg2/reg3 = key "b", val 20 */
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 0); chunk_emit(&c, (int)pool_key_a);
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 1); chunk_emit(&c, (int)pool_10);
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 2); chunk_emit(&c, (int)pool_key_b);
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 3); chunk_emit(&c, (int)pool_20);
        v3_reg_reserve(4);

        v3_emit_dict_new(&c, /*dest=*/4, /*pair_reg_base=*/0, /*pair_count=*/2);

        v3_emit_index_get(&c, /*dest=*/5, /*arr_reg=*/4, /*rk_idx=*/(int)pool_key_b | V3_RK_CONST_FLAG);
        v3_emit_index_set(&c, /*arr_reg=*/4, /*rk_idx=*/(int)pool_key_a | V3_RK_CONST_FLAG,
                           /*rk_val=*/(int)pool_99 | V3_RK_CONST_FLAG);
        v3_emit_index_get(&c, /*dest=*/6, /*arr_reg=*/4, /*rk_idx=*/(int)pool_key_a | V3_RK_CONST_FLAG);

        chunk_emit(&c, OP_HALT);

        check(run_chunk(&c),
              "{\"a\":10,\"b\":20} via OP_V3_DICT_NEW, then OP_V3_INDEX_GET/SET: chunk ran to completion without error");
        check(aer_as_int(v3_register_get(5)) == 20,
              "dict[\"b\"] == 20 read back via the same OP_V3_INDEX_GET used for arrays in Test 6");
        check(aer_as_int(v3_register_get(6)) == 99,
              "dict[\"a\"] == 99 after OP_V3_INDEX_SET — index get/set are genuinely type-generic, not array-only");

        chunk_free(&c);
    }

    /* Test 8 (M4 — iteration slice, arrays only): `for x in [10,20,30]: sum += x` by hand —
       proves OP_V3_ITER_NEXT_ARRAY's bounds-check/fetch/advance and its being its own back-edge
       target (same convention parse_for uses for the stack VM), plus confirms the loop variable's
       register (reg 6, the "item" dest) keeps its last-bound value after the loop exits, matching
       the stack VM's own for-loop variable semantics. */
    {
        Chunk c;
        chunk_init(&c);
        v3_reg_reset();

        unsigned int pool_10   = chunk_add_pool(&c, aer_int(10));
        unsigned int pool_20   = chunk_add_pool(&c, aer_int(20));
        unsigned int pool_30   = chunk_add_pool(&c, aer_int(30));
        unsigned int pool_zero = chunk_add_pool(&c, aer_int(0));

        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 0); chunk_emit(&c, (int)pool_10);
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 1); chunk_emit(&c, (int)pool_20);
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 2); chunk_emit(&c, (int)pool_30);
        v3_reg_reserve(3);   /* regs 0-2: the array's source items */

        v3_emit_array_new(&c, /*dest=*/3, /*item_reg_base=*/0, /*item_count=*/3);
        v3_reg_reserve(1);   /* reg 3: the collection being iterated */

        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 4); chunk_emit(&c, (int)pool_zero);  /* idx = 0 */
        chunk_emit(&c, OP_V3_LOADK); chunk_emit(&c, 5); chunk_emit(&c, (int)pool_zero);  /* sum = 0 */
        v3_reg_reserve(2);   /* regs 4-5: iterator index, running sum */

        unsigned int loop_start = c.count;   /* the iterate opcode is its own back-edge target */
        unsigned int exit_patch = v3_emit_iter_next_array(&c, /*col_reg=*/3, /*idx_reg=*/4,
                                                            /*item_dest_reg=*/6);

        /* sum = sum + item */
        chunk_emit(&c, OP_V3_BINARY);
        chunk_emit(&c, 5); chunk_emit(&c, 5); chunk_emit(&c, (int)OP_ADD); chunk_emit(&c, 6);

        chunk_emit(&c, OP_JUMP); chunk_emit(&c, (int)loop_start);
        v3_patch_jump(&c, exit_patch, c.count);
        chunk_emit(&c, OP_HALT);

        check(run_chunk(&c), "for x in [10,20,30]: sum += x — chunk ran to completion without error");
        check(aer_as_int(v3_register_get(5)) == 60,
              "sum == 60 after the loop, computed via OP_V3_ITER_NEXT_ARRAY + reused OP_JUMP");
        check(aer_as_int(v3_register_get(4)) == 3,
              "idx == 3 — the loop consumed every element and stopped exactly at the array's length");
        check(aer_as_int(v3_register_get(6)) == 30,
              "the loop variable's register (reg 6) retains its last-bound value (30) after the loop exits, matching the stack VM's for-loop semantics");

        chunk_free(&c);
    }

    /* Test 11 (M5 slice 2): real .aer source, for the first time, drives v3 codegen —
       `x = 2 + 3; y = x * 4`. First name assigned is `x` (register 0), second is `y` (register 1). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 2 + 3\ny = x * 4\n");

        check(ok, "real source 'x = 2 + 3; y = x * 4' compiled and ran via v3_parse without error");
        check(aer_as_int(v3_register_get(0)) == 5,  "x == 5 (register 0, first name assigned)");
        check(aer_as_int(v3_register_get(1)) == 20, "y == 20 (register 1, second name assigned) — reads x back out of its own permanent register");

        chunk_free(&c);
    }

    /* Test 12 (M5 slice 2): real source if/else, run for both branches. `x` is register 0, `y`
       register 1 in both scripts (same encounter order each time). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 10\nif x > 5:\n    y = 1\nelse:\n    y = 0\n");

        check(ok, "real source if/else (true branch) compiled and ran via v3_parse without error");
        check(aer_as_int(v3_register_get(1)) == 1, "y == 1 — the true branch ran (x=10 > 5)");

        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 3\nif x > 5:\n    y = 1\nelse:\n    y = 0\n");

        check(ok, "real source if/else (false branch) compiled and ran via v3_parse without error");
        check(aer_as_int(v3_register_get(1)) == 0, "y == 0 — the else branch ran (x=3 is not > 5)");

        chunk_free(&c);
    }

    /* Test 13 (M5 slice 2): real source `for <condition>:` (AER's while-form — there is no
       separate `while` keyword) — directly mirrors Test 4's hand-driven while loop, now compiled
       from real syntax. `sum` is register 0, `i` is register 1 (encounter order). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "sum = 0\ni = 0\nfor i < 5:\n    sum = sum + i\n    i = i + 1\n");

        check(ok, "real source 'for i < 5: sum = sum + i; i = i + 1' compiled and ran via v3_parse without error");
        check(aer_as_int(v3_register_get(0)) == 10, "sum == 10 after the real-source for-while loop");
        check(aer_as_int(v3_register_get(1)) == 5,  "i == 5 — the loop consumed exactly 5 iterations, same boundary as Test 4's hand-driven version");

        chunk_free(&c);
    }

    /* Test 14 (M5 slice 3): real source function definition + call — `square` is a FUNCTION name
       (its own separate table, not a variable), so `y` is the first and only variable, register 0. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "function square(x):\n    return x * x\ny = square(6)\n");

        check(ok, "real source function definition + call ('function square(x): return x*x' then 'y = square(6)') ran without error");
        check(aer_as_int(v3_register_get(0)) == 36, "y == 36 — the real-source call correctly returned the function's result");

        chunk_free(&c);
    }

    /* Test 15 (M5 slice 3): real source recursion — the real-source equivalent of Test 9's
       hand-driven factorial. Registering the function name BEFORE compiling its own body (see
       v3_parse_function) is what lets the self-call inside resolve. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "function factorial(n):\n"
            "    if n <= 1:\n"
            "        return 1\n"
            "    return n * factorial(n - 1)\n"
            "y = factorial(5)\n");

        check(ok, "real source recursive factorial(5) ran without error");
        check(aer_as_int(v3_register_get(0)) == 120, "y == 120 — real-source recursion works end to end, same as Test 9's hand-driven version");

        chunk_free(&c);
    }

    /* Test 16 (M5 slice 3): a bare call statement (result discarded) compiles and runs cleanly —
       proves v3_parse_statement's identifier/'(' dispatch works outside an assignment RHS too. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "function square(x):\n    return x * x\nsquare(4)\n");

        check(ok, "a bare call statement ('square(4)' alone, result discarded) compiled and ran without error");

        chunk_free(&c);
    }

    /* Test 17 (M5 slice 3): calling an undefined function fails cleanly (a parse error, caught by
       v3_run_source's parse_had_error check) — proves the stated 'no forward references/mutual
       recursion' limitation fails safely rather than compiling something broken. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = undefined_function(5)\n");

        check(!ok, "calling an undefined (or not-yet-defined) function reports a clean parse error, not a crash");

        chunk_free(&c);
    }

    /* Test 18 (M5 slice 4): real source array literal + read indexing. `arr` is register 0, `y`
       register 1 (encounter order). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "arr = [10, 20, 30]\ny = arr[1]\n");

        check(ok, "real source array literal + indexing ('arr = [10,20,30]; y = arr[1]') ran without error");
        check(aer_as_int(v3_register_get(1)) == 20, "y == 20 — arr[1] read back correctly through OP_V3_INDEX_GET");

        chunk_free(&c);
    }

    /* Test 19 (M5 slice 4): real source dict literal (string keys, via the new plain-string-literal
       support) + read indexing. `d` is register 0, `y` register 1. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "d = {\"a\": 1, \"b\": 2}\ny = d[\"b\"]\n");

        check(ok, "real source dict literal + indexing ('d = {\"a\":1,\"b\":2}; y = d[\"b\"]') ran without error");
        check(aer_as_int(v3_register_get(1)) == 2, "y == 2 — d[\"b\"] read back correctly through OP_V3_INDEX_GET");

        chunk_free(&c);
    }

    /* Test 20 (M5 slice 4): real source indexed write ('arr[i] = v'), and a chained read
       ('matrix[i][j]') proving v3_parse_primary's postfix '[...]' loop handles nested arrays. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "arr = [1, 2, 3]\narr[1] = 99\ny = arr[1]\n");

        check(ok, "real source indexed write ('arr[1] = 99') ran without error");
        check(aer_as_int(v3_register_get(1)) == 99, "y == 99 — the write through OP_V3_INDEX_SET is visible on the very same array");

        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "matrix = [[1, 2], [3, 4]]\ny = matrix[1][0]\n");

        check(ok, "real source chained indexing ('matrix[1][0]' on a real-source array-of-arrays) ran without error");
        check(aer_as_int(v3_register_get(1)) == 3, "y == 3 — matrix[1][0] resolved correctly through v3_parse_primary's postfix '[...]' chain");

        chunk_free(&c);
    }

    /* Test 21 (M5 slice 5): real source `for x in [...]:` array iteration. `sum` is assigned
       first (register 0), `x` (the loop variable) is bound second, once v3_parse_for_in reaches
       it (register 1) — reserved BEFORE the collection/index temps are computed, per
       v3_parse_for_in's own comment on why that ordering is required here. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "sum = 0\nfor x in [10, 20, 30]:\n    sum = sum + x\n");

        check(ok, "real source 'for x in [10,20,30]: sum = sum + x' ran without error");
        check(aer_as_int(v3_register_get(0)) == 60, "sum == 60 after the real-source for-in loop over a real-source array literal");

        chunk_free(&c);
    }

    /* Test 22 (M5 slice 6): real source struct definition + instantiation + field reads.
       `p` is register 0 (first variable), `total` is register 1. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "struct Point:\n"
            "    x\n"
            "    y\n"
            "p = Point(3, 4)\n"
            "total = p.x + p.y\n");

        check(ok, "real source struct def + instantiation + field reads ran without error");
        check(aer_as_int(v3_register_get(1)) == 7, "total == 7 — p.x + p.y read back correctly through OP_V3_FIELD_GET");

        chunk_free(&c);
    }

    /* Test 23 (M5 slice 6): field write ('p.x = 99'), read back through the same struct instance. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "struct Point:\n"
            "    x\n"
            "    y\n"
            "p = Point(1, 2)\n"
            "p.x = 99\n"
            "total = p.x\n");

        check(ok, "real source field write ('p.x = 99') ran without error");
        check(aer_as_int(v3_register_get(1)) == 99, "total == 99 — the write through OP_V3_FIELD_SET is visible on the very same struct instance");

        chunk_free(&c);
    }

    /* Test 24 (M5 slice 6): a struct field's default fills in when the constructor omits a
       trailing argument (mirrors the stack VM's own vm_default_value fallback). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "struct Pair:\n"
            "    a\n"
            "    b = 10\n"
            "q = Pair(5)\n"
            "total = q.b\n");

        check(ok, "real source struct instantiation with an omitted trailing (defaulted) field ran without error");
        check(aer_as_int(v3_register_get(1)) == 10, "total == 10 — q.b correctly took its declared default (5 was only supplied for 'a')");

        chunk_free(&c);
    }

    /* Test 25 (M5 slice 6): instantiating with too many arguments compiles fine (arity isn't known
       until OP_V3_STRUCT_NEW actually runs) but fails cleanly at runtime — proves the arity check
       mirrored from lbl_call's own struct-instantiation fallback actually fires. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "struct Pair:\n"
            "    a\n"
            "    b = 10\n"
            "q = Pair(1, 2, 3)\n");

        check(!ok, "instantiating a struct with more arguments than fields reports a clean runtime error, not a crash");

        chunk_free(&c);
    }

    /* Test 26 (M5 slice 7): unary operators — negate, not, bitwise-not, and a chained double
       negation. `y` is always register 1 (second variable, after `x`). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 5\ny = -x\n");
        check(ok, "real source unary negate ('y = -x') ran without error");
        check(aer_as_int(v3_register_get(1)) == -5, "y == -5");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = true\ny = !x\n");
        check(ok, "real source unary not ('y = !x') ran without error");
        check(aer_as_bool(v3_register_get(1)) == false, "y == false");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 5\ny = ~x\n");
        check(ok, "real source unary bitwise-not ('y = ~x') ran without error");
        check(aer_as_int(v3_register_get(1)) == -6, "y == -6 (~5 in two's complement)");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 5\ny = --x\n");
        check(ok, "real source chained unary ('y = --x', double negation) ran without error");
        check(aer_as_int(v3_register_get(1)) == 5, "y == 5 — double negation cancels, proving v3_parse_unary's self-recursion");
        chunk_free(&c);
    }

    /* Test 27 (M5 slice 7): and/or short-circuit — all four truth-table corners. AER spells these
       `&&`/`||` (lexer.h's TOKEN_AND/TOKEN_OR comments), not the words "and"/"or" — `y` is
       register 0 (the only variable in each script). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = true && true\n");
        check(ok, "'y = true && true' ran without error");
        check(aer_as_bool(v3_register_get(0)) == true, "y == true");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = true && false\n");
        check(ok, "'y = true && false' ran without error");
        check(aer_as_bool(v3_register_get(0)) == false, "y == false");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = false || true\n");
        check(ok, "'y = false || true' ran without error");
        check(aer_as_bool(v3_register_get(0)) == true, "y == true");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = false || false\n");
        check(ok, "'y = false || false' ran without error");
        check(aer_as_bool(v3_register_get(0)) == false, "y == false");
        chunk_free(&c);
    }

    /* Test 28 (M5 slice 7): compound assignment — arithmetic chain, one bitwise case, and a
       compile-time error for compound-assigning an undefined name. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 5\nx += 3\nx *= 2\n");
        check(ok, "real source compound assignment ('x = 5; x += 3; x *= 2') ran without error");
        check(aer_as_int(v3_register_get(0)) == 16, "x == 16 — (5 + 3) * 2");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 6\nx &= 3\n");
        check(ok, "real source bitwise compound assignment ('x = 6; x &= 3') ran without error");
        check(aer_as_int(v3_register_get(0)) == 2, "x == 2 (6 & 3)");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x += 3\n");
        check(!ok, "compound-assigning an undefined name reports a clean parse error, not a crash");
        chunk_free(&c);
    }

    /* Test 29 (M5 slice 7): bitwise/shift as plain (non-assignment) expressions. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = 6 & 3\n");
        check(ok, "real source bitwise AND expression ('y = 6 & 3') ran without error");
        check(aer_as_int(v3_register_get(0)) == 2, "y == 2");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = 1 << 4\n");
        check(ok, "real source left-shift expression ('y = 1 << 4') ran without error");
        check(aer_as_int(v3_register_get(0)) == 16, "y == 16");
        chunk_free(&c);
    }

    /* Test 30 (M5 slice 8): `break` inside a real-source for-while loop. `sum`/`i` are registers
       0/1 (encounter order). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "sum = 0\n"
            "i = 0\n"
            "for i < 10:\n"
            "    if i == 5:\n"
            "        break\n"
            "    sum = sum + i\n"
            "    i = i + 1\n");
        check(ok, "real source 'break' inside a for-while loop ran without error");
        check(aer_as_int(v3_register_get(0)) == 10, "sum == 10 — 0+1+2+3+4, break fired exactly at i==5");
        check(aer_as_int(v3_register_get(1)) == 5, "i == 5 — the loop exited via break, not the condition");
        chunk_free(&c);
    }

    /* Test 31 (M5 slice 8): `continue` inside a real-source for-while loop. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "sum = 0\n"
            "i = 0\n"
            "for i < 5:\n"
            "    i = i + 1\n"
            "    if i == 3:\n"
            "        continue\n"
            "    sum = sum + i\n");
        check(ok, "real source 'continue' inside a for-while loop ran without error");
        check(aer_as_int(v3_register_get(0)) == 12, "sum == 12 — 1+2+4+5, i==3's iteration skipped sum += i via continue");
        chunk_free(&c);
    }

    /* Test 32 (M5 slice 8): `break` inside a real-source for-in loop over an array. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "sum = 0\n"
            "for x in [1, 2, 3, 4, 5]:\n"
            "    if x == 4:\n"
            "        break\n"
            "    sum = sum + x\n");
        check(ok, "real source 'break' inside a for-in loop ran without error");
        check(aer_as_int(v3_register_get(0)) == 6, "sum == 6 — 1+2+3, break fired at x==4 before it was added");
        chunk_free(&c);
    }

    /* Test 33 (M5 slice 8): `break`/`continue` outside any loop are clean compile-time errors. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "break\n");
        check(!ok, "'break' outside a loop reports a clean parse error, not a crash");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "continue\n");
        check(!ok, "'continue' outside a loop reports a clean parse error, not a crash");
        chunk_free(&c);
    }

    /* Test 34 (M5 slice 9): plain string literal through the new v3_parse_string_literal path
       (no `{...}` — must still behave exactly like the old simple path did). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = \"hello\"\n");
        check(ok, "real source plain string literal ('y = \"hello\"') ran without error");
        check(v3_string_eq(v3_register_get(0), "hello"), "y == \"hello\"");
        chunk_free(&c);
    }

    /* Test 35 (M5 slice 9): single interpolation. `x` is register 0, `y` register 1. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 5\ny = \"value is {x}\"\n");
        check(ok, "real source string interpolation ('y = \"value is {x}\"') ran without error");
        check(v3_string_eq(v3_register_get(1), "value is 5"), "y == \"value is 5\"");
        chunk_free(&c);
    }

    /* Test 36 (M5 slice 9): multiple interpolations in one literal. `a`/`b`/`y` are registers 0/1/2. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "a = 1\nb = 2\ny = \"{a} and {b}\"\n");
        check(ok, "real source multi-interpolation ('y = \"{a} and {b}\"') ran without error");
        check(v3_string_eq(v3_register_get(2), "1 and 2"), "y == \"1 and 2\"");
        chunk_free(&c);
    }

    /* Test 37 (M5 slice 9): interpolating an undefined name is a clean compile-time error. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = \"{undefined_name}\"\n");
        check(!ok, "interpolating an undefined name reports a clean parse error, not a crash");
        chunk_free(&c);
    }

    /* Test 38 (M5 slice 8): 'in' — falls straight through to the generic OP_V3_BINARY/vm_binary
       path already used by every other binary operator, so this is really a table-entry test. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "arr = [1, 2, 3]\ny = 2 in arr\nz = 9 in arr\n");
        check(ok, "real source 'in' ('2 in arr') ran without error");
        check(aer_as_bool(v3_register_get(1)) == true,  "y == true — 2 is in arr");
        check(aer_as_bool(v3_register_get(2)) == false, "z == false — 9 is not in arr");
        chunk_free(&c);
    }

    /* Test 39 (M5 slice 8): 'as' casting — string (via OP_V3_UNARY's OP_TO_STR case) and the
       three OP_V3_CAST primitives. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 5\ny = x as string\n");
        check(ok, "real source 'x as string' ran without error");
        check(v3_string_eq(v3_register_get(1), "5"), "y == \"5\"");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = \"42\"\ny = x as integer\n");
        check(ok, "real source 'x as integer' ran without error");
        check(aer_as_int(v3_register_get(1)) == 42, "y == 42");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 3\ny = x as float\n");
        check(ok, "real source 'x as float' ran without error");
        check(aer_as_real(v3_register_get(1)) == 3.0, "y == 3.0");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 1\ny = x as boolean\n");
        check(ok, "real source 'x as boolean' ran without error");
        check(aer_as_bool(v3_register_get(1)) == true, "y == true");
        chunk_free(&c);
    }

    /* Test 40 (M5 slice 8): casting to an unknown/struct type name is a clean compile-time error
       in v3 (no OP_CHECK_SHAPE equivalent exists yet — see OP_V3_CAST's comment in vm.h). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 5\ny = x as SomeStruct\n");
        check(!ok, "'as' to an unknown type name reports a clean parse error, not a crash");
        chunk_free(&c);
    }

    /* Test 41 (M5 slice 10): pipe operator, no extra args — 'x |> f()' desugars to 'f(x)'. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "function square(x):\n    return x * x\ny = 6 |> square()\n");
        check(ok, "real source 'y = 6 |> square()' ran without error");
        check(aer_as_int(v3_register_get(0)) == 36, "y == 36 — piped value became square()'s only argument");
        chunk_free(&c);
    }

    /* Test 42 (M5 slice 10): pipe operator with extra args — 'x |> f(a)' desugars to 'f(x, a)'. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "function add(a, b):\n    return a + b\ny = 3 |> add(4)\n");
        check(ok, "real source 'y = 3 |> add(4)' ran without error");
        check(aer_as_int(v3_register_get(0)) == 7, "y == 7 — piped value became add()'s first argument, 4 the second");
        chunk_free(&c);
    }

    /* Test 43 (M5 slice 10): a module-qualified pipe target is a clean compile-time error in v3
       (no module system yet), not a crash or silent misparse. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "x = 5\ny = x |> string.upper()\n");
        check(!ok, "module-qualified pipe target reports a clean parse error, not a crash");
        chunk_free(&c);
    }

    /* Test 44 (M5 slice 11): 'import' + module-qualified call, statement position. `y` is
       register 0 — math.sqrt(16.0) returns 4.0 through OP_V3_CALL_MODULE's stack bridge. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "import math\ny = math.sqrt(16.0)\n");
        check(ok, "real source 'import math' + 'y = math.sqrt(16.0)' ran without error");
        check(aer_as_real(v3_register_get(0)) == 4.0, "y == 4.0");
        chunk_free(&c);
    }

    /* Test 45 (M5 slice 11): module call with two arguments, and as a bare statement (no
       assignment — result discarded, same as a plain function call statement). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "import math\ny = math.pow(2, 10)\nmath.floor(3.7)\n");
        check(ok, "real source 'math.pow(2, 10)' + a bare 'math.floor(3.7)' statement ran without error");
        check(aer_as_real(v3_register_get(0)) == 1024.0, "y == 1024.0");
        chunk_free(&c);
    }

    /* Test 46 (M5 slice 11): calling a module function without importing it first is a clean
       parse error (the module name is never registered via chunk_add_import, so it falls through
       to plain-identifier resolution and fails as an unknown function). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = math.sqrt(16.0)\n");
        check(!ok, "calling a module function without importing it first reports a clean parse error, not a crash");
        chunk_free(&c);
    }

    /* Test 47 (M5 slice 12): 'defer' runs LIFO. `log` is a shared array passed into run() by
       reference — each deferred record() overwrites log[0], so the FINAL value (read back via
       `result = log[0]` after run() returns) reveals which one ran LAST. If defers ran in the
       order they were written (FIFO), result would be 3; LIFO gives 1. Top-level registers:
       log=0, result=1 (run(log)'s own discarded result lands in a temp, not a named variable). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "function record(log, val):\n    log[0] = val\n"
            "function run(log):\n    defer record(log, 1)\n    defer record(log, 2)\n    defer record(log, 3)\n    return 0\n"
            "log = [0]\nrun(log)\nresult = log[0]\n");
        check(ok, "real source 'defer' x3 inside run(log) ran without error");
        check(aer_as_int(v3_register_get(1)) == 1, "result == 1 — defers drained LIFO (3, then 2, then 1 ran last)");
        chunk_free(&c);
    }

    /* Test 48 (M5 slice 12): a deferred call's arguments are snapshotted at the defer statement,
       not re-evaluated at replay time — x is reassigned to 99 AFTER the defer statement, but the
       deferred record() call still sees x's value at the moment it was deferred (5). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "function record(log, val):\n    log[0] = val\n"
            "function run(log, x):\n    defer record(log, x)\n    x = 99\n    return 0\n"
            "log = [0]\nrun(log, 5)\nresult = log[0]\n");
        check(ok, "real source 'defer record(log, x)' then reassigning x ran without error");
        check(aer_as_int(v3_register_get(1)) == 5, "result == 5 — the deferred call saw x's value at the defer statement, not its later reassignment");
        chunk_free(&c);
    }

    /* Test 49 (M5 slice 12): defer runs before an explicit return, and the real return value is
       preserved — y must be 42 (run()'s actual return value), independent of what the deferred
       call does to the shared log array. Top-level registers: log=0, y=1, result=2. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "function record(log, val):\n    log[0] = val\n"
            "function run(log):\n    defer record(log, 1)\n    return 42\n"
            "log = [0]\ny = run(log)\nresult = log[0]\n");
        check(ok, "real source 'defer' then 'return 42' ran without error");
        check(aer_as_int(v3_register_get(1)) == 42, "y == 42 — the real return value survives defer draining");
        check(aer_as_int(v3_register_get(2)) == 1, "result == 1 — the deferred call still ran before run() actually returned");
        chunk_free(&c);
    }

    /* Test 50 (M5 slice 12): 'defer' outside a function, and deferring an unknown function, are
       both clean compile-time errors, not crashes. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "defer foo()\n");
        check(!ok, "'defer' outside a function reports a clean parse error, not a crash");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "function run():\n    defer undefined_fn()\n");
        check(!ok, "deferring an undefined function reports a clean parse error, not a crash");
        chunk_free(&c);
    }

    /* Test 51 (feature completeness): 'for i in 0..5:' — range for-loop over a dynamic bound (n),
       proving OP_V3_ITER_RANGE's cur_reg is a genuinely fresh register: if it aliased `n`'s own
       register, `n` would be silently mutated to 5 by the loop. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "n = 5\nsum = 0\nfor i in 0..n:\n    sum = sum + i\n");
        check(ok, "real source 'for i in 0..n: sum += i' ran without error");
        check(aer_as_int(v3_register_get(1)) == 10, "sum == 10 — 0+1+2+3+4");
        check(aer_as_int(v3_register_get(0)) == 5, "n == 5 — unchanged by the loop (cur_reg didn't alias n's register)");
        chunk_free(&c);
    }

    /* Test 52 (feature completeness): 'a..b..step' explicit step, and a descending range (start >
       end infers descending direction, not the step's sign). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "sum = 0\nfor i in 0..10..2:\n    sum = sum + i\n");
        check(ok, "real source 'for i in 0..10..2:' ran without error");
        check(aer_as_int(v3_register_get(0)) == 20, "sum == 20 — 0+2+4+6+8");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "sum = 0\nfor i in 5..0:\n    sum = sum + i\n");
        check(ok, "real source 'for i in 5..0:' (descending) ran without error");
        check(aer_as_int(v3_register_get(0)) == 15, "sum == 15 — 5+4+3+2+1, direction inferred from bounds");
        chunk_free(&c);
    }

    /* Test 53 (feature completeness): break/continue still work inside a range for-loop, same as
       they already do for array for-in (Tests 30-32). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "sum = 0\nfor i in 0..10:\n    if i == 5:\n        break\n    sum = sum + i\n");
        check(ok, "real source 'break' inside a range for-loop ran without error");
        check(aer_as_int(v3_register_get(0)) == 10, "sum == 10 — 0+1+2+3+4, break fired at i==5");
        chunk_free(&c);
    }

    /* Test 54 (feature completeness): destructuring assignment, comma-separated RHS values packed
       into an implicit array then unpacked — 'px, py, pz = 0.0, 0.0, 0.0', the exact shape nbody.aer
       uses. Registers: px=0, py=1, pz=2. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "px, py, pz = 1, 2, 3\n");
        check(ok, "real source 'px, py, pz = 1, 2, 3' ran without error");
        check(aer_as_int(v3_register_get(0)) == 1, "px == 1");
        check(aer_as_int(v3_register_get(1)) == 2, "py == 2");
        check(aer_as_int(v3_register_get(2)) == 3, "pz == 3");
        chunk_free(&c);
    }

    /* Test 55 (feature completeness): destructuring from a single array-valued RHS expression
       (not a literal comma list) — 'a, b = arr' unpacks arr[0]/arr[1]. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "arr = [10, 20]\na, b = arr\n");
        check(ok, "real source 'a, b = arr' (single array-valued RHS) ran without error");
        check(aer_as_int(v3_register_get(1)) == 10, "a == 10 — arr[0]");
        check(aer_as_int(v3_register_get(2)) == 20, "b == 20 — arr[1]");
        chunk_free(&c);
    }

    /* Test 56 (feature completeness): destructuring still works when a temp register is live
       across the assignment (the RHS uses an existing variable in an expression) — proves target
       registers are reserved before the RHS is parsed, so a brand-new target variable's register
       can't collide with a temp the RHS is still using. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "n = 5\na, b = n + 1, n + 2\n");
        check(ok, "real source destructuring with a live RHS temp ran without error");
        check(aer_as_int(v3_register_get(0)) == 5, "n == 5 — unchanged");
        check(aer_as_int(v3_register_get(1)) == 6, "a == 6 — n + 1");
        check(aer_as_int(v3_register_get(2)) == 7, "b == 7 — n + 2");
        chunk_free(&c);
    }

    /* Test 57 (feature completeness): global builtins — length(), append(), type(). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "arr = [1, 2, 3]\ny = length(arr)\n");
        check(ok, "real source 'y = length(arr)' ran without error");
        check(aer_as_int(v3_register_get(1)) == 3, "y == 3");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "arr = [1, 2]\nappend(arr, 3)\ny = length(arr)\n");
        check(ok, "real source 'append(arr, 3)' as a bare statement ran without error");
        check(aer_as_int(v3_register_get(1)) == 3, "y == 3 — append() grew the same array in place");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = type(5)\n");
        check(ok, "real source 'y = type(5)' ran without error");
        check(v3_string_eq(v3_register_get(0), "integer"), "y == \"integer\"");
        chunk_free(&c);
    }

    /* Test 58 (feature completeness): calling an unrecognized name (not a variable, function,
       struct, module, or builtin) is still a clean compile-time error. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "y = totally_unknown_name(1)\n");
        check(!ok, "calling a genuinely unknown name reports a clean parse error, not a crash");
        chunk_free(&c);
    }

    /* Test 59 (feature completeness): compound assignment on a struct field ('p.x += 5'), the
       exact shape nbody.aer's advance() function uses ('bj.vx += dx * mi'). Reads the fields back
       via plain 'rx = p.x' since v3_register_get only sees registers, not struct fields directly.
       Registers: p=0, rx=1, ry=2. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "struct Point:\n    x\n    y\n"
            "p = Point(1, 2)\np.x += 5\np.y *= 3\nrx = p.x\nry = p.y\n");
        check(ok, "real source 'p.x += 5' / 'p.y *= 3' ran without error");
        check(aer_as_int(v3_register_get(1)) == 6, "rx == 6 — 1 + 5");
        check(aer_as_int(v3_register_get(2)) == 6, "ry == 6 — 2 * 3");
        chunk_free(&c);
    }

    /* Test 60 (feature completeness): compound assignment on an array index ('arr[0] += 5'),
       symmetric to the struct-field case just above. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "arr = [1, 2, 3]\narr[1] += 10\ny = arr[1]\n");
        check(ok, "real source 'arr[1] += 10' ran without error");
        check(aer_as_int(v3_register_get(1)) == 12, "y == 12 — 2 + 10");
        chunk_free(&c);
    }

    /* Test 61 (feature completeness): a top-level ("global") variable is readable from inside a
       function body via OP_V3_LOAD_GLOBAL, mirroring parser.c's own local-miss-falls-back-to-
       global read. `SCALE` (register 0) is defined before `scaled` is called; `scaled`'s own
       parameter `x` shadows nothing since there's no name collision here. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "SCALE = 10\n"
            "function scaled(x):\n    return x * SCALE\n"
            "y = scaled(4)\n");
        check(ok, "real source function reading a top-level global (SCALE) ran without error");
        check(aer_as_int(v3_register_get(1)) == 40, "y == 40 — 4 * SCALE (SCALE read correctly from inside scaled())");
        check(aer_as_int(v3_register_get(0)) == 10, "SCALE == 10 — unchanged by the call");
        chunk_free(&c);
    }

    /* Test 62 (feature completeness): assignment inside a function is always local — it must NOT
       silently write through to a same-named global. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "x = 1\n"
            "function set_local():\n    x = 99\n    return x\n"
            "y = set_local()\n");
        check(ok, "real source function-local assignment shadowing a global ran without error");
        check(aer_as_int(v3_register_get(1)) == 99, "y == 99 — set_local()'s own return value");
        check(aer_as_int(v3_register_get(0)) == 1, "x == 1 at top level — unchanged; the function's 'x = 99' was local, not a write-through to the global");
        chunk_free(&c);
    }

    /* Test 63 (feature completeness, regression): a NEW variable declared inside a nested for-in
       loop's body must not alias the OUTER loop's own long-lived iteration registers (cur_reg/
       step_reg for a range loop, idx_reg/col_reg for an array loop). Found via nbody.aer's real
       `for i in 0..num_bodies: for j in (i+1)..num_bodies: ...` shape, which silently corrupted
       the outer loop's own `i` before this was fixed (v3_parse_for_in temporarily promotes its
       long-lived registers via v3_reserved_floor for the loop's duration — see its own comment).
       n=4: total pairs (i,j) with i<j is 4*3/2=6, not reachable at all if the inner loop's own `j`
       corrupts the outer `i` (produces 3, or hangs, depending on exactly how it corrupts). */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "n = 4\ntotal = 0\nfor i in 0..n:\n    for j in (i+1)..n:\n        total = total + 1\n");
        check(ok, "real source nested range for-loops (inner bound depends on outer var) ran without error");
        check(aer_as_int(v3_register_get(1)) == 6, "total == 6 — 4 choose 2 pairs; a wrong/hung result would mean the inner loop's own j corrupted the outer loop's i");
        chunk_free(&c);
    }
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "n = 3\ntotal = 0\nfor i in 0..n:\n    for j in 0..n:\n        total = total + 1\n");
        check(ok, "real source nested range for-loops (independent bounds) ran without error");
        check(aer_as_int(v3_register_get(1)) == 9, "total == 9 — 3*3, same aliasing hazard even when the inner loop doesn't depend on the outer var");
        chunk_free(&c);
    }

    /* Test 64 (feature completeness, regression): a multi-argument call/array-literal/struct
       construction where a NON-LAST argument is a computed expression (not a bare constant or
       bare variable) must still land every argument in truly contiguous registers. Found via
       nbody.aer's Body() constructor calls, several of whose args are `const * DAYS_PER_YEAR`
       expressions — v3_arg_materialize used to always allocate a NEW register via v3_reg_alloc()
       even when its input was already the topmost live temp, silently leaving a one-register gap
       that shifted every following argument by one slot. With only 2 items the gap happened to go
       unnoticed (nothing after the last one to misalign); a 3rd item exposed values shifted by
       exactly one register. `y` here mixes a computed non-last arg (a+b) with plain trailing args,
       matching the shape that broke. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c, "a = 2\nb = 3\narr = [a + b, 10, 20]\nx = arr[0]\ny = arr[1]\nz = arr[2]\n");
        check(ok, "real source array literal with a computed non-last item, read back fully, ran without error");
        check(aer_as_int(v3_register_get(3)) == 5,  "x == 5 — arr[0], the computed (a + b) item");
        check(aer_as_int(v3_register_get(4)) == 10, "y == 10 — arr[1], correctly NOT shifted into arr[0]'s old register gap");
        check(aer_as_int(v3_register_get(5)) == 20, "z == 20 — arr[2], correctly still the last item");
        chunk_free(&c);
    }

    /* Test 65 (fusion, found via a real per-opcode dispatch audit on nbody.aer): `x OP y.field`
       (field on the RIGHT) should compile to one OP_V3_BINARY_FIELD instead of an
       OP_V3_FIELD_GET followed by OP_V3_BINARY — covers a register LHS, a constant LHS, and
       confirms the deliberately-unfused reverse shape (field on the LEFT) still produces the
       correct value via the ordinary fallback path. */
    {
        Chunk c;
        chunk_init(&c);
        bool ok = v3_run_source(&c,
            "struct Point:\n    x\n    y\n\n"
            "p = Point(10, 20)\n"
            "a = 3\n"
            "b = a - p.x\n"
            "d = 100 - p.y\n"
            "e = p.x - a\n");
        check(ok, "real source fused/unfused struct-field binary ops ran without error");
        check(aer_as_int(v3_register_get(2)) == -7, "b == -7 — a - p.x, fused (reg OP field)");
        check(aer_as_int(v3_register_get(3)) == 80, "d == 80 — 100 - p.y, fused (const OP field)");
        check(aer_as_int(v3_register_get(4)) == 7,  "e == 7 — p.x - a, NOT fused (field on the left), still correct via the ordinary path");
        chunk_free(&c);
    }

    if (failures == 0) printf("\nAll v3 smoke tests passed.\n");
    else                printf("\n%d v3 smoke test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}

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

    if (failures == 0) printf("\nAll v3 smoke tests passed.\n");
    else                printf("\n%d v3 smoke test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}

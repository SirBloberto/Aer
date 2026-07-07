/* v3 register-VM prototype, M1 smoke test — proves the register allocator (parser_v3.c) and the
   three OP_V3_* opcodes (vm.c) work correctly together for genuinely nested expressions, entirely
   independent of the real .aer lexer/parser/interpreter (which this test never touches). See the
   register-based bytecode plan for the full M1 scope and why this is deliberately isolated.

   Build and run: make test-v3
*/
#include <stdbool.h>
#include <stdio.h>
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

/* Runs `c` (already ending in OP_HALT) on a fresh VM, returning true on a clean finish. */
static bool run_chunk(Chunk* c) {
    VM vm;
    vm_init(&vm, c);
    return vm_run(&vm);
}

int main(void) {
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

    if (failures == 0) printf("\nAll v3 smoke tests passed.\n");
    else                printf("\n%d v3 smoke test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}

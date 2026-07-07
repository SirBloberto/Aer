#ifndef PARSER_V3_H
#define PARSER_V3_H

#include "vm.h"

/* v3 register-VM prototype, M1 — register allocator + a hand-specified expression-tree compiler.
   See the register-based bytecode plan (jazzy-floating-starlight.md) for the full design. This is
   NOT wired into the real lexer/parser (that's M2+) — a test constructs a V3Node tree directly in
   C and hands it to v3_compile_expr, purely to exercise the allocator and OP_V3_* opcodes together
   for genuinely nested expressions before any real .aer-source integration exists. */

/* Register allocator, modeled directly on Lua's own compiler (lparser.c/lcode.c's
   FuncState.freereg) — a watermark that grows when a sub-expression needs a temp register and
   shrinks back once its result has been consumed, LIFO, matching how recursive-descent
   compilation naturally nests. v3_reg_reserve is for a test to set aside registers already holding
   "local" values (via OP_V3_LOADK) before compiling an expression that reads them — those slots
   must never be handed out by the allocator or freed by v3_compile_expr. */
void v3_reg_reset(void);
void v3_reg_reserve(int count);
int  v3_reg_alloc(void);
void v3_reg_free(int count);

typedef enum { V3_NODE_CONST, V3_NODE_REG, V3_NODE_BINARY } V3NodeKind;

typedef struct V3Node {
    V3NodeKind kind;
    AerVal     const_value;    /* V3_NODE_CONST */
    int        reg;            /* V3_NODE_REG — an already-live register (e.g. a reserved "local") */
    Opcode     bin_op;         /* V3_NODE_BINARY */
    struct V3Node *lhs, *rhs;  /* V3_NODE_BINARY */
} V3Node;

/* Compiles `node` into `c`, emitting OP_V3_BINARY instructions via the allocator above. Returns an
   RK-encoded operand (see V3_RK_CONST_FLAG, vm.h) usable directly as a parent node's operand — a
   constant or already-live register leaf never allocates anything of its own, only a BINARY node's
   result does, since it must survive to be read by its parent. */
int v3_compile_node(Chunk* c, V3Node* node);

#endif

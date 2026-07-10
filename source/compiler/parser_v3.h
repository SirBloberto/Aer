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

/* M2 — control flow, mirroring parser.c's own emit-then-patch jump idiom (e.g. emit_jump_if_false)
   for the register-operand opcodes. Each emits its instruction with a 0 placeholder for `target`
   and returns the OFFSET of that placeholder word — pass it to v3_patch_jump once the real target
   address (usually c->count at the jump-to point) is known. */
unsigned int v3_emit_cmp_jump_false(Chunk* c, int rk_a, Opcode cmp_op, int rk_b);
unsigned int v3_emit_jump_if_false_reg(Chunk* c, int reg);
void v3_patch_jump(Chunk* c, unsigned int patch_offset, unsigned int target);

/* Function calls — see OP_V3_CALL/OP_V3_RETURN's comment in vm.h for the real per-call register
   windowing this now compiles to (M5), replacing M3's single fixed callee bank. callee_offset must
   already be known at the call site — unlike the jump emitters above, there's no patch step, so a
   hand-driven test emitting a call needs to lay out the callee body (and record its starting
   offset) before compiling the call that targets it, and needs to be recursion-friendly: the SAME
   callee_offset can be targeted by an OP_V3_CALL inside the callee's own body for a self-call.
   arg_reg_base/arg_count describe a contiguous range in the CALLER's own register numbering;
   dest_reg is also the caller's numbering — v3_emit_call copies arg_reg_base..+arg_count into the
   new callee frame's own registers starting at 0, at runtime, not here. src_reg for
   v3_emit_return is a plain 0-based index into the callee's own frame — no fixed-bank offset to
   account for, since every call gets an isolated bank now. */
void v3_emit_call(Chunk* c, int dest_reg, unsigned int callee_offset, int arg_reg_base, int arg_count);
void v3_emit_return(Chunk* c, int src_reg);

/* M4 (arrays only — see the plan's deferred list for dicts/structs/iteration). Same "caller
   picks registers via v3_reg_alloc/v3_reg_reserve, the emitter just emits" convention as
   v3_emit_call/v3_emit_return above. rk_idx/rk_val are RK-encoded exactly like v3_compile_node's
   return value — a register, or a constant via V3_RK_CONST_FLAG. */
void v3_emit_array_new(Chunk* c, int dest_reg, int item_reg_base, int item_count);
void v3_emit_index_get(Chunk* c, int dest_reg, int arr_reg, int rk_idx);
void v3_emit_index_set(Chunk* c, int arr_reg, int rk_idx, int rk_val);

/* M4 follow-up — pair_reg_base/pair_count describe pair_count (key,val) register pairs (so
   2*pair_count registers total: key at pair_reg_base+2*i, val at +2*i+1), same convention
   lbl_v3_dict_new (vm.c) reads them with. */
void v3_emit_dict_new(Chunk* c, int dest_reg, int pair_reg_base, int pair_count);

/* M4's iteration slice (arrays only — see OP_V3_ITER_NEXT_ARRAY's comment in vm.h). Same
   emit-then-patch idiom as v3_emit_cmp_jump_false/v3_emit_jump_if_false_reg: emits with a 0
   placeholder for end_target and returns its offset for v3_patch_jump once the loop-exit address
   (c->count right after the loop body + back-edge jump) is known. The caller must have already
   set idx_reg to 0 and col_reg to a live array before this instruction first executes, and should
   emit this instruction's own offset as the loop's back-edge (OP_JUMP) target, matching the stack
   VM's own "the iterate opcode is the loop's back-edge target" convention (parse_for, parser.c). */
unsigned int v3_emit_iter_next_array(Chunk* c, int col_reg, int idx_reg, int item_dest_reg);

/* Feature-completeness follow-up — the integer-range form flagged (but deferred) in
   OP_V3_ITER_NEXT_ARRAY's own comment. Same emit-then-patch idiom: cur_reg MUST already be a
   register the loop owns exclusively (mutated every iteration — never an aliased existing
   variable's register), end_reg/step_reg may safely alias an existing variable's register
   (read-only). Caller emits this instruction's own offset as the loop's back-edge target, same
   convention as v3_emit_iter_next_array. */
unsigned int v3_emit_iter_range(Chunk* c, int cur_reg, int end_reg, int step_reg, int item_dest_reg);

/* M5 slice 6 — structs. type_name_pool_idx/field_name_pool_idx are plain pool indices (not
   RK-encoded — a type/field name is always a compile-time-known constant, never a register).
   rk_val (v3_emit_field_set) is RK-encoded like every other value operand. */
void v3_emit_struct_new(Chunk* c, int dest_reg, unsigned int type_name_pool_idx, int arg_reg_base, int arg_count);
void v3_emit_field_get(Chunk* c, int dest_reg, int struct_reg, unsigned int field_name_pool_idx);
void v3_emit_field_set(Chunk* c, int struct_reg, unsigned int field_name_pool_idx, int rk_val);

/* M5 slice 2 — real .aer source wiring. Compiles REAL source (already lexed via the real lexer's
   lex()/token, exactly like parser.c's own parse()) into v3 bytecode, for a narrow grammar subset:
   plain 'name = expr' assignment, arithmetic/comparisons, and if/else/for-while — see
   parser_v3.c's own comment block above its implementation for the full scope and what's still
   out of reach from real source (functions, containers, structs, for-in iteration, and more). */
void v3_parse(Chunk* c);

#endif

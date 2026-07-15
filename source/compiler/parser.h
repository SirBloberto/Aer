#ifndef PARSER_H
#define PARSER_H

#include "vm.h"

/* The register-VM compiler — real lexer output in, real bytecode out; this is the only parser
   AER has (see the register-based bytecode plan, jazzy-floating-starlight.md, for the full design
   history). reg_reset/reg_reserve/reg_alloc/reg_free below are the register allocator this file's
   compile functions share; tests/smoke_test.c also drives them directly, below the level of a
   real .aer file, to hand-build bytecode and exercise the VM in isolation from the lexer/parser. */

/* Register allocator, modeled directly on Lua's own compiler (lparser.c/lcode.c's
   FuncState.freereg) — a watermark that grows when a sub-expression needs a temp register and
   shrinks back once its result has been consumed, LIFO, matching how recursive-descent
   compilation naturally nests. reg_reserve is for a test to set aside registers already holding
   "local" values (via OP_LOADK) before compiling an expression that reads them — those slots
   must never be handed out by the allocator or freed by compile_expr. */
void reg_reset(void);
void reg_reserve(int count);
int  reg_alloc(void);
void reg_free(int count);

/* Control flow for the register-operand opcodes. Each emits its instruction with a 0 placeholder
   for `target` and returns the OFFSET of that placeholder word — pass it to patch_jump once the
   real target address (usually c->count at the jump-to point) is known. */
unsigned int emit_cmp_jump_false(Chunk* c, int rk_a, Opcode cmp_op, int rk_b);
unsigned int emit_jump_if_false_reg(Chunk* c, int reg);
void patch_jump(Chunk* c, unsigned int patch_offset, unsigned int target);

/* Function calls — see OP_CALL/OP_RETURN's comment in vm.h for the real per-call register
   windowing this compiles to. callee_offset must already be known at the call site — unlike the
   jump emitters above, there's no patch step, so a hand-driven test emitting a call needs to lay
   out the callee body (and record its starting offset) before compiling the call that targets it,
   and needs to be recursion-friendly: the SAME callee_offset can be targeted by an OP_CALL inside
   the callee's own body for a self-call. arg_reg_base/arg_count describe a contiguous range in the
   CALLER's own register numbering; dest_reg is also the caller's numbering — emit_call copies
   arg_reg_base..+arg_count into the new callee frame's own registers starting at 0, at runtime,
   not here. src_reg for emit_return is a plain 0-based index into the callee's own frame — no
   fixed-bank offset to account for, since every call gets an isolated bank. */
unsigned int emit_call(Chunk* c, int dest_reg, unsigned int callee_offset, int arg_reg_base, int arg_count);
void emit_return(Chunk* c, int src_reg);

/* Functions as values (see OP_CALL_VALUE's comment in vm.h). */
void emit_call_value(Chunk* c, int dest_reg, int arg_reg_base, int arg_count, int callee_reg);

/* REPL/shell-mode support (see OP_PRINT_REPL's comment in vm.h). */
void emit_print_repl(Chunk* c, int src_reg);

/* Same "caller picks registers via reg_alloc/reg_reserve, the emitter just emits" convention as
   emit_call/emit_return above. rk_idx/rk_val are RK-encoded exactly like compile_node's return
   value — a register, or a constant via RK_CONST_FLAG. */
void emit_array_new(Chunk* c, int dest_reg, int item_reg_base, int item_count);
void emit_index_get(Chunk* c, int dest_reg, int arr_reg, int rk_idx);
void emit_index_set(Chunk* c, int arr_reg, int rk_idx, int rk_val);

/* See OP_SLICE_GET's own comment in vm.h. */
void emit_slice_get(Chunk* c, int dest_reg, int arr_reg, int rk_start, int rk_end);

/* pair_reg_base/pair_count describe pair_count (key,val) register pairs (so 2*pair_count
   registers total: key at pair_reg_base+2*i, val at +2*i+1), same convention lbl_dict_new (vm.c)
   reads them with. */
void emit_dict_new(Chunk* c, int dest_reg, int pair_reg_base, int pair_count);

/* Array iteration (see OP_ITER_NEXT_ARRAY's comment in vm.h). Same emit-then-patch idiom as
   emit_cmp_jump_false/emit_jump_if_false_reg: emits with a 0 placeholder for end_target and
   returns its offset for patch_jump once the loop-exit address (c->count right after the loop
   body + back-edge jump) is known. The caller must have already set idx_reg to 0 and col_reg to a
   live array before this instruction first executes, and should emit this instruction's own
   offset as the loop's back-edge (OP_JUMP) target. */
unsigned int emit_iter_next_array(Chunk* c, int col_reg, int idx_reg, int item_dest_reg);

/* `for k, v in dict:`, the two-loop-variable dict-only form (see OP_ITER_NEXT_PAIR's comment in
   vm.h). Same emit-then-patch idiom as emit_iter_next_array/emit_iter_range: caller emits this
   instruction's own offset as the loop's back-edge target. */
unsigned int emit_iter_next_pair(Chunk* c, int col_reg, int idx_reg, int key_dest_reg, int val_dest_reg);

/* The integer-range for-loop form (see OP_ITER_NEXT_ARRAY's own comment). Same emit-then-patch
   idiom: cur_reg MUST already be a register the loop owns exclusively (mutated every iteration —
   never an aliased existing variable's register), end_reg/step_reg may safely alias an existing
   variable's register (read-only). Caller emits this instruction's own offset as the loop's
   back-edge target, same convention as emit_iter_next_array. */
unsigned int emit_iter_range(Chunk* c, int cur_reg, int end_reg, int step_reg, int item_dest_reg);

/* Structs. type_name_pool_idx/field_name_pool_idx are plain pool indices (not RK-encoded — a
   type/field name is always a compile-time-known constant, never a register). rk_val
   (emit_field_set) is RK-encoded like every other value operand. */
void emit_struct_new(Chunk* c, int dest_reg, unsigned int type_name_pool_idx, int arg_reg_base, int arg_count);
void emit_field_get(Chunk* c, int dest_reg, int struct_reg, unsigned int field_name_pool_idx);
void emit_field_set(Chunk* c, int struct_reg, unsigned int field_name_pool_idx, int rk_val);

/* Resets every persistent compile-time table (register allocator, variable/global/function/
   struct-type tables, function/loop depth) to empty — call this ONCE before compiling a fresh,
   independent program (a REPL session's startup; a one-shot file run; each independent test
   case), never between statements of the SAME program/session, or names and registers from
   earlier lines would vanish. See parser_reset's own comment in parser.c for the full reasoning. */
void parser_reset(void);

/* Opaque snapshot of every persistent compile-time table — needed for a nested parse() call
   against a DIFFERENT chunk while an outer parse() call is still on the C call stack (a
   file-based `import` statement, processed synchronously mid-parse, aer_module.c). Every table
   these two functions touch is a file-scope static in parser.c shared by whichever parse() call
   is innermost — without this, compiling the imported file would silently corrupt the importing
   file's own still-in-progress compile state the moment control returns to it. Mirrors
   lexer_save_state()/lexer_restore_state()'s identical role for the lexer's own cursor/token
   (aer_module_load already saves/restores that around the same nested call). Save also resets
   every table to a blank slate (equivalent to parser_reset(), which the caller should NOT
   separately call) so the nested compile starts clean; restore reinstates the outer file's exact
   state and frees whatever the nested compile allocated. */
typedef struct ParserState ParserState;
ParserState* parser_save_state(void);
void           parser_restore_state(ParserState* saved);

/* Compiles real source (already lexed via the real lexer's lex()/token) into bytecode. Safe to
   call repeatedly against the SAME chunk without parser_reset in between (a REPL calling it once
   per line/block): per-statement error recovery rolls back a failed statement's partial bytecode
   and resumes at the next line/dedent boundary instead of aborting the whole call, and none of
   the persistent tables reset internally — see this function's own comment in parser.c. */
void parse(Chunk* c);

#endif

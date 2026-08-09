#ifndef PARSER_H
#define PARSER_H

#include "vm.h"

/* The compiler: lexer output in, bytecode out. tests/smoke_test.c also drives the emitters
   directly to hand-build bytecode. */

/* LIFO register allocator (Lua's freereg watermark model). reg_reserve pins slots a test has
   preloaded so the allocator never hands them out. */
void reg_reset(void);
void reg_reserve(int count);
int reg_alloc(void);
void reg_free(int count);

/* Emits with a 0 placeholder target and returns its offset for patch_jump. */
unsigned int emit_jump_if_false_reg(Chunk* c, int reg);
void patch_jump(Chunk* c, unsigned int patch_offset, unsigned int target);
/* Absolute counterpart of patch_jump, for a call's callee_offset -- see parser.c. */
void patch_call_target(Chunk* c, unsigned int patch_offset, unsigned int target);
void emit_jump_target(Chunk* c, unsigned int target);

/* callee_offset must be known at the call site (no patch step) -- lay out the callee first. */
unsigned int emit_call(Chunk* c, int dest_reg, unsigned int callee_offset, int arg_reg_base, int arg_count,
                       unsigned int func_index);
void emit_return(Chunk* c, int src_reg);

/* Functions as values (see OP_CALL_VALUE's comment in vm.h). */
void emit_call_value(Chunk* c, int dest_reg, int arg_reg_base, int arg_count, int callee_reg);

/* REPL/shell-mode support (see OP_PRINT_REPL's comment in vm.h). */
void emit_print_repl(Chunk* c, int src_reg);

/* rk_idx/rk_val are RK-encoded: a register, or a constant via RK_CONST_FLAG. */
void emit_array_new(Chunk* c, int dest_reg, int item_reg_base, int item_count);
void emit_index_get(Chunk* c, int dest_reg, int arr_reg, int rk_idx);
void emit_index_set(Chunk* c, int arr_reg, int rk_idx, int rk_val);

/* See OP_SLICE_GET's own comment in vm.h. */
void emit_slice_get(Chunk* c, int dest_reg, int arr_reg, int rk_start, int rk_end);

/* pair_count (key,val) register pairs: key at pair_reg_base+2*i, val at +2*i+1. */
void emit_dict_new(Chunk* c, int dest_reg, int pair_reg_base, int pair_count);

/* Emit-then-patch like emit_jump_if_false_reg; the caller emits this instruction's own offset
   as the loop's back-edge target. */
unsigned int emit_iter_next_array(Chunk* c, int col_reg, int idx_reg, int item_dest_reg);

/* The two-variable dict iteration form; same emit-then-patch idiom. */
unsigned int emit_iter_next_pair(Chunk* c, int col_reg, int idx_reg, int key_dest_reg, int val_dest_reg);

/* Rotated range-for. cur/end/step MUST be loop-owned snapshot registers, never an alias to a
   variable's register. prep returns the empty-range exit patch offset. */
unsigned int emit_iter_range_prep(Chunk* c, int cur_reg, int end_reg, int step_reg, int item_dest_reg,
                                  bool guard_nonneg);
void emit_iter_range_loop(Chunk* c, int cur_reg, int end_reg, int step_reg, int item_dest_reg,
                          unsigned int body_target);

/* Type/field names are plain pool indices (compile-time constants, never registers). */
void emit_struct_new(Chunk* c, int dest_reg, unsigned int type_name_pool_idx, int arg_reg_base,
                     int arg_count);
void emit_field_get(Chunk* c, int dest_reg, int struct_reg, unsigned int field_name_pool_idx);
void emit_field_set(Chunk* c, int struct_reg, unsigned int field_name_pool_idx, int rk_val);

/* `[value; count]` repeat-literal construction; rk_count is RK-encoded. */
void emit_array_repeat(Chunk* c, int dest_reg, int fill_reg, int narrow_flag, int rk_count);

/* A variable's value by name, from wherever the compiler placed it (boxed register, raw int slot,
   or raw real slot); false if never assigned. For tests: which storage a variable earns moves with
   optimization work, so a test reading a fixed register index asserts on a non-contract. */
bool parser_read_variable(VM* vm, Chunk* c, const char* name, AerVal* out);

/* Reset every persistent compile table -- ONCE per independent program, never between
   statements of the same session (REPL persistence depends on it). */
void parser_reset(void);

/* Snapshot/restore of the parser's file-scope tables, for a nested parse() of an imported
   file mid-compile -- without it the import corrupts the outer compile. */
typedef struct ParserState ParserState;
ParserState* parser_save_state(void);
void parser_restore_state(ParserState* saved);

/* Compile lexed source into bytecode. Safe to call repeatedly on the same chunk (REPL):
   per-statement recovery rolls back a failed statement and resumes. */
void parse(Chunk* c);

/* Lazily compiles a specialized body for a shape-sensitive function, keyed by a Shape observed at
   a real call site -- see vm.c's vm_call_resolve_specialization (the only caller) and its own comment
   in parser.c for the full contract. */
bool parser_specialize_function(Chunk* c, ChunkFunction* target_f, Shape* shape, SpecKind kind,
                                int param_index, SpecEntry* out_entry, const int* raw_param_regs,
                                const ValueType* raw_param_types, int raw_param_count);

#endif

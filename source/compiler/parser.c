#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include "error.h"
#include "lexer.h"

/* See parser.h's comment — this is the register allocator every compile function in this file
   shares. next_temp_register tracks the next free temp register; registers below reserved_floor
   are permanent (a real variable/global, or a loop's own promoted iteration state) and must never
   be handed out or freed here. */
static int next_temp_register = 0;
static int reserved_floor     = 0;

void reg_reset(void) {
    next_temp_register = 0;
    reserved_floor     = 0;
}

/* Must refuse to hand out/reserve a register >= FRAME_REGISTERS, the literal size of
   CallFrame.registers[] — an unchecked overflow here is an out-of-bounds write into CallFrame's
   neighboring fields (return_ip, dest_reg, defers, defer_count), surfacing much later as a GC-time
   segfault. Returns an in-bounds sentinel after erroring so the rest of this statement's
   compilation can't cause a second OOB access before parse_had_error is checked. */
void reg_reserve(int count) {
    if (reserved_floor + count > FRAME_REGISTERS) {
        error_at("Too many live variables/temporaries (max %d registers per call)", FRAME_REGISTERS);
        return;
    }
    reserved_floor     += count;
    next_temp_register += count;
}

int reg_alloc(void) {
    if (next_temp_register >= FRAME_REGISTERS) {
        error_at("Too many live variables/temporaries (max %d registers per call)", FRAME_REGISTERS);
        return FRAME_REGISTERS - 1;
    }
    return next_temp_register++;
}

void reg_free(int count) {
    next_temp_register -= count;
    /* Never free below the reserved floor — a bug in a future caller shouldn't be able to hand
       out a "local"'s register as if it were a free temp. */
    if (next_temp_register < reserved_floor) next_temp_register = reserved_floor;
}

int compile_node(Chunk* c, Node* node) {
    if (node->kind == NODE_CONST) {
        unsigned int pool_idx = chunk_add_pool(c, node->const_value);
        return (int)pool_idx | RK_CONST_FLAG;
    }
    if (node->kind == NODE_REG) {
        return node->reg;   /* already live — an RK "register" operand, not const-flagged */
    }

    int rk_lhs = compile_node(c, node->lhs);
    int rk_rhs = compile_node(c, node->rhs);

    /* Free operand registers (if this node allocated them — a CONST or an already-reserved REG
       leaf never does) BEFORE allocating the result's register, so the result reuses the lowest
       just-freed slot instead of growing the watermark further. Exactly Lua's own free-then-
       allocate discipline (lcode.c) — this is what keeps register usage compact across a deep
       expression tree instead of growing linearly with tree size. Only free slots THIS call
       allocated: a NODE_REG leaf's register belongs to whatever reserved it, not to us. */
    if (!(rk_rhs & RK_CONST_FLAG) && node->rhs->kind == NODE_BINARY) reg_free(1);
    if (!(rk_lhs & RK_CONST_FLAG) && node->lhs->kind == NODE_BINARY) reg_free(1);

    int dest = reg_alloc();
    chunk_emit(c, PACK2(OP_BINARY, dest, (int)node->bin_op));
    chunk_emit(c, rk_lhs);
    chunk_emit(c, rk_rhs);
    return dest;
}

unsigned int emit_cmp_jump_false(Chunk* c, int rk_a, Opcode cmp_op, int rk_b) {
    chunk_emit(c, PACK1(OP_CMP_JUMP_FALSE, (int)cmp_op));
    chunk_emit(c, rk_a);
    chunk_emit(c, rk_b);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by patch_jump once the target is known */
    return patch_offset;
}

unsigned int emit_jump_if_false_reg(Chunk* c, int reg) {
    chunk_emit(c, PACK1(OP_JUMP_IF_FALSE_REG, reg));
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);
    return patch_offset;
}

void patch_jump(Chunk* c, unsigned int patch_offset, unsigned int target) {
    c->code[patch_offset] = (int)target;
}

/* Returns the offset of the just-emitted callee_offset word — a forward-referencing call (M6
   slice 2, pending_call_add) doesn't know the real target yet, so it emits a placeholder here
   and needs this offset to patch in the real one later via patch_jump, same emit-then-patch
   idiom as emit_iter_next_array/emit_iter_range. An already-resolved call simply ignores the
   return value — patching isn't needed when callee_offset was already correct at emit time. */
unsigned int emit_call(Chunk* c, int dest_reg, unsigned int callee_offset, int arg_reg_base, int arg_count) {
    chunk_emit(c, PACK3(OP_CALL, dest_reg, arg_reg_base, arg_count));
    unsigned int patch_offset = c->count;
    chunk_emit(c, (int)callee_offset);
    return patch_offset;
}

void emit_return(Chunk* c, int src_reg) {
    chunk_emit(c, PACK1(OP_RETURN, src_reg));
}

/* functions as values. callee_reg is always a plain register (never RK-encoded —
   whatever's referenced as a value already went through materialize/arg_materialize by the
   time this is emitted), so no patching is ever needed here the way emit_call's callee_offset
   sometimes does. */
void emit_call_value(Chunk* c, int dest_reg, int arg_reg_base, int arg_count, int callee_reg) {
    chunk_emit(c, PACK3(OP_CALL_VALUE, dest_reg, arg_reg_base, arg_count));
    chunk_emit(c, callee_reg);
}

void emit_print_repl(Chunk* c, int src_reg) {
    chunk_emit(c, PACK1(OP_PRINT_REPL, src_reg));
}

void emit_array_new(Chunk* c, int dest_reg, int item_reg_base, int item_count) {
    chunk_emit(c, PACK3(OP_ARRAY_NEW, dest_reg, item_reg_base, item_count));
}

void emit_index_get(Chunk* c, int dest_reg, int arr_reg, int rk_idx) {
    chunk_emit(c, PACK2(OP_INDEX_GET, dest_reg, arr_reg));
    chunk_emit(c, rk_idx);
}

void emit_index_set(Chunk* c, int arr_reg, int rk_idx, int rk_val) {
    chunk_emit(c, PACK1(OP_INDEX_SET, arr_reg));
    chunk_emit(c, rk_idx);
    chunk_emit(c, rk_val);
}

/* see OP_SLICE_GET's own comment in vm.h. rk_start/rk_end are RK-encoded exactly
   like every other value operand — a missing bound is passed in as an RK-encoded null constant,
   built by the caller (parse_primary), not specially by this function. */
void emit_slice_get(Chunk* c, int dest_reg, int arr_reg, int rk_start, int rk_end) {
    chunk_emit(c, PACK2(OP_SLICE_GET, dest_reg, arr_reg));
    chunk_emit(c, rk_start);
    chunk_emit(c, rk_end);
}

void emit_dict_new(Chunk* c, int dest_reg, int pair_reg_base, int pair_count) {
    chunk_emit(c, PACK3(OP_DICT_NEW, dest_reg, pair_reg_base, pair_count));
}

unsigned int emit_iter_next_array(Chunk* c, int col_reg, int idx_reg, int item_dest_reg) {
    chunk_emit(c, PACK3(OP_ITER_NEXT_ARRAY, col_reg, idx_reg, item_dest_reg));
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by patch_jump once the loop-exit target is known */
    return patch_offset;
}

unsigned int emit_iter_next_pair(Chunk* c, int col_reg, int idx_reg, int key_dest_reg, int val_dest_reg) {
    /* Same word layout as emit_iter_range below — 4 narrow fields, val_dest_reg gets its own
       word since only 3 fit alongside the opcode, end_target stays dedicated per the patchable-
       jump-target rule. */
    chunk_emit(c, PACK3(OP_ITER_NEXT_PAIR, col_reg, idx_reg, key_dest_reg));
    chunk_emit(c, val_dest_reg);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by patch_jump once the loop-exit target is known */
    return patch_offset;
}

unsigned int emit_iter_range(Chunk* c, int cur_reg, int end_reg, int step_reg, int item_dest_reg) {
    /* 4 narrow fields, but only 3 fit alongside the opcode in one packed word — item_dest_reg
       gets its own word rather than displacing end_target from ITS dedicated word (hard rule:
       a patchable jump target is never packed alongside anything else, so patch_jump's blind
       overwrite stays correct). Still 3 words total, down from 6. */
    chunk_emit(c, PACK3(OP_ITER_RANGE, cur_reg, end_reg, step_reg));
    chunk_emit(c, item_dest_reg);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by patch_jump once the loop-exit target is known */
    return patch_offset;
}

void emit_struct_new(Chunk* c, int dest_reg, unsigned int type_name_pool_idx, int arg_reg_base, int arg_count) {
    chunk_emit(c, PACK3(OP_STRUCT_NEW, dest_reg, arg_reg_base, arg_count));
    chunk_emit(c, (int)type_name_pool_idx);
}

void emit_field_get(Chunk* c, int dest_reg, int struct_reg, unsigned int field_name_pool_idx) {
    chunk_emit(c, PACK2(OP_FIELD_GET, dest_reg, struct_reg));
    chunk_emit(c, (int)field_name_pool_idx);
}

void emit_field_set(Chunk* c, int struct_reg, unsigned int field_name_pool_idx, int rk_val) {
    chunk_emit(c, PACK1(OP_FIELD_SET, struct_reg));
    chunk_emit(c, (int)field_name_pool_idx);
    chunk_emit(c, rk_val);
}

/* ------------------------------------------------------------------ */
/* M5 — real .aer source wiring, built up slice by slice (variables/arithmetic/if/for-while;
   functions/calls incl. recursion; array/dict literals + indexing; `for x in y:` array
   iteration — plain string literals landed alongside containers, needed to test dict keys).
   An entirely separate recursive-descent compiler from parser.c, sharing only the real lexer's
   token stream (lex()/token/consume()/equal()/require(), lexer.h) — not a shared/refactored
   grammar, so this can't destabilize the production compiler while v3 remains unproven. See the
   plan file for the full roadmap and what's still deferred (structs, compound assignment, unary
   operators, and/or, string interpolation, dict-key/pair + range iteration, imports, defer,
   modules — none of that is reachable from real source through this entry point yet). */
/* ------------------------------------------------------------------ */

static int  parse_primary_inner(Chunk* c);
static int  parse_primary(Chunk* c);
static int  parse_postfix_chain(Chunk* c, int rk);
static int  parse_function_expr(Chunk* c);
static void discard_statement_result(Chunk* c, int dest);
static int  parse_string_literal(Chunk* c);
static int  parse_unary_inner(Chunk* c);
static int  parse_unary(Chunk* c);
static int  parse_binary_ops(Chunk* c, unsigned int min_prec, int lhs, unsigned int lhs_start);
static int  parse_binary(Chunk* c, unsigned int min_prec);
static int  compile_and(Chunk* c, int lhs, unsigned int prec);
static int  compile_or(Chunk* c, int lhs, unsigned int prec);
static int  compile_pipe(Chunk* c, int lhs);
static void parse_statement(Chunk* c);
static void parse_block(Chunk* c);
static void parse_if(Chunk* c);
static int  parse_if_expr(Chunk* c);
static void parse_for_while(Chunk* c);
static void parse_for_body(Chunk* c, unsigned int loop_top, int rk_cond);
static void parse_assignment(Chunk* c, unsigned int name_idx);
static void parse_chain_assignment(Chunk* c, unsigned int name_idx, bool first_is_index);
static int  parse_call(Chunk* c, unsigned int name_idx);
static void parse_function(Chunk* c);
static bool parse_literal_default(Chunk* c, AerVal* out);
static void parse_return(Chunk* c);
static void parse_struct(Chunk* c);
static bool at_module_name(Chunk* c);
static int  parse_module_call(Chunk* c);
static void parse_import(Chunk* c);
static void parse_defer(Chunk* c);
static bool is_builtin_name(Chunk* c, unsigned int name_idx);
static int  parse_builtin_call(Chunk* c, unsigned int name_idx);
static int  parse_contiguous_exprs(Chunk* c, TokenType close_tok, int* out_base);

/* name_idx -> register. var_regs[i] holds the actual register for var_names[i] — usually
   (but see var_slot's comment for the one case where it isn't) the same as i itself. No
   global/local distinction (unlike parser.c's current_locals) — this slice has no function defs,
   so every register in the one frame a script runs in is effectively local. */
static unsigned int var_names[FRAME_REGISTERS];
static int          var_regs[FRAME_REGISTERS];
static int          var_count = 0;

/* Nonzero while compiling a function body — lets parse_return reject a top-level `return`. No
   nesting to track (named functions can't nest), so a plain counter (0 or 1) is enough, not a
   stack. Declared here (rather than nearer parse_function, its main use) because var_slot below
   needs to check it too. */
static int function_depth = 0;

/* Top-level ("global") variable names: a name not found among the CURRENT function's own locals
   (var_lookup) may still be a top-level variable, readable (never assignable — assignment inside a
   function is always local) from inside any function body. Updated in lockstep with var_names by
   var_slot itself, but ONLY while function_depth == 0 — var_names gets reset/restored around a
   function body but this table doesn't, so it keeps accumulating every top-level name defined so
   far across a function def and back out again. A global defined AFTER a function that reads it
   (textually) won't be visible to that function — same "no forward references" limitation as
   functions/structs, not a new gap. */
static unsigned int global_names[FRAME_REGISTERS];
static int          global_regs[FRAME_REGISTERS];
static int          global_count = 0;

/* A new variable's register is reserved_floor — NOT var_count, even though the two almost always
   hold the same value (both only ever move together, by exactly 1, right here). reserved_floor is
   what actually marks "everything below this is permanent, never handed out as a temp" — var_count
   is just this function's own bookkeeping of how many names it has seen. They diverge when
   something else raises reserved_floor without registering a new named variable, which is exactly
   what a for-loop's long-lived iteration registers do (parse_for_in): cur_reg/step_reg (or
   idx_reg/col_reg) must keep living for the loop's entire body, so the loop temporarily promotes
   reserved_floor past them before compiling its body. A brand-new variable declared inside that
   body must land ABOVE those promoted registers — using var_count there instead would alias the
   loop's own iteration state (a real, silent-corruption bug: nested `for i in 0..n: for j in
   (i+1)..n:` let the inner loop's `j` collide with the outer loop's cur_reg). next_temp_register is
   resynced to match immediately after — safe because parsing any single expression leaves at most
   one temp live above wherever reserved_floor already was (this file's universal free-then-allocate
   discipline).
     Once reg/index do diverge for some name, var_regs[] is what makes name lookup still resolve to
   the correct actual register — the array position and the real register are no longer assumed to
   be the same number. */
static int var_slot(unsigned int name_idx) {
    for (int i = 0; i < var_count; i++)
        if (var_names[i] == name_idx) return var_regs[i];
    /* Checks reserved_floor — the register `reg` below is about to become — not var_count; the two
       can diverge (see this function's own comment above), and a for-loop's iteration-state
       promotion raises reserved_floor without touching var_count, so checking var_count here let a
       new variable declared inside such a loop's body silently receive an out-of-bounds register
       once enough promotions had piled up. */
    if (reserved_floor >= FRAME_REGISTERS) {
        error_at("Too many variables (max %d)", FRAME_REGISTERS);
        return -1;
    }
    int reg = reserved_floor;
    var_names[var_count] = name_idx;
    var_regs[var_count]  = reg;
    var_count++;
    reserved_floor++;                        /* permanently protects this register from the temp allocator */
    next_temp_register = reserved_floor;   /* resync — see this function's own comment for why that's always safe */
    if (function_depth == 0) {
        global_names[global_count] = name_idx;
        global_regs[global_count]  = reg;
        global_count++;
    }
    return reg;
}

/* Non-creating — a name found here is a top-level variable, readable from inside a function body
   (see global_names's own comment). */
static bool global_lookup(unsigned int name_idx, int* out_reg) {
    for (int i = 0; i < global_count; i++)
        if (global_names[i] == name_idx) { *out_reg = global_regs[i]; return true; }
    return false;
}

/* non-creating counterpart to var_slot, for compound assignment: unlike a plain
   `name = expr` (which may be defining `name` for the first time), `name += expr` needs `name` to
   already have a value to read, so it must not silently allocate a fresh (zero-garbage) register
   on a miss the way var_slot does. */
static bool var_lookup(unsigned int name_idx, int* out_reg) {
    for (int i = 0; i < var_count; i++)
        if (var_names[i] == name_idx) { *out_reg = var_regs[i]; return true; }
    return false;
}

/* Register-value equivalent of compile_node's "was this a NODE_BINARY" check (only
   meaningful for a hand-built tree) — works for any RK operand since a temp always lives at/above
   the reserved floor, a permanent variable register always below it. static int
   reserved_floor's declaration (top of this file) makes this valid here. */
static bool is_temp(int rk) {
    return !(rk & RK_CONST_FLAG) && rk >= reserved_floor;
}

/* Ensures rk is a plain register (not RK-const), materializing a constant into a fresh temp via
   OP_LOADK if needed — OP_JUMP_IF_FALSE_REG needs an actual register operand, no RK form. */
static int materialize(Chunk* c, int rk) {
    if (!(rk & RK_CONST_FLAG)) return rk;
    int reg = reg_alloc();
    chunk_emit(c, PACK1(OP_LOADK, reg)); chunk_emit(c, rk & ~RK_CONST_FLAG);
    return reg;
}

/* function name -> instruction offset, separate from var_names (a function needs a jump target,
   not a register). func_arities/min_arities/defaults support functions as values and default
   parameters: building a real runtime AerFunction (build_function_value, below), whether for a
   bare-name value reference or a direct call that omits trailing arguments, needs a function's
   full signature, not just its offset; calling by name with EVERY argument supplied still goes
   through func_lookup/OP_CALL directly — see parse_call's own comment on when each path is used.
   Grows on demand (xrealloc-doubling, matching chunk_pool_append). */
static unsigned int* func_names          = NULL;
static unsigned int* func_offsets        = NULL;
static unsigned int* func_arities        = NULL;
static unsigned int* func_min_arities    = NULL;
static AerVal**       func_defaults      = NULL;   /* func_defaults[i]: xmalloc'd array of (arity-min_arity) values, or NULL if none */
static bool*          func_has_receiver  = NULL;   /* `function f(target as Type, ...)`: see build_function_value's own comment */
static unsigned int*  func_receiver_type = NULL;   /* meaningful only where func_has_receiver[i] is true */
static int           func_count = 0;
static int           func_cap   = 0;

static bool func_lookup(unsigned int name_idx, unsigned int* out_offset) {
    for (int i = 0; i < func_count; i++)
        if (func_names[i] == name_idx) { *out_offset = func_offsets[i]; return true; }
    return false;
}

/* functions as values / default parameters. Separate from func_lookup since most
   call sites (a direct-by-name call supplying every argument) only ever need the offset; only a
   function referenced as a VALUE, or a direct call omitting trailing (defaulted) arguments, needs
   the rest of the signature to build a real AerFunction (build_function_value, below). */
static bool func_full_lookup(unsigned int name_idx, unsigned int* out_offset, unsigned int* out_arity,
                                 unsigned int* out_min_arity, AerVal** out_defaults,
                                 bool* out_has_receiver, unsigned int* out_receiver_type) {
    for (int i = 0; i < func_count; i++) {
        if (func_names[i] != name_idx) continue;
        *out_offset        = func_offsets[i];
        *out_arity         = func_arities[i];
        *out_min_arity     = func_min_arities[i];
        *out_defaults      = func_defaults[i];
        *out_has_receiver  = func_has_receiver[i];
        *out_receiver_type = func_receiver_type[i];
        return true;
    }
    return false;
}

/* Functions as values / default parameters. Builds a real runtime AerFunction, reusing the
   existing representation/constructor (vm_new_function/aer_function_val) rather than duplicating
   it. `defaults` is used AS-IS, not copied — it's already an independently xmalloc'd array
   (func_register's own comment). has_receiver/receiver_type (`function f(target as Type, ...)`)
   are checked once, at call setup (lbl_call_value, vm.c), not via a separate opcode a user's own
   code would need to emit. */
static AerVal build_function_value(unsigned int func_offset, unsigned int arity, unsigned int min_arity,
                                       AerVal* defaults, bool has_receiver, unsigned int receiver_type) {
    AerFunction* fn = vm_new_function();
    fn->code_offset   = func_offset;
    fn->arity         = (uint16_t)arity;
    fn->min_arity     = (uint16_t)min_arity;
    fn->defaults      = defaults;
    fn->has_receiver  = has_receiver;
    fn->receiver_type = receiver_type;
    return aer_function_val(fn);
}

/* forward references / mutual recursion. A call/defer site whose target isn't
   registered YET is optimistically assumed to be a function defined LATER in this same parse()
   call — recorded here (with a placeholder callee_offset already emitted at patch_offset, and the
   call site's own source cursor for a useful error message later) instead of failing immediately.
   func_register (below) patches every pending entry for a name the moment that name actually
   gets registered. Anything still pending once parse()'s top-level loop ends genuinely was
   never defined anywhere in this call, and is reported there — see parse's own comment for why
   "this call" (not "ever, across a whole REPL session") is the right scope: v3 resolves calls at
   COMPILE time, unlike the stack VM's runtime dynamic-scope lookup, so there's no sound way to
   leave a reference "maybe still resolvable" indefinitely across separate parse() calls the
   way the stack VM's names can. Grows on demand, same xrealloc-doubling convention as
   func_names/struct_names above. */
typedef struct {
    unsigned int name_idx;
    unsigned int patch_offset;
    const char*  call_site_cursor;
} PendingCall;
static PendingCall* pending_calls = NULL;
static int            pending_count = 0;
static int            pending_cap   = 0;

/* call_site_cursor is passed in explicitly (a caller-captured current_source_cursor() snapshot),
   not read internally — by the time this is called, the caller has already consumed the argument
   list (and possibly more), so "now" would point well past the actual name, not at it. Callers
   snapshot the cursor the moment they discover the name is unresolved, before parsing anything
   else, so a later deferred error_at (parse's own drain loop) points at the right place. */
static void pending_call_add(unsigned int name_idx, unsigned int patch_offset, const char* call_site_cursor) {
    if (pending_count >= pending_cap) {
        pending_cap    = pending_cap ? pending_cap * 2 : 8;
        pending_calls  = xrealloc(pending_calls, sizeof(PendingCall) * (size_t)pending_cap);
    }
    pending_calls[pending_count].name_idx         = name_idx;
    pending_calls[pending_count].patch_offset     = patch_offset;
    pending_calls[pending_count].call_site_cursor = call_site_cursor;
    pending_count++;
}

/* `defaults` is taken by ownership — an xmalloc'd array of (arity-min_arity) values the caller
   built (parse_function), or NULL if this function has no defaulted parameters; stored as-is,
   never copied, same as AerFunction.defaults itself never is. */
static void func_register(Chunk* c, unsigned int name_idx, unsigned int offset, unsigned int arity,
                              unsigned int min_arity, AerVal* defaults,
                              bool has_receiver, unsigned int receiver_type) {
    if (func_count >= func_cap) {
        func_cap          = func_cap ? func_cap * 2 : 16;
        func_names        = xrealloc(func_names,        sizeof(unsigned int) * (size_t)func_cap);
        func_offsets      = xrealloc(func_offsets,      sizeof(unsigned int) * (size_t)func_cap);
        func_arities      = xrealloc(func_arities,      sizeof(unsigned int) * (size_t)func_cap);
        func_min_arities  = xrealloc(func_min_arities,  sizeof(unsigned int) * (size_t)func_cap);
        func_defaults     = xrealloc(func_defaults,     sizeof(AerVal*) * (size_t)func_cap);
        func_has_receiver = xrealloc(func_has_receiver, sizeof(bool) * (size_t)func_cap);
        func_receiver_type= xrealloc(func_receiver_type,sizeof(unsigned int) * (size_t)func_cap);
    }
    func_names[func_count]        = name_idx;
    func_offsets[func_count]      = offset;
    func_arities[func_count]      = arity;
    func_min_arities[func_count]  = min_arity;
    func_defaults[func_count]     = defaults;
    func_has_receiver[func_count] = has_receiver;
    func_receiver_type[func_count]= receiver_type;
    func_count++;

    /* Also persists on the Chunk itself (not just this parser's own parse-time-only tables, which
       get reset/reused the moment a later, separate parse() call starts) — see ChunkFunction's
       own comment in vm.h for why a file-based `import`'s cross-module calls need this. */
    chunk_add_function(c, name_idx, offset, arity, min_arity, defaults, has_receiver, receiver_type);

    /* Patch every earlier forward-referencing call/defer to this name now that its real offset is
       known — swap-remove each match (order among pending entries never matters) so the list is
       left holding only genuinely still-unresolved entries. */
    for (int i = 0; i < pending_count; ) {
        if (pending_calls[i].name_idx == name_idx) {
            patch_jump(c, pending_calls[i].patch_offset, offset);
            pending_calls[i] = pending_calls[--pending_count];
        } else {
            i++;
        }
    }
}

/* break/continue loop-context stack. No `iter_slots`/pop step needed — a for-in loop's iterator
   state ([col_reg, idx_reg]) lives in ordinary registers the caller already owns, not extra
   value-stack slots that need balancing on an early exit (see OP_ITER_NEXT_ARRAY's own comment
   in vm.h). `break` is just "emit a jump, patch it once the loop's exit address is known". */
#define LOOP_MAX  16
#define BREAK_MAX 32
typedef struct {
    unsigned int top;                     /* continue's target — same value passed to parse_for_body/for_in */
    unsigned int patches[BREAK_MAX];   /* break's OP_JUMP operand offsets, patched once the loop ends */
    int          patch_count;
} LoopContext;
static LoopContext loop_stack[LOOP_MAX];
static int           loop_depth = 0;

static bool loop_push(unsigned int top) {
    if (loop_depth >= LOOP_MAX) {
        error_at("Too many nested loops (max %d)", LOOP_MAX);
        return false;
    }
    loop_stack[loop_depth].top         = top;
    loop_stack[loop_depth].patch_count = 0;
    loop_depth++;
    return true;
}

/* Patches every pending `break` recorded for the just-finished (innermost) loop to land at
   `exit_target` (the same address the loop's own condition-false exit already jumps to), then
   pops the loop context. */
static void loop_pop_and_patch(Chunk* c, unsigned int exit_target) {
    LoopContext* ctx = &loop_stack[loop_depth - 1];
    for (int i = 0; i < ctx->patch_count; i++)
        patch_jump(c, ctx->patches[i], exit_target);
    loop_depth--;
}

/* Struct type name registry, separate from func_names — a struct type isn't a callable offset,
   it resolves at runtime via chunk_find_shape(); OP_STRUCT_NEW just needs to know at compile time
   that `Name(...)` means "construct", not "call". */
static unsigned int* struct_names = NULL;
static int           struct_count = 0;
static int           struct_cap   = 0;

/* Max targets in a destructuring assignment (`a, b, c = ...`). */
#define MAX_DESTRUCT 16

static bool is_struct_name(unsigned int name_idx) {
    for (int i = 0; i < struct_count; i++)
        if (struct_names[i] == name_idx) return true;
    return false;
}

static void struct_register(unsigned int name_idx) {
    if (struct_count >= struct_cap) {
        struct_cap   = struct_cap ? struct_cap * 2 : 16;
        struct_names = xrealloc(struct_names, sizeof(unsigned int) * (size_t)struct_cap);
    }
    struct_names[struct_count++] = name_idx;
}

/* Forces rk into exactly the CURRENT allocator watermark, needed because OP_CALL's arguments
   must land in contiguous registers (its bulk-copy reads registers[arg_reg_base..+arg_count)).
   A fresh, just-computed temp is already the topmost live temp (this file's universal
   free-then-allocate discipline never leaves more than one live temp above the pre-expression
   watermark), so if `is_temp(rk) && rk == next_temp_register - 1`, rk is already sitting exactly
   where a fresh allocation would only end up copying it to anyway — reuse it directly, no MOVE
   needed. Calling reg_alloc() unconditionally instead allocates one register PAST rk, then emits a
   wasted MOVE and permanently abandons rk's own register as a gap — harmless for a single argument,
   but for the second or later argument in a multi-arg list that gap breaks contiguity outright: the
   bulk-copy opcode reads `arg_reg_base + i` for each i and silently gets a neighboring argument's
   leftover value instead. Only a bare constant or an existing permanent variable register (neither
   of which touched the temp watermark) still needs an actual fresh register. */
static int arg_materialize(Chunk* c, int rk) {
    if (!(rk & RK_CONST_FLAG) && is_temp(rk) && rk == next_temp_register - 1) {
        return rk;
    }
    int target = reg_alloc();
    if (rk & RK_CONST_FLAG) {
        chunk_emit(c, PACK1(OP_LOADK, target)); chunk_emit(c, rk & ~RK_CONST_FLAG);
    } else {
        chunk_emit(c, PACK2(OP_MOVE, target, rk));
    }
    return target;
}

/* Shared by call arguments, array literals, and (per-key/value) dict literals: all three
   bulk-copy opcodes (OP_CALL/OP_ARRAY_NEW/OP_DICT_NEW) need their operands in contiguous
   registers. Parses a comma-separated expression list up to (not including) close_tok — the
   caller still requires() close_tok itself, since a dict's pairs need extra ':' handling this
   bare list can't express. Returns the count; *out_base is unspecified when count==0. */
static int parse_contiguous_exprs(Chunk* c, TokenType close_tok, int* out_base) {
    int base  = -1;
    int count = 0;
    if (!equal(close_tok)) {
        do {
            int rk  = parse_binary(c, 0);
            int reg = arg_materialize(c, rk);
            if (count == 0) base = reg;
            count++;
        } while (consume(TOKEN_COMMA));
    }
    *out_base = base;
    return count;
}

/* Plain string literals only (see parse_string_literal for `"...{name}..."` interpolation). Needed
   because dict literals' keys must be strings. */
static unsigned int pool_escaped_string(Chunk* c, const char* s, unsigned int len) {
    static char buf[4096];
    if (len >= sizeof(buf)) {
        error_at("String literal too long (max %u bytes)", (unsigned int)sizeof(buf) - 1);
        len = 0;
    }
    unsigned int out = 0;
    for (unsigned int i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            i++;
            switch (s[i]) {
                case 'n':  buf[out++] = '\n'; break;
                case 't':  buf[out++] = '\t'; break;
                case '\\': buf[out++] = '\\'; break;
                case '"':  buf[out++] = '"';  break;
                /* `\{` suppresses interpolation (the pre-scan above already treats it as escaped,
                   never as an interpolation start), so it must unescape to a bare '{' the same
                   way `\"` unescapes to a bare '"' — without this case it fell through to
                   `default`, leaving a literal backslash in the output alongside the brace. */
                case '{':  buf[out++] = '{';  break;
                default:   buf[out++] = '\\'; buf[out++] = s[i]; break;
            }
        } else {
            buf[out++] = s[i];
        }
    }
    char* owned = xmalloc((size_t)out + 1);
    memcpy(owned, buf, out);
    owned[out] = '\0';
    AerVal sv = aer_make_string(owned, out);
    return chunk_add_pool(c, sv);
}

/* Operator precedence table. `and`/`or` are handled specially in parse_binary_ops (short-circuit
   jumps, not a plain OP_BINARY — see compile_and/compile_or), as is `as` (its "right-hand side" is
   a bare type name, not a normal expression). `|>` (pipe) is handled separately too (compile_pipe)
   since it needs call-argument prepending. Returns false for any operator outside this table. */
static bool binary_op_info(TokenType t, unsigned int* prec, Opcode* op) {
    switch (t) {
        case TOKEN_OR:            *prec = 1;  *op = OP_OR;         return true;
        case TOKEN_PIPE:          *prec = 1;  *op = OP_PIPE;       return true;
        case TOKEN_AND:           *prec = 2;  *op = OP_AND;        return true;
        case TOKEN_BITWISE_OR:    *prec = 3;  *op = OP_BITWISE_OR; return true;
        case TOKEN_BITWISE_XOR:   *prec = 4;  *op = OP_BITWISE_XOR;return true;
        case TOKEN_BITWISE_AND:   *prec = 5;  *op = OP_BITWISE_AND;return true;
        case TOKEN_EQUAL:         *prec = 6;  *op = OP_EQ;        return true;
        case TOKEN_NOT_EQUAL:     *prec = 6;  *op = OP_NEQ;       return true;
        case TOKEN_LESS:          *prec = 7;  *op = OP_LT;        return true;
        case TOKEN_GREATER:       *prec = 7;  *op = OP_GT;        return true;
        case TOKEN_LESS_EQUAL:    *prec = 7;  *op = OP_LTE;       return true;
        case TOKEN_GREATER_EQUAL: *prec = 7;  *op = OP_GTE;       return true;
        case TOKEN_IN:            *prec = 7;  *op = OP_IN;        return true;
        case TOKEN_LEFT_SHIFT:    *prec = 8;  *op = OP_LSHIFT;    return true;
        case TOKEN_RIGHT_SHIFT:   *prec = 8;  *op = OP_RSHIFT;    return true;
        case TOKEN_ADD:           *prec = 9;  *op = OP_ADD;       return true;
        case TOKEN_SUBTRACT:      *prec = 9;  *op = OP_SUB;       return true;
        case TOKEN_MULTIPLY:      *prec = 10; *op = OP_MUL;       return true;
        case TOKEN_DIVIDE:        *prec = 10; *op = OP_DIV;       return true;
        case TOKEN_MODULO:        *prec = 10; *op = OP_MOD;       return true;
        case TOKEN_FLOOR_DIVIDE:  *prec = 10; *op = OP_FLOOR_DIV; return true;
        case TOKEN_AS:            *prec = 11; *op = OP_CAST;      return true;
        default: return false;
    }
}

/* String literals with `{name}` interpolation: literal segments and interpolated values
   concatenate via OP_BINARY(OP_ADD) into registers, and a `{name}`'s value-to-string step goes
   through OP_UNARY's folded-in OP_TO_STR case. Interpolated names must already be a defined
   variable (var_lookup, non-creating) — there's no runtime scope-chain fallback to resolve
   against. */
static int parse_string_literal(Chunk* c) {
    AerString* ts    = aer_as_string(token.value);
    char*        s   = ts->data;
    unsigned int len = ts->length;

    bool has_interp = false;
    for (unsigned int k = 0; k < len; k++) {
        if (s[k] == '\\' && k + 1 < len) { k++; continue; }
        if (s[k] == '{') { has_interp = true; break; }
    }

    if (!has_interp) {
        unsigned int pool_idx = pool_escaped_string(c, s, len);
        lex();
        return (int)pool_idx | RK_CONST_FLAG;
    }

    int result = -1;   /* -1: no parts concatenated yet (a valid RK/register value is always >= 0) */
    unsigned int i = 0;
    while (i <= len) {
        /* Literal segment up to the next unescaped '{' or end. */
        unsigned int seg_start = i;
        while (i < len) {
            if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (s[i] == '{') break;
            i++;
        }
        if (i > seg_start) {
            int rk_seg = (int)pool_escaped_string(c, s + seg_start, i - seg_start) | RK_CONST_FLAG;
            if (result < 0) {
                result = materialize(c, rk_seg);
            } else {
                if (is_temp(result)) reg_free(1);
                int dest = reg_alloc();
                chunk_emit(c, PACK2(OP_BINARY, dest, (int)OP_ADD));
                chunk_emit(c, result); chunk_emit(c, rk_seg);
                result = dest;
            }
        }
        if (i >= len) break;

        /* Interpolation: {name} */
        i++;   /* skip '{' */
        unsigned int var_start = i;
        while (i < len && s[i] != '}') i++;
        if (i >= len) { error_at("Unclosed '{' in string"); break; }
        if (i == var_start) { error_at("Empty '{}' in string"); i++; continue; }

        unsigned int name_len = i - var_start;
        char* name_buf = xmalloc((size_t)name_len + 1);
        memcpy(name_buf, s + var_start, name_len);
        name_buf[name_len] = '\0';
        unsigned int name_pool_idx = chunk_add_pool(c, aer_make_string(name_buf, name_len));

        int var_reg;
        if (!var_lookup(name_pool_idx, &var_reg)) {
            error_at("'%.*s' is not defined (an interpolated name must already have a value)",
                     (int)name_len, s + var_start);
            i++;
            continue;
        }

        int str_dest = reg_alloc();
        chunk_emit(c, PACK2(OP_UNARY, str_dest, (int)OP_TO_STR)); chunk_emit(c, var_reg);

        if (result < 0) {
            result = str_dest;
        } else {
            if (is_temp(str_dest)) reg_free(1);
            if (is_temp(result))   reg_free(1);
            int dest = reg_alloc();
            chunk_emit(c, PACK2(OP_BINARY, dest, (int)OP_ADD));
            chunk_emit(c, result); chunk_emit(c, str_dest);
            result = dest;
        }
        i++;   /* skip '}' */
    }

    if (result < 0) {
        /* No parts at all — a fresh, owned empty buffer, not a static literal (AerString always
           owns its data). */
        char* empty_buf = xmalloc(1);
        empty_buf[0] = '\0';
        result = (int)chunk_add_pool(c, aer_make_string(empty_buf, 0)) | RK_CONST_FLAG;
    }

    lex();
    return result;
}

/* Literal/identifier/parenthesized/array-literal/dict-literal/call primary. Unary operators
   (`-x`/`!x`/`~x`) sit ABOVE this in the precedence chain — see parse_unary — matching the real
   grammar's parse_binary -> parse_unary -> parse_primary structure. */
static int parse_primary_inner(Chunk* c) {
    if (consume(TOKEN_IF)) return parse_if_expr(c);
    if (consume(TOKEN_FUNCTION)) return parse_function_expr(c);
    if (consume(TOKEN_OPEN_PARENTHESE)) {
        int rk = parse_binary(c, 0);
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after expression");
        return rk;
    }
    /* array literal. Elements share the same contiguous-register-materialization
       helper call arguments use (parse_contiguous_exprs), then OP_ARRAY_NEW (M4) — the
       result reuses the first element's register (freeing the rest), same compact-reuse
       convention as a call's result. */
    if (consume(TOKEN_OPEN_BRACKET)) {
        int item_reg_base;
        int item_count = parse_contiguous_exprs(c, TOKEN_CLOSE_BRACKET, &item_reg_base);
        require(TOKEN_CLOSE_BRACKET, "expected ']' after array literal");
        if (parse_had_error) return 0;
        int dest = (item_count > 0) ? item_reg_base : reg_alloc();
        if (item_count > 1) reg_free(item_count - 1);
        emit_array_new(c, dest, item_reg_base < 0 ? dest : item_reg_base, item_count);
        return dest;
    }
    /* Dict literal — not a bare comma list (each item is a key:value pair), so this doesn't reuse
       parse_contiguous_exprs; otherwise the exact same contiguous-materialize-then-bulk-copy
       shape, now via OP_DICT_NEW (M4). Key then value are materialized back to back, so
       pair i's key/value land at exactly pair_reg_base+2i/+2i+1, matching that opcode's layout. */
    if (consume(TOKEN_OPEN_BRACE)) {
        int pair_reg_base = -1;
        int pair_count = 0;
        if (!equal(TOKEN_CLOSE_BRACE)) {
            do {
                int rk_key  = parse_binary(c, 0);
                int reg_key = arg_materialize(c, rk_key);
                if (pair_count == 0) pair_reg_base = reg_key;
                require(TOKEN_COLON, "expected ':' after dict key");
                if (parse_had_error) return 0;
                int rk_val = parse_binary(c, 0);
                arg_materialize(c, rk_val);
                pair_count++;
            } while (consume(TOKEN_COMMA));
        }
        require(TOKEN_CLOSE_BRACE, "expected '}' after dict literal");
        if (parse_had_error) return 0;
        int dest = (pair_count > 0) ? pair_reg_base : reg_alloc();
        if (pair_count > 1) reg_free(2 * pair_count - 1);
        emit_dict_new(c, dest, pair_reg_base < 0 ? dest : pair_reg_base, pair_count);
        return dest;
    }
    if (token.type == TOKEN_INTEGER || token.type == TOKEN_REAL ||
        token.type == TOKEN_TRUE    || token.type == TOKEN_FALSE || token.type == TOKEN_NULL) {
        AerVal v;
        if      (token.type == TOKEN_INTEGER) v = aer_int(aer_as_int(token.value));
        else if (token.type == TOKEN_REAL)    v = aer_real(aer_as_real(token.value));
        else if (token.type == TOKEN_NULL)    v = aer_null();
        else                                   v = aer_bool(aer_as_bool(token.value));
        lex();
        return (int)chunk_add_pool(c, v) | RK_CONST_FLAG;
    }
    if (token.type == TOKEN_STRING) return parse_string_literal(c);
    if (at_module_name(c)) return parse_module_call(c);
    if (token.type == TOKEN_IDENTIFIER) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_OPEN_PARENTHESE)) return parse_call(c, name_idx);

        /* A name not among the CURRENT function's own locals may still be a top-level variable,
           readable via OP_LOAD_GLOBAL (see global_names's own comment). Checked only when
           var_lookup (non-creating) misses, so a local always shadows a global of the same
           name. */
        int reg;
        if (var_lookup(name_idx, &reg)) return reg;
        if (function_depth > 0) {
            int global_reg;
            if (global_lookup(name_idx, &global_reg)) {
                int dest = reg_alloc();
                chunk_emit(c, PACK2(OP_LOAD_GLOBAL, dest, global_reg));
                return dest;
            }
        }

        /* Functions as values (bare-name case): a name that isn't a variable/global but IS a
           known function, referenced here WITHOUT a following '(' (so this isn't a call at all —
           parse_call already claimed that case above), is a reference to the function itself as a
           value: `f = square` needs `square` to actually evaluate to something, not fall through
           to var_slot below and silently become a fresh, garbage-valued local. Built once per
           reference as an ordinary pool constant — chunk_add_pool already dedups identical
           TYPE_FUNCTION values by code_offset+arity, so repeated references to the same function
           share one AerFunction, not one each. */
        unsigned int func_offset, func_arity, func_min_arity, func_receiver_type;
        AerVal* func_defaults;
        bool func_has_receiver;
        if (func_full_lookup(name_idx, &func_offset, &func_arity, &func_min_arity, &func_defaults,
                                 &func_has_receiver, &func_receiver_type)) {
            AerVal fv = build_function_value(func_offset, func_arity, func_min_arity, func_defaults,
                                                 func_has_receiver, func_receiver_type);
            return (int)chunk_add_pool(c, fv) | RK_CONST_FLAG;
        }

        reg = var_slot(name_idx);   /* not a known local, global, or function — existing
                                           prototype permissiveness: silently creates a fresh local
                                           (unchanged) */
        return reg < 0 ? 0 : reg;      /* reg<0: error_at already called */
    }
    error_at("Expected an expression (only literals, variables, calls, array/dict literals, arithmetic/comparisons, and parentheses are supported)");
    return 0;
}

/* Postfix chain shared by parse_primary (a fresh primary expression) and
   parse_chain_assignment's fallback (an already-resolved chain step that turned out not to be
   an assignment target). `[index]`/`[a:b]` slice/`.field` reads, plus a trailing '(' — a call
   THROUGH whatever value the chain has produced so far, not a call BY NAME (that's parse_call's
   job, reached only when an identifier is IMMEDIATELY followed by '(', before this function ever
   runs) — covers functions stored in an array/dict and called via index/key (`ops[0](3, 4)`,
   `dispatch["add"](10, 20)`), and, as a free side effect, currying (`f()()`).
     `rk` is always reused in place as the call's own dest_reg (not allocated fresh above it) —
   safe because dest_reg is only WRITTEN after the callee's AerFunction has already been read out
   of that same register (lbl_call_value, vm.c) and because rk is guaranteed to be the topmost
   live temp here (this file's universal free-then-allocate discipline), so nothing above it can
   still be needed once the call's own (higher) argument registers are freed back down to it. */
static int parse_postfix_chain(Chunk* c, int rk) {
    for (;;) {
        if (consume(TOKEN_OPEN_PARENTHESE)) {
            int callee_reg = materialize(c, rk);
            int arg_reg_base;
            int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
            require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
            if (parse_had_error) return rk;
            int dest = callee_reg;
            int base = arg_reg_base < 0 ? dest : arg_reg_base;
            emit_call_value(c, dest, base, arg_count, callee_reg);
            if (arg_count > 0) reg_free(arg_count);
            rk = dest;
            continue;
        }
        if (consume(TOKEN_OPEN_BRACKET)) {
            /* `arr[a:b]`/`arr[a:]`/`arr[:b]`/`arr[:]`: a bare `:` right away means no start; a `]`
               right after the (optional) ':' means no end. Either missing bound compiles to an
               RK-encoded null constant rather than OP_SLICE_GET needing to know which bound was
               actually written. */
            bool has_start = !equal(TOKEN_COLON);
            int rk_start = has_start ? parse_binary(c, 0) : 0;

            if (!consume(TOKEN_COLON)) {
                require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
                if (parse_had_error) return rk;

                int arr_reg = materialize(c, rk);

                /* Free-then-allocate, matching parse_binary_ops's own discipline. */
                if (is_temp(rk_start)) reg_free(1);
                if (is_temp(arr_reg))  reg_free(1);

                int dest = reg_alloc();
                emit_index_get(c, dest, arr_reg, rk_start);
                rk = dest;
                continue;
            }

            int rk_slice_start = has_start ? rk_start : ((int)chunk_add_pool(c, aer_null()) | RK_CONST_FLAG);
            int rk_end;
            if (equal(TOKEN_CLOSE_BRACKET)) rk_end = (int)chunk_add_pool(c, aer_null()) | RK_CONST_FLAG;
            else                             rk_end = parse_binary(c, 0);
            require(TOKEN_CLOSE_BRACKET, "expected ']' after slice");
            if (parse_had_error) return rk;

            int arr_reg = materialize(c, rk);

            if (is_temp(rk_end))         reg_free(1);
            if (is_temp(rk_slice_start)) reg_free(1);
            if (is_temp(arr_reg))        reg_free(1);

            int dest = reg_alloc();
            emit_slice_get(c, dest, arr_reg, rk_slice_start, rk_end);
            rk = dest;
            continue;
        }
        /* Postfix '.field' read, same shape as '[...]' above: p.x and chained forms like p.pos.x
           work for free, since each step's result feeds back into the next. */
        if (consume(TOKEN_DOT)) {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return rk; }
            unsigned int field_idx = chunk_add_pool(c, token.value);
            lex();

            int struct_reg = materialize(c, rk);
            if (is_temp(struct_reg)) reg_free(1);

            int dest = reg_alloc();
            emit_field_get(c, dest, struct_reg, field_idx);
            rk = dest;
            continue;
        }
        break;
    }
    return rk;
}

/* A fresh primary expression, then its postfix chain (index/slice/field reads, and a trailing
   call-through-value) via the shared parse_postfix_chain above. */
static int parse_primary(Chunk* c) {
    return parse_postfix_chain(c, parse_primary_inner(c));
}

/* Unary operators: each recurses into parse_unary again (not parse_primary), so chained unary
   (`!!x`, `--x`, `~~x`) works, falling to parse_primary only once no more prefix operators apply.
   rk is RK-encoded like OP_BINARY's operands — OP_UNARY decodes it directly via vm_rk_value. */
static int parse_unary_inner(Chunk* c) {
    Opcode unary_op;
    if      (consume(TOKEN_NOT))         unary_op = OP_NOT;
    else if (consume(TOKEN_BITWISE_NOT)) unary_op = OP_BITWISE_NOT;
    else if (consume(TOKEN_SUBTRACT))    unary_op = OP_NEGATE;
    else return parse_primary(c);

    int rk = parse_unary(c);
    if (is_temp(rk)) reg_free(1);   /* free-then-allocate, matching every other site */
    int dest = reg_alloc();
    chunk_emit(c, PACK2(OP_UNARY, dest, (int)unary_op));
    chunk_emit(c, rk);
    return dest;
}

static int parse_unary(Chunk* c) {
    return parse_unary_inner(c);
}

/* `lhs and rhs`, short-circuit: rhs is only evaluated if lhs is truthy. Uses only
   emit_jump_if_false_reg (no dedicated opcode) — both the lhs-false and rhs-false paths land
   on the same "result = false" code, since either one alone is enough to decide the outcome.
   `dest` is allocated once, after both operands' temps are freed, and both outcome paths
   (true/false) write into that same register — safe because only one path ever executes at
   runtime. */
static int compile_and(Chunk* c, int lhs, unsigned int prec) {
    int reg_lhs = materialize(c, lhs);
    unsigned int patch_false_a = emit_jump_if_false_reg(c, reg_lhs);
    if (is_temp(reg_lhs)) reg_free(1);

    int rk_rhs = parse_binary(c, prec);
    int reg_rhs = materialize(c, rk_rhs);
    unsigned int patch_false_b = emit_jump_if_false_reg(c, reg_rhs);
    if (is_temp(reg_rhs)) reg_free(1);

    int dest = reg_alloc();
    unsigned int pool_true = chunk_add_pool(c, aer_bool(true));
    chunk_emit(c, PACK1(OP_LOADK, dest)); chunk_emit(c, (int)pool_true);
    chunk_emit(c, OP_JUMP);
    unsigned int patch_end = c->count; chunk_emit(c, 0);

    patch_jump(c, patch_false_a, c->count);
    patch_jump(c, patch_false_b, c->count);
    unsigned int pool_false = chunk_add_pool(c, aer_bool(false));
    chunk_emit(c, PACK1(OP_LOADK, dest)); chunk_emit(c, (int)pool_false);

    patch_jump(c, patch_end, c->count);
    return dest;
}

/* `lhs or rhs`, short-circuit: rhs is only evaluated if lhs is falsy. The stack VM's
   OP_OR has a dedicated OP_JUMP_IF_TRUE for this shape; v3 has no jump-if-true opcode, so this is
   restructured to use only jump-if-false — lhs-false jumps forward into a "check rhs" block
   instead of jumping past a jump-if-true, otherwise the same "one dest, two outcome paths"
   discipline as compile_and. */
static int compile_or(Chunk* c, int lhs, unsigned int prec) {
    int reg_lhs = materialize(c, lhs);
    unsigned int patch_check_rhs = emit_jump_if_false_reg(c, reg_lhs);
    if (is_temp(reg_lhs)) reg_free(1);

    int dest = reg_alloc();
    unsigned int pool_true = chunk_add_pool(c, aer_bool(true));
    chunk_emit(c, PACK1(OP_LOADK, dest)); chunk_emit(c, (int)pool_true);
    chunk_emit(c, OP_JUMP);
    unsigned int patch_end_a = c->count; chunk_emit(c, 0);

    patch_jump(c, patch_check_rhs, c->count);
    int rk_rhs = parse_binary(c, prec);
    int reg_rhs = materialize(c, rk_rhs);
    unsigned int patch_result_false = emit_jump_if_false_reg(c, reg_rhs);
    if (is_temp(reg_rhs)) reg_free(1);

    chunk_emit(c, PACK1(OP_LOADK, dest)); chunk_emit(c, (int)pool_true);
    chunk_emit(c, OP_JUMP);
    unsigned int patch_end_b = c->count; chunk_emit(c, 0);

    patch_jump(c, patch_result_false, c->count);
    unsigned int pool_false = chunk_add_pool(c, aer_bool(false));
    chunk_emit(c, PACK1(OP_LOADK, dest)); chunk_emit(c, (int)pool_false);

    patch_jump(c, patch_end_a, c->count);
    patch_jump(c, patch_end_b, c->count);
    return dest;
}

/* `x |> f(args)` desugars to f(x, args): the piped value becomes argument zero, ahead of
   whatever's inside the parentheses. Reuses parse_call's own function/struct resolution and
   arg_materialize's contiguous-register discipline directly rather than duplicating it — the
   only new part is materializing `lhs` into the argument-zero slot before parsing the rest of
   the list. */
static int compile_pipe(Chunk* c, int lhs) {
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after '|>'"); return lhs; }

    /* Module-qualified pipe target: `x |> module.fn(args)` desugars to module.fn(x, args), same
       "piped value becomes argument zero" idea as the bare-name case below, just routed through
       OP_CALL_MODULE instead of OP_CALL/OP_STRUCT_NEW. Checked via chunk_is_imported BEFORE
       consuming the identifier so a module name is never mistaken for an ordinary function/struct
       name below. */
    if (chunk_is_imported(c, aer_as_string(token.value)->data, aer_as_string(token.value)->length)) {
        unsigned int module_idx = chunk_add_pool(c, token.value);
        lex();
        require(TOKEN_DOT, "expected '.' after module name");
        if (parse_had_error) return lhs;
        if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after '.'"); return lhs; }
        unsigned int fn_idx = chunk_add_pool(c, token.value);
        lex();
        require(TOKEN_OPEN_PARENTHESE, "expected '(' after piped module function name");
        if (parse_had_error) return lhs;

        if (is_temp(lhs)) reg_free(1);
        int arg_reg_base = arg_materialize(c, lhs);
        int arg_count = 1;
        if (!equal(TOKEN_CLOSE_PARENTHESE)) {
            do {
                int rk = parse_binary(c, 0);
                arg_materialize(c, rk);
                arg_count++;
            } while (consume(TOKEN_COMMA));
        }
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after pipe call arguments");
        if (parse_had_error) return lhs;

        int dest = arg_reg_base;
        if (arg_count > 1) reg_free(arg_count - 1);
        chunk_emit(c, PACK3(OP_CALL_MODULE, dest, arg_reg_base, arg_count));
        chunk_emit(c, (int)module_idx);
        chunk_emit(c, (int)fn_idx);
        return dest;
    }

    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();

    if (equal(TOKEN_DOT)) {
        error_at("Unknown module (must be imported before use)");
        return lhs;
    }

    bool is_struct = is_struct_name(name_idx);
    unsigned int func_offset = 0;
    if (!is_struct && !func_lookup(name_idx, &func_offset)) {
        error_at("Unknown function or struct type (must be defined before use)");
        return lhs;
    }

    require(TOKEN_OPEN_PARENTHESE, "expected '(' after piped function name");
    if (parse_had_error) return lhs;

    if (is_temp(lhs)) reg_free(1);
    int arg_reg_base = arg_materialize(c, lhs);
    int arg_count = 1;
    if (!equal(TOKEN_CLOSE_PARENTHESE)) {
        do {
            int rk = parse_binary(c, 0);
            arg_materialize(c, rk);
            arg_count++;
        } while (consume(TOKEN_COMMA));
    }
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after pipe call arguments");
    if (parse_had_error) return lhs;

    int dest = arg_reg_base;
    if (arg_count > 1) reg_free(arg_count - 1);
    if (is_struct) emit_struct_new(c, dest, name_idx, arg_reg_base, arg_count);
    else           emit_call(c, dest, func_offset, arg_reg_base, arg_count);
    return dest;
}

/* Precedence-climbing loop, `lhs` already parsed — split from parse_binary so a for-while
   condition that starts with an identifier can resolve that identifier once (see
   parse_for_while) and climb from there. */
static int parse_binary_ops(Chunk* c, unsigned int min_prec, int lhs, unsigned int lhs_start) {
    for (;;) {
        unsigned int prec;
        Opcode op;
        if (!binary_op_info(token.type, &prec, &op) || prec <= min_prec) break;
        lex();

        if (op == OP_AND)  { lhs = compile_and(c, lhs, prec); lhs_start = c->count; continue; }
        if (op == OP_OR)   { lhs = compile_or(c, lhs, prec);  lhs_start = c->count; continue; }
        if (op == OP_PIPE) { lhs = compile_pipe(c, lhs);      lhs_start = c->count; continue; }

        /* `x as T` — T is a bare type name, read directly rather than through parse_binary.
           Struct-shape casting (`x as Point`) routes to OP_CHECK_SHAPE (its own comment in
           vm.h/vm.c) — checked via is_struct_name BEFORE the primitive-name checks below, since a
           struct type name and "string"/"integer"/"float"/"boolean" can never collide (checked
           against two disjoint tables, regardless of capitalization convention). */
        if (op == OP_CAST) {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a type name after 'as'"); return lhs; }
            unsigned int type_name_idx = chunk_add_pool(c, token.value);
            const char* type_name = aer_as_string(token.value)->data;
            unsigned int type_len = aer_as_string(token.value)->length;
            lex();

            if (is_temp(lhs)) reg_free(1);
            int dest = reg_alloc();

            if (is_struct_name(type_name_idx)) {
                chunk_emit(c, PACK2(OP_CHECK_SHAPE, dest, lhs));
                chunk_emit(c, (int)type_name_idx);
            } else if (type_len == 6 && strncmp(type_name, "string", 6) == 0) {
                chunk_emit(c, PACK2(OP_UNARY, dest, (int)OP_TO_STR));
                chunk_emit(c, lhs);
            } else {
                int cast_type;
                if      (type_len == 7 && strncmp(type_name, "integer", 7) == 0) cast_type = CAST_INTEGER;
                else if (type_len == 5 && strncmp(type_name, "float",   5) == 0) cast_type = CAST_FLOAT;
                else if (type_len == 7 && strncmp(type_name, "boolean", 7) == 0) cast_type = CAST_BOOLEAN;
                else {
                    error_at("Unknown type '%.*s' in 'as' cast (must be string/integer/float/boolean, or a known struct type)",
                             (int)type_len, type_name);
                    return dest;
                }
                chunk_emit(c, PACK2(OP_CAST, dest, cast_type));
                chunk_emit(c, lhs);
            }
            lhs = dest;
            lhs_start = c->count;
            continue;
        }

        /* Fusion — see OP_FIELD_BINARY's own comment in vm.h. Checked BEFORE parsing the RHS,
           while lhs's own bytecode (if it was a bare `struct.field` read) is still the last thing
           in the chunk — once RHS is parsed, its bytecode would sit after lhs's, and excising a
           chunk from the MIDDLE of the bytecode stream would risk invalidating any jump target RHS
           itself emitted (short-circuit and/or, nested calls, ...). Truncating here, before RHS
           exists at all, avoids that entirely — same "only ever discard from the tail" invariant
           the RHS-is-field fusion below relies on. */
        bool lhs_is_field = (c->count - lhs_start == 2 && (c->code[lhs_start] & 0xFF) == OP_FIELD_GET);
        int lhs_struct_reg = 0;
        unsigned int lhs_field_idx = 0;
        if (lhs_is_field) {
            lhs_struct_reg = UNPACK_B(c->code[lhs_start]);
            lhs_field_idx  = (unsigned int)c->code[lhs_start + 1];
            c->count = lhs_start;   /* discard lhs's OP_FIELD_GET, never executed */
        }

        unsigned int rhs_start = c->count;
        int rhs = parse_binary(c, prec);   /* same precedence as floor -> left-associative */

        if (lhs_is_field) {
            if (is_temp(rhs)) reg_free(1);
            if (is_temp(lhs)) reg_free(1);

            int dest = reg_alloc();
            chunk_emit(c, PACK3(OP_FIELD_BINARY, dest, lhs_struct_reg, (int)op));
            chunk_emit(c, (int)lhs_field_idx);
            chunk_emit(c, rhs);
            lhs = dest;
            lhs_start = c->count;
            continue;
        }

        /* Fusion — see OP_BINARY_FIELD's own comment in vm.h. Found via a real per-opcode
           dispatch audit on nbody.aer: `x OP y.field` (e.g. `dx = bix - bj.x`) compiled as a bare
           OP_FIELD_GET immediately followed by this function's own OP_BINARY reading its
           result back out of a register — two dispatches for one operation. Recognized here by
           checking whether the RHS we just compiled was EXACTLY one bare `struct.field` read
           (a single OP_FIELD_GET, 4 words, nothing chained after it) — if so, discard that
           instruction (never executed) and re-encode its two operands as this fused opcode's
           trailing operands instead. */
        if (c->count - rhs_start == 2 && (c->code[rhs_start] & 0xFF) == OP_FIELD_GET) {
            int struct_reg = UNPACK_B(c->code[rhs_start]);
            int field_idx  = c->code[rhs_start + 1];
            c->count = rhs_start;   /* discard the OP_FIELD_GET just emitted, never executed */

            if (is_temp(rhs)) reg_free(1);
            if (is_temp(lhs)) reg_free(1);

            int dest = reg_alloc();
            chunk_emit(c, PACK3(OP_BINARY_FIELD, dest, struct_reg, (int)op));
            chunk_emit(c, lhs);
            chunk_emit(c, field_idx);
            lhs = dest;
            lhs_start = c->count;
            continue;
        }

        /* Free-then-allocate, RHS then LHS, matching compile_node's own discipline exactly. */
        if (is_temp(rhs)) reg_free(1);
        if (is_temp(lhs)) reg_free(1);

        int dest = reg_alloc();
        chunk_emit(c, PACK2(OP_BINARY, dest, (int)op));
        chunk_emit(c, lhs);
        chunk_emit(c, rhs);
        lhs = dest;
        lhs_start = c->count;
    }
    return lhs;
}

static int parse_binary(Chunk* c, unsigned int min_prec) {
    unsigned int lhs_start = c->count;
    int lhs = parse_unary(c);
    return parse_binary_ops(c, min_prec, lhs, lhs_start);
}

/* All 11 compound-assign operators. No fusion or new opcode needed: `x OP= expr` just compiles to
   one OP_BINARY with dest == lhs == x's own register — already as efficient as it gets in a
   register machine. */
static const struct { TokenType tok; Opcode op; } compound_assign_ops[] = {
    { TOKEN_ADD_ASSIGN,          OP_ADD         },
    { TOKEN_SUBTRACT_ASSIGN,     OP_SUB         },
    { TOKEN_MULTIPLY_ASSIGN,     OP_MUL         },
    { TOKEN_DIVIDE_ASSIGN,       OP_DIV         },
    { TOKEN_MODULO_ASSIGN,       OP_MOD         },
    { TOKEN_FLOOR_DIVIDE_ASSIGN, OP_FLOOR_DIV   },
    { TOKEN_LEFT_SHIFT_ASSIGN,   OP_LSHIFT      },
    { TOKEN_RIGHT_SHIFT_ASSIGN,  OP_RSHIFT      },
    { TOKEN_AND_ASSIGN,          OP_BITWISE_AND },
    { TOKEN_OR_ASSIGN,           OP_BITWISE_OR  },
    { TOKEN_XOR_ASSIGN,          OP_BITWISE_XOR },
};
#define COMPOUND_ASSIGN_OP_COUNT (int)(sizeof(compound_assign_ops) / sizeof(*compound_assign_ops))

/* `name = expr` or `name OP= expr` — no indexed/field targets (out of scope, same limitation
   indexed/field writes already have). `name` is already consumed by the caller
   (parse_statement), which must decide between this, a bare call statement, and an indexed
   write first — all start with an identifier, and only one token of lookahead distinguishes them,
   so the identifier can't be re-consumed here. */
static void parse_assignment(Chunk* c, unsigned int name_idx) {
    /* Destructuring: `a, b, c = expr` or `a, b = x, y` (implicit array RHS). Reuses existing
       machinery instead of a dedicated OP_UNPACK: multiple RHS values are packed into a real array
       (OP_ARRAY_NEW, same as any array literal) and a single RHS expression is assumed to already
       evaluate to one; every target then reads its own index back out via OP_INDEX_GET, which
       gives the exact same runtime bounds-checking behavior as OP_UNPACK for free.
         Target registers are resolved via var_slot BEFORE the RHS is parsed — same ordering
       requirement parse_for_in's loop variable has (see its own comment): temps and permanent
       variables share one register-numbering space, so creating a brand-new variable AFTER a temp
       is already live could hand that temp's own register out from under it. */
    if (equal(TOKEN_COMMA)) {
        unsigned int names[MAX_DESTRUCT];
        names[0] = name_idx;
        unsigned int count = 1;
        while (consume(TOKEN_COMMA)) {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected identifier in destructuring"); return; }
            if (count >= MAX_DESTRUCT) { error_at("Too many destructuring targets (max %d)", MAX_DESTRUCT); return; }
            names[count++] = chunk_add_pool(c, token.value);
            lex();
        }
        require(TOKEN_ASSIGN, "expected '=' after destructuring targets");
        if (parse_had_error) return;

        int target_regs[MAX_DESTRUCT];
        for (unsigned int i = 0; i < count; i++) {
            target_regs[i] = var_slot(names[i]);
            if (target_regs[i] < 0) return;   /* error_at already called */
        }

        int rk_first = parse_binary(c, 0);
        if (parse_had_error) return;
        int rhs_reg_base = arg_materialize(c, rk_first);
        unsigned int rhs_count = 1;
        while (consume(TOKEN_COMMA)) {
            int rk_next = parse_binary(c, 0);
            if (parse_had_error) return;
            arg_materialize(c, rk_next);
            rhs_count++;
        }

        int arr_reg = rhs_reg_base;
        if (rhs_count > 1) {
            reg_free((int)rhs_count - 1);
            emit_array_new(c, arr_reg, rhs_reg_base, (int)rhs_count);
        }

        for (unsigned int i = 0; i < count; i++) {
            unsigned int pool_i = chunk_add_pool(c, aer_int((long long)i));
            emit_index_get(c, target_regs[i], arr_reg, (int)pool_i | RK_CONST_FLAG);
        }
        reg_free(1);   /* arr_reg — always a temp, guaranteed by arg_materialize */
        return;
    }

    if (consume(TOKEN_ASSIGN)) {
        int rk_val = parse_binary(c, 0);
        if (parse_had_error) return;
        int reg = var_slot(name_idx);
        if (reg < 0) return;   /* error_at already called */
        if (rk_val & RK_CONST_FLAG) {
            chunk_emit(c, PACK1(OP_LOADK, reg)); chunk_emit(c, rk_val & ~RK_CONST_FLAG);
        } else if (reg != rk_val) {
            chunk_emit(c, PACK2(OP_MOVE, reg, rk_val));
            /* Checked AFTER var_slot (which may have just reserved `reg`, raising the floor) so
               this correctly recognizes rk_val as no-longer-a-temp in the (common) case where a
               brand-new variable's assigned register happens to already be the exact temp its RHS
               computed into. */
            if (is_temp(rk_val)) reg_free(1);
        }
        /* reg == rk_val: the RHS's result temp already landed exactly where var_slot just
           reserved this new variable's own register (the common case for any single-result
           expression). Copying a register onto itself is a no-op, so skip the MOVE entirely —
           var_slot has already advanced reserved_floor/next_temp_register past `reg`, so no
           separate free is needed here either. */
        return;
    }

    for (int i = 0; i < COMPOUND_ASSIGN_OP_COUNT; i++) {
        if (!consume(compound_assign_ops[i].tok)) continue;

        /* Non-creating lookup — compound assignment to a name with no prior value has no
           sensible register to read from, so it's a compile-time error here. A name that isn't a
           local of the current function but IS an existing top-level global falls back to a
           global read-modify-write (OP_LOAD_GLOBAL + OP_BINARY + OP_STORE_GLOBAL) instead of
           erroring — see OP_STORE_GLOBAL's own comment in vm.h. */
        int reg;
        bool is_global = false;
        if (!var_lookup(name_idx, &reg)) {
            if (!(function_depth > 0 && global_lookup(name_idx, &reg))) {
                error_at("Compound assignment target must already have a value (no assigning to an undefined name this way)");
                return;
            }
            is_global = true;
        }

        if (is_global) {
            int local_reg = reg_alloc();
            chunk_emit(c, PACK2(OP_LOAD_GLOBAL, local_reg, reg));

            int rk_rhs = parse_binary(c, 0);
            if (parse_had_error) return;
            chunk_emit(c, PACK2(OP_BINARY, local_reg, (int)compound_assign_ops[i].op));
            chunk_emit(c, local_reg);
            chunk_emit(c, rk_rhs);
            if (is_temp(rk_rhs)) reg_free(1);

            chunk_emit(c, PACK1(OP_STORE_GLOBAL, reg));
            chunk_emit(c, local_reg);
            reg_free(1);   /* local_reg */
            return;
        }

        int rk_rhs = parse_binary(c, 0);
        if (parse_had_error) return;
        chunk_emit(c, PACK2(OP_BINARY, reg, (int)compound_assign_ops[i].op));
        chunk_emit(c, reg);
        chunk_emit(c, rk_rhs);
        if (is_temp(rk_rhs)) reg_free(1);
        return;
    }

    /* A pipe chain starting from a bare name, used as a STATEMENT (result discarded), e.g.
       `numbers_list |> pipe_stmt_append("touched")`. `name_idx` must already have a value (this is
       a read, not a definition) — non-creating lookup, same stance compound assignment above
       takes. */
    if (equal(TOKEN_PIPE)) {
        int reg;
        unsigned int lhs_start = c->count;
        if (!var_lookup(name_idx, &reg)) {
            if (function_depth > 0 && global_lookup(name_idx, &reg)) {
                int dest = reg_alloc();
                chunk_emit(c, PACK2(OP_LOAD_GLOBAL, dest, reg));
                reg = dest;
            } else {
                error_at("'%s' is not defined (a pipe chain's source must already have a value)",
                         aer_as_string(c->pool[name_idx])->data);
                return;
            }
        }
        int rk_result = parse_binary_ops(c, 0, reg, lhs_start);
        discard_statement_result(c, rk_result);
        return;
    }

    error_at("Only plain 'name = expr' or compound assignment is supported here (no field access)");
}

/* `name(.field | [idx])+ = / OP= expr`, arbitrary chain depth (`bodies[i].pos[0].x = v`, etc.):
   every step but the last resolves as a GET, and only the step still pending when the loop below
   exits becomes the final SET (or a fused compound read-modify-write).

   Register handling: every GET after the FIRST hop overwrites its own source register in place
   (dest_reg == the register just read from) instead of allocating a fresh temp — safe because
   OP_FIELD_GET/OP_INDEX_GET's runtime handlers already read their source register's value into a
   local before writing dest. It also sidesteps a real constraint of this file's register
   allocator: reg_free() is a pure LIFO stack-pointer decrement (no per-register bookkeeping), so
   an OLDER temp can never be freed while a NEWER one must stay live — which a naive "allocate a
   fresh dest_reg every hop" approach would do the moment a chain mixes an index step's own temp
   expression with a later hop. The only two cases that must still allocate a genuinely fresh
   register: the very first hop off `name`'s own PERMANENT variable register (which must never be
   overwritten), and — within that first hop — a leading index expression that isn't itself a temp
   (a bare variable, RK-const): reused in place when it IS a temp (dest_reg = pending_rk_idx
   directly), sidestepping the free-ordering hazard rather than triggering it. */
static void parse_chain_assignment(Chunk* c, unsigned int name_idx, bool first_is_index) {
    int obj_reg;
    bool obj_is_base;   /* true while obj_reg is still name_idx's own permanent register */
    if (var_lookup(name_idx, &obj_reg)) {
        obj_is_base = true;
    } else if (function_depth > 0 && global_lookup(name_idx, &obj_reg)) {
        /* `name` isn't a local of the current function but IS an existing top-level global — this
           is `name[idx] = v`/`name.field = v`, a READ of whatever `name` already refers to
           (arrays/structs are heap references — mutating through ANY register holding a copy of
           that same reference correctly mutates the one underlying object), never a definition of
           `name` itself. Calling var_slot (creating) unconditionally here would, on a miss,
           silently define a fresh, garbage-valued local shadowing the global instead of reading
           it. Loads the global into a fresh temp (obj_is_base = false) instead — treated exactly
           like any other non-base chain temp by every consumer below (safe to overwrite in place
           for a later hop, freed at the very end). */
        int global_reg = obj_reg;
        obj_reg = reg_alloc();
        chunk_emit(c, PACK2(OP_LOAD_GLOBAL, obj_reg, global_reg));
        obj_is_base = false;
    } else {
        error_at("'%s' is not defined (an indexed/field write target must already have a value)",
                 aer_as_string(c->pool[name_idx])->data);
        return;
    }

    bool pending_is_field = !first_is_index;
    unsigned int pending_field_idx = 0;
    int pending_rk_idx = 0;

    if (first_is_index) {
        pending_rk_idx = parse_binary(c, 0);
        require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
    } else {
        if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return; }
        pending_field_idx = chunk_add_pool(c, token.value);
        lex();
    }
    if (parse_had_error) return;

    while (equal(TOKEN_OPEN_BRACKET) || equal(TOKEN_DOT)) {
        bool reuse_pending_idx_reg = !pending_is_field && is_temp(pending_rk_idx);
        int dest_reg;
        if (!obj_is_base)              dest_reg = obj_reg;         /* reuse the chain's running temp in place */
        else if (reuse_pending_idx_reg) dest_reg = pending_rk_idx;  /* reuse the (dying) index temp's register */
        else                             dest_reg = reg_alloc(); /* first hop, nothing safe to reuse */

        if (pending_is_field) emit_field_get(c, dest_reg, obj_reg, pending_field_idx);
        else                   emit_index_get(c, dest_reg, obj_reg, pending_rk_idx);

        /* Only free the index temp when it occupies a DIFFERENT register than the one just reused
           as dest_reg — otherwise it was already folded into dest_reg above and there is nothing
           left to give back. When it IS a separate register, it was allocated more recently than
           obj_reg (parsed just this iteration, after obj_reg was already established), so it's
           genuinely the top of the stack here — a safe LIFO free. */
        if (reuse_pending_idx_reg && dest_reg != pending_rk_idx) reg_free(1);

        obj_reg = dest_reg;
        obj_is_base = false;

        if (consume(TOKEN_OPEN_BRACKET)) {
            pending_is_field = false;
            pending_rk_idx = parse_binary(c, 0);
            require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
        } else {
            consume(TOKEN_DOT);
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return; }
            pending_field_idx = chunk_add_pool(c, token.value);
            pending_is_field = true;
            lex();
        }
        if (parse_had_error) return;
    }

    if (consume(TOKEN_ASSIGN)) {
        int rk_val = parse_binary(c, 0);
        if (parse_had_error) return;

        if (pending_is_field) emit_field_set(c, obj_reg, pending_field_idx, rk_val);
        else                   emit_index_set(c, obj_reg, pending_rk_idx, rk_val);

        if (is_temp(rk_val)) reg_free(1);
        if (!pending_is_field && is_temp(pending_rk_idx)) reg_free(1);
        if (!obj_is_base) reg_free(1);
        return;
    }

    /* Compound assignment to the chain's final step: read-modify-write via the same fusion
       opcodes the single-level cases already used (OP_FIELD_BINARY for a field target,
       OP_BINARY-after-OP_INDEX_GET for an index target — no OP_INDEX_BINARY fusion
       exists, mirroring that this file never built one for the single-level case either). */
    for (int i = 0; i < COMPOUND_ASSIGN_OP_COUNT; i++) {
        if (!consume(compound_assign_ops[i].tok)) continue;

        if (pending_is_field) {
            int field_reg = reg_alloc();

            int rk_rhs = parse_binary(c, 0);
            if (parse_had_error) return;

            chunk_emit(c, PACK3(OP_FIELD_BINARY, field_reg, obj_reg, (int)compound_assign_ops[i].op));
            chunk_emit(c, (int)pending_field_idx);
            chunk_emit(c, rk_rhs);
            if (is_temp(rk_rhs)) reg_free(1);

            emit_field_set(c, obj_reg, pending_field_idx, field_reg);
            reg_free(1);   /* field_reg */
        } else {
            int item_reg = reg_alloc();
            emit_index_get(c, item_reg, obj_reg, pending_rk_idx);

            int rk_rhs = parse_binary(c, 0);
            if (parse_had_error) return;

            chunk_emit(c, PACK2(OP_BINARY, item_reg, (int)compound_assign_ops[i].op));
            chunk_emit(c, item_reg);
            chunk_emit(c, rk_rhs);
            if (is_temp(rk_rhs)) reg_free(1);

            emit_index_set(c, obj_reg, pending_rk_idx, item_reg);
            reg_free(1);   /* item_reg */
        }

        if (!pending_is_field && is_temp(pending_rk_idx)) reg_free(1);
        if (!obj_is_base) reg_free(1);
        return;
    }

    /* Not a plain or compound assignment: finish the pending step as a GET and fall through to a
       general expression statement — a call on the retrieved value (`queue[i]()`), further
       postfix chaining, and/or a trailing pipe (`basket.items |> pipe_stmt_append(...)`),
       discarded like any other expression statement. Anything else here is a genuine parse
       error. */
    if (!equal(TOKEN_OPEN_PARENTHESE) && !equal(TOKEN_PIPE)) {
        error_at("Expected an assignment ('='), a compound assignment ('+=' etc.), or a call/pipe continuation after this chain");
        return;
    }

    {
        bool reuse_pending_idx_reg = !pending_is_field && is_temp(pending_rk_idx);
        int dest_reg;
        if (!obj_is_base)               dest_reg = obj_reg;
        else if (reuse_pending_idx_reg) dest_reg = pending_rk_idx;
        else                             dest_reg = reg_alloc();

        if (pending_is_field) emit_field_get(c, dest_reg, obj_reg, pending_field_idx);
        else                   emit_index_get(c, dest_reg, obj_reg, pending_rk_idx);

        if (reuse_pending_idx_reg && dest_reg != pending_rk_idx) reg_free(1);

        int rk = parse_postfix_chain(c, dest_reg);
        unsigned int lhs_start = c->count;
        rk = parse_binary_ops(c, 0, rk, lhs_start);
        discard_statement_result(c, rk);
    }
}

/* Nested-block error recovery. Set just before parse_block returns (it always exits at a fresh
   statement boundary: EOF/ELSE/DEDENT) — lets an outer recovery loop tell "still mid-statement"
   from "a nested block already recovered here", or it would skip past the boundary a nested
   recovery already found and discard the NEXT statement along with the junk. */
static bool recovered_at_boundary = false;

/* Set whenever ANY statement — top-level or nested inside an if/for/function body — needed
   error recovery, even though parse_block/parse()'s own per-statement `parse_had_error` gets
   reset to false right after a successful recovery (so the recovery doesn't also roll back the
   enclosing statement/block that already recovered internally). Without this separate sticky
   flag, a script whose ONLY error is fully recovered inside a nested block (e.g. a malformed
   field access inside a function body that's never called) left `parse_had_error` false and
   `aer_had_error()` silently reporting success despite a real syntax error having been printed.
   Reset once per parse() call, never by parse_block. */
static bool any_compile_error = false;

/* Called by both parse_block's own recovery and parse's top-level recovery after their
   skip-to-boundary loop stops at a plain NEWLINE (not already a DEDENT/EOF boundary): a statement
   that failed to properly introduce a block (a mistyped 'for', say) leaves an orphaned indented
   block neither loop has a case for; left alone it cascades into one spurious "Expected a
   statement" per line instead of just the one error that explains the actual mistake. */
static void skip_orphaned_block(void) {
    if (!equal(TOKEN_NEW_LINE)) return;
    lex();
    if (!equal(TOKEN_INDENT)) return;
    int depth = 0;
    do {
        if (equal(TOKEN_INDENT)) depth++;
        else if (equal(TOKEN_DEDENT)) depth--;
        lex();
    } while (depth > 0 && !equal(TOKEN_END_OF_FILE));
}

/* NEWLINE+INDENT, statements until DEDENT/EOF. Per-statement rollback/skip-to-boundary recovery: a
   compile error inside an if/for/function body rolls back just that one statement's bytecode and
   resumes at the next line/dedent boundary, instead of aborting this whole block's (and
   everything after it in the enclosing statement's) compilation. */
static void parse_block(Chunk* c) {
    require(TOKEN_NEW_LINE, "expected newline before indented block");
    require(TOKEN_INDENT,   "expected indented block");
    if (parse_had_error) return;
    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_ELSE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        parse_had_error          = false;
        recovered_at_boundary = false;
        unsigned int saved       = c->count;
        unsigned int saved_marks = c->line_mark_count;
        chunk_mark_line(c, saved, current_source_line());
        parse_statement(c);
        if (parse_had_error) {
            any_compile_error  = true;
            c->count           = saved;
            c->line_mark_count = saved_marks;
            if (!recovered_at_boundary) {
                while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_ELSE) &&
                       !equal(TOKEN_DEDENT)       && !equal(TOKEN_NEW_LINE))
                    lex();
                skip_orphaned_block();
            }
        }
    }
    consume(TOKEN_DEDENT);
    /* However this loop exited (EOF/ELSE, or DEDENT just consumed), the lexer now sits at a fresh
       statement boundary regardless of recovery — same unconditional set parse_compound does. */
    recovered_at_boundary = true;
    /* If the LAST statement in this block failed and was recovered (rolled back + resynced to a
       boundary, right above), parse_had_error is left set from that statement and was never
       reset — the while loop's own condition just happened to become false before a fresh
       iteration could reset it. Every caller of parse_block checks parse_had_error immediately
       after calling this to decide whether ITS OWN enclosing construct failed, so a stale `true`
       here would make a block that had already fully recovered internally look like it failed
       too. For a function definition this is especially serious: func_register already ran
       (before the body compiled, to support self-recursion), so the caller's own recovery would
       roll `c->count` back to before the whole `function name(...):` statement even started —
       discarding the function's real bytecode while its name stays registered, pointing at
       whatever unrelated code later statements happen to compile at that same now-reused offset,
       which can manifest as a genuine infinite loop when that function is called. Resetting here
       means only a genuine top-level failure (require(NEWLINE)/require(INDENT) failing, the early
       return above) still propagates parse_had_error to the caller. */
    parse_had_error = false;
}

static void parse_if(Chunk* c) {
    int rk_cond = parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after if condition");
    /* require() can't abort us on failure (a known gap in parser.c's own parse_if too — see the
       "AER require() no-abort bug" project notes) — without this, a malformed condition still
       gets a real branch compiled in. */
    if (parse_had_error) return;

    int reg_cond = materialize(c, rk_cond);
    unsigned int patch_jif = emit_jump_if_false_reg(c, reg_cond);
    if (is_temp(reg_cond)) reg_free(1);

    parse_block(c);
    if (parse_had_error) return;

    if (consume(TOKEN_ELSE)) {
        require(TOKEN_COLON, "expected ':' after else");
        if (parse_had_error) return;
        chunk_emit(c, OP_JUMP);
        unsigned int patch_jmp = c->count;
        chunk_emit(c, 0);
        patch_jump(c, patch_jif, c->count);
        parse_block(c);
        patch_jump(c, patch_jmp, c->count);
    } else {
        patch_jump(c, patch_jif, c->count);
    }
}

/* Inline if-expression: `if cond: then_val else: else_val` (no newline after ':', unlike the
   statement form parse_if above — this is an EXPRESSION, reached only from parse_primary_inner).
   "No else means null". Both branches must converge on ONE result register, so result_reg is
   allocated BEFORE either branch is compiled, then each branch either LOADKs a constant into it
   or MOVEs a mismatched register into it (skipping the MOVE when it's already the right
   register). */
static int parse_if_expr(Chunk* c) {
    int rk_cond = parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after if condition");
    if (parse_had_error) return 0;

    int reg_cond = materialize(c, rk_cond);
    unsigned int patch_jif = emit_jump_if_false_reg(c, reg_cond);
    if (is_temp(reg_cond)) reg_free(1);

    int result_reg = reg_alloc();

    int rk_then = parse_binary(c, 0);
    if (parse_had_error) return result_reg;
    if (rk_then & RK_CONST_FLAG) {
        chunk_emit(c, PACK1(OP_LOADK, result_reg)); chunk_emit(c, rk_then & ~RK_CONST_FLAG);
    } else if (rk_then != result_reg) {
        chunk_emit(c, PACK2(OP_MOVE, result_reg, rk_then));
        if (is_temp(rk_then)) reg_free(1);
    }

    chunk_emit(c, OP_JUMP);
    unsigned int patch_jmp = c->count;
    chunk_emit(c, 0);
    patch_jump(c, patch_jif, c->count);

    if (consume(TOKEN_ELSE)) {
        require(TOKEN_COLON, "expected ':' after else");
        if (parse_had_error) return result_reg;
        int rk_else = parse_binary(c, 0);
        if (parse_had_error) return result_reg;
        if (rk_else & RK_CONST_FLAG) {
            chunk_emit(c, PACK1(OP_LOADK, result_reg)); chunk_emit(c, rk_else & ~RK_CONST_FLAG);
        } else if (rk_else != result_reg) {
            chunk_emit(c, PACK2(OP_MOVE, result_reg, rk_else));
            if (is_temp(rk_else)) reg_free(1);
        }
    } else {
        unsigned int null_idx = chunk_add_pool(c, aer_null());
        chunk_emit(c, PACK1(OP_LOADK, result_reg)); chunk_emit(c, (int)null_idx);
    }

    patch_jump(c, patch_jmp, c->count);
    return result_reg;
}

/* Shared while/for-while tail: require ':', branch-if-false, body, back-edge to loop_top. */
/* Shared "compile a for-loop's body and back-edge" tail, used by every for/while/for-in form
   below: loop_top is the back-edge jump target (the position of the loop's own condition/iterate
   opcode); patch_exit is the offset of that opcode's own exit-jump placeholder, patched here once
   the loop's full extent (body + back-edge) is known. Always returns having either pushed and
   popped a loop context, or (require()/loop_push failing) pushed nothing at all — callers that
   promoted reserved_floor for extra iteration-state registers can safely restore it unconditionally
   right after this returns, whether or not the body itself errored. */
static void parse_loop_body(Chunk* c, unsigned int loop_top, unsigned int patch_exit) {
    if (!loop_push(loop_top)) return;
    parse_block(c);
    if (parse_had_error) { loop_depth--; return; }

    chunk_emit(c, OP_JUMP);
    chunk_emit(c, (int)loop_top);
    patch_jump(c, patch_exit, c->count);
    loop_pop_and_patch(c, c->count);
}

static void parse_for_body(Chunk* c, unsigned int loop_top, int rk_cond) {
    require(TOKEN_COLON, "expected ':' after for/while condition");
    if (parse_had_error) return;

    int reg_cond = materialize(c, rk_cond);
    unsigned int patch_exit = emit_jump_if_false_reg(c, reg_cond);
    if (is_temp(reg_cond)) reg_free(1);

    parse_loop_body(c, loop_top, patch_exit);
}

/* `for x in collection:` (arrays, dicts, strings, and ranges — see the TOKEN_DOT_DOT branch
   below). No exit-time cleanup needed — col_reg/idx_reg are just ordinary registers (see
   OP_ITER_NEXT_ARRAY's own comment in vm.h).
     Reserves the loop variable's permanent register BEFORE compiling the collection expression or
   allocating the index register, since temps and permanent variables share one register-numbering
   space: reserving the loop variable first guarantees the collection/idx temps are allocated above
   it, so the temp allocator can never hand out the exact register the loop variable was just
   assigned. */
static void parse_for_in(Chunk* c, unsigned int loop_var_name) {
    int item_reg = var_slot(loop_var_name);
    if (item_reg < 0) return;

    int rk_start = parse_binary(c, 0);   /* the range's start, or the whole collection if no '..' follows */

    /* `a..b` or `a..b..step`: step defaults to a constant 1 when omitted, direction is inferred at
       runtime from cur vs end (OP_ITER_RANGE), not from step's sign. `..` is for-loop-specific
       syntax here, not a general binary operator. */
    if (consume(TOKEN_DOT_DOT)) {
        int rk_end = parse_binary(c, 0);
        int rk_step;
        if (consume(TOKEN_DOT_DOT)) rk_step = parse_binary(c, 0);
        else                        rk_step = (int)chunk_add_pool(c, aer_int(1)) | RK_CONST_FLAG;

        require(TOKEN_COLON, "expected ':' after for-in clause");
        if (parse_had_error) return;

        /* cur_reg MUST be a genuinely fresh register (arg_materialize, not materialize) since
           OP_ITER_RANGE mutates it every iteration — aliasing an existing variable's register
           here would make the loop silently corrupt that variable. end_reg/step_reg are read-only,
           so materialize's cheaper "reuse if already a plain register" is safe for them. */
        int cur_reg  = arg_materialize(c, rk_start);
        int end_reg  = materialize(c, rk_end);
        int step_reg = materialize(c, rk_step);

        /* Promotes whatever of the above are genuine temps to protected/permanent status for
           exactly the loop's duration, by raising reserved_floor to match the watermark right
           here. Without this, a brand-new variable declared inside the loop's OWN body could be
           handed cur_reg's or step_reg's own register — silent corruption of the loop's iteration
           state (a nested `for i in 0..n: for j in (i+1)..n:` lets the inner loop's own `j`
           collide with the outer loop's cur_reg). Restored to the pre-loop watermark once the
           loop's bytecode (body + back-edge) is fully emitted, at the cost of these registers
           never being reused by a LATER, sibling statement even when nothing inside the loop
           needed them promoted. */
        int saved_reserved_floor = reserved_floor;
        reserved_floor = next_temp_register;

        unsigned int loop_top = c->count;
        unsigned int patch_exit = emit_iter_range(c, cur_reg, end_reg, step_reg, item_reg);

        parse_loop_body(c, loop_top, patch_exit);

        reserved_floor     = saved_reserved_floor;
        next_temp_register = saved_reserved_floor;
        return;
    }

    require(TOKEN_COLON, "expected ':' after for-in clause");
    if (parse_had_error) return;

    int col_reg = materialize(c, rk_start);

    int idx_reg = reg_alloc();
    unsigned int pool_zero = chunk_add_pool(c, aer_int(0));
    chunk_emit(c, PACK1(OP_LOADK, idx_reg)); chunk_emit(c, (int)pool_zero);

    /* Same reasoning as the range branch's own comment above: idx_reg (always a genuine temp) and
       col_reg (a temp unless it aliases an existing variable) must stay valid across the loop's
       entire body, not just this setup statement, so they need to be protected from a new
       variable declared inside the body before that body gets compiled. */
    int saved_reserved_floor = reserved_floor;
    reserved_floor = next_temp_register;

    unsigned int loop_top = c->count;   /* the iterate opcode is its own back-edge target */
    unsigned int patch_exit = emit_iter_next_array(c, col_reg, idx_reg, item_reg);

    parse_loop_body(c, loop_top, patch_exit);

    reserved_floor     = saved_reserved_floor;
    next_temp_register = saved_reserved_floor;
}

/* `for k, v in dict:`, the two-loop-variable form. Dict-only at runtime (OP_ITER_NEXT_PAIR itself
   errors otherwise) — reserves BOTH loop variables' permanent registers before compiling the
   collection expression, same ordering reason parse_for_in's own comment gives. */
static void parse_for_in_pair(Chunk* c, unsigned int key_name, unsigned int val_name) {
    int key_reg = var_slot(key_name);
    if (key_reg < 0) return;
    int val_reg = var_slot(val_name);
    if (val_reg < 0) return;

    int rk_col = parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after for-in clause");
    if (parse_had_error) return;

    int col_reg = materialize(c, rk_col);

    int idx_reg = reg_alloc();
    unsigned int pool_zero = chunk_add_pool(c, aer_int(0));
    chunk_emit(c, PACK1(OP_LOADK, idx_reg)); chunk_emit(c, (int)pool_zero);

    int saved_reserved_floor = reserved_floor;
    reserved_floor = next_temp_register;

    unsigned int loop_top = c->count;
    unsigned int patch_exit = emit_iter_next_pair(c, col_reg, idx_reg, key_reg, val_reg);

    parse_loop_body(c, loop_top, patch_exit);

    reserved_floor     = saved_reserved_floor;
    next_temp_register = saved_reserved_floor;
}

/* AER has no separate `while` keyword — `for <condition>:` (no `in`) IS the while form. */
static void parse_for_while(Chunk* c) {
    if (equal(TOKEN_IDENTIFIER)) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();

        /* `for k, v in dict:` — unambiguous here: a comma right after a for-loop's first
           identifier can only mean a second loop variable, never destructuring (that's a
           statement-position construct, parsed by parse_assignment, which this function never
           calls into). */
        if (consume(TOKEN_COMMA)) {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected identifier after ','"); return; }
            unsigned int name2_idx = chunk_add_pool(c, token.value);
            lex();
            require(TOKEN_IN, "expected 'in' after 'for k, v'");
            if (parse_had_error) return;
            parse_for_in_pair(c, name_idx, name2_idx);
            return;
        }

        if (consume(TOKEN_IN)) {
            parse_for_in(c, name_idx);
            return;
        }
        int reg = var_slot(name_idx);
        if (reg < 0) return;
        unsigned int loop_top = c->count;
        /* Runs the shared postfix-chain helper before climbing to binary ops, so `for
           <postfix-chain>:` (the linked-list-traversal idiom, `for cur.next:`) resolves the chain
           first — a condition that's just the bare variable is unaffected, since the chain loop
           immediately finds no '.'/'['/'(' and returns `reg` unchanged. lhs_start is captured
           AFTER the chain runs so the fusion check in parse_binary_ops still sees exactly what
           this statement emitted. */
        int rk_chain = parse_postfix_chain(c, reg);
        int rk_cond = parse_binary_ops(c, 0, rk_chain, c->count);
        parse_for_body(c, loop_top, rk_cond);
        return;
    }
    unsigned int loop_top = c->count;
    int rk_cond = parse_binary(c, 0);
    parse_for_body(c, loop_top, rk_cond);
}

/* True if the current (not yet consumed) token names a module imported earlier, via the
   Chunk-level chunk_is_imported/chunk_add_import registry. Checked before the normal
   identifier/call/assignment path in both parse_primary_inner and parse_statement: a module name
   isn't a variable, so var_slot/func_lookup must never see it. */
static bool at_module_name(Chunk* c) {
    return token.type == TOKEN_IDENTIFIER &&
           chunk_is_imported(c, aer_as_string(token.value)->data, aer_as_string(token.value)->length);
}

/* `module.function(args)` — arguments reach the call via contiguous registers
   (parse_contiguous_exprs), same as every other call site; OP_CALL_MODULE's handler (vm.c)
   bridges to the shared stdlib dispatch (aer_math_call/aer_string_call/etc.) rather than
   reimplementing every stdlib function for registers — see its comment in vm.h. */
static int parse_module_call(Chunk* c) {
    unsigned int module_idx = chunk_add_pool(c, token.value);
    lex();
    if (!consume(TOKEN_DOT)) {
        error_at("A module can't be used as a value on its own — call a function on it, e.g. 'module.function(...)'");
        return 0;
    }
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after '.'"); return 0; }
    unsigned int fn_idx = chunk_add_pool(c, token.value);
    lex();
    require(TOKEN_OPEN_PARENTHESE, "expected '(' after module function name");
    if (parse_had_error) return 0;

    int arg_reg_base;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after module call arguments");
    if (parse_had_error) return 0;

    int dest = (arg_count > 0) ? arg_reg_base : reg_alloc();
    if (arg_count > 1) reg_free(arg_count - 1);
    int base = arg_reg_base < 0 ? dest : arg_reg_base;
    chunk_emit(c, PACK3(OP_CALL_MODULE, dest, base, arg_count));
    chunk_emit(c, (int)module_idx);
    chunk_emit(c, (int)fn_idx);
    return dest;
}

/* `import module[.sub]*` — top level only (checked via function_depth; an import inside an if/for
   block at top level is accepted, a narrow known gap). Doesn't check chunk_add_import's return
   value — a failed import is silently not registered, and a later module.function() call against
   it just falls through to "unknown function". */
static void parse_import(Chunk* c) {
    if (function_depth != 0) {
        error_at("'import' is only allowed at the top level of a file, not inside a function");
        return;
    }
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a module name after 'import'"); return; }

    char path_buf[256];
    unsigned int path_len = 0, bind_start = 0, bind_len = 0;
    while (true) {
        const char* seg      = aer_as_string(token.value)->data;
        unsigned int seg_len = aer_as_string(token.value)->length;
        if (path_len > 0) path_buf[path_len++] = '/';
        if (path_len + seg_len >= sizeof(path_buf)) { error_at("Import path too long"); return; }
        bind_start = path_len;
        memcpy(path_buf + path_len, seg, seg_len);
        path_len += seg_len;
        bind_len = seg_len;
        lex();
        if (!consume(TOKEN_DOT)) break;
        if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a module name segment after '.'"); return; }
    }
    chunk_add_import(c, path_buf + bind_start, bind_len, path_buf, path_len);
}

/* Global builtins (length/delete/append/print/type/assert/panic), checked by name against a fixed
   list rather than any runtime fallback chain, consistent with every other call target being
   resolved at compile time. Struct construction (vm_call_builtin's OTHER fallback branch, via
   chunk_find_shape) is deliberately excluded — already resolved at compile time via
   is_struct_name, checked before this ever runs. */
static bool is_builtin_name(Chunk* c, unsigned int name_idx) {
    AerString* s = aer_as_string(c->pool[name_idx]);
    static const char* const names[] = { "length", "delete", "append", "print", "type", "assert", "panic" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t len = strlen(names[i]);
        if (s->length == len && strncmp(s->data, names[i], len) == 0) return true;
    }
    return false;
}

/* `length(args)` etc. — same contiguous-register argument materialization and result-register
   reuse as parse_call, just emitting OP_CALL_BUILTIN instead. `name_idx` is already
   consumed and confirmed to be a builtin name by the caller. */
static int parse_builtin_call(Chunk* c, unsigned int name_idx) {
    int arg_reg_base;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error) return 0;

    int dest = (arg_count > 0) ? arg_reg_base : reg_alloc();
    if (arg_count > 1) reg_free(arg_count - 1);
    int base = arg_reg_base < 0 ? dest : arg_reg_base;
    chunk_emit(c, PACK3(OP_CALL_BUILTIN, dest, base, arg_count));
    chunk_emit(c, (int)name_idx);
    return dest;
}

/* Call sites. Reached both as an expression (parse_primary_inner, identifier immediately
   followed by '(') and as a bare statement (parse_statement, result discarded). `name_idx` is
   already consumed by the caller. Handles BOTH function calls and struct instantiation, resolved
   at compile time via is_struct_name rather than a runtime fallback chain. A third fallback,
   global builtins, is checked last (is_builtin_name). */
/* Tail-call optimization. Set to c->count right after parse_call emits a bare OP_CALL/OP_CALL_VALUE
   (not a struct construction — see the is_struct branch below, which deliberately doesn't touch
   this); checked by parse_return to detect a true tail call. c->count only grows, and anything
   that could wrap a call (a further postfix step, a binary operator, the multi-return comma) emits
   more words after — so last_bare_call_end == c->count is a robust proof the whole return
   expression is exactly one bare call. The (unsigned int)-1 sentinel means c->count can never
   accidentally match before any call has ever been compiled. */
static unsigned int last_bare_call_end = (unsigned int)-1;

static int parse_call(Chunk* c, unsigned int name_idx) {
    /* Functions as values: if `name_idx` is already a known variable (holding whatever value it
       was assigned — possibly a function value), this is a call THROUGH that variable, not a call
       BY NAME — resolved via OP_CALL_VALUE at runtime instead of the compile-time name resolution
       below. Same local-shadows-global-shadows-function precedence parse_primary_inner's own
       bare-reference case uses. A global's register number is only valid in FRAME 0 — var_reg is
       captured now but only actually materialized into a local temp further down, AFTER dest/arg
       registers are settled, so that temp ends up on top of the register stack and can be freed
       correctly (LIFO) once the call is emitted; materializing it here instead would leave it
       stranded below dest, unfreeable without corrupting the allocator's watermark. */
    int  var_reg = -1;
    bool is_local_var  = var_lookup(name_idx, &var_reg);
    bool is_global_var = !is_local_var && function_depth > 0 && global_lookup(name_idx, &var_reg);
    bool is_var = is_local_var || is_global_var;

    bool is_struct = !is_var && is_struct_name(name_idx);
    unsigned int func_offset = 0, func_arity = 0, func_min_arity = 0, func_receiver_type = 0;
    AerVal* func_defaults = NULL;
    bool func_has_receiver = false;
    bool is_func = !is_var && !is_struct &&
                   func_full_lookup(name_idx, &func_offset, &func_arity, &func_min_arity, &func_defaults,
                                        &func_has_receiver, &func_receiver_type);
    /* Forward references / mutual recursion. A name that isn't a variable, struct, known
       function, or builtin is not an immediate error: it's optimistically assumed to be a
       function defined LATER in this same parse() call (see pending_call_add's own comment). If
       it never actually gets defined, that's caught and reported once parse()'s top-level loop
       ends. */
    bool is_forward_ref = false;
    const char* call_site_cursor = NULL;
    if (!is_var && !is_struct && !is_func) {
        if (is_builtin_name(c, name_idx)) return parse_builtin_call(c, name_idx);
        is_forward_ref = true;
        call_site_cursor = current_source_cursor();   /* captured NOW — before the arg list below consumes past it */
    }

    /* Arguments must land in contiguous registers for OP_CALL/OP_STRUCT_NEW's bulk-copy —
       see arg_materialize's own comment for why this is usually a no-op check, not a copy. */
    int arg_reg_base;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error) return 0;

    /* Default parameters via a direct, by-name call — a direct call must check arity, since
       fewer args than declared parameters means some are meant to fall back to their defaults
       rather than read whatever garbage a prior occupant of that callee frame slot left. */
    bool arity_error = is_func && ((unsigned int)arg_count > func_arity || (unsigned int)arg_count < func_min_arity);
    if (arity_error) {
        const char* fname = aer_as_string(c->pool[name_idx])->data;
        if (func_min_arity == func_arity)
            error_at("Function '%s' expects %u argument%s, got %d", fname, func_arity, func_arity == 1 ? "" : "s", arg_count);
        else
            error_at("Function '%s' expects between %u and %u arguments, got %d", fname, func_min_arity, func_arity, arg_count);
        return 0;
    }
    bool needs_padding = is_func && (unsigned int)arg_count < func_arity;
    /* A receiver-checked function (`function f(target as Type, ...)`) must always route through
       OP_CALL_VALUE, even at exact arity: the receiver check itself lives in lbl_call_value, since
       that's the only call path with a real runtime AerFunction value to read
       has_receiver/receiver_type from — OP_CALL's target is just a bare compile-time offset, with
       no such value at all. */
    bool needs_call_value = needs_padding || func_has_receiver;

    /* The result reuses the first argument's register — matches Lua's own convention of reusing
       the base register for a call's result, same compact-register-use philosophy as
       compile_node's free-then-allocate discipline elsewhere. Determined BEFORE any callee_reg
       below is allocated (and before the compaction free at the very end), so a call needing an
       extra register just for its callee (a materialized global, or a freshly built function
       value for the padding/receiver-checked case) always lands ABOVE dest/the args, never
       aliasing one of them — seemingly harmless reordering that actually matters: reg_free() is
       a pure watermark rewind with no memory of what a register held, so allocating a new temp
       AFTER compacting (this function's own original, buggy order) could silently hand out the
       very register an omitted-defaults call's remaining argument was still sitting in. */
    int dest = (arg_count > 0) ? arg_reg_base : reg_alloc();
    int base = arg_reg_base < 0 ? dest : arg_reg_base;

    bool needs_callee_reg = (is_var && is_global_var) || needs_call_value;
    int callee_reg = -1;
    if (is_global_var) {
        callee_reg = reg_alloc();
        chunk_emit(c, PACK2(OP_LOAD_GLOBAL, callee_reg, var_reg));
    } else if (needs_call_value) {
        AerVal fv = build_function_value(func_offset, func_arity, func_min_arity, func_defaults,
                                             func_has_receiver, func_receiver_type);
        callee_reg = reg_alloc();
        chunk_emit(c, PACK1(OP_LOADK, callee_reg)); chunk_emit(c, (int)chunk_add_pool(c, fv));
    }

    if (is_var) {
        emit_call_value(c, dest, base, arg_count, is_local_var ? var_reg : callee_reg);
        last_bare_call_end = c->count;
    } else if (is_struct) {
        emit_struct_new(c, dest, name_idx, base, arg_count);
    } else if (is_func) {
        if (needs_call_value) emit_call_value(c, dest, base, arg_count, callee_reg);
        else                   emit_call(c, dest, func_offset, base, arg_count);   /* exact arity, no receiver check — no forward-ref patching needed, is_func means already resolved */
        last_bare_call_end = c->count;
    } else {
        unsigned int patch_offset = emit_call(c, dest, func_offset, base, arg_count);
        if (is_forward_ref) pending_call_add(name_idx, patch_offset, call_site_cursor);
        last_bare_call_end = c->count;
    }

    /* One combined compaction back down to just `dest`, accounting for BOTH the extra argument
       registers and callee_reg (if one was allocated) in a single free — not two separate,
       differently-timed frees — since nothing else allocates between them being materialized
       above and this point, a single count is exactly as correct as (and far simpler than) trying
       to free each piece the moment it stops being needed. */
    int extra = (arg_count > 1 ? arg_count - 1 : 0) + (needs_callee_reg ? 1 : 0);
    if (extra > 0) reg_free(extra);

    return dest;
}

/* `defer name(args)`. Resolves the target at compile time via func_lookup, consistent with every
   other call target being resolved at compile time. Only a plain function is a valid target: no
   module calls and no struct construction ("deferred construction" has no clear meaning) —
   is_struct_name is deliberately not checked here, so a defer targeting a struct type still
   ultimately fails, just via the same "never defined anywhere in this call" path a genuine typo
   would, rather than a dedicated message. Arguments are evaluated now, via the same
   contiguous-register materialization every other call site uses, then copied out immediately by
   OP_DEFER_PUSH — snapshotted at the defer statement, not re-evaluated at replay time. */
static void parse_defer(Chunk* c) {
    if (function_depth == 0) { error_at("'defer' outside function"); return; }
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after 'defer'"); return; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();

    /* Deferring a builtin (`defer append(log, "ran")`). Checked before the forward-reference
       assumption below: a builtin is always "already defined", never a forward reference. */
    bool is_builtin = is_builtin_name(c, name_idx);

    /* Same forward-reference assumption as parse_call: an unresolved name might still be a
       function defined later in this same parse() call. Cursor captured NOW, before the arg list
       below consumes past it. */
    unsigned int func_offset = 0;
    bool is_forward_ref = false;
    const char* call_site_cursor = NULL;
    if (!is_builtin) {
        is_forward_ref = !func_lookup(name_idx, &func_offset);
        call_site_cursor = is_forward_ref ? current_source_cursor() : NULL;
    }

    require(TOKEN_OPEN_PARENTHESE, "expected '(' after deferred function name");
    if (parse_had_error) return;

    int arg_reg_base;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after deferred call arguments");
    if (parse_had_error) return;

    if (arg_count > MAX_DEFER_ARGS) {
        error_at("Too many arguments to a deferred call (max %d)", MAX_DEFER_ARGS);
        return;
    }

    /* Builtins have no bytecode offset of their own (OP_CALL_BUILTIN calls straight into C), so the
       deferred-call replay mechanism (lbl_return, vm.c — jumps to a stored offset and runs it
       exactly like an ordinary call re-entry) has nothing to jump TO for one. Fixed with a tiny
       synthetic trampoline, emitted once right here (jumped over so it never runs inline): call
       the builtin with whatever args land in registers[0..arg_count), then return —
       indistinguishable from a real function to the replay mechanism. */
    unsigned int callee_offset;
    if (is_builtin) {
        chunk_emit(c, OP_JUMP);
        unsigned int patch = c->count;
        chunk_emit(c, 0);
        callee_offset = c->count;
        chunk_emit(c, PACK3(OP_CALL_BUILTIN, 0, 0, arg_count));
        chunk_emit(c, (int)name_idx);
        chunk_emit(c, PACK1(OP_RETURN, 0));
        patch_jump(c, patch, c->count);
    } else {
        callee_offset = func_offset;
    }

    chunk_emit(c, PACK2(OP_DEFER_PUSH, arg_count > 0 ? arg_reg_base : 0, arg_count));
    unsigned int patch_offset = c->count;
    chunk_emit(c, (int)callee_offset);
    if (!is_builtin && is_forward_ref) pending_call_add(name_idx, patch_offset, call_site_cursor);

    /* Args are copied out of these registers immediately by the opcode above — unlike a plain
       call, there's no result register to reuse one of them for, so all of them free. */
    if (arg_count > 0) reg_free(arg_count);
}

/* `return expr` or bare `return` (implicit null). `return a, b, ...` packs into an array
   (OP_ARRAY_NEW): the destructuring-assignment side (parse_assignment's single-RHS-expression
   case, already built for `a, b = some_array_expr`) already treats a call's result as "the array
   to unpack" whenever the RHS is just one expression — so `lo, hi = min_max(...)` needs no
   caller-side change once the callee returns an array this way.
     Tail-call optimization: `return f(args)`, with NOTHING else wrapping the call, patches that
   call's own OP_CALL/OP_CALL_VALUE opcode in place to its tail-call counterpart instead of
   emitting a normal return — just patching one byte of a packed descriptor word (the opcode, in
   the low 8 bits — see PACK3's own comment in vm.h) since call opcodes share their operand shape
   byte-for-byte with their tail-call counterparts. Not reachable for the multi-return (comma) case
   above — a tail call only makes sense when the return value IS the call's own result, not an
   array built to smuggle several values out. */
static void parse_return(Chunk* c) {
    if (function_depth == 0) { error_at("'return' outside function"); return; }

    if (!equal(TOKEN_NEW_LINE) && !equal(TOKEN_END_OF_FILE) && !equal(TOKEN_DEDENT)) {
        int rk_first = parse_binary(c, 0);
        if (parse_had_error) return;

        if (equal(TOKEN_COMMA)) {
            int reg_base = arg_materialize(c, rk_first);
            unsigned int count = 1;
            while (consume(TOKEN_COMMA)) {
                int rk_next = parse_binary(c, 0);
                if (parse_had_error) return;
                arg_materialize(c, rk_next);
                count++;
            }
            reg_free((int)count - 1);
            emit_array_new(c, reg_base, reg_base, (int)count);
            emit_return(c, reg_base);
            return;
        }

        if (last_bare_call_end == c->count) {
            int op_slot = (int)c->count - 2;
            int orig_op = c->code[op_slot] & 0xFF;
            if (orig_op == OP_CALL || orig_op == OP_CALL_VALUE) {
                int tail_op = (orig_op == OP_CALL) ? OP_TAIL_CALL : OP_TAIL_CALL_VALUE;
                c->code[op_slot] = (c->code[op_slot] & ~0xFF) | tail_op;
                return;
            }
        }

        int reg = materialize(c, rk_first);
        emit_return(c, reg);
        return;
    }

    int rk = (int)chunk_add_pool(c, aer_null()) | RK_CONST_FLAG;
    int reg = materialize(c, rk);
    emit_return(c, reg);
}

/* anonymous function expressions: `function(params): body`, a function VALUE in
   expression position (assignable, passable as a callback, storable in an array/dict field —
   everything a named function's bare-name reference already supports, see
   build_function_value's own comment), not a statement. Deliberately a near-duplicate of
   parse_function's own param/default/body/register-table-save-restore logic rather than a
   shared helper: parse_function must register the function's name (func_register) BEFORE
   compiling its body, so a self-recursive call inside resolves — an anonymous function has no
   name to self-reference by, so that ordering constraint (and the registration itself) simply
   doesn't apply here, which would make a shared helper's signature awkward for little benefit.
   No forward-reference/pending-call handling either, for the same reason: nothing could ever be
   forward-referencing THIS function by name, since it has none. */
static int parse_function_expr(Chunk* c) {
    require(TOKEN_OPEN_PARENTHESE, "expected '(' after 'function'");
    if (parse_had_error) return 0;

    unsigned int param_names[FRAME_REGISTERS];
    AerVal       param_defaults[FRAME_REGISTERS];
    int param_count     = 0;
    int min_param_count = 0;
    bool seen_default   = false;
    if (!equal(TOKEN_CLOSE_PARENTHESE)) {
        do {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected parameter name"); return 0; }
            if (param_count >= FRAME_REGISTERS) {
                error_at("Too many parameters (max %d)", FRAME_REGISTERS);
                return 0;
            }
            param_names[param_count] = chunk_add_pool(c, token.value);
            lex();
            if (consume(TOKEN_ASSIGN)) {
                if (!parse_literal_default(c, &param_defaults[param_count])) {
                    error_at("Parameter defaults must be a literal value");
                    return 0;
                }
                seen_default = true;
            } else if (seen_default) {
                error_at("A parameter without a default cannot follow one that has a default");
                return 0;
            } else {
                min_param_count++;
            }
            param_count++;
        } while (consume(TOKEN_COMMA));
    }
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after parameters");
    require(TOKEN_COLON,            "expected ':' after function signature");
    if (parse_had_error) return 0;

    chunk_emit(c, OP_JUMP);
    unsigned int patch = c->count;
    chunk_emit(c, 0);
    unsigned int func_start = c->count;

    unsigned int saved_var_names[FRAME_REGISTERS];
    int          saved_var_regs[FRAME_REGISTERS];
    int saved_var_count      = var_count;
    int saved_next_temp      = next_temp_register;
    int saved_reserved_floor = reserved_floor;
    memcpy(saved_var_names, var_names, sizeof(unsigned int) * (size_t)var_count);
    memcpy(saved_var_regs,  var_regs,  sizeof(int) * (size_t)var_count);
    var_count          = 0;
    next_temp_register = 0;
    reserved_floor     = 0;

    function_depth++;
    for (int i = 0; i < param_count; i++) var_slot(param_names[i]);

    parse_block(c);
    function_depth--;

    if (!parse_had_error) {
        int rk_null  = (int)chunk_add_pool(c, aer_null()) | RK_CONST_FLAG;
        int reg_null = materialize(c, rk_null);
        emit_return(c, reg_null);
    }

    var_count = saved_var_count;
    memcpy(var_names, saved_var_names, sizeof(unsigned int) * (size_t)saved_var_count);
    memcpy(var_regs,  saved_var_regs,  sizeof(int) * (size_t)saved_var_count);
    next_temp_register = saved_next_temp;
    reserved_floor     = saved_reserved_floor;

    patch_jump(c, patch, c->count);

    unsigned int default_count = (unsigned int)param_count - (unsigned int)min_param_count;
    AerVal* defaults = NULL;
    if (default_count > 0) {
        defaults = xmalloc(sizeof(AerVal) * default_count);
        for (unsigned int i = 0; i < default_count; i++) defaults[i] = param_defaults[(unsigned int)min_param_count + i];
    }
    AerVal fv = build_function_value(func_start, (unsigned int)param_count, (unsigned int)min_param_count,
                                         defaults, false, 0);
    return (int)chunk_add_pool(c, fv) | RK_CONST_FLAG;
}

/* `function name(params):` — named functions can't nest (matches the real language's own
   restriction). Default parameters: once one parameter has a default, every parameter after it
   must too. */
static void parse_function(Chunk* c) {
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected function name"); return; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();

    require(TOKEN_OPEN_PARENTHESE, "expected '(' after function name");
    if (parse_had_error) return;

    unsigned int param_names[FRAME_REGISTERS];
    AerVal       param_defaults[FRAME_REGISTERS];   /* only [min_param_count, param_count) are meaningful */
    int param_count     = 0;
    int min_param_count = 0;
    bool seen_default   = false;
    bool has_receiver          = false;
    unsigned int receiver_type = 0;
    if (!equal(TOKEN_CLOSE_PARENTHESE)) {
        do {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected parameter name"); return; }
            if (param_count >= FRAME_REGISTERS) {
                error_at("Too many parameters (max %d)", FRAME_REGISTERS);
                return;
            }
            param_names[param_count] = chunk_add_pool(c, token.value);
            lex();
            /* `target as Type`, only meaningful on parameter 0 (the receiver), checked once at
               call setup (lbl_call_value, vm.c) against the call's first argument. Always
               consumes a stray 'as' here (not just when param_count==0) so a later annotation
               gets one clear error instead of desyncing the parser. */
            if (consume(TOKEN_AS)) {
                if (param_count != 0) {
                    error_at("A struct-type parameter ('as Type') is only allowed on the first parameter — it names that parameter as the method receiver, checked once against the call's first argument; there's no mechanism to check later parameters, so give them a plain name instead");
                    return;
                }
                if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a struct type name after 'as'"); return; }
                receiver_type = chunk_add_pool(c, token.value);
                has_receiver  = true;
                lex();
            }
            if (consume(TOKEN_ASSIGN)) {
                if (!parse_literal_default(c, &param_defaults[param_count])) {
                    error_at("Parameter defaults must be a literal value");
                    return;
                }
                seen_default = true;
            } else if (seen_default) {
                error_at("A parameter without a default cannot follow one that has a default");
                return;
            } else {
                min_param_count++;
            }
            param_count++;
        } while (consume(TOKEN_COMMA));
    }
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after parameters");
    require(TOKEN_COLON,            "expected ':' after function signature");
    if (parse_had_error) return;

    chunk_emit(c, OP_JUMP);
    unsigned int patch = c->count;
    chunk_emit(c, 0);
    unsigned int func_start = c->count;

    /* Isolate the body's register space from the caller/top-level's — save the allocator's raw
       state and the variable table, reset both to fresh, restore after. No stack of saved tables
       needed since named functions can't nest. */
    unsigned int saved_var_names[FRAME_REGISTERS];
    int          saved_var_regs[FRAME_REGISTERS];
    int saved_var_count      = var_count;
    int saved_next_temp      = next_temp_register;
    int saved_reserved_floor = reserved_floor;
    memcpy(saved_var_names, var_names, sizeof(unsigned int) * (size_t)var_count);
    memcpy(saved_var_regs,  var_regs,  sizeof(int) * (size_t)var_count);
    var_count          = 0;
    next_temp_register = 0;
    reserved_floor     = 0;

    /* Register the function BEFORE compiling its body — func_start is already known, so a
       self-recursive call inside the body resolves correctly. A call to any other not-yet-defined
       function isn't required to fail here either (pending_call_add) — as long as that other
       function gets defined somewhere else in this same parse() call, forward references and
       mutual recursion both resolve correctly. */
    unsigned int default_count = (unsigned int)param_count - (unsigned int)min_param_count;
    AerVal* defaults = NULL;
    if (default_count > 0) {
        defaults = xmalloc(sizeof(AerVal) * default_count);
        for (unsigned int i = 0; i < default_count; i++) defaults[i] = param_defaults[(unsigned int)min_param_count + i];
    }
    func_register(c, name_idx, func_start, (unsigned int)param_count, (unsigned int)min_param_count, defaults,
                      has_receiver, receiver_type);

    /* Incremented BEFORE registering parameters (not after, as a first draft of the global-read
       feature had it) — var_slot only skips recording a top-level global when
       function_depth > 0, and a parameter is never a global no matter how early in this
       function's compilation it's registered. */
    function_depth++;
    for (int i = 0; i < param_count; i++) var_slot(param_names[i]);

    parse_block(c);
    function_depth--;

    if (!parse_had_error) {
        /* Implicit 'return null' if control falls off the end. */
        int rk_null  = (int)chunk_add_pool(c, aer_null()) | RK_CONST_FLAG;
        int reg_null = materialize(c, rk_null);
        emit_return(c, reg_null);
    }

    var_count = saved_var_count;
    memcpy(var_names, saved_var_names, sizeof(unsigned int) * (size_t)saved_var_count);
    memcpy(var_regs,  saved_var_regs,  sizeof(int) * (size_t)saved_var_count);
    next_temp_register = saved_next_temp;
    reserved_floor     = saved_reserved_floor;

    patch_jump(c, patch, c->count);
}

/* Shared by struct field defaults (below) and function parameter defaults (parse_function),
   including a `[]`/`{}` "empty-container template" special case: vm_default_value (vm.c) already
   allocates a FRESH empty array/dict from this template at each use (a struct instantiation, or a
   call that omits this argument), avoiding Python's mutable-default-argument bug. Returns false
   (no token consumed beyond what was peeked) if the current token isn't a valid literal default
   form. */
static bool parse_literal_default(Chunk* c, AerVal* out) {
    bool negative = false;
    if (token.type == TOKEN_SUBTRACT) {
        /* Peek without consuming yet — only actually a negative-number prefix if a number
           follows; consumed below only once that's confirmed. */
        lex();
        negative = true;
    }
    if (token.type == TOKEN_INTEGER) {
        long long n = aer_as_int(token.value);
        *out = aer_int(negative ? -n : n);
    } else if (token.type == TOKEN_REAL) {
        double d = aer_as_real(token.value);
        *out = aer_real(negative ? -d : d);
    } else if (negative) {
        return false;   /* '-' is only meaningful before a number */
    } else if (token.type == TOKEN_TRUE || token.type == TOKEN_FALSE) {
        *out = aer_bool(aer_as_bool(token.value));
    } else if (token.type == TOKEN_NULL) {
        *out = aer_null();
    } else if (token.type == TOKEN_STRING) {
        AerString* ts = aer_as_string(token.value);
        *out = c->pool[pool_escaped_string(c, ts->data, ts->length)];
    } else if (token.type == TOKEN_OPEN_BRACKET) {
        lex();
        if (token.type != TOKEN_CLOSE_BRACKET) return false;
        AerArray* a = vm_new_array();
        a->count = a->capacity = 0;
        a->items = NULL;
        a->shape = NULL;
        *out = aer_array_val(a);
    } else if (token.type == TOKEN_OPEN_BRACE) {
        lex();
        if (token.type != TOKEN_CLOSE_BRACE) return false;
        AerDict* d = vm_new_dict();
        memset(&d->map, 0, sizeof(d->map));
        d->map.is_inline = true;
        *out = aer_dict_val(d);
    } else {
        return false;
    }
    lex();
    return true;
}

/* `struct Name:\n    field [= literal]\n ...` — field-by-field. Emits OP_DEFINE_STRUCT directly
   (see this opcode's own comment in vm.h for why it needs no register-VM-specific wrapper), then
   registers the type name so a later `Name(args)` call site resolves to construction
   (parse_call). Field defaults share the same literal set (including `[]`/`{}`) via
   parse_literal_default above. */
static void parse_struct(Chunk* c) {
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected struct name"); return; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();
    require(TOKEN_COLON,    "expected ':' after struct name");
    require(TOKEN_NEW_LINE, "expected newline before indented struct body");
    require(TOKEN_INDENT,   "expected indented struct body");
    if (parse_had_error) return;

    unsigned int field_names[MAX_STRUCT_FIELDS];
    AerVal       field_defaults[MAX_STRUCT_FIELDS];
    unsigned int field_count = 0;

    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        if (!equal(TOKEN_IDENTIFIER))          { error_at("Expected field name");        return; }
        if (field_count >= MAX_STRUCT_FIELDS)  { error_at("Too many struct fields (max %d)", MAX_STRUCT_FIELDS); return; }

        unsigned int fname = chunk_add_pool(c, token.value);
        lex();
        AerVal dflt = aer_null();
        if (consume(TOKEN_ASSIGN)) {
            if (!parse_literal_default(c, &dflt)) {
                error_at("Struct field defaults must be a literal value");
                return;
            }
        }
        field_names[field_count]    = fname;
        field_defaults[field_count] = dflt;
        field_count++;

        if (!equal(TOKEN_DEDENT) && !equal(TOKEN_END_OF_FILE))
            require(TOKEN_NEW_LINE, "expected newline after struct field");
        if (parse_had_error) return;
    }
    consume(TOKEN_DEDENT);

    if (field_count == 0) { error_at("Struct must have at least one field"); return; }

    chunk_emit(c, OP_DEFINE_STRUCT);
    chunk_emit(c, (int)name_idx);
    chunk_emit(c, (int)field_count);
    for (unsigned int i = 0; i < field_count; i++) {
        chunk_emit(c, (int)field_names[i]);
        chunk_emit(c, (int)chunk_add_pool(c, field_defaults[i]));
    }

    struct_register(name_idx);
}

/* break/continue — no iter_slots POPs needed (see LoopContext's own comment). */
static void parse_break(Chunk* c) {
    if (loop_depth == 0) { error_at("'break' outside loop"); return; }
    LoopContext* ctx = &loop_stack[loop_depth - 1];
    if (ctx->patch_count >= BREAK_MAX) { error_at("Too many breaks in one loop (max %d)", BREAK_MAX); return; }
    chunk_emit(c, OP_JUMP);
    ctx->patches[ctx->patch_count++] = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by loop_pop_and_patch once the loop ends */
}

static void parse_continue(Chunk* c) {
    if (loop_depth == 0) { error_at("'continue' outside loop"); return; }
    chunk_emit(c, OP_JUMP);
    chunk_emit(c, (int)loop_stack[loop_depth - 1].top);   /* known at compile time — no patch needed */
}

/* A bare call/module-call used as a STATEMENT (its result isn't assigned to anything):
   auto-prints it in shell mode (`mode == MODE_SHELL`), and either way frees its now-dead result
   register. parse_call/parse_module_call both guarantee their returned register is a genuine temp
   (arg_materialize never hands back a permanent variable's register directly), so no is_temp
   guard is needed here. The free half matters even outside shell mode: without it, every bare
   call statement permanently wasted one of FRAME_REGISTERS' slots — invisible in a single
   one-shot compile, but fatal for a REPL session accumulating many statements in the same process
   (see reg_alloc/reg_reserve's own comment on why that cap is a hard, enforced ceiling rather
   than a silent overflow). */
static void discard_statement_result(Chunk* c, int dest) {
    if (parse_had_error) return;
    if (mode == MODE_SHELL) emit_print_repl(c, dest);
    reg_free(1);
}

static void parse_statement(Chunk* c) {
    if (consume(TOKEN_IF))       { parse_if(c);       return; }
    if (consume(TOKEN_FOR))      { parse_for_while(c); return; }
    if (consume(TOKEN_FUNCTION)) { parse_function(c);  return; }
    if (consume(TOKEN_RETURN))   { parse_return(c);    return; }
    if (consume(TOKEN_STRUCT))   { parse_struct(c);    return; }
    if (consume(TOKEN_BREAK))    { parse_break(c);     return; }
    if (consume(TOKEN_CONTINUE)) { parse_continue(c);  return; }
    if (consume(TOKEN_IMPORT))   { parse_import(c);    return; }
    if (consume(TOKEN_DEFER))    { parse_defer(c);     return; }
    if (at_module_name(c))    { discard_statement_result(c, parse_module_call(c)); return; }
    if (equal(TOKEN_IDENTIFIER)) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_OPEN_PARENTHESE)) { discard_statement_result(c, parse_call(c, name_idx)); return; }
        if (consume(TOKEN_OPEN_BRACKET))    { parse_chain_assignment(c, name_idx, true);  return; }
        if (consume(TOKEN_DOT))             { parse_chain_assignment(c, name_idx, false); return; }
        parse_assignment(c, name_idx);
        return;
    }
    error_at("Expected a statement (only assignment, indexed/field writes, if/else, for-while, break/continue, function/struct defs, return, imports, and calls are supported)");
}

/* isolates a fresh, independent program from any earlier chunk compiled in the same
   process: the register allocator, the variable/global/function/struct-type tables, and
   function_depth/loop_depth all reset to empty. Callers that need this call it exactly
   once — a one-shot file run (tests/run_file.c) calls it once before its only parse() call;
   a REPL calls it once at session startup and never again, so names/registers accumulate across
   every line typed afterward, matching how the stack VM's own parse() needs no such reset at all
   (its names resolve dynamically against live scope state, not a compile-time table); a test
   harness invoking parse() once per independent test case (tests/smoke_test.c's
   run_source) calls it once per case, the same way run_chunk resets runtime_had_error per test
   for the same isolation reason. */
void parser_reset(void) {
    reg_reset();
    var_count      = 0;
    global_count   = 0;
    func_count     = 0;
    struct_count   = 0;
    pending_count  = 0;
    function_depth = 0;
    loop_depth     = 0;
    parse_had_error   = false;
}

/* See ParserState's own comment in parser.h. Every field below is a direct mirror of one of
   this file's persistent statics, at the point in the file where it's declared. */
struct ParserState {
    unsigned int  var_names[FRAME_REGISTERS];
    int           var_regs[FRAME_REGISTERS];
    int           var_count;
    int           next_temp_register;
    int           reserved_floor;
    int           function_depth;
    unsigned int  global_names[FRAME_REGISTERS];
    int           global_regs[FRAME_REGISTERS];
    int           global_count;
    unsigned int* func_names;
    unsigned int* func_offsets;
    unsigned int* func_arities;
    unsigned int* func_min_arities;
    AerVal**      func_defaults;
    bool*         func_has_receiver;
    unsigned int* func_receiver_type;
    int           func_count, func_cap;
    PendingCall* pending_calls;
    int            pending_count, pending_cap;
    LoopContext  loop_stack[LOOP_MAX];
    int            loop_depth;
    unsigned int*  struct_names;
    int            struct_count, struct_cap;
    bool           recovered_at_boundary;
};

/* Snapshots every persistent table by TRANSPLANTING each heap array's pointer into the snapshot
   and nulling the live global (not just copying the pointer value) — parser_reset() only zeros
   *counts*, it deliberately never frees/reallocates these arrays (so a REPL's later lines keep
   reusing the same growable buffers across calls), which means a plain copy-and-reset here would
   leave the nested compile writing into the SAME underlying memory this snapshot is supposedly
   preserving. Nulling forces the nested parse() call's first registration of each kind to see
   count==0/cap==0 and allocate genuinely fresh arrays, exactly as if parser_reset() had run
   against an otherwise-untouched process. Also directly resets the register allocator/depth
   counters (equivalent to reg_reset()) — the caller should NOT call parser_reset() as well,
   that would be redundant with what this function already does. */
ParserState* parser_save_state(void) {
    ParserState* s = xmalloc(sizeof(ParserState));

    memcpy(s->var_names, var_names, sizeof(var_names));
    memcpy(s->var_regs,  var_regs,  sizeof(var_regs));
    s->var_count = var_count;                 var_count = 0;
    s->next_temp_register = next_temp_register;
    s->reserved_floor      = reserved_floor;
    s->function_depth = function_depth;       function_depth = 0;

    memcpy(s->global_names, global_names, sizeof(global_names));
    memcpy(s->global_regs,  global_regs,  sizeof(global_regs));
    s->global_count = global_count;           global_count = 0;

    s->func_names         = func_names;         func_names         = NULL;
    s->func_offsets        = func_offsets;       func_offsets        = NULL;
    s->func_arities        = func_arities;       func_arities        = NULL;
    s->func_min_arities    = func_min_arities;   func_min_arities    = NULL;
    s->func_defaults       = func_defaults;      func_defaults       = NULL;
    s->func_has_receiver   = func_has_receiver;  func_has_receiver   = NULL;
    s->func_receiver_type  = func_receiver_type; func_receiver_type  = NULL;
    s->func_count = func_count;                func_count = 0;
    s->func_cap   = func_cap;                  func_cap   = 0;

    s->pending_calls = pending_calls;          pending_calls = NULL;
    s->pending_count = pending_count;          pending_count = 0;
    s->pending_cap   = pending_cap;            pending_cap   = 0;

    memcpy(s->loop_stack, loop_stack, sizeof(loop_stack));
    s->loop_depth = loop_depth;                loop_depth = 0;

    s->struct_names = struct_names;            struct_names = NULL;
    s->struct_count = struct_count;             struct_count = 0;
    s->struct_cap   = struct_cap;               struct_cap   = 0;

    s->recovered_at_boundary = recovered_at_boundary;   recovered_at_boundary = false;

    reg_reset();
    return s;
}

/* Reinstates the outer file's exact compile-time state, and frees whatever the nested compile
   allocated for its own heap arrays — safe to free plainly (not deeply): each array holds only
   parse-time-scratch bookkeeping (this parser's OWN lookup tables), never the actual runtime data
   a running program depends on, which was separately persisted onto the nested file's own Chunk
   (chunk_add_function/OP_DEFINE_STRUCT's handler) before this call, and outlives this free(). */
void parser_restore_state(ParserState* s) {
    free(func_names);
    free(func_offsets);
    free(func_arities);
    free(func_min_arities);
    free(func_defaults);
    free(func_has_receiver);
    free(func_receiver_type);
    free(pending_calls);
    free(struct_names);

    memcpy(var_names, s->var_names, sizeof(var_names));
    memcpy(var_regs,  s->var_regs,  sizeof(var_regs));
    var_count = s->var_count;
    next_temp_register = s->next_temp_register;
    reserved_floor     = s->reserved_floor;
    function_depth     = s->function_depth;

    memcpy(global_names, s->global_names, sizeof(global_names));
    memcpy(global_regs,  s->global_regs,  sizeof(global_regs));
    global_count = s->global_count;

    func_names         = s->func_names;
    func_offsets       = s->func_offsets;
    func_arities       = s->func_arities;
    func_min_arities   = s->func_min_arities;
    func_defaults      = s->func_defaults;
    func_has_receiver  = s->func_has_receiver;
    func_receiver_type = s->func_receiver_type;
    func_count = s->func_count;
    func_cap   = s->func_cap;

    pending_calls = s->pending_calls;
    pending_count = s->pending_count;
    pending_cap   = s->pending_cap;

    memcpy(loop_stack, s->loop_stack, sizeof(loop_stack));
    loop_depth = s->loop_depth;

    struct_names = s->struct_names;
    struct_count = s->struct_count;
    struct_cap   = s->struct_cap;

    recovered_at_boundary = s->recovered_at_boundary;

    free(s);
}

/* Entry point. Per-statement rollback/skip-to-boundary error recovery: a compile error rolls back
   that one statement's partial bytecode and skips to the next line/dedent boundary instead of
   aborting the whole call, so a caller compiling one line/block at a time (a REPL, above all) can
   keep going after a mistake. any_compile_error tracks whether ANYTHING failed across the whole
   call, including errors already recovered from inside a nested if/for/function body (parse_block
   resets parse_had_error once it resyncs, so this call's own loop can't rely on that flag alone —
   see any_compile_error's own comment). Does not reset any of the parser's persistent name/register
   tables — see parser_reset's own comment for who does that and when.
     Once every statement in this call has compiled, anything still left in the forward-reference
   pending list (pending_call_add) never got defined anywhere in this call. A forward reference's
   failure can only be discovered here, at the very end — by then the statement that made the call
   already compiled "successfully" and was never rolled back, so a still-live OP_CALL/OP_DEFER_PUSH
   with a bogus target needs to be dealt with directly rather than via the per-statement rollback
   above. Each is reported at its own original call site via a saved/restored cursor override
   (lexer_set_cursor), then either: retargeted to OP_CALL_GLOBAL_VALUE/OP_TAIL_CALL_GLOBAL_VALUE if
   the name turned out to be an ordinary top-level variable holding a function value rather than a
   registered function (`greet` assigned before ever being called, say); or, if never defined at
   all, neutralized by overwriting the instruction's own packed opcode word with a bare OP_HALT —
   not just patching the offset to jump to OP_HALT elsewhere, which would still run the call's own
   arg-copy/frame-swap side effects first. Drained unconditionally so a later, separate parse() call
   always starts clean — forward references only resolve within one call, never across two. */
void parse(Chunk* c) {
    any_compile_error = false;
    while (!equal(TOKEN_END_OF_FILE)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        if (consume(TOKEN_DEDENT))   continue;
        parse_had_error          = false;
        recovered_at_boundary = false;
        unsigned int saved       = c->count;
        unsigned int saved_marks = c->line_mark_count;
        chunk_mark_line(c, saved, current_source_line());
        parse_statement(c);
        if (parse_had_error) {
            any_compile_error  = true;
            c->count           = saved;
            c->line_mark_count = saved_marks;
            if (!recovered_at_boundary) {
                while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_NEW_LINE) && !equal(TOKEN_DEDENT))
                    lex();
                skip_orphaned_block();
            }
        }
    }
    if (pending_count > 0) {
        const char* real_cursor = current_source_cursor();
        bool any_pending_error = false;
        for (int i = 0; i < pending_count; i++) {
            unsigned int patch_offset = pending_calls[i].patch_offset;
            int orig_op = c->code[patch_offset - 1] & 0xFF;
            int global_reg;
            if ((orig_op == OP_CALL || orig_op == OP_TAIL_CALL) &&
                global_lookup(pending_calls[i].name_idx, &global_reg)) {
                int new_op = (orig_op == OP_CALL) ? OP_CALL_GLOBAL_VALUE : OP_TAIL_CALL_GLOBAL_VALUE;
                c->code[patch_offset - 1] = (c->code[patch_offset - 1] & ~0xFF) | new_op;
                c->code[patch_offset] = global_reg;
                continue;
            }
            c->code[patch_offset - 1] = OP_HALT;
            lexer_set_cursor(pending_calls[i].call_site_cursor);
            error_at("Unknown function or struct type '%s' (never defined anywhere in this compile — not a valid forward reference, module call, or struct construction target)",
                     aer_as_string(c->pool[pending_calls[i].name_idx])->data);
            any_pending_error = true;
        }
        lexer_set_cursor(real_cursor);
        pending_count = 0;
        if (any_pending_error) any_compile_error = true;
    }
    if (any_compile_error) parse_had_error = true;
}

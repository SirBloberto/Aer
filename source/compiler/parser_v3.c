#include "parser_v3.h"
#include <string.h>
#include "error.h"
#include "lexer.h"

/* See parser_v3.h's comment — this whole file is M1's standalone prototype, not reachable from
   real .aer source. current_v3_watermark tracks the next free temp register; registers below
   reserved_floor are "locals" a test has already set up (via OP_V3_LOADK) and must never be
   handed out or freed here. */
static int v3_next_temp_register = 0;
static int v3_reserved_floor     = 0;

void v3_reg_reset(void) {
    v3_next_temp_register = 0;
    v3_reserved_floor     = 0;
}

void v3_reg_reserve(int count) {
    v3_reserved_floor     += count;
    v3_next_temp_register += count;
}

int v3_reg_alloc(void) {
    return v3_next_temp_register++;
}

void v3_reg_free(int count) {
    v3_next_temp_register -= count;
    /* Never free below the reserved floor — a bug in a future caller shouldn't be able to hand
       out a "local"'s register as if it were a free temp. */
    if (v3_next_temp_register < v3_reserved_floor) v3_next_temp_register = v3_reserved_floor;
}

int v3_compile_node(Chunk* c, V3Node* node) {
    if (node->kind == V3_NODE_CONST) {
        unsigned int pool_idx = chunk_add_pool(c, node->const_value);
        return (int)pool_idx | V3_RK_CONST_FLAG;
    }
    if (node->kind == V3_NODE_REG) {
        return node->reg;   /* already live — an RK "register" operand, not const-flagged */
    }

    int rk_lhs = v3_compile_node(c, node->lhs);
    int rk_rhs = v3_compile_node(c, node->rhs);

    /* Free operand registers (if this node allocated them — a CONST or an already-reserved REG
       leaf never does) BEFORE allocating the result's register, so the result reuses the lowest
       just-freed slot instead of growing the watermark further. Exactly Lua's own free-then-
       allocate discipline (lcode.c) — this is what keeps register usage compact across a deep
       expression tree instead of growing linearly with tree size. Only free slots THIS call
       allocated: a V3_NODE_REG leaf's register belongs to whatever reserved it, not to us. */
    if (!(rk_rhs & V3_RK_CONST_FLAG) && node->rhs->kind == V3_NODE_BINARY) v3_reg_free(1);
    if (!(rk_lhs & V3_RK_CONST_FLAG) && node->lhs->kind == V3_NODE_BINARY) v3_reg_free(1);

    int dest = v3_reg_alloc();
    chunk_emit(c, OP_V3_BINARY);
    chunk_emit(c, dest);
    chunk_emit(c, rk_lhs);
    chunk_emit(c, (int)node->bin_op);
    chunk_emit(c, rk_rhs);
    return dest;
}

unsigned int v3_emit_cmp_jump_false(Chunk* c, int rk_a, Opcode cmp_op, int rk_b) {
    chunk_emit(c, OP_V3_CMP_JUMP_FALSE);
    chunk_emit(c, rk_a);
    chunk_emit(c, (int)cmp_op);
    chunk_emit(c, rk_b);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by v3_patch_jump once the target is known */
    return patch_offset;
}

unsigned int v3_emit_jump_if_false_reg(Chunk* c, int reg) {
    chunk_emit(c, OP_V3_JUMP_IF_FALSE_REG);
    chunk_emit(c, reg);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);
    return patch_offset;
}

void v3_patch_jump(Chunk* c, unsigned int patch_offset, unsigned int target) {
    c->code[patch_offset] = (int)target;
}

void v3_emit_call(Chunk* c, int dest_reg, unsigned int callee_offset, int arg_reg_base, int arg_count) {
    chunk_emit(c, OP_V3_CALL);
    chunk_emit(c, dest_reg);
    chunk_emit(c, (int)callee_offset);
    chunk_emit(c, arg_reg_base);
    chunk_emit(c, arg_count);
}

void v3_emit_return(Chunk* c, int src_reg) {
    chunk_emit(c, OP_V3_RETURN);
    chunk_emit(c, src_reg);
}

void v3_emit_array_new(Chunk* c, int dest_reg, int item_reg_base, int item_count) {
    chunk_emit(c, OP_V3_ARRAY_NEW);
    chunk_emit(c, dest_reg);
    chunk_emit(c, item_reg_base);
    chunk_emit(c, item_count);
}

void v3_emit_index_get(Chunk* c, int dest_reg, int arr_reg, int rk_idx) {
    chunk_emit(c, OP_V3_INDEX_GET);
    chunk_emit(c, dest_reg);
    chunk_emit(c, arr_reg);
    chunk_emit(c, rk_idx);
}

void v3_emit_index_set(Chunk* c, int arr_reg, int rk_idx, int rk_val) {
    chunk_emit(c, OP_V3_INDEX_SET);
    chunk_emit(c, arr_reg);
    chunk_emit(c, rk_idx);
    chunk_emit(c, rk_val);
}

void v3_emit_dict_new(Chunk* c, int dest_reg, int pair_reg_base, int pair_count) {
    chunk_emit(c, OP_V3_DICT_NEW);
    chunk_emit(c, dest_reg);
    chunk_emit(c, pair_reg_base);
    chunk_emit(c, pair_count);
}

unsigned int v3_emit_iter_next_array(Chunk* c, int col_reg, int idx_reg, int item_dest_reg) {
    chunk_emit(c, OP_V3_ITER_NEXT_ARRAY);
    chunk_emit(c, col_reg);
    chunk_emit(c, idx_reg);
    chunk_emit(c, item_dest_reg);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by v3_patch_jump once the loop-exit target is known */
    return patch_offset;
}

unsigned int v3_emit_iter_range(Chunk* c, int cur_reg, int end_reg, int step_reg, int item_dest_reg) {
    chunk_emit(c, OP_V3_ITER_RANGE);
    chunk_emit(c, cur_reg);
    chunk_emit(c, end_reg);
    chunk_emit(c, step_reg);
    chunk_emit(c, item_dest_reg);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by v3_patch_jump once the loop-exit target is known */
    return patch_offset;
}

void v3_emit_struct_new(Chunk* c, int dest_reg, unsigned int type_name_pool_idx, int arg_reg_base, int arg_count) {
    chunk_emit(c, OP_V3_STRUCT_NEW);
    chunk_emit(c, dest_reg);
    chunk_emit(c, (int)type_name_pool_idx);
    chunk_emit(c, arg_reg_base);
    chunk_emit(c, arg_count);
}

void v3_emit_field_get(Chunk* c, int dest_reg, int struct_reg, unsigned int field_name_pool_idx) {
    chunk_emit(c, OP_V3_FIELD_GET);
    chunk_emit(c, dest_reg);
    chunk_emit(c, struct_reg);
    chunk_emit(c, (int)field_name_pool_idx);
}

void v3_emit_field_set(Chunk* c, int struct_reg, unsigned int field_name_pool_idx, int rk_val) {
    chunk_emit(c, OP_V3_FIELD_SET);
    chunk_emit(c, struct_reg);
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

static int  v3_parse_primary_inner(Chunk* c);
static int  v3_parse_primary(Chunk* c);
static int  v3_parse_string_literal(Chunk* c);
static int  v3_parse_unary_inner(Chunk* c);
static int  v3_parse_unary(Chunk* c);
static int  v3_parse_binary_ops(Chunk* c, unsigned int min_prec, int lhs);
static int  v3_parse_binary(Chunk* c, unsigned int min_prec);
static int  v3_compile_and(Chunk* c, int lhs, unsigned int prec);
static int  v3_compile_or(Chunk* c, int lhs, unsigned int prec);
static int  v3_compile_pipe(Chunk* c, int lhs);
static void v3_parse_statement(Chunk* c);
static void v3_parse_block(Chunk* c);
static void v3_parse_if(Chunk* c);
static void v3_parse_for_while(Chunk* c);
static void v3_parse_for_body(Chunk* c, unsigned int loop_top, int rk_cond);
static void v3_parse_assignment(Chunk* c, unsigned int name_idx);
static void v3_parse_index_assignment(Chunk* c, unsigned int name_idx);
static void v3_parse_field_assignment(Chunk* c, unsigned int name_idx);
static int  v3_parse_call(Chunk* c, unsigned int name_idx);
static void v3_parse_function(Chunk* c);
static void v3_parse_return(Chunk* c);
static void v3_parse_struct(Chunk* c);
static bool v3_at_module_name(Chunk* c);
static int  v3_parse_module_call(Chunk* c);
static void v3_parse_import(Chunk* c);
static void v3_parse_defer(Chunk* c);
static bool v3_is_builtin_name(Chunk* c, unsigned int name_idx);
static int  v3_parse_builtin_call(Chunk* c, unsigned int name_idx);
static int  v3_parse_contiguous_exprs(Chunk* c, TokenType close_tok, int* out_base);

/* name_idx -> register. v3_var_regs[i] holds the actual register for v3_var_names[i] — usually
   (but see v3_var_slot's comment for the one case where it isn't) the same as i itself. No
   global/local distinction (unlike parser.c's current_locals) — this slice has no function defs,
   so every register in the one frame a script runs in is effectively local. */
static unsigned int v3_var_names[V3_FRAME_REGISTERS];
static int          v3_var_regs[V3_FRAME_REGISTERS];
static int          v3_var_count = 0;

/* Nonzero while compiling a function body — lets v3_parse_return reject a top-level `return`,
   mirroring parser.c's own function_depth check (parser.c:1727). No nesting to track (named
   functions can't nest — matches the real language's own restriction, parser.c:1673-1682), so a
   plain counter (0 or 1) is enough, not a stack. Declared here (rather than nearer
   v3_parse_function, its main original use) because v3_var_slot below needs to check it too. */
static int v3_function_depth = 0;

/* Feature-completeness follow-up — top-level ("global") variable names, mirroring parser.c's own
   local-miss-falls-back-to-global read semantics (emit_load, parser.c:127-137): a name not found
   among the CURRENT function's own locals (v3_var_lookup) may still be a top-level variable,
   readable (never assignable — assignment inside a function is always local, matching parser.c's
   emit_store) from inside any function body. Updated in lockstep with v3_var_names by v3_var_slot
   itself, but ONLY while v3_function_depth == 0 — v3_var_names gets reset/restored around a
   function body (v3_parse_function), but this table doesn't, so it keeps accumulating "every
   top-level name defined so far" across a function def and back out again. A global defined AFTER
   a function that reads it (textually) won't be visible to that function — the same "no forward
   references" limitation v3 already has for functions/structs, not a new gap. */
static unsigned int v3_global_names[V3_FRAME_REGISTERS];
static int          v3_global_regs[V3_FRAME_REGISTERS];
static int          v3_global_count = 0;

/* Mirrors current_local_slot_or_alloc's exact shape (parser.c:114-124). A new variable's register
   is v3_reserved_floor — NOT v3_var_count, even though the two almost always hold the same value
   (both only ever move together, by exactly 1, right here). v3_reserved_floor is what actually
   marks "everything below this is permanent, never handed out as a temp" — v3_var_count is just
   this function's own bookkeeping of how many names it has seen. They can only diverge when
   something ELSE deliberately raises v3_reserved_floor without registering a new named variable —
   which is exactly what a for-loop's long-lived iteration registers now do (v3_parse_for_in's own
   comment): cur_reg/step_reg (or idx_reg/col_reg) must keep living for the loop's entire body, not
   just the one statement that computed them, so that loop temporarily promotes v3_reserved_floor
   past them before compiling its body. A brand-new variable declared inside that body must land
   ABOVE those promoted registers, which is exactly v3_reserved_floor's current value at that
   point — using v3_var_count there instead (this function's ORIGINAL implementation) would alias
   the loop's own iteration state, a real, silent-corruption bug found via nbody.aer's nested
   `for i in 0..n: for j in (i+1)..n:` (the inner loop's own `j` collided with the outer loop's own
   cur_reg). v3_next_temp_register is resynced to match immediately after — safe because parsing
   any single expression leaves at most one temp live above wherever v3_reserved_floor already was
   (this file's universal free-then-allocate discipline), so "collapse back down to the new floor"
   never discards a temp anything else still needs.
     Once reg/index do diverge for some name (e.g. a loop-body-local variable), v3_var_regs[] is
   what makes name lookup still resolve to the correct actual register — the array position (used
   for encounter-order bookkeeping) and the real register are no longer assumed to be the same
   number. */
static int v3_var_slot(unsigned int name_idx) {
    for (int i = 0; i < v3_var_count; i++)
        if (v3_var_names[i] == name_idx) return v3_var_regs[i];
    if (v3_var_count >= V3_FRAME_REGISTERS) {
        error_at("Too many variables for the v3 prototype (max %d)", V3_FRAME_REGISTERS);
        return -1;
    }
    int reg = v3_reserved_floor;
    v3_var_names[v3_var_count] = name_idx;
    v3_var_regs[v3_var_count]  = reg;
    v3_var_count++;
    v3_reserved_floor++;                        /* permanently protects this register from the temp allocator */
    v3_next_temp_register = v3_reserved_floor;   /* resync — see this function's own comment for why that's always safe */
    if (v3_function_depth == 0) {
        v3_global_names[v3_global_count] = name_idx;
        v3_global_regs[v3_global_count]  = reg;
        v3_global_count++;
    }
    return reg;
}

/* Non-creating — a name found here is a top-level variable, readable from inside a function body
   (see v3_global_names's own comment). */
static bool v3_global_lookup(unsigned int name_idx, int* out_reg) {
    for (int i = 0; i < v3_global_count; i++)
        if (v3_global_names[i] == name_idx) { *out_reg = v3_global_regs[i]; return true; }
    return false;
}

/* M5 slice 7 — non-creating counterpart to v3_var_slot, for compound assignment: unlike a plain
   `name = expr` (which may be defining `name` for the first time), `name += expr` needs `name` to
   already have a value to read, so it must not silently allocate a fresh (zero-garbage) register
   on a miss the way v3_var_slot does. */
static bool v3_var_lookup(unsigned int name_idx, int* out_reg) {
    for (int i = 0; i < v3_var_count; i++)
        if (v3_var_names[i] == name_idx) { *out_reg = v3_var_regs[i]; return true; }
    return false;
}

/* Register-value equivalent of v3_compile_node's "was this a V3_NODE_BINARY" check (only
   meaningful for a hand-built tree) — works for any RK operand since a temp always lives at/above
   the reserved floor, a permanent variable register always below it. static int
   v3_reserved_floor's declaration (top of this file) makes this valid here. */
static bool v3_is_temp(int rk) {
    return !(rk & V3_RK_CONST_FLAG) && rk >= v3_reserved_floor;
}

/* Ensures rk is a plain register (not RK-const), materializing a constant into a fresh temp via
   OP_V3_LOADK if needed — OP_V3_JUMP_IF_FALSE_REG needs an actual register operand, no RK form. */
static int v3_materialize(Chunk* c, int rk) {
    if (!(rk & V3_RK_CONST_FLAG)) return rk;
    int reg = v3_reg_alloc();
    chunk_emit(c, OP_V3_LOADK);
    chunk_emit(c, reg);
    chunk_emit(c, rk & ~V3_RK_CONST_FLAG);
    return reg;
}

/* M5 slice 3 — function name -> instruction offset, separate from v3_var_names (a function needs
   a jump target, not a register). Functions are NOT first-class values in this slice (no `f =
   square; f(5)`, no passing a function as an argument) — a real fix needs OP_V3_CALL's value-based
   counterpart (like the stack VM's OP_CALL_VALUE), out of scope here. */
#define V3_FUNC_MAX 16
static unsigned int v3_func_names[V3_FUNC_MAX];
static unsigned int v3_func_offsets[V3_FUNC_MAX];
static int          v3_func_count = 0;

static bool v3_func_lookup(unsigned int name_idx, unsigned int* out_offset) {
    for (int i = 0; i < v3_func_count; i++)
        if (v3_func_names[i] == name_idx) { *out_offset = v3_func_offsets[i]; return true; }
    return false;
}

/* M5 slice 8 — break/continue. Mirrors parser.c's LoopContext (parser.c:40-45) but simpler: no
   `iter_slots` field at all, since v3 has nothing analogous to pop — a for-in loop's iterator
   state ([col_reg, idx_reg]) lives in ordinary registers the caller already owns, not extra
   value-stack slots that need balancing on an early exit (see OP_V3_ITER_NEXT_ARRAY's own comment
   in vm.h). `break` is therefore just "emit a jump, patch it once the loop's exit address is
   known" with no cleanup step first. */
#define V3_LOOP_MAX  16
#define V3_BREAK_MAX 32
typedef struct {
    unsigned int top;                     /* continue's target — same value passed to v3_parse_for_body/for_in */
    unsigned int patches[V3_BREAK_MAX];   /* break's OP_JUMP operand offsets, patched once the loop ends */
    int          patch_count;
} V3LoopContext;
static V3LoopContext v3_loop_stack[V3_LOOP_MAX];
static int           v3_loop_depth = 0;

static bool v3_loop_push(unsigned int top) {
    if (v3_loop_depth >= V3_LOOP_MAX) {
        error_at("Too many nested loops for the v3 prototype (max %d)", V3_LOOP_MAX);
        return false;
    }
    v3_loop_stack[v3_loop_depth].top         = top;
    v3_loop_stack[v3_loop_depth].patch_count = 0;
    v3_loop_depth++;
    return true;
}

/* Patches every pending `break` recorded for the just-finished (innermost) loop to land at
   `exit_target` (the same address the loop's own condition-false exit already jumps to), then
   pops the loop context. */
static void v3_loop_pop_and_patch(Chunk* c, unsigned int exit_target) {
    V3LoopContext* ctx = &v3_loop_stack[v3_loop_depth - 1];
    for (int i = 0; i < ctx->patch_count; i++)
        v3_patch_jump(c, ctx->patches[i], exit_target);
    v3_loop_depth--;
}

/* M5 slice 6 — struct type name registry, separate from v3_func_names (a struct type isn't a
   callable offset — it resolves at runtime via chunk_find_shape(), OP_V3_STRUCT_NEW just needs to
   know AT COMPILE TIME that `Name(...)` means "construct", not "call", mirroring the stack VM's
   own OP_CALL-does-double-duty precedent one level up, at the v3_parse_call dispatch point rather
   than inside a single runtime opcode). */
#define V3_STRUCT_MAX 16
static unsigned int v3_struct_names[V3_STRUCT_MAX];
static int          v3_struct_count = 0;

/* Feature-completeness follow-up — max targets in a destructuring assignment (`a, b, c = ...`),
   mirroring parser.c's own MAX_DESTRUCT (parser.c:234), just not shared since that one is private
   to parser.c. */
#define V3_MAX_DESTRUCT 16

static bool v3_is_struct_name(unsigned int name_idx) {
    for (int i = 0; i < v3_struct_count; i++)
        if (v3_struct_names[i] == name_idx) return true;
    return false;
}

/* Forces rk into exactly the CURRENT allocator watermark, needed because OP_V3_CALL's arguments
   must land in contiguous registers (its bulk-copy reads v3_registers[arg_reg_base..+arg_count)).
   Feature-completeness fix: a fresh, just-computed temp (e.g. `1.66e-3 * DAYS_PER_YEAR` as one of
   several struct-constructor args) must be recognized and reused DIRECTLY here, not copied into a
   NEW register via v3_reg_alloc() first — calling v3_reg_alloc() unconditionally (the original
   implementation) allocates one register PAST rk (since parsing that expression already bumped
   the watermark to rk+1), then finds `rk != target` and emits a MOVE, permanently abandoning rk's
   own register as a gap in the sequence. For a single argument this is harmless waste (matches the
   "known, honest inefficiency" documented elsewhere in this file); for the SECOND or later argument
   in a multi-arg list, that one-register gap breaks contiguity outright — the bulk-copy opcode
   reads `arg_reg_base + i` for each i and silently gets a neighboring argument's leftover value or
   worse. Found via nbody.aer's Body() constructor calls, several of whose args are `const *
   DAYS_PER_YEAR`/`const * SOLAR_MASS` expressions: with 2 bodies this happened to go unnoticed
   (nothing after the last argument to misalign), but a 3rd body's construction (or reading back an
   earlier field two positions removed) exposed values shifted by exactly one register.
     rk is already the topmost live temp whenever it's a temp at all (this file's universal
   free-then-allocate discipline never leaves more than one live temp above the pre-expression
   watermark) — so if `v3_is_temp(rk) && rk == v3_next_temp_register - 1`, rk IS already sitting
   exactly where a fresh allocation would only end up copying it to anyway; reusing it directly
   costs nothing and needs no MOVE. Only a bare constant or an existing PERMANENT variable register
   (neither of which touched the temp watermark at all) still needs an actual fresh register. */
static int v3_arg_materialize(Chunk* c, int rk) {
    if (!(rk & V3_RK_CONST_FLAG) && v3_is_temp(rk) && rk == v3_next_temp_register - 1) {
        return rk;
    }
    int target = v3_reg_alloc();
    if (rk & V3_RK_CONST_FLAG) {
        chunk_emit(c, OP_V3_LOADK);
        chunk_emit(c, target);
        chunk_emit(c, rk & ~V3_RK_CONST_FLAG);
    } else {
        chunk_emit(c, OP_V3_MOVE);
        chunk_emit(c, target);
        chunk_emit(c, rk);
    }
    return target;
}

/* M5 slice 4 — shared by call arguments, array literals, and (per-key/value) dict literals: all
   three bulk-copy opcodes (OP_V3_CALL/OP_V3_ARRAY_NEW/OP_V3_DICT_NEW) need their operands in
   contiguous registers. Parses a comma-separated expression list up to (not including) close_tok
   — the caller still requires() close_tok itself, since a dict's pairs need extra ':' handling
   this bare list can't express. Returns the count; *out_base is unspecified when count==0. */
static int v3_parse_contiguous_exprs(Chunk* c, TokenType close_tok, int* out_base) {
    int base  = -1;
    int count = 0;
    if (!equal(close_tok)) {
        do {
            int rk  = v3_parse_binary(c, 0);
            int reg = v3_arg_materialize(c, rk);
            if (count == 0) base = reg;
            count++;
        } while (consume(TOKEN_COMMA));
    }
    *out_base = base;
    return count;
}

/* M5 slice 4 — plain string literals only (no `"...{name}..."` interpolation: that's a real,
   separate parser.c feature, out of scope here). Mirrors parser.c's own pool_escaped_string
   (parser.c:826-854) exactly for escape-sequence handling; duplicated rather than shared since
   that helper is static to parser.c and this file deliberately shares no code with it. Needed now
   (not deferred to a later "strings" slice) because dict literals' keys must be strings — there's
   no way to test M5 slice 4's dict-literal support from real source without it. */
static unsigned int v3_pool_escaped_string(Chunk* c, const char* s, unsigned int len) {
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

/* Subset of parser.c's binary_precedence[]/binary_ops[] table (parser.c:10-30), same relative
   precedence numbers. M5 slice 7 added and/or (handled specially in v3_parse_binary_ops, not via
   plain OP_V3_BINARY — see v3_compile_and/v3_compile_or) and bitwise/shift (free additions —
   OP_V3_BINARY already delegates to the same vm_binary() the stack VM uses for any operator, so
   these are pure table entries). M5 slice 9 added `in` — also a free addition, since OP_IN routes
   through the exact same generic lbl_binary/vm_binary path as every other operator in the stack
   VM (confirmed via vm.c's dispatch table: `[OP_IN] = &&lbl_binary`) — and `as`, handled specially
   (see v3_parse_binary_ops) since its "right-hand side" is a bare type name, not a normal
   expression. Still excluded: `|>` (pipe) — needs call-argument prepending, its own mechanism,
   not built for v3 yet. Returns false for any operator outside this subset. */
static bool v3_binary_op_info(TokenType t, unsigned int* prec, Opcode* op) {
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

/* M5 slice 9 — string literals with `{name}` interpolation, mirroring parse_primary_inner's
   TOKEN_STRING handling (parser.c:932-991) in shape: same has-interpolation pre-scan, same
   segment/interpolation-run loop, same "no parts -> empty string" fallback. Differs in mechanism,
   not behavior: literal segments and interpolated values concatenate via OP_V3_BINARY(OP_ADD)
   into registers instead of pushing onto the stack; a `{name}`'s value-to-string step goes through
   OP_V3_UNARY's folded-in OP_TO_STR case instead of the stack VM's separate OP_TO_STR opcode.
   Interpolated names must already be a defined v3 variable (v3_var_lookup, non-creating) — same
   compile-time-resolution stance slice 7's compound assignment already takes, since v3 has no
   runtime scope-chain fallback the way the real language's `emit_load` does. */
static int v3_parse_string_literal(Chunk* c) {
    AerString* ts    = aer_as_string(token.value);
    char*        s   = ts->data;
    unsigned int len = ts->length;

    bool has_interp = false;
    for (unsigned int k = 0; k < len; k++) {
        if (s[k] == '\\' && k + 1 < len) { k++; continue; }
        if (s[k] == '{') { has_interp = true; break; }
    }

    if (!has_interp) {
        unsigned int pool_idx = v3_pool_escaped_string(c, s, len);
        lex();
        return (int)pool_idx | V3_RK_CONST_FLAG;
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
            int rk_seg = (int)v3_pool_escaped_string(c, s + seg_start, i - seg_start) | V3_RK_CONST_FLAG;
            if (result < 0) {
                result = v3_materialize(c, rk_seg);
            } else {
                if (v3_is_temp(result)) v3_reg_free(1);
                int dest = v3_reg_alloc();
                chunk_emit(c, OP_V3_BINARY); chunk_emit(c, dest); chunk_emit(c, result);
                chunk_emit(c, (int)OP_ADD); chunk_emit(c, rk_seg);
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
        if (!v3_var_lookup(name_pool_idx, &var_reg)) {
            error_at("'%.*s' is not defined (v3 prototype: an interpolated name must already have a value)",
                     (int)name_len, s + var_start);
            i++;
            continue;
        }

        int str_dest = v3_reg_alloc();
        chunk_emit(c, OP_V3_UNARY); chunk_emit(c, str_dest); chunk_emit(c, (int)OP_TO_STR); chunk_emit(c, var_reg);

        if (result < 0) {
            result = str_dest;
        } else {
            if (v3_is_temp(str_dest)) v3_reg_free(1);
            if (v3_is_temp(result))   v3_reg_free(1);
            int dest = v3_reg_alloc();
            chunk_emit(c, OP_V3_BINARY); chunk_emit(c, dest); chunk_emit(c, result);
            chunk_emit(c, (int)OP_ADD); chunk_emit(c, str_dest);
            result = dest;
        }
        i++;   /* skip '}' */
    }

    if (result < 0) {
        /* No parts at all (matches parser.c's own parts==0 fallback) — a fresh, owned empty
           buffer, not a static literal (AerString always owns its data). */
        char* empty_buf = xmalloc(1);
        empty_buf[0] = '\0';
        result = (int)chunk_add_pool(c, aer_make_string(empty_buf, 0)) | V3_RK_CONST_FLAG;
    }

    lex();
    return result;
}

/* Literal/identifier/parenthesized/array-literal/dict-literal/call primary. Unary operators
   (`-x`/`!x`/`~x`) sit ABOVE this in the precedence chain — see v3_parse_unary — matching the real
   grammar's parse_binary -> parse_unary -> parse_primary structure. */
static int v3_parse_primary_inner(Chunk* c) {
    if (consume(TOKEN_OPEN_PARENTHESE)) {
        int rk = v3_parse_binary(c, 0);
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after expression");
        return rk;
    }
    /* M5 slice 4 — array literal. Elements share the same contiguous-register-materialization
       helper call arguments use (v3_parse_contiguous_exprs), then OP_V3_ARRAY_NEW (M4) — the
       result reuses the first element's register (freeing the rest), same compact-reuse
       convention as a call's result. */
    if (consume(TOKEN_OPEN_BRACKET)) {
        int item_reg_base;
        int item_count = v3_parse_contiguous_exprs(c, TOKEN_CLOSE_BRACKET, &item_reg_base);
        require(TOKEN_CLOSE_BRACKET, "expected ']' after array literal");
        if (parse_had_error) return 0;
        int dest = (item_count > 0) ? item_reg_base : v3_reg_alloc();
        if (item_count > 1) v3_reg_free(item_count - 1);
        v3_emit_array_new(c, dest, item_reg_base < 0 ? dest : item_reg_base, item_count);
        return dest;
    }
    /* Dict literal — not a bare comma list (each item is a key:value pair), so this doesn't reuse
       v3_parse_contiguous_exprs; otherwise the exact same contiguous-materialize-then-bulk-copy
       shape, now via OP_V3_DICT_NEW (M4). Key then value are materialized back to back, so
       pair i's key/value land at exactly pair_reg_base+2i/+2i+1, matching that opcode's layout. */
    if (consume(TOKEN_OPEN_BRACE)) {
        int pair_reg_base = -1;
        int pair_count = 0;
        if (!equal(TOKEN_CLOSE_BRACE)) {
            do {
                int rk_key  = v3_parse_binary(c, 0);
                int reg_key = v3_arg_materialize(c, rk_key);
                if (pair_count == 0) pair_reg_base = reg_key;
                require(TOKEN_COLON, "expected ':' after dict key");
                if (parse_had_error) return 0;
                int rk_val = v3_parse_binary(c, 0);
                v3_arg_materialize(c, rk_val);
                pair_count++;
            } while (consume(TOKEN_COMMA));
        }
        require(TOKEN_CLOSE_BRACE, "expected '}' after dict literal");
        if (parse_had_error) return 0;
        int dest = (pair_count > 0) ? pair_reg_base : v3_reg_alloc();
        if (pair_count > 1) v3_reg_free(2 * pair_count - 1);
        v3_emit_dict_new(c, dest, pair_reg_base < 0 ? dest : pair_reg_base, pair_count);
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
        return (int)chunk_add_pool(c, v) | V3_RK_CONST_FLAG;
    }
    if (token.type == TOKEN_STRING) return v3_parse_string_literal(c);
    if (v3_at_module_name(c)) return v3_parse_module_call(c);
    if (token.type == TOKEN_IDENTIFIER) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_OPEN_PARENTHESE)) return v3_parse_call(c, name_idx);

        /* Feature-completeness follow-up — a name not among the CURRENT function's own locals may
           still be a top-level variable, readable via OP_V3_LOAD_GLOBAL (see v3_global_names's own
           comment). Checked only when v3_var_lookup (non-creating) misses, so a local always
           shadows a global of the same name, matching parser.c's own local-first resolution. */
        int reg;
        if (v3_var_lookup(name_idx, &reg)) return reg;
        if (v3_function_depth > 0) {
            int global_reg;
            if (v3_global_lookup(name_idx, &global_reg)) {
                int dest = v3_reg_alloc();
                chunk_emit(c, OP_V3_LOAD_GLOBAL);
                chunk_emit(c, dest);
                chunk_emit(c, global_reg);
                return dest;
            }
        }

        reg = v3_var_slot(name_idx);   /* not a known local or global — existing prototype
                                           permissiveness: silently creates a fresh local (unchanged) */
        return reg < 0 ? 0 : reg;      /* reg<0: error_at already called */
    }
    error_at("Expected an expression (v3 prototype: only literals, variables, calls, array/dict literals, arithmetic/comparisons, and parentheses)");
    return 0;
}

/* M5 slice 4 — postfix `[index]` chain (arbitrarily deep reads: `matrix[i][j]` works for free,
   since each step's result just feeds back into the next). Only a single-level WRITE
   (`arr[i] = v`) is supported, dispatched separately from v3_parse_statement — chained/nested
   index writes need the same GET-until-the-last-step-then-SET shape parser.c's parse_assignment
   has (parser.c:352-398), out of scope for this slice. */
static int v3_parse_primary(Chunk* c) {
    int rk = v3_parse_primary_inner(c);
    for (;;) {
        if (consume(TOKEN_OPEN_BRACKET)) {
            int rk_idx = v3_parse_binary(c, 0);
            require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
            if (parse_had_error) return rk;

            int arr_reg = v3_materialize(c, rk);

            /* Free-then-allocate, matching v3_parse_binary_ops's own discipline. */
            if (v3_is_temp(rk_idx))  v3_reg_free(1);
            if (v3_is_temp(arr_reg)) v3_reg_free(1);

            int dest = v3_reg_alloc();
            v3_emit_index_get(c, dest, arr_reg, rk_idx);
            rk = dest;
            continue;
        }
        /* M5 slice 6 — postfix '.field' read, same shape as '[...]' above: p.x and chained forms
           like p.pos.x work for free, since each step's result feeds back into the next. */
        if (consume(TOKEN_DOT)) {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return rk; }
            unsigned int field_idx = chunk_add_pool(c, token.value);
            lex();

            int struct_reg = v3_materialize(c, rk);
            if (v3_is_temp(struct_reg)) v3_reg_free(1);

            int dest = v3_reg_alloc();
            v3_emit_field_get(c, dest, struct_reg, field_idx);
            rk = dest;
            continue;
        }
        break;
    }
    return rk;
}

/* M5 slice 7 — unary operators, mirroring parse_unary/parse_unary_inner (parser.c:812-823)
   exactly: each recurses into v3_parse_unary again (not v3_parse_primary), so chained unary
   (`!!x`, `--x`, `~~x`) works, falling to v3_parse_primary only once no more prefix operators
   apply. rk is RK-encoded like OP_V3_BINARY's operands — no need to materialize a constant into a
   register first, OP_V3_UNARY decodes it directly via vm_v3_rk_value. */
static int v3_parse_unary_inner(Chunk* c) {
    Opcode unary_op;
    if      (consume(TOKEN_NOT))         unary_op = OP_NOT;
    else if (consume(TOKEN_BITWISE_NOT)) unary_op = OP_BITWISE_NOT;
    else if (consume(TOKEN_SUBTRACT))    unary_op = OP_NEGATE;
    else return v3_parse_primary(c);

    int rk = v3_parse_unary(c);
    if (v3_is_temp(rk)) v3_reg_free(1);   /* free-then-allocate, matching every other site */
    int dest = v3_reg_alloc();
    chunk_emit(c, OP_V3_UNARY);
    chunk_emit(c, dest);
    chunk_emit(c, (int)unary_op);
    chunk_emit(c, rk);
    return dest;
}

static int v3_parse_unary(Chunk* c) {
    return v3_parse_unary_inner(c);
}

/* M5 slice 7 — `lhs and rhs`, short-circuit: rhs is only evaluated if lhs is truthy. Uses only
   v3_emit_jump_if_false_reg (no dedicated opcode) — both the lhs-false and rhs-false paths land
   on the same "result = false" code, since either one alone is enough to decide the outcome.
   `dest` is allocated once, after both operands' temps are freed, and both outcome paths
   (true/false) write into that same register — safe because only one path ever executes at
   runtime. */
static int v3_compile_and(Chunk* c, int lhs, unsigned int prec) {
    int reg_lhs = v3_materialize(c, lhs);
    unsigned int patch_false_a = v3_emit_jump_if_false_reg(c, reg_lhs);
    if (v3_is_temp(reg_lhs)) v3_reg_free(1);

    int rk_rhs = v3_parse_binary(c, prec);
    int reg_rhs = v3_materialize(c, rk_rhs);
    unsigned int patch_false_b = v3_emit_jump_if_false_reg(c, reg_rhs);
    if (v3_is_temp(reg_rhs)) v3_reg_free(1);

    int dest = v3_reg_alloc();
    unsigned int pool_true = chunk_add_pool(c, aer_bool(true));
    chunk_emit(c, OP_V3_LOADK); chunk_emit(c, dest); chunk_emit(c, (int)pool_true);
    chunk_emit(c, OP_JUMP);
    unsigned int patch_end = c->count; chunk_emit(c, 0);

    v3_patch_jump(c, patch_false_a, c->count);
    v3_patch_jump(c, patch_false_b, c->count);
    unsigned int pool_false = chunk_add_pool(c, aer_bool(false));
    chunk_emit(c, OP_V3_LOADK); chunk_emit(c, dest); chunk_emit(c, (int)pool_false);

    v3_patch_jump(c, patch_end, c->count);
    return dest;
}

/* M5 slice 7 — `lhs or rhs`, short-circuit: rhs is only evaluated if lhs is falsy. The stack VM's
   OP_OR has a dedicated OP_JUMP_IF_TRUE for this shape; v3 has no jump-if-true opcode, so this is
   restructured to use only jump-if-false — lhs-false jumps forward into a "check rhs" block
   instead of jumping past a jump-if-true, otherwise the same "one dest, two outcome paths"
   discipline as v3_compile_and. */
static int v3_compile_or(Chunk* c, int lhs, unsigned int prec) {
    int reg_lhs = v3_materialize(c, lhs);
    unsigned int patch_check_rhs = v3_emit_jump_if_false_reg(c, reg_lhs);
    if (v3_is_temp(reg_lhs)) v3_reg_free(1);

    int dest = v3_reg_alloc();
    unsigned int pool_true = chunk_add_pool(c, aer_bool(true));
    chunk_emit(c, OP_V3_LOADK); chunk_emit(c, dest); chunk_emit(c, (int)pool_true);
    chunk_emit(c, OP_JUMP);
    unsigned int patch_end_a = c->count; chunk_emit(c, 0);

    v3_patch_jump(c, patch_check_rhs, c->count);
    int rk_rhs = v3_parse_binary(c, prec);
    int reg_rhs = v3_materialize(c, rk_rhs);
    unsigned int patch_result_false = v3_emit_jump_if_false_reg(c, reg_rhs);
    if (v3_is_temp(reg_rhs)) v3_reg_free(1);

    chunk_emit(c, OP_V3_LOADK); chunk_emit(c, dest); chunk_emit(c, (int)pool_true);
    chunk_emit(c, OP_JUMP);
    unsigned int patch_end_b = c->count; chunk_emit(c, 0);

    v3_patch_jump(c, patch_result_false, c->count);
    unsigned int pool_false = chunk_add_pool(c, aer_bool(false));
    chunk_emit(c, OP_V3_LOADK); chunk_emit(c, dest); chunk_emit(c, (int)pool_false);

    v3_patch_jump(c, patch_end_a, c->count);
    v3_patch_jump(c, patch_end_b, c->count);
    return dest;
}

/* M5 slice 10 — `x |> f(args)` desugars to f(x, args), matching parser.c's OP_PIPE handling
   (parser.c:710-751): the piped value becomes argument zero, ahead of whatever's inside the
   parentheses. Reuses v3_parse_call's own function/struct resolution and v3_arg_materialize's
   contiguous-register discipline directly rather than duplicating it — the only new part is
   materializing `lhs` into the argument-zero slot before parsing the rest of the list. Unlike
   parser.c, there's no `pipe_forbid_calls` restriction modeled here (v3 has no such flag at all
   yet, for any call arguments) — out of scope for this prototype slice. Module-qualified pipe
   targets (`x |> string.upper()`) are out of scope too: v3 has no module/import system yet (next
   items on the roadmap) — reported as a clean compile-time error instead of silently mishandled. */
static int v3_compile_pipe(Chunk* c, int lhs) {
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after '|>'"); return lhs; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();

    if (equal(TOKEN_DOT)) {
        error_at("Module-qualified pipe calls ('x |> module.fn()') are not yet supported in the v3 prototype (no module system yet)");
        return lhs;
    }

    bool is_struct = v3_is_struct_name(name_idx);
    unsigned int func_offset = 0;
    if (!is_struct && !v3_func_lookup(name_idx, &func_offset)) {
        error_at("Unknown function or struct type (v3 prototype requires it to be defined before any use)");
        return lhs;
    }

    require(TOKEN_OPEN_PARENTHESE, "expected '(' after piped function name");
    if (parse_had_error) return lhs;

    if (v3_is_temp(lhs)) v3_reg_free(1);
    int arg_reg_base = v3_arg_materialize(c, lhs);
    int arg_count = 1;
    if (!equal(TOKEN_CLOSE_PARENTHESE)) {
        do {
            int rk = v3_parse_binary(c, 0);
            v3_arg_materialize(c, rk);
            arg_count++;
        } while (consume(TOKEN_COMMA));
    }
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after pipe call arguments");
    if (parse_had_error) return lhs;

    int dest = arg_reg_base;
    if (arg_count > 1) v3_reg_free(arg_count - 1);
    if (is_struct) v3_emit_struct_new(c, dest, name_idx, arg_reg_base, arg_count);
    else           v3_emit_call(c, dest, func_offset, arg_reg_base, arg_count);
    return dest;
}

/* Precedence-climbing loop, `lhs` already parsed — mirrors parser.c's parse_binary_ops/parse_binary
   split (parser.c:673-810) exactly, so a for-while condition that starts with an identifier can
   resolve that identifier once (see v3_parse_for_while) and climb from there, same as parser.c's
   own for-loop does for its while-fallback form. */
static int v3_parse_binary_ops(Chunk* c, unsigned int min_prec, int lhs) {
    for (;;) {
        unsigned int prec;
        Opcode op;
        if (!v3_binary_op_info(token.type, &prec, &op) || prec <= min_prec) break;
        lex();

        if (op == OP_AND)  { lhs = v3_compile_and(c, lhs, prec); continue; }
        if (op == OP_OR)   { lhs = v3_compile_or(c, lhs, prec);  continue; }
        if (op == OP_PIPE) { lhs = v3_compile_pipe(c, lhs);      continue; }

        /* `x as T` — T is a bare type name, read directly rather than through v3_parse_binary,
           mirroring parser.c's own OP_CAST handling (parser.c:752-765). Struct-shape casting
           (anything not a known primitive) has no v3 opcode yet (see OP_V3_CAST's comment in
           vm.h) so it's a compile-time error here rather than the stack VM's runtime
           OP_CHECK_SHAPE. */
        if (op == OP_CAST) {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a type name after 'as'"); return lhs; }
            const char* type_name = aer_as_string(token.value)->data;
            unsigned int type_len = aer_as_string(token.value)->length;
            lex();

            if (v3_is_temp(lhs)) v3_reg_free(1);
            int dest = v3_reg_alloc();

            if (type_len == 6 && strncmp(type_name, "string", 6) == 0) {
                chunk_emit(c, OP_V3_UNARY);
                chunk_emit(c, dest);
                chunk_emit(c, (int)OP_TO_STR);
                chunk_emit(c, lhs);
            } else {
                int cast_type;
                if      (type_len == 7 && strncmp(type_name, "integer", 7) == 0) cast_type = CAST_INTEGER;
                else if (type_len == 5 && strncmp(type_name, "float",   5) == 0) cast_type = CAST_FLOAT;
                else if (type_len == 7 && strncmp(type_name, "boolean", 7) == 0) cast_type = CAST_BOOLEAN;
                else {
                    error_at("v3 prototype only supports casting to string/integer/float/boolean (struct-shape casting via 'as' is not yet supported)");
                    return dest;
                }
                chunk_emit(c, OP_V3_CAST);
                chunk_emit(c, dest);
                chunk_emit(c, cast_type);
                chunk_emit(c, lhs);
            }
            lhs = dest;
            continue;
        }

        int rhs = v3_parse_binary(c, prec);   /* same precedence as floor -> left-associative */

        /* Free-then-allocate, RHS then LHS, matching v3_compile_node's own discipline exactly. */
        if (v3_is_temp(rhs)) v3_reg_free(1);
        if (v3_is_temp(lhs)) v3_reg_free(1);

        int dest = v3_reg_alloc();
        chunk_emit(c, OP_V3_BINARY);
        chunk_emit(c, dest);
        chunk_emit(c, lhs);
        chunk_emit(c, (int)op);
        chunk_emit(c, rhs);
        lhs = dest;
    }
    return lhs;
}

static int v3_parse_binary(Chunk* c, unsigned int min_prec) {
    int lhs = v3_parse_unary(c);
    return v3_parse_binary_ops(c, min_prec, lhs);
}

/* M5 slice 7 — mirrors parser.c's compound_assign_ops[] (parser.c:327-339) exactly, all 11
   operators. Unlike the stack VM's OP_COMPOUND_* family (which exists purely to avoid stack
   push/pop overhead a fused opcode saves), v3 needs no fusion or new opcode at all: `x OP= expr`
   just compiles to one OP_V3_BINARY with dest == lhs == x's own register — already as efficient
   as it gets in a register machine. */
static const struct { TokenType tok; Opcode op; } v3_compound_assign_ops[] = {
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
#define V3_COMPOUND_ASSIGN_OP_COUNT (int)(sizeof(v3_compound_assign_ops) / sizeof(*v3_compound_assign_ops))

/* `name = expr` or `name OP= expr` — no indexed/field targets (out of scope, same limitation
   indexed/field writes already have). `name` is already consumed by the caller
   (v3_parse_statement), which must decide between this, a bare call statement, and an indexed
   write first — all start with an identifier, and only one token of lookahead distinguishes them,
   so the identifier can't be re-consumed here. */
static void v3_parse_assignment(Chunk* c, unsigned int name_idx) {
    /* Feature-completeness follow-up — destructuring: `a, b, c = expr` or `a, b = x, y` (implicit
       array RHS), mirroring parser.c's own destructuring block (parser.c:541-568) in outcome, but
       reusing existing v3 machinery instead of a dedicated OP_UNPACK: multiple RHS values are
       packed into a real array (OP_V3_ARRAY_NEW, same as any array literal) and a single RHS
       expression is assumed to already evaluate to one; every target then reads its own index
       back out via OP_V3_INDEX_GET, which gives the exact same runtime bounds-checking behavior
       as OP_UNPACK for free — no new opcode needed at all.
         Target registers are resolved via v3_var_slot BEFORE the RHS is parsed — the same
       ordering requirement v3_parse_for_in's loop variable has (see its own comment): v3's temps
       and permanent variables share one register-numbering space, so creating a brand-new
       variable AFTER a temp is already live could hand that temp's own register out from under
       it (v3_var_slot's v3_reg_reserve unconditionally claims "the next" register, which is
       exactly wherever the temp allocator's watermark currently sits). */
    if (equal(TOKEN_COMMA)) {
        unsigned int names[V3_MAX_DESTRUCT];
        names[0] = name_idx;
        unsigned int count = 1;
        while (consume(TOKEN_COMMA)) {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected identifier in destructuring"); return; }
            if (count >= V3_MAX_DESTRUCT) { error_at("Too many destructuring targets (max %d)", V3_MAX_DESTRUCT); return; }
            names[count++] = chunk_add_pool(c, token.value);
            lex();
        }
        require(TOKEN_ASSIGN, "expected '=' after destructuring targets");
        if (parse_had_error) return;

        int target_regs[V3_MAX_DESTRUCT];
        for (unsigned int i = 0; i < count; i++) {
            target_regs[i] = v3_var_slot(names[i]);
            if (target_regs[i] < 0) return;   /* error_at already called */
        }

        int rk_first = v3_parse_binary(c, 0);
        if (parse_had_error) return;
        int rhs_reg_base = v3_arg_materialize(c, rk_first);
        unsigned int rhs_count = 1;
        while (consume(TOKEN_COMMA)) {
            int rk_next = v3_parse_binary(c, 0);
            if (parse_had_error) return;
            v3_arg_materialize(c, rk_next);
            rhs_count++;
        }

        int arr_reg = rhs_reg_base;
        if (rhs_count > 1) {
            v3_reg_free((int)rhs_count - 1);
            v3_emit_array_new(c, arr_reg, rhs_reg_base, (int)rhs_count);
        }

        for (unsigned int i = 0; i < count; i++) {
            unsigned int pool_i = chunk_add_pool(c, aer_int((long long)i));
            v3_emit_index_get(c, target_regs[i], arr_reg, (int)pool_i | V3_RK_CONST_FLAG);
        }
        v3_reg_free(1);   /* arr_reg — always a temp, guaranteed by v3_arg_materialize */
        return;
    }

    if (consume(TOKEN_ASSIGN)) {
        int rk_val = v3_parse_binary(c, 0);
        if (parse_had_error) return;
        int reg = v3_var_slot(name_idx);
        if (reg < 0) return;   /* error_at already called */
        if (rk_val & V3_RK_CONST_FLAG) {
            chunk_emit(c, OP_V3_LOADK);
            chunk_emit(c, reg);
            chunk_emit(c, rk_val & ~V3_RK_CONST_FLAG);
        } else {
            chunk_emit(c, OP_V3_MOVE);
            chunk_emit(c, reg);
            chunk_emit(c, rk_val);
            /* Checked AFTER v3_var_slot (which may have just reserved `reg`, raising the floor) so
               this correctly recognizes rk_val as no-longer-a-temp in the (common) case where a
               brand-new variable's assigned register happens to already be the exact temp its RHS
               computed into. */
            if (v3_is_temp(rk_val)) v3_reg_free(1);
        }
        return;
    }

    for (int i = 0; i < V3_COMPOUND_ASSIGN_OP_COUNT; i++) {
        if (!consume(v3_compound_assign_ops[i].tok)) continue;

        /* Non-creating lookup — compound assignment to a name with no prior value has no
           sensible register to read from, so it's a compile-time error here (unlike the stack
           VM, where an undefined global only fails at runtime). */
        int reg;
        if (!v3_var_lookup(name_idx, &reg)) {
            error_at("Compound assignment target must already have a value (v3 prototype: no assigning to an undefined name this way)");
            return;
        }

        int rk_rhs = v3_parse_binary(c, 0);
        if (parse_had_error) return;
        chunk_emit(c, OP_V3_BINARY);
        chunk_emit(c, reg);
        chunk_emit(c, reg);
        chunk_emit(c, (int)v3_compound_assign_ops[i].op);
        chunk_emit(c, rk_rhs);
        if (v3_is_temp(rk_rhs)) v3_reg_free(1);
        return;
    }

    error_at("v3 prototype only supports plain 'name = expr' or compound assignment (no field access)");
}

/* M5 slice 4 — `name[index] = expr`, single-level only (no `name[i][j] = v` or `name[i].field = v`
   — those need the GET-until-the-last-step-then-SET shape parser.c's parse_assignment has,
   parser.c:352-398, out of scope here). `name` is already consumed and confirmed to be a known
   variable's register (the array/dict it holds is resolved dynamically at runtime, same as the
   real language — v3 does no compile-time type checking). */
static void v3_parse_index_assignment(Chunk* c, unsigned int name_idx) {
    int arr_reg = v3_var_slot(name_idx);
    if (arr_reg < 0) return;   /* error_at already called */

    int rk_idx = v3_parse_binary(c, 0);
    require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
    if (parse_had_error) return;

    /* Feature-completeness follow-up — one level of chaining: `name[index].field = / OP= expr`
       (the shape nbody.aer's advance() uses: `bodies[i].x += dx * mi`). Materializes name[index]
       into a fresh temp register — a struct is a heap reference, so writing through this temp's
       own copy of that reference still mutates the SAME underlying object name[index] points at;
       no write-back into the array itself is needed. Still only ONE level past the index; a
       further chain (name[index].field[j], etc.) remains out of scope, same as every other v3
       chained-write limitation. */
    if (consume(TOKEN_DOT)) {
        if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return; }
        unsigned int field_idx = chunk_add_pool(c, token.value);
        lex();

        int struct_reg = v3_reg_alloc();
        v3_emit_index_get(c, struct_reg, arr_reg, rk_idx);

        if (consume(TOKEN_ASSIGN)) {
            int rk_val = v3_parse_binary(c, 0);
            if (parse_had_error) return;
            v3_emit_field_set(c, struct_reg, field_idx, rk_val);
            if (v3_is_temp(rk_val)) v3_reg_free(1);
            v3_reg_free(1);   /* struct_reg */
            if (v3_is_temp(rk_idx)) v3_reg_free(1);
            return;
        }

        for (int i = 0; i < V3_COMPOUND_ASSIGN_OP_COUNT; i++) {
            if (!consume(v3_compound_assign_ops[i].tok)) continue;

            int field_reg = v3_reg_alloc();
            v3_emit_field_get(c, field_reg, struct_reg, field_idx);

            int rk_rhs = v3_parse_binary(c, 0);
            if (parse_had_error) return;

            chunk_emit(c, OP_V3_BINARY);
            chunk_emit(c, field_reg);
            chunk_emit(c, field_reg);
            chunk_emit(c, (int)v3_compound_assign_ops[i].op);
            chunk_emit(c, rk_rhs);
            if (v3_is_temp(rk_rhs)) v3_reg_free(1);

            v3_emit_field_set(c, struct_reg, field_idx, field_reg);
            v3_reg_free(1);   /* field_reg */
            v3_reg_free(1);   /* struct_reg */
            if (v3_is_temp(rk_idx)) v3_reg_free(1);
            return;
        }

        error_at("v3 prototype only supports plain 'name[index].field = expr' or compound assignment here (no deeper chaining)");
        return;
    }

    if (consume(TOKEN_ASSIGN)) {
        int rk_val = v3_parse_binary(c, 0);
        if (parse_had_error) return;

        v3_emit_index_set(c, arr_reg, rk_idx, rk_val);

        /* arr_reg is a permanent variable register — never freed. rk_idx/rk_val may be temps, now
           consumed by the SET; free the later-allocated one first, matching the free-then-allocate
           ordering used elsewhere (doesn't affect correctness, just keeps register use compact). */
        if (v3_is_temp(rk_val)) v3_reg_free(1);
        if (v3_is_temp(rk_idx)) v3_reg_free(1);
        return;
    }

    /* Feature-completeness follow-up — `name[index] OP= expr`, same read-modify-write shape as
       the field version just above. rk_idx is RK-encoded and read-only, so it's safely reused for
       both the GET and the SET below without re-parsing or re-evaluating the index expression —
       no materialization needed (unlike v3_parse_for_in's cur_reg, nothing here mutates rk_idx). */
    for (int i = 0; i < V3_COMPOUND_ASSIGN_OP_COUNT; i++) {
        if (!consume(v3_compound_assign_ops[i].tok)) continue;

        int item_reg = v3_reg_alloc();
        v3_emit_index_get(c, item_reg, arr_reg, rk_idx);

        int rk_rhs = v3_parse_binary(c, 0);
        if (parse_had_error) return;

        chunk_emit(c, OP_V3_BINARY);
        chunk_emit(c, item_reg);
        chunk_emit(c, item_reg);
        chunk_emit(c, (int)v3_compound_assign_ops[i].op);
        chunk_emit(c, rk_rhs);
        if (v3_is_temp(rk_rhs)) v3_reg_free(1);

        v3_emit_index_set(c, arr_reg, rk_idx, item_reg);
        v3_reg_free(1);   /* item_reg */
        if (v3_is_temp(rk_idx)) v3_reg_free(1);
        return;
    }

    error_at("v3 prototype only supports plain 'name[index] = expr' or compound assignment (no chained/nested index or field writes)");
}

/* NEWLINE+INDENT, statements until DEDENT/EOF — mirrors parse_compound's shape (parser.c:1081-
   1110) minus its rollback/skip-to-boundary error recovery: a malformed script in this slice may
   abort ungracefully rather than recover per-statement, a stated simplification for a hand-fed
   test-string prototype. */
static void v3_parse_block(Chunk* c) {
    require(TOKEN_NEW_LINE, "expected newline before indented block");
    require(TOKEN_INDENT,   "expected indented block");
    if (parse_had_error) return;
    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_ELSE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        v3_parse_statement(c);
        if (parse_had_error) return;
    }
    consume(TOKEN_DEDENT);
}

static void v3_parse_if(Chunk* c) {
    int rk_cond = v3_parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after if condition");
    /* require() can't abort us on failure (a known gap in parser.c's own parse_if too — see the
       "AER require() no-abort bug" project notes) — without this, a malformed condition still
       gets a real branch compiled in. */
    if (parse_had_error) return;

    int reg_cond = v3_materialize(c, rk_cond);
    unsigned int patch_jif = v3_emit_jump_if_false_reg(c, reg_cond);
    if (v3_is_temp(reg_cond)) v3_reg_free(1);

    v3_parse_block(c);
    if (parse_had_error) return;

    if (consume(TOKEN_ELSE)) {
        require(TOKEN_COLON, "expected ':' after else");
        if (parse_had_error) return;
        chunk_emit(c, OP_JUMP);
        unsigned int patch_jmp = c->count;
        chunk_emit(c, 0);
        v3_patch_jump(c, patch_jif, c->count);
        v3_parse_block(c);
        v3_patch_jump(c, patch_jmp, c->count);
    } else {
        v3_patch_jump(c, patch_jif, c->count);
    }
}

/* Shared while/for-while tail: require ':', branch-if-false, body, back-edge to loop_top. */
static void v3_parse_for_body(Chunk* c, unsigned int loop_top, int rk_cond) {
    require(TOKEN_COLON, "expected ':' after for/while condition");
    if (parse_had_error) return;

    int reg_cond = v3_materialize(c, rk_cond);
    unsigned int patch_exit = v3_emit_jump_if_false_reg(c, reg_cond);
    if (v3_is_temp(reg_cond)) v3_reg_free(1);

    if (!v3_loop_push(loop_top)) return;
    v3_parse_block(c);
    if (parse_had_error) { v3_loop_depth--; return; }

    chunk_emit(c, OP_JUMP);
    chunk_emit(c, (int)loop_top);
    v3_patch_jump(c, patch_exit, c->count);
    v3_loop_pop_and_patch(c, c->count);
}

/* M5 slice 5 — `for x in collection:`, arrays only (matching M4's OP_V3_ITER_NEXT_ARRAY's own
   scope — dict-key/pair and range iteration are still further out). No exit-time cleanup needed
   (col_reg/idx_reg are just registers, unlike the stack VM's popped-on-exit iterator slots — see
   OP_V3_ITER_NEXT_ARRAY's own comment in vm.h).
     Reserves the loop variable's permanent register BEFORE compiling the collection expression or
   allocating the index register — the reverse of what would look natural mirroring parser.c
   (which evaluates the collection first). This order is required here specifically because v3's
   temps and permanent variables share one register-numbering space (parser.c's stack slots and
   operand stack are separate address spaces, so it never has to think about this): reserving the
   loop variable first guarantees the collection/idx temps are allocated above it, so the temp
   allocator can never hand out the exact register the loop variable was just assigned. */
static void v3_parse_for_in(Chunk* c, unsigned int loop_var_name) {
    int item_reg = v3_var_slot(loop_var_name);
    if (item_reg < 0) return;

    int rk_start = v3_parse_binary(c, 0);   /* the range's start, or the whole collection if no '..' follows */

    /* Feature-completeness follow-up — `a..b` or `a..b..step`, mirroring parse_for's range branch
       (parser.c:1201+) exactly: step defaults to a constant 1 when omitted, direction is inferred
       at runtime from cur vs end (OP_V3_ITER_RANGE), not from step's sign. `..` is for-loop-specific
       syntax here, same as in the real grammar — not a general binary operator. */
    if (consume(TOKEN_DOT_DOT)) {
        int rk_end = v3_parse_binary(c, 0);
        int rk_step;
        if (consume(TOKEN_DOT_DOT)) rk_step = v3_parse_binary(c, 0);
        else                        rk_step = (int)chunk_add_pool(c, aer_int(1)) | V3_RK_CONST_FLAG;

        require(TOKEN_COLON, "expected ':' after for-in clause");
        if (parse_had_error) return;

        /* cur_reg MUST be a genuinely fresh register (v3_arg_materialize, not v3_materialize) since
           OP_V3_ITER_RANGE mutates it every iteration — aliasing an existing variable's register
           here would make the loop silently corrupt that variable. end_reg/step_reg are read-only,
           so v3_materialize's cheaper "reuse if already a plain register" is safe for them. */
        int cur_reg  = v3_arg_materialize(c, rk_start);
        int end_reg  = v3_materialize(c, rk_end);
        int step_reg = v3_materialize(c, rk_step);

        /* Feature-completeness fix — promote whatever of the above are genuine temps to
           protected/permanent status for exactly the loop's duration, by raising v3_reserved_floor
           to match the watermark right here. Without this, a brand-new variable declared inside
           the loop's OWN body (v3_var_slot, which assumes "register == v3_var_count" and has no
           way to see that cur_reg/step_reg must keep living past this one statement) could be
           handed cur_reg's or step_reg's own register — silent corruption of the loop's iteration
           state, found via nbody.aer's nested `for i in 0..n: for j in (i+1)..n:` (the inner
           loop's own `j` collided with the outer loop's cur_reg). Restored to the pre-loop
           watermark once the loop's bytecode (body + back-edge) is fully emitted — same end state
           the old per-register v3_is_temp+v3_reg_free frees produced, just correct even when the
           body declares new variables in between, at the cost of these registers never being
           reused by a LATER, sibling statement even when nothing inside the loop needed them
           promoted (the same accepted "known, honest inefficiency" v3_var_slot's own comment
           already documents, just guaranteed here instead of merely possible). */
        int saved_reserved_floor = v3_reserved_floor;
        v3_reserved_floor = v3_next_temp_register;

        unsigned int loop_top = c->count;
        unsigned int patch_exit = v3_emit_iter_range(c, cur_reg, end_reg, step_reg, item_reg);

        if (!v3_loop_push(loop_top)) return;
        v3_parse_block(c);
        if (parse_had_error) { v3_loop_depth--; return; }

        chunk_emit(c, OP_JUMP);
        chunk_emit(c, (int)loop_top);
        v3_patch_jump(c, patch_exit, c->count);
        v3_loop_pop_and_patch(c, c->count);

        v3_reserved_floor     = saved_reserved_floor;
        v3_next_temp_register = saved_reserved_floor;
        return;
    }

    require(TOKEN_COLON, "expected ':' after for-in clause");
    if (parse_had_error) return;

    int col_reg = v3_materialize(c, rk_start);

    int idx_reg = v3_reg_alloc();
    unsigned int pool_zero = chunk_add_pool(c, aer_int(0));
    chunk_emit(c, OP_V3_LOADK);
    chunk_emit(c, idx_reg);
    chunk_emit(c, (int)pool_zero);

    /* Feature-completeness fix — same reasoning as the range branch's own comment above: idx_reg
       (always a genuine temp) and col_reg (a temp unless it aliases an existing variable) must
       stay valid across the loop's entire body, not just this setup statement, so they need to be
       protected from a new variable declared inside the body before that body gets compiled. */
    int saved_reserved_floor = v3_reserved_floor;
    v3_reserved_floor = v3_next_temp_register;

    unsigned int loop_top = c->count;   /* the iterate opcode is its own back-edge target */
    unsigned int patch_exit = v3_emit_iter_next_array(c, col_reg, idx_reg, item_reg);

    if (!v3_loop_push(loop_top)) return;
    v3_parse_block(c);
    if (parse_had_error) { v3_loop_depth--; return; }

    chunk_emit(c, OP_JUMP);
    chunk_emit(c, (int)loop_top);
    v3_patch_jump(c, patch_exit, c->count);
    v3_loop_pop_and_patch(c, c->count);

    v3_reserved_floor     = saved_reserved_floor;
    v3_next_temp_register = saved_reserved_floor;
}

/* Real AER has no separate `while` keyword — `for <condition>:` (no `in`) IS the while form (see
   parse_for's fallback branch, parser.c:1275-1319). */
static void v3_parse_for_while(Chunk* c) {
    if (equal(TOKEN_IDENTIFIER)) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_IN)) {
            v3_parse_for_in(c, name_idx);
            return;
        }
        int reg = v3_var_slot(name_idx);
        if (reg < 0) return;
        unsigned int loop_top = c->count;
        int rk_cond = v3_parse_binary_ops(c, 0, reg);
        v3_parse_for_body(c, loop_top, rk_cond);
        return;
    }
    unsigned int loop_top = c->count;
    int rk_cond = v3_parse_binary(c, 0);
    v3_parse_for_body(c, loop_top, rk_cond);
}

/* M5 slice 11 — module/stdlib calls. True if the current (not yet consumed) token names a module
   imported earlier — mirrors parser.c's own at_module_name (parser.c:1489-1494) exactly, reusing
   the same chunk_is_imported/chunk_add_import registry (Chunk-level state, not parser-specific),
   so v3's `import` and parser.c's are fully interchangeable within one chunk. Checked before the
   normal identifier/call/assignment path in both v3_parse_primary_inner and v3_parse_statement,
   same ordering parser.c uses for the same reason: a module name isn't a variable, so
   v3_var_slot/v3_func_lookup must never see it. */
static bool v3_at_module_name(Chunk* c) {
    return token.type == TOKEN_IDENTIFIER &&
           chunk_is_imported(c, aer_as_string(token.value)->data, aer_as_string(token.value)->length);
}

/* `module.function(args)` — mirrors parse_module_call (parser.c:1497-1514) in shape. Only HOW the
   arguments reach the call differs (contiguous registers via v3_parse_contiguous_exprs, like every
   other v3 call site) — WHAT gets called (aer_math_call/aer_string_call/etc.) is the exact same
   shared stack-based stdlib dispatch parser.c's OP_CALL_MODULE uses; OP_V3_CALL_MODULE's handler
   (vm.c) bridges the two calling conventions directly rather than reimplementing every stdlib
   function for registers — see its comment in vm.h. */
static int v3_parse_module_call(Chunk* c) {
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
    int arg_count = v3_parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after module call arguments");
    if (parse_had_error) return 0;

    int dest = (arg_count > 0) ? arg_reg_base : v3_reg_alloc();
    if (arg_count > 1) v3_reg_free(arg_count - 1);
    int base = arg_reg_base < 0 ? dest : arg_reg_base;
    chunk_emit(c, OP_V3_CALL_MODULE);
    chunk_emit(c, dest);
    chunk_emit(c, (int)module_idx);
    chunk_emit(c, (int)fn_idx);
    chunk_emit(c, base);
    chunk_emit(c, arg_count);
    return dest;
}

/* `import module[.sub]*` — top level only. Mirrors parse_import (parser.c:1461-1487) exactly for
   path-building and chunk_add_import registration. v3 has no block_depth counter (no nested-if/for
   restriction to mirror beyond function nesting), so only v3_function_depth is checked — an import
   inside an if/for block is accepted here where parser.c would reject it, a narrow, acceptable gap
   for this prototype. Doesn't check chunk_add_import's return value, matching parse_import's own
   behavior exactly (a failed import is silently not registered; a later module.function() call
   against it then just falls through to "unknown function", same failure mode parser.c has). */
static void v3_parse_import(Chunk* c) {
    if (v3_function_depth != 0) {
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

/* Feature-completeness follow-up — global builtins (length/delete/append/print/type/assert/
   panic), checked by name against a fixed list rather than any runtime fallback chain, consistent
   with v3 already resolving every other call target at compile time. Struct construction
   (vm_call_builtin's OTHER fallback branch, via chunk_find_shape) is deliberately excluded — v3
   already resolves that at compile time via v3_is_struct_name, checked before this ever runs. */
static bool v3_is_builtin_name(Chunk* c, unsigned int name_idx) {
    AerString* s = aer_as_string(c->pool[name_idx]);
    static const char* const names[] = { "length", "delete", "append", "print", "type", "assert", "panic" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t len = strlen(names[i]);
        if (s->length == len && strncmp(s->data, names[i], len) == 0) return true;
    }
    return false;
}

/* `length(args)` etc. — same contiguous-register argument materialization and result-register
   reuse as v3_parse_call, just emitting OP_V3_CALL_BUILTIN instead. `name_idx` is already
   consumed and confirmed to be a builtin name by the caller. */
static int v3_parse_builtin_call(Chunk* c, unsigned int name_idx) {
    int arg_reg_base;
    int arg_count = v3_parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error) return 0;

    int dest = (arg_count > 0) ? arg_reg_base : v3_reg_alloc();
    if (arg_count > 1) v3_reg_free(arg_count - 1);
    int base = arg_reg_base < 0 ? dest : arg_reg_base;
    chunk_emit(c, OP_V3_CALL_BUILTIN);
    chunk_emit(c, dest);
    chunk_emit(c, (int)name_idx);
    chunk_emit(c, base);
    chunk_emit(c, arg_count);
    return dest;
}

/* M5 slice 3 — call sites. Reached both as an expression (v3_parse_primary, when an identifier is
   immediately followed by '(') and as a bare statement (v3_parse_statement, result discarded).
   `name_idx` is already consumed by the caller. */
/* Reached both as an expression (v3_parse_primary_inner, identifier immediately followed by '(')
   and as a bare statement (v3_parse_statement, result discarded). `name_idx` is already consumed
   by the caller. Handles BOTH function calls and struct instantiation — mirrors the stack VM's own
   OP_CALL-does-double-duty precedent (lbl_call falls back to chunk_find_shape() when a name isn't
   a variable/function), just resolved one level up at compile time via v3_is_struct_name instead
   of a runtime fallback chain, consistent with v3 already resolving functions at compile time too.
   A third fallback, global builtins, is checked last (v3_is_builtin_name) — matching
   vm_call_builtin's own precedence on the stack VM side (struct shapes are checked there too, but
   never reached here since v3_is_struct_name already claimed that case above). */
static int v3_parse_call(Chunk* c, unsigned int name_idx) {
    bool is_struct = v3_is_struct_name(name_idx);
    unsigned int func_offset = 0;
    bool is_func = !is_struct && v3_func_lookup(name_idx, &func_offset);
    if (!is_struct && !is_func) {
        if (v3_is_builtin_name(c, name_idx)) return v3_parse_builtin_call(c, name_idx);
        error_at("Unknown function or struct type (v3 prototype requires it to be defined before any use — no forward references or mutual recursion)");
        return 0;
    }

    /* Arguments must land in contiguous registers for OP_V3_CALL/OP_V3_STRUCT_NEW's bulk-copy —
       see v3_arg_materialize's own comment for why this is usually a no-op check, not a copy. */
    int arg_reg_base;
    int arg_count = v3_parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error) return 0;

    /* The result reuses the first argument's register (freeing the rest) — matches Lua's own
       convention of reusing the base register for a call's result, same compact-register-use
       philosophy as v3_compile_node's free-then-allocate discipline elsewhere. */
    int dest = (arg_count > 0) ? arg_reg_base : v3_reg_alloc();
    if (arg_count > 1) v3_reg_free(arg_count - 1);
    int base = arg_reg_base < 0 ? dest : arg_reg_base;
    if (is_struct) v3_emit_struct_new(c, dest, name_idx, base, arg_count);
    else           v3_emit_call(c, dest, func_offset, base, arg_count);
    return dest;
}

/* M5 slice 12 — `defer name(args)`. Mirrors parse_defer (parser.c:1710-1724) in shape, but resolves
   the target at COMPILE time via v3_func_lookup, consistent with v3 already resolving every other
   call target at compile time (functions aren't first-class values in this prototype — see
   v3_parse_call's own comment) — unlike parser.c's version, whose name_idx re-resolves via scope
   lookup at replay time. Only a plain, already-defined v3 function is a valid target: no module
   calls (parser.c's own restriction too) and no struct construction (v3_is_struct_name is
   deliberately not checked here — "deferred construction" has no clear meaning and parser.c's
   defer doesn't support it either). Arguments are evaluated now, via the same contiguous-register
   materialization every other v3 call site uses, then copied out immediately by OP_V3_DEFER_PUSH —
   snapshotted at the defer statement, not re-evaluated at replay time, matching parser.c exactly. */
static void v3_parse_defer(Chunk* c) {
    if (v3_function_depth == 0) { error_at("'defer' outside function"); return; }
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after 'defer'"); return; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();

    unsigned int func_offset = 0;
    if (!v3_func_lookup(name_idx, &func_offset)) {
        error_at("Unknown function (v3 prototype requires a deferred call's target to already be a defined plain function — no module calls or struct construction)");
        return;
    }

    require(TOKEN_OPEN_PARENTHESE, "expected '(' after deferred function name");
    if (parse_had_error) return;

    int arg_reg_base;
    int arg_count = v3_parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after deferred call arguments");
    if (parse_had_error) return;

    if (arg_count > MAX_DEFER_ARGS) {
        error_at("Too many arguments to a deferred call (max %d)", MAX_DEFER_ARGS);
        return;
    }

    chunk_emit(c, OP_V3_DEFER_PUSH);
    chunk_emit(c, (int)func_offset);
    chunk_emit(c, arg_count > 0 ? arg_reg_base : 0);
    chunk_emit(c, arg_count);

    /* Args are copied out of these registers immediately by the opcode above — unlike a plain
       call, there's no result register to reuse one of them for, so all of them free. */
    if (arg_count > 0) v3_reg_free(arg_count);
}

/* `return expr` or bare `return` (implicit null) — no `return a, b` multi-value, no tail-call
   rewriting (both real parser.c features, out of scope here). */
static void v3_parse_return(Chunk* c) {
    if (v3_function_depth == 0) { error_at("'return' outside function"); return; }

    int rk;
    if (!equal(TOKEN_NEW_LINE) && !equal(TOKEN_END_OF_FILE) && !equal(TOKEN_DEDENT)) {
        rk = v3_parse_binary(c, 0);
        if (parse_had_error) return;
    } else {
        rk = (int)chunk_add_pool(c, aer_null()) | V3_RK_CONST_FLAG;
    }
    int reg = v3_materialize(c, rk);
    v3_emit_return(c, reg);
}

/* `function name(params):` — see this milestone's plan for the stated simplifications (no
   forward references/mutual recursion, no defaults, functions aren't first-class values, no
   nesting — the last one matches the real language's own restriction already). */
static void v3_parse_function(Chunk* c) {
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected function name"); return; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();

    require(TOKEN_OPEN_PARENTHESE, "expected '(' after function name");
    if (parse_had_error) return;

    unsigned int param_names[V3_FRAME_REGISTERS];
    int param_count = 0;
    if (!equal(TOKEN_CLOSE_PARENTHESE)) {
        do {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected parameter name"); return; }
            if (param_count >= V3_FRAME_REGISTERS) {
                error_at("Too many parameters for the v3 prototype (max %d)", V3_FRAME_REGISTERS);
                return;
            }
            param_names[param_count++] = chunk_add_pool(c, token.value);
            lex();
        } while (consume(TOKEN_COMMA));
    }
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after parameters");
    require(TOKEN_COLON,            "expected ':' after function signature");
    if (parse_had_error) return;

    if (v3_func_count >= V3_FUNC_MAX) {
        error_at("Too many functions for the v3 prototype (max %d)", V3_FUNC_MAX);
        return;
    }

    chunk_emit(c, OP_JUMP);
    unsigned int patch = c->count;
    chunk_emit(c, 0);
    unsigned int func_start = c->count;

    /* Isolate the body's register space from the caller/top-level's — save the allocator's raw
       state and the variable table, reset both to fresh, restore after. Mirrors parser.c's
       current_locals save/restore around a function body (parser.c:1612-1614, 1630-1631); no
       stack of saved tables needed since named functions can't nest. */
    unsigned int saved_var_names[V3_FRAME_REGISTERS];
    int          saved_var_regs[V3_FRAME_REGISTERS];
    int saved_var_count      = v3_var_count;
    int saved_next_temp      = v3_next_temp_register;
    int saved_reserved_floor = v3_reserved_floor;
    memcpy(saved_var_names, v3_var_names, sizeof(unsigned int) * (size_t)v3_var_count);
    memcpy(saved_var_regs,  v3_var_regs,  sizeof(int) * (size_t)v3_var_count);
    v3_var_count          = 0;
    v3_next_temp_register = 0;
    v3_reserved_floor     = 0;

    /* Register the function BEFORE compiling its body — func_start is already known, so a
       self-recursive call inside the body resolves correctly (mirrors Test 9's hand-driven
       factorial precedent); a call to any other not-yet-defined function still correctly fails
       (the stated "no forward references/mutual recursion" limitation). */
    v3_func_names[v3_func_count]   = name_idx;
    v3_func_offsets[v3_func_count] = func_start;
    v3_func_count++;

    /* Incremented BEFORE registering parameters (not after, as a first draft of the global-read
       feature had it) — v3_var_slot only skips recording a top-level global when
       v3_function_depth > 0, and a parameter is never a global no matter how early in this
       function's compilation it's registered. */
    v3_function_depth++;
    for (int i = 0; i < param_count; i++) v3_var_slot(param_names[i]);

    v3_parse_block(c);
    v3_function_depth--;

    if (!parse_had_error) {
        /* Implicit 'return null' if control falls off the end (mirrors parser.c:1624-1628). */
        int rk_null  = (int)chunk_add_pool(c, aer_null()) | V3_RK_CONST_FLAG;
        int reg_null = v3_materialize(c, rk_null);
        v3_emit_return(c, reg_null);
    }

    v3_var_count = saved_var_count;
    memcpy(v3_var_names, saved_var_names, sizeof(unsigned int) * (size_t)saved_var_count);
    memcpy(v3_var_regs,  saved_var_regs,  sizeof(int) * (size_t)saved_var_count);
    v3_next_temp_register = saved_next_temp;
    v3_reserved_floor     = saved_reserved_floor;

    v3_patch_jump(c, patch, c->count);
}

/* `struct Name:\n    field [= literal]\n ...` — mirrors parse_struct (parser.c:1414-1455)
   field-by-field, reusing the already-global MAX_STRUCT_FIELDS (vm.h) exactly as parser.c does.
   Emits the UNMODIFIED OP_DEFINE_STRUCT bytecode directly (see this opcode's own comment in vm.h
   for why it's genuinely stack-neutral and needs no v3-specific wrapper), then registers the type
   name so a later `Name(args)` call site resolves to construction (v3_parse_call). Field defaults
   are limited to the same literal set v3 already parses elsewhere (int/real/bool/null/string) — no
   bare `[]`/`{}` empty-container defaults, unlike parser.c's parse_literal_default. */
static void v3_parse_struct(Chunk* c) {
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
            if      (token.type == TOKEN_INTEGER) dflt = aer_int(aer_as_int(token.value));
            else if (token.type == TOKEN_REAL)    dflt = aer_real(aer_as_real(token.value));
            else if (token.type == TOKEN_TRUE || token.type == TOKEN_FALSE) dflt = aer_bool(aer_as_bool(token.value));
            else if (token.type == TOKEN_NULL)    dflt = aer_null();
            else if (token.type == TOKEN_STRING) {
                AerString* ts = aer_as_string(token.value);
                dflt = c->pool[v3_pool_escaped_string(c, ts->data, ts->length)];
            } else {
                error_at("Struct field defaults must be a literal value (v3 prototype: no array/dict defaults)");
                return;
            }
            lex();
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
    if (v3_struct_count >= V3_STRUCT_MAX) {
        error_at("Too many struct types for the v3 prototype (max %d)", V3_STRUCT_MAX);
        return;
    }

    chunk_emit(c, OP_DEFINE_STRUCT);
    chunk_emit(c, (int)name_idx);
    chunk_emit(c, (int)field_count);
    for (unsigned int i = 0; i < field_count; i++) {
        chunk_emit(c, (int)field_names[i]);
        chunk_emit(c, (int)chunk_add_pool(c, field_defaults[i]));
    }

    v3_struct_names[v3_struct_count++] = name_idx;
}

/* M5 slice 6 — `name.field = expr`, single-level only (no `name.a.b = v` or `name.a[0] = v` —
   those need the GET-until-the-last-step-then-SET shape parser.c's parse_assignment has, out of
   scope here, same limitation slice 4 already established for indexed writes). `name` is already
   consumed by the caller. */
static void v3_parse_field_assignment(Chunk* c, unsigned int name_idx) {
    int struct_reg = v3_var_slot(name_idx);
    if (struct_reg < 0) return;   /* error_at already called */

    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return; }
    unsigned int field_idx = chunk_add_pool(c, token.value);
    lex();

    if (consume(TOKEN_ASSIGN)) {
        int rk_val = v3_parse_binary(c, 0);
        if (parse_had_error) return;

        v3_emit_field_set(c, struct_reg, field_idx, rk_val);
        if (v3_is_temp(rk_val)) v3_reg_free(1);
        return;
    }

    /* Feature-completeness follow-up — `x.field OP= expr`: read-modify-write, same shape as
       v3_parse_assignment's plain-variable compound assignment, just via field get/set instead of
       reading/writing a permanent register directly. Order matches parser.c's OP_DUP_N-based
       version exactly (parser.c:462-501): the field's CURRENT value is read before the RHS is
       parsed, not after — matters if the RHS itself has a side effect on this same field. No
       fusion mechanism needed (unlike the stack VM's OP_COMPOUND_INDEXED_FIELD_* family, which
       exists purely to save a stack DUP — registers have nothing to duplicate). Still single-level
       only, same limitation the plain '=' case above already has. */
    for (int i = 0; i < V3_COMPOUND_ASSIGN_OP_COUNT; i++) {
        if (!consume(v3_compound_assign_ops[i].tok)) continue;

        int field_reg = v3_reg_alloc();
        v3_emit_field_get(c, field_reg, struct_reg, field_idx);

        int rk_rhs = v3_parse_binary(c, 0);
        if (parse_had_error) return;

        chunk_emit(c, OP_V3_BINARY);
        chunk_emit(c, field_reg);
        chunk_emit(c, field_reg);
        chunk_emit(c, (int)v3_compound_assign_ops[i].op);
        chunk_emit(c, rk_rhs);
        if (v3_is_temp(rk_rhs)) v3_reg_free(1);

        v3_emit_field_set(c, struct_reg, field_idx, field_reg);
        v3_reg_free(1);   /* field_reg itself */
        return;
    }

    error_at("v3 prototype only supports plain 'name.field = expr' or compound assignment (no chained field/index writes)");
}

/* M5 slice 8 — mirrors parse_break/parse_continue (parser.c:1326-1342) exactly, minus the
   iter_slots POPs (see V3LoopContext's own comment on why v3 needs none). */
static void v3_parse_break(Chunk* c) {
    if (v3_loop_depth == 0) { error_at("'break' outside loop"); return; }
    V3LoopContext* ctx = &v3_loop_stack[v3_loop_depth - 1];
    if (ctx->patch_count >= V3_BREAK_MAX) { error_at("Too many breaks in one loop (max %d)", V3_BREAK_MAX); return; }
    chunk_emit(c, OP_JUMP);
    ctx->patches[ctx->patch_count++] = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by v3_loop_pop_and_patch once the loop ends */
}

static void v3_parse_continue(Chunk* c) {
    if (v3_loop_depth == 0) { error_at("'continue' outside loop"); return; }
    chunk_emit(c, OP_JUMP);
    chunk_emit(c, (int)v3_loop_stack[v3_loop_depth - 1].top);   /* known at compile time — no patch needed */
}

static void v3_parse_statement(Chunk* c) {
    if (consume(TOKEN_IF))       { v3_parse_if(c);       return; }
    if (consume(TOKEN_FOR))      { v3_parse_for_while(c); return; }
    if (consume(TOKEN_FUNCTION)) { v3_parse_function(c);  return; }
    if (consume(TOKEN_RETURN))   { v3_parse_return(c);    return; }
    if (consume(TOKEN_STRUCT))   { v3_parse_struct(c);    return; }
    if (consume(TOKEN_BREAK))    { v3_parse_break(c);     return; }
    if (consume(TOKEN_CONTINUE)) { v3_parse_continue(c);  return; }
    if (consume(TOKEN_IMPORT))   { v3_parse_import(c);    return; }
    if (consume(TOKEN_DEFER))    { v3_parse_defer(c);     return; }
    if (v3_at_module_name(c))    { v3_parse_module_call(c); return; }
    if (equal(TOKEN_IDENTIFIER)) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_OPEN_PARENTHESE)) { v3_parse_call(c, name_idx); return; }
        if (consume(TOKEN_OPEN_BRACKET))    { v3_parse_index_assignment(c, name_idx); return; }
        if (consume(TOKEN_DOT))             { v3_parse_field_assignment(c, name_idx); return; }
        v3_parse_assignment(c, name_idx);
        return;
    }
    error_at("Expected a statement (v3 prototype: only assignment, indexed/field writes, if/else, for-while, break/continue, function/struct defs, return, imports, and calls are supported)");
}

/* Entry point — mirrors parse()'s own loop (parser.c:267-299) minus its rollback/skip-to-boundary
   recovery. Resets the register allocator, the variable table, the function table, and
   parse_had_error so this run is isolated from any earlier chunk compiled in the same process
   (same reasoning as tests/v3_smoke_test.c's run_chunk resetting runtime_had_error per test). */
void v3_parse(Chunk* c) {
    v3_reg_reset();
    v3_var_count      = 0;
    v3_global_count   = 0;
    v3_func_count     = 0;
    v3_struct_count   = 0;
    v3_function_depth = 0;
    v3_loop_depth     = 0;
    parse_had_error   = false;
    while (!equal(TOKEN_END_OF_FILE)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        if (consume(TOKEN_DEDENT))   continue;
        v3_parse_statement(c);
        if (parse_had_error) return;
    }
}

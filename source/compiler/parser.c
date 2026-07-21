#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include "error.h"
#include "lexer.h"

/* The register allocator every compile function shares. next_temp_register tracks the next
   free temp; registers below reserved_floor are permanent and must never be handed out here. */
static int next_temp_register = 0;
static int reserved_floor     = 0;

/* Highest watermark reached since the last reset -- the real peak register need, since both
   counters shrink back down as temps free. Read back after a function body finishes compiling
   but before its own restore runs (parse_function/parse_function_expr). */
static int max_register_used = 0;
static void track_peak(int v) { if (v > max_register_used) max_register_used = v; }

/* Raw-slot allocators, mirroring reg_alloc/reg_free/reg_reserve's shape -- except overflow:
   raw_int_alloc/raw_real_alloc return -1 instead of erroring, and every caller falls back to
   ordinary boxed storage for that one value. */
static int raw_int_next_temp = 0, raw_int_reserved_floor = 0;
static int raw_real_next_temp = 0, raw_real_reserved_floor = 0;

static int raw_int_alloc(void) {
    if (raw_int_next_temp >= RAW_REGISTERS_INT) return -1;
    return raw_int_next_temp++;
}
static void raw_int_free(int count) {
    raw_int_next_temp -= count;
    if (raw_int_next_temp < raw_int_reserved_floor) raw_int_next_temp = raw_int_reserved_floor;
}
static int raw_real_alloc(void) {
    if (raw_real_next_temp >= RAW_REGISTERS_REAL) return -1;
    return raw_real_next_temp++;
}
static void raw_real_free(int count) {
    raw_real_next_temp -= count;
    if (raw_real_next_temp < raw_real_reserved_floor) raw_real_next_temp = raw_real_reserved_floor;
}

/* Reserves a new permanent raw slot for a variable's first assignment -- the raw analog of
   var_slot claiming reserved_floor. Returns -1 on overflow (caller falls back to boxed). */
static int raw_int_reserve_one(void) {
    if (raw_int_reserved_floor >= RAW_REGISTERS_INT) return -1;
    int slot = raw_int_reserved_floor;
    raw_int_reserved_floor++;
    raw_int_next_temp = raw_int_reserved_floor;
    return slot;
}
static int raw_real_reserve_one(void) {
    if (raw_real_reserved_floor >= RAW_REGISTERS_REAL) return -1;
    int slot = raw_real_reserved_floor;
    raw_real_reserved_floor++;
    raw_real_next_temp = raw_real_reserved_floor;
    return slot;
}

void reg_reset(void) {
    next_temp_register = 0;
    reserved_floor     = 0;
    max_register_used  = 0;
    raw_int_next_temp = 0;  raw_int_reserved_floor = 0;
    raw_real_next_temp = 0; raw_real_reserved_floor = 0;
}

/* Must refuse a register >= FRAME_REGISTERS -- the register_stack bank is sized assuming no
   frame ever needs more. Returns an in-bounds sentinel after erroring. */
void reg_reserve(int count) {
    if (reserved_floor + count > FRAME_REGISTERS) {
        error_at("Too many live variables/temporaries (max %d registers per call)", FRAME_REGISTERS);
        return;
    }
    reserved_floor     += count;
    next_temp_register += count;
    track_peak(next_temp_register);
}

int reg_alloc(void) {
    if (next_temp_register >= FRAME_REGISTERS) {
        error_at("Too many live variables/temporaries (max %d registers per call)", FRAME_REGISTERS);
        return FRAME_REGISTERS - 1;
    }
    int reg = next_temp_register++;
    track_peak(next_temp_register);
    return reg;
}

void reg_free(int count) {
    next_temp_register -= count;
    /* Never free below the reserved floor -- a bug elsewhere shouldn't hand out a local's own
       register as a free temp. */
    if (next_temp_register < reserved_floor) next_temp_register = reserved_floor;
}

/* Guard for PACK_RK20 opcodes: 524288 registers-or-constants is far beyond any real program,
   but silent truncation would corrupt the instruction rather than refuse to compile. */
static bool rk20_fits(int rk) {
    return (rk & ~RK_CONST_FLAG) <= RK20_MAX_INDEX;
}

/* Same guard, narrower (256), for OP_BINARY's own family -- see PACK_BINARY's 9-bit RK field. */
static bool rk9_fits(int rk) {
    return (rk & ~RK_CONST_FLAG) <= RK9_MAX_INDEX;
}

/* Guard for OP_CALL_MODULE's module/fn indices (17 bits) and the field-fusion opcodes'
   field_idx (14 bits) -- tighter budgets since those words are already full. */
static bool pool_idx_fits(unsigned int idx, unsigned int max) {
    return idx <= max;
}

/* Forward-declared so emit_binary (which needs it) can come before it. */
static int box_if_raw(Chunk* c, int rk);

/* Every OP_BINARY emission funnels through here. Boxes any raw-flagged operand first
   (a no-op for a plain register or constant) -- only parse_binary_ops's own raw-composing path
   (try_emit_binary_raw) tries the native route before reaching here. */
static int materialize(Chunk* c, int rk);

static void emit_binary(Chunk* c, int dest, Opcode op, int rk_lhs, int rk_rhs) {
    rk_lhs = box_if_raw(c, rk_lhs);
    rk_rhs = box_if_raw(c, rk_rhs);
    /* A constant past RK9's budget is spilled into a scratch register instead of refusing to
       compile, freed immediately after the emit. */
    int spilled = 0;
    if (!rk9_fits(rk_lhs)) { rk_lhs = materialize(c, rk_lhs); spilled++; }
    if (!rk9_fits(rk_rhs)) { rk_rhs = materialize(c, rk_rhs); spilled++; }
    chunk_emit(c, PACK_BINARY(op, dest, rk_lhs, rk_rhs));
    if (spilled) reg_free(spilled);
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

/* Returns the callee_offset word's offset for a forward-referencing call to patch later
   (pending_call_add); an already-resolved call ignores the return value. */
unsigned int emit_call(Chunk* c, int dest_reg, unsigned int callee_offset, int arg_reg_base, int arg_count) {
    chunk_emit(c, PACK3(OP_CALL, dest_reg, arg_reg_base, arg_count));
    unsigned int patch_offset = c->count;
    chunk_emit(c, (int)callee_offset);
    return patch_offset;
}

void emit_return(Chunk* c, int src_reg) {
    chunk_emit(c, PACK1(OP_RETURN, src_reg));
}

/* Functions as values -- callee_reg is always a plain register (already materialized), so no
   patching is ever needed here. */
void emit_call_value(Chunk* c, int dest_reg, int arg_reg_base, int arg_count, int callee_reg) {
    chunk_emit(c, PACK_REG4(OP_CALL_VALUE, dest_reg, arg_reg_base, arg_count, callee_reg));
}

void emit_print_repl(Chunk* c, int src_reg) {
    chunk_emit(c, PACK1(OP_PRINT_REPL, src_reg));
}

void emit_array_new(Chunk* c, int dest_reg, int item_reg_base, int item_count) {
    chunk_emit(c, PACK3(OP_ARRAY_NEW, dest_reg, item_reg_base, item_count));
}

void emit_index_get(Chunk* c, int dest_reg, int arr_reg, int rk_idx) {
    rk_idx = box_if_raw(c, rk_idx);
    /* Same spill-to-register fallback as emit_binary. */
    if (!rk9_fits(rk_idx)) {
        rk_idx = materialize(c, rk_idx);
        chunk_emit(c, PACK_INDEX_GET(dest_reg, arr_reg, rk_idx));
        reg_free(1);
        return;
    }
    chunk_emit(c, PACK_INDEX_GET(dest_reg, arr_reg, rk_idx));
}

void emit_index_set(Chunk* c, int arr_reg, int rk_idx, int rk_val) {
    rk_idx = box_if_raw(c, rk_idx);
    rk_val = box_if_raw(c, rk_val);
    if (!rk20_fits(rk_idx) || !rk20_fits(rk_val)) {
        error_at("Expression too large to compile (register/constant index exceeds the index-set encoding's range)");
        return;
    }
    chunk_emit(c, PACK_INDEX_SET(arr_reg, rk_idx, rk_val));
}

/* rk_start/rk_end are RK-encoded like any value operand; a missing bound is an RK-encoded
   null constant built by the caller. */
void emit_slice_get(Chunk* c, int dest_reg, int arr_reg, int rk_start, int rk_end) {
    rk_start = box_if_raw(c, rk_start);
    rk_end   = box_if_raw(c, rk_end);
    if (!rk20_fits(rk_start) || !rk20_fits(rk_end)) {
        error_at("Expression too large to compile (register/constant index exceeds the slice-get encoding's range)");
        return;
    }
    chunk_emit(c, PACK_SLICE_GET(dest_reg, arr_reg, rk_start, rk_end));
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
    /* All 4 registers fit in one packed word (PACK_REG4); end_target stays its own word, since a
   patchable jump target is never packed alongside anything else. */
    chunk_emit(c, PACK_REG4(OP_ITER_NEXT_PAIR, col_reg, idx_reg, key_dest_reg, val_dest_reg));
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);   /* placeholder — patched by patch_jump once the loop-exit target is known */
    return patch_offset;
}

/* Loop-rotated range-for pair, used only by parse_for_in's `for i in a..b..step:` form. */
unsigned int emit_iter_range_prep(Chunk* c, int cur_reg, int end_reg, int step_reg, int item_dest_reg) {
    chunk_emit(c, PACK_REG4(OP_ITER_RANGE_PREP, cur_reg, end_reg, step_reg, item_dest_reg));
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);   /* placeholder — patched once the loop's overall exit address is known */
    return patch_offset;
}

/* body_target is always already resolved -- unlike every other loop jump, never a patch
   placeholder. */
void emit_iter_range_loop(Chunk* c, int cur_reg, int end_reg, int step_reg, int item_dest_reg, unsigned int body_target) {
    chunk_emit(c, PACK_REG4(OP_ITER_RANGE_LOOP, cur_reg, end_reg, step_reg, item_dest_reg));
    chunk_emit(c, (int)body_target);
}

void emit_struct_new(Chunk* c, int dest_reg, unsigned int type_name_pool_idx, int arg_reg_base, int arg_count) {
    chunk_emit(c, PACK_STRUCT_NEW(dest_reg, arg_reg_base, arg_count, type_name_pool_idx));
}

void emit_packed_array_new(Chunk* c, int dest_reg, unsigned int type_name_pool_idx, int rk_count) {
    chunk_emit(c, PACK_PACKED_ARRAY_NEW(dest_reg, type_name_pool_idx, rk_count));
}

void emit_field_get(Chunk* c, int dest_reg, int struct_reg, unsigned int field_name_pool_idx) {
    chunk_emit(c, PACK_FIELD_GET(dest_reg, struct_reg, field_name_pool_idx));
}

void emit_field_set(Chunk* c, int struct_reg, unsigned int field_name_pool_idx, int rk_val) {
    rk_val = box_if_raw(c, rk_val);
    if (!rk9_fits(rk_val) || field_name_pool_idx > RK9_MAX_INDEX) {
        error_at("Expression too large to compile (register/constant index exceeds the field-set encoding's range)");
        return;
    }
    chunk_emit(c, PACK_FIELD_SET(struct_reg, field_name_pool_idx, rk_val));
}

/* ------------------------------------------------------------------ */
/* Recursive-descent compiler: the full grammar, from the lexer's token stream straight to
   register bytecode. */
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
static void parse_for_while(Chunk* c);
static void parse_for_body(Chunk* c, unsigned int loop_top, int rk_cond);
static void parse_assignment(Chunk* c, unsigned int name_idx);
static void parse_chain_assignment(Chunk* c, unsigned int name_idx, bool first_is_index);
static int  parse_call(Chunk* c, unsigned int name_idx);
static int  parse_packed_array_new(Chunk* c, unsigned int name_idx);
static void parse_function(Chunk* c);
static bool parse_literal_default(Chunk* c, AerVal* out);
static void parse_return(Chunk* c);
static void parse_struct(Chunk* c);
static bool at_module_name(Chunk* c);
static int  module_call_id(AerString* name);
static int  module_fn_id(int module_id, AerString* name);
static int  parse_module_call(Chunk* c);
static void parse_import(Chunk* c);
static bool is_builtin_name(Chunk* c, unsigned int name_idx);
static int  parse_builtin_call(Chunk* c, unsigned int name_idx);
static int  parse_contiguous_exprs(Chunk* c, TokenType close_tok, int* out_base);

/* name_idx -> register. No global/local distinction (this slice has no function defs, so
   every register is effectively local). */
static unsigned int var_names[FRAME_REGISTERS];
static int          var_regs[FRAME_REGISTERS];
static int          var_count = 0;

/* Per-variable storage kind. VAR_BOXED is an ordinary registers[] index; VAR_RAW_INT/REAL is a
   raw_ints/raw_reals slot instead, earned only when a name's first assignment is provably
   int/real (rk_raw_kind), only inside a function body, and only outside any if/else branch
   (branch_depth == 0 -- a name assigned different types down mutually-exclusive branches can't
   be resolved without real dataflow analysis). Transitions are one-way: RAW_* can shadow to
   VAR_BOXED on a mismatch; VAR_BOXED never promotes to RAW_*. */
typedef enum { VAR_BOXED, VAR_RAW_INT, VAR_RAW_REAL } VarKind;
static VarKind var_kind[FRAME_REGISTERS];

/* Nonzero while compiling an if/else branch -- disqualifies raw storage (see var_kind). A
   real counter since if/else nests. */
static int branch_depth = 0;

/* Nonzero while compiling a function body -- lets parse_return reject a top-level return. No
   nesting (named functions can't nest), so 0/1 is enough. */
static int function_depth = 0;

/* Top-level variable names, kept solely to detect a function body referencing one -- entirely
   off-limits inside a function, enforced by var_slot's shadow-ban check. Updated in lockstep
   with var_names only while function_depth == 0, so it keeps accumulating across a function
   def and back out again. */
static unsigned int global_names[FRAME_REGISTERS];
static int          global_regs[FRAME_REGISTERS];
static int          global_count = 0;

/* True (after reporting the error) if name_idx is a top-level variable and the caller is
   inside a function body. */
static bool report_if_shadowed_global(Chunk* c, unsigned int name_idx) {
    if (function_depth == 0) return false;
    for (int i = 0; i < global_count; i++) {
        if (global_names[i] == name_idx) {
            error_at("'%s' is a top-level variable — not accessible inside a function; pass it as a parameter (or rename)",
                     aer_as_string(c->pool[name_idx])->data);
            return true;
        }
    }
    return false;
}

/* A new variable's register is reserved_floor, not var_count -- they diverge when a for-loop
   promotes reserved_floor for its own iteration registers without registering a name (a fresh
   variable declared inside that loop's body must land above them, or it silently aliases the
   loop's own state -- a real bug found this way with nested for-loops). var_regs[] is what makes
   lookup still resolve correctly once the two diverge. */
static int var_slot(Chunk* c, unsigned int name_idx) {
    for (int i = 0; i < var_count; i++)
        if (var_names[i] == name_idx) return var_regs[i];
    /* Shadowing a top-level name is a compile error, not a silent fresh local -- catches both a
       fresh local and a parameter with that name. function_depth == 0 (the definition itself)
       and an already-local name are exempt. */
    if (report_if_shadowed_global(c, name_idx)) return -1;
    /* Checks reserved_floor, not var_count -- var_count can lag behind reserved_floor once a
       for-loop promotes it (see this function's own comment above). */
    if (reserved_floor >= FRAME_REGISTERS) {
        error_at("Too many variables (max %d)", FRAME_REGISTERS);
        return -1;
    }
    int reg = reserved_floor;
    var_names[var_count] = name_idx;
    var_regs[var_count]  = reg;
    /* var_kind[] is a persistent static array shared across every function's compilation -- a
       PAST function's raw-tracked local can leave a stale kind at whatever index this new name
       lands on (save/restore only shrinks var_count, never zeroes higher indices). Real bug found
       this way: a fresh top-level var inherited VAR_RAW_INT from an earlier function's local at
       the same index. var_slot resets the kind explicitly here, the one place every name is created. */
    var_kind[var_count]  = VAR_BOXED;
    var_count++;
    reserved_floor++;                        /* permanently protects this register from the temp allocator */
    next_temp_register = reserved_floor;   /* resync — see this function's own comment for why that's always safe */
    track_peak(reserved_floor);
    if (function_depth == 0) {
        global_names[global_count] = name_idx;
        global_regs[global_count]  = reg;
        global_count++;
    }
    return reg;
}

/* Non-creating -- a match here is a top-level variable, grounds for the shadow-ban error, not
   a read/write. */
static bool global_lookup(unsigned int name_idx, int* out_reg) {
    for (int i = 0; i < global_count; i++)
        if (global_names[i] == name_idx) { *out_reg = global_regs[i]; return true; }
    return false;
}

/* Non-creating counterpart to var_slot -- `name += expr` needs an existing value, so it must
   not silently allocate a fresh register on a miss. */
static bool var_lookup(unsigned int name_idx, int* out_reg) {
    for (int i = 0; i < var_count; i++)
        if (var_names[i] == name_idx) { *out_reg = var_regs[i]; return true; }
    return false;
}

/* Works for any RK operand: a temp always lives at/above reserved_floor, a permanent variable
   below it. A raw-flagged rk must return false FIRST -- its numeric value could otherwise
   coincidentally compare as >= reserved_floor. */
static bool is_temp(int rk) {
    if (rk & (RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG)) return false;
    return !(rk & RK_CONST_FLAG) && rk >= reserved_floor;
}

/* The one bridge from raw storage to the rest of the compiler -- every function treating rk as
   register-or-constant must call this first. Frees the raw slot afterward if it was a temp. */
static int box_if_raw(Chunk* c, int rk) {
    if (rk & RK_RAW_INT_FLAG) {
        int slot = rk & RK_RAW_SLOT_MASK;
        int dest = reg_alloc();
        chunk_emit(c, PACK_BOX(OP_BOX_INT, dest, slot));
        if (slot >= raw_int_reserved_floor) raw_int_free(1);
        return dest;
    }
    if (rk & RK_RAW_REAL_FLAG) {
        int slot = rk & RK_RAW_SLOT_MASK;
        int dest = reg_alloc();
        chunk_emit(c, PACK_BOX(OP_BOX_REAL, dest, slot));
        if (slot >= raw_real_reserved_floor) raw_real_free(1);
        return dest;
    }
    return rk;
}

/* Shadows an existing raw-tracked name to boxed in place (no-op otherwise) -- needed before
   any var_slot call whose caller is about to write a non-raw value, since var_slot has no kind
   awareness (real bug found this way: reusing a raw-int name as a for-in loop variable). */
static void ensure_boxed(Chunk* c, unsigned int name_idx) {
    int existing_idx = -1;
    for (int i = 0; i < var_count; i++) if (var_names[i] == name_idx) { existing_idx = i; break; }
    if (existing_idx < 0 || var_kind[existing_idx] == VAR_BOXED) return;

    int old_slot = var_regs[existing_idx];
    Opcode box_op = (var_kind[existing_idx] == VAR_RAW_INT) ? OP_BOX_INT : OP_BOX_REAL;
    if (reserved_floor >= FRAME_REGISTERS) { error_at("Too many variables (max %d)", FRAME_REGISTERS); return; }
    int new_reg = reserved_floor;
    reserved_floor++;
    next_temp_register = reserved_floor;
    chunk_emit(c, PACK_BOX(box_op, new_reg, old_slot));
    var_regs[existing_idx] = new_reg;
    var_kind[existing_idx] = VAR_BOXED;
    /* No global_regs update needed -- this path only runs on a currently-raw name. */
}

/* Materializes a constant via OP_LOADK, or boxes a raw value -- OP_JUMP_IF_FALSE_REG needs an
   actual register, no RK/raw form. */
static int materialize(Chunk* c, int rk) {
    rk = box_if_raw(c, rk);
    if (!(rk & RK_CONST_FLAG)) return rk;
    int reg = reg_alloc();
    chunk_emit(c, PACK1(OP_LOADK, reg)); chunk_emit(c, rk & ~RK_CONST_FLAG);
    return reg;
}

/* Every reader of a variable's value must go through this, not raw var_regs[i], or a raw slot
   index gets misread as a plain register (real bug found in string interpolation). */
static bool var_lookup_rk(unsigned int name_idx, int* out_rk) {
    for (int i = 0; i < var_count; i++) {
        if (var_names[i] != name_idx) continue;
        switch (var_kind[i]) {
            case VAR_RAW_INT:  *out_rk = RK_RAW_INT_FLAG  | var_regs[i]; break;
            case VAR_RAW_REAL: *out_rk = RK_RAW_REAL_FLAG | var_regs[i]; break;
            default:           *out_rk = var_regs[i]; break;
        }
        return true;
    }
    return false;
}

typedef enum { RAWK_NONE, RAWK_INT, RAWK_REAL } RawKind;

/* The ONE gate deciding whether an expression can compose as raw: an already-raw operand, or a
   compile-time int/real literal. A call result, container read, string, or struct/array/dict is
   never raw-composable. */
static RawKind rk_raw_kind(Chunk* c, int rk) {
    if (rk & RK_RAW_INT_FLAG)  return RAWK_INT;
    if (rk & RK_RAW_REAL_FLAG) return RAWK_REAL;
    if (rk & RK_CONST_FLAG) {
        AerVal v = c->pool[rk & ~RK_CONST_FLAG];
        if (aer_type(v) == TYPE_INTEGER) return RAWK_INT;
        if (aer_type(v) == TYPE_REAL)    return RAWK_REAL;
    }
    return RAWK_NONE;
}

/* An already-raw operand's slot is reused directly; a literal loads into a fresh slot.
   Returns -1 on budget overflow (caller falls back to boxed). */
static int raw_materialize(Chunk* c, int rk, RawKind kind) {
    if (kind == RAWK_INT) {
        if (rk & RK_RAW_INT_FLAG) return rk & RK_RAW_SLOT_MASK;
        unsigned int pool_idx = rk & ~RK_CONST_FLAG;
        int64_t v = aer_as_int(c->pool[pool_idx]);
        int slot = raw_int_alloc();
        if (slot < 0) return -1;
        /* A literal outside the signed 20-bit range must go through the pool -- real bug found this
           way: `i < 20000000` silently truncated to 77056. */
        if (v >= -524288 && v <= 524287) {
            chunk_emit(c, PACK_RAW_LOAD_INT(slot, (int)v));
        } else {
            chunk_emit(c, PACK_RAW_LOAD_INT_POOL(slot, pool_idx));
        }
        return slot;
    } else {
        if (rk & RK_RAW_REAL_FLAG) return rk & RK_RAW_SLOT_MASK;
        unsigned int pool_idx = rk & ~RK_CONST_FLAG;   /* real literal already lives in the pool as a full double */
        int slot = raw_real_alloc();
        if (slot < 0) return -1;
        chunk_emit(c, PACK_RAW_LOAD_REAL(slot, pool_idx));
        return slot;
    }
}

/* Raw-vs-boxed ordering comparisons only (vm.h) -- a raw loop counter almost always compares
   against a non-raw bound (a parameter). Requires the raw side to ALREADY be a raw slot, not
   merely raw-composable: a bare literal would re-materialize a fresh OP_RAW_LOAD every
   iteration, paying more than the boxed RK path it replaces. */
static bool try_emit_cmp_raw_boxed(Chunk* c, Opcode op, int rk_lhs, RawKind kind_lhs,
                                       int rk_rhs, RawKind kind_rhs, int* out_rk) {
    bool lhs_raw   = (kind_lhs != RAWK_NONE);
    int  raw_rk    = lhs_raw ? rk_lhs : rk_rhs;
    RawKind kind   = lhs_raw ? kind_lhs : kind_rhs;
    int  boxed_rk  = lhs_raw ? rk_rhs : rk_lhs;

    if (!(raw_rk & (RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG))) return false;
    if (boxed_rk & (RK_CONST_FLAG | RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG)) return false;

    /* `boxed OP raw` is `raw (flip) OP boxed` — LT/GT and LTE/GTE swap. */
    Opcode effective_op = op;
    if (!lhs_raw) {
        switch (op) {
            case OP_LT:  effective_op = OP_GT;  break;
            case OP_GT:  effective_op = OP_LT;  break;
            case OP_LTE: effective_op = OP_GTE; break;
            case OP_GTE: effective_op = OP_LTE; break;
            default: return false;
        }
    }

    bool int_kind = (kind == RAWK_INT);
    Opcode raw_op;
    if (int_kind) {
        switch (effective_op) {
            case OP_LT:  raw_op = OP_RAW_LT_INT_BOXED;  break;
            case OP_GT:  raw_op = OP_RAW_GT_INT_BOXED;  break;
            case OP_LTE: raw_op = OP_RAW_LTE_INT_BOXED; break;
            case OP_GTE: raw_op = OP_RAW_GTE_INT_BOXED; break;
            default: return false;
        }
    } else {
        switch (effective_op) {
            case OP_LT:  raw_op = OP_RAW_LT_REAL_BOXED;  break;
            case OP_GT:  raw_op = OP_RAW_GT_REAL_BOXED;  break;
            case OP_LTE: raw_op = OP_RAW_LTE_REAL_BOXED; break;
            case OP_GTE: raw_op = OP_RAW_GTE_REAL_BOXED; break;
            default: return false;
        }
    }

    int slot = raw_materialize(c, raw_rk, kind);
    if (slot < 0) return false;   /* raw-slot budget exhausted: fall back to boxed */

    int floor_now = int_kind ? raw_int_reserved_floor : raw_real_reserved_floor;
    if (slot >= floor_now) { if (int_kind) raw_int_free(1); else raw_real_free(1); }
    if (is_temp(boxed_rk)) reg_free(1);

    int dest = reg_alloc();
    chunk_emit(c, PACK_RAW_CMP_BOXED(raw_op, dest, slot, boxed_rk));
    *out_rk = dest;
    return true;
}

/* Returns false if the operator has no raw-native form or the operand kinds mismatch, and the
   caller falls back to the boxed path. Only ADD/SUB/MUL/DIV/MOD/FLOOR_DIV and the 4 ordering
   comparisons are raw-native -- EQ/NEQ/bitwise/AND/OR/IN always stay boxed, deliberately. */
static bool try_emit_binary_raw(Chunk* c, Opcode op, int rk_lhs, int rk_rhs, int* out_rk) {
    RawKind kind_lhs = rk_raw_kind(c, rk_lhs);
    RawKind kind_rhs = rk_raw_kind(c, rk_rhs);
    if ((kind_lhs == RAWK_NONE) != (kind_rhs == RAWK_NONE))
        return try_emit_cmp_raw_boxed(c, op, rk_lhs, kind_lhs, rk_rhs, kind_rhs, out_rk);
    if (kind_lhs == RAWK_NONE || kind_rhs == RAWK_NONE || kind_lhs != kind_rhs) return false;
    bool int_kind = (kind_lhs == RAWK_INT);

    Opcode raw_op;
    bool is_cmp = false;
    bool div_int_promotes_to_real = false;   /* OP_DIV on two ints still yields a real, matching boxed semantics. */
    if (int_kind) {
        switch (op) {
            case OP_ADD: raw_op = OP_RAW_ADD_INT; break;
            case OP_SUB: raw_op = OP_RAW_SUB_INT; break;
            case OP_MUL: raw_op = OP_RAW_MUL_INT; break;
            case OP_DIV: raw_op = OP_RAW_DIV_INT; div_int_promotes_to_real = true; break;
            case OP_MOD: raw_op = OP_RAW_MOD_INT; break;
            case OP_FLOOR_DIV: raw_op = OP_RAW_FLOOR_DIV_INT; break;
            case OP_LT:  raw_op = OP_RAW_LT_INT;  is_cmp = true; break;
            case OP_GT:  raw_op = OP_RAW_GT_INT;  is_cmp = true; break;
            case OP_LTE: raw_op = OP_RAW_LTE_INT; is_cmp = true; break;
            case OP_GTE: raw_op = OP_RAW_GTE_INT; is_cmp = true; break;
            default: return false;
        }
    } else {
        switch (op) {
            case OP_ADD: raw_op = OP_RAW_ADD_REAL; break;
            case OP_SUB: raw_op = OP_RAW_SUB_REAL; break;
            case OP_MUL: raw_op = OP_RAW_MUL_REAL; break;
            case OP_DIV: raw_op = OP_RAW_DIV_REAL; break;
            case OP_LT:  raw_op = OP_RAW_LT_REAL;  is_cmp = true; break;
            case OP_GT:  raw_op = OP_RAW_GT_REAL;  is_cmp = true; break;
            case OP_LTE: raw_op = OP_RAW_LTE_REAL; is_cmp = true; break;
            case OP_GTE: raw_op = OP_RAW_GTE_REAL; is_cmp = true; break;
            default: return false;   /* no raw MOD/FLOOR_DIV for real */
        }
    }

    int slot_lhs = raw_materialize(c, rk_lhs, kind_lhs);
    int slot_rhs = raw_materialize(c, rk_rhs, kind_lhs);   /* same kind, confirmed above */
    if (slot_lhs < 0 || slot_rhs < 0) return false;   /* raw-slot budget exhausted: fall back to boxed */

    /* Free-then-allocate, RHS then LHS -- only frees a slot that was actually a temp. */
    int floor_now = int_kind ? raw_int_reserved_floor : raw_real_reserved_floor;
    if (slot_rhs >= floor_now) { if (int_kind) raw_int_free(1); else raw_real_free(1); }
    if (slot_lhs >= floor_now) { if (int_kind) raw_int_free(1); else raw_real_free(1); }

    if (is_cmp) {
        int dest = reg_alloc();
        chunk_emit(c, PACK_RAW_CMP(raw_op, dest, slot_lhs, slot_rhs));
        *out_rk = dest;
        return true;
    }
    if (div_int_promotes_to_real) {
        int dest = raw_real_alloc();
        if (dest < 0) return false;   /* extremely unlikely right after freeing 2 int slots, but stay safe */
        chunk_emit(c, PACK_RAW_ARITH_RR(raw_op, dest, slot_lhs, slot_rhs));
        *out_rk = RK_RAW_REAL_FLAG | dest;
        return true;
    }
    int dest = int_kind ? raw_int_alloc() : raw_real_alloc();
    if (dest < 0) return false;
    chunk_emit(c, PACK_RAW_ARITH_RR(raw_op, dest, slot_lhs, slot_rhs));
    *out_rk = (int_kind ? RK_RAW_INT_FLAG : RK_RAW_REAL_FLAG) | dest;
    return true;
}

/* Reads Chunk.functions directly -- the registry chunk_add_function already maintains is the
   single source of truth; the parser keeps no parallel copy. */
static bool func_lookup(Chunk* c, unsigned int name_idx, unsigned int* out_offset) {
    ChunkFunction* f = chunk_find_function_by_name_idx(c, name_idx);
    if (!f) return false;
    *out_offset = f->code_offset;
    return true;
}

/* A bare-name value reference or an omitted-defaults call needs the full signature to build a
   real AerFunction, not just the offset. */
static bool func_full_lookup(Chunk* c, unsigned int name_idx, unsigned int* out_offset, unsigned int* out_arity,
                                 unsigned int* out_min_arity, AerVal** out_defaults,
                                 unsigned int* out_max_registers) {
    ChunkFunction* f = chunk_find_function_by_name_idx(c, name_idx);
    if (!f) return false;
    *out_offset        = f->code_offset;
    *out_arity         = f->arity;
    *out_min_arity     = f->min_arity;
    *out_defaults      = f->defaults;
    *out_max_registers = f->max_registers;
    return true;
}

/* Builds a real runtime AerFunction, reusing the existing constructor. `defaults` is used
   as-is, not copied. */
static AerVal build_function_value(unsigned int func_offset, unsigned int arity, unsigned int min_arity,
                                       AerVal* defaults, unsigned int max_registers) {
    AerFunction* fn = vm_new_function();
    fn->code_offset   = func_offset;
    fn->arity         = (uint16_t)arity;
    fn->min_arity     = (uint16_t)min_arity;
    fn->defaults      = defaults;
    fn->max_registers = max_registers;
    return aer_function_val(fn);
}

/* A call to a not-yet-registered name is optimistically assumed to be defined later in this
   same parse() call -- recorded here with a placeholder already emitted; func_register patches
   every pending entry once that name registers. Still-pending entries at parse()'s end were
   never defined anywhere in this call. */
typedef struct {
    unsigned int name_idx;
    unsigned int patch_offset;
    const char*  call_site_cursor;
} PendingCall;
static PendingCall* pending_calls = NULL;
static int            pending_count = 0;
static int            pending_cap   = 0;

/* Passed in explicitly, not read internally -- by the time this is called the caller has
   already consumed the argument list, so "now" would point past the actual name. */
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

/* `defaults` is taken by ownership, never copied. */
static void func_register(Chunk* c, unsigned int name_idx, unsigned int offset, unsigned int arity,
                              unsigned int min_arity, AerVal* defaults) {
    chunk_add_function(c, name_idx, offset, arity, min_arity, defaults);

    /* Swap-remove each match (order doesn't matter), leaving only genuinely unresolved entries. */
    for (int i = 0; i < pending_count; ) {
        if (pending_calls[i].name_idx == name_idx) {
            patch_jump(c, pending_calls[i].patch_offset, offset);
            pending_calls[i] = pending_calls[--pending_count];
        } else {
            i++;
        }
    }
}

/* break/continue loop-context stack. A for-in loop's iterator state lives in ordinary
   registers the caller already owns, so no stack-balancing pop is needed on an early exit. */
#define LOOP_MAX  16
#define BREAK_MAX 32
typedef struct {
    unsigned int top;                     /* continue's target — same value passed to parse_for_body/for_in — meaningless when rotated (see below) */
    unsigned int patches[BREAK_MAX];   /* break's OP_JUMP operand offsets, patched once the loop ends */
    int          patch_count;
    /* Rotated range-for only -- also patches deferred continues to OP_ITER_RANGE_LOOP's own
   position, not the body's start (continue still needs the advance-and-check). */
    bool         rotated;
    unsigned int continue_patches[BREAK_MAX];
    int          continue_patch_count;
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
    loop_stack[loop_depth].rotated      = false;
    loop_depth++;
    return true;
}

/* Rotated range-for only -- `top` is never read here (parse_continue defers instead). */
static bool loop_push_rotated(void) {
    if (loop_depth >= LOOP_MAX) {
        error_at("Too many nested loops (max %d)", LOOP_MAX);
        return false;
    }
    loop_stack[loop_depth].patch_count          = 0;
    loop_stack[loop_depth].rotated              = true;
    loop_stack[loop_depth].continue_patch_count = 0;
    loop_depth++;
    return true;
}

/* Patches every pending break to exit_target, then pops the loop context. */
static void loop_pop_and_patch(Chunk* c, unsigned int exit_target) {
    LoopContext* ctx = &loop_stack[loop_depth - 1];
    for (int i = 0; i < ctx->patch_count; i++)
        patch_jump(c, ctx->patches[i], exit_target);
    loop_depth--;
}

/* Rotated range-for only — same as loop_pop_and_patch, plus patches every deferred `continue` to
   land at continue_target (OP_ITER_RANGE_LOOP's own position, not the loop body's start — continue
   still needs to run the advance-and-check, not just re-enter the body from the top). */
static void loop_pop_and_patch_rotated(Chunk* c, unsigned int exit_target, unsigned int continue_target) {
    LoopContext* ctx = &loop_stack[loop_depth - 1];
    for (int i = 0; i < ctx->patch_count; i++)
        patch_jump(c, ctx->patches[i], exit_target);
    for (int i = 0; i < ctx->continue_patch_count; i++)
        patch_jump(c, ctx->continue_patches[i], continue_target);
    loop_depth--;
}

/* A struct type isn't a callable offset -- OP_STRUCT_NEW just needs to know `Name(...)` means
   construct. Parse-time-only, can't live on the Chunk. */
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

/* Forces rk into the current watermark -- OP_CALL's args must be contiguous. A fresh temp
   already sits there (this file's free-then-allocate discipline), so it's reused directly
   instead of allocating past it and leaving a gap that breaks contiguity for a later argument. */
static int arg_materialize(Chunk* c, int rk) {
    rk = box_if_raw(c, rk);
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

/* Shared bulk-copy prep for calls/arrays/dicts -- all three opcodes need contiguous operand
   registers. Returns the count; *out_base unspecified when count==0. */
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

/* Decodes backslash escapes into `out`, returning the decoded length. Shared by
   pool_escaped_string and parse_interpolated_expr (whose raw text arrives with the same escapes
   still intact, since the outer lex_string() already scanned past them). */
static unsigned int decode_string_escapes(const char* s, unsigned int len, char* out) {
    unsigned int o = 0;
    for (unsigned int i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            i++;
            switch (s[i]) {
                case 'n':  out[o++] = '\n'; break;
                case 't':  out[o++] = '\t'; break;
                case 'r':  out[o++] = '\r'; break;
                case '\\': out[o++] = '\\'; break;
                case '"':  out[o++] = '"';  break;
                /* `\{` unescapes to a bare `{`, same as `\"` unescapes to a bare quote. */
                case '{':  out[o++] = '{';  break;
                default:   out[o++] = '\\'; out[o++] = s[i]; break;
            }
        } else {
            out[o++] = s[i];
        }
    }
    return o;
}

/* Plain string literals only -- needed because dict keys must be strings. */
static unsigned int pool_escaped_string(Chunk* c, const char* s, unsigned int len) {
    static char buf[4096];
    if (len >= sizeof(buf)) {
        error_at("String literal too long (max %u bytes)", (unsigned int)sizeof(buf) - 1);
        len = 0;
    }
    unsigned int out = decode_string_escapes(s, len, buf);
    char* owned = xmalloc((size_t)out + 1);
    memcpy(owned, buf, out);
    owned[out] = '\0';
    AerVal sv = aer_make_string(owned, out);
    return chunk_add_pool(c, sv);
}

/* `and`/`or`/`as`/`|>` are handled specially elsewhere (short-circuit jumps, a bare type name,
   call-argument prepending) -- returns false for anything outside this table. `not` isn't in this
   table at all: it's a prefix parsed by parse_not, recursing into parse_binary at precedence 2
   (and's own slot) so it binds tighter than and/or but looser than everything below. */
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

/* Sub-parses `{expr}` as a genuine expression (calls, arithmetic, indexing, field access), in
   its own lexer span so it can run mid-scan of the outer string literal. The whole span must be
   consumed by exactly one expression. */
static int parse_interpolated_expr(Chunk* c, const char* text, unsigned int len) {
    char* decoded = xmalloc((size_t)len + 1);
    unsigned int decoded_len = decode_string_escapes(text, len, decoded);

    LexerState* saved = lexer_save_state();
    lexer_begin_span(decoded, decoded_len);
    free(decoded);   /* lexer_begin_span copies it into its own owned buffer */
    lex();
    int rk = parse_binary(c, 0);
    if (!parse_had_error && !equal(TOKEN_END_OF_FILE))
        error_at("Unexpected token in string interpolation");
    lexer_restore_state(saved);
    return rk;
}

/* Literal segments and interpolated values concatenate via OP_ADD; interpolation's
   value-to-string step goes through OP_UNARY's folded-in OP_TO_STR. */
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
                emit_binary(c, dest, OP_ADD, result, rk_seg);
                result = dest;
            }
        }
        if (i >= len) break;

        /* Depth-aware so a nested '{'/'}'  (a dict literal) doesn't end the scan early. */
        i++;   /* skip '{' */
        unsigned int expr_start = i;
        int depth = 1;
        while (i < len && depth > 0) {
            if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (s[i] == '{') { depth++; i++; continue; }
            if (s[i] == '}') { depth--; if (depth == 0) break; i++; continue; }
            i++;
        }
        if (depth != 0) { error_at("Unclosed '{' in string"); break; }
        if (i == expr_start) { error_at("Empty '{}' in string"); i++; continue; }

        unsigned int expr_len = i - expr_start;
        int rk_expr = parse_interpolated_expr(c, s + expr_start, expr_len);
        if (parse_had_error) { i++; continue; }
        int expr_reg = materialize(c, rk_expr);

        /* Reuses expr_reg as the OP_TO_STR destination when it's already a temp, avoiding a stranded
           dead temp (reg_free is LIFO). A non-temp expr_reg still gets a fresh destination. */
        int str_dest = is_temp(expr_reg) ? expr_reg : reg_alloc();
        chunk_emit(c, PACK_UNARY(str_dest, OP_TO_STR, expr_reg));

        if (result < 0) {
            result = str_dest;
        } else {
            if (is_temp(str_dest)) reg_free(1);
            if (is_temp(result))   reg_free(1);
            int dest = reg_alloc();
            emit_binary(c, dest, OP_ADD, result, str_dest);
            result = dest;
        }
        i++;   /* skip '}' */
    }

    if (result < 0) {
        /* A fresh, owned empty buffer, not a static literal. */
        char* empty_buf = xmalloc(1);
        empty_buf[0] = '\0';
        result = (int)chunk_add_pool(c, aer_make_string(empty_buf, 0)) | RK_CONST_FLAG;
    }

    lex();
    return result;
}

/* Unary operators sit above this in the precedence chain (parse_unary). */
static int parse_primary_inner(Chunk* c) {
    if (consume(TOKEN_FUNCTION)) return parse_function_expr(c);
    if (consume(TOKEN_OPEN_PARENTHESE)) {
        int rk = parse_binary(c, 0);
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after expression");
        return rk;
    }
    /* Elements share call arguments' contiguous-materialization helper, then OP_ARRAY_NEW. */
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
    /* Each item is a key:value pair, so key/value materialize back to back, landing at
       pair_reg_base+2i/+2i+1 to match OP_DICT_NEW's layout. */
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

        /* Packed array construction, same is_struct_name precedence as `Type(...)`. Peeked (not
           consumed) so ordinary `someVar[i]` falls through untouched. */
        if (equal(TOKEN_OPEN_BRACKET)) {
            int dummy_reg;
            bool shadowed = var_lookup(name_idx, &dummy_reg) ||
                             (function_depth > 0 && global_lookup(name_idx, &dummy_reg));
            if (!shadowed && is_struct_name(name_idx)) {
                lex();   /* consume '[' */
                return parse_packed_array_new(c, name_idx);
            }
        }

        /* A raw-tracked name returns an RK_RAW_*_FLAG-tagged operand, letting a bare reference compose
           through further arithmetic without boxing. */
        int reg;
        if (var_lookup_rk(name_idx, &reg)) return reg;

        /* A known function referenced without a following '(' is a reference to the function itself
           as a value -- built once per reference as a deduped pool constant. */
        unsigned int func_offset, func_arity, func_min_arity, func_max_registers;
        AerVal* func_defaults;
        if (func_full_lookup(c, name_idx, &func_offset, &func_arity, &func_min_arity, &func_defaults,
                                 &func_max_registers)) {
            AerVal fv = build_function_value(func_offset, func_arity, func_min_arity, func_defaults,
                                                 func_max_registers);
            return (int)chunk_add_pool(c, fv) | RK_CONST_FLAG;
        }

        /* Must not fall through to var_slot -- that would read an uninitialized register. */
        if (report_if_shadowed_global(c, name_idx)) return 0;
        error_at("'%s' is not defined", aer_as_string(c->pool[name_idx])->data);
        return 0;
    }
    error_at("Expected an expression (only literals, variables, calls, array/dict literals, arithmetic/comparisons, and parentheses are supported)");
    return 0;
}

/* `[index]`/slice/`.field` reads plus a trailing call-THROUGH-value (not call-by-name) --
   covers functions stored in a container and called via index/key, and currying as a side
   effect. `rk` is reused in place as the call's own dest_reg. */
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
            /* A bare `:` means no start; `]` right after means no end -- either compiles to an RK null
               constant. */
            bool has_start = !equal(TOKEN_COLON);
            int rk_start = has_start ? parse_binary(c, 0) : 0;

            if (!consume(TOKEN_COLON)) {
                require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
                if (parse_had_error) return rk;

                int arr_reg = materialize(c, rk);

                /* Fused since a packed array has no standalone `expr[index]` value. Emitted for EVERY
                   `[index].field` read, dispatching at runtime on packed vs ordinary. */
                if (equal(TOKEN_DOT)) {
                    lex();
                    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return rk; }
                    unsigned int field_idx = chunk_add_pool(c, token.value);
                    lex();

                    if (is_temp(rk_start)) reg_free(1);
                    if (is_temp(arr_reg))  reg_free(1);

                    if (!rk20_fits(rk_start) || !pool_idx_fits(field_idx, FUSED_FIELD_NAME_MAX)) {
                        error_at("Expression too large to compile (register/constant/field index exceeds the fused index-field-op encoding's range)");
                        return rk;
                    }
                    int dest = reg_alloc();
                    chunk_emit(c, PACK_INDEX_FIELD_GET(dest, arr_reg, field_idx, rk_start));
                    rk = dest;
                    continue;
                }

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
        /* Same shape as '[...]' above -- chained forms (p.pos.x) work since each step feeds the next. */
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

/* A fresh primary, then its postfix chain via parse_postfix_chain. */
static int parse_primary(Chunk* c) {
    return parse_postfix_chain(c, parse_primary_inner(c));
}

/* `not` binds tighter than `and`/`or` but looser than every other operator (bitwise, comparison,
   `in`, arithmetic, `as`) -- Python's placement, not a uniform-tight unary like -/~. Achieved by
   recursing into the binary climb at `and`'s own precedence (2) instead of back into parse_unary:
   `not x in y` grabs `x in y` (prec 7 > 2) as its operand before `not` is applied, so it parses as
   `not (x in y)`, while a following `and`/`or` (prec <= 2) stops the climb and combines with the
   whole `not ...` result at the outer level. */
static int parse_not(Chunk* c) {
    int rk = parse_binary(c, 2);
    rk = box_if_raw(c, rk);
    if (is_temp(rk)) reg_free(1);
    int dest = reg_alloc();
    if (!rk9_fits(rk)) {
        error_at("Expression too large to compile (register/constant index exceeds the unary-op encoding's range)");
        return dest;
    }
    chunk_emit(c, PACK_UNARY(dest, OP_NOT, rk));
    return dest;
}

/* Recurses into parse_unary so chained unary (`~~x`) works. rk is RK-encoded like OP_BINARY. */
static int parse_unary_inner(Chunk* c) {
    if (consume(TOKEN_NOT)) return parse_not(c);

    Opcode unary_op;
    if      (consume(TOKEN_BITWISE_NOT)) unary_op = OP_BITWISE_NOT;
    else if (consume(TOKEN_SUBTRACT))    unary_op = OP_NEGATE;
    else return parse_primary(c);

    int rk = parse_unary(c);
    rk = box_if_raw(c, rk);   /* No raw-native unary form -- box first, or a raw slot index gets misread as a register. */
    if (is_temp(rk)) reg_free(1);   /* free-then-allocate, matching every other site */
    int dest = reg_alloc();
    if (!rk9_fits(rk)) {
        error_at("Expression too large to compile (register/constant index exceeds the unary-op encoding's range)");
        return dest;
    }
    chunk_emit(c, PACK_UNARY(dest, unary_op, rk));
    return dest;
}

static int parse_unary(Chunk* c) {
    return parse_unary_inner(c);
}

/* Both the lhs-false and rhs-false paths land on the same "result = false" code. `dest`
   is allocated once, after both operands free. */
/* `a && b` = a if falsy(a), else b -- the operand itself, not a coerced boolean (matches
   Python/Lua). dest is lhs's own temp register when it has one, else a fresh copy (never an
   existing variable's permanent register, which this must not clobber). */
static int compile_and(Chunk* c, int lhs, unsigned int prec) {
    int reg_lhs = materialize(c, lhs);
    int dest;
    if (is_temp(reg_lhs)) {
        dest = reg_lhs;
    } else {
        dest = reg_alloc();
        chunk_emit(c, PACK2(OP_MOVE, dest, reg_lhs));
    }
    unsigned int patch_skip = emit_jump_if_false_reg(c, dest);   /* lhs falsy -- dest already holds it */

    int rk_rhs = parse_binary(c, prec);
    int reg_rhs = materialize(c, rk_rhs);
    if (reg_rhs != dest) chunk_emit(c, PACK2(OP_MOVE, dest, reg_rhs));
    if (is_temp(reg_rhs)) reg_free(1);

    patch_jump(c, patch_skip, c->count);
    return dest;
}

/* `a || b` = a if truthy(a), else b. No jump-if-true opcode exists, so this restructures to
   jump-if-false only -- lhs-false jumps into a "use rhs" block; lhs-true jumps past it, keeping
   dest's already-in-place value. */
static int compile_or(Chunk* c, int lhs, unsigned int prec) {
    int reg_lhs = materialize(c, lhs);
    int dest;
    if (is_temp(reg_lhs)) {
        dest = reg_lhs;
    } else {
        dest = reg_alloc();
        chunk_emit(c, PACK2(OP_MOVE, dest, reg_lhs));
    }
    unsigned int patch_use_rhs = emit_jump_if_false_reg(c, dest);
    chunk_emit(c, OP_JUMP);
    unsigned int patch_end = c->count; chunk_emit(c, 0);

    patch_jump(c, patch_use_rhs, c->count);
    int rk_rhs = parse_binary(c, prec);
    int reg_rhs = materialize(c, rk_rhs);
    if (reg_rhs != dest) chunk_emit(c, PACK2(OP_MOVE, dest, reg_rhs));
    if (is_temp(reg_rhs)) reg_free(1);

    patch_jump(c, patch_end, c->count);
    return dest;
}

/* For a Result, the call is conditional: skipped (propagating the failed Result unchanged) or
   unwrapped in place. Dispatches on dest's runtime type via OP_IS_RESULT -- an ordinary value
   costs one extra type check over pre-Result behavior. */
static unsigned int compile_pipe_guard_begin(Chunk* c, int dest) {
    int check = reg_alloc();
    chunk_emit(c, PACK2(OP_IS_RESULT, check, dest));
    unsigned int patch_not_result = emit_jump_if_false_reg(c, check);

    unsigned int err_idx = chunk_add_pool(c, aer_int(1));
    emit_index_get(c, check, dest, (int)err_idx | RK_CONST_FLAG);
    unsigned int null_idx = chunk_add_pool(c, aer_null());
    emit_binary(c, check, OP_EQ, check, (int)null_idx | RK_CONST_FLAG);
    unsigned int patch_skip_call = emit_jump_if_false_reg(c, check);

    unsigned int val_idx = chunk_add_pool(c, aer_int(0));
    emit_index_get(c, dest, dest, (int)val_idx | RK_CONST_FLAG);

    patch_jump(c, patch_not_result, c->count);
    reg_free(1);   /* Freed before the next pipe-call argument, restoring next_temp_register to where
                       arg_materialize would put it regardless. */
    return patch_skip_call;
}

/* Jumps the "call happened" path past the skip block, patches the skip jump to the same spot. */
static void compile_pipe_guard_end(Chunk* c, unsigned int patch_skip_call) {
    chunk_emit(c, OP_JUMP);
    unsigned int patch_over_skip = c->count;
    chunk_emit(c, 0);
    unsigned int end = c->count;
    patch_jump(c, patch_skip_call, end);
    patch_jump(c, patch_over_skip, end);
}

/* Reuses parse_call's own resolution and arg_materialize directly -- the only new part is
   materializing `lhs` into argument zero first. */
static int compile_pipe(Chunk* c, int lhs) {
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after '|>'"); return lhs; }

    /* Same "piped value becomes argument zero" idea, routed through OP_CALL_MODULE. Checked via
       chunk_is_imported before consuming the identifier. */
    if (chunk_is_imported(c, aer_as_string(token.value)->data, aer_as_string(token.value)->length)) {
        int module_id = module_call_id(aer_as_string(token.value));
        unsigned int module_idx = chunk_add_pool(c, token.value);
        lex();
        require(TOKEN_DOT, "expected '.' after module name");
        if (parse_had_error) return lhs;
        if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after '.'"); return lhs; }
        int fn_id = module_fn_id(module_id, aer_as_string(token.value));
        unsigned int fn_idx = chunk_add_pool(c, token.value);
        lex();
        require(TOKEN_OPEN_PARENTHESE, "expected '(' after piped module function name");
        if (parse_had_error) return lhs;

        if (is_temp(lhs)) reg_free(1);
        int arg_reg_base = arg_materialize(c, lhs);
        int dest = arg_reg_base;
        unsigned int patch_skip_call = compile_pipe_guard_begin(c, dest);

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

        if (arg_count > 1) reg_free(arg_count - 1);
        if (!pool_idx_fits(module_idx, CALL_MODULE_NAME_MAX) || !pool_idx_fits(fn_idx, CALL_MODULE_NAME_MAX)) {
            error_at("Expression too large to compile (module/function name index exceeds the module-call encoding's range)");
            return dest;
        }
        chunk_emit(c, PACK_CALL_MODULE(dest, arg_reg_base, arg_count, module_idx, fn_idx));
        chunk_emit(c, module_id);
        chunk_emit(c, fn_id);
        compile_pipe_guard_end(c, patch_skip_call);
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
    if (!is_struct && !func_lookup(c, name_idx, &func_offset)) {
        error_at("Unknown function or struct type (must be defined before use)");
        return lhs;
    }

    require(TOKEN_OPEN_PARENTHESE, "expected '(' after piped function name");
    if (parse_had_error) return lhs;

    if (is_temp(lhs)) reg_free(1);
    int arg_reg_base = arg_materialize(c, lhs);
    int dest = arg_reg_base;
    /* No reasonable meaning for short-circuiting a struct construction, so the guard is skipped. */
    unsigned int patch_skip_call = is_struct ? 0 : compile_pipe_guard_begin(c, dest);

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

    if (arg_count > 1) reg_free(arg_count - 1);
    if (is_struct) {
        emit_struct_new(c, dest, name_idx, arg_reg_base, arg_count);
    } else {
        emit_call(c, dest, func_offset, arg_reg_base, arg_count);
        compile_pipe_guard_end(c, patch_skip_call);
    }
    return dest;
}

/* Split from parse_binary so a for-while condition starting with an identifier can resolve it
   once and climb from there. */
static int parse_binary_ops(Chunk* c, unsigned int min_prec, int lhs, unsigned int lhs_start) {
    for (;;) {
        unsigned int prec;
        Opcode op;
        if (!binary_op_info(token.type, &prec, &op) || prec <= min_prec) break;
        lex();

        if (op == OP_AND)  { lhs = compile_and(c, lhs, prec); lhs_start = c->count; continue; }
        if (op == OP_OR)   { lhs = compile_or(c, lhs, prec);  lhs_start = c->count; continue; }
        if (op == OP_PIPE) { lhs = compile_pipe(c, lhs);      lhs_start = c->count; continue; }

        /* T is a bare type name. `x as Point` routes to OP_CHECK_SHAPE, checked via is_struct_name
           before the primitive-name checks (the two tables are disjoint). integer/float/boolean/
           array/hashtable are reserved keywords, dispatched by token type directly so they can't
           be shadowed. `string` stays an ordinary identifier (matched by text, like a struct name)
           since it collides with the stdlib `string` module -- already immune to shadowing either
           way, since this whole cast grammar is a fixed production, never a name lookup. */
        if (op == OP_CAST) {
            bool is_reserved_type = equal(TOKEN_TYPE_INTEGER) || equal(TOKEN_TYPE_FLOAT) ||
                                     equal(TOKEN_TYPE_BOOLEAN) || equal(TOKEN_TYPE_ARRAY) || equal(TOKEN_TYPE_HASHTABLE);
            if (!equal(TOKEN_IDENTIFIER) && !is_reserved_type) { error_at("Expected a type name after 'as'"); return lhs; }

            /* A raw-tracked lhs has no raw-native cast form -- box it first. */
            lhs = box_if_raw(c, lhs);

            if (is_temp(lhs)) reg_free(1);
            int dest = reg_alloc();

            if (equal(TOKEN_IDENTIFIER)) {
                unsigned int type_name_idx = chunk_add_pool(c, token.value);
                const char* type_name = aer_as_string(token.value)->data;
                unsigned int type_len = aer_as_string(token.value)->length;
                lex();
                if (is_struct_name(type_name_idx)) {
                    /* lhs is always a plain register here -- no rk20_fits guard needed. */
                    chunk_emit(c, PACK_CHECK_SHAPE(dest, lhs, type_name_idx));
                } else if (type_len == 6 && strncmp(type_name, "string", 6) == 0) {
                    if (!rk9_fits(lhs)) {
                        error_at("Expression too large to compile (register/constant index exceeds the cast encoding's range)");
                        return dest;
                    }
                    chunk_emit(c, PACK_UNARY(dest, OP_TO_STR, lhs));
                } else {
                    error_at("Unknown type '%.*s' in 'as' cast (must be string/integer/float/boolean/array/hashtable, or a known struct type)",
                             (int)type_len, type_name);
                    return dest;
                }
            } else {
                int cast_type;
                if      (equal(TOKEN_TYPE_INTEGER)) cast_type = CAST_INTEGER;
                else if (equal(TOKEN_TYPE_FLOAT))    cast_type = CAST_FLOAT;
                else if (equal(TOKEN_TYPE_BOOLEAN)) cast_type = CAST_BOOLEAN;
                else if (equal(TOKEN_TYPE_ARRAY)) {
                    error_at("Cannot cast to 'array' -- casting only supports integer/float/string/boolean, or a struct type for a shape check");
                    return dest;
                } else {
                    error_at("Cannot cast to 'hashtable' -- casting only supports integer/float/string/boolean, or a struct type for a shape check");
                    return dest;
                }
                lex();
                if (!rk9_fits(lhs)) {
                    error_at("Expression too large to compile (register/constant index exceeds the cast encoding's range)");
                    return dest;
                }
                chunk_emit(c, PACK_CAST(dest, cast_type, lhs));
            }
            lhs = dest;
            lhs_start = c->count;
            continue;
        }

        /* Checked before parsing the RHS, while lhs's bytecode is still the tail of the chunk --
           excising bytecode from the middle would risk invalidating an RHS jump target. */
        bool lhs_is_field = (c->count - lhs_start == 1 && (c->code[lhs_start] & 0xFF) == OP_FIELD_GET);
        int lhs_struct_reg = 0;
        unsigned int lhs_field_idx = 0;
        if (lhs_is_field) {
            lhs_struct_reg = (int)UNPACK_FIELD_GET_STRUCT(c->code[lhs_start]);
            lhs_field_idx  = (unsigned int)UNPACK_FIELD_GET_FIELD(c->code[lhs_start]);
            c->count = lhs_start;   /* discard lhs's OP_FIELD_GET, never executed */
        }

        unsigned int rhs_start = c->count;
        int rhs = parse_binary(c, prec);   /* same precedence as floor -> left-associative */

        if (lhs_is_field) {
            /* lhs is always the OP_FIELD_GET result (never raw); rhs could be raw -- box it. */
            rhs = box_if_raw(c, rhs);
            if (is_temp(rhs)) reg_free(1);
            if (is_temp(lhs)) reg_free(1);

            int dest = reg_alloc();
            if (!rk20_fits(rhs) || !pool_idx_fits(lhs_field_idx, FUSED_FIELD_NAME_MAX)) {
                error_at("Expression too large to compile (register/constant/field index exceeds the fused field-op encoding's range)");
                return lhs;
            }
            chunk_emit(c, PACK_FIELD_BINARY(dest, lhs_struct_reg, op, lhs_field_idx, rhs));
            lhs = dest;
            lhs_start = c->count;
            continue;
        }

        /* Found via a per-opcode dispatch audit: `x OP y.field` compiled as OP_FIELD_GET immediately
           followed by OP_BINARY reading it back -- two dispatches for one operation. Recognized
           by checking whether the RHS was exactly one bare field read. */
        if (c->count - rhs_start == 1 && (c->code[rhs_start] & 0xFF) == OP_FIELD_GET) {
            int struct_reg = (int)UNPACK_FIELD_GET_STRUCT(c->code[rhs_start]);
            int field_idx  = (int)UNPACK_FIELD_GET_FIELD(c->code[rhs_start]);
            c->count = rhs_start;   /* discard the OP_FIELD_GET just emitted, never executed */

            /* rhs is always the OP_FIELD_GET result (never raw); lhs could be raw -- box it. */
            lhs = box_if_raw(c, lhs);
            if (is_temp(rhs)) reg_free(1);
            if (is_temp(lhs)) reg_free(1);

            int dest = reg_alloc();
            if (!rk20_fits(lhs) || !pool_idx_fits((unsigned int)field_idx, FUSED_FIELD_NAME_MAX)) {
                error_at("Expression too large to compile (register/constant/field index exceeds the fused field-op encoding's range)");
                return lhs;
            }
            chunk_emit(c, PACK_BINARY_FIELD(dest, struct_reg, op, lhs, field_idx));
            lhs = dest;
            lhs_start = c->count;
            continue;
        }

        /* Tries a native raw op first (both provably int/real); false means not raw-composable, and
           lhs/rhs still need the ordinary free/emit_binary treatment. */
        int raw_result;
        if (try_emit_binary_raw(c, op, lhs, rhs, &raw_result)) {
            lhs = raw_result;
            lhs_start = c->count;
            continue;
        }

        /* Free-then-allocate, RHS then LHS, matching compile_node's own discipline exactly. */
        if (is_temp(rhs)) reg_free(1);
        if (is_temp(lhs)) reg_free(1);

        int dest = reg_alloc();
        emit_binary(c, dest, op, lhs, rhs);
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

/* The 6 arithmetic compound-assign operators (bitwise OP= forms were deliberately dropped — a
   second spelling with no new capability). `x OP= expr` compiles to one OP_BINARY with dest ==
   lhs == x's own register. */
static const struct { TokenType tok; Opcode op; } compound_assign_ops[] = {
    { TOKEN_ADD_ASSIGN,          OP_ADD         },
    { TOKEN_SUBTRACT_ASSIGN,     OP_SUB         },
    { TOKEN_MULTIPLY_ASSIGN,     OP_MUL         },
    { TOKEN_DIVIDE_ASSIGN,       OP_DIV         },
    { TOKEN_MODULO_ASSIGN,       OP_MOD         },
    { TOKEN_FLOOR_DIVIDE_ASSIGN, OP_FLOOR_DIV   },
};
#define COMPOUND_ASSIGN_OP_COUNT (int)(sizeof(compound_assign_ops) / sizeof(*compound_assign_ops))

/* No indexed/field targets (out of scope). `name` is already consumed by the caller, which
   decided between this, a bare call, and an indexed write via one token of lookahead. */
static void parse_assignment(Chunk* c, unsigned int name_idx) {
    /* Multiple RHS values pack into a real array (OP_ARRAY_NEW); each target reads its own index
       back via OP_INDEX_GET. Targets resolve via var_slot BEFORE the RHS is parsed -- creating a
       variable after a temp is live could hand out that temp's own register. */
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
            ensure_boxed(c, names[i]);   /* A destructuring target is always a plain boxed write -- an existing raw name must shadow to
                                             boxed before var_slot looks it up. */
            target_regs[i] = var_slot(c, names[i]);
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

        /* 2 targets, 1 source: the (value, err) convention, tolerant of a bare non-Result value. */
        if (count == 2 && rhs_count == 1) {
            chunk_emit(c, PACK_DESTRUCTURE(target_regs[0], target_regs[1], arr_reg));
        } else {
            for (unsigned int i = 0; i < count; i++) {
                unsigned int pool_i = chunk_add_pool(c, aer_int((int64_t)i));
                emit_index_get(c, target_regs[i], arr_reg, (int)pool_i | RK_CONST_FLAG);
            }
        }
        reg_free(1);   /* arr_reg — always a temp, guaranteed by arg_materialize */
        return;
    }

    if (consume(TOKEN_ASSIGN)) {
        int rk_val = parse_binary(c, 0);
        if (parse_had_error) return;

        /* Looked up after parsing the RHS -- a self-referential first assignment creates the name as
           a side effect of parsing it, always boxed, so checking existence now naturally folds
           that case into the ordinary path. */
        int existing_idx = -1;
        for (int i = 0; i < var_count; i++) if (var_names[i] == name_idx) { existing_idx = i; break; }

        if (existing_idx < 0) {
            /* Checked before the raw-eligible fast path could register the name directly, bypassing
               var_slot's own identical check. */
            if (function_depth > 0) {
                for (int i = 0; i < global_count; i++) {
                    if (global_names[i] == name_idx) {
                        error_at("'%s' is a top-level variable — not accessible inside a function; pass it as a parameter (or rename)",
                                 aer_as_string(c->pool[name_idx])->data);
                        return;
                    }
                }
            }
            /* Eligible for raw storage iff: inside a function body, outside any if/else branch, and the
               RHS is provably int/real. */
            RawKind rhs_kind = rk_raw_kind(c, rk_val);
            if (function_depth > 0 && branch_depth == 0 && rhs_kind != RAWK_NONE) {
                int slot = (rhs_kind == RAWK_INT) ? raw_int_reserve_one() : raw_real_reserve_one();
                if (slot >= 0) {
                    int src_slot = raw_materialize(c, rk_val, rhs_kind);
                    if (src_slot < 0) {
                        /* Budget exhausted mid-materialize -- release the reservation, fall through to boxed. */
                        if (rhs_kind == RAWK_INT) raw_int_free(1); else raw_real_free(1);
                    } else {
                        if (src_slot != slot) {
                            /* Direct analog of the boxed path's "reg != rk_val -> MOVE" case. */
                            Opcode move_op = (rhs_kind == RAWK_INT) ? OP_RAW_MOVE_INT : OP_RAW_MOVE_REAL;
                            chunk_emit(c, PACK_RAW_MOVE(move_op, slot, src_slot));
                            int floor_now = (rhs_kind == RAWK_INT) ? raw_int_reserved_floor : raw_real_reserved_floor;
                            if (src_slot >= floor_now) { if (rhs_kind == RAWK_INT) raw_int_free(1); else raw_real_free(1); }
                        }
                        var_names[var_count] = name_idx;
                        var_regs[var_count]  = slot;
                        var_kind[var_count]  = (rhs_kind == RAWK_INT) ? VAR_RAW_INT : VAR_RAW_REAL;
                        var_count++;
                        return;
                    }
                }
            }
        } else if (var_kind[existing_idx] != VAR_BOXED) {
            /* Stays raw (writes in place, no shadow) whenever the new value is the same raw kind --
               the slot's identity never changes, so there's no phi/merge ambiguity to avoid.
               Otherwise shadows to a fresh boxed register -- var_slot must never be called here,
               since it would return the stale raw slot index as if it were a plain register. */
            RawKind rhs_kind = rk_raw_kind(c, rk_val);
            VarKind cur = var_kind[existing_idx];
            bool same_kind = (cur == VAR_RAW_INT && rhs_kind == RAWK_INT) ||
                                 (cur == VAR_RAW_REAL && rhs_kind == RAWK_REAL);
            if (same_kind) {
                int dest_slot = var_regs[existing_idx];
                int src_slot  = raw_materialize(c, rk_val, rhs_kind);
                if (src_slot >= 0) {
                    if (src_slot != dest_slot) {
                        Opcode move_op = (rhs_kind == RAWK_INT) ? OP_RAW_MOVE_INT : OP_RAW_MOVE_REAL;
                        chunk_emit(c, PACK_RAW_MOVE(move_op, dest_slot, src_slot));
                        int floor_now = (rhs_kind == RAWK_INT) ? raw_int_reserved_floor : raw_real_reserved_floor;
                        if (src_slot >= floor_now) { if (rhs_kind == RAWK_INT) raw_int_free(1); else raw_real_free(1); }
                    }
                    return;
                }
                /* Budget exhausted materializing the RHS -- fall through to the shadow path. */
            }
            /* Mirrors var_slot's register-claiming, but rebinds an existing entry in place.
                 A looped self-referential shadow (`total = total + x`) must refuse to compile
               rather than silently freeze at the pre-loop value -- the RHS bytecode reading the
               old raw value was already emitted before this shadow decision, and re-runs every
               iteration reading a slot nothing writes to anymore (real bug found this way). */
            if (loop_depth > 0) {
                error_at("This assignment would change '%s' from a fixed numeric type to a different type, but it's inside a loop — not supported. If you're accumulating with +, -, or *, use the compound form ('%s += ...' etc.) instead — it doesn't have this restriction. Otherwise, restructure so the type change happens outside any loop.",
                         aer_as_string(c->pool[name_idx])->data, aer_as_string(c->pool[name_idx])->data);
                return;
            }
            rk_val = box_if_raw(c, rk_val);
            if (reserved_floor >= FRAME_REGISTERS) {
                error_at("Too many variables (max %d)", FRAME_REGISTERS);
                return;
            }
            int new_reg = reserved_floor;
            reserved_floor++;
            next_temp_register = reserved_floor;
            var_regs[existing_idx] = new_reg;
            var_kind[existing_idx] = VAR_BOXED;
            /* No global_regs update needed — see ensure_boxed's identical reasoning: this path
               only runs on a currently-raw-tracked name, which can never be in global_names. */
            if (rk_val & RK_CONST_FLAG) {
                chunk_emit(c, PACK1(OP_LOADK, new_reg)); chunk_emit(c, rk_val & ~RK_CONST_FLAG);
            } else if (new_reg != rk_val) {
                chunk_emit(c, PACK2(OP_MOVE, new_reg, rk_val));
                if (is_temp(rk_val)) reg_free(1);
            }
            return;
        }

        /* Unchanged existing behavior. rk_val is boxed first in case it's raw-flagged. */
        rk_val = box_if_raw(c, rk_val);
        int reg = var_slot(c, name_idx);
        if (reg < 0) return;   /* error_at already called */
        if (rk_val & RK_CONST_FLAG) {
            chunk_emit(c, PACK1(OP_LOADK, reg)); chunk_emit(c, rk_val & ~RK_CONST_FLAG);
        } else if (reg != rk_val) {
            chunk_emit(c, PACK2(OP_MOVE, reg, rk_val));
            /* Checked after var_slot (which may have just raised the floor), so this correctly recognizes
               rk_val as no-longer-a-temp in the common case. */
            if (is_temp(rk_val)) reg_free(1);
        }
        /* reg == rk_val: the RHS already landed where var_slot reserved -- skip the no-op MOVE. */
        return;
    }

    for (int i = 0; i < COMPOUND_ASSIGN_OP_COUNT; i++) {
        if (!consume(compound_assign_ops[i].tok)) continue;

        /* A raw-tracked compound-assignment target needs its own path -- var_lookup would return a
           bare register-shaped int with no distinguishing flag (real bug: silently misread as a
           plain register). Stays raw for +=/-=/x= with a same-kind RHS; /= always shadows (int/int
           division promotes to real). */
        int existing_idx = -1;
        for (int j = 0; j < var_count; j++) if (var_names[j] == name_idx) { existing_idx = j; break; }

        if (existing_idx >= 0 && var_kind[existing_idx] != VAR_BOXED) {
            RawKind cur_kind = (var_kind[existing_idx] == VAR_RAW_INT) ? RAWK_INT : RAWK_REAL;
            Opcode boxed_op = compound_assign_ops[i].op;
            bool native_op_exists = (boxed_op == OP_ADD || boxed_op == OP_SUB || boxed_op == OP_MUL);

            int rk_rhs = parse_binary(c, 0);
            if (parse_had_error) return;
            RawKind rhs_kind = rk_raw_kind(c, rk_rhs);

            if (native_op_exists && rhs_kind == cur_kind) {
                int dest_slot = var_regs[existing_idx];
                int rhs_slot  = raw_materialize(c, rk_rhs, rhs_kind);
                if (rhs_slot >= 0) {
                    Opcode raw_op;
                    if (cur_kind == RAWK_INT) raw_op = (boxed_op == OP_ADD) ? OP_RAW_ADD_INT : (boxed_op == OP_SUB) ? OP_RAW_SUB_INT : OP_RAW_MUL_INT;
                    else                       raw_op = (boxed_op == OP_ADD) ? OP_RAW_ADD_REAL : (boxed_op == OP_SUB) ? OP_RAW_SUB_REAL : OP_RAW_MUL_REAL;
                    chunk_emit(c, PACK_RAW_ARITH_RR(raw_op, dest_slot, dest_slot, rhs_slot));
                    int floor_now = (cur_kind == RAWK_INT) ? raw_int_reserved_floor : raw_real_reserved_floor;
                    if (rhs_slot >= floor_now) { if (cur_kind == RAWK_INT) raw_int_free(1); else raw_real_free(1); }
                    return;
                }
                /* raw-slot budget exhausted materializing the RHS: fall through to the shadow
                   path below instead of leaving the variable half-updated. */
            }

            /* An ordinary boxed RHS (e.g. nbody.aer's `e += 0.5 * bim * (...)`) accumulates directly into
               the existing raw slot with a runtime tag check instead of shadowing -- no shadow, no
               allocation, safe every loop iteration. A provably-mismatched kind still shadows. */
            if (native_op_exists && rhs_kind == RAWK_NONE) {
                int dest_slot = var_regs[existing_idx];
                int boxed_reg = materialize(c, rk_rhs);
                Opcode raw_op;
                if (cur_kind == RAWK_INT) raw_op = (boxed_op == OP_ADD) ? OP_RAW_ADD_INT_BOXED : (boxed_op == OP_SUB) ? OP_RAW_SUB_INT_BOXED : OP_RAW_MUL_INT_BOXED;
                else                       raw_op = (boxed_op == OP_ADD) ? OP_RAW_ADD_REAL_BOXED : (boxed_op == OP_SUB) ? OP_RAW_SUB_REAL_BOXED : OP_RAW_MUL_REAL_BOXED;
                chunk_emit(c, PACK_RAW_ARITH_BOXED(raw_op, dest_slot, boxed_reg));
                if (is_temp(boxed_reg)) reg_free(1);
                return;
            }

            /* Boxes the current raw value, then performs the compound op -- unlike plain assignment's
               shadow, this genuinely depends on old_slot's value, so a loop re-executing it would
               re-read the stale value every iteration (real bug found this way). No single-pass
               fix exists, so refuse to compile rather than silently corrupt. */
            if (loop_depth > 0) {
                error_at("This compound assignment would change '%s' from a fixed numeric type to a different type, but it's inside a loop — not supported (restructure so the type change happens outside any loop)",
                         aer_as_string(c->pool[name_idx])->data);
                return;
            }
            int old_slot = var_regs[existing_idx];
            Opcode box_op = (cur_kind == RAWK_INT) ? OP_BOX_INT : OP_BOX_REAL;
            if (reserved_floor >= FRAME_REGISTERS) { error_at("Too many variables (max %d)", FRAME_REGISTERS); return; }
            int new_reg = reserved_floor;
            reserved_floor++;
            next_temp_register = reserved_floor;
            chunk_emit(c, PACK_BOX(box_op, new_reg, old_slot));
            var_regs[existing_idx] = new_reg;
            var_kind[existing_idx] = VAR_BOXED;
            /* No global_regs update needed — see ensure_boxed's identical reasoning. */
            rk_rhs = box_if_raw(c, rk_rhs);
            emit_binary(c, new_reg, boxed_op, new_reg, rk_rhs);
            if (is_temp(rk_rhs)) reg_free(1);
            return;
        }

        /* Compound assignment to an undefined name is a compile error -- same shadow-ban as a bare
           reference for an existing top-level global. */
        int reg;
        if (!var_lookup(name_idx, &reg)) {
            int dummy_reg;
            if (function_depth > 0 && global_lookup(name_idx, &dummy_reg)) {
                error_at("'%s' is a top-level variable — not accessible inside a function; pass it as a parameter (or rename)",
                         aer_as_string(c->pool[name_idx])->data);
                return;
            }
            error_at("Compound assignment target must already have a value (no assigning to an undefined name this way)");
            return;
        }

        int rk_rhs = parse_binary(c, 0);
        if (parse_had_error) return;
        emit_binary(c, reg, compound_assign_ops[i].op, reg, rk_rhs);
        if (is_temp(rk_rhs)) reg_free(1);
        return;
    }

    /* A pipe chain from a bare name used as a statement -- non-creating lookup. */
    if (equal(TOKEN_PIPE)) {
        int reg;
        unsigned int lhs_start = c->count;
        if (!var_lookup_rk(name_idx, &reg)) {
            int dummy_reg;
            if (function_depth > 0 && global_lookup(name_idx, &dummy_reg)) {
                error_at("'%s' is a top-level variable — not accessible inside a function; pass it as a parameter (or rename)",
                         aer_as_string(c->pool[name_idx])->data);
                return;
            }
            error_at("'%s' is not defined (a pipe chain's source must already have a value)",
                     aer_as_string(c->pool[name_idx])->data);
            return;
        }
        int rk_result = parse_binary_ops(c, 0, reg, lhs_start);
        discard_statement_result(c, rk_result);
        return;
    }

    error_at("Only plain 'name = expr' or compound assignment is supported here (no field access)");
}

/* Every step but the last resolves as a GET; the pending step becomes the final SET (or a
   fused read-modify-write). Every GET after the first overwrites its own source register in
   place instead of allocating fresh -- reg_free is a pure LIFO decrement, so an older temp can
   never free while a newer one must stay live. Only the first hop and a non-temp leading index
   still need a genuinely fresh register. */
static void parse_chain_assignment(Chunk* c, unsigned int name_idx, bool first_is_index) {
    int obj_reg;
    bool obj_is_base;   /* true while obj_reg is still name_idx's own permanent register */
    if (var_lookup(name_idx, &obj_reg)) {
        obj_is_base = true;
    } else if (function_depth > 0 && global_lookup(name_idx, &obj_reg)) {
        /* `name` isn't a local of the current function but IS an existing top-level global —
           same shadow-ban error var_slot enforces for a bare reference. */
        error_at("'%s' is a top-level variable — not accessible inside a function; pass it as a parameter (or rename)",
                 aer_as_string(c->pool[name_idx])->data);
        return;
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

    /* Fuses into OP_INDEX_FIELD_GET/SET for the same reason the read side does -- a packed array
       has no standalone `name[index]` value. Falls through to the general path for any further
       chaining. */
    if (first_is_index && equal(TOKEN_DOT)) {
        lex();
        if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected field name after '.'"); return; }
        unsigned int fused_field_idx = chunk_add_pool(c, token.value);
        lex();

        bool no_more_chaining = !equal(TOKEN_OPEN_BRACKET) && !equal(TOKEN_DOT);
        bool is_plain_assign  = equal(TOKEN_ASSIGN);
        int compound_i = -1;
        if (!is_plain_assign) {
            for (int ci = 0; ci < COMPOUND_ASSIGN_OP_COUNT; ci++) {
                if (equal(compound_assign_ops[ci].tok)) { compound_i = ci; break; }
            }
        }

        if (no_more_chaining && (is_plain_assign || compound_i >= 0)) {
            if (!rk9_fits(pending_rk_idx) || !pool_idx_fits(fused_field_idx, FUSED_FIELD_NAME_MAX)) {
                error_at("Expression too large to compile (register/constant/field index exceeds the fused index-field-op encoding's range)");
                return;
            }
            if (is_plain_assign) {
                lex();
                int rk_val = parse_binary(c, 0);
                if (parse_had_error) return;
                rk_val = box_if_raw(c, rk_val);
                if (!rk9_fits(rk_val)) {
                    error_at("Expression too large to compile (value exceeds the fused index-field-set encoding's range)");
                    return;
                }
                chunk_emit(c, PACK_INDEX_FIELD_SET(obj_reg, fused_field_idx, pending_rk_idx, rk_val));
                if (is_temp(rk_val)) reg_free(1);
            } else {
                lex();
                int field_reg = reg_alloc();
                chunk_emit(c, PACK_INDEX_FIELD_GET(field_reg, obj_reg, fused_field_idx, pending_rk_idx));
                int rk_rhs = parse_binary(c, 0);
                if (parse_had_error) return;
                emit_binary(c, field_reg, compound_assign_ops[compound_i].op, field_reg, rk_rhs);
                if (is_temp(rk_rhs)) reg_free(1);
                chunk_emit(c, PACK_INDEX_FIELD_SET(obj_reg, fused_field_idx, pending_rk_idx, field_reg));
                reg_free(1);   /* field_reg */
            }
            if (is_temp(pending_rk_idx)) reg_free(1);
            if (!obj_is_base) reg_free(1);
            return;
        }

        /* Replicates what the general loop's first iteration would do for a pending index step
           immediately followed by '.field', then falls through to the same general machinery. */
        bool reuse_pending_idx_reg = is_temp(pending_rk_idx);
        int dest_reg;
        if (!obj_is_base)               dest_reg = obj_reg;
        else if (reuse_pending_idx_reg) dest_reg = pending_rk_idx;
        else                             dest_reg = reg_alloc();

        emit_index_get(c, dest_reg, obj_reg, pending_rk_idx);

        if (reuse_pending_idx_reg && dest_reg != pending_rk_idx) reg_free(1);

        obj_reg = dest_reg;
        obj_is_base = false;
        pending_is_field  = true;
        pending_field_idx = fused_field_idx;
    }

    while (equal(TOKEN_OPEN_BRACKET) || equal(TOKEN_DOT)) {
        bool reuse_pending_idx_reg = !pending_is_field && is_temp(pending_rk_idx);
        int dest_reg;
        if (!obj_is_base)              dest_reg = obj_reg;         /* reuse the chain's running temp in place */
        else if (reuse_pending_idx_reg) dest_reg = pending_rk_idx;  /* reuse the (dying) index temp's register */
        else                             dest_reg = reg_alloc(); /* first hop, nothing safe to reuse */

        if (pending_is_field) emit_field_get(c, dest_reg, obj_reg, pending_field_idx);
        else                   emit_index_get(c, dest_reg, obj_reg, pending_rk_idx);

        /* Only free the index temp when it's a different register than dest_reg -- otherwise it was
           already folded in. */
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

    /* Read-modify-write via the same fusion opcodes the single-level cases use -- no dedicated
       OP_INDEX_BINARY fusion exists. */
    for (int i = 0; i < COMPOUND_ASSIGN_OP_COUNT; i++) {
        if (!consume(compound_assign_ops[i].tok)) continue;

        if (pending_is_field) {
            int field_reg = reg_alloc();

            int rk_rhs = parse_binary(c, 0);
            if (parse_had_error) return;
            rk_rhs = box_if_raw(c, rk_rhs);   /* no raw-native fused field-op form exists */

            if (!rk20_fits(rk_rhs) || !pool_idx_fits(pending_field_idx, FUSED_FIELD_NAME_MAX)) {
                error_at("Expression too large to compile (register/constant/field index exceeds the fused field-op encoding's range)");
                return;
            }
            chunk_emit(c, PACK_FIELD_BINARY(field_reg, obj_reg, compound_assign_ops[i].op, pending_field_idx, rk_rhs));
            if (is_temp(rk_rhs)) reg_free(1);

            emit_field_set(c, obj_reg, pending_field_idx, field_reg);
            reg_free(1);   /* field_reg */
        } else {
            int item_reg = reg_alloc();
            emit_index_get(c, item_reg, obj_reg, pending_rk_idx);

            int rk_rhs = parse_binary(c, 0);
            if (parse_had_error) return;

            emit_binary(c, item_reg, compound_assign_ops[i].op, item_reg, rk_rhs);
            if (is_temp(rk_rhs)) reg_free(1);

            emit_index_set(c, obj_reg, pending_rk_idx, item_reg);
            reg_free(1);   /* item_reg */
        }

        if (!pending_is_field && is_temp(pending_rk_idx)) reg_free(1);
        if (!obj_is_base) reg_free(1);
        return;
    }

    /* Finish the pending step as a GET, falling through to a general expression statement. */
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

/* Set just before parse_block returns at a fresh boundary -- lets an outer recovery loop tell
   "still mid-statement" from "a nested recovery already found this boundary". */
static bool recovered_at_boundary = false;

/* Sticky across parse_block's own per-statement reset -- without it, an error fully recovered
   inside a nested block left aer_had_error() silently reporting success. Reset once per parse(). */
static bool any_compile_error = false;

/* Called when the skip-to-boundary loop stops at a plain NEWLINE, not already a boundary -- a
   statement that failed to introduce a block leaves an orphaned indented block otherwise. */
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

/* Per-statement rollback: a compile error inside a body rolls back just that statement and
   resumes at the next boundary, instead of aborting the whole block. */
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
    /* However this loop exited, the lexer now sits at a fresh statement boundary. */
    recovered_at_boundary = true;
    /* If the last statement failed and recovered, parse_had_error is left stale true -- every
       caller checks it right after to decide if ITS OWN construct failed. For a function
       definition this is serious: func_register already ran, so a stale flag would roll back the
       function's real bytecode while its name stays registered (can manifest as an infinite loop
       when called). Reset here so only a genuine top-level failure propagates. */
    parse_had_error = false;
}

static void parse_if(Chunk* c) {
    int rk_cond = parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after if condition");
    /* require() can't abort on failure -- without this, a malformed condition still compiles a
       real branch. */
    if (parse_had_error) return;

    int reg_cond = materialize(c, rk_cond);
    unsigned int patch_jif = emit_jump_if_false_reg(c, reg_cond);
    if (is_temp(reg_cond)) reg_free(1);

    /* branch_depth disqualifies a variable assigned while nonzero from ever being raw-tracked --
       a name assigned different types down mutually-exclusive branches can't be resolved without
       real dataflow analysis. A counter since if/else nests. */
    branch_depth++;
    parse_block(c);
    branch_depth--;
    if (parse_had_error) return;

    if (consume(TOKEN_ELSE)) {
        require(TOKEN_COLON, "expected ':' after else");
        if (parse_had_error) return;
        chunk_emit(c, OP_JUMP);
        unsigned int patch_jmp = c->count;
        chunk_emit(c, 0);
        patch_jump(c, patch_jif, c->count);
        branch_depth++;
        parse_block(c);
        branch_depth--;
        patch_jump(c, patch_jmp, c->count);
    } else {
        patch_jump(c, patch_jif, c->count);
    }
}

/* Shared while/for-while tail: require ':', branch-if-false, body, back-edge to loop_top. */
/* Shared body-and-back-edge tail for every loop form. Callers that promoted reserved_floor can
   safely restore it unconditionally after this returns either way. */
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

/* No exit-time cleanup needed -- col_reg/idx_reg are ordinary registers. Reserves the loop
   variable's register BEFORE compiling the collection expression, so later temps can never
   alias it. */
static void parse_for_in(Chunk* c, unsigned int loop_var_name) {
    ensure_boxed(c, loop_var_name);   /* A for-in loop variable always holds a plain item, never raw -- an existing raw name of the
                                          same spelling must shadow to boxed first. */
    int item_reg = var_slot(c, loop_var_name);
    if (item_reg < 0) return;

    int rk_start = parse_binary(c, 0);   /* the range's start, or the whole collection if no '..' follows */

    /* Direction is inferred at runtime from cur vs end, not step's sign. `..` is for-loop-specific
       syntax here. */
    if (consume(TOKEN_DOT_DOT)) {
        int rk_end = parse_binary(c, 0);
        int rk_step;
        if (consume(TOKEN_DOT_DOT)) rk_step = parse_binary(c, 0);
        else                        rk_step = (int)chunk_add_pool(c, aer_int(1)) | RK_CONST_FLAG;

        require(TOKEN_COLON, "expected ':' after for-in clause");
        if (parse_had_error) return;

        /* Snapshotted once, matching Lua/Python's range-for semantics -- a later mutation of the
           source variable has no effect on an already-running loop. Used to reuse a plain register
           via materialize(), which meant `for i in 0..n:` silently re-read `n` every iteration (an
           undocumented, untested quirk) and forced OP_ITER_RANGE_LOOP to re-validate types every
           dispatch; the snapshot removes both. */
        int cur_reg  = arg_materialize(c, rk_start);
        int end_reg  = arg_materialize(c, rk_end);
        int step_reg = arg_materialize(c, rk_step);

        /* Promotes temps to permanent status for the loop's duration -- without it, a fresh variable
           inside the body could alias cur_reg/step_reg (real bug with nested ranged loops).
           Restored to the pre-loop watermark once the loop's bytecode is emitted. */
        int saved_reserved_floor = reserved_floor;
        reserved_floor = next_temp_register;

        /* Loop-rotated: PREP once before the loop, LOOP at the bottom of the body -- the one form
           whose continue must defer-patch instead of jumping to a known target. */
        unsigned int patch_empty = emit_iter_range_prep(c, cur_reg, end_reg, step_reg, item_reg);

        if (!loop_push_rotated()) {
            reserved_floor     = saved_reserved_floor;
            next_temp_register = saved_reserved_floor;
            return;
        }
        unsigned int body_start = c->count;
        parse_block(c);
        if (parse_had_error) {
            loop_depth--;
            reserved_floor     = saved_reserved_floor;
            next_temp_register = saved_reserved_floor;
            return;
        }

        unsigned int loop_bottom = c->count;
        emit_iter_range_loop(c, cur_reg, end_reg, step_reg, item_reg, body_start);

        unsigned int exit_pos = c->count;
        patch_jump(c, patch_empty, exit_pos);
        loop_pop_and_patch_rotated(c, exit_pos, loop_bottom);

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

    /* idx_reg/col_reg must stay valid across the whole body, so they're protected before the body
       compiles, same as the range branch. */
    int saved_reserved_floor = reserved_floor;
    reserved_floor = next_temp_register;

    unsigned int loop_top = c->count;   /* the iterate opcode is its own back-edge target */
    unsigned int patch_exit = emit_iter_next_array(c, col_reg, idx_reg, item_reg);

    parse_loop_body(c, loop_top, patch_exit);

    reserved_floor     = saved_reserved_floor;
    next_temp_register = saved_reserved_floor;
}

/* Dict-only at runtime -- reserves both loop variables' registers before compiling the
   collection expression. */
static void parse_for_in_pair(Chunk* c, unsigned int key_name, unsigned int val_name) {
    ensure_boxed(c, key_name);   /* same reasoning as parse_for_in's own ensure_boxed call */
    ensure_boxed(c, val_name);
    int key_reg = var_slot(c, key_name);
    if (key_reg < 0) return;
    int val_reg = var_slot(c, val_name);
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

        /* Unambiguous here -- a comma after a for-loop's first identifier can only mean a second loop
           variable, never destructuring. */
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
        /* An existing raw-tracked name's var_slot lookup would return its bare index with no flag
           (real bug: `for i <= n:` with a raw-int i compared garbage). var_slot only as a
           fallback for a genuinely new name. */
        int reg;
        if (!var_lookup_rk(name_idx, &reg)) {
            reg = var_slot(c, name_idx);
            if (reg < 0) return;
        }
        unsigned int loop_top = c->count;
        /* Resolves the postfix chain first, so `for cur.next:` works -- a bare-variable condition is
           unaffected, since the chain loop immediately returns unchanged. */
        int rk_chain = parse_postfix_chain(c, reg);
        int rk_cond = parse_binary_ops(c, 0, rk_chain, c->count);
        parse_for_body(c, loop_top, rk_cond);
        return;
    }
    unsigned int loop_top = c->count;
    int rk_cond = parse_binary(c, 0);
    parse_for_body(c, loop_top, rk_cond);
}

/* Checked before the normal identifier/call/assignment path -- a module name isn't a
   variable, so var_slot/func_lookup must never see it. */
static bool at_module_name(Chunk* c) {
    return token.type == TOKEN_IDENTIFIER &&
           chunk_is_imported(c, aer_as_string(token.value)->data, aer_as_string(token.value)->length);
}

/* Arguments reach the call via contiguous registers, same as every other call site;
   OP_CALL_MODULE bridges to the shared stdlib dispatch rather than reimplementing it. */
/* A module name is always a literal identifier, never a runtime value, so it's fully knowable
   here. Returns CALL_MODULE_DYNAMIC for anything not a fixed core built-in. */
static int module_call_id(AerString* name) {
    if (name->length == 4 && strncmp(name->data, "math", 4) == 0)   return CALL_MODULE_MATH;
    if (name->length == 6 && strncmp(name->data, "random", 6) == 0) return CALL_MODULE_RANDOM;
    if (name->length == 6 && strncmp(name->data, "string", 6) == 0) return CALL_MODULE_STRING;
    if (name->length == 4 && strncmp(name->data, "time", 4) == 0)   return CALL_MODULE_TIME;
    if (name->length == 4 && strncmp(name->data, "json", 4) == 0)   return CALL_MODULE_JSON;
    if (name->length == 10 && strncmp(name->data, "collection", 10) == 0) return CALL_MODULE_COLLECTION;
    if (name->length == 3 && strncmp(name->data, "net", 3) == 0)     return CALL_MODULE_NET;
    if (name->length == 5 && strncmp(name->data, "regex", 5) == 0)   return CALL_MODULE_REGEX;
    if (name->length == 5 && strncmp(name->data, "actor", 5) == 0)   return CALL_MODULE_ACTOR;
    if (name->length == 9 && strncmp(name->data, "scheduler", 9) == 0) return CALL_MODULE_SCHEDULER;
    return CALL_MODULE_DYNAMIC;
}

#define NAME_IS(lit) (name->length == sizeof(lit) - 1 && strncmp(name->data, lit, sizeof(lit) - 1) == 0)

/* A literal identifier, never ambiguous, so resolvable once here. FN_ID_UNKNOWN for anything
   not a known function of that module. */
static int module_fn_id(int module_id, AerString* name) {
    switch (module_id) {
        case CALL_MODULE_MATH:
            if (NAME_IS("sqrt"))    return FN_MATH_SQRT;
            if (NAME_IS("pow"))     return FN_MATH_POW;
            if (NAME_IS("floor"))   return FN_MATH_FLOOR;
            if (NAME_IS("ceil"))    return FN_MATH_CEIL;
            if (NAME_IS("abs"))     return FN_MATH_ABS;
            if (NAME_IS("min"))     return FN_MATH_MIN;
            if (NAME_IS("max"))     return FN_MATH_MAX;
            if (NAME_IS("sin"))     return FN_MATH_SIN;
            if (NAME_IS("cos"))     return FN_MATH_COS;
            if (NAME_IS("log"))     return FN_MATH_LOG;
            if (NAME_IS("log2"))    return FN_MATH_LOG2;
            if (NAME_IS("log10"))   return FN_MATH_LOG10;
            if (NAME_IS("pi"))      return FN_MATH_PI;
            if (NAME_IS("round"))   return FN_MATH_ROUND;
            if (NAME_IS("tan"))     return FN_MATH_TAN;
            if (NAME_IS("exp"))     return FN_MATH_EXP;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_RANDOM:
            if (NAME_IS("random"))  return FN_RANDOM_RANDOM;
            if (NAME_IS("randint")) return FN_RANDOM_RANDINT;
            if (NAME_IS("seed"))    return FN_RANDOM_SEED;
            if (NAME_IS("choice"))  return FN_RANDOM_CHOICE;
            if (NAME_IS("shuffle")) return FN_RANDOM_SHUFFLE;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_STRING:
            if (NAME_IS("upper"))       return FN_STRING_UPPER;
            if (NAME_IS("lower"))       return FN_STRING_LOWER;
            if (NAME_IS("trim"))        return FN_STRING_TRIM;
            if (NAME_IS("contains"))    return FN_STRING_CONTAINS;
            if (NAME_IS("split"))       return FN_STRING_SPLIT;
            if (NAME_IS("starts_with")) return FN_STRING_STARTS_WITH;
            if (NAME_IS("ends_with"))   return FN_STRING_ENDS_WITH;
            if (NAME_IS("repeat"))      return FN_STRING_REPEAT;
            if (NAME_IS("replace"))     return FN_STRING_REPLACE;
            if (NAME_IS("join"))        return FN_STRING_JOIN;
            if (NAME_IS("index_of"))    return FN_STRING_INDEX_OF;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_TIME:
            if (NAME_IS("now"))      return FN_TIME_NOW;
            if (NAME_IS("strftime")) return FN_TIME_STRFTIME;
            if (NAME_IS("sleep"))    return FN_TIME_SLEEP;
            if (NAME_IS("parse"))    return FN_TIME_PARSE;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_JSON:
            if (NAME_IS("encode")) return FN_JSON_ENCODE;
            if (NAME_IS("decode")) return FN_JSON_DECODE;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_COLLECTION:
            if (NAME_IS("append"))   return FN_COLLECTION_APPEND;
            if (NAME_IS("delete"))   return FN_COLLECTION_DELETE;
            if (NAME_IS("copy"))     return FN_COLLECTION_COPY;
            if (NAME_IS("insert"))   return FN_COLLECTION_INSERT;
            if (NAME_IS("index_of")) return FN_COLLECTION_INDEX_OF;
            if (NAME_IS("keys"))     return FN_COLLECTION_KEYS;
            if (NAME_IS("sort"))     return FN_COLLECTION_SORT;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_NET:
            if (NAME_IS("connect")) return FN_NET_CONNECT;
            if (NAME_IS("send"))    return FN_NET_SEND;
            if (NAME_IS("recv"))    return FN_NET_RECV;
            if (NAME_IS("close"))   return FN_NET_CLOSE;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_REGEX:
            if (NAME_IS("match"))   return FN_REGEX_MATCH;
            if (NAME_IS("find"))    return FN_REGEX_FIND;
            if (NAME_IS("replace")) return FN_REGEX_REPLACE;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_ACTOR:
            if (NAME_IS("spawn"))   return FN_ACTOR_SPAWN;
            if (NAME_IS("send"))    return FN_ACTOR_SEND;
            if (NAME_IS("receive")) return FN_ACTOR_RECEIVE;
            if (NAME_IS("call"))    return FN_ACTOR_CALL;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_SCHEDULER:
            if (NAME_IS("add")) return FN_SCHEDULER_ADD;
            if (NAME_IS("run")) return FN_SCHEDULER_RUN;
            return FN_ID_UNKNOWN;
        default:
            return FN_ID_UNKNOWN;
    }
}

#undef NAME_IS

static int parse_module_call(Chunk* c) {
    int module_id = module_call_id(aer_as_string(token.value));
    unsigned int module_idx = chunk_add_pool(c, token.value);
    lex();
    if (!consume(TOKEN_DOT)) {
        error_at("A module can't be used as a value on its own — call a function on it, e.g. 'module.function(...)'");
        return 0;
    }
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a function name after '.'"); return 0; }
    int fn_id = module_fn_id(module_id, aer_as_string(token.value));
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
    if (!pool_idx_fits(module_idx, CALL_MODULE_NAME_MAX) || !pool_idx_fits(fn_idx, CALL_MODULE_NAME_MAX)) {
        error_at("Expression too large to compile (module/function name index exceeds the module-call encoding's range)");
        return dest;
    }
    chunk_emit(c, PACK_CALL_MODULE(dest, base, arg_count, module_idx, fn_idx));
    chunk_emit(c, module_id);
    chunk_emit(c, fn_id);
    return dest;
}

/* The quoted form, for paths the dotted form can't express (explicit relative components, an
   absolute path). Used exactly as written, never dot-converted. Bare native module names aren't
   reachable this way -- a quoted path always means "look on disk". */
static void parse_import_path(Chunk* c) {
    AerString* path_str = aer_as_string(token.value);
    unsigned int path_len = path_str->length;
    char path_buf[256];
    if (path_len == 0 || path_len >= sizeof(path_buf)) { error_at("Import path is empty or too long"); return; }
    memcpy(path_buf, path_str->data, path_len);
    lex();

    char alias_buf[64];
    unsigned int alias_len;
    if (consume(TOKEN_AS)) {
        if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a name after 'as'"); return; }
        AerString* alias = aer_as_string(token.value);
        alias_len = alias->length;
        if (alias_len == 0 || alias_len >= sizeof(alias_buf)) { error_at("Import alias too long"); return; }
        memcpy(alias_buf, alias->data, alias_len);
        lex();
    } else {
        /* Derive the bound name from the last path segment, stripping a trailing ".aer". */
        unsigned int end = path_len;
        if (end > 4 && strncmp(path_buf + end - 4, ".aer", 4) == 0) end -= 4;
        unsigned int start = 0;
        for (unsigned int i = 0; i < end; i++)
            if (path_buf[i] == '/' || path_buf[i] == '\\') start = i + 1;
        alias_len = end - start;
        if (alias_len == 0 || alias_len >= sizeof(alias_buf)) {
            error_at("Cannot derive a module name from this path; add 'as name'");
            return;
        }
        memcpy(alias_buf, path_buf + start, alias_len);
    }
    chunk_add_import(c, alias_buf, alias_len, path_buf, path_len);
}

/* Top level only (a known narrow gap: an import inside an if/for at top level is accepted).
   A failed import is silently not registered. */
static void parse_import(Chunk* c) {
    if (function_depth != 0) {
        error_at("'import' is only allowed at the top level of a file, not inside a function");
        return;
    }
    if (token.type == TOKEN_STRING) { parse_import_path(c); return; }
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected a module name or a quoted path after 'import'"); return; }

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

/* Checked against a fixed list, consistent with every other call target resolving at compile
   time. Struct construction is deliberately excluded -- already resolved via is_struct_name. */
static bool is_builtin_name(Chunk* c, unsigned int name_idx) {
    AerString* s = aer_as_string(c->pool[name_idx]);
    static const char* const names[] = { "length", "print", "type", "assert", "panic", "Result" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t len = strlen(names[i]);
        if (s->length == len && strncmp(s->data, names[i], len) == 0) return true;
    }
    return false;
}

/* Mirrors module_call_id below — only called after is_builtin_name confirms a match. */
static int builtin_call_id(AerString* name) {
    if (name->length == 6 && strncmp(name->data, "length", 6) == 0) return CALL_BUILTIN_LENGTH;
    if (name->length == 5 && strncmp(name->data, "print", 5) == 0) return CALL_BUILTIN_PRINT;
    if (name->length == 4 && strncmp(name->data, "type", 4) == 0) return CALL_BUILTIN_TYPE;
    if (name->length == 6 && strncmp(name->data, "assert", 6) == 0) return CALL_BUILTIN_ASSERT;
    if (name->length == 6 && strncmp(name->data, "Result", 6) == 0) return CALL_BUILTIN_RESULT;
    return CALL_BUILTIN_PANIC;
}

/* Same contiguous-register materialization and result-register reuse as parse_call, emitting
   OP_CALL_BUILTIN instead. */
static int parse_builtin_call(Chunk* c, unsigned int name_idx) {
    int arg_reg_base;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error) return 0;

    int dest = (arg_count > 0) ? arg_reg_base : reg_alloc();
    if (arg_count > 1) reg_free(arg_count - 1);
    int base = arg_reg_base < 0 ? dest : arg_reg_base;
    chunk_emit(c, PACK_CALL_BUILTIN(dest, base, arg_count, name_idx));
    chunk_emit(c, (uint64_t)builtin_call_id(aer_as_string(c->pool[name_idx])));
    return dest;
}

/* Reached as an expression or a bare statement. Handles both function calls and struct
   instantiation (is_struct_name), with global builtins as a third fallback. */
/* Set right after a bare call emits, checked by parse_return to detect a true tail call --
   c->count only grows, so last_bare_call_end == c->count proves the whole return expression is
   exactly one bare call. last_bare_call_start is captured directly (OP_CALL is 2 words, OP_CALL_VALUE
   is 1, so no single fixed backward offset covers both). */
static unsigned int last_bare_call_end   = (unsigned int)-1;
static unsigned int last_bare_call_start = (unsigned int)-1;

/* Reuses the indexing-bracket grammar rather than a call-with-type-as-value form, consistent
   with `Type(...)` meaning construct. Eligibility can't be checked here (a Shape is only fully
   known once OP_DEFINE_STRUCT runs) -- deferred to OP_PACKED_ARRAY_NEW's own runtime check. */
static int parse_packed_array_new(Chunk* c, unsigned int name_idx) {
    int rk_count = parse_binary(c, 0);
    require(TOKEN_CLOSE_BRACKET, "expected ']' after packed array count");
    if (parse_had_error) return 0;
    rk_count = box_if_raw(c, rk_count);   /* no raw-native form of this opcode exists */
    if (!rk9_fits(rk_count)) {
        error_at("Expression too large to compile (register/constant exceeds the packed-array-count encoding's range)");
        return 0;
    }
    if (is_temp(rk_count)) reg_free(1);
    int dest = reg_alloc();
    emit_packed_array_new(c, dest, name_idx, rk_count);
    return dest;
}

static int parse_call(Chunk* c, unsigned int name_idx) {
    /* Builtins win unconditionally -- length/print/type/assert/panic/Result can never be shadowed. */
    if (is_builtin_name(c, name_idx)) return parse_builtin_call(c, name_idx);

    /* A call through an existing local variable resolves via OP_CALL_VALUE instead of compile-time
       name resolution. A top-level variable of this name is never callable from inside a
       function -- reported immediately so it isn't mistaken for a forward reference. */
    int  var_reg = -1;
    bool is_local_var = var_lookup(name_idx, &var_reg);
    if (!is_local_var && function_depth > 0) {
        int dummy_reg;
        if (global_lookup(name_idx, &dummy_reg)) {
            error_at("'%s' is a top-level variable — not accessible inside a function; pass it as a parameter (or rename)",
                     aer_as_string(c->pool[name_idx])->data);
            return 0;
        }
    }
    bool is_var = is_local_var;

    bool is_struct = !is_var && is_struct_name(name_idx);
    unsigned int func_offset = 0, func_arity = 0, func_min_arity = 0, func_max_registers = 0;
    AerVal* func_defaults = NULL;
    bool is_func = !is_var && !is_struct &&
                   func_full_lookup(c, name_idx, &func_offset, &func_arity, &func_min_arity, &func_defaults,
                                        &func_max_registers);
    /* Optimistically assumed to be a function defined later in this same parse() call -- caught
       and reported once parse()'s top-level loop ends if it never actually is. Builtins are already
       handled unconditionally at the top of this function, so reaching here with none of
       is_var/is_struct/is_func true always means an unresolved name, never a builtin. */
    bool is_forward_ref = false;
    const char* call_site_cursor = NULL;
    if (!is_var && !is_struct && !is_func) {
        is_forward_ref = true;
        call_site_cursor = current_source_cursor();   /* captured NOW — before the arg list below consumes past it */
    }

    /* Usually a no-op check, not a copy -- see arg_materialize's own comment. */
    int arg_reg_base;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error) return 0;

    /* A direct call must check arity -- fewer args than declared means some fall back to
       defaults, not garbage from a prior occupant of that frame slot. */
    bool arity_error = is_func && ((unsigned int)arg_count > func_arity || (unsigned int)arg_count < func_min_arity);
    if (arity_error) {
        const char* fname = aer_as_string(c->pool[name_idx])->data;
        if (func_min_arity == func_arity)
            error_at("Function '%s' expects %u argument%s, got %d", fname, func_arity, func_arity == 1 ? "" : "s", arg_count);
        else
            error_at("Function '%s' expects between %u and %u arguments, got %d", fname, func_min_arity, func_arity, arg_count);
        return 0;
    }
    bool needs_call_value = is_func && (unsigned int)arg_count < func_arity;

    /* Matches Lua's own convention of reusing the base register for the result. Determined before
       any callee_reg is allocated, so an extra register (a freshly built function value) always
       lands above dest/the args -- allocating after compaction (the original order) could
       silently hand out a register an omitted-defaults call's argument was still sitting in. */
    int dest = (arg_count > 0) ? arg_reg_base : reg_alloc();
    int base = arg_reg_base < 0 ? dest : arg_reg_base;

    bool needs_callee_reg = needs_call_value;
    int callee_reg = -1;
    if (needs_call_value) {
        AerVal fv = build_function_value(func_offset, func_arity, func_min_arity, func_defaults,
                                             func_max_registers);
        callee_reg = reg_alloc();
        chunk_emit(c, PACK1(OP_LOADK, callee_reg)); chunk_emit(c, (int)chunk_add_pool(c, fv));
    }

    if (is_var) {
        last_bare_call_start = c->count;
        emit_call_value(c, dest, base, arg_count, var_reg);
        last_bare_call_end = c->count;
    } else if (is_struct) {
        emit_struct_new(c, dest, name_idx, base, arg_count);
    } else if (is_func) {
        last_bare_call_start = c->count;
        if (needs_call_value) emit_call_value(c, dest, base, arg_count, callee_reg);
        else                   emit_call(c, dest, func_offset, base, arg_count);   /* exact arity — no forward-ref patching needed, is_func means already resolved */
        last_bare_call_end = c->count;
    } else {
        last_bare_call_start = c->count;
        unsigned int patch_offset = emit_call(c, dest, func_offset, base, arg_count);
        if (is_forward_ref) pending_call_add(name_idx, patch_offset, call_site_cursor);
        last_bare_call_end = c->count;
    }

    /* One combined compaction for both the extra argument registers and callee_reg, not two
       separately-timed frees -- nothing allocates between them. */
    int extra = (arg_count > 1 ? arg_count - 1 : 0) + (needs_callee_reg ? 1 : 0);
    if (extra > 0) reg_free(extra);

    return dest;
}

/* Emits exactly what `Result(value, err)` itself would (CALL_BUILTIN_RESULT), reusing its
   validation rather than duplicating it. Shared by `raise`'s own Result construction. */
static void emit_result_call_and_return(Chunk* c, int reg_base) {
    reg_free(1);   /* the err register — only dest (== reg_base) stays live past the call, same convention parse_builtin_call's own arg_count>1 case follows */
    char* name_buf = xmalloc(7);
    memcpy(name_buf, "Result", 7);
    unsigned int name_idx = chunk_add_pool(c, aer_make_string(name_buf, 6));
    chunk_emit(c, PACK_CALL_BUILTIN(reg_base, reg_base, 2, name_idx));
    chunk_emit(c, (uint64_t)CALL_BUILTIN_RESULT);
    emit_return(c, reg_base);
}

/* `return a, b, ...` packs into an array (OP_ARRAY_NEW) -- destructuring's single-RHS-expression
   case already treats a call's result as "the array to unpack". Tail-call optimization: `return
   f(args)` with nothing else wrapping the call patches that call's opcode in place, not
   reachable for the multi-return case (the array built there isn't the call's own result). A
   plain `return` always means "a success value (or values)" -- signaling failure is `raise`'s job
   alone, so there's no shape ambiguity left for this to detect or reject. */
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
            int op_slot = (int)last_bare_call_start;
            int orig_op = c->code[op_slot] & 0x7F;
            if (orig_op == OP_CALL || orig_op == OP_CALL_VALUE) {
                int tail_op = (orig_op == OP_CALL) ? OP_TAIL_CALL : OP_TAIL_CALL_VALUE;
                c->code[op_slot] = (c->code[op_slot] & ~0x7F) | tail_op;
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

/* `raise <expr>` signals a recoverable failure -- builds a Result with the value forced to null. */
static void parse_raise(Chunk* c) {
    if (function_depth == 0) { error_at("'raise' outside function"); return; }
    unsigned int null_idx = chunk_add_pool(c, aer_null());
    int reg_base = arg_materialize(c, (int)null_idx | RK_CONST_FLAG);
    int rk_err = parse_binary(c, 0);
    if (parse_had_error) return;
    arg_materialize(c, rk_err);
    emit_result_call_and_return(c, reg_base);
}

/* A function value in expression position, near-duplicate of parse_function's logic rather
   than a shared helper -- an anonymous function has no name to self-reference by, so
   parse_function's registration-before-body ordering simply doesn't apply. */
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
    VarKind      saved_var_kind[FRAME_REGISTERS];
    int saved_var_count      = var_count;
    int saved_next_temp      = next_temp_register;
    int saved_reserved_floor = reserved_floor;
    int saved_max_register_used = max_register_used;
    int saved_raw_int_next_temp      = raw_int_next_temp;
    int saved_raw_int_reserved_floor = raw_int_reserved_floor;
    int saved_raw_real_next_temp      = raw_real_next_temp;
    int saved_raw_real_reserved_floor = raw_real_reserved_floor;
    memcpy(saved_var_names, var_names, sizeof(unsigned int) * (size_t)var_count);
    memcpy(saved_var_regs,  var_regs,  sizeof(int) * (size_t)var_count);
    memcpy(saved_var_kind,  var_kind,  sizeof(VarKind) * (size_t)var_count);
    var_count          = 0;
    next_temp_register = 0;
    reserved_floor     = 0;
    max_register_used  = 0;
    raw_int_next_temp = 0;  raw_int_reserved_floor = 0;
    raw_real_next_temp = 0; raw_real_reserved_floor = 0;

    function_depth++;
    for (int i = 0; i < param_count; i++) var_slot(c, param_names[i]);

    parse_block(c);
    function_depth--;

    if (!parse_had_error) {
        int rk_null  = (int)chunk_add_pool(c, aer_null()) | RK_CONST_FLAG;
        int reg_null = materialize(c, rk_null);
        emit_return(c, reg_null);
    }

    unsigned int captured_max_registers = (unsigned int)max_register_used;

    var_count = saved_var_count;
    memcpy(var_names, saved_var_names, sizeof(unsigned int) * (size_t)saved_var_count);
    memcpy(var_regs,  saved_var_regs,  sizeof(int) * (size_t)saved_var_count);
    memcpy(var_kind,  saved_var_kind,  sizeof(VarKind) * (size_t)saved_var_count);
    next_temp_register = saved_next_temp;
    reserved_floor     = saved_reserved_floor;
    max_register_used  = saved_max_register_used;
    raw_int_next_temp = saved_raw_int_next_temp;   raw_int_reserved_floor = saved_raw_int_reserved_floor;
    raw_real_next_temp = saved_raw_real_next_temp; raw_real_reserved_floor = saved_raw_real_reserved_floor;

    patch_jump(c, patch, c->count);

    unsigned int default_count = (unsigned int)param_count - (unsigned int)min_param_count;
    AerVal* defaults = NULL;
    if (default_count > 0) {
        defaults = xmalloc(sizeof(AerVal) * default_count);
        for (unsigned int i = 0; i < default_count; i++) defaults[i] = param_defaults[(unsigned int)min_param_count + i];
    }
    AerVal fv = build_function_value(func_start, (unsigned int)param_count, (unsigned int)min_param_count,
                                         defaults, captured_max_registers);
    return (int)chunk_add_pool(c, fv) | RK_CONST_FLAG;
}

/* Named functions can't nest. Once one parameter has a default, every parameter after it must
   too. */
static void parse_function(Chunk* c) {
    if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected function name"); return; }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    if (is_builtin_name(c, name_idx)) {
        error_at("'%s' is a reserved builtin name and can't be redefined as a function",
                 aer_as_string(token.value)->data);
        return;
    }
    lex();

    require(TOKEN_OPEN_PARENTHESE, "expected '(' after function name");
    if (parse_had_error) return;

    unsigned int param_names[FRAME_REGISTERS];
    AerVal       param_defaults[FRAME_REGISTERS];   /* only [min_param_count, param_count) are meaningful */
    int param_count     = 0;
    int min_param_count = 0;
    bool seen_default   = false;
    if (!equal(TOKEN_CLOSE_PARENTHESE)) {
        do {
            if (!equal(TOKEN_IDENTIFIER)) { error_at("Expected parameter name"); return; }
            if (param_count >= FRAME_REGISTERS) {
                error_at("Too many parameters (max %d)", FRAME_REGISTERS);
                return;
            }
            param_names[param_count] = chunk_add_pool(c, token.value);
            lex();
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

    /* Saves the allocator/variable-table state, resets both fresh, restores after -- no stack
       needed since named functions can't nest. */
    unsigned int saved_var_names[FRAME_REGISTERS];
    int          saved_var_regs[FRAME_REGISTERS];
    VarKind      saved_var_kind[FRAME_REGISTERS];
    int saved_var_count      = var_count;
    int saved_next_temp      = next_temp_register;
    int saved_reserved_floor = reserved_floor;
    int saved_max_register_used = max_register_used;
    int saved_raw_int_next_temp      = raw_int_next_temp;
    int saved_raw_int_reserved_floor = raw_int_reserved_floor;
    int saved_raw_real_next_temp      = raw_real_next_temp;
    int saved_raw_real_reserved_floor = raw_real_reserved_floor;
    memcpy(saved_var_names, var_names, sizeof(unsigned int) * (size_t)var_count);
    memcpy(saved_var_regs,  var_regs,  sizeof(int) * (size_t)var_count);
    memcpy(saved_var_kind,  var_kind,  sizeof(VarKind) * (size_t)var_count);
    var_count          = 0;
    next_temp_register = 0;
    reserved_floor     = 0;
    max_register_used  = 0;
    raw_int_next_temp = 0;  raw_int_reserved_floor = 0;
    raw_real_next_temp = 0; raw_real_reserved_floor = 0;

    /* Registered before the body so a self-recursive call inside resolves -- any other
       not-yet-defined function's call just needs to resolve somewhere else in this parse() call
       (pending_call_add). */
    unsigned int default_count = (unsigned int)param_count - (unsigned int)min_param_count;
    AerVal* defaults = NULL;
    if (default_count > 0) {
        defaults = xmalloc(sizeof(AerVal) * default_count);
        for (unsigned int i = 0; i < default_count; i++) defaults[i] = param_defaults[(unsigned int)min_param_count + i];
    }
    func_register(c, name_idx, func_start, (unsigned int)param_count, (unsigned int)min_param_count, defaults);
    /* Patched in once the body finishes -- safe since nothing reads it until this function is
       actually called, long after compilation. Named functions can't nest, so no other
       chunk_add_function call can land between here and the patch. */
    unsigned int this_func_idx = c->function_count - 1;

    /* Incremented before registering parameters -- var_slot only skips recording a global when
       function_depth > 0, and a parameter is never a global. */
    function_depth++;
    for (int i = 0; i < param_count; i++) var_slot(c, param_names[i]);

    parse_block(c);
    function_depth--;

    if (!parse_had_error) {
        /* Implicit 'return null' if control falls off the end. */
        int rk_null  = (int)chunk_add_pool(c, aer_null()) | RK_CONST_FLAG;
        int reg_null = materialize(c, rk_null);
        emit_return(c, reg_null);
    }

    c->functions[this_func_idx].max_registers = (unsigned int)max_register_used;

    var_count = saved_var_count;
    memcpy(var_names, saved_var_names, sizeof(unsigned int) * (size_t)saved_var_count);
    memcpy(var_regs,  saved_var_regs,  sizeof(int) * (size_t)saved_var_count);
    memcpy(var_kind,  saved_var_kind,  sizeof(VarKind) * (size_t)saved_var_count);
    next_temp_register = saved_next_temp;
    reserved_floor     = saved_reserved_floor;
    max_register_used  = saved_max_register_used;
    raw_int_next_temp = saved_raw_int_next_temp;   raw_int_reserved_floor = saved_raw_int_reserved_floor;
    raw_real_next_temp = saved_raw_real_next_temp; raw_real_reserved_floor = saved_raw_real_reserved_floor;

    patch_jump(c, patch, c->count);
}

/* Includes a `[]`/`{}` empty-container template special case -- vm_default_value allocates a
   fresh empty array/dict at each use, avoiding Python's mutable-default bug. Returns false if the
   current token isn't a valid literal default. */
static bool parse_literal_default(Chunk* c, AerVal* out) {
    bool negative = false;
    if (token.type == TOKEN_SUBTRACT) {
        /* Peeked, not consumed -- only actually a negative-number prefix if a number follows. */
        lex();
        negative = true;
    }
    if (token.type == TOKEN_INTEGER) {
        int64_t n = aer_as_int(token.value);
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
        *out = aer_dict_val(d);
    } else {
        return false;
    }
    lex();
    return true;
}

/* Emits OP_DEFINE_STRUCT directly, then registers the type name so `Name(args)` resolves to
   construction. Field defaults share parse_literal_default with function parameters. */
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
    ValueType    field_types[MAX_STRUCT_FIELDS];
    unsigned int field_count = 0;

    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        if (!equal(TOKEN_IDENTIFIER))          { error_at("Expected field name");        return; }
        if (field_count >= MAX_STRUCT_FIELDS)  { error_at("Too many struct fields (max %d)", MAX_STRUCT_FIELDS); return; }

        unsigned int fname = chunk_add_pool(c, token.value);
        lex();

        /* No type annotation -- a field's type is always exactly its default's type. Every field
           already needs an explicit default (below), and that default was always required to match
           its own declared type anyway, so a separate ': type' never carried information the
           default didn't already have. 'null' is the one default with no matching ValueType, and
           is what TYPE_ANY (unconstrained) means here -- never a spelled-out keyword. */
        if (!consume(TOKEN_ASSIGN)) {
            error_at("Struct field '%.*s' must have an explicit default value",
                     (int)aer_as_string(c->pool[fname])->length, aer_as_string(c->pool[fname])->data);
            return;
        }
        AerVal dflt;
        if (!parse_literal_default(c, &dflt)) {
            error_at("Struct field defaults must be a literal value");
            return;
        }
        field_names[field_count]    = fname;
        field_defaults[field_count] = dflt;
        field_types[field_count]    = aer_type(dflt) == TYPE_NULL ? TYPE_ANY : aer_type(dflt);
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
        chunk_emit(c, (int)field_types[i]);
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
    LoopContext* ctx = &loop_stack[loop_depth - 1];
    chunk_emit(c, OP_JUMP);
    if (ctx->rotated) {
        if (ctx->continue_patch_count >= BREAK_MAX) { error_at("Too many continues in one loop (max %d)", BREAK_MAX); return; }
        ctx->continue_patches[ctx->continue_patch_count++] = c->count;
        chunk_emit(c, 0);   /* placeholder — patched by loop_pop_and_patch_rotated once OP_ITER_RANGE_LOOP's own position is known */
    } else {
        chunk_emit(c, (int)ctx->top);   /* known at compile time — no patch needed */
    }
}

/* Auto-prints in shell mode, and either way frees the now-dead result register -- without the
   free, every bare call statement would permanently waste a register slot (fatal for a REPL
   session accumulating statements). */
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
    if (consume(TOKEN_RAISE))    { parse_raise(c);     return; }
    if (consume(TOKEN_STRUCT))   { parse_struct(c);    return; }
    if (consume(TOKEN_BREAK))    { parse_break(c);     return; }
    if (consume(TOKEN_CONTINUE)) { parse_continue(c);  return; }
    if (consume(TOKEN_IMPORT))   { parse_import(c);    return; }
    {
        const char* reserved_word = NULL;
        if      (equal(TOKEN_TYPE_INTEGER))   reserved_word = "integer";
        else if (equal(TOKEN_TYPE_FLOAT))      reserved_word = "float";
        else if (equal(TOKEN_TYPE_BOOLEAN))   reserved_word = "boolean";
        else if (equal(TOKEN_TYPE_ARRAY))     reserved_word = "array";
        else if (equal(TOKEN_TYPE_HASHTABLE)) reserved_word = "hashtable";
        if (reserved_word) {
            error_at("'%s' is a reserved type name and can't be used as a variable", reserved_word);
            return;
        }
    }
    /* Checked right after the call, alongside the bare-name and field-chain pipe-statement cases
       elsewhere. */
    if (at_module_name(c)) {
        int lhs = parse_module_call(c);
        if (equal(TOKEN_PIPE)) {
            unsigned int lhs_start = c->count;
            lhs = parse_binary_ops(c, 0, lhs, lhs_start);
        }
        discard_statement_result(c, lhs);
        return;
    }
    if (equal(TOKEN_IDENTIFIER)) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_OPEN_PARENTHESE)) {
            int lhs = parse_call(c, name_idx);
            if (equal(TOKEN_PIPE)) {
                unsigned int lhs_start = c->count;
                lhs = parse_binary_ops(c, 0, lhs, lhs_start);
            }
            discard_statement_result(c, lhs);
            return;
        }
        if (consume(TOKEN_OPEN_BRACKET))    { parse_chain_assignment(c, name_idx, true);  return; }
        if (consume(TOKEN_DOT))             { parse_chain_assignment(c, name_idx, false); return; }
        parse_assignment(c, name_idx);
        return;
    }
    error_at("Expected a statement (only assignment, indexed/field writes, if/else, for-while, break/continue, function/struct defs, return, imports, and calls are supported)");
}

/* Isolates a fresh, independent program -- call exactly once per independent compile (a
   one-shot file run, a REPL session's startup, or one test case), never between statements of
   the same program. */
void parser_reset(void) {
    reg_reset();
    var_count      = 0;
    global_count   = 0;
    struct_count   = 0;
    pending_count  = 0;
    function_depth = 0;
    loop_depth     = 0;
    parse_had_error   = false;
    /* Function registrations live on the Chunk, not parser statics -- isolation follows each
       program's own fresh Chunk. */
}

/* Every field below mirrors one of this file's persistent statics. */
struct ParserState {
    unsigned int  var_names[FRAME_REGISTERS];
    int           var_regs[FRAME_REGISTERS];
    VarKind       var_kind[FRAME_REGISTERS];
    int           var_count;
    int           next_temp_register;
    int           reserved_floor;
    int           raw_int_next_temp,  raw_int_reserved_floor;
    int           raw_real_next_temp, raw_real_reserved_floor;
    int           branch_depth;
    int           function_depth;
    unsigned int  global_names[FRAME_REGISTERS];
    int           global_regs[FRAME_REGISTERS];
    int           global_count;
    PendingCall* pending_calls;
    int            pending_count, pending_cap;
    LoopContext  loop_stack[LOOP_MAX];
    int            loop_depth;
    unsigned int*  struct_names;
    int            struct_count, struct_cap;
    bool           recovered_at_boundary;
};

/* Transplants each heap array's pointer into the snapshot and nulls the live global -- a plain
   copy would leave the nested compile writing into the SAME memory this snapshot is meant to
   preserve, since parser_reset() only zeros counts, never reallocates. Also resets the register
   allocator directly -- the caller must not also call parser_reset(). */
ParserState* parser_save_state(void) {
    ParserState* s = xmalloc(sizeof(ParserState));

    memcpy(s->var_names, var_names, sizeof(var_names));
    memcpy(s->var_regs,  var_regs,  sizeof(var_regs));
    memcpy(s->var_kind,  var_kind,  sizeof(var_kind));
    s->var_count = var_count;                 var_count = 0;
    s->next_temp_register = next_temp_register;
    s->reserved_floor      = reserved_floor;
    s->raw_int_next_temp = raw_int_next_temp;
    s->raw_int_reserved_floor = raw_int_reserved_floor;
    s->raw_real_next_temp = raw_real_next_temp;
    s->raw_real_reserved_floor = raw_real_reserved_floor;   /* reg_reset() below zeroes all 4, matching how next_temp_register/reserved_floor are handled */
    s->branch_depth = branch_depth;           branch_depth = 0;
    s->function_depth = function_depth;       function_depth = 0;

    memcpy(s->global_names, global_names, sizeof(global_names));
    memcpy(s->global_regs,  global_regs,  sizeof(global_regs));
    s->global_count = global_count;           global_count = 0;

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

/* Frees the nested compile's own heap arrays -- safe to free plainly, since each array holds
   only this parser's own scratch bookkeeping, never the runtime data already persisted onto the
   nested file's own Chunk. */
void parser_restore_state(ParserState* s) {
    free(pending_calls);
    free(struct_names);

    memcpy(var_names, s->var_names, sizeof(var_names));
    memcpy(var_regs,  s->var_regs,  sizeof(var_regs));
    memcpy(var_kind,  s->var_kind,  sizeof(var_kind));
    var_count = s->var_count;
    next_temp_register = s->next_temp_register;
    reserved_floor     = s->reserved_floor;
    raw_int_next_temp = s->raw_int_next_temp;
    raw_int_reserved_floor = s->raw_int_reserved_floor;
    raw_real_next_temp = s->raw_real_next_temp;
    raw_real_reserved_floor = s->raw_real_reserved_floor;
    branch_depth       = s->branch_depth;
    function_depth     = s->function_depth;

    memcpy(global_names, s->global_names, sizeof(global_names));
    memcpy(global_regs,  s->global_regs,  sizeof(global_regs));
    global_count = s->global_count;

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

/* Per-statement rollback: a compile error rolls back that statement and resumes at the next
   boundary, so a REPL can keep going after a mistake. Does not reset the parser's persistent
   tables -- see parser_reset. Once every statement compiles, anything still in the
   forward-reference pending list never got defined in this call -- reported at its own original
   call site (a saved cursor), neutralized by overwriting the call's opcode word with a bare
   OP_HALT (not just patching the jump target, which would still run the call's own side effects
   first). Drained unconditionally -- forward references only resolve within one call. */
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

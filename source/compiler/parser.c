#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include "error.h"
#include "lexer.h"

/* Per-variable storage kind. VAR_BOXED is an ordinary registers[] index; VAR_RAW_INT/REAL is a
   raw_ints/raw_reals slot instead, earned only when a name's first assignment is provably
   int/real (rk_raw_kind), only inside a function body, and only outside any if/else branch
   (P.branch_depth == 0 -- a name assigned different types down mutually-exclusive branches can't
   be resolved without real dataflow analysis). Transitions are one-way: RAW_* can shadow to
   VAR_BOXED on a mismatch; VAR_BOXED never promotes to RAW_*. */
typedef enum { VAR_BOXED, VAR_RAW_INT, VAR_RAW_REAL } VarKind;

/* No comparison emitted yet -- see Parser.last_cmp_offset. */
#define NO_CMP_OFFSET ((unsigned int)-1)

/* A call to a not-yet-registered name is optimistically assumed to be defined later in this
   same parse() call -- recorded here with a placeholder already emitted; func_register patches
   every pending entry once that name registers. Still-pending entries at parse()'s end were
   never defined anywhere in this call. */
typedef struct {
    unsigned int name_idx;
    unsigned int patch_offset;
    const char* call_site_cursor;
} PendingCall;

/* break/continue loop-context stack. A for-in loop's iterator state lives in ordinary
   registers the caller already owns, so no stack-balancing pop is needed on an early exit. */
#define LOOP_MAX 16
#define BREAK_MAX 32
typedef struct {
    unsigned int
        top; /* continue's target -- same value passed to parse_for_body/for_in -- meaningless when rotated (see below) */
    unsigned int patches[BREAK_MAX]; /* break's OP_JUMP operand offsets, patched once the loop ends */
    int patch_count;
    /* Rotated range-for only -- also patches deferred continues to OP_ITER_RANGE_LOOP's own
   position, not the body's start (continue still needs the advance-and-check). */
    bool rotated;
    unsigned int continue_patches[BREAK_MAX];
    int continue_patch_count;
} LoopContext;

/* Every mutable global the compile functions share. P (below) is the live instance;
   parser_save_state/restore_state snapshot it wholesale for a nested compile. */
typedef struct Parser {
    /* The register allocator every compile function shares. next_temp_register tracks the next
       free temp; registers below reserved_floor are permanent and must never be handed out here. */
    int next_temp_register;
    int reserved_floor;
    /* Highest watermark reached since the last reset -- the real peak register need, since both
       counters shrink back down as temps free. Read back after a function body finishes compiling
       but before its own restore runs (parse_function/parse_function_expr). */
    int max_register_used;
    /* Same idea as max_register_used, for raw_int_next_temp/raw_real_next_temp -- the real peak
       raw-slot need per function, captured into ChunkFunction/AerFunction's max_raw_ints/reals
       right alongside max_registers (parse_function/parse_function_expr). */
    int max_raw_int_used, max_raw_real_used;
    /* Raw-slot allocators, mirroring reg_alloc/reg_free/reg_reserve's shape -- except overflow:
       raw_int_alloc/raw_real_alloc return -1 instead of erroring, and every caller falls back to
       ordinary boxed storage for that one value. */
    int raw_int_next_temp, raw_int_reserved_floor;
    int raw_real_next_temp, raw_real_reserved_floor;

    /* Every register's current variable binding (0 registers is effectively local -- see
       var_kind below for storage-kind tracking). */
    unsigned int var_names[FRAME_REGISTERS];
    int var_regs[FRAME_REGISTERS];
    int var_count;
    /* Per-variable storage kind -- see VarKind's own comment above. */
    VarKind var_kind[FRAME_REGISTERS];

    /* Shape-specializing compilation (see vm_call_resolve_specialization, vm.c) -- tracks which of the CURRENT
       function's parameters have been used as the base of a struct-field access, directly or
       through a one-hop plain-local alias. See mark_shape_sensitive's own comment for how these
       three fields work together. */
    bool shape_sensitive_param[FRAME_REGISTERS];
    int current_param_count;
    int alias_source_param[FRAME_REGISTERS]; /* -1 = no known alias */
    /* Populated ONLY during a specialization recompile -- see reg_known_shape's original
       standalone comment (git blame) for the full mechanism; consulted at '.field'/'[idx].field'
       emission sites to skip the generic runtime field-resolution path entirely. */
    Shape* reg_known_shape[FRAME_REGISTERS];
    Shape* reg_known_element_shape[FRAME_REGISTERS];
    /* Side-channel from the plain index-get site to parse_assignment's plain '=' handler -- see
       last_plain_index_dest_reg's original standalone comment (git blame) for why this is keyed
       on exact register-number equality rather than a syntactic flag. */
    int last_plain_index_dest_reg;
    int last_plain_index_src_param;
    Shape* last_plain_index_known_elem_shape;

    /* Highest temp register reached while compiling the loop condition currently being parsed --
       see parse_for_body for why the body must not be allowed to claim one of these. */
    int loop_cond_peak;

    /* Offset of the last single-word comparison emitted, or NO_OFFSET. Lets emit_cond_jump_if_false
       tell "the condition ends in a comparison" from "a word that happens to look like one" --
       instruction lengths vary, so the last word cannot be identified by reading backwards. */
    unsigned int last_cmp_offset;

    /* Nonzero while compiling an if/else branch -- disqualifies raw storage (see VarKind). */
    int branch_depth;
    /* Nonzero while compiling a function body -- lets parse_return reject a top-level return. */
    int function_depth;

    /* Loop-bound-hoisting safety tracking -- lets a `for i in 0..n:` loop skip an array's
       index-side runtime checks when n is proven == length() of the SAME array it indexes. Every
       fact is tracked per-array-register and re-checked at use, never keyed to one "interesting"
       parameter, so proving a bound safe for one array can't let a different one borrow the proof.
       invalidate_register must poison a safe_loop_* entry when EITHER the index or the array half
       is reassigned -- the array half is the easier one to forget. */
    int hint_param_reg;
    unsigned int length_tracked_name;
    bool length_tracked_valid;
    int length_tracked_source_reg;
    int last_length_call_result_reg;
    int last_length_call_arg_reg;
    int safe_loop_item_regs[LOOP_MAX];
    int safe_loop_array_regs[LOOP_MAX];
    int safe_loop_depth;

    /* Top-level variable names, kept solely to detect a function body referencing one -- see
       var_names's own original comment (git blame) for the shadow-ban mechanism. */
    unsigned int global_names[FRAME_REGISTERS];
    int global_regs[FRAME_REGISTERS];
    int global_count;

    /* Set to the assignment target's own name before parsing its RHS, read back after via
       self_ref_watch_seen. var_lookup_rk hooks it, so a self-reference is caught however deep in
       the RHS it sits. Distinguishes `total = total + x` (RHS reads the old value -- unsafe to
       shadow mid-loop) from `total = f()` (does not -- safe). */
    unsigned int self_ref_watch_name;
    bool self_ref_watch_seen;

    /* Forward-referenced calls not yet resolved to a real function -- see PendingCall's own
       comment above. */
    PendingCall* pending_calls;
    int pending_count, pending_cap;

    /* break/continue loop-context stack -- see LoopContext's own comment above. */
    LoopContext loop_stack[LOOP_MAX];
    int loop_depth;

    /* Struct type names declared so far this compile -- at_module_name/parse_struct_construction
       use this to distinguish a struct constructor call from an ordinary function call. */
    unsigned int* struct_names;
    int struct_count, struct_cap;

    /* Set just before parse_block returns at a fresh boundary -- lets an outer recovery loop tell
       "still mid-statement" from "a nested recovery already found this boundary". */
    bool recovered_at_boundary;
    /* Sticky across parse_block's own per-statement reset -- without it, an error fully recovered
       inside a nested block left aer_had_error() silently reporting success. Reset once per
       parse(). */
    bool any_compile_error;
} Parser;
static Parser P;

static void track_peak(int v) {
    if (v > P.max_register_used) P.max_register_used = v;
}
static void track_raw_int_peak(int v) {
    if (v > P.max_raw_int_used) P.max_raw_int_used = v;
}
static void track_raw_real_peak(int v) {
    if (v > P.max_raw_real_used) P.max_raw_real_used = v;
}

/* Raw-slot allocators, mirroring reg_alloc/reg_free/reg_reserve's shape -- except overflow:
   raw_int_alloc/raw_real_alloc return -1 instead of erroring, and every caller falls back to
   ordinary boxed storage for that one value. */
static int raw_int_alloc(void) {
    if (P.raw_int_next_temp >= RAW_REGISTERS_INT) return -1;
    track_raw_int_peak(P.raw_int_next_temp + 1);
    return P.raw_int_next_temp++;
}
static void raw_int_free(int count) {
    P.raw_int_next_temp -= count;
    if (P.raw_int_next_temp < P.raw_int_reserved_floor) P.raw_int_next_temp = P.raw_int_reserved_floor;
}
static int raw_real_alloc(void) {
    if (P.raw_real_next_temp >= RAW_REGISTERS_REAL) return -1;
    track_raw_real_peak(P.raw_real_next_temp + 1);
    return P.raw_real_next_temp++;
}
static void raw_real_free(int count) {
    P.raw_real_next_temp -= count;
    if (P.raw_real_next_temp < P.raw_real_reserved_floor) P.raw_real_next_temp = P.raw_real_reserved_floor;
}

/* Reserves a new permanent raw slot for a variable's first assignment -- the raw analog of
   var_slot claiming P.reserved_floor. Returns -1 on overflow (caller falls back to boxed). */
static int raw_int_reserve_one(void) {
    if (P.raw_int_reserved_floor >= RAW_REGISTERS_INT) return -1;
    int slot = P.raw_int_reserved_floor;
    P.raw_int_reserved_floor++;
    P.raw_int_next_temp = P.raw_int_reserved_floor;
    track_raw_int_peak(P.raw_int_reserved_floor);
    return slot;
}
static int raw_real_reserve_one(void) {
    if (P.raw_real_reserved_floor >= RAW_REGISTERS_REAL) return -1;
    int slot = P.raw_real_reserved_floor;
    P.raw_real_reserved_floor++;
    P.raw_real_next_temp = P.raw_real_reserved_floor;
    track_raw_real_peak(P.raw_real_reserved_floor);
    return slot;
}

void reg_reset(void) {
    P.next_temp_register = 0;
    P.reserved_floor = 0;
    P.max_register_used = 0;
    P.max_raw_int_used = 0;
    P.max_raw_real_used = 0;
    P.raw_int_next_temp = 0;
    P.raw_int_reserved_floor = 0;
    P.raw_real_next_temp = 0;
    P.raw_real_reserved_floor = 0;
}

/* Must refuse a register >= FRAME_REGISTERS -- the register_stack bank is sized assuming no
   frame ever needs more. Returns an in-bounds sentinel after erroring. */
void reg_reserve(int count) {
    if (P.reserved_floor + count > FRAME_REGISTERS) {
        error_at("Too many live variables/temporaries (max %d registers per call)", FRAME_REGISTERS);
        return;
    }
    P.reserved_floor += count;
    P.next_temp_register += count;
    track_peak(P.next_temp_register);
}

int reg_alloc(void) {
    if (P.next_temp_register >= FRAME_REGISTERS) {
        error_at("Too many live variables/temporaries (max %d registers per call)", FRAME_REGISTERS);
        return FRAME_REGISTERS - 1;
    }
    int reg = P.next_temp_register++;
    track_peak(P.next_temp_register);
    if (P.next_temp_register > P.loop_cond_peak) P.loop_cond_peak = P.next_temp_register;
    return reg;
}

void reg_free(int count) {
    P.next_temp_register -= count;
    /* Never free below the reserved floor -- a bug elsewhere shouldn't hand out a local's own
       register as a free temp. */
    if (P.next_temp_register < P.reserved_floor) P.next_temp_register = P.reserved_floor;
}

#ifdef AER_DEBUG_TOOLS
/* Registers below reserved_floor are variables; at or above it they are the temp allocator's to
   hand out. Breaking that hands a live variable's register to the next expression as scratch --
   the failure mode is a variable silently reading back some unrelated intermediate value, never a
   crash. Gated on the debug build so it costs nothing in a normal compile; CI runs the whole
   bench/ and tests/ corpus through that build for check-opcode-coverage, so it is exercised. */
static void assert_variables_below_floor(const char* where) {
    for (int i = 0; i < P.var_count; i++) {
        if (P.var_kind[i] != VAR_BOXED) continue; /* raw kinds index raw_ints/raw_reals, not registers */
        if (P.var_regs[i] >= P.reserved_floor) {
            fprintf(stderr,
                    "aer: internal error: %s left variable slot %d in register %d, at or above the "
                    "reserved floor %d\n",
                    where, i, P.var_regs[i], P.reserved_floor);
            abort();
        }
    }
}
#else
#define assert_variables_below_floor(where) ((void)0)
#endif

/* Guard for RK16-wire opcodes: 32767 registers-or-constants is far beyond any real program, but
   silent truncation would corrupt the instruction rather than refuse to compile. */
static bool rk16_fits(int rk) {
    return (rk & ~RK_CONST_FLAG) <= RK16_MAX_INDEX;
}

/* Same guard, narrower (128), for opcodes needing two RK operands in one packed word -- see
   RK8's own comment in vm.h. */
static bool rk8_fits(int rk) {
    return (rk & ~RK_CONST_FLAG) <= RK8_MAX_INDEX;
}

/* Forward-declared so emit_binary (which needs it) can come before it. */
static int box_if_raw(Chunk* c, int rk);

/* Every binary-operator emission funnels through here. Boxes any raw-flagged operand first
   (a no-op for a plain register or constant) -- only parse_binary_ops's own raw-composing path
   (try_emit_binary_raw) tries the native route before reaching here. */
static int materialize(Chunk* c, int rk);
/* Forward-declared so emit_cond_jump_if_false (below) can come before it. */
static bool is_temp(int rk);

static void emit_binary(Chunk* c, int dest, Opcode op, int rk_lhs, int rk_rhs) {
    rk_lhs = box_if_raw(c, rk_lhs);
    rk_rhs = box_if_raw(c, rk_rhs);
    /* A constant past RK8's budget is spilled into a scratch register instead of refusing to
       compile, freed immediately after the emit -- same materialize()/OP_LOADK hoist the old
       RK9 scheme used for its own, narrower overflow. */
    int spilled = 0;
    if (!rk8_fits(rk_lhs)) {
        rk_lhs = materialize(c, rk_lhs);
        spilled++;
    }
    if (!rk8_fits(rk_rhs)) {
        rk_rhs = materialize(c, rk_rhs);
        spilled++;
    }
    P.last_cmp_offset = c->count;
    chunk_emit(c, PACK3(op, dest, pack_rk8(rk_lhs), pack_rk8(rk_rhs)));
    if (spilled) reg_free(spilled);
}

/* Emits the branch-on-false half of an if/while condition, fusing a bare comparison directly with
   the branch instead of materializing a boolean just to read it back. Covers the plain-boxed and
   raw-boxed-int families only; the other three measured no benefit against the per-opcode
   branch-prediction cost (see vm.h). Detected by inspecting what was just compiled and rolling it
   back, so anything else -- and/or, a bare boolean, a spilled operand -- falls through. */
static unsigned int emit_cond_jump_if_false(Chunk* c, int rk_cond, unsigned int cond_start) {
    /* The comparison only has to be the LAST instruction of the condition, not the whole of it --
       `x * x + y * y > 4.0` computes into raw slots first, and used to miss the fusion entirely
       because the region was three words rather than two. Everything before it is kept. */
    unsigned int cmp_word_start = cond_start;
    if (P.last_cmp_offset != NO_CMP_OFFSET && P.last_cmp_offset >= cond_start &&
        P.last_cmp_offset == c->count - 1) {
        cmp_word_start = P.last_cmp_offset;
    }
    if (c->count - cmp_word_start == 1) {
        uint32_t w = c->code[cmp_word_start];
        bool matched = true;
        Opcode fused_op;
        switch ((Opcode)(w & 0xFF)) {
            case OP_EQ: fused_op = OP_EQ_JUMP_IF_FALSE; break;
            case OP_NEQ: fused_op = OP_NEQ_JUMP_IF_FALSE; break;
            case OP_LT: fused_op = OP_LT_JUMP_IF_FALSE; break;
            case OP_GT: fused_op = OP_GT_JUMP_IF_FALSE; break;
            case OP_LTE: fused_op = OP_LTE_JUMP_IF_FALSE; break;
            case OP_GTE: fused_op = OP_GTE_JUMP_IF_FALSE; break;
            case OP_RAW_LT_INT_BOXED: fused_op = OP_RAW_LT_INT_BOXED_JUMP_IF_FALSE; break;
            case OP_RAW_GT_INT_BOXED: fused_op = OP_RAW_GT_INT_BOXED_JUMP_IF_FALSE; break;
            case OP_RAW_LTE_INT_BOXED: fused_op = OP_RAW_LTE_INT_BOXED_JUMP_IF_FALSE; break;
            case OP_RAW_GTE_INT_BOXED: fused_op = OP_RAW_GTE_INT_BOXED_JUMP_IF_FALSE; break;
            case OP_RAW_LT_REAL_BOXED: fused_op = OP_RAW_LT_REAL_BOXED_JUMP_IF_FALSE; break;
            case OP_RAW_GT_REAL_BOXED: fused_op = OP_RAW_GT_REAL_BOXED_JUMP_IF_FALSE; break;
            case OP_RAW_LTE_REAL_BOXED: fused_op = OP_RAW_LTE_REAL_BOXED_JUMP_IF_FALSE; break;
            case OP_RAW_GTE_REAL_BOXED: fused_op = OP_RAW_GTE_REAL_BOXED_JUMP_IF_FALSE; break;
            default:
                matched = false;
                fused_op = OP_EQ_JUMP_IF_FALSE;
                break; /* value unused when matched is false */
        }
        if (matched && (int)UNPACK_A(w) == rk_cond) {
            uint8_t rk_lhs8 = (uint8_t)UNPACK_B(w), rk_rhs8 = (uint8_t)UNPACK_C(w);
            c->count =
                cmp_word_start; /* discard the standalone comparison (keeping any LOADK before it) -- fused below instead */
            chunk_emit(c, PACK3(fused_op, 0, rk_lhs8, rk_rhs8));
            unsigned int patch = c->count;
            chunk_emit(
                c, 0); /* placeholder -- patched by the caller, same convention as emit_jump_if_false_reg */
            return patch;
        }
    }
    int reg_cond = materialize(c, rk_cond);
    unsigned int patch = emit_jump_if_false_reg(c, reg_cond);
    if (is_temp(reg_cond)) reg_free(1);
    return patch;
}

unsigned int emit_jump_if_false_reg(Chunk* c, int reg) {
    chunk_emit(c, PACK1(OP_JUMP_IF_FALSE_REG, reg));
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);
    return patch_offset;
}

void patch_jump(Chunk* c, unsigned int patch_offset, unsigned int target) {
    c->code[patch_offset] = (uint32_t)target;
}

/* Returns the callee_offset word's offset for a forward-referencing call to patch later
   (pending_call_add); an already-resolved call ignores the return value. func_index is the
   target's index into chunk->functions[] -- lbl_call reads it to size the callee's frame from its
   real max_registers/max_raw_ints/max_raw_reals peaks instead of a flat, function-agnostic
   ceiling. */
unsigned int emit_call(Chunk* c, int dest_reg, unsigned int callee_offset, int arg_reg_base, int arg_count,
                       unsigned int func_index) {
    chunk_emit(c, PACK3(OP_CALL, dest_reg, arg_reg_base, arg_count));
    unsigned int patch_offset = c->count;
    chunk_emit(c, (uint32_t)callee_offset);
    chunk_emit(c, (uint32_t)func_index);
    return patch_offset;
}

void emit_return(Chunk* c, int src_reg) {
    chunk_emit(c, PACK1(OP_RETURN, src_reg));
}

/* Functions as values -- callee_reg is always a plain register (already materialized), so no
   patching is ever needed here. 3 registers fit word0 (PACK3); callee_reg is the 4th and gets
   its own trailing word (no room left in a 32-bit word0 for a 4th 8-bit field). */
void emit_call_value(Chunk* c, int dest_reg, int arg_reg_base, int arg_count, int callee_reg) {
    chunk_emit(c, PACK3(OP_CALL_VALUE, dest_reg, arg_reg_base, arg_count));
    chunk_emit(c, (uint32_t)callee_reg);
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
    if (!rk8_fits(rk_idx)) {
        rk_idx = materialize(c, rk_idx);
        chunk_emit(c, PACK3(OP_INDEX_GET, dest_reg, arr_reg, pack_rk8(rk_idx)));
        reg_free(1);
        return;
    }
    chunk_emit(c, PACK3(OP_INDEX_GET, dest_reg, arr_reg, pack_rk8(rk_idx)));
}

void emit_index_set(Chunk* c, int arr_reg, int rk_idx, int rk_val) {
    rk_idx = box_if_raw(c, rk_idx);
    rk_val = box_if_raw(c, rk_val);
    int spilled = 0;
    if (!rk8_fits(rk_idx)) {
        rk_idx = materialize(c, rk_idx);
        spilled++;
    }
    if (!rk8_fits(rk_val)) {
        rk_val = materialize(c, rk_val);
        spilled++;
    }
    chunk_emit(c, PACK3(OP_INDEX_SET, arr_reg, pack_rk8(rk_idx), pack_rk8(rk_val)));
    if (spilled) reg_free(spilled);
}

/* rk_start/rk_end are RK-encoded like any value operand; a missing bound is an RK-encoded
   null constant built by the caller. RK16 (32767 direct) is generous enough that no hoist is
   needed here in practice, but guard anyway rather than silently corrupt. */
void emit_slice_get(Chunk* c, int dest_reg, int arr_reg, int rk_start, int rk_end) {
    rk_start = box_if_raw(c, rk_start);
    rk_end = box_if_raw(c, rk_end);
    if (!rk16_fits(rk_start) || !rk16_fits(rk_end)) {
        error_at("Expression too large to compile (register/constant index exceeds the slice-get encoding's "
                 "range)");
        return;
    }
    chunk_emit(c, PACK3(OP_SLICE_GET, dest_reg, arr_reg, 0));
    chunk_emit(c, PACK_2X16(pack_rk16(rk_start), pack_rk16(rk_end)));
}

/* pair_rks holds 2*pair_count RK values, key then value. */
void emit_dict_new(Chunk* c, int dest_reg, const int* pair_rks, int pair_count) {
    chunk_emit(c, PACK2(OP_DICT_NEW, dest_reg, pair_count));
    for (int i = 0; i < 2 * pair_count; i++)
        chunk_emit(c, pack_rk16(pair_rks[i]));
}

unsigned int emit_iter_next_array(Chunk* c, int col_reg, int idx_reg, int item_dest_reg) {
    chunk_emit(c, PACK3(OP_ITER_NEXT_ARRAY, col_reg, idx_reg, item_dest_reg));
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0); /* placeholder -- patched by patch_jump once the loop-exit target is known */
    return patch_offset;
}

unsigned int emit_iter_next_pair(Chunk* c, int col_reg, int idx_reg, int key_dest_reg, int val_dest_reg) {
    /* 3 registers fit word0 (PACK3); the 4th (val_dest_reg) gets its own trailing word since a
       32-bit word0 has no room left for it alongside the opcode; end_target stays its own word
       too, uniformly, so patch_jump stays a blind overwrite. */
    chunk_emit(c, PACK3(OP_ITER_NEXT_PAIR, col_reg, idx_reg, key_dest_reg));
    chunk_emit(c, (uint32_t)val_dest_reg);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0); /* placeholder -- patched by patch_jump once the loop-exit target is known */
    return patch_offset;
}

/* Loop-rotated range-for pair, used only by parse_for_in's `for i in a..b..step:` form. */
unsigned int emit_iter_range_prep(Chunk* c, int cur_reg, int end_reg, int step_reg, int item_dest_reg) {
    chunk_emit(c, PACK3(OP_ITER_RANGE_PREP, cur_reg, end_reg, step_reg));
    chunk_emit(c, (uint32_t)item_dest_reg);
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0); /* placeholder -- patched once the loop's overall exit address is known */
    return patch_offset;
}

/* body_target is always already resolved -- unlike every other loop jump, never a patch
   placeholder -- but still gets its own dedicated word, uniformly with PREP, rather than
   packing tighter for one opcode as a special case. */
void emit_iter_range_loop(Chunk* c, int cur_reg, int end_reg, int step_reg, int item_dest_reg,
                          unsigned int body_target) {
    chunk_emit(c, PACK3(OP_ITER_RANGE_LOOP, cur_reg, end_reg, step_reg));
    chunk_emit(c, (uint32_t)item_dest_reg);
    chunk_emit(c, (uint32_t)body_target);
}

void emit_struct_new(Chunk* c, int dest_reg, unsigned int type_name_pool_idx, int arg_reg_base,
                     int arg_count) {
    chunk_emit(c, PACK3(OP_STRUCT_NEW, dest_reg, arg_reg_base, arg_count));
    chunk_emit(c, (uint32_t)type_name_pool_idx);
}

/* `[value; count]` -- fill_reg is already-materialized (the fill expression, evaluated exactly
   once); narrow_flag (0/1/2 = none/int32/float32) is a pure parse-time decision, set only when the
   fill expression was written as an `i`/`f`-suffixed literal directly in this position (see
   parse_primary_inner's own comment). */
void emit_array_repeat(Chunk* c, int dest_reg, int fill_reg, int narrow_flag, int rk_count) {
    chunk_emit(c, PACK3(OP_ARRAY_REPEAT, dest_reg, fill_reg, narrow_flag));
    chunk_emit(c, (uint32_t)pack_rk16(rk_count));
}

void emit_field_get(Chunk* c, int dest_reg, int struct_reg, unsigned int field_name_pool_idx) {
    chunk_emit(c, PACK3(OP_FIELD_GET, dest_reg, struct_reg, 0));
    chunk_emit(c, (uint32_t)field_name_pool_idx);
}

void emit_field_set(Chunk* c, int struct_reg, unsigned int field_name_pool_idx, int rk_val) {
    rk_val = box_if_raw(c, rk_val);
    if (!rk16_fits(rk_val)) {
        error_at("Expression too large to compile (register/constant index exceeds the field-set encoding's "
                 "range)");
        return;
    }
    chunk_emit(c, PACK_OP_A_W16(OP_FIELD_SET, struct_reg, pack_rk16(rk_val)));
    chunk_emit(c, (uint32_t)field_name_pool_idx);
}

/* ------------------------------------------------------------------ */
/* Recursive-descent compiler: the full grammar, from the lexer's token stream straight to
   register bytecode. */
/* ------------------------------------------------------------------ */

static int parse_primary_inner(Chunk* c);
static int parse_primary(Chunk* c);
static int parse_postfix_chain(Chunk* c, int rk);
static int parse_function_expr(Chunk* c);
static void discard_statement_result(Chunk* c, int dest);
static int parse_string_literal(Chunk* c);
static int parse_unary_inner(Chunk* c);
static int parse_unary(Chunk* c);
static int parse_binary_ops(Chunk* c, unsigned int min_prec, int lhs, unsigned int lhs_start);
static int parse_binary(Chunk* c, unsigned int min_prec);
static int compile_and(Chunk* c, int lhs, unsigned int prec);
static int compile_or(Chunk* c, int lhs, unsigned int prec);
static int compile_pipe(Chunk* c, int lhs);
static void parse_statement(Chunk* c);
static void parse_block(Chunk* c);
static void parse_if(Chunk* c);
static void parse_for_while(Chunk* c);
static void parse_for_body(Chunk* c, unsigned int loop_top, int rk_cond);
static void parse_assignment(Chunk* c, unsigned int name_idx);
static void parse_chain_assignment(Chunk* c, unsigned int name_idx, bool first_is_index);
static int parse_call(Chunk* c, unsigned int name_idx);
static void parse_function(Chunk* c);
static bool parse_literal_default(Chunk* c, AerVal* out, bool* out_narrow);
static void parse_return(Chunk* c);
static void parse_struct(Chunk* c);
static bool at_module_name(Chunk* c);
static int module_call_id(AerString* name);
static int module_fn_id(int module_id, AerString* name);
static int parse_module_call(Chunk* c);
static void parse_import(Chunk* c);
static bool is_builtin_name(Chunk* c, unsigned int name_idx);
static int parse_builtin_call(Chunk* c, unsigned int name_idx);
static int parse_contiguous_exprs(Chunk* c, TokenType close_tok, int* out_base);
static void parse_function_signature(Chunk* c, unsigned int* param_names, AerVal* param_defaults,
                                     int* out_param_count, int* out_min_param_count);
static void parse_function_body(Chunk* c, unsigned int* param_names, int param_count, int hint_param_reg,
                                Shape* hint_shape, bool hint_is_element_shape, const int* raw_param_regs,
                                const ValueType* raw_param_types, int raw_param_count,
                                unsigned int* out_max_registers, unsigned int* out_max_raw_ints,
                                unsigned int* out_max_raw_reals);

/* reg is the parameter's own register (0..P.current_param_count-1) OR a register whose value is
   known (via P.alias_source_param) to have come from indexing that parameter -- either way, marks
   that parameter shape-sensitive. Safe to call with any register (out-of-range/no-alias is a
   silent no-op), matching var_lookup_rk's own "harmless on a miss" convention. See the Parser
   struct's own field comments (top of file) for what each of these tables tracks. */
static void mark_shape_sensitive(int reg) {
    if (reg < 0 || reg >= FRAME_REGISTERS) return;
    if (reg < P.current_param_count) {
        P.shape_sensitive_param[reg] = true;
        return;
    }
    int src = P.alias_source_param[reg];
    if (src >= 0) P.shape_sensitive_param[src] = true;
}

/* True iff (arr_reg, idx_rk) matches a pair on the safe_loop_item/array_regs stack. Both halves
   must match: a bound proven for array A must never be trusted for a different array B that
   happens to reuse the same index register. idx_rk must be a plain register.
   Packed-array field callers additionally require arr_reg == P.hint_param_reg (the field offset
   is only valid for that one specialized parameter); typed-array callers do not. */
static bool index_safe_unchecked(int arr_reg, int idx_rk) {
    if (idx_rk & (RK_CONST_FLAG | RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG)) return false;
    for (int i = 0; i < P.safe_loop_depth; i++) {
        if (P.safe_loop_item_regs[i] == idx_rk && P.safe_loop_array_regs[i] == arr_reg) return true;
    }
    return false;
}

/* Call at every site that changes what `reg` holds. Poisons any safe_loop_item/array_regs entry
   keyed on reg as EITHER the index or the array -- missing the array half means trusting a
   register number regardless of what it now holds, an out-of-bounds read through the _UNCHECKED
   opcodes, which have no runtime check to fall back on. Also clears length_tracked_valid if reg is
   the parameter it depends on. Overwrites with -1 rather than removing, so parse_for_in's
   push/pop depth counting is untouched and a dead slot can never be resurrected. */
static void invalidate_register(int reg) {
    if (reg < 0) return;
    if (P.length_tracked_valid && reg == P.length_tracked_source_reg) P.length_tracked_valid = false;
    for (int i = 0; i < P.safe_loop_depth; i++) {
        if (P.safe_loop_item_regs[i] == reg) P.safe_loop_item_regs[i] = -1;
        if (P.safe_loop_array_regs[i] == reg) P.safe_loop_array_regs[i] = -1;
    }
}

/* Compile-time field lookup against a known Shape, resolving offset and type from the Shape's own
   arrays instead of vm_resolve_field_by_shape's runtime lookup. False means the field is absent;
   callers fall back to the generic opcode, which reports the source-level mistake correctly.
   *out_narrow picks between the wide RAW opcode family and its int32/float32 counterpart. */
static bool shape_find_field(Shape* shape, unsigned int field_name_idx, unsigned int* out_offset,
                             ValueType* out_type, bool* out_narrow) {
    for (unsigned int i = 0; i < shape->field_count; i++) {
        if (shape->field_names[i] == field_name_idx) {
            *out_offset = shape->field_offsets[i];
            *out_type = shape->field_types[i];
            *out_narrow = shape->field_narrow[i];
            return true;
        }
    }
    return false;
}

/* Nonzero while compiling an if/else branch -- disqualifies raw storage (see P.var_kind). A
   real counter since if/else nests. */

/* True (after reporting the error) if name_idx is a top-level variable and the caller is
   inside a function body -- see the Parser struct's own field comments (top of file) for how
   global_names/function_depth track this. */
static bool report_if_shadowed_global(Chunk* c, unsigned int name_idx) {
    if (P.function_depth == 0) return false;
    for (int i = 0; i < P.global_count; i++) {
        if (P.global_names[i] == name_idx) {
            error_at("'%s' is a top-level variable — not accessible inside a function; pass it as a "
                     "parameter (or rename)",
                     aer_as_string(c->pool[name_idx])->data);
            return true;
        }
    }
    return false;
}

/* A new variable's register is P.reserved_floor, not P.var_count -- they diverge when a for-loop
   promotes P.reserved_floor for its own iteration registers without registering a name (a fresh
   variable declared inside that loop's body must land above them, or it silently aliases the
   loop's own state -- a real bug found this way with nested for-loops). P.var_regs[] is what makes
   lookup still resolve correctly once the two diverge. */
static int var_slot(Chunk* c, unsigned int name_idx) {
    for (int i = 0; i < P.var_count; i++)
        if (P.var_names[i] == name_idx) return P.var_regs[i];
    /* Shadowing a top-level name is a compile error, not a silent fresh local -- catches both a
       fresh local and a parameter with that name. P.function_depth == 0 (the definition itself)
       and an already-local name are exempt. */
    if (report_if_shadowed_global(c, name_idx)) return -1;
    /* Checks P.reserved_floor, not P.var_count -- P.var_count can lag behind P.reserved_floor once a
       for-loop promotes it (see this function's own comment above). */
    if (P.reserved_floor >= FRAME_REGISTERS) {
        error_at("Too many variables (max %d)", FRAME_REGISTERS);
        return -1;
    }
    int reg = P.reserved_floor;
    P.var_names[P.var_count] = name_idx;
    P.var_regs[P.var_count] = reg;
    /* P.var_kind[] is a persistent static array shared across every function's compilation -- a
       PAST function's raw-tracked local can leave a stale kind at whatever index this new name
       lands on (save/restore only shrinks P.var_count, never zeroes higher indices). Real bug found
       this way: a fresh top-level var inherited VAR_RAW_INT from an earlier function's local at
       the same index. var_slot resets the kind explicitly here, the one place every name is created. */
    P.var_kind[P.var_count] = VAR_BOXED;
    P.var_count++;
    P.reserved_floor++; /* permanently protects this register from the temp allocator */
    P.next_temp_register =
        P.reserved_floor; /* resync -- see this function's own comment for why that's always safe */
    track_peak(P.reserved_floor);
    if (P.function_depth == 0) {
        P.global_names[P.global_count] = name_idx;
        P.global_regs[P.global_count] = reg;
        P.global_count++;
    }
    return reg;
}

/* Non-creating -- a match here is a top-level variable, grounds for the shadow-ban error, not
   a read/write. */
static bool global_lookup(unsigned int name_idx, int* out_reg) {
    for (int i = 0; i < P.global_count; i++)
        if (P.global_names[i] == name_idx) {
            *out_reg = P.global_regs[i];
            return true;
        }
    return false;
}

/* Non-creating counterpart to var_slot -- `name += expr` needs an existing value, so it must
   not silently allocate a fresh register on a miss. */
static bool var_lookup(unsigned int name_idx, int* out_reg) {
    for (int i = 0; i < P.var_count; i++)
        if (P.var_names[i] == name_idx) {
            *out_reg = P.var_regs[i];
            return true;
        }
    return false;
}

/* Works for any RK operand: a temp always lives at/above P.reserved_floor, a permanent variable
   below it. A raw-flagged rk must return false FIRST -- its numeric value could otherwise
   coincidentally compare as >= P.reserved_floor. */
static bool is_temp(int rk) {
    if (rk & (RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG)) return false;
    return !(rk & RK_CONST_FLAG) && rk >= P.reserved_floor;
}

/* The one bridge from raw storage to the rest of the compiler -- every function treating rk as
   register-or-constant must call this first. Frees the raw slot afterward if it was a temp. */
static int box_if_raw(Chunk* c, int rk) {
    if (rk & RK_RAW_INT_FLAG) {
        int slot = rk & RK_RAW_SLOT_MASK;
        int dest = reg_alloc();
        chunk_emit(c, PACK3(OP_BOX_INT, dest, slot, 0));
        if (slot >= P.raw_int_reserved_floor) raw_int_free(1);
        return dest;
    }
    if (rk & RK_RAW_REAL_FLAG) {
        int slot = rk & RK_RAW_SLOT_MASK;
        int dest = reg_alloc();
        chunk_emit(c, PACK3(OP_BOX_REAL, dest, slot, 0));
        if (slot >= P.raw_real_reserved_floor) raw_real_free(1);
        return dest;
    }
    return rk;
}

/* Shadows an existing raw-tracked name to boxed in place (no-op otherwise) -- needed before
   any var_slot call whose caller is about to write a non-raw value, since var_slot has no kind
   awareness (real bug found this way: reusing a raw-int name as a for-in loop variable). */
static void ensure_boxed(Chunk* c, unsigned int name_idx) {
    int existing_idx = -1;
    for (int i = 0; i < P.var_count; i++)
        if (P.var_names[i] == name_idx) {
            existing_idx = i;
            break;
        }
    if (existing_idx < 0 || P.var_kind[existing_idx] == VAR_BOXED) return;

    int old_slot = P.var_regs[existing_idx];
    Opcode box_op = (P.var_kind[existing_idx] == VAR_RAW_INT) ? OP_BOX_INT : OP_BOX_REAL;
    if (P.reserved_floor >= FRAME_REGISTERS) {
        error_at("Too many variables (max %d)", FRAME_REGISTERS);
        return;
    }
    int new_reg = P.reserved_floor;
    P.reserved_floor++;
    P.next_temp_register = P.reserved_floor;
    chunk_emit(c, PACK3(box_op, new_reg, old_slot, 0));
    P.var_regs[existing_idx] = new_reg;
    P.var_kind[existing_idx] = VAR_BOXED;
    /* No P.global_regs update needed -- this path only runs on a currently-raw name. */
}

/* Materializes a constant via OP_LOADK, or boxes a raw value -- OP_JUMP_IF_FALSE_REG needs an
   actual register, no RK/raw form. */
static int materialize(Chunk* c, int rk) {
    rk = box_if_raw(c, rk);
    if (!(rk & RK_CONST_FLAG)) return rk;
    int reg = reg_alloc();
    /* dest+pool_idx both fit word0 now (op(8)+dest(8)+pool_idx(16)) -- no trailing word. */
    chunk_emit(c, PACK_OP_A_W16(OP_LOADK, reg, (unsigned int)(rk & ~RK_CONST_FLAG)));
    return reg;
}

/* Every reader of a variable's value must go through this, not raw P.var_regs[i], or a raw slot
   index gets misread as a plain register (real bug found in string interpolation). */
static bool var_lookup_rk(unsigned int name_idx, int* out_rk) {
    if (name_idx == P.self_ref_watch_name) P.self_ref_watch_seen = true;
    for (int i = 0; i < P.var_count; i++) {
        if (P.var_names[i] != name_idx) continue;
        switch (P.var_kind[i]) {
            case VAR_RAW_INT: *out_rk = RK_RAW_INT_FLAG | P.var_regs[i]; break;
            case VAR_RAW_REAL: *out_rk = RK_RAW_REAL_FLAG | P.var_regs[i]; break;
            default: *out_rk = P.var_regs[i]; break;
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
    if (rk & RK_RAW_INT_FLAG) return RAWK_INT;
    if (rk & RK_RAW_REAL_FLAG) return RAWK_REAL;
    if (rk & RK_CONST_FLAG) {
        AerVal v = c->pool[rk & ~RK_CONST_FLAG];
        if (aer_type(v) == TYPE_INTEGER) return RAWK_INT;
        if (aer_type(v) == TYPE_REAL) return RAWK_REAL;
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
        /* A literal outside the signed 32-bit range must go through the pool instead -- OP_RAW_LOAD_INT's
           immediate is a full int32 now (the old 20-bit immediate's truncation bug -- `i < 20000000`
           silently becoming 77056 -- is closed outright, not just widened again), but AER integers are
           64-bit, so a value beyond INT32_MAX/MIN still needs the pool fallback. */
        if (v >= INT32_MIN && v <= INT32_MAX) {
            chunk_emit(c, PACK1(OP_RAW_LOAD_INT, slot));
            chunk_emit(c, (uint32_t)(int32_t)v);
        } else {
            chunk_emit(c, PACK1(OP_RAW_LOAD_INT_POOL, slot));
            chunk_emit(c, (uint32_t)pool_idx);
        }
        return slot;
    } else {
        if (rk & RK_RAW_REAL_FLAG) return rk & RK_RAW_SLOT_MASK;
        unsigned int pool_idx =
            rk & ~RK_CONST_FLAG; /* real literal already lives in the pool as a full double */
        int slot = raw_real_alloc();
        if (slot < 0) return -1;
        chunk_emit(c, PACK1(OP_RAW_LOAD_REAL, slot));
        chunk_emit(c, (uint32_t)pool_idx);
        return slot;
    }
}

/* Raw-vs-boxed ADD/MUL, real only. Commutative ops only -- SUB/DIV are order-sensitive and their
   common shape puts the raw operand where this can't help. Reuses the existing OP_RAW_*_REAL_BOXED
   opcodes, which promote an integer boxed operand to real.
   Real only, deliberately: the INT variants can't promote the same way, since int+real must promote
   the whole result to real and that cannot be done in place into a raw_ints[] slot. A raw int
   composing with a boxed operand falls through to the boxed path. */
static bool try_emit_arith_raw_boxed(Chunk* c, Opcode op, int rk_lhs, RawKind kind_lhs, int rk_rhs,
                                     RawKind kind_rhs, int* out_rk) {
    if (op != OP_ADD && op != OP_MUL) return false;
    bool lhs_raw = (kind_lhs != RAWK_NONE);
    int raw_rk = lhs_raw ? rk_lhs : rk_rhs;
    RawKind kind = lhs_raw ? kind_lhs : kind_rhs;
    int boxed_rk = lhs_raw ? rk_rhs : rk_lhs;
    if (kind != RAWK_REAL) return false;
    /* A literal boxed operand or (impossible here, defensive) a raw one: simpler to let the fully
       boxed fallback handle it than special-case materializing a literal into a register first. */
    if (boxed_rk & (RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG | RK_CONST_FLAG)) return false;

    int slot = raw_materialize(c, raw_rk, kind);
    if (slot < 0) return false; /* raw-slot budget exhausted: fall back to boxed */

    /* A temp raw_rk can double as dest -- nothing reads it again -- so use the in-place _BOXED
       opcode. A named raw local's slot cannot: mutating it corrupts the local for later use, so
       the non-destructive _TO variant reads it and writes a fresh dest instead. */
    int dest;
    Opcode raw_op;
    if (slot >= P.raw_real_reserved_floor) {
        dest = slot;
        raw_op = (op == OP_ADD) ? OP_RAW_ADD_REAL_BOXED : OP_RAW_MUL_REAL_BOXED;
        chunk_emit(c, PACK3(raw_op, dest, boxed_rk, 0));
    } else {
        dest = raw_real_alloc();
        if (dest < 0) return false;
        raw_op = (op == OP_ADD) ? OP_RAW_ADD_REAL_BOXED_TO : OP_RAW_MUL_REAL_BOXED_TO;
        chunk_emit(c, PACK3(raw_op, dest, slot, boxed_rk));
    }
    /* boxed_rk is confirmed a plain register above (no RAW/CONST flag) -- nothing to materialize.
       Both opcode families promote an integer boxed operand to real (see this function's own
       comment) -- no runtime type restriction beyond what the boxed path itself already allows. */
    if (is_temp(boxed_rk)) reg_free(1);

    *out_rk = RK_RAW_REAL_FLAG | dest;
    return true;
}

/* Raw-vs-boxed ordering comparisons only (vm.h) -- a raw loop counter almost always compares
   against a non-raw bound (a parameter). Requires the raw side to ALREADY be a raw slot, not
   merely raw-composable: a bare literal would re-materialize a fresh OP_RAW_LOAD every
   iteration, paying more than the boxed RK path it replaces. */
static bool try_emit_cmp_raw_boxed(Chunk* c, Opcode op, int rk_lhs, RawKind kind_lhs, int rk_rhs,
                                   RawKind kind_rhs, int* out_rk) {
    bool lhs_raw = (kind_lhs != RAWK_NONE);
    int raw_rk = lhs_raw ? rk_lhs : rk_rhs;
    RawKind kind = lhs_raw ? kind_lhs : kind_rhs;
    int boxed_rk = lhs_raw ? rk_rhs : rk_lhs;

    if (!(raw_rk & (RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG))) return false;
    if (boxed_rk & (RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG)) return false;
    if (!rk8_fits(boxed_rk)) return false;

    /* `boxed OP raw` is `raw (flip) OP boxed` -- LT/GT and LTE/GTE swap. */
    Opcode effective_op = op;
    if (!lhs_raw) {
        switch (op) {
            case OP_LT: effective_op = OP_GT; break;
            case OP_GT: effective_op = OP_LT; break;
            case OP_LTE: effective_op = OP_GTE; break;
            case OP_GTE: effective_op = OP_LTE; break;
            default: return false;
        }
    }

    bool int_kind = (kind == RAWK_INT);
    Opcode raw_op;
    if (int_kind) {
        switch (effective_op) {
            case OP_LT: raw_op = OP_RAW_LT_INT_BOXED; break;
            case OP_GT: raw_op = OP_RAW_GT_INT_BOXED; break;
            case OP_LTE: raw_op = OP_RAW_LTE_INT_BOXED; break;
            case OP_GTE: raw_op = OP_RAW_GTE_INT_BOXED; break;
            default: return false;
        }
    } else {
        switch (effective_op) {
            case OP_LT: raw_op = OP_RAW_LT_REAL_BOXED; break;
            case OP_GT: raw_op = OP_RAW_GT_REAL_BOXED; break;
            case OP_LTE: raw_op = OP_RAW_LTE_REAL_BOXED; break;
            case OP_GTE: raw_op = OP_RAW_GTE_REAL_BOXED; break;
            default: return false;
        }
    }

    int slot = raw_materialize(c, raw_rk, kind);
    if (slot < 0) return false; /* raw-slot budget exhausted: fall back to boxed */

    int floor_now = int_kind ? P.raw_int_reserved_floor : P.raw_real_reserved_floor;
    if (slot >= floor_now) {
        if (int_kind)
            raw_int_free(1);
        else
            raw_real_free(1);
    }
    if (is_temp(boxed_rk)) reg_free(1);

    int dest = reg_alloc();
    P.last_cmp_offset = c->count;
    chunk_emit(c, PACK3(raw_op, dest, slot, pack_rk8(boxed_rk)));
    *out_rk = dest;
    return true;
}

/* Returns false if the operator has no raw-native form or the operand kinds mismatch, and the
   caller falls back to the boxed path. Only ADD/SUB/MUL/DIV/MOD/FLOOR_DIV and the 4 ordering
   comparisons are raw-native -- EQ/NEQ/bitwise/AND/OR/IN always stay boxed, deliberately. */
static bool try_emit_binary_raw(Chunk* c, Opcode op, int rk_lhs, int rk_rhs, int* out_rk) {
    RawKind kind_lhs = rk_raw_kind(c, rk_lhs);
    RawKind kind_rhs = rk_raw_kind(c, rk_rhs);

    /* rk_raw_kind calls a bare literal raw-composable, which is right for arithmetic but wrong for
       a comparison against an already-raw operand: raw-materializing it costs an OP_RAW_LOAD every
       iteration, while the raw-vs-boxed compare takes it as a constant-pool operand for free. */
    if ((op == OP_LT || op == OP_GT || op == OP_LTE || op == OP_GTE) && kind_lhs != RAWK_NONE &&
        kind_lhs == kind_rhs) {
        bool lhs_is_slot = (rk_lhs & (RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG)) != 0;
        bool rhs_is_slot = (rk_rhs & (RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG)) != 0;
        bool lhs_is_const = (rk_lhs & RK_CONST_FLAG) != 0;
        bool rhs_is_const = (rk_rhs & RK_CONST_FLAG) != 0;
        /* Declines only when the constant overruns RK8's 7-bit index, and then the OP_LOADK spill
           puts it back in range for the second attempt below. */
        if (lhs_is_slot && rhs_is_const) {
            if (try_emit_cmp_raw_boxed(c, op, rk_lhs, kind_lhs, rk_rhs, RAWK_NONE, out_rk)) return true;
            rk_rhs = materialize(c, rk_rhs);
            kind_rhs = RAWK_NONE;
        } else if (rhs_is_slot && lhs_is_const) {
            if (try_emit_cmp_raw_boxed(c, op, rk_lhs, RAWK_NONE, rk_rhs, kind_rhs, out_rk)) return true;
            rk_lhs = materialize(c, rk_lhs);
            kind_lhs = RAWK_NONE;
        }
    }

    if ((kind_lhs == RAWK_NONE) != (kind_rhs == RAWK_NONE)) {
        if (try_emit_arith_raw_boxed(c, op, rk_lhs, kind_lhs, rk_rhs, kind_rhs, out_rk)) return true;
        return try_emit_cmp_raw_boxed(c, op, rk_lhs, kind_lhs, rk_rhs, kind_rhs, out_rk);
    }
    if (kind_lhs == RAWK_NONE || kind_rhs == RAWK_NONE || kind_lhs != kind_rhs) return false;
    bool int_kind = (kind_lhs == RAWK_INT);

    Opcode raw_op;
    bool is_cmp = false;
    bool div_int_promotes_to_real =
        false; /* OP_DIV on two ints still yields a real, matching boxed semantics. */
    if (int_kind) {
        switch (op) {
            case OP_ADD: raw_op = OP_RAW_ADD_INT; break;
            case OP_SUB: raw_op = OP_RAW_SUB_INT; break;
            case OP_MUL: raw_op = OP_RAW_MUL_INT; break;
            case OP_DIV:
                raw_op = OP_RAW_DIV_INT;
                div_int_promotes_to_real = true;
                break;
            case OP_MOD: raw_op = OP_RAW_MOD_INT; break;
            case OP_FLOOR_DIV: raw_op = OP_RAW_FLOOR_DIV_INT; break;
            case OP_LT:
                raw_op = OP_RAW_LT_INT;
                is_cmp = true;
                break;
            case OP_GT:
                raw_op = OP_RAW_GT_INT;
                is_cmp = true;
                break;
            case OP_LTE:
                raw_op = OP_RAW_LTE_INT;
                is_cmp = true;
                break;
            case OP_GTE:
                raw_op = OP_RAW_GTE_INT;
                is_cmp = true;
                break;
            default: return false;
        }
    } else {
        switch (op) {
            case OP_ADD: raw_op = OP_RAW_ADD_REAL; break;
            case OP_SUB: raw_op = OP_RAW_SUB_REAL; break;
            case OP_MUL: raw_op = OP_RAW_MUL_REAL; break;
            case OP_DIV: raw_op = OP_RAW_DIV_REAL; break;
            case OP_LT:
                raw_op = OP_RAW_LT_REAL;
                is_cmp = true;
                break;
            case OP_GT:
                raw_op = OP_RAW_GT_REAL;
                is_cmp = true;
                break;
            case OP_LTE:
                raw_op = OP_RAW_LTE_REAL;
                is_cmp = true;
                break;
            case OP_GTE:
                raw_op = OP_RAW_GTE_REAL;
                is_cmp = true;
                break;
            default: return false; /* no raw MOD/FLOOR_DIV for real */
        }
    }

    int slot_lhs = raw_materialize(c, rk_lhs, kind_lhs);
    int slot_rhs = raw_materialize(c, rk_rhs, kind_lhs); /* same kind, confirmed above */
    if (slot_lhs < 0 || slot_rhs < 0) return false; /* raw-slot budget exhausted: fall back to boxed */

    /* Free-then-allocate, RHS then LHS -- only frees a slot that was actually a temp. */
    int floor_now = int_kind ? P.raw_int_reserved_floor : P.raw_real_reserved_floor;
    if (slot_rhs >= floor_now) {
        if (int_kind)
            raw_int_free(1);
        else
            raw_real_free(1);
    }
    if (slot_lhs >= floor_now) {
        if (int_kind)
            raw_int_free(1);
        else
            raw_real_free(1);
    }

    if (is_cmp) {
        int dest = reg_alloc();
        chunk_emit(c, PACK3(raw_op, dest, slot_lhs, slot_rhs));
        *out_rk = dest;
        return true;
    }
    if (div_int_promotes_to_real) {
        int dest = raw_real_alloc();
        if (dest < 0) return false; /* extremely unlikely right after freeing 2 int slots, but stay safe */
        chunk_emit(c, PACK3(raw_op, dest, slot_lhs, slot_rhs));
        *out_rk = RK_RAW_REAL_FLAG | dest;
        return true;
    }
    int dest = int_kind ? raw_int_alloc() : raw_real_alloc();
    if (dest < 0) return false;
    chunk_emit(c, PACK3(raw_op, dest, slot_lhs, slot_rhs));
    *out_rk = (int_kind ? RK_RAW_INT_FLAG : RK_RAW_REAL_FLAG) | dest;
    return true;
}

/* Reads Chunk.functions directly -- the registry chunk_add_function already maintains is the
   single source of truth; the parser keeps no parallel copy. */
static bool func_lookup(Chunk* c, unsigned int name_idx, unsigned int* out_offset,
                        unsigned int* out_func_index) {
    ChunkFunction* f = chunk_find_function_by_name_idx(c, name_idx);
    if (!f) return false;
    *out_offset = f->code_offset;
    *out_func_index = (unsigned int)(f - c->functions);
    return true;
}

/* A bare-name value reference or an omitted-defaults call needs the full signature to build a
   real AerFunction, not just the offset. */
static bool func_full_lookup(Chunk* c, unsigned int name_idx, unsigned int* out_offset,
                             unsigned int* out_arity, unsigned int* out_min_arity, AerVal** out_defaults,
                             unsigned int* out_max_registers, unsigned int* out_max_raw_ints,
                             unsigned int* out_max_raw_reals, unsigned int* out_func_index) {
    ChunkFunction* f = chunk_find_function_by_name_idx(c, name_idx);
    if (!f) return false;
    *out_offset = f->code_offset;
    *out_arity = f->arity;
    *out_min_arity = f->min_arity;
    *out_defaults = f->defaults;
    *out_max_registers = f->max_registers;
    *out_max_raw_ints = f->max_raw_ints;
    *out_max_raw_reals = f->max_raw_reals;
    *out_func_index = (unsigned int)(f - c->functions);
    return true;
}

/* Builds a real runtime AerFunction, reusing the existing constructor. `defaults` is used
   as-is, not copied. */
static AerVal build_function_value(unsigned int func_offset, unsigned int arity, unsigned int min_arity,
                                   AerVal* defaults, unsigned int max_registers, unsigned int max_raw_ints,
                                   unsigned int max_raw_reals) {
    AerFunction* fn = vm_new_function();
    fn->code_offset = func_offset;
    fn->arity = (uint16_t)arity;
    fn->min_arity = (uint16_t)min_arity;
    fn->defaults = defaults;
    fn->max_registers = max_registers;
    fn->max_raw_ints = max_raw_ints;
    fn->max_raw_reals = max_raw_reals;
    return aer_function_val(fn);
}

/* Passed in explicitly, not read internally -- by the time this is called the caller has
   already consumed the argument list, so "now" would point past the actual name. */
static void pending_call_add(unsigned int name_idx, unsigned int patch_offset, const char* call_site_cursor) {
    if (P.pending_count >= P.pending_cap) {
        P.pending_cap = P.pending_cap ? P.pending_cap * 2 : 8;
        P.pending_calls = xrealloc(P.pending_calls, sizeof(PendingCall) * (size_t)P.pending_cap);
    }
    P.pending_calls[P.pending_count].name_idx = name_idx;
    P.pending_calls[P.pending_count].patch_offset = patch_offset;
    P.pending_calls[P.pending_count].call_site_cursor = call_site_cursor;
    P.pending_count++;
}

/* `defaults` is taken by ownership, never copied. */
static void func_register(Chunk* c, unsigned int name_idx, unsigned int offset, unsigned int arity,
                          unsigned int min_arity, AerVal* defaults) {
    chunk_add_function(c, name_idx, offset, arity, min_arity, defaults);
    unsigned int new_func_index = c->function_count - 1;

    /* Swap-remove each match (order doesn't matter), leaving only genuinely unresolved entries.
       Patches both the callee_offset word (emit_call's returned patch_offset) and the func_index
       word immediately after it (emit_call always emits them back-to-back) -- a forward-referenced
       call can't know its target's function index at emission time any more than it can know its
       code offset. */
    for (int i = 0; i < P.pending_count;) {
        if (P.pending_calls[i].name_idx == name_idx) {
            patch_jump(c, P.pending_calls[i].patch_offset, offset);
            c->code[P.pending_calls[i].patch_offset + 1] = new_func_index;
            P.pending_calls[i] = P.pending_calls[--P.pending_count];
        } else {
            i++;
        }
    }
}

static bool loop_push(unsigned int top) {
    if (P.loop_depth >= LOOP_MAX) {
        error_at("Too many nested loops (max %d)", LOOP_MAX);
        return false;
    }
    P.loop_stack[P.loop_depth].top = top;
    P.loop_stack[P.loop_depth].patch_count = 0;
    P.loop_stack[P.loop_depth].rotated = false;
    P.loop_depth++;
    return true;
}

/* Rotated range-for only -- `top` is never read here (parse_continue defers instead). */
static bool loop_push_rotated(void) {
    if (P.loop_depth >= LOOP_MAX) {
        error_at("Too many nested loops (max %d)", LOOP_MAX);
        return false;
    }
    P.loop_stack[P.loop_depth].patch_count = 0;
    P.loop_stack[P.loop_depth].rotated = true;
    P.loop_stack[P.loop_depth].continue_patch_count = 0;
    P.loop_depth++;
    return true;
}

/* Patches every pending break to exit_target, then pops the loop context. */
static void loop_pop_and_patch(Chunk* c, unsigned int exit_target) {
    LoopContext* ctx = &P.loop_stack[P.loop_depth - 1];
    for (int i = 0; i < ctx->patch_count; i++)
        patch_jump(c, ctx->patches[i], exit_target);
    P.loop_depth--;
}

/* Rotated range-for only -- same as loop_pop_and_patch, plus patches every deferred `continue` to
   land at continue_target (OP_ITER_RANGE_LOOP's own position, not the loop body's start -- continue
   still needs to run the advance-and-check, not just re-enter the body from the top). */
static void loop_pop_and_patch_rotated(Chunk* c, unsigned int exit_target, unsigned int continue_target) {
    LoopContext* ctx = &P.loop_stack[P.loop_depth - 1];
    for (int i = 0; i < ctx->patch_count; i++)
        patch_jump(c, ctx->patches[i], exit_target);
    for (int i = 0; i < ctx->continue_patch_count; i++)
        patch_jump(c, ctx->continue_patches[i], continue_target);
    P.loop_depth--;
}

/* A struct type isn't a callable offset -- OP_STRUCT_NEW just needs to know `Name(...)` means
   construct. Parse-time-only, can't live on the Chunk. */

/* Max targets in a destructuring assignment (`a, b, c = ...`). */
#define MAX_DESTRUCT 16

static bool is_struct_name(unsigned int name_idx) {
    for (int i = 0; i < P.struct_count; i++)
        if (P.struct_names[i] == name_idx) return true;
    return false;
}

static void struct_register(unsigned int name_idx) {
    if (P.struct_count >= P.struct_cap) {
        P.struct_cap = P.struct_cap ? P.struct_cap * 2 : 16;
        P.struct_names = xrealloc(P.struct_names, sizeof(unsigned int) * (size_t)P.struct_cap);
    }
    P.struct_names[P.struct_count++] = name_idx;
}

/* Forces rk into the current watermark -- OP_CALL's args must be contiguous. A fresh temp
   already sits there (this file's free-then-allocate discipline), so it's reused directly
   instead of allocating past it and leaving a gap that breaks contiguity for a later argument. */
static int arg_materialize(Chunk* c, int rk) {
    rk = box_if_raw(c, rk);
    if (!(rk & RK_CONST_FLAG) && is_temp(rk) && rk == P.next_temp_register - 1) {
        return rk;
    }
    int target = reg_alloc();
    if (rk & RK_CONST_FLAG) {
        chunk_emit(c, PACK_OP_A_W16(OP_LOADK, target, (unsigned int)(rk & ~RK_CONST_FLAG)));
    } else {
        chunk_emit(c, PACK2(OP_MOVE, target, rk));
    }
    return target;
}

/* Shared bulk-copy prep for calls/arrays/dicts -- all three opcodes need contiguous operand
   registers. Returns the count; *out_base unspecified when count==0. */
static int parse_contiguous_exprs(Chunk* c, TokenType close_tok, int* out_base) {
    int base = -1;
    int count = 0;
    if (!equal(close_tok)) {
        do {
            int rk = parse_binary(c, 0);
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
                case 'n': out[o++] = '\n'; break;
                case 't': out[o++] = '\t'; break;
                case 'r': out[o++] = '\r'; break;
                case '\\': out[o++] = '\\'; break;
                case '"': out[o++] = '"'; break;
                /* `\{` unescapes to a bare `{`, same as `\"` unescapes to a bare quote. */
                case '{': out[o++] = '{'; break;
                default:
                    out[o++] = '\\';
                    out[o++] = s[i];
                    break;
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
    AerVal sv = aer_make_string_copy(buf, out);
    return chunk_add_pool(c, sv);
}

/* `and`/`or`/`as`/`|>` are handled specially elsewhere (short-circuit jumps, a bare type name,
   call-argument prepending) -- returns false for anything outside this table. `not` isn't in this
   table at all: it's a prefix parsed by parse_not, recursing into parse_binary at precedence 2
   (and's own slot) so it binds tighter than and/or but looser than everything below. */
static bool binary_op_info(TokenType t, unsigned int* prec, Opcode* op) {
    switch (t) {
        case TOKEN_OR:
            *prec = 1;
            *op = OP_OR;
            return true;
        case TOKEN_PIPE:
            *prec = 1;
            *op = OP_PIPE;
            return true;
        case TOKEN_AND:
            *prec = 2;
            *op = OP_AND;
            return true;
        case TOKEN_BITWISE_OR:
            *prec = 3;
            *op = OP_BITWISE_OR;
            return true;
        case TOKEN_BITWISE_XOR:
            *prec = 4;
            *op = OP_BITWISE_XOR;
            return true;
        case TOKEN_BITWISE_AND:
            *prec = 5;
            *op = OP_BITWISE_AND;
            return true;
        case TOKEN_EQUAL:
            *prec = 6;
            *op = OP_EQ;
            return true;
        case TOKEN_NOT_EQUAL:
            *prec = 6;
            *op = OP_NEQ;
            return true;
        case TOKEN_LESS:
            *prec = 7;
            *op = OP_LT;
            return true;
        case TOKEN_GREATER:
            *prec = 7;
            *op = OP_GT;
            return true;
        case TOKEN_LESS_EQUAL:
            *prec = 7;
            *op = OP_LTE;
            return true;
        case TOKEN_GREATER_EQUAL:
            *prec = 7;
            *op = OP_GTE;
            return true;
        case TOKEN_IN:
            *prec = 7;
            *op = OP_IN;
            return true;
        case TOKEN_LEFT_SHIFT:
            *prec = 8;
            *op = OP_LSHIFT;
            return true;
        case TOKEN_RIGHT_SHIFT:
            *prec = 8;
            *op = OP_RSHIFT;
            return true;
        case TOKEN_ADD:
            *prec = 9;
            *op = OP_ADD;
            return true;
        case TOKEN_SUBTRACT:
            *prec = 9;
            *op = OP_SUB;
            return true;
        case TOKEN_MULTIPLY:
            *prec = 10;
            *op = OP_MUL;
            return true;
        case TOKEN_DIVIDE:
            *prec = 10;
            *op = OP_DIV;
            return true;
        case TOKEN_MODULO:
            *prec = 10;
            *op = OP_MOD;
            return true;
        case TOKEN_FLOOR_DIVIDE:
            *prec = 10;
            *op = OP_FLOOR_DIV;
            return true;
        /* `as` no longer exists at all -- primitive casts are integer(x)/float(x)/boolean(x)/
           string(x) now, struct shape-checks are type(x) == "Name" (both replacing what `x as T`
           used to emit), and import aliasing is the positional `import alias "path"` (replacing
           `import "path" as alias`). TOKEN_AS itself has been removed from the lexer. */
        default: return false;
    }
}

/* Sub-parses `{expr}` as a genuine expression (calls, arithmetic, indexing, field access), in
   its own lexer span so it can run mid-scan of the outer string literal. The whole span must be
   consumed by exactly one expression. */
static int parse_interpolated_expr(Chunk* c, const char* text, unsigned int len) {
    char* decoded = xmalloc((size_t)len + 1);
    unsigned int decoded_len = decode_string_escapes(text, len, decoded);

    /* Captured on the OUTER file before switching into the span -- current_source_line() would
       otherwise report a line relative to the span's own start once inside it. Harmless in
       practice today (chunk_mark_line is only called at statement boundaries, never mid-expression,
       so this value is never actually read), but keeping this call site honest rather than leaving
       it silently correct by coincidence. */
    unsigned int outer_line = current_source_line();
    LexerState* saved = lexer_save_state();
    lexer_begin_span(decoded, decoded_len, outer_line);
    free(decoded); /* lexer_begin_span copies it into its own owned buffer */
    lex();
    int rk = parse_binary(c, 0);
    if (!parse_had_error && !equal(TOKEN_END_OF_FILE)) error_at("Unexpected token in string interpolation");
    lexer_restore_state(saved);
    return rk;
}

/* Literal segments and interpolated values concatenate via OP_ADD; interpolation's
   value-to-string step goes through OP_UNARY's folded-in OP_TO_STR. */
/* One OP_INTERP over `parts`. The parts' temps are released before the destination is claimed so
   it reuses the lowest of them, the same trick the concatenate chain used; safe because the opcode
   reads every part before writing its destination. */
static int emit_interp(Chunk* c, const int* parts, int part_count) {
    int temps = 0;
    for (int i = 0; i < part_count; i++)
        if (is_temp(parts[i])) temps++;
    if (temps) reg_free(temps);
    int dest = reg_alloc();
    chunk_emit(c, PACK2(OP_INTERP, dest, part_count));
    for (int i = 0; i < part_count; i++)
        chunk_emit(c, pack_rk16(parts[i]));
    return dest;
}

static int parse_string_literal(Chunk* c) {
    AerString* ts = aer_as_string(token.value);
    char* s = ts->data;
    unsigned int len = ts->length;

    bool has_interp = false;
    for (unsigned int k = 0; k < len; k++) {
        if (s[k] == '\\' && k + 1 < len) {
            k++;
            continue;
        }
        if (s[k] == '{') {
            has_interp = true;
            break;
        }
    }

    if (!has_interp) {
        unsigned int pool_idx = pool_escaped_string(c, s, len);
        lex();
        return (int)pool_idx | RK_CONST_FLAG;
    }

    /* Parts are collected as RK values and emitted as one OP_INTERP at the end -- one dispatch and
       one allocation instead of a concatenate per part, each of which allocated a string that was
       garbage by the next one. Beyond INTERP_MAX_PARTS the old left-fold chain still runs, so there
       is no limit on how long an interpolation may be. */
    int parts[INTERP_MAX_PARTS];
    int part_count = 0;

    int result = -1; /* -1: no parts concatenated yet (a valid RK/register value is always >= 0) */
    unsigned int i = 0;
    while (i <= len) {
        /* Literal segment up to the next unescaped '{' or end. */
        unsigned int seg_start = i;
        while (i < len) {
            if (s[i] == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (s[i] == '{') break;
            i++;
        }
        if (i > seg_start) {
            int rk_seg = (int)pool_escaped_string(c, s + seg_start, i - seg_start) | RK_CONST_FLAG;
            /* A constant segment stays an RK constant -- OP_INTERP reads it straight from the pool,
               so unlike the concatenate chain it never needs materializing into a register. */
            if (part_count == INTERP_MAX_PARTS) {
                int folded = emit_interp(c, parts, part_count);
                part_count = 0;
                parts[part_count++] = folded;
            }
            parts[part_count++] = rk_seg;
        }
        if (i >= len) break;

        /* Depth-aware so a nested '{'/'}'  (a dict literal) doesn't end the scan early. */
        i++; /* skip '{' */
        unsigned int expr_start = i;
        int depth = 1;
        while (i < len && depth > 0) {
            if (s[i] == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (s[i] == '{') {
                depth++;
                i++;
                continue;
            }
            if (s[i] == '}') {
                depth--;
                if (depth == 0) break;
                i++;
                continue;
            }
            i++;
        }
        if (depth != 0) {
            error_at("Unclosed '{' in string");
            break;
        }
        if (i == expr_start) {
            error_at("Empty '{}' in string");
            i++;
            continue;
        }

        unsigned int expr_len = i - expr_start;
        int rk_expr = parse_interpolated_expr(c, s + expr_start, expr_len);
        if (parse_had_error) {
            i++;
            continue;
        }
        /* No OP_TO_STR: OP_INTERP formats an int/real/bool/null part directly into the result, so
           the throwaway string that step used to allocate never exists. */
        if (part_count == INTERP_MAX_PARTS) {
            int folded = emit_interp(c, parts, part_count);
            part_count = 0;
            parts[part_count++] = folded;
        }
        parts[part_count++] = rk16_fits(rk_expr) ? rk_expr : materialize(c, rk_expr);
        i++; /* skip '}' */
    }

    /* A string with no interpolation at all already returned above, so a single part here is a
       single interpolated expression -- which still has to be converted, whatever its type. */
    if (part_count > 0) {
        result = emit_interp(c, parts, part_count);
    } else {
        result = (int)chunk_add_pool(c, aer_make_string_copy("", 0)) | RK_CONST_FLAG;
    }

    lex();
    return result;
}

/* Unary operators sit above this in the precedence chain (parse_unary). */
/* Shared by integer(x)/float(x)/boolean(x)/string(x), replacing what `x as T` used to emit directly
   (`as` has been removed from the language entirely). Identical codegen either way: box a
   raw-tracked lhs first (no raw-native cast form exists), then OP_CAST (integer/float/boolean) or
   OP_UNARY/OP_TO_STR (string, cast_type < 0 --
   there's no CAST_STRING id since OP_TO_STR already existed as its own opcode before casting did). */
static int emit_primitive_cast(Chunk* c, int cast_type, int lhs) {
    lhs = box_if_raw(c, lhs);
    if (is_temp(lhs)) reg_free(1);
    int dest = reg_alloc();
    bool spilled = false;
    if (!rk8_fits(lhs)) {
        lhs = materialize(c, lhs);
        spilled = true;
    }
    if (cast_type < 0)
        chunk_emit(c, PACK3(OP_UNARY, dest, OP_TO_STR, pack_rk8(lhs)));
    else
        chunk_emit(c, PACK3(OP_CAST, dest, cast_type, pack_rk8(lhs)));
    if (spilled) reg_free(1);
    return dest;
}

/* integer(x)/float(x)/boolean(x) -- TOKEN_TYPE_INTEGER/FLOAT/BOOLEAN are reserved tokens, never
   identifiers, so they can't go through parse_call's name-based resolution at all; this is the
   direct equivalent for a primary expression starting with one of them followed by '('. Exactly one
   argument, enforced by the grammar itself (parse_binary parses one expression, not a list -- a
   second argument or zero arguments both fall through to a natural "expected ')'"/"expected an
   expression" error with no extra arity-checking code needed). */
static int parse_primitive_cast_call(Chunk* c, int cast_type) {
    lex(); /* consume the type token itself */
    require(TOKEN_OPEN_PARENTHESE, "expected '(' after type name");
    int lhs = parse_binary(c, 0);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after cast argument");
    if (parse_had_error) return 0;
    return emit_primitive_cast(c, cast_type, lhs);
}

static int parse_primary_inner(Chunk* c) {
    if (consume(TOKEN_FUNCTION)) return parse_function_expr(c);
    /* These 3 reserved tokens have no other valid meaning in primary-expression position now that
       'as' is gone -- parse_primitive_cast_call's own require(TOKEN_OPEN_PARENTHESE, ...) produces a
       clear error if '(' doesn't follow, so no 2-token lookahead is needed here. */
    if (equal(TOKEN_TYPE_INTEGER)) return parse_primitive_cast_call(c, CAST_INTEGER);
    if (equal(TOKEN_TYPE_FLOAT)) return parse_primitive_cast_call(c, CAST_FLOAT);
    if (equal(TOKEN_TYPE_BOOLEAN)) return parse_primitive_cast_call(c, CAST_BOOLEAN);
    if (consume(TOKEN_OPEN_PARENTHESE)) {
        int rk = parse_binary(c, 0);
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after expression");
        return rk;
    }
    /* `[]`/`[a, b, c]` (ordinary array literal, sharing call arguments' contiguous-materialization
       helper, then OP_ARRAY_NEW) vs. `[value; count]` (the repeat-literal -- replaces the old
       `Type[count]` entirely, see OP_ARRAY_REPEAT's own comment, vm.h). The two can't be told apart
       until the first element is already parsed (nothing before it distinguishes them), so the
       first element is always parsed as an ordinary expression first, then the next token decides
       which construct this actually is. */
    if (consume(TOKEN_OPEN_BRACKET)) {
        if (consume(TOKEN_CLOSE_BRACKET)) {
            int dest = reg_alloc();
            emit_array_new(c, dest, dest, 0);
            return dest;
        }
        /* A suffix is parse-time-only information, so narrow storage must be decided from the token
           before parse_binary consumes it. "Not wrapped in anything else" is confirmed by the first
           element emitting zero opcodes -- a lone literal folds into a constant operand. */
        bool narrow_int_candidate = (token.type == TOKEN_INTEGER && token.narrow);
        bool narrow_float_candidate = (token.type == TOKEN_REAL && token.narrow);
        unsigned int emit_count_before = c->count;
        int rk_first = parse_binary(c, 0);
        if (parse_had_error) return 0;

        if (consume(TOKEN_SEMICOLON)) {
            int narrow_flag = 0;
            if (c->count == emit_count_before) {
                if (narrow_int_candidate)
                    narrow_flag = 1;
                else if (narrow_float_candidate)
                    narrow_flag = 2;
            }
            int fill_reg = arg_materialize(c, rk_first);
            int rk_count = parse_binary(c, 0);
            require(TOKEN_CLOSE_BRACKET, "expected ']' after repeat-literal count");
            if (parse_had_error) return 0;
            rk_count = box_if_raw(c, rk_count);
            if (!rk16_fits(rk_count)) {
                error_at("Expression too large to compile (register/constant exceeds the repeat-literal "
                         "count encoding's range)");
                return 0;
            }
            /* Strict LIFO free order -- rk_count was allocated (if a temp at all) after fill_reg. */
            if (is_temp(rk_count)) reg_free(1);
            if (is_temp(fill_reg)) reg_free(1);
            int dest = reg_alloc();
            emit_array_repeat(c, dest, fill_reg, narrow_flag, rk_count);
            return dest;
        }

        int item_reg_base = arg_materialize(c, rk_first);
        int item_count = 1;
        while (consume(TOKEN_COMMA)) {
            int rk = parse_binary(c, 0);
            arg_materialize(c, rk);
            item_count++;
        }
        require(TOKEN_CLOSE_BRACKET, "expected ']' after array literal");
        if (parse_had_error) return 0;
        if (item_count > 1) reg_free(item_count - 1);
        emit_array_new(c, item_reg_base, item_reg_base, item_count);
        return item_reg_base;
    }
    /* Key and value are collected as RK values and emitted as OP_DICT_NEW's trailing words, so a
       literal one costs nothing to place. Temps are released before the destination is claimed so
       it reuses the lowest of them -- safe because the opcode reads every operand before writing
       its destination, the same ordering emit_interp relies on. */
    if (consume(TOKEN_OPEN_BRACE)) {
        int pair_rks[2 * DICT_MAX_LITERAL_PAIRS];
        int pair_count = 0;
        if (!equal(TOKEN_CLOSE_BRACE)) {
            do {
                if (pair_count == DICT_MAX_LITERAL_PAIRS) {
                    error_at("Dict literal has too many entries");
                    return 0;
                }
                int rk_key = box_if_raw(c, parse_binary(c, 0));
                require(TOKEN_COLON, "expected ':' after dict key");
                if (parse_had_error) return 0;
                int rk_val = box_if_raw(c, parse_binary(c, 0));
                if (!rk16_fits(rk_key) || !rk16_fits(rk_val)) {
                    error_at("Dict literal entry too large to compile (register/constant index "
                             "exceeds the dict-new encoding's range)");
                    return 0;
                }
                pair_rks[2 * pair_count] = rk_key;
                pair_rks[2 * pair_count + 1] = rk_val;
                pair_count++;
            } while (consume(TOKEN_COMMA));
        }
        require(TOKEN_CLOSE_BRACE, "expected '}' after dict literal");
        if (parse_had_error) return 0;
        int temps = 0;
        for (int i = 0; i < 2 * pair_count; i++)
            if (is_temp(pair_rks[i])) temps++;
        if (temps) reg_free(temps);
        int dest = reg_alloc();
        emit_dict_new(c, dest, pair_rks, pair_count);
        return dest;
    }
    if (token.type == TOKEN_INTEGER || token.type == TOKEN_REAL || token.type == TOKEN_TRUE ||
        token.type == TOKEN_FALSE || token.type == TOKEN_NULL) {
        AerVal v;
        if (token.type == TOKEN_INTEGER)
            v = aer_int(aer_as_int(token.value));
        else if (token.type == TOKEN_REAL)
            v = aer_real(aer_as_real(token.value));
        else if (token.type == TOKEN_NULL)
            v = aer_null();
        else
            v = aer_bool(aer_as_bool(token.value));
        lex();
        return (int)chunk_add_pool(c, v) | RK_CONST_FLAG;
    }
    if (token.type == TOKEN_STRING) return parse_string_literal(c);
    if (at_module_name(c)) return parse_module_call(c);
    if (token.type == TOKEN_IDENTIFIER) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_OPEN_PARENTHESE)) return parse_call(c, name_idx);

        /* A raw-tracked name returns an RK_RAW_*_FLAG-tagged operand, letting a bare reference compose
           through further arithmetic without boxing. */
        int reg;
        if (var_lookup_rk(name_idx, &reg)) return reg;

        /* A known function referenced without a following '(' is a reference to the function itself
           as a value -- built once per reference as a deduped pool constant. */
        unsigned int func_offset, func_arity, func_min_arity, func_max_registers, func_max_raw_ints,
            func_max_raw_reals;
        unsigned int
            func_index_unused; /* AerFunction carries its own peaks directly -- no func_index needed for OP_CALL_VALUE */
        AerVal* func_defaults;
        if (func_full_lookup(c, name_idx, &func_offset, &func_arity, &func_min_arity, &func_defaults,
                             &func_max_registers, &func_max_raw_ints, &func_max_raw_reals,
                             &func_index_unused)) {
            AerVal fv = build_function_value(func_offset, func_arity, func_min_arity, func_defaults,
                                             func_max_registers, func_max_raw_ints, func_max_raw_reals);
            return (int)chunk_add_pool(c, fv) | RK_CONST_FLAG;
        }

        /* Must not fall through to var_slot -- that would read an uninitialized register. */
        if (report_if_shadowed_global(c, name_idx)) return 0;
        error_at("'%s' is not defined", aer_as_string(c->pool[name_idx])->data);
        return 0;
    }
    error_at("Expected an expression (only literals, variables, calls, array/dict literals, "
             "arithmetic/comparisons, and parentheses are supported)");
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
            /* rk_start feeds pack_rk16 below, a 16-bit slot with no raw state -- a raw-flagged index
               would corrupt the encoding, and rk16_fits doesn't mask the flag bits either. */
            rk_start = box_if_raw(c, rk_start);

            if (!consume(TOKEN_COLON)) {
                require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
                if (parse_had_error) return rk;

                int arr_reg = materialize(c, rk);

                /* Fused since a packed array has no standalone `expr[index]` value. Emitted for EVERY
                   `[index].field` read, dispatching at runtime on packed vs ordinary. */
                if (equal(TOKEN_DOT)) {
                    lex();
                    if (!equal(TOKEN_IDENTIFIER)) {
                        error_at("Expected field name after '.'");
                        return rk;
                    }
                    unsigned int field_idx = chunk_add_pool(c, token.value);
                    lex();

                    if (is_temp(rk_start)) reg_free(1);
                    if (is_temp(arr_reg)) reg_free(1);

                    if (!rk16_fits(rk_start)) {
                        error_at("Expression too large to compile (register/constant index exceeds the fused "
                                 "index-field-op encoding's range)");
                        return rk;
                    }
                    mark_shape_sensitive(arr_reg);
                    /* arr_reg's shape is a compile-time fact here, so resolve the field directly and
                       route the result through a raw slot, composing via the same raw machinery a
                       raw local already uses. Falls back to the generic opcode if the field isn't
                       int/real or the raw-slot budget is exhausted. */
                    Shape* known =
                        (arr_reg >= 0 && arr_reg < FRAME_REGISTERS) ? P.reg_known_shape[arr_reg] : NULL;
                    unsigned int foffset;
                    ValueType ftype;
                    bool narrow;
                    if (known && shape_find_field(known, field_idx, &foffset, &ftype, &narrow) &&
                        (ftype == TYPE_INTEGER || ftype == TYPE_REAL)) {
                        bool is_int = (ftype == TYPE_INTEGER);
                        int slot = is_int ? raw_int_alloc() : raw_real_alloc();
                        if (slot >= 0) {
                            /* arr_reg == P.hint_param_reg checked explicitly here (not inside
                               index_safe_unchecked, which typed-array callers also use with no such
                               requirement) -- the field OFFSET this opcode trusts is only valid for
                               this one specialized parameter, never any other array a loop might
                               ALSO have proven a safe index for. */
                            bool unchecked =
                                arr_reg == P.hint_param_reg && index_safe_unchecked(arr_reg, rk_start);
                            Opcode op = narrow
                                            ? (unchecked ? (is_int ? OP_INDEX_FIELD_GET_RAW_INT32_UNCHECKED
                                                                   : OP_INDEX_FIELD_GET_RAW_FLOAT32_UNCHECKED)
                                                         : (is_int ? OP_INDEX_FIELD_GET_RAW_INT32
                                                                   : OP_INDEX_FIELD_GET_RAW_FLOAT32))
                                            : (unchecked ? (is_int ? OP_INDEX_FIELD_GET_RAW_INT_UNCHECKED
                                                                   : OP_INDEX_FIELD_GET_RAW_REAL_UNCHECKED)
                                                         : (is_int ? OP_INDEX_FIELD_GET_RAW_INT
                                                                   : OP_INDEX_FIELD_GET_RAW_REAL));
                            chunk_emit(c, PACK3(op, slot, arr_reg, 0));
                            chunk_emit(c, PACK_2X16((uint16_t)foffset, pack_rk16(rk_start)));
                            rk = is_int ? (RK_RAW_INT_FLAG | slot) : (RK_RAW_REAL_FLAG | slot);
                            continue;
                        }
                    }
                    int dest = reg_alloc();
                    chunk_emit(c, PACK3(OP_INDEX_FIELD_GET, dest, arr_reg, 0));
                    chunk_emit(c, PACK_2X16(field_idx, pack_rk16(rk_start)));
                    rk = dest;
                    continue;
                }

                /* Free-then-allocate, matching parse_binary_ops's own discipline. */
                if (is_temp(rk_start)) reg_free(1);
                if (is_temp(arr_reg)) reg_free(1);

                int dest = reg_alloc();
                /* rk_start is guaranteed plain and non-raw here, so it always fits RK8 directly.
                   This is a loop-safety proof only, not a container-type one -- the opcode still
                   checks TYPE_TYPED_ARRAY itself. */
                if (index_safe_unchecked(arr_reg, rk_start)) {
                    chunk_emit(c, PACK3(OP_TYPED_INDEX_GET_UNCHECKED, dest, arr_reg, pack_rk8(rk_start)));
                } else {
                    emit_index_get(c, dest, arr_reg, rk_start);
                }
                /* One-hop alias tracking for SPEC_KIND_ARRAY_OF_STRUCTS. Recorded unconditionally:
                   last_plain_index_src_param needs only arr_reg's identity, available during the
                   ordinary compile too. last_plain_index_known_elem_shape stays NULL outside a
                   specialization recompile, since reg_known_element_shape is only seeded there. */
                P.last_plain_index_dest_reg = dest;
                P.last_plain_index_src_param =
                    (arr_reg >= 0 && arr_reg < P.current_param_count) ? arr_reg : -1;
                P.last_plain_index_known_elem_shape =
                    (arr_reg >= 0 && arr_reg < FRAME_REGISTERS) ? P.reg_known_element_shape[arr_reg] : NULL;
                rk = dest;
                continue;
            }

            int rk_slice_start = has_start ? rk_start : ((int)chunk_add_pool(c, aer_null()) | RK_CONST_FLAG);
            int rk_end;
            if (equal(TOKEN_CLOSE_BRACKET))
                rk_end = (int)chunk_add_pool(c, aer_null()) | RK_CONST_FLAG;
            else
                rk_end = parse_binary(c, 0);
            require(TOKEN_CLOSE_BRACKET, "expected ']' after slice");
            if (parse_had_error) return rk;

            int arr_reg = materialize(c, rk);

            if (is_temp(rk_end)) reg_free(1);
            if (is_temp(rk_slice_start)) reg_free(1);
            if (is_temp(arr_reg)) reg_free(1);

            int dest = reg_alloc();
            emit_slice_get(c, dest, arr_reg, rk_slice_start, rk_end);
            rk = dest;
            continue;
        }
        /* Same shape as '[...]' above -- chained forms (p.pos.x) work since each step feeds the next. */
        if (consume(TOKEN_DOT)) {
            if (!equal(TOKEN_IDENTIFIER)) {
                error_at("Expected field name after '.'");
                return rk;
            }
            unsigned int field_idx = chunk_add_pool(c, token.value);
            lex();

            int struct_reg = materialize(c, rk);
            if (is_temp(struct_reg)) reg_free(1);

            mark_shape_sensitive(struct_reg);
            {
                Shape* known =
                    (struct_reg >= 0 && struct_reg < FRAME_REGISTERS) ? P.reg_known_shape[struct_reg] : NULL;
                unsigned int foffset;
                ValueType ftype;
                bool narrow;
                if (known && shape_find_field(known, field_idx, &foffset, &ftype, &narrow) &&
                    (ftype == TYPE_INTEGER || ftype == TYPE_REAL)) {
                    bool is_int = (ftype == TYPE_INTEGER);
                    int slot = is_int ? raw_int_alloc() : raw_real_alloc();
                    if (slot >= 0) {
                        Opcode op = narrow ? (is_int ? OP_FIELD_GET_RAW_INT32 : OP_FIELD_GET_RAW_FLOAT32)
                                           : (is_int ? OP_FIELD_GET_RAW_INT : OP_FIELD_GET_RAW_REAL);
                        chunk_emit(c, PACK3(op, slot, struct_reg, 0));
                        chunk_emit(c, foffset);
                        rk = is_int ? (RK_RAW_INT_FLAG | slot) : (RK_RAW_REAL_FLAG | slot);
                        continue;
                    }
                }
            }
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
    bool spilled = false;
    if (!rk8_fits(rk)) {
        rk = materialize(c, rk);
        spilled = true;
    }
    chunk_emit(c, PACK3(OP_UNARY, dest, OP_NOT, pack_rk8(rk)));
    if (spilled) reg_free(1);
    return dest;
}

/* Recurses into parse_unary so chained unary (`~~x`) works. rk is RK-encoded like any binary operand. */
static int parse_unary_inner(Chunk* c) {
    if (consume(TOKEN_NOT)) return parse_not(c);

    Opcode unary_op;
    if (consume(TOKEN_BITWISE_NOT))
        unary_op = OP_BITWISE_NOT;
    else if (consume(TOKEN_SUBTRACT))
        unary_op = OP_NEGATE;
    else
        return parse_primary(c);

    int rk = parse_unary(c);
    rk = box_if_raw(
        c, rk); /* No raw-native unary form -- box first, or a raw slot index gets misread as a register. */
    if (is_temp(rk)) reg_free(1); /* free-then-allocate, matching every other site */
    int dest = reg_alloc();
    bool spilled = false;
    if (!rk8_fits(rk)) {
        rk = materialize(c, rk);
        spilled = true;
    }
    chunk_emit(c, PACK3(OP_UNARY, dest, unary_op, pack_rk8(rk)));
    if (spilled) reg_free(1);
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
    unsigned int patch_skip = emit_jump_if_false_reg(c, dest); /* lhs falsy -- dest already holds it */

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
    unsigned int patch_end = c->count;
    chunk_emit(c, 0);

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
    reg_free(1); /* Freed before the next pipe-call argument, restoring P.next_temp_register to where
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
    if (!equal(TOKEN_IDENTIFIER)) {
        error_at("Expected a function name after '|>'");
        return lhs;
    }

    /* Same "piped value becomes argument zero" idea, routed through OP_CALL_MODULE. Checked via
       chunk_is_imported before consuming the identifier. */
    if (chunk_is_imported(c, aer_as_string(token.value)->data, aer_as_string(token.value)->length)) {
        int module_id = module_call_id(aer_as_string(token.value));
        unsigned int module_idx = chunk_add_pool(c, token.value);
        lex();
        require(TOKEN_DOT, "expected '.' after module name");
        if (parse_had_error) return lhs;
        if (!equal(TOKEN_IDENTIFIER)) {
            error_at("Expected a function name after '.'");
            return lhs;
        }
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
        chunk_emit(c, PACK3(OP_CALL_MODULE, dest, arg_reg_base, arg_count));
        chunk_emit(c, (uint32_t)module_idx);
        chunk_emit(c, (uint32_t)fn_idx);
        chunk_emit(c, PACK_2X16((uint16_t)module_id, (uint16_t)fn_id));
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
    unsigned int func_offset = 0, func_index = 0;
    if (!is_struct && !func_lookup(c, name_idx, &func_offset, &func_index)) {
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
        emit_call(c, dest, func_offset, arg_reg_base, arg_count, func_index);
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

        if (op == OP_AND) {
            lhs = compile_and(c, lhs, prec);
            lhs_start = c->count;
            continue;
        }
        if (op == OP_OR) {
            lhs = compile_or(c, lhs, prec);
            lhs_start = c->count;
            continue;
        }
        if (op == OP_PIPE) {
            lhs = compile_pipe(c, lhs);
            lhs_start = c->count;
            continue;
        }

        /* Checked before parsing the RHS, while lhs's bytecode is still the tail of the chunk --
           excising bytecode from the middle would risk invalidating an RHS jump target.
           OP_FIELD_GET is now a fixed 2-word shape (word0: op+dest+struct_reg, word1: field_idx). */
        bool lhs_is_field = (c->count - lhs_start == 2 && (c->code[lhs_start] & 0xFF) == OP_FIELD_GET);
        int lhs_struct_reg = 0;
        unsigned int lhs_field_idx = 0;
        if (lhs_is_field) {
            lhs_struct_reg = (int)UNPACK_B(c->code[lhs_start]);
            lhs_field_idx = c->code[lhs_start + 1];
            c->count = lhs_start; /* discard lhs's OP_FIELD_GET, never executed */
        }

        /* Fuses `(A op1 B) op2 C` into one typed-array pass, checked like lhs_is_field above --
           against just-emitted code, before the outer RHS is parsed. Both operators must be
           fusable (see vm.c's op_chain2_index) and the inner operands plain registers; anything
           else takes the ordinary unfused path. */
        bool lhs_is_chain2 = false;
        int lhs_chain2_a = 0, lhs_chain2_b = 0;
        Opcode lhs_chain2_op1 = OP_ADD;
        if (!lhs_is_field && c->count - lhs_start == 1) {
            uint32_t w = c->code[lhs_start];
            Opcode wop = (Opcode)(w & 0xFF);
            if ((wop == OP_ADD || wop == OP_SUB || wop == OP_MUL) && (int)UNPACK_A(w) == lhs &&
                (op == OP_ADD || op == OP_SUB || op == OP_MUL)) {
                uint8_t a8 = (uint8_t)UNPACK_B(w), b8 = (uint8_t)UNPACK_C(w);
                if (!RK8_IS_CONST(a8) && !RK8_IS_CONST(b8)) {
                    lhs_is_chain2 = true;
                    lhs_chain2_op1 = wop;
                    lhs_chain2_a = RK8_INDEX(a8);
                    lhs_chain2_b = RK8_INDEX(b8);
                    c->count = lhs_start; /* discard the inner op, never executed as its own instruction */
                }
            }
        }

        unsigned int rhs_start = c->count;
        int rhs = parse_binary(c, prec); /* same precedence as floor -> left-associative */

        if (lhs_is_field) {
            /* lhs is always the OP_FIELD_GET result (never raw); rhs could be raw -- box it. */
            rhs = box_if_raw(c, rhs);
            if (is_temp(rhs)) reg_free(1);
            if (is_temp(lhs)) reg_free(1);

            int dest = reg_alloc();
            if (!rk16_fits(rhs)) {
                error_at("Expression too large to compile (register/constant index exceeds the fused "
                         "field-op encoding's range)");
                return lhs;
            }
            chunk_emit(c, PACK3(OP_FIELD_BINARY, dest, lhs_struct_reg, op));
            chunk_emit(c, PACK_2X16(lhs_field_idx, pack_rk16(rhs)));
            lhs = dest;
            lhs_start = c->count;
            continue;
        }

        if (lhs_is_chain2) {
            /* C must be a plain register too -- the opcode reads registers[c_reg] directly, no RK
               decode. materialize handles both "was raw" and "was a bare constant" in one call. */
            int c_reg = materialize(c, rhs);
            if (is_temp(c_reg)) reg_free(1);
            int dest = reg_alloc();
            chunk_emit(c, PACK3(OP_TYPED_ARRAY_CHAIN2, dest, lhs_chain2_a, lhs_chain2_b));
            chunk_emit(c, PACK_2X16((uint16_t)lhs_chain2_op1, (uint16_t)c_reg));
            chunk_emit(c, (uint32_t)op);
            lhs = dest;
            lhs_start = c->count;
            continue;
        }

        /* Generalizes the compound-assignment FMA/FMS fusion to any plain expression whose running
           sum is a fresh raw-real temporary. Safe only for a temporary (slot >=
           raw_real_reserved_floor): a named variable's slot must not be overwritten in place, since
           unlike `x += a*b` a plain expression's lhs may still be read with its original value.
           Requires the RHS to be exactly one raw MUL; anything else falls through. Real only. */
        if (!lhs_is_field && !lhs_is_chain2 && (op == OP_ADD || op == OP_SUB) &&
            rk_raw_kind(c, lhs) == RAWK_REAL && (lhs & RK_RAW_SLOT_MASK) >= P.raw_real_reserved_floor &&
            c->count - rhs_start == 1) {
            uint32_t mw = c->code[rhs_start];
            if ((Opcode)(mw & 0xFF) == OP_RAW_MUL_REAL && rk_raw_kind(c, rhs) == RAWK_REAL &&
                (int)UNPACK_A(mw) == (rhs & RK_RAW_SLOT_MASK)) {
                int lhs_slot = lhs & RK_RAW_SLOT_MASK;
                int mul_a = (int)UNPACK_B(mw), mul_b = (int)UNPACK_C(mw);
                c->count = rhs_start; /* discard the MUL -- fused below instead */
                if ((int)UNPACK_A(mw) >= P.raw_real_reserved_floor) raw_real_free(1);
                Opcode fused = (op == OP_ADD) ? OP_RAW_FMA_REAL : OP_RAW_FMS_REAL;
                chunk_emit(c, PACK3(fused, lhs_slot, mul_a, mul_b));
                lhs = RK_RAW_REAL_FLAG | lhs_slot;
                lhs_start = c->count;
                continue;
            }
        }

        /* Fuses `x OP y.field`, recognized by the RHS being exactly one bare field read. Reuses
           OP_FIELD_BINARY (`field OP' x`) rather than a mirror opcode: the two agree whenever op is
           commutative or is a comparison whose operands can be swapped (`x < f` == `f > x`).
           SUB/DIV/MOD/shifts/IN have no such equivalent and fall through unfused. */
        if (c->count - rhs_start == 2 && (c->code[rhs_start] & 0xFF) == OP_FIELD_GET) {
            Opcode commuted_op;
            bool commutable = true;
            switch (op) {
                case OP_ADD:
                case OP_MUL:
                case OP_EQ:
                case OP_NEQ:
                case OP_BITWISE_AND:
                case OP_BITWISE_OR:
                case OP_BITWISE_XOR: commuted_op = op; break;
                case OP_LT: commuted_op = OP_GT; break;
                case OP_GT: commuted_op = OP_LT; break;
                case OP_LTE: commuted_op = OP_GTE; break;
                case OP_GTE: commuted_op = OP_LTE; break;
                default:
                    commutable = false;
                    commuted_op = op;
                    break; /* SUB/DIV/MOD/FLOOR_DIV/LSHIFT/RSHIFT/IN -- order-sensitive, no fusion */
            }
            if (commutable) {
                int struct_reg = (int)UNPACK_B(c->code[rhs_start]);
                unsigned int field_idx = c->code[rhs_start + 1];
                c->count = rhs_start; /* discard the OP_FIELD_GET just emitted, never executed */

                /* rhs is always the OP_FIELD_GET result (never raw); lhs could be raw -- box it. */
                lhs = box_if_raw(c, lhs);
                if (is_temp(rhs)) reg_free(1);
                if (is_temp(lhs)) reg_free(1);

                int dest = reg_alloc();
                if (!rk16_fits(lhs)) {
                    error_at("Expression too large to compile (register/constant index exceeds the fused "
                             "field-op encoding's range)");
                    return lhs;
                }
                chunk_emit(c, PACK3(OP_FIELD_BINARY, dest, struct_reg, commuted_op));
                chunk_emit(c, PACK_2X16(field_idx, pack_rk16(lhs)));
                lhs = dest;
                lhs_start = c->count;
                continue;
            }
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

/* The 6 arithmetic compound-assign operators (bitwise OP= forms were deliberately dropped -- a
   second spelling with no new capability). `x OP= expr` compiles to one binary opcode with dest ==
   lhs == x's own register. */
static const struct {
    TokenType tok;
    Opcode op;
} compound_assign_ops[] = {
    {TOKEN_ADD_ASSIGN, OP_ADD},    {TOKEN_SUBTRACT_ASSIGN, OP_SUB}, {TOKEN_MULTIPLY_ASSIGN, OP_MUL},
    {TOKEN_DIVIDE_ASSIGN, OP_DIV}, {TOKEN_MODULO_ASSIGN, OP_MOD},   {TOKEN_FLOOR_DIVIDE_ASSIGN, OP_FLOOR_DIV},
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
            if (!equal(TOKEN_IDENTIFIER)) {
                error_at("Expected identifier in destructuring");
                return;
            }
            if (count >= MAX_DESTRUCT) {
                error_at("Too many destructuring targets (max %d)", MAX_DESTRUCT);
                return;
            }
            names[count++] = chunk_add_pool(c, token.value);
            lex();
        }
        /* A destructuring target can never re-derive the length_tracked_name invariant (its value
           comes from array-index unpacking, never a fresh length() call) -- invalidate eagerly. */
        if (P.length_tracked_valid) {
            for (unsigned int i = 0; i < count; i++) {
                if (names[i] == P.length_tracked_name) {
                    P.length_tracked_valid = false;
                    break;
                }
            }
        }
        require(TOKEN_ASSIGN, "expected '=' after destructuring targets");
        if (parse_had_error) return;

        int target_regs[MAX_DESTRUCT];
        for (unsigned int i = 0; i < count; i++) {
            ensure_boxed(
                c,
                names
                    [i]); /* A destructuring target is always a plain boxed write -- an existing raw name must shadow to
                                             boxed before var_slot looks it up. */
            target_regs[i] = var_slot(c, names[i]);
            if (target_regs[i] < 0) return; /* error_at already called */
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

        /* Destructuring can rebind a shape-tracked or safe-loop-index register, so clear all four
           per-register facts for every target. Timed after the RHS is parsed so a legitimate read
           of a target's old value on the RHS still gets the fast path. */
        for (unsigned int i = 0; i < count; i++) {
            P.reg_known_shape[target_regs[i]] = NULL;
            P.reg_known_element_shape[target_regs[i]] = NULL;
            P.alias_source_param[target_regs[i]] = -1;
            invalidate_register(target_regs[i]);
        }

        int arr_reg = rhs_reg_base;
        if (rhs_count > 1) {
            reg_free((int)rhs_count - 1);
            emit_array_new(c, arr_reg, rhs_reg_base, (int)rhs_count);
        }

        /* 2 targets, 1 source: the (value, err) convention, tolerant of a bare non-Result value. */
        if (count == 2 && rhs_count == 1) {
            chunk_emit(c, PACK3(OP_DESTRUCTURE, target_regs[0], target_regs[1], arr_reg));
        } else {
            for (unsigned int i = 0; i < count; i++) {
                unsigned int pool_i = chunk_add_pool(c, aer_int((int64_t)i));
                emit_index_get(c, target_regs[i], arr_reg, (int)pool_i | RK_CONST_FLAG);
            }
        }
        reg_free(1); /* arr_reg -- always a temp, guaranteed by arg_materialize */
        return;
    }

    if (consume(TOKEN_ASSIGN)) {
        /* Watches for a self-reference during the RHS parse -- see self_ref_watch_name's own
           comment (top of file) for why and what consumes self_ref_watch_seen just below. */
        P.self_ref_watch_name = name_idx;
        P.self_ref_watch_seen = false;
        int rk_val = parse_binary(c, 0);
        if (parse_had_error) return;

        /* Tracks/invalidates length_tracked_name -- see P.last_length_call_result_reg's own
           comment. Equality against rk_val (the FINAL, fully-parsed RHS) rather than clearing this
           at every possible intervening op site: any further operation applied on top of the
           length() call allocates its own new register, so `x = length(p) + 1` naturally fails
           this check (rk_val is the ADD's dest, not length()'s), exactly as `x = length(p)` alone
           naturally passes it -- same self-correcting equality trick as last_plain_index_dest_reg. */
        if (P.last_length_call_result_reg >= 0 && rk_val == P.last_length_call_result_reg) {
            P.length_tracked_name = name_idx;
            P.length_tracked_valid = true;
            P.length_tracked_source_reg = P.last_length_call_arg_reg;
        } else if (P.length_tracked_valid && name_idx == P.length_tracked_name) {
            P.length_tracked_valid = false;
        }

        /* Looked up after parsing the RHS -- a self-referential first assignment creates the name as
           a side effect of parsing it, always boxed, so checking existence now naturally folds
           that case into the ordinary path. */
        int existing_idx = -1;
        for (int i = 0; i < P.var_count; i++)
            if (P.var_names[i] == name_idx) {
                existing_idx = i;
                break;
            }

        if (existing_idx < 0) {
            /* Checked before the raw-eligible fast path could register the name directly, bypassing
               var_slot's own identical check. */
            if (P.function_depth > 0) {
                for (int i = 0; i < P.global_count; i++) {
                    if (P.global_names[i] == name_idx) {
                        error_at("'%s' is a top-level variable — not accessible inside a function; pass it "
                                 "as a parameter (or rename)",
                                 aer_as_string(c->pool[name_idx])->data);
                        return;
                    }
                }
            }
            /* Raw storage iff outside any if/else branch and the RHS is provably int/real -- not
               gated on function_depth, so top-level qualifies. Safe there because frame 0's
               raw_ints/raw_reals are linked at full capacity for the VM's life and
               aer_vm_reset_for_reuse never touches them, exactly as boxed top-level registers
               already rely on. global_regs[] only compares register numbers, never dereferences. */
            RawKind rhs_kind = rk_raw_kind(c, rk_val);
            if (P.branch_depth == 0 && rhs_kind != RAWK_NONE) {
                int slot = (rhs_kind == RAWK_INT) ? raw_int_reserve_one() : raw_real_reserve_one();
                if (slot >= 0) {
                    int src_slot = raw_materialize(c, rk_val, rhs_kind);
                    if (src_slot < 0) {
                        /* Budget exhausted mid-materialize -- release the reservation, fall through to boxed. */
                        if (rhs_kind == RAWK_INT)
                            raw_int_free(1);
                        else
                            raw_real_free(1);
                    } else {
                        if (src_slot != slot) {
                            /* Direct analog of the boxed path's "reg != rk_val -> MOVE" case. */
                            Opcode move_op = (rhs_kind == RAWK_INT) ? OP_RAW_MOVE_INT : OP_RAW_MOVE_REAL;
                            chunk_emit(c, PACK3(move_op, slot, src_slot, 0));
                            int floor_now =
                                (rhs_kind == RAWK_INT) ? P.raw_int_reserved_floor : P.raw_real_reserved_floor;
                            if (src_slot >= floor_now) {
                                if (rhs_kind == RAWK_INT)
                                    raw_int_free(1);
                                else
                                    raw_real_free(1);
                            }
                        }
                        P.var_names[P.var_count] = name_idx;
                        P.var_regs[P.var_count] = slot;
                        P.var_kind[P.var_count] = (rhs_kind == RAWK_INT) ? VAR_RAW_INT : VAR_RAW_REAL;
                        P.var_count++;
                        /* Mirrors var_slot's own registration, which this path bypasses. Without it a
                           top-level int/real is invisible to the shadow ban, so a function could
                           silently declare a local of the same name -- the exact behaviour the ban
                           exists to reject, and which still errored for a top-level string or array. */
                        if (P.function_depth == 0 && P.global_count < FRAME_REGISTERS) {
                            P.global_names[P.global_count] = name_idx;
                            P.global_regs[P.global_count] = slot;
                            P.global_count++;
                        }
                        return;
                    }
                }
            }
        } else if (P.var_kind[existing_idx] != VAR_BOXED) {
            /* Stays raw (writes in place, no shadow) whenever the new value is the same raw kind --
               the slot's identity never changes, so there's no phi/merge ambiguity to avoid.
               Otherwise shadows to a fresh boxed register -- var_slot must never be called here,
               since it would return the stale raw slot index as if it were a plain register. */
            RawKind rhs_kind = rk_raw_kind(c, rk_val);
            VarKind cur = P.var_kind[existing_idx];
            bool same_kind = (cur == VAR_RAW_INT && rhs_kind == RAWK_INT) ||
                             (cur == VAR_RAW_REAL && rhs_kind == RAWK_REAL);
            if (same_kind) {
                int dest_slot = P.var_regs[existing_idx];
                int src_slot = raw_materialize(c, rk_val, rhs_kind);
                if (src_slot >= 0) {
                    if (src_slot != dest_slot) {
                        Opcode move_op = (rhs_kind == RAWK_INT) ? OP_RAW_MOVE_INT : OP_RAW_MOVE_REAL;
                        chunk_emit(c, PACK3(move_op, dest_slot, src_slot, 0));
                        int floor_now =
                            (rhs_kind == RAWK_INT) ? P.raw_int_reserved_floor : P.raw_real_reserved_floor;
                        if (src_slot >= floor_now) {
                            if (rhs_kind == RAWK_INT)
                                raw_int_free(1);
                            else
                                raw_real_free(1);
                        }
                    }
                    return;
                }
                /* Budget exhausted materializing the RHS -- fall through to the shadow path. */
            }
            /* Mirrors var_slot's register-claiming but rebinds in place. A looped self-referential
               shadow (`total = total + x`) must refuse to compile: the RHS reading the old raw value
               was already emitted and would re-read a slot nothing writes anymore. Gated on
               self_ref_watch_seen rather than loop_depth, since a reassignment that doesn't read the
               old value has nothing stale to re-read. */
            if (P.loop_depth > 0 && P.self_ref_watch_seen) {
                error_at("This assignment would change '%s' from a fixed numeric type to a different type, "
                         "but it's inside a loop — not supported. If you're accumulating with +, -, or *, "
                         "use the compound form ('%s += ...' etc.) instead — it doesn't have this "
                         "restriction. Otherwise, restructure so the type change happens outside any loop.",
                         aer_as_string(c->pool[name_idx])->data, aer_as_string(c->pool[name_idx])->data);
                return;
            }
            rk_val = box_if_raw(c, rk_val);
            if (P.reserved_floor >= FRAME_REGISTERS) {
                error_at("Too many variables (max %d)", FRAME_REGISTERS);
                return;
            }
            int new_reg = P.reserved_floor;
            P.reserved_floor++;
            P.next_temp_register = P.reserved_floor;
            P.var_regs[existing_idx] = new_reg;
            P.var_kind[existing_idx] = VAR_BOXED;
            /* No P.global_regs update needed -- see ensure_boxed's identical reasoning: this path
               only runs on a currently-raw-tracked name, which can never be in P.global_names. */
            if (rk_val & RK_CONST_FLAG) {
                chunk_emit(c, PACK_OP_A_W16(OP_LOADK, new_reg, (unsigned int)(rk_val & ~RK_CONST_FLAG)));
            } else if (new_reg != rk_val) {
                chunk_emit(c, PACK2(OP_MOVE, new_reg, rk_val));
                if (is_temp(rk_val)) reg_free(1);
            }
            return;
        }

        /* Unchanged existing behavior. rk_val is boxed first in case it's raw-flagged. */
        rk_val = box_if_raw(c, rk_val);
        int reg = var_slot(c, name_idx);
        if (reg < 0) return; /* error_at already called */
        /* See invalidate_safe_loop_reg's own comment -- without this, `reg` staying on
           safe_loop_item_regs after this reassignment would let a later arr[reg].field inside the
           same loop body keep trusting an index register that may no longer hold what the loop's
           own PREP/LOOP put there. */
        invalidate_register(reg);
        /* Cleared unconditionally before either branch: only parse_function_body's initial seed
           deserves a surviving "my elements are this shape" fact. Without this, reassigning a
           specialized array-of-structs parameter and indexing through a fresh alias resolves the
           stale shape's field offset -- reachable type confusion. */
        P.reg_known_element_shape[reg] = NULL;
        /* var_slot returns the same register for an already-declared name, so a reassigned
           shape-sensitive parameter would keep a hint that may no longer hold. Clear it by default
           -- always safe -- unless the RHS was exactly `some_param[idx]`, the one-hop alias pattern
           SPEC_KIND_ARRAY_OF_STRUCTS needs, where the parameter's element shape propagates onto the
           alias instead. Consumed immediately so a freed-and-reused temp can't match later. */
        if (rk_val == P.last_plain_index_dest_reg && P.last_plain_index_dest_reg >= 0) {
            P.reg_known_shape[reg] = P.last_plain_index_known_elem_shape;
            P.alias_source_param[reg] = P.last_plain_index_src_param;
        } else {
            P.reg_known_shape[reg] = NULL;
            P.alias_source_param[reg] = -1;
        }
        P.last_plain_index_dest_reg = -1;
        if (rk_val & RK_CONST_FLAG) {
            chunk_emit(c, PACK_OP_A_W16(OP_LOADK, reg, (unsigned int)(rk_val & ~RK_CONST_FLAG)));
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

        /* A compound assignment can never re-derive length_tracked_name's invariant (the RHS is
           combined with the OLD value, never a fresh length() call alone) -- invalidate eagerly. */
        if (P.length_tracked_valid && name_idx == P.length_tracked_name) P.length_tracked_valid = false;

        /* A raw-tracked compound-assignment target needs its own path -- var_lookup would return a
           bare register-shaped int with no distinguishing flag (real bug: silently misread as a
           plain register). Stays raw for +=/-=/x= with a same-kind RHS; /= always shadows (int/int
           division promotes to real). */
        int existing_idx = -1;
        for (int j = 0; j < P.var_count; j++)
            if (P.var_names[j] == name_idx) {
                existing_idx = j;
                break;
            }

        if (existing_idx >= 0 && P.var_kind[existing_idx] != VAR_BOXED) {
            RawKind cur_kind = (P.var_kind[existing_idx] == VAR_RAW_INT) ? RAWK_INT : RAWK_REAL;
            Opcode boxed_op = compound_assign_ops[i].op;
            bool native_op_exists = (boxed_op == OP_ADD || boxed_op == OP_SUB || boxed_op == OP_MUL);

            unsigned int rhs_start = c->count;
            int rk_rhs = parse_binary(c, 0);
            if (parse_had_error) return;
            RawKind rhs_kind = rk_raw_kind(c, rk_rhs);

            /* Fuses the RHS's just-emitted OP_RAW_MUL_REAL with this ADD/SUB into one FMA/FMS
               dispatch. Only when the RHS was exactly one raw MUL writing the slot rk_rhs points
               at; anything else falls through unfused. Real only. */
            if (cur_kind == RAWK_REAL && (boxed_op == OP_ADD || boxed_op == OP_SUB) &&
                rhs_kind == RAWK_REAL && c->count - rhs_start == 1) {
                uint32_t mw = c->code[rhs_start];
                if ((Opcode)(mw & 0xFF) == OP_RAW_MUL_REAL &&
                    (int)UNPACK_A(mw) == (rk_rhs & RK_RAW_SLOT_MASK)) {
                    int dest_slot = P.var_regs[existing_idx];
                    int mul_a = (int)UNPACK_B(mw), mul_b = (int)UNPACK_C(mw);
                    c->count = rhs_start; /* discard the MUL -- fused below instead */
                    if ((int)UNPACK_A(mw) >= P.raw_real_reserved_floor) raw_real_free(1);
                    Opcode fused = (boxed_op == OP_ADD) ? OP_RAW_FMA_REAL : OP_RAW_FMS_REAL;
                    chunk_emit(c, PACK3(fused, dest_slot, mul_a, mul_b));
                    return;
                }
            }

            if (native_op_exists && rhs_kind == cur_kind) {
                int dest_slot = P.var_regs[existing_idx];
                int rhs_slot = raw_materialize(c, rk_rhs, rhs_kind);
                if (rhs_slot >= 0) {
                    Opcode raw_op;
                    if (cur_kind == RAWK_INT)
                        raw_op = (boxed_op == OP_ADD)   ? OP_RAW_ADD_INT
                                 : (boxed_op == OP_SUB) ? OP_RAW_SUB_INT
                                                        : OP_RAW_MUL_INT;
                    else
                        raw_op = (boxed_op == OP_ADD)   ? OP_RAW_ADD_REAL
                                 : (boxed_op == OP_SUB) ? OP_RAW_SUB_REAL
                                                        : OP_RAW_MUL_REAL;
                    chunk_emit(c, PACK3(raw_op, dest_slot, dest_slot, rhs_slot));
                    int floor_now =
                        (cur_kind == RAWK_INT) ? P.raw_int_reserved_floor : P.raw_real_reserved_floor;
                    if (rhs_slot >= floor_now) {
                        if (cur_kind == RAWK_INT)
                            raw_int_free(1);
                        else
                            raw_real_free(1);
                    }
                    return;
                }
                /* raw-slot budget exhausted materializing the RHS: fall through to the shadow
                   path below instead of leaving the variable half-updated. */
            }

            /* An ordinary boxed RHS (e.g. nbody.aer's `e += 0.5 * bim * (...)`) accumulates directly into
               the existing raw slot with a runtime tag check instead of shadowing -- no shadow, no
               allocation, safe every loop iteration. A provably-mismatched kind still shadows. */
            if (native_op_exists && rhs_kind == RAWK_NONE) {
                int dest_slot = P.var_regs[existing_idx];
                int boxed_reg = materialize(c, rk_rhs);
                Opcode raw_op;
                if (cur_kind == RAWK_INT)
                    raw_op = (boxed_op == OP_ADD)   ? OP_RAW_ADD_INT_BOXED
                             : (boxed_op == OP_SUB) ? OP_RAW_SUB_INT_BOXED
                                                    : OP_RAW_MUL_INT_BOXED;
                else
                    raw_op = (boxed_op == OP_ADD)   ? OP_RAW_ADD_REAL_BOXED
                             : (boxed_op == OP_SUB) ? OP_RAW_SUB_REAL_BOXED
                                                    : OP_RAW_MUL_REAL_BOXED;
                chunk_emit(c, PACK3(raw_op, dest_slot, boxed_reg, 0));
                if (is_temp(boxed_reg)) reg_free(1);
                return;
            }

            /* Boxes the current raw value, then performs the compound op -- unlike plain assignment's
               shadow, this genuinely depends on old_slot's value, so a loop re-executing it would
               re-read the stale value every iteration (real bug found this way). No single-pass
               fix exists, so refuse to compile rather than silently corrupt. */
            if (P.loop_depth > 0) {
                error_at("This compound assignment would change '%s' from a fixed numeric type to a "
                         "different type, but it's inside a loop — not supported (restructure so the type "
                         "change happens outside any loop)",
                         aer_as_string(c->pool[name_idx])->data);
                return;
            }
            int old_slot = P.var_regs[existing_idx];
            Opcode box_op = (cur_kind == RAWK_INT) ? OP_BOX_INT : OP_BOX_REAL;
            if (P.reserved_floor >= FRAME_REGISTERS) {
                error_at("Too many variables (max %d)", FRAME_REGISTERS);
                return;
            }
            int new_reg = P.reserved_floor;
            P.reserved_floor++;
            P.next_temp_register = P.reserved_floor;
            chunk_emit(c, PACK3(box_op, new_reg, old_slot, 0));
            P.var_regs[existing_idx] = new_reg;
            P.var_kind[existing_idx] = VAR_BOXED;
            /* No P.global_regs update needed -- see ensure_boxed's identical reasoning. */
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
            if (P.function_depth > 0 && global_lookup(name_idx, &dummy_reg)) {
                error_at("'%s' is a top-level variable — not accessible inside a function; pass it as a "
                         "parameter (or rename)",
                         aer_as_string(c->pool[name_idx])->data);
                return;
            }
            error_at("Compound assignment target must already have a value (no assigning to an undefined "
                     "name this way)");
            return;
        }

        int rk_rhs = parse_binary(c, 0);
        if (parse_had_error) return;
        /* Same P.reg_known_shape invalidation as the plain-assignment tail above -- `reg`'s value
           is about to change (e.g. `bodies += extra_bodies`), so any shape hint on it is no longer
           trustworthy. alias_source_param and reg_known_element_shape cleared alongside it now too
           (earlier omissions here -- the plain-assignment tail clears all three); see
           invalidate_register's own comment for why safe_loop_item_regs needs the same treatment. */
        P.reg_known_shape[reg] = NULL;
        P.reg_known_element_shape[reg] = NULL;
        P.alias_source_param[reg] = -1;
        invalidate_register(reg);
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
            if (P.function_depth > 0 && global_lookup(name_idx, &dummy_reg)) {
                error_at("'%s' is a top-level variable — not accessible inside a function; pass it as a "
                         "parameter (or rename)",
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
    bool obj_is_base; /* true while obj_reg is still name_idx's own permanent register */
    if (var_lookup(name_idx, &obj_reg)) {
        obj_is_base = true;
    } else if (P.function_depth > 0 && global_lookup(name_idx, &obj_reg)) {
        /* `name` isn't a local of the current function but IS an existing top-level global --
           same shadow-ban error var_slot enforces for a bare reference. */
        error_at("'%s' is a top-level variable — not accessible inside a function; pass it as a parameter "
                 "(or rename)",
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
        /* Boxed unconditionally: the fused opcodes pack the index into a 16-bit RK slot with no raw
           state, and rk16_fits/rk8_fits mask only RK_CONST_FLAG, so a raw-flagged index corrupts the
           encoding (surfacing as a bogus "expression too large"). No-op for the general path. */
        pending_rk_idx = box_if_raw(c, pending_rk_idx);
        require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
    } else {
        if (!equal(TOKEN_IDENTIFIER)) {
            error_at("Expected field name after '.'");
            return;
        }
        pending_field_idx = chunk_add_pool(c, token.value);
        lex();
    }
    if (parse_had_error) return;

    /* Fuses into OP_INDEX_FIELD_GET/SET for the same reason the read side does -- a packed array
       has no standalone `name[index]` value. Falls through to the general path for any further
       chaining. */
    if (first_is_index && equal(TOKEN_DOT)) {
        lex();
        if (!equal(TOKEN_IDENTIFIER)) {
            error_at("Expected field name after '.'");
            return;
        }
        unsigned int fused_field_idx = chunk_add_pool(c, token.value);
        lex();

        bool no_more_chaining = !equal(TOKEN_OPEN_BRACKET) && !equal(TOKEN_DOT);
        bool is_plain_assign = equal(TOKEN_ASSIGN);
        int compound_i = -1;
        if (!is_plain_assign) {
            for (int ci = 0; ci < COMPOUND_ASSIGN_OP_COUNT; ci++) {
                if (equal(compound_assign_ops[ci].tok)) {
                    compound_i = ci;
                    break;
                }
            }
        }

        if (no_more_chaining && (is_plain_assign || compound_i >= 0)) {
            if (!rk16_fits(pending_rk_idx)) {
                error_at("Expression too large to compile (register/constant index exceeds the fused "
                         "index-field-op encoding's range)");
                return;
            }
            /* obj_reg is still obj's own register here (obj_is_base) -- this is the very first
               postfix step on the name, no chaining happened before it. */
            mark_shape_sensitive(obj_reg);
            Shape* known = (obj_reg >= 0 && obj_reg < FRAME_REGISTERS) ? P.reg_known_shape[obj_reg] : NULL;
            unsigned int foffset = 0;
            ValueType ftype = TYPE_ANY;
            bool field_narrow_bit = false;
            RawKind field_kind = RAWK_NONE;
            if (known && shape_find_field(known, fused_field_idx, &foffset, &ftype, &field_narrow_bit)) {
                if (ftype == TYPE_INTEGER)
                    field_kind = RAWK_INT;
                else if (ftype == TYPE_REAL)
                    field_kind = RAWK_REAL;
            }
            if (is_plain_assign) {
                lex();
                int rk_val = parse_binary(c, 0);
                if (parse_had_error) return;
                /* Specialized path: the field's byte offset/type is a compile-time-known fact
                   here (see P.reg_known_shape's own comment) -- write straight into it from a raw
                   slot, no boxing, when rk_val is already the matching raw kind (an already-raw
                   value, or a matching literal -- raw_materialize's own contract). Falls back to
                   the generic (boxing) path otherwise, same graceful-overflow convention raw
                   locals already use. */
                if (field_kind != RAWK_NONE && rk_raw_kind(c, rk_val) == field_kind) {
                    int slot = raw_materialize(c, rk_val, field_kind);
                    if (slot >= 0) {
                        /* obj_reg == P.hint_param_reg checked explicitly (see the GET site's
                           identical comment above) -- the field OFFSET these opcodes trust is only
                           valid for this one specialized parameter. */
                        bool unchecked =
                            obj_reg == P.hint_param_reg && index_safe_unchecked(obj_reg, pending_rk_idx);
                        Opcode op =
                            field_narrow_bit
                                ? (unchecked
                                       ? ((field_kind == RAWK_INT) ? OP_INDEX_FIELD_SET_RAW_INT32_UNCHECKED
                                                                   : OP_INDEX_FIELD_SET_RAW_FLOAT32_UNCHECKED)
                                       : ((field_kind == RAWK_INT) ? OP_INDEX_FIELD_SET_RAW_INT32
                                                                   : OP_INDEX_FIELD_SET_RAW_FLOAT32))
                                : (unchecked
                                       ? ((field_kind == RAWK_INT) ? OP_INDEX_FIELD_SET_RAW_INT_UNCHECKED
                                                                   : OP_INDEX_FIELD_SET_RAW_REAL_UNCHECKED)
                                       : ((field_kind == RAWK_INT) ? OP_INDEX_FIELD_SET_RAW_INT
                                                                   : OP_INDEX_FIELD_SET_RAW_REAL));
                        chunk_emit(c, PACK_OP_A_W16(op, obj_reg, pack_rk16(pending_rk_idx)));
                        chunk_emit(c, PACK_2X16((uint16_t)foffset, (uint16_t)slot));
                        int floor_now =
                            (field_kind == RAWK_INT) ? P.raw_int_reserved_floor : P.raw_real_reserved_floor;
                        if (slot >= floor_now) {
                            if (field_kind == RAWK_INT)
                                raw_int_free(1);
                            else
                                raw_real_free(1);
                        }
                        if (is_temp(pending_rk_idx)) reg_free(1);
                        if (!obj_is_base) reg_free(1);
                        return;
                    }
                }
                rk_val = box_if_raw(c, rk_val);
                if (!rk16_fits(rk_val)) {
                    error_at("Expression too large to compile (value exceeds the fused index-field-set "
                             "encoding's range)");
                    return;
                }
                chunk_emit(c, PACK_OP_A_W16(OP_INDEX_FIELD_SET, obj_reg, pack_rk16(pending_rk_idx)));
                chunk_emit(c, PACK_2X16(fused_field_idx, pack_rk16(rk_val)));
                if (is_temp(rk_val)) reg_free(1);
            } else {
                lex();
                int rk_rhs = parse_binary(c, 0);
                if (parse_had_error) return;
                Opcode bin_op = compound_assign_ops[compound_i].op;
                /* Decomposing into raw GET + arithmetic + SET measured worse whenever the rhs needed
                   boxing -- the generic opcode already does it all in one dispatch. When the rhs is
                   already raw there is no boxing to avoid, so the RAW variant is pure upside. */
                bool native_op_exists = (bin_op == OP_ADD || bin_op == OP_SUB || bin_op == OP_MUL);
                if (native_op_exists && field_kind != RAWK_NONE && rk_raw_kind(c, rk_rhs) == field_kind) {
                    int slot = raw_materialize(c, rk_rhs, field_kind);
                    if (slot >= 0) {
                        /* obj_reg == P.hint_param_reg checked explicitly (see the GET site's
                           identical comment above) -- the field OFFSET these opcodes trust is only
                           valid for this one specialized parameter. */
                        bool unchecked =
                            obj_reg == P.hint_param_reg && index_safe_unchecked(obj_reg, pending_rk_idx);
                        Opcode op =
                            field_narrow_bit
                                ? (unchecked
                                       ? ((field_kind == RAWK_INT)
                                              ? OP_INDEX_FIELD_COMPOUND_RAW_INT32_UNCHECKED
                                              : OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED)
                                       : ((field_kind == RAWK_INT) ? OP_INDEX_FIELD_COMPOUND_RAW_INT32
                                                                   : OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32))
                                : (unchecked ? ((field_kind == RAWK_INT)
                                                    ? OP_INDEX_FIELD_COMPOUND_RAW_INT_UNCHECKED
                                                    : OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED)
                                             : ((field_kind == RAWK_INT) ? OP_INDEX_FIELD_COMPOUND_RAW_INT
                                                                         : OP_INDEX_FIELD_COMPOUND_RAW_REAL));
                        chunk_emit(c, PACK3(op, obj_reg, bin_op, 0));
                        chunk_emit(c, PACK_2X16((uint16_t)foffset, pack_rk16(pending_rk_idx)));
                        chunk_emit(c, (uint32_t)slot);
                        int floor_now =
                            (field_kind == RAWK_INT) ? P.raw_int_reserved_floor : P.raw_real_reserved_floor;
                        if (slot >= floor_now) {
                            if (field_kind == RAWK_INT)
                                raw_int_free(1);
                            else
                                raw_real_free(1);
                        }
                        if (is_temp(pending_rk_idx)) reg_free(1);
                        if (!obj_is_base) reg_free(1);
                        return;
                    }
                }
                rk_rhs = box_if_raw(c, rk_rhs);
                if (!rk16_fits(rk_rhs)) {
                    error_at("Expression too large to compile (value exceeds the fused index-field-compound "
                             "encoding's range)");
                    return;
                }
                chunk_emit(c, PACK3(OP_INDEX_FIELD_COMPOUND, obj_reg, bin_op, 0));
                chunk_emit(c, PACK_2X16(fused_field_idx, pack_rk16(pending_rk_idx)));
                chunk_emit(c, PACK_2X16(0, pack_rk16(rk_rhs)));
                if (is_temp(rk_rhs)) reg_free(1);
            }
            if (is_temp(pending_rk_idx)) reg_free(1);
            if (!obj_is_base) reg_free(1);
            return;
        }

        /* Replicates what the general loop's first iteration would do for a pending index step
           immediately followed by '.field', then falls through to the same general machinery. */
        bool reuse_pending_idx_reg = is_temp(pending_rk_idx);
        int dest_reg;
        if (!obj_is_base)
            dest_reg = obj_reg;
        else if (reuse_pending_idx_reg)
            dest_reg = pending_rk_idx;
        else
            dest_reg = reg_alloc();

        emit_index_get(c, dest_reg, obj_reg, pending_rk_idx);

        if (reuse_pending_idx_reg && dest_reg != pending_rk_idx) reg_free(1);

        obj_reg = dest_reg;
        obj_is_base = false;
        pending_is_field = true;
        pending_field_idx = fused_field_idx;
    }

    while (equal(TOKEN_OPEN_BRACKET) || equal(TOKEN_DOT)) {
        bool reuse_pending_idx_reg = !pending_is_field && is_temp(pending_rk_idx);
        int dest_reg;
        if (!obj_is_base)
            dest_reg = obj_reg; /* reuse the chain's running temp in place */
        else if (reuse_pending_idx_reg)
            dest_reg = pending_rk_idx; /* reuse the (dying) index temp's register */
        else
            dest_reg = reg_alloc(); /* first hop, nothing safe to reuse */

        if (pending_is_field)
            emit_field_get(c, dest_reg, obj_reg, pending_field_idx);
        else
            emit_index_get(c, dest_reg, obj_reg, pending_rk_idx);

        /* Only free the index temp when it's a different register than dest_reg -- otherwise it was
           already folded in. */
        if (reuse_pending_idx_reg && dest_reg != pending_rk_idx) reg_free(1);

        obj_reg = dest_reg;
        obj_is_base = false;

        if (consume(TOKEN_OPEN_BRACKET)) {
            pending_is_field = false;
            pending_rk_idx = parse_binary(c, 0);
            /* Same fix as the first index step's own -- see that site's comment. */
            pending_rk_idx = box_if_raw(c, pending_rk_idx);
            require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
        } else {
            consume(TOKEN_DOT);
            if (!equal(TOKEN_IDENTIFIER)) {
                error_at("Expected field name after '.'");
                return;
            }
            pending_field_idx = chunk_add_pool(c, token.value);
            pending_is_field = true;
            lex();
        }
        if (parse_had_error) return;
    }

    if (consume(TOKEN_ASSIGN)) {
        int rk_val = parse_binary(c, 0);
        if (parse_had_error) return;

        /* Only a direct `param.field = ...` (obj_is_base, no preceding chain step) marks the
           parameter shape-sensitive -- an intermediate chain register (`a.b.c = ...`) isn't a
           parameter itself, and its own provenance isn't tracked (only the one-hop alias case,
           `local = param[idx]`, is -- see P.alias_source_param). */
        if (pending_is_field && obj_is_base) {
            mark_shape_sensitive(obj_reg);
            Shape* known = (obj_reg >= 0 && obj_reg < FRAME_REGISTERS) ? P.reg_known_shape[obj_reg] : NULL;
            unsigned int foffset;
            ValueType ftype;
            bool field_narrow_bit;
            if (known && shape_find_field(known, pending_field_idx, &foffset, &ftype, &field_narrow_bit)) {
                RawKind field_kind = (ftype == TYPE_INTEGER) ? RAWK_INT
                                     : (ftype == TYPE_REAL)  ? RAWK_REAL
                                                             : RAWK_NONE;
                if (field_kind != RAWK_NONE && rk_raw_kind(c, rk_val) == field_kind) {
                    int slot = raw_materialize(c, rk_val, field_kind);
                    if (slot >= 0) {
                        Opcode op = field_narrow_bit ? ((field_kind == RAWK_INT) ? OP_FIELD_SET_RAW_INT32
                                                                                 : OP_FIELD_SET_RAW_FLOAT32)
                                                     : ((field_kind == RAWK_INT) ? OP_FIELD_SET_RAW_INT
                                                                                 : OP_FIELD_SET_RAW_REAL);
                        chunk_emit(c, PACK3(op, obj_reg, 0, 0));
                        chunk_emit(c, foffset);
                        chunk_emit(c, (uint32_t)slot);
                        int floor_now =
                            (field_kind == RAWK_INT) ? P.raw_int_reserved_floor : P.raw_real_reserved_floor;
                        if (slot >= floor_now) {
                            if (field_kind == RAWK_INT)
                                raw_int_free(1);
                            else
                                raw_real_free(1);
                        }
                        return; /* obj_is_base is always true here, so no reg_free(1) for it needed */
                    }
                }
            }
        }
        if (pending_is_field) {
            emit_field_set(c, obj_reg, pending_field_idx, rk_val);
        } else if (index_safe_unchecked(obj_reg, pending_rk_idx)) {
            /* pending_rk_idx already guaranteed a plain register by index_safe_unchecked -- always
               fits RK8 directly. rk_val can be anything, so it still needs emit_index_set's own
               box/spill handling, just with the opcode swapped. */
            int rk_val_boxed = box_if_raw(c, rk_val);
            int spilled = 0;
            if (!rk8_fits(rk_val_boxed)) {
                rk_val_boxed = materialize(c, rk_val_boxed);
                spilled++;
            }
            chunk_emit(c, PACK3(OP_TYPED_INDEX_SET_UNCHECKED, obj_reg, pack_rk8(pending_rk_idx),
                                pack_rk8(rk_val_boxed)));
            if (spilled) reg_free(spilled);
        } else {
            emit_index_set(c, obj_reg, pending_rk_idx, rk_val);
        }

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
            if (obj_is_base) mark_shape_sensitive(obj_reg);

            /* Same compile-time field lookup the plain '=' branch above uses -- only meaningful
               when obj_is_base (this register really is the shape-sensitive parameter/alias, not
               an intermediate chain link -- P.reg_known_shape is never seeded for those). */
            Shape* known = (obj_is_base && obj_reg >= 0 && obj_reg < FRAME_REGISTERS)
                               ? P.reg_known_shape[obj_reg]
                               : NULL;
            unsigned int foffset = 0;
            ValueType ftype = TYPE_ANY;
            bool field_narrow_bit = false;
            RawKind field_kind = RAWK_NONE;
            if (known && shape_find_field(known, pending_field_idx, &foffset, &ftype, &field_narrow_bit)) {
                if (ftype == TYPE_INTEGER)
                    field_kind = RAWK_INT;
                else if (ftype == TYPE_REAL)
                    field_kind = RAWK_REAL;
            }

            int rk_rhs = parse_binary(c, 0);
            if (parse_had_error) return;

            Opcode bin_op = compound_assign_ops[i].op;
            /* Decomposing into raw GET + arithmetic + SET was tried and measured WORSE on
               nbody.aer whenever the rhs needed boxing first -- see the fused index-field-compound
               site's identical reasoning above. But when the rhs is ALREADY raw at this point (a
               raw local, or try_emit_arith_raw_boxed's fusion result), OP_FIELD_COMPOUND_RAW_INT/
               REAL reads/computes/writes the field raw AND takes rk_rhs raw directly -- no boxing
               to avoid paying for, only upside. */
            bool native_op_exists = (bin_op == OP_ADD || bin_op == OP_SUB || bin_op == OP_MUL);
            bool specialized = false;
            if (native_op_exists && field_kind != RAWK_NONE && rk_raw_kind(c, rk_rhs) == field_kind) {
                int slot = raw_materialize(c, rk_rhs, field_kind);
                if (slot >= 0) {
                    Opcode op = field_narrow_bit ? ((field_kind == RAWK_INT) ? OP_FIELD_COMPOUND_RAW_INT32
                                                                             : OP_FIELD_COMPOUND_RAW_FLOAT32)
                                                 : ((field_kind == RAWK_INT) ? OP_FIELD_COMPOUND_RAW_INT
                                                                             : OP_FIELD_COMPOUND_RAW_REAL);
                    chunk_emit(c, PACK3(op, obj_reg, bin_op, 0));
                    chunk_emit(c, foffset);
                    chunk_emit(c, (uint32_t)slot);
                    int floor_now =
                        (field_kind == RAWK_INT) ? P.raw_int_reserved_floor : P.raw_real_reserved_floor;
                    if (slot >= floor_now) {
                        if (field_kind == RAWK_INT)
                            raw_int_free(1);
                        else
                            raw_real_free(1);
                    }
                    specialized = true;
                }
            }
            if (!specialized) {
                /* One fused OP_FIELD_COMPOUND -- read, compute, and write back in a single
                   dispatch, a single vm_resolve_field call. No temp register needed: the result
                   writes straight back into the same field, never through a register at all. */
                rk_rhs = box_if_raw(c, rk_rhs);

                if (!rk16_fits(rk_rhs)) {
                    error_at("Expression too large to compile (register/constant index exceeds the fused "
                             "field-op encoding's range)");
                    return;
                }
                chunk_emit(c, PACK3(OP_FIELD_COMPOUND, obj_reg, bin_op, 0));
                chunk_emit(c, PACK_2X16(pending_field_idx, pack_rk16(rk_rhs)));
                if (is_temp(rk_rhs)) reg_free(1);
            }
        } else {
            int item_reg = reg_alloc();
            emit_index_get(c, item_reg, obj_reg, pending_rk_idx);

            int rk_rhs = parse_binary(c, 0);
            if (parse_had_error) return;

            emit_binary(c, item_reg, compound_assign_ops[i].op, item_reg, rk_rhs);
            if (is_temp(rk_rhs)) reg_free(1);

            emit_index_set(c, obj_reg, pending_rk_idx, item_reg);
            reg_free(1); /* item_reg */
        }

        if (!pending_is_field && is_temp(pending_rk_idx)) reg_free(1);
        if (!obj_is_base) reg_free(1);
        return;
    }

    /* Finish the pending step as a GET, falling through to a general expression statement. */
    if (!equal(TOKEN_OPEN_PARENTHESE) && !equal(TOKEN_PIPE)) {
        error_at("Expected an assignment ('='), a compound assignment ('+=' etc.), or a call/pipe "
                 "continuation after this chain");
        return;
    }

    {
        bool reuse_pending_idx_reg = !pending_is_field && is_temp(pending_rk_idx);
        int dest_reg;
        if (!obj_is_base)
            dest_reg = obj_reg;
        else if (reuse_pending_idx_reg)
            dest_reg = pending_rk_idx;
        else
            dest_reg = reg_alloc();

        if (pending_is_field)
            emit_field_get(c, dest_reg, obj_reg, pending_field_idx);
        else
            emit_index_get(c, dest_reg, obj_reg, pending_rk_idx);

        if (reuse_pending_idx_reg && dest_reg != pending_rk_idx) reg_free(1);

        int rk = parse_postfix_chain(c, dest_reg);
        unsigned int lhs_start = c->count;
        rk = parse_binary_ops(c, 0, rk, lhs_start);
        discard_statement_result(c, rk);
    }
}

/* Called when the skip-to-boundary loop stops at a plain NEWLINE, not already a boundary -- a
   statement that failed to introduce a block leaves an orphaned indented block otherwise. */
static void skip_orphaned_block(void) {
    if (!equal(TOKEN_NEW_LINE)) return;
    lex();
    if (!equal(TOKEN_INDENT)) return;
    int depth = 0;
    do {
        if (equal(TOKEN_INDENT))
            depth++;
        else if (equal(TOKEN_DEDENT))
            depth--;
        lex();
    } while (depth > 0 && !equal(TOKEN_END_OF_FILE));
}

/* Per-statement rollback: a compile error inside a body rolls back just that statement and
   resumes at the next boundary, instead of aborting the whole block. */
static void parse_block(Chunk* c) {
    require(TOKEN_NEW_LINE, "expected newline before indented block");
    require(TOKEN_INDENT, "expected indented block");
    if (parse_had_error) return;
    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_ELSE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        parse_had_error = false;
        P.recovered_at_boundary = false;
        unsigned int saved = c->count;
        unsigned int saved_marks = c->line_mark_count;
        chunk_mark_line(c, saved, current_source_line());
        parse_statement(c);
        if (parse_had_error) {
            P.any_compile_error = true;
            c->count = saved;
            c->line_mark_count = saved_marks;
            if (!P.recovered_at_boundary) {
                while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_ELSE) && !equal(TOKEN_DEDENT) &&
                       !equal(TOKEN_NEW_LINE))
                    lex();
                skip_orphaned_block();
            }
        }
    }
    consume(TOKEN_DEDENT);
    /* However this loop exited, the lexer now sits at a fresh statement boundary. */
    P.recovered_at_boundary = true;
    /* If the last statement failed and recovered, parse_had_error is left stale true -- every
       caller checks it right after to decide if ITS OWN construct failed. For a function
       definition this is serious: func_register already ran, so a stale flag would roll back the
       function's real bytecode while its name stays registered (can manifest as an infinite loop
       when called). Reset here so only a genuine top-level failure propagates. */
    parse_had_error = false;
}

static void parse_if(Chunk* c) {
    unsigned int cond_start = c->count;
    int rk_cond = parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after if condition");
    /* require() can't abort on failure -- without this, a malformed condition still compiles a
       real branch. */
    if (parse_had_error) return;

    unsigned int patch_jif = emit_cond_jump_if_false(c, rk_cond, cond_start);

    /* P.branch_depth disqualifies a variable assigned while nonzero from ever being raw-tracked --
       a name assigned different types down mutually-exclusive branches can't be resolved without
       real dataflow analysis. A counter since if/else nests. */
    P.branch_depth++;
    parse_block(c);
    P.branch_depth--;
    if (parse_had_error) return;

    if (consume(TOKEN_ELSE)) {
        require(TOKEN_COLON, "expected ':' after else");
        if (parse_had_error) return;
        chunk_emit(c, OP_JUMP);
        unsigned int patch_jmp = c->count;
        chunk_emit(c, 0);
        patch_jump(c, patch_jif, c->count);
        P.branch_depth++;
        parse_block(c);
        P.branch_depth--;
        patch_jump(c, patch_jmp, c->count);
    } else {
        patch_jump(c, patch_jif, c->count);
    }
}

/* Shared while/for-while tail: require ':', branch-if-false, body, back-edge to loop_top. */
/* Shared body-and-back-edge tail for every loop form. Callers that promoted P.reserved_floor can
   safely restore it unconditionally after this returns either way. */
static void parse_loop_body(Chunk* c, unsigned int loop_top, unsigned int patch_exit) {
    if (!loop_push(loop_top)) return;
    parse_block(c);
    if (parse_had_error) {
        P.loop_depth--;
        return;
    }

    chunk_emit(c, OP_JUMP);
    chunk_emit(c, (int)loop_top);
    patch_jump(c, patch_exit, c->count);
    loop_pop_and_patch(c, c->count);
}

static void parse_for_body(Chunk* c, unsigned int loop_top, int rk_cond) {
    require(TOKEN_COLON, "expected ':' after for/while condition");
    if (parse_had_error) return;

    /* The back-edge re-runs the condition, so every temp the condition writes is rewritten on each
       iteration. Those registers are ordinarily freed the moment the condition is compiled, which
       would let a shadowing assignment in the body ('total = f()' rebinding a raw local to a fresh
       boxed register) claim one as a permanent variable and have the next condition evaluation
       silently overwrite it. Holding the floor above them for the body's duration prevents that. */
    /* loop_top doubles as cond_start here -- it's already exactly "the bytecode offset the
       condition's own first instruction starts at" (the same reason the loop's own back-edge
       jumps there to re-evaluate the condition each iteration). Emitted BEFORE the floor is
       raised below: its compare/branch fusion only fires when the preceding LOADK's destination is
       a temp, so raising the floor first would silently disqualify every such loop. */
    unsigned int patch_exit = emit_cond_jump_if_false(c, rk_cond, loop_top);

    int saved_floor = P.reserved_floor;
    int raised_floor = saved_floor;
    if (P.loop_cond_peak > P.reserved_floor) {
        raised_floor = P.loop_cond_peak;
        P.reserved_floor = raised_floor;
        if (P.next_temp_register < P.reserved_floor) P.next_temp_register = P.reserved_floor;
    }

    parse_loop_body(c, loop_top, patch_exit);

    /* Only give the condition's registers back if the body claimed nothing above them. AER scopes
       variables to the whole function, so a name first assigned inside the body outlives the loop
       and its register must stay reserved -- lowering the floor past it would hand a live variable
       out as a temp to the next statement. */
    if (P.reserved_floor == raised_floor) {
        P.reserved_floor = saved_floor;
        if (P.next_temp_register < P.reserved_floor) P.next_temp_register = P.reserved_floor;
    }
    assert_variables_below_floor("for-loop body");
}

/* No exit-time cleanup needed -- col_reg/idx_reg are ordinary registers. Reserves the loop
   variable's register BEFORE compiling the collection expression, so later temps can never
   alias it. */
static void parse_for_in(Chunk* c, unsigned int loop_var_name) {
    ensure_boxed(
        c,
        loop_var_name); /* A for-in loop variable always holds a plain item, never raw -- an existing raw name of the
                                          same spelling must shadow to boxed first. */
    int item_reg = var_slot(c, loop_var_name);
    if (item_reg < 0) return;
    /* var_slot has no block scoping -- an already-declared name (a sibling or enclosing loop
       reusing the same spelling, e.g. `for i in 0..n: ... for i in 0..m: ...`) returns THAT
       register back, which may still carry a shape hint or a live safe_loop_item_regs entry from
       whatever last used it. This loop's own PREP/LOOP is about to start overwriting it every
       iteration regardless of what THIS loop turns out to prove, so any stale fact must be cleared
       before this loop's own body (or its own bound_safe/start_safe below) can be compiled. */
    P.reg_known_shape[item_reg] = NULL;
    P.reg_known_element_shape[item_reg] = NULL;
    P.alias_source_param[item_reg] = -1;
    invalidate_register(item_reg);
    /* invalidate_register cannot catch loop_var_name reusing the length_tracked_name NAME itself
       (`for n in 0..1000:` after `n = length(bodies)`) -- var_slot returns a bare register number,
       indistinguishable from a new one. This loop overwrites that register every iteration, so a
       later `for i in 0..n:` would otherwise trust the stale leftover. */
    if (P.length_tracked_valid && loop_var_name == P.length_tracked_name) P.length_tracked_valid = false;

    unsigned int start_code_begin = c->count;
    int rk_start = parse_binary(c, 0); /* the range's start, or the whole collection if no '..' follows */

    /* Direction is inferred at runtime from cur vs end, not step's sign. `..` is for-loop-specific
       syntax here. */
    if (consume(TOKEN_DOT_DOT)) {
        int rk_end = parse_binary(c, 0);
        int rk_step;
        if (consume(TOKEN_DOT_DOT))
            rk_step = parse_binary(c, 0);
        else
            rk_step = (int)chunk_add_pool(c, aer_int(1)) | RK_CONST_FLAG;

        require(TOKEN_COLON, "expected ':' after for-in clause");
        if (parse_had_error) return;

        /* Checked before arg_materialize, which may copy rk_start/rk_end into fresh registers that
           no longer identify their source. bound_safe: rk_end reads exactly the local proven ==
           length(P), and bound_array_reg records which P. start_safe: rk_start is literal 0, an
           enclosing safe loop's item register proven for that SAME array, or that plus a
           non-negative constant. Both fail closed -- an unrecognized shape just compiles as before. */
        bool bound_safe = false;
        int bound_array_reg = -1;
        if (P.length_tracked_valid && !(rk_end & (RK_CONST_FLAG | RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG))) {
            for (int vi = 0; vi < P.var_count; vi++) {
                if (P.var_names[vi] == P.length_tracked_name && P.var_kind[vi] == VAR_BOXED &&
                    P.var_regs[vi] == rk_end) {
                    bound_safe = true;
                    bound_array_reg = P.length_tracked_source_reg;
                    break;
                }
            }
        }
        bool start_safe = false;
        if (bound_safe) {
            if (rk_start & RK_CONST_FLAG) {
                /* Any non-negative literal start is exactly as safe as the 0 case this originally
                   only recognized -- the produced index sequence is still bounded below by this
                   same non-negative constant, and bound_safe already proves the upper bound. Not
                   just the common `for i in 0..n:` shape anymore -- covers `for p in 2..n:` (a
                   sieve-of-Eratosthenes-shaped loop skipping the first couple of indices) too. */
                AerVal startv = c->pool[rk_start & ~RK_CONST_FLAG];
                start_safe = (aer_type(startv) == TYPE_INTEGER && aer_as_int(startv) >= 0);
            } else if (!(rk_start & (RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG))) {
                for (int si = 0; si < P.safe_loop_depth; si++) {
                    if (P.safe_loop_item_regs[si] == rk_start &&
                        P.safe_loop_array_regs[si] == bound_array_reg) {
                        start_safe = true;
                        break;
                    }
                }
                /* Not a bare safe register -- check for exactly "safe_reg + non-negative-const",
                   the one instruction a `(i+1)` start compiles to (emit_binary, parser.c). Decoded
                   from the just-emitted word rather than matched syntactically, since this parser
                   compiles expressions directly with no separate AST pass to inspect. */
                if (!start_safe && c->count - start_code_begin == 1) {
                    uint32_t w = c->code[start_code_begin];
                    if ((w & 0xFF) == OP_ADD && (int)UNPACK_A(w) == rk_start) {
                        uint8_t lhs8 = (uint8_t)UNPACK_B(w), rhs8 = (uint8_t)UNPACK_C(w);
                        if (!RK8_IS_CONST(lhs8) && RK8_IS_CONST(rhs8)) {
                            int lhs_reg = RK8_INDEX(lhs8);
                            bool lhs_active_safe = false;
                            for (int si = 0; si < P.safe_loop_depth; si++) {
                                if (P.safe_loop_item_regs[si] == lhs_reg &&
                                    P.safe_loop_array_regs[si] == bound_array_reg) {
                                    lhs_active_safe = true;
                                    break;
                                }
                            }
                            if (lhs_active_safe) {
                                AerVal rhsv = c->pool[RK8_INDEX(rhs8)];
                                if (aer_type(rhsv) == TYPE_INTEGER && aer_as_int(rhsv) >= 0)
                                    start_safe = true;
                            }
                        }
                    }
                }
            }
        }
        bool this_loop_safe = bound_safe && start_safe && P.safe_loop_depth < LOOP_MAX;

        /* Snapshotted once, matching Lua/Python's range-for semantics -- a later mutation of the
           source variable has no effect on an already-running loop. Used to reuse a plain register
           via materialize(), which meant `for i in 0..n:` silently re-read `n` every iteration (an
           undocumented, untested quirk) and forced OP_ITER_RANGE_LOOP to re-validate types every
           dispatch; the snapshot removes both. */
        int cur_reg = arg_materialize(c, rk_start);
        int end_reg = arg_materialize(c, rk_end);
        int step_reg = arg_materialize(c, rk_step);

        /* Promotes temps to permanent status for the loop's duration -- without it, a fresh variable
           inside the body could alias cur_reg/step_reg (real bug with nested ranged loops).
           Restored to the pre-loop watermark once the loop's bytecode is emitted. */
        int saved_reserved_floor = P.reserved_floor;
        P.reserved_floor = P.next_temp_register;

        /* Loop-rotated: PREP once before the loop, LOOP at the bottom of the body -- the one form
           whose continue must defer-patch instead of jumping to a known target. */
        unsigned int patch_empty = emit_iter_range_prep(c, cur_reg, end_reg, step_reg, item_reg);

        if (!loop_push_rotated()) {
            P.reserved_floor = saved_reserved_floor;
            P.next_temp_register = saved_reserved_floor;
            return;
        }
        /* Pushed/popped exactly around this one loop's own body -- see safe_loop_item_regs's own
           comment (Parser struct) for why this is a small stack, not a whole-frame table. */
        if (this_loop_safe) {
            P.safe_loop_array_regs[P.safe_loop_depth] = bound_array_reg;
            P.safe_loop_item_regs[P.safe_loop_depth] = item_reg;
            P.safe_loop_depth++;
        }
        unsigned int body_start = c->count;
        parse_block(c);
        if (parse_had_error) {
            if (this_loop_safe) P.safe_loop_depth--;
            P.loop_depth--;
            P.reserved_floor = saved_reserved_floor;
            P.next_temp_register = saved_reserved_floor;
            return;
        }
        if (this_loop_safe) P.safe_loop_depth--;

        unsigned int loop_bottom = c->count;
        emit_iter_range_loop(c, cur_reg, end_reg, step_reg, item_reg, body_start);

        unsigned int exit_pos = c->count;
        patch_jump(c, patch_empty, exit_pos);
        loop_pop_and_patch_rotated(c, exit_pos, loop_bottom);

        P.reserved_floor = saved_reserved_floor;
        P.next_temp_register = saved_reserved_floor;
        return;
    }

    require(TOKEN_COLON, "expected ':' after for-in clause");
    if (parse_had_error) return;

    int col_reg = materialize(c, rk_start);

    int idx_reg = reg_alloc();
    unsigned int pool_zero = chunk_add_pool(c, aer_int(0));
    chunk_emit(c, PACK_OP_A_W16(OP_LOADK, idx_reg, pool_zero));

    /* idx_reg/col_reg must stay valid across the whole body, so they're protected before the body
       compiles, same as the range branch. */
    int saved_reserved_floor = P.reserved_floor;
    P.reserved_floor = P.next_temp_register;

    unsigned int loop_top = c->count; /* the iterate opcode is its own back-edge target */
    unsigned int patch_exit = emit_iter_next_array(c, col_reg, idx_reg, item_reg);

    parse_loop_body(c, loop_top, patch_exit);

    P.reserved_floor = saved_reserved_floor;
    P.next_temp_register = saved_reserved_floor;
}

/* Dict-only at runtime -- reserves both loop variables' registers before compiling the
   collection expression. */
static void parse_for_in_pair(Chunk* c, unsigned int key_name, unsigned int val_name) {
    ensure_boxed(c, key_name); /* same reasoning as parse_for_in's own ensure_boxed call */
    ensure_boxed(c, val_name);
    int key_reg = var_slot(c, key_name);
    if (key_reg < 0) return;
    int val_reg = var_slot(c, val_name);
    if (val_reg < 0) return;
    /* Same name-shadowing reasoning as parse_for_in's own identical block just above. */
    P.reg_known_shape[key_reg] = NULL;
    P.reg_known_element_shape[key_reg] = NULL;
    P.alias_source_param[key_reg] = -1;
    invalidate_register(key_reg);
    P.reg_known_shape[val_reg] = NULL;
    P.reg_known_element_shape[val_reg] = NULL;
    P.alias_source_param[val_reg] = -1;
    invalidate_register(val_reg);
    /* Same length_tracked_name-by-NAME reasoning as parse_for_in's own identical check. */
    if (P.length_tracked_valid && (key_name == P.length_tracked_name || val_name == P.length_tracked_name)) {
        P.length_tracked_valid = false;
    }

    int rk_col = parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after for-in clause");
    if (parse_had_error) return;

    int col_reg = materialize(c, rk_col);

    int idx_reg = reg_alloc();
    unsigned int pool_zero = chunk_add_pool(c, aer_int(0));
    chunk_emit(c, PACK_OP_A_W16(OP_LOADK, idx_reg, pool_zero));

    int saved_reserved_floor = P.reserved_floor;
    P.reserved_floor = P.next_temp_register;

    unsigned int loop_top = c->count;
    unsigned int patch_exit = emit_iter_next_pair(c, col_reg, idx_reg, key_reg, val_reg);

    parse_loop_body(c, loop_top, patch_exit);

    P.reserved_floor = saved_reserved_floor;
    P.next_temp_register = saved_reserved_floor;
}

/* AER has no separate `while` keyword -- `for <condition>:` (no `in`) IS the while form. */
static void parse_for_while(Chunk* c) {
    if (equal(TOKEN_IDENTIFIER)) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();

        /* Unambiguous here -- a comma after a for-loop's first identifier can only mean a second loop
           variable, never destructuring. */
        if (consume(TOKEN_COMMA)) {
            if (!equal(TOKEN_IDENTIFIER)) {
                error_at("Expected identifier after ','");
                return;
            }
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
        P.loop_cond_peak = P.next_temp_register;
        /* Resolves the postfix chain first, so `for cur.next:` works -- a bare-variable condition is
           unaffected, since the chain loop immediately returns unchanged. */
        int rk_chain = parse_postfix_chain(c, reg);
        int rk_cond = parse_binary_ops(c, 0, rk_chain, c->count);
        parse_for_body(c, loop_top, rk_cond);
        return;
    }
    unsigned int loop_top = c->count;
    P.loop_cond_peak = P.next_temp_register;
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
    if (name->length == 4 && strncmp(name->data, "math", 4) == 0) return CALL_MODULE_MATH;
    if (name->length == 6 && strncmp(name->data, "random", 6) == 0) return CALL_MODULE_RANDOM;
    if (name->length == 6 && strncmp(name->data, "string", 6) == 0) return CALL_MODULE_STRING;
    if (name->length == 4 && strncmp(name->data, "time", 4) == 0) return CALL_MODULE_TIME;
    if (name->length == 4 && strncmp(name->data, "json", 4) == 0) return CALL_MODULE_JSON;
    if (name->length == 10 && strncmp(name->data, "collection", 10) == 0) return CALL_MODULE_COLLECTION;
    if (name->length == 3 && strncmp(name->data, "net", 3) == 0) return CALL_MODULE_NET;
    if (name->length == 5 && strncmp(name->data, "regex", 5) == 0) return CALL_MODULE_REGEX;
    if (name->length == 5 && strncmp(name->data, "actor", 5) == 0) return CALL_MODULE_ACTOR;
    if (name->length == 9 && strncmp(name->data, "scheduler", 9) == 0) return CALL_MODULE_SCHEDULER;
    return CALL_MODULE_DYNAMIC;
}

#define NAME_IS(lit) (name->length == sizeof(lit) - 1 && strncmp(name->data, lit, sizeof(lit) - 1) == 0)

/* A literal identifier, never ambiguous, so resolvable once here. FN_ID_UNKNOWN for anything
   not a known function of that module. */
static int module_fn_id(int module_id, AerString* name) {
    switch (module_id) {
        case CALL_MODULE_MATH:
            if (NAME_IS("sqrt")) return FN_MATH_SQRT;
            if (NAME_IS("pow")) return FN_MATH_POW;
            if (NAME_IS("floor")) return FN_MATH_FLOOR;
            if (NAME_IS("ceil")) return FN_MATH_CEIL;
            if (NAME_IS("abs")) return FN_MATH_ABS;
            if (NAME_IS("min")) return FN_MATH_MIN;
            if (NAME_IS("max")) return FN_MATH_MAX;
            if (NAME_IS("sin")) return FN_MATH_SIN;
            if (NAME_IS("cos")) return FN_MATH_COS;
            if (NAME_IS("log")) return FN_MATH_LOG;
            if (NAME_IS("log2")) return FN_MATH_LOG2;
            if (NAME_IS("log10")) return FN_MATH_LOG10;
            if (NAME_IS("pi")) return FN_MATH_PI;
            if (NAME_IS("round")) return FN_MATH_ROUND;
            if (NAME_IS("tan")) return FN_MATH_TAN;
            if (NAME_IS("exp")) return FN_MATH_EXP;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_RANDOM:
            if (NAME_IS("random")) return FN_RANDOM_RANDOM;
            if (NAME_IS("randint")) return FN_RANDOM_RANDINT;
            if (NAME_IS("seed")) return FN_RANDOM_SEED;
            if (NAME_IS("choice")) return FN_RANDOM_CHOICE;
            if (NAME_IS("shuffle")) return FN_RANDOM_SHUFFLE;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_STRING:
            if (NAME_IS("upper")) return FN_STRING_UPPER;
            if (NAME_IS("lower")) return FN_STRING_LOWER;
            if (NAME_IS("trim")) return FN_STRING_TRIM;
            if (NAME_IS("contains")) return FN_STRING_CONTAINS;
            if (NAME_IS("split")) return FN_STRING_SPLIT;
            if (NAME_IS("starts_with")) return FN_STRING_STARTS_WITH;
            if (NAME_IS("ends_with")) return FN_STRING_ENDS_WITH;
            if (NAME_IS("repeat")) return FN_STRING_REPEAT;
            if (NAME_IS("replace")) return FN_STRING_REPLACE;
            if (NAME_IS("join")) return FN_STRING_JOIN;
            if (NAME_IS("index_of")) return FN_STRING_INDEX_OF;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_TIME:
            if (NAME_IS("now")) return FN_TIME_NOW;
            if (NAME_IS("strftime")) return FN_TIME_STRFTIME;
            if (NAME_IS("sleep")) return FN_TIME_SLEEP;
            if (NAME_IS("parse")) return FN_TIME_PARSE;
            if (NAME_IS("to_parts")) return FN_TIME_TO_PARTS;
            if (NAME_IS("from_parts")) return FN_TIME_FROM_PARTS;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_JSON:
            if (NAME_IS("encode")) return FN_JSON_ENCODE;
            if (NAME_IS("decode")) return FN_JSON_DECODE;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_COLLECTION:
            if (NAME_IS("append")) return FN_COLLECTION_APPEND;
            if (NAME_IS("delete")) return FN_COLLECTION_DELETE;
            if (NAME_IS("copy")) return FN_COLLECTION_COPY;
            if (NAME_IS("insert")) return FN_COLLECTION_INSERT;
            if (NAME_IS("index_of")) return FN_COLLECTION_INDEX_OF;
            if (NAME_IS("keys")) return FN_COLLECTION_KEYS;
            if (NAME_IS("sort")) return FN_COLLECTION_SORT;
            if (NAME_IS("reserve")) return FN_COLLECTION_RESERVE;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_NET:
            if (NAME_IS("connect")) return FN_NET_CONNECT;
            if (NAME_IS("send")) return FN_NET_SEND;
            if (NAME_IS("recv")) return FN_NET_RECV;
            if (NAME_IS("close")) return FN_NET_CLOSE;
            if (NAME_IS("listen")) return FN_NET_LISTEN;
            if (NAME_IS("accept")) return FN_NET_ACCEPT;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_REGEX:
            if (NAME_IS("match")) return FN_REGEX_MATCH;
            if (NAME_IS("find")) return FN_REGEX_FIND;
            if (NAME_IS("replace")) return FN_REGEX_REPLACE;
            if (NAME_IS("find_all")) return FN_REGEX_FIND_ALL;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_ACTOR:
            if (NAME_IS("spawn")) return FN_ACTOR_SPAWN;
            if (NAME_IS("send")) return FN_ACTOR_SEND;
            if (NAME_IS("receive")) return FN_ACTOR_RECEIVE;
            if (NAME_IS("call")) return FN_ACTOR_CALL;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_SCHEDULER:
            if (NAME_IS("add")) return FN_SCHEDULER_ADD;
            if (NAME_IS("run")) return FN_SCHEDULER_RUN;
            return FN_ID_UNKNOWN;
        default: return FN_ID_UNKNOWN;
    }
}

#undef NAME_IS

static int parse_module_call(Chunk* c) {
    int module_id = module_call_id(aer_as_string(token.value));
    unsigned int module_idx = chunk_add_pool(c, token.value);
    lex();
    if (!consume(TOKEN_DOT)) {
        error_at("A module can't be used as a value on its own — call a function on it, e.g. "
                 "'module.function(...)'");
        return 0;
    }
    if (!equal(TOKEN_IDENTIFIER)) {
        error_at("Expected a function name after '.'");
        return 0;
    }
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
    chunk_emit(c, PACK3(OP_CALL_MODULE, dest, base, arg_count));
    chunk_emit(c, (uint32_t)module_idx);
    chunk_emit(c, (uint32_t)fn_idx);
    chunk_emit(c, PACK_2X16((uint16_t)module_id, (uint16_t)fn_id));
    return dest;
}

/* The quoted form, for paths the dotted form can't express (explicit relative components, an
   absolute path). Used exactly as written, never dot-converted. Bare native module names aren't
   reachable this way -- a quoted path always means "look on disk". `alias`/`alias_len` are already
   known (from the identifier `parse_import` found immediately before this string -- `import alias
   "path"`, Go's own import-alias convention, replacing the old `import "path" as alias`) if
   `alias_len > 0`; otherwise the bound name is derived from the path's own last segment. */
static void parse_import_path(Chunk* c, const char* alias, unsigned int alias_len) {
    AerString* path_str = aer_as_string(token.value);
    unsigned int path_len = path_str->length;
    char path_buf[256];
    if (path_len == 0 || path_len >= sizeof(path_buf)) {
        error_at("Import path is empty or too long");
        return;
    }
    memcpy(path_buf, path_str->data, path_len);
    lex();

    char derived_buf[64];
    if (alias_len == 0) {
        /* Derive the bound name from the last path segment, stripping a trailing ".aer". */
        unsigned int end = path_len;
        if (end > 4 && strncmp(path_buf + end - 4, ".aer", 4) == 0) end -= 4;
        unsigned int start = 0;
        for (unsigned int i = 0; i < end; i++)
            if (path_buf[i] == '/' || path_buf[i] == '\\') start = i + 1;
        alias_len = end - start;
        if (alias_len == 0 || alias_len >= sizeof(derived_buf)) {
            error_at("Cannot derive a module name from this path; give it an alias: import name \"path\"");
            return;
        }
        memcpy(derived_buf, path_buf + start, alias_len);
        alias = derived_buf;
    }
    chunk_add_import(c, alias, alias_len, path_buf, path_len);
}

/* Top level only (a known narrow gap: an import inside an if/for at top level is accepted).
   A failed import is silently not registered. */
static void parse_import(Chunk* c) {
    if (P.function_depth != 0) {
        error_at("'import' is only allowed at the top level of a file, not inside a function");
        return;
    }
    if (token.type == TOKEN_STRING) {
        parse_import_path(c, NULL, 0);
        return;
    }
    if (!equal(TOKEN_IDENTIFIER)) {
        error_at("Expected a module name or a quoted path after 'import'");
        return;
    }

    /* The first identifier is ambiguous until we see what follows it: an alias immediately before a
       quoted path (`import myalias "some/path.aer"`), or the first segment of a dotted
       native-module name (`import math`, `import a.b.c`). Read it once, then decide -- both
       branches need it, so it's captured before either. */
    char path_buf[256];
    unsigned int path_len, bind_start = 0, bind_len;
    {
        const char* seg = aer_as_string(token.value)->data;
        unsigned int seg_len = aer_as_string(token.value)->length;
        if (seg_len >= sizeof(path_buf)) {
            error_at("Import name too long");
            return;
        }
        memcpy(path_buf, seg, seg_len);
        bind_len = seg_len;
        path_len = seg_len;
        lex();
    }
    if (token.type == TOKEN_STRING) {
        parse_import_path(c, path_buf, bind_len);
        return;
    }

    while (consume(TOKEN_DOT)) {
        if (!equal(TOKEN_IDENTIFIER)) {
            error_at("Expected a module name segment after '.'");
            return;
        }
        const char* seg = aer_as_string(token.value)->data;
        unsigned int seg_len = aer_as_string(token.value)->length;
        path_buf[path_len++] = '/';
        if (path_len + seg_len >= sizeof(path_buf)) {
            error_at("Import path too long");
            return;
        }
        bind_start = path_len;
        memcpy(path_buf + path_len, seg, seg_len);
        path_len += seg_len;
        bind_len = seg_len;
        lex();
    }
    chunk_add_import(c, path_buf + bind_start, bind_len, path_buf, path_len);
}

/* Checked against a fixed list, consistent with every other call target resolving at compile
   time. Struct construction is deliberately excluded -- already resolved via is_struct_name. */
static bool is_builtin_name(Chunk* c, unsigned int name_idx) {
    AerString* s = aer_as_string(c->pool[name_idx]);
    static const char* const names[] = {"length", "print", "type", "assert", "panic", "Result"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t len = strlen(names[i]);
        if (s->length == len && strncmp(s->data, names[i], len) == 0) return true;
    }
    return false;
}

/* Mirrors module_call_id below -- only called after is_builtin_name confirms a match. */
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
    unsigned int arg_code_begin = c->count;
    int arg_reg_base;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error) return 0;
    unsigned int arg_code_end = c->count;

    int dest = (arg_count > 0) ? arg_reg_base : reg_alloc();
    if (arg_count > 1) reg_free(arg_count - 1);
    int base = arg_reg_base < 0 ? dest : arg_reg_base;
    int call_id = builtin_call_id(aer_as_string(c->pool[name_idx]));
    chunk_emit(c, PACK3(OP_CALL_BUILTIN, dest, base, arg_count));
    chunk_emit(c, (uint32_t)name_idx);
    chunk_emit(c, (uint32_t)call_id);

    /* One-shot side channel to parse_assignment, consumed by exact register equality. Recognizes
       `length(P)` for any plain-register P, parameter or local -- a locally-built typed array is
       just as eligible for the bound proof, it simply has no shape to specialize on. Widening
       beyond parameters is safe because the field-access sites separately require
       arr_reg == P.hint_param_reg, so a local can only ever reach the bare-index family. */
    P.last_length_call_result_reg = -1;
    P.last_length_call_arg_reg = -1;
    if (call_id == CALL_BUILTIN_LENGTH && arg_count == 1) {
        int arg_orig_reg = base;
        if (arg_code_end - arg_code_begin == 1) {
            uint32_t w = c->code[arg_code_begin];
            if ((w & 0xFF) == OP_MOVE && (int)UNPACK_A(w) == base) arg_orig_reg = (int)UNPACK_B(w);
        }
        if (arg_orig_reg >= 0) {
            P.last_length_call_result_reg = dest;
            P.last_length_call_arg_reg = arg_orig_reg;
        }
    }
    return dest;
}

/* Reached as an expression or a bare statement. Handles both function calls and struct
   instantiation (is_struct_name), with global builtins as a third fallback. */
/* Set right after a bare call emits, checked by parse_return to detect a true tail call --
   c->count only grows, so last_bare_call_end == c->count proves the whole return expression is
   exactly one bare call. last_bare_call_start is captured directly (OP_CALL and OP_CALL_VALUE are
   both 2 words now, but for different reasons -- OP_CALL's 2nd word is callee_offset, OP_CALL_VALUE's
   is callee_reg -- so a fixed backward offset would still be fragile; explicit capture stays the
   simplest correct approach regardless). */
static unsigned int last_bare_call_end = (unsigned int)-1;
static unsigned int last_bare_call_start = (unsigned int)-1;

static int parse_call(Chunk* c, unsigned int name_idx) {
    /* string(x) is a cast (TO_STR), checked before anything else -- matching the exact "immune to
       shadowing, fixed production, never a name lookup" property `x as string` always had (see
       emit_primitive_cast's own comment). Not a reserved token like integer/float/boolean (it
       collides with the stdlib `string` module, so it stayed an ordinary identifier even before),
       but the parse-time behavior carries over unchanged: exactly one argument, no comma list. */
    AerString* callee_name = aer_as_string(c->pool[name_idx]);
    if (callee_name->length == 6 && strncmp(callee_name->data, "string", 6) == 0) {
        int lhs = parse_binary(c, 0);
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after cast argument");
        if (parse_had_error) return 0;
        return emit_primitive_cast(c, -1, lhs);
    }

    /* Builtins win unconditionally -- length/print/type/assert/panic/Result can never be shadowed. */
    if (is_builtin_name(c, name_idx)) return parse_builtin_call(c, name_idx);

    /* A call through an existing local variable resolves via OP_CALL_VALUE instead of compile-time
       name resolution. A top-level variable of this name is never callable from inside a
       function -- reported immediately so it isn't mistaken for a forward reference. */
    int var_reg = -1;
    bool is_local_var = var_lookup(name_idx, &var_reg);
    if (!is_local_var && P.function_depth > 0) {
        int dummy_reg;
        if (global_lookup(name_idx, &dummy_reg)) {
            error_at("'%s' is a top-level variable — not accessible inside a function; pass it as a "
                     "parameter (or rename)",
                     aer_as_string(c->pool[name_idx])->data);
            return 0;
        }
    }
    bool is_var = is_local_var;

    bool is_struct = !is_var && is_struct_name(name_idx);
    unsigned int func_offset = 0, func_arity = 0, func_min_arity = 0, func_max_registers = 0;
    unsigned int func_max_raw_ints = 0, func_max_raw_reals = 0, func_index = 0;
    AerVal* func_defaults = NULL;
    bool is_func =
        !is_var && !is_struct &&
        func_full_lookup(c, name_idx, &func_offset, &func_arity, &func_min_arity, &func_defaults,
                         &func_max_registers, &func_max_raw_ints, &func_max_raw_reals, &func_index);
    /* Optimistically assumed to be a function defined later in this same parse() call -- caught
       and reported once parse()'s top-level loop ends if it never actually is. Builtins are already
       handled unconditionally at the top of this function, so reaching here with none of
       is_var/is_struct/is_func true always means an unresolved name, never a builtin. */
    bool is_forward_ref = false;
    const char* call_site_cursor = NULL;
    if (!is_var && !is_struct && !is_func) {
        is_forward_ref = true;
        call_site_cursor =
            current_source_cursor(); /* captured NOW -- before the arg list below consumes past it */
    }

    /* Usually a no-op check, not a copy -- see arg_materialize's own comment. */
    int arg_reg_base;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error) return 0;

    /* A direct call must check arity -- fewer args than declared means some fall back to
       defaults, not garbage from a prior occupant of that frame slot. */
    bool arity_error =
        is_func && ((unsigned int)arg_count > func_arity || (unsigned int)arg_count < func_min_arity);
    if (arity_error) {
        const char* fname = aer_as_string(c->pool[name_idx])->data;
        if (func_min_arity == func_arity)
            error_at("Function '%s' expects %u argument%s, got %d", fname, func_arity,
                     func_arity == 1 ? "" : "s", arg_count);
        else
            error_at("Function '%s' expects between %u and %u arguments, got %d", fname, func_min_arity,
                     func_arity, arg_count);
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
                                         func_max_registers, func_max_raw_ints, func_max_raw_reals);
        callee_reg = reg_alloc();
        chunk_emit(c, PACK_OP_A_W16(OP_LOADK, callee_reg, chunk_add_pool(c, fv)));
    }

    if (is_var) {
        last_bare_call_start = c->count;
        emit_call_value(c, dest, base, arg_count, var_reg);
        last_bare_call_end = c->count;
    } else if (is_struct) {
        emit_struct_new(c, dest, name_idx, base, arg_count);
    } else if (is_func) {
        last_bare_call_start = c->count;
        if (needs_call_value)
            emit_call_value(c, dest, base, arg_count, callee_reg);
        else
            emit_call(
                c, dest, func_offset, base, arg_count,
                func_index); /* exact arity -- no forward-ref patching needed, is_func means already resolved */
        last_bare_call_end = c->count;
    } else {
        last_bare_call_start = c->count;
        unsigned int patch_offset = emit_call(c, dest, func_offset, base, arg_count, func_index);
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
    reg_free(
        1); /* the err register -- only dest (== reg_base) stays live past the call, same convention parse_builtin_call's own arg_count>1 case follows */
    unsigned int name_idx = chunk_add_pool(c, aer_make_string_copy("Result", 6));
    chunk_emit(c, PACK3(OP_CALL_BUILTIN, reg_base, reg_base, 2));
    chunk_emit(c, (uint32_t)name_idx);
    chunk_emit(c, (uint32_t)CALL_BUILTIN_RESULT);
    emit_return(c, reg_base);
}

/* `return a, b, ...` packs into an array (OP_ARRAY_NEW) -- destructuring's single-RHS-expression
   case already treats a call's result as "the array to unpack". Tail-call optimization: `return
   f(args)` with nothing else wrapping the call patches that call's opcode in place, not
   reachable for the multi-return case (the array built there isn't the call's own result). A
   plain `return` always means "a success value (or values)" -- signaling failure is `raise`'s job
   alone, so there's no shape ambiguity left for this to detect or reject. */
static void parse_return(Chunk* c) {
    if (P.function_depth == 0) {
        error_at("'return' outside function");
        return;
    }

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
            int orig_op = c->code[op_slot] & 0xFF;
            if (orig_op == OP_CALL || orig_op == OP_CALL_VALUE) {
                int tail_op = (orig_op == OP_CALL) ? OP_TAIL_CALL : OP_TAIL_CALL_VALUE;
                c->code[op_slot] = (c->code[op_slot] & ~0xFFU) | (uint32_t)tail_op;
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
    if (P.function_depth == 0) {
        error_at("'raise' outside function");
        return;
    }
    unsigned int null_idx = chunk_add_pool(c, aer_null());
    int reg_base = arg_materialize(c, (int)null_idx | RK_CONST_FLAG);
    int rk_err = parse_binary(c, 0);
    if (parse_had_error) return;
    arg_materialize(c, rk_err);
    emit_result_call_and_return(c, reg_base);
}

/* A function value in expression position -- an anonymous function has no name to self-reference
   by, so parse_function's registration-before-body ordering simply doesn't apply. Not a
   specialization target either way: lbl_call only specializes named, ChunkFunction-registered
   calls (func_index indexes chunk->functions[]), and AerFunction (what this compiles into) has no
   specialization table -- always compiles with no shape hint (-1/NULL). */
static int parse_function_expr(Chunk* c) {
    unsigned int param_names[FRAME_REGISTERS];
    AerVal param_defaults[FRAME_REGISTERS];
    int param_count, min_param_count;
    parse_function_signature(c, param_names, param_defaults, &param_count, &min_param_count);
    if (parse_had_error) return 0;

    chunk_emit(c, OP_JUMP);
    unsigned int patch = c->count;
    chunk_emit(c, 0);
    unsigned int func_start = c->count;

    unsigned int captured_max_registers, captured_max_raw_ints, captured_max_raw_reals;
    parse_function_body(c, param_names, param_count, -1, NULL, false, NULL, NULL, 0, &captured_max_registers,
                        &captured_max_raw_ints, &captured_max_raw_reals);

    patch_jump(c, patch, c->count);

    unsigned int default_count = (unsigned int)param_count - (unsigned int)min_param_count;
    AerVal* defaults = NULL;
    if (default_count > 0) {
        defaults = xmalloc(sizeof(AerVal) * default_count);
        for (unsigned int i = 0; i < default_count; i++)
            defaults[i] = param_defaults[(unsigned int)min_param_count + i];
    }
    AerVal fv =
        build_function_value(func_start, (unsigned int)param_count, (unsigned int)min_param_count, defaults,
                             captured_max_registers, captured_max_raw_ints, captured_max_raw_reals);
    return (int)chunk_add_pool(c, fv) | RK_CONST_FLAG;
}

/* Parses `(params...):' -- shared by the original top-level function compile and a later
   specialization recompile (vm.c's vm_call_resolve_specialization, re-lexing a retained source span, see
   ChunkFunction.source_span). Caller must already be positioned right at '('. Once one parameter
   has a default, every parameter after it must too. */
static void parse_function_signature(Chunk* c, unsigned int* param_names, AerVal* param_defaults,
                                     int* out_param_count, int* out_min_param_count) {
    require(TOKEN_OPEN_PARENTHESE, "expected '(' after function name");
    if (parse_had_error) return;

    int param_count = 0;
    int min_param_count = 0;
    bool seen_default = false;
    if (!equal(TOKEN_CLOSE_PARENTHESE)) {
        do {
            if (!equal(TOKEN_IDENTIFIER)) {
                error_at("Expected parameter name");
                return;
            }
            if (param_count >= FRAME_REGISTERS) {
                error_at("Too many parameters (max %d)", FRAME_REGISTERS);
                return;
            }
            param_names[param_count] = chunk_add_pool(c, token.value);
            lex();
            if (consume(TOKEN_ASSIGN)) {
                bool unused_narrow;
                if (!parse_literal_default(c, &param_defaults[param_count], &unused_narrow)) {
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
    require(TOKEN_COLON, "expected ':' after function signature");
    *out_param_count = param_count;
    *out_min_param_count = min_param_count;
}

/* Compiles a function's body -- parameter binding, parse_block, implicit return null, peak-register
   capture -- shared by the original compile and a specialization recompile. Saves and restores
   everything it touches. hint_param_reg/hint_shape seed one parameter's shape for this compile
   only; hint_is_element_shape picks the table: false means the parameter IS the struct/packed
   array, true means it is a plain array and hint_shape describes `param[idx]`. raw_param_regs/
   types/count bind those parameters as raw locals -- see OP_UNBOX_PARAM_INT/REAL (vm.h) for why. */
static void parse_function_body(Chunk* c, unsigned int* param_names, int param_count, int hint_param_reg,
                                Shape* hint_shape, bool hint_is_element_shape, const int* raw_param_regs,
                                const ValueType* raw_param_types, int raw_param_count,
                                unsigned int* out_max_registers, unsigned int* out_max_raw_ints,
                                unsigned int* out_max_raw_reals) {
    unsigned int saved_var_names[FRAME_REGISTERS];
    int saved_var_regs[FRAME_REGISTERS];
    VarKind saved_var_kind[FRAME_REGISTERS];
    int saved_var_count = P.var_count;
    int saved_next_temp = P.next_temp_register;
    int saved_reserved_floor = P.reserved_floor;
    int saved_max_register_used = P.max_register_used;
    int saved_max_raw_int_used = P.max_raw_int_used;
    int saved_max_raw_real_used = P.max_raw_real_used;
    int saved_raw_int_next_temp = P.raw_int_next_temp;
    int saved_raw_int_reserved_floor = P.raw_int_reserved_floor;
    int saved_raw_real_next_temp = P.raw_real_next_temp;
    int saved_raw_real_reserved_floor = P.raw_real_reserved_floor;
    memcpy(saved_var_names, P.var_names, sizeof(unsigned int) * (size_t)P.var_count);
    memcpy(saved_var_regs, P.var_regs, sizeof(int) * (size_t)P.var_count);
    memcpy(saved_var_kind, P.var_kind, sizeof(VarKind) * (size_t)P.var_count);
    P.var_count = 0;
    P.next_temp_register = 0;
    P.reserved_floor = 0;
    P.max_register_used = 0;
    P.max_raw_int_used = 0;
    P.max_raw_real_used = 0;
    P.raw_int_next_temp = 0;
    P.raw_int_reserved_floor = 0;
    P.raw_real_next_temp = 0;
    P.raw_real_reserved_floor = 0;

    memset(P.shape_sensitive_param, 0, sizeof(P.shape_sensitive_param));
    for (int i = 0; i < FRAME_REGISTERS; i++)
        P.alias_source_param[i] = -1;
    memset(P.reg_known_shape, 0, sizeof(P.reg_known_shape));
    memset(P.reg_known_element_shape, 0, sizeof(P.reg_known_element_shape));
    P.last_plain_index_dest_reg = -1;
    P.last_plain_index_src_param = -1;
    P.last_plain_index_known_elem_shape = NULL;
    P.current_param_count = param_count;
    /* hint_param_reg only enables the loop-bound-hoisting optimization for a packed-array
       specialization (kind == SPEC_KIND_PACKED_ARRAY, hint_is_element_shape false) -- an
       array-of-structs specialization has no per-object structural guarantee (see SPEC_KIND_
       ARRAY_OF_STRUCTS's own comment, vm.c) so index_safe_unchecked must never engage for one. */
    P.hint_param_reg = hint_is_element_shape ? -1 : hint_param_reg;
    P.length_tracked_valid = false;
    P.length_tracked_source_reg = -1;
    P.last_length_call_result_reg = -1;
    P.last_length_call_arg_reg = -1;
    P.safe_loop_depth = 0;

    P.function_depth++;
    for (int i = 0; i < param_count; i++) {
        var_slot(c, param_names[i]);
        /* Parameter i is always register i, so var_slot's return value needs no inspection. Called
           even for a parameter about to be rebound raw: it must still burn its boxed slot so the
           "parameter i occupies register i" assumption keeps holding for the others. */
        if (i == hint_param_reg) {
            if (hint_is_element_shape)
                P.reg_known_element_shape[i] = hint_shape;
            else
                P.reg_known_shape[i] = hint_shape;
        }
        for (int k = 0; k < raw_param_count; k++) {
            if (raw_param_regs[k] != i) continue;
            bool is_int = raw_param_types[k] == TYPE_INTEGER;
            int slot = is_int ? raw_int_reserve_one() : raw_real_reserve_one();
            if (slot >= 0) {
                /* var_slot just appended P.var_count-1 as this parameter's own (boxed) entry --
                   rebind THAT SAME entry to the raw slot instead, exactly as an ordinary local's
                   first raw-eligible assignment would, just applied after the fact rather than at
                   the point of declaration (a parameter has no "first assignment" of its own to
                   hook -- binding time IS its first assignment, conceptually). */
                Opcode unbox_op = is_int ? OP_UNBOX_PARAM_INT : OP_UNBOX_PARAM_REAL;
                chunk_emit(c, PACK2(unbox_op, slot, i));
                P.var_regs[P.var_count - 1] = slot;
                P.var_kind[P.var_count - 1] = is_int ? VAR_RAW_INT : VAR_RAW_REAL;
            }
            /* slot < 0: raw_ints/raw_reals budget exhausted (realistic -- this shape's own raw
               field usage already competes for the same 32-slot budget). Leave THIS ONE parameter
               boxed (var_slot's binding above already stands, untouched) and fall through to the
               other candidates in raw_param_regs -- one exhausted budget doesn't block the rest. */
            break;
        }
    }

    parse_block(c);
    P.function_depth--;

    if (!parse_had_error) {
        /* Implicit 'return null' if control falls off the end. */
        int rk_null = (int)chunk_add_pool(c, aer_null()) | RK_CONST_FLAG;
        int reg_null = materialize(c, rk_null);
        emit_return(c, reg_null);
    }

    *out_max_registers = (unsigned int)P.max_register_used;
    *out_max_raw_ints = (unsigned int)P.max_raw_int_used;
    *out_max_raw_reals = (unsigned int)P.max_raw_real_used;

    P.var_count = saved_var_count;
    memcpy(P.var_names, saved_var_names, sizeof(unsigned int) * (size_t)saved_var_count);
    memcpy(P.var_regs, saved_var_regs, sizeof(int) * (size_t)saved_var_count);
    memcpy(P.var_kind, saved_var_kind, sizeof(VarKind) * (size_t)saved_var_count);
    P.next_temp_register = saved_next_temp;
    P.reserved_floor = saved_reserved_floor;
    P.max_register_used = saved_max_register_used;
    P.max_raw_int_used = saved_max_raw_int_used;
    P.max_raw_real_used = saved_max_raw_real_used;
    P.raw_int_next_temp = saved_raw_int_next_temp;
    P.raw_int_reserved_floor = saved_raw_int_reserved_floor;
    P.raw_real_next_temp = saved_raw_real_next_temp;
    P.raw_real_reserved_floor = saved_raw_real_reserved_floor;
}

/* Named functions can't nest. Once one parameter has a default, every parameter after it must
   too. */
static void parse_function(Chunk* c) {
    if (!equal(TOKEN_IDENTIFIER)) {
        error_at("Expected function name");
        return;
    }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    if (is_builtin_name(c, name_idx)) {
        error_at("'%s' is a reserved builtin name and can't be redefined as a function",
                 aer_as_string(token.value)->data);
        return;
    }
    /* Captured unconditionally so the span from '(' onward is available if this function turns out
       shape-sensitive. Must happen before the lex() below: the cursor reflects the position past
       the token just scanned, which is still the name, so it sits exactly at '(' now. */
    const char* span_start = current_source_cursor();
    unsigned int span_start_line = current_source_line();
    lex();

    unsigned int param_names[FRAME_REGISTERS];
    AerVal param_defaults[FRAME_REGISTERS]; /* only [min_param_count, param_count) are meaningful */
    int param_count, min_param_count;
    parse_function_signature(c, param_names, param_defaults, &param_count, &min_param_count);
    if (parse_had_error) return;

    chunk_emit(c, OP_JUMP);
    unsigned int patch = c->count;
    chunk_emit(c, 0);
    unsigned int func_start = c->count;

    /* Registered before the body so a self-recursive call inside resolves -- any other
       not-yet-defined function's call just needs to resolve somewhere else in this parse() call
       (pending_call_add). */
    unsigned int default_count = (unsigned int)param_count - (unsigned int)min_param_count;
    AerVal* defaults = NULL;
    if (default_count > 0) {
        defaults = xmalloc(sizeof(AerVal) * default_count);
        for (unsigned int i = 0; i < default_count; i++)
            defaults[i] = param_defaults[(unsigned int)min_param_count + i];
    }
    func_register(c, name_idx, func_start, (unsigned int)param_count, (unsigned int)min_param_count,
                  defaults);
    /* Patched in once the body finishes -- safe since nothing reads it until this function is
       actually called, long after compilation. Named functions can't nest, so no other
       chunk_add_function call can land between here and the patch. */
    unsigned int this_func_idx = c->function_count - 1;

    unsigned int captured_max_registers, captured_max_raw_ints, captured_max_raw_reals;
    parse_function_body(c, param_names, param_count, -1, NULL, false, NULL, NULL, 0, &captured_max_registers,
                        &captured_max_raw_ints, &captured_max_raw_reals);

    /* Fold P.shape_sensitive_param[] into one bitmask; retain the source span (owned copy, see
       ChunkFunction.source_span's own comment) only when it's actually needed -- the common case
       (not shape-sensitive) pays nothing beyond the mask computation itself. parse_function_body
       already restored P.shape_sensitive_param/P.current_param_count's OWN inputs, but not the fold-in
       -- reads them here, right after the call, before anything else can touch them. */
    unsigned int shape_mask = 0;
    for (int i = 0; i < param_count && i < 32; i++)
        if (P.shape_sensitive_param[i]) shape_mask |= (1u << i);
    c->functions[this_func_idx].shape_sensitive_mask = shape_mask;
    if (shape_mask != 0) {
        const char* span_end = current_source_cursor();
        unsigned int span_len = (unsigned int)(span_end - span_start);
        char* span_copy = xmalloc((size_t)span_len + 1);
        memcpy(span_copy, span_start, span_len);
        span_copy[span_len] = '\0';
        c->functions[this_func_idx].source_span = span_copy;
        c->functions[this_func_idx].source_span_len = span_len;
        c->functions[this_func_idx].source_span_line = span_start_line;
    }

    c->functions[this_func_idx].max_registers = captured_max_registers;
    c->functions[this_func_idx].max_raw_ints = captured_max_raw_ints;
    c->functions[this_func_idx].max_raw_reals = captured_max_raw_reals;

    patch_jump(c, patch, c->count);
}

/* Lazily compiles a specialized body for target_f's shape-sensitive parameter, keyed by a Shape
   observed at a real call site (vm.c's vm_call_resolve_specialization is the only caller). Appends to the chunk
   currently executing, so the caller must re-run chunk_ensure_field_cache/call_spec_cache after.
   Every global it touches is saved and restored, so a false return can't corrupt what runs next.
   raw_param_regs/types/count (NULL/NULL/0 for a shape-only compile) additionally bind those
   parameters as raw locals; when set, *out_entry's shape/kind/raw_* are left for the caller. */
bool parser_specialize_function(Chunk* c, ChunkFunction* target_f, Shape* shape, SpecKind kind,
                                int param_index, SpecEntry* out_entry, const int* raw_param_regs,
                                const ValueType* raw_param_types, int raw_param_count) {
    if (!target_f->source_span)
        return false; /* defensive -- shouldn't happen alongside a nonzero shape_sensitive_mask */

    bool saved_had_error = parse_had_error;
    parse_had_error = false;

    ParserState* saved_parser = parser_save_state();
    LexerState* saved_lexer = lexer_save_state();
    lexer_begin_span(target_f->source_span, target_f->source_span_len, target_f->source_span_line);
    lex();

    unsigned int param_names[FRAME_REGISTERS];
    AerVal param_defaults[FRAME_REGISTERS];
    int param_count, min_param_count;
    parse_function_signature(c, param_names, param_defaults, &param_count, &min_param_count);

    unsigned int new_offset = c->count;
    unsigned int max_registers = 0, max_raw_ints = 0, max_raw_reals = 0;
    if (!parse_had_error) {
        parse_function_body(c, param_names, param_count, param_index, shape,
                            kind == SPEC_KIND_ARRAY_OF_STRUCTS, raw_param_regs, raw_param_types,
                            raw_param_count, &max_registers, &max_raw_ints, &max_raw_reals);
    }
    bool ok = !parse_had_error;

    lexer_restore_state(saved_lexer);
    parser_restore_state(saved_parser);
    parse_had_error = saved_had_error;

    if (!ok) return false;

    out_entry->code_offset = new_offset;
    out_entry->max_registers = max_registers;
    out_entry->max_raw_ints = max_raw_ints;
    out_entry->max_raw_reals = max_raw_reals;
    if (raw_param_count == 0) {
        /* Ordinary shape-only compile -- a freshly-created SpecEntry (see lbl_call, vm.c) needs its
           OWN raw-variant bookkeeping starting from a well-defined "never attempted" state (0),
           not whatever garbage sat in the caller's uninitialized stack SpecEntry otherwise. */
        out_entry->shape = shape;
        out_entry->kind = kind;
        out_entry->raw_param_count = 0;
    }
    return true;
}

/* Includes a `[]`/`{}` empty-container template special case -- vm_default_value allocates a
   fresh empty array/dict at each use, avoiding Python's mutable-default bug. Returns false if the
   current token isn't a valid literal default. */
static bool parse_literal_default(Chunk* c, AerVal* out, bool* out_narrow) {
    *out_narrow = false;
    bool negative = false;
    if (token.type == TOKEN_SUBTRACT) {
        /* Peeked, not consumed -- only actually a negative-number prefix if a number follows. */
        lex();
        negative = true;
    }
    if (token.type == TOKEN_INTEGER) {
        int64_t n = aer_as_int(token.value);
        *out = aer_int(negative ? -n : n);
        *out_narrow = token.narrow;
    } else if (token.type == TOKEN_REAL) {
        double d = aer_as_real(token.value);
        *out = aer_real(negative ? -d : d);
        *out_narrow = token.narrow;
    } else if (negative) {
        return false; /* '-' is only meaningful before a number */
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
        a->generation = 0;
        *out = aer_array_val(a);
    } else if (token.type == TOKEN_OPEN_BRACE) {
        lex();
        if (token.type != TOKEN_CLOSE_BRACE) return false;
        AerDict* d = vm_new_dict();
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
    if (!equal(TOKEN_IDENTIFIER)) {
        error_at("Expected struct name");
        return;
    }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();
    require(TOKEN_COLON, "expected ':' after struct name");
    require(TOKEN_NEW_LINE, "expected newline before indented struct body");
    require(TOKEN_INDENT, "expected indented struct body");
    if (parse_had_error) return;

    unsigned int field_names[MAX_STRUCT_FIELDS];
    AerVal field_defaults[MAX_STRUCT_FIELDS];
    ValueType field_types[MAX_STRUCT_FIELDS];
    bool field_narrow[MAX_STRUCT_FIELDS];
    unsigned int field_count = 0;

    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        if (!equal(TOKEN_IDENTIFIER)) {
            error_at("Expected field name");
            return;
        }
        if (field_count >= MAX_STRUCT_FIELDS) {
            error_at("Too many struct fields (max %d)", MAX_STRUCT_FIELDS);
            return;
        }

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
        bool narrow;
        if (!parse_literal_default(c, &dflt, &narrow)) {
            error_at("Struct field defaults must be a literal value");
            return;
        }
        field_names[field_count] = fname;
        field_defaults[field_count] = dflt;
        field_types[field_count] = aer_type(dflt) == TYPE_NULL ? TYPE_ANY : aer_type(dflt);
        /* Only integer/float can be narrow -- token.narrow can't be true for any other literal
           kind (see lex_number, lexer.c), but stay explicit about it rather than trust that by
           omission. */
        field_narrow[field_count] =
            narrow && (field_types[field_count] == TYPE_INTEGER || field_types[field_count] == TYPE_REAL);
        /* Caught here, at definition, rather than deferred to first construction -- every other
           construction-time int32 range check (vm_check_narrow_field_write, vm.c) trusts a field's
           OWN default without re-validating it (same "enforced once, trusted everywhere after"
           convention as an ordinary declared-type check), so an out-of-range default has to be
           rejected before that trust is established. */
        if (field_narrow[field_count] && field_types[field_count] == TYPE_INTEGER &&
            (aer_as_int(dflt) < INT32_MIN || aer_as_int(dflt) > INT32_MAX)) {
            error_at("Struct field '%.*s' default is out of range for a narrow (int32) field",
                     (int)aer_as_string(c->pool[fname])->length, aer_as_string(c->pool[fname])->data);
            return;
        }
        field_count++;

        if (!equal(TOKEN_DEDENT) && !equal(TOKEN_END_OF_FILE))
            require(TOKEN_NEW_LINE, "expected newline after struct field");
        if (parse_had_error) return;
    }
    consume(TOKEN_DEDENT);

    if (field_count == 0) {
        error_at("Struct must have at least one field");
        return;
    }

    chunk_emit(c, PACK_STRUCT_HEADER(name_idx, field_count));
    for (unsigned int i = 0; i < field_count; i++) {
        unsigned int default_idx = chunk_add_pool(c, field_defaults[i]);
        chunk_emit(c, PACK_2X16(field_names[i], default_idx));
        /* Low byte is the ValueType tag, bit 0x100 is the narrow (`i`/`f`-suffixed-literal) marker
           -- decoded by OP_DEFINE_STRUCT's handler (vm.c). */
        chunk_emit(c, (uint32_t)field_types[i] | (field_narrow[i] ? 0x100u : 0u));
    }

    struct_register(name_idx);
}

/* break/continue -- no iter_slots POPs needed (see LoopContext's own comment). */
static void parse_break(Chunk* c) {
    if (P.loop_depth == 0) {
        error_at("'break' outside loop");
        return;
    }
    LoopContext* ctx = &P.loop_stack[P.loop_depth - 1];
    if (ctx->patch_count >= BREAK_MAX) {
        error_at("Too many breaks in one loop (max %d)", BREAK_MAX);
        return;
    }
    chunk_emit(c, OP_JUMP);
    ctx->patches[ctx->patch_count++] = c->count;
    chunk_emit(c, 0); /* placeholder -- patched by loop_pop_and_patch once the loop ends */
}

static void parse_continue(Chunk* c) {
    if (P.loop_depth == 0) {
        error_at("'continue' outside loop");
        return;
    }
    LoopContext* ctx = &P.loop_stack[P.loop_depth - 1];
    chunk_emit(c, OP_JUMP);
    if (ctx->rotated) {
        if (ctx->continue_patch_count >= BREAK_MAX) {
            error_at("Too many continues in one loop (max %d)", BREAK_MAX);
            return;
        }
        ctx->continue_patches[ctx->continue_patch_count++] = c->count;
        chunk_emit(
            c,
            0); /* placeholder -- patched by loop_pop_and_patch_rotated once OP_ITER_RANGE_LOOP's own position is known */
    } else {
        chunk_emit(c, (int)ctx->top); /* known at compile time -- no patch needed */
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
    if (consume(TOKEN_IF)) {
        parse_if(c);
        return;
    }
    if (consume(TOKEN_FOR)) {
        parse_for_while(c);
        return;
    }
    if (consume(TOKEN_FUNCTION)) {
        parse_function(c);
        return;
    }
    if (consume(TOKEN_RETURN)) {
        parse_return(c);
        return;
    }
    if (consume(TOKEN_RAISE)) {
        parse_raise(c);
        return;
    }
    if (consume(TOKEN_STRUCT)) {
        parse_struct(c);
        return;
    }
    if (consume(TOKEN_BREAK)) {
        parse_break(c);
        return;
    }
    if (consume(TOKEN_CONTINUE)) {
        parse_continue(c);
        return;
    }
    if (consume(TOKEN_IMPORT)) {
        parse_import(c);
        return;
    }
    {
        const char* reserved_word = NULL;
        if (equal(TOKEN_TYPE_INTEGER))
            reserved_word = "integer";
        else if (equal(TOKEN_TYPE_FLOAT))
            reserved_word = "float";
        else if (equal(TOKEN_TYPE_BOOLEAN))
            reserved_word = "boolean";
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
        if (consume(TOKEN_OPEN_BRACKET)) {
            parse_chain_assignment(c, name_idx, true);
            return;
        }
        if (consume(TOKEN_DOT)) {
            parse_chain_assignment(c, name_idx, false);
            return;
        }
        parse_assignment(c, name_idx);
        return;
    }
    error_at("Expected a statement (only assignment, indexed/field writes, if/else, for-while, "
             "break/continue, function/struct defs, return, imports, and calls are supported)");
}

/* Isolates a fresh, independent program -- call exactly once per independent compile (a
   one-shot file run, a REPL session's startup, or one test case), never between statements of
   the same program. */
/* See parser.h for why this exists rather than tests reading a register index directly. */
bool parser_read_variable(VM* vm, Chunk* c, const char* name, AerVal* out) {
    for (int i = P.var_count - 1; i >= 0; i--) {
        AerVal entry = c->pool[P.var_names[i]];
        if (aer_type(entry) != TYPE_STRING) continue;
        AerString* s = aer_as_string(entry);
        if (strlen(name) != s->length || memcmp(s->data, name, s->length) != 0) continue;
        int slot = P.var_regs[i];
        switch (P.var_kind[i]) {
            case VAR_RAW_INT: *out = aer_int(vm->raw_ints[slot]); return true;
            case VAR_RAW_REAL: *out = aer_real(vm->raw_reals[slot]); return true;
            default: *out = vm->registers[slot]; return true;
        }
    }
    return false;
}

void parser_reset(void) {
    reg_reset();
    P.last_cmp_offset = NO_CMP_OFFSET;
    P.var_count = 0;
    P.global_count = 0;
    P.struct_count = 0;
    P.pending_count = 0;
    P.function_depth = 0;
    P.loop_depth = 0;
    parse_had_error = false;
    /* Function registrations live on the Chunk, not parser statics -- isolation follows each
       program's own fresh Chunk. */
}

/* A nested compile snapshots the whole Parser struct, starts P fresh, and restores afterward. Safe
   wholesale, including fields historically excluded: each is unconditionally reset before its own
   next real read, and the nested recompile path never observes what the snapshot carries for
   them. struct_names is the one exception -- see parser_restore_state. */
/* parser.h already forward-declares `struct ParserState` (an opaque handle for callers outside
   this file) -- a trivial one-field wrapper around Parser satisfies that without duplicating
   Parser's own field list a second time. */
struct ParserState {
    Parser p;
};

ParserState* parser_save_state(void) {
    ParserState* s = xmalloc(sizeof(ParserState));
    s->p = P;
    P = (Parser){
        0}; /* zeroes everything reg_reset() would, plus every other field -- see this function's own comment above */
    /* struct_names is the exception to "reset before its next read": struct definitions are a
       program-wide fact, never re-derived by a nested recompile, and a specialized body may
       construct any of them. With an empty table is_struct_name cannot tell `SomeStruct(...)` from
       an undefined function, and it compiles a call to whatever occupies index 0 -- reachable and
       memory-safety-adjacent. Carried by value so a nested compile's own registrations stay
       separate from the outer table. */
    if (s->p.struct_cap > 0) {
        P.struct_names = xmalloc(sizeof(unsigned int) * (size_t)s->p.struct_cap);
        memcpy(P.struct_names, s->p.struct_names, sizeof(unsigned int) * (size_t)s->p.struct_count);
        P.struct_cap = s->p.struct_cap;
    }
    P.struct_count = s->p.struct_count;
    return s;
}

/* Frees the nested compile's own heap arrays -- safe to free plainly, since each array holds
   only this parser's own scratch bookkeeping, never the runtime data already persisted onto the
   nested file's own Chunk. */
void parser_restore_state(ParserState* s) {
    free(P.pending_calls);
    free(P.struct_names);
    P = s->p;
    free(s);
}

/* Per-statement rollback so a REPL survives a mistake; persistent tables are left alone (see
   parser_reset). Anything still pending in the forward-reference list never got defined, and is
   reported at its original call site, then neutralized by overwriting the call's opcode word with
   OP_HALT -- patching only the jump target would still run the call's side effects. */
void parse(Chunk* c) {
    P.any_compile_error = false;
    while (!equal(TOKEN_END_OF_FILE)) {
        if (consume(TOKEN_NEW_LINE)) continue;
        if (consume(TOKEN_DEDENT)) continue;
        parse_had_error = false;
        P.recovered_at_boundary = false;
        unsigned int saved = c->count;
        unsigned int saved_marks = c->line_mark_count;
        chunk_mark_line(c, saved, current_source_line());
        parse_statement(c);
        if (parse_had_error) {
            P.any_compile_error = true;
            c->count = saved;
            c->line_mark_count = saved_marks;
            if (!P.recovered_at_boundary) {
                while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_NEW_LINE) && !equal(TOKEN_DEDENT))
                    lex();
                skip_orphaned_block();
            }
        }
    }
    if (P.pending_count > 0) {
        const char* real_cursor = current_source_cursor();
        bool any_pending_error = false;
        for (int i = 0; i < P.pending_count; i++) {
            unsigned int patch_offset = P.pending_calls[i].patch_offset;
            c->code[patch_offset - 1] = OP_HALT;
            lexer_set_cursor(P.pending_calls[i].call_site_cursor);
            error_at("Unknown function or struct type '%s' (never defined anywhere in this compile — not a "
                     "valid forward reference, module call, or struct construction target)",
                     aer_as_string(c->pool[P.pending_calls[i].name_idx])->data);
            any_pending_error = true;
        }
        lexer_set_cursor(real_cursor);
        P.pending_count = 0;
        if (any_pending_error) P.any_compile_error = true;
    }
    if (P.any_compile_error) parse_had_error = true;
}

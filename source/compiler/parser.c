#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include "aer_stdlib.h"
#include "error.h"
#include "lexer.h"

/* VAR_RAW_* is earned by a name whose first assignment is provably int/real outside any branch,
   and is lost on a mismatch; VAR_BOXED never promotes back. */
typedef enum { VAR_BOXED, VAR_RAW_INT, VAR_RAW_REAL } VarKind;

/* Nothing emitted yet at a tracked offset -- see Parser.last_cmp_offset / last_interp_offset. */
#define NO_OFFSET ((unsigned int)-1)

/* A call to a name not yet registered, patched once parse() has seen the whole file. */
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

/* A literal in a loop would reload every iteration, since whatever consumes its slot clobbers it.
   Each loop reserves a preheader gap up front and fills it once the body is known; HOIST_MAX loads
   fill the gap exactly, so a loop using fewer skips the rest with one OP_JUMP. */
#define HOIST_MAX 4
#define HOIST_GAP_WORDS (HOIST_MAX * 2)
typedef struct {
    unsigned int gap_offset;
    int count;
    bool is_int[HOIST_MAX];
    bool from_pool[HOIST_MAX]; /* int too wide for OP_RAW_LOAD_INT's int32 immediate */
    int64_t value[HOIST_MAX]; /* ints: the value itself, so dedup never depends on pool identity */
    unsigned int rawk_idx[HOIST_MAX];
    int slot[HOIST_MAX];
    /* Both runs are claimed up front rather than one slot at a time on first use. A first use sits
       part-way through an expression, where an integer claim would raise the floor between two of a
       call's argument registers and break the contiguous run OP_CALL needs, and either kind would
       take a temp the loop's own body hands back and re-uses every iteration. */
    int slot_base; /* -1 when the frame had no room, in which case no integer hoists */
    int int_count;
    int saved_floor, raised_floor;
    int real_base; /* highest of this loop's real run, counting down; -1 when there was no room */
    int real_count;
    int saved_real_floor, lowered_real_floor;
} LoopHoist;

typedef enum { RAWK_NONE, RAWK_INT, RAWK_REAL } RawKind;

/* Every mutable global the compile functions share. P (below) is the live instance;
   parser_save_state/restore_state snapshot it wholesale for a nested compile. */
typedef struct Parser {
    /* slot_next is the next free temp, slot_floor the line below which slots belong to variables.
   slot_max is read back after a body compiles, before its restore, since both shrink again. */
    int slot_next;
    int slot_floor;
    int slot_max;

    /* Real-typed slots, allocated DOWNWARD from FRAME_REGISTERS so every slot they reach holds a
       real for the function's life (see vm.h). Variables and loop-hoisted constants claim down to
       raw_real_floor, temps down to raw_real_next -- which is also the line the ordinary allocator
       must not cross. raw_real_low is the lowest ever reached, and says whether the body needs a
       full-size frame. */
    int raw_real_next, raw_real_floor, raw_real_low;

    /* Every register's current variable binding (0 registers is effectively local -- see
       var_kind below for storage-kind tracking). */
    unsigned int var_names[FRAME_REGISTERS];
    int var_regs[FRAME_REGISTERS];
    int var_count;
    VarKind var_kind[FRAME_REGISTERS];

    /* Which of this function's parameters were used as the base of a struct-field access, directly or
   through a one-hop alias. See mark_shape_sensitive for how the three work together. */
    bool shape_sensitive_param[FRAME_REGISTERS];
    int current_param_count;
    int alias_source_param[FRAME_REGISTERS]; /* -1 = no known alias */
    /* Set only during a specialization recompile, and read at field-access sites to skip the generic
   runtime resolution. */
    Shape* reg_known_shape[FRAME_REGISTERS];
    /* Proven >= 0: a non-negative literal, a bounded loop index, a length(), or those combined with
   + * // %. Wrong in one direction only -- a false positive is caught at runtime by
   OP_ITER_RANGE_PREP's own guard, so it can never be a safety hole. */
    bool reg_nonneg[FRAME_REGISTERS];
    Shape* reg_known_element_shape[FRAME_REGISTERS];
    /* Element kind of a typed array a register holds, from a `[numeric; count]` literal -- a
   parse-time fact, so the read can go straight to a raw slot. */
    RawKind reg_elem_kind[FRAME_REGISTERS];

    /* Two arrays built from ONE count value are the same length. value_class identifies what a
   register currently holds and changes on every write to it; len_class records the count's
   value_class at construction, so equal nonzero len_classes mean equal lengths. Read only by
   try_vectorize_reduction, where it is a correctness precondition rather than an optimization:
   folding a loop over several columns into one whole-array pass is only the same work when they
   really are the same length. */
    int reg_value_class[FRAME_REGISTERS];
    int reg_len_class[FRAME_REGISTERS];
    int next_value_class;
    /* Where the first argument of the call being parsed stopped emitting. Only a call that folds
       one argument and reads the rest as they are needs it -- collection.group_sum, whose values
       fuse but whose group column and group count do not. */
    unsigned int first_arg_end;
    /* Side channel from the index-get site to parse_assignment, keyed on exact register equality
   rather than a flag. */
    int last_plain_index_dest_reg;
    int last_plain_index_src_param;
    Shape* last_plain_index_known_elem_shape;

    /* Highest temp register reached while compiling the loop condition currently being parsed --
       see parse_for_body for why the body must not be allowed to claim one of these. */
    int loop_cond_peak;

    /* The last comparison emitted, so emit_cond_jump_if_false can tell one from a word that merely
   looks like one -- instruction lengths vary, so it cannot be found by reading backwards. */
    unsigned int last_cmp_offset;

    /* The last raw arithmetic emitted, so an assignment can retarget it at the variable's own slot
   instead of following it with a move. patch_epoch counts backpatches: a jump landing between the
   two would make the arithmetic conditional when the move was not. */
    unsigned int raw_write_offset;
    int raw_write_slot;
    RawKind raw_write_kind;
    unsigned int raw_write_epoch;
    unsigned int patch_epoch;

    /* Raw-vs-boxed opcodes reached for in this body. Nonzero means binding its numeric parameters raw
   would turn real work raw, which is the trigger for a numeric specialization. */
    unsigned int raw_boxed_emits;

    /* Index of the function whose body is compiling, and whether that body calls itself. A
       recursive numeric function is the one shape where binding parameters raw loses: its raw
       values exist only to be re-boxed as the next call's arguments (see SHAPE_MASK_NUMERIC_ONLY,
       vm.h). -1 outside any function body. */
    int current_func_idx;
    bool self_call_seen;
    /* Set only while a SPECIALIZED body is compiling, so a self-call inside it can skip resolution
       (OP_CALL_SELF, vm.h). A generic body must not: resolution is what triggers specialization in
       the first place, so bypassing it there means the variant is never compiled at all. */
    bool in_variant;

    /* Same trick for the last OP_INTERP: emit_index_get folds one into OP_INDEX_GET_INTERP when the
       interpolation it is indexing with is the instruction immediately before it. */
    unsigned int last_interp_offset;
    int last_interp_dest;

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

    /* Range-for loop variables currently in scope, innermost last, with whether the loop's own body
       ever writes one. A body that never does lets OP_ITER_RANGE_LOOP carry its counter IN the loop
       variable's register rather than a second one -- see parse_for_in's fused_counter. */
    int range_item_regs[LOOP_MAX];
    bool range_item_written[LOOP_MAX];
    int range_loop_depth;

    LoopHoist hoist_stack[LOOP_MAX];
    int hoist_depth;

    /* Top-level variable names, kept solely to detect a function body referencing one -- see
       var_names's own original comment (git blame) for the shadow-ban mechanism. */
    unsigned int global_names[FRAME_REGISTERS];
    int global_regs[FRAME_REGISTERS];
    int global_count;

    /* The assignment target's own name, so var_lookup_rk can flag a self-reference however deep in
       the RHS it sits: `total = total + x` cannot shadow mid-loop, `total = f()` can. */
    unsigned int self_ref_watch_name;
    bool self_ref_watch_seen;

    /* Forward-referenced calls not yet resolved to a real function -- see PendingCall's own
       comment above. */
    PendingCall* pending_calls;
    int pending_count, pending_cap;

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

/* Overflow returns -1 and the caller falls back to a dynamically typed value. reg_alloc errors
   instead, because nothing is below it to fall back to. */
static void track_peak(int v) {
    if (v > P.slot_max)
        P.slot_max = v;
}

/* True once a real slot has been claimed, which is also what forces a full-size frame. */
static bool raw_real_used(void) {
    return P.raw_real_low < FRAME_REGISTERS;
}

static void raw_track_low(int v) {
    if (v < P.raw_real_low)
        P.raw_real_low = v;
}

static int slot_alloc(RawKind kind) {
    if (kind == RAWK_INT) {
        if (P.slot_next >= P.raw_real_next)
            return -1;
        track_peak(P.slot_next + 1);
        return P.slot_next++;
    }
    if (P.raw_real_next - 1 < P.slot_max)
        return -1;
    int slot = --P.raw_real_next;
    raw_track_low(slot);
    P.reg_nonneg[slot] = false; /* same as reg_alloc: no proof carries over from the last occupant */
    return slot;
}

static void slot_free(RawKind kind, int count) {
    if (kind == RAWK_INT) {
        P.slot_next -= count;
        if (P.slot_next < P.slot_floor)
            P.slot_next = P.slot_floor;
        return;
    }
    P.raw_real_next += count;
    if (P.raw_real_next > P.raw_real_floor)
        P.raw_real_next = P.raw_real_floor;
}

/* Claims a permanent slot for a variable's first assignment, the analog of var_slot claiming the
   boxed floor. */
static int slot_reserve_one(RawKind kind) {
    if (kind == RAWK_INT) {
        if (P.slot_floor >= P.raw_real_next)
            return -1;
        int slot = P.slot_floor++;
        P.slot_next = P.slot_floor;
        track_peak(P.slot_floor);
        return slot;
    }
    if (P.raw_real_floor - 1 < P.slot_max)
        return -1;
    int slot = --P.raw_real_floor;
    P.raw_real_next = P.raw_real_floor;
    raw_track_low(slot);
    return slot;
}

/* Gives back the slot slot_reserve_one just claimed, for a caller that finds the value it wanted to
   put there can't be built. */
static void raw_unreserve_one(RawKind kind) {
    if (kind == RAWK_INT) {
        P.slot_floor--;
        P.slot_next = P.slot_floor;
        return;
    }
    P.raw_real_floor++;
    P.raw_real_next = P.raw_real_floor;
}

/* True for a scratch slot, false for one a variable or a hoisted constant owns -- the test every
   "release this if it was scratch" site needs. Reals live above raw_real_floor's own region and
   integers are ordinary registers, so which end a slot came from tells which rule applies. */
static bool raw_is_temp(int slot) {
    if (slot >= P.raw_real_next)
        return slot < P.raw_real_floor;
    return slot >= P.slot_floor;
}

static void raw_region_open(void) {
    P.raw_real_next = P.raw_real_floor = P.raw_real_low = FRAME_REGISTERS;
}

void reg_reset(void) {
    P.hoist_depth = 0;
    P.slot_next = P.slot_floor = P.slot_max = 0;
    raw_region_open();
}

/* Must refuse a register >= FRAME_REGISTERS -- the register_stack bank is sized assuming no
   frame ever needs more. Returns an in-bounds sentinel after erroring. */
void reg_reserve(int count) {
    if (P.slot_floor + count > P.raw_real_next) {
        return error_at("Too many live variables/temporaries (max %d registers per call)", FRAME_REGISTERS);
    }
    P.slot_floor += count;
    P.slot_next += count;
    track_peak(P.slot_next);
}

int reg_alloc(void) {
    if (P.slot_next >= P.raw_real_next) {
        error_at("Too many live variables/temporaries (max %d registers per call)", FRAME_REGISTERS);
        return FRAME_REGISTERS - 1;
    }
    int reg = P.slot_next++;
    P.reg_nonneg[reg] = false; /* a recycled register carries no proof from its last occupant */
    P.reg_elem_kind[reg] = RAWK_NONE;
    track_peak(P.slot_next);
    if (P.slot_next > P.loop_cond_peak)
        P.loop_cond_peak = P.slot_next;
    return reg;
}

void reg_free(int count) {
    P.slot_next -= count;
    /* Never free below the reserved floor -- a bug elsewhere shouldn't hand out a local's own
       register as a free temp. */
    if (P.slot_next < P.slot_floor)
        P.slot_next = P.slot_floor;
}

#ifdef AER_CHECKED
/* Registers below reserved_floor are variables; at or above it they are the temp allocator's to
   hand out. Breaking that hands a live variable's register to the next expression as scratch --
   the failure mode is a variable silently reading back some unrelated intermediate value, never a
   crash. Compiled only into the sanitiser builds, which run the whole corpus in CI. */
static void assert_variables_below_floor(const char* where) {
    for (int i = 0; i < P.var_count; i++) {
        /* A real-typed variable is exempt: it lives at the TOP of the frame, deliberately above
           everything the ordinary allocator hands out (see Parser.raw_real_next). */
        if (P.var_kind[i] == VAR_RAW_REAL && P.var_regs[i] >= P.raw_real_floor)
            continue;
        if (P.var_kind[i] != VAR_BOXED)
            if (P.var_regs[i] >= P.slot_floor) {
                fprintf(stderr,
                        "aer: internal error: %s left variable slot %d in register %d, at or above the "
                        "reserved floor %d\n",
                        where, i, P.var_regs[i], P.slot_floor);
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

static int drop_raw_marks(int rk);

/* Every binary-operator emission funnels through here. Boxes any raw-flagged operand first
   (a no-op for a plain register or constant) -- only parse_binary_ops's own raw-composing path
   (try_emit_binary_raw) tries the native route before reaching here. */
static int materialize(Chunk* c, int rk);
static bool is_temp(int rk);
static void release_if_top(int rk);
static int drop_raw_marks(int rk);

/* True when this operand is provably >= 0 -- see Parser.reg_nonneg. Raw slots are deliberately
   not tracked; they take the ordinary path. */
static bool rk_nonneg(Chunk* c, int rk) {
    if (rk & RK_CONST_FLAG) {
        AerVal v = c->pool[rk & ~RK_CONST_FLAG];
        return aer_type(v) == TYPE_INTEGER && aer_as_int(v) >= 0;
    }
    /* A statically-typed operand is an ordinary register, so its proof is read the same way. */
    int reg = drop_raw_marks(rk);
    return reg >= 0 && reg < FRAME_REGISTERS && P.reg_nonneg[reg];
}

/* Non-negativity survives + * // and %, and only those: subtraction and left-shift can produce a
   negative from non-negative operands. Overflow can too, which is why the range-for guard exists
   rather than trying to reason about it here. */
/* Every binary operator whose result is >= 0 whenever both operands are, which is the whole set --
   not a sampling of it. The three exclusions are exclusions on their merits: SUB goes negative,
   LSHIFT can shift a positive value into the sign bit, and DIV yields a real rather than an index. */
static bool binop_preserves_nonneg(Opcode op) {
    return op == OP_ADD || op == OP_MUL || op == OP_FLOOR_DIV || op == OP_MOD || op == OP_BITWISE_AND ||
           op == OP_BITWISE_OR || op == OP_BITWISE_XOR || op == OP_RSHIFT;
}

/* A plain register below the parameter count -- parameters occupy the frame's first registers. */
static bool rk_param(int rk) {
    return !(rk & RK_CONST_FLAG) && drop_raw_marks(rk) < P.current_param_count;
}

static void emit_binary(Chunk* c, int dest, Opcode op, int rk_lhs, int rk_rhs) {
    rk_lhs = drop_raw_marks(rk_lhs);
    rk_rhs = drop_raw_marks(rk_rhs);
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
    if (dest >= 0 && dest < FRAME_REGISTERS)
        P.reg_nonneg[dest] = binop_preserves_nonneg(op) && rk_nonneg(c, rk_lhs) && rk_nonneg(c, rk_rhs);
    P.last_cmp_offset = c->count;
    chunk_emit(c, PACK3(op, dest, pack_rk8(rk_lhs), pack_rk8(rk_rhs)));
    /* A parameter reaching the fully boxed path is the clearest sign binding it raw would pay --
       and the only sign at all for a body with no raw local for the _BOXED family to catch. Only
       arithmetic and ordering: equality and `in` are defined on every type, so they say nothing
       about whether the operand is a number. */
    if ((op <= OP_FLOOR_DIV || (op >= OP_LT && op <= OP_GTE)) && (rk_param(rk_lhs) || rk_param(rk_rhs)))
        P.raw_boxed_emits++;
    if (spilled)
        reg_free(spilled);
}

/* Emits the branch-on-false half of an if/while condition, fusing a bare comparison directly with
   the branch instead of materializing a boolean just to read it back. Covers the plain-boxed and
   raw-boxed-int families only; the other three measured no benefit against the per-opcode
   branch-prediction cost (see vm.h). Detected by inspecting what was just compiled and rolling it
   back, so anything else -- and/or, a bare boolean, a spilled operand -- falls through. */
static unsigned int emit_cond_jump_if_false(Chunk* c, int rk_cond, unsigned int cond_start) {
    /* The comparison need only be the LAST instruction of the condition, not the whole of it:
       `x * x + y * y > 4.0` computes into raw slots first, and all of that is kept. */
    unsigned int cmp_word_start = cond_start;
    if (P.last_cmp_offset != NO_OFFSET && P.last_cmp_offset >= cond_start &&
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
            case OP_RAW_LT_INT: fused_op = OP_RAW_LT_INT_JUMP_IF_FALSE; break;
            case OP_RAW_LTE_INT: fused_op = OP_RAW_LTE_INT_JUMP_IF_FALSE; break;
            case OP_RAW_EQ_INT: fused_op = OP_RAW_EQ_INT_JUMP_IF_FALSE; break;
            case OP_RAW_NEQ_INT: fused_op = OP_RAW_NEQ_INT_JUMP_IF_FALSE; break;
            case OP_RAW_LT_REAL: fused_op = OP_RAW_LT_REAL_JUMP_IF_FALSE; break;
            case OP_RAW_LTE_REAL: fused_op = OP_RAW_LTE_REAL_JUMP_IF_FALSE; break;
            case OP_RAW_EQ_REAL: fused_op = OP_RAW_EQ_REAL_JUMP_IF_FALSE; break;
            case OP_RAW_NEQ_REAL: fused_op = OP_RAW_NEQ_REAL_JUMP_IF_FALSE; break;
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
    release_if_top(reg_cond);
    return patch;
}

/* The rotated back-edge branches on the complement of the entry guard: "leave if !(i < n)" becomes
   "go round again if i < n", spelled as jump-if-false of `n <= i`.

   Ordering tests complement by swapping their operands, which is why reals are excluded: against a
   NaN both `a < b` and `b <= a` are false, so the guard would fall through and the back-edge would
   branch -- the loop would never end. Equality complements exactly at any type. */
static bool complement_branch(Opcode op, Opcode* out, bool* swap) {
    *swap = false;
    switch (op) {
        case OP_RAW_LT_INT_JUMP_IF_FALSE: *out = OP_RAW_LTE_INT_JUMP_IF_FALSE, *swap = true; return true;
        case OP_RAW_LTE_INT_JUMP_IF_FALSE: *out = OP_RAW_LT_INT_JUMP_IF_FALSE, *swap = true; return true;
        case OP_RAW_EQ_INT_JUMP_IF_FALSE: *out = OP_RAW_NEQ_INT_JUMP_IF_FALSE; return true;
        case OP_RAW_NEQ_INT_JUMP_IF_FALSE: *out = OP_RAW_EQ_INT_JUMP_IF_FALSE; return true;
        case OP_RAW_EQ_REAL_JUMP_IF_FALSE: *out = OP_RAW_NEQ_REAL_JUMP_IF_FALSE; return true;
        case OP_RAW_NEQ_REAL_JUMP_IF_FALSE: *out = OP_RAW_EQ_REAL_JUMP_IF_FALSE; return true;
        default: return false;
    }
}

unsigned int emit_jump_if_false_reg(Chunk* c, int reg) {
    chunk_emit(c, PACK1(OP_JUMP_IF_FALSE_REG, reg));
    unsigned int patch_offset = c->count;
    chunk_emit(c, 0);
    return patch_offset;
}

/* Control-flow targets are a signed delta from the word AFTER the operand -- exactly where pc sits
   once the VM has READ() it -- so a branch is `pc += delta` and never rebuilds `code`. Every
   jump-like operand in the instruction set is the last word read before its branch, which is what
   lets one base serve them all. */
void patch_jump(Chunk* c, unsigned int patch_offset, unsigned int target) {
    c->code[patch_offset] = (uint32_t)(int32_t)((int64_t)target - (int64_t)patch_offset - 1);
    P.patch_epoch++;
}

/* A call's callee_offset is an absolute function entry, not intra-function control flow. */
void patch_call_target(Chunk* c, unsigned int patch_offset, unsigned int target) {
    c->code[patch_offset] = (uint32_t)target;
}

/* Emits an already-known jump target in the same encoding patch_jump writes. */
void emit_jump_target(Chunk* c, unsigned int target) {
    chunk_emit(c, (uint32_t)(int32_t)((int64_t)target - (int64_t)c->count - 1));
}

/* Returns the callee_offset word's offset for a forward-referencing call to patch later
   (pending_call_add); an already-resolved call ignores the return value. func_index is the
   target's index into chunk->functions[] -- lbl_call reads it to size the callee's frame from its
   real max_registers peak instead of a flat, function-agnostic ceiling. */
unsigned int emit_call(Chunk* c, int dest_reg, unsigned int callee_offset, int arg_reg_base, int arg_count,
                       unsigned int func_index) {
    if ((int)func_index == P.current_func_idx)
        P.self_call_seen = true;
    chunk_emit(c, PACK3(OP_CALL, dest_reg, arg_reg_base, arg_count));
    unsigned int patch_offset = c->count;
    chunk_emit(c, (uint32_t)callee_offset);
    /* A BYTE offset, not an index: ChunkFunction is 328 bytes, so `&functions[i]` compiled to a
       multiply on every single call. The scale is known here and never at runtime. */
    chunk_emit(c, (uint32_t)(func_index * sizeof(ChunkFunction)));
    return patch_offset;
}

/* 0 for a boxed return; mixing kinds also collapses to 0. */

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

/* `h["key_{n}"]` builds a string only to hash it and drop it. When the index is an interpolation
   this emitter just produced, fold the two into one opcode that hashes the bytes directly. Detected
   by rollback like emit_cond_jump_if_false, so anything else falls through untouched. */
static bool try_fuse_index_get_interp(Chunk* c, int dest_reg, int arr_reg, int rk_idx) {
    if (P.last_interp_offset == NO_OFFSET || rk_idx != P.last_interp_dest)
        return false;
    if (arr_reg == rk_idx)
        return false; /* the receiver is what we are about to stop writing */
    uint32_t w = c->code[P.last_interp_offset];
    if ((Opcode)(w & 0xFF) != OP_INTERP || (int)UNPACK_A(w) != rk_idx)
        return false;
    unsigned int parts = UNPACK_B(w);
    if (P.last_interp_offset + 1 + parts != c->count)
        return false; /* not the immediately preceding instruction */

    uint32_t operands[INTERP_MAX_PARTS];
    for (unsigned int i = 0; i < parts; i++)
        operands[i] = c->code[P.last_interp_offset + 1 + i];
    c->count = P.last_interp_offset;
    chunk_emit(c, PACK3(OP_INDEX_GET_INTERP, dest_reg, arr_reg, (int)parts));
    for (unsigned int i = 0; i < parts; i++)
        chunk_emit(c, operands[i]);
    P.last_interp_offset = NO_OFFSET;
    return true;
}

void emit_index_get(Chunk* c, int dest_reg, int arr_reg, int rk_idx) {
    if (try_fuse_index_get_interp(c, dest_reg, arr_reg, rk_idx))
        return;
    rk_idx = drop_raw_marks(rk_idx);
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
    rk_idx = drop_raw_marks(rk_idx);
    /* Mirror of the raw read: a raw value stores straight from its slot when the array's element
       kind matches, instead of boxing only for vm_index_set_compute to unbox again. Bounds-checked,
       so no loop proof is needed. */
    RawKind elem = (arr_reg >= 0 && arr_reg < FRAME_REGISTERS) ? P.reg_elem_kind[arr_reg] : RAWK_NONE;
    RawKind val_kind = (rk_val & RK_RAW_INT_FLAG)    ? RAWK_INT
                       : (rk_val & RK_RAW_REAL_FLAG) ? RAWK_REAL
                                                     : RAWK_NONE;
    if (elem != RAWK_NONE && elem == val_kind && rk8_fits(rk_idx)) {
        int slot = rk_val & RK_RAW_SLOT_MASK;
        chunk_emit(c, PACK3(elem == RAWK_INT ? OP_INDEX_SET_RAW_INT : OP_INDEX_SET_RAW_REAL, arr_reg,
                            pack_rk8(rk_idx), slot));
        release_if_top(slot);
        return;
    }
    rk_val = drop_raw_marks(rk_val);
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
    if (spilled)
        reg_free(spilled);
}

/* rk_start/rk_end are RK-encoded like any value operand; a missing bound is an RK-encoded
   null constant built by the caller. RK16 (32767 direct) is generous enough that no hoist is
   needed here in practice, but guard anyway rather than silently corrupt. */
void emit_slice_get(Chunk* c, int dest_reg, int arr_reg, int rk_start, int rk_end) {
    rk_start = drop_raw_marks(rk_start);
    rk_end = drop_raw_marks(rk_end);
    if (!rk16_fits(rk_start) || !rk16_fits(rk_end)) {
        return error_at(
            "Expression too large to compile (register/constant index exceeds the slice-get encoding's "
            "range)");
    }
    chunk_emit(c, PACK3(OP_SLICE_GET, dest_reg, arr_reg, 0));
    chunk_emit(c, PACK_2X16(pack_rk16(rk_start), pack_rk16(rk_end)));
}

void emit_dict_new(Chunk* c, int dest_reg, int pair_reg_base, int pair_count) {
    chunk_emit(c, PACK3(OP_DICT_NEW, dest_reg, pair_reg_base, pair_count));
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
/* guard_nonneg rides in the item_dest word's spare high bits -- the register index needs 8 of its 32.
   Set when the body was compiled with unchecked indexing on a proven non-negative start, so PREP
   re-checks that once at loop entry; see RANGE_PREP_GUARD_NONNEG (vm.h). */
unsigned int emit_iter_range_prep(Chunk* c, int cur_reg, int end_reg, int step_reg, int item_dest_reg,
                                  bool guard_nonneg) {
    chunk_emit(c, PACK3(OP_ITER_RANGE_PREP, cur_reg, end_reg, step_reg));
    chunk_emit(c, (uint32_t)item_dest_reg | (guard_nonneg ? RANGE_PREP_GUARD_NONNEG : 0u));
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
    emit_jump_target(c, body_target);
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
    rk_val = drop_raw_marks(rk_val);
    if (!rk16_fits(rk_val)) {
        return error_at(
            "Expression too large to compile (register/constant index exceeds the field-set encoding's "
            "range)");
    }
    chunk_emit(c, PACK_OP_A_W16(OP_FIELD_SET, struct_reg, pack_rk16(rk_val)));
    chunk_emit(c, (uint32_t)field_name_pool_idx);
}

/* Recursive-descent compiler: the full grammar, from the lexer's token stream straight to
   register bytecode. */

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
static int parse_contiguous_exprs(Chunk* c, TokenType close_tok, int* out_base, bool* out_base_is_temp);
static void parse_function_signature(Chunk* c, unsigned int* param_names, AerVal* param_defaults,
                                     int* out_param_count, int* out_min_param_count);
static void parse_function_body(Chunk* c, unsigned int* param_names, int param_count, int hint_param_reg,
                                Shape* hint_shape, bool hint_is_element_shape, const int* raw_param_regs,
                                const ValueType* raw_param_types, int raw_param_count,
                                unsigned int* out_max_registers, unsigned short* out_frame_bounds);

/* reg is the parameter's own register (0..P.current_param_count-1) OR a register whose value is
   known (via P.alias_source_param) to have come from indexing that parameter -- either way, marks
   that parameter shape-sensitive. Safe to call with any register (out-of-range/no-alias is a
   silent no-op), matching var_lookup_rk's own "harmless on a miss" convention. See the Parser
   struct's own field comments (top of file) for what each of these tables tracks. */
static void mark_shape_sensitive(int reg) {
    if (reg < 0 || reg >= FRAME_REGISTERS)
        return;
    if (reg < P.current_param_count) {
        P.shape_sensitive_param[reg] = true;
        return;
    }
    int src = P.alias_source_param[reg];
    if (src >= 0)
        P.shape_sensitive_param[src] = true;
}

/* A nonzero id for the value an operand holds right now. Two reads of one untouched register, or of
   one pool constant, give the same id; any write to that register gives a fresh one. */
static int value_class_of(int rk) {
    if (rk & RK_CONST_FLAG)
        return -((rk & ~RK_CONST_FLAG) + 1);
    int reg = drop_raw_marks(rk);
    if (reg < 0 || reg >= FRAME_REGISTERS)
        return 0;
    if (P.reg_value_class[reg] == 0)
        P.reg_value_class[reg] = ++P.next_value_class;
    return P.reg_value_class[reg];
}

/* True iff (arr_reg, idx_rk) matches a pair on the safe_loop_item/array_regs stack. Both halves
   must match: a bound proven for array A must never be trusted for a different array B that
   happens to reuse the same index register. idx_rk must be a plain register.
   Packed-array field callers additionally require arr_reg == P.hint_param_reg (the field offset
   is only valid for that one specialized parameter); typed-array callers do not. */
static bool index_safe_unchecked(int arr_reg, int idx_rk) {
    if (idx_rk & RK_CONST_FLAG)
        return false;
    /* The static-type marks name a register now, not a separate bank, so a typed index is still an
       ordinary register and this proof is keyed on register identity. Rejecting it outright left
       sieve's marking loop on the generic OP_INDEX_SET the moment its loop variable became typed. */
    int idx_reg = drop_raw_marks(idx_rk);
    for (int i = 0; i < P.safe_loop_depth; i++) {
        if (P.safe_loop_item_regs[i] == idx_reg && P.safe_loop_array_regs[i] == arr_reg)
            return true;
    }
    return false;
}

/* Poisons what a write invalidates about a slot's CONTENTS, keyed on reg as either the index or
   the array half -- the _UNCHECKED opcodes have no runtime check to fall back on. Entries are
   overwritten with -1, not removed, so parse_for_in's push/pop depth counting is untouched.
   Deliberately leaves reg_nonneg alone; see ARCHITECTURE 5.58 for what clearing it cost. */
static void note_slot_written(int reg) {
    if (reg < 0)
        return;
    if (P.length_tracked_valid && reg == P.length_tracked_source_reg)
        P.length_tracked_valid = false;
    if (reg < FRAME_REGISTERS) {
        P.reg_elem_kind[reg] = RAWK_NONE;
        /* Whatever it held is gone, so it is no longer that array; and a count read from it after
           this is a different value, which must not match one read before. */
        P.reg_len_class[reg] = 0;
        P.reg_value_class[reg] = ++P.next_value_class;
    }
    for (int i = 0; i < P.safe_loop_depth; i++) {
        if (P.safe_loop_item_regs[i] == reg)
            P.safe_loop_item_regs[i] = -1;
        if (P.safe_loop_array_regs[i] == reg)
            P.safe_loop_array_regs[i] = -1;
    }
    for (int i = 0; i < P.range_loop_depth; i++)
        if (P.range_item_regs[i] == reg)
            P.range_item_written[i] = true;
}

/* note_slot_written plus the non-negativity proof, for writes that could produce any value. */
static void invalidate_register(int reg) {
    if (reg < 0)
        return;
    if (reg < FRAME_REGISTERS)
        P.reg_nonneg[reg] = false;
    note_slot_written(reg);
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
    if (P.function_depth == 0)
        return false;
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

/* P.slot_floor, not P.var_count: a for-loop promotes the floor for its own iteration registers
   without registering a name, so a variable declared in its body must land above them or it aliases
   the loop's state. P.var_regs[] keeps lookup correct once the two diverge. */
static int var_slot(Chunk* c, unsigned int name_idx) {
    for (int i = 0; i < P.var_count; i++)
        if (P.var_names[i] == name_idx)
            return P.var_regs[i];
    /* Shadowing a top-level name is a compile error, not a silent fresh local -- catches both a
       fresh local and a parameter with that name. P.function_depth == 0 (the definition itself)
       and an already-local name are exempt. */
    if (report_if_shadowed_global(c, name_idx))
        return -1;
    /* A reserved name always resolves to its builtin at the call site, so binding one produced a
       variable that could never be read -- `print = 5` silently did nothing. */
    if (is_builtin_name(c, name_idx)) {
        error_at("'%s' is a reserved function name and can't be used as a variable",
                 aer_as_string(c->pool[name_idx])->data);
        return -1;
    }
    /* Checks P.slot_floor, not P.var_count -- P.var_count can lag behind P.slot_floor once a
       for-loop promotes it (see this function's own comment above). */
    if (P.slot_floor >= P.raw_real_next) {
        error_at("Too many variables (max %d)", FRAME_REGISTERS);
        return -1;
    }
    int reg = P.slot_floor;
    P.var_names[P.var_count] = name_idx;
    P.var_regs[P.var_count] = reg;
    /* P.var_kind[] persists across every function's compilation and save/restore only shrinks
       P.var_count, so an earlier function's local leaves a stale kind at this index. Reset here,
       the one place every name is created. */
    P.var_kind[P.var_count] = VAR_BOXED;
    P.var_count++;
    P.slot_floor++; /* permanently protects this register from the temp allocator */
    P.slot_next = P.slot_floor; /* resync -- see this function's own comment for why that's always safe */
    track_peak(P.slot_floor);
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

/* Works for any RK operand: a dynamically typed temp lives at/above P.slot_floor, a variable of that
   kind below it, and the whole statically-typed block below that -- so this is false for a raw slot,
   which raw_is_temp answers for instead. */
static bool is_temp(int rk) {
    if (rk & RK_CONST_FLAG)
        return false;
    return drop_raw_marks(rk) >= P.slot_floor;
}

/* Drops the static-type marks so the rest of the compiler sees a plain register. Nothing is emitted:
   an unchecked opcode leaves a fully tagged value behind, so a slot the parser knows the type of is
   already a valid operand to one that does not. */
static int drop_raw_marks(int rk) {
    return rk & ~(RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG);
}

/* Releases a value's slot, but only when it really is the top of the stack -- the temp a caller
   wants to release is not always the most recent allocation, and popping blind hands the next
   allocation a slot another live operand still holds. Declining to free costs at most one slot until
   the statement ends. */
static void release_if_top(int rk) {
    if (is_temp(rk) && drop_raw_marks(rk) == P.slot_next - 1)
        reg_free(1);
}

/* Same, for a slot holding a statically-typed value -- a real from the top of the frame, or an
   integer from an ordinary register. Reals grow downward, so their top of stack is the low end. */
static void raw_release_if_top(int slot) {
    if (!raw_is_temp(slot))
        return;
    if (slot >= P.raw_real_next && slot < P.raw_real_floor) {
        if (slot == P.raw_real_next)
            slot_free(RAWK_REAL, 1);
    } else if (slot == P.slot_next - 1) {
        slot_free(RAWK_INT, 1);
    }
}

/* Forgets a name's known type, which var_slot has no awareness of -- required before writing a
   non-raw value to an existing name. */
static void ensure_boxed(unsigned int name_idx) {
    for (int i = 0; i < P.var_count; i++) {
        if (P.var_names[i] != name_idx)
            continue;
        /* Moving off the real slot is the point: those hold a real for the frame's life, and a
           dynamically typed write would leave a tag the unchecked opcodes keep. Nothing is copied
           out -- every caller overwrites the variable. */
        if (P.var_kind[i] == VAR_RAW_REAL) {
            if (P.slot_floor >= P.raw_real_next)
                return error_at("Too many variables (max %d)", FRAME_REGISTERS);
            P.var_regs[i] = P.slot_floor++;
            P.slot_next = P.slot_floor;
            track_peak(P.slot_floor);
        }
        P.var_kind[i] = VAR_BOXED;
        return;
    }
}

/* Materializes a constant via OP_LOADK, or boxes a raw value -- OP_JUMP_IF_FALSE_REG needs an
   actual register, no RK/raw form. */
static int materialize(Chunk* c, int rk) {
    rk = drop_raw_marks(rk);
    if (!(rk & RK_CONST_FLAG))
        return rk;
    int reg = reg_alloc();
    /* dest+pool_idx both fit word0 now (op(8)+dest(8)+pool_idx(16)) -- no trailing word. */
    chunk_emit(c, PACK_OP_A_W16(OP_LOADK, reg, (unsigned int)(rk & ~RK_CONST_FLAG)));
    return reg;
}

/* Every reader of a variable's value must go through this, not raw P.var_regs[i], or a raw slot
   index gets misread as a plain register (real bug found in string interpolation). */
static bool var_lookup_rk(unsigned int name_idx, int* out_rk) {
    if (name_idx == P.self_ref_watch_name)
        P.self_ref_watch_seen = true;
    for (int i = 0; i < P.var_count; i++) {
        if (P.var_names[i] != name_idx)
            continue;
        switch (P.var_kind[i]) {
            case VAR_RAW_INT: *out_rk = RK_RAW_INT_FLAG | P.var_regs[i]; break;
            case VAR_RAW_REAL: *out_rk = RK_RAW_REAL_FLAG | P.var_regs[i]; break;
            default: *out_rk = P.var_regs[i]; break;
        }
        return true;
    }
    return false;
}

/* The ONE gate deciding whether an expression can compose as raw: an already-raw operand, or a
   compile-time int/real literal. A call result, container read, string, or struct/array/dict is
   never raw-composable. */
static RawKind rk_raw_kind(Chunk* c, int rk) {
    if (rk & RK_RAW_INT_FLAG)
        return RAWK_INT;
    if (rk & RK_RAW_REAL_FLAG)
        return RAWK_REAL;
    if (rk & RK_CONST_FLAG) {
        AerVal v = c->pool[rk & ~RK_CONST_FLAG];
        if (aer_type(v) == TYPE_INTEGER)
            return RAWK_INT;
        if (aer_type(v) == TYPE_REAL)
            return RAWK_REAL;
    }
    return RAWK_NONE;
}

/* Reserves this loop's preheader gap. Call immediately before the loop's own first instruction --
   everything after the gap is either the condition (re-run per iteration) or the body, so the gap
   dominates every use it can serve. Returns false when the nesting bound is reached, in which case
   nothing hoists and hoist_end must be told so. */
static bool hoist_begin(Chunk* c) {
    if (P.hoist_depth >= LOOP_MAX)
        return false;
    LoopHoist* h = &P.hoist_stack[P.hoist_depth++];
    h->count = 0;
    h->gap_offset = c->count;
    h->int_count = 0;
    h->saved_floor = P.slot_floor;
    h->slot_base = (P.slot_floor + HOIST_MAX <= P.raw_real_next) ? P.slot_floor : -1;
    if (h->slot_base >= 0) {
        P.slot_floor += HOIST_MAX;
        P.slot_next = P.slot_floor;
        track_peak(P.slot_floor);
    }
    h->raised_floor = P.slot_floor;
    h->real_count = 0;
    h->saved_real_floor = P.raw_real_floor;
    h->real_base = (P.raw_real_floor - HOIST_MAX >= P.slot_max) ? P.raw_real_floor - 1 : -1;
    if (h->real_base >= 0) {
        P.raw_real_floor -= HOIST_MAX;
        P.raw_real_next = P.raw_real_floor;
        raw_track_low(P.raw_real_floor);
    }
    h->lowered_real_floor = P.raw_real_floor;
    for (int i = 0; i < HOIST_GAP_WORDS; i++)
        chunk_emit(c, 0);
    return true;
}

/* Backfills the gap now the body has been parsed. after_gap is the loop's own first instruction --
   the address the skip-jump targets when fewer than HOIST_MAX constants were hoisted. */
static void hoist_end(Chunk* c, unsigned int after_gap, bool active) {
    if (!active)
        return;
    LoopHoist* h = &P.hoist_stack[--P.hoist_depth];
    unsigned int w = h->gap_offset;
    for (int i = 0; i < h->count; i++) {
        if (!h->is_int[i]) {
            c->code[w++] = PACK1(OP_RAW_LOAD_REAL, h->slot[i]);
            c->code[w++] = h->rawk_idx[i];
        } else if (h->from_pool[i]) {
            c->code[w++] = PACK1(OP_RAW_LOAD_INT_POOL, h->slot[i]);
            c->code[w++] = h->rawk_idx[i];
        } else {
            c->code[w++] = PACK1(OP_RAW_LOAD_INT, h->slot[i]);
            c->code[w++] = (uint32_t)(int32_t)h->value[i];
        }
    }
    if (w < h->gap_offset + HOIST_GAP_WORDS) {
        c->code[w++] = OP_JUMP;
        c->code[w] = (uint32_t)(int32_t)((int64_t)after_gap - (int64_t)w - 1);
    }
    /* Conditional, like every other floor restore here: the body may have declared variables of its
       own above this block, and lowering the floor past them would hand a live variable's slot back
       to the temp allocator. The real block needs no such test -- loops nest strictly, so an inner
       one has already given its own claims back by the time this runs. */
    if (P.slot_floor == h->raised_floor) {
        P.slot_floor = h->saved_floor;
        P.slot_next = h->saved_floor;
    }
    if (P.raw_real_floor == h->lowered_real_floor) {
        P.raw_real_floor = h->saved_real_floor;
        P.raw_real_next = h->saved_real_floor;
    }
}

/* The hoisted slot for this constant in the innermost loop, reserving one on first use. -1 means
   "not hoisted" -- no enclosing loop, this loop's gap is full, or the raw-slot budget is spent --
   and the caller then emits the load inline exactly as before. */
static int hoist_constant(bool is_int, int64_t value, unsigned int rawk_idx, bool from_pool) {
    if (P.hoist_depth <= 0)
        return -1;
    LoopHoist* h = &P.hoist_stack[P.hoist_depth - 1];
    for (int i = 0; i < h->count; i++) {
        if (h->is_int[i] != is_int)
            continue;
        if (is_int ? (h->value[i] == value) : (h->rawk_idx[i] == rawk_idx))
            return h->slot[i];
    }
    if (h->count >= HOIST_MAX)
        return -1;
    int slot;
    if (is_int) {
        if (h->slot_base < 0 || h->int_count >= HOIST_MAX)
            return -1;
        slot = h->slot_base + h->int_count++;
    } else {
        if (h->real_base < 0 || h->real_count >= HOIST_MAX)
            return -1;
        slot = h->real_base - h->real_count++;
    }
    int i = h->count++;
    h->is_int[i] = is_int;
    h->from_pool[i] = from_pool;
    h->value[i] = value;
    h->rawk_idx[i] = rawk_idx;
    h->slot[i] = slot;
    return slot;
}

/* Widens an integer literal on the way in, matching what the raw real opcodes did at runtime when
   they still read a tagged pool entry. */
static unsigned int rawk_real_of_pool(Chunk* c, unsigned int pool_idx) {
    AerVal v = c->pool[pool_idx];
    return chunk_add_rawk_real(c, aer_type(v) == TYPE_INTEGER ? (double)aer_as_int(v) : aer_as_real(v));
}

/* An already-raw operand's slot is reused directly; a literal loads into a fresh slot.
   Returns -1 on budget overflow (caller falls back to boxed). */
static int raw_materialize(Chunk* c, int rk, RawKind kind) {
    if (kind == RAWK_INT) {
        if (rk & RK_RAW_INT_FLAG)
            return rk & RK_RAW_SLOT_MASK;
        int64_t v = aer_as_int(c->pool[rk & ~RK_CONST_FLAG]);
        bool wide = !(v >= INT32_MIN && v <= INT32_MAX);
        unsigned int rawk_idx = wide ? chunk_add_rawk_int(c, v) : 0;
        int hoisted = hoist_constant(true, v, rawk_idx, wide);
        if (hoisted >= 0)
            return hoisted;
        int slot = slot_alloc(RAWK_INT);
        if (slot < 0)
            return -1;
        /* AER integers are 64-bit and this immediate is int32, so a wider literal needs the side
           table. */
        if (!wide) {
            chunk_emit(c, PACK1(OP_RAW_LOAD_INT, slot));
            chunk_emit(c, (uint32_t)(int32_t)v);
        } else {
            chunk_emit(c, PACK1(OP_RAW_LOAD_INT_POOL, slot));
            chunk_emit(c, rawk_idx);
        }
        return slot;
    } else {
        if (rk & RK_RAW_REAL_FLAG)
            return rk & RK_RAW_SLOT_MASK;
        unsigned int rawk_idx = rawk_real_of_pool(c, rk & ~RK_CONST_FLAG);
        int hoisted = hoist_constant(false, 0, rawk_idx, false);
        if (hoisted >= 0)
            return hoisted;
        int slot = slot_alloc(RAWK_REAL);
        if (slot < 0)
            return -1;
        chunk_emit(c, PACK1(OP_RAW_LOAD_REAL, slot));
        chunk_emit(c, rawk_idx);
        return slot;
    }
}

static void note_raw_write(Chunk* c, int dest, RawKind kind) {
    P.raw_write_offset = c->count - 1;
    P.raw_write_slot = dest;
    P.raw_write_kind = kind;
    P.raw_write_epoch = P.patch_epoch;
}

/* Writes an assignment's value straight into the variable's slot, by re-pointing the arithmetic
   that produced it, so `x = a * b + c` needs no move afterwards. Declines unless that arithmetic is
   still the very last word emitted and no jump has been patched since -- either would mean the
   write is reachable on paths the move was not. The source slot must be a temp: retargeting one
   that belongs to a variable would drop that variable's own value. */
static bool retarget_raw_write(Chunk* c, int src_slot, int dest_slot, RawKind kind) {
    if (P.raw_write_offset == NO_OFFSET || P.raw_write_offset != c->count - 1)
        return false;
    if (P.raw_write_slot != src_slot || P.raw_write_kind != kind)
        return false;
    if (P.raw_write_epoch != P.patch_epoch || !raw_is_temp(src_slot))
        return false;
    uint32_t w = c->code[P.raw_write_offset];
    c->code[P.raw_write_offset] = PACK3((Opcode)(w & 0xFF), dest_slot, UNPACK_B(w), UNPACK_C(w));
    P.raw_write_offset = NO_OFFSET;
    return true;
}

/* Re-points a const-flagged RK at whichever raw constant table `kind` names. An int-kind opcode
   takes only an integer literal: promoting a real one would need the comparison rewritten around
   its fractional part, so it declines and the caller keeps the boxed form. */
static bool rawk_const_rk(Chunk* c, int rk, RawKind kind, int* out_rk) {
    unsigned int pool_idx = (unsigned int)(rk & ~RK_CONST_FLAG);
    AerVal v = c->pool[pool_idx];
    unsigned int idx;
    if (kind == RAWK_INT) {
        if (aer_type(v) != TYPE_INTEGER)
            return false;
        idx = chunk_add_rawk_int(c, aer_as_int(v));
    } else {
        if (aer_type(v) != TYPE_REAL && aer_type(v) != TYPE_INTEGER)
            return false;
        idx = rawk_real_of_pool(c, pool_idx);
    }
    *out_rk = (int)(RK_CONST_FLAG | idx);
    return true;
}

/* Rewrites a just-emitted typed index get into its raw form, so an element feeding raw arithmetic
   never becomes an AerVal at all. Only when that get is the whole of the operand's emitted code --
   the same "inspect what was just compiled" test compare fusion and FMA fusion already use. Safe to
   fail after rewriting: emit_binary's own box_if_raw handles a raw operand on the boxed path. */
static bool try_rewrite_index_get_raw(Chunk* c, int* rk, unsigned int start, RawKind want) {
    if (want == RAWK_NONE || c->count - start != 1)
        return false;
    if (*rk & (RK_CONST_FLAG | RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG))
        return false;
    uint32_t w = c->code[start];
    if ((Opcode)(w & 0xFF) != OP_TYPED_INDEX_GET_UNCHECKED || (int)UNPACK_A(w) != *rk)
        return false;

    bool is_int = (want == RAWK_INT);
    release_if_top(*rk);
    int slot = slot_alloc(want);
    if (slot < 0)
        return false;
    c->code[start] =
        PACK3(is_int ? OP_INDEX_GET_RAW_INT : OP_INDEX_GET_RAW_REAL, slot, UNPACK_B(w), UNPACK_C(w));
    *rk = (is_int ? RK_RAW_INT_FLAG : RK_RAW_REAL_FLAG) | slot;
    return true;
}

/* Returns false if the operator has no raw-native form or the operand kinds mismatch, and the
   caller falls back to the boxed path. Only ADD/SUB/MUL/DIV/MOD/FLOOR_DIV and the 4 ordering
   comparisons are raw-native -- EQ/NEQ/bitwise/AND/OR/IN always stay boxed, deliberately. */
static bool try_emit_binary_raw(Chunk* c, Opcode op, int rk_lhs, int rk_rhs, int* out_rk) {
    RawKind kind_lhs = rk_raw_kind(c, rk_lhs);
    RawKind kind_rhs = rk_raw_kind(c, rk_rhs);

    /* One raw operand and one whose type nothing has proven: check the boxed side once into a raw
       slot and let the rest be ordinary raw arithmetic, so the result stays raw and a local built
       this way never has to shadow. Arithmetic only -- these operators reject a non-number on
       either side anyway, so erroring at the unbox says the same thing one opcode earlier, which is
       not true of equality (`5 == "5"` is false, not an error). */
    if ((kind_lhs == RAWK_NONE) != (kind_rhs == RAWK_NONE) &&
        (op == OP_ADD || op == OP_SUB || op == OP_MUL)) {
        bool lhs_raw = (kind_lhs != RAWK_NONE);
        int boxed_rk = lhs_raw ? rk_rhs : rk_lhs;
        /* Real only. An integer raw side composed with a boxed real must promote the whole result
           to real, which an int slot cannot hold -- OP_UNBOX_REAL widens an integer the same way
           the boxed arithmetic does, so only that direction is safe here. */
        /* A register known to hold a typed array is not a number awaiting an unbox: `a * 2.0`
           broadcasts the scalar across it, so it has to stay on the boxed path that reaches
           vm_typed_array_scalar_op. Unboxing it raised "Cannot apply this operator to float and
           float32[]" one opcode early instead. */
        int boxed_reg = drop_raw_marks(boxed_rk);
        bool boxed_is_typed_array = !(boxed_rk & RK_CONST_FLAG) && boxed_reg >= 0 &&
                                    boxed_reg < FRAME_REGISTERS && P.reg_elem_kind[boxed_reg] != RAWK_NONE;
        if ((lhs_raw ? kind_lhs : kind_rhs) == RAWK_REAL && !boxed_is_typed_array &&
            !(boxed_rk & (RK_CONST_FLAG | RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG))) {
            release_if_top(boxed_rk);
            int tmp = slot_alloc(RAWK_REAL);
            if (tmp >= 0) {
                chunk_emit(c, PACK3(OP_UNBOX_REAL, tmp, boxed_rk, 0));
                if (lhs_raw) {
                    rk_rhs = RK_RAW_REAL_FLAG | tmp;
                    kind_rhs = RAWK_REAL;
                } else {
                    rk_lhs = RK_RAW_REAL_FLAG | tmp;
                    kind_lhs = RAWK_REAL;
                }
            }
        }
    }
    if (kind_lhs == RAWK_NONE || kind_rhs == RAWK_NONE || kind_lhs != kind_rhs)
        return false;
    bool int_kind = (kind_lhs == RAWK_INT);

    Opcode raw_op;
    bool is_cmp = false;
    /* `a > b` is `b < a`: emitted as the LT/LTE opcode with the slots swapped, so the raw-vs-raw
       family needs no GT/GTE members at all. */
    bool swap_cmp = false;
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
                raw_op = OP_RAW_LT_INT;
                is_cmp = true;
                swap_cmp = true;
                break;
            case OP_LTE:
                raw_op = OP_RAW_LTE_INT;
                is_cmp = true;
                break;
            case OP_GTE:
                raw_op = OP_RAW_LTE_INT;
                is_cmp = true;
                swap_cmp = true;
                break;
            case OP_EQ:
                raw_op = OP_RAW_EQ_INT;
                is_cmp = true;
                break;
            case OP_NEQ:
                raw_op = OP_RAW_NEQ_INT;
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
                raw_op = OP_RAW_LT_REAL;
                is_cmp = true;
                swap_cmp = true;
                break;
            case OP_LTE:
                raw_op = OP_RAW_LTE_REAL;
                is_cmp = true;
                break;
            case OP_GTE:
                raw_op = OP_RAW_LTE_REAL;
                is_cmp = true;
                swap_cmp = true;
                break;
            case OP_EQ:
                raw_op = OP_RAW_EQ_REAL;
                is_cmp = true;
                break;
            case OP_NEQ:
                raw_op = OP_RAW_NEQ_REAL;
                is_cmp = true;
                break;
            default: return false; /* no raw MOD/FLOOR_DIV for real */
        }
    }

    int slot_lhs = raw_materialize(c, rk_lhs, kind_lhs);
    if (slot_lhs < 0)
        return false; /* raw-slot budget exhausted: fall back to boxed */

    /* A literal right operand rides in the instruction as a raw-constant index instead of being
       loaded into a slot of its own. Comparisons only: an arithmetic operand is read on the hot
       path of every numeric loop, and making that read conditional measured +4 instructions on each
       of mandelbrot's 203M of them -- far more than the OP_RAW_LOAD it saves, which a loop's
       preheader hoists anyway. A comparison is one per iteration, where the trade goes the other
       way. Only the right one: the swap form moves it left, where the field is a bare slot index. */
    int rhs_field = -1;
    if (is_cmp && !swap_cmp && (rk_rhs & RK_CONST_FLAG)) {
        int const_rk;
        if (rawk_const_rk(c, rk_rhs, kind_lhs, &const_rk)) {
            unsigned int idx = (unsigned int)(const_rk & ~RK_CONST_FLAG);
            if (idx <= RK8_INDEX_MASK)
                rhs_field = (int)(RK8_CONST_FLAG | idx);
        }
    }
    /* Same idea for integer +/-, but via a dedicated opcode whose C field is a bare index (see
       OP_RAW_ADD_INT_K). Worth it only because a recursive body re-runs the load it replaces on
       every call, where a loop's would have been hoisted once into the preheader. */
    if (!is_cmp && int_kind && (raw_op == OP_RAW_ADD_INT || raw_op == OP_RAW_SUB_INT) &&
        (rk_rhs & RK_CONST_FLAG)) {
        int const_rk;
        if (rawk_const_rk(c, rk_rhs, RAWK_INT, &const_rk)) {
            unsigned int idx = (unsigned int)(const_rk & ~RK_CONST_FLAG);
            if (idx <= 0xFFu) {
                rhs_field = (int)idx;
                raw_op = (raw_op == OP_RAW_ADD_INT) ? OP_RAW_ADD_INT_K : OP_RAW_SUB_INT_K;
            }
        }
    }
    int slot_rhs = -1;
    if (rhs_field < 0) {
        slot_rhs = raw_materialize(c, rk_rhs, kind_lhs); /* same kind, confirmed above */
        if (slot_rhs < 0)
            return false;
        rhs_field = slot_rhs;
    }

    /* Free-then-allocate, RHS then LHS -- only frees a slot that was actually a temp. */
    raw_release_if_top(slot_rhs);
    raw_release_if_top(slot_lhs);

    if (is_cmp) {
        int dest = reg_alloc();
        P.last_cmp_offset = c->count;
        chunk_emit(c, PACK3(raw_op, dest, swap_cmp ? slot_rhs : slot_lhs, swap_cmp ? slot_lhs : rhs_field));
        *out_rk = dest;
        return true;
    }
    if (div_int_promotes_to_real) {
        int dest = slot_alloc(RAWK_REAL);
        if (dest < 0)
            return false; /* extremely unlikely right after freeing 2 int slots, but stay safe */
        chunk_emit(c, PACK3(raw_op, dest, slot_lhs, rhs_field));
        note_raw_write(c, dest, RAWK_REAL);
        *out_rk = RK_RAW_REAL_FLAG | dest;
        return true;
    }
    int dest = slot_alloc(int_kind ? RAWK_INT : RAWK_REAL);
    if (dest < 0)
        return false;
    chunk_emit(c, PACK3(raw_op, dest, slot_lhs, rhs_field));
    /* Carry the non-negativity proof exactly as emit_binary does for the checked form. Skipping it
       here meant an expression silently lost the proof the moment it became eligible for unchecked
       arithmetic -- `p*p` as a range start being the case that found it. */
    if (dest >= 0 && dest < FRAME_REGISTERS)
        P.reg_nonneg[dest] = binop_preserves_nonneg(op) && rk_nonneg(c, rk_lhs) && rk_nonneg(c, rk_rhs);
    note_raw_write(c, dest, int_kind ? RAWK_INT : RAWK_REAL);
    *out_rk = (int_kind ? RK_RAW_INT_FLAG : RK_RAW_REAL_FLAG) | dest;
    return true;
}

/* Reads Chunk.functions directly -- the registry chunk_add_function already maintains is the
   single source of truth; the parser keeps no parallel copy. */
static bool func_lookup(Chunk* c, unsigned int name_idx, unsigned int* out_offset,
                        unsigned int* out_func_index) {
    ChunkFunction* f = chunk_find_function_by_name_idx(c, name_idx);
    if (!f)
        return false;
    *out_offset = f->code_offset;
    *out_func_index = (unsigned int)(f - c->functions);
    return true;
}

/* A bare-name value reference or an omitted-defaults call needs the full signature to build a
   real AerFunction, not just the offset. */
static bool func_full_lookup(Chunk* c, unsigned int name_idx, unsigned int* out_offset,
                             unsigned int* out_arity, unsigned int* out_min_arity, AerVal** out_defaults,
                             unsigned int* out_max_registers, unsigned short* out_frame_bounds,
                             unsigned int* out_func_index) {
    ChunkFunction* f = chunk_find_function_by_name_idx(c, name_idx);
    if (!f)
        return false;
    *out_offset = f->code_offset;
    *out_arity = f->arity;
    *out_min_arity = f->min_arity;
    *out_defaults = f->defaults;
    *out_max_registers = f->max_registers;
    *out_frame_bounds = f->frame_bounds;
    *out_func_index = (unsigned int)(f - c->functions);
    return true;
}

/* Builds a real runtime AerFunction, reusing the existing constructor. `defaults` is used
   as-is, not copied. */
static AerVal build_function_value(unsigned int func_offset, unsigned int arity, unsigned int min_arity,
                                   AerVal* defaults, unsigned int max_registers,
                                   unsigned short frame_bounds) {
    AerFunction* fn = vm_new_function();
    fn->code_offset = func_offset;
    fn->arity = (uint16_t)arity;
    fn->min_arity = (uint16_t)min_arity;
    fn->defaults = defaults;
    fn->max_registers = max_registers;
    fn->frame_bounds = frame_bounds;
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
       Patches both the callee_offset word (emit_call's returned patch_offset) and the function's
       byte-offset word immediately after it (emit_call always emits them back-to-back) -- a
       forward-referenced call can't know its target's position at emission time any more than it
       can know its code offset. Must match emit_call's scaling exactly. */
    for (int i = 0; i < P.pending_count;) {
        if (P.pending_calls[i].name_idx == name_idx) {
            patch_call_target(c, P.pending_calls[i].patch_offset, offset);
            c->code[P.pending_calls[i].patch_offset + 1] = (uint32_t)(new_func_index * sizeof(ChunkFunction));
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
        if (P.struct_names[i] == name_idx)
            return true;
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
    rk = drop_raw_marks(rk);
    if (!(rk & RK_CONST_FLAG) && is_temp(rk) && rk == P.slot_next - 1) {
        return rk;
    }
    int target = reg_alloc();
    if (rk & RK_CONST_FLAG) {
        chunk_emit(c, PACK_OP_A_W16(OP_LOADK, target, (unsigned int)(rk & ~RK_CONST_FLAG)));
    } else {
        chunk_emit(c, PACK2(OP_MOVE, target, rk));
        if (rk >= 0 && rk < FRAME_REGISTERS)
            P.reg_elem_kind[target] = P.reg_elem_kind[rk];
        P.reg_len_class[target] = P.reg_len_class[rk];
    }
    return target;
}

/* Shared bulk-copy prep for calls/arrays/dicts -- all three need contiguous operand registers.
   Returns the count; *out_base unspecified when count==0. ONE operand has no run to be contiguous
   with, so it stays where it is and no move is emitted -- that move was 10.3% of binary_trees'
   dispatches. *out_base_is_temp then tells the caller whether the result may overwrite it, since a
   variable's own register is not the caller's to clobber. */
static int parse_contiguous_exprs(Chunk* c, TokenType close_tok, int* out_base, bool* out_base_is_temp) {
    int base = -1;
    int count = 0;
    *out_base_is_temp = true;
    P.first_arg_end = 0;
    if (!equal(close_tok)) {
        int rk = parse_binary(c, 0);
        if (!equal(TOKEN_COMMA)) {
            base = materialize(c, rk);
            *out_base_is_temp = is_temp(base);
            *out_base = base;
            P.first_arg_end = c->count;
            return 1;
        }
        base = arg_materialize(c, rk);
        P.first_arg_end = c->count;
        count = 1;
        while (consume(TOKEN_COMMA)) {
            arg_materialize(c, parse_binary(c, 0));
            count++;
        }
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
        /* There is no `as`: casts are integer(x)/float(x)/boolean(x)/string(x), shape checks are
           type(x) == "Name", and import aliasing is `import alias "path"`. */
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
    if (!parse_had_error && !equal(TOKEN_END_OF_FILE))
        error_at("Unexpected token in string interpolation");
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
        if (is_temp(parts[i]))
            temps++;
    if (temps)
        reg_free(temps);
    int dest = reg_alloc();
    P.last_interp_offset = c->count;
    P.last_interp_dest = dest;
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

    /* Collected as RK values and emitted as one OP_INTERP: one dispatch and one allocation, not a
       concatenate per part. Past INTERP_MAX_PARTS a left-fold chain takes over, so there is no
       limit on length. */
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
            if (s[i] == '{')
                break;
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
        if (i >= len)
            break;

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
                if (depth == 0)
                    break;
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

/* CAST_NONE for any other name. The four casts are ordinary reserved builtin names, not lexer
   keywords, so this is the only place that spelling is decided. */
static int cast_type_for_name(AerString* name) {
    static const struct {
        const char* word;
        unsigned int len;
        int cast;
    } casts[] = {
        {"integer", 7, CAST_INTEGER},
        {"float", 5, CAST_FLOAT},
        {"boolean", 7, CAST_BOOLEAN},
        {"string", 6, CAST_STRING},
    };
    for (unsigned int i = 0; i < sizeof(casts) / sizeof(*casts); i++)
        if (name->length == casts[i].len && strncmp(name->data, casts[i].word, casts[i].len) == 0)
            return casts[i].cast;
    return CAST_NONE;
}

/* Shared by all four casts: box a raw-tracked lhs first (no raw-native cast form exists), then
   OP_CAST, or OP_UNARY/OP_TO_STR for string(), which had its own opcode before casting existed. */
static int emit_primitive_cast(Chunk* c, int cast_type, int lhs) {
    /* `float(i)` on a raw int is a hardware conversion, not a reason to build an AerVal and tear it
       apart again. Only between the two raw kinds -- a cast from anything else still needs the
       general opcode's type dispatch. */
    RawKind src_kind = rk_raw_kind(c, lhs);
    if ((cast_type == CAST_FLOAT && src_kind == RAWK_INT) ||
        (cast_type == CAST_INTEGER && src_kind == RAWK_REAL)) {
        bool to_real = (cast_type == CAST_FLOAT);
        int src = raw_materialize(c, lhs, src_kind);
        if (src >= 0) {
            raw_release_if_top(src);
            int dest = slot_alloc(to_real ? RAWK_REAL : RAWK_INT);
            if (dest >= 0) {
                chunk_emit(c, PACK3(to_real ? OP_RAW_INT_TO_REAL : OP_RAW_REAL_TO_INT, dest, src, 0));
                return (to_real ? RK_RAW_REAL_FLAG : RK_RAW_INT_FLAG) | dest;
            }
        }
    }
    lhs = drop_raw_marks(lhs);
    release_if_top(lhs);
    int dest = reg_alloc();
    bool spilled = false;
    if (!rk8_fits(lhs)) {
        lhs = materialize(c, lhs);
        spilled = true;
    }
    if (cast_type == CAST_STRING)
        chunk_emit(c, PACK3(OP_UNARY, dest, OP_TO_STR, pack_rk8(lhs)));
    else
        chunk_emit(c, PACK3(OP_CAST, dest, cast_type, pack_rk8(lhs)));
    if (spilled)
        reg_free(1);
    return dest;
}

static int parse_primary_inner(Chunk* c) {
    if (consume(TOKEN_FUNCTION))
        return parse_function_expr(c);
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
        if (parse_had_error)
            return 0;

        if (consume(TOKEN_SEMICOLON)) {
            int narrow_flag = 0;
            if (c->count == emit_count_before) {
                if (narrow_int_candidate)
                    narrow_flag = 1;
                else if (narrow_float_candidate)
                    narrow_flag = 2;
            }
            RawKind fill_kind = rk_raw_kind(c, rk_first);
            int fill_reg = arg_materialize(c, rk_first);
            int rk_count = parse_binary(c, 0);
            require(TOKEN_CLOSE_BRACKET, "expected ']' after repeat-literal count");
            if (parse_had_error)
                return 0;
            rk_count = drop_raw_marks(rk_count);
            if (!rk16_fits(rk_count)) {
                error_at("Expression too large to compile (register/constant exceeds the repeat-literal "
                         "count encoding's range)");
                return 0;
            }
            /* Taken before the register is released, since dest may be handed the very same one. */
            int count_class = value_class_of(rk_count);
            /* Strict LIFO free order -- rk_count was allocated (if a temp at all) after fill_reg. */
            release_if_top(rk_count);
            release_if_top(fill_reg);
            int dest = reg_alloc();
            emit_array_repeat(c, dest, fill_reg, narrow_flag, rk_count);
            P.reg_elem_kind[dest] = fill_kind;
            P.reg_len_class[dest] = count_class;
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
        if (parse_had_error)
            return 0;
        if (item_count > 1)
            reg_free(item_count - 1);
        emit_array_new(c, item_reg_base, item_reg_base, item_count);
        return item_reg_base;
    }
    /* Each item is a key:value pair, so key/value materialize back to back, landing at
       pair_reg_base+2i/+2i+1 to match OP_DICT_NEW's layout. */
    if (consume(TOKEN_OPEN_BRACE)) {
        int pair_reg_base = -1;
        int pair_count = 0;
        if (!equal(TOKEN_CLOSE_BRACE)) {
            do {
                int rk_key = parse_binary(c, 0);
                int reg_key = arg_materialize(c, rk_key);
                if (pair_count == 0)
                    pair_reg_base = reg_key;
                require(TOKEN_COLON, "expected ':' after dict key");
                if (parse_had_error)
                    return 0;
                int rk_val = parse_binary(c, 0);
                arg_materialize(c, rk_val);
                pair_count++;
            } while (consume(TOKEN_COMMA));
        }
        require(TOKEN_CLOSE_BRACE, "expected '}' after dict literal");
        if (parse_had_error)
            return 0;
        int dest = (pair_count > 0) ? pair_reg_base : reg_alloc();
        if (pair_count > 1)
            reg_free(2 * pair_count - 1);
        emit_dict_new(c, dest, pair_reg_base < 0 ? dest : pair_reg_base, pair_count);
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
    if (token.type == TOKEN_STRING)
        return parse_string_literal(c);
    if (at_module_name(c))
        return parse_module_call(c);
    if (token.type == TOKEN_IDENTIFIER) {
        unsigned int name_idx = chunk_add_pool(c, token.value);
        lex();
        if (consume(TOKEN_OPEN_PARENTHESE))
            return parse_call(c, name_idx);

        /* A raw-tracked name returns an RK_RAW_*_FLAG-tagged operand, letting a bare reference compose
           through further arithmetic without boxing. */
        int reg;
        if (var_lookup_rk(name_idx, &reg))
            return reg;

        /* A known function referenced without a following '(' is a reference to the function itself
           as a value -- built once per reference as a deduped pool constant. */
        unsigned int func_offset, func_arity, func_min_arity, func_max_registers;
        unsigned short func_frame_bounds;
        unsigned int
            func_index_unused; /* AerFunction carries its own peaks directly -- no func_index needed for OP_CALL_VALUE */
        AerVal* func_defaults;
        if (func_full_lookup(c, name_idx, &func_offset, &func_arity, &func_min_arity, &func_defaults,
                             &func_max_registers, &func_frame_bounds, &func_index_unused)) {
            AerVal fv = build_function_value(func_offset, func_arity, func_min_arity, func_defaults,
                                             func_max_registers, func_frame_bounds);
            return (int)chunk_add_pool(c, fv) | RK_CONST_FLAG;
        }

        /* Must not fall through to var_slot -- that would read an uninitialized register. */
        if (report_if_shadowed_global(c, name_idx))
            return 0;
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
            bool arg_base_is_temp;
            int arg_count =
                parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base, &arg_base_is_temp);
            require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
            if (parse_had_error)
                return rk;
            int dest = callee_reg;
            int base = arg_reg_base < 0 ? dest : arg_reg_base;
            emit_call_value(c, dest, base, arg_count, callee_reg);
            if (arg_count > 0 && arg_base_is_temp)
                reg_free(arg_count);
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
            rk_start = drop_raw_marks(rk_start);

            if (!consume(TOKEN_COLON)) {
                require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
                if (parse_had_error)
                    return rk;

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

                    release_if_top(rk_start);
                    release_if_top(arr_reg);

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
                        int slot = slot_alloc(is_int ? RAWK_INT : RAWK_REAL);
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
                release_if_top(rk_start);
                release_if_top(arr_reg);

                /* Element kind known, so the read lands in a statically-typed slot and everything
                   downstream composes unchecked instead of tag-checking what it just produced. */
                RawKind elem =
                    (arr_reg >= 0 && arr_reg < FRAME_REGISTERS) ? P.reg_elem_kind[arr_reg] : RAWK_NONE;
                if (elem != RAWK_NONE && rk8_fits(rk_start)) {
                    int slot = slot_alloc(elem);
                    if (slot >= 0) {
                        /* With the loop proof in hand the index needs no bounds check either. The
                           kind here is not an inference: reg_elem_kind is set from watching the
                           array get built and cleared by any write to that register, unlike
                           try_rewrite_index_get_raw's guess from the consumer, which is what the
                           checked opcode's fallback exists to catch (15 in 22.1M, all from there). */
                        if (index_safe_unchecked(arr_reg, rk_start))
                            chunk_emit(
                                c, PACK3(OP_TYPED_INDEX_GET_UNCHECKED, slot, arr_reg, pack_rk8(rk_start)));
                        else
                            chunk_emit(c,
                                       PACK3(elem == RAWK_INT ? OP_INDEX_GET_RAW_INT : OP_INDEX_GET_RAW_REAL,
                                             slot, arr_reg, pack_rk8(rk_start)));
                        rk = (elem == RAWK_INT ? RK_RAW_INT_FLAG : RK_RAW_REAL_FLAG) | slot;
                        continue;
                    }
                }

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
            if (parse_had_error)
                return rk;

            int arr_reg = materialize(c, rk);

            release_if_top(rk_end);
            release_if_top(rk_slice_start);
            release_if_top(arr_reg);

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
            release_if_top(struct_reg);

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
                    int slot = slot_alloc(is_int ? RAWK_INT : RAWK_REAL);
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
    rk = drop_raw_marks(rk);
    release_if_top(rk);
    int dest = reg_alloc();
    bool spilled = false;
    if (!rk8_fits(rk)) {
        rk = materialize(c, rk);
        spilled = true;
    }
    chunk_emit(c, PACK3(OP_UNARY, dest, OP_NOT, pack_rk8(rk)));
    if (spilled)
        reg_free(1);
    return dest;
}

/* Recurses into parse_unary so chained unary (`~~x`) works. rk is RK-encoded like any binary operand. */
static int parse_unary_inner(Chunk* c) {
    if (consume(TOKEN_NOT))
        return parse_not(c);

    Opcode unary_op;
    if (consume(TOKEN_BITWISE_NOT))
        unary_op = OP_BITWISE_NOT;
    else if (consume(TOKEN_SUBTRACT))
        unary_op = OP_NEGATE;
    else
        return parse_primary(c);

    int rk = parse_unary(c);
    rk = drop_raw_marks(
        rk); /* No raw-native unary form -- box first, or a raw slot index gets misread as a register. */
    release_if_top(rk); /* free-then-allocate, matching every other site */
    int dest = reg_alloc();
    bool spilled = false;
    if (!rk8_fits(rk)) {
        rk = materialize(c, rk);
        spilled = true;
    }
    chunk_emit(c, PACK3(OP_UNARY, dest, unary_op, pack_rk8(rk)));
    if (spilled)
        reg_free(1);
    return dest;
}

static int parse_unary(Chunk* c) {
    return parse_unary_inner(c);
}

/* Re-points a just-emitted comparison at `dest` so && / || need no move to collect their result.
   Only a comparison, whose destination is always field A and whose only effect is that write, and
   only when it is still the last word emitted -- the same test emit_cond_jump_if_false's fusion
   uses, via the offset it already tracks. */
static bool retarget_last_cmp(Chunk* c, int reg_rhs, int dest) {
    if (P.last_cmp_offset == NO_OFFSET || P.last_cmp_offset != c->count - 1)
        return false;
    uint32_t w = c->code[P.last_cmp_offset];
    if ((int)UNPACK_A(w) != reg_rhs || !is_temp(reg_rhs))
        return false;
    c->code[P.last_cmp_offset] = PACK3((Opcode)(w & 0xFF), dest, UNPACK_B(w), UNPACK_C(w));
    /* This comparison is now the last word emitted, which would let an enclosing if/while fuse its
       branch with it -- but it sits behind the short circuit and only runs when the left operand was
       truthy. The move being emitted here is what used to make that impossible. */
    P.last_cmp_offset = NO_OFFSET;
    return true;
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
    if (reg_rhs != dest && !retarget_last_cmp(c, reg_rhs, dest))
        chunk_emit(c, PACK2(OP_MOVE, dest, reg_rhs));
    release_if_top(reg_rhs);

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
    if (reg_rhs != dest && !retarget_last_cmp(c, reg_rhs, dest))
        chunk_emit(c, PACK2(OP_MOVE, dest, reg_rhs));
    release_if_top(reg_rhs);

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
    reg_free(1); /* Freed before the next pipe-call argument, restoring P.slot_next to where
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
        if (parse_had_error)
            return lhs;
        if (!equal(TOKEN_IDENTIFIER)) {
            error_at("Expected a function name after '.'");
            return lhs;
        }
        int fn_id = module_fn_id(module_id, aer_as_string(token.value));
        unsigned int fn_idx = chunk_add_pool(c, token.value);
        lex();
        require(TOKEN_OPEN_PARENTHESE, "expected '(' after piped module function name");
        if (parse_had_error)
            return lhs;

        release_if_top(lhs);
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
        if (parse_had_error)
            return lhs;

        if (arg_count > 1)
            reg_free(arg_count - 1);
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
    if (parse_had_error)
        return lhs;

    release_if_top(lhs);
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
    if (parse_had_error)
        return lhs;

    if (arg_count > 1)
        reg_free(arg_count - 1);
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
        if (!binary_op_info(token.type, &prec, &op) || prec <= min_prec)
            break;
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
            rhs = drop_raw_marks(rhs);
            release_if_top(rhs);
            release_if_top(lhs);

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
            release_if_top(c_reg);
            int dest = reg_alloc();
            chunk_emit(c, PACK3(OP_TYPED_ARRAY_CHAIN2, dest, lhs_chain2_a, lhs_chain2_b));
            chunk_emit(c, PACK_2X16((uint16_t)lhs_chain2_op1, (uint16_t)c_reg));
            chunk_emit(c, (uint32_t)op);
            lhs = dest;
            lhs_start = c->count;
            continue;
        }

        /* Generalizes the compound-assignment FMA/FMS fusion to any plain expression whose running
           sum is a fresh raw-real temporary. Safe only for a temporary: a named variable's slot must
           not be overwritten in place, since unlike `x += a*b` a plain expression's lhs may still be
           read with its original value. Requires the RHS to be exactly one raw MUL; anything else
           falls through. Real only. */
        if (!lhs_is_field && !lhs_is_chain2 && (op == OP_ADD || op == OP_SUB) &&
            rk_raw_kind(c, lhs) == RAWK_REAL && raw_is_temp(lhs & RK_RAW_SLOT_MASK) &&
            c->count - rhs_start == 1) {
            uint32_t mw = c->code[rhs_start];
            if ((Opcode)(mw & 0xFF) == OP_RAW_MUL_REAL && rk_raw_kind(c, rhs) == RAWK_REAL &&
                (int)UNPACK_A(mw) == (rhs & RK_RAW_SLOT_MASK)) {
                int lhs_slot = lhs & RK_RAW_SLOT_MASK;
                int mul_a = (int)UNPACK_B(mw), mul_b = (int)UNPACK_C(mw);
                c->count = rhs_start; /* discard the MUL -- fused below instead */
                raw_release_if_top((int)UNPACK_A(mw));
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
                lhs = drop_raw_marks(lhs);
                release_if_top(rhs);
                release_if_top(lhs);

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

        /* One operand already raw makes the other's wanted kind known, which is the only thing a
           typed element read was missing to be read raw in the first place. */
        RawKind kind_l = rk_raw_kind(c, lhs), kind_r = rk_raw_kind(c, rhs);
        if (kind_l != RAWK_NONE && kind_r == RAWK_NONE)
            try_rewrite_index_get_raw(c, &rhs, rhs_start, kind_l);
        else if (kind_r != RAWK_NONE && kind_l == RAWK_NONE)
            try_rewrite_index_get_raw(c, &lhs, lhs_start, kind_r);

        /* Tries a native raw op first (both provably int/real); false means not raw-composable, and
           lhs/rhs still need the ordinary free/emit_binary treatment. */
        int raw_result;
        if (try_emit_binary_raw(c, op, lhs, rhs, &raw_result)) {
            lhs = raw_result;
            lhs_start = c->count;
            continue;
        }

        /* Elementwise arithmetic on a typed array yields a typed array of the same kind, and the
           result has to say so or the next operator in the chain sees an unproven register and
           unboxes it -- `(a - 1.0) * 2.0` failed on exactly that. Read before the registers are
           freed, since dest may be handed one of them. */
        RawKind chain_elem = RAWK_NONE;
        if (op == OP_ADD || op == OP_SUB || op == OP_MUL) {
            int lr = drop_raw_marks(lhs), rr = drop_raw_marks(rhs);
            if (!(lhs & RK_CONST_FLAG) && lr >= 0 && lr < FRAME_REGISTERS)
                chain_elem = P.reg_elem_kind[lr];
            if (chain_elem == RAWK_NONE && !(rhs & RK_CONST_FLAG) && rr >= 0 && rr < FRAME_REGISTERS)
                chain_elem = P.reg_elem_kind[rr];
        }

        /* Free-then-allocate, RHS then LHS, matching compile_node's own discipline exactly. */
        release_if_top(rhs);
        release_if_top(lhs);

        int dest = reg_alloc();
        emit_binary(c, dest, op, lhs, rhs);
        if (dest >= 0 && dest < FRAME_REGISTERS)
            P.reg_elem_kind[dest] = chain_elem;
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
    /* Checked here rather than in var_slot: the raw-promotion path below registers a name itself and
       never calls var_slot, so `integer = 5` would slip through. A reserved name always resolves to
       its builtin at the call site, so binding one made a variable nothing could ever read. */
    if (is_builtin_name(c, name_idx)) {
        return error_at("'%s' is a reserved function name and can't be used as a variable",
                        aer_as_string(c->pool[name_idx])->data);
    }
    /* Multiple RHS values pack into a real array (OP_ARRAY_NEW); each target reads its own index
       back via OP_INDEX_GET. Targets resolve via var_slot BEFORE the RHS is parsed -- creating a
       variable after a temp is live could hand out that temp's own register. */
    if (equal(TOKEN_COMMA)) {
        unsigned int names[MAX_DESTRUCT];
        names[0] = name_idx;
        unsigned int count = 1;
        while (consume(TOKEN_COMMA)) {
            if (!equal(TOKEN_IDENTIFIER)) {
                return error_at("Expected identifier in destructuring");
            }
            if (count >= MAX_DESTRUCT) {
                return error_at("Too many destructuring targets (max %d)", MAX_DESTRUCT);
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
        if (parse_had_error)
            return;

        int target_regs[MAX_DESTRUCT];
        for (unsigned int i = 0; i < count; i++) {
            /* A destructuring target is always a plain dynamic write -- an existing typed name of
               the same spelling must forget its type before var_slot looks it up. */
            ensure_boxed(names[i]);
            target_regs[i] = var_slot(c, names[i]);
            if (target_regs[i] < 0)
                return; /* error_at already called */
        }

        int rk_first = parse_binary(c, 0);
        if (parse_had_error)
            return;
        int rhs_reg_base = arg_materialize(c, rk_first);
        unsigned int rhs_count = 1;
        while (consume(TOKEN_COMMA)) {
            int rk_next = parse_binary(c, 0);
            if (parse_had_error)
                return;
            arg_materialize(c, rk_next);
            rhs_count++;
        }

        /* Destructuring can rebind a shape-tracked or safe-loop-index register, so clear all four
           per-register facts for every target. Timed after the RHS is parsed so a legitimate read
           of a target's old value on the RHS still gets the fast path. */
        for (unsigned int i = 0; i < count; i++) {
            P.reg_known_shape[target_regs[i]] = NULL;
            P.reg_known_element_shape[target_regs[i]] = NULL;
            P.reg_elem_kind[target_regs[i]] = RAWK_NONE;
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
        if (parse_had_error)
            return;

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
               registers are linked at full capacity for the VM's life and aer_vm_reset_for_reuse
               never touches them. global_regs[] only compares register numbers, never dereferences. */
            RawKind rhs_kind = rk_raw_kind(c, rk_val);
            if (P.branch_depth == 0 && rhs_kind != RAWK_NONE) {
                int slot = slot_reserve_one(rhs_kind);
                if (slot >= 0) {
                    int src_slot = raw_materialize(c, rk_val, rhs_kind);
                    if (src_slot < 0) {
                        /* Budget exhausted mid-materialize -- release the reservation, fall through to
                           boxed. */
                        raw_unreserve_one(rhs_kind);
                    } else {
                        if (src_slot != slot) {
                            /* Direct analog of the boxed path's "reg != rk_val -> MOVE" case. */
                            if (!retarget_raw_write(c, src_slot, slot, rhs_kind)) {
                                Opcode move_op = (rhs_kind == RAWK_INT) ? OP_RAW_MOVE_INT : OP_RAW_MOVE_REAL;
                                chunk_emit(c, PACK3(move_op, slot, src_slot, 0));
                            }
                            raw_release_if_top(src_slot);
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
                /* A statically-typed write is still a write: a loop-safety proof keyed on this
                   register, or a tracked length, stops holding. Its non-negativity does not --
                   see note_slot_written. */
                note_slot_written(dest_slot);
                int src_slot = raw_materialize(c, rk_val, rhs_kind);
                if (src_slot >= 0) {
                    if (src_slot != dest_slot) {
                        if (!retarget_raw_write(c, src_slot, dest_slot, rhs_kind)) {
                            Opcode move_op = (rhs_kind == RAWK_INT) ? OP_RAW_MOVE_INT : OP_RAW_MOVE_REAL;
                            chunk_emit(c, PACK3(move_op, dest_slot, src_slot, 0));
                        }
                        raw_release_if_top(src_slot);
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
                return error_at(
                    "This assignment would change '%s' from a fixed numeric type to a different type, "
                    "but it's inside a loop — not supported. If you're accumulating with +, -, or *, "
                    "use the compound form ('%s += ...' etc.) instead — it doesn't have this "
                    "restriction. Otherwise, restructure so the type change happens outside any loop.",
                    aer_as_string(c->pool[name_idx])->data, aer_as_string(c->pool[name_idx])->data);
            }
            rk_val = drop_raw_marks(rk_val);
            if (P.slot_floor >= P.raw_real_next) {
                return error_at("Too many variables (max %d)", FRAME_REGISTERS);
            }
            int new_reg = P.slot_floor;
            P.slot_floor++;
            P.slot_next = P.slot_floor;
            P.var_regs[existing_idx] = new_reg;
            P.var_kind[existing_idx] = VAR_BOXED;
            /* No P.global_regs update needed -- see ensure_boxed's identical reasoning: this path
               only runs on a currently-raw-tracked name, which can never be in P.global_names. */
            if (rk_val & RK_CONST_FLAG) {
                chunk_emit(c, PACK_OP_A_W16(OP_LOADK, new_reg, (unsigned int)(rk_val & ~RK_CONST_FLAG)));
            } else if (new_reg != rk_val) {
                chunk_emit(c, PACK2(OP_MOVE, new_reg, rk_val));
                release_if_top(rk_val);
            }
            return;
        }

        /* Unchanged existing behavior. rk_val is boxed first in case it's raw-flagged. */
        rk_val = drop_raw_marks(rk_val);
        int reg = var_slot(c, name_idx);
        if (reg < 0)
            return; /* error_at already called */
        /* Read before anything clears it. A `[numeric; count]` literal is built straight into the
           register var_slot then hands this name, so source and destination are usually the SAME
           one -- and invalidate_register below would wipe the fact this line is preserving. */
        bool rhs_plain_reg = !(rk_val & (RK_CONST_FLAG | RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG)) &&
                             rk_val >= 0 && rk_val < FRAME_REGISTERS;
        RawKind rhs_elem = rhs_plain_reg ? P.reg_elem_kind[rk_val] : RAWK_NONE;
        /* Preserved across invalidate_register for the same reason rhs_elem is: the literal was
           usually built straight into the register this name is about to be given. */
        int rhs_len_class = rhs_plain_reg ? P.reg_len_class[rk_val] : 0;
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
        P.reg_elem_kind[reg] = rhs_elem;
        P.reg_len_class[reg] = rhs_len_class;
        if (rk_val & RK_CONST_FLAG) {
            chunk_emit(c, PACK_OP_A_W16(OP_LOADK, reg, (unsigned int)(rk_val & ~RK_CONST_FLAG)));
        } else if (reg != rk_val) {
            chunk_emit(c, PACK2(OP_MOVE, reg, rk_val));
            /* Checked after var_slot (which may have just raised the floor), so this correctly recognizes
               rk_val as no-longer-a-temp in the common case. */
            release_if_top(rk_val);
        }
        /* reg == rk_val: the RHS already landed where var_slot reserved -- skip the no-op MOVE. */
        return;
    }

    for (int i = 0; i < COMPOUND_ASSIGN_OP_COUNT; i++) {
        if (!consume(compound_assign_ops[i].tok))
            continue;

        /* A compound assignment can never re-derive length_tracked_name's invariant (the RHS is
           combined with the OLD value, never a fresh length() call alone) -- invalidate eagerly. */
        if (P.length_tracked_valid && name_idx == P.length_tracked_name)
            P.length_tracked_valid = false;

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
            /* Once for every branch below, all of which write this slot -- same reason as the
               plain assignment path above. */
            note_slot_written(P.var_regs[existing_idx]);
            RawKind cur_kind = (P.var_kind[existing_idx] == VAR_RAW_INT) ? RAWK_INT : RAWK_REAL;
            Opcode boxed_op = compound_assign_ops[i].op;
            bool native_op_exists = (boxed_op == OP_ADD || boxed_op == OP_SUB || boxed_op == OP_MUL);

            unsigned int rhs_start = c->count;
            int rk_rhs = parse_binary(c, 0);
            if (parse_had_error)
                return;
            RawKind rhs_kind = rk_raw_kind(c, rk_rhs);
            /* The target's kind is the wanted kind, so `total += nums[i]` reads the element raw
               rather than boxing it for a tag check one opcode later. */
            if (rhs_kind == RAWK_NONE && try_rewrite_index_get_raw(c, &rk_rhs, rhs_start, cur_kind))
                rhs_kind = cur_kind;

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
                    raw_release_if_top((int)UNPACK_A(mw));
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
                    raw_release_if_top(rhs_slot);
                    return;
                }
                /* raw-slot budget exhausted materializing the RHS: fall through to the shadow
                   path below instead of leaving the variable half-updated. */
            }

            /* An ordinary boxed RHS (e.g. nbody.aer's `e += 0.5 * bim * (...)`) is checked once into
               a raw slot and then accumulated with ordinary raw arithmetic -- no shadow, no
               allocation, safe every loop iteration. A provably-mismatched kind still shadows. */
            if (native_op_exists && rhs_kind == RAWK_NONE) {
                bool int_kind = (cur_kind == RAWK_INT);
                int dest_slot = P.var_regs[existing_idx];
                int boxed_reg = materialize(c, rk_rhs);
                int tmp = slot_alloc(cur_kind);
                if (tmp >= 0) {
                    chunk_emit(c, PACK3(int_kind ? OP_UNBOX_INT : OP_UNBOX_REAL, tmp, boxed_reg, 0));
                    Opcode raw_op;
                    if (int_kind)
                        raw_op = (boxed_op == OP_ADD)   ? OP_RAW_ADD_INT
                                 : (boxed_op == OP_SUB) ? OP_RAW_SUB_INT
                                                        : OP_RAW_MUL_INT;
                    else
                        raw_op = (boxed_op == OP_ADD)   ? OP_RAW_ADD_REAL
                                 : (boxed_op == OP_SUB) ? OP_RAW_SUB_REAL
                                                        : OP_RAW_MUL_REAL;
                    chunk_emit(c, PACK3(raw_op, dest_slot, dest_slot, tmp));
                    raw_release_if_top(tmp);
                    release_if_top(boxed_reg);
                    return;
                }
                release_if_top(boxed_reg);
            }

            /* Boxes the current raw value, then performs the compound op -- unlike plain assignment's
               shadow, this genuinely depends on old_slot's value, so a loop re-executing it would
               re-read the stale value every iteration (real bug found this way). No single-pass
               fix exists, so refuse to compile rather than silently corrupt. */
            if (P.loop_depth > 0) {
                return error_at(
                    "This compound assignment would change '%s' from a fixed numeric type to a "
                    "different type, but it's inside a loop — not supported (restructure so the type "
                    "change happens outside any loop)",
                    aer_as_string(c->pool[name_idx])->data);
            }
            /* A real reads its old slot but writes a fresh one -- see ensure_boxed for why a
               real-block slot must never take a dynamically typed write. Either way the old slot
               holds a correctly tagged value, so it needs no boxing on the way out. */
            int old_slot = P.var_regs[existing_idx];
            int new_reg = old_slot;
            if (cur_kind == RAWK_REAL) {
                if (P.slot_floor >= P.raw_real_next) {
                    return error_at("Too many variables (max %d)", FRAME_REGISTERS);
                }
                new_reg = P.slot_floor++;
                P.slot_next = P.slot_floor;
                track_peak(P.slot_floor);
                P.var_regs[existing_idx] = new_reg;
            }
            P.var_kind[existing_idx] = VAR_BOXED;
            /* No P.global_regs update needed -- see ensure_boxed's identical reasoning. */
            rk_rhs = drop_raw_marks(rk_rhs);
            emit_binary(c, new_reg, boxed_op, old_slot, rk_rhs);
            release_if_top(rk_rhs);
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
            return error_at(
                "Compound assignment target must already have a value (no assigning to an undefined "
                "name this way)");
        }

        int rk_rhs = parse_binary(c, 0);
        if (parse_had_error)
            return;
        /* Same P.reg_known_shape invalidation as the plain-assignment tail above -- `reg`'s value
           is about to change (e.g. `bodies += extra_bodies`), so any shape hint on it is no longer
           trustworthy. alias_source_param and reg_known_element_shape cleared alongside it now too
           (earlier omissions here -- the plain-assignment tail clears all three); see
           invalidate_register's own comment for why safe_loop_item_regs needs the same treatment. */
        P.reg_known_shape[reg] = NULL;
        P.reg_known_element_shape[reg] = NULL;
        P.reg_elem_kind[reg] = RAWK_NONE;
        P.alias_source_param[reg] = -1;
        invalidate_register(reg);
        emit_binary(c, reg, compound_assign_ops[i].op, reg, rk_rhs);
        release_if_top(rk_rhs);
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
            return error_at("'%s' is not defined (a pipe chain's source must already have a value)",
                            aer_as_string(c->pool[name_idx])->data);
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
        return error_at("'%s' is not defined (an indexed/field write target must already have a value)",
                        aer_as_string(c->pool[name_idx])->data);
    }

    bool pending_is_field = !first_is_index;
    unsigned int pending_field_idx = 0;
    int pending_rk_idx = 0;

    if (first_is_index) {
        pending_rk_idx = parse_binary(c, 0);
        /* Boxed unconditionally: the fused opcodes pack the index into a 16-bit RK slot with no raw
           state, and rk16_fits/rk8_fits mask only RK_CONST_FLAG, so a raw-flagged index corrupts the
           encoding (surfacing as a bogus "expression too large"). No-op for the general path. */
        pending_rk_idx = drop_raw_marks(pending_rk_idx);
        require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
    } else {
        if (!equal(TOKEN_IDENTIFIER)) {
            return error_at("Expected field name after '.'");
        }
        pending_field_idx = chunk_add_pool(c, token.value);
        lex();
    }
    if (parse_had_error)
        return;

    /* Fuses into OP_INDEX_FIELD_GET/SET for the same reason the read side does -- a packed array
       has no standalone `name[index]` value. Falls through to the general path for any further
       chaining. */
    if (first_is_index && equal(TOKEN_DOT)) {
        lex();
        if (!equal(TOKEN_IDENTIFIER)) {
            return error_at("Expected field name after '.'");
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
                return error_at("Expression too large to compile (register/constant index exceeds the fused "
                                "index-field-op encoding's range)");
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
                if (parse_had_error)
                    return;
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
                        raw_release_if_top(slot);
                        release_if_top(pending_rk_idx);
                        if (!obj_is_base)
                            reg_free(1);
                        return;
                    }
                }
                rk_val = drop_raw_marks(rk_val);
                if (!rk16_fits(rk_val)) {
                    return error_at(
                        "Expression too large to compile (value exceeds the fused index-field-set "
                        "encoding's range)");
                }
                chunk_emit(c, PACK_OP_A_W16(OP_INDEX_FIELD_SET, obj_reg, pack_rk16(pending_rk_idx)));
                chunk_emit(c, PACK_2X16(fused_field_idx, pack_rk16(rk_val)));
                release_if_top(rk_val);
            } else {
                lex();
                int rk_rhs = parse_binary(c, 0);
                if (parse_had_error)
                    return;
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
                        raw_release_if_top(slot);
                        release_if_top(pending_rk_idx);
                        if (!obj_is_base)
                            reg_free(1);
                        return;
                    }
                }
                rk_rhs = drop_raw_marks(rk_rhs);
                if (!rk16_fits(rk_rhs)) {
                    return error_at(
                        "Expression too large to compile (value exceeds the fused index-field-compound "
                        "encoding's range)");
                }
                chunk_emit(c, PACK3(OP_INDEX_FIELD_COMPOUND, obj_reg, bin_op, 0));
                chunk_emit(c, PACK_2X16(fused_field_idx, pack_rk16(pending_rk_idx)));
                chunk_emit(c, PACK_2X16(0, pack_rk16(rk_rhs)));
                release_if_top(rk_rhs);
            }
            release_if_top(pending_rk_idx);
            if (!obj_is_base)
                reg_free(1);
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

        if (reuse_pending_idx_reg && dest_reg != pending_rk_idx)
            reg_free(1);

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
        if (reuse_pending_idx_reg && dest_reg != pending_rk_idx)
            reg_free(1);

        obj_reg = dest_reg;
        obj_is_base = false;

        if (consume(TOKEN_OPEN_BRACKET)) {
            pending_is_field = false;
            pending_rk_idx = parse_binary(c, 0);
            /* Same fix as the first index step's own -- see that site's comment. */
            pending_rk_idx = drop_raw_marks(pending_rk_idx);
            require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
        } else {
            consume(TOKEN_DOT);
            if (!equal(TOKEN_IDENTIFIER)) {
                return error_at("Expected field name after '.'");
            }
            pending_field_idx = chunk_add_pool(c, token.value);
            pending_is_field = true;
            lex();
        }
        if (parse_had_error)
            return;
    }

    if (consume(TOKEN_ASSIGN)) {
        int rk_val = parse_binary(c, 0);
        if (parse_had_error)
            return;

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
                        raw_release_if_top(slot);
                        return; /* obj_is_base is always true here, so no reg_free(1) for it needed */
                    }
                }
            }
        }
        if (pending_is_field) {
            emit_field_set(c, obj_reg, pending_field_idx, rk_val);
        } else if (index_safe_unchecked(obj_reg, pending_rk_idx)) {
            /* An already-raw value stores straight out of its slot -- boxing it here only to have
               the store tear the AerVal apart again is the whole cost this avoids. */
            RawKind val_kind = rk_raw_kind(c, rk_val);
            if (val_kind != RAWK_NONE) {
                int slot = raw_materialize(c, rk_val, val_kind);
                if (slot >= 0) {
                    chunk_emit(c, PACK3(val_kind == RAWK_INT ? OP_INDEX_SET_RAW_INT : OP_INDEX_SET_RAW_REAL,
                                        obj_reg, pack_rk8(pending_rk_idx), slot));
                    raw_release_if_top(slot);
                    release_if_top(pending_rk_idx);
                    if (!obj_is_base)
                        reg_free(1);
                    return;
                }
            }
            /* pending_rk_idx already guaranteed a plain register by index_safe_unchecked -- always
               fits RK8 directly. rk_val can be anything, so it still needs emit_index_set's own
               box/spill handling, just with the opcode swapped. */
            int rk_val_boxed = drop_raw_marks(rk_val);
            int spilled = 0;
            if (!rk8_fits(rk_val_boxed)) {
                rk_val_boxed = materialize(c, rk_val_boxed);
                spilled++;
            }
            chunk_emit(c, PACK3(OP_TYPED_INDEX_SET_UNCHECKED, obj_reg, pack_rk8(pending_rk_idx),
                                pack_rk8(rk_val_boxed)));
            if (spilled)
                reg_free(spilled);
        } else {
            emit_index_set(c, obj_reg, pending_rk_idx, rk_val);
        }

        release_if_top(rk_val);
        release_if_top(pending_rk_idx);
        if (!obj_is_base)
            reg_free(1);
        return;
    }

    /* Read-modify-write via the same fusion opcodes the single-level cases use -- no dedicated
       OP_INDEX_BINARY fusion exists. */
    for (int i = 0; i < COMPOUND_ASSIGN_OP_COUNT; i++) {
        if (!consume(compound_assign_ops[i].tok))
            continue;

        if (pending_is_field) {
            if (obj_is_base)
                mark_shape_sensitive(obj_reg);

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
            if (parse_had_error)
                return;

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
                    raw_release_if_top(slot);
                    specialized = true;
                }
            }
            if (!specialized) {
                /* One fused OP_FIELD_COMPOUND -- read, compute, and write back in a single
                   dispatch, a single vm_resolve_field call. No temp register needed: the result
                   writes straight back into the same field, never through a register at all. */
                rk_rhs = drop_raw_marks(rk_rhs);

                if (!rk16_fits(rk_rhs)) {
                    return error_at(
                        "Expression too large to compile (register/constant index exceeds the fused "
                        "field-op encoding's range)");
                }
                chunk_emit(c, PACK3(OP_FIELD_COMPOUND, obj_reg, bin_op, 0));
                chunk_emit(c, PACK_2X16(pending_field_idx, pack_rk16(rk_rhs)));
                release_if_top(rk_rhs);
            }
        } else {
            int item_reg = reg_alloc();
            emit_index_get(c, item_reg, obj_reg, pending_rk_idx);

            int rk_rhs = parse_binary(c, 0);
            if (parse_had_error)
                return;

            emit_binary(c, item_reg, compound_assign_ops[i].op, item_reg, rk_rhs);
            release_if_top(rk_rhs);

            emit_index_set(c, obj_reg, pending_rk_idx, item_reg);
            reg_free(1); /* item_reg */
        }

        release_if_top(pending_rk_idx);
        if (!obj_is_base)
            reg_free(1);
        return;
    }

    /* Finish the pending step as a GET, falling through to a general expression statement. */
    if (!equal(TOKEN_OPEN_PARENTHESE) && !equal(TOKEN_PIPE)) {
        return error_at("Expected an assignment ('='), a compound assignment ('+=' etc.), or a call/pipe "
                        "continuation after this chain");
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

        if (reuse_pending_idx_reg && dest_reg != pending_rk_idx)
            reg_free(1);

        int rk = parse_postfix_chain(c, dest_reg);
        unsigned int lhs_start = c->count;
        rk = parse_binary_ops(c, 0, rk, lhs_start);
        discard_statement_result(c, rk);
    }
}

/* Called when the skip-to-boundary loop stops at a plain NEWLINE, not already a boundary -- a
   statement that failed to introduce a block leaves an orphaned indented block otherwise. */
static void skip_orphaned_block(void) {
    if (!equal(TOKEN_NEW_LINE))
        return;
    lex();
    if (!equal(TOKEN_INDENT))
        return;
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
    if (parse_had_error)
        return;
    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_ELSE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE))
            continue;
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

#define COND_PATCH_MAX 32

/* Every jump that has to land where the condition turned out false. */
typedef struct {
    unsigned int at[COND_PATCH_MAX];
    int count;
} CondFalse;

static bool cond_add(CondFalse* f, unsigned int patch) {
    if (f->count >= COND_PATCH_MAX) {
        error_at("Condition too large to compile (more than 32 and/or operands)");
        return false;
    }
    f->at[f->count++] = patch;
    return true;
}

/* Compiles a condition for truth alone, so each comparison fuses with its own branch: `and` sends
   every conjunct's false-jump to one place, `or` sends an alternative's to the next alternative.
   compile_and cannot, because `and` yields the operand rather than a bool -- it must materialize
   each one, and its short-circuit jump then lands ON the second comparison, which is what stops
   emit_cond_jump_if_false folding that comparison into the branch it targets. */
static bool compile_condition(Chunk* c, CondFalse* out) {
    out->count = 0;
    unsigned int true_at[COND_PATCH_MAX];
    int true_count = 0;
    for (;;) {
        CondFalse alt;
        alt.count = 0;
        for (;;) {
            unsigned int start = c->count;
            /* Precedence 2 stops at `and` itself and at the two prec-1 operators below it. */
            int rk = parse_binary(c, 2);
            if (parse_had_error || !cond_add(&alt, emit_cond_jump_if_false(c, rk, start)))
                return false;
            if (!equal(TOKEN_AND))
                break;
            lex();
        }
        /* `|>` binds looser than `and`, so here it would pipe the whole chain's value -- which this
           lowering deliberately never produces. Nothing useful can be written that way anyway
           (`x |> f() > 0` already parses as `x |> (f() > 0)`), so it is refused, not miscompiled. */
        if (equal(TOKEN_PIPE)) {
            error_at("'|>' cannot be applied to a condition; parenthesise the piped expression");
            return false;
        }
        if (!equal(TOKEN_OR)) {
            for (int i = 0; i < alt.count; i++)
                if (!cond_add(out, alt.at[i]))
                    return false;
            break;
        }
        lex();
        /* This alternative held, so the body runs and the remaining ones are skipped. */
        chunk_emit(c, OP_JUMP);
        if (true_count >= COND_PATCH_MAX) {
            error_at("Condition too large to compile (more than 32 and/or operands)");
            return false;
        }
        true_at[true_count++] = c->count;
        chunk_emit(c, 0);
        for (int i = 0; i < alt.count; i++)
            patch_jump(c, alt.at[i], c->count);
    }
    for (int i = 0; i < true_count; i++)
        patch_jump(c, true_at[i], c->count);
    return true;
}

static void parse_if(Chunk* c) {
    CondFalse cond;
    bool ok = compile_condition(c, &cond);
    require(TOKEN_COLON, "expected ':' after if condition");
    /* require() can't abort on failure -- without this, a malformed condition still compiles a
       real branch. */
    if (!ok || parse_had_error)
        return;

    /* P.branch_depth disqualifies a variable assigned while nonzero from ever being raw-tracked --
       a name assigned different types down mutually-exclusive branches can't be resolved without
       real dataflow analysis. A counter since if/else nests. */
    P.branch_depth++;
    parse_block(c);
    P.branch_depth--;
    if (parse_had_error)
        return;

    if (consume(TOKEN_ELSE)) {
        require(TOKEN_COLON, "expected ':' after else");
        if (parse_had_error)
            return;
        chunk_emit(c, OP_JUMP);
        unsigned int patch_jmp = c->count;
        chunk_emit(c, 0);
        for (int i = 0; i < cond.count; i++)
            patch_jump(c, cond.at[i], c->count);
        P.branch_depth++;
        parse_block(c);
        P.branch_depth--;
        patch_jump(c, patch_jmp, c->count);
    } else {
        for (int i = 0; i < cond.count; i++)
            patch_jump(c, cond.at[i], c->count);
    }
}

/* Shared while/for-while tail: require ':', branch-if-false, body, back-edge to loop_top. */
/* Shared body-and-back-edge tail for every loop form. Callers that promoted P.slot_floor can
   safely restore it unconditionally after this returns either way. */
static void parse_loop_body(Chunk* c, unsigned int loop_top, unsigned int patch_exit) {
    if (!loop_push(loop_top))
        return;
    parse_block(c);
    if (parse_had_error) {
        P.loop_depth--;
        return;
    }

    chunk_emit(c, OP_JUMP);
    emit_jump_target(c, loop_top);
    patch_jump(c, patch_exit, c->count);
    loop_pop_and_patch(c, c->count);
}

/* Rotated form of parse_loop_body: the condition is re-emitted after the body and branches back,
   so an iteration costs one branch instead of a branch plus an unconditional jump. Evaluation
   count is unchanged -- the guard runs once and the back-edge runs once per iteration, which is
   the same N+1 the top-tested form ran, so a condition that can raise raises exactly as often. */
static void parse_loop_body_rotated(Chunk* c, unsigned int body_top, unsigned int patch_exit, Opcode back_op,
                                    uint8_t lhs, uint8_t rhs) {
    if (!loop_push_rotated())
        return;
    parse_block(c);
    if (parse_had_error) {
        P.loop_depth--;
        return;
    }

    unsigned int cond_pos = c->count;
    chunk_emit(c, PACK3(back_op, 0, lhs, rhs));
    unsigned int patch_back = c->count;
    chunk_emit(c, 0);
    patch_jump(c, patch_back, body_top);

    patch_jump(c, patch_exit, c->count);
    /* `continue` lands on the re-emitted condition, not on the body's first instruction -- it must
       still test before going round again. */
    loop_pop_and_patch_rotated(c, c->count, cond_pos);
}

static void parse_for_body(Chunk* c, unsigned int loop_top, int rk_cond) {
    require(TOKEN_COLON, "expected ':' after for/while condition");
    if (parse_had_error)
        return;

    /* Emitted BEFORE the floor is raised below: the compare/branch fusion only fires when the
       preceding LOADK wrote a temp, and raising the floor first disqualifies every such loop. */
    unsigned int patch_exit = emit_cond_jump_if_false(c, rk_cond, loop_top);

    /* Rotatable only when the whole condition IS the fused branch. Anything computed ahead of it
       would have to be duplicated at the back-edge too, and copying that region is a separate
       problem: a jump inside it that leaves it carries a delta only valid where it was emitted. */
    Opcode back_op = OP_HALT;
    uint8_t back_lhs = 0, back_rhs = 0;
    bool rotate = false;
    if (patch_exit == loop_top + 1) {
        uint32_t w = c->code[loop_top];
        bool swap;
        if (complement_branch((Opcode)(w & 0xFF), &back_op, &swap)) {
            uint8_t a = (uint8_t)UNPACK_B(w), b = (uint8_t)UNPACK_C(w);
            /* These opcodes take a constant in the right operand only, so an ordering test against
               a literal bound (`for i < 5:`) cannot be complemented by swapping -- the complement
               of `i < K` wants K on the left. Such loops keep the top-tested shape. */
            bool swap_encodable = !RK8_IS_CONST(a) && !RK8_IS_CONST(b);
            if (!swap || swap_encodable) {
                back_lhs = swap ? b : a;
                back_rhs = swap ? a : b;
                rotate = true;
            }
        }
    }

    /* The back-edge re-runs the condition, so holding the floor above the temps it writes stops a
       shadowing assignment in the body from claiming one as a permanent variable. */
    int saved_floor = P.slot_floor;
    int raised_floor = saved_floor;
    if (P.loop_cond_peak > P.slot_floor) {
        raised_floor = P.loop_cond_peak;
        P.slot_floor = raised_floor;
        if (P.slot_next < P.slot_floor)
            P.slot_next = P.slot_floor;
    }

    if (rotate)
        parse_loop_body_rotated(c, patch_exit + 1, patch_exit, back_op, back_lhs, back_rhs);
    else
        parse_loop_body(c, loop_top, patch_exit);

    /* Only give the condition's registers back if the body claimed nothing above them. AER scopes
       variables to the whole function, so a name first assigned inside the body outlives the loop
       and its register must stay reserved -- lowering the floor past it would hand a live variable
       out as a temp to the next statement. */
    if (P.slot_floor == raised_floor) {
        P.slot_floor = saved_floor;
        if (P.slot_next < P.slot_floor)
            P.slot_next = P.slot_floor;
    }
    assert_variables_below_floor("for-loop body");
}

/* A query's loop body runs longer than a reduction's: two aggregates and a count under one filter
   is around eighteen instructions, and at sixteen the recogniser gave up before reaching them. */
#define VEC_MAX_OPS 32

/* Rewrites a range-for into one whole-array reduction -- `for i in 0..length(a): t = t + a[i] * 2`
   becomes `t = t + collection.sum(a * 2)`. Called with the body already compiled, and rewinds over
   it on success; an unrecognised instruction returns false and leaves the loop exactly as it was.

   Integer accumulators only, on purpose: integer addition is associative and exact, so the four
   partial sums collection.sum keeps answer bit-identically, which a float accumulator would not. */
/* Every opcode accepted below is one word, which is what makes walking the body safe -- an
   unrecognised one stops the walk before its length would have mattered. */
#define CHAIN_MAX_LEAVES 8
/* The postfix program fills bits 0..59 (15 tokens, 4 bits each), so the filter's leaf count rides
   in the three above it rather than costing another call argument. Three bits is enough because a
   split leaves at least one leaf for the values. */
#define CHAIN_SPLIT_SHIFT 60

/* Each accepted operator's postfix token, matching aer_collection.c's CHAIN_TOK_*. Comparisons are
   in because a filter is the outermost operator of most array expressions worth fusing: without them
   `(quantity > 10) * (price < 400)` costs three whole-array passes to build a mask the expression
   that follows reads once. */
static bool chain_token_for(Opcode op, unsigned int* tok) {
    switch (op) {
        case OP_ADD: *tok = 1; return true;
        case OP_SUB: *tok = 2; return true;
        case OP_MUL: *tok = 3; return true;
        case OP_LT: *tok = 4; return true;
        case OP_LTE: *tok = 5; return true;
        case OP_GT: *tok = 6; return true;
        case OP_GTE: *tok = 7; return true;
        default: return false;
    }
}

typedef struct {
    unsigned int tok;
    /* Resolved when the operator is walked, not looked up afterwards: the parser reuses temp
       registers freely, so `reg7` names three different values across one expression and only the
       reader at the time knows which. */
    int child[2]; /* the node that produced this operand, or -1 for a leaf */
    int rk[2]; /* the leaf's operand, register or pool constant, when child is -1 */
} ChainNode;

/* Post-order, left child before right, so the tile evaluator's stack holds the two operands the
   right way round. Emission order alone would not: `mask * (a - b)` compiles the subtraction before
   it ever mentions mask, which puts the operands on the stack backwards -- harmless for `*`, wrong
   for every operator that is not commutative. */
static bool chain_postfix(const ChainNode* node, int idx, int* leaf, int* nleaf, uint64_t* prog, int* ntok) {
    for (int s = 0; s < 2; s++) {
        if (node[idx].child[s] >= 0) {
            if (!chain_postfix(node, node[idx].child[s], leaf, nleaf, prog, ntok))
                return false;
        } else {
            if (*nleaf >= CHAIN_MAX_LEAVES)
                return false;
            leaf[(*nleaf)++] = node[idx].rk[s];
            (*ntok)++; /* CHAIN_TOK_PUSH is zero, so a push contributes no bits */
        }
    }
    *prog |= (uint64_t)node[idx].tok << (4 * (*ntok));
    (*ntok)++;
    return true;
}

/* Recognises the compiled argument as a TREE of array operators -- `p * q * (1.0 - d) * k`, whose
   second operator starts a fresh subexpression, so the left-associative run this used to accept
   declined the very query that most wanted fusing. Returns the leaf count (0 for anything else) and
   the postfix program four bits per token; leaves are RK operands, so a constant is a leaf like any
   other and the tile evaluator broadcasts it. */
static int chain_of_array_ops(Chunk* c, unsigned int start, unsigned int end, int result_reg, int* leaf,
                              uint64_t* prog, int* mask_leaves) {
    ChainNode node[CHAIN_MAX_LEAVES];
    int refs[CHAIN_MAX_LEAVES];
    int producer[FRAME_REGISTERS];
    bool written[FRAME_REGISTERS];
    for (int i = 0; i < FRAME_REGISTERS; i++) {
        producer[i] = -1;
        written[i] = false;
    }

    int nops = 0;
    for (unsigned int at = start; at < end; at++) {
        uint32_t w = c->code[at];
        unsigned int tok;
        if (!chain_token_for((Opcode)(w & 0xFF), &tok))
            return 0;
        uint8_t d = (uint8_t)UNPACK_A(w), side[2] = {(uint8_t)UNPACK_B(w), (uint8_t)UNPACK_C(w)};
        /* A tree over k leaves has k-1 operators, so this bound follows from the leaf bound. */
        if (nops >= CHAIN_MAX_LEAVES || d >= FRAME_REGISTERS)
            return 0;
        node[nops].tok = tok;
        refs[nops] = 0;
        for (int s = 0; s < 2; s++) {
            if (RK8_IS_CONST(side[s])) {
                node[nops].child[s] = -1;
                node[nops].rk[s] = (int)(RK_CONST_FLAG | RK8_INDEX(side[s]));
                continue;
            }
            node[nops].child[s] = producer[side[s]];
            node[nops].rk[s] = side[s];
            if (producer[side[s]] >= 0)
                refs[producer[side[s]]]++;
        }
        producer[d] = nops;
        written[d] = true;
        nops++;
    }
    /* Two operators minimum: that is where fusing was measured to pay (0.0514s -> 0.0237s on four
       terms). At one it only saves a single materialisation, which the tile copies here give back. */
    if (nops < 2 || result_reg < 0 || result_reg >= FRAME_REGISTERS || producer[result_reg] != nops - 1)
        return 0;
    /* One tree with the last operator at its root: every other result feeds exactly one operator. A
       result read twice is a shared subexpression, and the tile evaluator would compute it twice. */
    for (int i = 0; i < nops; i++)
        if (refs[i] != (i == nops - 1 ? 0 : 1))
            return 0;

    /* A filter multiplied over the values -- `price * quantity * (price < 400.0)` -- is emitted as
       two programs rather than one, the filter's first. The runtime can then evaluate the filter
       alone, see how much it keeps, and read the value columns for surviving rows only: a masked
       pass costs the same at 1% selectivity as at 80%, where every other engine gets ~7x cheaper.
       Splitting is what makes that choice available; the choice itself is the runtime's. */
    int nleaf = 0, ntok = 0;
    int root = nops - 1;
    *mask_leaves = 0;
    if (node[root].tok == 3) { /* CHAIN_TOK_MUL */
        for (int s = 0; s < 2; s++) {
            int m = node[root].child[s], v = node[root].child[1 - s];
            /* The filter has to BE a comparison, and the values have to be a real subexpression --
               splitting `a * (b < c)` would leave a one-leaf value program that gains nothing. */
            if (m < 0 || node[m].tok < 4 || v < 0)
                continue;
            /* Values first, filter second -- the order natural postfix already produces, and the
               order that matters: a filter emitted first sits on the tile stack for the whole of the
               value expression, which pushed the working set past L1 and cost 28% on a query that
               keeps most of its rows. */
            int before = nleaf;
            if (!chain_postfix(node, v, leaf, &nleaf, prog, &ntok))
                return 0;
            int value_leaves = nleaf - before;
            if (!chain_postfix(node, m, leaf, &nleaf, prog, &ntok) || nleaf - value_leaves > 7)
                return 0; /* 7 is what the three bits carrying the filter's leaf count can hold */
            *mask_leaves = nleaf - value_leaves;
            /* The root multiply STAYS. Splitting only tells the runtime where the filter begins; the
               whole program must remain runnable as one, because a filter that keeps most rows
               should still be evaluated in a single fused pass. */
            *prog |= (uint64_t)node[root].tok << (4 * ntok);
            ntok++;
            if (ntok != 2 * nleaf - 1)
                return 0;
            break;
        }
    }
    if (*mask_leaves == 0 && (!chain_postfix(node, root, leaf, &nleaf, prog, &ntok) || ntok != 2 * nleaf - 1))
        return 0;
    /* Every leaf must still hold what it held at call time -- a register the expression also writes
       holds the later value by then, not the one the leaf was read for. */
    for (int i = 0; i < nleaf; i++)
        if (!(leaf[i] & RK_CONST_FLAG) && written[leaf[i]])
            return 0;
    /* At least one operand has to be a typed array, or this is scalar arithmetic. */
    for (int i = 0; i < nleaf; i++)
        if (!(leaf[i] & RK_CONST_FLAG) && P.reg_elem_kind[leaf[i]] == RAWK_REAL)
            return nleaf;
    return 0;
}

/* Leaves arrive as RK operands, so a constant loads from the pool where a register only moves. */
static void chain_emit_leaf(Chunk* c, int dest, int rk) {
    if (rk & RK_CONST_FLAG)
        chunk_emit(c, PACK_OP_A_W16(OP_LOADK, dest, (unsigned int)(rk & ~RK_CONST_FLAG)));
    else
        chunk_emit(c, PACK2(OP_MOVE, dest, rk));
}

/* The fused compare-and-branch forms, mapped to the operator whose 1/0 mask says the same thing.
   The branch jumps when the condition is FALSE, so the mask is the condition itself. */
static bool vec_guard_mask_op(Opcode op, Opcode* out) {
    switch (op) {
        case OP_RAW_LT_INT_JUMP_IF_FALSE:
        case OP_RAW_LT_REAL_JUMP_IF_FALSE:
        case OP_LT_JUMP_IF_FALSE: *out = OP_LT; return true;
        case OP_RAW_LTE_INT_JUMP_IF_FALSE:
        case OP_RAW_LTE_REAL_JUMP_IF_FALSE:
        case OP_LTE_JUMP_IF_FALSE: *out = OP_LTE; return true;
        case OP_GT_JUMP_IF_FALSE: *out = OP_GT; return true;
        case OP_GTE_JUMP_IF_FALSE: *out = OP_GTE; return true;
        default: return false;
    }
}

/* A raw comparison's constant operand indexes the chunk's raw int/real table, which the array-level
   operator cannot read -- it takes pool entries. Decoding one as the other is what once turned
   `price < 400.0` into "Cannot apply '<' to float[] and string". */
static int vec_guard_const(Chunk* c, uint8_t rk8, bool real) {
    unsigned int at = RK8_INDEX(rk8);
    AerVal v = real ? aer_real(c->rawk_d[at]) : aer_int(c->rawk_i[at]);
    return (int)(RK_CONST_FLAG | chunk_add_pool(c, v));
}

static bool rk_is_int_const(Chunk* c, int rk, int64_t want) {
    if (!(rk & RK_CONST_FLAG))
        return false;
    AerVal v = c->pool[rk & ~RK_CONST_FLAG];
    return aer_type(v) == TYPE_INTEGER && aer_as_int(v) == want;
}

typedef struct {
    Opcode op;
    uint8_t d, x, y;
    bool guard;
    bool accumulate; /* a reduction's op that closes a scalar accumulate */
    bool real_const; /* a guard whose constant lives in the raw REAL table rather than the int one */
} VecOp;

/* Which instructions a recogniser will even look at. Given the whole word because one of them has to
   range-check a constant table index it carries. */
typedef bool (*VecAccepts)(Chunk*, uint32_t, Opcode, const void*);

/* Walks a loop body into a flat op list. Shared because both recognisers need exactly this, and the
   filter handling is the fiddly part: `if <cond>:` compiles to a branch occupying two words which
   must skip to the very END of the body, or it is control flow rather than a WHERE clause. Returns
   the op count, or -1 for a body neither can use. */
static int vec_collect_body(Chunk* c, unsigned int body_start, unsigned int body_end, VecOp* ops,
                            int* out_guards, VecAccepts accepts, const void* ctx) {
    int n = 0, guards = 0;
    for (unsigned int at = body_start; at < body_end; at++) {
        if (n >= VEC_MAX_OPS)
            return -1;
        uint32_t w = c->code[at];
        Opcode op = (Opcode)(w & 0xFF);
        Opcode mask_op;
        if (vec_guard_mask_op(op, &mask_op)) {
            if ((unsigned int)((int)at + 2 + (int)(int32_t)c->code[at + 1]) != body_end)
                return -1;
            ops[n] = (VecOp){.op = mask_op,
                             .x = (uint8_t)UNPACK_B(w),
                             .y = (uint8_t)UNPACK_C(w),
                             .guard = true,
                             .real_const = op == OP_RAW_LT_REAL_JUMP_IF_FALSE ||
                                           op == OP_RAW_LTE_REAL_JUMP_IF_FALSE || op == OP_LT_JUMP_IF_FALSE ||
                                           op == OP_LTE_JUMP_IF_FALSE || op == OP_GT_JUMP_IF_FALSE ||
                                           op == OP_GTE_JUMP_IF_FALSE};
            n++;
            guards++;
            at++; /* the branch's own target word */
            continue;
        }
        if (!accepts(c, w, op, ctx))
            return -1;
        ops[n] = (VecOp){
            .op = op, .d = (uint8_t)UNPACK_A(w), .x = (uint8_t)UNPACK_B(w), .y = (uint8_t)UNPACK_C(w)};
        n++;
    }
    *out_guards = guards;
    return n;
}

/* The arithmetic a reduction accepts depends on the element's kind, so the kind rides along. */
typedef struct {
    bool is_int;
} VecReduceCtx;

static bool vec_accepts_reduction(Chunk* c, uint32_t w, Opcode op, const void* vctx) {
    bool is_int = ((const VecReduceCtx*)vctx)->is_int;
    bool k_form = is_int && (op == OP_RAW_ADD_INT_K || op == OP_RAW_SUB_INT_K);
    if (op != (is_int ? OP_INDEX_GET_RAW_INT : OP_INDEX_GET_RAW_REAL) && op != OP_TYPED_INDEX_GET_UNCHECKED &&
        op != (is_int ? OP_RAW_ADD_INT : OP_RAW_ADD_REAL) &&
        op != (is_int ? OP_RAW_SUB_INT : OP_RAW_SUB_REAL) &&
        op != (is_int ? OP_RAW_MUL_INT : OP_RAW_MUL_REAL) && !k_form)
        return false;
    /* The K forms carry a bare index into the chunk's raw-int table where the others carry a
       register, so it is read back to a value later rather than passed through as an operand. */
    return !(k_form && UNPACK_C(w) >= c->rawk_i_count);
}

static bool vec_accepts_group(Chunk* c, uint32_t w, Opcode op, const void* ctx) {
    (void)c;
    (void)w;
    (void)ctx;
    return op == OP_TYPED_INDEX_GET_UNCHECKED || op == OP_INDEX_GET_RAW_REAL || op == OP_RAW_REAL_TO_INT ||
           op == OP_RAW_ADD_REAL || op == OP_RAW_SUB_REAL || op == OP_RAW_MUL_REAL ||
           op == OP_INDEX_SET_RAW_REAL || op == OP_RAW_MOVE_INT || op == OP_RAW_MOVE_REAL || op == OP_MOVE;
}

static bool try_vectorize_reduction(Chunk* c, unsigned int prep_at, unsigned int body_start,
                                    unsigned int body_end, int item_reg, int array_reg, int rk_start,
                                    int rk_step) {
    if (array_reg < 0 || array_reg >= FRAME_REGISTERS || item_reg < 0 || item_reg >= FRAME_REGISTERS)
        return false;
    /* The reduction covers the whole array, so the loop has to as well. A start of `(i * i)` or a
       step of 2 visits a subset, and summing everything would answer a different question -- which
       is exactly what tests/test_nonneg_range_start.aer caught. */
    if (!rk_is_int_const(c, rk_start, 0) || !rk_is_int_const(c, rk_step, 1))
        return false;
    RawKind ek = P.reg_elem_kind[array_reg];
    if (ek != RAWK_INT && ek != RAWK_REAL)
        return false;
    bool is_int = ek == RAWK_INT;

    Opcode op_add = is_int ? OP_RAW_ADD_INT : OP_RAW_ADD_REAL;
    Opcode op_sub = is_int ? OP_RAW_SUB_INT : OP_RAW_SUB_REAL;
    Opcode op_get = is_int ? OP_INDEX_GET_RAW_INT : OP_INDEX_GET_RAW_REAL;

    VecOp ops[VEC_MAX_OPS];
    VecReduceCtx accept_ctx = {is_int};
    int guards = 0;
    int n = vec_collect_body(c, body_start, body_end, ops, &guards, vec_accepts_reduction, &accept_ctx);
    if (n < 2)
        return false;

    /* -1 = not array-derived. A body may hold several independent statements -- `sx = sx + a[i]`
       and `sy = sy + b[i]` in one loop are two reductions, not one -- so an accumulate is
       recognised wherever it appears rather than only as the last instruction. */
    int from_array[FRAME_REGISTERS];
    bool is_acc_slot[FRAME_REGISTERS];
    for (int i = 0; i < FRAME_REGISTERS; i++) {
        from_array[i] = -1;
        is_acc_slot[i] = false;
    }
    int acc_count = 0;
    for (int i = 0; i < n; i++) {
        bool k_form = ops[i].op == OP_RAW_ADD_INT_K || ops[i].op == OP_RAW_SUB_INT_K;
        if (ops[i].guard) {
            /* A constant here indexes the RAW constant table, not the pool the array-level operator
               would read -- decoding it as a pool entry produced a string. A literal bound is
               normally hoisted into a register by the loop preheader anyway, which is the form this
               accepts; an un-hoisted one declines rather than being mis-resolved. */
            if (RK8_IS_CONST(ops[i].x) || RK8_IS_CONST(ops[i].y))
                return false;
            /* The mask has to vary per row, so at least one side must come from a column. */
            if (from_array[ops[i].x] < 0 && from_array[ops[i].y] < 0)
                return false;
            continue;
        }
        if (ops[i].op == op_get || ops[i].op == OP_TYPED_INDEX_GET_UNCHECKED) {
            if (ops[i].y != (uint8_t)item_reg)
                return false;
            /* A second column may join in, but only once its length is known to match the one the
               loop was bounded by -- otherwise the whole-array pass would read a different number
               of elements than the loop did. Both being built from one count value proves it. */
            if (ops[i].x != (uint8_t)array_reg &&
                !(P.reg_len_class[ops[i].x] != 0 && P.reg_len_class[ops[i].x] == P.reg_len_class[array_reg] &&
                  P.reg_elem_kind[ops[i].x] == P.reg_elem_kind[array_reg]))
                return false;
            from_array[ops[i].d] = ops[i].x;
            continue;
        }
        /* `t = t + <array-derived>` where t is not itself array-derived closes a statement. */
        if (ops[i].op == op_add && ops[i].d == ops[i].x && from_array[ops[i].x] < 0 &&
            !RK8_IS_CONST(ops[i].y) && from_array[ops[i].y] >= 0) {
            ops[i].accumulate = true;
            is_acc_slot[ops[i].d] = true;
            acc_count++;
            continue;
        }
        if (k_form) {
            /* The constant is the whole of the right operand, so only the left can carry the array. */
            if (RK8_IS_CONST(ops[i].x) || from_array[ops[i].x] < 0)
                return false;
            from_array[ops[i].d] = 1;
            continue;
        }
        /* At least one side has to carry the array. The other may be a second column of the same
           length, a constant, or a loop-invariant scalar -- the array-level operators handle all
           three, the last two by broadcasting. */
        bool ax = !RK8_IS_CONST(ops[i].x) && from_array[ops[i].x] >= 0;
        bool ay = !RK8_IS_CONST(ops[i].y) && from_array[ops[i].y] >= 0;
        if (!ax && !ay)
            return false;
        from_array[ops[i].d] = 1; /* a real register is filled in during emission */
    }
    if (acc_count == 0)
        return false;
    /* An accumulator may only ever be read as its own running total. Anywhere else and the total is
       part of the arithmetic, which one pass over the array cannot reproduce. */
    for (int i = 0; i < n; i++) {
        if (ops[i].accumulate || ops[i].guard)
            continue;
        bool k_form = ops[i].op == OP_RAW_ADD_INT_K || ops[i].op == OP_RAW_SUB_INT_K;
        if (is_acc_slot[ops[i].d] || (!RK8_IS_CONST(ops[i].x) && is_acc_slot[ops[i].x]) ||
            (!k_form && !RK8_IS_CONST(ops[i].y) && is_acc_slot[ops[i].y]))
            return false;
    }

    /* Committed: the loop's own bytecode goes, and the array-level form takes its place. The
       registers the body claimed stay claimed, which wastes a few and cannot misbehave. */
    c->count = prep_at;
    unsigned int mod_idx = chunk_add_pool(c, aer_make_string_copy("collection", 10));
    unsigned int fn_idx = chunk_add_pool(c, aer_make_string_copy("sum", 3));
    int value[FRAME_REGISTERS];
    for (int i = 0; i < FRAME_REGISTERS; i++)
        value[i] = -1;
    int mask_reg = -1;

    for (int i = 0; i < n; i++) {
        if (ops[i].op == op_get || ops[i].op == OP_TYPED_INDEX_GET_UNCHECKED) {
            value[ops[i].d] = ops[i].x; /* the array itself replaces the element load */
            continue;
        }
        if (ops[i].guard) {
            /* Both are plain registers -- validation refused a constant operand above. */
            int gx = value[ops[i].x] >= 0 ? value[ops[i].x] : (int)ops[i].x;
            int gy = value[ops[i].y] >= 0 ? value[ops[i].y] : (int)ops[i].y;
            int one_mask = reg_alloc();
            if (one_mask < 0)
                return false;
            emit_binary(c, one_mask, ops[i].op, gx, gy);
            P.reg_elem_kind[one_mask] = ek; /* 1 or 0 in the element's own type */
            if (mask_reg < 0) {
                mask_reg = one_mask;
            } else {
                /* `and` compiles to one branch per conjunct, all skipping to the same place, so
                   several guards multiply: 1 only where every one of them held. */
                int combined = reg_alloc();
                if (combined < 0)
                    return false;
                emit_binary(c, combined, OP_MUL, mask_reg, one_mask);
                P.reg_elem_kind[combined] = ek;
                mask_reg = combined;
            }
            continue;
        }
        if (ops[i].accumulate) {
            int summed = value[ops[i].y];
            if (mask_reg >= 0) {
                int masked = reg_alloc();
                if (masked < 0)
                    return false;
                emit_binary(c, masked, OP_MUL, summed, mask_reg);
                P.reg_elem_kind[masked] = ek;
                summed = masked;
            }
            int sum_reg = reg_alloc();
            if (sum_reg < 0)
                return false;
            chunk_emit(c, PACK2(OP_MOVE, sum_reg, summed));
            chunk_emit(c, PACK3(OP_CALL_MODULE, sum_reg, sum_reg, 1));
            chunk_emit(c, (uint32_t)mod_idx);
            chunk_emit(c, (uint32_t)fn_idx);
            chunk_emit(c, PACK_2X16((uint16_t)CALL_MODULE_COLLECTION, (uint16_t)FN_COLLECTION_SUM));
            int tmp = slot_alloc(ek);
            if (tmp < 0)
                return false;
            chunk_emit(c, PACK3(is_int ? OP_UNBOX_INT : OP_UNBOX_REAL, tmp, sum_reg, 0));
            chunk_emit(c, PACK3(op_add, ops[i].d, ops[i].d, tmp));
            continue;
        }
        bool k_form = ops[i].op == OP_RAW_ADD_INT_K || ops[i].op == OP_RAW_SUB_INT_K;
        int rk_x = RK8_IS_CONST(ops[i].x) ? (int)(RK_CONST_FLAG | RK8_INDEX(ops[i].x))
                                          : (value[ops[i].x] >= 0 ? value[ops[i].x] : (int)ops[i].x);
        int rk_y;
        if (k_form)
            rk_y = (int)(RK_CONST_FLAG | chunk_add_pool(c, aer_int(c->rawk_i[ops[i].y])));
        else
            rk_y = RK8_IS_CONST(ops[i].y) ? (int)(RK_CONST_FLAG | RK8_INDEX(ops[i].y))
                                          : (value[ops[i].y] >= 0 ? value[ops[i].y] : (int)ops[i].y);
        Opcode boxed;
        if (ops[i].op == op_add || ops[i].op == OP_RAW_ADD_INT_K)
            boxed = OP_ADD;
        else if (ops[i].op == op_sub || ops[i].op == OP_RAW_SUB_INT_K)
            boxed = OP_SUB;
        else
            boxed = OP_MUL;
        int dest = reg_alloc();
        if (dest < 0)
            return false;
        emit_binary(c, dest, boxed, rk_x, rk_y);
        P.reg_elem_kind[dest] = ek; /* elementwise over a typed array yields a typed array */
        value[ops[i].d] = dest;
    }
    return true;
}

/* A GROUP BY with a WHERE clause and more than one aggregate -- what a query actually looks like.
   Becomes one scattering pass per aggregate, all sharing one filter, adding into the arrays the loop
   did so their identity survives. Kept apart from try_vectorize_reduction because these statements
   end in an indexed STORE whose index is another column's element; a scalar accumulate alongside
   them is recognised too, since COUNT(*) is always written that way. */
#define VEC_MAX_STORES 4
#define VEC_MAX_ACCS 4

/* One aggregate's worth of emission, so the walk that drives them stays readable. `slot` advances
   through the contiguous run the caller reserved. */
static void emit_group_scatter(Chunk* c, int* slot, int argc, int target, int group_col, int values,
                               int mask_reg, unsigned int mod_idx, unsigned int fn_idx) {
    chunk_emit(c, PACK2(OP_MOVE, *slot, target));
    chunk_emit(c, PACK2(OP_MOVE, *slot + 1, values));
    chunk_emit(c, PACK2(OP_MOVE, *slot + 2, group_col));
    if (mask_reg >= 0)
        chunk_emit(c, PACK2(OP_MOVE, *slot + 3, mask_reg));
    chunk_emit(c, PACK3(OP_CALL_MODULE, *slot, *slot, argc));
    chunk_emit(c, (uint32_t)mod_idx);
    chunk_emit(c, (uint32_t)fn_idx);
    chunk_emit(c, PACK_2X16((uint16_t)CALL_MODULE_COLLECTION, (uint16_t)FN_COLLECTION_GROUP_SUM_INTO));
    *slot += argc;
}

static bool emit_scalar_reduction(Chunk* c, int* slot, int acc_reg, int values, unsigned int mod_idx,
                                  unsigned int fn_idx) {
    chunk_emit(c, PACK2(OP_MOVE, *slot, values));
    chunk_emit(c, PACK3(OP_CALL_MODULE, *slot, *slot, 1));
    chunk_emit(c, (uint32_t)mod_idx);
    chunk_emit(c, (uint32_t)fn_idx);
    chunk_emit(c, PACK_2X16((uint16_t)CALL_MODULE_COLLECTION, (uint16_t)FN_COLLECTION_SUM));
    int tmp = slot_alloc(RAWK_REAL);
    if (tmp < 0)
        return false;
    chunk_emit(c, PACK3(OP_UNBOX_REAL, tmp, *slot, 0));
    chunk_emit(c, PACK3(OP_RAW_ADD_REAL, acc_reg, acc_reg, tmp));
    *slot += 2;
    return true;
}

static bool try_vectorize_group_accumulate(Chunk* c, unsigned int prep_at, unsigned int body_start,
                                           unsigned int body_end, int item_reg, int array_reg, int rk_start,
                                           int rk_step) {
    if (array_reg < 0 || array_reg >= FRAME_REGISTERS || item_reg < 0 || item_reg >= FRAME_REGISTERS)
        return false;
    if (!rk_is_int_const(c, rk_start, 0) || !rk_is_int_const(c, rk_step, 1))
        return false;

    VecOp ops[VEC_MAX_OPS];
    int guards = 0;
    int n = vec_collect_body(c, body_start, body_end, ops, &guards, vec_accepts_group, NULL);
    if (n < 0)
        return false;

    /* from_col[r]: r holds an element of that column (or 1 for something derived from one).
       group_of[r]: r indexes a group, and names the column the index came from.
       total_of[r]: r is a `t[g]` read, and names which t and which g. */
    int from_col[FRAME_REGISTERS], group_of[FRAME_REGISTERS];
    int total_target[FRAME_REGISTERS], total_gidx[FRAME_REGISTERS];
    int closes_total[FRAME_REGISTERS], closes_gidx[FRAME_REGISTERS], closes_value[FRAME_REGISTERS];
    bool is_acc[FRAME_REGISTERS];
    for (int i = 0; i < FRAME_REGISTERS; i++) {
        from_col[i] = -1;
        group_of[i] = -1;
        total_target[i] = -1;
        total_gidx[i] = -1;
        closes_total[i] = -1;
        closes_gidx[i] = -1;
        closes_value[i] = -1;
        is_acc[i] = false;
    }
    /* Only the counts are needed: what each aggregate scatters is settled during emission, where the
       registers holding it are still current. These bound the argument run reserved below. */
    int nstore = 0, naccum = 0;

    for (int i = 0; i < n; i++) {
        uint8_t d = ops[i].d, x = ops[i].x, y = ops[i].y;
        if (ops[i].guard) {
            /* A constant here indexes the RAW constant table, which the array-level comparison
               cannot read -- it is resolved to a pool entry at emission. What cannot be resolved is
               a mask that does not vary per row, so one side has to come from a column. */
            bool cx = RK8_IS_CONST(x), cy = RK8_IS_CONST(y);
            if (cx && cy)
                return false;
            if ((cx || from_col[x] < 0) && (cy || from_col[y] < 0))
                return false;
            continue;
        }
        if (RK8_IS_CONST(x) || RK8_IS_CONST(y))
            return false;
        if (ops[i].op == OP_TYPED_INDEX_GET_UNCHECKED || ops[i].op == OP_INDEX_GET_RAW_REAL) {
            if (y == (uint8_t)item_reg) {
                /* A column read: this row's element of x. A second column joins in only once its
                   length is known to match the one the loop was bounded by. */
                if (x != (uint8_t)array_reg &&
                    !(P.reg_len_class[x] != 0 && P.reg_len_class[x] == P.reg_len_class[array_reg]))
                    return false;
                from_col[d] = x;
                total_target[d] = -1;
            } else if (group_of[y] >= 0) {
                total_target[d] = x; /* a running total for one group of x */
                total_gidx[d] = y;
                from_col[d] = -1;
            } else {
                return false;
            }
            continue;
        }
        if (ops[i].op == OP_RAW_REAL_TO_INT) {
            if (from_col[x] < 0)
                return false;
            group_of[d] = from_col[x];
            continue;
        }
        /* Naming the group index copies it into the variable's own slot, so what a store indexes by
           is one move removed from what built it. */
        if (ops[i].op == OP_RAW_MOVE_INT || ops[i].op == OP_RAW_MOVE_REAL || ops[i].op == OP_MOVE) {
            from_col[d] = from_col[x];
            group_of[d] = group_of[x];
            total_target[d] = total_target[x];
            total_gidx[d] = total_gidx[x];
            continue;
        }
        if (ops[i].op == OP_INDEX_SET_RAW_REAL) {
            /* INDEX_SET carries array, index, value in A, B, C -- unlike the arithmetic forms,
               whose A is a destination. */
            if (nstore >= VEC_MAX_STORES || group_of[x] < 0 || d == (uint8_t)array_reg)
                return false;
            /* Whatever is being stored has to be this group's own running total plus a per-row
               value, which the add below recorded when it saw the total. */
            if (closes_total[y] != (int)d || closes_gidx[y] != (int)x)
                return false;
            nstore++;
            continue;
        }
        /* An add that reads exactly one running total is the write-back half of a grouped store, and
           is the ONLY instruction allowed to read one. It cannot be judged here -- the store that
           follows says which array and which group it belonged to -- so it is recorded and checked
           there. */
        if (ops[i].op == OP_RAW_ADD_REAL && (total_target[x] >= 0) != (total_target[y] >= 0)) {
            uint8_t total = total_target[x] >= 0 ? x : y, val = total_target[x] >= 0 ? y : x;
            if (total_target[val] >= 0 || is_acc[val] || is_acc[d])
                return false;
            /* A constant addend (`counts[g] += 1`) carries no column to scatter, so the filter's own
               1/0 column stands in for it -- which means an unfiltered constant has nothing to
               stand in and is left as a loop. */
            if (from_col[val] < 0 && guards == 0)
                return false;
            closes_total[d] = total_target[total];
            closes_gidx[d] = total_gidx[total];
            closes_value[d] = val;
            continue;
        }
        /* `t = t + <per-row value>` where t is neither a column nor a running total closes a scalar
           accumulate -- COUNT(*) and SUM(x) over the whole result are both written that way. */
        if (ops[i].op == OP_RAW_ADD_REAL && d == x && from_col[x] < 0 && total_target[x] < 0 &&
            total_target[y] < 0 && !is_acc[y] && (from_col[y] >= 0 || guards > 0)) {
            if (naccum >= VEC_MAX_ACCS)
                return false;
            naccum++;
            is_acc[d] = true;
            continue;
        }
        /* Anything else has to be per-row arithmetic over columns, and must not read a running
           total or an accumulator -- one pass over the array cannot reproduce either. */
        if (total_target[x] >= 0 || total_target[y] >= 0 || is_acc[x] || is_acc[y])
            return false;
        if (from_col[x] < 0 && from_col[y] < 0)
            return false;
        from_col[d] = 1;
    }
    if (nstore == 0 && naccum == 0)
        return false;
    /* An accumulator may only ever be read as its own running total, and a stored group total only
       by the add that writes it back. */
    for (int i = 0; i < n; i++) {
        if (ops[i].guard || ops[i].op == OP_INDEX_SET_RAW_REAL)
            continue;
        bool is_the_acc = ops[i].op == OP_RAW_ADD_REAL && ops[i].d == ops[i].x && is_acc[ops[i].d];
        if (is_the_acc)
            continue;
        if (is_acc[ops[i].d])
            return false;
    }

    /* Emit before discarding: the calls need contiguous argument runs, and every one of them is
       secured first -- once the loop's own code is gone there is no ordinary path to fall back to. */
    int argc = guards > 0 ? 4 : 3;
    int need = nstore * argc + naccum * 2;
    int base = -1, taken = 0;
    if (need > 0) {
        base = reg_alloc();
        taken = base >= 0 ? 1 : 0;
        for (int i = 0; base >= 0 && i < need - 1; i++) {
            if (reg_alloc() != base + 1 + i) {
                base = -1;
                break;
            }
            taken++;
        }
    }
    if (base < 0) {
        if (taken)
            reg_free(taken);
        return false;
    }

    c->count = prep_at;
    unsigned int mod_idx = chunk_add_pool(c, aer_make_string_copy("collection", 10));
    unsigned int gs_idx = chunk_add_pool(c, aer_make_string_copy("group_sum", 9));
    unsigned int sum_idx = chunk_add_pool(c, aer_make_string_copy("sum", 3));
    int slot = base;
    /* value[r]: the whole-array register standing in for what r held per row.
       snap[r]: the values an aggregate will scatter, captured where they are still current. */
    int value[FRAME_REGISTERS], snap[FRAME_REGISTERS];
    for (int i = 0; i < FRAME_REGISTERS; i++) {
        value[i] = -1;
        snap[i] = -1;
    }
    int mask_reg = -1;
    for (int i = 0; i < n; i++) {
        if (ops[i].guard) {
            int gx = RK8_IS_CONST(ops[i].x) ? vec_guard_const(c, ops[i].x, ops[i].real_const)
                                            : (value[ops[i].x] >= 0 ? value[ops[i].x] : (int)ops[i].x);
            int gy = RK8_IS_CONST(ops[i].y) ? vec_guard_const(c, ops[i].y, ops[i].real_const)
                                            : (value[ops[i].y] >= 0 ? value[ops[i].y] : (int)ops[i].y);
            int one = reg_alloc();
            if (one < 0)
                return false;
            emit_binary(c, one, ops[i].op, gx, gy);
            P.reg_elem_kind[one] = RAWK_REAL;
            if (mask_reg < 0) {
                mask_reg = one;
            } else {
                /* `and` compiles to one branch per conjunct, all skipping to the same place, so
                   several guards multiply: 1 only where every one of them held. */
                int both = reg_alloc();
                if (both < 0)
                    return false;
                emit_binary(c, both, OP_MUL, mask_reg, one);
                P.reg_elem_kind[both] = RAWK_REAL;
                mask_reg = both;
            }
            continue;
        }
        if (ops[i].op == OP_TYPED_INDEX_GET_UNCHECKED || ops[i].op == OP_INDEX_GET_RAW_REAL) {
            if (ops[i].y == (uint8_t)item_reg)
                value[ops[i].d] = ops[i].x; /* the array itself replaces the element load */
            continue;
        }
        if (ops[i].op == OP_RAW_REAL_TO_INT)
            continue; /* group_sum takes the column itself, so the conversion is not needed */
        if (ops[i].op == OP_RAW_MOVE_INT || ops[i].op == OP_RAW_MOVE_REAL || ops[i].op == OP_MOVE) {
            value[ops[i].d] = value[ops[i].x];
            continue;
        }
        if (ops[i].op == OP_INDEX_SET_RAW_REAL) {
            emit_group_scatter(c, &slot, argc, ops[i].d, group_of[ops[i].x], snap[ops[i].y], mask_reg,
                               mod_idx, gs_idx);
            continue;
        }
        /* The adds that close a store or an accumulate contribute no array-level work of their own:
           the scatter and the reduction do that adding. The value each one carries is captured HERE
           rather than at the end, because the parser reuses a temp register freely -- reading it
           later gave a grouped total the NEXT statement's values, silently doubling it. */
        if (ops[i].op == OP_RAW_ADD_REAL && closes_total[ops[i].d] >= 0) {
            snap[ops[i].d] = value[closes_value[ops[i].d]];
            if (snap[ops[i].d] < 0) {
                /* A constant addend: every surviving row contributes the same, so the filter's own
                   1/0 column scaled by it is exactly the column to scatter. */
                int scaled = reg_alloc();
                if (scaled < 0)
                    return false;
                emit_binary(c, scaled, OP_MUL, mask_reg, closes_value[ops[i].d]);
                P.reg_elem_kind[scaled] = RAWK_REAL;
                snap[ops[i].d] = scaled;
            }
            continue;
        }
        if (ops[i].op == OP_RAW_ADD_REAL && is_acc[ops[i].d]) {
            int v = value[ops[i].y];
            if (v < 0) {
                v = reg_alloc();
                if (v < 0)
                    return false;
                emit_binary(c, v, OP_MUL, mask_reg, (int)ops[i].y);
                P.reg_elem_kind[v] = RAWK_REAL;
            } else if (mask_reg >= 0) {
                /* Rejected rows must contribute nothing, and for a reduction multiplying by the
                   1/0 filter says exactly that. */
                int masked = reg_alloc();
                if (masked < 0)
                    return false;
                emit_binary(c, masked, OP_MUL, v, mask_reg);
                P.reg_elem_kind[masked] = RAWK_REAL;
                v = masked;
            }
            if (!emit_scalar_reduction(c, &slot, ops[i].d, v, mod_idx, sum_idx))
                return false;
            continue;
        }
        int rx = value[ops[i].x] >= 0 ? value[ops[i].x] : (int)ops[i].x;
        int ry = value[ops[i].y] >= 0 ? value[ops[i].y] : (int)ops[i].y;
        Opcode boxed =
            ops[i].op == OP_RAW_ADD_REAL ? OP_ADD : (ops[i].op == OP_RAW_SUB_REAL ? OP_SUB : OP_MUL);
        int dest = reg_alloc();
        if (dest < 0)
            return false;
        emit_binary(c, dest, boxed, rx, ry);
        P.reg_elem_kind[dest] = RAWK_REAL;
        value[ops[i].d] = dest;
    }
    if ((guards > 0) != (mask_reg >= 0))
        return false;

    reg_free(need - 1);
    return true;
}

/* No exit-time cleanup needed -- col_reg/idx_reg are ordinary registers. Reserves the loop
   variable's register BEFORE compiling the collection expression, so later temps can never
   alias it. */
static void parse_for_in(Chunk* c, unsigned int loop_var_name) {
    /* A for-in loop variable always holds a plain item, never a value of known type -- an existing
       typed name of the same spelling must forget its type first. */
    ensure_boxed(loop_var_name);
    int item_reg = var_slot(c, loop_var_name);
    if (item_reg < 0)
        return;
    /* var_slot has no block scoping -- an already-declared name (a sibling or enclosing loop
       reusing the same spelling, e.g. `for i in 0..n: ... for i in 0..m: ...`) returns THAT
       register back, which may still carry a shape hint or a live safe_loop_item_regs entry from
       whatever last used it. This loop's own PREP/LOOP is about to start overwriting it every
       iteration regardless of what THIS loop turns out to prove, so any stale fact must be cleared
       before this loop's own body (or its own bound_safe/start_safe below) can be compiled. */
    P.reg_known_shape[item_reg] = NULL;
    P.reg_known_element_shape[item_reg] = NULL;
    P.reg_elem_kind[item_reg] = RAWK_NONE;
    P.alias_source_param[item_reg] = -1;
    invalidate_register(item_reg);
    /* invalidate_register cannot catch loop_var_name reusing the length_tracked_name NAME itself
       (`for n in 0..1000:` after `n = length(bodies)`) -- var_slot returns a bare register number,
       indistinguishable from a new one. This loop overwrites that register every iteration, so a
       later `for i in 0..n:` would otherwise trust the stale leftover. */
    if (P.length_tracked_valid && loop_var_name == P.length_tracked_name)
        P.length_tracked_valid = false;

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
        if (parse_had_error)
            return;

        /* Checked before arg_materialize, which may copy rk_start/rk_end into fresh registers that
           no longer identify their source. bound_safe: rk_end reads exactly the local proven ==
           length(P), and bound_array_reg records which P. start_safe: rk_start is literal 0, an
           enclosing safe loop's item register proven for that SAME array, or that plus a
           non-negative constant. Both fail closed -- an unrecognized shape just compiles as before. */
        bool bound_safe = false;
        int bound_array_reg = -1;
        if (P.length_tracked_valid && !(rk_end & RK_CONST_FLAG)) {
            for (int vi = 0; vi < P.var_count; vi++) {
                /* Any kind, not just VAR_BOXED: what matters is that this name still reads the
                   length, which knowing its type does not change. */
                if (P.var_names[vi] == P.length_tracked_name && P.var_regs[vi] == drop_raw_marks(rk_end)) {
                    bound_safe = true;
                    bound_array_reg = P.length_tracked_source_reg;
                    break;
                }
            }
        }
        /* bound_safe proves the upper bound (the end IS length(arr)); this proves the lower one. Any
           expression the compiler can show is >= 0 will do -- see Parser.reg_nonneg, which replaced
           three hand-decoded special cases (literal, bare safe register, and `safe_reg + const`
           matched by inspecting the emitted OP_ADD word) with one composable predicate. It now also
           covers `p*p`, `i*i+j`, `(p+1)*2` and anything else built from + * // %. */
        bool start_safe = bound_safe && rk_nonneg(c, rk_start);
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
        int saved_reserved_floor = P.slot_floor;
        P.slot_floor = P.slot_next;
        int raised_reserved_floor = P.slot_floor;

        bool hoisting = hoist_begin(c);
        unsigned int prep_at = c->count;
        /* Loop-rotated: PREP once before the loop, LOOP at the bottom of the body -- the one form
           whose continue must defer-patch instead of jumping to a known target. */
        unsigned int patch_empty =
            emit_iter_range_prep(c, cur_reg, end_reg, step_reg, item_reg, this_loop_safe);

        if (!loop_push_rotated()) {
            hoist_end(c, prep_at, hoisting);
            P.slot_floor = saved_reserved_floor;
            P.slot_next = saved_reserved_floor;
            return;
        }
        /* Pushed/popped exactly around this one loop's own body -- see safe_loop_item_regs's own
           comment (Parser struct) for why this is a small stack, not a whole-frame table. */
        if (this_loop_safe) {
            P.safe_loop_array_regs[P.safe_loop_depth] = bound_array_reg;
            P.safe_loop_item_regs[P.safe_loop_depth] = item_reg;
            P.safe_loop_depth++;
            /* Accepting the loop required a non-negative start, and the step is positive, so
               every value this register takes is >= 0. */
            if (item_reg >= 0 && item_reg < FRAME_REGISTERS)
                P.reg_nonneg[item_reg] = true;
        }
        /* Same push/pop discipline as safe_loop_* above; invalidate_register does the recording. */
        bool range_tracked = P.range_loop_depth < LOOP_MAX;
        if (range_tracked) {
            P.range_item_regs[P.range_loop_depth] = item_reg;
            P.range_item_written[P.range_loop_depth] = false;
            P.range_loop_depth++;
        }
        /* PREP rejects a non-integer bound or step outright and publishes a tagged integer, and LOOP
           only ever advances it as one -- so this variable is an integer on every path that reaches
           the body, and saying so lets the body index and compute unchecked. Without it the
           idiomatic `for i in 0..n` compiled to the generic opcodes while the hand-written
           `i = 0; for i < n` did not, which measured 15.9% more instructions for the same work. */
        for (int v = P.var_count - 1; v >= 0; v--)
            if (P.var_names[v] == loop_var_name && P.var_regs[v] == item_reg) {
                P.var_kind[v] = VAR_RAW_INT;
                break;
            }
        unsigned int body_start = c->count;
        parse_block(c);
        if (parse_had_error) {
            hoist_end(c, prep_at, hoisting);
            if (range_tracked)
                P.range_loop_depth--;
            if (this_loop_safe)
                P.safe_loop_depth--;
            P.loop_depth--;
            P.slot_floor = saved_reserved_floor;
            P.slot_next = saved_reserved_floor;
            return;
        }
        if (this_loop_safe)
            P.safe_loop_depth--;
        bool body_wrote_item = !range_tracked || P.range_item_written[P.range_loop_depth - 1];
        if (range_tracked)
            P.range_loop_depth--;

        /* Attempted before LOOP is emitted, so a success rewinds over PREP and the body together.
           Requires the loop's own bound proof: the whole-array reduction reads length(array)
           elements, which is only the same work when the loop ran exactly that many. */
        if (this_loop_safe && !body_wrote_item && P.loop_stack[P.loop_depth - 1].patch_count == 0 &&
            P.loop_stack[P.loop_depth - 1].continue_patch_count == 0 &&
            (try_vectorize_reduction(c, prep_at, body_start, c->count, item_reg, bound_array_reg, rk_start,
                                     rk_step) ||
             try_vectorize_group_accumulate(c, prep_at, body_start, c->count, item_reg, bound_array_reg,
                                            rk_start, rk_step))) {
            P.loop_depth--;
            hoist_end(c, prep_at, hoisting);
            if (P.slot_floor == raised_reserved_floor) {
                P.slot_floor = saved_reserved_floor;
                P.slot_next = saved_reserved_floor;
            }
            return;
        }

        unsigned int loop_bottom = c->count;
        /* PREP already left the start value in item_reg, so when the body never writes that register
           the loop's counter can live there and LOOP maintains one register instead of two. PREP's
           own cur operand stays cur_reg either way -- it only ever reads it. */
        emit_iter_range_loop(c, body_wrote_item ? cur_reg : item_reg, end_reg, step_reg, item_reg,
                             body_start);

        unsigned int exit_pos = c->count;
        patch_jump(c, patch_empty, exit_pos);
        loop_pop_and_patch_rotated(c, exit_pos, loop_bottom);
        hoist_end(c, prep_at, hoisting);

        /* Conditional for the same reason the while-form's restore is: a name first assigned in the
           body outlives the loop, so lowering past its register would hand a live variable to the
           next statement as a temp. */
        if (P.slot_floor == raised_reserved_floor) {
            P.slot_floor = saved_reserved_floor;
            P.slot_next = saved_reserved_floor;
        }
        assert_variables_below_floor("range-for body");
        return;
    }

    require(TOKEN_COLON, "expected ':' after for-in clause");
    if (parse_had_error)
        return;

    int col_reg = materialize(c, rk_start);

    int idx_reg = reg_alloc();
    unsigned int pool_zero = chunk_add_pool(c, aer_int(0));
    chunk_emit(c, PACK_OP_A_W16(OP_LOADK, idx_reg, pool_zero));

    /* idx_reg/col_reg must stay valid across the whole body, so they're protected before the body
       compiles, same as the range branch. */
    int saved_reserved_floor = P.slot_floor;
    P.slot_floor = P.slot_next;
    int raised_reserved_floor = P.slot_floor;

    bool hoisting = hoist_begin(c);
    unsigned int loop_top = c->count; /* the iterate opcode is its own back-edge target */
    unsigned int patch_exit = emit_iter_next_array(c, col_reg, idx_reg, item_reg);

    parse_loop_body(c, loop_top, patch_exit);
    hoist_end(c, loop_top, hoisting);

    if (P.slot_floor == raised_reserved_floor) {
        P.slot_floor = saved_reserved_floor;
        P.slot_next = saved_reserved_floor;
    }
    assert_variables_below_floor("for-in body");
}

/* Dict-only at runtime -- reserves both loop variables' registers before compiling the
   collection expression. */
static void parse_for_in_pair(Chunk* c, unsigned int key_name, unsigned int val_name) {
    ensure_boxed(key_name); /* same reasoning as parse_for_in's own ensure_boxed call */
    ensure_boxed(val_name);
    int key_reg = var_slot(c, key_name);
    if (key_reg < 0)
        return;
    int val_reg = var_slot(c, val_name);
    if (val_reg < 0)
        return;
    /* Same name-shadowing reasoning as parse_for_in's own identical block just above. */
    P.reg_known_shape[key_reg] = NULL;
    P.reg_known_element_shape[key_reg] = NULL;
    P.reg_elem_kind[key_reg] = RAWK_NONE;
    P.alias_source_param[key_reg] = -1;
    invalidate_register(key_reg);
    P.reg_known_shape[val_reg] = NULL;
    P.reg_known_element_shape[val_reg] = NULL;
    P.reg_elem_kind[val_reg] = RAWK_NONE;
    P.alias_source_param[val_reg] = -1;
    invalidate_register(val_reg);
    /* Same length_tracked_name-by-NAME reasoning as parse_for_in's own identical check. */
    if (P.length_tracked_valid && (key_name == P.length_tracked_name || val_name == P.length_tracked_name)) {
        P.length_tracked_valid = false;
    }

    int rk_col = parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after for-in clause");
    if (parse_had_error)
        return;

    int col_reg = materialize(c, rk_col);

    int idx_reg = reg_alloc();
    unsigned int pool_zero = chunk_add_pool(c, aer_int(0));
    chunk_emit(c, PACK_OP_A_W16(OP_LOADK, idx_reg, pool_zero));

    int saved_reserved_floor = P.slot_floor;
    P.slot_floor = P.slot_next;

    bool hoisting = hoist_begin(c);
    unsigned int loop_top = c->count;
    unsigned int patch_exit = emit_iter_next_pair(c, col_reg, idx_reg, key_reg, val_reg);

    parse_loop_body(c, loop_top, patch_exit);
    hoist_end(c, loop_top, hoisting);

    P.slot_floor = saved_reserved_floor;
    P.slot_next = saved_reserved_floor;
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
                return error_at("Expected identifier after ','");
            }
            unsigned int name2_idx = chunk_add_pool(c, token.value);
            lex();
            require(TOKEN_IN, "expected 'in' after 'for k, v'");
            if (parse_had_error)
                return;
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
            if (reg < 0)
                return;
        }
        bool hoisting = hoist_begin(c);
        unsigned int loop_top = c->count;
        P.loop_cond_peak = P.slot_next;
        /* Resolves the postfix chain first, so `for cur.next:` works -- a bare-variable condition is
           unaffected, since the chain loop immediately returns unchanged. */
        int rk_chain = parse_postfix_chain(c, reg);
        int rk_cond = parse_binary_ops(c, 0, rk_chain, c->count);
        parse_for_body(c, loop_top, rk_cond);
        hoist_end(c, loop_top, hoisting);
        return;
    }
    bool hoisting = hoist_begin(c);
    unsigned int loop_top = c->count;
    P.loop_cond_peak = P.slot_next;
    int rk_cond = parse_binary(c, 0);
    parse_for_body(c, loop_top, rk_cond);
    hoist_end(c, loop_top, hoisting);
}

/* Checked before the normal identifier/call/assignment path -- a module name isn't a
   variable, so var_slot/func_lookup must never see it.

   The following '.' is part of the test, not just an expectation: `string` and `time` are module
   names AND builtin casts/functions, so `import string` used to make `string(42)` unreachable. A
   module use is always `name.function(...)`, so the dot is what tells the two apart. */
static bool at_module_name(Chunk* c) {
    if (token.type != TOKEN_IDENTIFIER ||
        !chunk_is_imported(c, aer_as_string(token.value)->data, aer_as_string(token.value)->length))
        return false;
    const char* p = current_source_cursor();
    while (*p == ' ' || *p == '\t')
        p++;
    return *p == '.';
}

/* Arguments reach the call via contiguous registers, same as every other call site;
   OP_CALL_MODULE bridges to the shared stdlib dispatch rather than reimplementing it. */
/* A module name is always a literal identifier, never a runtime value, so it's fully knowable
   here. Returns CALL_MODULE_DYNAMIC for anything not a fixed core built-in. */
static int module_call_id(AerString* name) {
#define AER_MODULE_LOOKUP(id, str, call)                                                                     \
    if (name->length == sizeof(str) - 1 && strncmp(name->data, str, sizeof(str) - 1) == 0)                   \
        return CALL_MODULE_##id;
    AER_NATIVE_MODULES(AER_MODULE_LOOKUP)
#undef AER_MODULE_LOOKUP
    return CALL_MODULE_DYNAMIC;
}

#define NAME_IS(lit) (name->length == sizeof(lit) - 1 && strncmp(name->data, lit, sizeof(lit) - 1) == 0)

/* A literal identifier, never ambiguous, so resolvable once here. FN_ID_UNKNOWN for anything
   not a known function of that module. */
static int module_fn_id(int module_id, AerString* name) {
    switch (module_id) {
        case CALL_MODULE_MATH:
            if (NAME_IS("sqrt"))
                return FN_MATH_SQRT;
            if (NAME_IS("pow"))
                return FN_MATH_POW;
            if (NAME_IS("floor"))
                return FN_MATH_FLOOR;
            if (NAME_IS("ceil"))
                return FN_MATH_CEIL;
            if (NAME_IS("abs"))
                return FN_MATH_ABS;
            if (NAME_IS("min"))
                return FN_MATH_MIN;
            if (NAME_IS("max"))
                return FN_MATH_MAX;
            if (NAME_IS("sin"))
                return FN_MATH_SIN;
            if (NAME_IS("cos"))
                return FN_MATH_COS;
            if (NAME_IS("log"))
                return FN_MATH_LOG;
            if (NAME_IS("log2"))
                return FN_MATH_LOG2;
            if (NAME_IS("log10"))
                return FN_MATH_LOG10;
            if (NAME_IS("pi"))
                return FN_MATH_PI;
            if (NAME_IS("round"))
                return FN_MATH_ROUND;
            if (NAME_IS("tan"))
                return FN_MATH_TAN;
            if (NAME_IS("exp"))
                return FN_MATH_EXP;
            if (NAME_IS("asin"))
                return FN_MATH_ASIN;
            if (NAME_IS("acos"))
                return FN_MATH_ACOS;
            if (NAME_IS("atan"))
                return FN_MATH_ATAN;
            if (NAME_IS("atan2"))
                return FN_MATH_ATAN2;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_RANDOM:
            if (NAME_IS("random"))
                return FN_RANDOM_RANDOM;
            if (NAME_IS("randint"))
                return FN_RANDOM_RANDINT;
            if (NAME_IS("seed"))
                return FN_RANDOM_SEED;
            if (NAME_IS("choice"))
                return FN_RANDOM_CHOICE;
            if (NAME_IS("shuffle"))
                return FN_RANDOM_SHUFFLE;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_STRING:
            if (NAME_IS("upper"))
                return FN_STRING_UPPER;
            if (NAME_IS("lower"))
                return FN_STRING_LOWER;
            if (NAME_IS("trim"))
                return FN_STRING_TRIM;
            if (NAME_IS("contains"))
                return FN_STRING_CONTAINS;
            if (NAME_IS("split"))
                return FN_STRING_SPLIT;
            if (NAME_IS("starts_with"))
                return FN_STRING_STARTS_WITH;
            if (NAME_IS("ends_with"))
                return FN_STRING_ENDS_WITH;
            if (NAME_IS("repeat"))
                return FN_STRING_REPEAT;
            if (NAME_IS("replace"))
                return FN_STRING_REPLACE;
            if (NAME_IS("join"))
                return FN_STRING_JOIN;
            if (NAME_IS("to_integer"))
                return FN_STRING_TO_INTEGER;
            if (NAME_IS("to_float"))
                return FN_STRING_TO_FLOAT;
            if (NAME_IS("index_of"))
                return FN_STRING_INDEX_OF;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_TIME:
            if (NAME_IS("now"))
                return FN_TIME_NOW;
            if (NAME_IS("strftime"))
                return FN_TIME_STRFTIME;
            if (NAME_IS("sleep"))
                return FN_TIME_SLEEP;
            if (NAME_IS("parse"))
                return FN_TIME_PARSE;
            if (NAME_IS("to_parts"))
                return FN_TIME_TO_PARTS;
            if (NAME_IS("from_parts"))
                return FN_TIME_FROM_PARTS;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_JSON:
            if (NAME_IS("encode"))
                return FN_JSON_ENCODE;
            if (NAME_IS("decode"))
                return FN_JSON_DECODE;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_COLLECTION:
            if (NAME_IS("append"))
                return FN_COLLECTION_APPEND;
            if (NAME_IS("delete"))
                return FN_COLLECTION_DELETE;
            if (NAME_IS("copy"))
                return FN_COLLECTION_COPY;
            if (NAME_IS("insert"))
                return FN_COLLECTION_INSERT;
            if (NAME_IS("index_of"))
                return FN_COLLECTION_INDEX_OF;
            if (NAME_IS("keys"))
                return FN_COLLECTION_KEYS;
            if (NAME_IS("sort"))
                return FN_COLLECTION_SORT;
            if (NAME_IS("reserve"))
                return FN_COLLECTION_RESERVE;
            if (NAME_IS("sum"))
                return FN_COLLECTION_SUM;
            if (NAME_IS("min"))
                return FN_COLLECTION_MIN;
            if (NAME_IS("max"))
                return FN_COLLECTION_MAX;
            if (NAME_IS("group_sum"))
                return FN_COLLECTION_GROUP_SUM;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_NET:
            if (NAME_IS("connect"))
                return FN_NET_CONNECT;
            if (NAME_IS("send"))
                return FN_NET_SEND;
            if (NAME_IS("recv"))
                return FN_NET_RECV;
            if (NAME_IS("close"))
                return FN_NET_CLOSE;
            if (NAME_IS("listen"))
                return FN_NET_LISTEN;
            if (NAME_IS("accept"))
                return FN_NET_ACCEPT;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_REGEX:
            if (NAME_IS("match"))
                return FN_REGEX_MATCH;
            if (NAME_IS("find"))
                return FN_REGEX_FIND;
            if (NAME_IS("replace"))
                return FN_REGEX_REPLACE;
            if (NAME_IS("find_all"))
                return FN_REGEX_FIND_ALL;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_ACTOR:
            if (NAME_IS("spawn"))
                return FN_ACTOR_SPAWN;
            if (NAME_IS("send"))
                return FN_ACTOR_SEND;
            if (NAME_IS("receive"))
                return FN_ACTOR_RECEIVE;
            if (NAME_IS("call"))
                return FN_ACTOR_CALL;
            if (NAME_IS("keep"))
                return FN_ACTOR_KEEP;
            if (NAME_IS("kept"))
                return FN_ACTOR_KEPT;
            if (NAME_IS("give"))
                return FN_ACTOR_GIVE;
            return FN_ID_UNKNOWN;
        case CALL_MODULE_SCHEDULER:
            if (NAME_IS("add"))
                return FN_SCHEDULER_ADD;
            if (NAME_IS("run"))
                return FN_SCHEDULER_RUN;
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
    if (parse_had_error)
        return 0;

    /* `math.sqrt(<real>)` on a slot whose type is already known needs neither the module calling
       convention's tag check nor one on the result. Every function aer_math_fn_is_raw_real names is
       unary, so the argument is parsed here rather than through the general contiguous-argument path
       -- that path returns registers, having already discarded the kind this needs. */
    if (module_id == CALL_MODULE_MATH && aer_math_fn_is_raw_real(fn_id)) {
        int rk_arg = parse_binary(c, 0);
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after module call arguments");
        if (parse_had_error)
            return 0;
        if (rk_raw_kind(c, rk_arg) == RAWK_REAL) {
            int src_slot = raw_materialize(c, rk_arg, RAWK_REAL);
            if (src_slot >= 0) {
                raw_release_if_top(src_slot);
                int dest_slot = slot_alloc(RAWK_REAL);
                if (dest_slot >= 0) {
                    chunk_emit(c, PACK3(OP_RAW_MATH_REAL, dest_slot, src_slot, (unsigned int)fn_id));
                    return RK_RAW_REAL_FLAG | dest_slot;
                }
            }
            rk_arg = src_slot >= 0 ? (RK_RAW_REAL_FLAG | src_slot) : rk_arg;
        }
        int dest = arg_materialize(c, rk_arg);
        chunk_emit(c, PACK3(OP_CALL_MODULE, dest, dest, 1));
        chunk_emit(c, (uint32_t)module_idx);
        chunk_emit(c, (uint32_t)fn_idx);
        chunk_emit(c, PACK_2X16((uint16_t)module_id, (uint16_t)fn_id));
        return dest;
    }

    /* `collection.group_sum(price * quantity * (1 - discount) * mask, region, G)` -- the values are
       the longest expression in a query and the scatter that consumes them is nearly free beside it,
       so the values fold into the scatter and are never written out. Its arguments are parsed one at
       a time rather than as one contiguous run: the values' own code has to be recognised and
       discarded before the group column is ever compiled, which the shared path gives no point to
       do. Only the first argument folds; the other two are read as they are. */
    if (module_id == CALL_MODULE_COLLECTION && fn_id == FN_COLLECTION_GROUP_SUM &&
        !equal(TOKEN_CLOSE_PARENTHESE)) {
        unsigned int values_start = c->count;
        int values_rk = parse_binary(c, 0);
        int leaf[CHAIN_MAX_LEAVES];
        uint64_t prog = 0;
        int mask_leaves = 0;
        int nleaf = (parse_had_error || (values_rk & RK_CONST_FLAG))
                        ? 0
                        : chain_of_array_ops(c, values_start, c->count, values_rk, leaf, &prog, &mask_leaves);
        /* The whole run is secured BEFORE anything is discarded -- once the values' code is gone
           there is no ordinary path left to fall back to. */
        int base = -1, taken = 0;
        if (nleaf > 0) {
            base = reg_alloc();
            taken = base >= 0 ? 1 : 0;
            for (int i = 0; base >= 0 && i < nleaf + 2; i++) {
                if (reg_alloc() != base + 1 + i) {
                    base = -1;
                    break;
                }
                taken++;
            }
        }
        if (base >= 0) {
            c->count = values_start; /* the per-operator passes go; every leaf still holds its own */
            int groups = -1, ngroups = -1;
            if (consume(TOKEN_COMMA))
                groups = materialize(c, parse_binary(c, 0));
            if (consume(TOKEN_COMMA))
                ngroups = materialize(c, parse_binary(c, 0));
            require(TOKEN_CLOSE_PARENTHESE, "expected ')' after module call arguments");
            if (parse_had_error || groups < 0 || ngroups < 0)
                return 0;
            uint64_t word = prog | ((uint64_t)mask_leaves << CHAIN_SPLIT_SHIFT);
            chunk_emit(c, PACK_OP_A_W16(OP_LOADK, base, chunk_add_pool(c, aer_int((int64_t)word))));
            chunk_emit(c, PACK2(OP_MOVE, base + 1, groups));
            chunk_emit(c, PACK2(OP_MOVE, base + 2, ngroups));
            for (int i = 0; i < nleaf; i++)
                chain_emit_leaf(c, base + 3 + i, leaf[i]);
            chunk_emit(c, PACK3(OP_CALL_MODULE, base, base, nleaf + 3));
            chunk_emit(c, (uint32_t)module_idx);
            chunk_emit(c, (uint32_t)fn_idx);
            chunk_emit(c,
                       PACK_2X16((uint16_t)CALL_MODULE_COLLECTION, (uint16_t)FN_COLLECTION_GROUP_SUM_CHAIN));
            reg_free(nleaf + 2);
            P.reg_elem_kind[base] = RAWK_REAL;
            return base;
        }
        if (taken)
            reg_free(taken);
        /* Unfused: place the values first in a contiguous run, exactly as the shared path would. */
        int argbase = arg_materialize(c, values_rk);
        int argc = 1;
        while (consume(TOKEN_COMMA)) {
            arg_materialize(c, parse_binary(c, 0));
            argc++;
        }
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after module call arguments");
        if (parse_had_error)
            return 0;
        if (argc > 1)
            reg_free(argc - 1);
        chunk_emit(c, PACK3(OP_CALL_MODULE, argbase, argbase, argc));
        chunk_emit(c, (uint32_t)module_idx);
        chunk_emit(c, (uint32_t)fn_idx);
        chunk_emit(c, PACK_2X16((uint16_t)module_id, (uint16_t)fn_id));
        return argbase;
    }

    unsigned int arg_code_start = c->count;
    int arg_reg_base;
    bool arg_base_is_temp;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base, &arg_base_is_temp);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after module call arguments");
    if (parse_had_error)
        return 0;

    /* `collection.sum(a * b * c)` costs one whole-array pass per operator, each writing a full-size
       intermediate the next reads straight back. Recognised here and folded into one tiled pass,
       which never materialises any of them -- 0.0429s to 0.0098s over 8M elements. */
    if (module_id == CALL_MODULE_COLLECTION && fn_id == FN_COLLECTION_SUM && arg_count == 1) {
        int leaf[CHAIN_MAX_LEAVES];
        uint64_t packed = 0;
        int mask_leaves = 0;
        int nleaf =
            chain_of_array_ops(c, arg_code_start, c->count, arg_reg_base, leaf, &packed, &mask_leaves);
        /* The run must be contiguous, and that is settled BEFORE anything is discarded -- once the
           argument's own code is gone there is no ordinary path left to fall back to. */
        int base = -1, taken = 0;
        if (nleaf > 0) {
            base = reg_alloc();
            taken = base >= 0 ? 1 : 0;
            for (int i = 0; base >= 0 && i < nleaf; i++) {
                if (reg_alloc() != base + 1 + i) {
                    base = -1;
                    break;
                }
                taken++;
            }
        }
        if (base >= 0) {
            c->count = arg_code_start; /* the per-operator passes go; the fused call replaces them */
            uint64_t word = packed | ((uint64_t)mask_leaves << CHAIN_SPLIT_SHIFT);
            chunk_emit(c, PACK_OP_A_W16(OP_LOADK, base, chunk_add_pool(c, aer_int((int64_t)word))));
            for (int i = 0; i < nleaf; i++)
                chain_emit_leaf(c, base + 1 + i, leaf[i]);
            chunk_emit(c, PACK3(OP_CALL_MODULE, base, base, nleaf + 1));
            chunk_emit(c, (uint32_t)module_idx);
            chunk_emit(c, (uint32_t)fn_idx);
            chunk_emit(c, PACK_2X16((uint16_t)CALL_MODULE_COLLECTION, (uint16_t)FN_COLLECTION_SUM_CHAIN));
            reg_free(nleaf);
            return base;
        }
        if (taken)
            reg_free(taken);
    }

    /* Reusing the argument base as the destination is only safe when it is a temp -- a lone
       argument now stays in its own register, which may be a variable's. */
    int dest = (arg_count > 0 && arg_base_is_temp) ? arg_reg_base : reg_alloc();
    if (arg_count > 1)
        reg_free(arg_count - 1);
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
        return error_at("Import path is empty or too long");
    }
    memcpy(path_buf, path_str->data, path_len);
    lex();

    char derived_buf[64];
    if (alias_len == 0) {
        /* Derive the bound name from the last path segment, stripping a trailing ".aer". */
        unsigned int end = path_len;
        if (end > 4 && strncmp(path_buf + end - 4, ".aer", 4) == 0)
            end -= 4;
        unsigned int start = 0;
        for (unsigned int i = 0; i < end; i++)
            if (path_buf[i] == '/' || path_buf[i] == '\\')
                start = i + 1;
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
        return error_at("'import' is only allowed at the top level of a file, not inside a function");
    }
    if (token.type == TOKEN_STRING) {
        parse_import_path(c, NULL, 0);
        return;
    }
    if (!equal(TOKEN_IDENTIFIER)) {
        return error_at("Expected a module name or a quoted path after 'import'");
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
            return error_at("Import name too long");
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
            return error_at("Expected a module name segment after '.'");
        }
        const char* seg = aer_as_string(token.value)->data;
        unsigned int seg_len = aer_as_string(token.value)->length;
        path_buf[path_len++] = '/';
        if (path_len + seg_len >= sizeof(path_buf)) {
            return error_at("Import path too long");
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
/* Reserved global function names: they always resolve to the builtin, so they cannot be variables
   either (var_slot rejects them). integer/float/boolean/string are here rather than being reserved
   lexer tokens -- one mechanism, and three fewer keywords in the language. */
static bool is_builtin_name(Chunk* c, unsigned int name_idx) {
    AerString* s = aer_as_string(c->pool[name_idx]);
    static const char* const names[] = {"length", "print",  "type",    "assert", "panic",
                                        "Result", "string", "integer", "float",  "boolean"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t len = strlen(names[i]);
        if (s->length == len && strncmp(s->data, names[i], len) == 0)
            return true;
    }
    return false;
}

/* Mirrors module_call_id below -- only called after is_builtin_name confirms a match. */
static int builtin_call_id(AerString* name) {
    if (name->length == 6 && strncmp(name->data, "length", 6) == 0)
        return CALL_BUILTIN_LENGTH;
    if (name->length == 5 && strncmp(name->data, "print", 5) == 0)
        return CALL_BUILTIN_PRINT;
    if (name->length == 4 && strncmp(name->data, "type", 4) == 0)
        return CALL_BUILTIN_TYPE;
    if (name->length == 6 && strncmp(name->data, "assert", 6) == 0)
        return CALL_BUILTIN_ASSERT;
    if (name->length == 6 && strncmp(name->data, "Result", 6) == 0)
        return CALL_BUILTIN_RESULT;
    return CALL_BUILTIN_PANIC;
}

/* Same contiguous-register materialization and result-register reuse as parse_call, emitting
   OP_CALL_BUILTIN instead. */
static int parse_builtin_call(Chunk* c, unsigned int name_idx) {
    unsigned int arg_code_begin = c->count;
    int arg_reg_base;
    bool arg_base_is_temp;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base, &arg_base_is_temp);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error)
        return 0;
    unsigned int arg_code_end = c->count;

    /* Reusing the argument base as the destination is only safe when it is a temp -- a lone
       argument now stays in its own register, which may be a variable's. */
    int dest = (arg_count > 0 && arg_base_is_temp) ? arg_reg_base : reg_alloc();
    if (arg_count > 1)
        reg_free(arg_count - 1);
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
        if (dest >= 0 && dest < FRAME_REGISTERS)
            P.reg_nonneg[dest] = true; /* a count */
        int arg_orig_reg = base;
        if (arg_code_end - arg_code_begin == 1) {
            uint32_t w = c->code[arg_code_begin];
            if ((w & 0xFF) == OP_MOVE && (int)UNPACK_A(w) == base)
                arg_orig_reg = (int)UNPACK_B(w);
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
    /* The four casts, checked before anything else. Exactly one argument, enforced by the grammar
       itself: parse_binary parses one expression, so a second argument or none falls through to a
       natural "expected ')'" rather than needing an arity check here. */
    int cast_type = cast_type_for_name(aer_as_string(c->pool[name_idx]));
    if (cast_type != CAST_NONE) {
        int lhs = parse_binary(c, 0);
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after cast argument");
        if (parse_had_error)
            return 0;
        return emit_primitive_cast(c, cast_type, lhs);
    }

    /* Builtins win unconditionally -- length/print/type/assert/panic/Result can never be shadowed. */
    if (is_builtin_name(c, name_idx))
        return parse_builtin_call(c, name_idx);

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
    unsigned short func_frame_bounds = 0;
    unsigned int func_index = 0;
    AerVal* func_defaults = NULL;
    bool is_func = !is_var && !is_struct &&
                   func_full_lookup(c, name_idx, &func_offset, &func_arity, &func_min_arity, &func_defaults,
                                    &func_max_registers, &func_frame_bounds, &func_index);
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
    bool arg_base_is_temp;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base, &arg_base_is_temp);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error)
        return 0;

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
    /* Reusing the argument base as the destination is only safe when it is a temp -- a lone argument
       now stays in its own register, which may be a variable's. */
    int dest = (arg_count > 0 && arg_base_is_temp) ? arg_reg_base : reg_alloc();
    int base = arg_reg_base < 0 ? dest : arg_reg_base;

    bool needs_callee_reg = needs_call_value;
    int callee_reg = -1;
    if (needs_call_value) {
        AerVal fv = build_function_value(func_offset, func_arity, func_min_arity, func_defaults,
                                         func_max_registers, func_frame_bounds);
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
        else if (P.in_variant && (int)func_index == P.current_func_idx && arg_count == (int)func_arity &&
                 func_arity == func_min_arity) {
            P.self_call_seen = true;
            chunk_emit(c, PACK3(OP_CALL_SELF, dest, base, arg_count));
        } else
            emit_call(
                c, dest, func_offset, base, arg_count,
                func_index); /* exact arity -- no forward-ref patching needed, is_func means already resolved */
        last_bare_call_end = c->count;
    } else {
        last_bare_call_start = c->count;
        unsigned int patch_offset = emit_call(c, dest, func_offset, base, arg_count, func_index);
        if (is_forward_ref)
            pending_call_add(name_idx, patch_offset, call_site_cursor);
        last_bare_call_end = c->count;
    }

    /* One combined compaction for both the extra argument registers and callee_reg, not two
       separately-timed frees -- nothing allocates between them. */
    int extra = (arg_count > 1 ? arg_count - 1 : 0) + (needs_callee_reg ? 1 : 0);
    if (extra > 0)
        reg_free(extra);

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
        return error_at("'return' outside function");
    }

    if (!equal(TOKEN_NEW_LINE) && !equal(TOKEN_END_OF_FILE) && !equal(TOKEN_DEDENT)) {
        int rk_first = parse_binary(c, 0);
        if (parse_had_error)
            return;

        if (equal(TOKEN_COMMA)) {
            int reg_base = arg_materialize(c, rk_first);
            unsigned int count = 1;
            while (consume(TOKEN_COMMA)) {
                int rk_next = parse_binary(c, 0);
                if (parse_had_error)
                    return;
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
        return error_at("'raise' outside function");
    }
    unsigned int null_idx = chunk_add_pool(c, aer_null());
    int reg_base = arg_materialize(c, (int)null_idx | RK_CONST_FLAG);
    int rk_err = parse_binary(c, 0);
    if (parse_had_error)
        return;
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
    if (parse_had_error)
        return 0;

    chunk_emit(c, OP_JUMP);
    unsigned int patch = c->count;
    chunk_emit(c, 0);
    unsigned int func_start = c->count;

    unsigned int captured_max_registers;
    unsigned short captured_frame_bounds;
    parse_function_body(c, param_names, param_count, -1, NULL, false, NULL, NULL, 0, &captured_max_registers,
                        &captured_frame_bounds);

    patch_jump(c, patch, c->count);

    unsigned int default_count = (unsigned int)param_count - (unsigned int)min_param_count;
    AerVal* defaults = NULL;
    if (default_count > 0) {
        defaults = xmalloc(sizeof(AerVal) * default_count);
        for (unsigned int i = 0; i < default_count; i++)
            defaults[i] = param_defaults[(unsigned int)min_param_count + i];
    }
    AerVal fv = build_function_value(func_start, (unsigned int)param_count, (unsigned int)min_param_count,
                                     defaults, captured_max_registers, captured_frame_bounds);
    return (int)chunk_add_pool(c, fv) | RK_CONST_FLAG;
}

/* Parses `(params...):' -- shared by the original top-level function compile and a later
   specialization recompile (vm.c's vm_call_resolve_specialization, re-lexing a retained source span, see
   ChunkFunction.source_span). Caller must already be positioned right at '('. Once one parameter
   has a default, every parameter after it must too. */
static void parse_function_signature(Chunk* c, unsigned int* param_names, AerVal* param_defaults,
                                     int* out_param_count, int* out_min_param_count) {
    require(TOKEN_OPEN_PARENTHESE, "expected '(' after function name");
    if (parse_had_error)
        return;

    int param_count = 0;
    int min_param_count = 0;
    bool seen_default = false;
    if (!equal(TOKEN_CLOSE_PARENTHESE)) {
        do {
            if (!equal(TOKEN_IDENTIFIER)) {
                return error_at("Expected parameter name");
            }
            if (param_count >= FRAME_REGISTERS) {
                return error_at("Too many parameters (max %d)", FRAME_REGISTERS);
            }
            param_names[param_count] = chunk_add_pool(c, token.value);
            lex();
            if (consume(TOKEN_ASSIGN)) {
                bool unused_narrow;
                if (!parse_literal_default(c, &param_defaults[param_count], &unused_narrow)) {
                    return error_at("Parameter defaults must be a literal value");
                }
                seen_default = true;
            } else if (seen_default) {
                return error_at("A parameter without a default cannot follow one that has a default");
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
   types/count bind those parameters as statically typed locals, in their own argument registers. */
static void parse_function_body(Chunk* c, unsigned int* param_names, int param_count, int hint_param_reg,
                                Shape* hint_shape, bool hint_is_element_shape, const int* raw_param_regs,
                                const ValueType* raw_param_types, int raw_param_count,
                                unsigned int* out_max_registers, unsigned short* out_frame_bounds) {
    unsigned int saved_var_names[FRAME_REGISTERS];
    int saved_var_regs[FRAME_REGISTERS];
    VarKind saved_var_kind[FRAME_REGISTERS];
    int saved_var_count = P.var_count;
    int saved_next_temp = P.slot_next;
    int saved_reserved_floor = P.slot_floor;
    int saved_max_slot_used = P.slot_max;
    int saved_raw_real_next = P.raw_real_next, saved_raw_real_floor = P.raw_real_floor;
    int saved_raw_real_low = P.raw_real_low;
    /* Per-register facts about the ENCLOSING scope's values, which the body is about to overwrite
       for its own registers. Losing reg_elem_kind used only to cost an optimization; since a
       register known to hold a typed array is also what stops `1.0 - discount` unboxing it, losing
       it now emits an unbox that fails at runtime -- a function defined between an array's
       construction and its use was enough. */
    RawKind saved_elem_kind[FRAME_REGISTERS];
    int saved_len_class[FRAME_REGISTERS];
    memcpy(saved_elem_kind, P.reg_elem_kind, sizeof(saved_elem_kind));
    memcpy(saved_len_class, P.reg_len_class, sizeof(saved_len_class));
    memcpy(saved_var_names, P.var_names, sizeof(unsigned int) * (size_t)P.var_count);
    memcpy(saved_var_regs, P.var_regs, sizeof(int) * (size_t)P.var_count);
    memcpy(saved_var_kind, P.var_kind, sizeof(VarKind) * (size_t)P.var_count);
    P.var_count = 0;
    P.slot_next = P.slot_floor = P.slot_max = 0;

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
    P.next_value_class = 0;

    /* Before the parameters, not after: a specialization recompile enters through
       parser_save_state, which zeroes the whole parser, and var_slot reads the region's low end as
       its own ceiling. Parameters sit at the bottom of the frame either way. */
    raw_region_open();

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
            if (raw_param_regs[k] != i)
                continue;
            /* The argument already sits in register i, correctly tagged, and the resolver verified
               its type before choosing this variant -- so recording the type is the whole of the
               binding. No second slot, and no prologue opcode to fill one: the parameter simply
               stops needing its tag checked. */
            P.var_kind[P.var_count - 1] = (raw_param_types[k] == TYPE_INTEGER) ? VAR_RAW_INT : VAR_RAW_REAL;
            break;
        }
    }

    parse_block(c);
    P.function_depth--;

    if (!parse_had_error) {
        /* Implicit 'return null' if control falls off the end. Given a slot above everything the
           body used rather than one recycled from the temp allocator: this instruction usually
           never executes, and recycling ties a tagged write to a register the hot loop writes
           unchecked, which forfeits that register's tag elision for the whole function. Costs one
           register in a frame whose peak was set elsewhere. */
        if (P.slot_next < P.slot_max)
            P.slot_next = P.slot_max;
        int rk_null = (int)chunk_add_pool(c, aer_null()) | RK_CONST_FLAG;
        int reg_null = materialize(c, rk_null);
        emit_return(c, reg_null);
    }

    /* A body holding real slots is given the whole bank, since those slots sit at the top of it.
       The gap that leaves between the dynamically typed peak and them is what frame_bounds names, so
       neither frame entry nor the collector pays for a register the body never had. */
    *out_max_registers = raw_real_used() ? FRAME_REGISTERS : (unsigned int)P.slot_max;
    *out_frame_bounds = FRAME_BOUNDS((unsigned int)P.slot_max, (unsigned int)P.raw_real_low);

    P.var_count = saved_var_count;
    memcpy(P.var_names, saved_var_names, sizeof(unsigned int) * (size_t)saved_var_count);
    memcpy(P.var_regs, saved_var_regs, sizeof(int) * (size_t)saved_var_count);
    memcpy(P.var_kind, saved_var_kind, sizeof(VarKind) * (size_t)saved_var_count);
    P.slot_next = saved_next_temp;
    P.slot_floor = saved_reserved_floor;
    P.slot_max = saved_max_slot_used;
    P.raw_real_next = saved_raw_real_next;
    P.raw_real_floor = saved_raw_real_floor;
    P.raw_real_low = saved_raw_real_low;
    memcpy(P.reg_elem_kind, saved_elem_kind, sizeof(saved_elem_kind));
    memcpy(P.reg_len_class, saved_len_class, sizeof(saved_len_class));
}

/* Named functions can't nest. Once one parameter has a default, every parameter after it must
   too. */
static void parse_function(Chunk* c) {
    if (!equal(TOKEN_IDENTIFIER)) {
        return error_at("Expected function name");
    }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    if (is_builtin_name(c, name_idx)) {
        return error_at("'%s' is a reserved builtin name and can't be redefined as a function",
                        aer_as_string(token.value)->data);
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
    if (parse_had_error)
        return;

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

    unsigned int captured_max_registers;
    unsigned short captured_frame_bounds;
    unsigned int raw_boxed_before = P.raw_boxed_emits;
    int saved_func_idx = P.current_func_idx;
    bool saved_self_call = P.self_call_seen;
    P.current_func_idx = (int)this_func_idx;
    P.self_call_seen = false;
    parse_function_body(c, param_names, param_count, -1, NULL, false, NULL, NULL, 0, &captured_max_registers,
                        &captured_frame_bounds);
    unsigned int raw_boxed_in_body = P.raw_boxed_emits - raw_boxed_before;
    P.current_func_idx = saved_func_idx;
    P.self_call_seen = saved_self_call;

    /* Fold P.shape_sensitive_param[] into one bitmask; retain the source span (owned copy, see
       ChunkFunction.source_span's own comment) only when it's actually needed -- the common case
       (not shape-sensitive) pays nothing beyond the mask computation itself. parse_function_body
       already restored P.shape_sensitive_param/P.current_param_count's OWN inputs, but not the fold-in
       -- reads them here, right after the call, before anything else can touch them. */
    unsigned int shape_mask = 0;
    for (int i = 0; i < param_count && i < 31; i++)
        if (P.shape_sensitive_param[i])
            shape_mask |= (1u << i);
    if (shape_mask == 0 && param_count > 0 && raw_boxed_in_body > 0)
        shape_mask = SHAPE_MASK_NUMERIC_ONLY;
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
    c->functions[this_func_idx].frame_bounds = captured_frame_bounds;

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
    unsigned int max_registers = 0;
    unsigned short frame_bounds = 0;
    P.current_func_idx = (int)(target_f - c->functions);
    /* Only a numeric variant may self-call without resolving: a shape-specialized body is chosen by
       the argument's SHAPE, which a recursive call has no guarantee of preserving. */
    P.in_variant = (raw_param_count > 0);
    if (!parse_had_error) {
        parse_function_body(c, param_names, param_count, param_index, shape,
                            kind == SPEC_KIND_ARRAY_OF_STRUCTS, raw_param_regs, raw_param_types,
                            raw_param_count, &max_registers, &frame_bounds);
    }
    P.in_variant = false;
    bool ok = !parse_had_error;

    lexer_restore_state(saved_lexer);
    parser_restore_state(saved_parser);
    parse_had_error = saved_had_error;

    if (!ok)
        return false;

    out_entry->code_offset = new_offset;
    out_entry->max_registers = max_registers;
    out_entry->frame_bounds = frame_bounds;
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
        if (token.type != TOKEN_CLOSE_BRACKET)
            return false;
        AerArray* a = vm_new_array();
        a->count = a->capacity = 0;
        a->items = NULL;
        a->shape = NULL;
        a->generation = 0;
        *out = aer_array_val(a);
    } else if (token.type == TOKEN_OPEN_BRACE) {
        lex();
        if (token.type != TOKEN_CLOSE_BRACE)
            return false;
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
        return error_at("Expected struct name");
    }
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();
    require(TOKEN_COLON, "expected ':' after struct name");
    require(TOKEN_NEW_LINE, "expected newline before indented struct body");
    require(TOKEN_INDENT, "expected indented struct body");
    if (parse_had_error)
        return;

    unsigned int field_names[MAX_STRUCT_FIELDS];
    AerVal field_defaults[MAX_STRUCT_FIELDS];
    ValueType field_types[MAX_STRUCT_FIELDS];
    bool field_narrow[MAX_STRUCT_FIELDS];
    unsigned int field_count = 0;

    while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_DEDENT)) {
        if (consume(TOKEN_NEW_LINE))
            continue;
        if (!equal(TOKEN_IDENTIFIER)) {
            return error_at("Expected field name");
        }
        if (field_count >= MAX_STRUCT_FIELDS) {
            return error_at("Too many struct fields (max %d)", MAX_STRUCT_FIELDS);
        }

        unsigned int fname = chunk_add_pool(c, token.value);
        lex();

        /* No type annotation -- a field's type is always exactly its default's type. Every field
           already needs an explicit default (below), and that default was always required to match
           its own declared type anyway, so a separate ': type' never carried information the
           default didn't already have. 'null' is the one default with no matching ValueType, and
           is what TYPE_ANY (unconstrained) means here -- never a spelled-out keyword. */
        if (!consume(TOKEN_ASSIGN)) {
            return error_at("Struct field '%.*s' must have an explicit default value",
                            (int)aer_as_string(c->pool[fname])->length, aer_as_string(c->pool[fname])->data);
        }
        AerVal dflt;
        bool narrow;
        if (!parse_literal_default(c, &dflt, &narrow)) {
            return error_at("Struct field defaults must be a literal value");
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
            return error_at("Struct field '%.*s' default is out of range for a narrow (int32) field",
                            (int)aer_as_string(c->pool[fname])->length, aer_as_string(c->pool[fname])->data);
        }
        field_count++;

        if (!equal(TOKEN_DEDENT) && !equal(TOKEN_END_OF_FILE))
            require(TOKEN_NEW_LINE, "expected newline after struct field");
        if (parse_had_error)
            return;
    }
    consume(TOKEN_DEDENT);

    if (field_count == 0) {
        return error_at("Struct must have at least one field");
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
        return error_at("'break' outside loop");
    }
    LoopContext* ctx = &P.loop_stack[P.loop_depth - 1];
    if (ctx->patch_count >= BREAK_MAX) {
        return error_at("Too many breaks in one loop (max %d)", BREAK_MAX);
    }
    chunk_emit(c, OP_JUMP);
    ctx->patches[ctx->patch_count++] = c->count;
    chunk_emit(c, 0); /* placeholder -- patched by loop_pop_and_patch once the loop ends */
}

static void parse_continue(Chunk* c) {
    if (P.loop_depth == 0) {
        return error_at("'continue' outside loop");
    }
    LoopContext* ctx = &P.loop_stack[P.loop_depth - 1];
    chunk_emit(c, OP_JUMP);
    if (ctx->rotated) {
        if (ctx->continue_patch_count >= BREAK_MAX) {
            return error_at("Too many continues in one loop (max %d)", BREAK_MAX);
        }
        ctx->continue_patches[ctx->continue_patch_count++] = c->count;
        chunk_emit(
            c,
            0); /* placeholder -- patched by loop_pop_and_patch_rotated once OP_ITER_RANGE_LOOP's own position is known */
    } else {
        emit_jump_target(c, ctx->top); /* known at compile time -- no patch needed */
    }
}

/* Auto-prints in shell mode, and either way frees the now-dead result register -- without the
   free, every bare call statement would permanently waste a register slot (fatal for a REPL
   session accumulating statements). */
static void discard_statement_result(Chunk* c, int dest) {
    if (parse_had_error)
        return;
    if (mode == MODE_SHELL)
        emit_print_repl(c, dest);
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
        if (aer_type(entry) != TYPE_STRING)
            continue;
        AerString* s = aer_as_string(entry);
        if (strlen(name) != s->length || memcmp(s->data, name, s->length) != 0)
            continue;
        *out = vm->call_stack[vm->call_depth].registers[P.var_regs[i]];
        return true;
    }
    return false;
}

void parser_reset(void) {
    reg_reset();
    P.last_cmp_offset = NO_OFFSET;
    P.raw_write_offset = NO_OFFSET;
    P.last_interp_offset = NO_OFFSET;
    P.current_func_idx = -1; /* 0 is a real function index, so zeroed is not "outside a body" */
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
    P.current_func_idx = -1; /* 0 is a real function index, so zeroed is not "outside a body" */
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
        if (consume(TOKEN_NEW_LINE))
            continue;
        if (consume(TOKEN_DEDENT))
            continue;
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
        if (any_pending_error)
            P.any_compile_error = true;
    }
    if (P.any_compile_error)
        parse_had_error = true;
}

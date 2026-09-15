#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include "aer_stdlib.h"
#include "aer_thread.h"
#include "error.h"
#include "lexer.h"

static aer_mutex specialize_lock = AER_MUTEX_INIT;

/* VAR_RAW_* is earned by a name whose first assignment is provably int/real outside any branch,
   and is lost on a mismatch; VAR_BOXED never promotes back. */
typedef enum { VAR_BOXED, VAR_RAW_INT, VAR_RAW_REAL } VarKind;

/* No instruction to fold into or retarget -- see LastInstruction. */
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
#define EXPR_DEPTH_MAX 256
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
    bool is_int;
    bool from_pool; /* int too wide for OP_RAW_LOAD_INT's int32 immediate */
    int64_t value; /* ints: the value itself, so dedup never depends on pool identity */
    unsigned int rawk_idx;
    int slot;
} HoistedConstant;

typedef struct {
    unsigned int gap_offset;
    int count;
    HoistedConstant consts[HOIST_MAX];
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

/* What the parser has worked out about each register. invalidate_register clears a register's
   entry across all of them at once, which is why they live together. */
typedef struct {
    VarKind var_kind[FRAME_REGISTERS];
    /* Which of this function's parameters were used as the base of a struct-field access, directly
       or through a one-hop alias. See mark_shape_sensitive for how the three work together. */
    bool shape_sensitive_param[FRAME_REGISTERS];
    int current_param_count;
    int alias_source_param[FRAME_REGISTERS]; /* -1 = no known alias */
    /* Set only during a specialization recompile, and read at field-access sites to skip the
       generic runtime resolution. */
    Shape* reg_known_shape[FRAME_REGISTERS];
    /* Proven >= 0: a non-negative literal, a bounded loop index, a length(), or those combined with
       + * // %. Wrong in one direction only -- a false positive is caught at runtime by
       OP_ITER_RANGE_PREP's own guard, so it can never be a safety hole. */
    bool reg_nonneg[FRAME_REGISTERS];
    Shape* reg_known_element_shape[FRAME_REGISTERS];
    /* Element kind of a typed array a register holds, from a `[numeric; count]` literal -- a
       parse-time fact, so the read can go straight to a raw slot. */
    RawKind reg_elem_kind[FRAME_REGISTERS];
} RegFacts;

/* The last foldable instruction emitted: a comparison or boxed binary op, raw arithmetic (raw_kind
   names its destination slot's kind), or an OP_INTERP. A later instruction may fold into it or
   retarget it only while last_instruction() still returns it -- instruction lengths vary, so it
   cannot be found by reading backwards, and a stale offset rewrites code another path can reach. */
typedef struct {
    unsigned int offset;
    unsigned int length;
    unsigned int epoch;
    RawKind raw_kind;
} LastInstruction;

typedef struct {
    LastInstruction last;
    /* Counts backpatches: a jump landing after the last instruction would make a rewrite of it
       reachable on paths the original was not. */
    unsigned int patch_epoch;
} PeepholeWindow;

/* An index read's element facts, handed to the assignment that consumes its result. Keyed on the
   exact result register and consumed at once, so a freed-and-reused temp cannot pick them up. */
typedef struct {
    int dest_reg;
    int src_param;
    Shape* elem_shape;
} IndexAliasHandoff;

/* Lets a `for i in 0..n:` loop skip an array's index-side runtime checks when n is proven ==
   length() of the SAME array it indexes. Every fact is tracked per-array-register and re-checked
   at use, never keyed to one "interesting" parameter, so proving a bound safe for one array can't
   let a different one borrow the proof. invalidate_register must poison a safe_loop_* entry when
   EITHER the index or the array half is reassigned -- the array half is the easier one to forget. */
typedef struct {
    int hint_param_reg;
    unsigned int length_tracked_name;
    bool length_tracked_valid;
    int length_tracked_source_reg;
    int last_length_call_result_reg;
    int last_length_call_arg_reg;
    int safe_loop_item_regs[LOOP_MAX];
    int safe_loop_array_regs[LOOP_MAX];
    int safe_loop_depth;
} LoopProofs;

/* The function body currently compiling. */
typedef struct {
    /* Index of the function whose body is compiling; -1 outside any function body. */
    int current_func_idx;
    /* Set only while a SPECIALIZED body is compiling, so a self-call inside it can skip resolution
       (OP_CALL_SELF, vm.h). A generic body must not: resolution is what triggers specialization in
       the first place, so bypassing it there means the variant is never compiled at all. */
    bool in_variant;
    /* Raw-vs-boxed opcodes reached for in this body. Nonzero means binding its numeric parameters
       raw would turn real work raw, which is the trigger for a numeric specialization. */
    unsigned int raw_boxed_emits;
} FuncCtx;

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

    RegFacts regs;
    PeepholeWindow peep;
    IndexAliasHandoff index_alias;
    LoopProofs proof;
    FuncCtx fn;
    /* Every register's current variable binding (0 registers is effectively local -- see
       var_kind below for storage-kind tracking). */
    unsigned int var_names[FRAME_REGISTERS];
    int var_regs[FRAME_REGISTERS];
    int var_count;
    VarKind var_kind[FRAME_REGISTERS];


    /* Highest temp register reached while compiling the loop condition currently being parsed --
       see parse_for_body for why the body must not be allowed to claim one of these. */
    int loop_cond_peak;


    /* Nonzero while compiling an if/else branch -- disqualifies raw storage (see VarKind). */
    int branch_depth;
    /* Nonzero while compiling a function body -- lets parse_return reject a top-level return. */
    int function_depth;
    /* Bounds parse_binary's recursion; statement nesting is already bounded by the lexer's own
       indentation cap. */
    int expr_depth;


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

static void peephole_window_reset(void) {
    P.peep.last.offset = NO_OFFSET;
}

/* Records the instruction from `offset` to the end of the chunk as the one a later emit may fold. */
static void note_last_instruction(Chunk* c, unsigned int offset, RawKind raw_kind) {
    P.peep.last = (LastInstruction){offset, c->count - offset, P.peep.patch_epoch, raw_kind};
}

/* Where the recorded instruction starts, or NO_OFFSET once anything was written after it or a jump
   was pointed past it. */
static unsigned int last_instruction(Chunk* c) {
    const LastInstruction* last = &P.peep.last;
    if (last->offset == NO_OFFSET || last->offset + last->length != c->count ||
        last->epoch != P.peep.patch_epoch)
        return NO_OFFSET;
    return last->offset;
}

/* Rewrites field A of the last instruction to dest, and forgets it: it now writes somewhere else. */
static void retarget_last(Chunk* c, unsigned int at, int dest) {
    uint32_t w = c->code[at];
    c->code[at] = PACK3((Opcode)(w & 0xFF), dest, UNPACK_B(w), UNPACK_C(w));
    P.peep.last.offset = NO_OFFSET;
}

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
    P.regs.reg_nonneg[slot] = false; /* same as reg_alloc: no proof carries over from the last occupant */
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
    P.regs.reg_nonneg[reg] = false; /* a recycled register carries no proof from its last occupant */
    P.regs.reg_elem_kind[reg] = RAWK_NONE;
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
        if (P.regs.var_kind[i] == VAR_RAW_REAL && P.var_regs[i] >= P.raw_real_floor)
            continue;
        if (P.regs.var_kind[i] != VAR_BOXED)
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

/* `+=` on a loop-proven packed element gets an opcode that skips the dispatch-time switch; every
   other compound form keeps the general one (see the ADD opcodes' own comment, vm.h). */
static Opcode compound_add_variant(Opcode op, Opcode bin_op) {
    if (bin_op != OP_ADD)
        return op;
    switch (op) {
        case OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED: return OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED_ADD;
        case OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED: return OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED_ADD;
        default: return op;
    }
}

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
    return reg >= 0 && reg < FRAME_REGISTERS && P.regs.reg_nonneg[reg];
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
    return !(rk & RK_CONST_FLAG) && drop_raw_marks(rk) < P.regs.current_param_count;
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
        P.regs.reg_nonneg[dest] = binop_preserves_nonneg(op) && rk_nonneg(c, rk_lhs) && rk_nonneg(c, rk_rhs);
    chunk_emit(c, PACK3(op, dest, pack_rk8(rk_lhs), pack_rk8(rk_rhs)));
    note_last_instruction(c, c->count - 1, RAWK_NONE);
    /* A parameter reaching the fully boxed path is the clearest sign binding it raw would pay --
       and the only sign at all for a body with no raw local for the _BOXED family to catch. Only
       arithmetic and ordering: equality and `in` are defined on every type, so they say nothing
       about whether the operand is a number. */
    if ((op <= OP_FLOOR_DIV || (op >= OP_LT && op <= OP_GTE)) && (rk_param(rk_lhs) || rk_param(rk_rhs)))
        P.fn.raw_boxed_emits++;
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
    unsigned int last = last_instruction(c);
    if (last != NO_OFFSET && last >= cond_start)
        cmp_word_start = last;
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
    P.peep.patch_epoch++;
}

/* A call's callee_offset is an absolute function entry, not intra-function control flow. */
void patch_call_target(Chunk* c, unsigned int patch_offset, unsigned int target) {
    c->code[patch_offset] = (uint32_t)target;
}

/* The two register-to-register primitives. Every site that moves a value into a register goes
   through one of these, so the word layout is written once rather than at each of twenty. */
static void emit_loadk(Chunk* c, int reg, unsigned int pool_idx) {
    chunk_emit(c, PACK_OP_A_W16(OP_LOADK, reg, pool_idx));
}

static void emit_move(Chunk* c, int dest, int src) {
    chunk_emit(c, PACK2(OP_MOVE, dest, src));
}

/* An RK operand lands in a register either way: a constant loads from the pool, a register moves. */
static void emit_move_rk(Chunk* c, int dest, int rk) {
    if (rk & RK_CONST_FLAG)
        emit_loadk(c, dest, (unsigned int)(rk & ~RK_CONST_FLAG));
    else
        emit_move(c, dest, rk);
}

/* Emits an already-known jump target in the same encoding patch_jump writes. */
void emit_jump_target(Chunk* c, unsigned int target) {
    chunk_emit(c, (uint32_t)(int32_t)((int64_t)target - (int64_t)c->count - 1));
}

/* Returns the callee_offset word's offset for a forward-referencing call to patch later
   (pending_call_add); an already-resolved call ignores the return value. func_index is the
   target's index into chunk->functions[] -- h_call reads it to size the callee's frame from its
   real max_registers peak instead of a flat, function-agnostic ceiling. */
unsigned int emit_call(Chunk* c, int dest_reg, unsigned int callee_offset, int arg_reg_base, int arg_count,
                       unsigned int func_index) {
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

/* A module function call: OP_CALL_MODULE's word, the module and function name constants, then the
   resolved ids -- the module's is CALL_MODULE_DYNAMIC when it resolves at runtime. */
static void emit_module_call(Chunk* c, int dest, int base, int argc, unsigned int module_idx,
                             unsigned int fn_idx, int module_id, int fn_id) {
    chunk_emit(c, PACK3(OP_CALL_MODULE, dest, base, argc));
    chunk_emit(c, (uint32_t)module_idx);
    chunk_emit(c, (uint32_t)fn_idx);
    chunk_emit(c, PACK_2X16((uint16_t)module_id, (uint16_t)fn_id));
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
    unsigned int at = last_instruction(c);
    if (at == NO_OFFSET)
        return false;
    if (arr_reg == rk_idx)
        return false; /* the receiver is what we are about to stop writing */
    uint32_t w = c->code[at];
    if ((Opcode)(w & 0xFF) != OP_INTERP || (int)UNPACK_A(w) != rk_idx)
        return false;

    unsigned int parts = UNPACK_B(w);
    uint32_t operands[INTERP_MAX_PARTS];
    for (unsigned int i = 0; i < parts; i++)
        operands[i] = c->code[at + 1 + i];
    c->count = at;
    chunk_emit(c, PACK3(OP_INDEX_GET_INTERP, dest_reg, arr_reg, (int)parts));
    for (unsigned int i = 0; i < parts; i++)
        chunk_emit(c, operands[i]);
    P.peep.last.offset = NO_OFFSET;
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
    RawKind elem = (arr_reg >= 0 && arr_reg < FRAME_REGISTERS) ? P.regs.reg_elem_kind[arr_reg] : RAWK_NONE;
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

/* reg is the parameter's own register (0..P.regs.current_param_count-1) OR a register whose value is
   known (via P.regs.alias_source_param) to have come from indexing that parameter -- either way, marks
   that parameter shape-sensitive. Safe to call with any register (out-of-range/no-alias is a
   silent no-op), matching var_lookup_rk's own "harmless on a miss" convention. See the Parser
   struct's own field comments (top of file) for what each of these tables tracks. */
static void mark_shape_sensitive(int reg) {
    if (reg < 0 || reg >= FRAME_REGISTERS)
        return;
    if (reg < P.regs.current_param_count) {
        P.regs.shape_sensitive_param[reg] = true;
        return;
    }
    int src = P.regs.alias_source_param[reg];
    if (src >= 0)
        P.regs.shape_sensitive_param[src] = true;
}

/* True iff (arr_reg, idx_rk) matches a pair on the safe_loop_item/array_regs stack. Both halves
   must match: a bound proven for array A must never be trusted for a different array B that
   happens to reuse the same index register. idx_rk must be a plain register.
   Packed-array field callers additionally require arr_reg == P.proof.hint_param_reg (the field offset
   is only valid for that one specialized parameter); typed-array callers do not. */
static bool index_safe_unchecked(int arr_reg, int idx_rk) {
    if (idx_rk & RK_CONST_FLAG)
        return false;
    /* The static-type marks name a register now, not a separate bank, so a typed index is still an
       ordinary register and this proof is keyed on register identity. Rejecting it outright left
       sieve's marking loop on the generic OP_INDEX_SET the moment its loop variable became typed. */
    int idx_reg = drop_raw_marks(idx_rk);
    for (int i = 0; i < P.proof.safe_loop_depth; i++) {
        if (P.proof.safe_loop_item_regs[i] == idx_reg && P.proof.safe_loop_array_regs[i] == arr_reg)
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
    if (P.proof.length_tracked_valid && reg == P.proof.length_tracked_source_reg)
        P.proof.length_tracked_valid = false;
    if (reg < FRAME_REGISTERS) {
        P.regs.reg_elem_kind[reg] = RAWK_NONE;
        /* Whatever it held is gone, so it is no longer that array; and a count read from it after
           this is a different value, which must not match one read before. */
    }
    for (int i = 0; i < P.proof.safe_loop_depth; i++) {
        if (P.proof.safe_loop_item_regs[i] == reg)
            P.proof.safe_loop_item_regs[i] = -1;
        if (P.proof.safe_loop_array_regs[i] == reg)
            P.proof.safe_loop_array_regs[i] = -1;
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
        P.regs.reg_nonneg[reg] = false;
    note_slot_written(reg);
}

/* invalidate_register plus what the register's value was known to be -- its shape and the parameter
   it was indexed from -- for a register about to take a value nothing is known about. */
static void forget_register(int reg) {
    if (reg >= 0 && reg < FRAME_REGISTERS) {
        P.regs.reg_known_shape[reg] = NULL;
        P.regs.reg_known_element_shape[reg] = NULL;
        P.regs.alias_source_param[reg] = -1;
    }
    invalidate_register(reg);
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

/* Nonzero while compiling an if/else branch -- disqualifies raw storage (see P.regs.var_kind). A
   real counter since if/else nests. */

/* True (after reporting the error) if name_idx is a top-level variable. Report it while the offending
   name is still the current token, or the error names a later line. */
static bool report_top_level_name(Chunk* c, unsigned int name_idx) {
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

/* report_top_level_name, when the caller is inside a function body -- see the Parser struct's own
   field comments (top of file) for how global_names/function_depth track this. */
static bool report_if_shadowed_global(Chunk* c, unsigned int name_idx) {
    return P.function_depth > 0 && report_top_level_name(c, name_idx);
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
    /* P.regs.var_kind[] persists across every function's compilation and save/restore only shrinks
       P.var_count, so an earlier function's local leaves a stale kind at this index. Reset here,
       the one place every name is created. */
    P.regs.var_kind[P.var_count] = VAR_BOXED;
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
        if (P.regs.var_kind[i] == VAR_RAW_REAL) {
            if (P.slot_floor >= P.raw_real_next)
                return error_at("Too many variables (max %d)", FRAME_REGISTERS);
            P.var_regs[i] = P.slot_floor++;
            P.slot_next = P.slot_floor;
            track_peak(P.slot_floor);
        }
        P.regs.var_kind[i] = VAR_BOXED;
        return;
    }
}

/* Materializes a constant via OP_LOADK, or boxes a raw value -- OP_JUMP_IF_FALSE_REG needs an
   actual register, no RK/raw form. */
/* True when the RHS just compiled to exactly one raw real MUL writing the slot rk_rhs names, which
   is what lets `field += a*b` fold the multiply in. Rewinds the MUL and hands back its operands;
   the caller emits the fused opcode instead. Mirrors the raw-local fusion in parse_binary_ops. */
static bool take_fused_mul_real(Chunk* c, unsigned int rhs_start, int rk_rhs, int* mul_a, int* mul_b) {
    if (c->count - rhs_start != 1)
        return false;
    uint32_t mw = c->code[rhs_start];
    if ((Opcode)(mw & 0xFF) != OP_RAW_MUL_REAL || (int)UNPACK_A(mw) != (rk_rhs & RK_RAW_SLOT_MASK))
        return false;
    *mul_a = (int)UNPACK_B(mw);
    *mul_b = (int)UNPACK_C(mw);
    c->count = rhs_start;
    raw_release_if_top((int)UNPACK_A(mw));
    return true;
}

static int materialize(Chunk* c, int rk) {
    rk = drop_raw_marks(rk);
    if (!(rk & RK_CONST_FLAG))
        return rk;
    int reg = reg_alloc();
    /* dest+pool_idx both fit word0 now (op(8)+dest(8)+pool_idx(16)) -- no trailing word. */
    emit_loadk(c, reg, (unsigned int)(rk & ~RK_CONST_FLAG));
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
        switch (P.regs.var_kind[i]) {
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
        const HoistedConstant* k = &h->consts[i];
        if (!k->is_int) {
            c->code[w++] = PACK1(OP_RAW_LOAD_REAL, k->slot);
            c->code[w++] = k->rawk_idx;
        } else if (k->from_pool) {
            c->code[w++] = PACK1(OP_RAW_LOAD_INT_POOL, k->slot);
            c->code[w++] = k->rawk_idx;
        } else {
            c->code[w++] = PACK1(OP_RAW_LOAD_INT, k->slot);
            c->code[w++] = (uint32_t)(int32_t)k->value;
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
        const HoistedConstant* k = &h->consts[i];
        if (k->is_int != is_int)
            continue;
        if (is_int ? (k->value == value) : (k->rawk_idx == rawk_idx))
            return k->slot;
    }
    if (h->count >= HOIST_MAX)
        return -1;
    int slot;
    if (is_int) {
        if (h->slot_base < 0)
            return -1;
        slot = h->slot_base + h->int_count++;
    } else {
        if (h->real_base < 0)
            return -1;
        slot = h->real_base - h->real_count++;
    }
    h->consts[h->count++] = (HoistedConstant){is_int, from_pool, value, rawk_idx, slot};
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

/* Writes an assignment's value straight into the variable's slot, by re-pointing the arithmetic
   that produced it, so `x = a * b + c` needs no move afterwards. The source slot must be a temp:
   retargeting one that belongs to a variable would drop that variable's own value. */
static bool retarget_raw_write(Chunk* c, int src_slot, int dest_slot, RawKind kind) {
    unsigned int at = last_instruction(c);
    if (at == NO_OFFSET || P.peep.last.raw_kind != kind || (int)UNPACK_A(c->code[at]) != src_slot)
        return false;
    if (!raw_is_temp(src_slot))
        return false;
    retarget_last(c, at, dest_slot);
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

/* A boxed operator's raw-native forms. `a > b` is `b < a`: emitted as the LT/LTE opcode with the slots
   swapped, so the raw family needs no GT/GTE members. OP_HALT marks a kind with no raw form. */
typedef struct {
    Opcode boxed, raw_int, raw_real;
    bool is_cmp, swap;
} RawOpForm;

static const RawOpForm raw_op_forms[] = {
    {OP_ADD, OP_RAW_ADD_INT, OP_RAW_ADD_REAL, false, false},
    {OP_SUB, OP_RAW_SUB_INT, OP_RAW_SUB_REAL, false, false},
    {OP_MUL, OP_RAW_MUL_INT, OP_RAW_MUL_REAL, false, false},
    {OP_DIV, OP_RAW_DIV_INT, OP_RAW_DIV_REAL, false, false},
    {OP_MOD, OP_RAW_MOD_INT, OP_HALT, false, false},
    {OP_FLOOR_DIV, OP_RAW_FLOOR_DIV_INT, OP_HALT, false, false},
    {OP_LT, OP_RAW_LT_INT, OP_RAW_LT_REAL, true, false},
    {OP_GT, OP_RAW_LT_INT, OP_RAW_LT_REAL, true, true},
    {OP_LTE, OP_RAW_LTE_INT, OP_RAW_LTE_REAL, true, false},
    {OP_GTE, OP_RAW_LTE_INT, OP_RAW_LTE_REAL, true, true},
    {OP_EQ, OP_RAW_EQ_INT, OP_RAW_EQ_REAL, true, false},
    {OP_NEQ, OP_RAW_NEQ_INT, OP_RAW_NEQ_REAL, true, false},
};

static const RawOpForm* raw_op_form(Opcode op) {
    for (size_t i = 0; i < sizeof(raw_op_forms) / sizeof(raw_op_forms[0]); i++)
        if (raw_op_forms[i].boxed == op)
            return &raw_op_forms[i];
    return NULL;
}

/* With one raw real operand, unboxes the other into a raw slot so the result stays raw. Arithmetic only
   (these reject a non-number anyway; `5 == "5"` must stay false), real only (OP_UNBOX_REAL widens an int,
   an int slot cannot hold a real), and never a typed array, which `a * 2.0` broadcasts across. */
static void unbox_other_side_real(Chunk* c, Opcode op, int* rk_lhs, RawKind* kind_lhs, int* rk_rhs,
                                  RawKind* kind_rhs) {
    if ((*kind_lhs == RAWK_NONE) == (*kind_rhs == RAWK_NONE) ||
        !(op == OP_ADD || op == OP_SUB || op == OP_MUL))
        return;
    bool lhs_raw = (*kind_lhs != RAWK_NONE);
    int boxed_rk = lhs_raw ? *rk_rhs : *rk_lhs;
    int boxed_reg = drop_raw_marks(boxed_rk);
    bool boxed_is_typed_array = !(boxed_rk & RK_CONST_FLAG) && boxed_reg >= 0 &&
                                boxed_reg < FRAME_REGISTERS && P.regs.reg_elem_kind[boxed_reg] != RAWK_NONE;
    if ((lhs_raw ? *kind_lhs : *kind_rhs) != RAWK_REAL || boxed_is_typed_array ||
        (boxed_rk & (RK_CONST_FLAG | RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG)))
        return;
    release_if_top(boxed_rk);
    int tmp = slot_alloc(RAWK_REAL);
    if (tmp < 0)
        return;
    chunk_emit(c, PACK3(OP_UNBOX_REAL, tmp, boxed_rk, 0));
    if (lhs_raw) {
        *rk_rhs = RK_RAW_REAL_FLAG | tmp;
        *kind_rhs = RAWK_REAL;
    } else {
        *rk_lhs = RK_RAW_REAL_FLAG | tmp;
        *kind_lhs = RAWK_REAL;
    }
}

/* The raw-constant index a literal right operand folds to, when it fits `limit`; -1 otherwise. */
static int rhs_constant_index(Chunk* c, int rk_rhs, RawKind kind, unsigned int limit) {
    int const_rk;
    if (!(rk_rhs & RK_CONST_FLAG) || !rawk_const_rk(c, rk_rhs, kind, &const_rk))
        return -1;
    unsigned int idx = (unsigned int)(const_rk & ~RK_CONST_FLAG);
    return idx <= limit ? (int)idx : -1;
}

/* Returns false if the operator has no raw-native form (raw_op_forms) or the operand kinds mismatch,
   and the caller falls back to the boxed path. Bitwise, AND/OR and IN always stay boxed. */
static bool try_emit_binary_raw(Chunk* c, Opcode op, int rk_lhs, int rk_rhs, int* out_rk) {
    RawKind kind_lhs = rk_raw_kind(c, rk_lhs);
    RawKind kind_rhs = rk_raw_kind(c, rk_rhs);

    unbox_other_side_real(c, op, &rk_lhs, &kind_lhs, &rk_rhs, &kind_rhs);
    if (kind_lhs == RAWK_NONE || kind_rhs == RAWK_NONE || kind_lhs != kind_rhs)
        return false;
    bool int_kind = (kind_lhs == RAWK_INT);

    const RawOpForm* form = raw_op_form(op);
    Opcode raw_op = form ? (int_kind ? form->raw_int : form->raw_real) : OP_HALT;
    if (raw_op == OP_HALT)
        return false;
    bool is_cmp = form->is_cmp;
    bool swap_cmp = form->swap;
    bool div_int_promotes_to_real = int_kind && op == OP_DIV; /* two ints still divide to a real */

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
    if (is_cmp && !swap_cmp) {
        int idx = rhs_constant_index(c, rk_rhs, kind_lhs, RK8_INDEX_MASK);
        if (idx >= 0)
            rhs_field = (int)(RK8_CONST_FLAG | (unsigned int)idx);
    }
    /* Same idea for integer +/-, but via a dedicated opcode whose C field is a bare index (see
       OP_RAW_ADD_INT_K). Worth it only because a recursive body re-runs the load it replaces on
       every call, where a loop's would have been hoisted once into the preheader. */
    if (!is_cmp && int_kind && (raw_op == OP_RAW_ADD_INT || raw_op == OP_RAW_SUB_INT)) {
        int idx = rhs_constant_index(c, rk_rhs, RAWK_INT, 0xFFu);
        if (idx >= 0) {
            rhs_field = idx;
            raw_op = (raw_op == OP_RAW_ADD_INT) ? OP_RAW_ADD_INT_K : OP_RAW_SUB_INT_K;
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
        chunk_emit(c, PACK3(raw_op, dest, swap_cmp ? slot_rhs : slot_lhs, swap_cmp ? slot_lhs : rhs_field));
        note_last_instruction(c, c->count - 1, RAWK_NONE);
        *out_rk = dest;
        return true;
    }
    if (div_int_promotes_to_real) {
        int dest = slot_alloc(RAWK_REAL);
        if (dest < 0)
            return false; /* extremely unlikely right after freeing 2 int slots, but stay safe */
        chunk_emit(c, PACK3(raw_op, dest, slot_lhs, rhs_field));
        note_last_instruction(c, c->count - 1, RAWK_REAL);
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
        P.regs.reg_nonneg[dest] = binop_preserves_nonneg(op) && rk_nonneg(c, rk_lhs) && rk_nonneg(c, rk_rhs);
    note_last_instruction(c, c->count - 1, int_kind ? RAWK_INT : RAWK_REAL);
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
        emit_loadk(c, target, (unsigned int)(rk & ~RK_CONST_FLAG));
    } else {
        emit_move(c, target, rk);
        if (rk >= 0 && rk < FRAME_REGISTERS)
            P.regs.reg_elem_kind[target] = P.regs.reg_elem_kind[rk];
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
    if (!equal(close_tok)) {
        int rk = parse_binary(c, 0);
        if (!equal(TOKEN_COMMA)) {
            base = materialize(c, rk);
            *out_base_is_temp = is_temp(base);
            *out_base = base;
            return 1;
        }
        base = arg_materialize(c, rk);
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

/* One OP_INTERP over `parts`. The parts' temps are released before the destination is claimed so it
   reuses the lowest of them -- safe because the opcode reads every part before writing its
   destination. */
static int emit_interp(Chunk* c, const int* parts, int part_count) {
    int temps = 0;
    for (int i = 0; i < part_count; i++)
        if (is_temp(parts[i]))
            temps++;
    if (temps)
        reg_free(temps);
    int dest = reg_alloc();
    unsigned int at = c->count;
    chunk_emit(c, PACK2(OP_INTERP, dest, part_count));
    for (int i = 0; i < part_count; i++)
        chunk_emit(c, pack_rk16(parts[i]));
    note_last_instruction(c, at, RAWK_NONE);
    return dest;
}

/* Index of the first '{' in s[i..len) not behind a backslash, or len. */
static unsigned int next_interp_open(const char* s, unsigned int len, unsigned int i) {
    while (i < len) {
        if (s[i] == '\\' && i + 1 < len) {
            i += 2;
            continue;
        }
        if (s[i] == '{')
            return i;
        i++;
    }
    return len;
}

/* Index of the '}' closing an interpolation whose body starts at i, or len when it never closes.
   Depth-aware, so a dict literal's own braces inside it do not end the scan early. */
static unsigned int interp_close(const char* s, unsigned int len, unsigned int i) {
    int depth = 1;
    while (i < len) {
        if (s[i] == '\\' && i + 1 < len) {
            i += 2;
            continue;
        }
        if (s[i] == '{')
            depth++;
        else if (s[i] == '}' && --depth == 0)
            return i;
        i++;
    }
    return len;
}

/* Folds a full OP_INTERP part list into one part, so a string has no limit on how many it has. */
static void interp_make_room(Chunk* c, int* parts, int* part_count) {
    if (*part_count == INTERP_MAX_PARTS) {
        parts[0] = emit_interp(c, parts, *part_count);
        *part_count = 1;
    }
}

static int parse_string_literal(Chunk* c) {
    AerString* ts = aer_as_string(token.value);
    char* s = ts->data;
    unsigned int len = ts->length;

    if (next_interp_open(s, len, 0) == len) {
        unsigned int pool_idx = pool_escaped_string(c, s, len);
        lex();
        return (int)pool_idx | RK_CONST_FLAG;
    }

    /* Collected as RK values and emitted as one OP_INTERP: one dispatch and one allocation, not a
       concatenate per part. */
    int parts[INTERP_MAX_PARTS];
    int part_count = 0;
    unsigned int i = 0;
    while (i <= len) {
        unsigned int seg_start = i;
        i = next_interp_open(s, len, i);
        if (i > seg_start) {
            /* A constant segment stays an RK constant -- OP_INTERP reads it straight from the pool. */
            int rk_seg = (int)pool_escaped_string(c, s + seg_start, i - seg_start) | RK_CONST_FLAG;
            interp_make_room(c, parts, &part_count);
            parts[part_count++] = rk_seg;
        }
        if (i >= len)
            break;

        unsigned int expr_start = i + 1;
        i = interp_close(s, len, expr_start);
        if (i == len) {
            error_at("Unclosed '{' in string");
            break;
        }
        if (i == expr_start) {
            error_at("Empty '{}' in string");
            i++;
            continue;
        }
        int rk_expr = parse_interpolated_expr(c, s + expr_start, i - expr_start);
        if (!parse_had_error) {
            /* No OP_TO_STR: OP_INTERP formats an int/real/bool/null part directly into the result. */
            interp_make_room(c, parts, &part_count);
            parts[part_count++] = rk16_fits(rk_expr) ? rk_expr : materialize(c, rk_expr);
        }
        i++; /* past '}' */
    }

    /* A string with no interpolation at all already returned above, so a single part here is a single
       interpolated expression -- which still has to be converted, whatever its type. */
    int result;
    if (part_count > 0)
        result = emit_interp(c, parts, part_count);
    else
        result = (int)chunk_add_pool(c, aer_make_string_copy("", 0)) | RK_CONST_FLAG;
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

/* The rest of `[value; count]` once the fill value is parsed. narrow_flag is 1 or 2 when the fill was a
   lone i- or f-suffixed literal. */
static int parse_array_repeat(Chunk* c, int rk_first, int narrow_flag) {
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
    /* Strict LIFO free order -- rk_count was allocated (if a temp at all) after fill_reg. */
    release_if_top(rk_count);
    release_if_top(fill_reg);
    int dest = reg_alloc();
    emit_array_repeat(c, dest, fill_reg, narrow_flag, rk_count);
    P.regs.reg_elem_kind[dest] = fill_kind;
    return dest;
}

/* `[a, b, c]` or `[value; count]`, after the '['. Nothing before the first element tells them apart, so
   it is parsed as an ordinary expression and the next token decides. */
static int parse_array_literal(Chunk* c) {
    if (consume(TOKEN_CLOSE_BRACKET)) {
        int dest = reg_alloc();
        emit_array_new(c, dest, dest, 0);
        return dest;
    }
    /* A suffix is parse-time-only information, so narrow storage must be decided from the token before
       parse_binary consumes it. "Not wrapped in anything else" is confirmed by the first element
       emitting zero opcodes -- a lone literal folds into a constant operand. */
    bool narrow_int_candidate = (token.type == TOKEN_INTEGER && token.narrow);
    bool narrow_float_candidate = (token.type == TOKEN_REAL && token.narrow);
    unsigned int emit_count_before = c->count;
    int rk_first = parse_binary(c, 0);
    if (parse_had_error)
        return 0;

    if (consume(TOKEN_SEMICOLON)) {
        int narrow_flag = 0;
        if (c->count == emit_count_before)
            narrow_flag = narrow_int_candidate ? 1 : narrow_float_candidate ? 2 : 0;
        return parse_array_repeat(c, rk_first, narrow_flag);
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

/* `{k: v, ...}` after the '{'. Key and value materialize back to back, landing at pair_reg_base+2i and
   +2i+1 to match OP_DICT_NEW's layout. */
static int parse_dict_literal(Chunk* c) {
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

/* A bare name, already consumed and not followed by '(': a variable, or a known function as a value. */
static int parse_name_reference(Chunk* c, unsigned int name_idx) {
    /* A raw-tracked name returns an RK_RAW_*_FLAG-tagged operand, letting a bare reference compose
       through further arithmetic without boxing. */
    int reg;
    if (var_lookup_rk(name_idx, &reg))
        return reg;

    /* Built once per reference as a deduped pool constant. AerFunction carries its own register peaks,
       so the function index is not needed. */
    unsigned int func_offset, func_arity, func_min_arity, func_max_registers, func_index_unused;
    unsigned short func_frame_bounds;
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

static int parse_primary_inner(Chunk* c) {
    if (consume(TOKEN_FUNCTION))
        return parse_function_expr(c);
    if (consume(TOKEN_OPEN_PARENTHESE)) {
        int rk = parse_binary(c, 0);
        require(TOKEN_CLOSE_PARENTHESE, "expected ')' after expression");
        return rk;
    }
    if (consume(TOKEN_OPEN_BRACKET))
        return parse_array_literal(c);
    if (consume(TOKEN_OPEN_BRACE))
        return parse_dict_literal(c);
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
        return parse_name_reference(c, name_idx);
    }
    error_at("Expected an expression (only literals, variables, calls, array/dict literals, "
             "arithmetic/comparisons, and parentheses are supported)");
    return 0;
}

/* `[index]`/slice/`.field` reads plus a trailing call-THROUGH-value (not call-by-name) --
   covers functions stored in a container and called via index/key, and currying as a side
   effect. `rk` is reused in place as the call's own dest_reg. */
typedef enum { FIELD_GET, FIELD_SET, FIELD_COMPOUND } FieldAccess;

/* The raw kind of field_idx on reg's compile-time-known shape, with its offset and narrowness: RAWK_NONE
   when the register has no known shape, the shape lacks the field, or the field is not an integer or
   real. reg may be -1 for a register whose shape is never tracked. */
static RawKind known_field_kind(int reg, unsigned int field_idx, unsigned int* offset, bool* narrow) {
    Shape* known = (reg >= 0 && reg < FRAME_REGISTERS) ? P.regs.reg_known_shape[reg] : NULL;
    ValueType type;
    if (!known || !shape_find_field(known, field_idx, offset, &type, narrow))
        return RAWK_NONE;
    return type == TYPE_INTEGER ? RAWK_INT : type == TYPE_REAL ? RAWK_REAL : RAWK_NONE;
}

/* The raw opcode for a `struct.field` read, store or compound: [access][narrow][real]. */
static Opcode field_raw_op(FieldAccess access, bool narrow, RawKind kind) {
    static const Opcode ops[3][2][2] = {
        {{OP_FIELD_GET_RAW_INT, OP_FIELD_GET_RAW_REAL}, {OP_FIELD_GET_RAW_INT32, OP_FIELD_GET_RAW_FLOAT32}},
        {{OP_FIELD_SET_RAW_INT, OP_FIELD_SET_RAW_REAL}, {OP_FIELD_SET_RAW_INT32, OP_FIELD_SET_RAW_FLOAT32}},
        {{OP_FIELD_COMPOUND_RAW_INT, OP_FIELD_COMPOUND_RAW_REAL},
         {OP_FIELD_COMPOUND_RAW_INT32, OP_FIELD_COMPOUND_RAW_FLOAT32}},
    };
    return ops[access][narrow][kind == RAWK_REAL];
}

/* The raw fused opcode for a `name[i].field` read, store or compound: [access][narrow][unchecked][real]. */
static Opcode index_field_raw_op(FieldAccess access, bool narrow, bool unchecked, RawKind kind) {
    static const Opcode ops[3][2][2][2] = {
        {{{OP_INDEX_FIELD_GET_RAW_INT, OP_INDEX_FIELD_GET_RAW_REAL},
          {OP_INDEX_FIELD_GET_RAW_INT_UNCHECKED, OP_INDEX_FIELD_GET_RAW_REAL_UNCHECKED}},
         {{OP_INDEX_FIELD_GET_RAW_INT32, OP_INDEX_FIELD_GET_RAW_FLOAT32},
          {OP_INDEX_FIELD_GET_RAW_INT32_UNCHECKED, OP_INDEX_FIELD_GET_RAW_FLOAT32_UNCHECKED}}},
        {{{OP_INDEX_FIELD_SET_RAW_INT, OP_INDEX_FIELD_SET_RAW_REAL},
          {OP_INDEX_FIELD_SET_RAW_INT_UNCHECKED, OP_INDEX_FIELD_SET_RAW_REAL_UNCHECKED}},
         {{OP_INDEX_FIELD_SET_RAW_INT32, OP_INDEX_FIELD_SET_RAW_FLOAT32},
          {OP_INDEX_FIELD_SET_RAW_INT32_UNCHECKED, OP_INDEX_FIELD_SET_RAW_FLOAT32_UNCHECKED}}},
        {{{OP_INDEX_FIELD_COMPOUND_RAW_INT, OP_INDEX_FIELD_COMPOUND_RAW_REAL},
          {OP_INDEX_FIELD_COMPOUND_RAW_INT_UNCHECKED, OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED}},
         {{OP_INDEX_FIELD_COMPOUND_RAW_INT32, OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32},
          {OP_INDEX_FIELD_COMPOUND_RAW_INT32_UNCHECKED, OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED}}},
    };
    return ops[access][narrow][unchecked][kind == RAWK_REAL];
}

/* `expr[index].field` as one fused read -- a packed array has no standalone `expr[index]` value. A
   field the array's known shape types as int/real reads straight into a raw slot. False once an
   error is reported, leaving *out_rk untouched. */
static bool emit_index_field_get(Chunk* c, int arr_reg, int rk_start, unsigned int field_idx, int* out_rk) {
    release_if_top(rk_start);
    release_if_top(arr_reg);
    if (!rk16_fits(rk_start)) {
        error_at("Expression too large to compile (register/constant index exceeds the fused "
                 "index-field-op encoding's range)");
        return false;
    }
    mark_shape_sensitive(arr_reg);
    unsigned int foffset = 0;
    bool narrow = false;
    RawKind kind = known_field_kind(arr_reg, field_idx, &foffset, &narrow);
    if (kind != RAWK_NONE) {
        int slot = slot_alloc(kind);
        if (slot >= 0) {
            /* Checked here, not in index_safe_unchecked, which typed-array reads share: the field
               offset is valid only for the one specialized parameter, never another array a loop
               also proved an index safe for. */
            bool unchecked = arr_reg == P.proof.hint_param_reg && index_safe_unchecked(arr_reg, rk_start);
            Opcode op = index_field_raw_op(FIELD_GET, narrow, unchecked, kind);
            chunk_emit(c, PACK3(op, slot, arr_reg, 0));
            chunk_emit(c, PACK_2X16((uint16_t)foffset, pack_rk16(rk_start)));
            *out_rk = (kind == RAWK_INT ? RK_RAW_INT_FLAG : RK_RAW_REAL_FLAG) | slot;
            return true;
        }
    }
    int dest = reg_alloc();
    chunk_emit(c, PACK3(OP_INDEX_FIELD_GET, dest, arr_reg, 0));
    chunk_emit(c, PACK_2X16(field_idx, pack_rk16(rk_start)));
    *out_rk = dest;
    return true;
}

/* `expr[index]`: into a raw slot when the array's element kind is known, unchecked once a loop proved
   the index. */
static int emit_index_read(Chunk* c, int arr_reg, int rk_start) {
    /* Free-then-allocate, matching parse_binary_ops's own discipline. */
    release_if_top(rk_start);
    release_if_top(arr_reg);

    RawKind elem = (arr_reg >= 0 && arr_reg < FRAME_REGISTERS) ? P.regs.reg_elem_kind[arr_reg] : RAWK_NONE;
    if (elem != RAWK_NONE && rk8_fits(rk_start)) {
        int slot = slot_alloc(elem);
        if (slot >= 0) {
            /* The kind here is not an inference: reg_elem_kind is set from watching the array get built
               and cleared by any write to that register, unlike try_rewrite_index_get_raw's guess from
               the consumer, which is what the checked opcode's fallback exists to catch (15 in 22.1M,
               all from there). */
            if (index_safe_unchecked(arr_reg, rk_start))
                chunk_emit(c, PACK3(OP_TYPED_INDEX_GET_UNCHECKED, slot, arr_reg, pack_rk8(rk_start)));
            else
                chunk_emit(c, PACK3(elem == RAWK_INT ? OP_INDEX_GET_RAW_INT : OP_INDEX_GET_RAW_REAL, slot,
                                    arr_reg, pack_rk8(rk_start)));
            return (elem == RAWK_INT ? RK_RAW_INT_FLAG : RK_RAW_REAL_FLAG) | slot;
        }
    }

    int dest = reg_alloc();
    /* rk_start is plain and non-raw here, so it fits RK8 directly. A loop-safety proof only, not a
       container-type one -- the opcode still checks TYPE_TYPED_ARRAY itself. */
    if (index_safe_unchecked(arr_reg, rk_start))
        chunk_emit(c, PACK3(OP_TYPED_INDEX_GET_UNCHECKED, dest, arr_reg, pack_rk8(rk_start)));
    else
        emit_index_get(c, dest, arr_reg, rk_start);
    /* One-hop alias tracking for SPEC_KIND_ARRAY_OF_STRUCTS. src_param needs only arr_reg's identity;
       elem_shape stays NULL outside a specialization recompile, the only place it is seeded. */
    P.index_alias.dest_reg = dest;
    P.index_alias.src_param = (arr_reg >= 0 && arr_reg < P.regs.current_param_count) ? arr_reg : -1;
    P.index_alias.elem_shape =
        (arr_reg >= 0 && arr_reg < FRAME_REGISTERS) ? P.regs.reg_known_element_shape[arr_reg] : NULL;
    return dest;
}

/* `expr.field`: into a raw slot when the struct's known shape types the field as int/real. */
static int emit_field_read(Chunk* c, int struct_reg, unsigned int field_idx) {
    release_if_top(struct_reg);
    mark_shape_sensitive(struct_reg);
    unsigned int foffset = 0;
    bool narrow = false;
    RawKind kind = known_field_kind(struct_reg, field_idx, &foffset, &narrow);
    if (kind != RAWK_NONE) {
        int slot = slot_alloc(kind);
        if (slot >= 0) {
            chunk_emit(c, PACK3(field_raw_op(FIELD_GET, narrow, kind), slot, struct_reg, 0));
            chunk_emit(c, foffset);
            return (kind == RAWK_INT ? RK_RAW_INT_FLAG : RK_RAW_REAL_FLAG) | slot;
        }
    }
    int dest = reg_alloc();
    emit_field_get(c, dest, struct_reg, field_idx);
    return dest;
}

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
                    if (!emit_index_field_get(c, arr_reg, rk_start, field_idx, &rk))
                        return rk;
                    continue;
                }
                rk = emit_index_read(c, arr_reg, rk_start);
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
            rk = emit_field_read(c, materialize(c, rk), field_idx);
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
   Only a comparison or boxed binary op, whose destination is always field A and whose only effect
   is that write, and only while last_instruction() still returns it. */
static bool retarget_last_cmp(Chunk* c, int reg_rhs, int dest) {
    unsigned int at = last_instruction(c);
    if (at == NO_OFFSET || P.peep.last.raw_kind != RAWK_NONE || (Opcode)(c->code[at] & 0xFF) == OP_INTERP)
        return false;
    if ((int)UNPACK_A(c->code[at]) != reg_rhs || !is_temp(reg_rhs))
        return false;
    /* Forgetting it matters here beyond bookkeeping: the retargeted comparison is the last word
       emitted, so an enclosing if/while would fuse its branch with it -- but it sits behind the
       short circuit and only runs when the left operand was truthy. */
    retarget_last(c, at, dest);
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
        emit_move(c, dest, reg_lhs);
    }
    unsigned int patch_skip = emit_jump_if_false_reg(c, dest); /* lhs falsy -- dest already holds it */

    int rk_rhs = parse_binary(c, prec);
    int reg_rhs = materialize(c, rk_rhs);
    if (reg_rhs != dest && !retarget_last_cmp(c, reg_rhs, dest))
        emit_move(c, dest, reg_rhs);
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
        emit_move(c, dest, reg_lhs);
    }
    unsigned int patch_use_rhs = emit_jump_if_false_reg(c, dest);
    chunk_emit(c, OP_JUMP);
    unsigned int patch_end = c->count;
    chunk_emit(c, 0);

    patch_jump(c, patch_use_rhs, c->count);
    int rk_rhs = parse_binary(c, prec);
    int reg_rhs = materialize(c, rk_rhs);
    if (reg_rhs != dest && !retarget_last_cmp(c, reg_rhs, dest))
        emit_move(c, dest, reg_rhs);
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

/* The piped value as argument zero, then the parenthesized arguments after it, contiguous. Returns the
   argument base, which is also the call's destination, or -1 once an error is reported. When `guarded`,
   *guard_patch is the short-circuit guard compile_pipe_guard_end closes. */
static int parse_pipe_args(Chunk* c, int lhs, bool guarded, int* arg_count, unsigned int* guard_patch) {
    release_if_top(lhs);
    int arg_reg_base = arg_materialize(c, lhs);
    *guard_patch = guarded ? compile_pipe_guard_begin(c, arg_reg_base) : 0;

    *arg_count = 1;
    if (!equal(TOKEN_CLOSE_PARENTHESE)) {
        do {
            int rk = parse_binary(c, 0);
            arg_materialize(c, rk);
            (*arg_count)++;
        } while (consume(TOKEN_COMMA));
    }
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after pipe call arguments");
    if (parse_had_error)
        return -1;
    if (*arg_count > 1)
        reg_free(*arg_count - 1);
    return arg_reg_base;
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

        int arg_count;
        unsigned int patch_skip_call;
        int dest = parse_pipe_args(c, lhs, true, &arg_count, &patch_skip_call);
        if (dest < 0)
            return lhs;
        emit_module_call(c, dest, dest, arg_count, module_idx, fn_idx, module_id, fn_id);
        compile_pipe_guard_end(c, patch_skip_call);
        return dest;
    }

    /* Captured before the name is consumed: pending_call_add reports at this cursor, and by the time
       it is called the argument list has been consumed too. */
    const char* call_site_cursor = current_source_cursor();
    unsigned int name_idx = chunk_add_pool(c, token.value);
    lex();

    if (equal(TOKEN_DOT)) {
        error_at("'%s' is not an imported module — add 'import %s' at the top level first",
                 aer_as_string(c->pool[name_idx])->data, aer_as_string(c->pool[name_idx])->data);
        return lhs;
    }

    bool is_struct = is_struct_name(name_idx);
    unsigned int func_offset = 0, func_index = 0;
    /* A name defined further down the file emits against a placeholder that parse() patches once it
       has seen the whole file, the same deferral a plain call gets -- `x |> f()` and `f(x)` resolve
       identically. A name never defined anywhere is reported by that sweep, not here. */
    bool is_forward_ref = !is_struct && !func_lookup(c, name_idx, &func_offset, &func_index);

    require(TOKEN_OPEN_PARENTHESE, "expected '(' after piped function name");
    if (parse_had_error)
        return lhs;

    int arg_count;
    unsigned int patch_skip_call;
    /* No reasonable meaning for short-circuiting a struct construction, so the guard is skipped. */
    int dest = parse_pipe_args(c, lhs, !is_struct, &arg_count, &patch_skip_call);
    if (dest < 0)
        return lhs;
    if (is_struct) {
        emit_struct_new(c, dest, name_idx, dest, arg_count);
    } else {
        unsigned int patch_offset = emit_call(c, dest, func_offset, dest, arg_count, func_index);
        if (is_forward_ref)
            pending_call_add(name_idx, patch_offset, call_site_cursor);
        compile_pipe_guard_end(c, patch_skip_call);
    }
    return dest;
}

/* Split from parse_binary so a for-while condition starting with an identifier can resolve it
   once and climb from there. */
typedef enum { FUSE_NONE, FUSE_DONE, FUSE_ERROR } FuseResult;

/* `t + a*b` / `t - a*b` where t is a fresh raw-real temporary and the rhs is exactly one raw MUL: one
   FMA/FMS writing t in place. Safe only for a temporary -- a named variable's slot must not be
   overwritten, since a plain expression's lhs may still be read with its original value. */
static bool try_fuse_fma_temp(Chunk* c, Opcode op, int* lhs, int rhs, unsigned int rhs_start) {
    if ((op != OP_ADD && op != OP_SUB) || rk_raw_kind(c, *lhs) != RAWK_REAL ||
        !raw_is_temp(*lhs & RK_RAW_SLOT_MASK) || c->count - rhs_start != 1)
        return false;
    uint32_t mw = c->code[rhs_start];
    if ((Opcode)(mw & 0xFF) != OP_RAW_MUL_REAL || rk_raw_kind(c, rhs) != RAWK_REAL ||
        (int)UNPACK_A(mw) != (rhs & RK_RAW_SLOT_MASK))
        return false;
    int lhs_slot = *lhs & RK_RAW_SLOT_MASK;
    int mul_a = (int)UNPACK_B(mw), mul_b = (int)UNPACK_C(mw);
    c->count = rhs_start; /* discard the MUL -- fused below instead */
    raw_release_if_top((int)UNPACK_A(mw));
    chunk_emit(c, PACK3(op == OP_ADD ? OP_RAW_FMA_REAL : OP_RAW_FMS_REAL, lhs_slot, mul_a, mul_b));
    *lhs = RK_RAW_REAL_FLAG | lhs_slot;
    return true;
}

/* The operator that gives `field OP' x` the value of `x OP field`: itself when commutative, the mirror
   comparison for an ordering. False for SUB/DIV/MOD/FLOOR_DIV/shifts/IN, which have no equivalent. */
static bool commuted_op(Opcode op, Opcode* out) {
    switch (op) {
        case OP_ADD:
        case OP_MUL:
        case OP_EQ:
        case OP_NEQ:
        case OP_BITWISE_AND:
        case OP_BITWISE_OR:
        case OP_BITWISE_XOR: *out = op; return true;
        case OP_LT: *out = OP_GT; return true;
        case OP_GT: *out = OP_LT; return true;
        case OP_LTE: *out = OP_GTE; return true;
        case OP_GTE: *out = OP_LTE; return true;
        default: return false;
    }
}

/* `x OP y.field`, when the rhs is exactly one bare field read: reuses OP_FIELD_BINARY (`field OP' x`)
   rather than a mirror opcode. */
static FuseResult try_fuse_field_rhs(Chunk* c, Opcode op, int* lhs, int rhs, unsigned int rhs_start) {
    Opcode field_op;
    if (c->count - rhs_start != 2 || (c->code[rhs_start] & 0xFF) != OP_FIELD_GET ||
        !commuted_op(op, &field_op))
        return FUSE_NONE;
    int struct_reg = (int)UNPACK_B(c->code[rhs_start]);
    unsigned int field_idx = c->code[rhs_start + 1];
    c->count = rhs_start; /* discard the OP_FIELD_GET just emitted, never executed */

    /* rhs is always the OP_FIELD_GET result (never raw); lhs could be raw -- box it. */
    *lhs = drop_raw_marks(*lhs);
    release_if_top(rhs);
    release_if_top(*lhs);

    int dest = reg_alloc();
    if (!rk16_fits(*lhs)) {
        error_at("Expression too large to compile (register/constant index exceeds the fused "
                 "field-op encoding's range)");
        return FUSE_ERROR;
    }
    chunk_emit(c, PACK3(OP_FIELD_BINARY, dest, struct_reg, field_op));
    chunk_emit(c, PACK_2X16(field_idx, pack_rk16(*lhs)));
    *lhs = dest;
    return FUSE_DONE;
}

/* The two LHS peepholes below read the LHS's just-emitted bytecode and, on a match, drop it so the
   operator can fuse it in. They run before the RHS is parsed, while that code is still the chunk's tail --
   excising bytecode from the middle could invalidate an RHS jump target. */

/* An LHS that is exactly one OP_FIELD_GET: its struct register and field, with the read discarded. */
static bool take_lhs_field_get(Chunk* c, unsigned int lhs_start, int* struct_reg, unsigned int* field_idx) {
    if (c->count - lhs_start != 2 || (c->code[lhs_start] & 0xFF) != OP_FIELD_GET)
        return false;
    *struct_reg = (int)UNPACK_B(c->code[lhs_start]);
    *field_idx = c->code[lhs_start + 1];
    c->count = lhs_start;
    return true;
}

/* An LHS that is exactly one `A op1 B` over plain registers, with op1 and the outer op both + - *: A, B and
   op1, with the inner op discarded, for one OP_TYPED_ARRAY_CHAIN2 pass (see vm.c's op_chain2_index). */
static bool take_lhs_chain2(Chunk* c, unsigned int lhs_start, int lhs, Opcode op, int* a, int* b,
                            Opcode* op1) {
    if (c->count - lhs_start != 1 || !(op == OP_ADD || op == OP_SUB || op == OP_MUL))
        return false;
    uint32_t w = c->code[lhs_start];
    Opcode wop = (Opcode)(w & 0xFF);
    if (!(wop == OP_ADD || wop == OP_SUB || wop == OP_MUL) || (int)UNPACK_A(w) != lhs)
        return false;
    uint8_t a8 = (uint8_t)UNPACK_B(w), b8 = (uint8_t)UNPACK_C(w);
    if (RK8_IS_CONST(a8) || RK8_IS_CONST(b8))
        return false;
    *op1 = wop;
    *a = RK8_INDEX(a8);
    *b = RK8_INDEX(b8);
    c->count = lhs_start;
    return true;
}

/* Elementwise + - * on a typed array yields a typed array of the same kind, and the result has to say so
   or the next operator in the chain sees an unproven register and unboxes it -- `(a - 1.0) * 2.0` failed
   on exactly that. Read before the operands' registers are freed, since dest may be handed one of them. */
static RawKind elementwise_result_kind(Opcode op, int lhs, int rhs) {
    if (!(op == OP_ADD || op == OP_SUB || op == OP_MUL))
        return RAWK_NONE;
    int lr = drop_raw_marks(lhs), rr = drop_raw_marks(rhs);
    RawKind kind = RAWK_NONE;
    if (!(lhs & RK_CONST_FLAG) && lr >= 0 && lr < FRAME_REGISTERS)
        kind = P.regs.reg_elem_kind[lr];
    if (kind == RAWK_NONE && !(rhs & RK_CONST_FLAG) && rr >= 0 && rr < FRAME_REGISTERS)
        kind = P.regs.reg_elem_kind[rr];
    return kind;
}

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

        int lhs_struct_reg = 0;
        unsigned int lhs_field_idx = 0;
        bool lhs_is_field = take_lhs_field_get(c, lhs_start, &lhs_struct_reg, &lhs_field_idx);
        int lhs_chain2_a = 0, lhs_chain2_b = 0;
        Opcode lhs_chain2_op1 = OP_ADD;
        bool lhs_is_chain2 = !lhs_is_field && take_lhs_chain2(c, lhs_start, lhs, op, &lhs_chain2_a,
                                                              &lhs_chain2_b, &lhs_chain2_op1);

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

        if (try_fuse_fma_temp(c, op, &lhs, rhs, rhs_start)) {
            lhs_start = c->count;
            continue;
        }
        FuseResult field_rhs = try_fuse_field_rhs(c, op, &lhs, rhs, rhs_start);
        if (field_rhs == FUSE_ERROR)
            return lhs;
        if (field_rhs == FUSE_DONE) {
            lhs_start = c->count;
            continue;
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

        RawKind chain_elem = elementwise_result_kind(op, lhs, rhs);

        /* Free-then-allocate, RHS then LHS, matching compile_node's own discipline exactly. */
        release_if_top(rhs);
        release_if_top(lhs);

        int dest = reg_alloc();
        emit_binary(c, dest, op, lhs, rhs);
        if (dest >= 0 && dest < FRAME_REGISTERS)
            P.regs.reg_elem_kind[dest] = chain_elem;
        lhs = dest;
        lhs_start = c->count;
    }
    return lhs;
}

static int parse_binary(Chunk* c, unsigned int min_prec) {
    if (P.expr_depth >= EXPR_DEPTH_MAX) {
        error_at("Expression nests too deeply (max %d)", EXPR_DEPTH_MAX);
        return 0;
    }
    P.expr_depth++;
    unsigned int lhs_start = c->count;
    int lhs = parse_unary(c);
    int rk = parse_binary_ops(c, min_prec, lhs, lhs_start);
    P.expr_depth--;
    return rk;
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

/* The compound_assign_ops index the next token opens, or -1. It only looks, so a dispatcher can
   choose a handler and the handler consumes the token. */
static int compound_assign_at(void) {
    for (int i = 0; i < COMPOUND_ASSIGN_OP_COUNT; i++)
        if (equal(compound_assign_ops[i].tok))
            return i;
    return -1;
}

/* `a, b = ...`. Targets resolve BEFORE the right-hand side is parsed -- creating a variable
   while a temp is live could hand out that temp's own register. */
static void parse_assign_destructuring(Chunk* c, unsigned int name_idx) {
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
        if (P.proof.length_tracked_valid) {
            for (unsigned int i = 0; i < count; i++) {
                if (names[i] == P.proof.length_tracked_name) {
                    P.proof.length_tracked_valid = false;
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
        for (unsigned int i = 0; i < count; i++)
            forget_register(target_regs[i]);

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
}
/* `x = ...`, including the raw-slot promotion a numeric value earns. */
/* Puts a raw value into dest_slot: re-points the arithmetic that produced it when that is still the
   last instruction, else a raw move. Frees src_slot if it was a temp. */
static void emit_raw_move(Chunk* c, int src_slot, int dest_slot, RawKind kind) {
    if (src_slot == dest_slot)
        return;
    if (!retarget_raw_write(c, src_slot, dest_slot, kind))
        chunk_emit(c, PACK3(kind == RAWK_INT ? OP_RAW_MOVE_INT : OP_RAW_MOVE_REAL, dest_slot, src_slot, 0));
    raw_release_if_top(src_slot);
}

/* `x = length(arr)` starts tracking x as arr's length; any other assignment to a tracked x stops it.
   Keyed on the final RHS register -- `x = length(p) + 1` lands in the ADD's register, not length()'s --
   the same self-correcting equality index_alias.dest_reg uses. */
static void note_length_assignment(unsigned int name_idx, int rk_val) {
    if (P.proof.last_length_call_result_reg >= 0 && rk_val == P.proof.last_length_call_result_reg) {
        P.proof.length_tracked_name = name_idx;
        P.proof.length_tracked_valid = true;
        P.proof.length_tracked_source_reg = P.proof.last_length_call_arg_reg;
    } else if (P.proof.length_tracked_valid && name_idx == P.proof.length_tracked_name) {
        P.proof.length_tracked_valid = false;
    }
}

/* A new name's first assignment: a raw slot when outside any if/else branch and the RHS is provably
   int/real. True once the assignment is done or an error reported; false leaves it to be boxed. */
static bool try_declare_raw_variable(Chunk* c, unsigned int name_idx, int rk_val) {
    /* Not gated on function_depth, so top-level qualifies: frame 0's registers are linked at full
       capacity for the VM's life and aer_vm_reset_for_reuse never touches them. */
    RawKind rhs_kind = rk_raw_kind(c, rk_val);
    if (P.branch_depth != 0 || rhs_kind == RAWK_NONE)
        return false;
    int slot = slot_reserve_one(rhs_kind);
    if (slot < 0)
        return false;
    int src_slot = raw_materialize(c, rk_val, rhs_kind);
    if (src_slot < 0) {
        raw_unreserve_one(rhs_kind); /* budget exhausted mid-materialize: fall through to boxed */
        return false;
    }
    emit_raw_move(c, src_slot, slot, rhs_kind);
    P.var_names[P.var_count] = name_idx;
    P.var_regs[P.var_count] = slot;
    P.regs.var_kind[P.var_count] = (rhs_kind == RAWK_INT) ? VAR_RAW_INT : VAR_RAW_REAL;
    P.var_count++;
    /* Mirrors var_slot's own registration, which this path bypasses: without it a top-level int/real is
       invisible to the shadow ban. */
    if (P.function_depth == 0 && P.global_count < FRAME_REGISTERS) {
        P.global_names[P.global_count] = name_idx;
        P.global_regs[P.global_count] = slot;
        P.global_count++;
    }
    return true;
}

/* Reassigning a raw variable: in place when the new value is the same raw kind, else shadowed by a
   fresh boxed register. var_slot must never be called here -- it would return the raw slot as if it
   were a plain register. */
static void assign_raw_variable(Chunk* c, unsigned int name_idx, int existing_idx, int rk_val) {
    RawKind rhs_kind = rk_raw_kind(c, rk_val);
    VarKind cur = P.regs.var_kind[existing_idx];
    if ((cur == VAR_RAW_INT && rhs_kind == RAWK_INT) || (cur == VAR_RAW_REAL && rhs_kind == RAWK_REAL)) {
        int dest_slot = P.var_regs[existing_idx];
        /* A typed write still ends any loop proof or tracked length keyed on this register. */
        note_slot_written(dest_slot);
        int src_slot = raw_materialize(c, rk_val, rhs_kind);
        if (src_slot >= 0) {
            emit_raw_move(c, src_slot, dest_slot, rhs_kind);
            return;
        }
        /* Budget exhausted materializing the RHS -- fall through to the shadow path. */
    }
    /* A looped self-referential shadow (`total = total + x`) must refuse to compile: the RHS already
       emitted reads the old raw slot, which nothing writes anymore. Gated on self_ref_watch_seen,
       since a reassignment that does not read the old value has nothing stale to re-read. */
    if (P.loop_depth > 0 && P.self_ref_watch_seen) {
        error_at("This assignment would change '%s' from a fixed numeric type to a different type, "
                 "but it's inside a loop — not supported. If you're accumulating with +, -, or *, "
                 "use the compound form ('%s += ...' etc.) instead — it doesn't have this "
                 "restriction. Otherwise, restructure so the type change happens outside any loop.",
                 aer_as_string(c->pool[name_idx])->data, aer_as_string(c->pool[name_idx])->data);
        return;
    }
    rk_val = drop_raw_marks(rk_val);
    if (P.slot_floor >= P.raw_real_next) {
        error_at("Too many variables (max %d)", FRAME_REGISTERS);
        return;
    }
    int new_reg = P.slot_floor;
    P.slot_floor++;
    P.slot_next = P.slot_floor;
    P.var_regs[existing_idx] = new_reg;
    P.regs.var_kind[existing_idx] = VAR_BOXED;
    /* No global_regs update: a raw-tracked name is never in global_names (see ensure_boxed). */
    if (rk_val & RK_CONST_FLAG) {
        emit_loadk(c, new_reg, (unsigned int)(rk_val & ~RK_CONST_FLAG));
    } else if (new_reg != rk_val) {
        emit_move(c, new_reg, rk_val);
        release_if_top(rk_val);
    }
}

/* An ordinary boxed assignment, through var_slot. */
static void assign_boxed_variable(Chunk* c, unsigned int name_idx, int rk_val) {
    rk_val = drop_raw_marks(rk_val);
    int reg = var_slot(c, name_idx);
    if (reg < 0)
        return; /* error_at already called */
    /* Read before invalidate_register wipes it: a `[numeric; count]` literal is usually built straight
       into the register var_slot hands this name, so source and destination are the same one. */
    bool rhs_plain_reg = !(rk_val & (RK_CONST_FLAG | RK_RAW_INT_FLAG | RK_RAW_REAL_FLAG)) && rk_val >= 0 &&
                         rk_val < FRAME_REGISTERS;
    RawKind rhs_elem = rhs_plain_reg ? P.regs.reg_elem_kind[rk_val] : RAWK_NONE;
    /* Without this, `reg` staying on safe_loop_item_regs would let a later arr[reg].field in the same
       loop body trust an index register that may no longer hold what the loop put there. */
    invalidate_register(reg);
    /* Only parse_function_body's initial seed deserves a surviving element-shape fact; keeping one here
       lets a reassigned array-of-structs parameter resolve a stale field offset -- type confusion. */
    P.regs.reg_known_element_shape[reg] = NULL;
    /* A reassigned shape-sensitive name keeps no shape hint, unless the RHS was exactly
       `some_param[idx]` -- the one-hop alias SPEC_KIND_ARRAY_OF_STRUCTS needs -- whose element shape
       propagates onto it. Consumed at once, so a freed-and-reused temp cannot match later. */
    if (rk_val == P.index_alias.dest_reg && P.index_alias.dest_reg >= 0) {
        P.regs.reg_known_shape[reg] = P.index_alias.elem_shape;
        P.regs.alias_source_param[reg] = P.index_alias.src_param;
    } else {
        P.regs.reg_known_shape[reg] = NULL;
        P.regs.alias_source_param[reg] = -1;
    }
    P.index_alias.dest_reg = -1;
    P.regs.reg_elem_kind[reg] = rhs_elem;
    if (rk_val & RK_CONST_FLAG) {
        emit_loadk(c, reg, (unsigned int)(rk_val & ~RK_CONST_FLAG));
    } else if (reg != rk_val) {
        emit_move(c, reg, rk_val);
        /* After var_slot, which may have raised the floor, so rk_val is correctly no longer a temp. */
        release_if_top(rk_val);
    }
}

static void parse_assign_plain(Chunk* c, unsigned int name_idx) {
    if (!consume(TOKEN_ASSIGN))
        return;
    /* Watches for a self-reference during the RHS parse -- see self_ref_watch_name (top of file). */
    P.self_ref_watch_name = name_idx;
    P.self_ref_watch_seen = false;
    int rk_val = parse_binary(c, 0);
    if (parse_had_error)
        return;
    note_length_assignment(name_idx, rk_val);

    /* Looked up after the RHS: a self-referential first assignment creates the name while parsing it,
       always boxed, which folds that case into the ordinary path. */
    int existing_idx = -1;
    for (int i = 0; i < P.var_count; i++)
        if (P.var_names[i] == name_idx) {
            existing_idx = i;
            break;
        }
    if (existing_idx < 0 && try_declare_raw_variable(c, name_idx, rk_val))
        return;
    if (existing_idx >= 0 && P.regs.var_kind[existing_idx] != VAR_BOXED) {
        assign_raw_variable(c, name_idx, existing_idx, rk_val);
        return;
    }
    assign_boxed_variable(c, name_idx, rk_val);
}
/* `x += ...` and friends. Answers whether it matched an operator, since a bare name followed
   by something else is still a legal statement. */
/* `x op= rhs` on a raw variable: raw arithmetic in place when the RHS is the same kind or can be
   unboxed into it; otherwise -- /= always, since int/int division promotes to real -- the variable
   becomes boxed. */
static void compound_assign_raw_variable(Chunk* c, unsigned int name_idx, int existing_idx,
                                         Opcode boxed_op) {
    /* Once for every branch below, all of which write this slot. */
    note_slot_written(P.var_regs[existing_idx]);
    RawKind cur_kind = (P.regs.var_kind[existing_idx] == VAR_RAW_INT) ? RAWK_INT : RAWK_REAL;
    bool native_op_exists = (boxed_op == OP_ADD || boxed_op == OP_SUB || boxed_op == OP_MUL);

    unsigned int rhs_start = c->count;
    int rk_rhs = parse_binary(c, 0);
    if (parse_had_error)
        return;
    RawKind rhs_kind = rk_raw_kind(c, rk_rhs);
    /* The target's kind is the wanted kind, so `total += nums[i]` reads the element raw rather than
       boxing it for a tag check one opcode later. */
    if (rhs_kind == RAWK_NONE && try_rewrite_index_get_raw(c, &rk_rhs, rhs_start, cur_kind))
        rhs_kind = cur_kind;
    int slot = P.var_regs[existing_idx];

    /* The RHS's just-emitted raw MUL and this ADD/SUB become one FMA/FMS writing the variable. */
    if (cur_kind == RAWK_REAL && (boxed_op == OP_ADD || boxed_op == OP_SUB) && rhs_kind == RAWK_REAL &&
        c->count - rhs_start == 1) {
        uint32_t mw = c->code[rhs_start];
        if ((Opcode)(mw & 0xFF) == OP_RAW_MUL_REAL && (int)UNPACK_A(mw) == (rk_rhs & RK_RAW_SLOT_MASK)) {
            c->count = rhs_start; /* discard the MUL -- fused below instead */
            raw_release_if_top((int)UNPACK_A(mw));
            Opcode fused = (boxed_op == OP_ADD) ? OP_RAW_FMA_REAL : OP_RAW_FMS_REAL;
            chunk_emit(c, PACK3(fused, slot, (int)UNPACK_B(mw), (int)UNPACK_C(mw)));
            return;
        }
    }

    if (native_op_exists) {
        const RawOpForm* form = raw_op_form(boxed_op);
        Opcode raw_op = (cur_kind == RAWK_INT) ? form->raw_int : form->raw_real;
        if (rhs_kind == cur_kind) {
            int rhs_slot = raw_materialize(c, rk_rhs, rhs_kind);
            if (rhs_slot >= 0) {
                chunk_emit(c, PACK3(raw_op, slot, slot, rhs_slot));
                raw_release_if_top(rhs_slot);
                return;
            }
            /* Budget exhausted materializing the RHS: box below rather than leave it half-updated. */
        } else if (rhs_kind == RAWK_NONE) {
            /* An ordinary boxed RHS (nbody's `e += 0.5 * bim * (...)`) is checked once into a raw
               slot and accumulated with raw arithmetic -- no shadow, safe every loop iteration. */
            int boxed_reg = materialize(c, rk_rhs);
            int tmp = slot_alloc(cur_kind);
            if (tmp >= 0) {
                Opcode unbox = (cur_kind == RAWK_INT) ? OP_UNBOX_INT : OP_UNBOX_REAL;
                chunk_emit(c, PACK3(unbox, tmp, boxed_reg, 0));
                chunk_emit(c, PACK3(raw_op, slot, slot, tmp));
                raw_release_if_top(tmp);
                release_if_top(boxed_reg);
                return;
            }
            release_if_top(boxed_reg);
        }
    }

    /* Boxing the variable here depends on its old value, so a loop re-executing it would re-read a
       stale slot every iteration. No single-pass fix exists: refuse to compile rather than corrupt. */
    if (P.loop_depth > 0) {
        error_at("This compound assignment would change '%s' from a fixed numeric type to a "
                 "different type, but it's inside a loop — not supported (restructure so the type "
                 "change happens outside any loop)",
                 aer_as_string(c->pool[name_idx])->data);
        return;
    }
    /* A real reads its old slot but writes a fresh register -- a real-block slot must never take a
       dynamically typed write (see ensure_boxed). The old slot is correctly tagged either way. */
    int new_reg = slot;
    if (cur_kind == RAWK_REAL) {
        if (P.slot_floor >= P.raw_real_next) {
            error_at("Too many variables (max %d)", FRAME_REGISTERS);
            return;
        }
        new_reg = P.slot_floor++;
        P.slot_next = P.slot_floor;
        track_peak(P.slot_floor);
        P.var_regs[existing_idx] = new_reg;
    }
    P.regs.var_kind[existing_idx] = VAR_BOXED;
    /* No global_regs update: a raw-tracked name is never in global_names (see ensure_boxed). */
    rk_rhs = drop_raw_marks(rk_rhs);
    emit_binary(c, new_reg, boxed_op, slot, rk_rhs);
    release_if_top(rk_rhs);
}

static void parse_assign_compound(Chunk* c, unsigned int name_idx) {
    Opcode op = compound_assign_ops[compound_assign_at()].op;
    lex();

    /* The RHS combines with the old value, never a fresh length() alone, so a tracked length ends. */
    if (P.proof.length_tracked_valid && name_idx == P.proof.length_tracked_name)
        P.proof.length_tracked_valid = false;

    /* A raw target needs its own path: var_lookup would return its slot as a bare register number,
       silently misread as a plain register. */
    int existing_idx = -1;
    for (int j = 0; j < P.var_count; j++)
        if (P.var_names[j] == name_idx) {
            existing_idx = j;
            break;
        }
    if (existing_idx >= 0 && P.regs.var_kind[existing_idx] != VAR_BOXED) {
        compound_assign_raw_variable(c, name_idx, existing_idx, op);
        return;
    }

    /* An undefined name is a compile error; parse_assignment has already reported a top-level one. */
    int reg;
    if (!var_lookup(name_idx, &reg)) {
        error_at("Compound assignment target must already have a value (no assigning to an undefined "
                 "name this way)");
        return;
    }

    int rk_rhs = parse_binary(c, 0);
    if (parse_had_error)
        return;
    /* `bodies += extra_bodies` changes what reg holds, so nothing known about it survives. */
    forget_register(reg);
    emit_binary(c, reg, op, reg, rk_rhs);
    release_if_top(rk_rhs);
}
/* A pipe chain from a bare name used as a statement -- a lookup that never creates. */
static void parse_assign_pipe(Chunk* c, unsigned int name_idx) {
    int reg;
    unsigned int lhs_start = c->count;
    if (!var_lookup_rk(name_idx, &reg))
        return error_at("'%s' is not defined (a pipe chain's source must already have a value)",
                        aer_as_string(c->pool[name_idx])->data);
    int rk_result = parse_binary_ops(c, 0, reg, lhs_start);
    discard_statement_result(c, rk_result);
}

/* A statement starting with a bare name, already consumed; the caller chose this over a call or an
   indexed/field write by one token of lookahead. */
static void parse_assignment(Chunk* c, unsigned int name_idx) {
    /* Checked here rather than in var_slot: the raw-promotion path below registers a name itself and
       never calls var_slot, so `integer = 5` would slip through. A reserved name always resolves to
       its builtin at the call site, so binding one made a variable nothing could ever read. */
    if (is_builtin_name(c, name_idx)) {
        return error_at("'%s' is a reserved function name and can't be used as a variable",
                        aer_as_string(c->pool[name_idx])->data);
    }
    /* Before the right-hand side is read, while the name is still on this line. */
    int existing_reg;
    if (!var_lookup(name_idx, &existing_reg) && report_if_shadowed_global(c, name_idx))
        return;
    /* Multiple RHS values pack into a real array (OP_ARRAY_NEW); each target reads its own index
       back via OP_INDEX_GET. Targets resolve via var_slot BEFORE the RHS is parsed -- creating a
       variable after a temp is live could hand out that temp's own register. */
    if (equal(TOKEN_COMMA))
        return parse_assign_destructuring(c, name_idx);
    if (equal(TOKEN_ASSIGN))
        return parse_assign_plain(c, name_idx);
    if (compound_assign_at() >= 0)
        return parse_assign_compound(c, name_idx);
    if (equal(TOKEN_PIPE))
        return parse_assign_pipe(c, name_idx);

    error_at("Only plain 'name = expr' or compound assignment is supported here (no field access)");
}

/* Every step but the last resolves as a GET; the pending step becomes the final SET (or a
   fused read-modify-write). Every GET after the first overwrites its own source register in
   place instead of allocating fresh -- reg_free is a pure LIFO decrement, so an older temp can
   never free while a newer one must stay live. Only the first hop and a non-temp leading index
   still need a genuinely fresh register. */
/* Where a chain like `a[i].f[j]` has got to: the register holding the object so far, whether
   that is still the variable's own, and the one step not yet resolved. The walk mutates all of
   this; a statement form that ENDS the chain only reads it. */
typedef struct {
    int obj_reg;
    bool obj_is_base;
    bool pending_is_field;
    unsigned int pending_field_idx;
    int pending_rk_idx;
} ChainTarget;

/* `a[i].f = value` -- the chain's last step becomes a SET. */
static void parse_chain_store(Chunk* c, ChainTarget t) {
    int obj_reg = t.obj_reg;
    bool obj_is_base = t.obj_is_base;
    bool pending_is_field = t.pending_is_field;
    unsigned int pending_field_idx = t.pending_field_idx;
    int pending_rk_idx = t.pending_rk_idx;
    if (consume(TOKEN_ASSIGN)) {
        int rk_val = parse_binary(c, 0);
        if (parse_had_error)
            return;

        /* Only a direct `param.field = ...` (obj_is_base, no preceding chain step) marks the
           parameter shape-sensitive -- an intermediate chain register (`a.b.c = ...`) isn't a
           parameter itself, and its own provenance isn't tracked (only the one-hop alias case,
           `local = param[idx]`, is -- see P.regs.alias_source_param). */
        if (pending_is_field && obj_is_base) {
            mark_shape_sensitive(obj_reg);
            unsigned int foffset = 0;
            bool field_narrow_bit = false;
            RawKind field_kind = known_field_kind(obj_reg, pending_field_idx, &foffset, &field_narrow_bit);
            if (field_kind != RAWK_NONE && rk_raw_kind(c, rk_val) == field_kind) {
                int slot = raw_materialize(c, rk_val, field_kind);
                if (slot >= 0) {
                    Opcode op = field_raw_op(FIELD_SET, field_narrow_bit, field_kind);
                    chunk_emit(c, PACK3(op, obj_reg, 0, 0));
                    chunk_emit(c, foffset);
                    chunk_emit(c, (uint32_t)slot);
                    raw_release_if_top(slot);
                    return; /* obj_is_base is always true here, so no reg_free(1) for it needed */
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
}
/* `a[i].f += value` -- read-modify-write through the same fusion opcodes the plain
   store uses, so there is no dedicated compound opcode per shape. */
/* `obj.field op= rhs`. True when parse_chain_compound's shared tail should run -- false after an error
   or the FMA fuse, which both skip it. */
static bool compound_assign_field(Chunk* c, ChainTarget t, Opcode bin_op) {
    if (t.obj_is_base)
        mark_shape_sensitive(t.obj_reg);

    /* Only a base register -- the shape-sensitive parameter or its alias -- has a tracked shape; an
       intermediate chain link never does. */
    unsigned int foffset = 0;
    bool field_narrow_bit = false;
    RawKind field_kind =
        known_field_kind(t.obj_is_base ? t.obj_reg : -1, t.pending_field_idx, &foffset, &field_narrow_bit);

    unsigned int rhs_start = c->count;
    int rk_rhs = parse_binary(c, 0);
    if (parse_had_error)
        return false;

    int mul_a, mul_b;
    if (bin_op == OP_ADD && field_kind == RAWK_REAL && field_narrow_bit &&
        rk_raw_kind(c, rk_rhs) == RAWK_REAL && take_fused_mul_real(c, rhs_start, rk_rhs, &mul_a, &mul_b)) {
        chunk_emit(c, PACK3(OP_FIELD_COMPOUND_RAW_FLOAT32_FMA, t.obj_reg, mul_a, mul_b));
        chunk_emit(c, foffset);
        return false; /* obj_is_base is always true here, so the tail has nothing to release */
    }
    /* Decomposing into raw GET + arithmetic + SET measured worse whenever the rhs needed boxing -- the
       fused opcode below does it all in one dispatch. An rhs that is already raw has nothing to box. */
    bool native_op_exists = (bin_op == OP_ADD || bin_op == OP_SUB || bin_op == OP_MUL);
    if (native_op_exists && field_kind != RAWK_NONE && rk_raw_kind(c, rk_rhs) == field_kind) {
        int slot = raw_materialize(c, rk_rhs, field_kind);
        if (slot >= 0) {
            Opcode op = field_raw_op(FIELD_COMPOUND, field_narrow_bit, field_kind);
            chunk_emit(c, PACK3(compound_add_variant(op, bin_op), t.obj_reg, bin_op, 0));
            chunk_emit(c, foffset);
            chunk_emit(c, (uint32_t)slot);
            raw_release_if_top(slot);
            return true;
        }
    }
    /* One fused OP_FIELD_COMPOUND -- read, compute and write back in a single dispatch and a single
       vm_resolve_field call, with no temp register: the result goes straight back into the field. */
    rk_rhs = drop_raw_marks(rk_rhs);
    if (!rk16_fits(rk_rhs)) {
        error_at("Expression too large to compile (register/constant index exceeds the fused field-op "
                 "encoding's range)");
        return false;
    }
    chunk_emit(c, PACK3(OP_FIELD_COMPOUND, t.obj_reg, bin_op, 0));
    chunk_emit(c, PACK_2X16(t.pending_field_idx, pack_rk16(rk_rhs)));
    release_if_top(rk_rhs);
    return true;
}

static void parse_chain_compound(Chunk* c, ChainTarget t) {
    Opcode op = compound_assign_ops[compound_assign_at()].op;
    lex();

    if (t.pending_is_field) {
        if (!compound_assign_field(c, t, op))
            return;
    } else {
        int item_reg = reg_alloc();
        emit_index_get(c, item_reg, t.obj_reg, t.pending_rk_idx);

        int rk_rhs = parse_binary(c, 0);
        if (parse_had_error)
            return;

        emit_binary(c, item_reg, op, item_reg, rk_rhs);
        release_if_top(rk_rhs);

        emit_index_set(c, t.obj_reg, t.pending_rk_idx, item_reg);
        reg_free(1); /* item_reg */
    }

    release_if_top(t.pending_rk_idx);
    if (!t.obj_is_base)
        reg_free(1);
}

/* One hop along an assignment target's chain: reads the pending index/field step into a register,
   which becomes the base the next step reads from. Claims a fresh register only on the first hop --
   after that the chain's own running temp is dead and gets reused in place, as does a dying index
   temp. */
static void chain_advance(Chunk* c, int* obj_reg, bool* obj_is_base, bool pending_is_field,
                          unsigned int pending_field_idx, int pending_rk_idx) {
    bool reuse_pending_idx_reg = !pending_is_field && is_temp(pending_rk_idx);
    int dest_reg;
    if (!*obj_is_base)
        dest_reg = *obj_reg;
    else if (reuse_pending_idx_reg)
        dest_reg = pending_rk_idx;
    else
        dest_reg = reg_alloc();

    if (pending_is_field)
        emit_field_get(c, dest_reg, *obj_reg, pending_field_idx);
    else
        emit_index_get(c, dest_reg, *obj_reg, pending_rk_idx);

    /* Only free the index temp when it is a different register than dest_reg -- otherwise it was
       already folded in. */
    if (reuse_pending_idx_reg && dest_reg != pending_rk_idx)
        reg_free(1);

    *obj_reg = dest_reg;
    *obj_is_base = false;
}

/* Reads one `[index]` or `.name` step of an assignment target after its opening token, into the pending
   fields. False once an error is reported. The index is boxed unconditionally: the fused opcodes pack it
   into a 16-bit RK slot with no raw state, and rk16_fits/rk8_fits mask only RK_CONST_FLAG, so a
   raw-flagged index would corrupt the encoding (surfacing as a bogus "expression too large"). */
static bool parse_chain_step(Chunk* c, bool is_index, bool* pending_is_field,
                             unsigned int* pending_field_idx, int* pending_rk_idx) {
    *pending_is_field = !is_index;
    if (is_index) {
        *pending_rk_idx = drop_raw_marks(parse_binary(c, 0));
        require(TOKEN_CLOSE_BRACKET, "expected ']' after index");
    } else {
        if (!equal(TOKEN_IDENTIFIER)) {
            error_at("Expected field name after '.'");
            return false;
        }
        *pending_field_idx = chunk_add_pool(c, token.value);
        lex();
    }
    return !parse_had_error;
}

/* `name[index].field = value` or `name[index].field op= value` as one fused opcode, when nothing
   chains after the field. obj_reg is still the name's own register. False once an error is
   reported; the caller releases the index and object only on success. */
static bool emit_index_field_assign(Chunk* c, int obj_reg, int rk_idx, unsigned int field_idx,
                                    int compound_i) {
    if (!rk16_fits(rk_idx)) {
        error_at("Expression too large to compile (register/constant index exceeds the fused "
                 "index-field-op encoding's range)");
        return false;
    }
    mark_shape_sensitive(obj_reg);
    unsigned int foffset = 0;
    bool narrow = false;
    RawKind field_kind = known_field_kind(obj_reg, field_idx, &foffset, &narrow);

    lex();
    unsigned int rhs_start = c->count;
    int rk_rhs = parse_binary(c, 0);
    if (parse_had_error)
        return false;
    /* The field offset these raw opcodes trust is valid only for the one specialized parameter. */
    bool unchecked = obj_reg == P.proof.hint_param_reg && index_safe_unchecked(obj_reg, rk_idx);

    if (compound_i < 0) {
        if (field_kind != RAWK_NONE && rk_raw_kind(c, rk_rhs) == field_kind) {
            int slot = raw_materialize(c, rk_rhs, field_kind);
            if (slot >= 0) {
                Opcode op = index_field_raw_op(FIELD_SET, narrow, unchecked, field_kind);
                chunk_emit(c, PACK_OP_A_W16(op, obj_reg, pack_rk16(rk_idx)));
                chunk_emit(c, PACK_2X16((uint16_t)foffset, (uint16_t)slot));
                raw_release_if_top(slot);
                return true;
            }
        }
        rk_rhs = drop_raw_marks(rk_rhs);
        if (!rk16_fits(rk_rhs)) {
            error_at("Expression too large to compile (value exceeds the fused index-field-set "
                     "encoding's range)");
            return false;
        }
        chunk_emit(c, PACK_OP_A_W16(OP_INDEX_FIELD_SET, obj_reg, pack_rk16(rk_idx)));
        chunk_emit(c, PACK_2X16(field_idx, pack_rk16(rk_rhs)));
        release_if_top(rk_rhs);
        return true;
    }

    Opcode bin_op = compound_assign_ops[compound_i].op;
    int mul_a, mul_b;
    if (bin_op == OP_ADD && field_kind == RAWK_REAL && !narrow && unchecked &&
        rk_raw_kind(c, rk_rhs) == RAWK_REAL && take_fused_mul_real(c, rhs_start, rk_rhs, &mul_a, &mul_b)) {
        chunk_emit(c, PACK3(OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED_FMA, obj_reg, mul_a, mul_b));
        chunk_emit(c, PACK_2X16((uint16_t)foffset, pack_rk16(rk_idx)));
        return true;
    }
    /* Decomposing into raw GET + arithmetic + SET measured worse whenever the rhs needed boxing -- the
       generic opcode already does it all in one dispatch. An rhs that is already raw has nothing to box. */
    bool native_op_exists = (bin_op == OP_ADD || bin_op == OP_SUB || bin_op == OP_MUL);
    if (native_op_exists && field_kind != RAWK_NONE && rk_raw_kind(c, rk_rhs) == field_kind) {
        int slot = raw_materialize(c, rk_rhs, field_kind);
        if (slot >= 0) {
            Opcode op = index_field_raw_op(FIELD_COMPOUND, narrow, unchecked, field_kind);
            chunk_emit(c, PACK3(compound_add_variant(op, bin_op), obj_reg, bin_op, 0));
            chunk_emit(c, PACK_2X16((uint16_t)foffset, pack_rk16(rk_idx)));
            chunk_emit(c, (uint32_t)slot);
            raw_release_if_top(slot);
            return true;
        }
    }
    rk_rhs = drop_raw_marks(rk_rhs);
    if (!rk16_fits(rk_rhs)) {
        error_at("Expression too large to compile (value exceeds the fused index-field-compound "
                 "encoding's range)");
        return false;
    }
    chunk_emit(c, PACK3(OP_INDEX_FIELD_COMPOUND, obj_reg, bin_op, 0));
    chunk_emit(c, PACK_2X16(field_idx, pack_rk16(rk_idx)));
    chunk_emit(c, PACK_2X16(0, pack_rk16(rk_rhs)));
    release_if_top(rk_rhs);
    return true;
}

static void parse_chain_assignment(Chunk* c, unsigned int name_idx, bool first_is_index) {
    int obj_reg;
    bool obj_is_base; /* true while obj_reg is still name_idx's own permanent register */
    if (var_lookup(name_idx, &obj_reg)) {
        obj_is_base = true;
    } else if (report_if_shadowed_global(c, name_idx)) {
        return;
    } else {
        return error_at("'%s' is not defined (an indexed/field write target must already have a value)",
                        aer_as_string(c->pool[name_idx])->data);
    }

    bool pending_is_field;
    unsigned int pending_field_idx = 0;
    int pending_rk_idx = 0;
    if (!parse_chain_step(c, first_is_index, &pending_is_field, &pending_field_idx, &pending_rk_idx))
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
        int compound_i = is_plain_assign ? -1 : compound_assign_at();

        if (no_more_chaining && (is_plain_assign || compound_i >= 0)) {
            if (emit_index_field_assign(c, obj_reg, pending_rk_idx, fused_field_idx, compound_i)) {
                release_if_top(pending_rk_idx);
                if (!obj_is_base)
                    reg_free(1);
            }
            return;
        }

        /* Replicates what the general loop's first iteration would do for a pending index step
           immediately followed by '.field', then falls through to the same general machinery. */
        chain_advance(c, &obj_reg, &obj_is_base, false, 0, pending_rk_idx);
        pending_is_field = true;
        pending_field_idx = fused_field_idx;
    }

    while (equal(TOKEN_OPEN_BRACKET) || equal(TOKEN_DOT)) {
        chain_advance(c, &obj_reg, &obj_is_base, pending_is_field, pending_field_idx, pending_rk_idx);

        bool is_index = consume(TOKEN_OPEN_BRACKET);
        if (!is_index)
            consume(TOKEN_DOT);
        if (!parse_chain_step(c, is_index, &pending_is_field, &pending_field_idx, &pending_rk_idx))
            return;
    }

    ChainTarget target = {obj_reg, obj_is_base, pending_is_field, pending_field_idx, pending_rk_idx};
    if (equal(TOKEN_ASSIGN))
        return parse_chain_store(c, target);
    if (compound_assign_at() >= 0)
        return parse_chain_compound(c, target);

    /* Finish the pending step as a GET, falling through to a general expression statement. */
    if (!equal(TOKEN_OPEN_PARENTHESE) && !equal(TOKEN_PIPE)) {
        return error_at("Expected an assignment ('='), a compound assignment ('+=' etc.), or a call/pipe "
                        "continuation after this chain");
    }

    chain_advance(c, &obj_reg, &obj_is_base, pending_is_field, pending_field_idx, pending_rk_idx);
    int rk = parse_postfix_chain(c, obj_reg);
    unsigned int lhs_start = c->count;
    rk = parse_binary_ops(c, 0, rk, lhs_start);
    discard_statement_result(c, rk);
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
        /* A lexer error while reading the boundary belongs to no statement, so no rollback records it. */
        P.any_compile_error |= parse_had_error;
        parse_had_error = false;
        P.expr_depth = 0;
        P.recovered_at_boundary = false;
        unsigned int saved = c->count;
        unsigned int saved_marks = c->line_mark_count;
        chunk_mark_line(c, saved, current_source_line());
        parse_statement(c);
        if (parse_had_error) {
            P.any_compile_error = true;
            c->count = saved;
            c->line_mark_count = saved_marks;
            peephole_window_reset();
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
       when called). Reset here so only a genuine top-level failure propagates -- after recording it,
       since an error the lexer raised on the block's last boundary was seen by no statement. */
    P.any_compile_error |= parse_had_error;
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
            bool swap_encodable = !RK8_IS_CONST(a) && !RK8_IS_CONST(b);
            /* The complement of `i < K` wants K on the left, where these opcodes take only a slot, so
               a literal bound is loaded once into a preheader slot for the back-edge to read. */
            if (swap && RK8_IS_CONST(a) != RK8_IS_CONST(b)) {
                uint8_t* k = RK8_IS_CONST(a) ? &a : &b;
                int64_t v = c->rawk_i[RK8_INDEX(*k)];
                bool wide = !(v >= INT32_MIN && v <= INT32_MAX);
                int slot = hoist_constant(true, v, wide ? RK8_INDEX(*k) : 0, wide);
                if (slot >= 0 && slot <= (int)RK8_INDEX_MASK) {
                    *k = (uint8_t)slot;
                    swap_encodable = true;
                }
            }
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
   second operator starts a fresh subexpression rather than continuing a left-associative run.
   Returns the leaf count (0 for anything else) and the postfix program four bits per token; leaves
   are RK operands, so a constant is a leaf like any other and the tile evaluator broadcasts it. */
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
        if (!(leaf[i] & RK_CONST_FLAG) && P.regs.reg_elem_kind[leaf[i]] == RAWK_REAL)
            return nleaf;
    return 0;
}


/* No exit-time cleanup needed -- col_reg/idx_reg are ordinary registers. Reserves the loop
   variable's register BEFORE compiling the collection expression, so later temps can never
   alias it. */
/* `for i in a..b`, with an optional step. Direction is inferred at runtime from cur vs end
   rather than from the step's sign, which is why this is for-loop-specific and not a value. */
/* True when rk_end reads exactly the local proven to hold length(arr), with arr's register in *array_reg.
   Fails closed -- an unrecognized shape just compiles without the proof. */
static bool range_end_is_length(int rk_end, int* array_reg) {
    if (!P.proof.length_tracked_valid || (rk_end & RK_CONST_FLAG))
        return false;
    for (int vi = 0; vi < P.var_count; vi++) {
        /* Any kind, not just VAR_BOXED: what matters is that this name still reads the length. */
        if (P.var_names[vi] == P.proof.length_tracked_name && P.var_regs[vi] == drop_raw_marks(rk_end)) {
            *array_reg = P.proof.length_tracked_source_reg;
            return true;
        }
    }
    return false;
}

/* Records what the body of a range loop may assume, popped by the caller around exactly this body. A
   bounds-safe loop pushes its array and item register (a small stack, see safe_loop_item_regs); every
   loop within LOOP_MAX tracks whether the body writes its item register. Returns that second push. */
static bool push_range_loop_facts(bool this_loop_safe, int bound_array_reg, int item_reg) {
    if (this_loop_safe) {
        P.proof.safe_loop_array_regs[P.proof.safe_loop_depth] = bound_array_reg;
        P.proof.safe_loop_item_regs[P.proof.safe_loop_depth] = item_reg;
        P.proof.safe_loop_depth++;
        /* A safe loop has a non-negative start and a positive step, so every value it takes is >= 0. */
        if (item_reg >= 0 && item_reg < FRAME_REGISTERS)
            P.regs.reg_nonneg[item_reg] = true;
    }
    bool range_tracked = P.range_loop_depth < LOOP_MAX;
    if (range_tracked) {
        P.range_item_regs[P.range_loop_depth] = item_reg;
        P.range_item_written[P.range_loop_depth] = false;
        P.range_loop_depth++;
    }
    return range_tracked;
}

/* PREP rejects a non-integer bound or step and LOOP only advances an integer, so the loop variable is an
   integer on every path into the body. Saying so lets the body index and compute unchecked -- without it
   `for i in 0..n` ran 15.9% more instructions than the hand-written `i = 0; for i < n`. */
static void mark_range_var_int(unsigned int loop_var_name, int item_reg) {
    for (int v = P.var_count - 1; v >= 0; v--)
        if (P.var_names[v] == loop_var_name && P.var_regs[v] == item_reg) {
            P.regs.var_kind[v] = VAR_RAW_INT;
            break;
        }
}

static void parse_for_range(Chunk* c, unsigned int loop_var_name, int item_reg, int rk_start) {
    int rk_end = parse_binary(c, 0);
    int rk_step;
    if (consume(TOKEN_DOT_DOT))
        rk_step = parse_binary(c, 0);
    else
        rk_step = (int)chunk_add_pool(c, aer_int(1)) | RK_CONST_FLAG;

    require(TOKEN_COLON, "expected ':' after for-in clause");
    if (parse_had_error)
        return;

    /* Checked before arg_materialize, which may copy rk_start/rk_end into registers that no longer
       identify their source. The end must be length(arr), and the start anything the compiler can show
       is >= 0 (Parser.reg_nonneg). */
    int bound_array_reg = -1;
    bool bound_safe = range_end_is_length(rk_end, &bound_array_reg);
    bool this_loop_safe = bound_safe && rk_nonneg(c, rk_start) && P.proof.safe_loop_depth < LOOP_MAX;

    /* Snapshotted once, matching Lua/Python's range-for semantics -- a later change to the bound has no
       effect on a running loop, and LOOP need not re-validate types every dispatch. */
    int cur_reg = arg_materialize(c, rk_start);
    int end_reg = arg_materialize(c, rk_end);
    int step_reg = arg_materialize(c, rk_step);

    /* Temps become permanent for the loop's duration, so a variable inside the body cannot alias
       cur_reg/step_reg; restored to the pre-loop watermark once the loop is emitted. */
    int saved_reserved_floor = P.slot_floor;
    P.slot_floor = P.slot_next;
    int raised_reserved_floor = P.slot_floor;

    bool hoisting = hoist_begin(c);
    unsigned int prep_at = c->count;
    /* Loop-rotated: PREP once before the loop, LOOP at the bottom of the body -- the one form whose
       continue must defer-patch instead of jumping to a known target. */
    unsigned int patch_empty = emit_iter_range_prep(c, cur_reg, end_reg, step_reg, item_reg, this_loop_safe);

    if (!loop_push_rotated()) {
        hoist_end(c, prep_at, hoisting);
        P.slot_floor = saved_reserved_floor;
        P.slot_next = saved_reserved_floor;
        return;
    }
    bool range_tracked = push_range_loop_facts(this_loop_safe, bound_array_reg, item_reg);
    mark_range_var_int(loop_var_name, item_reg);
    unsigned int body_start = c->count;
    parse_block(c);
    if (parse_had_error) {
        hoist_end(c, prep_at, hoisting);
        if (range_tracked)
            P.range_loop_depth--;
        if (this_loop_safe)
            P.proof.safe_loop_depth--;
        P.loop_depth--;
        P.slot_floor = saved_reserved_floor;
        P.slot_next = saved_reserved_floor;
        return;
    }
    if (this_loop_safe)
        P.proof.safe_loop_depth--;
    bool body_wrote_item = !range_tracked || P.range_item_written[P.range_loop_depth - 1];
    if (range_tracked)
        P.range_loop_depth--;

    unsigned int loop_bottom = c->count;
    /* PREP already left the start value in item_reg, so when the body never writes that register the
       loop's counter can live there and LOOP maintains one register instead of two. PREP's own cur
       operand stays cur_reg either way -- it only ever reads it. */
    emit_iter_range_loop(c, body_wrote_item ? cur_reg : item_reg, end_reg, step_reg, item_reg, body_start);

    unsigned int exit_pos = c->count;
    patch_jump(c, patch_empty, exit_pos);
    loop_pop_and_patch_rotated(c, exit_pos, loop_bottom);
    hoist_end(c, prep_at, hoisting);

    /* Conditional for the same reason the while-form's restore is: a name first assigned in the body
       outlives the loop, so lowering past its register would hand a live variable to the next
       statement as a temp. */
    if (P.slot_floor == raised_reserved_floor) {
        P.slot_floor = saved_reserved_floor;
        P.slot_next = saved_reserved_floor;
    }
    assert_variables_below_floor("range-for body");
    return;
}

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
    forget_register(item_reg);
    /* invalidate_register cannot catch loop_var_name reusing the length_tracked_name NAME itself
       (`for n in 0..1000:` after `n = length(bodies)`) -- var_slot returns a bare register number,
       indistinguishable from a new one. This loop overwrites that register every iteration, so a
       later `for i in 0..n:` would otherwise trust the stale leftover. */
    if (P.proof.length_tracked_valid && loop_var_name == P.proof.length_tracked_name)
        P.proof.length_tracked_valid = false;

    int rk_start = parse_binary(c, 0); /* the range's start, or the whole collection if no '..' follows */

    /* Direction is inferred at runtime from cur vs end, not step's sign. `..` is for-loop-specific
       syntax here. */
    if (consume(TOKEN_DOT_DOT))
        return parse_for_range(c, loop_var_name, item_reg, rk_start);

    require(TOKEN_COLON, "expected ':' after for-in clause");
    if (parse_had_error)
        return;

    int col_reg = materialize(c, rk_start);

    int idx_reg = reg_alloc();
    unsigned int pool_zero = chunk_add_pool(c, aer_int(0));
    emit_loadk(c, idx_reg, pool_zero);

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
    forget_register(key_reg);
    forget_register(val_reg);
    /* Same length_tracked_name-by-NAME reasoning as parse_for_in's own identical check. */
    if (P.proof.length_tracked_valid && (key_name == P.proof.length_tracked_name || val_name == P.proof.length_tracked_name)) {
        P.proof.length_tracked_valid = false;
    }

    int rk_col = parse_binary(c, 0);
    require(TOKEN_COLON, "expected ':' after for-in clause");
    if (parse_had_error)
        return;

    int col_reg = materialize(c, rk_col);

    int idx_reg = reg_alloc();
    unsigned int pool_zero = chunk_add_pool(c, aer_int(0));
    emit_loadk(c, idx_reg, pool_zero);

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
   names AND builtin casts/functions, so without it `import string` makes `string(42)` unreachable.
   A module use is always `name.function(...)`, so the dot is what tells the two apart. */
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

/* Every module function's spelling and wire id, expanded from aer_abi.h's AER_MODULE_FUNCTIONS. The
   length is a column because sizeof gives a literal's length only where the literal still is, not
   through the table's char pointer. */
#define MODFN(mod, lit, id) {CALL_MODULE_##mod, lit, sizeof(lit) - 1, id},
static const struct {
    int module_id;
    const char* name;
    unsigned int length;
    int fn_id;
} module_functions[] = {AER_MODULE_FUNCTIONS(MODFN)};
#undef MODFN

/* A literal identifier, never ambiguous, so resolvable once here. FN_ID_UNKNOWN for anything
   not a known function of that module. */
static int module_fn_id(int module_id, AerString* name) {
    for (unsigned int i = 0; i < sizeof(module_functions) / sizeof(*module_functions); i++)
        if (module_functions[i].module_id == module_id && name->length == module_functions[i].length &&
            strncmp(name->data, module_functions[i].name, module_functions[i].length) == 0)
            return module_functions[i].fn_id;
    return FN_ID_UNKNOWN;
}


/* count+1 registers as one contiguous run, for a fused call whose arguments the opcode reads by
   position. The base, or -1 when a register came back out of sequence; *taken counts what the caller
   must free in that case (the out-of-sequence one is not counted). */
static int reg_alloc_run(int count, int* taken) {
    int base = reg_alloc();
    *taken = base >= 0 ? 1 : 0;
    for (int i = 0; base >= 0 && i < count; i++) {
        if (reg_alloc() != base + 1 + i)
            return -1;
        (*taken)++;
    }
    return base;
}

/* `math.sqrt(<real>)` on a slot whose type is already known needs neither the module calling
   convention's tag check nor one on the result. Every function aer_math_fn_is_raw_real names is
   unary, so the argument is parsed here rather than through the general contiguous-argument path --
   that path returns registers, having already discarded the kind this needs. */
static int parse_math_raw_call(Chunk* c, unsigned int module_idx, unsigned int fn_idx, int fn_id) {
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
    emit_module_call(c, dest, dest, 1, module_idx, fn_idx, CALL_MODULE_MATH, fn_id);
    return dest;
}

/* `collection.group_sum(price * quantity * (1 - discount) * mask, region, G)` -- the values are the
   longest expression in a query and the scatter that consumes them is nearly free beside it, so the
   values fold into the scatter and are never written out. The arguments are parsed one at a time: the
   values' own code has to be recognised and discarded before the group column is compiled. Only the
   first argument folds; the other two are read as they are. */
static int parse_group_sum_call(Chunk* c, unsigned int module_idx, unsigned int fn_idx, int module_id,
                                int fn_id) {
    unsigned int values_start = c->count;
    int values_rk = parse_binary(c, 0);
    int leaf[CHAIN_MAX_LEAVES];
    uint64_t prog = 0;
    int mask_leaves = 0;
    int nleaf = (parse_had_error || (values_rk & RK_CONST_FLAG))
                    ? 0
                    : chain_of_array_ops(c, values_start, c->count, values_rk, leaf, &prog, &mask_leaves);
    /* The whole run is secured BEFORE anything is discarded -- once the values' code is gone there is
       no ordinary path left to fall back to. */
    int taken = 0;
    int base = nleaf > 0 ? reg_alloc_run(nleaf + 2, &taken) : -1;
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
        emit_loadk(c, base, chunk_add_pool(c, aer_int((int64_t)word)));
        emit_move(c, base + 1, groups);
        emit_move(c, base + 2, ngroups);
        for (int i = 0; i < nleaf; i++)
            emit_move_rk(c, base + 3 + i, leaf[i]);
        emit_module_call(c, base, base, nleaf + 3, module_idx, fn_idx, CALL_MODULE_COLLECTION,
                         FN_COLLECTION_GROUP_SUM_CHAIN);
        reg_free(nleaf + 2);
        P.regs.reg_elem_kind[base] = RAWK_REAL;
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
    emit_module_call(c, argbase, argbase, argc, module_idx, fn_idx, module_id, fn_id);
    return argbase;
}

/* `collection.sum(a * b * c)` costs one whole-array pass per operator, each writing a full-size
   intermediate the next reads straight back. Folded into one tiled pass that materialises none of
   them -- 0.0429s to 0.0098s over 8M elements. True, with *out_rk set, when it fused. */
static bool try_fuse_sum_chain(Chunk* c, unsigned int arg_code_start, int arg_reg_base,
                               unsigned int module_idx, unsigned int fn_idx, int* out_rk) {
    int leaf[CHAIN_MAX_LEAVES];
    uint64_t packed = 0;
    int mask_leaves = 0;
    int nleaf = chain_of_array_ops(c, arg_code_start, c->count, arg_reg_base, leaf, &packed, &mask_leaves);
    /* The run must be contiguous, and that is settled BEFORE anything is discarded -- once the
       argument's own code is gone there is no ordinary path left to fall back to. */
    int taken = 0;
    int base = nleaf > 0 ? reg_alloc_run(nleaf, &taken) : -1;
    if (base < 0) {
        if (taken)
            reg_free(taken);
        return false;
    }
    c->count = arg_code_start; /* the per-operator passes go; the fused call replaces them */
    uint64_t word = packed | ((uint64_t)mask_leaves << CHAIN_SPLIT_SHIFT);
    emit_loadk(c, base, chunk_add_pool(c, aer_int((int64_t)word)));
    for (int i = 0; i < nleaf; i++)
        emit_move_rk(c, base + 1 + i, leaf[i]);
    emit_module_call(c, base, base, nleaf + 1, module_idx, fn_idx, CALL_MODULE_COLLECTION,
                     FN_COLLECTION_SUM_CHAIN);
    reg_free(nleaf);
    *out_rk = base;
    return true;
}

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

    if (module_id == CALL_MODULE_MATH && aer_math_fn_is_raw_real(fn_id))
        return parse_math_raw_call(c, module_idx, fn_idx, fn_id);
    if (module_id == CALL_MODULE_COLLECTION && fn_id == FN_COLLECTION_GROUP_SUM &&
        !equal(TOKEN_CLOSE_PARENTHESE))
        return parse_group_sum_call(c, module_idx, fn_idx, module_id, fn_id);

    unsigned int arg_code_start = c->count;
    int arg_reg_base;
    bool arg_base_is_temp;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base, &arg_base_is_temp);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after module call arguments");
    if (parse_had_error)
        return 0;

    int fused;
    if (module_id == CALL_MODULE_COLLECTION && fn_id == FN_COLLECTION_SUM && arg_count == 1 &&
        try_fuse_sum_chain(c, arg_code_start, arg_reg_base, module_idx, fn_idx, &fused))
        return fused;

    /* Reusing the argument base as the destination is only safe when it is a temp -- a lone argument
       now stays in its own register, which may be a variable's. */
    int dest = (arg_count > 0 && arg_base_is_temp) ? arg_reg_base : reg_alloc();
    if (arg_count > 1)
        reg_free(arg_count - 1);
    int base = arg_reg_base < 0 ? dest : arg_reg_base;
    emit_module_call(c, dest, base, arg_count, module_idx, fn_idx, module_id, fn_id);
    return dest;
}

/* The quoted form, for paths the dotted form can't express (explicit relative components, an
   absolute path). Used exactly as written, never dot-converted. Bare native module names aren't
   reachable this way -- a quoted path always means "look on disk". `alias`/`alias_len` are already
   known when `alias_len > 0`, from the identifier `parse_import` found immediately before this
   string (`import alias "path"`, Go's import-alias convention); otherwise the bound name is derived
   from the path's own last segment. */
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

/* The builtin a name resolves to, or -1. */
static int builtin_call_id(AerString* name) {
#define AER_NAME_OF_BUILTIN(id, str) str,
    static const char* const names[] = {AER_BUILTINS(AER_NAME_OF_BUILTIN)};
#undef AER_NAME_OF_BUILTIN
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        size_t len = strlen(names[i]);
        if (name->length == len && strncmp(name->data, names[i], len) == 0)
            return i;
    }
    return -1;
}

/* Reserved global function names: a builtin or a cast, resolved at compile time, so they cannot be
   variables either (var_slot rejects them). The casts are names rather than lexer tokens -- one
   mechanism, and three fewer keywords in the language. Struct construction is resolved separately, via is_struct_name. */
static bool is_builtin_name(Chunk* c, unsigned int name_idx) {
    AerString* name = aer_as_string(c->pool[name_idx]);
    return builtin_call_id(name) >= 0 || cast_type_for_name(name) != CAST_NONE;
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
       arr_reg == P.proof.hint_param_reg, so a local can only ever reach the bare-index family. */
    P.proof.last_length_call_result_reg = -1;
    P.proof.last_length_call_arg_reg = -1;
    if (call_id == CALL_BUILTIN_LENGTH && arg_count == 1) {
        if (dest >= 0 && dest < FRAME_REGISTERS)
            P.regs.reg_nonneg[dest] = true; /* a count */
        int arg_orig_reg = base;
        if (arg_code_end - arg_code_begin == 1) {
            uint32_t w = c->code[arg_code_begin];
            if ((w & 0xFF) == OP_MOVE && (int)UNPACK_A(w) == base)
                arg_orig_reg = (int)UNPACK_B(w);
        }
        if (arg_orig_reg >= 0) {
            P.proof.last_length_call_result_reg = dest;
            P.proof.last_length_call_arg_reg = arg_orig_reg;
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

/* False after reporting that a direct call's argument count is outside the function's range. A call with
   fewer arguments than declared fills the rest from defaults, never from a prior occupant's register.
   Reported at call_site: by now the parser has read past the argument list, often onto the next line. */
static bool check_call_arity(Chunk* c, unsigned int name_idx, int arg_count, unsigned int min_arity,
                             unsigned int arity, const char* call_site) {
    if ((unsigned int)arg_count <= arity && (unsigned int)arg_count >= min_arity)
        return true;
    const char* fname = aer_as_string(c->pool[name_idx])->data;
    const char* real_start = current_token_start();
    lexer_set_token_start(call_site);
    if (min_arity == arity)
        error_at("Function '%s' expects %u argument%s, got %d", fname, arity, arity == 1 ? "" : "s",
                 arg_count);
    else
        error_at("Function '%s' expects between %u and %u arguments, got %d", fname, min_arity, arity,
                 arg_count);
    lexer_set_token_start(real_start);
    return false;
}

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
    bool is_var = var_lookup(name_idx, &var_reg);
    if (!is_var && report_if_shadowed_global(c, name_idx))
        return 0;

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
    bool is_forward_ref = !is_var && !is_struct && !is_func;
    /* Captured now, before the argument list below consumes past it. */
    const char* call_site_cursor = current_source_cursor();

    /* Usually a no-op check, not a copy -- see arg_materialize's own comment. */
    int arg_reg_base;
    bool arg_base_is_temp;
    int arg_count = parse_contiguous_exprs(c, TOKEN_CLOSE_PARENTHESE, &arg_reg_base, &arg_base_is_temp);
    require(TOKEN_CLOSE_PARENTHESE, "expected ')' after call arguments");
    if (parse_had_error)
        return 0;

    if (is_func && !check_call_arity(c, name_idx, arg_count, func_min_arity, func_arity, call_site_cursor))
        return 0;
    bool needs_call_value = is_func && (unsigned int)arg_count < func_arity;

    /* The result reuses the argument base, as Lua does, when that base is a temp -- a lone argument may
       be a variable's own register. Chosen before any callee_reg is allocated, so a freshly built
       function value always lands above dest and the arguments. */
    int dest = (arg_count > 0 && arg_base_is_temp) ? arg_reg_base : reg_alloc();
    int base = arg_reg_base < 0 ? dest : arg_reg_base;

    int callee_reg = -1;
    if (needs_call_value) {
        AerVal fv = build_function_value(func_offset, func_arity, func_min_arity, func_defaults,
                                         func_max_registers, func_frame_bounds);
        callee_reg = reg_alloc();
        emit_loadk(c, callee_reg, chunk_add_pool(c, fv));
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
        else if (P.fn.in_variant && (int)func_index == P.fn.current_func_idx && arg_count == (int)func_arity &&
                 func_arity == func_min_arity)
            chunk_emit(c, PACK3(OP_CALL_SELF, dest, base, arg_count));
        else /* already resolved, so no forward-reference patch */
            emit_call(c, dest, func_offset, base, arg_count, func_index);
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
    int extra = (arg_count > 1 ? arg_count - 1 : 0) + (needs_call_value ? 1 : 0);
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
            if (orig_op == OP_CALL || orig_op == OP_CALL_VALUE || orig_op == OP_CALL_SELF) {
                int tail_op = orig_op == OP_CALL         ? OP_TAIL_CALL
                              : orig_op == OP_CALL_VALUE ? OP_TAIL_CALL_VALUE
                                                         : OP_TAIL_CALL_SELF;
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
   specialization target either way: h_call only specializes named, ChunkFunction-registered
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
            if (report_top_level_name(c, param_names[param_count]))
                return;
            lex();
            if (consume(TOKEN_ASSIGN)) {
                bool unused_narrow;
                if (!parse_literal_default(c, &param_defaults[param_count], &unused_narrow)) {
                    return error_at("A parameter default must be a literal: a number, true, false, null, a "
                                    "string, an empty array [] or an empty hashtable {}");
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
    memcpy(saved_elem_kind, P.regs.reg_elem_kind, sizeof(saved_elem_kind));
    memcpy(saved_var_names, P.var_names, sizeof(unsigned int) * (size_t)P.var_count);
    memcpy(saved_var_regs, P.var_regs, sizeof(int) * (size_t)P.var_count);
    memcpy(saved_var_kind, P.regs.var_kind, sizeof(VarKind) * (size_t)P.var_count);
    P.var_count = 0;
    P.slot_next = P.slot_floor = P.slot_max = 0;

    memset(P.regs.shape_sensitive_param, 0, sizeof(P.regs.shape_sensitive_param));
    for (int i = 0; i < FRAME_REGISTERS; i++)
        P.regs.alias_source_param[i] = -1;
    memset(P.regs.reg_known_shape, 0, sizeof(P.regs.reg_known_shape));
    memset(P.regs.reg_known_element_shape, 0, sizeof(P.regs.reg_known_element_shape));
    P.index_alias.dest_reg = -1;
    P.index_alias.src_param = -1;
    P.index_alias.elem_shape = NULL;
    P.regs.current_param_count = param_count;
    /* hint_param_reg only enables the loop-bound-hoisting optimization for a packed-array
       specialization (kind == SPEC_KIND_PACKED_ARRAY, hint_is_element_shape false) -- an
       array-of-structs specialization has no per-object structural guarantee (see SPEC_KIND_
       ARRAY_OF_STRUCTS's own comment, vm.c) so index_safe_unchecked must never engage for one. */
    P.proof.hint_param_reg = hint_is_element_shape ? -1 : hint_param_reg;
    P.proof.length_tracked_valid = false;
    P.proof.length_tracked_source_reg = -1;
    P.proof.last_length_call_result_reg = -1;
    P.proof.last_length_call_arg_reg = -1;
    P.proof.safe_loop_depth = 0;

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
                P.regs.reg_known_element_shape[i] = hint_shape;
            else
                P.regs.reg_known_shape[i] = hint_shape;
        }
        for (int k = 0; k < raw_param_count; k++) {
            if (raw_param_regs[k] != i)
                continue;
            /* The argument already sits in register i, correctly tagged, and the resolver verified
               its type before choosing this variant -- so recording the type is the whole of the
               binding. No second slot, and no prologue opcode to fill one: the parameter simply
               stops needing its tag checked. */
            P.regs.var_kind[P.var_count - 1] = (raw_param_types[k] == TYPE_INTEGER) ? VAR_RAW_INT : VAR_RAW_REAL;
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
    memcpy(P.regs.var_kind, saved_var_kind, sizeof(VarKind) * (size_t)saved_var_count);
    P.slot_next = saved_next_temp;
    P.slot_floor = saved_reserved_floor;
    P.slot_max = saved_max_slot_used;
    P.raw_real_next = saved_raw_real_next;
    P.raw_real_floor = saved_raw_real_floor;
    P.raw_real_low = saved_raw_real_low;
    memcpy(P.regs.reg_elem_kind, saved_elem_kind, sizeof(saved_elem_kind));
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
    unsigned int raw_boxed_before = P.fn.raw_boxed_emits;
    int saved_func_idx = P.fn.current_func_idx;
    P.fn.current_func_idx = (int)this_func_idx;
    parse_function_body(c, param_names, param_count, -1, NULL, false, NULL, NULL, 0, &captured_max_registers,
                        &captured_frame_bounds);
    unsigned int raw_boxed_in_body = P.fn.raw_boxed_emits - raw_boxed_before;
    P.fn.current_func_idx = saved_func_idx;

    /* Fold P.regs.shape_sensitive_param[] into one bitmask; retain the source span (owned copy, see
       ChunkFunction.source_span's own comment) only when it's actually needed -- the common case
       (not shape-sensitive) pays nothing beyond the mask computation itself. parse_function_body
       already restored P.regs.shape_sensitive_param/P.regs.current_param_count's OWN inputs, but not the fold-in
       -- reads them here, right after the call, before anything else can touch them. */
    unsigned int shape_mask = 0;
    for (int i = 0; i < param_count && i < 31; i++)
        if (P.regs.shape_sensitive_param[i])
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

    /* Scheduler workers reach this from h_call, and the parser is one set of file statics. */
    aer_mutex_lock(&specialize_lock);
    /* error_at would otherwise longjmp to the enclosing vm_run_slice, past every restore below and
       out of a function whose contract is that a failed compile leaves nothing behind. */
    AerJmpBuf* saved_unwind = runtime_error_unwind_target;
    runtime_error_unwind_target = NULL;

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
    P.fn.current_func_idx = (int)(target_f - c->functions);
    /* Only a numeric variant may self-call without resolving: a shape-specialized body is chosen by
       the argument's SHAPE, which a recursive call has no guarantee of preserving. */
    P.fn.in_variant = (raw_param_count > 0);
    if (!parse_had_error) {
        parse_function_body(c, param_names, param_count, param_index, shape,
                            kind == SPEC_KIND_ARRAY_OF_STRUCTS, raw_param_regs, raw_param_types,
                            raw_param_count, &max_registers, &frame_bounds);
    }
    P.fn.in_variant = false;
    bool ok = !parse_had_error;

    lexer_restore_state(saved_lexer);
    parser_restore_state(saved_parser);
    parse_had_error = saved_had_error;
    runtime_error_unwind_target = saved_unwind;
    aer_mutex_unlock(&specialize_lock);

    if (!ok)
        return false;

    out_entry->code_offset = new_offset;
    out_entry->max_registers = max_registers;
    out_entry->frame_bounds = frame_bounds;
    if (raw_param_count == 0) {
        /* Ordinary shape-only compile -- a freshly-created SpecEntry (see h_call, vm.c) needs its
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
        *out = aer_array_val(vm_new_array(0));
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
            return error_at("A struct field default must be a literal: a number, true, false, null, a "
                            "string, an empty array [] or an empty hashtable {}");
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
    peephole_window_reset();
    P.fn.current_func_idx = -1; /* 0 is a real function index, so zeroed is not "outside a body" */
    P.var_count = 0;
    P.global_count = 0;
    P.struct_count = 0;
    P.pending_count = 0;
    P.function_depth = 0;
    P.loop_depth = 0;
    P.expr_depth = 0;
    aer_reset_parse_error_count();
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
    P.fn.current_func_idx = -1; /* 0 is a real function index, so zeroed is not "outside a body" */
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
    parse_had_error = false;
    aer_reset_parse_error_count();
    while (!equal(TOKEN_END_OF_FILE)) {
        if (consume(TOKEN_NEW_LINE))
            continue;
        if (consume(TOKEN_DEDENT))
            continue;
        /* A lexer error while reading the boundary belongs to no statement, so no rollback records it. */
        P.any_compile_error |= parse_had_error;
        parse_had_error = false;
        P.expr_depth = 0;
        P.recovered_at_boundary = false;
        unsigned int saved = c->count;
        unsigned int saved_marks = c->line_mark_count;
        chunk_mark_line(c, saved, current_source_line());
        parse_statement(c);
        if (parse_had_error) {
            P.any_compile_error = true;
            c->count = saved;
            c->line_mark_count = saved_marks;
            peephole_window_reset();
            if (!P.recovered_at_boundary) {
                while (!equal(TOKEN_END_OF_FILE) && !equal(TOKEN_NEW_LINE) && !equal(TOKEN_DEDENT))
                    lex();
                skip_orphaned_block();
            }
        }
    }
    if (P.pending_count > 0) {
        const char* real_start = current_token_start();
        bool any_pending_error = false;
        for (int i = 0; i < P.pending_count; i++) {
            unsigned int patch_offset = P.pending_calls[i].patch_offset;
            c->code[patch_offset - 1] = OP_HALT;
            lexer_set_token_start(P.pending_calls[i].call_site_cursor);
            error_at("Unknown function or struct type '%s' (never defined anywhere in this compile — not a "
                     "valid forward reference, module call, or struct construction target)",
                     aer_as_string(c->pool[P.pending_calls[i].name_idx])->data);
            any_pending_error = true;
        }
        lexer_set_token_start(real_start);
        P.pending_count = 0;
        if (any_pending_error)
            P.any_compile_error = true;
    }
    if (P.any_compile_error)
        parse_had_error = true;
}

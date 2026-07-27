#ifndef VM_H
#define VM_H

#include "error.h"
#include "hashtable.h"
#include "pool.h"
#include "value.h"

/* gc_state first — see pool.h and AerArray's own comment (value.h) for why. */
struct AerDict {
    unsigned char gc_state;
    HashTable     map;
};
_Static_assert(offsetof(struct AerDict, gc_state) == 0, "pool.c assumes gc_state is byte 0");

typedef enum {
    /* Binary arithmetic */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_FLOOR_DIV,

    /* Binary comparison */
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    OP_IN,              /* key in dict → key existence; value in array → element scan */

    /* Never dispatched — parser tags for `&&`/`||`, which compile to short-circuit jumps. */
    OP_AND, OP_OR,
    OP_PIPE,            /* never dispatched either — `x |> f(args)` desugars to a call at parse time; kept as a lookup-table tag only, same reason as OP_AND/OP_OR */

    /* Binary bitwise */
    OP_BITWISE_AND, OP_BITWISE_OR, OP_BITWISE_XOR, OP_LSHIFT, OP_RSHIFT,

    /* Unary */
    OP_NEGATE, OP_NOT, OP_BITWISE_NOT,

    /* OP_JUMP's handler only touches vm->ip, so register opcodes reuse it for unconditional jumps. */
    OP_JUMP,            /* operand: absolute code index */

    /* Struct definitions only — instantiation/field access are register opcodes below. */
    OP_DEFINE_STRUCT,    /* operands: name pool idx, field count, then that many (field-name, default-value) pool-idx pairs — registers a Shape in the chunk's shape table */

    /* Misc */
    OP_TO_STR,  /* used as OP_UNARY's unary_op tag (string interpolation's value-to-string step) — see vm_to_str() */
    OP_HALT,

    /* ---- register-VM opcodes (packed encoding: see PACK3/PACK_BINARY below). */
    OP_LOADK,  /* operands: dest_reg, pool_idx — registers[dest_reg] = chunk pool constant */
    OP_MOVE,   /* operands: dest_reg, src_reg — registers[dest_reg] = registers[src_reg] */
    /* dest_reg, src_reg — dest = (src is a Result); gates |>'s short-circuit (compile_pipe). */
    OP_IS_RESULT,
    /* RK-encoded operand: register index, or (bit 30 set) constant-pool index — see vm_rk_value. */
    OP_BINARY, /* operands: dest_reg, rk_b, bin_op, rk_c — registers[dest_reg] = rk_b OP rk_c */

    /* Condition is read straight from a register/RK constant, never popped. */
    OP_JUMP_IF_FALSE_REG, /* operands: reg, target — jump to target if registers[reg] is falsy */

    /* Calls — per-call register windowing (CallFrame): each call gets an isolated bank. */
    OP_CALL,   /* dest_reg, callee_offset, arg_reg_base, arg_count — copies args into the
                     callee frame, saves return address + dest, jumps */
    OP_RETURN, /* src_reg (callee frame) — result to caller's dest_reg, pops the frame, jumps back */

    /* Call through a runtime function value in a register; fills omitted trailing args from defaults. */
    OP_CALL_VALUE, /* dest_reg, arg_reg_base, arg_count, callee_reg */

    /* `return f(args)` with nothing wrapping the call: the already-emitted OP_CALL word's opcode
       byte is patched in place. Same dispatch label as OP_CALL; overwrites the CURRENT frame's
       registers[0..arg_count) (arg registers are always temps, above any local) and jumps —
       call_depth/dest_reg/return_ip stay untouched. */
    OP_TAIL_CALL,        /* same operands as OP_CALL; dest_reg is unused (ignored) here */
    OP_TAIL_CALL_VALUE,  /* same operands as OP_CALL_VALUE; dest_reg is unused (ignored) here */

    /* Bridges to the stack-based stdlib dispatch: push args from registers, call aer_*_call(),
       pop the one result into dest_reg. Stack-neutral. */
    /* One packed word (dest, module_idx, fn_idx, arg base/count) + two trailing words:
       parse-time-resolved module_id and fn_id (CALL_MODULE_DYNAMIC = host/file module,
       resolved by name at runtime). */
    OP_CALL_MODULE,

    /* Bare global builtins (length/print/type/assert/panic/Result): bridges to vm_call_builtin()
       via a small local array. Trailing word: builtin_id. */
    OP_CALL_BUILTIN, /* operands: dest_reg, name_pool_idx, arg_reg_base, arg_count, builtin_id */

    /* Registers can hold heap values — mark_vm_roots scans all of registers[] per live frame. */
    OP_ARRAY_NEW,  /* dest_reg, item_reg_base, item_count */
    OP_INDEX_GET,  /* dest_reg, arr_reg, rk_idx — via vm_index_get_compute */
    OP_INDEX_SET,  /* arr_reg, rk_idx, rk_val — via vm_index_set_compute (includes the write barrier) */

    /* `a, b = expr`: Result/array unpacks normally, anything else becomes (value, null). */
    OP_DESTRUCTURE, /* target0_reg, target1_reg, src_reg */

    /* Slicing (array or string); a missing bound compiles to an RK null constant. */
    OP_SLICE_GET,  /* dest_reg, arr_reg, rk_start, rk_end */

    /* `x as Point` for a struct type — shape check only, never converts. */
    OP_CHECK_SHAPE, /* dest_reg, src_reg, type_name_pool_idx */

    /* OP_ARRAY_NEW's bulk-copy applied to (key,val) register pairs; same key validation and
       owned-key discipline as stack dict construction. */
    OP_DICT_NEW,   /* dest_reg, pair_reg_base, pair_count — key at base+2*i, val at +2*i+1 */

    /* Single-variable iteration; break is a plain jump, no cleanup needed. */
    OP_ITER_NEXT_ARRAY, /* col_reg, idx_reg, item_dest_reg, end_target — despite the name, also accepts a dict
                              (yields keys) or string (yields chars) */

    /* Dict-only two-variable form; val_dest_reg needs its own word (only 3 narrow fields pack). */
    OP_ITER_NEXT_PAIR, /* col_reg, idx_reg, key_dest_reg, val_dest_reg, end_target — key is a fresh owned string */

    /* Rotated range-for (Lua's FORLOOP shape): PREP runs once before the loop, LOOP at the loop
       BOTTOM is itself the back-edge (no OP_JUMP). PREP repurposes end_reg/step_reg into a
       countdown + signed step — safe only because cur/end/step are loop-owned snapshots
       (arg_materialize), never aliases, so LOOP never re-validates their types. */
    OP_ITER_RANGE_PREP, /* cur, end, step, item_dest, empty_target — validates ints (the only place), computes the
                               iteration count via ceiling division; count 0 jumps to empty_target
                               untouched; else end_reg = count-1, step_reg = signed step,
                               item_dest = cur */
    OP_ITER_RANGE_LOOP, /* cur, remaining (was end), signed_step (was step), item_dest, body_target (already-resolved
                               address) — countdown 0: fall through to the exit; else advance
                               cur/item_dest, decrement, branch back to body_target */

    /* Instantiation/field get/set need register operands (definitions reuse OP_DEFINE_STRUCT). */
    OP_STRUCT_NEW, /* dest_reg, type_name_pool_idx, arg_reg_base, arg_count — arity-checked, trailing
                          fields default-filled */
    OP_FIELD_GET,  /* dest_reg, struct_reg, field_name_pool_idx */
    OP_FIELD_SET,  /* struct_reg, field_name_pool_idx, rk_val — includes the gc_barrier_array call */

    /* `Type[count]` — fixed-primitive fields only (checked here at runtime, once the Shape is
       known); every element default-initialized. */
    OP_PACKED_ARRAY_NEW, /* dest_reg, type_name_pool_idx, rk_count */

    /* Fused `obj[index].field` get/set — packed arrays have no standalone element reference, so
       the parser emits these only for the exact `expr[index].field` pattern. Ordinary arrays
       take the same opcodes with behavior identical to the old two-step sequence. */
    OP_INDEX_FIELD_GET, /* operands: dest_reg, obj_reg, field_name_pool_idx, rk_idx */
    OP_INDEX_FIELD_SET, /* operands: obj_reg, field_name_pool_idx, rk_idx, rk_val */

    /* `obj[index].field OP= rk_rhs` — resolves the index+field exactly once (one dispatch), instead
       of the OP_INDEX_FIELD_GET (read) + OP_INDEX_FIELD_SET (a second, redundant index+field
       resolution just to write the same slot back) pair this used to compile to. This is the
       pattern nbody-style code hits constantly (`bodies[j].vx += dx * mi`). */
    OP_INDEX_FIELD_COMPOUND, /* operands: obj_reg, field_name_pool_idx, rk_idx, bin_op, rk_rhs */

    /* unary_op reuses OP_NEGATE/OP_NOT/OP_BITWISE_NOT/OP_TO_STR as its tag, like bin_op. */
    OP_UNARY, /* dest_reg, unary_op, rk_operand — also folds OP_TO_STR (interpolation) via vm_to_str */

    /* `x as integer/float/boolean` via vm_cast; `as string` is OP_UNARY's TO_STR, `as Struct`
       is OP_CHECK_SHAPE. */
    OP_CAST, /* dest_reg, cast_type, rk_operand */

    /* Fusion of `x OP y.field` — parse_binary_ops truncates the just-emitted OP_FIELD_GET and
       re-encodes it as this opcode's trailing operands. */
    OP_BINARY_FIELD, /* dest_reg, rk_lhs, bin_op, struct_reg, field_name_pool_idx */

    /* Mirror for `y.field OP x` — the field is the LEFT operand, so correct for every operator. */
    OP_FIELD_BINARY, /* dest_reg, struct_reg, field_name_pool_idx, bin_op, rk_rhs */

    /* `struct.field OP= rhs` — reads, computes, and writes back in one dispatch, one
       vm_resolve_field call. Before this existed, the parser emitted OP_FIELD_BINARY (read+compute
       into a temp) followed by a separate OP_FIELD_SET (a second, redundant field resolution just
       to write the same field back) -- two inline-cache lookups for one logical operation. Same
       operand shape as OP_FIELD_BINARY (dest slot unused, no destination register needed). */
    OP_FIELD_COMPOUND, /* struct_reg, field_name_pool_idx, bin_op, rk_rhs */

    /* Shell mode: a bare statement's non-null result is printed. */
    OP_PRINT_REPL, /* operand: src_reg — prints registers[src_reg] unless it's TYPE_NULL */

    /* Raw (unboxed) arithmetic on provably-monotype locals; operands are raw_ints/raw_reals slot
       indices, no RK encoding. Comparisons produce a boxed boolean; OP_BOX_* is the only bridge
       back to registers[]. */
    OP_RAW_LOAD_INT, OP_RAW_LOAD_REAL,
    OP_RAW_ADD_INT, OP_RAW_SUB_INT, OP_RAW_MUL_INT, OP_RAW_DIV_INT,
    OP_RAW_MOD_INT, OP_RAW_FLOOR_DIV_INT,
    OP_RAW_ADD_REAL, OP_RAW_SUB_REAL, OP_RAW_MUL_REAL, OP_RAW_DIV_REAL,
    OP_RAW_LT_INT, OP_RAW_GT_INT, OP_RAW_LTE_INT, OP_RAW_GTE_INT,
    OP_RAW_LT_REAL, OP_RAW_GT_REAL, OP_RAW_LTE_REAL, OP_RAW_GTE_REAL,
    OP_BOX_INT, OP_BOX_REAL,
    /* Raw-to-raw copy — OP_MOVE's analog for raw slots. */
    OP_RAW_MOVE_INT, OP_RAW_MOVE_REAL,
    /* In-place accumulation of a BOXED value into a raw slot (`e += <boxed expr>`); runtime tag
       check, ADD/SUB/MUL only. */
    OP_RAW_ADD_INT_BOXED, OP_RAW_SUB_INT_BOXED, OP_RAW_MUL_INT_BOXED,
    OP_RAW_ADD_REAL_BOXED, OP_RAW_SUB_REAL_BOXED, OP_RAW_MUL_REAL_BOXED,
    /* Raw-vs-boxed comparison producing a boxed boolean — removes the OP_BOX_INT that dominated
       `for i <= limit:`-shaped loops. Not in-place. */
    OP_RAW_LT_INT_BOXED, OP_RAW_GT_INT_BOXED, OP_RAW_LTE_INT_BOXED, OP_RAW_GTE_INT_BOXED,
    OP_RAW_LT_REAL_BOXED, OP_RAW_GT_REAL_BOXED, OP_RAW_LTE_REAL_BOXED, OP_RAW_GTE_REAL_BOXED,
    /* Pool fallback for literals outside OP_RAW_LOAD_INT's signed 20-bit immediate — silent
       truncation once turned a 20M-iteration bound into 77056. */
    OP_RAW_LOAD_INT_POOL,
} Opcode;

/* RK bit: set = constant-pool index, clear = register (Lua's BITRK convention). */
#define RK_CONST_FLAG (1 << 30)

/* Compiler-internal raw-slot tags (bits 28/29) — never emitted into an instruction word. */
#define RK_RAW_INT_FLAG  (1 << 29)
#define RK_RAW_REAL_FLAG (1 << 28)
#define RK_RAW_SLOT_MASK 0x1F

/* Per-frame register bank size; a register index must stay within one packed byte. */
#define FRAME_REGISTERS 128

/* Raw slot counts — 5-bit fields in the packed words; overflow falls back to boxed storage. */
#define RAW_REGISTERS_INT  32
#define RAW_REGISTERS_REAL 32

/* Packed descriptor word: narrow fields (registers, small tags) share the opcode's word —
   [C:8][B:8][A:8][opcode:8]. WIDE fields (RK operands, pool indices, jump targets) keep their
   own word. Patchable jump targets are NEVER packed alongside anything — patch_jump must stay
   a blind word overwrite. */
#define PACK3(op, a, b, cc) \
    (((int)(op) & 0xFF) | (((a) & 0xFF) << 8) | (((b) & 0xFF) << 16) | (((cc) & 0xFF) << 24))
#define PACK2(op, a, b)   PACK3(op, a, b, 0)
#define PACK1(op, a)      PACK3(op, a, 0, 0)
#define UNPACK_A(word) (((word) >> 8)  & 0xFF)
#define UNPACK_B(word) (((word) >> 16) & 0xFF)
#define UNPACK_C(word) (((word) >> 24) & 0xFF)

/* Per-operator opcodes (OP_ADD..OP_IN, over a third of all dispatches on nbody) pack the whole
   instruction into the LOW 32 bits: opcode(7, = DISPATCH's mask) + dest(7) + two RK9 operands —
   a field straddling the 32-bit boundary costs a two-register reconstruction on 32-bit ARM.
   RK9 = 1 flag + 8 index bits; emit_binary spills a constant past that budget to a register. */
#define RK9_CONST_FLAG (1U << 8)
#define RK9_INDEX_MASK 0xFFU
#define RK9_MAX_INDEX  0xFF
#define PACK_RK9(rk) \
    (((rk) & RK_CONST_FLAG) \
        ? (RK9_CONST_FLAG | ((uint64_t)((rk) & ~RK_CONST_FLAG) & RK9_INDEX_MASK)) \
        : ((uint64_t)(rk) & RK9_INDEX_MASK))
#define PACK_BINARY(op, dest, rk_b, rk_c) \
    ( ((uint64_t)(op)   & 0x7F) \
    | (((uint64_t)(dest) & 0x7F) << 7) \
    | ((PACK_RK9(rk_b) & 0x1FFULL) << 14) \
    | ((PACK_RK9(rk_c) & 0x1FFULL) << 23) )
/* Cast to uint32_t BEFORE shifting — a uint64_t shift is a real two-register operation on
   32-bit ARM even when the useful bits fit low (confirmed via disassembly). */
#define UNPACK_BINARY_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x7FU)
#define UNPACK_RK_B9(word)       ((((uint32_t)(word)) >> 14) & 0x1FFU)
#define UNPACK_RK_C9(word)       ((((uint32_t)(word)) >> 23) & 0x1FFU)

/* RK20: 1 flag + 19 index bits — the wider RK scheme every other packed opcode uses. */
#define RK20_CONST_FLAG (1ULL << 19)
#define RK20_INDEX_MASK 0x7FFFFULL
#define RK20_MAX_INDEX  0x7FFFF
#define PACK_RK20(rk) \
    (((rk) & RK_CONST_FLAG) \
        ? (RK20_CONST_FLAG | ((uint64_t)((rk) & ~RK_CONST_FLAG) & RK20_INDEX_MASK)) \
        : ((uint64_t)(rk) & RK20_INDEX_MASK))

/* 7-bit register fields (no flag — provably < FRAME_REGISTERS). Patchable jump targets stay
   excluded from packing. */
#define PACK_REG4(op, a, b, cc, d) \
    ( ((uint64_t)(op) & 0xFF) \
    | (((uint64_t)(a)  & 0x7F) << 8) \
    | (((uint64_t)(b)  & 0x7F) << 15) \
    | (((uint64_t)(cc) & 0x7F) << 22) \
    | (((uint64_t)(d)  & 0x7F) << 29) )
#define UNPACK_REG4_A(word) (((word) >> 8)  & 0x7F)
#define UNPACK_REG4_B(word) (((word) >> 15) & 0x7F)
#define UNPACK_REG4_C(word) (((word) >> 22) & 0x7F)
#define UNPACK_REG4_D(word) (((word) >> 29) & 0x7F)

/* field_idx (bare pool index) gets the remaining 42 bits — no overflow guard needed there. */
#define PACK_FIELD_GET(dest, struct_reg, field_idx) \
    ( ((uint64_t)(OP_FIELD_GET)  & 0xFF) \
    | (((uint64_t)(dest)         & 0x7F) << 8) \
    | (((uint64_t)(struct_reg)   & 0x7F) << 15) \
    | (((uint64_t)(field_idx)    & 0x3FFFFFFFFFFULL) << 22) )
#define UNPACK_FIELD_GET_DEST(word)   (((word) >> 8)  & 0x7F)
#define UNPACK_FIELD_GET_STRUCT(word) (((word) >> 15) & 0x7F)
#define UNPACK_FIELD_GET_FIELD(word)  (((word) >> 22) & 0x3FFFFFFFFFFULL)

/* op(7)+struct(7)+field_idx(9)+rk_val(RK9) = 32 bits, low word — hottest opcode on nbody. */
#define PACK_FIELD_SET(struct_reg, field_idx, rk_val) \
    ( ((uint64_t)(OP_FIELD_SET) & 0x7F) \
    | (((uint64_t)(struct_reg)  & 0x7F) << 7) \
    | (((uint64_t)(field_idx)   & 0x1FFULL) << 14) \
    | ((PACK_RK9(rk_val) & 0x1FFULL) << 23) )
#define UNPACK_FIELD_SET_STRUCT(word) ((((uint32_t)(word)) >> 7)  & 0x7FU)
#define UNPACK_FIELD_SET_FIELD(word)  ((((uint32_t)(word)) >> 14) & 0x1FFU)
#define UNPACK_FIELD_SET_RK(word)     ((((uint32_t)(word)) >> 23) & 0x1FFU)

/* op(7)+dest(7)+arr(7)+rk_idx(RK9) = 30 bits, low word. */
#define PACK_INDEX_GET(dest, arr_reg, rk_idx) \
    ( ((uint64_t)(OP_INDEX_GET) & 0x7F) \
    | (((uint64_t)(dest)    & 0x7F) << 7) \
    | (((uint64_t)(arr_reg) & 0x7F) << 14) \
    | ((PACK_RK9(rk_idx) & 0x1FFULL) << 21) )
#define UNPACK_INDEX_GET_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x7FU)
#define UNPACK_INDEX_GET_ARR(word)  ((((uint32_t)(word)) >> 14) & 0x7FU)
#define UNPACK_INDEX_GET_RK(word)   ((((uint32_t)(word)) >> 21) & 0x1FFU)

/* Three plain registers, no RK encoding needed -- the source is already a materialized register
   by the time parse_assignment reaches this (arg_materialize already ran), same as PACK_INDEX_GET's
   own arr_reg operand. */
#define PACK_DESTRUCTURE(t0, t1, src_reg) \
    ( ((uint64_t)(OP_DESTRUCTURE) & 0x7F) \
    | (((uint64_t)(t0)      & 0x7F) << 7) \
    | (((uint64_t)(t1)      & 0x7F) << 14) \
    | (((uint64_t)(src_reg) & 0x7F) << 21) )
#define UNPACK_DESTRUCTURE_T0(word)  ((((uint32_t)(word)) >> 7)  & 0x7FU)
#define UNPACK_DESTRUCTURE_T1(word)  ((((uint32_t)(word)) >> 14) & 0x7FU)
#define UNPACK_DESTRUCTURE_SRC(word) ((((uint32_t)(word)) >> 21) & 0x7FU)

/* No patchable targets in these, so all fields fold into one word. */
/* unary_op is a real Opcode value, so it needs 7 bits, not a narrow tag. */
#define PACK_UNARY(dest, unary_op, rk) \
    ( ((uint64_t)(OP_UNARY) & 0x7F) \
    | (((uint64_t)(dest)     & 0x7F) << 7) \
    | (((uint64_t)(unary_op) & 0x7F) << 14) \
    | ((PACK_RK9(rk) & 0x1FFULL) << 21) )
#define UNPACK_UNARY_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x7FU)
#define UNPACK_UNARY_OP(word)   ((((uint32_t)(word)) >> 14) & 0x7FU)
#define UNPACK_UNARY_RK(word)   ((((uint32_t)(word)) >> 21) & 0x1FFU)

/* cast_type is a 3-value tag — 4 bits is enough. */
#define PACK_CAST(dest, cast_type, rk) \
    ( ((uint64_t)(OP_CAST) & 0x7F) \
    | (((uint64_t)(dest)      & 0x7F) << 7) \
    | (((uint64_t)(cast_type) & 0xFULL) << 14) \
    | ((PACK_RK9(rk) & 0x1FFULL) << 18) )
#define UNPACK_CAST_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x7FU)
#define UNPACK_CAST_TYPE(word) ((((uint32_t)(word)) >> 14) & 0xFU)
#define UNPACK_CAST_RK(word)   ((((uint32_t)(word)) >> 18) & 0x1FFU)

/* Raw-arithmetic encodings: no register-vs-constant flag anywhere — the parser knows
   statically. 5-bit slot indices; the (uint32_t) casts before shifts are required, not
   decorative (see PACK_BINARY's comment). */

/* op(7)+dest(5)+a(5)+b(5); all three slots address the same raw array (op says which). */
#define PACK_RAW_ARITH_RR(op, dest, a, b) \
    ( ((uint64_t)(op)    & 0x7F) \
    | (((uint64_t)(dest) & 0x1F) << 7) \
    | (((uint64_t)(a)    & 0x1F) << 12) \
    | (((uint64_t)(b)    & 0x1F) << 17) )
#define UNPACK_RAW_ARITH_RR_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x1FU)
#define UNPACK_RAW_ARITH_RR_A(word)    ((((uint32_t)(word)) >> 12) & 0x1FU)
#define UNPACK_RAW_ARITH_RR_B(word)    ((((uint32_t)(word)) >> 17) & 0x1FU)

/* Comparisons: boxed-boolean dest is a 7-bit registers[] index; a/b are 5-bit raw slots. */
#define PACK_RAW_CMP(op, dest, a, b) \
    ( ((uint64_t)(op)    & 0x7F) \
    | (((uint64_t)(dest) & 0x7F) << 7) \
    | (((uint64_t)(a)    & 0x1F) << 14) \
    | (((uint64_t)(b)    & 0x1F) << 19) )
#define UNPACK_RAW_CMP_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x7FU)
#define UNPACK_RAW_CMP_A(word)    ((((uint32_t)(word)) >> 14) & 0x1FU)
#define UNPACK_RAW_CMP_B(word)    ((((uint32_t)(word)) >> 19) & 0x1FU)

/* dest(5) + signed 20-bit immediate. */
#define PACK_RAW_LOAD_INT(dest, imm) \
    ( ((uint64_t)(OP_RAW_LOAD_INT) & 0x7F) \
    | (((uint64_t)(dest) & 0x1F) << 7) \
    | (((uint64_t)(imm)  & 0xFFFFFULL) << 12) )
#define UNPACK_RAW_LOAD_INT_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x1FU)
#define UNPACK_RAW_LOAD_INT_IMM(word) \
    (((int32_t)((((uint32_t)(word)) >> 12) << 12)) >> 12)

/* Reals need full precision — pool-sourced like OP_LOADK; pool_idx gets 20 bits, guarded at
   compile time. */
#define PACK_RAW_LOAD_REAL(dest, pool_idx) \
    ( ((uint64_t)(OP_RAW_LOAD_REAL) & 0x7F) \
    | (((uint64_t)(dest)     & 0x1F) << 7) \
    | (((uint64_t)(pool_idx) & 0xFFFFFULL) << 12) )
#define UNPACK_RAW_LOAD_REAL_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x1FU)
#define UNPACK_RAW_LOAD_REAL_POOL(word) ((((uint32_t)(word)) >> 12) & 0xFFFFFU)

/* Pool fallback for integers outside the 20-bit immediate; same shape as PACK_RAW_LOAD_REAL. */
#define PACK_RAW_LOAD_INT_POOL(dest, pool_idx) \
    ( ((uint64_t)(OP_RAW_LOAD_INT_POOL) & 0x7F) \
    | (((uint64_t)(dest)     & 0x1F) << 7) \
    | (((uint64_t)(pool_idx) & 0xFFFFFULL) << 12) )
#define UNPACK_RAW_LOAD_INT_POOL_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x1FU)
#define UNPACK_RAW_LOAD_INT_POOL_POOL(word) ((((uint32_t)(word)) >> 12) & 0xFFFFFU)

/* The only raw-to-registers[] bridge; op (OP_BOX_INT/REAL) says which raw array. */
#define PACK_BOX(op, dest, src) \
    ( ((uint64_t)(op)    & 0x7F) \
    | (((uint64_t)(dest) & 0x7F) << 7) \
    | (((uint64_t)(src)  & 0x1F) << 14) )
#define UNPACK_BOX_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x7FU)
#define UNPACK_BOX_SRC(word)  ((((uint32_t)(word)) >> 14) & 0x1FU)

/* Raw-to-raw copy; both 5-bit slots in the same array (op says which). */
#define PACK_RAW_MOVE(op, dest, src) \
    ( ((uint64_t)(op)   & 0x7F) \
    | (((uint64_t)(dest) & 0x1F) << 7) \
    | (((uint64_t)(src)  & 0x1F) << 12) )
#define UNPACK_RAW_MOVE_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x1FU)
#define UNPACK_RAW_MOVE_SRC(word)  ((((uint32_t)(word)) >> 12) & 0x1FU)

/* In-place boxed-into-raw accumulation: slot is both the read and write operand. */
#define PACK_RAW_ARITH_BOXED(op, slot, boxed_reg) \
    ( ((uint64_t)(op)        & 0x7F) \
    | (((uint64_t)(slot)     & 0x1F) << 7) \
    | (((uint64_t)(boxed_reg) & 0x7F) << 12) )
#define UNPACK_RAW_ARITH_BOXED_SLOT(word)  ((((uint32_t)(word)) >> 7)  & 0x1FU)
#define UNPACK_RAW_ARITH_BOXED_REG(word)   ((((uint32_t)(word)) >> 12) & 0x7FU)

/* Raw-vs-boxed comparison — not in-place, so it has its own boxed dest register. */
#define PACK_RAW_CMP_BOXED(op, dest, slot, boxed_reg) \
    ( ((uint64_t)(op)        & 0x7F) \
    | (((uint64_t)(dest)     & 0x7F) << 7) \
    | (((uint64_t)(slot)     & 0x1F) << 14) \
    | (((uint64_t)(boxed_reg) & 0x7F) << 19) )
#define UNPACK_RAW_CMP_BOXED_DEST(word) ((((uint32_t)(word)) >> 7)  & 0x7FU)
#define UNPACK_RAW_CMP_BOXED_SLOT(word) ((((uint32_t)(word)) >> 14) & 0x1FU)
#define UNPACK_RAW_CMP_BOXED_REG(word)  ((((uint32_t)(word)) >> 19) & 0x7FU)

/* type_name_idx is a bare pool index (compile-time name), 42 bits. */
#define PACK_CHECK_SHAPE(dest, lhs_reg, type_name_idx) \
    ( ((uint64_t)(OP_CHECK_SHAPE) & 0xFF) \
    | (((uint64_t)(dest)          & 0x7F) << 8) \
    | (((uint64_t)(lhs_reg)       & 0x7F) << 15) \
    | (((uint64_t)(type_name_idx) & 0x3FFFFFFFFFFULL) << 22) )
#define UNPACK_CHECK_SHAPE_DEST(word) (((word) >> 8)  & 0x7F)
#define UNPACK_CHECK_SHAPE_LHS(word)  (((word) >> 15) & 0x7F)
#define UNPACK_CHECK_SHAPE_NAME(word) (((word) >> 22) & 0x3FFFFFFFFFFULL)

/* type_name_pool_idx gets 35 bits — never patched (struct types resolve at parse time). */
#define PACK_STRUCT_NEW(dest, arg_reg_base, arg_count, type_name_idx) \
    ( ((uint64_t)(OP_STRUCT_NEW) & 0xFF) \
    | (((uint64_t)(dest)          & 0x7F) << 8) \
    | (((uint64_t)(arg_reg_base)  & 0x7F) << 15) \
    | (((uint64_t)(arg_count)     & 0x7F) << 22) \
    | (((uint64_t)(type_name_idx) & 0x7FFFFFFFFULL) << 29) )
#define UNPACK_STRUCT_NEW_DEST(word)     (((word) >> 8)  & 0x7F)
#define UNPACK_STRUCT_NEW_ARG_BASE(word) (((word) >> 15) & 0x7F)
#define UNPACK_STRUCT_NEW_ARG_COUNT(word) (((word) >> 22) & 0x7F)
#define UNPACK_STRUCT_NEW_NAME(word)     (((word) >> 29) & 0x7FFFFFFFFULL)

/* type_name_idx(35) + rk_count(RK9 — counts are usually registers). */
#define PACK_PACKED_ARRAY_NEW(dest, type_name_idx, rk_count) \
    ( ((uint64_t)(OP_PACKED_ARRAY_NEW) & 0xFF) \
    | (((uint64_t)(dest)          & 0x7F) << 8) \
    | (((uint64_t)(type_name_idx) & 0x7FFFFFFFFULL) << 15) \
    | ((PACK_RK9(rk_count) & 0x1FFULL) << 50) )
#define UNPACK_PACKED_ARRAY_NEW_DEST(word)  (((word) >> 8)  & 0x7F)
#define UNPACK_PACKED_ARRAY_NEW_NAME(word)  (((word) >> 15) & 0x7FFFFFFFFULL)
#define UNPACK_PACKED_ARRAY_NEW_COUNT(word) (((word) >> 50) & 0x1FFULL)

/* Tightest batch: OP_FIELD_BINARY/OP_BINARY_FIELD leave only 14 bits for field_idx (16384
   field names) — guarded like the rest. */
#define PACK_INDEX_SET(arr_reg, rk_idx, rk_val) \
    ( ((uint64_t)(OP_INDEX_SET) & 0xFF) \
    | (((uint64_t)(arr_reg) & 0x7F) << 8) \
    | ((PACK_RK20(rk_idx) & 0xFFFFFULL) << 15) \
    | ((PACK_RK20(rk_val) & 0xFFFFFULL) << 35) )
#define UNPACK_INDEX_SET_ARR(word) (((word) >> 8)  & 0x7F)
#define UNPACK_INDEX_SET_IDX(word) (((word) >> 15) & 0xFFFFFULL)
#define UNPACK_INDEX_SET_VAL(word) (((word) >> 35) & 0xFFFFFULL)

#define PACK_SLICE_GET(dest, arr_reg, rk_start, rk_end) \
    ( ((uint64_t)(OP_SLICE_GET) & 0xFF) \
    | (((uint64_t)(dest)    & 0x7F) << 8) \
    | (((uint64_t)(arr_reg) & 0x7F) << 15) \
    | ((PACK_RK20(rk_start) & 0xFFFFFULL) << 22) \
    | ((PACK_RK20(rk_end)   & 0xFFFFFULL) << 42) )
#define UNPACK_SLICE_GET_DEST(word)  (((word) >> 8)  & 0x7F)
#define UNPACK_SLICE_GET_ARR(word)   (((word) >> 15) & 0x7F)
#define UNPACK_SLICE_GET_START(word) (((word) >> 22) & 0xFFFFFULL)
#define UNPACK_SLICE_GET_END(word)   (((word) >> 42) & 0xFFFFFULL)

#define CALL_MODULE_NAME_MASK  0x1FFFFULL
#define CALL_MODULE_NAME_MAX   0x1FFFF
#define PACK_CALL_MODULE(dest, arg_reg_base, arg_count, module_idx, fn_idx) \
    ( ((uint64_t)(OP_CALL_MODULE) & 0xFF) \
    | (((uint64_t)(dest)         & 0x7F) << 8) \
    | (((uint64_t)(arg_reg_base) & 0x7F) << 15) \
    | (((uint64_t)(arg_count)    & 0x7F) << 22) \
    | (((uint64_t)(module_idx) & CALL_MODULE_NAME_MASK) << 29) \
    | (((uint64_t)(fn_idx)     & CALL_MODULE_NAME_MASK) << 46) )
#define UNPACK_CALL_MODULE_DEST(word)     (((word) >> 8)  & 0x7F)
#define UNPACK_CALL_MODULE_ARG_BASE(word) (((word) >> 15) & 0x7F)
#define UNPACK_CALL_MODULE_ARG_COUNT(word) (((word) >> 22) & 0x7F)
#define UNPACK_CALL_MODULE_MODULE(word)  (((word) >> 29) & CALL_MODULE_NAME_MASK)
#define UNPACK_CALL_MODULE_FN(word)      (((word) >> 46) & CALL_MODULE_NAME_MASK)

#define PACK_CALL_BUILTIN(dest, arg_reg_base, arg_count, name_idx) \
    ( ((uint64_t)(OP_CALL_BUILTIN) & 0xFF) \
    | (((uint64_t)(dest)         & 0x7F) << 8) \
    | (((uint64_t)(arg_reg_base) & 0x7F) << 15) \
    | (((uint64_t)(arg_count)    & 0x7F) << 22) \
    | (((uint64_t)(name_idx)     & 0x7FFFFFFFFULL) << 29) )
#define UNPACK_CALL_BUILTIN_DEST(word)     (((word) >> 8)  & 0x7F)
#define UNPACK_CALL_BUILTIN_ARG_BASE(word) (((word) >> 15) & 0x7F)
#define UNPACK_CALL_BUILTIN_ARG_COUNT(word) (((word) >> 22) & 0x7F)
#define UNPACK_CALL_BUILTIN_NAME(word)    (((word) >> 29) & 0x7FFFFFFFFULL)

#define FUSED_FIELD_NAME_MASK 0x3FFFULL
#define FUSED_FIELD_NAME_MAX  0x3FFF
#define PACK_FIELD_BINARY(dest, struct_reg, bin_op, field_idx, rk_rhs) \
    ( ((uint64_t)(OP_FIELD_BINARY) & 0xFF) \
    | (((uint64_t)(dest)       & 0x7F) << 8) \
    | (((uint64_t)(struct_reg) & 0x7F) << 15) \
    | (((uint64_t)(bin_op)     & 0xFF) << 22) \
    | (((uint64_t)(field_idx) & FUSED_FIELD_NAME_MASK) << 30) \
    | ((PACK_RK20(rk_rhs) & 0xFFFFFULL) << 44) )
#define UNPACK_FIELD_BINARY_DEST(word)   (((word) >> 8)  & 0x7F)
#define UNPACK_FIELD_BINARY_STRUCT(word) (((word) >> 15) & 0x7F)
#define UNPACK_FIELD_BINARY_OP(word)     (((word) >> 22) & 0xFF)
#define UNPACK_FIELD_BINARY_NAME(word)   (((word) >> 30) & FUSED_FIELD_NAME_MASK)
#define UNPACK_FIELD_BINARY_RK(word)     (((word) >> 44) & 0xFFFFFULL)

/* Same layout as PACK_FIELD_BINARY (the dest slot is simply unused -- the result writes back into
   the same field, no destination register needed), just tagged with OP_FIELD_COMPOUND instead. */
#define PACK_FIELD_COMPOUND(struct_reg, bin_op, field_idx, rk_rhs) \
    ( ((uint64_t)(OP_FIELD_COMPOUND) & 0xFF) \
    | (((uint64_t)(struct_reg) & 0x7F) << 15) \
    | (((uint64_t)(bin_op)     & 0xFF) << 22) \
    | (((uint64_t)(field_idx) & FUSED_FIELD_NAME_MASK) << 30) \
    | ((PACK_RK20(rk_rhs) & 0xFFFFFULL) << 44) )
#define UNPACK_FIELD_COMPOUND_STRUCT(word) (((word) >> 15) & 0x7F)
#define UNPACK_FIELD_COMPOUND_OP(word)     (((word) >> 22) & 0xFF)
#define UNPACK_FIELD_COMPOUND_NAME(word)   (((word) >> 30) & FUSED_FIELD_NAME_MASK)
#define UNPACK_FIELD_COMPOUND_RK(word)     (((word) >> 44) & 0xFFFFFULL)

#define PACK_BINARY_FIELD(dest, struct_reg, bin_op, rk_lhs, field_idx) \
    ( ((uint64_t)(OP_BINARY_FIELD) & 0xFF) \
    | (((uint64_t)(dest)       & 0x7F) << 8) \
    | (((uint64_t)(struct_reg) & 0x7F) << 15) \
    | (((uint64_t)(bin_op)     & 0xFF) << 22) \
    | ((PACK_RK20(rk_lhs) & 0xFFFFFULL) << 30) \
    | (((uint64_t)(field_idx) & FUSED_FIELD_NAME_MASK) << 50) )
#define UNPACK_BINARY_FIELD_DEST(word)   (((word) >> 8)  & 0x7F)
#define UNPACK_BINARY_FIELD_STRUCT(word) (((word) >> 15) & 0x7F)
#define UNPACK_BINARY_FIELD_OP(word)     (((word) >> 22) & 0xFF)
#define UNPACK_BINARY_FIELD_RK(word)     (((word) >> 30) & 0xFFFFFULL)
#define UNPACK_BINARY_FIELD_NAME(word)   (((word) >> 50) & FUSED_FIELD_NAME_MASK)

/* dest(7)+obj(7)+field_idx(14)+rk_idx(RK20). */
#define PACK_INDEX_FIELD_GET(dest, obj_reg, field_idx, rk_idx) \
    ( ((uint64_t)(OP_INDEX_FIELD_GET) & 0xFF) \
    | (((uint64_t)(dest)    & 0x7F) << 8) \
    | (((uint64_t)(obj_reg) & 0x7F) << 15) \
    | (((uint64_t)(field_idx) & FUSED_FIELD_NAME_MASK) << 22) \
    | ((PACK_RK20(rk_idx) & 0xFFFFFULL) << 36) )
#define UNPACK_INDEX_FIELD_GET_DEST(word)  (((word) >> 8)  & 0x7F)
#define UNPACK_INDEX_FIELD_GET_OBJ(word)   (((word) >> 15) & 0x7F)
#define UNPACK_INDEX_FIELD_GET_FIELD(word) (((word) >> 22) & FUSED_FIELD_NAME_MASK)
#define UNPACK_INDEX_FIELD_GET_RK(word)    (((word) >> 36) & 0xFFFFFULL)

/* obj(7)+field_idx(14)+rk_idx+rk_val — both RK9 to fit one word. */
#define PACK_INDEX_FIELD_SET(obj_reg, field_idx, rk_idx, rk_val) \
    ( ((uint64_t)(OP_INDEX_FIELD_SET) & 0xFF) \
    | (((uint64_t)(obj_reg)   & 0x7F) << 8) \
    | (((uint64_t)(field_idx) & FUSED_FIELD_NAME_MASK) << 15) \
    | ((PACK_RK9(rk_idx) & 0x1FFULL) << 29) \
    | ((PACK_RK9(rk_val) & 0x1FFULL) << 38) )
#define UNPACK_INDEX_FIELD_SET_OBJ(word)   (((word) >> 8)  & 0x7F)
#define UNPACK_INDEX_FIELD_SET_FIELD(word) (((word) >> 15) & FUSED_FIELD_NAME_MASK)
#define UNPACK_INDEX_FIELD_SET_IDX(word)   (((word) >> 29) & 0x1FFULL)
#define UNPACK_INDEX_FIELD_SET_VAL(word)   (((word) >> 38) & 0x1FFULL)

/* obj(7)+field_idx(14)+rk_idx(RK9)+bin_op(8)+rk_rhs(RK9) = 55 bits + opcode(8), fits one word. */
#define PACK_INDEX_FIELD_COMPOUND(obj_reg, field_idx, rk_idx, bin_op, rk_rhs) \
    ( ((uint64_t)(OP_INDEX_FIELD_COMPOUND) & 0xFF) \
    | (((uint64_t)(obj_reg)   & 0x7F) << 8) \
    | (((uint64_t)(field_idx) & FUSED_FIELD_NAME_MASK) << 15) \
    | ((PACK_RK9(rk_idx) & 0x1FFULL) << 29) \
    | (((uint64_t)(bin_op)    & 0xFFULL) << 38) \
    | ((PACK_RK9(rk_rhs) & 0x1FFULL) << 46) )
#define UNPACK_INDEX_FIELD_COMPOUND_OBJ(word)   (((word) >> 8)  & 0x7F)
#define UNPACK_INDEX_FIELD_COMPOUND_FIELD(word) (((word) >> 15) & FUSED_FIELD_NAME_MASK)
#define UNPACK_INDEX_FIELD_COMPOUND_IDX(word)   (((word) >> 29) & 0x1FFULL)
#define UNPACK_INDEX_FIELD_COMPOUND_OP(word)    (((word) >> 38) & 0xFFULL)
#define UNPACK_INDEX_FIELD_COMPOUND_RHS(word)   (((word) >> 46) & 0x1FFULL)

/* OP_CAST operand values — target type for `x as T` (T=string compiles to OP_TO_STR instead, since that conversion already existed). */
#define CAST_INTEGER 0
#define CAST_FLOAT   1
#define CAST_BOOLEAN 2

/* OP_CALL_MODULE's trailing module_id word, resolved at parse time; CALL_MODULE_DYNAMIC =
   host/file module, resolved by name at runtime. */
#define CALL_MODULE_MATH       0
#define CALL_MODULE_RANDOM     1
#define CALL_MODULE_STRING     2
#define CALL_MODULE_TIME       3
#define CALL_MODULE_JSON       4
#define CALL_MODULE_COLLECTION 5
#define CALL_MODULE_NET        6
#define CALL_MODULE_REGEX      7
#define CALL_MODULE_ACTOR      8
#define CALL_MODULE_SCHEDULER  9
#define CALL_MODULE_DYNAMIC    10

/* Second trailing word: fn_id within the module (each module owns a flat id space);
   FN_ID_UNKNOWN still errors by name, never misroutes to id 0. */
#define FN_ID_UNKNOWN   (-1)

#define FN_MATH_SQRT    0
#define FN_MATH_POW     1
#define FN_MATH_FLOOR   2
#define FN_MATH_CEIL    3
#define FN_MATH_ABS     4
#define FN_MATH_MIN     5
#define FN_MATH_MAX     6
#define FN_MATH_SIN     7
#define FN_MATH_COS     8
#define FN_MATH_LOG     9
#define FN_MATH_LOG2    10
#define FN_MATH_LOG10   11
#define FN_MATH_PI      12
#define FN_MATH_ROUND   13
#define FN_MATH_TAN     14
#define FN_MATH_EXP     15

#define FN_RANDOM_RANDOM  0
#define FN_RANDOM_RANDINT 1
#define FN_RANDOM_SEED    2
#define FN_RANDOM_CHOICE  3
#define FN_RANDOM_SHUFFLE 4

#define FN_STRING_UPPER       0
#define FN_STRING_LOWER       1
#define FN_STRING_TRIM        2
#define FN_STRING_CONTAINS    3
#define FN_STRING_SPLIT       4
#define FN_STRING_STARTS_WITH 5
#define FN_STRING_ENDS_WITH   6
#define FN_STRING_REPEAT      7
#define FN_STRING_REPLACE     8
#define FN_STRING_JOIN        9
#define FN_STRING_INDEX_OF    10

#define FN_TIME_NOW       0
#define FN_TIME_STRFTIME  1
#define FN_TIME_SLEEP     2
#define FN_TIME_PARSE     3
#define FN_TIME_TO_PARTS  4
#define FN_TIME_FROM_PARTS 5

#define FN_JSON_ENCODE 0
#define FN_JSON_DECODE 1

#define FN_COLLECTION_APPEND   0
#define FN_COLLECTION_DELETE   1
#define FN_COLLECTION_COPY     2
#define FN_COLLECTION_INSERT   3
#define FN_COLLECTION_INDEX_OF 4
#define FN_COLLECTION_KEYS     5
#define FN_COLLECTION_SORT     6

#define FN_NET_CONNECT 0
#define FN_NET_SEND    1
#define FN_NET_RECV    2
#define FN_NET_CLOSE   3
#define FN_NET_LISTEN  4
#define FN_NET_ACCEPT  5

#define FN_REGEX_MATCH    0
#define FN_REGEX_FIND     1
#define FN_REGEX_REPLACE  2
#define FN_REGEX_FIND_ALL 3

#define FN_ACTOR_SPAWN   0
#define FN_ACTOR_SEND    1
#define FN_ACTOR_RECEIVE 2
#define FN_ACTOR_CALL    3

#define FN_SCHEDULER_ADD 0
#define FN_SCHEDULER_RUN 1

/* OP_CALL_BUILTIN's trailing builtin_id — no DYNAMIC case; is_builtin_name gates every site. */
#define CALL_BUILTIN_LENGTH 0
#define CALL_BUILTIN_PRINT  1
#define CALL_BUILTIN_TYPE   2
#define CALL_BUILTIN_ASSERT 3
#define CALL_BUILTIN_PANIC  4
/* The only way AER source constructs a Result — lets user functions join |>'s short-circuit. */
#define CALL_BUILTIN_RESULT 5

#define MAX_STRUCT_FIELDS 16

/* A struct type's blueprint (field names in order + default literals); individually heap-allocated
   and never moved/realloc'd, so AerStruct.shape pointers stay valid as the shape table grows. */
struct Shape {
    unsigned int name;                              /* pool index of the struct's type name */
    unsigned int field_count;
    unsigned int field_names[MAX_STRUCT_FIELDS];     /* pool indices, declaration order       */
    AerVal       field_defaults[MAX_STRUCT_FIELDS];
    /* TYPE_ANY = no declared type. A declared type is enforced once at FIELD_SET/construction,
       then trusted — the fused opcodes skip the runtime check on that side. */
    ValueType    field_types[MAX_STRUCT_FIELDS];
    /* Byte offset of each field within an instance's fields buffer (AerStruct.fields) -- a typed
       field (TYPE_ANY excluded) is stored RAW in 8 bytes (no tag; the type is this Shape's own
       static knowledge, never read from the instance), an untyped (TYPE_ANY) field stays a full
       boxed AerVal (16 bytes), since it can hold any value including a reference type the GC must
       trace. Computed once in OP_DEFINE_STRUCT's handler, right after field_types is known. See
       vm_struct_field_read/vm_struct_field_write. */
    unsigned int field_offsets[MAX_STRUCT_FIELDS];
    unsigned int instance_bytes;   /* total size of the fields buffer -- sum of every field's width above */
};

/* A single struct instance -- its own type (TYPE_STRUCT), split out from AerArray specifically
   because sharing one C type/tag for "ordinary array" and "struct instance" meant every site
   handling TYPE_ARRAY had to remember to ask "but what if this is actually a struct" (one real
   site didn't -- for-x-in iteration silently walked a struct's fields with no shape check at all).
   No count/capacity: a struct's field count is always shape->field_count, fixed, never grows --
   carrying them the way the old shared AerArray design did was already dead weight. */
struct AerStruct {
    unsigned char  gc_state;   /* byte 0, same pool.c convention as every other pool-managed type */
    Shape*         shape;
    unsigned char* fields;     /* set to (char*)a + sizeof(AerStruct) at construction -- inline in
                                   the same pool cell, matching AerArray's own items pointer trick */
};
_Static_assert(offsetof(struct AerStruct, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* Reads/writes one struct field at its own byte offset -- raw (untagged, vm_packed_slot_read's
   scheme) for a typed field, a full boxed AerVal for a TYPE_ANY one. Shared by every struct-field
   opcode in vm.c plus json.encode's struct-serialization branch (aer_json.c), which is why these
   aren't file-static. */
AerVal vm_struct_field_read(AerStruct* s, unsigned int slot);
void   vm_struct_field_write(AerStruct* s, unsigned int slot, AerVal v);

/* Runtime-visible function registration — outlives the parser tables so cross-module calls
   can find exports by name after compilation (same precedent as chunk->shapes[]). */
typedef struct {
    unsigned int name;           /* pool index of the function's name */
    unsigned int code_offset;
    unsigned int arity;
    unsigned int min_arity;
    AerVal*      defaults;       /* (arity - min_arity) owned values or NULL; freed by chunk_free */
    /* Real peak register need, patched in after the body compiles; the FRAME_REGISTERS
       placeholder (read only by in-body self-reference) is never an under-allocation. */
    unsigned int max_registers;
} ChunkFunction;

/* ------------------------------------------------------------------ */
/* Bytecode chunk                                                       */
/* Flat 64-bit word array: one opcode word (with packed fields) plus optional operand words. */
/* ------------------------------------------------------------------ */

/* One per-callsite field-cache entry — see Chunk.field_cache's own comment below. Caches offset
   and ftype alongside slot, not just slot: both are pure functions of (shape, slot), so once the
   shape comparison confirms a cache hit, re-deriving them from shape->field_offsets[slot]/
   field_types[slot] on every single access was a second, avoidable indirection through Shape --
   this makes a cache hit read them from the entry itself (already touched for the shape check)
   instead. */
typedef struct {
    Shape*       shape;
    int          slot;
    unsigned int offset;
    ValueType    ftype;
} FieldCacheEntry;

typedef struct {
    uint64_t* code;
    unsigned int count, capacity;

    char* source_filename;   /* owned copy; NULL for a chunk with no real file (e.g. aer_run_source on a raw string) */

    AerVal*      pool;           /* constants and variable names — all deduplicated by value */
    unsigned int pool_count, pool_cap;

    /* name -> pool index, for O(1) dedup of TYPE_STRING pool entries (chunk_add_pool, vm.c); owns an independent copy of each key. */
    HashTable    name_index;

    /* Struct type registry appended to by OP_DEFINE_STRUCT; redeclaring a struct appends rather than replaces so old Shape pointers stay valid, and chunk_find_shape() searches newest-first. */
    Shape**      shapes;
    unsigned int shape_count, shape_cap;

    /* Function registry — appended by func_register, searched newest-first. */
    ChunkFunction* functions;
    unsigned int   function_count, function_cap;

    /* Module names from `import`, parse-time only, tracked on the chunk so a later REPL line still recognizes a module an earlier line imported. */
    char**       imported_modules;
    unsigned int import_count, import_cap;

    /* Offset -> source line, one entry per statement; strictly increasing (binary-searched);
       rolled back with bytecode on parse-error recovery. */
    unsigned int* line_mark_offsets;
    unsigned int* line_mark_lines;
    unsigned int  line_mark_count, line_mark_cap;

    /* Per-site inline cache for FIELD_GET/SET: last Shape* + resolved slot. Monomorphic sites
       skip the name scan; polymorphic sites just miss. Shape* is never reallocated, so a cached
       pointer can't go stale. */
    FieldCacheEntry* field_cache;
    unsigned int field_cache_cap;

#ifdef AER_DEBUG_TOOLS
    /* Per-word dispatch counters (debug-tools only); only opcode words increment. */
    uint64_t* debug_hits;
    unsigned int         debug_hits_cap;
#endif
} Chunk;

/* ------------------------------------------------------------------ */
/* Per-VM heap — every pool a VM allocates from, plus its own GC state. */
/* ------------------------------------------------------------------ */

typedef enum { REMEMBERED_ARRAY, REMEMBERED_DICT, REMEMBERED_STRUCT } RememberedKind;
typedef struct { void* ptr; RememberedKind kind; } RememberedEntry;

/* Explicit growable worklist, not C recursion, since user data structures have no depth limit;
   pool_mark's "already marked" return terminates cycles correctly. */
typedef struct {
    AerVal*      items;
    unsigned int count, cap;
} MarkWorklist;

typedef struct {
    Pool string_pool, array_pool, dict_pool, function_pool, struct_pool, packed_array_pool, result_pool;
    bool pools_initialized;

    /* Old objects a write barrier caught holding a young reference; entries are only ever
       added/deduped, never removed, and re-traced as extra roots on every minor collection
       thereafter. */
    RememberedEntry* remembered_set;
    unsigned int     remembered_count, remembered_cap;
    /* Set once, forever, on the first real collection this heap ever runs -- before that,
       POOL_OLD can't be set anywhere, so the write barrier is provably a no-op. */
    bool gc_ever_collected;

    MarkWorklist gc_worklist;

    /* Tuning (overridable via aer_gc_configure()): minor_gc_threshold is total cells allocated
       across all pools since the last minor GC; major_gc_every_n_minor runs a major pass after
       that many minor ones. 0 for gc_live_cell_ceiling means unlimited (aer_gc_set_ceiling). */
    unsigned int minor_gc_threshold, major_gc_every_n_minor, gc_live_cell_ceiling;
    unsigned int minor_collections_run, major_collections_run, minor_since_major;
    int          gc_suppress_depth;

    /* Cells allocated since gc_reset_alloc_counts -- gc_maybe_collect checks this against
       minor_gc_threshold. Was a single process-global counter (pool.c); now one per heap. */
    unsigned int pool_alloc_count;

    /* Every AerDict this heap owns gets its key/sparse-array storage from here -- was a single
       process-global HashPools (hashtable.c); now one per heap, same as the 7 GC pools above.
       Chunk.name_index (no owning VM) uses its own separate, still-process-global HashPools --
       see vm.c's chunk_name_index_pools. */
    HashPools dict_hash_pools;
} VmHeap;

/* ------------------------------------------------------------------ */
/* Virtual machine                                                      */
/* ------------------------------------------------------------------ */

#define VM_STACK_MAX    256
#define VM_CALL_MAX     64
#define VM_KEY_MAX      4096   /* max dict key length for stack-buffered lookups */

/* Per-call register frame; lives in the VM struct so a nested module VM gets its own chain. */
typedef struct {
    /* Bump-pointer base into vm->register_stack — a call is a pointer add, never an allocation. */
    AerVal*      registers;
    /* Registers THIS frame reserved (callee's compile-time peak; FRAME_REGISTERS for frame 0) —
       read by the next push. */
    unsigned int frame_size;

    /* Raw unboxed scratch for the primitive pass — never GC-scanned, never crosses a call
       boundary (only its boxed form does). */
    int64_t      raw_ints[RAW_REGISTERS_INT];
    double       raw_reals[RAW_REGISTERS_REAL];

    unsigned int return_ip;   /* where to resume in the CALLER */
    int          dest_reg;    /* which of the CALLER's registers gets the return value */

    unsigned int code_offset;  /* this frame's entry point, for stack traces; unset on frame 0 */
    unsigned int tail_calls_collapsed;  /* tail calls collapsed into this frame since its last real push */
    bool synthetic_entry;  /* true for a setup_call()-pushed frame -- return_ip isn't a real caller line */
} CallFrame;

typedef struct {
    Chunk*       chunk;
    unsigned int ip;

    /* This VM's own heap -- every pool it allocates from, independent of every other VM's. */
    VmHeap       heap;

    /* Per-VM capability toggles, seeded from the process-wide aer_io_enabled/aer_net_enabled
       defaults at vm_init AND every aer_run_source call (the REPL/embedding "run more code into an
       existing VM" entry point) -- see those externs' own comment below for why io/net moved here
       but import_enabled didn't. */
    bool         io_enabled;
    bool         net_enabled;

    /* Scratch argument channel for bridging out of the register convention (stdlib/module calls). */
    AerVal       stack[VM_STACK_MAX];
    int          stack_top;

    /* Always point at call_stack[call_depth]'s arrays — repointed together on call/return. */
    CallFrame  call_stack[VM_CALL_MAX];
    AerVal*      registers;
    int64_t*     raw_ints;
    double*      raw_reals;
    int          call_depth;

    /* One shared register bank for the whole chain (calls bump a base pointer). Same worst-case
       size as a flat design, but the actually-touched working set is far smaller. */
    AerVal       register_stack[VM_CALL_MAX * FRAME_REGISTERS];
} VM;

/* Bounds-checked push/pop for native-module files, outside vm_run's PUSH()/POP() macros. */
static inline bool vm_stack_push(VM* vm, AerVal v) {
    if (vm->stack_top >= VM_STACK_MAX) { error("Stack overflow"); return false; }
    vm->stack[vm->stack_top++] = v;
    return true;
}

static inline AerVal vm_stack_pop(VM* vm) {
    if (vm->stack_top <= 0) { error("Stack underflow"); return aer_null(); }
    return vm->stack[--vm->stack_top];
}

void         chunk_init(Chunk* c);
void         chunk_free(Chunk* c);
void         chunk_emit(Chunk* c, uint64_t word);

/* Records that bytecode from `offset` onward belongs to source `line`, once per statement not instruction (see line_mark_offsets); no-op if offset doesn't strictly increase from the last mark. */
void         chunk_mark_line(Chunk* c, unsigned int offset, unsigned int line);

/* The source line whose statement contains `offset` (the largest recorded mark at or before it), or 0 if the chunk has no marks yet. */
unsigned int chunk_line_for_offset(Chunk* c, unsigned int offset);

/* Formats a real guaranteeing a decimal point/exponent/nan-inf marker survives — bare "%g"
   prints 42.0 as "42", which flips to integer through the JSON round-trip. The one shared site
   (vm.c, aer_json.c, disasm.c). */
void aer_format_real(double d, char* buf, size_t bufsize);

unsigned int chunk_add_pool(Chunk* c, AerVal v);
Shape*       chunk_find_shape(Chunk* c, const char* name);

/* `defaults` is taken by ownership, never copied. */
void           chunk_add_function(Chunk* c, unsigned int name_idx, unsigned int code_offset,
                                   unsigned int arity, unsigned int min_arity, AerVal* defaults);
ChunkFunction* chunk_find_function(Chunk* c, const char* name);

/* Parse-time variant — name_idx is a dedup'd pool index, so this is an int compare, no strcmp. */
ChunkFunction* chunk_find_function_by_name_idx(Chunk* c, unsigned int name_idx);

/* Recovers a frame's function from its code_offset, for stack traces. Cold path only. */
ChunkFunction* chunk_find_function_by_offset(Chunk* c, unsigned int code_offset);

/* `name` binds the module; `path_name` resolves to the file (dots as separators). Neither
   is NUL-terminated. */
bool chunk_add_import(Chunk* c, const char* name, unsigned int len,
                       const char* path_name, unsigned int path_len);
bool chunk_is_imported(Chunk* c, const char* name, unsigned int len);

/* Process-wide capability DEFAULTS -- default true (every prior release's always-on behavior,
   unchanged unless a host/CLI flag opts out). io/net are only defaults now: vm_init copies them
   into VM.io_enabled/net_enabled at creation, and aer_net_call/the io dispatch case in vm_run_slice
   check the per-VM field, not these globals directly -- so two VMs in the same process can now run
   with different io/net capabilities (a plugin host running an untrusted script alongside a
   trusted one, say). import_enabled stays a real, directly-checked global: chunk_add_import() runs
   at PARSE time with only a Chunk* in scope, no VM* at all (the same "Chunk has no owning VM"
   situation the hashtable pools hit) -- giving it the same per-VM treatment would need routing
   through current_heap-style "current VM" plumbing for a check that fires once per import
   statement, not worth it for that. Set via aer_set_io_enabled()/aer_set_net_enabled()/
   aer_set_import_enabled() (include/aer.h), not directly. This is a blast-radius limiter, not a
   real permission system -- see the README's Sandboxing note. */
extern bool aer_io_enabled;
extern bool aer_net_enabled;
extern bool aer_import_enabled;

void vm_init(VM* vm, Chunk* chunk);
void vm_free(VM* vm);

/* Save/restore around a nested vm_init() on a fresh VM while the caller's own execution is paused
   on the C call stack (aer_module.c's aer_vm_instantiate_from_file) -- see vm_current_heap's own
   comment in vm.c for why vm_run_slice's save/restore alone isn't enough here. */
VmHeap* vm_current_heap(void);
void    vm_set_current_heap(VmHeap* heap);

/* Runs from vm->ip to OP_HALT or runtime error (returns false). A host reusing the VM after
   a false return must reset stack_top/call_depth first — see main.c's run(). */
bool vm_run(VM* vm);

typedef enum {
    VM_SLICE_DONE,      /* reached OP_HALT */
    VM_SLICE_YIELDED,   /* max_instructions reached at a loop back-edge or call; vm->ip is a valid resume point */
    VM_SLICE_ERROR,     /* runtime error, same as vm_run's false */
} VmSliceResult;

/* vm_run(vm) is exactly vm_run_slice(vm, 0) -- 0 means unlimited, the only budget every caller but
   the scheduler (aer_scheduler.c) ever passes. A nonzero budget bounds how many loop-back-edges and
   calls this call executes before returning VM_SLICE_YIELDED with vm->ip left at a valid resume
   point; calling vm_run_slice again on the same VM continues exactly where it left off, the same
   way vm_run already resumes from wherever vm->ip points (main.c's REPL already relies on this). */
VmSliceResult vm_run_slice(VM* vm, unsigned int max_instructions);

/* A counter, not a flag — imports/module-calls nest. Operates on whichever heap is current (see
   vm.c's current_heap); each VM now collects only its own independent heap, so this no longer
   guards against one VM's collection reaching into another's not-yet-rooted state (structurally
   impossible now, separate heaps) — aer_module.c's two call sites predate that split and are kept
   as harmless no-ops rather than removed speculatively. */
void vm_gc_suppress(void);
void vm_gc_unsuppress(void);

/* Cross-module call setup (aer_module_call only): pushes a real frame with dest_reg fixed at
   0 — after the trampoline drains, the result is in call_stack[0].registers[0]. */
bool setup_call(VM* target, ChunkFunction* fn, int arg_count,
                    AerVal* args, unsigned int return_ip);

/* Returns an uninitialized AerArray header from vm.c's internal slab pool, as if xmalloc'd directly (every in-file vm.c site still uses pool_alloc); exposed only because aer_stdlib.c's string.split() needs one and the pool isn't a raw global outside vm.c. */
AerArray* vm_new_array(void);

/* Same idea, for AerDict — exposed for aer_json.c's json.decode(); caller must zero-init map itself (see lbl_dict_new's call site in vm.c). */
AerDict* vm_new_dict(void);

/* Generational-GC write barrier — any store of `new_value` into an already-existing array must go
   through this (see gc_barrier_array's own comment, vm.c). Exposed for aer_collection.c's
   append/insert; a freshly built, not-yet-returned array needs no barrier. */
void gc_barrier_array(VM* vm, AerArray* a, AerVal new_value);

/* Same contract as gc_barrier_array, for a struct field-set -- AerStruct is its own type/pool now,
   not a shaped AerArray, so it needs its own barrier rather than gc_barrier_array's old
   shape-ternary dispatch. */
void gc_barrier_struct(VM* vm, AerStruct* s, AerVal new_value);

/* Structural/reference equality with no error path (see its comment in vm.c) — exposed for
   aer_collection.c's index_of, the same scan OP_IN's array case uses. */
bool values_equal(AerVal a, AerVal b);

/* Must come from function_pool (pool_mark's slab lookup fails on xmalloc'd cells); returns
   uninitialized memory — zero it yourself. */
AerFunction* vm_new_function(void);

/* Test-only register readback (tests/smoke_test.c). */
AerVal register_get(VM* vm, int slot);

#ifdef AER_DEBUG_TOOLS
#include <stdio.h>
/* Byte-accurate per-pool memory breakdown; debug-tools only. */
void aer_debug_memory_report(FILE* out);
#endif

#endif

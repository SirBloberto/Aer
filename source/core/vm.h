#ifndef VM_H
#define VM_H

#include "error.h"
#include "hashtable.h"
#include "pool.h"
#include "strbuf.h"
#include "value.h"

/* gc_state first -- see pool.h and AerArray's own comment (value.h) for why. Small fields ordered to
   fill gc_state's padding gap before map (which needs pointer alignment), not declaration-grouped
   by topic -- see value.h's own top comment. */
struct AerDict {
    unsigned char gc_state;
    /* Card marking for the O(n) minor-GC rescan fix -- same fields, same reasoning, as AerArray's
       own (value.h). dirty_cards indexes map.dense[] by its DENSE index (stable across ordinary
       insert/update; hashtable_remove's swap-compaction invalidates it, which is why
       collection.delete sets dirty_all rather than trying to shift the affected bit). */
    bool dirty_all;
    unsigned int dirty_cards_bytes;
    HashTable map;
    unsigned char* dirty_cards;
    /* Bounds the actual set-bit range since the last clear -- see AerArray's own comment (value.h)
       for why this is needed on top of dirty_cards itself. */
    unsigned int dirty_min_byte, dirty_max_byte;
};
_Static_assert(offsetof(struct AerDict, gc_state) == 0, "pool.c assumes gc_state is byte 0");

typedef enum {
    /* Binary arithmetic */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_FLOOR_DIV,

    /* Binary comparison */
    OP_EQ,
    OP_NEQ,
    OP_LT,
    OP_GT,
    OP_LTE,
    OP_GTE,
    OP_IN, /* key in dict → key existence; value in array → element scan */

    /* Never dispatched -- parser tags for `&&`/`||`, which compile to short-circuit jumps. */
    OP_AND,
    OP_OR,
    OP_PIPE, /* never dispatched either -- `x |> f(args)` desugars to a call at parse time; kept as a lookup-table tag only, same reason as OP_AND/OP_OR */

    /* Binary bitwise */
    OP_BITWISE_AND,
    OP_BITWISE_OR,
    OP_BITWISE_XOR,
    OP_LSHIFT,
    OP_RSHIFT,

    /* Unary */
    OP_NEGATE,
    OP_NOT,
    OP_BITWISE_NOT,

    /* OP_JUMP's handler only touches vm->ip, so register opcodes reuse it for unconditional jumps. */
    OP_JUMP, /* operand: absolute code index */

    /* Struct definitions only -- instantiation/field access are register opcodes below. */
    OP_DEFINE_STRUCT, /* operands: name pool idx, field count, then that many (field-name, default-value) pool-idx pairs -- registers a Shape in the chunk's shape table */

    /* Misc */
    OP_TO_STR, /* used as OP_UNARY's unary_op tag (string interpolation's value-to-string step) -- see vm_to_str() */
    OP_HALT,

    /* ---- register-VM opcodes (packed encoding: see PACK3/PACK_BINARY below). */
    OP_LOADK, /* operands: dest_reg, pool_idx -- registers[dest_reg] = chunk pool constant */
    OP_MOVE, /* operands: dest_reg, src_reg -- registers[dest_reg] = registers[src_reg] */
    /* dest_reg, src_reg -- dest = (src is a Result); gates |>'s short-circuit (compile_pipe). */
    OP_IS_RESULT,

    /* Condition is read straight from a register/RK constant, never popped. */
    OP_JUMP_IF_FALSE_REG, /* operands: reg, target -- jump to target if registers[reg] is falsy */

    /* Calls -- per-call register windowing (CallFrame): each call gets an isolated bank. */
    OP_CALL, /* dest_reg, callee_offset, arg_reg_base, arg_count -- copies args into the
                     callee frame, saves return address + dest, jumps */
    OP_RETURN, /* src_reg (callee frame) -- result to caller's dest_reg, pops the frame, jumps back */

    /* Call through a runtime function value in a register; fills omitted trailing args from defaults. */
    OP_CALL_VALUE, /* dest_reg, arg_reg_base, arg_count, callee_reg */

    /* `return f(args)` with nothing wrapping the call: the already-emitted OP_CALL word's opcode
       byte is patched in place. Same dispatch label as OP_CALL; overwrites the CURRENT frame's
       registers[0..arg_count) (arg registers are always temps, above any local) and jumps --
       call_depth/dest_reg/return_ip stay untouched. */
    OP_TAIL_CALL, /* same operands as OP_CALL; dest_reg is unused (ignored) here */
    OP_TAIL_CALL_VALUE, /* same operands as OP_CALL_VALUE; dest_reg is unused (ignored) here */

    /* Bridges to the stack-based stdlib dispatch: push args from registers, call aer_*_call(),
       pop the one result into dest_reg. Stack-neutral. */
    /* One packed word (dest, module_idx, fn_idx, arg base/count) + two trailing words:
       parse-time-resolved module_id and fn_id (CALL_MODULE_DYNAMIC = host/file module,
       resolved by name at runtime). */
    OP_CALL_MODULE,

    /* Bare global builtins (length/print/type/assert/panic/Result): bridges to vm_call_builtin()
       via a small local array. Trailing word: builtin_id. */
    OP_CALL_BUILTIN, /* operands: dest_reg, name_pool_idx, arg_reg_base, arg_count, builtin_id */

    /* Registers can hold heap values -- mark_vm_roots scans all of registers[] per live frame. */
    OP_ARRAY_NEW, /* dest_reg, item_reg_base, item_count */
    OP_INDEX_GET, /* dest_reg, arr_reg, rk_idx -- via vm_index_get_compute */
    OP_INDEX_SET, /* arr_reg, rk_idx, rk_val -- via vm_index_set_compute (includes the write barrier) */

    /* Typed-array-only OP_INDEX_GET/SET, same word layout, emitted only where index_safe_unchecked
       (parser.c) proved the index in range for the whole enclosing loop. elem_kind is read at
       runtime from the array, so one opcode covers every element kind. The TYPE_TYPED_ARRAY tag is
       still checked -- only the index-range half of the safety argument is a compile-time fact. */
    OP_TYPED_INDEX_GET_UNCHECKED, /* dest_reg, arr_reg, rk_idx */
    OP_TYPED_INDEX_SET_UNCHECKED, /* arr_reg, rk_idx, rk_val */

    /* `a, b = expr`: Result/array unpacks normally, anything else becomes (value, null). */
    OP_DESTRUCTURE, /* target0_reg, target1_reg, src_reg */

    /* Slicing (array or string); a missing bound compiles to an RK null constant. */
    OP_SLICE_GET, /* dest_reg, arr_reg, rk_start, rk_end */

    /* OP_ARRAY_NEW's bulk-copy applied to (key,val) register pairs; same key validation and
       owned-key discipline as stack dict construction. */
    OP_DICT_NEW, /* dest_reg, pair_reg_base, pair_count -- key at base+2*i, val at +2*i+1 */

    /* Single-variable iteration; break is a plain jump, no cleanup needed. */
    OP_ITER_NEXT_ARRAY, /* col_reg, idx_reg, item_dest_reg, end_target -- despite the name, also accepts a dict
                              (yields keys) or string (yields chars) */

    /* Dict-only two-variable form; val_dest_reg needs its own word (only 3 narrow fields pack). */
    OP_ITER_NEXT_PAIR, /* col_reg, idx_reg, key_dest_reg, val_dest_reg, end_target -- key is a fresh owned string */

    /* Rotated range-for (Lua's FORLOOP shape): PREP runs once before the loop, LOOP at the loop
       BOTTOM is itself the back-edge (no OP_JUMP). PREP repurposes end_reg/step_reg into a
       countdown + signed step -- safe only because cur/end/step are loop-owned snapshots
       (arg_materialize), never aliases, so LOOP never re-validates their types. */
    OP_ITER_RANGE_PREP, /* cur, end, step, item_dest, empty_target -- validates ints (the only place), computes the
                               iteration count via ceiling division; count 0 jumps to empty_target
                               untouched; else end_reg = count-1, step_reg = signed step,
                               item_dest = cur */
    OP_ITER_RANGE_LOOP, /* cur, remaining (was end), signed_step (was step), item_dest, body_target (already-resolved
                               address) -- countdown 0: fall through to the exit; else advance
                               cur/item_dest, decrement, branch back to body_target */

    /* Instantiation/field get/set need register operands (definitions reuse OP_DEFINE_STRUCT). */
    OP_STRUCT_NEW, /* dest_reg, type_name_pool_idx, arg_reg_base, arg_count -- arity-checked, trailing
                          fields default-filled */
    OP_FIELD_GET, /* dest_reg, struct_reg, field_name_pool_idx */
    OP_FIELD_SET, /* struct_reg, field_name_pool_idx, rk_val -- includes the gc_barrier_array call */

    /* `[value; count]` repeat-literal. Evaluates the fill expression once into fill_reg, then
       branches on its runtime type: TYPE_STRUCT builds a packed array copying that instance's own
       field values, TYPE_INTEGER/TYPE_REAL builds a typed array, anything else is a runtime error.
       narrow_flag (0 none, 1 int32, 2 float32) is parse-time only -- set solely when the fill was
       written as an `i`/`f` suffixed literal in this position, never derived from a runtime value. */
    OP_ARRAY_REPEAT, /* word0: dest_reg, fill_reg, narrow_flag -- word1: rk_count (RK16) */

    /* Fused `obj[index].field` get/set -- packed arrays have no standalone element reference, so
       the parser emits these only for the exact `expr[index].field` pattern. Ordinary arrays
       take the same opcodes with behavior identical to the old two-step sequence. */
    OP_INDEX_FIELD_GET, /* operands: dest_reg, obj_reg, field_name_pool_idx, rk_idx */
    OP_INDEX_FIELD_SET, /* operands: obj_reg, field_name_pool_idx, rk_idx, rk_val */

    /* `obj[index].field OP= rk_rhs` -- resolves the index+field exactly once (one dispatch), instead
       of the OP_INDEX_FIELD_GET (read) + OP_INDEX_FIELD_SET (a second, redundant index+field
       resolution just to write the same slot back) pair this used to compile to. This is the
       pattern nbody-style code hits constantly (`bodies[j].vx += dx * mi`). */
    OP_INDEX_FIELD_COMPOUND, /* operands: obj_reg, field_name_pool_idx, rk_idx, bin_op, rk_rhs */

    /* unary_op reuses OP_NEGATE/OP_NOT/OP_BITWISE_NOT/OP_TO_STR as its tag, like bin_op. */
    OP_UNARY, /* dest_reg, unary_op, rk_operand -- also folds OP_TO_STR (interpolation) via vm_to_str */

    /* integer(x)/float(x)/boolean(x) via vm_cast; string(x) is OP_UNARY's TO_STR instead (see
       emit_primitive_cast, parser.c) -- struct shape-checking is type(x) == "Name", no opcode
       of its own needed. */
    OP_CAST, /* dest_reg, cast_type, rk_operand */

    /* One opcode for both argument orders of `x OP y.field`. parse_binary_ops truncates the
       just-emitted OP_FIELD_GET, re-encodes it here, and canonicalizes the operator so the field is
       always the left operand -- free for the 11 commutative-or-flippable operators. The 7
       order-sensitive ones don't fuse in that order at all. */
    OP_FIELD_BINARY, /* dest_reg, struct_reg, field_name_pool_idx, bin_op, rk_rhs */

    /* `struct.field OP= rhs` -- reads, computes, and writes back in one dispatch, one
       vm_resolve_field call. Before this existed, the parser emitted OP_FIELD_BINARY (read+compute
       into a temp) followed by a separate OP_FIELD_SET (a second, redundant field resolution just
       to write the same field back) -- two inline-cache lookups for one logical operation. Same
       operand shape as OP_FIELD_BINARY (dest slot unused, no destination register needed). */
    OP_FIELD_COMPOUND, /* struct_reg, field_name_pool_idx, bin_op, rk_rhs */

    /* Fuses `(A op1 B) op2 C` written as one expression: parse_binary_ops truncates the inner op and
       re-encodes it alongside op2/C. Fuses only when A/B/C turn out to be matching typed arrays at
       runtime; anything else computes the same value unfused. */
    OP_TYPED_ARRAY_CHAIN2, /* dest_reg, a_reg, b_reg; word1: op1(hi16)/c_reg(lo16); word2: op2 */

    /* Shell mode: a bare statement's non-null result is printed. */
    OP_PRINT_REPL, /* operand: src_reg -- prints registers[src_reg] unless it's TYPE_NULL */

    /* Raw (unboxed) arithmetic on provably-monotype locals; operands are raw_ints/raw_reals slot
       indices, no RK encoding. Comparisons produce a boxed boolean; OP_BOX_* is the only bridge
       back to registers[]. */
    OP_RAW_LOAD_INT,
    OP_RAW_LOAD_REAL,
    OP_RAW_ADD_INT,
    OP_RAW_SUB_INT,
    OP_RAW_MUL_INT,
    OP_RAW_DIV_INT,
    OP_RAW_MOD_INT,
    OP_RAW_FLOOR_DIV_INT,
    OP_RAW_ADD_REAL,
    OP_RAW_SUB_REAL,
    OP_RAW_MUL_REAL,
    OP_RAW_DIV_REAL,
    /* `x += a*b` / `x -= a*b` on raw real locals, in place, collapsing the RHS's MUL and this ADD/SUB
       into one dispatch. Still two roundings, not a hardware FMA, so results are bit-identical to
       the unfused pair -- only a dispatch is removed. */
    OP_RAW_FMA_REAL,
    OP_RAW_FMS_REAL,
    OP_RAW_LT_INT,
    OP_RAW_LTE_INT,
    OP_RAW_LT_REAL,
    OP_RAW_LTE_REAL,
    OP_BOX_INT,
    OP_BOX_REAL,
    /* Raw-to-raw copy -- OP_MOVE's analog for raw slots. */
    OP_RAW_MOVE_INT,
    OP_RAW_MOVE_REAL,
    /* In-place accumulation of a BOXED value into a raw slot (`e += <boxed expr>`); runtime tag
       check, ADD/SUB/MUL only. */
    OP_RAW_ADD_INT_BOXED,
    OP_RAW_SUB_INT_BOXED,
    OP_RAW_MUL_INT_BOXED,
    OP_RAW_ADD_REAL_BOXED,
    OP_RAW_SUB_REAL_BOXED,
    OP_RAW_MUL_REAL_BOXED,
    /* Non-destructive form of the _BOXED family: raw_reals[dest] = raw_reals[src] <op> unbox(reg),
       leaving src untouched. Used where the raw operand is a permanent slot that must survive, which
       the in-place family would need a defensive OP_RAW_MOVE_REAL to protect. Real only, matching
       the promotion restriction; ADD/MUL only, since nothing emits an order-sensitive variant. */
    OP_RAW_ADD_REAL_BOXED_TO,
    OP_RAW_MUL_REAL_BOXED_TO,
    /* Raw-vs-boxed comparison producing a boxed boolean -- removes the OP_BOX_INT that dominated
       `for i <= limit:`-shaped loops. Not in-place. The boxed operand is RK8, so a literal bound
       (`x*x + y*y > 4.0`) is read from the pool rather than reloaded into a register each pass. */
    OP_RAW_LT_INT_BOXED,
    OP_RAW_GT_INT_BOXED,
    OP_RAW_LTE_INT_BOXED,
    OP_RAW_GTE_INT_BOXED,
    OP_RAW_LT_REAL_BOXED,
    OP_RAW_GT_REAL_BOXED,
    OP_RAW_LTE_REAL_BOXED,
    OP_RAW_GTE_REAL_BOXED,
    /* Pool fallback for literals outside the old 20-bit immediate; kept as a distinct opcode
       (rather than widening OP_RAW_LOAD_INT's own immediate) since the fixed-width redesign below
       gives OP_RAW_LOAD_INT a full 32-bit immediate anyway -- this opcode now only exists for
       pool-sourced values that don't fit an int32 (rare, kept for parser-side symmetry with
       OP_RAW_LOAD_REAL). */
    OP_RAW_LOAD_INT_POOL,

    /* Shape-specialized field access -- only ever emitted into a ChunkFunction's SPECIALIZED body
       (see ChunkFunction.specializations below), where the compiler has proven (via a runtime shape
       observed at a call site, then substituted in as compile-time-known for one recompile) which
       Shape a parameter has. The field's byte offset is then a compile-time constant -- no
       vm_resolve_field_by_shape call, no boxed AerVal ever constructed. Never appears in a
       function's ordinary (generic, always-present) body. */
    OP_INDEX_FIELD_GET_RAW_INT,
    OP_INDEX_FIELD_GET_RAW_REAL,
    OP_FIELD_GET_RAW_INT,
    OP_FIELD_GET_RAW_REAL,
    OP_INDEX_FIELD_SET_RAW_INT,
    OP_INDEX_FIELD_SET_RAW_REAL,
    OP_FIELD_SET_RAW_INT,
    OP_FIELD_SET_RAW_REAL,
    /* Compound assignment whose RHS already resolved to a raw value: reads the field raw, applies
       the op against the raw rhs, writes back raw, in one dispatch with neither side boxed and no
       runtime field lookup. ADD/SUB/MUL only; /=, %=, //= fall back to the generic opcode. */
    OP_FIELD_COMPOUND_RAW_INT,
    OP_FIELD_COMPOUND_RAW_REAL,
    OP_INDEX_FIELD_COMPOUND_RAW_INT,
    OP_INDEX_FIELD_COMPOUND_RAW_REAL,

    /* _UNCHECKED counterparts of the INDEX_FIELD_*_RAW_INT/REAL opcodes above: same compile-time
       offset, but skip vm_packed_raw_elem's index-type, negative-adjust, bounds and null checks.
       Emitted only where the index is provably a `for i in 0..n:` loop variable with n proven ==
       length(this same array) and unreassigned. Shape is still guarded by reg_known_shape, so only
       the index-safety half is removed. */
    OP_INDEX_FIELD_GET_RAW_INT_UNCHECKED,
    OP_INDEX_FIELD_GET_RAW_REAL_UNCHECKED,
    OP_INDEX_FIELD_SET_RAW_INT_UNCHECKED,
    OP_INDEX_FIELD_SET_RAW_REAL_UNCHECKED,
    OP_INDEX_FIELD_COMPOUND_RAW_INT_UNCHECKED,
    OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED,

    /* Narrow (int32/float32) counterparts of the raw field-access family: storage is 4 bytes, but the
       compute side is unchanged -- values widen into ordinary raw slots on read and narrow on write,
       so every raw arithmetic opcode is untouched. Unlike the boxed path these do not range-check on
       overflow, matching the "raw means unchecked" tradeoff every other raw opcode makes. */
    OP_INDEX_FIELD_GET_RAW_INT32,
    OP_INDEX_FIELD_GET_RAW_FLOAT32,
    OP_FIELD_GET_RAW_INT32,
    OP_FIELD_GET_RAW_FLOAT32,
    OP_INDEX_FIELD_SET_RAW_INT32,
    OP_INDEX_FIELD_SET_RAW_FLOAT32,
    OP_FIELD_SET_RAW_INT32,
    OP_FIELD_SET_RAW_FLOAT32,
    OP_FIELD_COMPOUND_RAW_INT32,
    OP_FIELD_COMPOUND_RAW_FLOAT32,
    OP_INDEX_FIELD_COMPOUND_RAW_INT32,
    OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32,

    /* _UNCHECKED counterparts of the narrow INDEX_FIELD_*_RAW_INT32/FLOAT32 opcodes above -- same
       relationship the wide _UNCHECKED family (above) has to its own checked counterparts: only
       the index-safety half of vm_packed_raw_elem's checks is skipped, gated behind the exact same
       index_safe_unchecked proof, narrow storage width unaffected. */
    OP_INDEX_FIELD_GET_RAW_INT32_UNCHECKED,
    OP_INDEX_FIELD_GET_RAW_FLOAT32_UNCHECKED,
    OP_INDEX_FIELD_SET_RAW_INT32_UNCHECKED,
    OP_INDEX_FIELD_SET_RAW_FLOAT32_UNCHECKED,
    OP_INDEX_FIELD_COMPOUND_RAW_INT32_UNCHECKED,
    OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED,

    /* Emitted once per raw-bound parameter at the start of a raw-numeric variant: reads the boxed
       AerVal the caller placed in that register -- the calling convention always copies arguments
       boxed -- and copies its payload into a raw slot. No tag check needed, unlike every other
       raw-vs-boxed opcode: lbl_call already proved the type before jumping here. The rest of the
       body then treats it as an ordinary raw local. */
    OP_UNBOX_PARAM_INT,
    OP_UNBOX_PARAM_REAL,

    /* A bare comparison forming an entire if/while condition collapses with its
       OP_JUMP_IF_FALSE_REG into one dispatch -- the boolean was only ever read once, immediately.
       Only the 6 plain boxed comparisons (raw ones have faster opcodes already) and only when
       nothing else is emitted around the comparison; anything wrapped falls back unfused.
       word0 = PACK3(op, unused, rk_lhs8, rk_rhs8); word1 = jump target. */
    OP_EQ_JUMP_IF_FALSE,
    OP_NEQ_JUMP_IF_FALSE,
    OP_LT_JUMP_IF_FALSE,
    OP_GT_JUMP_IF_FALSE,
    OP_LTE_JUMP_IF_FALSE,
    OP_GTE_JUMP_IF_FALSE,

    /* OP_LT_JUMP_IF_FALSE's fusion, for the raw-boxed int family only -- the other 12 raw
       comparison opcodes were measured earning nothing against the branch-predictor cost every
       added opcode imposes. Operand layout is the raw-boxed int opcode's own word0 B/C verbatim,
       plus a trailing jump-target word. */
    OP_RAW_LT_INT_BOXED_JUMP_IF_FALSE,
    OP_RAW_GT_INT_BOXED_JUMP_IF_FALSE,
    OP_RAW_LTE_INT_BOXED_JUMP_IF_FALSE,
    OP_RAW_GTE_INT_BOXED_JUMP_IF_FALSE,
    /* Same fusion for a raw REAL local against a boxed value -- the shape `x*x + y*y > 4.0` in any
       float loop, which was three dispatches (load the constant, compare, branch) against the int
       path's two. */
    OP_RAW_LT_REAL_BOXED_JUMP_IF_FALSE,
    OP_RAW_GT_REAL_BOXED_JUMP_IF_FALSE,
    OP_RAW_LTE_REAL_BOXED_JUMP_IF_FALSE,
    OP_RAW_GTE_REAL_BOXED_JUMP_IF_FALSE,

    /* The same fusion where BOTH operands are raw slots -- `iter < max_iter` once max_iter is a raw
       parameter rather than a boxed one. No GT/GTE members: the raw-vs-raw family emits `a > b` as
       `b < a` with the slots already swapped (try_emit_binary_raw, parser.c), so four cover eight
       operators. Word0 B/C are the two slots; the jump target trails. */
    OP_RAW_LT_INT_JUMP_IF_FALSE,
    OP_RAW_LTE_INT_JUMP_IF_FALSE,
    OP_RAW_LT_REAL_JUMP_IF_FALSE,
    OP_RAW_LTE_REAL_JUMP_IF_FALSE,

    /* Builds one string from N parts in a single allocation. `"key_{n}"` used to compile to
       OP_LOADK + OP_TO_STR + OP_ADD -- three dispatches and two AerStrings, the second immediately
       garbage. Each part is a whole trailing word holding an RK16, so a constant segment needs no
       OP_LOADK and a non-string part is formatted straight into the result rather than through a
       throwaway string. Variable-length like OP_DEFINE_STRUCT: header word, then part_count words.
       Parts beyond INTERP_MAX_PARTS fall back to the old chain (parse_string_literal). */
    OP_INTERP, /* word0: dest_reg, part_count -- then part_count words, each one RK16 */

    /* `dict["key_{n}"]` read: formats the key into a stack buffer, hashes and probes with it, and
       never builds the AerString at all -- the string existed only to be hashed and discarded, and
       hashtable_get_hashed already takes raw bytes. Falls back to OP_INTERP's builder plus the
       general index path whenever the fast route cannot apply (non-dict receiver, a part that is
       itself a collection, or a key past INTERP_KEY_MAX). Same operand shape as OP_INTERP. */
    OP_INDEX_GET_INTERP, /* word0: dest_reg, obj_reg, part_count -- then part_count RK16 words */

    OP_OPCODE_COUNT_MARKER /* not a real opcode -- sizes the static assert below */
} Opcode;
_Static_assert(OP_OPCODE_COUNT_MARKER <= 256, "Opcode enum exceeds one byte — widen the opcode field");

/* RK bit: set = constant-pool index, clear = register (Lua's BITRK convention). This is the
   COMPILE-TIME-internal representation the parser passes around; never emitted directly into the
   instruction stream -- every emission site converts it to one of the WIRE encodings below. */
#define RK_CONST_FLAG (1 << 30)

/* Compiler-internal raw-slot tags (bits 28/29) -- never emitted into an instruction word. */
#define RK_RAW_INT_FLAG (1 << 29)
#define RK_RAW_REAL_FLAG (1 << 28)
#define RK_RAW_SLOT_MASK 0x1F

/* Per-frame register bank size; a register index must stay within RK8's 7 index bits with zero
   headroom to spare -- see RK8 below. */
#define FRAME_REGISTERS 128

/* Raw slot counts -- stored as a full byte on the wire now (no bit-packing pressure), but the
   allocator ceiling itself is unchanged from the original design. */
#define RAW_REGISTERS_INT 32
#define RAW_REGISTERS_REAL 32

/* Fixed-width, word-granular instruction encoding: every instruction is one or more 32-bit words,
   the shape (1-word, 2-word, ...) fixed per opcode at compile time -- never a variable byte count.
   See ARCHITECTURE.md §3.1-3.2 for the full field vocabulary (PACK3/PACK2/PACK1, RK8, RK16,
   PACK_2X16) and why this design was chosen over a wider bit-packed word. */

/* op(8) | A(8) | B(8) | C(8), low byte first -- generic 1-4 byte-field packer, unchanged in spirit
   from the old scheme's own PACK3 (which already only used the low 32 bits of its wider word). */
#define PACK3(op, a, b, cc)                                                                                  \
    (((uint32_t)(op)&0xFF) | (((uint32_t)(a)&0xFF) << 8) | (((uint32_t)(b)&0xFF) << 16) |                    \
     (((uint32_t)(cc)&0xFF) << 24))
#define PACK2(op, a, b) PACK3(op, a, b, 0)
#define PACK1(op, a) PACK3(op, a, 0, 0)
#define UNPACK_A(word) (((word) >> 8) & 0xFF)
#define UNPACK_B(word) (((word) >> 16) & 0xFF)
#define UNPACK_C(word) (((word) >> 24) & 0xFF)

/* Packs two independent 16-bit fields into one word -- used for word1-style "two wide fields,
   no room for anything else" shapes (e.g. field_idx + an RK16 operand). */
#define PACK_2X16(hi, lo) ((((uint32_t)(hi)&0xFFFF) << 16) | ((uint32_t)(lo)&0xFFFF))
#define UNPACK_2X16_HI(word) (((word) >> 16) & 0xFFFF)
#define UNPACK_2X16_LO(word) ((word)&0xFFFF)

/* RK8: 1 flag bit + 7 index bits. A register index is always < FRAME_REGISTERS(128) by the time it
   reaches emission, so it fits with zero headroom; a constant-pool index past 127 must be hoisted
   into a scratch register first (parser.c's existing materialize(), unchanged). */
#define RK8_CONST_FLAG 0x80U
#define RK8_INDEX_MASK 0x7FU
#define RK8_MAX_INDEX 0x7F
static inline uint8_t pack_rk8(int rk) {
    if (rk & RK_CONST_FLAG)
        return (uint8_t)(RK8_CONST_FLAG | ((unsigned)(rk & ~RK_CONST_FLAG) & RK8_INDEX_MASK));
    return (uint8_t)((unsigned)rk & RK8_INDEX_MASK);
}
#define RK8_IS_CONST(b) ((b)&RK8_CONST_FLAG)
#define RK8_INDEX(b) ((b)&RK8_INDEX_MASK)

/* RK16: 1 flag bit + 15 index bits (32767 registers/constants direct) -- generous enough that no
   overflow path is needed anywhere it's used. */
#define RK16_CONST_FLAG (1U << 15)
#define RK16_INDEX_MASK 0x7FFFU
#define RK16_MAX_INDEX 0x7FFF
static inline uint16_t pack_rk16(int rk) {
    if (rk & RK_CONST_FLAG)
        return (uint16_t)(RK16_CONST_FLAG | ((unsigned)(rk & ~RK_CONST_FLAG) & RK16_INDEX_MASK));
    return (uint16_t)((unsigned)rk & RK16_INDEX_MASK);
}
#define RK16_IS_CONST(w) ((w)&RK16_CONST_FLAG)
#define RK16_INDEX(w) ((w)&RK16_INDEX_MASK)

/* type_name_idx / field_name_idx / module_idx / fn_idx / callee_offset / jump targets all get a
   full dedicated 32-bit word wherever this comment appears in the shapes below -- no packing, no
   guard needed, direct emit_u32-equivalent (a plain chunk_emit of the raw value). */

/* op(8) | a(8) | w16(16) -- one small field plus one 16-bit field, both in word0. Used by opcodes
   whose only two real fields are a register/small-count and one RK16/count16 value (OP_FIELD_SET,
   OP_INDEX_FIELD_SET's obj_reg+rk_idx half). */
#define PACK_OP_A_W16(op, a, w16)                                                                            \
    (((uint32_t)(op)&0xFF) | (((uint32_t)(a)&0xFF) << 8) | (((uint32_t)(w16)&0xFFFF) << 16))
#define UNPACK_W16(word) (((word) >> 16) & 0xFFFFU)

/* OP_DEFINE_STRUCT's header word: op(8) | name_idx(16) | field_count(8) -- name_idx sits in the
   middle (unlike PACK_OP_A_W16), so it gets its own macro rather than misusing that one. */
#define PACK_STRUCT_HEADER(name_idx, field_count)                                                            \
    (((uint32_t)(OP_DEFINE_STRUCT)&0xFF) | (((uint32_t)(name_idx)&0xFFFF) << 8) |                            \
     (((uint32_t)(field_count)&0xFF) << 24))
#define UNPACK_STRUCT_HEADER_NAME(word) (((word) >> 8) & 0xFFFFU)
#define UNPACK_STRUCT_HEADER_COUNT(word) (((word) >> 24) & 0xFFU)

/* OP_CAST operand values -- target type for `x as T` (T=string compiles to OP_TO_STR instead, since that conversion already existed). */
#define CAST_INTEGER 0
#define CAST_FLOAT 1
#define CAST_BOOLEAN 2

/* OP_CALL_MODULE's trailing module_id word, resolved at parse time; CALL_MODULE_DYNAMIC =
   host/file module, resolved by name at runtime. */
#define CALL_MODULE_MATH 0
#define CALL_MODULE_RANDOM 1
#define CALL_MODULE_STRING 2
#define CALL_MODULE_TIME 3
#define CALL_MODULE_JSON 4
#define CALL_MODULE_COLLECTION 5
#define CALL_MODULE_NET 6
#define CALL_MODULE_REGEX 7
#define CALL_MODULE_ACTOR 8
#define CALL_MODULE_SCHEDULER 9
#define CALL_MODULE_DYNAMIC 10

/* Second trailing word: fn_id within the module (each module owns a flat id space);
   FN_ID_UNKNOWN still errors by name, never misroutes to id 0. */
#define FN_ID_UNKNOWN (-1)

#define FN_MATH_SQRT 0
#define FN_MATH_POW 1
#define FN_MATH_FLOOR 2
#define FN_MATH_CEIL 3
#define FN_MATH_ABS 4
#define FN_MATH_MIN 5
#define FN_MATH_MAX 6
#define FN_MATH_SIN 7
#define FN_MATH_COS 8
#define FN_MATH_LOG 9
#define FN_MATH_LOG2 10
#define FN_MATH_LOG10 11
#define FN_MATH_PI 12
#define FN_MATH_ROUND 13
#define FN_MATH_TAN 14
#define FN_MATH_EXP 15

#define FN_RANDOM_RANDOM 0
#define FN_RANDOM_RANDINT 1
#define FN_RANDOM_SEED 2
#define FN_RANDOM_CHOICE 3
#define FN_RANDOM_SHUFFLE 4

#define FN_STRING_UPPER 0
#define FN_STRING_LOWER 1
#define FN_STRING_TRIM 2
#define FN_STRING_CONTAINS 3
#define FN_STRING_SPLIT 4
#define FN_STRING_STARTS_WITH 5
#define FN_STRING_ENDS_WITH 6
#define FN_STRING_REPEAT 7
#define FN_STRING_REPLACE 8
#define FN_STRING_JOIN 9
#define FN_STRING_INDEX_OF 10

#define FN_TIME_NOW 0
#define FN_TIME_STRFTIME 1
#define FN_TIME_SLEEP 2
#define FN_TIME_PARSE 3
#define FN_TIME_TO_PARTS 4
#define FN_TIME_FROM_PARTS 5

#define FN_JSON_ENCODE 0
#define FN_JSON_DECODE 1

#define FN_COLLECTION_APPEND 0
#define FN_COLLECTION_DELETE 1
#define FN_COLLECTION_COPY 2
#define FN_COLLECTION_INSERT 3
#define FN_COLLECTION_INDEX_OF 4
#define FN_COLLECTION_KEYS 5
#define FN_COLLECTION_SORT 6
#define FN_COLLECTION_RESERVE 7

#define FN_NET_CONNECT 0
#define FN_NET_SEND 1
#define FN_NET_RECV 2
#define FN_NET_CLOSE 3
#define FN_NET_LISTEN 4
#define FN_NET_ACCEPT 5

#define FN_REGEX_MATCH 0
#define FN_REGEX_FIND 1
#define FN_REGEX_REPLACE 2
#define FN_REGEX_FIND_ALL 3

#define FN_ACTOR_SPAWN 0
#define FN_ACTOR_SEND 1
#define FN_ACTOR_RECEIVE 2
#define FN_ACTOR_CALL 3

#define FN_SCHEDULER_ADD 0
#define FN_SCHEDULER_RUN 1

/* OP_CALL_BUILTIN's trailing builtin_id -- no DYNAMIC case; is_builtin_name gates every site. */
#define CALL_BUILTIN_LENGTH 0
#define CALL_BUILTIN_PRINT 1
#define CALL_BUILTIN_TYPE 2
#define CALL_BUILTIN_ASSERT 3
#define CALL_BUILTIN_PANIC 4
/* The only way AER source constructs a Result -- lets user functions join |>'s short-circuit. */
#define CALL_BUILTIN_RESULT 5

/* Parts in one OP_INTERP. Bounds the builder's stack scratch; a longer interpolation compiles to
   the ordinary concatenate chain instead, which has no limit. */
/* OP_ITER_RANGE_PREP's item_dest operand is a whole 32-bit word for what is an 8-bit register
   index, so the spare high bits carry a flag. GUARD_NONNEG means this loop's body was compiled
   with UNCHECKED indexing on the strength of a compile-time proof that the start is >= 0
   (Parser.reg_nonneg), so PREP verifies that once here instead of the body checking every index.
   Only reachable when integer overflow defeated the proof -- see 5.16k. */
#define RANGE_PREP_GUARD_NONNEG 0x80000000u
#define RANGE_PREP_ITEM_REG(w) ((int)((w)&0xFFu))

#define INTERP_MAX_PARTS 16

/* Longest interpolated dict key OP_INDEX_GET_INTERP will build without allocating. Anything longer
   falls back to the allocating path -- the buffer lives in a noinline helper's frame, never
   vm_run_slice's (see 5.16b). */
#define INTERP_KEY_MAX 256

#define MAX_STRUCT_FIELDS 16

/* A struct type's blueprint (field names in order + default literals); individually heap-allocated
   and never moved/realloc'd, so AerStruct.shape pointers stay valid as the shape table grows. */
struct Shape {
    unsigned int name; /* pool index of the struct's type name */
    unsigned int field_count;
    unsigned int field_names[MAX_STRUCT_FIELDS]; /* pool indices, declaration order       */
    AerVal field_defaults[MAX_STRUCT_FIELDS];
    /* TYPE_ANY = no declared type. A declared type is enforced once at FIELD_SET/construction,
       then trusted -- the fused opcodes skip the runtime check on that side. */
    ValueType field_types[MAX_STRUCT_FIELDS];
    /* True for an int/real field whose default carried an `i`/`f` suffix, selecting 4-byte storage
       instead of 8. Works the same for a plain instance or a packed-array element, since every
       access uses shape->instance_bytes as the stride. Meaningless for any other field kind. */
    bool field_narrow[MAX_STRUCT_FIELDS];
    /* Byte offset of each field within an instance's fields buffer (AerStruct.fields) -- a typed
       field (TYPE_ANY excluded) is stored RAW in 8 bytes (no tag; the type is this Shape's own
       static knowledge, never read from the instance) unless field_narrow marks it 4 instead, an
       untyped (TYPE_ANY) field stays a full boxed AerVal (16 bytes), since it can hold any value
       including a reference type the GC must trace. Computed once in OP_DEFINE_STRUCT's handler,
       right after field_types/field_narrow are known. See vm_struct_field_read. */
    unsigned int field_offsets[MAX_STRUCT_FIELDS];
    unsigned int instance_bytes; /* total size of the fields buffer -- sum of every field's width above */
};

/* A single struct instance -- its own type (TYPE_STRUCT), split out from AerArray specifically
   because sharing one C type/tag for "ordinary array" and "struct instance" meant every site
   handling TYPE_ARRAY had to remember to ask "but what if this is actually a struct" (one real
   site didn't -- for-x-in iteration silently walked a struct's fields with no shape check at all).
   No count/capacity: a struct's field count is always shape->field_count, fixed, never grows --
   carrying them the way the old shared AerArray design did was already dead weight. */
struct AerStruct {
    unsigned char gc_state; /* byte 0, same pool.c convention as every other pool-managed type */
    Shape* shape;
    unsigned char* fields; /* set to (char*)a + sizeof(AerStruct) at construction -- inline in
                                   the same pool cell, matching AerArray's own items pointer trick */
};
_Static_assert(offsetof(struct AerStruct, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* Reads/writes one struct field at its own byte offset -- raw (untagged, vm_packed_slot_read's
   scheme) for a typed field, a full boxed AerVal for a TYPE_ANY one. Shared by every struct-field
   opcode in vm.c plus json.encode's struct-serialization branch (aer_json.c), which is why these
   aren't file-static. */
AerVal vm_struct_field_read(AerStruct* s, unsigned int slot);

/* Byte width of one element: 4 for int32/float32, 8 for int64/float64. Not file-static -- gc.c's
   free_typed_array needs it too, to recompute a dying typed array's data-buffer size (count is
   already on the struct; the byte width isn't) before deciding whether it qualifies for the
   free-cache above. */
unsigned int vm_typed_elem_width(TypedArrayElemKind kind);

/* Which runtime shape a shape-sensitive parameter arrived as. STRUCT and PACKED_ARRAY carry a
   structural guarantee -- a struct's shape never changes, a packed array cannot hold mixed shapes
   -- so they need no per-access recheck. ARRAY_OF_STRUCTS carries none: a plain array may hold
   heterogeneous elements, so every call must re-verify uniformity first. */
typedef enum {
    SPEC_KIND_STRUCT,
    SPEC_KIND_PACKED_ARRAY,
    SPEC_KIND_ARRAY_OF_STRUCTS,
} SpecKind;

/* One already-compiled specialized body for a shape-sensitive function (ChunkFunction.
   specializations below) -- keyed by the Shape observed for its shape-sensitive parameter(s) at
   the point specialization was triggered. Each specialized body has its OWN max_registers/
   max_raw_ints/max_raw_reals peaks, independent of the generic body's (typically fewer boxed
   registers, more raw slots, since that's the whole point). */
#define SPEC_MAX_RAW_PARAMS 3

typedef struct {
    Shape* shape;
    SpecKind kind;
    unsigned int code_offset;
    unsigned int max_registers;
    unsigned int max_raw_ints;
    unsigned int max_raw_reals;

    /* Optional second specialized body for this shape that also binds up to SPEC_MAX_RAW_PARAMS
       numeric parameters as raw locals. Declines silently to this entry's baseline body when it
       doesn't fit -- never touches `megamorphic` or the shape table.
       raw_param_count: 0 = never attempted, -1 = attempted and permanently declined, >0 = compiled
       for exactly raw_param_regs/raw_param_types, reused only while observed types still match. */
    int raw_param_count;
    int raw_param_regs[SPEC_MAX_RAW_PARAMS];
    ValueType raw_param_types[SPEC_MAX_RAW_PARAMS];
    unsigned int raw_variant_code_offset;
    unsigned int raw_variant_max_registers;
    unsigned int raw_variant_max_raw_ints;
    unsigned int raw_variant_max_raw_reals;
} SpecEntry;
#define SPEC_MAX 4

/* Runtime-visible function registration -- outlives the parser tables so cross-module calls
   can find exports by name after compilation (same precedent as chunk->shapes[]). */
typedef struct {
    unsigned int name; /* pool index of the function's name */
    unsigned int code_offset;
    unsigned int arity;
    unsigned int min_arity;
    AerVal* defaults; /* (arity - min_arity) owned values or NULL; freed by chunk_free */
    /* Real peak register need, patched in after the body compiles; the FRAME_REGISTERS
       placeholder (read only by in-body self-reference) is never an under-allocation. */
    unsigned int max_registers;
    /* Same idea, for raw_ints[]/raw_reals[] -- placeholders are RAW_REGISTERS_INT/REAL (see
       chunk_add_function), never an under-allocation for the same in-body-self-reference reason. */
    unsigned int max_raw_ints;
    unsigned int max_raw_reals;

    /* Shape-specializing compilation (lazy, per-call-observed-shape recompiles) -- see
       vm_call_resolve_specialization (vm.c). Bit i set = parameter i was seen used as the base of a
       struct-field access (directly, or through a one-hop plain-local alias) during the ordinary
       compile; folded in at function-exit, same moment max_registers/max_raw_ints/max_raw_reals are
       captured. Zero means this function is never specialized, and lbl_call skips the lookup. */
    unsigned int shape_sensitive_mask;
/* Set instead of a parameter bit when no parameter is shape-sensitive but the body composed a raw
   local with a boxed value -- work that goes raw once the numeric parameters do. It rides in this
   mask rather than a field of its own so lbl_call's gate stays the one already-fetched test it is;
   parameter bits are capped at 31 to keep the top one free. */
#define SHAPE_MASK_NUMERIC_ONLY (1u << 31)
    /* Owned copy of the source text spanning from '(' through the end of the body -- NULL unless
       shape_sensitive_mask != 0. Needed to re-invoke the parser later (long after the original
       parse() call returned) with a specific parameter's Shape substituted in as compile-time-known.
       An owned copy, not a retained lexer pointer: the REPL/aer_run_source path frees and replaces
       its one static source buffer on the NEXT call, which would dangle a raw pointer the instant
       a second such call happens -- exactly when a lazy specialization might fire. */
    char* source_span;
    unsigned int source_span_len;
    /* Absolute source line the span's first character ('(') sits on -- passed to lexer_begin_span
       so a specialization recompile's bytecode gets tagged with true source line numbers instead
       of ones relative to the span's own start (a real bug: an error inside a specialized body used
       to report a line number offset by however many lines precede the function in its file). */
    unsigned int source_span_line;
    /* Small, bounded table of already-compiled specialized bodies, keyed by the shape observed for
       this function's shape-sensitive parameter(s). Checked (via the call site's own
       CallSpecCacheEntry first, then this table on a miss) before recompiling for a never-before-seen
       shape. megamorphic permanently stops specializing once the table fills, falling back to the
       generic body via code_offset above for every further call. */
    /* NULL until this function first specializes, which most never do. Inline, the SPEC_MAX
       entries were 83% of this struct (272 of 328 bytes) and spread every ChunkFunction across six
       cache lines for the handful of fields lbl_call reads per call. Allocated once at full
       SPEC_MAX size and never grown, so the SpecEntry* CallSpecCacheEntry caches stays valid. */
    SpecEntry* specializations;
    int specialization_count;
    bool megamorphic;
} ChunkFunction;

/* ------------------------------------------------------------------ */
/* Bytecode chunk                                                       */
/* Flat 32-bit word array: one descriptor word (opcode + narrow packed fields) plus, per opcode's
   own fixed shape, zero or more trailing wide-field words -- see the fixed-width encoding comment
   above chunk_emit_word/read_word (vm.c). */
/* ------------------------------------------------------------------ */

/* One per-callsite field-cache entry -- see Chunk.field_cache's own comment below. Caches offset
   and ftype alongside slot, not just slot: both are pure functions of (shape, slot), so once the
   shape comparison confirms a cache hit, re-deriving them from shape->field_offsets[slot]/
   field_types[slot] on every single access was a second, avoidable indirection through Shape --
   this makes a cache hit read them from the entry itself (already touched for the shape check)
   instead. */
typedef struct {
    Shape* shape;
    int slot;
    unsigned int offset;
    ValueType ftype;
    bool narrow; /* same reasoning as offset/ftype above -- cached, not re-derived from shape */
} FieldCacheEntry;

/* One per-callsite specialization-dispatch cache entry (lbl_call) -- same monomorphic-inline-
   cache idea as FieldCacheEntry above: the last shape seen AT THIS CALL SITE, checked before
   falling into the callee's own (function-wide) SpecEntry table on a miss. last_shape == NULL
   means never populated. */
typedef struct {
    Shape* last_shape;
    unsigned int last_code_offset;
    unsigned int last_max_registers;
    unsigned int last_max_raw_ints;
    unsigned int last_max_raw_reals;
    /* ARRAY_OF_STRUCTS only: the array (by pointer) and AerArray.generation at the last successful
       homogeneity scan. A later call with both unchanged can skip re-scanning. NULL/0 means never
       verified; only ever set on a scan that fully succeeded, so a failed one never poisons a
       later genuinely-uniform call. */
    AerArray* last_verified_array;
    unsigned int last_verified_generation;
    /* Direct pointer into target_f->specializations[] for whichever SpecEntry last_shape matched --
       lets a site-cache HIT still reach that entry's raw-numeric-variant fields (raw_param_regs/
       types, raw_variant_code_offset, ...) without needing its own full duplicate of them here.
       Stable for the chunk's life once set: SpecEntry lives inside a FIXED-SIZE array
       (ChunkFunction.specializations[SPEC_MAX], never reallocated/grown), the same stability
       assumption lbl_call already relies on for target_f itself across a specialization recompile. */
    SpecEntry* last_entry;
} CallSpecCacheEntry;

typedef struct {
    uint32_t* code;
    unsigned int count, capacity;

    char*
        source_filename; /* owned copy; NULL for a chunk with no real file (e.g. aer_run_source on a raw string) */

    AerVal* pool; /* constants and variable names -- all deduplicated by value */
    unsigned int pool_count, pool_cap;

    /* Numeric constants as raw scalars, for the raw opcode families. Those opcodes name their
       operand type (OP_RAW_LT_INT_BOXED is int by construction), so a const-flagged operand can
       index these directly -- no tag to check, and no encoding bit spent saying so. Kept separate
       from pool[] rather than replacing entries in it: a literal reached by both a raw and a boxed
       path needs both forms, and RK8's 7 index bits make a dense numeric-only table reach further. */
    int64_t* rawk_i;
    unsigned int rawk_i_count, rawk_i_cap;
    double* rawk_d;
    unsigned int rawk_d_count, rawk_d_cap;

    /* name -> pool index, for O(1) dedup of TYPE_STRING pool entries (chunk_add_pool, vm.c); owns an independent copy of each key. */
    HashTable name_index;

    /* Struct type registry appended to by OP_DEFINE_STRUCT; redeclaring a struct appends rather than replaces so old Shape pointers stay valid, and chunk_find_shape() searches newest-first. */
    Shape** shapes;
    unsigned int shape_count, shape_cap;

    /* Function registry -- appended by func_register, searched newest-first. */
    ChunkFunction* functions;
    unsigned int function_count, function_cap;

    /* Module names from `import`, parse-time only, tracked on the chunk so a later REPL line still recognizes a module an earlier line imported. */
    char** imported_modules;
    unsigned int import_count, import_cap;

    /* Offset -> source line, one entry per statement; strictly increasing (binary-searched);
       rolled back with bytecode on parse-error recovery. */
    unsigned int* line_mark_offsets;
    unsigned int* line_mark_lines;
    unsigned int line_mark_count, line_mark_cap;

    /* Per-site inline cache for FIELD_GET/SET: last Shape* + resolved slot. Monomorphic sites
       skip the name scan; polymorphic sites just miss. Shape* is never reallocated, so a cached
       pointer can't go stale. */
    FieldCacheEntry* field_cache;
    unsigned int field_cache_cap;

    /* Per-site inline cache for lbl_call's specialization dispatch, same growth/addressing idiom as
       field_cache above (sized to c->count, indexed by bytecode word offset). */
    CallSpecCacheEntry* call_spec_cache;
    unsigned int call_spec_cache_cap;

    /* Struct-name -> Shape*, keyed by the name's POOL index rather than the call site, since
       chunk_add_pool dedups strings and every site building the same struct shares that index.
       Without it every single struct construction re-ran chunk_find_shape's newest-first scan with
       a strcmp per shape -- 1.63% of bench/binary_trees.aer was strcmp alone. Cleared wholesale
       when a shape is registered (a redeclare must not keep resolving to the older Shape); struct
       definitions are rare and constructions are not, which is the whole trade. */
    Shape** shape_by_name;
    unsigned int shape_by_name_cap;

#ifdef AER_DEBUG_TOOLS
    /* Per-word dispatch counters (debug-tools only); only opcode words increment. */
    uint64_t* debug_hits;
    unsigned int debug_hits_cap;
#endif
} Chunk;

/* ------------------------------------------------------------------ */
/* Per-VM heap -- every pool a VM allocates from, plus its own GC state. */
/* ------------------------------------------------------------------ */

typedef enum { REMEMBERED_ARRAY, REMEMBERED_DICT, REMEMBERED_STRUCT } RememberedKind;
typedef struct {
    void* ptr;
    RememberedKind kind;
} RememberedEntry;

/* Explicit growable worklist, not C recursion, since user data structures have no depth limit;
   pool_mark's "already marked" return terminates cycles correctly. */
typedef struct {
    AerVal* items;
    unsigned int count, cap;
} MarkWorklist;

/* Size-keyed free-list cache for typed-array data buffers -- the variable-sized payload, not the
   header, which is pooled like every other GC object. Repeated same-shape transforms would
   otherwise churn malloc/free every pass at a size that never changes. Bounded by a handful of
   linearly-scanned slots and a per-buffer size ceiling, so one huge allocation cannot sit here
   retaining memory. Checked by vm_new_typed_array, populated by free_typed_array. */
#define TYPED_ARRAY_FREE_CACHE_SLOTS 8
#define TYPED_ARRAY_FREE_CACHE_MAX_BYTES (4u * 1024 * 1024)

typedef struct {
    size_t size; /* 0 = empty slot */
    unsigned char* ptr;
} TypedArrayFreeSlot;

/* Size-classed slab pools for struct instances, keyed by Shape.instance_bytes. One pool sized for
   the MAX_STRUCT_FIELDS worst case cost 268 bytes/cell regardless of a shape's real field count,
   wasting ~78% on a 3-field node. Mirrors hashtable.c's KEY_TIER_SIZE scheme; the tier table lives
   in vm.c. The largest tier covers the worst case, so no malloc fallback is needed here. */
#define STRUCT_PAYLOAD_TIER_COUNT 5

/* Size-classed pools for the separately-owned payload of a string too long to inline. Same scheme
   as struct_pools above and hashtable.c's key pools, including their plain-malloc fallback past the
   largest tier, since a string's length is unbounded. */
#define STRING_PAYLOAD_TIER_COUNT 4

typedef struct {
    Pool string_pool, array_pool, dict_pool, function_pool, packed_array_pool, typed_array_pool, result_pool;
    Pool struct_pools[STRUCT_PAYLOAD_TIER_COUNT];
    Pool string_payload_pools[STRING_PAYLOAD_TIER_COUNT];
    bool pools_initialized;

    TypedArrayFreeSlot typed_array_free_cache[TYPED_ARRAY_FREE_CACHE_SLOTS];

    /* Old objects a write barrier caught holding a young reference; entries are only ever
       added/deduped, never removed, and re-traced as extra roots on every minor collection
       thereafter. */
    RememberedEntry* remembered_set;
    unsigned int remembered_count, remembered_cap;
    /* Set once, forever, on the first real collection this heap ever runs -- before that,
       POOL_OLD can't be set anywhere, so the write barrier is provably a no-op. */
    bool gc_ever_collected;

    MarkWorklist gc_worklist;

    /* Tuning (overridable via aer_gc_configure()): minor_gc_threshold is total cells allocated
       across all pools since the last minor GC; major_gc_every_n_minor runs a major pass after
       that many minor ones. 0 for gc_live_cell_ceiling means unlimited (aer_gc_set_ceiling). */
    unsigned int minor_gc_threshold, major_gc_every_n_minor, gc_live_cell_ceiling;
    /* Floor for minor_gc_threshold, which gc_rescale_minor_threshold recomputes after every major
       collection as max(floor, live cell count) -- so a large mostly-static live heap gets a bigger
       nursery instead of re-tracing itself nearly as often. Never mutated by that rescale, only by
       aer_gc_configure, which is what lets the threshold shrink again rather than ratchet upward. */
    unsigned int minor_gc_threshold_floor;
    unsigned int minor_collections_run, major_collections_run, minor_since_major;
    int gc_suppress_depth;

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

#define VM_STACK_MAX 256
#define VM_CALL_MAX 64

/* Per-call register frame; lives in the VM struct so a nested module VM gets its own chain. */
typedef struct {
    /* The anonymous union pads sizeof(CallFrame) up to 64 without raising its ALIGNMENT. _Alignas(64)
       was the obvious way to get that size and is wrong: the requirement propagates to struct VM,
       which aer_module.c -- and any embedder -- allocates with plain malloc, only 8-byte aligned.
       Every module VM was undefined behaviour; UBSan caught it. Only the size matters here. */
    union {
        struct {
            /* Bump-pointer base into vm->register_stack -- a call is a pointer add, not an
               allocation. */
            AerVal* registers;
            /* Registers THIS frame reserved (callee's compile-time peak; FRAME_REGISTERS for frame
               0) -- read by the next push. */
            unsigned int frame_size;

            /* Raw unboxed scratch for the primitive pass -- never GC-scanned, never crosses a call
               boundary (only its boxed form does). Bump-pointer bases into
               vm->raw_int_stack/raw_real_stack rather than fixed inline arrays, which would cost
               every frame RAW_REGISTERS_INT+REAL slots whether or not it uses any raw locals. */
            int64_t* raw_ints;
            double* raw_reals;
            unsigned int raw_int_frame_size;
            unsigned int raw_real_frame_size;

            unsigned int return_ip; /* where to resume in the CALLER */
            int dest_reg; /* which of the CALLER's registers gets the return value */

            unsigned int code_offset; /* this frame's entry point, for stack traces; unset on frame 0 */
            unsigned int tail_calls_collapsed; /* tail calls collapsed since this frame's last real push */
            bool synthetic_entry; /* set by setup_call() -- return_ip isn't a real caller line */
        };
        char size_is_a_power_of_two[64];
    };
} CallFrame;
/* Indexing call_stack[] is `base + depth * sizeof(CallFrame)`, and at 44 bytes that compiled to a
   multiply plus a materialized constant per field on 32-bit ARM. A power of two makes it a shift. */
_Static_assert((sizeof(CallFrame) & (sizeof(CallFrame) - 1)) == 0,
               "CallFrame must stay a power of two -- see CALL_FRAME_PAD");
/* Keep these OUT of register_stack. mark_vm_roots scans a frame's whole register range including
   slots the callee has not written, which is safe only because register_stack has only ever held
   AerVals; interleaved raw words let the GC read a double as a pointer. Tried, segfaulted, and
   measured worse anyway -- see ARCHITECTURE 5.16f and tests/test_gc_raw_frames.aer. */

/* Regression guard: raw_ints/raw_reals used to be fixed inline arrays here (RAW_REGISTERS_INT +
   RAW_REGISTERS_REAL int64_t/double slots each), costing every single frame ~550+ bytes whether or
   not that function used any raw locals at all. They're pointers into a shared VM-level bump-pointer
   stack now (see raw_int_stack/raw_real_stack below) -- if this ever creeps back up near the old
   size, someone likely reintroduced fixed per-frame arrays instead of the shared-stack pattern. */
_Static_assert(sizeof(CallFrame) <= 96,
               "CallFrame grew unexpectedly large -- raw_ints/raw_reals should stay pointers into the shared "
               "VM-level raw_int_stack/raw_real_stack, not fixed inline per-frame arrays");

typedef struct {
    /* First. A Thumb-2 `ldr` reaches a 12-bit displacement, so a field past 4095 bytes needs its
       offset materialized into a register first, and call_stack itself is 4096 bytes. The active
       frame's registers/raw_ints/raw_reals used to be mirrored here too; they were a cache with no
       hot reader -- mark_vm_roots scans call_stack[f].registers, not the mirror -- so keeping it
       current cost three stores per call and three per return to serve only cold consumers. */
    int call_depth;

    Chunk* chunk;
    unsigned int ip;
    /* Where the currently-executing instruction is, for runtime error line lookup ONLY. A pointer
       rather than an offset because vm_run_slice writes it at every site that can raise, and an
       offset would force `code` to stay live across all of them just to do the subtraction. `ip`
       stays the offset: it is what SURVIVES, across a yield/resume and across the reparse a REPL
       line performs in between, which would leave any stored pointer dangling. */
    const uint32_t* error_pc;

    /* This VM's own heap -- every pool it allocates from, independent of every other VM's. */
    VmHeap heap;

    /* Per-VM capability toggles, seeded from the process-wide aer_io_enabled/aer_net_enabled
       defaults at vm_init AND every aer_run_source call (the REPL/embedding "run more code into an
       existing VM" entry point) -- see those externs' own comment below for why io/net moved here
       but import_enabled didn't. */
    bool io_enabled;
    bool net_enabled;

    CallFrame call_stack[VM_CALL_MAX];
    /* Scratch argument channel for bridging out of the register convention (stdlib/module calls).
       Deliberately AFTER call_stack: 4096 bytes in front of it pushed every CallFrame field past the
       12-bit displacement window, costing lbl_call eight materialized constants per call. Nothing in
       the dispatch loop's call path touches this array. */
    AerVal stack[VM_STACK_MAX];
    int stack_top;


    /* One shared register bank for the whole chain (calls bump a base pointer). Same worst-case
       size as a flat design, but the actually-touched working set is far smaller. */
    AerVal register_stack[VM_CALL_MAX * FRAME_REGISTERS];
    /* Same bump-pointer-bank idea as register_stack, for raw_ints[]/raw_reals[] -- see CallFrame's
       own comment for why this replaced per-frame fixed arrays. */
    int64_t raw_int_stack[VM_CALL_MAX * RAW_REGISTERS_INT];
    double raw_real_stack[VM_CALL_MAX * RAW_REGISTERS_REAL];
} VM;

/* Bounds-checked push/pop for native-module files, outside vm_run's PUSH()/POP() macros. */
static inline bool vm_stack_push(VM* vm, AerVal v) {
    if (vm->stack_top >= VM_STACK_MAX) {
        error("Stack overflow");
        return false;
    }
    vm->stack[vm->stack_top++] = v;
    return true;
}

static inline AerVal vm_stack_pop(VM* vm) {
    if (vm->stack_top <= 0) {
        error("Stack underflow");
        return aer_null();
    }
    return vm->stack[--vm->stack_top];
}

void chunk_init(Chunk* c);
void chunk_free(Chunk* c);
void chunk_emit(Chunk* c, uint32_t word);

/* Records that bytecode from `offset` onward belongs to source `line`, once per statement not instruction (see line_mark_offsets); no-op if offset doesn't strictly increase from the last mark. */
void chunk_mark_line(Chunk* c, unsigned int offset, unsigned int line);

/* The source line whose statement contains `offset` (the largest recorded mark at or before it), or 0 if the chunk has no marks yet. */
unsigned int chunk_line_for_offset(Chunk* c, unsigned int offset);

/* Formats a real guaranteeing a decimal point/exponent/nan-inf marker survives -- bare "%g"
   prints 42.0 as "42", which flips to integer through the JSON round-trip. The one shared site
   (vm.c, aer_json.c, disasm.c). */
unsigned int aer_format_real(double d, char* buf, size_t bufsize);

/* Fast snprintf("%lld", ...) replacement -- see its own comment, value_format.c. */
unsigned int aer_format_int(long long v, char* buf, size_t bufsize);

/* Shared recursive rendering (value_format.c) behind print() and vm_to_str() (interpolation, +,
   etc.) -- one consistent representation, not a terse "<array[3]>" fallback. */
void vm_format_value(Chunk* c, AerVal v, bool in_collection, StrBuf* sb);
void vm_print_value(Chunk* c, AerVal v, bool in_collection);

unsigned int chunk_add_pool(Chunk* c, AerVal v);

/* Interned index into Chunk.rawk_i/rawk_d -- see their comment on Chunk. */
unsigned int chunk_add_rawk_int(Chunk* c, int64_t v);
unsigned int chunk_add_rawk_real(Chunk* c, double v);

Shape* chunk_find_shape(Chunk* c, const char* name);

/* `defaults` is taken by ownership, never copied. */
void chunk_add_function(Chunk* c, unsigned int name_idx, unsigned int code_offset, unsigned int arity,
                        unsigned int min_arity, AerVal* defaults);
ChunkFunction* chunk_find_function(Chunk* c, const char* name);

/* Parse-time variant -- name_idx is a dedup'd pool index, so this is an int compare, no strcmp. */
ChunkFunction* chunk_find_function_by_name_idx(Chunk* c, unsigned int name_idx);

/* Recovers a frame's function from its code_offset, for stack traces. Cold path only. */
ChunkFunction* chunk_find_function_by_offset(Chunk* c, unsigned int code_offset);

/* `name` binds the module; `path_name` resolves to the file (dots as separators). Neither
   is NUL-terminated. */
bool chunk_add_import(Chunk* c, const char* name, unsigned int len, const char* path_name,
                      unsigned int path_len);
bool chunk_is_imported(Chunk* c, const char* name, unsigned int len);

/* Process-wide capability defaults, all true. io/net are defaults only: vm_init copies them into
   VM.io_enabled/net_enabled, and the runtime checks the per-VM field, so two VMs in one process can
   differ. import_enabled stays a directly-checked global because chunk_add_import runs at parse
   time with only a Chunk* in scope. Set via aer_set_*_enabled(). A blast-radius limiter, not a
   permission system -- see the README's Sandboxing note. */
extern bool aer_io_enabled;
extern bool aer_net_enabled;
extern bool aer_import_enabled;

void vm_init(VM* vm, Chunk* chunk);
void vm_free(VM* vm);

/* Save/restore around a nested vm_init() on a fresh VM while the caller's own execution is paused
   on the C call stack (aer_module.c's aer_vm_instantiate_from_file) -- see vm_current_heap's own
   comment in vm.c for why vm_run_slice's save/restore alone isn't enough here. */
VmHeap* vm_current_heap(void);
void vm_set_current_heap(VmHeap* heap);

/* Allocate/release a non-inline string's payload. `length` is the string's length, not the buffer
   size -- both sides derive the same size class from it, so a pooled buffer can never be plain-freed
   or vice versa. Only valid for length > AER_STRING_INLINE_MAX; shorter strings own nothing. */
char* vm_string_payload_alloc(VmHeap* heap, unsigned int length);
void vm_string_payload_free(VmHeap* heap, char* payload, unsigned int length);

/* Same save/restore need as vm_current_heap, for the file-scope active_vm_for_errors global (vm.c)
   -- vm_init() unconditionally repoints it at the new VM before that VM ever runs. */
VM* vm_active_error_vm(void);
void vm_set_active_error_vm(VM* vm);

/* Runs from vm->ip to OP_HALT or runtime error (returns false). A host reusing the VM after
   a false return must reset stack_top/call_depth first -- see main.c's run(). */
bool vm_run(VM* vm);

typedef enum {
    VM_SLICE_DONE, /* reached OP_HALT */
    VM_SLICE_YIELDED, /* max_instructions reached at a loop back-edge or call; vm->ip is a valid resume point */
    VM_SLICE_ERROR, /* runtime error, same as vm_run's false */
} VmSliceResult;

/* vm_run(vm) is exactly vm_run_slice(vm, 0) -- 0 means unlimited, the only budget every caller but
   the scheduler (aer_scheduler.c) ever passes. A nonzero budget bounds how many loop-back-edges and
   calls this call executes before returning VM_SLICE_YIELDED with vm->ip left at a valid resume
   point; calling vm_run_slice again on the same VM continues exactly where it left off, the same
   way vm_run already resumes from wherever vm->ip points (main.c's REPL already relies on this). */
VmSliceResult vm_run_slice(VM* vm, unsigned int max_instructions);

/* A counter, not a flag -- imports/module-calls nest. Operates on whichever heap is current (see
   vm.c's current_heap); each VM now collects only its own independent heap, so this no longer
   guards against one VM's collection reaching into another's not-yet-rooted state (structurally
   impossible now, separate heaps) -- aer_module.c's two call sites predate that split and are kept
   as harmless no-ops rather than removed speculatively. */
void vm_gc_suppress(void);
void vm_gc_unsuppress(void);

/* Cross-module call setup (aer_module_call only): pushes a real frame with dest_reg fixed at
   0 -- after the trampoline drains, the result is in call_stack[0].registers[0]. */
bool setup_call(VM* target, ChunkFunction* fn, int arg_count, AerVal* args, unsigned int return_ip);

/* Returns an uninitialized AerArray header from vm.c's internal slab pool, as if xmalloc'd directly (every in-file vm.c site still uses pool_alloc); exposed only because aer_stdlib.c's string.split() needs one and the pool isn't a raw global outside vm.c. */
AerArray* vm_new_array(void);

/* Same idea, for AerDict -- exposed for aer_json.c's json.decode(); caller must zero-init map itself (see lbl_dict_new's call site in vm.c). */
AerDict* vm_new_dict(void);

/* Generational-GC write barrier -- any store of `new_value` into an already-existing array must go
   through this (see gc_barrier_array's own comment, gc.c). `index` is the exact slot being written
   -- it feeds card marking, so a later minor GC only has to rescan indices actually dirtied since
   the last cycle rather than the whole array. Exposed for aer_collection.c's append/insert; a
   freshly built, not-yet-returned array needs no barrier. */
void gc_barrier_array(VM* vm, AerArray* a, unsigned int index, AerVal new_value);

/* Same contract as gc_barrier_array, for a struct field-set -- AerStruct is its own type/pool now,
   not a shaped AerArray, so it needs its own barrier rather than gc_barrier_array's old
   shape-ternary dispatch. No index/card-marking parameter -- see gc_barrier_struct's own comment
   (gc.c) for why a struct's small, fixed field count doesn't need it. */
void gc_barrier_struct(VM* vm, AerStruct* s, AerVal new_value);

/* Same contract, for a dict entry (update-in-place and new-entry paths) -- vm.c's vm_call_builtin
   is the only caller outside gc.c itself. `index` is the entry's DENSE index (map.dense[index]) --
   see gc_barrier_dict's own comment (gc.c) for how the caller resolves this before the actual
   hashtable write. */
void gc_barrier_dict(VM* vm, AerDict* d, unsigned int index, AerVal new_value);

/* Both defined in gc.c; called from vm.c's gc_maybe_collect (the tiny, always_inline gatekeeper
   checked once per DISPATCH()) once the rare threshold-crossing case actually happens, and from
   aer_gc_stats (embedding-facing introspection) respectively. */
unsigned int gc_count_live_cells(VmHeap* heap);
void gc_run_collection_cycle(VM* vm);

/* Frees every live cell's own separately-owned payload across all 7 pools, regardless of mark/
   generation state -- vm_free's one call site, whole-heap teardown (not a normal sweep). */
void gc_finalize_all_pools(VmHeap* heap);

/* Structural/reference equality with no error path (see its comment in vm.c) -- exposed for
   aer_collection.c's index_of, the same scan OP_IN's array case uses. */
bool values_equal(AerVal a, AerVal b);

/* Must come from function_pool (pool_mark's slab lookup fails on xmalloc'd cells); returns
   uninitialized memory -- zero it yourself. */
AerFunction* vm_new_function(void);

/* Test-only register readback (tests/smoke_test.c). */
AerVal register_get(VM* vm, int slot);

#ifdef AER_DEBUG_TOOLS
#include <stdio.h>
/* Byte-accurate per-pool memory breakdown; debug-tools only. */
void aer_debug_memory_report(FILE* out);

/* Prints, in order: a full annotated disassembly of c->code (offset, opcode name, one-line
   description, decoded operands, and -- if c->debug_hits is populated -- a hit count and source
   line for that instruction); a per-opcode summary table (name -> total hits, sorted descending);
   and a per-source-line hot-spot rollup (line -> total hits, sorted descending). Debug-build only --
   see the AER_DEBUG_TOOLS-gated Chunk.debug_hits field above. */
void aer_disassemble(Chunk* c, FILE* out);
#endif

#endif

#ifndef AER_ENCODING_H
#define AER_ENCODING_H

#include <stdint.h>
#include "opcodes.h"

/* Set = constant-pool index, clear = register (Lua's BITRK convention). Parser-internal: every
   emission site converts it to one of the wire encodings below. */
#define RK_CONST_FLAG (1 << 30)

/* Parser-internal too: a slot's static type, so the parser can pick an unchecked opcode. Not a
   separate bank -- every slot is an AerVal in the one register file. */
#define RK_RAW_INT_FLAG (1 << 29)
#define RK_RAW_REAL_FLAG (1 << 28)
#define RK_RAW_SLOT_MASK 0x7F

/* Per-frame register bank size; a register index must stay within RK8's 7 index bits with zero
   headroom to spare -- see RK8 below. */
#define FRAME_REGISTERS 128

/* Real-typed slots grow DOWN from the top of the frame, everything else up, so their position is
   fixed before a body compiles -- which a 7-bit operand needs and a per-function size cannot give.
   Frame entry tags them once and the unchecked real opcodes then store only the payload. Such a body
   gets a full-size frame; FRAME_BOUNDS names the gap that leaves, never written, tagged or traced.
   Integers need none of this -- their unchecked opcodes still write the tag. */
#define FRAME_BOUNDS(dyn_end, real_base) ((unsigned short)(((dyn_end) << 8) | (real_base)))
#define FRAME_DYN_END(bounds) ((unsigned int)((bounds) >> 8))
#define FRAME_REAL_BASE(bounds) ((unsigned int)((bounds) & 0xFF))

/* Fixed-width, word-granular instruction encoding: every instruction is one or more 32-bit words,
   the shape (1-word, 2-word, ...) fixed per opcode at compile time -- never a variable byte count.
   See ARCHITECTURE.md §3.1-3.2 for the full field vocabulary (PACK3/PACK2/PACK1, RK8, RK16,
   PACK_2X16) and why this design was chosen over a wider bit-packed word. */

#define UNPACK_A(word) (((word) >> 8) & 0xFF)
#define UNPACK_B(word) (((word) >> 16) & 0xFF)
#define UNPACK_C(word) (((word) >> 24) & 0xFF)

#define SCALED_A 1u
#define SCALED_B 2u
#define SCALED_C 4u

/* Which of an opcode's A/B/C fields hold a register slot doubled, so its handler reaches the 16-byte
   AerVal with an addressing scale of 8 instead of a separate shift. */
static inline unsigned op_scaled_fields(Opcode op) {
    switch (op) {
#define OPCODE(name, handler, scaled, ...)     case OP_##name:                                 return scaled;
#include "opcodes.def"
#undef OPCODE
    default:
        return 0;
    }
}

/* op(8) | A(8) | B(8) | C(8), low byte first. Takes and gives back operands as the parser means them:
   PACK3 scales a slot field and OPERAND_A/B/C undo it, so only a handler ever sees the wire value. */
static inline uint32_t pack3(Opcode op, uint32_t a, uint32_t b, uint32_t cc) {
    unsigned scaled = op_scaled_fields(op);
    a <<= (scaled & SCALED_A) != 0;
    b <<= (scaled & SCALED_B) != 0;
    cc <<= (scaled & SCALED_C) != 0;
    return ((uint32_t)op & 0xFF) | ((a & 0xFF) << 8) | ((b & 0xFF) << 16) | ((cc & 0xFF) << 24);
}
#define PACK3(op, a, b, cc) pack3((Opcode)(op), (uint32_t)(a), (uint32_t)(b), (uint32_t)(cc))
#define PACK2(op, a, b) PACK3(op, a, b, 0)
#define PACK1(op, a) PACK3(op, a, 0, 0)
#define OPERAND_A(word) (UNPACK_A(word) >> ((op_scaled_fields((Opcode)((word) & 0xFF)) & SCALED_A) != 0))
#define OPERAND_B(word) (UNPACK_B(word) >> ((op_scaled_fields((Opcode)((word) & 0xFF)) & SCALED_B) != 0))
#define OPERAND_C(word) (UNPACK_C(word) >> ((op_scaled_fields((Opcode)((word) & 0xFF)) & SCALED_C) != 0))

/* Packs two independent 16-bit fields into one word -- used for word1-style "two wide fields,
   no room for anything else" shapes (e.g. field_idx + an RK16 operand). */
#define PACK_2X16(hi, lo) ((((uint32_t)(hi) & 0xFFFF) << 16) | ((uint32_t)(lo) & 0xFFFF))
#define UNPACK_2X16_HI(word) (((word) >> 16) & 0xFFFF)
#define UNPACK_2X16_LO(word) ((word) & 0xFFFF)

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
#define RK8_IS_CONST(b) ((b) & RK8_CONST_FLAG)
#define RK8_INDEX(b) ((b) & RK8_INDEX_MASK)

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
#define RK16_IS_CONST(w) ((w) & RK16_CONST_FLAG)
#define RK16_INDEX(w) ((w) & RK16_INDEX_MASK)

/* type_name_idx / field_name_idx / module_idx / fn_idx / callee_offset / jump targets all get a
   full dedicated 32-bit word wherever this comment appears in the shapes below -- no packing, no
   guard needed, direct emit_u32-equivalent (a plain chunk_emit of the raw value). */

/* op(8) | a(8) | w16(16) -- one small field plus one 16-bit field, both in word0. Used by opcodes
   whose only two real fields are a register/small-count and one RK16/count16 value (OP_FIELD_SET,
   OP_INDEX_FIELD_SET's obj_reg+rk_idx half). */
#define PACK_OP_A_W16(op, a, w16) (PACK3(op, a, 0, 0) | (((uint32_t)(w16) & 0xFFFF) << 16))
#define UNPACK_W16(word) (((word) >> 16) & 0xFFFFU)

/* OP_DEFINE_STRUCT's header word: op(8) | name_idx(16) | field_count(8) -- name_idx sits in the
   middle (unlike PACK_OP_A_W16), so it gets its own macro rather than misusing that one. */
#define PACK_STRUCT_HEADER(name_idx, field_count)                                                            \
    (((uint32_t)(OP_DEFINE_STRUCT) & 0xFF) | (((uint32_t)(name_idx) & 0xFFFF) << 8) |                        \
     (((uint32_t)(field_count) & 0xFF) << 24))
#define UNPACK_STRUCT_HEADER_NAME(word) (((word) >> 8) & 0xFFFFU)
#define UNPACK_STRUCT_HEADER_COUNT(word) (((word) >> 24) & 0xFFU)

/* Module, function and builtin ids -- the stdlib's wire identity. */
#include "aer_abi.h"

/* OP_ITER_RANGE_PREP's item_dest operand is a whole 32-bit word for what is an 8-bit register
   index, so the spare high bits carry a flag. GUARD_NONNEG means this loop's body was compiled
   with UNCHECKED indexing on the strength of a compile-time proof that the start is >= 0
   (Parser.reg_nonneg), so PREP verifies that once here instead of the body checking every index.
   Only reachable when integer overflow defeated the proof -- see 5.27. */
#define RANGE_PREP_GUARD_NONNEG 0x80000000u
#define RANGE_PREP_ITEM_REG(w) ((int)((w) & 0xFFu))

/* Parts in one OP_INTERP. Bounds the builder's stack scratch; a longer interpolation compiles to
   the ordinary concatenate chain instead, which has no limit. */
#define INTERP_MAX_PARTS 16

#endif

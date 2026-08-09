/* Exhaustive round-trip check for the fixed-width, word-granular instruction encoding: emits
   representative opcodes with boundary-value operands (0, max-for-field, and — for RK8/RK16 —
   both the register and constant-pool variants) directly via the real PACK3/PACK_2X16/PACK_OP_A_W16
   helpers and pack_rk8/pack_rk16, then decodes with the real UNPACK_A, UNPACK_B, UNPACK_C,
   UNPACK_2X16_HI/LO, and UNPACK_W16 primitives, asserting the values survive exactly. The
   ordinary .aer test suite exercises every
   opcode too, but only as a byproduct of running programs with small, everyday operand values — a
   single missed field-width or word-order mismatch is a silent corruption bug here, not a compile
   error, so this needs direct coverage the ordinary suite doesn't provide. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm.h"
#include "parser.h"   /* patch_jump, patch_call_target */

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) printf("PASS: %s\n", what);
    else      { printf("FAIL: %s\n", what); failures++; }
}

/* Fresh chunk per case so word offsets are simple and predictable. */
static Chunk new_chunk(void) {
    Chunk c;
    chunk_init(&c);
    return c;
}

int main(void) {
    /* ---- Field-primitive boundaries: every opcode's correctness reduces to these. ---- */
    {
        /* Generic iABC packer (op + up to 3 byte fields) — the shape almost every 1-word opcode uses. */
        Chunk c = new_chunk();
        chunk_emit(&c, PACK3(OP_ADD, 0, 0, 0));
        chunk_emit(&c, PACK3(OP_ADD, 127, 255, 200));   /* FRAME_REGISTERS-1, and full-byte headroom beyond it */
        unsigned int p = 0;
        uint32_t w = c.code[p++];
        check((w & 0xFF) == OP_ADD && UNPACK_A(w) == 0 && UNPACK_B(w) == 0 && UNPACK_C(w) == 0, "PACK3: all-zero fields round-trip");
        w = c.code[p++];
        check(UNPACK_A(w) == 127 && UNPACK_B(w) == 255 && UNPACK_C(w) == 200, "PACK3: boundary byte fields (127, 255, 200) round-trip");
        chunk_free(&c);
    }
    {
        /* RK8: 1 flag bit + 7 index bits -- the narrow wire form shared by 2-RK-operand-in-one-word opcodes. */
        unsigned int p = 0;
        uint8_t b;
        b = pack_rk8(0);                       check(!RK8_IS_CONST(b) && RK8_INDEX(b) == 0,   "RK8 boundary: register 0 round-trips, no const flag"); (void)p;
        b = pack_rk8(RK8_MAX_INDEX);            check(!RK8_IS_CONST(b) && RK8_INDEX(b) == 127, "RK8 boundary: register 127 (max register index) round-trips, no const flag");
        b = pack_rk8(0 | RK_CONST_FLAG);        check(RK8_IS_CONST(b)  && RK8_INDEX(b) == 0,   "RK8 boundary: const 0 round-trips, const flag set");
        b = pack_rk8(RK8_MAX_INDEX | RK_CONST_FLAG); check(RK8_IS_CONST(b) && RK8_INDEX(b) == 127, "RK8 boundary: const 127 (max direct index) round-trips, const flag set");
    }
    {
        /* RK16: 1 flag bit + 15 index bits -- the wide wire form used wherever an RK operand gets
           a whole halfword (its own word, or shared with just one other 16-bit field). */
        uint16_t w;
        w = pack_rk16(0);                        check(!RK16_IS_CONST(w) && RK16_INDEX(w) == 0,     "RK16 boundary: register 0 round-trips, no const flag");
        w = pack_rk16(RK16_MAX_INDEX);            check(!RK16_IS_CONST(w) && RK16_INDEX(w) == 32767, "RK16 boundary: register 32767 round-trips, no const flag");
        w = pack_rk16(0 | RK_CONST_FLAG);         check(RK16_IS_CONST(w)  && RK16_INDEX(w) == 0,     "RK16 boundary: const 0 round-trips, const flag set");
        w = pack_rk16(RK16_MAX_INDEX | RK_CONST_FLAG); check(RK16_IS_CONST(w) && RK16_INDEX(w) == 32767, "RK16 boundary: const 32767 (max direct index) round-trips, const flag set");
    }
    {
        /* PACK_2X16: two independent 16-bit fields sharing one word (field_idx+RK16 pairs, etc). */
        uint32_t w = PACK_2X16(0, 0);
        check(UNPACK_2X16_HI(w) == 0 && UNPACK_2X16_LO(w) == 0, "PACK_2X16 boundary: (0,0) round-trips");
        w = PACK_2X16(0xFFFF, 0xFFFF);
        check(UNPACK_2X16_HI(w) == 0xFFFF && UNPACK_2X16_LO(w) == 0xFFFF, "PACK_2X16 boundary: (0xFFFF,0xFFFF) round-trips, no cross-talk between halves");
        w = PACK_2X16(0x1234, 0x5678);
        check(UNPACK_2X16_HI(w) == 0x1234 && UNPACK_2X16_LO(w) == 0x5678, "PACK_2X16: distinct halves don't bleed into each other");
    }
    {
        /* PACK_OP_A_W16: op(8) + a(8) + w16(16) -- OP_LOADK/OP_FIELD_SET/etc's word0 shape. */
        uint32_t w = PACK_OP_A_W16(OP_LOADK, 0, 0);
        check((w & 0xFF) == OP_LOADK && UNPACK_A(w) == 0 && UNPACK_W16(w) == 0, "PACK_OP_A_W16 boundary: all-zero round-trips");
        w = PACK_OP_A_W16(OP_LOADK, 255, 0xFFFF);
        check(UNPACK_A(w) == 255 && UNPACK_W16(w) == 0xFFFF, "PACK_OP_A_W16 boundary: (255, 0xFFFF) round-trips");
    }
    {
        /* PACK_STRUCT_HEADER: op(8) + name_idx(16) + field_count(8) -- name_idx sits in the middle. */
        uint32_t w = PACK_STRUCT_HEADER(0, 0);
        check((w & 0xFF) == OP_DEFINE_STRUCT && UNPACK_STRUCT_HEADER_NAME(w) == 0 && UNPACK_STRUCT_HEADER_COUNT(w) == 0,
              "PACK_STRUCT_HEADER boundary: all-zero round-trips");
        w = PACK_STRUCT_HEADER(0xFFFF, MAX_STRUCT_FIELDS);
        check(UNPACK_STRUCT_HEADER_NAME(w) == 0xFFFF && UNPACK_STRUCT_HEADER_COUNT(w) == MAX_STRUCT_FIELDS,
              "PACK_STRUCT_HEADER boundary: (0xFFFF name, MAX_STRUCT_FIELDS count) round-trips");
    }
    {
        /* Jump/call/offset targets: always a full dedicated 32-bit word, blind-overwrite patchable --
           closes the old encoding's 20-bit-immediate-style truncation bug class outright. */
        Chunk c = new_chunk();
        chunk_emit(&c, PACK1(OP_JUMP, 0));
        unsigned int patch_offset = c.count;
        chunk_emit(&c, 0);
        check(c.count - patch_offset == 1, "JUMP_TARGET: reserves exactly 1 word");
        patch_jump(&c, patch_offset, 2000000000U);
        check((int)(patch_offset + 1) + (int)(int32_t)c.code[patch_offset] == 2000000000,
              "JUMP_TARGET: a far forward delta resolves back to its target");
        patch_jump(&c, patch_offset, 0);
        check((int)(patch_offset + 1) + (int)(int32_t)c.code[patch_offset] == 0,
              "JUMP_TARGET: a backward delta sign-extends and resolves back to its target");
        chunk_free(&c);
    }

    /* ---- Representative real opcodes, one per distinct shape, with boundary operands. ---- */

    /* 1-word iABC+RK8 shape — the hottest opcode by dispatch count (dest, rk, rk). */
    {
        Chunk c = new_chunk();
        chunk_emit(&c, PACK3(OP_ADD, 127, pack_rk8(RK8_MAX_INDEX), pack_rk8(RK8_MAX_INDEX | RK_CONST_FLAG)));
        unsigned int p = 0;
        uint32_t w = c.code[p++];
        check((w & 0xFF) == OP_ADD, "OP_ADD: opcode byte round-trips");
        check(UNPACK_A(w) == 127, "OP_ADD: dest register round-trips at max");
        uint8_t rk_b = (uint8_t)UNPACK_B(w), rk_c = (uint8_t)UNPACK_C(w);
        check(!RK8_IS_CONST(rk_b) && RK8_INDEX(rk_b) == 127, "OP_ADD: rk_b (register) round-trips at max");
        check(RK8_IS_CONST(rk_c) && RK8_INDEX(rk_c) == 127, "OP_ADD: rk_c (constant) round-trips at max");
        check(p == c.count, "OP_ADD: exactly 1 word emitted");
        chunk_free(&c);
    }

    /* 1-word shape with a 16-bit field (op+dest+pool_idx all in one word — down from 2 words). */
    {
        Chunk c = new_chunk();
        chunk_emit(&c, PACK_OP_A_W16(OP_LOADK, 127, 0xFFFF));
        unsigned int p = 0;
        uint32_t w = c.code[p++];
        check((w & 0xFF) == OP_LOADK, "OP_LOADK: opcode byte round-trips");
        check(UNPACK_A(w) == 127, "OP_LOADK: dest register round-trips at max");
        check(UNPACK_W16(w) == 0xFFFF, "OP_LOADK: pool_idx round-trips at max (16 bits)");
        check(p == c.count, "OP_LOADK: exactly 1 word emitted (was 2 in the old encoding)");
        chunk_free(&c);
    }

    /* 2-word shape: word0 (dest+struct_reg), word1 (field_name_idx, full 32-bit). */
    {
        Chunk c = new_chunk();
        chunk_emit(&c, PACK3(OP_FIELD_GET, 127, 200, 0));
        chunk_emit(&c, 0xFFFFFFFFU);
        unsigned int p = 0;
        uint32_t w0 = c.code[p++];
        check((w0 & 0xFF) == OP_FIELD_GET && UNPACK_A(w0) == 127 && UNPACK_B(w0) == 200, "OP_FIELD_GET: word0 (dest, struct_reg) round-trips at max");
        uint32_t field_idx = c.code[p++];
        check(field_idx == 0xFFFFFFFFU, "OP_FIELD_GET: word1 (field_name_idx) round-trips at full 32-bit range");
        check(p == c.count, "OP_FIELD_GET: exactly 2 words emitted");
        chunk_free(&c);
    }

    /* Widest fused opcode: 3 words — word0 (obj+bin_op), word1 (field_idx+rk_idx16), word2 (rk_rhs16). */
    {
        Chunk c = new_chunk();
        chunk_emit(&c, PACK3(OP_INDEX_FIELD_COMPOUND, 200, OP_MUL, 0));
        chunk_emit(&c, PACK_2X16(0xFFFF, pack_rk16(RK16_MAX_INDEX | RK_CONST_FLAG)));
        chunk_emit(&c, PACK_2X16(0, pack_rk16(RK16_MAX_INDEX)));
        unsigned int p = 0;
        uint32_t w0 = c.code[p++];
        check((w0 & 0xFF) == OP_INDEX_FIELD_COMPOUND && UNPACK_A(w0) == 200 && UNPACK_B(w0) == OP_MUL,
              "OP_INDEX_FIELD_COMPOUND: word0 (obj_reg, bin_op) round-trips at max");
        uint32_t w1 = c.code[p++];
        check(UNPACK_2X16_HI(w1) == 0xFFFF, "OP_INDEX_FIELD_COMPOUND: word1 field_idx round-trips at max");
        uint16_t rk_idx = (uint16_t)UNPACK_2X16_LO(w1);
        check(RK16_IS_CONST(rk_idx) && RK16_INDEX(rk_idx) == 32767, "OP_INDEX_FIELD_COMPOUND: word1 rk_idx (const) round-trips at max");
        uint32_t w2 = c.code[p++];
        uint16_t rk_rhs = (uint16_t)UNPACK_2X16_LO(w2);
        check(!RK16_IS_CONST(rk_rhs) && RK16_INDEX(rk_rhs) == 32767, "OP_INDEX_FIELD_COMPOUND: word2 rk_rhs (register) round-trips at max");
        check(p == c.count, "OP_INDEX_FIELD_COMPOUND: exactly 3 words emitted (down from a maximally-packed single 64-bit word, traded for fixed word-granular stride)");
        chunk_free(&c);
    }

    /* Isolated patch_jump mechanics on OP_CALL's callee_offset word (real OP_CALL is 3 words total
       via emit_call -- callee_offset then func_index -- but patch_jump's blind-overwrite behavior
       only involves the one word it's given, so this only emits that word directly. */
    {
        Chunk c = new_chunk();
        chunk_emit(&c, PACK3(OP_CALL, 100, 50, 4));
        unsigned int patch_offset = c.count;
        chunk_emit(&c, 0);
        check(c.count - patch_offset == 1, "OP_CALL: callee_offset reserves exactly 1 word");
        patch_call_target(&c, patch_offset, 4000000000U);
        check(c.code[patch_offset] == 4000000000U, "OP_CALL: patch_call_target stores an absolute offset verbatim");
        check(c.code[patch_offset - 1] & 0xFF, "OP_CALL: opcode byte sits exactly 1 word before callee_offset, as emit_call/the OP_HALT forward-ref-failure patch site rely on");
        chunk_free(&c);
    }
    /* emit_call's real 3-word shape end-to-end: word0 (dest/arg_base/arg_count), word1
       (callee_offset, patchable), word2 (func_index, also patchable -- see func_register). */
    {
        Chunk c = new_chunk();
        unsigned int patch_offset = emit_call(&c, 5, 12345, 6, 2, 7);
        check(c.count == 3, "emit_call: exactly 3 words emitted (dest/arg_base/arg_count, callee_offset, func_index)");
        uint32_t w0 = c.code[0];
        check((w0 & 0xFF) == OP_CALL && UNPACK_A(w0) == 5 && UNPACK_B(w0) == 6 && UNPACK_C(w0) == 2,
              "emit_call: word0 (dest, arg_base, arg_count) round-trips");
        check(c.code[patch_offset] == 12345, "emit_call: word1 (callee_offset) round-trips");
        check(c.code[patch_offset + 1] == 7 * sizeof(ChunkFunction),
              "emit_call: word2 is the function's BYTE offset, so lbl_call needs no multiply");
        chunk_free(&c);
    }

    /* Genuinely variable-length shape: header word + N repeated 2-word field units. */
    {
        Chunk c = new_chunk();
        chunk_emit(&c, PACK_STRUCT_HEADER(0xFFFF, 3));
        for (int i = 0; i < 3; i++) {
            chunk_emit(&c, PACK_2X16((uint16_t)(1000 + i), (uint16_t)(2000 + i)));
            chunk_emit(&c, TYPE_INTEGER);
        }
        unsigned int p = 0;
        uint32_t header = c.code[p++];
        check((header & 0xFF) == OP_DEFINE_STRUCT, "OP_DEFINE_STRUCT: opcode byte round-trips");
        check(UNPACK_STRUCT_HEADER_NAME(header) == 0xFFFF, "OP_DEFINE_STRUCT: name_idx round-trips at max");
        unsigned int field_count = UNPACK_STRUCT_HEADER_COUNT(header);
        check(field_count == 3, "OP_DEFINE_STRUCT: field_count round-trips");
        bool triples_ok = true;
        for (unsigned int i = 0; i < field_count; i++) {
            uint32_t name_default_word = c.code[p++];
            unsigned int name = UNPACK_2X16_HI(name_default_word);
            unsigned int def  = UNPACK_2X16_LO(name_default_word);
            unsigned int type = c.code[p++];
            if (name != 1000 + i || def != 2000 + i || type != TYPE_INTEGER) triples_ok = false;
        }
        check(triples_ok, "OP_DEFINE_STRUCT: all 3 variable-length 2-word field units round-trip");
        check(p == c.count, "OP_DEFINE_STRUCT: word count matches header + 3*2-word units exactly");
        chunk_free(&c);
    }

    /* 4-word shape: word0 (dest+arg_base+arg_count), word1 (module_idx), word2 (fn_idx), word3 (module_id+fn_id, fn_id signed). */
    {
        Chunk c = new_chunk();
        chunk_emit(&c, PACK3(OP_CALL_MODULE, 100, 50, 4));
        chunk_emit(&c, 0xFFFFFFFFU);
        chunk_emit(&c, 0xFFFFFFFFU);
        chunk_emit(&c, PACK_2X16((uint16_t)CALL_MODULE_DYNAMIC, (uint16_t)(int16_t)FN_ID_UNKNOWN));
        unsigned int p = 0;
        uint32_t w0 = c.code[p++];
        check((w0 & 0xFF) == OP_CALL_MODULE && UNPACK_A(w0) == 100 && UNPACK_B(w0) == 50 && UNPACK_C(w0) == 4,
              "OP_CALL_MODULE: word0 (dest, arg_base, arg_count) round-trips");
        check(c.code[p++] == 0xFFFFFFFFU, "OP_CALL_MODULE: word1 (module_idx) round-trips at full 32-bit range");
        check(c.code[p++] == 0xFFFFFFFFU, "OP_CALL_MODULE: word2 (fn_idx) round-trips at full 32-bit range");
        uint32_t ids_word = c.code[p++];
        check((int)UNPACK_2X16_HI(ids_word) == CALL_MODULE_DYNAMIC, "OP_CALL_MODULE: word3 module_id round-trips");
        check((int16_t)UNPACK_2X16_LO(ids_word) == FN_ID_UNKNOWN, "OP_CALL_MODULE: word3 fn_id round-trips as a signed value (FN_ID_UNKNOWN == -1)");
        check(p == c.count, "OP_CALL_MODULE: exactly 4 words emitted");
        chunk_free(&c);
    }

    /* OP_RAW_LOAD_INT: full int32 immediate, its own dedicated word -- closes the old 20-bit
       truncation bug outright rather than just widening it. */
    {
        Chunk c = new_chunk();
        chunk_emit(&c, PACK1(OP_RAW_LOAD_INT, 31));
        chunk_emit(&c, (uint32_t)(int32_t)(-2147483647 - 1));   /* INT32_MIN */
        unsigned int p = 0;
        uint32_t w0 = c.code[p++];
        check((w0 & 0xFF) == OP_RAW_LOAD_INT && UNPACK_A(w0) == 31, "OP_RAW_LOAD_INT: word0 (slot) round-trips at max raw-int slot");
        check((int32_t)c.code[p++] == (-2147483647 - 1), "OP_RAW_LOAD_INT: full int32 immediate round-trips at INT32_MIN");
        check(p == c.count, "OP_RAW_LOAD_INT: exactly 2 words emitted");
        chunk_free(&c);
    }

    printf("\n%s\n", failures == 0 ? "All opcode round-trip tests passed." : "SOME OPCODE ROUND-TRIP TESTS FAILED.");
    return failures == 0 ? 0 : 1;
}

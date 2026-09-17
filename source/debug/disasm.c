#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "vm.h"
#include "error.h"

/* Where an operand sits: a slice of word0, or a whole trailing word or one of its halves. An
   instruction's word count is derived from the highest trailing word its operands name. */
typedef enum {
    AT_A,
    AT_B,
    AT_C,
    AT_W16, /* word0's upper halfword */
    AT_W1,
    AT_W1_HI,
    AT_W1_LO,
    AT_W2,
    AT_W2_HI,
    AT_W2_LO,
    AT_W3_HI,
    AT_W3_LO,
} At;

/* How an operand's value is rendered. */
typedef enum {
    F_END, /* ends an operand list */
    F_REG,
    F_RK8,
    F_RK16,
    F_RAWI, /* raw int slot */
    F_RAWR, /* raw real slot */
    F_RAWK_I, /* raw int slot, or an index into rawk_i[] when the const flag is set */
    F_RAWK_D,
    F_RAWK_I_AT, /* always an index into rawk_i[] */
    F_POOL,
    F_POOL_RAWI,
    F_POOL_RAWD,
    F_NAME,
    F_JUMP,
    F_COUNT,
    F_BINOP,
    F_CAST,
    F_OFF, /* a field's byte offset into a struct payload */
    F_IMM32,
    F_FN_ID,
    F_MODULE_ID,
    F_MODULE_FN,
    F_BUILTIN_ID,
} Fld;

typedef struct {
    unsigned char at;
    unsigned char fld;
} Operand;

#define MAX_OPERANDS 7

typedef struct {
    const char* name;
    const char* desc;
    Operand ops[MAX_OPERANDS];
    /* Word count comes from the instruction itself rather than from ops[]: OP_DEFINE_STRUCT,
       OP_INTERP and OP_INDEX_GET_INTERP each carry their operand count in word0. */
    bool variable;
} OpInfo;

/* OP_INDEX_GET_INTERP is the last member of the Opcode enum (vm.h). */
#define OP_INFO_MAX OP_INDEX_GET_INTERP

#define ROW(op, d, ...) [op] = {#op, d, {__VA_ARGS__}}

/* One shape per opcode family, so the kind, width and checked axes read as the matrix they are. */
#define ROW_BINOP(op, d) ROW(op, d, {AT_A, F_REG}, {AT_B, F_RK8}, {AT_C, F_RK8})
#define ROW_CMP_JUMP(op, d) ROW(op, d, {AT_B, F_RK8}, {AT_C, F_RK8}, {AT_W1, F_JUMP})
#define ROW_RAW_CMP_JUMP(op, d, K, KK) ROW(op, d, {AT_B, K}, {AT_C, KK}, {AT_W1, F_JUMP})
#define ROW_RAW_ARITH(op, d, K, KK) ROW(op, d, {AT_A, K}, {AT_B, K}, {AT_C, KK})
#define ROW_RAW_CMP(op, d, K, KK) ROW(op, d, {AT_A, F_REG}, {AT_B, K}, {AT_C, KK})
#define ROW_CALL(op, d)                                                                              \
    ROW(op, d, {AT_A, F_REG}, {AT_B, F_REG}, {AT_C, F_COUNT}, {AT_W1, F_JUMP}, {AT_W2, F_COUNT})
#define ROW_CALL_VALUE(op, d)                                                                        \
    ROW(op, d, {AT_A, F_REG}, {AT_B, F_REG}, {AT_C, F_COUNT}, {AT_W1, F_REG})
#define ROW_ITER(op, d)                                                                              \
    ROW(op, d, {AT_A, F_REG}, {AT_B, F_REG}, {AT_C, F_REG}, {AT_W1, F_REG}, {AT_W2, F_JUMP})
#define ROW_FIELD_GET_RAW(op, d, K) ROW(op, d, {AT_A, K}, {AT_B, F_REG}, {AT_W1, F_OFF})
#define ROW_FIELD_SET_RAW(op, d, K) ROW(op, d, {AT_A, F_REG}, {AT_W1, F_OFF}, {AT_W2, K})
#define ROW_FIELD_COMPOUND_RAW(op, d, K)                                                             \
    ROW(op, d, {AT_A, F_REG}, {AT_B, F_BINOP}, {AT_W1, F_OFF}, {AT_W2, K})
#define ROW_IDX_FIELD_GET_RAW(op, d, K)                                                              \
    ROW(op, d, {AT_A, K}, {AT_B, F_REG}, {AT_W1_HI, F_OFF}, {AT_W1_LO, F_RK16})
#define ROW_IDX_FIELD_SET_RAW(op, d, K)                                                              \
    ROW(op, d, {AT_A, F_REG}, {AT_W16, F_RK16}, {AT_W1_HI, F_OFF}, {AT_W1_LO, K})
#define ROW_IDX_FIELD_COMPOUND_RAW(op, d, K)                                                         \
    ROW(op, d, {AT_A, F_REG}, {AT_B, F_BINOP}, {AT_W1_HI, F_OFF}, {AT_W1_LO, F_RK16}, {AT_W2, K})

static const OpInfo op_info[OP_INFO_MAX + 1] = {
    ROW_BINOP(OP_ADD, "reg = rk + rk"),
    ROW_BINOP(OP_SUB, "reg = rk - rk"),
    ROW_BINOP(OP_MUL, "reg = rk * rk"),
    ROW_BINOP(OP_DIV, "reg = rk / rk"),
    ROW_BINOP(OP_MOD, "reg = rk % rk"),
    ROW_BINOP(OP_FLOOR_DIV, "reg = rk // rk"),
    ROW_BINOP(OP_EQ, "reg = rk == rk"),
    ROW_BINOP(OP_NEQ, "reg = rk != rk"),
    ROW_BINOP(OP_LT, "reg = rk < rk"),
    ROW_BINOP(OP_GT, "reg = rk > rk"),
    ROW_BINOP(OP_LTE, "reg = rk <= rk"),
    ROW_BINOP(OP_GTE, "reg = rk >= rk"),
    ROW_BINOP(OP_IN, "reg = rk in rk"),
    ROW_BINOP(OP_BITWISE_AND, "reg = rk & rk"),
    ROW_BINOP(OP_BITWISE_OR, "reg = rk | rk"),
    ROW_BINOP(OP_BITWISE_XOR, "reg = rk ^ rk"),
    ROW_BINOP(OP_LSHIFT, "reg = rk << rk"),
    ROW_BINOP(OP_RSHIFT, "reg = rk >> rk"),

    /* Never dispatched standalone (parser tags, or embedded in OP_UNARY), but F_BINOP rendering
       still reads their names from this table. */
    ROW(OP_AND, ""),
    ROW(OP_OR, ""),
    ROW(OP_PIPE, ""),
    ROW(OP_NEGATE, ""),
    ROW(OP_NOT, ""),
    ROW(OP_BITWISE_NOT, ""),
    ROW(OP_TO_STR, ""),

    ROW(OP_JUMP, "unconditional jump", {AT_W1, F_JUMP}),
    [OP_DEFINE_STRUCT] = {"OP_DEFINE_STRUCT",
                          "register a struct type (variable-length: header word, then that many field words)",
                          {{0, F_END}},
                          true},
    ROW(OP_HALT, "stop execution"),
    ROW(OP_LOADK, "reg = pool constant", {AT_A, F_REG}, {AT_W16, F_POOL}),
    ROW(OP_MOVE, "reg = reg", {AT_A, F_REG}, {AT_B, F_REG}),
    ROW(OP_IS_RESULT, "reg = is-result(reg)", {AT_A, F_REG}, {AT_B, F_REG}),
    ROW(OP_JUMP_IF_FALSE_REG, "jump if !reg, no pop", {AT_A, F_REG}, {AT_W1, F_JUMP}),
    ROW_CALL(OP_CALL, "call by compile-time-resolved offset"),
    ROW_CALL_VALUE(OP_CALL_VALUE, "call a runtime function value held in a register"),
    ROW_CALL(OP_TAIL_CALL, "tail call by compile-time-resolved offset, reuses this frame"),
    ROW_CALL_VALUE(OP_TAIL_CALL_VALUE, "tail call through a register value, reuses this frame"),
    ROW(OP_RETURN, "return reg to caller", {AT_A, F_REG}),
    ROW(OP_CALL_MODULE, "call a native or file-module function by (module, function) name",
        {AT_A, F_REG}, {AT_B, F_REG}, {AT_C, F_COUNT}, {AT_W1, F_NAME}, {AT_W2, F_NAME},
        {AT_W3_HI, F_MODULE_ID}, {AT_W3_LO, F_MODULE_FN}),
    ROW(OP_CALL_BUILTIN, "global builtin (length/print/etc.) by name", {AT_A, F_REG}, {AT_B, F_REG},
        {AT_C, F_COUNT}, {AT_W1, F_NAME}, {AT_W2, F_BUILTIN_ID}),
    ROW(OP_ARRAY_NEW, "reg = new array from a contiguous reg range", {AT_A, F_REG}, {AT_B, F_REG},
        {AT_C, F_COUNT}),
    ROW(OP_INDEX_GET, "reg = reg[rk]", {AT_A, F_REG}, {AT_B, F_REG}, {AT_C, F_RK8}),
    ROW(OP_INDEX_SET, "reg[rk] = rk", {AT_A, F_REG}, {AT_B, F_RK8}, {AT_C, F_RK8}),
    ROW(OP_TYPED_INDEX_GET_UNCHECKED, "loop-proven-safe: reg = typed_arr[rk]", {AT_A, F_REG},
        {AT_B, F_REG}, {AT_C, F_RK8}),
    ROW(OP_TYPED_INDEX_SET_UNCHECKED, "loop-proven-safe: typed_arr[rk] = rk", {AT_A, F_REG},
        {AT_B, F_RK8}, {AT_C, F_RK8}),
    ROW(OP_DESTRUCTURE, "reg, reg = destructure(reg)", {AT_A, F_REG}, {AT_B, F_REG}, {AT_C, F_REG}),
    ROW(OP_SLICE_GET, "reg = reg[rk:rk]", {AT_A, F_REG}, {AT_B, F_REG}, {AT_W1_HI, F_RK16},
        {AT_W1_LO, F_RK16}),
    ROW(OP_DICT_NEW, "reg = new dict from contiguous key/value reg pairs", {AT_A, F_REG},
        {AT_B, F_REG}, {AT_C, F_COUNT}),
    ROW(OP_ITER_NEXT_ARRAY, "for-each step, array or dict-keys", {AT_A, F_REG}, {AT_B, F_REG},
        {AT_C, F_REG}, {AT_W1, F_JUMP}),
    ROW_ITER(OP_ITER_NEXT_PAIR, "for-each step, dict key+value pairs"),
    ROW_ITER(OP_ITER_RANGE_PREP, "rotated range-for: once-before-loop check"),
    ROW_ITER(OP_ITER_RANGE_LOOP,
             "rotated range-for: bottom-of-loop advance+check+branch-back (bounds snapshotted "
             "once, never re-validated)"),
    ROW(OP_STRUCT_NEW, "reg = new struct instance from a contiguous reg range", {AT_A, F_REG},
        {AT_B, F_REG}, {AT_C, F_COUNT}, {AT_W1, F_NAME}),
    ROW(OP_FIELD_GET, "reg = struct.field", {AT_A, F_REG}, {AT_B, F_REG}, {AT_W1, F_NAME}),
    ROW(OP_FIELD_SET, "struct.field = rk", {AT_A, F_REG}, {AT_W16, F_RK16}, {AT_W1, F_NAME}),
    ROW(OP_ARRAY_REPEAT,
        "reg = [fill_reg; rk_count] (struct -> packed array, number -> typed array)", {AT_A, F_REG},
        {AT_B, F_REG}, {AT_C, F_COUNT}, {AT_W1_LO, F_RK16}),
    ROW(OP_INDEX_FIELD_GET, "fused: reg = reg[rk].field (packed or struct array)", {AT_A, F_REG},
        {AT_B, F_REG}, {AT_W1_HI, F_NAME}, {AT_W1_LO, F_RK16}),
    ROW(OP_INDEX_FIELD_SET, "fused: reg[rk].field = rk (packed or struct array)", {AT_A, F_REG},
        {AT_W16, F_RK16}, {AT_W1_HI, F_NAME}, {AT_W1_LO, F_RK16}),
    ROW(OP_INDEX_FIELD_COMPOUND,
        "fused: reg[rk].field OP= rk (resolved once, packed or struct array)", {AT_A, F_REG},
        {AT_B, F_BINOP}, {AT_W1_HI, F_NAME}, {AT_W1_LO, F_RK16}, {AT_W2_LO, F_RK16}),
    ROW(OP_UNARY, "reg = unary_op(rk)", {AT_A, F_REG}, {AT_B, F_BINOP}, {AT_C, F_RK8}),
    ROW(OP_CAST, "reg = cast(rk)", {AT_A, F_REG}, {AT_B, F_CAST}, {AT_C, F_RK8}),
    /* Covers both struct.field OP rk AND rk OP struct.field -- the parser canonicalizes the latter
       into this same opcode wherever that is exact (see vm.h). */
    ROW(OP_FIELD_BINARY, "fused: reg = struct.field OP rk (field on the left)", {AT_A, F_REG},
        {AT_B, F_REG}, {AT_C, F_BINOP}, {AT_W1_HI, F_NAME}, {AT_W1_LO, F_RK16}),
    ROW(OP_FIELD_COMPOUND, "fused: struct.field OP= rk (resolved once, no dest reg)", {AT_A, F_REG},
        {AT_B, F_BINOP}, {AT_W1_HI, F_NAME}, {AT_W1_LO, F_RK16}),
    ROW(OP_INDEX_COMPOUND, "fused: arr[rk] OP= rk (any receiver, no dest reg)", {AT_A, F_REG},
        {AT_B, F_BINOP}, {AT_W1_HI, F_RK16}, {AT_W1_LO, F_RK16}),
    ROW(OP_TYPED_ARRAY_CHAIN2,
        "fused: reg = (reg op1 reg) op2 reg (typed-array chain, runtime-checked)", {AT_A, F_REG},
        {AT_B, F_REG}, {AT_C, F_REG}, {AT_W1_HI, F_BINOP}, {AT_W1_LO, F_REG}, {AT_W2, F_BINOP}),
    ROW(OP_PRINT_REPL, "shell mode: print reg unless null", {AT_A, F_REG}),

    ROW(OP_RAW_LOAD_INT, "rawi = imm (full int32)", {AT_A, F_RAWI}, {AT_W1, F_IMM32}),
    ROW(OP_RAW_LOAD_REAL, "rawr = pool constant", {AT_A, F_RAWR}, {AT_W1, F_POOL_RAWD}),
    ROW_RAW_ARITH(OP_RAW_ADD_INT, "rawi = rawi + rawi", F_RAWI, F_RAWK_I),
    ROW_RAW_ARITH(OP_RAW_SUB_INT, "rawi = rawi - rawi", F_RAWI, F_RAWK_I),
    ROW_RAW_ARITH(OP_RAW_MUL_INT, "rawi = rawi * rawi", F_RAWI, F_RAWK_I),
    ROW_RAW_ARITH(OP_RAW_ADD_INT_K, "rawi = rawi + rawk", F_RAWI, F_RAWK_I_AT),
    ROW_RAW_ARITH(OP_RAW_SUB_INT_K, "rawi = rawi - rawk", F_RAWI, F_RAWK_I_AT),
    /* int/int division promotes, so this one alone writes a raw real. */
    ROW(OP_RAW_DIV_INT, "rawr = rawi / rawi (int/int division always promotes to real)",
        {AT_A, F_RAWR}, {AT_B, F_RAWI}, {AT_C, F_RAWK_I}),
    ROW_RAW_ARITH(OP_RAW_MOD_INT, "rawi = rawi % rawi", F_RAWI, F_RAWK_I),
    ROW_RAW_ARITH(OP_RAW_FLOOR_DIV_INT, "rawi = floor(rawi / rawi)", F_RAWI, F_RAWK_I),
    ROW_RAW_ARITH(OP_RAW_ADD_REAL, "rawr = rawr + rawr", F_RAWR, F_RAWK_D),
    ROW_RAW_ARITH(OP_RAW_SUB_REAL, "rawr = rawr - rawr", F_RAWR, F_RAWK_D),
    ROW_RAW_ARITH(OP_RAW_MUL_REAL, "rawr = rawr * rawr", F_RAWR, F_RAWK_D),
    ROW_RAW_ARITH(OP_RAW_DIV_REAL, "rawr = rawr / rawr", F_RAWR, F_RAWK_D),
    ROW_RAW_ARITH(OP_RAW_FMA_REAL,
                  "rawr = rawr + rawr * rawr (fused mul-add dispatch, two roundings)", F_RAWR,
                  F_RAWK_D),
    ROW_RAW_ARITH(OP_RAW_FMS_REAL,
                  "rawr = rawr - rawr * rawr (fused mul-sub dispatch, two roundings)", F_RAWR,
                  F_RAWK_D),
    ROW_RAW_CMP(OP_RAW_LT_INT, "reg = rawi < rawi", F_RAWI, F_RAWK_I),
    ROW_RAW_CMP(OP_RAW_LTE_INT, "reg = rawi <= rawi", F_RAWI, F_RAWK_I),
    ROW_RAW_CMP(OP_RAW_LT_REAL, "reg = rawr < rawr", F_RAWR, F_RAWK_D),
    ROW_RAW_CMP(OP_RAW_LTE_REAL, "reg = rawr <= rawr", F_RAWR, F_RAWK_D),
    ROW_RAW_CMP(OP_RAW_EQ_INT, "reg = rawi == rawi", F_RAWI, F_RAWK_I),
    ROW_RAW_CMP(OP_RAW_NEQ_INT, "reg = rawi != rawi", F_RAWI, F_RAWK_I),
    ROW_RAW_CMP(OP_RAW_EQ_REAL, "reg = rawr == rawr", F_RAWR, F_RAWK_D),
    ROW_RAW_CMP(OP_RAW_NEQ_REAL, "reg = rawr != rawr", F_RAWR, F_RAWK_D),
    ROW(OP_UNBOX_INT, "rawi = unbox(reg) (tag-checked)", {AT_A, F_RAWI}, {AT_B, F_REG}),
    ROW(OP_UNBOX_REAL, "rawr = unbox(reg) (tag-checked)", {AT_A, F_RAWR}, {AT_B, F_REG}),
    ROW(OP_RAW_MOVE_INT, "rawi = rawi", {AT_A, F_RAWI}, {AT_B, F_RAWI}),
    ROW(OP_RAW_MOVE_REAL, "rawr = rawr", {AT_A, F_RAWR}, {AT_B, F_RAWR}),
    ROW(OP_RAW_LOAD_INT_POOL, "rawi = pool constant", {AT_A, F_RAWI}, {AT_W1, F_POOL_RAWI}),

    /* Shape-specialized field access: one row per (storage kind, checked-ness) pair. See vm.h's
       own comment on this family, and on the compile-time proof behind the _UNCHECKED forms. */
    ROW_IDX_FIELD_GET_RAW(OP_INDEX_FIELD_GET_RAW_INT, "specialized: rawi = packed_arr[rk].field",
                          F_RAWI),
    ROW_IDX_FIELD_GET_RAW(OP_INDEX_FIELD_GET_RAW_REAL, "specialized: rawr = packed_arr[rk].field",
                          F_RAWR),
    ROW_FIELD_GET_RAW(OP_FIELD_GET_RAW_INT, "specialized: rawi = struct.field", F_RAWI),
    ROW_FIELD_GET_RAW(OP_FIELD_GET_RAW_REAL, "specialized: rawr = struct.field", F_RAWR),
    ROW_IDX_FIELD_SET_RAW(OP_INDEX_FIELD_SET_RAW_INT, "specialized: packed_arr[rk].field = rawi",
                          F_RAWI),
    ROW_IDX_FIELD_SET_RAW(OP_INDEX_FIELD_SET_RAW_REAL, "specialized: packed_arr[rk].field = rawr",
                          F_RAWR),
    ROW_FIELD_SET_RAW(OP_FIELD_SET_RAW_INT, "specialized: struct.field = rawi", F_RAWI),
    ROW_FIELD_SET_RAW(OP_FIELD_SET_RAW_REAL, "specialized: struct.field = rawr", F_RAWR),
    ROW_FIELD_COMPOUND_RAW(OP_FIELD_COMPOUND_RAW_INT, "specialized: struct.field OP= rawi", F_RAWI),
    ROW_FIELD_COMPOUND_RAW(OP_FIELD_COMPOUND_RAW_REAL, "specialized: struct.field OP= rawr", F_RAWR),
    ROW_IDX_FIELD_COMPOUND_RAW(OP_INDEX_FIELD_COMPOUND_RAW_INT,
                               "specialized: packed_arr[rk].field OP= rawi", F_RAWI),
    ROW_IDX_FIELD_COMPOUND_RAW(OP_INDEX_FIELD_COMPOUND_RAW_REAL,
                               "specialized: packed_arr[rk].field OP= rawr", F_RAWR),

    ROW_IDX_FIELD_GET_RAW(OP_INDEX_FIELD_GET_RAW_INT_UNCHECKED,
                          "specialized+loop-proven-safe: rawi = packed_arr[rk].field", F_RAWI),
    ROW_IDX_FIELD_GET_RAW(OP_INDEX_FIELD_GET_RAW_REAL_UNCHECKED,
                          "specialized+loop-proven-safe: rawr = packed_arr[rk].field", F_RAWR),
    ROW_IDX_FIELD_SET_RAW(OP_INDEX_FIELD_SET_RAW_INT_UNCHECKED,
                          "specialized+loop-proven-safe: packed_arr[rk].field = rawi", F_RAWI),
    ROW_IDX_FIELD_SET_RAW(OP_INDEX_FIELD_SET_RAW_REAL_UNCHECKED,
                          "specialized+loop-proven-safe: packed_arr[rk].field = rawr", F_RAWR),
    ROW_IDX_FIELD_COMPOUND_RAW(OP_INDEX_FIELD_COMPOUND_RAW_INT_UNCHECKED,
                               "specialized+loop-proven-safe: packed_arr[rk].field OP= rawi", F_RAWI),
    ROW_IDX_FIELD_COMPOUND_RAW(OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED,
                               "specialized+loop-proven-safe: packed_arr[rk].field OP= rawr", F_RAWR),

    /* Narrow (int32/float32) counterparts of the whole family above -- same word layouts, just a
       4-byte field instead of 8. */
    ROW_IDX_FIELD_GET_RAW(OP_INDEX_FIELD_GET_RAW_INT32,
                          "specialized: rawi = packed_arr[rk].field (narrow)", F_RAWI),
    ROW_IDX_FIELD_GET_RAW(OP_INDEX_FIELD_GET_RAW_FLOAT32,
                          "specialized: rawr = packed_arr[rk].field (narrow)", F_RAWR),
    ROW_FIELD_GET_RAW(OP_FIELD_GET_RAW_INT32, "specialized: rawi = struct.field (narrow)", F_RAWI),
    ROW_FIELD_GET_RAW(OP_FIELD_GET_RAW_FLOAT32, "specialized: rawr = struct.field (narrow)", F_RAWR),
    ROW_IDX_FIELD_SET_RAW(OP_INDEX_FIELD_SET_RAW_INT32,
                          "specialized: packed_arr[rk].field = rawi (narrow)", F_RAWI),
    ROW_IDX_FIELD_SET_RAW(OP_INDEX_FIELD_SET_RAW_FLOAT32,
                          "specialized: packed_arr[rk].field = rawr (narrow)", F_RAWR),
    ROW_FIELD_SET_RAW(OP_FIELD_SET_RAW_INT32, "specialized: struct.field = rawi (narrow)", F_RAWI),
    ROW_FIELD_SET_RAW(OP_FIELD_SET_RAW_FLOAT32, "specialized: struct.field = rawr (narrow)", F_RAWR),
    ROW_FIELD_COMPOUND_RAW(OP_FIELD_COMPOUND_RAW_INT32,
                           "specialized: struct.field OP= rawi (narrow)", F_RAWI),
    ROW_FIELD_COMPOUND_RAW(OP_FIELD_COMPOUND_RAW_FLOAT32,
                           "specialized: struct.field OP= rawr (narrow)", F_RAWR),
    ROW_IDX_FIELD_COMPOUND_RAW(OP_INDEX_FIELD_COMPOUND_RAW_INT32,
                               "specialized: packed_arr[rk].field OP= rawi (narrow)", F_RAWI),
    ROW_IDX_FIELD_COMPOUND_RAW(OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32,
                               "specialized: packed_arr[rk].field OP= rawr (narrow)", F_RAWR),

    ROW_IDX_FIELD_GET_RAW(OP_INDEX_FIELD_GET_RAW_INT32_UNCHECKED,
                          "specialized+loop-proven-safe: rawi = packed_arr[rk].field (narrow)",
                          F_RAWI),
    ROW_IDX_FIELD_GET_RAW(OP_INDEX_FIELD_GET_RAW_FLOAT32_UNCHECKED,
                          "specialized+loop-proven-safe: rawr = packed_arr[rk].field (narrow)",
                          F_RAWR),
    ROW_IDX_FIELD_SET_RAW(OP_INDEX_FIELD_SET_RAW_INT32_UNCHECKED,
                          "specialized+loop-proven-safe: packed_arr[rk].field = rawi (narrow)",
                          F_RAWI),
    ROW_IDX_FIELD_SET_RAW(OP_INDEX_FIELD_SET_RAW_FLOAT32_UNCHECKED,
                          "specialized+loop-proven-safe: packed_arr[rk].field = rawr (narrow)",
                          F_RAWR),
    ROW_IDX_FIELD_COMPOUND_RAW(OP_INDEX_FIELD_COMPOUND_RAW_INT32_UNCHECKED,
                               "specialized+loop-proven-safe: packed_arr[rk].field OP= rawi (narrow)",
                               F_RAWI),
    ROW_IDX_FIELD_COMPOUND_RAW(OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED,
                               "specialized+loop-proven-safe: packed_arr[rk].field OP= rawr (narrow)",
                               F_RAWR),

    ROW_IDX_FIELD_COMPOUND_RAW(OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED_ADD,
                               "specialized: field += raw", F_RAWR),
    ROW_IDX_FIELD_COMPOUND_RAW(OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED_ADD,
                               "specialized: field += raw", F_RAWR),

    ROW(OP_FIELD_COMPOUND_RAW_FLOAT32_FMA, "specialized: struct.field += rawr * rawr (narrow)",
        {AT_A, F_REG}, {AT_B, F_RAWR}, {AT_C, F_RAWR}, {AT_W1, F_OFF}),
    ROW(OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED_FMA,
        "specialized+loop-proven-safe: packed_arr[rk].field += rawr * rawr", {AT_A, F_REG},
        {AT_B, F_RAWR}, {AT_C, F_RAWR}, {AT_W1_HI, F_OFF}, {AT_W1_LO, F_RK16}),

    ROW_CMP_JUMP(OP_EQ_JUMP_IF_FALSE, "jump if !(rk == rk)"),
    ROW_CMP_JUMP(OP_NEQ_JUMP_IF_FALSE, "jump if !(rk != rk)"),
    ROW_CMP_JUMP(OP_LT_JUMP_IF_FALSE, "jump if !(rk < rk)"),
    ROW_CMP_JUMP(OP_GT_JUMP_IF_FALSE, "jump if !(rk > rk)"),
    ROW_CMP_JUMP(OP_LTE_JUMP_IF_FALSE, "jump if !(rk <= rk)"),
    ROW_CMP_JUMP(OP_GTE_JUMP_IF_FALSE, "jump if !(rk >= rk)"),

    /* Variable-length: a header word, then part_count RK16 words, one per part. */
    [OP_INTERP] = {"OP_INTERP", "reg = one string built from N parts", {{0, F_END}}, true},
    [OP_INDEX_GET_INTERP] = {"OP_INDEX_GET_INTERP", "reg = dict[N-part key], key never allocated",
                             {{0, F_END}}, true},

    ROW_RAW_CMP_JUMP(OP_RAW_LT_INT_JUMP_IF_FALSE, "jump if !(rawi < rawi)", F_RAWI, F_RAWK_I),
    ROW_RAW_CMP_JUMP(OP_RAW_LTE_INT_JUMP_IF_FALSE, "jump if !(rawi <= rawi)", F_RAWI, F_RAWK_I),
    ROW_RAW_CMP_JUMP(OP_RAW_LT_REAL_JUMP_IF_FALSE, "jump if !(rawr < rawr)", F_RAWR, F_RAWK_D),
    ROW_RAW_CMP_JUMP(OP_RAW_LTE_REAL_JUMP_IF_FALSE, "jump if !(rawr <= rawr)", F_RAWR, F_RAWK_D),
    ROW_RAW_CMP_JUMP(OP_RAW_EQ_INT_JUMP_IF_FALSE, "jump if !(rawi == rawi)", F_RAWI, F_RAWK_I),
    ROW_RAW_CMP_JUMP(OP_RAW_NEQ_INT_JUMP_IF_FALSE, "jump if !(rawi != rawi)", F_RAWI, F_RAWK_I),
    ROW_RAW_CMP_JUMP(OP_RAW_EQ_REAL_JUMP_IF_FALSE, "jump if !(rawr == rawr)", F_RAWR, F_RAWK_D),
    ROW_RAW_CMP_JUMP(OP_RAW_NEQ_REAL_JUMP_IF_FALSE, "jump if !(rawr != rawr)", F_RAWR, F_RAWK_D),

    ROW(OP_INDEX_GET_RAW_INT, "rawi = arr[rk] (checked)", {AT_A, F_RAWI}, {AT_B, F_REG},
        {AT_C, F_RK8}),
    ROW(OP_INDEX_SET_RAW_INT, "arr[rk] = rawi (checked)", {AT_A, F_REG}, {AT_B, F_RK8},
        {AT_C, F_RAWI}),
    ROW(OP_INDEX_SET_RAW_REAL, "arr[rk] = rawr (checked)", {AT_A, F_REG}, {AT_B, F_RK8},
        {AT_C, F_RAWR}),
    ROW(OP_INDEX_GET_RAW_REAL, "rawr = arr[rk] (checked)", {AT_A, F_RAWR}, {AT_B, F_REG},
        {AT_C, F_RK8}),
    ROW(OP_INDEX_COMPOUND_RAW_INT, "fused: arr[rk] OP= rawi (checked)", {AT_A, F_REG},
        {AT_B, F_BINOP}, {AT_C, F_RK8}, {AT_W1, F_RAWI}),
    ROW(OP_INDEX_COMPOUND_RAW_REAL, "fused: arr[rk] OP= rawr (checked)", {AT_A, F_REG},
        {AT_B, F_BINOP}, {AT_C, F_RK8}, {AT_W1, F_RAWR}),
    ROW(OP_CALL_SELF, "recursive call into this same specialized body", {AT_A, F_REG},
        {AT_B, F_REG}, {AT_C, F_COUNT}),
    ROW(OP_TAIL_CALL_SELF, "tail call into this same specialized body, reuses this frame",
        {AT_A, F_REG}, {AT_B, F_REG}, {AT_C, F_COUNT}),
    ROW(OP_RAW_MATH_REAL, "rawr = math fn(rawr), never boxed", {AT_A, F_RAWR}, {AT_B, F_RAWR},
        {AT_C, F_FN_ID}),
    ROW(OP_RAW_INT_TO_REAL, "rawr = (real)rawi", {AT_A, F_RAWR}, {AT_B, F_RAWI}),
    ROW(OP_RAW_REAL_TO_INT, "rawi = (int)rawr, truncating", {AT_A, F_RAWI}, {AT_B, F_RAWR}),
};

#define AER_NAME_OF_MODULE(id, str, call) str,
static const char* const module_names[] = {AER_NATIVE_MODULES(AER_NAME_OF_MODULE) "dynamic"};
#undef AER_NAME_OF_MODULE
#define AER_NAME_OF_BUILTIN(id, str) str,
static const char* const builtin_names[] = {AER_BUILTINS(AER_NAME_OF_BUILTIN)};
#undef AER_NAME_OF_BUILTIN

/* NULL for a byte that names no opcode -- reachable only from a corrupt chunk or a walk that
   has lost sync, and in both cases printing beats indexing past the table. */
static const OpInfo* op_row(int op) {
    return (op >= 0 && op <= OP_INFO_MAX && op_info[op].name) ? &op_info[op] : NULL;
}

#define AER_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* A desynchronized walk decodes arbitrary words as operands, so every table-driven read is
   bounded: the dump has to stay printable long enough to report where it went wrong. */
static uint32_t code_at(Chunk* c, unsigned int i) {
    return i < c->count ? c->code[i] : 0;
}

static AerVal pool_at(Chunk* c, uint32_t i) {
    return i < c->pool_count ? c->pool[i] : aer_null();
}

static int64_t rawk_i_at(Chunk* c, uint32_t i) {
    return i < c->rawk_i_count ? c->rawk_i[i] : 0;
}

static double rawk_d_at(Chunk* c, uint32_t i) {
    return i < c->rawk_d_count ? c->rawk_d[i] : 0.0;
}

static const char* pool_name_at(Chunk* c, uint32_t i) {
    AerVal v = pool_at(c, i);
    return aer_type(v) == TYPE_STRING ? aer_as_string(v)->data : "?";
}

static const char* name_or_q(const char* const* names, size_t n, uint32_t i) {
    return i < n ? names[i] : "?";
}

static const char* opcode_name(int op) {
    const OpInfo* info = op_row(op);
    return info ? info->name : "?";
}

static const char* cast_name(int k) {
    switch (k) {
        case CAST_INTEGER: return "integer";
        case CAST_FLOAT: return "float";
        case CAST_BOOLEAN: return "boolean";
        default: return "?";
    }
}

/* One-line pool-constant rendering -- containers are never pool literals, so no recursion needed. */
static void print_pool_value(FILE* out, AerVal v) {
    switch (aer_type(v)) {
        case TYPE_NULL: fprintf(out, "null"); break;
        case TYPE_BOOLEAN: fprintf(out, "%s", aer_as_bool(v) ? "true" : "false"); break;
        case TYPE_INTEGER: fprintf(out, "%lld", (long long)aer_as_int(v)); break;
        case TYPE_REAL: {
            char buf[64];
            aer_format_real(aer_as_real(v), buf, sizeof(buf));
            fprintf(out, "%s", buf);
            break;
        }
        case TYPE_STRING:
            fprintf(out, "\"%.*s\"", (int)aer_as_string(v)->length, aer_as_string(v)->data);
            break;
        case TYPE_FUNCTION: fprintf(out, "<function@%u>", aer_as_function(v)->code_offset); break;
        default: fprintf(out, "<value>"); break;
    }
}

/* Which trailing word an operand reaches into; 0 for word0's own slices. */
static unsigned int operand_word(unsigned char at) {
    switch (at) {
        case AT_W1:
        case AT_W1_HI:
        case AT_W1_LO: return 1;
        case AT_W2:
        case AT_W2_HI:
        case AT_W2_LO: return 2;
        case AT_W3_HI:
        case AT_W3_LO: return 3;
        default: return 0;
    }
}

static uint32_t operand_value(Chunk* c, unsigned int offset, unsigned char at) {
    uint32_t w = code_at(c, offset + operand_word(at));
    switch (at) {
        case AT_A: return UNPACK_A(w);
        case AT_B: return UNPACK_B(w);
        case AT_C: return UNPACK_C(w);
        case AT_W16: return UNPACK_W16(w);
        case AT_W1_HI:
        case AT_W2_HI:
        case AT_W3_HI: return UNPACK_2X16_HI(w);
        case AT_W1_LO:
        case AT_W2_LO:
        case AT_W3_LO: return UNPACK_2X16_LO(w);
        default: return w;
    }
}

/* The one place an instruction's word count is decided. */
static unsigned int instruction_words(Chunk* c, unsigned int offset) {
    uint32_t w0 = code_at(c, offset);
    Opcode op = (Opcode)(w0 & 0xFF);
    const OpInfo* info = op_row(op);
    if (!info)
        return 1;
    if (info->variable) {
        if (op == OP_INTERP)
            return 1 + UNPACK_B(w0);
        if (op == OP_INDEX_GET_INTERP)
            return 1 + UNPACK_C(w0);
        return 1 + (unsigned int)UNPACK_STRUCT_HEADER_COUNT(w0) * 2;
    }
    unsigned int words = 1;
    for (int i = 0; i < MAX_OPERANDS && info->ops[i].fld != F_END; i++) {
        unsigned int reach = operand_word(info->ops[i].at) + 1;
        if (reach > words)
            words = reach;
    }
    return words;
}

static void print_operand(FILE* out, Chunk* c, unsigned int offset, Operand o) {
    uint32_t v = operand_value(c, offset, o.at);
    switch (o.fld) {
        case F_REG: fprintf(out, "  reg=%d", (int)v); break;
        case F_RK8:
            if (v & RK8_CONST_FLAG) {
                fprintf(out, "  rk=const:");
                print_pool_value(out, pool_at(c, v & RK8_INDEX_MASK));
            } else
                fprintf(out, "  rk=reg%u", v & RK8_INDEX_MASK);
            break;
        case F_RK16:
            if (v & RK16_CONST_FLAG) {
                fprintf(out, "  rk=const:");
                print_pool_value(out, pool_at(c, v & RK16_INDEX_MASK));
            } else
                fprintf(out, "  rk=reg%u", v & RK16_INDEX_MASK);
            break;
        case F_RAWI: fprintf(out, "  rawi=%d", (int)v); break;
        case F_RAWR: fprintf(out, "  rawr=%d", (int)v); break;
        case F_RAWK_I:
            if (v & RK8_CONST_FLAG)
                fprintf(out, "  rawi_const=%lld", (long long)rawk_i_at(c, v & RK8_INDEX_MASK));
            else
                fprintf(out, "  rawi=%u", v);
            break;
        case F_RAWK_D:
            if (v & RK8_CONST_FLAG)
                fprintf(out, "  rawr_const=%g", rawk_d_at(c, v & RK8_INDEX_MASK));
            else
                fprintf(out, "  rawr=%u", v);
            break;
        case F_RAWK_I_AT: fprintf(out, "  rawk_i=%lld", (long long)rawk_i_at(c, v)); break;
        case F_POOL:
            fprintf(out, "  val=");
            print_pool_value(out, pool_at(c, v));
            break;
        case F_POOL_RAWI: fprintf(out, "  val=%lld", (long long)rawk_i_at(c, v)); break;
        case F_POOL_RAWD: fprintf(out, "  val=%g", rawk_d_at(c, v)); break;
        case F_NAME: fprintf(out, "  name=%s", pool_name_at(c, v)); break;
        /* Jump operands are stored relative (patch_jump, parser.c); print where they land, since a
           raw delta is unreadable beside the absolute offsets in the left-hand column. */
        case F_JUMP:
            fprintf(out, "  -> %d",
                    (int)(offset + operand_word(o.at) + 1) + (int)(int32_t)v);
            break;
        case F_COUNT: fprintf(out, "  n=%d", (int)v); break;
        case F_BINOP: fprintf(out, "  op=%s", opcode_name((int)v)); break;
        case F_CAST: fprintf(out, "  %s", cast_name((int)v)); break;
        case F_OFF: fprintf(out, "  off=%u", v); break;
        case F_IMM32: fprintf(out, "  imm=%d", (int)(int32_t)v); break;
        case F_FN_ID: fprintf(out, "  fn_id=%u", v); break;
        case F_MODULE_ID: fprintf(out, "  id=%s", name_or_q(module_names, AER_LEN(module_names), v)); break;
        case F_MODULE_FN: fprintf(out, " fn_id=%d", (int)(int16_t)v); break;
        case F_BUILTIN_ID: fprintf(out, "  id=%s", name_or_q(builtin_names, AER_LEN(builtin_names), v)); break;
        case F_END: break;
    }
}

/* The three opcodes whose operand count lives in the instruction rather than in op_info. */
static void print_variable_operands(FILE* out, Chunk* c, unsigned int offset, Opcode op) {
    uint32_t w0 = c->code[offset];
    unsigned int pos = offset + 1;
    if (op == OP_INTERP || op == OP_INDEX_GET_INTERP) {
        unsigned int count = (op == OP_INTERP) ? UNPACK_B(w0) : UNPACK_C(w0);
        if (op == OP_INTERP)
            fprintf(out, "  reg=%u  parts=%u  [", UNPACK_A(w0), count);
        else
            fprintf(out, "  reg=%u  obj=r%u  parts=%u  [", UNPACK_A(w0), UNPACK_B(w0), count);
        for (unsigned int i = 0; i < count; i++) {
            uint32_t rk = code_at(c, pos++);
            fprintf(out, "%s", i ? ", " : "");
            if (!RK16_IS_CONST(rk))
                fprintf(out, "reg%u", (unsigned int)RK16_INDEX(rk));
            else if (op == OP_INTERP)
                fprintf(out, "const:%u", (unsigned int)RK16_INDEX(rk));
            else
                print_pool_value(out, pool_at(c, RK16_INDEX(rk)));
        }
        fprintf(out, "]");
        return;
    }

    int field_count = (int)UNPACK_STRUCT_HEADER_COUNT(w0);
    fprintf(out, "  name=%s fields=%d [",
            pool_name_at(c, UNPACK_STRUCT_HEADER_NAME(w0)), field_count);
    for (int i = 0; i < field_count; i++) {
        uint32_t name_default_word = code_at(c, pos++);
        /* Low byte is the ValueType tag, bit 0x100 the narrow (i/f-suffixed-literal) marker.
           Masking is required, not cosmetic: indexing aer_value_type_names[] with the unmasked
           word reads out of bounds the moment a narrow field's 0x100 bit is set. */
        uint32_t ftype_word = code_at(c, pos++);
        int ftype = (int)(ftype_word & 0xFF);
        if (i > 0)
            fprintf(out, ", ");
        fprintf(out, "%s", pool_name_at(c, UNPACK_2X16_HI(name_default_word)));
        if (ftype != TYPE_ANY)
            fprintf(out, ": %s%s", name_or_q(aer_value_type_names, TYPE_STRUCT, (uint32_t)ftype),
                    (ftype_word & 0x100) ? " (narrow)" : "");
        fprintf(out, "=");
        print_pool_value(out, pool_at(c, UNPACK_2X16_LO(name_default_word)));
    }
    fprintf(out, "]");
}

static unsigned int disassemble_one(Chunk* c, unsigned int offset, FILE* out) {
    uint32_t w0 = c->code[offset];
    /* Full 8-bit mask must match DISPATCH()'s exactly -- opcode is unambiguously its own byte. */
    Opcode op = (Opcode)(w0 & 0xFF);
    const OpInfo* info = op_row(op);
    if (!info) {
        fprintf(out, "%6u  %-47s  unknown opcode byte in word %08x\n", offset, "?", w0);
        return offset + 1;
    }
    /* %-47s must stay >= the longest Opcode enum member's name (currently
       OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED, 45 chars) -- a shorter width doesn't truncate,
       it just lets that one line's description column start later than every other line's, since
       printf only pads a short name, never cuts a long one. Bump this if a future opcode name
       exceeds it. */
    fprintf(out, "%6u  %-47s  %s", offset, opcode_name(op), info->desc);

    if (info->variable)
        print_variable_operands(out, c, offset, op);
    else
        for (int i = 0; i < MAX_OPERANDS && info->ops[i].fld != F_END; i++)
            print_operand(out, c, offset, info->ops[i]);

    if (c->debug_hits && offset < c->debug_hits_cap && c->debug_hits[offset] > 0) {
        unsigned int line = chunk_line_for_offset(c, offset);
        fprintf(out, "   [hits=%llu, line=%u]", c->debug_hits[offset], line);
    }
    fprintf(out, "\n");
    return offset + instruction_words(c, offset);
}

typedef struct {
    const char* name;
    uint64_t hits;
} NamedCount;

static int cmp_named_count_desc(const void* a, const void* b) {
    uint64_t ha = ((const NamedCount*)a)->hits, hb = ((const NamedCount*)b)->hits;
    return (ha < hb) - (ha > hb);
}

typedef struct {
    unsigned int line;
    uint64_t hits;
} LineCount;

static int cmp_line_count_desc(const void* a, const void* b) {
    uint64_t ha = ((const LineCount*)a)->hits, hb = ((const LineCount*)b)->hits;
    return (ha < hb) - (ha > hb);
}

/* Line marks and (in a profile build) dispatch counts both name real instruction starts without
   consulting op_info, so a row claiming too many words shows up as a start the walk stepped
   over. A row claiming too few often escapes: the walk decodes the trailing word as a one-word
   instruction and re-aligns on the next one. */
static void report_encoding_desync(Chunk* c, FILE* out, const unsigned char* visited,
                                   unsigned int walk_end) {
    unsigned int bad = 0;
    for (unsigned int i = 0; i < c->line_mark_count; i++) {
        unsigned int off = c->line_mark_offsets[i];
        if (off >= c->count || visited[off])
            continue;
        if (bad++ == 0)
            fprintf(out, "\n--- ENCODING DESYNC ---\n");
        if (bad <= 8)
            fprintf(out, "  offset %u begins source line %u but the walk steps over it\n", off,
                    c->line_mark_lines[i]);
    }
    if (c->debug_hits)
        for (unsigned int i = 0; i < c->debug_hits_cap && i < c->count; i++) {
            if (c->debug_hits[i] == 0 || visited[i])
                continue;
            if (bad++ == 0)
                fprintf(out, "\n--- ENCODING DESYNC ---\n");
            if (bad <= 8)
                fprintf(out, "  offset %u was dispatched but the walk steps over it\n", i);
        }
    if (walk_end != c->count) {
        if (bad++ == 0)
            fprintf(out, "\n--- ENCODING DESYNC ---\n");
        fprintf(out, "  the walk ended at word %u, not this chunk's %u\n", walk_end, c->count);
    }
    if (bad > 8)
        fprintf(out, "  ... and %u more\n", bad - 8);
}

void aer_disassemble(Chunk* c, FILE* out) {
    fprintf(out, "--- disassembly (%u words) ---\n", c->count);
    /* The leading number on each line below is that instruction's own WORD offset into this
       Chunk's flat code[] array -- not a meaningless address: it's the exact same numbering space
       every jump target ("-> N") refers to, so it can be used directly to follow a jump to the
       instruction it lands on. */
    fprintf(out,
            "(leading number = word offset into code[]; jump targets \"-> N\" refer to this same offset)\n");

    uint64_t op_totals[OP_INFO_MAX + 1] = {0};
    unsigned char* visited = xcalloc(c->count ? c->count : 1, 1);
    unsigned int offset = 0;
    while (offset < c->count) {
        visited[offset] = 1;
        if (c->debug_hits && offset < c->debug_hits_cap)
            op_totals[c->code[offset] & 0xFF] += c->debug_hits[offset];
        offset = disassemble_one(c, offset, out);
    }

    if (!c->debug_hits) { /* static-only dump if no run happened yet */
        report_encoding_desync(c, out, visited, offset);
        free(visited);
        return;
    }

    NamedCount by_op[OP_INFO_MAX + 1];
    int by_op_count = 0;
    for (int i = 0; i <= OP_INFO_MAX; i++)
        if (op_totals[i] > 0)
            by_op[by_op_count++] = (NamedCount){opcode_name(i), op_totals[i]};
    qsort(by_op, (size_t)by_op_count, sizeof(NamedCount), cmp_named_count_desc);

    fprintf(out, "\n--- per-opcode summary ---\n");
    for (int i = 0; i < by_op_count; i++)
        fprintf(out, "  %-36s %llu\n", by_op[i].name,
                by_op[i].hits); /* keep in sync with disassemble_one's own %-36s */

    /* Per-line rollup: bucket by the source line each hit instruction belongs to. */
    LineCount* by_line = xmalloc(sizeof(LineCount) * c->debug_hits_cap);
    int by_line_count = 0;
    for (unsigned int i = 0; i < c->debug_hits_cap; i++) {
        if (c->debug_hits[i] == 0)
            continue;
        unsigned int line = chunk_line_for_offset(c, i);
        int found = -1;
        for (int j = 0; j < by_line_count; j++)
            if (by_line[j].line == line) {
                found = j;
                break;
            }
        if (found >= 0)
            by_line[found].hits += c->debug_hits[i];
        else
            by_line[by_line_count++] = (LineCount){line, c->debug_hits[i]};
    }
    qsort(by_line, (size_t)by_line_count, sizeof(LineCount), cmp_line_count_desc);

    fprintf(out, "\n--- hot source lines ---\n");
    for (int i = 0; i < by_line_count; i++)
        fprintf(out, "  line %-6u %llu\n", by_line[i].line, by_line[i].hits);
    free(by_line);

    report_encoding_desync(c, out, visited, offset);
    free(visited);
}

/* Per-pool memory report                                              */

/* aer_gc_stats() only counts live cells, which understates real usage -- string/array/dict
   payloads are separate xmalloc'd allocations the pool doesn't track. */
void aer_debug_memory_report(FILE* out) {
    /* vm_current_heap(), not vm.c's own require_current_heap() -- that one lazily installs the
       bootstrap heap, which is the right thing for an allocation path but not for a read-only
       report. Nothing to describe if no VM ever ran. */
    VmHeap* heap = vm_current_heap();
    if (!heap) {
        fprintf(out, "\n--- memory ---\n(no active heap)\n");
        return;
    }
    fprintf(out, "\n--- memory ---\n");

    uint64_t str_hdr = 0, str_payload = 0;
    {
        Pool* p = &heap->string_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerString* s = (AerString*)(p->slabs[i] + (size_t)j * p->stride);
                if (s->gc_state & POOL_FREE)
                    continue;
                str_hdr += sizeof(AerString);
                str_payload += s->length;
            }
        }
    }
    fprintf(out, "  string   header %10llu B  payload %10llu B\n", str_hdr, str_payload);

    uint64_t arr_hdr = 0, arr_payload = 0;
    {
        Pool* p = &heap->array_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerArray* a = (AerArray*)(p->slabs[i] + (size_t)j * p->stride);
                if (a->gc_state & POOL_FREE)
                    continue;
                arr_hdr += sizeof(AerArray);
                arr_payload += (uint64_t)a->capacity * sizeof(AerVal);
            }
        }
    }
    fprintf(out, "  array    header %10llu B  payload %10llu B\n", arr_hdr, arr_payload);

    uint64_t dict_hdr = 0, dict_payload = 0;
    {
        Pool* p = &heap->dict_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerDict* d = (AerDict*)(p->slabs[i] + (size_t)j * p->stride);
                if (d->gc_state & POOL_FREE)
                    continue;
                dict_hdr += sizeof(AerDict);
                dict_payload += (uint64_t)d->map.capacity * sizeof(unsigned int) +
                                (uint64_t)d->map.dense_capacity * sizeof(HashTableEntry);
                for (unsigned int b = 0; b < d->map.count; b++)
                    dict_payload += d->map.dense[b].length + 1;
            }
        }
    }
    fprintf(out, "  dict     header %10llu B  payload %10llu B\n", dict_hdr, dict_payload);

    uint64_t fn_hdr = 0, fn_payload = 0;
    {
        Pool* p = &heap->function_pool;
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerFunction* f = (AerFunction*)(p->slabs[i] + (size_t)j * p->stride);
                if (f->gc_state & POOL_FREE)
                    continue;
                fn_hdr += sizeof(AerFunction);
                if (f->defaults)
                    fn_payload += (uint64_t)(f->arity - f->min_arity) * sizeof(AerVal);
            }
        }
    }
    fprintf(out, "  function header %10llu B  payload %10llu B\n", fn_hdr, fn_payload);

    /* header = the fixed per-cell reservation; payload = each instance's own Shape.instance_bytes.
       Now size-classed (struct_pools[], one per STRUCT_PAYLOAD_TIER_SIZE tier) rather than one
       pool sized for MAX_STRUCT_FIELDS worst-case every time -- the remaining gap is just each
       instance's own distance up to its tier's ceiling, not a flat 256-byte-regardless-of-shape
       tax anymore. */
    uint64_t struct_hdr = 0, struct_payload = 0;
    for (unsigned int t = 0; t < STRUCT_PAYLOAD_TIER_COUNT; t++) {
        Pool* p = &heap->struct_pools[t];
        for (unsigned int i = 0; i < p->slab_count; i++) {
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                AerStruct* s = (AerStruct*)(p->slabs[i] + (size_t)j * p->stride);
                if (s->gc_state & POOL_FREE)
                    continue;
                struct_hdr += p->stride;
                struct_payload += s->shape->instance_bytes;
            }
        }
    }
    fprintf(out, "  struct   reserved %9llu B  used %10llu B\n", struct_hdr, struct_payload);

    unsigned int live, minor, major;
    aer_gc_stats(&live, &minor, &major);
    fprintf(out, "  %u live cells, %u minor collections, %u major collections\n", live, minor, major);
}

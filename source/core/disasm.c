#ifdef AER_DEBUG_TOOLS
#include <stdlib.h>
#include <string.h>
#include "disasm.h"
#include "error.h"

/* Kinds of operand word this disassembler knows how to decode/print. */
typedef enum {
    FLD_END,      /* marks the end of an opcode's operand list */
    FLD_POOL,     /* pool index -- resolve and print the constant's own value */
    FLD_NAME,     /* pool index known to be a TYPE_STRING name -- print just the string, no quotes */
    FLD_JUMP,     /* absolute code offset this instruction may jump to */
    FLD_COUNT,    /* a raw integer (arg count, item count, arity...) */
    FLD_BINOP,    /* an Opcode value used as an operand (bin_op in a fused op) */
    FLD_CAST,     /* CAST_INTEGER/CAST_FLOAT/CAST_BOOLEAN */
    FLD_REG,      /* a plain register index (packed or wide -- a register number either way) */
    FLD_RK,       /* an RK-encoded operand: RK_CONST_FLAG set = a pool constant, else a register */
} Field;

#define MAX_FIELDS 6

typedef struct {
    const char* name;
    const char* desc;
    Field       fields[MAX_FIELDS];
    bool        variable;   /* true only for OP_DEFINE_STRUCT -- see disassemble_one */
    /* Total WORD count beyond word0 (i.e. total instruction word count - 1) -- used only by
       aer_disassemble's per-opcode hit-total pass to skip to the next instruction. Every opcode
       below is either fully special-cased in disassemble_one (this is its only use) or falls
       through to the generic display path (which also uses `packed`, see disassemble_one). */
    int trailing_words;
    /* How many of fields[] come packed in word0 -- only meaningful for the handful of opcodes
       that still use the generic display path at the bottom of disassemble_one. */
    int packed;
} OpInfo;

/* OP_RAW_GTE_INT_BOXED_JUMP_IF_FALSE is the last member of the Opcode enum (vm.h). */
#define OP_INFO_MAX OP_RAW_GTE_INT_BOXED_JUMP_IF_FALSE

static const OpInfo op_info[OP_INFO_MAX + 1] = {
    /* OP_ADD..OP_IN: one opcode per operator, whole instruction in one word (PACK3 + RK8 pair) --
       special-cased in disassemble_one via binary_op_dispatched(). */
    [OP_ADD] = { "OP_ADD", "reg = rk + rk" },
    [OP_SUB] = { "OP_SUB", "reg = rk - rk" },
    [OP_MUL] = { "OP_MUL", "reg = rk * rk" },
    [OP_DIV] = { "OP_DIV", "reg = rk / rk" },
    [OP_MOD] = { "OP_MOD", "reg = rk % rk" },
    [OP_FLOOR_DIV] = { "OP_FLOOR_DIV", "reg = rk // rk" },
    [OP_EQ]  = { "OP_EQ",  "reg = rk == rk" },
    [OP_NEQ] = { "OP_NEQ", "reg = rk != rk" },
    [OP_LT]  = { "OP_LT",  "reg = rk < rk" },
    [OP_GT]  = { "OP_GT",  "reg = rk > rk" },
    [OP_LTE] = { "OP_LTE", "reg = rk <= rk" },
    [OP_GTE] = { "OP_GTE", "reg = rk >= rk" },
    [OP_IN]  = { "OP_IN",  "reg = rk in rk" },
    [OP_BITWISE_AND] = { "OP_BITWISE_AND", "reg = rk & rk" },
    [OP_BITWISE_OR]  = { "OP_BITWISE_OR",  "reg = rk | rk" },
    [OP_BITWISE_XOR] = { "OP_BITWISE_XOR", "reg = rk ^ rk" },
    [OP_LSHIFT] = { "OP_LSHIFT", "reg = rk << rk" },
    [OP_RSHIFT] = { "OP_RSHIFT", "reg = rk >> rk" },

    /* Never dispatched standalone (parser tags / OP_UNARY-embedded), but FLD_BINOP rendering
       still reads their names from this table. */
    [OP_AND] = { "OP_AND" }, [OP_OR] = { "OP_OR" }, [OP_PIPE] = { "OP_PIPE" },
    [OP_NEGATE] = { "OP_NEGATE" }, [OP_NOT] = { "OP_NOT" }, [OP_BITWISE_NOT] = { "OP_BITWISE_NOT" },
    [OP_TO_STR] = { "OP_TO_STR" },

    /* word0: op only. word1: target (dedicated, blind-overwrite patchable). */
    [OP_JUMP] = { "OP_JUMP", "unconditional jump", {FLD_JUMP}, false, 1, 0 },
    [OP_DEFINE_STRUCT] = { "OP_DEFINE_STRUCT", "register a struct type (variable-length: header word, then that many field words)", {0}, true },
    [OP_HALT] = { "OP_HALT", "stop execution", {0}, false, 0, 0 },

    /* dest+pool_idx both fit word0 now (op(8)+dest(8)+pool_idx(16)) -- no trailing word. */
    [OP_LOADK] = { "OP_LOADK", "reg = pool constant", {FLD_REG, FLD_POOL}, false, 0, 2 },
    [OP_MOVE]  = { "OP_MOVE",  "reg = reg", {FLD_REG, FLD_REG}, false, 0, 2 },
    [OP_IS_RESULT] = { "OP_IS_RESULT", "reg = is-result(reg)", {FLD_REG, FLD_REG}, false, 0, 2 },
    /* Whole instruction in one word (PACK3 + RK8 pair) -- special-cased, no trailing wide fields. */
    [OP_BINARY] = { "OP_BINARY", "reg = rk OP rk" },
    /* word0: op+reg. word1: target (dedicated). */
    [OP_JUMP_IF_FALSE_REG] = { "OP_JUMP_IF_FALSE_REG", "jump if !reg, no pop", {FLD_REG, FLD_JUMP}, false, 1, 1 },
    /* word0: dest+arg_base+arg_count. word1: callee_offset. */
    [OP_CALL] = { "OP_CALL", "call by compile-time-resolved offset", {FLD_REG, FLD_REG, FLD_COUNT, FLD_JUMP, FLD_COUNT}, false, 2, 3 },
    /* word0: dest+arg_base+arg_count. word1: callee_reg (never patched). */
    [OP_CALL_VALUE] = { "OP_CALL_VALUE", "call a runtime function value held in a register", {0}, false, 1, 0 },
    [OP_TAIL_CALL] = { "OP_TAIL_CALL", "tail call by compile-time-resolved offset, reuses this frame", {FLD_REG, FLD_REG, FLD_COUNT, FLD_JUMP, FLD_COUNT}, false, 2, 3 },
    [OP_TAIL_CALL_VALUE] = { "OP_TAIL_CALL_VALUE", "tail call through a register value, reuses this frame", {0}, false, 1, 0 },
    [OP_RETURN] = { "OP_RETURN", "return reg to caller", {FLD_REG}, false, 0, 1 },
    /* word0: dest+arg_base+arg_count. word1: module_idx. word2: fn_idx. word3: module_id+fn_id. */
    [OP_CALL_MODULE]  = { "OP_CALL_MODULE",  "call a native or file-module function by (module, function) name", {0}, false, 3, 0 },
    /* word0: dest+arg_base+arg_count. word1: name_idx. word2: builtin_id. */
    [OP_CALL_BUILTIN] = { "OP_CALL_BUILTIN", "global builtin (length/print/etc.) by name", {0}, false, 2, 0 },
    [OP_ARRAY_NEW] = { "OP_ARRAY_NEW", "reg = new array from a contiguous reg range", {FLD_REG, FLD_REG, FLD_COUNT}, false, 0, 3 },
    /* Single-word RK8-packed family -- special-cased in disassemble_one like OP_BINARY. */
    [OP_INDEX_GET] = { "OP_INDEX_GET", "reg = reg[rk]" },
    [OP_INDEX_SET] = { "OP_INDEX_SET", "reg[rk] = rk" },
    [OP_TYPED_INDEX_GET_UNCHECKED] = { "OP_TYPED_INDEX_GET_UNCHECKED", "loop-proven-safe: reg = typed_arr[rk]" },
    [OP_TYPED_INDEX_SET_UNCHECKED] = { "OP_TYPED_INDEX_SET_UNCHECKED", "loop-proven-safe: typed_arr[rk] = rk" },
    [OP_DESTRUCTURE] = { "OP_DESTRUCTURE", "reg, reg = destructure(reg)" },
    /* word0: dest+arr_reg. word1: rk_start16+rk_end16. */
    [OP_SLICE_GET] = { "OP_SLICE_GET", "reg = reg[rk:rk]", {0}, false, 1, 0 },
    [OP_DICT_NEW]  = { "OP_DICT_NEW",  "reg = new dict from contiguous key/value reg pairs", {FLD_REG, FLD_REG, FLD_COUNT}, false, 0, 3 },
    /* word0: col+idx+item_dest. word1: end_target (dedicated). */
    [OP_ITER_NEXT_ARRAY] = { "OP_ITER_NEXT_ARRAY", "for-each step, array or dict-keys", {FLD_REG, FLD_REG, FLD_REG, FLD_JUMP}, false, 1, 3 },
    /* word0: col+idx+key_dest. word1: val_dest. word2: end_target. */
    [OP_ITER_NEXT_PAIR]  = { "OP_ITER_NEXT_PAIR",  "for-each step, dict key+value pairs", {0}, false, 2, 0 },
    [OP_ITER_RANGE_PREP] = { "OP_ITER_RANGE_PREP", "rotated range-for: once-before-loop check", {0}, false, 2, 0 },
    [OP_ITER_RANGE_LOOP] = { "OP_ITER_RANGE_LOOP", "rotated range-for: bottom-of-loop advance+check+branch-back (bounds snapshotted once, never re-validated)", {0}, false, 2, 0 },
    /* word0: dest+arg_base+arg_count. word1: type_name_idx. */
    [OP_STRUCT_NEW] = { "OP_STRUCT_NEW", "reg = new struct instance from a contiguous reg range", {0}, false, 1, 0 },
    /* word0: dest+struct_reg. word1: field_name_idx. */
    [OP_FIELD_GET]  = { "OP_FIELD_GET",  "reg = struct.field", {0}, false, 1, 0 },
    /* word0: struct_reg+rk_val16. word1: field_name_idx. */
    [OP_FIELD_SET]  = { "OP_FIELD_SET",  "struct.field = rk", {0}, false, 1, 0 },
    /* word0: dest+fill_reg+narrow_flag. word1: rk_count16. */
    [OP_ARRAY_REPEAT] = { "OP_ARRAY_REPEAT", "reg = [fill_reg; rk_count] (struct -> packed array, number -> typed array)", {0}, false, 1, 0 },
    /* word0: dest+obj_reg. word1: field_idx16+rk_idx16. */
    [OP_INDEX_FIELD_GET]  = { "OP_INDEX_FIELD_GET",  "fused: reg = reg[rk].field (packed or struct array)", {0}, false, 1, 0 },
    /* word0: obj_reg+rk_idx16. word1: field_idx16+rk_val16. */
    [OP_INDEX_FIELD_SET]  = { "OP_INDEX_FIELD_SET",  "fused: reg[rk].field = rk (packed or struct array)", {0}, false, 1, 0 },
    /* word0: obj_reg+bin_op. word1: field_idx16+rk_idx16. word2: rk_rhs16. */
    [OP_INDEX_FIELD_COMPOUND] = { "OP_INDEX_FIELD_COMPOUND", "fused: reg[rk].field OP= rk (resolved once, packed or struct array)", {0}, false, 2, 0 },
    /* Single-word RK8-packed -- special-cased. */
    [OP_UNARY] = { "OP_UNARY", "reg = unary_op(rk)" },
    [OP_CAST]  = { "OP_CAST",  "reg = cast(rk)" },
    /* word0: dest+struct_reg+bin_op. word1: field_idx16+rk16. */
    [OP_BINARY_FIELD] = { "OP_BINARY_FIELD", "fused: reg = rk OP struct.field (field on the right)", {0}, false, 1, 0 },
    [OP_FIELD_BINARY] = { "OP_FIELD_BINARY", "fused: reg = struct.field OP rk (field on the left)", {0}, false, 1, 0 },
    /* word0: struct_reg+bin_op. word1: field_idx16+rk16. */
    [OP_FIELD_COMPOUND] = { "OP_FIELD_COMPOUND", "fused: struct.field OP= rk (resolved once, no dest reg)", {0}, false, 1, 0 },
    /* word0: dest+a_reg+b_reg. word1: op1(hi16)+c_reg(lo16). word2: op2. */
    [OP_TYPED_ARRAY_CHAIN2] = { "OP_TYPED_ARRAY_CHAIN2", "fused: reg = (reg op1 reg) op2 reg (typed-array chain, runtime-checked)", {0}, false, 2, 0 },
    [OP_PRINT_REPL] = { "OP_PRINT_REPL", "shell mode: print reg unless null", {FLD_REG}, false, 0, 1 },

    /* Raw-arithmetic family -- special-cased below; fields[]/packed/trailing_words kept for
       documentation only except where noted (LOAD_INT/LOAD_REAL/LOAD_INT_POOL are 2 words). */
    [OP_RAW_LOAD_INT]      = { "OP_RAW_LOAD_INT",      "rawi = imm (full int32)", {0}, false, 1, 0 },
    [OP_RAW_LOAD_REAL]     = { "OP_RAW_LOAD_REAL",     "rawr = pool constant", {0}, false, 1, 0 },
    [OP_RAW_ADD_INT]       = { "OP_RAW_ADD_INT",       "rawi = rawi + rawi" },
    [OP_RAW_SUB_INT]       = { "OP_RAW_SUB_INT",       "rawi = rawi - rawi" },
    [OP_RAW_MUL_INT]       = { "OP_RAW_MUL_INT",       "rawi = rawi * rawi" },
    [OP_RAW_DIV_INT]       = { "OP_RAW_DIV_INT",       "rawr = rawi / rawi (int/int division always promotes to real)" },
    [OP_RAW_MOD_INT]       = { "OP_RAW_MOD_INT",       "rawi = rawi % rawi" },
    [OP_RAW_FLOOR_DIV_INT] = { "OP_RAW_FLOOR_DIV_INT", "rawi = floor(rawi / rawi)" },
    [OP_RAW_ADD_REAL]      = { "OP_RAW_ADD_REAL",      "rawr = rawr + rawr" },
    [OP_RAW_SUB_REAL]      = { "OP_RAW_SUB_REAL",      "rawr = rawr - rawr" },
    [OP_RAW_MUL_REAL]      = { "OP_RAW_MUL_REAL",      "rawr = rawr * rawr" },
    [OP_RAW_DIV_REAL]      = { "OP_RAW_DIV_REAL",      "rawr = rawr / rawr" },
    [OP_RAW_FMA_REAL]      = { "OP_RAW_FMA_REAL",      "rawr = rawr + rawr * rawr (fused mul-add dispatch, two roundings)" },
    [OP_RAW_FMS_REAL]      = { "OP_RAW_FMS_REAL",      "rawr = rawr - rawr * rawr (fused mul-sub dispatch, two roundings)" },
    [OP_RAW_LT_INT]        = { "OP_RAW_LT_INT",        "reg = rawi < rawi" },
    [OP_RAW_GT_INT]        = { "OP_RAW_GT_INT",        "reg = rawi > rawi" },
    [OP_RAW_LTE_INT]       = { "OP_RAW_LTE_INT",       "reg = rawi <= rawi" },
    [OP_RAW_GTE_INT]       = { "OP_RAW_GTE_INT",       "reg = rawi >= rawi" },
    [OP_RAW_LT_REAL]       = { "OP_RAW_LT_REAL",       "reg = rawr < rawr" },
    [OP_RAW_GT_REAL]       = { "OP_RAW_GT_REAL",       "reg = rawr > rawr" },
    [OP_RAW_LTE_REAL]      = { "OP_RAW_LTE_REAL",      "reg = rawr <= rawr" },
    [OP_RAW_GTE_REAL]      = { "OP_RAW_GTE_REAL",      "reg = rawr >= rawr" },
    [OP_BOX_INT]           = { "OP_BOX_INT",           "reg = box(rawi)" },
    [OP_BOX_REAL]          = { "OP_BOX_REAL",          "reg = box(rawr)" },
    [OP_RAW_MOVE_INT]      = { "OP_RAW_MOVE_INT",      "rawi = rawi" },
    [OP_RAW_MOVE_REAL]     = { "OP_RAW_MOVE_REAL",     "rawr = rawr" },
    [OP_RAW_ADD_INT_BOXED]  = { "OP_RAW_ADD_INT_BOXED",  "rawi += reg (tag-checked)" },
    [OP_RAW_SUB_INT_BOXED]  = { "OP_RAW_SUB_INT_BOXED",  "rawi -= reg (tag-checked)" },
    [OP_RAW_MUL_INT_BOXED]  = { "OP_RAW_MUL_INT_BOXED",  "rawi *= reg (tag-checked)" },
    [OP_RAW_ADD_REAL_BOXED] = { "OP_RAW_ADD_REAL_BOXED", "rawr += reg (tag-checked)" },
    [OP_RAW_SUB_REAL_BOXED] = { "OP_RAW_SUB_REAL_BOXED", "rawr -= reg (tag-checked)" },
    [OP_RAW_MUL_REAL_BOXED] = { "OP_RAW_MUL_REAL_BOXED", "rawr *= reg (tag-checked)" },
    [OP_RAW_ADD_REAL_BOXED_TO] = { "OP_RAW_ADD_REAL_BOXED_TO", "rawr = rawr + reg (tag-checked, non-destructive)" },
    [OP_RAW_MUL_REAL_BOXED_TO] = { "OP_RAW_MUL_REAL_BOXED_TO", "rawr = rawr * reg (tag-checked, non-destructive)" },
    [OP_RAW_LOAD_INT_POOL]  = { "OP_RAW_LOAD_INT_POOL",  "rawi = pool constant", {0}, false, 1, 0 },

    /* Shape-specialized field access -- see vm.h's own comment on this opcode family. All fully
       special-cased below (custom word layouts), fields[]/packed unused. */
    [OP_INDEX_FIELD_GET_RAW_INT]  = { "OP_INDEX_FIELD_GET_RAW_INT",  "specialized: rawi = packed_arr[rk].field", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_GET_RAW_REAL] = { "OP_INDEX_FIELD_GET_RAW_REAL", "specialized: rawr = packed_arr[rk].field", {0}, false, 1, 0 },
    [OP_FIELD_GET_RAW_INT]        = { "OP_FIELD_GET_RAW_INT",        "specialized: rawi = struct.field",         {0}, false, 1, 0 },
    [OP_FIELD_GET_RAW_REAL]       = { "OP_FIELD_GET_RAW_REAL",       "specialized: rawr = struct.field",         {0}, false, 1, 0 },
    [OP_INDEX_FIELD_SET_RAW_INT]  = { "OP_INDEX_FIELD_SET_RAW_INT",  "specialized: packed_arr[rk].field = rawi", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_SET_RAW_REAL] = { "OP_INDEX_FIELD_SET_RAW_REAL", "specialized: packed_arr[rk].field = rawr", {0}, false, 1, 0 },
    [OP_FIELD_SET_RAW_INT]        = { "OP_FIELD_SET_RAW_INT",        "specialized: struct.field = rawi",         {0}, false, 2, 0 },
    [OP_FIELD_SET_RAW_REAL]       = { "OP_FIELD_SET_RAW_REAL",       "specialized: struct.field = rawr",         {0}, false, 2, 0 },
    [OP_FIELD_COMPOUND_RAW_INT]        = { "OP_FIELD_COMPOUND_RAW_INT",        "specialized: struct.field OP= rawi", {0}, false, 2, 0 },
    [OP_FIELD_COMPOUND_RAW_REAL]       = { "OP_FIELD_COMPOUND_RAW_REAL",       "specialized: struct.field OP= rawr", {0}, false, 2, 0 },
    [OP_INDEX_FIELD_COMPOUND_RAW_INT]  = { "OP_INDEX_FIELD_COMPOUND_RAW_INT",  "specialized: packed_arr[rk].field OP= rawi", {0}, false, 2, 0 },
    [OP_INDEX_FIELD_COMPOUND_RAW_REAL] = { "OP_INDEX_FIELD_COMPOUND_RAW_REAL", "specialized: packed_arr[rk].field OP= rawr", {0}, false, 2, 0 },

    /* _UNCHECKED counterparts -- same word layouts as the 3 wide INDEX_FIELD_*_RAW_INT/REAL
       families above, decoded by the same branches below; see vm.h's own comment on this family
       for the compile-time proof that makes skipping vm_packed_raw_elem's index checks safe. */
    [OP_INDEX_FIELD_GET_RAW_INT_UNCHECKED]       = { "OP_INDEX_FIELD_GET_RAW_INT_UNCHECKED",       "specialized+loop-proven-safe: rawi = packed_arr[rk].field", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_GET_RAW_REAL_UNCHECKED]      = { "OP_INDEX_FIELD_GET_RAW_REAL_UNCHECKED",      "specialized+loop-proven-safe: rawr = packed_arr[rk].field", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_SET_RAW_INT_UNCHECKED]       = { "OP_INDEX_FIELD_SET_RAW_INT_UNCHECKED",       "specialized+loop-proven-safe: packed_arr[rk].field = rawi", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_SET_RAW_REAL_UNCHECKED]      = { "OP_INDEX_FIELD_SET_RAW_REAL_UNCHECKED",      "specialized+loop-proven-safe: packed_arr[rk].field = rawr", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_COMPOUND_RAW_INT_UNCHECKED]  = { "OP_INDEX_FIELD_COMPOUND_RAW_INT_UNCHECKED",  "specialized+loop-proven-safe: packed_arr[rk].field OP= rawi", {0}, false, 2, 0 },
    [OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED] = { "OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED", "specialized+loop-proven-safe: packed_arr[rk].field OP= rawr", {0}, false, 2, 0 },

    /* Narrow (int32/float32) counterparts of the whole family above -- same word layouts, just a
       4-byte field instead of 8. See vm.h's own comment on this opcode family. */
    [OP_INDEX_FIELD_GET_RAW_INT32]   = { "OP_INDEX_FIELD_GET_RAW_INT32",   "specialized: rawi = packed_arr[rk].field (narrow)", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_GET_RAW_FLOAT32] = { "OP_INDEX_FIELD_GET_RAW_FLOAT32", "specialized: rawr = packed_arr[rk].field (narrow)", {0}, false, 1, 0 },
    [OP_FIELD_GET_RAW_INT32]         = { "OP_FIELD_GET_RAW_INT32",         "specialized: rawi = struct.field (narrow)",         {0}, false, 1, 0 },
    [OP_FIELD_GET_RAW_FLOAT32]       = { "OP_FIELD_GET_RAW_FLOAT32",       "specialized: rawr = struct.field (narrow)",         {0}, false, 1, 0 },
    [OP_INDEX_FIELD_SET_RAW_INT32]   = { "OP_INDEX_FIELD_SET_RAW_INT32",   "specialized: packed_arr[rk].field = rawi (narrow)", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_SET_RAW_FLOAT32] = { "OP_INDEX_FIELD_SET_RAW_FLOAT32", "specialized: packed_arr[rk].field = rawr (narrow)", {0}, false, 1, 0 },
    [OP_FIELD_SET_RAW_INT32]         = { "OP_FIELD_SET_RAW_INT32",         "specialized: struct.field = rawi (narrow)",         {0}, false, 2, 0 },
    [OP_FIELD_SET_RAW_FLOAT32]       = { "OP_FIELD_SET_RAW_FLOAT32",       "specialized: struct.field = rawr (narrow)",         {0}, false, 2, 0 },
    [OP_FIELD_COMPOUND_RAW_INT32]         = { "OP_FIELD_COMPOUND_RAW_INT32",         "specialized: struct.field OP= rawi (narrow)", {0}, false, 2, 0 },
    [OP_FIELD_COMPOUND_RAW_FLOAT32]       = { "OP_FIELD_COMPOUND_RAW_FLOAT32",       "specialized: struct.field OP= rawr (narrow)", {0}, false, 2, 0 },
    [OP_INDEX_FIELD_COMPOUND_RAW_INT32]   = { "OP_INDEX_FIELD_COMPOUND_RAW_INT32",   "specialized: packed_arr[rk].field OP= rawi (narrow)", {0}, false, 2, 0 },
    [OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32] = { "OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32", "specialized: packed_arr[rk].field OP= rawr (narrow)", {0}, false, 2, 0 },

    [OP_INDEX_FIELD_GET_RAW_INT32_UNCHECKED]      = { "OP_INDEX_FIELD_GET_RAW_INT32_UNCHECKED",      "specialized+loop-proven-safe: rawi = packed_arr[rk].field (narrow)", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_GET_RAW_FLOAT32_UNCHECKED]    = { "OP_INDEX_FIELD_GET_RAW_FLOAT32_UNCHECKED",    "specialized+loop-proven-safe: rawr = packed_arr[rk].field (narrow)", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_SET_RAW_INT32_UNCHECKED]      = { "OP_INDEX_FIELD_SET_RAW_INT32_UNCHECKED",      "specialized+loop-proven-safe: packed_arr[rk].field = rawi (narrow)", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_SET_RAW_FLOAT32_UNCHECKED]    = { "OP_INDEX_FIELD_SET_RAW_FLOAT32_UNCHECKED",    "specialized+loop-proven-safe: packed_arr[rk].field = rawr (narrow)", {0}, false, 1, 0 },
    [OP_INDEX_FIELD_COMPOUND_RAW_INT32_UNCHECKED]  = { "OP_INDEX_FIELD_COMPOUND_RAW_INT32_UNCHECKED",  "specialized+loop-proven-safe: packed_arr[rk].field OP= rawi (narrow)", {0}, false, 2, 0 },
    [OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED] = { "OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED", "specialized+loop-proven-safe: packed_arr[rk].field OP= rawr (narrow)", {0}, false, 2, 0 },

    [OP_UNBOX_PARAM_INT]  = { "OP_UNBOX_PARAM_INT",  "specialized: rawi = unbox(reg) [untagged]" },
    [OP_UNBOX_PARAM_REAL] = { "OP_UNBOX_PARAM_REAL", "specialized: rawr = unbox(reg) [untagged]" },
    [OP_EQ_JUMP_IF_FALSE]  = { "OP_EQ_JUMP_IF_FALSE",  "jump if !(rk == rk)", {0}, false, 1, 0 },
    [OP_NEQ_JUMP_IF_FALSE] = { "OP_NEQ_JUMP_IF_FALSE", "jump if !(rk != rk)", {0}, false, 1, 0 },
    [OP_LT_JUMP_IF_FALSE]  = { "OP_LT_JUMP_IF_FALSE",  "jump if !(rk < rk)",  {0}, false, 1, 0 },
    [OP_GT_JUMP_IF_FALSE]  = { "OP_GT_JUMP_IF_FALSE",  "jump if !(rk > rk)",  {0}, false, 1, 0 },
    [OP_LTE_JUMP_IF_FALSE] = { "OP_LTE_JUMP_IF_FALSE", "jump if !(rk <= rk)", {0}, false, 1, 0 },
    [OP_GTE_JUMP_IF_FALSE] = { "OP_GTE_JUMP_IF_FALSE", "jump if !(rk >= rk)", {0}, false, 1, 0 },
    [OP_RAW_LT_INT_BOXED_JUMP_IF_FALSE]  = { "OP_RAW_LT_INT_BOXED_JUMP_IF_FALSE",  "jump if !(rawi < reg) (tag-checked)",  {0}, false, 1, 0 },
    [OP_RAW_GT_INT_BOXED_JUMP_IF_FALSE]  = { "OP_RAW_GT_INT_BOXED_JUMP_IF_FALSE",  "jump if !(rawi > reg) (tag-checked)",  {0}, false, 1, 0 },
    [OP_RAW_LTE_INT_BOXED_JUMP_IF_FALSE] = { "OP_RAW_LTE_INT_BOXED_JUMP_IF_FALSE", "jump if !(rawi <= reg) (tag-checked)", {0}, false, 1, 0 },
    [OP_RAW_GTE_INT_BOXED_JUMP_IF_FALSE] = { "OP_RAW_GTE_INT_BOXED_JUMP_IF_FALSE", "jump if !(rawi >= reg) (tag-checked)", {0}, false, 1, 0 },
    [OP_RAW_LT_INT_BOXED]   = { "OP_RAW_LT_INT_BOXED",   "reg = rawi < reg (tag-checked)" },
    [OP_RAW_GT_INT_BOXED]   = { "OP_RAW_GT_INT_BOXED",   "reg = rawi > reg (tag-checked)" },
    [OP_RAW_LTE_INT_BOXED]  = { "OP_RAW_LTE_INT_BOXED",  "reg = rawi <= reg (tag-checked)" },
    [OP_RAW_GTE_INT_BOXED]  = { "OP_RAW_GTE_INT_BOXED",  "reg = rawi >= reg (tag-checked)" },
    [OP_RAW_LT_REAL_BOXED]  = { "OP_RAW_LT_REAL_BOXED",  "reg = rawr < reg (tag-checked)" },
    [OP_RAW_GT_REAL_BOXED]  = { "OP_RAW_GT_REAL_BOXED",  "reg = rawr > reg (tag-checked)" },
    [OP_RAW_LTE_REAL_BOXED] = { "OP_RAW_LTE_REAL_BOXED", "reg = rawr <= reg (tag-checked)" },
    [OP_RAW_GTE_REAL_BOXED] = { "OP_RAW_GTE_REAL_BOXED", "reg = rawr >= reg (tag-checked)" },
};

static const char* cast_name(int k) {
    switch (k) {
        case CAST_INTEGER: return "integer";
        case CAST_FLOAT:   return "float";
        case CAST_BOOLEAN: return "boolean";
        default:           return "?";
    }
}

/* One-line pool-constant rendering -- containers are never pool literals, so no recursion needed. */
static void print_pool_value(FILE* out, AerVal v) {
    switch (aer_type(v)) {
        case TYPE_NULL:     fprintf(out, "null"); break;
        case TYPE_BOOLEAN:  fprintf(out, "%s", aer_as_bool(v) ? "true" : "false"); break;
        case TYPE_INTEGER:  fprintf(out, "%lld", (long long)aer_as_int(v)); break;
        case TYPE_REAL: {
            char buf[64];
            aer_format_real(aer_as_real(v), buf, sizeof(buf));
            fprintf(out, "%s", buf);
            break;
        }
        case TYPE_STRING:   fprintf(out, "\"%.*s\"", (int)aer_as_string(v)->length, aer_as_string(v)->data); break;
        case TYPE_FUNCTION: fprintf(out, "<function@%u>", aer_as_function(v)->code_offset); break;
        default:            fprintf(out, "<value>"); break;
    }
}

static const char* opcode_name(int op) {
    return (op >= 0 && op <= OP_INFO_MAX && op_info[op].name) ? op_info[op].name : "?";
}

/* Prints one already-extracted field value -- the caller has already pulled it out of whichever
   word/sub-field it lives in. */
static void print_field(FILE* out, Chunk* c, Field kind, int word) {
    switch (kind) {
        case FLD_POOL: fprintf(out, "  val="); print_pool_value(out, c->pool[word]); break;
        case FLD_NAME: fprintf(out, "  name=%s", aer_as_string(c->pool[word])->data); break;
        case FLD_JUMP: fprintf(out, "  -> %d", word); break;
        case FLD_COUNT: fprintf(out, "  n=%d", word); break;
        case FLD_BINOP: fprintf(out, "  op=%s", opcode_name(word)); break;
        case FLD_CAST: fprintf(out, "  %s", cast_name(word)); break;
        case FLD_REG: fprintf(out, "  reg=%d", word); break;
        case FLD_RK:
            if (word & RK_CONST_FLAG) { fprintf(out, "  rk=const:"); print_pool_value(out, c->pool[word & ~RK_CONST_FLAG]); }
            else                          fprintf(out, "  rk=reg%d", word);
            break;
        case FLD_END: break;
    }
}

/* RK16's own printer (1 flag + 15 index bits) -- the wire form most RK operands use now. */
static void print_rk16(FILE* out, Chunk* c, uint32_t rk) {
    if (rk & RK16_CONST_FLAG) { fprintf(out, "  rk=const:"); print_pool_value(out, c->pool[rk & RK16_INDEX_MASK]); }
    else                          fprintf(out, "  rk=reg%u", rk & RK16_INDEX_MASK);
}

/* print_rk16's narrower sibling for RK8 (1 flag + 7 index bits) -- the family sharing a packed
   word with a dest register and another RK operand. */
static void print_rk8(FILE* out, Chunk* c, uint32_t rk) {
    if (rk & RK8_CONST_FLAG) { fprintf(out, "  rk=const:"); print_pool_value(out, c->pool[rk & RK8_INDEX_MASK]); }
    else                        fprintf(out, "  rk=reg%u", rk & RK8_INDEX_MASK);
}

/* Raw-slot printers -- kept separate so a dump reader can tell int=/real= from reg=/rk=. */
static void print_rawi(FILE* out, int slot) { fprintf(out, "  rawi=%d", slot); }
static void print_rawr(FILE* out, int slot) { fprintf(out, "  rawr=%d", slot); }

/* The per-operator opcodes sharing the one-word iABC+RK8 shape -- decoded identically. */
static bool binary_op_dispatched(Opcode op) {
    switch (op) {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_FLOOR_DIV:
        case OP_EQ: case OP_NEQ: case OP_LT: case OP_GT: case OP_LTE: case OP_GTE: case OP_IN:
        case OP_BITWISE_AND: case OP_BITWISE_OR: case OP_BITWISE_XOR: case OP_LSHIFT: case OP_RSHIFT:
            return true;
        default:
            return false;
    }
}

static unsigned int disassemble_one(Chunk* c, unsigned int offset, FILE* out) {
    uint32_t op_word = c->code[offset];
    /* Full 8-bit mask must match DISPATCH()'s exactly -- opcode is unambiguously its own byte now. */
    Opcode op = (Opcode)(op_word & 0xFF);
    const OpInfo* info = &op_info[op];
    /* %-47s must stay >= the longest Opcode enum member's name (currently
       OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED, 45 chars) -- a shorter width doesn't truncate,
       it just lets that one line's description column start later than every other line's, since
       printf only pads a short name, never cuts a long one. Bump this if a future opcode name
       exceeds it. */
    fprintf(out, "%6u  %-47s  %s", offset, opcode_name(op), info->desc);

    unsigned int pos = offset + 1;
    if (info->variable) {
        /* header word already decoded via op_word; then (name+default, type) pairs -- 2 words per field. */
        int name_idx    = (int)UNPACK_STRUCT_HEADER_NAME(op_word);
        int field_count = (int)UNPACK_STRUCT_HEADER_COUNT(op_word);
        fprintf(out, "  name=%s fields=%d [", aer_as_string(c->pool[name_idx])->data, field_count);
        static const char* const field_type_names[] = {
            "null", "boolean", "integer", "float", "string", "function", "array", "hashtable"
        };
        for (int i = 0; i < field_count; i++) {
            uint32_t name_default_word = c->code[pos++];
            int fname_idx    = (int)UNPACK_2X16_HI(name_default_word);
            int fdefault_idx = (int)UNPACK_2X16_LO(name_default_word);
            /* Low byte is the ValueType tag, bit 0x100 is the narrow (`i`/`f`-suffixed-literal)
               marker -- see OP_DEFINE_STRUCT's real decode (vm.c) and parse_struct's own emission
               (parser.c). Masking this out is required, not cosmetic: indexing field_type_names[]
               with the raw (un-masked) word is an out-of-bounds read the moment a narrow field's
               0x100 bit is set. */
            uint32_t ftype_word = (uint32_t)c->code[pos++];
            int  ftype  = (int)(ftype_word & 0xFF);
            bool narrow = (ftype_word & 0x100) != 0;
            if (i > 0) fprintf(out, ", ");
            fprintf(out, "%s", aer_as_string(c->pool[fname_idx])->data);
            if (ftype != TYPE_ANY) fprintf(out, ": %s%s", field_type_names[ftype], narrow ? " (narrow)" : "");
            fprintf(out, "=");
            print_pool_value(out, c->pool[fdefault_idx]);
        }
        fprintf(out, "]");
    } else if (binary_op_dispatched(op)) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_rk8(out, c, UNPACK_B(op_word));
        print_rk8(out, c, UNPACK_C(op_word));
    } else if (op == OP_EQ_JUMP_IF_FALSE || op == OP_NEQ_JUMP_IF_FALSE || op == OP_LT_JUMP_IF_FALSE ||
               op == OP_GT_JUMP_IF_FALSE || op == OP_LTE_JUMP_IF_FALSE || op == OP_GTE_JUMP_IF_FALSE) {
        print_rk8(out, c, UNPACK_B(op_word));
        print_rk8(out, c, UNPACK_C(op_word));
        print_field(out, c, FLD_JUMP, (int)c->code[pos++]);
    } else if (op == OP_RAW_LT_INT_BOXED_JUMP_IF_FALSE || op == OP_RAW_GT_INT_BOXED_JUMP_IF_FALSE ||
               op == OP_RAW_LTE_INT_BOXED_JUMP_IF_FALSE || op == OP_RAW_GTE_INT_BOXED_JUMP_IF_FALSE) {
        print_rawi(out, (int)UNPACK_B(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_C(op_word));
        print_field(out, c, FLD_JUMP, (int)c->code[pos++]);
    } else if (op == OP_INDEX_GET || op == OP_TYPED_INDEX_GET_UNCHECKED) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
        print_rk8(out, c, UNPACK_C(op_word));
    } else if (op == OP_INDEX_SET || op == OP_TYPED_INDEX_SET_UNCHECKED) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_rk8(out, c, UNPACK_B(op_word));
        print_rk8(out, c, UNPACK_C(op_word));
    } else if (op == OP_DESTRUCTURE) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_C(op_word));
    } else if (op == OP_SLICE_GET) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
        uint32_t bounds_word = c->code[pos++];
        print_rk16(out, c, UNPACK_2X16_HI(bounds_word));
        print_rk16(out, c, UNPACK_2X16_LO(bounds_word));
    } else if (op == OP_FIELD_GET) {
        print_field(out, c, FLD_REG,  (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG,  (int)UNPACK_B(op_word));
        int field_idx = (int)c->code[pos++];
        print_field(out, c, FLD_NAME, field_idx);
    } else if (op == OP_FIELD_SET) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_rk16(out, c, UNPACK_W16(op_word));
        int field_idx = (int)c->code[pos++];
        print_field(out, c, FLD_NAME, field_idx);
    } else if (op == OP_ARRAY_REPEAT) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
        print_field(out, c, FLD_COUNT, (int)UNPACK_C(op_word));
        uint32_t count_word = c->code[pos++];
        print_rk16(out, c, count_word & 0xFFFFU);
    } else if (op == OP_INDEX_FIELD_GET) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
        uint32_t field_rk_word = c->code[pos++];
        print_field(out, c, FLD_NAME, (int)UNPACK_2X16_HI(field_rk_word));
        print_rk16(out, c, UNPACK_2X16_LO(field_rk_word));
    } else if (op == OP_INDEX_FIELD_SET) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_rk16(out, c, UNPACK_W16(op_word));
        uint32_t field_val_word = c->code[pos++];
        print_field(out, c, FLD_NAME, (int)UNPACK_2X16_HI(field_val_word));
        print_rk16(out, c, UNPACK_2X16_LO(field_val_word));
    } else if (op == OP_INDEX_FIELD_GET_RAW_INT || op == OP_INDEX_FIELD_GET_RAW_REAL
            || op == OP_INDEX_FIELD_GET_RAW_INT32 || op == OP_INDEX_FIELD_GET_RAW_FLOAT32
            || op == OP_INDEX_FIELD_GET_RAW_INT_UNCHECKED || op == OP_INDEX_FIELD_GET_RAW_REAL_UNCHECKED
            || op == OP_INDEX_FIELD_GET_RAW_INT32_UNCHECKED || op == OP_INDEX_FIELD_GET_RAW_FLOAT32_UNCHECKED) {
        bool is_int = (op == OP_INDEX_FIELD_GET_RAW_INT || op == OP_INDEX_FIELD_GET_RAW_INT32
                    || op == OP_INDEX_FIELD_GET_RAW_INT_UNCHECKED || op == OP_INDEX_FIELD_GET_RAW_INT32_UNCHECKED);
        if (is_int) print_rawi(out, (int)UNPACK_A(op_word)); else print_rawr(out, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
        uint32_t field_rk_word = c->code[pos++];
        fprintf(out, "  off=%u", UNPACK_2X16_HI(field_rk_word));
        print_rk16(out, c, UNPACK_2X16_LO(field_rk_word));
    } else if (op == OP_FIELD_GET_RAW_INT || op == OP_FIELD_GET_RAW_REAL
            || op == OP_FIELD_GET_RAW_INT32 || op == OP_FIELD_GET_RAW_FLOAT32) {
        bool is_int = (op == OP_FIELD_GET_RAW_INT || op == OP_FIELD_GET_RAW_INT32);
        if (is_int) print_rawi(out, (int)UNPACK_A(op_word)); else print_rawr(out, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
        unsigned int foffset = c->code[pos++];
        fprintf(out, "  off=%u", foffset);
    } else if (op == OP_INDEX_FIELD_SET_RAW_INT || op == OP_INDEX_FIELD_SET_RAW_REAL
            || op == OP_INDEX_FIELD_SET_RAW_INT32 || op == OP_INDEX_FIELD_SET_RAW_FLOAT32
            || op == OP_INDEX_FIELD_SET_RAW_INT_UNCHECKED || op == OP_INDEX_FIELD_SET_RAW_REAL_UNCHECKED
            || op == OP_INDEX_FIELD_SET_RAW_INT32_UNCHECKED || op == OP_INDEX_FIELD_SET_RAW_FLOAT32_UNCHECKED) {
        bool is_int = (op == OP_INDEX_FIELD_SET_RAW_INT || op == OP_INDEX_FIELD_SET_RAW_INT32
                    || op == OP_INDEX_FIELD_SET_RAW_INT_UNCHECKED || op == OP_INDEX_FIELD_SET_RAW_INT32_UNCHECKED);
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_rk16(out, c, UNPACK_W16(op_word));
        uint32_t off_slot_word = c->code[pos++];
        fprintf(out, "  off=%u", UNPACK_2X16_HI(off_slot_word));
        if (is_int) print_rawi(out, (int)UNPACK_2X16_LO(off_slot_word)); else print_rawr(out, (int)UNPACK_2X16_LO(off_slot_word));
    } else if (op == OP_FIELD_SET_RAW_INT || op == OP_FIELD_SET_RAW_REAL
            || op == OP_FIELD_SET_RAW_INT32 || op == OP_FIELD_SET_RAW_FLOAT32) {
        bool is_int = (op == OP_FIELD_SET_RAW_INT || op == OP_FIELD_SET_RAW_INT32);
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        unsigned int foffset = c->code[pos++];
        fprintf(out, "  off=%u", foffset);
        int slot = (int)c->code[pos++];
        if (is_int) print_rawi(out, slot); else print_rawr(out, slot);
    } else if (op == OP_INDEX_FIELD_COMPOUND) {
        print_field(out, c, FLD_REG,   (int)UNPACK_A(op_word));
        print_field(out, c, FLD_BINOP, (int)UNPACK_B(op_word));
        uint32_t field_idx_word = c->code[pos++];
        print_field(out, c, FLD_NAME, (int)UNPACK_2X16_HI(field_idx_word));
        print_rk16(out, c, UNPACK_2X16_LO(field_idx_word));
        uint32_t rhs_word = c->code[pos++];
        print_rk16(out, c, UNPACK_2X16_LO(rhs_word));
    } else if (op == OP_ITER_NEXT_ARRAY) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_C(op_word));
        int target = (int)c->code[pos++];
        print_field(out, c, FLD_JUMP, target);
    } else if (op == OP_ITER_NEXT_PAIR || op == OP_ITER_RANGE_PREP || op == OP_ITER_RANGE_LOOP) {
        /* 3 regs in word0, a 4th register its own trailing word, then a dedicated target word. */
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_C(op_word));
        int fourth_reg = (int)c->code[pos++];
        print_field(out, c, FLD_REG, fourth_reg);
        int target = (int)c->code[pos++];
        print_field(out, c, FLD_JUMP, target);
    } else if (op == OP_CALL_VALUE || op == OP_TAIL_CALL_VALUE) {
        /* callee_reg is never a patched target, so nothing else trails. */
        print_field(out, c, FLD_REG,   (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_B(op_word));
        print_field(out, c, FLD_COUNT, (int)UNPACK_C(op_word));
        int callee_reg = (int)c->code[pos++];
        print_field(out, c, FLD_REG, callee_reg);
    } else if (op == OP_UNARY) {
        print_field(out, c, FLD_REG,   (int)UNPACK_A(op_word));
        print_field(out, c, FLD_BINOP, (int)UNPACK_B(op_word));
        print_rk8(out, c, UNPACK_C(op_word));
    } else if (op == OP_CAST) {
        print_field(out, c, FLD_REG,  (int)UNPACK_A(op_word));
        print_field(out, c, FLD_CAST, (int)UNPACK_B(op_word));
        print_rk8(out, c, UNPACK_C(op_word));
    } else if (op == OP_STRUCT_NEW) {
        print_field(out, c, FLD_REG,   (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_B(op_word));
        print_field(out, c, FLD_COUNT, (int)UNPACK_C(op_word));
        int name_idx = (int)c->code[pos++];
        print_field(out, c, FLD_NAME, name_idx);
    } else if (op == OP_CALL_MODULE) {
        static const char* const call_module_id_names[] = {
            "math", "random", "string", "time", "json", "collection",
            "net", "regex", "actor", "scheduler", "dynamic"
        };
        print_field(out, c, FLD_REG,   (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_B(op_word));
        print_field(out, c, FLD_COUNT, (int)UNPACK_C(op_word));
        int module_idx = (int)c->code[pos++];
        int fn_idx     = (int)c->code[pos++];
        print_field(out, c, FLD_NAME, module_idx);
        print_field(out, c, FLD_NAME, fn_idx);
        uint32_t ids_word = c->code[pos++];
        int module_id = (int)UNPACK_2X16_HI(ids_word);
        int fn_id     = (int16_t)UNPACK_2X16_LO(ids_word);
        fprintf(out, "  id=%s fn_id=%d", call_module_id_names[module_id], fn_id);
    } else if (op == OP_CALL_BUILTIN) {
        static const char* const call_builtin_id_names[] = {
            "length", "print", "type", "assert", "panic", "Result"
        };
        print_field(out, c, FLD_REG,   (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_B(op_word));
        print_field(out, c, FLD_COUNT, (int)UNPACK_C(op_word));
        int name_idx = (int)c->code[pos++];
        print_field(out, c, FLD_NAME, name_idx);
        int builtin_id = (int)c->code[pos++];
        fprintf(out, "  id=%s", call_builtin_id_names[builtin_id]);
    } else if (op == OP_BINARY_FIELD) {
        print_field(out, c, FLD_REG,   (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_B(op_word));
        print_field(out, c, FLD_BINOP, (int)UNPACK_C(op_word));
        uint32_t field_rk_word = c->code[pos++];
        print_field(out, c, FLD_NAME, (int)UNPACK_2X16_HI(field_rk_word));
        print_rk16(out, c, UNPACK_2X16_LO(field_rk_word));
    } else if (op == OP_FIELD_BINARY) {
        print_field(out, c, FLD_REG,   (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_B(op_word));
        print_field(out, c, FLD_BINOP, (int)UNPACK_C(op_word));
        uint32_t field_rk_word = c->code[pos++];
        print_field(out, c, FLD_NAME, (int)UNPACK_2X16_HI(field_rk_word));
        print_rk16(out, c, UNPACK_2X16_LO(field_rk_word));
    } else if (op == OP_FIELD_COMPOUND) {
        print_field(out, c, FLD_REG,   (int)UNPACK_A(op_word));
        print_field(out, c, FLD_BINOP, (int)UNPACK_B(op_word));
        uint32_t field_rk_word = c->code[pos++];
        print_field(out, c, FLD_NAME, (int)UNPACK_2X16_HI(field_rk_word));
        print_rk16(out, c, UNPACK_2X16_LO(field_rk_word));
    } else if (op == OP_TYPED_ARRAY_CHAIN2) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_C(op_word));
        uint32_t word1 = c->code[pos++];
        print_field(out, c, FLD_BINOP, (int)UNPACK_2X16_HI(word1));
        print_field(out, c, FLD_REG,   (int)UNPACK_2X16_LO(word1));
        uint32_t op2 = c->code[pos++];
        print_field(out, c, FLD_BINOP, (int)op2);
    } else if (op == OP_RAW_LOAD_INT) {
        print_rawi(out, (int)UNPACK_A(op_word));
        int32_t imm = (int32_t)c->code[pos++];
        fprintf(out, "  imm=%d", imm);
    } else if (op == OP_RAW_LOAD_REAL) {
        print_rawr(out, (int)UNPACK_A(op_word));
        unsigned int pool_idx = c->code[pos++];
        fprintf(out, "  val=");
        print_pool_value(out, c->pool[pool_idx]);
    } else if (op == OP_RAW_ADD_INT || op == OP_RAW_SUB_INT || op == OP_RAW_MUL_INT ||
               op == OP_RAW_DIV_INT || op == OP_RAW_MOD_INT || op == OP_RAW_FLOOR_DIV_INT) {
        /* OP_RAW_DIV_INT alone writes raw_reals[] (int/int division promotes) -- dest printer differs. */
        if (op == OP_RAW_DIV_INT) print_rawr(out, (int)UNPACK_A(op_word));
        else                      print_rawi(out, (int)UNPACK_A(op_word));
        print_rawi(out, (int)UNPACK_B(op_word));
        print_rawi(out, (int)UNPACK_C(op_word));
    } else if (op == OP_RAW_ADD_REAL || op == OP_RAW_SUB_REAL || op == OP_RAW_MUL_REAL || op == OP_RAW_DIV_REAL ||
               op == OP_RAW_FMA_REAL || op == OP_RAW_FMS_REAL) {
        print_rawr(out, (int)UNPACK_A(op_word));
        print_rawr(out, (int)UNPACK_B(op_word));
        print_rawr(out, (int)UNPACK_C(op_word));
    } else if (op == OP_RAW_LT_INT || op == OP_RAW_GT_INT || op == OP_RAW_LTE_INT || op == OP_RAW_GTE_INT) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_rawi(out, (int)UNPACK_B(op_word));
        print_rawi(out, (int)UNPACK_C(op_word));
    } else if (op == OP_RAW_LT_REAL || op == OP_RAW_GT_REAL || op == OP_RAW_LTE_REAL || op == OP_RAW_GTE_REAL) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_rawr(out, (int)UNPACK_B(op_word));
        print_rawr(out, (int)UNPACK_C(op_word));
    } else if (op == OP_BOX_INT) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_rawi(out, (int)UNPACK_B(op_word));
    } else if (op == OP_BOX_REAL) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_rawr(out, (int)UNPACK_B(op_word));
    } else if (op == OP_UNBOX_PARAM_INT) {
        print_rawi(out, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
    } else if (op == OP_UNBOX_PARAM_REAL) {
        print_rawr(out, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
    } else if (op == OP_RAW_MOVE_INT) {
        print_rawi(out, (int)UNPACK_A(op_word));
        print_rawi(out, (int)UNPACK_B(op_word));
    } else if (op == OP_RAW_MOVE_REAL) {
        print_rawr(out, (int)UNPACK_A(op_word));
        print_rawr(out, (int)UNPACK_B(op_word));
    } else if (op == OP_RAW_ADD_INT_BOXED || op == OP_RAW_SUB_INT_BOXED || op == OP_RAW_MUL_INT_BOXED) {
        print_rawi(out, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
    } else if (op == OP_RAW_ADD_REAL_BOXED || op == OP_RAW_SUB_REAL_BOXED || op == OP_RAW_MUL_REAL_BOXED) {
        print_rawr(out, (int)UNPACK_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_B(op_word));
    } else if (op == OP_RAW_ADD_REAL_BOXED_TO || op == OP_RAW_MUL_REAL_BOXED_TO) {
        print_rawr(out, (int)UNPACK_A(op_word));
        print_rawr(out, (int)UNPACK_B(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_C(op_word));
    } else if (op == OP_FIELD_COMPOUND_RAW_INT || op == OP_FIELD_COMPOUND_RAW_REAL
            || op == OP_FIELD_COMPOUND_RAW_INT32 || op == OP_FIELD_COMPOUND_RAW_FLOAT32) {
        bool is_int = (op == OP_FIELD_COMPOUND_RAW_INT || op == OP_FIELD_COMPOUND_RAW_INT32);
        print_field(out, c, FLD_REG,   (int)UNPACK_A(op_word));
        print_field(out, c, FLD_BINOP, (int)UNPACK_B(op_word));
        unsigned int foffset = c->code[pos++];
        fprintf(out, "  off=%u", foffset);
        int slot = (int)c->code[pos++];
        if (is_int) print_rawi(out, slot); else print_rawr(out, slot);
    } else if (op == OP_INDEX_FIELD_COMPOUND_RAW_INT || op == OP_INDEX_FIELD_COMPOUND_RAW_REAL
            || op == OP_INDEX_FIELD_COMPOUND_RAW_INT32 || op == OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32
            || op == OP_INDEX_FIELD_COMPOUND_RAW_INT_UNCHECKED || op == OP_INDEX_FIELD_COMPOUND_RAW_REAL_UNCHECKED
            || op == OP_INDEX_FIELD_COMPOUND_RAW_INT32_UNCHECKED || op == OP_INDEX_FIELD_COMPOUND_RAW_FLOAT32_UNCHECKED) {
        bool is_int = (op == OP_INDEX_FIELD_COMPOUND_RAW_INT || op == OP_INDEX_FIELD_COMPOUND_RAW_INT32
                    || op == OP_INDEX_FIELD_COMPOUND_RAW_INT_UNCHECKED || op == OP_INDEX_FIELD_COMPOUND_RAW_INT32_UNCHECKED);
        print_field(out, c, FLD_REG,   (int)UNPACK_A(op_word));
        print_field(out, c, FLD_BINOP, (int)UNPACK_B(op_word));
        uint32_t field_rk_word = c->code[pos++];
        fprintf(out, "  off=%u", UNPACK_2X16_HI(field_rk_word));
        print_rk16(out, c, UNPACK_2X16_LO(field_rk_word));
        int slot = (int)c->code[pos++];
        if (is_int) print_rawi(out, slot); else print_rawr(out, slot);
    } else if (op == OP_RAW_LOAD_INT_POOL) {
        print_rawi(out, (int)UNPACK_A(op_word));
        unsigned int pool_idx = c->code[pos++];
        fprintf(out, "  val=");
        print_pool_value(out, c->pool[pool_idx]);
    } else if (op == OP_RAW_LT_INT_BOXED || op == OP_RAW_GT_INT_BOXED ||
               op == OP_RAW_LTE_INT_BOXED || op == OP_RAW_GTE_INT_BOXED) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_rawi(out, (int)UNPACK_B(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_C(op_word));
    } else if (op == OP_RAW_LT_REAL_BOXED || op == OP_RAW_GT_REAL_BOXED ||
               op == OP_RAW_LTE_REAL_BOXED || op == OP_RAW_GTE_REAL_BOXED) {
        print_field(out, c, FLD_REG, (int)UNPACK_A(op_word));
        print_rawr(out, (int)UNPACK_B(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_C(op_word));
    } else {
        /* Generic path -- the handful of opcodes whose fields all fit the plain PACK3 shape
           (op+up to 3 byte fields) with nothing trailing, or nothing at all. */
        int i = 0;
        for (; i < info->packed; i++) {
            int word = (i == 0) ? (int)UNPACK_A(op_word) : (i == 1) ? (int)UNPACK_B(op_word) : (int)UNPACK_C(op_word);
            print_field(out, c, info->fields[i], word);
        }
        for (; i < MAX_FIELDS && info->fields[i] != FLD_END; i++) {
            int word = (int)c->code[pos++];
            print_field(out, c, info->fields[i], word);
        }
    }

    if (c->debug_hits && offset < c->debug_hits_cap && c->debug_hits[offset] > 0) {
        unsigned int line = chunk_line_for_offset(c, offset);
        fprintf(out, "   [hits=%llu, line=%u]", c->debug_hits[offset], line);
    }
    fprintf(out, "\n");
    return pos;
}

typedef struct { const char* name; uint64_t hits; } NamedCount;

static int cmp_named_count_desc(const void* a, const void* b) {
    uint64_t ha = ((const NamedCount*)a)->hits, hb = ((const NamedCount*)b)->hits;
    return (ha < hb) - (ha > hb);
}

typedef struct { unsigned int line; uint64_t hits; } LineCount;

static int cmp_line_count_desc(const void* a, const void* b) {
    uint64_t ha = ((const LineCount*)a)->hits, hb = ((const LineCount*)b)->hits;
    return (ha < hb) - (ha > hb);
}

void aer_disassemble(Chunk* c, FILE* out) {
    fprintf(out, "--- disassembly (%u words) ---\n", c->count);
    /* The leading number on each line below is that instruction's own WORD offset into this
       Chunk's flat code[] array -- not a meaningless address: it's the exact same numbering space
       every jump target ("-> N") refers to, so it can be used directly to follow a jump to the
       instruction it lands on. */
    fprintf(out, "(leading number = word offset into code[]; jump targets \"-> N\" refer to this same offset)\n");
    unsigned int offset = 0;
    while (offset < c->count) offset = disassemble_one(c, offset, out);

    if (!c->debug_hits) return;   /* static-only dump if no run happened yet */

    NamedCount by_op[OP_INFO_MAX + 1];
    int by_op_count = 0;
    uint64_t op_totals[OP_INFO_MAX + 1] = {0};
    offset = 0;
    while (offset < c->count) {
        Opcode op = (Opcode)(c->code[offset] & 0xFF);
        if (offset < c->debug_hits_cap) op_totals[op] += c->debug_hits[offset];
        const OpInfo* info = &op_info[op];
        unsigned int next;
        if (info->variable) {
            uint32_t header = c->code[offset];
            int field_count = (int)UNPACK_STRUCT_HEADER_COUNT(header);
            next = offset + 1 + (unsigned int)field_count * 2;
        } else {
            next = offset + 1 + (unsigned int)info->trailing_words;
        }
        offset = next;
    }
    for (int i = 0; i <= OP_INFO_MAX; i++)
        if (op_totals[i] > 0) by_op[by_op_count++] = (NamedCount){ opcode_name(i), op_totals[i] };
    qsort(by_op, (size_t)by_op_count, sizeof(NamedCount), cmp_named_count_desc);

    fprintf(out, "\n--- per-opcode summary ---\n");
    for (int i = 0; i < by_op_count; i++)
        fprintf(out, "  %-36s %llu\n", by_op[i].name, by_op[i].hits);   /* keep in sync with disassemble_one's own %-36s */

    /* Per-line rollup: bucket by the source line each hit instruction belongs to. */
    LineCount* by_line = xmalloc(sizeof(LineCount) * c->debug_hits_cap);
    int by_line_count = 0;
    for (unsigned int i = 0; i < c->debug_hits_cap; i++) {
        if (c->debug_hits[i] == 0) continue;
        unsigned int line = chunk_line_for_offset(c, i);
        int found = -1;
        for (int j = 0; j < by_line_count; j++)
            if (by_line[j].line == line) { found = j; break; }
        if (found >= 0) by_line[found].hits += c->debug_hits[i];
        else            by_line[by_line_count++] = (LineCount){ line, c->debug_hits[i] };
    }
    qsort(by_line, (size_t)by_line_count, sizeof(LineCount), cmp_line_count_desc);

    fprintf(out, "\n--- hot source lines ---\n");
    for (int i = 0; i < by_line_count; i++)
        fprintf(out, "  line %-6u %llu\n", by_line[i].line, by_line[i].hits);
    free(by_line);
}

#endif

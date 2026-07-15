#ifdef AER_DEBUG_TOOLS
#include <stdlib.h>
#include <string.h>
#include "disasm.h"
#include "error.h"

/* Kinds of operand word this disassembler knows how to decode/print. */
typedef enum {
    FLD_END,      /* marks the end of an opcode's operand list */
    FLD_POOL,     /* pool index — resolve and print the constant's own value */
    FLD_NAME,     /* pool index known to be a TYPE_STRING name — print just the string, no quotes */
    FLD_JUMP,     /* absolute code offset this instruction may jump to */
    FLD_COUNT,    /* a raw integer (arg count, item count, arity...) */
    FLD_BINOP,    /* an Opcode value used as an operand (bin_op in a fused op, or CMP_JUMP_FALSE's comparison) */
    FLD_CAST,     /* CAST_INTEGER/CAST_FLOAT/CAST_BOOLEAN */
    FLD_REG,      /* a plain register index (packed or wide — a register number either way) */
    FLD_RK,       /* an RK-encoded operand: RK_CONST_FLAG set = a pool constant, else a register */
} Field;

#define MAX_FIELDS 6

typedef struct {
    const char* name;
    const char* desc;
    Field       fields[MAX_FIELDS];
    bool        variable;   /* true only for OP_DEFINE_STRUCT — see disassemble_one */
    /* How many of fields[] (always the FIRST `packed` of them) come from the CURRENT instruction's
       own descriptor word (UNPACK_A/B/C, vm.h) instead of a separate word of their own — 0 for
       an opcode with no packed fields at all (OP_JUMP, OP_DEFINE_STRUCT, OP_HALT). See
       disassemble_one's own comment for the full reasoning. */
    int packed;
} OpInfo;

/* OP_RAW_LOAD_INT_POOL is the last member of the Opcode enum (vm.h) — the "primitive pass" raw-
   arithmetic family appended after OP_PRINT_REPL. */
#define OP_INFO_MAX OP_RAW_LOAD_INT_POOL

static const OpInfo op_info[OP_INFO_MAX + 1] = {
    /* OP_ADD..OP_RSHIFT/OP_IN are now real top-level dispatch targets, one opcode per operator
       (true single-level dispatch — see PACK_BINARY's own comment, vm.h) instead of riding along
       as a bin_op TAG inside a shared OP_BINARY word. Each gets a full entry, special-cased below
       in disassemble_one exactly like OP_BINARY used to be (dest + both RK operands all live in
       the one descriptor word, nothing trails) — see binary_op_dispatched(). `fields`/`packed`
       aren't actually read for special-cased opcodes (only `.name`/`.desc` are), kept here purely
       for documentation, matching the convention every other packed opcode in this table uses. */
    [OP_ADD] = { "OP_ADD", "reg = rk + rk", {FLD_REG}, false, 1 },
    [OP_SUB] = { "OP_SUB", "reg = rk - rk", {FLD_REG}, false, 1 },
    [OP_MUL] = { "OP_MUL", "reg = rk * rk", {FLD_REG}, false, 1 },
    [OP_DIV] = { "OP_DIV", "reg = rk / rk", {FLD_REG}, false, 1 },
    [OP_MOD] = { "OP_MOD", "reg = rk % rk", {FLD_REG}, false, 1 },
    [OP_FLOOR_DIV] = { "OP_FLOOR_DIV", "reg = rk // rk", {FLD_REG}, false, 1 },
    [OP_EQ]  = { "OP_EQ",  "reg = rk == rk", {FLD_REG}, false, 1 },
    [OP_NEQ] = { "OP_NEQ", "reg = rk != rk", {FLD_REG}, false, 1 },
    [OP_LT]  = { "OP_LT",  "reg = rk < rk",  {FLD_REG}, false, 1 },
    [OP_GT]  = { "OP_GT",  "reg = rk > rk",  {FLD_REG}, false, 1 },
    [OP_LTE] = { "OP_LTE", "reg = rk <= rk", {FLD_REG}, false, 1 },
    [OP_GTE] = { "OP_GTE", "reg = rk >= rk", {FLD_REG}, false, 1 },
    [OP_IN]  = { "OP_IN",  "reg = rk in rk", {FLD_REG}, false, 1 },
    [OP_BITWISE_AND] = { "OP_BITWISE_AND", "reg = rk & rk",  {FLD_REG}, false, 1 },
    [OP_BITWISE_OR]  = { "OP_BITWISE_OR",  "reg = rk | rk",  {FLD_REG}, false, 1 },
    [OP_BITWISE_XOR] = { "OP_BITWISE_XOR", "reg = rk ^ rk",  {FLD_REG}, false, 1 },
    [OP_LSHIFT] = { "OP_LSHIFT", "reg = rk << rk", {FLD_REG}, false, 1 },
    [OP_RSHIFT] = { "OP_RSHIFT", "reg = rk >> rk", {FLD_REG}, false, 1 },

    /* OP_AND/OP_OR/OP_PIPE/OP_NEGATE/OP_NOT/OP_BITWISE_NOT/OP_TO_STR are never dispatched as a
       standalone instruction — OP_AND/OP_OR/OP_PIPE purely as parser.c operator-token lookup tags
       (see their own comment, vm.h), the unary ones only ever embedded as a unary_op TAG inside
       OP_UNARY's packed word. They still need a `.name`-only entry here: print_field's FLD_BINOP
       case calls opcode_name(word) to render an embedded tag like that, which reads straight out
       of this table. Their `desc`/`fields` are never used (disassemble_one only reaches those for
       an instruction actually fetched via DISPATCH(), which these opcode values never are), so
       left blank. */
    [OP_AND] = { "OP_AND" }, [OP_OR] = { "OP_OR" }, [OP_PIPE] = { "OP_PIPE" },
    [OP_NEGATE] = { "OP_NEGATE" }, [OP_NOT] = { "OP_NOT" }, [OP_BITWISE_NOT] = { "OP_BITWISE_NOT" },
    [OP_TO_STR] = { "OP_TO_STR" },

    [OP_JUMP]          = { "OP_JUMP",          "unconditional jump", {FLD_JUMP} },
    [OP_DEFINE_STRUCT] = { "OP_DEFINE_STRUCT", "register a struct type (variable-length: name, field count, then that many field/default pairs)", {FLD_NAME, FLD_COUNT}, true },
    [OP_HALT]          = { "OP_HALT",          "stop execution", {0} },

    /* Register-VM opcodes. `packed` narrow fields (register indices, bin_op/unary_op/cast_type
       tags) come from the instruction's own descriptor word — see disassemble_one's own comment;
       everything after them in fields[] is a WIDE word exactly like OP_JUMP/OP_DEFINE_STRUCT/
       OP_HALT above. FLD_JUMP is reused for callee_offset (OP_CALL/OP_DEFER_PUSH) too — a
       function entry point is exactly as absolute-code-address-shaped as a jump target for
       disassembly purposes. */
    [OP_LOADK] = { "OP_LOADK", "reg = pool constant", {FLD_REG, FLD_POOL}, false, 1 },
    [OP_MOVE]  = { "OP_MOVE",  "reg = reg", {FLD_REG, FLD_REG}, false, 2 },
    /* OP_BINARY is the sole exception to the "packed fields come from A/B/C, everything else is a
       separate wide word" rule described above: its whole instruction (dest, bin_op, AND both RK
       operands) is packed into ONE word (PACK_BINARY, vm.h), so it has no trailing wide fields at
       all — disassemble_one special-cases it rather than going through the generic field loop. */
    [OP_BINARY] = { "OP_BINARY", "reg = rk OP rk", {FLD_REG, FLD_BINOP}, false, 2 },
    [OP_JUMP_IF_FALSE_REG] = { "OP_JUMP_IF_FALSE_REG", "jump if !reg, no pop", {FLD_REG, FLD_JUMP}, false, 1 },
    /* cmp_op/rk_a/rk_b all packed into one word (PACK_CMP_JUMP_FALSE, vm.h); the loop/if condition
       target stays its own dedicated word regardless — same patchable-target rule as everywhere
       else. Special-cased in disassemble_one. */
    [OP_CMP_JUMP_FALSE]    = { "OP_CMP_JUMP_FALSE",    "fused: jump if !(rk <op> rk)", {FLD_BINOP, FLD_RK, FLD_RK, FLD_JUMP}, false, 3 },
    [OP_CALL]  = { "OP_CALL",  "call by compile-time-resolved offset", {FLD_REG, FLD_REG, FLD_COUNT, FLD_JUMP}, false, 3 },
    /* OP_CALL_VALUE/OP_TAIL_CALL_VALUE pack all 4 fields into one word (PACK_REG4, vm.h) —
       callee_reg is always a plain register, never a patched target, unlike OP_CALL's
       callee_offset (see emit_call_value's own comment) — special-cased in disassemble_one. */
    [OP_CALL_VALUE] = { "OP_CALL_VALUE", "call a runtime function value held in a register", {FLD_REG, FLD_REG, FLD_COUNT, FLD_REG}, false, 4 },
    [OP_TAIL_CALL]       = { "OP_TAIL_CALL",       "tail call by compile-time-resolved offset, reuses this frame", {FLD_REG, FLD_REG, FLD_COUNT, FLD_JUMP}, false, 3 },
    [OP_TAIL_CALL_VALUE] = { "OP_TAIL_CALL_VALUE", "tail call through a register value, reuses this frame", {FLD_REG, FLD_REG, FLD_COUNT, FLD_REG}, false, 4 },
    [OP_CALL_GLOBAL_VALUE]      = { "OP_CALL_GLOBAL_VALUE",      "call a runtime function value held in the top-level frame's reg", {FLD_REG, FLD_REG, FLD_COUNT, FLD_REG}, false, 3 },
    [OP_TAIL_CALL_GLOBAL_VALUE] = { "OP_TAIL_CALL_GLOBAL_VALUE", "tail call through the top-level frame's reg, reuses this frame", {FLD_REG, FLD_REG, FLD_COUNT, FLD_REG}, false, 3 },
    [OP_RETURN] = { "OP_RETURN", "return reg to caller, drain pending defers first", {FLD_REG}, false, 1 },
    /* No patchable target at all — module/function/builtin names are always literal identifiers
       resolved at parse time — so everything packs into one word (PACK_CALL_MODULE/
       PACK_CALL_BUILTIN, vm.h). Special-cased in disassemble_one. */
    /* The trailing FLD_COUNT past `packed` fields in both entries below is a pure word-count
       placeholder for the per-opcode-summary walk (aer_disassemble) — the real trailing word
       (module_id/builtin_id) is decoded and printed specially in disassemble_one, not through the
       generic fields[] loop. Without it, that walk would think these opcodes are 1 word long
       instead of 2, misaligning every instruction after one — the same class of bug already found
       and fixed once in this file (see the opcode-mask fix, aer_disassemble). */
    [OP_CALL_MODULE]  = { "OP_CALL_MODULE",  "call a native or file-module function by (module, function) name", {FLD_REG, FLD_REG, FLD_COUNT, FLD_NAME, FLD_NAME, FLD_COUNT}, false, 5 },
    [OP_CALL_BUILTIN] = { "OP_CALL_BUILTIN", "global builtin (length/append/etc.) by name", {FLD_REG, FLD_REG, FLD_COUNT, FLD_NAME, FLD_COUNT}, false, 4 },
    [OP_LOAD_GLOBAL]  = { "OP_LOAD_GLOBAL",  "reg = top-level frame's reg (read-only)", {FLD_REG, FLD_REG}, false, 2 },
    [OP_STORE_GLOBAL] = { "OP_STORE_GLOBAL", "top-level frame's reg = rk", {FLD_REG, FLD_RK}, false, 2 },
    [OP_DEFER_PUSH]   = { "OP_DEFER_PUSH",   "snapshot args; run at this frame's OP_RETURN", {FLD_REG, FLD_COUNT, FLD_JUMP}, false, 2 },
    [OP_ARRAY_NEW] = { "OP_ARRAY_NEW", "reg = new array from a contiguous reg range", {FLD_REG, FLD_REG, FLD_COUNT}, false, 3 },
    /* OP_INDEX_GET/OP_ITER_NEXT_PAIR/OP_ITER_RANGE/OP_FIELD_GET/OP_FIELD_SET are the Slice A
       extension of OP_BINARY's single-word treatment (PACK_INDEX_GET/PACK_REG4/PACK_FIELD_GET/
       PACK_FIELD_SET, vm.h) — disassemble_one special-cases all of them below the same way it
       does OP_BINARY, since their RK/name fields no longer live in separate trailing words. Their
       `packed` count here still needs to equal fields[]'s full length (nothing genuinely trailing)
       so aer_disassemble's word-count accounting stays correct even though the generic per-field
       loop never actually runs for them. */
    [OP_INDEX_GET] = { "OP_INDEX_GET", "reg = reg[rk]", {FLD_REG, FLD_REG, FLD_RK}, false, 3 },
    [OP_INDEX_SET] = { "OP_INDEX_SET", "reg[rk] = rk", {FLD_REG, FLD_RK, FLD_RK}, false, 3 },
    [OP_SLICE_GET] = { "OP_SLICE_GET", "reg = reg[rk:rk]", {FLD_REG, FLD_REG, FLD_RK, FLD_RK}, false, 4 },
    [OP_CHECK_SHAPE] = { "OP_CHECK_SHAPE", "reg = check_shape(reg, type)", {FLD_REG, FLD_REG, FLD_NAME}, false, 3 },
    [OP_DICT_NEW]  = { "OP_DICT_NEW",  "reg = new dict from contiguous key/value reg pairs", {FLD_REG, FLD_REG, FLD_COUNT}, false, 3 },
    [OP_ITER_NEXT_ARRAY] = { "OP_ITER_NEXT_ARRAY", "for-each step, array or dict-keys", {FLD_REG, FLD_REG, FLD_REG, FLD_JUMP}, false, 3 },
    [OP_ITER_NEXT_PAIR]  = { "OP_ITER_NEXT_PAIR",  "for-each step, dict key+value pairs", {FLD_REG, FLD_REG, FLD_REG, FLD_REG, FLD_JUMP}, false, 4 },
    [OP_ITER_RANGE]      = { "OP_ITER_RANGE",      "for-each step, numeric a..b[..step] range", {FLD_REG, FLD_REG, FLD_REG, FLD_REG, FLD_JUMP}, false, 4 },
    [OP_STRUCT_NEW] = { "OP_STRUCT_NEW", "reg = new struct instance from a contiguous reg range", {FLD_REG, FLD_REG, FLD_COUNT, FLD_NAME}, false, 4 },
    [OP_FIELD_GET]  = { "OP_FIELD_GET",  "reg = struct.field", {FLD_REG, FLD_REG, FLD_NAME}, false, 3 },
    [OP_FIELD_SET]  = { "OP_FIELD_SET",  "struct.field = rk", {FLD_REG, FLD_NAME, FLD_RK}, false, 3 },
    [OP_UNARY] = { "OP_UNARY", "reg = unary_op(rk)", {FLD_REG, FLD_BINOP, FLD_RK}, false, 3 },
    [OP_CAST]  = { "OP_CAST",  "reg = cast(rk)", {FLD_REG, FLD_CAST, FLD_RK}, false, 3 },
    [OP_BINARY_FIELD] = { "OP_BINARY_FIELD", "fused: reg = rk OP struct.field (field on the right)", {FLD_REG, FLD_REG, FLD_BINOP, FLD_RK, FLD_NAME}, false, 5 },
    [OP_FIELD_BINARY] = { "OP_FIELD_BINARY", "fused: reg = struct.field OP rk (field on the left)", {FLD_REG, FLD_REG, FLD_BINOP, FLD_NAME, FLD_RK}, false, 5 },
    [OP_PRINT_REPL] = { "OP_PRINT_REPL", "shell mode: print reg unless null", {FLD_REG}, false, 1 },

    /* "Primitive pass" raw-arithmetic family (vm.h's OP_RAW_LOAD_INT comment) — all special-cased
       in disassemble_one below exactly like OP_UNARY/OP_CAST above (their own custom packed
       shape, not the generic fields[]-driven loop), so fields[]/packed here are unused and kept
       purely for documentation, same convention every other special-cased opcode's entry follows. */
    [OP_RAW_LOAD_INT]      = { "OP_RAW_LOAD_INT",      "rawi = imm", {FLD_REG}, false, 1 },
    [OP_RAW_LOAD_REAL]     = { "OP_RAW_LOAD_REAL",     "rawr = pool constant", {FLD_REG}, false, 1 },
    [OP_RAW_ADD_INT]       = { "OP_RAW_ADD_INT",       "rawi = rawi + rawi", {FLD_REG}, false, 1 },
    [OP_RAW_SUB_INT]       = { "OP_RAW_SUB_INT",       "rawi = rawi - rawi", {FLD_REG}, false, 1 },
    [OP_RAW_MUL_INT]       = { "OP_RAW_MUL_INT",       "rawi = rawi * rawi", {FLD_REG}, false, 1 },
    [OP_RAW_DIV_INT]       = { "OP_RAW_DIV_INT",       "rawr = rawi / rawi (int/int division always promotes to real)", {FLD_REG}, false, 1 },
    [OP_RAW_MOD_INT]       = { "OP_RAW_MOD_INT",       "rawi = rawi % rawi", {FLD_REG}, false, 1 },
    [OP_RAW_FLOOR_DIV_INT] = { "OP_RAW_FLOOR_DIV_INT", "rawi = floor(rawi / rawi)", {FLD_REG}, false, 1 },
    [OP_RAW_ADD_REAL]      = { "OP_RAW_ADD_REAL",      "rawr = rawr + rawr", {FLD_REG}, false, 1 },
    [OP_RAW_SUB_REAL]      = { "OP_RAW_SUB_REAL",      "rawr = rawr - rawr", {FLD_REG}, false, 1 },
    [OP_RAW_MUL_REAL]      = { "OP_RAW_MUL_REAL",      "rawr = rawr * rawr", {FLD_REG}, false, 1 },
    [OP_RAW_DIV_REAL]      = { "OP_RAW_DIV_REAL",      "rawr = rawr / rawr", {FLD_REG}, false, 1 },
    [OP_RAW_LT_INT]        = { "OP_RAW_LT_INT",        "reg = rawi < rawi", {FLD_REG}, false, 1 },
    [OP_RAW_GT_INT]        = { "OP_RAW_GT_INT",        "reg = rawi > rawi", {FLD_REG}, false, 1 },
    [OP_RAW_LTE_INT]       = { "OP_RAW_LTE_INT",       "reg = rawi <= rawi", {FLD_REG}, false, 1 },
    [OP_RAW_GTE_INT]       = { "OP_RAW_GTE_INT",       "reg = rawi >= rawi", {FLD_REG}, false, 1 },
    [OP_RAW_LT_REAL]       = { "OP_RAW_LT_REAL",       "reg = rawr < rawr", {FLD_REG}, false, 1 },
    [OP_RAW_GT_REAL]       = { "OP_RAW_GT_REAL",       "reg = rawr > rawr", {FLD_REG}, false, 1 },
    [OP_RAW_LTE_REAL]      = { "OP_RAW_LTE_REAL",      "reg = rawr <= rawr", {FLD_REG}, false, 1 },
    [OP_RAW_GTE_REAL]      = { "OP_RAW_GTE_REAL",      "reg = rawr >= rawr", {FLD_REG}, false, 1 },
    [OP_BOX_INT]           = { "OP_BOX_INT",           "reg = box(rawi)", {FLD_REG}, false, 1 },
    [OP_BOX_REAL]          = { "OP_BOX_REAL",          "reg = box(rawr)", {FLD_REG}, false, 1 },
    [OP_RAW_MOVE_INT]      = { "OP_RAW_MOVE_INT",      "rawi = rawi", {FLD_REG}, false, 1 },
    [OP_RAW_MOVE_REAL]     = { "OP_RAW_MOVE_REAL",     "rawr = rawr", {FLD_REG}, false, 1 },
    [OP_RAW_ADD_INT_BOXED]  = { "OP_RAW_ADD_INT_BOXED",  "rawi += reg (tag-checked)", {FLD_REG}, false, 1 },
    [OP_RAW_SUB_INT_BOXED]  = { "OP_RAW_SUB_INT_BOXED",  "rawi -= reg (tag-checked)", {FLD_REG}, false, 1 },
    [OP_RAW_MUL_INT_BOXED]  = { "OP_RAW_MUL_INT_BOXED",  "rawi *= reg (tag-checked)", {FLD_REG}, false, 1 },
    [OP_RAW_ADD_REAL_BOXED] = { "OP_RAW_ADD_REAL_BOXED", "rawr += reg (tag-checked)", {FLD_REG}, false, 1 },
    [OP_RAW_SUB_REAL_BOXED] = { "OP_RAW_SUB_REAL_BOXED", "rawr -= reg (tag-checked)", {FLD_REG}, false, 1 },
    [OP_RAW_MUL_REAL_BOXED] = { "OP_RAW_MUL_REAL_BOXED", "rawr *= reg (tag-checked)", {FLD_REG}, false, 1 },
    [OP_RAW_LOAD_INT_POOL]  = { "OP_RAW_LOAD_INT_POOL",  "rawi = pool constant", {FLD_REG}, false, 1 },
};

static const char* cast_name(int k) {
    switch (k) {
        case CAST_INTEGER: return "integer";
        case CAST_FLOAT:   return "float";
        case CAST_BOOLEAN: return "boolean";
        default:           return "?";
    }
}

/* Brief, one-line rendering of a pool constant — deliberately not the full recursive formatter
   vm.c's print()/interpolation use, since a struct/array/dict is never actually stored as a pool
   *literal* (those are always built at runtime by OP_ARRAY_NEW etc.) except a function value,
   which just gets a short tag here. */
static void print_pool_value(FILE* out, AerVal v) {
    switch (aer_type(v)) {
        case TYPE_NULL:     fprintf(out, "null"); break;
        case TYPE_BOOLEAN:  fprintf(out, "%s", aer_as_bool(v) ? "true" : "false"); break;
        case TYPE_INTEGER:  fprintf(out, "%lld", aer_as_int(v)); break;
        case TYPE_REAL:     fprintf(out, "%g", aer_as_real(v)); break;
        case TYPE_STRING:   fprintf(out, "\"%.*s\"", (int)aer_as_string(v)->length, aer_as_string(v)->data); break;
        case TYPE_FUNCTION: fprintf(out, "<function@%u>", aer_as_function(v)->code_offset); break;
        default:            fprintf(out, "<value>"); break;
    }
}

static const char* opcode_name(int op) {
    return (op >= 0 && op <= OP_INFO_MAX && op_info[op].name) ? op_info[op].name : "?";
}

/* Prints one field's already-extracted value, whether it came from a packed sub-field of the
   descriptor word or a separate wide word of its own — the caller (disassemble_one) handles
   telling the two apart; from here they're identical. */
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

/* Decodes and prints one instruction starting at c->code[offset]; returns the offset of the next
   instruction. OP_DEFINE_STRUCT is the sole variable-length exception (its field count is read
   from the operand stream itself, not known statically).
     Every register-VM instruction packs its narrow fields (registers, bin_op/unary_op/cast_type
   tags) into the SAME word as the opcode itself (PACK1/2/3, vm.h) — op_word is kept unmasked
   here specifically so those can still be extracted via UNPACK_A/B/C; OP_JUMP/OP_DEFINE_STRUCT/
   OP_HALT have packed==0, so the loop below is a no-op for them. */
static void print_rk20(FILE* out, Chunk* c, uint64_t rk) {
    if (rk & RK20_CONST_FLAG) { fprintf(out, "  rk=const:"); print_pool_value(out, c->pool[rk & RK20_INDEX_MASK]); }
    else                          fprintf(out, "  rk=reg%llu", rk & RK20_INDEX_MASK);
}

/* Same as print_rk20 above, for PACK_BINARY's narrower 9-bit RK operands (RK9_CONST_FLAG/
   RK9_INDEX_MASK, vm.h). */
static void print_rk9(FILE* out, Chunk* c, uint64_t rk) {
    if (rk & RK9_CONST_FLAG) { fprintf(out, "  rk=const:"); print_pool_value(out, c->pool[rk & RK9_INDEX_MASK]); }
    else                         fprintf(out, "  rk=reg%llu", rk & RK9_INDEX_MASK);
}

/* "Primitive pass" raw-slot printers — CallFrame.raw_ints/raw_reals indices, never RK-encoded
   (see PACK_RAW_ARITH_RR's own comment, vm.h, for why there's no register-vs-constant flag here
   at all). Kept as separate tiny helpers, one per array, purely so a reader scanning a
   disassembly dump can immediately tell a raw slot from a normal registers[] index (FLD_REG's
   "reg=%d") or an RK operand ("rk=..."). */
static void print_rawi(FILE* out, int slot) { fprintf(out, "  rawi=%d", slot); }
static void print_rawr(FILE* out, int slot) { fprintf(out, "  rawr=%d", slot); }

/* True for the 18 per-operator opcodes sharing PACK_BINARY's encoding (dest + both RK operands,
   all in the one descriptor word — see PACK_BINARY's own comment, vm.h). Each used to be a single
   bin_op TAG value inside a shared OP_BINARY word; now each is its own top-level dispatch target,
   but the word shape (and so the decoding needed here) is identical across all of them. */
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
    uint64_t op_word = c->code[offset];
    /* Masked to 7 bits (0x7F), matching DISPATCH()'s mask in vm.c exactly — must stay in sync,
       since PACK_BINARY's dest field now starts at bit 7 (vm.h); an 8-bit mask here would leak
       dest's LSB into the decoded opcode whenever dest is odd. */
    Opcode op = (Opcode)(op_word & 0x7F);
    const OpInfo* info = &op_info[op];
    fprintf(out, "%6u  %-28s  %s", offset, opcode_name(op), info->desc);

    unsigned int pos = offset + 1;
    if (info->variable) {
        /* OP_DEFINE_STRUCT: name pool idx, field_count, then field_count * (field-name, default,
           field-type) triples — the trailing type word was added by the typed-struct-fields
           feature (parse_struct, parser.c) but never wired up here, so this loop kept reading only
           2 words per field: every field after the first was decoded 1 word short, eventually
           reading an unrelated word (a field-type enum value, or a later opcode's own operand) as a
           pool index and crashing in aer_as_string. Found via gdb on a real segfault disassembling
           nbody.aer's own (untyped) Body struct. */
        int name_idx    = (int)c->code[pos++];
        int field_count = (int)c->code[pos++];
        fprintf(out, "  name=%s fields=%d [", aer_as_string(c->pool[name_idx])->data, field_count);
        static const char* const field_type_names[] = {
            "null", "boolean", "integer", "real", "string", "function", "array", "dict"
        };
        for (int i = 0; i < field_count; i++) {
            int fname_idx    = (int)c->code[pos++];
            int fdefault_idx = (int)c->code[pos++];
            int ftype        = (int)c->code[pos++];
            if (i > 0) fprintf(out, ", ");
            fprintf(out, "%s", aer_as_string(c->pool[fname_idx])->data);
            if (ftype != TYPE_ANY) fprintf(out, ": %s", field_type_names[ftype]);
            fprintf(out, "=");
            print_pool_value(out, c->pool[fdefault_idx]);
        }
        fprintf(out, "]");
    } else if (binary_op_dispatched(op)) {
        /* Packed single-word encoding (PACK_BINARY, vm.h) — dest and both RK operands live
           entirely within the low 32 bits of this same word (opcode:7 + dest:7 + RK9:9 + RK9:9),
           not the wider RK20 scheme every other packed opcode still uses, so they're decoded here
           via the narrower UNPACK_BINARY_DEST/UNPACK_RK_B9/C9 macros and print_rk9 (not print_rk20)
           rather than through the generic fields[]-driven loop below (which each of these opcodes'
           own op_info entry has no entries left for). There's no bin_op field to print anymore —
           the opcode itself (already printed via opcode_name() above) IS the operator. */
        print_field(out, c, FLD_REG, (int)UNPACK_BINARY_DEST(op_word));
        print_rk9(out, c, UNPACK_RK_B9(op_word));
        print_rk9(out, c, UNPACK_RK_C9(op_word));
    } else if (op == OP_INDEX_GET) {
        /* Tier 1, narrow RK9 encoding now (PACK_INDEX_GET's own comment, vm.h) — not RK20. */
        print_field(out, c, FLD_REG, (int)UNPACK_INDEX_GET_DEST(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_INDEX_GET_ARR(op_word));
        print_rk9(out, c, UNPACK_INDEX_GET_RK(op_word));
    } else if (op == OP_FIELD_GET) {
        print_field(out, c, FLD_REG,  (int)UNPACK_FIELD_GET_DEST(op_word));
        print_field(out, c, FLD_REG,  (int)UNPACK_FIELD_GET_STRUCT(op_word));
        print_field(out, c, FLD_NAME, (int)UNPACK_FIELD_GET_FIELD(op_word));
    } else if (op == OP_FIELD_SET) {
        /* Tier 1, narrow RK9 encoding now (PACK_FIELD_SET's own comment, vm.h) — not RK20. */
        print_field(out, c, FLD_REG,  (int)UNPACK_FIELD_SET_STRUCT(op_word));
        print_field(out, c, FLD_NAME, (int)UNPACK_FIELD_SET_FIELD(op_word));
        print_rk9(out, c, UNPACK_FIELD_SET_RK(op_word));
    } else if (op == OP_ITER_RANGE || op == OP_ITER_NEXT_PAIR) {
        /* 4 registers packed via PACK_REG4 (vm.h); the loop-exit target still gets its own
           trailing word regardless — a patchable jump target is never packed alongside anything
           else (see PACK_REG4's own comment), so pos must still advance past it here. */
        print_field(out, c, FLD_REG, (int)UNPACK_REG4_A(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_REG4_B(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_REG4_C(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_REG4_D(op_word));
        int target = (int)c->code[pos++];
        print_field(out, c, FLD_JUMP, target);
    } else if (op == OP_CALL_VALUE || op == OP_TAIL_CALL_VALUE) {
        /* See PACK_REG4's comment in vm.h / emit_call_value's own comment (parser.c) — callee_reg
           is never a patched target, so it packs alongside the other 3 fields with nothing left
           trailing. */
        print_field(out, c, FLD_REG,   (int)UNPACK_REG4_A(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_REG4_B(op_word));
        print_field(out, c, FLD_COUNT, (int)UNPACK_REG4_C(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_REG4_D(op_word));
    } else if (op == OP_STORE_GLOBAL) {
        /* Tier 1, narrow RK9 encoding now (PACK_STORE_GLOBAL's own comment, vm.h) — not RK20. */
        print_field(out, c, FLD_REG, (int)UNPACK_STORE_GLOBAL_REG(op_word));
        print_rk9(out, c, UNPACK_STORE_GLOBAL_RK(op_word));
    } else if (op == OP_UNARY) {
        print_field(out, c, FLD_REG,   (int)UNPACK_UNARY_DEST(op_word));
        print_field(out, c, FLD_BINOP, (int)UNPACK_UNARY_OP(op_word));
        print_rk9(out, c, UNPACK_UNARY_RK(op_word));
    } else if (op == OP_CAST) {
        print_field(out, c, FLD_REG,  (int)UNPACK_CAST_DEST(op_word));
        print_field(out, c, FLD_CAST, (int)UNPACK_CAST_TYPE(op_word));
        print_rk9(out, c, UNPACK_CAST_RK(op_word));
    } else if (op == OP_CHECK_SHAPE) {
        print_field(out, c, FLD_REG,  (int)UNPACK_CHECK_SHAPE_DEST(op_word));
        print_field(out, c, FLD_REG,  (int)UNPACK_CHECK_SHAPE_LHS(op_word));
        print_field(out, c, FLD_NAME, (int)UNPACK_CHECK_SHAPE_NAME(op_word));
    } else if (op == OP_STRUCT_NEW) {
        print_field(out, c, FLD_REG,   (int)UNPACK_STRUCT_NEW_DEST(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_STRUCT_NEW_ARG_BASE(op_word));
        print_field(out, c, FLD_COUNT, (int)UNPACK_STRUCT_NEW_ARG_COUNT(op_word));
        print_field(out, c, FLD_NAME,  (int)UNPACK_STRUCT_NEW_NAME(op_word));
    } else if (op == OP_CMP_JUMP_FALSE) {
        /* cmp_op/rk_a/rk_b packed (PACK_CMP_JUMP_FALSE, vm.h); the branch target still trails as
           its own word (patchable, never packed alongside anything else). */
        print_field(out, c, FLD_BINOP, (int)UNPACK_CMP_JUMP_OP(op_word));
        print_rk20(out, c, UNPACK_CMP_JUMP_RK_A(op_word));
        print_rk20(out, c, UNPACK_CMP_JUMP_RK_B(op_word));
        int target = (int)c->code[pos++];
        print_field(out, c, FLD_JUMP, target);
    } else if (op == OP_INDEX_SET) {
        print_field(out, c, FLD_REG, (int)UNPACK_INDEX_SET_ARR(op_word));
        print_rk20(out, c, UNPACK_INDEX_SET_IDX(op_word));
        print_rk20(out, c, UNPACK_INDEX_SET_VAL(op_word));
    } else if (op == OP_SLICE_GET) {
        print_field(out, c, FLD_REG, (int)UNPACK_SLICE_GET_DEST(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_SLICE_GET_ARR(op_word));
        print_rk20(out, c, UNPACK_SLICE_GET_START(op_word));
        print_rk20(out, c, UNPACK_SLICE_GET_END(op_word));
    } else if (op == OP_CALL_MODULE) {
        static const char* const call_module_id_names[] = {
            "math", "random", "string", "time", "json", "dynamic"
        };
        print_field(out, c, FLD_REG,   (int)UNPACK_CALL_MODULE_DEST(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_CALL_MODULE_ARG_BASE(op_word));
        print_field(out, c, FLD_COUNT, (int)UNPACK_CALL_MODULE_ARG_COUNT(op_word));
        print_field(out, c, FLD_NAME,  (int)UNPACK_CALL_MODULE_MODULE(op_word));
        print_field(out, c, FLD_NAME,  (int)UNPACK_CALL_MODULE_FN(op_word));
        /* Trailing word (see OP_CALL_MODULE's own comment, vm.h): module_id, resolved once at
           parse time so the VM can switch on it instead of running a strcmp chain per call. */
        int module_id = (int)c->code[pos++];
        fprintf(out, "  id=%s", call_module_id_names[module_id]);
    } else if (op == OP_CALL_BUILTIN) {
        static const char* const call_builtin_id_names[] = {
            "length", "delete", "append", "print", "type", "assert", "panic"
        };
        print_field(out, c, FLD_REG,   (int)UNPACK_CALL_BUILTIN_DEST(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_CALL_BUILTIN_ARG_BASE(op_word));
        print_field(out, c, FLD_COUNT, (int)UNPACK_CALL_BUILTIN_ARG_COUNT(op_word));
        print_field(out, c, FLD_NAME,  (int)UNPACK_CALL_BUILTIN_NAME(op_word));
        /* Trailing word (see OP_CALL_BUILTIN's own comment, vm.h): builtin_id, resolved once at
           parse time so the VM can switch on it instead of running a strcmp chain per call. */
        int builtin_id = (int)c->code[pos++];
        fprintf(out, "  id=%s", call_builtin_id_names[builtin_id]);
    } else if (op == OP_BINARY_FIELD) {
        print_field(out, c, FLD_REG,   (int)UNPACK_BINARY_FIELD_DEST(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_BINARY_FIELD_STRUCT(op_word));
        print_field(out, c, FLD_BINOP, (int)UNPACK_BINARY_FIELD_OP(op_word));
        print_rk20(out, c, UNPACK_BINARY_FIELD_RK(op_word));
        print_field(out, c, FLD_NAME,  (int)UNPACK_BINARY_FIELD_NAME(op_word));
    } else if (op == OP_FIELD_BINARY) {
        print_field(out, c, FLD_REG,   (int)UNPACK_FIELD_BINARY_DEST(op_word));
        print_field(out, c, FLD_REG,   (int)UNPACK_FIELD_BINARY_STRUCT(op_word));
        print_field(out, c, FLD_BINOP, (int)UNPACK_FIELD_BINARY_OP(op_word));
        print_field(out, c, FLD_NAME,  (int)UNPACK_FIELD_BINARY_NAME(op_word));
        print_rk20(out, c, UNPACK_FIELD_BINARY_RK(op_word));
    } else if (op == OP_RAW_LOAD_INT) {
        print_rawi(out, (int)UNPACK_RAW_LOAD_INT_DEST(op_word));
        fprintf(out, "  imm=%d", UNPACK_RAW_LOAD_INT_IMM(op_word));
    } else if (op == OP_RAW_LOAD_REAL) {
        print_rawr(out, (int)UNPACK_RAW_LOAD_REAL_DEST(op_word));
        fprintf(out, "  val=");
        print_pool_value(out, c->pool[UNPACK_RAW_LOAD_REAL_POOL(op_word)]);
    } else if (op == OP_RAW_ADD_INT || op == OP_RAW_SUB_INT || op == OP_RAW_MUL_INT ||
               op == OP_RAW_DIV_INT || op == OP_RAW_MOD_INT || op == OP_RAW_FLOOR_DIV_INT) {
        /* OP_RAW_DIV_INT is the sole exception: dest addresses raw_reals[], not raw_ints[] (int/
           int division always promotes to real — see this opcode's own comment, vm.c) — the two
           operands are still raw_ints[] either way, so only the dest printer differs here. */
        if (op == OP_RAW_DIV_INT) print_rawr(out, (int)UNPACK_RAW_ARITH_RR_DEST(op_word));
        else                      print_rawi(out, (int)UNPACK_RAW_ARITH_RR_DEST(op_word));
        print_rawi(out, (int)UNPACK_RAW_ARITH_RR_A(op_word));
        print_rawi(out, (int)UNPACK_RAW_ARITH_RR_B(op_word));
    } else if (op == OP_RAW_ADD_REAL || op == OP_RAW_SUB_REAL || op == OP_RAW_MUL_REAL || op == OP_RAW_DIV_REAL) {
        print_rawr(out, (int)UNPACK_RAW_ARITH_RR_DEST(op_word));
        print_rawr(out, (int)UNPACK_RAW_ARITH_RR_A(op_word));
        print_rawr(out, (int)UNPACK_RAW_ARITH_RR_B(op_word));
    } else if (op == OP_RAW_LT_INT || op == OP_RAW_GT_INT || op == OP_RAW_LTE_INT || op == OP_RAW_GTE_INT) {
        print_field(out, c, FLD_REG, (int)UNPACK_RAW_CMP_DEST(op_word));
        print_rawi(out, (int)UNPACK_RAW_CMP_A(op_word));
        print_rawi(out, (int)UNPACK_RAW_CMP_B(op_word));
    } else if (op == OP_RAW_LT_REAL || op == OP_RAW_GT_REAL || op == OP_RAW_LTE_REAL || op == OP_RAW_GTE_REAL) {
        print_field(out, c, FLD_REG, (int)UNPACK_RAW_CMP_DEST(op_word));
        print_rawr(out, (int)UNPACK_RAW_CMP_A(op_word));
        print_rawr(out, (int)UNPACK_RAW_CMP_B(op_word));
    } else if (op == OP_BOX_INT) {
        print_field(out, c, FLD_REG, (int)UNPACK_BOX_DEST(op_word));
        print_rawi(out, (int)UNPACK_BOX_SRC(op_word));
    } else if (op == OP_BOX_REAL) {
        print_field(out, c, FLD_REG, (int)UNPACK_BOX_DEST(op_word));
        print_rawr(out, (int)UNPACK_BOX_SRC(op_word));
    } else if (op == OP_RAW_MOVE_INT) {
        print_rawi(out, (int)UNPACK_RAW_MOVE_DEST(op_word));
        print_rawi(out, (int)UNPACK_RAW_MOVE_SRC(op_word));
    } else if (op == OP_RAW_MOVE_REAL) {
        print_rawr(out, (int)UNPACK_RAW_MOVE_DEST(op_word));
        print_rawr(out, (int)UNPACK_RAW_MOVE_SRC(op_word));
    } else if (op == OP_RAW_ADD_INT_BOXED || op == OP_RAW_SUB_INT_BOXED || op == OP_RAW_MUL_INT_BOXED) {
        print_rawi(out, (int)UNPACK_RAW_ARITH_BOXED_SLOT(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_RAW_ARITH_BOXED_REG(op_word));
    } else if (op == OP_RAW_ADD_REAL_BOXED || op == OP_RAW_SUB_REAL_BOXED || op == OP_RAW_MUL_REAL_BOXED) {
        print_rawr(out, (int)UNPACK_RAW_ARITH_BOXED_SLOT(op_word));
        print_field(out, c, FLD_REG, (int)UNPACK_RAW_ARITH_BOXED_REG(op_word));
    } else if (op == OP_RAW_LOAD_INT_POOL) {
        print_rawi(out, (int)UNPACK_RAW_LOAD_INT_POOL_DEST(op_word));
        fprintf(out, "  val=");
        print_pool_value(out, c->pool[UNPACK_RAW_LOAD_INT_POOL_POOL(op_word)]);
    } else {
        int i = 0;
        for (; i < info->packed; i++) {
            unsigned int uword = (unsigned int)op_word;
            int word = (i == 0) ? (int)UNPACK_A(uword) : (i == 1) ? (int)UNPACK_B(uword) : (int)UNPACK_C(uword);
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
    unsigned int offset = 0;
    while (offset < c->count) offset = disassemble_one(c, offset, out);

    if (!c->debug_hits) return;   /* static-only dump if no run happened yet */

    NamedCount by_op[OP_INFO_MAX + 1];
    int by_op_count = 0;
    uint64_t op_totals[OP_INFO_MAX + 1] = {0};
    offset = 0;
    while (offset < c->count) {
        Opcode op = (Opcode)(c->code[offset] & 0x7F);
        if (offset < c->debug_hits_cap) op_totals[op] += c->debug_hits[offset];
        unsigned int next = offset + 1;
        const OpInfo* info = &op_info[op];
        if (info->variable) {
            int field_count = (int)c->code[offset + 2];
            next = offset + 2 + (unsigned int)field_count * 2 + 1;
        } else {
            int n = 0;
            while (n < MAX_FIELDS && info->fields[n] != FLD_END) n++;
            n -= info->packed;
            next = offset + 1 + (unsigned int)n;
        }
        offset = next;
    }
    for (int i = 0; i <= OP_INFO_MAX; i++)
        if (op_totals[i] > 0) by_op[by_op_count++] = (NamedCount){ opcode_name(i), op_totals[i] };
    qsort(by_op, (size_t)by_op_count, sizeof(NamedCount), cmp_named_count_desc);

    fprintf(out, "\n--- per-opcode summary ---\n");
    for (int i = 0; i < by_op_count; i++)
        fprintf(out, "  %-28s %llu\n", by_op[i].name, by_op[i].hits);

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

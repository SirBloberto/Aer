#ifdef AER_DEBUG_TOOLS
#include <stdlib.h>
#include <string.h>
#include "disasm.h"
#include "error.h"
#include "value_box.h"

/* Kinds of operand word this disassembler knows how to decode/print. Distinct from parser.c's
   (unrelated, compile-time-only) OperandKind — this one describes bytecode layout, not fusability. */
typedef enum {
    FLD_END,      /* marks the end of an opcode's operand list */
    FLD_POOL,     /* pool index — resolve and print the constant's own value */
    FLD_NAME,     /* pool index known to be a TYPE_STRING name — print just the string, no quotes */
    FLD_SLOT,     /* local slot index */
    FLD_CACHE,    /* addr_cache slot index */
    FLD_JUMP,     /* absolute code offset this instruction may jump to */
    FLD_COUNT,    /* a raw integer (arg count, item count, arity...) */
    FLD_BINOP,    /* an Opcode value used as an operand (bin_op in a fused op, or CMP_JUMP_FALSE's comparison) */
    FLD_CAST,     /* CAST_INTEGER/CAST_FLOAT/CAST_BOOLEAN */
} Field;

#define MAX_FIELDS 6

typedef struct {
    const char* name;
    const char* desc;
    Field       fields[MAX_FIELDS];
    bool        variable;   /* true only for OP_DEFINE_STRUCT — see disassemble_one */
} OpInfo;

static const OpInfo op_info[OP_HALT + 1] = {
    [OP_PUSH]  = { "OP_PUSH",  "push a constant", {FLD_POOL} },
    [OP_DUP_N] = { "OP_DUP_N", "duplicate the top N stack values in place", {FLD_COUNT} },

    [OP_LOAD]  = { "OP_LOAD",  "push a global's value (cached address if populated)", {FLD_NAME, FLD_CACHE} },
    [OP_STORE] = { "OP_STORE", "pop and store into a global (create if new)", {FLD_NAME, FLD_CACHE} },
    [OP_DEFINE] = { "OP_DEFINE", "pop and define/update in the innermost (global, top-level) scope", {FLD_NAME} },

    [OP_LOAD_LOCAL]   = { "OP_LOAD_LOCAL",   "push this call's local slot", {FLD_SLOT} },
    [OP_STORE_LOCAL]  = { "OP_STORE_LOCAL",  "pop and store into this call's local slot", {FLD_SLOT} },
    [OP_DEFINE_LOCAL] = { "OP_DEFINE_LOCAL", "pop and define a local slot (first assignment to this name)", {FLD_SLOT, FLD_NAME} },

    [OP_COMPOUND_NAME_CONST]  = { "OP_COMPOUND_NAME_CONST",  "fused: global OP= constant", {FLD_NAME, FLD_CACHE, FLD_BINOP, FLD_POOL} },
    [OP_COMPOUND_NAME_NAME]   = { "OP_COMPOUND_NAME_NAME",   "fused: global OP= global", {FLD_NAME, FLD_CACHE, FLD_BINOP, FLD_NAME, FLD_CACHE} },
    [OP_COMPOUND_LOCAL_CONST] = { "OP_COMPOUND_LOCAL_CONST", "fused: local OP= constant", {FLD_SLOT, FLD_BINOP, FLD_POOL} },
    [OP_COMPOUND_LOCAL_LOCAL] = { "OP_COMPOUND_LOCAL_LOCAL", "fused: local OP= local", {FLD_SLOT, FLD_BINOP, FLD_SLOT} },
    [OP_COMPOUND_LOCAL_NAME]  = { "OP_COMPOUND_LOCAL_NAME",  "fused: local OP= global", {FLD_SLOT, FLD_BINOP, FLD_NAME, FLD_CACHE} },
    [OP_COMPOUND_NAME_LOCAL]  = { "OP_COMPOUND_NAME_LOCAL",  "fused: global OP= local", {FLD_NAME, FLD_CACHE, FLD_BINOP, FLD_SLOT} },

    [OP_BINARY_LOCAL_LOCAL] = { "OP_BINARY_LOCAL_LOCAL", "fused: local OP local (expression, not assignment)", {FLD_SLOT, FLD_BINOP, FLD_SLOT} },
    [OP_BINARY_LOCAL_CONST] = { "OP_BINARY_LOCAL_CONST", "fused: local OP constant", {FLD_SLOT, FLD_BINOP, FLD_POOL} },
    [OP_BINARY_LOCAL_NAME]  = { "OP_BINARY_LOCAL_NAME",  "fused: local OP global", {FLD_SLOT, FLD_BINOP, FLD_NAME, FLD_CACHE} },
    [OP_BINARY_NAME_LOCAL]  = { "OP_BINARY_NAME_LOCAL",  "fused: global OP local", {FLD_NAME, FLD_CACHE, FLD_BINOP, FLD_SLOT} },
    [OP_BINARY_NAME_CONST]  = { "OP_BINARY_NAME_CONST",  "fused: global OP constant", {FLD_NAME, FLD_CACHE, FLD_BINOP, FLD_POOL} },
    [OP_BINARY_NAME_NAME]   = { "OP_BINARY_NAME_NAME",   "fused: global OP global", {FLD_NAME, FLD_CACHE, FLD_BINOP, FLD_NAME, FLD_CACHE} },

    [OP_ADD] = { "OP_ADD", "pop b, a; push a + b", {0} },
    [OP_SUB] = { "OP_SUB", "pop b, a; push a - b", {0} },
    [OP_MUL] = { "OP_MUL", "pop b, a; push a * b", {0} },
    [OP_DIV] = { "OP_DIV", "pop b, a; push a / b (always real)", {0} },
    [OP_MOD] = { "OP_MOD", "pop b, a; push a % b", {0} },
    [OP_FLOOR_DIV] = { "OP_FLOOR_DIV", "pop b, a; push floor(a / b)", {0} },

    [OP_EQ]  = { "OP_EQ",  "pop b, a; push a == b", {0} },
    [OP_NEQ] = { "OP_NEQ", "pop b, a; push a != b", {0} },
    [OP_LT]  = { "OP_LT",  "pop b, a; push a < b",  {0} },
    [OP_GT]  = { "OP_GT",  "pop b, a; push a > b",  {0} },
    [OP_LTE] = { "OP_LTE", "pop b, a; push a <= b", {0} },
    [OP_GTE] = { "OP_GTE", "pop b, a; push a >= b", {0} },
    [OP_IN]  = { "OP_IN",  "pop b, a; push a in b (dict key or array element)", {0} },

    [OP_AND]  = { "OP_AND",  "pop b, a; push a && b (as boolean)", {0} },
    [OP_OR]   = { "OP_OR",   "pop b, a; push a || b (as boolean)", {0} },
    [OP_PIPE] = { "OP_PIPE", "never emitted — table-driven dispatch placeholder only", {0} },

    [OP_BITWISE_AND] = { "OP_BITWISE_AND", "pop b, a; push a & b",  {0} },
    [OP_BITWISE_OR]  = { "OP_BITWISE_OR",  "pop b, a; push a | b",  {0} },
    [OP_BITWISE_XOR] = { "OP_BITWISE_XOR", "pop b, a; push a ^ b",  {0} },
    [OP_LSHIFT]      = { "OP_LSHIFT",      "pop b, a; push a << b", {0} },
    [OP_RSHIFT]      = { "OP_RSHIFT",      "pop b, a; push a >> b", {0} },

    [OP_NEGATE]      = { "OP_NEGATE",      "pop a; push -a", {0} },
    [OP_NOT]         = { "OP_NOT",         "pop a; push !a (truthiness)", {0} },
    [OP_BITWISE_NOT] = { "OP_BITWISE_NOT", "pop a; push ~a", {0} },

    [OP_JUMP]          = { "OP_JUMP",          "unconditional jump", {FLD_JUMP} },
    [OP_JUMP_IF_FALSE] = { "OP_JUMP_IF_FALSE", "pop condition; jump if falsy", {FLD_JUMP} },
    [OP_JUMP_IF_TRUE]  = { "OP_JUMP_IF_TRUE",  "pop condition; jump if truthy", {FLD_JUMP} },
    [OP_CMP_JUMP_FALSE] = { "OP_CMP_JUMP_FALSE", "fused: pop b, a; jump if !(a <op> b)", {FLD_BINOP, FLD_JUMP} },

    [OP_PUSH_SCOPE] = { "OP_PUSH_SCOPE", "push a new local scope (once per call)", {0} },
    [OP_POP_SCOPE]  = { "OP_POP_SCOPE",  "pop the current local scope", {0} },

    [OP_CALL]       = { "OP_CALL",       "call by name (global, local-held function, builtin, or struct constructor)", {FLD_NAME, FLD_COUNT, FLD_CACHE} },
    [OP_TAIL_CALL]  = { "OP_TAIL_CALL",  "same as OP_CALL but reuses the current frame (true tail position)", {FLD_NAME, FLD_COUNT, FLD_CACHE} },
    [OP_CALL_VALUE] = { "OP_CALL_VALUE", "pop a function value, then its args; call it", {FLD_COUNT} },
    [OP_RETURN]     = { "OP_RETURN",     "pop return value; unwind scopes; resume at call site", {0} },
    [OP_DEFER_PUSH] = { "OP_DEFER_PUSH", "pop args; stash a deferred call to run at OP_RETURN", {FLD_NAME, FLD_COUNT} },

    [OP_ARRAY_NEW] = { "OP_ARRAY_NEW", "pop N values; push a new array", {FLD_COUNT} },
    [OP_DICT_NEW]  = { "OP_DICT_NEW",  "pop N key/value pairs; push a new dict", {FLD_COUNT} },
    [OP_INDEX_GET] = { "OP_INDEX_GET", "pop index, collection; push element", {0} },

    [OP_INDEX_GET_LOCAL_CONST] = { "OP_INDEX_GET_LOCAL_CONST", "fused: local[constant]", {FLD_SLOT, FLD_POOL} },
    [OP_INDEX_GET_LOCAL_LOCAL] = { "OP_INDEX_GET_LOCAL_LOCAL", "fused: local[local]", {FLD_SLOT, FLD_SLOT} },
    [OP_INDEX_GET_LOCAL_NAME]  = { "OP_INDEX_GET_LOCAL_NAME",  "fused: local[global]", {FLD_SLOT, FLD_NAME, FLD_CACHE} },
    [OP_INDEX_GET_NAME_CONST]  = { "OP_INDEX_GET_NAME_CONST",  "fused: global[constant]", {FLD_NAME, FLD_CACHE, FLD_POOL} },
    [OP_INDEX_GET_NAME_LOCAL]  = { "OP_INDEX_GET_NAME_LOCAL",  "fused: global[local]", {FLD_NAME, FLD_CACHE, FLD_SLOT} },
    [OP_INDEX_GET_NAME_NAME]   = { "OP_INDEX_GET_NAME_NAME",   "fused: global[global]", {FLD_NAME, FLD_CACHE, FLD_NAME, FLD_CACHE} },

    [OP_INDEX_SET] = { "OP_INDEX_SET", "pop value, index, collection; set element", {0} },

    [OP_INDEX_SET_LOCAL_CONST] = { "OP_INDEX_SET_LOCAL_CONST", "fused: local[constant] = constant", {FLD_SLOT, FLD_POOL, FLD_POOL} },
    [OP_INDEX_SET_LOCAL_LOCAL] = { "OP_INDEX_SET_LOCAL_LOCAL", "fused: local[local] = constant", {FLD_SLOT, FLD_SLOT, FLD_POOL} },
    [OP_INDEX_SET_LOCAL_NAME]  = { "OP_INDEX_SET_LOCAL_NAME",  "fused: local[global] = constant", {FLD_SLOT, FLD_NAME, FLD_CACHE, FLD_POOL} },
    [OP_INDEX_SET_NAME_CONST]  = { "OP_INDEX_SET_NAME_CONST",  "fused: global[constant] = constant", {FLD_NAME, FLD_CACHE, FLD_POOL, FLD_POOL} },
    [OP_INDEX_SET_NAME_LOCAL]  = { "OP_INDEX_SET_NAME_LOCAL",  "fused: global[local] = constant", {FLD_NAME, FLD_CACHE, FLD_SLOT, FLD_POOL} },
    [OP_INDEX_SET_NAME_NAME]   = { "OP_INDEX_SET_NAME_NAME",   "fused: global[global] = constant", {FLD_NAME, FLD_CACHE, FLD_NAME, FLD_CACHE, FLD_POOL} },

    [OP_SLICE_GET] = { "OP_SLICE_GET", "pop end, start, collection; push sub-range", {0} },
    [OP_UNPACK]    = { "OP_UNPACK",    "peek an array on the stack; push items[index]", {FLD_COUNT} },
    [OP_ITER_NEXT]      = { "OP_ITER_NEXT",      "for-each step over an array/string/dict-keys", {FLD_JUMP} },
    [OP_ITER_NEXT_PAIR] = { "OP_ITER_NEXT_PAIR", "for-each step over dict key+value pairs", {FLD_JUMP} },
    [OP_ITER_RANGE]     = { "OP_ITER_RANGE",     "for-each step over a numeric a..b[..step] range", {FLD_JUMP} },

    [OP_DEFINE_STRUCT] = { "OP_DEFINE_STRUCT", "register a struct type (variable-length: name, field count, then that many field/default pairs)", {FLD_NAME, FLD_COUNT}, true },
    [OP_FIELD_GET]   = { "OP_FIELD_GET",   "pop a struct; push one field", {FLD_NAME} },
    [OP_FIELD_SET]   = { "OP_FIELD_SET",   "pop value, struct; set one field", {FLD_NAME} },
    [OP_CHECK_SHAPE] = { "OP_CHECK_SHAPE", "`x as Type` — verify (never convert) a struct's exact type", {FLD_NAME} },

    [OP_CALL_MODULE] = { "OP_CALL_MODULE", "call a native or file-module function by (module, function) name", {FLD_NAME, FLD_NAME, FLD_COUNT} },

    [OP_PRINT_REPL] = { "OP_PRINT_REPL", "shell mode: pop and print unless null", {0} },
    [OP_POP]        = { "OP_POP",        "pop and discard", {0} },
    [OP_TO_STR]     = { "OP_TO_STR",     "pop; push its string representation", {0} },
    [OP_CAST]       = { "OP_CAST",       "`x as T` for a primitive T — pop; push converted", {FLD_CAST} },
    [OP_HALT]       = { "OP_HALT",       "stop execution", {0} },
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
   *literal* (those are always built at runtime by OP_ARRAY_NEW etc.) except a function value
   (emit_function_value), which just gets a short tag here. */
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
    return (op >= 0 && op <= OP_HALT && op_info[op].name) ? op_info[op].name : "?";
}

/* Decodes and prints one instruction starting at c->code[offset]; returns the offset of the next
   instruction. OP_DEFINE_STRUCT is the sole variable-length exception (its field count is read
   from the operand stream itself, not known statically). */
static unsigned int disassemble_one(Chunk* c, unsigned int offset, FILE* out) {
    Opcode op = (Opcode)c->code[offset];
    const OpInfo* info = &op_info[op];
    fprintf(out, "%6u  %-28s  %s", offset, opcode_name(op), info->desc);

    unsigned int pos = offset + 1;
    if (info->variable) {
        /* OP_DEFINE_STRUCT: name pool idx, field_count, then field_count * (field-name, default) pairs. */
        int name_idx    = c->code[pos++];
        int field_count = c->code[pos++];
        fprintf(out, "  name=%s fields=%d [", aer_as_string(c->pool[name_idx])->data, field_count);
        for (int i = 0; i < field_count; i++) {
            int fname_idx = c->code[pos++];
            int fdefault_idx = c->code[pos++];
            if (i > 0) fprintf(out, ", ");
            fprintf(out, "%s=", aer_as_string(c->pool[fname_idx])->data);
            print_pool_value(out, c->pool[fdefault_idx]);
        }
        fprintf(out, "]");
    } else {
        for (int i = 0; i < MAX_FIELDS && info->fields[i] != FLD_END; i++) {
            int word = c->code[pos++];
            switch (info->fields[i]) {
                case FLD_POOL: fprintf(out, "  val="); print_pool_value(out, c->pool[word]); break;
                case FLD_NAME: fprintf(out, "  name=%s", aer_as_string(c->pool[word])->data); break;
                case FLD_SLOT: fprintf(out, "  slot=%d", word); break;
                case FLD_CACHE: fprintf(out, "  cache=%d", word); break;
                case FLD_JUMP: fprintf(out, "  -> %d", word); break;
                case FLD_COUNT: fprintf(out, "  n=%d", word); break;
                case FLD_BINOP: fprintf(out, "  op=%s", opcode_name(word)); break;
                case FLD_CAST: fprintf(out, "  %s", cast_name(word)); break;
                case FLD_END: break;
            }
        }
    }

    if (c->debug_hits && offset < c->debug_hits_cap && c->debug_hits[offset] > 0) {
        unsigned int line = chunk_line_for_offset(c, offset);
        fprintf(out, "   [hits=%llu, line=%u]", c->debug_hits[offset], line);
    }
    fprintf(out, "\n");
    return pos;
}

typedef struct { const char* name; unsigned long long hits; } NamedCount;

static int cmp_named_count_desc(const void* a, const void* b) {
    unsigned long long ha = ((const NamedCount*)a)->hits, hb = ((const NamedCount*)b)->hits;
    return (ha < hb) - (ha > hb);
}

typedef struct { unsigned int line; unsigned long long hits; } LineCount;

static int cmp_line_count_desc(const void* a, const void* b) {
    unsigned long long ha = ((const LineCount*)a)->hits, hb = ((const LineCount*)b)->hits;
    return (ha < hb) - (ha > hb);
}

void aer_disassemble(Chunk* c, FILE* out) {
    fprintf(out, "--- disassembly (%u words) ---\n", c->count);
    unsigned int offset = 0;
    while (offset < c->count) offset = disassemble_one(c, offset, out);

    if (!c->debug_hits) return;   /* static-only dump if no run happened yet */

    NamedCount by_op[OP_HALT + 1];
    int by_op_count = 0;
    unsigned long long op_totals[OP_HALT + 1] = {0};
    offset = 0;
    while (offset < c->count) {
        Opcode op = (Opcode)c->code[offset];
        if (offset < c->debug_hits_cap) op_totals[op] += c->debug_hits[offset];
        unsigned int next = offset + 1;
        const OpInfo* info = &op_info[op];
        if (info->variable) {
            int field_count = c->code[offset + 2];
            next = offset + 2 + (unsigned int)field_count * 2 + 1;
        } else {
            int n = 0;
            while (n < MAX_FIELDS && info->fields[n] != FLD_END) n++;
            next = offset + 1 + (unsigned int)n;
        }
        offset = next;
    }
    for (int i = 0; i <= OP_HALT; i++)
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

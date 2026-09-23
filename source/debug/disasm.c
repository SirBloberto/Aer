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
    F_VARIABLE, /* first and only entry: the operand count lives in word0 */
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
} OpInfo;

#define OP_INFO_MAX (OP_OPCODE_COUNT_MARKER - 1)

/* Operand layouts for opcodes.def: one shape per opcode family, so the kind, width and checked axes
   read as the matrix they are. */
#define L_BINOP {AT_A, F_REG}, {AT_B, F_RK8}, {AT_C, F_RK8}
#define L_CMP_JUMP {AT_B, F_RK8}, {AT_C, F_RK8}, {AT_W1, F_JUMP}
#define L_RAW_CMP_JUMP(K, KK) {AT_B, K}, {AT_C, KK}, {AT_W1, F_JUMP}
#define L_RAW_ARITH(K, KK) {AT_A, K}, {AT_B, K}, {AT_C, KK}
#define L_RAW_CMP(K, KK) {AT_A, F_REG}, {AT_B, K}, {AT_C, KK}
#define L_CALL {AT_A, F_REG}, {AT_B, F_REG}, {AT_C, F_COUNT}, {AT_W1, F_JUMP}, {AT_W2, F_COUNT}
#define L_CALL_VALUE {AT_A, F_REG}, {AT_B, F_REG}, {AT_C, F_COUNT}, {AT_W1, F_REG}
#define L_ITER {AT_A, F_REG}, {AT_B, F_REG}, {AT_C, F_REG}, {AT_W1, F_REG}, {AT_W2, F_JUMP}
#define L_FIELD_GET_RAW(K) {AT_A, K}, {AT_B, F_REG}, {AT_W1, F_OFF}
#define L_FIELD_SET_RAW(K) {AT_A, F_REG}, {AT_W1, F_OFF}, {AT_W2, K}
#define L_FIELD_COMPOUND_RAW(K) {AT_A, F_REG}, {AT_B, F_BINOP}, {AT_W1, F_OFF}, {AT_W2, K}
#define L_IDX_FIELD_GET_RAW(K) {AT_A, K}, {AT_B, F_REG}, {AT_W1_HI, F_OFF}, {AT_W1_LO, F_RK16}
#define L_IDX_FIELD_SET_RAW(K) {AT_A, F_REG}, {AT_W16, F_RK16}, {AT_W1_HI, F_OFF}, {AT_W1_LO, K}
#define L_IDX_FIELD_COMPOUND_RAW(K)                                                                              {AT_A, F_REG}, {AT_B, F_BINOP}, {AT_W1_HI, F_OFF}, {AT_W1_LO, F_RK16}, {AT_W2, K}
#define L_VARIABLE {0, F_VARIABLE}

#define OPCODE(name, handler, scaled, desc, ...) [OP_##name] = {"OP_" #name, desc, {__VA_ARGS__}},
static const OpInfo op_info[OP_INFO_MAX + 1] = {
#include "opcodes.def"
};
#undef OPCODE

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
        case AT_A: return OPERAND_A(w);
        case AT_B: return OPERAND_B(w);
        case AT_C: return OPERAND_C(w);
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
    if (info->ops[0].fld == F_VARIABLE) {
        if (op == OP_INTERP)
            return 1 + OPERAND_B(w0);
        if (op == OP_INDEX_GET_INTERP)
            return 1 + OPERAND_C(w0);
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
        unsigned int count = (op == OP_INTERP) ? OPERAND_B(w0) : OPERAND_C(w0);
        if (op == OP_INTERP)
            fprintf(out, "  reg=%u  parts=%u  [", OPERAND_A(w0), count);
        else
            fprintf(out, "  reg=%u  obj=r%u  parts=%u  [", OPERAND_A(w0), OPERAND_B(w0), count);
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

    if (info->ops[0].fld == F_VARIABLE)
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

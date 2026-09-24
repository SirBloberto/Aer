#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chunk.h"
#include "error.h"
#include "hashtable.h"
#include "strbuf.h"

const char* const aer_value_type_names[TYPE_STRUCT] = {"null",   "boolean",  "integer", "float",
                                                       "string", "function", "array",   "hashtable"};
const char* const aer_typed_elem_names[TYPED_ELEM_BOOL + 1] = {"int32", "float32", "integer", "float",
                                                               "boolean"};

/* Returns the rendered length, saving every hot caller a strlen() -- worth 2.3% of
   lookup_table_bench. Prints the shortest spelling that reads back as the same double,
   as Python, Rust and JavaScript do: plain `%g` shows six significant digits, so a summed column
   came out as 3.58329e+09 and two engines computing different totals looked identical. 15 digits
   covers any double written as a decimal, and is tried first so the common value costs one pass. */
unsigned int aer_format_real(double d, char* buf, size_t bufsize) {
    int written = snprintf(buf, bufsize, "%.15g", d);
    for (int digits = 16; digits <= 17 && written > 0 && (size_t)written < bufsize; digits++) {
        if (strtod(buf, NULL) == d)
            break;
        written = snprintf(buf, bufsize, "%.*g", digits, d);
    }
    size_t len = (written < 0) ? strlen(buf) : (size_t)written;
    /* Skip nan/inf spellings -- they should never get a trailing ".0". */
    if (!strpbrk(buf, ".eEnNiI")) {
        if (len + 3 <= bufsize) {
            buf[len] = '.';
            buf[len + 1] = '0';
            buf[len + 2] = '\0';
            len += 2;
        }
    }
    return (unsigned int)len;
}

/* snprintf("%lld", ...) replacement: integer-to-string is hot for any script doing interpolation or
   print() with integers, and profiling a dict-heavy benchmark put printf internals at ~14% of
   cycles from this one conversion. bufsize is unchecked -- every caller passes >=64 bytes and
   int64's longest rendering is 20 bytes plus NUL. */
unsigned int aer_format_int(long long v, char* buf, size_t bufsize) {
    (void)bufsize;
    char tmp[20];
    int pos = 0;
    unsigned long long uv;
    size_t len = 0;
    if (v < 0) {
        buf[len++] = '-';
        uv = (unsigned long long)(-(v + 1)) + 1ULL; /* avoids signed overflow negating LLONG_MIN */
    } else {
        uv = (unsigned long long)v;
    }
    if (uv == 0) {
        buf[len++] = '0';
    } else if (uv <= 0xFFFFFFFFULL) {
        /* Narrowed to 32 bits whenever it fits, which is nearly always. A 64-bit /10 and %10 are a
           libgcc call each on 32-bit ARM -- once per digit -- while the 32-bit forms compile to a
           reciprocal multiply and no division at all. Same reasoning as aer_mod_int64 (vm.c). */
        unsigned int u32 = (unsigned int)uv;
        while (u32 > 0) {
            tmp[pos++] = (char)('0' + (u32 % 10u));
            u32 /= 10u;
        }
        while (pos > 0)
            buf[len++] = tmp[--pos];
    } else {
        while (uv > 0) {
            tmp[pos++] = (char)('0' + (uv % 10));
            uv /= 10;
        }
        while (pos > 0)
            buf[len++] = tmp[--pos];
    }
    buf[len] = '\0';
    return (unsigned int)len;
}

bool aer_format_scalar(AerVal v, char* scratch, size_t size, const char** text, unsigned int* len) {
    switch (aer_type(v)) {
        case TYPE_NULL:
            *text = "null";
            *len = 4;
            return true;
        case TYPE_INTEGER:
            *len = aer_format_int((long long)aer_as_int(v), scratch, size);
            *text = scratch;
            return true;
        case TYPE_REAL:
            *len = aer_format_real(aer_as_real(v), scratch, size);
            *text = scratch;
            return true;
        case TYPE_BOOLEAN:
            *text = aer_as_bool(v) ? "true" : "false";
            *len = aer_as_bool(v) ? 4u : 5u;
            return true;
        case TYPE_FUNCTION:
            *text = "<function>";
            *len = 10;
            return true;
        default: return false;
    }
}

/* Value formatting -- shared by print() and vm_to_str() (interpolation, +, etc.) for one consistent
   recursive rendering, not a terse "<array[3]>" fallback. */

#define FORMAT_MAX_DEPTH 64

static void format_value(Chunk* c, AerVal v, bool in_collection, StrBuf* sb, unsigned int depth) {
    char tmp[64];
    if (depth >= FORMAT_MAX_DEPTH) {
        strbuf_append(sb, "...");
        return;
    }
    const char* text;
    unsigned int len;
    if (aer_format_scalar(v, tmp, sizeof(tmp), &text, &len)) {
        strbuf_append_n(sb, text, len);
        return;
    }
    switch (aer_type(v)) {
        case TYPE_STRING: {
            AerString* s = aer_as_string(v);
            if (in_collection)
                strbuf_append(sb, "\"");
            strbuf_append_n(sb, s->data, s->length);
            if (in_collection)
                strbuf_append(sb, "\"");
            break;
        }
        case TYPE_ARRAY: {
            AerArray* a = aer_as_array(v);
            strbuf_append(sb, "[");
            for (unsigned int i = 0; i < a->count; i++) {
                if (i > 0)
                    strbuf_append(sb, ", ");
                format_value(c, a->items[i], true, sb, depth + 1);
            }
            strbuf_append(sb, "]");
            break;
        }
        case TYPE_STRUCT: {
            AerStruct* s = aer_as_struct(v);
            Shape* shape = s->shape;
            strbuf_append(sb, aer_as_string(c->pool[shape->name])->data);
            strbuf_append(sb, "{");
            for (unsigned int i = 0; i < shape->field_count; i++) {
                if (i > 0)
                    strbuf_append(sb, ", ");
                strbuf_append(sb, aer_as_string(c->pool[shape->field_names[i]])->data);
                strbuf_append(sb, ": ");
                format_value(c, vm_struct_field_read(s, i), true, sb, depth + 1);
            }
            strbuf_append(sb, "}");
            break;
        }
        case TYPE_PACKED_ARRAY: {
            AerPackedArray* pa = aer_as_packed_array(v);
            strbuf_append(sb, aer_as_string(c->pool[pa->shape->name])->data);
            strbuf_append(sb, "[");
            snprintf(tmp, sizeof(tmp), "%u", pa->count);
            strbuf_append(sb, tmp);
            strbuf_append(sb, "]");
            break;
        }
        case TYPE_TYPED_ARRAY: {
            AerTypedArray* ta = aer_as_typed_array(v);
            strbuf_append(sb, aer_typed_elem_names[ta->elem_kind]);
            strbuf_append(sb, "[");
            snprintf(tmp, sizeof(tmp), "%u", ta->count);
            strbuf_append(sb, tmp);
            strbuf_append(sb, "]");
            break;
        }
        case TYPE_DICT: {
            AerDict* d = aer_as_dict(v);
            strbuf_append(sb, "{");
            bool first = true;
            for (unsigned int i = 0; i < d->map.count; i++) {
                HashTableEntry* e = &d->map.dense[i];
                if (!first)
                    strbuf_append(sb, ", ");
                first = false;
                strbuf_append(sb, "\"");
                strbuf_append(sb, e->key);
                strbuf_append(sb, "\": ");
                format_value(c, e->payload, true, sb, depth + 1);
            }
            strbuf_append(sb, "}");
            break;
        }
        case TYPE_RESULT: {
            AerResult* r = aer_as_result(v);
            strbuf_append(sb, "Result(");
            format_value(c, r->value, true, sb, depth + 1);
            strbuf_append(sb, ", ");
            format_value(c, r->err, true, sb, depth + 1);
            strbuf_append(sb, ")");
            break;
        }
        case TYPE_NULL:
        case TYPE_INTEGER:
        case TYPE_REAL:
        case TYPE_BOOLEAN:
        case TYPE_FUNCTION:
        case TYPE_ANY: break; /* scalars returned above; TYPE_ANY is never a real AerVal's tag */
    }
}

void vm_format_value(Chunk* c, AerVal v, bool in_collection, StrBuf* sb) {
    format_value(c, v, in_collection, sb, 0);
}

void vm_print_value(Chunk* c, AerVal v, bool in_collection) {
    StrBuf sb;
    strbuf_init(&sb);
    vm_format_value(c, v, in_collection, &sb);
    printf("%s", sb.buf);
    free(sb.buf);
}

/* Structural/reference equality with no error path -- unlike OP_EQ, a type mismatch here just means "not
   this one, keep looking." Used by OP_IN's array scan and collection.index_of (aer_collection.c). */
bool values_equal(AerVal a, AerVal b) {
    if (aer_type(a) != aer_type(b))
        return false;
    switch (aer_type(a)) {
        case TYPE_NULL: return true;
        case TYPE_BOOLEAN: return aer_as_bool(a) == aer_as_bool(b);
        case TYPE_INTEGER: return aer_as_int(a) == aer_as_int(b);
        case TYPE_REAL: return aer_as_real(a) == aer_as_real(b);
        case TYPE_STRING:
            return aer_as_string(a)->length == aer_as_string(b)->length &&
                   strncmp(aer_as_string(a)->data, aer_as_string(b)->data, aer_as_string(a)->length) == 0;
        case TYPE_FUNCTION: return aer_as_function(a)->code_offset == aer_as_function(b)->code_offset;
        case TYPE_ARRAY: return aer_as_array(a) == aer_as_array(b);
        case TYPE_DICT: return aer_as_dict(a) == aer_as_dict(b);
        case TYPE_STRUCT: return aer_as_struct(a) == aer_as_struct(b);
        case TYPE_PACKED_ARRAY: return aer_as_packed_array(a) == aer_as_packed_array(b);
        case TYPE_TYPED_ARRAY: return aer_as_typed_array(a) == aer_as_typed_array(b);
        case TYPE_RESULT: return aer_as_result(a) == aer_as_result(b);
        case TYPE_ANY: break; /* never a real AerVal's tag -- only Shape.field_types[] uses it */
    }
    return false;
}


/* A struct reports its declared name, a packed array that name plus "[]". */
const char* vm_type_name(Chunk* c, AerVal v) {
    if (aer_type(v) == TYPE_STRUCT)
        return aer_as_string(c->pool[aer_as_struct(v)->shape->name])->data;
    if (aer_type(v) == TYPE_PACKED_ARRAY) {
        /* static buf is safe only because every caller consumes the result immediately. */
        AerPackedArray* pa = aer_as_packed_array(v);
        static char buf[128];
        snprintf(buf, sizeof(buf), "%s[]", aer_as_string(c->pool[pa->shape->name])->data);
        return buf;
    }
    if (aer_type(v) == TYPE_TYPED_ARRAY) {
        static char buf[32];
        snprintf(buf, sizeof(buf), "%s[]", aer_typed_elem_names[aer_as_typed_array(v)->elem_kind]);
        return buf;
    }
    if (aer_type(v) == TYPE_RESULT)
        return "Result";
    return aer_value_type_names[aer_type(v)];
}

/* Only used to name the operator in a type-mismatch message -- never on a path that already has
   its own more specific error (e.g. 'in' has vm_in()'s own messages above). */
const char* binop_symbol(Opcode op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_DIV: return "/";
        case OP_FLOOR_DIV: return "//";
        case OP_MOD: return "%";
        case OP_EQ: return "==";
        case OP_NEQ: return "!=";
        case OP_LT: return "<";
        case OP_GT: return ">";
        case OP_LTE: return "<=";
        case OP_GTE: return ">=";
        case OP_IN: return "in";
        default: return "that operator";
    }
}

AerVal vm_to_str(Chunk* c, AerVal v) {
    if (aer_type(v) == TYPE_STRING)
        return v;

    char buf[64];
    const char* text;
    unsigned int len;
    /* Not interned: a runtime string is used once, and interning would grow the pool forever -- 7x
       slower for 100k unique casts than for 10 distinct ones. */
    if (aer_format_scalar(v, buf, sizeof(buf), &text, &len))
        return aer_make_string_copy(text, len);
    /* Recursive content has no bounded size, so this reuses print()'s formatter and hands over its
       buffer as-is. */
    StrBuf sb;
    strbuf_init(&sb);
    vm_format_value(c, v, false, &sb);
    return aer_make_string(sb.buf, (unsigned int)sb.len);
}
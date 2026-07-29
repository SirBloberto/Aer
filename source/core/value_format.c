#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hashtable.h"
#include "strbuf.h"
#include "vm.h"

void aer_format_real(double d, char* buf, size_t bufsize) {
    snprintf(buf, bufsize, "%g", d);
    /* Skip nan/inf spellings -- they should never get a trailing ".0". */
    if (!strpbrk(buf, ".eEnNiI")) {
        size_t len = strlen(buf);
        if (len + 3 <= bufsize) { buf[len] = '.'; buf[len + 1] = '0'; buf[len + 2] = '\0'; }
    }
}

/* ------------------------------------------------------------------ */
/* Value formatting -- shared by print() and vm_to_str() (interpolation, +, etc.) for one consistent recursive rendering, not a terse "<array[3]>" fallback. */
/* ------------------------------------------------------------------ */

void vm_format_value(Chunk* c, AerVal v, bool in_collection, StrBuf* sb) {
    char tmp[64];
    switch (aer_type(v)) {
        case TYPE_NULL:     strbuf_append(sb, "null"); break;
        case TYPE_INTEGER:  snprintf(tmp, sizeof(tmp), "%lld", (long long)aer_as_int(v));  strbuf_append(sb, tmp); break;
        case TYPE_REAL:     aer_format_real(aer_as_real(v), tmp, sizeof(tmp)); strbuf_append(sb, tmp); break;
        case TYPE_BOOLEAN:  strbuf_append(sb, aer_as_bool(v) ? "true" : "false"); break;
        case TYPE_FUNCTION: strbuf_append(sb, "<function>"); break;
        case TYPE_STRING: {
            AerString* s = aer_as_string(v);
            if (in_collection) strbuf_append(sb, "\"");
            strbuf_append_n(sb, s->data, s->length);
            if (in_collection) strbuf_append(sb, "\"");
            break;
        }
        case TYPE_ARRAY: {
            AerArray* a = aer_as_array(v);
            strbuf_append(sb, "[");
            for (unsigned int i = 0; i < a->count; i++) {
                if (i > 0) strbuf_append(sb, ", ");
                vm_format_value(c, a->items[i], true, sb);
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
                if (i > 0) strbuf_append(sb, ", ");
                strbuf_append(sb, aer_as_string(c->pool[shape->field_names[i]])->data);
                strbuf_append(sb, ": ");
                vm_format_value(c, vm_struct_field_read(s, i), true, sb);
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
            /* Terse summary, matching TYPE_PACKED_ARRAY's own -- vm_type_name (vm.c) already
               derives "int32[]"/"float32[]"/"integer[]"/"float[]" from elem_kind. */
            static const char* elem_names[] = { "int32", "float32", "integer", "float" };
            AerTypedArray* ta = aer_as_typed_array(v);
            strbuf_append(sb, elem_names[ta->elem_kind]);
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
                if (!first) strbuf_append(sb, ", ");
                first = false;
                strbuf_append(sb, "\"");
                strbuf_append(sb, e->key);
                strbuf_append(sb, "\": ");
                vm_format_value(c, e->payload, true, sb);
            }
            strbuf_append(sb, "}");
            break;
        }
        case TYPE_RESULT: {
            AerResult* r = aer_as_result(v);
            strbuf_append(sb, "Result(");
            vm_format_value(c, r->value, true, sb);
            strbuf_append(sb, ", ");
            vm_format_value(c, r->err, true, sb);
            strbuf_append(sb, ")");
            break;
        }
        case TYPE_ANY: break;   /* never a real AerVal's tag -- only Shape.field_types[] uses it */
    }
}

void vm_print_value(Chunk* c, AerVal v, bool in_collection) {
    StrBuf sb;
    strbuf_init(&sb);
    vm_format_value(c, v, in_collection, &sb);
    printf("%s", sb.buf);
    free(sb.buf);
}

/* Structural/reference equality with no error path -- unlike OP_EQ, a type mismatch here just means "not this one, keep looking." Used by OP_IN's array scan and collection.index_of (aer_collection.c). */
bool values_equal(AerVal a, AerVal b) {
    if (aer_type(a) != aer_type(b)) return false;
    switch (aer_type(a)) {
        case TYPE_NULL:     return true;
        case TYPE_BOOLEAN:  return aer_as_bool(a) == aer_as_bool(b);
        case TYPE_INTEGER:  return aer_as_int(a) == aer_as_int(b);
        case TYPE_REAL:     return aer_as_real(a) == aer_as_real(b);
        case TYPE_STRING:   return aer_as_string(a)->length == aer_as_string(b)->length &&
                                    strncmp(aer_as_string(a)->data, aer_as_string(b)->data, aer_as_string(a)->length) == 0;
        case TYPE_FUNCTION: return aer_as_function(a)->code_offset == aer_as_function(b)->code_offset;
        case TYPE_ARRAY:    return aer_as_array(a) == aer_as_array(b);
        case TYPE_DICT:     return aer_as_dict(a) == aer_as_dict(b);
        case TYPE_STRUCT:   return aer_as_struct(a) == aer_as_struct(b);
        case TYPE_PACKED_ARRAY: return aer_as_packed_array(a) == aer_as_packed_array(b);
        case TYPE_TYPED_ARRAY: return aer_as_typed_array(a) == aer_as_typed_array(b);
        case TYPE_RESULT:   return aer_as_result(a) == aer_as_result(b);
        case TYPE_ANY:      break;   /* never a real AerVal's tag -- only Shape.field_types[] uses it */
    }
    return false;
}

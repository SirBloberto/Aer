#include <math.h>
#include <string.h>
#include "error.h"
#include "hashtable.h"
#include "heap.h"
#include "typed_array.h"
#include "value_format.h"
#include "value_ops.h"

/* The one `in` implementation -- called by h_in and by vm_binary_cold (reached when a fused
   opcode carries OP_IN as its runtime bin_op). */
AerVal vm_in(AerVal a, AerVal b) {
    if (aer_type(b) == TYPE_DICT) {
        if (aer_type(a) != TYPE_STRING) {
            error("Left side of 'in' must be a string when testing dict membership");
            return aer_bool(false);
        }
        AerString* as = aer_as_string(a);
        unsigned int klen = hashtable_key_true_len(as->data, as->length);
        return aer_bool(hashtable_get_hashed(&aer_as_dict(b)->map, as->data, klen,
                                             hashtable_string_hash(as, klen)) != NULL);
    }
    if (aer_type(b) == TYPE_ARRAY) {
        AerArray* arr = aer_as_array(b);
        for (unsigned int i = 0; i < arr->count; i++) {
            if (values_equal(a, arr->items[i]))
                return aer_bool(true);
        }
        return aer_bool(false);
    }
    if (aer_type(b) == TYPE_STRING) {
        if (aer_type(a) != TYPE_STRING) {
            error("Left side of 'in' must be a string when testing string membership");
            return aer_bool(false);
        }
        return aer_bool(aer_string_find(aer_as_string(b), aer_as_string(a)) >= 0);
    }
    error("Right side of 'in' must be a dict, array, or string");
    return aer_bool(false);
}

static AerVal vm_binary_real(double l, double rv, Opcode op) {
    switch (op) {
        case OP_ADD: return aer_real(l + rv);
        case OP_SUB: return aer_real(l - rv);
        case OP_MUL: return aer_real(l * rv);
        case OP_DIV:
            if (rv == 0.0) {
                error("Division by zero");
                return aer_real(0.0);
            }
            return aer_real(l / rv);
        case OP_FLOOR_DIV:
            if (rv == 0.0) {
                error("Division by zero");
                return aer_real(0.0);
            }
            return aer_real(floor(l / rv));
        case OP_MOD:
            if (rv == 0.0) {
                error("Modulo by zero");
                return aer_real(0.0);
            }
            return aer_real(aer_mod_double(l, rv));
        case OP_EQ: return aer_bool(l == rv);
        case OP_NEQ: return aer_bool(l != rv);
        case OP_LT: return aer_bool(l < rv);
        case OP_GT: return aer_bool(l > rv);
        case OP_LTE: return aer_bool(l <= rv);
        case OP_GTE: return aer_bool(l >= rv);
        default: error("Operator not valid for reals"); return aer_real(0.0);
    }
}

static AerVal vm_binary_string(AerString* as, AerString* bs, Opcode op) {
    bool eq = as->length == bs->length && strncmp(as->data, bs->data, as->length) == 0;
    if (op == OP_EQ)
        return aer_bool(eq);
    if (op == OP_NEQ)
        return aer_bool(!eq);
    if (op == OP_ADD) {
        unsigned int len = as->length + bs->length;
        if (len <= AER_STRING_INLINE_MAX) {
            /* Assembled on the stack, not the heap -- the concat result is short enough to land
               entirely inline in the new AerString cell, so there's nothing to allocate at all. */
            char stackbuf[AER_STRING_INLINE_MAX + 1];
            memcpy(stackbuf, as->data, as->length);
            memcpy(stackbuf + as->length, bs->data, bs->length);
            return aer_make_string_copy(stackbuf, len);
        }
        char* buf = vm_string_payload_alloc(vm_require_current_heap(), len);
        memcpy(buf, as->data, as->length);
        memcpy(buf + as->length, bs->data, bs->length);
        buf[len] = '\0';
        /* aer_make_string takes ownership of buf. Used once here, so no interning. */
        return aer_make_string(buf, len);
    }
    if (op == OP_LT || op == OP_GT || op == OP_LTE || op == OP_GTE) {
        /* Same total order collection.sort() uses for strings -- one shared helper (value.h). */
        int cmp = aer_string_compare(as, bs);
        switch (op) {
            case OP_LT: return aer_bool(cmp < 0);
            case OP_GT: return aer_bool(cmp > 0);
            case OP_LTE: return aer_bool(cmp <= 0);
            default: return aer_bool(cmp >= 0); /* OP_GTE */
        }
    }
    error("Operator not valid for strings");
    return aer_bool(false);
}

AerVal vm_binary_cold(Chunk* c, AerVal a, AerVal b, Opcode op, ValueType ta, ValueType tb) {
    /* Before the null handling below, so `null in arr` is a container search rather than a
       comparison error. */
    if (op == OP_IN)
        return vm_in(a, b);

    /* null equality: null == null is true; null op anything-else errors */
    if (ta == TYPE_NULL || tb == TYPE_NULL) {
        if (op == OP_EQ)
            return aer_bool(ta == TYPE_NULL && tb == TYPE_NULL);
        if (op == OP_NEQ)
            return aer_bool(!(ta == TYPE_NULL && tb == TYPE_NULL));
        error("Operator not valid for null");
        return aer_bool(false);
    }

    if (ta == TYPE_REAL || tb == TYPE_REAL) {
        a = vm_promote_real(a);
        b = vm_promote_real(b);
    }

    if (aer_type(a) == TYPE_REAL && aer_type(b) == TYPE_REAL)
        return vm_binary_real(aer_as_real(a), aer_as_real(b), op);

    if (aer_type(a) == TYPE_BOOLEAN && aer_type(b) == TYPE_BOOLEAN) {
        if (op == OP_EQ)
            return aer_bool(aer_as_bool(a) == aer_as_bool(b));
        if (op == OP_NEQ)
            return aer_bool(aer_as_bool(a) != aer_as_bool(b));
        error("Operator not valid for booleans");
        return aer_bool(false);
    }

    if (aer_type(a) == TYPE_STRING && aer_type(b) == TYPE_STRING)
        return vm_binary_string(aer_as_string(a), aer_as_string(b), op);

    /* Compared by identity only. Tested on the values' own tags: a struct-field caller passes the
       field's declared type as ta, which is TYPE_ANY for an untyped field. */
    if (aer_type(a) == aer_type(b) &&
        (aer_type(a) == TYPE_ARRAY || aer_type(a) == TYPE_DICT || aer_type(a) == TYPE_RESULT)) {
        if (op == OP_EQ || op == OP_NEQ)
            return aer_bool((a.as.ptr == b.as.ptr) == (op == OP_EQ));
        error("Operator not valid for %s",
              aer_type(a) == TYPE_ARRAY ? "arrays" : aer_type(a) == TYPE_DICT ? "dicts" : "Results");
        return aer_bool(false);
    }

    if (aer_type(a) == TYPE_TYPED_ARRAY || aer_type(b) == TYPE_TYPED_ARRAY)
        return vm_binary_column(c, a, b, op, ta, tb);

    error("Cannot apply '%s' to %s and %s", binop_symbol(op), vm_type_name(c, a), vm_type_name(c, b));
    return aer_bool(false);
}

/* Either bound may be null, meaning 0 or len. Out-of-range clamps rather than errors, Python-style. */
bool vm_slice_bounds(AerVal start_v, AerVal end_v, int64_t len, int64_t* out_start, int64_t* out_end) {
    if (aer_type(start_v) != TYPE_NULL && aer_type(start_v) != TYPE_INTEGER) {
        error("Slice bounds must be integers");
        return false;
    }
    if (aer_type(end_v) != TYPE_NULL && aer_type(end_v) != TYPE_INTEGER) {
        error("Slice bounds must be integers");
        return false;
    }
    int64_t start = (aer_type(start_v) == TYPE_NULL) ? 0 : aer_as_int(start_v);
    int64_t end = (aer_type(end_v) == TYPE_NULL) ? len : aer_as_int(end_v);
    if (start < 0)
        start += len;
    if (end < 0)
        end += len;
    if (start < 0)
        start = 0;
    if (end > len)
        end = len;
    if (end < start)
        end = start;
    *out_start = start;
    *out_end = end;
    return true;
}

/* integer(x)/float(x)/boolean(x)/string(x) conversion rules, shared by OP_CAST's handler below. */
AerVal vm_cast(AerVal v, int cast_type) {
    AerVal r = aer_null();
    switch (cast_type) {
        case CAST_INTEGER:
            switch (aer_type(v)) {
                case TYPE_INTEGER: r = v; break;
                case TYPE_REAL: r = aer_int((int64_t)aer_as_real(v)); break;
                case TYPE_BOOLEAN: r = aer_int(aer_as_bool(v) ? 1 : 0); break;
                case TYPE_STRING:
                    error("integer() converts between number types, which cannot fail; parse a "
                          "string with string.to_integer(), which returns (value, err)");
                    r = aer_int(0);
                    break;
                default:
                    error("integer() accepts a number or a boolean");
                    r = aer_int(0);
                    break;
            }
            break;
        case CAST_FLOAT:
            switch (aer_type(v)) {
                case TYPE_REAL: r = v; break;
                case TYPE_INTEGER: r = aer_real((double)aer_as_int(v)); break;
                case TYPE_BOOLEAN: r = aer_real(aer_as_bool(v) ? 1.0 : 0.0); break;
                case TYPE_STRING:
                    error("float() converts between number types, which cannot fail; parse a "
                          "string with string.to_float(), which returns (value, err)");
                    r = aer_real(0.0);
                    break;
                default:
                    error("float() accepts a number or a boolean");
                    r = aer_real(0.0);
                    break;
            }
            break;
        case CAST_BOOLEAN: r = aer_bool(vm_truthy(v)); break;
    }
    return r;
}

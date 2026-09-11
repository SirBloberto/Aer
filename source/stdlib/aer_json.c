#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer_stdlib.h"
#include "error.h"
#include "strbuf.h"

#define JSON_MAX_DEPTH 128

/* encode                                                              */

static void json_encode_string(StrBuf* b, const char* s, unsigned int len) {
    strbuf_append_char(b, '"');
    for (unsigned int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
            case '"': strbuf_append(b, "\\\""); break;
            case '\\': strbuf_append(b, "\\\\"); break;
            case '\n': strbuf_append(b, "\\n"); break;
            case '\r': strbuf_append(b, "\\r"); break;
            case '\t': strbuf_append(b, "\\t"); break;
            case '\b': strbuf_append(b, "\\b"); break;
            case '\f': strbuf_append(b, "\\f"); break;
            default:
                if (ch < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", ch);
                    strbuf_append(b, esc);
                } else {
                    strbuf_append_char(b, (char)ch);
                }
        }
    }
    strbuf_append_char(b, '"');
}

/* Returns false (error() already called) for a function or packed-array value -- everything else succeeds.
   A struct instance encodes as a JSON object keyed by field names, so they survive a round trip via
   json.decode(). */
static bool json_encode_value(Chunk* c, AerVal v, StrBuf* b, unsigned int depth) {
    char tmp[64];
    if (depth >= JSON_MAX_DEPTH) {
        error("json.encode() exceeded the maximum nesting depth of %d -- the value may be cyclic",
              JSON_MAX_DEPTH);
        return false;
    }
    switch (aer_type(v)) {
        case TYPE_NULL: strbuf_append(b, "null"); break;
        case TYPE_BOOLEAN: strbuf_append(b, aer_as_bool(v) ? "true" : "false"); break;
        case TYPE_INTEGER:
            aer_format_int((long long)aer_as_int(v), tmp, sizeof(tmp));
            strbuf_append(b, tmp);
            break;
        case TYPE_REAL:
            aer_format_real(aer_as_real(v), tmp, sizeof(tmp));
            strbuf_append(b, tmp);
            break;
        case TYPE_STRING: {
            AerString* s = aer_as_string(v);
            json_encode_string(b, s->data, s->length);
            break;
        }
        case TYPE_FUNCTION: error("json.encode() cannot serialize a function value"); return false;
        case TYPE_ARRAY: {
            AerArray* a = aer_as_array(v);
            strbuf_append_char(b, '[');
            for (unsigned int i = 0; i < a->count; i++) {
                if (i > 0)
                    strbuf_append_char(b, ',');
                if (!json_encode_value(c, a->items[i], b, depth + 1))
                    return false;
            }
            strbuf_append_char(b, ']');
            break;
        }
        case TYPE_STRUCT: {
            AerStruct* s = aer_as_struct(v);
            Shape* shape = s->shape;
            strbuf_append_char(b, '{');
            for (unsigned int i = 0; i < shape->field_count; i++) {
                if (i > 0)
                    strbuf_append_char(b, ',');
                AerString* fname = aer_as_string(c->pool[shape->field_names[i]]);
                json_encode_string(b, fname->data, fname->length);
                strbuf_append_char(b, ':');
                if (!json_encode_value(c, vm_struct_field_read(s, i), b, depth + 1))
                    return false;
            }
            strbuf_append_char(b, '}');
            break;
        }
        case TYPE_DICT: {
            AerDict* d = aer_as_dict(v);
            strbuf_append_char(b, '{');
            bool first = true;
            for (unsigned int i = 0; i < d->map.count; i++) {
                HashTableEntry* e = &d->map.dense[i];
                if (!first)
                    strbuf_append_char(b, ',');
                first = false;
                json_encode_string(b, e->key, e->length);
                strbuf_append_char(b, ':');
                if (!json_encode_value(c, e->payload, b, depth + 1))
                    return false;
            }
            strbuf_append_char(b, '}');
            break;
        }
        case TYPE_PACKED_ARRAY: error("json.encode() cannot serialize a packed array value"); return false;
        case TYPE_TYPED_ARRAY: error("json.encode() cannot serialize a typed array value"); return false;
        case TYPE_RESULT: error("json.encode() cannot serialize a Result value"); return false;
        case TYPE_ANY: break; /* never a real AerVal's tag -- only Shape.field_types[] uses it */
    }
    return true;
}

/* decode -- small recursive-descent parser                            */

typedef struct {
    const char* s;
    unsigned int len;
    unsigned int pos;
    char* err; /* xmalloc'd message once set; NULL means "still ok" */
    unsigned int depth;
} JsonParser;

static void json_skip_ws(JsonParser* p) {
    while (p->pos < p->len) {
        char ch = p->s[p->pos];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
            break;
        p->pos++;
    }
}

static void json_set_error(JsonParser* p, const char* msg) {
    if (p->err)
        return; /* keep the first error, not the last */
    size_t n = strlen(msg);
    p->err = xmalloc(n + 1);
    memcpy(p->err, msg, n + 1);
}

static bool json_match_literal(JsonParser* p, const char* lit) {
    size_t n = strlen(lit);
    if (p->pos + n > p->len || memcmp(p->s + p->pos, lit, n) != 0)
        return false;
    p->pos += (unsigned int)n;
    return true;
}

static AerVal json_parse_value(JsonParser* p);

static AerVal json_parse_string_raw(JsonParser* p) {
    p->pos++; /* opening quote */
    StrBuf b;
    strbuf_init(&b);
    while (p->pos < p->len && p->s[p->pos] != '"') {
        char ch = p->s[p->pos];
        if (ch != '\\') {
            strbuf_append_char(&b, ch);
            p->pos++;
            continue;
        }

        p->pos++;
        if (p->pos >= p->len) {
            json_set_error(p, "Unterminated escape in JSON string");
            free(b.buf);
            return aer_null();
        }
        char esc = p->s[p->pos];
        switch (esc) {
            case '"':
                strbuf_append_char(&b, '"');
                p->pos++;
                break;
            case '\\':
                strbuf_append_char(&b, '\\');
                p->pos++;
                break;
            case '/':
                strbuf_append_char(&b, '/');
                p->pos++;
                break;
            case 'n':
                strbuf_append_char(&b, '\n');
                p->pos++;
                break;
            case 't':
                strbuf_append_char(&b, '\t');
                p->pos++;
                break;
            case 'r':
                strbuf_append_char(&b, '\r');
                p->pos++;
                break;
            case 'b':
                strbuf_append_char(&b, '\b');
                p->pos++;
                break;
            case 'f':
                strbuf_append_char(&b, '\f');
                p->pos++;
                break;
            case 'u': {
                if (p->pos + 4 >= p->len) {
                    json_set_error(p, "Invalid \\u escape in JSON string");
                    free(b.buf);
                    return aer_null();
                }
                char hex[5];
                memcpy(hex, p->s + p->pos + 1, 4);
                hex[4] = '\0';
                unsigned int code = (unsigned int)strtoul(hex, NULL, 16);
                p->pos += 5;
                /* Encoded straight to UTF-8, basic-plane only -- no surrogate pair reconstruction, since
                   nothing here needs anything past the BMP. */
                if (code < 0x80) {
                    strbuf_append_char(&b, (char)code);
                } else if (code < 0x800) {
                    char buf2[2] = {(char)(0xC0 | (code >> 6)), (char)(0x80 | (code & 0x3F))};
                    strbuf_append_n(&b, buf2, 2);
                } else {
                    char buf3[3] = {(char)(0xE0 | (code >> 12)), (char)(0x80 | ((code >> 6) & 0x3F)),
                                    (char)(0x80 | (code & 0x3F))};
                    strbuf_append_n(&b, buf3, 3);
                }
                break;
            }
            default:
                json_set_error(p, "Invalid escape sequence in JSON string");
                free(b.buf);
                return aer_null();
        }
    }
    if (p->pos >= p->len) {
        json_set_error(p, "Unterminated string in JSON");
        free(b.buf);
        return aer_null();
    }
    p->pos++; /* closing quote */
    return aer_make_string(b.buf, (unsigned int)b.len);
}

static AerVal json_parse_number(JsonParser* p) {
    unsigned int start = p->pos;
    if (p->pos < p->len && p->s[p->pos] == '-')
        p->pos++;
    if (p->pos >= p->len || !isdigit((unsigned char)p->s[p->pos])) {
        json_set_error(p, "Invalid number in JSON");
        return aer_null();
    }
    if (p->s[p->pos] == '0') {
        p->pos++;
    } else {
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos]))
            p->pos++;
    }
    bool is_real = false;
    if (p->pos < p->len && p->s[p->pos] == '.') {
        is_real = true;
        p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->s[p->pos])) {
            json_set_error(p, "Invalid number in JSON");
            return aer_null();
        }
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos]))
            p->pos++;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        is_real = true;
        p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-'))
            p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->s[p->pos])) {
            json_set_error(p, "Invalid number in JSON");
            return aer_null();
        }
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos]))
            p->pos++;
    }
    unsigned int n = p->pos - start;
    char buf[64];
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1; /* absurd literal -- truncate rather than overflow */
    memcpy(buf, p->s + start, n);
    buf[n] = '\0';
    return is_real ? aer_real(strtod(buf, NULL)) : aer_int(strtoll(buf, NULL, 10));
}

static AerVal json_parse_array(JsonParser* p) {
    p->pos++; /* '[' */
    AerArray* r = vm_new_array();
    r->count = 0;
    r->capacity = 4;
    r->items = xmalloc(sizeof(AerVal) * r->capacity);
    r->shape = NULL;
    r->generation = 0;

    json_skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        return aer_array_val(r);
    }

    while (true) {
        AerVal v = json_parse_value(p);
        if (p->err)
            return aer_null();
        if (r->count >= r->capacity) {
            r->capacity *= 2;
            r->items = xrealloc(r->items, sizeof(AerVal) * r->capacity);
        }
        r->items[r->count++] = v;
        json_skip_ws(p);
        if (p->pos >= p->len) {
            json_set_error(p, "Unterminated array in JSON");
            return aer_null();
        }
        if (p->s[p->pos] == ',') {
            p->pos++;
            json_skip_ws(p);
            continue;
        }
        if (p->s[p->pos] == ']') {
            p->pos++;
            break;
        }
        json_set_error(p, "Expected ',' or ']' in JSON array");
        return aer_null();
    }
    return aer_array_val(r);
}

static AerVal json_parse_object(JsonParser* p) {
    p->pos++; /* '{' */
    AerDict* d = vm_new_dict();

    json_skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        return aer_dict_val(d);
    }

    while (true) {
        json_skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != '"') {
            json_set_error(p, "Expected string key in JSON object");
            return aer_null();
        }
        AerVal key_v = json_parse_string_raw(p);
        if (p->err)
            return aer_null();
        AerString* ks = aer_as_string(key_v);

        json_skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') {
            json_set_error(p, "Expected ':' in JSON object");
            return aer_null();
        }
        p->pos++;
        json_skip_ws(p);

        AerVal val = json_parse_value(p);
        if (p->err)
            return aer_null();

        unsigned int klen = hashtable_key_true_len(ks->data, ks->length);
        char* k = hashtable_key_dup_known(d->map.pools, ks->data, klen);
        hashtable_put(&d->map, k, klen, val);

        json_skip_ws(p);
        if (p->pos >= p->len) {
            json_set_error(p, "Unterminated object in JSON");
            return aer_null();
        }
        if (p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->s[p->pos] == '}') {
            p->pos++;
            break;
        }
        json_set_error(p, "Expected ',' or '}' in JSON object");
        return aer_null();
    }
    return aer_dict_val(d);
}

static AerVal json_parse_value(JsonParser* p) {
    json_skip_ws(p);
    if (p->pos >= p->len) {
        json_set_error(p, "Unexpected end of JSON input");
        return aer_null();
    }
    char ch = p->s[p->pos];
    if (ch == '"')
        return json_parse_string_raw(p);
    if (ch == '{' || ch == '[') {
        if (p->depth >= JSON_MAX_DEPTH) {
            json_set_error(p, "JSON nests too deeply");
            return aer_null();
        }
        p->depth++;
        AerVal v = (ch == '{') ? json_parse_object(p) : json_parse_array(p);
        p->depth--;
        return v;
    }
    if (ch == 't') {
        if (json_match_literal(p, "true"))
            return aer_bool(true);
        json_set_error(p, "Invalid literal in JSON");
        return aer_null();
    }
    if (ch == 'f') {
        if (json_match_literal(p, "false"))
            return aer_bool(false);
        json_set_error(p, "Invalid literal in JSON");
        return aer_null();
    }
    if (ch == 'n') {
        if (json_match_literal(p, "null"))
            return aer_null();
        json_set_error(p, "Invalid literal in JSON");
        return aer_null();
    }
    if (ch == '-' || isdigit((unsigned char)ch))
        return json_parse_number(p);
    json_set_error(p, "Unexpected character in JSON");
    return aer_null();
}

static AerVal json_decode(AerString* input, char** err_out) {
    JsonParser p = {input->data, input->length, 0, NULL, 0};
    AerVal result = json_parse_value(&p);
    if (!p.err) {
        json_skip_ws(&p);
        if (p.pos != p.len)
            json_set_error(&p, "Trailing data after JSON value");
    }
    *err_out = p.err;
    return p.err ? aer_null() : result;
}

/* dispatch                                                            */

bool aer_json_call(VM* vm, Chunk* c, int fn_id, int arg_count) {
    if (fn_id == FN_JSON_ENCODE && arg_count == 1) {
        AerVal v = vm_stack_pop(vm);
        StrBuf b;
        strbuf_init(&b);
        if (!json_encode_value(c, v, &b, 0)) {
            free(b.buf);
            vm_stack_push(vm, aer_null());
            return true;
        }
        vm_stack_push(vm, aer_make_string(b.buf, (unsigned int)b.len));
        return true;
    }
    if (fn_id == FN_JSON_DECODE && arg_count == 1) {
        AerVal s_v = vm_stack_pop(vm);
        if (aer_type(s_v) != TYPE_STRING) {
            error("json.decode() requires a string");
            vm_stack_push(vm, aer_null());
            return true;
        }

        char* err_msg = NULL;
        AerVal value = json_decode(aer_as_string(s_v), &err_msg);

        AerVal err = err_msg ? aer_make_string(err_msg, (unsigned int)strlen(err_msg)) : aer_null();
        vm_stack_push(vm, aer_make_result(err_msg ? aer_null() : value, err));
        return true;
    }

    return false;
}

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aer_stdlib.h"
#include "error.h"

/* Parsing a string can fail on input nobody controls, so these return (value, err) like every other
   fallible call rather than aborting the way the integer()/float() casts do -- those convert a
   number whose type is already known, which cannot fail. Shared by both since only the strtoll/
   strtod step and the error wording differ. */
static bool string_to_number(VM* vm, int arg_count, bool want_int) {
    const char* what = want_int ? "string.to_integer()" : "string.to_float()";
    AerVal a = vm_stack_pop(vm);
    if (aer_type(a) != TYPE_STRING) {
        error("%s requires a string", what);
        vm_stack_push(vm, aer_null());
        return true;
    }
    (void)arg_count;
    AerString* as = aer_as_string(a);
    /* strtoll/strtod consume only a leading sign and digits, so truncating to a fixed buffer cannot
       change what a real number parses to. */
    char buf[64];
    unsigned int n = as->length < sizeof(buf) - 1 ? as->length : (unsigned int)sizeof(buf) - 1;
    memcpy(buf, as->data, n);
    buf[n] = '\0';

    char* end;
    errno = 0;
    AerVal value = want_int ? aer_int(strtoll(buf, &end, 10)) : aer_real(strtod(buf, &end));
    while (*end == ' ' || *end == '\t')
        end++;
    if (end == buf || *end != '\0') {
        char msg[128];
        snprintf(msg, sizeof(msg), "'%.*s' is not %s", (int)n, buf, want_int ? "an integer" : "a number");
        vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(msg)));
        return true;
    }
    /* Out of range is a different failure from malformed, and silently clamping to the extreme
       would be a wrong answer rather than a reported one. */
    if (errno == ERANGE) {
        char msg[128];
        snprintf(msg, sizeof(msg), "'%.*s' is out of range", (int)n, buf);
        vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(msg)));
        return true;
    }
    vm_stack_push(vm, aer_make_result(value, aer_null()));
    return true;
}

bool aer_string_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_STRING_TO_INTEGER && arg_count == 1)
        return string_to_number(vm, arg_count, true);
    if (fn_id == FN_STRING_TO_FLOAT && arg_count == 1)
        return string_to_number(vm, arg_count, false);
    if (fn_id == FN_STRING_UPPER && arg_count == 1) {
        AerVal a = vm_stack_pop(vm);
        if (aer_type(a) != TYPE_STRING) {
            error("string.upper() requires a string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerString* as = aer_as_string(a);
        unsigned int len = as->length;
        if (len <= AER_STRING_INLINE_MAX) {
            char stackbuf[AER_STRING_INLINE_MAX + 1];
            for (unsigned int i = 0; i < len; i++)
                stackbuf[i] = (char)toupper((unsigned char)as->data[i]);
            vm_stack_push(vm, aer_make_string_copy(stackbuf, len));
            return true;
        }
        char* buf = xmalloc(len + 1);
        for (unsigned int i = 0; i < len; i++)
            buf[i] = (char)toupper((unsigned char)as->data[i]);
        buf[len] = '\0';
        vm_stack_push(vm, aer_make_string(buf, len));
        return true;
    }
    if (fn_id == FN_STRING_LOWER && arg_count == 1) {
        AerVal a = vm_stack_pop(vm);
        if (aer_type(a) != TYPE_STRING) {
            error("string.lower() requires a string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerString* as = aer_as_string(a);
        unsigned int len = as->length;
        if (len <= AER_STRING_INLINE_MAX) {
            char stackbuf[AER_STRING_INLINE_MAX + 1];
            for (unsigned int i = 0; i < len; i++)
                stackbuf[i] = (char)tolower((unsigned char)as->data[i]);
            vm_stack_push(vm, aer_make_string_copy(stackbuf, len));
            return true;
        }
        char* buf = xmalloc(len + 1);
        for (unsigned int i = 0; i < len; i++)
            buf[i] = (char)tolower((unsigned char)as->data[i]);
        buf[len] = '\0';
        vm_stack_push(vm, aer_make_string(buf, len));
        return true;
    }
    if (fn_id == FN_STRING_TRIM && arg_count == 1) {
        AerVal a = vm_stack_pop(vm);
        if (aer_type(a) != TYPE_STRING) {
            error("string.trim() requires a string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerString* as = aer_as_string(a);
        const char* data = as->data;
        unsigned int start = 0, end = as->length;
        while (start < end && isspace((unsigned char)data[start]))
            start++;
        while (end > start && isspace((unsigned char)data[end - 1]))
            end--;
        unsigned int n = end - start;
        vm_stack_push(vm, aer_make_string_copy(data + start, n));
        return true;
    }
    if (fn_id == FN_STRING_CONTAINS && arg_count == 2) {
        AerVal needle = vm_stack_pop(vm);
        AerVal hay = vm_stack_pop(vm);
        if (aer_type(hay) != TYPE_STRING || aer_type(needle) != TYPE_STRING) {
            error("string.contains() requires two strings");
            vm_stack_push(vm, aer_null());
            return true;
        }
        vm_stack_push(vm, aer_bool(aer_string_find(aer_as_string(hay), aer_as_string(needle)) >= 0));
        return true;
    }
    if (fn_id == FN_STRING_INDEX_OF && arg_count == 2) {
        AerVal needle = vm_stack_pop(vm);
        AerVal hay = vm_stack_pop(vm);
        if (aer_type(hay) != TYPE_STRING || aer_type(needle) != TYPE_STRING) {
            error("string.index_of() requires two strings");
            vm_stack_push(vm, aer_null());
            return true;
        }
        vm_stack_push(vm, aer_int(aer_string_find(aer_as_string(hay), aer_as_string(needle))));
        return true;
    }
    if (fn_id == FN_STRING_SPLIT && arg_count == 2) {
        AerVal sep_v = vm_stack_pop(vm);
        AerVal s_v = vm_stack_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(sep_v) != TYPE_STRING) {
            error("string.split() requires two strings");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerString* ss = aer_as_string(s_v);
        AerString* seps = aer_as_string(sep_v);
        unsigned int slen = ss->length, seplen = seps->length;
        if (seplen == 0) {
            error("string.split() separator cannot be empty");
            vm_stack_push(vm, aer_null());
            return true;
        }
        const char* s = ss->data;
        const char* sep = seps->data;

        AerArray* r = vm_new_array();
        r->count = 0;
        r->capacity = 4;
        r->items = xmalloc(sizeof(AerVal) * r->capacity);
        r->shape = NULL;
        r->generation = 0;

        /* aer_bytes_find returns slen when no separator remains, which is exactly where the final
           segment ends -- so the trailing segment needs no special case. */
        unsigned int seg_start = 0;
        for (;;) {
            unsigned int at = aer_bytes_find(s, slen, sep, seplen, seg_start);
            if (r->count >= r->capacity) {
                r->capacity *= 2;
                r->items = xrealloc(r->items, sizeof(AerVal) * r->capacity);
            }
            r->items[r->count++] = aer_make_string_copy(s + seg_start, at - seg_start);
            if (at == slen)
                break;
            seg_start = at + seplen;
        }
        vm_stack_push(vm, aer_array_val(r));
        return true;
    }
    if (fn_id == FN_STRING_STARTS_WITH && arg_count == 2) {
        AerVal prefix_v = vm_stack_pop(vm);
        AerVal s_v = vm_stack_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(prefix_v) != TYPE_STRING) {
            error("string.starts_with() requires two strings");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerString* ss = aer_as_string(s_v);
        AerString* ps = aer_as_string(prefix_v);
        bool matches = ps->length <= ss->length && memcmp(ss->data, ps->data, ps->length) == 0;
        vm_stack_push(vm, aer_bool(matches));
        return true;
    }
    if (fn_id == FN_STRING_ENDS_WITH && arg_count == 2) {
        AerVal suffix_v = vm_stack_pop(vm);
        AerVal s_v = vm_stack_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(suffix_v) != TYPE_STRING) {
            error("string.ends_with() requires two strings");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerString* ss = aer_as_string(s_v);
        AerString* fs = aer_as_string(suffix_v);
        bool matches = fs->length <= ss->length &&
                       memcmp(ss->data + (ss->length - fs->length), fs->data, fs->length) == 0;
        vm_stack_push(vm, aer_bool(matches));
        return true;
    }
    if (fn_id == FN_STRING_REPEAT && arg_count == 2) {
        AerVal n_v = vm_stack_pop(vm);
        AerVal s_v = vm_stack_pop(vm);
        if (aer_type(s_v) != TYPE_STRING) {
            error("string.repeat() requires a string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        if (aer_type(n_v) != TYPE_INTEGER) {
            error("string.repeat() count must be an integer");
            vm_stack_push(vm, aer_null());
            return true;
        }
        int64_t n = aer_as_int(n_v);
        if (n < 0) {
            error("string.repeat() count must not be negative");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerString* ss = aer_as_string(s_v);
        /* Checked in 64 bits before the 32-bit multiply below -- length * n silently wrapping
           would undersize the buffer and the copy loop would write past it. */
        if (ss->length > 0 && (uint64_t)ss->length * (uint64_t)n > 0x7FFFFFFFULL) {
            error("string.repeat() result too large");
            vm_stack_push(vm, aer_null());
            return true;
        }
        unsigned int total = ss->length * (unsigned int)n;
        char* buf = xmalloc(total + 1);
        for (int64_t i = 0; i < n; i++)
            memcpy(buf + (unsigned int)i * ss->length, ss->data, ss->length);
        buf[total] = '\0';
        vm_stack_push(vm, aer_make_string(buf, total));
        return true;
    }
    if (fn_id == FN_STRING_REPLACE && arg_count == 3) {
        AerVal new_v = vm_stack_pop(vm);
        AerVal old_v = vm_stack_pop(vm);
        AerVal s_v = vm_stack_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(old_v) != TYPE_STRING ||
            aer_type(new_v) != TYPE_STRING) {
            error("string.replace() requires three strings");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerString* ss = aer_as_string(s_v);
        AerString* os = aer_as_string(old_v);
        AerString* nsv = aer_as_string(new_v);
        if (os->length == 0) {
            error("string.replace() 'old' argument cannot be empty");
            vm_stack_push(vm, aer_null());
            return true;
        }

        /* Count matches first so the output buffer is sized exactly once. Both passes search for the
           next match rather than testing every position -- see aer_bytes_find (value.h). */
        unsigned int matches = 0;
        for (unsigned int i = 0; i < ss->length;) {
            unsigned int at = aer_bytes_find(ss->data, ss->length, os->data, os->length, i);
            if (at == ss->length)
                break;
            matches++;
            i = at + os->length;
        }
        unsigned int total = ss->length - matches * os->length + matches * nsv->length;
        char* buf = xmalloc(total + 1);
        unsigned int pos = 0;
        unsigned int i = 0;
        while (i < ss->length) {
            unsigned int at = aer_bytes_find(ss->data, ss->length, os->data, os->length, i);
            unsigned int keep = at - i; /* the run before the match, or the whole tail when absent */
            memcpy(buf + pos, ss->data + i, keep);
            pos += keep;
            if (at == ss->length)
                break;
            memcpy(buf + pos, nsv->data, nsv->length);
            pos += nsv->length;
            i = at + os->length;
        }
        buf[total] = '\0';
        vm_stack_push(vm, aer_make_string(buf, total));
        return true;
    }
    if (fn_id == FN_STRING_JOIN && arg_count == 2) {
        AerVal sep_v = vm_stack_pop(vm);
        AerVal arr_v = vm_stack_pop(vm);
        if (aer_type(arr_v) != TYPE_ARRAY) {
            error("string.join() requires an array");
            vm_stack_push(vm, aer_null());
            return true;
        }
        if (aer_type(sep_v) != TYPE_STRING) {
            error("string.join() separator must be a string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerArray* arr = aer_as_array(arr_v);
        for (unsigned int i = 0; i < arr->count; i++)
            if (aer_type(arr->items[i]) != TYPE_STRING) {
                error("string.join() requires an array of strings");
                vm_stack_push(vm, aer_null());
                return true;
            }

        AerString* seps = aer_as_string(sep_v);
        unsigned int seplen = seps->length;
        unsigned int total = arr->count > 0 ? (arr->count - 1) * seplen : 0;
        for (unsigned int i = 0; i < arr->count; i++)
            total += aer_as_string(arr->items[i])->length;

        char* buf = xmalloc(total + 1);
        unsigned int pos = 0;
        for (unsigned int i = 0; i < arr->count; i++) {
            if (i > 0) {
                memcpy(buf + pos, seps->data, seplen);
                pos += seplen;
            }
            AerString* item = aer_as_string(arr->items[i]);
            unsigned int n = item->length;
            memcpy(buf + pos, item->data, n);
            pos += n;
        }
        buf[total] = '\0';
        vm_stack_push(vm, aer_make_string(buf, total));
        return true;
    }

    return false;
}

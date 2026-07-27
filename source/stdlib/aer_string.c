#include <ctype.h>
#include <string.h>
#include "aer_stdlib.h"
#include "error.h"

bool aer_string_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_STRING_UPPER && arg_count == 1) {
        AerVal a = vm_stack_pop(vm);
        if (aer_type(a) != TYPE_STRING) { error("string.upper() requires a string"); vm_stack_push(vm, aer_null()); return true; }
        AerString* as = aer_as_string(a);
        unsigned int len = as->length;
        char* buf = xmalloc(len + 1);
        for (unsigned int i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)as->data[i]);
        buf[len] = '\0';
        vm_stack_push(vm, aer_make_string(buf, len)); return true;
    }
    if (fn_id == FN_STRING_LOWER && arg_count == 1) {
        AerVal a = vm_stack_pop(vm);
        if (aer_type(a) != TYPE_STRING) { error("string.lower() requires a string"); vm_stack_push(vm, aer_null()); return true; }
        AerString* as = aer_as_string(a);
        unsigned int len = as->length;
        char* buf = xmalloc(len + 1);
        for (unsigned int i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)as->data[i]);
        buf[len] = '\0';
        vm_stack_push(vm, aer_make_string(buf, len)); return true;
    }
    if (fn_id == FN_STRING_TRIM && arg_count == 1) {
        AerVal a = vm_stack_pop(vm);
        if (aer_type(a) != TYPE_STRING) { error("string.trim() requires a string"); vm_stack_push(vm, aer_null()); return true; }
        AerString* as = aer_as_string(a);
        const char* data = as->data;
        unsigned int start = 0, end = as->length;
        while (start < end && isspace((unsigned char)data[start]))     start++;
        while (end > start && isspace((unsigned char)data[end - 1]))   end--;
        unsigned int n = end - start;
        char* buf = xmalloc(n + 1);
        memcpy(buf, data + start, n);
        buf[n] = '\0';
        vm_stack_push(vm, aer_make_string(buf, n)); return true;
    }
    if (fn_id == FN_STRING_CONTAINS && arg_count == 2) {
        AerVal needle = vm_stack_pop(vm); AerVal hay = vm_stack_pop(vm);
        if (aer_type(hay) != TYPE_STRING || aer_type(needle) != TYPE_STRING) { error("string.contains() requires two strings"); vm_stack_push(vm, aer_null()); return true; }
        vm_stack_push(vm, aer_bool(aer_string_find(aer_as_string(hay), aer_as_string(needle)) >= 0)); return true;
    }
    if (fn_id == FN_STRING_INDEX_OF && arg_count == 2) {
        AerVal needle = vm_stack_pop(vm); AerVal hay = vm_stack_pop(vm);
        if (aer_type(hay) != TYPE_STRING || aer_type(needle) != TYPE_STRING) { error("string.index_of() requires two strings"); vm_stack_push(vm, aer_null()); return true; }
        vm_stack_push(vm, aer_int(aer_string_find(aer_as_string(hay), aer_as_string(needle)))); return true;
    }
    if (fn_id == FN_STRING_SPLIT && arg_count == 2) {
        AerVal sep_v = vm_stack_pop(vm); AerVal s_v = vm_stack_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(sep_v) != TYPE_STRING) { error("string.split() requires two strings"); vm_stack_push(vm, aer_null()); return true; }
        AerString* ss = aer_as_string(s_v);
        AerString* seps = aer_as_string(sep_v);
        unsigned int slen = ss->length, seplen = seps->length;
        if (seplen == 0) { error("string.split() separator cannot be empty"); vm_stack_push(vm, aer_null()); return true; }
        const char* s = ss->data;
        const char* sep = seps->data;

        AerArray* r = vm_new_array();
        r->count = 0;
        r->capacity = 4;
        r->items = xmalloc(sizeof(AerVal) * r->capacity);
        r->shape = NULL;
        r->generation = 0;

        unsigned int seg_start = 0, i = 0;
        while (i <= slen) {
            bool at_sep = i + seplen <= slen && memcmp(s + i, sep, seplen) == 0;
            if (at_sep || i == slen) {
                unsigned int n = i - seg_start;
                char* buf = xmalloc(n + 1);
                memcpy(buf, s + seg_start, n);
                buf[n] = '\0';
                if (r->count >= r->capacity) {
                    r->capacity *= 2;
                    r->items = xrealloc(r->items, sizeof(AerVal) * r->capacity);
                }
                r->items[r->count++] = aer_make_string(buf, n);
                if (i == slen) break;
                i += seplen;
                seg_start = i;
            } else {
                i++;
            }
        }
        vm_stack_push(vm, aer_array_val(r)); return true;
    }
    if (fn_id == FN_STRING_STARTS_WITH && arg_count == 2) {
        AerVal prefix_v = vm_stack_pop(vm); AerVal s_v = vm_stack_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(prefix_v) != TYPE_STRING) { error("string.starts_with() requires two strings"); vm_stack_push(vm, aer_null()); return true; }
        AerString* ss = aer_as_string(s_v);
        AerString* ps = aer_as_string(prefix_v);
        bool matches = ps->length <= ss->length && memcmp(ss->data, ps->data, ps->length) == 0;
        vm_stack_push(vm, aer_bool(matches)); return true;
    }
    if (fn_id == FN_STRING_ENDS_WITH && arg_count == 2) {
        AerVal suffix_v = vm_stack_pop(vm); AerVal s_v = vm_stack_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(suffix_v) != TYPE_STRING) { error("string.ends_with() requires two strings"); vm_stack_push(vm, aer_null()); return true; }
        AerString* ss = aer_as_string(s_v);
        AerString* fs = aer_as_string(suffix_v);
        bool matches = fs->length <= ss->length && memcmp(ss->data + (ss->length - fs->length), fs->data, fs->length) == 0;
        vm_stack_push(vm, aer_bool(matches)); return true;
    }
    if (fn_id == FN_STRING_REPEAT && arg_count == 2) {
        AerVal n_v = vm_stack_pop(vm); AerVal s_v = vm_stack_pop(vm);
        if (aer_type(s_v) != TYPE_STRING) { error("string.repeat() requires a string"); vm_stack_push(vm, aer_null()); return true; }
        if (aer_type(n_v) != TYPE_INTEGER) { error("string.repeat() count must be an integer"); vm_stack_push(vm, aer_null()); return true; }
        int64_t n = aer_as_int(n_v);
        if (n < 0) { error("string.repeat() count must not be negative"); vm_stack_push(vm, aer_null()); return true; }
        AerString* ss = aer_as_string(s_v);
        /* Checked in 64 bits before the 32-bit multiply below — length * n silently wrapping
           would undersize the buffer and the copy loop would write past it. */
        if (ss->length > 0 && (uint64_t)ss->length * (uint64_t)n > 0x7FFFFFFFULL) {
            error("string.repeat() result too large"); vm_stack_push(vm, aer_null()); return true;
        }
        unsigned int total = ss->length * (unsigned int)n;
        char* buf = xmalloc(total + 1);
        for (int64_t i = 0; i < n; i++) memcpy(buf + (unsigned int)i * ss->length, ss->data, ss->length);
        buf[total] = '\0';
        vm_stack_push(vm, aer_make_string(buf, total)); return true;
    }
    if (fn_id == FN_STRING_REPLACE && arg_count == 3) {
        AerVal new_v = vm_stack_pop(vm); AerVal old_v = vm_stack_pop(vm); AerVal s_v = vm_stack_pop(vm);
        if (aer_type(s_v) != TYPE_STRING || aer_type(old_v) != TYPE_STRING || aer_type(new_v) != TYPE_STRING) {
            error("string.replace() requires three strings"); vm_stack_push(vm, aer_null()); return true;
        }
        AerString* ss  = aer_as_string(s_v);
        AerString* os  = aer_as_string(old_v);
        AerString* nsv = aer_as_string(new_v);
        if (os->length == 0) { error("string.replace() 'old' argument cannot be empty"); vm_stack_push(vm, aer_null()); return true; }

        /* Count matches first so the output buffer is sized exactly once */
        unsigned int matches = 0;
        for (unsigned int i = 0; i + os->length <= ss->length; ) {
            if (memcmp(ss->data + i, os->data, os->length) == 0) { matches++; i += os->length; }
            else i++;
        }
        unsigned int total = ss->length - matches * os->length + matches * nsv->length;
        char* buf = xmalloc(total + 1);
        unsigned int pos = 0;
        for (unsigned int i = 0; i < ss->length; ) {
            if (i + os->length <= ss->length && memcmp(ss->data + i, os->data, os->length) == 0) {
                memcpy(buf + pos, nsv->data, nsv->length);
                pos += nsv->length;
                i   += os->length;
            } else {
                buf[pos++] = ss->data[i++];
            }
        }
        buf[total] = '\0';
        vm_stack_push(vm, aer_make_string(buf, total)); return true;
    }
    if (fn_id == FN_STRING_JOIN && arg_count == 2) {
        AerVal sep_v = vm_stack_pop(vm); AerVal arr_v = vm_stack_pop(vm);
        if (aer_type(arr_v) != TYPE_ARRAY) { error("string.join() requires an array"); vm_stack_push(vm, aer_null()); return true; }
        if (aer_type(sep_v) != TYPE_STRING) { error("string.join() separator must be a string"); vm_stack_push(vm, aer_null()); return true; }
        AerArray* arr = aer_as_array(arr_v);
        for (unsigned int i = 0; i < arr->count; i++)
            if (aer_type(arr->items[i]) != TYPE_STRING) { error("string.join() requires an array of strings"); vm_stack_push(vm, aer_null()); return true; }

        AerString* seps = aer_as_string(sep_v);
        unsigned int seplen = seps->length;
        unsigned int total = arr->count > 0 ? (arr->count - 1) * seplen : 0;
        for (unsigned int i = 0; i < arr->count; i++) total += aer_as_string(arr->items[i])->length;

        char* buf = xmalloc(total + 1);
        unsigned int pos = 0;
        for (unsigned int i = 0; i < arr->count; i++) {
            if (i > 0) { memcpy(buf + pos, seps->data, seplen); pos += seplen; }
            AerString* item = aer_as_string(arr->items[i]);
            unsigned int n = item->length;
            memcpy(buf + pos, item->data, n);
            pos += n;
        }
        buf[total] = '\0';
        vm_stack_push(vm, aer_make_string(buf, total)); return true;
    }

    return false;
}

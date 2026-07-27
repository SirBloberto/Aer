#include <stdlib.h>
#include <string.h>
#include "aer_stdlib.h"
#include "error.h"
#include "hashtable.h"

/* qsort() comparator for sort() — only called once the caller has verified every element is TYPE_STRING or every element is numeric, so no type-mismatch case needs handling here. */
static int sort_cmp(const void* pa, const void* pb) {
    const AerVal* a = (const AerVal*)pa;
    const AerVal* b = (const AerVal*)pb;
    if (aer_type(*a) == TYPE_STRING)
        return aer_string_compare(aer_as_string(*a), aer_as_string(*b));
    double da = aer_type(*a) == TYPE_INTEGER ? (double)aer_as_int(*a) : aer_as_real(*a);
    double db = aer_type(*b) == TYPE_INTEGER ? (double)aer_as_int(*b) : aer_as_real(*b);
    return da < db ? -1 : (da > db ? 1 : 0);
}

bool aer_collection_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_COLLECTION_APPEND && arg_count == 2) {
        AerVal val = vm_stack_pop(vm); AerVal arr = vm_stack_pop(vm);
        if (aer_type(arr) != TYPE_ARRAY) { error("append() requires an array"); vm_stack_push(vm, aer_null()); return true; }
        AerArray* a = aer_as_array(arr);
        if (a->shape) { error("append() cannot add fields to a struct instance — structs have a fixed shape"); vm_stack_push(vm, aer_null()); return true; }
        if (a->count >= a->capacity) {
            a->capacity = a->capacity ? a->capacity * 2 : 4;
            a->items = xrealloc(a->items, sizeof(AerVal) * a->capacity);
        }
        gc_barrier_array(vm, a, val);
        a->items[a->count++] = val;
        a->generation++;   /* see AerArray.generation's own comment, value.h */
        vm_stack_push(vm, arr); return true;
    }
    if (fn_id == FN_COLLECTION_RESERVE && arg_count == 2) {
        AerVal n_v = vm_stack_pop(vm); AerVal arr = vm_stack_pop(vm);
        if (aer_type(arr) != TYPE_ARRAY) { error("reserve() requires an array"); vm_stack_push(vm, aer_null()); return true; }
        AerArray* a = aer_as_array(arr);
        if (a->shape) { error("reserve() cannot resize a struct instance — structs have a fixed shape"); vm_stack_push(vm, aer_null()); return true; }
        if (aer_type(n_v) != TYPE_INTEGER) { error("reserve() count must be an integer"); vm_stack_push(vm, aer_null()); return true; }
        int64_t n = aer_as_int(n_v);
        if (n < 0) { error("reserve() count cannot be negative"); vm_stack_push(vm, aer_null()); return true; }
        /* Pre-sizes items[] once, up front, to skip append()'s incremental double-on-overflow
           growth for the common "build a huge array via many appends" pattern -- mirrors
           hashtable_reserve's own contract (hashtable.c): never shrinks, a no-op if already big
           enough. Doesn't touch count -- unlike a packed array's fixed-size construction, this is
           purely a capacity hint; elements still only exist once actually appended/assigned. */
        if ((unsigned int)n > a->capacity) {
            a->capacity = (unsigned int)n;
            a->items = xrealloc(a->items, sizeof(AerVal) * a->capacity);
        }
        vm_stack_push(vm, arr); return true;
    }
    if (fn_id == FN_COLLECTION_DELETE && arg_count == 2) {
        AerVal key = vm_stack_pop(vm); AerVal obj = vm_stack_pop(vm);
        if (aer_type(obj) == TYPE_DICT) {
            if (aer_type(key) != TYPE_STRING) { error("delete() key must be a string"); vm_stack_push(vm, aer_null()); return true; }
            AerString* ks = aer_as_string(key);
            if (ks->length > VM_KEY_MAX) { error("Hashtable key too long (max %d bytes)", VM_KEY_MAX); vm_stack_push(vm, aer_null()); return true; }
            unsigned int klen = hashtable_key_true_len(ks->data, ks->length);
            char kbuf[VM_KEY_MAX + 1];
            memcpy(kbuf, ks->data, klen);
            kbuf[klen] = '\0';
            hashtable_remove(&aer_as_dict(obj)->map, kbuf, klen);
            vm_stack_push(vm, obj); return true;
        }
        if (aer_type(obj) == TYPE_ARRAY) {
            AerArray* a = aer_as_array(obj);
            if (a->shape) { error("delete() cannot remove fields from a struct instance — structs have a fixed shape"); vm_stack_push(vm, aer_null()); return true; }
            if (aer_type(key) != TYPE_INTEGER) { error("Array delete() index must be an integer"); vm_stack_push(vm, aer_null()); return true; }
            int64_t i = aer_as_int(key);
            if (i < 0) i += (int64_t)a->count;
            if (i < 0 || (uint64_t)i >= a->count) { error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(key), a->count); vm_stack_push(vm, aer_null()); return true; }
            memmove(&a->items[i], &a->items[i + 1], (size_t)(a->count - (uint64_t)i - 1) * sizeof(AerVal));
            a->count--;
            a->generation++;   /* see AerArray.generation's own comment, value.h */
            vm_stack_push(vm, obj); return true;
        }
        error("delete() requires a hashtable or array");
        vm_stack_push(vm, aer_null()); return true;
    }
    if (fn_id == FN_COLLECTION_COPY && arg_count == 1) {
        AerVal src = vm_stack_pop(vm);
        if (aer_type(src) == TYPE_ARRAY && !aer_as_array(src)->shape) {
            AerArray* a = aer_as_array(src);
            AerArray* r = vm_new_array();
            r->count    = a->count;
            r->capacity = a->count ? a->count : 4;
            r->items    = xmalloc(sizeof(AerVal) * r->capacity);
            r->shape    = NULL;
            r->generation = 0;
            memcpy(r->items, a->items, sizeof(AerVal) * a->count);
            vm_stack_push(vm, aer_array_val(r)); return true;
        }
        if (aer_type(src) == TYPE_DICT) {
            AerDict* d = aer_as_dict(src);
            AerDict* r = vm_new_dict();
            if (d->map.count > 0) hashtable_reserve(&r->map, d->map.count);
            for (unsigned int i = 0; i < d->map.count; i++) {
                /* Duped into r's own pools, not d's -- the copy owns r->map, not the original.
                   Each source entry's own .hash was already computed once at its original
                   insertion (cached right there in the dense array, not on any AerString) --
                   reused here instead of hashing the same bytes again. */
                char* k = hashtable_key_dup(r->map.pools, d->map.dense[i].key, d->map.dense[i].length, NULL);
                hashtable_put_hashed(&r->map, k, d->map.dense[i].length, d->map.dense[i].hash, d->map.dense[i].payload);
            }
            vm_stack_push(vm, aer_dict_val(r)); return true;
        }
        /* A struct instance is deliberately excluded — construct a fresh one instead (a shaped
           copy would also land in the wrong GC pool, see gc_barrier_array's pool split, vm.c). */
        error("copy() requires an array or dict");
        vm_stack_push(vm, aer_null()); return true;
    }
    if (fn_id == FN_COLLECTION_INSERT && arg_count == 3) {
        AerVal val = vm_stack_pop(vm); AerVal idx = vm_stack_pop(vm); AerVal arr = vm_stack_pop(vm);
        if (aer_type(arr) != TYPE_ARRAY || aer_as_array(arr)->shape) { error("insert() requires an array"); vm_stack_push(vm, aer_null()); return true; }
        if (aer_type(idx) != TYPE_INTEGER) { error("insert() index must be an integer"); vm_stack_push(vm, aer_null()); return true; }
        AerArray* a = aer_as_array(arr);
        int64_t i = aer_as_int(idx);
        if (i < 0) i += (int64_t)a->count;
        /* i == count is valid: insert at the end, same as append(). */
        if (i < 0 || (uint64_t)i > a->count) { error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(idx), a->count); vm_stack_push(vm, aer_null()); return true; }
        if (a->count >= a->capacity) {
            a->capacity = a->capacity ? a->capacity * 2 : 4;
            a->items = xrealloc(a->items, sizeof(AerVal) * a->capacity);
        }
        gc_barrier_array(vm, a, val);
        memmove(&a->items[i + 1], &a->items[i], (size_t)(a->count - (uint64_t)i) * sizeof(AerVal));
        a->items[i] = val;
        a->count++;
        a->generation++;   /* see AerArray.generation's own comment, value.h */
        vm_stack_push(vm, arr); return true;
    }
    if (fn_id == FN_COLLECTION_INDEX_OF && arg_count == 2) {
        AerVal val = vm_stack_pop(vm); AerVal arr = vm_stack_pop(vm);
        if (aer_type(arr) != TYPE_ARRAY || aer_as_array(arr)->shape) { error("index_of() requires an array"); vm_stack_push(vm, aer_null()); return true; }
        AerArray* a = aer_as_array(arr);
        int64_t found = -1;
        for (unsigned int i = 0; i < a->count; i++)
            if (values_equal(val, a->items[i])) { found = (int64_t)i; break; }
        vm_stack_push(vm, aer_int(found)); return true;
    }
    if (fn_id == FN_COLLECTION_KEYS && arg_count == 1) {
        AerVal src = vm_stack_pop(vm);
        if (aer_type(src) != TYPE_DICT) { error("keys() requires a hashtable"); vm_stack_push(vm, aer_null()); return true; }
        AerDict* d = aer_as_dict(src);
        AerArray* r = vm_new_array();
        r->count    = 0;
        r->capacity = d->map.count ? d->map.count : 4;
        r->items    = xmalloc(sizeof(AerVal) * r->capacity);
        r->shape    = NULL;
        r->generation = 0;
        for (unsigned int i = 0; i < d->map.count; i++) {
            unsigned int n = d->map.dense[i].length;
            char* buf = xmalloc(n + 1);
            memcpy(buf, d->map.dense[i].key, n);
            buf[n] = '\0';
            r->items[r->count++] = aer_make_string(buf, n);
        }
        vm_stack_push(vm, aer_array_val(r)); return true;
    }
    if (fn_id == FN_COLLECTION_SORT && arg_count == 1) {
        AerVal arr = vm_stack_pop(vm);
        if (aer_type(arr) != TYPE_ARRAY) { error("sort() requires an array"); vm_stack_push(vm, aer_null()); return true; }
        AerArray* a = aer_as_array(arr);
        if (a->shape) { error("sort() cannot sort a struct instance"); vm_stack_push(vm, aer_null()); return true; }
        /* Ordering across mixed types has no sensible answer, so it's rejected up front rather than falling back to an arbitrary tie-break. */
        bool numeric = true, stringy = true;
        for (unsigned int i = 0; i < a->count; i++) {
            if (aer_type(a->items[i]) != TYPE_INTEGER && aer_type(a->items[i]) != TYPE_REAL) numeric = false;
            if (aer_type(a->items[i]) != TYPE_STRING) stringy = false;
        }
        if (a->count > 0 && !numeric && !stringy) {
            error("sort() requires all elements to be numbers, or all to be strings");
            vm_stack_push(vm, aer_null()); return true;
        }
        qsort(a->items, a->count, sizeof(AerVal), sort_cmp);
        vm_stack_push(vm, arr); return true;
    }

    return false;
}

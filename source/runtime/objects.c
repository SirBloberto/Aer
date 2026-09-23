#include <stdlib.h>
#include <string.h>
#include "error.h"
#include "hashtable.h"
#include "heap.h"
#include "objects.h"
#include "typed_array.h"

AerVal aer_make_string(char* data, unsigned int length) {
    AerString* s = aer_string_alloc(length);
    if (length <= AER_STRING_INLINE_MAX) {
        /* Small-string optimization (see AerString's own comment, value.h): copy into this cell's
           own inline_buf and drop the caller's separately-allocated buffer. This exists only to keep
           every EXISTING aer_make_string call site correct without editing it -- it does NOT avoid
           an allocation by itself (the caller already paid for `data`'s malloc before calling this);
           aer_make_string_copy, below, is the version that actually avoids one. */
        memcpy(s->inline_buf, data, length);
        s->inline_buf[length] = '\0';
        s->data = s->inline_buf;
        free(data);
    } else {
        s->data = data;
    }
    return aer_string_val(s);
}

AerVal aer_make_string_copy(const char* src, unsigned int length) {
    AerString* s = aer_string_alloc(length);
    if (length <= AER_STRING_INLINE_MAX) {
        memcpy(s->inline_buf, src, length);
        s->inline_buf[length] = '\0';
        s->data = s->inline_buf;
    } else {
        char* buf = vm_string_payload_alloc(vm_require_current_heap(), length);
        memcpy(buf, src, length);
        buf[length] = '\0';
        s->data = buf;
    }
    return aer_string_val(s);
}

AerArray* vm_new_array(unsigned int capacity) {
    return heap_new_array(vm_require_current_heap(), capacity);
}

void vm_array_alloc_items(AerArray* a, unsigned int capacity) {
    vm_require_current_heap()->young_bytes += (size_t)capacity * sizeof(AerVal);
    a->items = xmalloc(sizeof(AerVal) * capacity);
    a->capacity = capacity;
}

void vm_array_grow_items(AerArray* a, unsigned int capacity) {
    vm_require_current_heap()->young_bytes += (size_t)(capacity - a->capacity) * sizeof(AerVal);
    a->items = xrealloc(a->items, sizeof(AerVal) * capacity);
    a->capacity = capacity;
}

AerDict* vm_new_dict(void) {
    VmHeap* heap = vm_require_current_heap();
    AerDict* d = heap_alloc(heap, &heap->dict_pool);
    /* Zeroed here, not left to each caller, so setting .pools below can't be wiped out by a
       caller's own zeroing running afterward. */
    memset(&d->map, 0, sizeof(d->map));
    d->map.pools = &heap->dict_hash_pools;
    /* pool_alloc only zeroes gc_state (byte 0) -- a reused cell's previous occupant's dirty_cards
       pointer would otherwise survive as garbage; see vm_new_array's identical reasoning. */
    d->dirty_cards = NULL;
    d->dirty_cards_bytes = 0;
    d->dirty_min_byte = (unsigned int)-1;
    d->dirty_max_byte = 0;
    d->dirty_all = false;
    return d;
}

AerFunction* vm_new_function(void) {
    VmHeap* heap = vm_require_current_heap();
    return heap_alloc(heap, &heap->function_pool);
}

AerVal aer_make_result(AerVal value, AerVal err) {
    VmHeap* heap = vm_require_current_heap();
    AerResult* r = heap_alloc(heap, &heap->result_pool);
    r->value = value;
    r->err = err;
    return aer_result_val(r);
}

AerVal aer_make_error(const char* msg) {
    return aer_make_string_copy(msg, (unsigned int)strlen(msg));
}

/* One struct field at its Shape-computed offset: packed-slot access for a typed 8-byte field, the
   narrow reader/writer for a 4-byte one, a 16-byte memcpy for TYPE_ANY. Looks offset/ftype/narrow up
   fresh, so opcode call sites should use vm_struct_field_read_at once the field cache has already
   resolved them. */
AerVal vm_struct_field_read(AerStruct* s, unsigned int slot) {
    ValueType ftype = s->shape->field_types[slot];
    unsigned int offset = s->shape->field_offsets[slot];
    unsigned char* p = s->fields + offset;
    if (ftype == TYPE_ANY) {
        AerVal v;
        memcpy(&v, p, sizeof(AerVal));
        return v;
    }
    if (s->shape->field_narrow[slot])
        return vm_narrow_field_read(p, ftype);
    return vm_packed_slot_read(p, ftype);
}

/* Returns an owned copy of dense[*idx]'s key. Shared by array-iteration's dict branch and
   pair-iteration. False once exhausted -- the dense array has no holes, so this is a plain
   bounds check, not a scan. */


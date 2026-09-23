#ifndef AER_OBJECTS_H
#define AER_OBJECTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "error.h"
#include "hashtable.h"
#include "heap.h"
#include "typed_array.h"
#include "value.h"

/* gc_state first -- see pool.h and AerArray's own comment (value.h) for why. Small fields ordered to
   fill gc_state's padding gap before map (which needs pointer alignment), not declaration-grouped
   by topic -- see value.h's own top comment. */
struct AerDict {
    unsigned char gc_state;
    /* Card marking for the O(n) minor-GC rescan fix -- same fields, same reasoning, as AerArray's
       own (value.h). dirty_cards indexes map.dense[] by its DENSE index (stable across ordinary
       insert/update; hashtable_remove's swap-compaction invalidates it, which is why
       collection.delete sets dirty_all rather than trying to shift the affected bit). */
    bool dirty_all;
    unsigned int dirty_cards_bytes;
    HashTable map;
    unsigned char* dirty_cards;
    /* Bounds the actual set-bit range since the last clear -- see AerArray's own comment (value.h)
       for why this is needed on top of dirty_cards itself. */
    unsigned int dirty_min_byte, dirty_max_byte;
};
_Static_assert(offsetof(struct AerDict, gc_state) == 0, "pool.c assumes gc_state is byte 0");

#define MAX_STRUCT_FIELDS 16

/* A struct type's blueprint (field names in order + default literals); individually heap-allocated
   and never moved/realloc'd, so AerStruct.shape pointers stay valid as the shape table grows. */
struct Shape {
    unsigned int name; /* pool index of the struct's type name */
    unsigned int field_count;
    unsigned int field_names[MAX_STRUCT_FIELDS]; /* pool indices, declaration order       */
    AerVal field_defaults[MAX_STRUCT_FIELDS];
    /* TYPE_ANY = no declared type. A declared type is enforced once at FIELD_SET/construction,
       then trusted -- the fused opcodes skip the runtime check on that side. */
    ValueType field_types[MAX_STRUCT_FIELDS];
    /* An int/real field whose default carried an `i`/`f` suffix: 4-byte storage instead of 8.
       Meaningless for any other field kind. */
    bool field_narrow[MAX_STRUCT_FIELDS];
    /* Offset into AerStruct.fields. A typed field is stored raw -- 8 bytes, or 4 if field_narrow,
       with the type known from this Shape rather than the instance. A TYPE_ANY field stays a boxed
       16-byte AerVal, since it can hold a reference the GC must trace. */
    unsigned int field_offsets[MAX_STRUCT_FIELDS];
    unsigned int instance_bytes; /* total size of the fields buffer -- sum of every field's width above */
};

/* Its own type rather than an AerArray with a shape: sharing one tag meant every TYPE_ARRAY site
   had to ask "but what if this is a struct", and one didn't -- for-in iteration walked a struct's
   fields unchecked. No count/capacity either; a struct's field count is always shape->field_count. */
struct AerStruct {
    unsigned char gc_state; /* byte 0, same pool.c convention as every other pool-managed type */
    Shape* shape;
    unsigned char* fields; /* set to (char*)a + sizeof(AerStruct) at construction -- inline in
                                   the same pool cell, matching AerArray's own items pointer trick */
};
_Static_assert(offsetof(struct AerStruct, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* One field at its own offset: raw for a typed field, a boxed AerVal for TYPE_ANY. Not file-static
   -- json.encode serializes structs through it too. */
AerVal vm_struct_field_read(AerStruct* s, unsigned int slot);

/* An empty plain array with room for `capacity` items; 0 leaves items NULL. */
AerArray* vm_new_array(unsigned int capacity);

/* An array's item buffer at `capacity`, which both set as a->capacity and count toward the next
   collection -- the array's one cell says nothing about them. alloc is for a new array, whose items
   field is still garbage; grow is for an existing one. */
void vm_array_alloc_items(AerArray* a, unsigned int capacity);
void vm_array_grow_items(AerArray* a, unsigned int capacity);

/* Same, for AerDict -- exposed for json.decode(). The caller must zero-init `map` itself. */
AerDict* vm_new_dict(void);

/* Must come from function_pool (pool_mark's slab lookup fails on xmalloc'd cells); returns
   uninitialized memory -- zero it yourself. */
AerFunction* vm_new_function(void);

static inline void vm_array_index_error(AerVal idx, unsigned int count) {
    if (aer_type(idx) != TYPE_INTEGER)
        error("Array index must be an integer");
    else
        error("Array index %lld out of bounds (len %u)", (long long)aer_as_int(idx), count);
}

/* An empty plain array with room for `capacity` items; 0 leaves items NULL. Every field is set, since
   pool_alloc zeroes only gc_state and a reused cell's stale dirty_cards pointer would be unsafe. */
static inline AerArray* heap_new_array(VmHeap* heap, unsigned int capacity) {
    AerArray* a = heap_alloc(heap, &heap->array_pool);
    a->count = 0;
    a->shape = NULL;
    a->generation = 0;
    a->dirty_cards = NULL;
    a->dirty_cards_bytes = 0;
    a->dirty_min_byte = (unsigned int)-1;
    a->dirty_max_byte = 0;
    a->dirty_all = false;
    if (capacity > 0) {
        vm_array_alloc_items(a, capacity);
    } else {
        a->items = NULL;
        a->capacity = 0;
    }
    return a;
}

static inline bool vm_dict_next_key(AerDict* d, int64_t* idx, AerVal* out_key) {
    if ((uint64_t)*idx >= d->map.count)
        return false;
    unsigned int key_len = d->map.dense[*idx].length;
    *out_key = aer_make_string_copy(d->map.dense[*idx].key,
                                    key_len); /* no chunk_add_pool interning -- see vm_to_str's comment */
    return true;
}

/* always_inline: counting the text made LTO stop inlining it, putting a call on every string built. */
static inline __attribute__((always_inline)) AerString* aer_string_alloc(unsigned int length) {
    VmHeap* heap = vm_require_current_heap();
    vm_heap_init(heap);
    AerString* s = heap_alloc(heap, &heap->string_pool);
    if (length > AER_STRING_INLINE_MAX)
        heap->young_bytes += (size_t)length + 1; /* the text, which a cell's own size says nothing about */
    s->length = length;
    s->hash = 0;
    return s;
}

/* Reads one 8-byte packed slot as AerVal -- no switch on field type needed since AerVal.as is
   exactly 8 bytes and every eligible type's bit pattern matches directly. A 3-way switch here
   measurably regressed packed arrays on ARM -- keep this branchless. */
static inline AerVal vm_packed_slot_read(unsigned char* slot, ValueType ftype) {
    AerVal v;
    v.tag = ftype;
    memcpy(&v.as, slot, 8);
    return v;
}

/* Inverse of vm_packed_slot_read. Caller must already have type-checked v. */
static inline void vm_packed_slot_write(unsigned char* slot, ValueType ftype, AerVal v) {
    (void)ftype;
    memcpy(slot, &v.as, 8);
}

static inline AerVal vm_narrow_field_read(unsigned char* p, ValueType ftype) {
    if (ftype == TYPE_INTEGER) {
        int32_t v;
        memcpy(&v, p, 4);
        return aer_int(v);
    }
    float v;
    memcpy(&v, p, 4);
    return aer_real((double)v);
}

/* The field's declared type is already enforced on v, so a float32 field only ever receives a real. */
static inline void vm_narrow_field_write(unsigned char* p, ValueType ftype, AerVal v) {
    if (ftype == TYPE_INTEGER) {
        int32_t iv = (int32_t)aer_as_int(v);
        memcpy(p, &iv, 4);
        return;
    }
    float fv = (float)aer_as_real(v);
    memcpy(p, &fv, 4);
}

/* Same "fail loudly, don't silently truncate" convention as vm_typed_array_check -- a narrow
   (int32) struct field additionally range-checks; a non-narrow field's ordinary
   `declared != TYPE_ANY && val->tag != declared` check (every OP_FIELD_SET-family handler's own)
   already confirms val really is an integer whenever ftype/declared is TYPE_INTEGER, so this only
   needs to add the extra range check on top of that, not re-verify the type itself. */
static inline bool vm_fits_narrow_field(ValueType ftype, bool narrow, AerVal val) {
    return !narrow || ftype != TYPE_INTEGER || (aer_as_int(val) >= INT32_MIN && aer_as_int(val) <= INT32_MAX);
}

static inline bool vm_check_narrow_field_write(ValueType ftype, bool narrow, AerVal val) {
    if (vm_fits_narrow_field(ftype, narrow, val))
        return true;
    error("Value %lld out of range for a narrow (int32) field", (long long)aer_as_int(val));
    return false;
}

/* Narrow (4-byte) counterparts of the raw-slot memcpy the OP_FIELD_GET_RAW_INT/REAL handlers use --
   widen into an ordinary int64_t/double registers[].as.i/registers[].as.d slot on read, narrow back on
   write. No range check on the int32 write side -- see the narrow raw field family's own comment
   (opcodes.def) for why that's the intentional, consistent-with-every-other-raw-opcode tradeoff here. */
static inline int64_t vm_raw_read_int32(unsigned char* p) {
    int32_t v;
    memcpy(&v, p, 4);
    return (int64_t)v;
}

static inline void vm_raw_write_int32(unsigned char* p, int64_t v) {
    int32_t iv = (int32_t)v;
    memcpy(p, &iv, 4);
}

static inline double vm_raw_read_float32(unsigned char* p) {
    float v;
    memcpy(&v, p, 4);
    return (double)v;
}

static inline void vm_raw_write_float32(unsigned char* p, double v) {
    float fv = (float)v;
    memcpy(p, &fv, 4);
}

/* As above, but offset/ftype/narrow come from vm_resolve_field's cache instead of being re-derived
   from s->shape. The branches themselves are free at any monomorphic site; what this avoids is the
   extra pointer-chase through s->shape on every access. */
static inline AerVal vm_struct_field_read_at(AerStruct* s, unsigned int offset, ValueType ftype,
                                             bool narrow) {
    unsigned char* p = s->fields + offset;
    if (ftype == TYPE_ANY) {
        AerVal v;
        memcpy(&v, p, sizeof(AerVal));
        return v;
    }
    if (narrow)
        return vm_narrow_field_read(p, ftype);
    return vm_packed_slot_read(p, ftype);
}

static inline void vm_struct_field_write_at(AerStruct* s, unsigned int offset, ValueType ftype, bool narrow,
                                            AerVal v) {
    unsigned char* p = s->fields + offset;
    if (ftype == TYPE_ANY) {
        memcpy(p, &v, sizeof(AerVal));
        return;
    }
    if (narrow) {
        vm_narrow_field_write(p, ftype, v);
        return;
    }
    vm_packed_slot_write(p, ftype, v);
}

/* Where an array index lands in [0, count), counting from the end when negative. */
static inline bool vm_array_position(AerVal idx, unsigned int count, uint64_t* out) {
    if (aer_type(idx) != TYPE_INTEGER)
        return false;
    int64_t i = aer_as_int(idx);
    if (i < 0)
        i += (int64_t)count;
    if (i < 0 || (uint64_t)i >= count)
        return false;
    *out = (uint64_t)i;
    return true;
}

/* Shared by h_index_get and the fused index-get handlers. Writes through `out`, which avoids a
   16-byte stack round-trip returning by value (see BINARY_OP_INT_REAL's comment, handlers.c), and is
   safe even if `out` aliases obj's or idx's register. */
static inline void vm_index_get_compute(AerVal obj, AerVal idx, AerVal* out) {
    if (aer_type(obj) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(obj);
        if (a->shape) {
            error("Struct fields are accessed with '.', not '[]'");
            *out = aer_null();
            return;
        }
        uint64_t i;
        if (!vm_array_position(idx, a->count, &i)) {
            vm_array_index_error(idx, a->count);
            *out = aer_null();
            return;
        }
        *out = a->items[i];
        return;
    } else if (aer_type(obj) == TYPE_DICT) {
        if (aer_type(idx) != TYPE_STRING) {
            error("Hashtable key must be a string");
            *out = aer_null();
            return;
        }
        AerString* is = aer_as_string(idx);
        unsigned int klen = hashtable_key_true_len(is->data, is->length);
        AerVal* found =
            hashtable_get_hashed(&aer_as_dict(obj)->map, is->data, klen, hashtable_string_hash(is, klen));
        if (!found) {
            *out = aer_null();
            return;
        }
        *out = *found;
        return;
    } else if (aer_type(obj) == TYPE_STRING) {
        AerString* os = aer_as_string(obj);
        if (aer_type(idx) != TYPE_INTEGER) {
            error("String index must be an integer");
            *out = aer_null();
            return;
        }
        int64_t i = aer_as_int(idx);
        int64_t len = (int64_t)os->length;
        if (i < 0)
            i += len;
        if (i < 0 || i >= len) {
            error("String index %lld out of bounds (len %lld)", (long long)aer_as_int(idx), (long long)len);
            *out = aer_null();
            return;
        }
        /* A character is a length-1 string; the byte is copied, since obj may be collected later. */
        *out = aer_make_string_copy(os->data + i, 1);
        return; /* no chunk_add_pool interning -- see vm_to_str's comment */
    } else if (aer_type(obj) == TYPE_RESULT) {
        /* result[0] is the value, result[1] is the err -- the same order every stdlib fallible
           function returns. */
        if (aer_type(idx) != TYPE_INTEGER) {
            error("Result index must be an integer");
            *out = aer_null();
            return;
        }
        int64_t i = aer_as_int(idx);
        AerResult* r = aer_as_result(obj);
        if (i == 0) {
            *out = r->value;
            return;
        }
        if (i == 1) {
            *out = r->err;
            return;
        }
        error("Result index %lld out of bounds (a Result only has indices 0 and 1)",
              (long long)aer_as_int(idx));
        *out = aer_null();
    } else if (aer_type(obj) == TYPE_TYPED_ARRAY) {
        AerTypedArray* ta = aer_as_typed_array(obj);
        uint64_t i;
        if (!vm_array_position(idx, ta->count, &i)) {
            vm_array_index_error(idx, ta->count);
            *out = aer_null();
            return;
        }
        unsigned int width = vm_typed_elem_width(ta->elem_kind);
        *out = vm_typed_elem_read(ta->data + (size_t)i * width, ta->elem_kind);
    } else {
        error("This value cannot be indexed — indexing reads from an array, a column, a hashtable, "
              "a string, or a Result (0 for the value, 1 for the error)");
        *out = aer_null();
    }
}

/* `a, b = expr` -- a genuine Result unpacks to (value, err); a plain 2-element array (not a
   struct/packed array, both dot-only) unpacks positionally; anything else is treated as (that
   value, null), the same "not a real Result? just a plain value" duck-typing |> already applies on
   its own left operand. Lets a function that can never fail just `return value` without fabricating
   a second one to satisfy this shape. Never allocates -- both branches only copy existing AerVals. */
static inline void vm_destructure_compute(AerVal src, AerVal* out0, AerVal* out1) {
    if (aer_type(src) == TYPE_RESULT) {
        AerResult* r = aer_as_result(src);
        *out0 = r->value;
        *out1 = r->err;
        return;
    }
    if (aer_type(src) == TYPE_ARRAY) {
        AerArray* a = aer_as_array(src);
        if (!a->shape && a->count == 2) {
            *out0 = a->items[0];
            *out1 = a->items[1];
            return;
        }
    }
    *out0 = src;
    *out1 = aer_null();
}

#endif

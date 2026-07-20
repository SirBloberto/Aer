#ifndef VALUE_H
#define VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   /* memcmp, for the inline string helpers below */

/* Forward declarations — mutual references between AerVal and collection types */
typedef struct AerArray       AerArray;
typedef struct AerDict        AerDict;
typedef struct AerFunction    AerFunction;
typedef struct AerString      AerString;
typedef struct AerPackedArray AerPackedArray;
typedef struct AerResult      AerResult;
typedef struct Shape       Shape;   /* full definition in vm.h — needs pool-index arrays */

typedef enum ValueType {
    TYPE_NULL,      /* zero-value; (AerVal){0} is null */
    TYPE_BOOLEAN,
    TYPE_INTEGER,
    TYPE_REAL,
    TYPE_STRING,
    TYPE_FUNCTION,
    TYPE_ARRAY,
    TYPE_DICT,
    /* Struct instances packed inline at 8 bytes/field — fixed-primitive fields only, and no
       standalone `arr[i]` reference value (only `arr[i].field`); see AerPackedArray below. */
    TYPE_PACKED_ARRAY,
    /* Tagged (value, err) pair, exactly one non-null — a real type (not a 2-array) so dispatch
       can recognize a Result on sight. */
    TYPE_RESULT,
    /* Never a real AerVal tag — only Shape.field_types[]'s "no declared type"; must stay last. */
    TYPE_ANY,
} ValueType;

/* The one runtime value type everywhere. Zero-init MUST decode as null (TYPE_NULL == 0) —
   mark_vm_roots scans never-written registers unconditionally. */
typedef struct AerVal {
    ValueType tag;
    union {
        bool      b;
        int64_t i;
        double    d;
        void*     ptr;
    } as;
} AerVal;

/* gc_state must be byte 0 in every pool-managed struct — pool.c reads the leading byte generically. */
struct AerArray {
    unsigned char gc_state;
    AerVal*      items;
    unsigned int count;
    unsigned int capacity;
    Shape*       shape;   /* NULL for ordinary arrays; set for struct instances */
};
_Static_assert(offsetof(struct AerArray, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* One raw byte block, 8 bytes per field in declared order: element i's field j is at
   data + i*(field_count*8) + j*8. A GC leaf — fields are fixed primitives, never heap refs, so no
   write barrier and no mark recursion. */
struct AerPackedArray {
    unsigned char gc_state;
    unsigned char* data;
    unsigned int  count;
    Shape*        shape;
};
_Static_assert(offsetof(struct AerPackedArray, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* Field order is size-sorted to minimize padding; arity fields fit uint16_t (MAX_PARAMS == 32). */
struct AerFunction {
    unsigned char gc_state;
    AerVal*      defaults;        /* NULL if min_arity == arity; else (arity - min_arity) compile-time-literal values */
    unsigned int code_offset;
    unsigned int max_registers;   /* this function's real peak register need — see ChunkFunction's own comment, vm.h */
    uint16_t     arity;
    uint16_t     min_arity;       /* params [0, min_arity) are required; [min_arity, arity) use defaults[] below, in order */
};
_Static_assert(offsetof(struct AerFunction, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* `data` is always exclusively owned, never a borrowed view — the GC sweep frees it
   unconditionally, so a borrowed pointer would double-free or dangle. */
struct AerString {
    unsigned char gc_state;
    char*        data;
    unsigned int length;
};
_Static_assert(offsetof(struct AerString, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* Set once at construction, never mutated — so no write barrier needed, unlike AerArray/AerDict. */
struct AerResult {
    unsigned char gc_state;
    AerVal value;
    AerVal err;
};
_Static_assert(offsetof(struct AerResult, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* AerDict is defined in vm.h (needs HashTable which is in hashtable.h) */

/* Boxes an exclusively-owned buffer as a TYPE_STRING — takes ownership, never copies. */
AerVal aer_make_string(char* data, unsigned int length);

/* Builds a Result from a (value, err) pair — exactly one of the two should be null. */
AerVal aer_make_result(AerVal value, AerVal err);

/* Every payload read/write goes through these accessors, never a direct `.as.x` elsewhere. */

static inline ValueType aer_type(AerVal v) { return v.tag; }

static inline AerVal aer_null(void) {
    AerVal v; v.tag = TYPE_NULL; v.as.ptr = NULL; return v;
}

static inline AerVal aer_bool(bool b) {
    /* as.i = 0 first — vm_packed_slot_write memcpy's the whole 8-byte union, not just byte 0 */
    AerVal v; v.tag = TYPE_BOOLEAN; v.as.i = 0; v.as.b = b; return v;
}

static inline AerVal aer_real(double d) {
    AerVal v; v.tag = TYPE_REAL; v.as.d = d; return v;
}

/* A plain `int64_t` fits the value union at any magnitude — no heap-boxed overflow path needed. */
static inline AerVal aer_int(int64_t n) {
    AerVal v; v.tag = TYPE_INTEGER; v.as.i = n; return v;
}

static inline AerVal aer_box_ptr(ValueType tag, void* p) {
    AerVal v; v.tag = tag; v.as.ptr = p; return v;
}

static inline AerVal aer_string_val(AerString* s)     { return aer_box_ptr(TYPE_STRING, s); }
static inline AerVal aer_function_val(AerFunction* f)  { return aer_box_ptr(TYPE_FUNCTION, f); }
static inline AerVal aer_array_val(AerArray* a)        { return aer_box_ptr(TYPE_ARRAY, a); }
static inline AerVal aer_dict_val(AerDict* d)          { return aer_box_ptr(TYPE_DICT, d); }
static inline AerVal aer_packed_array_val(AerPackedArray* a) { return aer_box_ptr(TYPE_PACKED_ARRAY, a); }
static inline AerVal aer_result_val(AerResult* r)       { return aer_box_ptr(TYPE_RESULT, r); }

static inline bool      aer_as_bool(AerVal v) { return v.as.b; }
static inline double    aer_as_real(AerVal v) { return v.as.d; }
static inline int64_t aer_as_int(AerVal v)  { return v.as.i; }

static inline AerString*   aer_as_string(AerVal v)   { return (AerString*)v.as.ptr; }
static inline AerFunction* aer_as_function(AerVal v) { return (AerFunction*)v.as.ptr; }
static inline AerArray*    aer_as_array(AerVal v)     { return (AerArray*)v.as.ptr; }
static inline AerDict*     aer_as_dict(AerVal v)      { return (AerDict*)v.as.ptr; }
static inline AerPackedArray* aer_as_packed_array(AerVal v) { return (AerPackedArray*)v.as.ptr; }
static inline AerResult*      aer_as_result(AerVal v)        { return (AerResult*)v.as.ptr; }

/* Byte-index of needle's first occurrence in hay, or -1; empty needle matches at 0. The one
   substring search behind string.contains/index_of and `in` on strings. */
static inline int64_t aer_string_find(const AerString* hay, const AerString* needle) {
    if (needle->length == 0) return 0;
    for (unsigned int i = 0; i + needle->length <= hay->length; i++)
        if (memcmp(hay->data + i, needle->data, needle->length) == 0) return (int64_t)i;
    return -1;
}

/* Lexicographic byte order, length tiebreak — the one ordering behind collection.sort and `<` on strings. */
static inline int aer_string_compare(const AerString* a, const AerString* b) {
    unsigned int n = a->length < b->length ? a->length : b->length;
    int c = memcmp(a->data, b->data, n);
    if (c != 0) return c;
    return (int)a->length - (int)b->length;
}

/* Coerces integer or real to a plain double; false for any other type. */
static inline bool aer_as_double(AerVal v, double* out) {
    if (aer_type(v) == TYPE_INTEGER) { *out = (double)aer_as_int(v); return true; }
    if (aer_type(v) == TYPE_REAL)    { *out = aer_as_real(v);        return true; }
    return false;
}

#endif

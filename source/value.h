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
typedef struct AerStruct      AerStruct;   /* full definition in vm.h — needs Shape's real definition, defined there too */
typedef struct AerPackedArray AerPackedArray;
typedef struct AerTypedArray  AerTypedArray;
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
    /* A single struct instance — its own type, not an AerArray with shape set (that was the old
       design; see AerStruct's own comment in vm.h for why it was split out). Fields are either raw
       (typed, 8 bytes, no tag) or a full boxed AerVal (TYPE_ANY only) per-field, per Shape.field_offsets. */
    TYPE_STRUCT,
    /* Struct-typed *arrays* packed inline at 8 bytes/field — fixed-primitive fields only, and no
       standalone `arr[i]` reference value (only `arr[i].field`); see AerPackedArray below. */
    TYPE_PACKED_ARRAY,
    /* A dense, uniformly-typed numeric array (int32/float32/int64/float64) — the numeric half of the
       `[value; count]` repeat-literal (the struct half is TYPE_PACKED_ARRAY above). Unlike
       AerPackedArray this has no Shape at all -- just one fixed element kind for the whole array.
       See AerTypedArray below. */
    TYPE_TYPED_ARRAY,
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
    /* Bumped on every mutation that can change which shapes occupy items[] -- index-assignment
       replacing an element (OP_INDEX_SET), and collection.append/delete/insert (aer_collection.c).
       NOT bumped by collection.sort (reorders, never replaces -- homogeneity is a set property,
       unaffected by order). Lets lbl_call's SPEC_KIND_ARRAY_OF_STRUCTS per-call-site cache (vm.c)
       skip its O(n) homogeneity re-scan when the same array at the same generation was already
       verified against the same shape on a previous call. */
    unsigned int generation;
    /* Card marking for the O(n) minor-GC rescan fix -- see gc_barrier_array's own comment (gc.c).
       dirty_cards is NULL until this array is actually remembered (its first old-array-holding-a-
       young-value write), so the common case (never promoted to old) pays nothing extra; one bit
       per element once allocated. dirty_all is a coarser fallback set by any operation that shifts
       element-to-index correspondence (collection.delete/insert/sort) -- rather than shift every
       affected bit for a rare path, the next minor GC just rescans the whole array that one cycle
       and clears dirty_all again. */
    unsigned char* dirty_cards;
    unsigned int   dirty_cards_bytes;
    bool           dirty_all;
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

/* Which numeric width/kind a TYPE_TYPED_ARRAY's elements are stored as. INT64/FLOAT64 are the
   "wide" (unsuffixed-literal) case, INT32/FLOAT32 the "narrow" (`i`/`f`-suffixed-literal) one --
   see the repeat-literal construction opcode's own comment (vm.h) for how one is chosen. */
typedef enum {
    TYPED_ELEM_INT32,
    TYPED_ELEM_FLOAT32,
    TYPED_ELEM_INT64,
    TYPED_ELEM_FLOAT64,
} TypedArrayElemKind;

/* A dense, fixed-width numeric array -- element i's raw bytes are at data + i*elem_width. A GC
   leaf, same reasoning as AerPackedArray above: every element is a fixed numeric primitive, never a
   heap reference, so no write barrier and no mark recursion. No Shape -- there are no fields, just
   one uniform element kind for the whole array. */
struct AerTypedArray {
    unsigned char      gc_state;
    unsigned char*     data;
    unsigned int       count;
    TypedArrayElemKind elem_kind;
};
_Static_assert(offsetof(struct AerTypedArray, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* Field order is size-sorted to minimize padding; arity fields fit uint16_t (MAX_PARAMS == 32). */
struct AerFunction {
    unsigned char gc_state;
    AerVal*      defaults;        /* NULL if min_arity == arity; else (arity - min_arity) compile-time-literal values */
    unsigned int code_offset;
    unsigned int max_registers;   /* this function's real peak register need — see ChunkFunction's own comment, vm.h */
    unsigned int max_raw_ints;    /* this function's real peak raw_ints[] slot need -- see ChunkFunction's own comment, vm.h */
    unsigned int max_raw_reals;   /* same, for raw_reals[] */
    uint16_t     arity;
    uint16_t     min_arity;       /* params [0, min_arity) are required; [min_arity, arity) use defaults[] below, in order */
};
_Static_assert(offsetof(struct AerFunction, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* Small-string optimization: any string of length <= AER_STRING_INLINE_MAX lives entirely inside
   inline_buf, no separate heap allocation at all. Chosen so `data` is ALWAYS a valid pointer to
   dereference, whether it points at inline_buf (short) or a separate xmalloc'd buffer (long) --
   every existing read site (`s->data`/`s->length`) keeps working completely unchanged; only
   aer_make_string (the sole construction site) and free_string (gc.c, the sole finalizer) know the
   difference, by comparing `length` against AER_STRING_INLINE_MAX (never a separate flag -- length
   already determines it unambiguously). This is the real fix for the "11 separate heap events per
   log_processing.aer line" finding (ARCHITECTURE.md) -- almost all of those strings are well under
   this threshold. `data` is still always exclusively owned, never a borrowed view, for the long
   case: the GC sweep frees it unconditionally (unless it's pointing at this same cell's own
   inline_buf), so a borrowed pointer would double-free or dangle. No dict-key hash cache here
   (tried as cached_hash/hash_valid fields, then again as a side array keyed off an aligned-slab
   pool redesign) -- both were removed: the first for coupling a leaf value type to hashtable.c's
   hashing details for a narrow win, the second measured as a net regression across most real
   workloads (see pool.h). */
#define AER_STRING_INLINE_MAX 15
struct AerString {
    unsigned char gc_state;
    char*        data;
    unsigned int length;
    char         inline_buf[AER_STRING_INLINE_MAX + 1];   /* NUL-terminated, like data always is */
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

/* Copies from ANY source (stack buffer, source-text slice, string literal) -- never takes
   ownership, unlike aer_make_string above. This is the real small-string-optimization entry point:
   aer_make_string alone can't avoid a heap allocation for a short string, since by the time a
   caller has an owned buffer to hand it, that caller has already paid for one. A caller that
   already has the bytes sitting in a stack buffer or borrowed slice should call this instead of
   xmalloc+memcpy-ing its own owned copy first. */
AerVal aer_make_string_copy(const char* src, unsigned int length);

/* Builds a Result from a (value, err) pair — exactly one of the two should be null. */
AerVal aer_make_result(AerVal value, AerVal err);

/* Copies msg into an owned buffer and boxes it as a TYPE_STRING -- the common shape of building a
   Result's error side from a C string literal or a static message. */
AerVal aer_make_error(const char* msg);

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
static inline AerVal aer_struct_val(AerStruct* s)      { return aer_box_ptr(TYPE_STRUCT, s); }
static inline AerVal aer_packed_array_val(AerPackedArray* a) { return aer_box_ptr(TYPE_PACKED_ARRAY, a); }
static inline AerVal aer_typed_array_val(AerTypedArray* a) { return aer_box_ptr(TYPE_TYPED_ARRAY, a); }
static inline AerVal aer_result_val(AerResult* r)       { return aer_box_ptr(TYPE_RESULT, r); }

static inline bool      aer_as_bool(AerVal v) { return v.as.b; }
static inline double    aer_as_real(AerVal v) { return v.as.d; }
static inline int64_t aer_as_int(AerVal v)  { return v.as.i; }

static inline AerString*   aer_as_string(AerVal v)   { return (AerString*)v.as.ptr; }
static inline AerFunction* aer_as_function(AerVal v) { return (AerFunction*)v.as.ptr; }
static inline AerArray*    aer_as_array(AerVal v)     { return (AerArray*)v.as.ptr; }
static inline AerDict*     aer_as_dict(AerVal v)      { return (AerDict*)v.as.ptr; }
static inline AerStruct*   aer_as_struct(AerVal v)    { return (AerStruct*)v.as.ptr; }
static inline AerPackedArray* aer_as_packed_array(AerVal v) { return (AerPackedArray*)v.as.ptr; }
static inline AerTypedArray*  aer_as_typed_array(AerVal v)  { return (AerTypedArray*)v.as.ptr; }
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

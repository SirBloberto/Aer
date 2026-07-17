#ifndef VALUE_H
#define VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations — mutual references between AerVal and collection types */
typedef struct AerArray       AerArray;
typedef struct AerDict        AerDict;
typedef struct AerFunction    AerFunction;
typedef struct AerString      AerString;
typedef struct AerPackedArray AerPackedArray;
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
    /* A fixed-size, mass-allocated array of one struct type's instances, packed inline (no
       per-element heap allocation, no pointer indirection) — see AerPackedArray below. Only
       constructible for a struct whose every field is a fixed-primitive type (integer/float/
       boolean; enforced at construction, vm.c) — never any/string/nested-struct, since those can't
       be packed at a uniform byte width. Deliberately has no standalone per-element reference
       value: `arr[i]` alone is not legal, only `arr[i].field` (get/set) is — see OP_INDEX_FIELD_GET/
       SET's own comment, vm.h, for why (packed indexing is pure arithmetic, so there's nothing a
       standalone reference would save over recomputing it at each access). */
    TYPE_PACKED_ARRAY,
    /* Not a real value tag — never written into an AerVal.tag, only into Shape.field_types[] (see
       Shape's own comment, vm.h) to mean "this struct field has no declared type." Appended last
       so it can't disturb TYPE_NULL's load-bearing == 0 invariant or any existing tag value. */
    TYPE_ANY,
} ValueType;

/* AerVal: the one runtime value type, everywhere — every VM stack slot, register, array element,
   dict entry, struct field, and host-embedding argument/result (AerNativeFn, include/aer.h) is one
   of these. See the accessor functions further down this file.
   The tag MUST default to TYPE_NULL (0) on zero-init — mark_vm_roots (vm.c) scans every
   register unconditionally, relying on a never-yet-written register decoding as a harmless leaf
   value. TYPE_NULL is declared first in ValueType above specifically so this holds automatically
   for any zero-initialized AerVal. */
typedef struct AerVal {
    ValueType tag;
    union {
        bool      b;
        int64_t i;
        double    d;
        void*     ptr;
    } as;
} AerVal;

/* Defined after AerVal so items[] can use the complete internal value type.
   gc_state must be first — pool.c treats every pool-managed struct's leading byte as its GC
   state, generically, without knowing the rest of the layout (see pool.h). */
struct AerArray {
    unsigned char gc_state;
    AerVal*      items;
    unsigned int count;
    unsigned int capacity;
    Shape*       shape;   /* NULL for ordinary arrays; set for struct instances */
};
_Static_assert(offsetof(struct AerArray, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* One struct type's instances, packed inline in one raw byte block instead of AerArray's per-
   element AerVal pointers-to-scattered-instances — see TYPE_PACKED_ARRAY's own comment above.
   Every field packs at a fixed 8-byte slot (matching AerVal's numeric payload width — no bit-width
   type support), in the struct's declared field order, so element i's field j lives at
   `data + i * (shape->field_count * 8) + j * 8`. A LEAF for the GC — every field is a fixed
   primitive (integer/float/boolean), never a heap reference, so unlike AerArray this never needs a
   write barrier and its mark step never recurses into contents (see mark_value's TYPE_PACKED_ARRAY
   case, vm.c). gc_state first, same reasoning as AerArray above. */
struct AerPackedArray {
    unsigned char gc_state;
    unsigned char* data;
    unsigned int  count;
    Shape*        shape;
};
_Static_assert(offsetof(struct AerPackedArray, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* gc_state first, same reasoning as AerArray above. Then pointer, then the pool-index-sized int
   (code_offset can exceed 65535 in a large program's constant pool, so it stays full width), then
   the two fields bounded by a language-level cap (arity/min_arity <= MAX_PARAMS ==
   SCOPE_SLOT_MAX == 32, comfortably inside uint16_t) — every AerFunction is a plain function value
   now that closures (and their upvalues array) are gone. */
struct AerFunction {
    unsigned char gc_state;
    AerVal*      defaults;        /* NULL if min_arity == arity; else (arity - min_arity) compile-time-literal values */
    unsigned int code_offset;
    unsigned int max_registers;   /* this function's real peak register need — see ChunkFunction's own comment, vm.h */
    uint16_t     arity;
    uint16_t     min_arity;       /* params [0, min_arity) are required; [min_arity, arity) use defaults[] below, in order */
};
_Static_assert(offsetof(struct AerFunction, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* `data` is always owned by the AerString — every construction site (lexer
   tokenizing, string ops in vm.c, stdlib functions) hands aer_make_string() a
   freshly xmalloc'd buffer it exclusively owns, never a borrowed view into
   another string/buffer. This is load-bearing for the garbage collector
   (see Memory and Security in the README): sweeping an AerString always
   frees `data` unconditionally, so a borrowed pointer would double-free or
   dangle the moment either the borrower or the lender is collected.
   gc_state first, same reasoning as AerArray above. */
struct AerString {
    unsigned char gc_state;
    char*        data;
    unsigned int length;
};
_Static_assert(offsetof(struct AerString, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* AerDict is defined in vm.h (needs HashTable which is in hashtable.h) */

/* Wraps an existing (data, length) pair in a fresh heap-allocated AerString
   box and returns it as a TYPE_STRING AerVal. The caller must pass a buffer
   it owns exclusively (see AerString above) — aer_make_string() takes
   ownership, it never copies. Defined in vm.c; declared here so the lexer
   and parser (which construct string values directly from source text) can
   use it too, not just the VM itself. Internal-only (not part of the public
   embedding surface) — see include/aer.h for what host code actually uses. */
AerVal aer_make_string(char* data, unsigned int length);

/* Accessors for AerVal above — every read/write of its payload goes through one of these, never a
   direct `.as.x` elsewhere, so the representation can change again without touching call sites. */

static inline ValueType aer_type(AerVal v) { return v.tag; }

static inline AerVal aer_null(void) {
    AerVal v; v.tag = TYPE_NULL; v.as.ptr = NULL; return v;
}

static inline AerVal aer_bool(bool b) {
    /* as.i = 0 first: as.b only occupies byte 0, and vm_packed_slot_write (vm.c) memcpy's the
       whole 8-byte union into packed-array storage — without this, its other 7 bytes would be
       whatever garbage was already on the caller's stack. */
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

static inline bool      aer_as_bool(AerVal v) { return v.as.b; }
static inline double    aer_as_real(AerVal v) { return v.as.d; }
static inline int64_t aer_as_int(AerVal v)  { return v.as.i; }

static inline AerString*   aer_as_string(AerVal v)   { return (AerString*)v.as.ptr; }
static inline AerFunction* aer_as_function(AerVal v) { return (AerFunction*)v.as.ptr; }
static inline AerArray*    aer_as_array(AerVal v)     { return (AerArray*)v.as.ptr; }
static inline AerDict*     aer_as_dict(AerVal v)      { return (AerDict*)v.as.ptr; }
static inline AerPackedArray* aer_as_packed_array(AerVal v) { return (AerPackedArray*)v.as.ptr; }

/* Integer and real are both "a number" as far as most native-module math/time functions are
   concerned — coerces either into a plain double, false for any other type. */
static inline bool aer_as_double(AerVal v, double* out) {
    if (aer_type(v) == TYPE_INTEGER) { *out = (double)aer_as_int(v); return true; }
    if (aer_type(v) == TYPE_REAL)    { *out = aer_as_real(v);        return true; }
    return false;
}

#endif

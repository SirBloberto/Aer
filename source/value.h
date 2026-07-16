#ifndef VALUE_H
#define VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations — mutual references between Value and collection types */
typedef struct AerArray    AerArray;
typedef struct AerDict     AerDict;
typedef struct AerFunction AerFunction;
typedef struct AerString   AerString;
typedef struct Shape       Shape;   /* full definition in vm.h — needs pool-index arrays */
typedef struct Value       Value;   /* the STABLE PUBLIC boxed type — used only at the
                                        AerNativeFn host-embedding boundary from here on
                                        (see aer_host.c's aer_host_call and include/aer.h).
                                        Every internal storage location (the VM stack,
                                        scopes, arrays, dicts, chunk pool, ...) uses AerVal
                                        below instead. */

typedef enum ValueType {
    TYPE_NULL,      /* zero-value; (Value){0} is null */
    TYPE_BOOLEAN,
    TYPE_INTEGER,
    TYPE_REAL,
    TYPE_STRING,
    TYPE_FUNCTION,
    TYPE_ARRAY,
    TYPE_DICT,
    /* Not a real value tag — never written into an AerVal.tag, only into Shape.field_types[] (see
       Shape's own comment, vm.h) to mean "this struct field has no declared type." Appended last
       so it can't disturb TYPE_NULL's load-bearing == 0 invariant or any existing tag value. */
    TYPE_ANY,
} ValueType;

/* AerVal: the internal runtime value, as an explicit tagged union — every VM stack slot, scope
   variable, array element, dict entry, and struct field is one of these, not a Value. Replaced a
   NaN-boxed 8-byte encoding (a genuine double unless it matched a reserved bit pattern, which was
   then reinterpreted as a 3-bit tag + packed payload) after measuring, via direct machine-code
   disassembly against Lua's equivalent value representation, that NaN-boxing's decode cost
   (masking and shifting a word to test and extract that tag on every single touch) was the
   dominant remaining cost gap in the whole interpreter — a plain tag field is one aligned load,
   no decode at all. See the accessor functions further down this file.
     The tag MUST default to TYPE_NULL (0) on zero-init — mark_vm_roots (vm.c) scans every
   register unconditionally, relying on a never-yet-written register decoding as a harmless leaf
   value. TYPE_NULL is declared first in ValueType above specifically so this holds automatically
   for any zero-initialized AerVal, the same invariant this file already documents for the public
   Value struct above ("zero-value; (Value){0} is null"). */
typedef struct AerVal {
    ValueType tag;
    union {
        bool      b;
        int64_t i;
        double    d;
        void*     ptr;
    } as;
} AerVal;

typedef union ValueData {
    char   boolean;
    int64_t integer;
    double real;
    /* string/function are heap-allocated, not inline structs, so this union
       stays sized to a pointer instead of bloating every Value to fit their
       rarely-used fields. */
    AerString*   string;
    AerFunction* function;
    AerArray* array;
    AerDict*  dict;
} ValueData;

/* Value itself was already forward-declared above (needed by ValueData) —
   this completes it, matching the AerArray/AerDict/Shape pattern below. */
struct Value {
    ValueType type;
    ValueData data;
};

/* Defined after AerVal so items[] can use the complete internal value type.
   gc_state must be first — pool.c treats every pool-managed struct's leading byte as its GC
   state, generically, without knowing the rest of the layout (see pool.h). Costs real alignment
   padding (items needs pointer alignment) in exchange for pool.c never needing to load a per-pool
   offset before touching it — a variable-offset version was tried and measured a real ~7% slower
   wall-clock on sieve.aer (the added `Pool*` load + add on every write-barrier check outweighed
   the memory saved), so it was reverted in favor of this simpler, faster, universal-offset-0
   design — see pool.h's file comment and project memory for the measured trade-off. */
struct AerArray {
    unsigned char gc_state;
    AerVal*      items;
    unsigned int count;
    unsigned int capacity;
    Shape*       shape;   /* NULL for ordinary arrays; set for struct instances */
};
_Static_assert(offsetof(struct AerArray, gc_state) == 0, "pool.c assumes gc_state is byte 0");

/* gc_state first, same reasoning as AerArray above. Then pointer, then the two pool-index-sized
   ints (code_offset/receiver_type can each exceed 65535 in a large program's constant pool, so
   they stay full width), then the two fields bounded by a language-level cap (arity/min_arity <=
   MAX_PARAMS == SCOPE_SLOT_MAX == 32, comfortably inside uint16_t), then the single bool — every
   AerFunction is a plain function value now that closures (and their upvalues array) are gone. */
struct AerFunction {
    unsigned char gc_state;
    AerVal*      defaults;        /* NULL if min_arity == arity; else (arity - min_arity) compile-time-literal values */
    unsigned int code_offset;
    unsigned int receiver_type;   /* pool index of Type's name, if has_receiver */
    uint16_t     arity;
    uint16_t     min_arity;       /* params [0, min_arity) are required; [min_arity, arity) use defaults[] below, in order */
    bool         has_receiver;    /* true if param 0 was declared `p as Type` */
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
    AerVal v; v.tag = TYPE_BOOLEAN; v.as.b = b; return v;
}

static inline AerVal aer_real(double d) {
    AerVal v; v.tag = TYPE_REAL; v.as.d = d; return v;
}

/* No boxed-integer path exists under this representation — a plain `int64_t` fits the value
   union at any magnitude, no heap fallback needed (the old NaN-boxed encoding could only fit an
   inline 47-bit integer and needed a heap-boxed `long_pool` cell for the rare overflow case;
   that whole mechanism — long_pool, aer_int_boxed, aer_int_is_boxed/aer_int_box_ptr — is gone). */
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

static inline bool      aer_as_bool(AerVal v) { return v.as.b; }
static inline double    aer_as_real(AerVal v) { return v.as.d; }
static inline int64_t aer_as_int(AerVal v)  { return v.as.i; }

static inline AerString*   aer_as_string(AerVal v)   { return (AerString*)v.as.ptr; }
static inline AerFunction* aer_as_function(AerVal v) { return (AerFunction*)v.as.ptr; }
static inline AerArray*    aer_as_array(AerVal v)     { return (AerArray*)v.as.ptr; }
static inline AerDict*     aer_as_dict(AerVal v)      { return (AerDict*)v.as.ptr; }

/* Integer and real are both "a number" as far as most native-module math/time functions are
   concerned — coerces either into a plain double, false for any other type. */
static inline bool aer_as_double(AerVal v, double* out) {
    if (aer_type(v) == TYPE_INTEGER) { *out = (double)aer_as_int(v); return true; }
    if (aer_type(v) == TYPE_REAL)    { *out = aer_as_real(v);        return true; }
    return false;
}

/* The seam between AER's internal AerVal and the public boxed Value struct used by AerNativeFn's
   signature (README's Embedding section); used by aer_host.c's aer_host_call and by aer_io.c,
   which builds Value results from AerVal-returning helpers like aer_make_string. Value was
   already a plain tagged struct (never NaN-boxed), so this seam needed no changes when AerVal's
   own representation changed underneath it. */
static inline Value aer_val_to_public(AerVal v) {
    Value out = {0};
    out.type = aer_type(v);
    switch (out.type) {
        case TYPE_NULL:     break;
        case TYPE_BOOLEAN:  out.data.boolean  = aer_as_bool(v);     break;
        case TYPE_INTEGER:  out.data.integer  = aer_as_int(v);      break;
        case TYPE_REAL:     out.data.real     = aer_as_real(v);     break;
        case TYPE_STRING:   out.data.string   = aer_as_string(v);   break;
        case TYPE_FUNCTION: out.data.function = aer_as_function(v); break;
        case TYPE_ARRAY:    out.data.array    = aer_as_array(v);    break;
        case TYPE_DICT:     out.data.dict     = aer_as_dict(v);     break;
        case TYPE_ANY:      break;   /* never a real AerVal's tag — only Shape.field_types[] uses it */
    }
    return out;
}

/* The inverse. TYPE_REAL goes through aer_real() so a host constructing a value directly still
   goes through the same factory as everything else (no canonicalization needed anymore — that
   was purely a NaN-boxing concern, since a genuine NaN could collide with the boxed-tag bit
   pattern; an explicit tag field has no such collision to guard against). */
static inline AerVal aer_val_from_public(Value v) {
    switch (v.type) {
        case TYPE_NULL:     return aer_null();
        case TYPE_BOOLEAN:  return aer_bool(v.data.boolean);
        case TYPE_INTEGER:  return aer_int(v.data.integer);
        case TYPE_REAL:     return aer_real(v.data.real);
        case TYPE_STRING:   return aer_string_val(v.data.string);
        case TYPE_FUNCTION: return aer_function_val(v.data.function);
        case TYPE_ARRAY:    return aer_array_val(v.data.array);
        case TYPE_DICT:     return aer_dict_val(v.data.dict);
        case TYPE_ANY:      break;   /* never a real Value's type — only Shape.field_types[] uses it */
    }
    return aer_null();
}

#endif

#ifndef VALUE_BOX_H
#define VALUE_BOX_H

#include <stddef.h>
#include "value.h"

/* Accessor functions for AerVal's tagged-union representation (see AerVal's own definition,
   value.h, for the layout and the zero-init-is-TYPE_NULL invariant it relies on). Replaced a
   NaN-boxing scheme after measuring, via direct machine-code disassembly against Lua's own value
   representation, that NaN-boxing's decode cost (masking/shifting a packed word on every single
   value touch) was the dominant remaining cost gap in the interpreter — real measured result on
   nbody.aer: -11.2% instructions, -62.5% instructions on an isolated pure-arithmetic loop, and a
   3.3x drop in cache misses despite AerVal's size growing (8 -> 16 bytes) — the NaN-boxing decode
   logic this replaced turned out to cost real memory traffic of its own (64-bit mask constants
   too wide for an ARM immediate operand, loaded from a literal pool on every type check). */

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

/* The seam between AER's internal AerVal and the public boxed Value struct used by AerNativeFn's
   signature (README's Embedding section); used by aer_host.c's aer_host_call and by aer_io.c,
   which builds Value results from AerVal-returning helpers like aer_make_string. Value was
   already a plain tagged struct (never NaN-boxed), so this seam needed no changes at all when
   AerVal's own representation changed. */
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

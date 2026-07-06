#ifndef VALUE_BOX_H
#define VALUE_BOX_H

#include "value.h"

/* NaN-boxing encode/decode. AerVal (value.h) is one of two things:
   - a genuine IEEE-754 double, OR
   - a "boxed" value: sign=0, exponent=0x7FF (all 1s), quiet-bit=1 (bit 51),
     followed by a 3-bit tag (bits 50-48, one of the 8 ValueType values) and
     a 48-bit payload (bits 47-0).

       63              62──────52 51  50 49 48  47────────────────────────0
      ┌─┬─────────────────────┬───┬────────────┬──────────────────────────┐
      │S│  exponent (11 bits) │ Q │ tag (3b)   │        payload (48 bits) │
      └─┴─────────────────────┴───┴────────────┴──────────────────────────┘

   The boxed-test mask/pattern below is the standard NaN-boxing check used by
   JavaScriptCore/LuaJIT-style engines: any word that does NOT match TEST
   under MASK decodes directly as a double, zero-cost — real arithmetic never
   touches this file at all. Only a genuine *positive quiet NaN* (sign=0,
   exponent=0x7FF, quiet-bit=1) could collide with the reserved space; see
   aer_real()'s canonicalization below for why that never actually happens. */
#define AER_BOXED_MASK   0xFFF8000000000000ULL
#define AER_BOXED_TEST   0x7FF8000000000000ULL
#define AER_TAG_SHIFT    48
#define AER_PAYLOAD_MASK 0x0000FFFFFFFFFFFFULL   /* low 48 bits */

/* TYPE_INTEGER's own payload sub-format: bit 47 (the top payload bit) is an
   escape flag, not part of the value. 0 = the remaining 47 bits are a
   signed inline integer (±2^46, ~±70 trillion — 14 bits wider than the
   largest value anywhere in tests/test.aer today). 1 = the remaining 47
   bits are a pointer into `long_pool` (vm.c) holding the full 64-bit value,
   for the rare case a script needs more range than the inline fast path
   gives — this preserves AER's documented full 64-bit integer range exactly,
   at the cost of one heap cell only in that case. */
#define AER_BIGFLAG_BIT   (1ULL << 47)
#define AER_INT47_MASK    0x00007FFFFFFFFFFFULL  /* low 47 bits */
#define AER_INT47_SIGNBIT (1ULL << 46)           /* sign bit of the 47-bit value */
#define AER_INT47_MIN     (-(1LL << 46))
#define AER_INT47_MAX     ((1LL << 46) - 1)

static inline bool aer_is_boxed(AerVal v) {
    return (v.bits & AER_BOXED_MASK) == AER_BOXED_TEST;
}

static inline ValueType aer_type(AerVal v) {
    if (!aer_is_boxed(v)) return TYPE_REAL;
    return (ValueType)((v.bits >> AER_TAG_SHIFT) & 0x7ULL);
}

static inline AerVal aer_null(void) {
    AerVal v;
    v.bits = AER_BOXED_TEST | ((uint64_t)TYPE_NULL << AER_TAG_SHIFT);
    return v;
}

static inline AerVal aer_bool(bool b) {
    AerVal v;
    v.bits = AER_BOXED_TEST | ((uint64_t)TYPE_BOOLEAN << AER_TAG_SHIFT) | (b ? 1ULL : 0ULL);
    return v;
}

/* The only place a genuine NaN could collide with the boxed-tag space: `d != d` is true iff d is NaN, so any NaN (however it arose) is canonicalized to one fixed boxed pattern; every other real, including ±Infinity (quiet-bit=0, never collides), passes through untouched at zero cost. */
static inline AerVal aer_real(double d) {
    AerVal v;
    if (d != d) {
        v.bits = AER_BOXED_TEST | ((uint64_t)TYPE_REAL << AER_TAG_SHIFT);
        return v;
    }
    v.dbl = d;
    return v;
}

static inline AerVal aer_box_ptr(ValueType tag, void* p) {
    AerVal v;
    v.bits = AER_BOXED_TEST | ((uint64_t)tag << AER_TAG_SHIFT) |
             ((uint64_t)(uintptr_t)p & AER_PAYLOAD_MASK);
    return v;
}

static inline AerVal aer_string_val(AerString* s)     { return aer_box_ptr(TYPE_STRING, s); }
static inline AerVal aer_function_val(AerFunction* f)  { return aer_box_ptr(TYPE_FUNCTION, f); }
static inline AerVal aer_array_val(AerArray* a)        { return aer_box_ptr(TYPE_ARRAY, a); }
static inline AerVal aer_dict_val(AerDict* d)          { return aer_box_ptr(TYPE_DICT, d); }

/* aer_int (encode) needs to allocate from vm.c's long_pool on the rare overflow path, so unlike every other factory above it's a real function, defined in vm.c next to long_pool itself (same pattern as aer_make_string). */
AerVal aer_int(long long n);

static inline bool aer_as_bool(AerVal v) { return (v.bits & 1ULL) != 0; }

/* aer_real() canonicalizes every NaN to AER_BOXED_TEST's own pattern (TYPE_REAL tag, zero payload); reusing that exact pattern here decodes it correctly without relying on the platform's FP-exception behavior for 0.0/0.0. */
static inline double aer_as_real(AerVal v) {
    if (!aer_is_boxed(v)) return v.dbl;
    AerVal nan_val;
    nan_val.bits = AER_BOXED_TEST;
    return nan_val.dbl;
}

static inline void* aer_payload_ptr(AerVal v) {
    return (void*)(uintptr_t)(v.bits & AER_PAYLOAD_MASK);
}
static inline AerString*   aer_as_string(AerVal v)   { return (AerString*)aer_payload_ptr(v); }
static inline AerFunction* aer_as_function(AerVal v) { return (AerFunction*)aer_payload_ptr(v); }
static inline AerArray*    aer_as_array(AerVal v)     { return (AerArray*)aer_payload_ptr(v); }
static inline AerDict*     aer_as_dict(AerVal v)      { return (AerDict*)aer_payload_ptr(v); }

/* True iff v is a TYPE_INTEGER whose payload is a long_pool pointer, not inline — used by aer_as_int and vm.c's GC integration (mark_value/value_is_young) to know whether there's a heap cell to trace. */
static inline bool aer_int_is_boxed(AerVal v) {
    return aer_type(v) == TYPE_INTEGER && (v.bits & AER_BIGFLAG_BIT) != 0;
}
static inline void* aer_int_box_ptr(AerVal v) {
    return (void*)(uintptr_t)(v.bits & AER_INT47_MASK);
}

static inline long long aer_as_int(AerVal v) {
    if (aer_int_is_boxed(v)) return *(long long*)aer_int_box_ptr(v);
    uint64_t p47 = v.bits & AER_INT47_MASK;
    if (p47 & AER_INT47_SIGNBIT) p47 |= ~AER_INT47_MASK;   /* sign-extend from bit 46 */
    return (long long)p47;
}

/* The seam between AER's internal packed AerVal and the public boxed Value struct AerNativeFn's signature has always used (README's Embedding section); used by aer_host.c's aer_host_call and by aer_io.c, which builds Value results from AerVal-returning helpers like aer_make_string. */
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
    }
    return out;
}

/* The inverse. TYPE_REAL goes through aer_real() so a host constructing a NaN directly (bypassing every internal factory) is still canonicalized. */
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
    }
    return aer_null();
}

#endif

#ifndef VALUE_H
#define VALUE_H

#include <stdbool.h>
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
} ValueType;

/* AerVal: the internal, NaN-boxed 8-byte runtime value — every VM stack slot,
   scope variable, array element, dict entry, and struct field is one of
   these, not a Value. A word is a genuine IEEE-754 double UNLESS it matches
   a reserved bit pattern (sign=0, exponent=0x7FF, quiet-bit=1) — that region
   is repurposed to carry one of the 8 ValueTypes above in 3 tag bits plus a
   48-bit payload (a pointer, or — for TYPE_INTEGER — a 47-bit inline integer
   with a heap-boxed 64-bit fallback for the rare overflow case). See
   value_box.h for the full encode/decode logic and accessor functions;
   nothing outside that header and vm.c's `long_pool` wiring should ever read
   `.bits`/`.dbl` directly. */
typedef union AerVal {
    uint64_t bits;
    double   dbl;
} AerVal;

typedef union ValueData {
    char   boolean;
    long long integer;
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

/* Defined after AerVal so items[] can use the complete internal value type */
struct AerArray {
    AerVal*      items;
    unsigned int count;
    unsigned int capacity;
    Shape*       shape;   /* NULL for ordinary arrays; set for struct instances */
};

/* Pointer first, then the two pool-index-sized ints (code_offset/receiver_type can each exceed
   65535 in a large program's constant pool, so they stay full width), then the two fields bounded
   by a language-level cap (arity/min_arity <= MAX_PARAMS == SCOPE_SLOT_MAX == 32, comfortably
   inside uint16_t), then the single bool — 24 bytes on a 64-bit build now that closures (and their
   upvalues array) are gone; every AerFunction is a plain function value. */
struct AerFunction {
    AerVal*      defaults;        /* NULL if min_arity == arity; else (arity - min_arity) compile-time-literal values */
    unsigned int code_offset;
    unsigned int receiver_type;   /* pool index of Type's name, if has_receiver */
    uint16_t     arity;
    uint16_t     min_arity;       /* params [0, min_arity) are required; [min_arity, arity) use defaults[] below, in order */
    bool         has_receiver;    /* true if param 0 was declared `p as Type` */
};

/* `data` is always owned by the AerString — every construction site (lexer
   tokenizing, string ops in vm.c, stdlib functions) hands aer_make_string() a
   freshly xmalloc'd buffer it exclusively owns, never a borrowed view into
   another string/buffer. This is load-bearing for the garbage collector
   (see Memory and Security in the README): sweeping an AerString always
   frees `data` unconditionally, so a borrowed pointer would double-free or
   dangle the moment either the borrower or the lender is collected. */
struct AerString {
    char*        data;
    unsigned int length;
};

/* AerDict is defined in vm.h (needs HashMap which is in hashmap.h) */

/* Wraps an existing (data, length) pair in a fresh heap-allocated AerString
   box and returns it as a TYPE_STRING AerVal. The caller must pass a buffer
   it owns exclusively (see AerString above) — aer_make_string() takes
   ownership, it never copies. Defined in vm.c; declared here so the lexer
   and parser (which construct string values directly from source text) can
   use it too, not just the VM itself. Internal-only (not part of the public
   embedding surface) — see include/aer.h for what host code actually uses. */
AerVal aer_make_string(char* data, unsigned int length);

#endif

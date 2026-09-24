#ifndef AER_VALUE_FORMAT_H
#define AER_VALUE_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include "opcodes.h"
#include "strbuf.h"
#include "value.h"

/* Named rather than included: formatting a value should not need the bytecode layer. */
typedef struct Chunk Chunk;

/* Formats a real guaranteeing a decimal point/exponent/nan-inf marker survives -- bare "%g"
   prints 42.0 as "42", which flips to integer through the JSON round-trip. The one shared site
   (vm.c, aer_json.c, disasm.c). */
unsigned int aer_format_real(double d, char* buf, size_t bufsize);

/* Fast snprintf("%lld", ...) replacement -- see its own comment, value_format.c. */
unsigned int aer_format_int(long long v, char* buf, size_t bufsize);

/* The text of a value that renders without allocating -- null, integer, real, boolean, function -- using
   scratch for numbers; false for strings and collections. print(), interpolation and interpolated dict
   keys all render scalars through it, so a key cannot read back differently from how it was written. */
bool aer_format_scalar(AerVal v, char* scratch, size_t size, const char** text, unsigned int* len);

/* The spelling of each ValueType up to TYPE_DICT, and of each typed-array element kind. */
extern const char* const aer_value_type_names[TYPE_STRUCT];
extern const char* const aer_typed_elem_names[TYPED_ELEM_BOOL + 1];

/* Shared recursive rendering (value_format.c) behind print() and vm_to_str() (interpolation, +,
   etc.) -- one consistent representation, not a terse "<array[3]>" fallback. */
void vm_format_value(Chunk* c, AerVal v, bool in_collection, StrBuf* sb);
void vm_print_value(Chunk* c, AerVal v, bool in_collection);

/* Structural/reference equality, no error path -- the same scan OP_IN's array case uses. */
bool values_equal(AerVal a, AerVal b);

/* A struct reports its declared name, a packed array that name plus "[]". */
const char* vm_type_name(Chunk* c, AerVal v);

/* The operator as written, for a type-mismatch message. */
const char* binop_symbol(Opcode op);

/* The value as a string, formatting anything that is not one already. */
AerVal vm_to_str(Chunk* c, AerVal v);

#endif

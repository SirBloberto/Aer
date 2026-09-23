#ifndef AER_OPCODES_H
#define AER_OPCODES_H

typedef enum {
#define OPCODE(name, ...) OP_##name,
#include "opcodes.def"
#undef OPCODE
    OP_OPCODE_COUNT_MARKER /* not a real opcode -- sizes the static assert below */
} Opcode;
_Static_assert(OP_OPCODE_COUNT_MARKER <= 256, "Opcode enum exceeds one byte — widen the opcode field");

/* OP_CAST's target type. */
#define CAST_INTEGER 0
#define CAST_FLOAT 1
#define CAST_BOOLEAN 2
/* string() goes to OP_TO_STR rather than OP_CAST, which is why it has no vm_cast case. */
#define CAST_STRING 3
#define CAST_NONE (-1)

#endif

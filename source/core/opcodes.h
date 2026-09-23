#ifndef AER_OPCODES_H
#define AER_OPCODES_H

typedef enum {
#define OPCODE(name, ...) OP_##name,
#include "opcodes.def"
#undef OPCODE
    OP_OPCODE_COUNT_MARKER /* not a real opcode -- sizes the static assert below */
} Opcode;
_Static_assert(OP_OPCODE_COUNT_MARKER <= 256, "Opcode enum exceeds one byte — widen the opcode field");

#endif

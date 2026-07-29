#ifndef DISASM_H
#define DISASM_H
#ifdef AER_DEBUG_TOOLS

#include <stdio.h>
#include "vm.h"

/* Prints, in order: a full annotated disassembly of c->code (offset, opcode name, one-line
   description, decoded operands, and -- if c->debug_hits is populated -- a hit count and source
   line for that instruction); a per-opcode summary table (name -> total hits, sorted descending);
   and a per-source-line hot-spot rollup (line -> total hits, sorted descending). Debug-build only --
   see source/core/vm.h's AER_DEBUG_TOOLS-gated Chunk.debug_hits field. */
void aer_disassemble(Chunk* c, FILE* out);

#endif
#endif

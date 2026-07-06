#ifndef AER_JSON_H
#define AER_JSON_H

#include "vm.h"

/* Native module, dispatched like math/random/string/time (OP_CALL_MODULE, vm.c) — not host-registered like io, since JSON encode/decode needs no host capability. */
bool aer_json_call(VM* vm, Chunk* c, const char* name, int arg_count);

#endif

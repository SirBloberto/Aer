#ifndef AER_STDLIB_H
#define AER_STDLIB_H

#include "vm.h"

/* Declares the entire native-module surface (math/random/string/time/json/io) — one header for a
   fixed, closed set, since each is dispatched from the same OP_CALL_MODULE switch (vm.c). */

/* Called once from vm_init() — currently just seeds random()/randint(). */
void aer_stdlib_init(void);

/* The hardcoded set of native module names `import` accepts. `name` need not be NUL-terminated (matches a token's raw (data, length) span); len is how many bytes to compare. */
bool aer_stdlib_is_native_module(const char* name, unsigned int len);

/* Each pops arg_count arguments and pushes one result (or a null placeholder on a type error) if
   fn_id (FN_MATH_* etc., vm.h) belongs to that module, returning true; FN_ID_UNKNOWN (or a
   fn_id/arg_count combination that matches nothing) returns false with an untouched stack, which
   the call site (OP_CALL_MODULE, vm.c) turns into an unknown-function error by name. */
bool aer_math_call(VM* vm, int fn_id, int arg_count);
bool aer_random_call(VM* vm, int fn_id, int arg_count);
bool aer_string_call(VM* vm, int fn_id, int arg_count);
bool aer_time_call(VM* vm, int fn_id, int arg_count);

/* Native module, dispatched like math/random/string/time (OP_CALL_MODULE, vm.c) — not host-registered like io, since JSON encode/decode needs no host capability. Still takes Chunk* c (unlike the others above): json_encode_value needs it to resolve struct field names when encoding a struct instance. */
bool aer_json_call(VM* vm, Chunk* c, int fn_id, int arg_count);

/* Registers the "io" module (open/read/write/close) via aer_register_function — deliberately not wired into the OP_CALL_MODULE switch; file access is opt-in per host (main.c calls this after vm_init(); embed_smoke_test.c doesn't, so its scripts get none). See aer_io.c. */
void aer_io_register(void);

#endif

#ifndef AER_STDLIB_H
#define AER_STDLIB_H

#include "vm.h"

/* The entire native-module surface (math/random/string/time/json/collection/net/regex/io), all
   dispatched from vm.c's OP_CALL_MODULE switch. */

/* The hardcoded module names `import` accepts; `name` need not be NUL-terminated. */
bool aer_stdlib_is_native_module(const char* name, unsigned int len);

/* Each pops arg_count args and pushes one result if fn_id belongs to that module (true);
   an unmatched fn_id/arg_count returns false with an untouched stack. */
bool aer_math_call(VM* vm, int fn_id, int arg_count);
bool aer_random_call(VM* vm, int fn_id, int arg_count);
bool aer_string_call(VM* vm, int fn_id, int arg_count);
bool aer_time_call(VM* vm, int fn_id, int arg_count);
bool aer_collection_call(VM* vm, int fn_id, int arg_count);
bool aer_net_call(VM* vm, int fn_id, int arg_count);
bool aer_regex_call(VM* vm, int fn_id, int arg_count);

/* Takes Chunk* unlike the others — encoding a struct instance needs its field names. */
bool aer_json_call(VM* vm, Chunk* c, int fn_id, int arg_count);

/* Registers the "io" module via aer_register_function — called once per process by vm_init()
   (vm.c's ensure_io_registered), the same always-on status every other stdlib module already has.
   Declared here rather than called directly so aer_register_function's generic mechanism stays
   available for a host's own genuinely custom functions. */
void aer_io_register(void);

/* The argv slice io.args() returns; pointers are borrowed and must outlive every call. */
void aer_io_set_args(int argc, char** argv);

#endif

#ifndef AER_STDLIB_H
#define AER_STDLIB_H

#include "vm.h"

/* Called once from vm_init() — currently just seeds random()/randint(). */
void aer_stdlib_init(void);

/* The hardcoded set of native module names `import` accepts. `name` need not be NUL-terminated (matches a token's raw (data, length) span); len is how many bytes to compare. */
bool aer_stdlib_is_native_module(const char* name, unsigned int len);

/* Each pops arg_count arguments and pushes one result (or a null placeholder on a type error) if `name` belongs to that module, returning true; false with an untouched stack means an unknown-function error at the call site (OP_CALL_MODULE, vm.c). */
bool aer_math_call(VM* vm, Chunk* c, const char* name, int arg_count);
bool aer_random_call(VM* vm, Chunk* c, const char* name, int arg_count);
bool aer_string_call(VM* vm, Chunk* c, const char* name, int arg_count);
bool aer_time_call(VM* vm, Chunk* c, const char* name, int arg_count);

#endif

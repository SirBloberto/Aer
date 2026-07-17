#ifndef AER_HOST_H
#define AER_HOST_H

#include "aer.h"
#include "vm.h"

#define MAX_HOST_FUNCTIONS 64

/* AerNativeFn/aer_register_function are declared once, in the public header (include/aer.h) —
   this header adds only the registry internals the VM itself needs. */

/* True if name was registered as a module via aer_register_function; consulted by chunk_add_import (vm.c) so `import <name>` accepts it. */
bool aer_host_is_module(const char* name, unsigned int len);

/* Invokes module.fn_name with arg_count arguments already on vm's stack — pops them, calls the host function, pushes its result; returns false (stack untouched) if module/fn_name don't resolve. */
bool aer_host_call(VM* vm, const char* module, const char* fn_name, int arg_count);

#endif

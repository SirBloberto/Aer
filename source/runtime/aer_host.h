#ifndef AER_HOST_H
#define AER_HOST_H

#include "aer.h"
#include "vm.h"

#define MAX_HOST_FUNCTIONS 64

/* AerNativeFn and aer_register_function are in the public header; this is the registry side. */

/* Whether `import <name>` should accept it. */
bool aer_host_is_module(const char* name, unsigned int len);

/* Pops arg_count arguments from vm's stack and pushes the result. False, stack untouched, if the
   name does not resolve. */
bool aer_host_call(VM* vm, const char* module, const char* fn_name, int arg_count);

#endif

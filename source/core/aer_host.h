#ifndef AER_HOST_H
#define AER_HOST_H

#include "vm.h"

#define MAX_HOST_FUNCTIONS 64

/* A host-registered native function. `args` is a Value array built fresh for this call (aer_host_call converts each argument from AerVal) — valid only for the call's duration, not a live pointer into the VM's stack. Returns the result directly; call error() (error.h) to report a failure. */
typedef Value (*AerNativeFn)(VM* vm, int arg_count, Value* args, void* userdata);

/* Registers fn as module.name, callable once a script does `import module`; call before parsing/running any script that references it (typically right after vm_init()). Process-global, not per-VM. Returns false if the registry is full. */
bool aer_register_function(const char* module, const char* name, AerNativeFn fn, void* userdata);

/* True if name was registered as a module via aer_register_function; consulted by chunk_add_import (vm.c) so `import <name>` accepts it. */
bool aer_host_is_module(const char* name, unsigned int len);

/* Invokes module.fn_name with arg_count arguments already on vm's stack — pops them, calls the host function, pushes its result; returns false (stack untouched) if module/fn_name don't resolve. */
bool aer_host_call(VM* vm, const char* module, const char* fn_name, int arg_count);

#endif

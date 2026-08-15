#ifndef AER_MODULE_H
#define AER_MODULE_H

#include "vm.h"

/* Runs path_name.aer in its own Chunk+VM under `name`. Cached; a circular import errors. */
bool aer_module_load(const char* name, unsigned int len, const char* path_name, unsigned int path_len);

typedef enum { INSTANTIATE_OK, INSTANTIATE_PARSE_FAILED, INSTANTIATE_RUNTIME_FAILED } InstantiateResult;

/* Shared "make an independent VM+Chunk, load+compile+run its top-level code once" primitive --
   used by aer_module_load above and aer_actor_spawn (aer_actor.c). Out params are untouched on
   any failure; the partial VM/Chunk are already freed in that case. */
InstantiateResult aer_vm_instantiate_from_file(char* path, VM** out_vm, Chunk** out_chunk,
                                               unsigned int* out_halt_addr);

/* Pops arg_count arguments from vm's stack, runs fn in the module's own VM, pushes the result.
   False, stack untouched, if the name does not resolve. */
bool aer_module_call(VM* vm, const char* module, const char* fn, int arg_count);

/* For an embedding host tearing down; not called during normal execution. */
void aer_module_free_all(void);

#endif

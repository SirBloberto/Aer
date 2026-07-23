#ifndef AER_MODULE_H
#define AER_MODULE_H

#include "vm.h"

/* Loads, parses, and runs `path_name`.aer synchronously in its own Chunk+VM, registering it
   under `name` (call-only). Cached by name — a re-import is a no-op; circular imports error. */
bool aer_module_load(const char* name, unsigned int len,
                      const char* path_name, unsigned int path_len);

typedef enum { INSTANTIATE_OK, INSTANTIATE_PARSE_FAILED, INSTANTIATE_RUNTIME_FAILED } InstantiateResult;

/* Shared "make an independent VM+Chunk, load+compile+run its top-level code once" primitive --
   used by aer_module_load above and aer_actor_spawn (aer_actor.c). Out params are untouched on
   any failure; the partial VM/Chunk are already freed in that case. */
InstantiateResult aer_vm_instantiate_from_file(char* path, VM** out_vm, Chunk** out_chunk, unsigned int* out_halt_addr);

/* Invokes `fn` in file-module `module` with arg_count arguments already on the calling vm's stack — pops them, copies into the module's isolated VM, runs the call via a small trampoline, and pushes the result back. Returns false (stack untouched) if `module`/`fn` don't resolve. */
bool aer_module_call(VM* vm, const char* module, const char* fn, int arg_count);

/* GC root enumeration support (vm.c) — each file-module's VM/Chunk is permanent for the process's life (outside the collector's scope, see README's Memory and Security section) unless explicitly torn down via aer_module_free_all() below, so a mark phase must walk each one's roots too; returns false once `index` is past the last loaded module. */
bool aer_module_get(unsigned int index, VM** out_vm, Chunk** out_chunk);

/* Frees every loaded file-module's VM/Chunk and clears the registry — for an embedding host tearing down the process, not called during normal execution. */
void aer_module_free_all(void);

#endif

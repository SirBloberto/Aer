#ifndef AER_MODULE_H
#define AER_MODULE_H

#include "vm.h"

/* Resolves `path_name` (already using real '/' separators — a dotted import like `a.b` has its dots
   turned into '/' by the parser before this is called) to `path_name`.aer, either absolute or
   relative to the importing file's directory (falling back to AER_PATH), then reads/parses/runs it
   synchronously in its own Chunk and VM (never the caller's, keeping globals from colliding);
   registers it under `name` (see chunk_add_import — the bound identifier a program calls it by,
   independent of the path), caches by name (a second import is a no-op), and detects circular
   imports via a "currently loading" stack. Returns false on failure, having already called
   error_at()/error(). */
bool aer_module_load(const char* name, unsigned int len,
                      const char* path_name, unsigned int path_len);

/* Invokes `fn` in file-module `module` with arg_count arguments already on the calling vm's stack — pops them, copies into the module's isolated VM, runs the call via a small trampoline, and pushes the result back. Returns false (stack untouched) if `module`/`fn` don't resolve. */
bool aer_module_call(VM* vm, const char* module, const char* fn, int arg_count);

/* GC root enumeration support (vm.c) — each file-module's VM/Chunk is permanent for the process's life (outside the collector's scope, see README's Memory and Security section) unless explicitly torn down via aer_module_free_all() below, so a mark phase must walk each one's roots too; returns false once `index` is past the last loaded module. */
bool aer_module_get(unsigned int index, VM** out_vm, Chunk** out_chunk);

/* Frees every loaded file-module's VM/Chunk and clears the registry — for an embedding host tearing down the process, not called during normal execution. */
void aer_module_free_all(void);

#endif

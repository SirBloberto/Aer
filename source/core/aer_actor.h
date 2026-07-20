#ifndef AER_ACTOR_H
#define AER_ACTOR_H

#include "vm.h"

/* Actor-model groundwork: each Actor owns an independent, long-lived VM+Chunk (the same
   instantiation primitive aer_module_load uses for an import, aer_vm_instantiate_from_file), plus
   a plain host-side mailbox. This is deliberately NOT wired up to the AER language yet (no
   `import actor`, no scheduler) -- there's no way to suspend/resume a vm_run() mid-execution yet,
   so "receive" can't block the way real actor code would need it to. What's here is the
   foundation a future scheduler would sit on top of: spawning, calling a named function, and
   moving messages in and out, all driven by host (C) code today. */

typedef struct Actor Actor;

/* Loads path's top-level code into a fresh, independent VM once. NULL on a compile/runtime error. */
Actor* aer_actor_spawn(const char* path);

/* Calls a top-level function already defined in actor's own script -- the same trampoline shape
   aer_module_call uses for a file-module, just keyed by Actor* instead of a module-name lookup.
   Returns false if fn isn't defined, or if running it hit a runtime error (isolated to this
   actor -- it does not propagate to the caller). */
bool aer_actor_call(Actor* actor, const char* fn, int arg_count, AerVal* args, AerVal* out_result);

/* Mailbox: a plain host-side FIFO of byte strings, never a live AerVal -- a value from one
   actor's pools is meaningless in another's. Message content (e.g. JSON, via each side's own
   json.encode()/json.decode() calls) is entirely up to the AER code on each end; the mailbox
   itself only ever moves bytes. send() copies message; try_receive() hands back an owned buffer
   the caller must free(). */
bool aer_actor_send(Actor* actor, const char* message, unsigned int len);
bool aer_actor_try_receive(Actor* actor, char** out_message, unsigned int* out_len);

void aer_actor_free(Actor* actor);
void aer_actor_free_all(void);

/* GC root enumeration (vm.c) — mirrors aer_module_get: every actor's VM is a permanent root set
   for as long as it's alive, not just while its own vm_run() is on the stack, or a collection
   triggered by unrelated work elsewhere in the process could sweep an idle actor's still-live
   state. Returns false once `index` is past the last live actor. */
bool aer_actor_get(unsigned int index, VM** out_vm, Chunk** out_chunk);

#endif

#ifndef AER_ACTOR_H
#define AER_ACTOR_H

#include "vm.h"

/* Each Actor owns an independent, long-lived VM+Chunk (the same instantiation primitive
   aer_module_load uses for an import, aer_vm_instantiate_from_file), plus a plain host-side
   mailbox. Reachable from AER scripts via the `actor` module (aer_actor_module.c) and driven
   cooperatively by aer_scheduler.c/h, which calls aer_actor_prepare_call() then repeatedly
   vm_run_slice()s the actor's own VM instead of running a call to completion in one shot. */

typedef struct Actor Actor;

/* Loads path's top-level code into a fresh, independent VM once. NULL on a compile/runtime error. */
Actor* aer_actor_spawn(const char* path);

/* A stable, process-unique handle safe to hand to AER scripts as a plain integer (aer_actor_module.c).
   Never a raw pointer cast -- a script passing back a wrong/stale integer must get a clean "no such
   actor" error via aer_actor_find(), not a wild pointer dereference; the whole point of vm_run()
   never crashing the host applies just as much to a script's own mistakes here. */
unsigned int aer_actor_id(Actor* actor);
Actor* aer_actor_find(unsigned int id);

/* aer_actor_find() plus the type check every script-facing caller needs first -- shared by
   aer_actor_module.c and aer_scheduler_module.c so the "is this actually an integer handle" check
   lives in one place instead of two identical copies. NULL for anything that isn't a valid,
   currently-live actor handle. */
Actor* aer_actor_resolve(AerVal handle);

/* Calls a top-level function already defined in actor's own script -- the same trampoline shape
   aer_module_call uses for a file-module, just keyed by Actor* instead of a module-name lookup.
   Returns false if fn isn't defined, or if running it hit a runtime error (isolated to this
   actor -- it does not propagate to the caller). */
bool aer_actor_call(Actor* actor, const char* fn, int arg_count, AerVal* args, AerVal* out_result);

/* The shared first half of aer_actor_call -- resets the actor's VM for reuse, resolves fn, and
   calls setup_call() to prepare the frame without running anything yet. aer_actor_call uses this
   then runs to completion in one vm_run(); aer_scheduler.c uses this then drives the same VM in
   bounded vm_run_slice() calls instead, resuming across many scheduler rounds. False if fn isn't
   defined on actor's script. */
bool aer_actor_prepare_call(Actor* actor, const char* fn, int arg_count, AerVal* args);

/* Accessor for aer_scheduler.c, which drives actor->vm directly via vm_run_slice() -- Actor stays
   opaque everywhere else. */
VM* aer_actor_vm(Actor* actor);

/* Mailbox: a plain host-side FIFO of byte strings, never a live AerVal -- a value from one
   actor's pools is meaningless in another's. Message content (e.g. JSON, via each side's own
   json.encode()/json.decode() calls) is entirely up to the AER code on each end; the mailbox
   itself only ever moves bytes. send() copies message; try_receive() hands back an owned buffer
   the caller must free(). */
bool aer_actor_send(Actor* actor, const char* message, unsigned int len);
bool aer_actor_try_receive(Actor* actor, char** out_message, unsigned int* out_len);

void aer_actor_free(Actor* actor);
void aer_actor_free_all(void);

#endif

#ifndef AER_ACTOR_H
#define AER_ACTOR_H

#include "vm.h"

/* Each Actor owns an independent, long-lived VM+Chunk plus a plain host-side mailbox. Reached from
   scripts via the `actor` module at the end of aer_actor.c, and driven by aer_scheduler.c -- the
   only other file needing any of this. Everything the scheduler does not need is static there. */

typedef struct Actor Actor;

/* aer_actor_find() plus the type check a script-facing caller needs first -- NULL for anything
   that isn't a valid, currently-live actor handle. */
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

/* Rebuilds a value from an actor's heap in whichever heap is current. */
AerVal aer_actor_copy_result(AerVal v);

/* Frees every actor still alive, each one's whole VM+Chunk with it. The actor-side counterpart to
   aer_module_free_all() (aer_module.h), and called next to it -- an embedding host that spawns
   actors needs both to tear down cleanly, not just the module half. */
void aer_actor_free_all(void);

#endif

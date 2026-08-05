#ifndef AER_SCHEDULER_H
#define AER_SCHEDULER_H

#include "aer_actor.h"

/* Cooperative round-robin scheduler over already-spawned actors (aer_actor.h). Single OS thread,
   never true parallelism -- matches the process-global GC pools' existing constraint (see
   aer_actor.h's own comment). Each task is one function call on one actor, driven in bounded
   instruction slices via vm_run_slice (vm.h) instead of to completion, so a long-running task
   can't starve the others; interleaving comes from visiting every unfinished task once per round,
   not from anything opcode-level knowing about "other actors." */

/* Registers fn(args...) as a task to run on actor under the scheduler. False if fn isn't defined on
   actor's script. Must be called before aer_scheduler_run() -- a task can't be added mid-run. */
bool aer_scheduler_add(Actor* actor, const char* fn, int arg_count, AerVal* args);

/* Runs every added task to completion (or to a runtime error, isolated per task the same way
   aer_actor_call already isolates one) before returning. Blocks the calling thread for as long as
   any task takes -- there's no way to observe partial progress from outside; a task that never
   finishes (e.g. a genuine infinite loop) means this never returns, the same as any other infinite
   loop in AER already behaves. Clears the task list on return either way. */
void aer_scheduler_run(void);

#endif

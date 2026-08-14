#include <stdlib.h>
#include "aer_actor.h"
#include "aer_module.h"
#include "aer_stdlib.h"
#include "error.h"

/* Cooperative round-robin scheduler over already-spawned actors (aer_actor.h). Single OS thread,
   never true parallelism -- matches the process-global GC pools' existing constraint (see
   aer_actor.h's own comment). Each task is one function call on one actor, driven in bounded
   instruction slices via vm_run_slice (vm.h) instead of to completion, so a long-running task
   can't starve the others; interleaving comes from visiting every unfinished task once per round,
   not from anything opcode-level knowing about "other actors." */

/* Every yield-checkpoint in vm_run_slice (vm.c's lbl_jump/lbl_call/lbl_iter_range_loop) only ever
   fires at a genuine instruction boundary, so resuming a task is always just "call vm_run_slice
   again" -- no separate suspend/resume state to track here beyond "has this task's call already
   been set up." */
typedef struct Task {
    Actor* actor;
    bool finished;
    struct Task* next;
} Task;

static Task* tasks = NULL;
static Task* tasks_tail = NULL;

/* Small enough that no single task can visibly starve the others for long, large enough that a
   round-robin pass isn't dominated by scheduling overhead. Not user-configurable in this pass --
   see the plan's note on the scheduler being a first cut, not a tuned production scheduler. */
#define SCHEDULER_SLICE_INSTRUCTIONS 1000

/* Registers fn(args...) as a task to run on actor under the scheduler. False if fn isn't defined
   on actor's script. Must be called before aer_scheduler_run() -- a task can't be added mid-run. */
static bool aer_scheduler_add(Actor* actor, const char* fn, int arg_count, AerVal* args) {
    if (!aer_actor_prepare_call(actor, fn, arg_count, args))
        return false;

    Task* t = xmalloc(sizeof(Task));
    t->actor = actor;
    t->finished = false;
    t->next = NULL;
    if (tasks_tail)
        tasks_tail->next = t;
    else
        tasks = t;
    tasks_tail = t;
    return true;
}

/* Runs every added task to completion (or to a runtime error, isolated per task the same way
   aer_actor_call already isolates one) before returning. Blocks the calling thread for as long as
   any task takes -- there's no way to observe partial progress from outside; a task that never
   finishes (e.g. a genuine infinite loop) means this never returns, the same as any other infinite
   loop in AER already behaves. Clears the task list on return either way. */
static void aer_scheduler_run(void) {
    bool any_unfinished = true;
    while (any_unfinished) {
        any_unfinished = false;
        for (Task* t = tasks; t; t = t->next) {
            if (t->finished)
                continue;
            any_unfinished = true;

            vm_gc_suppress();
            VmSliceResult r = vm_run_slice(aer_actor_vm(t->actor), SCHEDULER_SLICE_INSTRUCTIONS);
            vm_gc_unsuppress();

            if (r != VM_SLICE_YIELDED) {
                t->finished = true;
                /* Isolation, same reasoning and same two flags as aer_actor_call's own reset --
                   one task's error must not poison every other task's (or the caller's) exit
                   status, and runtime_had_error alone isn't enough (see aer_actor.c). */
                runtime_had_error = false;
                parse_had_error = false;
            }
        }
    }

    while (tasks) {
        Task* next = tasks->next;
        free(tasks);
        tasks = next;
    }
    tasks_tail = NULL;
}

/* ------------------------------------------------------------------ */
/* Script-facing `scheduler` module                                     */
/* ------------------------------------------------------------------ */

/* noinline -- see aer_host_call's own comment (aer_host.c): same 4KB VM_STACK_MAX-array,
   single-call-site shape that was inflating vm_run_slice's stack frame via LTO. */
__attribute__((noinline)) bool aer_scheduler_module_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_SCHEDULER_ADD && arg_count >= 2) {
        AerVal popped[VM_STACK_MAX];
        for (int i = arg_count - 1; i >= 0; i--)
            popped[i] = vm_stack_pop(vm);

        if (aer_type(popped[1]) != TYPE_STRING) {
            error("scheduler.add() requires an actor handle and a function-name string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        Actor* a = aer_actor_resolve(popped[0]);
        if (!a) {
            error("scheduler.add(): no actor with that handle");
            vm_stack_push(vm, aer_null());
            return true;
        }

        bool ok = aer_scheduler_add(a, aer_as_string(popped[1])->data, arg_count - 2, &popped[2]);
        if (!ok) {
            error("scheduler.add(): '%s' is not defined on that actor's script",
                  aer_as_string(popped[1])->data);
            vm_stack_push(vm, aer_null());
            return true;
        }
        vm_stack_push(vm, aer_null());
        return true;
    }

    if (fn_id == FN_SCHEDULER_RUN && arg_count == 0) {
        aer_scheduler_run();
        vm_stack_push(vm, aer_null());
        return true;
    }

    return false;
}

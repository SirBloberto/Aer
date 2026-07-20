#include <stdlib.h>
#include "aer_scheduler.h"
#include "aer_module.h"
#include "error.h"

/* Every yield-checkpoint in vm_run_slice (vm.c's lbl_jump/lbl_call/lbl_iter_range_loop) only ever
   fires at a genuine instruction boundary, so resuming a task is always just "call vm_run_slice
   again" -- no separate suspend/resume state to track here beyond "has this task's call already
   been set up." */
typedef struct Task {
    Actor* actor;
    bool   finished;
    struct Task* next;
} Task;

static Task* tasks      = NULL;
static Task* tasks_tail = NULL;

/* Small enough that no single task can visibly starve the others for long, large enough that a
   round-robin pass isn't dominated by scheduling overhead. Not user-configurable in this pass --
   see the plan's note on the scheduler being a first cut, not a tuned production scheduler. */
#define SCHEDULER_SLICE_INSTRUCTIONS 1000

bool aer_scheduler_add(Actor* actor, const char* fn, int arg_count, AerVal* args) {
    if (!aer_actor_prepare_call(actor, fn, arg_count, args)) return false;

    Task* t = xmalloc(sizeof(Task));
    t->actor    = actor;
    t->finished = false;
    t->next     = NULL;
    if (tasks_tail) tasks_tail->next = t;
    else            tasks = t;
    tasks_tail = t;
    return true;
}

void aer_scheduler_run(void) {
    bool any_unfinished = true;
    while (any_unfinished) {
        any_unfinished = false;
        for (Task* t = tasks; t; t = t->next) {
            if (t->finished) continue;
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
                parse_had_error   = false;
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

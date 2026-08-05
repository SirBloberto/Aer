#include <stdlib.h>
#include "aer_scheduler.h"
#include "aer_module.h"
#include "aer_stdlib.h"
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

/* ------------------------------------------------------------------ */
/* Script-facing `scheduler` module                                     */
/* ------------------------------------------------------------------ */


/* noinline -- see aer_host_call's own comment (aer_host.c): same 4KB VM_STACK_MAX-array,
   single-call-site shape that was inflating vm_run_slice's stack frame via LTO. */
__attribute__((noinline))
bool aer_scheduler_module_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_SCHEDULER_ADD && arg_count >= 2) {
        AerVal popped[VM_STACK_MAX];
        for (int i = arg_count - 1; i >= 0; i--) popped[i] = vm_stack_pop(vm);

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
            error("scheduler.add(): '%s' is not defined on that actor's script", aer_as_string(popped[1])->data);
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

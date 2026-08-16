#include <stdlib.h>
#include "aer_actor.h"
#include "aer_thread.h"
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
    AerVal result;
    struct Task* next;
} Task;

static Task* tasks = NULL;
static Task* tasks_tail = NULL;

/* Registers fn(args...) as a task to run on actor under the scheduler. False if fn isn't defined
   on actor's script. Must be called before aer_scheduler_run() -- a task can't be added mid-run. */
static bool aer_scheduler_add(Actor* actor, const char* fn, int arg_count, AerVal* args) {
    if (!aer_actor_prepare_call(actor, fn, arg_count, args))
        return false;

    Task* t = xmalloc(sizeof(Task));
    t->actor = actor;
    t->finished = false;
    t->result = aer_null();
    t->next = NULL;
    if (tasks_tail)
        tasks_tail->next = t;
    else
        tasks = t;
    tasks_tail = t;
    return true;
}

/* Two shapes, because "concurrent" means different things per build: with threads a worker owns a
   task and runs it unbudgeted, without them tasks must take turns to interleave at all. Actors
   share nothing a worker reaches, so the lock covers only handing out the next task. */
#define SCHEDULER_SLICE_INSTRUCTIONS 1000

static void scheduler_finish(Task* t) {
    /* From the actor's frame while it is still the one that ran, exactly where aer_actor_call
       looks. Rebuilt in the caller's heap later, once every task is done. */
    t->result = aer_actor_vm(t->actor)->call_stack[0].registers[0];
    t->finished = true;
    /* Isolation, the same two flags aer_actor_call resets: one task's failure must not be read as
       another's, and both are per-thread in a threaded build. */
    runtime_had_error = false;
    parse_had_error = false;
}

#ifdef AER_HEAP_REF_TLS
static aer_mutex task_lock;
static Task* next_task = NULL;

static Task* scheduler_take_task(void) {
    aer_mutex_lock(&task_lock);
    Task* t = next_task;
    if (t)
        next_task = t->next;
    aer_mutex_unlock(&task_lock);
    return t;
}

static void* scheduler_worker(void* unused) {
    (void)unused;
    for (Task* t = scheduler_take_task(); t; t = scheduler_take_task()) {
        vm_run_slice(aer_actor_vm(t->actor), 0);
        scheduler_finish(t);
    }
    return NULL;
}
#endif

/* Round-robin in bounded slices, so no task can starve the others. */
static void scheduler_run_interleaved(void) {
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
            if (r != VM_SLICE_YIELDED)
                scheduler_finish(t);
        }
    }
}

/* Every task's return value, in the order they were added -- work split across actors is only
   useful if its answers come back. Each is rebuilt in the caller's heap, since it was produced in
   the actor's. */
static AerVal aer_scheduler_run_collect(void) {
#ifdef AER_HEAP_REF_TLS
    unsigned int count = 0;
    for (Task* t = tasks; t; t = t->next)
        count++;
    unsigned int workers = aer_thread_hardware_workers();
    if (workers > count)
        workers = count;
    /* One task needs no thread; spawning one would only add the handoff. */
    if (workers > 1) {
        aer_mutex_init(&task_lock);
        next_task = tasks;
        aer_thread* threads = xmalloc(sizeof(aer_thread) * workers);
        unsigned int started = 0;
        for (unsigned int i = 0; i < workers; i++)
            if (aer_thread_start(&threads[started], scheduler_worker, NULL) == 0)
                started++;
        /* This thread takes tasks too, which also covers any that failed to start. */
        scheduler_worker(NULL);
        for (unsigned int i = 0; i < started; i++)
            aer_thread_join(threads[i]);
        free(threads);
        aer_mutex_destroy(&task_lock);
        next_task = NULL;
    } else
#endif
        scheduler_run_interleaved();

    unsigned int n = 0;
    for (Task* t = tasks; t; t = t->next)
        n++;
    AerArray* out = vm_new_array();
    out->count = n;
    out->capacity = n ? n : 4;
    out->items = xmalloc(sizeof(AerVal) * out->capacity);
    out->shape = NULL;
    out->generation = 0;
    out->dirty_cards = NULL;
    out->dirty_cards_bytes = 0;
    out->dirty_min_byte = (unsigned int)-1;
    out->dirty_max_byte = 0;
    out->dirty_all = false;

    unsigned int i = 0;
    while (tasks) {
        Task* next = tasks->next;
        out->items[i++] = aer_actor_copy_result(tasks->result);
        free(tasks);
        tasks = next;
    }
    tasks_tail = NULL;
    return aer_array_val(out);
}

/* Script-facing `scheduler` module                                     */

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
        vm_stack_push(vm, aer_scheduler_run_collect());
        return true;
    }

    return false;
}

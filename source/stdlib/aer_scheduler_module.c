#include "aer_scheduler.h"
#include "aer_stdlib.h"
#include "error.h"

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

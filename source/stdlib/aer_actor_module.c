#include <string.h>
#include "aer_actor.h"
#include "aer_stdlib.h"
#include "error.h"

/* Script-facing surface over aer_actor.c's host-only primitives -- spawn/send/receive/call, same
   fixed CALL_MODULE_* dispatch shape as every other stdlib module. Adds no concurrency of its own:
   every call here still runs to completion before the caller's next line, exactly like
   aer_actor_call already does from host C code. Its purpose is giving actors a name AER scripts can
   reach, so the scheduler (aer_scheduler.c) has a real language feature to schedule instead of only
   a hand-written C test driver. */

bool aer_actor_module_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_ACTOR_SPAWN && arg_count == 1) {
        AerVal path_v = vm_stack_pop(vm);
        if (aer_type(path_v) != TYPE_STRING) {
            error("actor.spawn() requires a path string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        Actor* a = aer_actor_spawn(aer_as_string(path_v)->data);
        if (!a) {
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error("actor.spawn(): failed to load or run the script")));
            return true;
        }
        vm_stack_push(vm, aer_make_result(aer_int((int64_t)aer_actor_id(a)), aer_null()));
        return true;
    }

    if (fn_id == FN_ACTOR_SEND && arg_count == 2) {
        AerVal message_v = vm_stack_pop(vm);
        AerVal handle_v  = vm_stack_pop(vm);
        if (aer_type(message_v) != TYPE_STRING) {
            error("actor.send() requires an actor handle and a string message");
            vm_stack_push(vm, aer_null());
            return true;
        }
        Actor* a = aer_actor_resolve(handle_v);
        if (!a) {
            error("actor.send(): no actor with that handle");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerString* str = aer_as_string(message_v);
        aer_actor_send(a, str->data, str->length);
        vm_stack_push(vm, aer_null());
        return true;
    }

    if (fn_id == FN_ACTOR_RECEIVE && arg_count == 1) {
        AerVal handle_v = vm_stack_pop(vm);
        Actor* a = aer_actor_resolve(handle_v);
        if (!a) {
            error("actor.receive(): no actor with that handle");
            vm_stack_push(vm, aer_null());
            return true;
        }
        char* message; unsigned int len;
        /* No message ready is a normal, non-error outcome -- plain null, matching this language's
           existing "missing dict key returns null" idiom, not a Result. */
        if (!aer_actor_try_receive(a, &message, &len)) {
            vm_stack_push(vm, aer_null());
            return true;
        }
        vm_stack_push(vm, aer_make_string(message, len));
        return true;
    }

    if (fn_id == FN_ACTOR_CALL && arg_count >= 2) {
        /* Popped in reverse (LIFO) order, same shape as aer_host_call's own arg-copy -- popped[0]
           ends up as the first-pushed (handle), popped[arg_count-1] as the last extra argument. */
        AerVal popped[VM_STACK_MAX];
        for (int i = arg_count - 1; i >= 0; i--) popped[i] = vm_stack_pop(vm);

        if (aer_type(popped[1]) != TYPE_STRING) {
            error("actor.call() requires an actor handle and a function-name string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        Actor* a = aer_actor_resolve(popped[0]);
        if (!a) {
            error("actor.call(): no actor with that handle");
            vm_stack_push(vm, aer_null());
            return true;
        }

        AerVal result;
        bool ok = aer_actor_call(a, aer_as_string(popped[1])->data, arg_count - 2, &popped[2], &result);
        if (!ok) {
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error("actor.call(): function not found or the call failed")));
            return true;
        }
        vm_stack_push(vm, aer_make_result(result, aer_null()));
        return true;
    }

    return false;
}

#include <string.h>
#include "aer_host.h"
#include "error.h"
#include "value_box.h"

/* aer_val_to_public/aer_val_from_public (value_box.h) are the one seam between internal packed AerVal and the public boxed Value AerNativeFn has always used — converted once per host call, so the VM's hot dispatch loop never pays this cost for scripts that don't call a host function. */

typedef struct {
    char*       module;
    char*       name;
    AerNativeFn fn;
    void*       userdata;
} HostFunction;

static HostFunction host_functions[MAX_HOST_FUNCTIONS];
static int          host_function_count = 0;

bool aer_register_function(const char* module, const char* name, AerNativeFn fn, void* userdata) {
    if (host_function_count >= MAX_HOST_FUNCTIONS) return false;
    HostFunction* h = &host_functions[host_function_count++];
    h->module   = xstrdup(module);
    h->name     = xstrdup(name);
    h->fn       = fn;
    h->userdata = userdata;
    return true;
}

bool aer_host_is_module(const char* name, unsigned int len) {
    for (int i = 0; i < host_function_count; i++)
        if (strlen(host_functions[i].module) == len && strncmp(host_functions[i].module, name, len) == 0)
            return true;
    return false;
}

bool aer_host_call(VM* vm, const char* module, const char* fn_name, int arg_count) {
    HostFunction* h = NULL;
    for (int i = 0; i < host_function_count; i++) {
        if (strcmp(host_functions[i].module, module) != 0) continue;
        if (strcmp(host_functions[i].name, fn_name) != 0) continue;
        h = &host_functions[i];
        break;
    }
    if (!h) return false;

    if (vm->stack_top < arg_count) { error("Stack underflow"); return true; }
    AerVal* args = &vm->stack[vm->stack_top - arg_count];

    /* Converts to the public Value ABI for this one call (arg_count <= vm->stack_top <= VM_STACK_MAX keeps it in bounds) — a copy, not a live pointer into the VM's stack (see aer_host.h). */
    Value boxed_args[VM_STACK_MAX];
    for (int i = 0; i < arg_count; i++) boxed_args[i] = aer_val_to_public(args[i]);

    Value ret = h->fn(vm, arg_count, boxed_args, h->userdata);
    vm->stack_top -= arg_count;

    if (vm->stack_top >= VM_STACK_MAX) { error("Stack overflow"); return true; }
    vm->stack[vm->stack_top++] = aer_val_from_public(ret);
    return true;
}

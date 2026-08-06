#include <string.h>
#include "aer_host.h"
#include "error.h"

typedef struct {
    char* module;
    char* name;
    AerNativeFn fn;
    void* userdata;
} HostFunction;

static HostFunction host_functions[MAX_HOST_FUNCTIONS];
static int host_function_count = 0;

bool aer_register_function(const char* module, const char* name, AerNativeFn fn, void* userdata) {
    if (host_function_count >= MAX_HOST_FUNCTIONS) return false;
    HostFunction* h = &host_functions[host_function_count++];
    h->module = xstrdup(module);
    h->name = xstrdup(name);
    h->fn = fn;
    h->userdata = userdata;
    return true;
}

bool aer_host_is_module(const char* name, unsigned int len) {
    for (int i = 0; i < host_function_count; i++)
        if (strlen(host_functions[i].module) == len && strncmp(host_functions[i].module, name, len) == 0)
            return true;
    return false;
}

/* noinline: with one call site, LTO would fold this function's 4KB AerVal args[VM_STACK_MAX] frame
   into vm_run_slice's -- measured taking that frame from ~7KB to ~30KB once all three
   VM_STACK_MAX-array module-call functions were pulled in. Keeping a real call trades one cheap
   `bl` on a path that is not hot for a dispatch loop whose frame still fits L1: same instructions,
   ~9-10% fewer cycles. */
__attribute__((noinline)) bool aer_host_call(VM* vm, const char* module, const char* fn_name, int arg_count) {
    HostFunction* h = NULL;
    for (int i = 0; i < host_function_count; i++) {
        if (strcmp(host_functions[i].module, module) != 0) continue;
        if (strcmp(host_functions[i].name, fn_name) != 0) continue;
        h = &host_functions[i];
        break;
    }
    if (!h) return false;

    if (vm->stack_top < arg_count) {
        error("Stack underflow");
        return true;
    }

    /* A copy, not a live pointer into the VM's stack (see aer.h's AerNativeFn contract) --
       arg_count <= vm->stack_top <= VM_STACK_MAX keeps it in bounds. */
    AerVal args[VM_STACK_MAX];
    memcpy(args, &vm->stack[vm->stack_top - arg_count], sizeof(AerVal) * (size_t)arg_count);

    AerVal ret = h->fn(vm, arg_count, args, h->userdata);
    vm->stack_top -= arg_count;

    if (vm->stack_top >= VM_STACK_MAX) {
        error("Stack overflow");
        return true;
    }
    vm->stack[vm->stack_top++] = ret;
    return true;
}

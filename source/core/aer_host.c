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

/* noinline: this is the sole call site (lbl_call_module, vm.c), so LTO would otherwise fold this
   function's own 4KB `AerVal args[VM_STACK_MAX]` frame directly into vm_run_slice's -- measured on
   the Pi to blow that function's stack frame from ~7KB up to ~30KB once this and the two other
   VM_STACK_MAX-array module-call functions (aer_actor_module_call, aer_scheduler_module_call) all
   got pulled in alongside it. Keeping this a real call trades one cheap `bl` (this path already
   does a linear host-function scan and a memcpy, not remotely hot) for a dispatch loop whose own
   frame stays small enough to not crowd out L1: same instruction count, ~9-10% fewer cycles on
   nbody_large_packed_narrow.aer across 8 runs each side. */
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

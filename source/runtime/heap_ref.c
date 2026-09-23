#include "vm.h"

/* Which VM's heap is active, for allocations with no VM in scope. Its own TU behind noinline
   accessors, or _Thread_local reshapes vm_run_slice's register allocation (ARCHITECTURE 5.62). */
#ifdef AER_HEAP_REF_TLS
#define HEAP_REF_STORAGE _Thread_local
#else
#define HEAP_REF_STORAGE
#endif

static HEAP_REF_STORAGE VmHeap* current_heap = NULL;

/* For a host that configures the GC before creating any VM. */
static VmHeap bootstrap_heap = {0};

__attribute__((noinline)) VmHeap* vm_require_current_heap(void) {
    if (!current_heap)
        current_heap = &bootstrap_heap;
    return current_heap;
}

__attribute__((noinline)) VmHeap* vm_current_heap(void) {
    return current_heap;
}

__attribute__((noinline)) void vm_set_current_heap(VmHeap* h) {
    current_heap = h;
}

/* The VM currently dispatching, for error line lookup. Same TU and noinline, same reason. A nested
   compile+run saves and restores it around the whole cycle, or a failed one leaves it on a freed VM. */
static HEAP_REF_STORAGE VM* active_error_vm = NULL;

__attribute__((noinline)) VM* vm_active_error_vm(void) {
    return active_error_vm;
}

__attribute__((noinline)) void vm_set_active_error_vm(VM* vm) {
    active_error_vm = vm;
}

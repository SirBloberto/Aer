#include "vm.h"

/* Which VM's heap is active. It lives in its own translation unit, behind noinline accessors, for
   one measured reason: vm_run_slice is a single enormous function, and making this variable opaque
   inside it (which _Thread_local does) shifted register allocation across the whole thing -- 6.68%
   on fib_bench, a benchmark that never allocates, while allocation-heavy ones were unmoved. An
   opaque call is something that function already deals with everywhere; an opaque variable is not.
   -flto would inline these straight back into vm.c and undo that, hence noinline.

   It exists at all so an allocation with no VM* in scope (the lexer, and the parts of the parser
   that build pooled values before a Chunk's VM exists) still reaches the right heap. */
#ifdef AER_HEAP_REF_TLS
#define HEAP_REF_STORAGE _Thread_local
#else
#define HEAP_REF_STORAGE
#endif

static HEAP_REF_STORAGE VmHeap* current_heap = NULL;

/* For the one case with no VM anywhere yet -- a host calling aer_gc_configure() before creating
   one. Promoted lazily so nothing dereferences NULL. */
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

#ifndef VM_H
#define VM_H

#include "chunk.h"
#include "encoding.h"
#include "error.h"
#include "hashtable.h"
#include "heap.h"
#include "objects.h"
#include "opcodes.h"
#include "value.h"
#include "value_format.h"

/* Longest interpolated dict key OP_INDEX_GET_INTERP will build without allocating. Anything longer
   falls back to the allocating path -- the buffer lives in a noinline helper's frame, never
   vm_run_slice's (see 5.18). */
#define INTERP_KEY_MAX 256


/* Virtual machine                                                      */

#define VM_STACK_MAX 256
/* Non-tail call depth. The call stack and register bank grow on demand, about 2KB a frame, so this
   bounds what a runaway recursion can take rather than what every VM reserves. */
#define VM_CALL_MAX 10000

/* Per-call register frame; each VM owns its own chain, so a nested module VM gets its own. */
typedef struct {
    /* The anonymous union pads sizeof(CallFrame) up to 32 without raising its ALIGNMENT. _Alignas(32)
       was the obvious way to get that size and is wrong: the requirement propagates to struct VM,
       which aer_module.c -- and any embedder -- allocates with plain malloc, only 8-byte aligned.
       Every module VM was undefined behaviour; UBSan caught it. Only the size matters here. */
    union {
        struct {
            /* Bump-pointer base into vm->register_stack -- a call is a pointer add, not an
               allocation. */
            AerVal* registers;
            /* Registers THIS frame reserved (callee's compile-time peak; FRAME_REGISTERS for frame
               0) -- read by the next push. */
            unsigned int frame_size;

            unsigned int return_ip; /* where to resume in the CALLER */
            int dest_reg; /* which of the CALLER's registers gets the return value */

            unsigned int code_offset; /* this frame's entry point, for stack traces; unset on frame 0 */
            unsigned int tail_calls_collapsed; /* tail calls collapsed since this frame's last real push */
            bool synthetic_entry; /* set by setup_call() -- return_ip isn't a real caller line */
            unsigned short frame_bounds; /* FRAME_BOUNDS, above; fits the padding CallFrame had */
        };
        char size_is_a_power_of_two[32];
    };
} CallFrame;

/* Tags the slots a frame about to start running will actually use: the dynamically typed ones hold
   nothing yet, and the real ones at the top get the tag their unchecked opcodes then rely on --
   those store a payload and leave the tag exactly as this wrote it. The gap between the two is
   skipped, and frame_ref_slots stops before it, so a full-size frame costs no more here than the
   tight one it replaced. */
static inline void frame_init_tags(AerVal* registers, unsigned int from, unsigned short bounds) {
    for (unsigned int i = from, dyn_end = FRAME_DYN_END(bounds); i < dyn_end; i++)
        registers[i].tag = TYPE_NULL;
    /* Runs to FRAME_REGISTERS, not to the frame size, because only a frame that IS full size has
       real slots -- for any other, real_base is already FRAME_REGISTERS and this does nothing. That
       keeps the frame size off the call path entirely. */
    for (unsigned int i = FRAME_REAL_BASE(bounds); i < FRAME_REGISTERS; i++)
        registers[i].tag = TYPE_REAL;
}

/* The slots in this frame a heap reference can live in, for the GC to trace. Every slot is a tagged
   AerVal whatever its static type, so this stops only where nothing can reach: at the frame's
   dynamically typed peak, below both the untouched gap and the real slots above it. */
static inline unsigned int frame_ref_slots(CallFrame* frame, AerVal** out_slots) {
    unsigned int dyn_end = FRAME_DYN_END(frame->frame_bounds);
    *out_slots = frame->registers;
    /* Clamped, not trusted: a chunk built by hand rather than by the parser (the embedding tests do
       this) carries the placeholder bounds, which are wider than its frame. */
    return dyn_end < frame->frame_size ? dyn_end : frame->frame_size;
}
/* Indexing call_stack[] is `base + depth * sizeof(CallFrame)`, and at 44 bytes that compiled to a
   multiply plus a materialized constant per field on 32-bit ARM. A power of two makes it a shift. */
_Static_assert((sizeof(CallFrame) & (sizeof(CallFrame) - 1)) == 0,
               "CallFrame must stay a power of two -- see CALL_FRAME_PAD");

typedef struct VM {
    /* First, with call_stack beside it. A Thumb-2 `ldr` reaches a 12-bit displacement, so a field
       past 4095 bytes needs its offset materialized into a register first. Nothing mirrors the
       active frame's registers here: mark_vm_roots scans call_stack[f].registers directly, so a
       mirror would cost three stores per call and per return to serve no hot reader. */
    int call_depth;
    /* Frames the call stack can currently seat -- always <= VM_CALL_MAX. It stands in for the constant
       in the depth test every push already makes, and the cold side decides grow-or-overflow. */
    int call_depth_limit;
    CallFrame* call_stack;
    /* The highest base a new frame may start at: FRAME_REGISTERS short of the bank's end, the most any
       frame's tagging touches whatever its bounds say. A tail call reuses a base that already passed
       this test and the bank only grows, so pushes are the only thing that checks it. */
    AerVal* push_base_limit;

    Chunk* chunk;
    unsigned int ip;
    /* The yield budget, read and decremented only at the handful of checkpoints that can yield.
       Held here rather than in vm_run_slice locals so a handler reaches it through vm alone. */
    unsigned int slice_budget;
    unsigned int slice_max; /* 0 disables yielding entirely */

    /* For runtime error line lookup only. A pointer, not an offset: vm_run_slice writes it at
       every site that can raise, and an offset would keep `code` live across all of them. `ip`
       stays an offset because it must survive a yield/resume and a REPL reparse in between. */
    const uint32_t* error_pc;

    /* This VM's own heap -- every pool it allocates from, independent of every other VM's. */
    VmHeap heap;

    /* Seeded from the process-wide defaults at vm_init and at every aer_run_source call, so the
       documented "toggle off, run one thing, toggle back" pattern works on an existing VM. */
    bool io_enabled;
    bool net_enabled;

    /* Scratch argument channel for bridging out of the register convention (stdlib/module calls).
       Deliberately after every field the call path reads: its 4096 bytes in front of them would push
       their offsets past the 12-bit displacement window. Nothing in the call path touches it. */
    AerVal stack[VM_STACK_MAX];
    int stack_top;

    /* One value this VM holds onto between calls (actor.keep/actor.kept). Functions cannot reach a
       top-level variable, so without this an actor has nowhere to put data it wants to reuse and
       every call has to be handed it again -- which for a column meant copying it across the heap
       boundary every time. A GC root, and it outlives aer_vm_reset_for_reuse deliberately. */
    AerVal kept;

    /* One shared register bank for the whole chain (calls bump a base pointer). Grown on demand
       rather than reserved: inline at full size this was 93% of sizeof(VM), and every actor and
       every imported module allocates a VM. */
    AerVal* register_stack;
    size_t register_capacity; /* slots in register_stack */
} VM;

/* Frames the call stack seats, and full-size frames the bank holds, before either first grows. */
#define REGISTER_STACK_INITIAL_FRAMES 2

/* Bounds-checked push/pop for native-module files, outside vm_run's PUSH()/POP() macros. */
static inline bool vm_stack_push(VM* vm, AerVal v) {
    if (vm->stack_top >= VM_STACK_MAX) {
        error("Stack overflow");
        return false;
    }
    vm->stack[vm->stack_top++] = v;
    return true;
}

static inline AerVal vm_stack_pop(VM* vm) {
    if (vm->stack_top <= 0) {
        error("Stack underflow");
        return aer_null();
    }
    return vm->stack[--vm->stack_top];
}


/* Process-wide capability defaults, all true. io/net are defaults only: vm_init copies them into
   VM.io_enabled/net_enabled, and the runtime checks the per-VM field, so two VMs in one process can
   differ. import_enabled stays a directly-checked global because chunk_add_import runs at parse
   time with only a Chunk* in scope. Set via aer_set_*_enabled(). A blast-radius limiter, not a
   permission system -- see the README's Sandboxing note. */
extern bool aer_io_enabled;
extern bool aer_net_enabled;

void vm_init(VM* vm, Chunk* chunk);
void vm_free(VM* vm);

/* Same save/restore need as vm_current_heap, for the file-scope active_vm_for_errors global (vm.c)
   -- vm_init() unconditionally repoints it at the new VM before that VM ever runs. */
VM* vm_active_error_vm(void);
void vm_set_active_error_vm(VM* vm);

/* Runs from vm->ip to OP_HALT or runtime error (returns false). A host reusing the VM after
   a false return must reset stack_top/call_depth first -- see main.c's run(). */
bool vm_run(VM* vm);

typedef enum {
    VM_SLICE_DONE, /* reached OP_HALT */
    VM_SLICE_YIELDED, /* budget reached at a back-edge or call; vm->ip is a valid resume point */
    VM_SLICE_ERROR, /* runtime error, same as vm_run's false */
} VmSliceResult;

/* 0 = unlimited, which is what every caller but the scheduler passes. A nonzero budget counts loop
   back-edges and calls, then returns VM_SLICE_YIELDED with vm->ip at a valid resume point; calling
   again continues from there. */
VmSliceResult vm_run_slice(VM* vm, unsigned int max_instructions);

/* Cross-module call setup (aer_module_call only): pushes a real frame with dest_reg fixed at
   0 -- after the trampoline drains, the result is in call_stack[0].registers[0]. */
bool setup_call(VM* target, ChunkFunction* fn, int arg_count, AerVal* args, unsigned int return_ip);

/* Every store into an already-existing array goes through this. `index` feeds card marking, so a
   minor GC rescans only the slots dirtied since the last cycle. A freshly built, not-yet-returned
   array needs no barrier. */
void gc_barrier_array(VM* vm, AerArray* a, unsigned int index, AerVal new_value);

/* Same, for a struct field-set. No index: a struct's field count is small and fixed, so there is
   nothing for card marking to save. */
void gc_barrier_struct(VM* vm, AerStruct* s, AerVal new_value);

/* Same, for a dict entry. `index` is the DENSE index, which the caller resolves before the write. */
void gc_barrier_dict(VM* vm, AerDict* d, unsigned int index, AerVal new_value);

/* Called from gc_maybe_collect (vm_internal.h) once the threshold is actually crossed. gc_maybe_collect runs
   at hand-placed points in the allocating opcodes, not on every dispatch. */
void gc_run_collection_cycle(VM* vm);

/* Test-only register readback (tests/smoke_test.c). */
AerVal register_get(VM* vm, int slot);



/* Turns on the per-instruction counters DISPATCH() feeds. Must be called before the chunk runs;
   a run started without it never allocates the counters and never tests anything but a register. */
void aer_profile_enable(void);
bool aer_profile_is_enabled(void);

#endif

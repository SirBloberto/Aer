#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "aer_actor.h"
#include "aer_thread.h"
#include "aer_module.h"
#include "aer_stdlib.h"
#include "error.h"

typedef struct Mailbox {
    char* data;
    unsigned int len;
    struct Mailbox* next;
} Mailbox;

struct Actor {
    VM* vm;
    Chunk* chunk;
    unsigned int halt_addr;
    unsigned int id;
    /* The one part of an actor another thread touches: everything else is reached only by whichever
       worker is running this actor's task. */
    Mailbox* mailbox_head;
    Mailbox* mailbox_tail;
    aer_mutex mailbox_lock;
    struct Actor* next; /* process-wide registry, for GC root enumeration and free_all */
};

static Actor* actors = NULL;
static unsigned int next_actor_id = 1; /* 0 reserved as "no such actor" */

/* Loads path's top-level code into a fresh, independent VM once. NULL on a compile/runtime error. */
static Actor* aer_actor_spawn(const char* path) {
    /* read_file() (called inside aer_vm_instantiate_from_file) only ever reads through this
       pointer via fopen() -- never mutated, so the cast is safe in practice, matching every
       other path string in this codebase's own imprecise-but-harmless char* convention. */
    VM* vm = NULL;
    Chunk* chunk = NULL;
    unsigned int halt_addr = 0;
    if (aer_vm_instantiate_from_file((char*)path, &vm, &chunk, &halt_addr) != INSTANTIATE_OK)
        return NULL;

    Actor* a = xmalloc(sizeof(Actor));
    a->vm = vm;
    a->chunk = chunk;
    a->halt_addr = halt_addr;
    a->id = next_actor_id++;
    a->mailbox_head = NULL;
    a->mailbox_tail = NULL;
    aer_mutex_init(&a->mailbox_lock);
    a->next = actors;
    actors = a;
    return a;
}

/* A stable, process-unique handle safe to hand to AER scripts as a plain integer. Never a raw
   pointer cast -- a script passing back a wrong/stale integer must get a clean "no such actor"
   error via aer_actor_find(), not a wild pointer dereference; the whole point of vm_run() never
   crashing the host applies just as much to a script's own mistakes here. */
static unsigned int aer_actor_id(Actor* actor) {
    return actor->id;
}

static Actor* aer_actor_find(unsigned int id) {
    for (Actor* a = actors; a; a = a->next)
        if (a->id == id)
            return a;
    return NULL;
}

Actor* aer_actor_resolve(AerVal handle) {
    if (aer_type(handle) != TYPE_INTEGER)
        return NULL;
    return aer_actor_find((unsigned int)aer_as_int(handle));
}

VM* aer_actor_vm(Actor* actor) {
    return actor->vm;
}

bool aer_actor_prepare_call(Actor* actor, const char* fn, int arg_count, AerVal* args) {
    ChunkFunction* fnreg = chunk_find_function(actor->chunk, fn);
    if (!fnreg)
        return false;

    VM* mv = actor->vm;
    /* A prior call's error can leave call_depth/stack_top stuck above 0 (same reset
       aer_module_call needs and for the same reason -- see its own comment). */
    aer_vm_reset_for_reuse(mv);

    return setup_call(mv, fnreg, arg_count, args, actor->halt_addr);
}

bool aer_actor_call(Actor* actor, const char* fn, int arg_count, AerVal* args, AerVal* out_result) {
    if (!aer_actor_prepare_call(actor, fn, arg_count, args))
        return false;
    VM* mv = actor->vm;

    /* Called directly by host (C) code, never from inside another VM's bytecode dispatch, so
       there's no enclosing vm_run() to longjmp back into on failure the way aer_module_call
       needs -- a plain false return is enough. */
    vm_gc_suppress();
    vm_run(mv);
    vm_gc_unsuppress();
    bool ok = !runtime_had_error;
    /* error()/error_at() always set both flags together (error.c) -- runtime_had_error alone isn't
       enough to reset here, or parse_had_error stays permanently poisoned for the rest of the
       process the moment any actor's own call errors once, even though nothing is actually still
       mid-parse. This actor's own failure must not leak into the caller's other work at all. */
    runtime_had_error = false;
    parse_had_error = false;
    if (!ok)
        return false;

    *out_result = mv->call_stack[0].registers[0];
    return true;
}

/* Mailbox: a plain host-side FIFO of byte strings, never a live AerVal -- a value from one
   actor's pools is meaningless in another's. Message content (e.g. JSON, via each side's own
   json.encode()/json.decode() calls) is entirely up to the AER code on each end; the mailbox
   itself only ever moves bytes. send() copies message; try_receive() hands back an owned buffer
   the caller must free(). */
static bool aer_actor_send(Actor* actor, const char* message, unsigned int len) {
    Mailbox* m = xmalloc(sizeof(Mailbox));
    m->data = xmalloc(len);
    memcpy(m->data, message, len);
    m->len = len;
    m->next = NULL;
    aer_mutex_lock(&actor->mailbox_lock);
    if (actor->mailbox_tail)
        actor->mailbox_tail->next = m;
    else
        actor->mailbox_head = m;
    actor->mailbox_tail = m;
    aer_mutex_unlock(&actor->mailbox_lock);
    return true;
}

static bool aer_actor_try_receive(Actor* actor, char** out_message, unsigned int* out_len) {
    aer_mutex_lock(&actor->mailbox_lock);
    Mailbox* m = actor->mailbox_head;
    if (!m) {
        aer_mutex_unlock(&actor->mailbox_lock);
        return false;
    }
    actor->mailbox_head = m->next;
    if (!actor->mailbox_head)
        actor->mailbox_tail = NULL;
    *out_message = m->data;
    *out_len = m->len;
    aer_mutex_unlock(&actor->mailbox_lock);
    free(m);
    return true;
}

static void free_mailbox(Actor* a) {
    Mailbox* m = a->mailbox_head;
    while (m) {
        Mailbox* next = m->next;
        free(m->data);
        free(m);
        m = next;
    }
}

static void aer_actor_free(Actor* actor) {
    Actor** link = &actors;
    while (*link && *link != actor)
        link = &(*link)->next;
    if (*link)
        *link = actor->next;

    vm_free(actor->vm);
    chunk_free(actor->chunk);
    free(actor->vm);
    free(actor->chunk);
    free_mailbox(actor);
    aer_mutex_destroy(&actor->mailbox_lock);
    free(actor);
}

void aer_actor_free_all(void) {
    while (actors)
        aer_actor_free(actors);
}

/* ------------------------------------------------------------------ */
/* Script-facing `actor` module                                         */
/* ------------------------------------------------------------------ */

/* Script-facing surface over aer_actor.c's host-only primitives -- spawn/send/receive/call, same
   fixed CALL_MODULE_* dispatch shape as every other stdlib module. Adds no concurrency of its own:
   every call here still runs to completion before the caller's next line, exactly like
   aer_actor_call already does from host C code. Its purpose is giving actors a name AER scripts can
   reach, so the scheduler (aer_scheduler.c) has a real language feature to schedule instead of only
   a hand-written C test driver. */

/* noinline -- see aer_host_call's own comment (aer_host.c): this function's 4KB `AerVal
   popped[VM_STACK_MAX]` local was one of three such arrays LTO was folding into vm_run_slice's own
   frame, since each of the three has exactly one call site. */
__attribute__((noinline)) bool aer_actor_module_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_ACTOR_SPAWN && arg_count == 1) {
        AerVal path_v = vm_stack_pop(vm);
        if (aer_type(path_v) != TYPE_STRING) {
            error("actor.spawn() requires a path string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        Actor* a = aer_actor_spawn(aer_as_string(path_v)->data);
        if (!a) {
            vm_stack_push(vm,
                          aer_make_result(aer_null(),
                                          aer_make_error("actor.spawn(): failed to load or run the script")));
            return true;
        }
        vm_stack_push(vm, aer_make_result(aer_int((int64_t)aer_actor_id(a)), aer_null()));
        return true;
    }

    if (fn_id == FN_ACTOR_SEND && arg_count == 2) {
        AerVal message_v = vm_stack_pop(vm);
        AerVal handle_v = vm_stack_pop(vm);
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
        char* message;
        unsigned int len;
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
        for (int i = arg_count - 1; i >= 0; i--)
            popped[i] = vm_stack_pop(vm);

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
            vm_stack_push(
                vm, aer_make_result(aer_null(),
                                    aer_make_error("actor.call(): function not found or the call failed")));
            return true;
        }
        vm_stack_push(vm, aer_make_result(result, aer_null()));
        return true;
    }

    return false;
}

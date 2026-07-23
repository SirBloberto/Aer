#include <stdlib.h>
#include <string.h>
#include "aer.h"
#include "aer_actor.h"
#include "aer_module.h"
#include "error.h"

typedef struct Mailbox {
    char* data;
    unsigned int len;
    struct Mailbox* next;
} Mailbox;

struct Actor {
    VM*          vm;
    Chunk*       chunk;
    unsigned int halt_addr;
    unsigned int id;
    Mailbox*     mailbox_head;
    Mailbox*     mailbox_tail;
    struct Actor* next;   /* process-wide registry, for GC root enumeration and free_all */
};

static Actor* actors = NULL;
static unsigned int next_actor_id = 1;   /* 0 reserved as "no such actor" */

Actor* aer_actor_spawn(const char* path) {
    /* read_file() (called inside aer_vm_instantiate_from_file) only ever reads through this
       pointer via fopen() -- never mutated, so the cast is safe in practice, matching every
       other path string in this codebase's own imprecise-but-harmless char* convention. */
    VM* vm = NULL; Chunk* chunk = NULL; unsigned int halt_addr = 0;
    if (aer_vm_instantiate_from_file((char*)path, &vm, &chunk, &halt_addr) != INSTANTIATE_OK) return NULL;

    Actor* a = xmalloc(sizeof(Actor));
    a->vm           = vm;
    a->chunk        = chunk;
    a->halt_addr    = halt_addr;
    a->id           = next_actor_id++;
    a->mailbox_head = NULL;
    a->mailbox_tail = NULL;
    a->next         = actors;
    actors = a;
    return a;
}

unsigned int aer_actor_id(Actor* actor) { return actor->id; }

Actor* aer_actor_find(unsigned int id) {
    for (Actor* a = actors; a; a = a->next) if (a->id == id) return a;
    return NULL;
}

Actor* aer_actor_resolve(AerVal handle) {
    if (aer_type(handle) != TYPE_INTEGER) return NULL;
    return aer_actor_find((unsigned int)aer_as_int(handle));
}

VM* aer_actor_vm(Actor* actor) { return actor->vm; }

bool aer_actor_prepare_call(Actor* actor, const char* fn, int arg_count, AerVal* args) {
    ChunkFunction* fnreg = chunk_find_function(actor->chunk, fn);
    if (!fnreg) return false;

    VM* mv = actor->vm;
    /* A prior call's error can leave call_depth/stack_top stuck above 0 (same reset
       aer_module_call needs and for the same reason — see its own comment). */
    aer_vm_reset_for_reuse(mv);

    return setup_call(mv, fnreg, arg_count, args, actor->halt_addr);
}

bool aer_actor_call(Actor* actor, const char* fn, int arg_count, AerVal* args, AerVal* out_result) {
    if (!aer_actor_prepare_call(actor, fn, arg_count, args)) return false;
    VM* mv = actor->vm;

    /* Called directly by host (C) code, never from inside another VM's bytecode dispatch, so
       there's no enclosing vm_run() to longjmp back into on failure the way aer_module_call
       needs — a plain false return is enough. */
    vm_gc_suppress();
    vm_run(mv);
    vm_gc_unsuppress();
    bool ok = !runtime_had_error;
    /* error()/error_at() always set both flags together (error.c) -- runtime_had_error alone isn't
       enough to reset here, or parse_had_error stays permanently poisoned for the rest of the
       process the moment any actor's own call errors once, even though nothing is actually still
       mid-parse. This actor's own failure must not leak into the caller's other work at all. */
    runtime_had_error = false;
    parse_had_error   = false;
    if (!ok) return false;

    *out_result = mv->call_stack[0].registers[0];
    return true;
}

bool aer_actor_send(Actor* actor, const char* message, unsigned int len) {
    Mailbox* m = xmalloc(sizeof(Mailbox));
    m->data = xmalloc(len);
    memcpy(m->data, message, len);
    m->len  = len;
    m->next = NULL;
    if (actor->mailbox_tail) actor->mailbox_tail->next = m;
    else                     actor->mailbox_head = m;
    actor->mailbox_tail = m;
    return true;
}

bool aer_actor_try_receive(Actor* actor, char** out_message, unsigned int* out_len) {
    Mailbox* m = actor->mailbox_head;
    if (!m) return false;
    actor->mailbox_head = m->next;
    if (!actor->mailbox_head) actor->mailbox_tail = NULL;
    *out_message = m->data;
    *out_len     = m->len;
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

void aer_actor_free(Actor* actor) {
    Actor** link = &actors;
    while (*link && *link != actor) link = &(*link)->next;
    if (*link) *link = actor->next;

    vm_free(actor->vm);
    chunk_free(actor->chunk);
    free(actor->vm);
    free(actor->chunk);
    free_mailbox(actor);
    free(actor);
}

void aer_actor_free_all(void) {
    while (actors) aer_actor_free(actors);
}


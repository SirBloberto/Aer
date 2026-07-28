#include <stdlib.h>
#include "error.h"
#include "pool.h"

#define POOL_INITIAL_SLABS 4

void pool_init(Pool* p, size_t elem_size, unsigned int elems_per_slab) {
    p->slabs          = NULL;
    p->slab_count     = 0;
    p->slab_cap       = 0;
    p->elem_size      = elem_size;
    /* 8-byte alignment keeps every cell's pointer/double members aligned; the 16-byte floor is
       pool_free's free-list pointer at bytes [8,16). See stride's comment in pool.h. */
    size_t stride = (elem_size + 7) & ~(size_t)7;
    if (stride < 16) stride = 16;
    p->stride         = stride;
    p->elems_per_slab = elems_per_slab;
    p->next_index     = elems_per_slab;   /* forces the first pool_alloc to grab a slab */
    p->free_list      = NULL;
}

static void pool_grow(Pool* p) {
    if (p->slab_count >= p->slab_cap) {
        p->slab_cap = p->slab_cap ? p->slab_cap * 2 : POOL_INITIAL_SLABS;
        p->slabs    = xrealloc(p->slabs, sizeof(char*) * p->slab_cap);
    }
    p->slabs[p->slab_count] = xmalloc(p->stride * p->elems_per_slab);
    p->slab_count++;
    p->next_index = 0;
}

void pool_finalize_all(Pool* p, void (*on_free)(void* cell)) {
    for (unsigned int i = 0; i < p->slab_count; i++) {
        unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
        for (unsigned int j = 0; j < count; j++) {
            char* cell = p->slabs[i] + (size_t)j * p->stride;
            if (*(unsigned char*)cell & POOL_FREE) continue;
            on_free(cell);
        }
    }
}

void pool_destroy(Pool* p) {
    for (unsigned int i = 0; i < p->slab_count; i++) free(p->slabs[i]);
    free(p->slabs);
    *p = (Pool){0};
}

void* pool_alloc(Pool* p) {
    if (p->free_list) {
        void* cell = p->free_list;
        /* Next-pointer lives at [sizeof(void*), 2*sizeof(void*)), not [0, sizeof(void*)) — see pool.h. */
        p->free_list = *(void**)((char*)cell + sizeof(void*));
        *(unsigned char*)cell = 0;   /* always born young, regardless of which physical cell was reused — see pool.h */
        return cell;
    }
    if (p->next_index >= p->elems_per_slab) pool_grow(p);
    char* cell = p->slabs[p->slab_count - 1] + (size_t)p->next_index * p->stride;
    *(unsigned char*)cell = 0;
    p->next_index++;
    return cell;
}

void pool_free(Pool* p, void* cell) {
    /* Discards whatever mark/generation bits the cell had — neither is consulted on the free-list, and pool_alloc zeroes the byte again on reuse anyway. */
    *(unsigned char*)cell = POOL_FREE;
    *(void**)((char*)cell + sizeof(void*)) = p->free_list;   /* see pool_free's own comment on why not offset 0 */
    p->free_list = cell;
}

bool pool_mark(Pool* p, void* cell) {
    (void)p;
    unsigned char* state = (unsigned char*)cell;
    if (*state & POOL_MARKED) return true;
    *state |= POOL_MARKED;
    return false;
}

bool pool_is_young(Pool* p, void* cell) {
    (void)p;
    return (*(unsigned char*)cell & POOL_OLD) == 0;
}

bool pool_is_freed(Pool* p, void* cell) {
    (void)p;
    return (*(unsigned char*)cell & POOL_FREE) != 0;
}

bool pool_is_remembered(Pool* p, void* cell) {
    (void)p;
    return (*(unsigned char*)cell & POOL_REMEMBERED) != 0;
}

void pool_mark_remembered(Pool* p, void* cell) {
    (void)p;
    *(unsigned char*)cell |= POOL_REMEMBERED;
}

void pool_sweep(Pool* p, bool young_only, void (*on_free)(void* cell)) {
    for (unsigned int i = 0; i < p->slab_count; i++) {
        unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
        for (unsigned int j = 0; j < count; j++) {
            char* cell = p->slabs[i] + (size_t)j * p->stride;
            unsigned char* state = (unsigned char*)cell;
            if (*state & POOL_FREE) continue;                  /* already free-listed; nothing marks a free cell, so don't re-free it */
            if (young_only && (*state & POOL_OLD)) continue;    /* old cells are presumed live during a minor pass */
            if (*state & POOL_MARKED) {
                *state = (unsigned char)((*state & ~POOL_MARKED) | POOL_OLD);   /* survived -> promote */
            } else {
                on_free(cell);
                pool_free(p, cell);   /* sets *state = POOL_FREE internally */
            }
        }
    }
}

void pool_clear_marks(Pool* p, bool young_only) {
    for (unsigned int i = 0; i < p->slab_count; i++) {
        unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
        for (unsigned int j = 0; j < count; j++) {
            char* cell = p->slabs[i] + (size_t)j * p->stride;
            unsigned char* state = (unsigned char*)cell;
            /* An old cell's mark bit is never set OR read during a minor cycle in the first place
               (the mark phase itself now skips descending into old objects at all, see
               worklist_push's own comment, gc.c; pool_sweep's young_only path never consults an old
               cell's mark bit either) -- so a minor clear_marks has nothing to do for old cells,
               and large old pools (the common case for anything long-lived) stop costing anything
               here at all. Only a major cycle's full trace can set an old cell's mark bit, so only
               a major clear_marks needs to clear it. */
            if (young_only && (*state & POOL_OLD)) continue;
            *state &= (unsigned char)~POOL_MARKED;
        }
    }
}

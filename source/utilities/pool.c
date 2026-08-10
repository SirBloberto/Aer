#include <stdlib.h>
#include "error.h"
#include "pool.h"

#define POOL_INITIAL_SLABS 4

void pool_init(Pool* p, size_t elem_size, unsigned int elems_per_slab) {
    p->slabs = NULL;
    p->slab_count = 0;
    p->slab_cap = 0;
    p->elem_size = elem_size;
    /* 8-byte alignment keeps every cell's pointer/double members aligned; the 16-byte floor is
       pool_free's free-list pointer at bytes [8,16). See stride's comment in pool.h. */
    size_t stride = (elem_size + 7) & ~(size_t)7;
    if (stride < 16) stride = 16;
    p->stride = stride;
    p->elems_per_slab = elems_per_slab;
    p->next_index = elems_per_slab; /* forces the first pool_alloc to grab a slab */
    p->free_list = NULL;
    p->slab_young_count = NULL;
    p->slab_free_list = NULL;
    p->slab_free_next = NULL;
    p->free_slab_head = POOL_NO_SLAB;
    p->young_slab_prev = NULL;
    p->young_slab_next = NULL;
    p->young_slab_head = POOL_NO_SLAB;
}

static void pool_grow(Pool* p) {
    if (p->slab_count >= p->slab_cap) {
        p->slab_cap = p->slab_cap ? p->slab_cap * 2 : POOL_INITIAL_SLABS;
        p->slabs = xrealloc(p->slabs, sizeof(char*) * p->slab_cap);
        p->slab_young_count = xrealloc(p->slab_young_count, sizeof(unsigned int) * p->slab_cap);
        p->slab_free_list = xrealloc(p->slab_free_list, sizeof(void*) * p->slab_cap);
        p->slab_free_next = xrealloc(p->slab_free_next, sizeof(unsigned int) * p->slab_cap);
        p->young_slab_prev = xrealloc(p->young_slab_prev, sizeof(unsigned int) * p->slab_cap);
        p->young_slab_next = xrealloc(p->young_slab_next, sizeof(unsigned int) * p->slab_cap);
    }
    p->slabs[p->slab_count] = xmalloc(p->stride * p->elems_per_slab);
    p->slab_young_count[p->slab_count] = 0;
    p->slab_free_list[p->slab_count] =
        NULL; /* fresh slab has no free cells yet -- not in the free_slab_head list */
    p->slab_free_next[p->slab_count] = POOL_NO_SLAB;
    p->young_slab_prev[p->slab_count] =
        POOL_NO_SLAB; /* not yet in the young thread -- starts at young_count 0 */
    p->young_slab_next[p->slab_count] = POOL_NO_SLAB;
    p->slab_count++;
    p->next_index = 0;
}

/* Pushes slab i onto the head of the "slabs with young_count > 0" thread. Caller's responsibility
   to only call this on a 0 -> 1 young_count transition (see pool.h's own comment on young_slab_head)
   -- calling it on an already-linked slab would corrupt the list. */
static void pool_link_young(Pool* p, unsigned int i) {
    p->young_slab_prev[i] = POOL_NO_SLAB;
    p->young_slab_next[i] = p->young_slab_head;
    if (p->young_slab_head != POOL_NO_SLAB) p->young_slab_prev[p->young_slab_head] = i;
    p->young_slab_head = i;
}

/* Removes slab i from the young thread, from wherever it currently sits (not just the head) --
   see pool.h's own comment on why this thread is doubly-linked, unlike slab_free_list's. */
static void pool_unlink_young(Pool* p, unsigned int i) {
    unsigned int prev = p->young_slab_prev[i];
    unsigned int next = p->young_slab_next[i];
    if (prev != POOL_NO_SLAB)
        p->young_slab_next[prev] = next;
    else
        p->young_slab_head = next;
    if (next != POOL_NO_SLAB) p->young_slab_prev[next] = prev;
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
    for (unsigned int i = 0; i < p->slab_count; i++)
        free(p->slabs[i]);
    free(p->slabs);
    free(p->slab_young_count);
    free(p->slab_free_list);
    free(p->slab_free_next);
    free(p->young_slab_prev);
    free(p->young_slab_next);
    *p = (Pool){0};
}

void* pool_alloc(Pool* p) {
    /* Per-slab free lists first (populated only by pool_free_at, i.e. only for pools whose cells
       are freed via pool_sweep's own internal path -- see pool.h). O(1): free_slab_head always
       names a slab with >=1 free cell, no search needed. */
    if (p->free_slab_head != POOL_NO_SLAB) {
        unsigned int i = p->free_slab_head;
        void* cell = p->slab_free_list[i];
        /* Next-pointer lives at [sizeof(void*), 2*sizeof(void*)), not [0, sizeof(void*)) -- see pool.h. */
        p->slab_free_list[i] = *(void**)((char*)cell + sizeof(void*));
        if (!p->slab_free_list[i])
            p->free_slab_head = p->slab_free_next[i]; /* slab i is empty again -- leave the list */
        *(unsigned char*)cell =
            0; /* always born young, regardless of which physical cell was reused -- see pool.h */
        if (p->slab_young_count[i] == 0)
            pool_link_young(p, i); /* 0 -> 1 transition -- rejoin the young thread */
        p->slab_young_count[i]++; /* exact and unconditional -- this slab is known for certain */
        return cell;
    }
    /* Falls through to the pool-wide free list -- in practice only ever populated by hashtable.c's
       own external pool_free calls (see pool.h); the 8 GC-tracked pools never reach this branch. */
    if (p->free_list) {
        void* cell = p->free_list;
        p->free_list = *(void**)((char*)cell + sizeof(void*));
        *(unsigned char*)cell = 0;
        return cell;
    }
    if (p->next_index >= p->elems_per_slab) pool_grow(p);
    char* cell = p->slabs[p->slab_count - 1] + (size_t)p->next_index * p->stride;
    *(unsigned char*)cell = 0;
    p->next_index++;
    if (p->slab_young_count[p->slab_count - 1] == 0)
        pool_link_young(p, p->slab_count - 1); /* 0 -> 1 transition */
    p->slab_young_count[p->slab_count - 1]++;
    return cell;
}

void pool_free(Pool* p, void* cell) {
    /* Discards whatever mark/generation bits the cell had -- neither is consulted on the free-list, and pool_alloc zeroes the byte again on reuse anyway. */
    *(unsigned char*)cell = POOL_FREE;
    *(void**)((char*)cell + sizeof(void*)) =
        p->free_list; /* see pool_free's own comment on why not offset 0 */
    p->free_list = cell;
}

/* Only ever called from pool_sweep's own loop below, which already knows slab_index for free --
   see slab_free_list's own comment, pool.h. Threads cell onto slab_index's own free list instead
   of the pool-wide one, so a later reuse can attribute it to its real slab unconditionally. */
static void pool_free_at(Pool* p, void* cell, unsigned int slab_index) {
    *(unsigned char*)cell = POOL_FREE;
    *(void**)((char*)cell + sizeof(void*)) = p->slab_free_list[slab_index];
    bool was_empty = (p->slab_free_list[slab_index] == NULL);
    p->slab_free_list[slab_index] = cell;
    if (was_empty) { /* slab_index just gained its first free cell -- join the free_slab_head list */
        p->slab_free_next[slab_index] = p->free_slab_head;
        p->free_slab_head = slab_index;
    }
}

/* These five all read/write only the cell's own state byte -- a Pool* was never needed, since
   gc_state lives in the object, not a side table (see pool.h). */

bool pool_mark(void* cell) {
    unsigned char* state = (unsigned char*)cell;
    if (*state & POOL_MARKED) return true;
    *state |= POOL_MARKED;
    return false;
}

bool pool_is_young(void* cell) {
    return (*(unsigned char*)cell & POOL_OLD) == 0;
}

bool pool_is_freed(void* cell) {
    return (*(unsigned char*)cell & POOL_FREE) != 0;
}

bool pool_is_remembered(void* cell) {
    return (*(unsigned char*)cell & POOL_REMEMBERED) != 0;
}

void pool_mark_remembered(void* cell) {
    *(unsigned char*)cell |= POOL_REMEMBERED;
}

/* Leaves every surviving cell mark-free (the promote branch clears it), and pool_alloc zeroes the
   state byte of every cell it hands out -- together that is what makes a separate pre-mark clearing
   pass unnecessary. Don't add one back: it would be a pure no-op scan of the whole heap. */
void pool_sweep(Pool* p, bool young_only, void (*on_free)(void* cell), unsigned int* live_out) {
    if (young_only) {
        /* Walk ONLY the slabs the young thread says still have >=1 young cell -- O(live young
           slabs), not O(slab_count). slab_young_count[i] alone only let the per-CELL scan skip a
           fully-old-or-free slab in O(1); this outer walk is what makes SKIPPING that read
           altogether possible for the (usually large majority of) fully-promoted slabs a big,
           long-lived grow-only pool accumulates -- see young_slab_head's own comment, pool.h. */
        unsigned int i = p->young_slab_head;
        while (i != POOL_NO_SLAB) {
            unsigned int next_slab = p->young_slab_next[i]; /* save before i is possibly unlinked below */
            unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
            for (unsigned int j = 0; j < count; j++) {
                char* cell = p->slabs[i] + (size_t)j * p->stride;
                unsigned char* state = (unsigned char*)cell;
                if (*state & POOL_FREE)
                    continue; /* already free-listed; nothing marks a free cell, so don't re-free it */
                if (*state & POOL_OLD) continue; /* old cells are presumed live during a minor pass */
                if (*state & POOL_MARKED) {
                    *state = (unsigned char)((*state & ~POOL_MARKED) | POOL_OLD); /* survived -> promote */
                } else {
                    on_free(cell);
                    pool_free_at(
                        p, cell,
                        i); /* sets *state = POOL_FREE internally; i is this cell's real slab, known for free here */
                }
                p->slab_young_count[i]--; /* resolved either way (promoted or freed) -- no longer young */
            }
            if (p->slab_young_count[i] == 0)
                pool_unlink_young(p, i); /* every young cell in i resolved this pass -- leave the thread */
            i = next_slab;
        }
        return;
    }
    /* Major sweep: old cells matter too (may still get freed if genuinely unreachable), so every
       slab must be visited regardless of the young thread -- that thread only ever tracks young
       cells. */
    for (unsigned int i = 0; i < p->slab_count; i++) {
        unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
        for (unsigned int j = 0; j < count; j++) {
            char* cell = p->slabs[i] + (size_t)j * p->stride;
            unsigned char* state = (unsigned char*)cell;
            if (*state & POOL_FREE) continue;
            bool was_young = (*state & POOL_OLD) == 0;
            if (*state & POOL_MARKED) {
                *state = (unsigned char)((*state & ~POOL_MARKED) | POOL_OLD); /* survived -> promote */
                if (live_out) (*live_out)++;
            } else {
                on_free(cell);
                pool_free_at(p, cell, i);
            }
            if (was_young) {
                p->slab_young_count[i]--;
                if (p->slab_young_count[i] == 0)
                    pool_unlink_young(p, i); /* a major pass can also drain a slab's last young cell */
            }
        }
    }
}

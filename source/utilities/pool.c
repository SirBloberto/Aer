#include "error.h"
#include "pool.h"

#define POOL_INITIAL_SLABS 4

unsigned int pool_total_alloc_count = 0;

void pool_init(Pool* p, size_t elem_size, unsigned int elems_per_slab) {
    p->slabs          = NULL;
    p->cell_state     = NULL;
    p->slab_count     = 0;
    p->slab_cap       = 0;
    p->elem_size      = elem_size;
    /* Round elem_size up to the next power of two — see stride's own comment in pool.h. */
    size_t stride = 1;
    unsigned int shift = 0;
    while (stride < elem_size) { stride <<= 1; shift++; }
    p->stride         = stride;
    p->stride_shift   = shift;
    p->elems_per_slab = elems_per_slab;
    p->next_index     = elems_per_slab;   /* forces the first pool_alloc to grab a slab */
    p->free_list      = NULL;
}

static void pool_grow(Pool* p) {
    if (p->slab_count >= p->slab_cap) {
        p->slab_cap   = p->slab_cap ? p->slab_cap * 2 : POOL_INITIAL_SLABS;
        p->slabs      = xrealloc(p->slabs,      sizeof(char*) * p->slab_cap);
        p->cell_state = xrealloc(p->cell_state, sizeof(unsigned char*) * p->slab_cap);
    }
    p->slabs[p->slab_count]      = xmalloc(p->stride * p->elems_per_slab);
    p->cell_state[p->slab_count] = xcalloc(p->elems_per_slab, 1);   /* all zero: unmarked, young */
    p->slab_count++;
    p->next_index = 0;
}

/* Maps a cell pointer to its state byte via a linear scan over the pool's slabs; every cell
   pool_alloc ever handed out is found here by construction. Scanned NEWEST slab first, not oldest:
   pool_alloc always bump-allocates fresh cells from the newest slab once the free list is empty,
   and the hottest caller (gc_barrier_array, called on every array/dict/struct-field write) is
   disproportionately likely to be checking a cell that was itself just allocated — e.g. a freshly
   constructed array getting its first element written. Found via perf: this function (previously
   oldest-first) was consistently 2-3% of nbody.aer's total cycles despite the comment above once
   calling it "fine at this project's scale" — a long-running, allocation-heavy program accumulates
   many slabs, and an oldest-first scan pays the full slab_count-1 traversal for exactly the
   allocations most likely to be looked up again immediately. */
static unsigned char* pool_cell_state_or_null(Pool* p, void* cell) {
    for (unsigned int i = p->slab_count; i-- > 0; ) {
        char*  slab       = p->slabs[i];
        size_t slab_bytes = p->stride * p->elems_per_slab;
        if ((char*)cell >= slab && (char*)cell < slab + slab_bytes) {
            size_t offset = (size_t)((char*)cell - slab);
            return &p->cell_state[i][offset >> p->stride_shift];
        }
    }
    return NULL;
}

static unsigned char* pool_cell_state(Pool* p, void* cell) {
    unsigned char* state = pool_cell_state_or_null(p, cell);
    if (!state) aer_report_fatal("pool_cell_state: cell not found in any slab");
    return state;
}

void* pool_alloc(Pool* p) {
    pool_total_alloc_count++;
    if (p->free_list) {
        void* cell = p->free_list;
        p->free_list = *(void**)cell;
        *pool_cell_state(p, cell) = 0;   /* always born young, regardless of which physical cell was reused — see pool.h */
        return cell;
    }
    if (p->next_index >= p->elems_per_slab) pool_grow(p);
    char* cell = p->slabs[p->slab_count - 1] + (size_t)p->next_index * p->stride;
    p->cell_state[p->slab_count - 1][p->next_index] = 0;
    p->next_index++;
    return cell;
}

void pool_free(Pool* p, void* cell) {
    /* Discards whatever mark/generation bits the cell had — neither is consulted on the free-list, and pool_alloc zeroes the byte again on reuse anyway. */
    *pool_cell_state(p, cell) = POOL_FREE;
    *(void**)cell = p->free_list;
    p->free_list  = cell;
}

bool pool_mark(Pool* p, void* cell) {
    unsigned char* state = pool_cell_state(p, cell);
    if (*state & POOL_MARKED) return true;
    *state |= POOL_MARKED;
    return false;
}

bool pool_is_young(Pool* p, void* cell) {
    return (*pool_cell_state(p, cell) & POOL_OLD) == 0;
}

bool pool_is_freed(Pool* p, void* cell) {
    return (*pool_cell_state(p, cell) & POOL_FREE) != 0;
}

bool pool_is_remembered(Pool* p, void* cell) {
    return (*pool_cell_state(p, cell) & POOL_REMEMBERED) != 0;
}

void pool_mark_remembered(Pool* p, void* cell) {
    *pool_cell_state(p, cell) |= POOL_REMEMBERED;
}

void pool_sweep(Pool* p, bool young_only, void (*on_free)(void* cell)) {
    for (unsigned int i = 0; i < p->slab_count; i++) {
        unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
        for (unsigned int j = 0; j < count; j++) {
            unsigned char* state = &p->cell_state[i][j];
            if (*state & POOL_FREE) continue;                  /* already free-listed; nothing marks a free cell, so don't re-free it */
            if (young_only && (*state & POOL_OLD)) continue;    /* old cells are presumed live during a minor pass */
            char* cell = p->slabs[i] + (size_t)j * p->stride;
            if (*state & POOL_MARKED) {
                *state = (unsigned char)((*state & ~POOL_MARKED) | POOL_OLD);   /* survived -> promote */
            } else {
                on_free(cell);
                pool_free(p, cell);   /* sets *state = POOL_FREE internally */
            }
        }
    }
}

void pool_clear_marks(Pool* p) {
    for (unsigned int i = 0; i < p->slab_count; i++) {
        unsigned int count = (i == p->slab_count - 1) ? p->next_index : p->elems_per_slab;
        for (unsigned int j = 0; j < count; j++)
            p->cell_state[i][j] &= (unsigned char)~POOL_MARKED;
    }
}

#ifndef POOL_H
#define POOL_H

#include <stdbool.h>
#include <stddef.h>

/* Slab allocator for fixed-size objects, extended for generational mark-sweep GC. Each cell's
   one-byte GC state is the owning struct's own first field (pinned to offset 0 by _Static_asserts
   in value.h) — in the object, not a side table, so barrier checks need no reverse lookup.

   An aligned-slab + per-slab packed side-array alternative (state bytes moved out of the struct
   entirely, addressed by masking a cell pointer) was prototyped and measured against sieve/nbody/
   fib/dict/small-dict/lookup-table/hash-cache-micro: no net win on the large-dict case it targeted,
   and a real ~8-20% regression on sieve, small_dict_bench, lookup_table_bench, and
   hash_cache_micro (the extra mask+divide+separate-cache-line cost on every individual
   pool_mark/pool_is_young call outweighed the sweep/clear_marks locality win everywhere but the one
   case it was aimed at). Reverted; this embedded-byte design stays. */
#define POOL_MARKED     0x1   /* this collection cycle only */
#define POOL_OLD        0x2   /* set once a cell survives a collection; cleared on every pool_alloc */
#define POOL_FREE       0x4   /* on the free-list — stops pool_sweep re-pushing, and lets a stale remembered-set entry be detected */
#define POOL_REMEMBERED 0x8   /* in the GC's remembered set; never cleared (the set is add-only) */

typedef struct {
    char**          slabs;
    unsigned int    slab_count, slab_cap;
    unsigned int    next_index;      /* next free cell within the current (last) slab */
    size_t          elem_size;
    size_t          stride;          /* elem_size 8-aligned, 16-byte floor for the free-list pointer */
    unsigned int    elems_per_slab;
    void*           free_list;       /* linked through freed cells' bytes [8,16) — offset 0 would clobber gc_state */
} Pool;

void  pool_init(Pool* p, size_t elem_size, unsigned int elems_per_slab);

/* Calls on_free on every live (non-free-listed) cell, ignoring mark/generation bits entirely --
   for tearing down a whole pool (every cell's payload needs freeing, not just the ones a normal
   generational sweep would collect). Call before pool_destroy, which only frees the pool's own
   slab memory, not each cell's own separately-owned payload. */
void  pool_finalize_all(Pool* p, void (*on_free)(void* cell));

/* Frees every slab buffer plus the slabs array itself -- for tearing down a whole pool (a VM's own
   heap going away), not for freeing one cell (see pool_free). Leaves *p zeroed, safe to reuse. */
void  pool_destroy(Pool* p);

/* Returns uninitialized memory, like malloc — caller fills it in; always born young, whether reused from the free-list or carved from a fresh slab. */
void* pool_alloc(Pool* p);

void  pool_free(Pool* p, void* cell);

/* Sets cell's mark bit; returns true if already set — the mark phase's guard against recursing into an already-visited cell. */
bool  pool_mark(Pool* p, void* cell);

bool  pool_is_young(Pool* p, void* cell);

/* True if cell is already on the free-list — a remembered-set entry can outlive its cell (entries are never removed, see vm.c), so retracing the set must check this before dereferencing. */
bool  pool_is_freed(Pool* p, void* cell);

/* True if cell's remembered-set bit is already set. */
bool  pool_is_remembered(Pool* p, void* cell);

/* Sets cell's remembered-set bit. Never cleared — see the comment above. */
void  pool_mark_remembered(Pool* p, void* cell);

/* Walks every carved-out cell; free-listed cells are always skipped, and if young_only, old cells too (a minor collection assumes old cells are live). Unmarked cells go to on_free then the free-list; marked cells are promoted with their mark bit cleared for next cycle. */
void  pool_sweep(Pool* p, bool young_only, void (*on_free)(void* cell));

/* Clears every cell's mark bit -- with young_only, skips old cells entirely (their mark bit is
   never set OR read during a minor cycle in the new mark-phase design, see worklist_push's own
   comment, gc.c, so a minor cycle has nothing to clear for them); without it (a major cycle),
   clears everything, since a major trace can mark an old cell and its sweep needs to see a clean
   slate. Must run before every mark phase either way. */
void  pool_clear_marks(Pool* p, bool young_only);

#endif

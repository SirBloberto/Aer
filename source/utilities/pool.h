#ifndef POOL_H
#define POOL_H

#include <stdbool.h>
#include <stddef.h>

/* Slab allocator for fixed-size objects, extended for generational mark-sweep GC. Each cell's
   one-byte GC state is the owning struct's own first field (pinned to offset 0 by _Static_asserts
   in value.h) -- in the object, not a side table, so barrier checks need no reverse lookup.

   An aligned-slab + per-slab packed side-array alternative (state bytes moved out of the struct
   entirely, addressed by masking a cell pointer) was prototyped and measured against sieve/nbody/
   fib/dict/small-dict/lookup-table/hash-cache-micro: no net win on the large-dict case it targeted,
   and a real ~8-20% regression on sieve, small_dict_bench, lookup_table_bench, and
   hash_cache_micro (the extra mask+divide+separate-cache-line cost on every individual
   pool_mark/pool_is_young call outweighed the sweep/clear_marks locality win everywhere but the one
   case it was aimed at). Reverted; this embedded-byte design stays. */
#define POOL_MARKED 0x1 /* this collection cycle only */
#define POOL_OLD 0x2 /* set once a cell survives a collection; cleared on every pool_alloc */
#define POOL_FREE                                                                                            \
    0x4 /* on the free-list -- stops pool_sweep re-pushing, and lets a stale remembered-set entry be detected */
#define POOL_REMEMBERED 0x8 /* in the GC's remembered set; never cleared (the set is add-only) */

/* Sentinel for Pool.free_slab_head / Pool.slab_free_next: "no slab" (empty list / list terminator). */
#define POOL_NO_SLAB ((unsigned int)-1)

typedef struct {
    char** slabs;
    unsigned int slab_count, slab_cap;
    unsigned int next_index; /* next free cell within the current (last) slab */
    size_t elem_size;
    size_t stride; /* elem_size 8-aligned, 16-byte floor for the free-list pointer */
    unsigned int elems_per_slab;

    /* Pool-wide free list, used ONLY by external (non-GC-scan-path) callers of pool_free -- in
       practice that's exclusively hashtable.c's own key/sparse-array pools, which gc_collect never
       passes through pool_clear_marks/pool_sweep with young_only, so they have no use for (and never
       populate) the per-slab machinery below. Linked through freed cells' bytes [8,16) -- offset 0
       would clobber gc_state. */
    void* free_list;

    /* Per-slab live-young-cell count, parallel to slabs[] (grown alongside it in pool_grow). Lets
       pool_clear_marks/pool_sweep skip an entire slab in O(1) once every cell in it is old or free
       -- the common shape for a large, incrementally-built, never-freed collection (the exact
       pattern struct_array_scan.aer's construction phase hit: pool_clear_marks+pool_sweep were ~90%
       of cycles even after old cells stopped being individually re-traced, because every minor
       cycle still touched every cell's state byte just to confirm it's old). */
    unsigned int* slab_young_count;

    /* Doubly-linked "slabs with young_count > 0" thread, parallel to slabs[], rooted at
       young_slab_head (POOL_NO_SLAB if none). slab_young_count[] alone only lets pool_sweep skip a
       fully-old-or-free slab's per-CELL scan in O(1) -- the sweep's own outer loop still visited
       EVERY slab index just to read that one flag, every single minor cycle, an O(slab_count) cost
       that dominates once a large grow-only pool (struct_array_scan.aer's 2M-particle struct_pools
       tier, ~2000 slabs) has accumulated many fully-promoted slabs behind a live construction phase. This
       thread lets a minor pool_sweep walk ONLY the slabs that still have >=1 young cell, true
       O(live young slabs) instead. Doubly-linked (not singly, unlike slab_free_list/free_slab_head
       above) because removal can happen to ANY slab in the thread, not just the head -- pool_sweep
       unlinks whichever slab it just finished sweeping to 0, wherever that slab sits in the list --
       whereas the free-list thread only ever pops its own head (pool_alloc's only consumer). A
       slab's young_count can go from 0 back above 0 later (a cell pool_sweep just freed gets reused
       by a later pool_alloc, which always allocates "born young" -- see pool_alloc's own comment),
       so this is add/remove, not a one-directional watermark: pool_alloc re-links a slab the moment
       its count crosses 0 -> 1, pool_sweep unlinks it the moment a sweep drives it back to 0. */
    unsigned int* young_slab_prev;
    unsigned int* young_slab_next;
    unsigned int young_slab_head;

    /* Per-slab free lists (parallel to slabs[]) plus an O(1) "which slabs currently have a free
       cell" thread (slab_free_next, rooted at free_slab_head) -- modeled directly on Luau's
       per-page free list + page-linking (lua_Page's own free list and prev/next fields), not AER's
       own earlier design here, which used ONE free list shared by the whole pool and therefore
       couldn't tell which slab a reused cell came from -- exactly the case that used to force
       slab_young_count's skip to give up entirely (see the removed `reused` field this replaces).
       Every one of the 8 GC-tracked pools frees cells exclusively via pool_sweep's own internal
       path (AER has no manual/explicit object destruction, only GC-driven reclamation), which
       always knows its own slab index for free -- so slab_young_count is now unconditionally exact
       for them, no fallback needed. hashtable.c's pools never populate this (they only ever call
       the public pool_free, never pool_free_at), so free_slab_head simply stays POOL_NO_SLAB for
       them forever and they fall straight through to the pool-wide free_list above, completely
       unaffected. */
    void** slab_free_list; /* per-slab free-list head, parallel to slabs[] */
    unsigned int* slab_free_next; /* per-slab "next slab with a free cell" thread, parallel to slabs[] */
    unsigned int free_slab_head; /* POOL_NO_SLAB if no slab currently has a free cell */
} Pool;

void pool_init(Pool* p, size_t elem_size, unsigned int elems_per_slab);

/* Calls on_free on every live (non-free-listed) cell, ignoring mark/generation bits entirely --
   for tearing down a whole pool (every cell's payload needs freeing, not just the ones a normal
   generational sweep would collect). Call before pool_destroy, which only frees the pool's own
   slab memory, not each cell's own separately-owned payload. */
void pool_finalize_all(Pool* p, void (*on_free)(void* cell));

/* Frees every slab buffer plus the slabs array itself -- for tearing down a whole pool (a VM's own
   heap going away), not for freeing one cell (see pool_free). Leaves *p zeroed, safe to reuse. */
void pool_destroy(Pool* p);

/* Returns uninitialized memory, like malloc -- caller fills it in; always born young, whether reused from the free-list or carved from a fresh slab. */
void* pool_alloc(Pool* p);

void pool_free(Pool* p, void* cell);

/* All five below read/write only *cell's own state byte -- no Pool* needed, since gc_state lives in
   the object itself, not a side table (see this file's own top comment). */

/* Sets cell's mark bit; returns true if already set -- the mark phase's guard against recursing into an already-visited cell. */
bool pool_mark(void* cell);

bool pool_is_young(void* cell);

/* True if cell is already on the free-list -- a remembered-set entry can outlive its cell (entries are never removed, see vm.c), so retracing the set must check this before dereferencing. */
bool pool_is_freed(void* cell);

/* True if cell's remembered-set bit is already set. */
bool pool_is_remembered(void* cell);

/* Sets cell's remembered-set bit. Never cleared -- see the comment above. */
void pool_mark_remembered(void* cell);

/* Walks every carved-out cell; free-listed cells are always skipped, and if young_only, old cells too (a minor collection assumes old cells are live). Unmarked cells go to on_free then the free-list; marked cells are promoted with their mark bit cleared for next cycle. */
void pool_sweep(Pool* p, bool young_only, void (*on_free)(void* cell));

#endif

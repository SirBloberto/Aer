#ifndef POOL_H
#define POOL_H

#include <stdbool.h>
#include <stddef.h>

/* A slab (bump/arena) allocator for fixed-size objects, extended for generational mark-and-sweep GC (see README's Memory and Security section). Allocation hands out a free-list cell if one exists, else the next cell in the current slab, growing by one fresh slab (one xmalloc, not one per object) when exhausted.

   Each cell carries its own one-byte GC state as its own literal first field (every pool-managed
   struct — AerString/AerArray/AerDict/AerFunction — declares `unsigned char gc_state;` first, and
   each has a _Static_assert pinning it to offset 0): bit 0 is the mark bit (this collection cycle
   only); bit 1 is the generation bit (0 = young, 1 = old, set once a cell survives a collection);
   bit 2 marks a cell on the free-list, so pool_sweep never re-discovers and re-pushes an
   already-free cell; bit 3 marks a cell already in the GC's remembered set (gc_remember, vm.c),
   never cleared once set (the remembered set itself is add-only). Byte-per-cell, not bit-packed,
   per this project's readability-over-micro-opt precedent.

   State lives IN the object, not a side array keyed by pointer, to avoid a pointer -> slab ->
   index reverse lookup on every write-barrier check. Offset 0 is a compile-time constant, never a
   per-pool stored value, so pool_is_young/pool_mark/etc. touch the cell directly with no extra load.

   pool_free's free-list "next" pointer is written starting at cell offset sizeof(void*), NOT
   offset 0 — writing at offset 0 would clobber the leading gc_state byte the instant a cell is
   freed, destroying the POOL_FREE bit pool_is_freed depends on (the remembered set can hold a
   stale pointer to an already-freed cell and must be able to detect that). Every pool-managed
   struct is comfortably larger than 2*sizeof(void*) once padded, so this is always safe space.

   Aging is per-cell, not slab-position-based: a slab-boundary scheme breaks the moment a freed OLD
   cell is reused for a young allocation, silently inheriting the wrong generation. pool_alloc
   always clears the generation bit on every allocation. */
#define POOL_MARKED     0x1
#define POOL_OLD        0x2
#define POOL_FREE       0x4
#define POOL_REMEMBERED 0x8

typedef struct {
    char**          slabs;
    unsigned int    slab_count, slab_cap;
    unsigned int    next_index;      /* next free cell within the current (last) slab */
    size_t          elem_size;
    /* Cell-to-cell byte stride for every address computation (all `index * stride` — no
       pointer-to-index division exists anywhere): elem_size rounded up to 8-byte alignment,
       16-byte floor for pool_free's free-list pointer at bytes [8,16). Computed once in
       pool_init. */
    size_t          stride;
    unsigned int    elems_per_slab;
    void*           free_list;       /* singly-linked through freed cells' bytes [sizeof(void*), 2*sizeof(void*)) — see the file comment above on why not [0, sizeof(void*)) */
} Pool;

/* Cells allocated across all GC-managed pools since last reset — gc_maybe_collect (vm.c) checks this on allocating opcodes; one shared counter beats summing per-pool fields that often. */
extern unsigned int pool_total_alloc_count;

void  pool_init(Pool* p, size_t elem_size, unsigned int elems_per_slab);

/* Returns uninitialized memory, like malloc — caller fills it in; always born young, whether reused from the free-list or carved from a fresh slab. */
void* pool_alloc(Pool* p);

/* Pushes cell back onto the free-list — called from pool_sweep (GC-managed header pools) and
   directly by any other pool user that frees a cell outside the mark-sweep cycle (e.g. hashtable.c's
   size-classed key/bucket-array pools, which are never independently GC-marked/swept — a cell is
   returned exactly when its owning caller says so, not via pool_sweep). */
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

/* Clears every cell's mark bit, young and old alike, with no other side effect — must run before every mark phase, since a minor sweep skips old cells and would otherwise leave their mark bit stuck forever. */
void  pool_clear_marks(Pool* p);

#endif

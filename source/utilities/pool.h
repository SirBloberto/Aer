#ifndef POOL_H
#define POOL_H

#include <stdbool.h>
#include <stddef.h>

/* Slab allocator for fixed-size objects, extended for generational mark-sweep GC. Each cell's
   one-byte GC state is the owning struct's own first field (pinned to offset 0 by _Static_asserts
   in value.h) — in the object, not a side table, so barrier checks need no reverse lookup. */
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

/* Cells allocated across all GC-managed pools since last reset — gc_maybe_collect (vm.c) checks this on allocating opcodes; one shared counter beats summing per-pool fields that often. */
extern unsigned int pool_total_alloc_count;

void  pool_init(Pool* p, size_t elem_size, unsigned int elems_per_slab);

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

/* Clears every cell's mark bit, young and old alike, with no other side effect — must run before every mark phase, since a minor sweep skips old cells and would otherwise leave their mark bit stuck forever. */
void  pool_clear_marks(Pool* p);

#endif

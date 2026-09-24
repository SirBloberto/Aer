#ifndef AER_HEAP_H
#define AER_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "hashtable.h"
#include "pool.h"
#include "value.h"

/* Per-VM heap -- every pool a VM allocates from, plus its own GC state. */

typedef enum { REMEMBERED_ARRAY, REMEMBERED_DICT, REMEMBERED_STRUCT } RememberedKind;
typedef struct {
    void* ptr;
    RememberedKind kind;
} RememberedEntry;

/* A wide array or dict still being traced, from element `next` on. */
typedef struct {
    void* container;
    unsigned int next;
    bool is_dict;
} MarkRange;

/* Explicit growable worklist, not C recursion, since user data structures have no depth limit;
   pool_mark's "already marked" return terminates cycles correctly. */
typedef struct {
    AerVal* items;
    unsigned int count, cap;
    MarkRange* ranges;
    unsigned int range_count, range_cap;
} MarkWorklist;

/* Size-keyed free-list cache for typed-array data buffers -- the payload, not the header, which is
   pooled like every other GC object. Checked by vm_new_typed_array, populated by free_typed_array.

   Budgeted by total bytes rather than per buffer: a per-buffer ceiling turned away the single 16MB
   buffer a 4M-element transform recycles every pass, costing a fresh mmap and 4096 page faults
   each time, while still admitting eight buffers sitting just under it. */
#define TYPED_ARRAY_FREE_CACHE_SLOTS 8
#define TYPED_ARRAY_FREE_CACHE_MAX_TOTAL_BYTES (32u * 1024 * 1024)

typedef struct {
    size_t size; /* 0 = empty slot */
    unsigned char* ptr;
} TypedArrayFreeSlot;

/* Size-classed slab pools for struct instances, keyed by Shape.instance_bytes. One pool sized for
   the MAX_STRUCT_FIELDS worst case cost 268 bytes/cell regardless of a shape's real field count,
   wasting ~78% on a 3-field node. Mirrors hashtable.c's KEY_TIER_SIZE scheme; the tier table lives
   in heap.c. The largest tier covers the worst case, so no malloc fallback is needed here. */
#define STRUCT_PAYLOAD_TIER_COUNT 5

/* Size-classed pools for the separately-owned payload of a string too long to inline. Same scheme
   as struct_pools above and hashtable.c's key pools, including their plain-malloc fallback past the
   largest tier, since a string's length is unbounded. */
#define STRING_PAYLOAD_TIER_COUNT 4

typedef struct {
    Pool string_pool, array_pool, dict_pool, function_pool, packed_array_pool, typed_array_pool, result_pool;
    Pool struct_pools[STRUCT_PAYLOAD_TIER_COUNT];
    Pool string_payload_pools[STRING_PAYLOAD_TIER_COUNT];
    bool pools_initialized;

    TypedArrayFreeSlot typed_array_free_cache[TYPED_ARRAY_FREE_CACHE_SLOTS];
    size_t typed_array_free_cache_bytes;

    /* Old objects a write barrier caught holding a young reference; entries are only ever
       added/deduped, never removed, and re-traced as extra roots on every minor collection
       thereafter. */
    RememberedEntry* remembered_set;
    unsigned int remembered_count, remembered_cap;
    /* Set once, forever, on the first real collection this heap ever runs -- before that,
       POOL_OLD can't be set anywhere, so the write barrier is provably a no-op. */
    bool gc_ever_collected;

    MarkWorklist gc_worklist;

    /* All in bytes. A minor runs once young_bytes, allocated since the last one, reaches
       nursery_bytes. old_bytes is what the last major left live plus everything promoted since, so
       old garbage keeps counting until a major reclaims it; a major runs once it reaches
       growth_factor times old_bytes_after_major. nursery_bytes and growth_factor come from
       aer_gc_configure; 0 for gc_live_cell_ceiling means unlimited. */
    size_t young_bytes, nursery_bytes, old_bytes, old_bytes_after_major;
    unsigned int growth_factor, gc_live_cell_ceiling;
    unsigned int minor_collections_run, major_collections_run;
    int gc_suppress_depth;

    /* Every AerDict this heap owns gets its key/sparse-array storage from here, one per heap like
       the 7 GC pools above. Chunk.name_index has no owning VM, so each Chunk carries its own
       (chunk.c). */
    HashPools dict_hash_pools;
} VmHeap;

/* Save/restore around a nested vm_init() on a fresh VM while the caller's own execution is paused
   on the C call stack (aer_module.c's aer_vm_instantiate_from_file) -- see vm_current_heap's own
   comment in heap_ref.c for why vm_run_slice's save/restore alone isn't enough here. */
VmHeap* vm_current_heap(void);
void vm_set_current_heap(VmHeap* heap);
/* Same variable, for the allocation sites with no VM in scope; promotes a NULL to the bootstrap
   heap first. All three live in heap_ref.c -- see that file for why. */
VmHeap* vm_require_current_heap(void);

/* Allocate/release a non-inline string's payload. `length` is the string's length, not the buffer
   size -- both sides derive the same size class from it, so a pooled buffer can never be plain-freed
   or vice versa. Only valid for length > AER_STRING_INLINE_MAX; shorter strings own nothing. */
char* vm_string_payload_alloc(VmHeap* heap, unsigned int length);
void vm_string_payload_free(VmHeap* heap, char* payload, unsigned int length);

/* Initializes one heap's pools -- called once per VM (vm_init), not once per process, since every
   VM owns its own. */
void vm_heap_init(VmHeap* heap);

/* Smallest tier that fits instance_bytes -- ceiling, never floor, since a cell smaller than the
   shape's own fields buffer would let AerStruct.fields (set to right after the header, in the
   same cell) run past the cell's actual allocation. */
Pool* struct_pool_for_size(VmHeap* heap, unsigned int instance_bytes);

/* Every allocation from a heap's GC-managed pools goes through here, so young_bytes (the minor
   collection's trigger) counts each cell exactly once. */
static inline void* heap_alloc(VmHeap* heap, Pool* p) {
    heap->young_bytes += p->stride;
    return pool_alloc(p);
}

/* A counter, not a flag -- imports/module-calls nest. Operates on whichever heap is current (see
   heap_ref.c's current_heap). */
void vm_gc_suppress(void);
void vm_gc_unsuppress(void);

/* From bookkeeping, not a fresh trace, so it counts garbage not yet swept -- aer_gc_stats. */
unsigned int gc_count_live_cells(VmHeap* heap);

/* Whole-heap teardown, not a sweep: frees every live cell's payload regardless of mark state. */
void gc_finalize_all_pools(VmHeap* heap);

/* Byte-accurate per-pool memory breakdown. */
void aer_debug_memory_report(FILE* out);

#endif

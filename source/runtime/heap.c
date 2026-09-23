#include <stdlib.h>
#include "aer.h"
#include "error.h"
#include "hashtable.h"
#include "heap.h"

/* Tuning defaults every freshly-initialized heap inherits -- process-wide mutable state, not
   hardcoded constants, specifically so aer_gc_configure()/aer_gc_set_ceiling() work when called
   before any VM exists yet (configure once, then create VMs that pick it up). aer_gc_configure/
   set_ceiling update these AND current_heap's own live fields, so both "configure ahead of time"
   and "reconfigure an already-running VM" work. */
static unsigned int default_nursery_bytes = 1024u * 1024;
static unsigned int default_growth_factor = 2;
static unsigned int default_gc_live_cell_ceiling = 0; /* 0 = unlimited */

/* Tier sizes for the size-classed struct pools (VmHeap.struct_pools, heap.h's own comment on why).
   Powers of two, same spirit as hashtable.c's KEY_TIER_SIZE -- the largest tier (256 = MAX_STRUCT_
   FIELDS * sizeof(AerVal)) exactly covers the worst case, so every shape's instance_bytes fits
   some tier; no malloc-fallback tier needed. pool_init itself floors any stride below 16 bytes
   (pool.c, for the free-list pointer), so the smallest tier needs no special-casing here either. */
static const size_t STRUCT_PAYLOAD_TIER_SIZE[STRUCT_PAYLOAD_TIER_COUNT] = {16, 32, 64, 128, 256};
static const unsigned int STRUCT_TIER_ELEMS_PER_SLAB[STRUCT_PAYLOAD_TIER_COUNT] = {64, 64, 64, 32, 16};

/* Smallest tier that fits instance_bytes -- ceiling, never floor, since a cell smaller than the
   shape's own fields buffer would let AerStruct.fields (set to right after the header, in the
   same cell) run past the cell's actual allocation. */
Pool* struct_pool_for_size(VmHeap* heap, unsigned int instance_bytes) {
    for (unsigned int i = 0; i < STRUCT_PAYLOAD_TIER_COUNT; i++)
        if (instance_bytes <= STRUCT_PAYLOAD_TIER_SIZE[i])
            return &heap->struct_pools[i];
    return &heap->struct_pools
                [STRUCT_PAYLOAD_TIER_COUNT -
                 1]; /* unreachable given the 256 ceiling above, but fail safe rather than out-of-bounds */
}

/* Smallest tier is 32, not 16: a payload only exists above AER_STRING_INLINE_MAX, so the smallest
   buffer this ever allocates is 17 bytes and a 16-byte tier could never be selected. */
static const size_t STRING_PAYLOAD_TIER_SIZE[STRING_PAYLOAD_TIER_COUNT] = {32, 64, 128, 256};
static const unsigned int STRING_PAYLOAD_TIER_ELEMS_PER_SLAB[STRING_PAYLOAD_TIER_COUNT] = {128, 64, 32, 16};

/* Initializes one heap's pools -- called once per VM (vm_init), not once per process, since every
   VM now owns its own. */
void vm_heap_init(VmHeap* heap) {
    if (heap->pools_initialized)
        return;
    pool_init(&heap->string_pool, sizeof(AerString), 256);
    pool_init(&heap->array_pool, sizeof(AerArray), 256);
    pool_init(&heap->dict_pool, sizeof(AerDict), 64);
    pool_init(&heap->function_pool, sizeof(AerFunction), 64);
    for (unsigned int i = 0; i < STRUCT_PAYLOAD_TIER_COUNT; i++)
        pool_init(&heap->struct_pools[i], sizeof(AerStruct) + STRUCT_PAYLOAD_TIER_SIZE[i],
                  STRUCT_TIER_ELEMS_PER_SLAB[i]);
    for (unsigned int i = 0; i < STRING_PAYLOAD_TIER_COUNT; i++)
        pool_init(&heap->string_payload_pools[i], STRING_PAYLOAD_TIER_SIZE[i],
                  STRING_PAYLOAD_TIER_ELEMS_PER_SLAB[i]);
    pool_init(&heap->packed_array_pool, sizeof(AerPackedArray), 64);
    pool_init(&heap->typed_array_pool, sizeof(AerTypedArray), 64);
    pool_init(&heap->result_pool, sizeof(AerResult), 64);
    hashtable_pools_init(&heap->dict_hash_pools);
    for (unsigned int i = 0; i < TYPED_ARRAY_FREE_CACHE_SLOTS; i++) {
        heap->typed_array_free_cache[i].size = 0;
        heap->typed_array_free_cache[i].ptr = NULL;
    }
    heap->typed_array_free_cache_bytes = 0;
    heap->nursery_bytes = default_nursery_bytes;
    heap->growth_factor = default_growth_factor;
    heap->gc_live_cell_ceiling = default_gc_live_cell_ceiling;
    heap->pools_initialized = true;
}

/* Tier owning a `length`-character payload, or -1 for the plain-malloc fallback. Both alloc and
   free derive the class through this one function, so they cannot disagree. */
static int string_payload_tier(unsigned int length) {
    size_t need = (size_t)length + 1; /* + NUL, exactly what the allocation must hold */
    for (int i = 0; i < STRING_PAYLOAD_TIER_COUNT; i++)
        if (need <= STRING_PAYLOAD_TIER_SIZE[i])
            return i;
    return -1;
}

char* vm_string_payload_alloc(VmHeap* heap, unsigned int length) {
    int tier = string_payload_tier(length);
    if (tier < 0 || !heap)
        return xmalloc((size_t)length + 1);
    vm_heap_init(heap);
    return pool_alloc(&heap->string_payload_pools[tier]);
}

void vm_string_payload_free(VmHeap* heap, char* payload, unsigned int length) {
    int tier = string_payload_tier(length);
    if (tier < 0) {
        free(payload);
        return;
    }
    /* No heap means the payload outlived its own pools, which vm_free prevents by bracketing
       gc_finalize_all_pools with current_heap. Leave it to the slab teardown rather than guess. */
    if (!heap)
        return;
    pool_free(&heap->string_payload_pools[tier], payload);
}

/* None of these take a VM* -- adding one would break every existing embedder. Suppress/unsuppress
   and aer_gc_stats act on the current heap. configure/set_ceiling also update process-wide
   defaults, since real usage calls them before any VM exists. */
void vm_gc_suppress(void) {
    vm_require_current_heap()->gc_suppress_depth++;
}
void vm_gc_unsuppress(void) {
    VmHeap* h = vm_require_current_heap();
    if (h->gc_suppress_depth > 0)
        h->gc_suppress_depth--;
}

void aer_gc_configure(unsigned int nursery_bytes, unsigned int growth_factor) {
    if (nursery_bytes)
        default_nursery_bytes = nursery_bytes;
    if (growth_factor)
        default_growth_factor = growth_factor;
    /* Also apply immediately to whichever heap is already current, if one exists -- so
       reconfiguring an already-running VM takes effect right away, not just for the next one. */
    VmHeap* heap = vm_require_current_heap();
    if (nursery_bytes)
        heap->nursery_bytes = nursery_bytes;
    if (growth_factor)
        heap->growth_factor = growth_factor;
}

void aer_gc_set_ceiling(unsigned int max_live_cells) {
    default_gc_live_cell_ceiling = max_live_cells;
    vm_require_current_heap()->gc_live_cell_ceiling = max_live_cells;
}

/* live_cells is bookkeeping, not a fresh trace, so it counts garbage not yet swept. */
void aer_gc_stats(unsigned int* live_cells, unsigned int* minor_collections,
                  unsigned int* major_collections) {
    VmHeap* heap = vm_require_current_heap();
    if (live_cells)
        *live_cells = gc_count_live_cells(heap);
    if (minor_collections)
        *minor_collections = heap->minor_collections_run;
    if (major_collections)
        *major_collections = heap->major_collections_run;
}

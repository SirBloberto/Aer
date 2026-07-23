#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"
#include "hashtable.h"
#include "pool.h"

#define HASHTABLE_INIT_SIZE 16
#define HASHTABLE_HIGH      70
#define HASHTABLE_LOW       50
#define SPARSE_EMPTY        0xFFFFFFFFu

static void rehash_sparse(HashTable* t);

/* t->capacity is always a power of two, so every probe index uses `& (capacity - 1)`. */

/* Size-classed slab pools for keys and the sparse probe array (both alloc-new/free-old, never
   realloc'd in place); sizes past the largest tier fall back to plain malloc. The dense array is
   a separate, plain xrealloc-doubling allocation (aer_collection.c's append() growth pattern) --
   its access pattern is sequential append/scan, not the random-probe pattern pooling helps with.
   Tier layout lives here (not per-HashPools-instance) since every HashPools instance uses the same
   fixed tier sizes -- only the actual Pool state differs per instance. */
/* 16-byte floor is required, not tuning — pool_free writes its free-list pointer at bytes [8,16). */
static const size_t       KEY_TIER_SIZE[HASH_KEY_TIER_COUNT]           = { 16, 32, 64, 128 };
static const unsigned int KEY_TIER_ELEMS_PER_SLAB[HASH_KEY_TIER_COUNT] = { 256, 128, 64, 32 };

static const unsigned int SPARSE_TIER_CAPACITY[HASH_SPARSE_TIER_COUNT]       = { 16, 32, 64, 128, 256 };
static const unsigned int SPARSE_TIER_ELEMS_PER_SLAB[HASH_SPARSE_TIER_COUNT] = { 64, 32, 16, 8, 4 };

void hashtable_pools_init(HashPools* pools) {
    if (pools->initialized) return;
    for (unsigned int i = 0; i < HASH_KEY_TIER_COUNT; i++)
        pool_init(&pools->key_pools[i], KEY_TIER_SIZE[i], KEY_TIER_ELEMS_PER_SLAB[i]);
    for (unsigned int i = 0; i < HASH_SPARSE_TIER_COUNT; i++)
        pool_init(&pools->sparse_pools[i], (size_t)SPARSE_TIER_CAPACITY[i] * sizeof(unsigned int), SPARSE_TIER_ELEMS_PER_SLAB[i]);
    pools->initialized = true;
}

static char* key_alloc(HashPools* pools, size_t size) {
    for (unsigned int i = 0; i < HASH_KEY_TIER_COUNT; i++)
        if (size <= KEY_TIER_SIZE[i]) return pool_alloc(&pools->key_pools[i]);
    return xmalloc(size);
}

static void key_free(HashPools* pools, char* p, size_t size) {
    for (unsigned int i = 0; i < HASH_KEY_TIER_COUNT; i++)
        if (size <= KEY_TIER_SIZE[i]) { pool_free(&pools->key_pools[i], p); return; }
    free(p);
}

static unsigned int* sparse_array_alloc(HashPools* pools, unsigned int capacity) {
    unsigned int* s;
    for (unsigned int i = 0; i < HASH_SPARSE_TIER_COUNT; i++)
        if (capacity == SPARSE_TIER_CAPACITY[i]) { s = pool_alloc(&pools->sparse_pools[i]); goto fill; }
    s = xmalloc((size_t)capacity * sizeof(unsigned int));
fill:
    /* Every byte 0xFF makes every unsigned int slot SPARSE_EMPTY (0xFFFFFFFF). */
    memset(s, 0xFF, (size_t)capacity * sizeof(unsigned int));
    return s;
}

static void sparse_array_free(HashPools* pools, unsigned int* sparse, unsigned int capacity) {
    for (unsigned int i = 0; i < HASH_SPARSE_TIER_COUNT; i++)
        if (capacity == SPARSE_TIER_CAPACITY[i]) { pool_free(&pools->sparse_pools[i], sparse); return; }
    free(sparse);
}

unsigned int hashtable_key_true_len(const char* data, unsigned int len) {
    unsigned int true_len = 0;
    while (true_len < len && data[true_len] != '\0') true_len++;
    return true_len;
}

char* hashtable_key_dup(HashPools* pools, const char* data, unsigned int len, unsigned int* out_len) {
    unsigned int true_len = hashtable_key_true_len(data, len);
    char* k = key_alloc(pools, (size_t)true_len + 1);
    memcpy(k, data, true_len);
    k[true_len] = '\0';
    if (out_len) *out_len = true_len;
    return k;
}

void hashtable_key_free(HashPools* pools, char* key, unsigned int len) {
    key_free(pools, key, (size_t)len + 1);
}

uint64_t hashtable_hash_bytes(const char* key, unsigned int length) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (unsigned int i = 0; i < length; i++) {
        hash ^= (unsigned char)key[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static bool hash_match(const HashTableEntry* entry, const char* key, unsigned int length) {
    return entry->key && entry->length == length && memcmp(entry->key, key, length) == 0;
}

static void dense_grow_if_needed(HashTable* t) {
    if (t->count < t->dense_capacity) return;
    t->dense_capacity = t->dense_capacity ? t->dense_capacity * 2 : 4;
    t->dense = xrealloc(t->dense, sizeof(HashTableEntry) * t->dense_capacity);
}

void hashtable_put(HashTable* t, char* key, unsigned int length, AerVal value) {
    hashtable_put_hashed(t, key, length, hashtable_hash_bytes(key, length), value);
}

void hashtable_put_hashed(HashTable* t, char* key, unsigned int length, uint64_t hash, AerVal value) {
    if (!t->sparse) {
        t->sparse   = sparse_array_alloc(t->pools, HASHTABLE_INIT_SIZE);
        t->capacity = HASHTABLE_INIT_SIZE;
    } else if (t->count * 100 >= HASHTABLE_HIGH * t->capacity)
        rehash_sparse(t);

    for (unsigned int i = 0; i < t->capacity; i++) {
        unsigned int* slot = &t->sparse[(hash + i) & (t->capacity - 1)];
        if (*slot != SPARSE_EMPTY && hash_match(&t->dense[*slot], key, length)) {
            /* Key already present (e.g. duplicate key in a dict literal) — just overwrite the payload. */
            hashtable_key_free(t->pools, key, length);
            t->dense[*slot].payload = value;
            return;
        }
        if (*slot == SPARSE_EMPTY) {
            dense_grow_if_needed(t);
            HashTableEntry* entry = &t->dense[t->count];
            entry->key     = key;
            entry->length  = length;
            entry->hash    = hash;
            entry->payload = value;
            *slot = t->count;
            t->count++;
            return;
        }
    }
}

AerVal* hashtable_get(HashTable* t, const char* key, unsigned int length) {
    return hashtable_get_hashed(t, key, length, hashtable_hash_bytes(key, length));
}

AerVal* hashtable_get_hashed(HashTable* t, const char* key, unsigned int length, uint64_t hash) {
    if (!t->sparse) return NULL;
    for (unsigned int i = 0; i < t->capacity; i++) {
        unsigned int slot = t->sparse[(hash + i) & (t->capacity - 1)];
        if (slot == SPARSE_EMPTY) return NULL;
        if (hash_match(&t->dense[slot], key, length)) return &t->dense[slot].payload;
    }
    return NULL;
}

/* Places a dense-array index into the (already-allocated, already-sized) sparse array using its
   entry's own cached hash -- no key comparison needed, since a fresh sparse array being rebuilt
   from scratch can never already contain the index being placed. Never rehashes itself. */
static void sparse_place(HashTable* t, unsigned int dense_idx) {
    uint64_t hash = t->dense[dense_idx].hash;
    for (unsigned int i = 0; i < t->capacity; i++) {
        unsigned int* slot = &t->sparse[(hash + i) & (t->capacity - 1)];
        if (*slot == SPARSE_EMPTY) { *slot = dense_idx; return; }
    }
}

/* Grows and rebuilds ONLY the sparse array -- the dense array (the big one) is never touched: no
   entry moves, no key is ever copied or compared during a grow. This is the mechanism that fixes
   the measured cost of growing a large table: the randomly-probed structure stays small (4 bytes/
   slot) even as the table grows into the hundreds of thousands of entries. */
static void rehash_sparse(HashTable* t) {
    unsigned int capacity = t->capacity;
    while (t->count * 100 >= HASHTABLE_LOW * capacity)
        capacity *= 2;

    unsigned int* old_sparse = t->sparse;
    unsigned int  old_capacity = t->capacity;
    t->sparse   = sparse_array_alloc(t->pools, capacity);
    t->capacity = capacity;

    for (unsigned int i = 0; i < t->count; i++)
        sparse_place(t, i);

    sparse_array_free(t->pools, old_sparse, old_capacity);
}

/* Pre-sizes for a known final count (a dict literal's pair_count, at parse time) -- skips the
   incremental one-entry-at-a-time growth hashtable_put would otherwise do, most noticeable for
   literals with more than a handful of keys (a 3-4 key dict already converges to its final dense
   capacity in one growth step either way). Never shrinks: a no-op if the table's already at least
   this big, so it's always safe to call even when the real count later turns out smaller (a
   duplicate key in the literal, say). */
void hashtable_reserve(HashTable* t, unsigned int expected_count) {
    if (t->dense_capacity < expected_count) {
        t->dense_capacity = expected_count;
        t->dense = xrealloc(t->dense, sizeof(HashTableEntry) * t->dense_capacity);
    }
    if (!t->sparse) {
        unsigned int capacity = HASHTABLE_INIT_SIZE;
        while (expected_count * 100 >= HASHTABLE_HIGH * capacity)
            capacity *= 2;
        t->sparse   = sparse_array_alloc(t->pools, capacity);
        t->capacity = capacity;
    } else if (expected_count * 100 >= HASHTABLE_HIGH * t->capacity) {
        rehash_sparse(t);
    }
}

/* Swap-compact removal, no tombstones -- matches this table's existing "reinsert the probe chain,
   never leave a dead marker" philosophy, just relocating 4-byte sparse indices instead of full
   entries. Two independent fixups: the sparse array's probe chain after the removed slot must be
   repaired (classic open-addressing deletion), and the dense array must stay hole-free by moving
   the last entry into the vacated slot. */
void hashtable_remove(HashTable* t, const char* key, unsigned int length) {
    if (!t->sparse) return;
    uint64_t hash = hashtable_hash_bytes(key, length);

    unsigned int found_slot = t->capacity;
    unsigned int removed_idx = 0;
    for (unsigned int i = 0; i < t->capacity; i++) {
        unsigned int probe = (unsigned int)((hash + i) & (t->capacity - 1));
        unsigned int slot_val = t->sparse[probe];
        if (slot_val == SPARSE_EMPTY) return;   /* not found */
        if (hash_match(&t->dense[slot_val], key, length)) { found_slot = probe; removed_idx = slot_val; break; }
    }
    if (found_slot == t->capacity) return;

    hashtable_key_free(t->pools, t->dense[removed_idx].key, t->dense[removed_idx].length);

    /* Sparse-side repair walk: clear the found slot, then reinsert the rest of its probe chain
       (same technique as before, moving indices instead of whole entries). */
    t->sparse[found_slot] = SPARSE_EMPTY;
    unsigned int pos = (found_slot + 1) & (t->capacity - 1);
    while (t->sparse[pos] != SPARSE_EMPTY) {
        unsigned int moved_idx = t->sparse[pos];
        t->sparse[pos] = SPARSE_EMPTY;
        sparse_place(t, moved_idx);
        pos = (pos + 1) & (t->capacity - 1);
    }

    /* Dense-side compaction: move the last entry into the vacated slot (unless it already was the
       last), then find and rewrite the one sparse slot that pointed at the old last index. */
    unsigned int last = t->count - 1;
    if (removed_idx != last) {
        t->dense[removed_idx] = t->dense[last];
        uint64_t moved_hash = t->dense[removed_idx].hash;
        for (unsigned int i = 0; i < t->capacity; i++) {
            unsigned int probe = (unsigned int)((moved_hash + i) & (t->capacity - 1));
            if (t->sparse[probe] == last) { t->sparse[probe] = removed_idx; break; }
        }
    }
    t->count--;
}

void hashtable_clear(HashTable* t) {
    for (unsigned int i = 0; i < t->count; i++)
        hashtable_key_free(t->pools, t->dense[i].key, t->dense[i].length);
    t->count = 0;
}

void hashtable_free(HashTable* t) {
    HashPools* pools = t->pools;
    hashtable_clear(t);
    if (t->sparse) sparse_array_free(pools, t->sparse, t->capacity);
    free(t->dense);
    *t = (HashTable){0};
}

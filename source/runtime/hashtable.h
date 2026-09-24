#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <stdbool.h>
#include <stdint.h>
#include "pool.h"
#include "value.h"

/* Key and probe-array storage for the tables one owner creates. Never process-global: a table can
   belong to a VM's heap (an AerDict) or to no VM at all (a Chunk's name_index, which outlives any
   of them), and a key duped from one HashPools and freed into another corrupts both. */
#define HASH_KEY_TIER_COUNT 4
#define HASH_SPARSE_TIER_COUNT 5
#define HASH_DENSE_TIER_COUNT 6
typedef struct {
    Pool key_pools[HASH_KEY_TIER_COUNT];
    Pool sparse_pools[HASH_SPARSE_TIER_COUNT];
    Pool dense_pools[HASH_DENSE_TIER_COUNT];
    bool initialized;
} HashPools;

/* FNV-1a, 32-bit on purpose: it fits the padding AerString already rounds up to, so a key can
   memoize its own hash for free. */
typedef uint32_t HashValue;

/* Idempotent. */
void hashtable_pools_init(HashPools* pools);
void hashtable_pools_free(HashPools* pools);

typedef struct {
    char* key; /* owned; the AerVal payload's heap cells belong to the GC */
    unsigned int length;
    HashValue hash; /* cached at insertion, so a rehash never recomputes it */
    AerVal payload;
} HashTableEntry;

/* A small sparse array of probe indices pointing into a dense array of entries packed in insertion
   order. Growing touches only the sparse side -- dense entries are appended, never moved, which is
   what lets AerDict's card marking index them by dense position. */
typedef struct {
    HashTableEntry* dense; /* packed [0, count), no holes */
    unsigned int* sparse; /* UINT32_MAX = empty, else a dense index */
    unsigned int count;
    unsigned int dense_capacity;
    unsigned int capacity; /* of sparse; power of two */
    HashPools* pools;
} HashTable;

/* `length` must be the key's TRUE length (hashtable_key_true_len). get/put/remove disagreeing on it
   for the same bytes makes a lookup silently miss its own insert. */
void hashtable_put(HashTable* t, char* key, unsigned int length, AerVal value);
AerVal* hashtable_get(HashTable* t, const char* key, unsigned int length);
void hashtable_remove(HashTable* t, const char* key, unsigned int length);
void hashtable_clear(HashTable* t);
void hashtable_free(HashTable* t);

/* Pre-sizes both arrays for a count known up front, skipping incremental growth. Never shrinks. */
void hashtable_reserve(HashTable* t, unsigned int expected_count);

/* For a caller holding a cached hash. It MUST equal hashtable_hash_bytes(key, length), or this
   table's probe sequence disagrees with the plain versions' and lookups corrupt. */
void hashtable_put_hashed(HashTable* t, char* key, unsigned int length, HashValue hash, AerVal value);
AerVal* hashtable_get_hashed(HashTable* t, const char* key, unsigned int length, HashValue hash);

/* The entry's dense index, or -1 -- what the GC write barrier needs instead of a payload pointer. */
int hashtable_get_index_hashed(HashTable* t, const char* key, unsigned int length, HashValue hash);

HashValue hashtable_hash_bytes(const char* key, unsigned int length);

/* Every owned key copy goes through this pair: they use size-class pools, so alloc size must equal
   free size. Truncates at the first embedded NUL. */
char* hashtable_key_dup(HashPools* pools, const char* data, unsigned int len, unsigned int* out_len);
char* hashtable_key_dup_known(HashPools* pools, const char* data, unsigned int true_len);
void hashtable_key_free(HashPools* pools, char* key, unsigned int len);

/* Where hashtable_key_dup would truncate, for a caller that needs the length before deciding
   whether to look up or insert. */
unsigned int hashtable_key_true_len(const char* data, unsigned int len);

/* Strings are immutable after allocation, so the memo cannot go stale; a truncated key opts out
   rather than caching a hash its own length disagrees with. */
static inline HashValue hashtable_string_hash(AerString* s, unsigned int true_len) {
    if (true_len != s->length)
        return hashtable_hash_bytes(s->data, true_len);
    if (s->hash == 0)
        s->hash = hashtable_hash_bytes(s->data, true_len);
    return s->hash;
}

#endif

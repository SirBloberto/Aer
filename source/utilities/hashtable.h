#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <stdbool.h>
#include <stdint.h>
#include "pool.h"
#include "value.h"

/* Every hashtable's key storage and sparse probe array come from a HashPools instance -- never a
   single process-global set, since a HashTable can belong to a specific VM's own heap (an AerDict)
   or have no owning VM at all (a Chunk's name_index, which outlives/exists independently of any
   one VM). Each owner supplies its own HashPools (a VM's own, embedded in VmHeap; a process-global
   one for Chunk.name_index) and every HashTable records which one it was created with. */
#define HASH_KEY_TIER_COUNT 4
#define HASH_SPARSE_TIER_COUNT 5
#define HASH_DENSE_TIER_COUNT 6
typedef struct {
    Pool key_pools[HASH_KEY_TIER_COUNT];
    Pool sparse_pools[HASH_SPARSE_TIER_COUNT];
    Pool dense_pools[HASH_DENSE_TIER_COUNT];
    bool initialized;
} HashPools;

/* Idempotent -- safe to call every time a HashPools might not be initialized yet. */
void hashtable_pools_init(HashPools* pools);

/* Compact hashtable backing Chunk's name_index and AerDict: a small sparse array of probe indices
   (cache-resident even at large table sizes) pointing into a dense array of the actual entries,
   packed in insertion order with no holes. Growing the table only ever touches the sparse array --
   the dense array's entries are never moved by a rehash, only appended to. The table owns only the
   key; the AerVal payload's heap cells belong to the GC. `hash` is cached at insertion so a rehash
   or remove's repair walk never recomputes it. */
typedef struct {
    char* key;
    unsigned int length;
    uint64_t hash;
    AerVal payload;
} HashTableEntry;

typedef struct {
    HashTableEntry* dense; /* insertion-appended, packed [0, count), no holes */
    unsigned int* sparse; /* capacity-sized probe array; UINT32_MAX = empty,
                                        otherwise a dense-array index */
    unsigned int count; /* live entries -- also dense's used length */
    unsigned int dense_capacity; /* allocated length of dense -- grows independently of capacity */
    unsigned int capacity; /* allocated length of sparse; power of two */
    HashPools* pools; /* set once at creation -- see HashPools' own comment above */
    /* Keys point into permanent constant-pool storage instead of being owned, so none are freed.
       Set only by hashtable_put_borrowed; any owning mutation calls adopt_keys first, so a table
       is never half-borrowed. Fits existing padding -- HashTable does not grow. */
    bool keys_borrowed;
} HashTable;

/* `length` must be the key's TRUE length -- i.e. already truncated at any embedded NUL via
   hashtable_key_true_len below, the same length hashtable_key_dup would produce. get/put/remove
   must all agree on this same truncated length for a given key's bytes, or a lookup and its own
   prior insert can silently disagree (get/put pairs originating from the same source bytes should
   always call hashtable_key_true_len once and reuse the result for both). */
void hashtable_put(HashTable* t, char* key, unsigned int length, AerVal value);
AerVal* hashtable_get(HashTable* t, const char* key, unsigned int length);
void hashtable_remove(HashTable* t, const char* key, unsigned int length);
void hashtable_clear(HashTable* t);
void hashtable_free(HashTable* t);

/* Pre-sizes both the sparse and dense arrays for an expected final entry count known up front (a
   dict literal's pair_count, at parse time) -- t->pools must already be set. Skips the incremental
   growth every hashtable_put would otherwise do one entry at a time; a no-op if the table's
   already at least this big. Never shrinks anything. */
void hashtable_reserve(HashTable* t, unsigned int expected_count);

/* Same contract as the plain versions above, but the caller has already computed (or cached) the
   key's hash itself -- e.g. an AerString reused as a dict key many times, or an existing
   HashTableEntry's own cached .hash when copying it into another table -- and skips this table's
   own hash_bytes() call. `hash` MUST equal hash_bytes(key, length) exactly, or this table's probe
   sequence silently disagrees with a plain hashtable_get/put's, corrupting lookups. The plain
   versions are defined in terms of these, not the other way around. */
void hashtable_put_hashed(HashTable* t, char* key, unsigned int length, uint64_t hash, AerVal value);
AerVal* hashtable_get_hashed(HashTable* t, const char* key, unsigned int length, uint64_t hash);

/* Inserts without copying `key`, which must outlive this table -- a constant-pool AerString's
   bytes, kept alive permanently by mark_chunk_roots. Only valid while the table owns no keys. */
void hashtable_put_borrowed(HashTable* t, const char* key, unsigned int length, uint64_t hash,
                            AerVal value);

/* Same probe as hashtable_get_hashed, but returns the entry's dense-array index (or -1) instead of
   a payload pointer -- see its own comment, hashtable.c, for why the GC's write barrier needs this. */
int hashtable_get_index_hashed(HashTable* t, const char* key, unsigned int length, uint64_t hash);

/* The exact hash function every HashTable in this codebase uses -- exposed so a caller can
   precompute (and cache) a hash to pass to the _hashed calls above, using the identical algorithm
   this file's own internal hash_key() already used before this existed. */
uint64_t hashtable_hash_bytes(const char* key, unsigned int length);

/* Every owned key copy must go through this pair (they use `pools`' size-class pools, and the pool
   lookup needs alloc size == free size). Truncates at the first embedded NUL; `len` is the
   strlen-equivalent, the NUL is accounted for internally. `pools` must match whatever HashTable the
   key will eventually be put into (or was removed from) -- a key duped from one HashPools and
   freed into another would corrupt both. */
char* hashtable_key_dup(HashPools* pools, const char* data, unsigned int len, unsigned int* out_len);
void hashtable_key_free(HashPools* pools, char* key, unsigned int len);

/* The length hashtable_key_dup would truncate `data`/`len` to (first embedded NUL, or `len`
   itself if none). A caller that needs the true length before deciding whether to look up or
   insert -- rather than going through hashtable_key_dup, which only reports it after copying --
   calls this directly. */
unsigned int hashtable_key_true_len(const char* data, unsigned int len);

#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"
#include "hashtable.h"
#include "pool.h"

#define HASHTABLE_INIT_SIZE 16
#define HASHTABLE_HIGH      70
#define HASHTABLE_LOW       50

static void rehash(HashTable* t);

/* t->capacity is always a power of two (starts at HASHTABLE_INIT_SIZE, only ever doubles in
   rehash), so every probe index below uses `& (capacity - 1)` instead of `% capacity`. */

/* Size-classed slab pools for the two payload categories that never grow in place — a dict's
   bucket array is always alloc-new+copy+free-old on growth (rehash below never reallocs in place),
   and a key is alloc-once/free-once for its whole life (rehash moves ownership by pointer, never
   re-copies). Reuses pool.c's existing slab mechanism verbatim; not a general-purpose arena — a
   size beyond the largest tier falls back to plain xmalloc/xcalloc/free, unchanged from before
   these pools existed. String/array payloads are explicitly out of scope: they grow via xrealloc,
   which can extend in place, a property this alloc-new+copy+free-old scheme can't replicate without
   risking a regression there. */
#define KEY_TIER_COUNT 4
/* 16-byte floor is not a tuning choice — pool_free (pool.c) writes a free-list pointer at
   [sizeof(void*), 2*sizeof(void*)), which is [8,16) on a 64-bit build; anything smaller is unsafe. */
static const size_t       KEY_TIER_SIZE[KEY_TIER_COUNT]           = { 16, 32, 64, 128 };
static const unsigned int KEY_TIER_ELEMS_PER_SLAB[KEY_TIER_COUNT] = { 256, 128, 64, 32 };
static Pool key_pools[KEY_TIER_COUNT];

/* 16 = HASHTABLE_INIT_SIZE exactly (every table's first allocation); doubling matches rehash()'s
   own growth; 256 * HASHTABLE_HIGH% =~ 179 live entries before another rehash, covering small/
   medium dicts. Byte size per tier is capacity * sizeof(HashTableEntry), computed at pool_init
   time below (never hardcoded — sizeof(HashTableEntry) varies by target: 24 bytes on 32-bit ARM,
   32 on a 64-bit dev box). */
#define BUCKET_TIER_COUNT 5
static const unsigned int BUCKET_TIER_CAPACITY[BUCKET_TIER_COUNT]       = { 16, 32, 64, 128, 256 };
static const unsigned int BUCKET_TIER_ELEMS_PER_SLAB[BUCKET_TIER_COUNT] = { 64, 32, 16, 8, 4 };
static Pool bucket_pools[BUCKET_TIER_COUNT];

static bool hashtable_pools_initialized = false;

void hashtable_pools_init_once(void) {
    if (hashtable_pools_initialized) return;
    for (unsigned int i = 0; i < KEY_TIER_COUNT; i++)
        pool_init(&key_pools[i], KEY_TIER_SIZE[i], KEY_TIER_ELEMS_PER_SLAB[i]);
    for (unsigned int i = 0; i < BUCKET_TIER_COUNT; i++)
        pool_init(&bucket_pools[i], (size_t)BUCKET_TIER_CAPACITY[i] * sizeof(HashTableEntry), BUCKET_TIER_ELEMS_PER_SLAB[i]);
    hashtable_pools_initialized = true;
}

static char* key_alloc(size_t size) {
    for (unsigned int i = 0; i < KEY_TIER_COUNT; i++)
        if (size <= KEY_TIER_SIZE[i]) return pool_alloc(&key_pools[i]);
    return xmalloc(size);
}

static void key_free(char* p, size_t size) {
    for (unsigned int i = 0; i < KEY_TIER_COUNT; i++)
        if (size <= KEY_TIER_SIZE[i]) { pool_free(&key_pools[i], p); return; }
    free(p);
}

static HashTableEntry* bucket_array_alloc(unsigned int capacity) {
    for (unsigned int i = 0; i < BUCKET_TIER_COUNT; i++)
        if (capacity == BUCKET_TIER_CAPACITY[i]) {
            HashTableEntry* b = pool_alloc(&bucket_pools[i]);
            /* pool_alloc returns uninitialized memory (pool.h) — key==NULL meaning "empty slot" is
               semantically required here, unlike xcalloc's zero-init this replaces, so it must be
               explicit. */
            memset(b, 0, (size_t)capacity * sizeof(HashTableEntry));
            return b;
        }
    return xcalloc(capacity, sizeof(HashTableEntry));
}

static void bucket_array_free(HashTableEntry* buckets, unsigned int capacity) {
    for (unsigned int i = 0; i < BUCKET_TIER_COUNT; i++)
        if (capacity == BUCKET_TIER_CAPACITY[i]) { pool_free(&bucket_pools[i], buckets); return; }
    free(buckets);
}

char* hashtable_key_dup(const char* data, unsigned int len, unsigned int* out_len) {
    unsigned int true_len = 0;
    while (true_len < len && data[true_len] != '\0') true_len++;
    char* k = key_alloc((size_t)true_len + 1);
    memcpy(k, data, true_len);
    k[true_len] = '\0';
    if (out_len) *out_len = true_len;
    return k;
}

void hashtable_key_free(char* key, unsigned int len) {
    key_free(key, (size_t)len + 1);
}

static uint64_t hash_key(const char* key) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (; *key; key++) {
        hash ^= (unsigned char)*key;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static bool hash_match(const char* entry_key, unsigned int entry_length,
                        const char* key, unsigned int length) {
    return entry_key && entry_length == length && strcmp(entry_key, key) == 0;
}

void hashtable_put(HashTable* t, char* key, AerVal value) {
    if (!t->buckets) {
        t->buckets  = bucket_array_alloc(HASHTABLE_INIT_SIZE);
        t->capacity = HASHTABLE_INIT_SIZE;
    } else if (t->count * 100 >= HASHTABLE_HIGH * t->capacity)
        rehash(t);

    uint64_t hash   = hash_key(key);
    unsigned int        length = (unsigned int)strlen(key);

    for (unsigned int i = 0; i < t->capacity; i++) {
        HashTableEntry* entry = &t->buckets[(hash + i) & (t->capacity - 1)];
        if (hash_match(entry->key, entry->length, key, length)) {
            /* Key already present (e.g. duplicate key in a dict literal) — just overwrite the payload. */
            hashtable_key_free(key, length);
            entry->payload = value;
            return;
        }
        if (entry->key == NULL) {
            entry->key     = key;
            entry->payload = value;
            entry->length  = length;
            t->count++;
            return;
        }
    }
}

AerVal* hashtable_get(HashTable* t, const char* key) {
    if (!t->buckets) return NULL;
    uint64_t hash   = hash_key(key);
    unsigned int        length = (unsigned int)strlen(key);
    for (unsigned int i = 0; i < t->capacity; i++) {
        HashTableEntry* entry = &t->buckets[(hash + i) & (t->capacity - 1)];
        if (hash_match(entry->key, entry->length, key, length)) return &entry->payload;
        if (entry->key == NULL) return NULL;
    }
    return NULL;
}

/* Inserts into buckets already sized for it, never growing/rehashing — used only by
   hashtable_remove's repair loop below, where t->capacity must stay fixed across every
   reinsertion (rehash mid-loop would strand pos/cap in the old array's coordinates). */
static void hashtable_put_raw(HashTable* t, char* key, AerVal value) {
    uint64_t hash   = hash_key(key);
    unsigned int length = (unsigned int)strlen(key);
    for (unsigned int i = 0; i < t->capacity; i++) {
        HashTableEntry* entry = &t->buckets[(hash + i) & (t->capacity - 1)];
        if (entry->key == NULL) {
            entry->key     = key;
            entry->payload = value;
            entry->length  = length;
            t->count++;
            return;
        }
    }
}

void hashtable_remove(HashTable* t, const char* key) {
    if (!t->buckets) return;
    uint64_t hash   = hash_key(key);
    unsigned int        length = (unsigned int)strlen(key);
    unsigned int        cap    = t->capacity;
    unsigned int        start  = (unsigned int)(hash & (cap - 1));

    /* Find the entry */
    unsigned int found = cap;
    for (unsigned int i = 0; i < cap; i++) {
        unsigned int idx = (start + i) & (cap - 1);
        if (!t->buckets[idx].key) return;   /* hit an empty slot — not found */
        if (hash_match(t->buckets[idx].key, t->buckets[idx].length, key, length)) { found = idx; break; }
    }
    if (found == cap) return;

    /* Free and clear the slot */
    hashtable_key_free(t->buckets[found].key, t->buckets[found].length);
    t->buckets[found] = (HashTableEntry){0};
    t->count--;

    /* Reinsert entries in the probe chain that may now be unreachable: advance to the next empty slot, removing and reinserting each to restore the invariant. hashtable_put_raw (not hashtable_put) is load-bearing here — it never rehashes, so cap/pos stay valid against t->buckets for the whole loop. */
    unsigned int pos = (found + 1) & (cap - 1);
    while (t->buckets[pos].key) {
        HashTableEntry e = t->buckets[pos];
        t->buckets[pos] = (HashTableEntry){0};
        t->count--;
        hashtable_put_raw(t, e.key, e.payload);
        pos = (pos + 1) & (cap - 1);
    }
}

void hashtable_clear(HashTable* t) {
    if (!t->buckets) return;
    for (unsigned int i = 0; i < t->capacity; i++) {
        HashTableEntry* e = &t->buckets[i];
        if (e->key) {
            hashtable_key_free(e->key, e->length);
            *e = (HashTableEntry){0};
        }
    }
    t->count = 0;
}

void hashtable_free(HashTable* t) {
    hashtable_clear(t);
    bucket_array_free(t->buckets, t->capacity);
    *t = (HashTable){0};
}

static void rehash(HashTable* t) {
    unsigned int capacity = t->capacity;
    while (t->count * 100 >= HASHTABLE_LOW * capacity)
        capacity *= 2;

    HashTable copy = *t;
    copy.buckets  = bucket_array_alloc(capacity);
    copy.capacity = capacity;
    copy.count    = 0;

    for (unsigned int i = 0; i < t->capacity; i++) {
        HashTableEntry* entry = &t->buckets[i];
        if (entry->key)
            hashtable_put(&copy, entry->key, entry->payload);
    }
    bucket_array_free(t->buckets, t->capacity);
    *t = copy;
}

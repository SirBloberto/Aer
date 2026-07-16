#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"
#include "hashtable.h"

#define HASHTABLE_INIT_SIZE 16
#define HASHTABLE_HIGH      70
#define HASHTABLE_LOW       50

static void rehash(HashTable* t);

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
        t->buckets  = xcalloc(HASHTABLE_INIT_SIZE, sizeof(HashTableEntry));
        t->capacity = HASHTABLE_INIT_SIZE;
    } else if ((t->count * 100) / t->capacity >= HASHTABLE_HIGH)
        rehash(t);

    uint64_t hash   = hash_key(key);
    unsigned int        length = (unsigned int)strlen(key);

    for (unsigned int i = 0; i < t->capacity; i++) {
        HashTableEntry* entry = &t->buckets[(hash + i) % t->capacity];
        if (hash_match(entry->key, entry->length, key, length)) {
            /* Key already present (e.g. duplicate key in a dict literal) — just overwrite the payload. */
            free(key);
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
        HashTableEntry* entry = &t->buckets[(hash + i) % t->capacity];
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
        HashTableEntry* entry = &t->buckets[(hash + i) % t->capacity];
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
    unsigned int        start  = (unsigned int)(hash % cap);

    /* Find the entry */
    unsigned int found = cap;
    for (unsigned int i = 0; i < cap; i++) {
        unsigned int idx = (start + i) % cap;
        if (!t->buckets[idx].key) return;   /* hit an empty slot — not found */
        if (hash_match(t->buckets[idx].key, t->buckets[idx].length, key, length)) { found = idx; break; }
    }
    if (found == cap) return;

    /* Free and clear the slot */
    free(t->buckets[found].key);
    t->buckets[found] = (HashTableEntry){0};
    t->count--;

    /* Reinsert entries in the probe chain that may now be unreachable: advance to the next empty slot, removing and reinserting each to restore the invariant. hashtable_put_raw (not hashtable_put) is load-bearing here — it never rehashes, so cap/pos stay valid against t->buckets for the whole loop. */
    unsigned int pos = (found + 1) % cap;
    while (t->buckets[pos].key) {
        HashTableEntry e = t->buckets[pos];
        t->buckets[pos] = (HashTableEntry){0};
        t->count--;
        hashtable_put_raw(t, e.key, e.payload);
        pos = (pos + 1) % cap;
    }
}

void hashtable_clear(HashTable* t) {
    if (!t->buckets) return;
    for (unsigned int i = 0; i < t->capacity; i++) {
        HashTableEntry* e = &t->buckets[i];
        if (e->key) {
            free(e->key);
            *e = (HashTableEntry){0};
        }
    }
    t->count = 0;
}

void hashtable_free(HashTable* t) {
    hashtable_clear(t);
    free(t->buckets);
    *t = (HashTable){0};
}

static void rehash(HashTable* t) {
    unsigned int capacity = t->capacity;
    while ((t->count * 100) / capacity >= HASHTABLE_LOW)
        capacity *= 2;

    HashTable copy = *t;
    copy.buckets  = xcalloc(capacity, sizeof(HashTableEntry));
    copy.capacity = capacity;
    copy.count    = 0;

    for (unsigned int i = 0; i < t->capacity; i++) {
        HashTableEntry* entry = &t->buckets[i];
        if (entry->key)
            hashtable_put(&copy, entry->key, entry->payload);
    }
    free(t->buckets);
    *t = copy;
}

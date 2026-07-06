#include <stdlib.h>
#include <string.h>
#include "error.h"
#include "hash_util.h"
#include "hashtable.h"

#define HASHTABLE_INIT_SIZE 16
#define HASHTABLE_HIGH      70
#define HASHTABLE_LOW       50

static void rehash(HashTable* t);

/* The only real difference between boxed and inline tables: boxed payloads own a separate allocation to free on overwrite/remove/clear; inline owns nothing. */
static void free_payload(HashTable* t, HashPayload p) {
    if (!t->is_inline) free(p.boxed);
}

void hashtable_put(HashTable* t, char* key, HashPayload value) {
    if (!t->buckets) {
        t->buckets  = xcalloc(HASHTABLE_INIT_SIZE, sizeof(HashTableEntry));
        t->capacity = HASHTABLE_INIT_SIZE;
    } else if ((t->count * 100) / t->capacity >= HASHTABLE_HIGH)
        rehash(t);

    unsigned long long hash   = hash_key(key);
    unsigned int        length = (unsigned int)strlen(key);

    for (unsigned int i = 0; i < t->capacity; i++) {
        HashTableEntry* entry = &t->buckets[(hash + i) % t->capacity];
        if (hash_match(entry->key, entry->length, key, length)) {
            /* Key already present (e.g. duplicate key in a dict literal): caller's key duplicates the entry's own, and the old payload is about to be orphaned. */
            free_payload(t, entry->payload);
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

HashPayload* hashtable_get(HashTable* t, const char* key) {
    if (!t->buckets) return NULL;
    unsigned long long hash   = hash_key(key);
    unsigned int        length = (unsigned int)strlen(key);
    for (unsigned int i = 0; i < t->capacity; i++) {
        HashTableEntry* entry = &t->buckets[(hash + i) % t->capacity];
        if (hash_match(entry->key, entry->length, key, length)) return &entry->payload;
        if (entry->key == NULL) return NULL;
    }
    return NULL;
}

void hashtable_remove(HashTable* t, const char* key) {
    if (!t->buckets) return;
    unsigned long long hash   = hash_key(key);
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
    free_payload(t, t->buckets[found].payload);
    t->buckets[found] = (HashTableEntry){0};
    t->count--;

    /* Reinsert entries in the probe chain that may now be unreachable: advance to the next empty slot, removing and reinserting each to restore the invariant. */
    unsigned int pos = (found + 1) % cap;
    while (t->buckets[pos].key) {
        HashTableEntry e = t->buckets[pos];
        t->buckets[pos] = (HashTableEntry){0};
        t->count--;
        hashtable_put(t, e.key, e.payload);
        pos = (pos + 1) % cap;
    }
}

void hashtable_clear(HashTable* t) {
    if (!t->buckets) return;
    for (unsigned int i = 0; i < t->capacity; i++) {
        HashTableEntry* e = &t->buckets[i];
        if (e->key) {
            free(e->key);
            free_payload(t, e->payload);
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

    /* Copies is_inline (and every other field) forward automatically, so the grown copy stays in whatever mode this table was already in. */
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

#ifndef HASHTABLE_H
#define HASHTABLE_H

#include "value.h"

/* Open-addressing string-keyed table backing both Chunk's name_index and AerDict; payload is a
   plain AerVal stored inline in the bucket, never boxed — string/array/dict/function payloads are
   heap cells owned by the GC, so the table never needs to free one, only the key. */
typedef struct {
    char*        key;
    unsigned int length;
    AerVal       payload;
} HashTableEntry;

typedef struct {
    HashTableEntry* buckets;
    unsigned int    count;
    unsigned int    capacity;
} HashTable;

void    hashtable_put(HashTable* t, char* key, AerVal value);
AerVal* hashtable_get(HashTable* t, const char* key);
void    hashtable_remove(HashTable* t, const char* key);
void    hashtable_clear(HashTable* t);
void    hashtable_free(HashTable* t);

/* Every owned key copy backing either a Chunk's name_index or an AerDict goes through this pair,
   not a bare xmalloc/memcpy — see hashtable.c's size-classed key/bucket-array pools. Truncates at
   the first embedded NUL byte: hash_match already compares keys via strlen+strcmp, so bytes past a
   NUL are already invisible to every get/put/remove — truncating here keeps the allocated size and
   hashtable_put's own strlen-derived entry->length identical, which the pool's size-class lookup
   depends on. `len` is the strlen-equivalent (entry->length), not len+1 — the NUL terminator is
   accounted for internally. */
char* hashtable_key_dup(const char* data, unsigned int len, unsigned int* out_len);
void  hashtable_key_free(char* key, unsigned int len);

/* Idempotent; must be called (directly or via vm_pools_init_once) before any hashtable_put. */
void  hashtable_pools_init_once(void);

#endif

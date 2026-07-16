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

#endif

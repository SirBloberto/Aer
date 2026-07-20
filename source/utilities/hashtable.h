#ifndef HASHTABLE_H
#define HASHTABLE_H

#include "value.h"

/* Open-addressing string-keyed table backing Chunk's name_index and AerDict. The table owns only
   the key; the AerVal payload's heap cells belong to the GC. */
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

/* Every owned key copy must go through this pair (they use hashtable.c's size-class pools, and
   the pool lookup needs alloc size == free size). Truncates at the first embedded NUL; `len` is
   the strlen-equivalent, the NUL is accounted for internally. */
char* hashtable_key_dup(const char* data, unsigned int len, unsigned int* out_len);
void  hashtable_key_free(char* key, unsigned int len);

/* Idempotent; must be called (directly or via vm_pools_init_once) before any hashtable_put. */
void  hashtable_pools_init_once(void);

#endif

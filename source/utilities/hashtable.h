#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <stdbool.h>
#include "value.h"

/* Shared open-addressing engine behind HashMap (boxed) and DictMap (inline) — same probing/load-factor/rehash; boxed payloads survive a rehash for callers retaining a pointer (e.g. Chunk's name_index), inline is safe when nothing retains a pointer across mutation (AerDict). is_inline defaults false on zero-init like HashMap; only lbl_dict_new (vm.c) opts a DictMap into inline mode. */
typedef union {
    void*  boxed;
    AerVal inline_val;
} HashPayload;

typedef struct {
    char*        key;
    unsigned int length;
    HashPayload  payload;
} HashTableEntry;

typedef struct {
    HashTableEntry* buckets;
    unsigned int    count;
    unsigned int    capacity;
    bool            is_inline;
} HashTable;

void         hashtable_put(HashTable* t, char* key, HashPayload value);
HashPayload* hashtable_get(HashTable* t, const char* key);
void         hashtable_remove(HashTable* t, const char* key);
void         hashtable_clear(HashTable* t);
void         hashtable_free(HashTable* t);

#endif

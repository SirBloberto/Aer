#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <stdint.h>
#include "value.h"

/* Compact hashtable backing Chunk's name_index and AerDict: a small sparse array of probe indices
   (cache-resident even at large table sizes) pointing into a dense array of the actual entries,
   packed in insertion order with no holes. Growing the table only ever touches the sparse array —
   the dense array's entries are never moved by a rehash, only appended to. The table owns only the
   key; the AerVal payload's heap cells belong to the GC. `hash` is cached at insertion so a rehash
   or remove's repair walk never recomputes it. */
typedef struct {
    char*        key;
    unsigned int length;
    uint64_t     hash;
    AerVal       payload;
} HashTableEntry;

typedef struct {
    HashTableEntry* dense;          /* insertion-appended, packed [0, count), no holes */
    unsigned int*   sparse;         /* capacity-sized probe array; UINT32_MAX = empty,
                                        otherwise a dense-array index */
    unsigned int    count;          /* live entries -- also dense's used length */
    unsigned int    dense_capacity; /* allocated length of dense -- grows independently of capacity */
    unsigned int    capacity;       /* allocated length of sparse; power of two */
} HashTable;

/* `length` must be the key's TRUE length -- i.e. already truncated at any embedded NUL via
   hashtable_key_true_len below, the same length hashtable_key_dup would produce. get/put/remove
   must all agree on this same truncated length for a given key's bytes, or a lookup and its own
   prior insert can silently disagree (get/put pairs originating from the same source bytes should
   always call hashtable_key_true_len once and reuse the result for both). */
void    hashtable_put(HashTable* t, char* key, unsigned int length, AerVal value);
AerVal* hashtable_get(HashTable* t, const char* key, unsigned int length);
void    hashtable_remove(HashTable* t, const char* key, unsigned int length);
void    hashtable_clear(HashTable* t);
void    hashtable_free(HashTable* t);

/* Every owned key copy must go through this pair (they use hashtable.c's size-class pools, and
   the pool lookup needs alloc size == free size). Truncates at the first embedded NUL; `len` is
   the strlen-equivalent, the NUL is accounted for internally. */
char* hashtable_key_dup(const char* data, unsigned int len, unsigned int* out_len);
void  hashtable_key_free(char* key, unsigned int len);

/* The length hashtable_key_dup would truncate `data`/`len` to (first embedded NUL, or `len`
   itself if none). A caller that needs the true length before deciding whether to look up or
   insert -- rather than going through hashtable_key_dup, which only reports it after copying --
   calls this directly. */
unsigned int hashtable_key_true_len(const char* data, unsigned int len);

/* Idempotent; must be called (directly or via vm_pools_init_once) before any hashtable_put. */
void  hashtable_pools_init_once(void);

#endif

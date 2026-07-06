#include <stddef.h>
#include "hashmap.h"

void hashmap_put(HashMap* map, char* key, void* value) {
    HashPayload p;
    p.boxed = value;
    hashtable_put(map, key, p);
}

void* hashmap_get(HashMap* map, const char* key) {
    HashPayload* p = hashtable_get(map, key);
    return p ? p->boxed : NULL;
}

void hashmap_free(HashMap* map) { hashtable_free(map); }

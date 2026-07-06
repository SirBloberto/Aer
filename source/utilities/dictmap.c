#include <stddef.h>
#include "dictmap.h"

void dictmap_put(DictMap* map, char* key, AerVal value) {
    HashPayload p;
    p.inline_val = value;
    hashtable_put(map, key, p);
}

AerVal* dictmap_get(DictMap* map, const char* key) {
    HashPayload* p = hashtable_get(map, key);
    return p ? &p->inline_val : NULL;
}

void dictmap_remove(DictMap* map, const char* key) { hashtable_remove(map, key); }
void dictmap_free(DictMap* map)                    { hashtable_free(map); }

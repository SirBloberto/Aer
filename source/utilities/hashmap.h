#ifndef HASHMAP_H
#define HASHMAP_H

#include "hashtable.h"

/* HashMap = HashTable in boxed mode (the zero-initialized default); its own type name just prevents mixing it up with DictMap at call sites — see hashtable.h/dictmap.h. */
typedef HashTable HashMap;

void  hashmap_put(HashMap* map, char* key, void* value);
void* hashmap_get(HashMap* map, const char* key);
void  hashmap_free(HashMap* map);

#endif

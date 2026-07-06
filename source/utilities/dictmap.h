#ifndef DICTMAP_H
#define DICTMAP_H

#include "hashtable.h"
#include "value.h"

/* DictMap = HashTable in inline mode (dict values never outlive a mutation, so entries hold the Value directly); set in lbl_dict_new (vm.c) — see hashtable.h/hashmap.h for boxed vs inline. */
typedef HashTable DictMap;

void    dictmap_put(DictMap* map, char* key, AerVal value);
AerVal* dictmap_get(DictMap* map, const char* key);
void    dictmap_remove(DictMap* map, const char* key);
void    dictmap_free(DictMap* map);

#endif

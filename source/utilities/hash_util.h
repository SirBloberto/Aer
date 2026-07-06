#ifndef HASH_UTIL_H
#define HASH_UTIL_H

#include <stdbool.h>

/* Shared by hashmap.c and dictmap.c: same open-addressing table, differing only in boxed vs inline payload, and hashing/matching never touch the payload — so one copy suffices for both. */
unsigned long long hash_key(const char* key);
bool hash_match(const char* entry_key, unsigned int entry_length,
                 const char* key, unsigned int length);

#endif

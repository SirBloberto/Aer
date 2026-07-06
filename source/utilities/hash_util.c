#include <string.h>
#include "hash_util.h"

unsigned long long hash_key(const char* key) {
    unsigned long long hash = 0xcbf29ce484222325ULL;
    for (; *key; key++) {
        hash ^= (unsigned char)*key;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

bool hash_match(const char* entry_key, unsigned int entry_length,
                 const char* key, unsigned int length) {
    return entry_key && entry_length == length && strcmp(entry_key, key) == 0;
}

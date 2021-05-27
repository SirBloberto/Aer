#ifndef MEMORY_H
#define MEMORY_H

#include "Value.h"

struct TableEntry;

typedef struct Table {
    struct TableEntry** table;
    unsigned int size;
} Table;

typedef struct TableEntry {
    char* key;
    struct Value value;
    TableEntry* next;
} TableEntry;

#endif
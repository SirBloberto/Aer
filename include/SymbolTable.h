#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "Value.h"

typedef struct SymbolTable {
    char* identifier;
    Value value;
} SymbolTable;

int AddSymbol(char* identifier);

int FindSymbol(char* identifier);

#endif
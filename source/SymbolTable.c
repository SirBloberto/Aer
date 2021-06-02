#include <string.h>
#include "../include/Compiler.h"
#include "../include/SymbolTable.h"

static int globalVariablesCount = 0;

int AddSymbol(char* symbol) {
    int i;

    if(FindSymbol(symbol) != -1)
        return FindSymbol(symbol);

    i = globalVariablesCount++;
    globalVariables[i].identifier = symbol;
    return i;
}

int FindSymbol(char* symbol) {
    for(int i = 0; i < globalVariablesCount; i++) {
        if(!strcmp(symbol, globalVariables[i].identifier))
            return i;
    }
    return -1;
}
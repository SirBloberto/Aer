#include "../include/Compiler.h"
#include "../include/SymbolTable.h"

static int globalVariablesCount = 0;

int AddSymbol(char* identifier) {
    int i;

    if(FindSymbol(identifier) != -1)
        return FindSymbol(identifier);

    i = globalVariablesCount++;
    globalVariables[i].identifier = identifier;
    return i;
}

int FindSymbol(char* identifier) {
    for(int i = 0; i < globalVariablesCount; i++) {
        if(identifier == globalVariables[i].identifier)
            return i;
    }
    return -1;
}
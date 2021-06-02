#ifndef COMPILER_H
#define COMPILER_H

#include <stdio.h>

#include "Lexer.h"
#include "SymbolTable.h"
#include "Value.h"

#define TABLE_SIZE 512

FILE* file;
Token token;
unsigned int line;
unsigned int position;
Value value;
struct SymbolTable globalVariables[TABLE_SIZE];

void Compile();

#endif
#ifndef COMPILER_H
#define COMPILER_H

#include <stdio.h>

#include "IndentStack.h"
#include "Lexer.h"
#include "SymbolTable.h"
#include "Value.h"

#define INDENT_STACK_SIZE 32
#define TABLE_SIZE 512

FILE* file;
Token token;
unsigned int line;
unsigned int position;
Value value;
struct IndentStack indentStack;
struct SymbolTable globalVariables[TABLE_SIZE];

void Compile();

#endif
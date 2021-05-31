#ifndef COMPILER_H
#define COMPILER_H

#include <stdio.h>

#include "Lexer.h"
#include "Value.h"

FILE* file;
Token token;
unsigned int line;
unsigned int position;
Value value;

#endif
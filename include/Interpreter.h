#ifndef INTERPRETER_H
#define INTERPRETER_H

#include "Parser.h"
#include "Value.h"

Value* Interpret(ASTNode* node);

#endif
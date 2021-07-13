#include "../include/Compiler.h"
#include "../include/IndentStack.h"

void PushIndent(unsigned int indent) {
    indentStack.stack[indentStack.count++] = indent;
}

void PopDedent() {
    indentStack.stack[indentStack.count--] = 0;
}

unsigned int CheckIndent() {
    return indentStack.stack[indentStack.count - 1];
}
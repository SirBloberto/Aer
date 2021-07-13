#ifndef INDENT_STACK_H
#define INDENT_STACK_H

typedef struct IndentStack {
    unsigned int* stack;
    unsigned int count;
} IndentStack;

unsigned int CheckIndent();

void PushIndent(unsigned int indent);

void PopDedent();

#endif
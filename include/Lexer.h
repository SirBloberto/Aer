#ifndef LEXER_H
#define LEXER_H

#include <stdio.h>

typedef enum Token {
    PLUS,
    MINUS,
    STAR,
    SLASH,
} Token;

FILE* file;
Token token;
unsigned int line;
unsigned int position;

void OpenFile(const char* filename);

void CloseFile(void);

int CheckNext(Token check);

#endif
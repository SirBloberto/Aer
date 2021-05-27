#ifndef LEXER_H
#define LEXER_H

#include <stdio.h>

#include "../include/Value.h"

typedef enum Token {
    TOKEN_INTEGER,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_END_OF_FILE
} Token;

FILE* file;
Token token;
unsigned int line;
unsigned int position;

Value value;

void OpenFile(const char* filename);

void CloseFile(void);

void Lex(void);

int CheckNext(Token check);

#endif
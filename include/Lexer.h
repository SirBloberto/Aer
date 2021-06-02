#ifndef LEXER_H
#define LEXER_H

#include "../include/Value.h"

typedef enum Token {
    TOKEN_INTEGER,
    TOKEN_IDENTIFIER,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_EQUALS,
    TOKEN_NEW_LINE,
    TOKEN_END_OF_FILE
} Token;

void OpenFile(const char* filename);

void CloseFile(void);

void Lex(void);

int CheckNext(Token check);

#endif
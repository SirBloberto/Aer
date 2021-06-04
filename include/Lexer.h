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
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_EQUALS_EQUALS,
    TOKEN_EXCLAMATION_EQUALS,
    TOKEN_LESS,
    TOKEN_GREATER,
    TOKEN_LESS_EQUALS,
    TOKEN_GREATER_EQUALS,
    TOKEN_NEW_LINE,
    TOKEN_END_OF_FILE
} Token;

void OpenFile(const char* filename);

void CloseFile(void);

void Lex(void);

int CheckNext(Token check);

#endif
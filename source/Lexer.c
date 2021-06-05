#include <stdio.h>
#include <string.h>
#include "../include/Compiler.h"
#include "../include/Error.h"
#include "../include/Lexer.h"

int NextCharacter(void) {
    int character = fgetc(file);
    position++;
    return character;
}

void PutbackCharacter(int character) {
    ungetc(character, file);
    position++;
}

void Number(int character) {
    token = TOKEN_INTEGER;
    value.type = TYPE_INTEGER;
    value.integerValue = character - '0';

    character = NextCharacter();
    while(character >= '0' && character <= '9') {
        value.integerValue = value.integerValue * 10 + character - '0';
        character = NextCharacter();
    }
    PutbackCharacter(character);
}

void Identifier(char* identifier, int character) {
    identifier[0] = character;
    int i = 1;
    character = NextCharacter();
    while(character >= 'A' && character <= 'z') {
        identifier[i++] = character;
        character = NextCharacter();
    }
    identifier[i] = 0;
    PutbackCharacter(character);
}

int Keyword(char* identifier) {
    if(!strcmp(identifier, "True")) {
        token = TOKEN_TRUE;
        value.type = TYPE_BOOLEAN;
        value.booleanValue = 1;
    } else if(!strcmp(identifier, "False")) {
        token = TOKEN_FALSE;
        value.type = TYPE_BOOLEAN;
        value.booleanValue = 0;
    } else 
        return 0;
    return 1;
}

void Lex() {
    int character = NextCharacter();

    while(character == ' ')
        character = NextCharacter();

    if(character == '(')
        token = TOKEN_OPEN_PARENTHESE;
    else if(character == ')')
        token = TOKEN_CLOSE_PARENTHESE;
    else if(character == '+')
        token = TOKEN_PLUS;
    else if(character == '-')
        token = TOKEN_MINUS;
    else if(character == '*')
        token = TOKEN_STAR;
    else if(character == '/')
        token = TOKEN_SLASH;
    else if(character == '%')
        token = TOKEN_PERCENT;
    else if(character == '^')
        token = TOKEN_CARET;
    else if(character == '&') {
        if((character = NextCharacter()) == '&')
            token = TOKEN_AMPERSAND_AMPERSAND;
        else {
            token = TOKEN_AMPERSAND;
            PutbackCharacter(character);
        }
    } else if(character == '|') {
        if((character = NextCharacter()) == '|')
            token = TOKEN_PIPE_PIPE;
        else {
            token = TOKEN_PIPE;
            PutbackCharacter(character);
        }
    } else if(character == '=') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_EQUALS_EQUALS;
        else {
            token = TOKEN_EQUALS;
            PutbackCharacter(character);
        }
    } else if(character == '!') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_EXCLAMATION_EQUALS;
        else
            PutbackCharacter(character);
    } else if(character == '<') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_LESS_EQUALS;
        else if(character == '<')
            token = TOKEN_LESS_LESS;        
        else {
            token = TOKEN_LESS;
            PutbackCharacter(character);
        }
    } else if(character == '>') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_GREATER_EQUALS;
        else if(character == '>')
            token = TOKEN_GREATER_GREATER;
        else {
            token = TOKEN_GREATER;
            PutbackCharacter(character);
        }
    } else if(character == '\n')
        token = TOKEN_NEW_LINE;
    else if(character == EOF)
        token = TOKEN_END_OF_FILE;
    else if(character >= '1' && character <= '9')
        Number(character);
    else if(character >= 'A' && character <= 'z') {
        char identifier[512];
        Identifier(identifier, character);

        if(!Keyword(identifier)) {
            token = TOKEN_IDENTIFIER;
            value.type = TYPE_IDENTIFIER;
            value.identifierValue = strdup(identifier);
        }
    }
}

void OpenFile(const char* filename) {
    file = fopen(filename, "r");

    if(file == NULL)
        Error("Input file does not exist");

    line = 0;
    position = 0;
}

void CloseFile() {
    fclose(file);
}

int CheckNext(Token check) {
    if(check != token)
        return 0;
    Lex();
    return 1;
}
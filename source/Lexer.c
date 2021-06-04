#include <stdio.h>
#include <string.h>
#include "../include/Compiler.h"
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

void Identifier(int character, char* identifier) {
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

    if(character == '+')
        token = TOKEN_PLUS;
    else if(character == '-')
        token = TOKEN_MINUS;
    else if(character == '*')
        token = TOKEN_STAR;
    else if(character == '/')
        token = TOKEN_SLASH;
    if(character == '%')
        token = TOKEN_PERCENT;
    else if(character == '=') {
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
        else {
            token = TOKEN_LESS;
            PutbackCharacter(character);
        }
    } else if(character == '>') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_GREATER_EQUALS;
        else {
            token = TOKEN_GREATER;
            PutbackCharacter(character);
        }
    } else if(character == '\n')
        token = TOKEN_NEW_LINE;
    else if(character == EOF)
        token = TOKEN_END_OF_FILE;
    else if(character >= '1' && character <= '9') {
        token = TOKEN_INTEGER;
        value.type = TYPE_INTEGER;
        value.integerValue = character - '0';

        character = NextCharacter();
        while(character >= '0' && character <= '9') {
            value.integerValue = value.integerValue * 10 + character - '0';
            character = NextCharacter();
        }
        PutbackCharacter(character);
    } else if(character >= 'A' && character <= 'z') {
        char identifier[512];
        Identifier(character, identifier);

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
        fprintf(stderr, "Input file does not exist");

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
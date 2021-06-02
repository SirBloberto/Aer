#include <stdio.h>
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

void Lex() {
    int character = NextCharacter();

    while(character == ' ')
        character = NextCharacter();

    if(character == '+')
        token = TOKEN_PLUS;
    if(character == '-')
        token = TOKEN_MINUS;
    if(character == '*')
        token = TOKEN_STAR;
    if(character == '/')
        token = TOKEN_SLASH;
    if(character == '%')
        token = TOKEN_PERCENT;
    if(character == '=')
        token = TOKEN_EQUALS;
    if(character == '\n')
        token = TOKEN_NEW_LINE;
    if(character == EOF)
        token = TOKEN_END_OF_FILE;

    if(character >= '1' && character <= '9') {
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

    if(character >= 'A' && character <= 'z') {
        token = TOKEN_IDENTIFIER;
        //value.type = TYPE_IDENTIFIER;
        identifier[0] = character;

        int i = 1;
        character = NextCharacter();
        while(character >= 'A' && character <= 'z') {
            identifier[i++] = character;
            character = NextCharacter();
        }
        identifier[i] = 0;
        PutbackCharacter(character);
        //value.identifierValue = identifier;
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
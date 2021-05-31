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

    while(character == '\n') {
        character = NextCharacter();
        line++;
        position = 0;
    }

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
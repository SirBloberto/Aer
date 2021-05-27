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
        token = PLUS;
    if(character == '-')
        token = MINUS;
    if(character == '*')
        token = STAR;
    if(character == '/')
        token = SLASH;
}

void OpenFile(const char* filename) {
    file = fopen(filename, "r");

    if(file == NULL)
        fprintf(stderr, "Input file does not exist");

    line = 0;
    position = 0;
    Lex();
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
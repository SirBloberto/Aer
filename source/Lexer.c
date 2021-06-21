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
    value.integerValue = character - '0';

    character = NextCharacter();
    while(character >= '0' && character <= '9') {
        value.integerValue = value.integerValue * 10 + character - '0';
        character = NextCharacter();
    }
    if(character == '.') {
        value.realValue = (double)value.integerValue;
        character = NextCharacter();
        for(int i = 10; character >= '0' && character <= '9'; i *= 10) {
            value.realValue += (double)(character - '0') / i;
            character = NextCharacter();
        }
        token = TOKEN_REAL;
        value.type = TYPE_REAL;
    }
    else {
        token = TOKEN_INTEGER;
        value.type = TYPE_INTEGER;
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
    else if(character == '~')
        token = TOKEN_BITWISE_NOT;
    else if(character == '+') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_ADD_ASSIGN;
        else {
            token = TOKEN_ADD;
            PutbackCharacter(character);
        }
    } else if(character == '-') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_SUBTRACT_ASSIGN;
        else {
            token = TOKEN_SUBTRACT;
            PutbackCharacter(character);
        }
    } else if(character == '*') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_MULTIPLY_ASSIGN;
        else {
            token = TOKEN_MULTIPLY;
            PutbackCharacter(character);
        }
    } else if(character == '/') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_DIVIDE_ASSIGN;
        else {
            token = TOKEN_DIVIDE;
            PutbackCharacter(character);
        }
    } else if(character == '%') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_MODULO_ASSIGN;
        else {
            token = TOKEN_MODULO;
            PutbackCharacter(character);
        }
    } else if(character == '^') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_BITWISE_XOR_ASSIGN;
        else {
            token = TOKEN_BITWISE_XOR;
            PutbackCharacter(character);
        }
    } else if(character == '&') {
        if((character = NextCharacter()) == '&')
            token = TOKEN_LOGICAL_AND;
        else if(character == '=')
            token = TOKEN_BITWISE_AND_ASSIGN;
        else {
            token = TOKEN_BITWISE_AND;
            PutbackCharacter(character);
        }
    } else if(character == '|') {
        if((character = NextCharacter()) == '|')
            token = TOKEN_LOGICAL_OR;
        else if(character == '=')
            token = TOKEN_BITWISE_OR_ASSIGN;
        else {
            token = TOKEN_BITWISE_OR;
            PutbackCharacter(character);
        }
    } else if(character == '=') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_EQUAL;
        else {
            token = TOKEN_ASSIGN;
            PutbackCharacter(character);
        }
    } else if(character == '!') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_NOT_EQUAL;
        else {
            token = TOKEN_LOGICAL_NOT;
            PutbackCharacter(character);
        }
    } else if(character == '<') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_LESS_EQUAL;
        else if(character == '<') {
            token = TOKEN_LEFT_SHIFT;
            int thirdCharacter;
            if((thirdCharacter == NextCharacter()) == '=')
                token = TOKEN_LEFT_SHIFT_ASSIGN;
            else {
                PutbackCharacter(thirdCharacter);
                PutbackCharacter(character);
            }  
        } else {
            token = TOKEN_LESS;
            PutbackCharacter(character);
        }
    } else if(character == '>') {
        if((character = NextCharacter()) == '=')
            token = TOKEN_GREATER_EQUAL;
        else if(character == '>')
            token = TOKEN_RIGHT_SHIFT;
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
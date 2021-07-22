#include "../include/Compiler.h"
#include "../include/Lexer.h"
#include "../include/Parser.h"
#include "../include/Interpreter.h"

void PrintMemory() {
    for(int i = 0; i < TABLE_SIZE; i++) {
        if(globalVariables[i].identifier != 0) {
            printf("Identifier: %s,  ", globalVariables[i].identifier);
            Value value = globalVariables[i].value;
            if(value.type == TYPE_INTEGER)
                printf("Value: %ld\n", value.integerValue);
            if(value.type == TYPE_BOOLEAN){
                if(value.booleanValue == 0)
                    printf("Value: False\n");
                else
                    printf("Value: True\n");
            }if(value.type == TYPE_REAL)
                printf("Value: %lf\n", value.realValue);
        }
    }
}

void Compile() {
    Lex();
    while(token != TOKEN_END_OF_FILE) {
        Interpret(Parse());
    }
}

int main(int argc, char** argv) {
    OpenFile(argv[1]);
    Compile();
    PrintMemory();
    CloseFile();
}
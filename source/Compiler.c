#include "../include/Compiler.h"
#include "../include/Lexer.h"
#include "../include/Parser.h"
#include "../include/Interpreter.h"

void PrintMemory() {
    for(int i = 0; i < TABLE_SIZE; i++) {
        if(globalVariables[i].identifier != 0) {
            printf("Identifier: %s,  ", globalVariables[i].identifier);
            Value test = globalVariables[i].value;
            if(test.type == TYPE_INTEGER)
                printf("Value: %d\n", test.integerValue);
            if(test.type == TYPE_FLOAT)
                printf("Value: %f\n", test.floatValue);
        }
    }
}

int main(int argc, char** argv) {
    OpenFile(argv[1]);
    Lex();
    Parse();
    PrintMemory();
    CloseFile();
}
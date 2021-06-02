#include "../include/Compiler.h"
#include "../include/Lexer.h"
#include "../include/Parser.h"
#include "../include/Interpreter.h"

void PrintMemory() {
    for(int i = 0; i < TABLE_SIZE; i++) {
        if(globalVariables[i].identifier != 0) {
            printf("Identifier: %s,  ", globalVariables[i].identifier);
            Value test = globalVariables[i].value;
            printf("Value: %d", test.integerValue);
        }
    }
}

int main(int argc, char** argv) {
    OpenFile(argv[1]);
    Parse();
    //PrintMemory();
    CloseFile();
}
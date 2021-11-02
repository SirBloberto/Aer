#include "../include/Compiler.h"
#include "../include/Interpreter.h"
#include "../include/Lexer.h"
#include "../include/Parser.h"

void Compile() {
    Lex();
    while(token != TOKEN_END_OF_FILE) {
        Interpret(Parse());
    }
}

int main(int argumentCount, char** arguments) {
    OpenFile(arguments[1]);
    Compile();
    CloseFile();
}
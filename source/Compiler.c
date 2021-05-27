#include "../include/Lexer.h"
#include "../include/Parser.h"
#include "../include/Interpreter.h"

int main(int argc, char** argv) {
    OpenFile(argv[1]);
    Lex();
    Interpret(Parse());
    CloseFile();
}
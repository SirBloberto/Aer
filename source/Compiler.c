#include "../include/Lexer.h"
#include "../include/Parser.h"

int main(int argc, char** argv) {
    OpenFile(argv[1]);
    Lex();
    Parse();
    CloseFile();
}
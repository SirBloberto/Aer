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
                printf("Value: %d\n", (int)value.integerValue);
            if(value.type == TYPE_BOOLEAN){
                if(value.booleanValue == 0)
                    printf("Value: False\n");
                else
                    printf("Value: True\n");
            }
        }
    }
}

void Compile() {
    while(token != TOKEN_END_OF_FILE) {
        Lex();
        Interpret(Parse());
    }
}

/*Things to do:
 1. Clean up code: lexer definetly needs work.  Parser can use some minor optimization and interperter can be cleaned up slightly.
        SymbolTable needs a rework for sure aswell.  Need more type of error message and better error messages.  Error messages do not have to be good rn.
 2. Add Unary operations
 3. Add all assignment operations
 4. Add floating point numbers
 5. Add Hex and Binary number (integers)
 6. If statements
 7. Loops
*/

int main(int argc, char** argv) {
    OpenFile(argv[1]);
    Compile();
    PrintMemory();
    CloseFile();
}
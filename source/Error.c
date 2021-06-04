#include <stdio.h>
#include <stdlib.h>

#include "../include/Compiler.h"
#include "../include/Error.h"

void Error(char* message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}
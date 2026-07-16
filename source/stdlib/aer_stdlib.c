#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "aer_stdlib.h"

void aer_stdlib_init(void) {
    srand((unsigned int)time(NULL));
}

bool aer_stdlib_is_native_module(const char* name, unsigned int len) {
    return (len == 4 && strncmp(name, "math",   4) == 0) ||
           (len == 6 && strncmp(name, "random", 6) == 0) ||
           (len == 6 && strncmp(name, "string", 6) == 0) ||
           (len == 4 && strncmp(name, "time",   4) == 0) ||
           (len == 4 && strncmp(name, "json",   4) == 0);
}

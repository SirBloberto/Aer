#include <string.h>
#include "aer_stdlib.h"

bool aer_stdlib_is_native_module(const char* name, unsigned int len) {
    return (len == 4 && strncmp(name, "math", 4) == 0) || (len == 6 && strncmp(name, "random", 6) == 0) ||
           (len == 6 && strncmp(name, "string", 6) == 0) || (len == 4 && strncmp(name, "time", 4) == 0) ||
           (len == 4 && strncmp(name, "json", 4) == 0) ||
           (len == 10 && strncmp(name, "collection", 10) == 0) ||
           (len == 3 && strncmp(name, "net", 3) == 0) || (len == 5 && strncmp(name, "regex", 5) == 0) ||
           (len == 5 && strncmp(name, "actor", 5) == 0) || (len == 9 && strncmp(name, "scheduler", 9) == 0);
}

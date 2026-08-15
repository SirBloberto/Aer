#include <string.h>
#include "error.h"
#include "aer_abi.h"
#include "aer_stdlib.h"

bool aer_stdlib_is_native_module(const char* name, unsigned int len) {
#define AER_MODULE_NAME_CHECK(id, str, call)                                                                 \
    if (len == sizeof(str) - 1 && strncmp(name, str, sizeof(str) - 1) == 0)                                  \
        return true;
    AER_NATIVE_MODULES(AER_MODULE_NAME_CHECK)
#undef AER_MODULE_NAME_CHECK
    return false;
}

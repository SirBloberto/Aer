#include "../include/Error.h"
#include "../include/Value.h"

char CorrectBoolean(char booleanValue) {
    if(booleanValue != 0)
        return 1;
    return 0;    
}

long CoerceInteger(Value value) {
    switch(value.type) {
        case TYPE_INTEGER:
            return (long)value.integerValue;
        default:
            Error("Coerce Integer Error");
    }
}
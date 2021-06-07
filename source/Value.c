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
        case TYPE_REAL:
            return (long)value.realValue;
        default:
            Error("Coerce Integer Error");
    }
}

double CoerceReal(Value value) {
    switch(value.type) {
        case TYPE_INTEGER:
            return (double)value.integerValue;
        case TYPE_REAL:
            return (double)value.realValue;
        default:
            Error("Coerce Real Error");
    }
}
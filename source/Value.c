#include "../include/Value.h"

long CoerceInteger(Value value) {
    switch(value.type) {
        case TYPE_BOOLEAN:
            return (long)value.booleanValue;
        case TYPE_INTEGER:
            return (long)value.integerValue;
        case TYPE_FLOAT:
            return (long)value.floatValue;
        default:
            return 0;
    }
}

double CoerceFloat(Value value) {
    switch(value.type) {
        case TYPE_BOOLEAN:
            return (double)value.booleanValue;
        case TYPE_INTEGER:
            return (double)value.integerValue;
        case TYPE_FLOAT:
            return (double)value.floatValue;
        default:
            return 0;
    }
}
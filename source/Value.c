#include "../include/Value.h"

long CoerceInteger(Value value) {
    switch(value.type) {
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
        case TYPE_INTEGER:
            return (long)value.integerValue;
        case TYPE_FLOAT:
            return (long)value.floatValue;
        default:
            return 0;
    }
}
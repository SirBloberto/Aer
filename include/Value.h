#ifndef VALUE_H
#define VALUE_H

typedef enum ValueType {
    TYPE_BOOLEAN,
    TYPE_IDENTIFIER,
    TYPE_INTEGER,
    TYPE_FLOAT
} Type;

typedef struct Value {
    enum ValueType type;
    union {
        char booleanValue;
        char* identifierValue;
        long integerValue;
        double floatValue;
    };
} Value;

long CoerceInteger(Value value);

double CoerceFloat(Value value);

#endif
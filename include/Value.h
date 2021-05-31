#ifndef VALUE_H
#define VALUE_H

typedef enum ValueType {
    TYPE_INTEGER,
    TYPE_FLOAT
} Type;

typedef struct Value {
    enum ValueType type;
    union {
        long integerValue;
        double floatValue;
    };
} Value;

long CoerceInteger(Value value);

double CoerceFloat(Value value);

#endif
#ifndef VALUE_H
#define VALUE_H

typedef enum ValueType {
    TYPE_BOOLEAN,
    TYPE_INTEGER,
    TYPE_REAL,
    TYPE_IDENTIFIER
} Type;

typedef struct Value {
    enum ValueType type;
    union {
        char booleanValue;
        long integerValue;
        double realValue;
        char* stringValue;
        char* identifierValue;
    };
} Value;

char CorrectBoolean(char booleanValue);

long CoerceInteger(Value value);

double CoerceReal(Value value);

#endif
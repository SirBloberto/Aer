#ifndef VALUE_H
#define VALUE_H

typedef enum ValueType {
    TYPE_INTEGER
} Type;

typedef struct Value {
    enum ValueType type;
    union {
        int integerData;
    };
} Value;

#endif
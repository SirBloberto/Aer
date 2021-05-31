#ifndef PARSER_H
#define PARSER_H

#include "Value.h"

struct ASTNode;

typedef enum BinaryOperation {
    OPERATION_ADD,
    OPERATION_SUBTRACT,
    OPERATION_MULTIPLY,
    OPERATION_DIVIDE,
    OPERATION_MODULO
} BinaryOperation;

typedef struct BinaryExpression {
    struct ASTNode* left;
    struct ASTNode* right;
    BinaryOperation operation;
} BinaryExpression;

typedef enum NodeType {
    NODE_BINARY_EXPRESSION,
    NODE_VALUE
} NodeType;

typedef struct ASTNode {
    enum NodeType type;
    union {
        BinaryExpression binaryExpression;
        Value value;
    };
} ASTNode;

ASTNode* BinaryExpressionNode(ASTNode* left, ASTNode* right, BinaryOperation operation);
ASTNode* ValueNode(Value value);

ASTNode* Parse();

#endif
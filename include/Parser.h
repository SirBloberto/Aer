#ifndef PARSER_H
#define PARSER_H

#include "Value.h"

struct ASTNode;

typedef enum AssignmentOperation {
    ASSIGNMENT_OPERATION_ASSIGN
} AssignmentOperation;

typedef struct AssignmentExpression {
    struct ASTNode* left;
    struct ASTNode* right;
    AssignmentOperation operation;
} AssignmentExpression;

typedef enum BinaryOperation {
    BINARY_OPERATION_LOGICAL_OR,
    BINARY_OPERATION_LOGICAL_AND,
    
    BINARY_OPERATION_BITWISE_OR,
    BINARY_OPERATION_BITWISE_XOR,
    BINARY_OPERATION_BITWISE_AND,

    BINARY_OPERATION_EQUAL,
    BINARY_OPERATION_NOT_EQUAL,

    BINARY_OPERATION_GREATER,
    BINARY_OPERATION_LESS,
    BINARY_OPERATION_GREATER_EQUAL,
    BINARY_OPERATION_LESS_EQUAL,

    BINARY_OPERATION_LEFT_SHIFT,
    BINARY_OPERATION_RIGHT_SHIFT,

    BINARY_OPERATION_ADD,
    BINARY_OPERATION_SUBTRACT,
    
    BINARY_OPERATION_MULTIPLY,
    BINARY_OPERATION_DIVIDE,
    BINARY_OPERATION_MODULO
} BinaryOperation;

typedef struct BinaryExpression {
    struct ASTNode* left;
    struct ASTNode* right;
    BinaryOperation operation;
} BinaryExpression;

typedef enum NodeType {
    NODE_ASSIGNMENT_EXPRESSION,
    NODE_BINARY_EXPRESSION,
    NODE_VALUE
} NodeType;

typedef struct ASTNode {
    enum NodeType type;
    union {
        AssignmentExpression assignmentExpression;
        BinaryExpression binaryExpression;
        Value value;
    };
} ASTNode;

ASTNode* AssignmentExpressionNode(ASTNode* left, ASTNode* right, AssignmentOperation operation);
ASTNode* BinaryExpressionNode(ASTNode* left, ASTNode* right, BinaryOperation operation);
ASTNode* ValueNode(Value value);

ASTNode* Parse();

#endif
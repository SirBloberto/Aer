#ifndef PARSER_H
#define PARSER_H

#include "Value.h"

struct ASTNode;

typedef enum AssignmentOperation {
    OPERATION_ASSIGN
} AssignmentOperation;

typedef struct AssignmentExpression {
    struct ASTNode* left;
    struct ASTNode* right;
    AssignmentOperation operation;
} AssignmentExpression;

typedef enum BinaryOperation {
    OPERATION_IS_EQUAL,
    OPERATION_IS_NOT_EQUAL,
    OPERATION_IS_LESS,
    OPERATION_IS_GREATER,
    OPERATION_IS_LESS_EQUAL,
    OPERATION_IS_GREATER_EQUAL,
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
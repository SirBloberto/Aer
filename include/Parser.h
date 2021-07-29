#ifndef PARSER_H
#define PARSER_H

#include "Value.h"

struct ASTNode;

typedef enum AssignmentOperation {
    ASSIGNMENT_OPERATION_ASSIGN,
    ASSIGNMENT_OPERATION_ADD_ASSIGN,
    ASSIGNMENT_OPERATION_SUBTRACT_ASSIGN,
    ASSIGNMENT_OPERATION_MULTIPLY_ASSIGN,
    ASSIGNMENT_OPERATION_DIVIDE_ASSIGN,
    ASSIGNMENT_OPERATION_MODULO_ASSIGN,
    ASSIGNMENT_OPERATION_LEFT_SHIFT_ASSIGN,
    ASSIGNMENT_OPERATION_RIGHT_SHIFT_ASSIGN,
    ASSIGNMENT_OPERATION_AND_ASSIGN,
    ASSIGNMENT_OPERATION_OR_ASSIGN,
    ASSIGNMENT_OPERATION_XOR_ASSIGN
} AssignmentOperation;

typedef struct AssignmentExpression {
    struct ASTNode* left;
    struct ASTNode* right;
    AssignmentOperation operation;
} AssignmentExpression;

//Can use a mask to Determine what operations are valid on which types and which types they return
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

typedef enum UnaryOperation {
    UNARY_OPERATION_LOGICAL_NOT,
    UNARY_OPERATION_BITWISE_NOT,
    UNARY_OPERATION_NEGATE
} UnaryOperation;

typedef struct UnaryExpression {
    struct ASTNode* node;
    UnaryOperation operation;
} UnaryExpression;

typedef struct CompoundStatement {
    struct ASTNode** nodes;
    unsigned int count;
} CompoundStatement;

typedef struct IfStatement {
    struct ASTNode* condition;
    struct ASTNode* trueBlock;
    struct ASTNode* falseBlock;
} IfStatement;

typedef struct LoopStatement {
    struct ASTNode* condition;
    struct ASTNode* block;
} LoopStatement;

typedef enum NodeType {
    NODE_ASSIGNMENT_EXPRESSION,
    NODE_BINARY_EXPRESSION,
    NODE_UNARY_EXPRESSION,
    NODE_COMPOUND_STATEMENT,
    NODE_IF_STATEMENT,
    NODE_LOOP_STATEMENT,
    NODE_VALUE
} NodeType;

typedef struct ASTNode {
    enum NodeType type;
    union {
        AssignmentExpression assignmentExpression;
        BinaryExpression binaryExpression;
        UnaryExpression unaryExpression;

        CompoundStatement compoundStatement;
        IfStatement ifStatement;
        LoopStatement loopStatement;

        Value value;
    };
} ASTNode;

ASTNode* AssignmentExpressionNode(ASTNode* left, ASTNode* right, AssignmentOperation operation);
ASTNode* BinaryExpressionNode(ASTNode* left, ASTNode* right, BinaryOperation operation);
ASTNode* UnaryExpressionNode(ASTNode* expression, UnaryOperation operation);
ASTNode* CompoundStatementNode(ASTNode** nodes, unsigned int count);
ASTNode* IfStatementNode(ASTNode* condition, ASTNode* trueBlock, ASTNode* elseBlock);
ASTNode* LoopStatementNode(ASTNode* condition, ASTNode* block);
ASTNode* ValueNode(Value value);

ASTNode* Parse();

#endif
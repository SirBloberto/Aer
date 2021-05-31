#include "../include/Compiler.h"
#include "../include/Lexer.h"
#include "../include/Parser.h"

#include <stdlib.h>

ASTNode* AdditiveExpression();
ASTNode* Expression();
ASTNode* MultiplicativeExpression();
ASTNode* PrimairyExpression();

ASTNode* BinaryExpressionNode(ASTNode* left, ASTNode* right, BinaryOperation operation) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = NODE_BINARY_EXPRESSION;
    node->binaryExpression.left = left;
    node->binaryExpression.right = right;
    node->binaryExpression.operation = operation;
    return node;
}

ASTNode* ValueNode(Value value) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = NODE_VALUE;
    node->value = value;
    return node;
}

ASTNode* Parse() {
    return Expression();
}

ASTNode* AdditiveExpression() {
    ASTNode* left = MultiplicativeExpression();

    if(CheckNext(TOKEN_PLUS))
        return BinaryExpressionNode(left, AdditiveExpression(), OPERATION_ADD);
    if(CheckNext(TOKEN_MINUS))
        return BinaryExpressionNode(left, AdditiveExpression(), OPERATION_SUBTRACT);

    return left;
}

ASTNode* Expression() {
    return AdditiveExpression();
}

ASTNode* MultiplicativeExpression() {
    ASTNode* left = PrimairyExpression();

    if(CheckNext(TOKEN_STAR))
        return BinaryExpressionNode(left, MultiplicativeExpression(), OPERATION_MULTIPLY);
    if(CheckNext(TOKEN_SLASH))
        return BinaryExpressionNode(left, MultiplicativeExpression(), OPERATION_DIVIDE);
    if(CheckNext(TOKEN_PERCENT))
        return BinaryExpressionNode(left, MultiplicativeExpression(), OPERATION_MODULO);

    return left;
}

ASTNode* PrimairyExpression() {
    if(CheckNext(TOKEN_INTEGER))
        return ValueNode(value);
}
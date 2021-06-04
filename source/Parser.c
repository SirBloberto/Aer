#include "../include/Compiler.h"
#include "../include/Interpreter.h"
#include "../include/Lexer.h"
#include "../include/Parser.h"

#include <stdlib.h>

ASTNode* ParseAssignmentExpression();
ASTNode* ParseExpression();
ASTNode* ParseEqualityExpression();
ASTNode* ParseRelationalExpression();
ASTNode* ParseAdditiveExpression();
ASTNode* ParseMultiplicativeExpression();
ASTNode* ParsePrimairyExpression();

ASTNode* AssignmentExpressionNode(ASTNode* left, ASTNode* right, AssignmentOperation operation) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = NODE_ASSIGNMENT_EXPRESSION;
    node->assignmentExpression.left = left;
    node->assignmentExpression.right = right;
    node->assignmentExpression.operation = operation;
    return node;
}

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
    if(CheckNext(TOKEN_IDENTIFIER))
        return ParseAssignmentExpression();
}

ASTNode* ParseAssignmentExpression() {
    ASTNode* left = ValueNode(value);

    ASTNode* node;
    if(CheckNext(TOKEN_EQUALS))
        return AssignmentExpressionNode(left, ParseExpression(), OPERATION_ASSIGN);
}

ASTNode* ParseExpression() {
    return ParseEqualityExpression();
}

ASTNode* ParseEqualityExpression() {
    ASTNode* left = ParseRelationalExpression();

    if(CheckNext(TOKEN_EQUALS_EQUALS))
        return BinaryExpressionNode(left, ParseEqualityExpression(), OPERATION_IS_EQUAL);
    if(CheckNext(TOKEN_EXCLAMATION_EQUALS))
        return BinaryExpressionNode(left, ParseEqualityExpression(), OPERATION_IS_NOT_EQUAL);
    
    return left;
}

ASTNode* ParseRelationalExpression() {
    ASTNode* left = ParseAdditiveExpression();

    if(CheckNext(TOKEN_GREATER))
        return BinaryExpressionNode(left, ParseRelationalExpression(), OPERATION_IS_GREATER);
    if(CheckNext(TOKEN_LESS))
        return BinaryExpressionNode(left, ParseRelationalExpression(), OPERATION_IS_LESS);
    if(CheckNext(TOKEN_GREATER_EQUALS))
        return BinaryExpressionNode(left, ParseRelationalExpression(), OPERATION_IS_GREATER_EQUAL);
    if(CheckNext(TOKEN_LESS_EQUALS))
        return BinaryExpressionNode(left, ParseRelationalExpression(), OPERATION_IS_LESS_EQUAL);

    return left;
}

ASTNode* ParseAdditiveExpression() {
    ASTNode* left = ParseMultiplicativeExpression();

    if(CheckNext(TOKEN_PLUS))
        return BinaryExpressionNode(left, ParseAdditiveExpression(), OPERATION_ADD);
    if(CheckNext(TOKEN_MINUS))
        return BinaryExpressionNode(left, ParseAdditiveExpression(), OPERATION_SUBTRACT);

    return left;
}

ASTNode* ParseMultiplicativeExpression() {
    ASTNode* left = ParsePrimairyExpression();

    if(CheckNext(TOKEN_STAR))
        return BinaryExpressionNode(left, ParseMultiplicativeExpression(), OPERATION_MULTIPLY);
    if(CheckNext(TOKEN_SLASH))
        return BinaryExpressionNode(left, ParseMultiplicativeExpression(), OPERATION_DIVIDE);
    if(CheckNext(TOKEN_PERCENT))
        return BinaryExpressionNode(left, ParseMultiplicativeExpression(), OPERATION_MODULO);

    return left;
}

ASTNode* ParsePrimairyExpression() {
    if(CheckNext(TOKEN_INTEGER))
        return ValueNode(value);
    if(CheckNext(TOKEN_IDENTIFIER)) {
        int id = FindSymbol(value.identifierValue);
        return ValueNode(globalVariables[id].value);
    } if(CheckNext(TOKEN_TRUE) || CheckNext(TOKEN_FALSE))
        return ValueNode(value);
    return 0;
}
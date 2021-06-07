#include "../include/Compiler.h"
#include "../include/Error.h"
#include "../include/Interpreter.h"
#include "../include/Lexer.h"
#include "../include/Parser.h"

#include <stdlib.h>

ASTNode* ParseAssignmentExpression();
ASTNode* ParseExpression();
ASTNode* ParseLogicalOrExpression();
ASTNode* ParseLogicalAndExpression();
ASTNode* ParseBitwiseOrExpression();
ASTNode* ParseBitwiseXorExpression();
ASTNode* ParseBitwiseAndExpression();
ASTNode* ParseEqualityExpression();
ASTNode* ParseRelationalExpression();
ASTNode* ParseShiftExpression();
ASTNode* ParseAdditiveExpression();
ASTNode* ParseMultiplicativeExpression();
ASTNode* ParseUnaryExpression();
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

ASTNode* UnaryExpressionNode(ASTNode* expression, UnaryOperation operation) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = NODE_UNARY_EXPRESSION;
    node->unaryExpression.node = expression;
    node->unaryExpression.operation = operation;
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
    Error("Parse Error");
}

ASTNode* ParseAssignmentExpression() {
    ASTNode* left = ValueNode(value);

    if(CheckNext(TOKEN_EQUALS))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_ASSIGN);
    Error("Parse AssignmentExpression Error");
}

ASTNode* ParseExpression() {
    return ParseLogicalOrExpression();
}

ASTNode* ParseLogicalOrExpression() {
    ASTNode* left = ParseLogicalAndExpression();

    if(CheckNext(TOKEN_PIPE_PIPE))
        return BinaryExpressionNode(left, ParseLogicalOrExpression(), BINARY_OPERATION_LOGICAL_OR);

    return left;
}

ASTNode* ParseLogicalAndExpression() {
    ASTNode* left = ParseBitwiseOrExpression();

    if(CheckNext(TOKEN_AMPERSAND_AMPERSAND))
        return BinaryExpressionNode(left, ParseLogicalAndExpression(), BINARY_OPERATION_LOGICAL_AND);

    return left;
}

ASTNode* ParseBitwiseOrExpression() {
    ASTNode* left = ParseBitwiseXorExpression();

    if(CheckNext(TOKEN_PIPE))
        return BinaryExpressionNode(left, ParseBitwiseOrExpression(), BINARY_OPERATION_BITWISE_OR);

    return left;
}

ASTNode* ParseBitwiseXorExpression() {
    ASTNode* left = ParseBitwiseAndExpression();

    if(CheckNext(TOKEN_CARET))
        return BinaryExpressionNode(left, ParseBitwiseXorExpression(), BINARY_OPERATION_BITWISE_XOR);

    return left;
}

ASTNode* ParseBitwiseAndExpression() {
    ASTNode* left = ParseEqualityExpression();

    if(CheckNext(TOKEN_AMPERSAND))
        return BinaryExpressionNode(left, ParseBitwiseAndExpression(), BINARY_OPERATION_BITWISE_AND);

    return left;
}

ASTNode* ParseEqualityExpression() {
    ASTNode* left = ParseRelationalExpression();

    if(CheckNext(TOKEN_EQUALS_EQUALS))
        return BinaryExpressionNode(left, ParseEqualityExpression(), BINARY_OPERATION_EQUAL);
    if(CheckNext(TOKEN_EXCLAMATION_EQUALS))
        return BinaryExpressionNode(left, ParseEqualityExpression(), BINARY_OPERATION_NOT_EQUAL);
    
    return left;
}

ASTNode* ParseRelationalExpression() {
    ASTNode* left = ParseShiftExpression();

    if(CheckNext(TOKEN_GREATER))
        return BinaryExpressionNode(left, ParseRelationalExpression(), BINARY_OPERATION_GREATER);
    if(CheckNext(TOKEN_LESS))
        return BinaryExpressionNode(left, ParseRelationalExpression(), BINARY_OPERATION_LESS);
    if(CheckNext(TOKEN_GREATER_EQUALS))
        return BinaryExpressionNode(left, ParseRelationalExpression(), BINARY_OPERATION_GREATER_EQUAL);
    if(CheckNext(TOKEN_LESS_EQUALS))
        return BinaryExpressionNode(left, ParseRelationalExpression(), BINARY_OPERATION_LESS_EQUAL);

    return left;
}

ASTNode* ParseShiftExpression() {
    ASTNode* left = ParseAdditiveExpression();

    if(CheckNext(TOKEN_GREATER_GREATER))
        return BinaryExpressionNode(left, ParseShiftExpression(), BINARY_OPERATION_RIGHT_SHIFT);
    if(CheckNext(TOKEN_LESS_LESS))
        return BinaryExpressionNode(left, ParseShiftExpression(), BINARY_OPERATION_LEFT_SHIFT);

    return left;
}

ASTNode* ParseAdditiveExpression() {
    ASTNode* left = ParseMultiplicativeExpression();

    if(CheckNext(TOKEN_PLUS))
        return BinaryExpressionNode(left, ParseAdditiveExpression(), BINARY_OPERATION_ADD);
    if(CheckNext(TOKEN_MINUS))
        return BinaryExpressionNode(left, ParseAdditiveExpression(), BINARY_OPERATION_SUBTRACT);

    return left;
}

ASTNode* ParseMultiplicativeExpression() {
    ASTNode* left = ParseUnaryExpression();

    if(CheckNext(TOKEN_STAR))
        return BinaryExpressionNode(left, ParseMultiplicativeExpression(), BINARY_OPERATION_MULTIPLY);
    if(CheckNext(TOKEN_SLASH))
        return BinaryExpressionNode(left, ParseMultiplicativeExpression(), BINARY_OPERATION_DIVIDE);
    if(CheckNext(TOKEN_PERCENT))
        return BinaryExpressionNode(left, ParseMultiplicativeExpression(), BINARY_OPERATION_MODULO);

    return left;
}

ASTNode* ParseUnaryExpression() {
    if(CheckNext(TOKEN_EXCLAMATION))
        return UnaryExpressionNode(ParsePrimairyExpression(), UNARY_OPERATION_LOGICAL_NOT);
    if(CheckNext(TOKEN_MINUS))
        return UnaryExpressionNode(ParsePrimairyExpression(), UNARY_OPERATION_NEGATE);
    if(CheckNext(TOKEN_TILDE))
        return UnaryExpressionNode(ParsePrimairyExpression(), UNARY_OPERATION_BITWISE_NOT);
}

ASTNode* ParsePrimairyExpression() {
    if(CheckNext(TOKEN_TRUE) || CheckNext(TOKEN_FALSE))
        return ValueNode(value);
    else if(CheckNext(TOKEN_INTEGER))
        return ValueNode(value);
    else if(CheckNext(TOKEN_IDENTIFIER)) {
        int id = FindSymbol(value.identifierValue);
        return ValueNode(globalVariables[id].value);
    } else if(CheckNext(TOKEN_OPEN_PARENTHESE)) {
        ASTNode* node = ParseExpression();
        if(!CheckNext(TOKEN_CLOSE_PARENTHESE))
            Error("No closing parenthese");
        return node;
    }
    Error("Parse PrimairyExpression Error");
}
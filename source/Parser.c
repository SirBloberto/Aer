#include "../include/Compiler.h"
#include "../include/Error.h"
#include "../include/Interpreter.h"
#include "../include/Lexer.h"
#include "../include/Parser.h"

#include <stdlib.h>

ASTNode* ParseCompoundStatement();
ASTNode* ParseIfStatement();
ASTNode* ParseLoopStatement();
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

ASTNode** AddNodeToCompoundNode(ASTNode** nodes, unsigned int count, ASTNode* node) {
    ASTNode** nodesCopy = (ASTNode**)malloc(sizeof(ASTNode*) * ++count);
    if(nodes != 0) {
        *nodesCopy = *nodes;
        free(nodes);
    }
    nodesCopy[count - 1] = node;
    return nodesCopy;
}

ASTNode* CompoundStatementNode(ASTNode** nodes, unsigned int count) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = NODE_COMPOUND_STATEMENT;
    node->compoundStatement.nodes = nodes;
    node->compoundStatement.count = count;
    return node;
}

ASTNode* IfStatementNode(ASTNode* condition, ASTNode* trueBlock, ASTNode* falseBlock) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = NODE_IF_STATEMENT;
    node->ifStatement.condition = condition;
    node->ifStatement.trueBlock = trueBlock;
    node->ifStatement.falseBlock = falseBlock;
    return node;
}

ASTNode* LoopStatementNode(ASTNode* condition, ASTNode* block) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = NODE_LOOP_STATEMENT;
    node->loopStatement.condition = condition;
    node->loopStatement.block = block;
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
    if(CheckNext(TOKEN_IF))
        return ParseIfStatement();
    if(CheckNext(TOKEN_LOOP))
        return ParseLoopStatement();
    else if(CheckNext(TOKEN_NEW_LINE))
        return Parse();
    Error("Parse Error");
}

ASTNode* ParseCompoundStatement() {
    ASTNode** nodes = 0;
    unsigned int count = 0;
    if(CheckNext(TOKEN_INDENT)) {
        ASTNode* node;
        while(1) {
            if(CheckNext(TOKEN_IDENTIFIER))
                node = ParseAssignmentExpression();
            else if(CheckNext(TOKEN_IF))
                node = ParseIfStatement();
            else if(CheckNext(TOKEN_NEW_LINE))
                continue;
            else if(CheckNext(TOKEN_DEDENT))
                break;
            nodes = AddNodeToCompoundNode(nodes, count, node);
            count++;
        }
        return CompoundStatementNode(nodes, count);
    } else if(CheckNext(TOKEN_NEW_LINE))
        ParseCompoundStatement();
    else
        Error("Parse Compound Error: No indent token");
}

ASTNode* ParseIfStatement() {
    ASTNode* condition = ParseExpression();

    if(CheckNext(TOKEN_COLON)) {
        ASTNode* trueBlock = ParseCompoundStatement();
        ASTNode* falseBlock;

        if(CheckNext(TOKEN_ELSE)) {
            if(CheckNext(TOKEN_COLON))
                falseBlock = ParseCompoundStatement();
            else {
                Error("Parse If Error: No Colon on else");
            }
        }

        return IfStatementNode(condition, trueBlock, falseBlock);
    } else
        Error("Parse If Error: No colon on if");
}

ASTNode* ParseLoopStatement() {
    ASTNode* condition = ParseExpression();

    if(CheckNext(TOKEN_COLON)) {
        ASTNode* block = ParseCompoundStatement();

        return LoopStatementNode(condition, block);
    } else 
        Error("Parse Loop Error: No conlon on loop");
}

ASTNode* ParseAssignmentExpression() {
    ASTNode* left = ValueNode(value);

    if(CheckNext(TOKEN_ASSIGN))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_ASSIGN);
    if(CheckNext(TOKEN_ADD_ASSIGN))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_ADD_ASSIGN);
    if(CheckNext(TOKEN_SUBTRACT_ASSIGN))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_SUBTRACT_ASSIGN);
    if(CheckNext(TOKEN_MULTIPLY_ASSIGN))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_MULTIPLY_ASSIGN);
    if(CheckNext(TOKEN_DIVIDE_ASSIGN))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_DIVIDE_ASSIGN);
    if(CheckNext(TOKEN_MODULO_ASSIGN))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_MODULO_ASSIGN);
    if(CheckNext(TOKEN_LEFT_SHIFT_ASSIGN))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_LEFT_SHIFT_ASSIGN);
    if(CheckNext(TOKEN_RIGHT_SHIFT_ASSIGN))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_RIGHT_SHIFT_ASSIGN);
    if(CheckNext(TOKEN_BITWISE_AND_ASSIGN))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_AND_ASSIGN);
    if(CheckNext(TOKEN_BITWISE_OR_ASSIGN))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_OR_ASSIGN);
    if(CheckNext(TOKEN_BITWISE_XOR_ASSIGN))
        return AssignmentExpressionNode(left, ParseExpression(), ASSIGNMENT_OPERATION_XOR_ASSIGN);
    Error("Parse AssignmentExpression Error");
}

ASTNode* ParseExpression() {
    return ParseLogicalOrExpression();
}

ASTNode* ParseLogicalOrExpression() {
    ASTNode* left = ParseLogicalAndExpression();

    if(CheckNext(TOKEN_LOGICAL_OR))
        return BinaryExpressionNode(left, ParseLogicalOrExpression(), BINARY_OPERATION_LOGICAL_OR);

    return left;
}

ASTNode* ParseLogicalAndExpression() {
    ASTNode* left = ParseBitwiseOrExpression();

    if(CheckNext(TOKEN_LOGICAL_AND))
        return BinaryExpressionNode(left, ParseLogicalAndExpression(), BINARY_OPERATION_LOGICAL_AND);

    return left;
}

ASTNode* ParseBitwiseOrExpression() {
    ASTNode* left = ParseBitwiseXorExpression();

    if(CheckNext(TOKEN_BITWISE_OR))
        return BinaryExpressionNode(left, ParseBitwiseOrExpression(), BINARY_OPERATION_BITWISE_OR);

    return left;
}

ASTNode* ParseBitwiseXorExpression() {
    ASTNode* left = ParseBitwiseAndExpression();

    if(CheckNext(TOKEN_BITWISE_XOR))
        return BinaryExpressionNode(left, ParseBitwiseXorExpression(), BINARY_OPERATION_BITWISE_XOR);

    return left;
}

ASTNode* ParseBitwiseAndExpression() {
    ASTNode* left = ParseEqualityExpression();

    if(CheckNext(TOKEN_BITWISE_AND))
        return BinaryExpressionNode(left, ParseBitwiseAndExpression(), BINARY_OPERATION_BITWISE_AND);

    return left;
}

ASTNode* ParseEqualityExpression() {
    ASTNode* left = ParseRelationalExpression();

    if(CheckNext(TOKEN_EQUAL))
        return BinaryExpressionNode(left, ParseEqualityExpression(), BINARY_OPERATION_EQUAL);
    if(CheckNext(TOKEN_NOT_EQUAL))
        return BinaryExpressionNode(left, ParseEqualityExpression(), BINARY_OPERATION_NOT_EQUAL);
    
    return left;
}

ASTNode* ParseRelationalExpression() {
    ASTNode* left = ParseShiftExpression();

    if(CheckNext(TOKEN_GREATER))
        return BinaryExpressionNode(left, ParseRelationalExpression(), BINARY_OPERATION_GREATER);
    if(CheckNext(TOKEN_LESS))
        return BinaryExpressionNode(left, ParseRelationalExpression(), BINARY_OPERATION_LESS);
    if(CheckNext(TOKEN_GREATER_EQUAL))
        return BinaryExpressionNode(left, ParseRelationalExpression(), BINARY_OPERATION_GREATER_EQUAL);
    if(CheckNext(TOKEN_LESS_EQUAL))
        return BinaryExpressionNode(left, ParseRelationalExpression(), BINARY_OPERATION_LESS_EQUAL);

    return left;
}

ASTNode* ParseShiftExpression() {
    ASTNode* left = ParseAdditiveExpression();

    if(CheckNext(TOKEN_RIGHT_SHIFT))
        return BinaryExpressionNode(left, ParseShiftExpression(), BINARY_OPERATION_RIGHT_SHIFT);
    if(CheckNext(TOKEN_LEFT_SHIFT))
        return BinaryExpressionNode(left, ParseShiftExpression(), BINARY_OPERATION_LEFT_SHIFT);

    return left;
}

ASTNode* ParseAdditiveExpression() {
    ASTNode* left = ParseMultiplicativeExpression();

    if(CheckNext(TOKEN_ADD))
        return BinaryExpressionNode(left, ParseAdditiveExpression(), BINARY_OPERATION_ADD);
    if(CheckNext(TOKEN_SUBTRACT))
        return BinaryExpressionNode(left, ParseAdditiveExpression(), BINARY_OPERATION_SUBTRACT);

    return left;
}

ASTNode* ParseMultiplicativeExpression() {
    ASTNode* left = ParseUnaryExpression();

    if(CheckNext(TOKEN_MULTIPLY))
        return BinaryExpressionNode(left, ParseMultiplicativeExpression(), BINARY_OPERATION_MULTIPLY);
    if(CheckNext(TOKEN_DIVIDE))
        return BinaryExpressionNode(left, ParseMultiplicativeExpression(), BINARY_OPERATION_DIVIDE);
    if(CheckNext(TOKEN_MODULO))
        return BinaryExpressionNode(left, ParseMultiplicativeExpression(), BINARY_OPERATION_MODULO);

    return left;
}

ASTNode* ParseUnaryExpression() {
    if(CheckNext(TOKEN_LOGICAL_NOT))
        return UnaryExpressionNode(ParsePrimairyExpression(), UNARY_OPERATION_LOGICAL_NOT);
    if(CheckNext(TOKEN_SUBTRACT))
        return UnaryExpressionNode(ParsePrimairyExpression(), UNARY_OPERATION_NEGATE);
    if(CheckNext(TOKEN_BITWISE_NOT))
        return UnaryExpressionNode(ParsePrimairyExpression(), UNARY_OPERATION_BITWISE_NOT);
    else 
        return ParsePrimairyExpression();
}

ASTNode* ParsePrimairyExpression() {
    if(CheckNext(TOKEN_TRUE) || CheckNext(TOKEN_FALSE))
        return ValueNode(value);
    else if(CheckNext(TOKEN_INTEGER))
        return ValueNode(value);
    else if(CheckNext(TOKEN_REAL))
        return ValueNode(value);
    else if(CheckNext(TOKEN_IDENTIFIER)) {
        return ValueNode(value);
    } else if(CheckNext(TOKEN_OPEN_PARENTHESE)) {
        ASTNode* node = ParseExpression();
        if(!CheckNext(TOKEN_CLOSE_PARENTHESE))
            Error("No closing parenthese");
        return node;
    }
    Error("Parse PrimairyExpression Error");
}
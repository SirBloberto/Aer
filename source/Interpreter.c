#include "../include/Compiler.h"
#include "../include/Interpreter.h"
#include "../include/SymbolTable.h"

Value InterpretAssignmentExpression(ASTNode* node) {
    Value leftValue, rightValue;
    if(node->assignmentExpression.left)
        leftValue = Interpret(node->assignmentExpression.left);
    if(node->assignmentExpression.right)
        rightValue = Interpret(node->assignmentExpression.right);

    if(FindSymbol(leftValue.identifierValue) == -1)
        AddSymbol(leftValue.identifierValue);

    globalVariables[FindSymbol(leftValue.identifierValue)].value = rightValue;
}


Value InterpretBinaryExpression(ASTNode* node) {
    Value leftValue, rightValue;
    if(node->binaryExpression.left)
        leftValue = Interpret(node->binaryExpression.left);
    if(node->binaryExpression.right)
        rightValue = Interpret(node->binaryExpression.right);

    if(leftValue.type == TYPE_INTEGER && rightValue.type == TYPE_INTEGER) {
        Value resultValue;
        long leftInteger = CoerceInteger(leftValue);
        long rightInteger = CoerceInteger(rightValue);

        if(node->binaryExpression.operation == OPERATION_ADD)
            resultValue.integerValue = leftInteger + rightInteger;
        if(node->binaryExpression.operation == OPERATION_SUBTRACT)
            resultValue.integerValue = leftInteger - rightInteger;
        if(node->binaryExpression.operation == OPERATION_MULTIPLY)
            resultValue.integerValue = leftInteger * rightInteger;
        if(node->binaryExpression.operation == OPERATION_DIVIDE)
            resultValue.integerValue = leftInteger / rightInteger;
        if(node->binaryExpression.operation == OPERATION_MODULO)
            resultValue.integerValue = leftInteger % rightInteger;

        return resultValue;
    }
}

Value InterpretValue(ASTNode* node) {
    return node->value;
}

Value Interpret(ASTNode* node) {
    if(node->type == NODE_ASSIGNMENT_EXPRESSION)
        return InterpretAssignmentExpression(node);
    if(node->type == NODE_BINARY_EXPRESSION)
        return InterpretBinaryExpression(node);
    if(node->type == NODE_VALUE)
        return InterpretValue(node);
}
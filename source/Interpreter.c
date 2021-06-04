#include "../include/Compiler.h"
#include "../include/Error.h"
#include "../include/Interpreter.h"
#include "../include/SymbolTable.h"

Value InterpretAssignmentExpression(ASTNode* node) {
    Value leftValue = Interpret(node->assignmentExpression.left);
    Value rightValue = Interpret(node->assignmentExpression.right);

    if(FindSymbol(leftValue.identifierValue) == -1)
        AddSymbol(leftValue.identifierValue);

    globalVariables[FindSymbol(leftValue.identifierValue)].value = rightValue;

    return rightValue;
}

long InterpretBinaryExpressionInteger(long leftInteger, long rightInteger, BinaryOperation operation, Type* type) {
    switch(operation) {
        case BINARY_OPERATION_ADD:
            *type = TYPE_INTEGER;
            return leftInteger + rightInteger;
        case BINARY_OPERATION_SUBTRACT:
            *type = TYPE_INTEGER;
            return leftInteger - rightInteger;
        case BINARY_OPERATION_MULTIPLY:
            *type = TYPE_INTEGER;
            return leftInteger * rightInteger;
        case BINARY_OPERATION_DIVIDE:
            *type = TYPE_INTEGER;
            return leftInteger / rightInteger;
        case BINARY_OPERATION_MODULO:
            *type = TYPE_INTEGER;
            return leftInteger % rightInteger;
        case BINARY_OPERATION_EQUAL:
            *type = TYPE_BOOLEAN;
            return leftInteger == rightInteger;
        case BINARY_OPERATION_NOT_EQUAL:
            *type = TYPE_BOOLEAN;
            return leftInteger != rightInteger;
        case BINARY_OPERATION_GREATER:
            *type = TYPE_BOOLEAN;
            return leftInteger > rightInteger;
        case BINARY_OPERATION_LESS:
            *type = TYPE_BOOLEAN;
            return leftInteger < rightInteger;
        case BINARY_OPERATION_GREATER_EQUAL:
            *type = TYPE_BOOLEAN;
            return leftInteger >= rightInteger;
        case BINARY_OPERATION_LESS_EQUAL:
            *type = TYPE_BOOLEAN;
            return leftInteger <= rightInteger;
        default:
            Error("Interpret BinaryExpressionInteger Error");
    }
}

int IsBooleanBinaryOperation(BinaryOperation operation) {
    switch(operation) {
        case BINARY_OPERATION_EQUAL: return 1;
        case BINARY_OPERATION_NOT_EQUAL: return 1;
        case BINARY_OPERATION_GREATER: return 1;
        case BINARY_OPERATION_LESS: return 1;
        case BINARY_OPERATION_GREATER_EQUAL: return 1;
        case BINARY_OPERATION_LESS_EQUAL: return 1;
        default: return 0;
    }
}

Value InterpretBinaryExpression(ASTNode* node) {
    Value leftValue = Interpret(node->assignmentExpression.left);
    Value rightValue = Interpret(node->assignmentExpression.right);

    Value resultValue;
    if(leftValue.type == TYPE_INTEGER && rightValue.type == TYPE_INTEGER) {
        long leftInteger = CoerceInteger(leftValue);
        long rightInteger = CoerceInteger(rightValue);

        BinaryOperation operation = node->binaryExpression.operation;
        if(IsBooleanBinaryOperation(node->binaryExpression.operation)) {
            resultValue.type = TYPE_BOOLEAN;

            if(operation == BINARY_OPERATION_EQUAL)
                resultValue.booleanValue = leftInteger == rightInteger;
            if(operation == BINARY_OPERATION_NOT_EQUAL)
                resultValue.booleanValue = leftInteger != rightInteger;
            if(operation == BINARY_OPERATION_GREATER)
                resultValue.booleanValue = leftInteger > rightInteger;
            if(operation == BINARY_OPERATION_LESS)
                resultValue.booleanValue = leftInteger < rightInteger;
            if(operation == BINARY_OPERATION_GREATER_EQUAL)
                resultValue.booleanValue = leftInteger >= rightInteger;
            if(operation == BINARY_OPERATION_LESS_EQUAL)
                resultValue.booleanValue = leftInteger <= rightInteger;

            resultValue.booleanValue = CorrectBoolean(resultValue.booleanValue);
        } else {
            resultValue.type = TYPE_INTEGER;

            if(operation == BINARY_OPERATION_ADD)
                resultValue.integerValue = leftInteger + rightInteger;
            if(operation == BINARY_OPERATION_SUBTRACT)
                resultValue.integerValue = leftInteger - rightInteger;
            if(operation == BINARY_OPERATION_MULTIPLY)
                resultValue.integerValue = leftInteger * rightInteger;
            if(operation == BINARY_OPERATION_DIVIDE)
                resultValue.integerValue = leftInteger / rightInteger;
            if(operation == BINARY_OPERATION_MODULO)
                resultValue.integerValue = leftInteger % rightInteger;
        }
    }
    
    return resultValue;
}

Value InterpretValue(ASTNode* node) {
    return node->value;
}

Value Interpret(ASTNode* node) {
    switch(node->type) {
        case NODE_ASSIGNMENT_EXPRESSION:
            return InterpretAssignmentExpression(node);
        case NODE_BINARY_EXPRESSION:
            return InterpretBinaryExpression(node);
        case NODE_VALUE:
            return InterpretValue(node);
        default:
            Error("Interpret Error");
    }
}
#include "../include/Interpreter.h"

Value Interpret(ASTNode* node);

Value InterpretBinaryExpression(BinaryExpression binaryExpression) {
    Value leftValue = Interpret(binaryExpression.left);
    Value rightValue = Interpret(binaryExpression.right);

    if(leftValue.type == TYPE_INTEGER && rightValue.type == TYPE_INTEGER) {
        Value resultValue;
        long leftInteger = CoerceInteger(leftValue);
        long rightInteger = CoerceInteger(rightValue);

        if(binaryExpression.operation == OPERATION_ADD)
            resultValue.integerValue = leftInteger + rightInteger;
        if(binaryExpression.operation == OPERATION_SUBTRACT)
            resultValue.integerValue = leftInteger - rightInteger;
        if(binaryExpression.operation == OPERATION_MULTIPLY)
            resultValue.integerValue = leftInteger * rightInteger;
        if(binaryExpression.operation == OPERATION_DIVIDE)
            resultValue.integerValue = leftInteger / rightInteger;
        if(binaryExpression.operation == OPERATION_MODULO)
            resultValue.integerValue = leftInteger % rightInteger;

        return resultValue;
    }
}

Value InterpretValue(Value value) {
    return value;
}

Value Interpret(ASTNode* node) {
    if(node->type == NODE_BINARY_EXPRESSION)
        return InterpretBinaryExpression(node->binaryExpression);
    if(node->type == NODE_VALUE)
        return InterpretValue(node->value);
}
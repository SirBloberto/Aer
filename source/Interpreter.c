#include "../include/Interpreter.h"

int Interpret(ASTNode* node) {
    if(node->type == NODE_BINARY_EXPRESSION) {
        int leftValue, rightValue;
        if(node->binaryExpression.left)
            leftValue = Interpret(node->binaryExpression.left);
        if(node->binaryExpression.right)
            rightValue = Interpret(node->binaryExpression.right);

        switch(node->binaryExpression.operation) {
            case OPERATION_ADD: 
                return (leftValue + rightValue);
            case OPERATION_SUBTRACT:
                return (leftValue - rightValue);
            case OPERATION_MULTIPLY:
                return (leftValue * rightValue);
            case OPERATION_DIVIDE:
                return (leftValue / rightValue);
            case OPERATION_MODULO:
                return (leftValue % rightValue);
            default:
                printf("Error");
        }
    } else if(node->type == NODE_VALUE)
        return node->value.integerData;
    else
        printf("Error");
}
#include "../include/Compiler.h"
#include "../include/Error.h"
#include "../include/Interpreter.h"
#include "../include/SymbolTable.h"

Value InterpretAssignmentExpression(ASTNode* node) {
    Value leftValue = Interpret(node->assignmentExpression.left);
    Value rightValue = Interpret(node->assignmentExpression.right);

    AssignmentOperation operation = node->assignmentExpression.operation;
    int location = FindSymbol(leftValue.identifierValue);

    if(operation == ASSIGNMENT_OPERATION_ASSIGN) {
        if(location == -1)
            location = AddSymbol(leftValue.identifierValue);

        globalVariables[location].value = rightValue;
        return rightValue;
    } else if(location == -1)
        Error("Identifier does not exist cannot perform operation to nothing.");

    if(globalVariables[location].value.type == TYPE_INTEGER && rightValue.type == TYPE_INTEGER) {
        int rightInteger = CoerceInteger(rightValue);

        if(operation == ASSIGNMENT_OPERATION_ADD_ASSIGN)
            globalVariables[location].value.integerValue += rightInteger;
        else if(operation == ASSIGNMENT_OPERATION_SUBTRACT_ASSIGN)
            globalVariables[location].value.integerValue -= rightInteger;
        else if(operation == ASSIGNMENT_OPERATION_MULTIPLY_ASSIGN)
            globalVariables[location].value.integerValue *= rightInteger;
        else if(operation == ASSIGNMENT_OPERATION_DIVIDE_ASSIGN)
            globalVariables[location].value.integerValue /= rightInteger;
        else if(operation == ASSIGNMENT_OPERATION_MODULO_ASSIGN)
            globalVariables[location].value.integerValue %= rightInteger;
        else if(operation == ASSIGNMENT_OPERATION_LEFT_SHIFT_ASSIGN)
            globalVariables[location].value.integerValue <<= rightInteger;
        else if(operation == ASSIGNMENT_OPERATION_RIGHT_SHIFT_ASSIGN)
            globalVariables[location].value.integerValue >>= rightInteger;
        else if(operation == ASSIGNMENT_OPERATION_AND_ASSIGN)
            globalVariables[location].value.integerValue &= rightInteger;
        else if(operation == ASSIGNMENT_OPERATION_OR_ASSIGN)
            globalVariables[location].value.integerValue |= rightInteger;
        else if(operation == ASSIGNMENT_OPERATION_XOR_ASSIGN)
            globalVariables[location].value.integerValue ^= rightInteger;
    } else if((globalVariables[location].value.type == TYPE_REAL && rightValue.type == TYPE_REAL) ||
              (globalVariables[location].value.type == TYPE_INTEGER && rightValue.type == TYPE_REAL) ||
              (globalVariables[location].value.type == TYPE_REAL && rightValue.type == TYPE_INTEGER)) {

        globalVariables[location].value.realValue = CoerceReal(globalVariables[location].value);
        globalVariables[location].value.type = TYPE_REAL;
        double rightReal = CoerceReal(rightValue);

        if(operation == ASSIGNMENT_OPERATION_ADD_ASSIGN)
            globalVariables[location].value.realValue += rightReal;
        else if(operation == ASSIGNMENT_OPERATION_SUBTRACT_ASSIGN)
            globalVariables[location].value.realValue -= rightReal;
        else if(operation == ASSIGNMENT_OPERATION_MULTIPLY_ASSIGN)
            globalVariables[location].value.realValue *= rightReal;
        else if(operation == ASSIGNMENT_OPERATION_DIVIDE_ASSIGN)
            globalVariables[location].value.realValue /= rightReal;
    }

    return rightValue;
}

int IsBooleanBinaryOperation(BinaryOperation operation) {
    switch(operation) {
        case BINARY_OPERATION_LOGICAL_AND: return 1;
        case BINARY_OPERATION_LOGICAL_OR: return 1;
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
    Value leftValue = Interpret(node->binaryExpression.left);
    Value rightValue = Interpret(node->binaryExpression.right);

    Value resultValue;
    BinaryOperation operation = node->binaryExpression.operation;
    if(leftValue.type == TYPE_INTEGER && rightValue.type == TYPE_INTEGER) {
        long leftInteger = CoerceInteger(leftValue);
        long rightInteger = CoerceInteger(rightValue);

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
            
            if(operation == BINARY_OPERATION_BITWISE_AND)
                resultValue.integerValue = leftInteger & rightInteger;
            if(operation == BINARY_OPERATION_BITWISE_OR)
                resultValue.integerValue = leftInteger | rightInteger;
            if(operation == BINARY_OPERATION_BITWISE_XOR)
                resultValue.integerValue = leftInteger ^ rightInteger;
            if(operation == BINARY_OPERATION_LEFT_SHIFT)
                resultValue.integerValue = leftInteger << rightInteger;
            if(operation == BINARY_OPERATION_RIGHT_SHIFT)
                resultValue.integerValue = leftInteger >> rightInteger;
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
    } else if(leftValue.type == TYPE_BOOLEAN && rightValue.type == TYPE_BOOLEAN) {
        resultValue.type = TYPE_BOOLEAN;
        char leftBoolean = CorrectBoolean(leftValue.booleanValue);
        char rightBoolean = CorrectBoolean(rightValue.booleanValue);

        if(operation == BINARY_OPERATION_LOGICAL_AND)
            resultValue.booleanValue = leftBoolean && rightBoolean;
        if(operation == BINARY_OPERATION_LOGICAL_OR)
            resultValue.booleanValue = leftBoolean || rightBoolean;

        CorrectBoolean(resultValue.booleanValue);
    } else if((leftValue.type == TYPE_REAL && rightValue.type == TYPE_REAL) ||
              (leftValue.type == TYPE_INTEGER && rightValue.type == TYPE_REAL) ||
              (leftValue.type == TYPE_REAL && rightValue.type == TYPE_INTEGER)) {

        double leftReal = CoerceReal(leftValue);
        double rightReal = CoerceReal(rightValue);

        if(IsBooleanBinaryOperation(node->binaryExpression.operation)) {
            resultValue.type = TYPE_BOOLEAN;

            if(operation == BINARY_OPERATION_EQUAL)
                resultValue.booleanValue = leftReal == rightReal;
            if(operation == BINARY_OPERATION_NOT_EQUAL)
                resultValue.booleanValue = leftReal != rightReal;
            if(operation == BINARY_OPERATION_GREATER)
                resultValue.booleanValue = leftReal > rightReal;
            if(operation == BINARY_OPERATION_LESS)
                resultValue.booleanValue = leftReal < rightReal;
            if(operation == BINARY_OPERATION_GREATER_EQUAL)
                resultValue.booleanValue = leftReal >= rightReal;
            if(operation == BINARY_OPERATION_LESS_EQUAL)
                resultValue.booleanValue = leftReal <= rightReal;

            resultValue.booleanValue = CorrectBoolean(resultValue.booleanValue);
        } else {
            resultValue.type = TYPE_REAL;
            
            if(operation == BINARY_OPERATION_ADD)
                resultValue.realValue = leftReal + rightReal;
            if(operation == BINARY_OPERATION_SUBTRACT)
                resultValue.realValue = leftReal - rightReal;
            if(operation == BINARY_OPERATION_MULTIPLY)
                resultValue.realValue = leftReal * rightReal;
            if(operation == BINARY_OPERATION_DIVIDE)
                resultValue.realValue = leftReal / rightReal;
        }
    }
    
    return resultValue;
}

Value InterpretUnaryExpression(ASTNode* node) {
    Value value = Interpret(node->unaryExpression.node);

    UnaryOperation operation = node->unaryExpression.operation;
    if(value.type == TYPE_INTEGER) {
        long integerValue = CoerceInteger(value);

        if(operation == UNARY_OPERATION_NEGATE)
            value.integerValue = -integerValue;
        if(operation == UNARY_OPERATION_BITWISE_NOT)
            value.integerValue = ~integerValue;
    } else if(value.type == TYPE_BOOLEAN) {
        char booleanValue = CorrectBoolean(value.booleanValue);

        if(operation == UNARY_OPERATION_LOGICAL_NOT)
            value.booleanValue = !booleanValue;
    } else if(value.type == TYPE_REAL) {
        double realValue = value.realValue;

        if(operation == UNARY_OPERATION_NEGATE)
            value.realValue = -realValue;
    }

    return value;
}

Value InterpretValue(ASTNode* node) {
    return node->value;
}

Value InterpretCompoundStatement(ASTNode* node) {
    CompoundStatement compoundStatement = node->compoundStatement;

    for(int i = 0; i < compoundStatement.count; i++)
        Interpret(compoundStatement.nodes[i]);
}

Value InterpretIfStatement(ASTNode* node) {
    IfStatement ifStatement = node->ifStatement;

    Value value = Interpret(ifStatement.condition);
    if(value.type == TYPE_BOOLEAN) {
        if(value.booleanValue)
            Interpret(ifStatement.trueBlock);
        else if(ifStatement.falseBlock != 0)
            Interpret(ifStatement.falseBlock);
    } else {
        //Error
    }
}

Value Interpret(ASTNode* node) {
    switch(node->type) {
        case NODE_ASSIGNMENT_EXPRESSION:
            return InterpretAssignmentExpression(node);
        case NODE_BINARY_EXPRESSION:
            return InterpretBinaryExpression(node);
        case NODE_UNARY_EXPRESSION:
            return InterpretUnaryExpression(node);
        case NODE_VALUE:
            return InterpretValue(node);
        case NODE_COMPOUND_STATEMENT:
            return InterpretCompoundStatement(node);
        case NODE_IF_STATEMENT:
            return InterpretIfStatement(node);
        default:
            Error("Interpret Error");
    }
}
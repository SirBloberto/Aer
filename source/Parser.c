#include "../include/Lexer.h"
#include "../include/Parser.h"

ASTNode* AdditiveExpression();
ASTNode* Expression();
ASTNode* MultiplicativeExpression();
ASTNode* PrimairyExpression();

ASTNode* Parse() {
    while(token != TOKEN_END_OF_FILE) {
        Expression();
    }
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
    if(CheckNext(TOKEN_INTEGER)) {
        return ValueNode(value);
}
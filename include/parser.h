#pragma once
// =============================================================================
// 阶段 2：语法分析器 (Parser)
// =============================================================================
// 核心职责：将线性的 Token 流转换为一棵树状的 AST。
// 实现方式：递归下降解析（Recursive Descent Parsing）。
// 每个文法规则对应一个解析函数，函数之间互相调用形成递归。
// =============================================================================

#include "token.h"
#include "ast.h"
#include <vector>
#include <stdexcept>

namespace minicc {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    // 解析整个编译单元
    TranslationUnit parseTranslationUnit();

private:
    std::vector<Token> m_tokens;
    size_t             m_pos = 0;

    // ── Token 流操作 ──
    const Token& current() const;
    const Token& peek() const;
    const Token& advance();
    bool         check(TokenType t) const;
    bool         match(TokenType t);
    const Token& expect(TokenType t, const std::string& msg);
    bool         isAtEnd() const;

    // ── 错误处理 ──
    [[noreturn]] void error(const std::string& msg) const;
    [[noreturn]] void errorAt(const Token& tok, const std::string& msg) const;

    // ── 类型解析 ──
    TypePtr parseType();

    // ── 声明解析 ──
    DeclPtr        parseDeclaration();
    TemplateDeclPtr parseTemplateDecl();
    ClassDeclPtr    parseClassDecl();
    FuncDeclPtr     parseFunctionDecl(bool isVirtual = false,
                                      const std::string& ownerClass = "");
    FuncDeclPtr     parseMethodDecl(const std::string& ownerClass,
                                    AccessModifier access);

    // ── 语句解析 ──
    StmtPtr parseStatement();
    StmtPtr parseBlockStmt();
    StmtPtr parseVarDeclStmt(TypePtr type);
    StmtPtr parseIfStmt();
    StmtPtr parseWhileStmt();
    StmtPtr parseReturnStmt();
    StmtPtr parseExprOrAssignStmt();

    // ── 表达式解析（优先级从低到高） ──
    ExprPtr parseExpression();
    ExprPtr parseOrExpr();
    ExprPtr parseAndExpr();
    ExprPtr parseEqualityExpr();
    ExprPtr parseComparisonExpr();
    ExprPtr parseAdditiveExpr();
    ExprPtr parseMultiplicativeExpr();
    ExprPtr parseUnaryExpr();
    ExprPtr parsePostfixExpr();
    ExprPtr parsePrimaryExpr();

    // ── 辅助 ──
    std::vector<ExprPtr> parseArgumentList();
    std::vector<Parameter> parseParameterList();
    BinaryOp tokenToBinaryOp(TokenType t) const;
};

} // namespace minicc

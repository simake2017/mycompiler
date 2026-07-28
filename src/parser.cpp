// =============================================================================
// 阶段 2：语法分析器实现 —— 递归下降解析
// =============================================================================
// 递归下降是最直观的解析方法：每个文法规则变成一个函数，
// 函数调用自身或其他规则函数来匹配输入，自然地形成一棵 AST。
//
// 优先级处理：
//   表达式的解析按优先级从低到高层层嵌套：
//     parseExpression → parseOrExpr → parseAndExpr → ... → parsePrimaryExpr
//   低优先级函数调用高优先级函数来获取操作数，从而保证优先级正确。
// =============================================================================

#include "parser.h"
#include <format>
#include <iostream>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// 构造
// ─────────────────────────────────────────────────────────────────────────────
Parser::Parser(std::vector<Token> tokens)
    : m_tokens(std::move(tokens)) {}

// ─────────────────────────────────────────────────────────────────────────────
// Token 流操作
// ─────────────────────────────────────────────────────────────────────────────
const Token& Parser::current() const {
    return m_tokens[m_pos];
}

const Token& Parser::peek() const {
    return m_tokens[m_pos];
}

const Token& Parser::advance() {
    const Token& tok = m_tokens[m_pos];
    if (!isAtEnd()) m_pos++;
    return tok;
}

bool Parser::check(TokenType t) const {
    return current().type == t;
}

bool Parser::match(TokenType t) {
    if (check(t)) {
        advance();
        return true;
    }
    return false;
}

const Token& Parser::expect(TokenType t, const std::string& msg) {
    if (check(t)) {
        return advance();
    }
    errorAt(current(), std::format("{}: expected {}, got '{}'",
        msg, tokenTypeName(t), current().text));
}

bool Parser::isAtEnd() const {
    return m_pos >= m_tokens.size() || current().is(TokenType::Eof);
}

// ─────────────────────────────────────────────────────────────────────────────
// 错误处理
// ─────────────────────────────────────────────────────────────────────────────
[[noreturn]] void Parser::error(const std::string& msg) const {
    errorAt(current(), msg);
}

[[noreturn]] void Parser::errorAt(const Token& tok, const std::string& msg) const {
    throw std::runtime_error(
        std::format("[Parse Error] {} at '{}': {}",
            tok.location.toString(), tok.text, msg));
}

// ─────────────────────────────────────────────────────────────────────────────
// 类型解析
// ─────────────────────────────────────────────────────────────────────────────
// 支持:
//   基础类型: int, double, bool, void, auto
//   类名/模板参数名: MyClass, T
//   指针: T*, T**, T***
//   左值引用: T&
//   右值引用: T&&
//   常量: const T
//   组合: const T&, const T&&, T*&, const T*
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Parser::parseType() {
    // ── Step 1: 处理 const 前缀 ──
    bool isConst = false;
    if (check(TokenType::KwConst)) {
        isConst = true;
        advance();
        std::cout << std::format("  [parse:type] ✦ const prefix detected\n");
    }

    // ── Step 2: 解析基础类型 ──
    TypePtr base;

    if (match(TokenType::KwInt)) {
        base = Type::makeInt();
        std::cout << std::format("  [parse:type] base = int\n");
    }
    else if (match(TokenType::KwDouble)) {
        base = Type::makeDouble();
        std::cout << std::format("  [parse:type] base = double\n");
    }
    else if (match(TokenType::KwBool)) {
        base = Type::makeBool();
        std::cout << std::format("  [parse:type] base = bool\n");
    }
    else if (match(TokenType::KwVoid)) {
        base = Type::makeVoid();
        std::cout << std::format("  [parse:type] base = void\n");
    }
    else if (match(TokenType::KwAuto)) {
        base = Type::makeAuto();
        std::cout << std::format("  [parse:type] base = auto\n");
    }
    else if (check(TokenType::Identifier)) {
        // 类名或模板参数名
        std::string name = advance().text;
        base = Type::makeClass(name);
        std::cout << std::format("  [parse:type] base = {} (class/tparam)\n", name);
    }
    else {
        error("Expected type name");
    }

    // ── Step 3: 处理后缀修饰符（*, &, &&）──
    // 注意：后缀可以连续出现，如 T*&, const T**
    while (true) {
        if (match(TokenType::Star)) {
            // 指针: T*
            base = Type::makePointer(base);
            std::cout << std::format("  [parse:type] suffix * → {}\n", base->toString());
        }
        else if (check(TokenType::AmpAmp)) {
            // 右值引用: T&&
            // 注意：AmpAmp 是 &&（逻辑与 Token），在类型上下文中表示右值引用
            advance();
            base = Type::makeRValueReference(base);
            std::cout << std::format("  [parse:type] suffix && → {} (rvalue ref)\n",
                base->toString());
        }
        else if (check(TokenType::Ampersand)) {
            // 左值引用: T&
            advance();
            base = Type::makeLValueReference(base);
            std::cout << std::format("  [parse:type] suffix & → {} (lvalue ref)\n",
                base->toString());
        }
        else {
            break; // 没有更多后缀修饰符
        }
    }

    // ── Step 4: 应用 const 限定 ──
    if (isConst) {
        base = Type::makeConst(base);
        std::cout << std::format("  [parse:type] const applied → {}\n", base->toString());
    }

    std::cout << std::format("  [parse:type] ★ final type = {}\n", base->toString());
    return base;
}

// ─────────────────────────────────────────────────────────────────────────────
// 编译单元
// ─────────────────────────────────────────────────────────────────────────────
TranslationUnit Parser::parseTranslationUnit() {
    TranslationUnit unit;
    while (!isAtEnd()) {
        unit.declarations.push_back(parseDeclaration());
    }
    return unit;
}

// ─────────────────────────────────────────────────────────────────────────────
// 声明解析：根据 Token 类型分派到具体解析函数
// ─────────────────────────────────────────────────────────────────────────────
DeclPtr Parser::parseDeclaration() {
    // template<typename T> ...
    if (check(TokenType::KwTemplate)) {
        return parseTemplateDecl();
    }

    // class ...
    if (check(TokenType::KwClass)) {
        return parseClassDecl();
    }

    // 函数声明（可能是 virtual）
    bool isVirtual = false;
    if (check(TokenType::KwVirtual)) {
        isVirtual = true;
    }

    // 尝试解析函数声明：类型 名字 ( 参数列表 ) { ... }
    return parseFunctionDecl(isVirtual);
}

// ─────────────────────────────────────────────────────────────────────────────
// 模板声明：template<typename T> class MyPtr { ... };
//          template<class T, class U> class Pair { ... };
// ─────────────────────────────────────────────────────────────────────────────
// Parser 将模板声明作为"蓝图"存储，暂不进行语义分析。
// 等到模板实例化阶段（阶段4）才克隆并替换模板参数。
//
// 支持的模板参数声明方式：
//   template<typename T>          ← 标准写法
//   template<class T>             ← C++ 中等价于 typename
//   template<typename T, typename U>  ← 多参数
//   template<class T, class U>    ← 混合也可以
// ─────────────────────────────────────────────────────────────────────────────
TemplateDeclPtr Parser::parseTemplateDecl() {
    auto decl = std::make_shared<TemplateDecl>();
    decl->location = current().location;

    std::cout << std::format("  [parse:template] ▶ template declaration at {}\n",
        current().location.toString());

    expect(TokenType::KwTemplate, "Expected 'template'");
    expect(TokenType::Less, "Expected '<' after 'template'");

    std::cout << "  [parse:template]   parsing parameter list <";

    // 解析模板参数列表
    do {
        // ★ 同时支持 typename 和 class 关键字 ★
        // C++ 标准中 template<class T> 和 template<typename T> 完全等价
        if (check(TokenType::KwTypename)) {
            advance();
            std::cout << "typename ";
        }
        else if (check(TokenType::KwClass)) {
            advance();
            std::cout << "class ";
        }
        else {
            errorAt(current(),
                std::format("Expected 'typename' or 'class' in template parameter, got '{}'",
                    current().text));
        }

        const Token& paramName = expect(TokenType::Identifier, "Expected parameter name");
        decl->typeParams.push_back(paramName.text);
        std::cout << std::format("{}", paramName.text);

        std::cout << std::format("\n  [parse:template]   ★ type parameter registered: '{}'\n",
            paramName.text);

    } while (match(TokenType::Comma) && (std::cout << ", ", true));

    std::cout << ">\n";

    expect(TokenType::Greater, "Expected '>' after template parameters");

    // 解析模板类
    std::cout << std::format("  [parse:template]   parsing class body for template...\n");
    decl->classTemplate = parseClassDecl();

    std::cout << std::format("  [parse:template] ◀ template '{}' with {} parameter(s) stored as blueprint\n",
        decl->classTemplate->name, decl->typeParams.size());

    // 打印蓝图摘要
    std::cout << std::format("  [parse:template]   blueprint summary:\n");
    std::cout << std::format("    template <");
    for (size_t i = 0; i < decl->typeParams.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << "typename " << decl->typeParams[i];
    }
    std::cout << std::format("> class {} {{ ... }}\n", decl->classTemplate->name);

    return decl;
}

// ─────────────────────────────────────────────────────────────────────────────
// 类声明：class Name [: public Base] { ... };
// ─────────────────────────────────────────────────────────────────────────────
ClassDeclPtr Parser::parseClassDecl() {
    auto decl = std::make_shared<ClassDecl>();
    decl->location = current().location;

    expect(TokenType::KwClass, "Expected 'class'");
    const Token& nameToken = expect(TokenType::Identifier, "Expected class name");
    decl->name = nameToken.text;

    // 可选的继承
    if (match(TokenType::Colon)) {
        match(TokenType::KwPublic); // 简化：只支持 public 继承
        const Token& baseName = expect(TokenType::Identifier, "Expected base class name");
        decl->baseClassName = baseName.text;
    }

    expect(TokenType::LBrace, "Expected '{' after class name");

    // 解析类成员
    while (!check(TokenType::RBrace) && !isAtEnd()) {
        // 访问修饰符
        AccessModifier access = decl->currentAccess;
        if (check(TokenType::KwPublic)) {
            advance();
            expect(TokenType::Colon, "Expected ':' after 'public'");
            decl->currentAccess = AccessModifier::Public;
            access = AccessModifier::Public;
            continue;
        }
        if (check(TokenType::KwPrivate)) {
            advance();
            expect(TokenType::Colon, "Expected ':' after 'private'");
            decl->currentAccess = AccessModifier::Private;
            access = AccessModifier::Private;
            continue;
        }
        if (check(TokenType::KwProtected)) {
            advance();
            expect(TokenType::Colon, "Expected ':' after 'protected'");
            decl->currentAccess = AccessModifier::Protected;
            access = AccessModifier::Protected;
            continue;
        }

        // 虚函数
        bool isVirtual = false;
        if (check(TokenType::KwVirtual)) {
            isVirtual = true;
        }

        // 尝试判断是方法还是字段
        // 向前看：如果看到 类型 名字 ( → 方法
        // 否则 → 字段
        size_t savedPos = m_pos;
        bool isMethod = false;

        // 跳过 virtual
        if (isVirtual) {
            advance();
        }

        // 跳过类型
        parseType();

        // 名字
        if (check(TokenType::Identifier)) {
            advance();
            if (check(TokenType::LParen)) {
                isMethod = true;
            }
        }

        // 恢复位置
        m_pos = savedPos;

        if (isMethod) {
            auto method = parseMethodDecl(decl->name, access);
            method->isVirtual = isVirtual;
            if (isVirtual) {
                // 消费 virtual 关键字（parseMethodDecl 不会消费它）
                // 实际上 parseMethodDecl 从 parseFunctionDecl 调用
                // 我们需要在 parseFunctionDecl 之前消费 virtual
            }
            decl->methods.push_back(method);
        } else {
            // 字段声明
            if (isVirtual) advance(); // 消费 virtual（不应该出现在字段上，但容错）

            TypePtr fieldType = parseType();
            const Token& fieldName = expect(TokenType::Identifier, "Expected field name");
            expect(TokenType::Semicolon, "Expected ';' after field declaration");

            FieldInfo field;
            field.name = fieldName.text;
            field.type = fieldType;
            field.access = access;
            decl->fields.push_back(field);
        }
    }

    expect(TokenType::RBrace, "Expected '}' after class body");
    expect(TokenType::Semicolon, "Expected ';' after class definition");

    return decl;
}

// ─────────────────────────────────────────────────────────────────────────────
// 方法声明
// ─────────────────────────────────────────────────────────────────────────────
FuncDeclPtr Parser::parseMethodDecl(const std::string& ownerClass,
                                     AccessModifier access) {
    bool isVirtual = false;
    if (check(TokenType::KwVirtual)) {
        advance();
        isVirtual = true;
    }

    auto func = parseFunctionDecl(false, ownerClass);
    func->isVirtual = isVirtual;

    // 检查 override
    // override 出现在函数声明的末尾（在 { 或 ; 之前）
    // 由于 parseFunctionDecl 已经解析了函数体和 ;，这里需要特殊处理
    // 简化处理：在 parseFunctionDecl 中处理 override

    return func;
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数声明：返回类型 函数名 ( 参数列表 ) { 函数体 }
// ─────────────────────────────────────────────────────────────────────────────
FuncDeclPtr Parser::parseFunctionDecl(bool isVirtual, const std::string& ownerClass) {
    auto decl = std::make_shared<FunctionDecl>();
    decl->location = current().location;
    decl->isVirtual = isVirtual;
    decl->ownerClassName = ownerClass;

    // 跳过 virtual（如果在 parseMethodDecl 中没有被消费）
    if (check(TokenType::KwVirtual)) {
        advance();
    }

    // 返回类型
    decl->returnType = parseType();

    // 函数名
    const Token& nameToken = expect(TokenType::Identifier, "Expected function name");
    decl->name = nameToken.text;

    // 参数列表
    expect(TokenType::LParen, "Expected '(' after function name");
    decl->parameters = parseParameterList();
    expect(TokenType::RParen, "Expected ')' after parameters");

    // override 关键字（在 { 或 ; 之前）
    if (match(TokenType::KwOverride)) {
        decl->isOverride = true;
    }

    // 函数体或分号
    if (check(TokenType::LBrace)) {
        decl->body = std::static_pointer_cast<BlockStmt>(parseBlockStmt());
    } else {
        expect(TokenType::Semicolon, "Expected '{' or ';' after function declaration");
    }

    return decl;
}

// ─────────────────────────────────────────────────────────────────────────────
// 参数列表
// ─────────────────────────────────────────────────────────────────────────────
std::vector<Parameter> Parser::parseParameterList() {
    std::vector<Parameter> params;

    if (check(TokenType::RParen)) return params; // 空参数列表

    do {
        Parameter param;
        param.type = parseType();
        const Token& nameToken = expect(TokenType::Identifier, "Expected parameter name");
        param.name = nameToken.text;
        params.push_back(param);
    } while (match(TokenType::Comma));

    return params;
}

// ─────────────────────────────────────────────────────────────────────────────
// 语句解析
// ─────────────────────────────────────────────────────────────────────────────
StmtPtr Parser::parseStatement() {
    if (check(TokenType::LBrace))    return parseBlockStmt();
    if (check(TokenType::KwIf))      return parseIfStmt();
    if (check(TokenType::KwWhile))   return parseWhileStmt();
    if (check(TokenType::KwReturn))  return parseReturnStmt();

    // 变量声明：以类型关键字开头
    if (current().isTypeKeyword()) {
        TypePtr type = parseType();
        return parseVarDeclStmt(type);
    }

    // 类名开头的变量声明（需要向前看）
    if (check(TokenType::Identifier)) {
        // 向前看：如果是 标识符 标识符 ; 或 标识符 标识符 = → 变量声明
        size_t savedPos = m_pos;
        std::string firstName = advance().text;

        // 检查是否是 类名 * → 指针类型
        while (match(TokenType::Star)) {} // 跳过指针标记

        if (check(TokenType::Identifier)) {
            // 是变量声明
            m_pos = savedPos;
            TypePtr type = parseType();
            return parseVarDeclStmt(type);
        }

        // 不是变量声明，恢复位置，当作表达式语句
        m_pos = savedPos;
    }

    return parseExprOrAssignStmt();
}

// ─── 代码块 ──────────────────────────────────────────────────────────────────
StmtPtr Parser::parseBlockStmt() {
    expect(TokenType::LBrace, "Expected '{'");
    std::vector<StmtPtr> stmts;

    while (!check(TokenType::RBrace) && !isAtEnd()) {
        stmts.push_back(parseStatement());
    }

    expect(TokenType::RBrace, "Expected '}'");
    return std::make_shared<BlockStmt>(std::move(stmts));
}

// ─── 变量声明 ─────────────────────────────────────────────────────────────────
StmtPtr Parser::parseVarDeclStmt(TypePtr type) {
    auto loc = current().location;
    const Token& nameToken = expect(TokenType::Identifier, "Expected variable name");

    ExprPtr init = nullptr;
    if (match(TokenType::Assign)) {
        init = parseExpression();
    }

    expect(TokenType::Semicolon, "Expected ';' after variable declaration");

    auto stmt = std::make_shared<VarDeclStmt>(nameToken.text, type, init);
    stmt->location = loc;
    return stmt;
}

// ─── if 语句 ──────────────────────────────────────────────────────────────────
StmtPtr Parser::parseIfStmt() {
    auto loc = current().location;
    expect(TokenType::KwIf, "Expected 'if'");
    expect(TokenType::LParen, "Expected '(' after 'if'");

    ExprPtr cond = parseExpression();

    expect(TokenType::RParen, "Expected ')' after condition");

    StmtPtr thenBranch = parseStatement();
    StmtPtr elseBranch = nullptr;

    if (match(TokenType::KwElse)) {
        elseBranch = parseStatement();
    }

    auto stmt = std::make_shared<IfStmt>(cond, thenBranch, elseBranch);
    stmt->location = loc;
    return stmt;
}

// ─── while 语句 ───────────────────────────────────────────────────────────────
StmtPtr Parser::parseWhileStmt() {
    auto loc = current().location;
    expect(TokenType::KwWhile, "Expected 'while'");
    expect(TokenType::LParen, "Expected '(' after 'while'");

    ExprPtr cond = parseExpression();

    expect(TokenType::RParen, "Expected ')' after condition");

    StmtPtr body = parseStatement();

    auto stmt = std::make_shared<WhileStmt>(cond, body);
    stmt->location = loc;
    return stmt;
}

// ─── return 语句 ──────────────────────────────────────────────────────────────
StmtPtr Parser::parseReturnStmt() {
    auto loc = current().location;
    expect(TokenType::KwReturn, "Expected 'return'");

    ExprPtr value = nullptr;
    if (!check(TokenType::Semicolon)) {
        value = parseExpression();
    }

    expect(TokenType::Semicolon, "Expected ';' after return");

    auto stmt = std::make_shared<ReturnStmt>(value);
    stmt->location = loc;
    return stmt;
}

// ─── 表达式语句或赋值语句 ─────────────────────────────────────────────────────
StmtPtr Parser::parseExprOrAssignStmt() {
    ExprPtr expr = parseExpression();

    // 赋值语句
    if (match(TokenType::Assign)) {
        ExprPtr value = parseExpression();
        expect(TokenType::Semicolon, "Expected ';' after assignment");
        auto stmt = std::make_shared<AssignStmt>(expr, value);
        stmt->location = expr->location;
        return stmt;
    }

    expect(TokenType::Semicolon, "Expected ';' after expression");
    auto stmt = std::make_shared<ExprStmt>(expr);
    stmt->location = expr->location;
    return stmt;
}

// ─────────────────────────────────────────────────────────────────────────────
// 表达式解析（优先级从低到高）
// ─────────────────────────────────────────────────────────────────────────────

ExprPtr Parser::parseExpression() {
    return parseOrExpr();
}

// ─── || ──────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseOrExpr() {
    ExprPtr left = parseAndExpr();

    while (check(TokenType::PipePipe)) {
        auto loc = current().location;
        advance();
        ExprPtr right = parseAndExpr();
        auto expr = std::make_shared<BinaryExpr>(BinaryOp::Or, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// ─── && ──────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseAndExpr() {
    ExprPtr left = parseEqualityExpr();

    while (check(TokenType::AmpAmp)) {
        auto loc = current().location;
        advance();
        ExprPtr right = parseEqualityExpr();
        auto expr = std::make_shared<BinaryExpr>(BinaryOp::And, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// ─── == != ───────────────────────────────────────────────────────────────────
ExprPtr Parser::parseEqualityExpr() {
    ExprPtr left = parseComparisonExpr();

    while (check(TokenType::EqualEqual) || check(TokenType::BangEqual)) {
        auto loc = current().location;
        BinaryOp op = (current().type == TokenType::EqualEqual)
                      ? BinaryOp::Eq : BinaryOp::Neq;
        advance();
        ExprPtr right = parseComparisonExpr();
        auto expr = std::make_shared<BinaryExpr>(op, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// ─── < > <= >= ──────────────────────────────────────────────────────────────
ExprPtr Parser::parseComparisonExpr() {
    ExprPtr left = parseAdditiveExpr();

    while (check(TokenType::Less) || check(TokenType::Greater)
           || check(TokenType::LessEqual) || check(TokenType::GreaterEqual)) {
        auto loc = current().location;
        BinaryOp op;
        switch (current().type) {
            case TokenType::Less:         op = BinaryOp::Lt;  break;
            case TokenType::Greater:      op = BinaryOp::Gt;  break;
            case TokenType::LessEqual:    op = BinaryOp::Le;  break;
            case TokenType::GreaterEqual: op = BinaryOp::Ge;  break;
            default:                      op = BinaryOp::Lt;  break;
        }
        advance();
        ExprPtr right = parseAdditiveExpr();
        auto expr = std::make_shared<BinaryExpr>(op, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// ─── + - ─────────────────────────────────────────────────────────────────────
ExprPtr Parser::parseAdditiveExpr() {
    ExprPtr left = parseMultiplicativeExpr();

    while (check(TokenType::Plus) || check(TokenType::Minus)) {
        auto loc = current().location;
        BinaryOp op = (current().type == TokenType::Plus)
                      ? BinaryOp::Add : BinaryOp::Sub;
        advance();
        ExprPtr right = parseMultiplicativeExpr();
        auto expr = std::make_shared<BinaryExpr>(op, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// ─── * / % ───────────────────────────────────────────────────────────────────
ExprPtr Parser::parseMultiplicativeExpr() {
    ExprPtr left = parseUnaryExpr();

    while (check(TokenType::Star) || check(TokenType::Slash)
           || check(TokenType::Percent)) {
        auto loc = current().location;
        BinaryOp op;
        switch (current().type) {
            case TokenType::Star:    op = BinaryOp::Mul; break;
            case TokenType::Slash:   op = BinaryOp::Div; break;
            case TokenType::Percent: op = BinaryOp::Mod; break;
            default:                 op = BinaryOp::Mul; break;
        }
        advance();
        ExprPtr right = parseUnaryExpr();
        auto expr = std::make_shared<BinaryExpr>(op, left, right);
        expr->location = loc;
        left = expr;
    }

    return left;
}

// ─── 一元表达式：-x, !x ─────────────────────────────────────────────────────
ExprPtr Parser::parseUnaryExpr() {
    if (check(TokenType::Minus) || check(TokenType::Bang)) {
        auto loc = current().location;
        UnaryOp op = (current().type == TokenType::Minus)
                     ? UnaryOp::Neg : UnaryOp::Not;
        advance();
        ExprPtr operand = parseUnaryExpr();
        auto expr = std::make_shared<UnaryExpr>(op, operand);
        expr->location = loc;
        return expr;
    }

    return parsePostfixExpr();
}

// ─── 后缀表达式：函数调用、成员访问 ─────────────────────────────────────────
ExprPtr Parser::parsePostfixExpr() {
    ExprPtr expr = parsePrimaryExpr();

    while (true) {
        if (check(TokenType::LParen)) {
            // 函数调用
            auto loc = current().location;
            auto args = parseArgumentList();
            auto call = std::make_shared<CallExpr>(expr, std::move(args));
            call->location = loc;
            expr = call;
        }
        else if (check(TokenType::Dot) || check(TokenType::Arrow)) {
            // 成员访问
            auto loc = current().location;
            bool isArrow = (current().type == TokenType::Arrow);
            advance();
            const Token& member = expect(TokenType::Identifier, "Expected member name");
            auto memExpr = std::make_shared<MemberExpr>(expr, member.text, isArrow);
            memExpr->location = loc;
            expr = memExpr;
        }
        else {
            break;
        }
    }

    return expr;
}

// ─── 基本表达式：字面量、变量、new、this、括号表达式 ────────────────────────
ExprPtr Parser::parsePrimaryExpr() {
    auto loc = current().location;

    // 整数字面量
    if (check(TokenType::IntLiteral)) {
        int64_t val = std::stoll(advance().text);
        auto expr = std::make_shared<IntLiteralExpr>(val);
        expr->location = loc;
        return expr;
    }

    // 布尔字面量
    if (check(TokenType::KwTrue)) {
        advance();
        auto expr = std::make_shared<BoolLiteralExpr>(true);
        expr->location = loc;
        return expr;
    }
    if (check(TokenType::KwFalse)) {
        advance();
        auto expr = std::make_shared<BoolLiteralExpr>(false);
        expr->location = loc;
        return expr;
    }

    // 字符串字面量
    if (check(TokenType::StringLiteral)) {
        std::string val = advance().text;
        auto expr = std::make_shared<StringLiteralExpr>(val);
        expr->location = loc;
        return expr;
    }

    // nullptr
    if (check(TokenType::KwNullptr)) {
        advance();
        auto expr = std::make_shared<NullptrLiteralExpr>();
        expr->location = loc;
        return expr;
    }

    // this
    if (check(TokenType::KwThis)) {
        advance();
        auto expr = std::make_shared<ThisExpr>();
        expr->location = loc;
        return expr;
    }

    // new 表达式
    if (check(TokenType::Identifier) && current().text == "new") {
        advance(); // 消费 new（简化处理：把 new 当作标识符）
        const Token& className = expect(TokenType::Identifier, "Expected class name after 'new'");
        auto expr = std::make_shared<NewExpr>(className.text);
        expr->location = loc;

        // 可选的构造参数
        if (match(TokenType::LParen)) {
            if (!check(TokenType::RParen)) {
                do {
                    expr->constructorArgs.push_back(parseExpression());
                } while (match(TokenType::Comma));
            }
            expect(TokenType::RParen, "Expected ')' after constructor arguments");
        }

        return expr;
    }

    // 变量引用
    if (check(TokenType::Identifier)) {
        std::string name = advance().text;
        auto expr = std::make_shared<VarExpr>(name);
        expr->location = loc;
        return expr;
    }

    // 括号表达式
    if (match(TokenType::LParen)) {
        ExprPtr inner = parseExpression();
        expect(TokenType::RParen, "Expected ')' after expression");
        return inner;
    }

    error(std::format("Unexpected token '{}' in expression", current().text));
}

// ─────────────────────────────────────────────────────────────────────────────
// 参数列表（函数调用）
// ─────────────────────────────────────────────────────────────────────────────
std::vector<ExprPtr> Parser::parseArgumentList() {
    expect(TokenType::LParen, "Expected '('");
    std::vector<ExprPtr> args;

    if (!check(TokenType::RParen)) {
        do {
            args.push_back(parseExpression());
        } while (match(TokenType::Comma));
    }

    expect(TokenType::RParen, "Expected ')'");
    return args;
}

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：Token 类型转二元运算符
// ─────────────────────────────────────────────────────────────────────────────
BinaryOp Parser::tokenToBinaryOp(TokenType t) const {
    switch (t) {
        case TokenType::Plus:         return BinaryOp::Add;
        case TokenType::Minus:        return BinaryOp::Sub;
        case TokenType::Star:         return BinaryOp::Mul;
        case TokenType::Slash:        return BinaryOp::Div;
        case TokenType::Percent:      return BinaryOp::Mod;
        case TokenType::EqualEqual:   return BinaryOp::Eq;
        case TokenType::BangEqual:    return BinaryOp::Neq;
        case TokenType::Less:         return BinaryOp::Lt;
        case TokenType::Greater:      return BinaryOp::Gt;
        case TokenType::LessEqual:    return BinaryOp::Le;
        case TokenType::GreaterEqual: return BinaryOp::Ge;
        case TokenType::AmpAmp:       return BinaryOp::And;
        case TokenType::PipePipe:     return BinaryOp::Or;
        default:                      return BinaryOp::Add;
    }
}

} // namespace minicc

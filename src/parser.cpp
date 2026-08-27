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
//
// 【在编译管线中的位置】
//   阶段0 Preprocessor → 阶段1 Lexer → ★ 阶段2 Parser ★ → 阶段3 SemanticAnalyzer
//   → 阶段4 TemplateDeduction/Instantiation → 阶段5 CodeGen(.s)
//   输入：std::vector<Token>（线性词法流）；输出：TranslationUnit（AST 根）。
//   本阶段只做结构识别，不查符号表、不做类型检查（那是阶段3的职责）。
//
// 【理论背景】
//   · 递归下降（Recursive Descent）：自顶向下解析的教科书实现。文法中每个
//     非终结符对应一个函数；LL(1) 性质保证每步分支决策只需 1 个前瞻 Token。
//   · 前瞻与回溯：遇到 LL(1) 无法单 Token 判定的歧义（如类体内"方法 vs
//     字段"、语句中"类名变量声明 vs 表达式"、template-id vs 比较表达式），
//     采用"保存游标 → 试探解析 → 失败回滚"的试探法（speculative parsing）。
//   · EBNF 文法骨架：
//       translation-unit := declaration*
//       declaration      := template-decl | class-decl | ['virtual'] function-decl
//       stmt             := '{' stmt* '}' | 'if' | 'while' | 'return' | var-decl | expr-stmt
//       expr             := 优先级链 || > && > ==/!= > 比较 > +/- > */% > 一元 > 后缀 > primary
//   · most-vexing-parse 简化：真 C++ 中 `T x(Foo());` 按 [stmt.dcl] 会被解析
//     为函数声明（"最令人头疼的解析"）。本编译器不支持括号初始化，语句级
//     变量声明仅识别 `类型 名字 [= expr];` 形式，类体内用"名字后是否跟 '('"
//     区分方法/字段——用最简单的判据回避该歧义。
//
// 【对应 clang 模块】（参照源码 llvm-project/clang/lib/Parse/）
//   Parser.cpp            → ParseTopLevelDecl / ParseStatement（对应本文件的
//                           parseTranslationUnit / parseStatement 分派）
//   ParseDecl.cpp         → ParseDeclOrFunctionDefInternal / ParseCXXClassMemberDecl
//                           （对应 parseFunctionDecl / parseClassDecl）
//   ParseTemplate.cpp     → ParseTemplateDeclaration / ParseTemplateParameters
//                           （对应 parseTemplateDecl）
//   ParseExpr.cpp         → ParseExpression / 优先级链（对应 parseExpression 系列）
//   ParseExprCXX.cpp      → template-id 歧义消解（对应 parsePrimaryExpr 中 '<' 试探）
//
// 【调用关系总览（ASCII）】
//   parseTranslationUnit
//    └─ parseDeclaration ─┬─ parseTemplateDecl ─┬─ parseClassDecl
//                         │                     └─ parseFunctionDecl
//                         ├─ parseClassDecl ──┬─ parseMethodDecl → parseFunctionDecl
//                         │                   └─ 字段（内联解析）
//                         └─ parseFunctionDecl ─┬─ parseType
//                                               ├─ parseParameterList
//                                               └─ parseBlockStmt → parseStatement
//   parseStatement ─┬─ parseBlockStmt / parseIfStmt / parseWhileStmt / parseReturnStmt
//                   ├─ parseType + parseVarDeclStmt
//                   └─ parseExprOrAssignStmt → parseExpression
//   parseExpression → parseOrExpr → parseAndExpr → parseEqualityExpr
//     → parseComparisonExpr → parseAdditiveExpr → parseMultiplicativeExpr
//     → parseUnaryExpr → parsePostfixExpr → parsePrimaryExpr
// =============================================================================

#include "parser.h"
#include <format>
#include <iostream>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// 构造
// ─────────────────────────────────────────────────────────────────────────────
// 文法：（无）—— 仅持有 Token 流，游标 m_pos 初始化为 0（指向第一个 Token）。
// 入参 demo：tokens = [int][main][(][)][{][return][0][;][}][Eof]
//           构造后 m_pos = 0 → current() = [int]
Parser::Parser(std::vector<Token> tokens)
    : m_tokens(std::move(tokens)) {}

// ─────────────────────────────────────────────────────────────────────────────
// Token 流操作
// ─────────────────────────────────────────────────────────────────────────────
// ── 前瞻原语：返回游标所指 Token，不移动 m_pos。所有分支决策的信息来源。──
const Token& Parser::current() const {
    return m_tokens[m_pos];
}

// ── peek 与 current 实现相同，保留两个名字是为了调用点语义清晰：
//    current() = "我正要处理的 Token"，peek() = "只瞄一眼决定走哪条路"。──
const Token& Parser::peek() const {
    return m_tokens[m_pos];
}

// ── 消费并推进游标。末尾哨兵 Eof 保证越界前必先 isAtEnd()，故不递增。──
// 游标推进示意：m_pos=2 → advance() 返回 tok[2]，m_pos=3
//     [int][main][(][)][{]...
//                ^ m_pos 移动前        ^ 移动后
const Token& Parser::advance() {
    const Token& tok = m_tokens[m_pos];
    if (!isAtEnd()) m_pos++;
    return tok;
}

// ── 前瞻判断（LL(1) 的核心动作）：只比较类型，不消费。──
bool Parser::check(TokenType t) const {
    return current().type == t;
}

// ── "尝试消费"：命中则吃掉并返回 true；未命中不动游标、返回 false。
//    用于文法中的可选项（如 `['else' stmt]`、`['=' expr]`）。──
bool Parser::match(TokenType t) {
    if (check(t)) {
        advance();
        return true;
    }
    return false;
}

// ── "强制消费"：语法上此处必须是 t，否则报错（panic：抛异常中止编译单元）。
//    与 match 的对比：match 容忍缺失（可选文法），expect 不容忍（必选文法）。──
const Token& Parser::expect(TokenType t, const std::string& msg) {
    if (check(t)) {
        return advance();
    }
    errorAt(current(), std::format("{}: expected {}, got '{}'",
        msg, tokenTypeName(t), current().text));
}

// ── 流末尾判断：越过数组边界 或 撞到 Eof 哨兵（Lexer 保证末尾有 Eof）。──
bool Parser::isAtEnd() const {
    return m_pos >= m_tokens.size() || current().is(TokenType::Eof);
}

// ─────────────────────────────────────────────────────────────────────────────
// 错误处理
// ─────────────────────────────────────────────────────────────────────────────
// ── 错误报告（当前 Token 处出错）──
[[noreturn]] void Parser::error(const std::string& msg) const {
    errorAt(current(), msg);
}

// ── 错误报告（指定 Token 处出错）。
//    错误恢复策略：panic 模式 —— 抛 std::runtime_error 直接中止本编译单元，
//    不做 Token 级跳过/同步恢复。调用方（main/驱动）捕获后打印信息并退出。
//    教学取舍：换取实现极简与错误信息确定性（每处语法错误恰好一条报告）。──
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
// 文法：type := ['const'] base-type ('*' | '&' | '&&')*
// 对应 clang：ParseDeclSpec 的 DeclSpec 部分（类型说明符 + 派生类型 declarator
//            的指针/引用后缀，clang 里拆在 ParseDeclarator 中，此处合并实现）。
//
// ─── parseType 支持的典型类型样例表 ──────────────────────────────────────────
// ┌──────────────────────────┬─────────────────────────────┬────────────────────────────────────────────────────────┐
// │ 输入类型样例 (C++ 代码)  │ 对应 Token 流序列           │ 生成的 AST 类型节点结构                                │
// ├──────────────────────────┼─────────────────────────────┼────────────────────────────────────────────────────────┤
// │ int                      │ [int]                       │ PrimitiveType(Int)                                     │
// │ double                   │ [double]                    │ PrimitiveType(Double)                                  │
// │ bool                     │ [bool]                      │ PrimitiveType(Bool)                                    │
// │ void                     │ [void]                      │ PrimitiveType(Void)                                    │
// │ auto                     │ [auto]                      │ AutoType                                               │
// │ MyClass                  │ [MyClass]                   │ ClassType("MyClass")                                   │
// │ T                        │ [T]                         │ ClassType("T") (模板形参占位类型)                      │
// │ std::string              │ [std][::][string]           │ ClassType("std::string") (作用域限定类型)              │
// │ int*                     │ [int][*]                    │ PointerType(int)                                       │
// │ int**                    │ [int][*][*]                 │ PointerType(PointerType(int)) (多级指针)               │
// │ int&                     │ [int][&]                    │ LValueReferenceType(int) (左值引用)                    │
// │ int&&                    │ [int][&&]                   │ RValueReferenceType(int) (右值引用)                    │
// │ T&&                      │ [T][&&]                     │ RValueReferenceType(T) (通用引用/右值引用占位)         │
// │ int*&                    │ [int][*][&]                 │ LValueReferenceType(PointerType(int)) (指针的引用)     │
// │ const int                │ [const][int]                │ ConstType(int)                                         │
// │ const MyClass&           │ [const][MyClass][&]         │ ConstType(LValueReferenceType(MyClass))                │
// │ const int*               │ [const][int][*]             │ ConstType(PointerType(int))                            │
// └──────────────────────────┴─────────────────────────────┴────────────────────────────────────────────────────────┘
// 注：const 按教学简化统一作用于整个类型（真 C++ 中 const T* 与 T* const
// 的 const 归属不同，这里不区分顶层/底层 const）。
TypePtr Parser::parseType() {
    // ── Step 1: 处理 const 前缀（例如 const int, const Vec&）──
    bool isConst = false;
    if (check(TokenType::KwConst)) {
        isConst = true;
        advance();
        std::cout << std::format("  [parse:type] ✦ const prefix detected\n");
    }

    // ── Step 2: 解析基础类型（基本类型 / 标识符 / 命名空间限定类型）──
    TypePtr base;

    if (match(TokenType::KwInt)) {
        base = Type::makeInt();          // 样例: int
        std::cout << std::format("  [parse:type] base = int\n");
    }
    else if (match(TokenType::KwDouble)) {
        base = Type::makeDouble();       // 样例: double
        std::cout << std::format("  [parse:type] base = double\n");
    }
    else if (match(TokenType::KwBool)) {
        base = Type::makeBool();         // 样例: bool
        std::cout << std::format("  [parse:type] base = bool\n");
    }
    else if (match(TokenType::KwVoid)) {
        base = Type::makeVoid();         // 样例: void
        std::cout << std::format("  [parse:type] base = void\n");
    }
    else if (match(TokenType::KwAuto)) {
        base = Type::makeAuto();         // 样例: auto
        std::cout << std::format("  [parse:type] base = auto\n");
    }
    else if (check(TokenType::Identifier)) {
        // 样例: "MyClass", 模板形参 "T", 命名空间限定名 "std::string" 或 "A::B::Type"
        std::string name = advance().text;
        while (check(TokenType::ColonColon)) {
            advance();
            name += "::" + expect(TokenType::Identifier, "Expected type name after '::'").text;
        }
        base = Type::makeClass(name);
        std::cout << std::format("  [parse:type] base = {} (class/tparam)\n", name);
    }
    else {
        error("Expected type name");
    }

    // ── Step 3: 处理后缀修饰符（*, &, &&），支持链式组合 ──
    // 样例: int* (单指针), int** (二级指针), int& (左值引用), T&& (右值引用), int*& (指针的引用)
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
    // 样例: const int, const MyClass&, const double*
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
// 文法：translation-unit := declaration*     （零个或多个顶层声明直到 Eof）
// 对应 clang：ParseTopLevelDecl（Parser.cpp）—— 同样是循环调用声明解析。
// 入参 demo：Token 流 [int][foo][(][)][{][...][}][class][C][{][...][}][;][Eof]
//   → 产出 TranslationUnit{ declarations = [FunctionDecl "foo", ClassDecl "C"] }
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
// 文法：declaration := template-decl | class-decl | ['virtual'] function-decl
// 对应 clang：ParseDeclaration / ParseDeclOrFunctionDefInternal（ParseDecl.cpp）
//
// 分派决策树（全部基于 1 个前瞻 Token，LL(1)）：
//   current() == 'template'  → parseTemplateDecl
//   current() == 'class'     → parseClassDecl
//   current() == 'virtual'   → 记 isVirtual=true，继续走函数声明
//   其余（类型关键字/标识符） → parseFunctionDecl
//
// TODO(minicc): 目前顶层声明只实现了 Class、Function、Template 三个最小核心子集。
// 真正的 C++ 在此还应支持 Namespace、Enum、全局变量（VarDecl）、Typedef/Using 等。
// 如果在此处解析到 `int a = 1;` 这种全局变量，当前会由于 fallback 到 `parseFunctionDecl`
// 并期待 `(` 而报错。这将在后续 Roadmap（数组/enum/namespace等特性）中扩展。
//
// 入参 demo：Token 流 [virtual][void][draw][(][)][{][...][}]
//   → isVirtual=true → parseFunctionDecl 产出 FunctionDecl("draw", void, virtual)
DeclPtr Parser::parseDeclaration() {
    // template<typename T> ...
    if (check(TokenType::KwTemplate)) { //wangyang 模板
        return parseTemplateDecl();
    }

    // class ... / struct ... //wangyang 触发方式为 class 或者struct
    if (check(TokenType::KwClass) || check(TokenType::KwStruct)) {
        return parseClassDecl();
    }

    // enum ...
    if (check(TokenType::KwEnum)) {
        return parseEnumDecl();
    }

    // namespace ...
    if (check(TokenType::KwNamespace)) {
        return parseNamespaceDecl();
    }

    // using ... / typedef ...
    if (check(TokenType::KwUsing) || check(TokenType::KwTypedef)) {
        return parseTypeAliasDecl();
    }

    // 函数声明或全局变量声明
    bool isVirtual = false;
    if (check(TokenType::KwVirtual)) {
        isVirtual = true;
    }

    // 区分函数声明与全局变量声明
    // 采用试探性前瞻（Tentative Lookahead）：存档 -> 试探跳过类型 -> 检查后续是否为 Identifier + '(' -> 回滚
    size_t savedPos = m_pos;
    if (isVirtual) {
        advance();
    }
    parseType(); // 试探性解析：仅用于前进 token 流以观察后续符号，返回值不保留
    bool isFunc = false;
    if (check(TokenType::Identifier)) {
        advance();
        if (check(TokenType::LParen)) {
            isFunc = true;
        }
    }
    m_pos = savedPos; // 回滚到初始位置，后续统一在分支函数内部正式 parseType 并构建 AST

    if (isFunc) {
        return parseFunctionDecl(isVirtual);
    } else {
        return parseGlobalVarDecl();
    }
}

// ─── 全局变量声明 ─────────────────────────────────────────────────────────────
GlobalVarDeclPtr Parser::parseGlobalVarDecl() {
    auto decl = std::make_shared<GlobalVarDecl>();
    decl->location = current().location;
    decl->declaredType = parseType();
    const Token& nameTok = expect(TokenType::Identifier, "Expected global variable name");
    decl->name = nameTok.text;
    if (match(TokenType::Assign)) {
        decl->initializer = parseExpression();
    }
    expect(TokenType::Semicolon, "Expected ';' after global variable declaration");
    return decl;
}

// ─── 枚举声明 ─────────────────────────────────────────────────────────────────
EnumDeclPtr Parser::parseEnumDecl() {
    auto decl = std::make_shared<EnumDecl>();
    decl->location = current().location;
    expect(TokenType::KwEnum, "Expected 'enum'");
    if (match(TokenType::KwClass) || match(TokenType::KwStruct)) {
        decl->isScoped = true;
    }
    if (check(TokenType::Identifier)) {
        decl->name = advance().text;
    }
    if (match(TokenType::Colon)) {
        decl->underlyingType = parseType();
    } else {
        decl->underlyingType = Type::makeInt();
    }
    expect(TokenType::LBrace, "Expected '{' in enum declaration");
    int64_t nextVal = 0;
    while (!check(TokenType::RBrace) && !isAtEnd()) {
        EnumItem item;
        item.location = current().location;
        const Token& nameTok = expect(TokenType::Identifier, "Expected enum item name");
        item.name = nameTok.text;
        if (match(TokenType::Assign)) {
            item.hasCustomValue = true;
            item.valueExpr = parseExpression();
            if (auto lit = std::dynamic_pointer_cast<IntLiteralExpr>(item.valueExpr)) {
                item.value = lit->value;
                nextVal = lit->value + 1;
            }
        } else {
            item.value = nextVal++;
        }
        decl->items.push_back(std::move(item));
        if (!match(TokenType::Comma)) {
            break;
        }
    }
    expect(TokenType::RBrace, "Expected '}' after enum items");
    expect(TokenType::Semicolon, "Expected ';' after enum declaration");
    return decl;
}

// ─── 命名空间声明 ─────────────────────────────────────────────────────────────
NamespaceDeclPtr Parser::parseNamespaceDecl() {
    auto decl = std::make_shared<NamespaceDecl>();
    decl->location = current().location;
    expect(TokenType::KwNamespace, "Expected 'namespace'");
    const Token& nameTok = expect(TokenType::Identifier, "Expected namespace name");
    decl->name = nameTok.text;
    expect(TokenType::LBrace, "Expected '{' in namespace declaration");
    while (!check(TokenType::RBrace) && !isAtEnd()) {
        decl->declarations.push_back(parseDeclaration());
    }
    expect(TokenType::RBrace, "Expected '}' after namespace declaration");
    return decl;
}

// ─── 类型别名声明 ─────────────────────────────────────────────────────────────
TypeAliasDeclPtr Parser::parseTypeAliasDecl() {
    auto decl = std::make_shared<TypeAliasDecl>();
    decl->location = current().location;
    if (match(TokenType::KwUsing)) {
        const Token& aliasTok = expect(TokenType::Identifier, "Expected alias name in using declaration");
        decl->aliasName = aliasTok.text;
        expect(TokenType::Assign, "Expected '=' in using declaration");
        decl->underlyingType = parseType();
        expect(TokenType::Semicolon, "Expected ';' after using declaration");
    } else if (match(TokenType::KwTypedef)) {
        decl->underlyingType = parseType();
        const Token& aliasTok = expect(TokenType::Identifier, "Expected alias name in typedef declaration");
        decl->aliasName = aliasTok.text;
        expect(TokenType::Semicolon, "Expected ';' after typedef declaration");
    }
    return decl;
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
// 文法：template-decl := 'template' '<' template-param (',' template-param)* '>'
//                        ( class-decl | function-decl )
//       template-param := ('typename' | 'class') IDENT
// 对应 clang：ParseTemplateDeclaration / ParseTemplateParameters（ParseTemplate.cpp）
//
// 模板体分派（解析顺序的关键决策，前瞻 template<...> 之后的第一个 Token）：
//   'class'                          → 类模板分支  parseClassDecl
//   类型关键字 / 标识符（如 T、int） → 函数模板分支 parseFunctionDecl
//                                      （返回类型可以是模板参数名 T）
//   其他                             → 报错
//
// 入参 demo：Token 流 [template][<][typename][T][>][class][Box][{][...][}][;]
//   → typeParams = ["T"]，classTemplate = ClassDecl("Box")
//   → 产出 TemplateDecl 蓝图（暂不做语义分析，阶段4 实例化时才克隆替换）
TemplateDeclPtr Parser::parseTemplateDecl() {
    auto decl = std::make_shared<TemplateDecl>();
    decl->location = current().location;

    std::cout << std::format("  [parse:template] ▶ template declaration at {}\n",
        current().location.toString());

    // 强制消费 'template' 与 '<'（必选文法，用 expect 保证结构） //wangyang expect 会往前推进
    expect(TokenType::KwTemplate, "Expected 'template'");
    expect(TokenType::Less, "Expected '<' after 'template'");

    std::cout << "  [parse:template]   parsing parameter list <";

    // 解析模板参数列表（支持类型参数 typename/class T 与非类型参数 int N 等 NTTP）
    do {
        TemplateParam param;
        param.location = current().location;

        /**
         *wangyang 验证参数
         */
        if (check(TokenType::KwTypename)) {
            advance();
            std::cout << "typename ";
            param.kind = TemplateParamKind::Type;
            const Token& paramName = expect(TokenType::Identifier, "Expected parameter name");
            param.name = paramName.text;
            decl->typeParams.push_back(param.name);
            decl->templateParams.push_back(param);
            std::cout << std::format("{}", param.name);
            std::cout << std::format("\n  [parse:template]   ★ type parameter registered: '{}'\n",
                param.name);
        }
        else if (check(TokenType::KwClass)) {
            advance();
            std::cout << "class ";
            param.kind = TemplateParamKind::Type;
            const Token& paramName = expect(TokenType::Identifier, "Expected parameter name");
            param.name = paramName.text;
            decl->typeParams.push_back(param.name);
            decl->templateParams.push_back(param);
            std::cout << std::format("{}", param.name);
            std::cout << std::format("\n  [parse:template]   ★ type parameter registered: '{}'\n",
                param.name);
        }
        else {
            // ★ 非类型模板参数 (NTTP)：如 int N, bool Flag 等 ★
            param.kind = TemplateParamKind::NonType;
            param.nonType = parseType();
            const Token& paramName = expect(TokenType::Identifier, "Expected parameter name after type in template parameter");
            param.name = paramName.text;
            decl->typeParams.push_back(param.name);
            decl->templateParams.push_back(param);
            std::cout << std::format("{} {}", param.nonType->toString(), param.name);
            std::cout << std::format("\n  [parse:template]   ★ non-type parameter registered: {} '{}'\n",
                param.nonType->toString(), param.name);
        }

    } while (match(TokenType::Comma) && (std::cout << ", ", true));

    std::cout << ">\n";

    expect(TokenType::Greater, "Expected '>' after template parameters");

    // 解析模板体：类模板 或 函数模板（S1+）
    // 分派依据：template<...> 之后的第一个 Token
    //   class                          → 类模板
    //   类型关键字 / 标识符（如 T、int） → 函数模板（返回类型可以是模板参数名 T）
    //   其他                           → 报错
    if (check(TokenType::KwClass)) {
        std::cout << std::format("  [parse:template]   parsing class body for template...\n");
        decl->classTemplate = parseClassDecl(); // wangyang** class tempalte
    }
    else if (current().isTypeKeyword() || check(TokenType::Identifier)) {
        std::cout << std::format("  [parse:template]   parsing function signature for template...\n");
        decl->funcTemplate = parseFunctionDecl(); // wangyang** function template
    }
    else {
        errorAt(current(),
            "Expected 'class' or a function signature after template parameter list");
    }

    std::cout << std::format("  [parse:template] ◀ {} template '{}' with {} parameter(s) stored as blueprint\n",
        decl->isClassTemplate() ? "class" : "function",
        decl->templateName(), decl->typeParams.size());

    // 打印蓝图摘要
    std::cout << std::format("  [parse:template]   blueprint summary:\n");
    std::cout << std::format("    template <");
    for (size_t i = 0; i < decl->typeParams.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << "typename " << decl->typeParams[i];
    }
    if (decl->isClassTemplate()) {
        std::cout << std::format("> class {} {{ ... }}\n", decl->templateName());
    } else {
        std::cout << std::format("> {} {}(",
            decl->funcTemplate->returnType ? decl->funcTemplate->returnType->toString() : "?",
            decl->templateName());
        for (size_t i = 0; i < decl->funcTemplate->parameters.size(); i++) {
            if (i > 0) std::cout << ", ";
            std::cout << decl->funcTemplate->parameters[i].type->toString() << " "
                      << decl->funcTemplate->parameters[i].name;
        }
        std::cout << ") { ... }\n";
    }

    return decl;
}

// ─────────────────────────────────────────────────────────────────────────────
// 类声明：class Name [: public Base] { ... };
// ─────────────────────────────────────────────────────────────────────────────
// 文法：class-decl := 'class' IDENT [':' 'public' IDENT] '{' member* '}' ';'
//       member    := access-spec | ['virtual'] ( method-decl | field-decl )
//       field-decl := type IDENT ';'
// 对应 clang：ParseCXXClassMemberDecl（ParseDecl.cpp）
//
// 入参 demo：Token 流 [class][Shape][:][public][Base][{][virtual][void][draw][(][)][;][}]
//   → 产出 ClassDecl{ name="Shape", baseClassName="Base",
//                     methods=[virtual FuncDecl "draw"] }
//
// 方法 vs 字段的判定（教学简化版的"most-vexing-parse 回避"）：
//   单靠 1 个前瞻 Token 无法区分 `int x;`（字段）与 `int x();`（方法），
//   故采用 保存游标 → 试探解析 `类型 名字` → 看下一个是否为 '(' → 恢复游标
//   的试探法（详见函数体内 savedPos 注释）。
// ─── 构造函数初始化列表 ───────────────────────────────────────────────────────
std::vector<CtorInitializer> Parser::parseCtorInitializerList() {
    std::vector<CtorInitializer> list;
    expect(TokenType::Colon, "Expected ':' to start constructor initializer list");
    do {
        CtorInitializer init;
        init.location = current().location;
        const Token& memberTok = expect(TokenType::Identifier, "Expected member or base name in initializer");
        init.memberName = memberTok.text;
        expect(TokenType::LParen, "Expected '(' after member name in initializer");
        if (!check(TokenType::RParen)) {
            do {
                init.arguments.push_back(parseExpression());
            } while (match(TokenType::Comma));
        }
        expect(TokenType::RParen, "Expected ')' in member initializer");
        list.push_back(std::move(init));
    } while (match(TokenType::Comma));
    return list;
}

// ─── 构造函数声明 ─────────────────────────────────────────────────────────────
CtorDeclPtr Parser::parseConstructorDecl(const std::string& ownerClass) {
    auto ctor = std::make_shared<ConstructorDecl>();
    ctor->location = current().location;
    ctor->ownerClassName = ownerClass;
    const Token& nameTok = expect(TokenType::Identifier, "Expected constructor name");
    ctor->name = nameTok.text;
    expect(TokenType::LParen, "Expected '(' in constructor parameter list");
    ctor->parameters = parseParameterList();
    expect(TokenType::RParen, "Expected ')' after constructor parameters");

    if (check(TokenType::Colon)) {
        ctor->initList = parseCtorInitializerList();
    }

    if (check(TokenType::LBrace)) {
        ctor->body = std::dynamic_pointer_cast<BlockStmt>(parseBlockStmt());
    } else {
        expect(TokenType::Semicolon, "Expected '{' or ';' after constructor declaration");
    }
    return ctor;
}

// ─── 析构函数声明 ─────────────────────────────────────────────────────────────
DtorDeclPtr Parser::parseDestructorDecl(const std::string& ownerClass, bool isVirtual) {
    auto dtor = std::make_shared<DestructorDecl>();
    dtor->location = current().location;
    dtor->ownerClassName = ownerClass;
    dtor->isVirtual = isVirtual;
    expect(TokenType::Tilde, "Expected '~' in destructor declaration");
    const Token& nameTok = expect(TokenType::Identifier, "Expected destructor name");
    dtor->name = "~" + nameTok.text;
    expect(TokenType::LParen, "Expected '(' in destructor parameter list");
    expect(TokenType::RParen, "Expected ')' in destructor parameter list");

    if (check(TokenType::LBrace)) {
        dtor->body = std::dynamic_pointer_cast<BlockStmt>(parseBlockStmt());
    } else {
        expect(TokenType::Semicolon, "Expected '{' or ';' after destructor declaration");
    }
    return dtor;
}

// ─────────────────────────────────────────────────────────────────────────────
// 类声明：class Name [: public Base] { ... };
// ─────────────────────────────────────────────────────────────────────────────
// 文法：class-decl := ('class' | 'struct') IDENT [':' 'public' IDENT] '{' member* '}' ';'
ClassDeclPtr Parser::parseClassDecl() {
    auto decl = std::make_shared<ClassDecl>();
    decl->location = current().location;

    bool isStruct = false;
    if (match(TokenType::KwStruct)) {
        isStruct = true;
        decl->currentAccess = AccessModifier::Public;
    } else {
        expect(TokenType::KwClass, "Expected 'class' or 'struct'");
        decl->currentAccess = AccessModifier::Private;
    }
    (void)isStruct;

    const Token& nameToken = expect(TokenType::Identifier, "Expected class name");
    decl->name = nameToken.text;

    // 可选的继承
    if (match(TokenType::Colon)) {
        match(TokenType::KwPublic); // 简化：只支持 public 继承
        const Token& baseName = expect(TokenType::Identifier, "Expected base class name");
        decl->baseClassName = baseName.text; //wangyang 这里是class decl 的基础类名称
    }

    expect(TokenType::LBrace, "Expected '{' after class name"); // wangyang 左括号

    // 解析类成员
    while (!check(TokenType::RBrace) && !isAtEnd()) {
        // 访问修饰符
        AccessModifier access = decl->currentAccess;
        if (check(TokenType::KwPublic)) { // 判断成员的访问级别是 public 还是priviate
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

        // 虚函数标记
        bool isVirtual = false;
        if (check(TokenType::KwVirtual)) {
            isVirtual = true;
            advance();
        }

        // 1. 析构函数：[virtual] ~ClassName()
        if (check(TokenType::Tilde)) {
            auto dtor = parseDestructorDecl(decl->name, isVirtual);
            decl->methods.push_back(dtor);
            continue;
        }

        // 2. 构造函数：ClassName(...) [: init-list]
        // 歧义前瞻说明：
        //   当行首标识符等于类名 (current().text == decl->name) 时，有两种可能：
        //   a) 构造函数：Node(...) -> 类名后紧跟 '('
        //   b) 以自身类作为类型的成员：如 Node* next; / Node& ref; / Node clone(); -> 类名后跟 '*', '&', 标识符等
        //   注：C++ 标准禁止成员变量与类名同名。因此若类名后跟 '(' 则判定为构造函数；
        //       否则回滚，进入步骤 3 将类名作为类型 (parseType) 解析字段或成员方法。
        if (!isVirtual && check(TokenType::Identifier) && current().text == decl->name) {
            size_t saved = m_pos;
            advance();
            if (check(TokenType::LParen)) { // 构造方法
                m_pos = saved;
                auto ctor = parseConstructorDecl(decl->name);
                decl->methods.push_back(ctor);
                continue;
            }
            m_pos = saved;
        }

        // 3. 尝试判断是普通方法还是字段
        // 采用试探性前瞻：存档 -> 试探跳过类型 -> 检查是否为 Identifier + '(' (方法) -> 回滚
        size_t savedPos = m_pos;
        bool isMethod = false;

        parseType(); // 试探性解析：仅用于前进 token 流以观察后续符号
        if (check(TokenType::Identifier)) {
            advance();
            if (check(TokenType::LParen)) {
                isMethod = true;
            }
        }
        m_pos = savedPos; // 回滚到初始位置，后续在分支内部正式解析类型并创建 AST 节点

        if (isMethod) {
            auto method = parseMethodDecl(decl->name, access);
            method->isVirtual = isVirtual;
            decl->methods.push_back(method);
        } else {
            // 字段声明
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
// 文法：method-decl := ['virtual'] function-decl
//       （函数声明本体复用 parseFunctionDecl，此处只负责剥掉可选的 'virtual'
//         前缀并把所属类名 ownerClass、虚函数标记缝进 AST 节点）
// 对应 clang：ParseCXXClassMemberDecl 中对成员函数说明符的处理。
// 入参 demo：类体内 Token 流 [virtual][int][area][(][)][{][...][}]
//   ownerClass="Shape" → 消费 virtual → parseFunctionDecl("Shape")
//   → 产出 FunctionDecl{ name="area", isVirtual=true, ownerClassName="Shape" }
FuncDeclPtr Parser::parseMethodDecl(const std::string& ownerClass,
                                     AccessModifier /*access*/) {
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
// 文法：function-decl := type IDENT '(' parameter-list ')' ['override']
//                        ( compound-stmt | ';' )
// 对应 clang：ParseDeclOrFunctionDefInternal（ParseDecl.cpp）
//   简化点：无存储类说明符、无尾置返回类型、无形参默认值、无函数重载区分
//   （同名函数由后续语义阶段处理）；函数体只支持复合语句块。
//
// 入参 demo：Token 流 [int][add][(][int][a][,][int][b][)][{][return][a][+][b][;][}]
//   → returnType=int, name="add", parameters=[(int,a),(int,b)]
//   → body=BlockStmt[ ReturnStmt( a + b ) ]
//   若函数体写成 ';'（仅前向声明），则 body 为空。
//
// 解析顺序（严格从左到右，单遍）：
//   [virtual] → 返回类型 parseType → 函数名 → '(' 参数列表 ')'
//   → [override] → '{' 函数体 ';' 二者择一
FuncDeclPtr Parser::parseFunctionDecl(bool isVirtual, const std::string& ownerClass) { // wangyang 这里在头文件上默认声明了 这两个变量
    auto decl = std::make_shared<FunctionDecl>();
    decl->location = current().location;
    decl->isVirtual = isVirtual;
    decl->ownerClassName = ownerClass;

    // 跳过 virtual（如果在 parseMethodDecl 中没有被消费）
    if (check(TokenType::KwVirtual)) {
        advance();
    }

    // 返回类型
    decl->returnType = parseType(); // wangyang 解析出返回类型

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

    //函数体或分号
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
// 文法：parameter-list := [ parameter (',' parameter)* ]
//       parameter     := type IDENT          （调用约定：调用处游标停在 '(' 之后）
// 对应 clang：ParseParameterDeclarationList（ParseDecl.cpp）
// 简化点：无默认实参、无省略号 varargs、无形参修饰符（如 const 形参由类型携带）。
// 入参 demo：Token 流 [int][a][,][double][b]（外层 '(' ')' 由调用方消费）
//   → 产出 [ (int, "a"), (double, "b") ]
//   Token 流 [)]（紧跟右括号）→ 直接返回空列表
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
// 文法：stmt := compound-stmt | if-stmt | while-stmt | return-stmt
//             | var-decl-stmt | expr-or-assign-stmt
// 对应 clang：ParseStatement（Parser.cpp），同样是按首 Token 分派。
//
// 分派表（前瞻决策）：
//   '{'                        → 复合语句 parseBlockStmt
//   'if' / 'while' / 'return'  → 对应关键字语句
//   类型关键字(int/auto/…)      → 变量声明（类型先行解析，再交给 parseVarDeclStmt）
//   标识符 + 前瞻是标识符/'*'   → 类名型变量声明（试探法，见下）
//   其余                        → 表达式/赋值语句
//
// 入参 demo：Token 流 [int][x][=][42][;]
//   → 类型关键字 int → parseType 得 int → parseVarDeclStmt
//   → 产出 VarDeclStmt{ name="x", type=int, init=IntLiteral(42) }
StmtPtr Parser::parseStatement() {
    if (check(TokenType::LBrace))    return parseBlockStmt();
    if (check(TokenType::KwIf))      return parseIfStmt();
    if (check(TokenType::KwWhile))   return parseWhileStmt();
    if (check(TokenType::KwReturn))  return parseReturnStmt();
    if (check(TokenType::KwDelete))  return parseDeleteStmt();

    // 变量声明：以类型关键字开头
    if (current().isTypeKeyword()) {
        TypePtr type = parseType();
        return parseVarDeclStmt(type);
    }

    // 类名开头的变量声明（需要向前看）
    // 歧义：标识符开头既可能是 `MyClass obj;`（声明），也可能是
    //       `foo(...);` / `a = 3;`（表达式）。LL(1) 单 Token 无法区分，
    //       采用试探法：存档 → 跳过头个标识符和连续 '*' → 若下一个还是
    //       标识符则判定为声明 → 回滚后重新正式解析；否则回滚走表达式分支。
    if (check(TokenType::Identifier)) {
        // 向前看：如果是 标识符 标识符 ; 或 标识符 标识符 = → 变量声明
        size_t savedPos = m_pos;
        std::string firstName = advance().text;

        // 检查是否是 类名 * → 指针类型
        while (match(TokenType::Star)) {} // 跳过指针标记

        if (check(TokenType::Identifier)) {
            // 是变量声明：回滚到存档点，让 parseType 从头正式解析类型
            m_pos = savedPos; // wangyang 说明 这里是MyObj value 这样的类型
            TypePtr type = parseType();
            return parseVarDeclStmt(type);
        }

        // 不是变量声明，恢复位置，当作表达式语句
        m_pos = savedPos;
    }

    return parseExprOrAssignStmt(); // wangyang 可能是表达式，比如 foo() 这样的函数语句
}

// ─── 代码块 ──────────────────────────────────────────────────────────────────
// 文法：compound-stmt := '{' stmt* '}'
// 对应 clang：ParseCompoundStatement（Parser.cpp）
// 入参 demo：Token 流 [{][int][x][;][x][=][1][;][}]
//   → 产出 BlockStmt{ statements = [VarDeclStmt x, AssignStmt x=1] }
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
// 文法：var-decl-stmt := IDENT ['=' expr] ';'
//   注：类型由调用方（parseStatement）预先 parseType 后作为入参传入，
//       这样"试探判定是不是声明"与"正式解析类型"可以解耦。
//   简化点：不支持括号初始化 `T x(...)`、列表初始化 `T x{...}`、
//           一条语句声明多个变量 —— 正是这些简化回避了 most-vexing-parse。
// 对应 clang：ParseSimpleDeclaration（ParseDecl.cpp）
// 入参 demo：入参 type=int；Token 流 [x][=][42][;]
//   → 产出 VarDeclStmt{ name="x", type=int, init=IntLiteral(42) }
StmtPtr Parser::parseVarDeclStmt(TypePtr type) {
    auto loc = current().location;
    const Token& nameToken = expect(TokenType::Identifier, "Expected variable name");

    ExprPtr init = nullptr;
    if (match(TokenType::Assign)) {
        init = parseExpression(); // wangyang 这里就是语句
    }

    expect(TokenType::Semicolon, "Expected ';' after variable declaration");

    auto stmt = std::make_shared<VarDeclStmt>(nameToken.text, type, init);
    stmt->location = loc;
    return stmt;
}

// ─── if 语句 ──────────────────────────────────────────────────────────────────
// 文法：if-stmt := 'if' '(' expr ')' stmt ['else' stmt]
//   'else' 是可选分支（match 容忍缺失）；then/else 均为单条 stmt，
//   多条语句需写 { }（悬垂 else 按 C 惯例就近绑定，递归下降自然满足）。
// 对应 clang：ParseIfStatement（ParseStmt.cpp）
// 入参 demo：Token 流 [if][(][x][<][10][)][{][...][}][else][return][0][;]
//   → 产出 IfStmt{ cond=(x<10), then=BlockStmt, else=ReturnStmt(0) }
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
// 文法：while-stmt := 'while' '(' expr ')' stmt
// 对应 clang：ParseWhileStatement（ParseStmt.cpp）
// 入参 demo：Token 流 [while][(][i][<][n][)][i][=][i][+][1][;]
//   → 产出 WhileStmt{ cond=(i<n), body=AssignStmt(i = i+1) }
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
// 文法：return-stmt := 'return' [expr] ';'
//   返回值可选：前瞻到 ';' 即为裸 return（void 函数）。
// 对应 clang：ParseReturnStatement（ParseStmt.cpp）
// 入参 demo：Token 流 [return][a][+][b][;] → ReturnStmt{ value = BinaryExpr(a+b) }
//           Token 流 [return][;]           → ReturnStmt{ value = nullptr }
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

// ─── delete 语句 ──────────────────────────────────────────────────────────────
StmtPtr Parser::parseDeleteStmt() {
    auto loc = current().location;
    expect(TokenType::KwDelete, "Expected 'delete'");
    bool isArray = false;
    if (match(TokenType::LBracket)) {
        expect(TokenType::RBracket, "Expected ']' after '[' in delete[]");
        isArray = true;
    }
    ExprPtr expr = parseExpression();
    expect(TokenType::Semicolon, "Expected ';' after delete statement");
    auto stmt = std::make_shared<DeleteStmt>(expr, isArray);
    stmt->location = loc;
    return stmt;
}

// ─── 表达式语句或赋值语句 ─────────────────────────────────────────────────────
// 文法：expr-or-assign := expr ['=' expr] ';'
//   决策：先把左侧完整解析为 expr，再前瞻是否跟 '='：
//         跟了 → AssignStmt（赋值）；没跟 → ExprStmt（纯表达式语句）。
//   简化点：不支持复合赋值 +=/-= 等，也不支持链式赋值 a=b=c。
// 对应 clang：ParseExpressionStatement（ParseStmt.cpp）
// 入参 demo：Token 流 [x][=][42][;]  → AssignStmt{ lhs=Var(x), rhs=IntLiteral(42) }
//           Token 流 [foo][(][)][;] → ExprStmt{ expr=CallExpr(foo, []) }
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
// 方法：优先级分层递归下降（precedence climbing 的文法编码形式）。
//   运算符优先级用文法嵌套表达：优先级越低的运算符出现在越外层的产生式，
//   因此越晚绑定。每层统一模式：
//       left := 下一层();                       // 先取更高优先级的操作数
//       while (当前 Token 是本层运算符) {       // 左结合：循环向左折叠
//           消费运算符; right := 下一层();
//           left := BinaryExpr(op, left, right);
//       }
//
//   优先级链（低 → 高）：
//     || → && → ==/!= → <,>,<=,>= → +,- → *,/,% → 一元 -,! → 后缀 call/./-> → primary
//
//   demo：`1 + 2 * 3` 的解析形状
//     parseAdditiveExpr: left = parseMultiplicativeExpr()   → Int(1)
//       见到 '+', right = parseMultiplicativeExpr()         → Mul(2,3)  ← * 先绑定
//       → Add(1, Mul(2,3)) ✓ 乘法成为加法的右孩子，优先级正确
//
// 对应 clang：ParseExpression / ParseRHSOfBinaryExpression（ParseExpr.cpp）
//   clang 用一张运算符优先级表迭代处理，本实现用函数嵌套链，原理相同。

ExprPtr Parser::parseExpression() {
    return parseOrExpr();
}

// ─── || ──────────────────────────────────────────────────────────────────────
// 文法：or-expr := and-expr ('||' and-expr)*      最低优先级，左结合
// 入参 demo：Token 流 [a][||][b][&&][c]
//   left=Var(a)；见 '||'，right = parseAndExpr → And(b,c)
//   → 产出 Or(a, And(b,c))   ← && 比 || 绑定更紧，成为其右孩子
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
// 文法：and-expr := equality ('&&' equality)*     左结合
// 入参 demo：Token 流 [x][&&][y] → And(Var(x), Var(y))
// 注：此处只做结构解析；短路求值是语义/代码生成阶段的事。
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
// 文法：equality := comparison (('==' | '!=') comparison)*   左结合
// 入参 demo：Token 流 [a][==][b] → BinaryExpr(Eq, Var(a), Var(b))
// 决策：前瞻 Token 在 EqualEqual / BangEqual 二者中映射到 Eq / Neq。
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
// 文法：comparison := additive (('<'|'>'|'<='|'>=') additive)*   左结合
// 入参 demo：Token 流 [i][<][n] → BinaryExpr(Lt, Var(i), Var(n))
// 注意：比较运算符 '<' 与模板实参列表的 '<' 是同一个 Token —— 模板 id 的
//       歧义消解发生在 parsePrimaryExpr（标识符后试探 '<' 类型列表 '('），
//       本层拿到的 '<' 一律按比较运算符处理。
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
// 文法：additive := multiplicative (('+' | '-') multiplicative)*   左结合
// 入参 demo：Token 流 [a][-][b][-][c]
//   循环向左折叠：先 Sub(a,b)，再 Sub(Sub(a,b), c) → 左结合 ✓
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
// 文法：multiplicative := unary (('*' | '/' | '%') unary)*   左结合
// 入参 demo：Token 流 [2][*][3][%][4] → Mod(Mul(2,3), 4)
// 注意：'*' 在类型上下文（parseType）里是指针后缀，在表达式上下文里是乘法——
//       同一 Token 的含义由调用它的文法位置决定（上下文相关）。
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
// 文法：unary := ('-' | '!') unary | postfix
//   递归调用自身 → 支持一元运算符任意叠加（如 -!x、--x 在此文法下均合法，
//   语义合法性由阶段3检查）。
// 对应 clang：ParseCastExpression 中对一元前缀运算符的处理（ParseExpr.cpp）
// 入参 demo：Token 流 [-][x] → UnaryExpr(Neg, Var(x))
//           Token 流 [!][flag] → UnaryExpr(Not, Var(flag))
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
// 文法：postfix := primary ( call-args | ('.' | '->') IDENT )*
//   while 循环 + 每次把结果包成新节点 → 天然左结合，支持任意长后缀链。
// 对应 clang：ParsePostfixExpressionSuffix（ParseExpr.cpp）
// 入参 demo：Token 流 [p][->][next][(][)]
//   primary=Var(p) → 见 '->' 包成 MemberExpr(p.next, isArrow)
//   → 见 '(' 包成 CallExpr(MemberExpr(p.next), [])
//   → 产出 Call( Member(p, next), [] )   ← 后缀从左到右逐层外包
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
// 文法：primary := INT | BOOL | STRING | 'nullptr' | 'this'
//                | 'new' IDENT ['(' [expr (',' expr)*] ')']
//                | IDENT ['<' type (',' type)* '>']      ← template-id（S3 显式模板实参）
//                | '(' expr ')'
// 优先级链的最内层（结合力最强），是递归下降的"叶子"层。
// 对应 clang：ParsePrimaryExpression / ParsePrimaryExpressionOrUnaryExpression
//            （ParseExpr.cpp、ParseExprCXX.cpp）
//
// 分派表（按前瞻 Token 逐个尝试，全部 LL(1)）：
//   IntLiteral/StringLiteral/true/false/nullptr/this → 对应字面量节点
//   标识符 "new"（词法上未单列关键字，按文本识别）   → NewExpr
//   其他标识符                                        → VarExpr（+ 可选 template-id 试探）
//   '('                                              → 括号表达式，递归回 parseExpression
//
// 入参 demo：Token 流 [new][Node][(][1][,][2][)]
//   → 产出 NewExpr{ className="Node", constructorArgs=[Int(1), Int(2)] }
//           Token 流 [(][a][+][b][)] → 递归 parseExpression → Add(a,b)
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
    if (check(TokenType::KwNew) || (check(TokenType::Identifier) && current().text == "new")) {
        advance(); // 消费 new
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

    // 变量引用（或 template-id：foo<int>(...) —— S3 显式模板实参）
    if (check(TokenType::Identifier)) {
        std::string name = advance().text;
        while (check(TokenType::ColonColon)) {
            advance();
            name += "::" + expect(TokenType::Identifier, "Expected identifier after '::'").text;
        }
        auto expr = std::make_shared<VarExpr>(name);
        expr->location = loc;

        // 显式模板实参：name '<' 类型列表 '>'，且其后必须紧跟 '('
        // 歧义消解（clang ParseImplicitTemplateId 的简化版）：
        //   a < b > c 是比较表达式；foo<int>(x) 是 template-id。
        //   判据：试探解析逗号分隔的类型列表，成功匹配 '>' 且下一个是 '(' 才算 template-id，
        //   否则回滚，把 '<' 交还给比较表达式解析。
        //
        // 试探法三步曲（与类体内方法/字段判定同一模式）：
        //   ① saved = m_pos 存档；② try 块内空跑"类型列表 + '>' + '(' 前瞻"，
        //   parseType 中途报错说明 '<' 后不是类型（如 a<b），异常即失败信号；
        //   ③ 失败则 m_pos = saved 回滚，外层 while 会把 '<' 当比较运算符继续。
        if (check(TokenType::Less)) {
            size_t saved = m_pos;
            advance(); // '<'
            std::vector<TypePtr> explicitArgs;
            bool isTemplateId = true;
            try {
                if (!check(TokenType::Greater)) {
                    do {
                        explicitArgs.push_back(parseType());
                    } while (match(TokenType::Comma));
                }
                if (!match(TokenType::Greater) || !check(TokenType::LParen)) {
                    isTemplateId = false;
                }
            } catch (const std::exception&) {
                isTemplateId = false;
            }
            if (isTemplateId) {
                expr->explicitTemplateArgs = std::move(explicitArgs);
                std::cout << std::format("  [parse] template-id: {}<{} explicit arg(s)>\n",
                    name, expr->explicitTemplateArgs.size());
            } else {
                m_pos = saved; // 回滚：这是 'a < b' 比较
            }
        }

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
// 文法：call-args := '(' [ expr (',' expr)* ] ')'
//   与 parseParameterList（声明侧：类型+名字）不同，调用侧是纯表达式列表。
// 对应 clang：ParseExpressionList（ParseExpr.cpp）
// 入参 demo：Token 流 [(][x][,][f][(][)][)]
//   → 产出 [ Var(x), Call(f, []) ]；Token 流 [(][)] → 空列表
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
// 纯查表映射（TokenType → BinaryOp），无副作用。各层优先级函数目前内联了
// 自己的 switch，此表作为集中式对照备用；default 分支兜底返回 Add 仅为
// 满足 return 语义（调用方保证只传入运算符 Token）。
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

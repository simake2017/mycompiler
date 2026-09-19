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

// ── 模板形参作用域查询：裸名是否命中当前 template<...> 的类型形参？──
// 供 parseType 区分 TemplateParam("T") 与 Class("T")。
// 线性扫描即可：形参个数极少（个位数），无需 hash（教学取舍：可讲解 > 高性能）。
// 对应 clang: Sema 的 TemplateParameterScope 查找（ActOnIdentifier 前判定依赖名）。
bool Parser::isInTemplateParamScope(const std::string& name) const {
    for (const auto& p : m_templateParamScope) {
        if (p == name) return true;
    }
    return false;
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
// │ const MyClass&           │ [const][MyClass][&]         │ LValueReferenceType(ConstType(MyClass))                │
// │ const int*               │ [const][int][*]             │ PointerType(ConstType(int))                            │
// │ int* const               │ [int][*][const]             │ ConstType(PointerType(int))                            │
// └──────────────────────────┴─────────────────────────────┴────────────────────────────────────────────────────────┘
// ★ cv 限定符的归属（[dcl.type.cv] + [dcl.decl]）
//   const 出现在【类型说明符】一侧（`const int` 的 const）时修饰的是【基类型】，
//   后面每遇到一个 */& 都被它包在外层：
//       const int*   →  Pointer(Const(Int))     "指向 const int 的指针"
//   const 出现在【声明符】一侧（`int* const` 的 const）时修饰的是【已建好的类型】：
//       int* const   →  Const(Pointer(Int))     "const 的指针"
//   两者不可互换 —— 建错了树，下游的偏特化匹配就会【静默选错】，
//   详见 tests/tmpl/test_tmpl_47_cv_position.cpp 与 docs/learn/22 的 ⑨。
//   对照 clang：Parser 把 const 收进 DeclSpec（类型说明符），
//   声明符算子（* & const）由 ParseDeclarator 在 GetTypeForDeclarator 里
//   由内向外套 —— 与本文件的 Step 3 / Step 4 分工一致。
TypePtr Parser::parseType() {
    // ── Step 1: 处理 const 前缀（例如 const int, const Vec&）──
    bool isConst = false;
    if (check(TokenType::KwConst)) {
        isConst = true;
        advance();
        std::cout << std::format("  [parse:type] ✦ const prefix detected\n");
    }

    // ── Step 1.5: 处理 typename 前缀（依赖限定名消歧，[temp.res]/5）──
    // 源码形态：typename T::type / typename Box<T>::value_type
    // 【它到底在说什么】"跟在后面的这个名字是个【类型】，不是值" ——
    //   编译器在模板里看到一个限定名 `T::x` 时无法只凭语法判断 x 是
    //   类型还是静态成员，故标准要求写 typename 消歧（C++20 起在
    //   非依赖上下文里可省）。本实现把它当纯粹的语法前缀收下：
    //   类型/值之分后面由语义阶段查别名表得出结论。
    // 对照 clang：Parser::TryAnnotateTypeOrScopeToken 识别 typename 后
    //   走 ParseTypenameType；被消歧的那个名字建成 DependentNameType。
    if (check(TokenType::KwTypename)) {
        advance();
        std::cout << "  [parse:type] ✦ typename prefix (依赖限定名消歧)\n";
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
    else if (check(TokenType::KwDecltype)) {
        // ── decltype(expr)：延迟求值的类型查询 ──
        // 语法：decltype ( expression )
        // ★ 记录"多套一层括号"：这是 [dcl.type.decltype] 两套规则的开关——
        //     decltype(e)   其中 e 是【未加括号】的 id-expression / 成员访问
        //                   → 取 e 的【声明类型】
        //     decltype((e)) 加了括号 → 取【表达式类型】（左值带 &）
        //   例（int a;）：
        //     decltype(a)    → int        （声明类型）
        //     decltype((a))  → int&       （a 是左值，表达式类型是左值引用）
        //   对照 clang：Sema::ActOnDecltypeExpression + BuildDecltypeType。
        auto kwLoc = advance().location;   // 消费 'decltype'
        expect(TokenType::LParen, "Expected '(' after 'decltype'");

        // 外层 '(' 之后若紧跟 '('，说明原文是 decltype((...))，
        // 即操作数自身被括号包住 —— 走"表达式类型"那套规则。
        bool paren = check(TokenType::LParen);

        ExprPtr operand = parseExpression();
        expect(TokenType::RParen, "Expected ')' to close 'decltype'");

        base = Type::makeDecltype(operand, paren);
        base->name = "decltype";
        std::cout << std::format(
            "  [parse:type] base = decltype({}) [deferred: 不求值，留待替换阶段]\n",
            paren ? "(e)" : "e");
        (void)kwLoc;
    }
    else if (check(TokenType::Identifier)) {
        // 样例: "MyClass", 模板形参 "T", 命名空间限定名 "std::string" 或 "A::B::Type"
        std::string name = advance().text;

        // ★ 依赖限定名 typename T::type（[temp.res]/5）──
        // 【判据】首个标识符命中当前模板形参作用域【且】后面跟 '::'：
        //   此时限定者是"依赖的"（T 要等实例化才知道），整条名字是
        //   依赖类型名，不能用下面"拼成一个字符串"的老路 ——
        //   拼成 "T::type" 后语义阶段既查不到符号、也无从替换。
        // 【为什么要 typename 也走这里】C++20 起在非依赖上下文里 typename
        //   可有可无；本实现两种写法都收，语义相同（都是"去限定者里取成员类型"）。
        // 对照 clang：Parser::ParseTypenameType → Sema::ActOnTypenameType
        //   → 建 DependentNameType（限定者是依赖的）/ TypenameType。
        if (isInTemplateParamScope(name) && check(TokenType::ColonColon)) {
            TypePtr qual = Type::makeTemplateParam(name);
            std::cout << std::format(
                "  [parse:type] ★ 依赖限定名: {}::...（限定者是模板形参）\n", name);
            while (check(TokenType::ColonColon)) {
                advance();
                const Token& memberTok = expect(TokenType::Identifier,
                    "Expected member type name after '::'");
                TypePtr nested = Type::makeClass(memberTok.text);
                nested->nestedQualifier = qual;
                qual = nested;
                std::cout << std::format("  [parse:type] ★ nested name: {}\n",
                    qual->toString());
            }
            base = qual;
            // 到此为止：不 return，落到下面的 Step 2.5（const）与 Step 3
            // （后缀循环），于是 `const typename T::type&` / `typename T::type*`
            // 这些组合也自动成立。
        }
        else {
            while (check(TokenType::ColonColon)) {
                advance();
                name += "::" + expect(TokenType::Identifier, "Expected type name after '::'").text;
            }
            // 查询模板形参作用域 —— 裸标识符若命中当前 template<...>
            // 声明的类型形参，建成 TemplateParam 节点而非 Class 节点。
            // 对应 clang: Sema::isIdentiferADependentTemplateName / ActOnType
            // 在模板上下文中把 T 解析为 TemplateTypeParmType。
            // 注意：只认不含 '::' 的裸名（std::T 这种限定名不可能是模板形参）。
            if (name.find("::") == std::string::npos && isInTemplateParamScope(name)) {
                base = Type::makeTemplateParam(name);
                std::cout << std::format("  [parse:type] base = {} (template param, scope hit)\n", name);
            }
            else {
                base = Type::makeClass(name);
                std::cout << std::format("  [parse:type] base = {} (class/tparam)\n", name);
                // 模板 id（P3）——标识符后紧跟 '<' 即模板实参表：
                //   Box<int>、Buf<4>、Map<int, double>，实参类型/值分流见
                //   parseTemplateArgumentList（类型递归 parseType，支持嵌套
                //   List<Box<int>>；嵌套的 '>>' 由词法保证是两个 Greater token）。
                // 对照 clang：ParseTemplateName + ParseTemplateArgumentList，
                // 产出 TemplateSpecializationType；此处直接把实参挂在 Class 节点
                // 的 templateArgs 上，语义阶段再按需实例化（[temp.inst]）。
                if (check(TokenType::Less)) {
                    base->templateArgs = parseTemplateArgumentList();
                    std::cout << std::format(
                        "  [parse:type] ★ template-id: {}\n", base->toString());
                }

                // ── 嵌套类型名：S<int>::type ──
                // 纯标识符链（A::B::C，含 std::true_type）已在上面拼成一个名字；
                // 这里处理的是【模板 id 之后】或【成员名再来一层】的 '::' ——
                // 语义是"去限定者所指的那个类里取成员类型别名"。
                // 它是半成品：限定者 S<int> 可能还没实例化（[temp.inst]），
                // 故不在此处解析，留一个 nestedQualifier 指针给 resolveType。
                // 对照 clang：ParseNestedNameSpecifier + DependentNameType。
                while (check(TokenType::ColonColon)) {
                    advance();
                    const Token& memberTok = expect(TokenType::Identifier,
                        "Expected member type name after '::'");
                    TypePtr nested = Type::makeClass(memberTok.text);
                    nested->nestedQualifier = base;
                    base = nested;
                    std::cout << std::format("  [parse:type] ★ nested name: {}\n",
                        base->toString());
                }
            }
        }
    }
    else {
        error("Expected type name");
    }

    // ── Step 2.5: 应用【类型说明符侧】的 const（必须在后缀循环之前）──
    // ★ 位置就是语义：这里的 const 修饰的是【基类型】，之后每套一层 */& 都在它外面。
    //     const int*  ⇒ Pointer(Const(Int))      指向 const int 的指针
    //     const int&  ⇒ LValueRef(Const(Int))    指向 const int 的引用
    //   此前这步放在后缀循环【之后】，于是两式都被建成
    //     Const(Pointer(Int)) / Const(LValueRef(Int))
    //   —— 顶层节点不是 Pointer / 引用，下游按结构匹配的偏特化全部失手：
    //     C<T*> 收不到 const int*，C<const T&> 更是永不匹配（见 test_tmpl_47）。
    //   对照 clang：DeclSpec 里的 const 在 GetTypeForDeclarator 中先于
    //   声明符算子生效，方向与此一致。
    if (isConst) {
        base = Type::makeConst(base);
        std::cout << std::format("  [parse:type] const applied to base → {}\n", base->toString());
        isConst = false;   // 已消费，Step 4 不再重复套
    }

    // ── Step 3: 处理后缀修饰符（*, &, &&, const），支持链式组合 ──
    // 样例: int* (单指针), int** (二级指针), int& (左值引用), T&& (右值引用),
    //       int*& (指针的引用), int* const (const 指针)
    // ★ 这里出现的 const 属于【声明符侧】（[dcl.decl]）—— 它修饰的是
    //   "到目前为止已建好的那个类型"，所以套在【最外层】而非基类型上：
    //       int* const   ⇒ Const(Pointer(Int))    const 的指针
    //       const int* const ⇒ Const(Pointer(Const(Int)))   两者兼有
    //   与 Step 2.5 的 const 恰好相反，这正是"位置即语义"的两个端点。
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
        else if (check(TokenType::KwConst)) {
            // 声明符侧 const：int* const ⇒ Const(Pointer(Int))
            // 只在后缀循环里收到（类型说明符侧的那个已在 Step 2.5 消费掉）。
            advance();
            base = Type::makeConst(base);
            std::cout << std::format(
                "  [parse:type] suffix const → {} (const-qualified declarator)\n",
                base->toString());
        }
        else {
            break; // 没有更多后缀修饰符
        }
    }

    std::cout << std::format("  [parse:type] ★ final type = {}\n", base->toString());
    return base;
}

// ─────────────────────────────────────────────────────────────────────────────
// 模板实参表解析（[temp.arg]）—— 类型实参与非类型实参（NTTP）的分流点
// ─────────────────────────────────────────────────────────────────────────────
// 【做什么】吃掉 '<' ... '>' 整段实参表，逐个判定每个实参是"类型"还是"值"，
//           分别打包成 TemplateArg{Type} / TemplateArg{Integral}。
//
// 【理论】[temp.arg] 规定模板实参有四类形态，本项目只实现前两类：
//   · 类型实参     Box<int>  → 实参是 type-id
//   · 非类型实参   Buf<4>    → 实参是 constant-expression（NTTP 的值）
//   · 模板模板实参 template<class> class TT  → 未实现
//   · 包展开 ...                             → 未实现
//
// 【判定依据】只看 1 个 Token（LL(1)）：
//   IntLiteral           → 非类型实参 → TemplateArg{Integral, std::stoll(text)}
//   其他（类型关键字/标识符）→ 类型实参   → TemplateArg{Type, parseType()}
//
// 【与 clang 的差距】clang 在 Sema::ActOnNonTypeTemplateArgument
//   （SemaTemplate.cpp）里对 constant-expression 做完整解析 + 常量求值 +
//   形参类型匹配（如把 4 转成形参声明的 unsigned 等）。本项目：
//   · 只认整数字面量，不做常量折叠（Buf<2+2> 不支持，见 ROADMAP 主线 D）
//   · 类型匹配推迟到 SemanticAnalyzer 实例化前做（见 checkTemplateArguments）
//
// 【demo】Buf<4>        → [ TemplateArg{kind=Integral, value=4} ]
//         Box<int>      → [ TemplateArg{kind=Type,     type=int} ]
//         Pair<int, 8>  → [ TemplateArg{Type,int}, TemplateArg{Integral,8} ]
std::vector<TemplateArg> Parser::parseTemplateArgumentList() {
    expect(TokenType::Less, "Expected '<' before template arguments");

    std::vector<TemplateArg> args;
    if (!check(TokenType::Greater)) {
        do {
            // ── 分流点：整数（含负号）→ 非类型实参；否则 → 类型实参 ──
            // demo：Buf<4> 的 4 → TemplateArg{Integral,4}；Box<int> 的 int → {Type,int}
            // 注意：parseType 失败会抛异常，由调用方的 try/catch 决定报错还是回滚
            // （见 parsePrimaryExpr 的 template-id 歧义消解三歩曲）。
            //
            // 先判负数：'-' 紧跟整数字面量才算，靠 1 个 Token 的前瞻区分
            // 「Buf<-3>」（负实参）与「Buf<a-b>」（表达式，本函数不支持）。
            if (check(TokenType::Minus) && m_pos + 1 < m_tokens.size()
                && m_tokens[m_pos + 1].is(TokenType::IntLiteral)) {
                // ★ 严格说 -3 不是字面量，而是「一元减 + 字面量」构成的常量表达式
                //   （[expr.unary.op] + [expr.const]）。完整实现要解析任意常量表达式
                //   再求值（Buf<2+2> 也应支持）—— 那属于常量折叠，见 ROADMAP 主线 D。
                //   此处只识别最常见的「负号 + 整数字面量」，覆盖 Buf<-3>。
                //   对应 Itanium 编码 _Z3BufILin3EE（n 表负数，见 NameMangler）。
                advance();  // '-'
                int64_t value = -std::stoll(advance().text);
                std::cout << std::format(
                    "  [parse:targ] ★ non-type argument (NTTP): negative integer {}\n", value);
                args.push_back(TemplateArg::ofValue(value));
            }
            else if (check(TokenType::IntLiteral)) {
                const Token& tok = advance();
                int64_t value = std::stoll(tok.text);
                std::cout << std::format(
                    "  [parse:targ] ★ non-type argument (NTTP): integer literal {}\n", value);
                args.push_back(TemplateArg::ofValue(value));
            }
            else {
                args.push_back(TemplateArg::ofType(parseType()));
            }
        } while (match(TokenType::Comma));
    }

    expect(TokenType::Greater, "Expected '>' after template arguments");

    std::cout << std::format("  [parse:targ] ★ template argument list: <{} argument(s)> — ",
        args.size());
    for (size_t i = 0; i < args.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << (args[i].isValue() ? "(value) " : "(type) ") << args[i].toString();
    }
    std::cout << "\n";
    return args;
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
    if (check(TokenType::KwTemplate)) {
        return parseTemplateDecl();
    }

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

    // ── 非模板推导指引：`Box(int) -> Box<int>;`（[temp.deduct.guide]）──
    // 必须排在"函数 vs 全局变量"的前瞻【之前】：两者都从 parseType 开始试探，
    // 而 `Box(int)` 会让 parseType 只吃掉 `Box`，随后 check(Identifier) 失败
    // （下一个是 '('）⇒ 被误判成全局变量声明 ⇒ 报 "Expected global variable name"。
    if (looksLikeDeductionGuide()) {
        return parseDeductionGuide();
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
            // 枚举项只支持整型字面量初始化式（其余形态留给 CodeGen 报错）
            if (item.valueExpr->kind == NodeKind::IntLiteral) {
                auto lit = std::static_pointer_cast<IntLiteralExpr>(item.valueExpr);
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

// ─── 推导指引（deduction guide，[temp.deduct.guide]）─────────────────────────
// 前瞻：Identifier '(' <深度配对> ')' '->'
// 【为什么不能只看一个 Token】`MyPtr(T) -> MyPtr<T>;` 与普通函数声明
//   `MyPtr f(T);` 的前两个 Token 都是 Identifier + '('，分岔点在【配对右括号
//   之后】：跟 '->' 才是指引。故必须先把括号配对走完。
// 对照 clang：Parser::TryParseDeductionGuide 也是"解析完形参表后回头看 '->'"，
//   只是 clang 真的先把声明解析出来再决定归属，本实现用纯扫描更省事。
bool Parser::looksLikeDeductionGuide() const {
    if (!check(TokenType::Identifier)) return false;
    size_t i = m_pos + 1;
    if (i >= m_tokens.size() || !m_tokens[i].is(TokenType::LParen)) return false;
    int depth = 0;
    for (; i < m_tokens.size(); i++) {
        if (m_tokens[i].is(TokenType::LParen)) depth++;
        else if (m_tokens[i].is(TokenType::RParen)) {
            depth--;
            if (depth == 0) break;
        }
        else if (m_tokens[i].is(TokenType::Semicolon) ||
                 m_tokens[i].is(TokenType::LBrace)) return false;  // 跑出声明范围了
    }
    if (i + 1 >= m_tokens.size()) return false;
    return m_tokens[i + 1].is(TokenType::Arrow);
}

DeductionGuideDeclPtr Parser::parseDeductionGuide() {
    auto decl = std::make_shared<DeductionGuideDecl>();
    decl->location = current().location;

    const Token& nameTok = expect(TokenType::Identifier, "Expected class template name in deduction guide");
    decl->guideName = nameTok.text;

    expect(TokenType::LParen, "Expected '(' in deduction guide");
    if (!check(TokenType::RParen)) {
        do {
            Parameter p;
            p.type = parseType();
            // 形参名可选：`MyPtr(T)` 与 `MyPtr(T value)` 都合法
            // （指引形参名对推导毫无影响，clang 也允许省略）
            if (check(TokenType::Identifier)) p.name = advance().text;
            decl->parameters.push_back(std::move(p));
        } while (match(TokenType::Comma));
    }
    expect(TokenType::RParen, "Expected ')' in deduction guide");

    expect(TokenType::Arrow, "Expected '->' in deduction guide");

    // `->` 右侧：目标类型 id（Name<args>）。用 parseType 解析后取名字与实参。
    TypePtr target = parseType();
    if (!target->isClass() || target->templateArgs.empty()) {
        errorAt(current(), std::format(
            "deduction guide for '{}' must map to a specialization of a class "
            "template (e.g. {}<...>)", decl->guideName, decl->guideName));
    }
    for (const auto& arg : target->templateArgs) {
        if (!arg.isType()) {
            errorAt(current(), "deduction guide target arguments must be types");
        }
        decl->targetArgs.push_back(arg.type);
    }

    expect(TokenType::Semicolon, "Expected ';' after deduction guide");

    std::cout << std::format("  [parse:guide] ★ deduction guide: {}(", decl->guideName);
    for (size_t i = 0; i < decl->parameters.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << decl->parameters[i].type->toString();
    }
    std::cout << ") -> " << target->toString() << "\n";
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

    // 强制消费 'template' 与 '<'（必选文法，用 expect 保证结构）
    expect(TokenType::KwTemplate, "Expected 'template'");
    expect(TokenType::Less, "Expected '<' after 'template'");

    std::cout << "  [parse:template]   parsing parameter list <";

    // 解析模板参数列表（支持类型参数 typename/class T 与非类型参数 int N 等 NTTP）
    // ★ 空形参表 template<> 是合法的 —— 它是【全特化】的标记（[temp.expl.spec]）：
    //   template<> struct Box<int*, int> { ... };
    //   故此处先判 '>' 再进循环（不能用 do-while 无条件吃一个形参）。
    if (!check(TokenType::Greater)) {
        int unnamedSeq = 0;   // 无名形参的合成名序号（见下方 unnamed 分支）
        do {
            TemplateParam param;
            param.location = current().location;

            if (check(TokenType::KwTypename) || check(TokenType::KwClass)) {
                // ── 类型形参：typename T / class T ──
                // ★ 形参名可以【省略】：`template <typename T, typename = void>`
                //   是 SFINAE 探测的标配写法（第二个形参只用来承接 void_t 的推导，
                //   名字根本用不上，标准允许无名，见 [temp.param]/3）。
                //   判据：后一个 token 是 Identifier 才读名字；否则是 '=' 或 '>' 或 ','
                //   —— 那说明这是个无名形参。
                //   无名时给个合成名（"$1" 形式），内部照常有键可用，
                //   只是用户代码引用不到它 —— 与"没有名字"的语义一致。
                param.kind = TemplateParamKind::Type;
                bool isClassKw = check(TokenType::KwClass);
                advance();
                std::cout << (isClassKw ? "class " : "typename ");

                if (check(TokenType::Identifier)) {
                    param.name = advance().text;
                    std::cout << std::format("{}", param.name);
                    std::cout << std::format(
                        "\n  [parse:template]   ★ type parameter registered: '{}'\n",
                        param.name);
                } else {
                    // 无名形参：合成一个用户写不出来的名字（带 '$' 前缀）。
                    // 用不可写字符是刻意的——保证不会与任何真实标识符撞车。
                    param.name = "$unnamed" + std::to_string(unnamedSeq++);
                    param.isUnnamed = true;
                    std::cout << std::format(
                        "(unnamed) ⇒ 内部合成名 '{}'\n  [parse:template]   "
                        "★ type parameter registered: '{}' (nameless, [temp.param]/3)\n",
                        param.name, param.name);
                }
            }
            else {
                // ★ 非类型模板参数 (NTTP)：如 int N, bool Flag 等 ★
                param.kind = TemplateParamKind::NonType;
                param.nonType = parseType();
                const Token& paramName = expect(TokenType::Identifier, "Expected parameter name after type in template parameter");
                param.name = paramName.text;
                std::cout << std::format("{} {}", param.nonType->toString(), param.name);
                std::cout << std::format("\n  [parse:template]   ★ non-type parameter registered: {} '{}'\n",
                    param.nonType->toString(), param.name);
            }

            // ── 默认模板实参（[temp.param]/12）：形参名后可选 '= 默认值' ──
            // demo：template<typename T, typename U = void>
            //         → U 的 defaultArg = TemplateArg{Type, void}, hasDefault = true
            // 分派与实例化实参解析同构：整数字面量 → 值实参；否则 → 类型实参。
            if (match(TokenType::Assign)) {
                if (check(TokenType::IntLiteral)) {
                    param.defaultArg = TemplateArg::ofValue(std::stoll(advance().text));
                }
                else if (check(TokenType::Minus)
                         && m_pos + 1 < m_tokens.size()
                         && m_tokens[m_pos + 1].is(TokenType::IntLiteral)) {
                    advance();
                    param.defaultArg = TemplateArg::ofValue(-std::stoll(advance().text));
                }
                else {
                    param.defaultArg = TemplateArg::ofType(parseType());
                }
                param.hasDefault = true;
                std::cout << std::format(
                    "  [parse:template]   ★ default template argument: '{}' = {}\n",
                    param.name, param.defaultArg.toString());
            }

            decl->typeParams.push_back(param.name);
            decl->templateParams.push_back(param);

        } while (match(TokenType::Comma) && (std::cout << ", ", true));
    }
    else {
        std::cout << "  [parse:template] ★ empty parameter list — explicit (full) specialization\n";
    }

    std::cout << ">\n";

    expect(TokenType::Greater, "Expected '>' after template parameters");

    // ── 默认实参约束（[temp.param]/12）──
    // 一旦某位形参带了默认值，其后每一位都必须带；否则出现"空洞"，
    // 调用者无法用位置实参填满。对照 clang: err_template_param_default_arg_missing
    for (size_t i = 0; i < decl->templateParams.size(); i++) {
        if (decl->templateParams[i].hasDefault) {
            for (size_t j = i + 1; j < decl->templateParams.size(); j++) {
                if (!decl->templateParams[j].hasDefault) {
                    errorAt(current(), std::format(
                        "template parameter '{}' must have a default argument "
                        "because '{}' (declared before it) has one",
                        decl->templateParams[j].name, decl->templateParams[i].name));
                }
            }
            break;
        }
    }

    // 把【类型形参名】压入模板形参作用域，使模板体解析期间
    // parseType 能把裸 T 识别为 TemplateParam 节点。
    // 对应 clang: Parser 进入模板声明时压入 TemplateParameterDepth
    // （Sema::TemplateParameterScope），模板体解析完（此处为函数返回前）弹出。
    // 只压 Type 形参：NTTP 名字（如 int N 的 N）不是类型名，不能当类型用。
    size_t scopeBase = m_templateParamScope.size();
    for (const auto& param : decl->templateParams) {
        if (param.kind == TemplateParamKind::Type) {
            m_templateParamScope.push_back(param.name);
            std::cout << std::format("  [parse:template]   ↗ push tparam '{}' into scope\n",
                param.name);
        }
    }

    // 解析模板体：类模板 或 函数模板（S1+）
    // 分派依据：template<...> 之后的第一个 Token
    //   class / struct                 → 类模板（struct 与 class 只差默认访问级别）
    //   类型关键字 / 标识符（如 T、int） → 函数模板（返回类型可以是模板参数名 T）
    //   其他                           → 报错
    if (check(TokenType::KwClass) || check(TokenType::KwStruct)) {
        std::cout << std::format("  [parse:template]   parsing class body for template...\n");
        // 类名后可能跟模板 id（Box<T*, T> / Box<int*, int>）—— 那是特化，
        // 由 parseClassDecl 把尖括号里的模式回填进 specPattern（[temp.class.spec]）。
        decl->classTemplate = parseClassDecl(&decl->specPattern);

        // ── 判定主模板 / 偏特化 / 全特化（[temp.class.spec] / [temp.expl.spec]）──
        // 依据两条：形参表是否为空 + 模式里有没有模板参数
        //   template<>        struct Box<int*, int>  → 形参空   → 全特化
        //   template<class T> struct Box<T*, T>      → 形参非空 → 偏特化
        //   struct Box { ... }                       → 无尖括号 → 主模板
        //
        // ★ 这两个条件【不可互相替代】，它们量的是两个独立维度：
        //     specPattern 空    ⇔ 类名后【没写】<...>
        //     templateParams 空 ⇔ 写了 `template<>`
        //   主模板恰好落在"specPattern 空、templateParams 非空"这一格 ——
        //   所以外层必须先按 specPattern 摘出主模板，内层才能按 templateParams
        //   区分全/偏特化。
        //
        // ★ 先堵一个漏：两个都空 = 写了 `template<>` 却没写 `<...>`。
        //   那不是任何合法形态（全特化必须点明模板实参），若不拦下会被
        //   静默当成主模板 —— 用户以为在特化，实际在重定义主模板，
        //   而且没有任何报错。对照 clang：err_extraneous_template_spec。
        if (decl->templateParams.empty() && decl->specPattern.empty()) {
            error(std::format(
                "extraneous 'template<>' in declaration of class '{}' —— "
                "写了 `template<>` 却没有在类名后写 `<...>`：全特化必须点明模板实参"
                "（如 `template<> struct {}<int, int>`）；若本意是定义主模板，"
                "请去掉 `template<>`",
                decl->templateName(), decl->templateName()));
        }

        //    ┌──────────────────────────────────────────┬─────────────┬────────────────┬───────────────┐
        // │                   写法                   │ specPattern │ templateParams │     判定      │
        // ├──────────────────────────────────────────┼─────────────┼────────────────┼───────────────┤
        // │ template<class T> struct Box {...}       │ 空          │ [T] 非空       │ 主模板 ← 反例 │
        // ├──────────────────────────────────────────┼─────────────┼────────────────┼───────────────┤
        // │ template<class T> struct Box<T*,T> {...} │ [T*,T]      │ [T]            │ 偏特化        │
        // ├──────────────────────────────────────────┼─────────────┼────────────────┼───────────────┤
        // │ template<> struct Box<int*,int> {...}    │ [int*,int]  │ 空             │ 全特化        │
        // └──────────────────────────────────────────┴─────────────┴────────────────┴───────────────┘
        if (!decl->specPattern.empty()) {
            decl->specKind = decl->templateParams.empty()
                                 ? TemplateSpecKind::ExplicitSpec
                                 : TemplateSpecKind::PartialSpec;
            std::cout << std::format(
                "  [parse:template] ★ {} specialization of '{}' with pattern <{}>\n",
                decl->isExplicitSpec() ? "EXPLICIT (full)" : "PARTIAL",
                decl->templateName(),
                [&] { std::string s;
                      for (size_t i = 0; i < decl->specPattern.size(); i++) {
                          if (i > 0) s += ", ";
                          s += decl->specPattern[i] ? decl->specPattern[i]->toString() : "?";
                      } return s; }());
        }
    }
    else if (check(TokenType::Identifier) && looksLikeDeductionGuide()) {
        // ── 模板推导指引：template<class T> MyPtr(T) -> MyPtr<T>; ──
        // 分派依据是"配对右括号后跟 '->'"，与函数模板 `T f(T)` 区分开。
        decl->guide = parseDeductionGuide();
        decl->guide->templateParams = decl->templateParams;   // 形参表由外层 template<> 提供
    }
    else if (check(TokenType::KwUsing)) {
        // ── 别名模板（[temp.alias]）──
        //   template<class T> using Vec = MyPtr<T>;
        // 形态上它比类/函数模板都"轻"：右手边直接就是一个类型 id，
        // 没有函数体、没有成员表，故复用命名空间层的 parseTypeAliasDecl
        //（它同时认 `using X = T;` 与 `typedef T X;` 两种写法）。
        // 注：`template<...> typedef ...` 在 C++ 里【非法】（typedef 没有
        //   模板形式），这里若真出现 KwTypedef，会落到下面的 else 报错。
        std::cout << std::format("  [parse:template]   parsing alias template...\n");
        decl->aliasTemplate = parseTypeAliasDecl();
    }
    else if (current().isTypeKeyword() || check(TokenType::Identifier)) {
        std::cout << std::format("  [parse:template]   parsing function signature for template...\n");
        decl->funcTemplate = parseFunctionDecl();
    }
    else {
        errorAt(current(),
            "Expected 'class' or a function signature after template parameter list");
    }

    std::cout << std::format("  [parse:template] ◀ {} template '{}' with {} parameter(s) stored as blueprint\n",
        decl->isClassTemplate()      ? "class"
        : decl->isAliasTemplate()    ? "alias"
        : decl->isDeductionGuide()   ? "deduction-guide"
                                     : "function",
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
    } else if (decl->isAliasTemplate()) {
        std::cout << std::format("> using {} = {}\n", decl->templateName(),
            decl->aliasTemplate->underlyingType
                ? decl->aliasTemplate->underlyingType->toString() : "?");
    } else if (decl->isDeductionGuide()) {
        // 指引没有类体/函数体可言，摘要只报形参表
        std::cout << std::format("> {}(", decl->templateName());
        for (size_t i = 0; i < decl->guide->parameters.size(); i++) {
            if (i > 0) std::cout << ", ";
            std::cout << decl->guide->parameters[i].type->toString();
        }
        std::cout << ") -> " << decl->templateName() << "<...>  (deduction guide)\n";
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

    // 模板体解析完毕，弹出本层形参作用域（与上方 push 配对）。
    // 教学取舍：不实现嵌套模板（template<template> 套娃）的逐层作用域栈深度，
    // 但用 size 恢复而非 clear，天然支持未来嵌套。
    m_templateParamScope.resize(scopeBase); // 这里就是弹出刚才push 进去的元素
    std::cout << "  [parse:template]   ↘ tparam scope popped\n";

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
        // parseBlockStmt 恒返回 BlockStmt，直接下行转换（与 :1421 的函数体一致）
        ctor->body = std::static_pointer_cast<BlockStmt>(parseBlockStmt());
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
        dtor->body = std::static_pointer_cast<BlockStmt>(parseBlockStmt());
    } else {
        expect(TokenType::Semicolon, "Expected '{' or ';' after destructor declaration");
    }
    return dtor;
}

// ─────────────────────────────────────────────────────────────────────────────
// 类声明：class Name [: public Base [, public Base2 ...]] { ... };
// ─────────────────────────────────────────────────────────────────────────────
// 文法：class-decl := ('class' | 'struct') IDENT
//                     [':' 'public' IDENT (',' 'public' IDENT)*]
//                     '{' member* '}' ';'
// 多继承（[class.mi]）：逗号分隔的基类列表，每个基类必须带 public 说明符；
// 声明顺序即子对象摆放顺序（主基类优化：第一个多态基类的虚表与派生类合并）。
ClassDeclPtr Parser::parseClassDecl(std::vector<TypePtr>* outSpecPattern) {
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

    // ── 类名后的模板 id（特化专用，[temp.class.spec]）──
    // 主模板写 `class Box {`，特化写 `class Box<T*, T> {`。
    // 尖括号里的那串模式回填给调用方（parseTemplateDecl 存进 specPattern）。
    // 只允许类型实参作模式：NTTP 模式的偏特化（Box<int, N>）本项目未实现。
    if (outSpecPattern && check(TokenType::Less)) {
        auto raw = parseTemplateArgumentList();
        outSpecPattern->reserve(raw.size());
        for (auto& a : raw) {
            if (!a.isType()) {
                error(std::format(
                    "non-type argument '{}' is not supported in a class template "
                    "specialization pattern (only type patterns are implemented)",
                    a.toString()));
            }
            outSpecPattern->push_back(a.type);
        }
        std::cout << std::format("  [parse:class] ★ specialization pattern for '{}'\n",
            decl->name);
    }

    // 可选的继承列表：`:` 后逗号分隔，每个基类前必须显式写 public。
    // 真实语法允许省略说明符（struct 默认 public），本项目为教学明确性强制要求。
    if (match(TokenType::Colon)) {
        do {
            if (!match(TokenType::KwPublic)) {
                error("仅支持 public 继承（每个基类前需写 'public'）");
            }
            // 基类名支持命名空间限定：: public std::false_type
            // （std 垫片 is_range 的基类正是 std::true_type / std::false_type，
            //  与 parseType 的限定名处理同构）
            std::string baseName = expect(TokenType::Identifier,
                                          "Expected base class name").text;
            while (check(TokenType::ColonColon)) {
                advance();
                baseName += "::" + expect(TokenType::Identifier,
                    "Expected base class name after '::'").text;
            }
            decl->baseClassNames.push_back(baseName);
        } while (match(TokenType::Comma));
    }

    expect(TokenType::LBrace, "Expected '{' after class name");

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

        // ── 成员类型别名（[dcl.typedef]）──
        //   using type = T;     （C++11 别名声明）
        //   typedef T type;     （C 风格写法，语义完全等价）
        // 两者产出同一条 ClassDecl::typeAliases 条目。
        // 【理论】类型别名是纯编译期设施：不产生新类型、不占对象内存、
        //   不进链接符号（链接器根本不认识它），只在语义阶段做一次名字替换。
        //   它正是 type_traits 全家桶的出口 —— 每个元函数都靠 `using type = ...`
        //   交回结果，没有它写不出任何 trait（见 tests/tmpl/test_tmpl_48）。
        // 【依赖情形】目标可以是模板形参（`using type = T;`），
        //   实例化时由 TemplateInstantiator 做结构化替换，与字段/方法同一条路。
        // 对照 clang：ParseTypedefDecl / ParseAliasDeclaration，
        //   产物都是 TypedefNameDecl，仅存储形态不同。
        if (check(TokenType::KwUsing)) {
            advance();
            const Token& aliasTok = expect(TokenType::Identifier,
                                           "Expected alias name after 'using'");
            expect(TokenType::Assign, "Expected '=' in alias declaration");
            TypePtr target = parseType();
            expect(TokenType::Semicolon, "Expected ';' after alias declaration");
            decl->typeAliases[aliasTok.text] = target;
            decl->typeAliasOrder.push_back(aliasTok.text);
            std::cout << std::format("  [parse:alias] using {} = {}\n",
                aliasTok.text, target->toString());
            continue;
        }
        if (check(TokenType::KwTypedef)) {
            advance();
            TypePtr target = parseType();
            const Token& aliasTok = expect(TokenType::Identifier,
                                           "Expected alias name in typedef");
            expect(TokenType::Semicolon, "Expected ';' after typedef");
            decl->typeAliases[aliasTok.text] = target;
            decl->typeAliasOrder.push_back(aliasTok.text);
            std::cout << std::format("  [parse:alias] typedef {} {}\n",
                target->toString(), aliasTok.text);
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
    if (check(TokenType::KwTypename) || check(TokenType::Identifier)) {
        // 向前看：如果是 标识符 标识符 ; 或 标识符 标识符 = → 变量声明
        size_t savedPos = m_pos;
        // `typename T::type v;`：typename 只是消歧前缀，跳过它再看名字
        if (check(TokenType::KwTypename)) advance();
        advance();  // 跳过首个标识符（类型名本身）—— 纯前瞻，只要推进游标

        // P3 模板 id 前缀 —— `Box<int> b;` / `Map<int, double> m;`
        // 标识符后紧跟 '<' → 按深度配对跳过整段实参表。实参只可能是类型
        // （不含比较表达式），所以 '<'/'>' 直接计深度即可；嵌套
        // `Box<Box<int>>` 的 '>>' 词法阶段就是两个 Greater token。
        // 对照 clang：isDeclarationSpecifier → TryAnnotateTypeToken 会把
        // 模板 id 注解为类型 Token，教学版用"跳过 + 回滚"的试探法等价实现。
        if (check(TokenType::Less)) {
            int depth = 1;
            advance();  // 消费 '<'
            while (!isAtEnd() && depth > 0) {
                if (check(TokenType::Less)) depth++;
                else if (check(TokenType::Greater)) depth--;
                advance();
            }
        }

        // ★ 限定名后缀：`Box<int>::type v;` / `Plain::Int y;`
        // 类型名后面跟 `::` 说明用的是【成员类型】而不是同名对象。
        // 对照 clang：isDeclarationSpecifier 里 TryAnnotateTypeToken 会把
        //   `Cls<...>::member` 整体注解成一个类型 Token（ParseCXXScopeSpecifier
        //   之后的 nested-name-specifier 处理），教学版同样用"跳过后回滚"近似。
        while (check(TokenType::ColonColon)) {
            advance();  // 消费 '::'
            if (!check(TokenType::Identifier)) break;
            advance();  // 成员名
        }

        // 检查是否是 类名 * / 类名 & → 指针或引用类型
        // ★ 这里必须把 '*' 和 '&' 一起跳过 —— 它们是【声明符】的一部分，
        //   不是类型名的一部分。此前只跳 '*'，于是
        //       S& r = a;          （普通类）
        //       Box<int>& r = b;   （模板 id）
        //   都会因为前瞻停在 '&' 上、发现下一个不是 Identifier 而
        //   被误判成表达式语句 → `Expected ';' after expression`。
        //   内建类型（`int& r = a;`）不走这条前瞻，所以一直是好的 ——
        //   这个洞只在【标识符开头的类型】上暴露，容易被漏掉。
        // 对照 clang：ParseDeclarator 的指针/引用算子循环，两者同处一层。
        while (check(TokenType::Star) || check(TokenType::Ampersand) ||
               check(TokenType::AmpAmp)) {
            advance();
        }

        if (check(TokenType::Identifier)) {
            // 是变量声明：回滚到存档点，让 parseType 从头正式解析类型
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
    std::vector<ExprPtr> ctorArgs;
    if (match(TokenType::Assign)) {
        init = parseExpression(); //wangyang**** 这里就是变量 statement的初始声明表达式
    }
    else if (check(TokenType::LParen)) {
        // ── 直接初始化 `Type name(args...);`（[dcl.init]/16）──
        // 本实现此前只认 `Type name;` 与 `Type name = expr;`，
        // 于是 `MyPtr m(7);` 直接报 "Expected ';' after variable declaration"。
        // 而 CTAD（[dcl.type.class.deduct]）恰恰**只在直接初始化时触发** ——
        // 推导的输入就是这串实参，所以这半步是 CTAD 的前置条件。
        // 对照 clang：ParseDeclaration → ParseDeclarator 的 '(' 分支
        //   （ParseFunctionDeclarator / ParseParenDeclarator）。
        advance();  // 消费 '('
        if (!check(TokenType::RParen)) {
            do {
                ctorArgs.push_back(parseExpression());
            } while (match(TokenType::Comma));
        }
        expect(TokenType::RParen, "Expected ')' after constructor arguments");
    }

    expect(TokenType::Semicolon, "Expected ';' after variable declaration");

    auto stmt = std::make_shared<VarDeclStmt>(nameToken.text, type, init);
    stmt->ctorArgs = std::move(ctorArgs);
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
// 表达式语句 类似 x = 42  foo() 这种都算
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
    if (check(TokenType::Minus) || check(TokenType::Bang)
        || check(TokenType::Ampersand)) {
        auto loc = current().location;
        // 一元位置上的 '&' 是【取地址】而非引用/位与：
        //   引用只出现在类型里（V& r），位与是二元运算符（a & b），
        //   走到 parseUnaryExpr 说明 '&' 前面没有左操作数，故必然是取地址。
        //   对照 clang：ParseCastExpression 的 tok::ampersand 分支。
        // demo：&a → UnaryExpr{ op=Addr, operand=VarExpr{a} }
        UnaryOp op = (current().type == TokenType::Minus)   ? UnaryOp::Neg
                   : (current().type == TokenType::Bang)    ? UnaryOp::Not
                                                            : UnaryOp::Addr;
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
        else if (check(TokenType::LBracket)) {
            // 下标访问 v[i] —— operator[] 的语法糖
            // 文法：postfix '[' expression ']'
            // 对应真实编译器：clang 的 ParsePostfixExpressionSuffix 处理
            // '[' 时产出 ArraySubscriptExpr（内建数组）或
            // CXXOperatorCallExpr（类类型 → operator[] 重载）。
            // minicc 无运算符重载 → 产出 IndexExpr，语义阶段降级为
            // at()/set() 成员调用（见 include/ast.h IndexExpr 注释）。
            auto loc = current().location;
            advance();  // consume '['
            ExprPtr indexExpr = parseExpression();
            expect(TokenType::RBracket, "Expected ']' after subscript");
            auto idxExpr = std::make_shared<IndexExpr>(expr, indexExpr);
            idxExpr->location = loc;
            expr = idxExpr;
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

    // dynamic_cast<T*>(expr) 表达式（[expr.dynamic.cast]）
    // 文法：'dynamic_cast' '<' 类名 '*' '>' '(' expr ')'
    // 简化点：只支持"类名*"目标类型（真 C++ 还允许引用形式与静态偏移转型）。
    if (check(TokenType::KwDynamicCast)) {
        advance(); // 消费 dynamic_cast
        expect(TokenType::Less, "Expected '<' after 'dynamic_cast'");
        const Token& clsTok = expect(TokenType::Identifier,
            "Expected class name in dynamic_cast<...>");
        expect(TokenType::Star, "dynamic_cast target must be a pointer type (T*)");
        expect(TokenType::Greater, "Expected '>' after dynamic_cast<...>");
        expect(TokenType::LParen, "Expected '(' after dynamic_cast<T*>");
        ExprPtr operand = parseExpression();
        expect(TokenType::RParen, "Expected ')' to close dynamic_cast");
        auto expr = std::make_shared<DynamicCastExpr>(clsTok.text, std::move(operand));
        expr->location = loc;
        std::cout << std::format("  [parse] dynamic_cast<{}*>(operand)\n", clsTok.text);
        return expr;
    }

    // new 表达式
    if (check(TokenType::KwNew) || (check(TokenType::Identifier) && current().text == "new")) {
        advance(); // 消费 new
        const Token& className = expect(TokenType::Identifier, "Expected class name after 'new'");
        auto expr = std::make_shared<NewExpr>(className.text);
        expr->location = loc;

        // P3 —— 可选模板实参表：new Box<int>() / new Buf<4>()。
        // 与 parseType 的模板 id 分支同构（复用 parseTemplateArgumentList），
        // 实参暂挂在节点上，语义阶段实例化后改写 className 为实例名。
        if (check(TokenType::Less)) {
            expr->templateArgs = parseTemplateArgumentList();
            std::cout << std::format(
                "  [parse:new] ★ template-id: new {}<{} argument(s)>\n",
                expr->className, expr->templateArgs.size());
        }

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
        // 判据（两种合法后续，对应两条不同的语义路径）：
        //   ① 紧跟 '('  → 函数模板调用        foo<int>(x)
        //   ② 紧跟 ':' ':' → 类型限定静态成员 Cls<int>::value
        //      （后者是 std 垫片 is_range<T>::value 依赖的写法，
        //        对照 clang：ParseCXXScopeSpecifier 的 template-id 分支）
        if (check(TokenType::Less)) {
            size_t saved = m_pos;
            std::vector<TemplateArg> explicitArgs;
            bool isTemplateId = true;
            try {
                // parseTemplateArgumentList 自消费 '<' 与 '>'；
                // 其中 parseType 对非类型实参抛异常（如 a<b 里的 b 不是类型），
                // 异常即"这不是 template-id"的信号。
                explicitArgs = parseTemplateArgumentList();
                if (!check(TokenType::LParen) && !check(TokenType::ColonColon)) {
                    isTemplateId = false;
                }
            } catch (const std::exception&) {
                isTemplateId = false;
            }
            if (isTemplateId) {
                expr->explicitTemplateArgs = std::move(explicitArgs);
                std::cout << std::format("  [parse] template-id: {}<{} explicit arg(s)>\n",
                    name, expr->explicitTemplateArgs.size());

                // ── 类型限定访问：Cls<Args>::member ──
                // 把模板 id 当作【类型】而非对象，产出一个带 isTypeAccess 的 MemberExpr。
                // 语义阶段据此走"静态常量查找 + 折叠为字面量"，而不是按偏移量取字段。
                if (check(TokenType::ColonColon)) {
                    advance();  // 消费 '::'
                    const Token& memTok = expect(TokenType::Identifier,
                        "Expected member name after '::'");
                    auto me = std::make_shared<MemberExpr>(expr, memTok.text, /*arrow=*/false);
                    me->isTypeAccess = true; // 标明这是一个类型访问，不是一个对象取值
                    me->location = loc;
                    std::cout << std::format(
                        "  [parse] 类型限定访问: <template-id>::{} (static member)\n",
                        memTok.text);
                    return me;
                }
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

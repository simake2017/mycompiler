#pragma once
// =============================================================================
// 阶段 2：语法分析器 (Parser)（理论见 docs/learn/09、30）
// =============================================================================
// 职责：把线性的 Token 流转成树状 AST。手法：递归下降（Recursive Descent）——
// 文法中每个非终结符对应一个解析函数，互相调用形成递归；若文法为 LL(1) 则每步只需
// 1 个前瞻 Token，遇歧义用"存游标 + 回溯"（speculative parsing）消解。
//
// 管线：预处理器 → Lexer → ★Parser★ → Sema → 模板推导/实例化 → CodeGen(.s)
//   输入 std::vector<Token>（Lexer 产物，含末尾哨兵 Eof）
//   输出 TranslationUnit（AST 根节点，内含声明列表）
//
// ── 本编译器接受的语法（EBNF 概览）──────────────────────────────────────
//   translation-unit := declaration*
//   declaration      := template-decl | class-decl | ['virtual'] function-decl
//   template-decl    := 'template' '<' template-params '>' ( class-decl | function-decl )
//   class-decl       := 'class' IDENT [':' 'public' IDENT] '{' member* '}' ';'
//   function-decl    := type IDENT '(' params ')' ['override'] ( compound-stmt | ';' )
//   stmt             := block | if | while | return | var-decl | expr-stmt
//   expr             := or-expr（运算符优先级链：|| > && > == > < > + > * > 一元 > 后缀）
//
// ⚠ most-vexing-parse 简化：真 C++ 里 `T x(Foo());` 是函数声明（[stmt.dcl]）。本实现
//   从源头回避该歧义 —— 语句级变量声明只认【类型关键字】或【标识符 + 标识符/(*)】
//   开头（不支持括号初始化 `T x(...)`），类体内同样用"标识符后是否跟 '(' "区分方法与字段。
//
// ── clang 对照（参照源码 llvm-project/clang/lib/Parse/）─────────────────
//   Parser.cpp                    → parseTranslationUnit / 语句分派 / Token 流操作
//   ParseDecl.cpp                 → ParseDeclOrFunctionDefInternal（声明与类成员解析）
//   ParseTemplate.cpp             → ParseTemplateDeclaration（模板声明解析）
//   ParseExpr.cpp / ParseExprCXX  → 表达式优先级链与 template-id 歧义消解
// =============================================================================

#include "token.h"
#include "ast.h"
#include <vector>
#include <stdexcept>

namespace minicc {

// =============================================================================
// Parser 类：单遍扫描 Token 流，产出 AST。
// 状态极简：只有 Token 数组 + 一个游标 m_pos —— 这就是递归下降解析器的
// 全部运行时状态（符号表、类型检查等属于后续语义分析阶段）。
// =============================================================================
class Parser {
public:
    // 按值接收后 move 进成员，避免拷贝整个 Token 数组
    explicit Parser(std::vector<Token> tokens);

    // 解析整个编译单元（入口函数，由 main 驱动调用）
    TranslationUnit parseTranslationUnit();

private:
    std::vector<Token> m_tokens;   // 完整 Token 序列（含末尾哨兵 Eof）
    size_t             m_pos = 0;  // 游标：下一个待消费的 Token 下标

    // ── 模板形参作用域：帧链（对应 clang 的 Scope::TemplateParamScope 链）──
    // clang 的模板形参作用域**不是**独立容器，只是统一 Scope 链上的一个【种类位】
    //   （clang/include/clang/Sema/Scope.h:81）：Sema::ActOnTypeParameter 末尾用
    //   S->AddDecl(Param) 把形参挂进当前 Scope 的声明链（SemaTemplate.cpp:1074），
    //   进出由 RAII 的 MultiParseScope 负责（ParseTemplate.cpp:332）⇒"内层优先"是
    //   Scope 链的天然性质，无需手写 parent。
    // 本项目没有通用 Scope 类，故用等价的【帧链】：一个 template<...> 一份帧，
    //   parent 指针顶替 Scope::getParent()。
    // 【为什么帧是栈上局部对象】生命周期 == 该 template 声明的解析范围：构造即入栈、
    //   析构即出栈，异常路径由栈展开自动保证恢复（对照 clang：MultiParseScope 同样是
    //   "构造 Enter、析构 Exit"）。
    struct TemplateParamFrame {
        const TemplateDecl* owner  = nullptr;  // 归属：这是哪个 template<>
        size_t              count  = 0;        // 已注册形参个数（随解析推进增长）
        TemplateParamFrame* parent = nullptr;  // 外层帧（clang: Scope::getParent()）
        Parser*             parser = nullptr;  // 出栈时回写 m_currentFrame

        TemplateParamFrame(Parser* p, const TemplateDecl* d)
            : owner(d), parent(p->m_currentFrame), parser(p) {
            p->m_currentFrame = this;                        // ← 入栈
        }
        ~TemplateParamFrame() {
            if (parser) parser->m_currentFrame = parent;     // ← 出栈
        }
        TemplateParamFrame(const TemplateParamFrame&) = delete;
        TemplateParamFrame& operator=(const TemplateParamFrame&) = delete;
    };

    // 当前帧（无模板上下文时为 nullptr）—— 对应 clang 的"当前 Scope"
    TemplateParamFrame* m_currentFrame = nullptr;

    // 查模板形参：从当前帧往外层帧走（内层优先），返回形参本身而非 bool（查不到即
    //   nullptr）。对照 clang：Sema 的名字查找沿 Scope 链上行。
    // ★ 分水岭是「存下标 vs 存地址」，不是「存什么类型的指针」：帧里只留 owner + count，
    //   count 是上界，查询时现取 `owner->templateParams[i]` —— 下标描述的始终是同一
    //   逻辑位置，与 vector 扩容无关。
    // ★ 别把因果说反：防悬空【不是】只存下标的功劳。ast.h 的元素早已是 shared_ptr，节点
    //   住在 vector 之外的稳定地址上（扩容搬的只是指针值），缓存 `const TemplateParam*`
    //   本就不会悬空；只存下标的收益是【少一条必须记住的不变量】，防悬空由 shared_ptr 承担。
    //   对照 clang：TemplateParameterList 只存 NamedDecl* 数组 + 长度，节点的命由
    //   ASTContext 的 arena 保，二者分工一致。
    const TemplateParam* lookupTemplateParam(const std::string& name) const;

    // ── Token 流操作（LL(1) 前瞻的底层设施）──
    // 前瞻（lookahead）：不移动游标，查看当前 Token，据此决定走哪条产生式分支。
    const Token& current() const;            // 查看当前 Token（不消费）
    const Token& peek() const;               // current() 的别名（语义提示：只看不吃）
    const Token& advance();                  // 消费当前 Token 并推进游标，返回被消费的 Token
    bool         check(TokenType t) const;   // 前瞻判断：当前 Token 是否为 t？
    bool         match(TokenType t);         // check + 条件 advance（"尝试消费"）
    const Token& expect(TokenType t, const std::string& msg);  // 强制消费，不符则报错
    bool         isAtEnd() const;            // 是否到达流末尾（Eof 哨兵）

    // ── 错误处理（panic 模式：抛异常中止当前编译单元，不做 Token 级错误恢复）──
    [[noreturn]] void error(const std::string& msg) const;
    [[noreturn]] void errorAt(const Token& tok, const std::string& msg) const;

    // ── 类型解析 ──
    // type := ['const'] base-type ('*' | '&' | '&&')*
    // base-type := 'int'|'double'|'bool'|'void'|'auto' | IDENT(类名/模板参数名)
    TypePtr parseType();

    // ── 模板实参解析（[temp.arg]）──
    // template-argument-list := '<' template-argument (',' template-argument)* '>'
    // template-argument      := type-id | constant-expression
    //   类型实参（Box<int>）  → TemplateArg{kind=Type,     type}
    //   非类型实参（Buf<4>）  → TemplateArg{kind=Integral, value}   ★ NTTP
    // 调用约定：本函数自己消费 '<' 与 '>'（调用前不预消费，返回时整段已吃掉）。
    // 对照 clang：Parser::ParseTemplateArgumentList（ParseTemplate.cpp）
    //   真实现里 constant-expression 要走完整表达式解析 + 常量求值
    //   （Sema::ActOnNonTypeTemplateArgument）。本项目教学简化：只认整数字面量，
    //   不做常量折叠（如 Buf<2+2> 不支持，见 ROADMAP 主线 D）。
    std::vector<TemplateArg> parseTemplateArgumentList();

    // ── 声明解析 ──
    // declaration := template-decl | class-decl | enum-decl | namespace-decl
    //              | type-alias-decl | global-var-decl | ['virtual'] function-decl
    DeclPtr            parseDeclaration();
    TemplateDeclPtr    parseTemplateDecl();
    // outSpecPattern 非空时，若类名后紧跟模板 id（Box<T*, T>），
    // 把尖括号里的模式写回该向量 —— 供 parseTemplateDecl 判定偏特化/全特化。
    ClassDeclPtr       parseClassDecl(std::vector<TypePtr>* outSpecPattern = nullptr);
    EnumDeclPtr        parseEnumDecl();
    NamespaceDeclPtr   parseNamespaceDecl();
    TypeAliasDeclPtr   parseTypeAliasDecl();
    // ── 推导指引（[temp.deduct.guide]，理论见 docs/learn/28）──
    // 文法：Name '(' params ')' '->' Name '<' args '>' ';'
    //   template<class T> MyPtr(T) -> MyPtr<T>;   （模板指引，形参由外层 template<> 给）
    //   Box(int)          -> Box<int>;            （非模板指引，写死映射）
    // ⚠ 为什么要前瞻：`MyPtr(T) -> MyPtr<T>;` 与函数声明 `MyPtr f(T);` 前两个
    //   Token 完全一样（标识符 + '('），只能靠"配对右括号之后是不是 '->'"区分。
    // 对照 clang：TryParseDeductionGuide / isDeductionGuide。
    bool               looksLikeDeductionGuide() const;
    DeductionGuideDeclPtr parseDeductionGuide();
    GlobalVarDeclPtr   parseGlobalVarDecl();
    FuncDeclPtr        parseFunctionDecl(bool isVirtual = false,
                                         const std::string& ownerClass = "");
    FuncDeclPtr        parseMethodDecl(const std::string& ownerClass,
                                       AccessModifier access);
    CtorDeclPtr        parseConstructorDecl(const std::string& ownerClass);
    DtorDeclPtr        parseDestructorDecl(const std::string& ownerClass, bool isVirtual = false);
    std::vector<CtorInitializer> parseCtorInitializerList();

    // ── 语句解析 ──
    // stmt := compound-stmt | if-stmt | while-stmt | return-stmt
    //       | var-decl-stmt | expr-or-assign-stmt
    // 分派依据：前瞻 Token（'{'/'if'/'while'/'return'/类型关键字/标识符）
    StmtPtr parseStatement();
    // compound-stmt := '{' stmt* '}'
    StmtPtr parseBlockStmt();
    // var-decl-stmt := IDENT ['=' expr] ';'   （类型已由调用方 parseType 解析好）
    StmtPtr parseVarDeclStmt(TypePtr type);
    // if-stmt := 'if' '(' expr ')' stmt ['else' stmt]
    StmtPtr parseIfStmt();
    // while-stmt := 'while' '(' expr ')' stmt
    StmtPtr parseWhileStmt();
    // return-stmt := 'return' [expr] ';'
    StmtPtr parseReturnStmt();
    // delete-stmt := 'delete' ['[' ']'] expr ';'
    StmtPtr parseDeleteStmt();
    // expr-or-assign := expr ['=' expr] ';'
    StmtPtr parseExprOrAssignStmt();

    // ── 表达式解析（优先级从低到高：优先级越低在文法中越靠外/越晚绑定）──
    // 优先级上升链（每层都是 left-assoc 左结合）：
    //   expr := or-expr
    ExprPtr parseExpression();
    // or-expr  := and-expr ('||' and-expr)*
    ExprPtr parseOrExpr();
    // and-expr := equality ('&&' equality)*
    ExprPtr parseAndExpr();
    // equality := comparison (('==' | '!=') comparison)*
    ExprPtr parseEqualityExpr();
    // comparison := additive (('<'|'>'|'<='|'>=') additive)*
    ExprPtr parseComparisonExpr();
    // additive := multiplicative (('+' | '-') multiplicative)*
    ExprPtr parseAdditiveExpr();
    // multiplicative := unary (('*' | '/' | '%') unary)*
    ExprPtr parseMultiplicativeExpr();
    // unary := ('-' | '!') unary | postfix
    ExprPtr parseUnaryExpr();
    // postfix := primary ( call-args | ('.'|'->') IDENT )*
    ExprPtr parsePostfixExpr();
    // primary := INT|BOOL|STRING|nullptr|this | 'new' IDENT ['(' args ')']
    //          | IDENT ['<' type-list '>' ]（template-id，S3 显式模板实参）
    //          | '(' expr ')'
    ExprPtr parsePrimaryExpr();

    // ── 辅助 ──
    std::vector<ExprPtr> parseArgumentList();   // call-args := '(' [expr (',' expr)*] ')'
    std::vector<Parameter> parseParameterList(); // params := [type IDENT (',' type IDENT)*]
    BinaryOp tokenToBinaryOp(TokenType t) const;
};

} // namespace minicc

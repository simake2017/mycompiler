// =============================================================================
// minicc —— Mini C++ Compiler 主程序（编译器驱动，按顺序编排 6 个阶段）
// =============================================================================
// 用法  ./minicc <source.cpp> [-o output] [-I dir] [-S] [-E] [--dump-*]
//
//   参数 ⇒ 效果（速查；<source.cpp> 必填）：
//     -o <path>  ⇒ 输出路径：默认去扩展名出可执行文件；-S 出 .s；-E 写预处理文本
//     -I <dir>   ⇒ 头文件搜索目录，可重复（-I include -I third_party）；
//                  出现顺序即搜索优先级，原样传给预处理器
//     -S         ⇒ 只跑到阶段 5 为止，出 .s 汇编（等价 gcc -S，不链接）
//     -E         ⇒ 只跑阶段 0，输出展开后纯文本（等价 gcc -E，调试宏/include 用）
//     --dump-tokens / --dump-ast       ⇒ 阶段 1 后打印 Token 流 / 阶段 2 后打印 AST
//     --dump-hierarchy / --dump-layout ⇒ 阶段 3 后打印类层次图 / 类内存布局详图
//     （多个 --dump-* 可同时使用，如 --dump-hierarchy --dump-layout）
//
// demo: ./minicc tests/test_tmpl_01.cpp             ⇒ 全管线，产出 test_tmpl_01.s
//       ./minicc tests/pp/main.cpp -I tests/pp -E   ⇒ 只看预处理结果
//
// 阶段表 —— 每阶段「把什么变成什么」：
//   0 预处理     源码.cpp + 头文件  ⇒ 展开后的纯文本（无 '#'、无宏）
//   1 词法分析   纯文本             ⇒ Token 流（以 Eof 收尾）
//   2 语法分析   Token 流           ⇒ AST（TranslationUnit）
//   3 语义分析   AST                ⇒ 类型标注 + 类布局/vtable/RTTI
//   4 模板实例化 模板蓝图 + 实参    ⇒ 具体类/函数（类=替换，函数=推导）
//   5 代码生成   带类型的 AST       ⇒ x86-64 AT&T 汇编（.s）
//   6 链接       .o（系统 as 产出） ⇒ 非 PIE 可执行文件
// 上表各阶段的实现入口（顺序同表）+ 标准章节：
//   preprocessor.cpp::processFile()            [cpp]/[lex.phases]1~4
//   lexer.cpp::tokenizeAll()                   [lex.*]
//   parser.cpp::parseTranslationUnit()         [gram]
//   semantic_analyzer.cpp::analyze()           [basic]/[class]
//   template_instantiation.cpp::instantiate()  [temp.*]
//   codegen.cpp::generate()                    （x86-64 AT&T 后端，无标准章节）
//   src/linker.cpp::link()（见 docs/learn/15）
// =============================================================================

#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "preprocessor.h"
#include "template_instantiation.h"
#include "codegen.h"
#include "linker.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

#include <format>

using namespace minicc;

// ─────────────────────────────────────────────────────────────────────────────
// 辅助函数
// ─────────────────────────────────────────────────────────────────────────────

// 读取整个文件到字符串（通用辅助 —— 当前驱动未使用：源码由阶段 0 的
// Preprocessor::readFileContents 读入）
std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error(std::format("Cannot open file: {}", path));
    }
    return std::string(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
}

// 写入字符串到文件
void writeFile(const std::string& path, const std::string& content) {
    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error(std::format("Cannot write to file: {}", path));
    }
    file << content;
}

// 打印分隔线（全阶段中文日志的一部分：每阶段开头的醒目横幅，
// 便于在滚动日志里定位"现在跑到哪一步了"）
void printPhase(const std::string& phase) {
    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════\n";
    std::cout << std::format("  {}\n", phase);
    std::cout << "════════════════════════════════════════════════════════════\n";
}

// 打印 Token 流（由 --dump-tokens 触发）。格式：行:列 类型 文本
// demo: `1:1   INT             'int'` / `1:5   IDENTIFIER      'main'`
void dumpTokens(const std::vector<Token>& tokens) {
    for (auto& tok : tokens) {
        std::cout << std::format("  {:4}:{:<3} {:<15} '{}'\n",
            tok.location.line, tok.location.column,
            tokenTypeName(tok.type), tok.text);
    }
}

// 打印节点标签 + 子节点前缀：树形连接符在这里统一决定。
static void printNode(const std::string& prefix, bool isLast, const std::string& label) {
    std::cout << prefix << (isLast ? "└── " : "├── ") << label << "\n";
}

// ── AST 树形打印（访问者版，由 --dump-ast 触发）───────────────────────────────
// 用 ├──/└──/│ 风格绘制完整 AST（含函数体、语句、表达式）。
// 分派手法：14 种表达式 + 8 种语句全部走 accept 的虚表分派，本类只重写关心的
// visit 重载，零 cast（判据见 semantic_analyzer.h 注释；理论见 docs/learn/29）。
// 未重写的节点类型 = "什么都不做"，与原先 else 分支的语义等价。
//
// 节点 ⇒ 打印出的标签（只列代表性的，其余同形）：
//   IntLiteralExpr(42)  ⇒ "IntLiteral: 42"
//   VarExpr(x)          ⇒ "VarExpr: x"
//   BinaryExpr(Add)     ⇒ "BinaryExpr: +"（左右子树递归下钻）
//   CallExpr            ⇒ "CallExpr" + arg[i] 子树
//   MemberExpr          ⇒ "MemberExpr: .x" / "->x"
//   VarDeclStmt(x:int)  ⇒ "VarDeclStmt: x : int"
//   IfStmt              ⇒ condition / thenBranch / elseBranch 三支
//   FunctionDecl(f)     ⇒ "FunctionDecl: f() → 返回类型"（构造/析构共用同一分支）
//
// 【位置信息怎么传】prefix/isLast 是"每个节点各不相同"的上下文，塞不进通用访问者接口
// ⇒ 存成成员变量，由 dumpXxx 在 accept 之前设好：
//   dumpExpr(e, "  ", true) ⇒ setPos("  ",true) ⇒ e->accept(*this) ⇒ visit 读 m_prefix/m_isLast
//   ★ 递归约定：每个 visit 必须先把 m_prefix/m_isLast 取进【局部变量】再往下递归 ——
//   子节点的 dumpXxx 会覆写成员，回来后读到的就是别人的前缀。
class AstDumper : public AstVisitor {
public:
    // ── 入口：每个都对 null 做保护，再设好位置状态后 accept ──
    void dumpExpr(const ExprPtr& e, const std::string& prefix, bool isLast) {
        if (!e) { printNode(prefix, isLast, "nullptr"); return; }
        setPos(prefix, isLast);
        e->accept(*this);
    }
    void dumpStmt(const StmtPtr& s, const std::string& prefix, bool isLast) {
        if (!s) { printNode(prefix, isLast, "nullptr"); return; }
        setPos(prefix, isLast);
        s->accept(*this);
    }
    void dumpDecl(const DeclPtr& d, const std::string& prefix, bool isLast) {
        if (!d) { printNode(prefix, isLast, "nullptr"); return; }
        setPos(prefix, isLast);
        d->accept(*this);
    }

    // 函数体打印（支持普通函数、构造函数、析构函数）。
    // 这里只有一个二路判断，用已有的 NodeKind 标签足够 —— 访问者是为 N 路
    // 分派准备的，两路分支没必要为它绕一层虚调用。
    void dumpFunctionBody(FunctionDecl& func, const std::string& prefix) {
        if (!func.body) return;

        std::string childPrefix = prefix + "    ";

        // 构造函数的初始化列表
        if (func.kind == NodeKind::Constructor) {
            auto& ctor = static_cast<ConstructorDecl&>(func);
            if (!ctor.initList.empty()) {
                std::cout << prefix << "├── InitList\n";
                for (size_t i = 0; i < ctor.initList.size(); ++i) {
                    bool last = (i + 1 == ctor.initList.size());
                    std::cout << childPrefix << (last ? "└── " : "├── ")
                              << ctor.initList[i].memberName << "\n";
                    std::string argPrefix = childPrefix + (last ? "    " : "│   ");
                    for (size_t j = 0; j < ctor.initList[i].arguments.size(); ++j) {
                        dumpExpr(ctor.initList[i].arguments[j], argPrefix,
                                 j + 1 == ctor.initList[i].arguments.size());
                    }
                }
            }
        }

        // 函数体
        dumpBlock(func.body, prefix);
    }

    // Block 打印（函数体）：与语句层的 BlockStmt 渲染不同（那里带 "BlockStmt" 标签），
    // 这里固定打成 "└── Block"，故不走 accept，直接渲染。
    void dumpBlock(const std::shared_ptr<BlockStmt>& block, const std::string& prefix) {
        if (!block) return;
        std::cout << prefix << "└── Block\n";
        std::string childPrefix = prefix + "    ";
        for (size_t i = 0; i < block->statements.size(); ++i) {
            dumpStmt(block->statements[i], childPrefix,
                     i + 1 == block->statements.size());
        }
    }

    // ── 表达式（14）──────────────────────────────────────────────────────
    void visit(IntLiteralExpr& e) override {
        printNode(m_prefix, m_isLast, std::format("IntLiteral: {}", e.value));
    }
    void visit(BoolLiteralExpr& e) override {
        printNode(m_prefix, m_isLast, std::format("BoolLiteral: {}", e.value ? "true" : "false"));
    }
    void visit(StringLiteralExpr& e) override {
        printNode(m_prefix, m_isLast, std::format("StringLiteral: \"{}\"", e.value));
    }
    void visit(NullptrLiteralExpr&) override {
        printNode(m_prefix, m_isLast, "NullptrLiteral");
    }
    void visit(VarExpr& e) override {
        printNode(m_prefix, m_isLast, std::format("VarExpr: {}", e.name));
    }
    void visit(ThisExpr&) override {
        printNode(m_prefix, m_isLast, "ThisExpr");
    }
    void visit(BinaryExpr& e) override {
        std::string opStr;
        switch (e.op) {
            case BinaryOp::Add: opStr = "+"; break;
            case BinaryOp::Sub: opStr = "-"; break;
            case BinaryOp::Mul: opStr = "*"; break;
            case BinaryOp::Div: opStr = "/"; break;
            case BinaryOp::Mod: opStr = "%"; break;
            case BinaryOp::Eq:  opStr = "=="; break;
            case BinaryOp::Neq: opStr = "!="; break;
            case BinaryOp::Lt:  opStr = "<"; break;
            case BinaryOp::Gt:  opStr = ">"; break;
            case BinaryOp::Le:  opStr = "<="; break;
            case BinaryOp::Ge:  opStr = ">="; break;
            case BinaryOp::And: opStr = "&&"; break;
            case BinaryOp::Or:  opStr = "||"; break;
        }
        printNode(m_prefix, m_isLast, std::format("BinaryExpr: {}", opStr));
        std::string cp = childPrefix();
        dumpExpr(e.left, cp, false);
        dumpExpr(e.right, cp, true);
    }
    void visit(UnaryExpr& e) override {
        // ★ 运算符文本必须【逐个列举】：早先写成 `op == Neg ? "-" : "!"` 的二元三元式，
        //   新增 Deref/Addr 后会把它们全印成 "!"，--dump-ast 直接误导（静默错味）。
        printNode(m_prefix, m_isLast,
                  std::format("UnaryExpr: {}",
                      e.op == UnaryOp::Neg   ? "-"
                    : e.op == UnaryOp::Not   ? "!"
                    : e.op == UnaryOp::Addr  ? "&" : "*"));
        dumpExpr(e.operand, childPrefix(), true);
    }
    void visit(CallExpr& e) override {
        printNode(m_prefix, m_isLast, "CallExpr");
        std::string cp = childPrefix();
        dumpExpr(e.callee, cp, e.arguments.empty());
        for (size_t i = 0; i < e.arguments.size(); ++i) {
            bool last = (i + 1 == e.arguments.size());
            std::cout << cp << (last ? "└── " : "├── ") << "arg[" << i << "]\n";
            dumpExpr(e.arguments[i], cp + (last ? "    " : "│   "), true);
        }
    }
    void visit(MemberExpr& e) override {
        printNode(m_prefix, m_isLast,
                  std::format("MemberExpr: {}{}", e.isArrow ? "->" : ".", e.memberName));
        dumpExpr(e.object, childPrefix(), true);
    }
    void visit(IndexExpr& e) override {
        printNode(m_prefix, m_isLast, "IndexExpr");
        std::string cp = childPrefix();
        dumpExpr(e.object, cp, false);
        dumpExpr(e.index, cp, true);
    }
    void visit(NewExpr& e) override {
        printNode(m_prefix, m_isLast, std::format("NewExpr: {}", e.className));
        std::string cp = childPrefix();
        for (size_t i = 0; i < e.constructorArgs.size(); ++i) {
            bool last = (i + 1 == e.constructorArgs.size());
            std::cout << cp << (last ? "└── " : "├── ") << "arg[" << i << "]\n";
            dumpExpr(e.constructorArgs[i], cp + (last ? "    " : "│   "), true);
        }
    }
    void visit(DynamicCastExpr& e) override {
        printNode(m_prefix, m_isLast, std::format("DynamicCastExpr: <{}*>", e.targetClassName));
        dumpExpr(e.operand, childPrefix(), true);
    }
    void visit(DeleteExpr& e) override {
        printNode(m_prefix, m_isLast, e.isArray ? "DeleteExpr[]" : "DeleteExpr");
        dumpExpr(e.pointerExpr, childPrefix(), true);
    }

    // ── 语句（8）────────────────────────────────────────────────────────
    void visit(ExprStmt& s) override {
        printNode(m_prefix, m_isLast, "ExprStmt");
        dumpExpr(s.expr, childPrefix(), true);
    }
    void visit(VarDeclStmt& s) override {
        printNode(m_prefix, m_isLast, std::format("VarDeclStmt: {} : {}", s.name,
                 s.declaredType ? s.declaredType->toString() : "auto"));
        if (s.initializer) dumpExpr(s.initializer, childPrefix(), true);
    }
    void visit(AssignStmt& s) override {
        printNode(m_prefix, m_isLast, "AssignStmt");
        std::string cp = childPrefix();
        dumpExpr(s.target, cp, false);
        dumpExpr(s.value, cp, true);
    }
    void visit(ReturnStmt& s) override {
        printNode(m_prefix, m_isLast, "ReturnStmt");
        if (s.value) dumpExpr(s.value, childPrefix(), true);
    }
    void visit(IfStmt& s) override {
        printNode(m_prefix, m_isLast, "IfStmt");
        std::string cp = childPrefix();
        std::cout << cp << "├── condition\n";
        dumpExpr(s.condition, cp + "│   ", true);
        std::cout << cp << "├── thenBranch\n";
        dumpStmt(s.thenBranch, cp + "│   ", true);
        if (s.elseBranch) {
            std::cout << cp << "└── elseBranch\n";
            dumpStmt(s.elseBranch, cp + "    ", true);
        }
    }
    void visit(WhileStmt& s) override {
        printNode(m_prefix, m_isLast, "WhileStmt");
        std::string cp = childPrefix();
        std::cout << cp << "├── condition\n";
        dumpExpr(s.condition, cp + "│   ", true);
        std::cout << cp << "└── body\n";
        dumpStmt(s.body, cp + "    ", true);
    }
    void visit(BlockStmt& s) override {
        printNode(m_prefix, m_isLast, "BlockStmt");
        std::string cp = childPrefix();
        for (size_t i = 0; i < s.statements.size(); ++i) {
            dumpStmt(s.statements[i], cp, i + 1 == s.statements.size());
        }
    }
    void visit(DeleteStmt& s) override {
        printNode(m_prefix, m_isLast, s.isArray ? "DeleteStmt[]" : "DeleteStmt");
        dumpExpr(s.pointerExpr, childPrefix(), true);
    }

    // ── 顶层声明（10）────────────────────────────────────────────────────
    // 普通函数与构造/析构函数共用一份头部渲染：三者都打成 "FunctionDecl: ..."
    //（构造/析构是 FunctionDecl 的派生类，故共用同一渲染分支）。
    void visit(FunctionDecl& f) override      { renderFunctionDecl(f); }
    void visit(ConstructorDecl& f) override   { renderFunctionDecl(f); }
    void visit(DestructorDecl& f) override    { renderFunctionDecl(f); }

    void visit(GlobalVarDecl& g) override {
        printNode(m_prefix, m_isLast, std::format("GlobalVarDecl: {} : {}", g.name,
                 g.declaredType ? g.declaredType->toString() : "auto"));
        if (g.initializer) dumpExpr(g.initializer, childPrefix(), true);
    }
    void visit(EnumDecl& e) override {
        printNode(m_prefix, m_isLast,
                  std::format("EnumDecl: {} ({} items)", e.name, e.items.size()));
        std::string cp = childPrefix();
        for (size_t j = 0; j < e.items.size(); ++j) {
            bool last = (j + 1 == e.items.size());
            std::cout << cp << (last ? "└── " : "├── ")
                      << std::format("{} = {}\n", e.items[j].name, e.items[j].value);
        }
    }
    void visit(NamespaceDecl& ns) override {
        printNode(m_prefix, m_isLast, std::format("NamespaceDecl: {}", ns.name));
        // 递归打印命名空间内容（目前只占位提示，未真正展开）
        if (!ns.declarations.empty()) {
            std::cout << childPrefix() << "  ...\n";
        }
    }
    void visit(TypeAliasDecl& ta) override {
        printNode(m_prefix, m_isLast, std::format("TypeAliasDecl: {} = {}", ta.aliasName,
                 ta.underlyingType ? ta.underlyingType->toString() : "?"));
    }
    void visit(ClassDecl& cls) override {
        std::string bases;
        if (!cls.baseClassNames.empty()) {
            bases = " : ";
            for (size_t j = 0; j < cls.baseClassNames.size(); ++j) {
                bases += (j ? ", " : "") + cls.baseClassNames[j];
            }
        }
        printNode(m_prefix, m_isLast, std::format("ClassDecl: {}{}", cls.name, bases));
        std::string cp = childPrefix();

        // 字段
        for (size_t j = 0; j < cls.fields.size(); ++j) {
            auto& f = cls.fields[j];
            bool lastField = (j + 1 == cls.fields.size()) && cls.methods.empty();
            std::cout << cp << (lastField ? "└── " : "├── ")
                      << std::format("Field: {} : {} (+{}, {}B)\n", f.name,
                             f.type ? f.type->toString() : "?", f.offset, f.size);
        }

        // 方法
        for (size_t j = 0; j < cls.methods.size(); ++j) {
            auto& method = cls.methods[j];
            bool lastMethod = (j + 1 == cls.methods.size());
            std::string flags;
            if (method->isVirtual)  flags += " [virtual]";
            if (method->isOverride) flags += " [override]";
            if (method->kind == NodeKind::Constructor) flags += " [ctor]";
            if (method->kind == NodeKind::Destructor)  flags += " [dtor]";

            std::cout << cp << (lastMethod ? "└── " : "├── ")
                      << std::format("Method: {}() → {}{}\n", method->name,
                             method->returnType ? method->returnType->toString() : "void",
                             flags);

            if (method->body) {
                dumpFunctionBody(*method, cp + (lastMethod ? "    " : "│   "));
            }
        }
    }
    void visit(TemplateDecl& tmpl) override {
        std::string params = "<";
        for (size_t j = 0; j < tmpl.typeParams.size(); ++j) {
            params += (j ? ", " : "") + tmpl.typeParams[j];
        }
        params += ">";
        printNode(m_prefix, m_isLast, std::format("TemplateDecl({}){}",
            tmpl.isClassTemplate()    ? "class"
            : tmpl.isAliasTemplate()  ? "alias"
            : tmpl.isDeductionGuide() ? "deduction-guide"
                                      : "function", params));
        std::string cp = childPrefix();

        if (tmpl.classTemplate) {
            std::cout << cp << "└── ClassTemplate\n";
        }
        if (tmpl.funcTemplate) {
            std::cout << cp << "└── "
                      << std::format("FunctionTemplate: {}({} params) → {}\n",
                             tmpl.funcTemplate->name,
                             tmpl.funcTemplate->parameters.size(),
                             tmpl.funcTemplate->returnType
                                 ? tmpl.funcTemplate->returnType->toString() : "void");
        }
    }

private:
    std::string m_prefix;
    bool        m_isLast = true;

    void setPos(const std::string& prefix, bool isLast) {
        m_prefix = prefix;
        m_isLast = isLast;
    }
    std::string childPrefix() const {
        return m_prefix + (m_isLast ? "    " : "│   ");
    }

    void renderFunctionDecl(FunctionDecl& func) {
        printNode(m_prefix, m_isLast, std::format("FunctionDecl: {}() → {}", func.name,
                 func.returnType ? func.returnType->toString() : "?"));
        std::string cp = childPrefix();

        // 参数
        for (size_t j = 0; j < func.parameters.size(); ++j) {
            bool lastParam = (j + 1 == func.parameters.size()) && !func.body;
            std::cout << cp << (lastParam ? "└── " : "├── ")
                      << std::format("Param: {} : {}\n", func.parameters[j].name,
                             func.parameters[j].type
                                 ? func.parameters[j].type->toString() : "?");
        }

        if (func.body) {
            dumpFunctionBody(func, cp);
        }
    }
};

// 顶层声明打印
void dumpAST(const TranslationUnit& unit) {
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  语法树 (Abstract Syntax Tree)                                   ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n  TranslationUnit\n";

    AstDumper dumper;
    for (size_t i = 0; i < unit.declarations.size(); ++i) {
        dumper.dumpDecl(unit.declarations[i], "  ", i + 1 == unit.declarations.size());
    }

    std::cout << "\n════════════════════════════════════════════════════════════\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// 主函数：编译器驱动 —— 解析命令行 → 依次跑阶段 0~6 → 写汇编 / 链接
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // 缺少输入文件 → 打印用法并以非零码退出
    if (argc < 2) {
        std::cerr << "Usage: minicc <source.cpp> [-o output] [-I dir] [-S] [-E] "
                     "[--dump-tokens] [--dump-ast]\n";
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile;
    // ── DumpOptions：诊断可视化标志（--dump-* 按需触发，正常编译流程不受影响）──
    struct DumpOptions {
        bool tokens = false;        // --dump-tokens
        bool ast = false;           // --dump-ast
        bool hierarchy = false;     // --dump-hierarchy
        bool layout = false;        // --dump-layout
        bool any() const { return tokens || ast || hierarchy || layout; }
    } dump;
    bool emitPreprocessed = false;          // -E：只输出预处理结果（同 gcc -E）
    bool emitAsmOnly = false;              // -S：只吐汇编，不链接（同 gcc -S）
    std::vector<std::string> includeDirs;   // -I 搜索目录（可多次）

    // 解析命令行参数：单破折号短选项；-o/-I 吃掉紧随其后的值；-I 可多次出现，
    // 顺序即搜索优先级；-E 让驱动在阶段 0 结束后立即返回。
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            outputFile = argv[++i];
        } else if (arg == "-I" && i + 1 < argc) {
            includeDirs.push_back(argv[++i]);
        } else if (arg == "-E") {
            emitPreprocessed = true;
        } else if (arg == "-S") {
            emitAsmOnly = true;             // 只到汇编为止（保留旧行为）
        } else if (arg == "--dump-tokens") {
            dump.tokens = true;
        } else if (arg == "--dump-ast") {
            dump.ast = true;
        } else if (arg == "--dump-hierarchy") {
            dump.hierarchy = true;
        } else if (arg == "--dump-layout") {
            dump.layout = true;
        }
    }

    // 默认输出文件名（-E 模式不需要）
    if (outputFile.empty() && !emitPreprocessed) {
        if (emitAsmOnly) {
            outputFile = std::filesystem::path(inputFile)
                .replace_extension(".s").string();
        } else {
            // 默认直出可执行文件（同 gcc：去扩展名）
            outputFile = std::filesystem::path(inputFile)
                .replace_extension("").string();
        }
    }

    // 六个阶段串联执行；任何阶段抛出的异常（预处理错/语法错/类型错...）
    // 都由函数末尾的 catch 统一打印 [ERROR] 并以非零码退出。
    try {
        // ═══════════════════════════════════════════════════════════════
        // 阶段 0：预处理 (Preprocessing)
        // ═══════════════════════════════════════════════════════════════
        // [cpp.phase] 翻译阶段 2~4：行拼接 → #include 并合 → 条件编译 → 宏展开。
        // 之后词法分析工作在"展开后的纯文本"上 —— 教学版是文本级实现，
        // 与 clang 的 token 级预处理不同（见 docs/learn/07）。
        printPhase("Phase 0: Preprocessing (预处理)");

        // 把命令行的 -I 目录交给预处理器（搜索顺序 = 出现顺序）
        PreprocessorOptions ppOpts;
        ppOpts.includePaths = includeDirs;
        Preprocessor preprocessor(ppOpts); // 预处理器
        // 一次调用完成全部阶段 0 工作：行拼接 → #include 并合 → 条件编译 → 宏展开
        std::string preprocessed = preprocessor.processFile(inputFile);

        std::cout << std::format("  {} file(s) included, {} macro(s) defined\n",
            preprocessor.includeCount(), preprocessor.macros().size());

        // -E 模式到此为止：预处理文本写到 -o 指定的文件；
        // 未指定 -o 时直接打印到 stdout（同 gcc -E 的行为）。
        if (emitPreprocessed) {
            if (outputFile.empty()) {
                std::cout << preprocessed;
            } else {
                writeFile(outputFile, preprocessed);
                std::cout << std::format("  Preprocessed output written to: {}\n", outputFile);
            }
            return 0;
        }

        printPhase("Phase 1: Lexical Analysis (词法分析)");

        // Lexer 的输入是预处理后的文本而非原始文件：此时已无 '#' 指令、宏已就地替换、
        // 头文件已并合（[lex.phases]：分词在预处理之后）。
        std::string source = preprocessed;   // 词法分析作用于预处理后的文本
        Lexer lexer(source);
        std::vector<Token> tokens = lexer.tokenizeAll();

        std::cout << std::format("  {} tokens generated\n", tokens.size());

        if (dump.tokens) {
            std::cout << "\n  Token dump:\n";
            dumpTokens(tokens);
        }

        // ═══════════════════════════════════════════════════════════════
        // 阶段 2：语法分析 (Syntax Analysis)
        // ═══════════════════════════════════════════════════════════════
        printPhase("Phase 2: Syntax Analysis (语法分析)");

        // 递归下降解析（对照标准 [gram] 文法的子集），
        // 产出 TranslationUnit = 顶层声明列表（函数/类/模板）。
        Parser parser(tokens);
        TranslationUnit unit = parser.parseTranslationUnit();

        std::cout << std::format("  {} top-level declarations parsed\n",
            unit.declarations.size());

        if (dump.ast) {
            std::cout << "\n  AST dump:\n";
            dumpAST(unit);
        }

        // ═══════════════════════════════════════════════════════════════
        // 阶段 3：语义分析 (Semantic Analysis)
        // ═══════════════════════════════════════════════════════════════
        printPhase("Phase 3: Semantic Analysis (语义分析)");
        std::cout << "  Type checking & auto deduction...\n";

        // 类型检查 + auto 推导 + 类内存布局（字段偏移/对齐、vtable、RTTI）。
        // 布局结果供两处消费：下方打印（可观测性）与阶段 5 CodeGen（访存偏移）。
        SemanticAnalyzer semaAnalyzer;
        semaAnalyzer.analyze(unit);

        auto& classTypes = semaAnalyzer.getClassTypes();
        auto& functions = semaAnalyzer.getFunctions();

        std::cout << std::format("  {} classes analyzed\n", classTypes.size());
        std::cout << std::format("  {} functions analyzed\n", functions.size());

        // 打印类的内存布局
        for (auto& [name, type] : classTypes) {
            std::cout << std::format("\n  Class '{}' layout:\n", name);
            std::cout << std::format("    Total size: {} bytes\n",
                type->classLayout.totalSize);
            if (type->classLayout.hasVTable) {
                std::cout << "    Has vtable: YES\n";
                std::cout << std::format("    _vptr at offset 0 (8 bytes, hidden)\n");
                for (auto& entry : type->classLayout.vtableEntries) {
                    std::cout << std::format(
                        "    vtable[{}]: {}\n", entry.index, entry.mangledName);
                }
                std::cout << std::format("    RTTI: {} (at vtable[-1])\n",
                    type->classLayout.rttiMangledName);
            }
            for (auto& field : type->classLayout.fields) {
                std::cout << std::format("    Field '{}' : {} (offset {}, size {})\n",
                    field.name,
                    field.type ? field.type->toString() : "?",
                    field.offset, field.size);
            }
        }

        // ── 诊断可视化输出（--dump-* 触发）──
        if (dump.hierarchy) {
            semaAnalyzer.dumpHierarchy(classTypes);
        }
        if (dump.layout) {
            semaAnalyzer.dumpLayout(classTypes);
        }

        // ═══════════════════════════════════════════════════════════════
        // 阶段 4：模板实例化 (Template Instantiation)
        // ═══════════════════════════════════════════════════════════════
        printPhase("Phase 4: Template Instantiation (模板实例化)");

        // 模板实例化 = 结构化替换（[temp.inst]）：用具体类型实参替换模板参数。
        // 类模板：此处用 int/double/int*/int&/int&&/const int& 六种实参演示；
        // 函数模板：蓝图只存储，实例化由调用点实参推导驱动（[temp.deduct]，S2+）。
        TemplateInstantiator instantiator;
        auto& templates = semaAnalyzer.getTemplates();

        if (!templates.empty()) {
            std::cout << std::format("  Found {} template blueprint(s)\n\n", templates.size());

            for (auto& tmpl : templates) {
                std::cout << std::format("  Template blueprint: {} <",
                    tmpl->templateName());
                for (size_t i = 0; i < tmpl->typeParams.size(); i++) {
                    if (i > 0) std::cout << ", ";
                    std::cout << tmpl->typeParams[i];
                }
                std::cout << ">\n";

                // 函数模板（S1）：蓝图存储即可，实例化由调用点实参推导驱动（S2+）。
                // 别名模板（[temp.alias]）：别名不是新类型 ⇒ 没有"实例化"这一步，
                // 使用时在 Phase 3 由 expandAliasTemplate【解糖】成既有类型，
                // 既不产生新类也不产生新符号。
                // ⚠ 必须在这里拦下：下面各分支都假设 classTemplate != nullptr，
                // 别名模板的 classTemplate 是空的，会直接空指针崩。
                if (tmpl->isAliasTemplate()) {
                    std::cout << std::format(
                        "  (alias template '{}': no instantiation — desugars to an "
                        "existing type at use sites — see Phase 3)\n",
                        tmpl->templateName());
                    continue;
                }

                // 推导指引（[temp.deduct.guide]）：没有实体，只在 CTAD 那一刻被查一次、
                // 用完即弃 —— 不参与实例化演示，同别名模板必须在这里拦下。
                if (tmpl->isDeductionGuide()) {
                    std::cout << std::format(
                        "  (deduction guide for '{}': a CTAD-only rule — never "
                        "instantiated; see Phase 3)\n", tmpl->templateName());
                    continue;
                }

                if (tmpl->isFunctionTemplate()) {
                    std::cout << std::format(
                        "  (function template '{}': blueprint stored; instantiation is "
                        "call-site driven — see S2 deduction / S5 instantiation)\n",
                        tmpl->templateName());
                    continue;
                }

                // ── 特化（偏/全）不参与"用各种类型演示实例化"（[temp.class.spec]）──
                // 特化的形参模式限定了它只对某一类实参有意义（如 Box<T*,T> 只收
                // 「指针 + 同类型」），拿 int/double/int&/… 硬填会产出语义错误的实例。
                // 它的正确触发方式是【使用点实参】—— Sema::selectClassTemplate 择优命中。
                if (tmpl->isSpecialization()) {
                    std::cout << std::format(
                        "  ({} specialization of '{}': matched on demand at use sites "
                        "via selectClassTemplate — see Phase 3)\n",
                        tmpl->isExplicitSpec() ? "explicit (full)" : "partial",
                        tmpl->templateName());
                    continue;
                }

                // 实参包装便利函数：TemplateArg 是 tagged 值，类型实参要显式标形态。
                // demo: ta(Type::makeInt()) ⇒ TemplateArg{kind=Type, type=int}
                auto ta = [](TypePtr t) { return TemplateArg::ofType(std::move(t)); };

                // ★ 多种实例化演示（仅类模板）★
                // 分支顺序：先按【形参形态】判 NTTP，再按个数展开类型实参演示 ——
                // 否则 template<int N> 会落进下面的「1 个类型形参」分支，被硬塞 6 个类型实参。
                if (tmpl->typeParams.size() == 1
                    && tmpl->templateParams[0]->kind == TemplateParamKind::NonType) {
                    // 单 NTTP 模板：Buf<4> —— 值替换的完整演示
                    std::cout << std::format(
                        "\n  ─── Instantiation (NTTP): {}<4> ───\n",
                        tmpl->classTemplate->name);
                    auto instance = instantiator.instantiate(
                        tmpl, {TemplateArg::ofValue(4)});
                    std::cout << std::format("  → Instantiated: {}\n", instance->name);
                }
                else if (tmpl->typeParams.size() == 1) {
                    // 实例化 1: T = int
                    std::cout << std::format("\n  ─── Instantiation 1: {}<int> ───\n",
                        tmpl->classTemplate->name);
                    auto instance1 = instantiator.instantiate(
                        tmpl, {ta(Type::makeInt())});
                    std::cout << std::format("  → Instantiated: {}\n", instance1->name);

                    // 实例化 2: T = double
                    std::cout << std::format("\n  ─── Instantiation 2: {}<double> ───\n",
                        tmpl->classTemplate->name);
                    auto instance2 = instantiator.instantiate(
                        tmpl, {ta(Type::makeDouble())});
                    std::cout << std::format("  → Instantiated: {}\n", instance2->name);

                    // 实例化 3: T = int* (指针类型)
                    std::cout << std::format("\n  ─── Instantiation 3: {}<int*> ───\n",
                        tmpl->classTemplate->name);
                    auto instance3 = instantiator.instantiate(
                        tmpl, {ta(Type::makePointer(Type::makeInt()))});
                    std::cout << std::format("  → Instantiated: {}\n", instance3->name);

                    // 实例化 4: T = int& (左值引用)
                    std::cout << std::format("\n  ─── Instantiation 4: {}<int&> ───\n",
                        tmpl->classTemplate->name);
                    auto instance4 = instantiator.instantiate(
                        tmpl, {ta(Type::makeLValueReference(Type::makeInt()))});
                    std::cout << std::format("  → Instantiated: {}\n", instance4->name);

                    // 实例化 5: T = int&& (右值引用 — 演示万能引用)
                    std::cout << std::format("\n  ─── Instantiation 5: {}<int&&> ───\n",
                        tmpl->classTemplate->name);
                    auto instance5 = instantiator.instantiate(
                        tmpl, {ta(Type::makeRValueReference(Type::makeInt()))});
                    std::cout << std::format("  → Instantiated: {}\n", instance5->name);

                    // 实例化 6: T = const int& (常量引用)
                    std::cout << std::format("\n  ─── Instantiation 6: {}<const int&> ───\n",
                        tmpl->classTemplate->name);
                    auto instance6 = instantiator.instantiate(
                        tmpl, {ta(Type::makeLValueReference(Type::makeConst(Type::makeInt())))});
                    std::cout << std::format("  → Instantiated: {}\n", instance6->name);
                }
                else if (tmpl->typeParams.size() == 2) {
                    // ── 按形参形态分派第二组实参（[temp.arg]）──
                    // 第二实参是类型还是值，一律由 templateParams[1].kind 决定。
                    // ★ 若固定传类型实参，template<class T, int N> 会拿类型去填 NTTP 槽。
                    const TemplateParam& p1 = *tmpl->templateParams[0];
                    const TemplateParam& p2 = *tmpl->templateParams[1];
                    if (p1.kind == TemplateParamKind::Template
                        || p2.kind == TemplateParamKind::Template) {
                        // 模板模板形参（[temp.param]/4）没有"用固定类型演示"的合理形态：
                        // 这一位要的是【模板名】，喂 int/double 会造出一个字段类型停在
                        // 裸 C 上的假实例。这种模板只能由使用点驱动实例化。
                        std::cout << std::format(
                            "  (template template parameter present: instantiation is "
                            "use-site driven — see Phase 3)\n");
                    }
                    else if (p1.kind == TemplateParamKind::NonType) {
                        // 首形参是 NTTP（`template<bool B, class T>` 这种"值 + 类型"混排）。
                        // ★ 此前没有这条分支，`enable_if<B, T>` 会落进下面的"两类型形参"
                        //   分支、被喂 <int, double> —— 类型实参塞进 NTTP 槽，实例化直接抛
                        //   "must be a value, but 'int' is a type"。判据必须【逐位看形参
                        //   自己的 kind】，不能只看第二位的形态。
                        std::cout << std::format(
                            "\n  ─── Instantiation: {}<true, int> (NTTP + type) ───\n",
                            tmpl->classTemplate->name);
                        auto instance = instantiator.instantiate(
                            tmpl, {TemplateArg::ofValue(1, Type::makeBool()),
                                   ta(Type::makeInt())});
                        std::cout << std::format("  → Instantiated: {}\n", instance->name);
                    }
                    else if (p2.kind == TemplateParamKind::NonType) {
                        std::cout << std::format(
                            "\n  ─── Instantiation: {}<int, 8> (NTTP) ───\n",
                            tmpl->classTemplate->name);
                        auto instance = instantiator.instantiate(
                            tmpl, {ta(Type::makeInt()), TemplateArg::ofValue(8)});
                        std::cout << std::format("  → Instantiated: {}\n", instance->name);
                    }
                    else {
                        // 多参数模板实例化: T=int, U=double
                        std::cout << std::format("\n  ─── Instantiation: {}<int, double> ───\n",
                            tmpl->classTemplate->name);
                        auto instance = instantiator.instantiate(
                            tmpl, {ta(Type::makeInt()), ta(Type::makeDouble())});
                        std::cout << std::format("  → Instantiated: {}\n", instance->name);
                    }
                }
            }

            // ── 引用折叠专项演示 ──
            std::cout << std::format("\n  ─── Reference Collapsing Demo ───\n");
            std::cout << "  万能引用(Forwarding Reference) 引用折叠规则:\n";
            std::cout << "    T&& where T=int   → int&&       (右值引用)\n";
            std::cout << "    T&& where T=int&  → int& && → int&   (折叠为左值引用)\n";
            std::cout << "    T&& where T=int&& → int&& && → int&&  (折叠为右值引用)\n";
            std::cout << "  ★ 上面的实例化过程中 substituteType() 已自动处理引用折叠 ★\n";
        } else {
            std::cout << "  No templates to instantiate.\n";
        }

        // ═══════════════════════════════════════════════════════════════
        // 阶段 5：代码生成 (Code Generation)
        // ═══════════════════════════════════════════════════════════════
        printPhase("Phase 5: Code Generation (代码生成)");
        std::cout << "  Generating x86-64 assembly...\n";

        // 遍历带类型信息的 AST，生成 x86-64 AT&T 语法汇编
        // （含 vtable/RTTI 数据段、GCC 风格 mangling 的符号名）。
        CodeGen codegen;
        std::string assembly = codegen.generate(unit, classTypes, functions);

        // 写入汇编文件（-S 模式写到 -o 目标；默认模式写临时 .s 供阶段 6 链接）
        if (emitAsmOnly) {
            writeFile(outputFile, assembly);
            std::cout << std::format("  Assembly written to: {}\n", outputFile);
        }

        // ═══════════════════════════════════════════════════════════════
        // 阶段 6：链接（系统 as + 内置 mini-ld → 可执行文件）
        // ═══════════════════════════════════════════════════════════════
        printPhase("Phase 6: Linking (链接)");

        if (emitAsmOnly) {
            // -S：止步于汇编（等价 gcc -S），链接器不出场
            std::cout << "  -S 已指定：只输出汇编，跳过链接。\n";
            printPhase("Compilation Complete! 编译完成!");
            return 0;
        }

        // 中间文件放系统临时目录，不污染用户目录
        std::string stem = std::filesystem::path(outputFile).filename().string();
        auto tmpDir = std::filesystem::temp_directory_path();
        std::string asmPath = (tmpDir / ("minicc_" + stem + ".s")).string();
        std::string objPath = (tmpDir / ("minicc_" + stem + ".o")).string();
        writeFile(asmPath, assembly);

        // ① 汇编：借用系统 as（汇编器只做机械翻译，见 docs/learn/15）
        std::cout << std::format("  [as] {} → {}\n", asmPath, objPath);
        int rc = std::system(("as -o " + objPath + " " + asmPath).c_str());
        if (rc != 0) {
            std::cerr << std::format("[ERROR] 汇编失败 (as 退出码 {})\n", rc);
            return 1;
        }

        // ② 链接：内置 MiniLinker（符号决议 + 重定位回填 + 写可执行 ELF）
        MiniLinker linker;
        LinkResult lr = linker.link({objPath}, outputFile);
        if (!lr.ok) {
            std::cerr << std::format("\n[LINK ERROR] {}\n", lr.errorMsg);
            return 1;
        }
        std::cout << std::format("  链接成功：{} 个符号决议, {} 条重定位回填, 入口 {:#x}\n",
            lr.inputSymbols, lr.resolvedRelocs, lr.entryAddr);
        std::cout << std::format("  可执行文件: {}   （直接运行: {}）\n", outputFile, outputFile);

        printPhase("Compilation Complete! 编译完成!");
        return 0;

    } catch (const std::exception& e) {
        // 统一错误出口：各阶段（含预处理器的 ppError）抛出的异常都在这里收口
        std::cerr << std::format("\n[ERROR] {}\n", e.what());
        return 1;
    }
}

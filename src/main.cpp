// =============================================================================
// minicc —— Mini C++ Compiler 主程序
// =============================================================================
// 编译器驱动：按顺序编排 6 个阶段的执行。
//
// 完整流水线：
//   源码 (.cpp)
//     → [1] 词法分析 (Lexer)        → Token 流
//     → [2] 语法分析 (Parser)       → AST
//     → [3] 语义分析 (SemaAnalyzer) → 带类型信息的 AST + 内存布局
//     → [4] 模板实例化 (TemplateInst) → 展开后的真实代码
//     → [5] 代码生成 (CodeGen)      → x86-64 汇编 (.s)
//     → [6] 链接 (MiniLinker)       → 可执行文件（内置 _start + malloc，不依赖系统 ld）
//
// 用法：
//   ./minicc <source.cpp> [-o output.s] [--dump-tokens] [--dump-ast]
// =============================================================================
//
// ─── 命令行参数详解（本文件 main() 解析）─────────────────────────────────
//   minicc <source.cpp> [-o output] [-I dir] [-S] [-E] [--dump-*]
//
//   <source.cpp>    输入源文件（必填）
//   -o output       输出路径：默认模式输出可执行文件（缺省取 <source> 去扩展名）；
//                   -S 模式输出汇编 <source>.s；-E 模式写预处理文本。
//   -S              只到阶段 5 为止，输出 .s 汇编（等价 gcc -S，不链接）
//   -I dir          头文件搜索目录，可重复（-I include -I third_party），
//                   出现顺序即搜索优先级，原样传给预处理器。
//   -E              只执行阶段 0（预处理）并输出结果，等价 gcc -E，
//                   用于调试宏/include/条件编译问题。
//   --dump-tokens       阶段 1 后打印完整 Token 流（调试）
//   --dump-ast          阶段 2 后打印 AST 顶层声明（调试）
//   --dump-hierarchy    阶段 3 后打印类层次结构图（继承树 + typeinfo 链）
//   --dump-layout       阶段 3 后打印类内存布局详图（对象→vtable→RTTI 三层）
//   多个 --dump-* 可同时使用，如 --dump-hierarchy --dump-layout
//
// 示例：
//   ./minicc tests/test_tmpl_01.cpp                 # 全管线 → test_tmpl_01.s
//   ./minicc tests/pp/main.cpp -I tests/pp -E       # 只看预处理结果
//
// ─── 编译驱动全景图（阶段 ↔ 实现文件 ↔ 本文件调用点）────────────────────
//
//   source.cpp
//     │ 阶段0 预处理     preprocessor.cpp            processFile()
//     │        行拼接→include并合→条件编译→宏展开     [cpp]/[lex.phases]1~4
//     ▼
//   展开后纯文本（宏已替换、头文件已并合、条件分支已裁决）
//     │ 阶段1 词法分析   lexer.cpp                   tokenizeAll()
//     ▼
//   Token 流
//     │ 阶段2 语法分析   parser.cpp                  parseTranslationUnit()
//     ▼
//   AST（抽象语法树，TranslationUnit）
//     │ 阶段3 语义分析   semantic_analyzer.cpp       analyze()
//     │        类型检查 / auto 推导 / 内存布局 / vtable
//     ▼
//   带类型信息的 AST + 类布局表
//     │ 阶段4 模板实例化 template_instantiation.cpp  instantiate()
//     │        类模板=结构化替换；函数模板=调用点推导驱动
//     ▼
//   展开后的实例
//     │ 阶段5 代码生成   codegen.cpp                 generate()
//     ▼
//   output.s（x86-64 AT&T 汇编）
//     │ 阶段6 链接：系统 as 出 .o → 内置 MiniLinker（src/linker.cpp）
//     │        合并节 → 符号决议 → 重定位回填 → 写最小可执行 ELF
//     ▼
//   可执行文件（非 PIE，入口 _start；内置 malloc/free，不依赖系统 ld）
// ─────────────────────────────────────────────────────────────────────────

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

// 读取整个文件到字符串
// （通用辅助；注意当前驱动实际未使用——阶段 0 的 Preprocessor 自带
// readFileContents，由它负责读入源码）
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

// 打印分隔线
// 全阶段中文日志的一部分：每个阶段开始前先打一条醒目横幅，
// 方便在滚动日志里快速定位"现在跑到哪一步了"。
void printPhase(const std::string& phase) {
    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════\n";
    std::cout << std::format("  {}\n", phase);
    std::cout << "════════════════════════════════════════════════════════════\n";
}

// 打印 Token 流（调试用）
// 由 --dump-tokens 触发。格式：行:列 类型 文本。demo：
//      1:1   INT             'int'
//      1:5   IDENTIFIER      'main'
void dumpTokens(const std::vector<Token>& tokens) {
    for (auto& tok : tokens) {
        std::cout << std::format("  {:4}:{:<3} {:<15} '{}'\n",
            tok.location.line, tok.location.column,
            tokenTypeName(tok.type), tok.text);
    }
}

// ── AST 树形打印（增强版）──────────────────────────────────────────────────────
// 使用 ├──/└──/│ 风格绘制完整 AST，包含函数体、语句、表达式。
// 由 --dump-ast 触发。

static void dumpExpr(ExprPtr expr, const std::string& prefix, bool isLast);
static void dumpStmt(StmtPtr stmt, const std::string& prefix, bool isLast);
static void dumpBlock(std::shared_ptr<BlockStmt> block, const std::string& prefix);

// 打印节点标签 + 子节点前缀
static void printNode(const std::string& prefix, bool isLast, const std::string& label) {
    std::string branch = isLast ? "└── " : "├── ";
    std::cout << prefix << branch << label << "\n";
}

// 表达式打印：按动态类型分发
static void dumpExpr(ExprPtr expr, const std::string& prefix, bool isLast) {
    if (!expr) {
        printNode(prefix, isLast, "nullptr");
        return;
    }

    std::string branch = isLast ? "└── " : "├── ";
    std::string childPrefix = prefix + (isLast ? "    " : "│   ");

    if (auto e = std::dynamic_pointer_cast<IntLiteralExpr>(expr)) {
        printNode(prefix, isLast, std::format("IntLiteral: {}", e->value));
    }
    else if (auto e = std::dynamic_pointer_cast<BoolLiteralExpr>(expr)) {
        printNode(prefix, isLast, std::format("BoolLiteral: {}", e->value ? "true" : "false"));
    }
    else if (auto e = std::dynamic_pointer_cast<StringLiteralExpr>(expr)) {
        printNode(prefix, isLast, std::format("StringLiteral: \"{}\"", e->value));
    }
    else if (auto e = std::dynamic_pointer_cast<NullptrLiteralExpr>(expr)) {
        printNode(prefix, isLast, "NullptrLiteral");
    }
    else if (auto e = std::dynamic_pointer_cast<VarExpr>(expr)) {
        printNode(prefix, isLast, std::format("VarExpr: {}", e->name));
    }
    else if (auto e = std::dynamic_pointer_cast<ThisExpr>(expr)) {
        printNode(prefix, isLast, "ThisExpr");
    }
    else if (auto e = std::dynamic_pointer_cast<BinaryExpr>(expr)) {
        std::string opStr;
        switch (e->op) {
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
        printNode(prefix, isLast, std::format("BinaryExpr: {}", opStr));
        dumpExpr(e->left, childPrefix, false);
        dumpExpr(e->right, childPrefix, true);
    }
    else if (auto e = std::dynamic_pointer_cast<UnaryExpr>(expr)) {
        std::string opStr = (e->op == UnaryOp::Neg) ? "-" : "!";
        printNode(prefix, isLast, std::format("UnaryExpr: {}", opStr));
        dumpExpr(e->operand, childPrefix, true);
    }
    else if (auto e = std::dynamic_pointer_cast<CallExpr>(expr)) {
        printNode(prefix, isLast, "CallExpr");
        dumpExpr(e->callee, childPrefix, e->arguments.empty());
        for (size_t i = 0; i < e->arguments.size(); ++i) {
            bool last = (i + 1 == e->arguments.size());
            std::cout << childPrefix << (last ? "└── " : "├── ") << "arg[" << i << "]\n";
            dumpExpr(e->arguments[i], childPrefix + (last ? "    " : "│   "), true);
        }
    }
    else if (auto e = std::dynamic_pointer_cast<MemberExpr>(expr)) {
        std::string access = e->isArrow ? "->" : ".";
        printNode(prefix, isLast, std::format("MemberExpr: {}{}", access, e->memberName));
        dumpExpr(e->object, childPrefix, true);
    }
    else if (auto e = std::dynamic_pointer_cast<IndexExpr>(expr)) {
        printNode(prefix, isLast, "IndexExpr");
        dumpExpr(e->object, childPrefix, false);
        dumpExpr(e->index, childPrefix, true);
    }
    else if (auto e = std::dynamic_pointer_cast<NewExpr>(expr)) {
        printNode(prefix, isLast, std::format("NewExpr: {}", e->className));
        for (size_t i = 0; i < e->constructorArgs.size(); ++i) {
            bool last = (i + 1 == e->constructorArgs.size());
            std::cout << childPrefix << (last ? "└── " : "├── ") << "arg[" << i << "]\n";
            dumpExpr(e->constructorArgs[i], childPrefix + (last ? "    " : "│   "), true);
        }
    }
    else if (auto e = std::dynamic_pointer_cast<DynamicCastExpr>(expr)) {
        printNode(prefix, isLast, std::format("DynamicCastExpr: <{}*>", e->targetClassName));
        dumpExpr(e->operand, childPrefix, true);
    }
    else if (auto e = std::dynamic_pointer_cast<DeleteExpr>(expr)) {
        printNode(prefix, isLast, e->isArray ? "DeleteExpr[]" : "DeleteExpr");
        dumpExpr(e->pointerExpr, childPrefix, true);
    }
    else {
        printNode(prefix, isLast, "UnknownExpr");
    }
}

// 语句打印：按动态类型分发
static void dumpStmt(StmtPtr stmt, const std::string& prefix, bool isLast) {
    if (!stmt) {
        printNode(prefix, isLast, "nullptr");
        return;
    }

    std::string branch = isLast ? "└── " : "├── ";
    std::string childPrefix = prefix + (isLast ? "    " : "│   ");

    if (auto s = std::dynamic_pointer_cast<ExprStmt>(stmt)) {
        printNode(prefix, isLast, "ExprStmt");
        dumpExpr(s->expr, childPrefix, true);
    }
    else if (auto s = std::dynamic_pointer_cast<VarDeclStmt>(stmt)) {
        std::string typeStr = s->declaredType ? s->declaredType->toString() : "auto";
        printNode(prefix, isLast, std::format("VarDeclStmt: {} : {}", s->name, typeStr));
        if (s->initializer) {
            dumpExpr(s->initializer, childPrefix, true);
        }
    }
    else if (auto s = std::dynamic_pointer_cast<AssignStmt>(stmt)) {
        printNode(prefix, isLast, "AssignStmt");
        dumpExpr(s->target, childPrefix, false);
        dumpExpr(s->value, childPrefix, true);
    }
    else if (auto s = std::dynamic_pointer_cast<ReturnStmt>(stmt)) {
        printNode(prefix, isLast, "ReturnStmt");
        if (s->value) {
            dumpExpr(s->value, childPrefix, true);
        }
    }
    else if (auto s = std::dynamic_pointer_cast<IfStmt>(stmt)) {
        printNode(prefix, isLast, "IfStmt");
        std::cout << childPrefix << "├── condition\n";
        dumpExpr(s->condition, childPrefix + "│   ", true);
        std::cout << childPrefix << "├── thenBranch\n";
        dumpStmt(s->thenBranch, childPrefix + "│   ", true);
        if (s->elseBranch) {
            std::cout << childPrefix << "└── elseBranch\n";
            dumpStmt(s->elseBranch, childPrefix + "    ", true);
        }
    }
    else if (auto s = std::dynamic_pointer_cast<WhileStmt>(stmt)) {
        printNode(prefix, isLast, "WhileStmt");
        std::cout << childPrefix << "├── condition\n";
        dumpExpr(s->condition, childPrefix + "│   ", true);
        std::cout << childPrefix << "└── body\n";
        dumpStmt(s->body, childPrefix + "    ", true);
    }
    else if (auto s = std::dynamic_pointer_cast<BlockStmt>(stmt)) {
        printNode(prefix, isLast, "BlockStmt");
        for (size_t i = 0; i < s->statements.size(); ++i) {
            bool last = (i + 1 == s->statements.size());
            dumpStmt(s->statements[i], childPrefix, last);
        }
    }
    else if (auto s = std::dynamic_pointer_cast<DeleteStmt>(stmt)) {
        printNode(prefix, isLast, s->isArray ? "DeleteStmt[]" : "DeleteStmt");
        dumpExpr(s->pointerExpr, childPrefix, true);
    }
    else {
        printNode(prefix, isLast, "UnknownStmt");
    }
}

// Block 打印（函数体）
static void dumpBlock(std::shared_ptr<BlockStmt> block, const std::string& prefix) {
    if (!block) return;
    std::cout << prefix << "└── Block\n";
    std::string childPrefix = prefix + "    ";
    for (size_t i = 0; i < block->statements.size(); ++i) {
        bool last = (i + 1 == block->statements.size());
        dumpStmt(block->statements[i], childPrefix, last);
    }
}

// 函数体打印（支持普通函数、构造函数、析构函数）
static void dumpFunctionBody(FuncDeclPtr func, const std::string& prefix) {
    if (!func || !func->body) return;

    std::string childPrefix = prefix + "    ";

    // 构造函数的初始化列表
    if (auto ctor = std::dynamic_pointer_cast<ConstructorDecl>(func)) {
        if (!ctor->initList.empty()) {
            std::cout << prefix << "├── InitList\n";
            for (size_t i = 0; i < ctor->initList.size(); ++i) {
                bool last = (i + 1 == ctor->initList.size());
                std::string branch = last ? "└── " : "├── ";
                std::cout << childPrefix << branch << ctor->initList[i].memberName << "\n";
                std::string argPrefix = childPrefix + (last ? "    " : "│   ");
                for (size_t j = 0; j < ctor->initList[i].arguments.size(); ++j) {
                    bool lastArg = (j + 1 == ctor->initList[i].arguments.size());
                    dumpExpr(ctor->initList[i].arguments[j], argPrefix, lastArg);
                }
            }
        }
    }

    // 函数体
    if (func->body) {
        dumpBlock(func->body, prefix);
    }
}

// 顶层声明打印
void dumpAST(const TranslationUnit& unit) {
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  语法树 (Abstract Syntax Tree)                                   ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n  TranslationUnit\n";

    for (size_t i = 0; i < unit.declarations.size(); ++i) {
        bool isLast = (i + 1 == unit.declarations.size());
        std::string prefix = "  ";
        std::string branch = isLast ? "└── " : "├── ";
        std::string childPrefix = prefix + (isLast ? "    " : "│   ");

        auto& decl = unit.declarations[i];

        if (auto cls = std::dynamic_pointer_cast<ClassDecl>(decl)) {
            std::string bases;
            if (!cls->baseClassNames.empty()) {
                bases = " : ";
                for (size_t j = 0; j < cls->baseClassNames.size(); ++j) {
                    bases += (j ? ", " : "") + cls->baseClassNames[j];
                }
            }
            std::cout << prefix << branch << std::format("ClassDecl: {}{}\n", cls->name, bases);

            // 字段
            for (size_t j = 0; j < cls->fields.size(); ++j) {
                auto& f = cls->fields[j];
                bool lastField = (j + 1 == cls->fields.size()) && cls->methods.empty();
                std::string typeStr = f.type ? f.type->toString() : "?";
                std::cout << childPrefix << (lastField ? "└── " : "├── ")
                          << std::format("Field: {} : {} (+{}, {}B)\n",
                              f.name, typeStr, f.offset, f.size);
            }

            // 方法
            for (size_t j = 0; j < cls->methods.size(); ++j) {
                auto& method = cls->methods[j];
                bool lastMethod = (j + 1 == cls->methods.size());
                std::string flags;
                if (method->isVirtual) flags += " [virtual]";
                if (method->isOverride) flags += " [override]";
                if (auto ctor = std::dynamic_pointer_cast<ConstructorDecl>(method)) {
                    flags += " [ctor]";
                }
                if (auto dtor = std::dynamic_pointer_cast<DestructorDecl>(method)) {
                    flags += " [dtor]";
                }

                std::string retStr = method->returnType ? method->returnType->toString() : "void";
                std::cout << childPrefix << (lastMethod ? "└── " : "├── ")
                          << std::format("Method: {}() → {}{}\n",
                              method->name, retStr, flags);

                if (method->body) {
                    dumpFunctionBody(method, childPrefix + (lastMethod ? "    " : "│   "));
                }
            }
        }
        else if (auto func = std::dynamic_pointer_cast<FunctionDecl>(decl)) {
            std::string retStr = func->returnType ? func->returnType->toString() : "?";
            std::cout << prefix << branch << std::format("FunctionDecl: {}() → {}\n",
                func->name, retStr);

            // 参数
            for (size_t j = 0; j < func->parameters.size(); ++j) {
                bool lastParam = (j + 1 == func->parameters.size()) && !func->body;
                std::string typeStr = func->parameters[j].type ?
                    func->parameters[j].type->toString() : "?";
                std::cout << childPrefix << (lastParam ? "└── " : "├── ")
                          << std::format("Param: {} : {}\n",
                              func->parameters[j].name, typeStr);
            }

            if (func->body) {
                dumpFunctionBody(func, childPrefix);
            }
        }
        else if (auto gvar = std::dynamic_pointer_cast<GlobalVarDecl>(decl)) {
            std::string typeStr = gvar->declaredType ? gvar->declaredType->toString() : "auto";
            std::cout << prefix << branch << std::format("GlobalVarDecl: {} : {}\n",
                gvar->name, typeStr);
            if (gvar->initializer) {
                dumpExpr(gvar->initializer, childPrefix, true);
            }
        }
        else if (auto enm = std::dynamic_pointer_cast<EnumDecl>(decl)) {
            std::cout << prefix << branch << std::format("EnumDecl: {} ({} items)\n",
                enm->name, enm->items.size());
            for (size_t j = 0; j < enm->items.size(); ++j) {
                bool lastItem = (j + 1 == enm->items.size());
                std::cout << childPrefix << (lastItem ? "└── " : "├── ")
                          << std::format("{} = {}\n", enm->items[j].name, enm->items[j].value);
            }
        }
        else if (auto ns = std::dynamic_pointer_cast<NamespaceDecl>(decl)) {
            std::cout << prefix << branch << std::format("NamespaceDecl: {}\n", ns->name);
            // 递归打印命名空间内容
            TranslationUnit subUnit;
            subUnit.declarations = ns->declarations;
            for (size_t j = 0; j < subUnit.declarations.size(); ++j) {
                // 简单递归（可以优化为更完整的实现）
                std::cout << childPrefix << "  ...\n";
                break;
            }
        }
        else if (auto ta = std::dynamic_pointer_cast<TypeAliasDecl>(decl)) {
            std::string typeStr = ta->underlyingType ? ta->underlyingType->toString() : "?";
            std::cout << prefix << branch << std::format("TypeAliasDecl: {} = {}\n",
                ta->aliasName, typeStr);
        }
        else if (auto tmpl = std::dynamic_pointer_cast<TemplateDecl>(decl)) {
            std::string params = "<";
            for (size_t j = 0; j < tmpl->typeParams.size(); ++j) {
                params += (j ? ", " : "") + tmpl->typeParams[j];
            }
            params += ">";
            std::cout << prefix << branch << std::format("TemplateDecl({}){}\n",
                tmpl->isClassTemplate() ? "class" : "function", params);

            if (tmpl->classTemplate) {
                std::cout << childPrefix << "└── ClassTemplate\n";
                // 可以递归打印类模板内容
            }
            if (tmpl->funcTemplate) {
                std::string retStr = tmpl->funcTemplate->returnType ?
                    tmpl->funcTemplate->returnType->toString() : "void";
                std::cout << childPrefix << "└── "
                          << std::format("FunctionTemplate: {}({} params) → {}\n",
                              tmpl->funcTemplate->name,
                              tmpl->funcTemplate->parameters.size(),
                              retStr);
            }
        }
    }

    std::cout << "\n════════════════════════════════════════════════════════════\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// 主函数：编译器驱动
// ─────────────────────────────────────────────────────────────────────────────
// 编译器驱动：解析命令行 → 依次跑阶段 0~6 → 写出汇编。
// 用法：minicc <src.cpp> [-o out.s] [-I dir] [-E] [--dump-tokens] [--dump-ast]
int main(int argc, char* argv[]) {
    // 缺少输入文件 → 打印用法并以非零码退出
    if (argc < 2) {
        std::cerr << "Usage: minicc <source.cpp> [-o output] [-I dir] [-S] [-E] "
                     "[--dump-tokens] [--dump-ast]\n";
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile;
    // ── DumpOptions：诊断可视化标志 ──
    // 正常编译不变，--dump-* 按需触发对应的可视化输出
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

    // 解析命令行参数
    // 单破折号短选项；-o/-I 吃掉紧随其后的值；-I 可多次出现，
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
        // 阶段 1：词法分析 (Lexical Analysis)
        // ═══════════════════════════════════════════════════════════════
        // ── 阶段 0：预处理（[cpp.phase] 翻译阶段 2~4）──
        // 行拼接 → #include 并合 → 条件编译 → 宏展开，
        // 之后词法分析工作在"展开后的纯文本"上（与 clang 的 token 级
        // 预处理不同，教学版是文本级，见 docs/learn/07）。
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

        // Lexer 的输入是预处理后的文本而非原始文件：此时已没有任何 '#' 指令，
        // 宏已就地替换、头文件已并合（[lex.phases]：分词属于阶段 5，在预处理之后）。
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

                // 函数模板（S1）：蓝图存储即可，实例化由调用点实参推导驱动（S2+）
                if (tmpl->isFunctionTemplate()) {
                    std::cout << std::format(
                        "  (function template '{}': blueprint stored; instantiation is "
                        "call-site driven — see S2 deduction / S5 instantiation)\n",
                        tmpl->templateName());
                    continue;
                }

                // ★ 多种实例化演示（仅类模板）★
                if (tmpl->typeParams.size() == 1) {
                    // 实例化 1: T = int
                    std::cout << std::format("\n  ─── Instantiation 1: {}<int> ───\n",
                        tmpl->classTemplate->name);
                    auto instance1 = instantiator.instantiate(
                        tmpl, {Type::makeInt()});
                    std::cout << std::format("  → Instantiated: {}\n", instance1->name);

                    // 实例化 2: T = double
                    std::cout << std::format("\n  ─── Instantiation 2: {}<double> ───\n",
                        tmpl->classTemplate->name);
                    auto instance2 = instantiator.instantiate(
                        tmpl, {Type::makeDouble()});
                    std::cout << std::format("  → Instantiated: {}\n", instance2->name);

                    // 实例化 3: T = int* (指针类型)
                    std::cout << std::format("\n  ─── Instantiation 3: {}<int*> ───\n",
                        tmpl->classTemplate->name);
                    auto instance3 = instantiator.instantiate(
                        tmpl, {Type::makePointer(Type::makeInt())});
                    std::cout << std::format("  → Instantiated: {}\n", instance3->name);

                    // 实例化 4: T = int& (左值引用)
                    std::cout << std::format("\n  ─── Instantiation 4: {}<int&> ───\n",
                        tmpl->classTemplate->name);
                    auto instance4 = instantiator.instantiate(
                        tmpl, {Type::makeLValueReference(Type::makeInt())});
                    std::cout << std::format("  → Instantiated: {}\n", instance4->name);

                    // 实例化 5: T = int&& (右值引用 — 演示万能引用)
                    std::cout << std::format("\n  ─── Instantiation 5: {}<int&&> ───\n",
                        tmpl->classTemplate->name);
                    auto instance5 = instantiator.instantiate(
                        tmpl, {Type::makeRValueReference(Type::makeInt())});
                    std::cout << std::format("  → Instantiated: {}\n", instance5->name);

                    // 实例化 6: T = const int& (常量引用)
                    std::cout << std::format("\n  ─── Instantiation 6: {}<const int&> ───\n",
                        tmpl->classTemplate->name);
                    auto instance6 = instantiator.instantiate(
                        tmpl, {Type::makeLValueReference(Type::makeConst(Type::makeInt()))});
                    std::cout << std::format("  → Instantiated: {}\n", instance6->name);
                }
                else if (tmpl->typeParams.size() == 2) {
                    // 多参数模板实例化: T=int, U=double
                    std::cout << std::format("\n  ─── Instantiation: {}<int, double> ───\n",
                        tmpl->classTemplate->name);
                    auto instance = instantiator.instantiate(
                        tmpl, {Type::makeInt(), Type::makeDouble()});
                    std::cout << std::format("  → Instantiated: {}\n", instance->name);
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

        // ① 汇编：借用系统 as（汇编器只做机械翻译，见 docs/learn/09）
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

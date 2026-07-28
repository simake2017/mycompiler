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
//     → [6] 链接 (Linker, 简化)     → 可执行文件（留给系统链接器）
//
// 用法：
//   ./minicc <source.cpp> [-o output.s] [--dump-tokens] [--dump-ast]
// =============================================================================

#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "template_instantiation.h"
#include "codegen.h"

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
void printPhase(const std::string& phase) {
    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════\n";
    std::cout << std::format("  {}\n", phase);
    std::cout << "════════════════════════════════════════════════════════════\n";
}

// 打印 Token 流（调试用）
void dumpTokens(const std::vector<Token>& tokens) {
    for (auto& tok : tokens) {
        std::cout << std::format("  {:4}:{:<3} {:<15} '{}'\n",
            tok.location.line, tok.location.column,
            tokenTypeName(tok.type), tok.text);
    }
}

// 打印 AST（简化版）
void dumpAST(const TranslationUnit& unit, int indent = 0) {
    auto printIndent = [&](int level) {
        for (int i = 0; i < level; i++) std::cout << "  ";
    };

    for (auto& decl : unit.declarations) {
        if (auto cls = std::dynamic_pointer_cast<ClassDecl>(decl)) {
            printIndent(indent);
            std::cout << std::format("ClassDecl: {}\n", cls->name);
            if (!cls->baseClassName.empty()) {
                printIndent(indent + 1);
                std::cout << std::format("Base: {}\n", cls->baseClassName);
            }
            for (auto& field : cls->fields) {
                printIndent(indent + 1);
                std::cout << std::format("Field: {} : {}\n",
                    field.name, field.type ? field.type->toString() : "?");
            }
            for (auto& method : cls->methods) {
                printIndent(indent + 1);
                std::cout << std::format("Method: {} ({}) {}\n",
                    method->name,
                    method->isVirtual ? "virtual" : "normal",
                    method->returnType ? method->returnType->toString() : "?");
            }
        }
        else if (auto func = std::dynamic_pointer_cast<FunctionDecl>(decl)) {
            printIndent(indent);
            std::cout << std::format("FunctionDecl: {} → {}\n",
                func->name,
                func->returnType ? func->returnType->toString() : "?");
        }
        else if (auto tmpl = std::dynamic_pointer_cast<TemplateDecl>(decl)) {
            printIndent(indent);
            std::cout << std::format("TemplateDecl: <");
            for (size_t i = 0; i < tmpl->typeParams.size(); i++) {
                if (i > 0) std::cout << ", ";
                std::cout << tmpl->typeParams[i];
            }
            std::cout << ">\n";
            if (tmpl->classTemplate) {
                dumpAST({{tmpl->classTemplate}}, indent + 1);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 主函数：编译器驱动
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: minicc <source.cpp> [-o output.s] "
                     "[--dump-tokens] [--dump-ast]\n";
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile;
    bool dumpTokensFlag = false;
    bool dumpAstFlag = false;

    // 解析命令行参数
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            outputFile = argv[++i];
        } else if (arg == "--dump-tokens") {
            dumpTokensFlag = true;
        } else if (arg == "--dump-ast") {
            dumpAstFlag = true;
        }
    }

    // 默认输出文件名
    if (outputFile.empty()) {
        outputFile = std::filesystem::path(inputFile)
            .replace_extension(".s").string();
    }

    try {
        // ═══════════════════════════════════════════════════════════════
        // 阶段 1：词法分析 (Lexical Analysis)
        // ═══════════════════════════════════════════════════════════════
        printPhase("Phase 1: Lexical Analysis (词法分析)");

        std::string source = readFile(inputFile);
        Lexer lexer(source);
        std::vector<Token> tokens = lexer.tokenizeAll();

        std::cout << std::format("  {} tokens generated\n", tokens.size());

        if (dumpTokensFlag) {
            std::cout << "\n  Token dump:\n";
            dumpTokens(tokens);
        }

        // ═══════════════════════════════════════════════════════════════
        // 阶段 2：语法分析 (Syntax Analysis)
        // ═══════════════════════════════════════════════════════════════
        printPhase("Phase 2: Syntax Analysis (语法分析)");

        Parser parser(tokens);
        TranslationUnit unit = parser.parseTranslationUnit();

        std::cout << std::format("  {} top-level declarations parsed\n",
            unit.declarations.size());

        if (dumpAstFlag) {
            std::cout << "\n  AST dump:\n";
            dumpAST(unit);
        }

        // ═══════════════════════════════════════════════════════════════
        // 阶段 3：语义分析 (Semantic Analysis)
        // ═══════════════════════════════════════════════════════════════
        printPhase("Phase 3: Semantic Analysis (语义分析)");
        std::cout << "  Type checking & auto deduction...\n";

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

        // ═══════════════════════════════════════════════════════════════
        // 阶段 4：模板实例化 (Template Instantiation)
        // ═══════════════════════════════════════════════════════════════
        printPhase("Phase 4: Template Instantiation (模板实例化)");

        TemplateInstantiator instantiator;
        auto& templates = semaAnalyzer.getTemplates();

        if (!templates.empty()) {
            std::cout << std::format("  Found {} template blueprint(s)\n\n", templates.size());

            for (auto& tmpl : templates) {
                std::cout << std::format("  Template blueprint: {} <",
                    tmpl->classTemplate->name);
                for (size_t i = 0; i < tmpl->typeParams.size(); i++) {
                    if (i > 0) std::cout << ", ";
                    std::cout << tmpl->typeParams[i];
                }
                std::cout << ">\n";

                // ★ 多种实例化演示 ★
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

        CodeGen codegen;
        std::string assembly = codegen.generate(unit, classTypes, functions);

        // 写入汇编文件
        writeFile(outputFile, assembly);
        std::cout << std::format("  Assembly written to: {}\n", outputFile);

        // ═══════════════════════════════════════════════════════════════
        // 阶段 6：链接（简化：输出提示信息）
        // ═══════════════════════════════════════════════════════════════
        printPhase("Phase 6: Linking (链接)");
        std::cout << "  Assembly file ready for system assembler/linker.\n";
        std::cout << std::format("  To assemble and link:\n");
        std::cout << std::format("    gcc -o output {} -lstdc++\n", outputFile);
        std::cout << std::format("    # 或:\n");
        std::cout << std::format("    as -o output.o {} && ld -o output output.o -lc\n",
            outputFile);

        printPhase("Compilation Complete! 编译完成!");
        return 0;

    } catch (const std::exception& e) {
        std::cerr << std::format("\n[ERROR] {}\n", e.what());
        return 1;
    }
}

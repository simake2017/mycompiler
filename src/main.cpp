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
//
// ─── 命令行参数详解（本文件 main() 解析）─────────────────────────────────
//   minicc <source.cpp> [-o output.s] [-I dir] [-E] [--dump-tokens] [--dump-ast]
//
//   <source.cpp>    输入源文件（必填）
//   -o output.s     输出路径：正常模式写汇编；-E 模式写预处理文本。
//                   缺省时正常模式取 <source>.s，-E 模式直接打印到 stdout。
//   -I dir          头文件搜索目录，可重复（-I include -I third_party），
//                   出现顺序即搜索优先级，原样传给预处理器。
//   -E              只执行阶段 0（预处理）并输出结果，等价 gcc -E，
//                   用于调试宏/include/条件编译问题。
//   --dump-tokens   阶段 1 后打印完整 Token 流（调试）
//   --dump-ast      阶段 2 后打印 AST 顶层声明（调试）
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
//     │ 阶段6 链接（简化）：交给系统工具链
//     ▼
//   gcc -o output output.s -lstdc++
// ─────────────────────────────────────────────────────────────────────────

#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "preprocessor.h"
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

// 打印 AST（简化版）
// 由 --dump-ast 触发：只展开顶层声明——类（含基类/字段/方法）、
// 函数、模板（类模板递归打印其蓝图体，函数模板打印参数个数与返回类型）。
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
        else if (auto gvar = std::dynamic_pointer_cast<GlobalVarDecl>(decl)) {
            printIndent(indent);
            std::cout << std::format("GlobalVarDecl: {} : {}\n",
                gvar->name,
                gvar->declaredType ? gvar->declaredType->toString() : "auto");
        }
        else if (auto enm = std::dynamic_pointer_cast<EnumDecl>(decl)) {
            printIndent(indent);
            std::cout << std::format("EnumDecl: {} ({} items)\n",
                enm->name, enm->items.size());
        }
        else if (auto ns = std::dynamic_pointer_cast<NamespaceDecl>(decl)) {
            printIndent(indent);
            std::cout << std::format("NamespaceDecl: {}\n", ns->name);
            dumpAST({ns->declarations}, indent + 1);
        }
        else if (auto ta = std::dynamic_pointer_cast<TypeAliasDecl>(decl)) {
            printIndent(indent);
            std::cout << std::format("TypeAliasDecl: {} = {}\n",
                ta->aliasName,
                ta->underlyingType ? ta->underlyingType->toString() : "?");
        }
        else if (auto tmpl = std::dynamic_pointer_cast<TemplateDecl>(decl)) {
            printIndent(indent);
            std::cout << std::format("TemplateDecl({}): <",
                tmpl->isClassTemplate() ? "class" : "function");
            for (size_t i = 0; i < tmpl->typeParams.size(); i++) {
                if (i > 0) std::cout << ", ";
                std::cout << tmpl->typeParams[i];
            }
            std::cout << ">\n";
            if (tmpl->classTemplate) {
                dumpAST({{tmpl->classTemplate}}, indent + 1);
            }
            if (tmpl->funcTemplate) {
                printIndent(indent + 1);
                std::cout << std::format("FunctionTemplate: {}({} params) → {}\n",
                    tmpl->funcTemplate->name,
                    tmpl->funcTemplate->parameters.size(),
                    tmpl->funcTemplate->returnType ?
                        tmpl->funcTemplate->returnType->toString() : "void");
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 主函数：编译器驱动
// ─────────────────────────────────────────────────────────────────────────────
// 编译器驱动：解析命令行 → 依次跑阶段 0~6 → 写出汇编。
// 用法：minicc <src.cpp> [-o out.s] [-I dir] [-E] [--dump-tokens] [--dump-ast]
int main(int argc, char* argv[]) {
    // 缺少输入文件 → 打印用法并以非零码退出
    if (argc < 2) {
        std::cerr << "Usage: minicc <source.cpp> [-o output.s] [-I dir] [-E] "
                     "[--dump-tokens] [--dump-ast]\n";
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile;
    bool dumpTokensFlag = false;
    bool dumpAstFlag = false;
    bool emitPreprocessed = false;          // -E：只输出预处理结果（同 gcc -E）
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
        } else if (arg == "--dump-tokens") {
            dumpTokensFlag = true;
        } else if (arg == "--dump-ast") {
            dumpAstFlag = true;
        }
    }

    // 默认输出文件名（-E 模式不需要）
    if (outputFile.empty() && !emitPreprocessed) {
        outputFile = std::filesystem::path(inputFile)
            .replace_extension(".s").string();
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

        if (dumpTokensFlag) {
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

        if (dumpAstFlag) {
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

        // 写入汇编文件
        writeFile(outputFile, assembly);
        std::cout << std::format("  Assembly written to: {}\n", outputFile);

        // ═══════════════════════════════════════════════════════════════
        // 阶段 6：链接（简化：输出提示信息）
        // ═══════════════════════════════════════════════════════════════
        // 教学版到此为止：汇编落盘后把链接委托给系统工具链（后续计划：自动链接）
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
        // 统一错误出口：各阶段（含预处理器的 ppError）抛出的异常都在这里收口
        std::cerr << std::format("\n[ERROR] {}\n", e.what());
        return 1;
    }
}

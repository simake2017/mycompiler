// =============================================================================
// tests/unit/test_preprocessor.cpp —— 预处理器白盒单元测试（GoogleTest）
// =============================================================================
// 目的：直接调用 Preprocessor 的内部处理方法，逐条观察「输入 → 展开结果」，
//       让预处理器每个复杂子算法的行为可讲解、可观测（项目定位：学习编译器理论）。
//
// 运行方式：
//   ① CLion：Reload CMake 后，本文件每个 TEST() 左侧出现绿色三角，点击单跑；
//   ② 命令行：cmake --build build-linux --target unit_tests
//              && ./build-linux/unit_tests            （全部用例）
//              && ./build-linux/unit_tests --gtest_filter='ProcessText.*'
//              ctest --test-dir build-linux --output-on-failure
//
// private 访问：preprocessor.h 中声明了 friend class PreprocessorTestPeer;
//   本文件定义同名桩类，把 processText/expand/substituteParams/evalConstantExpr
//   等 private 方法包装成静态接口。除此之外不依赖任何源码改动。
//
// 套件 → 理论点 → clang 对照（详见 src/preprocessor.cpp 头注）：
//   ProcessText       翻译阶段 2~4 总循环（行拼接/指令/条件栈/展开）
//                     → clang lib/Lex/PPDirectives.cpp
//   Expand            宏重扫描 + 涂蓝 [cpp.rescan]
//                     → clang lib/Lex/PPMacroExpansion.cpp
//   SubstituteParams  函数宏实参替换（词边界、串内不替换）[cpp.subst]
//   EvalConstantExpr  #if 常量表达式 [cpp.cond]
//                     → clang lib/Lex/PPExpressions.cpp
//   LexUtils          文本级"迷你词法"工具
// =============================================================================

#include <gtest/gtest.h>
#include "preprocessor.h"

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

using minicc::MacroDef;
using minicc::Preprocessor;

// ── 测试桩：借助 friend 授权访问 Preprocessor 的 private 成员 ─────────────
// 注意：friend 声明在 namespace minicc 内，故桩类必须同属 minicc 命名空间，
// 否则 friend 授权不生效（这是 C++ 名字查找的经典坑）。
namespace minicc {
class PreprocessorTestPeer {
public:
    static std::string processText(Preprocessor& pp, const std::string& src,
                                   const std::string& fileName = "<test>") {
        return pp.processText(src, fileName);
    }
    static std::string expand(Preprocessor& pp, const std::string& text,
                              std::unordered_set<std::string> hide = {},
                              const std::string& fileName = "<test>", int line = 1) {
        return pp.expand(text, hide, fileName, line);
    }
    static std::string substituteParams(const MacroDef& m,
                                        const std::vector<std::string>& args) {
        Preprocessor pp;   // substituteParams 不依赖宏表状态，借临时实例调用
        return pp.substituteParams(m, args);
    }
    static long evalConstantExpr(Preprocessor& pp, const std::string& expr) {
        return pp.evalConstantExpr(expr, "<test>", 1);
    }
    // 静态 private 词法工具
    static size_t readWord(const std::string& s, size_t pos, std::string& out) {
        return Preprocessor::readWord(s, pos, out);
    }
    static std::string stripComment(const std::string& line) {
        return Preprocessor::stripComment(line);
    }
    static std::string trim(const std::string& s) {
        return Preprocessor::trim(s);
    }
};
} // namespace minicc

using minicc::PreprocessorTestPeer;

namespace {

// 观测助手：把「输入 → 输出」打印出来（gtest 通过与否都可见），
// EXPECT_EQ 失败时 gtest 还会额外打印 Expected/Actual 对照。
void showAndExpectEq(const char* what, const std::string& input,
                     const std::string& actual, const std::string& expected) {
    std::printf("── %s\n   输入: [%s]\n   展开: [%s]\n", what, input.c_str(), actual.c_str());
    EXPECT_EQ(actual, expected);
}

// processText 的输出里指令行被替换成空行（保行号），不便整串比对，用子串断言。
void showAndExpectContains(const char* what, const std::string& input,
                           const std::string& output, const std::string& needle) {
    std::printf("── %s\n   输入: [%s]\n   输出:\n%s", what, input.c_str(), output.c_str());
    EXPECT_NE(output.find(needle), std::string::npos)
        << "输出中应包含: [" << needle << "]";
}

void expectNotContains(const std::string& output, const std::string& needle) {
    EXPECT_EQ(output.find(needle), std::string::npos)
        << "输出中不应包含: [" << needle << "]";
}

} // namespace

// =============================================================================
// 套件一：processText —— 翻译阶段 2~4 总循环（[lex.phases] / [cpp.cond]）
// =============================================================================

// 1. 行拼接（[lex.phases] 翻译阶段 2）：'\'+换行先删除，宏体可跨行书写
TEST(ProcessText, LineSplicing) {
    Preprocessor pp;
    std::string src = "#define N \\\n10 \nint a = N;\n";
    auto out = PreprocessorTestPeer::processText(pp, src);
    showAndExpectContains("行拼接：跨行 #define", src, out, "int a = 10;");
}

// 2. 对象宏 + 链式重扫描（[cpp.rescan]）：SIZE → N*2 → 10*2
TEST(ProcessText, ChainedRescan) {
    Preprocessor pp;
    std::string src = "#define N 10\n#define SIZE N*2\nint a = SIZE;\n";
    auto out = PreprocessorTestPeer::processText(pp, src);
    showAndExpectContains("链式重扫描 SIZE→N*2→10*2", src, out, "int a = 10*2;");
}

// 3. 函数宏（[cpp.replace]）：实参先展开再代入，整体再重扫描
TEST(ProcessText, FunctionLikeMacro) {
    Preprocessor pp;
    std::string src = "#define MAX(a,b) ((a)>(b)?(a):(b))\nint m = MAX(x, y+1);\n";
    auto out = PreprocessorTestPeer::processText(pp, src);
    showAndExpectContains("函数宏 MAX(x, y+1)", src, out,
                          "int m = ((x)>(y+1)?(x):(y+1));");
}

// 4. 自引用涂蓝（[cpp.rescan]）：#define A A+1 不得无限展开
TEST(ProcessText, SelfReferencePaintedBlue) {
    Preprocessor pp;
    std::string src = "#define A A+1\nint a = A;\n";
    auto out = PreprocessorTestPeer::processText(pp, src);
    showAndExpectContains("自引用涂蓝：A 展开一次即停", src, out, "int a = A+1;");
}

// 5. 条件编译（[cpp.cond]）：#if defined / #ifdef / #ifndef / #else / #endif
//    只保留激活分支，未激活分支连宏都不展开
TEST(ProcessText, ConditionalCompilation) {
    Preprocessor pp;
    std::string src =
        "#define DEBUG 1\n"
        "#if defined(DEBUG)\n"
        "int a = 1;\n"
        "#endif\n"
        "#ifdef NOPE\n"
        "int b = 1;\n"
        "#else\n"
        "int b = 0;\n"
        "#endif\n"
        "#ifndef NOPE\n"
        "int c = 1;\n"
        "#endif\n";
    auto out = PreprocessorTestPeer::processText(pp, src);
    std::printf("── 条件编译三分支\n   输入:\n%s\n   输出:\n%s", src.c_str(), out.c_str());
    EXPECT_NE(out.find("int a = 1;"), std::string::npos) << "#if defined(DEBUG) 分支应激活";
    EXPECT_NE(out.find("int b = 0;"), std::string::npos) << "#else 分支应激活";
    EXPECT_NE(out.find("int c = 1;"), std::string::npos) << "#ifndef NOPE 分支应激活";
    expectNotContains(out, "int b = 1;");
}

// 6. #undef（[cpp.undef]）：移除后名字不再展开
TEST(ProcessText, UndefStopsExpansion) {
    Preprocessor pp;
    std::string src = "#define N 10\n#undef N\nint a = N;\n";
    auto out = PreprocessorTestPeer::processText(pp, src);
    showAndExpectContains("#undef 后 N 原样保留", src, out, "int a = N;");
}

// 7. 字符串字面量内不展开：预处理"串内盲区"
TEST(ProcessText, NoExpansionInsideStringLiteral) {
    Preprocessor pp;
    std::string src = "#define N 10\nput(\"N and N\");\n";
    auto out = PreprocessorTestPeer::processText(pp, src);
    showAndExpectContains("串内 N 不展开", src, out, "put(\"N and N\");");
}

// 8. 内建宏（[cpp.predefined]）：__LINE__ 随展开位置变化，__FILE__ 带引号
TEST(ProcessText, BuiltinLineAndFile) {
    Preprocessor pp;
    std::string src = "int a = __LINE__;\nint b = __LINE__;\nconst char* f = __FILE__;\n";
    auto out = PreprocessorTestPeer::processText(pp, src);
    std::printf("── 内建宏 __LINE__/__FILE__\n   输入:\n%s\n   输出:\n%s",
                src.c_str(), out.c_str());
    EXPECT_NE(out.find("int a = 1;"), std::string::npos);
    EXPECT_NE(out.find("int b = 2;"), std::string::npos) << "__LINE__ 应随行号递增";
    EXPECT_NE(out.find("\"<test>\""), std::string::npos) << "__FILE__ 应展开为带引号文件名";
}

// =============================================================================
// 套件二：expand —— 单级观察重扫描与涂蓝集（[cpp.rescan]）
// =============================================================================

// 9. 对象宏单级展开
TEST(Expand, ObjectMacro) {
    Preprocessor pp;
    PreprocessorTestPeer::processText(pp, "#define N 10\n");  // 只登记宏表
    std::string in = "arr[N]";
    auto out = PreprocessorTestPeer::expand(pp, in);
    showAndExpectEq("expand 单级：对象宏", in, out, "arr[10]");
}

// 10. hide 集即"涂蓝"：名字在 hide 中则跳过，这正是自引用不死循环的机制
TEST(Expand, HideSetBlocksExpansion) {
    Preprocessor pp;
    PreprocessorTestPeer::processText(pp, "#define N 10\n");
    std::string in = "N";
    auto open  = PreprocessorTestPeer::expand(pp, in, {});          // hide 为空 → 展开
    auto blocked = PreprocessorTestPeer::expand(pp, in, {"N"});     // N 涂蓝 → 原样
    showAndExpectEq("expand：hide 为空", in, open, "10");
    showAndExpectEq("expand：hide={N} 涂蓝", in, blocked, "N");
}

// =============================================================================
// 套件三：substituteParams —— 函数宏实参替换（词边界 / 串内盲区）
// =============================================================================

// 11. 词边界：独立词 ab 被替换，abc 中的子串 ab 不受影响
TEST(SubstituteParams, WordBoundary) {
    MacroDef m{.name = "M", .params = {"ab"}, .body = "ab abc ab", .functionLike = true};
    auto out = PreprocessorTestPeer::substituteParams(m, {"x"});
    showAndExpectEq("词边界替换 a→x", "body=[ab abc ab], args=[x]", out, "x abc x");
}

// 12. 字符串字面量内的参数名不替换
TEST(SubstituteParams, SkipInsideStringLiteral) {
    MacroDef m{.name = "M", .params = {"ab"}, .body = "\"ab\" ab", .functionLike = true};
    auto out = PreprocessorTestPeer::substituteParams(m, {"x"});
    showAndExpectEq("串内参数名保留", "body=[\"ab\" ab], args=[x]", out, "\"ab\" x");
}

// =============================================================================
// 套件四：evalConstantExpr —— #if 常量表达式（[cpp.cond]）
// =============================================================================

// 13. 逻辑与比较混合：1 && 2 > 1 → 1
TEST(EvalConstantExpr, LogicAndCompare) {
    Preprocessor pp;
    std::string in = "1 && 2 > 1";
    long v = PreprocessorTestPeer::evalConstantExpr(pp, in);
    std::printf("── #if 求值\n   输入: [%s]\n   结果: %ld\n", in.c_str(), v);
    EXPECT_EQ(v, 1);
}

// 14. 算术优先级：* 高于 +
TEST(EvalConstantExpr, ArithmeticPrecedence) {
    Preprocessor pp;
    std::string in = "2*3+4";
    long v = PreprocessorTestPeer::evalConstantExpr(pp, in);
    std::printf("── #if 求值\n   输入: [%s]\n   结果: %ld\n", in.c_str(), v);
    EXPECT_EQ(v, 10);
}

// 15. 逻辑非
TEST(EvalConstantExpr, LogicalNot) {
    Preprocessor pp;
    std::string in = "!0";
    long v = PreprocessorTestPeer::evalConstantExpr(pp, in);
    std::printf("── #if 求值\n   输入: [%s]\n   结果: %ld\n", in.c_str(), v);
    EXPECT_EQ(v, 1);
}

// 16. defined(X)：未定义 → 0；#define 后 → 1
TEST(EvalConstantExpr, DefinedOperator) {
    Preprocessor pp;
    std::string in1 = "defined(NOSUCH)";
    long v1 = PreprocessorTestPeer::evalConstantExpr(pp, in1);
    std::printf("── #if 求值\n   输入: [%s]\n   结果: %ld\n", in1.c_str(), v1);
    EXPECT_EQ(v1, 0);

    PreprocessorTestPeer::processText(pp, "#define YES 1\n");
    std::string in2 = "defined(YES)";
    long v2 = PreprocessorTestPeer::evalConstantExpr(pp, in2);
    std::printf("   输入: [%s]（已 #define YES）\n   结果: %ld\n", in2.c_str(), v2);
    EXPECT_EQ(v2, 1);
}

// =============================================================================
// 套件五：LexUtils —— 文本级"迷你词法"工具
// =============================================================================

// 17. readWord：从 pos 读一个完整标识符，停在非词字符处
TEST(LexUtils, ReadWord) {
    std::string out;
    size_t next = PreprocessorTestPeer::readWord("MAX(a,b)", 0, out);
    std::printf("── readWord(\"MAX(a,b)\", 0)\n   读出: [%s]  停在: %zu\n", out.c_str(), next);
    EXPECT_EQ(out, "MAX");
    EXPECT_EQ(next, 3u) << "应停在 '(' 处（下标 3）";
}

// 18. stripComment：行注释剥掉、块注释换成空格、串内 // 与转义不误伤
TEST(LexUtils, StripComment) {
    std::string in1 = "int a; // 行注释";
    std::string in2 = "/* 块 */ int b;";
    std::string in3 = "x = \"// in str\"; // 真注释";
    auto out1 = PreprocessorTestPeer::stripComment(in1);
    auto out2 = PreprocessorTestPeer::stripComment(in2);
    auto out3 = PreprocessorTestPeer::stripComment(in3);
    showAndExpectEq("行注释", in1, out1, "int a; ");
    // 块注释整体替换为一个空格，原文 "*/" 后的空格保留 → 共两个空格
    showAndExpectEq("块注释→空格", in2, out2, "  int b;");
    showAndExpectEq("串内 // 保留", in3, out3, "x = \"// in str\"; ");
}

// 19. trim：去首尾空白
TEST(LexUtils, Trim) {
    std::string in = "  x  ";
    auto out = PreprocessorTestPeer::trim(in);
    showAndExpectEq("trim", in, out, "x");
}

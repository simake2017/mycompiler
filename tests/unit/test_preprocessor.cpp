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
#include <iostream>
#include <sstream>
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

// 给一行 [pp] 流水加中文注释，方便看 trace 时理解每步在干什么。
// 顺序：缩进剥离 → 关键词匹配 → 拼人话。匹配不上原样返回。
std::string explainPpLine(const std::string& line) {
    // 形如：  [pp] <test>:2   ├─ d=1 enter  text=[x]  hide={}
    //       或  [pp] <test>:2 │  func-macro MAX(x, y+1)  args(raw)=[x | y+1]
    // 先把开头 "  [pp] <file>:<line>  " 去掉，留主体
    std::string body = line;
    auto p1 = body.find("[pp]");
    if (p1 != std::string::npos) body = body.substr(p1 + 4);
    auto p2 = body.find_first_not_of(" \t");
    if (p2 != std::string::npos) body = body.substr(p2);

    // 跳过 "<file>:<line>" 段
    auto colon = body.find(':');
    if (colon != std::string::npos) {
        // 文件名可能带路径（不包含 :），取最后一个空格后下一个 token 的冒号
        // 简化：找第一个空格，认为空格前是 "file:line"
        auto sp = body.find(' ');
        if (sp != std::string::npos) body = body.substr(sp + 1);
    }

    // 拆缩进符 ┌ ├ │ └ ┴ 与层级号 d=N
    std::string hint;
    if (body.find("enter") != std::string::npos && body.find("d=") != std::string::npos) {
        hint = "→ 进入展开层（递归深度见 d），hide 是涂蓝集";
    } else if (body.find("return body") != std::string::npos) {
        hint = "← 本层展开完成，body 即为替换后的整段文本";
    } else if (body.find("str-literal pass-through") != std::string::npos) {
        hint = "字符串字面量原样输出（串内不展开）";
    } else if (body.find("builtin __LINE__") != std::string::npos) {
        hint = "__LINE__ 就地替换为当前行号";
    } else if (body.find("builtin __FILE__") != std::string::npos) {
        hint = "__FILE__ 就地替换为当前文件名（带引号）";
    } else if (body.find("not a macro") != std::string::npos) {
        hint = "普通标识符（非宏），原样保留";
    } else if (body.find("in hide-set") != std::string::npos) {
        hint = "宏名在涂蓝集里：阻止再次展开，避免自引用死循环 [cpp.rescan]";
    } else if (body.find("object-macro") != std::string::npos
            && body.find("→ body=") != std::string::npos) {
        hint = "对象宏：先取宏体递归展开（重扫描）";
    } else if (body.find("object-macro") != std::string::npos
            && body.find("resolved") != std::string::npos) {
        hint = "对象宏：递归回来，得到最终替换串";
    } else if (body.find("is function-like but no '('") != std::string::npos) {
        hint = "形似函数宏但缺 '('，按普通标识符原样输出 [cpp.replace]";
    } else if (body.find("func-macro") != std::string::npos
            && body.find("args(raw)") != std::string::npos) {
        hint = "函数宏：按括号配平 + 顶层逗号切分，得到实参列表（未展开）";
    } else if (body.find("args(expanded)") != std::string::npos) {
        hint = "实参先展开 [cpp.subst]，注意是先于参数名替换";
    } else if (body.find("body before=") != std::string::npos
            && body.find("→ after=") != std::string::npos) {
        hint = "substituteParams：按整词边界把参数名替换为实参文本";
    } else if (body.find("final-substitution") != std::string::npos) {
        hint = "整体重扫描结束 → 落盘到 out";
    } else {
        hint = "";
    }
    return hint;
}

// 捕获 Preprocessor::processText/expand 通过 std::cout 写出的 [pp] 流水，
// 并把它和"展开后输出"一起喂给 dumpWithExplanation 打可视化 box。
// 实现：把 stdout 重定向到临时 stringstream，调用完恢复。
class StdoutCapture {
    std::streambuf* old_;
    std::stringstream buf_;
public:
    StdoutCapture() : old_(nullptr), buf_() { old_ = std::cout.rdbuf(buf_.rdbuf()); }
    ~StdoutCapture() { std::cout.rdbuf(old_); }
    std::string str() const { return buf_.str(); }
};

// 把 [pp] 开头的日志行逐行带分隔线打印，行尾追加中文解释。
// 非 [pp] 行（输入/输出 box）原样展示。
//   trace : 捕获到的 stdout 内容（含 [pp] 行）
//   input : 原始输入（用于显示 [输入] 块）
//   output: 展开后文本（用于 [最终输出] 块 + 断言）
//
// 分块策略：每遇到 [src-line] 标记就开始一个新的"源行块"，用 === 横线隔开。
// 这样阅读时：每个宏定义 / 每条普通代码 各自独立成段，块内只跟它相关的 trace。
void dumpWithExplanation(const char* what, const std::string& input,
                         const std::string& output, const std::string& trace) {
    std::printf("\n=================================================================\n");
    std::printf("── %s\n", what);
    std::printf("=================================================================\n");
    std::printf("[输入]\n%s\n", input.c_str());
    std::printf("-----------------------------------------------------------------\n");
    std::printf("[展开 trace（按源行分块，每行 [pp] 后附中文解释）]\n");
    std::printf("=================================================================\n");
    size_t pos = 0;
    int blockIdx = 0;
    while (pos < trace.size()) {
        auto eol = trace.find('\n', pos);
        std::string line = trace.substr(pos, (eol == std::string::npos ? std::string::npos : eol - pos));
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // 块锚点：[src-line] 出现时另起一块
        if (line.find("[src-line]") != std::string::npos) { // 如果存在，那么执行 if
            if (blockIdx > 0) std::printf("  ─────────────────────────────────────────────────────────────────\n");
            // 提取 <SRC>...</SRC> 之间的源文本
            auto ps = line.find("<SRC>");
            auto pe = line.find("</SRC>");
            std::string srcText = (ps != std::string::npos && pe != std::string::npos && pe > ps)
                ? line.substr(ps + 5, pe - ps - 5) : std::string("");
            std::printf("  [源行 #%d]  %s\n", ++blockIdx, srcText.c_str());
            std::printf("  ─────────────────────────────────────────────────────────────────\n");
        } else if (line.find("[pp]") != std::string::npos) {
            std::string hint = explainPpLine(line);
            if (!hint.empty()) std::printf("      %s   # %s\n", line.c_str(), hint.c_str());
            else               std::printf("      %s\n", line.c_str());
        } else if (!line.empty()) {
            std::printf("      %s\n", line.c_str());
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    std::printf("=================================================================\n");
    std::printf("[最终输出]\n%s\n", output.c_str());
    std::printf("=================================================================\n\n");
}

// 兼容旧签名：调用 dumpWithExplanation 打带分块、带解释的可视化 box，
// 同时保留子串断言。多数 processText 用例都用这个入口。
// 注意：调用前请用 StdoutCapture 捕获 processText 期间的 [pp] 日志。
void showAndExpectContains(const char* what, const std::string& input,
                           const std::string& output, const std::string& needle,
                           const std::string& trace = "") {
    dumpWithExplanation(what, input, output, trace);
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
    // std::string src = "#define N \\\n10 * \\\n2 \nint a = N;\n";
    std::string trace, out;
    { StdoutCapture cap; out = PreprocessorTestPeer::processText(pp, src); trace = cap.str(); }
    showAndExpectContains("行拼接：跨行 #define", src, out, "int a = 10;", trace);
}

// 2. 对象宏 + 链式重扫描（[cpp.rescan]）：SIZE → N*2 → 10*2
TEST(ProcessText, ChainedRescan) {
    Preprocessor pp;
    std::string src = "#define N 10\n#define SIZE N*2\nint a = SIZE;\n";
    std::string trace, out;
    { StdoutCapture cap; out = PreprocessorTestPeer::processText(pp, src); trace = cap.str(); }
    showAndExpectContains("链式重扫描 SIZE→N*2→10*2", src, out, "int a = 10*2;", trace);
}

// 3. 函数宏（[cpp.replace]）：实参先展开再代入，整体再重扫描
TEST(ProcessText, FunctionLikeMacro) {
    Preprocessor pp;
    std::string src = "#define MAX(a,b) ((a)>(b)?(a):(b))\nint m = MAX(x, y+1);\n";
    std::string trace, out;
    { StdoutCapture cap; out = PreprocessorTestPeer::processText(pp, src); trace = cap.str(); }
    showAndExpectContains("函数宏 MAX(x, y+1)", src, out,
                          "int m = ((x)>(y+1)?(x):(y+1));", trace);
}

// 4. 自引用涂蓝（[cpp.rescan]）：#define A A+1 不得无限展开
TEST(ProcessText, SelfReferencePaintedBlue) {
    Preprocessor pp;
    std::string src = "#define A A+1\nint a = A;\n";
    std::string trace, out;
    { StdoutCapture cap; out = PreprocessorTestPeer::processText(pp, src); trace = cap.str(); }
    showAndExpectContains("自引用涂蓝：A 展开一次即停", src, out, "int a = A+1;", trace);
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
    std::string trace, out;
    { StdoutCapture cap; out = PreprocessorTestPeer::processText(pp, src); trace = cap.str(); }
    dumpWithExplanation("条件编译三分支", src, out, trace);
    EXPECT_NE(out.find("int a = 1;"), std::string::npos) << "#if defined(DEBUG) 分支应激活";
    EXPECT_NE(out.find("int b = 0;"), std::string::npos) << "#else 分支应激活";
    EXPECT_NE(out.find("int c = 1;"), std::string::npos) << "#ifndef NOPE 分支应激活";
    expectNotContains(out, "int b = 1;");
}

// 6. #undef（[cpp.undef]）：移除后名字不再展开
TEST(ProcessText, UndefStopsExpansion) {
    Preprocessor pp;
    std::string src = "#define N 10\n#undef N\nint a = N;\n";
    std::string trace, out;
    { StdoutCapture cap; out = PreprocessorTestPeer::processText(pp, src); trace = cap.str(); }
    showAndExpectContains("#undef 后 N 原样保留", src, out, "int a = N;", trace);
}

// 7. 字符串字面量内不展开：预处理"串内盲区"
TEST(ProcessText, NoExpansionInsideStringLiteral) {
    Preprocessor pp;
    std::string src = "#define N 10\nput(\"N and N\");\n";
    std::string trace, out;
    { StdoutCapture cap; out = PreprocessorTestPeer::processText(pp, src); trace = cap.str(); }
    showAndExpectContains("串内 N 不展开", src, out, "put(\"N and N\");", trace);
}

// 8. 内建宏（[cpp.predefined]）：__LINE__ 随展开位置变化，__FILE__ 带引号
TEST(ProcessText, BuiltinLineAndFile) {
    Preprocessor pp;
    // std::string src = "int a =  __LINE__;\nint b = __LINE__;\nconst char* f = __FILE__;\n";
    std::string src = "int a = \\\n __LINE__;\nint b = __LINE__;\nconst char* f = __FILE__;\n";
    auto out = PreprocessorTestPeer::processText(pp, src);
    std::printf("── 内建宏 __LINE__/__FILE__\n   输入:\n%s\n   输出:\n%s",
                src.c_str(), out.c_str());
    EXPECT_NE(out.find("int a =  1;"), std::string::npos);
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
    auto out = PreprocessorTestPeer::substituteParams(m, {"x"}); // 后面的{x}实际就是变量完全展开的结果
    showAndExpectEq("词边界替换 a→x", "body=[ab abc ab], args=[x]", out, "x abc x");
}

// 12. 字符串字面量内的参数名不替换
TEST(SubstituteParams, SkipInsideStringLiteral) {
    MacroDef m{.name = "M", .params = {"ab"}, .body = "\"ab\" ab", .functionLike = true};
    auto out = PreprocessorTestPeer::substituteParams(m, {"x"}); //字符串字面量不展开
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

// =============================================================================
// tests/unit/test_codegen_frame.cpp —— 栈帧大小 ≥ 最深局部偏移（白盒不变量）
// =============================================================================
// 考察理论点：栈帧布局（System V AMD64 ABI / 编译器后端的"帧描述"）
//   · 序言 `subq $N, %rsp` 里的 N 必须【覆盖】函数体内所有 %rbp 负偏移访问，
//     否则那些槽位落在 rsp 之下 —— 而 rsp 之下正是：
//       ① 表达式求值的 `pushq %rax`（左操作数暂存）
//       ② `callq` 压入的返回地址
//     两者都写在 rsp-8，于是"自己的暂存/返回地址"踩掉"自己的变量"。
//   · 根因形态：帧大小与局部布局【各算一份】就会对不上。
//     局部偏移从 -8 起步、每个 spill 形参再占 8 字节，而 estimateBlockSize
//     只数局部变量 ⇒ 帧恒定浅 8~56 字节。
//   · 对照 clang：两者出自同一份分配器
//     （llvm/lib/Target/X86/X86FrameLowering.cpp::determineFrameLayout 按
//      MFI.getStackSize() / getMaxCallFrameSize() 统一算出帧大小；
//      局部对象偏移由 PEI 的 assignFrameObjects 从同一指针推进），
//     结构上不可能出现"两处各算一份"。
//
// 断言的是【不变量】而不是症状：
//   对每个函数：max{ |offset| : `-X(%rbp)` 出现在该函数体内 } ≤ `subq $N` 的 N
// 这样即使将来换了分配策略（复用槽位、对象按真实尺寸对齐……），
// 只要不再让局部落到 rsp 之下，测试就仍然有意义。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='CodegenFrame.*'
// =============================================================================

#include <gtest/gtest.h>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "obs_helpers.h"

#include <algorithm>
#include <regex>
#include <string>
#include <vector>

using namespace minicc;

namespace {

// 源码 → 完整汇编（语义日志全部吞掉）
std::string compileQuiet(const std::string& src) {
    StdoutCapture cap;
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    TranslationUnit unit = parser.parseTranslationUnit();
    SemanticAnalyzer sema;
    sema.analyze(unit);
    CodeGen cg;
    return cg.generate(unit, sema.getClassTypes(), sema.getFunctions());
}

// 一个函数的汇编片段：逐行扫，从 `<label>:` 那一行起，到下一个 `.globl` 止。
// 注意不能只按子串找结束标记：符号导出行缩进是 8 空格、标签行是 4 空格，
// 混用会一路吞到文件尾（把别的函数的最深偏移算进来）。
std::string funcBody(const std::string& asmCode, const std::string& label) {
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : asmCode) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) lines.push_back(cur);
    }

    auto strip = [](const std::string& s) {
        auto b = s.find_first_not_of(" \t");
        return b == std::string::npos ? std::string() : s.substr(b);
    };

    size_t begin = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        // 标签行后面跟着注释（`    main:            # 函数入口标签`），
        // 故按前缀匹配而不是全等
        if (strip(lines[i]).rfind(label + ":", 0) == 0) { begin = i; break; }
    }
    if (begin == lines.size()) return "";

    std::string body;
    for (size_t i = begin; i < lines.size(); ++i) {
        if (i > begin && strip(lines[i]).rfind(".globl", 0) == 0) break;
        body += lines[i] + "\n";
    }
    return body;
}

// 帧大小 N（序言里的 `subq $N, %rsp`）
int frameSizeOf(const std::string& body) {
    std::smatch m;
    std::regex re(R"(subq \$(\d+), %rsp)");
    if (std::regex_search(body, m, re)) return std::stoi(m[1]);
    return -1;
}

// 该函数体里最深（绝对值最大）的 %rbp 负偏移
int deepestLocalOffset(const std::string& body) {
    std::regex re(R"(-(\d+)\(%rbp\))");
    int deepest = 0;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
         it != std::sregex_iterator(); ++it) {
        deepest = std::max(deepest, std::stoi((*it)[1].str()));
    }
    return deepest;
}

// ── 核心断言：帧 ≥ 最深偏移（每个函数各查一遍）──────────────────────────
void expectFrameCoversLocals(const std::string& asmCode,
                             const std::vector<std::string>& labels) {
    for (const auto& label : labels) {
        std::string body = funcBody(asmCode, label);
        ASSERT_FALSE(body.empty()) << "找不到函数 " << label;
        int frame = frameSizeOf(body);
        int deepest = deepestLocalOffset(body);
        ASSERT_GE(frame, 0) << label << " 没有 subq $N, %rsp 序言";
        EXPECT_GE(frame, deepest)
            << "函数 " << label << "：帧 " << frame << " 字节，"
            << "却访问到 -" << deepest << "(%rbp) —— 局部槽位落在 rsp 之下，"
            << "会被 pushq 暂存与 callq 返回地址踩掉";
    }
}

} // namespace

// =============================================================================
// 回归：曾经的最小复现 —— 8 个 int 局部，返回 a + h
//   修复前：帧 64，h 在 -72 ⇒ 求值 `a + h` 的 pushq 暂存正好写进 h 的槽位，
//           读回的 h 其实是刚压栈的 a ⇒ 返回 2（clang 9）
// =============================================================================
TEST(CodegenFrame, T1_EightIntLocals) {
    std::string asmCode = compileQuiet(R"(
int main() {
    int a = 1; int b = 2; int c = 3; int d = 4;
    int e = 5; int f = 6; int g = 7; int h = 8;
    return a + h;
}
)");
    expectFrameCoversLocals(asmCode, {"main"});
    // 精准钉住症状位置：h 是第 8 个局部 ⇒ 偏移 -72，帧至少要 72
    std::string body = funcBody(asmCode, "main");
    EXPECT_GE(frameSizeOf(body), 72);
}

// =============================================================================
// 回归：函数调用存在时更凶险 —— callq 的返回地址同样落在 rsp-8
//   修复前：`add(g, h)` 里 h（-72）被 add 调用压入的返回地址覆盖 ⇒ 14（clang 15）
// =============================================================================
TEST(CodegenFrame, T2_CallClobbersBelowRsp) {
    std::string asmCode = compileQuiet(R"(
int add(int x, int y) { return x + y; }
int main() {
    int a = 1; int b = 2; int c = 3; int d = 4;
    int e = 5; int f = 6; int g = 7; int h = 8;
    return add(g, h);
}
)");
    // 有 2 个形参的 add：帧里除了形参 spill 还要放返回表达式求值的暂存
    expectFrameCoversLocals(asmCode, {"add", "main"});
    std::string body = funcBody(asmCode, "add");
    // add 的 x/y 两个形参各自 spill 到 -8/-16，帧至少覆盖到 -16
    EXPECT_GE(frameSizeOf(body), 16);
}

// =============================================================================
// 回归：栈上类对象按真实尺寸占位（不是固定 8 字节），且帧要数进去
//   Box<int> 布局 16 字节（int v @0 + int* p @8），它一个人占 2 个槽
// =============================================================================
TEST(CodegenFrame, T3_ClassObjectTakesRealSize) {
    std::string asmCode = compileQuiet(R"(
template <typename T>
struct Box { T v; T* p; };
int main() {
    Box<int> bi;
    bi.v = 3;
    int a = 1; int b = 2; int c = 3; int d = 4;
    int e = 5; int f = 6;
    return bi.v + f;
}
)");
    expectFrameCoversLocals(asmCode, {"main"});
}

// =============================================================================
// 成员函数：this 占 rdi 且先 spill 一个槽，参数寄存器整体右移一位
//   （Itanium ABI：this 是隐式第 0 参数）—— 帧同样要覆盖这些 spit 槽
// =============================================================================
TEST(CodegenFrame, T4_MemberFunctionThisSlot) {
    std::string asmCode = compileQuiet(R"(
struct Vec {
    int a;
    int b;
    int sum(int k) {
        int x = 1; int y = 2; int z = 3;
        int u = 4; int v = 5; int w = 6;
        return a + b + k + x + y + z + u + v + w;
    }
};
int main() {
    Vec s;
    s.a = 1;
    s.b = 2;
    return s.sum(3);
}
)");
    // 方法内部有 6 个局部 + this + 形参 k，帧必须盖住
    std::string body = funcBody(asmCode, "Vec_sum_1");
    ASSERT_FALSE(body.empty());
    EXPECT_GE(frameSizeOf(body), deepestLocalOffset(body));
}

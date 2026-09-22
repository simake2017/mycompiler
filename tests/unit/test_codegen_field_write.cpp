// =============================================================================
// tests/unit/test_codegen_field_write.cpp —— 字段写入必须按【字段宽度】选指令
// =============================================================================
// 考察理论点：标量存储宽度（类型宽度 → 指令选择；[expr.ass] 左值写入的落地）
//   · N 字节字段要用对应宽度的指令：1B movb │ ≤4B movl │ 8B movq
//   · 一律 movl ⇒ 8B 字段（指针 / long）高 32 位被砍 ⇒ 指针悬空、值错
//   · 对照 clang：CodeGenFunction::EmitStoreOfScalar 按 TI.Width 选指令
//     （clang/lib/CodeGen/CGExpr.cpp）
//
// 【为什么三条路径必须分别钉】它们在 codegen 里是三段互不相干的代码：
//   ① 成员表达式  q.p = v;          → emitAssign 的 NodeKind::Member 分支
//   ② 初始化列表  : p(q)            → emitConstructor 的字段初始化
//   ③ 裸字段名    void f(){ p=q; }  → emitAssign 的 NodeKind::Var 分支
//   ★ BUGS.md B3 正是 ③ 写死了 movl；①② 早已按宽度分派 —— 不一起钉就会再漏。
//
// 断言的是【不变量】而非症状文案：
//   每条字段写入指令的宽度字母 ⟺ 该偏移上字段声明的宽度。
// 这样即便将来改文案、换寄存器、挪偏移，只要宽度还对，测试就仍然有意义。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='CodegenFieldWrite.*'
// =============================================================================

#include <gtest/gtest.h>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "obs_helpers.h"

#include <map>
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

// 布局固定（bool 0 / int 4 / int* 8 / int 16），三条写路径各覆盖一遍：
//   ① main 里 h.p = &v; / h.i = 3;
//   ② 初始化列表 b(true), i(1), p(q), tag(7)
//   ③ setp/seti/setb —— 三个裸字段名赋值
const char* kSource = R"(
struct Holder {
    bool b;
    int  i;
    int* p;
    int  tag;
    Holder(int* q) : b(true), i(1), p(q), tag(7) {}
    void setp(int* q) { p = q; }
    void seti(int x)   { i = x; }
    void setb(bool x)  { b = x; }
};
int main() {
    int v = 7;
    Holder h(&v);
    h.p = &v;
    h.i = 3;
    return 0;
}
)";

// 偏移 ⇒ 该字段应有的宽度字母（与 kSource 的布局一致）
const std::map<int, char>& expectedWidths() {
    static const std::map<int, char> m = {
        {0, 'b'},   // bool  b
        {4, 'l'},   // int   i
        {8, 'q'},   // int*  p   ← B3 的受害字段：曾被 movl 砍掉高 32 位
        {16, 'l'},  // int   tag
    };
    return m;
}

// 一条字段写入：(宽度字母, 偏移)
struct FieldWrite {
    char width;
    int  offset;
    std::string raw;   // 原始指令，失败时打印用
};

// 抓 `movX %e/rax, [+-]N(%rcx)`。三条路径都用 %rcx 承载对象地址；
// 成员表达式路径的偏移带 '+'（`+8(%rcx)`），故正则里容忍一个可选正号。
std::vector<FieldWrite> fieldWrites(const std::string& asmCode) {
    static const std::regex re(R"(mov([bwlq])\s+%[er]ax,\s*\+?(-?\d+)\(%rcx\))");
    std::vector<FieldWrite> out;
    for (auto it = std::sregex_iterator(asmCode.begin(), asmCode.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.push_back({(*it)[1].str()[0], std::stoi((*it)[2].str()), it->str()});
    }
    return out;
}

} // namespace

// ── 1. 核心不变量：写入宽度 == 字段宽度 ────────────────────────────────────
TEST(CodegenFieldWrite, EveryWriteMatchesFieldWidth) {
    std::string asmCode = compileQuiet(kSource);
    auto writes = fieldWrites(asmCode);

    ASSERT_FALSE(writes.empty()) << "一条字段写入都没抓到 —— 正则失效或 codegen 改了发射形态";

    std::printf("── 抓到 %zu 条字段写入 ──\n", writes.size());
    for (const auto& w : writes) {
        auto it = expectedWidths().find(w.offset);
        ASSERT_NE(it, expectedWidths().end())
            << "偏移 " << w.offset << " 不在预期布局里（指令：" << w.raw << "）";
        EXPECT_EQ(w.width, it->second)
            << "偏移 " << w.offset << " 的字段应为 " << it->second << " 宽度指令，"
            << "实际用了 mov" << w.width << " —— 指令：" << w.raw;
        std::printf("   偏移 %-3d mov%c  %s\n", w.offset, w.width, w.raw.c_str());
    }
}

// ── 2. 三条写路径都必须被覆盖到 ────────────────────────────────────────────
// 只钉住宽度还不够：若某条路径整体消失（例如 setp 没被发射），
// 上面那条测试会因为"少了几条写入"而静默变弱。这里用各路径的文案特征确认都到场。
TEST(CodegenFieldWrite, AllThreePathsAreCovered) {
    std::string asmCode = compileQuiet(kSource);

    // ① 成员表达式：文案「写入字段 .x（偏移 +N，...）」
    EXPECT_NE(asmCode.find("写入字段 .p"), std::string::npos)
        << "路径①（成员表达式 q.p = v）未覆盖";
    // ② 初始化列表：文案「初始化字段 x（偏移 N，NB）」
    EXPECT_NE(asmCode.find("初始化字段 p"), std::string::npos)
        << "路径②（初始化列表 : p(q)）未覆盖";
    // ③ 裸字段名：文案「.x = ...（偏移 N，NB 字节写入）」
    EXPECT_NE(asmCode.find(".p = ..."), std::string::npos)
        << "路径③（裸字段名 p = q）未覆盖";
}

// ── 3. B3 的症状钉子：裸字段名路径必须按 8B 分派 ───────────────────────────
// 路径③ 曾经硬编码 movl，对 int* 字段写 4 字节。这里直接钉住那条指令的产物：
// 偏移 8 出现「8 字节写入」即说明宽度分派生效（4 字节写法不会打印这个后缀）。
TEST(CodegenFieldWrite, BareFieldNameWriteHonors8ByteWidth) {
    std::string asmCode = compileQuiet(kSource);

    EXPECT_NE(asmCode.find("偏移 8，8 字节写入"), std::string::npos)
        << "裸字段名路径没有按 8B 分派 —— BUGS.md B3 复发（指针高 32 位会被砍）";
    // 反向保护：裸字段名路径不得出现对 8 字节字段的 4 字节写入
    EXPECT_EQ(asmCode.find(".p = ...（偏移 8，4 字节写入）"), std::string::npos)
        << "裸字段名路径仍在对 8 字节字段做 4 字节写入";
}

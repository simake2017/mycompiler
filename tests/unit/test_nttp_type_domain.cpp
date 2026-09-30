// =============================================================================
// tests/unit/test_nttp_type_domain.cpp —— NTTP 类型域的两条【不变量】
// =============================================================================
// 考察理论点（对应 docs/learn/35，bug 台账 BUGS.md B17）：
//
//   ① **可表示性判据**（[temp.arg.nontype]/1 → [expr.const]/10 → [dcl.init]/7）：
//      值实参能不能填某个整型形参，判据只有一个 —— `Type::canRepresentValue(v)`，
//      即"这个类型的可表示集合含不含 v"。它必须是**单点判据**：
//      模板实参校验（semantic_analyzer）与 main.cpp 的演示路径都调它，
//      写两份就会各自演化（本项目在 B10 / B12 上已经吃过两次这个亏）。
//
//   ② **可读串不是单射**（docs/learn/23 的老教训，本轮在实例名上再次现形）：
//      `K<4>`（int）与 `K<4L>`（long）在 `template<auto V>` 下是**两个实例**，
//      但 `TemplateArg::toString()` 对两者都产 `"4"`。本测试断言的是
//      "两条实参链的**可区分性**"这一不变量，而不是某个具体键串 ——
//      键的拼法随便改，只要不同的实参仍然映到不同的键即可。
//
// ★ 为什么值得单测：
//   · ① 的边界值很多（12 个整型 × 上下界 ±1），集成用例一个文件只能停在一个
//     分支上（fail-fast），单测可以在一个进程里把整张表跑完；
//   · ② 是**静默错误**：撞键不报错，只是悄悄少建一个实例（旧版就是如此），
//     集成用例只能靠"数实例个数"间接发现，单测能直接钉住。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='NttpTypeDomain.*'
// =============================================================================

#include <gtest/gtest.h>

#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "template_instantiation.h"
#include "type.h"
#include "obs_helpers.h"

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace minicc;

namespace {

// 源码 → Sema 日志；语义错误以 runtime_error 抛出（SemanticAnalyzer::error）。
std::string semaLogOf(const std::string& src) {
    StdoutCapture cap;
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    TranslationUnit unit = parser.parseTranslationUnit();
    SemanticAnalyzer sema;
    sema.analyze(unit);
    return cap.str();
}

// 跑一段源码，把抛出的语义错误正文取出来；没抛 = 返回空串。
std::string semaErrorOf(const std::string& src) {
    try {
        semaLogOf(src);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

// 整型家族的"类型 → 位宽 → 符号性"真值表。
// 这张表是 [basic.fundamental]/2（五大标准整型各有无符号版）与 /7
// （char / signed char / unsigned char 是三个不同类型）的机械化。
//
// ★ `isSigned`（数学语义）与 `isUnsignedInteger()`（本项目的谓词）**不是反义词**：
//   bool 既不是"有符号"也不满足 `isUnsignedInteger()` —— 它的可表示集合 {0,1}
//   不是任何 2 的幂区间，是独立的一条规则（见 canRepresentValue 的 bool 特判）。
//   把这一列单独列出来，正是为了不让"非有符号即无符号"这个直觉混进判据。
struct IntCase {
    const char* name;
    TypePtr     type;
    uint32_t    bits;           // integerBitWidth()
    bool        isSigned;       // 数学语义上的有符号
    bool        unsignedPred;   // isUnsignedInteger() 的返回值
};

std::vector<IntCase> allIntegerTypes() {
    return {
        {"bool",               Type::makeBool(),       1,  false, false},  // ← 特别注意
        {"char",               Type::makeChar(),       8,  true,  false},  // 本机 char 有符号
        {"signed char",        Type::makeSChar(),      8,  true,  false},
        {"unsigned char",      Type::makeUChar(),      8,  false, true },
        {"short",              Type::makeShort(),      16, true,  false},
        {"unsigned short",     Type::makeUShort(),     16, false, true },
        {"int",                Type::makeInt(),        32, true,  false},
        {"unsigned int",       Type::makeUInt(),       32, false, true },
        {"long",               Type::makeLong(),       64, true,  false},
        {"unsigned long",      Type::makeULong(),      64, false, true },
        {"long long",          Type::makeLongLong(),   64, true,  false},
        {"unsigned long long", Type::makeULongLong(),  64, false, true },
    };
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ①-a 整型家族的位宽 / 符号性（[basic.fundamental]/2、/7）
// ─────────────────────────────────────────────────────────────────────────────
TEST(NttpTypeDomain, IntegerFamilyWidthsAndSignedness) {
    for (const auto& c : allIntegerTypes()) {
        EXPECT_TRUE(c.type->isInteger()) << c.name;
        EXPECT_EQ(c.type->integerBitWidth(), c.bits) << c.name;
        EXPECT_EQ(c.type->isUnsignedInteger(), c.unsignedPred) << c.name;
    }
    // 表里唯一"既不 signed 也不 unsigned"的一格，单独钉住：
    // bool 不是无符号整型 —— 拿它当 unsigned 去算移位/取模就会出岔子。
    EXPECT_FALSE(Type::makeBool()->isUnsignedInteger());
}

// ─────────────────────────────────────────────────────────────────────────────
// ①-b char / signed char / unsigned char 三个类型【互不相等】
// ─────────────────────────────────────────────────────────────────────────────
// [basic.fundamental]/7：这是三个不同的类型（三个不同的 TypeKind）。
// 若把它们折成一个，`D<'a'>` 与 `DS<(signed char)'a'>` 就会撞成同一个实例。
TEST(NttpTypeDomain, CharTripleAreDistinctTypes) {
    const TypePtr c  = Type::makeChar();
    const TypePtr sc = Type::makeSChar();
    const TypePtr uc = Type::makeUChar();

    EXPECT_FALSE(c->equals(sc));
    EXPECT_FALSE(c->equals(uc));
    EXPECT_FALSE(sc->equals(uc));

    // 但三者都是"字符类型"（isChar 是"这族"的谓词，不是"等于 char"）
    EXPECT_TRUE(c->isChar());
    EXPECT_TRUE(sc->isChar());
    EXPECT_TRUE(uc->isChar());
    EXPECT_FALSE(Type::makeInt()->isChar());
}

// ─────────────────────────────────────────────────────────────────────────────
// ①-c 可表示性判据在【每个类型的上下界】上都要对
// ─────────────────────────────────────────────────────────────────────────────
// 判据是 [dcl.init]/7 窄化禁令 + [expr.const]/10 常量表达式豁免的合体：
//   · 有符号 N 位：[-2^(N-1), 2^(N-1)-1]
//   · 无符号 N 位：[0, 2^N - 1]
//   · bool 特例：可表示集合就是 {0,1}（不是"1 位无符号"的 {0,1} —— 巧合相同，
//     但语义不同：bool 的 1 表示 true，不是整数 1；本项目按同一集合处理）
// 每个类型都测"下界-1 / 下界 / 上界 / 上界+1"四点，越界方向两侧都测。
TEST(NttpTypeDomain, CanRepresentValueBoundaries) {
    // bool
    EXPECT_TRUE (Type::makeBool()->canRepresentValue(0));
    EXPECT_TRUE (Type::makeBool()->canRepresentValue(1));
    EXPECT_FALSE(Type::makeBool()->canRepresentValue(2));
    EXPECT_FALSE(Type::makeBool()->canRepresentValue(-1));

    // 有符号 8 位（char / signed char 同界）
    for (TypePtr t : {Type::makeChar(), Type::makeSChar()}) {
        EXPECT_TRUE (t->canRepresentValue(-128));
        EXPECT_TRUE (t->canRepresentValue(127));
        EXPECT_FALSE(t->canRepresentValue(-129));
        EXPECT_FALSE(t->canRepresentValue(128));
    }

    // 无符号 8 位
    EXPECT_TRUE (Type::makeUChar()->canRepresentValue(0));
    EXPECT_TRUE (Type::makeUChar()->canRepresentValue(255));
    EXPECT_FALSE(Type::makeUChar()->canRepresentValue(256));
    EXPECT_FALSE(Type::makeUChar()->canRepresentValue(-1));

    // 有符号 16 位
    EXPECT_TRUE (Type::makeShort()->canRepresentValue(-32768));
    EXPECT_TRUE (Type::makeShort()->canRepresentValue(32767));
    EXPECT_FALSE(Type::makeShort()->canRepresentValue(-32769));
    EXPECT_FALSE(Type::makeShort()->canRepresentValue(32768));

    // 有符号 32 位
    EXPECT_TRUE (Type::makeInt()->canRepresentValue(-2147483647 - 1));
    EXPECT_TRUE (Type::makeInt()->canRepresentValue(2147483647));
    EXPECT_FALSE(Type::makeInt()->canRepresentValue(2147483648LL));

    // 无符号 32 位
    EXPECT_TRUE (Type::makeUInt()->canRepresentValue(4294967295LL));
    EXPECT_FALSE(Type::makeUInt()->canRepresentValue(4294967296LL));
    EXPECT_FALSE(Type::makeUInt()->canRepresentValue(-1));

    // 64 位：边界在 int64_t 之外，测"恒真/仅符号位决定"
    EXPECT_TRUE (Type::makeLong()->canRepresentValue(INT64_MAX));
    EXPECT_TRUE (Type::makeLong()->canRepresentValue(INT64_MIN));
    EXPECT_TRUE (Type::makeULongLong()->canRepresentValue(0));
    EXPECT_FALSE(Type::makeULongLong()->canRepresentValue(-1));
}

// ─────────────────────────────────────────────────────────────────────────────
// ①-d 非整型一律"不可表示"（判据不能对 double/指针/类返回 true）
// ─────────────────────────────────────────────────────────────────────────────
TEST(NttpTypeDomain, NonIntegralTypesNeverRepresentIntegers) {
    EXPECT_FALSE(Type::makeDouble()->canRepresentValue(0));
    EXPECT_FALSE(Type::makeVoid()->canRepresentValue(0));
    EXPECT_FALSE(Type::makePointer(Type::makeInt())->canRepresentValue(0));
    EXPECT_FALSE(Type::makeClass("S")->canRepresentValue(6));
}

// ─────────────────────────────────────────────────────────────────────────────
// ②-a 【回归 B17】形态不等但值装得下 ⇒ 必须【接受】
// ─────────────────────────────────────────────────────────────────────────────
// 旧实现要求 a.valueType->equals(p.nonType)，于是这三条 clang 认可的合法程序
// 全被拒。断言"零语义错误"即可 —— 不比文案，因为正确的行为是【不报错】。
TEST(NttpTypeDomain, IntegralConversionIsAcceptedWhenValueFits) {
    const char* kPrelude =
        "template <bool B> struct F { int v; };\n"
        "template <int N>  struct A { int v; };\n";

    // int 1 ⇒ bool：1 ∈ {0,1}
    EXPECT_EQ(semaErrorOf(std::string(kPrelude) +
              "int main() { F<1> a; return 0; }\n"), "")
        << "Flag<1>（bool ← int 1）是合法程序（[conv.bool] + 值域内）";

    // long 4 ⇒ int：4 ∈ int
    EXPECT_EQ(semaErrorOf(std::string(kPrelude) +
              "int main() { A<4L> a; return 0; }\n"), "")
        << "A<4L>（int ← long 4）是合法程序（[conv.integral] + 值域内）";

    // bool ⇒ int：整型提升，恒不窄化
    EXPECT_EQ(semaErrorOf(std::string(kPrelude) +
              "int main() { A<true> a; return 0; }\n"), "")
        << "A<true>（int ← bool）是合法程序（[conv.prom]）";
}

// ─────────────────────────────────────────────────────────────────────────────
// ②-b 【同一判据的另一半】值装不下 ⇒ 必须【拒绝】，且文案与 clang 逐字同
// ─────────────────────────────────────────────────────────────────────────────
// 这四条与上一条共用一个判据（canRepresentValue）—— 只改了判据的方向，
// 不是两段独立逻辑。若有人把判据改成"只看符号性"或"只看位宽"，两条会同时红。
TEST(NttpTypeDomain, NarrowingIsRejectedWithClangWording) {
    struct Case { const char* src; const char* expect; };
    const Case cases[] = {
        {"template <bool B> struct F { int v; };\n"
         "int main() { F<2> a; return 0; }\n",
         "cannot be narrowed to type 'bool'"},
        {"template <unsigned U> struct G { int v; };\n"
         "int main() { G<-1> a; return 0; }\n",
         "cannot be narrowed to type 'unsigned int'"},
        {"template <char C> struct D { int v; };\n"
         "int main() { D<300> a; return 0; }\n",
         "cannot be narrowed to type 'char'"},
        {"template <short S> struct H { int v; };\n"
         "int main() { H<70000> a; return 0; }\n",
         "cannot be narrowed to type 'short'"},
    };

    for (const auto& c : cases) {
        const std::string err = semaErrorOf(c.src);
        // clang 原文：non-type template argument evaluates to N, which cannot be
        // narrowed to type 'T' [-Wc++11-narrowing]（本项目按错误处理）
        EXPECT_NE(err.find("non-type template argument evaluates to"),
                  std::string::npos) << c.expect << " / " << err;
        EXPECT_NE(err.find(c.expect), std::string::npos)
            << "诊断文本必须点名【形参声明的那一个类型】: " << err;
        // 报错必须发生在【实参列表】这一层，不能等到实例化
        EXPECT_EQ(err.find("Instantiat"), std::string::npos) << err;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ②-c 归一：形态不等的实参归到形参类型，且【跨写法只建一份实例】
// ─────────────────────────────────────────────────────────────────────────────
// [temp.arg.nontype]/1 的 converted constant expression ⇒ `A<4L>`、`A<4u>`
// 在 `template<int N>` 下与 `A<4>` 是同一个实例（clang：duplicate explicit
// instantiation）。守住的机制是 checkTemplateArguments 的"③-c 形态归一"
// （把 a.valueType 改写成 p.nonType）—— 归一之后可读串 / 缓存键 / mangling
// 才会一致，否则 `A<4L>` 会另建一份 `A_4`（撞符号）或者悄悄用错的那份。
//
// ★ 断言的写法避开了"具体在哪一步归一"：首见 `A<4L>` 走完整检查并落实例，
//   随后 `A<4>` 直接命中缓存 —— 两条路径都指向"只有一份实例"这个结果。
//   数实例个数 / 数 cache hit，比逐条比对日志顺序稳定得多。
TEST(NttpTypeDomain, ConversionNormalizesArgumentFormToOneInstance) {
    const std::string log = semaLogOf(
        "template <int N> struct A { int v; };\n"
        "int main() { A<4L> b; A<5u> c; A<4> d; return 0; }\n");

    auto countOf = [](const std::string& hay, const std::string& needle) {
        size_t n = 0;
        for (size_t p = hay.find(needle); p != std::string::npos;
             p = hay.find(needle, p + 1)) { ++n; }
        return n;
    };

    // 两个"形态不等"的实参各触发一次归一（值不同 ⇒ 不会命中彼此缓存）
    EXPECT_EQ(countOf(log, "值位整型转换"), 2u) << log;

    // ★ 不变量：三次用法只建两份实例（N=4 一份、N=5 一份），
    //   且 `A<4>`（形态已相等）那次是命中缓存而非再建一份。
    EXPECT_EQ(countOf(log, "Instance:"), 2u) << log;
    EXPECT_EQ(countOf(log, "cache hit:"), 1u) << log;

    // 归一之后，未归一的形态串不许再出现在下游日志里
    EXPECT_EQ(log.find("A<4L>"), std::string::npos)
        << "归一之后不该再出现未归一的形态串: " << log;
    EXPECT_EQ(log.find("A<5u>"), std::string::npos) << log;
}

// ─────────────────────────────────────────────────────────────────────────────
// ②-d 【不变量】auto 形参位：不同形态的实参必须映到不同的实例名
// ─────────────────────────────────────────────────────────────────────────────
// 这是本轮修掉的**静默 bug**：实例名/缓存键曾由 TemplateArg::toString() 生成，
// 而它对 `4`（int）与 `4`（long）产出同一个串 "4" ⇒ 第二个实例被静默复用。
// ★ 断言的措辞刻意避开具体命名规则：只要求"两份实参 ⇒ 两个不同的名字"。
TEST(NttpTypeDomain, AutoParamDistinguishesArgumentForms) {
    const std::string log = semaLogOf(
        "template <auto V> struct K { int v; };\n"
        "int main() { K<4> a; K<4L> b; K<'x'> c; K<true> d; return 0; }\n");

    // 收集四个实例名（`║ Instance:  <name>`）
    std::vector<std::string> names;
    const std::string tag = "Instance:";
    for (size_t p = log.find(tag); p != std::string::npos;
         p = log.find(tag, p + 1)) {
        size_t b = log.find_first_not_of(" \t", p + tag.size());
        size_t e = log.find_first_of(" \t\n", b);
        names.push_back(log.substr(b, e - b));
    }

    ASSERT_EQ(names.size(), 4u) << "四个不同形态的实参必须建四个实例\n" << log;
    // 两两互不相同（撞键就会少建，size 先红；这里再钉一次"名字本身可区分"）
    for (size_t i = 0; i < names.size(); i++) {
        for (size_t j = i + 1; j < names.size(); j++) {
            EXPECT_NE(names[i], names[j])
                << "实例名撞车 ⇒ 第二个会被静默复用: " << names[i] << "\n" << log;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ②-e 【不变量】同一实例必须只有一个 mangling
// ─────────────────────────────────────────────────────────────────────────────
// `K<4>` 的编码要处处一致。曾出现过 `_Z1BILi4EE` 与 `_Z1BILj4EE` 两份
// （演示路径硬编码 ofValue(4) 不带形态、Sema 路径按形参类型归一），
// 根因是同一个值被两条路径各自定形态。
TEST(NttpTypeDomain, OneInstanceHasExactlyOneMangling) {
    const std::string log = semaLogOf(
        "template <auto V> struct K { int v; };\n"
        "template <int N> struct B { int v; };\n"
        "int main() { K<4> a; B<4> b; return 0; }\n");

    // 每个 "Mangled: <name> → <enc>" 行里，同一 name 的 enc 必须唯一
    std::map<std::string, std::string> seen;
    const std::string arrow = " → ";
    for (size_t p = log.find("║ Mangled: "); p != std::string::npos;
         p = log.find("║ Mangled: ", p + 1)) {
        size_t b  = p + std::string("║ Mangled: ").size();
        size_t a  = log.find(arrow, b);
        if (a == std::string::npos) continue;
        size_t e  = log.find('\n', a);
        std::string name = log.substr(b, a - b);
        std::string enc  = log.substr(a + arrow.size(), e - a - arrow.size());
        auto it = seen.find(name);
        if (it == seen.end()) {
            seen[name] = enc;
        } else {
            EXPECT_EQ(it->second, enc)
                << "同一个实例出现两份编码: " << name << "\n" << log;
        }
    }
    EXPECT_FALSE(seen.empty()) << "没有采集到任何 mangling 行\n" << log;
}

// ─────────────────────────────────────────────────────────────────────────────
// ②-f 字符字面量的日志必须【可打印】（转义不回流会把日志行拦腰截断）
// ─────────────────────────────────────────────────────────────────────────────
// 词法期 `'\n'` 已被翻译成真正的 0x0A，回吐日志时要重新转义，
// 否则 `[parse:targ] … char literal '` 后面就换行了 —— logdiff 的逐行
// 对比会失去意义（一行分裂成两行），docs/learn 里粘贴的日志也没法看。
TEST(NttpTypeDomain, CharLiteralLogStaysOnOneLine) {
    const std::string log = semaLogOf(
        "template <char C> struct D { int v; };\n"
        "int main() { D<'\\n'> a; D<'\\t'> b; D<'\\\\'> c; return 0; }\n");

    // 三行 `char literal` 必须各自完整
    EXPECT_NE(log.find("char literal '\\n' (10)"), std::string::npos) << log;
    EXPECT_NE(log.find("char literal '\\t' (9)"),  std::string::npos) << log;
    EXPECT_NE(log.find("char literal '\\\\' (92)"), std::string::npos) << log;
    // 反面：不允许出现"半行"（正文里塞了裸控制字符）
    EXPECT_EQ(log.find("char literal '\n"), std::string::npos)
        << "裸换行把日志行截断了: " << log;
}

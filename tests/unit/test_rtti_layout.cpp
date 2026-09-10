// =============================================================================
// tests/unit/test_rtti_layout.cpp —— 对象内存布局 / vtable / typeinfo 布局实验
// =============================================================================
// 被测对象（双轨）：
//   Part A —— minicc 管线的 RTTI 发射行为（白盒）：
//     · typeinfo 三槽何时发射、助手 __minicc_dynamic_cast 何时按需发射；
//     · Sema 的 dynamic_cast 静态检查（同体系放行 / 无关类拒绝）。
//   Part B —— 真实 ABI（本机 clang++/libc++）的内存布局实验（黑盒观测）：
//     · 各种继承形态的对象内存布局草图 + 实测偏移对拍；
//     · 对象头部指针指向的结构（vtable / vbtable）逐槽解剖；
//     · vtable[-1] 指向的 typeinfo 再逐槽解剖（typeinfo 自己也是多态对象！）；
//     · __vmi_class_type_info 的 base_count/offset_flags 逐字节解码。
//
// 考察理论点（对应 docs/learn/11-dynamic_cast-rtti.md）：
//   1. 无虚函数 + 无 RTTI 使用点 → 无 vptr、无 vtable、无 typeinfo（零开销）
//   2. 多态类 → 对象首 8 字节 _vptr；vtable = [-2]offset-to-top [-1]&typeinfo
//      [0..]虚函数地址
//   3. 单继承：基类子对象叠在偏移 0；typeinfo 用 __si 形态（第三槽 = 基类 ti）
//   4. 多继承：子对象并排，各有独立头指针；第二张表 offset-to-top = -16，
//      函数槽是 thunk；typeinfo 用 __vmi 形态（基类数组编码偏移）
//   5. 虚继承：vbase 子对象被推到对象尾部；有虚函数时头指针 = vptr+vbptr 合一，
//      表内 [-3] 槽 = 到共享虚基的距离；无虚函数时头指针 = 纯 _vbptr
//   6. typeinfo 按继承拓扑三选一：__class / __si_class / __vmi_class_type_info
//   7. offset_flags 编码：低 2 位 = virtual|public 标志，高位 >> 8 = 偏移
//
// 观测风格：每个用例先给"源码片段 + 推演草图"，然后【逐槽打印内存图形】——
// 对象字段表、头部指针、vtable 每一槽、typeinfo 每一槽，全部带实测值与
// 符号名（依赖 -rdynamic，见 CMakeLists），最后用 EXPECT_EQ 钉死黄金值。
// =============================================================================

#include <gtest/gtest.h>

#include <cxxabi.h>   // abi::__cxa_demangle（libc++ 提供）
#include <dlfcn.h>    // dladdr：地址 → 符号名

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "obs_helpers.h"

using namespace minicc;

// ─────────────────────────────────────────────────────────────────────────────
// 管线辅助：源码 → AST（日志静默）
// ─────────────────────────────────────────────────────────────────────────────
static TranslationUnit parseQuiet(const std::string& src) {
    StdoutCapture cap;
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    return parser.parseTranslationUnit();
}

// 源码 → 完整汇编字符串（语义日志全部捕获，供断言）
static std::string compileQuiet(const std::string& src, std::string* semaLog) {
    StdoutCapture cap;
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    TranslationUnit unit = parser.parseTranslationUnit();
    SemanticAnalyzer sema;
    sema.analyze(unit);
    CodeGen cg;
    std::string asmCode = cg.generate(unit, sema.getClassTypes(), sema.getFunctions());
    if (semaLog) *semaLog = cap.str();
    return asmCode;
}

// 从汇编里摘出以 marker 开头的连续 block（到空行为止），便于打印观测
static std::string extractBlock(const std::string& asmCode, const std::string& marker) {
    auto pos = asmCode.find(marker);
    if (pos == std::string::npos) return "(未找到: " + marker + ")";
    auto lineBegin = asmCode.rfind('\n', pos);
    lineBegin = (lineBegin == std::string::npos) ? 0 : lineBegin + 1;
    auto blockEnd = asmCode.find("\n\n", pos);
    return asmCode.substr(lineBegin, blockEnd == std::string::npos
                                        ? std::string::npos
                                        : blockEnd - lineBegin);
}

// =============================================================================
// Part A —— minicc 管线的 RTTI 发射行为
// =============================================================================

// ─────────────────────────────────────────────────────────────────────────────
// A1. 助手按需发射：没写 dynamic_cast，.s 里绝不出现 __minicc_dynamic_cast
// ─────────────────────────────────────────────────────────────────────────────
TEST(MiniccRtti, HelperOnlyEmittedWhenDynamicCastUsed) {
    std::string src =
        "class Animal { public: int legs; virtual int speak() { return 0; } };\n"
        "int main() { Animal* a = new Animal(); return a->speak(); }\n";
    std::string log;
    std::string asmCode = compileQuiet(src, &log);

    // 推演：虚函数存在 → vtable/typeinfo 必须发射；但没有 dynamic_cast →
    //       运行时助手毫无用处，按"最小发射"原则不出现
    bool hasTi = asmCode.find("_ZTI6Animal:") != std::string::npos;
    bool hasHelper = asmCode.find("__minicc_dynamic_cast:") != std::string::npos;
    std::printf("[发射决策] 有虚函数 + 无 dynamic_cast：\n"
                "    typeinfo _ZTI6Animal          → %s\n"
                "    助手 __minicc_dynamic_cast    → %s（按需发射原则）\n",
                hasTi ? "发射 ✓" : "缺失 ✗",
                hasHelper ? "发射 ✗（不该出现!）" : "不发射 ✓");
    EXPECT_TRUE(hasTi) << "多态类应有 typeinfo";
    EXPECT_FALSE(hasHelper) << "未使用 dynamic_cast 时不应发射运行时助手";
}

// ─────────────────────────────────────────────────────────────────────────────
// A2. typeinfo 三槽格式：链式基类指针（__si 风格），根类第三槽为 0
//
//   _ZTI3Dog:                          _ZTI6Animal:
//     .quad 0        # vptr(简化)        .quad 0
//     .quad <名字>    # 类型名            .quad <名字>
//     .quad _ZTI6Animal # ★基类指针      .quad 0        # ★链尾
// ─────────────────────────────────────────────────────────────────────────────
TEST(MiniccRtti, TypeinfoThreeSlotsChainLinksBases) {
    std::string src =
        "class Animal { public: int legs; virtual int speak() { return 0; } };\n"
        "class Dog : public Animal { public: int speed; "
        "virtual int speak() { return 1; } };\n"
        "int main() {\n"
        "    Animal* a = new Dog();\n"
        "    Dog* d = dynamic_cast<Dog*>(a);\n"
        "    if (d == nullptr) { return 1; }\n"
        "    return 0;\n"
        "}\n";
    std::string log;
    std::string asmCode = compileQuiet(src, &log);

    // ── 逐槽打印两个 typeinfo 的发射内容 ──────────────────────────────
    std::string dogTi = extractBlock(asmCode, "_ZTI3Dog:");
    std::string animalTi = extractBlock(asmCode, "_ZTI6Animal:");
    std::printf("\n┌─ minicc 发射的 typeinfo（.data 段，逐槽）\n"
                "│\n"
                "│ 派生类 _ZTI3Dog（__si 风格，链的中间节点）：\n");
    for (char ch : dogTi) std::putchar(ch);
    std::printf("│     槽0(+0)  type_info 自身 vptr（教学版简化为 0）\n"
                "│     槽1(+8)  类型名字符串 → \"Dog\"\n"
                "│     槽2(+16) ★基类 typeinfo 指针 → _ZTI6Animal\n"
                "│\n"
                "│ 根类 _ZTI6Animal（链尾）：\n");
    for (char ch : animalTi) std::putchar(ch);
    std::printf("│     槽2(+16) = 0 ★链尾标记（助手循环的终止条件）\n"
                "└─\n");
    std::printf("[运行期链条] obj._vptr → _ZTV3Dog → [-1]=&_ZTI3Dog\n"
                "             → [+16]=&_ZTI6Animal → [+16]=0（到根，停）\n");

    EXPECT_NE(dogTi.find(".quad _ZTI6Animal"), std::string::npos);
    EXPECT_NE(animalTi.find(".quad 0                 # 无基类（基类计数 = 0）"),
              std::string::npos);

    // 助手被发射且只发射一次
    size_t first = asmCode.find("__minicc_dynamic_cast:");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(asmCode.find("__minicc_dynamic_cast:", first + 1), std::string::npos);

    // 助手汇编的关键访存（多继承 DFS 版本）：
    //   (%rdi)      对象 → _vptr
    //   -8(%rcx)    vtable[-1] → most-derived typeinfo
    //   16(%rdx)    typeinfo base count (计数式布局)
    std::printf("[助手关键访存] movq(%%rdi),%%rcx=对象→vptr   "
                "movq -8(%%rcx),%%rdx=ti   movq 16(%%rdx)=base_count\n");
    EXPECT_NE(asmCode.find("movq (%rdi), %rcx"), std::string::npos);
    EXPECT_NE(asmCode.find("movq -8(%rcx), %rdx"), std::string::npos);
    EXPECT_NE(asmCode.find("movq 16(%rdx), %rax"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// A3. 非多态类：无 typeinfo、无 vtable（与真实编译器一致）
// ─────────────────────────────────────────────────────────────────────────────
TEST(MiniccRtti, NonPolymorphicClassGetsNoRtti) {
    std::string src =
        "class Point { public: int x; };\n"
        "int main() { Point* p = new Point(); return p->x; }\n";
    std::string asmCode = compileQuiet(src, nullptr);

    bool noTi = asmCode.find("_ZTI4Point") == std::string::npos;
    bool noVt = asmCode.find("_ZTV4Point") == std::string::npos;
    std::printf("[发射决策] 无虚函数类 Point：typeinfo %s，vtable %s"
                "（零运行时开销，与 clang 一致）\n",
                noTi ? "不发射 ✓" : "发射了 ✗",
                noVt ? "不发射 ✓" : "发射了 ✗");
    EXPECT_TRUE(noTi) << "无虚函数类不应有 typeinfo（没有 vtable 可挂）";
    EXPECT_TRUE(noVt) << "无虚函数类不应有 vtable";
}

// ─────────────────────────────────────────────────────────────────────────────
// A4. Sema：同体系转型放行，且日志可观测 [dynamic_cast] Src* → Target*
// ─────────────────────────────────────────────────────────────────────────────
TEST(MiniccRtti, SemaAcceptsRelatedClasses) {
    std::string src =
        "class Animal { public: virtual int speak() { return 0; } };\n"
        "class Dog : public Animal { public: virtual int speak() { return 1; } };\n"
        "int main() {\n"
        "    Animal* a = new Dog();\n"
        "    Dog* d = dynamic_cast<Dog*>(a);\n"
        "    if (d == nullptr) { return 1; }\n"
        "    return 0;\n"
        "}\n";
    std::string log;
    compileQuiet(src, &log);
    auto pos = log.find("[dynamic_cast]");
    if (pos != std::string::npos) {
        auto eol = log.find('\n', pos);
        std::printf("[Sema 日志] %s（静态层：同体系放行）\n",
                    log.substr(pos, eol - pos).c_str());
    }
    EXPECT_NE(log.find("[dynamic_cast] Animal* → Dog*"), std::string::npos)
        << "语义日志应打印转型方向";
}

// ─────────────────────────────────────────────────────────────────────────────
// A5. Sema：无关类拒绝（对应 clang "cannot cast 'A *' to 'B *'"）
// ─────────────────────────────────────────────────────────────────────────────
TEST(MiniccRtti, SemaRejectsUnrelatedClasses) {
    std::string src =
        "class Alpha { public: virtual int f() { return 0; } };\n"
        "class Beta  { public: virtual int g() { return 0; } };\n"
        "int main() {\n"
        "    Alpha* p = new Alpha();\n"
        "    Beta* q = dynamic_cast<Beta*>(p);\n"
        "    return 0;\n"
        "}\n";
    StdoutCapture cap;
    TranslationUnit unit = parseQuiet(src);
    SemanticAnalyzer sema;
    try {
        sema.analyze(unit);
        FAIL() << "无关类的 dynamic_cast 应被拒绝";
    } catch (const std::runtime_error& e) {
        std::printf("[Sema 拒绝] %s（静态层：不同继承体系，编译期即报错）\n",
                    e.what());
        EXPECT_NE(std::string(e.what()).find("unrelated class types"),
                  std::string::npos);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// A6. 继承图观测：多态类带 [polymorphic] 标记，非多态类没有
// ─────────────────────────────────────────────────────────────────────────────
TEST(MiniccRtti, InheritanceGraphMarksPolymorphicOnly) {
    std::string src =
        "class Animal { public: virtual int speak() { return 0; } };\n"
        "class Dog : public Animal { public: virtual int speak() { return 1; } };\n"
        "class Point { public: int x; };\n"
        "int main() { return 0; }\n";
    std::string log;
    compileQuiet(src, &log);
    auto graphPos = log.find("类型继承图");
    ASSERT_NE(graphPos, std::string::npos);
    // 打印继承图区段（到 Pass 3 为止）供观测
    auto endPos = log.find("Pass 3", graphPos);
    std::printf("[Sema 继承图]\n%.*s",
                static_cast<int>((endPos == std::string::npos ? log.size() : endPos) - graphPos),
                log.c_str() + graphPos);
    EXPECT_NE(log.find("Animal [polymorphic]"), std::string::npos);
    EXPECT_NE(log.find("Dog [polymorphic]"), std::string::npos);
    // Point 出现在继承图里，但它那一行必须没有 [polymorphic] 标记
    auto pointPos = log.find("Point", graphPos);
    ASSERT_NE(pointPos, std::string::npos);
    auto lineEnd = log.find('\n', pointPos);
    std::string pointLine = log.substr(pointPos, lineEnd - pointPos);
    EXPECT_EQ(pointLine.find("[polymorphic]"), std::string::npos);
}

// =============================================================================
// Part B —— 真实 ABI 内存布局实验（本机 clang++ 编译，观测真实世界）
// =============================================================================

namespace {

// 各实验用类型（名字互不冲突，便于一个翻译单元内共存）
struct Plain { int x; };                                 // 实验①：无任何运行时
struct Poly  { virtual ~Poly() {} int x; };              // 实验②：多态基类
struct Dog   : Poly { int speed; };                      // 实验③：单继承

struct MiA { int a; virtual ~MiA() {} };                 // 实验④：多继承
struct MiB { int b; virtual ~MiB() {} };
struct MiC : MiA, MiB { int c; };

struct DiaB { int v; virtual ~DiaB() {} };               // 实验⑤：菱形虚继承
struct DiaD : virtual DiaB { int d; };
struct DiaE : virtual DiaB { int e; };
struct DiaX : DiaD, DiaE { int x; };

struct ViV { int v; };                                   // 实验⑥：虚继承无虚函数
struct ViW : virtual ViV { int w; };

struct SibA { virtual ~SibA() {} };                      // 实验⑧：兄弟转型
struct SibB : SibA {};
struct SibC : SibA {};

// 指针差（字节）
long off(const void* p, const void* base) {
    return static_cast<const char*>(p) - static_cast<const char*>(base);
}

// ─────────────────────────────────────────────────────────────────────────────
// 内存图形渲染工具：把"对象 → 头指针 → vtable/vbtable → typeinfo"整条链
// 逐槽画成 ASCII 表格，所有数值均为运行期实测
// ─────────────────────────────────────────────────────────────────────────────

// ── 符号注册表：匿名命名空间类的 vtable/typeinfo 是内部链接符号，
//    -rdynamic 也导不进动态符号表，dladdr 查不到。因此在每个用例开头
//    把实测指针→名字注册进表；查表优先，其次 dladdr（libc++ 里
//    __si/__vmi 等 ABI 内部类的虚表是导出的，可以解析），最后退化。
std::vector<std::pair<const void*, std::string>> g_symReg;

void regSym(const void* p, std::string name) {
    g_symReg.emplace_back(p, std::move(name));
}

// 地址 → 符号名。查表 → dladdr → 退化。
std::string symName(const void* p) {
    if (!p) return "(null)";
    for (const auto& [k, v] : g_symReg)
        if (k == p) return v;
    Dl_info info{};
    if (dladdr(p, &info) && info.dli_sname) {
        int st = 0;
        char* dem = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &st);
        std::string s = (st == 0 && dem) ? dem : info.dli_sname;
        std::free(dem);
        long delta = static_cast<const char*>(p) -
                     static_cast<const char*>(info.dli_saddr);
        if (delta != 0) s += "+" + std::to_string(delta);
        return s;
    }
    return "(纯数据)";
}

std::string fmtAddr(const void* p) {
    std::ostringstream os;
    os << p;
    return os.str();
}

std::string fmtHex(unsigned long v) {
    std::ostringstream os;
    os << "0x" << std::hex << v;
    return os.str();
}

// 字节级 hex（Plain 这类小对象用）
void drawRawBytes(const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    std::printf("│ 原始字节: ");
    for (size_t i = 0; i < n; ++i) std::printf("%02x ", b[i]);
    std::printf(" （小端：%d 存在低字节）\n", *static_cast<const int*>(p));
}

// 对象字段表：偏移 / 字段 / 实测内容
struct Field { const char* name; long offset; long size; std::string note; };

void drawObjectMap(const std::string& title, const std::vector<Field>& fields,
                   size_t total) {
    std::printf("\n┌─ %s（sizeof = %zu B）\n", title.c_str(), total);
    std::printf("│ 偏移    字段                    实测内容\n");
    std::printf("│ ------  ----------------------  --------------------------------------\n");
    for (const auto& f : fields) {
        char head[80];
        std::snprintf(head, sizeof(head), "+%-5ld %s(%ldB)", f.offset, f.name, f.size);
        std::printf("│ %-28s %s\n", head, f.note.c_str());
    }
    std::printf("└─\n");
}

// vtable 逐槽解剖。vptr 指向槽[0]（第一个虚函数）；
// ABI 规定 [-2]=offset-to-top，[-1]=&typeinfo。
// showVbase=true 时额外展示 [-3]（虚继承的 vbase 距离槽）。
void drawVTable(const std::string& who, const void* vptr, int nFnSlots,
                bool showVbase = false) {
    const long* s = static_cast<const long*>(vptr);
    std::printf("┌─ %s 的头指针 = %s → %s\n", who.c_str(),
                fmtAddr(vptr).c_str(), symName(vptr).c_str());
    std::printf("│ 槽位   含义                          实测值\n");
    std::printf("│ -----  ----------------------------  ----------------------------------\n");
    if (showVbase)
        std::printf("│ [-3]   vbase 距离(本部分→共享虚基)     %ld\n", s[-3]);
    std::printf("│ [-2]   offset-to-top(到完整对象顶部)   %ld\n", s[-2]);
    std::printf("│ [-1]   typeinfo 指针                 %s = %s\n",
                fmtAddr(reinterpret_cast<const void*>(s[-1])).c_str(),
                symName(reinterpret_cast<const void*>(s[-1])).c_str());
    for (int i = 0; i < nFnSlots; ++i)
        std::printf("│ [%2d]   虚函数槽                      %s [%s]\n", i,
                    fmtAddr(reinterpret_cast<const void*>(s[i])).c_str(),
                    symName(reinterpret_cast<const void*>(s[i])).c_str());
    std::printf("└─\n");
}

// typeinfo 逐槽解剖：typeinfo 自己也是多态对象——
//   +0  它自己的 vptr（→ __class/__si/__vmi 三种 ABI 内部类之一的虚表）
//   +8  类型名（mangled 字符串，如 "3Dog"）
//   __si 再 +16 = 基类 typeinfo（顺着它递归 = 运行期上溯继承链）
//   __vmi 再 +16/+20 = flags/base_count（两个 u32），+24 起 = 基类数组
void drawTypeInfo(const void* ti, int indent = 0) {
    if (!ti) return;
    const long* s = static_cast<const long*>(ti);
    std::string pad(static_cast<size_t>(indent) * 3, ' ');

    // 这个 typeinfo 的【运行期类别】：__class / __si_class / __vmi_class
    const auto& tiObj = *reinterpret_cast<const std::type_info*>(ti);
    int st = 0;
    char* kindDem = abi::__cxa_demangle(typeid(tiObj).name(), nullptr, nullptr, &st);
    std::string kind = (st == 0 && kindDem) ? kindDem : typeid(tiObj).name();
    std::free(kindDem);

    // 槽[1] = 类型名字符串（mangled），解缠后打印
    const char* raw = reinterpret_cast<const char*>(s[1]);
    char* nameDem = abi::__cxa_demangle(raw, nullptr, nullptr, &st);
    std::string pretty = (st == 0 && nameDem) ? nameDem : raw;
    std::free(nameDem);

    std::printf("%s┌─ typeinfo for %s @ %s\n", pad.c_str(), pretty.c_str(),
                fmtAddr(ti).c_str());
    std::printf("%s│ 运行期类别: %s\n", pad.c_str(), kind.c_str());
    std::printf("%s│ +0   自身 vptr → %s\n", pad.c_str(),
                symName(reinterpret_cast<const void*>(s[0])).c_str());
    std::printf("%s│ +8   类型名    \"%s\"（mangled: \"%s\"）\n", pad.c_str(),
                pretty.c_str(), raw);

    if (kind.find("__si_class_type_info") != std::string::npos) {
        const void* base = reinterpret_cast<const void*>(s[2]);
        std::printf("%s│ +16  __base_type → %s\n", pad.c_str(),
                    fmtAddr(base).c_str());
        std::printf("%s└─ （单继承链：沿此槽递归 = dynamic_cast 的上溯路径）\n",
                    pad.c_str());
        drawTypeInfo(base, indent + 1);
    } else if (kind.find("__vmi_class_type_info") != std::string::npos) {
        auto u32At = [&](long o) {
            return *reinterpret_cast<const unsigned int*>(
                reinterpret_cast<const char*>(ti) + o);
        };
        unsigned int flags = u32At(16), count = u32At(20);
        std::printf("%s│ +16  flags(u32)      = %u\n", pad.c_str(), flags);
        std::printf("%s│ +20  base_count(u32) = %u   ★两个连续 u32 打包在 +16..+24\n",
                    pad.c_str(), count);
        for (unsigned int i = 0; i < count; ++i) {
            const long* b = reinterpret_cast<const long*>(
                reinterpret_cast<const char*>(ti) + 24 + 16L * i);
            const void* bti = reinterpret_cast<const void*>(b[0]);
            unsigned long of = static_cast<unsigned long>(b[1]);
            const char* braw =
                reinterpret_cast<const char*>(static_cast<const long*>(bti)[1]);
            char* bd = abi::__cxa_demangle(braw, nullptr, nullptr, &st);
            std::string bpretty = (st == 0 && bd) ? bd : braw;
            std::free(bd);
            std::printf("%s│ base[%u]: %s @ %s\n", pad.c_str(), i,
                        bpretty.c_str(), fmtAddr(bti).c_str());
            if (of & 1) {
                // 虚基类：高位 >> 8 不是"对象内偏移"，而是【有符号】值，
                // 指向 vbtable 内"虚基距离条目"相对 vbptr 的字节位置。
                // 实测统一为 -24 → 条目在 vbptr[-3]（表头第 3 槽之前）。
                long entry = static_cast<long>(static_cast<long long>(of) >> 8);
                long slot = entry / 8;
                std::printf("%s│          offset_flags raw=%s → virtual=1 public=%d；\n"
                            "%s│          ★高位>>8 = %+ld（有符号，字节）= vbptr 槽[%ld]：\n"
                            "%s│            该槽存\"本部分到共享虚基的字节距离\"（vbptr 间接寻址）\n",
                            pad.c_str(), fmtHex(of).c_str(),
                            static_cast<int>((of >> 1) & 1),
                            pad.c_str(), entry, slot,
                            pad.c_str());
            } else {
                std::printf("%s│          offset_flags raw=%s → 偏移=%lu public=%d virtual=0（对象内直接偏移）\n",
                            pad.c_str(), fmtHex(of).c_str(), of >> 8,
                            static_cast<int>((of >> 1) & 1));
            }
        }
        std::printf("%s└─ （多继承图：基类数组取代单链）\n", pad.c_str());
    } else {
        std::printf("%s└─ 根类型：只有 2 槽，无基类（链尾）\n", pad.c_str());
    }
}

// 从对象头指针出发，读出 vtable[-1] 的 typeinfo
const void* tiFromHead(const void* headPtrValue) {
    return reinterpret_cast<const void*>(
        *(static_cast<const long*>(headPtrValue) - 1));
}

// ── 符号注册（匿名命名空间类型是内部链接，-rdynamic 导不进动态符号表，
//    dladdr 查不到；把实测指针与"它是什么"显式登记，打印时查表）
template <typename T>
void regPrimary(const char* cls) {
    T obj;  // 栈对象：读主头指针
    regSym(*reinterpret_cast<void**>(&obj),
           std::string("vtable for ") + cls + "（主表）");
    regSym(&typeid(T), std::string("typeinfo for ") + cls);
}

// 注册对象内某个偏移处的头指针（多继承/虚继承的次表）
void regHeadAt(const void* objBase, long byteOff, std::string name) {
    void* p = *reinterpret_cast<void* const*>(
        static_cast<const char*>(objBase) + byteOff);
    regSym(p, std::move(name));
}

// 注册 vtable 的第 idx 个函数槽（析构函数槽 / thunk）
void regFnSlot(const void* vptr, int idx, std::string name) {
    regSym(reinterpret_cast<const void*>(static_cast<const long*>(vptr)[idx]),
           std::move(name));
}

// 一次性注册所有实验类型的头部符号（幂等，每个用例开头调用）
void registerAllRttiSymbols() {
    static bool done = false;
    if (done) return;
    done = true;
    regPrimary<Poly>("Poly");
    regPrimary<Dog>("Dog");
    regPrimary<MiA>("MiA");
    regPrimary<MiB>("MiB");
    regPrimary<MiC>("MiC");
    regPrimary<DiaB>("DiaB");
    regPrimary<DiaD>("DiaD");
    regPrimary<DiaE>("DiaE");
    regPrimary<DiaX>("DiaX");
    regPrimary<ViW>("ViW");
    regPrimary<SibA>("SibA");
    regPrimary<SibB>("SibB");
    regPrimary<SibC>("SibC");

    // 多继承次表 / 菱形三张子表：用实例读偏移处的头指针
    MiC mic;
    regHeadAt(&mic, 16, "vtable for MiC（次表，MiB 部分）");
    void* vpB = *reinterpret_cast<void**>(reinterpret_cast<char*>(&mic) + 16);
    regFnSlot(vpB, 0, "non-virtual thunk to MiC::~MiC()");
    void* vpA = *reinterpret_cast<void**>(&mic);
    regFnSlot(vpA, 0, "MiC::~MiC() [D1]");
    regFnSlot(vpA, 1, "MiC::~MiC() [D0]");
    regFnSlot(vpB, 1, "non-virtual thunk to MiC::~MiC()");

    DiaX diax;
    regHeadAt(&diax, 16, "vtable for DiaX（E 部分次表）");
    regHeadAt(&diax, 32, "vtable for DiaX（共享 DiaB 子表）");
    void* vD = *reinterpret_cast<void**>(&diax);
    regFnSlot(vD, 0, "DiaX::~DiaX() [D1]");
    regFnSlot(vD, 1, "DiaX::~DiaX() [D0]");
    void* vE = *reinterpret_cast<void**>(reinterpret_cast<char*>(&diax) + 16);
    regFnSlot(vE, 0, "non-virtual thunk to DiaX::~DiaX()");
    regFnSlot(vE, 1, "non-virtual thunk to DiaX::~DiaX()");
    void* vBs = *reinterpret_cast<void**>(reinterpret_cast<char*>(&diax) + 32);
    regFnSlot(vBs, 0, "virtual thunk to DiaX::~DiaX()");
    regFnSlot(vBs, 1, "virtual thunk to DiaX::~DiaX()");

    Dog dog;
    void* vDog = *reinterpret_cast<void**>(&dog);
    regFnSlot(vDog, 0, "Dog::~Dog() [D1]");
    regFnSlot(vDog, 1, "Dog::~Dog() [D0]");
    Poly poly;
    void* vPoly = *reinterpret_cast<void**>(&poly);
    regFnSlot(vPoly, 0, "Poly::~Poly() [D1]");
    regFnSlot(vPoly, 1, "Poly::~Poly() [D0]");
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// B1. 无虚函数 → 对象就是数据本身，头部什么都没有
//
//   struct Plain { int x; };
//   推演：没有虚函数 → 无需派发 → 无需 vptr → sizeof == sizeof(int)
//
//   ┌────────┐
//   │ x (4B) │   全部 4 字节都是成员，零运行时头
//   └────────┘
//   +0       +4
// ─────────────────────────────────────────────────────────────────────────────
TEST(LayoutSketch, PlainObjectNoRuntimeHeader) {
    Plain obj{42};
    drawObjectMap("struct Plain { int x; } —— 非多态类",
                  {{"x", 0, 4, "42（仅此一个成员，无任何隐藏头）"}},
                  sizeof(Plain));
    drawRawBytes(&obj, sizeof(Plain));
    std::printf("[结论] 4 字节全是成员数据 → 无 vptr、无 typeinfo、零运行时开销；\n"
                "       对它做 dynamic_cast 连编译都过不了（'not polymorphic'）\n");
    EXPECT_EQ(sizeof(Plain), 4u);
}

// ─────────────────────────────────────────────────────────────────────────────
// B2. 多态类 → 头部 8 字节被 _vptr 占据；顺着它解剖 vtable 和 typeinfo
//
//   struct Poly { virtual ~Poly() {} int x; };
//   推演：vptr(8) + x(4) + 对齐 padding(4) = 16
// ─────────────────────────────────────────────────────────────────────────────
TEST(LayoutSketch, PolymorphicObjectVptrAtHead) {
    registerAllRttiSymbols();
    Poly obj; obj.x = 7;
    void* head = *reinterpret_cast<void**>(&obj);

    drawObjectMap("struct Poly { virtual ~Poly(); int x; } —— 多态类",
                  {{"_vptr", 0, 8, fmtAddr(head) + "（非空！见下方解剖）"},
                   {"x", 8, 4, "7"},
                   {"pad", 12, 4, "对齐填充（8 字节对齐）"}},
                  sizeof(Poly));
    drawVTable("Poly", head, 2);                       // 虚函数只有析构 → 2 槽
    std::printf("  ★ 头部指针指向的结构 = vtable：前 2 槽是元数据"
                "（offset-to-top / typeinfo），之后才是虚函数地址。\n");
    std::printf("  ★ vtable[-1] 又指向 typeinfo——继续解剖：\n");
    drawTypeInfo(tiFromHead(head));

    EXPECT_EQ(sizeof(Poly), 16u);
    EXPECT_NE(head, nullptr);
    EXPECT_EQ(off(&obj.x, &obj), 8);   // x 被 vptr 挤到 +8
    // vtable[-2] offset-to-top = 0（主表，子对象即对象顶部）
    EXPECT_EQ(*(static_cast<long*>(head) - 2), 0);
    // vtable[-1] == &typeid(obj)（链条闭环）
    EXPECT_EQ(tiFromHead(head), static_cast<const void*>(&typeid(obj)));
}

// ─────────────────────────────────────────────────────────────────────────────
// B3. 单继承 → 基类子对象叠在偏移 0（这就是 dynamic_cast 免指针调整的原因）
//
//   struct Dog : Poly { int speed; };
//   推演：Poly 部分完整落在 Dog 头部：vptr + x 之后直接排 speed
//   ★ Dog 对象地址 == Poly 子对象地址 —— 所以 Dog*→Poly* 零调整
// ─────────────────────────────────────────────────────────────────────────────
TEST(LayoutSketch, SingleInheritanceSubobjectAtZero) {
    registerAllRttiSymbols();
    Dog obj; obj.x = 1; obj.speed = 30;
    Poly* asPoly = &obj;                       // 隐式 upcast
    void* head = *reinterpret_cast<void**>(&obj);

    drawObjectMap("struct Dog : Poly { int speed; } —— 单继承",
                  {{"_vptr", 0, 8, fmtAddr(head) + " → " + symName(head)},
                   {"x", 8, 4, "1（继承自 Poly，叠在头部）"},
                   {"speed", 12, 4, "30（Dog 自有）"}},
                  sizeof(Dog));
    std::printf("  ★ &obj = %s，upcast 后 Poly* = %s，指针差 = %ld"
                " → 单继承零调整\n", fmtAddr(&obj).c_str(),
                fmtAddr(asPoly).c_str(), off(asPoly, &obj));
    drawVTable("Dog", head, 2);
    std::printf("  ★ typeinfo 解剖：Dog 是 __si 形态，第 3 槽直指基类 Poly 的 ti——\n"
                "    这条链就是 dynamic_cast 运行期逐跳上溯的物理路径：\n");
    drawTypeInfo(tiFromHead(head));

    EXPECT_EQ(sizeof(Dog), 16u);
    EXPECT_EQ(off(&obj.x, &obj), 8);
    EXPECT_EQ(off(&obj.speed, &obj), 12);
    EXPECT_EQ(off(asPoly, &obj), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// B4. 多继承 → 基类子对象并排，各有独立头指针；第二张表 offset-to-top = -16
//
//   struct MiA { int a; virtual ~MiA(){} };  struct MiB { int b; virtual ~MiB(){} };
//   struct MiC : MiA, MiB { int c; };
//   推演：两个基类各自带 vptr，子对象首尾相接；
//   ★ MiC 地址 == MiA 子对象地址（+0），但 MiB 子对象在 +16
//     → dynamic_cast<MiB*>(c) 必须 +16，这个偏移只有运行时知道在哪
// ─────────────────────────────────────────────────────────────────────────────
TEST(LayoutSketch, MultipleInheritanceSubobjectsSideBySide) {
    registerAllRttiSymbols();
    MiC obj; obj.a = 1; obj.b = 2; obj.c = 3;
    MiA* asA = &obj;
    MiB* asB = &obj;
    MiB* dynB = dynamic_cast<MiB*>(asA);    // 经 A 子对象找回 B 子对象

    char* base = reinterpret_cast<char*>(&obj);
    void* vptrA = *reinterpret_cast<void**>(base);
    void* vptrB = *reinterpret_cast<void**>(base + 16);

    drawObjectMap("struct MiC : MiA, MiB { int c; } —— 多继承",
                  {{"vptr_A", 0, 8, fmtAddr(vptrA) + " → " + symName(vptrA)},
                   {"a", 8, 4, "1（MiA 成员）"},
                   {"pad", 12, 4, "填充"},
                   {"vptr_B", 16, 8, fmtAddr(vptrB) + " → " + symName(vptrB)},
                   {"b", 24, 4, "2（MiB 成员）"},
                   {"c", 28, 4, "3（MiC 自有）"}},
                  sizeof(MiC));
    std::printf("  ★ 一个对象、两个头指针：MiA 视角从 +0 看，MiB 视角从 +16 看。\n"
                "    MiC→MiA* 差 = %ld（免调整）；MiC→MiB* 差 = %+ld（要调整）\n",
                off(asA, &obj), off(asB, &obj));

    std::printf("\n  ── 第一张表（MiA 部分，主表）──\n");
    drawVTable("MiC[+0]", vptrA, 2);
    std::printf("\n  ── 第二张表（MiB 部分，次表）──\n");
    drawVTable("MiC[+16]", vptrB, 2);
    std::printf("  ★ 注意两点：\n"
                "    ① [-2] offset-to-top = -16：从 MiB 子对象回到对象顶部要 -16，\n"
                "       delete 一个 MiB* 时 delete-thunk 靠它恢复完整对象地址；\n"
                "    ② 函数槽是 thunk（_ZThn16_...）：先 this -= 16 再跳真析构。\n");

    std::printf("\n  ── MiC 的 typeinfo：__vmi 形态（基类数组，见下节逐字节解码）──\n");
    drawTypeInfo(static_cast<const void*>(&typeid(MiC)));

    std::printf("\n  [运行时验证] dynamic_cast<MiB*>(MiA*) 结果 = %s，"
                "与对象差 = %+ld —— 运行时算出的调整量 == 布局常量 +16 ✓\n",
                fmtAddr(dynB).c_str(), off(dynB, &obj));

    EXPECT_EQ(sizeof(MiC), 32u);
    EXPECT_EQ(off(&obj.a, &obj), 8);
    EXPECT_EQ(off(&obj.b, &obj), 24);
    EXPECT_EQ(off(&obj.c, &obj), 28);
    EXPECT_EQ(off(asA, &obj), 0);
    EXPECT_EQ(off(asB, &obj), 16);
    ASSERT_NE(dynB, nullptr);
    EXPECT_EQ(off(dynB, &obj), 16);      // 运行时调整量 == 编译期布局常量
    // 黄金值：第二张表 offset-to-top = -16（thunk 的调整依据）
    EXPECT_EQ(*(static_cast<long*>(vptrB) - 2), -16);
}

// ─────────────────────────────────────────────────────────────────────────────
// B5. 菱形虚继承 → 共享子对象被推到对象尾部，经由头指针表里的距离槽间接寻址
//
//   struct DiaB { int v; virtual ~DiaB(){} };
//   struct DiaD : virtual DiaB { int d; };
//   struct DiaE : virtual DiaB { int e; };
//   struct DiaX : DiaD, DiaE { int x; };
//
//   推演：若按普通拼接，B 会出现两次——虚继承要求唯一，只能把 B 挪到
//   所有路径都"够得着"的位置：对象尾部，用表内距离槽记录"离我多远"。
//   DiaB 本身是多态的 → D 部分、E 部分的头指针 = vptr + vbptr【合一】。
// ─────────────────────────────────────────────────────────────────────────────
TEST(LayoutSketch, DiamondVirtualBaseAtTail) {
    registerAllRttiSymbols();
    DiaX obj; obj.d = 1; obj.e = 2; obj.x = 3; obj.v = 4;
    DiaB* asB = &obj;                        // 虚基类指针调整
    DiaD* asD = &obj;

    char* base = reinterpret_cast<char*>(&obj);
    void* vD = *reinterpret_cast<void**>(base);        // D 部分头指针
    void* vE = *reinterpret_cast<void**>(base + 16);   // E 部分头指针
    void* vB = *reinterpret_cast<void**>(base + 32);   // 共享 DiaB 的 vptr

    drawObjectMap("struct DiaX : DiaD, DiaE { int x; } —— 菱形虚继承",
                  {{"vptr_D", 0, 8, fmtAddr(vD) + " → " + symName(vD)},
                   {"d", 8, 4, "1（DiaD 成员）"},
                   {"pad", 12, 4, "填充"},
                   {"vptr_E", 16, 8, fmtAddr(vE) + " → " + symName(vE)},
                   {"e", 24, 4, "2（DiaE 成员）"},
                   {"x", 28, 4, "3（DiaX 自有）"},
                   {"[共享B]vptr", 32, 8, fmtAddr(vB) + " → " + symName(vB)},
                   {"[共享B]v", 40, 4, "4（★全对象唯一一份）"},
                   {"[共享B]pad", 44, 4, "填充"}},
                  sizeof(DiaX));
    std::printf("  ★ 三个头指针：D 部分@+0、E 部分@+16、共享 B@+32。\n"
                "    DiaX→DiaD* 差 = %ld；DiaX→DiaB* 差 = %+ld"
                "（指向尾部共享子对象）\n", off(asD, &obj), off(asB, &obj));

    std::printf("\n  ── 三张子虚表逐槽解剖（[-3] = vbase 距离槽）──\n");
    std::printf("\n  [表1] D 部分主表 @+0：\n");
    drawVTable("DiaX-D", vD, 2, /*showVbase=*/true);
    std::printf("\n  [表2] E 部分次表 @+16：\n");
    drawVTable("DiaX-E", vE, 2, /*showVbase=*/true);
    std::printf("\n  [表3] 共享 DiaB 子表 @+32：\n");
    drawVTable("DiaX-B", vB, 2, /*showVbase=*/true);

    std::printf("  ★ 三点观察：\n"
                "    ① [-3] vbase 距离：D 部分(+0)→共享B(+32) = 32；\n"
                "       E 部分(+16)→共享B(+32) = 16 —— vbptr 间接寻址的距离就存这里；\n"
                "    ② [-2] offset-to-top：主表 0，E 次表 -16，共享 B 子表 -32；\n"
                "    ③ 三张表 [-1] 全是 typeinfo for DiaX（最派生类！）——\n"
                "       所以从任何子对象出发读 typeid，拿到的都是 DiaX。\n");
    std::printf("\n  ── typeinfo 解剖（菱形 → __vmi）──\n");
    drawTypeInfo(tiFromHead(vD));

    EXPECT_EQ(sizeof(DiaB), 16u);
    EXPECT_EQ(sizeof(DiaD), 32u);    // vptr 8 + d 4 + pad 4 + 共享 B 16
    EXPECT_EQ(sizeof(DiaX), 48u);
    EXPECT_EQ(off(&obj.d, &obj), 8);
    EXPECT_EQ(off(&obj.e, &obj), 24);
    EXPECT_EQ(off(&obj.x, &obj), 28);
    EXPECT_EQ(off(&obj.v, &obj), 40);    // v 是共享 B 的成员（子对象 +8）
    EXPECT_EQ(off(asD, &obj), 0);
    EXPECT_EQ(off(asB, &obj), 32);       // B 子对象起点在 +32，成员 v 在 +40
    // 黄金值：三张表的 vbase 距离槽 / offset-to-top
    EXPECT_EQ(*(static_cast<long*>(vD) - 3), 32);
    EXPECT_EQ(*(static_cast<long*>(vE) - 3), 16);
    EXPECT_EQ(*(static_cast<long*>(vB) - 2), -32);
    // 三张表的 typeinfo 槽全指向同一个 typeinfo for DiaX
    EXPECT_EQ(tiFromHead(vD), tiFromHead(vE));
    EXPECT_EQ(tiFromHead(vD), tiFromHead(vB));
    EXPECT_EQ(tiFromHead(vD), static_cast<const void*>(&typeid(obj)));
}

// ─────────────────────────────────────────────────────────────────────────────
// B6. 虚继承但没有虚函数 → 头部 8 字节照样存在（纯 _vbptr）
//
//   struct ViV { int v; };                        ← 无虚函数
//   struct ViW : virtual ViV { int w; };          ← 虚继承
//
//   推演（易错）：没有虚函数 = 没有头部指针？
//   实测：错。虚继承的寻址需要一个锚点，ABI 仍在头部放 8 字节 _vbptr，
//   指向"虚基类表"——表内结构与 vtable 神似：[-3]=到共享虚基的距离，
//   [-2]=offset-to-top，[-1]=typeinfo。（虚继承 + 虚函数同现时两者合一。）
// ─────────────────────────────────────────────────────────────────────────────
TEST(LayoutSketch, VirtualInheritanceWithoutVirtualFuncsStillPaysHeader) {
    registerAllRttiSymbols();
    ViW obj; obj.w = 1; obj.v = 2;
    void* head = *reinterpret_cast<void**>(&obj);

    drawObjectMap("struct ViW : virtual ViV { int w; } —— 虚继承、无虚函数",
                  {{"_vbptr", 0, 8, fmtAddr(head) + "（非空！虚继承锚点）"},
                   {"w", 8, 4, "1"},
                   {"ViV::v", 12, 4, "2（共享虚基子对象，紧贴尾部，仅 4B）"}},
                  sizeof(ViW));

    // vbptr 指向的表逐槽解剖（dladdr 常把该地址报成 VTT 符号：
    // 无虚函数时虚基距离表与 VTT 恰好紧邻/重合）
    const long* vt = static_cast<const long*>(head);
    std::printf("┌─ _vbptr 指向的表（dladdr 识别为 %s）\n", symName(head).c_str());
    std::printf("│ [-3] = %-4ld ★到共享虚基 ViV 的距离：头部(+0) → v(+12) = 12 ✓\n", vt[-3]);
    std::printf("│ [-2] = %-4ld offset-to-top\n", vt[-2]);
    std::printf("│ [-1] = %s ← typeinfo 指针（无虚函数也有！）\n",
                symName(reinterpret_cast<const void*>(vt[-1])).c_str());
    std::printf("└─\n");
    std::printf("  ── typeinfo 解剖（含虚基类 → __vmi，base_count=1）──\n");
    drawTypeInfo(reinterpret_cast<const void*>(vt[-1]));

    std::printf("  ★ 结论：虚继承【总是】付出 8B 头 + 一张表；"
                "虚函数只是最常见的额外开销，不是唯一来源。\n");
    EXPECT_EQ(sizeof(ViW), 16u);
    EXPECT_NE(head, nullptr);
    EXPECT_EQ(off(&obj.w, &obj), 8);
    EXPECT_EQ(off(&obj.v, &obj), 12);
    EXPECT_EQ(vt[-3], 12);                       // 黄金值：vbase 距离 = 12
    EXPECT_EQ(vt[-2], 0);
    EXPECT_EQ(reinterpret_cast<const void*>(vt[-1]),
              static_cast<const void*>(&typeid(obj)));
}

// ─────────────────────────────────────────────────────────────────────────────
// B7. vptr 的"每类一份、每对象一拷贝" + vtable[-1] → typeinfo 物理链条
//
//   对象 ──(首8字节)──► vptr ──(-8)──► typeinfo
//   同类的两个对象：vptr 值相同（指向同一张表）
//   不同类：vptr 值不同；vtable[-1] 恰好是 &typeid(对象)
// ─────────────────────────────────────────────────────────────────────────────
TEST(VtableRtti, VptrPerClassAndTypeinfoChain) {
    registerAllRttiSymbols();
    Poly p1, p2;
    Dog d1, d2;
    void* vptrP1 = *reinterpret_cast<void**>(&p1);
    void* vptrP2 = *reinterpret_cast<void**>(&p2);
    void* vptrD1 = *reinterpret_cast<void**>(&d1);
    void* vptrD2 = *reinterpret_cast<void**>(&d2);

    // vtable[-1] = typeinfo 地址，与 &typeid(静态引用) 恒等
    void* tiViaVtable = *(reinterpret_cast<void**>(vptrD1) - 1);
    const void* tiDirect = &typeid(d1);

    std::printf("\n[链条图形]（全部实测地址）\n"
                "  p1 @ %s ──► vptr %s ──► %s\n"
                "  p2 @ %s ──► vptr %s          ← 同类同表（p1==p2）\n"
                "  d1 @ %s ──► vptr %s ──► %s\n"
                "  d2 @ %s ──► vptr %s          ← 同类同表（d1==d2）\n"
                "  vtable[-1] 读出 = %s\n"
                "  &typeid(d1)   = %s  → 两者%s\n",
                fmtAddr(&p1).c_str(), fmtAddr(vptrP1).c_str(), symName(vptrP1).c_str(),
                fmtAddr(&p2).c_str(), fmtAddr(vptrP2).c_str(),
                fmtAddr(&d1).c_str(), fmtAddr(vptrD1).c_str(), symName(vptrD1).c_str(),
                fmtAddr(&d2).c_str(), fmtAddr(vptrD2).c_str(),
                fmtAddr(tiViaVtable).c_str(), fmtAddr(tiDirect).c_str(),
                tiViaVtable == tiDirect ? "恒等 ✓（链条闭环）" : "不等 ✗");

    EXPECT_EQ(vptrP1, vptrP2);              // 同类共享一张表
    EXPECT_EQ(vptrD1, vptrD2);
    EXPECT_NE(vptrP1, vptrD1);              // 不同类不同表
    EXPECT_EQ(tiViaVtable, tiDirect);       // ★ 链条闭环：对象 → vptr → -8 → typeinfo
}

// ─────────────────────────────────────────────────────────────────────────────
// B8. typeinfo 按继承拓扑三选一：__class / __si_class / __vmi
//
//   无基类      → __class_type_info      （链的起点）
//   单个非虚基类 → __si_class_type_info   （链的中间，一个基类指针）
//   多/虚基类   → __vmi_class_type_info  （图，基类数组）
// ─────────────────────────────────────────────────────────────────────────────
TEST(VtableRtti, TypeinfoTopologyThreeFlavors) {
    registerAllRttiSymbols();
    std::string namePlain = typeid(typeid(Plain)).name();
    std::string nameDog   = typeid(typeid(Dog)).name();
    std::string nameMiC   = typeid(typeid(MiC)).name();
    std::string nameDiaX  = typeid(typeid(DiaX)).name();

    std::printf("\n[拓扑→形态 对照]（typeid 的运行期类别，已解缠）\n");
    auto show = [](const char* cls, const void* ti, const std::string& mangled) {
        int st = 0;
        char* d = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &st);
        std::printf("  %-6s → %s\n", cls, (st == 0 && d) ? d : mangled.c_str());
        std::free(d);
        (void)ti;
    };
    show("Plain", &typeid(Plain), namePlain);
    show("Dog", &typeid(Dog), nameDog);
    show("MiC", &typeid(MiC), nameMiC);
    show("DiaX", &typeid(DiaX), nameDiaX);
    std::printf("  （菱形 DiaX 也是 __vmi：只要继承是\"图\"就用数组形态）\n");

    // 无基类 → 基础 __class_type_info（名字不含 __si/__vmi 前缀）
    EXPECT_NE(namePlain.find("__class_type_info"), std::string::npos);
    EXPECT_EQ(namePlain.find("__si_class_type_info"), std::string::npos);
    EXPECT_EQ(namePlain.find("__vmi_class_type_info"), std::string::npos);
    // 单继承 → __si
    EXPECT_NE(nameDog.find("__si_class_type_info"), std::string::npos);
    // 多继承 / 虚继承 → __vmi（菱形也是 __vmi！）
    EXPECT_NE(nameMiC.find("__vmi_class_type_info"), std::string::npos);
    EXPECT_NE(nameDiaX.find("__vmi_class_type_info"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// B9. 逐字节解码 __vmi_class_type_info：base_count + offset_flags
//
//   Itanium ABI 编码（<cxxabi.h> __offset_flags_masks）：
//     bit0 = __virtual_mask   bit1 = __public_mask
//     offset = raw >> 8（__offset_shift）
//
//   MiC 的 typeinfo（libc++/libstdc++ 内部布局同构，用手写复刻体读取）：
//     base_count = 2
//     bases[0]: MiA  raw=0x0002 → 偏移 0,  public, 非 virtual
//     bases[1]: MiB  raw=0x1002 → 偏移 16, public, 非 virtual
//                                    ▲
//                          与 B4 实测的 MiB 子对象偏移 +16 严丝合缝
// ─────────────────────────────────────────────────────────────────────────────
namespace {
// 手写复刻 Itanium ABI 结构（不依赖 <cxxabi.h>，libc++/libstdc++ 发射字节同构）。
// 注意字段宽度：flags 与 base_count 是【两个连续的 u32】（偏移 16 / 20），
// 基类数组紧随其后从偏移 24 开始——用错宽度会把 base[0] 的指针读成
// base_count（曾经踩过的坑）。
struct ReplicaBase { const void* base_ti; unsigned long offset_flags; };
struct ReplicaVmi {
    const void* vptr;
    const char* name;
    unsigned int flags;        // +16（u32）
    unsigned int base_count;   // +20（u32）
    ReplicaBase bases[8];      // +24 起，每元素 16 字节
};
} // namespace

TEST(VmiDecode, BaseCountAndOffsetFlags) {
    registerAllRttiSymbols();
    const auto* ti = reinterpret_cast<const ReplicaVmi*>(&typeid(MiC));

    // ── 整个 __vmi 结构的内存图（逐字段实测）──────────────────────────
    drawObjectMap("typeinfo for MiC（__vmi_class_type_info）内存图",
                  {{"vptr", 0, 8, fmtAddr(ti->vptr) + " → " + symName(ti->vptr)},
                   {"name", 8, 8, std::string("\"") + ti->name + "\"（mangled）"},
                   {"flags", 16, 4, std::to_string(ti->flags) + "（u32）"},
                   {"base_count", 20, 4, std::to_string(ti->base_count) + "（u32，★与 flags 打包在 8B 槽里）"},
                   {"bases[0].ti", 24, 8,
                    fmtAddr(ti->bases[0].base_ti) + " → typeinfo for MiA"},
                   {"bases[0].of", 32, 8,
                    fmtHex(ti->bases[0].offset_flags) + " → 偏移 0, public"},
                   {"bases[1].ti", 40, 8,
                    fmtAddr(ti->bases[1].base_ti) + " → typeinfo for MiB"},
                   {"bases[1].of", 48, 8,
                    fmtHex(ti->bases[1].offset_flags) + " → 偏移 16, public"}},
                  24 + 16 * ti->base_count);

    ASSERT_EQ(ti->base_count, 2u);
    std::printf("[解码] offset_flags: bit0=virtual bit1=public 高位>>8=偏移：\n");
    for (unsigned long i = 0; i < ti->base_count; ++i) {
        unsigned long raw = ti->bases[i].offset_flags;
        std::printf("  base[%lu]: raw=%s → 偏移=%lu public=%d virtual=%d\n", i,
                    fmtHex(raw).c_str(), raw >> 8,
                    static_cast<int>((raw >> 1) & 1), static_cast<int>(raw & 1));
    }
    std::printf("  ★ bases[1] 偏移 16 == B4 实测 MiB 子对象偏移 +16"
                " —— dynamic_cast 跨基类调整量的出处就是这里。\n");

    // bases[0] = MiA：偏移 0、public
    EXPECT_EQ(ti->bases[0].offset_flags >> 8, 0u);
    EXPECT_TRUE(ti->bases[0].offset_flags & 2);
    // bases[1] = MiB：偏移 16、public —— 与 B4 的布局实测互证
    EXPECT_EQ(ti->bases[1].offset_flags >> 8, 16u);
    EXPECT_TRUE(ti->bases[1].offset_flags & 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// B10. dynamic_cast 指针调整全家桶（多继承视角，[expr.dynamic.cast]/8）
//
//   ① upcast   MiC* → MiA*  ：+0（第一个基类在头部）
//   ② crosscast MiA* → MiB* ：+16（兄弟子对象之间横跳）
//   ③ downcast  MiA* → MiC* ：回到对象起点（最派生类型匹配）
//   ④ 错兄弟    SibB* → SibC* ：nullptr（运行时沿链找不到）
// ─────────────────────────────────────────────────────────────────────────────
TEST(DynamicCastAdjust, PointerAdjustmentFamily) {
    registerAllRttiSymbols();
    MiC obj;
    MiA* asA = &obj;

    MiA* up = dynamic_cast<MiA*>(asA);
    MiB* cross = dynamic_cast<MiB*>(asA);     // ★ 横跳：需要 typeinfo 指路
    MiC* down = dynamic_cast<MiC*>(asA);

    std::printf("\n┌─ dynamic_cast 指针调整全家桶（对象 MiC @ %s）\n",
                fmtAddr(&obj).c_str());
    std::printf("│ 方向        转型                 结果地址           调整量\n");
    std::printf("│ ----------  -------------------  -----------------  --------\n");
    std::printf("│ upcast      MiA* → MiA*          %-17s  %+ld（头部即基类）\n",
                fmtAddr(up).c_str(), off(up, &obj));
    std::printf("│ crosscast   MiA* → MiB*          %-17s  %+ld（★typeinfo 指路横跳）\n",
                fmtAddr(cross).c_str(), off(cross, &obj));
    std::printf("│ downcast    MiA* → MiC*          %-17s  %+ld（匹配最派生后回原点）\n",
                fmtAddr(down).c_str(), off(down, &obj));
    EXPECT_EQ(off(up, &obj), 0);
    ASSERT_NE(cross, nullptr);
    EXPECT_EQ(off(cross, &obj), 16);
    ASSERT_NE(down, nullptr);
    EXPECT_EQ(off(down, &obj), 0);

    // 错兄弟：B 对象转不成 C（运行时失败，非崩溃）
    SibB bObj;
    SibA* base = &bObj;
    SibC* wrong = dynamic_cast<SibC*>(base);
    std::printf("│ 错兄弟      SibB 对象 → SibC*    %-17s  安全失败（沿链找不到）\n",
                wrong == nullptr ? "(nullptr)" : "(非空!错误)");
    std::printf("└─\n");
    EXPECT_EQ(wrong, nullptr);
}

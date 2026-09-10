// =============================================================================
// typeinfo_test_suite.cpp —— Google Test 风格，8 个独立可点测试用例
// =============================================================================
// 编译: clang++-18 -std=c++20 -g typeinfo_test_suite.cpp -o typeinfo_test_suite -ldl -lgtest -lgtest_main -pthread
// 运行: ./typeinfo_test_suite              （跑全部）
//       ./typeinfo_test_suite --gtest_filter="*Layout*"    （只跑布局）
//       ./typeinfo_test_suite --gtest_filter="*CrossCast*" （只跑跨转型）
// 调试: gdb ./typeinfo_test_suite
//       (gdb) break typeinfo_test.cc:160
//       (gdb) run --gtest_filter="*EndToEnd*"
//
// 理论背景：
//   Itanium C++ ABI 定义了三种 typeinfo 形态，用于支持 dynamic_cast 和 RTTI：
//   ① __class_type_info      无基类（链终点，仅 16 字节头部）
//   ② __si_class_type_info   单基类（单链表，24 字节 = 头部 + base 指针）
//   ③ __vmi_class_type_info  多继承（变长，头部 + 基类数组 + 偏移编码）
//
//   dynamic_cast 算法核心：
//     1. 读对象 vptr[-2] = offset-to-top（负偏移，指向最派生对象顶部）
//     2. top = obj + ott（归顶）
//     3. 读 vptr[-1] = 最派生 typeinfo
//     4. DFS 遍历 typeinfo 基类图，累计子对象偏移
//     5. 命中 → 返回 top + 累计偏移；未命中 → nullptr
// =============================================================================

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <typeinfo>
#include <gtest/gtest.h>

// ─────────────────────────────────────────────────────────────────────────────
// 一、类型定义（Itanium ABI 三种形态 + 工具函数）
// ─────────────────────────────────────────────────────────────────────────────

// 形态哨兵
static const char FORM_CLASS = 'C';
static const char FORM_SI    = 'S';
static const char FORM_VMI   = 'V';

// 公共头部（所有 typeinfo 的前 16 字节）
struct type_info {
    const void*  vptr;
    const char*  name;
};

// 形态①：无基类（16B，链终点）
struct __class_type_info : type_info {};

// 形态②：单一公有基类（24B，单链表）
struct __si_class_type_info : type_info {
    const type_info* base;
};

// 形态③：多继承（变长，基类数组）
struct __vmi_class_type_info : type_info {
    unsigned base_count;
    struct base_info {
        const type_info* ti;
        long offset_flags;   // 低 2 位标志 | 偏移<<8
    } bases[4];

    static constexpr long PUBLIC_FLAG = 0x1;
    static long offset_of(long f)  { return f >> 8; }
    static bool is_public(long f)  { return f & PUBLIC_FLAG; }
    static long encode(long offset, bool pub) {
        return (offset << 8) | (pub ? PUBLIC_FLAG : 0);
    }
};

// FakeVTable：模拟 vptr[-2]=offset-to-top, vptr[-1]=typeinfo
struct FakeVTable {
    long             ott;
    const type_info* ti;
    const void*      fn0;
};

static const void** make_vtable(long ott, const type_info* ti) {
    static FakeVTable tables[32];
    static int n = 0;
    tables[n] = {ott, ti, nullptr};
    return &tables[n++].fn0;
}

// DFS 搜索
static const type_info* dfs_find(const type_info* ti, const type_info* target,
                                 long acc, long& out_acc) {
    if (ti == target) { out_acc = acc; return ti; }
    char form = *(const char*)ti->vptr;
    if (form == FORM_SI) {
        auto si = static_cast<const __si_class_type_info*>(ti);
        return dfs_find(si->base, target, acc + 0, out_acc);
    }
    if (form == FORM_VMI) {
        auto vmi = static_cast<const __vmi_class_type_info*>(ti);
        for (unsigned i = 0; i < vmi->base_count; ++i) {
            long off = __vmi_class_type_info::offset_of(vmi->bases[i].offset_flags);
            const type_info* hit =
                dfs_find(vmi->bases[i].ti, target, acc + off, out_acc);
            if (hit) return hit;
        }
    }
    return nullptr;
}

// dynamic_cast 模拟
static void* mi_dynamic_cast(void* obj, const type_info* target) {
    if (!obj || !target) return nullptr;
    const void** vptr = *(const void***)obj;
    long ott            = (long)vptr[-2];
    const type_info* ti = (const type_info*)vptr[-1];
    char* top = (char*)obj + ott;
    long acc = 0;
    if (dfs_find(ti, target, 0, acc)) return top + acc;
    return nullptr;
}

// 辅助打印
static void print_typeinfo(const type_info* ti, const char* label) {
    char form = *(const char*)ti->vptr;
    printf("    %s @%p: 形态='%c' 名字=\"%s\"", label, (const void*)ti, form, ti->name);
    if (form == FORM_SI) {
        auto si = static_cast<const __si_class_type_info*>(ti);
        printf("  base→\"%s\"", si->base->name);
    } else if (form == FORM_VMI) {
        auto vmi = static_cast<const __vmi_class_type_info*>(ti);
        printf("  基类数=%u [", vmi->base_count);
        for (unsigned i = 0; i < vmi->base_count; ++i)
            printf("%s\"%s\"@%ld", i ? ", " : "", vmi->bases[i].ti->name,
                   __vmi_class_type_info::offset_of(vmi->bases[i].offset_flags));
        printf("]");
    }
    printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// 二、测试用例（每个 TEST 可独立运行、独立调试）
// ─────────────────────────────────────────────────────────────────────────────

// ---------------------------------------------------------------------------
// T1: 三种形态的 sizeof 与槽布局验证
// ---------------------------------------------------------------------------
TEST(TypeInfoTest, T1_Layout) {
    printf("[T1] 三种形态的槽布局（8 字节指针，LP64）\n");

    printf("    sizeof(type_info)              = %zu\n", sizeof(type_info));
    printf("    sizeof(__class_type_info)      = %zu\n", sizeof(__class_type_info));
    printf("    sizeof(__si_class_type_info)   = %zu\n", sizeof(__si_class_type_info));
    printf("    sizeof(__vmi_class_type_info)  = %zu\n", sizeof(__vmi_class_type_info));

    EXPECT_EQ(sizeof(type_info), 16u)
        << "type_info 公共头部 = 16B（2 槽：vptr + name）";
    EXPECT_EQ(sizeof(__class_type_info), 16u)
        << "__class 形态 = 公共头部 = 16B（无附加字段，链终点）";
    EXPECT_EQ(sizeof(__si_class_type_info), 24u)
        << "__si 形态 = 24B（3 槽：头部 16B + base 指针 8B）";

    // 构造实例逐槽打印
    __class_type_info ti{{&FORM_CLASS, "Solo"}};
    auto p = reinterpret_cast<const uint64_t*>(&ti);
    printf("    __class_type_info 槽布局图：\n");
    printf("      +0  vptr(形态) = %p (哨兵='%c')\n",
           (const void*)p[0], *(const char*)p[0]);
    printf("      +8  name       = \"%s\"\n", (const char*)p[1]);

    EXPECT_EQ(*(const char*)p[0], FORM_CLASS)
        << "+0 槽读出形态标记 'C' = __class_type_info";
    EXPECT_STREQ((const char*)p[1], "Solo")
        << "+8 槽读出类型名 \"Solo\"";
}

// ---------------------------------------------------------------------------
// T2: __si 单链表 —— D → A 的 typeinfo 上溯
// ---------------------------------------------------------------------------
TEST(TypeInfoTest, T2_SiChain) {
    printf("[T2] __si 单链表：D : public A\n");

    __class_type_info ti_A{{&FORM_CLASS, "A"}};
    __si_class_type_info ti_D{{&FORM_SI, "D"}, &ti_A};
    print_typeinfo(&ti_A, "ti_A");
    print_typeinfo(&ti_D, "ti_D");

    long acc = 0;
    const type_info* hit = dfs_find(&ti_D, &ti_A, 0, acc);
    printf("    dfs_find(ti_D, ti_A) → %s (acc=%ld)\n",
           hit ? hit->name : "nullptr", acc);
    EXPECT_EQ(hit, &ti_A) << "D → A 命中：沿 base 指针找到 ti_A";
    EXPECT_EQ(acc, 0) << "单继承路径偏移累计 = 0";

    acc = 0;
    hit = dfs_find(&ti_D, &ti_D, 0, acc);
    printf("    dfs_find(ti_D, ti_D) → %s (acc=%ld)\n",
           hit ? hit->name : "nullptr", acc);
    EXPECT_EQ(hit, &ti_D) << "D → D 自身命中：起始即目标";
    EXPECT_EQ(acc, 0) << "自身命中偏移 = 0";

    __class_type_info ti_X{{&FORM_CLASS, "X"}};
    acc = 0;
    hit = dfs_find(&ti_D, &ti_X, 0, acc);
    printf("    dfs_find(ti_D, ti_X) → %s\n",
           hit ? hit->name : "nullptr");
    EXPECT_EQ(hit, nullptr) << "无关类 X 未命中 → 到达 __class 链终点返回 nullptr";

    acc = 0;
    hit = dfs_find(&ti_A, &ti_D, 0, acc);
    printf("    dfs_find(ti_A, ti_D) → %s\n",
           hit ? hit->name : "nullptr");
    EXPECT_EQ(hit, nullptr) << "从 ti_A 出发找不到 ti_D（继承链只能沿基类方向走）";
}

// ---------------------------------------------------------------------------
// T3: __vmi 基类数组 —— D : A@0, B@16
// ---------------------------------------------------------------------------
TEST(TypeInfoTest, T3_VmiArray) {
    printf("[T3] __vmi 基类数组：D : A@0, B@16\n");

    __class_type_info ti_A{{&FORM_CLASS, "A"}};
    __class_type_info ti_B{{&FORM_CLASS, "B"}};
    __vmi_class_type_info ti_D{{&FORM_VMI, "D"}, 2,
        {{&ti_A, __vmi_class_type_info::encode(0, true)},
         {&ti_B, __vmi_class_type_info::encode(16, true)},
         {}, {}}};

    print_typeinfo(&ti_A, "ti_A");
    print_typeinfo(&ti_B, "ti_B");
    print_typeinfo(&ti_D, "ti_D");

    for (unsigned i = 0; i < ti_D.base_count; ++i) {
        printf("    bases[%u]: ti→\"%s\"  offset_flags=0x%lx  (偏移=%ld, public=%d)\n",
               i, ti_D.bases[i].ti->name, ti_D.bases[i].offset_flags,
               __vmi_class_type_info::offset_of(ti_D.bases[i].offset_flags),
               __vmi_class_type_info::is_public(ti_D.bases[i].offset_flags));
    }

    long acc = 0;
    const type_info* hit = dfs_find(&ti_D, &ti_B, 0, acc);
    printf("    dfs_find(ti_D, ti_B) → %s (acc=%ld)\n",
           hit ? hit->name : "nullptr", acc);
    EXPECT_EQ(hit, &ti_B) << "D → B 命中：遍历 bases[1] 找到 ti_B";
    EXPECT_EQ(acc, 16) << "命中 B 时累计偏移 = 16";

    acc = 0;
    hit = dfs_find(&ti_D, &ti_A, 0, acc);
    printf("    dfs_find(ti_D, ti_A) → %s (acc=%ld)\n",
           hit ? hit->name : "nullptr", acc);
    EXPECT_EQ(hit, &ti_A) << "D → A 命中：遍历 bases[0] 找到 ti_A";
    EXPECT_EQ(acc, 0) << "命中 A（主基类）累计偏移 = 0";

    __class_type_info ti_X{{&FORM_CLASS, "X"}};
    acc = 0;
    hit = dfs_find(&ti_D, &ti_X, 0, acc);
    printf("    dfs_find(ti_D, ti_X) → %s\n",
           hit ? hit->name : "nullptr");
    EXPECT_EQ(hit, nullptr) << "无关类 X 未命中";
}

// ---------------------------------------------------------------------------
// T4: 端到端 —— 模拟对象 + 双虚表 + offset-to-top 归顶 + dynamic_cast
// ---------------------------------------------------------------------------
struct MiObj {
    const void** vptr_A;   // +0
    long         a;        // +8
    const void** vptr_B;   // +16
    long         b;        // +24
    long         c;        // +32
};

TEST(TypeInfoTest, T4_EndToEnd) {
    printf("[T4] 端到端：MiC : MiA, MiB（模拟对象 + 双虚表 + 归顶）\n");

    __class_type_info ti_A{{&FORM_CLASS, "MiA"}};
    __class_type_info ti_B{{&FORM_CLASS, "MiB"}};
    __vmi_class_type_info ti_C{{&FORM_VMI, "MiC"}, 2,
        {{&ti_A, __vmi_class_type_info::encode(0, true)},
         {&ti_B, __vmi_class_type_info::encode(16, true)},
         {}, {}}};
    print_typeinfo(&ti_C, "ti_C");

    const void** main_vtable = make_vtable(0,   &ti_C);
    const void** sub_vtable  = make_vtable(-16, &ti_C);
    MiObj obj{main_vtable, 111, sub_vtable, 222, 333};

    printf("    对象 @%p\n", (void*)&obj);
    printf("      +0  vptr_A = %p  (vptr_A[-2]=%ld, vptr_A[-1]=ti_C)\n",
           (void*)obj.vptr_A, (long)obj.vptr_A[-2]);
    printf("      +8  a      = %ld\n", obj.a);
    printf("      +16 vptr_B = %p  (vptr_B[-2]=%ld, vptr_B[-1]=ti_C)\n",
           (void*)obj.vptr_B, (long)obj.vptr_B[-2]);
    printf("      +24 b      = %ld\n", obj.b);
    printf("      +32 c      = %ld\n", obj.c);

    void* via_A = mi_dynamic_cast(&obj, &ti_B);
    printf("    mi_dynamic_cast(&obj, &ti_B) → %p (期望=%p)\n",
           via_A, (char*)&obj + 16);
    EXPECT_EQ(via_A, (char*)&obj + 16)
        << "经 A* 转 B*：归顶(ott=0) → DFS 找到 B → 返回 top+16";

    void* via_B = mi_dynamic_cast((char*)&obj + 16, &ti_A);
    printf("    mi_dynamic_cast(obj+16, &ti_A) → %p (期望=%p)\n",
           via_B, &obj);
    EXPECT_EQ(via_B, &obj)
        << "经 B* 转 A*：vptr_B[-2]=-16 归顶到 &obj → DFS 找到 A → 返回 top+0";

    void* self = mi_dynamic_cast((char*)&obj + 16, &ti_C);
    printf("    mi_dynamic_cast(obj+16, &ti_C) → %p (期望=%p)\n",
           self, &obj);
    EXPECT_EQ(self, &obj)
        << "经 B* 转最派生 MiC*：归顶后 DFS 自身命中 → 返回 top";
}

// ---------------------------------------------------------------------------
// T5: 兄弟类拒绝 —— 无公共可达路径时返回 nullptr
// ---------------------------------------------------------------------------
TEST(TypeInfoTest, T5_SiblingReject) {
    printf("[T5] 兄弟类拒绝：A 与 B 互为兄弟，不可互转\n");

    __class_type_info ti_A{{&FORM_CLASS, "A"}};
    __class_type_info ti_B{{&FORM_CLASS, "B"}};
    __class_type_info ti_X{{&FORM_CLASS, "X"}};
    __vmi_class_type_info ti_D{{&FORM_VMI, "D"}, 2,
        {{&ti_A, __vmi_class_type_info::encode(0, true)},
         {&ti_B, __vmi_class_type_info::encode(16, true)},
         {}, {}}};
    print_typeinfo(&ti_D, "ti_D");

    const void** vt = make_vtable(0, &ti_D);
    struct { const void** vp; long pad; const void** vp2; long pad2; long pad3; }
        obj{vt, 0, vt, 0, 0};

    void* result = mi_dynamic_cast(&obj, &ti_X);
    printf("    mi_dynamic_cast(obj, &ti_X) → %p\n", result);
    EXPECT_EQ(result, nullptr) << "无关类 X → nullptr（不在继承图中）";

    long acc = 0;
    const type_info* hit = dfs_find(&ti_A, &ti_B, 0, acc);
    printf("    dfs_find(ti_A, ti_B) → %s\n",
           hit ? hit->name : "nullptr");
    EXPECT_EQ(hit, nullptr)
        << "从 ti_A 出发找不到 ti_B —— 继承链只能沿基类方向走，兄弟不可达";

    acc = 0;
    hit = dfs_find(&ti_B, &ti_A, 0, acc);
    printf("    dfs_find(ti_B, ti_A) → %s\n",
           hit ? hit->name : "nullptr");
    EXPECT_EQ(hit, nullptr)
        << "从 ti_B 出发找不到 ti_A —— 兄弟关系是对称的";
}

// ---------------------------------------------------------------------------
// T6: 跨转型（cross-cast）—— A* → B* 经共同派生类 D
// ---------------------------------------------------------------------------
struct MiObj24 {
    const void** vptr_A;   // +0
    long         pad1;     // +8
    long         pad2;     // +16
    const void** vptr_B;   // +24
    long         pad3;     // +32
};

TEST(TypeInfoTest, T6_CrossCast) {
    printf("[T6] 跨转型 A* → B*（经共同派生类 D，运行时完成）\n");

    __class_type_info ti_A{{&FORM_CLASS, "A"}};
    __class_type_info ti_B{{&FORM_CLASS, "B"}};
    __vmi_class_type_info ti_D{{&FORM_VMI, "D"}, 2,
        {{&ti_A, __vmi_class_type_info::encode(0, true)},
         {&ti_B, __vmi_class_type_info::encode(24, true)},
         {}, {}}};
    print_typeinfo(&ti_D, "ti_D");

    const void** main_vt = make_vtable(0,   &ti_D);
    const void** sub_vt  = make_vtable(-24, &ti_D);
    MiObj24 obj{main_vt, 0, 0, sub_vt, 0};

    printf("    对象 @%p: vptr_A=%p(ott=%ld) vptr_B=%p(ott=%ld)\n",
           (void*)&obj, (void*)obj.vptr_A, (long)obj.vptr_A[-2],
           (void*)obj.vptr_B, (long)obj.vptr_B[-2]);

    void* a_ptr = &obj;
    void* b = mi_dynamic_cast(a_ptr, &ti_B);
    printf("    mi_dynamic_cast(A*, &ti_B) → %p (期望=%p = obj+24)\n",
           b, (char*)&obj + 24);
    EXPECT_EQ(b, (char*)&obj + 24)
        << "A* → B*：归顶(ott=0) → DFS 找到 B@24 → 返回 top+24";

    void* back = mi_dynamic_cast(b, &ti_A);
    printf("    mi_dynamic_cast(B*, &ti_A) → %p (期望=%p = obj)\n",
           back, &obj);
    EXPECT_EQ(back, &obj)
        << "B* → A*：归顶(ott=-24 → top=&obj) → DFS 找到 A@0 → 返回 top+0";

    void* down = mi_dynamic_cast(a_ptr, &ti_D);
    printf("    mi_dynamic_cast(A*, &ti_D) → %p (期望=%p = obj)\n",
           down, &obj);
    EXPECT_EQ(down, &obj)
        << "A* → D*（下溯）：归顶后 DFS 自身命中 → 返回 top";
}

// ---------------------------------------------------------------------------
// T7: offset_flags 位编码 —— 低 2 位标志，>>8 偏移
// ---------------------------------------------------------------------------
TEST(TypeInfoTest, T7_OffsetFlags) {
    printf("[T7] offset_flags 位编码：flags=低2位 | 偏移<<8（Itanium ABI）\n");

    long enc = __vmi_class_type_info::encode(16, true);
    printf("    encode(16, public) = 0x%lx\n", enc);
    printf("    二进制: ");
    for (int i = 15; i >= 0; --i) putchar((enc >> i) & 1 ? '1' : '0');
    printf("\n");

    EXPECT_EQ(enc, ((16 << 8) | 0x1))
        << "编码 = 0x1001：偏移 16 左移 8 位 + public 标志 bit0";
    EXPECT_EQ(__vmi_class_type_info::offset_of(enc), 16)
        << "解码 >>8 得偏移 16";
    EXPECT_TRUE(__vmi_class_type_info::is_public(enc))
        << "解码 &1 得 public 标志 = true";

    long priv = __vmi_class_type_info::encode(32, false);
    printf("    encode(32, !public) = 0x%lx\n", priv);
    EXPECT_EQ(priv, (32 << 8))
        << "非 public 编码 = 偏移<<8（无标志位）";
    EXPECT_FALSE(__vmi_class_type_info::is_public(priv))
        << "解码 &1 = false（非 public 基类）";
    EXPECT_EQ(__vmi_class_type_info::offset_of(priv), 32)
        << "解码 >>8 得偏移 32";

    long zero = __vmi_class_type_info::encode(0, true);
    printf("    encode(0, public) = 0x%lx\n", zero);
    EXPECT_EQ(zero, 0x1)
        << "偏移 0 的编码 = 仅标志位 0x1";
    EXPECT_EQ(__vmi_class_type_info::offset_of(zero), 0)
        << "解码 >>8 得偏移 0";

    long big = __vmi_class_type_info::encode(1024, true);
    printf("    encode(1024, public) = 0x%lx\n", big);
    EXPECT_EQ(__vmi_class_type_info::offset_of(big), 1024)
        << "大偏移 1024 编码后解码仍正确";
}

// ---------------------------------------------------------------------------
// T8: 与真实 typeid 对照 —— 形态选择由继承拓扑决定
// ---------------------------------------------------------------------------
struct RealA { virtual ~RealA() {} };
struct RealB { virtual ~RealB() {} };
struct RealD : RealA, RealB {};
struct RealMid : RealA {};

static const char* real_form(const std::type_info& ti) {
    Dl_info info;
    void* vptr = nullptr;
    std::memcpy(&vptr, static_cast<const void*>(&ti), sizeof vptr);
    if (dladdr(vptr, &info) && info.dli_sname) return info.dli_sname;
    return "(unknown)";
}

TEST(TypeInfoTest, T8_RealAbi) {
    printf("[T8] 与真实 ABI 对照（dladdr 反查 typeinfo 的形态类）\n");

    const char* f_solo = real_form(typeid(RealA));
    const char* f_si   = real_form(typeid(RealMid));
    const char* f_vmi  = real_form(typeid(RealD));

    printf("    无基类   RealA           → %s\n", f_solo);
    printf("    单继承   RealMid : RealA → %s\n", f_si);
    printf("    多继承   RealD : RealA, RealB → %s\n", f_vmi);

    EXPECT_TRUE(strstr(f_solo, "__class_type_info") != nullptr)
        << "无基类 → __class_type_info（对应本演示 FORM_CLASS='C'）";
    EXPECT_TRUE(strstr(f_si, "__si_class_type_info") != nullptr)
        << "单继承 → __si_class_type_info（对应本演示 FORM_SI='S'）";
    EXPECT_TRUE(strstr(f_vmi, "__vmi_class_type_info") != nullptr)
        << "多继承 → __vmi_class_type_info（对应本演示 FORM_VMI='V'）";

    printf("    typeid(RealA).name()    = \"%s\"\n", typeid(RealA).name());
    printf("    typeid(RealMid).name()  = \"%s\"\n", typeid(RealMid).name());
    printf("    typeid(RealD).name()    = \"%s\"\n", typeid(RealD).name());

    EXPECT_TRUE(strstr(typeid(RealA).name(), "RealA") != nullptr)
        << "typeid.name() 包含类型名（Itanium mangling）";
}

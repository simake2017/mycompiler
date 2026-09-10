// =============================================================================
// 测试：嵌套类字段的布局与链式访问 (Nested Class Field Layout & Access)
// =============================================================================
// 理论点：
//   - size 与 align 分离（对照 clang RecordLayoutBuilder.cpp:1850 LayoutField
//     的 TI.Width / TI.Align）：类类型字段的对齐 = 成员 align 的递归最大值，
//     与它的 size 无关。Five{bool×5} size=5 但 align=1，绝不能拿 size 当 align。
//   - 成员声明处类型必须 complete（[class.mem]/2），布局经注册类型取真值，
//     不存在 size=0 的占位类型。
//   - 嵌套类字段是"内联子对象"：o->f.a 应折叠为地址计算（obj + f.offset + a.offset），
//     而非把 f 的头 8 字节当指针解引用。
// 预期行为：
//   - Outer 布局：tag@0, f@4(5B), tail@12, size=16（与 clang oracle 完全一致）
//   - o->f.a 链式读写正确（返回 9 = 7 + 2）
// 回归背景（三处修复）：
//   ① Sema：字段类型补 resolveType——修复前占位 Class("Five") 的 totalSize=0，
//      f 占 0 字节，tail 压上来重叠，size=8（malloc 分配不足会越界写）。
//   ② Sema：新增 alignOf() 分离 size/align——修复前拿 size 当 align，
//      Five{bool×5} 会推出 alignTo(4,5) 的非 2 幂错位布局。
//   ③ CodeGen：emitMember 对类类型字段改用 leaq 取子对象地址——修复前
//      按 8B 走 movq 把成员内容当指针解引用 → o->f.a 段错误。
// clang oracle（-Xclang -fdump-record-layouts）：
//   0|tag 4|f(b1..b5 占 4..8) 12|tail  sizeof=16 align=4
// =============================================================================

class Five {
public:
    bool b1; bool b2; bool b3; bool b4; bool b5;
    int  a;
    Five() {}
    Five(int v) : a(v) {}
};

class Outer {
public:
    int  tag;
    Five f;
    int  tail;
    Outer() {}
    Outer(int t) : tag(t), f(7) {}
};

int main() {
    Outer* o = new Outer(1);
    o->tail = 2;
    // f.a 由 ctor 初始化为 7；tail=2 → 返回 9
    // 修复前：布局重叠 + 链式访问段错误
    return o->f.a + o->tail;
}

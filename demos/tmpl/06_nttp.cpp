// ============================================================================
// demo 06 —— 非类型模板参数（NTTP）：把【值】当模板参数
// ============================================================================
// 理论：[temp.param]/4 —— 模板参数不只能是类型，还可以是一个编译期常量：
//
//     template <int N> struct Size { ... };
//     Size<4> 与 Size<8> 是【两个不同的类型】，各有各的实例与汇编符号。
//
//   ★ 值参与 mangling：Size<4> 编成 `Size<4>` 的符号、Size<8> 编成另一个 ——
//     这与类型实参走的是同一套 Itanium `L...E` 编码（见 docs/learn/18）。
//   ★ 类型形参与非类型形参可以在同一个模板里混排：template <class T, int N>。
//
// 看两个实例如何分流：
//   ./build-linux/minicc demos/tmpl/06_nttp.cpp -S -o /tmp/demo06.s
// 终端可见（搜 "instantiate:name"）：
//   [instantiate:name] 'Size<4>' → 汇编符号前缀 'Size_4'
//   [instantiate:name] 'Size<8>' → 汇编符号前缀 'Size_8'
//
// 预期：退出码 0
// ============================================================================

// 单个非类型参数
template <int N>
struct Size {
    int value() { return N; }
};

// 类型 + 值混排
template <class T, int N>
struct Buf {
    T first;
    int count() { return N; }
};

int main() {
    // ① 值不同 ⇒ 类型不同 ⇒ 两个独立实例
    Size<4> a;
    Size<8> b;
    if (a.value() != 4) return 1;
    if (b.value() != 8) return 2;

    // ② 类型与值混排
    Buf<int, 3> c;
    c.first = 7;
    if (c.first != 7)   return 3;
    if (c.count() != 3) return 4;

    return 0;
}

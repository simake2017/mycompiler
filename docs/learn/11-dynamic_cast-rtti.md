# 11 dynamic_cast 与 RTTI：运行时类型识别

> `dynamic_cast<T*>(p)` 是 C++ 里唯一"编译期检查合法性、运行期才出结果"的转型。
> 它的背后是 RTTI（Run-Time Type Information）：编译器为每个多态类埋下
> 类型元数据（typeinfo），运行时顺着对象的 `_vptr` 找到它，再沿继承链匹配。
> 本文从标准条款讲到 Itanium ABI，最后对照 minicc 的完整实现。

---

## ① 理论背景

**标准依据**：[expr.dynamic.cast]（dynamic_cast 的静态约束与运行时语义）。

### 1.1 为什么 `static_cast` 不够

```cpp
Animal* a = getAnimal();   // 动态类型可能是 Dog，也可能是 Cat
Dog* d = static_cast<Dog*>(a);  // 编译器只看静态类型：Animal* → Dog* 照做
d->speed;                       // 若实际是 Cat → 未定义行为（静默踩雷）
```

`static_cast` 是**编译期的信仰之跃**：只检查类型关系，不检查对象实况。
`dynamic_cast` 则要求编译器在运行时**真的去问对象**："你到底是谁？"

### 1.2 [expr.dynamic.cast] 的两层规则

**静态层（编译期，[expr.dynamic.cast]/3-5）**：

- 源类型必须是"指向多态类的指针/引用"（至少有一个虚函数——否则没有
  vtable，运行时无法识别类型）；
- 目标类型必须与源类型"存在继承关联"。注意标准措辞是"源与目标属于
  同一继承体系"，**并不要求目标是源的基类**——兄弟类型之间的转型
  静态上合法，只是运行时通常失败：

```cpp
Animal* a = new Cat();
Dog* d = dynamic_cast<Dog*>(a);   // 编译 OK（同属 Animal 体系）
                                   // 运行时 a 是 Cat → 返回 nullptr
```

**动态层（运行时，[expr.dynamic.cast]/7-9）**：

- 指针版：成功返回指向目标子对象的指针，失败返回 **空指针**；
- 引用版：失败抛 `std::bad_cast`（minicc 未实现引用版与异常）；
- 空指针输入：直接失败（标准明确：空指针转空指针）。

### 1.3 运行时凭什么知道类型：typeinfo

Itanium C++ ABI（Linux/GCC/Clang 共用）规定：

```text
每个多态类 X 都有两个编译器生成的全局符号：

  _ZTV1X   vtable：[-2]offset-to-top  [-1]&typeinfo  [0..]虚函数地址...
  _ZTI1X   typeinfo：这个类"是谁"的元数据
```

关键链条——**对象如何自报家门**：

```text
对象首 8 字节 = _vptr ──► vtable
                            ├─ vtable[-1] = &_ZTI（我是谁）
                            └─ vtable[0..] = 虚函数（我能干什么）
```

### 1.4 typeinfo 的三种形态（`__si` 是单继承的关键）

libstdc++ 中 `std::type_info` 有三个派生类，对应三种继承形态：

| ABI 类型 | 适用 | 额外槽位 |
|---|---|---|
| `__fundamental_type_info` | 内建类型（int 等） | 无 |
| `__si_class_type_info` | **单继承**类 | 1 个指针：直接基类的 typeinfo |
| `__vmi_class_type_info` | 多继承类 | 基类计数 + 基类数组 |

单继承下，继承链就是一条单链表——`dynamic_cast` 只需从对象的
typeinfo 出发，沿"基类指针"槽逐级上溯比较即可。这正是 minicc 实现的形态。

---

## ② 真实编译器怎么做（clang/gcc 对照）

### 2.1 探测命令（可复现）

```bash
cat > /tmp/dc_probe.cpp <<'EOF'
struct Animal { int legs; virtual int speak() { return 0; } };
struct Dog : Animal { int speed; virtual int speak() { return 1; } };
int main() {
    Dog* d = new Dog();
    Animal* a = dynamic_cast<Animal*>(d);
    Dog* d2 = dynamic_cast<Dog*>(a);
    return d2 ? d2->speak() : -1;
}
EOF
clang++ -S -O1 -o /tmp/dc_clang.s /tmp/dc_probe.cpp
grep -E "__dynamic_cast|_ZTI" /tmp/dc_clang.s
```

### 2.2 clang 生成的关键片段

```asm
leaq  _ZTI6Animal(%rip), %rsi     # 目标 typeinfo 作为参数
leaq  _ZTI3Dog(%rip), %rdx
callq __dynamic_cast@PLT          # libstdc++ 提供的运行时库函数
```

结论：真实编译器把匹配逻辑放在 **libstdc++ 的 `__dynamic_cast` 库函数**里，
编译器只负责发参数（4 个：源指针、源 ti、目标 ti、hint）。

### 2.3 minicc 的对照实现

| 环节 | clang + libstdc++ | minicc |
|---|---|---|
| typeinfo 符号 | `_ZTI6Animal`（同名同 mangling） | 同 |
| typeinfo 布局 | vptr + 名字 + `__si` 基类指针 | 3 槽简化版（语义同构） |
| 匹配入口 | `call __dynamic_cast@PLT`（库函数） | `call __minicc_dynamic_cast`（汇编内联到 .s） |
| 上溯方式 | 运行时解析 `__si_class_type_info::base` | 直接读 typeinfo 第 3 槽（+16） |
| 失败语义 | 指针返回 0 / 引用抛异常 | 指针返回 0 |
| vtable[-1] 槽 | `&typeinfo` | 同 |

**设计决策**：把"库函数"直接发射进每个 .s 文件（`__minicc_dynamic_cast`），
这样产物自包含，一条 `gcc test.s` 即可链接运行，教学上每一步可观测。

---

## ③ minicc 中的完整流水线

`dynamic_cast<Dog*>(a2)` 这一行源码的六阶段旅程：

```text
阶段 1 Lexer      include/token.h 注册关键字 → 切出 KwDynamicCast token
阶段 2 Parser     src/parser.cpp parsePrimaryExpr → DynamicCastExpr{目标类, 操作数}
阶段 3 Sema       inferDynamicCast：两检查（目标类存在 / 同体系）→ 类型定为 T*
阶段 4 继承图     printInheritanceGraph：Pass 1 后 DFS 打印整棵继承树
阶段 5 模板克隆   template_instantiation.cpp cloneExpr（模板内也能用）
阶段 6 CodeGen    emitDynamicCast：取操作数 → leaq 目标 ti → call 助手
```

### 3.1 typeinfo 布局（.data 段，三槽）

```asm
_ZTI3Dog:
    .quad 0                # 槽0：type_info 的 vtable（教学版简化为 0）
    .quad .Ltype_name_Dog  # 槽1：类型名字符串（.rodata）
    .quad _ZTI6Animal      # 槽2：★基类 typeinfo 指针（__si 风格）
                           #        根类此槽为 0 —— 链的终点
```

对照真实 `__si_class_type_info`：

```text
真实：  { __class_type_info 基类部分(含 vptr+name) , __base_type }
minicc：{ vptr槽 , name槽 , __base_type }   ← 语义结构同构，只是拍平成 3 槽
```

### 3.2 运行时助手（发射在每个 .s 尾部）

```asm
# __minicc_dynamic_cast(%rdi=对象指针, %rsi=目标typeinfo) → %rax
__minicc_dynamic_cast:
    testq %rdi, %rdi        # 空指针输入 → 直接失败
    je    .Ldc_fail
    movq  (%rdi), %rcx      # rcx = obj._vptr（对象首 8 字节）
    movq  -8(%rcx), %rax    # rax = vtable[-1] = 对象实际类型的 typeinfo
.Ldc_loop:
    cmpq  %rsi, %rax        # 命中目标？（typeinfo 全局唯一 → 比地址即可）
    je    .Ldc_ok
    movq  16(%rax), %rax    # 未命中 → 沿"基类指针"槽(+16)上溯
    testq %rax, %rax        # 到根了（槽为 0）？
    je    .Ldc_fail
    jmp   .Ldc_loop
.Ldc_ok:   movq %rdi, %rax  # 成功：返回原指针（单继承无偏移调整）
           ret
.Ldc_fail: xorq %rax, %rax  # 失败：返回空指针
           ret
```

**两个教学要点**：

1. **比地址而非比内容**：每个类的 typeinfo 是链接期唯一的全局符号，
   两个指针"指向同一个 `_ZTI`"等价于"同一个类型"——无需 `strcmp` 名字；
2. **单继承无指针调整**：基类子对象就在派生类对象的起始处，
   成功时原指针即正确结果。多继承才需要 offset 调整（本项目不支持）。

### 3.3 继承图：编译期静态视图

`printInheritanceGraph()`（Sema Pass 1 之后）从 `m_classDecls` 建
"基类 → 孩子们"邻接表，对无基类的根做 DFS。示例（`tests/test_rtti_03_inheritance_graph.cpp`）：

```text
│ ═══ 类型继承图（inheritance graph）═══
│ Animal [polymorphic]
│ ├── Cat [polymorphic]
│ └── Dog [polymorphic]
│ Point
│ Shape [polymorphic]
│ └── Rectangle [polymorphic]
│     └── Square [polymorphic]
```

- `[polymorphic]` 标记有虚表的类——只有它们才有 typeinfo、才配做
  `dynamic_cast` 的操作数，两个特性在同一个概念上汇合；
- `Point` 无虚函数 → 无标记，它是继承图的旁观者。

---

## ④ 内存布局实验：从源码片段到布局草图（全实测）

> 本节所有数字均来自本机实验（`clang++-18 -stdlib=libc++`，64 位），
> 对应单元测试 `tests/unit/test_rtti_layout.cpp`（16 个用例，全部打印
> 布局并用黄金值钉死）。先画推演草图，再实测对拍——推演被推翻的地方
> 单独记录在 4.9。

### 4.1 发射决策表：vtable / typeinfo 到底什么时候出现

运行时支持不是"类"自带的，而是被**特定需求**拖出来的：

| 场景 | vtable | typeinfo | 探测结论 |
|---|---|---|---|
| 普通类，无虚函数、无 RTTI 使用点 | ❌ | ❌ | `objdump -t` 符号表干净 |
| 无虚函数类 + 写了 `typeid(类名)` | ❌ | ✅（弱符号） | 静态 typeid 直接引用 typeinfo，不经过 vtable |
| **虚继承，无虚函数，涉及构造** | ✅（+VTT） | ✅ | 构造函数要安置 vbptr，被迫发射 |
| 多态类（有虚函数） | ✅ | ✅ | 标准情形 |
| 多态类 + `-fno-rtti` | ✅ | ❌ | `dynamic_cast`/`typeid(表达式)` 直接编译错 |
| 多重继承（无虚函数、无 RTTI） | ❌ | ❌ | 布局全是编译期常量，一条 `add` 搞定指针转换 |

探测命令（可复现）：

```bash
cat > /tmp/probe.cpp <<'EOF'
struct A { int a; };
struct B { int b; };
struct C : A, B { int c; };     // 多重继承，无虚函数
B* toB(C* p) { return p; }
EOF
clang++-18 -O0 -c /tmp/probe.cpp -o /tmp/probe.o
objdump -t /tmp/probe.o | c++filt      # 只有 toB(C*)，无任何 RTTI 符号
objdump -d /tmp/probe.o                # toB 里就一条 add $0x4, %rax
```

### 4.2 草图一：普通类——对象就是数据本身

```cpp
struct Plain { int x; };
```

```text
推演：无虚函数 → 无需派发 → 无需头部
┌────────┐
│ x (4B) │        sizeof = 4，零运行时头
└────────┘
+0      +4
```

### 4.3 草图二：多态类——头部、头指针、头部指向的结构（三步全拆）

```cpp
struct Poly { virtual ~Poly() {} int x; };
```

**第一步：对象头部到底有什么。** 多态 = 对象头 8 字节被 `_vptr` 占据：

```text
对象 Poly（16 字节）
┌────────┬────────────┬────────────────────────────┐
│ 偏移   │ 字段       │ 内容                       │
├────────┼────────────┼────────────────────────────┤
│ +0     │ _vptr (8B) │ 头指针 → vtable for Poly   │
│ +8     │ x (4B)     │ 数据成员                   │
│ +12    │ pad (4B)   │ 8 字节对齐填充             │
└────────┴────────────┴────────────────────────────┘
推演：vptr(8) + x(4) + pad(4) = 16，实测 sizeof 相符
```

**第二步：头指针指向什么结构。** `_vptr` 并不指向 vtable 符号的开头，
而是指向 `_ZTVPoly + 16`——第一个虚函数槽。前面 16 字节是"表头区"，
用**负下标**访问（dladdr 实测："vtable for Poly +16"）：

```text
vtable for Poly（_ZTVPoly，.data 段，逐槽实测）
┌───────┬──────────────────┬─────────────────────────────────────┐
│ 槽    │ 字段             │ 实测内容                            │
├───────┼──────────────────┼─────────────────────────────────────┤
│ [-2]  │ offset-to-top    │ 0（本表对应的部分在对象顶端）       │
│ [-1]  │ typeinfo 指针    │ → typeinfo for Poly                 │
│ [0]   │ 虚函数槽 0       │ Poly::~Poly()  D1（完整析构）       │
│ [1]   │ 虚函数槽 1       │ Poly::~Poly()  D0（deleting 析构）  │
└───────┴──────────────────┴─────────────────────────────────────┘
            ▲
            └── vptr 指向这里（符号地址 +16）
```

为什么一个虚析构占两个槽：D1 析构整个对象；D0 在析构后再调
`operator delete`——虚析构在表里永远成对出现。

**第三步：vtable[-1] 又指向哪。** typeinfo 本身也是多态对象，
它的头部也有 vptr，指向它自己的类（三种形态之一）的 vtable：

```text
typeinfo for Poly（_ZTI4Poly，16 字节，__class_type_info 形态）
┌───────┬─────────────────┬──────────────────────────────────────────────┐
│ 偏移  │ 字段            │ 实测内容                                     │
├───────┼─────────────────┼──────────────────────────────────────────────┤
│ +0    │ 自身 vptr       │ → vtable for __cxxabiv1::__class_type_info+16│
│ +8    │ mangled 类型名  │ "4Poly"（4=长度，Poly=名字）                 │
└───────┴─────────────────┴──────────────────────────────────────────────┘
★ 无基类的类到此为止：__class 形态只有 2 槽，再读 +16 就是越界
```

链条闭环（`VtableRtti.VptrPerClassAndTypeinfoChain` 用例验证）：

```text
对象首8字节 ──► vptr ──(vptr[-1])──► typeinfo == &typeid(Poly)
                     └─(vptr[0..])──► 虚函数派发
同类两个对象：vptr 值相同（一张表）；不同类：值不同
```

### 4.4 草图三：单继承——基类子对象叠在偏移 0，typeinfo 链成单链表

```cpp
struct Dog : Poly { int speed; };
```

```text
┌──────────────────── Poly 子对象 ────────────────────┐
│ _vptr (8B)     │ x (4B)    │ speed (4B) │           │
└────────────────┴───────────┴────────────┘           │
+0               +8          +12            +16 = sizeof(Dog)

★ Dog 对象地址 == Poly 子对象地址（实测指针差 0）
→ 这就是 minicc 助手"成功即返回原指针"成立的物理基础
```

**头指针指向的 vtable（逐槽）**：派生类一张自己的表，槽位结构与
Poly 完全同构，只是函数槽换成了自己的析构：

```text
vtable for Dog（_ZTV3Dog）
┌───────┬──────────────────┬──────────────────────────┐
│ 槽    │ 字段             │ 实测内容                 │
├───────┼──────────────────┼──────────────────────────┤
│ [-2]  │ offset-to-top    │ 0（Dog 部分在对象顶端）  │
│ [-1]  │ typeinfo 指针    │ → typeinfo for Dog       │
│ [0]   │ 虚函数槽 0       │ Dog::~Dog() D1           │
│ [1]   │ 虚函数槽 1       │ Dog::~Dog() D0           │
└───────┴──────────────────┴──────────────────────────┘
```

**头部指针指向的 typeinfo（逐槽，关键在第三槽）**：Dog 有基类，
所以 typeinfo 从 `__class` 形态升级为 `__si`（single inheritance）
形态，多出一个 +16 槽指向基类的 typeinfo——**继承链在这里物理成型**：

```text
typeinfo for Dog（_ZTI3Dog，24 字节，__si_class_type_info 形态）
┌───────┬───────────────────┬──────────────────────────────────────────┐
│ 偏移  │ 字段              │ 实测内容                                 │
├───────┼───────────────────┼──────────────────────────────────────────┤
│ +0    │ 自身 vptr         │ → vtable for __si_class_type_info +16    │
│ +8    │ mangled 类型名    │ "3Dog"                                   │
│ +16   │ ★__base_type      │ → typeinfo for Poly（基类 ti 指针）      │
└───────┴───────────────────┴──────────────────────────────────────────┘
```

把两条链串起来，就是 `dynamic_cast` 沿链上溯的全部物理图景：

```text
Dog 对象 ─► vtable for Dog ─► typeinfo for Dog ─► typeinfo for Poly
              [-1]槽              [+16]槽              ↑ 链的终点
                                            （Poly 无基类，没有 +16 槽）

dynamic_cast<Poly*>(dog)：从 typeinfo(Dog) 出发，
  Dog ≠ Poly → 读 +16 槽 → Poly == Poly ✓ 命中（单继承下指针零调整）
```

这条链也解释了 §3.2 助手为什么"读 typeinfo 第 3 槽（+16）"就能上溯：
那正是 `__si_class_type_info` 的 `__base_type` 成员的物理位置。

### 4.5 草图四：多重继承——两个头、两张子表、指针必须调整

```cpp
struct MiA { int a; virtual ~MiA() {} };
struct MiB { int b; virtual ~MiB() {} };
struct MiC : MiA, MiB { int c; };
```

**第一步：对象里有几个"头"。** 每个带虚函数的基类子对象都要有自己的
`_vptr`——两个基类 → 两个头指针，对象被分成三段：

```text
对象 MiC（32 字节，实测字段表）
┌────────┬────────────┬──────────────────────────────────────────┐
│ 偏移   │ 字段       │ 内容                                     │
├────────┼────────────┼──────────────────────────────────────────┤
│ +0     │ vptr_A (8B)│ ★头指针 1 → 主子表（dladdr: _ZTV3MiC+16）│
│ +8     │ a (4B)     │ MiA 的成员                               │
│ +12    │ pad (4B)   │ 对齐                                     │
│ +16    │ vptr_B (8B)│ ★头指针 2 → 次子表（dladdr: _ZTV3MiC+48）│
│ +24    │ b (4B)     │ MiB 的成员                               │
│ +28    │ c (4B)     │ MiC 自身成员                             │
└────────┴────────────┴──────────────────────────────────────────┘
实测：MiC→MiA* 差 = 0（第一基类在头部，免调整）
      MiC→MiB* 差 = +16（第二基类要指针调整）
```

注意：**两张子表都在同一个 `_ZTV3MiC` 符号里**，只是偏移不同
（+16 是主子表起点，+48 是次子表起点；每张表前 16 字节是表头区，
用负下标访问）。

**第二步：两张子表各自指向什么（逐槽）**。表头区的 `offset-to-top`
记录"本表所属子对象距完整对象起点多远"，次子表是 **-16**——从 +16
回到 0：

```text
主子表 _ZTV3MiC+16（vptr_A 指向它）          次子表 _ZTV3MiC+48（vptr_B 指向它）
┌───────┬────────────────┬──────────────┐    ┌───────┬────────────────┬─────────────────────┐
│ 槽    │ 字段           │ 实测         │    │ 槽    │ 字段           │ 实测                │
├───────┼────────────────┼──────────────┤    ├───────┼────────────────┼─────────────────────┤
│ [-2]  │ offset-to-top  │ 0            │    │ [-2]  │ offset-to-top  │ ★ -16               │
│ [-1]  │ typeinfo       │ → ti(MiC)    │    │ [-1]  │ typeinfo       │ → ti(MiC)（同一个） │
│ [0]   │ 虚函数槽 0     │ MiC::~MiC D1 │    │ [0]   │ 虚函数槽 0     │ non-virtual thunk   │
│ [1]   │ 虚函数槽 1     │ MiC::~MiC D0 │    │ [1]   │ 虚函数槽 1     │ to MiC::~MiC()      │
└───────┴────────────────┴──────────────┘    └───────┴────────────────┴─────────────────────┘
```

★ 次子表函数槽不是直接放 `MiC::~MiC()`，而是放一个 **thunk**
（小跳板，符号名 `_ZThn16_N3MiCD1Ev` = "thunk 16"）：它先把
`this` 减 16（从 MiB 子对象跳回完整对象），再调真正的析构。

**第三步：为什么两张表的 typeinfo 都是 MiC？** 因为对象整体只有一个
"最派生类型"。不管拿着哪个头指针去查 `vtable[-1]`，查到的都是
`typeinfo for MiC`——dynamic_cast 从任何一个基类子对象入口进来，
都能认出这是 MiC。

**第四步：typeinfo for MiC 怎么存储"两个基类"的信息（逐槽）**。
两个基类 → 单链表的单槽装不下 → 升级为 `__vmi` 形态：

```text
typeinfo for MiC（32 字节 + 基类数组，__vmi_class_type_info 形态）
┌────────┬───────────────────┬───────────────────────────────────────────┐
│ 偏移   │ 字段              │ 实测内容                                  │
├────────┼───────────────────┼───────────────────────────────────────────┤
│ +0     │ 自身 vptr         │ → vtable for __vmi_class_type_info +16    │
│ +8     │ mangled 类型名    │ "3MiC"                                    │
│ +16    │ flags (u32)       │ 0（无虚基类标志）                         │
│ +20    │ base_count (u32)  │ ★ 2（两个直接基类）                       │
│ +24    │ bases[0].ti       │ → typeinfo for MiA                        │
│ +32    │ bases[0].of_flags │ 0x0002（public=1，偏移=0）                │
│ +40    │ bases[1].ti       │ → typeinfo for MiB                        │
│ +48    │ bases[1].of_flags │ 0x1002（public=1，偏移=16）               │
└────────┴───────────────────┴───────────────────────────────────────────┘
★ 注意 +16/+20 是两个独立的 u32，打包进同一个 8 字节槽——
  按 u64 读会得到 0x200000000，把 base_count 误当"巨大指针"
```

`offset_flags` 解码规则（`<cxxabi.h>`）：`bit0=virtual, bit1=public,
高 24 位 = 基类子对象偏移`。于是 `bases[1] = 0x1002` 翻译过来就是：
"MiB 子对象在 +16，public 继承，非虚基类"——**与上面布局表实测的
`MiC→MiB* 差 = +16` 完全互证**。

**dynamic_cast 在这里不再是"原样返回"**——`dynamic_cast<MiB*>(a)`
要拿着 MiA 子对象地址去问 typeinfo："MiB 子对象在哪儿？"答案就编码
在上面的 `bases[1].offset_flags` 里。运行时流程：

```text
从 vptr_A 查 vtable[-1] → ti(MiC) → 发现是 __vmi（vptr 指向 __vmi vtable）
  → 遍历 bases[]：找到 ti(MiB)，读 offset_flags=0x1002
  → 指针 = 原地址 + 16，返回
```

更深的解码细节见 4.7，菱形虚继承下的变体见 4.6。

### 4.6 草图五：菱形虚继承——共享子对象被推到尾部，靠 vbptr 间接寻址

```cpp
struct DiaB { int v; virtual ~DiaB() {} };
struct DiaD : virtual DiaB { int d; };
struct DiaE : virtual DiaB { int e; };
struct DiaX : DiaD, DiaE { int x; };
```

推演：普通拼接会让 DiaB 出现两份，虚继承要求唯一 → 只能挪到
所有路径都够得着的位置（尾部），靠 vbptr 间接寻址：

```text
对象 DiaX（48 字节，实测字段表）
┌────────┬──────────────┬───────────────────────────────────────────┐
│ 偏移   │ 字段         │ 内容                                      │
├────────┼──────────────┼───────────────────────────────────────────┤
│ +0     │ vptr_D (8B)  │ ★头指针 1 → _ZTV4DiaX+24（D 部分子表）    │
│ +8     │ d (4B)       │ DiaD 成员                                 │
│ +12    │ pad (4B)     │ 对齐                                      │
│ +16    │ vptr_E (8B)  │ ★头指针 2 → _ZTV4DiaX+64（E 部分子表）    │
│ +24    │ e (4B)       │ DiaE 成员                                 │
│ +28    │ x (4B)       │ DiaX 自身成员                             │
│ +32    │ vptr_B (8B)  │ ★头指针 3 → _ZTV4DiaX+104（共享 DiaB 表） │
│ +40    │ v (4B)       │ ★共享成员，只有一个！                     │
│ +44    │ pad (4B)     │ 对齐                                      │
└────────┴──────────────┴───────────────────────────────────────────┘
实测：DiaX→DiaD* 差 = 0；DiaX→DiaB* 差 = +32（共享子对象在尾部）
```

★ 虚基类带来一个特殊安排：D/E 部分的头指针是 **vptr 与 vbptr 合一**
（基类 DiaB 本身是多态的，所以 D/E 部分必须有可派发的头；同时它们
又要间接寻址共享 B——两个角色合并进同一张子表：`[-3]` 槽承担
vbptr 职责，`[0..]` 槽承担派发职责）。**三个头指针指向同一个
`_ZTV4DiaX` 符号内的三张子表**。

**三张子表逐槽对比（这是虚继承布局的精华）**。有虚基类时，表头区
多出一个 `[-3]` 槽——**vbase 距离**（virtual thunk 运行时靠它算地址）：

```text
D 部分子表 _ZTV4DiaX+24        E 部分子表 _ZTV4DiaX+64        共享 DiaB 表 _ZTV4DiaX+104
┌──────┬─────────────────┐    ┌──────┬─────────────────┐    ┌──────┬────────────────────┐
│ 槽   │ 实测            │    │ 槽   │ 实测            │    │ 槽   │ 实测               │
├──────┼─────────────────┤    ├──────┼─────────────────┤    ├──────┼────────────────────┤
│ [-3] │ ★ 32（到共享B） │    │ [-3] │ ★ 16（到共享B） │    │ [-3] │ ★ -32（回到完整对象）│
│ [-2] │ 0               │    │ [-2] │ -16             │    │ [-2] │ -32                │
│ [-1] │ → ti(DiaX)      │    │ [-1] │ → ti(DiaX)      │    │ [-1] │ → ti(DiaX)         │
│ [0]  │ DiaX::~DiaX D1  │    │ [0]  │ _ZThn16_ thunk  │    │ [0]  │ _ZTv0_n24_ 虚thunk │
│ [1]  │ DiaX::~DiaX D0  │    │ [1]  │ _ZThn16_ thunk  │    │ [1]  │ _ZTv0_n24_ 虚thunk │
└──────┴─────────────────┘    └──────┴─────────────────┘    └──────┴────────────────────┘
```

逐列解读：

| 列 | `[-3]` vbase 距离 | `[-2]` offset-to-top | 函数槽 |
|---|---|---|---|
| D 部分 | +32 = 本部分 vbptr(+0) 到共享 B(+32) 的字节数 | 0（在对象顶端） | 直接函数 |
| E 部分 | +16 = vbptr(+16) 到共享 B(+32) | -16（回到顶端） | 普通 thunk（`this -= 16`） |
| 共享 B | -32 = 从共享 B 回到完整对象顶端 | -32 | **虚 thunk**：先读 vbptr 的 `[-3]` 槽动态算出完整对象地址，再调函数 |

★ 普通多继承的 thunk 把调整量**写死在符号名里**（`_ZThn16_`）；
菱形里"共享 B 距完整对象多远"随最派生类型而变，编译期写不死——
所以共享 B 的槽用**虚 thunk**，运行时现场读表。这正是虚继承比
普通多继承贵、又必须存在的理由。

对照实验——把 DiaD 单独实例化（`dia_solo` 探测），共享 B 就在它
自己内部 +16：此时 DiaD 的子表 `[-3] = 16`（不再是 32）。**同一张
"角色表"的 `[-3]` 值随最派生类型改变**，这就是"表里的数字是
为最终布局量身定做"的直接证据。

**无虚函数的虚继承也一样有头**：`struct ViV { int v; };
struct ViW : virtual ViV { int w; };`——两个类都没有虚函数，但
`sizeof(ViW)=16`，头部 8 字节非空（实测）。因为虚基类必须靠
vbptr 间接定位，构造函数的职责就是安置它。ViW 的"表"长这样：

```text
ViW 头部指向的结构（头指针 = _ZTT3ViW，即 VTT——构造用子表数组）
┌──────┬──────────────────────────────────────────────┐
│ 槽   │ 实测                                         │
├──────┼──────────────────────────────────────────────┤
│ [-3] │ ★ 12 = vbptr(+0) 到 ViV 子对象(+12) 的距离   │
│ [-2] │ 0                                            │
│ [-1] │ → typeinfo for ViW                           │
└──────┴──────────────────────────────────────────────┘
（无虚函数 → 没有 [0..] 函数槽，表只有头；VTT 其余条目供构造函数链使用）
```

**ViW 的 typeinfo：虚基类在 `__vmi` 里怎么编码（逐槽）**。ViW 是
多/虚基类形态，但基类数组的条目编码方式变了——虚基类不能写死
对象内偏移，改用**符号距离指向 vbtable 槽**：

```text
typeinfo for ViW（__vmi 形态）
┌────────┬──────────────────┬─────────────────────────────────────────────┐
│ 偏移   │ 字段             │ 实测                                        │
├────────┼──────────────────┼─────────────────────────────────────────────┤
│ +16    │ flags (u32)      │ 0                                           │
│ +20    │ base_count (u32) │ 1（一个虚基类 ViV）                         │
│ +24    │ bases[0].ti      │ → typeinfo for ViV                          │
│ +32    │ bases[0].of_flags│ 0xffffffffffffe803                          │
└────────┴──────────────────┴─────────────────────────────────────────────┘
解码 0xffffffffffffe803：
  bit0(virtual) = 1    bit1(public) = 1
  ★ 虚基类时高位按【有符号】解释：>>8 = -24（字节）
  -24 = vbptr 的 [-3] 槽位置（-24 字节 / 8 = 槽 [-3]）
```

串起来就是虚基类的完整寻址链：

```text
dynamic_cast 想从 ViW 找到 ViV 子对象：
  ti(ViW) → bases[0].of_flags = -24 → 去读 vbptr[-3]
  vbptr[-3] = 12 → ViV 子对象地址 = vbptr + 12
（对比普通基类 MiB：offset_flags = +16 → 直接对象地址 + 16，一步到位）
```

### 4.7 逐字节解码 `__vmi_class_type_info`

typeinfo 按继承拓扑三选一（`VtableRtti.TypeinfoTopologyThreeFlavors`）：

```text
无基类        → __class_type_info      （Plain）
单继承        → __si_class_type_info   （Dog —— minicc 实现的形态）
多/虚基类     → __vmi_class_type_info  （MiC、DiaX）
```

`__vmi` 的基类数组编码（`<cxxabi.h>` `__offset_flags_masks`）：

```text
offset_flags:  bit0 = virtual   bit1 = public   offset = raw >> 8

MiC 的 typeinfo（实测）：
  base_count = 2
  base[0]: MiA  raw=0x0002  → 偏移 0   public  ✓（与 4.5 布局互证）
  base[1]: MiB  raw=0x1002  → 偏移 16  public  ✓
```

解码用例 `VmiDecode.BaseCountAndOffsetFlags` 用手写复刻结构直接读
`typeid(MiC)` 的字节（libc++/libstdc++ 发射字节同构，不依赖
`<cxxabi.h>` 扩展）；踩坑记录：`flags`/`base_count` 是两个连续的
**u32**（+16/+20），按 u64 读会把 base[0] 指针误当计数。

### 4.8 多重继承下的 `dynamic_cast`：三种结局

| 场景 | 结局 | 实验证据 |
|---|---|---|
| 无虚函数，转型方向编译期可定 | 退化成 `static_cast`，零 RTTI 发射 | `objdump -t` 无 typeinfo/无 `__dynamic_cast` |
| 无虚函数，真正的 downcast | **编译错误** `'A' is not polymorphic` | 无 vptr 可查，标准直接禁止 |
| 有虚函数 + downcast | 全套机器：`__dynamic_cast` + `__vmi` typeinfo | 符号表出现 UND `__dynamic_cast`、UND `__vmi_class_type_info` vtable |

指针调整全家桶（`DynamicCastAdjust.PointerAdjustmentFamily`）：

```text
upcast    MiC* → MiA*   差 = 0    （第一基类在头部）
crosscast MiA* → MiB*   差 = +16  （兄弟子对象横跳，运行时指路）
downcast  MiA* → MiC*   差 = 0    （匹配最派生类型后回到起点）
错兄弟    SibB* → SibC*  = nullptr （运行时安全失败，不崩溃）
```

对照：`static_cast<B*>(p)` 在 clang -O1 下就一条 `mov`——编译期的
信仰之跃；`dynamic_cast` 则是 `testq` 空检查 + 运行时调用。

### 4.9 被实验推翻的推演（勘误记录）

| 推演 | 实验结果 | 修正 |
|---|---|---|
| "虚继承无虚函数 = 零运行时，与多继承同" | `sizeof(W)=16` 头部 8 字节非空；构造函数出现后发射 vtable+VTT | 虚继承**总是**带 `_vbptr`（8B 头）+ vbtable；vbptr 前 -24 处存 vbase 偏移（实测读出 12 == 成员偏移） |
| "多继承必然拖出运行时" | `toB(C*)` 只有一条 `add $0x4` | 多继承本身零运行时；运行时是被"虚函数"或"dynamic_cast"**分别**拖出来的 |
| "typeinfo 只有多态类才有" | `typeid(Plain)` 发射弱符号 `typeinfo for Plain` | typeinfo 的发射条件是 **RTTI 使用点**，虚函数只是最常见的使用场景 |

---

## ⑤ 本项目的已知简化与真实世界差距

| 简化 | 真实编译器 | 影响 |
|---|---|---|
| typeinfo 比地址 | 同（`__do_catch` 等才比内容） | 无 |
| 仅单继承上溯 | `__vmi` 处理多继承/菱形 | 不支持多继承转型 |
| 无 `dynamic_cast<T&>` | 失败抛 `std::bad_cast` | 仅指针形式 |
| 无跨 .so 的 typeinfo 合并 | 弱符号 + 名字比较 | 单文件产物无此问题 |
| 兄弟转型静态放行 | 同（[expr.dynamic.cast] 要求同体系） | 语义一致 |

---

## ⑥ 复现步骤

```bash
cd /root/cppproject/mycompiler
cmake --build cmake-build-debug

# 编译：观察继承图 + [dynamic_cast] 语义日志
./cmake-build-debug/minicc tests/test_rtti_01_dynamic_cast_ok.cpp
./cmake-build-debug/minicc tests/test_rtti_03_inheritance_graph.cpp | grep "│"

# 检查产物：三槽 typeinfo + 运行时助手
grep -A5 "_ZTI3Dog:" tests/test_rtti_01_dynamic_cast_ok.s
grep -B2 -A14 "__minicc_dynamic_cast:" tests/test_rtti_01_dynamic_cast_ok.s

# 链接并运行：成功路径退出码 0，失败路径退出码 0（负例断言均按预期）
gcc -o /tmp/rtti01 tests/test_rtti_01_dynamic_cast_ok.s -no-pie && /tmp/rtti01; echo $?
gcc -o /tmp/rtti02 tests/test_rtti_02_dynamic_cast_fail.s -no-pie && /tmp/rtti02; echo $?
gcc -o /tmp/rtti03 tests/test_rtti_03_inheritance_graph.s -no-pie && /tmp/rtti03; echo $?

# 布局实验单测（打印所有草图的实测值）
./cmake-build-debug/unit_tests --gtest_filter="MiniccRtti.*:LayoutSketch.*:VtableRtti.*:VmiDecode.*:DynamicCastAdjust.*"
```

### 测试矩阵

| 文件 | 覆盖点 | 运行期预期 |
|---|---|---|
| `test_rtti_01_dynamic_cast_ok.cpp` | upcast + downcast 成功、字段与虚调用仍有效 | exit 0 |
| `test_rtti_02_dynamic_cast_fail.cpp` | 兄弟转型失败、向下转型失败（返回 0）、多级链成功 | exit 0 |
| `test_rtti_03_inheritance_graph.cpp` | 多根继承树、三级继承缩进、非多态类无标记 | exit 0 |
| `tests/unit/test_rtti_layout.cpp` | 发射决策（助手按需/非多态类无 RTTI/无关类拒绝）+ 真实布局草图（普通/多态/单继承/多继承/菱形/虚继承）+ vptr-typeinfo 链条 + `__vmi` 解码 + 指针调整全家桶 | 16 TEST 全绿 |

---

## ⑦ 一图总结

```text
编译期（静态）                          运行期（动态）
─────────────────────                  ─────────────────────────────
类声明 ──► 继承图（Sema 打印）         对象 ──► _vptr ──► vtable[-1]
类声明 ──► _ZTI 三槽（含基类指针）              = &_ZTI（我是谁）
dynamic_cast ──► 同体系检查（静态放行）          │
              ──► call __minicc_dynamic_cast ◄──┘
                    沿基类槽上溯比对地址
                    ├─ 命中 → 返回原指针
                    └─ 到根 → 返回 0
```

**一句话**：`dynamic_cast` = 编译期发一张"允许去问"的通行证，
运行期拿着对象自己的 typeinfo 沿继承链走一趟——链是编译期写死在
`.data` 里的，走链的代码也是编译器发射的，C++ 的多态闭环就此完整。

**多继承扩展视角**（第④节实验的总结）：一旦子对象并排/菱形，
"链"变"图"，上表每个环节都升级一档：

```text
单继承（minicc 现状）                多继承（真实世界）
──────────────────────              ──────────────────────────────
typeinfo 第三槽：1 个基类指针         __vmi：base_count + 基类数组
沿 +16 线性上溯                      遍历数组，处理菱形共享
命中 → 返回【原指针】                 命中 → 按 offset_flags 做指针调整
                                    （base[1]=0x1002 → 实测 +16）
```

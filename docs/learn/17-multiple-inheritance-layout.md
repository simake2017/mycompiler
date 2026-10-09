# 17. 多继承布局：primary base 判定、size/align 分离与嵌套子对象

> 对应 C++ 标准：[class.mi]（多继承对象模型）、[class.mem]/2（成员必须 complete）、
> [basic.align]（对齐要求）、[class.virtual]/2（动态分派）、Itanium C++ ABI §2.4
> （non-virtual base allocation）与 §2.9（RTTI/typeinfo）。
> 参照源码：`clang/lib/AST/RecordLayoutBuilder.cpp`（ItaniumRecordLayoutBuilder）。

## 1. 理论背景

### 1.1 primary base：谁占 offset 0

Itanium ABI 规定：派生类的对象布局中，**第一个多态基类**（有 vtable 的）
作为 primary base，子对象放在 offset 0，与派生类**共享主虚表**（primary base
optimization）。关键点是"**第一个多态的**"，不是"声明的第一个"：

```text
class D : public A(非多态), public P(多态);

声明序：A 在前 —— 但 primary 是 P！
正确布局：  [P 子对象@0 (含 _vptr)] [A 子对象@P尾部] [D 自身字段]
错误布局：  A@0 且 P@0 → 重叠，A.x 压在 _vptr 上，写 x 即毁虚表指针
```

全部基类都非多态时，无 _vptr 之争，约定取声明序第一个当"视作 primary"。

### 1.2 size 与 align 是两个独立维度

clang 摆放每个成员时从 `Context.getTypeInfoInChars(T)` 一次取回
`TI.Width`（大小）与 `TI.Align`（对齐）两个**独立**的值
（`RecordLayoutBuilder.cpp:1850 LayoutField`）：

- 标量：查 TargetInfo ABI 表（int→4、double→8、bool→1、指针→8）
- 类类型：align = 成员 align 的递归最大值（`UpdateAlignment`，:2213），
  **与 size 无关**——`Five{bool×5}` size=5、align=1；`Five{int,int}` size=8、align=4

拿 size 当 align 会推出 `alignTo(4,5)=5` 这种非 2 幂的错位布局。

### 1.3 成员类型必须 complete

[class.mem]/2：类成员不能是不完整类型。clang 里成员布局信息永远来自
`ASTContext::getASTRecordLayout`（按需递归构建 + 缓存），**不存在占位类型**。
教学编译器若让 Parser 的占位 `Class("Five")`（空布局，totalSize=0）漏进布局
计算，字段会占 0 字节 → 后续字段重叠 → malloc 分配不足 → 越界写。

### 1.4 嵌套类字段 = 内联子对象

`o->f.a` 中 `f` 不是指针，是内联在父对象里的子对象。访问 = 地址折叠：
`addr(o) + offset(f) + offset(a)`。任何把子对象头 8 字节当指针解引用
（movq）的代码生成都是错的。

### 1.5 槽位里放的是【符号名】，不是"函数本身"

vtable 是**数据**：槽位在目标文件里就是一条重定位（`.quad <sym>`），链接器只认
裸字符串。Itanium ABI 里这张表由每个 TU 各自发射，链接器再用一组规则
（vague linkage / 主 vtable）去重 —— 所以"槽里的名字"与"函数定义的名字"
必须是同一个字符串，这个是**硬约束**，与语言层的类型检查无关。

对照 clang：`CodeGenModule::EmitVTable` 填槽时拿到的是刚发射过的函数
（`GlobalDecl` 句柄），名字来自同一个 `MangleContext` ⇒ 结构上不可能错配。
minicc 没有句柄、只能各自拼字符串 ⇒ 判据必须收口（见 docs/learn/13 §13.4.3）。

**次表（secondary）的语义**因此要格外小心：次基类子对象在派生类里**换了位置**，
但槽里该放谁的名字，取决于**谁提供了这个实现**，而不是**这是谁的表**。

**槽位身份不只是名字（B20 缺陷 c / B22，已修）**：`vtable` 的"这是谁的槽"
= （裸名 + **形参类型**），不是裸名 —— 这正是 [class.virtual]/2 的覆写判据。
`class C { virtual int f(); int f(int); };` 里，非虚的 `f(int)` 会认领 `f()` 的槽
并被误标成 `virtual`，于是 `c.f(2)` 走虚调用跳进无参的 `f()`（clang rc=0、
minicc 编译 rc=0 但**运行返回 255** —— 静默算错，汇编里一点异常都看不出来）。
修法是 `VTableEntry::signature` 存形参类型链，匹配时一并比对（见 §2 Bug 4）。

★ 这个缺陷还留下一条**方法论**：只断言"守恒式"不变量（谁占槽 + 谁被直接调用 = 总定义数）
**挡不住它** —— 认错槽位后两者整体互换，计数照样配平（突变验证时单测全绿）。
有判别力的是**不对称**的那条：经基类指针的虚调用**必须**是 `callq *%rax` 间接调用。
见 tests/unit/test_member_identity.cpp 的 `VirtualCallOnPointerStaysVirtual`。

## 2. 踩坑史（四个连环 bug，全部实测复现后修复）

### Bug 1：primary 判定两处标准打架

- `processClassDecl` 基类循环：非多态分支"边扫边放"先占 offset 0，
  primary 分支又写死 offset 0 → `D : A(非多态), P(多态)` 两子对象重叠
- `computeClassLayout` 用 `bi==0`（声明序）选 primary，与上面的
  "第一个多态基类"标准不一致

**修复**：循环内不再计算 offset（删除 currentOffset），循环后统一
`[relocate]`：primary 恒占 0，其余从 primary 尾部依次 `alignTo(place,8)`。
对照 clang `DeterminePrimaryBase`（:852）/ `LayoutNonVirtualBases`（:1017）。

### Bug 2：全非多态时 [relocate] 的 place 停在 0

`place` 计算被 `if (anyPoly)` 守卫 → `D : A, B`（都非多态）时 B 被放到
offset 0，与 A 重叠（构造 B 覆盖 A.x，实测返回 70 而非 60）。

**修复**：去掉守卫，place 恒从 primary（含"视作 primary"的首个非多态基类）
尾部起算。

### Bug 3：size/align 混用 + 占位类型 + 子对象当指针（嵌套字段三连坑）

```text
class Five { bool b1..b5; int a; };
class Outer { int tag; Five f; int tail; };

修复前：+0 tag(4) +4 f(0 bytes!) +4 tail(4)  size=8   ← f 与 tail 重叠
clang：  0 tag    4 f(占4..15)    16 tail     size=20
```

三处修复：

1. **Sema**：字段注册前 `field.type = resolveType(field.type)`，
   占位类型换成注册版 → size 从 0 变真值
2. **Sema**：新增 `alignOf()`（Class 递归取成员 max，封顶 8；标量 min(size,8)），
   `computeClassLayout` 用它替代 `min(sizeInBytes(),8)`
3. **CodeGen**：
   - `emitMember` 遇类类型字段改发 `leaq offset(%rax),%rax`（取子对象地址）
   - ctor 初始化列表遇类类型字段改发"调整 this + callq 嵌套构造函数"
     （`f(7,9)` 原来是把实参裸 movq 进子对象头部）
   - 标量字段初始化按宽度选 movb/movl/movq（原来一律 movq，4 字节 int
     字段会踩坏相邻字段）

### Bug 4：次表槽里被"重造"出来的假符号（BUGS.md B20 缺陷 b）

- 收集次基类条目时，名字被按 `次基类名 + "_" + 裸名` **重造**了一遍
  （原意："这条槽属于次基类 Q，那就该叫 Q_f"）
- 但基类自己**没覆写**该虚函数时，槽里原本指向的是**更上游**基类的实现

```text
class X { virtual int f(); };       // 实现处：.globl X_f
class P : public X {};              // primary：条目原样拷贝 ⇒ X_f ✓
class Q : public X {};              // secondary：条目被重造成 Q_f ✗（没人定义 Q_f）
class Diamond : public P, public Q {};

修复前：Diamond 次表[0] = .quad Q_f   ⇒ [LINK ERROR] undefined reference to 'Q_f'
修复后：Diamond 次表[0] = .quad X_f   ⇒ ✓
```

**教训**：名字是**路径**（谁提供的实现），不是**位置**（这是谁的表）。
同一句话在本项目已经出现过三次：B13~B15（把显示名当索引）、docs/learn/23
（`toString()` 不是单射却被当缓存键）、B20（这里）。
**修法**：次表条目**原样透传**（`VTableEntry secEntry = baseEntry;`）；
真正的覆写发生在后面的方法循环里 —— 那时才把槽改指本类实现并配 thunk。

**现场**：这个 bug 一直藏在 `tests/mi/test_mi_04_error.cpp` 里。那个文件当时写的是
菱形继承，期望"重复基类被拒收"，实测 clang rc=0（非虚继承的菱形在标准下**合法**，
只有成员访问歧义才报错）——rc=1 的真原因是这条 `undefined reference to 'Q_f'`。
修好 B20 后该文件改为真正的错误用例（私有继承），菱形正例搬到
`tests/mi/test_mi_12_secondary_inherited_slot.cpp`。
**又一次印证**：logdiff 基线会把**失败**也固化成契约，修完必须回头看 rc。

### Bug 5：槽位身份与符号定名的同一条判据（B20 缺陷 c + B22，已修）

上面 Bug 4 暴露出"槽位匹配只比裸名"⇒ 非虚的 `f(int)` 认领虚的 `f()` 的槽、并被误标
`virtual`。实测 `C c; return c.f() + c.f(2) - 3;`：clang rc=0，minicc 编译 rc=0 但
**运行返回 255**（静默算错）。

把这条往外推一步就是 B22 的另三处现场 —— **同一句"这是哪个函数"总共写在四个地方**：

| # | 落点 | 修前 | 修后 |
|---|---|---|---|
| ① | 汇编符号名 `memberMethodSymbolName` | 后缀只编码形参**个数** ⇒ 同签名个数的重载都叫 `C_f_1`，as 报 `symbol is already defined` | 有兄弟时再挂**形参类型链**（`C_f_1_int` / `C_f_1_S`） |
| ② | vtable 槽位身份 `processClassDecl` | 只比裸名 | 再比 `entry.signature` |
| ③ | 成员调用选定 `findMethodInClass` | 循环内首个同名同个数命中即返回 | 收候选集 → `pickBestByArgs` 择优 |
| ④ | 构造函数选定 `processVarDeclStmt` | **只数个数**，且把刚推出来的实参类型扔掉 | 同走 `pickBestByArgs` |

③④ 共用同一个本地静态函数 —— 承 B10/B12 那条铁律：**同一判据不许写两份**。
回归：`tests/lang/test_basics_04_*`、`tests/mi/test_mi_13_*`、
单测 `tests/unit/test_member_identity.cpp`（6 例，三条突变逐条实跑变红）；
既有 116 个集成用例**逐字节零漂移**（符号名只在"有兄弟"时才挂类型链）。

## 3. clang 源码对照表

| clang（RecordLayoutBuilder.cpp） | minicc 对应位置 | 简化了什么 |
|---|---|---|
| `DeterminePrimaryBase` :852 | `processClassDecl` [relocate] 块（semantic_analyzer.cpp:673 起） | 无虚继承/无空基类优化 |
| `LayoutNonVirtualBases` :1017 / `LayoutBase` :1198 | 同上，`sub.offset = alignTo(place,8)` | 子对象一律 8B 对齐（clang 按 nvalign） |
| `LayoutField` :1850（TI.Width/TI.Align 分离） | `computeClassLayout` 字段放置循环 + `alignOf()` | 无位域/无 tail padding 复用 |
| `UpdateAlignment` :2213（成员 align 递归 max） | `alignOf()` Class 分支 | 封顶 8，无 16B SSE 对齐 |
| `FinishLayout` :2135（nvsize/一次成型） | `computeClassLayout` 尾部 + :891 回填 | 两段式（decl->fields 工作清单 → 整体覆盖），clang 是不可变 ASTRecordLayout |
| ASTContext::getASTRecordLayout（complete 保证） | 字段注册处 `resolveType(field.type)` | 无递归按需构建，靠声明序（基类必须先定义） |
| CGRecordLayoutBuilder 链式访问 GEP 折叠 | `emitMember` 逐层 leaq | 保留每一跳便于观察 |
| `CodeGenModule::EmitVTable`（槽放刚发射的 GlobalDecl） | `processClassDecl` 的条目循环 + `codegen.cpp` 次表段发射 | 无句柄，槽里是**拼出来的字符串**（故判据必须单点，见 §2 Bug 4）|
| `ItaniumMangleContext::mangleName` | `memberMethodSymbolName()`（semantic_analyzer.cpp:1445） | 简化规则：`类名_方法名[_<形参个数>]`，不编码形参类型 |

## 4. 可复现实验

```bash
# oracle：看 clang 的真实布局
clang++-18 -Xclang -fdump-record-layouts -c tests/mi/test_mi_07_nested_class_field.cpp -o /dev/null

# minicc：观察 [relocate]/布局日志 + 运行验证
./build-linux/minicc tests/mi/test_mi_06_nonpoly_only.cpp -o /tmp/m6 && /tmp/m6; echo $?   # 60
./build-linux/minicc tests/mi/test_mi_07_nested_class_field.cpp -o /tmp/m7 && /tmp/m7; echo $? # 9

# Bug 4（B20 缺陷 b）：次表槽该指谁 —— 看汇编里的 .quad 那一行
#   注：-S 且不给 -o 时，汇编落在【源文件旁边的同名 .s】（tests/mi/*.s 即由此而来）
./minicc tests/mi/test_mi_12_secondary_inherited_slot.cpp -S > /dev/null
grep -n "secondary\[" tests/mi/test_mi_12_secondary_inherited_slot.s   # X_f / X_g_1（不是 Q_f）

# Bug 4 的两个 oracle 对照（clang 全 rc=0）
clang++-18 -std=c++20 tests/mi/test_mi_12_secondary_inherited_slot.cpp -o /tmp/c12 && /tmp/c12; echo $?
clang++-18 -std=c++20 tests/lang/test_basics_03_virtual_with_params.cpp -o /tmp/c03 && /tmp/c03; echo $?

# 回归红线
for i in 01 02 03 04 05 06 07 08 09 10; do
  ./build-linux/minicc tests/tmpl/test_tmpl_${i}_*.cpp -o /tmp/t && /tmp/t; echo "tmpl_$i=$?"
done
```

## 5. 关键过程 ASCII 图

```text
D : A(非多态), P(多态) —— [relocate] 摆放过程：

bases 收集（循环内，不算 offset）:      [A: non-poly] [P: primary]
                                              │
[relocate] 第一步：找 primary ────────────────┘
   第一个 hasVTable 的基类 = P → P.offset := 0, place := P.totalSize(16)
[relocate] 第二步：其余子对象依次摆放
   A.offset := alignTo(16,8) = 16, place := 16 + 4 = 20
[computeClassLayout] 第三步：字段落位
   _vptr@0  p@8(复用 P 内偏移)  A.x@16(=base.offset+bf.offset)
   d@24(自身字段, alignTo(20,8)=24)  → totalSize = 32

Outer{int tag; Five f; int tail}（Five{bool×5,int a}）:

   resolveType 后 f.type → 注册版 Five（size=12, alignOf=4）
   tag:  alignTo(0,4)=0   → +0,  place=4
   f:    alignTo(4,4)=4   → +4,  place=4+12=16     ← align 取 4（成员 int a）
   tail: alignTo(16,4)=16 → +16, place=20
   totalSize = alignTo(20,4) = 20                   ← 与 clang 完全一致

o->f.a 的代码生成（逐层地址折叠）:
   movq -16(%rbp), %rax     # o
   leaq 4(%rax), %rax       # &o->f   ← 类类型字段：取地址不是取值
   movl 8(%rax), %eax       # f.a     ← Five 内 a 的偏移
```

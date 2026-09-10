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

## 2. 踩坑史（三个连环 bug，全部实测复现后修复）

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

## 4. 可复现实验

```bash
# oracle：看 clang 的真实布局
clang++-18 -Xclang -fdump-record-layouts -c tests/mi/test_mi_07_nested_class_field.cpp -o /dev/null

# minicc：观察 [relocate]/布局日志 + 运行验证
./build-linux/minicc tests/mi/test_mi_06_nonpoly_only.cpp -o /tmp/m6 && /tmp/m6; echo $?   # 60
./build-linux/minicc tests/mi/test_mi_07_nested_class_field.cpp -o /tmp/m7 && /tmp/m7; echo $? # 9

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

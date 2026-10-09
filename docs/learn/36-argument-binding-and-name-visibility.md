# 36 · 实参绑定与名字可见性：一轮"该报不报 / 不该拒却拒"的收口

> 本轮修的五个缺陷（BUGS.md B21 / B24 / B25 / B26 / B27）看起来散，
> 其实是一条线上的四种错法：**"这个名字在这个位置可见吗"** 与
> **"这个实参绑得上那个形参吗"** —— 两个判据此前都只写了一半，
> 于是同一个程序在两个方向上被错待：
> 该拒的静默放行（**接受非法程序**），不该拒的当场拒收（**拒收合法程序**）。
>
> 判据的**缺失**比判据的**写错**更难发现：写错会报错，缺了什么都不发生。

---

## 0. 起因：五个实测

先把验收面摆清楚 —— 每一条都以 `clang++-18 -std=c++20` 为 oracle 实测：

| # | 程序 | clang | minicc（修前） | 错的方向 |
|---|---|---|---|---|
| ① | `template<class T> int id(const T&); id(5)` | rc=0 | rc=1 `cannot bind non-const lvalue reference ... to an rvalue` | 拒收合法程序 |
| ② | `int f(int&&); f(a)`（`a` 是左值） | rc=1 | rc=0 | 接受非法程序 |
| ③ | `struct C{int f();}; c.f(1)` | rc=1 `too many arguments` | rc=0 | 接受非法程序 |
| ④ | `struct C{int f(int); int f(S);}; c.f(p)`（`p` 是 `int*`） | rc=1 `no matching member function` | rc=0，**静默调 `f(int)`** | 接受非法程序 + 静默算错 |
| ⑤ | `struct D:A{int f();}; d.f(7)`（`f(int)` 在 `A`） | rc=1（名字隐藏） | rc=0，调到了 `A::f(int)` | 接受非法程序 |

另有 **B21**（限定名成员 `S::v` 报 `Undefined variable`，拒收合法程序）——
它是另一条线（查找的**范围**），本文 §4 讲。

①②③④⑤ 全是"**判据缺一半**"：同一件事在两个地方各判一次，只有一处判。
这正是本项目反复出现的 B10 / B12 / B13~B15 / B20 / B22 那条老病根
（"同一判据不许写两份"），只是这一轮的两份**分得更开** —— 模板路径 vs 非模板路径。

---

## 1. 值类别：判据的第一个输入

### 1.1 理论

引用初始化（[dcl.init.ref]）**不是类型匹配**，而是"类型 + 值类别"两件事：

| 形参 | 实参 lvalue | 实参 rvalue | 依据 |
|---|---|---|---|
| `T&` | ✅ | ❌ | [dcl.init.ref]/5 |
| `const T&` | ✅ | ✅（延长生存期） | [dcl.init.ref]/5.4.2 |
| `T&&` | ❌ | ✅ | [dcl.init.ref]/5.3 |
| `T&&`（T 是模板形参） | ✅ | ✅ | 转发引用，经引用折叠退化 |

最后一行是**唯一**的例外，也是本项目 S2 轮做过的部分 —— 而正因为那一轮把
检查写在了 `deducePair` 里，**非模板调用那条路上一行都没有**（见 §2）。

值类别本身，本项目用的是简化模型（[basic.lval] 的粗粒度版）：

```text
有确定内存地址的表达式 ⇒ 左值        变量 / 成员访问 / 解引用 / 下标
其余                   ⇒ 右值        字面量 / 算术结果 / 调用返回值
```

★ 第一版模型只写了"变量 / 成员访问"，**漏掉解引用与下标** ——
[expr.unary.op]/1 与 [expr.sub]/1 都明确规定二者产生左值，
漏掉的直接后果是 `void g(int&); g(*p);` 被引用绑定检查误拒。
判据要么按标准写全，要么不如不写 —— 半份判据会**造出新的**拒收合法程序。

> 注：本项目对下标 `v[i]` 走的是 `at()` 语法糖，`at()` 的返回类型若是 `T&`
> 则 `v[i]` 为左值。这一条在实现上按"节点种类"判，不追返回类型 —— 见 §7 边界表。

### 1.2 判据单点

判据落在 `include/type.h` / `src/type.cpp`，**全项目仅此一处**：

```cpp
// include/type.h（尾部）
bool referenceBindsValueCategory(const TypePtr& paramType, bool argIsLValue,
                                 std::string* why = nullptr);
```

两个调用点：

| 调用点 | 位置 | 覆盖 |
|---|---|---|
| 模板推导 | `TemplateDeducer::deducePair` 的 P=LRef 分支 | `template<class T> f(const T&)` |
| 非模板调用 | `inferCall` 里的 `checkArgBinding` lambda | `f(const int&)`、`f(int&&)` |

★ 谓词与报错**分开**：`argsBindTo(f, why, badIdx)` 是判据，`checkArgBinding(f)` 只是它
一个"报错"的调用者。为什么非得拆 —— 因为**绑定是可行性的一部分**（[overload.viable]：
隐式转换序列不可行的候选不算候选），必须在**择优之前**就把不可绑定的候选剔掉。
否则 `f(int&)` 与 `f(const int&)` 并存时 `f(5)` 会先按"平手取注册顺序靠前者"
选中 `f(int&)`、再被绑定检查拒掉 —— 而 clang 的答案是选 `f(const int&)`。
**先剔后退，两条都对；先选后查，会拒掉合法程序。**

> 注：自由函数重载这条路眼下测不了（B7：自由函数重载没有 mangling，两个 `f`
> 在汇编期就撞符号了）。成员重载那条路由 `pickBestByArgs` 择优，它按
> "精确 → 可隐式转换"两档走，同样**不做**转换等级排序（ROADMAP 主线 D）。

日志形态（模板路径，`id(5)` 与 `id(a)` 都会走这里）：

```text
  [deduction] ▶ id — 模板参数 <T>，实参 1 个
  [deduction]   P=const T      A=int          ⇒ 剥顶层 const
  [deduction]   P=T            A=int          ⇒ T := int
  [deduction] ◀ 推导成功: <T=int>
```

失败时的文案（②）：

```text
[ERROR] [Semantic Error] 2:33: no matching function for call to 'f' — cannot bind
rvalue reference 'int&&' to an lvalue ('int')
```

---

## 2. 名字可见性与可行性：两个正交的筛选

成员调用查找要经过**两道互相独立的筛选**，顺序不能换：

```text
        c.f(1)
          │
   ┌──────▼──────────────────────────────────────────┐
   │ ① 名字可见性 [class.member.lookup]/3            │
   │    · 从对象类开始沿基类链 BFS                    │
   │    · 一旦某个类【声明了这个名字】⇒ 停            │
   │      （基类中的【所有】同名声明被隐藏，          │
   │        哪怕签名毫不相干）                        │
   │    产出：候选声明集（可见的那一族）              │
   └──────┬──────────────────────────────────────────┘
          │  候选集为空 ──► 换基类继续；都空 ⇒ No member 'f'
   ┌──────▼──────────────────────────────────────────┐
   │ ② 可行性 [overload.best.viable]                 │
   │    · 逐个实参能否形成隐式转换序列                │
   │      （个数是第一道可行性判据）                  │
   │     产出：可行集 → 最优                          │
   └──────┬──────────────────────────────────────────┘
          │  可行集为空 ──► **非良构，必须报错**
          └──────────────────────────────────────────► 选定 ← CodeGen 用
```

### 2.1 ① 没做到：名字隐藏失效（B24）

`findMethodInHierarchy` 的遍历条件是"本类找不到这个名字才往基类走"，
而"找得到"的判据是**裸名**。于是"派生类一个 `f()` 挡不住基类 `f(int)` 整族"：

```cpp
struct A { int f(int x) { return x; } };
struct D : A { int f() { return 1; } };
D d; d.f(7);      // clang: too many arguments ... did you mean 'A::f'?
```

隐藏是**按名字**发生的，与签名无关 —— 这一点最容易记反
（直觉会觉得"签名不同就是两个函数"）。它和 ② 的按类型择优是**两个方向**的筛选：
隐藏先砍掉一整族，可行性再从中挑。

### 2.2 ② 没做到：筛空了没人下结论（B25 / B27）

两种"筛空"：

| 情形 | 例子 | clang 文案 |
|---|---|---|
| 个数不符 | `struct C{int f(int x);}; c.f()` | `too few arguments to function call, single argument 'x' was not specified` |
| 个数不符 | `struct C{int f();}; c.f(1,2)` | `too many arguments to function call, expected 0, have 2` |
| 类型不可行 | `f(int)` / `f(S)` 收 `int*` | `no matching member function for call to 'f'` |

**为什么静默**：落空之后还有好几条兜底通路 —— 自由函数查找 → ADL → 模板 →
最后 `inferType(expr.callee)` 兜回来，日志自己都写着
`[call] non-template 'f' arg count mismatch (0 vs 2), keep searching`，
而 **"keep searching" 后面没有终点**。落到 CodeGen 时符号为空 ⇒ 硬拼 `类名_方法名`
⇒ 0 参方法恰好拼对（静默接受非法程序），带参方法拼错（链接期
`undefined reference to 'C_f'`，一个字不提"个数不对"）。

修法：**查到名字就下结论**。可见集非空 ⇒ 要么报个数（区外），要么报类型（区内），
**不往基类走、也不落进普通查找**。文案按 clang 的两类分开：

```text
[ERROR] [Semantic Error] 2:29: too many arguments to function call, expected 0, have 1
[ERROR] [Semantic Error] 3:53: no matching member function for call to 'f' in class 'C' (1 argument(s) given; 实参类型无法按 [overload.viable] 转换)
```

个数区间由"本类为这个名字声明过哪些参数个数"的 `[lo, hi]` 决定，
**逐字对齐** clang 的四种句式（多形参 / 单形参各有说法）。

---

## 3. 一处早该修的连带发现：基线会把非法程序固化成契约

B24 修好之后，`tests/unit/test_member_identity.cpp` 的
`DerivedOverrideKeepsSlotCount` **当场变红** —— 那个用例里写的是：

```cpp
class Base { public: virtual int f() { return 1; } int f(int x); };
class Derived : public Base { public: virtual int f() { return 100; } };
Derived d; Base* p = &d; return p->f() + d.f(2);   // ← d.f(2) 是非法程序
```

clang 对它 rc=1（就是 B24 那条规则），minicc 此前 rc=0 ⇒ 用例"通过"了。
改成 `p->f(2)`（经基类指针调，不触发隐藏）后既合法又保住原意。

这是本项目**第三次**踩同一个坑（前两次：`tests/mi/test_mi_04_error.cpp`
把次基类假符号的链接失败被解释成"菱形本该被拒收"；
`tests/pp/test_pp_01_include.cpp` 的 `#include` 路径写错导致该特性从未被真正测到）。
**教训**：写用例时先用 clang 过一遍"这程序真的合法吗"，别让编译器替你回答。

---

## 4. 限定名访问成员数据（B21）：查找的**范围**

### 4.1 现象

```cpp
struct S { int v; int g() { return S::v; } };   // 类内限定名 → 隐式 this
struct Base { int v; };
struct Derived : Base { int v; int h() { return Base::v; } };
Derived d; d.Base::v = 3;                        // 类外限定名
```

写限定名的**唯一用途**是绕过名字隐藏（§2.1）—— 派生类声明了 `v`，基类的 `v`
只剩这一种取法。修前：类内报 `Undefined variable 'S::v'`；
类外**停在语法期**（Parser 不认 `.` 后面跟 `A::B`）。

### 4.2 clang 把这件事建模成什么

`S::v` 在成员函数体内**不是**限定名查找，而是 [expr.prim.id.general]/3 的
**隐式 this 成员访问**：

```text
`-ReturnStmt
    `-MemberExpr  'int' lvalue ->v      ← 注意是 ->v，base 是隐式 this
        `-CXXThisExpr  'S *' implicit this
```

而 `Derived d; d.Base::v` 是 `MemberExpr` 的嵌套名限定形式（`NestedNameSpecifier`）。
本项目两条语法各有一半：

| 形态 | clang | 本项目（Parser 的处置） |
|---|---|---|
| `Base::v`（类内） | MemberExpr + 隐式 this | 拼成名字串 `"Base::v"` 的 VarExpr |
| `d.Base::v`（类外） | MemberExpr + NNS | 成员名拼成 `"Base::v"` 的 MemberExpr |
| `N::S` / `N::get(s)` | DeclRefExpr + NNS | 见 docs/learn/26（命名空间那条通路） |

★ 有意思的方向错位：本项目唯一从 `NodeKind::Member` 走过的限定名是
`Cls<int>::value`（静态常量折叠），而它在 clang 里是 **DeclRefExpr**，不是成员表达式。
"clang 里是 MemberExpr 的形态我们收不到，我们造 MemberExpr 的形态 clang 不是。"

### 4.3 修法：解析到字段，把**位置**交给机器

查找路径与裸名**完全同一条**（字段查找 + 隐式 this），只是查找的**范围**
被限定在 `Q` 之内。定位原语一处，两个调用点共用：

```cpp
const FieldInfo* resolveQualifiedField(ownerClass, qualifier, member);
```

"这条字段归不归 `qualifier` 管"三个条件（为什么不是一个条件，见 B13~B15 的教训）：

| # | 条件 | 覆盖的情形 |
|---|---|---|
| ① | `sourceClass == qualifier` | 继承自它（主基类合并时 `sourceClass` 被补成基类名） |
| ② | `sourceClass` 为空且 `qualifier` 就是本类 | 本类自己的字段 |
| ③ | `qualifier` 是 `sourceClass` 的祖辈 | 字段声明在 Q，经若干层才带到本类布局 |

★ **关键一步是回填**：写限定名要求"偏移取 Q 那一条"，而按裸名查会命中派生类
自己那一条（`findField` 的"自身字段优先"）—— 于是解析完必须把**权威偏移**
写回节点（`VarExpr::resolvedFieldOffset` / `MemberExpr::resolvedFieldOffset`），
CodeGen 优先采用。**名字是给人看的，位置才是机器要的**（同 B10 / B20 / B22）。

实测日志与汇编（`Base::v` 与裸名 `v` 落在**不同**的槽位）：

```text
  [resolve] 'Base::v' → int    (qualified member, offset=0 隐式 this)
  [resolve] 'v' → int    (class field, offset=8)
    [member] Derived.Base::v → int    (qualified member, offset=0)
    [member] Derived.v → int    (offset=8, size=4)
```

```asm
    movl 0(%rax), %eax    # 读取限定名成员 .v（偏移 +0 字节）    ← Sema 回填
    movl 8(%rax), %eax    # 读取字段 .v（偏移 +8 字节）          ← 按名字查布局
    movl %eax, 0(%rcx)    # 写入限定名成员 .v（偏移 +0）
    movl %eax, 8(%rcx)    # 写入字段 .v（偏移 +8）
```

---

## 5. clang 源码对照表

| 本实现 | clang 对应物 | 简化了什么 |
|---|---|---|
| `referenceBindsValueCategory`（type.h/type.cpp，判据单点） | `Sema::CheckReferenceInit` + `Sema::InitializeReference`（SemaInit.cpp） | 不做用户定义转换、不做派生类到基类的引用绑定、不延长生存期（本项目无栈对象生存期管理器） |
| `inferCall` 的 `checkArgBinding`（非模板调用点） | `Sema::BuildCallExpr` → `CheckFunctionCall`（SemaExpr.cpp） | 不做转换等级排序（[overload.icp]），只做"能/不能" |
| `TemplateDeducer::deducePair` 的 P=LRef 分支 | `Sema::DeduceTemplateArguments` 的 `Type::LValueReference` 情形（SemaTemplateDeduction.cpp） | 同一套判据的另一个调用点 |
| 值类别（节点种类判定） | `Expr::getValueKind()`（Expr.h，`VK_LValue/VK_XValue/VK_PRValue`） | 本项目只有 L/R 两态、且按**语法节点种类**判，不追语义（如 `at()` 的返回类型） |
| 成员调用的"查到名字就停" | `LookupResult` + `Sema::LookupName`／`LookupMemberName`（名字隐藏由 `DeclContext::lookup` 天然保证） | 本项目手写 BFS；clang 的隐藏是作用域数据结构的**性质**，不需要额外判据 |
| 个数诊断 `too few/too many arguments` | `Sema::CheckFunctionCall`（SemaExpr.cpp，按 `FunctionDecl` 的形参表与实参表比） | 文案逐字对齐，但本项目不支持默认实参与形参包 |
| `resolveQualifiedField` + 偏移回填 | `Sema::BuildMemberExpr`（`NestedNameSpecifier` 那条路径）+ `FieldDecl::getFieldIndex()` | clang 的成员是 `FieldDecl*` 句柄，位置天生跟着句柄走；本项目用"回填偏移"补上这一层 |

---

## 6. 可复现实验

```bash
cd /root/cppproject/mycompiler

# ① const T& 收右值 / ② int&& 拒左值（同一个判据的两个调用点）
cat > /tmp/a.cpp <<'EOF'
template<class T> int id(const T& x) { return x; }
int take(int&& r) { return r; }
int main() { int a = 1; return id(5) + take(a); }
EOF
./minicc /tmp/a.cpp          # rc=1，报 cannot bind rvalue reference 'int&&' to an lvalue

# ③ 个数不符（clang 文案逐字）
printf 'struct C{int f(int x){return x;}};int main(){C c;return c.f();}\n' > /tmp/b.cpp
./minicc /tmp/b.cpp | grep ERROR
clang++-18 -std=c++20 -fsyntax-only /tmp/b.cpp 2>&1 | head -1   # 对齐看

# ④ 无可行候选（修前静默调 f(int)）
printf 'struct S{int v;};class C{public:int f(int x){return x;}int f(S s){return s.v;}};int main(){C c;int a=1;int*p=&a;return c.f(p);}\n' > /tmp/c.cpp
./minicc /tmp/c.cpp          # rc=1, no matching member function

# ⑤ 名字隐藏
printf 'struct A{int f(int x){return x;}};struct D:A{int f(){return 1;}};int main(){D d;return d.f(7);}\n' > /tmp/d.cpp
./minicc /tmp/d.cpp          # rc=1, too many arguments to function call, expected 0, have 1

# B21 限定名成员（含隐藏场景）——日志与汇编都要看
cat > /tmp/e.cpp <<'EOF'
struct Base { public: int v; };
struct Derived : public Base { public: int v; int h() { return Base::v; } };
int main() { Derived d; d.Base::v = 3; d.v = 4; return d.h() + d.Base::v + d.v - 10; }
EOF
./minicc /tmp/e.cpp -S -o /tmp/e.s && grep -E "读取限定名成员|读取字段" /tmp/e.s
# 运行：rc=0（3+3+4-10）

# 回归三件套
./build.sh && ctest --test-dir build-linux && ./logdiff.sh diff
./build-linux/unit_tests --gtest_filter='ArgBinding.*'
```

---

## 7. 边界表（本轮**有意未做**，按组合列）

| 未做项 | 现状 | 归属 |
|---|---|---|
| 限定名调用**成员函数** `d.Base::f(2)` / `this->Base::f(2)` | 类内 `Base::f()` 走既有 mangled 分支勉强通；`d.Base::f(2)` 报 `No member 'Base::f'` | 缺口（与 B21 同族但属**函数**那半边） |
| `v[i]` 的值类别按 `at()` 返回类型判定 | 按节点种类判为左值 | 语义精确化，需先有"函数返回引用"的完整表达 |
| 派生类到基类的引用绑定 `Base& b = d;` | 无此检查 | [dcl.init.ref]/5.4.1 |
| 转换等级排序（[overload.icp]） | 只有"能/不能"两档 | ROADMAP 主线 D |
| `using Base::f;` 把被隐藏的名字拉回来 | 不支持（Parser 不认 using 声明） | 与 B24 配套的另一半 |
| 默认实参导致的"个数区间" | 只按声明的形参个数算 | 函数默认实参（未做项） |
| 命名空间限定名的数据成员 `N::g_var` | 走符号表（§4.2 的第三行） | docs/learn/26 |

---

## 8. 三条教训

1. **判据的"缺失"比"写错"更难发现**。写错会报错；缺了什么都不发生 ——
   本轮五个缺陷里有四个是"缺一半"（模板有、非模板没有；有筛选、没结论）。
   自检方式：**把判据的名字写出来，然后 grep 全项目**。`referenceBindsValueCategory`
   若早先存在，B26 就不可能发生（引用绑定的名字只有一个，grep 一下就知道谁没调它）。

2. **半份判据会造出反方向的缺陷**。值类别模型漏掉解引用与下标 ⇒
   `g(*p)` 被误拒。加检查之前先问："这个判据按标准写全了吗？"
   写不全的部分宁可显式留边界（§7），也别让它装作完整。

3. **名字是给人看的，位置才是机器要的**。B21 的修法再次落到这句上
   （第三次：B10 的符号回填、B13~B15 的 `declaredName`/`viaBase`、
   B22 的 `paramTypeChain`）。限定名解析出**偏移**并回填，
   而不是把名字改写成裸名交给下游再查一遍 —— 后者在隐藏场景下必然查错。

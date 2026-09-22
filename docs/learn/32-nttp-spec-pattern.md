# 32 · 特化模式里的非类型位：enable_if 的地基

> 主题：`specPattern` 从「类型数组」升级为「实参数组」，让 `enable_if<true, T>` 这类
> 「用 NTTP 选中特化」的惯用法成立。
> 对应标准：[temp.class.spec]、[temp.class.spec.match]、[temp.arg.nontype]。
> 对应编译原理：**受限合一（restricted unification）**——模式位是常量时不参与绑定，
> 只做一致性校验；这是合一算法里「常量 vs 变量」这一最基本区分的直接体现。

---

## 1. 理论背景

### 1.1 模式位可以是任何模板实参

[temp.class.spec]/1 规定偏特化的实参表（`X<...>` 里的东西）**与普通模板实参同构**：

```cpp
template <class T, int N> struct X;          // 主模板

template <class T> struct X<T*, 4> { };      // 模式位：类型 T* + 值 4
```

这一位写 `4` 还是写 `N`，是**两种不同的东西**：

| 模式位写法 | 含义 | 匹配行为 |
|---|---|---|
| `T` / `T*`（含形参） | 变量 | 参与合一，可绑定量 |
| `4` / `true`（常量） | 常量 | **不绑定**，只与实参位逐位判等 |

后者正是 `enable_if` 的地基——先看惯用法本身。

### 1.2 enable_if：用值位做「编译期开关」

```cpp
template <bool B, class T = void>
struct enable_if { };                        // 主模板：没有 type 成员

template <class T>
struct enable_if<true, T> { using type = T; };// ★ 只对 B == true 有 type
```

于是 `typename enable_if<B, T>::type` 的存在性本身就是 `B` 的谓词：

- `B == true` ⇒ 命中偏特化 ⇒ `type` 存在 ⇒ 探测成功；
- `B == false` ⇒ 只有主模板 ⇒ 没有 `type` ⇒ 在**直接上下文**里软失败（SFINAE）。

★ 关键推论：**值位必须真的比**。如果匹配时不比值（比如"凡值位一律放行"），
`enable_if<true, T>` 与 `enable_if<false, T>` 会同时匹配 ⇒ 只能报歧义，
且 `enable_if_t<false, T>` 会意外解析成功——整个 SFINAE 协议失效。

### 1.3 值的「形态」也是实参的一部分

[temp.arg.nontype]/1：非类型实参的**类型**由对应形参的类型决定。
`Flag<true>`（bool 形参）与 `Buf<1>`（int 形参）数值都是 1，但不是同一个实参：

| 实参 | 形参 | Itanium 编码 |
|---|---|---|
| `Buf<1>` | `int N` | `Li1E` |
| `Flag<true>` | `bool B` | `Lb1E` |

故 `TemplateArg` 携带 `valueType`（形态），`equals()` 把形态纳入判断。

### 1.4 为什么"值位不绑定"是合一的自然结论

把匹配看成解方程：模式是含未知量的表达式，实参是已知值。

```text
模式 <true, T>      实参 <1, int>
     ↑ 无未知量          ↑ 常量
```

第 0 位两边都是**闭项**（ground term）⇒ 合一退化为**相等检查**，
没有任何变量可解。第 1 位左式是变量 `T` ⇒ 绑成 `int`。

这与 prolog 里 `foo(true, X) = foo(true, int)` 的解完全一致：
`true` 是原子、不是变量，故不产生绑定。**一阶合一把"常量"与"变量"分开处理，
本实现里的对应物就是 `TemplateArg::isValue()` 这一个判断。**

---

## 2. 数据流（ASCII 图）

```text
        源码  enable_if<true, int> e;
              │
              ▼
   ┌──────────────────────────┐
   │ Parser                   │  模式位与实参位都走同一个
   │ parseTemplateArgumentList│  parseTemplateArgumentList ⇒ 天然同构
   └──────────┬───────────────┘
              │  specPattern : vector<TemplateArg>   ← ★ 本次改型
              │  args        : vector<TemplateArg>
              ▼
   ┌──────────────────────────────────────────────┐
   │ Sema::selectClassTemplate                    │
   │   ① 全特化：specPattern[i].equals(args[i])   │  值位/类型位统一走 equals
   │   ② 偏特化：deducer.matchPattern(pattern,args)│
   │   ③ 主模板兜底                                │
   └──────────┬───────────────────────────────────┘
              │
              ▼
   ┌──────────────────────────────────────────────┐
   │ TemplateDeducer::matchPattern                │
   │   for i in 0..n:                             │
   │     pattern[i].isValue() ?                   │
   │        ├─ 是：equals(args[i]) ? continue : ✗ │  ← ★ 新增分支（不绑定）
   │        └─ 否：reducePattern → deducePair     │  ← 原有路径（绑定）
   └──────────┬───────────────────────────────────┘
              │  { T := int }
              ▼
        实例化 enable_if_1_int  →  _Z9enable_ifILb1EiE
```

---

## 3. 实现改动清单

| # | 文件:位置 | 改动 | 为什么必须改 |
|---|---|---|---|
| 1 | `include/ast.h` · `TemplateDecl::specPattern` | `vector<TypePtr>` → `vector<TemplateArg>` | 旧类型**表达不了**值位；这是根因 |
| 2 | `src/parser.cpp` · `parseClassDecl(outSpecPattern)` | 直接用 `parseTemplateArgumentList()` | 模式与实参走同一解析器 ⇒ 形态自动对齐 |
| 3 | `src/template_deduction.cpp` · `matchPattern` | 新增值位分支 + 形态错位守卫 | 值位比值不绑定；错位响亮失败而非踩空 |
| 4 | `src/semantic_analyzer.cpp` · `selectClassTemplate` | **拆掉** "含值实参就跳过特化"守卫 | 拆守卫才是功能开关：不拆则 enable_if 永远走主模板 |
| 5 | `src/semantic_analyzer.cpp` · 全特化循环 | `specPattern[i]->equals()` → `.equals()` | 全特化同样支持值位模式（`template <> struct Flag<0>`） |
| 6 | `src/main.cpp` · Phase 4 演示分支 | 新增「首形参是 NTTP」分支 | 否则 `template<bool B, class T>` 被喂 `<int,double>` ⇒ 抛形态错 |
| 7 | `src/parser.cpp` · blueprint summary | 用 `templateParams` 而非 `typeParams` | PITFALLS **I4**：NTTP 被打成 `typename N` |

### 3.1 为什么第 4 项是「拆守卫」而不是「加分支」

改动前 `selectClassTemplate` 开头这样写：

```cpp
std::vector<TypePtr> argTypes;
bool allTypeArgs = true;
for (const auto& a : args) {
    if (!a.isType()) { allTypeArgs = false; break; }   // ← 值实参 ⇒ 放弃特化
    argTypes.push_back(a.type);
}
if (allTypeArgs) { ...特化匹配... }
else { std::cout << "实参含非类型值，跳过特化匹配\n"; }
```

这条守卫在 **specPattern 只含类型** 的前提下是**对的**——留着它，`enable_if<true,T>`
的实参含值 ⇒ 直接跳过全部特化 ⇒ 走主模板 ⇒ 没有 `type` 成员 ⇒ 探测永远失败。
功能开关就在这一行，不在推导器里。

★ 教训：**同一个简化假设会同时写进"数据结构的类型"和"使用它的守卫"里**；
放宽假设时要两处一起动，只改一处会得到一个编译通过但功能全错的结果。

### 3.2 值位分支要防两种"踩空"

`matchPattern` 的循环体后面会按裸 `TypePtr` 取用 `args[i]`（引用结构检查、`deducePair`）。
若实参位是值（`TemplateArg{value}`），其 `type` 是空指针。故新增两道守卫：

```cpp
if (pattern[i].isValue()) {          // ① 模式值位：比值，不绑定
    if (!args[i].isValue() || !pattern[i].equals(args[i])) { ...; return false; }
    continue;
}
if (!args[i].isType()) {             // ② 模式类型位配到值实参
    ...; return false;               //    响亮失败，而不是拿空指针往下走
}
```

这正是《PITFALLS》里反复出现的模式：**结构变了，就要在新边界上把关**。

### 3.3 Phase 4 的演示分支：判据必须逐位看形参

`src/main.cpp` 的"多种实例化演示"按形参个数分派。旧版两形参分支只看
`templateParams[1].kind`：

```cpp
if (p2.kind == NonType) { ... <int, 8> ... }
else                   { ... <int, double> ... }   // ← 首形参是 NTTP 时落这里
```

`template<bool B, class T>` 因此被喂 `<int, double>` ⇒ 类型实参塞进 NTTP 槽 ⇒
实例化抛 `must be a value, but 'int' is a type`。

修法：**先看 `templateParams[0].kind`**，首形参是 NTTP 时走
`<true, int>`（值 + 类型混排）。同一条经验再次出现——
「只看一位就下结论」在形参表这种逐位结构上必然出事。

---

## 4. 可复现实验

```bash
# ① 端到端（期望 minicc 与 clang 都返回 0 —— bad 是第一个失败用例编号）
./minicc tests/tmpl/test_tmpl_52_nttp_spec_pattern.cpp -o /tmp/t52 && /tmp/t52; echo $?
clang++-18 -std=c++20 tests/tmpl/test_tmpl_52_nttp_spec_pattern.cpp -o /tmp/t52c && /tmp/t52c; echo $?

# ② 看值位匹配的逐位推理
./minicc tests/tmpl/test_tmpl_52_nttp_spec_pattern.cpp -S -o /tmp/t52.s 2>&1 \
  | grep -E "spec:select|deduction\]" | grep -v "\[pp\]"

# ③ enable_if_t 端到端（别名模板 + 依赖类型名 + 值位偏特化三件套齐活）
cat > /tmp/eif.cpp <<'EOF'
template <bool B, class T = void> struct enable_if { };
template <class T> struct enable_if<true, T> { using type = T; };
template <bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;
template <class T> enable_if_t<true, T> identity(T x) { return x; }
int main() { return identity(5); }
EOF
./minicc /tmp/eif.cpp -o /tmp/eif && /tmp/eif; echo $?   # 期望 5

# ④ mangling 与 clang 逐字符核对
cat > /tmp/mng2.cpp <<'EOF'
template <bool B, class T> struct enable_if { void m(); };
template <class T> struct enable_if<true, T> { void m(); };
template <int N> struct Flag { void m(); };
template <> struct Flag<0> { void m(); };
template <> void enable_if<true, int>::m() {}
template <> void enable_if<false, int>::m() {}
void Flag<0>::m() {}
template struct Flag<3>;
EOF
clang++-18 -std=c++20 -c /tmp/mng2.cpp -o /tmp/mng2.o && nm /tmp/mng2.o | grep -iE "enable|flag"

# ⑤ 全量回归（三绿：0 警告 / ctest / logdiff 零差异）
./build.sh && ctest --test-dir build-linux -j6 && ./logdiff.sh diff
```

clang 侧实测符号（`nm` 原文）：

```text
0000000000000000 T _ZN9enable_ifILb1EiE1mEv     ← enable_if<true, int>
0000000000000010 T _ZN9enable_ifILb0EiE1mEv     ← enable_if<false, int>
0000000000000020 T _ZN4FlagILi0EE1mEv          ← Flag<0>
```

本实现只编码到类名一层（不含成员函数段 `1mEv`），前缀 `9enable_ifILb1EiE` 逐字符相同。

---

## 5. 边界（本项目**未做**的）

| 缺口 | 症状 | 卡在哪 |
|---|---|---|
| 值位模式里的**形参名** | `template<int N, class T> struct X<T, N>` 的 `N` | Parser 在实参位遇到 NTTP 名会建 `Class("N")`；替换阶段有还原逻辑（见 27 §3.3），但模式位未走通 |
| 表达式型值位 | `X<T, N + 1>` | 需要实参位接受常量表达式（常量折叠，ROADMAP 主线 D） |
| 模板模板参数位 | `template <template<class> class C>` | 独立缺口 #19，`TemplateArgKind::Template` 尚未定义 |
| 值位参与**偏序**去相关 | 两个偏特化值位不同时的裁决 | 值位是常量，本就不与对方形参同名，天然无关（`classSpecAtLeastAsSpecialized` 直接原样搬运），无需合成名 |

---

## 6. clang 源码对照表

| clang（`~/cppproject/llvm-project/`） | 本实现位置 | 简化了什么 |
|---|---|---|
| `TemplateArgument` 联合体（Type/Integral/Template/…) | `TemplateArg`（`type` + `value` + `valueType`） | 只支持 Type / Integral 两态；无 structural value 的深度比较 |
| `ClassTemplatePartialSpecializationDecl::getTemplateArgs()` | `TemplateDecl::specPattern` | clang 返回 `ArrayRef<TemplateArgument>`，与本实现改型后同形 |
| `Sema::CheckClassTemplatePartialSpecializationArgs` | `Parser::parseClassDecl` + `Sema::processTemplateDecl` | 不做「模式位的每个形参都必须可推导」的静态检查 |
| `DeduceTemplateArguments`（`TDK_NonDeducedType` / 非类型实参路径） | `TemplateDeducer::matchPattern` 值位分支 | 只做同形态同值判等；不做 clang 那套 `TemplateArgument` 规范化 |
| `Sema::CheckTemplateArgumentList` 的显式特化精确匹配 | `selectClassTemplate` ① 分支 | 用 `TemplateArg::equals` 线性比，无 clang 的 profile/缓存 |
| `UniqueSynthesizedType`（偏序去相关） | `renameTemplateParams` + `$ord_` 前缀 | 值位不参与，直接搬运 |

---

## 7. 关键日志片段（`test_tmpl_52` 实跑）

`enable_if<true, int> e;` —— 两个偏特化都参与匹配，值位把 false 分支筛掉：

```text
  [sema:targ]   ✓ param 1: 'B' (non-type) ← 1
  [sema:targ]   ✓ param 2: 'T' (type) ← int
  [sema:targ] ✓ template arguments OK: enable_if<1, int>
  [spec:select] ★ selecting class template 'enable_if' for <1, int>
  [deduction]   P=1  A=1  ⇒ 值位相等 ✓（NTTP 模式位，无绑定）
  [deduction]   P=T            A=int          ⇒ T := int
  [spec:select]   ├─ ② 候选：'enable_if<1, T>' 匹配成功
  [deduction]   ✗ 值位不匹配：non-type pattern '0' does not match argument '1'
  [spec:select]   ├─ ② 候选不匹配：non-type pattern '0' does not match argument '1'
  [spec:select]   │  唯一候选 → 直接选中 'enable_if<1, T>'
  [instantiate:name] 'enable_if<1,int>' → 汇编符号前缀 'enable_if_1_int'
  ║ Mangled: enable_if_1_int → _Z9enable_ifILb1EiE
```

`OnlyTrue<false, int> u;` —— 值位不匹配 ⇒ 回落主模板（拿到 `generic` 字段）：

```text
  [spec:select] ★ selecting class template 'OnlyTrue' for <0, int>
  [deduction]   ✗ 值位不匹配：non-type pattern '1' does not match argument '0'
  [spec:select]   ├─ ② 候选不匹配：non-type pattern '1' does not match argument '0'
  [spec:select]   └─ ③ falling back to PRIMARY template
```

`Flag<0> z;` —— 全特化走 `equals` 逐位比（值位同样适用）：

```text
  [spec:select] ★ selecting class template 'Flag' for <0>
  [spec:select]   ├─ ① explicit specialization matched (exact argument equality) → USING IT
  ║ Mangled: Flag_0 → _Z4FlagILi0EE
```

`enable_if_t` 端到端（实验 ③）—— 别名模板解糖 → 依赖类型名替换 →
值位偏特化选中，三级链路贯通，minicc 与 clang 同返 5。

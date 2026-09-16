# 19 类模板特化：默认实参 / 偏特化 / 全特化

> 主题：同一个模板名下并存三种版本，靠实参择优。
> 起点是一份真实的教学代码——`Box<T,U=void>` 主模板 + `Box<T*,T>` 偏特化
> + `Box<int*,int>` 全特化，外加用户注释里那句关键提问：
> 「**这里是规定了模板需要有两个参数，如果不写 `U = void`，使用时必须提供 2 个参数**」。

## ① 理论背景

### 三种版本共存（[temp.class.spec] / [temp.expl.spec]）

```cpp
template<typename T, typename U = void> struct Box;  // ① 主模板 Primary
template<typename T>                struct Box<T*, T>; // ② 偏特化 PartialSpec
template<>                          struct Box<int*, int>; // ③ 全特化 ExplicitSpec
```

它们在**语法形态**上的区分标准只有两条：

| | 形参表 | 模板名后 | 判定 |
|---|---|---|---|
| 主模板 | 非空 | 裸名字 `Box` | 无尖括号 ⇒ Primary |
| 偏特化 | 非空 | 模式 `Box<T*, T>` | 有尖括号 + 形参非空 ⇒ PartialSpec |
| 全特化 | **空** `template<>` | 具体 `Box<int*, int>` | 有尖括号 + 形参为空 ⇒ ExplicitSpec |

对照 clang：三者是**不同的 AST 节点**——
`ClassTemplateDecl`（主模板）持有 `ClassTemplatePartialSpecializationDecl`
链表与 `ClassTemplateSpecializationDecl` 集合，由 Sema 关联。
本项目用「同一结构 + `TemplateSpecKind` 判别枚举 + 两条注册表」保持 AST 扁平。

### 择优顺序

[temp.class.spec.match] 与 [temp.expl.spec]/6 共同规定：

```
① 全特化：specPattern 与实参【逐位类型相等】  → 命中即用（最高优先）
② 偏特化：用实参【推导】specPattern（合一）   → 全位成功即匹配
③ 都不中 → 主模板 + 默认实参补全
```

①与②的**算法完全不同**——这正是本项目把两张注册表分开存的原因
（全特化没有未知量可推，做的是 `equals()` 直接比对；偏特化则要跑合一）：

```
全特化  Box<int*, int>  ←→ 实参 [int*, int]
        int* == int* ✓   int == int ✓          → 纯比较，无推导

偏特化  Box<T*, T>      ←→ 实参 [double*, double]
        P=T* 配 double* ⇒ T := double ✓
        P=T  配 double  ⇒ T := double（一致 ✓） → 合一，有推导
```

### 默认模板实参（[temp.param]/12）

```cpp
template<typename T, typename U = void> struct Box;
Box<int> b;   // 补全为 Box<int, void>
```

两条约束：

1. **只能写在主模板上**——偏特化的形参表不得有默认实参。
   所以用户注释里「哪怕下面提供特化的默认参数」这个说法不成立：
   `Box<decltype(&a)>` 能只写一个参数，完全是**主模板那一个** `U = void` 在起作用。
2. **不能有空洞**——一旦某位有默认值，其后每一位都必须有（报错见 ⑥）。

### 补全时机：必须在选择特化之前

这是实现时最容易踩的一处：

```
Box<int>  →  ① 补全成 <int, void>  →  ② 拿去匹配特化  →  ③ 回落主模板
                    ↑
              必须在这一步就补完
```

因为特化的匹配是对**完整实参表**做的：

```
template<> struct Box<int*, int>;   // 全特化，模式有 2 位
Box<int*, int> c;
```

若拿「用户写了几位」去匹配，`Box<int>` 只有 1 位实参，
连模式长度都对不上。补全后再匹配，长度与类型语义才一致。

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 三版本的表示 | 同一 `TemplateDecl` + `TemplateSpecKind` + `specPattern` | 保持 AST 扁平，与项目"用判别枚举代替继承"的既有风格一致 |
| 注册表 | `m_classTemplates`（主）+ `m_partialSpecs` / `m_explicitSpecs`（两个 vector） | 全特化与偏特化的匹配算法完全不同，混在一张表里没法写 |
| 偏特化匹配算法 | **复用 `TemplateDeducer::deducePair`** | 与函数模板实参推导是同一个合一算法，零重复代码 |
| 偏序裁决 | **不做**：多偏特化同时匹配取先注册者 | 教学上先讲清"选哪一档"，再讲"同档怎么挑"；遇歧义不静默选错 |
| 特化体替换表来源 | 由 `selectClassTemplate` 推导后**显式传入** `instantiate` | 见下方"一个踩过的坑" |
| 实例名 / mangling | 一律用**使用点实参**而非特化自己的形参 | `Box<double*,double>` → `Box_doubleP_double`，否则不同实参撞名 |
| 默认实参补全位置 | `getOrInstantiateClass` 中，**选择特化之前** | 特化匹配需要完整实参表 |
| 模板体 `struct` | `parseTemplateDecl` 分派加判 `KwStruct` | 与 class 只差默认访问级别，模板机制无区别 |

### 一个踩过的坑：全特化的替换表本来就是空的

第一版把 `instantiate()` 的路径判别写成「替换表为空 ⇒ 走主模板」，结果全特化直接崩：

```
template<> struct Box<int*, int>;   // 形参 0 个 ⇒ 推导出的替换表 = {} （空！）
Box<int*, int> d;
→ [ERROR] template 'Box' expects 0 argument(s), got 2
```

全特化**没有未知量可绑定**，替换表本就该是空的。用「空」当判别条件，
就把它误判成了主模板，进而撞上主模板的个数校验（0 形参 vs 2 实参）。

修法：路径归属是**调用方的知识**（`selectClassTemplate` 已经判定过），
必须显式传入，不能从数据里猜：

```cpp
ClassDeclPtr instantiate(TemplateDeclPtr decl,
                         const std::vector<TemplateArg>& args,
                         const TypeSubstitution* substOverride = nullptr);
//                       ↑ nullptr = 主模板路径；非 nullptr = 特化路径（表可为空）
```

这是本文件里最值得记住的一条工程经验：**不要用"数据为空"表示"走另一条路"**。

## ③ 完整数据流

```
源码:  template<typename T, typename U = void> class Box { int tag(){return 0;} };
       template<typename T> class Box<T*, T>    { int tag(){return 1;} };
       template<> class Box<int*, int>          { int tag(){return 2;} };
       Box<int> a;  Box<int*,void> b;  Box<double*,double> c;  Box<int*,int> d;

┌─ Phase 2 Parser ────────────────────────────────────────────────────┐
│ 形参表: TemplateParam{T, Type}、TemplateParam{U, Type, default=void} │
│ 类名后 '>' 检查:                                                     │
│   Box        → specPattern 空   ⇒ Primary                            │
│   Box<T*, T> → specPattern 非空 + 形参非空 ⇒ PartialSpec             │
│   Box<int*,int> → specPattern 非空 + 形参【空】⇒ ExplicitSpec        │
│ 默认实参空洞检查: 有默认值之后每位都必须有默认值                      │
└──────────────────┬───────────────────────────────────────────────────┘
                   ▼
┌─ Phase 3 Sema: processTemplateDecl 按 specKind 归档 ────────────────┐
│   m_classTemplates["Box"]     = 主模板                               │
│   m_partialSpecs ["Box"]      = [ Box<T*,T> ]                        │
│   m_explicitSpecs["Box"]      = [ Box<int*,int> ]                    │
└──────────────────┬───────────────────────────────────────────────────┘
                   ▼
┌─ Phase 3 Sema: 使用点 Box<...>（resolveType 触发）──────────────────┐
│  ⓪ checkTemplateArguments（个数 ≤ 形参数 + 每位形态）               │
│  ① 默认实参补全:  Box<int> → [int, void]                             │
│  ② selectClassTemplate 择优:                                         │
│       Box<int,void>       ─① 不中 ─② T* 配 int  ✗ ─③ 主模板        │
│       Box<int*,void>      ─① 不中 ─② T 冲突(int/void) ✗ ─③ 主模板  │
│       Box<double*,double> ─① 不中 ─② T := double ✓ ──→ 偏特化      │
│       Box<int*,int>       ─① int*==int* ∧ int==int ✓ ──→ 全特化     │
└──────────────────┬───────────────────────────────────────────────────┘
                   ▼
┌─ Phase 4 instantiate(选中版本, 完整实参表, substOverride) ──────────┐
│   主模板: substOverride=nullptr → 按 templateParams 逐位分派         │
│   偏特化: substOverride={T→double} → 直接采用                        │
│   全特化: substOverride={} （空表）→ 逐字克隆特化体                  │
│   实例名/mangling 一律用【使用点实参】                               │
└──────────────────┬───────────────────────────────────────────────────┘
                   ▼
   Box_int_void         _Z3BoxIivE            tag() → 0
   Box_intP_void        _Z3BoxIPivE           tag() → 0
   Box_doubleP_double   _Z3BoxIPddE           tag() → 1
   Box_intP_int         _Z3BoxIPiiE           tag() → 2
                                          Σ = 3 ✓
```

### 实例名对照表（注意实例名取使用点实参，不是特化自己的形参）

```
使用点                    走哪条    实例名                mangling
Box<int>                  ③ 主模板  Box_int_void          _Z3BoxIivE
Box<int*, void>           ③ 主模板  Box_intP_void        _Z3BoxIPivE
Box<double*, double>      ② 偏特化  Box_doubleP_double   _Z3BoxIPddE
Box<int*, int>            ① 全特化  Box_intP_int         _Z3BoxIPiiE
                                    └────┬────┘
                              编码的是使用点实参 (double*, double)
                              而非偏特化的形参 T ⇒ 是 IPddE 不是 IPdE
```

## ④ clang 对照

| 本实现 | clang 源码 | 简化了什么 |
|---|---|---|
| `TemplateSpecKind` + `specPattern` | `ClassTemplateDecl` / `ClassTemplatePartialSpecializationDecl` / `ClassTemplateSpecializationDecl` | 三种 AST 节点 → 同结构 + 判别枚举 |
| `m_partialSpecs` / `m_explicitSpecs` | `ClassTemplateDecl::getPartialSpecializations()` / `lookupSpecialization()` | vector 线性扫描（教学代码里特化数量个位数） |
| `selectClassTemplate` | `Sema::CheckClassTemplatePartialSpecializationArgs` + `Sema::InstantiateClassTemplateSpecialization` | **不做 [temp.class.order] 偏序裁决**：多偏特化同时匹配取先注册者 |
| `matchPattern` → `deducePair` | `DeduceTemplateArguments`（与函数模板同一族入口） | 只支持类型模式；NTTP 模式（`Box<int, N>`）未实现 |
| 默认实参补全 | `Sema::CheckTemplateDefaultArgs` | 无包展开、无 `template-template` 默认值 |
| 空洞检查 | `err_template_param_default_arg_missing` | 位置在 Parser 而非 Sema |

## ⑤ 实验

```bash
# ① struct 写模板（本会话修复的既有缺口）
./minicc tests/tmpl/test_tmpl_27_struct_template.cpp
./tests/tmpl/test_tmpl_27_struct_template; echo $?     # 7

# ② 默认实参：Box<int> 补全为 Box<int, void>
./minicc tests/tmpl/test_tmpl_28_default_arg.cpp
#   关注: ★ default template argument: 'U' = void
#         [sema:targ] ⤷ default argument filled: 'U' := void
#         ║ Instance: Box_int_void → _Z3BoxIivE
./tests/tmpl/test_tmpl_28_default_arg; echo $?         # 3

# ③ 偏特化：用实参推导模式
./minicc tests/tmpl/test_tmpl_29_partial_spec.cpp
#   关注: [spec:select] ② partial specialization 'Box<T*, T>' matched by deduction → USING IT
./tests/tmpl/test_tmpl_29_partial_spec; echo $?        # 1

# ④ 全特化：逐位类型相等
./minicc tests/tmpl/test_tmpl_30_explicit_spec.cpp
#   关注: [spec:select] ① explicit specialization matched (exact type equality) → USING IT
#         [subst:map] (specialization) ... (none — fully concrete body, verbatim clone)
./tests/tmpl/test_tmpl_30_explicit_spec; echo $?       # 2

# ⑤ ★ 三路对照：四个使用点走完三条路径
./minicc tests/tmpl/test_tmpl_31_spec_selection.cpp
#   四次 [spec:select] 各给出结论 + 失败原因
./tests/tmpl/test_tmpl_31_spec_selection; echo $?      # 3

# ⑥ 错误用例：默认实参空洞（声明期检查，与实参表无关）
./minicc tests/tmpl/test_tmpl_32_error_default_gap.cpp
#   template parameter 'U' must have a default argument because
#   'T' (declared before it) has one

# ⑦ 语义 oracle：与 clang 逐字符核对 mangling
clang++-18 -std=c++20 -c tests/tmpl/test_tmpl_31_spec_selection.cpp -o /tmp/t31.o
nm /tmp/t31.o | grep -o '_ZN3BoxI.*E3tagEv'
#   _ZN3BoxIivE3tagEv        ← 主模板 Box<int,void>
#   _ZN3BoxIPivE3tagEv       ← 主模板 Box<int*,void>
#   _ZN3BoxIPddE3tagEv       ← 偏特化 Box<double*,double>
#   _ZN3BoxIPiiE3tagEv       ← 全特化 Box<int*,int>
clang++-18 -std=c++20 tests/tmpl/test_tmpl_31_spec_selection.cpp -o /tmp/t31 && /tmp/t31; echo $?
#   → 3      （与本实现一致）

# ⑧ 单元测试（4 个偏特化匹配用例，进 ctest）
ctest --test-dir build-linux -R PartialSpec --output-on-failure
```

## ⑥ 已知边界

| 边界 | 现状 | 归属 |
|---|---|---|
| `[temp.class.order]` 偏序裁决 | ✅ **已补**，见 **learn/21** | learn/21 |
| NTTP 特化模式 | `Box<int, N>` 形式的偏特化不支持（Parser 直接报错） | 后续 |
| 全特化的成员在类外定义 | 只支持特化体写在声明里 | 后续 |
| 特化必须先于使用点声明 | 依赖「先注册后使用」的顺序，无延迟匹配 | 后续 |
| 函数模板特化 | 未实现（本次只做类模板） | 后续 |
| `decltype` / 运算符重载 / 范围 for | 用户原代码的管道部分仍不通 | 见 learn/18 与 ROADMAP |

### 偏序裁决（2026-09 补记：已完成，详见 learn/21）

> 本节写于 learn/19 成文时。当时的结论"本次先不做"已被推翻 ——
> 偏序已在主线 H 落地，本节保留作为**问题陈述**，实现说明见 learn/21。

当时的分析：

多个偏特化同时匹配时，标准要求用 [temp.class.order] 的"互相推导"判定谁更特化：

```cpp
template<class T> struct S<T*>;    // A
template<class T> struct S<T**>;   // B
S<int**>  →  A、B 都匹配
   用 A 的形参推 B：T* vs T** → 成功
   用 B 的形参推 A：T** vs T* → 失败
   ⇒ B 更特化，选 B
```

这套算法与已有的 S6 函数模板偏序（`[temp.func.order]`）是同一类问题，
复用现成引擎即可，但需要额外处理"同为偏特化"这一层的候选集构造与
歧义诊断。

**后续进展（主线 H，learn/21）**：以上全部已实现 ——
`classSpecAtLeastAsSpecialized` 复用 `TemplateDeducer::matchPattern`
做互相推导，唯一合成类型用 `"$ord_<模板名>_<形参名>"` 前缀保证，
`selectClassTemplate` 用 dominance 循环从 N 个同时匹配的偏特化里选出
唯一最特化者，**互不更特化时报错**（而非静默取先注册者）。
测试 `tests/tmpl/test_tmpl_40..43`，其中 40/41 是一对判别性实验，
用于证伪"取第一个/最后一个匹配者"这类偷懒实现。

## ⑦ 与用户原代码的距离

用户原始 test case 里的 `Box` 部分现在**已经可以编译运行**：

```cpp
Box<decltype(&a)> b;   // ← decltype 已在 learn/20 实现
b.hello();             //   Box<int*, void> ⇒ 主模板
```

完整链路见 `tests/tmpl/test_tmpl_39_is_range_demo.cpp`。

其余部分（`operator|` / `operator||` / 范围 for / `std::` 库）
的距离见 learn/20 ⑦ 与 ROADMAP。

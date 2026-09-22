# 34 · 成员模板：同一套合一算法，多两件"出身"的事

> 主题：`struct Acc { template <class T> T add(T x) { return base + x; } };`
> 对应标准：[temp.mem]（成员模板）、[temp.deduct]（推导规则**完全一致**）。
> 对应编译原理：**绑定在哪个环境里求解**。合一算法本身与自由函数模板一字不差，
> 差别全在"解得之后怎么落地"——有没有隐式 this、符号怎么起名。

---

## 1. 理论背景

### 1.1 一句话：推导复用，落地分叉

[temp.mem]/1 明确写着成员模板的模板实参推导与普通函数模板规则相同。
本实现因此**直接复用 `TemplateDeducer`**，没写第二套合一算法：

```text
   template <class T> T twice(T x);          // 自由函数模板
   struct S { template <class T> T id(T x); }; // 成员模板

   两者对实参 int 的推导逐字相同：
     [deduction]   P=T            A=int          ⇒ T := int
```

分叉发生在推导**之后**：

| | 自由函数模板 | 成员模板 |
|---|---|---|
| 实例的 ownerClassName | `""` | 类名（⇒ 有隐式 this） |
| 符号 | Itanium 模板实例名 `_Z5twiceIiET_T_` | `类名_方法名_实参后缀` `Acc_add_int` |
| 调用点 | 改写 callee 的 VarExpr 名字 | 回填 `MemberExpr::resolvedCalleeSymbol` |

### 1.2 为什么符号必须是 `类名_方法名_实参后缀`

本项目的普通成员调用在 CodeGen 里是**硬拼**的：

```cpp
funcName = objType->name + "_" + mem->memberName;    // Acc_add
```

成员模板一旦有多个实例（`add(int)` / `add(int*)`），硬拼会让两者都调 `Acc_add` ——
而那个符号**根本没被发射过**（实例叫 `Acc_add_int`），链接期报 undefined reference。

反过来，若把实例符号起成 Itanium 模板名 `_Z3addIiET_S0_`，又和 CodeGen 的
成员调用约定（`Cls_method`）对不上。故取**两者的折中**：
前缀沿用成员函数的 `类名_方法名`，后缀用实参清洗串区分实例。

★ 这需要 CodeGen 知道"这次调用该用哪个符号"，于是 `MemberExpr` 新增
`resolvedCalleeSymbol` 字段，由 Sema 在推导+实例化后回填；为空时走原硬拼路径
（**既有符号逐字节不变**，logdiff 零差异即证）。

### 1.3 名字存在 ≠ 能选出函数

成员调用在 Sema 里会经过两个入口：

```text
   s.id(5)
     │
     ├─ inferCall：接手 Member callee，先补推导 callee 表达式
     │     └─ inferType(mem) ⇒ inferMember
     │           ★ 此刻【手上没有实参】，推不出 T，选不出实例
     │
     └─ inferCall 继续：带实参查方法表 → 查成员模板表 ⇒ 推导 + 实例化
```

若 `inferMember` 直接报 `No member 'id'`，整条成员模板调用路径会在**推导之前**
被掐死（实测就是这个症状）。故 `inferMember` 对成员模板只回答"名字确实存在"，
返回一个 void 占位，把选择权留给带实参的调用点。

限定条件是 `expr.isMethodCall`：`auto x = s.id;`（取成员模板本身）仍然报错 ——
clang 里那是未决名字，同样不合法。

### 1.4 查找顺序：非模板优先

成员模板的查找排在**普通方法表之后**：

```text
   [over.match.best]：可行候选里，非模板函数优先于模板特化
```

本项目的调用点查找简化为"先到先得 + 精确匹配"，故把成员模板放在后面查询，
效果与标准的"非模板优先"一致。

---

## 2. 数据流（ASCII 图）

```text
   Parser（类体循环）
     check(KwTemplate)
       │
       ├─ 吃掉形参表（TemplateParamFrame 立作用域，使体内裸 T 认得出是形参）
       ├─ parseMethodDecl ⇒ 普通 FunctionDecl（isVirtual / isStatic 照样可带）
       └─ 装成 TemplateDecl{funcTemplate=…, isMemberTemplate=true}
            挂进 ClassDecl::memberTemplates
                     │
                     ▼
   Sema::processClassDecl
     ↳ member template 'Acc::add' registered
       存进 m_classMemberTemplates[类名]
       （★ 不进 methods：它不是可调用实体，是张"配方"）
                     │
   调用点  a.add(5)
                     ▼
   ┌──────────────────────────────────────────────┐
   │ inferCall：Member callee                     │
   │   ① inferType(mem) ⇒ inferMember             │
   │        名字命中成员模板 ⇒ 返回 void 占位      │
   │   ② 方法表按名 + 个数查 ⇒ 未命中              │
   │   ③ 成员模板表查 ⇒ 命中 'add'                │
   └──────────────┬───────────────────────────────┘
                  ▼
   ┌──────────────────────────────────────────────┐
   │ getOrInstantiateMemberFunction               │
   │   推导（同一套 TemplateDeducer）⇒ T := int   │
   │   缓存键 = sanitizeSymbolChars("Acc_add")+_int│
   │   实例化（ownerClassName="Acc"）              │
   │   落地解析（返回类型/形参 resolveType）        │
   │   注册进 m_functionMap / m_functions          │
   └──────────────┬───────────────────────────────┘
                  ▼
   mem->resolvedCalleeSymbol = "Acc_add_int"   （CodeGen 采用）
```

---

## 3. 实现改动清单

| # | 文件:位置 | 改动 |
|---|---|---|
| 1 | `include/ast.h` · `ClassDecl` | 新增 `memberTemplates`（`vector<TemplateDeclPtr>`） |
| 2 | `include/ast.h` · `TemplateDecl` | 新增 `isMemberTemplate` 标志 |
| 3 | `include/ast.h` · `MemberExpr` | 新增 `resolvedCalleeSymbol`（Sema 回填，CodeGen 采用） |
| 4 | `src/parser.cpp` · 类体循环 | 新增 `KwTemplate` 分支：形参表 + `parseMethodDecl` ⇒ 成员模板 |
| 5 | `src/parser.cpp` · 类体循环 | `isStatic`/`isVirtual` 的**声明提到分支之前**（见 §3.1） |
| 6 | `src/semantic_analyzer.cpp` · `processClassDecl` | 注册成员模板表 |
| 7 | `src/semantic_analyzer.cpp` · `inferMember` | 成员模板 ⇒ 返回 void 占位而非报 No member |
| 8 | `src/semantic_analyzer.cpp` · `inferCall` | 方法表未命中后查成员模板表 ⇒ 推导 + 实例化 + 回填符号 |
| 9 | `src/semantic_analyzer.cpp` · 新增 `getOrInstantiateMemberFunction` | 与自由函数模板同构的实例化入口 |
| 10 | `src/template_instantiation.cpp` · `instantiateFunction` | 加 `ownerClassName` 参数；成员路径换符号规则 |
| 11 | `src/template_instantiation.cpp` · `sanitizeSymbolChars` | 从 `instantiate()` 内联代码中**抽出为公共函数** |

### 3.1 声明提到分支之前：又一次"位置即语义"

`isStatic` / `isVirtual` 原本紧接在成员分支之前声明。插入成员模板分支时若放在
它们**之前**，成员模板分支里就引用不到这两个变量（编译错误，看得见）。

真正值得注意的是**反过来的情形**：若把成员模板分支放在能编译的位置，
但语义上"template 在外层、static/virtual 在内层"的关系被写反，就会静默改变
`template <class T> static T f(T)` 这类写法的解释。故注释里写明了顺序理由。

### 3.2 清洗函数抽成公共的：拒绝"两处一份规则"

`sanitizeSymbolChars` 原本内联在类模板的 `instantiate()` 里。成员模板实例化需要
**同一份**规则来拼缓存键与符号名——两处各写一份，改一处必漏另一处，
表现为"缓存命中失败（重复实例化）"或更糟的"误命中（调错函数）"，**都不报错**。

故抽成 `namespace minicc` 下的自由函数，头文件声明，两处共用。
这与 `emitFunction` 的 `hasThis` 被复制成两份那次是同一类教训。

---

## 4. 可复现实验

```bash
# ① 端到端（期望两个编译器都返回 0）
./minicc tests/tmpl/test_tmpl_56_member_templates.cpp -o /tmp/t56 && /tmp/t56; echo $?
clang++-18 -std=c++20 tests/tmpl/test_tmpl_56_member_templates.cpp -o /tmp/t56c && /tmp/t56c; echo $?

# ② 看两个不同实例的符号（★ 不能都叫 Acc_add）
./minicc tests/tmpl/test_tmpl_56_member_templates.cpp -S -o /tmp/t56.s 2>&1 \
  | grep -E "Symbol: (add|identity)"
# ⇒ ║ Symbol: add → Acc_add_int
#    ║ Symbol: identity → Acc_identity_intP      ← T := int* 是另一个实例

# ③ 最小复现：成员模板访问成员变量（隐式 this 传递）
cat > /tmp/mt2.cpp <<'EOF'
struct Acc {
    int base;
    Acc(int b) : base(b) {}
    template <class T> T add(T x) { return base + x; }
};
int main() { Acc a(10); int r1 = a.add(5); Acc b(100); int r2 = b.add(7); return r1 + r2 - 122; }
EOF
./minicc /tmp/mt2.cpp -o /tmp/mt2 && /tmp/mt2; echo $?   # 期望 0

# ④ 全量回归
./build.sh && ctest --test-dir build-linux -j6 && ./logdiff.sh diff
```

---

## 5. 边界（本项目**未做**的）

| 缺口 | 症状 | 卡在哪 |
|---|---|---|
| 成员模板显式实参 | `a.id<int>(5)` | Parser 的 `s.id` 后接 `<` 未走 template-id 试探（成员访问路径） |
| 类外定义成员模板 | `template <class T> T S::id(T x) {…}` | 类外成员定义整体未实现 |
| 成员模板偏特化 | `template <class T> T S::id<T*>(T*)` | 函数模板偏特化本身未实现 |
| 变参成员模板 / 参数包 | `template <class... Ts>` | 参数包未实现 |
| 模板模板参数的成员模板 | 上述两者叠加 | docs/learn/33 §5 已记 |
| 静态成员模板的 this 省略 | `static` + 成员模板叠加 | 未验证 |

---

## 6. clang 源码对照表

| clang（`~/cppproject/llvm-project/`） | 本实现位置 | 简化了什么 |
|---|---|---|
| `MemberTemplateDecl` / `CXXMethodDecl` + `TemplateParameterList` | `TemplateDecl{funcTemplate, isMemberTemplate}` 挂在 `ClassDecl::memberTemplates` | 不建独立的 Decl 层级，复用外层 TemplateDecl |
| `Sema::CheckMemberTemplateDeclaration` | `processClassDecl` 的注册循环 | 不做签名合法性检查 |
| `Sema::AddTemplateOverloadCandidate`（成员模板走同一条重载候选路径） | `inferCall` 的成员模板分支 | 不做隐式转换序列排序，只做"名字 + 推导成功" |
| `DeduceTemplateArguments`（对成员/自由**同一份**） | `TemplateDeducer::deduce`（**直接复用**） | 同为教学精简版 |
| `Sema::BuildCXXMemberCallExpr` → `CXXMethodDecl*` | `MemberExpr::resolvedCalleeSymbol` | 存符号名而非 Decl 指针（CodeGen 只需要符号） |
| `ItaniumMangle::mangleFunctionEncoding`（成员模板实例仍编成 `_ZN…`） | `sanitizeSymbolChars(类名_方法名) + 实参后缀` | 不走 Itanium —— 保持与普通成员调用约定一致 |

---

## 7. 关键日志片段（`test_tmpl_56` 实跑）

定义与注册：

```text
  [parse:member] ★ 'add' 是成员模板（1 个模板形参，[temp.mem]）—— 调用点按实参推导实例化
    ↳ member template 'Acc::add' registered (1 template parameter(s))
```

两个入口各司其职（`inferMember` 只回答"名字在"，`inferCall` 才做推导）：

```text
    [member] Acc.add —— 成员模板（待实参推导，[temp.mem]）
  [call] Acc::add — 成员模板候选，开始推导（[temp.mem]）
  [call] Acc::add → Acc_add_int (member template instantiated) → int
  [call] Acc.add(1 args) → int    [member template instantiated]
```

两个不同实例的符号（同一个 `identity`，T 不同 ⇒ 符号必须不同）：

```text
  ║ Symbol: identity → Acc_identity_intP
  [call] Acc::identity → Acc_identity_intP (member template instantiated) → int*
```

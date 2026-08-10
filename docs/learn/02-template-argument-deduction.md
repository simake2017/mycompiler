# 02 模板实参推导（S2，核心）

> 子阶段 S2：对每个 (参数模式 P, 实参类型 A) 配对做受限合一，
> 绑定模板参数；同一参数多次绑定必须一致。这是 C++ 模板机制的理论核心。

## ① 理论背景

**标准依据**：[temp.deduct.call]（函数调用实参推导）。

- **推导 = 受限合一**：与 Hindley-Milner 类型推断的合一算法同源，但受限在——
  只有"参数模式"一侧可以含变量（T），实参一侧是具体类型；
  没有通用方程求解，也没有 let-多态。
- **P/A 配对规则**（本实现的五条，对应标准逐款）：
  | P 形态 | 处理 |
  |---|---|
  | `const X` | 剥 P 顶层 const，继续 |
  | `T&` | 实参必须左值（否则拒绝），对 P 内层继续 |
  | `T&&` | 万能引用：左值实参 ⇒ T := A&（折叠为 A&）；右值 ⇒ 对内层继续 |
  | `T*` | 实参必须指针，对 pointee 递归 |
  | `T`（裸参数） | 值传递调整：剥 A 顶层引用/const 后绑定 |
  | 非依赖类型 | 要求 P == A 恒等 |
- **一致性 = 替换合成**：`mix2(1, true)` 第一次 T := int，第二次 T := bool →
  合一失败。这就是替换必须满足的"幂等且一致"约束。
- **左值性是推导输入**：所以 sema 在 `inferCall` 里为每个实参记录
  `isLValue`（VarExpr/MemberExpr 为左值），而不只是类型。

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 独立模块 `TemplateDeducer` | `deduce()` 返回 `DeductionResult{success, deducedArgs, trace}` | 推导与实例化解耦；trace 逐对记录供教学日志 |
| T 的表示 | 同时认 `TemplateParam("T")` 和 `Class("T")` | parseType 把裸 T 解析成 Class 类型（S1 的简化），推导层吸收这个差异 |
| 左值性来源 | sema 侧按表达式种类判定（变量/成员访问 = 左值） | 没有独立的值类别系统（无 xvalue/prvalue 细分），够用且可讲清 |
| 失败即否决候选 | 任一对失败 → 该候选不可行，继续试下一个 | 为 S6 重载决议铺路：否决不等于报错 |
| 恒等判定 `Type::equals` | 不做隐式转换（int≠double） | 转换排序属于 S6 范畴，此处保持推导纯粹 |

## ③ clang 对照表

| 本实现 | clang 源 | 简化了什么 |
|---|---|---|
| `TemplateDeducer::deduce` | `SemaTemplateDeduction.cpp → DeduceTemplateArguments`（3 个重载：from call / from type / for partial ordering） | 只实现 from call；无 partial ordering 专用入口（S6 复用普通推导） |
| `deducePair` 五分支 | `TemplateDeductionCallback::Deduce`（P/A 递归核心，约 400 行） | 无 non-type 模板参数、模板模板参数、函数指针参数、初始化列表实参 |
| 万能引用折叠 | 同文件 `DeduceAutoType` 与 [temp.deduct.call]/4.2 实现 | 无 auto&&、无转发 `std::forward` |
| 值传递调整（剥顶层 cv/ref） | 同文件开头对 A 的调整（"If P is not a reference..."） | 一致 |
| 一致性检查 `bind` | `DeducedTemplateArgument` 合并时的冲突诊断 | clang 区分"转换后冲突"与"多次推导冲突"两类诊断 |

## ④ 实验手册

```bash
cd /home/magene/runtime/cppproject/mycompiler

# 基础推导（逐对日志 + 实例化 + 执行）
./build-linux/minicc tests/test_tmpl_14_deduce_basic.cpp -o /tmp/t14.s
gcc /tmp/t14.s -o /tmp/t14 && /tmp/t14; echo $?     # 0

# 引用/const/万能引用形态
./build-linux/minicc tests/test_tmpl_15_deduce_forms.cpp -o /tmp/t15.s
gcc /tmp/t15.s -o /tmp/t15 && /tmp/t15; echo $?     # 0

# 冲突绑定（错误用例）
./build-linux/minicc tests/test_tmpl_16_conflict_error.cpp; echo $?   # 1，见 conflicting

# oracle：clang 的推导诊断与产物
clang++-18 tests/test_tmpl_16_conflict_error.cpp -fsyntax-only        # 对照冲突报错措辞
clang++-18 -emit-llvm -S tests/test_tmpl_14_deduce_basic.cpp -o - | grep define   # 对照实例化符号
```

## ⑤ 关键过程图

```
调用 twice(21)
    │  sema.inferCall: 实参类型 [int]，左值性 [false]
    ▼
deduce(tmpl=twice, A=[int])
    │
    ├─ 参数对 0:  P = T          A = int
    │             P 是裸模板参数 → 值传递调整（A 无顶层 ref/const，不变）
    │             bind(T, int)   →  subst = {T := int}   ✓
    │
    ├─ 收尾检查：typeParams [T] 全部已绑定            ✓（否则 S4 报错）
    ▼
deducedArgs = [int]
    │
    ▼  （S5）instantiateFunction(twice, [int])
subst {T→int} 克隆蓝图 → twice<int>，符号 _Z5twiceIiE
    │
    ▼  callee 重写：VarExpr "twice" → "_Z5twiceIiE"
codegen: callq _Z5twiceIiE   ←→   _Z5twiceIiE: 标签   ✓ 链接闭环
```

# 04 不可推导上下文（S4）

> 子阶段 S4：模板参数只出现在"推不动"的位置时，诊断并提示显式指定。

## ① 理论背景

**标准依据**：[temp.deduct.type] 列出的 non-deduced contexts，常见：

- 只出现在**返回类型**（`template<typename T> T gen();` 调 `gen()`）
- 出现在 `sizeof(T)`、`decltype` 等非推导表达式中
- 非类型模板参数表达式里的 T、默认实参里的 T

本实现的判定是**收尾检查**：走完全部 P/A 配对后，凡未被绑定的模板参数
一律视为不可推导（上述位置都不会产生绑定，自然落到这里）。比逐语法位置
枚举简单，且对教学足够精确。

## ② 设计决策

- 检查点在 `deduce()` 末尾，错误信息直接给出修复建议：
  `specify it explicitly, e.g. gen<int>()` —— 把 S3 与 S4 串成闭环。
- 不区分"哪种不可推导上下文"（clang 有专门诊断类别）——简化。

## ③ clang 对照

| 本实现 | clang | 简化 |
|---|---|---|
| 收尾未绑定检查 | `SemaTemplateDeduction.cpp` 收尾 + `TemplateDeductionCallback` 在非推导位置主动跳过绑定并记录 `NonDeduced` | 无细粒度诊断分类 |
| 错误文案建议显式实参 | clang 诊断 `note: couldn't infer template argument 'T'` | 一致思路 |

## ④ 实验

```bash
./build-linux/minicc tests/test_tmpl_18_nondeduced_error.cpp; echo $?   # 1
# 期望：non-deduced context: template parameter 'T' cannot be deduced ...
# 修复演示：把 gen() 改成 gen<int>() 即可通过（S3 路径）
```

## ⑤ 过程图

```
gen()  →  参数对数量 = 0（没有 P/A 配对可做）
       →  收尾检查：T 未绑定
       →  ✗ non-deduced context 报错（指向返回类型位置）
```

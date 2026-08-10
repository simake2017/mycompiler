# 06 重载决议（S6）

> 子阶段 S6：同名候选（普通函数 + 函数模板们）→ 可行性过滤 → 排序选赢家。

## ① 理论背景

**标准依据**：[over.match.best]（最佳可行函数）、[temp.func.order]（偏序）。

完整重载决议三步：**候选收集 → 可行性 → 排序**。本项目实现最小可用子集：

1. **非模板优先**：普通函数可行（参数个数匹配）⇒ 直接胜出，
   不进入模板推导。这是 [over.match.best]/2 的首要规则。
2. **模板可行性 = 推导成功**：推导失败（S2~S4 任一原因）即否决该候选。
3. **偏序（最易错点）**：F1 比 F2 更特化 ⟺ 用 **F1 自己的参数类型当合成实参**
   去推导 **F2** 成功。方向记忆法：*"更特化者的领域能被更泛化者吸收"*——
   `po(T*)` 的领域（指针）代入 `po(T)` 推导成功（T := T*），
   故 `po(T)` 覆盖 `po(T*)` ⇒ `po(T*)` 更特化 ⇒ 选它。

   本项目省略了标准中的转换排序（Exact/Promotion/Conversion 三级）与
   tie-break 规则；隐式转换仅在非模板可行性处按"个数匹配"粗判。

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 候选视图 | 普通函数（m_functionMap）+ 模板候选集（m_functionTemplateCandidates）分开查 | 保持符号表"名字→单符号"的教学模型（S1 决策延续） |
| 偏序实现 | 复用 `TemplateDeducer`，合成实参 = 对方参数类型 | deduction-based partial ordering 本就是标准算法，零新机制 |
| 无偏序关系时 | 保留先声明者（不报 ambiguous） | 简化；日志如实标注"无特化关系" |

## ③ clang 对照

| 本实现 | clang | 简化 |
|---|---|---|
| 非模板优先分支 | `SemaOverload.cpp → AddOverloadCandidate` vs `AddTemplateOverloadCandidate` 的排序权重 | 无转换等级细排 |
| `isAtLeastAsSpecialized` | `SemaOverload.cpp → IsAtLeastAsSpecialized`（调 `DeduceTemplateArguments` 的 partial-ordering 重载） | 无函数参数转换的"去引用/去顶层 cv"标准化 |
| 可行性 = 推导成功 | 推导成功后还要检查隐式转换序列可行 | 本项目只查个数 |

## ④ 实验

```bash
./build-linux/minicc tests/test_tmpl_19_overload.cpp -o /tmp/t19.s 2>&1 |
    grep -E "non-template preferred|partial ordering|Instantiation"
gcc /tmp/t19.s -o /tmp/t19 && /tmp/t19; echo $?    # 0
# 若误选模板 twice<T>：twice(2)=4，退出码 ≠ 0 —— 用执行结果反证决议正确
```

## ⑤ 过程图

```
调用 twice(2)                        调用 po(nullptr)
    ▼                                    ▼
候选: 普通 twice(int)  可行(个数✓)   候选: po(T) 推导 T:=void*  ✓
    ⇒ 非模板优先，结束               候选: po(T*) 推导 T:=void ✓
                                         ▼
                                   偏序: 合成实参 [T*] 推导 po(T) → 成功
                                         ⇒ po(T*) 更特化 ⇒ 实例化 po(void*)
```

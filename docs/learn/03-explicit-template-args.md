# 03 显式模板实参与 template-id 解析（S3）

> 子阶段 S3：`mix<int>(1, 2)` —— 显式实参占模板参数表前缀，其余继续推导。

## ① 理论背景

- **前缀规则**（[temp.explicit]/[temp.deduct]）：显式实参从左到右填充模板参数表，
  只能省略尾部可推导的参数；`mix<int>` ⇒ T 给定、U 推导。
- **template-id 歧义**：`<` 在表达式里既是"小于"又是"模板实参表开头"。
  C++ 的解法要求名字已被声明为模板（语境消解）；本项目 Parser 无符号信息，
  用**结构判据**：`名字 '<' 类型列表 '>'` 之后紧跟 `'('` 才算 template-id，
  否则回滚交还比较表达式。这是 clang `ParseImplicitTemplateId` 的简化版。

## ② 设计决策

- 显式实参挂在 `VarExpr::explicitTemplateArgs`（template-id 出现在 `(` 之前，
  CallExpr 还不存在）；sema 在 `inferCall` 取用。
- 解析失败回滚用 `m_pos = saved` + try/catch（parseType 报错可恢复）——
  与类成员"方法 vs 字段"的前瞻同款机制，保持 Parser 单一风格。
- 推导引擎侧零成本：显式实参只是预填 subst 表的前缀。

## ③ clang 对照

| 本实现 | clang | 简化 |
|---|---|---|
| parsePrimaryExpr 的前瞻分支 | `ParseTemplate.cpp → ParseImplicitTemplateId` + Sema ActOnExplicitTemplateArgumentList | 无语境消解（不查名字是否模板）、不支持 `template` 消歧关键字 |
| 前缀填充 subst | `DeduceTemplateArguments` 开头对 ExplicitTemplateArguments 的循环 | 一致 |

## ④ 实验

```bash
./build-linux/minicc tests/test_tmpl_17_explicit_args.cpp -o /tmp/t17.s
gcc /tmp/t17.s -o /tmp/t17 && /tmp/t17; echo $?   # 0
# 日志关注：[parse] template-id: mix<...>、"显式给定 / 推导补全"分界
```

## ⑤ 过程图

```
mix<int>(1, 2)
  parse: VarExpr{ name=mix, explicitTemplateArgs=[int] }
         └─ 前瞻: '<' int '>' 后是 '(' → template-id ✓（不回滚）
  sema:  subst 预填 {T := int}（显式）
         P/A 对 b(U) vs 实参 int → U := int（推导）
         ⇒ <T=int, U=int>
```

# 05 函数模板实例化（S5）

> 子阶段 S5：推导结果 → 克隆蓝图 → 替换 T → mangled 符号 → 注册 codegen。

## ① 理论背景

- **隐式实例化**（[temp.inst]）：函数模板在**被调用（ODR-used）时**才实例化，
  与类模板的"使用点实例化"一致。推导出的实参就是实例化的输入。
- **两阶段名称查找的第二阶段**：蓝图存储期（S1）只登记不检查；
  实例化时函数体在**具体类型**下做完整的语义分析
  （本项目：对实例调用 `analyzeFunctionBody`）。
- **符号唯一性**：`twice<int>` 与 `twice<double>` 必须生成不同符号
  （_Z5twiceIiE / _Z5twiceIdE），调用点重写为该符号，与普通函数调用同构。
- **实例化缓存**：同一组实参只实例化一次（clang 用 SpecializationDecl 去重）。

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 复用类模板克隆引擎 | `TemplateInstantiator::cloneMethod(blueprint, subst, "")` | 替换规则（含引用折叠）与类方法完全一致，零重复代码 |
| 符号命名 | `NameMangler::mangleTemplateInstance(name, args)` → `_Z5twiceIiE` | 与类实例 mangling 同构；真实 ABI 还带尾部参数编码，教学版省略 |
| callee 重写 | sema 把 VarExpr 名字改成 mangled 符号 | codegen 的 `callq <name>` 无需改造，最小侵入 |
| 缓存键 | mangled 名 → 实例 | 天然唯一 |
| 实例体检查 | 实例化后立即 `analyzeFunctionBody(instance)` | 依赖名在此阶段才解析（两阶段查找落地） |

## ③ clang 对照

| 本实现 | clang | 简化 |
|---|---|---|
| instantiateFunction | `SemaTemplateInstantiate.cpp → TemplateInstantiator`（TreeTransform 子类，18k 行） | clang 在 AST 上"原地变换"而非深拷贝；本项目深拷贝更直观 |
| 实例注册/去重 | `FunctionTemplateSpecializationInfo` + ASTContext 去重 | 无 explicit instantiation 声明、无实例化深度限制诊断 |
| 调用点重写 | CodeGen 层直接引用 specialization decl | 本项目改 AST 名字，更"看得见" |

## ④ 实验

```bash
./build-linux/minicc tests/test_tmpl_14_deduce_basic.cpp -o /tmp/t14.s
# 日志关注：Function Template Instantiation (S5) 框、Substitution map、Symbol 行
grep -n "_Z5twiceIiE" /tmp/t14.s          # 定义与调用点都用 mangled 符号
gcc /tmp/t14.s -o /tmp/t14 && /tmp/t14; echo $?   # 0
```

## ⑤ 过程图

```
deducedArgs [int]
    ▼
instantiateFunction(twice, [int])
    subst = {T → int}
    cloneMethod: returnType T→int, param T x→int x, body x+x 类型随之具体化
    mangledName = _Z5twiceIiE
    ▼
sema: cache[mangled] = instance
      m_functions += instance        → codegen 发射函数体
      analyzeFunctionBody(instance)  → 两阶段查找第二阶段
    ▼
inferCall: calleeVar->name = "_Z5twiceIiE"   → callq 闭环
```

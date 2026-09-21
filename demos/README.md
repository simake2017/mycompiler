# demos —— 可运行的编译器示例

> 与 `tests/` 的分工：**tests 钉住行为**（带诊断码、是回归红线，改动前先看它），
> **demos 给人看**（短小，每个只演示一个理论点，配可观测的日志线索）。

## 怎么跑

```bash
./demos/run.sh              # 跑全部
./demos/run.sh tmpl         # 只跑某个子目录
./demos/run.sh -v tmpl/01   # 失败时显示完整编译日志
```

每个 demo 的约定：**编译成功且运行退出码 = 0**。
（非 0 表示该 demo 内部的断言失败，具体码值的含义写在 demo 源码里。）

单个 demo 也可以手工跑，顺便看编译日志：

```bash
./build-linux/minicc demos/tmpl/01_deduction.cpp -S -o /tmp/demo01.s
```

## 索引

| demo | 演示什么 | 理论点 | 看什么日志 |
|---|---|---|---|
| `core/01_control_flow` | if / while 降级成什么形状 | 控制流 → 基本块 + 回边 | 生成的 `.s` 里的 `while_begin_N` / `jmp` 回边 |
| `core/02_preprocessor` | 宏替换与条件编译 | 预处理是纯文本阶段 | `-E` 看展开结果；`[pp]` 日志看宏调用点 |
| `oop/01_vtable` | 同一调用点、运行期决定函数体 | [class.virtual] + Itanium vtable | `.s` 里的 `movq (%rax),%rax` 三部曲 |
| `oop/02_dynamic_cast` | 运行期沿继承链找目标类型 | [expr.dynamic.cast] | `callq __minicc_dynamic_cast` |
| `oop/03_multiple_inheritance` | 一个对象、多个基类子对象与指针调整 | Itanium ABI §2.4（thunk） | `.s` 里的 `thunk` 与 offset-to-top |
| `tmpl/01_deduction` | 从调用实参反推出 T | [temp.deduct.call] | `[deduction] P=T A=int ⇒ T := int` |
| `tmpl/02_reference_collapsing` | 同一个 `T&&` 折叠出三种类型 | [dcl.ref]/6 | `[subst] ★ Reference collapsing: int&&& → int&` |
| `tmpl/03_specialization` | 主模板 / 偏特化 / 全特化三路择优 | [temp.class.spec] | `[spec:select] ① ② ③` 三步裁决 |
| `tmpl/04_partial_order` | 两个函数模板都能匹配时选更特殊的 | [temp.func.order] | `[overload] partial ordering check...` |
| `tmpl/05_sfinae_void_t` | 用 void_t 探测成员是否存在 | [temp.deduct]/8 | `[sfinae] void_t<2 个实参> —— 逐个替换 + 求值探测` |
| `tmpl/06_nttp` | 把值当模板参数 | [temp.param]/4 | `[instantiate:name] 'Size<4>' → 'Size_4'` |
| `tmpl/07_alias_and_ctad` | 别名模板解糖 + CTAD 推导 | [temp.alias] / [dcl.type.class.deduct] | `[ctad] ▶ m MyPtr(int*)` |

每个 demo 的源码头部都写清了：**演示什么 / 理论点 / 看什么日志 / 预期退出码**。

## 自己加一个

1. 建文件 `demos/<分类>/NN_<名字>.cpp`
2. 头部注释写四件事：演示什么、理论点（带标准章节号）、看什么日志、预期退出码
3. 用它自己的退出码做断言：`if (实际 != 期望) return <诊断码>;`
4. 跑 `./demos/run.sh` —— 它会自动发现新文件

> 写 demo 时请**实际跑一遍**。minicc 仍有一些语法边界（如函数形参不能省名、
> `if` 条件里不能直接放模板 id），这些边界会随时补进这里的 demo 注释中。

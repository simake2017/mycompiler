# minicc 项目记忆

## 项目定位（最高优先级）
本项目目的是**学习编译器理论**，不是造生产编译器。重点方向：C++ 模板机制
（实参推导≈合一算法、实例化≈结构化替换、两阶段查找、重载决议）。
一切设计决策以"可讲解、可观测"优先于"高性能、功能全"。

## 项目快照（已盘点，勿重复摸底）
- 管线：Lexer → Parser → SemanticAnalyzer → TemplateInstantiation → CodeGen（x86-64 AT&T .s），约 5.5k 行 C++20，全阶段中文日志
- 已有：基础类型/auto/指针/左右值引用/const、类/public 继承/虚函数+vtable、new、类模板（仅显式实参）、if/while、字符串字面量、GCC 风格 mangling
- 缺口优先级：✅ P0 函数模板实参推导（S1~S6）✅ P0 预处理器（#include/#define/条件编译/pragma once）→ 下一项：**构造/析构** → 自动链接出可执行文件 → P2 for/break/continue、常量折叠、数组/enum/namespace
- 扩展点已定位：parseTemplateDecl 仅类模板分支（src/parser.cpp:264）；TemplateInstantiator 的 substituteType/clone* 引擎可复用于函数模板；符号表单符号需改候选集

## 交付三件套（每个子阶段必须全部产出，缺一不算完成）
1. **代码**：按固定顺序改动——README 语法表 → Ast/Parser → Sema → 新模块 → CodeGen → tests
2. **文档**：`docs/learn/NN-<主题>.md`，必含：理论背景（点名对应 C++ 标准章节如 [temp.deduct]、对应编译原理概念如合一算法）、设计决策、clang 源码对照表（文件:函数 → 本实现位置 → 简化了什么）、可复现实验命令、关键过程 ASCII 图
3. **测试**：`tests/test_tmpl_NN_*.cpp`（延续现有 01..10 编号），文件头注释写：预期行为、考察理论点；错误用例注明期望报错文案。回归红线：test_tmpl_01..10 永远全绿

## 开发规范
- 全阶段中文日志，关键算法要有 trace（如推导逐对打印 `P=T A=int ⇒ T := int ✓`）
- 语义 oracle：任何新特性先跑 `clang++-18 -emit-llvm -S` 确认预期，再写实现
- 参考源只移植算法、禁止整段拷贝 clang 代码；参照源码在 ~/cppproject/llvm-project/（即 /root/cppproject/llvm-project/）
- 工具链用本机 clang++-18；Makefile 的 macOS homebrew 路径是坏的要避开/修复

## 当前主线任务
✅ **函数模板实参推导 S1~S6 已完成**（解析 → 基础推导 → 显式实参 → 不可推导上下文 → 实例化 → 重载决议；
测试 tests/test_tmpl_11..19；文档 docs/learn/01..06）。
✅ **预处理器 P0 已完成**（#include 搜索路径 / #define 对象+函数宏 / 条件编译 / #pragma once / -E；
测试 tests/test_pp_01..04 + tests/pp/ 头文件夹具；文档 docs/learn/07）。
✅ **主线 A 构造/析构与顶层声明已完成**（构造函数/初始化列表、虚析构/delete、全局变量、枚举、命名空间、类型别名；
测试 tests/unit/test_decl_and_ctor.cpp + tests/test_ctor_01..02 + tests/test_decl_01；文档 docs/learn/08）。
✅ **主线 B 自研链接器已完成**（默认直出非 PIE 可执行文件：借系统 as 产 .o，自研链接器合并节/符号决议/重定位回填；
内置 _start+64KB bump malloc/free，不依赖系统 ld/crt/libc；-S 只吐汇编；
新文件 include/linker.h + src/linker.cpp；文档 docs/learn/15，编号顺延因 09..14 已被占用）。
构建目录 build-linux/（clang++-18）。
⏭ 后续计划见 **docs/ROADMAP.md**（主线 C 控制流 → D 常量折叠 → E 数组/高级类型 → F 深水区选做，
每条含理论点/clang 参照/任务分解/验收）。新会话接手：先读本文件与 ROADMAP，选定主线再开工。

**完整提示词与子阶段定义：docs/prompt-template-deduction.md**——新会话接手时先读它。

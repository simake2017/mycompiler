# minicc 项目记忆

## 项目定位（最高优先级）
本项目目的是**学习编译器理论**，不是造生产编译器。重点方向：C++ 模板机制
（实参推导≈合一算法、实例化≈结构化替换、两阶段查找、重载决议）。
一切设计决策以"可讲解、可观测"优先于"高性能、功能全"。

## 项目快照（已盘点，勿重复摸底）
- 管线：预处理器 → Lexer → Parser → SemanticAnalyzer → TemplateInstantiation → CodeGen（x86-64 AT&T .s）→ 自研链接器，约 18695 行 C++20，全阶段中文日志
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
✅ **非类型模板参数 NTTP 已完成**（`template<int N>` + `Buf<4>`，类型/值混排、负数实参、
形态校验；核心是 TemplateArg tagged 值取代裸 TypePtr，替换分两层（substituteType 管类型位置 /
cloneExpr 管表达式位置）；Itanium `L...E` 编码与 clang 逐字符核对；
测试 tests/tmpl/test_tmpl_21..26；文档 docs/learn/18）。
构建目录 build-linux/（clang++-18）。
✅ **类模板特化已完成**（`struct` 模板体、默认模板实参、偏特化 `Box<T*,T>`、全特化 `template<>`、三路择优；
测试 tests/tmpl/test_tmpl_27..32 + tests/unit/test_template_deduction.cpp 的 PartialSpec 套件；
文档 docs/learn/19 —— 顺延用 19 因为 18 已给 NTTP）。**未做**：NTTP 特化模式 `Box<int, N>`、类外定义特化成员。
✅ **主线 H decltype / SFINAE / 偏序裁决已完成**（
decltype 两套规则 [dcl.type.decltype] + 依赖上下文两段式求值；
SFINAE 软失败（SubstitutionFailure 独立异常类型）+ void_t 探测惯例 CWG 1558 + declval；
std 垫片（false_type/true_type/void_t/declval）走 registerBuiltins 内建注入，绕开别名模板与类内 static 的语法缺口；
[temp.class.order] 完整偏序：合成类型前缀 `$ord_` + dominance 循环 + 歧义报错；
顺带修了 matchPattern 的引用结构检查（此前 `Probe<int>` 会错误匹配 `Probe<T&>`）；
另有负向 test_tmpl_44（`template<>` 却没写 `<...>`）/ 45（全特化模式里用了未声明的名字）——
这两处此前都是**静默接受**（44 当主模板、45 永不匹配且零报错），修复点分别在
parseClassDecl 的 (templateParams 空 ∧ specPattern 空) 守卫、processTemplateDecl 的 ExplicitSpec 分支；
测试 tests/tmpl/test_tmpl_33..45 + tests/unit/test_decltype_sfinae.cpp（Decltype./Sfinae./PartialOrder./SfinaeProtocol. 四套件）；
SFINAE 已抽成独立模块 include/sfinae.h + src/sfinae.cpp（信号 SubstitutionFailure /
吸收器 Sfinae::attempt / 直接上下文 SfinaeContext / 统一日志出口，收口点全项目仅三处）；
文档 docs/learn/20、21）。
✅ **「名字在哪一层当真」一批修复已完成**（③语句层前瞻不跳 `&` → `S& r = a;` 不可解析；
⑤`isTypeKeyword()` 缺 `KwConst` → 语句层 `const int x` 不可解析；
⑦resolveType 符号表查不到即静默放行 → `Undeclared q;` rc=0 通过；
⑩实例名清洗把 `*`/`&` 都归一成 `_` → `Box<int*>` 与 `Box<int&>` 撞汇编符号；
清单一表见 docs/learn/22。⑦ 是 `a < b > c;`（[stmt.ambig]）能静默编译的原因，现已响亮报错）。
✅ **⑨ cv 限定符位置修正已完成**（`const int*`=Pointer(Const(Int))、`int* const`=Const(Pointer(Int))、
`const int&`=LValueRef(Const(Int))；此前 const 一律套在最外层 ⇒ 偏特化**静默选错**、`int* const` 直接解析失败。
顺带修了 `Type::toString()` 不是单射（`Const(Pointer(Int))` 与 `Pointer(Const(Int))` 都印成 `const int*`，
而实例缓存键吃它 ⇒ 串味）；
测试 tests/tmpl/test_tmpl_47_cv_position.cpp；文档 docs/learn/23）。
✅ **类内类型别名已完成**（`using X = T;` / `typedef T X;`，三个使用点＝三条查找路径：
类外限定名非模板 `Plain::Int`（符号表 `Cls::alias`）/ 类外限定名模板实例 `Box<int>::type`
（resolveType 的 nested-name 分支：限定者先实例化，再查实例类的 typeAliases）/
类体内非限定名 `type v;`（resolveType 的类作用域回退 m_currentClassName）；
实例化时别名目标随形参替换（复用 substituteType）。
**顺带修了一个与别名无关的 codegen bug**：`estimateFrameSize` 只数局部变量、漏算
「帧基 + 形参 spill 槽」⇒ 帧浅 8~56 字节，最深的局部落在 rsp 之下，被表达式求值的
`pushq` 暂存与 `callq` 返回地址踩掉（症状：变量单独读对、参与二元表达式就错，
`int a..h; return a+h;` 返回 2 而非 9）。
测试 tests/tmpl/test_tmpl_48_class_type_aliases.cpp + tests/unit/test_codegen_frame.cpp
（CodegenFrame.*，断言不变量「帧 N ≥ 最深 -X(%rbp)」）；文档 docs/learn/24）。
✅ **依赖类型名 `typename T::type` 已完成**（[temp.res]/5 悬案：定义期判不出 `T::x` 是类型还是值；
Parser 收 `typename` 前缀 + 首名是本模板形参且后跟 `::` 时建「限定者=TemplateParam」的嵌套节点；
Sema resolveType 遇依赖限定者⇒原样保留等替换；
TemplateInstantiator::substituteType 新增 Case 5.5 在【替换当场】查成员表解糖
—— 那里正是 [temp.deduct]/8 的直接上下文，查不到即 Sfinae::fail（软失败），
`void_t<typename T::type>` 探测惯用法由此成立；新增 MemberTypeResolver 回调接口
（与 DecltypeEvaluator 同构，避免 Sema ⇄ Instantiator 双向依赖）；
硬错误文案与 clang 逐字相同「no type named 'type' in 'WithoutType'」。
测试 tests/tmpl/test_tmpl_49_dependent_type_name.cpp；文档 docs/learn/25）。
✅ **ADL + 限定名查找已完成**（三条路：限定名 `N::S`/`N::get(s)`；命名空间内非限定名
（resolveType 新增命名空间作用域回退 m_currentNamespace）；ADL `get(s)`
（inferCall 重写为「合并候选 + 精确匹配裁决」——★ ADL 不是兜底而是补进同一候选集，
`measure(s)` 里 N::measure(S) 与全局 measure(int) 同场竞争，先到先得会静默调错函数）。
顺带修了三处基础设施问题：①Pass 2/3 扁平遍历漏掉命名空间（症状：Sema 全对但
CodeGen 没收到方法 ⇒ undefined reference 'N__S_N__S'）＋其镜像 bug（Pass 1 顺手注册
导致符号重复定义）；②codegen 符号拼装点未全部过 asmSymbol（命名空间类的 ctor/dtor、
vtable 条目在汇编期 junk）；③候选池必须只收「普通自由函数」，否则函数模板实例
（name 保留模板名）会被精确匹配抢走 ⇒ test_tmpl_17 链接失败。
测试 tests/decl/test_decl_02_adl_and_qualified_lookup.cpp；文档 docs/learn/26）。
✅ **别名模板已完成**（`template<class T> using Vec = MyPtr<T>;`，[temp.alias]/1：
别名**不是新类型** ⇒ 没有"实例化"只有"解糖"，不产生新类、不发新符号。
三处解糖点，缺一不可：① 使用点（Sema::resolveType 的模板 id 分支，排在类模板分支**之前**
——两者语法同形，只能靠注册表区分）；② 替换期（substituteType 新增 Case 5.8，
依赖上下文里在【直接上下文】当场解，失败即 Sfinae::fail）；③ ★推导期
（TemplateDeducer::desugarAlias —— 调用点 A 侧早已解糖成 `MyPtr_int`，
P 侧不解就**名字不等 ⇒ 候选被静默剔除**，这是最容易漏的一环）。
配套新增 AliasTemplateResolver 回调接口（与 DecltypeEvaluator / MemberTypeResolver 同构，第三次出现同一分层形状）。
**顺带补齐两块此前一直缺的拼图**（没有它们，别名一放进函数模板形参就废）：
(a) 类模板 id 的结构合一（[temp.deduct.type]/8 的 `T<T1...>` 情形，deducePair 此前落到
"P 非依赖 ⇒ 恒等"兜底 ⇒ `MyPtr<T>` vs `MyPtr_int` 必失败）；
(b) 实例"出身"记录（Type::templateOriginName/templateOriginArgs）—— minicc 的实例类型是
**改名后的 Type**（`MyPtr_int`，可读串非单射），模板 id 的信息在改名那刻就丢了，
clang 不存在此问题（其实例仍是带实参表的 Decl）。
另修两个静默错误：`containsTemplateParam` 守卫（此前 `Box<T>` 会拿形参 T 当实参真的去实例化，
造出假实例 `Box_T`，不报错只是全线算错）；NTTP 名在实参位被 Parser 误建为 Class("N")
⇒ 替换阶段按"表里绑的是值"还原成值实参（`template<int N> using BufA = Buf<N>;` 由此可用）。
测试 tests/tmpl/test_tmpl_50_alias_templates.cpp；文档 docs/learn/27）。
✅ **CTAD + 推导指引已完成**（`MyPtr m(7);` 由构造实参反推类模板形参，[dcl.type.class.deduct]：
与函数模板推导是**同一套合一算法的反向使用** —— 模式来自构造函数形参表，实现上直接复用
`TemplateDeducer::deducePair`（也因此把它提到 public）。CTAD **只在直接初始化触发**，
故顺带补上 `Type name(args);` 文法（此前 Parser 直接报 "Expected ';'"）。
推导指引 [temp.deduct.guide]：`template<class T> Box(T) -> Box<T>;`（模板形态）与
`Two(int) -> Two<int,int>;`（非模板形态，用来补构造函数**根本推不出**的形参）；
**显式指引优先于构造函数**（指引存在的意义就是改写默认规则）；前瞻靠"配对右括号后是否跟 `->`"，
与函数声明区分（`MyPtr(T)` vs `MyPtr f(T)` 前两个 Token 完全相同）。
★ 顺带修一个真问题：**替换 ≠ 实例化** —— 实例化函数后签名里残留的半成品
`MyPtr<int>` 必须再过一次 resolveType 才成 `MyPtr_int`（否则函数体里 `p.value`
报 "No member 'value' in class 'MyPtr'"；别名形参位因解糖内部已调 resolveType 而侥幸不暴露）。
另：构造函数符号由 Sema 选定并回填 `VarDeclStmt::ctorSymbol` —— mangling 是有状态的
（同名多参追加 `_N` 后缀），CodeGen 按 `Name_Name` 硬拼会拼出不存在的符号。
测试 tests/tmpl/test_tmpl_51_ctad_and_guides.cpp；文档 docs/learn/28）。
✅ **AST 分派重构已完成（五批次，全项目零 RTTI）**（Visitor 模式 + 标签分派：
把 4 条 `if-else + dynamic_pointer_cast` 分派链收进类型系统。
`include/ast_visitor.h` 手写 clang `RecursiveASTVisitor` 的对应物 —— 32 个 `visit`
重载 + `ASTNode::accept` 纯虚挂钩；`AstDumper`（main.cpp 的 `--dump-ast`）、
`CodeGen`、`Sema` 语句链已迁到 visit 重载上；声明链 / `inferType` / `cloneExpr` /
`estimateBlockSize` 用 `NodeKind` switch。**全项目 dynamic_pointer_cast 147 → 0**）。
★ **判据（本项目唯一权威表述在 include/semantic_analyzer.h 的注释里）**：
**handler 只要引用 → 访问者（accept 虚分派）；还要 shared_ptr 所有权或返回值 → 标签分派（switch）**。
clang 同此分法：`RecursiveASTVisitor` 只服务遍历，类型计算走 `dyn_cast`/switch。
零 RTTI 的向下转换 = `node->kind == NodeKind::X` + `static_pointer_cast`，
与 LLVM `cast<>` 用 `classof()` 查 `getKind()` 同理。
★ 另两条关键经验：①`accept` 必须**类内 inline 定义**，否则成为 Itanium ABI 的
**键函数**、vtable 只在定义它的 TU 发射 ⇒ 链接期满屏 `undefined reference to vtable`；
②批次 3 曾因给**声明链**硬套访问者而编译失败（注册表存 `shared_ptr`，`visit` 只给引用）
—— 这次回退直接催生了上面那条判据。**先看 handler 形状，再选分派手法。**
★ 配套安全网 `logdiff.sh`（`save` 存基线 / `diff` 逐字节比对）：本项目**日志即契约**
——单测靠子串匹配日志原文（obs_helpers.h: explainPipelineLine）、docs/learn 直接粘贴日志片段，
故重构的铁律是**日志与生成的汇编逐字节不变**。基线含 84 个集成测试的
编译日志+退出码+程序输出+汇编全文。每批三绿（build.sh 0 警告 / ctest 全绿 / logdiff 零差异）才进下一批。
新增测试 tests/unit/test_ast_visitor.cpp（`AstVisitorDispatch.*` 5 例：精确重载路由、
默认空体、递归由调用方驱动、★ 标签不变式 kind≡实际类型（零 RTTI 转换的安全前提，
用 RTTI 作 oracle 钉住）、忘记下钻则静默漏访问）—— 全量 154 → 159。
顺带把散落的 69 处 `// wangyang` 个人阅读笔记提炼进 `docs/NOTES-阅读笔记.md`
（源码只留正式注释）；并提取了 Pass 2/Pass 3 重复的命名空间递归走查为 `forEachFunctionDecl`。
★ 该轮漏了 5 处，已于 B19 那轮补齐（`parser.cpp:2158`、`semantic_analyzer.cpp:731/744/4218`
共 4 处，另 1 处随 B19 的代码改动一并消失）—— 补遗与"★★ 兑现点"两条实测要点记在
`docs/NOTES-阅读笔记.md` §六；**源码里现已无 `// wangyang`**。
文档 docs/REFACTOR-ast-visitor.md（重构全过程）+ docs/learn/29-ast-dispatch-two-idioms.md（判据与 clang 对照）。
✅ **语言基础补齐 + 模板三项收尾已完成**（一/二/三梯队，本轮）：
① `struct X : Base` 默认 **public** 继承（[class.derived]/2；此前按 private 拒收，是**拒收合法程序**的 bug，B9）；
② 一元 `*p` 解引用（[expr.unary.op]/1，结果按 pointee 宽度分派读/写）+ 后置 `const` 成员函数（[dcl.fct]/7）
   + 类内 `static` 成员函数（[class.static]/2，无 this ⇒ 参数寄存器从 rdi 起）；
③ **NTTP 值位参与偏特化模式**（`enable_if<true,T>` 的地基 —— `specPattern` 由 `vector<TypePtr>` 改型为
   `vector<TemplateArg>`，拆掉"实参含值位就跳过特化"的旧守卫；`std::enable_if_t` 由此端到端可用）；
④ **模板模板参数**（[temp.param]/4，`Wrap<Box,int>`，二级替换：C→Box 再落地解析）；
⑤ **成员模板**（[temp.mem]，`Acc::add(T)`，推导复用同一套合一算法，符号改用 `类名_方法名_实参后缀`）。
测试 tests/lang/test_basics_01 + tests/tmpl/test_tmpl_52/53/54/56；文档 docs/learn/31..34。
顺带修 PITFALLS **I4**（蓝图摘要把 NTTP 打成 `typename N`）与 **F2 残留**（Phase 4 演示只看第二位形参，
`template<bool B, class T>` 被喂 `<int,double>` 触发形态自检）。全量 159 → **224** 单测 / 94 集成测试。
✅ **带参成员方法符号回填（bug 修复，docs/BUGS.md B10）** ——
**症状**：`class C { public: int f(int x) { return x; } };` 的 `c.f(1)` 一律
`undefined reference to 'C_f'`；下标糖 `v[i]` 同样中招（调用点拼 `IntVec_at`，
定义点其实是 `IntVec_at_1`）。错误停在链接期，编译期全程绿灯。
**根因**：同一条语义判断写在两处且不一致 —— 定义点（Sema `registerFunction`）给
**带参**成员方法名追加"参数个数"后缀，调用点（CodeGen）硬拼 `类名_方法名`。
**修法**：让普通成员方法复用成员模板早就用上的回填机制
（`MemberExpr::resolvedCalleeSymbol`），并给 `IndexExpr` 增 `atSymbol`/`setSymbol` 两槽；
CodeGen 优先用回填值、空则退回硬拼。**虚调用不受影响**（CodeGen 先查 vtable 即 return）
—— 实证：修复后的基线差异**恰好只有 `test_stl_02..05` 四个文件**，其余 90 个逐字节不变。
测试 tests/stl/test_stl_02..05（四个文件头即回归说明）+ 新增单测
tests/unit/test_symbol_consistency.cpp（`SymbolConsistency.*` 5 例，断言【不变量】
"每个 callq 目标都有 .globl 定义"，而非具体符号名 —— 命名规则随便改都不会误报）。
★ 顺带发现【另案缺口】（**已修**，见下批 B12）：继承来的成员方法调用
（`Derived d; d.f(3)`，f 在 Base）报 `No member 'f' in class 'Derived'`
—— 成员查找不走基类链（继承的字段访问是通的）。
**同批顺带修好**：`tests/pp/test_pp_01_include.cpp` 的 `#include` 路径写错
（`"pp/math_helper.h"` ⇒ 搜到 `tests/pp/pp/` 下，必然找不到），使 `#include` /
`#pragma once` 这个 P0 特性**从来没有被真正测到**（rc=1 被基线固化成"契约"）；
现已真跑通。另：`tests/mi/test_mi_03` 原文用未实现的三元 `?:` ⇒ 停在**词法期**，
已改写为 if，暴露出真缺口「跨转型 `B*`→`A*` 未实现」并写进文件头。
**教训**：logdiff 基线会把**失败**也固化成契约 —— 修完必须回头看 rc，不能只看"零差异"。

✅ **多继承「成员住在哪个子对象里」一批修复已完成（BUGS.md B11~B15）** ——
起点是一个问题："多继承下基类字段要不要改名"。
①**B11 vptr 压字段**（**真 miscompile**：本类自身有虚函数、基类全非多态时，
`[relocate]` 把首基类当 primary 摆到 0，而本类自己的 `_vptr` 也要占 0
⇒ 写基类字段即写坏虚表指针，随后虚调用跳飞 SIGSEGV）。Itanium 的 primary **只在动态基类里选**，
一个都没有时 vptr 自己占 0、基类从 8 起 —— 旧判据只写了两态，补上第三态。
②**B12 继承方法查不到**（`D d; d.g()` 报 `No member 'g'`）：成员查找收口成
`findMethodInClass` / `findMethodInHierarchy` 两个原语，`inferMember` 与 `inferCall` 共用同一谓词
（承 B10 教训：同一判据不许写两份）。
③**B13/B14/B15 同根**：**把显示名当成了索引**。`FieldInfo` 补
`declaredName`（权威裸名，查找的键）/ `viaBase`（装着它的**直接**基类子对象）/
`baseFieldIndex`（在基类布局里的下标）；`computeClassLayout` 的字段放置合并成
`子对象偏移(viaBase) + 基类布局[下标].offset`（主基类偏移恒 0 ⇒ 自动退化，与旧实现同值）；
`findField` 改三级（全限定名 / **自身字段优先**（[class.member.lookup]/3 隐藏）/ 继承），
新增 `findFields` + 歧义诊断（[class.member.lookup]/8：`Member 'x' is ambiguous ...`）。
修掉的两条**真 miscompile**：B14 `d.x` 曾静默指向 `A::x`（隐藏方向做反）、B11 段错误。
测试 tests/mi/test_mi_08..11 + tests/unit/test_layout_lookup.cpp（`LayoutLookup.*` 4 例，
断言的是**不变量**「多态类任何字段不得落在 `[0,8)`」「`d.x` 命中的那条必须 `viaBase` 为空」等）；
**五条修复逐条做了突变负向验证**（单测级 + 集成级双跑）。
单测 229 → **233**，集成 94 → **98**，logdiff 重刷基线（既有 94 个**零漂移**）。
索引与根因复盘见 docs/BUGS.md 的 B11~B15 与文末「小结 字符串兼任 ID 与路径」。

✅ **模板模板实参的逐位签名匹配已完成**（[temp.arg.template]/2，P0522R0 口径）——
**缺口**：模板模板形参的内层表此前**只数个数**（`size_t templateArity`，且该字段**从没被读过**）
⇒ `template<template<class,class> class C>` 收一个只有 1 位的 `Box` 一路放行，
直到替换出假类型 `Box<int,int>` 才在下游报"Box 至多 1 个实参"（诊断指向派生类型，不是根因）。
**改法**：内层表逐位建 `TemplateParam` 节点存进 `TemplateParam::innerParams`
（kind / NTTP 类型 / 默认值全带上），`checkTemplateArguments` 的模板位分支新增
`ttpSignatureMismatch` 三条判据：①位数相同 ②逐位同 kind ③值位声明类型相同。
★ **口径全部以 clang++-18 -std=c++20 探针实测确定**（docs/learn/33 §3.4 六条探针）——
第一版我按旧规则写"实参多出的位有默认实参即可"，clang 当场报 `too many`（P0522R0 已废除该放宽）。
**顺带修一个同族的静默接受**：`Wrap<Box<int>, int>`（拿**模板特化类型**填模板位）——
Parser 把 `Box` 与 `Box<int>` 都建成 Class 节点、**名字段都是 "Box"**，于是"查名字在不在
注册表"的形态守卫被骗过去，`<int>` 静默蒸发、这一位按裸 `Box` 用；更糟的是产物与
`Wrap<Box,int>` **逐字节相同**（替换期 `C<int>` → `Box<int>` 后落地成同一个 `Box_int`），
**连"算错"这个症状都没有**。修法：守卫补 `!arg.type->templateArgs.empty()`
（带实参的 id 是类型，不是模板名）。测试 tests/tmpl/test_tmpl_60 + 单测
`TtpSignature.TemplateIdArgumentIsNotATemplateName`（突变验证：删掉该条件即变红）。
**顺带验证**：别名模板作模板模板实参（形参表取别名自身、替换期正常解糖。
测试 tests/tmpl/test_tmpl_57（正例：类模板 / 别名模板 / 形参顺序交换）+
test_tmpl_58（too few）/ 59（kind 不符）+ 新增单测 tests/unit/test_ttp_signature.cpp
（`TtpSignature.*` 5 例：四条判据分支 + 正例不变量，**四条分支逐条做了突变负向验证**）；
文档 docs/learn/33 §3.3/§3.4/§5/§6/§7。**有意日志漂移**：模板位的绑定标签
`(non-type)` → `(template)` + 新增 `✓ 签名匹配` 行（仅 test_tmpl_53），已重刷基线。

✅ **NTTP 类型域扩展已完成**（形参从「只认 `int`」扩到 整型家族 + 字符 + `auto`）——
三段各自独立的知识点，此前全缺：
① **形参写法**（[dcl.type.simple] 的 type-specifier-seq）：`unsigned long` / `long unsigned` /
   `short` / `char` / `long long int` ⇒ `Parser::parseBuiltinTypeSpecifierSeq`
   **先收集六组计数器、再一次归一**（对照 clang `DeclSpec` 的四组位掩码），
   非法组合（`long long long`、`signed unsigned`、`char int`）各自报错；
   新 TypeKind 10 个 + `isInteger/isChar/isUnsignedInteger/integerBitWidth` 谓词，
   `sizeInBytes` 改为**从位宽推导**（不再两处各写一份）。
② **实参字面量**（[lex.icon]/2）：进制前缀 `0x`/`0b`/前导 `0` + 后缀 `u/U/l/L` 任意组合 +
   `'` 数字分隔符；**Lexer 只切片、`Parser::parseIntLiteral` 才解释成 `(value, type)`**
   （对照 clang 的 `Lexer` vs `NumericLiteralParser` 分工）。该函数**刻意不打日志**
   —— 它被每条整数表达式调用，打点会把日志淹掉（实测全量漂移数百行）；
   观测点改在 `[parse:targ] … (形态 X)` 与 `[infer] IntLiteral(4) → long`。
   另加 `[lex.ccon]` 字符字面量（`'a'`/`'\n'`/`'ab'` 折叠成 24930）；
   ★ 词法已翻译转义 ⇒ 回吐日志必须 `escapeCharText` 重新转义，否则一行被裸换行截断。
③ **★ 判据归位（BUGS.md B17，拒收合法程序）**：旧版要求
   `a.valueType->equals(p.nonType)`（**形态精确相等**）⇒ `Flag<1>`(bool←int 1) /
   `A<4L>`(int←long 4) / `A<true>`(int←bool 提升) **三条 clang 认可的合法程序全被拒**，
   而 `F<2>`/`U<-1>`/`D<300>` 这三条真该拒的**旧版也拒** —— 负向全绿掩盖了它。
   正解是 [temp.arg.nontype]/1 → [expr.const]/10 → [dcl.init]/7 的**可表示性**，
   收口成 `Type::canRepresentValue`（判据**单点**，Sema 与 main.cpp 演示路径共用）；
   通过后新增 **③-c 形态归一**（`a.valueType = p.nonType`）——这不是修饰而是语义必需，
   归一后 `Buf<4L>` 与 `Buf<4>` 的可读串/缓存键/mangling 才一致（clang 亦认同一实例）。
④ **`template<auto V>`**（[temp.param]/6 的 deduced non-type parameter）：
   形态即类型 ⇒ `K<4>`(int) 与 `K<4L>`(long) 是**两个实例**。
   ★ 顺带修掉一个**静默撞键**：实例名/缓存键此前用 `TemplateArg::toString()`
   （对两者都产 `"4"`）⇒ 第二个**静默复用**第一个，不报错、不算错、连症状都没有。
   修法是键与实例名同源于 `losslessArgumentsKey`/`renderArgLossless`，
   **形态只在 `auto` 形参位写入**（既有测试零漂移），实例名 `K_4Cint`/`K_4Clong`
   —— `sanitizeSymbolChars` 同批补 `:` → `C`（第一版忘了，汇编期吐非法符号）。
   承 docs/learn/23 与 B13~B15 的同一句教训：**拿给人看的字符串当机器用的键，早晚出事**。
⑤ **CodeGen 一行未改** —— `codegen.cpp` 全程按宽度（1/≤4/8 字节）分派、零 `TypeKind` 引用，
   新标量类型的代码生成成本为 0。
mangling 12/12 与 clang **逐字符相同**（`b c a h s t i j l m x y`，含
`char`≠`signed char`≠`unsigned char` 三元区分）；`TnDa`（`<template-param-decl>`）
**有意不写** —— 它只出现在**函数**模板的 `auto` NTTP 上，而函数模板的显式 NTTP 实参
目前直接报 `not supported yet`，没有能到达该编码的路径。★ 但**未做**「字面量溢出自动升格」
（[lex.icon]/2 规定十进制 `3000000000` 类型是 `long`）、`\x`/`\u` 转义。
测试 tests/tmpl/test_tmpl_61..68（正例 61/62/63/64/68 各返回 7，负例 65/66/67 rc=1
且文案与 clang **逐字相同**）+ 单测 tests/unit/test_nttp_type_domain.cpp
（`NttpTypeDomain.*` 10 例：四条判据分支 + 三条不变量，**四条分支逐条做了突变负向验证**）；
文档 docs/learn/35；BUGS.md 新增 **B17**；ROADMAP 主线 D 的「与 NTTP 的接口」段落已更新
（**下游接口全部就绪，主线 D 只剩"把字面量分支换成完整常量表达式分支"**）。
全量 239 → **249** 单测 / 110 集成测试；**有意日志漂移**只有
`[parse:targ] … (形态 X)` 一处（10 个既有文件），已重刷基线。

✅ **类模板里的成员模板（bug 修复，docs/BUGS.md B18）** ——
**症状**：`template<class T> struct Box { template<class U> U pick(U x); };` 的
`b.pick(7)` 报 `[Semantic Error] No member 'pick' in class 'Box_int'`（rc=1）。
解析层完全正常（`[parse:member] ★ 'pick' 是成员模板` 照印），断在语义层。
**根因**：这个写法有**两层模板形参、绑定时机不同** —— 外层 `T` 由类实例化绑定，
内层 `U` 由调用点推导绑定；而"类实例化"这一步只搬了字段/方法/类内别名，
**没搬 `memberTemplates`**；同时成员模板注册表挂在 `processClassDecl` 里
（semantic_analyzer.cpp:1822），而**类模板蓝图从不经过 processClassDecl**
（Pass 1 只打 `↳ class template 'Box' registered`）⇒ 蓝图那张表从没进过注册表
⇒ 调用点查 `m_classMemberTemplates["Box_int"]` 落空。
**修法**：`instantiateClassTemplate` 新增步骤 **5.6**：把本次**命中**的那份蓝图
（主模板/偏特化/全特化一视同仁）的成员模板 `cloneMethod` 一份挂到实例类，
**只替换外层形参**（`subst` 里只有 T ⇒ `substituteType` Case 1 对 U 走
"不在表中则原样保留"，那条注释里早就写着"如外层模板的形参"，这是它第一次真正派上用场）。
**三种错法三种症状**（本条的价值主要在判据表）：忘了搬 ⇒ 错误**拒收合法程序**；
搬了但**就地改蓝图** ⇒ 第二个实例串到第一个的绑定（**静默算错**）；
把内层 `U` 也替换掉 ⇒ 调用点无可推导。
测试 tests/tmpl/test_tmpl_69（含偏特化蓝图一例）+ 单测
tests/unit/test_member_template_in_class_template.cpp（`MemberTemplateInClassTemplate.*`
5 例，断言守恒/分层/隔离/符号一致四条不变量；**突变验证：停掉 5.6 的循环 ⇒ 5/5 全红**）；
文档 docs/learn/34 新增 §1.5 / §3.3（并补一句："边界表要按**组合**列，
只按特性列会漏掉交叉点上的缝" —— 这张表当时就漏了"类模板×成员模板"）。
全量 249 → **262** 单测（ctest 257 → 262）/ 111 集成测试；既有用例**逐字节零漂移**
（反证此前**没有任何用例覆盖这个组合**），已重刷基线。

✅ **static/virtual 的识别位置（bug 修复，docs/BUGS.md B19）** ——
**症状**：`template<class U> static U f(U x)`（标准写法）报
`[Parse Error] … at 'static': Expected type name`（rc=1）而 clang rc=0；
**反过来** `static template<class U> U f(U x)`（非法写法）却被解析放行。
**性质**：同一处判据**两个方向都反了** —— 拒收合法程序 + 接受非法程序。
**根因**：`static`/`virtual` 的识别写在类体循环里成员模板分支**之前**（1641 起，
原注释还专门交代了"位置必须在成员模板之前"），只认得到"说明符在 `template` 前"的顺序；
而 [temp.pre] 规定 template-head 必须在最前 ⇒ 标准写法在吃完形参表后**不再前瞻**、
直落 parseMethodDecl ⇒ `parseType` 撞死在 `static` 上。
★ 原注释的理由（"变量必须先声明"）说的是**变量声明顺序**，实现却把它与**语法位置**
绑在一起 —— "先声明"与"先出现"是两件事。
**修法**：吃完形参表、进 parseMethodDecl **之前**再认一次，两种说明符**处置相反**：
`static` 收下（[class.static]/2），`virtual` **当场报错**（[temp.mem]/2 末句：
member function templates shall not be virtual；文案与 clang 逐字相同）。
—— 本轮没有顺手把 virtual 也"支持"掉，是**判据**决定的，不是工作量决定的。
**同批加代码位置标注**：成员模板分支写明"**本项目只做成员函数模板**"——
成员类模板 / 成员别名模板 / 静态数据成员模板一律在此响亮拒收（`Expected type name`），
对照 clang 的做法（先解析完整声明再由结果反推 Decl 种类）与现状见 docs/learn/34 §3.5。
测试 tests/tmpl/test_tmpl_70（普通类 + 类模板交叉点 + 非 static 对照，rc=0）+
test_tmpl_71（rc=1，文案逐字同 clang，位置也对上）；**突变验证 3 条**（删 static 识别 /
删 virtual 报错 / 写回 isStatic）逐条实跑变红。既有 112 个集成用例**逐字节零漂移**；
基线 112 → 114。文档 docs/learn/34 新增 §3.4（处置表）与 §3.5（只做函数模板）。

✅ **vtable 槽里的符号名与定义点不同源（bug 修复，docs/BUGS.md B20）** ——
起点是"虚函数连续往下派生类透传、修饰名一直是原先最上层基类"这个问题，查下去是**两个**缺陷：
① **带参虚函数链接失败**：vtable 条目点（`processClassDecl`，**Pass 1**）自己硬拼
   `类名_方法名`，而定义点（`registerFunction`，**Pass 2**）对带形参的方法追加
   `_<形参个数>` ⇒ 汇编里 `_ZTV` 槽写 `.quad A_f`、定义却是 `.globl A_f_1`
   ⇒ `undefined reference to 'A_f'`，**编译期全绿**。无参虚函数两侧恰好一致
   （此前**没有任何用例写过带参虚函数**，所以一直是绿的）。
② **次基类未覆写的槽被重造出假符号**：收集次表条目时按 `次基类名 + "_" + 裸名`
   重造名字（原意"这条槽属于 Q，就该叫 Q_f"），但 Q 没覆写时槽里指的是**更上游**的
   `X_f` ⇒ 链接期 `undefined reference to 'Q_f'`。
**修法**：① **判据单点** —— 抽 `memberMethodSymbolName(owner, name, paramCount,
earlierSameNameCount)`（semantic_analyzer.cpp:1445），`registerFunction` 与 vtable
三个落点（主表覆写 / 次表覆写 / 新条目）**共用同一个函数**；② **次表名字原样透传**
（`VTableEntry secEntry = baseEntry;`），真覆写交给后面的方法循环（那时才改指本类实现 + 配 thunk）。
★ 计数口径有坑：`earlierSameNameCount` 只数**排在当前方法之前**的同名方法
（Pass 2 注册时扫 `m_functions` 也只看得见前者），数成"同类同名总数"会让
`virtual int f(); virtual int f(int);` 里 f() 得 `X_f_0`、定义点却是 `X_f` —— 又是个只在链接期炸的错配。
★ **现场就在 tests/mi/test_mi_04_error.cpp**：那个文件原先写菱形继承、把 rc=1 解释成
"菱形本该被拒收"，实测 clang **rc=0**（非虚继承的菱形合法，只有成员访问歧义才报错）——
真原因是 ②。修复后该文件改写为真正的错误用例（私有继承），菱形正例搬到
tests/mi/test_mi_12_secondary_inherited_slot.cpp（**又一次印证 logdiff 基线会把失败固化成契约**）。
测试：集成 tests/lang/test_basics_03_virtual_with_params.cpp + tests/mi/test_mi_12；
单测 tests/unit/test_vtable_symbols.cpp（`VTableSymbols.*` 4 例，断言**不变量**
"每条 `.quad <sym>` 都必须有 `.globl <sym>` 定义"，命名规则随便改都不会误报）；
**突变验证 2 条**（vtable 三落点改回硬拼 ⇒ 2 例红；次表改回重造 ⇒ 恰好 1 例红）。
既有集成用例漂移**恰好 1 个**（就是 mi_04 那个现场），其余逐字节不变；
单测 262 → **266** / 集成 114 → **116**。文档 docs/learn/13 新增 §13.4.3（单点判据与
"为什么必须收口"）、docs/learn/17 新增 §1.5 与 §2 Bug 4。

✅ **「身份 = 名字 + 形参类型」四处落点收口（B22 + B20 缺陷 c）** ——
**症状两条**：① 同名同个数的成员重载 `f(int)` / `f(S)` 在汇编期撞符号
``symbol `C_f_1' is already defined``（拒收合法程序，报错点还落在 as 上）；
② `virtual int f(); int f(int);` 里非虚的后者**认领**前者的 vtable 槽并被误标 virtual
⇒ `c.f()+c.f(2)-3` **运行返回 255**（静默算错，汇编里毫无异常）。
**根因同一个**：**"名字 + 参数个数"被当成了身份**。它写在这份实现的**四个地方**：
① 符号名 `memberMethodSymbolName`（后缀只编码个数）② vtable 槽位认领
（`processClassDecl`，只比裸名）③ 成员调用选定 `findMethodInClass`（循环内首个命中即返回）
④ **构造函数选定** `processVarDeclStmt`（只数个数，且把刚推出来的实参类型**扔掉**）。
**修法**：判据 `paramTypeChain`（形参类型链，每段过 `sanitizeSymbolChars`）+
**一个**共用择优原语 `pickBestByArgs`（精确 → 可隐式转换 → 退回首个）——
③④ 都走它。★ 符号名的类型链**只在"同类里有同名同个数的兄弟"时才挂**
（`countSameNameSameArity`）⇒ 既有 116 个集成用例**逐字节零漂移**。
★ **突变验证的教训**（值得单独记）：第一版单测只断言"槽里只有一个符号""派生类槽数不变"，
拆掉 `signature` 判据时**全绿** —— 认错槽位后"谁占槽、谁被直接调"会**整体互换**，
守恒式计数照样配平。有判别力的是**不对称**那条：**经基类指针的虚调用必须退化成
`callq *%rax` 间接调用**（认错 ⇒ CodeGen 按 `resolvedCalleeSymbol` 精确比对落空 ⇒
退化成直接调用 ⇒ 运行期不派发）。**守恒式不变量挡不住互换型缺陷。**
测试 tests/lang/test_basics_04 + tests/mi/test_mi_13 + 单测
tests/unit/test_member_identity.cpp（`MemberIdentity.*` 6 例，**三条突变逐条实跑变红**：
符号定名回退 4/6 红、槽位回退 2/6 红、构造函数按个数选 2/6 红）；
文档 docs/BUGS.md B22（含修复小节）、B20 缺陷 c 转 ✅、docs/learn/13 §13.4.3 规则表、
docs/learn/17 §2 Bug 5；单测 266 → **272** / 集成 116 → **118**。

⚠ **本轮新发现（docs/BUGS.md B24，未修）**：成员方法的**名字隐藏**
（[class.member.lookup]/3）—— `Derived::f()` 一声明，基类 `f(int)` 整族应被隐藏，
clang 拒收 `d.f(2)`，minicc 仍能查到（**接受非法程序**）。根因在
`findMethodInHierarchy` 按**裸名**判"本类有没有" ⇒ 与 B20c/B22 是同一个
"身份比裸名宽"的老毛病，只是方向相反（该藏没藏）。修法：隐藏先按**名字**砍掉基类整族
（与重载决议的按类型择优是**两个正交的筛选**）。是写 mi_13 时被 clang oracle 当场抓住的。

**未做（按优先级）**：④[stmt.ambig] 完整裁决 → ⑥后置 const 的重载区分与 const 正确性检查 →
⑥三元 `?:`（ROADMAP 主线 C）→ **`T[N]` 数组类型偏特化**（需新开 `TypeKind::Array`，
属 ROADMAP 主线 E 整条，不是顺手项）→ 类外成员定义 `int C::f() const {}`、函数默认实参、
函数形参里的 decltype 依赖表达式、`operator|`/`operator||` 那半边；
别名模板偏特化 —— 见 docs/learn/27 §5 边界表；
模板模板参数：形参包 `class...`（签名的偏序不可达）、模板位默认实参、嵌套模板模板参数
（depth > 1 直接报错）—— 见 docs/learn/33 §5；
NTTP：**任意常量表达式** `Buf<2+2>` / `Buf<k>`（主线 D）、字面量溢出自动升格、
`\x`/`\u` 转义、非整型 NTTP（指针/引用/枚举/字面量类）、`template<auto V>` 的**偏特化**模式
—— 见 docs/learn/35 §5。
⏭ 后续计划见 **docs/ROADMAP.md**（主线 C 控制流 → D 常量折叠 → E 数组/高级类型 → F 深水区选做，
每条含理论点/clang 参照/任务分解/验收）。新会话接手：先读本文件与 ROADMAP，选定主线再开工。

**完整提示词与子阶段定义：docs/prompt-template-deduction.md**——新会话接手时先读它。

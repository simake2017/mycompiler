# 多级继承支持规格（3+ 层级）

## 1. 现状分析

### 1.1 当前能力边界

当前系统已实现 **2 层多继承**（直接父子关系），通过以下测试验证：
- `test_mi_01_layout.cpp`：双基类字段布局
- `test_mi_02_dispatch.cpp`：主表/次表虚函数派发
- `test_mi_03_dynamic_cast.cpp`：upcast/downcast/crosscast
- `test_mi_04_error.cpp`：菱形继承拒绝

**限制**：所有测试仅涉及 2 层继承（D → A/B），未覆盖 3+ 层链式继承（GrandChild → Parent → GrandParent）。

### 1.2 3+ 层继承的核心挑战

当继承链超过 2 层时，以下问题暴露：

1. **Upcast 偏移累积**
   - 2 层：`Child* → Parent*`，偏移 = `bases[Parent].offset`
   - 3 层：`GrandChild* → GrandParent*`，偏移 = `bases[Parent].offset` + `Parent.bases[GrandParent].offset`
   - N 层：需递归或迭代计算整条链的偏移和

2. **Vtable 段计算**
   - 每层可能引入新的次表段
   - 覆写函数需正确定位到最派生类的 vtable 槽位
   - Thunk 调整量需跨层累积

3. **布局嵌套**
   - GrandChild 的布局 = Parent 的布局（含 GrandParent 子对象）+ GrandChild 自身字段
   - 字段偏移需相对于完整对象起始地址，而非当前子对象

4. **构造链传递**
   - `GrandChild()` 调 `Parent()`，`Parent()` 再调 `GrandParent()`
   - 每层 this 指针调整量不同：
     - GrandChild → Parent：`this + bases[Parent].offset`
     - Parent → GrandParent：`(this + bases[Parent].offset) + Parent.bases[GrandParent].offset`

---

## 2. 核心算法

### 2.1 递归 Upcast 偏移计算

**问题**：计算从 `derivedClass` 到 `targetBase` 的完整偏移。

**算法**：
```cpp
uint32_t computeUpcastOffset(const std::string& derivedClass, 
                             const std::string& targetBase,
                             const ClassLayout& layout) {
    // 基础情况：直接基类
    for (const auto& base : layout.bases) {
        if (base.baseClassName == targetBase) {
            return base.offset;
        }
    }
    
    // 递归情况：在基类中查找
    for (const auto& base : layout.bases) {
        auto baseTypeIt = m_classTypes->find(base.baseClassName);
        if (baseTypeIt == m_classTypes->end()) continue;
        
        uint32_t subOffset = computeUpcastOffset(
            base.baseClassName, 
            targetBase, 
            baseTypeIt->second->classLayout
        );
        
        if (subOffset != UINT32_MAX) {
            // 找到：当前层偏移 + 子层偏移
            return base.offset + subOffset;
        }
    }
    
    // 未找到
    return UINT32_MAX;
}
```

**复杂度**：O(N)，N = 继承链深度。

### 2.2 迭代 Upcast 偏移计算（推荐）

**优化**：避免递归开销，使用 BFS/DFS 遍历继承图。

**算法**：
```cpp
uint32_t CodeGen::getBaseOffset(const std::string& derivedClassName,
                                const std::string& baseClassName) const {
    if (derivedClassName == baseClassName) return 0;
    if (!m_classTypes) return 0;
    
    // BFS 队列：(当前类名, 累积偏移)
    std::queue<std::pair<std::string, uint32_t>> queue;
    queue.push({derivedClassName, 0});
    
    while (!queue.empty()) {
        auto [currentName, accOffset] = queue.front();
        queue.pop();
        
        auto typeIt = m_classTypes->find(currentName);
        if (typeIt == m_classTypes->end()) continue;
        
        for (const auto& base : typeIt->second->classLayout.bases) {
            uint32_t newOffset = accOffset + base.offset;
            
            if (base.baseClassName == baseClassName) {
                return newOffset;
            }
            
            queue.push({base.baseClassName, newOffset});
        }
    }
    
    return 0;  // 无继承关系或主基类
}
```

**优势**：
- 无递归栈开销
- 易于调试（可打印遍历路径）
- 支持菱形继承检测（同一类多次入队时报错）

---

## 3. Sema 层改造

### 3.1 computeClassLayout 递归调用

**现状**：当前 `computeClassLayout` 仅处理直接基类字段。

**改造**：
```cpp
void SemanticAnalyzer::computeClassLayout(ClassDeclPtr decl) {
    TypePtr classType = decl->classType;
    if (!classType) {
        classType = Type::makeClass(decl->name);
        decl->classType = classType;
    }
    
    uint32_t currentFieldOffset = 0;
    
    // 步骤 1：处理基类（递归布局）
    for (const auto& baseName : decl->baseClassNames) {
        auto baseDeclIt = m_classDecls->find(baseName);
        if (baseDeclIt == m_classDecls->end()) continue;
        
        // 递归：确保基类已布局
        if (baseDeclIt->second->classType->classLayout.fields.empty()) {
            computeClassLayout(baseDeclIt->second);
        }
        
        // 拷贝基类字段，调整偏移
        uint32_t baseOffset = /* 当前基类在派生类中的偏移 */;
        for (auto field : baseDeclIt->second->classType->classLayout.fields) {
            field.offset += baseOffset;
            classType->classLayout.fields.push_back(field);
        }
        
        currentFieldOffset = baseOffset + baseDeclIt->second->classType->classLayout.totalSize;
    }
    
    // 步骤 2：处理自身字段
    for (auto& field : decl->fields) {
        field.offset = currentFieldOffset;
        currentFieldOffset += field.size;
        classType->classLayout.fields.push_back(field);
    }
    
    classType->classLayout.totalSize = currentFieldOffset;
}
```

**关键点**：
- 基类字段**拷贝**而非引用（每层独立存储偏移）
- 偏移调整：`field.offset += baseOffset`
- 递归终止条件：`fields.empty()` 检查

### 3.2 Override 检测跨层传播

**问题**：GrandChild 覆写 GrandParent 的虚函数，需在 GrandChild 的 vtable 中更新槽位。

**算法**：
```cpp
void detectOverrides(ClassDeclPtr decl, ClassLayout& layout) {
    // 遍历所有方法
    for (auto& method : decl->methods) {
        // 在所有基类（含间接基类）中查找同名方法
        std::string overridePath = "";
        VTableEntry* target = findOverriddenMethod(method->name, layout, overridePath);
        
        if (target) {
            // 标记覆写
            method->isOverride = true;
            target->isOverridden = true;
            
            // 计算 thunk 调整量（跨层累积）
            target->thunkAdjust = computeUpcastOffset(
                decl->name, 
                extractClassFromPath(overridePath),
                layout
            );
        }
    }
}
```

**示例**：
```cpp
class A { virtual void foo(); };
class B : public A { /* 继承 A::foo */ };
class C : public B { void foo() override; };

// C 的 vtable：
//   slot 0: C_foo (覆写 A::foo)
//   thunkAdjust = offset(B in C) + offset(A in B)
```

### 3.3 RTTI 树深度遍历

**现状**：`emitRTTI` 仅处理直接基类。

**改造**：递归收集所有祖先类。

```cpp
void collectAllBases(const std::string& className, 
                     std::vector<std::pair<std::string, uint32_t>>& allBases) {
    auto typeIt = m_classTypes->find(className);
    if (typeIt == m_classTypes->end()) return;
    
    for (const auto& base : typeIt->second->classLayout.bases) {
        allBases.push_back({base.baseClassName, base.offset});
        
        // 递归收集
        collectAllBases(base.baseClassName, allBases);
    }
}

void CodeGen::emitRTTI(const std::string& className, TypePtr classType,
                       const std::string& /* baseClassName */) {
    std::vector<std::pair<std::string, uint32_t>> allBases;
    collectAllBases(className, allBases);
    
    // 发射 RTTI：[vptr, name, baseCount, base0_typeinfo, base0_offset, ...]
    emitData(std::format("    .quad 0                    # vptr (unused)"));
    emitData(std::format("    .quad {}                   # name", className));
    emitData(std::format("    .quad {}                   # base count", allBases.size()));
    
    for (const auto& [baseName, offset] : allBases) {
        emitData(std::format("    .quad _ZTI{}             # base typeinfo", baseName));
        emitData(std::format("    .quad {}                 # base offset", offset));
    }
}
```

---

## 4. Codegen 层改造

### 4.1 Upcast 指针调整（赋值时）

**位置**：`emitVarDecl` 的标量路径（line 926）。

**改造**：
```cpp
if (decl->initializer) {
    emitComment(std::format("var {} = ...", decl->name));
    emitExpr(decl->initializer);
    
    // 检查是否需要 upcast 调整
    auto initType = decl->initializer->inferredType;
    auto declType = decl->declaredType;
    
    if (initType && declType && 
        initType->isPointer() && declType->isPointer()) {
        
        std::string derivedName = initType->pointeeType->name;
        std::string baseName = declType->pointeeType->name;
        
        uint32_t adjust = getBaseOffset(derivedName, baseName);
        
        if (adjust > 0) {
            emitComment(std::format("upcast adjust: {} -> {} (+{} bytes)",
                derivedName, baseName, adjust));
            emit(std::format("addq ${}, %rax    # upcast adjustment", adjust));
        }
    }
    
    emit(std::format("movq %rax, {}(%rbp)    # store to {}",
        m_currentStackOffset, decl->name));
}
```

**示例**：
```cpp
GrandChild* gc = new GrandChild();
GrandParent* gp = gc;  // 需要 upcast 调整

// 生成的汇编：
//   callq GrandChild_GrandChild  # 构造
//   addq $16, %rax               # upcast: GrandChild -> Parent (+0) -> GrandParent (+16)
//   movq %rax, -8(%rbp)          # 存储到 gp
```

### 4.2 Vtable 段计算（多级）

**现状**：`vtableSegmentOffset` 仅考虑直接次表。

**改造**：递归累加所有祖先的 vtable 段大小。

```cpp
uint32_t computeVtableSegmentOffset(const std::string& className,
                                    const std::string& targetBase) {
    auto typeIt = m_classTypes->find(className);
    if (typeIt == m_classTypes->end()) return 0;
    
    uint32_t offset = 16;  // 主表头（8B offset-to-top + 8B RTTI）
    
    // 主表槽位
    offset += typeIt->second->classLayout.vtableEntries.size() * 8;
    
    // 遍历次表段
    for (const auto& base : typeIt->second->classLayout.bases) {
        if (base.isPrimary) continue;
        
        if (base.baseClassName == targetBase) {
            return offset;
        }
        
        // 递归：检查 targetBase 是否在 base 的子树中
        uint32_t subOffset = computeVtableSegmentOffset(base.baseClassName, targetBase);
        if (subOffset > 0) {
            return offset + subOffset;
        }
        
        // 跳过当前次表段
        offset += 16 + base.entries.size() * 8;
    }
    
    return 0;  // 未找到
}
```

### 4.3 Thunk 调整量累积

**问题**：次表覆写 GrandParent 的函数时，this 指针需调整到完整对象起始地址。

**算法**：
```cpp
int computeThunkAdjust(const std::string& derivedClass,
                       const std::string& overriddenBase,
                       const ClassLayout& layout) {
    // 计算 derivedClass 到 overriddenBase 的 upcast 偏移
    uint32_t offset = computeUpcastOffset(derivedClass, overriddenBase, layout);
    
    // Thunk 需将 this 从子对象偏移归零到完整对象起始
    return -static_cast<int>(offset);
}
```

**示例**：
```cpp
class A { virtual void foo(); };           // offset 0
class B : public A { };                    // offset 0 (主基类)
class C : public B { void foo() override; };

// C 的 vtable 中，A::foo 的槽位：
//   如果 C 是主基类 B 的派生类：
//     thunkAdjust = 0（无需调整）
//   如果 C 有次基类 B：
//     thunkAdjust = -offset(B in C)
```

### 4.4 构造链 this 指针调整

**位置**：`emitFunction` 的构造函数路径（line 588-660）。

**改造**：递归调用基类构造时，累积 this 偏移。

```cpp
for (auto& init : ctor->initList) {
    // 查找基类
    for (auto& base : m_currentClassType->classLayout.bases) {
        if (init.memberName == base.baseClassName) {
            // 计算完整 upcast 偏移（递归）
            uint32_t totalOffset = getBaseOffset(m_currentClassName, base.baseClassName);
            
            // 发射基类构造调用
            emit("movq -8(%rbp), %rdi    # this");
            if (totalOffset > 0) {
                emit(std::format("addq ${}, %rdi    # adjust to {}", 
                    totalOffset, base.baseClassName));
            }
            
            // 调用基类构造
            emit(std::format("callq {}_{}", base.baseClassName, base.baseClassName));
            break;
        }
    }
}
```

**示例**：
```cpp
class A { A() { /* this = A 子对象起始 */ } };
class B : public A { B() : A() { /* this = B 子对象起始 */ } };
class C : public B { C() : B() { /* this = C 完整对象起始 */ } };

// C::C() 生成的汇编：
//   movq -8(%rbp), %rdi     # this (C 完整对象)
//   addq $0, %rdi           # B 在 C 中偏移 0（主基类）
//   callq B_B               # B::B()
//
// B::B() 生成的汇编：
//   movq -8(%rbp), %rdi     # this (B 子对象 = C 完整对象)
//   addq $0, %rdi           # A 在 B 中偏移 0（主基类）
//   callq A_A               # A::A()
```

---

## 5. 测试用例设计

### 5.1 test_mi_05_three_level_layout.cpp

**目标**：验证 3 层链式继承的字段布局。

```cpp
class GrandParent {
public:
    int gp_field;
    virtual int getGP() { return gp_field; }
};

class Parent : public GrandParent {
public:
    int p_field;
    virtual int getParent() { return p_field; }
};

class Child : public Parent {
public:
    int c_field;
};

int main() {
    Child* c = new Child();
    c->gp_field = 10;
    c->p_field = 20;
    c->c_field = 30;
    
    // 验证字段偏移
    assert(c->gp_field == 10);
    assert(c->p_field == 20);
    assert(c->c_field == 30);
    
    return 0;
}
```

**预期布局**：
```
Child 对象：
  +0:  _vptr (共享主表)
  +8:  gp_field (来自 GrandParent)
  +12: p_field (来自 Parent)
  +16: c_field (Child 自身)
```

### 5.2 test_mi_06_three_level_upcast.cpp

**目标**：验证跨层 upcast 的指针调整。

```cpp
class GrandParent {
public:
    int gp_field;
    virtual int getGP() { return gp_field; }
};

class Parent : public GrandParent {
public:
    int p_field;
};

class Child : public Parent {
public:
    int c_field;
};

int main() {
    Child* c = new Child();
    c->gp_field = 100;
    
    // Upcast: Child* -> GrandParent*
    GrandParent* gp = c;
    
    // 验证指针调整正确
    assert(gp->gp_field == 100);
    
    return 0;
}
```

### 5.3 test_mi_07_three_level_override.cpp

**目标**：验证跨层虚函数覆写。

```cpp
class GrandParent {
public:
    virtual int who() { return 1; }
};

class Parent : public GrandParent {
    // 继承 GrandParent::who()
};

class Child : public Parent {
public:
    int who() override { return 3; }
};

int main() {
    GrandParent* gp = new Child();
    
    // 虚调用：应派发至 Child::who()
    assert(gp->who() == 3);
    
    return 0;
}
```

### 5.4 test_mi_08_four_level_diamond.cpp

**目标**：验证 4 层继承 + 菱形继承检测。

```cpp
class A { public: int a; };
class B : public A { };
class C : public A { };
class D : public B, public C { };  // 菱形继承，应报错

int main() {
    D* d = new D();
    return 0;
}
```

**预期**：编译器报错 "diamond/duplicate base 'A' detected"。

---

## 6. 文档更新

### 6.1 docs/learn/13-multiple-inheritance.md 追加章节

#### 6.1.1 多级继承（3+ 层）

**理论背景**：
- Itanium ABI §2.9.5：继承链的布局规则
- C++ 标准 [class.derived]：派生类对象的子对象排列

**关键概念**：
1. **偏移累积**：upcast 偏移 = 各层偏移之和
2. **Vtable 段嵌套**：每层次表段在主表中的偏移需递归计算
3. **Thunk 链**：覆写函数的 thunk 调整量 = 完整 upcast 偏移

**算法对比**：

| 方案 | 优点 | 缺点 |
|------|------|------|
| 递归 Upcast | 代码简洁 | 栈开销，调试困难 |
| 迭代 Upcast (BFS) | 无栈开销，易调试 | 代码稍长 |
| 缓存偏移表 | O(1) 查询 | 内存开销，缓存失效 |

**推荐**：迭代 BFS（见 2.2 节）。

**Clang 对照**：
- `CGRecordLayoutBuilder::ComputeNonVirtualBaseLayout`：处理多级基类布局
- `VTableBuilder::LayoutPrimaryAndSecondaryVTables`：递归构建 vtable 段

#### 6.1.2 ASCII 图示

```
GrandParent (GP)
    |
    | offset 0 (主基类)
    v
Parent (P)
    |
    | offset 0 (主基类)
    v
Child (C)

Child 完整对象：
+0:  _vptr (指向 Child 的主表)
+8:  gp_field (GP 子对象)
+12: p_field (P 子对象)
+16: c_field (C 自身)

Upcast: Child* -> GrandParent*
  调整量 = offset(P in C) + offset(GP in P)
         = 0 + 0
         = 0
```

---

## 7. 实施路线图

### Phase 1：核心算法实现（2-3 小时）
1. 实现 `CodeGen::getBaseOffset` 迭代版本
2. 改造 `emitVarDecl` 支持 upcast 调整
3. 更新 `emitFunction` 构造链 this 调整

**验证**：`test_mi_05_three_level_layout.cpp` 通过。

### Phase 2：Vtable 与 Thunk（3-4 小时）
1. 改造 `injectVTableAndRTTI` 递归计算 vtable 段偏移
2. 更新 `detectOverrides` 跨层查找
3. 修正 thunk 调整量累积

**验证**：`test_mi_07_three_level_override.cpp` 通过。

### Phase 3：RTTI 与 dynamic_cast（2-3 小时）
1. 改造 `emitRTTI` 收集所有祖先
2. 更新 `emitDynamicCastHelper` 深度遍历

**验证**：`test_mi_06_three_level_upcast.cpp` 通过。

### Phase 4：测试与文档（2 小时）
1. 添加 test_mi_05~08
2. 更新 docs/learn/13-multiple-inheritance.md
3. 回归测试（确保 test_mi_01~04 仍通过）

**总工时**：9-12 小时。

---

## 8. 风险与缓解

### 8.1 性能风险
- **问题**：递归布局计算导致 O(N²) 复杂度（N = 继承深度）
- **缓解**：缓存已布局的类（`m_classLayoutCache`）

### 8.2 兼容性风险
- **问题**：现有 2 层测试可能因布局算法变更而失败
- **缓解**：Phase 4 回归测试，失败时回滚并修正

### 8.3 菱形继承误报
- **问题**：3 层链式继承（A → B → C）可能被误判为菱形
- **缓解**：区分"同一基类多次出现"与"链式继承"
  - 菱形：`D : B, C`，且 B、C 共享祖先 A
  - 链式：`C : B`，`B : A`（A 仅出现一次）

---

## 9. 验收标准

1. **功能**：test_mi_05~08 全部通过
2. **回归**：test_mi_01~04 仍通过
3. **性能**：10 层继承链的编译时间 < 100ms
4. **文档**：docs/learn/13-multiple-inheritance.md 包含多级继承章节
5. **代码质量**：无递归爆栈风险，关键算法有注释

---

## 10. 附录：关键数据结构扩展

### 10.1 ClassLayout 新增字段

```cpp
struct ClassLayout {
    // 现有字段
    std::vector<FieldInfo> fields;
    std::vector<VTableEntry> vtableEntries;
    std::vector<BaseSubobject> bases;
    uint32_t totalSize;
    
    // 新增：完整继承链缓存（避免重复计算）
    std::unordered_map<std::string, uint32_t> upcastOffsets;  // baseName -> offset
};
```

### 10.2 BaseSubobject 新增字段

```cpp
struct BaseSubobject {
    std::string baseClassName;
    uint32_t offset;
    bool hasVTable;
    bool isPrimary;
    uint32_t vtableSegmentOffset;
    std::vector<VTableEntry> entries;
    
    // 新增：完整继承深度（用于调试和错误报告）
    uint32_t inheritanceDepth;  // 0 = 直接基类，1 = 祖父类，...
};
```

---

**文档版本**：1.0  
**最后更新**：2026-09-01  
**作者**：minicc 团队  
**审阅者**：待定

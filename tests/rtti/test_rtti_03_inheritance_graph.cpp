// =============================================================================
// 测试：编译期打印类型继承图（inheritance graph）
// =============================================================================
// 理论点：
//   C++ 的类继承关系在【编译期】就是完全已知的静态结构
//   （[class.derived]：每个类的直接基类写在声明里）。
//   语义分析阶段收集所有类声明后，即可按"基类 → 派生类"建边，
//   做一遍 DFS 把整棵继承树打印出来——这不需要任何运行时信息，
//   与 dynamic_cast 依赖的运行时 RTTI 形成对照：
//     继承图  = 编译期静态视图（源码写了什么就是什么）
//     RTTI    = 运行时动态视图（对象实际是哪个类型）
// 本文件刻意构造：
//   - 两棵独立的树（Shape 系 与 Animal 系）→ 验证多根输出
//   - 三级继承（Shape → Rectangle → Square）→ 验证缩进递进
//   - 非多态类（Point，无虚函数）→ 验证 [polymorphic] 标记只标虚类
// 预期：编译日志中出现
//   │ ═══ 类型继承图（inheritance graph）═══
//   │ Shape [polymorphic]
//   │ └── Rectangle [polymorphic]
//   │     └── Square [polymorphic]
//   │ Animal [polymorphic]
//   │ ├── Dog [polymorphic]
//   │ └── Cat [polymorphic]
//   │ Point
// =============================================================================

class Shape {
public:
    virtual int area() { return 0; }
};

class Rectangle : public Shape {
public:
    int w;
    int h;
    virtual int area() { return 1; }
};

class Square : public Rectangle {
public:
    virtual int area() { return 2; }
};

class Animal {
public:
    virtual int speak() { return 0; }
};

class Dog : public Animal {
public:
    virtual int speak() { return 1; }
};

class Cat : public Animal {
public:
    virtual int speak() { return 2; }
};

class Point {
public:
    int x;
    int y;
};

int main() {
    // 只验证编译能过：继承图的产出发生在语义阶段，
    // 运行时本程序本身没有事可做
    Point* p = new Point();
    p->x = 1;
    p->y = 2;
    return 0;
}

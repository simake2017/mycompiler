// =============================================================================
// 测试用例：展示 minicc 编译器支持的所有特性
// =============================================================================

// ─── 1. 普通函数 ─────────────────────────────────────────────────────────────
int add(int a, int b) {
    return a + b;
}

int factorial(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

// ─── 2. auto 类型推导 ────────────────────────────────────────────────────────
int testAuto() {
    auto x = 10;
    auto y = 20;
    auto sum = x + y;
    auto flag = true;
    return sum;
}

// ─── 3. 类与虚函数（展示 vtable 和 RTTI 注入） ──────────────────────────────
class Shape {
public:
    int x;
    int y;

    virtual int area() {
        return 0;
    }

    virtual int perimeter() {
        return 0;
    }
};

class Rectangle : public Shape {
public:
    int width;
    int height;

    virtual int area() override {
        return width * height;
    }

    virtual int perimeter() override {
        return (width + height) * 2;
    }
};

class Circle : public Shape {
public:
    int radius;

    virtual int area() override {
        return 3 * radius * radius;
    }
};

// ─── 4. 多态调用（虚函数调用三部曲） ─────────────────────────────────────────
int testPolymorphism() {
    Shape* shape = new Rectangle();
    int a = shape->area();
    int p = shape->perimeter();
    return a + p;
}

// ─── 5. 模板类（蓝图，等待实例化） ───────────────────────────────────────────
template<typename T>
class MyPtr {
public:
    T value;

    T get() {
        return value;
    }

    void set(T v) {
        value = v;
    }
};

// ─── 6. 字段访问（展示偏移量消除） ──────────────────────────────────────────
int testFieldAccess() {
    Rectangle rect;
    rect.width = 10;
    rect.height = 20;
    int a = rect.width * rect.height;
    return a;
}

// ─── 7. 控制流 ──────────────────────────────────────────────────────────────
int testControlFlow() {
    int sum = 0;
    int i = 1;
    while (i <= 10) {
        sum = sum + i;
        i = i + 1;
    }

    if (sum > 50) {
        return 1;
    } else {
        return 0;
    }
}

// ─── 8. 主函数 ──────────────────────────────────────────────────────────────
int main() {
    auto result1 = add(3, 4);
    auto result2 = factorial(5);
    auto result3 = testAuto();
    auto result4 = testFieldAccess();
    auto result5 = testControlFlow();
    auto result6 = testPolymorphism();

    return 0;
}

// =============================================================================
// 测试：构造函数与成员初始化列表 (Constructor & Member Initializer List)
// =============================================================================
// 理论点：
//   - C++ 构造函数 [class.ctor] 与成员初始化列表 [class.base.init]
//   - new 表达式动态分配与构造调用：malloc(sizeof(T)) -> T::T(args) -> return ptr
// 预期行为：
//   - Point 包含 (x, y) 两个成员，通过构造函数初始化列表赋值
//   - 编译并通过，生成 Point_Point 构造调用汇编
// =============================================================================

class Point {
public:
    int x;
    int y;

    Point(int px, int py) : x(px), y(py) {}

    int sum() {
        return x + y;
    }
};

int main() {
    Point* p = new Point(10, 20);
    return p->sum();
}

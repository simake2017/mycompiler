// =============================================================================
// 测试：堆上模板实例 —— new Box<int>() / Box<int>* p / p->method() / delete
// =============================================================================
// 理论点：
//   [expr.new] 的模板形态：new Box<int>() 里的模板 id 在语义阶段被
//   实例化抹平（className 原地改写为 Box_int），CodeGen::emitNew 只认
//   实例名——与"先实例化、后降级"的全局策略一致；
//   堆对象不走 RAII 自动析构（块尾析构只覆盖栈对象），必须显式 delete
//   —— 这正是 unique_ptr 存在的原因（见 test_stl_01）。
//
// 预期：编译通过，运行退出码 0
// =============================================================================

template<typename T>
class Box {
public:
    T value;
    Box() { value = 0; }
    void set(T v) { value = v; }
    T get() { return value; }
};

int main() {
    Box<int>* p = new Box<int>();   // malloc 4 字节 + Box_int_Box_int 构造
    p->set(9);                      // 箭头成员调用：this = 指针值
    int v = p->get();
    delete p;                       // 显式析构 + free（堆对象不自动析构）
    return v - 9;                   // 0
}

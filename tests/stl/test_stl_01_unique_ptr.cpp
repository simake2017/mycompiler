// =============================================================================
// 测试：STL 封装第一步 —— 块作用域析构（RAII）与智能指针雏形
// =============================================================================
// 理论点（[class.dtor] + RAII）：
//   1. 栈上类对象在离开作用域时自动调用析构函数，且逆声明序
//      （LIFO —— 与构造顺序相反，和符号表 enterScope/exitScope 同构）；
//   2. RAII（Resource Acquisition Is Initialization）：把资源释放绑定到
//      栈对象的析构上 —— 对象死，资源灭，无需手写释放代码；
//   3. unique_ptr 的本质 = 一个"析构时 delete 所持指针"的类对象。
//      minicc 的 new 目前只认类类型，故包装类持有 new Resource() 的指针。
//
// 本文件覆盖点：
//   A. 栈对象构造：块内 Resource r —— 帧内分配 + 清零 + 调构造
//   B. 块尾自动析构 + 逆序（观察 .s 中 "[block dtor]" 注释的顺序）
//   C. 虚析构的栈对象：经 vtable 分派的析构调用
//   D. UniqueResource（unique_ptr 雏形）：析构体内 delete ptr，
//      块尾自动析构 → 连锁释放堆内存（RAII 完整闭环）
//
// 预期：编译通过，运行退出码 0（v == 42 → 返回 0）
// =============================================================================

class Resource {
public:
    int value;
    Resource() { value = 42; }
    int get() { return value; }
    ~Resource() { value = 0; }
};

// 虚析构验证：含虚函数的类 → 析构进 vtable → 栈对象析构走虚调用
// （~VBase 必须写 virtual，才会进入 vtable —— 见 docs/learn/11）
class VBase {
public:
    int tag;
    VBase() { tag = 7; }
    virtual int kind() { return 1; }
    virtual ~VBase() { tag = 0; }
};

// unique_ptr 雏形：析构时释放所持资源
// （注意：构造函数里 ptr = new Resource(); 是"裸字段赋值"，
//   依赖 codegen 的 this + 偏移写入路径）
class UniqueResource {
public:
    Resource* ptr;
    UniqueResource() { ptr = new Resource(); }
    // （用 get() 间接取值：minicc 的 "->" 成员方法调用可用，
    //   "->" 裸字段读取尚未支持 —— 见 docs/learn/12 已知简化表）
    int get() { return ptr->get(); }
    ~UniqueResource() { delete ptr; }
};

int main() {
    int v = 0;

    {   // ── 块 1：普通栈对象的构造/析构 + 逆序 ──
        Resource r1;      // 构造 1
        Resource r2;      // 构造 2（块尾应先析构 r2 再析构 r1）
        v = r1.get();     // 42
    }   // ← 自动析构点（观察 .s：r2 的析构在前）

    {   // ── 块 2：虚析构栈对象 ──
        VBase b;
        v = v + b.kind(); // 42 + 1 = 43
    }   // ← 析构经 vtable 分派（观察 .s："[virtual dtor]" 三部曲）

    {   // ── 块 3：RAII 闭环 —— UniqueResource 自动释放堆内存 ──
        UniqueResource u;
        v = v + u.get();  // 43 + 42 = 85
    }   // ← u 析构 → delete ptr → 析构 Resource + free

    return v - 85;        // 0
}

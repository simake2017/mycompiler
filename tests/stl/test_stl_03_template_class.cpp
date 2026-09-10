// =============================================================================
// 测试：类模板按需实例化闭环（P3）—— Box<int> / Box<double> 同蓝图两实例
// =============================================================================
// 理论点（[temp.inst] / [temp.spec]）：
//   真 C++ 里类模板不是类型，模板 id（Box<int>）在"被使用"的那一点才实例化
//   出一个真正的类；同一实参组合只实例化一次（[temp.inst]/3）。
//   minicc 的闭环路径：
//     parseType  ：Box<int> → Class(name=Box, templateArgs=[int])
//     parseStmt  ：模板 id 前缀前瞻 → 判定为变量声明
//     resolveType：带实参的类名命中蓝图 → getOrInstantiateClass
//     instantiate：深拷贝蓝图 + {T→int} 结构化替换 → Box_int
//     sema 注册  ：processClassDecl 完整注册 + 方法体两阶段第二阶段分析
//     codegen    ：Box_int 与普通类同等待遇（构造/析构/布局）
//
// 预期：编译通过，运行退出码 0
//   (7 + 0 + 3) - 10 == 0
// =============================================================================

template<typename T>
class Box {
public:
    T value;

    Box() { value = 0; }

    T get() { return value; }

    void set(T v) { value = v; }
};

int main() {
    Box<int> bi;          // 触发 Box_int 实例化（按需，首次出现处）
    bi.set(7);

    Box<double> bd;       // 触发 Box_double 实例化（同一蓝图第二实例）
    bd.set(3);

    Box<int> bi2;         // 命中缓存：不重复实例化（日志见 cache hit）
    bi2.set(0);

    // double 实例的值单独取用再汇入：minicc 数值规则里 int+double 会提升为
    // double，直接混算会让 return 的类型检查失败（[stmt.return]）
    int sum = bi.get() + bi2.get();
    int dov = 0;
    if (bd.get() == 3) { dov = 3; }   // 比较运算 → bool，双精度实例真的在干活

    return sum + dov - 10;   // 0
}

// =============================================================================
// 测试：全局变量、命名空间、枚举与类型别名 (Declarations)
// =============================================================================
// 理论点：
//   - 全局变量发射到 .data 段并支持 RIP 相对寻址 [dcl.dcl]
//   - 命名空间作用域与限定名查找 [namespace.def]
//   - 枚举类型与作用域枚举 [dcl.enum]
//   - 类型别名 using / typedef 别名展开 [dcl.typedef]
// =============================================================================

using Integer = int;
typedef int Counter;

enum Status {
    Ok = 0,
    Error = 1
};

namespace Math {
    int g_factor = 2;
    int scale(int x) {
        return x * 2;
    }
}

int g_total = 10;

int main() {
    Integer a = 5;
    Counter b = Math::scale(a);
    return g_total + b;
}

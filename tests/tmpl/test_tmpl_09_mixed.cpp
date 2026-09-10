// =============================================================================
// 测试：模板 + 非模板混合 (Template + Non-Template Mixed)
// =============================================================================
// 模板类与普通类、普通函数共存的场景。
// 验证编译器在处理模板蓝图时不会干扰普通代码的分析。
//
// 编译过程中的关键日志：
//   Phase 3 [Sema]:
//     Pass 1:
//       [register] class 'Animal'      ← 普通类正常注册
//       [register] template <typename T> Stack (blueprint stored)  ← 模板只存蓝图
//     Pass 2:
//       [register] Animal::getName()   ← 普通方法正常注册
//       [register] add()               ← 普通函数正常注册
//     Pass 3:
//       Function Body: Animal::getName  ← 普通方法正常分析
//       Function Body: add              ← 普通函数正常分析
//
//   Phase 4 [Instantiation]:
//     模板实例化不影响已注册的普通类
// =============================================================================

class Animal {
public:
    int age;

    int getAge() {
        return age;
    }

    void setAge(int a) {
        age = a;
    }
};

template<typename T>
class Stack {
public:
    T top;
    int size;

    T peek() {
        return top;
    }

    void push(T val) {
        top = val;
    }

    int getSize() {
        return size;
    }
};

int add(int a, int b) {
    return a + b;
}

int main() {
    auto result = add(10, 20);
    return 0;
}

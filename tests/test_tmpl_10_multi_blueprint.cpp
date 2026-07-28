// =============================================================================
// 测试：多模板蓝图共存 (Multiple Template Blueprints Coexisting)
// =============================================================================
// 同一个文件中定义多个不同的模板类，验证编译器能正确存储和处理多个蓝图。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     template <typename T> Container stored as blueprint
//     template <typename T, typename U> Mapper stored as blueprint
//
//   Phase 3 [Sema]:
//     [register] template <typename T> Container (blueprint stored)
//     [register] template <typename T, typename U> Mapper (blueprint stored)
//
//   Phase 4 [Instantiation]:
//     Found 2 template blueprint(s)
//
//     Template blueprint: Container <T>
//       ─── Instantiation 1: Container<int> ───
//       ─── Instantiation 2: Container<double> ───
//       ... (6 instantiations for single-param)
//
//     Template blueprint: Mapper <T, U>
//       ─── Instantiation: Mapper<int, double> ───
// =============================================================================

template<typename T>
class Container {
public:
    T data;

    T get() {
        return data;
    }

    void set(T val) {
        data = val;
    }
};

template<typename T, typename U>
class Mapper {
public:
    T key;
    U value;

    T getKey() {
        return key;
    }

    U getValue() {
        return value;
    }

    void setKey(T k) {
        key = k;
    }

    void setValue(U v) {
        value = v;
    }
};

int main() {
    auto x = 1;
    return 0;
}

// =============================================================================
// 测试：三参数模板 + class/typename 混合 (Triple Parameters + Mixed Keywords)
// =============================================================================
// 三个模板参数，并且混合使用 class 和 typename 关键字。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     [parse:template] parsing parameter list <class T, typename U, class V>
//     ★ type parameter registered: 'T'   (class keyword)
//     ★ type parameter registered: 'U'   (typename keyword)
//     ★ type parameter registered: 'V'   (class keyword)
//
//   Phase 4 [Instantiation]:
//     Substitution map: { 'T' → 'int', 'U' → 'double' }
//     注意：main.cpp 中只处理了 1参数和2参数模板的自动实例化
//     三参数模板需要手动扩展 main.cpp 的实例化逻辑
//     但模板蓝图的解析和存储是完全正确的
// =============================================================================

template<class T, typename U, class V>
class Triple {
public:
    T first;
    U second;
    V third;

    T getFirst() {
        return first;
    }

    U getSecond() {
        return second;
    }

    V getThird() {
        return third;
    }

    void setFirst(T f) {
        first = f;
    }

    void setSecond(U s) {
        second = s;
    }

    void setThird(V t) {
        third = t;
    }
};

int main() {
    auto x = 1;
    return 0;
}

// =============================================================================
// 测试：多参数模板 (Multi-Parameter Template)
// =============================================================================
// 两个模板参数 T 和 U，展示多参数替换过程。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     [parse:template] parsing parameter list <typename T, typename U>
//     ★ type parameter registered: 'T'
//     ★ type parameter registered: 'U'
//
//   Phase 4 [Instantiation]:
//     Substitution map: { 'T' → 'int', 'U' → 'double' }
//     field 'first'  : T → int
//     field 'second' : U → double
//     method 'getFirst'  : return T → int
//     method 'getSecond' : return U → double
//     method 'setFirst'  : param f:T → int
//     method 'setSecond' : param s:U → double
//     Mangled: Pair_int_double → _Z4PairIdE  (注意: i=int, d=double)
// =============================================================================

template<typename T, typename U>
class Pair {
public:
    T first;
    U second;

    T getFirst() {
        return first;
    }

    U getSecond() {
        return second;
    }

    void setFirst(T f) {
        first = f;
    }

    void setSecond(U s) {
        second = s;
    }
};

int main() {
    auto x = 1;
    return 0;
}

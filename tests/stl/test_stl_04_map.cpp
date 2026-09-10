// =============================================================================
// 测试：P2 × P3 合体 —— 双参数类模板 Map<K,V> + 下标糖 m[k] ≡ m.at/set
// =============================================================================
// 理论点：
//   [temp.arg.explicit] 多实参模板：Map<int, int> 两实参与两形参对齐，
//     实例名按"模板名_实参1_实参2"编码为 Map_int_int；
//   [expr.sub] 糖化约定（P2）：类提供 at(K)/set(K,V) 即获得下标读写能力，
//     模板实例同样自动获得——实例类与普通类在 CodeGen 眼中一视同仁；
//   槽位管理：定容 4 槽，key==0 约定为"空槽"（依赖栈对象零初始化，
//     即 P1 的"清零 + 构造"约定），真容器用 malloc/哈希，见已知简化。
//
// 预期：编译通过，运行退出码 0
//   111 + 200 - 311 == 0
// =============================================================================

template<typename K, typename V>
class Map {
public:
    K k0;
    K k1;
    K k2;
    K k3;
    V v0;
    V v1;
    V v2;
    V v3;

    // 约定方法 1：at(key) —— 下标读
    V at(K key) {
        if (k0 == key) { return v0; }
        if (k1 == key) { return v1; }
        if (k2 == key) { return v2; }
        return v3;
    }

    // 约定方法 2：set(key, val) —— 下标写（已有 key 覆盖，否则占空槽）
    V set(K key, V val) {
        if (k0 == key) { v0 = val; return val; }
        if (k1 == key) { v1 = val; return val; }
        if (k2 == key) { v2 = val; return val; }
        if (k3 == key) { v3 = val; return val; }
        if (k0 == 0) { k0 = key; v0 = val; return val; }
        if (k1 == 0) { k1 = key; v1 = val; return val; }
        if (k2 == 0) { k2 = key; v2 = val; return val; }
        k3 = key;
        v3 = val;
        return val;
    }
};

int main() {
    Map<int, int> m;      // 双参数按需实例化 → Map_int_int（栈对象，零初始化）
    m[10] = 100;          // → m.set(10, 100)：占槽 0
    m[20] = 200;          // → m.set(20, 200)：占槽 1
    m[10] = 111;          // key 已存在 → 覆盖槽 0，不新开槽
    int a = m[10];        // → m.at(10) = 111
    int b = m[20];        // → m.at(20) = 200
    return a + b - 311;   // 0
}

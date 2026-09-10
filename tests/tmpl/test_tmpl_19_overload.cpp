// =============================================================================
// test_tmpl_19_overload.cpp —— S6 正例：重载决议（非模板优先 + 偏序）
// =============================================================================
// 考察理论点（[over.match.best]）：
//   1. 非模板函数优先于同签名的模板特化 —— twice(2) 选普通 twice(int)
//      得 6（若误选模板 twice<T> 得 4，退出码 ≠ 0）
//   2. 模板间偏序（[temp.func.order]）：po(nullptr) 两个候选都可行，
//      po(T*) 比 po(T) 更特化（用 T* 当合成实参推导 po(T) 成功，
//      反向失败）→ 日志中应见偏序选择 po(T*) 实例化
//
// 预期行为：退出码 = 0；日志含 "partial ordering" 与 po 的 _Z...  实例化
// =============================================================================

int twice(int x) {
    return x * 3;
}

template<typename T>
T twice(T x) {
    return x + x;
}

template<typename T>
void po(T x) {}

template<typename T>
void po(T* x) {}

int main() {
    int r = twice(2);   // 非模板优先 → 6
    po(nullptr);        // 偏序 → po(T*) 胜（日志验证）
    return r - 6;
}

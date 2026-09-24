// =============================================================================
// 测试：下标运算符语法糖 —— v[i] ≡ v.at(i)，v[i] = x ≡ v.set(i, x)
// =============================================================================
// 理论点（[expr.sub] / [over.sub] 的糖化）：
//   真 C++ 里 v[i] 是对 operator[] 的重载调用，重载决议发生在语义阶段；
//   minicc 尚无运算符重载，于是把决议"固化"成约定：
//     类提供 at(int)  → 自动获得"下标读"能力
//     类提供 set(int, T) → 自动获得"下标写"能力
//   Parser 产出 IndexExpr；sema inferIndex 校验 at() 约定并定型；
//   CodeGen 右值位置发射 callq <类>_at，赋值目标位置发射 callq <类>_set。
//
// 本文件覆盖点：
//   A. 写形态：v[0] = 10（AssignStmt.target 是 IndexExpr → set）
//   B. 读形态：int x = v[1]（IndexExpr 作为表达式 → at）
//   C. 嵌套：v[2] = v[0] + v[1] + 1（两侧都是下标糖）
//   D. 栈对象 + RAII：v 是栈上类对象，离开 main 块前自动析构
//
// 预期：编译通过，运行退出码 0（10 + 20 + 31 == 61 → 返回 0）
//
// ✅ 回归点（此处历史上曾恒失败）——【带参成员方法】的符号拼装两侧不一致：
//     定义点（Sema）：方法名追加“参数个数”后缀 ⇒ IntVec_at_1 / IntVec_set_2
//     调用点（CodeGen）：硬拼“类名_方法名”       ⇒ IntVec_at / IntVec_set
//   ⇒ 报 "undefined reference to 'IntVec_at'"
//   修法：Sema 把 at()/set() 的符号回填进 IndexExpr（atSymbol / setSymbol），
//   CodeGen 优先用回填值 —— 与 MemberExpr 的 resolvedCalleeSymbol 同一套路；
//   普通带参成员调用 `c.f(1)` 走的是同一处修复。
// =============================================================================

// 定容 4 槽的整数容器（不依赖 malloc —— 模板版/扩容版见 test_stl_03/04）
class IntVec {
public:
    int a0;
    int a1;
    int a2;
    int a3;

    // 约定方法 1：at(i) —— 下标读
    int at(int i) {
        if (i == 0) { return a0; }
        if (i == 1) { return a1; }
        if (i == 2) { return a2; }
        return a3;
    }

    // 约定方法 2：set(i, v) —— 下标写
    int set(int i, int v) {
        if (i == 0) { a0 = v; }
        if (i == 1) { a1 = v; }
        if (i == 2) { a2 = v; }
        if (i == 3) { a3 = v; }
        return 0;
    }
};

int main() {
    IntVec v;          // 栈对象：清零 + 合成构造；离开 main 自动析构
    v[0] = 10;         // B → set：IntVec_set(this, 0, 10)
    v[1] = 20;         // B → set
    v[2] = v[0] + v[1] + 1;  // C：读读读，写 —— 21
    int x = v[0] + v[1] + v[2];   // C：10 + 20 + 31 = 61
    return x - 61;     // 0
}

// =============================================================================
// 测试：带参虚函数的 vtable 槽符号（定义点与槽位必须同源）
// =============================================================================
// 理论点：
//   - [class.virtual]/1：覆写走"同槽位改写"——索引不变、只有槽里的地址换成本类实现。
//     槽里那个地址在汇编层面就是**一个符号名**（`.quad <sym>`），链接器只按裸字符串
//     比对，所以槽里的名字必须与函数定义处的 `.globl` 逐字相同。
//   - 本项目 mangling：[temp]/[class] 教学级简化 —— 成员函数符号 = `类名_方法名`，
//     且**带形参时追加 `_<形参个数>`**（semantic_analyzer.cpp registerFunction）。
//     这条后缀规则一旦只写在定义点、没写在 vtable 条目点，就是"同一条语义判断写两处"。
//   - 对照 clang：定义与引用共用同一个 MangleContext（ItaniumMangleContext::mangleName），
//     结构上不可能两边不一致；本实现是靠把判据收口到一个函数来达到同样效果。
// 预期行为：
//   - `Shape::area(int)` 定义为 Shape_area_1；vtable 槽里也必须是 Shape_area_1
//   - `Square::area(int)` 覆写 ⇒ 同一槽位改写为 Square_area_1（索引不变）
//   - `scaled(int,int)` 不覆写 ⇒ 槽里仍是 Shape_scaled_2，通过基类指针也能调到
//   - 运行返回 0
// 回归背景（docs/BUGS.md B20 缺陷 a）：
//   - 修复前 vtable 条目按 `类名_方法名` 硬拼（无 `_<参数个数>` 后缀），
//     定义点却是 Shape_area_1 ⇒ 汇编里 `_ZTV` 槽写 `.quad Shape_area`，
//     链接期 `undefined reference to 'Shape_area'`；编译期全程绿灯。
//   - 无参虚函数（area()）不受影响 —— 两侧算出的都是 Shape_area，故旧测试全绿，
//     这个缺口一直没被任何用例覆盖到。
// =============================================================================

class Shape {
public:
    virtual int area(int k) { return k; }          // 带 1 参 ⇒ 符号 Shape_area_1
    virtual int scaled(int k, int m) { return k * m; }  // 带 2 参 ⇒ Shape_scaled_2
};

class Square : public Shape {
public:
    int area(int k) { return k * 2; }              // 覆写：同槽位改写（索引仍为 0）
    // scaled 不覆写 ⇒ 槽里仍指向 Shape_scaled_2
};

int main() {
    Square s;
    Shape* p = &s;

    int a = p->area(3);        // 动态分派 → Square_area_1 = 6
    if (a != 6) return 1;

    int b = p->scaled(3, 4);   // 未覆写 → Shape_scaled_2 = 12
    if (b != 12) return 2;

    Shape sh;
    Shape* q = &sh;
    if (q->area(3) != 3) return 3;      // 基类自身 → Shape_area_1
    if (q->scaled(2, 5) != 10) return 4;

    return 0;
}

// =============================================================================
// dump 测试 04：深层继承 + 多重字段
// =============================================================================
// 测试目的：验证深层继承链和多字段的内存布局
// 继承关系：Level1 → Level2 → Level3 → Level4
// 每层都有多个字段和虚函数
// 运行命令：
//   ./minicc tests/dump/dump_04_deep.cpp --dump-hierarchy
//   ./minicc tests/dump/dump_04_deep.cpp --dump-layout
// =============================================================================

class Level1 {
public:
    int l1_a;
    int l1_b;
    virtual int sum1() { return l1_a + l1_b; }
};

class Level2 : public Level1 {
public:
    int l2_a;
    int l2_b;
    int l2_c;
    virtual int sum2() { return l2_a + l2_b + l2_c; }
    virtual int sum1() { return l1_a + l1_b; }  // override
};

class Level3 : public Level2 {
public:
    int l3_x;
    int l3_y;
    virtual int sum3() { return l3_x + l3_y; }
    virtual int sum1() { return l1_a + l1_b; }  // override
    virtual int sum2() { return l2_a + l2_b + l2_c; }  // override
};

class Level4 : public Level3 {
public:
    int l4_only;
    virtual int sum4() { return l4_only; }
    virtual int sum1() { return l1_a + l1_b; }  // override
    virtual int sum2() { return l2_a + l2_b + l2_c; }  // override
    virtual int sum3() { return l3_x + l3_y; }  // override
};

int main() {
    Level4* obj = new Level4();
    obj->l1_a = 1;
    obj->l1_b = 2;
    obj->l2_a = 10;
    obj->l2_b = 20;
    obj->l2_c = 30;
    obj->l3_x = 100;
    obj->l3_y = 200;
    obj->l4_only = 1000;
    return obj->sum1() + obj->sum2() + obj->sum3() + obj->sum4();
}

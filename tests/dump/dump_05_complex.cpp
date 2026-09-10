// =============================================================================
// dump 测试 05：多继承 + 多层
// =============================================================================
// 测试目的：验证复杂的多继承场景
// 继承关系：
//   Base1, Base2 (独立基类)
//      \   /
//      Middle (多继承 Base1, Base2)
//         |
//       Final (继承 Middle，添加新字段)
// 运行命令：
//   ./minicc tests/dump/dump_05_complex.cpp --dump-hierarchy
//   ./minicc tests/dump/dump_05_complex.cpp --dump-layout
// =============================================================================

class Base1 {
public:
    int b1;
    virtual int getB1() { return b1; }
};

class Base2 {
public:
    int b2;
    virtual int getB2() { return b2; }
};

class Middle : public Base1, public Base2 {
public:
    int mid;
    virtual int getMid() { return mid; }
    virtual int getB1() { return b1; }  // override
    virtual int getB2() { return b2; }  // override
};

class Final : public Middle {
public:
    int final_val;
    virtual int getFinal() { return final_val; }
    virtual int getB1() { return b1; }  // override
    virtual int getB2() { return b2; }  // override
    virtual int getMid() { return mid; }  // override
};

int main() {
    Final* obj = new Final();
    obj->b1 = 1;
    obj->b2 = 2;
    obj->mid = 10;
    obj->final_val = 100;
    return obj->getB1() + obj->getB2() + obj->getMid() + obj->getFinal();
}

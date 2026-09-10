// =============================================================================
// dump 测试 03：钻石继承（菱形继承）
// =============================================================================
// 测试目的：验证钻石继承的类层次和内存布局输出
// 继承关系：
//       Base
//      /    \
//    Left  Right
//      \    /
//      Bottom
// 注意：minicc 不支持虚继承，所以 Bottom 会有两份 Base 子对象
// 运行命令：
//   ./minicc tests/dump/dump_03_diamond.cpp --dump-hierarchy
//   ./minicc tests/dump/dump_03_diamond.cpp --dump-layout
// =============================================================================

class Base {
public:
    int base_val;
    virtual int getBase() { return base_val; }
};

class Left : public Base {
public:
    int left_val;
    virtual int getLeft() { return left_val; }
    virtual int getBase() { return base_val; }  // override
};

class Right : public Base {
public:
    int right_val;
    virtual int getRight() { return right_val; }
    virtual int getBase() { return base_val; }  // override
};

class Bottom : public Left, public Right {
public:
    int bottom_val;
    virtual int getBottom() { return bottom_val; }
    virtual int getBase() { return 0; }  // override (ambiguous, just return 0)
    virtual int getLeft() { return 0; }  // override
    virtual int getRight() { return 0; }  // override
};

int main() {
    Bottom* obj = new Bottom();
    obj->bottom_val = 30;
    return obj->getBottom();
}

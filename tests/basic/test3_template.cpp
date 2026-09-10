// =============================================================================
// 测试用例 3：模板高级特性演示
// =============================================================================
// 本文件展示 minicc 编译器支持的模板高级特性：
//
//   1. T*          — 指针类型模板参数
//   2. T&          — 左值引用
//   3. T&&         — 右值引用 / 万能引用 (Forwarding Reference)
//   4. const T&    — 常量引用
//   5. class T     — 用 class 关键字代替 typename
//   6. 多参数模板  — template<typename T, typename U>
//   7. 引用折叠    — T&& 当 T=int& 时自动折叠为 int&
// =============================================================================

// ─── 1. 基础模板（复习） ─────────────────────────────────────────────────────
// 最简单的模板：一个参数，字段和方法都使用 T
template<typename T>
class MyPtr {
public:
    T value;

    T get() {
        return value;
    }

    void set(T v) {
        value = v;
    }
};

// ─── 2. 指针和引用类型的模板 ─────────────────────────────────────────────────
// 展示 T*, T&, T&& 在模板中的使用
template<typename T>
class RefHolder {
public:
    T*  ptr;        // 指针字段：T* — 当 T=int 时变为 int*
    T&  ref;        // 左值引用字段：T& — 当 T=int 时变为 int&
    T&& rref;       // ★ 右值引用字段：T&& — 万能引用！
                    //   当 T=int  时 → int&&  (右值引用)
                    //   当 T=int& 时 → int&   (引用折叠！左值引用胜出)
                    //   当 T=int&& → int&&    (引用折叠！右值引用保持)

    T* getPtr() {
        return ptr;
    }

    T& getRef() {
        return ref;
    }

    T&& getRRef() {
        return rref;
    }

    void setPtr(T* p) {
        ptr = p;
    }

    void setRef(T& r) {
        ref = r;
    }

    void setRRef(T&& rr) {
        rref = rr;
    }
};

// ─── 3. 用 class 关键字代替 typename ────────────────────────────────────────
// C++ 标准中 template<class T> 和 template<typename T> 完全等价
// minicc 同时支持两种写法
template<class T>
class Container {
public:
    T data;
    int size;

    T getData() {
        return data;
    }

    void setData(T d) {
        data = d;
    }

    int getSize() {
        return size;
    }
};

// ─── 4. 常量引用模板 ────────────────────────────────────────────────────────
// 展示 const T& 的使用场景
template<typename T>
class ConstRefBox {
public:
    const T& value;     // 常量引用字段

    const T& get() {
        return value;
    }
};

// ─── 5. 多参数模板 ──────────────────────────────────────────────────────────
// 两个模板参数：T 和 U
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

// ─── 6. 指针与引用的混合模板 ────────────────────────────────────────────────
// 展示 T** 和 T*& 等组合
template<typename T>
class PointerBox {
public:
    T** doublePtr;     // 双指针：T** — 当 T=int 时变为 int**

    T** getDoublePtr() {
        return doublePtr;
    }

    void setDoublePtr(T** dp) {
        doublePtr = dp;
    }
};

// ─── 7. 普通函数（模板之外的代码仍然正常工作）───────────────────────────────
int add(int a, int b) {
    return a + b;
}

int main() {
    auto result = add(1, 2);
    return 0;
}

// =============================================================================
// 测试：指针类型模板参数 (Pointer Type Template Parameters)
// =============================================================================
// 展示 T* 和 T** 在模板中的使用。
//
// 编译过程中的关键日志：
//   Phase 4 [Instantiation]:
//     当 T = int 时：
//       field 'ptr'  : T* → int*
//       field 'doublePtr' : T** → int**
//       [subst] Pointer(T*) → recursing into pointee...
//       [subst] ★ TemplateParam 'T' → 'int' (direct replacement)
//       [subst] ★ Pointer substituted: T* → int*
//
//     当 T = int* 时（指针的指针！）：
//       field 'ptr'  : T* → int**
//       field 'doublePtr' : T** → int***
//       嵌套指针的递归替换过程清晰可见
// =============================================================================

template<typename T>
class PtrHolder {
public:
    T*  ptr;
    T** doublePtr;

    T* getPtr() {
        return ptr;
    }

    T** getDoublePtr() {
        return doublePtr;
    }

    void setPtr(T* p) {
        ptr = p;
    }

    void setDoublePtr(T** dp) {
        doublePtr = dp;
    }
};

int main() {
    auto x = 42;
    return 0;
}

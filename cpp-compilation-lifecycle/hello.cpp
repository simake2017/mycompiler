// hello.cpp — C++ 编译全生命周期演示用例（LLVM/Clang + macOS arm64）
#include <iostream>

#define VERSION "1.0.0"
#define SQUARE(x) ((x) * (x))

int add(int a, int b) {
    return a + b;
}

int main() {
    int result = add(3, 4);
    std::cout << "Version: " << VERSION << std::endl;
    std::cout << "3 + 4 = " << result << std::endl;
    std::cout << "5^2 = " << SQUARE(5) << std::endl;
    return 0;
}

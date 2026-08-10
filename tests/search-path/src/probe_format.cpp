// 本项目的原始案例：<format> 在 c++/10 档落空 → 全链报错；换 libc++ 后命中
#include <format>
#include <cstdio>
int main() { std::printf("%s\n", std::format("{}", "libc++").c_str()); }

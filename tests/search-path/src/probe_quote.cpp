// 双引号形式：先找"当前文件所在目录"，再按 -I → -isystem → 系统目录 下沉
#include "myhdr.h"
#include <cstdio>
int main() { std::printf("%s\n", HDR_SOURCE); }

// 尖括号形式：跳过"当前文件所在目录"，直接从 -I 开始
#include <myhdr.h>
#include <cstdio>
int main() { std::printf("%s\n", HDR_SOURCE); }

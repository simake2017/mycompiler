// 真实头文件的落空下沉：<stdio.h> 在标准库档/内置档都没有，一路沉到 /usr/include
#include <stdio.h>
int main() { return 0; }

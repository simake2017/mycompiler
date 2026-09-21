// 探针：观察 std::vector 扩容对元素地址的影响（配套 docs/learn/30 §4.2）
//
// 要证的命题：push_back 的"往后放"只在 size < capacity 时成立；
//   一旦撞上 capacity，vector 会申请更大的块、把【全部旧元素】搬过去、
//   free 旧块 —— 于是先前取得的 `&v[i]` 全部失联（而下标 i 不受影响）。
//
// 编译运行：
//   clang++-18 -std=c++20 docs/learn/probes/vector_growth_probe.cpp -o /tmp/vgrow && /tmp/vgrow
#include <cstdio>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> v;
    std::printf("%-14s size=%zu cap=%zu\n", "起始", v.size(), v.capacity());

    size_t prevCap = v.capacity();
    for (char c = 'A'; c <= 'E'; ++c) {
        v.push_back(std::string(1, c));
        // 容量变了 ⇒ 发生过重新分配（size == 1 是首次分配，没有旧元素可搬）
        const bool reallocated = (v.capacity() != prevCap && v.size() > 1);
        std::printf("push 第 %zu 个后 size=%zu cap=%zu  &v[0]=%p%s\n",
                    v.size(), v.size(), v.capacity(), (void*)&v[0],
                    reallocated ? "  ★ 搬家了 —— 此前元素的地址全变"
                                : "  （原地追加，地址没动）");
        prevCap = v.capacity();
    }
    return 0;
}

#pragma once
// =============================================================================
// ministl/ministl.h —— 汇总头
// =============================================================================
// 一次引入全部组件。真实项目中更推荐按需 include 单个头文件，
// 这里提供汇总入口只是为了使用方便。
// =============================================================================

// ── 基础层 ──
#include "type_traits.h"   // 类型萃取（转发 std + 自有 is_memmovable）
#include "utility.h"       // move / forward / swap / pair / 仿函数
#include "iterator.h"      // 迭代器标签 / traits / 适配器 / 插入迭代器

// ── 内存层 ──
#include "memory.h"        // allocator / unique_ptr / shared_ptr / weak_ptr
#include "hash.h"          // 哈希函数

// ── 容器 ──
#include "vector.h"        // 动态数组（连续存储，迭代器即裸指针）
#include "list.h"          // 双向链表（哨兵节点，O(1) 插入删除）
#include "unordered_map.h" // 哈希表（链地址法 + 质数桶）

// ── 算法与视图 ──
#include "algorithm.h"     // sort / find / copy / transform ...
#include "ranges.h"        // 惰性视图与管道（transform / filter / take ...）

// ── 类型擦除与工具 ──
#include "function.h"      // std::function 的等价物

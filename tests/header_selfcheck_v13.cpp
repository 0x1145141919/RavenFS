/*
 * ═══════════════════════════════════════════════════════════════════════════
 * header_selfcheck_v13.cpp —— disk_struct_v1.3.h 自校验翻译单元（TU）
 *
 * 目的（**纯 IDE / 编译期护栏，无运行时行为**）：
 *   1. 让 `include/disk_struct_v1.3.h` 进入 CMake 导出的 compile_commands.json，
 *      从而 clangd / VSCode 能拿到**真实命令**（`-I include`、`-std=gnu++20`），
 *      不再退回 .vscode/c_cpp_properties.json 的兜底（那套带 `-D__KERNEL__`
 *      且内核头路径版本可能过期 → `uint64_t` 未知类型 → 满屏假错）。
 *   2. 顺带把 v1.3 盘面结构的**布局契约**钉在编译期：每个 `RFS_STATIC_ASSERT`
 *      （在头内）+ 本文件的显式 static_assert（关键 offset）任何一条不符即编译失败。
 *
 * 说明：
 *   · 本 TU **不定义任何符号、不链接任何东西**；构造一个可执行仅为让 CMake 收录。
 *   · 头文件对 __cplusplus 走 stdint.h 分支（非 __KERNEL__），与 god(C++) 一致；
 *     ko(C / gnu11) 的校验由内核侧构建另行覆盖。
 *   · v1.3.h 当前仍在改制中：字段一旦定稿，请把下方 offset 断言补全。
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "disk_struct_v1.3.h"

#include <cstddef>   /* offsetof */

/* ── 顶层尺寸契约（头内已有同名断言，此处再以 C++ static_assert 复述） ── */
static_assert(sizeof(struct ptr_in_idx)             == 8,    "ptr_in_idx 8B");
static_assert(sizeof(struct parent_ptr_t)           == 8,    "parent_ptr_t 8B");
static_assert(sizeof(struct name_heap_controller_t) == 16,   "nhc 16B");
static_assert(sizeof(struct raven_Inode_t)          == 256,  "raven_Inode_t 256B");
static_assert(sizeof(struct raven_dir_Inode_t)      == 256,  "raven_dir_Inode_t 256B");
static_assert(sizeof(struct Bptree_meta_t)          == 64,   "Bptree_meta_t 64B");
static_assert(sizeof(struct Bptree_ddentry_leaf_node_t)  == 8192, "dentry leaf 8K");
static_assert(sizeof(struct Bptree_ddentry_inner_node_t) == 8192, "dentry inner 8K");
static_assert(sizeof(struct Bptree_extent_leaf_node_t)   == 8192, "extent leaf 8K");
static_assert(sizeof(struct Bptree_extent_inner_node_t)  == 8192, "extent inner 8K");
static_assert(sizeof(struct rfs_xattr_block_hdr)    == 32,   "xattr hdr 32B");
static_assert(sizeof(struct rfs_xattr_entry)        == 8,    "xattr entry 8B");
static_assert(sizeof(struct RavenSuperblock)        == 4096, "SB 4096B");

/* ── DIR inode 关键 offset（v1.3：数据面双树取代 10 堆描述符） ── */
static_assert(offsetof(struct raven_dir_Inode_t, word1)             == 0,   "word1@0");
static_assert(offsetof(struct raven_dir_Inode_t, mode_xattr)        == 32,  "mode_xattr@32");
static_assert(offsetof(struct raven_dir_Inode_t, size)              == 40,  "size@40");
static_assert(offsetof(struct raven_dir_Inode_t, ddentry_Bptree_root) == 72, "ddentry root@72");
static_assert(offsetof(struct raven_dir_Inode_t, name_data_ctl)               == 80,  "name_data_ctl@80");
static_assert(offsetof(struct raven_dir_Inode_t, name_data_piece_count)       == 88,  "name_data_piece_count@88");
static_assert(offsetof(struct raven_dir_Inode_t, name_data_free_ranges_ctl)   == 96,  "name_data_free_ranges_ctl@96");
static_assert(offsetof(struct raven_dir_Inode_t, name_data_free_ranges_count) == 104, "name_data_free_ranges_count@104");
static_assert(offsetof(struct raven_dir_Inode_t, possible_inline_free_ranges) == 112, "inline_free_ranges@112");

/* ── v1.3 名字数据面双树（映射树 + 空闲区间树）── */
static_assert(sizeof(struct free_range_t)       == 16, "free_range_t 16B");
static_assert(sizeof(struct free_ranges_hint_t) == 16, "free_ranges_hint_t 16B");
static_assert(RFS_ND_FREE_INLINE_MAX == 9, "inline free-range max 9");
static_assert(sizeof(struct Bptree_dir_name_data_extent_inner_node_t)     == 8192, "name_data extent inner 8K");
static_assert(sizeof(struct Bptree_dir_name_data_extent_leaf_node_t)      == 8192, "name_data extent leaf 8K");
static_assert(sizeof(struct Bptree_dir_name_data_free_ranges_inner_node_t) == 8192, "name_data free_ranges inner 8K");
static_assert(sizeof(struct Bptree_dir_name_data_free_ranges_leaf_node_t)  == 8192, "name_data free_ranges leaf 8K");

int main(void) { return 0; }

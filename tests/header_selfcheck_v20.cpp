/*
 * ═══════════════════════════════════════════════════════════════════════════
 * header_selfcheck_v20.cpp —— disk_struct_v2.0.h 自校验翻译单元（TU）
 *
 * 目的（**纯 IDE / 编译期护栏，无运行时行为**）：
 *   1. 让 `include/disk_struct_v2.0.h` 进入 CMake 导出的 compile_commands.json，
 *      从而 clangd / VSCode 能拿到**真实命令**（`-I include`、`-std=gnu++20`），
 *      不再退回 .vscode/c_cpp_properties.json 的兜底（那套带 `-D__KERNEL__`
 *      且内核头路径版本可能过期 → `uint64_t` 未知类型 → 满屏假错）。
 *      —— 与 header_selfcheck_v13.cpp 同套路，v2.0 头单独开一个 TU。
 *   2. 顺带把 v2.0 盘面结构的**布局契约**钉在编译期：每个 `RFS_STATIC_ASSERT`
 *      （在头内）+ 本文件的显式 static_assert（关键尺寸/offset）任何一条不符即编译失败。
 *
 * v2.0 相对 v1.3 的结构级变化（本文件负责钉死）：
 *   · `RavenSuperblock.magic` 8B → **32B（256-bit 高强度魔数）**；
 *   · 删 `inode_array_count` / `root_inode`（root 硬编码 0），新增全局
 *     inode 数据面四字段 `inode_data_ctl / inode_data_plane_pieces_count /
 *     inode_data_fr_ctl / inode_data_fr_count`；
 *   · 时间戳/挂载统计字段扩为 9 个 u64（含 `latest_syn_time` 断电判据）；
 *   · `pad[]` 同步收窄，SB 仍恒 4096B。
 *
 * 说明：
 *   · 本 TU **不定义任何符号、不链接任何东西**；构造一个可执行仅为让 CMake 收录。
 *   · 头文件对 __cplusplus 走 stdint.h 分支（非 __KERNEL__），与 god(C++) 一致；
 *     ko(C / gnu11) 的校验由内核侧构建另行覆盖。
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "disk_struct_v2.0.h"
#include "magics.h"  /* 256-bit 扫描魔数（与盘面头分离；此处仅校验魔数独立） */

#include <cstddef>   /* offsetof */

/* ── v1.3 继承的公共结构尺寸（v2.0 未改动，继续钉死） ── */
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

/* ── 名字数据面双树（v1.3 机械，v2.0 复用同族） ── */
static_assert(sizeof(struct free_range_t)       == 16, "free_range_t 16B");
static_assert(sizeof(struct free_ranges_hint_t) == 24, "free_ranges_hint_t 24B");
static_assert(sizeof(struct Bptree_dir_name_data_extent_inner_node_t)     == 8192, "name_data extent inner 8K");
static_assert(sizeof(struct Bptree_dir_name_data_extent_leaf_node_t)      == 8192, "name_data extent leaf 8K");
static_assert(sizeof(struct Bptree_dir_name_data_free_ranges_inner_node_t) == 8192, "name_data free_ranges inner 8K");
static_assert(sizeof(struct Bptree_dir_name_data_free_ranges_leaf_node_t)  == 8192, "name_data free_ranges leaf 8K");

/* ── v2.0 全局 inode 数据面：映射面 B+树（快照）+ 空闲面扁快照 ── */
static_assert(sizeof(struct inode_dataplane_coordinate_free_range_t) == 16, "idp coord free_range 16B");
static_assert(sizeof(struct inode_data_fr_snapshot_hdr)              == 64, "idp fr snapshot hdr 64B");
static_assert(sizeof(struct Bptree_inode_data_extent_inner_node_t)   == 8192, "inode_data extent inner 8K");
static_assert(sizeof(struct Bptree_inode_data_extent_leaf_node_t)    == 8192, "inode_data extent leaf 8K");
/* 映射 B+树 337 阶 + 1 瞬时槽 = 338 槽，用 extent_t / extent_hint_t */
static_assert(338 * sizeof(struct extent_t) + sizeof(struct Bptree_meta_t) <= 8192, "extent node 337 order fits");
static_assert(338 * sizeof(struct extent_hint_t) + sizeof(struct Bptree_meta_t) <= 8192, "extent inner 337 order fits");
/* 魔数独立性：映射 B+树节点 vs 扁 fr 快照区头 */
static_assert(INODE_DATA_EXTENT_MAGIC1 != INODE_DATA_FR_SNAPSHOT_MAGIC1, "distinct magic groups");

/* ── 寻址坐标语言：常量恒等式（8K 片 = 2 个 4K 块 = 32 个 256B inode） ── */
static_assert(RFS_IDP_INODE_BYTES == 256, "idp inode 256B");
static_assert(RFS_IDP_PIECE_BYTES == 8192, "idp piece 8K");
static_assert(RFS_IDP_BLOCKS_PER_PIECE == 2, "2 blocks / piece");
static_assert(RFS_IDP_INODES_PER_BLOCK == 16, "16 inodes / 4K block");
static_assert(RFS_IDP_INODES_PER_PIECE == 32, "32 inodes / 8K piece");
static_assert((1u << RFS_IDP_SHIFT_INODE_BYTE)  == RFS_IDP_INODE_BYTES, "shift inode->byte = 8");
static_assert((1u << RFS_IDP_SHIFT_INODE_BLOCK) == RFS_IDP_INODES_PER_BLOCK, "shift inode->block = 4");
static_assert((1u << RFS_IDP_SHIFT_INODE_PIECE) == RFS_IDP_INODES_PER_PIECE, "shift inode->piece = 5");
static_assert(RFS_IDP_INODES_PER_BLOCK * RFS_IDP_BLOCKS_PER_PIECE == RFS_IDP_INODES_PER_PIECE, "block*2 = piece");
static_assert(RFS_IDP_INODE_BYTES * RFS_IDP_INODES_PER_PIECE == RFS_IDP_PIECE_BYTES, "256*32 = 8192");
/* 换算宏往返/边界恒等（映射树坐标 = inode>>4；fr 坐标 = inode） */
static_assert(RFS_IDP_INODE_TO_BYTE(0)  == 0 && RFS_IDP_BYTE_TO_INODE(0) == 0, "idp roundtrip 0");
static_assert(RFS_IDP_INODE_TO_BYTE(31) == 31u * 256u, "idx31 -> byte");
static_assert(RFS_IDP_INODE_TO_BLOCK(15) == 0 && RFS_IDP_INODE_TO_BLOCK(16) == 1, "idx15/16 -> block");
static_assert(RFS_IDP_INODE_TO_PIECE(31) == 0 && RFS_IDP_INODE_TO_PIECE(32) == 1, "idx31/32 -> piece");
static_assert(RFS_IDP_BLOCK_TO_INODE(RFS_IDP_INODE_TO_BLOCK(1234)) == 1232, "block roundtrip truncates");

/* ── 超级块顶层尺寸（v2.0 核心契约） ── */
static_assert(sizeof(struct RavenSuperblock) == 4096, "SB 4096B");

/* ── v2.0 契约常量：root 硬编码 + 256-bit SB magic 前缀 ── */
static_assert(RFS_ROOT_INODE_IDX == 0, "root inode idx contract must be 0");
static_assert(RFS_SB_MAGIC_W0 == SUPERBLOCK_MAGIC_V20_W0, "v2.0 SB magic prefix shared (disk/magics)");
static_assert((RFS_SB_MAGIC_W0 & 0xFFFFFFFFu) == 0x45564152u, "SB magic bytes[0:4] = \"RAVE\"");

/* ── 超级块关键 offset（v2.0：magic 32B + 全局 inode 数据面四字段） ── */
static_assert(offsetof(struct RavenSuperblock, magic)                           == 0,   "magic@0 (256-bit)");
static_assert(offsetof(struct RavenSuperblock, version)                         == 32,  "version@32");
static_assert(offsetof(struct RavenSuperblock, world_base_lba)                  == 40,  "world_base_lba@40");
static_assert(offsetof(struct RavenSuperblock, total_blocks)                    == 48,  "total_blocks@48");
static_assert(offsetof(struct RavenSuperblock, inode_data_ctl)                 == 56,  "inode_data_ctl@56");
static_assert(offsetof(struct RavenSuperblock, inode_data_plane_pieces_count)  == 64,  "inode_data_plane_pieces_count@64");
static_assert(offsetof(struct RavenSuperblock, inode_data_fr_ctl)              == 72,  "inode_data_fr_ctl@72");
static_assert(offsetof(struct RavenSuperblock, inode_data_fr_count)            == 80,  "inode_data_fr_count@80");
static_assert(offsetof(struct RavenSuperblock, uuid)                           == 88,  "uuid@88");
static_assert(offsetof(struct RavenSuperblock, volume_name)                    == 104, "volume_name@104");
static_assert(offsetof(struct RavenSuperblock, crc32)                          == 152, "crc32@152");
static_assert(offsetof(struct RavenSuperblock, backup_prev)                    == 156, "backup_prev@156");
static_assert(offsetof(struct RavenSuperblock, backup_next)                    == 164, "backup_next@164");
static_assert(offsetof(struct RavenSuperblock, backup_count)                   == 172, "backup_count@172");
static_assert(offsetof(struct RavenSuperblock, born_time)                      == 176, "born_time@176");
static_assert(offsetof(struct RavenSuperblock, latest_mount_time)              == 184, "latest_mount_time@184");
static_assert(offsetof(struct RavenSuperblock, latest_syn_time)                == 192, "latest_syn_time@192");
static_assert(offsetof(struct RavenSuperblock, latest_umount_time)             == 200, "latest_umount_time@200");
static_assert(offsetof(struct RavenSuperblock, system_mount_time_accumulate)   == 208, "system_mount_time_accumulate@208");
static_assert(offsetof(struct RavenSuperblock, system_mount_counter)           == 216, "system_mount_counter@216");
static_assert(offsetof(struct RavenSuperblock, tools_mount_time_accumulate)    == 224, "tools_mount_time_accumulate@224");
static_assert(offsetof(struct RavenSuperblock, tools_mount_counter)            == 232, "tools_mount_counter@232");
static_assert(offsetof(struct RavenSuperblock, abnormal_shutdown_counts)       == 240, "abnormal_shutdown_counts@240");

/* ── SB inode 数据面控制字：与 raven_dir_Inode_t 名字数据面逐位同构 ── */
static_assert(RFS_IDP_CTL_DIRECT_SHIFT == RFS_ND_CTL_DIRECT_SHIFT, "idp ctl direct shift == nd");
static_assert(RFS_IDP_CTL_PTR_SHIFT    == RFS_ND_CTL_PTR_SHIFT,    "idp ctl ptr shift == nd");
static_assert(RFS_IDP_CTL_PTR_MASK     == RFS_ND_CTL_PTR_MASK,     "idp ctl ptr mask == nd");
static_assert(RFS_IDP_CTL_RSV_MASK     == RFS_ND_CTL_RSV_MASK,     "idp ctl rsv mask == nd");
static_assert(RFS_IDP_CTL_INACTIVE     == ~0ULL, "idp ctl inactive all-F");
static_assert(RFS_IDP_CTL_PTR_INVALID  == RFS_MASK51, "idp ctl ptr invalid 51-ones");
static_assert(RFS_IDP_PIECE_RAW_MASK   == RFS_ND_PIECE_RAW_MASK,   "idp piece raw mask == nd");
/* fr ctl：扁快照区指针 ⇒ bit0 保留；位布局仍与 name_data fr ctl 对位 */
static_assert(RFS_IDP_FR_CTL_RSV0_SHIFT == RFS_ND_FR_CTL_INLINE_SHIFT, "idp fr rsv0 shift == nd inline shift");
static_assert(RFS_IDP_FR_CTL_PTR_SHIFT == RFS_ND_FR_CTL_PTR_SHIFT,     "idp fr ptr shift == nd");
static_assert(RFS_IDP_FR_CTL_PTR_MASK  == RFS_ND_FR_CTL_PTR_MASK,      "idp fr ptr mask == nd");
static_assert(RFS_IDP_FR_CTL_PTR_INVALID == RFS_MASK51, "idp fr ptr invalid 51-ones");
static_assert(RFS_IDP_FR_CTL_RSV0_OK(RFS_IDP_FR_CTL_INACTIVE), "idp fr inactive keeps rsv0=0");
static_assert(RFS_IDP_FR_CTL_PTR(RFS_IDP_FR_CTL_INACTIVE) == RFS_MASK51, "idp fr inactive ptr invalid");
/* 位域功能：pack → get 往返；direct 位不影响指针 */
static_assert(RFS_IDP_CTL_IS_DIRECT(RFS_IDP_CTL_PACK(1, 1234)) == 1, "idp ctl direct bit");
static_assert(RFS_IDP_CTL_PTR(RFS_IDP_CTL_PACK(1, 1234)) == 1234,    "idp ctl ptr roundtrip");
static_assert(RFS_IDP_CTL_PTR(RFS_IDP_CTL_PACK(0, 1234)) == 1234,    "idp ctl ptr independent of direct");
static_assert(RFS_IDP_FR_CTL_RSV0_OK(RFS_IDP_FR_CTL_PACK(999)), "idp fr pack keeps rsv0=0");
static_assert(RFS_IDP_FR_CTL_PTR(RFS_IDP_FR_CTL_PACK(999)) == 999, "idp fr ptr roundtrip");
static_assert(RFS_IDP_PIECE_COUNT(RFS_IDP_PIECE_PACK(4141)) == 4141, "idp piece count roundtrip");

int main(void) { return 0; }

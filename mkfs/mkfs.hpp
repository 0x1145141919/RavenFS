#pragma once

/* mkfs 写出口：v1.1 / v1.2(默认) / v1.3 / v2.0 四版盘面。
 *   · v1.1(version=2) / v1.2(version=3) / v1.3(version=4)：固定 inode 数组 + inode 位图，
 *     实现 mkfs.cpp（盘面头 disk_struct_v1.3.h）。
 *   · v2.0：**弹性 inode 数据面**（无固定数组 / 无 inode 位图；256KB direct 起步），
 *     实现 mkfs_v20.cpp（盘面头 disk_struct_v2.0.h）。
 *
 * ⚠️ 两套盘面头（disk_struct_v1.3.h / disk_struct_v2.0.h）**类型重名且不兼容**，
 *    不能同 TU include：故本头保持**中性**（只 include 纯宏 magics.h，不拉任何盘面
 *    结构头）；disk_struct_v1.3.h 的 include 已从本头移到 mkfs.cpp，v2.0 在
 *    mkfs_v20.cpp 各 include 自己的盘面头。main.cpp 只 include 本中性头。 */

#include "../include/magics.h"   /* SUPERBLOCK_MAGIC / SUPERBLOCK_MAGIC_V20_W0（纯宏，无结构） */

#include <cstdint>
#include <cstddef>

/* 磁盘格式号（写进 SB.version；与各版盘的 RAVENFS_VERSION 对齐） */
enum MkfsFormat : int {
    MKFS_FMT_V1_1 = 1,   /* SB.version = 2（v1.1 盘，与 v1.2/1.3 不兼容） */
    MKFS_FMT_V1_2 = 2,   /* SB.version = 3（v1.2 盘） */
    MKFS_FMT_V1_3 = 3,   /* SB.version = 4（v1.3 盘） */
    MKFS_FMT_V2_0 = 4    /* SB.version = RAVENFS_VERSION（disk_struct_v2.0.h；弹性 inode 数据面） */
};

/* 格式号 → SB.version（v1.1=2 / v1.2=3 / v1.3=4） / 可读名。v2.0 的版本号由盘面头
 * RAVENFS_VERSION 提供，format_fs_v20 直接取用；mkfs_fmt_version 仅服务 v1.x 路径
 * （v2.0 分支返回 RAVENFS_VERSION 的镜像占位，不参与 v2.0 写盘）。 */
uint32_t    mkfs_fmt_version(MkfsFormat f);
const char *mkfs_fmt_name(MkfsFormat f);

/* ── mkfs 局部实现细节（**非契约 · 勿外泄**）——**仅 v1.1/v1.2/v1.3 适用** ──
 * 根目录 inode 索引 = 16：mkfs 为根目录挑的一个确定非零索引（避开 st_ino==0 哨兵）。
 * ⚠️ v2.0 **推翻**此模型：根 hardcode 为 0（**契约级**，见 disk_struct_v2.0.h
 *   `RFS_ROOT_INODE_IDX`）。本宏仅 v1.x 内部使用。 */
#define MKFS_ROOT_INODE 16ULL

struct MkfsParams {
    const char *device;
    uint64_t    inode_ratio;     // 每 inode 管辖的 FS_block 数（默认 8；v2.0 忽略）
    uint64_t    inode_count;     // 输出：由 ratio 换算得，或 --inode-count 直指定（v2.0 忽略）
    bool        inode_count_explicit; // true 表示 --inode-count / --inodes 直指定
    uint32_t    backups;
    char        label[48];
    uint8_t     uuid[16];
    bool        force;
    bool        verbose;
    MkfsFormat  fmt;              // 写哪版盘面（默认 MKFS_FMT_V1_2；--v1.1/--v1.2/--v1.3/--v2.0）

    uint64_t    lba_size;        // NVMe→Identify, file→4096
    uint64_t    multiplier;      // 4096 / lba_size
    uint64_t    world_base_lba;  // FS_block[0] 字节偏移 / lba_size
};

/* v1.x 几何（固定 inode 数组 + inode 位图，尾部链式推导） */
struct FsGeometry {
    uint64_t total_blocks;

    uint64_t bitmap_blocks;
    uint64_t bitmap_start;
    uint64_t inode_blocks;
    uint64_t inode_array_base;
    uint64_t inode_bitmap_blocks;
    uint64_t inode_array_bitmap_base;
};

FsGeometry calc_geometry(uint64_t total_blocks, uint64_t inode_count);

/* v2.0 几何：**只有 block bitmap 尾巴**（bitmap 仍按几何推导在尾部）；无 inode array /
 * 无 inode bitmap（inode 数据面是分配出来的 direct 段，锚在超级块 inode_data_* 字段）。 */
struct V20Geometry {
    uint64_t total_blocks;
    uint64_t bitmap_blocks;
    uint64_t bitmap_start;
};

V20Geometry calc_geometry_v20(uint64_t total_blocks);

enum DevType : int {
    DEV_BLOCK_NVME  = 1,
    DEV_BLOCK_ATA   = 2,
    DEV_REGULAR     = 3,
    DEV_UNKNOWN     = 0
};

struct DevInfo {
    DevType  type;
    uint64_t bytes;
    bool     is_mounted;
};

DevType probe_hardware(int fd, const char *device_path);
int     get_device_info(const char *path, DevInfo *info);
uint64_t get_lba_size(int fd, DevType type);
bool    check_existing_fs(int fd);

/* v1.x 写出口（mkfs.cpp） */
int format_fs(int fd, const MkfsParams &params, const FsGeometry &geo);
/* v2.0 写出口（mkfs_v20.cpp） */
int format_fs_v20(int fd, const MkfsParams &params, const V20Geometry &geo);

#pragma once

/* v1.3：mkfs 支持三版盘面写出口（--v1.1 / --v1.2（默认） / --v1.3）
 *   · v1.1（version=2）：god 仍以 v1.1 头挂盘，测试 / 上帝类团队用
 *   · v1.2（version=3，默认）：现役 mainline 盘面
 *   · v1.3（version=4）：DIR 名字堆 → 名字数据面双树（见 disk_struct_v1.3.h）
 * 三版 SB/inode/dentry 关键偏移一致；分叉点：version · root inode · type 编码 ·
 * permission_bits/mode_xattr · **DIR inode 尾段**（v1.1/v1.2 = 10 名字堆描述符；
 * v1.3 = 名字数据面双树描述 + inline 空闲区间表）。
 * 盘面真值头：include/disk_struct_v1.3.h（C/C++ 双兼容；内含 __KERNEL__ 分支）。 */
#include "../include/disk_struct_v1.3.h"
#include "../include/magics.h"

#include <cstdint>
#include <cstddef>

/* 三版磁盘格式号（写进 SB.version；与各版盘的 RAVENFS_VERSION 对齐） */
enum MkfsFormat : int {
    MKFS_FMT_V1_1 = 1,   /* SB.version = 2（v1.1 盘，与 v1.2/1.3 不兼容） */
    MKFS_FMT_V1_2 = 2,   /* SB.version = 3（v1.2 盘） */
    MKFS_FMT_V1_3 = 3    /* SB.version = 4（v1.3 盘） */
};

/* 格式号 → SB.version（2/3/4） / 可读名（"v1.1"/"v1.2"/"v1.3"） */
uint32_t    mkfs_fmt_version(MkfsFormat f);
const char *mkfs_fmt_name(MkfsFormat f);

/* ── mkfs 局部实现细节（**非契约 · 勿外泄**）─────────────────────────────
 * 根目录 inode 索引 = 16：这只是 **mkfs 格式化时为根目录挑的一个确定非零索引**
 *   （避开 idx 0 —— 用户态把 st_ino==0 视作“已删除/无效”哨兵，见 POSIX/glibc 惯例）。
 * ⚠️ 该值 **不是** 磁盘格式契约的一部分；**任何消费者都不得假定 16**：
 *   根 inode 一律按**普通契约**解析——**从 superblock 的 `root_inode` 字段读取**。
 * ⚠️ **不存在任何保留区**：仅此一个 idx 被根占用；idx 0..15 与 17.. 全部为合法可分配槽
 *   （2026-09-26 更正：曾误写“0..15 不分配（保留位）”，该说法完全不存在，已铲除）。
 * 本宏仅 mkfs 内部使用，故以 MKFS_ 前缀标示“局部实现”，防被误当格式常量。 */
#define MKFS_ROOT_INODE 16ULL

struct MkfsParams {
    const char *device;
    uint64_t    inode_ratio;     // 每 inode 管辖的 FS_block 数（默认 8）
    uint64_t    inode_count;     // 输出：由 ratio 换算得，或 --inode-count 直指定，16 向上取整
    bool        inode_count_explicit; // true 表示 --inode-count / --inodes 直指定
    uint32_t    backups;
    char        label[48];
    uint8_t     uuid[16];
    bool        force;
    bool        verbose;
    MkfsFormat  fmt;              // 写哪版盘面（默认 MKFS_FMT_V1_2；--v1.1/--v1.2/--v1.3）

    uint64_t    lba_size;        // NVMe→Identify, file→4096
    uint64_t    multiplier;      // 4096 / lba_size
    uint64_t    world_base_lba;  // FS_block[0] 字节偏移 / lba_size
};

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

int format_fs(int fd, const MkfsParams &params, const FsGeometry &geo);

#pragma once

/* mkfs 写出口：**仅 v2.0**（弹性 inode 数据面）。
 *   · v2.0：无固定 inode 数组 / 无 inode 位图；256KB direct 起步，
 *     实现 mkfs_v20.cpp（盘面头 disk_struct_v2.0.h）。
 *   · 设备探测 / 已有 FS 检测 / LBA 探测 / 格式名：mkfs_common.cpp。
 *
 * v1.1/v1.2/v1.3 写出口（固定 inode 数组 + inode 位图，盘面头 disk_struct_v1.3.h）
 * 已随 v1.x 盘面头一同退役删除。 */

#include "../include/magics.h"   /* SUPERBLOCK_MAGIC / SUPERBLOCK_MAGIC_V20_W0（纯宏，无结构） */

#include <cstdint>
#include <cstddef>

/* 磁盘格式号（写进 SB.version；与盘面 RAVENFS_VERSION 对齐）。 */
enum MkfsFormat : int {
    MKFS_FMT_V2_0 = 4    /* SB.version = RAVENFS_VERSION（disk_struct_v2.0.h；弹性 inode 数据面） */
};

const char *mkfs_fmt_name(MkfsFormat f);

struct MkfsParams {
    const char *device;
    uint64_t    inode_ratio;     // 每 inode 管辖的 FS_block 数（v2.0 忽略）
    uint64_t    inode_count;     // 输出：由 ratio 换算得，或 --inode-count 直指定（v2.0 忽略）
    bool        inode_count_explicit; // true 表示 --inode-count / --inodes 直指定
    uint32_t    backups;
    char        label[48];
    uint8_t     uuid[16];
    bool        force;
    bool        verbose;
    MkfsFormat  fmt;              // 写哪版盘面（v2.0）

    uint64_t    lba_size;        // NVMe→Identify, file→4096
    uint64_t    multiplier;      // 4096 / lba_size
    uint64_t    world_base_lba;  // FS_block[0] 字节偏移 / lba_size
};

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

/* v2.0 写出口（mkfs_v20.cpp） */
int format_fs_v20(int fd, const MkfsParams &params, const V20Geometry &geo);

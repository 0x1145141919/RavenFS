#pragma once

/* v1.2：mkfs 适配 v1.2 盘面（type:6 / mode_xattr:12|52 / xattr_ptr 哨兵 / version 3）
 * 注：用户态上帝类（juvenile_god）**刻意暂不迁移**，仍用 disk_struct_v1.1.hpp；
 *     mkfs 先行适配 v1.2（见 disk_layout_v1.2_inode.md §8）。
 *
 * 双版本写出口（2026-09-28）：**默认写 v1.2；`--v1.1` 写 v1.1 盘**（god/测试团队
 * 专用 —— god 仍以 disk_struct_v1.1.hpp 挂盘，version≠2 即 RC_ERR_VERSION，
 * v1.2 盘挂不进 god）。两版盘上字节布局全等（SB/inode/dentry 关键偏移实测一致，
 * 见 089be87 “字节 offset 不变，语义重定义”），仅语义取值分叉四处：
 *   version 2|3 · root inode 0|16 · type 编码 DIR=1|4 · permission_bits 裸值|mode_xattr 打包 */
#include "../include/disk_struct_v1.2.h"
#include "../include/magics.h"

#include <cstdint>
#include <cstddef>

/* 磁盘格式版本号由 disk_struct_v1.2.h 的 RAVENFS_VERSION 宏提供（v1.2 = 3） */

/* 根目录 inode 索引（2026-09-26 用户裁定：硬编码 16，不用 0）
 * 理由：用户态 st_ino=0 在 POSIX 语义下 = “已删除”；0 也常作无效哨兵。
 * ⚠️ **只有 root=16 这一个 inode 被 mkfs 占用；不存在任何保留区** ——
 *    索引 0..15 与 17.. 全部可正常分配（2026-09-26 更正：曾误写“0..15 不分配（保留位）”，
 *    该说法完全不存在，已铲除）。 */
#define RAVENFS_ROOT_INODE 16ULL

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
    bool        v11;              // true → 写 v1.1 盘面（--v1.1；god/测试团队用；默认 v1.2）

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

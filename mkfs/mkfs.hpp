#pragma once

#include "../include/disk_struct.hpp"
#include "../include/magics.h"

#include <cstdint>
#include <cstddef>

struct MkfsParams {
    const char *device;
    uint64_t    inode_ratio;     // 每 inode 管辖的 FS_block 数（默认 8）
    uint64_t    inode_count;     // 输出：由 ratio 换算得，16 向上取整
    uint32_t    backups;
    char        label[48];
    uint8_t     uuid[16];
    bool        force;
    bool        verbose;

    uint64_t    lba_size;        // NVMe→Identify, file→4096
    uint64_t    multiplier;      // 4096 / lba_size
    uint64_t    world_base_lba;  // 分区起始 LBA
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
uint64_t detect_partition_start(const char *device_path);
bool    check_existing_fs(int fd);

int format_fs(int fd, const MkfsParams &params, const FsGeometry &geo);

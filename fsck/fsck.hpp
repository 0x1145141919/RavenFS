#pragma once

#include "../include/disk_struct.hpp"
#include "../include/magics.h"

#include <cstdint>
#include <cstddef>

struct FsckResult {
    int errors;
    int warnings;
};

#ifndef RAVENFS_FSGEOMETRY_DEFINED
#define RAVENFS_FSGEOMETRY_DEFINED
struct FsGeometry {
    uint64_t total_blocks;
    uint64_t bitmap_blocks;
    uint64_t bitmap_start;
    uint64_t inode_blocks;
    uint64_t inode_array_base;
    uint64_t inode_bitmap_blocks;
    uint64_t inode_array_bitmap_base;
};
#endif

FsGeometry derive_geometry(uint64_t total_blocks, uint64_t inode_count);

// 返回 LBA_size（字节），失败返回 0
uint64_t fsck_probe_lba_size(int fd);

int fsck_dump(int fd, bool verbose, uint64_t lba_size);

// Dentry B+tree 全量校验（从根目录递归）
int validate_dentry_tree(int fd, uint64_t world_base_lba,
                         uint64_t total_blocks, uint64_t inode_count,
                         uint64_t root_inode);

#pragma once

#include "../include/disk_struct.hpp"
#include "../include/magics.h"

#include <cstdint>
#include <cstddef>

struct FsckResult {
    int errors;
    int warnings;
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

FsGeometry derive_geometry(uint64_t total_blocks, uint64_t inode_count);

// 返回 LBA_size（字节），失败返回 0
uint64_t fsck_probe_lba_size(int fd);

int fsck_dump(int fd, bool verbose, uint64_t lba_size);

#pragma once

#include <cstdint>

#include "bitmap_types.hpp"

struct InodeBitmapCacheConfig {
    const BlockDevice     *dev;
    uint64_t               bitmap_base;
    uint64_t               bitmap_block_count;
    uint64_t               inode_array_count;
    uint8_t                num_of_blocks_per_bucket_log2;
};

class InodeBitmapCacheController {
public:
    explicit InodeBitmapCacheController(const InodeBitmapCacheConfig &cfg);
    ~InodeBitmapCacheController();

    InodeBitmapCacheController(const InodeBitmapCacheController &) = delete;
    InodeBitmapCacheController &operator=(const InodeBitmapCacheController &) = delete;

    uint64_t alloc_inode();
    void     free_inode(uint64_t inum);
    bool     test_inode(uint64_t inum);

    void     shutdown_all();
    bool     is_valid() const { return all_controller != nullptr; }

private:
    const BlockDevice   *dev;
    uint64_t             bitmap_base;
    uint64_t             bitmap_block_count;
    uint64_t             inode_array_upper;
    uint8_t              N;

    uint64_t             ctrl_count;
    uint64_t             bucket_sz;
    uint64_t             bucket_bits;
    uint64_t             ctrl_seg_bits;

    BitmapBlockController *all_controller;
    ProcessorBitmapState   proc;
    uint8_t               *bucket;

    void load_bucket(uint64_t cid);
    void flush_bucket(uint64_t cid);
    void consume_head(BitmapBlockController *ctrl);

    uint64_t try_alloc();
    void     switch_to(uint64_t new_cid);

    static uint64_t scan_run(const uint8_t *buf, uint64_t buf_bits, uint64_t start);
    static void     set_bits(uint8_t *buf, uint64_t start, uint64_t count);
    static void     clear_bits(uint8_t *buf, uint64_t start, uint64_t count);

    BitmapFields decode(uint64_t inum) const;
    uint64_t     encode(uint64_t cid, uint64_t bucket_pos) const;
};

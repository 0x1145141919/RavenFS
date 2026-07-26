#pragma once

#include <cstdint>
#include <thread>
#include <atomic>

#include "bitmap_types.hpp"

class BitmapCacheController {
public:
    explicit BitmapCacheController(const BitmapCacheConfig &cfg);
    ~BitmapCacheController();

    BitmapCacheController(const BitmapCacheController &) = delete;
    BitmapCacheController &operator=(const BitmapCacheController &) = delete;

    uint64_t hard_alloc(uint64_t count);
    uint64_t soft_alloc(uint64_t max_count);
    void     free(uint64_t fblock, uint64_t count);

    void shutdown_all();

    bool is_valid() const { return all_controller != nullptr; }

private:
    const BlockDevice  *dev;
    uint64_t            bitmap_base;
    uint64_t            bitmap_block_count;
    uint16_t            processor_max;
    uint8_t             N;

    uint64_t            ctrl_count;
    uint64_t            bucket_sz;
    uint64_t            bucket_bits;
    uint64_t            ctrl_seg_bits;

    BitmapBlockController *all_controller;
    ProcessorBitmapState  *all_proc_state;
    uint8_t               *raw_buckets;

    std::thread            sweeper;
    std::atomic<bool>      sweeper_stop{false};
    uint8_t               *sweeper_bucket;

    static uint64_t scan_run(const uint8_t *buf, uint64_t buf_bits,
                              uint64_t start, uint64_t need);
    static uint64_t find_longest(const uint8_t *buf, uint64_t buf_bits,
                                  uint64_t max_count);
    static void     set_bits(uint8_t *buf, uint64_t start, uint64_t count);
    static void     clear_bits(uint8_t *buf, uint64_t start, uint64_t count);

    void load_bucket(uint8_t *bucket, uint64_t cid) const;
    void flush_bucket(const uint8_t *bucket, uint64_t cid) const;

    BitmapFields decode(uint64_t bit_idx) const;
    uint64_t     encode(uint64_t cid, uint64_t bucket_pos) const;

    uint64_t try_alloc(ProcessorBitmapState *ps, uint64_t cid,
                        uint64_t count);
    uint64_t try_alloc_soft(ProcessorBitmapState *ps, uint64_t cid,
                             uint64_t max_count);
    void     consume_head(uint8_t *bucket, BitmapBlockController *ctrl);
    void     switch_to(uint64_t old_cid, uint64_t new_cid);

    void flush_processor(uint64_t pid);
    void flush_controller(uint64_t cid);
    void stop_sweeper();
    void sweeper_loop();
};

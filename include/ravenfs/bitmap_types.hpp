#pragma once

#include <cstdint>

#include "block_device.hpp"
#include "../util/lock.h"

static constexpr uint8_t BITMAP_CTRL_OFFLINE     = 0;
static constexpr uint8_t BITMAP_CTRL_ONLINE      = 1;
static constexpr uint8_t BITMAP_CTRL_INTEGRATING = 2;

struct BitmapFields {
    uint64_t controller_id;
    uint64_t block_idx;
    uint64_t bit_in_block;
};

static inline BitmapFields decode_bitmap_idx(uint64_t bit_idx, uint8_t N) {
    uint64_t ctrl_mask = (1ULL << N) - 1;
    return {
        .controller_id = bit_idx >> (N + 15),
        .block_idx     = (bit_idx >> 15) & ctrl_mask,
        .bit_in_block  = bit_idx & 0x7FFF
    };
}

struct bitmap_return_record_sll{
    bitmap_return_record_sll*next;
    uint32_t base;
    uint32_t entry_count;
};

struct BitmapBlockController{
    spinlock_cpp_t lock;
    uint8_t state;
    uint16_t flags;
    uint32_t free_count;
    uint64_t scan_hint;
    bitmap_return_record_sll*head;
};

struct ProcessorBitmapState {
    spinlock_cpp_t   alloc_lock;
    uint64_t         current_cid;
    uint64_t         last_hint_cid;
    uint8_t         *bucket;
};

struct BitmapCacheConfig {
    const BlockDevice     *dev;
    uint64_t               bitmap_base;
    uint64_t               bitmap_block_count;
    uint16_t               processor_max;
    uint8_t                num_of_blocks_per_bucket_log2;
};

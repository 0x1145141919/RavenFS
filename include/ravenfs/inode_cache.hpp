#pragma once

#include <cstdint>

#include "block_device.hpp"
#include "../disk_struct.hpp"
#include "../util/lock.h"

struct Inode_cache_line {
    raven_Inode_t entries[16];
};
static_assert(sizeof(Inode_cache_line) == 4096, "Inode cache line must be 4096B");

struct Inode_cache_line_controller {
    uint16_t           flags;
    spintrylock_cpp_t  lock;
    uint8_t            reserved0;
    uint32_t           num_of_accessor;
    uint64_t           last_access_stamp;
    uint64_t           tag;
};
static_assert(sizeof(Inode_cache_line_controller) == 24,
              "Controller must be 24B");

static constexpr uint16_t INODE_CACHE_V = 1 << 0;
static constexpr uint16_t INODE_CACHE_D = 1 << 1;
static constexpr uint16_t INODE_CACHE_T = 1 << 2;

static constexpr uint8_t WAYS_PER_SET = 8;

struct controller_set {
    Inode_cache_line_controller controllers[WAYS_PER_SET];
};
static_assert(sizeof(controller_set) == 192,
              "controller_set must be 192B (8 × 24B)");

struct InodeCacheConfig {
    const BlockDevice  *dev;
    uint64_t            inode_array_base;
    uint64_t            inode_array_count;
    controller_set     *all_entries;
    Inode_cache_line   *cache_lines;
    uint8_t             num_of_entries_log2;
};

class Inode_cache_controller {
    InodeCacheConfig cfg;
    uint64_t         access_stamp_counter;

    Inode_cache_line *slot_data(uint64_t set, int way) const {
        return cfg.cache_lines + (set * WAYS_PER_SET + way);
    }

public:
    Inode_cache_controller(const InodeCacheConfig &cfg)
        : cfg(cfg), access_stamp_counter(0) {}

    raven_Inode_t *cache_get(uint64_t inum);
    void           cache_put(uint64_t inum);
    void           mark_dirty(uint64_t inum);
    void           flush_all();
    void           flush_inum(uint64_t inum);

    uint64_t sets()            const { return 1ULL << cfg.num_of_entries_log2; }
    uint64_t total_slots()     const { return sets() * WAYS_PER_SET; }
    uint64_t bytes()           const { return total_slots() * 4096; }
    bool     is_valid()        const { return cfg.all_entries != nullptr && cfg.cache_lines != nullptr; }

    const InodeCacheConfig &config() const { return cfg; }

private:
    struct InumFields {
        uint64_t row;
        uint64_t set_idx;
        uint64_t tag;
    };

    InumFields decode_inum(uint64_t inum) const {
        uint64_t set_mask = (1ULL << cfg.num_of_entries_log2) - 1;
        return {
            .row     = inum & 0xF,
            .set_idx = (inum >> 4) & set_mask,
            .tag     = inum >> (4 + cfg.num_of_entries_log2)
        };
    }

    uint64_t inum_to_block(uint64_t inum) const {
        return cfg.inode_array_base + (inum / 16);
    }
    int  select_victim_and_lock(controller_set *cs);

    raven_Inode_t *load_slot_locked(controller_set *cs, int way,
                              uint64_t inum, const InumFields &f);
};

#include "ravenfs/ravenfs.h"

#include <cstdlib>
#include <cstring>

/* 简易私有 buffer cache（TODO: 换 LRU/hash） */
static constexpr int CACHE_SLOTS = 64;

struct CacheSlot {
    uint64_t block = ~0ull;
    uint8_t  data[4096];
    bool     dirty = false;
};

uint8_t* RavenFS::cache_fetch(uint64_t block) {
    /* TODO */
    (void)block;
    return nullptr;
}

void RavenFS::cache_mark_dirty(uint64_t block) {
    (void)block;
}

void RavenFS::cache_flush_block(uint64_t block) {
    (void)block;
}

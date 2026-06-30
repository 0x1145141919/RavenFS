#include "ravenfs/ravenfs.h"

bool RavenFS::inode_load(uint64_t inum, raven_Inode_t* inode) {
    if (!sb_) return false;
    uint64_t block = sb_->inode_array_base + (inum * 256) / 4096;
    uint64_t off   = (inum * 256) % 4096;
    uint8_t buf[4096];
    if (!read_block(block, buf)) return false;
    std::memcpy(inode, buf + off, sizeof(raven_Inode_t));
    return true;
}

bool RavenFS::inode_store(uint64_t inum, const raven_Inode_t* inode) {
    if (!sb_) return false;
    uint64_t block = sb_->inode_array_base + (inum * 256) / 4096;
    uint64_t off   = (inum * 256) % 4096;
    uint8_t* buf = cache_fetch(block);
    if (!buf) return false;
    std::memcpy(buf + off, inode, sizeof(raven_Inode_t));
    cache_mark_dirty(block);
    return true;
}

bool RavenFS::inode_alloc(uint64_t* inum) {
    (void)inum;
    /* TODO */
    return false;
}

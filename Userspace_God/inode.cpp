#include "../include/Userspace_God.hpp"
#include <cstring>

void RavenFS::inode_read(uint64_t inum, raven_Inode_t *inode) {
    uint64_t byte_in_array = inum * sizeof(raven_Inode_t);
    uint64_t abs_fblock    = geo.inode_array_base + byte_in_array / 4096;
    uint64_t byte_in_block = byte_in_array % 4096;

    uint8_t buf[4096];
    inode_cache_load(abs_fblock, buf);
    memcpy(inode, buf + byte_in_block, sizeof(raven_Inode_t));
}

void RavenFS::inode_write(uint64_t inum, const raven_Inode_t *inode) {
    uint64_t byte_in_array = inum * sizeof(raven_Inode_t);
    uint64_t abs_fblock    = geo.inode_array_base + byte_in_array / 4096;
    uint64_t byte_in_block = byte_in_array % 4096;

    uint8_t buf[4096];
    inode_cache_load(abs_fblock, buf);
    memcpy(buf + byte_in_block, inode, sizeof(raven_Inode_t));
    inode_cache_commit(abs_fblock, buf);
}

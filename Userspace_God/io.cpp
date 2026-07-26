#include "../include/ravenfs/ravenfs.hpp"
#include <unistd.h>
#include <cstring>

RavenFS::RavenFS(const char *device_path, uint64_t *res_code) {
    if (res_code)
        *res_code = init_mount(device_path);
}

RavenFS::~RavenFS() { unmount(); }

uint64_t RavenFS::fblock_to_byte(uint64_t fblock) const {
    return 4096 + fblock * 4096;
}

void RavenFS::read_raw(uint64_t byte_off, void *buf, size_t len) {
    ::pread(bdev.fd, buf, len, byte_off);
}

void RavenFS::write_raw(uint64_t byte_off, const void *buf, size_t len) {
    ::pwrite(bdev.fd, buf, len, byte_off);
}

void RavenFS::read_block(uint64_t fblock, void *buf) {
    if (block_cache) {
        int idx = fblock % BLOCK_CACHE_ENTRIES;
        auto &e = block_cache[idx];
        if (e.valid && e.block == fblock) {
            memcpy(buf, e.data, 4096);
            return;
        }
    }
    read_raw(fblock_to_byte(fblock), buf, 4096);
    if (block_cache) {
        int idx = fblock % BLOCK_CACHE_ENTRIES;
        auto &e = block_cache[idx];
        e.block = fblock;
        e.valid = true;
        memcpy(e.data, buf, 4096);
    }
}

void RavenFS::write_block(uint64_t fblock, const void *buf) {
    write_raw(fblock_to_byte(fblock), buf, 4096);
    if (block_cache) {
        int idx = fblock % BLOCK_CACHE_ENTRIES;
        auto &e = block_cache[idx];
        e.block = fblock;
        e.valid = true;
        memcpy(e.data, buf, 4096);
    }
}

void RavenFS::read_btnode(uint64_t fblock, void *node, size_t node_size) {
    uint8_t *p = (uint8_t *)node;
    for (size_t off = 0; off < node_size; off += 4096)
        read_block(fblock + off / 4096, p + off);
}

void RavenFS::write_btnode(uint64_t fblock, const void *node, size_t node_size) {
    const uint8_t *p = (const uint8_t *)node;
    for (size_t off = 0; off < node_size; off += 4096)
        write_block(fblock + off / 4096, p + off);
}

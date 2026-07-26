#pragma once

#include <cstdint>
#include <unistd.h>

struct BlockDevice {
    int      fd;
    uint64_t lba_size;
    uint64_t multiplier;

    void read_block(uint64_t fblock, void *buf) const {
        ::pread(fd, buf, 4096, 4096 + fblock * 4096);
    }
    void write_block(uint64_t fblock, const void *buf) const {
        ::pwrite(fd, buf, 4096, 4096 + fblock * 4096);
    }
};

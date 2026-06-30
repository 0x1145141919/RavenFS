#pragma once

#include <cstdint>

/* ═══════════════════════════════════════════════════════
 * I/O 后端抽象 — 三端各实现此接口
 *
 * host:       pread/pwrite loop 文件或分区
 * sparrowos:  kmod 函数指针 → NVMe 读写
 * linux:      bio_alloc + submit_bio
 * ═══════════════════════════════════════════════════════ */
class RavenIOBackend {
public:
    virtual ~RavenIOBackend() = default;

    /* 块大小 = 4096，所有读写以 block 为单位 */
    static constexpr uint64_t BLOCK_SIZE = 4096;

    /* 读 blocks[block_start, block_start + count) 到 buf */
    virtual bool read_blocks(void* buf, uint64_t block_start, uint64_t count) = 0;

    /* 从 buf 写到 blocks[block_start, block_start + count) */
    virtual bool write_blocks(const void* buf, uint64_t block_start, uint64_t count) = 0;

    /* 总块数（分区大小 / BLOCK_SIZE） */
    virtual uint64_t total_blocks() const = 0;

    /* 同步 flush。host 端可为空，sparrowos/linux 视需要实现写回 */
    virtual void flush() {}
};

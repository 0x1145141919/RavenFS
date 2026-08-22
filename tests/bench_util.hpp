/* ═══════════════════════════════════════════════════════════════════
 * bench_util.hpp — 真盘 benchmark 公共设施
 *
 * 按 block_model.md 链式推导法则：读 Primary Superblock（fd byte[0,4096)），
 * total_blocks + inode_array_count 零冗余推导全部几何；
 * FS_block[n] fd 偏移 = 4096 + n×4096。
 * 供 benchmark_radix.cpp / benchmark_session_vs_nvme.cpp 共用。
 * ═══════════════════════════════════════════════════════════════════ */
#pragma once
#include "ravenfs/inode_cache_radix.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

static constexpr uint64_t SUPERBLOCK_MAGIC = 0x5241564EULL;   /* "RAVN" */
static constexpr uint32_t RAVENFS_VERSION  = 2;               /* v1.1 */

/* ── CRC32（与 mkfs 同算法：crc32 字段置 0 后全结构计算） ── */
inline uint32_t crc32_table[256];
inline bool crc32_ready = false;
inline void crc32_init() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        crc32_table[i] = crc;
    }
    crc32_ready = true;
}
inline uint32_t crc32_compute(const void *data, size_t len) {
    if (!crc32_ready) crc32_init();
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *buf = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

/* ── 设备 I/O 回调（O_DIRECT 直通裸盘，4K 对齐） ── */
inline int g_fd = -1;
inline uint64_t g_reads = 0, g_writes = 0, g_read_bytes = 0, g_write_bytes = 0;

inline result_code_t rd(void *, uint64_t blk, void *buf) {
    g_reads++; g_read_bytes += 4096;
    ssize_t r = pread(g_fd, buf, 4096, (off_t)4096 + (off_t)blk * 4096);
    return (r == 4096) ? RC_OK : RC_ERR_IO;
}
inline result_code_t wr(void *, uint64_t blk, const void *buf) {
    g_writes++; g_write_bytes += 4096;
    ssize_t r = pwrite(g_fd, buf, 4096, (off_t)4096 + (off_t)blk * 4096);
    return (r == 4096) ? RC_OK : RC_ERR_IO;
}
inline void *al(void *, uint64_t sz) { return aligned_alloc(4096, (sz + 4095) & ~4095ull); }
inline void fr(void *, void *p) { free(p); }

/* 直读盘（不经缓存） */
inline void disk_read_blk(uint64_t blk, void *buf) {
    ssize_t r = pread(g_fd, buf, 4096, (off_t)4096 + (off_t)blk * 4096);
    assert(r == 4096);
}

/* ── 链式推导几何（block_model.md §3.2，零冗余） ── */
struct FsGeometry {
    uint64_t total_blocks, inodes;
    uint64_t bitmap_blocks, bitmap_start;
    uint64_t inode_blocks, inode_array_base;
    uint64_t ibm_blocks, ia_bm_base;
};
inline FsGeometry derive_geometry(uint64_t total_blocks, uint64_t inodes) {
    FsGeometry g{};
    g.total_blocks = total_blocks;
    g.inodes       = inodes;
    g.bitmap_blocks = (total_blocks + 32767) / 32768;
    g.bitmap_start  = total_blocks - g.bitmap_blocks;
    g.inode_blocks  = (inodes * 256 + 4095) / 4096;
    g.inode_array_base = g.bitmap_start - g.inode_blocks;
    g.ibm_blocks    = (inodes + 32767) / 32768;
    g.ia_bm_base    = g.inode_array_base - g.ibm_blocks;
    assert(g.bitmap_start + g.bitmap_blocks == total_blocks);
    assert(g.inode_array_base + g.inode_blocks == g.bitmap_start);
    assert(g.ia_bm_base + g.ibm_blocks == g.inode_array_base);
    return g;
}

/* 打开设备 + 读 Primary Superblock + 校验 + 推导几何 */
inline RavenSuperblock open_ravenfs(const char *dev, FsGeometry *geo_out) {
    g_fd = open(dev, O_RDWR | O_DIRECT);
    assert(g_fd >= 0);
    RavenSuperblock sb;
    assert(pread(g_fd, &sb, sizeof(sb), 0) == (ssize_t)sizeof(sb));
    assert(sb.magic == SUPERBLOCK_MAGIC);
    assert(sb.version == RAVENFS_VERSION);
    const uint32_t crc_stored = sb.crc32;
    sb.crc32 = 0;
    assert(crc32_compute(&sb, sizeof(sb)) == crc_stored);
    *geo_out = derive_geometry(sb.total_blocks, sb.inode_array_count);
    return sb;
}

#include "../include/Userspace_God.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ═══════════════════════════════════════════════════════════════
 * cache_load — 装载所有缓存
 *
 * 1. 全量读取 bitmap → bitmap_cache
 * 2. 全量读取 ibitmap → ibitmap_cache
 * 3. 重置 inode cache（所有行标记 invalid）
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS::cache_load() {
    /* ─── Bitmap 全量 ─── */
    size_t bm_bytes = geo.bitmap_blocks * 4096;
    bitmap_cache = (uint8_t *)malloc(bm_bytes);
    if (bitmap_cache) {
        for (uint64_t i = 0; i < geo.bitmap_blocks; i++)
            read_block(geo.bitmap_start + i, bitmap_cache + i * 4096);
        bitmap_dirty = false;
        fprintf(stderr, "RavenFS: bitmap cache loaded  %lu blocks (%lu KB)\n",
                (unsigned long)geo.bitmap_blocks,
                (unsigned long)(bm_bytes / 1024));
    } else {
        fprintf(stderr, "RavenFS: WARNING bitmap cache OOM (%lu KB), fallback to direct I/O\n",
                (unsigned long)(bm_bytes / 1024));
    }

    /* ─── Inode Bitmap 全量 ─── */
    size_t ibm_bytes = geo.inode_bitmap_blocks * 4096;
    ibitmap_cache = (uint8_t *)malloc(ibm_bytes);
    if (ibitmap_cache) {
        for (uint64_t i = 0; i < geo.inode_bitmap_blocks; i++)
            read_block(geo.inode_array_bitmap_base + i, ibitmap_cache + i * 4096);
        ibitmap_dirty = false;
        fprintf(stderr, "RavenFS: ibitmap cache loaded  %lu blocks (%lu KB)\n",
                (unsigned long)geo.inode_bitmap_blocks,
                (unsigned long)(ibm_bytes / 1024));
    } else {
        fprintf(stderr, "RavenFS: WARNING ibitmap cache OOM (%lu KB), fallback to direct I/O\n",
                (unsigned long)(ibm_bytes / 1024));
    }

    /* ─── 通用 block cache（16MB 直映射） ─── */
    block_cache = (decltype(block_cache))calloc(BLOCK_CACHE_ENTRIES, sizeof(*block_cache));
    if (!block_cache)
        fprintf(stderr, "RavenFS: WARNING block cache OOM (%d MB), fallback to direct I/O\n",
                (int)(BLOCK_CACHE_ENTRIES * 4096 / 1024 / 1024));

    /* ─── Inode Table 缓存初始化（堆分配 8MB） ─── */
    size_t ic_sz = INODE_CACHE_SETS * INODE_CACHE_WAYS * sizeof(InodeCacheLine);
    inode_cache = (InodeCacheLine *)malloc(ic_sz);
    if (!inode_cache) {
        fprintf(stderr, "RavenFS: WARNING inode cache OOM (%zu MB), fallback to direct I/O\n",
                ic_sz / (1024 * 1024));
    } else {
        memset(inode_cache, 0, ic_sz);
    }

    inode_evict_next = (uint8_t *)calloc(INODE_CACHE_SETS, 1);
    if (!inode_evict_next) {
        fprintf(stderr, "RavenFS: WARNING inode evict table OOM\n");
    }
}

/* ═══════════════════════════════════════════════════════════════
 * cache_flush — 回写所有脏数据
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS::cache_flush() {
    /* ─── Bitmap 回写 ─── */
    if (bitmap_cache && bitmap_dirty) {
        for (uint64_t i = 0; i < geo.bitmap_blocks; i++)
            write_block(geo.bitmap_start + i, bitmap_cache + i * 4096);
        bitmap_dirty = false;
        fprintf(stderr, "RavenFS: bitmap cache flushed  %lu blocks\n",
                (unsigned long)geo.bitmap_blocks);
    }

    /* ─── Inode Bitmap 回写 ─── */
    if (ibitmap_cache && ibitmap_dirty) {
        for (uint64_t i = 0; i < geo.inode_bitmap_blocks; i++)
            write_block(geo.inode_array_bitmap_base + i, ibitmap_cache + i * 4096);
        ibitmap_dirty = false;
        fprintf(stderr, "RavenFS: ibitmap cache flushed  %lu blocks\n",
                (unsigned long)geo.inode_bitmap_blocks);
    }

    /* ─── Inode Table 脏行回写 ─── */
    if (inode_cache) {
        int flushed = 0;
        for (int s = 0; s < INODE_CACHE_SETS; s++) {
            for (int w = 0; w < INODE_CACHE_WAYS; w++) {
                auto &line = inode_cache[s * INODE_CACHE_WAYS + w];
                if (line.valid && line.dirty) {
                    write_block(line.tag * INODE_CACHE_SETS + s, line.data);
                    line.dirty = false;
                    flushed++;
                }
            }
        }
        if (flushed)
            fprintf(stderr, "RavenFS: inode cache flushed  %d lines\n", flushed);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * inode_cache_load — 将 abs_fblock 加载到缓存并拷贝到 buf
 *
 * 缓存命中：直接拷贝
 * 缓存未命中：淘汰一行、若脏则回写、读盘、拷贝
 * (inode_cache 为 NULL 时退化为直接读盘)
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS::inode_cache_load(uint64_t abs_fblock, uint8_t *buf) {
    if (!inode_cache) {
        read_block(abs_fblock, buf);
        return;
    }

    uint64_t set = abs_fblock % INODE_CACHE_SETS;
    uint64_t tag = abs_fblock / INODE_CACHE_SETS;
    InodeCacheLine *set_lines = inode_cache + set * INODE_CACHE_WAYS;

    for (int w = 0; w < INODE_CACHE_WAYS; w++) {
        auto &line = set_lines[w];
        if (line.valid && line.tag == tag) {
            memcpy(buf, line.data, 4096);
            return;
        }
    }

    int victim = inode_evict_next[set];
    inode_evict_next[set] = (victim + 1) % INODE_CACHE_WAYS;

    auto &line = set_lines[victim];
    if (line.valid && line.dirty) {
        write_block(line.tag * INODE_CACHE_SETS + set, line.data);
    }

    read_block(abs_fblock, line.data);
    line.tag   = tag;
    line.valid = true;
    line.dirty = false;
    memcpy(buf, line.data, 4096);
}

/* ═══════════════════════════════════════════════════════════════
 * inode_cache_commit — 将 buf 提交到缓存（标记脏）
 *
 * (inode_cache 为 NULL 或未命中时退化为直接写盘)
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS::inode_cache_commit(uint64_t abs_fblock, const uint8_t *buf) {
    if (!inode_cache) {
        write_block(abs_fblock, buf);
        return;
    }

    uint64_t set = abs_fblock % INODE_CACHE_SETS;
    uint64_t tag = abs_fblock / INODE_CACHE_SETS;
    InodeCacheLine *set_lines = inode_cache + set * INODE_CACHE_WAYS;

    for (int w = 0; w < INODE_CACHE_WAYS; w++) {
        auto &line = set_lines[w];
        if (line.valid && line.tag == tag) {
            memcpy(line.data, buf, 4096);
            line.dirty = true;
            return;
        }
    }

    write_block(abs_fblock, buf);
}

#include "../include/Userspace_God.hpp"
#include <cstdio>

static void bitmap_location(uint64_t bitmap_start, uint64_t fblock,
                            uint64_t *abs_block, uint64_t *byte_off, uint64_t *bit_off)
{
    uint64_t bm_idx    = fblock / 32768;
    uint64_t byte_idx  = (fblock % 32768) / 8;
    *abs_block = bitmap_start + bm_idx;
    *byte_off  = byte_idx;
    *bit_off   = fblock % 8;
}

bool RavenFS::bitmap_test(uint64_t fblock) {
    uint64_t abs_block, byte_off, bit_off;
    bitmap_location(geo.bitmap_start, fblock, &abs_block, &byte_off, &bit_off);

    if (bitmap_cache) {
        uint64_t idx = (abs_block - geo.bitmap_start) * 4096 + byte_off;
        return (bitmap_cache[idx] >> bit_off) & 1;
    }

    uint8_t buf[4096];
    read_block(abs_block, buf);
    return (buf[byte_off] >> bit_off) & 1;
}

void RavenFS::bitmap_set(uint64_t fblock) {
    uint64_t abs_block, byte_off, bit_off;
    bitmap_location(geo.bitmap_start, fblock, &abs_block, &byte_off, &bit_off);

    if (bitmap_cache) {
        uint64_t idx = (abs_block - geo.bitmap_start) * 4096 + byte_off;
        bitmap_cache[idx] |= (uint8_t)(1 << bit_off);
        bitmap_dirty = true;
        return;
    }

    uint8_t buf[4096];
    read_block(abs_block, buf);
    buf[byte_off] |= (uint8_t)(1 << bit_off);
    write_block(abs_block, buf);
}

void RavenFS::bitmap_clear(uint64_t fblock) {
    uint64_t abs_block, byte_off, bit_off;
    bitmap_location(geo.bitmap_start, fblock, &abs_block, &byte_off, &bit_off);

    if (bitmap_cache) {
        uint64_t idx = (abs_block - geo.bitmap_start) * 4096 + byte_off;
        bitmap_cache[idx] &= (uint8_t)~(1 << bit_off);
        bitmap_dirty = true;
        return;
    }

    uint8_t buf[4096];
    read_block(abs_block, buf);
    buf[byte_off] &= (uint8_t)~(1 << bit_off);
    write_block(abs_block, buf);
}

uint64_t RavenFS::bitmap_alloc_block() {
    return bitmap_alloc_blocks(1);
}

uint64_t RavenFS::bitmap_alloc_blocks(uint64_t count) {
    for (uint64_t i = 0; i <= geo.total_blocks - count; i++) {
        bool ok = true;
        for (uint64_t j = 0; j < count; j++) {
            if (bitmap_test(i + j)) { ok = false; break; }
        }
        if (!ok) continue;
        for (uint64_t j = 0; j < count; j++)
            bitmap_set(i + j);
        return i;
    }
    fprintf(stderr, "RavenFS: no %lu consecutive free FS_blocks (total=%lu)\n",
            (unsigned long)count, (unsigned long)geo.total_blocks);
    return (uint64_t)-1;
}

/* ═══════════════════════════════════════════════════════════════
 * Inode Bitmap — 走 ibitmap_cache
 * ═══════════════════════════════════════════════════════════════ */

bool RavenFS::ibitmap_test(uint64_t inum) {
    uint64_t idx = inum / 32768;
    uint64_t bo  = (inum % 32768) / 8;
    uint64_t bi  = inum % 8;

    if (ibitmap_cache)
        return (ibitmap_cache[idx * 4096 + bo] >> bi) & 1;

    uint64_t abs_block = geo.inode_array_bitmap_base + idx;
    uint8_t buf[4096];
    read_block(abs_block, buf);
    return (buf[bo] >> bi) & 1;
}

void RavenFS::ibitmap_set(uint64_t inum) {
    uint64_t idx = inum / 32768;
    uint64_t bo  = (inum % 32768) / 8;
    uint64_t bi  = inum % 8;

    if (ibitmap_cache) {
        ibitmap_cache[idx * 4096 + bo] |= (uint8_t)(1 << bi);
        ibitmap_dirty = true;
        return;
    }

    uint64_t abs_block = geo.inode_array_bitmap_base + idx;
    uint8_t buf[4096];
    read_block(abs_block, buf);
    buf[bo] |= (uint8_t)(1 << bi);
    write_block(abs_block, buf);
}

void RavenFS::ibitmap_clear(uint64_t inum) {
    uint64_t idx = inum / 32768;
    uint64_t bo  = (inum % 32768) / 8;
    uint64_t bi  = inum % 8;

    if (ibitmap_cache) {
        ibitmap_cache[idx * 4096 + bo] &= (uint8_t)~(1 << bi);
        ibitmap_dirty = true;
        return;
    }

    uint64_t abs_block = geo.inode_array_bitmap_base + idx;
    uint8_t buf[4096];
    read_block(abs_block, buf);
    buf[bo] &= (uint8_t)~(1 << bi);
    write_block(abs_block, buf);
}

uint64_t RavenFS::ibitmap_alloc_inode() {
    for (uint64_t i = 1; i < sb.inode_array_count; i++) {
        if (!ibitmap_test(i))
            return i;
    }
    fprintf(stderr, "RavenFS: no free inode (max=%lu)\n",
            (unsigned long)sb.inode_array_count);
    return (uint64_t)-1;
}

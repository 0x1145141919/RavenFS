#include "../include/ravenfs/ravenfs.hpp"

static void bitmap_location(uint64_t bitmap_start, uint64_t fblock,
                            uint64_t *abs_block, uint64_t *byte_off, uint64_t *bit_off)
{
    uint64_t bm_idx    = fblock / 32768;
    uint64_t byte_idx  = (fblock % 32768) / 8;
    *abs_block = bitmap_start + bm_idx;
    *byte_off  = byte_idx;
    *bit_off   = fblock % 8;
}

/* validate 专用：直读不缓存 */
bool RavenFS::bitmap_test(uint64_t fblock) {
    uint64_t abs_block, byte_off, bit_off;
    bitmap_location(geo.bitmap_start, fblock, &abs_block, &byte_off, &bit_off);
    uint8_t buf[4096];
    read_block(abs_block, buf);
    return (buf[byte_off] >> bit_off) & 1;
}

/* validate 专用：直读不缓存 */
bool RavenFS::ibitmap_test(uint64_t inum) {
    uint64_t idx = inum / 32768;
    uint64_t bo  = (inum % 32768) / 8;
    uint64_t bi  = inum % 8;
    uint8_t buf[4096];
    read_block(geo.inode_array_bitmap_base + idx, buf);
    return (buf[bo] >> bi) & 1;
}

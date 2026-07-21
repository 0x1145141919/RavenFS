#include "../include/Userspace_God.hpp"
#include <cstdio>

uint64_t RavenFS::validate_world_base() {
    if (world_base_lba * lba_size != 4096) {
        fprintf(stderr, "RavenFS: world_base_lba=%lu  lba_size=%lu  "
                        "product=%lu, expected 4096\n",
                (unsigned long)world_base_lba, (unsigned long)lba_size,
                (unsigned long)(world_base_lba * lba_size));
        return RAVENFS_ERR_WORLD_BASE;
    }
    return RAVENFS_OK;
}

uint64_t RavenFS::validate_sb_crc32() {
    uint32_t stored_crc = sb.crc32;
    sb.crc32 = 0;
    uint32_t computed = crc32_compute(&sb, sizeof(sb));
    sb.crc32 = stored_crc;

    if (stored_crc != computed) {
        fprintf(stderr, "RavenFS: CRC32 mismatch  stored=0x%08X  computed=0x%08X\n",
                stored_crc, computed);
        return RAVENFS_ERR_CRC;
    }
    return RAVENFS_OK;
}

uint64_t RavenFS::validate_backup_chain() {
    if (sb.backup_count == 0)
        return RAVENFS_OK;

    if (sb.backup_prev != ~0ULL) {
        fprintf(stderr, "RavenFS: Primary backup_prev=%lu, expected ~0ull\n",
                (unsigned long)sb.backup_prev);
        return RAVENFS_ERR_BACKUP;
    }

    if (sb.backup_next == ~0ULL) {
        fprintf(stderr, "RavenFS: backup_count=%u but backup_next=~0ull (no chain head)\n",
                (unsigned)sb.backup_count);
        return RAVENFS_ERR_BACKUP;
    }

    uint64_t cur = sb.backup_next;
    uint32_t count = 0;

    while (cur != ~0ULL) {
        RavenSuperblock bk;
        read_block(cur, &bk);
        count++;

        if (bk.magic != SUPERBLOCK_MAGIC) {
            fprintf(stderr, "RavenFS: Backup[%u] at block %lu: bad magic 0x%016lX\n",
                    count, (unsigned long)cur, (unsigned long)bk.magic);
            return RAVENFS_ERR_BACKUP;
        }

        uint32_t bk_crc = bk.crc32;
        bk.crc32 = 0;
        uint32_t bk_computed = crc32_compute(&bk, sizeof(bk));
        bk.crc32 = bk_crc;
        if (bk_crc != bk_computed) {
            fprintf(stderr, "RavenFS: Backup[%u] at block %lu: CRC32 mismatch\n",
                    count, (unsigned long)cur);
            return RAVENFS_ERR_BACKUP;
        }

        if (bk.total_blocks     != sb.total_blocks   ||
            bk.inode_array_count != sb.inode_array_count ||
            bk.root_inode       != sb.root_inode     ||
            bk.version          != sb.version        ||
            bk.backup_count     != sb.backup_count)
        {
            fprintf(stderr, "RavenFS: Backup[%u] at block %lu: common fields mismatch\n",
                    count, (unsigned long)cur);
            return RAVENFS_ERR_BACKUP;
        }

        cur = bk.backup_next;
    }

    if (count != sb.backup_count) {
        fprintf(stderr, "RavenFS: Backup count mismatch: traversed %u, expected %u\n",
                count, (unsigned)sb.backup_count);
        return RAVENFS_ERR_BACKUP;
    }

    fprintf(stderr, "RavenFS: backup chain OK  count=%u  head block=%lu\n",
            count, (unsigned long)sb.backup_next);
    return RAVENFS_OK;
}

uint64_t RavenFS::validate_bitmap_self_consistency() {
    auto check_range = [&](uint64_t start, uint64_t end, const char *label) -> bool {
        for (uint64_t b = start; b < end; b++) {
            if (!bitmap_test(b)) {
                fprintf(stderr, "RavenFS: bit[%lu] in %s [%lu,%lu) is 0, expected 1\n",
                        (unsigned long)b, label,
                        (unsigned long)start, (unsigned long)end);
                return false;
            }
        }
        return true;
    };

    if (!check_range(geo.bitmap_start, geo.total_blocks, "bitmap"))
        return RAVENFS_ERR_BITMAP;

    if (!check_range(geo.inode_array_base, geo.bitmap_start, "inode array"))
        return RAVENFS_ERR_BITMAP;

    if (!check_range(geo.inode_array_bitmap_base, geo.inode_array_base, "inode bitmap"))
        return RAVENFS_ERR_BITMAP;

    {
        uint64_t total_bits   = geo.bitmap_blocks * 32768;
        for (uint64_t bit = geo.total_blocks; bit < total_bits; bit++) {
            if (!bitmap_test(bit)) {
                fprintf(stderr, "RavenFS: bitmap tail bit[%lu] is 0, expected 1\n",
                        (unsigned long)bit);
                return RAVENFS_ERR_BITMAP;
            }
        }
    }

    {
        uint64_t total_ibits  = geo.inode_bitmap_blocks * 32768;
        for (uint64_t bit = sb.inode_array_count; bit < total_ibits; bit++) {
            if (!ibitmap_test(bit)) {
                fprintf(stderr, "RavenFS: inode bitmap tail bit[%lu] is 0, expected 1\n",
                        (unsigned long)bit);
                return RAVENFS_ERR_BITMAP;
            }
        }
    }

    return RAVENFS_OK;
}

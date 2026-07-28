#include "../include/ravenfs/ravenfs.hpp"
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

uint64_t RavenFS::init_mount(const char *device_path) {
    if (bdev.fd >= 0) {
        fprintf(stderr, "RavenFS: already mounted\n");
        return RAVENFS_ERR_OPEN;
    }

    bdev.fd = open(device_path, O_RDWR);
    if (bdev.fd < 0) {
        perror("RavenFS: open");
        return RAVENFS_ERR_OPEN;
    }

    struct stat st;
    bool is_regular_file = false;
    if (fstat(bdev.fd, &st) == 0 && S_ISREG(st.st_mode))
        is_regular_file = true;

    if (is_regular_file) {
        bdev.lba_size = 4096;
    } else {
        int sz = 0;
        if (ioctl(bdev.fd, BLKSSZGET, &sz) != 0 || sz <= 0) {
            fprintf(stderr, "RavenFS: BLKSSZGET failed, assuming 512\n");
            bdev.lba_size = 512;
        } else {
            bdev.lba_size = (uint64_t)sz;
        }
    }

    if (4096 % bdev.lba_size != 0) {
        fprintf(stderr, "RavenFS: LBA_size %lu does not divide 4096\n",
                (unsigned long)bdev.lba_size);
        close(bdev.fd); bdev.fd = -1;
        return RAVENFS_ERR_LBA;
    }

    bdev.multiplier = 4096 / bdev.lba_size;

    read_raw(0, &sb, sizeof(sb));

    if (sb.magic != SUPERBLOCK_MAGIC) {
        fprintf(stderr, "RavenFS: no RAVN magic at offset 0 (got 0x%016lX)\n",
                (unsigned long)sb.magic);
        close(bdev.fd); bdev.fd = -1;
        return RAVENFS_ERR_NOMAGIC;
    }

    if (sb.version != 1) {
        fprintf(stderr, "RavenFS: unsupported version %u\n", sb.version);
        close(bdev.fd); bdev.fd = -1;
        return RAVENFS_ERR_VERSION;
    }

    root_inum      = sb.root_inode;

    uint64_t bb = (sb.total_blocks + 32767) / 32768;
    geo.bitmap_blocks = bb;
    geo.bitmap_start  = sb.total_blocks - bb;

    uint64_t ib = (sb.inode_array_count * 256 + 4095) / 4096;
    geo.inode_blocks = ib;

    uint64_t ibb = (sb.inode_array_count + 32767) / 32768;
    geo.inode_bitmap_blocks = ibb;

    geo.inode_array_base         = geo.bitmap_start - ib;
    geo.inode_array_bitmap_base  = geo.inode_array_base - ibb;
    geo.total_blocks             = sb.total_blocks;

    block_cache = (decltype(block_cache))calloc(BLOCK_CACHE_ENTRIES, sizeof(*block_cache));
    if (!block_cache)
        fprintf(stderr, "RavenFS: WARNING block cache OOM (%d MB), fallback to direct I/O\n",
                (int)(BLOCK_CACHE_ENTRIES * 4096 / 1024 / 1024));

    {
        uint64_t err;
        err = validate_world_base();
        if (err) { unmount(); return err; }

        err = validate_sb_crc32();
        if (err) { unmount(); return err; }

        err = validate_backup_chain();
        if (err) { unmount(); return err; }

        err = validate_bitmap_self_consistency();
        if (err) { unmount(); return err; }
    }

    /* ── 5. v2 Inode Cache ── */
    cache_entries = new controller_set[INODE_CACHE_SETS]();
    cache_data    = new Inode_cache_line[INODE_CACHE_SETS * WAYS_PER_SET]();

    {
        InodeCacheConfig ic_cfg;
        ic_cfg.dev                 = &bdev;
        ic_cfg.inode_array_base    = geo.inode_array_base;
        ic_cfg.inode_array_count   = sb.inode_array_count;
        ic_cfg.all_entries         = cache_entries;
        ic_cfg.cache_lines         = cache_data;
        ic_cfg.num_of_entries_log2 = 8;
        icache = new Inode_cache_controller(ic_cfg);
    }

    /* ── 6. Bitmap Cache Controller ── */
    {
        long nproc = sysconf(_SC_NPROCESSORS_ONLN);
        if (nproc < 1) nproc = 1;

        BitmapCacheConfig bm_cfg;
        bm_cfg.dev                          = &bdev;
        bm_cfg.bitmap_base                  = geo.bitmap_start;
        bm_cfg.bitmap_block_count           = geo.bitmap_blocks;
        bm_cfg.processor_max                = (uint16_t)nproc;
        bm_cfg.num_of_blocks_per_bucket_log2 = 3;
        bcache = new BitmapCacheController(bm_cfg);
    }

    /* ── 7. Inode Bitmap Cache Controller ── */
    {
        InodeBitmapCacheConfig ib_cfg;
        ib_cfg.dev                       = &bdev;
        ib_cfg.bitmap_base               = geo.inode_array_bitmap_base;
        ib_cfg.bitmap_block_count        = geo.inode_bitmap_blocks;
        ib_cfg.inode_array_count         = sb.inode_array_count;
        ib_cfg.num_of_blocks_per_bucket_log2 = 3;
        ibcache = new InodeBitmapCacheController(ib_cfg);
    }

    /* ── 8. VFS 逻辑缓存层 ── */
    vfs_cache_init();

    fprintf(stderr, "RavenFS: mounted  blocks=%lu  inodes=%lu  root=%lu\n",
            (unsigned long)sb.total_blocks,
            (unsigned long)sb.inode_array_count,
            (unsigned long)root_inum);
    return RAVENFS_OK;
}

void RavenFS::unmount() {
    if (bdev.fd >= 0) {
        vfs_cache_destroy();

        if (bcache) {
            bcache->shutdown_all();
            delete bcache;
            bcache = nullptr;
        }

        if (ibcache) {
            ibcache->shutdown_all();
            delete ibcache;
            ibcache = nullptr;
        }

        if (icache) {
            icache->flush_all();
            delete icache;
            icache = nullptr;
        }
        delete[] cache_data;    cache_data    = nullptr;
        delete[] cache_entries; cache_entries = nullptr;

        free(block_cache);     block_cache     = nullptr;

        close(bdev.fd);
        bdev.fd = -1;
        fprintf(stderr, "RavenFS: unmounted\n");
    }
}

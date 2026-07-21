#include "../include/Userspace_God.hpp"
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

uint64_t RavenFS::init_mount(const char *device_path) {
    if (dev_fd >= 0) {
        fprintf(stderr, "RavenFS: already mounted\n");
        return RAVENFS_ERR_OPEN;
    }

    dev_fd = open(device_path, O_RDWR);
    if (dev_fd < 0) {
        perror("RavenFS: open");
        return RAVENFS_ERR_OPEN;
    }

    struct stat st;
    bool is_regular_file = false;
    if (fstat(dev_fd, &st) == 0 && S_ISREG(st.st_mode))
        is_regular_file = true;

    if (is_regular_file) {
        lba_size = 4096;
    } else {
        int sz = 0;
        if (ioctl(dev_fd, BLKSSZGET, &sz) != 0 || sz <= 0) {
            fprintf(stderr, "RavenFS: BLKSSZGET failed, assuming 512\n");
            lba_size = 512;
        } else {
            lba_size = (uint64_t)sz;
        }
    }

    if (4096 % lba_size != 0) {
        fprintf(stderr, "RavenFS: LBA_size %lu does not divide 4096\n",
                (unsigned long)lba_size);
        close(dev_fd); dev_fd = -1;
        return RAVENFS_ERR_LBA;
    }

    multiplier = 4096 / lba_size;

    read_raw(0, &sb, sizeof(sb));

    if (sb.magic != SUPERBLOCK_MAGIC) {
        fprintf(stderr, "RavenFS: no RAVN magic at offset 0 (got 0x%016lX)\n",
                (unsigned long)sb.magic);
        close(dev_fd); dev_fd = -1;
        return RAVENFS_ERR_NOMAGIC;
    }

    if (sb.version != 1) {
        fprintf(stderr, "RavenFS: unsupported version %u\n", sb.version);
        close(dev_fd); dev_fd = -1;
        return RAVENFS_ERR_VERSION;
    }

    world_base_lba = sb.world_base_lba;
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

    cache_load();

    {
        uint64_t err;
        err = validate_world_base();
        if (err) { close(dev_fd); dev_fd = -1; return err; }

        err = validate_sb_crc32();
        if (err) { close(dev_fd); dev_fd = -1; return err; }

        err = validate_backup_chain();
        if (err) { close(dev_fd); dev_fd = -1; return err; }

        err = validate_bitmap_self_consistency();
        if (err) { close(dev_fd); dev_fd = -1; return err; }
    }

    fprintf(stderr, "RavenFS: mounted  blocks=%lu  inodes=%lu  root=%lu\n",
            (unsigned long)sb.total_blocks,
            (unsigned long)sb.inode_array_count,
            (unsigned long)root_inum);
    return RAVENFS_OK;
}

void RavenFS::unmount() {
    if (dev_fd >= 0) {
        cache_flush();

        free(bitmap_cache);    bitmap_cache    = nullptr;
        free(ibitmap_cache);   ibitmap_cache   = nullptr;
        free(inode_cache);     inode_cache     = nullptr;
        free(inode_evict_next); inode_evict_next = nullptr;
        free(block_cache);     block_cache     = nullptr;

        close(dev_fd);
        dev_fd = -1;
        fprintf(stderr, "RavenFS: unmounted\n");
    }
}

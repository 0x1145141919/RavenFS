#include "mkfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cerrno>

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/fs.h>
#include <linux/hdreg.h>
#include <linux/nvme_ioctl.h>
#include <uuid/uuid.h>

/* ═══════════════════════════════════════════════════════════════
 * FsGeometry
 * ═══════════════════════════════════════════════════════════════ */
FsGeometry calc_geometry(uint64_t total_blocks, uint64_t inode_count) {
    FsGeometry g{};
    g.total_blocks        = total_blocks;

    uint64_t bb = (total_blocks + 32767) / 32768;
    g.bitmap_blocks       = bb;
    g.bitmap_start        = total_blocks - bb;

    uint64_t ib = (inode_count * 256 + 4095) / 4096;
    g.inode_blocks        = ib;
    g.inode_array_base    = g.bitmap_start - ib;

    uint64_t ibb = (inode_count + 32767) / 32768;
    g.inode_bitmap_blocks = ibb;
    g.inode_array_bitmap_base = g.inode_array_base - ibb;

    return g;
}

/* ═══════════════════════════════════════════════════════════════
 * 硬件探针
 * ═══════════════════════════════════════════════════════════════ */
static int try_open_parent(const char *device_path) {
    struct stat st;
    if (stat(device_path, &st) < 0 || !S_ISBLK(st.st_mode))
        return -1;

    char link[256];
    snprintf(link, sizeof(link),
             "/sys/dev/block/%d:%d",
             major(st.st_rdev), minor(st.st_rdev));

    char target[256];
    ssize_t len = readlink(link, target, sizeof(target) - 1);
    if (len < 0) return -1;
    target[len] = '\0';

    const char *devname = strrchr(target, '/');
    if (!devname) return -1;
    devname++;

    char parent[256];
    strncpy(parent, devname, sizeof(parent) - 1);
    char *p = nullptr;

    if (strncmp(parent, "nvme", 4) == 0) {
        p = strrchr(parent, 'p');
        if (p && p[1] >= '0' && p[1] <= '9') *p = '\0';
    } else {
        p = parent + strlen(parent);
        while (p > parent && p[-1] >= '0' && p[-1] <= '9') p--;
        *p = '\0';
    }

    if (strcmp(parent, devname) == 0) return -1;

    char devpath[512];
    snprintf(devpath, sizeof(devpath), "/dev/%s", parent);
    return open(devpath, O_RDONLY);
}

static bool probe_nvme(int fd) {
    // NVME_IOCTL_ID returns nsid (>0) on success, -1 on error
    return ioctl(fd, NVME_IOCTL_ID, 0) > 0;
}

static bool probe_ata(int fd) {
    struct hd_driveid id;
    return ioctl(fd, HDIO_GET_IDENTITY, &id) == 0;
}

DevType probe_hardware(int fd, const char *device_path) {
    if (probe_nvme(fd)) return DEV_BLOCK_NVME;
    if (probe_ata(fd))  return DEV_BLOCK_ATA;

    int parent_fd = try_open_parent(device_path);
    if (parent_fd >= 0) {
        DevType t = DEV_UNKNOWN;
        if (probe_nvme(parent_fd))         t = DEV_BLOCK_NVME;
        else if (probe_ata(parent_fd))     t = DEV_BLOCK_ATA;
        close(parent_fd);
        return t;
    }

    return DEV_UNKNOWN;
}

/* ═══════════════════════════════════════════════════════════════
 * 设备信息
 * ═══════════════════════════════════════════════════════════════ */
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif
#ifndef BLKSSZGET
#define BLKSSZGET  _IO(0x12, 104)
#endif

static bool is_mounted(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISBLK(st.st_mode))
        return false;

    char link_buf[256];
    snprintf(link_buf, sizeof(link_buf),
             "/sys/dev/block/%d:%d",
             major(st.st_rdev), minor(st.st_rdev));
    char real[256];
    ssize_t len = readlink(link_buf, real, sizeof(real) - 1);
    if (len < 0) return false;
    real[len] = '\0';

    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return false;

    char line[1024];
    bool mounted = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned int mj = 0, mn = 0;
        char mp[256] = {};
        int n = sscanf(line,
            "%*d %*d %*u:%*u / %255s %*s %*s %*s %u:%u",
            mp, &mj, &mn);
        if (n >= 3 && (int)major(st.st_rdev) == (int)mj
            && (int)minor(st.st_rdev) == (int)mn) {
            mounted = true;
            break;
        }
    }
    fclose(f);
    return mounted;
}

int get_device_info(const char *path, DevInfo *info) {
    struct stat st;
    if (stat(path, &st) < 0) {
        perror("stat");
        return -1;
    }
    memset(info, 0, sizeof(*info));

    if (S_ISBLK(st.st_mode)) {
        int bfd = open(path, O_RDONLY);
        if (bfd < 0) { perror("open"); return -1; }
        if (ioctl(bfd, BLKGETSIZE64, &info->bytes) < 0) {
            perror("BLKGETSIZE64"); close(bfd); return -1;
        }
        close(bfd);
        info->is_mounted = is_mounted(path);
        info->type = DEV_UNKNOWN;
        return 0;
    }

    if (S_ISREG(st.st_mode)) {
        info->type  = DEV_REGULAR;
        info->bytes = (uint64_t)st.st_size;
        return 0;
    }

    return -1;
}

/* ═══════════════════════════════════════════════════════════════
 * 分区探测（sysfs）
 * ═══════════════════════════════════════════════════════════════ */
uint64_t detect_partition_start(const char *device_path) {
    struct stat st;
    if (stat(device_path, &st) < 0 || !S_ISBLK(st.st_mode))
        return 0;

    char path[512];
    snprintf(path, sizeof(path),
             "/sys/dev/block/%d:%d/start",
             major(st.st_rdev), minor(st.st_rdev));

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    uint64_t start = 0;
    if (fscanf(f, "%lu", &start) != 1) start = 0;
    fclose(f);
    return start;
}

/* ═══════════════════════════════════════════════════════════════
 * LBA_size 探测（ioctl）
 * ═══════════════════════════════════════════════════════════════ */
uint64_t get_lba_size(int fd, DevType type) {
    if (type == DEV_REGULAR) return 4096;

    int sz = 0;
    if (ioctl(fd, BLKSSZGET, &sz) != 0 || sz <= 0) {
        fprintf(stderr, "error: BLKSSZGET failed to determine LBA_size\n");
        return 0;
    }
    if (4096 % (unsigned)sz != 0) {
        fprintf(stderr, "error: LBA_size %d does not divide 4096\n", sz);
        return 0;
    }
    return (uint64_t)sz;
}

/* ═══════════════════════════════════════════════════════════════
 * 已有 FS 检测
 * ═══════════════════════════════════════════════════════════════ */
#define EXT4_MAGIC_OFFSET  0x438
#define EXT4_MAGIC_VALUE   0xEF53

bool check_existing_fs(int fd) {
    uint64_t raven_magic = 0;
    pread(fd, &raven_magic, 8, 0);
    if (raven_magic == SUPERBLOCK_MAGIC) {
        fprintf(stderr, "RavenFS superblock found — use -f to overwrite\n");
        return true;
    }

    uint16_t ext4_magic = 0;
    pread(fd, &ext4_magic, 2, EXT4_MAGIC_OFFSET);
    if (ext4_magic == EXT4_MAGIC_VALUE) {
        fprintf(stderr, "ext4 filesystem detected — use -f to overwrite\n");
        return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════
 * 主格式化
 *
 *   1. build Primary Superblock (memory)
 *   2. select backup locations
 *   3. build + write Bitmap
 *   4. build + write Inode Bitmap
 *   5. build + write Inode Array (root inode at index 0)
 *   6. build + write root dentry B+tree node
 *   7. write Backup Superblocks
 *   8. write Primary Superblock
 *   9. read-back verification
 * ═══════════════════════════════════════════════════════════════ */

static uint64_t fs_block_offset(const FsGeometry &, uint64_t block,
                                 const MkfsParams &p) {
    uint64_t dev_lba = p.world_base_lba + block * p.multiplier;
    return dev_lba * p.lba_size;
}

static uint64_t rand_range(uint64_t lo, uint64_t hi, uint64_t *seed) {
    *seed = *seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return lo + (*seed % (hi - lo));
}

static void build_superblock(RavenSuperblock *sb, const MkfsParams &p,
                              const FsGeometry &geo,
                              const uint64_t *backup_blocks,
                              uint64_t root_dentry_block) {
    memset(sb, 0, sizeof(*sb));
    sb->magic          = SUPERBLOCK_MAGIC;
    sb->version        = 1;
    sb->world_base_lba = p.world_base_lba;
    sb->total_blocks   = geo.total_blocks;
    sb->inode_array_count = p.inode_count;
    sb->root_inode     = 0;
    memcpy(sb->uuid, p.uuid, 16);
    memcpy(sb->volume_name, p.label, sizeof(sb->volume_name));

    sb->backup_prev  = 0;
    sb->backup_next  = p.backups > 0 ? backup_blocks[0] : 0;
    sb->backup_count = p.backups;

    // crc32 computed at time of write
    (void)root_dentry_block;
}

static void build_bitmap(uint8_t *bitmap, uint64_t bitmap_bytes,
                          const FsGeometry &geo,
                          const uint64_t *backup_blocks, uint32_t backup_count,
                          uint64_t root_dentry_block) {
    memset(bitmap, 0, bitmap_bytes);

    // Helper: set bit i
    auto set_bit = [&](uint64_t i) {
        bitmap[i / 8] |= (uint8_t)(1 << (i % 8));
    };

    // FS_block[0] reserved (overlaps Primary SB)
    set_bit(0);

    // Root dentry B+tree root
    set_bit(root_dentry_block);

    // Bitmap self
    for (uint64_t i = geo.bitmap_start; i < geo.total_blocks; i++)
        set_bit(i);

    // Inode array
    for (uint64_t i = geo.inode_array_base; i < geo.bitmap_start; i++)
        set_bit(i);

    // Inode bitmap
    for (uint64_t i = geo.inode_array_bitmap_base; i < geo.inode_array_base; i++)
        set_bit(i);

    // Backup blocks
    for (uint32_t bi = 0; bi < backup_count; bi++)
        set_bit(backup_blocks[bi]);

    // Tail padding bits past total_blocks
    uint64_t total_bits = geo.bitmap_blocks * 32768;
    for (uint64_t i = geo.total_blocks; i < total_bits; i++)
        set_bit(i);
}

static void build_inode_bitmap(uint8_t *ibm, uint64_t ibm_bytes,
                                uint64_t inode_count) {
    memset(ibm, 0, ibm_bytes);

    // Root inode (index 0)
    ibm[0] |= 1;

    // Tail padding
    uint64_t total_bits = ((inode_count + 32767) / 32768) * 32768;
    for (uint64_t i = inode_count; i < total_bits; i++)
        ibm[i / 8] |= (uint8_t)(1 << (i % 8));
}

static void build_root_inode(raven_Inode_t *inode, uint64_t root_dentry_block) {
    memset(inode, 0, sizeof(*inode));
    inode->type                   = INODE_TYPE_DIR;
    inode->is_data_root_direct_bit = 0;
    inode->uid                     = 0;
    inode->gid                     = 0;
    inode->permission_bits         = 0755;
    inode->size                    = 0;
    inode->data_root               = root_dentry_block;
    inode->link_count              = 1;
}

static void build_root_dentry_node(Dentry_Bptree_node_t *node,
                                    uint64_t parent_block) {
    memset(node, 0, sizeof(*node));
    node->head.head.fields.entry_count        = 0;
    node->head.head.fields.is_internal_node   = 0;
    node->head.head.fields.is_root_node       = 1;
    node->head.head.fields.parent_ptr         = 0;
    node->head.ptr1 = 0;
    node->head.ptr2 = 0;
    node->head.magic1 = DENTRY_MAGIC1;
    node->head.magic2 = DENTRY_MAGIC2;
    node->head.magic3 = DENTRY_MAGIC3;
    node->head.magic4 = DENTRY_MAGIC4;

    // "." → self (inode 0)
    node->entries[0].ptr = 0;
    strcpy(node->entries[0].name, ".");

    // ".." → parent (root, same as self)
    node->entries[1].ptr = 0;
    strcpy(node->entries[1].name, "..");

    node->head.head.fields.entry_count = 1;

    (void)parent_block;
}

int format_fs(int fd, const MkfsParams &params, const FsGeometry &geo) {
    uint64_t root_dentry_block = 1;  // block[0] reserved, block[1] = root dentry

    /* ─── Backup locations ─── */
    uint64_t backup_blocks[256];
    uint32_t bc = params.backups > 256 ? 256 : params.backups;

    uint64_t seed = 0;
    memcpy(&seed, params.uuid, sizeof(seed));

    for (uint32_t bi = 0; bi < bc; bi++) {
        uint64_t lo = geo.inode_array_bitmap_base;
        uint64_t hi = geo.total_blocks;
        if (hi - lo < 2) { lo = 2; hi = geo.total_blocks; }
        uint64_t blk;
    retry:
        blk = rand_range(lo, hi, &seed);
        if (blk == root_dentry_block) goto retry;
        for (uint32_t j = 0; j < bi; j++)
            if (backup_blocks[j] == blk) goto retry;
        backup_blocks[bi] = blk;
    }

    /* ─── Superblock (memory) ─── */
    RavenSuperblock sb;
    build_superblock(&sb, params, geo, backup_blocks, root_dentry_block);

    /* ─── 1. Bitmap ─── */
    uint64_t bitmap_bytes = geo.bitmap_blocks * 4096;
    uint8_t *bitmap = (uint8_t *)malloc(bitmap_bytes);
    build_bitmap(bitmap, bitmap_bytes, geo, backup_blocks, bc, root_dentry_block);

    uint64_t bm_off = fs_block_offset(geo, geo.bitmap_start, params);
    pwrite(fd, bitmap, bitmap_bytes, bm_off);
    if (params.verbose)
        fprintf(stderr, "  bitmap:  %lu blocks at offset %lu\n",
                (unsigned long)geo.bitmap_blocks, (unsigned long)bm_off);

    /* ─── 2. Inode Bitmap ─── */
    uint64_t ibm_bytes = geo.inode_bitmap_blocks * 4096;
    uint8_t *ibm = (uint8_t *)malloc(ibm_bytes);
    build_inode_bitmap(ibm, ibm_bytes, params.inode_count);

    uint64_t ibm_off = fs_block_offset(geo, geo.inode_array_bitmap_base, params);
    pwrite(fd, ibm, ibm_bytes, ibm_off);
    if (params.verbose)
        fprintf(stderr, "  inode bm: %lu blocks at offset %lu\n",
                (unsigned long)geo.inode_bitmap_blocks, (unsigned long)ibm_off);

    /* ─── 3. Inode Array ─── */
    uint64_t inode_bytes = geo.inode_blocks * 4096;
    uint8_t *inode_buf = (uint8_t *)calloc(1, inode_bytes);

    raven_Inode_t *root_i = (raven_Inode_t *)(inode_buf);
    build_root_inode(root_i, root_dentry_block);

    uint64_t ia_off = fs_block_offset(geo, geo.inode_array_base, params);
    pwrite(fd, inode_buf, inode_bytes, ia_off);
    if (params.verbose)
        fprintf(stderr, "  inodes:   %lu blocks at offset %lu\n",
                (unsigned long)geo.inode_blocks, (unsigned long)ia_off);

    /* ─── 4. Root dentry B+tree node ─── */
    Dentry_Bptree_node_t dentry_node;
    build_root_dentry_node(&dentry_node, root_dentry_block);

    uint64_t dn_off = fs_block_offset(geo, root_dentry_block, params);
    pwrite(fd, &dentry_node, sizeof(dentry_node), dn_off);
    if (params.verbose)
        fprintf(stderr, "  dentry:   block %lu at offset %lu\n",
                (unsigned long)root_dentry_block, (unsigned long)dn_off);

    /* ─── 5. Backup Superblocks ─── */
    for (uint32_t bi = 0; bi < bc; bi++) {
        RavenSuperblock backup_sb = sb;
        if (bi == 0)
            backup_sb.backup_prev = 0;
        else
            backup_sb.backup_prev = backup_blocks[bi - 1];
        backup_sb.backup_next = (bi + 1 < bc) ? backup_blocks[bi + 1] : 0;

        uint64_t bk_off = fs_block_offset(geo, backup_blocks[bi], params);
        // Zero out then write
        uint8_t zero[4096] = {};
        pwrite(fd, zero, 4096, bk_off);
        pwrite(fd, &backup_sb, sizeof(backup_sb), bk_off);
        if (params.verbose)
            fprintf(stderr, "  backup#%u: block %lu at offset %lu\n",
                    bi, (unsigned long)backup_blocks[bi], (unsigned long)bk_off);
    }

    /* ─── 6. Primary Superblock ─── */
    uint64_t sb_off = 0;
    pwrite(fd, &sb, sizeof(sb), sb_off);
    if (params.verbose)
        fprintf(stderr, "  primary:  at offset 0\n");

    /* ─── Read-back verify ─── */
    RavenSuperblock verify;
    pread(fd, &verify, sizeof(verify), 0);
    if (verify.magic != SUPERBLOCK_MAGIC) {
        fprintf(stderr, "ERROR: Primary Superblock verify failed\n");
        free(bitmap);
        free(ibm);
        free(inode_buf);
        return -1;
    }

    free(bitmap);
    free(ibm);
    free(inode_buf);
    return 0;
}

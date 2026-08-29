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
#include <sys/mman.h>

#include <linux/fs.h>
#include <linux/hdreg.h>
#include <linux/nvme_ioctl.h>
#include <uuid/uuid.h>

/* ═══════════════════════════════════════════════════════════════
 * CRC32 (Castagnoli polynomial 0xEDB88320)
 * ═══════════════════════════════════════════════════════════════ */
static uint32_t crc32_table[256];
static bool crc32_table_init = false;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        crc32_table[i] = crc;
    }
    crc32_table_init = true;
}

static uint32_t crc32_compute(const void *data, size_t len) {
    if (!crc32_table_init) crc32_init();
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *buf = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

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

static uint64_t block_to_offset(uint64_t block, const MkfsParams &p) {
    return (p.world_base_lba + block * p.multiplier) * p.lba_size;
}

static uint64_t rand_range(uint64_t lo, uint64_t hi, uint64_t *seed) {
    *seed = *seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return lo + (*seed % (hi - lo));
}

static void build_superblock(RavenSuperblock *sb, const MkfsParams &p,
                              const FsGeometry &geo,
                              const uint64_t *backup_blocks) {
    memset(sb, 0, sizeof(*sb));
    sb->magic          = SUPERBLOCK_MAGIC;
    sb->version        = RAVENFS_VERSION;   /* v1.1 = 2 */
    sb->world_base_lba = p.world_base_lba;
    sb->total_blocks   = geo.total_blocks;
    sb->inode_array_count = p.inode_count;
    sb->root_inode     = 0;
    memcpy(sb->uuid, p.uuid, 16);
    memcpy(sb->volume_name, p.label, sizeof(sb->volume_name));
    sb->backup_prev  = PTR_INVALID;
    sb->backup_next  = p.backups > 0 ? backup_blocks[0] : PTR_INVALID;
    sb->backup_count = p.backups;
}

/* 52 位指针空间无效哨兵（PTR_INVALID 的 52 位截断形态；ptrs[i].ptr 为位域） */
static constexpr uint64_t PTR52_INVALID = 0xFFFFFFFFFFFFFULL;

static void build_root_dentry_node(Bptree_dentry_leaf_node_t *node)
{
    /* v1.1 根叶子（Bptree_dentry_leaf_node_t）：
     *   parent_ptr  = {entry_count:0, is_inner:0, is_root:1, ptr=0}  → . = 根 inode 自身
     *   ptrs[1]     = .. inode（根目录的父亲是自己 = inode 0）
     *   ptrs[0]/[2] = 预留（52 位全 1 = 无效）
     *   entries     = 全空 — . / .. 由 parent_ptr / ptrs[1] 提供 */
    memset(node, 0, sizeof(*node));
    node->meta.magics[0] = DENTRY_MAGIC1;
    node->meta.magics[1] = DENTRY_MAGIC2;
    node->meta.magics[2] = DENTRY_MAGIC3;
    node->meta.magics[3] = DENTRY_MAGIC4;
    node->meta.parent_ptr.entry_count   = 0;
    node->meta.parent_ptr.is_inner_node = 0;
    node->meta.parent_ptr.is_root_node  = 1;
    node->meta.parent_ptr.ptr           = 0;   /* . = 根 inode 自身 */
    node->meta.ptrs[0].ptr = PTR52_INVALID;   /* 预留 */
    node->meta.ptrs[1].ptr = 0;               /* .. = 根 inode（根的父亲是自己） */
    node->meta.ptrs[2].ptr = PTR52_INVALID;   /* 预留 */
    /* entries[127] 全零（memset） */
}

static void build_root_inode(raven_Inode_t *inode, uint64_t root_dentry_block) {
    memset(inode, 0, sizeof(*inode));
    inode->type                    = INODE_TYPE_DIR;
    inode->uid                     = 0;
    inode->gid                     = 0;
    inode->permission_bits         = 0755;
    inode->size                    = 0;
    inode->data_root               = root_dentry_block;
    inode->link_count              = 2;
    /* 名字堆 10 描述符：未激活哨兵（bitmap/data_base 双指针 52 位全 1） */
    constexpr uint64_t NAME_HEAP_INACTIVE = (1ULL << 52) - 1;
    raven_dir_Inode_t *dino = (raven_dir_Inode_t *)inode;
    for (int i = 0; i < 10; i++) {
        ((name_heap_controller_t *)&dino->heaps_8B_8k)[i].bitmap_base_block_ptr = NAME_HEAP_INACTIVE;
        ((name_heap_controller_t *)&dino->heaps_8B_8k)[i].data_base_block_ptr   = NAME_HEAP_INACTIVE;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * BufBlock — RAII 元数据缓冲区
 *
 * 构造：mmap 匿名页（内核保证零初始化）
 * 析构：pwrite 落盘 → munmap 释放
 * ═══════════════════════════════════════════════════════════════ */
struct BufBlock {
    uint8_t *base;
    size_t   bytes;
    int      fd;
    uint64_t offset;

    BufBlock(int fd_, size_t bytes_, uint64_t offset_)
        : base((uint8_t *)mmap(nullptr, bytes_,
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS,
                                -1, 0)),
          bytes(bytes_), fd(fd_), offset(offset_)
    {
    }

    ~BufBlock() {
        if (base && base != MAP_FAILED) {
            pwrite(fd, base, bytes, offset);
            munmap(base, bytes);
        }
    }

    BufBlock(const BufBlock &) = delete;
    BufBlock &operator=(const BufBlock &) = delete;
};

/* ═══════════════════════════════════════════════════════════════
 * BitmapBuf — FS_block 分配图
 * ═══════════════════════════════════════════════════════════════ */
struct BitmapBuf {
    BufBlock blk;
    uint64_t total_blocks;
    uint64_t bitmap_blocks;

    BitmapBuf(int fd, const FsGeometry &geo, uint64_t bm_off)
        : blk(fd, geo.bitmap_blocks * 4096, bm_off),
          total_blocks(geo.total_blocks),
          bitmap_blocks(geo.bitmap_blocks) {}

    void mark(uint64_t i) {
        blk.base[i / 8] |= (uint8_t)(1 << (i % 8));
    }

    void mark_range(uint64_t base, uint64_t count) {
        for (uint64_t i = base; i < base + count; i++)
            mark(i);
    }

    void mark_backup(const uint64_t *blocks, uint32_t count) {
        for (uint32_t i = 0; i < count; i++)
            mark(blocks[i]);
    }

    void mark_tail() {
        uint64_t total_bits = bitmap_blocks * 32768;
        for (uint64_t i = total_blocks; i < total_bits; i++)
            mark(i);
    }
};

/* ═══════════════════════════════════════════════════════════════
 * InodeArrayBuf — Inode 数组
 * ═══════════════════════════════════════════════════════════════ */
struct InodeArrayBuf {
    BufBlock blk;

    InodeArrayBuf(int fd, const FsGeometry &geo, uint64_t ia_off)
        : blk(fd, geo.inode_blocks * 4096, ia_off) {}

    void set(uint64_t idx, const raven_Inode_t *inode) {
        memcpy(blk.base + idx * sizeof(raven_Inode_t), inode, sizeof(raven_Inode_t));
    }
};

/* ═══════════════════════════════════════════════════════════════
 * InodeBitmapBuf — Inode 分配图
 * ═══════════════════════════════════════════════════════════════ */
struct InodeBitmapBuf {
    BufBlock blk;
    uint64_t inode_count;

    InodeBitmapBuf(int fd, const FsGeometry &geo, uint64_t ibm_off, uint64_t inode_count_)
        : blk(fd, geo.inode_bitmap_blocks * 4096, ibm_off),
          inode_count(inode_count_) {}

    void mark(uint64_t i) {
        blk.base[i / 8] |= (uint8_t)(1 << (i % 8));
    }

    void mark_tail() {
        uint64_t total_bits = ((inode_count + 32767) / 32768) * 32768;
        for (uint64_t i = inode_count; i < total_bits; i++)
            mark(i);
    }
};

int format_fs(int fd, const MkfsParams &params, const FsGeometry &geo) {
    uint64_t root_dentry_block = 0;

    /* ─── Backup locations ─── */
    uint64_t backup_blocks[256];
    uint32_t bc = params.backups > 256 ? 256 : params.backups;

    uint64_t seed = 0;
    memcpy(&seed, params.uuid, sizeof(seed));

    /* Backup 散布在数据区 [2, inode_array_bitmap_base)，
     * 避开根 dentry (block 0-1) 和所有元数据（bitmap、inode array、inode bitmap） */
    uint64_t data_lo = 2;
    uint64_t data_hi = geo.inode_array_bitmap_base;
    for (uint32_t bi = 0; bi < bc; bi++) {
        if (data_hi - data_lo < 2) break;  // 数据区太小，能放几个放几个
        uint64_t blk;
    retry:
        blk = rand_range(data_lo, data_hi, &seed);
        if (blk == root_dentry_block || blk == root_dentry_block + 1) goto retry;
        for (uint32_t j = 0; j < bi; j++)
            if (backup_blocks[j] == blk) goto retry;
        backup_blocks[bi] = blk;
    }

    /* ─── Superblock (memory) ─── */
    RavenSuperblock sb;
    build_superblock(&sb, params, geo, backup_blocks);
    sb.crc32 = 0;
    sb.crc32 = crc32_compute(&sb, sizeof(sb));

    /* ─── 偏移量 ─── */
    uint64_t bm_off  = block_to_offset(geo.bitmap_start, params);
    uint64_t ia_off  = block_to_offset(geo.inode_array_base, params);
    uint64_t ibm_off = block_to_offset(geo.inode_array_bitmap_base, params);
    uint64_t dn_off  = block_to_offset(root_dentry_block, params);

    Bptree_dentry_leaf_node_t dentry_node;
    build_root_dentry_node(&dentry_node);

    raven_Inode_t root_inode;
    build_root_inode(&root_inode, root_dentry_block);

    {
        BitmapBuf      bitmap(fd, geo, bm_off);
        InodeArrayBuf  inodes(fd, geo, ia_off);
        InodeBitmapBuf ibitmap(fd, geo, ibm_off, params.inode_count);

        bitmap.mark_range(root_dentry_block, 2);
        bitmap.mark_range(geo.inode_array_bitmap_base, geo.inode_bitmap_blocks);
        bitmap.mark_range(geo.inode_array_base, geo.inode_blocks);
        bitmap.mark_range(geo.bitmap_start, geo.bitmap_blocks);
        bitmap.mark_backup(backup_blocks, bc);
        bitmap.mark_tail();

        inodes.set(0, &root_inode);

        ibitmap.mark(0);
        ibitmap.mark_tail();

        // Root dentry + Backup SBs（显式写）
        pwrite(fd, &dentry_node, sizeof(dentry_node), dn_off);
        for (uint32_t bi = 0; bi < bc; bi++) {
            RavenSuperblock backup_sb = sb;
            backup_sb.backup_prev = bi > 0 ? backup_blocks[bi - 1] : PTR_INVALID;
            backup_sb.backup_next =
                (bi + 1 < bc) ? backup_blocks[bi + 1] : PTR_INVALID;  // 链尾哨兵（文档 §2.1）
            backup_sb.crc32 = 0;  // 重算 CRC32（backup_prev/next 与 Primary 不同）
            backup_sb.crc32 = crc32_compute(&backup_sb, sizeof(backup_sb));
            uint64_t bk_off = block_to_offset(backup_blocks[bi], params);
            pwrite(fd, &backup_sb, sizeof(backup_sb), bk_off);
        }

        if (params.verbose) {
            fprintf(stderr, "  bitmap:   %lu blocks at offset %lu\n",
                    (unsigned long)geo.bitmap_blocks, (unsigned long)bm_off);
            fprintf(stderr, "  inodes:   %lu blocks at offset %lu\n",
                    (unsigned long)geo.inode_blocks, (unsigned long)ia_off);
            fprintf(stderr, "  inode bm: %lu blocks at offset %lu\n",
                    (unsigned long)geo.inode_bitmap_blocks, (unsigned long)ibm_off);
            fprintf(stderr, "  dentry:   block 1 at offset %lu\n",
                    (unsigned long)dn_off);
        }
    }

    /* ─── Primary Superblock（最后写：崩溃安全） ─── */
    pwrite(fd, &sb, sizeof(sb), 0);
    if (params.verbose)
        fprintf(stderr, "  primary:  at offset 0\n");

    /* ─── Read-back verify ─── */
    RavenSuperblock verify;
    pread(fd, &verify, sizeof(verify), 0);
    if (verify.magic != SUPERBLOCK_MAGIC) {
        fprintf(stderr, "ERROR: Primary Superblock verify failed\n");
        return -1;
    }
    return 0;
}

/*
 * ═══════════════════════════════════════════════════════════════════════════
 * mkfs v2.0 写出口（**独立 TU**：v2.0 盘面头与 v1.3 头类型重名且不兼容，
 *   不能与 mkfs.cpp 同 TU include —— 见 mkfs.hpp 顶部说明）。
 *
 * v2.0 与 v1.x 的关键差异：
 *   · 无固定 inode 数组 / 无 inode 位图；inode 存储 = 超级块统辖的**弹性数据面**。
 *   · block bitmap 仍在尾部、仍按几何推导：bitmap_blocks = ceil(total/32768)。
 *   · 数据面**起步 = 一段 256KB direct 连续段**（64 块 / 32 个 8K 片 / 1024 inode）：
 *       inode_data_ctl = direct(1) | ptr = PLANE_BLOCK
 *       inode_data_plane_pieces_count = 32
 *   · 空闲面 = **扁 fr 快照**（hdr 64B + N×16B），起步 1 条 {base=1, len=1023}
 *     （idx 0 归 root，契约级硬编码 RFS_ROOT_INODE_IDX=0）。
 *   · 根 dentry 树根叶镜像 v1.3（parent_ptr.ptr = "." = 0；ptrs[1] = ".." = 0）。
 *
 * 低块确定性布局（FS_block 号）：
 *   [0, 2)   根 dentry 根叶（8K）
 *   [2, 66)  inode 数据面 direct 段（256KB，8K 对齐起点）
 *   [66, 68) 扁 fr 快照区（8K）
 *   [68, bitmap_start)  Backup SB（随机散布）
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "mkfs.hpp"
#include "disk_struct_v2.0.h"
#include "magics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdint>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr uint64_t FS_BLOCK = 4096;
constexpr uint64_t PIECE    = 8192;
constexpr uint64_t INODE    = 256;

/* 256KB direct 段：64 块 / 32 片 / 1024 inode */
constexpr uint64_t PLANE_BYTES  = 256ull * 1024;
constexpr uint64_t PLANE_BLOCKS = PLANE_BYTES / FS_BLOCK;      /* 64 */
constexpr uint64_t PLANE_PIECES = PLANE_BYTES / PIECE;         /* 32 */
constexpr uint64_t PLANE_INODES = PLANE_BYTES / INODE;         /* 1024 */

/* 低块确定性布局 */
constexpr uint64_t ROOT_DENTRY_BLOCK = 0;
constexpr uint64_t PLANE_BLOCK       = 2;                              /* 8K 对齐（偶） */
constexpr uint64_t FR_BLOCK          = PLANE_BLOCK + PLANE_BLOCKS;    /* 66 */
constexpr uint64_t FR_BLOCKS         = 1;                              /* 1 块内容（hdr+entries）；fr 仅需 1 块对齐 */
constexpr uint64_t META_END          = FR_BLOCK + FR_BLOCKS;           /* 68 */

uint64_t lcg(uint64_t *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}
uint64_t rand_range(uint64_t lo, uint64_t hi, uint64_t *s) {
    return lo + (lcg(s) % (hi - lo));
}

uint64_t blk_off(uint64_t blk, const MkfsParams &p) {
    return (p.world_base_lba + blk * p.multiplier) * p.lba_size;
}

/* CRC32（与 mkfs.cpp 同源：poly 0xEDB88320 反射，初/末取反） */
uint32_t crc32_of(const void *data, size_t len) {
    static uint32_t tbl[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0u);
            tbl[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *b = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) crc = tbl[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

bool pw(int fd, const void *buf, size_t len, uint64_t off) {
    return pwrite(fd, buf, len, (off_t)off) == (ssize_t)len;
}

void bm_mark_range(uint8_t *bm, uint64_t base, uint64_t count) {
    for (uint64_t i = base; i < base + count; i++)
        bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}

void build_root_dentry_v20(Bptree_ddentry_leaf_node_t *node) {
    memset(node, 0, sizeof(*node));
    node->meta.magics[0] = DENTRY_MAGIC1;
    node->meta.magics[1] = DENTRY_MAGIC2;
    node->meta.magics[2] = DENTRY_MAGIC3;
    node->meta.magics[3] = DENTRY_MAGIC4;
    node->meta.parent_ptr.v = RFS_PP_PACK(0, 0, 1, RFS_ROOT_INODE_IDX); /* "." = 根 inode idx 0 */
    node->meta.ptrs[0].v = RFS_PTR_PACK(0, RFS_MASK52);                 /* 预留（52 位全 1） */
    node->meta.ptrs[1].v = RFS_PTR_PACK(0, RFS_ROOT_INODE_IDX);         /* ".." = 根自身 */
    node->meta.ptrs[2].v = RFS_PTR_PACK(0, RFS_MASK52);                 /* 预留 */
    /* entries[127] 全零（memset） */
}

void build_root_inode_v20(raven_Inode_t *inode, uint64_t dentry_blk, uint64_t stamp) {
    memset(inode, 0, sizeof(*inode));
    inode->mtime = inode->ctime = inode->atime = stamp;
    inode->m_ns_stamp = inode->c_ns_stamp = inode->a_ns_stamp = 0;
    inode->word1 = RFS_INO1_PACK(INODE_TYPE_DIR, 0, 0);
    inode->mode_xattr = RFS_MODE_XATTR_PACK(0755, RFS_XATTR_NONE);
    inode->size = 0;
    inode->link_count = 2;
    /* DIR 视图：dentry 树根 + 名字数据面空态哨兵 */
    raven_dir_Inode_t *d = (raven_dir_Inode_t *)(void *)inode;
    d->ddentry_Bptree_root       = dentry_blk;              /* == data_root @72 */
    d->name_data_ctl             = RFS_ND_CTL_INACTIVE;
    d->name_data_piece_count     = RFS_ND_PIECE_PACK(0);
    d->name_data_free_ranges_ctl = RFS_ND_FR_CTL_INACTIVE;
    d->name_data_free_ranges_count = 0;
    /* possible_inline_free_ranges[] 已由 memset 清 0 */
}

}  // namespace

/* ── v2.0 几何：只有 block bitmap 尾部（几何推导）── */
V20Geometry calc_geometry_v20(uint64_t total_blocks) {
    V20Geometry g{};
    g.total_blocks = total_blocks;
    const uint64_t bb = (total_blocks + 32767) / 32768;
    g.bitmap_blocks = bb;
    g.bitmap_start  = total_blocks - bb;
    return g;
}

int format_fs_v20(int fd, const MkfsParams &params, const V20Geometry &geo) {
    const uint64_t total_blocks = geo.total_blocks;

    if (geo.bitmap_start <= META_END + 1) {
        fprintf(stderr,
            "error: device too small for v2.0 metadata "
            "(need bitmap_start > %llu blocks)\n",
            (unsigned long long)(META_END + 1));
        return 5;
    }

    /* ── Backup SB 位置：数据区 [META_END, bitmap_start) 随机散布 ── */
    uint64_t backup_blocks[256];
    uint32_t bc = params.backups > 256 ? 256 : params.backups;
    uint64_t seed = 0;
    memcpy(&seed, params.uuid, sizeof(seed));
    const uint64_t data_lo = META_END;
    const uint64_t data_hi = geo.bitmap_start;
    for (uint32_t i = 0; i < bc; i++) {
        uint64_t blk;
    retry:
        blk = rand_range(data_lo, data_hi, &seed);
        for (uint32_t j = 0; j < i; j++)
            if (backup_blocks[j] == blk) goto retry;
        backup_blocks[i] = blk;
    }

    const uint64_t now = (uint64_t)time(nullptr);

    /* ── 根 dentry 根叶（8K）── */
    Bptree_ddentry_leaf_node_t dentry;
    build_root_dentry_v20(&dentry);

    /* ── inode 数据面 direct 段（256KB 全零，idx0 写根 inode）── */
    uint8_t *plane = (uint8_t *)calloc(1, PLANE_BYTES);
    if (!plane) { fprintf(stderr, "error: calloc inode plane\n"); return 6; }
    {
        raven_Inode_t root;
        build_root_inode_v20(&root, ROOT_DENTRY_BLOCK, now);
        memcpy(plane, &root, sizeof(root));                 /* 逻辑 idx 0 @ 段内字节 0 */
    }

    /* ── 扁 fr 快照区（8K）：hdr + 1 条 {base=1, len=PLANE_INODES-1}（idx0 归 root）── */
    uint8_t *frbuf = (uint8_t *)calloc(1, FR_BLOCKS * FS_BLOCK);
    if (!frbuf) { free(plane); return 6; }
    {
        inode_data_fr_snapshot_hdr *h = (inode_data_fr_snapshot_hdr *)frbuf;
        h->magic[0] = INODE_DATA_FR_SNAPSHOT_MAGIC1;
        h->magic[1] = INODE_DATA_FR_SNAPSHOT_MAGIC2;
        h->magic[2] = INODE_DATA_FR_SNAPSHOT_MAGIC3;
        h->magic[3] = INODE_DATA_FR_SNAPSHOT_MAGIC4;
        h->entry_count = 1;
        h->capacity    = 1;
        h->rsv0        = 0;
        h->rsv1[0]     = 0;     /* 分配器水位线不落盘（运行时 fs_allocator.hpp）*/
        inode_dataplane_coordinate_free_range_t *e =
            (inode_dataplane_coordinate_free_range_t *)(frbuf + sizeof(*h));
        e->base = 1;                        /* idx 0 = root，不可分配 */
        e->len  = PLANE_INODES - 1;         /* 1023 */
        h->crc32 = 0;
        h->crc32 = crc32_of(frbuf, sizeof(*h) + sizeof(*e));
    }

    /* ── 块位图（零 + 标元数据 / backup / 自身 / 尾巴）── */
    const uint64_t bm_bytes = geo.bitmap_blocks * FS_BLOCK;
    uint8_t *bm = (uint8_t *)calloc(1, bm_bytes);
    if (!bm) { free(plane); free(frbuf); return 6; }
    bm_mark_range(bm, 0, META_END);                              /* [0,68)：dentry/plane/fr */
    bm_mark_range(bm, geo.bitmap_start, geo.bitmap_blocks);      /* block bitmap 自身 */
    for (uint32_t i = 0; i < bc; i++) bm_mark_range(bm, backup_blocks[i], 1);
    {
        const uint64_t total_bits = geo.bitmap_blocks * 32768ull;
        bm_mark_range(bm, total_blocks, total_bits - total_blocks);  /* mid 尾巴全 1 */
    }

    /* ── Superblock ── */
    RavenSuperblock sb;
    memset(&sb, 0, sizeof(sb));
    uint64_t mseed = 0;
    memcpy(&mseed, params.uuid, sizeof(mseed));
    sb.magic[0] = RFS_SB_MAGIC_W0;
    sb.magic[1] = lcg(&mseed);
    sb.magic[2] = lcg(&mseed);
    sb.magic[3] = lcg(&mseed);
    sb.version        = RAVENFS_VERSION;
    sb.world_base_lba = params.world_base_lba;
    sb.total_blocks   = total_blocks;
    sb.inode_data_ctl = RFS_IDP_CTL_PACK(1 /*direct*/, PLANE_BLOCK);
    sb.inode_data_plane_pieces_count = RFS_IDP_PIECE_PACK(PLANE_PIECES);
    sb.inode_data_fr_ctl   = RFS_IDP_FR_CTL_PACK(FR_BLOCK);
    sb.inode_data_fr_count = 1;
    memcpy(sb.uuid, params.uuid, 16);
    memcpy(sb.volume_name, params.label, sizeof(sb.volume_name));
    sb.backup_prev  = PTR_INVALID;
    sb.backup_next  = bc ? backup_blocks[0] : PTR_INVALID;
    sb.backup_count = bc;
    sb.born_time    = now;
    /* latest_mount/syn/umount、*_mount_*、abnormal_shutdown_counts 全 0（memset）*/
    sb.crc32 = 0;
    sb.crc32 = crc32_of(&sb, sizeof(sb));

    /* ── 落盘（Primary 最后写：崩溃安全）── */
    int rc = 0;
    if (!rc && !pw(fd, &dentry, sizeof(dentry),                  blk_off(ROOT_DENTRY_BLOCK, params))) rc = -1;
    if (!rc && !pw(fd, plane, PLANE_BYTES,                        blk_off(PLANE_BLOCK, params)))        rc = -1;
    if (!rc && !pw(fd, frbuf, FR_BLOCKS * FS_BLOCK,               blk_off(FR_BLOCK, params)))           rc = -1;
    if (!rc && !pw(fd, bm, bm_bytes,                              blk_off(geo.bitmap_start, params)))   rc = -1;

    for (uint32_t i = 0; !rc && i < bc; i++) {
        RavenSuperblock bsb = sb;
        bsb.backup_prev = (i > 0) ? backup_blocks[i - 1] : PTR_INVALID;
        bsb.backup_next = (i + 1 < bc) ? backup_blocks[i + 1] : PTR_INVALID;
        bsb.crc32 = 0;
        bsb.crc32 = crc32_of(&bsb, sizeof(bsb));
        if (!pw(fd, &bsb, sizeof(bsb), blk_off(backup_blocks[i], params))) rc = -1;
    }

    if (!rc && !pw(fd, &sb, sizeof(sb), 0)) rc = -1;

    free(bm); free(frbuf); free(plane);

    if (rc) { fprintf(stderr, "error: v2.0 metadata write failed\n"); return 6; }

    /* ── 回读校验 ── */
    RavenSuperblock verify;
    if (pread(fd, &verify, sizeof(verify), 0) != (ssize_t)sizeof(verify) ||
        verify.magic[0] != RFS_SB_MAGIC_W0 ||
        verify.version  != RAVENFS_VERSION) {
        fprintf(stderr, "ERROR: v2.0 Primary Superblock verify failed\n");
        return -1;
    }

    if (params.verbose) {
        fprintf(stderr,
            "v2.0 layout (FS_block world, total_blocks=%llu):\n"
            "  block bitmap:      [%llu, %llu)  (%llu blocks)\n"
            "  root dentry:       [%llu, %llu)\n"
            "  inode data plane:  [%llu, %llu)  direct %lluKB = %llu pieces = %llu inodes\n"
            "  fr snapshot:       [%llu, %llu)  (flat, %u entry)\n"
            "  backups:           %u\n"
            "  root inode idx:    %llu  (FS 契约硬编码)\n",
            (unsigned long long)total_blocks,
            (unsigned long long)geo.bitmap_start,
            (unsigned long long)(geo.bitmap_start + geo.bitmap_blocks),
            (unsigned long long)geo.bitmap_blocks,
            (unsigned long long)ROOT_DENTRY_BLOCK,
            (unsigned long long)(ROOT_DENTRY_BLOCK + 2),
            (unsigned long long)PLANE_BLOCK,
            (unsigned long long)(PLANE_BLOCK + PLANE_BLOCKS),
            (unsigned long long)(PLANE_BYTES / 1024),
            (unsigned long long)PLANE_PIECES,
            (unsigned long long)PLANE_INODES,
            (unsigned long long)FR_BLOCK,
            (unsigned long long)(FR_BLOCK + FR_BLOCKS),
            1u,
            (unsigned)bc,
            (unsigned long long)RFS_ROOT_INODE_IDX);
    }
    return 0;
}

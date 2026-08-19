/* ═══════════════════════════════════════════════════════════════════
 * ravenfs_juvenile.cpp — RavenFS v1.1 幼年期上帝类实现
 *
 * 本文件负责（Raven 分工）：
 *   构造/析构（挂载/卸载）、块 I/O、inode I/O、
 *   双位图分配器（block 组 + inode 组，全量内存 + write-through）
 *   CRC32、dump_info
 *
 * 不实现（Sparrow 手写，见 dentry_tree.cpp）：
 *   name_hash / dentry_* 全套 / root_dir_* 全套 / dump_dentry_tree
 *
 * 零缓存原则：所有块/inode 访问直接 pread/pwrite，无缓存层。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/ravenfs_v1.1.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <new>

/* ═══════════════════════════════════════════════════════════════
 * CRC32（Castagnoli 0xEDB88320，查表）— SB 校验用
 * ═══════════════════════════════════════════════════════════════ */
uint32_t RavenFS_juvenile::crc32_table[256];
bool     RavenFS_juvenile::crc32_ready = false;

void RavenFS_juvenile::crc32_init() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320u : 0);
        crc32_table[i] = crc;
    }
    crc32_ready = true;
}

uint32_t RavenFS_juvenile::crc32_compute(const void *data, size_t len) {
    if (!crc32_ready) crc32_init();
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *buf = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ═══════════════════════════════════════════════════════════════
 * 构造即挂载
 *   open → 读 Primary SB → magic/version/crc32 三连验 → 几何推导
 *   → 双位图全量入内存 + 自描述校验 → 读 root inode（取树根块号）
 * 任一失败：清理资源、fd=-1、*res_code = RC_ERR_*
 * ═══════════════════════════════════════════════════════════════ */
RavenFS_juvenile::RavenFS_juvenile(const char *blk_device_path, uint64_t *res_code) {
    if (res_code) *res_code = RC_ERR_OPEN;   /* 默认失败态 */

    fd = ::open(blk_device_path, O_RDWR);
    if (fd < 0) { if (res_code) *res_code = RC_ERR_OPEN; return; }

    /* ── 读 Primary Superblock ── */
    if (pread(fd, &sb, sizeof(sb), 0) != (ssize_t)sizeof(sb)) {
        close(fd); fd = -1;
        if (res_code) *res_code = RC_ERR_IO;
        return;
    }
    if (sb.magic != SUPERBLOCK_MAGIC) {
        close(fd); fd = -1;
        if (res_code) *res_code = RC_ERR_MAGIC;
        return;
    }
    if (sb.version != RAVENFS_VERSION) {
        close(fd); fd = -1;
        if (res_code) *res_code = RC_ERR_VERSION;
        return;
    }
    {
        RavenSuperblock sb_copy = sb;
        sb_copy.crc32 = 0;
        if (sb.crc32 != crc32_compute(&sb_copy, sizeof(sb_copy))) {
            close(fd); fd = -1;
            if (res_code) *res_code = RC_ERR_CRC;
            return;
        }
    }

    /* ── 几何推导（block_model §3.2，与 mkfs calc_geometry 同源）── */
    total_blocks = sb.total_blocks;
    inode_count  = sb.inode_array_count;
    bitmap_blocks    = (total_blocks + 32767) / 32768;
    bitmap_start     = total_blocks - bitmap_blocks;
    inode_blocks     = (inode_count * 256 + 4095) / 4096;
    inode_array_base = bitmap_start - inode_blocks;
    inode_bm_blocks  = (inode_count + 32767) / 32768;
    inode_bm_base    = inode_array_base - inode_bm_blocks;

    /* ── 双位图全量入内存 + 自描述校验 ── */
    if (!bitmaps_load()) {
        close(fd); fd = -1;
        if (res_code) *res_code = RC_ERR_BITMAP_SELF;
        return;
    }

    /* ── root inode：取 dentry 树根块号 ── */
    root_inum = sb.root_inode;
    raven_Inode_t root_inode;
    inode_read(root_inum, &root_inode);
    if (root_inode.type != INODE_TYPE_DIR || root_inode.data_root == PTR_INVALID) {
        delete[] block_bits; block_bits = nullptr;
        delete[] inode_bits; inode_bits = nullptr;
        close(fd); fd = -1;
        if (res_code) *res_code = RC_ERR_STATE;
        return;
    }
    root_dentry_blk = root_inode.data_root;

    if (res_code) *res_code = RC_OK;
}

/* 析构即卸载：双位图全量写回 → 释放内存 → 关 fd */
RavenFS_juvenile::~RavenFS_juvenile() {
    if (fd >= 0 && block_bits && inode_bits)
        bitmaps_flush();
    delete[] block_bits; block_bits = nullptr;
    delete[] inode_bits; inode_bits = nullptr;
    if (fd >= 0) { close(fd); fd = -1; }
}

/* ═══════════════════════════════════════════════════════════════
 * 块 I/O（零缓存）
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::read_block(uint64_t blk, void *buf) {
    pread(fd, buf, 4096, (off_t)fblock_to_byte(blk));
}

void RavenFS_juvenile::write_block(uint64_t blk, const void *buf) {
    pwrite(fd, buf, 4096, (off_t)fblock_to_byte(blk));
}

/* ═══════════════════════════════════════════════════════════════
 * inode I/O（零缓存；inode array 从 inode_array_base 块起线性排布）
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::inode_read(uint64_t inum, raven_Inode_t *out) const {
    pread(fd, out, sizeof(raven_Inode_t),
          (off_t)(fblock_to_byte(inode_array_base) + inum * sizeof(raven_Inode_t)));
}

void RavenFS_juvenile::inode_write(uint64_t inum, const raven_Inode_t *in) {
    pwrite(fd, in, sizeof(raven_Inode_t),
           (off_t)(fblock_to_byte(inode_array_base) + inum * sizeof(raven_Inode_t)));
}

/* ═══════════════════════════════════════════════════════════════
 * 位图核心
 * ═══════════════════════════════════════════════════════════════ */

/* mount：读入双位图 + 自描述校验（block_model §6.6）
 * 校验点：bitmap 自身区域 [bitmap_start, total_blocks) 全 1
 *         inode 元数据区域 [inode_bm_base, bitmap_start) 全 1
 *         inode 位图 bit0（root inode）必须为 1 */
bool RavenFS_juvenile::bitmaps_load() {
    try {
        block_bits = new uint8_t[bitmap_blocks * 4096];
        inode_bits = new uint8_t[inode_bm_blocks * 4096];
    } catch (const std::bad_alloc &) {
        return false;
    }

    if (pread(fd, block_bits, bitmap_blocks * 4096,
              (off_t)fblock_to_byte(bitmap_start)) != (ssize_t)(bitmap_blocks * 4096))
        goto fail;
    if (pread(fd, inode_bits, inode_bm_blocks * 4096,
              (off_t)fblock_to_byte(inode_bm_base)) != (ssize_t)(inode_bm_blocks * 4096))
        goto fail;

    /* 自描述校验 */
    for (uint64_t i = bitmap_start; i < total_blocks; i++)
        if (!(block_bits[i / 8] & (1u << (i % 8)))) goto fail;
    for (uint64_t i = inode_bm_base; i < bitmap_start; i++)
        if (!(block_bits[i / 8] & (1u << (i % 8)))) goto fail;
    if (!(inode_bits[0] & 1u)) goto fail;   /* root inode 亮灯 */

    return true;

fail:
    delete[] block_bits; block_bits = nullptr;
    delete[] inode_bits; inode_bits = nullptr;
    return false;
}

/* 全量写回：双位图各 pwrite 一次（析构时调用；分配/释放只改内存态） */
void RavenFS_juvenile::bitmaps_flush() {
    pwrite(fd, block_bits, bitmap_blocks * 4096,
           (off_t)fblock_to_byte(bitmap_start));
    pwrite(fd, inode_bits, inode_bm_blocks * 4096,
           (off_t)fblock_to_byte(inode_bm_base));
}

/* 置位/清位（内存态） */
void RavenFS_juvenile::bm_set(uint8_t *bits, uint64_t bit, bool v) {
    if (v) bits[bit / 8] |=  (uint8_t)(1u << (bit % 8));
    else   bits[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

/* 单 bit first-fit（inode 组用） */
uint64_t RavenFS_juvenile::bm_alloc_first(uint8_t *bits, uint64_t nbits, uint64_t start) {
    for (uint64_t i = start; i < nbits; i++)
        if (!(bits[i / 8] & (1u << (i % 8)))) return i;
    return PTR_INVALID;
}

/* 区间分配：从 start 起找 count 个连续空闲 bit，起始位 2^align_log2 对齐。
 * 返回起始位号或 PTR_INVALID。 */
uint64_t RavenFS_juvenile::bm_alloc_range(uint8_t *bits, uint64_t nbits,
                                          uint64_t count, uint8_t align_log2,
                                          uint64_t start) {
    if (count == 0 || count > nbits) return PTR_INVALID;
    uint64_t align = 1ULL << (align_log2 > 52 ? 52 : align_log2);
    if (align == 0) return PTR_INVALID;

    uint64_t i = start;
    if (i % align) i = ((i + align - 1) / align) * align;
    for (; i + count <= nbits; i += align) {
        bool free_run = true;
        for (uint64_t j = i; j < i + count; j++) {
            if (bits[j / 8] & (1u << (j % 8))) { free_run = false; break; }
        }
        if (free_run) return i;
    }
    return PTR_INVALID;
}

/* ═══════════════════════════════════════════════════════════════
 * 位图接口组 A：FS_block 分配器
 * ═══════════════════════════════════════════════════════════════ */

/* 硬分配：count 连续 + 对齐，原子；失败 PTR_INVALID（不部分分配） */
uint64_t RavenFS_juvenile::block_alloc_hard(uint64_t require_count, uint8_t align_log2) {
    if (require_count == 0) return PTR_INVALID;

    uint64_t blk = bm_alloc_range(block_bits, total_blocks, require_count, align_log2, bm_cursor);
    if (blk == PTR_INVALID)   /* 游标段碎片 → 绕回 2 再试一次 */
        blk = bm_alloc_range(block_bits, total_blocks, require_count, align_log2, 2);
    if (blk == PTR_INVALID) return PTR_INVALID;

    for (uint64_t i = 0; i < require_count; i++)
        bm_set(block_bits, blk + i, true);

    bm_cursor = blk + require_count;   /* 游标推进 */
    return blk;
}

/* 软分配：第一个 ≥1 连续区间，实际分配 min(区间长, require)；
 * *remain_count = 缺口数（require − 实际）；一块没有 → PTR_INVALID */
uint64_t RavenFS_juvenile::block_alloc_soft(uint64_t require_count, uint64_t *remain_count) {
    if (remain_count) *remain_count = require_count;
    if (require_count == 0) return PTR_INVALID;

    uint64_t start = bm_alloc_first(block_bits, total_blocks, bm_cursor);
    if (start == PTR_INVALID)
        start = bm_alloc_first(block_bits, total_blocks, 2);   /* 绕回 */
    if (start == PTR_INVALID) return PTR_INVALID;

    uint64_t run = 0;                    /* 连续空闲长度 */
    while (start + run < total_blocks &&
           !(block_bits[(start + run) / 8] & (1u << ((start + run) % 8))))
        run++;

    uint64_t got = run < require_count ? run : require_count;
    for (uint64_t i = 0; i < got; i++)
        bm_set(block_bits, start + i, true);

    bm_cursor = start + got;
    if (remain_count) *remain_count = require_count - got;
    return start;
}

/* 区间释放 */
void RavenFS_juvenile::block_free(uint64_t blkbase, uint64_t count) {
    if (count == 0) return;
    for (uint64_t i = 0; i < count; i++)
        bm_set(block_bits, blkbase + i, false);
}

bool RavenFS_juvenile::block_test(uint64_t blk) const {
    return (block_bits[blk / 8] & (1u << (blk % 8))) != 0;
}

/* ═══════════════════════════════════════════════════════════════
 * 位图接口组 B：inode 槽分配器
 * ═══════════════════════════════════════════════════════════════ */
uint64_t RavenFS_juvenile::inode_alloc() {
    uint64_t inum = bm_alloc_first(inode_bits, inode_count, 1);   /* 0 = root */
    if (inum == PTR_INVALID) return PTR_INVALID;
    bm_set(inode_bits, inum, true);
    return inum;
}

void RavenFS_juvenile::inode_free(uint64_t inum) {
    bm_set(inode_bits, inum, false);
}

bool RavenFS_juvenile::inode_test(uint64_t inum) const {
    return (inode_bits[inum / 8] & (1u << (inum % 8))) != 0;
}

/* ═══════════════════════════════════════════════════════════════
 * 调试：dump_info（不碰 dentry 树）
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::dump_info() const {
    uint64_t used_blocks = 0, used_inodes = 0;
    for (uint64_t i = 0; i < total_blocks; i++)
        if (block_bits[i / 8] & (1u << (i % 8))) used_blocks++;
    for (uint64_t i = 0; i < inode_count; i++)
        if (inode_bits[i / 8] & (1u << (i % 8))) used_inodes++;

    printf("RavenFS_juvenile v1.1\n"
           "  fd=%d  mounted=%s\n"
           "  magic=%#llx  version=%u  uuid=%02x%02x..%02x%02x  label=%.48s\n"
           "  total_blocks=%llu  inode_count=%llu  root_inode=%llu\n"
           "  bitmap     [%llu, %llu)  %llu blk\n"
           "  inode arr  [%llu, %llu)  %llu blk\n"
           "  inode bm   [%llu, %llu)  %llu blk\n"
           "  blocks used: %llu/%llu (%.2f%%)\n"
           "  inodes used: %llu/%llu (%.2f%%)\n"
           "  root_dentry_blk=%llu  bm_cursor=%llu\n",
           fd, fd >= 0 ? "yes" : "no",
           (unsigned long long)sb.magic, sb.version,
           sb.uuid[0], sb.uuid[1], sb.uuid[14], sb.uuid[15],
           sb.volume_name,
           (unsigned long long)total_blocks, (unsigned long long)inode_count,
           (unsigned long long)root_inum,
           (unsigned long long)bitmap_start, (unsigned long long)total_blocks,
           (unsigned long long)bitmap_blocks,
           (unsigned long long)inode_array_base, (unsigned long long)bitmap_start,
           (unsigned long long)inode_blocks,
           (unsigned long long)inode_bm_base, (unsigned long long)inode_array_base,
           (unsigned long long)inode_bm_blocks,
           (unsigned long long)used_blocks, (unsigned long long)total_blocks,
           total_blocks ? 100.0 * (double)used_blocks / (double)total_blocks : 0.0,
           (unsigned long long)used_inodes, (unsigned long long)inode_count,
           inode_count ? 100.0 * (double)used_inodes / (double)inode_count : 0.0,
           (unsigned long long)root_dentry_blk, (unsigned long long)bm_cursor);
}

/* ═══════════════════════════════════════════════════════════════
 * 以下方法由 Sparrow 手写实现（dentry_tree.cpp）：
 *   name_hash / dentry_leaf_locate / dentry_leaf_shift /
 *   dentry_insert_recurse / dentry_remove_recurse / dentry_collect /
 *   root_dir_insert_dentry / root_dir_rm_dentry / root_dir_lookup /
 *   root_dir_readdir / dump_dentry_tree
 * ═══════════════════════════════════════════════════════════════ */

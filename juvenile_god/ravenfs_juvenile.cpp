/* ═══════════════════════════════════════════════════════════════════
 * ravenfs_juvenile.cpp — RavenFS v1.1 幼年期上帝类实现
 *
 * 本文件负责（Raven 分工）：
 *   构造/析构（挂载/卸载）、块 I/O、inode I/O、
 *   双位图分配器（block 组 + inode 组，全量内存 + write-through）
 *   CRC32、name_hash（XXH64）、dump_info
 *
 * 不实现（Sparrow 手写，见同目录下）：
 *   dentry_tree.cpp — dentry B+tree 全套（walk/lookup/insert/delete/split/balance）
 *   root_dir.cpp    — root_dir_insert_dentry / root_dir_rm_dentry
 *
 * 零缓存原则：所有块/inode 访问直接 pread/pwrite，无缓存层。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <new>
bool tmp_name_len_limiter(const char*name){
    return strlen(name)<=39;
}
bool is_extent_node(Bptree_meta_t *meta)
{
    return meta->magics[0]==BPTREE_MAGIC1&&
    meta->magics[1]==BPTREE_MAGIC2&&
    meta->magics[2]==BPTREE_MAGIC3&&
    meta->magics[3]==BPTREE_MAGIC4;
}
bool is_dentry_node(Bptree_meta_t *meta)
{
    return meta->magics[0]==DENTRY_MAGIC1&&
    meta->magics[1]==DENTRY_MAGIC2&&
    meta->magics[2]==DENTRY_MAGIC3&&
    meta->magics[3]==DENTRY_MAGIC4;
}
/* ═══════════════════════════════════════════════════════════════
 * 8K 节点临时缓冲轮子（node_buf_alloc / node_buf_free）
 * 内部节点临时分配必须 8K 对齐：结构体直读 + O_DIRECT 直写双满足。
 * ═══════════════════════════════════════════════════════════════ */
void *node_buf_alloc() {
    void *p = nullptr;
    if (posix_memalign(&p, 8192, 8192) != 0) return nullptr;
    return p;
}
void node_buf_free(void *p) { free(p); }
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
 * 名字 hash：XXH64（seed=0）— v1.1 §9 决策（2026-08-20）
 *
 * 实现：移植自 xxHash v0.5.1（/home/PS/PS_git/xxHash，Cyan4973/xxHash，
 * BSD 2-Clause），xxhash.c 的 XXH64_endian_align 精简版：
 *   - 删除 endian/align 宏分支：目标平台 x64 固定小端，读 64/32 位
 *     统一走 memcpy（天然处理非对齐，sparrowos 内核端可移植）
 *   - seed 硬编码 0（本函数签名无 seed 参数）
 *   - 无 XXH_ACCEPT_NULL_INPUT_POINTER 分支（name 恒非 NULL）
 * 算法本体（PRIME 常量、round/mergeRound、尾部分档）与官方逐行同构。
 *
 * 为什么不是 crc32（原临时方案）：
 *   - 只有 32 位熵：~77k 条目即出现生日碰撞；碰撞簇过大时叶子无法
 *     正确分裂（不变量 #2：hints[i].hash_max 严格递增，全同 hash 时
 *     左半最大 == 右半最大，separator 提升直接违反上界路由）
 *   - crc32 线性可构造：攻击者可批量构造同 crc32 名字灌爆单叶
 *   XXH64：64 位熵（~5e9 条目才期望首碰撞）、雪崩质量好、无 key、
 *   自包含、x64 极快（名字 ≤2047B 耗时无感）。
 *
 * 稳定性约束：hash 值落盘（每个 dentry 都存），跨挂载必须可复现，
 * 故 seed 固定 0，禁止按挂载随机 key。将来上网络服务（ravenfs_server）
 * 要抗 DoS 时：换 SipHash-2-4，key mkfs 生成一次存 Superblock
 * （pad 现成空间），接口签名不变。
 * ═══════════════════════════════════════════════════════════════ */
uint64_t RavenFS_juvenile::name_hash(const char *name) const {
    /* xxHash PRIME64 常量（xxhash.c 原值） */
    const uint64_t P1 = 11400714785074694791ULL;
    const uint64_t P2 = 14029467366897019727ULL;
    const uint64_t P3 = 1609587929392839161ULL;
    const uint64_t P4 = 9650029242287828579ULL;
    const uint64_t P5 = 2870177450012600261ULL;
    const uint64_t seed = 0;

    const uint8_t *p  = (const uint8_t *)name;
    const size_t   len = strlen(name);
    const uint8_t *bEnd = p + len;
    uint64_t h64;

    auto rotl64 = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
    auto round  = [&](uint64_t acc, uint64_t input) {
        acc += input * P2;
        acc = rotl64(acc, 31);
        acc *= P1;
        return acc;
    };
    auto mergeRound = [&](uint64_t &acc, uint64_t val) {
        val = round(0, val);
        acc ^= val;
        acc = acc * P1 + P4;
    };

    if (len >= 32) {
        const uint8_t *limit = bEnd - 32;
        uint64_t v1 = seed + P1 + P2;
        uint64_t v2 = seed + P2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - P1;
        do {
            uint64_t l1, l2, l3, l4;
            memcpy(&l1, p, 8);        p += 8;
            memcpy(&l2, p, 8);        p += 8;
            memcpy(&l3, p, 8);        p += 8;
            memcpy(&l4, p, 8);        p += 8;
            v1 = round(v1, l1);
            v2 = round(v2, l2);
            v3 = round(v3, l3);
            v4 = round(v4, l4);
        } while (p <= limit);

        h64 = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        mergeRound(h64, v1);
        mergeRound(h64, v2);
        mergeRound(h64, v3);
        mergeRound(h64, v4);
    } else {
        h64 = seed + P5;
    }

    h64 += (uint64_t)len;

    while (p + 8 <= bEnd) {
        uint64_t k1;
        memcpy(&k1, p, 8);
        k1 *= P2;
        k1 = rotl64(k1, 31);
        k1 *= P1;
        h64 ^= k1;
        h64 = rotl64(h64, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= bEnd) {
        uint32_t k2;
        memcpy(&k2, p, 4);
        h64 ^= (uint64_t)k2 * P1;
        h64 = rotl64(h64, 23) * P2 + P3;
        p += 4;
    }
    while (p < bEnd) {
        h64 ^= (uint64_t)(*p) * P5;
        h64 = rotl64(h64, 11) * P1;
        p++;
    }

    h64 ^= h64 >> 33;
    h64 *= P2;
    h64 ^= h64 >> 29;
    h64 *= P3;
    h64 ^= h64 >> 32;
    return h64;
}
/* ── 获取节点最大 hash（叶子末项 hash 或内部末项 hint.hash_max） ── */

RavenFS_juvenile::RavenFS_juvenile(const linux_userspace_mount_t &mnt, uint64_t *res_code) {
    if (res_code) *res_code = RC_ERR_OPEN;   /* 默认失败态 */

    /* 无状态消费：全部参数来自调用者结构体，类不做设备几何的内部推导/校验
     * （设备探测是外部职责）。fd 生命周期归外部：只借不关。 */
    fd = mnt.dev.fd;
    param = mnt.param;
    dev_type = mnt.dev.type;
    lba_size_log2 = mnt.dev.lba_size_log2;
    is_device_direct = mnt.dev.is_device_direct;

    /* 契约（结构体自述）：readonly（rfs-audit 静态 mmap 审核）不得配 O_DIRECT */
    if (param.is_readonly && is_device_direct) {
        if (res_code) *res_code = RC_ERR_STATE;
        return;
    }

    /* ── 读 Primary Superblock（fd byte[0,4096)，block_model §4.1）── */
    if (pread(fd, &sb, sizeof(sb), 0) != (ssize_t)sizeof(sb)) {
        fd = -1;
        if (res_code) *res_code = RC_ERR_IO;
        return;
    }
    if (sb.magic != SUPERBLOCK_MAGIC) {
        fd = -1;
        if (res_code) *res_code = RC_ERR_MAGIC;
        return;
    }
    if (sb.version != RAVENFS_VERSION) {
        fd = -1;
        if (res_code) *res_code = RC_ERR_VERSION;
        return;
    }
    {
        RavenSuperblock sb_copy = sb;
        sb_copy.crc32 = 0;
        if (sb.crc32 != crc32_compute(&sb_copy, sizeof(sb_copy))) {
            fd = -1;
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
        fd = -1;
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
        fd = -1;
        if (res_code) *res_code = RC_ERR_STATE;
        return;
    }
    root_dentry_blk = root_inode.data_root;

    if (res_code) *res_code = RC_OK;
}

/* 析构即卸载：双位图全量写回（readonly 跳过）→ 释放内存。
 * fd 生命周期归外部（借入不关）。 */
RavenFS_juvenile::~RavenFS_juvenile() {
    if (!param.is_readonly && fd >= 0 && block_bits && inode_bits)
        bitmaps_flush();
    delete[] block_bits; block_bits = nullptr;
    delete[] inode_bits; inode_bits = nullptr;
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

/* 批量块 I/O（零缓存）：blk 起连续 blk_len 个 FS_block，一次 pread/pwrite。
 * 参数非法（buf 为空 / 区间越界）→ RC_ERR_STATE；实际读写字节不符 → RC_ERR_IO。 */
result_code_t RavenFS_juvenile::read_buff(uint64_t blk, uint64_t blk_len, void *buf) {
    if (!buf || blk_len == 0) return RC_ERR_STATE;
    if (blk >= total_blocks || blk_len > total_blocks - blk) return RC_ERR_STATE;
    size_t bytes = blk_len * 4096;
    if (pread(fd, buf, bytes, (off_t)fblock_to_byte(blk)) != (ssize_t)bytes)
        return RC_ERR_IO;
    return RC_OK;
}

result_code_t RavenFS_juvenile::write_buff(uint64_t blk, uint64_t blk_len, void *buf) {
    if (!buf || blk_len == 0) return RC_ERR_STATE;
    if (blk >= total_blocks || blk_len > total_blocks - blk) return RC_ERR_STATE;
    size_t bytes = blk_len * 4096;
    if (pwrite(fd, buf, bytes, (off_t)fblock_to_byte(blk)) != (ssize_t)bytes)
        return RC_ERR_IO;
    return RC_OK;
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
 * 以下方法由 人类手写实现（dentry_tree.cpp）：
 *   root_dir_insert_dentry / root_dir_rm_dentry / root_dir_lookup /
 *   root_dir_readdir
 * ═══════════════════════════════════════════════════════════════ */
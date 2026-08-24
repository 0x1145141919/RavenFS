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

    /* ── 分片初始化：slice_count 计算 + 双组逐片 enable_slice ── */
    if (!slices_setup(mnt.logical_processor_count)) {
        free(block_bits); block_bits = nullptr;
        free(inode_bits); inode_bits = nullptr;
        fd = -1;
        if (res_code) *res_code = RC_ERR_STATE;
        return;
    }

    /* ── root inode：取 dentry 树根块号 ── */
    root_inum = sb.root_inode;
    raven_Inode_t root_inode;
    inode_read(root_inum, &root_inode);
    if (root_inode.type != INODE_TYPE_DIR || root_inode.data_root == PTR_INVALID) {
        slices_teardown();
        free(block_bits); block_bits = nullptr;
        free(inode_bits); inode_bits = nullptr;
        fd = -1;
        if (res_code) *res_code = RC_ERR_STATE;
        return;
    }
    root_dentry_blk = root_inode.data_root;

    if (res_code) *res_code = RC_OK;
}

/* 析构即卸载：双位图全量写回（readonly 跳过）→ 拆片 → 释放位图内存。
 * fd 生命周期归外部（借入不关）。 */
RavenFS_juvenile::~RavenFS_juvenile() {
    if (!param.is_readonly && fd >= 0 && block_bits && inode_bits)
        bitmaps_flush();
    slices_teardown();
    free(block_bits); block_bits = nullptr;
    free(inode_bits); inode_bits = nullptr;
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

/* mount：读入双位图（4K 对齐分配）+ 自描述校验（block_model §6.6）+ 填 descriptor
 * 校验点：bitmap 自身区域 [bitmap_start, total_blocks) 全 1
 *         inode 元数据区域 [inode_bm_base, bitmap_start) 全 1
 *         inode 位图 bit0（root inode）必须为 1 */
bool RavenFS_juvenile::bitmaps_load() {
    if (posix_memalign((void **)&block_bits, 4096, bitmap_blocks * 4096) != 0)
        return false;
    if (posix_memalign((void **)&inode_bits, 4096, inode_bm_blocks * 4096) != 0) {
        free(block_bits); block_bits = nullptr;
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

    /* 填 descriptor：bit i ↔ FS_block[i] / inode[i]（FS_blk_base=0） */
    block_bitsmap_descriptor.FS_blk_base = 0;
    block_bitsmap_descriptor.bitmap_raw = reinterpret_cast<uint64_t *>(block_bits);
    block_bitsmap_descriptor.bits_count = bitmap_blocks * 4096 * 8;
    inode_bitsmap_descriptor.FS_blk_base = 0;
    inode_bitsmap_descriptor.bitmap_raw = reinterpret_cast<uint64_t *>(inode_bits);
    inode_bitsmap_descriptor.bits_count = inode_bm_blocks * 4096 * 8;

    return true;

fail:
    free(block_bits); block_bits = nullptr;
    free(inode_bits); inode_bits = nullptr;
    return false;
}

/* 全量写回：双位图各 pwrite 一次（析构时调用；分配/释放只改内存态） */
void RavenFS_juvenile::bitmaps_flush() {
    pwrite(fd, block_bits, bitmap_blocks * 4096,
           (off_t)fblock_to_byte(bitmap_start));
    pwrite(fd, inode_bits, inode_bm_blocks * 4096,
           (off_t)fblock_to_byte(inode_bm_base));
}

/* ═══════════════════════════════════════════════════════════════
 * 分片初始化 / 拆除
 * ═══════════════════════════════════════════════════════════════ */

/* 分片初始化：slice_count = min(2^n 上界(cpu), 双位图满片数约束)，
 * 且保证末片非空（(n-1)*per < nbits 不满足则 n 减半，n 保持 2 幂）。
 * 每片 slice_bitscount = 名义片长（32768 倍数，末片可短），
 * slice_base 4K 对齐（begin/8 = 4096 倍数）；逐片 enable_slice。
 * 失败 → false（内部已清理 slices 数组）。 */
bool RavenFS_juvenile::slices_setup(uint32_t cpu_count) {
    const uint64_t blk_nbits = block_bitsmap_descriptor.bits_count;
    const uint64_t ino_nbits = inode_bitsmap_descriptor.bits_count;

    /* 2^n 上界(cpu) */
    uint64_t n = 1;
    while (n < cpu_count && n < (1ULL << 30)) n <<= 1;
    /* 满片数约束：每片 ≥ 32768 bit（4K 对齐下限） */
    uint64_t blk_cap = blk_nbits / 32768, ino_cap = ino_nbits / 32768;
    uint64_t cap = blk_cap < ino_cap ? blk_cap : ino_cap;
    if (cap == 0) cap = 1;
    if (n > cap) n = cap;
    /* 末片非空迭代 */
    for (;;) {
        uint64_t per_b = ((blk_nbits + n - 1) / n + 32767) / 32768 * 32768;
        uint64_t per_i = ((ino_nbits + n - 1) / n + 32767) / 32768 * 32768;
        if (per_b == 0) per_b = 32768;
        if (per_i == 0) per_i = 32768;
        if ((n - 1) * per_b < blk_nbits && (n - 1) * per_i < ino_nbits) break;
        if (n == 1) break;
        n >>= 1;
    }
    slice_count = (uint32_t)n;

    try {
        block_bitmap_slices = new bitmap_slice_t[slice_count];
        inode_bitmap_slices = new bitmap_slice_t[slice_count];
    } catch (const std::bad_alloc &) {
        block_bitmap_slices = nullptr;
        inode_bitmap_slices = nullptr;
        return false;
    }

    /* block 组切分 */
    block_per_slice_bits = ((blk_nbits + slice_count - 1) / slice_count + 32767) / 32768 * 32768;
    if (block_per_slice_bits == 0) block_per_slice_bits = 32768;
    for (uint64_t i = 0; i < slice_count; i++) {
        uint64_t begin = i * block_per_slice_bits;
        uint64_t cnt = blk_nbits - begin;
        if (cnt > block_per_slice_bits) cnt = block_per_slice_bits;
        slice_config_t cfg;
        cfg.slice_base = block_bits + begin / 8;
        cfg.slice_bitscount = cnt;
        if (block_bitmap_slices[i].enable_slice(&block_bitsmap_descriptor, cfg) != RC_OK)
            goto fail;
    }
    /* inode 组切分；片 0 的 hint 跳过 bit 0（root inode 保留） */
    inode_per_slice_bits = ((ino_nbits + slice_count - 1) / slice_count + 32767) / 32768 * 32768;
    if (inode_per_slice_bits == 0) inode_per_slice_bits = 32768;
    for (uint64_t i = 0; i < slice_count; i++) {
        uint64_t begin = i * inode_per_slice_bits;
        uint64_t cnt = ino_nbits - begin;
        if (cnt > inode_per_slice_bits) cnt = inode_per_slice_bits;
        slice_config_t cfg;
        cfg.slice_base = inode_bits + begin / 8;
        cfg.slice_bitscount = cnt;
        if (inode_bitmap_slices[i].enable_slice(&inode_bitsmap_descriptor, cfg) != RC_OK)
            goto fail;
    }
    if (slice_count > 0) inode_bitmap_slices[0].set_hint(1);   /* 跳过 root */
    return true;

fail:
    delete[] block_bitmap_slices; block_bitmap_slices = nullptr;
    delete[] inode_bitmap_slices; inode_bitmap_slices = nullptr;
    return false;
}

void RavenFS_juvenile::slices_teardown() {
    delete[] block_bitmap_slices; block_bitmap_slices = nullptr;
    delete[] inode_bitmap_slices; inode_bitmap_slices = nullptr;
}

/* ═══════════════════════════════════════════════════════════════
 * 位图接口组 A：FS_block 分配器（分片路由）
 *
 * 锁纪律：分配走 try（拿不到锁换片，仿 FPA::alloc spintrylock_try_guard）；
 * 释放走自旋重试（释放不可丢，仿 FPA::free spintrylock_spin_guard）。
 * ═══════════════════════════════════════════════════════════════ */

/* 硬分配：count 连续 + 对齐，单片内完成（不跨片）；失败 PTR_INVALID */
uint64_t RavenFS_juvenile::block_alloc_hard(uint64_t require_count, uint8_t align_log2) {
    if (require_count == 0 || !block_bitmap_slices || slice_count == 0) return PTR_INVALID;
    for (uint64_t round = 0; round < slice_count; round++) {
        uint64_t idx = (block_preferred_slice + round) % slice_count;
        bitmap_slice_t &s = block_bitmap_slices[idx];
        if (!s.slice_lock.try_lock_write()) continue;      /* 锁忙换片 */
        uint64_t out = 0;
        result_code_t rc = s.alloc_hard(align_log2, require_count, out);
        s.slice_lock.unlock_write();
        if (rc == RC_OK) { block_preferred_slice = idx; return out; }
    }
    return PTR_INVALID;
}

/* 软分配：一次只从单片取（preferred 优先），实际 = min(区间长, require)；
 * *remain_count = 缺口数；一块没有 → PTR_INVALID。 */
uint64_t RavenFS_juvenile::block_alloc_soft(uint64_t require_count, uint64_t *remain_count) {
    if (remain_count) *remain_count = require_count;
    if (require_count == 0 || !block_bitmap_slices || slice_count == 0) return PTR_INVALID;
    for (uint64_t round = 0; round < slice_count; round++) {
        uint64_t idx = (block_preferred_slice + round) % slice_count;
        bitmap_slice_t &s = block_bitmap_slices[idx];
        if (!s.slice_lock.try_lock_write()) continue;
        uint64_t out = 0, left = require_count;
        result_code_t rc = s.alloc_soft(0, require_count, out, left);
        s.slice_lock.unlock_write();
        if (rc == RC_OK) {
            block_preferred_slice = idx;
            if (remain_count) *remain_count = left;
            return out;
        }
    }
    return PTR_INVALID;
}

/* 区间释放：定位所属片（blkbase / 名义片长）→ 自旋拿写锁 → 清位 */
void RavenFS_juvenile::block_free(uint64_t blkbase, uint64_t count) {
    if (count == 0 || !block_bitmap_slices || slice_count == 0) return;
    uint64_t idx = blkbase / block_per_slice_bits;
    if (idx >= slice_count) return;
    bitmap_slice_t &s = block_bitmap_slices[idx];
    while (!s.slice_lock.try_lock_write()) { /* 释放不可丢，自旋等 */ }
    s.free_interval(blkbase, count);
    s.slice_lock.unlock_write();
}

bool RavenFS_juvenile::block_test(uint64_t blk) const {
    return (block_bits[blk / 8] & (1u << (blk % 8))) != 0;
}

/* ═══════════════════════════════════════════════════════════════
 * 位图接口组 B：inode 槽分配器（分片路由）
 * ═══════════════════════════════════════════════════════════════ */
uint64_t RavenFS_juvenile::inode_alloc() {
    if (!inode_bitmap_slices || slice_count == 0) return PTR_INVALID;
    for (uint64_t round = 0; round < slice_count; round++) {
        uint64_t idx = (inode_preferred_slice + round) % slice_count;
        bitmap_slice_t &s = inode_bitmap_slices[idx];
        if (!s.slice_lock.try_lock_write()) continue;
        uint64_t out = 0, left = 0;
        result_code_t rc = s.alloc_soft(0, 1, out, left);
        s.slice_lock.unlock_write();
        if (rc == RC_OK) { inode_preferred_slice = idx; return out; }
    }
    return PTR_INVALID;
}

void RavenFS_juvenile::inode_free(uint64_t inum) {
    if (!inode_bitmap_slices || slice_count == 0) return;
    uint64_t idx = inum / inode_per_slice_bits;
    if (idx >= slice_count) return;
    bitmap_slice_t &s = inode_bitmap_slices[idx];
    while (!s.slice_lock.try_lock_write()) { /* 释放不可丢，自旋等 */ }
    s.free_interval(inum, 1);
    s.slice_lock.unlock_write();
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
           "  root_dentry_blk=%llu  slices=%u  blk_pref=%llu\n",
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
           (unsigned long long)root_dentry_blk, slice_count,
           (unsigned long long)block_preferred_slice);
}

/* ═══════════════════════════════════════════════════════════════
 * 以下方法由 人类手写实现（dentry_tree.cpp）：
 *   root_dir_insert_dentry / root_dir_rm_dentry / root_dir_lookup /
 *   root_dir_readdir
 * ═══════════════════════════════════════════════════════════════ */
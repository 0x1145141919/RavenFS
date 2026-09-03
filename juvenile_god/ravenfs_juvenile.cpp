/* ═══════════════════════════════════════════════════════════════════
 * ravenfs_juvenile.cpp — RavenFS v1.1 幼年期上帝类实现
 *
 * 本文件负责（Raven 分工）：
 *   构造/析构（挂载/卸载）、块 I/O、inode I/O、
 *   位图原始内存加载/写回 + 装配 bitmap_slices_mgr_t（分配器本体已顶出
 *   到 bitmap_slices.cpp，2026-09-03）
 *   CRC32、name_hash（XXH64）、dump_info
 *
 * 不实现（Sparrow 手写，见同目录下）：
 *   dentry_tree.cpp — dentry B+tree 全套（walk/lookup/insert/delete/split/balance）
 *   root_dir.cpp    — root_dir_insert_dentry / root_dir_rm_dentry
 *
 * 零缓存原则：所有块/inode 访问直接 pread/pwrite，无缓存层。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"   /* Inode_cache_cfg_t_v2：编排构造（头里仅前置声明） */
#include "../include/ravenfs/mdata_piece_caches.hpp"  /* mdata_piece_caches_cfg_t：编排构造 */
#include "../include/ravenfs/bitmap_slices.hpp"  /* bitmap_slices_cfg_t / bitmap_slices_mgr_t：位图模块（2026-09-03 顶出） */
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>   /* mmap/munmap：非直写后端整盘映射 + offload canvas 切片 */
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <new>
extern "C" uint32_t get_processor_id();

bool name_len_limiter(const char*name){
    return strlen(name)<=RAVENFS_MAX_NAME_LEN;
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

    /* ── 读 Primary Superblock（fd byte[0,4096)，block_model §4.1）──
     * O_DIRECT 契约：4Kn 设备要求长度/缓冲 4096 对齐 → 整 4K 读入对齐缓冲，
     * 再拷前 sizeof(sb) 字节（SB 区固定 4096，长度按结构体实际尺寸）。 */
    alignas(4096) uint8_t sb_raw[4096];
    if (pread(fd, sb_raw, 4096, 0) != 4096) {
        fd = -1;
        if (res_code) *res_code = RC_ERR_IO;
        return;
    }
    memcpy(&sb, sb_raw, sizeof(sb));
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

    /* ── 决策树第一层：mmap 后端（非直写）/ 直写模式 ──
     * 非直写（普通文件/回环镜像/rfs-audit）：整盘从 offset 0 mmap（含 SB 区
     * byte[0,4096)，FS_block[n] = base+4096+n×4096），位图与缓存 canvas
     * 全部从窗口切片，不刻意预取（内核按需调页）。
     * readonly 配 MAP_PRIVATE+PROT_READ（写不落盘）；否则 MAP_SHARED 读写。 */
    if (!is_device_direct) {
        const off_t sz = lseek(fd, 0, SEEK_END);   /* 文件/块设备均返回尺寸 */
        if (sz <= 0) {
            fd = -1;
            if (res_code) *res_code = RC_ERR_STATE;
            return;
        }
        mmap_size_ = (uint64_t)sz;
        const int prot  = param.is_readonly ? PROT_READ : (PROT_READ | PROT_WRITE);
        const int flags = param.is_readonly ? MAP_PRIVATE : MAP_SHARED;
        mmap_base_ = (uint8_t *)mmap(nullptr, mmap_size_, prot, flags, fd, 0);
        if (mmap_base_ == MAP_FAILED) {
            mmap_base_ = nullptr;
            mmap_size_ = 0;
            fd = -1;
            if (res_code) *res_code = RC_ERR_STATE;
            return;
        }
        fsblk0_base_ = mmap_base_ + 4096;   /* FS_block[0] 虚拟基址（Primary SB 占 byte[0,4096)，§3.1） */
    }

    /* ctor 失败统一清理：按后端释放（mmap munmap / 直写 free）+ 子模块/分片拆除 */
    auto fail = [&](uint64_t code) {
        caches_teardown();
        delete bitmap_mgr; bitmap_mgr = nullptr;
        if (mmap_base_) {
            munmap(mmap_base_, mmap_size_);
            mmap_base_ = nullptr;
            mmap_size_ = 0;
            fsblk0_base_ = nullptr;
        } else {
            free(block_bits);
            free(inode_bits);
        }
        block_bits = nullptr;
        inode_bits = nullptr;
        fd = -1;
        if (res_code) *res_code = code;
    };

    /* ── 位图：直写=全量 pread 入内存；mmap 后端=窗口切片（不预取） ── */
    if (!bitmaps_load()) { fail(RC_ERR_BITMAP_SELF); return; }

    /* ── 双位图分片分配器装配（bitmap_slices_mgr_t，2026-09-03 顶出）──
     * 位图原始内存（block_bits/inode_bits）生命周期归本类（直写 posix_memalign
     * / mmap 窗口切片）；mgr 只认内存：装配 = 几何推导 + 双组切片 enable。 */
    {
        bitmap_slices_cfg_t bcfg;
        bcfg.block_bits_raw   = block_bits;
        bcfg.block_bits_count = bitmap_blocks * 4096 * 8;
        bcfg.inode_bits_raw   = inode_bits;
        bcfg.inode_bits_count = inode_bm_blocks * 4096 * 8;
        bcfg.logical_processor_count = mnt.logical_processor_count;
        uint64_t brc = RC_ERR_STATE;
        bitmap_mgr = new bitmap_slices_mgr_t(bcfg, &brc);
        if (brc != RC_OK) { delete bitmap_mgr; bitmap_mgr = nullptr; fail(RC_ERR_STATE); return; }
    }

    /* ── 缓存子模块编排（决策树第二层：offload / self_reliat / no_cache） ── */
    {
        result_code_t rc = caches_setup();
        if (rc != RC_OK) { fail(rc); return; }
    }

    /* ── root inode：取 dentry 树根块号 ── */
    root_inum = sb.root_inode;
    raven_Inode_t root_inode;
    inode_read(root_inum, &root_inode);
    if (root_inode.type != INODE_TYPE_DIR || root_inode.data_root == PTR_INVALID) {
        fail(RC_ERR_STATE);
        return;
    }
    root_dentry_blk = root_inode.data_root;

    if (res_code) *res_code = RC_OK;
}

/* 析构即卸载：缓存同步点（flush_all → delete）→ 位图写回（仅直写模式）→
 * 拆片 → 释放后端（mmap munmap / 直写 free）。
 * readonly 不 flush（mmap MAP_PRIVATE/PROT_READ 下写回本就禁止）。
 * fd 生命周期归外部（借入不关）。 */
RavenFS_juvenile::~RavenFS_juvenile() {
    caches_teardown();
    if (!param.is_readonly && fd >= 0 && block_bits && inode_bits)
        bitmaps_flush();
    delete bitmap_mgr; bitmap_mgr = nullptr;
    if (mmap_base_) {
        munmap(mmap_base_, mmap_size_);   /* mmap 后端：位图/缓存全在窗口内，一次释放 */
        mmap_base_ = nullptr;
        mmap_size_ = 0;
        fsblk0_base_ = nullptr;
    } else {
        free(block_bits);
        free(inode_bits);
    }
    block_bits = nullptr;
    inode_bits = nullptr;
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

result_code_t RavenFS_juvenile::write_buff(uint64_t blk, uint64_t blk_len, const void *buf) {
    if (!buf || blk_len == 0) return RC_ERR_STATE;
    if (blk >= total_blocks || blk_len > total_blocks - blk) return RC_ERR_STATE;
    size_t bytes = blk_len * 4096;
    if (pwrite(fd, buf, bytes, (off_t)fblock_to_byte(blk)) != (ssize_t)bytes)
        return RC_ERR_IO;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 缓存回调适配器（static 成员，可访问私有 read_buff/write_buff）
 * 两个子模块回调签名一致（(io_ctx, blk, blk_len, buf)），共用一对。
 * io_ctx = this（ctor 组装 cfg 时传入）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::fs_io_read(void *io_ctx, uint64_t blk, uint64_t blk_len, void *buf) {
    return static_cast<RavenFS_juvenile *>(io_ctx)->read_buff(blk, blk_len, buf);
}
result_code_t RavenFS_juvenile::fs_io_write(void *io_ctx, uint64_t blk, uint64_t blk_len, const void *buf) {
    return static_cast<RavenFS_juvenile *>(io_ctx)->write_buff(blk, blk_len, buf);
}

/* ═══════════════════════════════════════════════════════════════
 * caches_setup — 缓存子模块初始化编排（决策树第二层）
 *
 * 第一层 is_device_direct（ctor 已定）：
 *   mmap 后端（非直写）：
 *     - inode 缓存 offload：canvas = inode array 起始 FS_block 的窗口切片
 *     - mdata_piece 缓存 offload：canvas = FS_block[0] 的窗口切片（数据区 base=0）
 *     - 数据不落用户态，全由内核页缓存管；位图 ctor 已从窗口切片（不预取）
 *  直写模式：
 *     - inode 缓存：param.if_will_use_inode_cache ? self_reliat : no_cache
 *     - mdata_piece：param.if_will_use_mdata_piece_cache ? self_reliat : no_cache
 *       （self_reliat 未实装 → ctor 拒 RC_ERR_STATE，挂载失败；TODO 实装后自然可用）
 * 失败：已建子模块清理后返回错误码。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::caches_setup() {
    result_code_t rc = RC_OK;

    /* ── inode 缓存子模块 ── */
    Inode_cache_cfg_t_v2 icfg{};
    icfg.read_line  = fs_io_read;
    icfg.write_line = fs_io_write;
    icfg.io_ctx     = this;
    icfg.Inode_count = inode_count;
    icfg.Inode_base_FS_block_idx = inode_array_base;
    if (!is_device_direct) {
        icfg.mode = cache_work_mode_t::offload_cache;
        icfg.offload_specific.virtual_canvas_base =
            (uint64_t)(fsblk0_base_ + inode_array_base * 4096);   /* FS_block[inode_array_base]（inode array 起点） */
    } else if (param.if_will_use_inode_cache) {
        icfg.mode = cache_work_mode_t::self_reliat_cache;
        icfg.self_reliat_specific.ttl_bare_seconds = 0;   /* 0 = ctor 按 Invariant TSC 换算默认 2s */
    } else {
        icfg.mode = cache_work_mode_t::no_cache;
    }
    inode_cache_mgr = new Inode_cache_mgr_t(icfg, &rc);
    if (rc != RC_OK) { delete inode_cache_mgr; inode_cache_mgr = nullptr; return rc; }

    /* ── mdata_piece 缓存子模块（8K 元数据负载片） ── */
    mdata_piece_caches_cfg_t mcfg{};
    mcfg.read_block  = fs_io_read;
    mcfg.write_block = fs_io_write;
    mcfg.io_ctx      = this;
    mcfg.Piece_base_FS_block_idx = 0;      /* 数据区起点（元数据全在尾巴，block_model §3.2） */
    mcfg.Piece_count = inode_bm_base / 2;  /* 覆盖数据区 [0, inode_bm_base)，每片 2 连续块 */
    if (!is_device_direct) {
        mcfg.mode = cache_work_mode_t::offload_cache;
        mcfg.offload_specific.virtual_canvas_base =
            (uint64_t)fsblk0_base_;   /* FS_block[0]（数据区 base=0） */
    } else if (param.if_will_use_mdata_piece_cache) {
        mcfg.mode = cache_work_mode_t::self_reliat_cache;   /* 双档：CTRL_ARRAY ≤13 / RADIX >13（mdata_piece_cache.cpp） */
    } else {
        mcfg.mode = cache_work_mode_t::no_cache;
    }
    mdata_piece_mgr = new mdata_piece_caches_mgr(mcfg, &rc);
    if (rc != RC_OK) {
        delete mdata_piece_mgr;
        mdata_piece_mgr = nullptr;
        delete inode_cache_mgr;   /* 拆已建子模块 */
        inode_cache_mgr = nullptr;
        return rc;
    }

    return RC_OK;
}

/* caches_teardown — 卸载：同步点（flush_all）→ delete 子模块 → 指针置空。
 * readonly 不 flush（mmap MAP_PRIVATE/PROT_READ 下 msync 无意义，写回本就禁止）。 */
void RavenFS_juvenile::caches_teardown() {
    if (!param.is_readonly) {
        if (inode_cache_mgr)  inode_cache_mgr->flush_all();
        if (mdata_piece_mgr)  mdata_piece_mgr->flush_all();
    }
    delete inode_cache_mgr;
    inode_cache_mgr = nullptr;
    delete mdata_piece_mgr;
    mdata_piece_mgr = nullptr;
}

/* ═══════════════════════════════════════════════════════════════
 * inode I/O（inode array 从 inode_array_base 块起线性排布，16 inode/块）
 *
 * O_DIRECT 契约：256B 直读/直写非法（长度/缓冲须对齐逻辑块）→
 *   直写模式整块 4K 读-改-写（写时读回整块再改目标 256B）；
 *   mmap 后端直接读写窗口切片（零拷贝，内核页缓存管）。
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::inode_read(uint64_t inum, raven_Inode_t *out) const {
    /* self_reliat 后端：读必须走缓存 —— 业务/测试的读-改-写经 InodeSession
     * 落在用户态私有副本，直读盘会读到旧值（offload 直映页缓存 / no_cache
     * 直读盘均无此不一致；2026-08-30 分裂建新根 data_root 丢失即此坑）。 */
    if (inode_cache_mgr &&
        inode_cache_mgr->backed_mode == cache_work_mode_t::self_reliat_cache) {
        Inode_cache_mgr_t::slice_cache_acces_token_t tok{};
        raven_Inode_t *p = inode_cache_mgr->self_reliat_get(inum, &tok);
        if (p) {
            *out = *p;
            inode_cache_mgr->self_reliat_put(inum, tok);
            return;
        }
        /* 缓存读失败（退化配置）：回落到直读 */
    }
    if (fsblk0_base_) {   /* mmap 后端：直接 memcpy 自窗口（FS_block[inode_array_base] 起） */
        memcpy(out, fsblk0_base_ + inode_array_base * 4096 + inum * sizeof(raven_Inode_t),
               sizeof(raven_Inode_t));
        return;
    }
    alignas(4096) uint8_t blk[4096];   /* O_DIRECT：整块 4K 读，再取目标 256B */
    const uint64_t line = inum >> 4;
    pread(fd, blk, 4096, (off_t)(fblock_to_byte(inode_array_base) + line * 4096));
    memcpy(out, blk + (inum & 15) * sizeof(raven_Inode_t), sizeof(raven_Inode_t));
}

void RavenFS_juvenile::inode_write(uint64_t inum, const raven_Inode_t *in) {
    /* self_reliat 后端：写必须走缓存（改缓存副本 + 脏标记，flush/dtor 写回） */
    if (inode_cache_mgr &&
        inode_cache_mgr->backed_mode == cache_work_mode_t::self_reliat_cache) {
        Inode_cache_mgr_t::slice_cache_acces_token_t tok{};
        raven_Inode_t *p = inode_cache_mgr->self_reliat_get(inum, &tok);
        if (p) {
            *p = *in;
            inode_cache_mgr->self_reliat_mark_dirty(inum);
            inode_cache_mgr->self_reliat_put(inum, tok);
            return;
        }
        /* 缓存写失败（退化配置）：回落到直写 */
    }
    if (fsblk0_base_) {   /* mmap 后端：直接写窗口切片 */
        memcpy(fsblk0_base_ + inode_array_base * 4096 + inum * sizeof(raven_Inode_t),
               in, sizeof(raven_Inode_t));
        return;
    }
    alignas(4096) uint8_t blk[4096];   /* O_DIRECT：整块 4K 读-改-写 */
    const uint64_t line = inum >> 4;
    const off_t off = (off_t)(fblock_to_byte(inode_array_base) + line * 4096);
    pread(fd, blk, 4096, off);
    memcpy(blk + (inum & 15) * sizeof(raven_Inode_t), in, sizeof(raven_Inode_t));
    pwrite(fd, blk, 4096, off);
}

/* ═══════════════════════════════════════════════════════════════
 * 位图核心
 * ═══════════════════════════════════════════════════════════════ */

/* mount：位图入内存/切片 + 自描述校验（block_model §6.6）+ 填 descriptor
 * 直写模式：全量 pread（4K 对齐分配，O_DIRECT 兼容）
 * mmap 后端：直接从窗口切片，不预取（内核按需调页）；切片 4K 对齐
 *   （位图区起始 FS_block × 4096 + SB 区 4096，恒 4096 倍数）
 * 校验点：bitmap 自身区域 [bitmap_start, total_blocks) 全 1
 *         inode 元数据区域 [inode_bm_base, bitmap_start) 全 1
 *         inode 位图 bit0（root inode）必须为 1 */
bool RavenFS_juvenile::bitmaps_load() {
    if (is_device_direct) {
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
    } else {
        /* mmap 后端：位图直接从窗口切片，不预取（内核按需调页）。
         * 切片 4K 对齐：位图区起始 FS_block × 4096（fsblk0_base_ 已含 SB 区 4096）。 */
        block_bits = fsblk0_base_ + bitmap_start * 4096;
        inode_bits = fsblk0_base_ + inode_bm_base * 4096;
    }

    /* 自描述校验（两模式共用；mmap 模式直接校验内核页缓存视图） */
    for (uint64_t i = bitmap_start; i < total_blocks; i++)
        if (!(block_bits[i / 8] & (1u << (i % 8)))) goto fail;
    for (uint64_t i = inode_bm_base; i < bitmap_start; i++)
        if (!(block_bits[i / 8] & (1u << (i % 8)))) goto fail;
    if (!(inode_bits[0] & 1u)) goto fail;   /* root inode 亮灯 */


    return true;

fail:
    /* mmap 模式：指针指向窗口切片，不能 free（munmap 由 ctor 失败路径/dtor 负责） */
    if (is_device_direct) {
        free(block_bits);
        free(inode_bits);
    }
    block_bits = nullptr;
    inode_bits = nullptr;
    return false;
}

/* 全量写回：仅直写模式 pwrite（析构时调用；分配/释放只改内存态）。
 * mmap 后端由内核页缓存管（MAP_SHARED 自动写回），无需显式 pwrite。 */
void RavenFS_juvenile::bitmaps_flush() {
    if (!is_device_direct) return;
    pwrite(fd, block_bits, bitmap_blocks * 4096,
           (off_t)fblock_to_byte(bitmap_start));
    pwrite(fd, inode_bits, inode_bm_blocks * 4096,
           (off_t)fblock_to_byte(inode_bm_base));
}

/* ═══════════════════════════════════════════════════════════════
 * 位图接口组 A/B：薄转发 → bitmap_slices_mgr_t（独立模块）
 * 完整契约/锁纪律/几何推导见 include/ravenfs/bitmap_slices.hpp；
 * 本层仅保留签名不变的转发壳，调用点零改动（2026-09-03 顶出）。
 * ═══════════════════════════════════════════════════════════════ */
uint64_t RavenFS_juvenile::block_alloc_hard(uint64_t require_count, uint8_t align_log2) {
    return bitmap_mgr ? bitmap_mgr->block_alloc_hard(require_count, align_log2) : PTR_INVALID;
}
uint64_t RavenFS_juvenile::block_alloc_soft(uint64_t require_count, uint64_t *remain_count) {
    if (remain_count) *remain_count = require_count;
    return bitmap_mgr ? bitmap_mgr->block_alloc_soft(require_count, remain_count) : PTR_INVALID;
}
uint64_t RavenFS_juvenile::block_occupy_soft(uint64_t blkbase, uint64_t require_count,
                                             uint64_t *rest_count) {
    if (rest_count) *rest_count = require_count;
    return bitmap_mgr ? bitmap_mgr->block_occupy_soft(blkbase, require_count, rest_count) : PTR_INVALID;
}
void RavenFS_juvenile::block_free(uint64_t blkbase, uint64_t count) {
    if (bitmap_mgr) bitmap_mgr->block_free(blkbase, count);
}
bool RavenFS_juvenile::block_test(uint64_t blk) const {
    return bitmap_mgr ? bitmap_mgr->block_test(blk) : false;
}
uint64_t RavenFS_juvenile::inode_alloc() {
    return bitmap_mgr ? bitmap_mgr->inode_alloc() : PTR_INVALID;
}
void RavenFS_juvenile::inode_free(uint64_t inum) {
    if (bitmap_mgr) bitmap_mgr->inode_free(inum);
}
bool RavenFS_juvenile::inode_test(uint64_t inum) const {
    return bitmap_mgr ? bitmap_mgr->inode_test(inum) : false;
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
           "  root_dentry_blk=%llu  slices=%u  cpu_slots=%u  blk_pref0=%llu\n",
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
           (unsigned long long)root_dentry_blk,
           (unsigned)bitmap_mgr->slice_count(), (unsigned)bitmap_mgr->cpu_slots(),
           (unsigned long long)bitmap_mgr->block_preferred_of(0));
}

/* ═══════════════════════════════════════════════════════════════
 * 以下方法由 人类手写实现（dentry_tree.cpp）：
 *   root_dir_insert_dentry / root_dir_rm_dentry / root_dir_lookup /
 *   root_dir_readdir
 * ═══════════════════════════════════════════════════════════════ */
/* ═══════════════════════════════════════════════════════════════════
 * test_fs_allocator_router.cpp — fs_allocator_t 路由层单测（2026-10-03；2026-10-06 v2.0 改）
 *
 * 靶场：4K 对齐的 **block** 原始位图（先全 1 = 全占用，再按需清 0 = 空闲）
 * + 一个**全局 inode 数据面**（单条连续 extent + fr 排除 idx0）喂 ctor。
 *
 * v2.0 变更：inode 组不再是位图源，改为 inode_data_plane_t（extent 翻译 + fr 空闲面）。
 * 本文件聚焦 block 组覆盖 + inode 面基础语义（best-fit / free 融合 / locate）。
 * inode 增长（用尽→grow）、init 校验负数例、随机压测见
 * tests/test_fs_allocator_inode_plane.cpp。
 *
 * 覆盖：
 *   1. ctor 分片推导（block 单组 2^n 上界 ∩ 满片数 / 末片非空 / 参数缺失自清理）
 *   2. block_alloc_hard：单片命中 / 全败 / **单片原子不跨片拼**
 *   3. block_alloc_soft：多段凑满（碎片优先跨片）+ 不够 **回滚**
 *   4. block_occupy_soft：跨片续圈真实连续 / 中途撞占用停 / 锚点占用 NO_BLOCK
 *   5. block_free 跨片 + block_intervals_free 链式消费
 *   6. inode 数据面：best-fit / inode_test / free 融合 / locate / 用尽无块 NO_BLOCK
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/fs_allocator.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <forward_list>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

static const uint64_t MSB  = 32768;      /* 片下限 = 1 位图块 */
static const uint64_t BITS = MSB * 4;    /* 4 片靶场 = 131072 bit */

/* 4K 对齐原始位图：构造即全 1（全占用），clear() 挖空闲 */
struct Raw {
    uint8_t *mem = nullptr;
    explicit Raw(uint64_t nbits) {
        const uint64_t bytes = (nbits + 7) / 8;
        if (posix_memalign((void **)&mem, 4096, bytes) != 0) { printf("  FAIL memalign\n"); exit(1); }
        memset(mem, 0xFF, bytes);
    }
    ~Raw() { free(mem); }
    Raw(const Raw &) = delete;
    Raw &operator=(const Raw &) = delete;
    void clear(uint64_t start, uint64_t n) {
        for (uint64_t i = 0; i < n; i++)
            mem[(start + i) >> 3] &= (uint8_t)~(1u << ((start + i) & 7));
    }
    bool bit(uint64_t i) const { return (mem[i >> 3] >> (i & 7)) & 1; }
};

/* inode 数据面靶场：默认单条连续 extent（pieces 片）+ fr[1, pieces*32)。 */
struct Plane {
    uint64_t pieces = 2;
    uint64_t phys = 4096;                 /* 偶基址（靶场不真读盘） */
    std::vector<fs_inode_extent_t> ext;
    std::vector<inode_dataplane_coordinate_free_range_t> fr;
    void make(uint64_t p = 2, uint64_t phys_base = 4096) {
        pieces = p; phys = phys_base; ext.clear(); fr.clear();
        ext.push_back(fs_inode_extent_t{0, phys, pieces * 2});
        fr.push_back(inode_dataplane_coordinate_free_range_t{1, pieces * 32 - 1});
    }
};

static slices_cfg_t make_cfg(Raw &blk, Plane &p, uint32_t cpus = 4) {
    slices_cfg_t cfg{};
    cfg.block_bits_raw = blk.mem;
    cfg.block_bits_count = BITS;
    cfg.inode.pieces = p.pieces;
    cfg.inode.extents = p.ext.data();
    cfg.inode.extent_count = p.ext.size();
    cfg.inode.fr = p.fr.data();
    cfg.inode.fr_count = p.fr.size();
    cfg.logical_processor_count = cpus;
    return cfg;
}

static uint64_t sum_runs(const fs_run_list_t &runs) {
    uint64_t s = 0;
    for (const fs_run_t &r : runs) s += r.len;
    return s;
}

/* ═══ 1. ctor 分片推导 ═══ */
static int test_ctor_geometry() {
    puts("[1] ctor 分片推导（block 单组）");

    {   /* 4 片：cap=4，n=4 */
        Raw b(BITS); Plane p; p.make(2);
        slices_cfg_t cfg = make_cfg(b, p, 4);
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        CHECK(rc == RC_OK, "ctor 4-slice ok");
        CHECK(a.slice_count() == 4, "slice_count==4");
        CHECK(a.cpu_slots() == 4, "cpu_slots==4");
        CHECK(a.block_per_slice_bits() == MSB, "block per==32768");
        for (uint32_t c = 0; c < 4; c++)
            CHECK(a.block_preferred_of(c) == c, "block preferred init");
        CHECK(a.block_free_total() == 0, "all-occupied block free==0");
        CHECK(a.block_test(0) == true, "block 0 occupied");
        /* inode 面：root 恒占用，rest 空闲 */
        CHECK(a.inode_test(0) == true, "inode 0 (root) occupied");
        CHECK(a.inode_free_total() == 2 * 32 - 1, "inode free = pieces*32-1");
    }
    {   /* 满片数约束把 cpu 上界压到 1 */
        Raw b(MSB); Plane p; p.make(2);
        slices_cfg_t cfg = make_cfg(b, p, 4);
        cfg.block_bits_count = MSB;          /* 只喂 1 片 */
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        CHECK(rc == RC_OK, "ctor 1-slice ok");
        CHECK(a.slice_count() == 1, "cap=1 -> slice_count==1");
        CHECK(a.block_per_slice_bits() == MSB, "1-slice per==32768");
    }
    {   /* cap=3 压 2^n 上界 4 -> 3 */
        const uint64_t N3 = MSB * 3;
        Raw b(N3); Plane p; p.make(2);
        slices_cfg_t cfg{};
        cfg.block_bits_raw = b.mem; cfg.block_bits_count = N3;
        cfg.inode.pieces = p.pieces;
        cfg.inode.extents = p.ext.data(); cfg.inode.extent_count = p.ext.size();
        cfg.inode.fr = p.fr.data(); cfg.inode.fr_count = p.fr.size();
        cfg.logical_processor_count = 4;
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        CHECK(rc == RC_OK, "ctor 3-slice ok");
        CHECK(a.slice_count() == 3, "cap=3 -> slice_count==3");
    }
    {   /* 参数缺失：block 源空 → 空态 + RC_ERR_STATE */
        slices_cfg_t cfg{};
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        CHECK(rc == RC_ERR_STATE, "null raw -> RC_ERR_STATE");
        CHECK(a.slice_count() == 0, "null raw -> empty");
        CHECK(a.block_free_total() == 0, "empty free_total==0");
        CHECK(a.block_test(0) == false, "empty test==false");
    }
    {   /* block 源有效但 inode 数据面非法（pieces=0）→ 整体失败自清理 */
        Raw b(BITS);
        slices_cfg_t cfg{};
        cfg.block_bits_raw = b.mem; cfg.block_bits_count = BITS;
        /* inode 默认全 0 → init 失败 */
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        CHECK(rc == RC_ERR_STATE, "bad inode plane -> ctor fail");
        CHECK(a.slice_count() == 0, "bad inode plane -> block rolled back");
        CHECK(a.block_free_total() == 0, "bad inode plane -> block free 0");
    }
    return 0;
}

/* ═══ 2. block_alloc_hard：命中 / 全败 / 单片原子 ═══ */
static int test_block_alloc_hard() {
    puts("[2] block_alloc_hard：单片命中 / 全败 / 不跨片");
    Raw b(BITS); Plane p; p.make(2);
    b.clear(100, 8);                    /* 单片命中靶：[100,108) */
    b.clear(32764, 4);                  /* 跨片原子靶：slice0 尾 [32764,32768) */
    b.clear(32768, 4);                  /*             slice1 首 [32768,32772) */
    slices_cfg_t cfg = make_cfg(b, p, 4);
    uint64_t rc = 1;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");
    CHECK(a.block_free_total() == 16, "free 8+4+4=16");

    CHECK(a.block_alloc_hard(8, 0) == 100, "hard hit @100");
    for (uint64_t k = 100; k < 108; k++) CHECK(a.block_test(k) == true, "consumed bit occupied");
    CHECK(a.block_free_total() == 8, "free 16-8==8");

    CHECK(a.block_alloc_hard(8, 0) == PTR_INVALID, "hard never spans slices / all fail");
    CHECK(a.block_alloc_hard(4, 0) == 32764, "hard single-slice 4 ok @32764");
    CHECK(a.block_alloc_hard(4, 0) == 32768, "hard other slice 4 ok @32768");
    CHECK(a.block_free_total() == 0, "free drained");
    CHECK(a.block_alloc_hard(1, 0) == PTR_INVALID, "hard full -> PTR_INVALID");
    return 0;
}

/* ═══ 3. block_alloc_soft：多段凑满 + 不够回滚 ═══ */
static int test_block_alloc_soft() {
    puts("[3] block_alloc_soft：跨片多段凑满 / 不够回滚");
    Raw b(BITS); Plane p; p.make(2);
    b.clear(32760, 4);
    b.clear(32768, 4);
    b.clear(65536, 4);
    b.clear(98304, 8);
    slices_cfg_t cfg = make_cfg(b, p, 4);
    uint64_t rc = 1;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");
    CHECK(a.block_free_total() == 20, "free 4+4+4+8=20");

    fs_run_list_t runs;
    CHECK(a.block_alloc_soft(12, 0, &runs) == RC_OK, "soft rc ok");
    CHECK(sum_runs(runs) == 12, "soft delivered 12 (multi-seg)");
    uint64_t segs = 0;
    for (const fs_run_t &r : runs) { (void)r; segs++; }
    CHECK(segs >= 3, "soft: >=3 fragments assembled");
    for (const fs_run_t &r : runs)
        for (uint64_t k = 0; k < r.len; k++)
            CHECK(a.block_test(r.base + k) == true, "soft run bits occupied");
    CHECK(a.block_free_total() == 8, "free 20-12==8");
    runs.clear();

    CHECK(a.block_alloc_soft(12, 0, &runs) == RC_ERR_NO_BLOCK, "soft not enough -> NO_BLOCK");
    CHECK(runs.empty(), "soft rollback: runs emptied");
    CHECK(a.block_free_total() == 8, "soft rollback: 账复原==8");

    CHECK(a.block_alloc_soft(4, 0, nullptr) == RC_ERR_STATE, "soft nullptr runs");
    CHECK(a.block_alloc_soft(0, 0, &runs) == RC_ERR_STATE, "soft zero count");
    return 0;
}

/* ═══ 4. block_occupy_soft ═══ */
static int test_block_occupy_soft() {
    puts("[4] block_occupy_soft：跨片续圈 / 停在占用 / 锚点占用");
    {   Raw b(BITS); Plane p; p.make(2);
        b.clear(32760, 16);
        slices_cfg_t cfg = make_cfg(b, p, 4);
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        CHECK(rc == RC_OK, "ctor ok");
        CHECK(a.block_free_total() == 16, "free 16");
        uint64_t rest = 999;
        CHECK(a.block_occupy_soft(32760, 16, &rest) == RC_OK, "occupy cross-slice ok");
        CHECK(rest == 0, "occupy fully satisfied");
        for (uint64_t k = 0; k < 16; k++) CHECK(a.block_test(32760 + k) == true, "cross-slice bit occupied");
        CHECK(a.block_free_total() == 0, "free drained");
        CHECK(a.block_free(32760, 16) == RC_OK, "cross-slice free ok");
        CHECK(a.block_free_total() == 16, "free restored 16");
        for (uint64_t k = 0; k < 16; k++) CHECK(a.block_test(32760 + k) == false, "freed bits free");
    }
    {   Raw b(BITS); Plane p; p.make(2);
        b.clear(32760, 8);
        slices_cfg_t cfg = make_cfg(b, p, 4);
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        uint64_t rest = 999;
        CHECK(a.block_occupy_soft(32760, 16, &rest) == RC_OK, "occupy partial-prefix ok");
        CHECK(rest == 8, "rest==8 (stopped at occupied next slice)");
        for (uint64_t k = 0; k < 8; k++) CHECK(a.block_test(32760 + k) == true, "claimed bits");
        CHECK(a.block_test(32768) == true, "next slice start untouched (was occupied)");
        CHECK(a.block_free_total() == 0, "free drained");
    }
    {   Raw b(BITS); Plane p; p.make(2);
        slices_cfg_t cfg = make_cfg(b, p, 4);
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        uint64_t rest = 999;
        CHECK(a.block_occupy_soft(100, 4, &rest) == RC_ERR_NO_BLOCK, "anchor occupied -> NO_BLOCK");
        CHECK(rest == 4, "rest==require");
        CHECK(a.block_occupy_soft(100, 0, &rest) == RC_ERR_STATE, "occupy zero count");
    }
    return 0;
}

/* ═══ 5. block_free 跨片 + block_intervals_free 链式 ═══ */
static int test_block_free_chains() {
    puts("[5] block_free 跨片 / block_intervals_free");
    Raw b(BITS); Plane p; p.make(2);
    b.clear(32760, 8);
    b.clear(32768, 8);
    slices_cfg_t cfg = make_cfg(b, p, 4);
    uint64_t rc = 1;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");
    CHECK(a.block_free_total() == 16, "free 8+8=16");

    uint64_t rest = 999;
    CHECK(a.block_occupy_soft(32760, 12, &rest) == RC_OK, "occupy cross-slice 12");
    CHECK(rest == 0, "rest 0");
    for (uint64_t k = 0; k < 12; k++) CHECK(a.block_test(32760 + k) == true, "occupied cross-slice");
    CHECK(a.block_free_total() == 4, "free 16-12==4");

    CHECK(a.block_free(32760, 12) == RC_OK, "block_free cross-slice ok");
    CHECK(a.block_free_total() == 16, "free restored 16");
    for (uint64_t k = 0; k < 12; k++) CHECK(a.block_test(32760 + k) == false, "freed bits free");
    CHECK(a.block_free(40000, 0) == RC_OK, "free count==0 no-op");

    fs_run_list_t runs;
    CHECK(a.block_alloc_soft(8, 0, &runs) == RC_OK, "soft 8");
    CHECK(!runs.empty(), "soft produced runs");
    CHECK(a.block_free_total() == 8, "free 16-8==8");
    CHECK(a.block_intervals_free(&runs) == RC_OK, "intervals_free ok");
    CHECK(runs.empty(), "runs drained");
    CHECK(a.block_free_total() == 16, "free restored via chain");
    CHECK(a.block_intervals_free(nullptr) == RC_ERR_STATE, "intervals_free nullptr");
    return 0;
}

/* ═══ 6. inode 数据面：best-fit / free 融合 / locate / 用尽无块 ═══ */
static int test_inode() {
    puts("[6] inode 数据面：best-fit / free 融合 / locate / 用尽");

    Raw b(BITS); Plane p; p.make(2, 4096);
    slices_cfg_t cfg = make_cfg(b, p, 4);
    uint64_t rc = 1;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");
    CHECK(a.inode_free_total() == 63, "free 2*32-1==63");
    CHECK(a.inode_test(0) == true, "root occupied");
    CHECK(a.inode_test(1) == false, "1 free initially");

    uint64_t n = PTR_INVALID;
    CHECK(a.inode_alloc(n) == RC_OK && n == 1, "alloc best-fit -> 1");
    CHECK(a.inode_test(1) == true, "1 occupied");
    CHECK(a.inode_free_total() == 62, "free 63-1==62");
    CHECK(a.inode_alloc(n) == RC_OK && n == 2, "alloc -> 2");

    /* locate：idx1 在 extent[0]（lblk 0..3, phys 4096）→ blkbase 4096, off 256 */
    {
        inode_location_t loc{};
        CHECK(a.inode_locate(1, &loc) == RC_OK, "locate(1) ok");
        CHECK(loc.blkbase == 4096, "locate blkbase==4096");
        CHECK(loc.inblk_offset == 256, "locate off==(1&31)*256");
        CHECK(a.inode_locate(32, &loc) == RC_OK && loc.blkbase == 4098, "locate(32) -> next 8K piece");
        CHECK(a.inode_locate(63, &loc) == RC_OK && loc.blkbase == 4098 && loc.inblk_offset == 7936,
              "locate(63) -> 2nd 4K of piece, off=31*256");
        CHECK(a.inode_locate(1000, &loc) == RC_ERR_NOT_FOUND, "locate beyond extent -> NOT_FOUND");
    }

    a.inode_free(1);
    CHECK(a.inode_test(1) == false, "1 freed");
    CHECK(a.inode_free_total() == 62, "free 62+1? (2 allocated, 1 freed) = 62");
    a.inode_free(1);   /* 幂等 */
    CHECK(a.inode_free_total() == 62, "double free idempotent");
    a.inode_free(2);   /* 与 [3,..] + [1,..) 融合回 [1,63) */
    CHECK(a.inode_free_total() == 63, "free merge -> 63");
    a.inode_free(0);   /* root 拒绝 */
    CHECK(a.inode_test(0) == true, "root not freed");

    /* 用尽：把 63 个空闲全取走 → 下一次 alloc 触发 grow；block 靶场全占用 → NO_BLOCK */
    for (uint64_t i = 0; i < 63; i++) {
        uint64_t got = PTR_INVALID;
        CHECK(a.inode_alloc(got) == RC_OK && got != PTR_INVALID, "drain alloc");
    }
    CHECK(a.inode_free_total() == 0, "drained to 0");
    uint64_t got = 0;
    CHECK(a.inode_alloc(got) == RC_ERR_NO_BLOCK, "exhaust + no free block -> NO_BLOCK");
    CHECK(a.inode_test(9999) == false, "OOB test false");
    return 0;
}

int main() {
    test_ctor_geometry();
    test_block_alloc_hard();
    test_block_alloc_soft();
    test_block_occupy_soft();
    test_block_free_chains();
    test_inode();
    printf("\n==== test_fs_allocator_router: PASS %d, FAIL %d ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

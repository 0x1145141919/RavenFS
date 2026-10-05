/* ═══════════════════════════════════════════════════════════════════
 * test_fs_allocator_inode_plane.cpp — 全局 inode 数据面专项（v2.0 P1）
 *
 * 靶场：block 位图（供 grow 取片）+ inode_data_plane_cfg（extent + fr）。
 * 覆盖：
 *   1. init 校验负数例（对齐 / 洞 / 重叠 / Σ / idx0 / fr 越界）→ ctor 失败
 *   2. locate 回环 vs 朴素模型
 *   3. alloc best-fit / free 融合 / inode_test
 *   4. 用尽 → grow（硬分配 8K 对齐；物理尾邻接合并 extent）
 *   5. 随机压测（alloc/free 影子对账 + fr 不变量 + locate 抽样）
 *   6. 小并发（4 线程共享数据面：无重复交付 / 收束归位）
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/fs_allocator.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

static const uint64_t MSB = 32768;

/* block 位图：默认全占用（0xFF）；free() 挖空闲；mark() 占用 */
struct Raw {
    uint8_t *mem = nullptr;
    explicit Raw(uint64_t nbits) {
        const uint64_t bytes = (nbits + 7) / 8;
        if (posix_memalign((void **)&mem, 4096, bytes) != 0) { printf("  FAIL memalign\n"); exit(1); }
        memset(mem, 0xFF, bytes);
    }
    ~Raw() { free(mem); }
    Raw(const Raw &) = delete; Raw &operator=(const Raw &) = delete;
    void free_bits(uint64_t start, uint64_t n) {
        for (uint64_t i = 0; i < n; i++) mem[(start + i) >> 3] &= (uint8_t)~(1u << ((start + i) & 7));
    }
    void mark_bits(uint64_t start, uint64_t n) {
        for (uint64_t i = 0; i < n; i++) mem[(start + i) >> 3] |= (uint8_t)(1u << ((start + i) & 7));
    }
};

/* ═══ 1. init 校验负数例 ═══ */
static void test_init_validation() {
    puts("[1] init 校验负数例");
    Raw blk(MSB);

    auto build = [&](std::vector<fs_inode_extent_t> ext,
                     std::vector<inode_dataplane_coordinate_free_range_t> fr,
                     uint64_t pieces, uint64_t *rc_out) {
        slices_cfg_t cfg{};
        cfg.block_bits_raw = blk.mem; cfg.block_bits_count = MSB;
        cfg.inode.pieces = pieces;
        cfg.inode.extents = ext.data(); cfg.inode.extent_count = ext.size();
        cfg.inode.fr = fr.data();       cfg.inode.fr_count = fr.size();
        cfg.logical_processor_count = 1;
        uint64_t rc = 0;
        { fs_allocator_t a(cfg, &rc); }
        *rc_out = rc;
    };

    uint64_t rc = 0;
    /* 基线：合法单条 extent + fr[1,63) */
    build({{0, 0, 4}}, {{1, 63}}, 2, &rc);
    CHECK(rc == RC_OK, "baseline ok");

    build({{0, 0, 3}}, {{1, 63}}, 2, &rc);          /* length 奇 */
    CHECK(rc == RC_ERR_STATE, "odd length rejected");

    build({{0, 1, 4}}, {{1, 63}}, 2, &rc);          /* phys 奇 */
    CHECK(rc == RC_ERR_STATE, "odd phys rejected");

    build({{1, 0, 2}, {3, 2, 2}}, {{1, 63}}, 2, &rc);  /* lblk 有洞（不始于 0）*/
    CHECK(rc == RC_ERR_STATE, "lblk hole rejected");

    build({{0, 0, 2}, {4, 2, 2}}, {{1, 63}}, 2, &rc);  /* 洞 */
    CHECK(rc == RC_ERR_STATE, "density hole rejected");

    build({{0, 0, 4}}, {{1, 63}}, 3, &rc);          /* Σlength(4) != pieces*2(6) */
    CHECK(rc == RC_ERR_STATE, "sum mismatch rejected");

    build({{0, 0, 4}}, {{0, 4}}, 2, &rc);           /* fr 含 idx0 */
    CHECK(rc == RC_ERR_STATE, "fr idx0 rejected");

    build({{0, 0, 4}}, {{1, 40}, {30, 10}}, 2, &rc);/* fr 重叠 */
    CHECK(rc == RC_ERR_STATE, "fr overlap rejected");

    build({{0, 0, 4}}, {{50, 100}}, 2, &rc);        /* fr 越界 */
    CHECK(rc == RC_ERR_STATE, "fr oob rejected");
}

/* ═══ 2. locate 回环 ═══ */
static void test_locate() {
    puts("[2] locate 回环 vs 朴素模型");
    Raw blk(MSB);
    std::vector<fs_inode_extent_t> ext = {{0, 0, 4}, {4, 10, 2}};  /* 两段：phys[0,4)+[10,12) */
    std::vector<inode_dataplane_coordinate_free_range_t> fr = {{1, 63}, {64, 32}};    /* idx64..95 */
    slices_cfg_t cfg{};
    cfg.block_bits_raw = blk.mem; cfg.block_bits_count = MSB;
    cfg.inode.pieces = 3;
    cfg.inode.extents = ext.data(); cfg.inode.extent_count = ext.size();
    cfg.inode.fr = fr.data(); cfg.inode.fr_count = fr.size();
    cfg.logical_processor_count = 1;
    uint64_t rc = 0;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");

    /* 朴素：lblk=idx>>4；第 0 段覆盖 lblk[0,4) phys 0；第 1 段覆盖 lblk[4,6) phys 10 */
    uint64_t bad = 0;
    for (uint64_t i = 0; i < 96; i++) {
        inode_location_t loc{};
        if (a.inode_locate(i, &loc) != RC_OK) { bad++; continue; }
        uint64_t lblk = i >> 4;
        uint64_t phys = (lblk < 4) ? (0 + lblk) : (10 + (lblk - 4));
        uint64_t want_blk = phys & ~1ULL;              /* 8K 片基（偶块） */
        uint16_t want_off = (uint16_t)((i & 31) * 256);
        if (loc.blkbase != want_blk || loc.inblk_offset != want_off) bad++;
    }
    CHECK(bad == 0, "locate roundtrip all 96 inodes");
    {
        inode_location_t loc{};
        CHECK(a.inode_locate(96, &loc) == RC_ERR_NOT_FOUND, "locate out of extent -> NOT_FOUND");
        CHECK(a.inode_locate(0, nullptr) == RC_ERR_STATE, "locate null out -> STATE");
    }
}

/* ═══ 3. alloc best-fit / free 融合 ═══ */
static void test_alloc_free() {
    puts("[3] alloc best-fit / free 融合");
    Raw blk(MSB);
    std::vector<fs_inode_extent_t> ext = {{0, 0, 2}};
    std::vector<inode_dataplane_coordinate_free_range_t> fr = {{1, 31}};   /* pieces=1 -> 32 inode */
    slices_cfg_t cfg{};
    cfg.block_bits_raw = blk.mem; cfg.block_bits_count = MSB;
    cfg.inode.pieces = 1;
    cfg.inode.extents = ext.data(); cfg.inode.extent_count = 1;
    cfg.inode.fr = fr.data(); cfg.inode.fr_count = 1;
    cfg.logical_processor_count = 1;
    uint64_t rc = 0;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");
    CHECK(a.inode_free_total() == 31, "free 31");
    CHECK(a.inode_test(0) == true, "root occupied");
    CHECK(a.inode_test(1) == false, "1 free");

    uint64_t n = 0;
    CHECK(a.inode_alloc(n) == RC_OK && n == 1, "alloc 1");
    CHECK(a.inode_alloc(n) == RC_OK && n == 2, "alloc 2");
    CHECK(a.inode_free_total() == 29, "free 29");

    /* free(1)+free(2) 与 [3,32) 融合回 [1,32) */
    a.inode_free(1);
    CHECK(a.inode_free_total() == 30, "free 30");
    a.inode_free(1);
    CHECK(a.inode_free_total() == 30, "double free idempotent");
    a.inode_free(2);
    CHECK(a.inode_free_total() == 31, "merge -> 31");
    a.inode_free(0);
    CHECK(a.inode_test(0) == true, "root never freed");
    a.inode_free(1000);
    CHECK(a.inode_free_total() == 31, "OOB free ignored");
}

/* ═══ 4. 用尽 → grow ═══ */
static void test_grow() {
    puts("[4] 用尽 -> grow（硬分配 + 物理尾合并 / 非邻接分裂）");
    /* block 靶场：默认全占用；只留两个 len=2 的空闲区间 [2,4) 与 [100,102)。
     * mgr_slice_t::alloc_hard = best-fit 最小 len + **从区间尾下挖**：
     *   两次都命中 len=2（tie 按 base 小）→ 第一次得块 2（与 extent 尾 2 邻接→合并）、
     *   第二次得块 100（与尾 4 不邻接→新 extent）。 */
    Raw blk(MSB);
    blk.free_bits(2, 2);
    blk.free_bits(100, 2);

    std::vector<fs_inode_extent_t> ext = {{0, 0, 2}};   /* phys[0,2) */
    std::vector<inode_dataplane_coordinate_free_range_t> fr = {{1, 31}};  /* 1 片：idx1..31 */
    slices_cfg_t cfg{};
    cfg.block_bits_raw = blk.mem; cfg.block_bits_count = MSB;
    cfg.inode.pieces = 1;
    cfg.inode.extents = ext.data(); cfg.inode.extent_count = 1;
    cfg.inode.fr = fr.data(); cfg.inode.fr_count = 1;
    cfg.inode.grow_batch_pieces = 1;
    cfg.logical_processor_count = 1;
    uint64_t rc = 0;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");
    CHECK(a.inode_free_total() == 31, "free 31");

    for (int i = 0; i < 31; i++) { uint64_t m = 0; CHECK(a.inode_alloc(m) == RC_OK, "drain 1"); }
    CHECK(a.inode_free_total() == 0, "drained");

    uint64_t n = 0;
    CHECK(a.inode_alloc(n) == RC_OK && n == 32, "grow1 -> inode 32");
    CHECK(a.inode_plane().pieces() == 2, "pieces 2");
    CHECK(a.inode_plane().extent_count() == 1, "grow1 physical-tail merge -> 1 extent");
    {
        inode_location_t loc{};
        CHECK(a.inode_locate(32, &loc) == RC_OK && loc.blkbase == 2, "locate 32 @ blk 2 (merged)");
    }
    for (int i = 0; i < 31; i++) { uint64_t m = 0; a.inode_alloc(m); }
    CHECK(a.inode_alloc(n) == RC_OK && n == 64, "grow2 -> inode 64");
    CHECK(a.inode_plane().pieces() == 3, "pieces 3");
    CHECK(a.inode_plane().extent_count() == 2, "grow2 non-contiguous -> 2 extents");
    {
        inode_location_t loc{};
        CHECK(a.inode_locate(64, &loc) == RC_OK && loc.blkbase == 100, "locate 64 @ blk 100");
    }
}

/* ═══ 5. 随机压测（单线程影子对账 + 不变量） ═══ */
static void test_fuzz() {
    puts("[5] 随机压测（单线程）");
    const uint64_t PIECES = 64;                 /* 2048 inode */
    const uint64_t N = PIECES * 32;
    Raw blk(MSB);                               /* 全占用 → 不允许 grow（锁定空间） */
    std::vector<fs_inode_extent_t> ext = {{0, 0, PIECES * 2}};
    std::vector<inode_dataplane_coordinate_free_range_t> fr = {{1, N - 1}};
    slices_cfg_t cfg{};
    cfg.block_bits_raw = blk.mem; cfg.block_bits_count = MSB;
    cfg.inode.pieces = PIECES;
    cfg.inode.extents = ext.data(); cfg.inode.extent_count = 1;
    cfg.inode.fr = fr.data(); cfg.inode.fr_count = 1;
    cfg.logical_processor_count = 1;
    uint64_t rc = 0;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");

    std::vector<uint8_t> live(N, 0);
    live[0] = 1;                                /* root */
    std::vector<uint64_t> held;
    std::mt19937_64 rng(20261006);
    uint64_t allocs = 0;
    for (uint64_t it = 0; it < 200000; it++) {
        const bool do_alloc = held.empty() || ((rng() & 1) && held.size() < N / 2);
        if (do_alloc) {
            uint64_t n = 0;
            result_code_t r = a.inode_alloc(n);
            if (r == RC_OK) {
                if (n >= N || live[n]) { g_fail++; printf("  FAIL fuzz double/OOB alloc %llu\n", (unsigned long long)n); break; }
                live[n] = 1; held.push_back(n); allocs++;
            } else if (r != RC_ERR_NO_BLOCK) {
                g_fail++; printf("  FAIL fuzz alloc rc=%d\n", (int)r); break;
            }
        } else {
            const size_t k = (size_t)(rng() % held.size());
            const uint64_t n = held[k];
            held[k] = held.back(); held.pop_back();
            live[n] = 0;
            a.inode_free(n);
        }
        /* 抽样 locate：已分配项必须可翻译 */
        if ((it & 1023) == 0) {
            uint64_t probe = rng() % N;
            if (live[probe]) {
                inode_location_t loc{};
                if (a.inode_locate(probe, &loc) != RC_OK) { g_fail++; printf("  FAIL locate live %llu\n", (unsigned long long)probe); break; }
                uint64_t lblk = probe >> 4;
                if (loc.blkbase != (lblk & ~1ULL) || loc.inblk_offset != (uint16_t)((probe & 31) * 256)) {
                    g_fail++; printf("  FAIL locate mismatch %llu\n", (unsigned long long)probe); break;
                }
            }
        }
    }
    /* 收束：全归还 → fr 回到 [1,N) */
    for (uint64_t n : held) a.inode_free(n);
    held.clear();
    CHECK(a.inode_free_total() == N - 1, "fuzz final free == N-1");
    CHECK(allocs > 1000, "fuzz did meaningful allocs");

    /* fr 不变量：dump 出的区间 base 升序、互不重叠、并集 == N-1（inode 坐标类型） */
    std::list<inode_dataplane_coordinate_free_range_t> dl;
    CHECK(a.inode_plane().dump_fr(&dl) == RC_OK, "dump_fr ok");
    uint64_t sum = 0, prev_end = 0; bool ok = true;
    for (const inode_dataplane_coordinate_free_range_t &r : dl) {
        if (r.base < prev_end || r.len == 0) { ok = false; break; }
        prev_end = r.base + r.len; sum += r.len;
    }
    CHECK(ok && sum == N - 1, "fr disjoint + sum == N-1");

    std::list<fs_inode_extent_t> el;
    CHECK(a.inode_plane().dump_extents(&el) == RC_OK && el.size() == 1, "dump_extents 1");
    if (!el.empty()) {
        const fs_inode_extent_t &e = el.front();
        CHECK(e.lblk == 0 && e.phys_base == 0 && e.length == PIECES * 2, "extent intact");
    }
}

/* ═══ 6. 小并发（共享数据面） ═══ */
static std::atomic<uint64_t> g_dup{0};
static std::mutex g_shadow_mu;
static std::vector<uint8_t> g_shadow;

static void mt_worker(fs_allocator_t *a, uint64_t n_total, int seed) {
    std::mt19937_64 rng(0xC0FFEEULL ^ (uint64_t)seed);
    std::vector<uint64_t> held;
    for (uint64_t it = 0; it < 40000; it++) {
        const bool do_alloc = held.empty() || ((rng() & 1) && held.size() < 256);
        if (do_alloc) {
            uint64_t n = 0;
            result_code_t r = a->inode_alloc(n);
            if (r == RC_OK) {
                std::lock_guard<std::mutex> lk(g_shadow_mu);
                if (n >= n_total || g_shadow[n]) { g_dup.fetch_add(1); }
                else g_shadow[n] = 1;
                held.push_back(n);
            }
        } else {
            const size_t k = (size_t)(rng() % held.size());
            const uint64_t n = held[k];
            held[k] = held.back(); held.pop_back();
            { std::lock_guard<std::mutex> lk(g_shadow_mu); g_shadow[n] = 0; }
            a->inode_free(n);
        }
    }
    for (uint64_t n : held) { { std::lock_guard<std::mutex> lk(g_shadow_mu); g_shadow[n] = 0; } a->inode_free(n); }
}

static void test_concurrent() {
    puts("[6] 小并发（4 线程共享数据面）");
    const uint64_t PIECES = 32;                 /* 1024 inode */
    const uint64_t N = PIECES * 32;
    Raw blk(MSB);
    std::vector<fs_inode_extent_t> ext = {{0, 0, PIECES * 2}};
    std::vector<inode_dataplane_coordinate_free_range_t> fr = {{1, N - 1}};
    slices_cfg_t cfg{};
    cfg.block_bits_raw = blk.mem; cfg.block_bits_count = MSB;
    cfg.inode.pieces = PIECES;
    cfg.inode.extents = ext.data(); cfg.inode.extent_count = 1;
    cfg.inode.fr = fr.data(); cfg.inode.fr_count = 1;
    cfg.logical_processor_count = 4;
    uint64_t rc = 0;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");

    g_shadow.assign(N, 0); g_shadow[0] = 1; g_dup.store(0);
    std::vector<std::thread> th;
    for (int t = 0; t < 4; t++) th.emplace_back(mt_worker, &a, N, t);
    for (auto &x : th) x.join();

    CHECK(g_dup.load() == 0, "no double delivery (concurrent)");
    CHECK(a.inode_free_total() == N - 1, "concurrent final free == N-1");
    uint64_t shadow_live = 0; for (uint8_t b : g_shadow) shadow_live += b;
    CHECK(shadow_live == 1, "shadow live == root only");
}

int main() {
    test_init_validation();
    test_locate();
    test_alloc_free();
    test_grow();
    test_fuzz();
    test_concurrent();
    printf("\n==== test_fs_allocator_inode_plane: PASS %d, FAIL %d ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

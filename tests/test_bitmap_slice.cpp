/* ═══════════════════════════════════════════════════════════════════
 * test_bitmap_slice.cpp — bitmap_slice_t 分片位图自治单元单测
 *
 * 靶场构造：全 1 位图（全部占用）→ 挖空闲位/区间（模拟 free 出的
 * 空间）→ enable_slice 扫片统计 → 跑 alloc 场景 → 校验：
 *   free_count 变化、get_hint/set_hint 的 hint 推进、分配位号集合。
 *
 * 用例：
 *   1. alloc_hard hint 命中：空闲区间在 hint 之后，扫一次即中
 *   2. alloc_hard hint miss：空闲区间在 hint 之前，段尾绕回 [0,hint) 中
 *   3. alloc_hard 对齐：2^align 片内对齐；段内无对齐候选 → 失败
 *   4. alloc_hard 全占用：RC_ERR_NO_BLOCK
 *   5. alloc_soft 1-bit 打散 gather：1000 个随机 1-bit 空闲，轮番
 *      alloc_soft(count=1) 全量回收，位号集合完全一致，free_count 归 0
 *   6. alloc_soft count=3 轮番 gather：混合回收总量精确
 *
 * 坐标说明：slice 从全局位图 0 起（base_idx=0），全局位号 == 片内偏移，
 * 便于直接对照 out_base_idx 与 get_hint()。
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <set>

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); return 1; } \
} while (0)

/* 供 void 函数使用：失败直接退出 */
#define CHECK_FATAL(cond, msg) do { \
    if (!(cond)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); exit(1); } \
} while (0)

/* ── 确定性伪随机（xorshift64）── */
static uint64_t g_seed = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd64() {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return g_seed;
}

/* ── 靶场：全局位图（4K 对齐，全 1）+ 一个 slice（从 0 起 65536 bit）── */
using Slice = RavenFS_juvenile::bitmap_slice_t;
using Desc  = RavenFS_juvenile::fs_bitmap_descriptor_t;
using Cfg   = RavenFS_juvenile::slice_config_t;

static constexpr uint64_t SLICE_BITS = 65536;   /* 32768 倍数：非末片约束满足 */
static constexpr uint64_t GLOBAL_BITS = 131072; /* 全局位图 16KB */

struct Range {
    uint8_t *mem = nullptr;
    Desc desc;
    Slice slice;
};

static void make_range(Range &r) {
    if (posix_memalign((void **)&r.mem, 4096, GLOBAL_BITS / 8) != 0) {
        printf("  FAIL posix_memalign\n");
        exit(1);
    }
    memset(r.mem, 0xFF, GLOBAL_BITS / 8);   /* 全 1 = 全部占用 */
    r.desc.FS_blk_base = 0;
    r.desc.bitmap_raw = reinterpret_cast<uint64_t *>(r.mem);
    r.desc.bits_count = GLOBAL_BITS;
}

static void enable(Range &r) {
    Cfg cfg;
    cfg.slice_base = r.mem;                 /* 从全局 0 起，4K 对齐，base_idx=0 */
    cfg.slice_bitscount = SLICE_BITS;
    CHECK_FATAL(r.slice.enable_slice(&r.desc, cfg) == RC_OK, "enable_slice");
}

static void carve_bit(Range &r, uint64_t idx, bool v) {
    if (v) r.mem[idx >> 3] |=  (uint8_t)(1u << (idx & 7));
    else   r.mem[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
}
static bool peek_bit(const Range &r, uint64_t idx) {
    return (r.mem[idx >> 3] >> (idx & 7)) & 1;
}

/* ═══ 用例 1：alloc_hard hint 命中 ═══ */
static int test_alloc_hard_hint_hit() {
    printf("[1] alloc_hard hint 命中\n");
    Range r;
    make_range(r);
    for (uint64_t i = 500; i < 504; i++) carve_bit(r, i, false);   /* 挖 [500,504) */
    enable(r);
    CHECK(r.slice.get_free_count() == 4, "free_count==4");

    r.slice.set_hint(100);
    uint64_t out = 0;
    CHECK(r.slice.alloc_hard(0, 4, out) == RC_OK, "alloc rc");
    CHECK(out == 500, "out==500（hint 后扫一次即中）");
    CHECK(r.slice.get_hint() == 504, "hint 推进到 504");
    CHECK(r.slice.get_free_count() == 0, "free_count 归 0");
    for (uint64_t i = 500; i < 504; i++) CHECK(peek_bit(r, i), "bit 置 1");
    free(r.mem);
    return 0;
}

/* ═══ 用例 2：alloc_hard hint miss（绕回）═══ */
static int test_alloc_hard_hint_miss() {
    printf("[2] alloc_hard hint miss（段尾绕回）\n");
    Range r;
    make_range(r);
    for (uint64_t i = 100; i < 104; i++) carve_bit(r, i, false);   /* 挖 [100,104) */
    enable(r);
    CHECK(r.slice.get_free_count() == 4, "free_count==4");

    r.slice.set_hint(1000);                  /* hint 在空闲区间之后 */
    uint64_t out = 0;
    CHECK(r.slice.alloc_hard(0, 4, out) == RC_OK, "alloc rc");
    CHECK(out == 100, "out==100（[1000,尾) 无空闲 → 绕回 [0,1000) 命中）");
    CHECK(r.slice.get_hint() == 104, "hint 推进到 104");
    CHECK(r.slice.get_free_count() == 0, "free_count 归 0");
    free(r.mem);
    return 0;
}

/* ═══ 用例 3：alloc_hard 对齐语义 ═══ */
static int test_alloc_hard_align() {
    printf("[3] alloc_hard 对齐（2^3）\n");
    {
        Range r;
        make_range(r);
        for (uint64_t i = 8; i < 16; i++) carve_bit(r, i, false);  /* 挖 [8,16)，8 对齐候选 8 */
        enable(r);
        r.slice.set_hint(0);
        uint64_t out = 0;
        CHECK(r.slice.alloc_hard(3, 2, out) == RC_OK, "alloc rc");
        CHECK(out == 8, "out==8（段内 8 对齐候选）");
        CHECK(r.slice.get_hint() == 10, "hint==10");
        CHECK(r.slice.get_free_count() == 6, "free_count 8-2==6");
        free(r.mem);
    }
    {
        Range r;
        make_range(r);
        for (uint64_t i = 9; i < 17; i++) carve_bit(r, i, false);  /* 挖 [9,17)，8 对齐候选 16 放不下 2 */
        enable(r);
        r.slice.set_hint(0);
        uint64_t out = 0;
        CHECK(r.slice.alloc_hard(3, 2, out) == RC_ERR_NO_BLOCK, "对齐候选不足 → NO_BLOCK");
        CHECK(r.slice.get_hint() == 0, "失败不推进 hint");
        CHECK(r.slice.get_free_count() == 8, "free_count 不变");
        free(r.mem);
    }
    return 0;
}

/* ═══ 用例 4：alloc_hard 全占用 ═══ */
static int test_alloc_hard_full() {
    printf("[4] alloc_hard 全占用\n");
    Range r;
    make_range(r);
    enable(r);                              /* 没挖任何空闲 */
    CHECK(r.slice.get_free_count() == 0, "free_count==0");
    uint64_t out = 0;
    CHECK(r.slice.alloc_hard(0, 1, out) == RC_ERR_NO_BLOCK, "NO_BLOCK");
    free(r.mem);
    return 0;
}

/* ═══ 用例 5：alloc_soft 1-bit 打散全量 gather ═══ */
static int test_alloc_soft_gather_1bit() {
    printf("[5] alloc_soft 1-bit 打散 gather（1000 个随机空闲）\n");
    Range r;
    make_range(r);
    std::set<uint64_t> freed;
    while (freed.size() < 1000) {
        uint64_t b = rnd64() % SLICE_BITS;
        if (freed.insert(b).second) carve_bit(r, b, false);
    }
    enable(r);
    CHECK(r.slice.get_free_count() == 1000, "free_count==1000");

    std::set<uint64_t> got;
    uint64_t rounds = 0;
    for (;;) {
        uint64_t out = 0, left = 0;
        result_code_t rc = r.slice.alloc_soft(0, 1, out, left);
        if (rc == RC_ERR_NO_BLOCK) break;
        CHECK(rc == RC_OK, "alloc_soft rc");
        CHECK(left == 0, "count=1 无缺口");
        CHECK(r.slice.get_hint() == out + 1, "hint == out+1（每次只拿 1）");
        CHECK(got.insert(out).second, "无重复分配");
        rounds++;
    }
    CHECK(got.size() == 1000, "全量 gather 1000");
    CHECK(got == freed, "位号集合完全一致");
    CHECK(r.slice.get_free_count() == 0, "free_count 归 0");
    printf("    rounds=%llu\n", (unsigned long long)rounds);
    free(r.mem);
    return 0;
}

/* ═══ 用例 6：alloc_soft count=3 轮番 gather ═══ */
static int test_alloc_soft_gather_multi() {
    printf("[6] alloc_soft count=3 轮番 gather（100 个随机空闲）\n");
    Range r;
    make_range(r);
    std::set<uint64_t> freed;
    while (freed.size() < 100) {
        uint64_t b = rnd64() % SLICE_BITS;
        if (freed.insert(b).second) carve_bit(r, b, false);
    }
    enable(r);
    CHECK(r.slice.get_free_count() == 100, "free_count==100");

    uint64_t total = 0, rounds = 0;
    for (;;) {
        uint64_t out = 0, left = 0;
        result_code_t rc = r.slice.alloc_soft(0, 3, out, left);
        if (rc == RC_ERR_NO_BLOCK) break;
        CHECK(rc == RC_OK, "alloc_soft rc");
        uint64_t got_this = 3 - left;
        CHECK(got_this >= 1 && got_this <= 3, "got∈[1,3]");
        CHECK(r.slice.get_hint() == out + got_this, "hint == out+got_this");
        total += got_this;
        rounds++;
    }
    CHECK(total == 100, "总量精确 100（全量 gather）");
    CHECK(r.slice.get_free_count() == 0, "free_count 归 0");
    printf("    rounds=%llu\n", (unsigned long long)rounds);
    free(r.mem);
    return 0;
}

int main() {
    int fail = 0;
    fail += test_alloc_hard_hint_hit();
    fail += test_alloc_hard_hint_miss();
    fail += test_alloc_hard_align();
    fail += test_alloc_hard_full();
    fail += test_alloc_soft_gather_1bit();
    fail += test_alloc_soft_gather_multi();
    if (fail) { printf("RESULT: %d FAILED\n", fail); return 1; }
    printf("RESULT: ALL PASS\n");
    return 0;
}

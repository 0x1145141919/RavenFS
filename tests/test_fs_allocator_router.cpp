/* ═══════════════════════════════════════════════════════════════════
 * test_fs_allocator_router.cpp — fs_allocator_t 路由层单测（2026-10-03）
 *
 * 靶场：4K 对齐的 block / inode 原始位图（先全 1 = 全占用，再按需清 0 = 空闲），
 * 交 fs_allocator_t ctor 建树；后续全走路由接口 + block_test/inode_test 观测。
 *
 * 覆盖：
 *   1. ctor 分片推导（2^n 上界 ∩ 满片数 / 末片非空 / 参数缺失自清理）+ preferred 初值
 *   2. block_alloc_hard：单片命中 / 全败 / **单片原子不跨片拼**
 *   3. block_alloc_soft：多段凑满（碎片优先跨片）+ 不够 **回滚**（runs 清空、账复原）
 *   4. block_occupy_soft：跨片续圈真实连续 / 中途撞占用停 / 锚点占用 NO_BLOCK
 *   5. block_free 跨片 + block_intervals_free 链式消费
 *   6. inode_alloc / inode_free / inode_test（含 root 不空闲）
 *   7. block_free_total / inode_free_total / preferred_of / 越界 test
 *   8. all_syn：RC_ERR_NOT_SUPPORTED（缺 IO 面的显式裁断）
 *
 * 坐标：全局位号；片长 = 32768（msb）。4 片靶场 = 131072 bit。
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/fs_allocator.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <forward_list>

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

static slices_cfg_t make_cfg(Raw &blk, Raw &ino, uint32_t cpus = 4) {
    slices_cfg_t cfg{};
    cfg.block_bits_raw = blk.mem;
    cfg.block_bits_count = BITS;
    cfg.inode_bits_raw = ino.mem;
    cfg.inode_bits_count = BITS;
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
    puts("[1] ctor 分片推导 / preferred / all_syn");

    {   /* 4 片：cap=4，n=4 */
        Raw b(BITS), i(BITS);
        slices_cfg_t cfg = make_cfg(b, i, 4);
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        CHECK(rc == RC_OK, "ctor 4-slice ok");
        CHECK(a.slice_count() == 4, "slice_count==4");
        CHECK(a.cpu_slots() == 4, "cpu_slots==4");
        CHECK(a.block_per_slice_bits() == MSB, "block per==32768");
        CHECK(a.inode_per_slice_bits() == MSB, "inode per==32768");
        for (uint32_t c = 0; c < 4; c++) {
            CHECK(a.block_preferred_of(c) == c, "block preferred init");
            CHECK(a.inode_preferred_of(c) == c, "inode preferred init");
        }
        CHECK(a.block_free_total() == 0, "all-occupied block free==0");
        CHECK(a.inode_free_total() == 0, "all-occupied inode free==0");
        CHECK(a.block_test(0) == true, "block 0 occupied");
        CHECK(a.inode_test(0) == true, "inode 0 (root) occupied");
        CHECK(a.all_syn() == RC_ERR_NOT_SUPPORTED, "all_syn: no IO surface");
    }
    {   /* 满片数约束把 cpu 上界压到 1：3*msb 位 / cpu4 但 cap=... 见下 */
        Raw b(MSB), i(MSB);
        slices_cfg_t cfg = make_cfg(b, i, 4);
        /* 手工修正 count（Raw 按 BITS 分配，但 cfg 只喂 MSB 位） */
        cfg.block_bits_count = MSB; cfg.inode_bits_count = MSB;
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        CHECK(rc == RC_OK, "ctor 1-slice ok");
        CHECK(a.slice_count() == 1, "cap=1 -> slice_count==1");
        CHECK(a.block_per_slice_bits() == MSB, "1-slice per==32768");
    }
    {   /* cap=3 压 2^n 上界 4 -> 3 */
        const uint64_t N3 = MSB * 3;
        Raw b(N3), i(N3);
        slices_cfg_t cfg{};
        cfg.block_bits_raw = b.mem; cfg.block_bits_count = N3;
        cfg.inode_bits_raw = i.mem; cfg.inode_bits_count = N3;
        cfg.logical_processor_count = 4;
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        CHECK(rc == RC_OK, "ctor 3-slice ok");
        CHECK(a.slice_count() == 3, "cap=3 -> slice_count==3");
    }
    {   /* 参数缺失：空态 + RC_ERR_STATE */
        slices_cfg_t cfg{};
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        CHECK(rc == RC_ERR_STATE, "null raw -> RC_ERR_STATE");
        CHECK(a.slice_count() == 0, "null raw -> empty");
        CHECK(a.block_free_total() == 0, "empty free_total==0");
        CHECK(a.block_test(0) == false, "empty test==false");
    }
    return 0;
}

/* ═══ 2. block_alloc_hard：命中 / 全败 / 单片原子 ═══ */
static int test_block_alloc_hard() {
    puts("[2] block_alloc_hard：单片命中 / 全败 / 不跨片");
    /* ⚠ 原始位图只在 ctor 被消费一次（enable_slice 建树即弃）——所有空闲位
     * 必须在构造前挖好；构造后再改 raw 不会反映到树里。 */
    Raw b(BITS), i(BITS);
    b.clear(100, 8);                    /* 单片命中靶：[100,108) */
    b.clear(32764, 4);                  /* 跨片原子靶：slice0 尾 [32764,32768) */
    b.clear(32768, 4);                  /*             slice1 首 [32768,32772) */
    slices_cfg_t cfg = make_cfg(b, i, 4);
    uint64_t rc = 1;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");
    CHECK(a.block_free_total() == 16, "free 8+4+4=16");

    CHECK(a.block_alloc_hard(8, 0) == 100, "hard hit @100");
    for (uint64_t k = 100; k < 108; k++) CHECK(a.block_test(k) == true, "consumed bit occupied");
    CHECK(a.block_free_total() == 8, "free 16-8==8");

    /* 剩两片各 4 位；需 8 → 原子失败（不得跨片拼） */
    CHECK(a.block_alloc_hard(8, 0) == PTR_INVALID, "hard never spans slices / all fail");
    /* 单片 4 位可以：best-fit 取最小 len≥4 者（len 升序，base 辅） */
    CHECK(a.block_alloc_hard(4, 0) == 32764, "hard single-slice 4 ok @32764");
    CHECK(a.block_alloc_hard(4, 0) == 32768, "hard other slice 4 ok @32768");
    CHECK(a.block_free_total() == 0, "free drained");
    CHECK(a.block_alloc_hard(1, 0) == PTR_INVALID, "hard full -> PTR_INVALID");
    return 0;
}

/* ═══ 3. block_alloc_soft：多段凑满 + 不够回滚 ═══ */
static int test_block_alloc_soft() {
    puts("[3] block_alloc_soft：跨片多段凑满 / 不够回滚");
    Raw b(BITS), i(BITS);
    b.clear(32760, 4);                  /* slice0 [32760,32764) */
    b.clear(32768, 4);                  /* slice1 [32768,32772) */
    b.clear(65536, 4);                  /* slice2 [65536,65540) */
    b.clear(98304, 8);                  /* slice3 [98304,98312) */
    slices_cfg_t cfg = make_cfg(b, i, 4);
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

    /* 只剩 8，却要 12 → 穷尽所有片仍不够 → 回滚（all-or-nothing） */
    CHECK(a.block_alloc_soft(12, 0, &runs) == RC_ERR_NO_BLOCK, "soft not enough -> NO_BLOCK");
    CHECK(runs.empty(), "soft rollback: runs emptied");
    CHECK(a.block_free_total() == 8, "soft rollback: 账复原==8");

    /* 门闸 */
    CHECK(a.block_alloc_soft(4, 0, nullptr) == RC_ERR_STATE, "soft nullptr runs");
    CHECK(a.block_alloc_soft(0, 0, &runs) == RC_ERR_STATE, "soft zero count");
    return 0;
}

/* ═══ 4. block_occupy_soft：跨片续圈 / 撞占用停 / 锚点占用 ═══ */
static int test_block_occupy_soft() {
    puts("[4] block_occupy_soft：跨片续圈 / 停在占用 / 锚点占用");
    {   /* 跨片连续空洞 [32760,32776) → 续圈占领 16 */
        Raw b(BITS), i(BITS);
        b.clear(32760, 16);
        slices_cfg_t cfg = make_cfg(b, i, 4);
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        CHECK(rc == RC_OK, "ctor ok");
        CHECK(a.block_free_total() == 16, "free 16");
        uint64_t rest = 999;
        CHECK(a.block_occupy_soft(32760, 16, &rest) == RC_OK, "occupy cross-slice ok");
        CHECK(rest == 0, "occupy fully satisfied");
        for (uint64_t k = 0; k < 16; k++) CHECK(a.block_test(32760 + k) == true, "cross-slice bit occupied");
        CHECK(a.block_free_total() == 0, "free drained");
        /* 跨片释放（同一连续 run 逆向） */
        CHECK(a.block_free(32760, 16) == RC_OK, "cross-slice free ok");
        CHECK(a.block_free_total() == 16, "free restored 16");
        for (uint64_t k = 0; k < 16; k++) CHECK(a.block_test(32760 + k) == false, "freed bits free");
    }
    {   /* 片尾之后即占用：占领到片尾为止，rest 记账 */
        Raw b(BITS), i(BITS);
        b.clear(32760, 8);              /* [32760,32768) 直到 slice0 尾 */
        slices_cfg_t cfg = make_cfg(b, i, 4);
        uint64_t rc = 1;
        fs_allocator_t a(cfg, &rc);
        uint64_t rest = 999;
        CHECK(a.block_occupy_soft(32760, 16, &rest) == RC_OK, "occupy partial-prefix ok");
        CHECK(rest == 8, "rest==8 (stopped at occupied next slice)");
        for (uint64_t k = 0; k < 8; k++) CHECK(a.block_test(32760 + k) == true, "claimed bits");
        CHECK(a.block_test(32768) == true, "next slice start untouched (was occupied)");
        CHECK(a.block_free_total() == 0, "free drained");
    }
    {   /* 锚点即占用 → NO_BLOCK，rest==require */
        Raw b(BITS), i(BITS);
        slices_cfg_t cfg = make_cfg(b, i, 4);
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
    Raw b(BITS), i(BITS);
    b.clear(32760, 8);                  /* slice0 尾 [32760,32768)，直到片尾 */
    b.clear(32768, 8);                  /* slice1   [32768,32776)，与上成 16 连续跨界 */
    slices_cfg_t cfg = make_cfg(b, i, 4);
    uint64_t rc = 1;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");
    CHECK(a.block_free_total() == 16, "free 8+8=16");

    /* 跨片续圈占领 12：slice0 到片尾 8，续 slice1 再取 4，真实连续 */
    uint64_t rest = 999;
    CHECK(a.block_occupy_soft(32760, 12, &rest) == RC_OK, "occupy cross-slice 12");
    CHECK(rest == 0, "rest 0");
    for (uint64_t k = 0; k < 12; k++) CHECK(a.block_test(32760 + k) == true, "occupied cross-slice");
    CHECK(a.block_free_total() == 4, "free 16-12==4");

    /* block_free 单段跨片 */
    CHECK(a.block_free(32760, 12) == RC_OK, "block_free cross-slice ok");
    CHECK(a.block_free_total() == 16, "free restored 16");
    for (uint64_t k = 0; k < 12; k++) CHECK(a.block_test(32760 + k) == false, "freed bits free");
    CHECK(a.block_free(40000, 0) == RC_OK, "free count==0 no-op");

    /* block_intervals_free：soft 取一段 → 链式归还 */
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

/* ═══ 6. inode_alloc / inode_free / inode_test ═══ */
static int test_inode() {
    puts("[6] inode_alloc / inode_free / inode_test");
    Raw b(BITS), i(BITS);
    i.clear(10, 1);                     /* slice0 [10,11) */
    i.clear(32770, 4);                  /* slice1 [32770,32774) */
    slices_cfg_t cfg = make_cfg(b, i, 4);
    uint64_t rc = 1;
    fs_allocator_t a(cfg, &rc);
    CHECK(rc == RC_OK, "ctor ok");
    CHECK(a.inode_free_total() == 5, "inode free 1+4=5");
    CHECK(a.inode_test(0) == true, "root inode occupied in source");

    uint64_t num = PTR_INVALID;
    CHECK(a.inode_alloc(num) == RC_OK && num == 10, "inode_alloc slice0 -> 10");
    CHECK(a.inode_test(10) == true, "10 now occupied");
    CHECK(a.inode_free_total() == 4, "free 5-1==4");

    /* slice0 已空 → 亲和旋转到 slice1；[32770,32774) 的单 bit 取尾位 32773 */
    CHECK(a.inode_alloc(num) == RC_OK && num == 32773, "inode_alloc slice1 -> 32773");
    CHECK(a.inode_test(32773) == true, "32773 occupied");

    a.inode_free(10);
    CHECK(a.inode_test(10) == false, "10 freed");
    a.inode_free(32773);
    CHECK(a.inode_test(32773) == false, "32773 freed");
    CHECK(a.inode_free_total() == 5, "free restored 5");

    /* 全占用 → NO_BLOCK */
    {
        Raw bz(BITS), iz(BITS);
        slices_cfg_t cz = make_cfg(bz, iz, 4);
        uint64_t rc2 = 1;
        fs_allocator_t az(cz, &rc2);
        uint64_t n2 = PTR_INVALID;
        CHECK(az.inode_alloc(n2) == RC_ERR_NO_BLOCK, "inode full -> NO_BLOCK");
    }

    /* 越界 test */
    CHECK(a.inode_test(BITS + 5) == false, "inode_test OOB false");
    CHECK(a.block_test(BITS + 5) == false, "block_test OOB false");
    CHECK(a.block_preferred_of(99) == 0, "preferred_of OOB -> 0");
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

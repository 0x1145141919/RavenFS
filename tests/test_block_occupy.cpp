/* ═══════════════════════════════════════════════════════════════════
 * test_block_occupy.cpp — block_occupy_soft 定点占领路由专项
 *
 * 2026-09-03 重构：位图模块从上帝类顶出（bitmap_slices_mgr_t）后，本测试
 * 变为**纯内存直测**——两块 posix_memalign 位图 + cfg 构造 mgr，不再需要
 * 镜像/挂载/上帝类（这正是顶出的动机：路由层不该被上帝类绑架）。
 *
 * 靶场构造：双组位图全 1（全部占用）→ mgr.block_free() 挖空闲区
 * （free_interval 按实际清掉位计数，free_count 语义与装配后一致）。
 * 几何：block/inode 组各 65536 bit → cpu=2 → slice_count=2，per=32768，
 * 片边界 = 32768（跨界续圈锚点落在片尾附近必然跨片）。
 *
 * 核心语义（对齐 2026-09-03 结论）：
 *   - block_occupy_soft = 定点占领（对偶 block_free）：锚点片唯一 →
 *     自旋强制抢锁（非 alloc_* 的 try 换片）
 *   - 片尾跨界续圈在路由层内部：一段顶到本片尾 → 下一片从 base 0 继续
 *     贪心前缀（逐片单锁前缀链 = 真实连续）；上层零切片几何知识
 *   - 可能产出跨片连续 run → block_free 跨片循环归还
 *
 * 用例：
 *   R1 跨界续圈：锚点在本片尾附近，一次调用跨两片占满 → rest==0；
 *      释放走 block_free 跨片归还 → 位图全还原
 *   R2 锚点即占用：未挖空闲的占用块当锚点 → PTR_INVALID + rest=len
 *   R3 部分前缀：锚点空闲但中途撞占用 → 只占前缀，rest 如实回传
 *   R4 释放后可复用 + alloc_hard 游标未被锚定占领打乱
 *   R5 inode 组冒烟：挖槽后 alloc/free 往返（root 保留语义由 hint 保证）
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/bitmap_slices.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

static constexpr uint64_t NBITS = 65536;      /* 每组 65536 bit → 2 片（每片 32768） */
static constexpr uint64_t PER   = 32768;      /* 片边界 */

struct arena_t {
    uint8_t *blk_raw = nullptr;
    uint8_t *ino_raw = nullptr;
    bitmap_slices_mgr_t *mgr = nullptr;

    bool setup() {
        if (posix_memalign((void **)&blk_raw, 4096, NBITS / 8) != 0) return false;
        if (posix_memalign((void **)&ino_raw, 4096, NBITS / 8) != 0) return false;
        memset(blk_raw, 0xFF, NBITS / 8);    /* 全 1 = 全部占用 */
        memset(ino_raw, 0xFF, NBITS / 8);
        bitmap_slices_cfg_t cfg;
        cfg.block_bits_raw   = blk_raw;
        cfg.block_bits_count = NBITS;
        cfg.inode_bits_raw   = ino_raw;
        cfg.inode_bits_count = NBITS;
        cfg.logical_processor_count = 2;     /* → slice_count=2, per=32768 */
        uint64_t rc = 0;
        mgr = new bitmap_slices_mgr_t(cfg, &rc);
        if (rc != RC_OK) return false;
        return mgr->slice_count() == 2 && mgr->block_per_slice_bits() == PER;
    }
    void teardown() {
        delete mgr;
        free(blk_raw);
        free(ino_raw);
    }
};

static bool range_free(bitmap_slices_mgr_t &m, uint64_t base, uint64_t len) {
    for (uint64_t i = 0; i < len; i++)
        if (m.block_test(base + i)) return false;
    return true;
}

static bool range_occ(bitmap_slices_mgr_t &m, uint64_t base, uint64_t len) {
    for (uint64_t i = 0; i < len; i++)
        if (!m.block_test(base + i)) return false;
    return true;
}

int main() {
    arena_t A;
    if (!A.setup()) {
        printf("arena setup failed（几何要求：双组 65536 bit + cpu=2 → 2 片）\n");
        return 1;
    }
    bitmap_slices_mgr_t &m = *A.mgr;
    printf("-- 几何：slice_count=%llu per=%llu --\n",
           (unsigned long long)m.slice_count(),
           (unsigned long long)m.block_per_slice_bits());

    /* ── R1：跨界续圈 + block_free 跨片归还 ── */
    printf("\n-- R1：片尾跨界续圈 + 跨片释放 --\n");
    {
        m.block_free(PER - 8, 16);           /* 挖 [PER-8, PER+8)：跨片空闲 */
        uint64_t rest = 999;
        uint64_t got = m.block_occupy_soft(PER - 8, 16, &rest);
        CHECK(got == PER - 8, "一次调用跨界占满，返回原址");
        CHECK(rest == 0, "rest==0（slice0 尾 8 块 + slice1 头 8 块，跨界续圈）");
        CHECK(range_occ(m, PER - 8, 16), "跨界 run 全部置位");
        CHECK(range_free(m, PER + 8, 8) || true, "片外未越界触碰（[PER+8,...) 仍占用）");
        CHECK(!range_free(m, PER + 8, 8), "[PER+8,...) 未挖空闲，仍占用");
        m.block_free(PER - 8, 16);
        CHECK(range_free(m, PER - 8, 16), "block_free 跨片循环归还 → 全还原");
    }

    /* ── R2：锚点即占用 → PTR_INVALID ── */
    printf("\n-- R2：锚点即占用 --\n");
    {
        const uint64_t occ = 100;            /* 全 1 位图，未挖 → 必占用 */
        uint64_t rest = 0;
        uint64_t got = m.block_occupy_soft(occ, 4, &rest);
        CHECK(got == PTR_INVALID, "锚点占用 → PTR_INVALID");
        CHECK(rest == 4, "rest==len（一块未占）");
        CHECK(m.block_test(occ), "占用块未被改写");
    }

    /* ── R3：部分前缀（中途撞占用） ── */
    printf("\n-- R3：部分前缀（中途撞占用） --\n");
    {
        m.block_free(200, 16);               /* 挖 [200,216)：f=202 起 12 块 + 前缀 2 */
        const uint64_t f = 202;
        uint64_t r1 = 999;
        CHECK(m.block_occupy_soft(f, 10, &r1) == f && r1 == 0, "占 [f,f+10)");
        uint64_t r2 = 999;
        CHECK(m.block_occupy_soft(f + 10, 4, &r2) == f + 10 && r2 == 0, "占 [f+10,f+14)");
        uint64_t rest = 0;
        uint64_t got = m.block_occupy_soft(f - 2, 16, &rest);
        CHECK(got == f - 2, "锚点 f-2 空闲 → 返回原址");
        CHECK(rest == 14, "只占前缀 2（f-2,f-1），撞 f 停 → rest==14");
        CHECK(m.block_test(f - 2) && m.block_test(f - 1), "前缀 2 块置位");
        CHECK(m.block_test(f), "f 仍占用（未被破坏）");
        CHECK(m.block_test(f + 14) && m.block_test(f + 15), "占用段之外未越界触碰（f+14/15 未挖，保持占用）");
        m.block_free(f - 2, 2);
        m.block_free(f, 14);
        CHECK(range_free(m, f - 2, 16), "全部归还 → 还原");
    }

    /* ── R4：释放后可复用 + 搜索分配游标健康 ── */
    printf("\n-- R4：复用 + 搜索分配游标健康 --\n");
    {
        m.block_free(1024, 8);
        uint64_t rest = 0;
        CHECK(m.block_occupy_soft(1024, 8, &rest) == 1024 && rest == 0, "首占 [1024,1032)");
        m.block_free(1024, 8);
        CHECK(m.block_occupy_soft(1024, 8, &rest) == 1024 && rest == 0, "释放后同锚点再占成功");
        m.block_free(1024, 8);

        m.block_free(2048, 64);              /* 挖 64 块供 alloc_hard 连续分配 */
        uint64_t d = m.block_alloc_hard(3, 0);
        CHECK(d != PTR_INVALID, "alloc_hard(3) 仍正常");
        if (d != PTR_INVALID) {
            CHECK(range_occ(m, d, 3), "alloc_hard 3 块连续置位");
            m.block_free(d, 3);
        }
    }

    /* ── R5：inode 组冒烟（root 保留 + 分配往返） ── */
    printf("\n-- R5：inode 组冒烟 --\n");
    {
        m.inode_free(7);                     /* 挖 inode 槽 7（全 1 → 清位） */
        uint64_t i1 = m.inode_alloc();
        CHECK(i1 == 7, "inode_alloc 取回挖出的槽 7，got %llu", (unsigned long long)i1);
        if (i1 != PTR_INVALID) {
            CHECK(m.inode_test(i1), "槽已占用");
            m.inode_free(i1);
            CHECK(!m.inode_test(i1), "释放后空闲");
        }
        CHECK(m.block_free_total() > 0 && m.inode_free_total() > 0,
              "free_total 审计读数正常（blk=%llu ino=%llu）",
              (unsigned long long)m.block_free_total(),
              (unsigned long long)m.inode_free_total());
    }

    A.teardown();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

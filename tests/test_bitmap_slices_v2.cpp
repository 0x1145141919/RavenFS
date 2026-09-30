/* ═══════════════════════════════════════════════════════════════════
 * test_bitmap_slices_v2.cpp — chunk_slice_mgr_t（v2 窗口版 slice）内存模拟盘自测
 *
 * 2026-09-30。独立 main（**未接 CMake**，手工编译）：
 *   g++ -std=c++17 -Wall -Wextra -Iinclude/ravenfs \
 *       tests/test_bitmap_slices_v2.cpp juvenile_god/bitmap_slices_v2.cpp \
 *       -o /tmp/bsv2_test && /tmp/bsv2_test
 *
 * 覆盖（内存模拟盘 = read/write 回调 + 位图镜像）：
 *   门闸（enable 前/双 enable/shutdown 后全服务 RC_ERR_STATE）→
 *   enable 整片全量读实测 free_count + 填摘要（恰好 1 次读）→
 *   alloc_hard：摘要 ALL_USED 免载入跳窗 + ALL_FREE 免读(memset)换片命中 →
 *   alloc 对齐（align_log2=7）→ free_interval 在线/离线一律挂链（零 I/O、记账）→
 *   在线窗操作前 drain（alloc 复用被释放位）→ 离线窗换片 drain-before-publish
 *   （盘上全 1 的窗里只靠记录还出来的 3 位命中）→
 *   test_interval 摘要裁决免载入 → soft_occupy 锚定前缀/撞占用/ALL_USED 免载入断 →
 *   alloc_soft 窗口内截断缺口回传 → free_count gate →
 *   chunks_asyn 1 读 1 写 + 整片重扫校准（free_count == 盘上实测 0 位数）→
 *   out_calib 校准回报（人为重复 free 制造漂移 → old != new 且 new == 盘实测）
 *   + 对齐上界 align_log2>15 入口拒 RC_ERR_STATE →
 *   shutdown 落盘残留记录 → 重新 enable 自洽（轮转对账）。
 * 几何 B：order=2（chunk=16K）+ 6 块 ⇒ 尾窗（tail=65536 位）专项。
 *
 * + mgr 级路由（bitmap_slices_mgr_t_v2；按 cfg 绝对 FS_block 寻址的模拟盘）：
 *   ctor 装配 → alloc_hard 命中（全局号合法 + preferred 亲和）→ block_free
 *   跨片逐段 → occupy 跨片续圈 / 撞占用停 → 填片后复用 → alloc_soft 缺口回传
 *   → inode_alloc/free（无 root 保留）→ free_total 双组自洽 → dtor 落盘对账。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/bitmap_slices_v2.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            ++g_fail;                                                 \
            std::printf("  [FAIL] line %d: ", __LINE__);              \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
        }                                                             \
    } while (0)

/* ── 内存模拟盘：data = 本片位图区（base 起 blocks × 4096B），rd/wr 计数 ── */
struct mock_disk_t {
    std::vector<uint8_t> data;
    uint64_t base = 0;   /* 本片位图区起始 FS_block */
    uint64_t blocks = 0;
    uint64_t rd = 0, wr = 0;
    unsigned slow_us = 0;   /* >0 则读时慢睡（持锁等 I/O 窗口，供 BUSY 测试） */
};

static result_code_t mock_read(void *ctx, uint64_t blk, uint64_t len, void *buf) {
    mock_disk_t *d = static_cast<mock_disk_t *>(ctx);
    if (len == 0 || blk < d->base || blk - d->base + len > d->blocks) return RC_ERR_IO;
    if (d->slow_us)
        std::this_thread::sleep_for(std::chrono::microseconds(d->slow_us));
    std::memcpy(buf, &d->data[(blk - d->base) * 4096], (size_t)len * 4096);
    d->rd++;
    return RC_OK;
}
static result_code_t mock_write(void *ctx, uint64_t blk, uint64_t len, const void *buf) {
    mock_disk_t *d = static_cast<mock_disk_t *>(ctx);
    if (len == 0 || blk < d->base || blk - d->base + len > d->blocks) return RC_ERR_IO;
    std::memcpy(&d->data[(blk - d->base) * 4096], buf, (size_t)len * 4096);
    d->wr++;
    return RC_OK;
}

static bool dbit(const mock_disk_t &d, uint64_t idx) {
    return ((d.data[idx >> 3] >> (idx & 7)) & 1u) != 0;
}
static void dset(mock_disk_t &d, uint64_t idx) {
    d.data[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}
static uint64_t dzeros(const mock_disk_t &d) {
    uint64_t z = 0;
    for (size_t i = 0; i < d.data.size(); i++)
        for (int b = 0; b < 8; b++)
            if (((d.data[i] >> b) & 1u) == 0) z++;
    return z;
}
/* 盘上 [lo,hi) 全为 v？（越界 = false 报警） */
static bool dall(const mock_disk_t &d, uint64_t lo, uint64_t hi, bool v) {
    if (hi > d.data.size() * 8) return false;
    for (uint64_t i = lo; i < hi; i++)
        if (dbit(d, i) != v) return false;
    return true;
}

/* ═══ 几何 A：order=0（chunk = 4K = 32768 位），4 窗，片 131072 位 ═══ */
static void test_geometry_a() {
    std::printf("[几何A] order=0 / 4 chunk / 片 131072 位\n");
    mock_disk_t d;
    d.base = 0;
    d.blocks = 4;
    d.data.assign((size_t)d.blocks * 4096, 0);
    for (uint64_t i = 0; i < 32768; i++) dset(d, i);          /* 窗0 = 全 1（ALL_USED） */
    for (uint64_t i = 65536; i < 98304; i++) dset(d, i);      /* 窗2 = 全 1（ALL_USED） */
    /* 窗1 / 窗3 = 全 0（ALL_FREE） */

    slice_soul_t soul{};
    soul.read_block = mock_read;
    soul.write_block = mock_write;
    soul.io_ctx = &d;
    soul.chunk_base_blk = 0;
    soul.chunk_blk_length = 4;
    soul.chunk_order = 0;

    uint64_t out = 0, left = 0, rest = 0;
    chunk_slice_mgr_t s;

    /* ── 门闸：enable 前全服务拒 ── */
    CHECK(s.alloc_hard(0, 1, out) == RC_ERR_STATE, "alloc before enable");
    CHECK(s.alloc_soft(0, 1, out, left) == RC_ERR_STATE, "alloc_soft before enable");
    CHECK(s.free_interval(0, 1) == RC_ERR_STATE, "free before enable");
    CHECK(s.test_interval(true, 0, 1) == RC_ERR_STATE, "test before enable");
    CHECK(s.soft_occupy(0, 1, &rest) == RC_ERR_STATE, "occupy before enable");
    CHECK(s.chunks_asyn() == RC_ERR_STATE, "asyn before enable");
    CHECK(s.shutdown() == RC_ERR_STATE, "shutdown before enable");
    CHECK(s.get_free_count() == 0, "free_count before enable");

    /* ── enable：整片全量读一趟实测（恰好 1 读 0 写）── */
    CHECK(s.enable(&soul, 777) == RC_OK, "enable rc");
    CHECK(s.in_global_base_idx == 777, "in_global_base_idx=%llu",
          (unsigned long long)s.in_global_base_idx);
    CHECK(s.get_free_count() == 65536, "enable free=%llu (want 65536)",
          (unsigned long long)s.get_free_count());
    CHECK(d.rd == 1 && d.wr == 0, "io after enable rd=%llu wr=%llu (want 1/0)",
          (unsigned long long)d.rd, (unsigned long long)d.wr);
    CHECK(s.enable(&soul, 777) == RC_ERR_STATE, "double enable");

    /* ── alloc_hard：hint=0 → 窗0 摘要 ALL_USED 免载入跳过、窗1 ALL_FREE 免读
     *    memset 换片命中 → out=32768，仍 0 次额外 I/O ── */
    CHECK(s.alloc_hard(0, 10, out) == RC_OK, "alloc_hard win1");
    CHECK(out == 32768, "out=%llu (want 32768)", (unsigned long long)out);
    CHECK(s.get_free_count() == 65526, "free=%llu (want 65526)",
          (unsigned long long)s.get_free_count());
    CHECK(s.get_hint() == 32778, "hint=%llu (want 32778)", (unsigned long long)s.get_hint());
    CHECK(d.rd == 1, "no extra read rd=%llu (want 1)", (unsigned long long)d.rd);

    /* ── test_interval：在线窗直读 / 离线窗摘要裁决（均零 I/O）── */
    CHECK(s.test_interval(true, 32768, 10) == RC_OK, "test online used");
    CHECK(s.test_interval(false, 32768, 1) == RC_ERR_NOT_FOUND, "test online free-bit");
    CHECK(s.test_interval(true, 0, 1) == RC_OK, "test offline ALL_USED shortcut");
    CHECK(s.test_interval(false, 0, 1) == RC_ERR_NOT_FOUND, "test offline mismatch");
    CHECK(d.rd == 1, "test zero-io rd=%llu", (unsigned long long)d.rd);

    /* ── 对齐分配：hint=40000 → align 128 上取整 = 40064 ── */
    s.set_hint(40000);
    CHECK(s.alloc_hard(7, 5, out) == RC_OK, "alloc_hard align");
    CHECK(out == 40064 && (out % 128) == 0, "align out=%llu (want 40064)",
          (unsigned long long)out);
    CHECK(s.get_free_count() == 65521, "free=%llu (want 65521)",
          (unsigned long long)s.get_free_count());

    /* ── 在线窗 free：一律挂链（零 I/O、记账 +5）→ alloc 前 drain → 复用同位 ── */
    CHECK(s.free_interval(40064, 5) == RC_OK, "free online");
    CHECK(s.get_free_count() == 65526, "free after挂链=%llu (want 65526)",
          (unsigned long long)s.get_free_count());
    CHECK(d.rd == 1, "free zero-io rd=%llu", (unsigned long long)d.rd);
    s.set_hint(40064);
    CHECK(s.alloc_hard(0, 5, out) == RC_OK, "reuse alloc");
    CHECK(out == 40064, "reuse out=%llu (want 40064, drain 先于扫描)",
          (unsigned long long)out);
    CHECK(s.get_free_count() == 65521, "free=%llu (want 65521)",
          (unsigned long long)s.get_free_count());
    CHECK(s.test_interval(true, 40064, 5) == RC_OK, "reused bits set");

    /* ── 离线窗 free：挂链（盘上仍全 1）→ 换片 drain-before-publish ── */
    CHECK(s.free_interval(70000, 3) == RC_OK, "free offline");
    CHECK(s.get_free_count() == 65524, "free=%llu (want 65524)",
          (unsigned long long)s.get_free_count());
    CHECK(d.rd == 1 && d.wr == 0, "free zero-io rd=%llu wr=%llu",
          (unsigned long long)d.rd, (unsigned long long)d.wr);
    s.set_hint(65536);
    const uint64_t rd0 = d.rd, wr0 = d.wr;
    CHECK(s.alloc_hard(0, 3, out) == RC_OK, "switch-window alloc");
    CHECK(out == 70000, "out=%llu (want 70000 —— 只可能来自 publish 前 drain)",
          (unsigned long long)out);
    CHECK(d.wr == wr0 + 1, "evict dirty write wr=%llu (want +%llu)",
          (unsigned long long)d.wr, (unsigned long long)(d.wr - wr0));
    CHECK(d.rd == rd0 + 1, "load read rd=%llu", (unsigned long long)d.rd);
    CHECK(s.get_free_count() == 65521, "free=%llu (want 65521)",
          (unsigned long long)s.get_free_count());
    /* 窗1 脏窗已回写：32768..32777 / 40064..40068 落盘为 1 */
    CHECK(dall(d, 32768, 32778, true) && dall(d, 40064, 40069, true),
          "evicted win1 persisted");

    /* ── soft_occupy：窗0 离线 ALL_USED 免载入直接断（rest 如实）── */
    rest = 0;
    const uint64_t rd1 = d.rd;
    CHECK(s.soft_occupy(0, 5, &rest) == RC_ERR_NO_BLOCK, "occupy ALL_USED skip");
    CHECK(rest == 5, "occupy rest=%llu (want 5)", (unsigned long long)rest);
    CHECK(d.rd == rd1, "occupy zero-io rd=%llu", (unsigned long long)d.rd);
    /* 锚点在全 0 窗3 → 换片（ALL_FREE 免读）+ 认领 50 位 */
    rest = 99;   /* 口径防御：*rest > len → STATE */
    CHECK(s.soft_occupy(100000, 50, &rest) == RC_ERR_STATE, "occupy rest>len");
    rest = 0;
    CHECK(s.soft_occupy(100000, 50, &rest) == RC_OK, "occupy win3");
    CHECK(rest == 0, "occupy rest=%llu (want 0)", (unsigned long long)rest);
    CHECK(d.rd == rd1, "occupy ALL_FREE 免读 rd=%llu", (unsigned long long)d.rd);
    CHECK(s.test_interval(true, 100000, 50) == RC_OK, "occupied bits");
    /* 撞占用：同锚点再占 → 0 位 */
    rest = 0;
    CHECK(s.soft_occupy(100000, 10, &rest) == RC_ERR_NO_BLOCK, "occupy hit-used");
    CHECK(rest == 10, "occupy rest=%llu (want 10)", (unsigned long long)rest);

    /* ── alloc_soft：hint=100050（occupy 推进）→ 窗3 段尾截断，缺口回传 ── */
    CHECK(s.get_hint() == 100050, "occupy hint=%llu (want 100050)",
          (unsigned long long)s.get_hint());
    CHECK(s.alloc_soft(0, 40000, out, left) == RC_OK, "alloc_soft");
    CHECK(out == 100050 && left == 8978, "soft out=%llu left=%llu (want 100050/8978)",
          (unsigned long long)out, (unsigned long long)left);

    /* ── free_count gate / 参数校验 ── */
    CHECK(s.alloc_hard(0, s.get_free_count() + 1, out) == RC_ERR_NO_BLOCK, "gate");
    CHECK(s.alloc_hard(0, 0, out) == RC_ERR_STATE, "count=0");
    CHECK(s.alloc_hard(0, 131073, out) == RC_ERR_STATE, "count>slice");
    CHECK(s.alloc_hard(0, 70000, out) == RC_ERR_NO_BLOCK, "count>单窗容量（不跨窗）");
    CHECK(s.test_interval(true, 131071, 2) == RC_ERR_STATE, "test 越界");
    CHECK(s.free_interval(131070, 3) == RC_ERR_STATE, "free 越界");
    CHECK(s.free_interval(0, 0) == RC_OK, "free len=0");

    /* ── chunks_asyn：1 读 1 写 + 整片重扫校准（free_count == 盘上 0 位数）──
     *    离线窗2 已于 asyn 摘要重算：全 1 → ALL_USED（免载入依据保真）── */
    const uint64_t rd2 = d.rd, wr2 = d.wr;
    CHECK(s.chunks_asyn() == RC_OK, "chunks_asyn");
    CHECK(d.rd == rd2 + 1 && d.wr == wr2 + 1, "asyn 1读1写 rd=%llu wr=%llu",
          (unsigned long long)d.rd, (unsigned long long)d.wr);
    CHECK(s.get_free_count() == dzeros(d), "asyn free=%llu vs 盘实测 %llu",
          (unsigned long long)s.get_free_count(), (unsigned long long)dzeros(d));
    CHECK(dall(d, 70000, 70003, true), "asyn 盘上 70000..70002 = 1");
    CHECK(dall(d, 40064, 40069, true), "asyn 盘上 40064..40068 = 1");
    CHECK(dall(d, 100000, 100050, true), "asyn 盘上占用段 = 1");
    CHECK(dall(d, 98304, 100000, false), "asyn 盘上空闲段 = 0");
    {
        const uint64_t rd3 = d.rd;
        CHECK(s.test_interval(true, 70000, 3) == RC_OK,
              "asyn 后窗2 摘要 ALL_USED 免载入测试");
        CHECK(d.rd == rd3, "summary shortcut rd=%llu", (unsigned long long)d.rd);
    }

    /* ── shutdown：残留离线记录落盘 → 全服务回 STATE ── */
    const uint64_t free_before = s.get_free_count();   /* 挂 3 位之前的基线 */
    CHECK(s.free_interval(100, 3) == RC_OK, "free pending before shutdown");
    CHECK(s.get_free_count() == free_before + 3, "挂链记账 +3");
    CHECK(dall(d, 100, 103, true), "pending 位盘上仍 1");
    const uint64_t rd4 = d.rd, wr4 = d.wr;
    CHECK(s.shutdown() == RC_OK, "shutdown");
    CHECK(d.rd == rd4 + 1 && d.wr == wr4 + 1, "shutdown 落盘 1读1写 rd=%llu wr=%llu",
          (unsigned long long)d.rd, (unsigned long long)d.wr);
    CHECK(dall(d, 100, 103, false), "shutdown 已把记录写盘（100..102 = 0）");
    CHECK(s.get_free_count() == 0, "free after shutdown=%llu",
          (unsigned long long)s.get_free_count());
    CHECK(s.alloc_hard(0, 1, out) == RC_ERR_STATE, "alloc after shutdown");
    CHECK(s.free_interval(0, 1) == RC_ERR_STATE, "free after shutdown");
    CHECK(s.test_interval(true, 0, 1) == RC_ERR_STATE, "test after shutdown");
    CHECK(s.soft_occupy(0, 1, &rest) == RC_ERR_STATE, "occupy after shutdown");
    CHECK(s.chunks_asyn() == RC_ERR_STATE, "asyn after shutdown");

    /* ── 重新 enable：整片实测应等于盘上 0 位数（轮转对账自洽）── */
    CHECK(s.enable(&soul, 0) == RC_OK, "re-enable");
    CHECK(s.get_free_count() == dzeros(d), "re-enable free=%llu vs 盘 %llu",
          (unsigned long long)s.get_free_count(), (unsigned long long)dzeros(d));
    CHECK(s.get_free_count() == free_before + 3,
          "re-enable free=%llu = shutdown 前 +3（挂链已落盘）",
          (unsigned long long)s.get_free_count());
    CHECK(s.test_interval(false, 100, 3) == RC_OK, "re-enable 100..102 空闲");
    CHECK(s.test_interval(true, 100, 1) == RC_ERR_NOT_FOUND, "re-enable 100 已用位");
    CHECK(s.shutdown() == RC_OK, "final shutdown");
}

/* ═══ 几何 B：order=2（chunk = 16K = 131072 位），6 块 ⇒ 2 窗，尾窗 65536 位 ═══
 *    尾块公式验算：tail = 32768 × (6 mod 4) = 65536；盘块 = base + ctl<<order。 */
static void test_geometry_b() {
    std::printf("[几何B] order=2 / 6 块 / 片 196608 位 / 尾窗 65536 位\n");
    mock_disk_t d;
    d.base = 4;
    d.blocks = 6;
    d.data.assign((size_t)d.blocks * 4096, 0);            /* 全 0（ALL_FREE） */

    slice_soul_t soul{};
    soul.read_block = mock_read;
    soul.write_block = mock_write;
    soul.io_ctx = &d;
    soul.chunk_base_blk = 4;
    soul.chunk_blk_length = 6;
    soul.chunk_order = 2;

    uint64_t out = 0;
    chunk_slice_mgr_t s;
    CHECK(s.enable(&soul, 1000000) == RC_OK, "enable B");
    CHECK(s.get_free_count() == 196608, "B free=%llu (want 196608)",
          (unsigned long long)s.get_free_count());
    CHECK(d.rd == 1, "B enable 1 读 rd=%llu", (unsigned long long)d.rd);

    /* 单窗上限：196608 位 > 单窗最大 131072 → 不跨窗硬拼，NO_BLOCK */
    CHECK(s.alloc_hard(0, 196608, out) == RC_ERR_NO_BLOCK, "B count>单窗");
    CHECK(s.alloc_hard(0, 196609, out) == RC_ERR_STATE, "B count>片");

    /* 尾窗专项：hint 顶到尾窗首 → 命中片尾段（免读换片 = memset 0 免 I/O） */
    s.set_hint(131072);
    const uint64_t rd0 = d.rd;
    CHECK(s.alloc_hard(0, 1000, out) == RC_OK, "B tail alloc");
    CHECK(out == 131072, "B out=%llu (want 131072)", (unsigned long long)out);
    CHECK(d.rd == rd0, "B ALL_FREE 免读 rd=%llu", (unsigned long long)d.rd);
    CHECK(s.test_interval(true, 131072, 1000) == RC_OK, "B tail bits");

    /* 尾窗 free → 挂链 → 复用（片内偏移 196000 在尾窗内，196000+100=196100 < 196608） */
    CHECK(s.free_interval(196000, 100) == RC_OK, "B free tail");
    s.set_hint(196000);
    CHECK(s.alloc_hard(0, 100, out) == RC_OK, "B tail reuse");
    CHECK(out == 196000, "B reuse out=%llu (want 196000)", (unsigned long long)out);

    /* 盘边界：尾窗尾段盘块 = 4 + 1<<2 = 8..9（2 块 = 65536 位），越界 I/O 会被 mock 拒 */
    CHECK(s.chunks_asyn() == RC_OK, "B asyn");
    CHECK(s.get_free_count() == dzeros(d), "B asyn free=%llu vs 盘 %llu",
          (unsigned long long)s.get_free_count(), (unsigned long long)dzeros(d));
    CHECK(dall(d, 196000, 196100, true), "B 盘上尾段复用位 = 1");
    CHECK(s.shutdown() == RC_OK, "B shutdown");
}

/* ═══ out_calib 校准回报 + 对齐上界（0..15）：人为重复 free 制造 free_count 漂移 ═══
 *    slice 层允许重复 free（fs 全局不允许）→ 挂链即 +len 使 free_count 虚高 →
 *    chunks_asyn 整片重扫校准，out_calib 回报 {旧, 新} 供外部判定不同步。 */
static void test_calib_out() {
    std::printf("[校准] out_calib 漂移回报 + align_log2>15 拒\n");
    mock_disk_t d;
    d.base = 0;
    d.blocks = 4;
    d.data.assign((size_t)d.blocks * 4096, 0);          /* 全 0（ALL_FREE） */

    slice_soul_t soul{};
    soul.read_block = mock_read;
    soul.write_block = mock_write;
    soul.io_ctx = &d;
    soul.chunk_base_blk = 0;
    soul.chunk_blk_length = 4;
    soul.chunk_order = 0;

    uint64_t out = 0, left = 0;
    chunk_slice_mgr_t s;
    CHECK(s.enable(&soul, 0) == RC_OK, "enable calib");

    /* 对齐上界：>15 入口拒（RC_ERR_STATE，不静默截断）；=15 合法且命中片首 */
    CHECK(s.alloc_hard(16, 1, out) == RC_ERR_STATE, "align16 hard = STATE");
    CHECK(s.alloc_soft(16, 1, out, left) == RC_ERR_STATE, "align16 soft = STATE");
    CHECK(s.alloc_hard(15, 1, out) == RC_OK && out == 0, "align15 hard out=%llu",
          (unsigned long long)out);

    /* 重复 free：同一区间 free 两次（slice 层允许）→ 挂链即 +len → 记账虚高 */
    CHECK(s.free_interval(0, 1) == RC_OK, "free #1");
    CHECK(s.free_interval(0, 1) == RC_OK, "free #2（重复，slice 层允许）");
    CHECK(s.get_free_count() == 131073, "free 漂移=%llu (want 131073)",
          (unsigned long long)s.get_free_count());

    /* asyn 校准回报：old = 漂移记账值，new = 整片重扫实测 0 位数（漂移被检出） */
    free_count_calib_t calib{};
    CHECK(s.chunks_asyn(&calib) == RC_OK, "asyn out_calib");
    CHECK(calib.old_free_count == 131073, "calib.old=%llu (want 131073)",
          (unsigned long long)calib.old_free_count);
    CHECK(calib.new_free_count == dzeros(d), "calib.new=%llu vs 盘整片实测 %llu",
          (unsigned long long)calib.new_free_count, (unsigned long long)dzeros(d));
    CHECK(calib.old_free_count != calib.new_free_count, "漂移被检出（old != new）");
    CHECK(s.get_free_count() == calib.new_free_count, "校准后 free_count = new");
    CHECK(s.shutdown() == RC_OK, "shutdown calib");
}

/* ═══ 锁纪律：alloc 走 try_lock（拿不到 = 本片忙 → RC_ERR_BUSY），free/asyn 走阻塞 ═══
 *    手法：后台线程跑 chunks_asyn（读回调慢睡持锁 300ms），主线程在其持锁窗口内 alloc。 */
static void test_busy_lock() {
    std::printf("[锁] alloc try_lock → RC_ERR_BUSY；free 阻塞等锁\n");
    mock_disk_t d;
    d.base = 0;
    d.blocks = 4;
    d.data.assign((size_t)d.blocks * 4096, 0);          /* 全 0 */

    slice_soul_t soul{};
    soul.read_block = mock_read;
    soul.write_block = mock_write;
    soul.io_ctx = &d;
    soul.chunk_base_blk = 0;
    soul.chunk_blk_length = 4;
    soul.chunk_order = 0;

    uint64_t out = 0, left = 0;
    chunk_slice_mgr_t s;
    CHECK(s.enable(&soul, 0) == RC_OK, "enable busy-test");

    d.slow_us = 300000;                                   /* 下面每次读都慢睡 300ms */
    std::thread bg([&s]() { CHECK(s.chunks_asyn() == RC_OK, "bg asyn"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));   /* 等它拿锁进读 */
    CHECK(s.alloc_hard(0, 1, out) == RC_ERR_BUSY, "alloc_hard BUSY");
    CHECK(s.alloc_soft(0, 1, out, left) == RC_ERR_BUSY, "alloc_soft BUSY");
    bg.join();
    d.slow_us = 0;                                        /* 恢复快路（join 后无并发） */
    CHECK(s.alloc_hard(0, 1, out) == RC_OK, "busy 后恢复");
    CHECK(s.shutdown() == RC_OK, "shutdown busy-test");
}

/* ═══════════════════════════════════════════════════════════════════
 * mgr 级（路由侧 bitmap_slices_mgr_t_v2）：内存模拟盘按 cfg 绝对 FS_block
 * 寻址（两组位图区共一盘）；cpu 身份走 g_fake_cpu 桩。
 * 几何：block 位图区 FS_block 0..7（8 块 = 262144 位）、inode 位图区
 * FS_block 8..11（4 块 = 131072 位）、processor_count=4 ⇒ slice_count_=4：
 * block 片 = 65536 位(2 块)、inode 片 = 32768 位(1 块)、两组 order 均 0。
 * ═══════════════════════════════════════════════════════════════════ */
struct mgr_disk_t {
    std::vector<uint8_t> data;   /* 12 × 4096B，下标 = 绝对 FS_block × 4096 */
    uint64_t rd = 0, wr = 0;
};
static result_code_t mgr_read(void *ctx, uint64_t blk, uint64_t len, void *buf) {
    mgr_disk_t *d = static_cast<mgr_disk_t *>(ctx);
    if (len == 0 || blk + len > (uint64_t)(d->data.size() / 4096)) return RC_ERR_IO;
    std::memcpy(buf, &d->data[(size_t)blk * 4096], (size_t)len * 4096);
    d->rd++;
    return RC_OK;
}
static result_code_t mgr_write(void *ctx, uint64_t blk, uint64_t len, const void *buf) {
    mgr_disk_t *d = static_cast<mgr_disk_t *>(ctx);
    if (len == 0 || blk + len > (uint64_t)(d->data.size() / 4096)) return RC_ERR_IO;
    std::memcpy(&d->data[(size_t)blk * 4096], buf, (size_t)len * 4096);
    d->wr++;
    return RC_OK;
}
/* 盘位（全局位号：block 组 0..262143；inode 组从 262144 起） */
static bool mgbit(const mgr_disk_t &d, uint64_t bit) {
    return ((d.data[bit >> 3] >> (bit & 7)) & 1u) != 0;
}
static void mgset(mgr_disk_t &d, uint64_t bit) {
    d.data[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

/* cpu 身份桩（v1 cpu_id_stub.cpp 的测试强定义；mgr 路由取 preferred 用） */
static uint32_t g_fake_cpu = 0;
extern "C" uint32_t get_processor_id() { return g_fake_cpu; }

static void test_mgr() {
    std::printf("[mgr] 装配 / alloc 命中 / 跨片 free·occupy / 复用 / inode / 审计\n");
    mgr_disk_t disk;
    disk.data.assign((size_t)12 * 4096, 0);
    for (uint64_t i = 0; i < 10; i++) mgset(disk, i);      /* block 位 0..9 预置占用 */

    bitmap_slices_v2cfg_t cfg{};
    cfg.read_block = mgr_read;
    cfg.write_block = mgr_write;
    cfg.io_ctx = &disk;
    cfg.block_bitmap_base_FS_block = 0;
    cfg.block_bitmap_blocks = 8;
    cfg.inode_bitmap_base_FS_block = 8;
    cfg.inode_bitmap_blocks = 4;
    cfg.inobm_chunk_order = 0;
    cfg.blkbm_chunk_order = 0;
    cfg.processor_count = 4;

    uint64_t rc = 0;
    {
        bitmap_slices_mgr_t_v2 mgr(cfg, &rc);

        /* ── ctor 装配成功：slice_count=4、双组 free 实测、preferred 初值 ── */
        CHECK(rc == (uint64_t)RC_OK, "mgr ctor rc=%llu", (unsigned long long)rc);
        CHECK(mgr.slice_count() == 4, "slice_count=%llu (want 4)",
              (unsigned long long)mgr.slice_count());
        CHECK(mgr.block_free_total() == 262144 - 10, "init blk free=%llu (want 262134)",
              (unsigned long long)mgr.block_free_total());
        CHECK(mgr.inode_free_total() == 131072, "init ino free=%llu",
              (unsigned long long)mgr.inode_free_total());
        for (uint32_t i = 0; i < 4; i++) {
            CHECK(mgr.block_preferred_of(i) == i, "blk preferred[%u]=%u", i,
                  mgr.block_preferred_of(i));
            CHECK(mgr.inode_preferred_of(i) == i, "ino preferred[%u]=%u", i,
                  mgr.inode_preferred_of(i));
        }

        uint64_t held = 10;                 /* block 组仍被占的位（含预置 10） */
        auto blk_expect = [&](const char *tag) {
            CHECK(mgr.block_free_total() == 262144 - held,
                  "%s blk free=%llu (want %llu)", tag,
                  (unsigned long long)mgr.block_free_total(),
                  (unsigned long long)(262144 - held));
        };

        /* ── alloc_hard：cpu0 亲和片0 → 命中 10（跳过预置 0..9），全局号合法 ── */
        g_fake_cpu = 0;
        uint64_t b = mgr.block_alloc_hard(10, 0);
        CHECK(b == 10, "alloc_hard=%llu (want 10)", (unsigned long long)b);
        CHECK(mgr.block_test(10) && mgr.block_test(19) && !mgr.block_test(20),
              "block_test 10..19");
        held += 10; blk_expect("P2");
        CHECK(mgr.block_preferred_of(0) == 0, "preferred[0] 写回");

        /* ── alloc_hard：cpu2 亲和片2 → 全局 131072（跨片寻址正确）── */
        g_fake_cpu = 2;
        b = mgr.block_alloc_hard(1, 0);
        CHECK(b == 131072, "alloc_hard cpu2=%llu (want 131072)", (unsigned long long)b);
        CHECK(mgr.block_test(131072), "cpu2 位占用");
        CHECK(mgr.block_preferred_of(2) == 2, "preferred[2] 写回");
        held += 1; blk_expect("P3");

        /* ── block_occupy_soft 跨片续圈：196606（片2 尾 2 位）+ 片3 首 6 位 ── */
        uint64_t rest = 0;
        const uint64_t occ = mgr.block_occupy_soft(196606, 8, &rest);
        CHECK(occ == 196606 && rest == 0, "occupy 跨片 occ=%llu rest=%llu",
              (unsigned long long)occ, (unsigned long long)rest);
        CHECK(mgr.block_test(196606) && mgr.block_test(196613), "occupy 位占用");
        held += 8; blk_expect("P4");

        /* ── block_free 跨片逐段：同区间还回 → 位清 + 记账自洽 ── */
        mgr.block_free(196606, 8);
        CHECK(!mgr.block_test(196606) && !mgr.block_test(196613), "跨片 free 已清");
        held -= 8; blk_expect("P5");

        /* ── occupy 撞占用停：131070 起 10 位 → 片1 尾 2 位 + 片2 首位已占 →
         *    rest=8（中途撞占用即断，不硬续）── */
        rest = 0;
        const uint64_t occ2 = mgr.block_occupy_soft(131070, 10, &rest);
        CHECK(occ2 == 131070 && rest == 8, "occupy 停 occ=%llu rest=%llu (want 8)",
              (unsigned long long)occ2, (unsigned long long)rest);
        CHECK(mgr.block_test(131070) && mgr.block_test(131071), "片1 尾 2 位");
        CHECK(!mgr.block_test(131073) && !mgr.block_test(131079), "占用后未越染");
        held += 2; blk_expect("P6");

        /* ── 复用：cpu0 填满片0（窗1 32768 + 窗0 32748）→ free(10,10) →
         *    再 alloc 绕回命中 10（归还链先于扫描 drain）── */
        g_fake_cpu = 0;
        b = mgr.block_alloc_hard(32768, 0);
        CHECK(b == 32768, "填片0 窗1=%llu (want 32768)", (unsigned long long)b);
        held += 32768;
        b = mgr.block_alloc_hard(32748, 0);
        CHECK(b == 20, "填片0 窗0=%llu (want 20)", (unsigned long long)b);
        held += 32748; blk_expect("P7 填满");
        mgr.block_free(10, 10);
        held -= 10; blk_expect("P7 free");
        b = mgr.block_alloc_hard(10, 0);
        CHECK(b == 10, "复用 alloc=%llu (want 10)", (unsigned long long)b);
        CHECK(mgr.block_test(10) && mgr.block_test(19), "复用位占用");
        held += 10; blk_expect("P7 复用");
        CHECK(mgr.block_free_total() == 196605, "绝对值 blk free=%llu (want 196605)",
              (unsigned long long)mgr.block_free_total());

        /* ── 全败：count=0 / count > 片位数 → PTR_INVALID ── */
        CHECK(mgr.block_alloc_hard(0, 0) == PTR_INVALID, "alloc count=0");
        CHECK(mgr.block_alloc_hard(70000, 0) == PTR_INVALID, "alloc count>片位数");
        blk_expect("P8");

        /* ── 对齐路由：片0 已满 → 换片1，局部 0 对齐 8 → 全局 65536 ── */
        b = mgr.block_alloc_hard(4, 3);
        CHECK(b == 65536 && (b % 8) == 0, "align 路由=%llu (want 65536)",
              (unsigned long long)b);
        held += 4;
        mgr.block_free(65536, 4);
        held -= 4; blk_expect("P9");

        /* ── alloc_soft 缺口回传：片1 窗0 段 32764 位 < 40000 → left=7236 ── */
        uint64_t left = 0;
        const uint64_t s = mgr.block_alloc_soft(40000, &left);
        CHECK(s == 65540 && left == 7236, "soft s=%llu left=%llu (want 65540/7236)",
              (unsigned long long)s, (unsigned long long)left);
        CHECK(mgr.block_test(65540), "soft 位占用");
        held += 32764;
        mgr.block_free(65540, 32764);
        held -= 32764; blk_expect("P10");

        /* ── inode_alloc/free：无「片0 hint=1」根保留 → 首枚即 0；cpu3 → 98304 ── */
        g_fake_cpu = 0;
        uint64_t inum = mgr.inode_alloc();
        CHECK(inum == 0, "inode_alloc=%llu (want 0 —— 无 root 保留)",
              (unsigned long long)inum);
        CHECK(mgr.inode_test(0), "inode 0 占用");
        CHECK(mgr.inode_free_total() == 131071, "ino free=%llu",
              (unsigned long long)mgr.inode_free_total());
        mgr.inode_free(0);
        CHECK(!mgr.inode_test(0), "inode 0 已还");
        CHECK(mgr.inode_free_total() == 131072, "ino free 还原=%llu",
              (unsigned long long)mgr.inode_free_total());
        g_fake_cpu = 3;
        inum = mgr.inode_alloc();
        CHECK(inum == 98304, "inode_alloc cpu3=%llu (want 98304)",
              (unsigned long long)inum);
        CHECK(mgr.inode_test(98304), "inode 98304 占用");
        mgr.inode_free(98304);
        CHECK(!mgr.inode_test(98304), "inode 98304 已还");
        g_fake_cpu = 0;

        /* ── 终局审计自洽（双组 free_total 与持有量对账）── */
        CHECK(mgr.block_free_total() == 262144 - held,
              "audit blk=%llu (want %llu)", (unsigned long long)mgr.block_free_total(),
              (unsigned long long)(262144 - held));
        CHECK(mgr.inode_free_total() == 131072, "audit ino=%llu",
              (unsigned long long)mgr.inode_free_total());
    }   /* dtor：逐片 shutdown 落盘 */

    /* ── dtor 对账：持有位 = 1、已还/未用位 = 0（跨片 free 归还链已合并落盘）── */
    CHECK(mgbit(disk, 0) && mgbit(disk, 9), "盘 0..9 仍占用");
    CHECK(mgbit(disk, 10) && mgbit(disk, 19), "盘 10..19（复用持有）");
    CHECK(mgbit(disk, 20) && mgbit(disk, 65535), "盘 20..65535（片0 填满）");
    CHECK(!mgbit(disk, 65536) && !mgbit(disk, 65539), "盘 65536..65539 已还");
    CHECK(!mgbit(disk, 65540) && !mgbit(disk, 98303), "盘 soft 区已还");
    CHECK(mgbit(disk, 131070) && mgbit(disk, 131072), "盘 131070..131072 占用");
    CHECK(!mgbit(disk, 131073) && !mgbit(disk, 196605), "盘片1/片2 中段空闲");
    CHECK(!mgbit(disk, 196606) && !mgbit(disk, 196613), "盘跨片 free 区已还");
    CHECK(!mgbit(disk, 262143), "盘 block 区尾空闲");
    CHECK(!mgbit(disk, 262144) && !mgbit(disk, 262144 + 98304), "盘 inode 区全空");
    CHECK(disk.rd > 0 && disk.wr > 0, "dtor 落盘 rd=%llu wr=%llu",
          (unsigned long long)disk.rd, (unsigned long long)disk.wr);

    /* ── ctor 失败自清理：回调缺 → 空态 + 服务全拒 ── */
    bitmap_slices_v2cfg_t bad{};
    bad.block_bitmap_blocks = 8;
    bad.inode_bitmap_blocks = 4;
    bad.processor_count = 4;
    uint64_t rc2 = (uint64_t)RC_OK;
    bitmap_slices_mgr_t_v2 m2(bad, &rc2);
    CHECK(rc2 == (uint64_t)RC_ERR_STATE, "坏 cfg rc=%llu", (unsigned long long)rc2);
    CHECK(m2.slice_count() == 0 && m2.block_free_total() == 0 && m2.inode_free_total() == 0,
          "坏 cfg 空态");
    CHECK(m2.block_alloc_hard(1, 0) == PTR_INVALID, "坏 cfg alloc");
    CHECK(m2.block_preferred_of(0) == 0, "坏 cfg preferred");

    /* ── processor_count=0 拒（头无 cpu_slots_，内部 % 会除零）── */
    bitmap_slices_v2cfg_t bad2 = cfg;
    bad2.processor_count = 0;
    uint64_t rc3 = (uint64_t)RC_OK;
    bitmap_slices_mgr_t_v2 m3(bad2, &rc3);
    CHECK(rc3 == (uint64_t)RC_ERR_STATE, "pc=0 rc=%llu", (unsigned long long)rc3);
}

int main() {
    test_mgr();
    test_geometry_a();
    test_geometry_b();
    test_calib_out();
    test_busy_lock();
    if (g_fail == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILED: %d\n", g_fail);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * stress_v2_adversarial.cpp — bitmap_slices_v2 边界/对抗 + 错误注入专项
 *
 * 2026-09-30。独立 main（**未接 CMake**，手工编译）：
 *   g++ -std=c++17 -Wall -Wextra -Iinclude/ravenfs \
 *       tests/stress_v2_adversarial.cpp juvenile_god/bitmap_slices_v2.cpp \
 *       -o /tmp/v2adv && /tmp/v2adv
 *
 * 被测：include/ravenfs/bitmap_slices_v2.hpp + juvenile_god/bitmap_slices_v2.cpp
 *   · chunk_slice_mgr_t（片层：alloc/free/occupy/test/asyn/enable/shutdown）
 *   · bitmap_slices_mgr_t_v2（路由层：全局↔局部换算、跨片 free/occupy）
 *
 * 用例（全部有界；运行限时 ≤60s，超时 alarm 打 TIMEOUT 并以非 0 退出）：
 *   ① 门闸        enable 前 / 双 enable / shutdown 后 全服务 RC_ERR_STATE；mgr 坏 cfg
 *   ② 几何 order0  恰占满整片、align_log2 0..15 全接受 + >15 拒、对齐取整
 *   ③ 几何尾窗     order=1 nblocks=3（slice 98304，尾窗 32768）
 *   ④ 几何 order2  nblocks=6（尾窗 65536）单窗上限 / 尾窗夹断；nblocks=2（slice<chunk）
 *   ⑤ 跨窗 occupy  顶窗尾续下一窗再撞占用而停；跨窗 free
 *   ⑥ 重复 free    free_count 虚高 → chunks_asyn(&calib) old!=new 并校准为盘实测
 *   ⑦ 错误注入     读失败(load) / 写失败(evict) / 写失败(asyn) → 不双分配、记录
 *                  可重试、之后仍可正常工作
 *   ⑧ 影子对拍     片层 3000 随机操作无重复分配 + free_count 记账不变量 + asyn 校准
 *   ⑨ mgr 对抗     跨片 occupy 中途撞占用停、跨 3 片 free、inode、全盘影子对拍
 *   ⑩ 最小复现     在线窗 drain 未置脏 → evict 不回写 → 已释放位丢失（真实 bug）
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/bitmap_slices_v2.hpp"

#include <csignal>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

/* ── 用例级 PASS/FAIL 记账 ── */
static int g_fail = 0;
static int g_case_fail = 0;
static const char *g_case = "";

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++g_fail;                                                       \
            ++g_case_fail;                                                  \
            std::printf("  [FAIL] %s:%d: ", g_case, __LINE__);              \
            std::printf(__VA_ARGS__);                                       \
            std::printf("\n");                                              \
        }                                                                   \
    } while (0)

template <typename F>
static void run_case(const char *name, F f) {
    g_case = name;
    g_case_fail = 0;
    f();
    std::printf("  --> %s : %s\n", name, g_case_fail ? "FAIL" : "PASS");
}

/* ── 运行限时：SIGALRM 到点打 TIMEOUT 并非 0 退出 ── */
static void on_alarm(int) {
    const char *m = "\n[TIMEOUT] exceeded time limit\n";
    ssize_t r = ::write(2, m, ::strlen(m));
    (void)r;
    ::_exit(2);
}

/* ═══════════════════════════════════════════════════════════════════
 * 内存模拟盘（4K FS_block；data 下标 = (blk-base)*4096），带错误注入：
 *   fail_read / fail_write > 0 时，下次对应 I/O 直接返 RC_ERR_IO（不落盘）。
 *   rd / wr 只计成功次数。
 * ═══════════════════════════════════════════════════════════════════ */
struct FakeDisk {
    std::vector<uint8_t> data;
    uint64_t base = 0;
    uint64_t blocks = 0;
    uint64_t rd = 0, wr = 0;
    int fail_read = 0, fail_write = 0;
};

static result_code_t fdisk_read(void *ctx, uint64_t blk, uint64_t len, void *buf) {
    FakeDisk *d = static_cast<FakeDisk *>(ctx);
    if (len == 0 || blk < d->base || blk - d->base + len > d->blocks) return RC_ERR_IO;
    if (d->fail_read > 0) { d->fail_read--; return RC_ERR_IO; }
    std::memcpy(buf, &d->data[(size_t)(blk - d->base) * 4096], (size_t)len * 4096);
    d->rd++;
    return RC_OK;
}
static result_code_t fdisk_write(void *ctx, uint64_t blk, uint64_t len, const void *buf) {
    FakeDisk *d = static_cast<FakeDisk *>(ctx);
    if (len == 0 || blk < d->base || blk - d->base + len > d->blocks) return RC_ERR_IO;
    if (d->fail_write > 0) { d->fail_write--; return RC_ERR_IO; }
    std::memcpy(&d->data[(size_t)(blk - d->base) * 4096], buf, (size_t)len * 4096);
    d->wr++;
    return RC_OK;
}

/* 盘上片内位 idx（相对本片位图区） */
static void disk_set(FakeDisk &d, uint64_t idx) { d.data[idx >> 3] |= (uint8_t)(1u << (idx & 7)); }
static bool disk_get(const FakeDisk &d, uint64_t idx) {
    return ((d.data[idx >> 3] >> (idx & 7)) & 1u) != 0;
}
static uint64_t disk_zeros(const FakeDisk &d) {
    uint64_t z = 0;
    for (size_t i = 0; i < d.data.size(); i++)
        z += 8u - (uint64_t)__builtin_popcount((unsigned)d.data[i]);
    return z;
}
static bool disk_all(const FakeDisk &d, uint64_t lo, uint64_t n, bool v) {
    for (uint64_t i = 0; i < n; i++)
        if (disk_get(d, lo + i) != v) return false;
    return true;
}

/* ── 逻辑影子位图（1 = 已用）── */
struct Shadow {
    std::vector<uint8_t> b;
    void init(uint64_t n) { b.assign((size_t)n, 0); }
    bool free_all(uint64_t lo, uint64_t n) const {
        for (uint64_t i = 0; i < n; i++)
            if (b[(size_t)(lo + i)]) return false;
        return true;
    }
    void set(uint64_t lo, uint64_t n) { for (uint64_t i = 0; i < n; i++) b[(size_t)(lo + i)] = 1; }
    void clr(uint64_t lo, uint64_t n) { for (uint64_t i = 0; i < n; i++) b[(size_t)(lo + i)] = 0; }
    uint64_t used() const {
        uint64_t c = 0;
        for (uint8_t v : b) c += v;
        return c;
    }
};

static slice_soul_t soul_of(FakeDisk *d, uint64_t base, uint64_t blocks, uint8_t order) {
    slice_soul_t s{};
    s.read_block = fdisk_read;
    s.write_block = fdisk_write;
    s.io_ctx = d;
    s.chunk_base_blk = base;
    s.chunk_blk_length = blocks;
    s.chunk_order = order;
    return s;
}

/* cpu 身份桩（路由取 preferred 用；强定义覆盖 cpu_id_stub 的 weak 桩） */
static uint32_t g_cpu = 0;
extern "C" uint32_t get_processor_id() { return g_cpu; }

/* ═══════════════════════════════════════════════════════════════════
 * ① 门闸：enable 前 / 双 enable / shutdown 后 全服务 RC_ERR_STATE
 * ═══════════════════════════════════════════════════════════════════ */
static void case_gate() {
    FakeDisk d;
    d.base = 0;
    d.blocks = 1;
    d.data.assign(4096, 0);
    slice_soul_t soul = soul_of(&d, 0, 1, 0);

    uint64_t out = 0, left = 0, rest = 0;
    chunk_slice_mgr_t s;
    CHECK(s.alloc_hard(0, 1, out) == RC_ERR_STATE, "hard pre-enable");
    CHECK(s.alloc_soft(0, 1, out, left) == RC_ERR_STATE, "soft pre-enable");
    CHECK(s.free_interval(0, 1) == RC_ERR_STATE, "free pre-enable");
    CHECK(s.test_interval(true, 0, 1) == RC_ERR_STATE, "test pre-enable");
    CHECK(s.soft_occupy(0, 1, &rest) == RC_ERR_STATE, "occupy pre-enable");
    CHECK(s.chunks_asyn() == RC_ERR_STATE, "asyn pre-enable");
    CHECK(s.shutdown() == RC_ERR_STATE, "shutdown pre-enable");
    CHECK(s.get_free_count() == 0, "free_count pre-enable");

    CHECK(s.enable(&soul, 9) == RC_OK, "enable");
    CHECK(s.get_free_count() == 32768, "free=%llu", (unsigned long long)s.get_free_count());
    CHECK(s.enable(&soul, 9) == RC_ERR_STATE, "double enable");
    /* 参数边界：count=0 / count>slice / align>15 / 越界区间 */
    CHECK(s.alloc_hard(0, 0, out) == RC_ERR_STATE, "count=0");
    CHECK(s.alloc_hard(0, 32769, out) == RC_ERR_STATE, "count>slice");
    CHECK(s.alloc_hard(16, 1, out) == RC_ERR_STATE, "align16");
    CHECK(s.test_interval(true, 32767, 2) == RC_ERR_STATE, "test 越界");
    CHECK(s.free_interval(32768, 1) == RC_ERR_STATE, "free 越界");

    CHECK(s.shutdown() == RC_OK, "shutdown");
    CHECK(s.alloc_hard(0, 1, out) == RC_ERR_STATE, "hard post-shutdown");
    CHECK(s.alloc_soft(0, 1, out, left) == RC_ERR_STATE, "soft post-shutdown");
    CHECK(s.free_interval(0, 1) == RC_ERR_STATE, "free post-shutdown");
    CHECK(s.test_interval(true, 0, 1) == RC_ERR_STATE, "test post-shutdown");
    CHECK(s.soft_occupy(0, 1, &rest) == RC_ERR_STATE, "occupy post-shutdown");
    CHECK(s.chunks_asyn() == RC_ERR_STATE, "asyn post-shutdown");
    CHECK(s.shutdown() == RC_ERR_STATE, "double shutdown");

    /* 重新 enable 自洽（门闸可往返） */
    CHECK(s.enable(&soul, 0) == RC_OK, "re-enable");
    CHECK(s.alloc_hard(0, 1, out) == RC_OK && out == 0, "re-enable alloc");
    CHECK(s.get_free_count() == 32767, "re-enable free=%llu",
          (unsigned long long)s.get_free_count());
    CHECK(s.shutdown() == RC_OK, "final shutdown");
}

/* ═══════════════════════════════════════════════════════════════════
 * ② 几何 order=0：恰占满整片 + align_log2 0..15 全接受 + >15 拒 + 对齐取整
 * ═══════════════════════════════════════════════════════════════════ */
static void case_geom_order0() {
    FakeDisk d;
    d.base = 0;
    d.blocks = 4;                       /* slice = 4 窗 × 32768 = 131072 位 */
    d.data.assign((size_t)d.blocks * 4096, 0);
    slice_soul_t soul = soul_of(&d, 0, 4, 0);

    uint64_t out = 0;
    chunk_slice_mgr_t s;
    CHECK(s.enable(&soul, 0) == RC_OK, "enable");
    CHECK(s.get_free_count() == 131072, "free=%llu", (unsigned long long)s.get_free_count());

    /* 恰好占满整片：4 次整窗硬分配 */
    const uint64_t want[4] = {0, 32768, 65536, 98304};
    for (int i = 0; i < 4; i++) {
        CHECK(s.alloc_hard(0, 32768, out) == RC_OK, "fill #%d", i);
        CHECK(out == want[i], "fill out=%llu (want %llu)", (unsigned long long)out,
              (unsigned long long)want[i]);
    }
    CHECK(s.get_free_count() == 0, "full free=%llu", (unsigned long long)s.get_free_count());
    CHECK(s.alloc_hard(0, 1, out) == RC_ERR_NO_BLOCK, "full alloc gate");
    CHECK(s.test_interval(true, 0, 131072) == RC_OK, "full test all-used");
    CHECK(s.test_interval(false, 0, 1) == RC_ERR_NOT_FOUND, "full no free bit");
    /* 单窗上限：count == 整片但 > 单窗 → 不跨窗，NO_BLOCK */
    CHECK(s.free_interval(0, 131072) == RC_OK, "free whole slice");
    CHECK(s.get_free_count() == 131072, "after free=%llu",
          (unsigned long long)s.get_free_count());
    CHECK(s.alloc_hard(0, 131072, out) == RC_ERR_NO_BLOCK, "single-window cap");

    /* align_log2 取整：前缀 0..9 置占用（模拟盘），从 hint=10 起 */
    for (uint64_t i = 0; i < 10; i++) disk_set(d, i);
    chunk_slice_mgr_t s2;
    CHECK(s2.enable(&soul, 0) == RC_OK, "enable align");
    for (uint64_t a = 0; a <= 15; a++) {
        s2.set_hint(10);
        CHECK(s2.alloc_hard((uint8_t)a, 1, out) == RC_OK, "align%llu accept",
              (unsigned long long)a);
        CHECK(out >= 10 && (out & ((1ULL << a) - 1)) == 0,
              "align%llu out=%llu 未对齐", (unsigned long long)a, (unsigned long long)out);
        CHECK(s2.free_interval(out, 1) == RC_OK, "align%llu free", (unsigned long long)a);
    }
    CHECK(s2.alloc_hard(16, 1, out) == RC_ERR_STATE, "align16 hard STATE");
    uint64_t left = 0;
    CHECK(s2.alloc_soft(16, 1, out, left) == RC_ERR_STATE, "align16 soft STATE");
    CHECK(s2.alloc_hard(255, 1, out) == RC_ERR_STATE, "align255 hard STATE");
    CHECK(s2.shutdown() == RC_OK, "shutdown");
}

/* ═══════════════════════════════════════════════════════════════════
 * ③ 几何尾窗：order=1 nblocks=3 → slice 98304，chunk 65536，尾窗 32768（非整倍）
 * ═══════════════════════════════════════════════════════════════════ */
static void case_geom_tail_order1() {
    FakeDisk d;
    d.base = 0;
    d.blocks = 3;
    d.data.assign((size_t)d.blocks * 4096, 0);
    slice_soul_t soul = soul_of(&d, 0, 3, 1);

    uint64_t out = 0;
    chunk_slice_mgr_t s;
    CHECK(s.enable(&soul, 0) == RC_OK, "enable");
    CHECK(s.get_free_count() == 98304, "free=%llu (want 98304)",
          (unsigned long long)s.get_free_count());

    CHECK(s.alloc_hard(0, 65536, out) == RC_OK && out == 0, "win0 out=%llu",
          (unsigned long long)out);
    CHECK(s.alloc_hard(0, 32768, out) == RC_OK && out == 65536, "tail out=%llu (want 65536)",
          (unsigned long long)out);
    CHECK(s.get_free_count() == 0, "tail-full free=%llu",
          (unsigned long long)s.get_free_count());
    CHECK(s.test_interval(true, 65536, 32768) == RC_OK, "tail test");
    /* 尾窗容量：count=32769 时 hint 顶到片尾 → 绕回窗0 取（可分配），验证不越片尾 */
    CHECK(s.free_interval(0, 98304) == RC_OK, "free all");
    s.set_hint(65536);
    /* count=60000 放不进尾窗(32768)，绕回窗0(65536)命中 → 不越片尾 */
    CHECK(s.alloc_hard(0, 60000, out) == RC_OK, "绕回分配 out=%llu",
          (unsigned long long)out);
    CHECK(out == 0 && out + 60000 <= 98304, "绕回不越片尾 out=%llu",
          (unsigned long long)out);
    CHECK(s.chunks_asyn() == RC_OK, "asyn");
    CHECK(s.get_free_count() == disk_zeros(d), "asyn free=%llu disk=%llu",
          (unsigned long long)s.get_free_count(), (unsigned long long)disk_zeros(d));
    CHECK(s.shutdown() == RC_OK, "shutdown");
}

/* ═══════════════════════════════════════════════════════════════════
 * ④ 几何 order=2：nblocks=6（slice 196608，chunk 131072，尾窗 65536）
 *    单窗上限 / 尾窗夹断；nblocks=2（slice 65536 < chunk 131072，单窗被 tail 夹断）
 * ═══════════════════════════════════════════════════════════════════ */
static void case_geom_order2() {
    /* --- nblocks=6：2 窗，尾窗 65536 --- */
    {
        FakeDisk d;
        d.base = 0;
        d.blocks = 6;
        d.data.assign((size_t)d.blocks * 4096, 0);
        slice_soul_t soul = soul_of(&d, 0, 6, 2);
        uint64_t out = 0, rest = 0;
        chunk_slice_mgr_t s;
        CHECK(s.enable(&soul, 0) == RC_OK, "enable 6blk");
        CHECK(s.get_free_count() == 196608, "free=%llu (want 196608)",
              (unsigned long long)s.get_free_count());
        /* 整片大于单窗 → 不跨窗，NO_BLOCK；> 整片 → STATE */
        CHECK(s.alloc_hard(0, 196608, out) == RC_ERR_NO_BLOCK, "6blk single-window");
        CHECK(s.alloc_hard(0, 196609, out) == RC_ERR_STATE, "6blk count>slice");
        /* 尾窗夹断：occupy len=65536 恰好=尾窗；len=65537 → claimed 65536, rest 1 */
        rest = 0;
        CHECK(s.soft_occupy(131072, 65536, &rest) == RC_OK && rest == 0,
              "tail exact rest=%llu", (unsigned long long)rest);
        CHECK(s.test_interval(true, 131072, 65536) == RC_OK, "tail full");
        CHECK(s.free_interval(131072, 65536) == RC_OK, "free tail");
        rest = 0;
        const uint64_t got = s.soft_occupy(131072, 65537, &rest);
        CHECK(got == RC_OK && rest == 1, "tail clamp rc=%llu rest=%llu (want rest 1)",
              (unsigned long long)got, (unsigned long long)rest);
        CHECK(s.chunks_asyn() == RC_OK, "asyn 6blk");
        CHECK(s.get_free_count() == disk_zeros(d), "asyn free=%llu disk=%llu",
              (unsigned long long)s.get_free_count(), (unsigned long long)disk_zeros(d));
        CHECK(s.shutdown() == RC_OK, "shutdown 6blk");
    }
    /* --- nblocks=2：slice 65536 < chunk 131072 ⇒ limit_of(0)=tail=65536 --- */
    {
        FakeDisk d;
        d.base = 0;
        d.blocks = 2;
        d.data.assign((size_t)d.blocks * 4096, 0);
        slice_soul_t soul = soul_of(&d, 0, 2, 2);
        uint64_t out = 0, rest = 0;
        chunk_slice_mgr_t s;
        CHECK(s.enable(&soul, 0) == RC_OK, "enable 2blk");
        CHECK(s.get_free_count() == 65536, "free=%llu (want 65536)",
              (unsigned long long)s.get_free_count());
        CHECK(s.alloc_hard(0, 65536, out) == RC_OK && out == 0, "2blk full alloc");
        CHECK(s.free_interval(0, 65536) == RC_OK, "free");
        rest = 0;
        CHECK(s.soft_occupy(0, 65537, &rest) == RC_OK && rest == 1,
              "2blk tail clamp rest=%llu", (unsigned long long)rest);
        CHECK(s.test_interval(true, 0, 65536) == RC_OK, "2blk all used");
        CHECK(s.shutdown() == RC_OK, "shutdown 2blk");
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * ⑤ 跨窗 occupy / free（片层）：顶窗尾续下一窗 → 中途撞占用停；跨窗 free
 * ═══════════════════════════════════════════════════════════════════ */
static void case_cross_window() {
    FakeDisk d;
    d.base = 0;
    d.blocks = 3;                       /* order=1：窗0 [0,65536)，窗1 [65536,98304) */
    d.data.assign((size_t)d.blocks * 4096, 0);
    disk_set(d, 65540);                 /* 窗1 局部 4 占用（制造中途撞占用） */
    slice_soul_t soul = soul_of(&d, 0, 3, 1);

    uint64_t rest = 0;
    chunk_slice_mgr_t s;
    CHECK(s.enable(&soul, 0) == RC_OK, "enable");
    CHECK(s.get_free_count() == 98303, "free=%llu (want 98303)",
          (unsigned long long)s.get_free_count());

    /* 65530 起 20 位：窗0 认领 6（65530..65535），续窗1 认领 4（65536..65539），
     * 撞 65540 占用而停 → claimed 10，rest 10 */
    const uint64_t g = s.soft_occupy(65530, 20, &rest);
    CHECK(g == RC_OK, "occupy rc=%llu", (unsigned long long)g);
    CHECK(rest == 10, "occupy rest=%llu (want 10)", (unsigned long long)rest);
    CHECK(s.test_interval(true, 65530, 10) == RC_OK, "claimed 段占用");
    CHECK(s.test_interval(true, 65540, 1) == RC_OK, "65540 预置占用仍在");
    CHECK(s.test_interval(false, 65540, 1) == RC_ERR_NOT_FOUND, "65540 未越染");

    /* 跨窗 free：65530 起 10 位跨越窗界（窗0 6 + 窗1 4） */
    CHECK(s.free_interval(65530, 10) == RC_OK, "cross-window free");
    CHECK(s.get_free_count() == 98303, "free after=%llu (want 98303)",
          (unsigned long long)s.get_free_count());
    CHECK(s.test_interval(false, 65530, 10) == RC_OK, "freed 段空闲");
    CHECK(s.chunks_asyn() == RC_OK, "asyn");
    CHECK(s.get_free_count() == disk_zeros(d), "asyn free=%llu disk=%llu",
          (unsigned long long)s.get_free_count(), (unsigned long long)disk_zeros(d));
    CHECK(disk_all(d, 65530, 10, false), "盘上已释放段 = 0");
    CHECK(disk_get(d, 65540), "盘上 65540 仍 1");
    CHECK(s.shutdown() == RC_OK, "shutdown");
}

/* ═══════════════════════════════════════════════════════════════════
 * ⑥ 重复 free 漂移：free_count 虚高 → chunks_asyn(&calib) old!=new，校准为盘实测
 * ═══════════════════════════════════════════════════════════════════ */
static void case_drift_calib() {
    FakeDisk d;
    d.base = 0;
    d.blocks = 4;
    d.data.assign((size_t)d.blocks * 4096, 0);
    slice_soul_t soul = soul_of(&d, 0, 4, 0);

    chunk_slice_mgr_t s;
    CHECK(s.enable(&soul, 0) == RC_OK, "enable");
    CHECK(s.get_free_count() == 131072, "init free");

    /* 同一区间重复 free：slice 层允许 → 挂链即记账，free_count 虚高 +len */
    CHECK(s.free_interval(100, 40) == RC_OK, "free #1");
    CHECK(s.free_interval(100, 40) == RC_OK, "free #2 重复");
    CHECK(s.free_interval(100, 40) == RC_OK, "free #3 重复");
    CHECK(s.get_free_count() == 131192, "漂移 free=%llu (want 131192)",
          (unsigned long long)s.get_free_count());

    free_count_calib_t cal{};
    CHECK(s.chunks_asyn(&cal) == RC_OK, "asyn");
    CHECK(cal.old_free_count == 131192, "calib.old=%llu (want 131192)",
          (unsigned long long)cal.old_free_count);
    CHECK(cal.new_free_count == 131072, "calib.new=%llu (want 盘实测 131072)",
          (unsigned long long)cal.new_free_count);
    CHECK(cal.old_free_count != cal.new_free_count, "漂移被检出 old!=new");
    CHECK(cal.new_free_count == disk_zeros(d), "calib.new == 盘 0 位数");
    CHECK(s.get_free_count() == cal.new_free_count, "校准后 free_count = new");
    /* 二次 asyn：已校准 → old == new（幂等） */
    free_count_calib_t cal2{};
    CHECK(s.chunks_asyn(&cal2) == RC_OK, "asyn #2");
    CHECK(cal2.old_free_count == cal2.new_free_count, "校准后 old==new (%llu/%llu)",
          (unsigned long long)cal2.old_free_count, (unsigned long long)cal2.new_free_count);
    CHECK(s.shutdown() == RC_OK, "shutdown");
}

/* ═══════════════════════════════════════════════════════════════════
 * ⑦ 错误注入（片层）：load 读失败 / evict 写失败 / asyn 写失败
 *    → 不双分配、记录保留可重试、之后仍能正常工作
 * ═══════════════════════════════════════════════════════════════════ */
static void case_err_inject() {
    FakeDisk d;
    d.base = 0;
    d.blocks = 4;
    d.data.assign((size_t)d.blocks * 4096, 0);
    for (uint64_t i = 0; i < 10; i++) disk_set(d, i);   /* 窗0 = MIXED（强制 load） */
    slice_soul_t soul = soul_of(&d, 0, 4, 0);

    uint64_t out = 0;
    chunk_slice_mgr_t s;
    CHECK(s.enable(&soul, 0) == RC_OK, "enable");
    const uint64_t f0 = s.get_free_count();
    CHECK(f0 == 131062, "init free=%llu (want 131062)", (unsigned long long)f0);

    /* (a) load 读失败：hint=0 → 窗0 离线 MIXED → ensure_online 读失败 → RC_ERR_IO */
    d.fail_read = 1;
    const uint64_t rd0 = d.rd;
    CHECK(s.alloc_hard(0, 5, out) == RC_ERR_IO, "load-fail alloc RC_ERR_IO");
    CHECK(s.get_free_count() == f0, "load-fail free_count 不变");
    CHECK(d.rd == rd0, "失败读不计成功 rd");
    /* 清除故障后重试同一分配应成功 → 不双分配（out==10，唯一） */
    CHECK(s.alloc_hard(0, 5, out) == RC_OK, "retry alloc");
    CHECK(out == 10, "retry out=%llu (want 10)", (unsigned long long)out);
    CHECK(s.get_free_count() == f0 - 5, "retry free=%llu",
          (unsigned long long)s.get_free_count());
    CHECK(s.test_interval(true, 10, 5) == RC_OK, "retry 位占用");

    /* (b) evict 写失败：hint 顶到窗1 → 换片需先 evict 脏窗0 → 写失败 → RC_ERR_IO
     *     且窗0 保持在线（状态零损坏），bit10..14 仍占用，无新分配 */
    const uint64_t free_b = s.get_free_count();
    s.set_hint(32768);
    d.fail_write = 1;
    const uint64_t wr0 = d.wr;
    CHECK(s.alloc_hard(0, 5, out) == RC_ERR_IO, "evict-fail alloc RC_ERR_IO");
    CHECK(s.get_free_count() == free_b, "evict-fail free_count 不变");
    CHECK(d.wr == wr0, "失败写不计成功 wr");
    CHECK(s.test_interval(true, 10, 5) == RC_OK, "evict-fail 旧分配仍在");
    /* 清除故障后重试：evict 成功 → 新命中窗1（32768），与 10..19 不重叠 */
    CHECK(s.alloc_hard(0, 5, out) == RC_OK, "evict retry alloc");
    CHECK(out == 32768, "evict retry out=%llu (want 32768)", (unsigned long long)out);
    CHECK(s.get_free_count() == free_b - 5, "evict retry free=%llu",
          (unsigned long long)s.get_free_count());
    CHECK(s.test_interval(true, 10, 5) == RC_OK && s.test_interval(true, 32768, 5) == RC_OK,
          "两段均占用（无互相覆盖/双分配）");

    /* (c) asyn 写失败：离线窗0 挂归还记录 → 写失败 → 记录保留、free_count 不校准 */
    CHECK(s.free_interval(10, 5) == RC_OK, "free 挂账");
    const uint64_t inflated = s.get_free_count();
    CHECK(inflated == free_b - 5 + 5, "inflated free=%llu", (unsigned long long)inflated);
    d.fail_write = 1;
    const uint64_t wr1 = d.wr;
    CHECK(s.chunks_asyn() == RC_ERR_IO, "asyn 写失败 RC_ERR_IO");
    CHECK(d.wr == wr1, "asyn 失败写不计成功");
    CHECK(s.get_free_count() == inflated, "asyn 失败 free_count 未校准（保留漂移）");
    CHECK(disk_all(d, 10, 5, true), "asyn 失败盘上 10..14 仍 1（记录未落盘）");
    /* 清故障重试：成功 → 盘上 10..14 清 0、free_count 校准为盘实测 */
    CHECK(s.chunks_asyn() == RC_OK, "asyn retry");
    CHECK(disk_all(d, 10, 5, false), "asyn retry 盘上已清");
    CHECK(s.get_free_count() == disk_zeros(d), "asyn retry 校准 free=%llu disk=%llu",
          (unsigned long long)s.get_free_count(), (unsigned long long)disk_zeros(d));

    /* 之后仍能正常工作：正常 alloc / free / test */
    uint64_t left = 0;
    CHECK(s.alloc_soft(0, 3, out, left) == RC_OK, "post-err soft");
    CHECK(s.test_interval(true, out, 3) == RC_OK, "post-err test");
    CHECK(s.free_interval(out, 3) == RC_OK, "post-err free");
    CHECK(s.chunks_asyn() == RC_OK, "post-err asyn");
    CHECK(s.get_free_count() == disk_zeros(d), "post-err 校准");
    CHECK(s.shutdown() == RC_OK, "shutdown");
}

/* ═══════════════════════════════════════════════════════════════════
 * ⑧ 影子对拍（片层，有界随机 3000 操作）：无重复分配 + 记账不变量 + asyn 校准
 * ═══════════════════════════════════════════════════════════════════ */
static void case_shadow_slice() {
    const uint64_t N = 131072;                  /* order=0，4 窗 */
    FakeDisk d;
    d.base = 0;
    d.blocks = 4;
    d.data.assign((size_t)d.blocks * 4096, 0);
    slice_soul_t soul = soul_of(&d, 0, 4, 0);

    chunk_slice_mgr_t s;
    CHECK(s.enable(&soul, 0) == RC_OK, "enable");
    Shadow sh;
    sh.init(N);
    uint64_t logfree = N;                        /* 记账：含重复 free 的 len */
    std::mt19937_64 rng(20260930ULL);

    for (int op = 0; op < 3000; op++) {
        const int before = g_case_fail;          /* 首违即停：输出有界 */
        const uint32_t k = (uint32_t)(rng() % 100);
        if (k < 35) {                            /* alloc_hard */
            const uint8_t a = (uint8_t)(rng() % 16);
            const uint64_t c = 1 + rng() % 3000;
            uint64_t out = 0;
            const result_code_t rc = s.alloc_hard(a, c, out);
            if (rc == RC_OK) {
                CHECK(out + c <= N, "hard 越界 out=%llu c=%llu",
                      (unsigned long long)out, (unsigned long long)c);
                CHECK((out & ((1ULL << a) - 1)) == 0, "hard 未对齐 a=%u out=%llu", a,
                      (unsigned long long)out);
                CHECK(sh.free_all(out, c), "hard 重复分配 out=%llu c=%llu",
                      (unsigned long long)out, (unsigned long long)c);
                sh.set(out, c);
                logfree -= c;
            } else {
                CHECK(rc == RC_ERR_NO_BLOCK, "hard rc=%llu", (unsigned long long)rc);
            }
        } else if (k < 55) {                     /* alloc_soft */
            const uint8_t a = (uint8_t)(rng() % 16);
            const uint64_t c = 1 + rng() % 3000;
            uint64_t out = 0, left = 0;
            const result_code_t rc = s.alloc_soft(a, c, out, left);
            if (rc == RC_OK) {
                const uint64_t got = c - left;
                CHECK(got >= 1 && got <= c, "soft got=%llu", (unsigned long long)got);
                CHECK(out + got <= N, "soft 越界");
                CHECK(sh.free_all(out, got), "soft 重复分配 out=%llu got=%llu",
                      (unsigned long long)out, (unsigned long long)got);
                sh.set(out, got);
                logfree -= got;
            } else {
                CHECK(rc == RC_ERR_NO_BLOCK, "soft rc=%llu", (unsigned long long)rc);
            }
        } else if (k < 80) {                     /* free 任取区间（可含未分配） */
            uint64_t base = rng() % N;
            uint64_t len = 1 + rng() % 2000;
            if (base + len > N) len = N - base;
            CHECK(s.free_interval(base, len) == RC_OK, "free rc");
            sh.clr(base, len);
            logfree += len;
        } else if (k < 90) {                     /* soft_occupy 锚定 */
            uint64_t base = rng() % N;
            uint64_t len = 1 + rng() % 1500;
            if (base + len > N) len = N - base;
            uint64_t rest = 0;
            const result_code_t rc = s.soft_occupy(base, len, &rest);
            if (rc == RC_OK) {
                const uint64_t claimed = len - rest;
                CHECK(claimed >= 1 && claimed <= len, "occupy claimed=%llu",
                      (unsigned long long)claimed);
                CHECK(sh.free_all(base, claimed), "occupy 重复分配 base=%llu claimed=%llu",
                      (unsigned long long)base, (unsigned long long)claimed);
                sh.set(base, claimed);
                logfree -= claimed;
            } else {
                CHECK(rc == RC_ERR_NO_BLOCK, "occupy rc=%llu", (unsigned long long)rc);
            }
        } else if (k < 95) {                     /* asyn + 校准对拍 */
            free_count_calib_t cal{};
            CHECK(s.chunks_asyn(&cal) == RC_OK, "asyn rc");
            CHECK(cal.old_free_count == logfree,
                  "asyn old=%llu 记账=%llu", (unsigned long long)cal.old_free_count,
                  (unsigned long long)logfree);
            const uint64_t phys = N - sh.used();
            CHECK(cal.new_free_count == phys, "asyn new=%llu 物理=%llu",
                  (unsigned long long)cal.new_free_count, (unsigned long long)phys);
            CHECK(s.get_free_count() == phys, "asyn free=%llu 物理=%llu",
                  (unsigned long long)s.get_free_count(), (unsigned long long)phys);
            logfree = phys;
        } else {                                 /* test_interval 抽样对拍 */
            for (int t = 0; t < 20; t++) {
                const uint64_t p = rng() % N;
                const bool used = sh.b[(size_t)p] != 0;
                const result_code_t rc = s.test_interval(true, p, 1);
                if (used) {
                    CHECK(rc == RC_OK, "test used p=%llu rc=%llu",
                          (unsigned long long)p, (unsigned long long)rc);
                } else {
                    CHECK(rc == RC_ERR_NOT_FOUND, "test free p=%llu rc=%llu",
                          (unsigned long long)p, (unsigned long long)rc);
                }
            }
        }
        if ((op % 50) == 0) {
            CHECK(s.get_free_count() == logfree, "op%d agg free=%llu 记账=%llu", op,
                  (unsigned long long)s.get_free_count(), (unsigned long long)logfree);
        }
        if (g_case_fail != before) break;        /* 首违即停 */
    }
    if (g_case_fail == 0) {                     /* 未出违 → 终局对账 */
        CHECK(s.chunks_asyn() == RC_OK, "final asyn");
        CHECK(s.get_free_count() == N - sh.used(), "final free=%llu 影子空闲=%llu",
              (unsigned long long)s.get_free_count(),
              (unsigned long long)(N - sh.used()));
        CHECK(s.get_free_count() == disk_zeros(d), "final free == 盘实测");
        CHECK(s.free_interval(0, N) == RC_OK, "clear all");
        CHECK(s.chunks_asyn() == RC_OK, "clear asyn");
        CHECK(s.get_free_count() == N, "clear free=%llu",
              (unsigned long long)s.get_free_count());
        CHECK(disk_zeros(d) == N, "clear 盘全 0");
    }
    CHECK(s.shutdown() == RC_OK, "shutdown");
}

/* ═══════════════════════════════════════════════════════════════════
 * ⑨ mgr 对抗：跨片 occupy 中途撞占用停、跨 3 片 free、inode、全盘影子对拍
 *    几何：block bm FS_block[0,8)=262144 位、inode bm FS_block[8,12)=131072 位、
 *          processor_count=4、order=0 ⇒ slice_count=4；block 片 65536、inode 片 32768。
 * ═══════════════════════════════════════════════════════════════════ */
static void case_mgr_adversarial() {
    const uint64_t BLK_TOTAL = 262144, INO_TOTAL = 131072;
    FakeDisk disk;
    disk.base = 0;
    disk.blocks = 12;
    disk.data.assign((size_t)disk.blocks * 4096, 0);
    for (uint64_t i = 0; i < 10; i++) disk_set(disk, i);   /* block 位 0..9 预置 */
    disk_set(disk, 65538);                                  /* slice1 局部 2 预置（撞占用点） */

    bitmap_slices_v2cfg_t cfg{};
    cfg.read_block = fdisk_read;
    cfg.write_block = fdisk_write;
    cfg.io_ctx = &disk;
    cfg.block_bitmap_base_FS_block = 0;
    cfg.block_bitmap_blocks = 8;
    cfg.inode_bitmap_base_FS_block = 8;
    cfg.inode_bitmap_blocks = 4;
    cfg.inobm_chunk_order = 0;
    cfg.blkbm_chunk_order = 0;
    cfg.processor_count = 4;

    Shadow sb, si;
    sb.init(BLK_TOTAL);
    si.init(INO_TOTAL);
    for (uint64_t i = 0; i < 10; i++) sb.set(i, 1);
    sb.set(65538, 1);

    uint64_t rc = 0;
    {
        bitmap_slices_mgr_t_v2 mgr(cfg, &rc);
        CHECK(rc == (uint64_t)RC_OK, "ctor rc=%llu", (unsigned long long)rc);
        CHECK(mgr.slice_count() == 4, "slice_count=%llu",
              (unsigned long long)mgr.slice_count());
        CHECK(mgr.block_free_total() == BLK_TOTAL - 11, "init blk free=%llu",
              (unsigned long long)mgr.block_free_total());
        CHECK(mgr.inode_free_total() == INO_TOTAL, "init ino free=%llu",
              (unsigned long long)mgr.inode_free_total());

        auto blk_ok = [&](const char *tag) {
            CHECK(mgr.block_free_total() == BLK_TOTAL - sb.used(),
                  "%s blk free=%llu 影子=%llu", tag,
                  (unsigned long long)mgr.block_free_total(),
                  (unsigned long long)(BLK_TOTAL - sb.used()));
        };

        g_cpu = 0;
        /* alloc_hard 跳过预置 0..9 → 命中 10；无重复分配 */
        uint64_t b = mgr.block_alloc_hard(10, 0);
        CHECK(b == 10, "hard=%llu (want 10)", (unsigned long long)b);
        CHECK(sb.free_all(b, 10), "hard 重复分配");
        sb.set(b, 10);
        blk_ok("hard");

        /* 跨片 occupy 中途撞占用停：65534 起 6 → slice0 尾 2 位 + slice1 首 2 位，
         * 到 65538（预置占用）停 → rest=2 */
        uint64_t rest = 999;
        const uint64_t occ = mgr.block_occupy_soft(65534, 6, &rest);
        CHECK(occ == 65534 && rest == 2, "occ=%llu rest=%llu (want rest 2)",
              (unsigned long long)occ, (unsigned long long)rest);
        CHECK(sb.free_all(65534, 4), "occupy 重复分配");
        sb.set(65534, 4);
        CHECK(mgr.block_test(65534) && mgr.block_test(65537), "occupy 4 位占用");
        CHECK(mgr.block_test(65538), "预置 65538 仍占用");
        CHECK(!mgr.block_test(65539), "未越染 65539");
        blk_ok("occ");
        /* 跨片 free（slice0 尾 2 + slice1 首 2） */
        mgr.block_free(65534, 4);
        sb.clr(65534, 4);
        CHECK(!mgr.block_test(65534) && !mgr.block_test(65537), "跨片 free 已清");
        CHECK(mgr.block_test(65538), "free 不误清预置");
        blk_ok("free");

        /* 跨 3 片 occupy（slice2 尾 8 + slice3 16）→ 跨 3 片 free */
        rest = 0;
        const uint64_t occ2 = mgr.block_occupy_soft(196600, 24, &rest);
        CHECK(occ2 == 196600 && rest == 0, "occ2=%llu rest=%llu",
              (unsigned long long)occ2, (unsigned long long)rest);
        CHECK(sb.free_all(196600, 24), "occ2 重复分配");
        sb.set(196600, 24);
        CHECK(mgr.block_test(196600) && mgr.block_test(196623), "occ2 段占用");
        blk_ok("occ2");
        mgr.block_free(196600, 24);
        sb.clr(196600, 24);
        CHECK(!mgr.block_test(196600) && !mgr.block_test(196623), "跨 3 片 free");
        blk_ok("free2");

        /* alloc_soft 缺口回传 + 复用不双分配 */
        uint64_t left = 0;
        const uint64_t sft = mgr.block_alloc_soft(100, &left);
        CHECK(sft != PTR_INVALID, "soft");
        const uint64_t got = 100 - left;
        CHECK(got >= 1 && sb.free_all(sft, got), "soft 重复分配");
        sb.set(sft, got);
        blk_ok("soft");
        mgr.block_free(sft, got);
        sb.clr(sft, got);
        blk_ok("soft-free");

        /* 越界：count=0 / count > 片位数 → PTR_INVALID */
        CHECK(mgr.block_alloc_hard(0, 0) == PTR_INVALID, "alloc count=0");
        CHECK(mgr.block_alloc_hard(70000, 0) == PTR_INVALID, "alloc count>片位数");

        /* inode：无 root 保留 → 首枚取 0；cpu3 → 98304 */
        g_cpu = 0;
        uint64_t inum = mgr.inode_alloc();
        CHECK(inum == 0, "inode0=%llu (want 0)", (unsigned long long)inum);
        CHECK(si.free_all(inum, 1), "inode 重复分配");
        si.set(inum, 1);
        CHECK(mgr.inode_test(0), "inode0 占用");
        CHECK(mgr.inode_free_total() == INO_TOTAL - 1, "ino free=%llu",
              (unsigned long long)mgr.inode_free_total());
        mgr.inode_free(0);
        si.clr(0, 1);
        CHECK(!mgr.inode_test(0), "inode0 已还");
        CHECK(mgr.inode_free_total() == INO_TOTAL, "ino free 还原");
        g_cpu = 3;
        inum = mgr.inode_alloc();
        CHECK(inum == 98304, "inode cpu3=%llu (want 98304)", (unsigned long long)inum);
        CHECK(si.free_all(inum, 1), "inode3 重复分配");
        si.set(inum, 1);
        mgr.inode_free(98304);
        si.clr(98304, 1);
        g_cpu = 0;

        /* 逐点影子对拍（抽样） */
        for (uint64_t p = 0; p < BLK_TOTAL; p += 991)
            CHECK(mgr.block_test(p) == (sb.b[(size_t)p] != 0), "shadow blk p=%llu",
                  (unsigned long long)p);
        for (uint64_t p = 0; p < INO_TOTAL; p += 997)
            CHECK(mgr.inode_test(p) == (si.b[(size_t)p] != 0), "shadow ino p=%llu",
                  (unsigned long long)p);

        CHECK(mgr.block_free_total() == BLK_TOTAL - sb.used(), "final blk");
        CHECK(mgr.inode_free_total() == INO_TOTAL - si.used(), "final ino");
    }   /* dtor：逐片 shutdown 落盘 */

    /* 全盘影子对拍：dtor 后盘上 block 区 == 影子、inode 区 == 影子 */
    bool blk_ok = true, ino_ok = true;
    for (uint64_t p = 0; p < BLK_TOTAL; p++)
        if (disk_get(disk, p) != (sb.b[(size_t)p] != 0)) { blk_ok = false; break; }
    for (uint64_t p = 0; p < INO_TOTAL; p++)
        if (disk_get(disk, BLK_TOTAL + p) != (si.b[(size_t)p] != 0)) { ino_ok = false; break; }
    CHECK(blk_ok, "dtor 后 block 区盘 == 影子");
    CHECK(ino_ok, "dtor 后 inode 区盘 == 影子");
    CHECK(disk.rd > 0 && disk.wr > 0, "dtor 落盘 rd=%llu wr=%llu",
          (unsigned long long)disk.rd, (unsigned long long)disk.wr);

    /* mgr 坏 cfg：回调缺 / processor_count=0 → 空态 + 服务拒 */
    bitmap_slices_v2cfg_t bad{};
    bad.block_bitmap_blocks = 8;
    bad.inode_bitmap_blocks = 4;
    bad.processor_count = 4;
    uint64_t rc2 = (uint64_t)RC_OK;
    bitmap_slices_mgr_t_v2 m2(bad, &rc2);
    CHECK(rc2 == (uint64_t)RC_ERR_STATE, "坏 cfg rc=%llu", (unsigned long long)rc2);
    CHECK(m2.slice_count() == 0 && m2.block_free_total() == 0, "坏 cfg 空态");
    CHECK(m2.block_alloc_hard(1, 0) == PTR_INVALID, "坏 cfg alloc");
    CHECK(m2.block_preferred_of(0) == 0, "坏 cfg preferred");

    bitmap_slices_v2cfg_t bad2 = cfg;
    bad2.processor_count = 0;
    uint64_t rc3 = (uint64_t)RC_OK;
    bitmap_slices_mgr_t_v2 m3(bad2, &rc3);
    CHECK(rc3 == (uint64_t)RC_ERR_STATE, "pc=0 rc=%llu", (unsigned long long)rc3);
}

/* ═══════════════════════════════════════════════════════════════════
 * ⑩ 最小复现：drain 未置脏 → evict 不回写 → 已释放位丢失（真实 bug）
 *
 * 链路：在线窗被 test_interval/0-claim 路径 drain（把归还记录并入位图、清位）
 *   但 drain_chunk() **不置 CHUNK_FLAG_DIRTY**；若该窗此前已被 asyn/evict 清脏
 *   （即除本次 drain 外无其它改动），下次 evict_online() 见 dirty=0 → 跳过写回
 *   → 被清掉的位在盘上仍是 1；后续 asyn 读回陈旧盘 → 重扫把 free_count 校准为
 *   偏小值 → 已释放位永久泄漏，且 test_interval 把空闲位误判为占用。
 *   （对照：ensure_online 换片 drain 后有 `if (drained) DIRTY`，alloc 成功也置脏；
 *    唯 test/失败 alloc/0-claim occupy 这几条“只 drain 不成事”路径漏置脏。）
 * 本用例断言**应为**的逻辑，故会 FAIL —— 即 bug 的可重放证据。
 * ═══════════════════════════════════════════════════════════════════ */
static void case_leak_drain_no_dirty() {
    const uint64_t N = 131072;
    FakeDisk d;
    d.base = 0;
    d.blocks = 4;
    d.data.assign((size_t)d.blocks * 4096, 0);
    slice_soul_t soul = soul_of(&d, 0, 4, 0);

    uint64_t out = 0;
    chunk_slice_mgr_t s;
    CHECK(s.enable(&soul, 0) == RC_OK, "enable");
    CHECK(s.alloc_hard(0, 5, out) == RC_OK && out == 0, "alloc 0..4");
    CHECK(s.chunks_asyn() == RC_OK, "asyn#1（清脏/落盘）");
    CHECK(disk_all(d, 0, 5, true), "盘 0..4 = 1");
    /* 在线窗0 空闲（已无脏）；挂归还记录（free 不置脏） */
    CHECK(s.free_interval(0, 5) == RC_OK, "free 0..4");
    CHECK(s.get_free_count() == N, "logical free=%llu (want %llu)",
          (unsigned long long)s.get_free_count(), (unsigned long long)N);
    /* drain 在线窗0 但不置脏：期望此后盘上最终应清 0 */
    CHECK(s.test_interval(false, 0, 5) == RC_OK, "test 触发 drain");
    /* evict 窗0：切到窗1 分配（窗0 不脏 → 不回写） */
    s.set_hint(32768);
    CHECK(s.alloc_hard(0, 10, out) == RC_OK && out == 32768, "w1 alloc");
    /* asyn 读回整片：若 drain 正确置脏，盘上 0..4 应为 0，校准 free = N-10 */
    free_count_calib_t cal{};
    CHECK(s.chunks_asyn(&cal) == RC_OK, "asyn#2");
    CHECK(cal.new_free_count == N - 10,
          "[BUG] asyn 校准后 free=%llu，逻辑应为 %llu（0..4 被 drain 清位但未置脏，"
          "evict 未回写 → 盘上陈旧为 1 → 释放位泄漏）",
          (unsigned long long)cal.new_free_count, (unsigned long long)(N - 10));
    CHECK(s.test_interval(true, 0, 5) == RC_ERR_NOT_FOUND,
          "[BUG] 释放位 0..4 应空闲（test(true)==NOT_FOUND），实得 rc=%llu",
          (unsigned long long)s.test_interval(true, 0, 5));
    CHECK(s.shutdown() == RC_OK, "shutdown");
}

int main() {
    std::signal(SIGALRM, on_alarm);
    alarm(55);                                  /* 限时 ≤60s（留 5s 余量） */

    run_case("gate", case_gate);
    run_case("geom_order0", case_geom_order0);
    run_case("geom_tail_order1", case_geom_tail_order1);
    run_case("geom_order2", case_geom_order2);
    run_case("cross_window", case_cross_window);
    run_case("drift_calib", case_drift_calib);
    run_case("err_inject", case_err_inject);
    run_case("shadow_slice", case_shadow_slice);
    run_case("mgr_adversarial", case_mgr_adversarial);
    run_case("leak_drain_no_dirty", case_leak_drain_no_dirty);

    alarm(0);
    if (g_fail == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILED: %d\n", g_fail);
    return 1;
}

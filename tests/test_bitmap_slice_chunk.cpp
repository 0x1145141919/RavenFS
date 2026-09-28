/* ═══════════════════════════════════════════════════════════════════
 * test_bitmap_slice_chunk.cpp — bitmap_slices CHUNK 后端专项（03 §13 刀④）
 *
 * 靶场：mock chunk_io 盘后端（block/inode 两组独立字节位图当"盘" + I/O 计数
 * + §10.3 审计），合成几何（chunk_order=1 → chunk = 4K<<1 = 8192B = 65536 bit）：
 *   block 组 491520 bit / cpu=2 → per=262144（8 页）
 *     片0 = 4 个整窗；片1 = 229376 bit = 7 页 → k=7，k mod 2 = 1 ≠ 0
 *     → 片1 尾窗 tail = 32768 bit = 恰 1 页（§3 尾块数学）
 *   inode 组 163840 bit → per=98304（3 页）→ 片0 = 2 窗（尾 32768）、片1 = 1 整窗
 *
 * 用例（03 §13 刀④ 验收清单逐条）：
 *   T1  mount 顺序扫描：free_count/组全局计数/2bit 摘要/三段校验 + 尾窗 1 页读
 *   T2  换片快/慢路径 + 在线 free 直改（零 I/O）+ evict 写回（alloc 置脏）
 *   T3  离线 free → record → 上线 drain 顺序（§10.4 发布前先还账）→ TTL 落盘
 *   T4  池空兜底（强制上线直清，零第二批记录）+ record/在线两态 TTL 收口
 *   T5  摘要 ALL_USED 免载入跳过（0 读到 ENOSPC）+ record 追加降级 MIXED（§10.5）
 *   T6  hint 绕回（正向窗序扫毕 → [0,hint) 窗序列，零多余载入）
 *   T7  尾块几何（k mod 2^order ≠ 0 → 尾窗恰 1 页 32768 bit 读）
 *   T8  occupy 跨 chunk 续圈（顶到窗尾换下一窗 = 原跨片续圈同构）
 *   T9  TTL 三态分派（写脏 / 回灌离线 record / 干净跳过）
 *   T10 cfg/backing 契约 + 自描述三段校验（裁决点 1/§7.4）+ 装配失败自清理
 *   T11 随机化影子对账：TTL 收尾后 盘像 == 逻辑影子（端到端不变量）
 *
 * §10.3 可测化：mock 每次回调先断言 ravenfs_bitmap_in_slice_lock() == false
 * （盘 I/O 永不进 slice_lock）；「I/O 只走 chg_mu 慢路径」另以快路径操作的
 * 零 I/O 计数佐证（chg_mu 是库内成员，外部不可见，取其可观测代理）。
 *
 * 2026-09-28 · Docs/runtime_tech/03_chunk_backed_slices.md
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/bitmap_slices.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>

/* bitmap_slices.cpp 导出的审计钩子（头文件已声明；测试用它做 §10.3 断言） */
extern "C" bool ravenfs_bitmap_in_slice_lock(void);

/* ── 计数断言（风格对齐 tests/test_block_occupy.cpp：[PASS]/[FAIL] + 汇总） ── */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do {                                             \
    if (cond) { g_pass++; }                                               \
    else { g_fail++; std::printf("  [FAIL] ");                            \
           std::printf(__VA_ARGS__); std::printf("\n"); }                 \
} while (0)
#define SECTION(name) std::printf("\n-- %s --\n", name)

/* ═══ mock 盘后端：字节位图当盘 + 计数 + 违例探测 ═══ */
struct mock_disk_t {
    uint8_t *blk = nullptr;
    uint8_t *ino = nullptr;
    uint64_t blk_bytes = 0, ino_bytes = 0;
    uint64_t reads = 0, writes = 0;
    uint64_t read_blocks = 0, write_blocks = 0;
    bool lock_violation = false;   /* §10.3 违例：I/O 发生在 slice_lock 内 */
    bool shape_violation = false;  /* 块数/越界违例（4K 块粒度契约） */
    uint64_t fail_reads = 0;       /* 注入：接下来 N 次读直接失败 */

    void alloc_img(uint64_t blk_bits, uint64_t ino_bits) {
        blk_bytes = blk_bits / 8;
        ino_bytes = ino_bits / 8;
        blk = new uint8_t[blk_bytes]();
        ino = new uint8_t[ino_bytes]();
    }
    ~mock_disk_t() { delete[] blk; delete[] ino; }
};

static inline bool img_bit(const uint8_t *img, uint64_t i) {
    return (img[i / 8] >> (i % 8)) & 1u;
}
static inline void img_set(uint8_t *img, uint64_t i) {
    img[i / 8] |= (uint8_t)(1u << (i % 8));
}
static inline void img_clr(uint8_t *img, uint64_t i) {
    img[i / 8] &= (uint8_t)~(1u << (i % 8));
}
static uint64_t img_zeros(const uint8_t *img, uint64_t bytes) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < bytes * 8; i++)
        if (!img_bit(img, i)) n++;
    return n;
}

static bool mock_shape_ok(const mock_disk_t *d, bool is_inode, uint64_t rel, uint64_t nb) {
    const uint64_t total = is_inode ? d->ino_bytes : d->blk_bytes;
    return nb > 0 && (rel + nb) * 4096 <= total;
}

static int mock_read(void *ctx, bool is_inode, uint64_t rel, void *buf, uint64_t nb) {
    mock_disk_t *d = static_cast<mock_disk_t *>(ctx);
    if (ravenfs_bitmap_in_slice_lock()) d->lock_violation = true;   /* §10.3 铁律 */
    d->reads++;
    if (d->fail_reads > 0) { d->fail_reads--; return -1; }
    if (!mock_shape_ok(d, is_inode, rel, nb)) { d->shape_violation = true; return -1; }
    std::memcpy(buf, (is_inode ? d->ino : d->blk) + rel * 4096, nb * 4096);
    d->read_blocks += nb;
    return 0;
}

static int mock_write(void *ctx, bool is_inode, uint64_t rel, const void *buf, uint64_t nb) {
    mock_disk_t *d = static_cast<mock_disk_t *>(ctx);
    if (ravenfs_bitmap_in_slice_lock()) d->lock_violation = true;   /* §10.3 铁律 */
    d->writes++;
    if (!mock_shape_ok(d, is_inode, rel, nb)) { d->shape_violation = true; return -1; }
    std::memcpy((is_inode ? d->ino : d->blk) + rel * 4096, buf, nb * 4096);
    d->write_blocks += nb;
    return 0;
}

static const bitmap_chunk_io_t g_mock_io = { mock_read, mock_write };

/* ═══ 合成几何（见文件头） ═══ */
static constexpr uint64_t BLK_BITS = 491520;   /* 15 页；片1 = 7 页（k mod 2 = 1） */
static constexpr uint64_t INO_BITS = 163840;   /* 5 页 */
static constexpr uint64_t CHUNK_BITS = 65536;  /* 32768 << order(=1)（§3） */
static constexpr uint64_t PAGE_BITS = 32768;   /* 1 位图块 = 4096 B */
static constexpr uint64_t TAIL0 = 458752;      /* 片1 尾窗首 = 262144 + 3*65536 */

/* ═══ fixture：盘像 + mgr ═══ */
struct fixture_t {
    mock_disk_t disk;
    uint64_t res = RC_ERR_STATE;
    bitmap_slices_mgr_t *mgr = nullptr;

    fixture_t() { disk.alloc_img(BLK_BITS, INO_BITS); }
    ~fixture_t() { delete mgr; }

    bitmap_slices_mgr_t *mount(uint8_t order = 1, uint32_t pool_depth = 64,
                               bool skip_self_check = true,
                               uint64_t self_lo = 0, uint64_t self_hi = 0) {
        bitmap_slices_cfg_t cfg;
        cfg.block_bits_raw = nullptr;          /* 裁决点 1：CHUNK 允许空（全图不驻留） */
        cfg.block_bits_count = BLK_BITS;
        cfg.inode_bits_raw = nullptr;
        cfg.inode_bits_count = INO_BITS;
        cfg.logical_processor_count = 2;
        cfg.backing_mode = BITMAP_BACKING_CHUNK;
        cfg.chunk_order = order;
        cfg.chunk_io = &g_mock_io;
        cfg.chunk_io_ctx = &disk;
        cfg.rec_pool_depth = pool_depth;
        cfg.skip_self_check = skip_self_check;
        cfg.self_check_lo = self_lo;
        cfg.self_check_hi = self_hi;
        mgr = new bitmap_slices_mgr_t(cfg, &res);
        return mgr;
    }
    void audit(const char *what) {
        CHECK(!disk.lock_violation, "%s: §10.3 盘 I/O 未进 slice_lock", what);
        CHECK(!disk.shape_violation, "%s: chunk_io 块形状合法", what);
    }
};

/* 盘像铺法：全空闲 + 每个 chunk 窗首位置 1 → 每窗 MIXED（强制真实读盘） */
static void img_mixed(uint8_t *img, uint64_t bits) {
    for (uint64_t w = 0; w < bits; w += CHUNK_BITS) img_set(img, w);
}
static void img_root_lamp(uint8_t *img) { img_set(img, 0); }  /* §7.4 root 亮灯 */

/* ═══ T1 mount 顺序扫描 ═══ */
static void t1_mount_scan() {
    SECTION("T1 mount 顺序扫描：计数/摘要/三段校验 + 尾窗 1 页读");
    fixture_t f;
    for (uint64_t i = 0; i < BLK_BITS; i += 97) img_set(f.disk.blk, i);
    for (uint64_t i = 1; i < INO_BITS; i += 53) img_set(f.disk.ino, i);
    img_root_lamp(f.disk.ino);
    f.mount(1, 64, /*skip=*/false);           /* 走三段校验（root 亮灯恒查） */
    CHECK(f.res == RC_OK, "ctor rc=%llu", (unsigned long long)f.res);
    if (f.res != RC_OK) return;
    CHECK(f.mgr->slice_count() == 2, "slice_count=%llu（期望 2）",
          (unsigned long long)f.mgr->slice_count());
    CHECK(f.mgr->cpu_slots() == 2, "cpu_slots=%llu", (unsigned long long)f.mgr->cpu_slots());
    CHECK(f.mgr->block_per_slice_bits() == 262144, "block per_slice=%llu（期望 262144）",
          (unsigned long long)f.mgr->block_per_slice_bits());
    CHECK(f.mgr->inode_per_slice_bits() == 98304, "inode per_slice=%llu（期望 98304）",
          (unsigned long long)f.mgr->inode_per_slice_bits());
    /* mount = 块组 8 窗 + inode 组 3 窗；读块 = 15 + 5 页（尾窗恰 1 页，§3） */
    CHECK(f.disk.reads == 11, "mount 窗读=%llu（期望 11 = 8+3）",
          (unsigned long long)f.disk.reads);
    CHECK(f.disk.read_blocks == 20, "mount 页读=%llu（期望 20 = 15+5，尾窗 1 页）",
          (unsigned long long)f.disk.read_blocks);
    CHECK(f.disk.writes == 0, "mount 零写盘（write=%llu）", (unsigned long long)f.disk.writes);
    const uint64_t bz = img_zeros(f.disk.blk, f.disk.blk_bytes);
    const uint64_t iz = img_zeros(f.disk.ino, f.disk.ino_bytes);
    CHECK(f.mgr->block_free_total() == bz, "block_free_total=%llu 期望 %llu",
          (unsigned long long)f.mgr->block_free_total(), (unsigned long long)bz);
    CHECK(f.mgr->inode_free_total() == iz, "inode_free_total=%llu 期望 %llu",
          (unsigned long long)f.mgr->inode_free_total(), (unsigned long long)iz);
    /* root 亮灯：inode ctl0 摘要 MIXED → 走慢路径上线读（+1 读） */
    CHECK(f.mgr->inode_test(0) == true, "root inode 亮灯（inode_test(0)==true）");
    CHECK(f.disk.reads == 12, "root test 载入 1 窗（reads=%llu 期望 12）",
          (unsigned long long)f.disk.reads);
    /* 尾窗末位（片1 尾窗 32768 bit）：混合 → 上线读（+1 读），返回盘真值 */
    CHECK(f.mgr->block_test(BLK_BITS - 1) == img_bit(f.disk.blk, BLK_BITS - 1),
          "尾窗末位 block_test 与盘一致");
    CHECK(f.disk.reads == 13, "尾窗 test 载入 1 窗（reads=%llu 期望 13）",
          (unsigned long long)f.disk.reads);
    f.audit("T1");
}

/* ═══ T2 换片快/慢路径 + 在线 free 直改 + evict 写回 ═══ */
static void t2_switch_paths() {
    SECTION("T2 换片快/慢路径 + 在线 free 直改 + evict 写回");
    fixture_t f;
    img_mixed(f.disk.blk, BLK_BITS);
    img_mixed(f.disk.ino, INO_BITS);
    img_root_lamp(f.disk.ino);
    f.mount();
    CHECK(f.res == RC_OK, "ctor rc=%llu", (unsigned long long)f.res);
    if (f.res != RC_OK) return;
    const uint64_t r0 = f.disk.reads, w0 = f.disk.writes;
    const uint64_t free0 = f.mgr->block_free_total();

    /* 慢路径：首分配载入窗（MIXED → 真读） */
    uint64_t remain = 0;
    const uint64_t a1 = f.mgr->block_alloc_soft(1, &remain);
    CHECK(a1 != PTR_INVALID, "alloc#1 成功");
    CHECK(f.disk.reads == r0 + 1, "慢路径载入 1 窗（reads-r0=%llu 期望 1）",
          (unsigned long long)(f.disk.reads - r0));
    /* 快路径：hint 局部连发 4 次 → 零 I/O（chg_mu 不碰，§5/§10.3） */
    for (int i = 0; i < 4; i++) f.mgr->block_alloc_soft(1, &remain);
    CHECK(f.disk.reads == r0 + 1 && f.disk.writes == w0,
          "快路径 4 连发零 I/O（r=%llu w=%llu）",
          (unsigned long long)(f.disk.reads - r0), (unsigned long long)(f.disk.writes - w0));
    const uint64_t free1 = f.mgr->block_free_total();   /* 已分配 5 位后的基数 */
    /* 在线 free 直改：零 record 零 I/O，free_count 立即反映，test 即读内存 */
    f.mgr->block_free(a1, 1);
    CHECK(f.mgr->block_free_total() == free1 + 1, "在线 free 后 free_total 恰 +1（%llu）",
          (unsigned long long)f.mgr->block_free_total());
    CHECK(f.mgr->block_test(a1) == false, "在线 free 后 block_test==false");
    CHECK(f.disk.reads == r0 + 1 && f.disk.writes == w0,
          "在线 free/test 全程零 I/O（r=%llu w=%llu）",
          (unsigned long long)(f.disk.reads - r0), (unsigned long long)(f.disk.writes - w0));
    /* 换片：occupy 锚在窗1 → 载入窗1 + evict 脏窗0（alloc 置过脏 → 写回） */
    const uint64_t anchor = CHUNK_BITS + 100;
    uint64_t rest = 0;
    const uint64_t occ = f.mgr->block_occupy_soft(anchor, 200, &rest);
    CHECK(occ == anchor && rest == 0, "occupy 窗1 命中（ret=%llu rest=%llu）",
          (unsigned long long)occ, (unsigned long long)rest);
    CHECK(f.disk.reads == r0 + 2, "慢路径换片载入窗1（r=%llu 期望 +2）",
          (unsigned long long)(f.disk.reads - r0));
    CHECK(f.disk.writes == w0 + 1, "evict 写回脏窗0（w=%llu 期望 +1）",
          (unsigned long long)(f.disk.writes - w0));
    /* occupy 在态在线窗 → 先 TTL 回写再对盘（§9） */
    f.mgr->chunk_ttl_sync();
    CHECK(img_bit(f.disk.blk, anchor), "TTL 回写后盘像反映 occupy 置位");
    CHECK(f.mgr->block_free_total() == free0 - 204,
          "账本累计：free0-5 alloc+1 free-200 occupy = %llu",
          (unsigned long long)f.mgr->block_free_total());
    f.audit("T2");
}

/* ═══ T3 离线 free → record → 上线 drain 顺序 → TTL 落盘 ═══ */
static void t3_record_drain() {
    SECTION("T3 离线 free → record → 上线 drain 顺序（§10.4）→ 落盘");
    fixture_t f;
    img_mixed(f.disk.blk, BLK_BITS);
    img_mixed(f.disk.ino, INO_BITS);
    img_root_lamp(f.disk.ino);
    f.mount();
    CHECK(f.res == RC_OK, "ctor rc=%llu", (unsigned long long)f.res);
    if (f.res != RC_OK) return;
    const uint64_t r0 = f.disk.reads, w0 = f.disk.writes;
    const uint64_t free0 = f.mgr->block_free_total();

    uint64_t remain = 0;
    const uint64_t A = f.mgr->block_alloc_soft(1, &remain);   /* 载入窗0 */
    CHECK(A != PTR_INVALID && A < CHUNK_BITS, "A 落在窗0（A=%llu）", (unsigned long long)A);
    const uint64_t r1 = f.disk.reads, w1 = f.disk.writes;

    /* 换到窗1：窗0 下线（A 已在盘上 = 1） */
    uint64_t rest = 0;
    const uint64_t occ1 = f.mgr->block_occupy_soft(CHUNK_BITS + 100, 100, &rest);
    CHECK(occ1 == CHUNK_BITS + 100, "换到窗1");
    CHECK(f.disk.reads == r1 + 1 && f.disk.writes == w1 + 1,
          "换片 = 读窗1 + 写回窗0（r+=%llu w+=%llu）",
          (unsigned long long)(f.disk.reads - r1), (unsigned long long)(f.disk.writes - w1));

    /* 离线 free → 挂 record：零 I/O、free_count 记账、test 走 record 覆盖 */
    const uint64_t r2 = f.disk.reads, w2 = f.disk.writes;
    f.mgr->block_free(A, 1);
    CHECK(f.disk.reads == r2 && f.disk.writes == w2, "离线 free 挂 record 零 I/O");
    CHECK(f.mgr->block_free_total() == free0 - 100,
          "record 挂账后 free_total=%llu（free0−100 occupy，A 返还）",
          (unsigned long long)f.mgr->block_free_total());
    CHECK(f.mgr->block_test(A) == false, "record 覆盖 → block_test==false（零 I/O）");
    CHECK(f.disk.reads == r2, "record 覆盖的 test 零 I/O（r+=%llu）",
          (unsigned long long)(f.disk.reads - r2));
    CHECK(img_bit(f.disk.blk, A), "盘像仍是 1（record 未落盘，等 drain/TTL）");

    /* 上线窗0：发布前必须先 drain（§10.4）→ B 可被占、A 必须已是 0 */
    const uint64_t B = 10;
    const uint64_t occB = f.mgr->block_occupy_soft(B, 1, &rest);
    CHECK(occB == B, "载入窗0 并占领 B（drain 未挡 occupy）");
    CHECK(f.mgr->block_test(A) == false, "★ drain 先于发布：上线后 A 已清 0（§10.4）");
    CHECK(f.disk.reads == r2 + 1, "上线窗0 载入 1 次（r+=%llu）",
          (unsigned long long)(f.disk.reads - r2));

    /* 再换走（窗0 evict）→ record 已还账的 A 必须写到盘上 */
    const uint64_t w3 = f.disk.writes;
    f.mgr->block_occupy_soft(2 * CHUNK_BITS + 500, 10, &rest);
    CHECK(f.disk.writes == w3 + 1, "evict 脏窗0（含 drain 还账）（w+=%llu）",
          (unsigned long long)(f.disk.writes - w3));
    CHECK(!img_bit(f.disk.blk, A), "★ 盘像 A==0：record → drain → evict 全链路落盘");
    CHECK(img_bit(f.disk.blk, B), "盘像 B==1：occupy 置位已落盘");
    f.audit("T3");
}

/* ═══ T4 池空兜底（rec_pool_depth=1）═══ */
static void t4_pool_exhaust() {
    SECTION("T4 池空兜底：强制上线直清，零第二批记录");
    fixture_t f;
    img_mixed(f.disk.blk, BLK_BITS);
    img_mixed(f.disk.ino, INO_BITS);
    img_root_lamp(f.disk.ino);
    f.mount(1, /*pool_depth=*/1);              /* 池深 1：第二个离线段必然池空 */
    CHECK(f.res == RC_OK, "ctor rc=%llu", (unsigned long long)f.res);
    if (f.res != RC_OK) return;
    const uint64_t r0 = f.disk.reads, w0 = f.disk.writes;
    const uint64_t free0 = f.mgr->block_free_total();

    uint64_t remain = 0;
    const uint64_t A = f.mgr->block_alloc_soft(1, &remain);   /* 载入窗0（r+1） */
    CHECK(A != PTR_INVALID, "alloc A 成功");
    uint64_t rest = 0;
    f.mgr->block_occupy_soft(CHUNK_BITS + 100, 100, &rest);   /* 换到窗1（r+1 w+1） */
    f.mgr->block_free(A, 1);                                 /* 离线 → 用掉唯一节点，零 I/O */
    CHECK(f.disk.reads == r0 + 2 && f.disk.writes == w0 + 1,
          "record 用尽池前零 I/O（r+=%llu w+=%llu）",
          (unsigned long long)(f.disk.reads - r0), (unsigned long long)(f.disk.writes - w0));

    /* 窗2 离线 + 池空 → 兜底：强制上线（evict 窗1 + 读窗2）→ 直清 */
    const uint64_t Y = 2 * CHUNK_BITS;                      /* 盘像窗2 首位 = 1 */
    f.mgr->block_free(Y, 1);
    CHECK(f.disk.reads == r0 + 3, "兜底强制上线窗2（r+=%llu 期望 3）",
          (unsigned long long)(f.disk.reads - r0));
    CHECK(f.disk.writes == w0 + 2, "兜底先 evict 脏窗1（w+=%llu 期望 2）",
          (unsigned long long)(f.disk.writes - w0));
    CHECK(f.mgr->block_test(Y) == false, "兜底直清后 block_test==false（窗2 在线）");
    CHECK(f.mgr->block_free_total() == free0 - 99,
          "free_total=%llu（期望 %llu：-100 occupy +A 返还 +Y 返还）",
          (unsigned long long)f.mgr->block_free_total(), (unsigned long long)(free0 - 99));
    /* TTL 收口：窗0 的 record 回灌（态2）+ 窗2 在线脏写（态1） */
    const uint64_t rT = f.disk.reads, wT = f.disk.writes;
    f.mgr->chunk_ttl_sync();
    CHECK(f.disk.reads == rT + 1 && f.disk.writes == wT + 2,
          "TTL = 态2 读写各 1 + 态1 写 1（r+=%llu w+=%llu）",
          (unsigned long long)(f.disk.reads - rT), (unsigned long long)(f.disk.writes - wT));
    CHECK(!img_bit(f.disk.blk, A), "兜底不丢账：A 经 TTL 落盘为 0");
    CHECK(!img_bit(f.disk.blk, Y), "兜底不丢账：Y 经 TTL 落盘为 0");
    f.audit("T4");
}

/* ═══ T5 摘要 ALL_USED 跳过 + record 降级 ═══ */
static void t5_summary() {
    SECTION("T5 摘要 ALL_USED 免载入跳过 + record 追加降级 MIXED");
    fixture_t f;
    std::memset(f.disk.blk, 0xFF, f.disk.blk_bytes);   /* block 组全占用 */
    img_root_lamp(f.disk.ino);                         /* inode 只亮 root */
    f.mount(1, 64, /*skip=*/false);
    CHECK(f.res == RC_OK, "ctor rc=%llu", (unsigned long long)f.res);
    if (f.res != RC_OK) return;
    const uint64_t r0 = f.disk.reads;

    /* 全片 ALL_USED → 扫描不载入任何窗，直接 ENOSPC（§7「不产生连锁载入悬崖」） */
    uint64_t remain = 0;
    const uint64_t got = f.mgr->block_alloc_soft(1, &remain);
    CHECK(got == PTR_INVALID, "全占用 → PTR_INVALID（ENOSPC）");
    CHECK(f.disk.reads == r0, "★ 摘要跳过：ENOSPC 扫描零载入（r+=%llu）",
          (unsigned long long)(f.disk.reads - r0));

    /* 离线 free 一个已占位 → record 零 I/O + 摘要降级 MIXED */
    const uint64_t P = CHUNK_BITS + 1234;
    f.mgr->block_free(P, 1);
    CHECK(f.disk.reads == r0, "离线 free 挂 record 零 I/O（r+=%llu）",
          (unsigned long long)(f.disk.reads - r0));
    /* 再扫描：窗1 已降级 MIXED → 载入 1 窗即命中 P（不被 ALL_USED 误跳 = §10.5） */
    const uint64_t got2 = f.mgr->block_alloc_soft(1, &remain);
    CHECK(got2 == P, "降级后分配命中 P（got=%llu 期望 %llu）",
          (unsigned long long)got2, (unsigned long long)P);
    CHECK(f.disk.reads == r0 + 1, "只载入降级窗（r+=%llu 期望 1）",
          (unsigned long long)(f.disk.reads - r0));
    CHECK(f.mgr->block_test(P) == true, "P 已分配 → block_test==true（在线零 I/O）");
    f.audit("T5");
}

/* ═══ T6 hint 绕回 ═══ */
static void t6_hint_wrap() {
    SECTION("T6 hint 绕回：正向窗序扫毕 → [0,hint) 窗序列");
    fixture_t f;
    /* 窗0 全空闲；窗1..片尾全占用（mount 摘要 = ALL_USED → 扫描免载入） */
    std::memset(f.disk.blk, 0xFF, f.disk.blk_bytes);
    std::memset(f.disk.blk, 0x00, CHUNK_BITS / 8);
    img_root_lamp(f.disk.ino);
    f.mount(1, 64, /*skip=*/false);
    CHECK(f.res == RC_OK, "ctor rc=%llu", (unsigned long long)f.res);
    if (f.res != RC_OK) return;
    const uint64_t free0 = f.mgr->block_free_total();
    const uint64_t r0 = f.disk.reads;

    /* 硬分配把整个窗0 吃掉 → hint 顶到 65536（跨进窗1 的边界） */
    const uint64_t got = f.mgr->block_alloc_hard(CHUNK_BITS, 0);
    CHECK(got != PTR_INVALID, "窗0 整窗硬分配成功（got=%llu）", (unsigned long long)got);
    CHECK(f.mgr->block_free_total() == free0 - CHUNK_BITS, "free_total 扣掉整窗");

    /* 在窗0 里放 4 个空位（窗0 在线 → 直清零 I/O，hint 不回退） */
    f.mgr->block_free(10, 4);
    CHECK(f.disk.reads == r0, "在线 free 零 I/O（r+=%llu）",
          (unsigned long long)(f.disk.reads - r0));

    /* 再分配 1 位：正向窗1..3 全 ALL_USED 免载入跳过 → 绕回 [0,65536) 命中 */
    const uint64_t wrap = f.mgr->block_alloc_hard(1, 0);
    CHECK(wrap != PTR_INVALID && wrap < CHUNK_BITS,
          "★ 绕回命中窗0（wrap=%llu < %llu）", (unsigned long long)wrap, (unsigned long long)CHUNK_BITS);
    CHECK(f.disk.reads == r0, "绕回扫描零载入（窗0 在线 + 他窗摘要跳过）（r+=%llu）",
          (unsigned long long)(f.disk.reads - r0));
    f.audit("T6");
}

/* ═══ T7 尾块几何（k mod 2^order ≠ 0）═══ */
static void t7_tail_chunk() {
    SECTION("T7 尾块几何：片1 尾窗 = 32768 bit = 恰 1 页");
    fixture_t f;
    img_mixed(f.disk.blk, BLK_BITS);
    img_mixed(f.disk.ino, INO_BITS);
    img_root_lamp(f.disk.ino);
    f.mount();
    CHECK(f.res == RC_OK, "ctor rc=%llu", (unsigned long long)f.res);
    if (f.res != RC_OK) return;
    const uint64_t free0 = f.mgr->block_free_total();
    const uint64_t rb0 = f.disk.read_blocks, r0 = f.disk.reads;

    /* 锚点 = 尾窗首 + 10（避开窗首的铺置 1 位），认领 32700（≤ 尾窗余量 32758） */
    const uint64_t anchor = TAIL0 + 10;
    uint64_t rest = 0;
    const uint64_t occ = f.mgr->block_occupy_soft(anchor, 32700, &rest);
    CHECK(occ == anchor && rest == 0, "尾窗 occupy 命中（rest=%llu）",
          (unsigned long long)rest);
    CHECK(f.disk.read_blocks == rb0 + 1, "★ 尾窗载入恰 1 页（pages+=%llu）",
          (unsigned long long)(f.disk.read_blocks - rb0));
    CHECK(f.disk.reads == r0 + 1, "尾窗载入 1 次（r+=%llu）",
          (unsigned long long)(f.disk.reads - r0));
    CHECK(f.mgr->block_test(anchor) == true, "尾窗已占位 → block_test==true（零 I/O）");
    CHECK(f.mgr->block_test(BLK_BITS - 1) == false, "尾窗外末位仍空闲");
    CHECK(f.mgr->block_free_total() == free0 - 32700, "free_total 扣 32700（%llu）",
          (unsigned long long)f.mgr->block_free_total());
    f.mgr->block_free(anchor, 32700);
    CHECK(f.mgr->block_free_total() == free0, "整段释放回初值（尾窗边界算术无漂移）");
    f.audit("T7");
}

/* ═══ T8 occupy 跨 chunk 续圈 ═══ */
static void t8_occupy_cross_chunk() {
    SECTION("T8 occupy 跨 chunk 续圈（顶到窗尾换下一窗）");
    fixture_t f;
    /* 全空闲；窗0 放 1 个占用位（逼 MIXED 真读），窗1 的续路段保持空闲 */
    img_set(f.disk.blk, 100);
    img_set(f.disk.blk, 2 * CHUNK_BITS + 77);   /* 窗2 也 MIXED（防 ALL_FREE 免读干扰计数） */
    img_root_lamp(f.disk.ino);
    img_mixed(f.disk.ino, INO_BITS);
    f.mount();
    CHECK(f.res == RC_OK, "ctor rc=%llu", (unsigned long long)f.res);
    if (f.res != RC_OK) return;
    const uint64_t free0 = f.mgr->block_free_total();
    const uint64_t r0 = f.disk.reads;

    /* 锚点 = 窗0 尾 10 位处，要 100 → 窗0 认 10、窗1 续 90 */
    const uint64_t anchor = CHUNK_BITS - 10;
    uint64_t rest = 0;
    const uint64_t occ = f.mgr->block_occupy_soft(anchor, 100, &rest);
    CHECK(occ == anchor && rest == 0, "跨窗 occupy 成功（occ=%llu rest=%llu）",
          (unsigned long long)occ, (unsigned long long)rest);
    /* 续圈确实跨进窗1（下方 100 位含 [65536,65626)）；窗1 盘态全 0 →
     * 摘要 ALL_FREE 免读直置空窗（§7 摘要收益）→ 只有窗0 真读 1 次 */
    CHECK(f.disk.reads == r0 + 1, "窗0 真读 1 次 + 窗1 ALL_FREE 免读（r+=%llu）",
          (unsigned long long)(f.disk.reads - r0));
    bool all_used = true;
    for (uint64_t i = 0; i < 100; i++)
        if (!f.mgr->block_test(anchor + i)) all_used = false;
    CHECK(all_used, "★ 100 位全部置位（含跨窗的 [65536,65626)）");
    CHECK(f.mgr->block_free_total() == free0 - 100, "free_total 扣 100（%llu）",
          (unsigned long long)f.mgr->block_free_total());
    f.mgr->block_free(anchor, 100);
    CHECK(f.mgr->block_free_total() == free0, "跨窗 run 释放回初值（block_free 跨段归还）");
    f.audit("T8");
}

/* ═══ T9 TTL 三态分派 ═══ */
static void t9_ttl() {
    SECTION("T9 TTL 三态分派：写脏 / 回灌离线 record / 干净跳过");
    fixture_t f;
    img_mixed(f.disk.blk, BLK_BITS);
    img_mixed(f.disk.ino, INO_BITS);
    img_root_lamp(f.disk.ino);
    f.mount();
    CHECK(f.res == RC_OK, "ctor rc=%llu", (unsigned long long)f.res);
    if (f.res != RC_OK) return;
    const uint64_t r0 = f.disk.reads, w0 = f.disk.writes;

    uint64_t remain = 0;
    const uint64_t A = f.mgr->block_alloc_soft(1, &remain);   /* 脏 online */
    CHECK(A != PTR_INVALID, "alloc A");
    const uint64_t r1 = f.disk.reads, w1 = f.disk.writes;
    f.mgr->chunk_ttl_sync();
    CHECK(f.disk.writes == w1 + 1, "态1 ONLINE&&dirty → 写 1（w+=%llu）",
          (unsigned long long)(f.disk.writes - w1));
    CHECK(f.disk.reads == r1, "态1 零读（r+=%llu）", (unsigned long long)(f.disk.reads - r1));
    const uint64_t w2 = f.disk.writes;
    f.mgr->chunk_ttl_sync();
    CHECK(f.disk.writes == w2, "★ 干净 chunk 跳过（态3，w+=%llu 期望 0）",
          (unsigned long long)(f.disk.writes - w2));

    /* 制造离线 record：换到窗1 再 free 窗0 的 A */
    uint64_t rest = 0;
    f.mgr->block_occupy_soft(CHUNK_BITS + 100, 100, &rest);   /* evict 窗0（A 已写 1） */
    CHECK(img_bit(f.disk.blk, A), "evict 后盘 A=1");
    const uint64_t r2 = f.disk.reads, w3 = f.disk.writes;
    f.mgr->block_free(A, 1);                                 /* 离线 record */
    f.mgr->chunk_ttl_sync();
    CHECK(f.disk.reads == r2 + 1, "态2 回灌读 1（r+=%llu）",
          (unsigned long long)(f.disk.reads - r2));
    CHECK(f.disk.writes == w3 + 2,
          "态2 回灌写 1 + 态1 在线脏窗（窗1 occupy）写 1（w+=%llu 期望 2）",
          (unsigned long long)(f.disk.writes - w3));
    CHECK(!img_bit(f.disk.blk, A), "★ 态2 后盘 A=0（record 已回灌）");
    const uint64_t w4 = f.disk.writes;
    f.mgr->chunk_ttl_sync();
    CHECK(f.disk.writes == w4, "账清完后再 TTL 零写（w+=%llu）",
          (unsigned long long)(f.disk.writes - w4));
    (void)r0; (void)w0;
    f.audit("T9");
}

/* ═══ T10 cfg/backing 契约 + 自描述三段校验 ═══ */
static void t10_contract() {
    SECTION("T10 cfg/backing 契约 + 自描述三段校验 + 装配失败自清理");

    /* (a) FULL 必须非空 raw（裁决点 1 的另一半） */
    {
        bitmap_slices_cfg_t cfg;                       /* 全默认 = FULL + raw 空 */
        cfg.block_bits_count = BLK_BITS;
        cfg.inode_bits_count = INO_BITS;
        uint64_t rc = RC_OK;
        bitmap_slices_mgr_t m(cfg, &rc);
        CHECK(rc != RC_OK, "FULL + raw 空 → 拒装（现行为不变）");
        CHECK(m.slice_count() == 0, "失败态 slice_count==0");
    }
    /* (b) FULL + 合法 raw → 正常装配，raw 直读语义不变（红线对照） */
    {
        uint8_t *br = nullptr, *ir = nullptr;
        posix_memalign((void **)&br, 4096, BLK_BITS / 8);
        posix_memalign((void **)&ir, 4096, INO_BITS / 8);
        std::memset(br, 0x00, BLK_BITS / 8);
        std::memset(ir, 0x00, INO_BITS / 8);
        img_set(br, 7); img_set(ir, 0);
        bitmap_slices_cfg_t cfg;
        cfg.block_bits_raw = br; cfg.block_bits_count = BLK_BITS;
        cfg.inode_bits_raw = ir; cfg.inode_bits_count = INO_BITS;
        uint64_t rc = RC_ERR_STATE;
        bitmap_slices_mgr_t m(cfg, &rc);
        CHECK(rc == RC_OK, "FULL + 合法 raw 装配成功（rc=%llu）", (unsigned long long)rc);
        CHECK(m.block_test(7) == true && m.block_test(8) == false, "FULL raw 直读语义不变");
        CHECK(m.inode_test(0) == true, "FULL inode root 直读");
        free(br); free(ir);
    }
    /* (c)(d) CHUNK 缺 io / order 越界 → 装配期拒（§2） */
    {
        bitmap_slices_cfg_t cfg;
        cfg.block_bits_count = BLK_BITS; cfg.inode_bits_count = INO_BITS;
        cfg.backing_mode = BITMAP_BACKING_CHUNK;    /* chunk_io = nullptr */
        uint64_t rc = RC_OK;
        bitmap_slices_mgr_t m(cfg, &rc);
        CHECK(rc != RC_OK, "CHUNK 缺 chunk_io → 拒装");
    }
    {
        mock_disk_t d; d.alloc_img(BLK_BITS, INO_BITS);
        img_root_lamp(d.ino);
        bitmap_slices_cfg_t cfg;
        cfg.block_bits_count = BLK_BITS; cfg.inode_bits_count = INO_BITS;
        cfg.backing_mode = BITMAP_BACKING_CHUNK;
        cfg.chunk_order = 9;                        /* > 8 */
        cfg.chunk_io = &g_mock_io; cfg.chunk_io_ctx = &d;
        uint64_t rc = RC_OK;
        bitmap_slices_mgr_t m(cfg, &rc);
        CHECK(rc != RC_OK, "chunk_order=9 → 拒装（有效 0..8）");
    }
    /* (e) mount 读失败 → 拒装且对象自清理（§7 I/O 失败路径。
     * 注：ctor 合约只回传 RC_ERR_STATE 现行为，不细分错误码——FULL 同款，不改）。 */
    {
        fixture_t f;
        img_mixed(f.disk.blk, BLK_BITS);
        img_root_lamp(f.disk.ino);
        f.disk.fail_reads = 1;
        f.mount();
        CHECK(f.res != RC_OK, "mount 首读失败 → 拒装（rc=%llu）",
              (unsigned long long)f.res);
        CHECK(f.mgr->block_free_total() == 0, "失败态自清理（slices 空 → free_total=0）");
    }
    /* (f) 自描述三段校验：block 尾段全 1 → 过；挖 0 → 拒挂；skip → 过 */
    {
        fixture_t f;
        std::memset(f.disk.blk, 0x00, f.disk.blk_bytes);
        for (uint64_t i = BLK_BITS - CHUNK_BITS; i < BLK_BITS; i++) img_set(f.disk.blk, i);
        img_root_lamp(f.disk.ino);
        const uint64_t lo = BLK_BITS - CHUNK_BITS, hi = BLK_BITS;
        f.mount(1, 64, /*skip=*/false, lo, hi);
        CHECK(f.res == RC_OK, "尾段全 1 → 三段校验过（rc=%llu）",
              (unsigned long long)f.res);
    }
    {
        fixture_t f;
        std::memset(f.disk.blk, 0x00, f.disk.blk_bytes);
        for (uint64_t i = BLK_BITS - CHUNK_BITS; i < BLK_BITS; i++) img_set(f.disk.blk, i);
        img_clr(f.disk.blk, BLK_BITS - 1234);          /* 尾段挖 0 */
        img_root_lamp(f.disk.ino);
        const uint64_t lo = BLK_BITS - CHUNK_BITS, hi = BLK_BITS;
        f.mount(1, 64, /*skip=*/false, lo, hi);
        CHECK(f.res != RC_OK, "★ 尾段有 0 → 拒挂（rc=%llu）", (unsigned long long)f.res);
        CHECK(f.mgr->block_free_total() == 0, "拒挂后对象自清理（free_total=0）");
        fixture_t g;
        std::memset(g.disk.blk, 0x00, g.disk.blk_bytes);
        for (uint64_t i = BLK_BITS - CHUNK_BITS; i < BLK_BITS; i++) img_set(g.disk.blk, i);
        img_clr(g.disk.blk, BLK_BITS - 1234);
        img_root_lamp(g.disk.ino);
        g.mount(1, 64, /*skip=*/true, lo, hi);
        CHECK(g.res == RC_OK, "skip_self_check=true → 放行（单测合成位图）");
    }
    /* (g) root 亮灯：inode bit0 = 0 → 拒挂 */
    {
        fixture_t f;
        img_mixed(f.disk.blk, BLK_BITS);
        img_mixed(f.disk.ino, INO_BITS);
        img_clr(f.disk.ino, 0);
        f.mount(1, 64, /*skip=*/false);
        CHECK(f.res != RC_OK, "★ root 灯灭 → 拒挂（rc=%llu）", (unsigned long long)f.res);
    }
}

/* ═══ T11 随机化影子对账（端到端不变量）═══ */
static void t11_shadow_reconcile() {
    SECTION("T11 随机化影子对账：TTL 收尾后 盘像 == 逻辑影子");
    fixture_t f;
    img_mixed(f.disk.blk, BLK_BITS);
    for (uint64_t i = 1; i < INO_BITS; i += 41) img_set(f.disk.ino, i);
    img_root_lamp(f.disk.ino);
    f.mount(1, 8);                                  /* 池深压到 8（触发兜底路径） */
    CHECK(f.res == RC_OK, "ctor rc=%llu", (unsigned long long)f.res);
    if (f.res != RC_OK) return;

    /* 影子 = 盘像初值 ⊕ 我施加的 alloc/occupy/free */
    uint8_t *sb = new uint8_t[f.disk.blk_bytes];
    uint8_t *si = new uint8_t[f.disk.ino_bytes];
    std::memcpy(sb, f.disk.blk, f.disk.blk_bytes);
    std::memcpy(si, f.disk.ino, f.disk.ino_bytes);

    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    uint64_t n_alloc = 0, n_occ = 0, n_free = 0;
    for (int i = 0; i < 3000; i++) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        const uint64_t r = rng >> 17;
        const int op = (int)((r >> 3) % 5);
        if (op == 0 || op == 1) {                     /* 软分配 1..16 */
            const uint64_t n = 1 + ((r >> 11) % 16);
            uint64_t left = 0;
            const uint64_t got = f.mgr->block_alloc_soft(n, &left);
            if (got != PTR_INVALID) {
                for (uint64_t k = 0; k < n - left; k++) img_set(sb, got + k);
                n_alloc++;
            }
        } else if (op == 2) {                          /* 锚定认领 1..64 */
            const uint64_t n = 1 + ((r >> 11) % 64);
            const uint64_t base = (r >> 19) % (BLK_BITS - 64);
            uint64_t left = 0;
            if (f.mgr->block_occupy_soft(base, n, &left) != PTR_INVALID) {
                for (uint64_t k = 0; k < n - left; k++) img_set(sb, base + k);
                n_occ++;
            }
        } else if (op == 3) {                          /* 释放一段 */
            const uint64_t base = (r >> 19) % (BLK_BITS - 64);
            f.mgr->block_free(base, 1 + ((r >> 27) % 32));
            for (uint64_t k = 0; k < 1 + ((r >> 27) % 32); k++) img_clr(sb, base + k);
            n_free++;
        } else {                                       /* inode 槽 */
            if ((r >> 11) & 1) {
                const uint64_t ino = f.mgr->inode_alloc();
                if (ino != PTR_INVALID) { img_set(si, ino); n_alloc++; }
            } else {
                const uint64_t ino = (r >> 19) % INO_BITS;
                if (ino != 0) { f.mgr->inode_free(ino); img_clr(si, ino); n_free++; }
            }
        }
    }
    /* 收尾：所有脏 online 窗 + 离线 record 一次性写回（§9） */
    f.mgr->chunk_ttl_sync();

    int bb = std::memcmp(sb, f.disk.blk, f.disk.blk_bytes);
    int ib = std::memcmp(si, f.disk.ino, f.disk.ino_bytes);
    CHECK(bb == 0, "★ block 盘像 == 影子（%s）", bb == 0 ? "OK" : "MISMATCH");
    CHECK(ib == 0, "★ inode 盘像 == 影子（%s）", ib == 0 ? "OK" : "MISMATCH");
    if (bb != 0) {
        for (uint64_t i = 0; i < BLK_BITS; i++)
            if (img_bit(sb, i) != img_bit(f.disk.blk, i)) {
                std::printf("    首个分歧 bit=%llu 影子=%d 盘=%d\n",
                            (unsigned long long)i, (int)img_bit(sb, i),
                            (int)img_bit(f.disk.blk, i));
                break;
            }
    }
    std::printf("    ops: alloc=%llu occupy=%llu free=%llu\n",
                (unsigned long long)n_alloc, (unsigned long long)n_occ,
                (unsigned long long)n_free);
    delete[] sb;
    delete[] si;
    f.audit("T11");
}

int main() {
    std::printf("== bitmap_slices CHUNK 后端专项（03 §13 刀④）==\n");
    t1_mount_scan();
    t2_switch_paths();
    t3_record_drain();
    t4_pool_exhaust();
    t5_summary();
    t6_hint_wrap();
    t7_tail_chunk();
    t8_occupy_cross_chunk();
    t9_ttl();
    t10_contract();
    t11_shadow_reconcile();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) std::printf("RESULT: ALL PASS\n");
    return g_fail == 0 ? 0 : 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * test_bitmap_slice_invariants.cpp — §10 不变量缺口专项（Phase A-3）
 *
 * 既有 125 项（test_bitmap_slice_chunk.cpp）已覆盖的不重写；本文件只补缺口：
 *   INV-1  §10.3/§5 时序：盘 I/O 在途时 —— 快路径零阻塞 / 换片被 chg_mu
 *          串行 / I/O 永不进 slice_lock（mock 门闸把写钉在回调里做确定性时序）
 *   INV-6  §10.6 计数与驻留无关：换片链 / 离线 record / drain / 池空兜底 /
 *          TTL 全程 free_count == 影子真值（drain 绝不重复记账）
 *   INV-8  §10.8 尾块几何：chunk_order ∈ {0,1,4,8} × 尾>0 / 尾==0 矩阵 ——
 *          mount/换片/TTL 的读写块数逐窗精确（无 sub-page、无越读）
 *   INV-9  §10.9 池空兜底：兜底强制上线的 I/O 精确计数 + 兜底后**池节点回收
 *          可复用**（后续离线 free 零 I/O，不退化成每次兜底）
 *   INV-5b §7 摘要 ALL_FREE 免载入：block_test / alloc 全程零读盘
 *
 * 与既有测试的关系：T3(④drain 顺序)/T5(⑤ALL_USED+降级)/T4(⑨兜底主路径)/
 * T7(⑧尾块 order=1 单点)/T9(⑨TTL 三态)/T11(镜像影子) 均在 125 项内，此处
 * 不复制，只扩几何/时序/计数维度。发现 bug → 只登记复现单，不改实现。
 *
 * 2026-09-28 · Docs/runtime_tech/03_chunk_backed_slices.md §10
 * ═══════════════════════════════════════════════════════════════════ */
#include "chunk_common.hpp"

#include <chrono>
#include <future>
#include <thread>

#define TICKET(...) do { \
    g_fail++; std::printf("  [BUG] "); std::printf(__VA_ARGS__); std::printf("\n"); \
} while (0)

/* 15p/5p cpu2 order1：slice0 = 8 页 = 4 窗（window0/1 同片 ⇒ 同 chg_mu）、
 * slice1 = 7 页 = 4 窗（尾窗 1 页）。与既有 125 项同款几何。 */
static constexpr uint64_t BLK_BITS = 491520;
static constexpr uint64_t INO_BITS = 163840;
static constexpr uint32_t CPU = 2;
static constexpr uint8_t ORDER = 1;
static constexpr uint64_t CHUNK_BITS = PAGE_BITS << ORDER;   /* 65536 bit */

static geom_t geom_b() { return {"inv order1 15p/5p", BLK_BITS, INO_BITS, CPU, ORDER}; }

/* 盘像：每窗首位置 1 → 全窗 MIXED（强制真实读盘；ALL_FREE 会走免读路径） */
static void img_mixed_everywhere(uint8_t *img, const std::vector<win_t> &ws) {
    for (const win_t &w : ws) img_set(img, w.lo);
}
static void root_lamp(uint8_t *img) { img_set(img, 0); }

static std::vector<win_t> windows_of(const geom_t &g, bool is_inode) {
    const uint64_t sc = derive_slice_count(g);
    const uint64_t nbits = is_inode ? g.ino_bits : g.blk_bits;
    std::vector<win_t> w;
    build_windows(nbits, derive_per_slice(nbits, sc), sc, PAGE_BITS << g.order, w);
    return w;
}

/* ═══ INV-1（§10.3/§5 时序专项）═══ */
static void inv1_fast_path_vs_inflight_io() {
    SECTION("INV-1 §10.3/§5：I/O 在途 → 快路径零阻塞 / 换片被 chg_mu 串行 / 零进 slice_lock");
    const geom_t g = geom_b();
    std::vector<win_t> wb = windows_of(g, false);
    std::vector<uint8_t> init_b(BLK_BITS / 8, 0), init_i(INO_BITS / 8, 0);
    img_mixed_everywhere(init_b.data(), wb);
    root_lamp(init_i.data());

    side_t S;
    if (!S.mount(g, init_b, init_i, true)) {
        TICKET("INV-1 装配失败 rc=%llu", (unsigned long long)S.rc);
        return;
    }
    CHECK(S.disk.reads == wb.size() + windows_of(g, true).size(),
          "INV-1 mount 读窗数 = %llu", (unsigned long long)S.disk.reads);

    /* 分配一位 → window0 上线且置脏（TTL 将对它发起态1写） */
    uint64_t left = 0;
    const uint64_t a = S.mgr->block_alloc_soft(1, &left);
    CHECK(a != PTR_INVALID && a < CHUNK_BITS, "INV-1 alloc 落在 window0（a=%llu）",
          (unsigned long long)a);
    if (a == PTR_INVALID) return;

    /* 武装门闸 → TTL 线程发起回写，把「写在途」钉死在 mock 回调里 */
    S.disk.gate_armed.store(true);
    S.disk.gate_open.store(false);
    std::thread ttl([&] { S.finish(); });
    bool inflight = false;
    for (int i = 0; i < 200 && !inflight; i++) {
        inflight = (S.disk.in_write.load() > 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(inflight, "INV-1 TTL 态1 写已钉在盘回调（in_write>0）");
    if (!inflight) {                     /* 没钉住就开门放行，别吊死线程 */
        std::lock_guard<std::mutex> lk(S.disk.gate_mu);
        S.disk.gate_open = true;
        S.disk.gate_cv.notify_all();
        ttl.join();
        S.disk.gate_armed = false;
        return;
    }

    /* 快路径：window0 在线 → block_test 纯内存，不得被 chg_mu 在途写挡住 */
    auto fut_fast = std::async(std::launch::async, [&] { return S.mgr->block_test(4242); });
    const bool fast_done =
        fut_fast.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    CHECK(fast_done, "★ §5 快路径不碰 chg_mu：写在途时 window0 的 block_test 500ms 内完成");

    /* 换片路径：window1（同片 → 同 chg_mu）MIXED 上线必须等写完成 */
    auto fut_slow = std::async(std::launch::async,
                               [&] { return S.mgr->block_test(CHUNK_BITS + 4242); });
    const bool slow_early =
        fut_slow.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready;
    CHECK(!slow_early, "★ §10.3 换片串行：写在途时同片 window1 上线被 chg_mu 挡住（300ms 未完成）");

    /* 开门 → 写完成 → 换片放行 */
    {
        std::lock_guard<std::mutex> lk(S.disk.gate_mu);
        S.disk.gate_open = true;
        S.disk.gate_cv.notify_all();
    }
    const bool slow_done =
        fut_slow.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    CHECK(slow_done, "写完成后 window1 上线完成");
    ttl.join();
    S.disk.gate_armed.store(false);
    if (fast_done) (void)fut_fast.get();
    if (slow_done) (void)fut_slow.get();

    CHECK(!S.disk.lock_violation.load(), "§10.3 全程盘 I/O 未进 slice_lock");
    CHECK(!S.disk.shape_violation.load(), "chunk_io 块形状合法");
}

/* ═══ INV-6（§10.6 计数与驻留无关）═══ */
static void inv6_counts_vs_residency() {
    SECTION("INV-6 §10.6：换片链/record/drain/兜底/TTL 全程 free_count == 影子真值");
    const geom_t g = geom_b();
    std::vector<win_t> wb = windows_of(g, false);
    std::vector<uint8_t> init_b(BLK_BITS / 8, 0), init_i(INO_BITS / 8, 0);
    img_mixed_everywhere(init_b.data(), wb);
    root_lamp(init_i.data());

    side_t S;
    if (!S.mount(g, init_b, init_i, true, /*pool_depth=*/4)) {
        TICKET("INV-6 装配失败 rc=%llu", (unsigned long long)S.rc);
        return;
    }
    std::vector<uint8_t> oracle = init_b;
    auto zt = [&] { return img_zeros(oracle.data(), oracle.size()); };
    auto ck = [&](const char *stage) -> bool {
        if (S.free_total(false) != zt()) {
            TICKET("INV-6 free_count 失配 stage=%s mgr=%llu 影子=%llu（偏差 %lld）→ "
                   "计数与驻留不同步（§10.6）", stage, (unsigned long long)S.free_total(false),
                   (unsigned long long)zt(),
                   (long long)(S.free_total(false) - zt()));
            return false;
        }
        return true;
    };
    if (!ck("mount")) return;

    /* Stage1：逐窗扫一遍 occupy → 强制全量换片链（上一窗 evict、下一窗 load） */
    for (size_t i = 0; i < wb.size(); i++) {
        const uint64_t anchor = wb[i].lo + 100, len = 50;
        const uint64_t oracle_claim = shadow_greedy(oracle.data(), anchor, len);
        uint64_t rest = 0;
        const uint64_t v = S.mgr->block_occupy_soft(anchor, len, &rest);
        const uint64_t claim = (v == PTR_INVALID) ? 0 : len - rest;
        if (claim != oracle_claim) {
            TICKET("INV-6 occupy 分歧 window=%zu claim=%llu oracle=%llu（%s）", i,
                   (unsigned long long)claim, (unsigned long long)oracle_claim,
                   claim > oracle_claim ? "虚报" : "漏报");
            return;
        }
        for (uint64_t k = 0; k < claim; k++) img_set(oracle.data(), anchor + k);
        if (!ck("sweep-occupy")) return;
    }

    /* Stage2：离线 free 前 4 窗的置位位 → 挂 record（零 I/O）+ test 立即可见 */
    const uint64_t r0 = S.disk.reads.load(), w0 = S.disk.writes.load();
    for (size_t i = 0; i < 4; i++) {
        const uint64_t bit = wb[i].lo;
        if (!img_bit(oracle.data(), bit)) continue;
        S.mgr->block_free(bit, 1);
        img_clr(oracle.data(), bit);
        if (S.mgr->block_test(bit)) {
            TICKET("INV-6 record 覆盖后 block_test 仍为 1 window=%zu bit=%llu", i,
                   (unsigned long long)bit);
            return;
        }
        if (!ck("offline-record")) return;
    }
    CHECK(S.disk.reads.load() == r0 && S.disk.writes.load() == w0,
          "★ §10.6 离线 free 挂 record 零 I/O（r+=%llu w+=%llu）",
          (unsigned long long)(S.disk.reads.load() - r0),
          (unsigned long long)(S.disk.writes.load() - w0));

    /* Stage3：再扫换片链（窗0..4）→ 各带 record 的窗上线 drain；
     * **drain 绝不重复记账** —— 每步 free_count 仍须 == 影子 */
    for (size_t i = 0; i < 5; i++) {
        const uint64_t anchor = wb[i].lo + 300, len = 20;
        const uint64_t oracle_claim = shadow_greedy(oracle.data(), anchor, len);
        uint64_t rest = 0;
        const uint64_t v = S.mgr->block_occupy_soft(anchor, len, &rest);
        const uint64_t claim = (v == PTR_INVALID) ? 0 : len - rest;
        if (claim != oracle_claim) {
            TICKET("INV-6 drain 后 occupy 分歧 window=%zu claim=%llu oracle=%llu", i,
                   (unsigned long long)claim, (unsigned long long)oracle_claim);
            return;
        }
        for (uint64_t k = 0; k < claim; k++) img_set(oracle.data(), anchor + k);
        if (!ck("post-drain-sweep")) return;
    }
    S.finish();                                    /* TTL 落盘 */
    if (!ck("post-ttl")) return;
    if (std::memcmp(S.img(false), oracle.data(), oracle.size()) != 0) {
        for (uint64_t i = 0; i < oracle.size() * 8; i++)
            if (img_bit(S.img(false), i) != img_bit(oracle.data(), i)) {
                TICKET("INV-6 TTL 后镜像≠影子 首分歧 bit=%llu", (unsigned long long)i);
                break;
            }
        return;
    }

    /* Stage4：池空兜底（pool=4）。目标窗须**离线**：stage3 后 slice0 在线=wb[3]、
     * 占窗7 后 slice1 在线=wb[7] → 取 wb[0,1,2,4,5,6] 共 6 段：
     * 前 4 段挂 record（零 I/O），第 5 段（wb[5]）与第 6 段（wb[6]）池空兜底
     * = evict 脏在线窗写 1 + 读目标窗 */
    uint64_t rest = 0;
    const uint64_t occ7 = S.mgr->block_occupy_soft(wb[7].lo + 200, 10, &rest);
    if (occ7 != wb[7].lo + 200 || rest != 0) {
        TICKET("INV-6 占窗7 失败 ret=%llu rest=%llu", (unsigned long long)occ7,
               (unsigned long long)rest);
        return;
    }
    for (uint64_t k = 0; k < 10; k++) img_set(oracle.data(), wb[7].lo + 200 + k);
    if (!ck("move-to-w7")) return;

    static const size_t TGT[6] = {0, 1, 2, 4, 5, 6};
    for (size_t j = 0; j < 6; j++) {
        const uint64_t bit = wb[TGT[j]].lo + 100;   /* stage1 在 +100..150 置过位 */
        if (!img_bit(oracle.data(), bit)) {
            TICKET("INV-6 测试构造失配：wb[%zu].lo+100 未置位", TGT[j]);
            return;
        }
        const uint64_t rr = S.disk.reads.load(), ww = S.disk.writes.load();
        S.mgr->block_free(bit, 1);
        img_clr(oracle.data(), bit);
        const uint64_t dr = S.disk.reads.load() - rr, dw = S.disk.writes.load() - ww;
        if (j < 4) {
            if (dr != 0 || dw != 0) {
                TICKET("INV-6 第%zu 段离线 free 应走 record 零 I/O（r+=%llu w+=%llu）",
                       j + 1, (unsigned long long)dr, (unsigned long long)dw);
                return;
            }
        } else {
            if (dr != 1 || dw != 1) {
                TICKET("INV-6 第%zu 段池空兜底 I/O 失配：期望 r+1 w+1，实得 r+=%llu w+=%llu",
                       j + 1, (unsigned long long)dr, (unsigned long long)dw);
                return;
            }
        }
        if (!ck("pool-exhaust-free")) return;
    }
    S.finish();
    if (!ck("final-ttl")) return;
    if (std::memcmp(S.img(false), oracle.data(), oracle.size()) != 0) {
        TICKET("INV-6 终态镜像≠影子（兜底链路丢账）");
        return;
    }
    S.audit("INV-6");
    std::printf("    [INV-6] 换片链×2 + record×9 + drain×4 + 兜底×2 + TTL：计数/镜像全等 OK\n");
}

/* ═══ INV-8（§10.8 尾块几何 × chunk_order 矩阵）═══ */
struct t8_geom {
    const char *name;
    uint64_t blk, ino;
    uint32_t cpu;
    uint8_t order;
    bool expect_tail;      /* block 组末窗是否为非整窗尾（k mod 2^order ≠ 0） */
};
static void inv8_tail_geometry() {
    SECTION("INV-8 §10.8 尾块几何：order 0/1/4/8 × 尾>0 / 尾==0（读写块数逐窗精确）");
    static const t8_geom TG[] = {
        {"order0 尾=0（页即窗）",      491520,   163840, 2, 0, false},
        {"order1 尾=1p",              491520,   163840, 2, 1, true},
        {"order1 尾=0",               524288,   131072, 2, 1, false},
        {"order4 尾=4p/1p",           655360,   557056, 1, 4, true},
        {"order4 尾=0",              1048576,   524288, 1, 4, false},
        {"order8 尾=119p/69p",      12288000, 10649600, 1, 8, true},
        {"order8 尾=0",              8388608,  8388608, 1, 8, false},
    };
    for (const t8_geom &t : TG) {
        const geom_t g{t.name, t.blk, t.ino, t.cpu, t.order};
        std::vector<win_t> wb = windows_of(g, false), wi = windows_of(g, true);
        std::vector<uint8_t> init_b(t.blk / 8, 0), init_i(t.ino / 8, 0);
        img_mixed_everywhere(init_b.data(), wb);
        root_lamp(init_i.data());

        /* §10.8 几何前置：尾窗恒 0 或 ≥1 整页（无 sub-page 尾块） */
        const win_t &last = wb.back();
        const uint64_t tail_bits = last.hi - last.lo;
        bool geom_ok = (tail_bits % PAGE_BITS == 0) && (tail_bits >= PAGE_BITS);
        CHECK(geom_ok, "%s：末窗 tail=%llu bit（恒 0 或 ≥1 整页 32768）", t.name,
              (unsigned long long)tail_bits);
        const bool is_tail = tail_bits != (PAGE_BITS << t.order);
        CHECK(is_tail == t.expect_tail,
              "%s：尾片形态 %s（期望 %s）", t.name, is_tail ? "非整窗尾" : "整窗",
              t.expect_tail ? "非整窗尾" : "整窗");

        side_t S;
        if (!S.mount(g, init_b, init_i, true)) {
            TICKET("INV-8 装配失败 geom=%s rc=%llu", t.name, (unsigned long long)S.rc);
            continue;
        }
        const uint64_t pages = (t.blk + t.ino) / PAGE_BITS;
        CHECK(S.disk.reads.load() == wb.size() + wi.size() &&
                  S.disk.read_blocks.load() == pages,
              "%s：mount 读窗=%llu（期望 %llu）读页=%llu（期望 %llu，尾窗恰整页）",
              t.name, (unsigned long long)S.disk.reads.load(),
              (unsigned long long)(wb.size() + wi.size()),
              (unsigned long long)S.disk.read_blocks.load(), (unsigned long long)pages);
        CHECK(S.disk.writes.load() == 0, "%s：mount 零写", t.name);

        /* 占尾窗（锚点 = 尾窗首 + 100，盘态该处空闲）→ 只应读尾窗整窗页数 */
        const uint64_t r1 = S.disk.reads.load(), rb1 = S.disk.read_blocks.load();
        uint64_t rest = 0;
        const uint64_t v = S.mgr->block_occupy_soft(last.lo + 100, 64, &rest);
        const uint64_t want_pages = tail_bits / PAGE_BITS;
        CHECK(v == last.lo + 100 && rest == 0, "%s：尾窗 occupy 命中", t.name);
        CHECK(S.disk.reads.load() == r1 + 1 && S.disk.read_blocks.load() == rb1 + want_pages,
              "%s：尾窗载入 1 次、恰 %llu 页（实读页+=%llu）", t.name,
              (unsigned long long)want_pages,
              (unsigned long long)(S.disk.read_blocks.load() - rb1));

        /* TTL → 恰写尾窗整窗页数（无 sub-page 写、无越界写） */
        const uint64_t wb1 = S.disk.write_blocks.load();
        S.finish();
        CHECK(S.disk.write_blocks.load() == wb1 + want_pages,
              "%s：TTL 恰写尾窗 %llu 页（实写页+=%llu）", t.name,
              (unsigned long long)want_pages,
              (unsigned long long)(S.disk.write_blocks.load() - wb1));
        S.audit(t.name);
    }
}

/* ═══ INV-9（§10.9 池空兜底 + 池节点回收复用）═══ */
static void inv9_pool_exhaust_and_reuse() {
    SECTION("INV-9 §10.9：兜底强制上线 I/O 精确计数 + 兜底后池节点复用（后续零 I/O）");
    const geom_t g = geom_b();
    std::vector<win_t> wb = windows_of(g, false);
    std::vector<uint8_t> init_b(BLK_BITS / 8, 0), init_i(INO_BITS / 8, 0);
    img_mixed_everywhere(init_b.data(), wb);
    root_lamp(init_i.data());

    side_t S;
    if (!S.mount(g, init_b, init_i, true, /*pool_depth=*/1)) {
        TICKET("INV-9 装配失败 rc=%llu", (unsigned long long)S.rc);
        return;
    }
    std::vector<uint8_t> oracle = init_b;
    auto ck = [&](const char *stage) -> bool {
        const uint64_t z = img_zeros(oracle.data(), oracle.size());
        if (S.free_total(false) != z) {
            TICKET("INV-9 free_count 失配 stage=%s mgr=%llu 影子=%llu", stage,
                   (unsigned long long)S.free_total(false), (unsigned long long)z);
            return false;
        }
        return true;
    };

    uint64_t r0 = S.disk.reads.load(), w0 = S.disk.writes.load();
    /* ① 载入窗1（occupy 置脏） */
    uint64_t rest = 0;
    const uint64_t occ = S.mgr->block_occupy_soft(wb[1].lo + 100, 100, &rest);
    if (occ != wb[1].lo + 100) { TICKET("INV-9 占窗1 失败"); return; }
    for (uint64_t k = 0; k < 100; k++) img_set(oracle.data(), wb[1].lo + 100 + k);
    CHECK(S.disk.reads.load() == r0 + 1 && S.disk.writes.load() == w0,
          "① occupy 载入窗1 一次读、零写（r+=%llu w+=%llu）",
          (unsigned long long)(S.disk.reads.load() - r0),
          (unsigned long long)(S.disk.writes.load() - w0));
    if (!ck("load-w1")) return;

    /* ② 离线 free 窗0 首位 → 用掉池内唯一节点，零 I/O */
    r0 = S.disk.reads.load(); w0 = S.disk.writes.load();
    S.mgr->block_free(wb[0].lo, 1);
    img_clr(oracle.data(), wb[0].lo);
    CHECK(S.disk.reads.load() == r0 && S.disk.writes.load() == w0,
          "② 离线 free 挂 record 零 I/O（池深 1，节点已占）");
    if (!ck("record-w0")) return;

    /* ③ 离线 free 窗2 → 池空兜底：强制上线（先 evict 脏窗1 + 读窗2）→ 直清 */
    r0 = S.disk.reads.load(); w0 = S.disk.writes.load();
    S.mgr->block_free(wb[2].lo, 1);
    img_clr(oracle.data(), wb[2].lo);
    CHECK(S.disk.reads.load() == r0 + 1 && S.disk.writes.load() == w0 + 1,
          "③ 池空兜底 = evict 脏窗1 写 1 + 读窗2（r+=%llu w+=%llu）",
          (unsigned long long)(S.disk.reads.load() - r0),
          (unsigned long long)(S.disk.writes.load() - w0));
    if (!ck("fallback-w2")) return;
    CHECK(S.mgr->block_test(wb[2].lo) == false, "③ 兜底直清后 block_test==false（窗2 在线）");

    /* ④ TTL：态2 回灌窗0（读+写）+ 态1 写在线脏窗2 → record 消费 ⇒ 池节点归还 */
    r0 = S.disk.reads.load(); w0 = S.disk.writes.load();
    S.finish();
    CHECK(S.disk.reads.load() == r0 + 1 && S.disk.writes.load() == w0 + 2,
          "④ TTL = 态2 读1写1 + 态1 写1（r+=%llu w+=%llu）",
          (unsigned long long)(S.disk.reads.load() - r0),
          (unsigned long long)(S.disk.writes.load() - w0));
    if (!ck("ttl-flush")) return;

    /* ⑤ ★池复用：record 已被 TTL 消费归还 → 离线 free 窗3 应再走 record，零 I/O
     *（若节点没回收，这里会退化成兜底强制上线 = 多出读写 → 断言抓出） */
    r0 = S.disk.reads.load(); w0 = S.disk.writes.load();
    S.mgr->block_free(wb[3].lo, 1);
    img_clr(oracle.data(), wb[3].lo);
    CHECK(S.disk.reads.load() == r0 && S.disk.writes.load() == w0,
          "★ ⑤ 池节点复用：后续离线 free 仍零 I/O（r+=%llu w+=%llu）",
          (unsigned long long)(S.disk.reads.load() - r0),
          (unsigned long long)(S.disk.writes.load() - w0));
    if (!ck("reuse-w3")) return;

    /* ⑥ 终态 TTL → 镜像全等 */
    S.finish();
    if (!ck("final")) return;
    if (std::memcmp(S.img(false), oracle.data(), oracle.size()) != 0) {
        TICKET("INV-9 终态镜像≠影子（兜底链路丢账）");
        return;
    }
    S.audit("INV-9");
    std::printf("    [INV-9] 兜底 I/O 精确计数 + 池节点复用 OK\n");
}

/* ═══ INV-5b（§7 摘要 ALL_FREE 免载入）═══ */
static void inv5b_all_free_skip() {
    SECTION("INV-5b §7：全空闲位图 → 摘要 ALL_FREE，test/alloc 免载入零读盘");
    const geom_t g = geom_b();
    std::vector<uint8_t> init_b(BLK_BITS / 8, 0), init_i(INO_BITS / 8, 0);
    root_lamp(init_i.data());                      /* block 组全 0 → 每窗 ALL_FREE */

    side_t S;
    if (!S.mount(g, init_b, init_i, true)) {
        TICKET("INV-5b 装配失败 rc=%llu", (unsigned long long)S.rc);
        return;
    }
    const uint64_t r0 = S.disk.reads.load();
    CHECK(S.free_total(false) == BLK_BITS, "mount free_total = %llu（全空闲）",
          (unsigned long long)S.free_total(false));

    CHECK(S.mgr->block_test(123456) == false, "ALL_FREE 窗 block_test==false");
    CHECK(S.disk.reads.load() == r0, "★ 摘要 ALL_FREE 裁决零读盘（r+=%llu）",
          (unsigned long long)(S.disk.reads.load() - r0));

    uint64_t left = 0;
    const uint64_t a = S.mgr->block_alloc_soft(10, &left);
    CHECK(a != PTR_INVALID && left == 0, "ALL_FREE 窗分配命中（a=%llu）", (unsigned long long)a);
    CHECK(S.disk.reads.load() == r0,
          "★ ALL_FREE 整窗免载入直置空窗：分配零读盘（r+=%llu）",
          (unsigned long long)(S.disk.reads.load() - r0));
    if (a == PTR_INVALID) return;

    S.mgr->block_free(a, 10);                      /* 在线直清 */
    S.finish();
    uint64_t z = 0;
    for (uint64_t i = 0; i < BLK_BITS; i++) z += !img_bit(S.img(false), i);
    CHECK(S.free_total(false) == z, "TTL 后 free_total == 镜像空闲数（%llu）",
          (unsigned long long)z);
    S.audit("INV-5b");
}

int main() {
    std::printf("== §10 不变量缺口专项（Phase A-3）==\n");
    inv1_fast_path_vs_inflight_io();
    inv6_counts_vs_residency();
    inv8_tail_geometry();
    inv9_pool_exhaust_and_reuse();
    inv5b_all_free_skip();
    std::printf("%d passed, %d failed\n", g_pass.load(), g_fail.load());
    if (g_fail.load() == 0) std::printf("RESULT: ALL PASS\n");
    else std::printf("RESULT: FAIL（复现单见 [BUG] 行）\n");
    return g_fail.load() ? 1 : 0;
}

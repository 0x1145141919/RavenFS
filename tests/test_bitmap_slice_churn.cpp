/* ═══════════════════════════════════════════════════════════════════
 * test_bitmap_slice_churn.cpp — 并发多线程 churn 长跑 + 影子对账（Phase A-2）
 *
 * 目标（任务书 A-2 / 03 T11 模式）：4 线程 alloc/free/occupy/test **真混合
 * 并发**压 CHUNK 后端（换片 / record 池 / 池空兜底 / TTL 并行），**每步与
 * 影子位图对账**（可对账的最强子集），每轮收口做 T11 式端到端校验
 * （TTL 后 整盘镜像 == 影子 + free_count == 影子），≥3 轮长随机，零分歧。
 *
 * ── 并发下影子记账的可靠性协议（本测试的设计核心）──────────────────
 * 影子 ⊆ 管理器（恒真不变式）：影子置 1 ⇒ 管理器必已 1。三条规则保持它：
 *   R1 free   ：全局锁内先把影子 1→0（**只清影子为 1 的子段**），解锁后再
 *               mgr.free —— 顺序保证 in-flight alloc 抢不走未清账的位；
 *               「影子 0 但管理器 1」的 in-flight 位跳过不清（否则两态序
 *               不可复原）。副作用：churn 里的 free 全是真 1→0 ⇒ 每步计数
 *               增量精确（KB2 类虚高只在顺序对拍里登记，不污染并发对账）。
 *   R2 occupy ：锁内算 oracle 贪心前缀 → 解锁 mgr.occupy → 锁内比对：
 *               claim>oracle = 虚报（KB1 同族，登记）；claim<oracle ⇒
 *               要求停止位在管理器中为 1（并发 alloc 落位是合法解释，
 *               位仍空 = 漏报/泄漏，登记）；影子按 min(claim,oracle) 记账。
 *   R3 alloc  ：先 mgr.alloc，回锁后断言返回位影子必为 0（**双分配探测**），
 *               置影子。影子 1 ⇒ 管理器 1 保证返回位影子必为 0。
 *   R4 test   ：只断言「影子 1 ⇒ 管理器 1」（丢置位 = 双分配方向，必抓）；
 *               影子 0 不断言（可能有 in-flight alloc 未记账）。
 * 每轮屏障：TTL 收尾（另有 TTL 线程与负载并行跑 §9）→ 镜像 memcmp + 计数
 * 全等 → 不一致即端到端 [BUG] 并终止本轮次后续（状态已不可信）。
 *
 * 已知 bug 容忍（只登记复现单）：KB1（occupy 摘要 ALL_USED 虚报，见
 * differential 头注）发生时按 R2 记账，屏障照常复核状态。
 *
 * 2026-09-28 · Docs/runtime_tech/03_chunk_backed_slices.md §10
 * ═══════════════════════════════════════════════════════════════════ */
#include "chunk_common.hpp"

#include <string>
#include <thread>

#define TICKET(...) do { \
    g_fail++; std::printf("  [BUG] "); std::printf(__VA_ARGS__); std::printf("\n"); \
} while (0)

static const geom_t GEOMS[] = {
    {"G-B order1 cpu2 15p/5p",   491520,   163840,  2, 1},   /* 多片 + record/兜底主战场 */
    {"G-E order0 tiny 1p/1p",     32768,    32768,  4, 0},   /* 单片单窗：全负载挤 chg_mu */
    {"G-F order4 cpu2 40p/34p", 1310720,  1114112,  2, 4},   /* 大 chunk + 跨片 */
};
static const int NTHREADS = 4, ROUNDS = 3, NOPS = 1200;

/* 每类分歧只打一条完整复现单，其余计数（防日志雪崩 + 保留总量口径） */
struct diver_t {
    int over = 0, under = 0, dbl = 0, lost = 0;
    bool ticked_over = false, ticked_under = false, ticked_dbl = false, ticked_lost = false;
};

struct churn_run_t {
    side_t *S = nullptr;
    const geom_t *g = nullptr;
    uint64_t seed = 0;
    std::vector<uint8_t> sb;         /* block 影子 */
    std::vector<uint8_t> si;         /* inode 影子 */
    uint64_t zb = 0, zi = 0;
    std::mutex M;                    /* 影子锁（R1/R2/R3 记账段） */
    std::atomic<bool> bad{false};    /* 硬分歧 → 全员收手 */
    std::atomic<uint64_t> n_alloc{0}, n_alloc_fail{0}, n_occ{0}, n_free{0}, n_test{0};
    diver_t dv;
    std::mutex tk_mu;
    bool with_occupy = true;   /* no-occ 配置：occupy 因 BUG#3（见 differential 头注）不进
                                * 并发压测时，用本开关跑免该 bug 的 alloc/free/test 长跑 */

    /* 分歧登记：cls ∈ {over, under, dbl, lost} */
    void hit(int cls, const char *fmt, auto &&...args) {
        std::lock_guard<std::mutex> lk(tk_mu);
        bool *ticked = nullptr;
        int *cnt = nullptr;
        switch (cls) {
            case 0: cnt = &dv.over;  ticked = &dv.ticked_over;  break;
            case 1: cnt = &dv.under; ticked = &dv.ticked_under; break;
            case 2: cnt = &dv.dbl;   ticked = &dv.ticked_dbl;   break;
            default: cnt = &dv.lost; ticked = &dv.ticked_lost;  break;
        }
        (*cnt)++;
        if (!*ticked) {
            *ticked = true;
            g_fail++;
            std::printf("  [BUG] ");
            std::printf(fmt, args...);
            std::printf("\n");
        }
    }
    void hard_fail(const char *fmt, auto &&...args) {
        std::lock_guard<std::mutex> lk(tk_mu);
        g_fail++;
        std::printf("  [BUG] ");
        std::printf(fmt, args...);
        std::printf("\n");
        bad = true;
    }
};

static void churn_worker(churn_run_t &C, int t, int round) {
    t_fake_proc_id = (uint32_t)(t % C.S->mgr->cpu_slots());
    const uint64_t nbits = C.g->blk_bits;
    const uint64_t region = nbits / NTHREADS;
    const uint64_t lo = (uint64_t)t * region;
    const uint64_t hi = (t == NTHREADS - 1) ? nbits : lo + region;
    rng_t r(C.seed ^ (0x51ED2701ull * (uint64_t)(round + 1)) ^
            (0x9E3779B9ull * (uint64_t)(t + 1)));
    bitmap_slices_mgr_t &m = *C.S->mgr;

    for (int i = 0; i < NOPS && !C.bad; i++) {
        const int k = (int)r.below(100);

        /* ── occupy（R2；no-occ 配置跳过 → 落入 free 段） ── */
        if (C.with_occupy && k < 28) {
            uint64_t base = lo + r.below(hi - lo);
            uint64_t len = 1 + r.below(128);
            if (base + len > hi) len = hi - base;
            if (len == 0) continue;
            uint64_t oracle;
            {
                std::lock_guard<std::mutex> lk(C.M);
                oracle = shadow_greedy(C.sb.data(), base, len);
            }
            uint64_t rest = 0;
            const uint64_t v = m.block_occupy_soft(base, len, &rest);
            const uint64_t claim = (v == PTR_INVALID) ? 0 : len - rest;
            std::lock_guard<std::mutex> lk(C.M);
            if (claim > oracle) {
                C.hit(0, "KB1类 occupy 虚报（churn）geom=%s seed=%llx round=%d "
                         "base=%llu len=%llu oracle=%llu claim=%llu ret=%s rest=%llu "
                         "→ 状态由轮末镜像/计数复核", C.g->name,
                      (unsigned long long)C.seed, round, (unsigned long long)base,
                      (unsigned long long)len, (unsigned long long)oracle,
                      (unsigned long long)claim, v == PTR_INVALID ? "INVALID" : "blkbase",
                      (unsigned long long)rest);
            } else if (claim < oracle) {
                const bool stopbit = m.block_test(base + claim);
                if (!stopbit) {
                    C.hit(1, "occupy 漏报（停止位仍空闲＝泄漏方向）geom=%s seed=%llx "
                             "round=%d base=%llu len=%llu oracle=%llu claim=%llu",
                          C.g->name, (unsigned long long)C.seed, round,
                          (unsigned long long)base, (unsigned long long)len,
                          (unsigned long long)oracle, (unsigned long long)claim);
                }
            }
            const uint64_t applied = claim < oracle ? claim : oracle;
            for (uint64_t x = 0; x < applied; x++) img_set(C.sb.data(), base + x);
            C.zb -= applied;
            C.n_occ++;
            continue;
        }

        /* ── free（R1：锁内清影子子段 → 解锁后 mgr.free） ── */
        if (k < 52) {
            uint64_t base = lo + r.below(hi - lo);
            uint64_t len = 1 + r.below(128);
            if (base + len > hi) len = hi - base;
            struct seg { uint64_t p, q; };
            std::vector<seg> segs;
            {
                std::lock_guard<std::mutex> lk(C.M);
                uint64_t p = base;
                const uint64_t end = base + len;
                while (p < end) {
                    if (!img_bit(C.sb.data(), p)) { p++; continue; }
                    uint64_t q = p;
                    while (q < end && img_bit(C.sb.data(), q)) q++;
                    for (uint64_t x = p; x < q; x++) img_clr(C.sb.data(), x);
                    segs.push_back({p, q});
                    C.zb += (q - p);
                    p = q;
                }
            }
            for (const seg &s : segs) m.block_free(s.p, s.q - s.p);
            C.n_free++;
            continue;
        }

        /* ── test（R4：只断言影子1 ⇒ 管理器1） ── */
        if (k < 66) {
            const uint64_t bit = lo + r.below(hi - lo);
            bool s;
            {
                std::lock_guard<std::mutex> lk(C.M);
                s = img_bit(C.sb.data(), bit);
            }
            const bool v = m.block_test(bit);
            if (s && !v) {
                C.hit(3, "★ 影子置位但管理器已空（丢置位=双分配方向）geom=%s seed=%llx "
                         "round=%d bit=%llu", C.g->name, (unsigned long long)C.seed, round,
                      (unsigned long long)bit);
                C.bad = true;
                return;
            }
            C.n_test++;
            continue;
        }

        /* ── alloc（R3：mgr 先行，回锁断言影子必 0） ── */
        if (k < 88) {
            uint64_t v = PTR_INVALID;
            uint64_t got = 0;
            if (r.below(2) == 0) {
                const uint64_t n = 1 + r.below(32);
                uint64_t left = n;
                v = m.block_alloc_soft(n, &left);
                if (v != PTR_INVALID) got = n - left;
            } else {
                const uint64_t n = 1 + r.below(32);
                static const uint8_t als[4] = {0, 1, 2, 3};
                v = m.block_alloc_hard(n, als[r.below(4)]);
                if (v != PTR_INVALID) got = n;
            }
            if (v == PTR_INVALID) { C.n_alloc_fail++; continue; }
            {
                std::lock_guard<std::mutex> lk(C.M);
                for (uint64_t x = 0; x < got; x++) {
                    if (img_bit(C.sb.data(), v + x)) {
                        C.hit(2, "★ 双分配：alloc 返回已置位区间 geom=%s seed=%llx round=%d "
                                 "base=%llu k=%llu", C.g->name, (unsigned long long)C.seed,
                              round, (unsigned long long)v, (unsigned long long)x);
                        C.bad = true;
                        return;
                    }
                }
                for (uint64_t x = 0; x < got; x++) img_set(C.sb.data(), v + x);
                C.zb -= got;
            }
            C.n_alloc++;
            continue;
        }

        /* ── inode 组同协议（alloc 先行 / free 只清影子1 / test 单向断言） ── */
        if (k < 94) {
            if (r.below(2) == 0) {
                const uint64_t ino = m.inode_alloc();
                if (ino != PTR_INVALID) {
                    std::lock_guard<std::mutex> lk(C.M);
                    if (img_bit(C.si.data(), ino)) {
                        C.hit(2, "★ inode 双分配 geom=%s seed=%llx round=%d inum=%llu",
                              C.g->name, (unsigned long long)C.seed, round,
                              (unsigned long long)ino);
                        C.bad = true;
                        return;
                    }
                    img_set(C.si.data(), ino);
                    C.zi--;
                }
            } else {
                const uint64_t ino = r.below(C.g->ino_bits);
                bool s;
                {
                    std::lock_guard<std::mutex> lk(C.M);
                    s = img_bit(C.si.data(), ino);
                    if (s) { img_clr(C.si.data(), ino); C.zi++; }
                }
                if (s) m.inode_free(ino);   /* 影子0（in-flight/已空闲）→ 不 free（R1 同理） */
            }
            continue;
        }
        {  /* inode_test 单向 */
            const uint64_t ino = r.below(C.g->ino_bits);
            bool s;
            {
                std::lock_guard<std::mutex> lk(C.M);
                s = img_bit(C.si.data(), ino);
            }
            if (s && !m.inode_test(ino)) {
                C.hit(3, "★ inode 影子置位但管理器空 geom=%s seed=%llx round=%d inum=%llu",
                      C.g->name, (unsigned long long)C.seed, round, (unsigned long long)ino);
                C.bad = true;
                return;
            }
        }
    }
}

/* 轮末 T11 收口：TTL 线程退场 → 镜像 memcmp + 计数全等 */
static bool churn_reconcile(churn_run_t &C, int round) {
    C.S->finish();                                    /* 收尾 TTL（与负载并行的 TTL 已退场） */
    C.S->audit("churn");
    bool ok = true;
    const uint64_t ft = C.S->free_total(false);
    if (ft != C.zb) {
        TICKET("churn 轮末 block_free_total 失配 geom=%s seed=%llx round=%d mode=%s "
               "mgr=%llu 影子=%llu（偏差 %lld）", C.g->name, (unsigned long long)C.seed,
               round, C.S->chunked ? "CHUNK" : "FULL", (unsigned long long)ft,
               (unsigned long long)C.zb, (long long)(ft - C.zb));
        ok = false;
    }
    const uint64_t it = C.S->free_total(true);
    if (it != C.zi) {
        TICKET("churn 轮末 inode_free_total 失配 geom=%s seed=%llx round=%d mgr=%llu 影子=%llu",
               C.g->name, (unsigned long long)C.seed, round, (unsigned long long)it,
               (unsigned long long)C.zi);
        ok = false;
    }
    for (int grp = 0; grp < 2; grp++) {
        const bool is_ino = grp == 1;
        uint8_t *img = C.S->img(is_ino);
        uint8_t *sh = is_ino ? C.si.data() : C.sb.data();
        const uint64_t bytes = C.S->bytes(is_ino);
        if (std::memcmp(img, sh, bytes) != 0) {
            for (uint64_t i = 0; i < bytes * 8; i++)
                if (img_bit(img, i) != img_bit(sh, i)) {
                    TICKET("★ churn 轮末镜像≠影子 geom=%s seed=%llx round=%d grp=%s "
                           "首分歧 bit=%llu mgr=%d shadow=%d", C.g->name,
                          (unsigned long long)C.seed, round, is_ino ? "inode" : "block",
                          (unsigned long long)i, (int)img_bit(img, i), (int)img_bit(sh, i));
                    break;
                }
            ok = false;
        }
    }
    return ok;
}

static void churn_mode(const geom_t &g, uint64_t seed, bool want_chunk, bool with_occupy) {
    std::vector<uint8_t> init_b, init_i;
    gen_init(g, seed, init_b, init_i);

    churn_run_t C;
    C.g = &g;
    C.seed = seed;
    C.with_occupy = with_occupy;
    C.sb = init_b;
    C.si = init_i;
    C.zb = img_zeros(C.sb.data(), C.sb.size());
    C.zi = img_zeros(C.si.data(), C.si.size());

    side_t S;
    /* CHUNK 用小池（8）逼并发池空兜底；FULL 同参（池字段被忽略） */
    if (!S.mount(g, init_b, init_i, want_chunk, /*pool_depth=*/8)) {
        TICKET("churn 装配失败 geom=%s mode=%s rc=%llu", g.name,
               want_chunk ? "CHUNK" : "FULL", (unsigned long long)S.rc);
        return;
    }
    C.S = &S;

    bool all_ok = true;
    for (int round = 0; round < ROUNDS && !C.bad && all_ok; round++) {
        /* TTL 并行线程（§9 与分配并发；只在负载期间跑） */
        std::atomic<bool> stop_ttl{false};
        std::thread ttl_th;
        if (S.chunked) {
            ttl_th = std::thread([&] {
                while (!stop_ttl.load(std::memory_order_acquire)) {
                    S.finish();
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            });
        }
        std::vector<std::thread> th;
        for (int t = 0; t < NTHREADS; t++) th.emplace_back(churn_worker, std::ref(C), t, round);
        for (auto &x : th) x.join();
        stop_ttl = true;
        if (ttl_th.joinable()) ttl_th.join();
        if (C.bad) { all_ok = false; break; }
        all_ok = churn_reconcile(C, round);
    }

    std::printf("    [churn] %-24s mode=%-5s cfg=%-6s rounds=%d ops=%llu(alloc %llu/fail %llu "
                "occ %llu free %llu test %llu) divergences(over/under/dbl/lost)=%d/%d/%d/%d %s\n",
                g.name, want_chunk ? "CHUNK" : "FULL", with_occupy ? "mix" : "no-occ", ROUNDS,
                (unsigned long long)(C.n_alloc.load() + C.n_alloc_fail.load() +
                                     C.n_occ.load() + C.n_free.load() + C.n_test.load()),
                (unsigned long long)C.n_alloc.load(), (unsigned long long)C.n_alloc_fail.load(),
                (unsigned long long)C.n_occ.load(), (unsigned long long)C.n_free.load(),
                (unsigned long long)C.n_test.load(), C.dv.over, C.dv.under, C.dv.dbl,
                C.dv.lost, all_ok ? "OK" : "FAIL");
}

int main() {
    std::printf("== 并发 churn 影子对账（Phase A-2，%d 线程 × %d 轮 × %d op）==\n",
                NTHREADS, ROUNDS, NOPS);
    SECTION("每步对账（R1–R4 可靠性协议）+ 每轮 TTL 收口（镜像 memcmp + free_count 全等）");
    for (const geom_t &g : GEOMS) {
        SECTION((std::string(g.name) + " · 全混合（occupy 参与，BUG#3 会被登记）").c_str());
        churn_mode(g, 0x7A11C0DEull, /*want_chunk=*/false, /*with_occupy=*/true);  /* FULL 控制组 */
        churn_mode(g, 0x7A11C0DEull, /*want_chunk=*/true,  /*with_occupy=*/true);  /* CHUNK 被测组 */
        SECTION((std::string(g.name) + " · no-occ（alloc/free/test，验 3 轮零分歧收口）").c_str());
        churn_mode(g, 0x7A11C0DEull, /*want_chunk=*/false, /*with_occupy=*/false);
        churn_mode(g, 0x7A11C0DEull, /*want_chunk=*/true,  /*with_occupy=*/false);
    }
    std::printf("%d passed, %d failed\n", g_pass.load(), g_fail.load());
    if (g_fail.load() == 0) std::printf("RESULT: ALL PASS\n");
    else std::printf("RESULT: FAIL（分歧清单见 [BUG] 行）\n");
    return g_fail.load() ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * tests/test_mdata_piece_cache_smoke.cpp
 *
 * 黑盒 / 功能冒烟测试：8K 元数据负载片缓存（mdata_piece_caches_mgr）
 * 覆盖 include/ravenfs/mdata_piece_caches.hpp 的 self_reliat 双档
 * （CTRL_ARRAY bits<=13 / RADIX bits>13），重点覆盖近期新增：
 *   · 双档强淘汰 self_reliat_reap_expired_strong（PRESSURE / LOWLOAD）
 *   · 新增统计钩子（mdp_stat_hook）埋点契约 + 未注入 no-op
 *
 * 本文件仅测试，不修改任何生产源码。链接预构建静态库。
 *
 * 构建：
 *   g++ -I include -std=gnu++20 -pthread -O2 -g \
 *       tests/test_mdata_piece_cache_smoke.cpp tests/cpu_id_stub.cpp \
 *       build_v20/juvenile_god/libravenfs_juvenile.a -o /tmp/opencode/test_mdata_piece_cache_smoke
 * ═══════════════════════════════════════════════════════════════════ */
#include "ravenfs/mdata_piece_caches.hpp"
#include "ravenfs/mdata_piece_caches_stats.hpp"
#include "util/lock.h"   /* rdtsc_timestamp */
#include "util/tsc.h"    /* tsc_probe */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

/* ── 断言/用例框架 ─────────────────────────────────────────────────── */
static int g_pass = 0;
static int g_fail = 0;
static int __case_fail0 = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) { g_pass++; }                                                 \
        else { g_fail++; std::printf("  [FAIL] line %d: %s\n", __LINE__, (msg)); } \
    } while (0)

#define BEGIN_CASE(id, name)                                                    \
    do {                                                                        \
        std::printf("\n[CASE %s] %s\n", (id), (name));                          \
        __case_fail0 = g_fail;                                                  \
    } while (0)

#define END_CASE(id)                                                            \
    do {                                                                        \
        std::printf("  -> CASE %s: %s\n", (id),                                 \
                    (g_fail == __case_fail0) ? "PASS" : "FAIL");                \
    } while (0)

/* ── 内存后备镜像 + I/O 回调 ────────────────────────────────────────
 * read_block/write_block 收到的是「FS_block 号 + 块数」，每块 4096B；
 * 负载恒 2 块（blk_len == 2 → 8192B）。镜像按块线性排布。 */
struct MemImage {
    std::vector<uint8_t> bytes;
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> bad_len{0};   /* 记录任何 blk_len != 2 的调用（应为 0） */

    explicit MemImage(uint64_t nblocks, uint8_t seed = 0) {
        bytes.resize((size_t)nblocks * 4096u);
        for (size_t i = 0; i < bytes.size(); i++)
            bytes[i] = (uint8_t)(seed + (uint8_t)((i * 131u) & 0xffu));
    }
};

static result_code_t read_cb(void *io, uint64_t blk, uint64_t blk_len, void *buf) {
    MemImage *m = static_cast<MemImage *>(io);
    m->reads.fetch_add(1, std::memory_order_relaxed);
    if (blk_len != 2) m->bad_len.fetch_add(1, std::memory_order_relaxed);
    uint64_t off = blk * 4096ull;
    uint64_t n = blk_len * 4096ull;
    if (off + n > m->bytes.size()) return RC_ERR_IO;
    std::memcpy(buf, m->bytes.data() + off, (size_t)n);
    return RC_OK;
}

static result_code_t write_cb(void *io, uint64_t blk, uint64_t blk_len, const void *buf) {
    MemImage *m = static_cast<MemImage *>(io);
    m->writes.fetch_add(1, std::memory_order_relaxed);
    if (blk_len != 2) m->bad_len.fetch_add(1, std::memory_order_relaxed);
    uint64_t off = blk * 4096ull;
    uint64_t n = blk_len * 4096ull;
    if (off + n > m->bytes.size()) return RC_ERR_IO;
    std::memcpy(m->bytes.data() + off, buf, (size_t)n);
    return RC_OK;
}

/* ── 统计钩子采集器（(field,delta) 累加） ─────────────────────────── */
struct StatCollector {
    std::atomic<uint64_t> cnt[MDP_STAT_COUNT];
    StatCollector() { reset(); }
    void reset() {
        for (int i = 0; i < MDP_STAT_COUNT; i++)
            cnt[i].store(0, std::memory_order_relaxed);
    }
    uint64_t get(mdp_stat f) const { return cnt[f].load(std::memory_order_relaxed); }
};

static void stat_inc(void *ctx, uint32_t field, uint64_t delta) {
    static_cast<StatCollector *>(ctx)->cnt[field].fetch_add(delta, std::memory_order_relaxed);
}

/* ── 配置构造 ─────────────────────────────────────────────────────── */
static mdata_piece_caches_cfg_t make_cfg(MemImage *img, uint64_t count, uint64_t base,
                                         bool with_stat, StatCollector *sc) {
    mdata_piece_caches_cfg_t cfg{};
    cfg.mode = cache_work_mode_t::self_reliat_cache;
    cfg.read_block = read_cb;
    cfg.write_block = write_cb;
    cfg.io_ctx = img;
    cfg.Piece_count = count;
    cfg.Piece_base_FS_block_idx = base;
    cfg.self_reliat_specific.ttl_bare_seconds = 0;   /* 测试自行 set_ttl_ticks */
    if (with_stat && sc) {
        cfg.stat.inc = stat_inc;
        cfg.stat.ctx = sc;
    } else {
        cfg.stat.inc = nullptr;
        cfg.stat.ctx = nullptr;
    }
    return cfg;
}

/* ── TSC ──────────────────────────────────────────────────────────── */
static uint64_t g_tsc_hz = 0;

static void spin_ticks(uint64_t ticks) {
    uint64_t s = rdtsc_timestamp();
    while (rdtsc_timestamp() - s < ticks) {
        __asm__ __volatile__("pause");
    }
}

/* 测试用负载序数（<=896，落盘块号小，镜像可保持紧凑）。RADIX(bits=14) 下
 * 128 步长映射到不同分片（slice = idx>>7）。 */
static const uint64_t kPieces[8] = {0, 128, 256, 384, 512, 640, 768, 896};

static inline uint64_t blk_of(uint64_t base, int i) { return base + kPieces[i] * 2; }

/* ═══════════════════════════════════════════════════════════════════
 * 单个模式的完整用例套件
 * ═══════════════════════════════════════════════════════════════════ */
static void run_suite(const char *label, uint64_t count, mdata_piece_mode_t expect_mode) {
    const char *mode_s = (expect_mode == MDP_MODE_RADIX_SHARD) ? "RADIX" : "CTRL_ARRAY";
    std::printf("\n================ SUITE %s (Piece_count=%llu, expect=%s) ================\n",
                label, (unsigned long long)count, mode_s);

    const uint64_t base = 0;
    const uint64_t img_blocks = kPieces[7] * 2 + 8;

    /* ── CASE 1：构造 + is_ready + 装配统计 ────────────────────────── */
    {
        BEGIN_CASE("1", "ctor success + is_ready + init stats");
        MemImage img(img_blocks);
        StatCollector sc;
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, base, true, &sc);
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        CHECK(rc == RC_OK, "ctor rc == RC_OK");
        CHECK(mgr.is_ready(), "is_ready() true");
        CHECK(mgr.self_reliat_mode() == expect_mode, "self_reliat_mode matches bits档");
        CHECK(mgr.piece_count() == count, "piece_count matches");
        CHECK(sc.get(MDP_INIT_OK) == 1, "MDP_INIT_OK == 1");
        if (expect_mode == MDP_MODE_RADIX_SHARD) {
            CHECK(sc.get(MDP_INIT_RADIX) == 1, "MDP_INIT_RADIX == 1");
            CHECK(sc.get(MDP_SLICES) >= 1, "MDP_SLICES >= 1");
            CHECK(sc.get(MDP_TABLE_ALLOC_LEAF) > 0, "RADIX preallocs leaf tables");
        } else {
            CHECK(sc.get(MDP_INIT_CTRL_ARRAY) == 1, "MDP_INIT_CTRL_ARRAY == 1");
            CHECK(sc.get(MDP_INIT_RADIX) == 0, "MDP_INIT_RADIX == 0");
        }
        std::printf("  observed: INIT_OK=%llu INIT_RADIX=%llu INIT_CTRL=%llu SLICES=%llu\n",
                    (unsigned long long)sc.get(MDP_INIT_OK),
                    (unsigned long long)sc.get(MDP_INIT_RADIX),
                    (unsigned long long)sc.get(MDP_INIT_CTRL_ARRAY),
                    (unsigned long long)sc.get(MDP_SLICES));
        END_CASE("1");
    }

    /* ── CASE 2：get miss→read once；get hit→no read；put ──────────── */
    {
        BEGIN_CASE("2", "get miss/hit + put counters");
        MemImage img(img_blocks);
        StatCollector sc;
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, base, true, &sc);
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        CHECK(rc == RC_OK, "ctor ok");
        sc.reset();
        uint64_t blk = blk_of(base, 1);

        uint64_t r0 = img.reads.load();
        uint8_t *p = mgr.self_reliat_get(blk);
        CHECK(p != nullptr, "get returns page");
        CHECK(img.reads.load() == r0 + 1, "miss = exactly 1 read_block");
        CHECK(img.bad_len.load() == 0, "read blk_len always 2");
        CHECK(sc.get(MDP_GET_MISS) == 1, "MDP_GET_MISS == 1");
        CHECK(sc.get(MDP_NODE_LOAD) == 1, "MDP_NODE_LOAD == 1");
        CHECK(sc.get(MDP_RESIDENT_ADD_BYTES) == 8192, "MDP_RESIDENT_ADD_BYTES == 8192");
        CHECK(std::memcmp(p, img.bytes.data() + blk * 4096, 8192) == 0, "loaded data == backing");

        mgr.self_reliat_put(blk);
        CHECK(sc.get(MDP_PUT) == 1, "MDP_PUT == 1");

        uint64_t r1 = img.reads.load();
        uint8_t *p2 = mgr.self_reliat_get(blk);
        CHECK(p2 == p, "hit returns same page pointer");
        CHECK(img.reads.load() == r1, "hit causes no extra read_block");
        CHECK(sc.get(MDP_GET_HIT) == 1, "MDP_GET_HIT == 1");
        mgr.self_reliat_put(blk);
        std::printf("  observed: GET_MISS=%llu GET_HIT=%llu NODE_LOAD=%llu PUT=%llu reads=%llu\n",
                    (unsigned long long)sc.get(MDP_GET_MISS),
                    (unsigned long long)sc.get(MDP_GET_HIT),
                    (unsigned long long)sc.get(MDP_NODE_LOAD),
                    (unsigned long long)sc.get(MDP_PUT),
                    (unsigned long long)img.reads.load());
        END_CASE("2");
    }

    /* ── CASE 3：mark_dirty + flush_all（脏落盘 / 非脏丢弃） ───────── */
    {
        BEGIN_CASE("3", "mark_dirty then flush_all persists dirty, drops clean");
        MemImage img(img_blocks);
        StatCollector sc;
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, base, true, &sc);
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        uint64_t blkA = blk_of(base, 0);
        uint64_t blkB = blk_of(base, 2);

        uint8_t patA[8192];
        for (int i = 0; i < 8192; i++)
            patA[i] = (uint8_t)(0xA5 ^ ((i * 3) & 0xff) ^ (int)(blkA & 0xff));

        {
            mdata_piece_session s(&mgr, blkA);
            CHECK(s.data != nullptr && !s.index_error(), "session A loaded");
            std::memcpy(s.data, patA, 8192);
            s.dirty();
        }
        CHECK(sc.get(MDP_DIRTY) >= 1, "MDP_DIRTY >= 1");
        { mdata_piece_session s2(&mgr, blkB); (void)s2; }

        sc.reset();
        uint64_t w0 = img.writes.load();
        result_code_t frc = mgr.flush_all();
        CHECK(frc == RC_OK, "flush_all RC_OK");
        CHECK(img.writes.load() == w0 + 1, "exactly 1 write_block (the dirty row)");
        CHECK(std::memcmp(img.bytes.data() + blkA * 4096, patA, 8192) == 0,
              "dirty bytes persisted to backing image");
        CHECK(sc.get(MDP_FLUSH_WB) == 1, "MDP_FLUSH_WB == 1");
        CHECK(sc.get(MDP_FLUSH_DROP) == 1, "MDP_FLUSH_DROP == 1");

        uint64_t rA = img.reads.load();
        uint8_t *pa = mgr.self_reliat_get(blkA);
        CHECK(pa != nullptr && img.reads.load() == rA, "dirty row kept resident (no read)");
        mgr.self_reliat_put(blkA);

        uint64_t rB = img.reads.load();
        uint8_t *pb = mgr.self_reliat_get(blkB);
        CHECK(pb != nullptr && img.reads.load() == rB + 1, "clean row dropped -> re-read on get");
        mgr.self_reliat_put(blkB);
        std::printf("  observed: FLUSH_WB=%llu FLUSH_DROP=%llu writes=%llu\n",
                    (unsigned long long)sc.get(MDP_FLUSH_WB),
                    (unsigned long long)sc.get(MDP_FLUSH_DROP),
                    (unsigned long long)img.writes.load());
        END_CASE("3");
    }

    /* ── CASE 4：reap_expired（弱 TTL 淘汰） ──────────────────────── */
    {
        BEGIN_CASE("4", "reap_expired evicts cold rows after TTL");
        MemImage img(img_blocks);
        StatCollector sc;
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, base, true, &sc);
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        uint64_t T = g_tsc_hz ? (g_tsc_hz / 50) : 100000000ull;   /* ~20ms */
        mgr.self_reliat_set_ttl_ticks(T);

        uint64_t rows[4] = {blk_of(base, 0), blk_of(base, 1), blk_of(base, 2), blk_of(base, 3)};
        for (uint64_t b : rows) {
            mgr.self_reliat_get(b); mgr.self_reliat_put(b);   /* load */
            mgr.self_reliat_get(b); mgr.self_reliat_put(b);   /* hit -> refresh accss_stamp */
        }
        spin_ticks(T * 2);

        sc.reset();
        mgr.self_reliat_reap_expired();
        CHECK(sc.get(MDP_REAP) == 4, "MDP_REAP == 4");
        CHECK(sc.get(MDP_RESIDENT_DEL_BYTES) == 4 * 8192, "MDP_RESIDENT_DEL_BYTES == 4*8192");

        uint64_t r1 = img.reads.load();
        uint8_t *p = mgr.self_reliat_get(rows[0]);
        CHECK(p != nullptr && img.reads.load() == r1 + 1, "evicted row re-reads on next get");
        mgr.self_reliat_put(rows[0]);
        std::printf("  observed: REAP=%llu RESIDENT_DEL=%llu\n",
                    (unsigned long long)sc.get(MDP_REAP),
                    (unsigned long long)sc.get(MDP_RESIDENT_DEL_BYTES));
        END_CASE("4");
    }

    /* ── CASE 5a：强淘汰 PRESSURE（无视 TTL，全踹 0-ref） ─────────── */
    {
        BEGIN_CASE("5a", "strong reap PRESSURE evicts all N clean 0-ref rows");
        MemImage img(img_blocks);
        StatCollector sc;
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, base, true, &sc);
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        const int N = 8;
        uint64_t rows[N];
        for (int i = 0; i < N; i++) {
            rows[i] = blk_of(base, i);
            mgr.self_reliat_get(rows[i]);
            mgr.self_reliat_put(rows[i]);
        }
        sc.reset();
        mgr.self_reliat_reap_expired_strong(MDP_SQUEEZE_PRESSURE);
        CHECK(sc.get(MDP_REAP_STRONG) == (uint64_t)N, "MDP_REAP_STRONG == N");
        CHECK(sc.get(MDP_RESIDENT_DEL_BYTES) == (uint64_t)N * 8192,
              "MDP_RESIDENT_DEL_BYTES == N*8192");
        if (expect_mode == MDP_MODE_RADIX_SHARD) {
            CHECK(sc.get(MDP_TABLE_FREE) > 0, "RADIX MDP_TABLE_FREE > 0");
            CHECK(sc.get(MDP_TABLE_DEL_BYTES) > 0, "RADIX MDP_TABLE_DEL_BYTES > 0");
        }
        uint64_t reads0 = img.reads.load();
        uint64_t reloaded = 0;
        for (int i = 0; i < N; i++) {
            if (mgr.self_reliat_get(rows[i])) reloaded++;
            mgr.self_reliat_put(rows[i]);
        }
        CHECK(reloaded == (uint64_t)N, "all N re-get succeed");
        CHECK(img.reads.load() == reads0 + (uint64_t)N, "all N re-read from disk (were evicted)");
        std::printf("  observed: REAP_STRONG=%llu REAP(weak slot)=%llu RESIDENT_DEL=%llu TABLE_FREE=%llu rereads=%llu\n",
                    (unsigned long long)sc.get(MDP_REAP_STRONG),
                    (unsigned long long)sc.get(MDP_REAP),
                    (unsigned long long)sc.get(MDP_RESIDENT_DEL_BYTES),
                    (unsigned long long)sc.get(MDP_TABLE_FREE),
                    (unsigned long long)(img.reads.load() - reads0));
        END_CASE("5a");
    }

    /* ── CASE 5b：强淘汰 LOWLOAD（只踹 TTL 到期的冷行，放过热行） ─── */
    {
        BEGIN_CASE("5b", "strong reap LOWLOAD evicts cold, spares hot");
        MemImage img(img_blocks);
        StatCollector sc;
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, base, true, &sc);
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        uint64_t T = g_tsc_hz ? (g_tsc_hz / 50) : 100000000ull;
        mgr.self_reliat_set_ttl_ticks(T);

        uint64_t cold[4] = {blk_of(base, 0), blk_of(base, 1), blk_of(base, 2), blk_of(base, 3)};
        uint64_t hot[4]  = {blk_of(base, 4), blk_of(base, 5), blk_of(base, 6), blk_of(base, 7)};
        for (uint64_t b : cold) { mgr.self_reliat_get(b); mgr.self_reliat_put(b); }
        for (uint64_t b : hot)  { mgr.self_reliat_get(b); mgr.self_reliat_put(b); }
        spin_ticks(T * 3);
        for (uint64_t b : hot) { mgr.self_reliat_get(b); mgr.self_reliat_put(b); }   /* refresh hot */

        sc.reset();
        mgr.self_reliat_reap_expired_strong(MDP_SQUEEZE_LOWLOAD);
        CHECK(sc.get(MDP_REAP_STRONG) == 4, "MDP_REAP_STRONG == 4 (cold only)");

        uint64_t rr0 = img.reads.load();
        uint8_t *pc = mgr.self_reliat_get(cold[0]);
        CHECK(pc != nullptr && img.reads.load() == rr0 + 1, "cold row re-read after LOWLOAD");
        mgr.self_reliat_put(cold[0]);

        uint64_t rr1 = img.reads.load();
        uint8_t *ph = mgr.self_reliat_get(hot[0]);
        CHECK(ph != nullptr && img.reads.load() == rr1, "hot row SPARED (no re-read)");
        mgr.self_reliat_put(hot[0]);
        std::printf("  observed: REAP_STRONG=%llu REAP(weak slot)=%llu (cold), hot rereads=%llu\n",
                    (unsigned long long)sc.get(MDP_REAP_STRONG),
                    (unsigned long long)sc.get(MDP_REAP),
                    (unsigned long long)(img.reads.load() - rr1));
        END_CASE("5b");
    }

    /* ── CASE 5c：in-use 行两档都不被强淘汰 ──────────────────────── */
    for (int gi = 0; gi < 2; gi++) {
        mdp_squeeze_gear_t gear = (gi == 0) ? MDP_SQUEEZE_PRESSURE : MDP_SQUEEZE_LOWLOAD;
        char cid[16];
        std::snprintf(cid, sizeof cid, "5c-%s", (gi == 0) ? "P" : "L");
        BEGIN_CASE(cid, gi == 0 ? "in-use row preserved under PRESSURE"
                                 : "in-use row preserved under LOWLOAD");
        MemImage img(img_blocks);
        StatCollector sc;
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, base, true, &sc);
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        uint64_t T = g_tsc_hz ? (g_tsc_hz / 50) : 100000000ull;
        if (gear == MDP_SQUEEZE_LOWLOAD) mgr.self_reliat_set_ttl_ticks(T);

        uint64_t x = blk_of(base, 0);
        uint64_t y = blk_of(base, 1);
        uint8_t *px = mgr.self_reliat_get(x);   /* 持用不 put：accessor > 0 */
        CHECK(px != nullptr, "in-use get returns page");
        for (int i = 0; i < 8192; i++) px[i] = (uint8_t)(0x5A ^ i);

        mgr.self_reliat_get(y);                 /* 干净 0-ref 行 */
        mgr.self_reliat_put(y);
        if (gear == MDP_SQUEEZE_LOWLOAD) spin_ticks(T * 2);

        sc.reset();
        mgr.self_reliat_reap_expired_strong(gear);
        CHECK(sc.get(MDP_REAP_STRONG) == 1, "only the idle 0-ref row reaped");
        CHECK(px[0] == (uint8_t)(0x5A ^ 0) && px[123] == (uint8_t)(0x5A ^ 123),
              "in-use page data intact after strong reap");

        mgr.self_reliat_put(x);
        uint64_t rr = img.reads.load();
        uint8_t *px2 = mgr.self_reliat_get(x);
        CHECK(px2 == px, "in-use row still resident (same page ptr)");
        CHECK(img.reads.load() == rr, "in-use row not re-read (never evicted)");
        mgr.self_reliat_put(x);
        std::printf("  observed: REAP_STRONG=%llu REAP(weak slot)=%llu (expected strong=1)\n",
                    (unsigned long long)sc.get(MDP_REAP_STRONG),
                    (unsigned long long)sc.get(MDP_REAP));
        END_CASE(cid);
    }

    /* ── CASE 6：stat.inc == nullptr → 干净 no-op ─────────────────── */
    {
        BEGIN_CASE("6", "null stat hook is a clean no-op");
        MemImage img(img_blocks);
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, base, false, nullptr);
        CHECK(cfg.stat.inc == nullptr, "hook indeed null");
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        CHECK(rc == RC_OK, "ctor ok with null hook");
        uint64_t b = blk_of(base, 0);
        {
            mdata_piece_session s(&mgr, b);
            CHECK(s.data != nullptr, "session works with null hook");
            std::memset(s.data, 0x11, 8192);
            s.dirty();
        }
        mgr.flush_all();
        mgr.self_reliat_set_ttl_ticks(1);
        mgr.self_reliat_reap_expired();
        mgr.self_reliat_reap_expired_strong(MDP_SQUEEZE_PRESSURE);
        CHECK(mgr.is_ready(), "still ready after no-op hook path");
        END_CASE("6");
    }

    /* ── CASE 7：blkidx 边界（非偶 / 越界） ───────────────────────── */
    {
        BEGIN_CASE("7", "blkidx_valid + get/session edge (odd / out-of-range)");
        MemImage img(img_blocks);
        StatCollector sc;
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, base, true, &sc);
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);

        CHECK(!mgr.blkidx_valid(base + 1), "odd blkidx invalid");
        CHECK(!mgr.blkidx_valid(base + count * 2), "blkidx at end (OOR) invalid");
        CHECK(mgr.blkidx_valid(base + count * 2 - 2), "last even blkidx valid");
        CHECK(mgr.blkidx_valid(blk_of(base, 0)), "even in-range valid");

        uint64_t r0 = img.reads.load();
        CHECK(mgr.self_reliat_get(base + 1) == nullptr, "get(odd) -> nullptr");
        CHECK(mgr.self_reliat_get(base + count * 2) == nullptr, "get(OOR) -> nullptr");
        CHECK(img.reads.load() == r0, "no read_block for invalid blkidx");

        {
            mdata_piece_session s(&mgr, base + 1);
            CHECK(s.index_error(), "session(odd) index_error");
            CHECK(s.data == nullptr, "session(odd) data null");
        }
        {
            mdata_piece_session s(&mgr, base + count * 2);
            CHECK(s.index_error(), "session(OOR) index_error");
            CHECK(s.data == nullptr, "session(OOR) data null");
        }
        uint64_t b = blk_of(base, 0);
        uint8_t *p = mgr.self_reliat_get(b);
        CHECK(p != nullptr, "get(valid even) -> page");
        mgr.self_reliat_put(b);
        END_CASE("7");
    }

    /* ── CASE 8：退化配置 ─────────────────────────────────────────── */
    {
        BEGIN_CASE("8", "degenerate cfg: Piece_count == 0 rejected");
        MemImage img(64);
        StatCollector sc;
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, 0, base, true, &sc);
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        CHECK(rc != RC_OK, "rc != RC_OK for Piece_count==0");
        CHECK(!mgr.is_ready(), "not ready for Piece_count==0");
        END_CASE("8");
    }
    {
        BEGIN_CASE("8b", "degenerate cfg: odd Piece_base rejected (self_reliat)");
        MemImage img(img_blocks);
        StatCollector sc;
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, 1, true, &sc);
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        std::printf("  observed: rc=%llu (RC_OK=%d, RC_ERR_STATE=%d) is_ready=%d\n",
                    (unsigned long long)rc, (int)RC_OK, (int)RC_ERR_STATE, (int)mgr.is_ready());
        CHECK(rc != RC_OK, "rc != RC_OK for odd Piece_base (spec: 基址必须偶数)");
        CHECK(!mgr.is_ready(), "not ready for odd Piece_base");
        END_CASE("8b");
    }
    {
        BEGIN_CASE("8c", "contrast: no_cache rejects odd Piece_base (asymmetry check)");
        MemImage img(img_blocks);
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, 1, false, nullptr);
        cfg.mode = cache_work_mode_t::no_cache;
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        std::printf("  observed: no_cache odd base rc=%llu is_ready=%d\n",
                    (unsigned long long)rc, (int)mgr.is_ready());
        CHECK(rc != RC_OK, "no_cache rejects odd base");
        CHECK(!mgr.is_ready(), "no_cache not ready for odd base");
        END_CASE("8c");
    }
    {
        BEGIN_CASE("9", "obs: freshly loaded row treated as TTL-cold under LOWLOAD");
        MemImage img(img_blocks);
        StatCollector sc;
        mdata_piece_caches_cfg_t cfg = make_cfg(&img, count, base, true, &sc);
        result_code_t rc = ~0ull;
        mdata_piece_caches_mgr mgr(cfg, &rc);
        /* 1s TTL，且不 spin：若模块在 miss-load 时记录访问戳，则该行不冷、不该被踹。 */
        uint64_t T = g_tsc_hz ? g_tsc_hz : 1000000000ull;
        mgr.self_reliat_set_ttl_ticks(T);
        uint64_t b = blk_of(base, 0);
        mgr.self_reliat_get(b);      /* miss-load */
        mgr.self_reliat_put(b);      /* 归 0-ref，未再 hit（accss_stamp 仍 0） */
        sc.reset();
        mgr.self_reliat_reap_expired_strong(MDP_SQUEEZE_LOWLOAD);
        uint64_t reaped = sc.get(MDP_REAP) + sc.get(MDP_REAP_STRONG);
        std::printf("  observed: just-loaded row reaped=%llu (REAP=%llu REAP_STRONG=%llu) with TTL=1s, no delay\n",
                    (unsigned long long)reaped,
                    (unsigned long long)sc.get(MDP_REAP),
                    (unsigned long long)sc.get(MDP_REAP_STRONG));
        CHECK(reaped == 0, "freshly loaded (never hit) row should not be TTL-cold");
        END_CASE("9");
    }
}

int main() {
    tsc_info_t ti;
    if (tsc_probe(&ti)) g_tsc_hz = ti.freq_hz;
    std::printf("TSC probe: reliable=%d freq_hz=%llu\n", (int)ti.reliable,
                (unsigned long long)g_tsc_hz);

    run_suite("CTRL_ARRAY", 8192, MDP_MODE_CTRL_ARRAY);
    run_suite("RADIX_SHARD", 16384, MDP_MODE_RADIX_SHARD);

    std::printf("\n================ TOTAL: PASS=%d FAIL=%d ================\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

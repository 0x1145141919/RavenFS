/* ═══════════════════════════════════════════════════════════════════
 * tests/test_mdata_piece_cache_concurrency.cpp
 *
 * WHITE-BOX concurrency stress for mdata_piece_caches_mgr (self_reliat).
 * Tests both geometry tiers:
 *   · CTRL_ARRAY  : Piece_count = 8192  (bits 13, ctrl_array_)
 *   · RADIX_SHARD : Piece_count = 16384 (bits 14, 128 shards, statey_14 {7,7})
 *
 * Phases per tier:
 *   A) same-key read-only contention + background reapers (reap_expired,
 *      reap_expired_strong PRESSURE, reap_expired_strong LOWLOAD)
 *   B) spread-key read/write workers + background reapers
 *   C) long-hold invariance: each holder writes a magic into its page and
 *      re-validates it while strong reap hammers -> row must NEVER be evicted
 *      while a live session holds it (ASan catches UAF if it is)
 *   D) flush_all concurrent with get/put (host says flush-vs-reap must not
 *      overlap; flush-vs-get is claimed safe)
 *   E) flush-vs-get lost-dirty determinism probe (CTRL_ARRAY, slow writeback)
 *
 * Invariants asserted after every phase:
 *   · accessor accounting balanced (sum over all ctrl rows == 0)
 *   · (GET_HIT + GET_MISS) == PUT  (stat hook counter cross-check)
 *   · pages read back == canonical pattern for pristine key ranges
 *
 * NOTE: production sources are NOT modified. `private`/`protected` are
 * macro-remapped around the include ONLY to let this white-box test read the
 * accessor fields for accounting; this is a test-side view, not a code change.
 * ═══════════════════════════════════════════════════════════════════ */
#include <atomic>
#include <shared_mutex>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <thread>
#include <cstring>
#include <cstdio>
#include <memory>
#include <chrono>
#include <cpuid.h>
#include <new>
#if defined(__linux__) || defined(__unix__)
#include <sys/mman.h>
#endif

/* White-box view: remap `private`->`public` (explicit sections) AND
 * `class`->`struct` (implicit-private default of nested slice_radix_tree).
 * All transitive system headers are pre-included above so the macro cannot
 * leak into libstdc++. Production sources are untouched. */
#define private public
#define protected public
#define class struct
#include "ravenfs/mdata_piece_caches.hpp"
#undef class
#undef protected
#undef private

#include "util/lock.h"

/* ── md_ stat hook (lock-free, thread-safe) ── */
static std::atomic<uint64_t> g_stat[MDP_STAT_COUNT];
static void stat_inc(void *, uint32_t field, uint64_t delta) {
    if (field < MDP_STAT_COUNT) g_stat[field].fetch_add(delta, std::memory_order_relaxed);
}
static void stat_reset() {
    for (auto &s : g_stat) s.store(0, std::memory_order_relaxed);
}

/* ── in-memory block image ── */
static constexpr uint64_t BS    = 4096;
static constexpr uint64_t PIECE = 8192;

struct image_t {
    std::vector<uint8_t> buf;
    uint64_t nblocks = 0;
    std::mutex mtx;
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> write_started{0};
    std::atomic<uint64_t> write_slow_us{0};   /* artificial writeback delay */
};

static inline uint8_t pat_byte(uint64_t blk, uint64_t off) {
    uint64_t x = blk * 0x9E3779B97F4A7C15ull + off * 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29; x *= 0x94D049BB133111EBull; x ^= x >> 32;
    return (uint8_t)x;
}
static void fill_image(image_t &im) {
    for (uint64_t b = 0; b < im.nblocks; b++)
        for (uint64_t o = 0; o < BS; o++)
            im.buf[b * BS + o] = pat_byte(b, o);
}
static bool page_matches(const uint8_t *p, uint64_t blk) {
    for (uint64_t o = 0; o < PIECE; o++)
        if (p[o] != pat_byte(blk + (o / BS), o % BS)) return false;
    return true;
}
static result_code_t img_read(void *ctx, uint64_t blk, uint64_t bl, void *out) {
    image_t *im = (image_t *)ctx;
    if (blk + bl > im->nblocks) return RC_ERR_IO;
    std::lock_guard<std::mutex> g(im->mtx);
    std::memcpy(out, im->buf.data() + blk * BS, bl * BS);
    im->reads.fetch_add(1, std::memory_order_relaxed);
    return RC_OK;
}
static result_code_t img_write(void *ctx, uint64_t blk, uint64_t bl, const void *in) {
    image_t *im = (image_t *)ctx;
    if (blk + bl > im->nblocks) return RC_ERR_IO;
    {
        std::lock_guard<std::mutex> g(im->mtx);
        std::memcpy(im->buf.data() + blk * BS, in, bl * BS);
        im->writes.fetch_add(1, std::memory_order_relaxed);
    }
    /* capture complete -> let a concurrent writer dirty the live page while the
     * (simulated slow) device write is still in flight, before flush clears DIRTY */
    im->write_started.fetch_add(1, std::memory_order_relaxed);
    uint64_t slow = im->write_slow_us.load(std::memory_order_relaxed);
    if (slow) std::this_thread::sleep_for(std::chrono::microseconds(slow));
    return RC_OK;
}

/* ── fixture ── */
struct fixture_t {
    image_t img;
    mdata_piece_caches_cfg_t cfg{};
    mdata_piece_caches_mgr *mgr = nullptr;
    uint64_t count = 0, base = 0;

    bool init(uint64_t cnt, uint64_t b) {
        count = cnt; base = b;
        img.nblocks = b + cnt * 2;
        img.buf.assign(img.nblocks * BS, 0);
        fill_image(img);
        cfg.mode = cache_work_mode_t::self_reliat_cache;
        cfg.read_block = img_read;
        cfg.write_block = img_write;
        cfg.io_ctx = &img;
        cfg.Piece_count = cnt;
        cfg.Piece_base_FS_block_idx = b;
        cfg.stat = { stat_inc, nullptr };
        result_code_t rc = RC_ERR_IO;
        mgr = new mdata_piece_caches_mgr(cfg, &rc);
        return rc == RC_OK && mgr && mgr->is_ready();
    }
    ~fixture_t() { delete mgr; }
};

/* ── white-box accessor accounting walk ── */
static uint64_t walk_accessor(void *table, uint8_t level, uint8_t tree_levels,
                              uint8_t *lb, uint8_t total_levels) {
    using MT = mdata_piece_caches_mgr::mdata_piece_ctrl_t;
    if (level == (uint8_t)(tree_levels - 1)) {
        const uint64_t n = 1ull << lb[total_levels - 1];
        MT *leaf = (MT *)table;
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; i++)
            sum += (leaf[i].ctrl_word.load(std::memory_order_acquire) >> CTRL_ACCESSOR_SHIFT);
        return sum;
    }
    void **t = (void **)table;
    const uint64_t w = 1ull << lb[1 + level];
    uint64_t sum = 0;
    for (uint64_t i = 0; i < w; i++)
        if (t[i]) sum += walk_accessor(t[i], (uint8_t)(level + 1), tree_levels, lb, total_levels);
    return sum;
}
static uint64_t accessor_sum(mdata_piece_caches_mgr *m) {
    if (m->mode_ == MDP_MODE_CTRL_ARRAY) {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < m->piece_count_; i++)
            sum += (m->ctrl_array_[i].ctrl_word.load(std::memory_order_acquire) >> CTRL_ACCESSOR_SHIFT);
        return sum;
    }
    uint64_t sum = 0;
    for (uint64_t i = 0; i < m->n_slices_; i++) {
        auto *s = m->slices_[i];
        if (!s || !s->root_) continue;
        sum += walk_accessor(s->root_, 0, s->tree_levels_,
                             s->plan_.level_bits, s->plan_.total_levels);
    }
    return sum;
}

/* ── shared test state ── */
struct counters_t {
    std::atomic<uint64_t> worker_errs{0};
    std::atomic<uint64_t> holder_lost{0};
    std::atomic<uint64_t> attach_fail{0};
};
static counters_t C;
static void counters_reset() {
    C.worker_errs.store(0); C.holder_lost.store(0); C.attach_fail.store(0);
}

static uint64_t now_ticks() { return rdtsc_timestamp(); }

/* ── background reaper ── */
static void reaper_loop(mdata_piece_caches_mgr *m, std::atomic<bool> *stop, int kind) {
    while (!stop->load(std::memory_order_relaxed)) {
        if (kind == 0 || kind == 2) m->self_reliat_reap_expired();
        if (kind == 0 || kind == 1) m->self_reliat_reap_expired_strong(MDP_SQUEEZE_PRESSURE);
        if (kind == 0 || kind == 2) m->self_reliat_reap_expired_strong(MDP_SQUEEZE_LOWLOAD);
        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

/* ── phase A: same-key read-only contention + reapers ── */
static void phase_samekey(fixture_t &f, int nthreads, int ms, int nreap) {
    std::atomic<bool> stop{false};
    std::vector<std::thread> th;
    for (int t = 0; t < nthreads; t++) {
        th.emplace_back([&, t] {
            uint64_t rng = 0x1234567u + t * 7919u;
            while (!stop.load(std::memory_order_relaxed)) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                uint64_t piece = rng & 7;             /* pieces 0..7 shared */
                uint64_t blk = f.base + piece * 2;
                mdata_piece_session s(f.mgr, blk);
                if (!s) { C.attach_fail.fetch_add(1); continue; }
                if (!page_matches(s.data, blk)) C.worker_errs.fetch_add(1);
                std::this_thread::yield();
            }
        });
    }
    std::vector<std::thread> rp;
    for (int r = 0; r < nreap; r++)
        rp.emplace_back([&, r] { reaper_loop(f.mgr, &stop, r % 3); });
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    stop.store(true, std::memory_order_relaxed);
    for (auto &x : th) x.join();
    for (auto &x : rp) x.join();
}

/* ── phase B/C: spread-key workers (write magic + hold-verify) + reapers ── */
static void phase_spread_hold(fixture_t &f, int nthreads, int ms, int nreap,
                              uint64_t piece_lo, uint64_t pieces_per_thread,
                              int hold_checks) {
    std::atomic<bool> stop{false};
    std::vector<std::thread> th;
    for (int t = 0; t < nthreads; t++) {
        th.emplace_back([&, t] {
            uint64_t rng = 0xABCDEF01u + t * 104729u;
            uint64_t off = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                uint64_t piece = piece_lo + t * pieces_per_thread + (off % pieces_per_thread);
                off++;
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                uint64_t blk = f.base + piece * 2;
                mdata_piece_session s(f.mgr, blk);
                if (!s) { C.attach_fail.fetch_add(1); continue; }
                uint32_t magic = (uint32_t)(0xB16B00B5u ^ (uint32_t)(t * 0x1000u) ^ (uint32_t)rng);
                std::memcpy(s.data, &magic, 4);
                s.dirty();
                for (int h = 0; h < hold_checks && !stop.load(std::memory_order_relaxed); h++) {
                    uint32_t v;
                    std::memcpy(&v, s.data, 4);
                    /* Row is held (accessor>0): strong reap must NOT free it.
                     * If it did, ASan would already have fired on this read. */
                    if (v != magic) C.holder_lost.fetch_add(1);
                    std::this_thread::yield();
                }
            }
        });
    }
    std::vector<std::thread> rp;
    for (int r = 0; r < nreap; r++)
        rp.emplace_back([&, r] { reaper_loop(f.mgr, &stop, r % 3); });
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    stop.store(true, std::memory_order_relaxed);
    for (auto &x : th) x.join();
    for (auto &x : rp) x.join();
}

/* ── phase D: flush_all concurrent with get/put ── */
static void phase_flush_race(fixture_t &f, int nthreads, int ms, bool with_reap) {
    std::atomic<bool> stop{false};
    std::vector<std::thread> th;
    for (int t = 0; t < nthreads; t++) {
        th.emplace_back([&, t] {
            uint64_t rng = 0x55AA0000u + t * 31337u;
            uint64_t off = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                uint64_t piece = 4096 + (uint64_t)t * 32 + (off % 32);
                off++;
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                uint64_t blk = f.base + piece * 2;
                mdata_piece_session s(f.mgr, blk);
                if (!s) { C.attach_fail.fetch_add(1); continue; }
                uint32_t magic = (uint32_t)(0xF00DF00Du ^ (uint32_t)(t << 8) ^ (uint32_t)rng);
                std::memcpy(s.data, &magic, 4);
                s.dirty();
                uint32_t v; std::memcpy(&v, s.data, 4);
                if (v != magic) C.holder_lost.fetch_add(1);
                std::this_thread::yield();
            }
        });
    }
    std::thread fl([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            f.mgr->flush_all();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    std::vector<std::thread> rp;
    if (with_reap) rp.emplace_back([&] { reaper_loop(f.mgr, &stop, 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    stop.store(true, std::memory_order_relaxed);
    for (auto &x : th) x.join();
    fl.join();
    for (auto &x : rp) x.join();
}

/* ── phase E: deterministic flush-vs-get lost-dirty probe ── */
static int phase_lost_dirty() {
    fixture_t f;
    if (!f.init(512, 0)) { printf("  [E] fixture init FAILED\n"); return -1; }
    f.img.write_slow_us.store(40000, std::memory_order_relaxed);   /* 40ms writeback */
    const uint64_t blk = 2;   /* piece 1 */
    uint32_t magic1 = 0x11111111u, magic2 = 0x22222222u;

    /* 1) session1 loads, writes magic1, dirties, releases (accessor 0, DIRTY set) */
    {
        mdata_piece_session s(f.mgr, blk);
        if (!s) { printf("  [E] session1 FAILED\n"); return -1; }
        std::memcpy(s.data, &magic1, 4);
        s.dirty();
    }
    /* 2) flush_all in a thread: hits dirty row -> slow writeback -> clears DIRTY */
    std::atomic<bool> flush_done{false};
    std::thread fl([&] { f.mgr->flush_all(); flush_done.store(true); });
    /* 3) wait until writeback actually started (it then sleeps 40ms) */
    while (f.img.write_started.load(std::memory_order_relaxed) == 0 &&
           !flush_done.load(std::memory_order_relaxed))
        std::this_thread::yield();
    /* 4) concurrent get: write magic2 + mark_dirty during flush's writeback */
    {
        mdata_piece_session s(f.mgr, blk);
        if (s) { std::memcpy(s.data, &magic2, 4); s.dirty(); }
    }
    fl.join();

    /* 5) force eviction (PRESSURE, no TTL) and reload from disk */
    f.mgr->self_reliat_reap_expired_strong(MDP_SQUEEZE_PRESSURE);
    uint32_t back = 0;
    {
        mdata_piece_session s(f.mgr, blk);
        if (s) std::memcpy(&back, s.data, 4);
    }
    printf("  [E] after flush+reload: 0x%08x (magic1=0x%08x magic2=0x%08x) writes=%llu -> %s\n",
           back, magic1, magic2, (unsigned long long)f.img.writes.load(),
           back == magic2 ? "PRESERVED" : (back == magic1 ? "LOST-UPDATE (magic2 dropped)" : "OTHER"));
    return (back == magic2) ? 0 : 1;   /* 1 == lost update reproduced */
}

/* ── phase F: TTL stamp-on-load white-box probe ── */
static int phase_ttl_stamp() {
    fixture_t f;
    if (!f.init(512, 0)) { printf("  [F] fixture init FAILED\n"); return -1; }
    f.mgr->self_reliat_set_ttl_ticks(1);   /* 1 tick TTL */
    const uint64_t blk = 4;                /* piece 2 (CTRL_ARRAY: base=0) */
    { mdata_piece_session s(f.mgr, blk); if (!s) { printf("  [F] get FAILED\n"); return -1; } }
    auto &c = f.mgr->ctrl_array_[blk >> 1];
    uint64_t stamp = c.accss_stamp.load(std::memory_order_relaxed);
    bool is_working = ((c.ctrl_word.load() & CTRL_STATE_MASK) == working);
    uint64_t reap_before = g_stat[MDP_REAP].load();
    f.mgr->self_reliat_reap_expired();
    uint64_t reap_after = g_stat[MDP_REAP].load();
    uint64_t reclaimed = reap_after - reap_before;
    printf("  [F] miss-load then put: accss_stamp=%llu state=working?%d  reap_expired reclaimed=%llu row(s)\n",
           (unsigned long long)stamp, (int)is_working, (unsigned long long)reclaimed);
    if (stamp == 0 && reclaimed > 0)
        printf("      !! freshly-loaded row has accss_stamp==0 -> instantly 'TTL-expired' (premature eviction)\n");
    return (stamp == 0 && reclaimed > 0) ? 1 : 0;
}

static bool g_skip_unsupported = false;

/* ── per-tier driver ── */
static int run_tier(const char *name, uint64_t count) {
    printf("═══ tier %s (Piece_count=%llu) ═══\n", name, (unsigned long long)count);
    fixture_t f;
    stat_reset();
    if (!f.init(count, 0)) { printf("  init FAILED\n"); return 2; }
    printf("  mode=%d (0=CTRL_ARRAY 1=RADIX) n_slices=%llu\n",
           (int)f.mgr->self_reliat_mode(), (unsigned long long)f.mgr->n_slices_);
    /* aggressive TTL so background reap actually recycles */
    f.mgr->self_reliat_set_ttl_ticks(2000);

    int rc = 0;
    counters_reset();

    /* A) same-key read-only + reapers */
    printf("  [A] same-key read-only contention + reapers ...\n");
    phase_samekey(f, 8, 900, 3);
    uint64_t accA = accessor_sum(f.mgr);
    uint64_t hitA = g_stat[MDP_GET_HIT].load(), missA = g_stat[MDP_GET_MISS].load();
    uint64_t putA = g_stat[MDP_PUT].load();
    printf("      errs=%llu attach_fail=%llu accessor=%llu hit=%llu miss=%llu put=%llu\n",
           (unsigned long long)C.worker_errs.load(), (unsigned long long)C.attach_fail.load(),
           (unsigned long long)accA, (unsigned long long)hitA, (unsigned long long)missA,
           (unsigned long long)putA);
    if (accA != 0) { printf("      !! accessor NOT balanced\n"); rc = 1; }
    if (hitA + missA != putA) { printf("      !! get/put stat mismatch\n"); rc = 1; }
    if (C.worker_errs.load() != 0) { printf("      !! pristine-page mismatch\n"); rc = 1; }

    /* B/C) spread-key write + hold-verify + reapers */
    printf("  [B/C] spread-key write + hold-verify + reapers ...\n");
    counters_reset();
    uint64_t base_putB = g_stat[MDP_PUT].load();
    uint64_t base_getB = g_stat[MDP_GET_HIT].load() + g_stat[MDP_GET_MISS].load();
    phase_spread_hold(f, 8, 1200, 3, 4096, 128, 8);
    uint64_t accB = accessor_sum(f.mgr);
    uint64_t putB = g_stat[MDP_PUT].load() - base_putB;
    uint64_t getB = (g_stat[MDP_GET_HIT].load() + g_stat[MDP_GET_MISS].load()) - base_getB;
    printf("      holder_lost=%llu attach_fail=%llu accessor=%llu get=%llu put=%llu\n",
           (unsigned long long)C.holder_lost.load(), (unsigned long long)C.attach_fail.load(),
           (unsigned long long)accB, (unsigned long long)getB, (unsigned long long)putB);
    if (accB != 0) { printf("      !! accessor NOT balanced\n"); rc = 1; }
    if (getB != putB) { printf("      !! get/put stat mismatch\n"); rc = 1; }
    if (C.holder_lost.load() != 0) { printf("      !! row evicted while held (harness-visible)\n"); rc = 1; }

    /* D) flush_all concurrent with get/put (no reap, per contract) */
    printf("  [D] flush_all concurrent with get/put (no reap) ...\n");
    counters_reset();
    uint64_t base_putD = g_stat[MDP_PUT].load();
    uint64_t base_getD = g_stat[MDP_GET_HIT].load() + g_stat[MDP_GET_MISS].load();
    phase_flush_race(f, 6, 900, false);
    uint64_t accD = accessor_sum(f.mgr);
    uint64_t putD = g_stat[MDP_PUT].load() - base_putD;
    uint64_t getD = (g_stat[MDP_GET_HIT].load() + g_stat[MDP_GET_MISS].load()) - base_getD;
    printf("      holder_lost=%llu accessor=%llu get=%llu put=%llu\n",
           (unsigned long long)C.holder_lost.load(), (unsigned long long)accD,
           (unsigned long long)getD, (unsigned long long)putD);
    if (accD != 0) { printf("      !! accessor NOT balanced\n"); rc = 1; }
    if (getD != putD) { printf("      !! get/put stat mismatch\n"); rc = 1; }

    /* D2) flush_all + reap concurrently (UNSUPPORTED by host contract — observe) */
    if (g_skip_unsupported) {
        printf("  [D2] SKIPPED (MDP_SKIP_UNSUPPORTED=1)\n");
    } else {
    printf("  [D2] flush_all + reap concurrently (contract says unsupported) ...\n");
    counters_reset();
    phase_flush_race(f, 6, 900, true);
    uint64_t accD2 = accessor_sum(f.mgr);
    printf("      holder_lost=%llu accessor=%llu (0 expected)\n",
           (unsigned long long)C.holder_lost.load(), (unsigned long long)accD2);
    if (accD2 != 0) { printf("      !! accessor NOT balanced\n"); rc = 1; }
    if (C.holder_lost.load() != 0) { printf("      !! row evicted while held\n"); rc = 1; }
    }

    printf("  tier %s -> %s\n\n", name, rc ? "FAIL" : "PASS");
    return rc;
}

int main() {
    printf("mdata_piece_caches concurrency stress (white-box)\n");
    printf("hardware_concurrency=%u\n\n", std::thread::hardware_concurrency());
    g_skip_unsupported = (std::getenv("MDP_SKIP_UNSUPPORTED") != nullptr);
    if (g_skip_unsupported) printf("(unsupported flush+reap overlap phase disabled)\n");
    int rc = 0;
    rc |= run_tier("CTRL_ARRAY", 8192);
    rc |= run_tier("RADIX_SHARD", 16384);
    printf("=== phase E: flush-vs-get lost-dirty probe (CTRL_ARRAY) ===\n");
    int e = phase_lost_dirty();
    if (e == 1) rc |= 4;
    printf("=== phase F: TTL stamp-on-load white-box probe (CTRL_ARRAY) ===\n");
    int fs = phase_ttl_stamp();
    if (fs == 1) rc |= 8;

    const int stress_fail = rc & 3;     /* accessor balance / stat / holder invariants */
    const int hazards = (rc & 4) ? 1 : 0;
    const int ttl_hazard = (rc & 8) ? 1 : 0;
    printf("\nSTRESS ASSERTIONS: %s\n", stress_fail ? "FAIL" : "PASS");
    printf("HAZARD PROBES: lost-dirty=%s ttl-stamp-on-load=%s\n",
           (rc & 4) ? "REPRODUCED" : "clean",
           (rc & 8) ? "REPRODUCED" : "clean");
    if (stress_fail) return 1;                     /* genuine invariant break */
    if (hazards || ttl_hazard) return 2;           /* invariants OK, known hazards reproduced */
    return 0;
}

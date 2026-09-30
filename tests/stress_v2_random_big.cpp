/* ═══════════════════════════════════════════════════════════════════
 * stress_v2_random_big.cpp — bitmap_slices_v2 「超高规格随机压测」（独立 main）
 *
 * 2026-09-30。只读被测实现，不改其一行（头/实现/既有测试/CMake 全不动）。
 * 手工编译（0 error 0 warn）：
 *   g++ -std=c++17 -O2 -Wall -Wextra -Iinclude/ravenfs \
 *       tests/stress_v2_random_big.cpp juvenile_god/bitmap_slices_v2.cpp \
 *       -o /tmp/v2big
 *
 * 被测：include/ravenfs/bitmap_slices_v2.hpp（契约）
 *     + juvenile_god/bitmap_slices_v2.cpp（chunk_slice_mgr_t / bitmap_slices_mgr_t_v2）
 *
 * 规模（写死，按任务）：
 *   block 位图区 blocks=524288（2GB/4096）base=0；inode 位图区 blocks=131072
 *   （512MB/4096）base=524288；processor_count=8；8 线程；blkbm_chunk_order=3；
 *   inobm_chunk_order=0。mock 盘 = (524288+131072)×4096 ≈ 2.68GB（std::vector）。
 *
 * ⚠ 验证省内存（**不做每 bit 一字节影子**，那要 ~17GB）：
 *   · **每线程区间表** vector<{base,len}>，释放时移除/劈分。
 *   · **全局区间结构** std::map<start,end> + 一把 mutex：增量插入/删除 +
 *     邻接/重叠判定 —— 跨线程重复分配 = 插入时命中已有区间。
 *     释放时**先**在全局结构注销、**再**调分配器（否则会出现「分配器已空闲、
 *     结构仍占用」窗口 → 另线程合法重分配被误判重叠；见并发测的同类注记）。
 *   · **free_count 一致性**：每 BARRIER_EVERY 次操作全体线程过**屏障**（静默点），
 *     再判 block_free_total()/inode_free_total() == 总位数 − Σ 各线程 live 计数。
 *   · **逐点对拍**（屏障点）：随机位 block_test/inode_test 与全局结构预期一致。
 *   · **收尾全清审计**：全释放后 free_total == 满、全局结构清空，dtor 落盘后
 *     直扫 mock 盘位图区（2.68GB）全 0。
 *   · **chunks_asyn(&calib)**：片层专项（mgr 不暴露 asyn，见下），calib.new ==
 *     盘实测 0 位数、old 语义正确（含人为重复 free 的漂移检出）。
 *
 * 覆盖：block hard/soft/free/occupy_soft + inode alloc/free；大跨度（occupy 一次
 *   上百万块，跨多 chunk/跨片）；跨片锚点（k×per_slice ± ε）；chunk 尾窗跨窗锚点；
 *   片尾/末尾边界；align_log2 0..15；8 线程按 get_processor_id() 桩独立 CPU。
 *
 * ★ 已知契约边（既有测试亦有记载）：alloc_hard/alloc_soft 为**单窗**实现，
 *   count > 单窗上限（blkbm order3 = 262144 位）→ RC_ERR_NO_BLOCK，**不跨窗硬拼**；
 *   多 chunk/跨片大跨度只能走 block_occupy_soft（本压测大跨度即走此路）。本测在
 *   片层对此边界做定向探针，不视为缺陷。
 * ★ mgr（bitmap_slices_mgr_t_v2）**不暴露 slices / chunks_asyn** ⇒ asyn 语义在
 *   片层用同一 order 几何专项验证；mgr 级以 dtor shutdown 全量合并落盘为同构见证。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/bitmap_slices_v2.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

/* ───────────────────────── 几何（写死） ───────────────────────── */
static const uint64_t BLK_BLOCKS = 524288;
static const uint64_t INO_BLOCKS = 131072;
static const uint64_t INO_BASE   = BLK_BLOCKS;               /* inode 位图区起始 FS_block */
static const uint64_t TOTAL_BLOCKS = BLK_BLOCKS + INO_BLOCKS;/* 655360 → 2.68GB */
static const uint32_t CPU_CNT    = 8;
static const uint32_t NTHREADS   = 8;
static const uint8_t  BLK_ORDER  = 3;   /* chunk = 4096<<3 = 32768B = 262144 位 */
static const uint8_t  INO_ORDER  = 0;   /* chunk = 4096B   = 32768 位  */
static const uint64_t BLK_BITS   = BLK_BLOCKS << 15;   /* 17179869184 = 2^34 */
static const uint64_t INO_BITS   = INO_BLOCKS << 15;   /* 4294967296  = 2^32 */
static const uint64_t BLK_CHUNK_BITS = 32768ULL << BLK_ORDER;  /* 262144 */
static const uint64_t INO_CHUNK_BITS = 32768ULL << INO_ORDER;  /* 32768  */
static const uint64_t BARRIER_EVERY  = 4000;   /* 屏障/一致性校验间隔 */
static const size_t   MAXSEG = 1024;           /* 每线程区间数上限 */
static const uint64_t MAXLIVE = 4000000;       /* 每线程 live 位数上限（触发归还）*/

/* ───────────────────────── 失败上报 ───────────────────────── */
static std::atomic<int> g_fail{0};
static std::atomic<int> g_printed{0};
static std::mutex       g_pmu;
static const char      *g_phase = "";

#define CK(cond, ...)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            g_fail.fetch_add(1, std::memory_order_relaxed);                  \
            if (g_printed.fetch_add(1, std::memory_order_relaxed) < 10) {    \
                std::lock_guard<std::mutex> lk(g_pmu);                       \
                std::printf("  [FAIL] phase=%s ", g_phase);                  \
                std::printf(__VA_ARGS__);                                    \
                std::printf("\n");                                           \
                std::fflush(stdout);                                         \
            }                                                                \
        }                                                                    \
    } while (0)

/* ───────────────────────── cpu 身份桩（按线程返回） ───────────────────────── */
static thread_local uint32_t t_cpu = 0;
extern "C" uint32_t get_processor_id() { return t_cpu; }

/* ───────────────────────── mock 盘（绝对 FS_block） ───────────────────────── */
struct Disk {
    std::vector<uint8_t> data;
    std::atomic<uint64_t> rd{0}, wr{0};
    std::atomic<uint64_t> io_err{0};
};
static result_code_t disk_read(void *ctx, uint64_t blk, uint64_t len, void *buf) {
    Disk *d = static_cast<Disk *>(ctx);
    if (len == 0 || blk >= TOTAL_BLOCKS || len > TOTAL_BLOCKS - blk) { d->io_err++; return RC_ERR_IO; }
    std::memcpy(buf, &d->data[(size_t)blk * 4096], (size_t)len * 4096);
    d->rd.fetch_add(1, std::memory_order_relaxed);
    return RC_OK;
}
static result_code_t disk_write(void *ctx, uint64_t blk, uint64_t len, const void *buf) {
    Disk *d = static_cast<Disk *>(ctx);
    if (len == 0 || blk >= TOTAL_BLOCKS || len > TOTAL_BLOCKS - blk) { d->io_err++; return RC_ERR_IO; }
    std::memcpy(&d->data[(size_t)blk * 4096], buf, (size_t)len * 4096);
    d->wr.fetch_add(1, std::memory_order_relaxed);
    return RC_OK;
}

/* 区域 0 位数（区起 base_blk，位数 nbits；位序无关 popcount） */
static uint64_t region_zeros(const Disk &d, uint64_t base_blk, uint64_t nbits) {
    const uint8_t *p = d.data.data() + (size_t)base_blk * 4096;
    const uint64_t nbytes = nbits >> 3;
    uint64_t z = 0;
    uint64_t i = 0;
    for (; i + 8 <= nbytes; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        z += 64 - (uint64_t)__builtin_popcountll(w);
    }
    for (; i < nbytes; i++) z += 8 - (uint64_t)__builtin_popcount((unsigned)p[i]);
    return z;
}

/* ───────────────────────── 区间表 + 全局区间结构 ───────────────────────── */
struct seg { uint64_t start, len; };

struct Registry {
    std::mutex mu;
    std::map<uint64_t, uint64_t> m;   /* start → end（互不重叠；允许邻接） */
    uint64_t total = 0;               /* Σ 长度（持 mu 维护） */
    uint64_t overlap = 0;             /* 检出重叠/不一致次数 */

    bool insert(uint64_t s, uint64_t e, uint64_t *bs, uint64_t *be) {
        std::lock_guard<std::mutex> lk(mu);
        if (s >= e) { overlap++; return false; }
        auto it = m.lower_bound(s);
        if (it != m.end() && it->first < e) { if (bs) *bs = it->first; if (be) *be = it->second; overlap++; return false; }
        if (it != m.begin()) {
            auto p = std::prev(it);
            if (p->second > s) { if (bs) *bs = p->first; if (be) *be = p->second; overlap++; return false; }
        }
        m.emplace(s, e);
        total += e - s;
        return true;
    }
    /* 原子注销 [s,e) 并插入 [s,s+off)、[s+off+cnt,e)（整段释放时 off=0,cnt=e-s）。 */
    bool split_free(uint64_t s, uint64_t e, uint64_t off, uint64_t cnt) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = m.find(s);
        if (it == m.end() || it->second != e) { overlap++; return false; }
        m.erase(it);
        total -= e - s;
        uint64_t a = s, b = s + off;
        if (b > a) { m.emplace(a, b); total += b - a; }
        uint64_t c = s + off + cnt, dd = e;
        if (dd > c) { m.emplace(c, dd); total += dd - c; }
        return true;
    }
    bool contains(uint64_t b) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = m.upper_bound(b);
        if (it == m.begin()) return false;
        --it;
        return b >= it->first && b < it->second;
    }
    uint64_t len() { std::lock_guard<std::mutex> lk(mu); return total; }
    bool empty() { std::lock_guard<std::mutex> lk(mu); return m.empty(); }
};

static Registry g_reg_b, g_reg_i;

/* ───────────────────────── 屏障（C++17 手搓） ───────────────────────── */
struct Barrier {
    std::mutex mu;
    std::condition_variable cv;
    uint32_t n, count = 0, gen = 0;
    explicit Barrier(uint32_t nn) : n(nn) {}
    void wait() {
        std::unique_lock<std::mutex> lk(mu);
        uint32_t g = gen;
        if (++count == n) { count = 0; gen++; cv.notify_all(); }
        else cv.wait(lk, [&] { return g != gen; });
    }
};

/* ───────────────────────── 几何推导（复刻 mgr 口径） ───────────────────────── */
static uint64_t pow2_le(uint64_t x) { uint64_t n = 1; while (n < x && n < (1ULL << 30)) n <<= 1; return n; }
static uint64_t per_of(uint64_t nbits, uint64_t sc) {
    uint64_t per = ((nbits + sc - 1) / sc + 32767) / 32768 * 32768;
    return per ? per : 32768;
}
static uint64_t EXPECT_SLICES, PER_BLK, PER_INO;
static void derive_geometry() {
    uint64_t n = pow2_le(CPU_CNT);
    uint64_t blk_cap = BLK_BITS / 32768, ino_cap = INO_BITS / 32768;
    uint64_t cap = blk_cap < ino_cap ? blk_cap : ino_cap;
    if (cap == 0) cap = 1;
    if (n > cap) n = cap;
    for (;;) {
        uint64_t pb = per_of(BLK_BITS, n), pi = per_of(INO_BITS, n);
        if ((n - 1) * pb < BLK_BITS && (n - 1) * pi < INO_BITS) break;
        if (n == 1) break;
        n >>= 1;
    }
    EXPECT_SLICES = n;
    PER_BLK = per_of(BLK_BITS, n);
    PER_INO = per_of(INO_BITS, n);
}

/* ───────────────────────── 负载生成器 ───────────────────────── */
static uint64_t gen_hard_count(std::mt19937_64 &r) {
    uint32_t c = (uint32_t)(r() % 100);
    if (c < 85) return 1 + r() % 64;
    if (c < 98) return 1 + r() % 2000;
    return 1 + r() % 100000;                 /* ≤ 单窗 262144 */
}
static uint64_t gen_soft_count(std::mt19937_64 &r) {
    uint32_t c = (uint32_t)(r() % 100);
    if (c < 80) return 1 + r() % 300;
    if (c < 98) return 1 + r() % 5000;
    return 1 + r() % 200000;                 /* ≤ 单窗 */
}
static uint64_t gen_occ_count(std::mt19937_64 &r) {
    uint32_t c = (uint32_t)(r() % 100);
    if (c < 70) return 1 + r() % 300;
    if (c < 92) return 1 + r() % 20000;
    if (c < 99) return 1 + r() % 300000;
    return 300000 + r() % 2000000;           /* 大跨度：跨多 chunk/片（上百万块）*/
}
static uint8_t gen_align(std::mt19937_64 &r) {
    if (r() % 100 < 80) return (uint8_t)(r() % 4);
    return (uint8_t)(r() % 16);
}
static uint64_t gen_occ_base(std::mt19937_64 &r) {
    uint32_t mode = (uint32_t)(r() % 10);
    if (mode < 6) return r() % BLK_BITS;                       /* 均匀 */
    if (mode < 8) {                                            /* 片界 ±ε */
        uint64_t k = 1 + r() % (EXPECT_SLICES - 1);
        uint64_t e = k * PER_BLK, d = r() % 2048;
        return e > d ? e - d : 0;
    }
    if (mode < 9) {                                            /* chunk 尾窗跨界 */
        uint64_t nc = BLK_BITS / BLK_CHUNK_BITS;
        uint64_t k = r() % nc;
        return k * BLK_CHUNK_BITS + BLK_CHUNK_BITS - (1 + r() % 2048);
    }
    return BLK_BITS - 1 - (r() % 4096);                        /* 整体末尾 */
}

/* ═══════════════════════════════════════════════════════════════════
 * 阶段 A：片层 chunks_asyn / calib 语义（mgr 不暴露 asyn → 片层专项）
 *   与写死几何同 order：block 片 order3（slice_bits=2^31，256MB 位图区）、
 *   inode 片 order0（slice_bits=2^29，64MB）。单线程随机 + 周期 asyn。
 * ═══════════════════════════════════════════════════════════════════ */
static void run_slice(const char *tag, Disk &d, uint64_t base_blk, uint64_t nblk,
                      uint8_t order, uint64_t chunk_bits, uint64_t seed, uint64_t nops) {
    g_phase = tag;
    const uint64_t sbits = nblk << 15;
    std::printf("[A:%s] order=%u blocks=%llu bits=%llu (%.0fMB) seed=%llu ops=%llu\n",
                tag, (unsigned)order, (unsigned long long)nblk, (unsigned long long)sbits,
                (double)sbits / 8.0 / 1048576.0, (unsigned long long)seed, (unsigned long long)nops);

    slice_soul_t soul{};
    soul.read_block = disk_read;
    soul.write_block = disk_write;
    soul.io_ctx = &d;
    soul.chunk_base_blk = base_blk;
    soul.chunk_blk_length = nblk;
    soul.chunk_order = order;

    chunk_slice_mgr_t s;
    CK(s.enable(&soul, 0) == RC_OK, "enable rc");
    CK(s.get_free_count() == sbits, "init free=%llu want %llu",
       (unsigned long long)s.get_free_count(), (unsigned long long)sbits);
    CK(region_zeros(d, base_blk, sbits) == sbits, "init disk not all-free");

    /* 单窗上限探针：hard = 单窗成功、> 单窗 → NO_BLOCK（不跨窗硬拼，契约已知）；
     * soft > 单窗 → OK + 缺口回传，实际认领被夹在单窗内（soft 本即部分分配）。 */
    {
        uint64_t out = 0, left = 0;
        CK(s.alloc_hard(0, chunk_bits, out) == RC_OK, "ceiling hard==chunk rc");
        CK(out == 0, "ceiling hard==chunk out=%llu", (unsigned long long)out);
        CK(s.free_interval(0, chunk_bits) == RC_OK, "ceiling free");
        CK(s.alloc_hard(0, chunk_bits + 1, out) == RC_ERR_NO_BLOCK, "ceiling hard>chunk");
        const result_code_t sfr = s.alloc_soft(0, chunk_bits + 1, out, left);
        CK(sfr == RC_OK, "ceiling soft>chunk rc=%d", (int)sfr);
        const uint64_t sgot = chunk_bits + 1 - left;
        CK(left >= 1 && sgot >= 1 && sgot <= chunk_bits, "ceiling soft got=%llu left=%llu",
           (unsigned long long)sgot, (unsigned long long)left);
        CK(s.free_interval(out, sgot) == RC_OK, "ceiling soft free");
    }

    Registry reg;
    std::vector<seg> lv; lv.reserve(MAXSEG);
    uint64_t live = 0;
    std::mt19937_64 r(seed);
    uint64_t c_ff = 0, c_hard = 0, c_soft = 0, c_occ = 0, c_free = 0, c_occinv = 0;

    for (uint64_t op = 0; op < nops; op++) {
        if (!lv.empty() && (lv.size() >= MAXSEG || live >= sbits / 20)) {
            c_ff++;
            /* 强制释放（lv 非空才走此路，否则必分配出首段）*/
            size_t idx = lv.empty() ? 0 : (size_t)(r() % lv.size());
            if (!lv.empty()) {
                seg e = lv[idx];
                bool whole = (r() % 3 != 0) || e.len <= 1;
                uint64_t off = 0, cnt = e.len;
                if (!whole) { cnt = 1 + r() % e.len; if (cnt > e.len) cnt = e.len; off = e.len > cnt ? r() % (e.len - cnt + 1) : 0; }
                CK(reg.split_free(e.start, e.start + e.len, off, cnt), "A split_free");
                lv[idx] = lv.back(); lv.pop_back();
                if (off > 0) lv.push_back({e.start, off});
                if (e.len - off - cnt > 0) lv.push_back({e.start + off + cnt, e.len - off - cnt});
                CK(s.free_interval(e.start + off, cnt) == RC_OK, "A free rc");
                live -= cnt;
            }
        } else {
            uint32_t z = (uint32_t)(r() % 100);
            if (lv.empty()) z = 0;           /* 无货必分配，避免空表取 index */
            if (z < 34) {                     /* alloc_hard */
                c_hard++;
                uint64_t cnt = gen_hard_count(r); uint8_t al = gen_align(r); uint64_t out = 0;
                if (s.alloc_hard(al, cnt, out) == RC_OK) {
                    const uint64_t am = (al ? (1ULL << al) : 1ULL) - 1;
                    CK((out & am) == 0, "A hard align");
                    CK(out + cnt <= sbits, "A hard range");
                    uint64_t bs = 0, be = 0;
                    CK(reg.insert(out, out + cnt, &bs, &be), "A hard dup out=%llu", (unsigned long long)out);
                    lv.push_back({out, cnt}); live += cnt;
                }
            } else if (z < 52) {              /* alloc_soft */
                c_soft++;
                uint64_t cnt = gen_soft_count(r), out = 0, left = 0;
                if (s.alloc_soft(0, cnt, out, left) == RC_OK) {
                    uint64_t got = cnt - left;
                    CK(got >= 1 && got <= cnt && out + got <= sbits, "A soft range");
                    uint64_t bs = 0, be = 0;
                    CK(reg.insert(out, out + got, &bs, &be), "A soft dup");
                    lv.push_back({out, got}); live += got;
                }
            } else if (z < 70) {              /* soft_occupy（大跨度主力）*/
                c_occ++;
                uint64_t cnt = gen_occ_count(r), rest = 0;
                uint64_t base = (r() % 4 == 0) ? (r() % sbits) : (gen_occ_base(r) % (sbits ? sbits : 1));
                if (base >= sbits) base = sbits - 1;
                uint64_t occ = s.soft_occupy(base, cnt, &rest);
                if (occ == RC_OK) {
                    uint64_t got = cnt - rest;
                    CK(got >= 1 && got <= cnt && base + got <= sbits, "A occ range");
                    uint64_t bs = 0, be = 0;
                    CK(reg.insert(base, base + got, &bs, &be), "A occ dup");
                    lv.push_back({base, got}); live += got;
                } else {
                    c_occinv++;
                    CK(rest == cnt, "A occ invalid rest");
                }
            } else {                          /* free */
                c_free++;
                size_t idx = (size_t)(r() % lv.size());
                seg e = lv[idx];
                bool whole = (r() % 2 == 0) || e.len <= 1;
                uint64_t off = 0, cnt = e.len;
                if (!whole) { cnt = 1 + r() % e.len; if (cnt > e.len) cnt = e.len; off = e.len > cnt ? r() % (e.len - cnt + 1) : 0; }
                CK(reg.split_free(e.start, e.start + e.len, off, cnt), "A free split");
                lv[idx] = lv.back(); lv.pop_back();
                if (off > 0) lv.push_back({e.start, off});
                if (e.len - off - cnt > 0) lv.push_back({e.start + off + cnt, e.len - off - cnt});
                CK(s.free_interval(e.start + off, cnt) == RC_OK, "A free rc");
                live -= cnt;
            }
        }

        CK(s.get_free_count() == sbits - live, "A agg free=%llu want %llu",
           (unsigned long long)s.get_free_count(), (unsigned long long)(sbits - live));
        CK(reg.len() == live, "A reg len=%llu live=%llu",
           (unsigned long long)reg.len(), (unsigned long long)live);

        if (op > 0 && op % 4000 == 0) {
            uint64_t pre = s.get_free_count();
            free_count_calib_t cal{};
            CK(s.chunks_asyn(&cal) == RC_OK, "A asyn rc");
            CK(cal.old_free_count == pre, "A calib.old=%llu pre=%llu",
               (unsigned long long)cal.old_free_count, (unsigned long long)pre);
            CK(cal.new_free_count == s.get_free_count(), "A calib.new vs fc");
            CK(cal.new_free_count == region_zeros(d, base_blk, sbits),
               "A calib.new=%llu vs disk %llu",
               (unsigned long long)cal.new_free_count,
               (unsigned long long)region_zeros(d, base_blk, sbits));
            CK(cal.old_free_count == cal.new_free_count, "A calib drift old=%llu new=%llu",
               (unsigned long long)cal.old_free_count, (unsigned long long)cal.new_free_count);
            /* 抽样对拍：live 区间在盘上置位、随机空闲位在盘上清零 */
            const uint8_t *p = d.data.data() + (size_t)base_blk * 4096;
            for (int t = 0; t < 24 && !lv.empty(); t++) {
                seg e = lv[(size_t)(r() % lv.size())];
                uint64_t b = e.start + r() % e.len;
                CK(((p[b >> 3] >> (b & 7)) & 1u) != 0, "A live bit %llu not set on disk", (unsigned long long)b);
            }
            for (int t = 0; t < 24; t++) {
                uint64_t b = r() % sbits;
                bool exp = reg.contains(b);
                bool act = (((p[b >> 3] >> (b & 7)) & 1u) != 0);
                CK(exp == act, "A sample bit %llu exp=%d act=%d", (unsigned long long)b, (int)exp, (int)act);
            }
        }
    }

    /* 全释放 → asyn → 盘全 0 */
    for (auto &e : lv) {
        CK(reg.split_free(e.start, e.start + e.len, 0, e.len), "A freeall");
        CK(s.free_interval(e.start, e.len) == RC_OK, "A freeall rc");
        live -= e.len;
    }
    lv.clear();
    CK(live == 0 && reg.empty(), "A freeall live=%llu", (unsigned long long)live);
    CK(s.get_free_count() == sbits, "A freeall fc");
    {
        free_count_calib_t cal{};
        CK(s.chunks_asyn(&cal) == RC_OK, "A final asyn");
        CK(cal.new_free_count == sbits, "A final calib.new=%llu", (unsigned long long)cal.new_free_count);
        CK(region_zeros(d, base_blk, sbits) == sbits, "A final disk not all-free");
    }

    /* 尾窗专项（仅当 slice_bits 非 chunk 整数倍）：末片尾窗 occupy/整窗抢占/free。 */
    if (sbits % chunk_bits != 0) {
        const uint64_t tailbase = (sbits / chunk_bits) * chunk_bits;
        uint64_t rest = 0;
        const result_code_t torc = s.soft_occupy(tailbase, sbits - tailbase, &rest);
        CK(torc == RC_OK && rest == 0, "A tail occupy rc=%llu rest=%llu",
           (unsigned long long)torc, (unsigned long long)rest);
        CK(s.test_interval(true, tailbase, sbits - tailbase) == RC_OK, "A tail bits set");
        CK(s.free_interval(tailbase, sbits - tailbase) == RC_OK, "A tail free");
        free_count_calib_t cal{};
        CK(s.chunks_asyn(&cal) == RC_OK, "A tail asyn");
        CK(s.get_free_count() == sbits && region_zeros(d, base_blk, sbits) == sbits, "A tail restore");
    }

    /* 人为重复 free → free_count 虚高 → asyn 校准检出漂移（old > new） */
    {
        uint64_t out = 0;
        CK(s.alloc_hard(0, 8, out) == RC_OK, "A dupfree alloc");
        uint64_t pre = s.get_free_count();
        CK(s.free_interval(out, 8) == RC_OK, "A dupfree #1");
        CK(s.free_interval(out, 8) == RC_OK, "A dupfree #2");
        CK(s.get_free_count() == pre + 16, "A dupfree inflate=%llu want %llu",
           (unsigned long long)s.get_free_count(), (unsigned long long)(pre + 16));
        free_count_calib_t cal{};
        CK(s.chunks_asyn(&cal) == RC_OK, "A drift asyn");
        CK(cal.old_free_count == pre + 16, "A drift calib.old=%llu", (unsigned long long)cal.old_free_count);
        CK(cal.new_free_count == region_zeros(d, base_blk, sbits), "A drift calib.new vs disk");
        CK(cal.old_free_count > cal.new_free_count, "A drift not detected");
    }

    CK(s.shutdown() == RC_OK, "A shutdown");
    CK(region_zeros(d, base_blk, sbits) == sbits, "A post-shutdown disk not all-free");
    std::printf("  [A:%s] 分支 ff=%llu hard=%llu soft=%llu occ=%llu(inv=%llu) free=%llu\n",
                tag, (unsigned long long)c_ff, (unsigned long long)c_hard,
                (unsigned long long)c_soft, (unsigned long long)c_occ,
                (unsigned long long)c_occinv, (unsigned long long)c_free);
}

/* ═══════════════════════════════════════════════════════════════════
 * 阶段 B：mgr 级 8 线程超高规格随机压测
 * ═══════════════════════════════════════════════════════════════════ */
static bitmap_slices_mgr_t_v2 *g_mgr = nullptr;
static Barrier                *g_bar = nullptr;
static std::atomic<uint64_t>   g_live_b[NTHREADS];
static std::atomic<uint64_t>   g_live_i[NTHREADS];
static std::mt19937_64         g_brng(0xB16B00B5C0FFEEULL);
/* 覆盖统计 */
static std::atomic<uint64_t> g_c_hard{0}, g_c_soft{0}, g_c_occ{0}, g_c_free{0};
static std::atomic<uint64_t> g_c_ino_a{0}, g_c_ino_f{0};
static std::atomic<uint64_t> g_c_bigocc{0};    /* occupy 认领 ≥ 100000 块 */
static std::atomic<uint64_t> g_maxocc{0};      /* occupy 单次最大认领 */

static void barrier_check() {
    uint64_t sb = 0, si = 0;
    for (uint32_t i = 0; i < NTHREADS; i++) {
        sb += g_live_b[i].load(std::memory_order_relaxed);
        si += g_live_i[i].load(std::memory_order_relaxed);
    }
    CK(g_mgr->block_free_total() == BLK_BITS - sb,
       "barrier blk free=%llu want %llu",
       (unsigned long long)g_mgr->block_free_total(), (unsigned long long)(BLK_BITS - sb));
    CK(g_mgr->inode_free_total() == INO_BITS - si,
       "barrier ino free=%llu want %llu",
       (unsigned long long)g_mgr->inode_free_total(), (unsigned long long)(INO_BITS - si));
    CK(g_reg_b.len() == sb, "barrier reg_b len=%llu live=%llu",
       (unsigned long long)g_reg_b.len(), (unsigned long long)sb);
    CK(g_reg_i.len() == si, "barrier reg_i len=%llu live=%llu",
       (unsigned long long)g_reg_i.len(), (unsigned long long)si);

    /* 逐点对拍（静默点：预期与实测稳定） */
    for (int t = 0; t < 64; t++) {
        uint64_t b = g_brng() % BLK_BITS;
        bool exp = g_reg_b.contains(b);
        CK(exp == g_mgr->block_test(b), "barrier blk_test(%llu) exp=%d", (unsigned long long)b, (int)exp);
    }
    for (int t = 0; t < 64; t++) {
        uint64_t j = g_brng() % INO_BITS;
        bool exp = g_reg_i.contains(j);
        CK(exp == g_mgr->inode_test(j), "barrier ino_test(%llu) exp=%d", (unsigned long long)j, (int)exp);
    }
    /* block_test 取样（前移，避免 printf 干扰）：已在上 */
}

static void worker_mgr(uint32_t id, uint64_t nops) {
    t_cpu = id;
    std::mt19937_64 r((uint64_t)0x9E3779B97F4A7C15ULL ^ ((uint64_t)(id + 1) * 2654435761u));
    std::vector<seg> lv; lv.reserve(MAXSEG);
    std::vector<uint64_t> li; li.reserve(MAXSEG);
    uint64_t lb = 0, lbi = 0;

    for (uint64_t op = 0; op < nops; op++) {
        if (op % BARRIER_EVERY == 0) {
            g_live_b[id].store(lb, std::memory_order_relaxed);
            g_live_i[id].store(lbi, std::memory_order_relaxed);
            g_bar->wait();                 /* 全体到齐（到达点） */
            if (id == 0) barrier_check();  /* 仅 0 号在全体停顿时点校验 */
            g_bar->wait();                 /* 第二道会合：他线程等校验完再走 */
        }

        bool force_free = (lv.size() >= MAXSEG) || (lb >= MAXLIVE);
        int kind;
        if (force_free) kind = 3;
        else {
            uint32_t z = (uint32_t)(r() % 100);
            kind = z < 30 ? 0 : z < 45 ? 1 : z < 60 ? 2 : z < 80 ? 3 : z < 93 ? 4 : 5;
        }
        if (kind <= 2 && lv.empty()) kind = 0;
        if (kind == 3 && lv.empty()) kind = 0;
        if (kind == 5 && li.empty()) kind = 4;

        if (kind == 0) {                          /* block_alloc_hard */
            uint64_t cnt = gen_hard_count(r);
            uint8_t al = gen_align(r);
            uint64_t g = g_mgr->block_alloc_hard(cnt, al);
            if (g != PTR_INVALID) {
                const uint64_t am = (al ? (1ULL << al) : 1ULL) - 1;
                CK((g & am) == 0, "B hard align a=%u g=%llu", (unsigned)al, (unsigned long long)g);
                CK(g + cnt <= BLK_BITS, "B hard range g=%llu c=%llu", (unsigned long long)g, (unsigned long long)cnt);
                uint64_t bs = 0, be = 0;
                CK(g_reg_b.insert(g, g + cnt, &bs, &be),
                   "B HARD DUP g=%llu c=%llu hit=[%llu,%llu) thr=%u",
                   (unsigned long long)g, (unsigned long long)cnt,
                   (unsigned long long)bs, (unsigned long long)be, (unsigned)id);
                lv.push_back({g, cnt}); lb += cnt;
                g_c_hard.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (kind == 1) {                   /* block_alloc_soft */
            uint64_t cnt = gen_soft_count(r), rem = 0;
            uint64_t g = g_mgr->block_alloc_soft(cnt, &rem);
            if (g != PTR_INVALID) {
                uint64_t got = cnt - rem;
                CK(got >= 1 && got <= cnt && g + got <= BLK_BITS, "B soft range");
                uint64_t bs = 0, be = 0;
                CK(g_reg_b.insert(g, g + got, &bs, &be),
                   "B SOFT DUP g=%llu got=%llu hit=[%llu,%llu) thr=%u",
                   (unsigned long long)g, (unsigned long long)got,
                   (unsigned long long)bs, (unsigned long long)be, (unsigned)id);
                lv.push_back({g, got}); lb += got;
                g_c_soft.fetch_add(1, std::memory_order_relaxed);
            } else {
                CK(rem == cnt, "B soft invalid rem=%llu c=%llu", (unsigned long long)rem, (unsigned long long)cnt);
            }
        } else if (kind == 2) {                   /* block_occupy_soft（大跨度主力）*/
            uint64_t base = gen_occ_base(r);
            uint64_t cnt = gen_occ_count(r), rest = 0;
            uint64_t occ = g_mgr->block_occupy_soft(base, cnt, &rest);
            if (occ != PTR_INVALID) {
                uint64_t got = cnt - rest;
                CK(occ == base, "B occ anchor %llu != %llu", (unsigned long long)occ, (unsigned long long)base);
                CK(got >= 1 && got <= cnt && base + got <= BLK_BITS, "B occ range base=%llu got=%llu",
                   (unsigned long long)base, (unsigned long long)got);
                uint64_t bs = 0, be = 0;
                CK(g_reg_b.insert(base, base + got, &bs, &be),
                   "B OCC DUP base=%llu got=%llu hit=[%llu,%llu) thr=%u",
                   (unsigned long long)base, (unsigned long long)got,
                   (unsigned long long)bs, (unsigned long long)be, (unsigned)id);
                lv.push_back({base, got}); lb += got;
                g_c_occ.fetch_add(1, std::memory_order_relaxed);
                if (got >= 100000) g_c_bigocc.fetch_add(1, std::memory_order_relaxed);
                uint64_t mo = g_maxocc.load(std::memory_order_relaxed);
                while (got > mo && !g_maxocc.compare_exchange_weak(mo, got, std::memory_order_relaxed)) {}
            } else {
                CK(rest == cnt, "B occ invalid rest=%llu c=%llu", (unsigned long long)rest, (unsigned long long)cnt);
            }
        } else if (kind == 3) {                   /* block_free（可子区间劈分）*/
            size_t idx = (size_t)(r() % lv.size());
            seg e = lv[idx];
            bool whole = (r() % 2 == 0) || e.len <= 1;
            uint64_t off = 0, cnt = e.len;
            if (!whole) { cnt = 1 + r() % e.len; if (cnt > e.len) cnt = e.len; off = e.len > cnt ? r() % (e.len - cnt + 1) : 0; }
            CK(g_reg_b.split_free(e.start, e.start + e.len, off, cnt), "B free split");
            lv[idx] = lv.back(); lv.pop_back();
            if (off > 0) lv.push_back({e.start, off});
            if (e.len - off - cnt > 0) lv.push_back({e.start + off + cnt, e.len - off - cnt});
            lb -= cnt;
            g_mgr->block_free(e.start + off, cnt);   /* 结构先注销、再还分配器 */
            g_c_free.fetch_add(1, std::memory_order_relaxed);
        } else if (kind == 4) {                   /* inode_alloc */
            uint64_t in = g_mgr->inode_alloc();
            if (in != PTR_INVALID) {
                CK(in < INO_BITS, "B ino range %llu", (unsigned long long)in);
                uint64_t bs = 0, be = 0;
                CK(g_reg_i.insert(in, in + 1, &bs, &be),
                   "B INO DUP in=%llu hit=[%llu,%llu) thr=%u",
                   (unsigned long long)in, (unsigned long long)bs, (unsigned long long)be, (unsigned)id);
                li.push_back(in); lbi += 1;
                g_c_ino_a.fetch_add(1, std::memory_order_relaxed);
            }
        } else {                                  /* inode_free */
            size_t idx = (size_t)(r() % li.size());
            uint64_t in = li[idx];
            CK(g_reg_i.split_free(in, in + 1, 0, 1), "B ino free split");
            li[idx] = li.back(); li.pop_back();
            lbi -= 1;
            g_mgr->inode_free(in);
            g_c_ino_f.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /* 收束：归还本线程全部持有 */
    for (auto &e : lv) {
        CK(g_reg_b.split_free(e.start, e.start + e.len, 0, e.len), "B freeall split");
        lb -= e.len;
        g_mgr->block_free(e.start, e.len);
    }
    lv.clear();
    for (uint64_t in : li) {
        CK(g_reg_i.split_free(in, in + 1, 0, 1), "B ino freeall split");
        lbi -= 1;
        g_mgr->inode_free(in);
    }
    li.clear();
    g_live_b[id].store(lb, std::memory_order_relaxed);
    g_live_i[id].store(lbi, std::memory_order_relaxed);
}

/* 全盘（位图区 = 全盘）直扫：全 0 位数 == 期望 */
static uint64_t disk_zero_bits(const Disk &d) {
    uint64_t z = 0;
    const size_t n = d.data.size();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w; std::memcpy(&w, d.data.data() + i, 8);
        z += 64 - (uint64_t)__builtin_popcountll(w);
    }
    for (; i < n; i++) z += 8 - (uint64_t)__builtin_popcount((unsigned)d.data[i]);
    return z;
}

int main(int argc, char **argv) {
    uint64_t seed = (argc > 1) ? std::strtoull(argv[1], nullptr, 0) : 0x5EEDB16ULL;
    uint64_t ops  = (argc > 2) ? std::strtoull(argv[2], nullptr, 0) : 80000ULL;
    derive_geometry();
    std::printf("=== stress_v2_random_big seed=%llu threads=%u ops/thread=%llu ===\n",
                (unsigned long long)seed, NTHREADS, (unsigned long long)ops);
    std::printf("几何: blk_bits=%llu ino_bits=%llu slices=%llu per_blk=%llu per_ino=%llu "
                "blk_chunk=%llu ino_chunk=%llu disk=%.2fGB\n",
                (unsigned long long)BLK_BITS, (unsigned long long)INO_BITS,
                (unsigned long long)EXPECT_SLICES, (unsigned long long)PER_BLK,
                (unsigned long long)PER_INO, (unsigned long long)BLK_CHUNK_BITS,
                (unsigned long long)INO_CHUNK_BITS,
                (double)TOTAL_BLOCKS * 4096.0 / 1e9);
    CK(EXPECT_SLICES == 8, "derive slices=%llu (want 8)", (unsigned long long)EXPECT_SLICES);

    auto t0 = std::chrono::steady_clock::now();
    Disk disk;
    disk.data.assign((size_t)TOTAL_BLOCKS * 4096, 0);   /* 2.68GB 单份 */
    std::printf("mock 盘 %.2fGB 分配完成 (%.1fs)\n",
                (double)disk.data.size() / 1e9,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

    /* ── 阶段 A：片层 asyn/calib（复用大盘子区，不另副本）── */
    g_phase = "A";
    run_slice("blk-slice", disk, 0, BLK_BLOCKS / EXPECT_SLICES, BLK_ORDER,
              BLK_CHUNK_BITS, seed + 11, 20000);
    run_slice("ino-slice", disk, INO_BASE, INO_BLOCKS / EXPECT_SLICES, INO_ORDER,
              INO_CHUNK_BITS, seed + 22, 20000);
    /* 尾窗专项几何：100 块 / order=3（chunk=8 块）⇒ 尾窗 = 4 块（非整数倍）*/
    run_slice("blk-tail", disk, BLK_BLOCKS - 100, 100, BLK_ORDER,
              BLK_CHUNK_BITS, seed + 33, 8000);
    /* 子区（若残留）清零，保证 mgr ctor 读到全 0 */
    std::memset(disk.data.data(), 0, (size_t)(BLK_BLOCKS / EXPECT_SLICES) * 4096);
    std::memset(disk.data.data() + (size_t)(BLK_BLOCKS - 100) * 4096, 0, 100 * 4096);
    std::memset(disk.data.data() + (size_t)INO_BASE * 4096, 0,
                (size_t)(INO_BLOCKS / EXPECT_SLICES) * 4096);

    /* ── 阶段 B：mgr 级 8 线程 ── */
    g_phase = "B";
    {
        bitmap_slices_v2cfg_t cfg{};
        cfg.read_block = disk_read;
        cfg.write_block = disk_write;
        cfg.io_ctx = &disk;
        cfg.block_bitmap_base_FS_block = 0;
        cfg.block_bitmap_blocks = BLK_BLOCKS;
        cfg.inode_bitmap_base_FS_block = INO_BASE;
        cfg.inode_bitmap_blocks = INO_BLOCKS;
        cfg.inobm_chunk_order = INO_ORDER;
        cfg.blkbm_chunk_order = BLK_ORDER;
        cfg.processor_count = CPU_CNT;

        auto tb = std::chrono::steady_clock::now();
        uint64_t rc = 0;
        bitmap_slices_mgr_t_v2 mgr(cfg, &rc);
        CK(rc == (uint64_t)RC_OK, "B ctor rc=%llu", (unsigned long long)rc);
        CK(mgr.slice_count() == EXPECT_SLICES, "B slice_count=%llu", (unsigned long long)mgr.slice_count());
        CK(mgr.block_free_total() == BLK_BITS, "B init blk free=%llu", (unsigned long long)mgr.block_free_total());
        CK(mgr.inode_free_total() == INO_BITS, "B init ino free=%llu", (unsigned long long)mgr.inode_free_total());
        std::printf("B ctor 完成 (%.1fs) slices=%llu blk_free=%llu ino_free=%llu\n",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - tb).count(),
                    (unsigned long long)mgr.slice_count(),
                    (unsigned long long)mgr.block_free_total(),
                    (unsigned long long)mgr.inode_free_total());

        g_mgr = &mgr;
        Barrier bar(NTHREADS);
        g_bar = &bar;
        for (uint32_t i = 0; i < NTHREADS; i++) { g_live_b[i].store(0); g_live_i[i].store(0); }

        auto ts = std::chrono::steady_clock::now();
        std::vector<std::thread> th;
        for (uint32_t i = 0; i < NTHREADS; i++) th.emplace_back(worker_mgr, i, ops);
        for (auto &x : th) x.join();
        std::printf("B 压测结束 %llu 操作 (%.1fs)\n",
                    (unsigned long long)(NTHREADS * ops),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count());

        /* 收尾审计（静默点） */
        uint64_t sb = 0, si = 0;
        for (uint32_t i = 0; i < NTHREADS; i++) { sb += g_live_b[i].load(); si += g_live_i[i].load(); }
        CK(sb == 0 && si == 0, "B residual live blk=%llu ino=%llu", (unsigned long long)sb, (unsigned long long)si);
        CK(g_reg_b.empty() && g_reg_i.empty(), "B registry not empty blk=%llu ino=%llu",
           (unsigned long long)g_reg_b.len(), (unsigned long long)g_reg_i.len());
        CK(mgr.block_free_total() == BLK_BITS, "B after-free blk free=%llu want %llu (⑦)",
           (unsigned long long)mgr.block_free_total(), (unsigned long long)BLK_BITS);
        CK(mgr.inode_free_total() == INO_BITS, "B after-free ino free=%llu want %llu (⑦)",
           (unsigned long long)mgr.inode_free_total(), (unsigned long long)INO_BITS);
        for (int t = 0; t < 2000; t++) {
            uint64_t b = g_brng() % BLK_BITS, j = g_brng() % INO_BITS;
            CK(!mgr.block_test(b), "B after-free blk_test(%llu)=true (⑦)", (unsigned long long)b);
            CK(!mgr.inode_test(j), "B after-free ino_test(%llu)=true (⑦)", (unsigned long long)j);
        }
    }   /* dtor：逐片 shutdown → 合并落盘 */

    /* ── 阶段 C：dtor 后全盘位图区直扫全 0 ── */
    g_phase = "C";
    auto tc = std::chrono::steady_clock::now();
    uint64_t zeros = disk_zero_bits(disk);
    CK(zeros == (BLK_BITS + INO_BITS), "C disk not all-free: set=%llu (want 0)",
       (unsigned long long)(BLK_BITS + INO_BITS - zeros));
    std::printf("覆盖: hard=%llu soft=%llu occ=%llu(≥1e5块:%llu max=%llu) free=%llu ino_a=%llu ino_f=%llu\n",
                (unsigned long long)g_c_hard.load(), (unsigned long long)g_c_soft.load(),
                (unsigned long long)g_c_occ.load(), (unsigned long long)g_c_bigocc.load(),
                (unsigned long long)g_maxocc.load(), (unsigned long long)g_c_free.load(),
                (unsigned long long)g_c_ino_a.load(), (unsigned long long)g_c_ino_f.load());
    std::printf("C 全盘直扫: set_bits=%llu (%.1fs) rd=%llu wr=%llu io_err=%llu\n",
                (unsigned long long)(BLK_BITS + INO_BITS - zeros),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - tc).count(),
                (unsigned long long)disk.rd.load(), (unsigned long long)disk.wr.load(),
                (unsigned long long)disk.io_err.load());

    double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("总耗时 %.1fs  失败=%d\n", total, g_fail.load());
    if (g_fail.load() == 0) { std::printf("ALL PASS\n"); return 0; }
    std::printf("FAILED: %d\n", g_fail.load());
    return 1;
}

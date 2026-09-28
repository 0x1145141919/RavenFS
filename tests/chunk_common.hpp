/* ═══════════════════════════════════════════════════════════════════
 * chunk_common.hpp — Phase A 三个新测试共享的靶场轮子（2026-09-28）
 *
 * 供给：test_bitmap_slice_differential.cpp（FULL↔CHUNK 对拍）
 *       test_bitmap_slice_churn.cpp      （并发 churn 影子对账）
 *       test_bitmap_slice_invariants.cpp （§10 缺口专项）
 *
 * 内容：计数断言宏 / 分段宏 · xorshift 随机 · 位图镜像小工具 ·
 *       mock chunk_io 盘（计数 + §10.3 审计 + I/O 门闸）·
 *       几何推导（复刻 mgr ctor 的 slice_count/per_slice 公式）·
 *       窗口表 + 合成初值生成（margin 纪律，见下）· side_t 双模式装配
 *
 * margin 纪律（对拍等价性的前提，Phase A-1 设计约定）：
 *   block 组初值与随机 free 一律把每个 chunk 窗首/尾各 64 bit 置 1、
 *   free 区间严格落在 [wlo+64, whi−64) ⇒ **任何 0-run 不跨窗**。
 *   依据：FULL 的段扫描是整片连续、CHUNK 按窗扫描——0-run 跨窗时
 *   多位 alloc 的段延伸/对齐候选两边可能取到不同位置（§8 单窗 find 的
 *   已知形态差异）。occupy/free/test/count 不需要该约束（本就逐位等价）。
 *   约束测试而非实现——实现侧跨窗行为差异若属 bug，由 churn/影子对账抓。
 *
 * 只读红线：不 include/修改任何实现细节之外的东西；实现文件只读。
 * ═══════════════════════════════════════════════════════════════════ */
#pragma once
#include "../include/ravenfs/bitmap_slices.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

/* ── 计数断言（atomic：并发段允许直接 CHECK） ── */
inline std::atomic<int> g_pass{0}, g_fail{0};
#define CHECK(cond, ...) do {                                                      \
    if (cond) { g_pass++; }                                                        \
    else { g_fail++; std::printf("  [FAIL] "); std::printf(__VA_ARGS__);           \
           std::printf("\n"); }                                                    \
} while (0)
#define SECTION(name) std::printf("\n-- %s --\n", name)

/* 平台身份（外部合约）：多线程分片压测强定义覆盖 weak 桩（同
 * test_extent_fusion.cpp 的写法）；单线程段恒 0。 */
inline thread_local uint32_t t_fake_proc_id = 0;
extern "C" uint32_t get_processor_id() { return t_fake_proc_id; }

/* ── xorshift64* 随机（确定性、跨平台同种子同序列——对拍要求） ── */
struct rng_t {
    uint64_t s;
    explicit rng_t(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        uint64_t x = (s += 0x9E3779B97F4A7C15ull);
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }
    uint64_t below(uint64_t n) { return n ? next() % n : 0; }
};

/* ── 位图镜像小工具（字节位图，bit i ↔ byte[i/8] bit i%8） ── */
inline bool img_bit(const uint8_t *img, uint64_t i) {
    return (img[i / 8] >> (i % 8)) & 1u;
}
inline void img_set(uint8_t *img, uint64_t i) { img[i / 8] |= (uint8_t)(1u << (i % 8)); }
inline void img_clr(uint8_t *img, uint64_t i) { img[i / 8] &= (uint8_t)~(1u << (i % 8)); }
inline uint64_t img_zeros(const uint8_t *img, uint64_t bytes) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < bytes * 8; i++) n += !img_bit(img, i);
    return n;
}
/* 影子侧：区间 [base,base+len) 内为 1 的位数（free 的 oracle 增量） */
inline uint64_t shadow_ones(const uint8_t *img, uint64_t base, uint64_t len) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < len; i++) if (img_bit(img, base + i)) n++;
    return n;
}
/* occupy oracle：从 base 起的贪心连续空闲前缀（≤ len） */
inline uint64_t shadow_greedy(const uint8_t *img, uint64_t base, uint64_t len) {
    uint64_t k = 0;
    while (k < len && !img_bit(img, base + k)) k++;
    return k;
}

/* ═══ mock chunk_io 盘：字节位图当盘 + I/O 计数 + §10.3 审计 + 门闸 ═══ */
struct mock_disk_t {
    uint8_t *blk = nullptr;
    uint8_t *ino = nullptr;
    uint64_t blk_bytes = 0, ino_bytes = 0;
    std::atomic<uint64_t> reads{0}, writes{0}, read_blocks{0}, write_blocks{0};
    std::atomic<bool> lock_violation{false}, shape_violation{false};
    /* I/O 门闸（§10.3/§5 时序专项用；不武装时零开销穿过） */
    std::atomic<bool> gate_armed{false}, gate_open{false};
    std::atomic<int> in_write{0};
    std::mutex gate_mu;
    std::condition_variable gate_cv;

    void alloc_img(uint64_t blk_bits, uint64_t ino_bits) {
        blk_bytes = blk_bits / 8;
        ino_bytes = ino_bits / 8;
        blk = new uint8_t[blk_bytes]();
        ino = new uint8_t[ino_bytes]();
    }
    ~mock_disk_t() { delete[] blk; delete[] ino; }

    static bool shape_ok(const mock_disk_t *d, bool is_inode, uint64_t rel, uint64_t nb) {
        const uint64_t total = is_inode ? d->ino_bytes : d->blk_bytes;
        return nb > 0 && (rel + nb) * 4096 <= total;
    }
    static int cb_read(void *ctx, bool is_inode, uint64_t rel, void *buf, uint64_t nb) {
        auto *d = static_cast<mock_disk_t *>(ctx);
        if (ravenfs_bitmap_in_slice_lock()) d->lock_violation = true;   /* §10.3 铁律 */
        d->reads++;
        if (!shape_ok(d, is_inode, rel, nb)) { d->shape_violation = true; return -1; }
        std::memcpy(buf, (is_inode ? d->ino : d->blk) + rel * 4096, nb * 4096);
        d->read_blocks += nb;
        return 0;
    }
    static int cb_write(void *ctx, bool is_inode, uint64_t rel, const void *buf, uint64_t nb) {
        auto *d = static_cast<mock_disk_t *>(ctx);
        if (ravenfs_bitmap_in_slice_lock()) d->lock_violation = true;   /* §10.3 铁律 */
        d->writes++;
        if (!shape_ok(d, is_inode, rel, nb)) { d->shape_violation = true; return -1; }
        if (d->gate_armed.load(std::memory_order_acquire) &&
            !d->gate_open.load(std::memory_order_acquire)) {
            /* 时序专项：把写钉在盘回调里，模拟 ~100µs 级写盘在途（10s 上限防吊死） */
            d->in_write++;
            std::unique_lock<std::mutex> lk(d->gate_mu);
            d->gate_cv.wait_for(lk, std::chrono::seconds(10),
                                [&] { return d->gate_open.load(std::memory_order_acquire); });
            d->in_write--;
        }
        std::memcpy((is_inode ? d->ino : d->blk) + rel * 4096, buf, nb * 4096);
        d->write_blocks += nb;
        return 0;
    }
};
inline const bitmap_chunk_io_t g_mock_io{ mock_disk_t::cb_read, mock_disk_t::cb_write };

/* ═══ 几何：复刻 mgr ctor / setup_group 的推导（对拍侧用同一公式预计算
 *    窗口表与初值——若实现推导与公式不符，mount 后的 slice_count 断言会抓）═══ */
struct geom_t {
    const char *name;
    uint64_t blk_bits, ino_bits;   /* 须为 32768 倍数（1 位图页粒度，§3） */
    uint32_t cpu;                  /* logical_processor_count（2^n 上界 ∩ 满片数约束） */
    uint8_t order;                 /* chunk = 4K<<order 字节；0..8 */
};

inline uint64_t derive_slice_count(const geom_t &g) {
    uint64_t n = 1;
    while (n < g.cpu && n < (1ULL << 30)) n <<= 1;
    const uint64_t blk_cap = g.blk_bits / 32768, ino_cap = g.ino_bits / 32768;
    uint64_t cap = blk_cap < ino_cap ? blk_cap : ino_cap;
    if (cap == 0) cap = 1;
    if (n > cap) n = cap;
    for (;;) {
        uint64_t per_b = ((g.blk_bits + n - 1) / n + 32767) / 32768 * 32768;
        uint64_t per_i = ((g.ino_bits + n - 1) / n + 32767) / 32768 * 32768;
        if (per_b == 0) per_b = 32768;
        if (per_i == 0) per_i = 32768;
        if ((n - 1) * per_b < g.blk_bits && (n - 1) * per_i < g.ino_bits) break;
        if (n == 1) break;
        n >>= 1;
    }
    return n;
}
inline uint64_t derive_per_slice(uint64_t nbits, uint64_t slice_count) {
    uint64_t per = ((nbits + slice_count - 1) / slice_count + 32767) / 32768 * 32768;
    return per ? per : 32768;
}

struct win_t { uint64_t lo, hi; };   /* 全局位号窗口 [lo, hi) */
inline void build_windows(uint64_t nbits, uint64_t per, uint64_t slice_count,
                          uint64_t chunk_bits, std::vector<win_t> &out) {
    out.clear();
    for (uint64_t s = 0; s < slice_count; s++) {
        const uint64_t begin = s * per;
        if (begin >= nbits) break;
        uint64_t cnt = nbits - begin;
        if (cnt > per) cnt = per;
        for (uint64_t w = 0; w < cnt; w += chunk_bits) {
            uint64_t hi = w + chunk_bits;
            if (hi > cnt) hi = cnt;
            out.push_back({begin + w, begin + hi});
        }
    }
}

constexpr uint64_t PAGE_BITS = 32768;
constexpr uint64_t WIN_MARGIN = 64;   /* margin 纪律：窗首/尾各 64 bit 恒 1 */

/* 合成初值：逐窗选密度（1.0 / 0.75 / 0.5 / 0.25）+ margin 恒 1。
 * density=1 的窗 = 摘要 ALL_USED（喂 §10.5 occupy 摘要跳过路径）。 */
inline void gen_init(const geom_t &g, uint64_t seed, std::vector<uint8_t> &b,
                     std::vector<uint8_t> &i) {
    const uint64_t sc = derive_slice_count(g);
    const uint64_t per_b = derive_per_slice(g.blk_bits, sc);
    const uint64_t per_i = derive_per_slice(g.ino_bits, sc);
    const uint64_t chunk_bits = PAGE_BITS << g.order;
    std::vector<win_t> wb, wi;
    build_windows(g.blk_bits, per_b, sc, chunk_bits, wb);
    build_windows(g.ino_bits, per_i, sc, chunk_bits, wi);
    rng_t rng(seed);
    auto fill = [&](std::vector<uint8_t> &img, uint64_t bits, const std::vector<win_t> &ws) {
        img.assign(bits / 8, 0);
        for (const win_t &w : ws) {
            const uint64_t dsel = rng.below(4);          /* 0→全1 1→75% 2→50% 3→25% */
            const uint64_t den = (dsel == 2) ? 2 : 4;
            const uint64_t thr = (dsel == 1) ? 3 : (dsel == 2) ? 1 : 1;  /* den 内 <thr 概率 */
            for (uint64_t p = w.lo; p < w.hi; p++) {
                const bool margin = (p - w.lo) < WIN_MARGIN || (w.hi - p) <= WIN_MARGIN;
                if (margin || dsel == 0) { img_set(img.data(), p); continue; }
                if (rng.next() % den < thr) img_set(img.data(), p);
            }
        }
    };
    fill(b, g.blk_bits, wb);
    fill(i, g.ino_bits, wi);
    img_set(i.data(), 0);                                 /* root 亮灯（即便 skip 也保真） */
}

/* ═══ side_t：一个模式的完整装配（FULL=常驻 raw / CHUNK=mock 盘）═══ */
struct side_t {
    bool chunked = false;
    uint8_t order = 0;
    uint64_t nbits_b = 0, nbits_i = 0;
    mock_disk_t disk;                        /* CHUNK：盘 */
    uint8_t *raw_b = nullptr, *raw_i = nullptr;  /* FULL：常驻位图（即对拍镜像） */
    bitmap_slices_mgr_t *mgr = nullptr;
    uint64_t rc = RC_ERR_STATE;

    ~side_t() {
        delete mgr;
        free(raw_b);
        free(raw_i);
    }
    bool mount(const geom_t &g, const std::vector<uint8_t> &init_b,
               const std::vector<uint8_t> &init_i, bool want_chunk,
               uint32_t pool_depth = 64) {
        chunked = want_chunk;
        order = g.order;
        nbits_b = g.blk_bits;
        nbits_i = g.ino_bits;
        bitmap_slices_cfg_t cfg;
        cfg.block_bits_count = g.blk_bits;
        cfg.inode_bits_count = g.ino_bits;
        cfg.logical_processor_count = g.cpu;
        if (chunked) {
            disk.alloc_img(g.blk_bits, g.ino_bits);
            std::memcpy(disk.blk, init_b.data(), init_b.size());
            std::memcpy(disk.ino, init_i.data(), init_i.size());
            cfg.block_bits_raw = nullptr;      /* 裁决点：CHUNK 全图不驻留 */
            cfg.inode_bits_raw = nullptr;
            cfg.backing_mode = BITMAP_BACKING_CHUNK;
            cfg.chunk_order = order;
            cfg.chunk_io = &g_mock_io;
            cfg.chunk_io_ctx = &disk;
            cfg.rec_pool_depth = pool_depth;
            cfg.skip_self_check = true;        /* 合成位图 */
        } else {
            if (posix_memalign((void **)&raw_b, 4096, init_b.size()) != 0) return false;
            if (posix_memalign((void **)&raw_i, 4096, init_i.size()) != 0) return false;
            std::memcpy(raw_b, init_b.data(), init_b.size());
            std::memcpy(raw_i, init_i.data(), init_i.size());
            cfg.block_bits_raw = raw_b;
            cfg.inode_bits_raw = raw_i;
        }
        mgr = new bitmap_slices_mgr_t(cfg, &rc);
        return rc == RC_OK;
    }
    /* 模式可见状态镜像：FULL=常驻 raw；CHUNK=mock 盘（TTL 后 ≡ 逻辑态） */
    uint8_t *img(bool is_inode) {
        if (chunked) return is_inode ? disk.ino : disk.blk;
        return is_inode ? raw_i : raw_b;
    }
    uint64_t bytes(bool is_inode) const { return is_inode ? nbits_i / 8 : nbits_b / 8; }
    uint64_t free_total(bool is_inode) const {
        return is_inode ? mgr->inode_free_total() : mgr->block_free_total();
    }
    void finish() { if (chunked) mgr->chunk_ttl_sync(); }   /* TTL 收尾（§9） */
    void audit(const char *what) {
        CHECK(!disk.lock_violation, "%s: §10.3 盘 I/O 未进 slice_lock", what);
        CHECK(!disk.shape_violation, "%s: chunk_io 块形状合法（4K 粒度/不越界）", what);
    }
};

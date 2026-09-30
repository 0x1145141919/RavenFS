/* ═══════════════════════════════════════════════════════════════════
 * stress_v2_concurrent.cpp — bitmap_slices_v2 并发压测 + 竞态检测（独立 main）
 *
 * 2026-09-30。只读被测实现，不改其一行/头/CMake/既有测试；自带 mock 盘
 * （计数用原子，避免自造 data race）。手工编译：
 *   g++ -std=c++17 -Wall -Wextra -Iinclude/ravenfs \
 *       tests/stress_v2_concurrent.cpp juvenile_god/bitmap_slices_v2.cpp \
 *       -o /tmp/v2conc && /tmp/v2conc
 *
 * 阶段：
 *   P1  mgr 级：4 线程 × 独立 fake cpu（get_processor_id 桩按线程返回），
 *       混合 block_alloc_hard/soft/free/occupy_soft + inode_alloc/free；
 *       全局影子集合判 alloc 返回块号全局唯一；终局对账（free_total）。
 *   P2  slice 级：单个 chunk_slice_mgr_t + 4 线程，统计 RC_ERR_BUSY；
 *       另设 BUSY 定向压测（asyn 慢 I/O 持锁 → alloc try_lock 必 BUSY），
 *       确认换片/忙路径被覆盖。
 *
 * 有界纪律：每线程操作数封顶；所有循环带硬上限；主进程墙钟看门狗
 * （60s）超时打印 TIMEOUT 并以非 0 退出（_Exit 保证即使线程死锁也退出）。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/bitmap_slices_v2.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

/* ── 全局失败计数 / 统计 ── */
static int g_fail = 0;
static std::atomic<uint64_t> g_dup{0};        /* 全局重复分配次数（竞态证据） */
static std::atomic<uint64_t> g_busy_hard{0};  /* P2 alloc_hard 的 RC_ERR_BUSY */
static std::atomic<uint64_t> g_busy_soft{0};  /* P2 alloc_soft 的 RC_ERR_BUSY */
static std::chrono::steady_clock::time_point g_deadline;
static std::atomic<bool> g_done{false};

#define CK(cond, ...)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_fail;                                                        \
            std::printf("  [FAIL] %s:%d ", __FILE__, __LINE__);              \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
            std::fflush(stdout);                                             \
        }                                                                    \
    } while (0)

/* ── 墙钟看门狗：超时硬退出（防止任何死锁使进程悬挂）── */
static void watchdog(int sec) {
    std::thread th([sec] {
        auto t0 = std::chrono::steady_clock::now();
        while (!g_done.load(std::memory_order_relaxed)) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(sec)) {
                std::printf("TIMEOUT: exceeded %ds wall clock\n", sec);
                std::fflush(stdout);
                std::_Exit(2);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    th.detach();
}

/* ═══ 线程安全 mock 盘（绝对 FS_block 寻址；多线程仅访问互不相交的块区，
 *     计数器用原子避免自造 race）═══ */
struct stress_disk_t {
    std::vector<uint8_t> data;
    std::atomic<uint64_t> rd{0}, wr{0};
    unsigned slow_us = 0;   /* >0：每次 I/O 前慢睡（用来拉长持锁窗口） */
};
static result_code_t sd_read(void *ctx, uint64_t blk, uint64_t len, void *buf) {
    stress_disk_t *d = static_cast<stress_disk_t *>(ctx);
    if (len == 0 || blk + len > (uint64_t)(d->data.size() / 4096)) return RC_ERR_IO;
    if (d->slow_us) std::this_thread::sleep_for(std::chrono::microseconds(d->slow_us));
    std::memcpy(buf, &d->data[(size_t)blk * 4096], (size_t)len * 4096);
    d->rd.fetch_add(1, std::memory_order_relaxed);
    return RC_OK;
}
static result_code_t sd_write(void *ctx, uint64_t blk, uint64_t len, const void *buf) {
    stress_disk_t *d = static_cast<stress_disk_t *>(ctx);
    if (len == 0 || blk + len > (uint64_t)(d->data.size() / 4096)) return RC_ERR_IO;
    if (d->slow_us) std::this_thread::sleep_for(std::chrono::microseconds(d->slow_us));
    std::memcpy(&d->data[(size_t)blk * 4096], buf, (size_t)len * 4096);
    d->wr.fetch_add(1, std::memory_order_relaxed);
    return RC_OK;
}

/* ── get_processor_id 桩：按线程返回独立 fake cpu（线程局部）── */
static thread_local uint32_t t_cpu = 0;
extern "C" uint32_t get_processor_id() { return t_cpu; }

/* ── 全局影子集合：逐位认领/释放；认领命中既有位 = 重复分配（竞态证据）── */
struct shadow_t {
    std::mutex mu;
    std::vector<uint8_t> bits;    /* 0/1 */
    std::vector<uint8_t> owner;   /* 持有线程；255 = 空闲 */
    explicit shadow_t(uint64_t n) : bits((size_t)n, 0), owner((size_t)n, 255) {}
    bool claim(uint64_t base, uint64_t cnt, int caller, uint64_t *bad, int *owner_out) {
        std::lock_guard<std::mutex> lk(mu);
        for (uint64_t i = 0; i < cnt; i++)
            if (bits[(size_t)(base + i)]) {
                if (bad) *bad = base + i;
                if (owner_out) *owner_out = owner[(size_t)(base + i)];
                g_dup.fetch_add(1);
                return false;
            }
        for (uint64_t i = 0; i < cnt; i++) {
            bits[(size_t)(base + i)] = 1;
            owner[(size_t)(base + i)] = (uint8_t)caller;
        }
        return true;
    }
    void release(uint64_t base, uint64_t cnt) {
        std::lock_guard<std::mutex> lk(mu);
        for (uint64_t i = 0; i < cnt; i++) {
            bits[(size_t)(base + i)] = 0;
            owner[(size_t)(base + i)] = 255;
        }
    }
    uint64_t live() {
        std::lock_guard<std::mutex> lk(mu);
        uint64_t c = 0;
        for (uint8_t b : bits) c += b;
        return c;
    }
};
static void report_dup(const char *tag, uint64_t base, uint64_t cnt, uint64_t bad,
                       int owner, int caller) {
    if (g_dup.load() <= 8)
        std::printf("[RACE] %s double-alloc base=%llu cnt=%llu offending=%llu "
                    "owner_thread=%d caller=%d\n",
                    tag, (unsigned long long)base, (unsigned long long)cnt,
                    (unsigned long long)bad, owner, caller);
}

struct rec_t { uint64_t base, cnt; };

/* ═══ P1：mgr 级并发 worker ═══ */
static const uint64_t BLK_TOTAL = 8ULL << 15;   /* block 组 262144 位（8 块） */
static const uint64_t INO_TOTAL = 4ULL << 15;   /* inode 组 131072 位（4 块） */

static void worker_mgr(bitmap_slices_mgr_t_v2 *mgr, uint32_t cpu, int ops,
                       shadow_t *bs, shadow_t *is) {
    t_cpu = cpu;
    std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ ((uint64_t)(cpu + 1) * 2654435761u));
    std::vector<rec_t> br, ir;
    br.reserve(640); ir.reserve(320);
    for (int i = 0; i < ops; i++) {
        if ((i & 63) == 0 && std::chrono::steady_clock::now() > g_deadline) break;
        uint64_t r = rng() % 100;
        if (br.size() >= 512) r = 85;        /* 本地积压过多 → 强制归还 */
        if (br.empty()) r = 0;               /* 无货 → 强制分配 */

        if (r < 42) {                        /* block_alloc_hard */
            uint64_t cnt = 1 + rng() % 8;
            uint8_t al = (uint8_t)(rng() % 4);
            uint64_t b = mgr->block_alloc_hard(cnt, al);
            if (b != PTR_INVALID) {
                uint64_t bad = 0; int ow = -1;
                if (!bs->claim(b, cnt, (int)cpu, &bad, &ow)) report_dup("P1.hard", b, cnt, bad, ow, (int)cpu);
                br.push_back({b, cnt});
            }
        } else if (r < 60) {                 /* block_alloc_soft */
            uint64_t req = 1 + rng() % 16, rem = 0;
            uint64_t b = mgr->block_alloc_soft(req, &rem);
            if (b != PTR_INVALID) {
                uint64_t got = req - rem;
                if (got) {
                    uint64_t bad = 0; int ow = -1;
                    if (!bs->claim(b, got, (int)cpu, &bad, &ow)) report_dup("P1.soft", b, got, bad, ow, (int)cpu);
                    br.push_back({b, got});
                }
            }
        } else if (r < 74) {                 /* block_occupy_soft */
            uint64_t base = rng() % BLK_TOTAL, req = 1 + rng() % 8, rest = 0;
            uint64_t b = mgr->block_occupy_soft(base, req, &rest);
            if (b != PTR_INVALID) {
                uint64_t got = req - rest;
                if (got) {
                    uint64_t bad = 0; int ow = -1;
                    if (!bs->claim(b, got, (int)cpu, &bad, &ow)) report_dup("P1.occ", b, got, bad, ow, (int)cpu);
                    br.push_back({b, got});
                }
            }
        } else if (r < 90) {                 /* block_free */
            if (!br.empty()) {
                rec_t x = br.back(); br.pop_back();
                /* 2026-09-30 修：影子先释放、再调分配器释放。
                 * 若反序（free 先、release 后），会出现「分配器已空闲、影子仍占用」的
                 * 窗口 → 另线程合法重新分配该块后 claim 撞影子置位 → 假 RACE。
                 * 影子语义 = 分配器活块集，须在分配器转空闲**不晚于**其时刻转空闲。 */
                bs->release(x.base, x.cnt);
                mgr->block_free(x.base, x.cnt);
            }
        } else {                             /* inode_alloc / inode_free */
            if (ir.empty() || (rng() & 1)) {
                uint64_t in = mgr->inode_alloc();
                if (in != PTR_INVALID) {
                    uint64_t bad = 0; int ow = -1;
                    if (!is->claim(in, 1, (int)cpu, &bad, &ow)) report_dup("P1.ino", in, 1, bad, ow, (int)cpu);
                    ir.push_back({in, 1});
                }
            } else {
                rec_t x = ir.back(); ir.pop_back();
                is->release(x.base, 1);
                mgr->inode_free(x.base);
            }
        }
    }
    /* 收束：归还本线程全部持有 → 影子清零（对账前置） */
    for (const rec_t &x : br) { bs->release(x.base, x.cnt); mgr->block_free(x.base, x.cnt); }
    for (const rec_t &x : ir) { is->release(x.base, 1); mgr->inode_free(x.base); }
}

/* ═══ P2：slice 级并发 worker ═══ */
static const uint64_t SLICE_BITS = 4ULL << 15;  /* 131072 位 */

static void worker_slice(chunk_slice_mgr_t *s, uint32_t cpu, int ops, shadow_t *sh) {
    t_cpu = cpu;
    std::mt19937_64 rng(0xD1B54A32D192ED03ULL ^ ((uint64_t)(cpu + 7) * 40503u));
    std::vector<rec_t> recs;
    recs.reserve(640);
    for (int i = 0; i < ops; i++) {
        if ((i & 63) == 0 && std::chrono::steady_clock::now() > g_deadline) break;
        uint64_t r = rng() % 100;
        if (recs.size() >= 512) r = 90;
        if (recs.empty()) r = 0;

        if (r < 50) {                        /* alloc_hard（try_lock → 可能 BUSY） */
            uint64_t cnt = 1 + rng() % 8, out = 0;
            result_code_t rc = s->alloc_hard((uint8_t)(rng() % 5), cnt, out);
            if (rc == RC_OK) {
                uint64_t bad = 0; int ow = -1;
                if (!sh->claim(out, cnt, (int)cpu, &bad, &ow)) report_dup("P2.hard", out, cnt, bad, ow, (int)cpu);
                recs.push_back({out, cnt});
            } else if (rc == RC_ERR_BUSY) {
                g_busy_hard.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (r < 70) {                 /* alloc_soft（try_lock → 可能 BUSY） */
            uint64_t req = 1 + rng() % 16, out = 0, left = 0;
            result_code_t rc = s->alloc_soft(0, req, out, left);
            if (rc == RC_OK) {
                uint64_t got = req - left;
                if (got) {
                    uint64_t bad = 0; int ow = -1;
                    if (!sh->claim(out, got, (int)cpu, &bad, &ow)) report_dup("P2.soft", out, got, bad, ow, (int)cpu);
                    recs.push_back({out, got});
                }
            } else if (rc == RC_ERR_BUSY) {
                g_busy_soft.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (r < 80) {                 /* soft_occupy（阻塞锁） */
            uint64_t base = rng() % SLICE_BITS, len = 1 + rng() % 8, rest = 0;
            result_code_t rc = s->soft_occupy(base, len, &rest);
            if (rc == RC_OK) {
                uint64_t got = len - rest;
                if (got) {
                    uint64_t bad = 0; int ow = -1;
                    if (!sh->claim(base, got, (int)cpu, &bad, &ow)) report_dup("P2.occ", base, got, bad, ow, (int)cpu);
                    recs.push_back({base, got});
                }
            }
        } else {                             /* free_interval（阻塞锁） */
            if (!recs.empty()) {
                rec_t x = recs.back(); recs.pop_back();
                sh->release(x.base, x.cnt);
                (void)s->free_interval(x.base, x.cnt);
            }
        }
    }
    for (const rec_t &x : recs) { sh->release(x.base, x.cnt); (void)s->free_interval(x.base, x.cnt); }
}

/* ═══ BUSY 定向压测：asyn 慢 I/O 持锁，3 线程 alloc try_lock → RC_ERR_BUSY ═══ */
static void busy_probe() {
    stress_disk_t d;
    d.data.assign((size_t)4 * 4096, 0);
    slice_soul_t soul{};
    soul.read_block = sd_read;
    soul.write_block = sd_write;
    soul.io_ctx = &d;
    soul.chunk_base_blk = 0;
    soul.chunk_blk_length = 4;
    soul.chunk_order = 0;
    chunk_slice_mgr_t s;
    if (s.enable(&soul, 0) != RC_OK) { ++g_fail; std::printf("  [FAIL] busy_probe enable\n"); return; }

    d.slow_us = 5000;                        /* 每次 I/O 5ms → 显著拉长持锁窗口 */
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> local_busy{0};
    std::vector<std::thread> hs;
    for (int t = 0; t < 3; t++) {
        hs.emplace_back([&, t] {
            std::mt19937_64 rng((uint64_t)(t + 1) * 7919u + 13);
            for (uint64_t k = 0; k < 5000000 && !stop.load(); k++) {
                uint64_t out = 0;
                result_code_t rc = s.alloc_hard(0, 1 + rng() % 4, out);
                if (rc == RC_ERR_BUSY) local_busy.fetch_add(1, std::memory_order_relaxed);
                else if (rc == RC_OK) (void)s.free_interval(out, 1);
            }
        });
    }
    for (int k = 0; k < 40; k++) (void)s.chunks_asyn();   /* 有界 40 轮，每轮 ~10ms I/O */
    stop.store(true);
    for (auto &h : hs) h.join();
    d.slow_us = 0;
    g_busy_hard.fetch_add(local_busy.load());
    (void)s.shutdown();
}

int main() {
    g_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(50);
    watchdog(60);
    auto t0 = std::chrono::steady_clock::now();
    uint64_t dup_prev = g_dup.load();   /* 分阶段基线：g_dup 全局不复位 */

    /* ═══ P1：mgr 级并发 ═══ */
    std::printf("[P1] mgr 级：4 线程 × 4000 次（block hard/soft/free/occupy + inode）\n");
    {
        stress_disk_t disk;
        disk.data.assign((size_t)12 * 4096, 0);   /* block 8 块 + inode 4 块，全空 */
        bitmap_slices_v2cfg_t cfg{};
        cfg.read_block = sd_read;
        cfg.write_block = sd_write;
        cfg.io_ctx = &disk;
        cfg.block_bitmap_base_FS_block = 0;
        cfg.block_bitmap_blocks = 8;
        cfg.inode_bitmap_base_FS_block = 8;
        cfg.inode_bitmap_blocks = 4;
        cfg.inobm_chunk_order = 0;
        cfg.blkbm_chunk_order = 0;
        cfg.processor_count = 8;                  /* ≥4；cpu 0..3 各占独立 preferred 片 */

        shadow_t bs(BLK_TOTAL), is(INO_TOTAL);
        uint64_t rc = 0;
        bitmap_slices_mgr_t_v2 mgr(cfg, &rc);
        CK(rc == (uint64_t)RC_OK, "P1 ctor rc=%llu", (unsigned long long)rc);
        CK(mgr.slice_count() == 4, "P1 slice_count=%llu (want 4)",
           (unsigned long long)mgr.slice_count());
        CK(mgr.block_free_total() == BLK_TOTAL, "P1 init blk free=%llu (want %llu)",
           (unsigned long long)mgr.block_free_total(), (unsigned long long)BLK_TOTAL);
        CK(mgr.inode_free_total() == INO_TOTAL, "P1 init ino free=%llu (want %llu)",
           (unsigned long long)mgr.inode_free_total(), (unsigned long long)INO_TOTAL);

        std::vector<std::thread> th;
        for (uint32_t t = 0; t < 4; t++)
            th.emplace_back(worker_mgr, &mgr, t, 4000, &bs, &is);
        for (auto &x : th) x.join();

        CK(bs.live() == 0, "P1 block shadow live=%llu (want 0)", (unsigned long long)bs.live());
        CK(is.live() == 0, "P1 inode shadow live=%llu (want 0)", (unsigned long long)is.live());
        CK(g_dup.load() == dup_prev, "P1 double-alloc count=%llu (want %llu)",
           (unsigned long long)g_dup.load(), (unsigned long long)dup_prev);
        dup_prev = g_dup.load();
        CK(mgr.block_free_total() == BLK_TOTAL,
           "P1 block_free_total=%llu (want %llu)", (unsigned long long)mgr.block_free_total(),
           (unsigned long long)BLK_TOTAL);
        CK(mgr.inode_free_total() == INO_TOTAL,
           "P1 inode_free_total=%llu (want %llu)", (unsigned long long)mgr.inode_free_total(),
           (unsigned long long)INO_TOTAL);
        /* 采样：归还全部后不应有残留置位 */
        uint64_t setbits = 0;
        for (uint64_t b = 0; b < BLK_TOTAL; b += 997) if (mgr.block_test(b)) setbits++;
        CK(setbits == 0, "P1 residual set bits=%llu (want 0)", (unsigned long long)setbits);
        std::printf("     blk_free_total=%llu ino_free_total=%llu dup=%llu rd=%llu wr=%llu\n",
                    (unsigned long long)mgr.block_free_total(),
                    (unsigned long long)mgr.inode_free_total(),
                    (unsigned long long)g_dup.load(),
                    (unsigned long long)disk.rd.load(),
                    (unsigned long long)disk.wr.load());
    }

    /* ═══ P2：slice 级并发 ═══ */
    std::printf("[P2] slice 级：单 chunk_slice_mgr_t + 4 线程 × 2500 次\n");
    {
        stress_disk_t d;
        d.data.assign((size_t)4 * 4096, 0);
        slice_soul_t soul{};
        soul.read_block = sd_read;
        soul.write_block = sd_write;
        soul.io_ctx = &d;
        soul.chunk_base_blk = 0;
        soul.chunk_blk_length = 4;
        soul.chunk_order = 0;
        chunk_slice_mgr_t s;
        CK(s.enable(&soul, 0) == RC_OK, "P2 enable");
        shadow_t sh(SLICE_BITS);
        std::vector<std::thread> th;
        for (uint32_t t = 0; t < 4; t++)
            th.emplace_back(worker_slice, &s, t, 2500, &sh);
        for (auto &x : th) x.join();
        CK(sh.live() == 0, "P2 shadow live=%llu (want 0)", (unsigned long long)sh.live());
        CK(s.get_free_count() == SLICE_BITS, "P2 free_count=%llu (want %llu)",
           (unsigned long long)s.get_free_count(), (unsigned long long)SLICE_BITS);
        CK(s.chunks_asyn() == RC_OK, "P2 asyn");
        CK(s.get_free_count() == SLICE_BITS, "P2 asyn free_count=%llu (want %llu)",
           (unsigned long long)s.get_free_count(), (unsigned long long)SLICE_BITS);
        CK(s.shutdown() == RC_OK, "P2 shutdown");
    }
    CK(g_dup.load() == dup_prev, "P2 double-alloc count=%llu (want %llu)",
       (unsigned long long)g_dup.load(), (unsigned long long)dup_prev);
    dup_prev = g_dup.load();

    busy_probe();
    CK(g_dup.load() == dup_prev, "P3 double-alloc count=%llu (want %llu)",
       (unsigned long long)g_dup.load(), (unsigned long long)dup_prev);
    dup_prev = g_dup.load();

    auto us = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    std::printf("[P3] BUSY 定向压测后：RC_ERR_BUSY hard=%llu soft=%llu\n",
                (unsigned long long)g_busy_hard.load(),
                (unsigned long long)g_busy_soft.load());
    CK(g_busy_hard.load() + g_busy_soft.load() > 0,
       "RC_ERR_BUSY total=%llu (want >0 —— 换片/忙路径未覆盖)",
       (unsigned long long)(g_busy_hard.load() + g_busy_soft.load()));

    std::printf("elapsed=%lldms dup=%llu busy_hard=%llu busy_soft=%llu\n",
                (long long)us, (unsigned long long)g_dup.load(),
                (unsigned long long)g_busy_hard.load(),
                (unsigned long long)g_busy_soft.load());

    g_done.store(true);
    if (g_fail == 0) { std::printf("STRESS PASS\n"); return 0; }
    std::printf("STRESS FAILED: %d\n", g_fail);
    return 1;
}

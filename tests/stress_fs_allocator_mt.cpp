/* ═══════════════════════════════════════════════════════════════════
 * stress_fs_allocator_mt.cpp — fs_allocator_t 多线程集成压力
 *
 * 2026-10-04 新建（RavenFS 测试工件；**不改任何生产代码**）。
 * 契约权威：include/ravenfs/fs_allocator.hpp。8 线程并发混合：
 *   block_alloc_hard / block_alloc_soft / block_occupy_soft /
 *   block_free / block_intervals_free / inode_alloc / inode_free / block_test。
 *
 * 校验：
 *   - 无 crash / 无死锁（墙钟看门狗兜底）；
 *   - 结束时 block_free_total()/inode_free_total() 与初始一致；
 *   - **无重复交付**：原子影子账本（互斥保护的位图）在线程认领时检测
 *     「同一块/同一 inode 被两线程同时拿到」；
 *   - 归还后 test 恢复（静止期抽样）。
 *
 * get_processor_id：本文件**强定义**（thread_local 伪 CPU），按线程返回独立
 *   id —— 覆盖 cpu_id_stub.cpp 的 weak 桩，让每线程命中独立 preferred 槽，
 *   否则 8 线程共用一个槽会产生 preferred 写-写 data race（那是桩的问题，
 *   非被测分配器）。
 *
 * ── ThreadSanitizer 变体（可选，手工编译）──
 *   g++ -std=c++20 -O1 -g -fsanitize=thread -Iinclude \
 *       tests/stress_fs_allocator_mt.cpp juvenile_god/fs_allocator.cpp \
 *       juvenile_god/fs_allocator_router.cpp -o /tmp/mt_tsan -lpthread
 *   /tmp/mt_tsan
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/fs_allocator.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

static const uint64_t MSB    = 32768;
static const uint64_t SLICES = 4;
static const uint64_t BITS   = MSB * SLICES;   /* 131072 bit */
static const uint32_t CPUS   = 8;

static std::atomic<int> g_fail{0};
static std::atomic<uint64_t> g_dup{0};
static std::atomic<uint64_t> g_ops{0};
static std::atomic<bool> g_done{false};
static std::mutex g_msg_mu;

#define CK(cond, ...) do { \
    if (!(cond)) { \
        int n = g_fail.fetch_add(1) + 1; \
        char buf[512]; std::snprintf(buf, sizeof buf, __VA_ARGS__); \
        std::lock_guard<std::mutex> lk(g_msg_mu); \
        if (n <= 30) { std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, buf); } \
    } \
} while (0)

static void watchdog(int sec) {
    std::thread th([sec] {
        auto t0 = std::chrono::steady_clock::now();
        while (!g_done.load()) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(sec)) {
                std::printf("TIMEOUT: exceeded %ds wall clock (deadlock?)\n", sec);
                std::fflush(stdout);
                std::_Exit(2);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    th.detach();
}

/* ── 4K 对齐原始位图（cfg 用；只在 ctor 消费一次）── */
struct Raw {
    uint8_t *mem = nullptr;
    explicit Raw(uint64_t nbits, uint8_t fill) {
        const uint64_t bytes = (nbits + 7) / 8;
        if (posix_memalign((void **)&mem, 4096, bytes) != 0) { std::printf("FAIL memalign\n"); std::exit(2); }
        memset(mem, fill, bytes);
    }
    ~Raw() { free(mem); }
    Raw(const Raw &) = delete;
    Raw &operator=(const Raw &) = delete;
    void set_bit(uint64_t i) { mem[i >> 3] |= (uint8_t)(1u << (i & 7)); }
};

/* ── 原子影子账本：bits 1=占用；claim 命中已占 = 重复交付 ── */
struct Shadow {
    std::vector<uint8_t> bits;
    std::mutex mu;
    explicit Shadow(uint64_t n) : bits((size_t)n, 0) {}
    bool claim(uint64_t base, uint64_t cnt, int caller) {
        std::lock_guard<std::mutex> lk(mu);
        for (uint64_t i = 0; i < cnt; i++) {
            if (bits[(size_t)(base + i)]) {
                if (g_dup.fetch_add(1) < 8)
                    std::printf("[RACE] double-alloc base=%llu cnt=%llu offending=%llu caller=%d\n",
                                (unsigned long long)base, (unsigned long long)cnt,
                                (unsigned long long)(base + i), caller);
                return false;
            }
        }
        for (uint64_t i = 0; i < cnt; i++) bits[(size_t)(base + i)] = 1;
        return true;
    }
    void release(uint64_t base, uint64_t cnt) {
        std::lock_guard<std::mutex> lk(mu);
        for (uint64_t i = 0; i < cnt; i++) bits[(size_t)(base + i)] = 0;
    }
    uint64_t live() {
        std::lock_guard<std::mutex> lk(mu);
        uint64_t c = 0; for (uint8_t b : bits) c += b; return c;
    }
    bool occ(uint64_t i) {
        std::lock_guard<std::mutex> lk(mu);
        return i < bits.size() && bits[(size_t)i] != 0;
    }
};

struct rec_t { uint64_t base, cnt; };

/* ── get_processor_id 强定义：按线程返回独立伪 CPU（覆盖 weak 桩）── */
static thread_local uint32_t t_cpu = 0;
extern "C" uint32_t get_processor_id() { return t_cpu; }

static std::chrono::steady_clock::time_point g_deadline;

static void worker(fs_allocator_t *a, uint32_t cpu, Shadow *bs, Shadow *is) {
    t_cpu = cpu;
    std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ ((uint64_t)(cpu + 1) * 2654435761u));
    std::vector<rec_t> br;  br.reserve(400);       /* 持有的 block run */
    std::vector<uint64_t> ir; ir.reserve(200);      /* 持有的 inode */
    uint64_t local_ops = 0;

    for (uint64_t it = 0; ; it++) {
        if ((it & 63) == 0 && std::chrono::steady_clock::now() > g_deadline) break;
        uint64_t r = rng() % 100;
        if (br.size() >= 256) r = 70;               /* 积压 → 强制归还 */
        if (br.empty())       r = 0;                /* 无货 → 强制分配 */

        if (r < 30) {                               /* block_alloc_hard */
            uint64_t cnt = 1 + rng() % 16;
            uint8_t  al  = (uint8_t)(rng() % 4);
            uint64_t b = a->block_alloc_hard(cnt, al);
            if (b != PTR_INVALID) {
                bs->claim(b, cnt, (int)cpu);
                br.push_back(rec_t{b, cnt});
            }
        } else if (r < 50) {                        /* block_alloc_soft */
            uint64_t req = 1 + rng() % 32;
            fs_run_list_t runs;
            result_code_t rc = a->block_alloc_soft(req, 0, &runs);
            if (rc == RC_OK) {
                for (const fs_run_t &q : runs) {
                    bs->claim(q.base, q.len, (int)cpu);
                    br.push_back(rec_t{q.base, q.len});
                }
            } else if (rc != RC_ERR_NO_BLOCK) {
                CK(false, "block_alloc_soft unexpected rc=%d", (int)rc);
            }
            runs.clear();
        } else if (r < 62) {                        /* block_occupy_soft */
            uint64_t base = rng() % BITS;
            uint64_t req  = 1 + rng() % 16;
            uint64_t rest = 0;
            result_code_t rc = a->block_occupy_soft(base, req, &rest);
            if (rc == RC_OK) {
                uint64_t got = req - rest;
                if (got) {
                    bs->claim(base, got, (int)cpu);
                    br.push_back(rec_t{base, got});
                }
            } else if (rc != RC_ERR_NO_BLOCK) {
                CK(false, "block_occupy_soft unexpected rc=%d", (int)rc);
            }
        } else if (r < 85) {                        /* block_free（单段） */
            if (!br.empty()) {
                rec_t x = br.back(); br.pop_back();
                bs->release(x.base, x.cnt);          /* 影子先空闲，再归还分配器 */
                result_code_t rc = a->block_free(x.base, x.cnt);
                if (rc != RC_OK) CK(false, "block_free rc=%d", (int)rc);
            }
        } else if (r < 91) {                        /* block_intervals_free（链式） */
            fs_run_list_t fl;
            for (int k = 0; k < 2 && !br.empty(); k++) {
                rec_t x = br.back(); br.pop_back();
                bs->release(x.base, x.cnt);
                fl.push_front(fs_run_t{x.base, x.cnt});
            }
            result_code_t rc = a->block_intervals_free(&fl);
            if (rc != RC_OK) CK(false, "block_intervals_free rc=%d", (int)rc);
        } else {                                    /* inode + block_test 采样 */
            if (rng() & 1) (void)a->block_test(rng() % BITS);   /* 只压锁路径 */
            if (ir.empty() || (rng() & 1)) {
                uint64_t in = 0;
                result_code_t rc = a->inode_alloc(in);
                if (rc == RC_OK) {
                    is->claim(in, 1, (int)cpu);
                    ir.push_back(in);
                } else if (rc != RC_ERR_NO_BLOCK) {
                    CK(false, "inode_alloc unexpected rc=%d", (int)rc);
                }
            } else {
                uint64_t in = ir.back(); ir.pop_back();
                is->release(in, 1);
                a->inode_free(in);
            }
        }
        local_ops++;
    }

    /* 收束：归还本线程全部持有 */
    for (const rec_t &x : br) { bs->release(x.base, x.cnt); (void)a->block_free(x.base, x.cnt); }
    for (uint64_t in : ir)    { is->release(in, 1);         a->inode_free(in); }
    g_ops.fetch_add(local_ops);
}

int main() {
    puts("==== stress_fs_allocator_mt : fs_allocator_t 多线程集成压力 (8 threads) ====");
    watchdog(30);
    auto t0 = std::chrono::steady_clock::now();
    g_deadline = t0 + std::chrono::seconds(5);      /* 运行 ≥ 数秒 */

    Raw blk(BITS, 0x00);        /* block 组：全空闲 */
    Raw ino(BITS, 0x00);        /* inode 组：全空闲 */
    ino.set_bit(0);             /* inode 0 = root：源位图亮灯（占用） */

    slices_cfg_t cfg{};
    cfg.block_bits_raw = blk.mem; cfg.block_bits_count = BITS;
    cfg.inode_bits_raw = ino.mem; cfg.inode_bits_count = BITS;
    cfg.logical_processor_count = CPUS;

    uint64_t rc = 1;
    fs_allocator_t a(cfg, &rc);
    CK(rc == (uint64_t)RC_OK, "ctor rc=%llu", (unsigned long long)rc);
    CK(a.slice_count() == SLICES, "slice_count=%llu (want %llu)",
       (unsigned long long)a.slice_count(), (unsigned long long)SLICES);
    CK(a.cpu_slots() == CPUS, "cpu_slots=%llu", (unsigned long long)a.cpu_slots());
    CK(a.block_free_total() == BITS, "init block_free_total=%llu (want %llu)",
       (unsigned long long)a.block_free_total(), (unsigned long long)BITS);
    CK(a.inode_free_total() == BITS - 1, "init inode_free_total=%llu (want %llu)",
       (unsigned long long)a.inode_free_total(), (unsigned long long)(BITS - 1));
    CK(a.block_test(0) == false, "init block_test(0)==false");
    CK(a.inode_test(0) == true, "init inode_test(0)==true (root)");

    Shadow bs(BITS), is(BITS);
    is.bits[0] = 1;             /* 影子同步 root */

    std::vector<std::thread> th;
    for (uint32_t t = 0; t < CPUS; t++) th.emplace_back(worker, &a, t, &bs, &is);
    for (auto &x : th) x.join();

    /* ── 终局对账 ── */
    CK(bs.live() == 0, "final block shadow live=%llu (want 0)", (unsigned long long)bs.live());
    /* inode 影子含 root(0) 恒占用：全归还后应恰为 1（root 本身） */
    CK(is.live() == 1, "final inode shadow live=%llu (want 1 = root only)", (unsigned long long)is.live());
    CK(g_dup.load() == 0, "double-alloc count=%llu (want 0)", (unsigned long long)g_dup.load());
    CK(a.block_free_total() == BITS, "final block_free_total=%llu (want %llu)",
       (unsigned long long)a.block_free_total(), (unsigned long long)BITS);
    CK(a.inode_free_total() == BITS - 1, "final inode_free_total=%llu (want %llu)",
       (unsigned long long)a.inode_free_total(), (unsigned long long)(BITS - 1));

    /* 静止期抽样：归还后 test 恢复（例外：inode 0 = root 恒占用） */
    uint64_t blk_bad = 0, ino_bad = 0;
    for (uint64_t b = 0; b < BITS; b += 977) if (a.block_test(b)) blk_bad++;
    for (uint64_t i = 1; i < BITS; i += 991) if (a.inode_test(i)) ino_bad++;
    CK(blk_bad == 0, "residual block bits=%llu (want 0)", (unsigned long long)blk_bad);
    CK(ino_bad == 0, "residual inode bits=%llu (want 0)", (unsigned long long)ino_bad);
    CK(a.inode_test(0) == true, "root inode still occupied after stress");

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    printf("  ops=%llu  elapsed=%lldms  slices=%llu  cpus=%u  dup=%llu  blk_free=%llu  ino_free=%llu\n",
           (unsigned long long)g_ops.load(), (long long)ms,
           (unsigned long long)a.slice_count(), CPUS,
           (unsigned long long)g_dup.load(),
           (unsigned long long)a.block_free_total(),
           (unsigned long long)a.inode_free_total());

    g_done.store(true);
    printf("\n==== stress_fs_allocator_mt: PASS %s, FAIL %d ====\n",
           g_fail.load() == 0 ? "OK" : "--", g_fail.load());
    if (g_fail.load() == 0) { puts("STRESS PASS"); return 0; }
    puts("STRESS FAILED");
    return 1;
}

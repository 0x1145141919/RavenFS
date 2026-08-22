/* ═══════════════════════════════════════════════════════════════════
 * benchmark_session_vs_nvme.cpp — InodeSession 缓存命中 vs 直接 NVMe 读
 *
 * 课题：访问一个已加载好的缓存 inode（RADIX 树寻址 + 状态机 + 锁，
 *       ~6~7 次 load 跳表）与直接 O_DIRECT 读盘（4K 页 → 取 inode）的
 *       时间开销对比。
 *
 * 方法：真盘（mkfs 后的 /dev/nvme1n1，1.22 亿 inode，bits=27 → 128 片 2 层树）。
 *   1. 选 64 行（1024 个 inode）预热进缓存，禁用 TTL 淘汰
 *   2. 预生成 10 万次随机 inum（全部落在预热集内 → 恒命中）
 *   3. 分别计时：InodeSession 全生命周期 / get-only / put-only /
 *      直接 pread 4K / 懒加载参考（未命中）
 * ═══════════════════════════════════════════════════════════════════ */
#include "bench_util.hpp"
#include <chrono>
#include <random>
#include <vector>

using Clock = std::chrono::steady_clock;

static double ns_per(const Clock::time_point &a, const Clock::time_point &b, uint64_t n) {
    return std::chrono::duration<double, std::nano>(b - a).count() / (double)n;
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/nvme1n1";
    FsGeometry geo;
    RavenSuperblock sb = open_ravenfs(dev, &geo);
    printf("device=%s  blocks=%lu inodes=%lu root=%lu\n",
           dev, (unsigned long)geo.total_blocks, (unsigned long)geo.inodes,
           (unsigned long)sb.root_inode);

    uint8_t bits = 0;
    while ((1ull << bits) < geo.inodes) bits++;
    printf("bits=%u → RADIX（128 片 2 层树，行内 row_bits=4）\n", bits);

    Inode_cache_cfg_t cfg{};
    cfg.inodes_count = geo.inodes;
    cfg.bits_len_of_inodes_count = bits;
    cfg.inode_array_base = geo.inode_array_base;
    cfg.ttl_ticks = 0;                 /* ctor 默认 2s，稍后显式禁用淘汰 */
    cfg.read_page = rd; cfg.write_page = wr;
    cfg.io_ctx = nullptr;
    cfg.alloc = al; cfg.free = fr;

    result_code_t rc = RC_ERR_IO;
    Inode_cache_mgr_t mgr(cfg, &rc);
    assert(rc == RC_OK);
    mgr.set_ttl_ticks(0);              /* 0 = 禁用 TTL 淘汰（set 语义），保证全程命中 */

    /* ── 预热行集：全范围均匀抽 64 行（跨片跨叶表，体现平均寻址路径） ── */
    std::mt19937_64 rng(0x5EED);
    std::uniform_int_distribution<uint64_t> line_dist(1, geo.inodes / 16 - 1);
    std::vector<uint64_t> lines;
    while (lines.size() < 64) {
        uint64_t l = line_dist(rng);
        bool dup = false;
        for (uint64_t x : lines) if (x == l) { dup = true; break; }
        if (!dup) lines.push_back(l);
    }

    /* 预热：get/put 一遍 → 全部 working */
    for (uint64_t line : lines) {
        Inode_cache_mgr_t::slice_cache_acces_token_t tok{};
        raven_Inode_t *p = mgr.get(line * 16, &tok);
        assert(p != nullptr);
        mgr.put(line * 16, tok);
    }
    printf("prewarmed %zu lines (%zu inodes)\n\n", lines.size(), lines.size() * 16);

    /* ── 预生成 10 万次访问序列（inode ∈ 预热集，恒命中） ── */
    const uint64_t N = 100000;
    std::vector<uint64_t> seq(N);
    std::uniform_int_distribution<int> li(0, (int)lines.size() - 1);
    std::uniform_int_distribution<int> row(0, 15);
    for (uint64_t i = 0; i < N; i++)
        seq[i] = lines[li(rng)] * 16 + row(rng);

    /* ── 1. InodeSession 全生命周期（构造=get，析构=put） ── */
    {
        const auto a = Clock::now();
        uint64_t sum = 0;
        for (uint64_t i = 0; i < N; i++) {
            InodeSession s(&mgr, seq[i]);
            assert(s.inode);
            sum += s.inode->link_count;   /* 防优化 */
        }
        const auto b = Clock::now();
        printf("InodeSession 全生命周期 : %8.1f ns/op  (%8.0f ops/s)  [sum=%lu]\n",
               ns_per(a, b, N), N / std::chrono::duration<double>(b - a).count(),
               (unsigned long)sum);
    }

    /* ── 2. get-only / put-only（紧包计时；单次 chrono ~20ns 开销占比 <2%） ── */
    {
        double acc = 0;
        for (uint64_t i = 0; i < N; i++) {
            Inode_cache_mgr_t::slice_cache_acces_token_t tok{};
            const auto a = Clock::now();
            raven_Inode_t *p = mgr.get(seq[i], &tok);
            const auto b = Clock::now();
            acc += std::chrono::duration<double, std::nano>(b - a).count();
            assert(p);
            mgr.put(seq[i], tok);
        }
        printf("get-only (紧包计时)   : %8.1f ns/op\n", acc / (double)N);
    }
    {
        double acc = 0;
        for (uint64_t i = 0; i < N; i++) {
            Inode_cache_mgr_t::slice_cache_acces_token_t tok{};
            raven_Inode_t *p = mgr.get(seq[i], &tok);
            const auto a = Clock::now();
            mgr.put(seq[i], tok);
            const auto b = Clock::now();
            acc += std::chrono::duration<double, std::nano>(b - a).count();
            (void)p;
        }
        printf("put-only (紧包计时)   : %8.1f ns/op\n", acc / (double)N);
    }

    /* ── 3. 直接 O_DIRECT pread 4K（随机行，NVMe 直通） ── */
    {
        void *buf = aligned_alloc(4096, 4096);
        assert(buf);
        const auto a = Clock::now();
        uint64_t sum = 0;
        for (uint64_t i = 0; i < N; i++) {
            const uint64_t blk = geo.inode_array_base + (seq[i] >> 4);
            ssize_t r = pread(g_fd, buf, 4096, (off_t)4096 + (off_t)blk * 4096);
            assert(r == 4096);
            sum += ((const raven_Inode_t *)buf)[seq[i] & 15].link_count;
        }
        const auto b = Clock::now();
        printf("直接 O_DIRECT pread   : %8.1f ns/op  (%8.0f ops/s)  [sum=%lu]\n",
               ns_per(a, b, N), N / std::chrono::duration<double>(b - a).count(),
               (unsigned long)sum);
        free(buf);
    }

    /* ── 4. 懒加载参考（未命中：RADIX 寻址 + 真盘读） ── */
    {
        std::vector<uint64_t> cold;
        while (cold.size() < 64) {
            uint64_t l = line_dist(rng);
            bool dup = false;
            for (uint64_t x : lines) if (x == l) { dup = true; break; }
            for (uint64_t x : cold) if (x == l) { dup = true; break; }
            if (!dup) cold.push_back(l);
        }
        std::vector<uint64_t> cold_seq(N);
        std::uniform_int_distribution<int> ci(0, (int)cold.size() - 1);
        for (uint64_t i = 0; i < N; i++)
            cold_seq[i] = cold[ci(rng)] * 16 + row(rng);
        const auto a = Clock::now();
        uint64_t sum = 0;
        for (uint64_t i = 0; i < N; i++) {
            InodeSession s(&mgr, cold_seq[i]);
            assert(s.inode);
            sum += s.inode->link_count;
        }
        const auto b = Clock::now();
        printf("懒加载参考 (未命中)   : %8.1f ns/op  (%8.0f ops/s)\n",
               ns_per(a, b, N), N / std::chrono::duration<double>(b - a).count());
        (void)sum;
    }

    printf("\n盘 I/O 统计: 读 %lu 次 (%lu MB) 写 %lu 次 (%lu MB)\n",
           (unsigned long)g_reads, (unsigned long)(g_read_bytes >> 20),
           (unsigned long)g_writes, (unsigned long)(g_write_bytes >> 20));
    printf("SESSION-VS-NVME PASS\n");
    close(g_fd);
    return 0;
}

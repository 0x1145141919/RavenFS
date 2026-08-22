/* ═══════════════════════════════════════════════════════════════════
 * benchmark_radix.cpp — Inode 缓存控制器真盘大测（RADIX 模式）
 *
 * 场景：mkfs.ravenfs /dev/nvme1n1 -f --inode-ratio=4
 *       blocks=488378645 inodes=122094672（约 1.22 亿 inode，bits=27 → statey_27）
 *
 * 规矩（按 block_model.md 链式推导法则）：
 *   - 读 Primary Superblock（fd byte[0,4096)）→ magic/version/crc32 校验
 *   - total_blocks + inode_array_count 零冗余链式推导全部几何：
 *       bitmap_blocks / bitmap_start / inode_blocks / inode_array_base
 *   - FS_block[n] fd 偏移 = 4096 + n×4096
 *   - 不踩踏 root_inode（inum 0）槽位：写路径测试范围 [16, inode_array_count)
 *
 * 阶段：
 *   A 正确性  ：root inode 读回为 DIR；随机行懒加载内容与直读盘逐字节一致
 *   B 大规模  ：200 万次随机 get/put（5 万随机行 + 1 万热点行），1% 标脏改 link_count
 *   C 持久化  ：flush_all 后直读盘校验 link_count == 特征值（脏页真落盘）
 *   D reap    ：TTL 调短，50 轮 reap 全扫 + 随机重载，校验内容一致
 *   E flush   ：flush_all 计时
 * ═══════════════════════════════════════════════════════════════════ */
#include "ravenfs/inode_cache_radix.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <random>
#include <vector>

static const char *DEV = "/dev/nvme1n1";
static const uint64_t SUPERBLOCK_MAGIC = 0x5241564EULL;   /* "RAVN" */
static const uint32_t RAVENFS_VERSION  = 2;               /* v1.1 */

/* ── CRC32（与 mkfs 同算法：字段置 0 后全结构计算） ── */
static uint32_t crc32_table[256];
static bool crc32_ready = false;
static void crc32_init() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        crc32_table[i] = crc;
    }
    crc32_ready = true;
}
static uint32_t crc32_compute(const void *data, size_t len) {
    if (!crc32_ready) crc32_init();
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *buf = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

static int g_fd = -1;
static uint64_t g_reads = 0, g_writes = 0, g_read_bytes = 0, g_write_bytes = 0;

static result_code_t rd(void *, uint64_t blk, void *buf) {
    g_reads++; g_read_bytes += 4096;
    ssize_t r = pread(g_fd, buf, 4096, (off_t)4096 + (off_t)blk * 4096);
    return (r == 4096) ? RC_OK : RC_ERR_IO;
}
static result_code_t wr(void *, uint64_t blk, const void *buf) {
    g_writes++; g_write_bytes += 4096;
    ssize_t r = pwrite(g_fd, buf, 4096, (off_t)4096 + (off_t)blk * 4096);
    return (r == 4096) ? RC_OK : RC_ERR_IO;
}
static void *al(void *, uint64_t sz) { return aligned_alloc(4096, (sz + 4095) & ~4095ull); }
static void fr(void *, void *p) { free(p); }

/* 直读盘（不经缓存），用于内容对比 */
static void disk_read_blk(uint64_t blk, void *buf) {
    ssize_t r = pread(g_fd, buf, 4096, (off_t)4096 + (off_t)blk * 4096);
    assert(r == 4096);
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : DEV;
    g_fd = open(dev, O_RDWR | O_DIRECT);
    if (g_fd < 0) { perror("open"); return 1; }

    /* ── 超级块读取 + 校验 ── */
    RavenSuperblock sb;
    assert(pread(g_fd, &sb, sizeof(sb), 0) == (ssize_t)sizeof(sb));
    assert(sb.magic == SUPERBLOCK_MAGIC);
    assert(sb.version == RAVENFS_VERSION);
    const uint32_t crc_stored = sb.crc32;
    sb.crc32 = 0;
    assert(crc32_compute(&sb, sizeof(sb)) == crc_stored);

    const uint64_t total_blocks = sb.total_blocks;
    const uint64_t inodes       = sb.inode_array_count;
    printf("superblock OK: blocks=%lu inodes=%lu root=%lu uuid=%02x%02x%02x%02x...\n",
           (unsigned long)total_blocks, (unsigned long)inodes, (unsigned long)sb.root_inode,
           sb.uuid[0], sb.uuid[1], sb.uuid[2], sb.uuid[3]);
    assert(sb.root_inode == 0);

    /* ── 链式推导（block_model.md §3.2，零冗余） ── */
    const uint64_t bitmap_blocks = (total_blocks + 32767) / 32768;
    const uint64_t bitmap_start  = total_blocks - bitmap_blocks;
    const uint64_t inode_blocks  = (inodes * 256 + 4095) / 4096;
    const uint64_t inode_array_base = bitmap_start - inode_blocks;
    const uint64_t ibm_blocks    = (inodes + 32767) / 32768;
    const uint64_t ia_bm_base    = inode_array_base - ibm_blocks;
    printf("geometry: bitmap=[%lu,%lu) inode_array=[%lu,%lu) inode_bm=[%lu,%lu)\n",
           (unsigned long)bitmap_start, (unsigned long)total_blocks,
           (unsigned long)inode_array_base, (unsigned long)bitmap_start,
           (unsigned long)ia_bm_base, (unsigned long)inode_array_base);
    /* 等价校验：尾巴连续布局成立 */
    assert(bitmap_start + bitmap_blocks == total_blocks);
    assert(inode_array_base + inode_blocks == bitmap_start);
    assert(ia_bm_base + ibm_blocks == inode_array_base);

    /* bits = ceil(log2(inodes))：1.22 亿 → 27 → statey_27={7,9,7}（128 片 2 层树） */
    uint8_t bits = 0;
    while ((1ull << bits) < inodes) bits++;
    assert(bits >= 19 && bits <= 52);   /* RADIX 档 */
    printf("bits=%u → statey_%u, row_bits=4\n", bits, bits);

    /* ── 缓存控制器初始化（I/O 直通裸盘，O_DIRECT 4K 对齐） ── */
    Inode_cache_cfg_t cfg{};
    cfg.inodes_count = inodes;
    cfg.bits_len_of_inodes_count = bits;
    cfg.inode_array_base = inode_array_base;
    cfg.ttl_ticks = 0;                  /* ctor 按 TSC 换算默认 2s */
    cfg.read_page = rd; cfg.write_page = wr;
    cfg.io_ctx = nullptr;
    cfg.alloc = al; cfg.free = fr;

    result_code_t rc = RC_ERR_IO;
    Inode_cache_mgr_t mgr(cfg, &rc);
    assert(rc == RC_OK);
    assert(mgr.mode() == CACHE_MODE_RADIX_SHARD);
    printf("mgr ready (RADIX, %u slices)\n\n", 1u << 7);

    /* ═══ A：正确性 ═══ */
    {
        /* root inode 只读：应为 DIR（光杆司令 /） */
        Inode_cache_mgr_t::slice_cache_acces_token_t tok{};
        raven_Inode_t *root = mgr.get(0, &tok);
        assert(root != nullptr);
        assert(root->type == INODE_TYPE_DIR);
        mgr.put(0, tok);
        printf("A1 root_inode: type=DIR OK\n");

        /* 随机 64 行懒加载内容与直读盘逐字节一致 */
        std::mt19937_64 rng(0xA11CE);
        std::uniform_int_distribution<uint64_t> line_dist(0, inodes / 16 - 1);
        std::vector<uint8_t> disk_page(4096);
        for (int i = 0; i < 64; i++) {
            const uint64_t inum = line_dist(rng) * 16;
            Inode_cache_mgr_t::slice_cache_acces_token_t t{};
            raven_Inode_t *p = mgr.get(inum, &t);
            assert(p != nullptr);
            const uint64_t blk = inode_array_base + (inum >> 4);
            disk_read_blk(blk, disk_page.data());
            assert(memcmp(p, disk_page.data(), 4096) == 0);
            mgr.put(inum, t);
        }
        printf("A2 懒加载 64 行与直读盘一致 OK\n\n");
    }

    /* ═══ B：大规模随机压测 ═══ */
    std::mt19937_64 rng(0x94CF210);
    std::uniform_int_distribution<uint64_t> line_dist(0, inodes / 16 - 1);
    std::vector<uint64_t> hot_lines;
    for (int i = 0; i < 10000; i++) hot_lines.push_back(1 + i);       /* 热点：行 1~10000 */
    std::uniform_int_distribution<int> hot_pick(0, (int)hot_lines.size() - 1);
    std::uniform_int_distribution<int> pick100(0, 99);

    const uint64_t N_OPS = 2000000;
    uint64_t dirty_rows = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < N_OPS; i++) {
        const uint64_t line = (pick100(rng) < 70)
            ? hot_lines[hot_pick(rng)]
            : line_dist(rng);
        const uint64_t inum = line * 16 + (uint64_t)(line % 16);   /* 行内偏移随行号，覆盖槽位 */
        Inode_cache_mgr_t::slice_cache_acces_token_t tok{};
        raven_Inode_t *p = mgr.get(inum, &tok);
        assert(p != nullptr);
        if (pick100(rng) < 1) {              /* 1% 标脏 + 改 link_count（特征 = inum ^ 0x5EED） */
            p->link_count = (uint32_t)(inum ^ 0x5EEDu);
            mgr.mark_dirty(inum);
            dirty_rows++;
        }
        mgr.put(inum, tok);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    printf("B  200万 ops  %.3fs  %.0f ops/s\n", sec, N_OPS / sec);
    printf("   盘读 %lu 次 (%lu MB)  盘写 %lu 次 (%lu MB)  标脏行 %lu\n",
           (unsigned long)g_reads, (unsigned long)(g_read_bytes >> 20),
           (unsigned long)g_writes, (unsigned long)(g_write_bytes >> 20),
           (unsigned long)dirty_rows);

    /* ═══ C：持久化（flush_all 后直读盘校验 link_count 特征） ═══ */
    {
        const auto f0 = std::chrono::steady_clock::now();
        assert(mgr.flush_all() == RC_OK);
        const auto f1 = std::chrono::steady_clock::now();
        printf("C  flush_all %.3fs\n", std::chrono::duration<double>(f1 - f0).count());

        /* 抽查 32 行：直读盘，link_count 应为特征值或原始值（未被改到的行） */
        std::vector<uint8_t> disk_page(4096);
        uint64_t checked = 0;
        for (int i = 0; i < 32; i++) {
            const uint64_t line = pick100(rng) < 70
                ? hot_lines[hot_pick(rng)] : line_dist(rng);
            const uint64_t inum = line * 16 + (line % 16);
            const uint64_t blk = inode_array_base + line;
            disk_read_blk(blk, disk_page.data());
            raven_Inode_t *di = (raven_Inode_t *)disk_page.data();
            const uint32_t expect = (uint32_t)(inum ^ 0x5EEDu);
            /* 缓存里可能残留旧值：以 mgr 视图为准 —— 重新 get 对比 */
            Inode_cache_mgr_t::slice_cache_acces_token_t tok{};
            raven_Inode_t *p = mgr.get(inum, &tok);
            assert(p != nullptr);
            if (p->link_count == expect) {
                assert(di[inum & 15].link_count == expect);   /* 脏页必须已落盘 */
                checked++;
            }
            mgr.put(inum, tok);
        }
        printf("C  脏行落盘校验通过 %lu/32（其余行未被标脏）\n", (unsigned long)checked);
    }

    /* ═══ D：reap 压测（TTL 调短 → 全扫 + 重载循环） ═══ */
    {
        mgr.set_ttl_ticks(100000);              /* ~50us @2GHz：基本次次超时 */
        const uint64_t r0 = g_reads;
        const auto d0 = std::chrono::steady_clock::now();
        for (int round = 0; round < 50; round++) {
            mgr.reap_expired();
            const uint64_t line = pick100(rng) < 70
                ? hot_lines[hot_pick(rng)] : line_dist(rng);
            const uint64_t inum = line * 16 + (line % 16);
            Inode_cache_mgr_t::slice_cache_acces_token_t tok{};
            raven_Inode_t *p = mgr.get(inum, &tok);
            assert(p != nullptr);
            if (p->link_count != 0 && p->link_count != (uint32_t)(inum ^ 0x5EEDu))
                assert(false && "内容损坏");
            mgr.put(inum, tok);
        }
        const auto d1 = std::chrono::steady_clock::now();
        printf("D  50 轮 reap+重载 %.3fs  盘读增量 %lu\n",
               std::chrono::duration<double>(d1 - d0).count(),
               (unsigned long)(g_reads - r0));
    }

    /* ═══ E：最终 flush + 汇总 ═══ */
    {
        const auto e0 = std::chrono::steady_clock::now();
        assert(mgr.flush_all() == RC_OK);
        const auto e1 = std::chrono::steady_clock::now();
        printf("E  final flush_all %.3fs\n", std::chrono::duration<double>(e1 - e0).count());
    }
    printf("\nTOTAL 盘读 %lu MB / 盘写 %lu MB\n",
           (unsigned long)(g_read_bytes >> 20), (unsigned long)(g_write_bytes >> 20));
    printf("BENCHMARK PASS\n");
    close(g_fd);
    return 0;
}

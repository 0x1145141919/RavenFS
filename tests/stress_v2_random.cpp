/* ═══════════════════════════════════════════════════════════════════
 * stress_v2_random.cpp — bitmap_slices_v2 随机压力 + 影子模型不变量对拍
 *
 * 2026-09-30。独立 main（**未接 CMake**，手工编译）：
 *   g++ -std=c++17 -Wall -Wextra -Iinclude/ravenfs \
 *       tests/stress_v2_random.cpp juvenile_god/bitmap_slices_v2.cpp \
 *       -o /tmp/v2rand && /tmp/v2rand
 *
 * 被测：bitmap_slices_v2.hpp（契约）+ juvenile_god/bitmap_slices_v2.cpp
 *   · bitmap_slices_mgr_t_v2（路由层：block/inode 两组、全局坐标、跨片 free/occupy）
 *   · chunk_slice_mgr_t（片层：chunks_asyn / free_count_calib_t 语义）
 *
 * 手法：自带「绝对 FS_block 寻址」mock 盘（read/write 回调 + 字节镜像）。
 *   **影子模型**：维护逻辑已分配块集合（block 组 + inode 组各一份 uint8 位图
 *   + 已分配数），每次操作后与 mgr 位图态对拍（聚合 free_total 每操作；
 *   block_test/inode_test 逐点全量重扫每 N 操作；盘字节全量对拍在 flush 后）。
 *
 * 随机负载（run A：默认 200k 操作，可 argv 覆盖）：混合
 *   block_alloc_hard / block_alloc_soft / block_free /
 *   block_occupy_soft / inode_alloc / inode_free；
 *   count/align_log2(0..15) 随机，且刻意大跨度（occupy 锚点贴片界）以频繁触发
 *   换窗（evict/load/drain）。
 *
 * 不变量：
 *   ① 无重复分配（命中区间在影子中须全空闲）
 *   ② free 只作用于已分配块
 *   ③ occupy 结果连续（自锚点起）且 ≤ 额度
 *   ④ block_free_total/inode_free_total 与影子重算一致（每操作；每 N 操作全量逐点）
 *   ⑤ 操作后 free_count == 逻辑 0 位数（聚合计账；全量扫描为独立见证）
 *   ⑥ chunks_asyn 后 free_count 与重扫一致、free_count_calib_t.old/new 语义正确
 *      （run C 片层专项；run B 以 dtor shutdown 落盘对拍为同构见证）
 *   ⑦ all-alloc→all-free 后回到初始 free_count（run A 末尾清空 + 盘全 0）
 *
 * 固定 seed 可复跑（argv[1]=seed、argv[2]=ops）；失败打印 seed + 最近操作序列。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/bitmap_slices_v2.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

/* ───────────────────────── 几何（run A/B：mgr 级） ─────────────────────────
 * block 组位图区 FS_block[0,32) = 1048576 位；inode 组 FS_block[32,40) = 262144 位；
 * processor_count=4、chunk_order=0 ⇒ slice_count=4：block 片 262144 位(8 窗)、
 * inode 片 65536 位(2 窗)。**每片 8 窗**给足换窗（evict/load/drain）机会。 */
static const uint64_t BLK_BLOCKS = 32;
static const uint64_t INO_BLOCKS = 8;
static const uint32_t CPU_CNT    = 4;
static const uint8_t  CHUNK_ORDER = 0;
static const uint64_t BLK_BITS   = BLK_BLOCKS << 15;              /* 1048576 */
static const uint64_t INO_BITS   = INO_BLOCKS << 15;              /* 262144  */
static const uint64_t INO_BIT_OFF = (BLK_BLOCKS * 4096ULL) * 8ULL;/* 1048576 */
static const uint64_t TOTAL_BLOCKS = BLK_BLOCKS + INO_BLOCKS;
static const uint32_t EXPECT_SLICES = 4;
static const uint64_t SCAN_EVERY = 20000;   /* 全量逐点重扫间隔 */

/* ───────────────────────────── cpu 身份桩 ───────────────────────────── */
static uint32_t g_cpu = 0;
extern "C" uint32_t get_processor_id() { return g_cpu; }

/* ───────────────────────── 失败上报 / 复现序列 ───────────────────────── */
static uint64_t g_seed = 0;
static size_t   g_opidx = 0;
static int      g_fail = 0;
static int      g_failprint = 0;
static std::deque<std::string> g_ring;
static const char *g_phase = "";

static void dump_repro() {
    std::printf("    seed=%llu phase=%s op#%zu ; last ops:\n",
                (unsigned long long)g_seed, g_phase, g_opidx);
    for (const auto &s : g_ring) std::printf("      %s\n", s.c_str());
}

#define CHK(cond, ...)                                                    \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ++g_fail;                                                     \
            if (g_failprint < 6) {                                        \
                ++g_failprint;                                            \
                std::printf("  [FAIL] ");                                 \
                std::printf(__VA_ARGS__);                                 \
                std::printf("\n");                                        \
                dump_repro();                                             \
            }                                                             \
        }                                                                 \
    } while (0)

static void logop(const char *s) {
    g_ring.push_back(std::string(s));
    if (g_ring.size() > 128) g_ring.pop_front();
}

/* ───────────────────────── mock 盘（绝对 FS_block） ───────────────────────── */
struct mgr_disk_t {
    std::vector<uint8_t> data;   /* data[blk*4096 .. ] */
    uint64_t rd = 0, wr = 0;
};
static result_code_t disk_read(void *ctx, uint64_t blk, uint64_t len, void *buf) {
    mgr_disk_t *d = static_cast<mgr_disk_t *>(ctx);
    if (len == 0 || blk + len > (uint64_t)(d->data.size() / 4096)) return RC_ERR_IO;
    std::memcpy(buf, &d->data[(size_t)blk * 4096], (size_t)len * 4096);
    d->rd++;
    return RC_OK;
}
static result_code_t disk_write(void *ctx, uint64_t blk, uint64_t len, const void *buf) {
    mgr_disk_t *d = static_cast<mgr_disk_t *>(ctx);
    if (len == 0 || blk + len > (uint64_t)(d->data.size() / 4096)) return RC_ERR_IO;
    std::memcpy(&d->data[(size_t)blk * 4096], buf, (size_t)len * 4096);
    d->wr++;
    return RC_OK;
}
/* 盘位（bit 索引：组内位号 / inode 全局位号 = INO_BIT_OFF + j） */
static inline bool pbit(const mgr_disk_t &d, uint64_t bit) {
    return ((d.data[bit >> 3] >> (bit & 7)) & 1u) != 0;
}
static uint64_t disk_zeros(const mgr_disk_t &d, uint64_t bits) {
    uint64_t z = 0;
    for (uint64_t i = 0; i < bits; i++) if (!pbit(d, i)) z++;
    return z;
}

/* ───────────────────────── 影子模型 ───────────────────────── */
struct shadow_t {
    std::vector<uint8_t> bits;
    uint64_t alloc = 0;          /* 已分配位数 */
    uint64_t total = 0;
};
struct ext_t { uint64_t start, len; };   /* 逻辑已分配连续区间（供 free 选靶） */

static uint64_t per_of(uint64_t nbits, uint32_t sc) {
    uint64_t per = ((nbits + sc - 1) / sc + 32767) / 32768 * 32768;
    if (per == 0) per = 32768;
    return per;
}

/* count / occupy 额度分布：多数小、少中、偶大（大跨度触发跨窗/跨片） */
static uint64_t gen_count(std::mt19937_64 &r) {
    uint32_t c = (uint32_t)(r() % 100);
    if (c < 80) return 1 + (r() % 64);
    if (c < 97) return 1 + (r() % 3000);
    return 1 + (r() % 30000);
}
static uint64_t gen_occ(std::mt19937_64 &r) {
    uint32_t c = (uint32_t)(r() % 100);
    if (c < 70) return 1 + (r() % 200);
    if (c < 95) return 1 + (r() % 5000);
    return 1 + (r() % 120000);
}

/* ───────────────────────── run A/B：mgr 级随机压力 ───────────────────────── */

/* 全量逐点重扫（block_test/inode_test 为 mgr 逻辑真相的独立见证） */
static void scan_logical(bitmap_slices_mgr_t_v2 &mgr, shadow_t &sb, shadow_t &si) {
    for (uint64_t i = 0; i < BLK_BITS; i++) {
        bool e = sb.bits[i] != 0, g = mgr.block_test(i);
        if (e != g) { CHK(false, "logical blk bit %llu shadow=%d mgr=%d",
                          (unsigned long long)i, (int)e, (int)g); return; }
    }
    for (uint64_t j = 0; j < INO_BITS; j++) {
        bool e = si.bits[j] != 0, g = mgr.inode_test(j);
        if (e != g) { CHK(false, "logical ino bit %llu shadow=%d mgr=%d",
                          (unsigned long long)j, (int)e, (int)g); return; }
    }
}

static void stress_mgr_run(uint64_t seed, uint64_t nops, bool free_all_end,
                           const char *tag) {
    g_phase = tag; g_seed = seed; g_opidx = 0; g_ring.clear();
    std::printf("[%s] mgr random seed=%llu ops=%llu %s\n", tag,
                (unsigned long long)seed, (unsigned long long)nops,
                free_all_end ? "(free-all end → ⑦)" : "(keep state → flush 对拍)");

    mgr_disk_t disk;
    disk.data.assign((size_t)TOTAL_BLOCKS * 4096, 0);

    bitmap_slices_v2cfg_t cfg{};
    cfg.read_block = disk_read;
    cfg.write_block = disk_write;
    cfg.io_ctx = &disk;
    cfg.block_bitmap_base_FS_block = 0;
    cfg.block_bitmap_blocks = BLK_BLOCKS;
    cfg.inode_bitmap_base_FS_block = BLK_BLOCKS;
    cfg.inode_bitmap_blocks = INO_BLOCKS;
    cfg.inobm_chunk_order = CHUNK_ORDER;
    cfg.blkbm_chunk_order = CHUNK_ORDER;
    cfg.processor_count = CPU_CNT;

    shadow_t sb, si;
    sb.bits.assign(BLK_BITS, 0); sb.total = BLK_BITS;
    si.bits.assign(INO_BITS, 0); si.total = INO_BITS;
    std::vector<ext_t> live_b;
    std::vector<uint64_t> live_i;   /* 逻辑已分配 inode 号（供 free 选靶） */
    std::mt19937_64 r(seed);

    uint64_t rc = 0;
    {
        bitmap_slices_mgr_t_v2 mgr(cfg, &rc);
        CHK(rc == (uint64_t)RC_OK, "ctor rc=%llu", (unsigned long long)rc);
        if (rc != (uint64_t)RC_OK) return;
        CHK(mgr.slice_count() == EXPECT_SLICES, "slice_count=%llu (want %u)",
            (unsigned long long)mgr.slice_count(), EXPECT_SLICES);
        CHK(mgr.block_free_total() == BLK_BITS, "init blk free=%llu (want %llu)",
            (unsigned long long)mgr.block_free_total(), (unsigned long long)BLK_BITS);
        CHK(mgr.inode_free_total() == INO_BITS, "init ino free=%llu (want %llu)",
            (unsigned long long)mgr.inode_free_total(), (unsigned long long)INO_BITS);
        const uint64_t per_b = per_of(BLK_BITS, (uint32_t)mgr.slice_count());

        for (uint64_t op = 0; op < nops; op++) {
            g_opidx = op;
            g_cpu = (uint32_t)(r() % CPU_CNT);
            char buf[192];

            const uint64_t alloc = sb.alloc;
            int kind;
            if (alloc * 100 >= BLK_BITS * 70) {
                kind = (r() % 100 < 15) ? 5 : 2;                 /* free / inode_free */
            } else if (alloc * 100 <= BLK_BITS * 20) {
                uint32_t z = (uint32_t)(r() % 100);
                kind = z < 75 ? 0 : (z < 95 ? 1 : 4);            /* mostly alloc */
            } else {
                uint32_t z = (uint32_t)(r() % 100);
                kind = z < 40 ? 0 : z < 62 ? 1 : z < 84 ? 2
                     : z < 93 ? 3 : z < 98 ? 4 : 5;
            }
            if (kind == 2 && live_b.empty()) kind = 0;
            if (kind == 5 && live_i.empty()) kind = 4;

            if (kind == 0) {                     /* ── block_alloc_hard ── */
                uint64_t count = gen_count(r);
                uint8_t  al = (uint8_t)(r() % 16);
                const uint64_t amask = (al ? (1ULL << al) : 1ULL) - 1;
                uint64_t g = mgr.block_alloc_hard(count, al);
                if (g == PTR_INVALID) {
                    std::snprintf(buf, sizeof buf, "HARD c=%llu a=%u -> INVALID",
                                  (unsigned long long)count, (unsigned)al);
                } else {
                    std::snprintf(buf, sizeof buf, "HARD c=%llu a=%u -> %llu",
                                  (unsigned long long)count, (unsigned)al,
                                  (unsigned long long)g);
                    if (g + count > BLK_BITS) {
                        CHK(false, "HARD range overflow g=%llu c=%llu", 
                            (unsigned long long)g, (unsigned long long)count);
                    } else {
                        CHK((g & amask) == 0, "HARD align a=%u g=%llu", 
                            (unsigned)al, (unsigned long long)g);
                        bool dup = false;
                        for (uint64_t i = 0; i < count; i++) if (sb.bits[g + i]) { dup = true; break; }
                        CHK(!dup, "HARD dup alloc g=%llu c=%llu (①)",
                            (unsigned long long)g, (unsigned long long)count);
                        for (uint64_t i = 0; i < count; i++) sb.bits[g + i] = 1;
                        sb.alloc += count;
                        live_b.push_back({g, count});
                    }
                }
            } else if (kind == 1) {              /* ── block_alloc_soft ── */
                uint64_t count = gen_count(r), remain = 0;
                uint64_t g = mgr.block_alloc_soft(count, &remain);
                if (g == PTR_INVALID) {
                    std::snprintf(buf, sizeof buf, "SOFT c=%llu -> INVALID rem=%llu",
                                  (unsigned long long)count, (unsigned long long)remain);
                    CHK(remain == count, "SOFT invalid rem=%llu c=%llu",
                        (unsigned long long)remain, (unsigned long long)count);
                } else {
                    uint64_t got = (remain <= count) ? count - remain : 0;
                    std::snprintf(buf, sizeof buf, "SOFT c=%llu -> %llu got=%llu",
                                  (unsigned long long)count, (unsigned long long)g,
                                  (unsigned long long)got);
                    CHK(got >= 1, "SOFT got=0 with RC_OK");
                    if (got >= 1 && g + got <= BLK_BITS) {
                        bool dup = false;
                        for (uint64_t i = 0; i < got; i++) if (sb.bits[g + i]) { dup = true; break; }
                        CHK(!dup, "SOFT dup alloc g=%llu got=%llu (①)",
                            (unsigned long long)g, (unsigned long long)got);
                        for (uint64_t i = 0; i < got; i++) sb.bits[g + i] = 1;
                        sb.alloc += got;
                        live_b.push_back({g, got});
                    } else {
                        CHK(false, "SOFT range overflow g=%llu got=%llu",
                            (unsigned long long)g, (unsigned long long)got);
                    }
                }
            } else if (kind == 2) {              /* ── block_free ── */
                size_t idx = (size_t)(r() % live_b.size());
                ext_t e = live_b[idx];
                uint64_t cnt = (r() % 100 < 60) ? e.len : (1 + r() % e.len);
                if (cnt > e.len) cnt = e.len;
                uint64_t off = (e.len > cnt) ? (r() % (e.len - cnt + 1)) : 0;
                uint64_t p = e.start + off;
                std::snprintf(buf, sizeof buf, "FREE  %llu+%llu (of %llu+%llu)",
                              (unsigned long long)p, (unsigned long long)cnt,
                              (unsigned long long)e.start, (unsigned long long)e.len);
                bool bad = false;
                for (uint64_t i = 0; i < cnt; i++) if (!sb.bits[p + i]) { bad = true; break; }
                CHK(!bad, "FREE on unallocated %llu+%llu (②)",
                    (unsigned long long)p, (unsigned long long)cnt);
                mgr.block_free(p, cnt);
                for (uint64_t i = 0; i < cnt; i++) sb.bits[p + i] = 0;
                sb.alloc -= cnt;
                live_b[idx] = live_b.back(); live_b.pop_back();
                if (off > 0) live_b.push_back({e.start, off});
                uint64_t right = e.len - off - cnt;
                if (right > 0) live_b.push_back({p + cnt, right});
            } else if (kind == 3) {              /* ── block_occupy_soft ── */
                uint64_t base;
                if (r() % 4 == 0 && EXPECT_SLICES > 1) {   /* 贴片界 → 跨片续圈 */
                    uint64_t k = 1 + r() % (EXPECT_SLICES - 1);
                    uint64_t e = k * per_b;
                    uint64_t d = r() % 1000;
                    base = (e > d) ? e - d : 0;
                } else {
                    base = r() % BLK_BITS;
                }
                uint64_t count = gen_occ(r), rest = 0;
                uint64_t occ = mgr.block_occupy_soft(base, count, &rest);
                if (occ == PTR_INVALID) {
                    std::snprintf(buf, sizeof buf, "OCC   %llu c=%llu -> INVALID rem=%llu",
                                  (unsigned long long)base, (unsigned long long)count,
                                  (unsigned long long)rest);
                    CHK(rest == count, "OCC invalid rem=%llu c=%llu",
                        (unsigned long long)rest, (unsigned long long)count);
                } else {
                    uint64_t claimed = (rest <= count) ? count - rest : 0;
                    std::snprintf(buf, sizeof buf, "OCC   %llu c=%llu -> claimed=%llu",
                                  (unsigned long long)base, (unsigned long long)count,
                                  (unsigned long long)claimed);
                    CHK(occ == base, "OCC return %llu != anchor %llu (③)",
                        (unsigned long long)occ, (unsigned long long)base);
                    CHK(claimed >= 1 && claimed <= count, "OCC claimed=%llu c=%llu (③)",
                        (unsigned long long)claimed, (unsigned long long)count);
                    if (claimed >= 1 && base + claimed <= BLK_BITS) {
                        bool bad = false;
                        for (uint64_t i = 0; i < claimed; i++) if (sb.bits[base + i]) { bad = true; break; }
                        CHK(!bad, "OCC not contiguous-free %llu+%llu (①③)",
                            (unsigned long long)base, (unsigned long long)claimed);
                        for (uint64_t i = 0; i < claimed; i++) sb.bits[base + i] = 1;
                        sb.alloc += claimed;
                        live_b.push_back({base, claimed});
                    } else {
                        CHK(false, "OCC range overflow base=%llu claimed=%llu",
                            (unsigned long long)base, (unsigned long long)claimed);
                    }
                }
            } else if (kind == 4) {              /* ── inode_alloc ── */
                uint64_t inum = mgr.inode_alloc();
                if (inum == PTR_INVALID) {
                    std::snprintf(buf, sizeof buf, "IALLOC -> INVALID");
                } else {
                    std::snprintf(buf, sizeof buf, "IALLOC -> %llu", (unsigned long long)inum);
                    CHK(inum < INO_BITS, "IALLOC inum=%llu >= %llu",
                        (unsigned long long)inum, (unsigned long long)INO_BITS);
                    if (inum < INO_BITS) {
                        CHK(!si.bits[inum], "IALLOC dup inum=%llu (①)",
                            (unsigned long long)inum);
                        si.bits[inum] = 1; si.alloc++;
                        live_i.push_back(inum);
                    }
                }
            } else {                             /* ── inode_free ── */
                size_t idx = (size_t)(r() % live_i.size());
                uint64_t inum = live_i[idx];
                std::snprintf(buf, sizeof buf, "IFREE %llu", (unsigned long long)inum);
                CHK(si.bits[inum] != 0, "IFREE on unallocated %llu (②)",
                    (unsigned long long)inum);
                mgr.inode_free(inum);
                si.bits[inum] = 0; si.alloc--;
                live_i[idx] = live_i.back(); live_i.pop_back();
            }
            logop(buf);

            /* ④⑤ 聚合对拍：每操作 */
            CHK(mgr.block_free_total() == BLK_BITS - sb.alloc,
                "agg blk free=%llu want %llu",
                (unsigned long long)mgr.block_free_total(),
                (unsigned long long)(BLK_BITS - sb.alloc));
            CHK(mgr.inode_free_total() == INO_BITS - si.alloc,
                "agg ino free=%llu want %llu",
                (unsigned long long)mgr.inode_free_total(),
                (unsigned long long)(INO_BITS - si.alloc));

            if (op > 0 && op % SCAN_EVERY == 0) scan_logical(mgr, sb, si);
        }

        scan_logical(mgr, sb, si);   /* 终局全量逐点（独立见证） */

        if (free_all_end) {          /* ⑦ all-alloc → all-free */
            uint64_t i = 0;
            while (i < BLK_BITS) {
                if (sb.bits[i]) {
                    uint64_t j = i;
                    while (j < BLK_BITS && sb.bits[j]) j++;
                    mgr.block_free(i, j - i);
                    i = j;
                } else i++;
            }
            sb.alloc = 0; sb.bits.assign(BLK_BITS, 0);
            for (uint64_t j = 0; j < INO_BITS; j++)
                if (si.bits[j]) { mgr.inode_free(j); si.bits[j] = 0; }
            si.alloc = 0;
            CHK(mgr.block_free_total() == BLK_BITS, "after free-all blk free=%llu want %llu (⑦)",
                (unsigned long long)mgr.block_free_total(), (unsigned long long)BLK_BITS);
            CHK(mgr.inode_free_total() == INO_BITS, "after free-all ino free=%llu want %llu (⑦)",
                (unsigned long long)mgr.inode_free_total(), (unsigned long long)INO_BITS);
            for (int t = 0; t < 5000; t++) {     /* 抽样见证逻辑全空 */
                uint64_t b = r() % BLK_BITS, j = r() % INO_BITS;
                CHK(!mgr.block_test(b), "after free-all blk_test(%llu)=true (⑦)",
                    (unsigned long long)b);
                CHK(!mgr.inode_test(j), "after free-all inode_test(%llu)=true (⑦)",
                    (unsigned long long)j);
            }
        }
    }   /* dtor：逐片 shutdown → 落盘 */

    /* ⑥-同构：flush（dtor shutdown）后盘字节 == 影子（记录合并/脏窗回写见证） */
    uint64_t mism = 0;
    for (uint64_t i = 0; i < BLK_BITS; i++)
        if (pbit(disk, i) != (sb.bits[i] != 0)) mism++;
    for (uint64_t j = 0; j < INO_BITS; j++)
        if (pbit(disk, INO_BIT_OFF + j) != (si.bits[j] != 0)) mism++;
    CHK(mism == 0, "flush physical mismatch=%llu (blk/ino bits)", (unsigned long long)mism);
    std::printf("  [%s] done: blk_alloc=%llu live_ext=%zu ino_alloc=%llu disk rd=%llu wr=%llu\n",
                tag, (unsigned long long)sb.alloc, live_b.size(),
                (unsigned long long)si.alloc, (unsigned long long)disk.rd,
                (unsigned long long)disk.wr);
}

/* ───────────────────────── run C：片层 chunks_asyn / calib 语义 ───────────────────────── */

static void stress_slice_asyn(uint64_t seed, uint64_t nops) {
    g_phase = "slice-asyn"; g_seed = seed; g_opidx = 0; g_ring.clear();
    const uint64_t SBLOCKS = 10;
    const uint8_t  SORDER = 1;                 /* chunk = 8192B = 65536 位 */
    const uint64_t SBITS = SBLOCKS << 15;      /* 327680 位，5 窗，尾窗 65536 位 */
    std::printf("[slice-asyn] chunk_order=%u blocks=%llu bits=%llu seed=%llu ops=%llu\n",
                (unsigned)SORDER, (unsigned long long)SBLOCKS,
                (unsigned long long)SBITS, (unsigned long long)seed,
                (unsigned long long)nops);

    mgr_disk_t disk;
    disk.data.assign((size_t)SBLOCKS * 4096, 0);
    slice_soul_t soul{};
    soul.read_block = disk_read;
    soul.write_block = disk_write;
    soul.io_ctx = &disk;
    soul.chunk_base_blk = 0;
    soul.chunk_blk_length = SBLOCKS;
    soul.chunk_order = SORDER;

    chunk_slice_mgr_t s;
    shadow_t sb;
    sb.bits.assign(SBITS, 0); sb.total = SBITS;
    std::vector<ext_t> live;
    std::mt19937_64 r(seed);

    CHK(s.enable(&soul, 0) == RC_OK, "enable");
    CHK(s.get_free_count() == SBITS, "init free=%llu (want %llu)",
        (unsigned long long)s.get_free_count(), (unsigned long long)SBITS);

    for (uint64_t op = 0; op < nops; op++) {
        g_opidx = op;
        char buf[192];
        const uint64_t alloc = sb.alloc;
        int kind;
        if (alloc * 100 >= SBITS * 70) kind = 2;
        else if (alloc * 100 <= SBITS * 20) kind = (r() % 100 < 80) ? 0 : 1;
        else {
            uint32_t z = (uint32_t)(r() % 100);
            kind = z < 42 ? 0 : z < 65 ? 1 : z < 88 ? 2 : 3;
        }
        if (kind == 2 && live.empty()) kind = 0;

        if (kind == 0) {
            uint64_t count = gen_count(r);
            uint8_t al = (uint8_t)(r() % 16);
            uint64_t g = 0;
            result_code_t rc = s.alloc_hard(al, count, g);
            if (rc == RC_OK) {
                std::snprintf(buf, sizeof buf, "HARD a=%u c=%llu -> %llu",
                              (unsigned)al, (unsigned long long)count, (unsigned long long)g);
                const uint64_t amask = (al ? (1ULL << al) : 1ULL) - 1;
                CHK(g + count <= SBITS, "s.HARD overflow");
                CHK((g & amask) == 0, "s.HARD align a=%u g=%llu", (unsigned)al,
                    (unsigned long long)g);
                bool dup = false;
                for (uint64_t i = 0; i < count; i++) if (sb.bits[g + i]) { dup = true; break; }
                CHK(!dup, "s.HARD dup (①)");
                for (uint64_t i = 0; i < count; i++) sb.bits[g + i] = 1;
                sb.alloc += count; live.push_back({g, count});
            } else {
                std::snprintf(buf, sizeof buf, "HARD a=%u c=%llu -> rc=%d",
                              (unsigned)al, (unsigned long long)count, (int)rc);
            }
        } else if (kind == 1) {
            uint64_t count = gen_count(r), left = 0, g = 0;
            result_code_t rc = s.alloc_soft(0, count, g, left);
            if (rc == RC_OK) {
                uint64_t got = (left <= count) ? count - left : 0;
                std::snprintf(buf, sizeof buf, "SOFT c=%llu -> %llu got=%llu",
                              (unsigned long long)count, (unsigned long long)g,
                              (unsigned long long)got);
                CHK(got >= 1 && g + got <= SBITS, "s.SOFT range");
                bool dup = false;
                for (uint64_t i = 0; i < got; i++) if (sb.bits[g + i]) { dup = true; break; }
                CHK(!dup, "s.SOFT dup (①)");
                for (uint64_t i = 0; i < got; i++) sb.bits[g + i] = 1;
                sb.alloc += got; live.push_back({g, got});
            } else {
                std::snprintf(buf, sizeof buf, "SOFT c=%llu -> rc=%d",
                              (unsigned long long)count, (int)rc);
            }
        } else if (kind == 2) {
            size_t idx = (size_t)(r() % live.size());
            ext_t e = live[idx];
            uint64_t cnt = (r() % 100 < 60) ? e.len : (1 + r() % e.len);
            if (cnt > e.len) cnt = e.len;
            uint64_t off = (e.len > cnt) ? (r() % (e.len - cnt + 1)) : 0;
            uint64_t p = e.start + off;
            std::snprintf(buf, sizeof buf, "FREE  %llu+%llu", (unsigned long long)p,
                          (unsigned long long)cnt);
            bool bad = false;
            for (uint64_t i = 0; i < cnt; i++) if (!sb.bits[p + i]) { bad = true; break; }
            CHK(!bad, "s.FREE unallocated (②)");
            CHK(s.free_interval(p, cnt) == RC_OK, "s.free_interval rc");
            for (uint64_t i = 0; i < cnt; i++) sb.bits[p + i] = 0;
            sb.alloc -= cnt;
            live[idx] = live.back(); live.pop_back();
            if (off > 0) live.push_back({e.start, off});
            uint64_t right = e.len - off - cnt;
            if (right > 0) live.push_back({p + cnt, right});
        } else {
            uint64_t base = r() % SBITS;
            uint64_t count = gen_occ(r), rest = 0;
            result_code_t rc = s.soft_occupy(base, count, &rest);
            if (rc == RC_OK) {
                uint64_t claimed = (rest <= count) ? count - rest : 0;
                std::snprintf(buf, sizeof buf, "OCC   %llu c=%llu claimed=%llu",
                              (unsigned long long)base, (unsigned long long)count,
                              (unsigned long long)claimed);
                CHK(claimed >= 1 && base + claimed <= SBITS, "s.OCC range (③)");
                bool bad = false;
                for (uint64_t i = 0; i < claimed; i++) if (sb.bits[base + i]) { bad = true; break; }
                CHK(!bad, "s.OCC not contiguous-free (①③)");
                for (uint64_t i = 0; i < claimed; i++) sb.bits[base + i] = 1;
                sb.alloc += claimed; live.push_back({base, claimed});
            } else {
                std::snprintf(buf, sizeof buf, "OCC   %llu c=%llu -> rc=%d rest=%llu",
                              (unsigned long long)base, (unsigned long long)count,
                              (int)rc, (unsigned long long)rest);
            }
        }
        logop(buf);

        CHK(s.get_free_count() == SBITS - sb.alloc, "s.agg free=%llu want %llu (④⑤)",
            (unsigned long long)s.get_free_count(),
            (unsigned long long)(SBITS - sb.alloc));

        if (op > 0 && op % 2000 == 0) {
            uint64_t pre = s.get_free_count();
            free_count_calib_t cal{};
            CHK(s.chunks_asyn(&cal) == RC_OK, "s.chunks_asyn rc");
            CHK(cal.old_free_count == pre, "calib.old=%llu pre=%llu (⑥)",
                (unsigned long long)cal.old_free_count, (unsigned long long)pre);
            CHK(cal.new_free_count == s.get_free_count(), "calib.new=%llu fc=%llu (⑥)",
                (unsigned long long)cal.new_free_count,
                (unsigned long long)s.get_free_count());
            CHK(cal.new_free_count == disk_zeros(disk, SBITS),
                "calib.new=%llu disk_zeros=%llu (⑥)",
                (unsigned long long)cal.new_free_count,
                (unsigned long long)disk_zeros(disk, SBITS));
            CHK(cal.old_free_count == cal.new_free_count,
                "unexpected drift old=%llu new=%llu",
                (unsigned long long)cal.old_free_count,
                (unsigned long long)cal.new_free_count);
            for (uint64_t i = 0; i < SBITS; i++)
                CHK(pbit(disk, i) == (sb.bits[i] != 0),
                    "asyn physical mismatch @%llu", (unsigned long long)i);
        }
    }

    /* 末尾 asyn：⑥ free_count 与重扫一致 */
    {
        uint64_t pre = s.get_free_count();
        free_count_calib_t cal{};
        CHK(s.chunks_asyn(&cal) == RC_OK, "final asyn rc");
        CHK(cal.old_free_count == pre && cal.new_free_count == s.get_free_count(),
            "final calib old=%llu new=%llu fc=%llu",
            (unsigned long long)cal.old_free_count,
            (unsigned long long)cal.new_free_count,
            (unsigned long long)s.get_free_count());
        CHK(cal.new_free_count == disk_zeros(disk, SBITS), "final calib.new vs disk_zeros");
        uint64_t mism = 0;
        for (uint64_t i = 0; i < SBITS; i++)
            if (pbit(disk, i) != (sb.bits[i] != 0)) mism++;
        CHK(mism == 0, "final physical mismatch=%llu", (unsigned long long)mism);
    }

    /* ⑥ calib 漂移语义：人为重复 free（片层允许）→ old > new */
    {
        uint64_t p = SBITS;
        for (uint64_t i = 0; i < SBITS; i++) if (sb.bits[i]) { p = i; break; }
        if (p < SBITS) {
            uint64_t pre = s.get_free_count();
            CHK(s.free_interval(p, 1) == RC_OK, "dupfree #1");
            CHK(s.free_interval(p, 1) == RC_OK, "dupfree #2");
            CHK(s.get_free_count() == pre + 2, "dupfree fc=%llu want %llu",
                (unsigned long long)s.get_free_count(), (unsigned long long)(pre + 2));
            free_count_calib_t cal{};
            CHK(s.chunks_asyn(&cal) == RC_OK, "asyn after dup");
            CHK(cal.old_free_count == pre + 2, "calib.old=%llu want %llu",
                (unsigned long long)cal.old_free_count, (unsigned long long)(pre + 2));
            CHK(cal.new_free_count == disk_zeros(disk, SBITS), "calib.new vs disk_zeros");
            CHK(cal.old_free_count > cal.new_free_count, "drift not detected old=%llu new=%llu",
                (unsigned long long)cal.old_free_count,
                (unsigned long long)cal.new_free_count);
            CHK(s.get_free_count() == cal.new_free_count, "fc==new after calib");
            CHK(!pbit(disk, p), "physical bit %llu cleared", (unsigned long long)p);
            sb.bits[p] = 0; sb.alloc--;
        } else {
            std::printf("  [slice-asyn] (no allocated bit to dup-free; skipped drift check)\n");
        }
    }

    CHK(s.shutdown() == RC_OK, "shutdown");
    std::printf("  [slice-asyn] done: blk_alloc=%llu live=%zu disk rd=%llu wr=%llu\n",
                (unsigned long long)sb.alloc, live.size(),
                (unsigned long long)disk.rd, (unsigned long long)disk.wr);
}

int main(int argc, char **argv) {
    uint64_t seed = (argc > 1) ? std::strtoull(argv[1], nullptr, 0) : 0xC0FFEEULL;
    uint64_t ops  = (argc > 2) ? std::strtoull(argv[2], nullptr, 0) : 200000ULL;
    std::printf("=== stress_v2_random seed=%llu ops=%llu ===\n",
                (unsigned long long)seed, (unsigned long long)ops);

    stress_mgr_run(seed, ops, true, "runA");        /* ①-⑤⑦ mgr 级，末尾清空 */
    stress_mgr_run(seed + 1, 30000, false, "runB"); /* ⑥-同构：flush 后盘==影子 */
    stress_slice_asyn(seed + 2, 60000);             /* ⑥ chunks_asyn + calib 语义 */

    if (g_fail == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILED: %d\n", g_fail);
    return 1;
}

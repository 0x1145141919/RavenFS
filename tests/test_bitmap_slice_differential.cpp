/* ═══════════════════════════════════════════════════════════════════
 * test_bitmap_slice_differential.cpp — FULL ↔ CHUNK 双模式对拍（Phase A-1）
 *
 * 最强等价性 oracle：同一种子、同一随机操作序列，分别驱动
 *   FULL（全量常驻，03 §1 基线红线）与 CHUNK（懒加载 + mock 盘），
 * 每步与**影子位图**三重对账（返回值 / free_count / block_test），
 * 终态 TTL 收尾后 **整盘镜像 memcmp + free_count 对照必须全等**。
 *
 * 结构：
 *   S 组 = 顺序对拍（op 全集：alloc_hard / alloc_soft / occupy / free / test
 *         + inode_alloc/free/test；种子矩阵 × 6 种几何）
 *   C 组 = 并发对拍（4 线程；阶段化：F 阶段=分区 free/test 并发 → 屏障 →
 *         A 阶段=全同参 alloc(1) 并发（伪 ENOSPC 重试）→ 屏障；两模式各自跑
 *         同一脚本，终态互 memcmp + 计数对照。阶段化理由：分区外的 alloc 落点
 *         由调度决定，跨模式可比性要求操作**可交换**（同参 alloc 可交换，
 *         混合参数不可）。
 *         ⚠ occupy 不进并发段：CHUNK occupy 撞锁忙即放弃（BUG#3，见阶段 F 注）
 *         → 其并发放大由 churn 覆盖并登记；本段保证其余原语的终态对拍收口。
 *
 * 已知 bug 容忍（**只登记复现单不改实现**；签名见下，容忍仅用于继续挖新 bug）：
 *   KB1 = occupy 落在「摘要 ALL_USED 且无 pending record」的窗（§7 免载入
 *         跳过）→ chunk_soft_occupy 在首窗 break 时不更新 *rest_bits →
 *         路由层算出 claimed = remain = 全额虚报（位没置、计数没动）。
 *         容忍条件：**状态与 oracle 一致**（free_count/镜像无损）→ 登记后继续。
 *   BUG3 = 并发下 chunk_soft_occupy 撞片锁忙（RC_CHUNK_BUSY）直接放弃定点占领
 *         （FULL 自旋强抢不放弃；§10.10 违例）→ 并发段不跑 occupy，由 churn 登记。
 *   BUG4 = chunk_test_bit 换片竞态保守 true（online≠c → `: true`）→ 并发 test
 *         假报已用；以「重试取真值」甄别（状态未损则登记继续）。
 *   KB2 = 离线 record 路径 `free_count += n` 不数实际 1→0 位 → 对**已空闲位**
 *         重复 free 时 free_total 虚高（FULL 精确）。签名：free 后 chunk 计数
 *         增量 == 区间长 n 且 > 实际清位数 → 登记 + 计数模型吸收后继续。
 *   其余任何分歧（含状态级分歧）→ 打印完整复现单（几何/种子/op 序号/参数/
 *         期望/实际）→ 终止本轮。
 *
 * 2026-09-28 · Docs/runtime_tech/03_chunk_backed_slices.md §10 验收
 * ═══════════════════════════════════════════════════════════════════ */
#include "chunk_common.hpp"

#include <chrono>
#include <string>
#include <thread>

#define TICKET(...) do { \
    g_fail++; std::printf("  [BUG] "); std::printf(__VA_ARGS__); std::printf("\n"); \
} while (0)

/* ═══ 种子矩阵 × 几何矩阵（含 k mod 2^order ≠ 0 尾片 / order 0·1·4·8 /
 *    多片 / 单片 / 极小位图）═══ */
static const geom_t GEOMS[] = {
    /* name                        blk_bits  ino_bits  cpu  order */
    {"G-A order0 cpu2 15p/5p",      491520,   163840,   2,   0},  /* chunk=4K；片1=7 页 */
    {"G-B order1 cpu2 15p/5p",      491520,   163840,   2,   1},  /* 尾窗恰 1 页（既有 125 项同款） */
    {"G-C order4 cpu1 20p/17p",     655360,   557056,   1,   4},  /* 单片；尾 4p/1p */
    {"G-D order8 cpu1 375p/325p", 12288000, 10649600,   1,   8},  /* chunk=1MB；尾 119p/69p */
    {"G-E order0 cpu4 tiny 1p/1p",   32768,    32768,   4,   0},  /* 极小位图：1 片 1 窗 */
    {"G-F order4 cpu2 40p/34p",    1310720,  1114112,   2,   4},  /* 多片 order4：尾 4p/1p */
};
static const uint64_t SEEDS[] = { 0xA11CE5EEDull, 0xB0B5EEDull, 0xC0FFEE11ull };

/* 运行级 bug 统计（KB1/KB2 每轮每类登记一次完整复现单，其余计数汇总） */
struct kbstats_t { int kb1 = 0, kb2 = 0; int kb1_ticketed = 0, kb2_ticketed = 0; };
static int g_kb1_total = 0, g_kb2_total = 0, g_bug4_total = 0;

/* ═══════════════════════════════════════════════════════════════════
 * S 组：顺序对拍
 * ═══════════════════════════════════════════════════════════════════ */
static bool seq_run(const geom_t &g, uint64_t seed, int nops, kbstats_t &kb) {
    std::vector<uint8_t> init_b, init_i;
    gen_init(g, seed, init_b, init_i);

    side_t full, chunk;
    if (!full.mount(g, init_b, init_i, /*want_chunk=*/false)) {
        TICKET("FULL 装配失败 geom=%s seed=%llx rc=%llu", g.name,
               (unsigned long long)seed, (unsigned long long)full.rc);
        return false;
    }
    if (!chunk.mount(g, init_b, init_i, /*want_chunk=*/true)) {
        TICKET("CHUNK 装配失败 geom=%s seed=%llx rc=%llu", g.name,
               (unsigned long long)seed, (unsigned long long)chunk.rc);
        return false;
    }
    /* 几何对拍：slice_count / per_slice 必须两模式一致且 == 公式值 */
    const uint64_t sc = derive_slice_count(g);
    CHECK(full.mgr->slice_count() == sc && chunk.mgr->slice_count() == sc,
          "%s seed=%llx slice_count full=%llu chunk=%llu 期望 %llu", g.name,
          (unsigned long long)seed, (unsigned long long)full.mgr->slice_count(),
          (unsigned long long)chunk.mgr->slice_count(), (unsigned long long)sc);
    CHECK(full.mgr->block_per_slice_bits() == chunk.mgr->block_per_slice_bits(),
          "%s per_slice 两模式一致", g.name);

    /* 影子 = 初值 ⊕ 操作；zb/zi = FULL 侧 oracle 计数；mb/mi = CHUNK 侧
     * 计数模型（= oracle + KB2 吸收的虚高偏差） */
    std::vector<uint8_t> sb = init_b, si = init_i;
    uint64_t zb = img_zeros(init_b.data(), init_b.size());
    uint64_t zi = img_zeros(init_i.data(), init_i.size());
    uint64_t mb = zb, mi = zi;

    auto cnt_ok = [&](int op, const char *stage) -> bool {
        if (full.free_total(false) != zb) {
            TICKET("FULL block_free_total 分歧 geom=%s seed=%llx op=%d stage=%s 实得 %llu 期望 %llu",
                   g.name, (unsigned long long)seed, op, stage,
                   (unsigned long long)full.free_total(false), (unsigned long long)zb);
            return false;
        }
        if (chunk.free_total(false) != mb) {
            TICKET("CHUNK block_free_total 分歧 geom=%s seed=%llx op=%d stage=%s 实得 %llu 模型 %llu（oracle %llu）",
                   g.name, (unsigned long long)seed, op, stage,
                   (unsigned long long)chunk.free_total(false),
                   (unsigned long long)mb, (unsigned long long)zb);
            return false;
        }
        if (full.free_total(true) != zi || chunk.free_total(true) != mi) {
            TICKET("inode free_total 分歧 geom=%s seed=%llx op=%d stage=%s full=%llu/%llu chunk=%llu/%llu(模型 %llu)",
                   g.name, (unsigned long long)seed, op, stage,
                   (unsigned long long)full.free_total(true), (unsigned long long)zi,
                   (unsigned long long)chunk.free_total(true), (unsigned long long)mi,
                   (unsigned long long)zi);
            return false;
        }
        return true;
    };
    if (!cnt_ok(-1, "mount")) return false;

    const uint64_t chunk_bits = PAGE_BITS << g.order;
    std::vector<win_t> wb, wi;
    build_windows(g.blk_bits, derive_per_slice(g.blk_bits, sc), sc, chunk_bits, wb);
    build_windows(g.ino_bits, derive_per_slice(g.ino_bits, sc), sc, chunk_bits, wi);

    rng_t rng(seed ^ 0x5E9E17A11ull);
    /* [dbg] inode 触碰轨迹（定位影子漂移） */
    std::vector<std::pair<int, uint64_t>> ino_touch(g.ino_bits, {-1, 0});
    const uint8_t aligns[6] = {0, 1, 2, 3, 5, 12};

    for (int op = 0; op < nops; op++) {
        const int kind = (int)rng.below(100);

        /* ── alloc_soft（对齐恒 0，同 mgr 调用口径） ── */
        if (kind < 20) {
            const uint64_t n = 1 + rng.below(64);
            uint64_t lf = n, lc = n;
            const uint64_t a = full.mgr->block_alloc_soft(n, &lf);
            const uint64_t b = chunk.mgr->block_alloc_soft(n, &lc);
            if (a != b || lf != lc) {
                TICKET("alloc_soft 分歧 geom=%s seed=%llx op=%d n=%llu "
                       "FULL=(%s,%llu) CHUNK=(%s,%llu)", g.name, (unsigned long long)seed,
                       op, (unsigned long long)n, a == PTR_INVALID ? "INVALID" : "OK",
                       (unsigned long long)lf, b == PTR_INVALID ? "INVALID" : "OK",
                       (unsigned long long)lc);
                return false;
            }
            if (a != PTR_INVALID) {
                const uint64_t got = n - lf;
                for (uint64_t k = 0; k < got; k++) {
                    if (img_bit(sb.data(), a + k)) {
                        TICKET("alloc_soft 双分配 geom=%s seed=%llx op=%d base=%llu k=%llu",
                               g.name, (unsigned long long)seed, op,
                               (unsigned long long)a, (unsigned long long)k);
                        return false;
                    }
                    img_set(sb.data(), a + k);
                }
                zb -= got;
                mb -= got;
            }
            if (!cnt_ok(op, "alloc_soft")) return false;
            continue;
        }

        /* ── alloc_hard（含对齐；单窗 find 是 §8 已知形态差异 → margin 纪律
         *    保证 0-run 不跨窗 ⇒ 两模式必须逐位等价） ── */
        if (kind < 34) {
            const uint64_t n = 1 + rng.below(64);
            const uint8_t al = aligns[rng.below(6)];
            const uint64_t a = full.mgr->block_alloc_hard(n, al);
            const uint64_t b = chunk.mgr->block_alloc_hard(n, al);
            if (a != b) {
                TICKET("alloc_hard 分歧 geom=%s seed=%llx op=%d n=%llu align=%u "
                       "FULL=%s%llu CHUNK=%s%llu", g.name, (unsigned long long)seed, op,
                       (unsigned long long)n, (unsigned)al,
                       a == PTR_INVALID ? "INVALID +" : "", (unsigned long long)a,
                       b == PTR_INVALID ? "INVALID +" : "", (unsigned long long)b);
                return false;
            }
            if (a != PTR_INVALID) {
                for (uint64_t k = 0; k < n; k++) {
                    if (img_bit(sb.data(), a + k)) {
                        TICKET("alloc_hard 双分配 geom=%s seed=%llx op=%d base=%llu",
                               g.name, (unsigned long long)seed, op, (unsigned long long)a);
                        return false;
                    }
                    img_set(sb.data(), a + k);
                }
                zb -= n;
                mb -= n;
            }
            if (!cnt_ok(op, "alloc_hard")) return false;
            continue;
        }

        /* ── occupy（锚定贪心；oracle = 影子贪心前缀） ── */
        if (kind < 50) {
            uint64_t base = rng.below(g.blk_bits);
            uint64_t len = 1 + rng.below(128);
            if (base + len > g.blk_bits) len = g.blk_bits - base;
            if (len == 0) continue;
            const uint64_t oracle = shadow_greedy(sb.data(), base, len);
            uint64_t rf = 0, rc2 = 0;
            const uint64_t vf = full.mgr->block_occupy_soft(base, len, &rf);
            const uint64_t vc = chunk.mgr->block_occupy_soft(base, len, &rc2);
            const uint64_t cf = (vf == PTR_INVALID) ? 0 : len - rf;
            const uint64_t cc = (vc == PTR_INVALID) ? 0 : len - rc2;
            if (cf != oracle) {
                TICKET("ORACLE 失配（FULL occupy != 影子贪心）geom=%s seed=%llx op=%d "
                       "base=%llu len=%llu FULL=%llu oracle=%llu",
                       g.name, (unsigned long long)seed, op, (unsigned long long)base,
                       (unsigned long long)len, (unsigned long long)cf,
                       (unsigned long long)oracle);
                return false;
            }
            if (cc != cf) {
                /* KB1 签名分类：状态仍与 oracle 一致 = 仅返回值虚报/漏报 */
                if (chunk.free_total(false) == mb - oracle &&
                    full.free_total(false) == zb - oracle) {
                    kb.kb1++;
                    if (!kb.kb1_ticketed) {
                        kb.kb1_ticketed = 1;
                        TICKET("KB1 occupy 虚报（已知，容忍继续）geom=%s seed=%llx op=%d "
                               "base=%llu len=%llu oracle_claim=%llu CHUNK_claim=%llu "
                               "→ 期望 PTR_INVALID/rest=%llu，实际 ret=%s/rest=%llu（无位被置）"
                               "复现：occupy 锚点落在摘要 ALL_USED 窗（§7 免载入）→ "
                               "chunk_soft_occupy 首窗 break 不回填 *rest_bits → "
                               "路由 claimed=remain 全额虚报（双分配方向，§10 禁）",
                               g.name, (unsigned long long)seed, op,
                               (unsigned long long)base, (unsigned long long)len,
                               (unsigned long long)oracle, (unsigned long long)cc,
                               (unsigned long long)len,
                               vc == PTR_INVALID ? "PTR_INVALID" : "blkbase",
                               (unsigned long long)rc2);
                    }
                } else {
                    TICKET("occupy 状态级分歧 geom=%s seed=%llx op=%d base=%llu len=%llu "
                           "oracle=%llu FULL_claim=%llu CHUNK_claim=%llu "
                           "full_cnt=%llu chunk_cnt=%llu 模型=%llu",
                           g.name, (unsigned long long)seed, op, (unsigned long long)base,
                           (unsigned long long)len, (unsigned long long)oracle,
                           (unsigned long long)cf, (unsigned long long)cc,
                           (unsigned long long)full.free_total(false),
                           (unsigned long long)chunk.free_total(false),
                           (unsigned long long)mb);
                    return false;
                }
            }
            /* oracle 记账（KB1 容忍路径同样以 oracle 为真值：状态已证一致） */
            for (uint64_t k = 0; k < oracle; k++) img_set(sb.data(), base + k);
            zb -= oracle;
            mb -= oracle;
            if (!cnt_ok(op, "occupy")) return false;
            continue;
        }

        /* ── free（窗口内 margin 收敛；喂 KB2：允许双清已空闲位） ── */
        if (kind < 68) {
            const win_t &w = wb[rng.below(wb.size())];
            const uint64_t lo = w.lo + WIN_MARGIN, hi = w.hi - WIN_MARGIN;
            if (hi <= lo) continue;
            const uint64_t base = lo + rng.below(hi - lo);
            uint64_t len = 1 + rng.below(128);
            if (base + len > hi) len = hi - base;
            if (len == 0) continue;
            const uint64_t cleared = shadow_ones(sb.data(), base, len);
            full.mgr->block_free(base, len);
            chunk.mgr->block_free(base, len);
            zb += cleared;
            const uint64_t cf_now = chunk.free_total(false);
            if (cf_now == mb + cleared) {
                mb += cleared;                     /* 正常：增量 = 实际 1→0 位数 */
            } else if (cf_now == mb + len && len > cleared) {
                /* KB2 签名：离线 record 路径按区间长记账（实际只清 cleared 位） */
                kb.kb2++;
                if (!kb.kb2_ticketed) {
                    kb.kb2_ticketed = 1;
                    TICKET("KB2 离线 free 计数虚高（已知，容忍继续）geom=%s seed=%llx "
                           "op=%d free(%llu,%llu) 盘态已空闲 %llu 位 → "
                           "FULL +%llu（精确）CHUNK +%llu（==区间长）→ free_total 虚高 %llu"
                           " 复现：离线窗挂 record 时 `free_count += n` 不数实际清位（§10.6 计数保真违例；"
                           "statfs/block_free_total 谎报）",
                           g.name, (unsigned long long)seed, op, (unsigned long long)base,
                           (unsigned long long)len, (unsigned long long)(len - cleared),
                           (unsigned long long)cleared, (unsigned long long)len,
                           (unsigned long long)(len - cleared));
                }
                mb = cf_now;                       /* 吸收偏差，继续挖新 bug */
            } else {
                TICKET("CHUNK free 计数异常分歧 geom=%s seed=%llx op=%d free(%llu,%llu) "
                       "cleared=%llu delta=%lld 模型=%llu",
                       g.name, (unsigned long long)seed, op, (unsigned long long)base,
                       (unsigned long long)len, (unsigned long long)cleared,
                       (long long)(cf_now - mb), (unsigned long long)mb);
                return false;
            }
            for (uint64_t k = 0; k < len; k++) img_clr(sb.data(), base + k);
            if (!cnt_ok(op, "free")) return false;
            continue;
        }

        /* ── test（三重对账：FULL == CHUNK == 影子） ── */
        if (kind < 78) {
            const int k = 1 + (int)rng.below(8);
            for (int j = 0; j < k; j++) {
                const uint64_t bit = rng.below(g.blk_bits);
                const bool t = full.mgr->block_test(bit);
                const bool u = chunk.mgr->block_test(bit);
                const bool s = img_bit(sb.data(), bit);
                if (t != s) {
                    TICKET("FULL block_test != 影子 geom=%s seed=%llx op=%d bit=%llu "
                           "full=%d shadow=%d", g.name, (unsigned long long)seed, op,
                           (unsigned long long)bit, (int)t, (int)s);
                    return false;
                }
                if (u != s) {
                    TICKET("CHUNK block_test != 影子 geom=%s seed=%llx op=%d bit=%llu "
                           "chunk=%d shadow=%d（状态/摘要分歧）", g.name,
                           (unsigned long long)seed, op, (unsigned long long)bit, (int)u,
                           (int)s);
                    return false;
                }
            }
            continue;
        }

        /* ── inode_alloc ── */
        if (kind < 86) {
            const uint64_t a = full.mgr->inode_alloc();
            const uint64_t b = chunk.mgr->inode_alloc();
            if (a != b) {
                TICKET("inode_alloc 分歧 geom=%s seed=%llx op=%d FULL=%llu CHUNK=%llu",
                       g.name, (unsigned long long)seed, op, (unsigned long long)a,
                       (unsigned long long)b);
                return false;
            }
            if (a != PTR_INVALID) {
                if (img_bit(si.data(), a)) {
                    TICKET("inode_alloc 双分配 geom=%s seed=%llx op=%d inum=%llu "
                           "[dbg] FULL_raw=%d CHUNK_disk=%d shadow=%d 上次触碰 op=%d kind=%c "
                           "full_cnt=%llu/%llu chunk_cnt=%llu/%llu",
                           g.name, (unsigned long long)seed, op, (unsigned long long)a,
                           (int)img_bit(full.img(true), a), (int)img_bit(chunk.img(true), a),
                           (int)img_bit(si.data(), a), ino_touch[a].first,
                           (char)ino_touch[a].second,
                           (unsigned long long)full.free_total(true), (unsigned long long)zi,
                           (unsigned long long)chunk.free_total(true), (unsigned long long)mi);
                    return false;
                }
                img_set(si.data(), a);
                zi--;
                mi--;
                ino_touch[a] = {op, 'A'};
            }
            if (!cnt_ok(op, "inode_alloc")) return false;
            continue;
        }

        /* ── inode_free（含重复释放 → 喂 inode 组 KB2） ── */
        if (kind < 94) {
            const uint64_t ino = rng.below(g.ino_bits);
            const uint64_t cleared = shadow_ones(si.data(), ino, 1);
            full.mgr->inode_free(ino);
            chunk.mgr->inode_free(ino);
            ino_touch[ino] = {op, 'F'};
            img_clr(si.data(), ino);        /* 影子同步清位（漏了会假报 inode 双分配） */
            zi += cleared;
            const uint64_t cf_now = chunk.free_total(true);
            if (cf_now == mi + cleared) {
                mi += cleared;
            } else if (cf_now == mi + 1 && cleared == 0) {
                kb.kb2++;                          /* KB2 同签名（inode 组） */
                if (!kb.kb2_ticketed) {
                    kb.kb2_ticketed = 1;
                    TICKET("KB2 inode 离线 free 计数虚高（已知）geom=%s seed=%llx "
                           "op=%d inode_free(%llu) 盘态已空闲 → CHUNK +1 FULL +0",
                           g.name, (unsigned long long)seed, op, (unsigned long long)ino);
                }
                mi = cf_now;
            } else {
                TICKET("CHUNK inode_free 计数异常 geom=%s seed=%llx op=%d inum=%llu "
                       "cleared=%llu delta=%lld 模型=%llu [dbg] FULL_raw=%d CHUNK_disk=%d "
                       "shadow=%d 上次触碰 op=%d kind=%c", g.name,
                       (unsigned long long)seed, op, (unsigned long long)ino,
                       (unsigned long long)cleared, (long long)(cf_now - mi),
                       (unsigned long long)mi, (int)img_bit(full.img(true), ino),
                       (int)img_bit(chunk.img(true), ino), (int)img_bit(si.data(), ino),
                       ino_touch[ino].first, (char)ino_touch[ino].second);
                return false;
            }
            if (!cnt_ok(op, "inode_free")) return false;
            continue;
        }

        /* ── inode_test ── */
        {
            const uint64_t ino = rng.below(g.ino_bits);
            const bool t = full.mgr->inode_test(ino);
            const bool u = chunk.mgr->inode_test(ino);
            const bool s = img_bit(si.data(), ino);
            if (t != s || u != s) {
                TICKET("inode_test 分歧 geom=%s seed=%llx op=%d inum=%llu full=%d "
                       "chunk=%d shadow=%d", g.name, (unsigned long long)seed, op,
                       (unsigned long long)ino, (int)t, (int)u, (int)s);
                return false;
            }
        }
    }

    /* ── 终态：TTL 收尾 → 整盘镜像 memcmp + free_count 对照 ── */
    chunk.finish();
    full.audit("seq/FULL");
    chunk.audit("seq/CHUNK");
    if (!cnt_ok(nops, "final")) return false;

    int bad = 0;
    for (int grp = 0; grp < 2; grp++) {
        const bool is_ino = grp == 1;
        const uint64_t bytes = full.bytes(is_ino);
        uint8_t *fimg = full.img(is_ino), *cimg = chunk.img(is_ino);
        uint8_t *simg = is_ino ? si.data() : sb.data();
        if (std::memcmp(fimg, cimg, bytes) != 0) {
            for (uint64_t i = 0; i < bytes * 8; i++)
                if (img_bit(fimg, i) != img_bit(cimg, i)) {
                    TICKET("★ 终态镜像 FULL≠CHUNK geom=%s seed=%llx grp=%s 首分歧 bit=%llu "
                           "FULL=%d CHUNK=%d", g.name, (unsigned long long)seed,
                           is_ino ? "inode" : "block", (unsigned long long)i,
                           (int)img_bit(fimg, i), (int)img_bit(cimg, i));
                    break;
                }
            bad++;
        }
        if (std::memcmp(fimg, simg, bytes) != 0) {
            for (uint64_t i = 0; i < bytes * 8; i++)
                if (img_bit(fimg, i) != img_bit(simg, i)) {
                    TICKET("★ 终态镜像 FULL≠影子 geom=%s seed=%llx grp=%s 首分歧 bit=%llu",
                           g.name, (unsigned long long)seed, is_ino ? "inode" : "block",
                           (unsigned long long)i);
                    break;
                }
            bad++;
        }
        if (std::memcmp(cimg, simg, bytes) != 0) {
            for (uint64_t i = 0; i < bytes * 8; i++)
                if (img_bit(cimg, i) != img_bit(simg, i)) {
                    TICKET("★ 终态镜像 CHUNK≠影子 geom=%s seed=%llx grp=%s 首分歧 bit=%llu",
                           g.name, (unsigned long long)seed, is_ino ? "inode" : "block",
                           (unsigned long long)i);
                    break;
                }
            bad++;
        }
    }
    (void)bad;
    std::printf("    [seq] %-26s seed=%016llx ops=%d KB1=%d KB2=%d %s\n", g.name,
                (unsigned long long)seed, nops, kb.kb1, kb.kb2,
                (kb.kb1 || kb.kb2) ? "（已知 bug 登记）" : "OK");
    g_kb1_total += kb.kb1;
    g_kb2_total += kb.kb2;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * C 组：并发对拍（阶段化，两模式跑同一脚本 → 终态互 memcmp）
 *  阶段 F（分区并发）：线程 t 只碰 [t 区) 的 free/test —— 结果与
 *    线程交错无关 ⇒ 跨模式可比；每步与影子严格对账（无并发 alloc ⇒ 影子
 *    在分区内精确）。free 只收影子中为 1 的子段（避免与 in-flight 双记）。
 *  阶段 A（全同参 alloc 并发）：所有线程同参 alloc_soft(1) —— 同参状态
 *    函数两两可交换 ⇒ 终态与交错无关（跨模式/跨次可比）；影子在全局锁下
 *    记账并顺带做「返回位必须原为空闲」双分配探测。
 * ═══════════════════════════════════════════════════════════════════ */
static void conc_pass(side_t &S, const geom_t &g, uint64_t seed,
                      std::vector<uint8_t> &sb, uint64_t &zb,
                      std::atomic<bool> &bad, std::mutex &tk_mu, kbstats_t &kb,
                      bool with_phaseA) {
    const int nthreads = 4;
    const int rounds = 3;
    const int nF = 300, nA = 120;
    const uint64_t region = g.blk_bits / nthreads;
    std::mutex M;   /* 影子锁（阶段 A 记账） */

    std::atomic<int> ev{0};   /* [dbg] 分歧事件日志上限计数 */
    auto ticket_th = [&](const char *fmt, auto &&...args) {
        std::lock_guard<std::mutex> lk(tk_mu);
        g_fail++;
        std::printf("  [BUG] ");
        std::printf(fmt, args...);
        std::printf("\n");
        bad = true;
    };

    for (int round = 0; round < rounds && !bad; round++) {
        /* ── 阶段 F：分区 free / test（occupy 因 BUG#3 不进并发段） ── */
        {
            std::vector<std::thread> th;
            std::vector<int64_t> dz(nthreads, 0);   /* 净额：occupy −claim / free +cleared */
            for (int t = 0; t < nthreads; t++) {
                th.emplace_back([&, t] {
                    t_fake_proc_id = (uint32_t)(t % S.mgr->cpu_slots());
                    rng_t r(seed ^ (0x10001ull * (uint64_t)(round + 1)) ^
                            (0x9E3779B1ull * (uint64_t)(t + 1)));
                    const uint64_t lo = (uint64_t)t * region;
                    const uint64_t hi = (t == nthreads - 1) ? g.blk_bits : lo + region;
                    for (int i = 0; i < nF && !bad; i++) {
                        const int k = (int)r.below(10);
                        /* occupy 不进并发对拍段：CHUNK chunk_soft_occupy 撞片锁忙
                         * （RC_CHUNK_BUSY）会**直接放弃整个定点占领**（FULL 自旋强抢
                         * 永不放弃；§10.10 + 头注「free/定点自旋强抢」自毁约）→ 已单开
                         * BUG 复现单。occupy 并发可靠性由 churn（影子协议可容忍并登记）
                         * 覆盖；本段跑免该 bug 的 free/test，保证跨模式终态对拍能收口。 */
                        if (k < 5) {                     /* free（只清影子为 1 的子段） */
                            const uint64_t w = r.below(region) + lo;
                            uint64_t base = w, end = w;
                            if (base + 128 > hi) base = hi - 128;
                            if (base < lo) base = lo;
                            end = base + 1 + r.below(128);
                            if (end > hi) end = hi;
                            uint64_t p = base;
                            while (p < end) {
                                if (!img_bit(sb.data(), p)) { p++; continue; }
                                uint64_t q = p;
                                while (q < end && img_bit(sb.data(), q)) q++;
                                for (uint64_t x = p; x < q; x++) img_clr(sb.data(), x);
                                dz[t] += (int64_t)(q - p);
                                S.mgr->block_free(p, q - p);
                                p = q;
                            }
                        } else {                         /* test（分区精确） */
                            const uint64_t bit = lo + r.below(hi - lo);
                            const bool u = S.mgr->block_test(bit);
                            const bool sh = img_bit(sb.data(), bit);
                            if (u != sh && sh == false) {
                                /* 区分 BUG#4（chunk_test_bit 的 window-switch 竞态保守
                                 * true：换片后 online≠c → `: true` 把空闲位报成已用，
                                 * §1.1 语义与 FULL 不等）与真状态分歧：重试取真值。
                                 * 重试仍恒 true ⇒ 位确实被置 ⇒ 真状态分歧（硬失败）。 */
                                bool recovered = false;
                                for (int t5 = 0; t5 < 5 && !recovered; t5++) {
                                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                                    if (!S.mgr->block_test(bit)) recovered = true;
                                }
                                if (recovered) {
                                    std::lock_guard<std::mutex> lk(tk_mu);
                                    g_bug4_total++;
                                    if (ev.fetch_add(1) < 6) {
                                    g_fail++;   /* BUG4 复现单（前 6 条完整，之后只计数） */
                                        std::printf("  [BUG] BUG#4 chunk_test_bit 竞态保守 true"
                                                    " geom=%s seed=%llx round=%d thr=%d op=%d bit=%llu "
                                                    "首次读=1（换片竞态），重试读=0；shadow=0 ⇒ 假报已用"
                                                    "（§1.1 test 语义与 FULL 不等；调用方将漏分配）\n",
                                                    g.name, (unsigned long long)seed, round, t, i,
                                                    (unsigned long long)bit);
                                    }
                                    continue;   /* 状态未损：继续压 */
                                }
                                std::lock_guard<std::mutex> lk(tk_mu);
                                g_fail++;
                                std::printf("  [BUG] 分区 test 真状态分歧 geom=%s seed=%llx round=%d "
                                            "thr=%d op=%d bit=%llu mgr=1 shadow=0（重试恒 1）"
                                            " near mgr=%d%d%d%d shadow=%d%d%d%d\n", g.name,
                                            (unsigned long long)seed, round, t, i,
                                            (unsigned long long)bit,
                                            (int)S.mgr->block_test(bit-1), 1,
                                            (int)S.mgr->block_test(bit+1), (int)S.mgr->block_test(bit+2),
                                            (int)img_bit(sb.data(), bit-1), (int)sh,
                                            (int)img_bit(sb.data(), bit+1), (int)img_bit(sb.data(), bit+2));
                                bad = true;
                                return;
                            }
                            if (u != sh) {   /* sh==true && u==0：丢失置位（双分配方向） */
                                std::lock_guard<std::mutex> lk(tk_mu);
                                g_fail++;
                                std::printf("  [BUG] 分区 test 丢置位 geom=%s seed=%llx round=%d "
                                            "thr=%d op=%d bit=%llu mgr=0 shadow=1\n", g.name,
                                            (unsigned long long)seed, round, t, i,
                                            (unsigned long long)bit);
                                bad = true;
                                return;
                            }
                        }
                    }
                });
            }
            for (auto &x : th) x.join();
            for (int64_t d : dz) zb = (uint64_t)((int64_t)zb + d);
            if (bad) return;
            const uint64_t ft = S.free_total(false);
            if (ft != zb) {
                ticket_th("屏障计数失配(阶段F) geom=%s seed=%llx round=%d mode=%s "
                          "free_total=%llu 影子=%llu", g.name, (unsigned long long)seed,
                          round, S.chunked ? "CHUNK" : "FULL", (unsigned long long)ft,
                          (unsigned long long)zb);
                return;
            }
            if (S.chunked) S.finish();                   /* 屏障期 TTL（§9 与分配并行已由 churn 覆盖） */
        }

        /* ── 阶段 A：全同参 alloc_soft(1) 并发（F-only 配置跳过） ── */
        if (!with_phaseA) continue;
        {
            std::vector<std::thread> th;
            for (int t = 0; t < nthreads; t++) {
                th.emplace_back([&, t] {
                    t_fake_proc_id = (uint32_t)(t % S.mgr->cpu_slots());
                    for (int i = 0; i < nA && !bad; i++) {
                        uint64_t left = 1;
                        uint64_t v = S.mgr->block_alloc_soft(1, &left);
                        for (int retry = 0; v == PTR_INVALID && retry < 4000; retry++) {
                            /* god try 纪律下全片锁瞬时全忙 → 伪 ENOSPC；影子仍有空闲则
                             * 重试，保证两模式成功次数一致 ⇒ 跨模式终态可比 */
                            bool has_free;
                            { std::lock_guard<std::mutex> lk(M); has_free = zb > 0; }
                            if (!has_free) break;
                            std::this_thread::sleep_for(std::chrono::microseconds(50));
                            left = 1;
                            v = S.mgr->block_alloc_soft(1, &left);
                        }
                        if (v == PTR_INVALID) {
                            bool has_free;
                            { std::lock_guard<std::mutex> lk(M); has_free = zb > 0; }
                            if (has_free) {
                                ticket_th("并发 alloc 反复失败但影子仍有空闲（伪 ENOSPC）"
                                          "geom=%s seed=%llx round=%d", g.name,
                                          (unsigned long long)seed, round);
                                return;
                            }
                            continue;   /* 真满：两模式同样终止（可交换性保持） */
                        }
                        if (left != 0) {
                            ticket_th("alloc_soft(1) 返回残缺 geom=%s seed=%llx round=%d "
                                      "base=%llu left=%llu", g.name, (unsigned long long)seed,
                                      round, (unsigned long long)v, (unsigned long long)left);
                            return;
                        }
                        std::lock_guard<std::mutex> lk(M);
                        if (img_bit(sb.data(), v)) {
                            ticket_th("★ 并发 alloc 双分配 geom=%s seed=%llx round=%d base=%llu "
                                      "（影子已置位）", g.name, (unsigned long long)seed, round,
                                      (unsigned long long)v);
                            return;
                        }
                        img_set(sb.data(), v);
                        zb--;
                    }
                });
            }
            for (auto &x : th) x.join();
            if (bad) return;
            const uint64_t ft = S.free_total(false);
            if (ft != zb) {
                ticket_th("屏障计数失配(阶段A) geom=%s seed=%llx round=%d mode=%s "
                          "free_total=%llu 影子=%llu", g.name, (unsigned long long)seed, round,
                          S.chunked ? "CHUNK" : "FULL", (unsigned long long)ft,
                          (unsigned long long)zb);
                return;
            }
            if (S.chunked) S.finish();
        }
    }
}

static void conc_run(const geom_t &g, uint64_t seed, kbstats_t &kb, bool with_phaseA) {
    std::vector<uint8_t> init_b, init_i;
    gen_init(g, seed, init_b, init_i);
    side_t full, chunk;
    if (!full.mount(g, init_b, init_i, false) || !chunk.mount(g, init_b, init_i, true)) {
        TICKET("并发对拍装配失败 geom=%s seed=%llx", g.name, (unsigned long long)seed);
        return;
    }
    std::vector<uint8_t> sb_f = init_b, sb_c = init_b;
    uint64_t zb_f = img_zeros(sb_f.data(), sb_f.size());
    uint64_t zb_c = img_zeros(sb_c.data(), sb_c.size());
    std::atomic<bool> bad{false};
    std::mutex tk_mu;

    conc_pass(full, g, seed, sb_f, zb_f, bad, tk_mu, kb, with_phaseA);
    const bool full_bad = bad;
    bad = false;
    conc_pass(chunk, g, seed, sb_c, zb_c, bad, tk_mu, kb, with_phaseA);
    const bool chunk_bad = bad;

    /* ── 每模式终态自证（T11 式）：镜像 == 自己的影子 + 计数 == 影子 ── */
    auto perpass = [&](side_t &S, std::vector<uint8_t> &sb, uint64_t zb,
                       const char *who) -> bool {
        S.finish();
        if (S.free_total(false) != zb) {
            TICKET("并发终态计数失配 geom=%s seed=%llx mode=%s free_total=%llu 影子=%llu",
                   g.name, (unsigned long long)seed, who,
                   (unsigned long long)S.free_total(false), (unsigned long long)zb);
            return false;
        }
        if (std::memcmp(S.img(false), sb.data(), sb.size()) != 0) {
            for (uint64_t i = 0; i < sb.size() * 8; i++)
                if (img_bit(S.img(false), i) != img_bit(sb.data(), i)) {
                    TICKET("★ 并发终态镜像≠影子 geom=%s seed=%llx mode=%s 首分歧 bit=%llu",
                           g.name, (unsigned long long)seed, who, (unsigned long long)i);
                    break;
                }
            return false;
        }
        return true;
    };
    const bool pf = perpass(full, sb_f, zb_f, "FULL");
    const bool pc = perpass(chunk, sb_c, zb_c, "CHUNK");
    full.audit("conc/FULL");
    chunk.audit("conc/CHUNK");

    /* ── 跨模式终态对照 ──
     * F-only（free/test 并发）：结果与线程交错无关（分区确定性）⇒ 强制全等。
     * 含阶段 A（alloc 并发）：god try 换片次序使**落点本身非确定**（FULL↔FULL
     * 复跑亦可不等，多片几何必现）⇒ 跨模式逐位全等判据不适用；降级为
     * 「各自影子全等」（上面 perpass，强制）+ 落点差异仅记 INFO。 */
    const char *cfg = with_phaseA ? "F+A" : "F-only";
    if (pf && pc && !full_bad && !chunk_bad) {
        const bool zb_eq = zb_f == zb_c;
        const bool img_eq = std::memcmp(full.img(false), chunk.img(false),
                                        full.bytes(false)) == 0;
        const bool sh_eq = std::memcmp(sb_f.data(), sb_c.data(), sb_f.size()) == 0;
        if (!with_phaseA) {
            if (zb_eq && sh_eq && img_eq) {
                std::printf("    [conc] %-26s seed=%016llx cfg=%-7s 3 轮完成："
                            "跨模式终态镜像+计数 memcmp 全等 OK\n", g.name,
                            (unsigned long long)seed, cfg);
            } else {
                for (uint64_t i = 0; i < full.bytes(false); i++)
                    if (img_bit(full.img(false), i) != img_bit(chunk.img(false), i)) {
                        TICKET("★ 并发(F-only) 跨模式终态镜像不等 geom=%s 首分歧 bit=%llu "
                               "FULL=%d CHUNK=%d（分区操作本应与交错无关）", g.name,
                               (unsigned long long)i, (int)img_bit(full.img(false), i),
                               (int)img_bit(chunk.img(false), i));
                        break;
                    }
                if (!zb_eq)
                    TICKET("并发(F-only) 跨模式终态计数不等 geom=%s FULL侧=%llu CHUNK侧=%llu",
                           g.name, (unsigned long long)zb_f, (unsigned long long)zb_c);
                std::printf("    [conc] %-26s seed=%016llx cfg=%-7s FAIL（见上）\n",
                            g.name, (unsigned long long)seed, cfg);
            }
        } else if (zb_eq && sh_eq && img_eq) {
            std::printf("    [conc] %-26s seed=%016llx cfg=%-7s 3 轮完成：各自影子全等 + "
                        "跨模式终态亦全等 OK\n", g.name, (unsigned long long)seed, cfg);
        } else {
            std::printf("    [note] %-26s cfg=F+A 跨模式落点差异（FULL侧=%llu CHUNK侧=%llu，"
                        "首影子分歧 bit 存在）：god try 换片次序非确定（多片几何必现），"
                        "跨模式逐位判据降级为「各自影子全等」= OK\n",
                        g.name, (unsigned long long)zb_f, (unsigned long long)zb_c);
            for (uint64_t i = 0; i < sb_f.size() * 8; i++)
                if (img_bit(sb_f.data(), i) != img_bit(sb_c.data(), i)) {
                    std::printf("    [note] 首落点分歧 bit=%llu（FULL侧=%d CHUNK侧=%d）\n",
                                (unsigned long long)i, (int)img_bit(sb_f.data(), i),
                                (int)img_bit(sb_c.data(), i));
                    break;
                }
            std::printf("    [conc] %-26s seed=%016llx cfg=%-7s 各自影子全等 OK（跨模式落点"
                        "差异记 INFO）\n", g.name, (unsigned long long)seed, cfg);
        }
    } else {
        std::printf("    [conc] %-26s seed=%016llx cfg=%-7s 中断（见上方 [BUG]）\n",
                    g.name, (unsigned long long)seed, cfg);
    }
    g_kb1_total += kb.kb1;
    g_kb2_total += kb.kb2;
}

int main() {
    std::printf("== FULL ↔ CHUNK 双模式对拍（Phase A-1，03 §10 oracle）==\n");

    SECTION("S 组：顺序对拍（alloc_hard/alloc_soft/occupy/free/test + inode 全集）");
    for (const geom_t &g : GEOMS) {
        for (uint64_t seed : SEEDS) {
            kbstats_t kb;
            seq_run(g, seed, 3000, kb);
        }
    }

    SECTION("C1：并发对拍 F-only（分区 free/test 并发，跨模式终态强制 memcmp）");
    for (const geom_t &g : GEOMS) {
        kbstats_t kb;
        conc_run(g, SEEDS[0], kb, /*with_phaseA=*/false);
    }
    SECTION("C2：并发对拍 F+A（+ alloc 并发；各自影子强制全等，落点差异记 INFO）");
    for (const geom_t &g : GEOMS) {
        kbstats_t kb;
        conc_run(g, SEEDS[0], kb, /*with_phaseA=*/true);
    }

    std::printf("\n累计：KB1(occupy 虚报)=%d；KB2(离线 free 计数虚高)=%d；"
                "BUG#4(test 竞态保守 true)=%d 次\n", g_kb1_total, g_kb2_total, g_bug4_total);
    std::printf("%d passed, %d failed\n", g_pass.load(), g_fail.load());
    if (g_fail.load() == 0) std::printf("RESULT: ALL PASS\n");
    else std::printf("RESULT: FAIL（含已知 bug 复现单，见 [BUG] 行）\n");
    return g_fail.load() ? 1 : 0;
}

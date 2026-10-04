/* ═══════════════════════════════════════════════════════════════════
 * stress_fs_allocator_slice.cpp — mgr_slice_t 单线程随机 + 对抗压力
 *
 * 2026-10-04 新建（RavenFS 测试工件；**不改任何生产代码**）。
 * 契约权威：include/ravenfs/fs_allocator.hpp（逐字为规格）。
 * 本轮覆盖片自治单元 7 接口：
 *   alloc_hard / soft_alloc / soft_occupy / free_interval / free_intervals /
 *   test_interval / all_dump（+ get_free_count 对账 / shutdown 对位）。
 *
 * 方法：**影子账本**。构造前先在 4K 对齐的原始位图里挖好空闲位（enable_slice
 * 一次性消费），影子账本逐位自重演「哪些位被占」。每 N 步全量对账：
 *   - 主树区间严格不重叠（且因融合纪律，相邻区间必须留缝）；
 *   - get_free_count() == 影子空闲位数；
 *   - all_dump 区间并集 == 影子空闲集合（逐区间精确比对）；
 *   - 随机 test_interval 与影子一致（全空闲/全占用/混合）；
 *   - alloc 出的区间 test 为占用、2^align_log2 对齐满足；
 *   - 归还后 test 为空闲；
 *   - soft_occupy 的 rest_bits 记账自洽。
 *
 * 对抗/边界：free 重叠必须报错、bitscount=0、越界 base/len、片尾、len==0、
 *   全部归还后 free_count 复原初值。**远上界越界在独立探测片里验证并报告**
 *   （该路径会在现有原型上触发一个真实契约违反，见 prototype_probe）。
 *
 * 编译/运行（由 CMake 目标 ravenfs_stress_fs_allocator_slice 提供）：
 *   ./build_fresh/tests/ravenfs_stress_fs_allocator_slice
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/fs_allocator.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <list>
#include <forward_list>
#include <vector>
#include <random>

static long g_pass = 0, g_fail = 0, g_bug = 0, g_audits = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; if (g_fail <= 40) printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

static const uint64_t OFF = 65536;   /* 片全局起点（2 的幂，便于全档对齐） */
static const uint64_t N   = 4096;    /* 片位数 */

/* ── 影子账本（自重演哪些位被占；索引 i 对应全局 OFF+i）── */
struct Shadow {
    uint64_t off, n;
    std::vector<uint8_t> occ;    /* 1 = 占用，0 = 空闲 */
    Shadow(uint64_t o, uint64_t nn) : off(o), n(nn), occ(nn, 0) {}
    bool all_free(uint64_t b, uint64_t l) const {
        for (uint64_t i = 0; i < l; i++) if (occ[b - off + i]) return false;
        return true;
    }
    bool all_occ(uint64_t b, uint64_t l) const {
        for (uint64_t i = 0; i < l; i++) if (!occ[b - off + i]) return false;
        return true;
    }
    bool any_free(uint64_t b, uint64_t l) const {
        for (uint64_t i = 0; i < l; i++) if (!occ[b - off + i]) return true;
        return false;
    }
    void mark(uint64_t b, uint64_t l) { for (uint64_t i = 0; i < l; i++) occ[b - off + i] = 1; }
    void clear(uint64_t b, uint64_t l) { for (uint64_t i = 0; i < l; i++) occ[b - off + i] = 0; }
    uint64_t free_count() const { uint64_t c = 0; for (uint8_t x : occ) c += x ? 0 : 1; return c; }
    /* base 起连续空闲前缀长（不越片尾） */
    uint64_t prefix_free(uint64_t b) const {
        uint64_t k = 0; while (b + k < off + n && !occ[b - off + k]) k++; return k;
    }
    /* 从 from（局部下标）起找第一个空闲位，全局号写入 out */
    bool find_free(uint64_t from, uint64_t &out) const {
        for (uint64_t i = from; i < n; i++) if (!occ[i]) { out = off + i; return true; }
        for (uint64_t i = 0; i < from; i++) if (!occ[i]) { out = off + i; return true; }
        return false;
    }
};

/* 取 all_dump 结果 */
static std::vector<fs_run_t> dump_vec(mgr_slice_t &s) {
    std::list<fs_run_t> dl;
    result_code_t rc = s.all_dump(&dl);
    CHECK(rc == RC_OK, "all_dump rc==RC_OK");
    return std::vector<fs_run_t>(dl.begin(), dl.end());
}

/* 影子空闲集合 = 极大空闲区间序列（应与 all_dump 逐项相等） */
static std::vector<fs_run_t> shadow_free_runs(const Shadow &sh) {
    std::vector<fs_run_t> v;
    uint64_t i = 0;
    while (i < sh.n) {
        if (sh.occ[i]) { i++; continue; }
        uint64_t j = i; while (j < sh.n && !sh.occ[j]) j++;
        v.push_back(fs_run_t{sh.off + i, j - i});
        i = j;
    }
    return v;
}

/* ── 全量对账 ── */
static void audit(mgr_slice_t &s, Shadow &sh, std::mt19937_64 &rng, const char *tag) {
    (void)tag;
    g_audits++;
    std::vector<fs_run_t> v = dump_vec(s);
    uint64_t sum = 0;
    for (size_t i = 0; i < v.size(); i++) {
        CHECK(v[i].len > 0, "audit: run len>0");
        CHECK(v[i].base >= OFF && v[i].base + v[i].len <= OFF + N, "audit: run within slice");
        sum += v[i].len;
        if (i > 0)
            CHECK(v[i - 1].base + v[i - 1].len < v[i].base,
                  "audit: intervals strictly ascending & non-adjacent");
    }
    CHECK(sum == sh.free_count(), "audit: sum(runs)==shadow free count");
    CHECK(s.get_free_count() == sh.free_count(), "audit: get_free_count==shadow free count");

    std::vector<fs_run_t> want = shadow_free_runs(sh);
    CHECK(v.size() == want.size(), "audit: run count==shadow count");
    if (v.size() == want.size()) {
        for (size_t i = 0; i < v.size(); i++)
            CHECK(v[i].base == want[i].base && v[i].len == want[i].len,
                  "audit: dump==shadow interval-by-interval");
    }

    /* 随机 test_interval 与影子比对（区间内） */
    for (int k = 0; k < 8; k++) {
        uint64_t b = OFF + rng() % N;
        uint64_t l = rng() % (N - (b - OFF) + 1);
        bool val = (rng() & 1) != 0;
        result_code_t got = s.test_interval(val, b, l);
        result_code_t exp;
        if (l == 0)                       exp = RC_OK;
        else if (val)                     exp = sh.all_occ(b, l) ? RC_OK : RC_ERR_NOT_FOUND;
        else                              exp = sh.all_free(b, l) ? RC_OK : RC_ERR_NOT_FOUND;
        CHECK(got == exp, "audit: test_interval matches shadow");
    }
}

/* ── 安全边界（非破坏性；均落在合法/立即拒绝的范围）── */
static void boundary_checks(mgr_slice_t &s) {
    uint64_t out = 0, rest = 0;
    fs_run_list_t fl;
    std::list<fs_run_t> dl;
    bool sat = false;

    CHECK(s.alloc_hard(0, 0, out) == RC_ERR_STATE, "boundary: alloc_hard bitscount==0");
    CHECK(s.alloc_hard(64, 1, out) == RC_ERR_STATE, "boundary: alloc_hard align_log2>63");
    CHECK(s.soft_alloc(0, 0, &fl, sat) == RC_ERR_STATE, "boundary: soft_alloc bitscount==0");
    CHECK(s.soft_alloc(64, 1, &fl, sat) == RC_ERR_STATE, "boundary: soft_alloc align_log2>63");
    CHECK(s.soft_alloc(0, 1, nullptr, sat) == RC_ERR_STATE, "boundary: soft_alloc nullptr");

    CHECK(s.test_interval(false, OFF - 1, 1) == RC_ERR_STATE, "boundary: test below slice");
    CHECK(s.test_interval(false, OFF + N, 1) == RC_ERR_STATE, "boundary: test at slice end");
    CHECK(s.free_interval(OFF - 1, 1) == RC_ERR_STATE, "boundary: free below slice");
    CHECK(s.free_interval(OFF + N, 1) == RC_ERR_STATE, "boundary: free at slice end");
    CHECK(s.free_interval(OFF, 0) == RC_OK, "boundary: free len==0 no-op");
    CHECK(s.test_interval(false, OFF, 0) == RC_OK, "boundary: test len==0 no-op");

    CHECK(s.soft_occupy(OFF, 0, &rest) == RC_OK, "boundary: occupy len==0 no-op");
    CHECK(s.soft_occupy(OFF - 1, 1, &rest) == RC_ERR_STATE, "boundary: occupy below slice");
    CHECK(s.soft_occupy(OFF + N + 1, 1, &rest) == RC_ERR_STATE, "boundary: occupy above slice");
    rest = 5;
    CHECK(s.soft_occupy(OFF, 1, &rest) == RC_ERR_STATE, "boundary: occupy rest>len");
    CHECK(s.soft_occupy(OFF, 1, nullptr) == RC_ERR_STATE, "boundary: occupy nullptr rest");

    CHECK(s.all_dump(nullptr) == RC_ERR_STATE, "boundary: all_dump nullptr");
    CHECK(s.free_intervals(nullptr) == RC_ERR_STATE, "boundary: free_intervals nullptr");
    (void)dl; (void)out; (void)fl;
}

/* ── 远上界越界探测（独立片）：报告契约违反，不污染主片 ── */
static void prototype_probe() {
    const uint64_t POFF = 200000, PN = 64;
    uint8_t *pm = nullptr;
    if (posix_memalign((void **)&pm, 4096, (PN + 7) / 8) != 0) { printf("  probe memalign fail\n"); g_fail++; return; }
    memset(pm, 0xFF, (PN + 7) / 8);
    for (uint64_t i = 0; i < 8; i++) pm[i >> 3] &= (uint8_t)~(1u << (i & 7));  /* 前 8 位空闲 */
    mgr_slice_t p;
    slice_config_t c{}; c.slice_base = pm; c.in_global_base_idx = POFF; c.slice_bitscount = PN;
    if (p.enable_slice(c) != RC_OK) { printf("  probe enable fail\n"); g_fail++; free(pm); return; }
    const uint64_t end = POFF + PN;

    struct Case { const char *what; result_code_t got; result_code_t want; };
    /* 非破坏性两例 */
    Case cs[2] = {
        { "test_interval(true,  slice_end+1, 1)", p.test_interval(true,  end + 1, 1), RC_ERR_STATE },
        { "test_interval(false, slice_end+1, 1)", p.test_interval(false, end + 1, 1), RC_ERR_STATE },
    };
    for (int i = 0; i < 2; i++) {
        bool ok = (cs[i].got == cs[i].want);
        printf("  [PROBE] %s = %d (contract RC_ERR_STATE=%d) %s\n",
               cs[i].what, (int)cs[i].got, (int)cs[i].want,
               ok ? "" : "<-- PROTOTYPE BUG (far-above base not rejected)");
        if (ok) g_pass++; else g_bug++;
    }
    /* 破坏性一例：契约要求 RC_ERR_STATE，实际会插入片外区间 */
    result_code_t r3 = p.free_interval(end + 1, 1);
    bool ok3 = (r3 == RC_ERR_STATE);
    printf("  [PROBE] free_interval(slice_end+1, 1)      = %d (contract RC_ERR_STATE=%d) %s\n",
           (int)r3, (int)RC_ERR_STATE,
           ok3 ? "" : "<-- PROTOTYPE BUG (inserts out-of-slice interval)");
    if (ok3) g_pass++; else g_bug++;
    std::list<fs_run_t> dl; p.all_dump(&dl);
    printf("  [PROBE] all_dump after far-above free: %zu run(s):", dl.size());
    for (const fs_run_t &r : dl)
        printf(" [%llu,%llu)", (unsigned long long)r.base, (unsigned long long)(r.base + r.len));
    printf("\n");
    free(pm);
}

int main() {
    puts("==== stress_fs_allocator_slice : mgr_slice_t 随机+对抗压力 ====");
    std::mt19937_64 rng(0xC0FFEEULL);

    {   /* 门闸：未使能对象 */
        mgr_slice_t g; uint64_t out = 0, rest = 0; bool sat = false;
        fs_run_list_t fl; std::list<fs_run_t> dl;
        CHECK(g.test_interval(false, OFF, 1) == RC_ERR_STATE, "gate: test before enable");
        CHECK(g.free_interval(OFF, 1) == RC_ERR_STATE, "gate: free before enable");
        CHECK(g.alloc_hard(0, 1, out) == RC_ERR_STATE, "gate: alloc_hard before enable");
        CHECK(g.soft_alloc(0, 1, &fl, sat) == RC_ERR_STATE, "gate: soft_alloc before enable");
        CHECK(g.soft_occupy(OFF, 1, &rest) == RC_ERR_STATE, "gate: occ before enable");
        CHECK(g.all_dump(&dl) == RC_ERR_STATE, "gate: dump before enable");
        CHECK(g.free_intervals(&fl) == RC_ERR_STATE, "gate: free_intervals before enable");
        CHECK(g.shutdown() == RC_ERR_STATE, "gate: shutdown on fresh rejected");
    }

    uint8_t *mem = nullptr;
    if (posix_memalign((void **)&mem, 4096, (N + 7) / 8) != 0) { printf("FAIL memalign\n"); return 2; }
    memset(mem, 0, (N + 7) / 8);          /* 先全 0 = 全空闲 */
    Shadow sh(OFF, N);
    for (uint64_t i = 0; i < N; i++)
        if (rng() % 100 < 30) { sh.occ[i] = 1; mem[i >> 3] |= (uint8_t)(1u << (i & 7)); }

    mgr_slice_t s;
    slice_config_t cfg{}; cfg.slice_base = mem; cfg.in_global_base_idx = OFF; cfg.slice_bitscount = N;
    CHECK(s.enable_slice(cfg) == RC_OK, "enable ok");
    CHECK(s.enable_slice(cfg) == RC_ERR_STATE, "enable: repeat rejected");

    const Shadow init = sh;                       /* 初始占用快照 */
    const uint64_t init_free = sh.free_count();
    CHECK(s.get_free_count() == init_free, "init: get_free_count==initial free");

    boundary_checks(s);
    audit(s, sh, rng, "init");

    std::vector<fs_run_t> live;                   /* 我方持有的占用 run（影子同步） */
    uint64_t live_bits = 0;
    const long OPS = 300000;
    long n_hard = 0, n_soft = 0, n_occ = 0, n_free = 0, n_batch = 0, n_adv = 0, n_test = 0;

    for (long op = 0; op < OPS; op++) {
        uint64_t r = rng() % 100;
        if (live.empty())                 r = 0;      /* 无货 → 强制分配 */
        else if (live_bits > N * 3 / 5)   r = 70;     /* 积压过多 → 强制归还 */

        if (r < 30) {                                  /* ── alloc_hard ── */
            n_hard++;
            uint8_t al = (uint8_t)(rng() % 5);
            uint64_t cnt = 1 + rng() % 48;
            uint64_t out = 0;
            result_code_t rc = s.alloc_hard(al, cnt, out);
            if (rc == RC_OK) {
                const uint64_t mask = al ? ((1ULL << al) - 1) : 0ULL;
                CHECK(out >= OFF && out + cnt <= OFF + N, "hard: in range");
                CHECK((out & mask) == 0, "hard: 2^align_log2 aligned");
                CHECK(sh.all_free(out, cnt), "hard: delivered bits were free");
                CHECK(s.test_interval(true, out, cnt) == RC_OK, "hard: delivered run test occupied");
                sh.mark(out, cnt); live.push_back(fs_run_t{out, cnt}); live_bits += cnt;
            } else if (rc == RC_ERR_NO_BLOCK) {
                /* 失败 ⇒ 影子中确无「能容纳对齐段」的空闲区（否则是原型漏分配） */
                uint64_t mask = al ? ((1ULL << al) - 1) : 0ULL;
                bool feasible = false;
                std::vector<fs_run_t> runs = shadow_free_runs(sh);
                for (const fs_run_t &q : runs) {
                    if (q.len < cnt) continue;
                    uint64_t st = (q.base + mask) & ~mask;
                    if (st + cnt <= q.base + q.len) { feasible = true; break; }
                }
                CHECK(!feasible, "hard: NO_BLOCK but room exists (missed alloc?)");
            } else {
                CHECK(false, "hard: unexpected rc");
            }
        } else if (r < 56) {                           /* ── soft_alloc（align 0）── */
            n_soft++;
            uint64_t req = 1 + rng() % 96;
            uint64_t free_before = sh.free_count();
            fs_run_list_t runs; bool sat = false;
            result_code_t rc = s.soft_alloc(0, req, &runs, sat);
            uint64_t sum = 0; bool allfree = true, inrange = true;
            for (const fs_run_t &q : runs) {
                sum += q.len;
                if (!sh.all_free(q.base, q.len)) allfree = false;
                if (q.base < OFF || q.base + q.len > OFF + N) inrange = false;
            }
            if (rc == RC_OK) {
                CHECK(inrange, "soft: runs in range");
                CHECK(allfree, "soft: delivered bits were free");
                uint64_t exp_sum = free_before < req ? free_before : req;
                CHECK(sum == exp_sum, "soft: delivered == min(req, free_before)");
                CHECK(sat == (free_before >= req), "soft: all_satisfied consistency");
                for (const fs_run_t &q : runs) {
                    CHECK(s.test_interval(true, q.base, q.len) == RC_OK, "soft: run test occupied");
                    sh.mark(q.base, q.len); live.push_back(q); live_bits += q.len;
                }
            } else if (rc == RC_ERR_NO_BLOCK) {
                CHECK(free_before == 0, "soft: NO_BLOCK but free bits exist");
                CHECK(runs.empty(), "soft: NO_BLOCK gives empty runs");
                CHECK(!sat, "soft: NO_BLOCK => not satisfied");
            } else {
                CHECK(false, "soft: unexpected rc");
            }
            runs.clear();
        } else if (r < 70) {                            /* ── soft_occupy ── */
            n_occ++;
            uint64_t base = OFF + rng() % N;
            uint64_t len  = 1 + rng() % 24;
            uint64_t rest = rng() % (len + 1);          /* 已满足量 0..len */
            uint64_t rest_in = rest;
            result_code_t rc = s.soft_occupy(base, len, &rest);
            uint64_t real = len - rest_in;
            uint64_t pref = sh.prefix_free(base);
            if (real == 0) {
                CHECK(rc == RC_OK, "occ: real==0 => OK no-op");
                CHECK(rest == rest_in, "occ: real==0 rest unchanged");
            } else if (pref == 0) {
                CHECK(rc == RC_ERR_NO_BLOCK, "occ: anchor occupied => NO_BLOCK");
                CHECK(rest == len, "occ: NO_BLOCK rest==len");
            } else {
                uint64_t claimed = pref < real ? pref : real;
                CHECK(rc == RC_OK, "occ: rc OK");
                CHECK(sh.all_free(base, claimed), "occ: claimed prefix was free");
                CHECK(rest == len - claimed, "occ: rest accounting (len-claimed)");
                CHECK(s.test_interval(true, base, claimed) == RC_OK, "occ: claimed bits occupied");
                sh.mark(base, claimed); live.push_back(fs_run_t{base, claimed}); live_bits += claimed;
            }
        } else if (r < 85) {                            /* ── free_interval（单段）── */
            n_free++;
            fs_run_t x = live.back(); live.pop_back();
            CHECK(sh.all_occ(x.base, x.len), "free: record was occupied");
            sh.clear(x.base, x.len); live_bits -= x.len;
            CHECK(s.free_interval(x.base, x.len) == RC_OK, "free: rc OK");
            CHECK(s.test_interval(false, x.base, x.len) == RC_OK, "free: freed run test free");
            CHECK(s.free_interval(x.base, x.len) == RC_ERR_OVERLAP, "free: double-free rejected");
        } else if (r < 91) {                            /* ── free_intervals（链式）── */
            n_batch++;
            fs_run_list_t fl;
            uint64_t k = 1 + rng() % 4;
            for (uint64_t j = 0; j < k && !live.empty(); j++) {
                fs_run_t x = live.back(); live.pop_back();
                CHECK(sh.all_occ(x.base, x.len), "batch: record was occupied");
                sh.clear(x.base, x.len); live_bits -= x.len;
                fl.push_front(x);
            }
            CHECK(s.free_intervals(&fl) == RC_OK, "batch: free_intervals rc OK");
            CHECK(fl.empty(), "batch: chain drained");
        } else if (r < 97) {                            /* ── 对抗：重叠释放必报错 ── */
            n_adv++;
            /* 选一个影子空闲位 g，构造覆盖它的区间 → 必然重叠 */
            uint64_t g = 0;
            if (sh.find_free(rng() % N, g)) {
                uint64_t lo = g, hi = g + 1;
                if (g > OFF && (rng() & 1)) lo = g - 1;
                if (g + 1 < OFF + N && (rng() & 1)) hi = g + 2;
                CHECK(sh.any_free(lo, hi - lo), "adv: interval contains a free bit (guard)");
                CHECK(s.free_interval(lo, hi - lo) == RC_ERR_OVERLAP, "adv: overlap rejected");
                result_code_t texp = sh.all_free(lo, hi - lo) ? RC_OK : RC_ERR_NOT_FOUND;
                CHECK(s.test_interval(false, lo, hi - lo) == texp, "adv: interval test matches shadow");
            }
            /* 越界下界（非破坏性）每次顺带验一次 */
            CHECK(s.free_interval(OFF - 1, 1) == RC_ERR_STATE, "adv: below slice rejected");
            CHECK(s.free_interval(OFF + N, 1) == RC_ERR_STATE, "adv: at slice end rejected");
        } else {                                        /* ── test_interval 随机 ── */
            n_test++;
            uint64_t b = OFF + rng() % N;
            uint64_t l = rng() % (N - (b - OFF) + 1);
            bool val = (rng() & 1) != 0;
            result_code_t got = s.test_interval(val, b, l);
            result_code_t exp;
            if (l == 0)   exp = RC_OK;
            else if (val) exp = sh.all_occ(b, l) ? RC_OK : RC_ERR_NOT_FOUND;
            else          exp = sh.all_free(b, l) ? RC_OK : RC_ERR_NOT_FOUND;
            CHECK(got == exp, "rand: test_interval matches shadow");
        }

        if (((op + 1) % 1000) == 0) audit(s, sh, rng, "step");
    }

    /* ── 全部归还 → 复原初值 ── */
    while (!live.empty()) {
        fs_run_t x = live.back(); live.pop_back();
        CHECK(sh.all_occ(x.base, x.len), "drain: record was occupied");
        sh.clear(x.base, x.len); live_bits -= x.len;
        CHECK(s.free_interval(x.base, x.len) == RC_OK, "drain: rc OK");
    }
    CHECK(live_bits == 0, "drain: live_bits==0");
    audit(s, sh, rng, "final");
    CHECK(s.get_free_count() == init_free, "final: free_count restored to initial");

    bool same = true;
    for (uint64_t i = 0; i < N; i++) if (sh.occ[i] != init.occ[i]) same = false;
    CHECK(same, "final: shadow identical to initial occupancy");
    {
        std::vector<fs_run_t> v = dump_vec(s);
        std::vector<fs_run_t> want = shadow_free_runs(init);
        bool eq = (v.size() == want.size());
        if (eq) for (size_t i = 0; i < v.size(); i++) if (v[i].base != want[i].base || v[i].len != want[i].len) eq = false;
        CHECK(eq, "final: dump identical to initial free intervals");
    }
    CHECK(s.shutdown() == RC_OK, "shutdown: true->false");
    CHECK(s.test_interval(false, OFF, 1) == RC_ERR_STATE, "after shutdown: gated");

    prototype_probe();

    free(mem);
    printf("\n==== stress_fs_allocator_slice: PASS %ld, FAIL %ld, PROTOTYPE-BUG %ld "
           "(ops=%ld hard=%ld soft=%ld occ=%ld free=%ld batch=%ld adv=%ld test=%ld, audits=%ld) ====\n",
           g_pass, g_fail, g_bug, OPS, n_hard, n_soft, n_occ, n_free, n_batch, n_adv, n_test, g_audits);
    return (g_fail == 0 && g_bug == 0) ? 0 : 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * test_fs_allocator.cpp — mgr_slice_t 主从红黑树分片分配器单测
 *
 * 2026-10-03 新建。覆盖本轮 6 接口：
 *   enable_slice（手工位图 → 极大空闲区间；门闸/校验/重复使能）
 *   test_interval（全空闲/全占用两态 + 越界 + len==0）
 *   free_interval（纯插 / 单侧融合 / 双侧融合 / 重叠报错）
 *   free_intervals（drain；nullptr；中途出错余段留链）
 *   all_dump（base 递增序；空片空链）
 *   soft_occupy（前缀认领 / 缩劈删 / 锚点占用 NO_BLOCK / rest 链式记账 / hint 只进不退）
 *
 * 靶场构造法：手工位图（先全 1=占用，再按需要清 0=空闲），全局起点选非 0
 * 偏移以验证「坐标一律全局」。
 * free_count_ 通过 all_dump 求和间接验证（get_free_count 本轮未实现，不链接）。
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/fs_allocator.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <list>
#include <forward_list>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

/* ── 靶场：4K 对齐位图内存 ── */
struct Bits {
    uint8_t *mem = nullptr;
    uint64_t bytes = 0;
    void alloc(uint64_t nbits) {
        bytes = (nbits + 7) / 8;
        if (posix_memalign((void **)&mem, 4096, bytes) != 0) { printf("  FAIL memalign\n"); exit(1); }
        memset(mem, 0xFF, bytes);              /* 全 1 = 全部占用 */
    }
    void clear_mem() { if (mem) { free(mem); mem = nullptr; } }
    /* 位置 i（片内偏移）置空闲（清 0） */
    void set_free(uint64_t i, uint64_t n) {
        for (uint64_t k = 0; k < n; k++) mem[(i + k) >> 3] &= (uint8_t)~(1u << ((i + k) & 7));
    }
};

/* 取 all_dump 结果并按顺序转成 vector，同时求和（= free_count 间接验证） */
static std::vector<fs_run_t> dump_vec(mgr_slice_t &s, uint64_t *sum = nullptr) {
    std::list<fs_run_t> dl;
    result_code_t rc = s.all_dump(&dl);
    if (rc != RC_OK) { printf("  FAIL all_dump rc=%llu\n", (unsigned long long)rc); g_fail++; }
    std::vector<fs_run_t> v(dl.begin(), dl.end());
    if (sum) { uint64_t t = 0; for (auto &r : v) t += r.len; *sum = t; }
    return v;
}

static bool eq_intervals(const std::vector<fs_run_t> &v,
                         const std::vector<std::pair<uint64_t, uint64_t>> &want) {
    if (v.size() != want.size()) return false;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].base != want[i].first || v[i].len != want[i].second) return false;
    return true;
}

int main() {
    const uint64_t OFF = 500;      /* 本片全局起点：非 0，验证全局坐标 */
    const uint64_t NBITS = 64;

    /* ═══════ 1. enable_slice：手工位图 → 极大空闲区间 ═══════ */
    puts("[1] enable_slice 建树");
    mgr_slice_t s1;
    /* 未使能门闸：其它接口一律 RC_ERR_STATE */
    CHECK(s1.test_interval(false, OFF, 1) == RC_ERR_STATE, "gate: test before enable");
    CHECK(s1.free_interval(OFF, 1) == RC_ERR_STATE, "gate: free before enable");
    {
        std::list<fs_run_t> dl; CHECK(s1.all_dump(&dl) == RC_ERR_STATE, "gate: dump before enable");
        fs_run_list_t fl; CHECK(s1.free_intervals(&fl) == RC_ERR_STATE, "gate: free_intervals before enable");
        uint64_t r = 0; CHECK(s1.soft_occupy(OFF, 1, &r) == RC_ERR_STATE, "gate: occ before enable");
    }
    /* 校验失败路径 */
    {
        slice_config_t bad{}; bad.slice_base = nullptr; bad.in_global_base_idx = OFF; bad.slice_bitscount = NBITS;
        CHECK(s1.enable_slice(bad) == RC_ERR_STATE, "enable: null base");
        Bits mis; mis.alloc(NBITS + 8);
        slice_config_t bad2{}; bad2.slice_base = mis.mem + 1; bad2.in_global_base_idx = OFF; bad2.slice_bitscount = NBITS;
        CHECK(s1.enable_slice(bad2) == RC_ERR_STATE, "enable: misaligned base");
        slice_config_t bad3{}; bad3.slice_base = mis.mem; bad3.in_global_base_idx = OFF; bad3.slice_bitscount = 0;
        CHECK(s1.enable_slice(bad3) == RC_ERR_STATE, "enable: zero bits");
        mis.clear_mem();
    }

    Bits b1; b1.alloc(NBITS);
    /* 空闲 run（片内偏移）：[0,10) [30,40) [60,64)  → 全局 [500,510) [530,540) [560,564) */
    b1.set_free(0, 10);
    b1.set_free(30, 10);
    b1.set_free(60, 4);
    slice_config_t cfg1{}; cfg1.slice_base = b1.mem; cfg1.in_global_base_idx = OFF; cfg1.slice_bitscount = NBITS;
    CHECK(s1.enable_slice(cfg1) == RC_OK, "enable ok");
    CHECK(s1.enable_slice(cfg1) == RC_ERR_STATE, "enable: repeat rejected");
    {
        uint64_t sum = 0; auto v = dump_vec(s1, &sum);
        CHECK(eq_intervals(v, {{500,10},{530,10},{560,4}}), "enable: 3 maximal runs");
        CHECK(sum == 24, "enable: free_count=24");
    }

    /* ═══════ 2. test_interval 两态 ═══════ */
    puts("[2] test_interval");
    CHECK(s1.test_interval(false, 530, 10) == RC_OK,        "test free: exact run");
    CHECK(s1.test_interval(false, 530, 11) == RC_ERR_NOT_FOUND, "test free: overrun run");
    CHECK(s1.test_interval(false, 535, 3)  == RC_OK,        "test free: sub-run");
    CHECK(s1.test_interval(false, 500, 11) == RC_ERR_NOT_FOUND, "test free: crosses occupied");
    CHECK(s1.test_interval(true, 510, 10)  == RC_OK,        "test occ: gap");
    CHECK(s1.test_interval(true, 525, 10)  == RC_ERR_NOT_FOUND, "test occ: touches free");
    CHECK(s1.test_interval(true, 500, 1)   == RC_ERR_NOT_FOUND, "test occ: free bit");
    CHECK(s1.test_interval(false, 530, 0)  == RC_OK,        "test len==0");
    CHECK(s1.test_interval(false, 499, 1)  == RC_ERR_STATE, "test: below slice start");
    CHECK(s1.test_interval(false, 560, 5)  == RC_ERR_STATE, "test: above slice end");
    /* bug#1 回归：base 越过片尾时，右式 `slice_bitscount_ - off` 无符号下溢会令检查失效 ——
     * 修正后必须一律 RC_ERR_STATE（修正前 base>end 会被放行，test_interval(true) 甚至误返 RC_OK）。 */
    CHECK(s1.test_interval(true,  565, 1)  == RC_ERR_STATE, "bug#1: occ base==end+1 rejected");
    CHECK(s1.test_interval(false, 565, 1)  == RC_ERR_STATE, "bug#1: free base==end+1 rejected");
    CHECK(s1.test_interval(false, 600, 4)  == RC_ERR_STATE, "bug#1: far beyond end rejected");
    CHECK(s1.test_interval(false, 564, 1)  == RC_ERR_STATE, "bug#1: base==end exact rejected");

    /* ═══════ 3. free_interval：纯插 / 融合 / 重叠 ═══════ */
    puts("[3] free_interval");
    CHECK(s1.free_interval(520, 10) == RC_OK, "free: right-merge only");
    {
        uint64_t sum = 0; auto v = dump_vec(s1, &sum);
        CHECK(eq_intervals(v, {{500,10},{520,20},{560,4}}), "free: (520,20) merged right");
        CHECK(sum == 34, "free: count +10");
    }
    CHECK(s1.free_interval(505, 3) == RC_ERR_OVERLAP, "free: overlap rejected (inside)");
    CHECK(s1.free_interval(509, 3) == RC_ERR_OVERLAP, "free: overlap rejected (straddle)");
    CHECK(s1.free_interval(510, 10) == RC_OK, "free: both-side merge");
    {
        uint64_t sum = 0; auto v = dump_vec(s1, &sum);
        CHECK(eq_intervals(v, {{500,40},{560,4}}), "free: (500,40) both merge");
        CHECK(sum == 44, "free: count +10");
    }
    CHECK(s1.free_interval(550, 5) == RC_OK, "free: pure insert (no adjacency)");
    {
        uint64_t sum = 0; auto v = dump_vec(s1, &sum);
        CHECK(eq_intervals(v, {{500,40},{550,5},{560,4}}), "free: inserted (550,5)");
        CHECK(sum == 49, "free: count +5");
    }
    CHECK(s1.free_interval(499, 1) == RC_ERR_STATE, "free: below slice start");
    CHECK(s1.free_interval(562, 4) == RC_ERR_STATE, "free: above slice end");
    /* bug#1 回归：free 亦受同一无符号下溢影响（修正前会越界插区间、污染主树）。 */
    {
        uint64_t sum_before = 0; (void)dump_vec(s1, &sum_before);
        CHECK(s1.free_interval(565, 1) == RC_ERR_STATE, "bug#1: free base==end+1 rejected");
        CHECK(s1.free_interval(600, 8) == RC_ERR_STATE, "bug#1: free far-above rejected");
        uint64_t sum_after = 0; auto v_after = dump_vec(s1, &sum_after);
        CHECK(sum_before == sum_after, "bug#1: rejected free left free_count untouched");
        CHECK(!eq_intervals(v_after, {{500,40},{550,5},{560,4},{565,1}}), "bug#1: no OOB interval inserted");
    }
    CHECK(s1.free_interval(550, 0) == RC_OK, "free: len==0 no-op");

    /* ═══════ 4. soft_occupy：前缀 / 停 / 缩劈删 / rest / hint ═══════ */
    puts("[4] soft_occupy");
    {
        uint64_t rest = 0;
        CHECK(s1.soft_occupy(505, 10, &rest) == RC_OK, "occ: claim 10");
        CHECK(rest == 0, "occ: rest=0");
        uint64_t sum = 0; auto v = dump_vec(s1, &sum);
        CHECK(eq_intervals(v, {{500,5},{515,25},{550,5},{560,4}}), "occ: split (500,40)");
        CHECK(sum == 39, "occ: count -10");
    }
    {
        uint64_t rest = 0;   /* 锚点即占用（505 已占）：NO_BLOCK */
        CHECK(s1.soft_occupy(505, 5, &rest) == RC_ERR_NO_BLOCK, "occ: anchor occupied");
        CHECK(rest == 5, "occ: rest=len on no-block");
    }
    {
        uint64_t rest = 0;   /* 吃满整段 → 删节点 */
        CHECK(s1.soft_occupy(515, 25, &rest) == RC_OK, "occ: exact-eat whole run");
        CHECK(rest == 0, "occ: rest=0");
        uint64_t sum = 0; auto v = dump_vec(s1, &sum);
        CHECK(eq_intervals(v, {{500,5},{550,5},{560,4}}), "occ: node deleted");
        CHECK(sum == 14, "occ: count -25");
    }
    {
        uint64_t rest = 0;   /* 额度 10 > 可用 5 → 停于区间尾，rest 记账 */
        CHECK(s1.soft_occupy(550, 10, &rest) == RC_OK, "occ: stop at run end");
        CHECK(rest == 5, "occ: rest = 10-5 = 5");
        uint64_t sum = 0; auto v = dump_vec(s1, &sum);
        CHECK(eq_intervals(v, {{500,5},{560,4}}), "occ: (550,5) consumed fully");
        CHECK(sum == 9, "occ: count -5");
    }
    {
        uint64_t rest = 0;   /* 前缀占领（留右残余）*/
        CHECK(s1.soft_occupy(500, 3, &rest) == RC_OK, "occ: prefix of run");
        uint64_t sum = 0; auto v = dump_vec(s1, &sum);
        CHECK(eq_intervals(v, {{503,2},{560,4}}), "occ: left residual (503,2)");
        CHECK(sum == 6, "occ: count -3");
    }
    {
        uint64_t rest = 4;   /* rest 入值 = 已满足量；real = 0 → 无事，rest 不动 */
        CHECK(s1.soft_occupy(560, 4, &rest) == RC_OK, "occ: real==0 no-op");
        CHECK(rest == 4, "occ: rest unchanged");
        uint64_t rest2 = 2;  /* real = 2 → 认领 2，出 rest = 4-2 = 2 */
        CHECK(s1.soft_occupy(560, 4, &rest2) == RC_OK, "occ: rest-accounted claim 2");
        CHECK(rest2 == 2, "occ: rest=2");
        uint64_t over = 5;   /* 口径防御：已满足量 > 需求 → RC_ERR_STATE */
        CHECK(s1.soft_occupy(560, 4, &over) == RC_ERR_STATE, "occ: rest>len rejected");
    }
    CHECK(s1.soft_occupy(499, 1, nullptr) == RC_ERR_STATE, "occ: null rest");
    {
        uint64_t r = 0;
        CHECK(s1.soft_occupy(499, 1, &r) == RC_ERR_STATE, "occ: below slice start");
        CHECK(s1.soft_occupy(565, 1, &r) == RC_ERR_STATE, "occ: above slice end");
    }

    /* ═══════ 5. all_dump 顺序 + 空片 ═══════ */
    puts("[5] all_dump");
    {
        uint64_t sum = 0; auto v = dump_vec(s1, &sum);
        /* 递增序断言 */
        bool inc = true;
        for (size_t i = 1; i < v.size(); i++) if (v[i].base <= v[i-1].base) inc = false;
        CHECK(inc, "dump: base ascending");
        CHECK(v.size() == 2, "dump: two runs remain");
    }
    {
        std::list<fs_run_t> dl;   /* nullptr 入参 */
        CHECK(s1.all_dump(nullptr) == RC_ERR_STATE, "dump: nullptr rejected");
        mgr_slice_t empty;        /* 全占用片 → 空链 */
        Bits bz; bz.alloc(NBITS); /* 全 1 = 无空闲 */
        slice_config_t cz{}; cz.slice_base = bz.mem; cz.in_global_base_idx = 0; cz.slice_bitscount = NBITS;
        CHECK(empty.enable_slice(cz) == RC_OK, "empty: enable");
        CHECK(empty.all_dump(&dl) == RC_OK, "empty: dump ok");
        CHECK(dl.empty(), "empty: no runs");
        bz.clear_mem();
    }

    /* ═══════ 6. free_intervals drain ═══════ */
    puts("[6] free_intervals");
    {
        mgr_slice_t s2;
        Bits b2; b2.alloc(32);
        /* 空闲：片内 [0,4) [8,12) [16,20) → 全局偏移 1000 → [1000,1004) [1008,1012) [1016,1020) */
        b2.set_free(0, 4); b2.set_free(8, 4); b2.set_free(16, 4);
        slice_config_t c2{}; c2.slice_base = b2.mem; c2.in_global_base_idx = 1000; c2.slice_bitscount = 32;
        CHECK(s2.enable_slice(c2) == RC_OK, "s2 enable");
        { uint64_t sum = 0; auto v = dump_vec(s2, &sum);
          CHECK(eq_intervals(v, {{1000,4},{1008,4},{1016,4}}), "s2 initial runs"); CHECK(sum==12, "s2 count=12"); }

        /* nullptr */
        CHECK(s2.free_intervals(nullptr) == RC_ERR_STATE, "free_intervals: nullptr");

        /* 正常 drain：{1004,4} 融合 [1000,1004)+[1008,1012) → [1000,1012)；{1012,4} 接上 → [1000,1020) */
        fs_run_list_t fl;
        fl.push_front(fs_run_t{1012, 4});   /* 先 push 的落在后 */
        fl.push_front(fs_run_t{1004, 4});
        CHECK(s2.free_intervals(&fl) == RC_OK, "free_intervals: drain ok");
        CHECK(fl.empty(), "free_intervals: chain drained");
        { uint64_t sum = 0; auto v = dump_vec(s2, &sum);
          CHECK(eq_intervals(v, {{1000,20}}), "free_intervals: all merged"); CHECK(sum==20, "s2 count=20"); }

        /* 中途出错：front 段重叠 → 立即报错；已消费段不回滚，余段留链 */
        fs_run_list_t bad;
        bad.push_front(fs_run_t{5000, 4});  /* 靠后：本应留链的余段 */
        bad.push_front(fs_run_t{1010, 4});  /* front 段：1010 落在已空闲 [1000,1020) → 重叠报错 */
        result_code_t rc = s2.free_intervals(&bad);
        CHECK(rc == RC_ERR_OVERLAP, "free_intervals: overlap error");
        CHECK(!bad.empty(), "free_intervals: remaining kept");
        CHECK(bad.front().base == 5000, "free_intervals: unconsumed segment kept intact");
        b2.clear_mem();
    }

    /* ═══════ 7. alloc_hard：best-fit + 头部下挖 + shutdown 对位 ═══════ */
    puts("[7] alloc_hard / shutdown");
    {
        Bits b3;
        const uint64_t B3_OFF = 700, B3_N = 64;
        b3.alloc(B3_N);                /* 全占用 */
        b3.set_free(0, 16);            /* [700,716) 空闲 16 */
        b3.set_free(24, 8);            /* [724,732) 空闲 8 */
        b3.set_free(40, 24);           /* [740,764) 空闲 24（共 48） */
        mgr_slice_t s3;
        uint64_t o = 0;
        CHECK(s3.alloc_hard(0, 1, o) == RC_ERR_STATE, "alloc_hard: gate before enable");
        slice_config_t c3{}; c3.slice_base = b3.mem; c3.in_global_base_idx = B3_OFF; c3.slice_bitscount = B3_N;
        CHECK(s3.enable_slice(c3) == RC_OK, "enable s3");
        CHECK(s3.alloc_hard(0, 0, o) == RC_ERR_STATE, "alloc_hard: zero rejected");
        /* best-fit：需 10 → 最小 len≥10 的 [700,716)(16) → 头部下挖 [700,710)，余 [710,716) */
        CHECK(s3.alloc_hard(0, 10, o) == RC_OK, "alloc_hard best-fit rc");
        CHECK(o == 706, "alloc_hard tail-carve base");
        { uint64_t sum=0; auto v = dump_vec(s3,&sum);
          CHECK(eq_intervals(v, {{700,6},{724,8},{740,24}}), "alloc_hard remainder (base kept)");
          CHECK(sum == 38, "alloc_hard count 48-10"); }
        /* 对齐：align_log2=3 需 8 → [724,732) 贴尾起点 724 非 8 对齐 → 下移 720<724 不可行 →
         * [740,764) → 贴尾 756 下移到 752（8 对齐）；挖 [752,760) → 余 [740,12)+[760,4) */
        CHECK(s3.alloc_hard(3, 8, o) == RC_OK, "alloc_hard aligned rc");
        CHECK(o == 752, "alloc_hard aligned base");
        { uint64_t sum=0; auto v = dump_vec(s3,&sum);
          CHECK(eq_intervals(v, {{700,6},{724,8},{740,12},{760,4}}), "alloc_hard aligned split");
          CHECK(sum == 30, "alloc_hard count 38-8"); }
        CHECK(s3.alloc_hard(0, 100, o) == RC_ERR_NO_BLOCK, "alloc_hard: no fit");
        /* shutdown：对位 true→false */
        CHECK(s3.shutdown() == RC_OK, "shutdown: true->false");
        CHECK(s3.shutdown() == RC_ERR_STATE, "shutdown: repeat rejected");
        CHECK(s3.test_interval(false, 700, 1) == RC_ERR_STATE, "after shutdown: gated");
        { std::list<fs_run_t> dl; CHECK(s3.all_dump(&dl) == RC_ERR_STATE, "after shutdown: dump gated"); }
        CHECK(s3.enable_slice(c3) == RC_OK, "re-enable after shutdown");
        b3.clear_mem();
    }

    /* ═══════ 8. soft_alloc：碎片优先 + all_satisfied + 部分交付 ═══════ */
    puts("[8] soft_alloc");
    {
        Bits b4;
        const uint64_t B4_OFF = 800, B4_N = 128;
        b4.alloc(B4_N);
        b4.set_free(0, 3);      /* [800,803)  len 3 */
        b4.set_free(20, 2);     /* [820,822)  len 2 */
        b4.set_free(40, 10);    /* [840,850)  len 10 */
        b4.set_free(80, 20);    /* [880,900)  len 20（共 35） */
        mgr_slice_t s4;
        bool sat = true;
        fs_run_list_t r0;
        CHECK(s4.soft_alloc(0, 1, &r0, sat) == RC_ERR_STATE, "soft_alloc: gate before enable");
        slice_config_t c4{}; c4.slice_base = b4.mem; c4.in_global_base_idx = B4_OFF; c4.slice_bitscount = B4_N;
        CHECK(s4.enable_slice(c4) == RC_OK, "enable s4");
        CHECK(s4.soft_alloc(0, 0, &r0, sat) == RC_ERR_STATE, "soft_alloc: zero rejected");
        CHECK(s4.soft_alloc(0, 1, nullptr, sat) == RC_ERR_STATE, "soft_alloc: null runs rejected");

        /* 6 位：小碎片优先 [800,803)+[820,822) 整吃，再从 [840,850) 挖尾 1 → {849,1} */
        fs_run_list_t r1;
        CHECK(s4.soft_alloc(0, 6, &r1, sat) == RC_OK, "soft_alloc rc");
        CHECK(sat == true, "soft_alloc: satisfied");
        { std::vector<fs_run_t> rv(r1.begin(), r1.end());
          uint64_t s = 0; for (auto &x : rv) s += x.len;
          CHECK(rv.size() == 3, "soft_alloc: 3 fragments");
          CHECK(s == 6, "soft_alloc: 6 bits delivered");
          CHECK(rv[0].base == 849 && rv[0].len == 1, "soft_alloc: tail-carve {849,1} at list head"); }
        { uint64_t sum=0; auto v = dump_vec(s4,&sum);
          CHECK(eq_intervals(v, {{840,9},{880,20}}), "soft_alloc: big run [880,20) untouched");
          CHECK(sum == 29, "soft_alloc count 35-6"); }

        /* 100 位：穷尽剩余 [840,9)+[880,20)=29 → 部分交付 + all_satisfied=false */
        fs_run_list_t r2;
        CHECK(s4.soft_alloc(0, 100, &r2, sat) == RC_OK, "soft_alloc partial rc");
        CHECK(sat == false, "soft_alloc: not satisfied (partial)");
        { std::vector<fs_run_t> rv(r2.begin(), r2.end());
          uint64_t s = 0; for (auto &x : rv) s += x.len;
          CHECK(s == 29, "soft_alloc: delivered all remaining 29"); }
        { uint64_t sum=0; auto v = dump_vec(s4,&sum);
          CHECK(v.empty(), "soft_alloc: slice drained");
          CHECK(sum == 0, "soft_alloc: free_count 0"); }

        /* 空片：一位都拿不到 → NO_BLOCK */
        fs_run_list_t r3;
        CHECK(s4.soft_alloc(0, 1, &r3, sat) == RC_ERR_NO_BLOCK, "soft_alloc: no block");
        CHECK(sat == false, "soft_alloc: no-block not satisfied");
        CHECK(r3.empty(), "soft_alloc: no runs on failure");
        b4.clear_mem();
    }

    /* ═══════ 9. bug#2 回归：非空树 shutdown（侵入从树钩子先摘，再析构主树元素）═══════
     * 修正前：shutdown() 先 pos_.clear() 析构元素 → size_ 仍链接已释放节点 →
     *   安全钩子断言 `!hook.is_linked()` 失败 / Release 静默 UAF。修正后应 RC_OK 且可重建。 */
    puts("[9] shutdown on non-empty tree (bug#2)");
    {
        mgr_slice_t s9;
        Bits b9; b9.alloc(64);
        b9.set_free(0, 8); b9.set_free(16, 8); b9.set_free(32, 8);
        slice_config_t c9{}; c9.slice_base = b9.mem; c9.in_global_base_idx = 900; c9.slice_bitscount = 64;
        CHECK(s9.enable_slice(c9) == RC_OK, "bug#2: enable non-empty");
        CHECK(s9.shutdown() == RC_OK, "bug#2: shutdown non-empty (no UAF)");
        CHECK(s9.enable_slice(c9) == RC_OK, "bug#2: re-enable after shutdown");
        { uint64_t sum = 0; auto v = dump_vec(s9, &sum);
          CHECK(eq_intervals(v, {{900,8},{916,8},{932,8}}), "bug#2: trees rebuilt correct");
          CHECK(sum == 24, "bug#2: free_count rebuilt"); }
        CHECK(s9.shutdown() == RC_OK, "bug#2: second shutdown");
        b9.clear_mem();
    }

    b1.clear_mem();

    printf("\n==== test_fs_allocator: PASS %d, FAIL %d ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

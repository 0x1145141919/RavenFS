/* ═══════════════════════════════════════════════════════════════════
 * test_extent_ops.cpp — extent_map_range / extent_unmap_range 原语对专项
 *
 * 按 Sparrow 2026-09-03 建议的测试路线：
 *   1. root_dir_insert_dentry 建真 REG 文件（正门拿 inode，不伪造槽）
 *   2. 测试内做 inline→extent 跃迁（alloc 树根 2 块 + 根叶 meta +
 *      inode.data_root 回写）——模拟 direct_file_* 层未来 helper 的形态
 *   3. 对文件 extent 树直接 map/unmap：
 *      A. 基础语义：纯插/左并/右并桥接/重叠报错/三字段全等校验/NOT_FOUND
 *      B. 分裂级联：340 条记录打穿根叶裂 → 根内部（depth2）→ 全删触发
 *         欠载合并/根收缩 → 回到空根叶
 *   全程树完整性审计（DFS：键序/父子指针/max key/兄弟链/canonical 邻接）。
 *
 * 假性分配：数据 phys 全部编造（不经 block_alloc）——原语对不碰数据侧
 * 位图，天然支持"纯记录不落实分配契约"；树节点块（2 块一组）真分配。
 *
 * 用法：ravenfs_juv_extent_ops <device>            （A/B/C/D 串行语义组）
 *       ravenfs_juv_extent_ops <device> par [workers] （E 组：并行压测）
 *       E 组：workers（默认 8）个线程各持伪造 processor_id，各对独立文件跑
 *       12 万级极端增删——并发竞争面 = block/inode 分片分配器 per-CPU 路由
 *       + 缓存锁 + 盘；死锁由看门狗盯（30s 无推进 → abort 留现场）。
 *       要求盘是 mkfs --inode-count≥262144（8 片需要）且每次 par 前重 mkfs。
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session 完整类型 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static linux_userspace_mount_t make_mount(const char *path, uint32_t cpu_count = 1) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, O_RDWR|O_DIRECT);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_NVME;
    mnt.dev.is_device_direct = true;
    mnt.param = {.if_will_use_inode_cache=true,.if_will_use_mdata_piece_cache=true};
    mnt.logical_processor_count = cpu_count;   /* 假 CPU 数 = 并行 worker 数（切片几何按它展开） */
    return mnt;
}

static std::atomic<int> g_pass{0}, g_fail{0};
#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

static const char *rc_name(result_code_t rc) {
    switch (rc) {
    case RC_OK:             return "RC_OK";
    case RC_ERR_OPEN:       return "RC_ERR_OPEN";
    case RC_ERR_MAGIC:      return "RC_ERR_MAGIC";
    case RC_ERR_VERSION:    return "RC_ERR_VERSION";
    case RC_ERR_CRC:        return "RC_ERR_CRC";
    case RC_ERR_BITMAP_SELF:return "RC_ERR_BITMAP_SELF";
    case RC_ERR_EXIST:      return "RC_ERR_EXIST";
    case RC_ERR_NOT_FOUND:  return "RC_ERR_NOT_FOUND";
    case RC_ERR_NO_BLOCK:   return "RC_ERR_NO_BLOCK";
    case RC_ERR_NO_INODE:   return "RC_ERR_NO_INODE";
    case RC_ERR_NAME_LEN:   return "RC_ERR_NAME_LEN";
    case RC_ERR_IO:         return "RC_ERR_IO";
    case RC_ERR_STATE:      return "RC_ERR_STATE";
    case RC_ERR_NOT_EMPTY:  return "RC_ERR_NOT_EMPTY";
    case RC_ERR_NOT_SUPPORTED: return "RC_ERR_NOT_SUPPORTED";
    case RC_ERR_OVERLAP:    return "RC_ERR_OVERLAP";
    default:                return "RC_???";
    }
}

static extent_t mk_ext(uint64_t fb, uint64_t len, uint64_t phys) {
    extent_t e{};
    e.fblkbase = fb;
    e.length = len;
    e.block_base.ptr = phys;       /* 假性 phys：纯记录，不落实分配 */
    e.block_base.free_to_use = 0;
    return e;
}

/* ── inline → extent-tree 跃迁（模拟 direct_file 层未来 helper）── */
static uint64_t file_to_extent_mode(RavenFS_juvenile &fs, uint64_t inum) {
    uint64_t root_blk = fs.block_alloc_hard(2, 1);
    if (root_blk == PTR_INVALID) return PTR_INVALID;
    {
        mdata_piece_session s(fs.mdata_piece_mgr, root_blk);
        auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
        memset(s.data, 0, 8192);
        leaf->meta.magics[0] = BPTREE_MAGIC1;
        leaf->meta.magics[1] = BPTREE_MAGIC2;
        leaf->meta.magics[2] = BPTREE_MAGIC3;
        leaf->meta.magics[3] = BPTREE_MAGIC4;
        leaf->meta.parent_ptr.entry_count   = 0;
        leaf->meta.parent_ptr.is_inner_node = 0;
        leaf->meta.parent_ptr.is_root_node  = 1;
        leaf->meta.parent_ptr.ptr           = inum;   /* "." 审计锚 */
        leaf->meta.ptrs[0].ptr = PTR_INVALID;         /* 文件树根无链表无 ".." */
        leaf->meta.ptrs[1].ptr = PTR_INVALID;
        leaf->meta.ptrs[2].ptr = PTR_INVALID;
        s.dirty();
    }
    raven_Inode_t ino;
    fs.inode_read(inum, &ino);
    ino.data_root = root_blk;
    ino.is_data_root_direct_bit = 0;   /* extent-tree 模式 */
    fs.inode_write(inum, &ino);
    return root_blk;
}

/* ═══════════════════ 树完整性审计（DFS）═══════════════════ */
struct audit_t {
    uint64_t nodes = 0, inner = 0, leaves = 0, records = 0;
    uint64_t bad = 0;
    std::vector<uint64_t> leaf_blk;      /* DFS 序（左→右） */
    std::vector<uint64_t> leaf_cnt;
    std::vector<uint64_t> leaf_end;      /* 末记录开边界 fblkbase+length */
    std::vector<uint64_t> leaf_phys_end; /* 末记录 phys 尾 */
};

static uint64_t audit_node(RavenFS_juvenile &fs, audit_t &a, uint64_t blk,
                           uint64_t expect_parent, bool expect_root,
                           uint64_t inum, uint64_t depth) {
    if (depth > 64) { a.bad++; return 0; }
    mdata_piece_session s(fs.mdata_piece_mgr, blk);
    if (s.index_error() || !s) { a.bad++; return 0; }
    Bptree_meta_t *meta = (Bptree_meta_t *)s.data;
    if (!is_extent_node(meta)) { a.bad++; return 0; }
    if (meta->parent_ptr.is_root_node != expect_root) { a.bad++; return 0; }
    if (expect_root) {
        if (meta->parent_ptr.ptr != inum) a.bad++;   /* 根 "." 锚 */
    } else {
        if (meta->parent_ptr.ptr != expect_parent) a.bad++;   /* 子父指针一致 */
    }
    a.nodes++;
    const uint16_t cnt = (uint16_t)meta->parent_ptr.entry_count;

    if (!meta->parent_ptr.is_inner_node) {
        a.leaves++;
        auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
        a.records += cnt;
        if (cnt > 337) a.bad++;
        if (!expect_root && (cnt < 168 || cnt == 0)) a.bad++;   /* 非根欠载不允许残留 */
        for (uint16_t i = 0; i < cnt; i++) {
            const extent_t e = leaf->extents[i];
            if (i > 0) {
                const extent_t p = leaf->extents[i - 1];
                if (p.fblkbase + p.length > e.fblkbase) a.bad++;            /* 重叠/逆序 */
                if (p.fblkbase + p.length == e.fblkbase &&
                    p.block_base.ptr + p.length == e.block_base.ptr) a.bad++; /* 可合并未合并 */
            }
        }
        a.leaf_blk.push_back(blk);
        a.leaf_cnt.push_back(cnt);
        if (cnt > 0) {
            const extent_t last = leaf->extents[cnt - 1];
            a.leaf_end.push_back(last.fblkbase + last.length);
            a.leaf_phys_end.push_back(last.block_base.ptr + last.length);
        } else {
            a.leaf_end.push_back(0);
            a.leaf_phys_end.push_back(0);
        }
        return cnt > 0 ? a.leaf_end.back() : 0;
    }

    /* 内部节点 */
    a.inner++;
    auto *inner = (Bptree_extent_inner_node_t *)s.data;
    if (cnt == 0) a.bad++;
    if (cnt > 337) a.bad++;
    if (expect_root && cnt == 1) a.bad++;          /* 根内部单孩残留（应收缩） */
    if (!expect_root && cnt < 168) a.bad++;        /* 非根内部欠载 */
    uint64_t prev_key = 0;
    uint64_t subtree_max = 0;
    for (uint16_t i = 0; i < cnt; i++) {
        const extent_hint_t h = inner->extents[i];
        if (h.sub_node_ptr.ptr == 0 || h.sub_node_ptr.ptr == PTR_INVALID) a.bad++;
        if (i > 0 && h.key <= prev_key) a.bad++;   /* key 严格递增 */
        prev_key = h.key;
        uint64_t child_max = audit_node(fs, a, h.sub_node_ptr.ptr, blk, false, inum, depth + 1);
        if (h.key != child_max) a.bad++;           /* hint key == 子树最大开边界 */
        if (child_max > subtree_max) subtree_max = child_max;
    }
    return prev_key;
}

/* 兄弟链校验：从最左叶沿 ptrs[2] 走到尾，须与 DFS 叶序完全一致；
 * 跨叶 canonical：前叶末项与后叶首项不得可合并。 */
static void audit_chain(RavenFS_juvenile &fs, audit_t &a, uint64_t root_blk) {
    /* 下钻最左叶 */
    uint64_t blk = root_blk;
    for (int d = 0; d < 64; d++) {
        mdata_piece_session s(fs.mdata_piece_mgr, blk);
        if (s.index_error() || !s) { a.bad++; return; }
        Bptree_meta_t *m = (Bptree_meta_t *)s.data;
        if (!m->parent_ptr.is_inner_node) break;
        auto *inner = (Bptree_extent_inner_node_t *)s.data;
        if (inner->meta.parent_ptr.entry_count == 0) { a.bad++; return; }
        blk = inner->extents[0].sub_node_ptr.ptr;
    }
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    size_t idx = 0;
    uint64_t prev_end = 0, prev_phys_end = 0;
    bool have_prev = false;
    while (blk != PTR_INVALID_52 && idx < a.leaf_blk.size() + 1) {
        if (idx >= a.leaf_blk.size()) { a.bad++; return; }   /* 链比 DFS 长 */
        if (blk != a.leaf_blk[idx]) { a.bad++; return; }      /* 链序 != DFS 序 */
        if (a.leaf_cnt[idx] > 0 && have_prev) {
            /* 跨叶 canonical：gap==0 && phys 连续 → 应已合并 */
            uint64_t end0 = a.leaf_end[idx - 1], ph0 = a.leaf_phys_end[idx - 1];
            mdata_piece_session s(fs.mdata_piece_mgr, blk);
            if (s.index_error() || !s) { a.bad++; return; }
            auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
            const extent_t f = leaf->extents[0];
            if (end0 == f.fblkbase && ph0 == f.block_base.ptr) a.bad++;
        }
        have_prev = true;
        prev_end = a.leaf_end[idx];
        prev_phys_end = a.leaf_phys_end[idx];
        idx++;
        mdata_piece_session s(fs.mdata_piece_mgr, blk);
        if (s.index_error() || !s) { a.bad++; return; }
        Bptree_meta_t *m = (Bptree_meta_t *)s.data;
        if (!m->parent_ptr.is_inner_node) {
            auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
            if (idx < a.leaf_blk.size() && leaf->meta.ptrs[2].ptr == PTR_INVALID_52) { a.bad++; return; }
            blk = leaf->meta.ptrs[2].ptr;
        } else {
            a.bad++; return;
        }
    }
    if (idx != a.leaf_blk.size()) a.bad++;   /* 链比 DFS 短 */
}

static void audit_run(RavenFS_juvenile &fs, uint64_t inum, const char *tag, audit_t *out) {
    audit_t a;
    raven_Inode_t ino;
    fs.inode_read(inum, &ino);
    uint64_t root = ino.data_root;
    uint64_t sm = audit_node(fs, a, root, 0, true, inum, 0);
    (void)sm;
    if (a.leaves > 0) audit_chain(fs, a, root);
    if (out) *out = a;
    char msg[256];
    snprintf(msg, sizeof(msg),
             "audit[%s]: nodes=%lu inner=%lu leaves=%lu records=%lu bad=%lu",
             tag, a.nodes, a.inner, a.leaves, a.records, a.bad);
    CHECK(a.bad == 0, "%s", msg);
}

/* ═══════════════════════════════════════════════════════════════════
 * E 组：并行压测（真盘直写，W 假 CPU × 每文件 12 万级极端增删）
 *
 * 模型：单个共享 fs 实例（一份位图/缓存/盘），W 个 worker 线程；每个 worker
 * 持有伪造 processor_id ∈ [0,W)（thread_local 强定义覆盖 cpu_id_stub 的 weak
 * 桩，等价用户态每线程 GS 假 id 的轻量版），各自对**独立文件**跑 D 组同款
 * 12 万条打穿内部层增删——per-file 树互不共享 → 并发竞争面精确收敛在：
 *   block_alloc_hard/soft/inode_alloc 的 per-CPU preferred 分片路由 + try/自旋
 *   锁、mdata/inode 缓存 RADIX 锁、盘本身。
 *
 * 正确性：每 worker 相位间全树审计（键序/父子/hint/兄弟链/canonical）；
 * 性能：每相位计时 + 总壁钟；
 * 死锁预案：看门狗 2s 轮询各 worker 推进（ops+stage），30s 全局无推进 →
 * 打印各 worker 现场并 abort()（SIGABRT + core，留证据）。
 * ═══════════════════════════════════════════════════════════════════ */
static constexpr uint64_t PAR_NA = 120000;    /* 每 worker A 空间条数（>113569，打穿内部层） */
static constexpr uint64_t PAR_NB = 60000;     /* 每 worker B 空间条数 */
static constexpr uint64_t PAR_B_BASE = 400000;/* B 空间 fblkbase 起点（与 A 的 [0,240000) 不重叠） */

/* 伪造 processor_id：每线程一份（等价内核 GS per-cpu slot 的用户态替身） */
static thread_local uint32_t t_fake_proc_id = 0;
extern "C" uint32_t get_processor_id() { return t_fake_proc_id; }  /* 强定义，覆盖 weak 桩 */

struct par_worker_t {
    RavenFS_juvenile *fs = nullptr;
    uint64_t inum = 0;
    uint32_t fake_id = 0;
    uint64_t seed = 0;
    std::atomic<uint64_t> ops{0};      /* 看门狗：已完成原语数 */
    std::atomic<int> stage{0};         /* 看门狗：当前相位 */
    uint64_t ms_insA = 0, ms_delH = 0, ms_insB = 0, ms_delA = 0, ms_churn = 0;
    uint64_t not_found = 0;
    bool ok = true;
    uint32_t fail_k = 0;
    result_code_t fail_rc = RC_OK;
    char fail_ph[24] = {0};
};

static const char *par_stage_name(int s) {
    switch (s) {
    case 1: return "insA";        case 2: return "audit-full";
    case 3: return "delHalf";     case 4: return "audit-half";
    case 5: return "insB";        case 6: return "audit-refill";
    case 7: return "delAll";      case 8: return "audit-final";
    case 9: return "inode-churn"; default: return "idle";
    }
}

#define PAR_FAIL(ph_, k_, rc_)                                             \
    do { W->ok = false; W->fail_k = (uint32_t)(k_); W->fail_rc = (rc_);    \
         snprintf(W->fail_ph, sizeof(W->fail_ph), "%s", (ph_)); return; }  \
    while (0)

static void par_worker_run(par_worker_t *W) {
    t_fake_proc_id = W->fake_id;   /* 本线程伪造 processor_id（挂到本线程身份） */
    RavenFS_juvenile &fs = *W->fs;
    const uint64_t inum = W->inum;
    const uint64_t PHY_BASE = 0x200000ull + (uint64_t)W->fake_id * 0x1000000ull;
    const uint64_t NA = PAR_NA, NB = PAR_NB, B_BASE = PAR_B_BASE;
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    auto ms_el = [&]() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   clk::now() - t0).count();
    };
    result_code_t r;
    std::mt19937 rng(W->seed);
    std::vector<uint64_t> ord;

    /* ── 阶段 1：随机序插 A 空间 12 万（同 D 组：fb=k*2，永不合并） ── */
    W->stage = 1;
    {
        ord.resize(NA);
        for (uint64_t i = 0; i < NA; i++) ord[i] = i;
        std::shuffle(ord.begin(), ord.end(), rng);
        uint64_t ta = ms_el();
        for (uint64_t k = 0; k < NA; k++) {
            uint64_t fb = ord[k] * 2;
            r = fs.extent_map_range(inum, mk_ext(fb, 1, PHY_BASE + fb));
            W->ops.fetch_add(1, std::memory_order_relaxed);
            if (r != RC_OK) PAR_FAIL("insA", k, r);
        }
        W->ms_insA = ms_el() - ta;
    }

    /* ── 阶段 2：满树审计（打穿内部层形态） ── */
    W->stage = 2;
    {
        audit_t a;
        char tag[32]; snprintf(tag, sizeof(tag), "E#%u-full", W->fake_id);
        audit_run(fs, inum, tag, &a);
        CHECK(a.records == NA && a.inner >= 2 && a.leaves > 338 && a.bad == 0,
              "par#%u 满树 records=%lu inner=%lu leaves=%lu",
              W->fake_id, a.records, a.inner, a.leaves);
        if (a.records != NA || a.bad != 0) PAR_FAIL("audit-full", 0, RC_ERR_STATE);
    }

    /* ── 阶段 3：随机序删 A 空间 6 万（欠载平衡在随机叶触发） ── */
    W->stage = 3;
    {
        uint64_t ta = ms_el();
        for (uint64_t k = 0; k < NA / 2; k++) {
            uint64_t fb = ord[k] * 2;
            r = fs.extent_unmap_range(inum, mk_ext(fb, 1, PHY_BASE + fb));
            W->ops.fetch_add(1, std::memory_order_relaxed);
            if (r != RC_OK) PAR_FAIL("delHalf", k, r);
        }
        W->ms_delH = ms_el() - ta;
    }

    /* ── 阶段 4：半树审计（内部层合并/收缩路径） ── */
    W->stage = 4;
    {
        audit_t a;
        char tag[32]; snprintf(tag, sizeof(tag), "E#%u-half", W->fake_id);
        audit_run(fs, inum, tag, &a);
        CHECK(a.records == NA / 2 && a.bad == 0,
              "par#%u 半树 records=%lu", W->fake_id, a.records);
        if (a.records != NA / 2 || a.bad != 0) PAR_FAIL("audit-half", 0, RC_ERR_STATE);
    }

    /* ── 阶段 5：随机序插 B 空间 6 万（穿插回既有树，二次打穿） ── */
    W->stage = 5;
    {
        ord.resize(NB);
        for (uint64_t i = 0; i < NB; i++) ord[i] = i;
        std::shuffle(ord.begin(), ord.end(), rng);
        uint64_t ta = ms_el();
        for (uint64_t k = 0; k < NB; k++) {
            uint64_t fb = B_BASE + ord[k] * 2;
            r = fs.extent_map_range(inum, mk_ext(fb, 1, PHY_BASE + fb));
            W->ops.fetch_add(1, std::memory_order_relaxed);
            if (r != RC_OK) PAR_FAIL("insB", k, r);
        }
        W->ms_insB = ms_el() - ta;
    }

    /* ── 阶段 6：回满审计 ── */
    W->stage = 6;
    {
        audit_t a;
        char tag[32]; snprintf(tag, sizeof(tag), "E#%u-refill", W->fake_id);
        audit_run(fs, inum, tag, &a);
        CHECK(a.records == NA && a.bad == 0,
              "par#%u 回满 records=%lu", W->fake_id, a.records);
        if (a.records != NA || a.bad != 0) PAR_FAIL("audit-refill", 0, RC_ERR_STATE);
    }

    /* ── 阶段 7：随机序全删（A 全 12 万 + B 6 万；一半 A 本不在树 → NOT_FOUND） ── */
    W->stage = 7;
    {
        ord.resize(NA + NB);
        for (uint64_t i = 0; i < NA; i++) ord[i] = i * 2;
        for (uint64_t i = 0; i < NB; i++) ord[NA + i] = B_BASE + i * 2;
        std::shuffle(ord.begin(), ord.end(), rng);
        uint64_t ta = ms_el();
        for (uint64_t k = 0; k < ord.size(); k++) {
            r = fs.extent_unmap_range(inum, mk_ext(ord[k], 1, PHY_BASE + ord[k]));
            if (r == RC_ERR_NOT_FOUND) { W->not_found++; continue; }
            W->ops.fetch_add(1, std::memory_order_relaxed);
            if (r != RC_OK) PAR_FAIL("delAll", k, r);
        }
        W->ms_delA = ms_el() - ta;
    }

    /* ── 阶段 8：终态审计（收缩链走完 → 空根叶） ── */
    W->stage = 8;
    {
        audit_t a;
        char tag[32]; snprintf(tag, sizeof(tag), "E#%u-final", W->fake_id);
        audit_run(fs, inum, tag, &a);
        CHECK(a.records == 0 && a.leaves == 1 && a.nodes == 1 && a.bad == 0,
              "par#%u 终态 records=%lu leaves=%lu nodes=%lu",
              W->fake_id, a.records, a.leaves, a.nodes);
        if (a.records != 0 || a.leaves != 1 || a.nodes != 1 || a.bad != 0)
            PAR_FAIL("audit-final", 0, RC_ERR_STATE);
    }

    /* ── 阶段 9：inode 分片并行冒烟（每 worker 2048 对 alloc/free，碰 inode 侧锁） ── */
    W->stage = 9;
    {
        uint64_t ta = ms_el();
        for (uint64_t k = 0; k < 2048; k++) {
            uint64_t ino = fs.inode_alloc();
            if (ino == PTR_INVALID) PAR_FAIL("churn", k, RC_ERR_NO_INODE);
            fs.inode_free(ino);   /* 释放自旋：并发下真枪实弹打 inode 片锁 */
            W->ops.fetch_add(1, std::memory_order_relaxed);
        }
        W->ms_churn = ms_el() - ta;
    }
}

/* 看门狗：2s 轮询；30s 无任何推进（ops/stage 全静止）→ 打印现场 + abort */
static void par_watchdog(std::vector<par_worker_t> &args, std::atomic<bool> &all_done) {
    std::vector<uint64_t> last_ops(args.size(), 0);
    std::vector<int> last_stage(args.size(), 0);
    int stall = 0, tick = 0;
    while (!all_done.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        uint64_t adv = 0, live = 0;
        for (size_t i = 0; i < args.size(); i++) {
            uint64_t o = args[i].ops.load(std::memory_order_relaxed);
            int s = args[i].stage.load(std::memory_order_relaxed);
            adv += (o - last_ops[i]) + (uint64_t)(s != last_stage[i]);
            live += o;
            last_ops[i] = o; last_stage[i] = s;
        }
        if (adv == 0) {
            if (++stall >= 15) {
                fprintf(stderr,
                        "\n[WATCHDOG] 30s 全局零推进 —— 疑似死锁/活锁！各 worker 现场：\n");
                for (size_t i = 0; i < args.size(); i++)
                    fprintf(stderr, "  fake_cpu=%u  stage=%s  ops=%lu\n",
                            args[i].fake_id,
                            par_stage_name(args[i].stage.load(std::memory_order_relaxed)),
                            args[i].ops.load(std::memory_order_relaxed));
                fflush(stderr);
                std::abort();   /* SIGABRT + core 抓现场 */
            }
        } else {
            stall = 0;
        }
        if ((++tick % 7) == 0)
            printf("  [E] 看门狗心跳 %lus：累计原语 %lu\n", (unsigned long)(tick * 2),
                   (unsigned long)live);
    }
}

static void run_parallel_group(RavenFS_juvenile &fs, uint32_t workers) {
    printf("\n-- E：并行压测（%u 假 CPU/worker × 每文件 12 万记录增删，真盘直写） --\n",
           workers);
    /* 准备：串行建 W 个独立文件（正门拿 inode + 跃迁建树根） */
    std::vector<uint64_t> inums(workers, 0);
    for (uint32_t w = 0; w < workers; w++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "par%u.bin", w);
        result_code_t r = fs.root_dir_insert_dentry(nm, INODE_TYPE_REGULAR);
        if (r != RC_OK) {
            CHECK(false, "建 %s → %s（E 组需全新 mkfs：--inode-count≥%u 供 %u 片）",
                  nm, rc_name(r), workers * 32768u, workers);
            return;
        }
        uint64_t inum = 0;
        fs.root_dir_lookup(nm, &inum);
        if (file_to_extent_mode(fs, inum) == PTR_INVALID) {
            CHECK(false, "%s 跃迁建树根失败", nm);
            return;
        }
        inums[w] = inum;
    }
    printf("  已建 %u 个独立文件（par0..par%u），线程就绪\n", workers, workers - 1);

    std::vector<par_worker_t> args(workers);
    std::vector<std::thread> ths;
    ths.reserve(workers);
    for (uint32_t w = 0; w < workers; w++) {
        args[w].fs = &fs;
        args[w].inum = inums[w];
        args[w].fake_id = w;
        args[w].seed = 0x9E3779B97F4A7C15ull ^ (0x9E3779B1ull * (w + 1));
    }

    std::atomic<bool> all_done{false};
    std::thread watchdog(par_watchdog, std::ref(args), std::ref(all_done));
    auto twall = std::chrono::steady_clock::now();
    for (uint32_t w = 0; w < workers; w++) ths.emplace_back(par_worker_run, &args[w]);
    for (uint32_t w = 0; w < workers; w++) ths[w].join();
    uint64_t wall_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - twall).count();
    all_done = true;
    watchdog.join();

    printf("\nE 结果表（每 worker 独立文件 / 独立 fake processor_id）：\n");
    printf("  %-4s %8s %8s %8s %8s %8s %8s %6s  %s\n", "cpu", "insA_ms",
           "delH_ms", "insB_ms", "delA_ms", "churn_ms", "sum_ms", "NF", "状态");
    bool all_ok = true;
    uint64_t tot_nf = 0, tot_ms = 0;
    for (uint32_t w = 0; w < workers; w++) {
        par_worker_t &a = args[w];
        uint64_t sum = a.ms_insA + a.ms_delH + a.ms_insB + a.ms_delA + a.ms_churn;
        tot_ms += sum; tot_nf += a.not_found;
        if (!a.ok) all_ok = false;
        printf("  %-4u %8lu %8lu %8lu %8lu %8lu %8lu %6lu  %s%s\n",
               a.fake_id, a.ms_insA, a.ms_delH, a.ms_insB, a.ms_delA, a.ms_churn,
               sum, a.not_found, a.ok ? "OK" : "FAIL",
               a.ok ? "" : "");
        if (!a.ok)
            printf("        └─ fail@%s k=%u rc=%s\n", a.fail_ph, a.fail_k, rc_name(a.fail_rc));
    }
    uint64_t total_ops = workers * (PAR_NA + PAR_NA / 2 + PAR_NB + PAR_NA + PAR_NB);
    double ops_s = (double)total_ops * 1000.0 / (double)(wall_ms ? wall_ms : 1);
    printf("  总壁钟 %lu ms；原语 %lu 个（不含 NOT_FOUND 与 churn）；聚合 %lu ops/s\n",
           wall_ms, total_ops, (unsigned long)ops_s);
    CHECK(all_ok, "E 全部 worker 无预期外 RC");
    CHECK(tot_nf == workers * (PAR_NA / 2),
          "E 总 NOT_FOUND=%lu（期望 %lu，A 半空间本不在树）",
          tot_nf, workers * (PAR_NA / 2));
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device> [par [workers]]\n", argv[0]); return 2; }
    uint32_t par_workers = 0;
    if (argc >= 3 && strcmp(argv[2], "par") == 0) {
        par_workers = (argc >= 4) ? (uint32_t)strtoul(argv[3], nullptr, 10) : 8;
        if (par_workers == 0 || par_workers > 64) {
            printf("workers ∈ [1,64]\n");
            return 2;
        }
    }

    uint64_t rc;
    linux_userspace_mount_t mnt = make_mount(argv[1], par_workers ? par_workers : 1);
    RavenFS_juvenile fs(mnt, &rc);
    if (!fs.is_mounted()) { printf("mount failed rc=%s\n", rc_name(rc)); return 1; }

    if (par_workers > 0) {
        run_parallel_group(fs, par_workers);
    } else {
    /* ── 1. root_dir 正门创建真 REG 文件 ── */
    printf("-- 准备：root_dir 建文件 + inline→extent 跃迁 --\n");
    rc = fs.root_dir_insert_dentry("a.bin", INODE_TYPE_REGULAR);
    CHECK(rc == RC_OK, "root_dir_insert_dentry(a.bin, REGULAR) = %s", rc_name(rc));
    uint64_t inum = 0;
    rc = fs.root_dir_lookup("a.bin", &inum);
    CHECK(rc == RC_OK && inum != 0, "lookup a.bin → inum=%lu", inum);
    uint64_t root_blk = file_to_extent_mode(fs, inum);
    CHECK(root_blk != PTR_INVALID, "inline→extent 跃迁建树根 blk=%lu", root_blk);

    /* ── 2. A 组：基础语义 ── */
    printf("\n-- A：基础 map/unmap 语义（单叶根树） --\n");
    audit_run(fs, inum, "empty", nullptr);

    CHECK(fs.extent_map_range(inum, mk_ext(0, 4, 100)) == RC_OK, "map [0,4)p100");
    audit_run(fs, inum, "A1", nullptr);

    CHECK(fs.extent_map_range(inum, mk_ext(4, 2, 104)) == RC_OK, "map [4,6)p104 → 左并");
    CHECK(fs.extent_map_range(inum, mk_ext(10, 3, 200)) == RC_OK, "map [10,13)p200（留洞 [6,10)）");

    audit_t a2;
    audit_run(fs, inum, "A2", &a2);
    CHECK(a2.records == 2, "A2 records=2（[0,6)p100 与 [10,13)p200）got %lu", a2.records);

    CHECK(fs.extent_map_range(inum, mk_ext(6, 4, 106)) == RC_OK, "map [6,10)p106 → 双并桥接");
    audit_t a3;
    audit_run(fs, inum, "A3", &a3);
    CHECK(a3.records == 2, "A3 records=2（[0,10)p100 与 [10,13)p200）got %lu", a3.records);

    /* 重叠防御 */
    CHECK(fs.extent_map_range(inum, mk_ext(1, 1, 0)) == RC_ERR_OVERLAP, "overlap [1,2) 内嵌 → OVERLAP");
    CHECK(fs.extent_map_range(inum, mk_ext(5, 3, 0)) == RC_ERR_OVERLAP, "overlap [5,8) 覆盖 → OVERLAP");
    CHECK(fs.extent_map_range(inum, mk_ext(9, 2, 7)) == RC_ERR_OVERLAP, "overlap [9,11) 跨记录 → OVERLAP");
    CHECK(fs.extent_map_range(inum, mk_ext(0, 13, 1)) == RC_ERR_OVERLAP, "overlap [0,13) 全包 → OVERLAP");
    audit_t a4;
    audit_run(fs, inum, "A4(overlap 后无副作用)", &a4);
    CHECK(a4.records == 2 && a4.bad == 0, "overlap 失败零副作用 records=2");

    /* unmap：三字段全等校验 */
    CHECK(fs.extent_unmap_range(inum, mk_ext(10, 3, 200)) == RC_OK, "unmap 精确 [10,13)p200");
    audit_t a5;
    audit_run(fs, inum, "A5", &a5);
    CHECK(a5.records == 1, "A5 records=1 got %lu", a5.records);

    CHECK(fs.extent_unmap_range(inum, mk_ext(0, 10, 999)) == RC_ERR_STATE, "unmap phys 失配 → STATE");
    CHECK(fs.extent_unmap_range(inum, mk_ext(0, 9, 100)) == RC_ERR_STATE,  "unmap length 失配 → STATE");
    CHECK(fs.extent_unmap_range(inum, mk_ext(1, 9, 100)) == RC_ERR_STATE,  "unmap fblkbase 失配 → STATE");
    CHECK(fs.extent_unmap_range(inum, mk_ext(20, 1, 50)) == RC_ERR_NOT_FOUND, "unmap 空洞 → NOT_FOUND");
    audit_run(fs, inum, "A6(失配后无副作用)", nullptr);

    CHECK(fs.extent_unmap_range(inum, mk_ext(0, 10, 100)) == RC_OK, "unmap [0,10)p100 → 空树");
    audit_t a7;
    audit_run(fs, inum, "A7", &a7);
    CHECK(a7.records == 0 && a7.leaves == 1, "A7 空根叶 records=0 got %lu", a7.records);
    CHECK(fs.extent_unmap_range(inum, mk_ext(0, 10, 100)) == RC_ERR_NOT_FOUND, "空树再 unmap → NOT_FOUND");

    /* ── 3. B 组：分裂级联（根叶 337 满 → 338 裂 → 根内部） ── */
    printf("\n-- B：分裂级联 + 欠载合并/根收缩 --\n");
    const uint64_t N = 340;
    for (uint64_t i = 0; i < N; i++) {
        result_code_t r = fs.extent_map_range(inum, mk_ext(i * 2, 1, 1000 + i));
        if (r != RC_OK) { CHECK(false, "map #%lu → %s", i, rc_name(r)); break; }
    }
    audit_t b1;
    audit_run(fs, inum, "B 340 记录", &b1);
    CHECK(b1.records == N, "B records=%lu got %lu", N, b1.records);
    CHECK(b1.leaves == 2 && b1.inner == 1, "B 根内部 + 2 叶（根叶已裂）leaves=%lu inner=%lu",
          b1.leaves, b1.inner);

    /* 全删：逐条 unmap，触发欠载合并/抹匀/根收缩，回到空根叶 */
    bool all_ok = true;
    for (uint64_t i = 0; i < N; i++) {
        result_code_t r = fs.extent_unmap_range(inum, mk_ext(i * 2, 1, 1000 + i));
        if (r != RC_OK) { CHECK(false, "unmap #%lu → %s", i, rc_name(r)); all_ok = false; break; }
        if ((i + 1) % 100 == 0) audit_run(fs, inum, "B 删除中", nullptr);
    }
    if (all_ok) {
        audit_t b2;
        audit_run(fs, inum, "B 全删后", &b2);
        CHECK(b2.records == 0 && b2.leaves == 1 && b2.inner == 0 && b2.nodes == 1,
              "B 终态：空根叶（收缩链走完）records=%lu leaves=%lu inner=%lu",
              b2.records, b2.leaves, b2.inner);
    }

    /* ── 4. 二次增长：收缩后的树还能继续 map（根收缩后 data_root 一致性） ── */
    printf("\n-- C：收缩后复用（data_root 一致性回归） --\n");
    CHECK(fs.extent_map_range(inum, mk_ext(0, 8, 500)) == RC_OK, "收缩后 map [0,8)p500");
    CHECK(fs.extent_map_range(inum, mk_ext(8, 4, 508)) == RC_OK, "map [8,12)p508 → 左并");
    audit_t c1;
    audit_run(fs, inum, "C", &c1);
    CHECK(c1.records == 1, "C records=1（合并 [0,12)p500）got %lu", c1.records);

    /* ═══════════════════════════════════════════════════════════════
     * D 组：十万级极端增删（单文件压力，打穿内部层）
     *
     * 规模：120000 条 length=1 记录（fblkbase = i*2，间隔 1 → 永不合并，
     *       记录数恒等于插删净值，审计断言干净）。
     *   树形态预期：120000/337 ≈ 356+ 叶 > 338 → 根内部 337 孩满必裂
     *   （打穿内部层：叶裂 → 根内部裂 → depth3 = 新根 + 中间内部 ×2 + 叶）。
     *   对比：337×337 = 113569 条是打穿内部层的最小规模。
     *
     * 路径设计（双向随机，固定种子可复现）：
     *   阶段 1  随机序插 A 空间 120000 条（树从空长到 depth3）
     *   阶段 2  审计（满树：断言已打穿，inner≥2、叶 >338）
     *   阶段 3  随机序删 A 空间 60000 条（欠载平衡在随机叶触发）
     *   阶段 4  审计（半树，含内部层合并/收缩路径）
     *   阶段 5  随机序插 B 空间 60000 条（新空间穿插进既有树）
     *   阶段 6  审计（回到 120000 条）
     *   阶段 7  随机序全删（A 剩 60000 + B 60000）→ 空根叶
     *   阶段 8  终态审计
     *   全程 42 万次原语调用，任一次非预期 RC 即定位报错。
     * ═══════════════════════════════════════════════════════════════ */
    printf("\n-- D：十万级极端增删（单文件 42 万次原语调用，打穿内部层） --\n");
    const uint64_t STRESS_NA = 120000;   /* A 空间条数（>113569，打穿内部层） */
    const uint64_t STRESS_NB = 60000;    /* B 空间条数 */
    const uint64_t B_SPACE_BASE = 400000; /* B 空间 fblkbase 起点（与 A 的 [0,240000) 不重叠） */

    /* 建第二个文件（独立树，顺带验证多文件共存） */
    rc = fs.root_dir_insert_dentry("stress.bin", INODE_TYPE_REGULAR);
    CHECK(rc == RC_OK, "root_dir_insert_dentry(stress.bin) = %s", rc_name(rc));
    uint64_t sinum = 0;
    rc = fs.root_dir_lookup("stress.bin", &sinum);
    CHECK(rc == RC_OK && sinum != 0, "lookup stress.bin → inum=%lu", sinum);
    CHECK(file_to_extent_mode(fs, sinum) != PTR_INVALID, "stress.bin 跃迁建树根");

    std::mt19937 rng(42);   /* 固定种子：确定性可复现 */

    auto stress_map = [&](uint64_t fb) -> result_code_t {
        return fs.extent_map_range(sinum, mk_ext(fb, 1, 0x200000 + fb));
    };
    auto stress_unmap = [&](uint64_t fb) -> result_code_t {
        return fs.extent_unmap_range(sinum, mk_ext(fb, 1, 0x200000 + fb));
    };

    /* 阶段 1：随机序插 A 空间 12 万条 */
    {
        std::vector<uint64_t> order(STRESS_NA);
        for (uint64_t i = 0; i < STRESS_NA; i++) order[i] = i;
        std::shuffle(order.begin(), order.end(), rng);
        auto t0 = std::chrono::steady_clock::now();
        bool ok = true;
        for (uint64_t k = 0; k < STRESS_NA; k++) {
            result_code_t r = stress_map(order[k] * 2);
            if (r != RC_OK) {
                CHECK(false, "阶段1 map #%lu (fb=%lu) → %s", k, order[k] * 2, rc_name(r));
                ok = false; break;
            }
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        CHECK(ok, "阶段1：随机序插入 %lu 条完成 (%ld ms)", STRESS_NA, ms);
    }
    audit_t d1;
    audit_run(fs, sinum, "D 满树", &d1);
    CHECK(d1.records == STRESS_NA, "D 满树 records=%lu got %lu", STRESS_NA, d1.records);
    CHECK(d1.inner >= 2 && d1.leaves > 338,
          "D 满树已打穿内部层：inner=%lu leaves=%lu（期望 inner≥2 且叶>338）",
          d1.inner, d1.leaves);

    /* 阶段 3：随机序删 A 空间 6 万条（剩 6 万） */
    {
        std::vector<uint64_t> order(STRESS_NA);
        for (uint64_t i = 0; i < STRESS_NA; i++) order[i] = i;
        std::shuffle(order.begin(), order.end(), rng);
        auto t0 = std::chrono::steady_clock::now();
        bool ok = true;
        for (uint64_t k = 0; k < STRESS_NA / 2; k++) {
            result_code_t r = stress_unmap(order[k] * 2);
            if (r != RC_OK) {
                CHECK(false, "阶段3 unmap #%lu (fb=%lu) → %s", k, order[k] * 2, rc_name(r));
                ok = false; break;
            }
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        CHECK(ok, "阶段3：随机序删除 %lu 条完成 (%ld ms)", STRESS_NA / 2, ms);
    }
    audit_t d2;
    audit_run(fs, sinum, "D 半树", &d2);
    CHECK(d2.records == STRESS_NA / 2, "D 半树 records=%lu got %lu", STRESS_NA / 2, d2.records);

    /* 阶段 5：随机序插 B 空间 6 万条（穿插进既有树） */
    {
        std::vector<uint64_t> order(STRESS_NB);
        for (uint64_t i = 0; i < STRESS_NB; i++) order[i] = i;
        std::shuffle(order.begin(), order.end(), rng);
        auto t0 = std::chrono::steady_clock::now();
        bool ok = true;
        for (uint64_t k = 0; k < STRESS_NB; k++) {
            result_code_t r = stress_map(B_SPACE_BASE + order[k] * 2);
            if (r != RC_OK) {
                CHECK(false, "阶段5 map #%lu (fb=%lu) → %s", k, B_SPACE_BASE + order[k] * 2, rc_name(r));
                ok = false; break;
            }
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        CHECK(ok, "阶段5：随机序插入 %lu 条（B 空间）完成 (%ld ms)", STRESS_NB, ms);
    }
    audit_t d3;
    audit_run(fs, sinum, "D 回满", &d3);
    CHECK(d3.records == STRESS_NA, "D 回满 records=%lu got %lu", STRESS_NA, d3.records);

    /* 阶段 7：随机序全删（A 剩 6 万在树 + B 6 万） */
    {
        std::vector<uint64_t> to_del;
        to_del.reserve(STRESS_NA + STRESS_NB);
        for (uint64_t i = 0; i < STRESS_NA; i++) to_del.push_back(i * 2);            /* A 全空间 */
        for (uint64_t i = 0; i < STRESS_NB; i++) to_del.push_back(B_SPACE_BASE + i * 2); /* B 全空间 */
        std::shuffle(to_del.begin(), to_del.end(), rng);
        auto t0 = std::chrono::steady_clock::now();
        uint64_t not_found = 0;
        bool ok = true;
        for (uint64_t k = 0; k < to_del.size(); k++) {
            result_code_t r = stress_unmap(to_del[k]);
            if (r == RC_ERR_NOT_FOUND) { not_found++; continue; }   /* A 被阶段3 删掉的那半本不在树 */
            if (r != RC_OK) {
                CHECK(false, "阶段7 unmap #%lu (fb=%lu) → %s", k, to_del[k], rc_name(r));
                ok = false; break;
            }
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        CHECK(ok && not_found == STRESS_NA / 2,
              "阶段7：随机序删除完成，NOT_FOUND(=本不在树)=%lu (期望 %lu) (%ld ms)",
              not_found, STRESS_NA / 2, ms);
    }
    audit_t d4;
    audit_run(fs, sinum, "D 终态", &d4);
    CHECK(d4.records == 0 && d4.leaves == 1 && d4.nodes == 1,
          "D 终态：空根叶 records=%lu leaves=%lu", d4.records, d4.leaves);
    }   /* 非 par 模式：A/B/C/D 串行语义组 */

    printf("\n%d passed, %d failed\n", g_pass.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}

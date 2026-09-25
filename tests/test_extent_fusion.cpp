/* ═══════════════════════════════════════════════════════════════════
 * test_extent_fusion.cpp — 融合策略专项测试（F 组转型基座，2026-09-06 定案）
 *
 * 历史（2026-09-06）：F 组原为 map 自动融合路线探测器，实证内建融合不完备
 * （f1d 跨界左并盲区 / F2 融合致欠载叶不回收 / D/E 组 42 万压测假绿），
 * 促成 7bde35c 降级 + 09-06 融合定案（决策过程稿已完成使命扬）。本文件随定案转型：
 *
 * 新世界观（演进史见 Docs/pwrite_pipeline.md §1/§2）：
 *   1. map 叶内融合四形态完备（2026-09-07 换代，原只左融合推广）—— 同叶
 *      L/R 双判定：纯插 / 左融合（扩 GLB 尾）/ 右融合（并入右邻头）/ 双融合
 *      （L&&R 三合一，count−1 → 非根欠载走 extent_balance_from）。跨界
 *      （entry[0] 前跨前叶 / 叶末越叶）仍刻意跳过 → 可并残留仅存跨叶边界。
 *   2. 全量融合 = 激进重建（extent_rebuild_tree）：收集 → 全并 → 均分打包
 *      → 自下而上建树 → free 旧树 → swap 新根。失败原子性：新树建成前
 *      旧树只读，ENOSPC = 丢新节点旧树无损。
 *   3. 归零回收（2026-09-07 补票）：regular_file_reclaim + destroy_data，
 *      D 组三形态验证。
 *
 * 因此本文件断言分层：
 *   - audit（结构义务）：只查重叠/超满/欠载/父链 key/兄弟链序。可并残留
 *     不再 bad（canonical 承诺废除）；结构层从"查融合"退为"查不变量"。
 *   - 行为断言（融合语义）用 range_sample / window_is_single 记录粒度验证：
 *     F1a 左并扩长 → 单条；F1b 右并（前插右邻融合）→ 单条；F1c 双并桥接
 *     （L&&R 三合一）→ 单条；F1d 跨叶边界缺口 → 残留两条合法；F1e 双并致
 *     欠载（非根 168 叶边界）→ 平衡链 + 根收缩收敛单叶根；F2 乱序 12 万 →
 *     结构绿 + 确有融合 + rebuild 收敛单条；L1 顺序写 N 次记录恒 1（左并
 *     核心承诺）；L2 phys 断点分条正确；L3 GLB=叶末左并 → 父链 key 刷新
 *     正确；R1 rebuild 碎片树 → 单条；R2 rebuild 中途 ENOSPC → 旧树无损。
 *
 * 附带工具（用户调试用）：range_sample() —— 基于 extent_iterator 的区间
 * 取样：构造 extent_iterator(&fs, leaf_blk, glb)（walk 落点起游走），列出
 * 窗口内相交的整条记录。跨界/空叶跳过路径随跑。
 *
 * 用法：ravenfs_juv_extent_fusion <device>            （F + L + R）
 *       ravenfs_juv_extent_fusion <device> par [workers] （并行压测，需大镜像）
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "audit_check.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"

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

static std::atomic<int> g_pass{0}, g_fail{0};
#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

static const char *rc_name(result_code_t rc) {
    switch (rc) {
    case RC_OK: return "RC_OK"; case RC_ERR_OPEN: return "RC_ERR_OPEN";
    case RC_ERR_MAGIC: return "RC_ERR_MAGIC"; case RC_ERR_VERSION: return "RC_ERR_VERSION";
    case RC_ERR_CRC: return "RC_ERR_CRC"; case RC_ERR_BITMAP_SELF: return "RC_ERR_BITMAP_SELF";
    case RC_ERR_EXIST: return "RC_ERR_EXIST"; case RC_ERR_NOT_FOUND: return "RC_ERR_NOT_FOUND";
    case RC_ERR_NO_BLOCK: return "RC_ERR_NO_BLOCK"; case RC_ERR_NO_INODE: return "RC_ERR_NO_INODE";
    case RC_ERR_NAME_LEN: return "RC_ERR_NAME_LEN"; case RC_ERR_IO: return "RC_ERR_IO";
    case RC_ERR_STATE: return "RC_ERR_STATE"; case RC_ERR_NOT_EMPTY: return "RC_ERR_NOT_EMPTY";
    case RC_ERR_NOT_SUPPORTED: return "RC_ERR_NOT_SUPPORTED";
    case RC_ERR_OVERLAP: return "RC_ERR_OVERLAP";
    default: return "RC_???";
    }
}

static linux_userspace_mount_t make_mount(const char *path, uint32_t cpu_count = 1) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_NVME;
    mnt.dev.is_device_direct = false;
    mnt.logical_processor_count = cpu_count;
    return mnt;
}

static extent_t mk_ext(uint64_t fb, uint64_t len, uint64_t phys) {
    extent_t e{};
    e.fblkbase = fb;
    e.length = len;
    e.block_base.ptr = phys;
    e.block_base.free_to_use = 0;
    return e;
}

static uint64_t file_to_extent_mode(RavenFS_juvenile &fs, uint64_t inum) {
    uint64_t root_blk = fs.block_alloc_hard(2, 1);
    if (root_blk == PTR_INVALID) return PTR_INVALID;
    {
        mdata_piece_session s(fs.mdata_piece_mgr, root_blk);
        auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
        memset(s.data, 0, 8192);
        leaf->meta.magics[0] = BPTREE_MAGIC1; leaf->meta.magics[1] = BPTREE_MAGIC2;
        leaf->meta.magics[2] = BPTREE_MAGIC3; leaf->meta.magics[3] = BPTREE_MAGIC4;
        leaf->meta.parent_ptr.entry_count = 0;
        leaf->meta.parent_ptr.is_inner_node = 0;
        leaf->meta.parent_ptr.is_root_node = 1;
        leaf->meta.parent_ptr.ptr = inum;
        leaf->meta.ptrs[0].ptr = PTR_INVALID;
        leaf->meta.ptrs[1].ptr = PTR_INVALID;
        leaf->meta.ptrs[2].ptr = PTR_INVALID;
        s.dirty();
    }
    raven_Inode_t ino;
    fs.inode_read(inum, &ino);
    ino.data_root = root_blk;
    ino.is_data_root_direct_bit = 0;
    fs.inode_write(inum, &ino);
    return root_blk;
}

/* ═══════════════ 树完整性审计（DFS） ═══════════════
 * 结构义务（新世界观，2026-09-06）：
 *   - 记录逻辑不重叠（半开区间）；cnt ∈ [1,337]；非根节点 ∈ [168,337]；
 *   - 父链 key == 子树 max；兄弟链序 == DFS 叶序。
 * 可并残留（同叶相邻 / 跨叶边界）不再 bad —— canonical 承诺已废除，
 * 融合语义由 range_sample 级断言负责（本审计只管结构不变量）。 */
struct audit_t {
    uint64_t nodes = 0, inner = 0, leaves = 0, records = 0, bad = 0;
    std::vector<uint64_t> leaf_blk, leaf_cnt, leaf_end, leaf_phys_end;
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
    if (expect_root) { if (meta->parent_ptr.ptr != inum) a.bad++; }
    else { if (meta->parent_ptr.ptr != expect_parent) a.bad++; }
    a.nodes++;
    const uint16_t cnt = (uint16_t)meta->parent_ptr.entry_count;

    if (!meta->parent_ptr.is_inner_node) {
        a.leaves++;
        auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
        a.records += cnt;
        if (cnt > 337) a.bad++;
        if (!expect_root && (cnt < 168 || cnt == 0)) a.bad++;
        for (uint16_t i = 0; i < cnt; i++) {
            const extent_t e = leaf->extents[i];
            if (i > 0) {
                const extent_t p = leaf->extents[i - 1];
                if (p.fblkbase + p.length > e.fblkbase) a.bad++;   /* 重叠（唯一红线） */
            }
        }
        a.leaf_blk.push_back(blk);
        a.leaf_cnt.push_back(cnt);
        if (cnt > 0) {
            const extent_t last = leaf->extents[cnt - 1];
            a.leaf_end.push_back(last.fblkbase + last.length);
            a.leaf_phys_end.push_back(last.block_base.ptr + last.length);
        } else { a.leaf_end.push_back(0); a.leaf_phys_end.push_back(0); }
        return cnt > 0 ? a.leaf_end.back() : 0;
    }
    a.inner++;
    auto *inner = (Bptree_extent_inner_node_t *)s.data;
    if (cnt == 0) a.bad++;
    if (cnt > 337) a.bad++;
    if (expect_root && cnt == 1) a.bad++;
    if (!expect_root && cnt < 168) a.bad++;
    uint64_t prev_key = 0;
    for (uint16_t i = 0; i < cnt; i++) {
        const extent_hint_t h = inner->extents[i];
        if (h.sub_node_ptr.ptr == 0 || h.sub_node_ptr.ptr == PTR_INVALID) a.bad++;
        if (i > 0 && h.key <= prev_key) a.bad++;
        prev_key = h.key;
        uint64_t child_max = audit_node(fs, a, h.sub_node_ptr.ptr, blk, false, inum, depth + 1);
        if (h.key != child_max) a.bad++;
    }
    return prev_key;
}

/* 兄弟链序 == DFS 叶序（链表完整性；跨叶可并不再 bad） */
static void audit_chain(RavenFS_juvenile &fs, audit_t &a, uint64_t root_blk) {
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
    while (blk != PTR_INVALID_52 && idx < a.leaf_blk.size() + 1) {
        if (idx >= a.leaf_blk.size()) { a.bad++; return; }
        if (blk != a.leaf_blk[idx]) { a.bad++; return; }
        idx++;
        mdata_piece_session s(fs.mdata_piece_mgr, blk);
        if (s.index_error() || !s) { a.bad++; return; }
        Bptree_meta_t *m = (Bptree_meta_t *)s.data;
        if (!m->parent_ptr.is_inner_node) {
            auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
            blk = leaf->meta.ptrs[2].ptr;
        } else { a.bad++; return; }
    }
    if (idx != a.leaf_blk.size()) a.bad++;
}

static void audit_run(RavenFS_juvenile &fs, uint64_t root, uint64_t inum,
                      const char *tag, audit_t *out) {
    audit_t a;
    audit_node(fs, a, root, 0, true, inum, 0);
    if (a.leaves > 0) audit_chain(fs, a, root);
    if (out) *out = a;
    char msg[256];
    snprintf(msg, sizeof(msg),
             "audit[%s]: nodes=%lu inner=%lu leaves=%lu records=%lu bad=%lu",
             tag, a.nodes, a.inner, a.leaves, a.records, a.bad);
    CHECK(a.bad == 0, "%s", msg);
}

/* ═══════════════ 区间取样（基于 extent_iterator，调试工具） ═══════════════
 * 给定 (root, base, len)，列出与 [base, base+len) 相交的整条记录（记录粒度，
 * 不裁剪）。驱动 iterator 跨界/空叶跳过路径。返回 false = 结构错误。
 * extent_iterator 构造 = (&fs, walk 落点 leaf_blk, GLB idx) 起游走。 */
static bool range_sample(RavenFS_juvenile &fs, uint64_t root, uint64_t base,
                         uint64_t len, std::vector<extent_t> &out) {
    out.clear();
    bptree_path_t trace;
    if (fs.extent_Bptree_walk(root, &trace, base) != RC_OK) return false;
    const uint64_t leaf = trace.blocks[trace.depth - 1];
    const uint16_t glb = trace.child_idx[trace.depth - 1];
    const bool hit = trace.is_leaf_hit;
    RavenFS_juvenile::extent_iterator it(&fs, leaf, glb);
    if (it.iterator_erro_flag) return false;

    if (glb == RavenFS_juvenile::EXTENT_LEAF_GLB_NONE) {
        ++it;                        /* 虚拟前位 → 首项 */
        if (it.iterator_erro_flag) return true;   /* 空树：空取样 */
    } else if (!hit) {
        ++it;                        /* GLB 完全在窗口左（end<=base）→ 前进 */
    }
    const uint64_t end = base + len;
    extent_t e;
    while (!it.iterator_erro_flag) {
        if (!it.current_extent(e)) break;
        if (e.fblkbase >= end) break;                 /* 有序：已过窗口右界 */
        if (e.fblkbase + e.length > base) out.push_back(e);  /* 与窗口相交 */
        ++it;
    }
    return true;
}

/* ── 断言窗口 [a,b) 内恰 1 条记录且精确覆盖（融合到位的判定） ── */
static bool window_is_single(RavenFS_juvenile &fs, uint64_t root,
                             uint64_t a, uint64_t b, uint64_t phys_expect,
                             uint64_t *got_cnt) {
    std::vector<extent_t> v;
    if (!range_sample(fs, root, a, b - a, v)) return false;
    if (got_cnt) *got_cnt = v.size();
    if (v.size() != 1) return false;
    return v[0].fblkbase == a && v[0].length == b - a &&
           v[0].block_base.ptr == phys_expect;
}

/* ═══════════════════════════════════════════════════════════════════
 * F1：简单融合矩阵（新世界观断言）
 * 打底模式：N 条 fb=i*2、phys=1000+2i —— 双保险永不自动合并，铺稳定多叶。
 * ═══════════════════════════════════════════════════════════════════ */
static void fusion_base_340(RavenFS_juvenile &fs, uint64_t &root, uint64_t inum) {
    for (uint64_t i = 0; i < 340; i++) {
        result_code_t r = fs.extent_map_range(root, inum, mk_ext(i * 2, 1, 1000 + 2 * i));
        if (r != RC_OK) { CHECK(false, "打底 map #%lu → %s", i, rc_name(r)); return; }
    }
}

static void run_f1(RavenFS_juvenile &fs, uint64_t inum, uint64_t &root) {
    printf("\n-- F1：叶内融合矩阵（四形态） --\n");
    /* 清场：unmap 当前全部记录（三字段全等）→ 空树 */
    auto wipe = [&]() -> bool {
        std::vector<extent_t> v;
        if (!range_sample(fs, root, 0, 1ull << 40, v)) return false;
        for (auto it = v.rbegin(); it != v.rend(); ++it) {
            if (fs.extent_unmap_range_loose(root, inum, *it) != RC_OK) return false;
        }
        return true;
    };

    /* f1a 左并（追加到既有记录尾：紧贴+连续 → 扩长不增记录） */
    CHECK(wipe(), "f1a 清场");
    CHECK(fs.extent_map_range(root, inum, mk_ext(0, 4, 100)) == RC_OK, "f1a map [0,4)p100");
    CHECK(fs.extent_map_range(root, inum, mk_ext(4, 2, 104)) == RC_OK, "f1a map [4,6)p104");
    CHECK(window_is_single(fs, root, 0, 6, 100, nullptr), "f1a 左并 → [0,6)p100 单条");
    {
        audit_t a; audit_run(fs, root, inum, "f1a", &a);
        CHECK(a.records == 1 && a.nodes == 1, "f1a records=1 nodes=1 got %lu/%lu", a.records, a.nodes);
    }

    /* f1b 右并场景（前插）：GLB=NONE 无左并对象，但右邻紧贴 + phys 连续 → 右融合：
     * 段并入右邻头（fblkbase/block_base 同步左移）→ 单条；count 不变 → 无欠载
     * 无分裂零分配；右邻终点未动 → 叶 max 不变 → 父链零刷新。 */
    CHECK(wipe(), "f1b 清场");
    CHECK(fs.extent_map_range(root, inum, mk_ext(10, 3, 200)) == RC_OK, "f1b map [10,13)p200");
    CHECK(fs.extent_map_range(root, inum, mk_ext(6, 4, 196)) == RC_OK, "f1b map [6,10)p196");
    CHECK(window_is_single(fs, root, 6, 13, 196, nullptr), "f1b 右融合 → [6,13)p196 单条");
    {
        audit_t a; audit_run(fs, root, inum, "f1b", &a);
        CHECK(a.records == 1 && a.nodes == 1, "f1b records=1 nodes=1 got %lu/%lu", a.records, a.nodes);
    }

    /* f1c 双并桥接（L&&R 三合一）：B 填洞紧贴 A 尾 && phys 连续，且 B 尾紧贴 C 头
     * && phys 连续 → 双融合：A 扩长吞 B 与 C、memmove 删 C → 单条 [0,13)p100，
     * count−1。根叶无欠载 → 零收尾（叶 max 不变：删末项后新末项终点 = 原末项
     * 终点 → 父链零刷新）。 */
    CHECK(wipe(), "f1c 清场");
    CHECK(fs.extent_map_range(root, inum, mk_ext(0, 4, 100)) == RC_OK, "f1c map A=[0,4)p100");
    CHECK(fs.extent_map_range(root, inum, mk_ext(10, 3, 110)) == RC_OK, "f1c map C=[10,13)p110");
    CHECK(fs.extent_map_range(root, inum, mk_ext(4, 6, 104)) == RC_OK, "f1c map B=[4,10)p104（洞）");
    CHECK(window_is_single(fs, root, 0, 13, 100, nullptr), "f1c 双融合 → [0,13)p100 单条");
    {
        audit_t a; audit_run(fs, root, inum, "f1c", &a);
        CHECK(a.records == 1 && a.nodes == 1 && a.bad == 0,
              "f1c records=1 nodes=1 got %lu/%lu", a.records, a.nodes);
    }

    /* f1d 跨叶边界缺口（叶内完备后唯一残留缝）：340 打底双保险分离 →
     * L1=168 条 / L2=172 条。X=[335,336)p1335 路由落 L2 头（GLB=NONE）→ 无左
     * 并对象但右邻紧贴 + 连续 → 右融合进 L2 首 → [335,337)p1335；与 L1 末
     * [334,335)p1334 跨叶紧贴连续 → 跨界不并 → 窗口 [334,337) 残留 2 条合法。
     * audit 绿 = 右融合改叶首起点不动叶 max → 父 hint 无需刷新。 */
    {
        CHECK(wipe(), "f1d 清场");
        fusion_base_340(fs, root, inum);
        audit_t ad0;
        audit_run(fs, root, inum, "f1d 打底340", &ad0);
        CHECK(ad0.records == 340 && ad0.bad == 0, "f1d 打底 records=340 got %lu", ad0.records);
        CHECK(fs.extent_map_range(root, inum, mk_ext(335, 1, 1335)) == RC_OK, "f1d map X=[335,336)p1335");
        audit_t ad;
        audit_run(fs, root, inum, "f1d 跨叶缺口", &ad);
        CHECK(ad.records == 340 && ad.bad == 0, "f1d records=340（右并不增条）got %lu", ad.records);
        std::vector<extent_t> v;
        range_sample(fs, root, 334, 3, v);
        CHECK(v.size() == 2, "f1d 跨叶残留 2 条合法（got %zu）：[334,335)+[335,337)", v.size());
        bool shape = v.size() == 2 && v[0].fblkbase == 334 && v[0].length == 1 &&
                     v[0].block_base.ptr == 1334 && v[1].fblkbase == 335 &&
                     v[1].length == 2 && v[1].block_base.ptr == 1335;
        CHECK(shape, "f1d 形态 = [334,335)p1334 + [335,337)p1335（X 已右并入 L2 首）");
    }

    /* f1e 双并致欠载 → 平衡链 + 根收缩（map 欠载路径直接验证，2026-09-07）：
     * 打底 A[0,4)p100 + C[10,13)p110 + 双保险分离填充至 338 条 → 第 338 条纯插
     * 触发分裂（左叶 168 = A+C+166 填充 [0,346)，右叶 170，根内 2 孩）→ 左叶
     * 非根 count==168 恰在欠载线上。map B=[4,10)p104（洞）命中 L&&R → 双融合
     * 删 C → 左叶 167 欠载 → extent_balance_from：167+170=337 ≤337 → 合并单叶
     * → 父(根)2→1 → root_shrink → 337 条单叶根（root_blk 引用带出）。audit
     * 绿 + nodes=1 即整链走通（根内 1 孩残留会被 audit 抓 bad）。 */
    {
        CHECK(wipe(), "f1e 清场");
        CHECK(fs.extent_map_range(root, inum, mk_ext(0, 4, 100)) == RC_OK, "f1e map A=[0,4)p100");
        CHECK(fs.extent_map_range(root, inum, mk_ext(10, 3, 110)) == RC_OK, "f1e map C=[10,13)p110");
        bool ok = true;
        for (uint64_t fb = 14; fb < 346 && ok; fb += 2) {      /* 166 条左叶填充 */
            result_code_t r = fs.extent_map_range(root, inum, mk_ext(fb, 1, 10000 + fb));
            if (r != RC_OK) { CHECK(false, "f1e 左叶填充 fb=%lu → %s", fb, rc_name(r)); ok = false; }
        }
        for (uint64_t fb = 346; fb < 686 && ok; fb += 2) {     /* 170 条 → 第 338 条触发分裂 */
            result_code_t r = fs.extent_map_range(root, inum, mk_ext(fb, 1, 10000 + fb));
            if (r != RC_OK) { CHECK(false, "f1e 右叶填充 fb=%lu → %s", fb, rc_name(r)); ok = false; }
        }
        if (ok) {
            audit_t a0; audit_run(fs, root, inum, "f1e 打底338", &a0);
            CHECK(a0.records == 338 && a0.bad == 0 && a0.inner == 1 && a0.leaves == 2,
                  "f1e 打底 records=338 inner=1 leaves=2 got %lu/%lu/%lu",
                  a0.records, a0.inner, a0.leaves);
            CHECK(fs.extent_map_range(root, inum, mk_ext(4, 6, 104)) == RC_OK, "f1e map B=[4,10)p104（桥接）");
            audit_t a; audit_run(fs, root, inum, "f1e 欠载平衡后", &a);
            CHECK(a.bad == 0, "f1e 平衡链后结构绿");
            CHECK(a.records == 337 && a.nodes == 1 && a.leaves == 1,
                  "f1e 双并−1+合并+根收缩 → records=337 nodes=1 got %lu/%lu", a.records, a.nodes);
            CHECK(window_is_single(fs, root, 0, 13, 100, nullptr), "f1e A 扩长吞 C → [0,13)p100 单条");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * F2：BOSS —— 12 万条紧贴 + phys 连续乱序插（叶内四形态世界观）
 * 左并/右并渐进收敛 + 双并桥接 + 分裂/刷新机械在乱序下的稳定性靶：
 * 每窗审计结构绿；终态审计绿 + 记录数 < 总数（确有融合；残留 = 跨叶
 * 边界缺口 + 渐进左并后新产生的叶内紧贴连续对，显式接受）；
 * 终态交 R1 激进重建 → 单条 [0,N)（端到端）。
 * ═══════════════════════════════════════════════════════════════════ */
static void run_f2(RavenFS_juvenile &fs, uint64_t inum, uint64_t &root) {
    printf("\n-- F2：BOSS 12 万条乱序插（左并稳定性） --\n");
    const uint64_t WINDOW = 10000, TOTAL = 120000, PHY = 0x200000;
    std::mt19937 rng(42);
    auto t0 = std::chrono::steady_clock::now();
    bool ok = true;
    for (uint64_t w = 0; w < TOTAL / WINDOW && ok; w++) {
        std::vector<uint64_t> ord(WINDOW);
        for (uint64_t i = 0; i < WINDOW; i++) ord[i] = w * WINDOW + i;
        std::shuffle(ord.begin(), ord.end(), rng);
        for (uint64_t k = 0; k < WINDOW; k++) {
            uint64_t fb = ord[k];
            result_code_t r = fs.extent_map_range(root, inum, mk_ext(fb, 1, PHY + fb));
            if (r != RC_OK) { CHECK(false, "F2 map #%lu (fb=%lu) → %s", w * WINDOW + k, fb, rc_name(r)); ok = false; break; }
        }
        if (!ok) break;
        audit_t aw;
        audit_run(fs, root, inum, "F2 窗口审计", &aw);
        if (aw.bad != 0) { ok = false; break; }
    }
    if (ok) {
        audit_t af;
        audit_run(fs, root, inum, "F2 终态", &af);
        CHECK(af.bad == 0, "F2 终态结构绿");
        printf("  [F2] 终态 records=%lu（12 万条全覆盖 phys 连续，残留 = 跨叶缺口 + 叶内紧贴连续对）\n", af.records);
        CHECK(af.records < TOTAL, "F2 确有融合发生：records=%lu < %lu", af.records, TOTAL);
        std::vector<extent_t> v;
        range_sample(fs, root, 0, TOTAL, v);
        CHECK(v.size() == af.records, "F2 全域取样一致（%zu）", v.size());
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        printf("  [F2] 12 万条乱序插入完成 (%ld ms)\n", ms);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * L：左融合专项
 *   L1 顺序写恒 1 —— 左并核心承诺：occupy 尾巴延展下顺序写记录数恒 1，
 *      不随写次数涨（50000 次 map，每 1000 次断言单条 + 单叶根）。
 *   L2 phys 断点分条 —— 断点后 phys 不连续 → 新记录（记录数 = phys run 数）。
 *   L3 GLB=叶末左并刷链 —— 多叶树中向最右叶叶末追加：叶 max 变 → 父链
 *      key 刷新（audit key==child_max 兜底验证）。
 * ═══════════════════════════════════════════════════════════════════ */
static void run_l(RavenFS_juvenile &fs, uint64_t inum, uint64_t &root) {
    printf("\n-- L：左融合专项 --\n");
    auto wipe = [&]() -> bool {
        std::vector<extent_t> v;
        if (!range_sample(fs, root, 0, 1ull << 40, v)) return false;
        for (auto it = v.rbegin(); it != v.rend(); ++it)
            if (fs.extent_unmap_range_loose(root, inum, *it) != RC_OK) return false;
        return true;
    };

    /* L1：顺序写 50000 块（紧贴 + phys 连续）→ 恒单条 [0,k) + 单叶根 */
    {
        CHECK(wipe(), "L1 清场");
        const uint64_t N = 50000, PHY = 0x300000;
        bool ok = true;
        for (uint64_t i = 0; i < N && ok; i++) {
            result_code_t r = fs.extent_map_range(root, inum, mk_ext(i, 1, PHY + i));
            if (r != RC_OK) { CHECK(false, "L1 map #%lu → %s", i, rc_name(r)); ok = false; break; }
            if (i % 1000 == 999 || i == 0) {
                uint64_t cnt = 0;
                if (!window_is_single(fs, root, 0, i + 1, PHY, &cnt)) {
                    CHECK(false, "L1 写 #%lu 后 [0,%lu) 未保持单条（cnt=%lu）", i, i + 1, cnt);
                    ok = false;
                }
            }
        }
        CHECK(ok, "L1 顺序写 %lu 次：记录恒 1（不随写次数涨）", N);
        audit_t a; audit_run(fs, root, inum, "L1 终态", &a);
        CHECK(a.records == 1 && a.leaves == 1 && a.nodes == 1,
              "L1 终态单条单叶根（records=%lu nodes=%lu）", a.records, a.nodes);
    }

    /* L2：phys 断点分条 —— [0,100) 连续，断 50 块后 [100,150) → 2 条 */
    {
        CHECK(wipe(), "L2 清场");
        const uint64_t PHY = 0x400000;
        CHECK(fs.extent_map_range(root, inum, mk_ext(0, 100, PHY)) == RC_OK, "L2 map [0,100)p0x400000");
        CHECK(fs.extent_map_range(root, inum, mk_ext(100, 50, PHY + 200)) == RC_OK,
              "L2 map [100,150)p0x400000+200（phys 断 50 块）");
        std::vector<extent_t> v;
        range_sample(fs, root, 0, 150, v);
        CHECK(v.size() == 2, "L2 断点分条：2 条（got %zu）", v.size());
        bool shape = v.size() == 2 && v[0].fblkbase == 0 && v[0].length == 100 &&
                     v[1].fblkbase == 100 && v[1].length == 50 &&
                     v[1].block_base.ptr == PHY + 200;
        CHECK(shape, "L2 形态 = [0,100)pPHY + [100,150)pPHY+200（phys run 数=记录数）");
        audit_t a; audit_run(fs, root, inum, "L2", &a);
        CHECK(a.records == 2 && a.bad == 0, "L2 records=2 got %lu", a.records);
    }

    /* L3：340 分离打底（L1=168/L2=172 两叶）→ 向最右叶叶末追加紧贴+连续段：
     * GLB = 叶末项 → 左并扩长 → 叶 max 变 → 父链 key 刷新（audit 兜底）。 */
    {
        CHECK(wipe(), "L3 清场");
        fusion_base_340(fs, root, inum);   /* fb=i*2 双保险分离：340 条 / 2 叶 */
        /* 最右叶末项 = [678,679)p1678；追加 [679,680)p1679 → 左并扩为 [678,680) */
        CHECK(fs.extent_map_range(root, inum, mk_ext(679, 1, 1679)) == RC_OK, "L3 map [679,680)p1679");
        audit_t a; audit_run(fs, root, inum, "L3 叶末左并刷链", &a);
        CHECK(a.bad == 0, "L3 父链 key 一致（叶末左并后 max 上刷）");
        std::vector<extent_t> v;
        range_sample(fs, root, 678, 2, v);
        CHECK(v.size() == 1 && v[0].fblkbase == 678 && v[0].length == 2,
              "L3 叶末左并生效：[678,680) 单条（got %zu）", v.size());
    }
    /* L3 终态：留 340+1 分离碎片树给 R2（ENOSPC 无损验证） */
}

/* ═══════════════════════════════════════════════════════════════════
 * R：激进重建（extent_rebuild_tree）
 *   R1 端到端：F2 乱序碎片终态 → rebuild → 单条 [0,TOTAL) + 结构绿。
 *   R2 失败原子性：占光位图 → rebuild → RC_ERR_NO_BLOCK → 释放 →
 *      旧树 audit 绿 + 记录数不变（多叶碎片态验证）。
 * ═══════════════════════════════════════════════════════════════════ */
static void run_r1(RavenFS_juvenile &fs, uint64_t inum, uint64_t root0) {
    printf("\n-- R1：激进重建（F2 碎片终态 → 单条） --\n");
    uint64_t root = root0;
    std::vector<extent_t> before;
    if (!range_sample(fs, root, 0, 1ull << 40, before)) { CHECK(false, "R1 rebuild 前取样失败"); return; }
    printf("  [R1] rebuild 前 records=%zu\n", before.size());

    const uint64_t old_root = root;
    result_code_t r = fs.extent_rebuild_tree(root, inum);
    CHECK(r == RC_OK, "R1 rebuild → %s", rc_name(r));
    if (r != RC_OK) return;
    CHECK(root != old_root, "R1 根已更替（%lu → %lu）", old_root, root);

    audit_t a;
    audit_run(fs, root, inum, "R1 重建后", &a);
    CHECK(a.bad == 0, "R1 重建树结构绿");
    CHECK(window_is_single(fs, root, 0, 120000, 0x200000, nullptr),
          "R1 全域融成单条 [0,120000)p0x200000");
    CHECK(a.records == 1, "R1 records=1 got %lu", a.records);
}

/* R2：ENOSPC 注入 —— #define private public 直操作位图占光空闲块 */
static void run_r2(RavenFS_juvenile &fs, uint64_t inum, uint64_t root0) {
    printf("\n-- R2：重建失败原子性（ENOSPC 注入） --\n");
    uint64_t root = root0;
    audit_t a0;
    audit_run(fs, root, inum, "R2 前态", &a0);
    std::vector<extent_t> v0;
    range_sample(fs, root, 0, 1ull << 40, v0);
    const size_t rec0 = v0.size();
    const uint64_t old_root = root;

    /* 占光位图（每次 2 块；记录清单供释放） */
    std::vector<uint64_t> held;
    for (int guard = 0; guard < (1 << 26); guard++) {
        uint64_t b = fs.block_alloc_hard(2, 1);
        if (b == PTR_INVALID) break;
        held.push_back(b);
    }
    if (held.empty()) { CHECK(false, "R2 无法占光位图（镜像太空？）"); return; }
    printf("  [R2] 已占光：%zu 次 alloc 后位图耗尽\n", held.size());

    result_code_t r = fs.extent_rebuild_tree(root, inum);
    CHECK(r == RC_ERR_NO_BLOCK, "R2 rebuild 在 ENOSPC 下失败 → %s（期望 RC_ERR_NO_BLOCK）", rc_name(r));
    CHECK(root == old_root, "R2 失败后 root 未动");

    for (uint64_t b : held) fs.block_free(b, 2);   /* 释放占用的 → 环境复原 */

    audit_t a1;
    audit_run(fs, root, inum, "R2 后态", &a1);
    CHECK(a1.bad == 0 && a1.records == rec0 && a1.nodes == a0.nodes && a1.leaves == a0.leaves,
          "R2 旧树无损：bad=%lu records=%lu→%lu nodes=%lu→%lu",
          a1.bad, rec0, a1.records, a0.nodes, a1.nodes);
    std::vector<extent_t> v1;
    range_sample(fs, root, 0, 1ull << 40, v1);
    CHECK(v1.size() == rec0, "R2 记录集完整（%zu）", v1.size());
}

/* ═══════════════════════════════════════════════════════════════════
 * D：销毁模式回收（root_dir_rm_dentry destroy_data=true）
 * 覆盖 regular_file_reclaim 三形态：
 *   - extent：真分配数据块写 0xAB → map → destroy 删除 → 数据块清零+free+
 *     树节点 free + inode 槽 free（零泄露）；
 *   - direct：手造 direct inode（direct_bit + data_root + size>160）→ destroy
 *     删除 → 直指区间清零+free；
 *   - inline：空 inline 文件 destroy 删除 → inode 槽回收。
 * 对照：同构造仅释放（destroy=false）→ 数据块 free 但内容残留（0xAB 可读）。
 * ═══════════════════════════════════════════════════════════════════ */
static void run_destroy(RavenFS_juvenile &fs, const char *nm, uint64_t inum, uint64_t root) {
    printf("\n-- D：销毁/仅释放回收（regular_file_reclaim 三形态） --\n");

    /* ── extent 型：真分配数据写 0xAB ── */
    const uint64_t DB = fs.block_alloc_hard(4, 1);
    CHECK(DB != PTR_INVALID, "D-extent 分配 4 数据块 @%lu", DB);
    uint8_t pat[4096];
    memset(pat, 0xAB, sizeof(pat));
    for (int i = 0; i < 4; i++) fs.write_block(DB + i, pat);
    CHECK(fs.extent_map_range(root, inum, mk_ext(0, 4, DB)) == RC_OK, "D-extent map [0,4)p%lu", DB);
    {
        raven_Inode_t ino;
        fs.inode_read(inum, &ino);
        ino.size = 4096 * 4;   /* extent 形态前提：ftype 判定 size>160（真实 pwrite 维护） */
        fs.inode_write(inum, &ino);
    }
    const uint64_t root_blk = root;   /* 单叶根未分裂 */
    {
        audit_t a; audit_run(fs, root_blk, inum, "D-extent 删前", &a);
        CHECK(a.records == 1 && a.bad == 0, "D-extent 删前 records=1");
    }
    /* 仅释放对照：destroy=false → 数据块 free、内容残留 */
    CHECK(fs.root_dir_rm_dentry(nm, false) == RC_OK, "D-extent rm(false) → RC_OK");
    CHECK(!fs.block_test(DB) && !fs.block_test(DB + 1) && !fs.block_test(DB + 2) && !fs.block_test(DB + 3),
          "D-extent 数据块已 free（仅释放）");
    CHECK(!fs.block_test(root_blk) && !fs.block_test(root_blk + 1), "D-extent 树根节点已 free");
    CHECK(!fs.inode_test(inum), "D-extent inode 槽已回收");
    {
        uint8_t rd[4096];
        fs.read_block(DB, rd);
        CHECK(rd[0] == 0xAB && rd[123] == 0xAB, "D-extent 仅释放：内容残留（0xAB 可读，非销毁）");
    }
    /* 容量恢复验证：free 后能再分配（hint 路由不保证复用同一块，只验证不泄漏） */
    {
        uint64_t reuse = fs.block_alloc_hard(4, 1);
        CHECK(reuse != PTR_INVALID, "D-extent 释放后容量恢复（可再分配 4 块 @%lu）", reuse);
        if (reuse != PTR_INVALID) fs.block_free(reuse, 4);
    }

    /* ── extent 销毁：destroy=true → 数据块清零 ── */
    {
        uint64_t i2 = 0, r2 = 0;
        char nm2[64];
        snprintf(nm2, sizeof(nm2), "%s.x", nm);
        result_code_t rc2 = fs.root_dir_insert_dentry(nm2, INODE_TYPE_REGULAR);
        CHECK(rc2 == RC_OK, "D-extent-destroy 建 %s", nm2);
        fs.root_dir_lookup(nm2, &i2);
        r2 = fs.block_alloc_hard(2, 1);
        CHECK(r2 != PTR_INVALID, "D-extent-destroy 建树根");
        {
            mdata_piece_session s(fs.mdata_piece_mgr, r2);
            auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
            memset(s.data, 0, 8192);
            leaf->meta.magics[0] = BPTREE_MAGIC1; leaf->meta.magics[1] = BPTREE_MAGIC2;
            leaf->meta.magics[2] = BPTREE_MAGIC3; leaf->meta.magics[3] = BPTREE_MAGIC4;
            leaf->meta.parent_ptr.entry_count = 0;
            leaf->meta.parent_ptr.is_inner_node = 0;
            leaf->meta.parent_ptr.is_root_node = 1;
            leaf->meta.parent_ptr.ptr = i2;
            leaf->meta.ptrs[0].ptr = PTR_INVALID;
            leaf->meta.ptrs[1].ptr = PTR_INVALID;
            leaf->meta.ptrs[2].ptr = PTR_INVALID;
            s.dirty();
        }
        raven_Inode_t ino;
        fs.inode_read(i2, &ino);
        ino.data_root = r2;
        ino.size = 4096 * 4;   /* extent 形态前提（同仅释放用例） */
        ino.is_data_root_direct_bit = 0;
        fs.inode_write(i2, &ino);
        const uint64_t DB2 = fs.block_alloc_hard(4, 1);
        CHECK(DB2 != PTR_INVALID, "D-extent-destroy 分配数据块 @%lu", DB2);
        for (int i = 0; i < 4; i++) fs.write_block(DB2 + i, pat);
        CHECK(fs.extent_map_range(r2, i2, mk_ext(0, 4, DB2)) == RC_OK, "D-extent-destroy map");
        CHECK(fs.root_dir_rm_dentry(nm2, true) == RC_OK, "D-extent-destroy rm(true) → RC_OK");
        CHECK(!fs.block_test(DB2) && !fs.block_test(DB2 + 1) && !fs.block_test(DB2 + 2) && !fs.block_test(DB2 + 3),
              "D-extent-destroy 数据块已 free");
        uint8_t rd[4096];
        bool zeroed = true;
        for (int i = 0; i < 4 && zeroed; i++) {
            fs.read_block(DB2 + i, rd);
            for (int b = 0; b < 4096; b++)
                if (rd[b] != 0) { zeroed = false; break; }
        }
        CHECK(zeroed, "D-extent-destroy 数据块已清零（防恢复）");
        CHECK(!fs.block_test(r2) && !fs.block_test(r2 + 1), "D-extent-destroy 树节点已 free");
        CHECK(!fs.inode_test(i2), "D-extent-destroy inode 槽已回收");
    }

    /* ── direct 型销毁：手造 direct inode（direct_bit + data_root + size>160） ── */
    {
        char nm3[64];
        snprintf(nm3, sizeof(nm3), "%s.dr", nm);
        uint64_t i3 = 0;
        CHECK(fs.root_dir_insert_dentry(nm3, INODE_TYPE_REGULAR) == RC_OK, "D-direct 建 %s", nm3);
        fs.root_dir_lookup(nm3, &i3);
        const uint64_t DD = fs.block_alloc_hard(2, 1);   /* 直指 2 块 */
        CHECK(DD != PTR_INVALID, "D-direct 分配直指区 @%lu", DD);
        for (int i = 0; i < 2; i++) fs.write_block(DD + i, pat);
        raven_Inode_t ino;
        fs.inode_read(i3, &ino);
        ino.size = 4096 * 2;
        ino.data_root = DD;
        ino.is_data_root_direct_bit = 1;
        fs.inode_write(i3, &ino);
        CHECK(fs.root_dir_rm_dentry(nm3, true) == RC_OK, "D-direct rm(true) → RC_OK");
        CHECK(!fs.block_test(DD) && !fs.block_test(DD + 1), "D-direct 直指区已 free");
        uint8_t rd[4096];
        bool zeroed = true;
        for (int i = 0; i < 2 && zeroed; i++) {
            fs.read_block(DD + i, rd);
            for (int b = 0; b < 4096; b++)
                if (rd[b] != 0) { zeroed = false; break; }
        }
        CHECK(zeroed, "D-direct 直指区已清零");
        CHECK(!fs.inode_test(i3), "D-direct inode 槽已回收");
    }

    /* ── inline 销毁：空 inline 文件 destroy 删除 → 槽回收 ── */
    {
        char nm4[64];
        snprintf(nm4, sizeof(nm4), "%s.in", nm);
        uint64_t i4 = 0;
        CHECK(fs.root_dir_insert_dentry(nm4, INODE_TYPE_REGULAR) == RC_OK, "D-inline 建 %s", nm4);
        fs.root_dir_lookup(nm4, &i4);
        CHECK(fs.root_dir_rm_dentry(nm4, true) == RC_OK, "D-inline rm(true) → RC_OK");
        CHECK(!fs.inode_test(i4), "D-inline inode 槽已回收");
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * E：map 分裂预算原子性（2026-09-07）
 * 场景：单满叶（337 条）→ 占光位图 → 第 338 条 map（必裂，预算 depth+1=2）→
 * 期望 RC_ERR_NO_BLOCK + 树零改动（audit 逐项一致、root_blk 未动、无孤儿块）
 * → 释放位图 → 同条 map 成功 → 根叶裂 168+170 树结构绿。
 * 预算泄漏由 F2 乱序 12 万间接覆盖（泄漏则中途 ENOSPC）；此处验证失败原子。
 * ═══════════════════════════════════════════════════════════════════ */
static void run_e(RavenFS_juvenile &fs, uint64_t inum, uint64_t &root) {
    printf("\n-- E：map 分裂预算原子性（ENOSPC 注入） --\n");
    auto wipe = [&]() -> bool {
        std::vector<extent_t> v;
        if (!range_sample(fs, root, 0, 1ull << 40, v)) return false;
        for (auto it = v.rbegin(); it != v.rend(); ++it)
            if (fs.extent_unmap_range_loose(root, inum, *it) != RC_OK) return false;
        return true;
    };
    CHECK(wipe(), "E 清场");

    /* 铺满叶：337 条分离记录（fb=i*2 双保险不并）→ 单叶根 count=337（满） */
    for (uint64_t i = 0; i < 337; i++) {
        result_code_t r = fs.extent_map_range(root, inum, mk_ext(i * 2, 1, 1000 + i));
        if (r != RC_OK) { CHECK(false, "E 铺底 #%lu → %s", i, rc_name(r)); return; }
    }
    audit_t a0;
    audit_run(fs, root, inum, "E 满叶前态", &a0);
    CHECK(a0.records == 337 && a0.leaves == 1 && a0.nodes == 1, "E 单满叶 337 条");
    const uint64_t root0 = root;

    /* 占光位图 → 第 338 条 map（满叶必裂，预算 alloc 首击即败） */
    std::vector<uint64_t> held;
    for (int guard = 0; guard < (1 << 26); guard++) {
        uint64_t b = fs.block_alloc_hard(2, 1);
        if (b == PTR_INVALID) break;
        held.push_back(b);
    }
    if (held.empty()) { CHECK(false, "E 无法占光位图"); return; }
    result_code_t r = fs.extent_map_range(root, inum, mk_ext(700, 1, 2000));
    CHECK(r == RC_ERR_NO_BLOCK, "E 满叶 map 在 ENOSPC 下 → %s（期望 RC_ERR_NO_BLOCK）", rc_name(r));
    CHECK(root == root0, "E 失败后 root_blk 未动");

    audit_t a1;
    audit_run(fs, root, inum, "E 失败后态", &a1);
    CHECK(a1.bad == 0 && a1.records == a0.records && a1.leaves == a0.leaves && a1.nodes == a0.nodes,
          "E 树零改动：records=%lu→%lu nodes=%lu→%lu（无孤儿无半树）",
          a0.records, a1.records, a0.nodes, a1.nodes);
    for (uint64_t b : held) fs.block_free(b, 2);   /* 复原 */

    /* 同条 map 重试 → 成功，根叶裂 168+170 → 根内部 + 2 叶 */
    CHECK(fs.extent_map_range(root, inum, mk_ext(700, 1, 2000)) == RC_OK, "E 释放后同条 map → RC_OK");
    audit_t a2;
    audit_run(fs, root, inum, "E 分裂后", &a2);
    CHECK(a2.records == 338 && a2.leaves == 2 && a2.inner == 1 && a2.bad == 0,
          "E 根叶裂：records=%lu leaves=%lu inner=%lu", a2.records, a2.leaves, a2.inner);
    std::vector<extent_t> v;
    range_sample(fs, root, 700, 1, v);
    CHECK(v.size() == 1 && v[0].fblkbase == 700, "E 新记录可查（[700,701)）");
}

/* ═══════════════════════════════════════════════════════════════════
 * F3：并行压测（每 worker 独立文件 × 12 万条乱序；竞争面 = 分配器/缓存/盘）
 * 新世界观断言：全程结构绿 + 成功（融合完备性断言已随 canonical 废除）。
 * 需大镜像（inode ≥ 262144）。
 * ═══════════════════════════════════════════════════════════════════ */
static thread_local uint32_t t_fake_proc_id = 0;
extern "C" uint32_t get_processor_id() { return t_fake_proc_id; }

struct f3_worker_t {
    RavenFS_juvenile *fs = nullptr;
    uint64_t inum = 0, root_blk = 0, seed = 0;
    uint32_t fake_id = 0;
    std::atomic<uint64_t> ops{0};
    std::atomic<int> stage{0};
    bool ok = true;
};

static void f3_worker_run(f3_worker_t *W) {
    t_fake_proc_id = W->fake_id;
    RavenFS_juvenile &fs = *W->fs;
    const uint64_t inum = W->inum;
    uint64_t &root = W->root_blk;
    const uint64_t WINDOW = 10000, TOTAL = 120000;
    const uint64_t PHY = 0x400000ull + (uint64_t)W->fake_id * 0x100000ull;
    std::mt19937 rng(W->seed);
    W->stage = 1;
    for (uint64_t w = 0; w < TOTAL / WINDOW && W->ok; w++) {
        std::vector<uint64_t> ord(WINDOW);
        for (uint64_t i = 0; i < WINDOW; i++) ord[i] = w * WINDOW + i;
        std::shuffle(ord.begin(), ord.end(), rng);
        for (uint64_t k = 0; k < WINDOW; k++) {
            uint64_t fb = ord[k];
            result_code_t r = fs.extent_map_range(root, inum, mk_ext(fb, 1, PHY + fb));
            W->ops.fetch_add(1, std::memory_order_relaxed);
            if (r != RC_OK) { W->ok = false; return; }
        }
        audit_t aw;
        audit_node(fs, aw, root, 0, true, inum, 0);
        if (aw.leaves > 0) audit_chain(fs, aw, root);
        if (aw.bad != 0) { W->ok = false; return; }
    }
    W->stage = 2;
}

static void run_f3(RavenFS_juvenile &fs, uint32_t workers) {
    printf("\n-- F3：并行压测（%u worker × 12 万条乱序，结构绿） --\n", workers);
    std::vector<uint64_t> inums(workers, 0);
    for (uint32_t w = 0; w < workers; w++) {
        char nm[32]; snprintf(nm, sizeof(nm), "f3%u.bin", w);
        result_code_t r = fs.root_dir_insert_dentry(nm, INODE_TYPE_REGULAR);
        if (r != RC_OK) {
            CHECK(false, "建 %s → %s（F3 需 --inode-count≥%u 大镜像）", nm, rc_name(r), workers * 32768u);
            return;
        }
        uint64_t inum = 0;
        fs.root_dir_lookup(nm, &inum);
        uint64_t rb = file_to_extent_mode(fs, inum);
        if (rb == PTR_INVALID) { CHECK(false, "%s 跃迁失败", nm); return; }
        inums[w] = inum;
    }
    std::vector<f3_worker_t> args(workers);
    std::vector<std::thread> ths;
    for (uint32_t w = 0; w < workers; w++) {
        args[w].fs = &fs;
        args[w].inum = inums[w];
        raven_Inode_t ino;
        fs.inode_read(inums[w], &ino);
        args[w].root_blk = ino.data_root;
        args[w].fake_id = w;
        args[w].seed = 0x9E3779B97F4A7C15ull ^ (0x9E3779B1ull * (w + 1));
    }
    auto tw = std::chrono::steady_clock::now();
    for (uint32_t w = 0; w < workers; w++) ths.emplace_back(f3_worker_run, &args[w]);
    for (uint32_t w = 0; w < workers; w++) ths[w].join();
    uint64_t wall = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - tw).count();
    bool all_ok = true;
    for (uint32_t w = 0; w < workers; w++) {
        if (!args[w].ok) all_ok = false;
        printf("  worker %u: %s\n", w, args[w].ok ? "OK" : "FAIL");
    }
    CHECK(all_ok, "F3 全部 worker 结构绿（%lu ms）", wall);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device> [par [workers]]\n", argv[0]); return 2; }
    uint32_t par_workers = 0;
    if (argc >= 3 && strcmp(argv[2], "par") == 0) {
        par_workers = (argc >= 4) ? (uint32_t)strtoul(argv[3], nullptr, 10) : 8;
        if (par_workers == 0 || par_workers > 64) { printf("workers ∈ [1,64]\n"); return 2; }
    }

    uint64_t rc;
    linux_userspace_mount_t mnt = make_mount(argv[1], par_workers ? par_workers : 1);
    RavenFS_juvenile fs(mnt, &rc);
    if (!fs.is_mounted()) { printf("mount failed rc=%s\n", rc_name(rc)); return 1; }

    if (par_workers > 0) {
        run_f3(fs, par_workers);
    } else {
        auto new_file = [&](const char *nm, uint64_t *inum, uint64_t *root) -> bool {
            result_code_t r = fs.root_dir_insert_dentry(nm, INODE_TYPE_REGULAR);
            if (r != RC_OK) { printf("insert %s failed %s\n", nm, rc_name(r)); return false; }
            fs.root_dir_lookup(nm, inum);
            *root = file_to_extent_mode(fs, *inum);
            if (*root == PTR_INVALID) { printf("extentify %s failed\n", nm); return false; }
            return true;
        };
        printf("-- 准备：建文件 + inline→extent 跃迁 --\n");
        uint64_t inum = 0, root = 0;
        if (!new_file("f.bin", &inum, &root)) return 1;
        run_f1(fs, inum, root);

        uint64_t i2 = 0, r2 = 0;
        if (!new_file("f2.bin", &i2, &r2)) return 1;
        run_f2(fs, i2, r2);
        run_r1(fs, i2, r2);          /* R1：F2 碎片终态 rebuild → 单条 */

        uint64_t i3 = 0, r3 = 0;
        if (!new_file("f3.bin", &i3, &r3)) return 1;
        run_l(fs, i3, r3);   /* L 组；终态留 340+1 分离碎片树 */
        run_r2(fs, i3, r3);  /* R2：ENOSPC 注入无损验证（用 L3 碎片态） */

        uint64_t i4 = 0, r4 = 0;
        if (!new_file("d.bin", &i4, &r4)) return 1;
        run_destroy(fs, "d.bin", i4, r4);   /* D：销毁/仅释放回收三形态 */

        uint64_t i5 = 0, r5 = 0;
        if (!new_file("e.bin", &i5, &r5)) return 1;
        run_e(fs, i5, r5);   /* E：map 分裂预算原子性 */
    }
    (void)audit_clean(fs, "extent-fusion-final");
    printf("\n%d passed, %d failed\n", g_pass.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}

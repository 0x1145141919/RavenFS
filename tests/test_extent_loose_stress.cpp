/* ═══════════════════════════════════════════════════════════════════
 * test_extent_loose_stress.cpp — extent_unmap_range_loose 大型压测
 *
 * 2026-09-07 设计（Sparrow 提案 + Raven 落定）：
 *   打底 = 12 万条两两不相邻 extent（宽 4 规整：fb=5k、phys=PHY0+5k，
 *   间隙 1 洞 → 永不左并永不 merge，每条独立记录）。
 *   12 万条按 k 划 4 批（各 3 万 = 一分支一批）：
 *     F 批 k∈[0,30000)   full  整删       H 批 k∈[30000,60000)  head  挖头
 *     T 批 k∈[60000,90000) tail 挖尾       M 批 k∈[90000,120000) mid  中裂
 *   每批前 4096 条 = A 规整组（公式化 loose，树形态可预期 → 精确断言）；
 *   每批剩余 25904 条 = B 混沌组（批内 shuffle 乱序 + Δ/m 随机）。
 *   批间记录不相交 → 任意执行顺序合法，无"原料耗尽/死循环"面。
 *
 * 阶段：
 *   S0  map 12 万（顺序，尾叶周期裂）→ audit + 模型同步
 *   S1  A 规整组（4096×4，逐组预期断言 + 抽样三字段 + 全树差分）
 *   S2  B 混沌组（103616 条乱序，分支内 Δ/m 随机；每 ~3 万次全树差分）
 *   S3  乱序全删收尾（shuffle 模型 key 整删 → 空根叶收敛，
 *        欠载平衡 + 根收缩链在 loose 全路径终态验证）
 *   S4  NO_BLOCK 注入（独立 b.bin 满 337 叶 + 抢光块分配 → mid 预算
 *        失败零改动 → 归还 → 重试成功）：拆两端预算失败面闭环
 *
 * 验证支柱 = 账本差分：std::map<fb,(len,phys)> 镜像与每次操作同步；
 * 全树收集（DFS，含 audit 键序/父子/hint key==子树最大开边界/欠载校验）
 * vs 镜像逐条比对——混沌不可预期，但差分让每步可验证。
 *
 * 用法：ravenfs_juv_extent_loose_stress <device>
 * 盘要求：mkfs --inode-count≥70000（256M 以上；树节点峰值 ~1500 块）
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
#include <map>
#include <algorithm>
#include <random>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

/* ── 规模常量 ── */
static constexpr uint64_t NALL   = 120000;   /* 打底记录数 */
static constexpr uint64_t NPER   = 30000;    /* 每批（分支）3 万 */
static constexpr uint64_t A_SIZE = 4096;     /* A 规整组每批条数 */
static constexpr uint64_t PHY0   = 0x1000000ULL; /* 假性 phys 基址 */

/* ── 基础 helpers（同 test_extent_ops.cpp 手法）── */
static linux_userspace_mount_t make_mount(const char *path) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_NVME;
    mnt.dev.is_device_direct = false;
    mnt.logical_processor_count = 1;
    return mnt;
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

static const char *rc_name(result_code_t rc) {
    switch (rc) {
    case RC_OK:             return "RC_OK";
    case RC_ERR_EXIST:      return "RC_ERR_EXIST";
    case RC_ERR_NOT_FOUND:  return "RC_ERR_NOT_FOUND";
    case RC_ERR_NO_BLOCK:   return "RC_ERR_NO_BLOCK";
    case RC_ERR_IO:         return "RC_ERR_IO";
    case RC_ERR_STATE:      return "RC_ERR_STATE";
    case RC_ERR_OVERLAP:    return "RC_ERR_OVERLAP";
    default:                return "RC_???";
    }
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
        leaf->meta.magics[0] = BPTREE_MAGIC1;
        leaf->meta.magics[1] = BPTREE_MAGIC2;
        leaf->meta.magics[2] = BPTREE_MAGIC3;
        leaf->meta.magics[3] = BPTREE_MAGIC4;
        leaf->meta.parent_ptr.entry_count   = 0;
        leaf->meta.parent_ptr.is_inner_node = 0;
        leaf->meta.parent_ptr.is_root_node  = 1;
        leaf->meta.parent_ptr.ptr           = inum;
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

/* 读覆盖 key 的叶记录 */
static bool leaf_rec_at(RavenFS_juvenile &fs, uint64_t root, uint64_t key, extent_t &out) {
    bptree_path_t tr;
    if (fs.extent_Bptree_walk(root, &tr, key) != RC_OK) return false;
    if (!tr.is_leaf_hit) return false;
    mdata_piece_session s(fs.mdata_piece_mgr, tr.blocks[tr.depth - 1]);
    if (s.index_error() || !s) return false;
    auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
    out = leaf->extents[tr.child_idx[tr.depth - 1]];
    return true;
}

/* ── 全树收集 + audit（DFS：键序/父子/hint key==子树最大开边界/欠载）──
 * 收集结果（叶序 = 逻辑序递增）供账本差分。返回 bad 计数。 */
struct audit_t {
    uint64_t nodes = 0, inner = 0, leaves = 0;
    uint64_t bad = 0;
    std::vector<extent_t> recs;
};

static uint64_t audit_collect(RavenFS_juvenile &fs, audit_t &a, uint64_t blk,
                              uint64_t expect_parent, bool expect_root,
                              uint64_t inum, uint64_t depth) {
    if (depth > 64) { a.bad++; return 0; }
    mdata_piece_session s(fs.mdata_piece_mgr, blk);
    if (s.index_error() || !s) { a.bad++; return 0; }
    Bptree_meta_t *meta = (Bptree_meta_t *)s.data;
    if (!is_extent_node(meta)) { a.bad++; return 0; }
    if (meta->parent_ptr.is_root_node != expect_root) { a.bad++; return 0; }
    if (expect_root) { if (meta->parent_ptr.ptr != inum) a.bad++; }
    else             { if (meta->parent_ptr.ptr != expect_parent) a.bad++; }
    a.nodes++;
    const uint16_t cnt = (uint16_t)meta->parent_ptr.entry_count;

    if (!meta->parent_ptr.is_inner_node) {
        a.leaves++;
        auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
        if (cnt > 337) a.bad++;
        if (!expect_root && (cnt < 168 || cnt == 0)) a.bad++;
        for (uint16_t i = 0; i < cnt; i++) {
            const extent_t e = leaf->extents[i];
            if (i > 0) {
                const extent_t p = leaf->extents[i - 1];
                if (p.fblkbase + p.length > e.fblkbase) a.bad++;
            }
            a.recs.push_back(e);
        }
        return cnt > 0 ? leaf->extents[cnt - 1].fblkbase + leaf->extents[cnt - 1].length : 0;
    }

    a.inner++;
    auto *inner = (Bptree_extent_inner_node_t *)s.data;
    if (cnt == 0 || cnt > 337) a.bad++;
    if (expect_root && cnt == 1) a.bad++;
    if (!expect_root && cnt < 168) a.bad++;
    uint64_t prev_key = 0;
    for (uint16_t i = 0; i < cnt; i++) {
        const extent_hint_t h = inner->extents[i];
        if (h.sub_node_ptr.ptr == 0 || h.sub_node_ptr.ptr == PTR_INVALID) a.bad++;
        if (i > 0 && h.key <= prev_key) a.bad++;
        prev_key = h.key;
        uint64_t child_max = audit_collect(fs, a, h.sub_node_ptr.ptr, blk, false, inum, depth + 1);
        if (h.key != child_max) a.bad++;
    }
    return prev_key;
}

/* ── 账本模型（与每次 loose/map 同步的镜像真相）── */
struct model_t {
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> m;   /* fb → (len, phys) */
    void put(uint64_t fb, uint64_t len, uint64_t phys) { m[fb] = {len, phys}; }
    void del(uint64_t fb) { m.erase(fb); }
    uint64_t size() const { return (uint64_t)m.size(); }
    /* 差分：全树收集 vs 模型（数量 + 逐条三字段） */
    bool diff(RavenFS_juvenile &fs, uint64_t root, uint64_t inum, const char *tag,
              uint64_t &tree_records) {
        audit_t a;
        audit_collect(fs, a, root, 0, true, inum, 0);
        tree_records = (uint64_t)a.recs.size();
        bool ok = (a.bad == 0);
        if (!ok) printf("    diff[%s] audit bad=%lu\n", tag, a.bad);
        if (a.recs.size() != m.size()) {
            ok = false;
            printf("    diff[%s] records 树=%lu 模型=%lu\n", tag, a.recs.size(), m.size());
        }
        size_t mism = 0;
        auto it = m.begin();
        for (size_t i = 0; i < a.recs.size() && it != m.end(); i++, ++it) {
            const extent_t &e = a.recs[i];
            if (e.fblkbase != it->first || e.length != it->second.first ||
                e.block_base.ptr != it->second.second) {
                if (mism < 5)
                    printf("    diff[%s] #%zu 树{%lu,%lu,p%lu} 模型{%lu,%lu,p%lu}\n",
                           tag, i, e.fblkbase, e.length, e.block_base.ptr,
                           it->first, it->second.first, it->second.second);
                mism++;
                ok = false;
            }
        }
        if (mism) printf("    diff[%s] 失配 %zu 条\n", tag, mism);
        CHECK(ok, "diff[%s]: 树==模型（records=%lu, nodes=%lu inner=%lu leaves=%lu）",
              tag, tree_records, a.nodes, a.inner, a.leaves);
        return ok;
    }
};

/* 全树记录计数 + audit bad（轻量，不收集） */
static uint64_t tree_count(RavenFS_juvenile &fs, uint64_t root, uint64_t inum, const char *tag) {
    audit_t a;
    audit_collect(fs, a, root, 0, true, inum, 0);
    CHECK(a.bad == 0, "audit[%s]: records=%lu bad=%lu", tag, a.recs.size(), a.bad);
    return (uint64_t)a.recs.size();
}

/* 四批 k 段（k ∈ [0,120000)，fb = 5k，宽 4） */
static bool k_in_F(uint64_t k) { return k < 30000; }
static bool k_in_H(uint64_t k) { return k >= 30000 && k < 60000; }
static bool k_in_T(uint64_t k) { return k >= 60000 && k < 90000; }
static bool k_in_M(uint64_t k) { return k >= 90000; }

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device>\n", argv[0]); return 2; }
    uint64_t rc;
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    RavenFS_juvenile fs(mnt, &rc);
    if (!fs.is_mounted()) { printf("mount failed rc=%s\n", rc_name(rc)); return 1; }

    using clk = std::chrono::steady_clock;
    auto ms_el = [&](clk::time_point t0) {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   clk::now() - t0).count();
    };
    std::mt19937_64 rng(0x5EEDC0DEull);

    printf("-- S0：root_dir 建文件 + map 12 万宽 4 规整 extent --\n");
    rc = fs.root_dir_insert_dentry("a.bin", INODE_TYPE_REGULAR);
    CHECK(rc == RC_OK, "root_dir_insert_dentry(a.bin) = %s", rc_name(rc));
    uint64_t inum = 0;
    fs.root_dir_lookup("a.bin", &inum);
    uint64_t root_blk = file_to_extent_mode(fs, inum);
    CHECK(root_blk != PTR_INVALID, "inline→extent 跃迁 blk=%lu", root_blk);

    model_t model;
    auto t0 = clk::now();
    bool s0_ok = true;
    for (uint64_t k = 0; k < NALL; k++) {
        if (fs.extent_map_range(root_blk, inum, mk_ext(5 * k, 4, PHY0 + 5 * k)) != RC_OK) {
            s0_ok = false;
            printf("    S0 map 失败 k=%lu\n", k);
            break;
        }
        model.put(5 * k, 4, PHY0 + 5 * k);
    }
    CHECK(s0_ok, "S0 map 12 万完成（%lu ms）", ms_el(t0));
    uint64_t dummy = 0;
    model.diff(fs, root_blk, inum, "S0 全树", dummy);
    CHECK(dummy == NALL, "S0 records=%lu（期望 120000）", dummy);

    printf("\n-- S1：A 规整组（每批前 4096 条，公式化 loose，形态可预期） --\n");
    t0 = clk::now();
    /* F 批规整：整删 k∈[0,4096) */
    bool s1_ok = true;
    for (uint64_t k = 0; k < A_SIZE; k++) {
        if (fs.extent_unmap_range_loose(root_blk, inum, mk_ext(5 * k, 4, PHY0 + 5 * k)) != RC_OK) { s1_ok = false; break; }
        model.del(5 * k);
    }
    CHECK(s1_ok, "S1-F 规整 full 4096 条");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(0, 1, PHY0)) == RC_ERR_NOT_FOUND,
          "S1-F 抽样 k=0 fb=0 → NOT_FOUND（已整删）");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(5 * 2048, 1, PHY0 + 5 * 2048)) == RC_ERR_NOT_FOUND,
          "S1-F 抽样 k=2048 → NOT_FOUND");

    /* H 批规整：挖头 2 块 → 剩 {5k+2, 2, phys+2} */
    s1_ok = true;
    for (uint64_t k = 30000; k < 30000 + A_SIZE; k++) {
        if (fs.extent_unmap_range_loose(root_blk, inum, mk_ext(5 * k, 2, PHY0 + 5 * k)) != RC_OK) { s1_ok = false; break; }
        model.del(5 * k);                        /* head 挖头：原 fb 变洞，条目迁到 fb+2 */
        model.put(5 * k + 2, 2, PHY0 + 5 * k + 2);
    }
    CHECK(s1_ok, "S1-H 规整 head 4096 条（挖头 2）");
    extent_t rec;
    CHECK(leaf_rec_at(fs, root_blk, 5 * 30001 + 3, rec) && rec.fblkbase == 5 * 30001 + 2 &&
          rec.length == 2 && rec.block_base.ptr == PHY0 + 5 * 30001 + 2,
          "S1-H 抽样 k=30001 剩 {%lu,%lu,p%lu}", rec.fblkbase, rec.length, rec.block_base.ptr);

    /* T 批规整：挖尾 2 块 → 剩 {5k, 2, phys} */
    s1_ok = true;
    for (uint64_t k = 60000; k < 60000 + A_SIZE; k++) {
        if (fs.extent_unmap_range_loose(root_blk, inum, mk_ext(5 * k + 2, 2, PHY0 + 5 * k + 2)) != RC_OK) { s1_ok = false; break; }
        model.put(5 * k, 2, PHY0 + 5 * k);
    }
    CHECK(s1_ok, "S1-T 规整 tail 4096 条（挖尾 2）");
    CHECK(leaf_rec_at(fs, root_blk, 5 * 60002, rec) && rec.fblkbase == 5 * 60002 &&
          rec.length == 2 && rec.block_base.ptr == PHY0 + 5 * 60002,
          "S1-T 抽样 k=60002 剩 {%lu,%lu,p%lu}", rec.fblkbase, rec.length, rec.block_base.ptr);

    /* M 批规整：宽 4 挖中 2 → 拆 {5k,1,phys} + {5k+3,1,phys+3} */
    s1_ok = true;
    for (uint64_t k = 90000; k < 90000 + A_SIZE; k++) {
        if (fs.extent_unmap_range_loose(root_blk, inum, mk_ext(5 * k + 1, 2, PHY0 + 5 * k + 1)) != RC_OK) { s1_ok = false; break; }
        model.put(5 * k, 1, PHY0 + 5 * k);
        model.put(5 * k + 3, 1, PHY0 + 5 * k + 3);
    }
    CHECK(s1_ok, "S1-M 规整 mid 4096 条（宽 4 挖中 2 → 1 拆 2）");
    CHECK(leaf_rec_at(fs, root_blk, 5 * 90001, rec) && rec.fblkbase == 5 * 90001 &&
          rec.length == 1 && rec.block_base.ptr == PHY0 + 5 * 90001,
          "S1-M 抽样 k=90001 左段 {%lu,%lu,p%lu}", rec.fblkbase, rec.length, rec.block_base.ptr);
    CHECK(leaf_rec_at(fs, root_blk, 5 * 90001 + 3, rec) && rec.fblkbase == 5 * 90001 + 3 &&
          rec.length == 1 && rec.block_base.ptr == PHY0 + 5 * 90001 + 3,
          "S1-M 抽样 k=90001 右段 {%lu,%lu,p%lu}", rec.fblkbase, rec.length, rec.block_base.ptr);
    model.diff(fs, root_blk, inum, "S1 规整组后", dummy);
    CHECK(dummy == NALL - A_SIZE + A_SIZE,   /* full -4096、mid +4096 → 对冲不变 */
          "S1 后 records=%lu（期望仍 120000）", dummy);

    printf("\n-- S2：B 混沌组（每批剩余 25904 条，shuffle 乱序 + Δ/m 随机） --\n");
    /* 构造操作序列：每条剩余记录恰好一次，子区间形状随机（len 恒 4：批间不相交）
     * rfb = 记录起点（模型 key）；fb = 挖的起点（head/tail/mid 各不相同） */
    struct op_t { uint64_t rfb, fb, len, phys; };
    std::vector<op_t> ops;
    ops.reserve(NALL - 4 * A_SIZE);
    std::uniform_int_distribution<uint64_t> d_head(1, 3);      /* Δ ∈ [1,3]（宽 4 挖头） */
    for (uint64_t k = A_SIZE; k < NPER; k++) {                 /* F 批剩余 → full */
        ops.push_back({5 * k, 5 * k, 4, PHY0 + 5 * k});
    }
    for (uint64_t k = NPER + A_SIZE; k < 2 * NPER; k++) {      /* H 批剩余 → head 挖头 Δ */
        uint64_t d = d_head(rng);
        ops.push_back({5 * k, 5 * k, d, PHY0 + 5 * k});
    }
    for (uint64_t k = 2 * NPER + A_SIZE; k < 3 * NPER; k++) {  /* T 批剩余 → tail 挖尾 Δ */
        uint64_t d = d_head(rng);
        ops.push_back({5 * k, 5 * k + 4 - d, d, PHY0 + 5 * k + 4 - d});
    }
    std::uniform_int_distribution<uint64_t> d_mid_m(1, 2);     /* m ∈ [1,2]（宽 4 挖中） */
    std::uniform_int_distribution<uint64_t> d_mid_d(1, 2);
    for (uint64_t k = 3 * NPER + A_SIZE; k < 4 * NPER; k++) {  /* M 批剩余 → mid */
        uint64_t m = d_mid_m(rng);
        uint64_t d = d_mid_d(rng);
        if (m + d > 3) d = 4 - m - 1;                          /* 尾段 ≥1 */
        ops.push_back({5 * k, 5 * k + m, d, PHY0 + 5 * k + m});
    }
    std::shuffle(ops.begin(), ops.end(), rng);                 /* p3：乱序执行 */
    CHECK(ops.size() == NPER * 4 - 4 * A_SIZE,
          "S2 操作数=%lu（期望 %lu）", ops.size(), NPER * 4 - 4 * A_SIZE);

    t0 = clk::now();
    bool s2_ok = true;
    uint64_t bad_k = 0;
    uint64_t s2_full = 0, s2_head = 0, s2_tail = 0, s2_mid = 0;
    uint64_t s2_checked = 0;
    for (uint64_t i = 0; i < ops.size(); i++) {
        const op_t &o = ops[i];
        /* 模型同步预测（执行前按模型现形态计算）——批内记录未被其他批碰过，形态恒已知 */
        auto it = model.m.find(o.rfb);
        if (it == model.m.end()) { s2_ok = false; bad_k = i; break; }   /* 模型不一致：真 bug */
        const uint64_t mlen = it->second.first;
        const uint64_t mphys = it->second.second;
        result_code_t r = fs.extent_unmap_range_loose(root_blk, inum, mk_ext(o.fb, o.len, o.phys));
        if (r != RC_OK) { s2_ok = false; bad_k = i; printf("    S2 #%lu loose rc=%s\n", i, rc_name(r)); break; }
        /* 模型更新（分支判定：按挖的起点相对记录起点） */
        if (o.len == mlen) {                 /* full：整删 */
            model.del(o.rfb); s2_full++;
        } else if (o.fb == it->first) {      /* head：挖头 → 条目迁到 fb+Δ */
            model.del(it->first);
            model.put(o.fb + o.len, mlen - o.len, mphys + o.len); s2_head++;
        } else if (o.fb + o.len == it->first + mlen) {  /* tail：挖尾 */
            model.put(it->first, mlen - o.len, mphys); s2_tail++;
        } else {                             /* mid：1 拆 2 */
            model.put(it->first, o.fb - it->first, mphys);
            model.put(o.fb + o.len, it->first + mlen - (o.fb + o.len),
                      mphys + (o.fb - it->first) + o.len);
            s2_mid++;
        }
        if ((i + 1) % 30000 == 0) {
            model.diff(fs, root_blk, inum, "S2 中段", dummy);
            s2_checked++;
        }
    }
    CHECK(s2_ok, "S2 混沌 loose 全部 RC_OK（%lu 次，bad@#%lu）", ops.size(), bad_k);
    model.diff(fs, root_blk, inum, "S2 完成", dummy);
    CHECK(dummy == NALL, "S2 后 records=%lu（full -3万 / mid +3万 对冲 → 仍 120000）", dummy);
    CHECK(s2_full + s2_head + s2_tail + s2_mid == ops.size() && s2_mid == NPER - A_SIZE,
          "S2 分支实况：full=%lu head=%lu tail=%lu mid=%lu（各应 25904）",
          s2_full, s2_head, s2_tail, s2_mid);
    printf("    S2 耗时 %lu ms（含 %lu 次全树差分）\n", ms_el(t0), s2_checked + 1);

    printf("\n-- S3：乱序全删收尾（模型 key shuffle → 整删 → 空根叶） --\n");
    t0 = clk::now();
    std::vector<uint64_t> keys;
    keys.reserve(model.m.size());
    for (auto &kv : model.m) keys.push_back(kv.first);
    std::shuffle(keys.begin(), keys.end(), rng);
    bool s3_ok = true;
    uint64_t s3_bad = 0;
    for (uint64_t i = 0; i < keys.size(); i++) {
        auto it = model.m.find(keys[i]);
        result_code_t r = fs.extent_unmap_range_loose(root_blk, inum,
                            mk_ext(it->first, it->second.first, it->second.second));
        if (r != RC_OK) { s3_ok = false; s3_bad = i; printf("    S3 #%lu rc=%s\n", i, rc_name(r)); break; }
        model.del(keys[i]);
        if ((i + 1) % 30000 == 0) model.diff(fs, root_blk, inum, "S3 中段", dummy);
    }
    CHECK(s3_ok, "S3 乱序全删 %lu 条 RC_OK（bad@#%lu，%lu ms）", keys.size(), s3_bad, ms_el(t0));
    model.diff(fs, root_blk, inum, "S3 终态", dummy);
    CHECK(dummy == 0, "S3 树空 records=0");
    {
        audit_t a;
        audit_collect(fs, a, root_blk, 0, true, inum, 0);
        CHECK(a.leaves == 1 && a.inner == 0 && a.recs.empty(),
              "S3 收敛空根叶（leaves=%lu inner=%lu）", a.leaves, a.inner);
    }

    printf("\n-- S4：NO_BLOCK 注入（b.bin 满 337 叶 + 抢光块 → mid 预算失败零改动） --\n");
    rc = fs.root_dir_insert_dentry("b.bin", INODE_TYPE_REGULAR);
    CHECK(rc == RC_OK, "S4 root_dir_insert_dentry(b.bin) = %s", rc_name(rc));
    uint64_t inum_b = 0;
    fs.root_dir_lookup("b.bin", &inum_b);
    uint64_t root_b = file_to_extent_mode(fs, inum_b);
    CHECK(root_b != PTR_INVALID, "S4 b.bin 跃迁 blk=%lu", root_b);
    CHECK(fs.extent_map_range(root_b, inum_b, mk_ext(0, 40, 2000)) == RC_OK, "S4 map 长记录 [0,40)p2000");
    bool s4_ok = true;
    for (uint64_t k = 0; k < 336; k++) {
        if (fs.extent_map_range(root_b, inum_b, mk_ext(40 + 2 * k, 1, 3000 + k)) != RC_OK) { s4_ok = false; break; }
    }
    CHECK(s4_ok, "S4 336 短记录 → 满 337 叶");
    CHECK(tree_count(fs, root_b, inum_b, "S4 满叶") == 337, "S4 records=337");

    /* 抢光剩余块（2 块一组，树节点粒度） */
    std::vector<uint64_t> grabbed;
    while (true) {
        uint64_t b = fs.block_alloc_hard(2, 1);
        if (b == PTR_INVALID) break;
        grabbed.push_back(b);
        if (grabbed.size() > 100000) { printf("    S4 抢光异常（>10 万块）\n"); break; }
    }
    CHECK(grabbed.size() > 0, "S4 抢光 %lu 组块", grabbed.size());
    CHECK(fs.extent_unmap_range_loose(root_b, inum_b, mk_ext(20, 10, 2020)) == RC_ERR_NO_BLOCK,
          "S4 满叶 mid 预算 alloc 失败 → NO_BLOCK");
    CHECK(leaf_rec_at(fs, root_b, 10, rec) && rec.fblkbase == 0 && rec.length == 40 &&
          rec.block_base.ptr == 2000,
          "S4 失败零改动：长记录 [0,40)p2000 原样 {%lu,%lu,p%lu}",
          rec.fblkbase, rec.length, rec.block_base.ptr);
    CHECK(tree_count(fs, root_b, inum_b, "S4 失败后") == 337, "S4 records 仍 337");
    for (uint64_t b : grabbed) fs.block_free(b, 2);
    CHECK(fs.extent_unmap_range_loose(root_b, inum_b, mk_ext(20, 10, 2020)) == RC_OK,
          "S4 归还后重试 mid → RC_OK（预算恢复）");
    CHECK(tree_count(fs, root_b, inum_b, "S4 重试后") == 338, "S4 重试后 records=338（1 拆 2）");

    printf("\n════════════════════════════════════════════════════\n");
    (void)audit_clean(fs, "extent-loose-stress-final");
    printf(" SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    printf("════════════════════════════════════════════════════\n");
    return g_fail ? 1 : 0;
}

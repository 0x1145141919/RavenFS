/* ═══════════════════════════════════════════════════════════════════
 * test_extent_unmap_loose.cpp — extent_unmap_range_loose 子集删除原语专项
 *
 * 2026-09-07 发烟测试（分支正确性小测；大型压测设计另行探讨）：
 *   A 契约防御：空区间 RC_OK / 起点空洞 NOT_FOUND / 终点越出记录 STATE /
 *               phys 偏移自洽失败 STATE / 失败零副作用
 *   B 缩水两分支 + full：head 挖头（fblkbase+phys 同步右移）/ tail 挖尾
 *     （剩头段）/ full 整删交 unmap_range；挖后可回填（洞语义正确）
 *   C mid 拆两端：1 拆 2 + 记录内容与 phys 线性映射正确 + 可回填左并
 *   D phase2 左并回滚模拟：map 左并 → loose 裁尾段 → 恢复旧记录原形
 *   E 满叶 mid 分裂级联：337 满叶中间挖洞 → 预算 + 拆两端 → 根叶裂
 *     （烟测预算池 + extent_split_cascade 共用内核路径）
 *   全程 audit_run：键序/父子指针/hint key==子树最大开边界/兄弟链——
 *   缩水与拆两端的"何时刷父链"论证由 audit 的 hint key 校验兜底。
 *
 * 用法：ravenfs_juv_extent_unmap_loose <device>
 * 盘要求：mkfs --inode-count≥70000（root_dir 正门建文件用）
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session 完整类型 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

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
    e.block_base.ptr = phys;       /* 假性 phys：纯记录，不落实分配 */
    e.block_base.free_to_use = 0;
    return e;
}

/* ── inline → extent-tree 跃迁（同 test_extent_ops.cpp 手法）── */
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

/* 读覆盖 key 的叶记录（命中返回 true；供断言缩水/拆两端后的记录形态） */
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

/* ── 树完整性审计（DFS：键序/父子/hint key/max/兄弟链，同 ops 测试）── */
struct audit_t {
    uint64_t nodes = 0, inner = 0, leaves = 0, records = 0;
    uint64_t bad = 0;
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
        if (meta->parent_ptr.ptr != inum) a.bad++;
    } else {
        if (meta->parent_ptr.ptr != expect_parent) a.bad++;
    }
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
                if (p.fblkbase + p.length > e.fblkbase) a.bad++;   /* 重叠/逆序 */
            }
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
        uint64_t child_max = audit_node(fs, a, h.sub_node_ptr.ptr, blk, false, inum, depth + 1);
        if (h.key != child_max) a.bad++;   /* hint key == 子树最大开边界（缩水刷链论证的哨兵） */
    }
    return prev_key;
}

static void audit_run(RavenFS_juvenile &fs, uint64_t root, uint64_t inum, const char *tag) {
    audit_t a;
    audit_node(fs, a, root, 0, true, inum, 0);
    CHECK(a.bad == 0, "audit[%s]: nodes=%lu inner=%lu leaves=%lu records=%lu bad=%lu",
          tag, a.nodes, a.inner, a.leaves, a.records, a.bad);
}

static uint64_t audit_records(RavenFS_juvenile &fs, uint64_t root, uint64_t inum, const char *tag) {
    audit_t a;
    audit_node(fs, a, root, 0, true, inum, 0);
    CHECK(a.bad == 0, "audit[%s]: records=%lu bad=%lu", tag, a.records, a.bad);
    return a.records;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device>\n", argv[0]); return 2; }
    uint64_t rc;
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    RavenFS_juvenile fs(mnt, &rc);
    if (!fs.is_mounted()) { printf("mount failed rc=%s\n", rc_name(rc)); return 1; }

    printf("-- 准备：root_dir 建文件 + inline→extent 跃迁 --\n");
    rc = fs.root_dir_insert_dentry("a.bin", INODE_TYPE_REGULAR);
    CHECK(rc == RC_OK, "root_dir_insert_dentry(a.bin) = %s", rc_name(rc));
    uint64_t inum = 0;
    fs.root_dir_lookup("a.bin", &inum);
    uint64_t root_blk = file_to_extent_mode(fs, inum);
    CHECK(root_blk != PTR_INVALID, "inline→extent 跃迁建树根 blk=%lu", root_blk);
    const uint64_t ROOT0 = root_blk;

    printf("\n-- A：契约防御（子集约束 + phys 自洽） --\n");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(0, 0, 0)) == RC_OK,
          "A 空区间 len=0 → RC_OK");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(0, 5, 100)) == RC_ERR_NOT_FOUND,
          "A 空树 loose → NOT_FOUND（起点空洞）");

    CHECK(fs.extent_map_range(root_blk, inum, mk_ext(0, 10, 100)) == RC_OK, "A map [0,10)p100");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(0, 8, 999)) == RC_ERR_STATE,
          "A phys 自洽失败（off=0 期望 p100，给 999）→ STATE");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(0, 12, 100)) == RC_ERR_STATE,
          "A 终点越出记录（[0,12) 超 [0,10)）→ STATE");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(10, 1, 110)) == RC_ERR_NOT_FOUND,
          "A 起点恰在记录终点（空洞）→ NOT_FOUND");
    CHECK(fs.extent_map_range(root_blk, inum, mk_ext(20, 5, 200)) == RC_OK, "A map [20,5)p200");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(21, 4, 202)) == RC_ERR_STATE,
          "A 贴尾 phys 偏移自洽失败（off=1 期望 p201，给 202）→ STATE");
    CHECK(audit_records(fs, root_blk, inum, "A 防御失败后零副作用") == 2,
          "A records=2（[0,10)p100 + [20,5)p200，全防御失败无副作用）");
    /* 清场：后续 B~E 组从空树开始（计数断言依赖确定初态） */
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(0, 10, 100)) == RC_OK, "A cleanup unmap [0,10)p100");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(20, 5, 200)) == RC_OK, "A cleanup unmap [20,5)p200");
    CHECK(audit_records(fs, root_blk, inum, "A-清场") == 0, "A 组尾树空（后续组从空树开始）");

    printf("\n-- B：head/tail 缩水 + full 整删 --\n");
    extent_t rec;
    CHECK(fs.extent_map_range(root_blk, inum, mk_ext(0, 10, 100)) == RC_OK, "B map [0,10)p100（自给）");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(0, 3, 100)) == RC_OK,
          "B head 挖头 [0,3)p100");
    CHECK(leaf_rec_at(fs, root_blk, 5, rec) && rec.fblkbase == 3 && rec.length == 7 &&
          rec.block_base.ptr == 103,
          "B head 后剩 [3,10)p103（fblkbase/phys 同步右移 3）got {%lu,%lu,p%lu}",
          rec.fblkbase, rec.length, rec.block_base.ptr);
    audit_run(fs, root_blk, inum, "B-head");

    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(6, 4, 106)) == RC_OK,
          "B tail 挖尾 [6,10)p106");
    CHECK(leaf_rec_at(fs, root_blk, 4, rec) && rec.fblkbase == 3 && rec.length == 3 &&
          rec.block_base.ptr == 103,
          "B tail 后剩 [3,6)p103 got {%lu,%lu,p%lu}",
          rec.fblkbase, rec.length, rec.block_base.ptr);
    audit_run(fs, root_blk, inum, "B-tail");

    CHECK(fs.extent_map_range(root_blk, inum, mk_ext(6, 4, 106)) == RC_OK,
          "B 挖出的洞 [6,10) 可回填 map");
    audit_run(fs, root_blk, inum, "B-回填");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(3, 7, 103)) == RC_OK,
          "B 回填后左并 [3,10)p103 整条 unmap（cleanup）");

    CHECK(fs.extent_map_range(root_blk, inum, mk_ext(50, 5, 500)) == RC_OK, "B map [50,5)p500");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(50, 5, 500)) == RC_OK,
          "B full 整条 loose（三字段全等）→ 交 unmap_range");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(50, 5, 500)) == RC_ERR_NOT_FOUND,
          "B full 后记录消失 → 再 loose NOT_FOUND");
    CHECK(audit_records(fs, root_blk, inum, "B-full") == 0,
          "B 组尾树空（C 组从空树开始）");

    printf("\n-- C：mid 拆两端（1 拆 2） --\n");
    CHECK(fs.extent_map_range(root_blk, inum, mk_ext(100, 20, 1000)) == RC_OK, "C map [100,20)p1000");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(108, 4, 1008)) == RC_OK,
          "C mid 挖中段 [108,112)p1008");
    CHECK(leaf_rec_at(fs, root_blk, 104, rec) && rec.fblkbase == 100 && rec.length == 8 &&
          rec.block_base.ptr == 1000,
          "C 左段 [100,108)p1000 got {%lu,%lu,p%lu}", rec.fblkbase, rec.length, rec.block_base.ptr);
    CHECK(leaf_rec_at(fs, root_blk, 116, rec) && rec.fblkbase == 112 && rec.length == 8 &&
          rec.block_base.ptr == 1012,
          "C 右段 [112,120)p1012 got {%lu,%lu,p%lu}", rec.fblkbase, rec.length, rec.block_base.ptr);
    CHECK(audit_records(fs, root_blk, inum, "C-mid") == 2, "C records=2");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(108, 4, 1008)) == RC_ERR_NOT_FOUND,
          "C 挖出的洞 [108,112) 再 loose → NOT_FOUND（洞内无记录）");
    CHECK(fs.extent_map_range(root_blk, inum, mk_ext(108, 4, 1008)) == RC_OK,
          "C 洞回填 map（紧贴+连续 → 与左段左并）");
    audit_run(fs, root_blk, inum, "C-回填");
    /* cleanup：回填后形态 = [100,112)p1000（左并）+ [112,120)p1012（可并残留合法），逐条删 */
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(100, 12, 1000)) == RC_OK, "C cleanup unmap [100,112)p1000");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(112, 8, 1012)) == RC_OK, "C cleanup unmap [112,120)p1012");
    CHECK(audit_records(fs, root_blk, inum, "C-清场") == 0, "C 组尾树空");

    printf("\n-- D：phase2 左并回滚模拟（loose 贴尾 = 裁切回滚） --\n");
    CHECK(fs.extent_map_range(root_blk, inum, mk_ext(0, 10, 100)) == RC_OK, "D map [0,10)p100（旧 G）");
    CHECK(fs.extent_map_range(root_blk, inum, mk_ext(10, 5, 110)) == RC_OK,
          "D map [10,15)p110 紧贴+phys 连续 → 左并");
    CHECK(audit_records(fs, root_blk, inum, "D-左并后") == 1, "D 左并 records=1（单条 [0,15)p100）");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(10, 5, 110)) == RC_OK,
          "D loose [10,15)p110（逆序账本裁 X 段）");
    CHECK(leaf_rec_at(fs, root_blk, 5, rec) && rec.fblkbase == 0 && rec.length == 10 &&
          rec.block_base.ptr == 100,
          "D 回滚后恢复 [0,10)p100 got {%lu,%lu,p%lu}", rec.fblkbase, rec.length, rec.block_base.ptr);
    audit_run(fs, root_blk, inum, "D-回滚后");
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(0, 10, 100)) == RC_OK, "D 整条 unmap（cleanup）");

    printf("\n-- E：满叶 mid 拆两端 → 分裂级联（预算池路径烟测） --\n");
    /* 337 条塞满根叶：1 长 [0,40)p1000 + 336 短（fb=40+2k 留洞隔离防左并，phys 错开） */
    CHECK(fs.extent_map_range(root_blk, inum, mk_ext(0, 40, 1000)) == RC_OK, "E map 长记录 [0,40)p1000");
    bool e_ok = true;
    for (uint64_t k = 0; k < 336; k++) {
        if (fs.extent_map_range(root_blk, inum, mk_ext(40 + 2 * k, 1, 3000 + k)) != RC_OK) { e_ok = false; break; }
    }
    CHECK(e_ok, "E 336 短记录填充（fb 隔洞防并）");
    CHECK(audit_records(fs, root_blk, inum, "E-满叶前") == 337, "E 满叶 records=337");
    /* 挖长记录中段 [20,30)：off=20 len=10 rem=10 → mid + count==337 → 预算 + 裂 */
    CHECK(fs.extent_unmap_range_loose(root_blk, inum, mk_ext(20, 10, 1020)) == RC_OK,
          "E mid 挖长记录中段 [20,30)p1020（满叶 → 预算 + 分裂级联）");
    CHECK(audit_records(fs, root_blk, inum, "E-分裂后") == 338,
          "E 拆两端 records=338（337-1+2）");
    CHECK(leaf_rec_at(fs, root_blk, 10, rec) && rec.fblkbase == 0 && rec.length == 20 &&
          rec.block_base.ptr == 1000,
          "E 左段 [0,20)p1000 got {%lu,%lu,p%lu}", rec.fblkbase, rec.length, rec.block_base.ptr);
    CHECK(leaf_rec_at(fs, root_blk, 35, rec) && rec.fblkbase == 30 && rec.length == 10 &&
          rec.block_base.ptr == 1030,
          "E 右段 [30,40)p1030 got {%lu,%lu,p%lu}", rec.fblkbase, rec.length, rec.block_base.ptr);

    /* detach 断言：分裂建新根 → root_blk 更迭；inode.data_root 未被原语改写（stale 预期） */
    raven_Inode_t ino;
    fs.inode_read(inum, &ino);
    CHECK(root_blk != ROOT0 && ino.data_root == ROOT0,
          "E detach：根裂更迭 root_blk=%lu（≠初值 %lu），inode.data_root 未被原语改写=%lu",
          root_blk, ROOT0, ino.data_root);

    printf("\n════════════════════════════════════════════════════\n");
    printf(" SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    printf("════════════════════════════════════════════════════\n");
    return g_fail ? 1 : 0;
}

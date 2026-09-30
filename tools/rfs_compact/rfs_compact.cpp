/* ═══════════════════════════════════════════════════════════════════
 * rfs_compact.cpp — RavenFS 离线「extent 树瘦身 + 单条坍缩 direct」定向修复器
 *
 * 目的（修复 ko v0.1.1 的 D-5/D-6 盘面创伤，见 Docs/linux/ravenfs-linux-ko-v0.1.1-defects.md）：
 *   · D-6：>160B 文件恒背 8K 区段树节点（单 extent 也背）→ 坍缩 direct 消掉节点税。
 *   · D-5：ko 写路径无叶内融合 → 相邻 extent 未合并（如 root-ext4.qcow2 133k 条）→ 激进重建融合。
 *
 * 做法：用户态挂载（mmap 后端 + inode/mdata_piece 缓存 = offload），从根 DFS 全目录树，
 * 对每个 REG inode：若为 extent 树形态 → 先 extent_rebuild_tree（全量融合），再
 * extent_tree_collapse_to_direct（单条覆盖 → direct + 释放节点）。
 *
 * 用法：
 *   rfs_compact <device>            # dry-run（只读挂载，扫描+报告，不写）
 *   rfs_compact <device> --apply    # 真正修复（rw 挂载 + 写）
 *   rfs_compact <device> --apply --audit   # 修复后跑一次全盘 audit
 *
 * ⚠️ 安全：--apply 会就地改写盘面。务必先 umount（内核不可见）+ 有备份/知道风险。
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session 完整类型 */
#include "ravenfs/bitmap_slices.hpp"        /* bitmap_slices_mgr_t 完整类型 */
#include "ravenfs/ravenfs_audit.hpp"         /* audit_opts_t / audit_report_t */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <string>
#include <unordered_set>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static const uint64_t PTR52_INVALID = (1ULL << 52) - 1;

struct stats_t {
    uint64_t dirs = 0, reg_files = 0, extent_files = 0;
    uint64_t pre_records = 0, post_records = 0;
    uint64_t collapse_legal = 0;
    uint64_t rebuilt = 0, collapsed = 0, failed = 0;
};

static const char *rc_name(result_code_t rc) {
    switch (rc) {
    case RC_OK:               return "RC_OK";
    case RC_ERR_NO_BLOCK:     return "RC_ERR_NO_BLOCK";
    case RC_ERR_STATE:        return "RC_ERR_STATE";
    case RC_ERR_IO:           return "RC_ERR_IO";
    case RC_ERR_NOT_FOUND:    return "RC_ERR_NOT_FOUND";
    case RC_ERR_BITMAP_SELF:  return "RC_ERR_BITMAP_SELF";
    case RC_ERR_VERSION:      return "RC_ERR_VERSION";
    default:                  return "RC_???";
    }
}

/* 收集 extent 树全部记录（左叶下钻 + ptrs[2] 叶链），并推算 rebuild 后条数 + 坍缩合法性 */
static void analyze_extent(RavenFS_juvenile &fs, uint64_t root_blk, uint64_t size, stats_t &st) {
    const uint64_t size_ceil = (size + 4095) / 4096;
    std::vector<extent_t> recs;
    uint64_t blk = root_blk;
    for (int d = 0; d < 64; d++) {
        mdata_piece_session s(fs.mdata_piece_mgr, blk);
        if (s.index_error() || !s) return;
        Bptree_meta_t *m = (Bptree_meta_t *)s.data;
        if (!is_extent_node(m)) return;
        if (!RFS_PP_IS_INNER(m->parent_ptr)) break;
        auto *in = (Bptree_extent_inner_node_t *)s.data;
        uint16_t c = (uint16_t)RFS_PP_COUNT(in->meta.parent_ptr);
        if (c == 0) return;
        blk = RFS_PTR_ADDR(in->extents[0].sub_node_ptr);
    }
    for (int g = 0; blk != PTR52_INVALID && g < 4000000; g++) {
        mdata_piece_session s(fs.mdata_piece_mgr, blk);
        if (s.index_error() || !s) return;
        Bptree_meta_t *m = (Bptree_meta_t *)s.data;
        if (!is_extent_node(m) || RFS_PP_IS_INNER(m->parent_ptr)) return;
        auto *lf = (Bptree_extent_leaf_node_t *)s.data;
        uint16_t c = (uint16_t)RFS_PP_COUNT(lf->meta.parent_ptr);
        for (uint16_t i = 0; i < c; i++) recs.push_back(lf->extents[i]);
        blk = RFS_PTR_ADDR(lf->meta.ptrs[2]);
    }
    uint64_t post = recs.empty() ? 0 : 1;
    for (size_t i = 1; i < recs.size(); i++) {
        const extent_t &p = recs[i - 1]; const extent_t &q = recs[i];
        const bool adj = (p.fblkbase + p.length == q.fblkbase) &&
                         (RFS_PTR_ADDR(p.block_base) + p.length == RFS_PTR_ADDR(q.block_base));
        if (!adj) post++;
    }
    st.pre_records += recs.size();
    st.post_records += post;
    if (post == 1 && !recs.empty()) {
        const uint64_t front = recs.front().fblkbase;
        const uint64_t back  = recs.back().fblkbase + recs.back().length;
        if (front == 0 && back == size_ceil) st.collapse_legal++;
    }
}

static void process_reg(RavenFS_juvenile &fs, uint64_t inum, bool apply, stats_t &st) {
    raven_Inode_t ino;
    fs.inode_read(inum, &ino);
    const uint32_t t = RFS_INO1_GET_TYPE(ino.word1);
    if (t != INODE_TYPE_REGULAR) return;               /* 只处理 REG */
    st.reg_files++;
    if (ino.size <= 160) return;                        /* inline */
    if (RFS_INO1_GET_DIRECT(ino.word1)) return;         /* 已 direct */
    if (ino.data_root == PTR_INVALID) return;
    st.extent_files++;

    if (!apply) { analyze_extent(fs, ino.data_root, ino.size, st); return; }

    /* 先激进重建（全量融合），再坍缩 direct */
    uint64_t rb = ino.data_root;
    result_code_t r = fs.extent_rebuild_tree(rb, inum);
    if (r != RC_OK) { st.failed++; fprintf(stderr, "  [rebuild fail] ino=%llu size=%llu rc=%s\n",
                                           (unsigned long long)inum, (unsigned long long)ino.size, rc_name(r)); return; }
    st.rebuilt++;
    ino.data_root = rb;
    fs.inode_write(inum, &ino);
    bool did = false;
    result_code_t cr = fs.extent_tree_collapse_to_direct(inum, &did);
    if (cr == RC_OK && did) st.collapsed++;
    else if (cr != RC_OK) { st.failed++; fprintf(stderr, "  [collapse fail] ino=%llu rc=%s\n",
                                                 (unsigned long long)inum, rc_name(cr)); }
}

static void dfs_dir(RavenFS_juvenile &fs, uint64_t dir_inum, bool apply, stats_t &st,
                    std::unordered_set<uint64_t> &visited, std::unordered_set<uint64_t> &seen_reg, int depth) {
    if (depth > 64) return;
    if (!visited.insert(dir_inum).second) return;       /* 防环 */
    raven_Inode_t dino;
    fs.inode_read(dir_inum, &dino);
    if (RFS_INO1_GET_TYPE(dino.word1) != INODE_TYPE_DIR) return;
    st.dirs++;
    uint64_t blk = dino.data_root;
    if (blk == PTR_INVALID) return;

    /* 最左叶：内部节点沿 hints[0] 下钻 */
    for (int d = 0; d < 64; d++) {
        mdata_piece_session s(fs.mdata_piece_mgr, blk);
        if (s.index_error() || !s) return;
        Bptree_meta_t *m = (Bptree_meta_t *)s.data;
        if (!is_dentry_node(m)) return;
        if (!RFS_PP_IS_INNER(m->parent_ptr)) break;
        auto *in = (Bptree_ddentry_inner_node_t *)s.data;
        uint16_t c = (uint16_t)RFS_PP_COUNT(in->meta.parent_ptr);
        if (c == 0) return;
        blk = RFS_PTR_ADDR(in->hints[0].sub_ptr);
    }
    /* 叶链遍历（ptrs[2] 后继）收集条目 → 递归/处理 */
    for (int g = 0; blk != PTR52_INVALID && g < 4000000; g++) {
        mdata_piece_session s(fs.mdata_piece_mgr, blk);
        if (s.index_error() || !s) break;
        Bptree_meta_t *m = (Bptree_meta_t *)s.data;
        if (!is_dentry_node(m) || RFS_PP_IS_INNER(m->parent_ptr)) break;
        auto *lf = (Bptree_ddentry_leaf_node_t *)s.data;
        uint16_t c = (uint16_t)RFS_PP_COUNT(lf->meta.parent_ptr);
        for (uint16_t i = 0; i < c; i++) {
            uint64_t e_inum = RFS_PTR_ADDR(lf->entries[i].inode_ptr);
            if (e_inum == 0 || e_inum == PTR52_INVALID || e_inum >= fs.inode_count) continue;
            raven_Inode_t cino;
            fs.inode_read(e_inum, &cino);
            uint32_t ct = RFS_INO1_GET_TYPE(cino.word1);
            if (ct == INODE_TYPE_DIR) dfs_dir(fs, e_inum, apply, st, visited, seen_reg, depth + 1);
            else if (ct == INODE_TYPE_REGULAR) {
                if (seen_reg.insert(e_inum).second) process_reg(fs, e_inum, apply, st);
            }
        }
        blk = RFS_PTR_ADDR(lf->meta.ptrs[2]);
    }
}

static linux_userspace_mount_t make_mount(const char *path, bool readonly) {
    linux_userspace_mount_t m{};
    m.dev.fd = ::open(path, readonly ? O_RDONLY : O_RDWR);
    int lba = 4096;
    if (::ioctl(m.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    m.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    m.dev.type = DEV_NVME;
    m.dev.is_device_direct = false;                 /* mmap 后端 */
    m.param.if_will_use_inode_cache = true;         /* offload 带缓存 */
    m.param.if_will_use_mdata_piece_cache = true;
    m.param.is_readonly = readonly;
    m.logical_processor_count = 1;
    return m;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <device> [--apply] [--audit]\n"
                        "  (默认 dry-run：只读挂载、扫描报告，不写盘)\n", argv[0]);
        return 2;
    }
    bool apply = false, do_audit = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--apply")) apply = true;
        else if (!strcmp(argv[i], "--audit")) do_audit = true;
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }

    printf("════════════════════════════════════════════════════════\n");
    printf(" RavenFS extent 瘦身/坍缩修复器  ——  %s\n", apply ? "【APPLY 写盘】" : "【DRY-RUN 只读】");
    printf(" device: %s\n", argv[1]);
    printf("════════════════════════════════════════════════════════\n");

    uint64_t rc = 0;
    linux_userspace_mount_t mnt = make_mount(argv[1], !apply);
    RavenFS_juvenile fs(mnt, &rc);
    if (!fs.is_mounted()) { fprintf(stderr, "mount failed rc=%s\n", rc_name(rc)); return 1; }
    printf(" mount OK. root_inode=%llu\n", (unsigned long long)fs.root_inum);

    uint64_t free_before = fs.bitmap_mgr ? fs.bitmap_mgr->block_free_total() : 0;

    stats_t st;
    std::unordered_set<uint64_t> visited, seen_reg;
    dfs_dir(fs, fs.root_inum, apply, st, visited, seen_reg, 0);

    uint64_t free_after = fs.bitmap_mgr ? fs.bitmap_mgr->block_free_total() : 0;

    printf("\n── 结果 ──\n");
    printf(" 目录 dirs=%llu   REG 文件=%llu   其中 extent 形态=%llu\n",
           (unsigned long long)st.dirs, (unsigned long long)st.reg_files, (unsigned long long)st.extent_files);
    if (!apply) {
        printf(" [dry-run] extent 记录 %llu → 融合后 ~%llu（可省 ~%llu 条）\n",
               (unsigned long long)st.pre_records, (unsigned long long)st.post_records,
               (unsigned long long)(st.pre_records - st.post_records));
        printf(" [dry-run] 融合后单条且整域覆盖 → 可坍缩 direct 的文件 = %llu\n",
               (unsigned long long)st.collapse_legal);
        printf(" [dry-run] 未写盘。确认无误后加 --apply。\n");
    } else {
        printf(" rebuild 成功=%llu   collapse 成功=%llu   失败=%llu\n",
               (unsigned long long)st.rebuilt, (unsigned long long)st.collapsed, (unsigned long long)st.failed);
        printf(" 位图空闲块 free_total %llu -> %llu（回收 %lld 块 = %lld KiB）\n",
               (unsigned long long)free_before, (unsigned long long)free_after,
               (long long)(free_after - free_before), (long long)(free_after - free_before) * 4);
    }

    if (do_audit) {
        audit_walk_report_t r{};                      /* 可达性审计（新模型） */
        fs.audit_dfs_root(&r, stdout);
        uint64_t bad = r.total_bad();
        printf(" [audit] bad=%llu (btree=%llu inode=%llu heap=%llu unprot_b=%llu leak_b=%llu)\n",
               (unsigned long long)bad, (unsigned long long)r.btree_bad,
               (unsigned long long)r.inode_bad, (unsigned long long)r.heap_bad,
               (unsigned long long)r.unprotected_blk, (unsigned long long)r.leaked_blk);
    }
    return st.failed ? 1 : 0;
}

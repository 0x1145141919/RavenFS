/* ═══════════════════════════════════════════════════════════════════
 * rfs_compact.cpp — RavenFS v2.0 离线「extent 文件碎片压缩」运维工具
 *
 * 目标（对 REG / SOFT_SYM 的 **extent 形态**文件做碎片消化）：
 *   · 把「逻辑连续、物理碎裂」的 run 搬到新连续物理段 → 一条 extent；
 *   · 洞原样保留（稀疏语义不变；绝不 hard_alloc 整域再打洞）；
 *   · 收敛成「单条整域」的稠密文件落 direct（顺带消 8K 树节点税）。
 * 算法在上帝类原语 `RavenFS_juvenile::regular_file_compact`（见 file_compact.cpp）；
 * 本工具只负责 DFS 枚举 + 调用 + 报表 + 审计。
 *
 * 分配模式：挂载时置 slices_mode = DEFRAG（block 单分片 + alloc_hard_longest_first）
 *   → 新数据优先填最长空闲段头部，利于空闲空间整合。
 *
 * 用法：
 *   rfs_compact <device|image>                 # dry-run（只读挂载，扫描+报告）
 *   rfs_compact <device|image> --apply         # 真修复（rw 挂载 + 写）
 *   rfs_compact <device|image> --apply --audit # 修复后跑一次全盘可达性审计
 *   rfs_compact <device|image> --max-move-blocks N   # 单 run 搬数上限（块）
 *
 * ⚠️ 安全：--apply 就地改写盘面。务必先 umount（内核不可见）；ctor 内有
 *    device_is_kernel_mounted 防呆（已挂载 → RC_ERR_BUSY 拒挂）。
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "ravenfs/ravenfs_juvenile.hpp"
#undef private
#include "ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session 完整类型 */
#include "ravenfs/inode_cache_radix.hpp"    /* InodeSession */
#include "ravenfs/fs_allocator.hpp"         /* fs_allocator_t（block_free_total） */
#include "ravenfs/ravenfs_audit.hpp"        /* audit_walk_report_t */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static const uint64_t PTR_INVALID_52 = (1ULL << 52) - 1;

struct stats_t {
    uint64_t dirs = 0;
    uint64_t reg_files = 0, symlinks = 0;
    uint64_t extent_files = 0;
    uint64_t records_before = 0, records_after = 0;
    uint64_t runs_total = 0, runs_fused = 0, runs_relocated = 0, runs_skipped = 0;
    uint64_t blocks_moved = 0;
    uint64_t compacted = 0, became_direct = 0, direct_swept = 0, failed = 0;
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
    case RC_ERR_BUSY:         return "RC_ERR_BUSY";
    default:                  return "RC_???";
    }
}

/* 处理单个文件（REG/SOFT_SYM）——薄转发到上帝类原语 + 汇总报表。 */
static void process_file(RavenFS_juvenile &fs, uint64_t inum, bool apply,
                         const file_compact_opts_t &opts, stats_t &st)
{
    file_compact_opts_t o = opts;
    o.dry_run = !apply;
    file_compact_report_t rep{};
    result_code_t rc = fs.regular_file_compact(inum, o, &rep);
    if (rc != RC_OK) {
        st.failed++;
        fprintf(stderr, "  [compact fail] ino=%llu rc=%s\n",
                (unsigned long long)inum, rc_name(rc));
        return;
    }
    if (rep.records_before == 0) return;   /* inline / direct / 空：非 extent 碎片 */

    /* ── 最后一步：单 run（单条整域）→ 落 direct ──
     * 压缩原语内已内联一次；这里再作**显式收口**（幂等）：覆盖「只搬数未坍缩」
     * 的边界，保证工具出口不变式 = 稠密单 run 文件必为 direct。 */
    if (apply && opts.collapse_direct && !rep.became_direct) {
        bool collapsed = false;
        if (fs.extent_tree_collapse_to_direct(inum, &collapsed) == RC_OK && collapsed) {
            rep.became_direct = true;
            rep.wrote = true;
            rep.records_after = 1;
            st.direct_swept++;
        }
    }

    st.extent_files++;
    st.records_before += rep.records_before;
    st.records_after  += rep.records_after;
    st.runs_total     += rep.runs_total;
    st.runs_fused     += rep.runs_fused;
    st.runs_relocated += rep.runs_relocated;
    st.runs_skipped   += rep.runs_skipped;
    st.blocks_moved   += rep.blocks_moved;
    if (rep.became_direct) st.became_direct++;
    if (rep.wrote)         st.compacted++;

    if (rep.runs_relocated || rep.runs_fused || rep.runs_skipped || rep.became_direct) {
        printf("  ino=%-8llu size=%-12llu recs %llu→%llu  runs=%llu "
               "(fuse=%llu move=%llu skip=%llu)  moved=%llu blk%s\n",
               (unsigned long long)rep.inum, (unsigned long long)rep.size,
               (unsigned long long)rep.records_before, (unsigned long long)rep.records_after,
               (unsigned long long)rep.runs_total, (unsigned long long)rep.runs_fused,
               (unsigned long long)rep.runs_relocated, (unsigned long long)rep.runs_skipped,
               (unsigned long long)rep.blocks_moved,
               rep.became_direct ? "  [→direct]" : "");
    }
}

/* 目录 DFS：dentry 树最左叶下钻 + 叶链 → 逐条目分派（DIR 递归 / REG·SYM 处理）。 */
static void dfs_dir(RavenFS_juvenile &fs, uint64_t dir_inum, bool apply,
                    const file_compact_opts_t &opts, stats_t &st,
                    std::unordered_set<uint64_t> &visited,
                    std::unordered_set<uint64_t> &seen_file, int depth)
{
    if (depth > 64) return;
    if (!visited.insert(dir_inum).second) return;       /* 防环 */

    uint64_t root_blk = 0;
    {
        InodeSession dino(fs.inode_io, dir_inum);
        if (!dino.inode) return;
        if (RFS_INO1_GET_TYPE(dino.inode->word1) != INODE_TYPE_DIR) return;
        root_blk = dino.inode->data_root;
    }
    st.dirs++;
    if (root_blk == PTR_INVALID_52 || root_blk == PTR_INVALID) return;

    /* 最左叶：内部节点沿 hints[0].sub_ptr 下钻 */
    uint64_t blk = root_blk;
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

    /* 叶链遍历（ptrs[2] 后继）→ 逐条目分派 */
    for (int g = 0; blk != PTR_INVALID_52 && g < 4000000; g++) {
        mdata_piece_session s(fs.mdata_piece_mgr, blk);
        if (s.index_error() || !s) break;
        Bptree_meta_t *m = (Bptree_meta_t *)s.data;
        if (!is_dentry_node(m) || RFS_PP_IS_INNER(m->parent_ptr)) break;
        auto *lf = (Bptree_ddentry_leaf_node_t *)s.data;
        uint16_t c = (uint16_t)RFS_PP_COUNT(lf->meta.parent_ptr);
        for (uint16_t i = 0; i < c; i++) {
            uint64_t e_inum = RFS_PTR_ADDR(lf->entries[i].inode_ptr);
            if (e_inum == 0 || e_inum == PTR_INVALID_52 || e_inum >= fs.inode_count) continue;

            uint32_t ct;
            {
                InodeSession cino(fs.inode_io, e_inum);
                if (!cino.inode) continue;
                ct = RFS_INO1_GET_TYPE(cino.inode->word1);
            }
            if (ct == INODE_TYPE_DIR) {
                dfs_dir(fs, e_inum, apply, opts, st, visited, seen_file, depth + 1);
            } else if (ct == INODE_TYPE_REGULAR || ct == INODE_TYPE_SOFT_SYM) {
                if (!seen_file.insert(e_inum).second) continue;
                if (ct == INODE_TYPE_REGULAR) st.reg_files++; else st.symlinks++;
                process_file(fs, e_inum, apply, opts, st);
            }
        }
        blk = RFS_PTR_ADDR(lf->meta.ptrs[2]);
    }
}

static linux_userspace_mount_t make_mount(const char *path, bool readonly) {
    linux_userspace_mount_t m{};
    m.dev.fd = ::open(path, readonly ? O_RDONLY : O_RDWR);
    int lba = 4096;
    if (m.dev.fd >= 0 && ::ioctl(m.dev.fd, BLKSSZGET, &lba) != 0) lba = 4096;
    if (lba <= 0) lba = 4096;
    m.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    m.dev.type = DEV_NVME;
    m.dev.is_device_direct = false;                 /* mmap 后端 */
    m.param.if_will_use_inode_cache = true;         /* offload 带缓存 */
    m.param.if_will_use_mdata_piece_cache = true;
    m.param.is_readonly = readonly;
    m.param.slices_mode = SLICES_MODE_DEFRAG;   /* 碎片缓解模式：block 单分片 + longest-first */
    m.logical_processor_count = 1;
    return m;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <device|image> [--apply] [--audit] [--max-move-blocks N] [--no-direct]\n"
                        "  (默认 dry-run：只读挂载、扫描报告，不写盘)\n", argv[0]);
        return 2;
    }
    bool apply = false, do_audit = false;
    file_compact_opts_t opts{};
    opts.dry_run = true;
    opts.max_move_blocks = 0;
    opts.collapse_direct = true;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--apply")) apply = true;
        else if (!strcmp(argv[i], "--audit")) do_audit = true;
        else if (!strcmp(argv[i], "--no-direct")) opts.collapse_direct = false;
        else if (!strcmp(argv[i], "--max-move-blocks")) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for --max-move-blocks\n"); return 2; }
            opts.max_move_blocks = strtoull(argv[++i], nullptr, 0);
        } else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }

    printf("════════════════════════════════════════════════════════\n");
    printf(" RavenFS v2.0 extent 文件碎片压缩器  ——  %s\n", apply ? "【APPLY 写盘】" : "【DRY-RUN 只读】");
    printf(" device: %s\n", argv[1]);
    printf(" mode  : %s   max_move_blocks=%llu\n",
           apply ? "DEFRAG(apply)" : "DEFRAG(dry-run)",
           (unsigned long long)opts.max_move_blocks);
    printf("════════════════════════════════════════════════════════\n");

    uint64_t rc = 0;
    linux_userspace_mount_t mnt = make_mount(argv[1], !apply);
    if (mnt.dev.fd < 0) { perror(argv[1]); return 1; }
    RavenFS_juvenile fs(mnt, &rc);
    if (!fs.is_mounted()) { fprintf(stderr, "mount failed rc=%s\n", rc_name(rc)); return 1; }
    printf(" mount OK. root_inode=%llu  inode_count=%llu\n",
           (unsigned long long)fs.root_inum, (unsigned long long)fs.inode_count);

    const uint64_t free_before = fs.allocator ? fs.allocator->block_free_total() : 0;

    stats_t st;
    std::unordered_set<uint64_t> visited, seen_file;
    dfs_dir(fs, fs.root_inum, apply, opts, st, visited, seen_file, 0);

    const uint64_t free_after = fs.allocator ? fs.allocator->block_free_total() : 0;

    printf("\n── 结果 ──\n");
    printf(" 目录 dirs=%llu   REG=%llu   SYM=%llu   （其中 extent 形态=%llu）\n",
           (unsigned long long)st.dirs, (unsigned long long)st.reg_files,
           (unsigned long long)st.symlinks, (unsigned long long)st.extent_files);
    printf(" extent 记录 %llu → %llu（%s %llu 条）\n",
           (unsigned long long)st.records_before, (unsigned long long)st.records_after,
           apply ? "实收省" : "可省",
           (unsigned long long)(st.records_before > st.records_after
                                ? st.records_before - st.records_after : 0));
    printf(" runs 总=%llu  零搬融合=%llu  搬迁=%llu  跳过=%llu  搬数=%llu 块%s\n",
           (unsigned long long)st.runs_total, (unsigned long long)st.runs_fused,
           (unsigned long long)st.runs_relocated, (unsigned long long)st.runs_skipped,
           (unsigned long long)st.blocks_moved, apply ? "" : "（将搬）");
    if (apply) {
        printf(" 实际提交文件=%llu   降 direct=%llu（其中最后一步收口=%llu）   失败=%llu\n",
               (unsigned long long)st.compacted, (unsigned long long)st.became_direct,
               (unsigned long long)st.direct_swept, (unsigned long long)st.failed);
        printf(" 位图空闲块 free_total %llu → %llu（%+lld 块）\n",
               (unsigned long long)free_before, (unsigned long long)free_after,
               (long long)(free_after - free_before));
    } else {
        printf(" [dry-run] 将降 direct=%llu；未写盘。确认无误后加 --apply。\n",
               (unsigned long long)st.became_direct);
    }

    if (do_audit) {
        audit_walk_report_t r{};
        fs.audit_dfs_root(&r, stdout);
        printf(" [audit] bad=%llu (btree=%llu inode=%llu namedata=%llu inodeplane=%llu "
               "xattr=%llu unprot_b=%llu leak_b=%llu)\n",
               (unsigned long long)r.total_bad(), (unsigned long long)r.btree_bad,
               (unsigned long long)r.inode_bad, (unsigned long long)r.namedata_bad,
               (unsigned long long)r.inodeplane_bad, (unsigned long long)r.xattr_bad,
               (unsigned long long)r.unprotected_blk, (unsigned long long)r.leaked_blk);
    }
    return st.failed ? 1 : 0;
}

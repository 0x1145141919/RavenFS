/*
 * file_compact.cpp — 文件碎片压缩（离线运维件，regular_file_compact）
 *
 * 语义 / 写序 / 契约见 include/ravenfs/ravenfs_juvenile.hpp 的 file_compact_opts_t
 * 与 regular_file_compact 声明。要点：
 *   · 以「逻辑 run」为单位：run 内物理断裂 → 搬到新连续段；run 内物理连续 →
 *     零搬数融合；**洞不碰**（稀疏语义不变）。
 *   · 先分配新段 + 拷数 → 建**全新**树（extent_build_tree；旧树全程只读）→
 *     inode 一次写（唯一提交点）→ 回收被搬 run 的旧数据 + 旧树节点。
 *   · 崩溃窗口全是孤儿（fsck 收）；提交前旧态零扰动。
 *   · 逐 run 尽力：分配不到 / 超 max_move_blocks 的 run 跳过，其余照提。
 */
#include "../include/ravenfs/ravenfs_juvenile.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session 完整类型 */
#include "../include/ravenfs/inode_cache_radix.hpp"    /* InodeSession：inode 访问统一走缓存 */
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr uint64_t FC_COPY_CHUNK = 256;   /* 搬数缓冲 256 块 = 1 MiB（分段 IO） */

struct fc_run_t {                          /* 一个逻辑 run（逻辑相邻的一串记录） */
    uint64_t              fblkbase = 0;    /* 逻辑起点 */
    uint64_t              len      = 0;    /* Σ 记录长度（块） */
    std::vector<extent_t> recs;            /* 原记录（逻辑序） */
};

}  // namespace

result_code_t RavenFS_juvenile::regular_file_compact(uint64_t inode_idx,
                                                     const file_compact_opts_t &opts,
                                                     file_compact_report_t *out)
{
    if (out) *out = file_compact_report_t{};
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    constexpr uint64_t FS_BLOCK = 4096;

    /* ── 1. 入口读态：受理校验 + 形态/树根/大小快照 ── */
    uint64_t fsize = 0, old_root = 0;
    {
        InodeSession s(inode_io, inode_idx);
        if (!s.inode) return RC_ERR_IO;
        raven_Inode_t *ino = s.inode;
        const uint32_t t = RFS_INO1_GET_TYPE(ino->word1);
        if (t != INODE_TYPE_REGULAR && t != INODE_TYPE_SOFT_SYM) return RC_ERR_STATE;
        fsize = ino->size;
        if (out) { out->inum = inode_idx; out->size = fsize; }
        if (fsize <= 160) return RC_OK;                      /* inline：无 extent 树 */
        if (RFS_INO1_GET_DIRECT(ino->word1)) return RC_OK;   /* direct：无碎片 */
        old_root = ino->data_root;
    }
    if (old_root == PTR_INVALID_52 || old_root == PTR_INVALID) return RC_ERR_STATE;

    /* ── 2. 单趟栈式 DFS 收集全部记录（子倒序入栈 → 出栈序 = 逻辑序）── */
    std::vector<extent_t> recs;
    {
        std::vector<uint64_t> stack;
        stack.push_back(old_root);
        uint64_t guard = 0;
        while (!stack.empty()) {
            if (++guard > (1ULL << 26)) return RC_ERR_STATE;
            const uint64_t blk = stack.back();
            stack.pop_back();
            if (blk == PTR_INVALID_52) return RC_ERR_STATE;
            mdata_piece_session sess(mdata_piece_mgr, blk);
            if (sess.index_error()) return RC_ERR_STATE;
            if (!sess) return RC_ERR_IO;
            Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
            if (!is_extent_node(meta)) return RC_ERR_STATE;
            if (RFS_PP_IS_INNER(meta->parent_ptr)) {
                Bptree_extent_inner_node_t *inner = (Bptree_extent_inner_node_t *)sess.data;
                const uint16_t cnt = (uint16_t)RFS_PP_COUNT(inner->meta.parent_ptr);
                if (cnt == 0) return RC_ERR_STATE;
                for (uint16_t i = cnt; i-- > 0;)
                    stack.push_back(RFS_PTR_ADDR(inner->extents[i].sub_node_ptr));
            } else {
                Bptree_extent_leaf_node_t *leaf = (Bptree_extent_leaf_node_t *)sess.data;
                const uint16_t cnt = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
                const size_t old = recs.size();
                recs.resize(old + cnt);
                memcpy(&recs[old], leaf->extents, (size_t)cnt * sizeof(extent_t));
            }
        }
    }
    if (out) out->records_before = recs.size();
    if (recs.empty()) return RC_OK;   /* 无数据记录：不建空树（结构畸形归 audit） */

    /* 记录合法性防御（phys 无效 / 零长 → 拒绝，防乱搬乱 free） */
    for (const extent_t &e : recs)
        if (RFS_PTR_ADDR(e.block_base) == PTR_INVALID_52 || e.length == 0)
            return RC_ERR_STATE;

    /* ── 3. 归并成逻辑 run ── */
    std::vector<fc_run_t> runs;
    for (const extent_t &c : recs) {
        if (!runs.empty() && runs.back().fblkbase + runs.back().len == c.fblkbase) {
            runs.back().recs.push_back(c);
            runs.back().len += c.length;
        } else {
            fc_run_t r;
            r.fblkbase = c.fblkbase;
            r.len      = c.length;
            r.recs.push_back(c);
            runs.push_back(std::move(r));
        }
    }
    if (out) out->runs_total = runs.size();

    /* ── 4. 规划：每 run → 单条（融合/搬迁/保留，逐 run 尽力）── */
    std::vector<extent_t> new_recs;
    new_recs.reserve(runs.size());
    std::vector<extent_t> moved_new;   /* 已搬 run 的新记录（建树失败回滚清单） */
    std::vector<extent_t> old_moved;   /* 已搬 run 的旧记录（提交后 free） */
    bool any_change = false;

    for (const fc_run_t &r : runs) {
        if (r.recs.size() == 1) {              /* 单条：已是该 run 最小表示 */
            new_recs.push_back(r.recs[0]);
            continue;
        }

        /* run 内 phys 链连续？→ 零搬数融合成单条 */
        bool chain_ok = true;
        for (size_t i = 1; i < r.recs.size(); i++) {
            if (RFS_PTR_ADDR(r.recs[i - 1].block_base) + r.recs[i - 1].length !=
                RFS_PTR_ADDR(r.recs[i].block_base)) { chain_ok = false; break; }
        }
        extent_t nr;
        nr.fblkbase = r.fblkbase;
        nr.length   = r.len;

        if (chain_ok) {
            RFS_PTR_SET_ADDR(nr.block_base, RFS_PTR_ADDR(r.recs[0].block_base));
            new_recs.push_back(nr);
            if (out) out->runs_fused++;
            any_change = true;
            continue;
        }

        /* 超限 → 跳过（保留原记录） */
        if (opts.max_move_blocks != 0 && r.len > opts.max_move_blocks) {
            if (out) out->runs_skipped++;
            for (const extent_t &e : r.recs) new_recs.push_back(e);
            continue;
        }

        /* dry-run：不分配不拷贝，按「理想单条」计入 records_after */
        if (opts.dry_run) {
            if (out) { out->runs_relocated++; out->blocks_moved += r.len; }
            RFS_PTR_SET_ADDR(nr.block_base, RFS_PTR_ADDR(r.recs[0].block_base));
            new_recs.push_back(nr);
            continue;
        }

        /* 真搬：分配连续新段（DEFRAG 下 = 最长空闲段头部） */
        const uint64_t np = block_alloc_hard(r.len, 0);
        if (np == PTR_INVALID) {
            if (out) out->runs_skipped++;
            for (const extent_t &e : r.recs) new_recs.push_back(e);
            continue;
        }

        /* 逐记录读→写拼接到新段（分段 1MiB；4K 对齐缓冲，兼容 O_DIRECT） */
        void *buf = nullptr;
        if (posix_memalign(&buf, FS_BLOCK, (size_t)FC_COPY_CHUNK * FS_BLOCK) != 0 || !buf) {
            block_free(np, r.len);
            if (out) out->runs_skipped++;
            for (const extent_t &e : r.recs) new_recs.push_back(e);
            continue;
        }
        bool copy_ok = true;
        uint64_t dst = np;
        for (const extent_t &e : r.recs) {
            const uint64_t src = RFS_PTR_ADDR(e.block_base);
            uint64_t done = 0;
            while (done < e.length) {
                const uint64_t n = std::min<uint64_t>(e.length - done, FC_COPY_CHUNK);
                if (read_buff(src + done, n, buf) != RC_OK) { copy_ok = false; break; }
                if (write_buff(dst + done, n, buf) != RC_OK) { copy_ok = false; break; }
                done += n;
            }
            if (!copy_ok) break;
            dst += e.length;
        }
        free(buf);
        if (!copy_ok) {
            block_free(np, r.len);
            if (out) out->runs_skipped++;
            for (const extent_t &e : r.recs) new_recs.push_back(e);
            continue;
        }

        RFS_PTR_SET_ADDR(nr.block_base, np);
        new_recs.push_back(nr);
        moved_new.push_back(nr);
        for (const extent_t &e : r.recs) old_moved.push_back(e);
        if (out) { out->runs_relocated++; out->blocks_moved += r.len; }
        any_change = true;
    }

    if (out) out->records_after = new_recs.size();

    /* ── 5. 收敛判定：结果单条且整域覆盖 [0, ceil(size/4K)) → direct ── */
    const uint64_t size_ceil = (fsize + FS_BLOCK - 1) / FS_BLOCK;
    bool make_direct = false;
    uint64_t direct_base = 0;
    if (opts.collapse_direct && new_recs.size() == 1 &&
        new_recs[0].fblkbase == 0 && new_recs[0].length == size_ceil) {
        make_direct = true;
        direct_base = RFS_PTR_ADDR(new_recs[0].block_base);
    }

    if (opts.dry_run) {
        if (out) out->became_direct = make_direct;
        return RC_OK;                       /* 未动盘 */
    }

    /* 无变化且不降 direct → 旧态本就最优，no-op */
    if (!any_change && !make_direct) return RC_OK;

    /* 无搬数但可降 direct（本就单条整域 extent）→ 复用现成坍缩原语 */
    if (!any_change && make_direct) {
        bool collapsed = false;
        result_code_t rc = extent_tree_collapse_to_direct(inode_idx, &collapsed);
        if (out) { out->became_direct = collapsed; out->wrote = (rc == RC_OK && collapsed); }
        return rc;
    }

    /* ── 6. 建态：全新树 / direct（失败 = 还新块，旧态零扰动）── */
    uint64_t new_root = PTR_INVALID;
    if (!make_direct) {
        result_code_t rc = extent_build_tree(inode_idx, new_recs, new_root);
        if (rc != RC_OK) {
            for (const extent_t &e : moved_new)
                block_free(RFS_PTR_ADDR(e.block_base), e.length);
            return rc;
        }
    }

    /* ── 7. 提交点：inode 一次写（data_root / direct 位）── */
    {
        InodeSession s(inode_io, inode_idx);
        if (!s.inode) {
            if (new_root != PTR_INVALID) extent_orphan_release(new_root);
            for (const extent_t &e : moved_new)
                block_free(RFS_PTR_ADDR(e.block_base), e.length);
            return RC_ERR_IO;
        }
        if (make_direct) {
            RFS_INO1_SET_DIRECT(*s.inode, 1);
            s.inode->data_root = direct_base;
        } else {
            RFS_INO1_SET_DIRECT(*s.inode, 0);
            s.inode->data_root = new_root;
        }
        s.dirty();
    }

    /* ── 8. 提交后回收（尽力而为；失败 = 孤儿，fsck 收）──
     * 只 free **被搬 run 的旧数据**（保留/融合 run 的块仍被新态引用，绝不 free）。 */
    for (const extent_t &e : old_moved)
        block_free(RFS_PTR_ADDR(e.block_base), e.length);
    extent_orphan_release(old_root);

    if (out) { out->became_direct = make_direct; out->wrote = true; }
    return RC_OK;
}

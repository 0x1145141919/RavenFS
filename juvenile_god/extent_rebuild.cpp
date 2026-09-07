#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>
#include <vector>
#include <algorithm>

/* ═══════════════════════════════════════════════════════════════════
 * extent_rebuild.cpp — 激进重建原语（全量融合，原 extent_tree.cpp 拆出）
 *
 * 本文件负责：extent_rebuild_tree + 打包助手 rb_group_size/rb_parent_of
 * （static，仅本文件使用）。定位 = 低频兜底面：entry[0] 跨界残留 / 随机
 * 写中间插入残留 / 历史遗留的可并对全并 + 均分重打包重建。
 * ═══════════════════════════════════════════════════════════════════ */


/* ═══════════════════════════════════════════════════════════════════
 * 激进重建打包助手（extent_rebuild_tree 专用，2026-09-06 定案）
 *
 * 均分打包：items 个元素分到 groups 组（前 rem 组多 1 个）。
 * 容量论证：groups = ceil(items/337) 时每组 ∈ [168, 337]（items ≥ 338 且
 * groups ≥ 2；items ≤ 337 时单组根节点任意 count）——非根节点欠载免疫，
 * 审计的 168 下限自动满足。337 阶容量：ceil ≤ 337 由 groups 定义保证。
 * ═══════════════════════════════════════════════════════════════════ */
static uint64_t rb_group_size(uint64_t items, uint64_t groups, uint64_t idx) {
    return items / groups + (idx < items % groups ? 1 : 0);
}
/* 子→父反函数：child_idx 在父层 groups 个节点中的归属（O(1)，均分反解） */
static uint64_t rb_parent_of(uint64_t child_items, uint64_t parent_groups, uint64_t child_idx) {
    const uint64_t base = child_items / parent_groups;   /* ≥1：groups ≤ items */
    const uint64_t rem = child_items % parent_groups;
    if (rem == 0) return child_idx / base;
    const uint64_t big_end = rem * (base + 1);           /* 前 rem 组是大组（base+1） */
    return (child_idx < big_end) ? child_idx / (base + 1)
                                 : rem + (child_idx - big_end) / base;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_rebuild_tree — 激进重建原语（全量融合，2026-09-06 定案）
 *
 * 定位：全量融合 = 激进重建（低频兜底面：entry[0] 跨界残留 / 随机写中间
 * 插入残留 / 历史遗留）。完整算法链：
 *   叶链遍历收集全部记录（叶序 = 逻辑序）→ 可并对全并（紧贴 && phys 连续）
 *   → 均分重打包（每节点 168..337）→ 自下而上构建内部节点 → swap 新根
 *   → free 旧树。顺带兑现：整树节点收集释放轮子（待办 #3）。
 *
 * 失败原子性：新树全部节点**先分配先写盘**（旧树全程只读）——任何一步
 * 失败 = 仅 free 已分配的新节点、旧树无损（调用方可重试或 fsck）。
 * 旧树 free 与 root_blk 更替在成功路径末尾：崩溃窗口（free 后 inode 未
 * 回写）与 unmap 后数据块 free 同模型，归 fsck 范畴（C 决策）。
 *
 * 打包规则：每层节点数 = ceil(本层元素 / 337)，均分填充（非根节点
 * 恒 ∈ [168,337]）；根单节点（叶或内）允许任意 count ∈ [1,337]。
 * 叶链表重建：ptrs[1]=前驱 / ptrs[2]=后继（首尾 PTR_INVALID），
 * 兄弟链序 == 层内块序（DFS 序一致，audit_chain 可验）。
 *
 * 外显契约（detach 同 map/unmap）：root_blk in/out（换新根经引用带出）；
 * owner_inode_idx 纯审计锚（根 meta "."）；不碰 inode；调用方负责把新
 * root_blk 回写 inode.data_root（文件层提交点）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_rebuild_tree(uint64_t &root_blk, uint64_t owner_inode_idx)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    std::vector<uint64_t> old_blocks;   /* 旧树全部节点块号（整体释放） */
    std::vector<extent_t> recs;         /* 全部记录（叶序 = 逻辑序） */

    /* ── 1. 旧树遍历：单趟栈式 DFS，节点块收集 + 叶记录收集 ──
     * 子节点倒序入栈 → 出栈序 = 左→右 = 逻辑序（树不变量：子树区间有序）。 */
    {
        std::vector<uint64_t> stack;
        stack.push_back(root_blk);
        uint64_t guard = 0;                       /* 防环/天文节点数防御 */
        while (!stack.empty()) {
            if (++guard > (1ull << 26)) return RC_ERR_STATE;
            const uint64_t blk = stack.back();
            stack.pop_back();
            if (blk == PTR_INVALID_52) return RC_ERR_STATE;
            mdata_piece_session sess(mdata_piece_mgr, blk);
            if (sess.index_error()) return RC_ERR_STATE;
            if (!sess) return RC_ERR_IO;
            Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
            if (!is_extent_node(meta)) return RC_ERR_STATE;
            old_blocks.push_back(blk);
            if (meta->parent_ptr.is_inner_node) {
                Bptree_extent_inner_node_t *inner = (Bptree_extent_inner_node_t *)sess.data;
                const uint16_t cnt = (uint16_t)inner->meta.parent_ptr.entry_count;
                if (cnt == 0) return RC_ERR_STATE;
                for (uint16_t i = cnt; i-- > 0;)   /* 倒序：先入栈最右 → 最左先出 */
                    stack.push_back(inner->extents[i].sub_node_ptr.ptr);
            } else {
                Bptree_extent_leaf_node_t *leaf = (Bptree_extent_leaf_node_t *)sess.data;
                const uint16_t cnt = (uint16_t)leaf->meta.parent_ptr.entry_count;
                const size_t old = recs.size();
                recs.resize(old + cnt);
                memcpy(&recs[old], leaf->extents, (size_t)cnt * sizeof(extent_t));
            }
        }
    }

    /* ── 2. 可并对全并（就地压缩）：紧贴 && phys 连续 → 吸收右邻 ── */
    size_t w = 0;
    for (size_t i = 0; i < recs.size(); i++) {
        if (w > 0) {
            extent_t &p = recs[w - 1];
            const extent_t &c = recs[i];
            if (p.fblkbase + p.length == c.fblkbase &&
                p.block_base.ptr + p.length == c.block_base.ptr) {
                p.length += c.length;
                continue;
            }
        }
        recs[w++] = recs[i];
    }
    recs.resize(w);

    const uint64_t M = recs.size();
    if (M == 0) return RC_OK;   /* 空树无融合需求（结构畸形由 audit 另查） */

    /* ── 3. 层规划 + 全量预分配（失败 = free 新节点，旧树无损） ── */
    std::vector<uint64_t> m;                          /* m[k] = k 层节点数 */
    m.push_back((M + 336) / 337);
    while (m.back() > 1) m.push_back((m.back() + 336) / 337);
    const size_t top = m.size() - 1;

    std::vector<std::vector<uint64_t>> blks(m.size());
    std::vector<uint64_t> all_new;                    /* 回滚清单 */
    for (size_t k = 0; k < m.size(); k++) {
        blks[k].resize(m[k]);
        for (uint64_t i = 0; i < m[k]; i++) {
            const uint64_t b = block_alloc_hard(2, 1);
            if (b == PTR_INVALID) {
                for (uint64_t x : all_new) block_free(x, 2);
                return RC_ERR_NO_BLOCK;               /* 原子失败：旧树未动 */
            }
            blks[k][i] = b;
            all_new.push_back(b);
        }
    }

    /* ── 4. 填叶层（level 0）：均分打包 + 叶链表 + 逐叶 max ── */
    std::vector<uint64_t> lmax(m[0]);                 /* 层 max key 数组 */
    {
        uint64_t off = 0;
        for (uint64_t g = 0; g < m[0]; g++) {
            const uint64_t cnt_g = rb_group_size(M, m[0], g);
            const bool leaf_is_root = (m.size() == 1);
            const uint64_t parent = leaf_is_root
                ? owner_inode_idx
                : blks[1][rb_parent_of(m[0], m[1], g)];
            mdata_piece_session sess(mdata_piece_mgr, blks[0][g]);
            if (sess.index_error()) { for (uint64_t x : all_new) block_free(x, 2); return RC_ERR_STATE; }
            if (!sess) { for (uint64_t x : all_new) block_free(x, 2); return RC_ERR_IO; }
            Bptree_extent_leaf_node_t *leaf = (Bptree_extent_leaf_node_t *)sess.data;
            extent_node_meta_init(sess.data, (uint16_t)cnt_g, false, leaf_is_root, parent);
            leaf->meta.ptrs[0].ptr = PTR_INVALID;
            leaf->meta.ptrs[1].ptr = (g == 0) ? PTR_INVALID : blks[0][g - 1];
            leaf->meta.ptrs[2].ptr = (g == m[0] - 1) ? PTR_INVALID : blks[0][g + 1];
            memcpy(leaf->extents, &recs[off], (size_t)cnt_g * sizeof(extent_t));
            lmax[g] = recs[off + cnt_g - 1].fblkbase + recs[off + cnt_g - 1].length;
            off += cnt_g;
            sess.dirty();
        }
    }

    /* ── 5. 填内层（自底向上）：hint = 子层 max + 子块号；写子即定父（预分配） ── */
    for (size_t k = 1; k <= top; k++) {
        std::vector<uint64_t> kmax(m[k]);
        uint64_t coff = 0;
        for (uint64_t p = 0; p < m[k]; p++) {
            const uint64_t cnt_p = rb_group_size(m[k - 1], m[k], p);
            const bool is_root_p = (k == top);
            const uint64_t parent = is_root_p
                ? owner_inode_idx
                : blks[k + 1][rb_parent_of(m[k], m[k + 1], p)];
            mdata_piece_session sess(mdata_piece_mgr, blks[k][p]);
            if (sess.index_error()) { for (uint64_t x : all_new) block_free(x, 2); return RC_ERR_STATE; }
            if (!sess) { for (uint64_t x : all_new) block_free(x, 2); return RC_ERR_IO; }
            Bptree_extent_inner_node_t *inner = (Bptree_extent_inner_node_t *)sess.data;
            extent_node_meta_init(sess.data, (uint16_t)cnt_p, true, is_root_p, parent);
            inner->meta.ptrs[0].ptr = PTR_INVALID;
            inner->meta.ptrs[1].ptr = PTR_INVALID;
            inner->meta.ptrs[2].ptr = PTR_INVALID;
            for (uint64_t t = 0; t < cnt_p; t++) {
                inner->extents[t].key = lmax[coff + t];
                inner->extents[t].sub_node_ptr.ptr = blks[k - 1][coff + t];
            }
            kmax[p] = inner->extents[cnt_p - 1].key;
            coff += cnt_p;
            sess.dirty();
        }
        lmax.swap(kmax);
    }

    /* ── 6. 收尾：free 旧树（去重防环）→ root_blk 换新根 ── */
    std::sort(old_blocks.begin(), old_blocks.end());
    old_blocks.erase(std::unique(old_blocks.begin(), old_blocks.end()), old_blocks.end());
    for (uint64_t b : old_blocks) block_free(b, 2);
    root_blk = blks[top][0];
    return RC_OK;
}

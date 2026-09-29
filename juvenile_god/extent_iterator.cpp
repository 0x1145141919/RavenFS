#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>
#include <vector>
#include <algorithm>

/* ═══════════════════════════════════════════════════════════════════
 * extent_iterator.cpp — 稀疏文件空洞/区间游走器（原 extent_tree.cpp 拆出）
 *
 * 本文件负责：extent_iterator 全套（构造 / next_gap / prev_gap /
 * 前缀后缀 ++ / -- / current_extent）。只读游走，零写盘。
 *
 * 位置模型（头文件契约摘要）：
 *   - idx ∈ [0, count) 指向叶子内真实 extent；gap 查询与 ++/-- 基于它
 *   - idx == 0xFFFF = 虚拟前位（GLB=NONE 落点），只允许 ++（→ 本叶子首项
 *     或跨界首个非空叶子）；其余操作置 erro_flag
 *   - 块访问一律 mdata_piece_session（同 dentry_tree.cpp 零缓存直读）；
 *     跨界场景两个叶子数据并存 = 两个 session 并存（dentry_leaf_split
 *     同开三会话先例），平铺直写
 *   - 兄弟链表 ptrs[1]=前驱 / ptrs[2]=后继（仅非根叶子有效）；
 *     PTR_INVALID 位域比较截断 52 位（dentry 踩过的坑）
 *   - 根叶子（单叶子树）= 最左兼最右：next→tail_infinite、prev→prev_noexist、
 *     跨界步进→erro
 *   - 空叶子（count==0，平衡中间态防御）：跨界扫描透明跳过，不产出伪 gap
 * ═══════════════════════════════════════════════════════════════════ */


/* ═══════════════════════════════════════════════════════════════════
 * extent_iterator 实现 — 稀疏文件空洞/区间游走器
 *
 * 位置模型（头文件契约摘要）：
 *   - idx ∈ [0, count) 指向叶子内真实 extent；gap 查询与 ++/-- 基于它
 *   - idx == 0xFFFF = 虚拟前位（GLB=NONE 落点），只允许 ++（→ 本叶子首项
 *     或跨界首个非空叶子）；其余操作置 erro_flag
 *
 * 实现要点：
 *   - 块访问一律 mdata_piece_session（同 dentry_tree.cpp 零缓存直读）；
 *     跨界场景两个叶子数据并存 = 两个 session 并存（dentry_leaf_split
 *     同开三会话先例），平铺直写
 *   - 兄弟链表 ptrs[1]=前驱 / ptrs[2]=后继（仅非根叶子有效）；
 *     PTR_INVALID 位域比较截断 52 位（dentry 踩过的坑）
 *   - 根叶子（单叶子树）= 最左兼最右：next→tail_infinite、prev→prev_noexist、
 *     跨界步进→erro
 *   - 空叶子（count==0，平衡中间态防御）：跨界扫描透明跳过，不产出伪 gap
 *   - 任何失败（IO/结构/越界/非法位）：iterator_erro_flag = true 且
 *     位置保持不变（不悬垂移动）；成功操作复位 false
 * ═══════════════════════════════════════════════════════════════════ */

RavenFS_juvenile::extent_iterator::extent_iterator(RavenFS_juvenile *fs_owner,
                                                   uint64_t leaf_blk, uint16_t leaf_idx)
    : fs(fs_owner), leaf_node_blkbase(leaf_blk), idx(leaf_idx), iterator_erro_flag(false)
{
    if (!fs) iterator_erro_flag = true;
}

/* ── 读当前叶子 + 结构校验 → 本方法共用头部（失败置 erro 并 return） ── */
#define EXTENT_IT_SESS_OPEN(sess_var, blk)                                       \
    mdata_piece_session sess_var(fs->mdata_piece_mgr, (blk));                    \
    if (sess_var.index_error() || !sess_var) { iterator_erro_flag = true;        \
        return _it_fail_ret_; }                                                  \
    auto *leaf_##sess_var = (Bptree_extent_leaf_node_t *)sess_var.data;          \
    if (!is_extent_node(&leaf_##sess_var->meta) ||                                \
        RFS_PP_IS_INNER(leaf_##sess_var->meta.parent_ptr)) {                        \
        iterator_erro_flag = true; return _it_fail_ret_; }                       \
    uint16_t count_##sess_var = (uint16_t)RFS_PP_COUNT(leaf_##sess_var->meta.parent_ptr);

/* ═══════════════════════════════════════════════════════════════════
 * next_gap — 当前 extent 之后的紧邻空洞
 *   叶内        : [cur_end, extents[idx+1].fblkbase)，len 可 0（紧邻=合并信号）
 *   叶子末项跨界: 读后继叶子首项（空叶子透明跳过）→ normal
 *   树最右/单叶 : tail_infinite（interval_base = cur_end）        */
RavenFS_juvenile::gap_desicritor_t RavenFS_juvenile::extent_iterator::next_gap()
{
    gap_desicritor_t g{};                                   /* {0,0,normal} */
    const gap_desicritor_t _it_fail_ret_ = g;               /* 失败返回占位 */
    if (iterator_erro_flag || !fs) return g;
    if (idx == EXTENT_LEAF_GLB_NONE) { iterator_erro_flag = true; return g; }  /* 虚拟前位无当前 extent */

    EXTENT_IT_SESS_OPEN(sess, leaf_node_blkbase);
    if (idx >= count_sess) { iterator_erro_flag = true; return _it_fail_ret_; }  /* 槽位越界 */

    const uint64_t cur_end = leaf_sess->extents[idx].fblkbase + leaf_sess->extents[idx].length;

    if (idx + 1 < count_sess) {                             /* 叶内后继存在 */
        g.interval_base_logical_block = cur_end;
        g.gap_len = leaf_sess->extents[idx + 1].fblkbase - cur_end;  /* ≥0：排序+无重叠不变量 */
        return g;
    }
    if (RFS_PP_IS_ROOT(leaf_sess->meta.parent_ptr)) {          /* 单叶子树 = 树最右 */
        g.interval_base_logical_block = cur_end;
        g.type = tail_infinite;
        return g;
    }
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    uint64_t nb = RFS_PTR_ADDR(leaf_sess->meta.ptrs[2]);              /* 后继指针（sess 存活期取） */
    while (nb != PTR_INVALID_52) {                          /* 跨界：跳过空叶子找首个非空 */
        EXTENT_IT_SESS_OPEN(ns, nb);
        if (count_ns > 0) {
            g.interval_base_logical_block = cur_end;
            g.gap_len = leaf_ns->extents[0].fblkbase - cur_end;
            return g;
        }
        nb = RFS_PTR_ADDR(leaf_ns->meta.ptrs[2]);
    }
    g.interval_base_logical_block = cur_end;                /* 链表扫尽仍无 → 实际最右 */
    g.type = tail_infinite;
    return g;
}

/* ═══════════════════════════════════════════════════════════════════
 * prev_gap — 当前 extent 之前的紧邻空洞
 *   叶内        : [extents[idx-1] 开边界, cur.fblkbase)，len 可 0
 *   叶子首项跨界: 读前驱叶子末项（空叶子透明跳过）→ normal
 *   树最左/单叶 : prev_noexist（头部空洞调用方自理）          */
RavenFS_juvenile::gap_desicritor_t RavenFS_juvenile::extent_iterator::prev_gap()
{
    gap_desicritor_t g{};
    const gap_desicritor_t _it_fail_ret_ = g;
    if (iterator_erro_flag || !fs) return g;
    if (idx == EXTENT_LEAF_GLB_NONE) { iterator_erro_flag = true; return g; }

    EXTENT_IT_SESS_OPEN(sess, leaf_node_blkbase);
    if (idx >= count_sess) { iterator_erro_flag = true; return _it_fail_ret_; }

    const uint64_t cur_fblk = leaf_sess->extents[idx].fblkbase;

    if (idx > 0) {                                          /* 叶内前驱存在 */
        const extent_t &pe = leaf_sess->extents[idx - 1];
        g.interval_base_logical_block = pe.fblkbase + pe.length;
        g.gap_len = cur_fblk - g.interval_base_logical_block;
        return g;
    }
    if (RFS_PP_IS_ROOT(leaf_sess->meta.parent_ptr)) {          /* 单叶子树 = 树最左 */
        g.type = prev_noexist;
        return g;
    }
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    uint64_t pb = RFS_PTR_ADDR(leaf_sess->meta.ptrs[1]);              /* 前驱指针（sess 存活期取） */
    while (pb != PTR_INVALID_52) {                          /* 跨界：跳过空叶子找末项 */
        EXTENT_IT_SESS_OPEN(ps, pb);
        if (count_ps > 0) {
            const extent_t &pe = leaf_ps->extents[count_ps - 1];
            g.interval_base_logical_block = pe.fblkbase + pe.length;
            g.gap_len = cur_fblk - g.interval_base_logical_block;
            return g;
        }
        pb = RFS_PTR_ADDR(leaf_ps->meta.ptrs[1]);
    }
    g.type = prev_noexist;                                  /* 扫尽仍无前驱 extent */
    return g;
}

/* ═══════════════════════════════════════════════════════════════════
 * operator++ — 前进到下一个 extent
 *   虚拟前位 → 本叶子首项（空叶子则跨界首个非空叶子 idx=0）
 *   叶子内    → idx+1
 *   叶子末项  → 跨界后继叶子首项（空叶子透明跳过）
 *   树最右/单叶子末尾再 ++ → erro（位置不变）               */
RavenFS_juvenile::extent_iterator &RavenFS_juvenile::extent_iterator::operator++()
{
    if (iterator_erro_flag || !fs) return *this;
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);

    mdata_piece_session sess(fs->mdata_piece_mgr, leaf_node_blkbase);
    if (sess.index_error() || !sess) { iterator_erro_flag = true; return *this; }
    auto *leaf = (Bptree_extent_leaf_node_t *)sess.data;
    if (!is_extent_node(&leaf->meta) || RFS_PP_IS_INNER(leaf->meta.parent_ptr)) {
        iterator_erro_flag = true; return *this;
    }
    const uint16_t count = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);

    if (idx == EXTENT_LEAF_GLB_NONE) {                  /* 虚拟前位：目标 = 本叶子首项 */
        if (count > 0) { idx = 0; iterator_erro_flag = false; return *this; }
        /* 空叶子：无首项，落跨界路径 */
    } else {
        if (idx >= count) { iterator_erro_flag = true; return *this; }   /* 槽位越界 */
        if (idx + 1 < count) { idx++; iterator_erro_flag = false; return *this; }
    }

    if (RFS_PP_IS_ROOT(leaf->meta.parent_ptr)) {           /* 单叶子树无后继 */
        iterator_erro_flag = true; return *this;
    }
    uint64_t nb = RFS_PTR_ADDR(leaf->meta.ptrs[2]);               /* 跨界：沿后继链找非空叶子 */
    while (nb != PTR_INVALID_52) {
        mdata_piece_session ns(fs->mdata_piece_mgr, nb);
        if (ns.index_error() || !ns) { iterator_erro_flag = true; return *this; }
        auto *nl = (Bptree_extent_leaf_node_t *)ns.data;
        if (!is_extent_node(&nl->meta) || RFS_PP_IS_INNER(nl->meta.parent_ptr)) {
            iterator_erro_flag = true; return *this;
        }
        const uint16_t ncnt = (uint16_t)RFS_PP_COUNT(nl->meta.parent_ptr);
        if (ncnt > 0) {
            leaf_node_blkbase = nb;
            idx = 0;
            iterator_erro_flag = false;
            return *this;
        }
        nb = RFS_PTR_ADDR(nl->meta.ptrs[2]);
    }
    iterator_erro_flag = true;                          /* 树最右末尾再 ++：越界，位置不变 */
    return *this;
}

/* ═══════════════════════════════════════════════════════════════════
 * operator-- — 后退到上一个 extent
 *   叶子内    → idx-1
 *   叶子首项  → 跨界前驱叶子末项（空叶子透明跳过）
 *   树最左/单叶子开头再 -- → erro（位置不变）
 *   虚拟前位 -- → 非法（erro；契约：虚拟前位只允许 ++）   */
RavenFS_juvenile::extent_iterator &RavenFS_juvenile::extent_iterator::operator--()
{
    if (iterator_erro_flag || !fs) return *this;
    if (idx == EXTENT_LEAF_GLB_NONE) { iterator_erro_flag = true; return *this; }
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);

    mdata_piece_session sess(fs->mdata_piece_mgr, leaf_node_blkbase);
    if (sess.index_error() || !sess) { iterator_erro_flag = true; return *this; }
    auto *leaf = (Bptree_extent_leaf_node_t *)sess.data;
    if (!is_extent_node(&leaf->meta) || RFS_PP_IS_INNER(leaf->meta.parent_ptr)) {
        iterator_erro_flag = true; return *this;
    }
    const uint16_t count = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
    if (idx >= count) { iterator_erro_flag = true; return *this; }   /* 槽位越界 */

    if (idx > 0) { idx--; iterator_erro_flag = false; return *this; }

    if (RFS_PP_IS_ROOT(leaf->meta.parent_ptr)) {           /* 单叶子树无前驱 */
        iterator_erro_flag = true; return *this;
    }
    uint64_t pb = RFS_PTR_ADDR(leaf->meta.ptrs[1]);               /* 跨界：沿前驱链找非空叶子 */
    while (pb != PTR_INVALID_52) {
        mdata_piece_session ps(fs->mdata_piece_mgr, pb);
        if (ps.index_error() || !ps) { iterator_erro_flag = true; return *this; }
        auto *pl = (Bptree_extent_leaf_node_t *)ps.data;
        if (!is_extent_node(&pl->meta) || RFS_PP_IS_INNER(pl->meta.parent_ptr)) {
            iterator_erro_flag = true; return *this;
        }
        const uint16_t ncnt = (uint16_t)RFS_PP_COUNT(pl->meta.parent_ptr);
        if (ncnt > 0) {
            leaf_node_blkbase = pb;
            idx = (uint16_t)(ncnt - 1);
            iterator_erro_flag = false;
            return *this;
        }
        pb = RFS_PTR_ADDR(pl->meta.ptrs[1]);
    }
    iterator_erro_flag = true;                          /* 树最左开头再 --：越界，位置不变 */
    return *this;
}

RavenFS_juvenile::extent_iterator RavenFS_juvenile::extent_iterator::operator++(int)
{
    extent_iterator tmp(*this);
    ++(*this);
    return tmp;
}
RavenFS_juvenile::extent_iterator RavenFS_juvenile::extent_iterator::operator--(int)
{
    extent_iterator tmp(*this);
    --(*this);
    return tmp;
}

/* ═══════════════════════════════════════════════════════════════════
 * current_extent — 当前 extent 只读副本
 *   idx==0xFFFF（虚拟前位）→ false（无当前项，非错误，erro 不变）
 *   IO/结构失败 / 槽位越界 → erro + false                          */
bool RavenFS_juvenile::extent_iterator::current_extent(extent_t &out)
{
    if (iterator_erro_flag || !fs) return false;
    if (idx == EXTENT_LEAF_GLB_NONE) return false;

    mdata_piece_session sess(fs->mdata_piece_mgr, leaf_node_blkbase);
    if (sess.index_error() || !sess) { iterator_erro_flag = true; return false; }
    auto *leaf = (Bptree_extent_leaf_node_t *)sess.data;
    if (!is_extent_node(&leaf->meta) || RFS_PP_IS_INNER(leaf->meta.parent_ptr)) {
        iterator_erro_flag = true; return false;
    }
    const uint16_t count = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
    if (idx >= count) { iterator_erro_flag = true; return false; }

    out = leaf->extents[idx];
    return true;
}


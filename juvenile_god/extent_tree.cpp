#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>
#include <vector>
#include <algorithm>

/* ═══════════════════════════════════════════════════════════════════
 * extent_tree.cpp — RavenFS v1.1 extent B+tree 轮子区
 *
 * 本文件负责：
 *   extent_Bptree_walk / extent_inner_node_lookup / extent_leaf_node_lookup
 *   extent_iterator 全套（构造 / prev_gap / next_gap / ++ / -- / current_extent）
 *   extent_map_range / extent_unmap_range 原语对 + 内部机械（2026-09-03）
 *     （分裂/平衡/根收缩/父 key 刷新，见文件尾原语对实现区注释）
 *
 * 区段树与 dentry 树的本质差异（半开区间语义，2026-09-02 定稿）：
 *   - 叶子键   = extent.fblkbase（文件逻辑块起点），区间 [fblkbase, fblkbase+length)
 *   - 内部 key = 子树最大开边界 (fblkbase+length)，查找 target < key → 该子树
 *   - 因此内部路由是**严格上界**（第一个 key > target），dentry 的 ≥ 不可照抄：
 *     target == key 意味着 target 恰是下一棵子树的起始块，必须落右子树。
 *   - 击穿只发生在 target ≥ 全部 key（右击穿）：特化直取最后 hint 下钻最右叶子；
 *     最左边界由 lower_bound 返回 0 自然消化（同 dentry 论证，无专门分支）。
 *   - child_idx 必须 uint16_t 直赋：337 阶 hint_idx 可 > 255，
 *     dentry 的 (uint8_t) 截断是 126 阶专属，抄过来直接炸树。
 *
 * 零缓存原则：所有块访问直接 pread/pwrite，无缓存层。
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * extent_Bptree_walk — 区段树根到叶下钻（参照 dentry_Bptree_walk）
 *
 * 输入：根块号 + 目标文件逻辑块号（logical_block_idx）
 * 输出：trace
 *   - trace->blocks[depth-1] = 落点叶子块号（depth = 叶子深度）
 *   - trace->child_idx[depth-1] = 叶子内 GLB（extent_leaf_node_lookup 结果，
 *     可为 EXTENT_LEAF_GLB_NONE = 最左插入）
 *   - trace->is_leaf_hit = GLB 覆盖 target（真命中，区别于 dentry 的精准命中）
 *
 * 内部层 child_idx[d] = 路由 hint 索引（0xFFFF 仅在 dentry 语义下是哨兵；
 * 本函数内部击穿已就地收敛为最后一个 hint，落 trace 的恒为有效索引）。
 *
 * 路由不变量：target 必然 > 前驱子树最大开边界（严格上界路由保证），
 * 故叶子内无下界（GLB == NONE）时归宿 = 本叶子开头，无需向左兄弟借位。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_Bptree_walk(uint64_t root_blk_idx, bptree_path_t *trace, uint64_t logical_block_idx)
{
    if (!trace) return RC_ERR_STATE;
    trace->depth = 0;
    trace->is_leaf_hit = false;

    uint64_t cur_blk = root_blk_idx;

    for (int depth = 0; depth < 64; depth++) {   /* 深度上限=blocks[64]；超限即树有环 */
        /* 每层独立会话：缓冲随层释放；sess.data 直用，零复制 */
        mdata_piece_session sess(mdata_piece_mgr, cur_blk);
        if (sess.index_error()) return RC_ERR_STATE;
        if (!sess) return RC_ERR_IO;

        Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
        if (!is_extent_node(meta)) return RC_ERR_STATE;
        /* 根校验：第 0 层必须是根，后续层必须非根（树损坏即报） */
        if ((depth == 0) != (bool)meta->parent_ptr.is_root_node)
            return RC_ERR_STATE;

        trace->blocks[depth] = cur_blk;

        if (!meta->parent_ptr.is_inner_node) {
            /* 叶子：GLB 查找 + 覆盖检测收尾 */
            auto *leaf = (Bptree_extent_leaf_node_t *)sess.data;
            leaf_node_result_t res = extent_leaf_node_lookup(leaf, logical_block_idx);
            trace->depth = depth + 1;
            trace->is_leaf_hit = res.is_idx_hitted;
            trace->child_idx[depth] = res.greates_lower_bound_idx;
            return RC_OK;
        }

        /* 内部节点：严格上界路由（找第一个 key > target） */
        auto *node = (Bptree_extent_inner_node_t *)sess.data;
        if (node->meta.parent_ptr.entry_count == 0) return RC_ERR_STATE;
        uint16_t hint_idx = extent_inner_node_lookup(node, logical_block_idx);
        if (hint_idx == EXTENT_LEAF_GLB_NONE) {
            /* 右击穿：target ≥ 全部子树开边界。目标明确——新区间是
             * 全局最大，归宿 = 最右叶子末尾。特化：不再二分，每层
             * 直取最后一个 hint 下钻到最右叶子。 */
            hint_idx = (uint16_t)(node->meta.parent_ptr.entry_count - 1);
        }
        trace->child_idx[depth] = hint_idx;   /* uint16_t 直赋；337 阶不可截断 */
        cur_blk = node->extents[hint_idx].sub_node_ptr.ptr;
    }
    return RC_ERR_STATE;   /* 深度超限：树成环 / 指针损坏 */
}
/* ═══════════════════════════════════════════════════════════════════
 * extent_leaf_node_lookup — 叶子内 GLB 查找 + 覆盖检测
 *
 * 输入：叶子节点 + 目标文件逻辑块号
 * 输出：leaf_node_result_t
 *   greates_lower_bound_idx：fblkbase ≤ target 的最大条目索引（GLB）；
 *     EXTENT_LEAF_GLB_NONE(0xFFFF) = 无下界（本叶子所有 fblkbase > target）
 *   is_idx_hitted：GLB **覆盖** target（target - fblkbase < length，半开区间）
 *
 * 与 dentry_leaf_node_lookup 的语义差异（区间键 vs 点键）：
 *   - dentry 的 is_idx_hitted = 精准命中（hash+name 全等）；
 *   - extent 没有"相等"概念，只有"被覆盖 / 落在空洞"：
 *     hitted = target 落在 GLB 区间内（read/write 命中既有 extent），
 *     未命中 = target 在空洞或文件末尾（GLB 是插入锚点，可能与右邻合并/拆分）。
 *
 * 覆盖判定用 target - fblkbase < length 而非 fblkbase + length > target：
 * 避免 64 位加法溢出（fblkbase+length 理论可进位）。
 *
 * 算法：一次二分（extents 按 fblkbase 严格递增，区间合并后无重叠相邻不合并
 * 不变量由插入路径保证）；命中检测只需检查 GLB——若存在 fblkbase == target
 * 的条目，它必是最后一个 ≤ target 的条目，即 GLB 本身。
 * ═══════════════════════════════════════════════════════════════════ */
RavenFS_juvenile::leaf_node_result_t RavenFS_juvenile::extent_leaf_node_lookup(Bptree_extent_leaf_node_t *leaf, uint64_t target)
{
    leaf_node_result_t res;
    res.greates_lower_bound_idx = EXTENT_LEAF_GLB_NONE;
    res.is_idx_hitted = false;

    const uint16_t count = (uint16_t)leaf->meta.parent_ptr.entry_count; /* ≤337 */
    uint16_t lo = 0, hi = count;
    uint16_t glb = EXTENT_LEAF_GLB_NONE;   /* 哨兵：无下界（最左插入） */
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        if (leaf->extents[mid].fblkbase <= target) {
            glb = mid;      /* 候选下界，继续向右找更大的 */
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    res.greates_lower_bound_idx = glb;
    if (glb != EXTENT_LEAF_GLB_NONE) {
        const extent_t &e = leaf->extents[glb];
        if (target - e.fblkbase < e.length)
            res.is_idx_hitted = true;
    }
    return res;
}
/* ═══════════════════════════════════════════════════════════════════
 * extent_inner_node_lookup — 内部节点严格上界路由
 *
 * hints 按 key 递增（§10 不变量 #2），找第一个 key > target 的 hint
 * （最小严格上界）。key = 子树最大开边界 (fblkbase+length)。
 *
 * 半开区间关键差异：target == key 必须落**右**子树（target 恰为下一棵
 * 子树的最小起始块，落在其第一个区间内），故比较为 key <= target →
 * 右移（dentry 的 hash_max < target → 右移 是 ≥ 语义，不可混用）。
 *
 * 全部 key ≤ target（右击穿）→ 0xFFFF（与叶子 GLB 哨兵同值）。
 * 空节点（entry_count=0）→ 0xFFFF。
 * ═══════════════════════════════════════════════════════════════════ */
uint16_t RavenFS_juvenile::extent_inner_node_lookup(Bptree_extent_inner_node_t *node, uint64_t target)
{
    const uint16_t count = (uint16_t)node->meta.parent_ptr.entry_count;
    uint16_t lo = 0, hi = count;
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        if (node->extents[mid].key <= target)   /* target ≥ key：右半（== 属右子树） */
            lo = mid + 1;
        else
            hi = mid;
    }
    /* lo = 第一个 key > target 的位置；lo == count → 全部 ≤ target（右击穿） */
    return lo < count ? lo : 0xFFFF;
}
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
        leaf_##sess_var->meta.parent_ptr.is_inner_node) {                        \
        iterator_erro_flag = true; return _it_fail_ret_; }                       \
    uint16_t count_##sess_var = (uint16_t)leaf_##sess_var->meta.parent_ptr.entry_count;

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
    if (leaf_sess->meta.parent_ptr.is_root_node) {          /* 单叶子树 = 树最右 */
        g.interval_base_logical_block = cur_end;
        g.type = tail_infinite;
        return g;
    }
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    uint64_t nb = leaf_sess->meta.ptrs[2].ptr;              /* 后继指针（sess 存活期取） */
    while (nb != PTR_INVALID_52) {                          /* 跨界：跳过空叶子找首个非空 */
        EXTENT_IT_SESS_OPEN(ns, nb);
        if (count_ns > 0) {
            g.interval_base_logical_block = cur_end;
            g.gap_len = leaf_ns->extents[0].fblkbase - cur_end;
            return g;
        }
        nb = leaf_ns->meta.ptrs[2].ptr;
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
    if (leaf_sess->meta.parent_ptr.is_root_node) {          /* 单叶子树 = 树最左 */
        g.type = prev_noexist;
        return g;
    }
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    uint64_t pb = leaf_sess->meta.ptrs[1].ptr;              /* 前驱指针（sess 存活期取） */
    while (pb != PTR_INVALID_52) {                          /* 跨界：跳过空叶子找末项 */
        EXTENT_IT_SESS_OPEN(ps, pb);
        if (count_ps > 0) {
            const extent_t &pe = leaf_ps->extents[count_ps - 1];
            g.interval_base_logical_block = pe.fblkbase + pe.length;
            g.gap_len = cur_fblk - g.interval_base_logical_block;
            return g;
        }
        pb = leaf_ps->meta.ptrs[1].ptr;
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
    if (!is_extent_node(&leaf->meta) || leaf->meta.parent_ptr.is_inner_node) {
        iterator_erro_flag = true; return *this;
    }
    const uint16_t count = (uint16_t)leaf->meta.parent_ptr.entry_count;

    if (idx == EXTENT_LEAF_GLB_NONE) {                  /* 虚拟前位：目标 = 本叶子首项 */
        if (count > 0) { idx = 0; iterator_erro_flag = false; return *this; }
        /* 空叶子：无首项，落跨界路径 */
    } else {
        if (idx >= count) { iterator_erro_flag = true; return *this; }   /* 槽位越界 */
        if (idx + 1 < count) { idx++; iterator_erro_flag = false; return *this; }
    }

    if (leaf->meta.parent_ptr.is_root_node) {           /* 单叶子树无后继 */
        iterator_erro_flag = true; return *this;
    }
    uint64_t nb = leaf->meta.ptrs[2].ptr;               /* 跨界：沿后继链找非空叶子 */
    while (nb != PTR_INVALID_52) {
        mdata_piece_session ns(fs->mdata_piece_mgr, nb);
        if (ns.index_error() || !ns) { iterator_erro_flag = true; return *this; }
        auto *nl = (Bptree_extent_leaf_node_t *)ns.data;
        if (!is_extent_node(&nl->meta) || nl->meta.parent_ptr.is_inner_node) {
            iterator_erro_flag = true; return *this;
        }
        const uint16_t ncnt = (uint16_t)nl->meta.parent_ptr.entry_count;
        if (ncnt > 0) {
            leaf_node_blkbase = nb;
            idx = 0;
            iterator_erro_flag = false;
            return *this;
        }
        nb = nl->meta.ptrs[2].ptr;
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
    if (!is_extent_node(&leaf->meta) || leaf->meta.parent_ptr.is_inner_node) {
        iterator_erro_flag = true; return *this;
    }
    const uint16_t count = (uint16_t)leaf->meta.parent_ptr.entry_count;
    if (idx >= count) { iterator_erro_flag = true; return *this; }   /* 槽位越界 */

    if (idx > 0) { idx--; iterator_erro_flag = false; return *this; }

    if (leaf->meta.parent_ptr.is_root_node) {           /* 单叶子树无前驱 */
        iterator_erro_flag = true; return *this;
    }
    uint64_t pb = leaf->meta.ptrs[1].ptr;               /* 跨界：沿前驱链找非空叶子 */
    while (pb != PTR_INVALID_52) {
        mdata_piece_session ps(fs->mdata_piece_mgr, pb);
        if (ps.index_error() || !ps) { iterator_erro_flag = true; return *this; }
        auto *pl = (Bptree_extent_leaf_node_t *)ps.data;
        if (!is_extent_node(&pl->meta) || pl->meta.parent_ptr.is_inner_node) {
            iterator_erro_flag = true; return *this;
        }
        const uint16_t ncnt = (uint16_t)pl->meta.parent_ptr.entry_count;
        if (ncnt > 0) {
            leaf_node_blkbase = pb;
            idx = (uint16_t)(ncnt - 1);
            iterator_erro_flag = false;
            return *this;
        }
        pb = pl->meta.ptrs[1].ptr;
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
    if (!is_extent_node(&leaf->meta) || leaf->meta.parent_ptr.is_inner_node) {
        iterator_erro_flag = true; return false;
    }
    const uint16_t count = (uint16_t)leaf->meta.parent_ptr.entry_count;
    if (idx >= count) { iterator_erro_flag = true; return false; }

    out = leaf->extents[idx];
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent 原语对实现（2026-09-03 定稿）——
 *   extent_map_range   ：声明式映射（插入 + 邻接合并 + 溢出分裂级联）
 *   extent_unmap_range ：精确匹配删除单条记录（三字段全等校验 + 欠载平衡）
 *
 * 责任外显契约（对比 dentry 内敛）：
 *   - 数据块 alloc/free 全归调用方（map 的 ext.block_base 须已分配、
 *     unmap 成功后调用方自行 block_free）——本对只动树，测试可假性分配。
 *   - inline→extent 跃迁（建树根）归 direct_file_* 层：本对要求文件
 *     data_root ≠ PTR_INVALID 且 is_data_root_direct_bit==0。
 *   - 树节点块（2 块一组）内部分配/释放（分裂/平衡/根更替）。
 *   - 根更替（分裂建新根 / 根收缩）回写文件 inode.data_root。
 *
 * 与 dentry 树的结构差异：
 *   - key = 子树最大开边界 (fblkbase+length)，非 hash_max；
 *   - 合并义务：fblk 紧贴 && phys 连续的两条记录必须合一（canonical 形态）
 *     —— phys 是外部声明的事实，故合并判定在本对内部做；
 *   - 文件树根无 ".."：ptrs[1] 恒 PTR_INVALID，parent_ptr.ptr = 文件自身
 *     inode（"." 审计锚）；
 *   - 阶 337：欠载 <168（非根）、合并上界 total≤337、分裂左 168 右 count−168。
 *
 * 会话纪律：no_cache 下同块双会话各自持独立拷贝、析构写回——绝不跨会话
 * 互读修改；分裂/平衡/刷新一律先关掉本块旧会话再开新会话。
 * ═══════════════════════════════════════════════════════════════════ */

/* ── 新建 extent 节点通用初始化（整块清零 + 256-bit 魔数 + parent 四件套）；
 * ptrs[3] 按角色由调用方填（文件树根无 ".."，调用方按需写 PTR_INVALID）。 ── */
static void extent_node_meta_init(void *node_buf, uint16_t entry_count,
                                  bool is_inner, bool is_root, uint64_t parent_ptr) {
    memset(node_buf, 0, 8192);
    Bptree_meta_t *meta = (Bptree_meta_t *)node_buf;
    meta->magics[0] = BPTREE_MAGIC1;
    meta->magics[1] = BPTREE_MAGIC2;
    meta->magics[2] = BPTREE_MAGIC3;
    meta->magics[3] = BPTREE_MAGIC4;
    meta->parent_ptr.entry_count   = entry_count;
    meta->parent_ptr.is_inner_node = is_inner ? 1 : 0;
    meta->parent_ptr.is_root_node  = is_root ? 1 : 0;
    meta->parent_ptr.ptr           = parent_ptr;
}
/* 节点最大开边界 key：叶子 = 末项 fblkbase+length；内部 = 末 hint.key。
 * 空节点 → 0（仅根可空；空非根节点应已被平衡吞并，调用方不得依赖）。 */
static uint64_t extent_node_max_key(const void *node_buf) {
    const Bptree_meta_t *meta = (const Bptree_meta_t *)node_buf;
    uint16_t cnt = (uint16_t)meta->parent_ptr.entry_count;
    if (cnt == 0) return 0;
    if (meta->parent_ptr.is_inner_node) {
        const Bptree_extent_inner_node_t *inner = (const Bptree_extent_inner_node_t *)node_buf;
        return inner->extents[cnt - 1].key;
    }
    const Bptree_extent_leaf_node_t *leaf = (const Bptree_extent_leaf_node_t *)node_buf;
    return leaf->extents[cnt - 1].fblkbase + leaf->extents[cnt - 1].length;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_refresh_key_chain — 沿父链上溯刷新 hint key
 *
 * 场景：叶/子树最大开边界变更后，父 hint 中该子树的 key 必须同步
 * （上界路由正确性）；子树的 key 变更可能使父的 max 变（仅当该子树
 * 是父的末孩），继续上溯，直到 key 未变/到根即止。
 * 调用点必须保证 blk 存活（勿在平衡吞并后刷已释放块）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_refresh_key_chain(uint64_t blk)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    uint64_t cur = blk;
    for (int hop = 0; hop < 64; hop++) {   /* 深度上限；超限 = 树成环 */
        mdata_piece_session cs(mdata_piece_mgr, cur);
        if (cs.index_error()) return RC_ERR_STATE;
        if (!cs) return RC_ERR_IO;
        Bptree_meta_t *cm = (Bptree_meta_t *)cs.data;
        if (!is_extent_node(cm)) return RC_ERR_STATE;
        if (cm->parent_ptr.is_root_node) return RC_OK;   /* 根无父，链止 */
        const uint64_t new_key = extent_node_max_key(cs.data);
        const uint64_t parent_blk = cm->parent_ptr.ptr;  /* 52 位形态 */
        if (parent_blk == PTR_INVALID_52) return RC_ERR_STATE;
        mdata_piece_session ps(mdata_piece_mgr, parent_blk);
        if (ps.index_error()) return RC_ERR_STATE;
        if (!ps) return RC_ERR_IO;
        Bptree_extent_inner_node_t *parent = (Bptree_extent_inner_node_t *)ps.data;
        if (!is_extent_node(&parent->meta) || !parent->meta.parent_ptr.is_inner_node)
            return RC_ERR_STATE;
        const uint16_t pcnt = (uint16_t)parent->meta.parent_ptr.entry_count;
        uint16_t i = 0;
        bool found = false;
        for (; i < pcnt; i++) {
            if (parent->extents[i].sub_node_ptr.ptr == cur) { found = true; break; }
        }
        if (!found) return RC_ERR_STATE;
        if (parent->extents[i].key == new_key) return RC_OK;   /* 未变，链止 */
        parent->extents[i].key = new_key;
        ps.dirty();
        cur = parent_blk;
    }
    return RC_ERR_STATE;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_leaf_split — 叶分裂（337 阶：左 168 右 count−168）
 * 不修改父层；low_node_maxhash 承载左半最大开边界 key。
 * 2026-09-07：prealloc_blk = 调用方预算池提供的节点块号（map_range 按树高预分配），
 * 本函数不再 alloc → 无 ENOSPC 失败面（IO/STATE 校验保留）。
 * 链表维护（同 dentry_leaf_split）：
 *   victim(左) → new(右) → old_next；old_next.prev = new（若存在）；
 *   根叶分裂后 victim 转链表成员：无前驱 → ptrs[1]=INVALID。
 * ═══════════════════════════════════════════════════════════════════ */
RavenFS_juvenile::split_result_t RavenFS_juvenile::extent_leaf_split(uint64_t victim_blk,
                                                                     uint64_t prealloc_blk)
{
    split_result_t res;
    res.rc = RC_OK;
    res.low_node_blkidx = PTR_INVALID;
    res.high_node_blkidx = PTR_INVALID;
    res.low_node_maxhash = 0;
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);

    mdata_piece_session victim_sess(mdata_piece_mgr, victim_blk);
    if (victim_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
    if (!victim_sess) { res.rc = RC_ERR_IO; return res; }
    Bptree_extent_leaf_node_t *victim = (Bptree_extent_leaf_node_t *)victim_sess.data;
    if (!is_extent_node(&victim->meta) || victim->meta.parent_ptr.is_inner_node) {
        res.rc = RC_ERR_STATE; return res;
    }
    uint16_t count = (uint16_t)victim->meta.parent_ptr.entry_count;
    if (count <= 337) { res.rc = RC_ERR_STATE; return res; }   /* 只有超满才裂 */

    const uint64_t new_blk = prealloc_blk;   /* 预算块：调用方保证已分配 */

    /* ⚠ 旧后继在改写 victim.ptrs[2] 之前取；位域读出为 52 位形态 */
    const bool victim_is_root = victim->meta.parent_ptr.is_root_node;
    const uint64_t old_next = victim->meta.ptrs[2].ptr;

    mdata_piece_session new_sess(mdata_piece_mgr, new_blk);
    if (new_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
    if (!new_sess) { res.rc = RC_ERR_IO; return res; }
    Bptree_extent_leaf_node_t *new_leaf = (Bptree_extent_leaf_node_t *)new_sess.data;
    extent_node_meta_init(new_sess.data, (uint16_t)(count - 168), false, false,
                          victim->meta.parent_ptr.ptr);   /* 同父（暂态：根裂时父值由上层 reparent 修正） */
    new_leaf->meta.ptrs[0].ptr = PTR_INVALID;
    new_leaf->meta.ptrs[1].ptr = victim_blk;                       /* prev = victim */
    new_leaf->meta.ptrs[2].ptr = victim_is_root ? PTR_INVALID : old_next;

    victim->meta.parent_ptr.entry_count = 168;
    victim->meta.ptrs[2].ptr = new_blk;
    if (victim_is_root) {
        victim->meta.ptrs[1].ptr = PTR_INVALID;   /* 无前驱；文件树根叶本无 ".." */
    }

    memcpy(new_leaf->extents, &victim->extents[168],
           (size_t)(count - 168) * sizeof(extent_t));   /* 右半 → 新叶 */

    if (!victim_is_root && old_next != PTR_INVALID_52) {
        mdata_piece_session next_sess(mdata_piece_mgr, old_next);
        if (next_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
        if (!next_sess) { res.rc = RC_ERR_IO; return res; }
        Bptree_extent_leaf_node_t *next_node = (Bptree_extent_leaf_node_t *)next_sess.data;
        next_node->meta.ptrs[1].ptr = new_blk;
        next_sess.dirty();
    }

    victim_sess.dirty();
    new_sess.dirty();

    res.low_node_blkidx  = victim_blk;
    res.high_node_blkidx = new_blk;
    res.low_node_maxhash = victim->extents[167].fblkbase + victim->extents[167].length;
    return res;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_inner_split — 内部节点分裂（337 阶：左 168 右 count−168）
 * 内部节点无兄弟链表；右半孩子全部 reparent 到新块（否则上溯走错父）。
 * 2026-09-07：prealloc_blk = 预算池节点块号，本函数不再 alloc（无 ENOSPC 面）。
 * ═══════════════════════════════════════════════════════════════════ */
RavenFS_juvenile::split_result_t RavenFS_juvenile::extent_inner_split(uint64_t victim_blk,
                                                                      uint64_t prealloc_blk)
{
    split_result_t res;
    res.rc = RC_OK;
    res.low_node_blkidx = PTR_INVALID;
    res.high_node_blkidx = PTR_INVALID;
    res.low_node_maxhash = 0;
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);

    mdata_piece_session victim_sess(mdata_piece_mgr, victim_blk);
    if (victim_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
    if (!victim_sess) { res.rc = RC_ERR_IO; return res; }
    Bptree_extent_inner_node_t *victim = (Bptree_extent_inner_node_t *)victim_sess.data;
    if (!is_extent_node(&victim->meta) || !victim->meta.parent_ptr.is_inner_node) {
        res.rc = RC_ERR_STATE; return res;
    }
    uint16_t count = (uint16_t)victim->meta.parent_ptr.entry_count;
    if (count <= 337) { res.rc = RC_ERR_STATE; return res; }

    const uint64_t new_blk = prealloc_blk;   /* 预算块：调用方保证已分配 */

    mdata_piece_session new_sess(mdata_piece_mgr, new_blk);
    if (new_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
    if (!new_sess) { res.rc = RC_ERR_IO; return res; }
    Bptree_extent_inner_node_t *new_inner = (Bptree_extent_inner_node_t *)new_sess.data;
    extent_node_meta_init(new_sess.data, (uint16_t)(count - 168), true, false,
                          victim->meta.parent_ptr.ptr);   /* 同父 */
    new_inner->meta.ptrs[0].ptr = PTR_INVALID;
    new_inner->meta.ptrs[1].ptr = PTR_INVALID;
    new_inner->meta.ptrs[2].ptr = PTR_INVALID;

    victim->meta.parent_ptr.entry_count = 168;
    const size_t copy_count = count - 168;
    memcpy(new_inner->extents, &victim->extents[168], copy_count * sizeof(extent_hint_t));

    /* ⚠ 新内部节点接管右半孩子：孩子 parent_ptr 改指 new_blk */
    for (size_t i = 0; i < copy_count; i++) {
        uint64_t child_blk = new_inner->extents[i].sub_node_ptr.ptr;
        if (child_blk == PTR_INVALID_52) { res.rc = RC_ERR_STATE; return res; }
        mdata_piece_session c_sess(mdata_piece_mgr, child_blk);
        if (c_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
        if (!c_sess) { res.rc = RC_ERR_IO; return res; }
        Bptree_meta_t *cmeta = (Bptree_meta_t *)c_sess.data;
        if (!is_extent_node(cmeta)) { res.rc = RC_ERR_STATE; return res; }
        if (cmeta->parent_ptr.is_root_node) { res.rc = RC_ERR_STATE; return res; }
        cmeta->parent_ptr.ptr = new_blk;
        c_sess.dirty();
    }

    victim_sess.dirty();
    new_sess.dirty();

    res.low_node_blkidx  = victim_blk;
    res.high_node_blkidx = new_blk;
    res.low_node_maxhash = victim->extents[167].key;   /* 左半最大开边界 */
    return res;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_spilt_parent_apply — 分裂结果应用到父层
 *   hints[victim_idx].key = 左半 key；victim_idx+1 处插入右孩 hint
 *   （key 现算）；父 count+1 超 337 → parent_spilt=true（需递归分裂）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_spilt_parent_apply(
    uint64_t parent_blk, uint16_t victim_idx, split_result_t r, bool &parent_spilt)
{
    if (r.rc != RC_OK) return r.rc;
    parent_spilt = false;

    mdata_piece_session parent_sess(mdata_piece_mgr, parent_blk);
    if (parent_sess.index_error()) return RC_ERR_STATE;
    if (!parent_sess) return RC_ERR_IO;
    Bptree_extent_inner_node_t *parent = (Bptree_extent_inner_node_t *)parent_sess.data;
    if (!is_extent_node(&parent->meta) || !parent->meta.parent_ptr.is_inner_node)
        return RC_ERR_STATE;
    uint16_t count = (uint16_t)parent->meta.parent_ptr.entry_count;
    if (victim_idx >= count) return RC_ERR_STATE;
    if (parent->extents[victim_idx].sub_node_ptr.ptr != r.low_node_blkidx)
        return RC_ERR_STATE;

    mdata_piece_session child_sess(mdata_piece_mgr, r.high_node_blkidx);
    if (child_sess.index_error()) return RC_ERR_STATE;
    if (!child_sess) return RC_ERR_IO;
    if (!is_extent_node((Bptree_meta_t *)child_sess.data)) return RC_ERR_STATE;
    const uint64_t high_key = extent_node_max_key(child_sess.data);

    parent->extents[victim_idx].key = r.low_node_maxhash;   /* 左半 key（分裂不增大左半 max） */

    const uint16_t insert_pos = (uint16_t)(victim_idx + 1);
    if (insert_pos < count) {
        memmove(&parent->extents[insert_pos + 1], &parent->extents[insert_pos],
                (size_t)(count - insert_pos) * sizeof(extent_hint_t));
    }
    parent->extents[insert_pos].key = high_key;
    parent->extents[insert_pos].sub_node_ptr.ptr = r.high_node_blkidx;
    parent->extents[insert_pos].sub_node_ptr.free_to_use = 0;
    parent->meta.parent_ptr.entry_count = count + 1;

    if (count + 1 > 337) parent_spilt = true;
    parent_sess.dirty();
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_new_root_inner — 分裂后建新根内部节点（文件树专用）
 * 低/高两子提升为新根 hints[0]/[1]；两子 parent_ptr 收敛到新根；
 * 新根块号经 root_blk& 带出（detach，2026-09-06：不再回写 inode.data_root，
 * 提交权归调用方）。根 meta：parent_ptr.ptr = owner_inode_idx（"." 审计锚），ptrs[1] 恒 PTR_INVALID（文件树无 ".."）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_new_root_inner(uint64_t low_blk, uint64_t low_key,
                                                      uint64_t high_blk,
                                                      uint64_t owner_inode_idx,
                                                      uint64_t &root_blk,
                                                      uint64_t prealloc_blk)
{
    const uint64_t new_root_blk = prealloc_blk;   /* 预算块：调用方保证已分配 */

    mdata_piece_session root_sess(mdata_piece_mgr, new_root_blk);
    if (root_sess.index_error()) return RC_ERR_STATE;
    if (!root_sess) return RC_ERR_IO;
    Bptree_extent_inner_node_t *new_root = (Bptree_extent_inner_node_t *)root_sess.data;
    extent_node_meta_init(root_sess.data, 2, true, true, owner_inode_idx);   /* "." 审计锚 */
    new_root->meta.ptrs[1].ptr = PTR_INVALID;   /* 无 ".." */

    new_root->extents[0].key = low_key;
    new_root->extents[0].sub_node_ptr.ptr = low_blk;
    new_root->extents[0].sub_node_ptr.free_to_use = 0;

    mdata_piece_session right_sess(mdata_piece_mgr, high_blk);
    if (right_sess.index_error()) return RC_ERR_STATE;
    if (!right_sess) return RC_ERR_IO;
    if (!is_extent_node((Bptree_meta_t *)right_sess.data)) return RC_ERR_STATE;
    new_root->extents[1].key = extent_node_max_key(right_sess.data);
    new_root->extents[1].sub_node_ptr.ptr = high_blk;
    new_root->extents[1].sub_node_ptr.free_to_use = 0;

    /* 两子去 root 标志、父指新根 */
    mdata_piece_session left_sess(mdata_piece_mgr, low_blk);
    if (left_sess.index_error()) return RC_ERR_STATE;
    if (!left_sess) return RC_ERR_IO;
    Bptree_meta_t *lm = (Bptree_meta_t *)left_sess.data;
    if (!is_extent_node(lm)) return RC_ERR_STATE;
    lm->parent_ptr.ptr = new_root_blk;
    lm->parent_ptr.is_root_node = 0;
    left_sess.dirty();

    Bptree_meta_t *rm = (Bptree_meta_t *)right_sess.data;
    rm->parent_ptr.ptr = new_root_blk;
    rm->parent_ptr.is_root_node = 0;
    right_sess.dirty();

    root_sess.dirty();

    /* 根更迭：新根块号沿引用带出（不碰 inode；提交 = 调用方写 data_root） */
    root_blk = new_root_blk;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_node_balance — 平衡（合并或抹匀），针对父中 idx 指向的欠载子节点
 * 规则（同 dentry）：头部（idx==0）向右看，非头部向左看；
 *   total ≤ 337 合并（保留 child、free neighbor），否则抹匀（total/2 分）。
 * 返回 need_balance = 父删 hint 后欠载（非根 count<168），需上层继续上溯。
 * 父是根内部且只剩 1 孩 → RC_ERR_STATE（根收缩由上层 extent_balance_from
 * 处理）。合并/抹匀后父内 hint key 就地更新；更上层 key 由调用方
 * extent_refresh_key_chain 兜底。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_node_balance(
    uint64_t parent_blk, uint16_t idx, bool &need_balance)
{
    need_balance = false;
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);

    auto node_max_key = [](const void *buf) -> uint64_t {
        const Bptree_meta_t *m = (const Bptree_meta_t *)buf;
        uint16_t cnt = (uint16_t)m->parent_ptr.entry_count;
        if (cnt == 0) return 0;
        if (m->parent_ptr.is_inner_node) {
            const Bptree_extent_inner_node_t *n = (const Bptree_extent_inner_node_t *)buf;
            return n->extents[cnt - 1].key;
        }
        const Bptree_extent_leaf_node_t *l = (const Bptree_extent_leaf_node_t *)buf;
        return l->extents[cnt - 1].fblkbase + l->extents[cnt - 1].length;
    };
    auto remove_parent_hint = [](Bptree_extent_inner_node_t *parent, uint16_t del) {
        uint16_t cnt = (uint16_t)parent->meta.parent_ptr.entry_count;
        if (del < cnt - 1) {
            memmove(&parent->extents[del], &parent->extents[del + 1],
                    (size_t)(cnt - del - 1) * sizeof(extent_hint_t));
        }
        parent->meta.parent_ptr.entry_count = cnt - 1;
    };
    auto reparent_children = [&](Bptree_extent_inner_node_t *node,
                                 uint64_t node_blk, uint16_t cnt) -> result_code_t {
        for (uint16_t i = 0; i < cnt; i++) {
            uint64_t cblk = node->extents[i].sub_node_ptr.ptr;
            if (cblk == PTR_INVALID_52) return RC_ERR_STATE;
            mdata_piece_session c_sess(mdata_piece_mgr, cblk);
            if (c_sess.index_error()) return RC_ERR_STATE;
            if (!c_sess) return RC_ERR_IO;
            Bptree_meta_t *cm = (Bptree_meta_t *)c_sess.data;
            if (!is_extent_node(cm)) return RC_ERR_STATE;
            if (cm->parent_ptr.is_root_node) return RC_ERR_STATE;
            if (cm->parent_ptr.ptr != node_blk) {
                cm->parent_ptr.ptr = node_blk;
                c_sess.dirty();
            }
        }
        return RC_OK;
    };

    /* ── 1. 父节点 ── */
    mdata_piece_session parent_sess(mdata_piece_mgr, parent_blk);
    if (parent_sess.index_error()) return RC_ERR_STATE;
    if (!parent_sess) return RC_ERR_IO;
    Bptree_extent_inner_node_t *parent = (Bptree_extent_inner_node_t *)parent_sess.data;
    if (!is_extent_node(&parent->meta) || !parent->meta.parent_ptr.is_inner_node)
        return RC_ERR_STATE;
    uint16_t parent_count = (uint16_t)parent->meta.parent_ptr.entry_count;
    if (idx >= parent_count) return RC_ERR_STATE;

    /* ── 2. 目标子节点 ── */
    uint64_t child_blk = parent->extents[idx].sub_node_ptr.ptr;
    mdata_piece_session child_sess(mdata_piece_mgr, child_blk);
    if (child_sess.index_error()) return RC_ERR_STATE;
    if (!child_sess) return RC_ERR_IO;
    Bptree_meta_t *child_meta = (Bptree_meta_t *)child_sess.data;
    if (!is_extent_node(child_meta)) return RC_ERR_STATE;
    uint16_t child_count = (uint16_t)child_meta->parent_ptr.entry_count;
    bool is_inner = child_meta->parent_ptr.is_inner_node;
    if (child_count >= 168) return RC_OK;   /* 已健康 */

    /* ── 3. 邻居方向：头部向右，其余向左 ── */
    uint16_t neighbor_idx;
    if (idx == 0) {
        if (parent_count == 1) return RC_ERR_STATE;   /* 根单孩 → 上层收缩 */
        neighbor_idx = 1;
    } else {
        neighbor_idx = (uint16_t)(idx - 1);
    }
    uint64_t neighbor_blk = parent->extents[neighbor_idx].sub_node_ptr.ptr;
    mdata_piece_session neighbor_sess(mdata_piece_mgr, neighbor_blk);
    if (neighbor_sess.index_error()) return RC_ERR_STATE;
    if (!neighbor_sess) return RC_ERR_IO;
    Bptree_meta_t *neighbor_meta = (Bptree_meta_t *)neighbor_sess.data;
    if (!is_extent_node(neighbor_meta)) return RC_ERR_STATE;
    if (neighbor_meta->parent_ptr.is_inner_node != is_inner) return RC_ERR_STATE;
    uint16_t neighbor_count = (uint16_t)neighbor_meta->parent_ptr.entry_count;
    uint16_t total = (uint16_t)(child_count + neighbor_count);

    /* ── 4. total ≤ 337：合并（保留 child，free neighbor） ── */
    if (total <= 337) {
        if (is_inner) {
            Bptree_extent_inner_node_t *child = (Bptree_extent_inner_node_t *)child_sess.data;
            Bptree_extent_inner_node_t *neighbor = (Bptree_extent_inner_node_t *)neighbor_sess.data;
            if (idx == 0) {
                /* child 左、neighbor 右：追加到末尾 */
                memcpy(&child->extents[child_count], neighbor->extents,
                       neighbor_count * sizeof(extent_hint_t));
            } else {
                /* child 右、neighbor 左：邻居插到开头 */
                memmove(&child->extents[neighbor_count], &child->extents[0],
                        child_count * sizeof(extent_hint_t));
                memcpy(&child->extents[0], neighbor->extents,
                       neighbor_count * sizeof(extent_hint_t));
            }
            child->meta.parent_ptr.entry_count = total;
            result_code_t rr = reparent_children(child, child_blk, total);
            if (rr != RC_OK) return rr;
            child_sess.dirty();
        } else {
            Bptree_extent_leaf_node_t *child = (Bptree_extent_leaf_node_t *)child_sess.data;
            Bptree_extent_leaf_node_t *neighbor = (Bptree_extent_leaf_node_t *)neighbor_sess.data;
            if (idx == 0) {
                /* child 左、neighbor 右：追加 + 链后继接管 */
                memcpy(&child->extents[child_count], neighbor->extents,
                       neighbor_count * sizeof(extent_t));
                child->meta.ptrs[2].ptr = neighbor->meta.ptrs[2].ptr;
                uint64_t next_blk = neighbor->meta.ptrs[2].ptr;
                if (next_blk != PTR_INVALID_52) {
                    mdata_piece_session next_sess(mdata_piece_mgr, next_blk);
                    if (next_sess.index_error()) return RC_ERR_STATE;
                    if (!next_sess) return RC_ERR_IO;
                    Bptree_extent_leaf_node_t *next_node = (Bptree_extent_leaf_node_t *)next_sess.data;
                    next_node->meta.ptrs[1].ptr = child_blk;
                    next_sess.dirty();
                }
            } else {
                /* child 右、neighbor 左：邻居插到开头 + 链前驱接管 */
                memmove(&child->extents[neighbor_count], &child->extents[0],
                        child_count * sizeof(extent_t));
                memcpy(&child->extents[0], neighbor->extents,
                       neighbor_count * sizeof(extent_t));
                child->meta.ptrs[1].ptr = neighbor->meta.ptrs[1].ptr;
                uint64_t prev_blk = neighbor->meta.ptrs[1].ptr;
                if (prev_blk != PTR_INVALID_52) {
                    mdata_piece_session prev_sess(mdata_piece_mgr, prev_blk);
                    if (prev_sess.index_error()) return RC_ERR_STATE;
                    if (!prev_sess) return RC_ERR_IO;
                    Bptree_extent_leaf_node_t *prev_node = (Bptree_extent_leaf_node_t *)prev_sess.data;
                    prev_node->meta.ptrs[2].ptr = child_blk;
                    prev_sess.dirty();
                }
            }
            child->meta.parent_ptr.entry_count = total;
            child_sess.dirty();
        }

        block_free(neighbor_blk, 2);   /* 节点恒 2 FS_block */

        /* 父：删邻居 hint，保留的 child hint key 更新为合并后 max */
        uint16_t del_idx = (idx == 0) ? 1 : (uint16_t)(idx - 1);
        remove_parent_hint(parent, del_idx);
        uint64_t new_max = node_max_key(child_sess.data);
        uint16_t kept_before = (idx == 0) ? 0 : (uint16_t)(idx - 1);
        uint16_t kept_after = kept_before;
        if (del_idx < kept_before) kept_after--;
        parent->extents[kept_after].key = new_max;

        uint16_t npc = (uint16_t)parent->meta.parent_ptr.entry_count;
        bool parent_is_root = parent->meta.parent_ptr.is_root_node;
        if (!parent_is_root && npc < 168) need_balance = true;
        parent_sess.dirty();
        return RC_OK;
    }

    /* ── 5. total > 337：抹匀（左 total/2 右余） ── */
    uint16_t left_count = (uint16_t)(total / 2);
    uint16_t right_count = (uint16_t)(total - left_count);
    if (is_inner) {
        Bptree_extent_inner_node_t *child = (Bptree_extent_inner_node_t *)child_sess.data;
        Bptree_extent_inner_node_t *neighbor = (Bptree_extent_inner_node_t *)neighbor_sess.data;
        extent_hint_t merged[506];
        if (idx == 0) {
            memcpy(merged, child->extents, child_count * sizeof(extent_hint_t));
            memcpy(merged + child_count, neighbor->extents, neighbor_count * sizeof(extent_hint_t));
            memcpy(child->extents, merged, left_count * sizeof(extent_hint_t));
            memcpy(neighbor->extents, merged + left_count, right_count * sizeof(extent_hint_t));
            child->meta.parent_ptr.entry_count = left_count;
            neighbor->meta.parent_ptr.entry_count = right_count;
            result_code_t rr = reparent_children(child, child_blk, left_count);
            if (rr != RC_OK) return rr;
            rr = reparent_children(neighbor, neighbor_blk, right_count);
            if (rr != RC_OK) return rr;
        } else {
            memcpy(merged, neighbor->extents, neighbor_count * sizeof(extent_hint_t));
            memcpy(merged + neighbor_count, child->extents, child_count * sizeof(extent_hint_t));
            memcpy(neighbor->extents, merged, left_count * sizeof(extent_hint_t));
            memcpy(child->extents, merged + left_count, right_count * sizeof(extent_hint_t));
            neighbor->meta.parent_ptr.entry_count = left_count;
            child->meta.parent_ptr.entry_count = right_count;
            result_code_t rr = reparent_children(neighbor, neighbor_blk, left_count);
            if (rr != RC_OK) return rr;
            rr = reparent_children(child, child_blk, right_count);
            if (rr != RC_OK) return rr;
        }
        child_sess.dirty();
        neighbor_sess.dirty();
    } else {
        Bptree_extent_leaf_node_t *child = (Bptree_extent_leaf_node_t *)child_sess.data;
        Bptree_extent_leaf_node_t *neighbor = (Bptree_extent_leaf_node_t *)neighbor_sess.data;
        extent_t merged[506];
        if (idx == 0) {
            memcpy(merged, child->extents, child_count * sizeof(extent_t));
            memcpy(merged + child_count, neighbor->extents, neighbor_count * sizeof(extent_t));
            memcpy(child->extents, merged, left_count * sizeof(extent_t));
            memcpy(neighbor->extents, merged + left_count, right_count * sizeof(extent_t));
            child->meta.parent_ptr.entry_count = left_count;
            neighbor->meta.parent_ptr.entry_count = right_count;
        } else {
            memcpy(merged, neighbor->extents, neighbor_count * sizeof(extent_t));
            memcpy(merged + neighbor_count, child->extents, child_count * sizeof(extent_t));
            memcpy(neighbor->extents, merged, left_count * sizeof(extent_t));
            memcpy(child->extents, merged + left_count, right_count * sizeof(extent_t));
            neighbor->meta.parent_ptr.entry_count = left_count;
            child->meta.parent_ptr.entry_count = right_count;
        }
        /* 兄弟链表不变（两节点都存活，链序不变） */
        child_sess.dirty();
        neighbor_sess.dirty();
    }

    /* 抹匀不删 hint：两个 hint key 就地更新为各自新 max */
    uint16_t nb_idx = (idx == 0) ? 1 : (uint16_t)(idx - 1);
    parent->extents[idx].key = node_max_key(child_sess.data);
    parent->extents[nb_idx].key = node_max_key(neighbor_sess.data);
    need_balance = false;
    parent_sess.dirty();
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_root_shrink — 根收缩：根内部只剩 1 孩时提升该孩为新根
 * root_blk& in/out：入 = 旧根，收缩成功时出 = 提升后的新根（detach，2026-09-06，
 * 不写 inode）。孩子 is_root=1、parent_ptr.ptr = owner_inode_idx（"." 审计锚）；
 * 叶子清兄弟链（文件树根无 ".." → ptrs 全 INVALID）；释放旧根 2 块。
 * 返回：RC_OK（成功或无需收缩：非内部/entry_count≠1）/ RC_ERR_STATE（非根）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_root_shrink(uint64_t &root_blk, uint64_t owner_inode_idx)
{
    const uint64_t old_root = root_blk;   /* 引用入参：收缩前先捕获旧根块号 */
    mdata_piece_session root_sess(mdata_piece_mgr, old_root);
    if (root_sess.index_error()) return RC_ERR_STATE;
    if (!root_sess) return RC_ERR_IO;
    Bptree_extent_inner_node_t *root = (Bptree_extent_inner_node_t *)root_sess.data;
    if (!is_extent_node(&root->meta)) return RC_ERR_STATE;
    if (!root->meta.parent_ptr.is_root_node) return RC_ERR_STATE;
    if (!root->meta.parent_ptr.is_inner_node) return RC_OK;
    uint16_t cnt = (uint16_t)root->meta.parent_ptr.entry_count;
    if (cnt != 1) return RC_OK;

    uint64_t child_blk = root->extents[0].sub_node_ptr.ptr;
    mdata_piece_session child_sess(mdata_piece_mgr, child_blk);
    if (child_sess.index_error()) return RC_ERR_STATE;
    if (!child_sess) return RC_ERR_IO;
    Bptree_meta_t *cm = (Bptree_meta_t *)child_sess.data;
    if (!is_extent_node(cm)) return RC_ERR_STATE;
    if (cm->parent_ptr.is_root_node) return RC_ERR_STATE;   /* 孩子已是根：结构损坏 */

    cm->parent_ptr.is_root_node = 1;
    cm->parent_ptr.ptr = owner_inode_idx;   /* "." 审计锚 */
    if (!cm->parent_ptr.is_inner_node) {
        Bptree_extent_leaf_node_t *cl = (Bptree_extent_leaf_node_t *)child_sess.data;
        cl->meta.ptrs[0].ptr = PTR_INVALID;   /* 清前驱（根无链表） */
        cl->meta.ptrs[1].ptr = PTR_INVALID;   /* 文件树根无 ".." */
        cl->meta.ptrs[2].ptr = PTR_INVALID;   /* 清后继 */
    } else {
        Bptree_extent_inner_node_t *ci = (Bptree_extent_inner_node_t *)child_sess.data;
        ci->meta.ptrs[1].ptr = PTR_INVALID;   /* 无 ".."（保险） */
    }
    child_sess.dirty();

    /* 根更迭：新根（原孩）块号沿引用带出（不碰 inode；提交 = 调用方写 data_root） */
    root_blk = child_blk;
    block_free(old_root, 2);
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_balance_from — 欠载链修复（自下而上，父指针驱动，非 trace 依赖）
 *   读子：根 →（根内部 count==1 残留 → 收缩）链止；
 *         非根欠载（count<168）→ 父中定位 → extent_node_balance
 *         （RC_ERR_STATE = 父为根单孩 → 收缩提升）→ 父 key 上溯刷新
 *         → need 则子=父继续，否则查父是否根内单孩残留并收尾。
 * 供 unmap 删记录欠载路径使用（map 2026-09-06 降级为纯插不融合后，
 * 无"跨界右并致邻叶欠载"路径；未来 merge_range 收敛致欠载亦复用本链）。
 * detach（2026-09-06）：root_blk& in/out 穿透 —— 根收缩发生时把新根带回调用方；
 * owner_inode_idx 仅传给收缩侧写 "." 锚。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_balance_from(uint64_t child_blk, uint64_t owner_inode_idx,
                                                    uint64_t &root_blk)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    uint64_t cur = child_blk;
    for (int hop = 0; hop < 64; hop++) {
        mdata_piece_session cs(mdata_piece_mgr, cur);
        if (cs.index_error()) return RC_ERR_STATE;
        if (!cs) return RC_ERR_IO;
        Bptree_meta_t *cm = (Bptree_meta_t *)cs.data;
        if (!is_extent_node(cm)) return RC_ERR_STATE;
        const uint16_t cnt = (uint16_t)cm->parent_ptr.entry_count;

        if (cm->parent_ptr.is_root_node) {
            if (cm->parent_ptr.is_inner_node && cnt == 1)
                return extent_root_shrink(root_blk, owner_inode_idx);   /* 合并残留收尾（cur 即根） */
            return RC_OK;                                    /* 根叶/健康根内：链止 */
        }
        if (cnt >= 168) return RC_OK;                        /* 已健康 */

        const uint64_t parent_blk = cm->parent_ptr.ptr;      /* 52 位形态 */
        if (parent_blk == PTR_INVALID_52) return RC_ERR_STATE;

        mdata_piece_session ps(mdata_piece_mgr, parent_blk);
        if (ps.index_error()) return RC_ERR_STATE;
        if (!ps) return RC_ERR_IO;
        Bptree_extent_inner_node_t *parent = (Bptree_extent_inner_node_t *)ps.data;
        if (!is_extent_node(&parent->meta) || !parent->meta.parent_ptr.is_inner_node)
            return RC_ERR_STATE;
        const uint16_t pcnt = (uint16_t)parent->meta.parent_ptr.entry_count;
        uint16_t idx = 0;
        bool found = false;
        for (; idx < pcnt; idx++) {
            if (parent->extents[idx].sub_node_ptr.ptr == cur) { found = true; break; }
        }
        if (!found) return RC_ERR_STATE;

        bool need = false;
        result_code_t rc = extent_node_balance(parent_blk, idx, need);
        if (rc == RC_ERR_STATE) {
            /* 父是根内部且只剩此一孩 → 无邻居可平衡，收缩提升（含空叶升根） */
            return extent_root_shrink(root_blk, owner_inode_idx);
        }
        if (rc) return rc;

        rc = extent_refresh_key_chain(parent_blk);   /* 平衡后父自身 max 可能变 */
        if (rc) return rc;

        if (!need) {
            /* 收尾：合并路径可能让根内部残留 count==1 */
            mdata_piece_session rps(mdata_piece_mgr, parent_blk);
            if (rps.index_error()) return RC_ERR_STATE;
            if (!rps) return RC_ERR_IO;
            Bptree_meta_t *rm = (Bptree_meta_t *)rps.data;
            if (rm->parent_ptr.is_root_node && rm->parent_ptr.is_inner_node &&
                rm->parent_ptr.entry_count == 1)
                return extent_root_shrink(root_blk, owner_inode_idx);
            return RC_OK;
        }
        cur = parent_blk;   /* 父欠载 → 继续上溯 */
    }
    return RC_ERR_STATE;   /* 深度超限：树成环 */
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_map_range — 声明式映射原语（插侧 · 2026-09-06 定案：有界左融合）
 *
 * 演进史：
 *   - 2026-09-03 内建自动融合（左并/右并/跨界）→ F 组测试实证不完备
 *     （f1d 跨界左并盲区 / F2 融合致欠载叶不回收）→ 7bde35c 降级为纯插；
 *   - 2026-09-06 定案（演进史见 Docs/pwrite_pipeline.md §1/§2）：map 只左融合（同叶
 *     GLB 单对）+ 全量融合归激进重建（extent_rebuild_tree）。
 *
 * 左融合语义（有界单对，不承诺完备）：
 *   插入前查 GLB —— 逻辑紧贴（GLB.end == ext.fblkbase）&& phys 连续
 *   （GLB.phys_end == ext.block_base.ptr）→ 不插新记录，GLB.length += 段长。
 *   count 不变、无腾槽 → 无超满分裂可能 → 零新增失败路径；不删记录 →
 *   无欠载制造路径（与旧融合死因无交集）。顺序写（occupy 尾巴延展）下
 *   记录数 ≈ phys run 数，不随写次数涨。
 *   刻意不做：entry[0]（GLB=NONE）跨界左并——跨叶要动前叶 count/max +
 *   父 hint 刷新链，不值；f1d 形态残留显式接受，交激进重建兑底。
 *
 * 语义：声明 [ext.fblkbase, ext.fblkbase+ext.length) 由 ext.block_base 起
 * length 个连续 FS_block 背书（纯记录层，不碰数据侧位图）。重叠防御双向：
 *   - 落点 GLB 覆盖 ext.fblkbase（起点落入既有区间）→ RC_ERR_OVERLAP；
 *   - 区间终点盖右邻起点（ext.end > 右邻.fblkbase）→ RC_ERR_OVERLAP
 *     （左并/纯插共用：扩长后终点同样不得盖右邻）。
 * 紧贴合法（半开区间无交集）；紧贴+连续走左融合，其余残留合法。
 *
 * 内部：左融合命中 = 零结构变化（仅 GLB 为叶末项时叶 max 变 → 上刷父链，
 * 复用 insert 尾插的 extent_refresh_key_chain）；否则纯插（GLB 后 memmove
 * 腾槽）→ count+1 → 超满（>337）分裂级联 → 父 key 上溯刷新。
 *
 * 外显契约（detach，2026-09-06）：root_blk in/out（根裂建新根经引用带出）；
 * owner_inode_idx 纯审计锚（根 meta "."）；不碰 inode。调用方保证 root_blk
 * 指向合法 extent 树根（inline→extent 跃迁归 direct_file_* 层）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_map_range(uint64_t &root_blk, uint64_t owner_inode_idx,
                                                 const extent_t &ext)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    if (ext.length == 0) return RC_OK;   /* 空映射：无操作 */

    bptree_path_t trace;
    result_code_t rc = extent_Bptree_walk(root_blk, &trace, ext.fblkbase);
    if (rc) return rc;
    const uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    const uint16_t glb = trace.child_idx[trace.depth - 1];
    const bool hit = trace.is_leaf_hit;

    bool need_split = false;
    bool did_split = false;
    uint64_t split_high_leaf = 0;
    bool left_merged = false;       /* 左融合命中（同叶 GLB 扩长，count 不变） */
    bool left_key_changed = false;  /* 左融合对象为叶末项 → 叶 max 变 → 需刷父链 */
    std::vector<uint64_t> reserve_pool;   /* 分裂预算池（2026-09-07 map 原子性） */
    size_t pool_used = 0;

    {
        mdata_piece_session sess(mdata_piece_mgr, leaf_blk);
        if (sess.index_error()) return RC_ERR_STATE;
        if (!sess) return RC_ERR_IO;
        Bptree_extent_leaf_node_t *leaf = (Bptree_extent_leaf_node_t *)sess.data;
        if (!is_extent_node(&leaf->meta) || leaf->meta.parent_ptr.is_inner_node)
            return RC_ERR_STATE;
        uint16_t count = (uint16_t)leaf->meta.parent_ptr.entry_count;

        if (hit) return RC_ERR_OVERLAP;   /* 起点被 GLB 覆盖（区间重叠） */

        /* 右邻防御（左并/纯插共用：无论扩长还是新插，终点都不得盖右邻起点） */
        const uint16_t pos = (glb == EXTENT_LEAF_GLB_NONE) ? 0 : (uint16_t)(glb + 1);
        if (pos < count) {
            const extent_t r = leaf->extents[pos];
            if (ext.fblkbase + ext.length > r.fblkbase)
                return RC_ERR_OVERLAP;
        }

        /* ── 左融合（2026-09-06 定案）：GLB 紧贴 && phys 连续 → 扩长不插记录。
         * 有界单对：不承诺完备、不删记录（无欠载路径）；GLB=NONE（entry[0]
         * 落点）刻意跳过跨界左并（跨叶成本不值，残留交激进重建兑底）。 ── */
        if (glb != EXTENT_LEAF_GLB_NONE) {
            const extent_t &g = leaf->extents[glb];
            if (g.fblkbase + g.length == ext.fblkbase &&
                g.block_base.ptr + g.length == ext.block_base.ptr) {
                leaf->extents[glb].length += ext.length;  /* 零结构变化：count/槽位不动 */
                left_key_changed = (glb == count - 1);    /* 叶末项 → 子树 max 变 */
                left_merged = true;
                sess.dirty();
            }
        }

        if (!left_merged) {
            /* ── 分裂预算（2026-09-07 map 原子性）：满叶（337）纯插必裂 → 按树高预分配
             * 整条分裂链节点：depth+1 = 叶裂 1 + 每内层至多 1 + 根裂建新根 1。
             * alloc 在树改动（memmove）之前 → 失败 = free 已分配 + 返回，树零改动、
             * 真原子失败（级联中途 ENOSPC 留孤儿的 C 决策缺口对 map 关闭）。
             * 不满叶插入零预算；左并分支零预算。 ── */
            if (count == 337) {
                reserve_pool.reserve((size_t)trace.depth + 1);
                for (int i = 0; i <= trace.depth; i++) {
                    const uint64_t b = block_alloc_hard(2, 1);
                    if (b == PTR_INVALID) {
                        for (uint64_t x : reserve_pool) block_free(x, 2);
                        return RC_ERR_NO_BLOCK;   /* 未 dirty：会话析构不写回，树零改动 */
                    }
                    reserve_pool.push_back(b);
                }
            }
            /* ── 纯插（不融合）：GLB 后腾槽插入 ── */
            if (pos < count) {
                memmove(&leaf->extents[pos + 1], &leaf->extents[pos],
                        (size_t)(count - pos) * sizeof(extent_t));
            }
            leaf->extents[pos] = ext;
            count++;
            leaf->meta.parent_ptr.entry_count = count;

            need_split = count > 337;
            sess.dirty();
        }
    }   /* 叶会话析构写回；分裂/刷链前必须关（同块双会话不互见修改） */

    if (left_merged) {
        /* 零结构变化收尾：count 未增 → 无分裂路径；GLB 非叶末 → 叶 max 未变，
         * 父链零刷新。仅 GLB 为叶末项时叶 max 变 → 复用 insert 尾插刷链路径。 */
        if (left_key_changed) {
            rc = extent_refresh_key_chain(leaf_blk);
            if (rc) return rc;
        }
        return RC_OK;
    }

    /* ── 分裂级联 + 父 key 刷新（lambda：错误经 return 统一走预算归还收尾） ── */
    auto split_and_refresh = [&]() -> result_code_t {
        if (need_split) {
            split_result_t sr = extent_leaf_split(leaf_blk, reserve_pool[pool_used++]);
            if (sr.rc != RC_OK) return sr.rc;
            did_split = true;
            split_high_leaf = sr.high_node_blkidx;
            if (trace.depth == 1) {
                rc = extent_new_root_inner(sr.low_node_blkidx, sr.low_node_maxhash,
                                           sr.high_node_blkidx, owner_inode_idx, root_blk,
                                           reserve_pool[pool_used++]);
                if (rc) return rc;
            } else {
                uint64_t parent_blk = trace.blocks[trace.depth - 2];
                uint16_t child_idx = trace.child_idx[trace.depth - 2];
                bool parent_spilt = false;
                rc = extent_spilt_parent_apply(parent_blk, child_idx, sr, parent_spilt);
                if (rc) return rc;
                uint64_t cur_parent = parent_blk;
                int guard = 0;
                while (parent_spilt) {
                    if (++guard > 64) return RC_ERR_STATE;
                    mdata_piece_session ps(mdata_piece_mgr, cur_parent);
                    if (ps.index_error()) return RC_ERR_STATE;
                    if (!ps) return RC_ERR_IO;
                    Bptree_extent_inner_node_t *pn = (Bptree_extent_inner_node_t *)ps.data;
                    if (!is_extent_node(&pn->meta) || !pn->meta.parent_ptr.is_inner_node)
                        return RC_ERR_STATE;
                    const bool parent_is_root = pn->meta.parent_ptr.is_root_node;
                    const uint64_t grand_blk = pn->meta.parent_ptr.ptr;
                    split_result_t isr = extent_inner_split(cur_parent, reserve_pool[pool_used++]);
                    if (isr.rc != RC_OK) return isr.rc;
                    if (parent_is_root) {
                        rc = extent_new_root_inner(isr.low_node_blkidx, isr.low_node_maxhash,
                                                   isr.high_node_blkidx, owner_inode_idx,
                                                   root_blk, reserve_pool[pool_used++]);
                        if (rc) return rc;
                        break;
                    }
                    if (grand_blk == PTR_INVALID_52) return RC_ERR_STATE;
                    mdata_piece_session gs(mdata_piece_mgr, grand_blk);
                    if (gs.index_error()) return RC_ERR_STATE;
                    if (!gs) return RC_ERR_IO;
                    Bptree_extent_inner_node_t *grand = (Bptree_extent_inner_node_t *)gs.data;
                    if (!is_extent_node(&grand->meta) || !grand->meta.parent_ptr.is_inner_node)
                        return RC_ERR_STATE;
                    const uint16_t gcnt = (uint16_t)grand->meta.parent_ptr.entry_count;
                    uint16_t grand_idx = 0;
                    bool found = false;
                    for (; grand_idx < gcnt; grand_idx++) {
                        if (grand->extents[grand_idx].sub_node_ptr.ptr == cur_parent) {
                            found = true; break;
                        }
                    }
                    if (!found) return RC_ERR_STATE;
                    rc = extent_spilt_parent_apply(grand_blk, grand_idx, isr, parent_spilt);
                    if (rc) return rc;
                    cur_parent = grand_blk;
                }
            }
        }

        /* 父 key 刷新：从本叶上溯；分裂后两半叶都刷。 */
        rc = extent_refresh_key_chain(leaf_blk);
        if (rc) return rc;
        if (did_split) {
            rc = extent_refresh_key_chain(split_high_leaf);
            if (rc) return rc;
        }
        return RC_OK;
    };
    rc = split_and_refresh();

    /* 预算归还（统一收尾）：级联实际消耗 pool_used 个（多数情况只裂一层 → 大部
     * 分预算归还；最坏全链分裂 depth+1 个全部消耗）。错误路径同样在此归还。 */
    for (; pool_used < reserve_pool.size(); pool_used++)
        block_free(reserve_pool[pool_used], 2);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_unmap_range — 撤销映射原语（原语对·删侧）
 *
 * 外部侦查契约：ext 必须与树内一条记录**三字段全等**
 * （fblkbase / length / block_base.ptr），任一不符 = 侦查过期/错误 →
 * RC_ERR_STATE（防错删——物理块由外部按侦查结果 free，错删即错 free）。
 * 起点无记录覆盖（walk 未命中）→ RC_ERR_NOT_FOUND。
 * 只做整条记录删除；裁头/裁尾/中裂由外部按"unmap+map 序列"表达。
 * 内部：叶内 memmove 删除 → 欠载（非根 count<168）→ extent_balance_from
 * （含根收缩链）；否则父 key 上溯刷新（删末项 max 变时）。
 * 外显：记录删除后数据块 free 归外部（测试可假性跳过）。
 * detach（2026-09-06）：root_blk in/out —— 欠载链根收缩经引用带出新根；
 * 不碰 inode；owner_inode_idx 仅传给平衡侧写 "." 锚。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_unmap_range(uint64_t &root_blk, uint64_t owner_inode_idx,
                                                   const extent_t &ext)
{
    if (ext.length == 0) return RC_OK;   /* 空区间：无操作 */

    bptree_path_t trace;
    result_code_t rc = extent_Bptree_walk(root_blk, &trace, ext.fblkbase);
    if (rc) return rc;
    if (!trace.is_leaf_hit) return RC_ERR_NOT_FOUND;   /* 起点无记录覆盖：目标不存在 */
    const uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    const uint16_t glb = trace.child_idx[trace.depth - 1];

    bool is_root = false;
    bool underflow = false;
    {
        mdata_piece_session sess(mdata_piece_mgr, leaf_blk);
        if (sess.index_error()) return RC_ERR_STATE;
        if (!sess) return RC_ERR_IO;
        Bptree_extent_leaf_node_t *leaf = (Bptree_extent_leaf_node_t *)sess.data;
        if (!is_extent_node(&leaf->meta) || leaf->meta.parent_ptr.is_inner_node)
            return RC_ERR_STATE;
        uint16_t count = (uint16_t)leaf->meta.parent_ptr.entry_count;
        if (glb == EXTENT_LEAF_GLB_NONE || glb >= count) return RC_ERR_STATE;

        const extent_t rec = leaf->extents[glb];   /* packed 成员禁引用绑定 → 拷贝 */
        if (rec.fblkbase != ext.fblkbase || rec.length != ext.length ||
            rec.block_base.ptr != ext.block_base.ptr)
            return RC_ERR_STATE;   /* 三字段全等校验失败：侦查过期/错误 */

        if (glb < count - 1) {
            memmove(&leaf->extents[glb], &leaf->extents[glb + 1],
                    (size_t)(count - glb - 1) * sizeof(extent_t));
        }
        memset(&leaf->extents[count - 1], 0, sizeof(extent_t));
        count--;
        leaf->meta.parent_ptr.entry_count = count;
        sess.dirty();

        is_root = leaf->meta.parent_ptr.is_root_node;
        underflow = (!is_root && count < 168);
    }   /* 会话析构写回；平衡/刷新前必须关（同块双会话不互见修改） */

    if (is_root) return RC_OK;   /* 根叶欠载/空均合法，且无父 key */
    if (underflow) return extent_balance_from(leaf_blk, owner_inode_idx, root_blk);
    return extent_refresh_key_chain(leaf_blk);   /* 仅叶 max 变时上溯（删末项/删空） */
}

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

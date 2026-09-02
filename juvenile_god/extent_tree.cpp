#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */

/* ═══════════════════════════════════════════════════════════════════
 * extent_tree.cpp — RavenFS v1.1 extent B+tree 轮子区
 *
 * 本文件负责：
 *   extent_Bptree_walk / extent_inner_node_lookup / extent_leaf_node_lookup
 *   extent_iterator 全套（构造 / prev_gap / next_gap / ++ / -- / current_extent）
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

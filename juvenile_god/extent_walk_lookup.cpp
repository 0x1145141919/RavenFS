#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>
#include <vector>
#include <algorithm>

/* ═══════════════════════════════════════════════════════════════════
 * extent_walk_lookup.cpp — 区段树只读导航（原 extent_tree.cpp 拆出，2026-09-07）
 *
 * 本文件负责：extent_Bptree_walk（根到叶下钻）/ extent_leaf_node_lookup
 * （叶内 GLB + 覆盖检测）/ extent_inner_node_lookup（内部严格上界路由）。
 * 零写盘零分配；map/unmap/iterator/rebuild 的落点侦查都建立在本层之上。
 *
 * 区段树与 dentry 树的本质差异（半开区间语义，2026-09-02 定稿）——
 * 全轮子共享的键语义总纲：
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

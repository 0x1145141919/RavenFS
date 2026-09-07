#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>
#include <vector>
#include <algorithm>

/* ═══════════════════════════════════════════════════════════════════
 * extent_split.cpp — 区段树分裂域 + 节点基建（原 extent_tree.cpp 拆出）
 *
 * 本文件负责：
 *   extent_node_meta_init / extent_node_max_key —— 类静态节点基建
 *     （2026-09-07 拆文件升格：跨 split/rebuild 两域使用，原文件级 static
 *     无法跨翻译单元，定义随主要消费者留本文件）
 *   extent_refresh_key_chain —— 节点 max key 变更后沿父链上溯刷新
 *   extent_leaf_split / extent_inner_split / extent_spilt_parent_apply /
 *   extent_new_root_inner —— 337 阶分裂三件套 + 建根
 *
 * 2026-09-07 预算化契约：分裂函数不再 alloc —— 节点块号由调用方预分配
 * 传入（prealloc_blk），调用方按树高预算整条分裂链（depth+1）→ 分裂级联
 * 无 ENOSPC 面；预算用不完由调用方收尾归还（map 原子性闭环）。
 *
 * 会话纪律（全 extent 轮子共享）：no_cache 下同块双会话各自持独立拷贝、
 * 析构写回——绝不跨会话互读修改；分裂/平衡/刷新一律先关掉本块旧会话
 * 再开新会话。分裂不修改父层；low_node_maxhash 承载左半最大开边界 key。
 * ═══════════════════════════════════════════════════════════════════ */


/* ── 新建 extent 节点通用初始化（整块清零 + 256-bit 魔数 + parent 四件套）；
 * ptrs[3] 按角色由调用方填（文件树根无 ".."，调用方按需写 PTR_INVALID）。 ── */
void RavenFS_juvenile::extent_node_meta_init(void *node_buf, uint16_t entry_count,
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
uint64_t RavenFS_juvenile::extent_node_max_key(const void *node_buf) {
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


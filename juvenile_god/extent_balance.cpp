#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>
#include <vector>
#include <algorithm>

/* ═══════════════════════════════════════════════════════════════════
 * extent_balance.cpp — 区段树欠载平衡域（原 extent_tree.cpp 拆出）
 *
 * 本文件负责：extent_node_balance（合并或抹匀）/ extent_root_shrink
 * （根收缩提升）/ extent_balance_from（欠载链自下而上修复）。
 * unmap/rebuild 删侧与重建侧的树形收敛都走本域。
 *
 * detach 契约（2026-09-06）：root_blk& in/out 穿透 —— 根收缩发生时把
 * 新根带回调用方；owner_inode_idx 仅传给收缩侧写 "." 审计锚，零 inode I/O。
 *
 * 阶 337：欠载 <168（非根）、合并上界 total≤337。
 * ═══════════════════════════════════════════════════════════════════ */


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
    uint64_t parent_blk, uint16_t idx, bool &need_balance, extent_tree_kind_t kind)
{
    need_balance = false;
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);

    auto node_max_key = [](const void *buf) -> uint64_t {
        const Bptree_meta_t *m = (const Bptree_meta_t *)buf;
        uint16_t cnt = (uint16_t)RFS_PP_COUNT(m->parent_ptr);
        if (cnt == 0) return 0;
        if (RFS_PP_IS_INNER(m->parent_ptr)) {
            const Bptree_extent_inner_node_t *n = (const Bptree_extent_inner_node_t *)buf;
            return n->extents[cnt - 1].key;
        }
        const Bptree_extent_leaf_node_t *l = (const Bptree_extent_leaf_node_t *)buf;
        return l->extents[cnt - 1].fblkbase + l->extents[cnt - 1].length;
    };
    auto remove_parent_hint = [](Bptree_extent_inner_node_t *parent, uint16_t del) {
        uint16_t cnt = (uint16_t)RFS_PP_COUNT(parent->meta.parent_ptr);
        if (del < cnt - 1) {
            memmove(&parent->extents[del], &parent->extents[del + 1],
                    (size_t)(cnt - del - 1) * sizeof(extent_hint_t));
        }
        RFS_PP_SET_COUNT(parent->meta.parent_ptr, cnt - 1);
    };
    auto reparent_children = [&](Bptree_extent_inner_node_t *node,
                                 uint64_t node_blk, uint16_t cnt) -> result_code_t {
        for (uint16_t i = 0; i < cnt; i++) {
            uint64_t cblk = RFS_PTR_ADDR(node->extents[i].sub_node_ptr);
            if (cblk == PTR_INVALID_52) return RC_ERR_STATE;
            mdata_piece_session c_sess(mdata_piece_mgr, cblk);
            if (c_sess.index_error()) return RC_ERR_STATE;
            if (!c_sess) return RC_ERR_IO;
            Bptree_meta_t *cm = (Bptree_meta_t *)c_sess.data;
            if (!is_extent_node_kind(cm, kind)) return RC_ERR_STATE;
            if (RFS_PP_IS_ROOT(cm->parent_ptr)) return RC_ERR_STATE;
            if (RFS_PP_PTR(cm->parent_ptr) != node_blk) {
                RFS_PP_SET_PTR(cm->parent_ptr, node_blk);
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
    if (!is_extent_node_kind(&parent->meta, kind) || !RFS_PP_IS_INNER(parent->meta.parent_ptr))
        return RC_ERR_STATE;
    uint16_t parent_count = (uint16_t)RFS_PP_COUNT(parent->meta.parent_ptr);
    if (idx >= parent_count) return RC_ERR_STATE;

    /* ── 2. 目标子节点 ── */
    uint64_t child_blk = RFS_PTR_ADDR(parent->extents[idx].sub_node_ptr);
    mdata_piece_session child_sess(mdata_piece_mgr, child_blk);
    if (child_sess.index_error()) return RC_ERR_STATE;
    if (!child_sess) return RC_ERR_IO;
    Bptree_meta_t *child_meta = (Bptree_meta_t *)child_sess.data;
    if (!is_extent_node_kind(child_meta, kind)) return RC_ERR_STATE;
    uint16_t child_count = (uint16_t)RFS_PP_COUNT(child_meta->parent_ptr);
    bool is_inner = RFS_PP_IS_INNER(child_meta->parent_ptr);
    if (child_count >= 168) return RC_OK;   /* 已健康 */

    /* ── 3. 邻居方向：头部向右，其余向左 ── */
    uint16_t neighbor_idx;
    if (idx == 0) {
        if (parent_count == 1) return RC_ERR_STATE;   /* 根单孩 → 上层收缩 */
        neighbor_idx = 1;
    } else {
        neighbor_idx = (uint16_t)(idx - 1);
    }
    uint64_t neighbor_blk = RFS_PTR_ADDR(parent->extents[neighbor_idx].sub_node_ptr);
    mdata_piece_session neighbor_sess(mdata_piece_mgr, neighbor_blk);
    if (neighbor_sess.index_error()) return RC_ERR_STATE;
    if (!neighbor_sess) return RC_ERR_IO;
    Bptree_meta_t *neighbor_meta = (Bptree_meta_t *)neighbor_sess.data;
    if (!is_extent_node_kind(neighbor_meta, kind)) return RC_ERR_STATE;
    if (RFS_PP_IS_INNER(neighbor_meta->parent_ptr) != is_inner) return RC_ERR_STATE;
    uint16_t neighbor_count = (uint16_t)RFS_PP_COUNT(neighbor_meta->parent_ptr);
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
            RFS_PP_SET_COUNT(child->meta.parent_ptr, total);
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
                RFS_PTR_SET_ADDR(child->meta.ptrs[2], RFS_PTR_ADDR(neighbor->meta.ptrs[2]));
                uint64_t next_blk = RFS_PTR_ADDR(neighbor->meta.ptrs[2]);
                if (next_blk != PTR_INVALID_52) {
                    mdata_piece_session next_sess(mdata_piece_mgr, next_blk);
                    if (next_sess.index_error()) return RC_ERR_STATE;
                    if (!next_sess) return RC_ERR_IO;
                    Bptree_extent_leaf_node_t *next_node = (Bptree_extent_leaf_node_t *)next_sess.data;
                    RFS_PTR_SET_ADDR(next_node->meta.ptrs[1], child_blk);
                    next_sess.dirty();
                }
            } else {
                /* child 右、neighbor 左：邻居插到开头 + 链前驱接管 */
                memmove(&child->extents[neighbor_count], &child->extents[0],
                        child_count * sizeof(extent_t));
                memcpy(&child->extents[0], neighbor->extents,
                       neighbor_count * sizeof(extent_t));
                RFS_PTR_SET_ADDR(child->meta.ptrs[1], RFS_PTR_ADDR(neighbor->meta.ptrs[1]));
                uint64_t prev_blk = RFS_PTR_ADDR(neighbor->meta.ptrs[1]);
                if (prev_blk != PTR_INVALID_52) {
                    mdata_piece_session prev_sess(mdata_piece_mgr, prev_blk);
                    if (prev_sess.index_error()) return RC_ERR_STATE;
                    if (!prev_sess) return RC_ERR_IO;
                    Bptree_extent_leaf_node_t *prev_node = (Bptree_extent_leaf_node_t *)prev_sess.data;
                    RFS_PTR_SET_ADDR(prev_node->meta.ptrs[2], child_blk);
                    prev_sess.dirty();
                }
            }
            RFS_PP_SET_COUNT(child->meta.parent_ptr, total);
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

        uint16_t npc = (uint16_t)RFS_PP_COUNT(parent->meta.parent_ptr);
        bool parent_is_root = RFS_PP_IS_ROOT(parent->meta.parent_ptr);
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
            RFS_PP_SET_COUNT(child->meta.parent_ptr, left_count);
            RFS_PP_SET_COUNT(neighbor->meta.parent_ptr, right_count);
            result_code_t rr = reparent_children(child, child_blk, left_count);
            if (rr != RC_OK) return rr;
            rr = reparent_children(neighbor, neighbor_blk, right_count);
            if (rr != RC_OK) return rr;
        } else {
            memcpy(merged, neighbor->extents, neighbor_count * sizeof(extent_hint_t));
            memcpy(merged + neighbor_count, child->extents, child_count * sizeof(extent_hint_t));
            memcpy(neighbor->extents, merged, left_count * sizeof(extent_hint_t));
            memcpy(child->extents, merged + left_count, right_count * sizeof(extent_hint_t));
            RFS_PP_SET_COUNT(neighbor->meta.parent_ptr, left_count);
            RFS_PP_SET_COUNT(child->meta.parent_ptr, right_count);
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
            RFS_PP_SET_COUNT(child->meta.parent_ptr, left_count);
            RFS_PP_SET_COUNT(neighbor->meta.parent_ptr, right_count);
        } else {
            memcpy(merged, neighbor->extents, neighbor_count * sizeof(extent_t));
            memcpy(merged + neighbor_count, child->extents, child_count * sizeof(extent_t));
            memcpy(neighbor->extents, merged, left_count * sizeof(extent_t));
            memcpy(child->extents, merged + left_count, right_count * sizeof(extent_t));
            RFS_PP_SET_COUNT(neighbor->meta.parent_ptr, left_count);
            RFS_PP_SET_COUNT(child->meta.parent_ptr, right_count);
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
result_code_t RavenFS_juvenile::extent_root_shrink(uint64_t &root_blk, uint64_t owner_inode_idx,
                                                   extent_tree_kind_t kind)
{
    const uint64_t old_root = root_blk;   /* 引用入参：收缩前先捕获旧根块号 */
    mdata_piece_session root_sess(mdata_piece_mgr, old_root);
    if (root_sess.index_error()) return RC_ERR_STATE;
    if (!root_sess) return RC_ERR_IO;
    Bptree_extent_inner_node_t *root = (Bptree_extent_inner_node_t *)root_sess.data;
    if (!is_extent_node_kind(&root->meta, kind)) return RC_ERR_STATE;
    if (!RFS_PP_IS_ROOT(root->meta.parent_ptr)) return RC_ERR_STATE;
    if (!RFS_PP_IS_INNER(root->meta.parent_ptr)) return RC_OK;
    uint16_t cnt = (uint16_t)RFS_PP_COUNT(root->meta.parent_ptr);
    if (cnt != 1) return RC_OK;

    uint64_t child_blk = RFS_PTR_ADDR(root->extents[0].sub_node_ptr);
    mdata_piece_session child_sess(mdata_piece_mgr, child_blk);
    if (child_sess.index_error()) return RC_ERR_STATE;
    if (!child_sess) return RC_ERR_IO;
    Bptree_meta_t *cm = (Bptree_meta_t *)child_sess.data;
    if (!is_extent_node_kind(cm, kind)) return RC_ERR_STATE;
    if (RFS_PP_IS_ROOT(cm->parent_ptr)) return RC_ERR_STATE;   /* 孩子已是根：结构损坏 */

    RFS_PP_SET_IS_ROOT(cm->parent_ptr, 1);
    RFS_PP_SET_PTR(cm->parent_ptr, owner_inode_idx);   /* "." 审计锚 */
    if (!RFS_PP_IS_INNER(cm->parent_ptr)) {
        Bptree_extent_leaf_node_t *cl = (Bptree_extent_leaf_node_t *)child_sess.data;
        RFS_PTR_SET_ADDR(cl->meta.ptrs[0], PTR_INVALID);   /* 清前驱（根无链表） */
        RFS_PTR_SET_ADDR(cl->meta.ptrs[1], PTR_INVALID);   /* 文件树根无 ".." */
        RFS_PTR_SET_ADDR(cl->meta.ptrs[2], PTR_INVALID);   /* 清后继 */
    } else {
        Bptree_extent_inner_node_t *ci = (Bptree_extent_inner_node_t *)child_sess.data;
        RFS_PTR_SET_ADDR(ci->meta.ptrs[1], PTR_INVALID);   /* 无 ".."（保险） */
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
                                                    uint64_t &root_blk, extent_tree_kind_t kind)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    uint64_t cur = child_blk;
    for (int hop = 0; hop < 64; hop++) {
        mdata_piece_session cs(mdata_piece_mgr, cur);
        if (cs.index_error()) return RC_ERR_STATE;
        if (!cs) return RC_ERR_IO;
        Bptree_meta_t *cm = (Bptree_meta_t *)cs.data;
        if (!is_extent_node_kind(cm, kind)) return RC_ERR_STATE;
        const uint16_t cnt = (uint16_t)RFS_PP_COUNT(cm->parent_ptr);

        if (RFS_PP_IS_ROOT(cm->parent_ptr)) {
            if (RFS_PP_IS_INNER(cm->parent_ptr) && cnt == 1)
                return extent_root_shrink(root_blk, owner_inode_idx, kind);   /* 合并残留收尾（cur 即根） */
            return RC_OK;                                    /* 根叶/健康根内：链止 */
        }
        if (cnt >= 168) return RC_OK;                        /* 已健康 */

        const uint64_t parent_blk = RFS_PP_PTR(cm->parent_ptr);      /* 52 位形态 */
        if (parent_blk == PTR_INVALID_52) return RC_ERR_STATE;

        mdata_piece_session ps(mdata_piece_mgr, parent_blk);
        if (ps.index_error()) return RC_ERR_STATE;
        if (!ps) return RC_ERR_IO;
        Bptree_extent_inner_node_t *parent = (Bptree_extent_inner_node_t *)ps.data;
        if (!is_extent_node_kind(&parent->meta, kind) || !RFS_PP_IS_INNER(parent->meta.parent_ptr))
            return RC_ERR_STATE;
        const uint16_t pcnt = (uint16_t)RFS_PP_COUNT(parent->meta.parent_ptr);
        uint16_t idx = 0;
        bool found = false;
        for (; idx < pcnt; idx++) {
            if (RFS_PTR_ADDR(parent->extents[idx].sub_node_ptr) == cur) { found = true; break; }
        }
        if (!found) return RC_ERR_STATE;

        bool need = false;
        result_code_t rc = extent_node_balance(parent_blk, idx, need, kind);
        if (rc == RC_ERR_STATE) {
            /* 父是根内部且只剩此一孩 → 无邻居可平衡，收缩提升（含空叶升根） */
            return extent_root_shrink(root_blk, owner_inode_idx, kind);
        }
        if (rc) return rc;

        rc = extent_refresh_key_chain(parent_blk, kind);   /* 平衡后父自身 max 可能变 */
        if (rc) return rc;

        if (!need) {
            /* 收尾：合并路径可能让根内部残留 count==1 */
            mdata_piece_session rps(mdata_piece_mgr, parent_blk);
            if (rps.index_error()) return RC_ERR_STATE;
            if (!rps) return RC_ERR_IO;
            Bptree_meta_t *rm = (Bptree_meta_t *)rps.data;
            if (RFS_PP_IS_ROOT(rm->parent_ptr) && RFS_PP_IS_INNER(rm->parent_ptr) &&
                RFS_PP_COUNT(rm->parent_ptr) == 1)
                return extent_root_shrink(root_blk, owner_inode_idx, kind);
            return RC_OK;
        }
        cur = parent_blk;   /* 父欠载 → 继续上溯 */
    }
    return RC_ERR_STATE;   /* 深度超限：树成环 */
}


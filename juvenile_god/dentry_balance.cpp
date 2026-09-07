#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>

/* ═══════════════════════════════════════════════════════════════════
 * dentry_balance.cpp — dentry B+tree 欠载平衡域（原 dentry_tree.cpp 拆出）
 *
 * 本文件负责：dentry_node_balance（合并或再分配）/ dentry_root_shrink
 * （根收缩：根内部节点只剩 1 孩时提升该孩为新根）。
 *
 * 平衡规则：头部（idx==0）只向右看；非头部只向左看；
 * total ≤ 126 合并，否则再分配。根收缩由上层调用者驱动
 * （本文件 node_balance 遇根收缩情形返回 RC_ERR_STATE 上抛）。
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * dentry_node_balance — 平衡（合并或再分配），针对父节点中 idx 指向的子节点
 *
 * 新规则：
 *   - 头部（idx==0）：只向右看（与 idx+1 邻居操作）
 *   - 非头部（idx>0）：只向左看（与 idx-1 邻居操作）
 *   - 若 total <= 126 则合并，否则再分配
 *
 * 返回：
 *   is_parent_need_balance = true 表示父节点删除了 hint，可能欠载需递归处理
 *   = false 表示父节点仅更新了 hash_max，无需递归
 *
 * 根节点收缩（父节点为根且只剩一个孩子）由上层调用者处理，
 * 本函数遇到此情形返回 RC_ERR_STATE。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dentry_node_balance(
    uint64_t parent_node_blkbase,
    uint16_t idx,
    bool &is_parent_need_balance)
{
    is_parent_need_balance = false;

    /* 位域读出的指针是 52 位形态，与 PTR_INVALID 比较必须同样截断 */
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);

    // ---------- 辅助工具 ----------
    // 获取节点末项 hash（叶子 entry 或内部 hint）
    auto get_node_max_hash = [](const void *node_buf, bool is_inner) -> uint64_t {
        const Bptree_meta_t *meta = (const Bptree_meta_t *)node_buf;
        uint16_t cnt = (uint16_t)meta->parent_ptr.entry_count;
        if (cnt == 0) return 0;
        if (is_inner) {
            const Bptree_dentry_inner_node_t *inner = (const Bptree_dentry_inner_node_t *)node_buf;
            return inner->hints[cnt - 1].hash_max;
        } else {
            const Bptree_dentry_leaf_node_t *leaf = (const Bptree_dentry_leaf_node_t *)node_buf;
            return leaf->entries[cnt - 1].hash;
        }
    };

    // 更新父节点中指定 hint 的 hash_max
    auto update_parent_hint = [](Bptree_dentry_inner_node_t *parent, uint16_t hint_idx, uint64_t new_hash) {
        parent->hints[hint_idx].hash_max = new_hash;
    };

    // 从父节点删除指定 hint（memmove 左移）
    auto remove_parent_hint = [](Bptree_dentry_inner_node_t *parent, uint16_t del_idx) {
        uint16_t cnt = (uint16_t)parent->meta.parent_ptr.entry_count;
        if (del_idx < cnt - 1) {
            memmove(&parent->hints[del_idx], &parent->hints[del_idx + 1],
                    (cnt - del_idx - 1) * sizeof(Dentry_hint_t));
        }
        parent->meta.parent_ptr.entry_count = cnt - 1;
    };

    // ⚠ 孩子 parent_ptr 收敛：合并/抹匀后节点归属可能变化，
    // 将 node 的 cnt 个 hint 指向的孩子 parent_ptr 统一改为 node_blk。
    // （叶子无孩子，仅内部节点调用；只读-改-写 parent_ptr 变化的，IO 最小化）
    auto reparent_children = [&](Bptree_dentry_inner_node_t *node,
                                 uint64_t node_blk, uint16_t cnt) -> result_code_t {
        for (uint16_t i = 0; i < cnt; i++) {
            uint64_t cblk = node->hints[i].sub_ptr.ptr;
            if (cblk == PTR_INVALID_52) return RC_ERR_STATE;
            mdata_piece_session c_sess(mdata_piece_mgr, cblk);
            if (c_sess.index_error()) return RC_ERR_STATE;
            if (!c_sess) return RC_ERR_IO;
            Bptree_meta_t *cm = (Bptree_meta_t *)c_sess.data;
            if (!is_dentry_node(cm)) return RC_ERR_STATE;
            if (cm->parent_ptr.is_root_node) return RC_ERR_STATE;
            if (cm->parent_ptr.ptr != node_blk) {
                cm->parent_ptr.ptr = node_blk;
                c_sess.dirty();
            }
        }
        return RC_OK;
    };

    // ---------- 1. 读父节点 ----------
    mdata_piece_session parent_sess(mdata_piece_mgr, parent_node_blkbase);
    if (parent_sess.index_error()) return RC_ERR_STATE;
    if (!parent_sess) return RC_ERR_IO;
    Bptree_dentry_inner_node_t *parent = (Bptree_dentry_inner_node_t *)parent_sess.data;
    if (!is_dentry_node(&parent->meta)) return RC_ERR_STATE;
    if (!parent->meta.parent_ptr.is_inner_node) return RC_ERR_STATE;

    uint16_t parent_count = (uint16_t)parent->meta.parent_ptr.entry_count;
    if (idx >= parent_count) return RC_ERR_STATE;

    // ---------- 2. 读目标子节点 ----------
    uint64_t child_blk = parent->hints[idx].sub_ptr.ptr;
    mdata_piece_session child_sess(mdata_piece_mgr, child_blk);
    if (child_sess.index_error()) return RC_ERR_STATE;
    if (!child_sess) return RC_ERR_IO;
    Bptree_meta_t *child_meta = (Bptree_meta_t *)child_sess.data;
    if (!is_dentry_node(child_meta)) return RC_ERR_STATE;
    uint16_t child_count = (uint16_t)child_meta->parent_ptr.entry_count;
    bool is_inner = child_meta->parent_ptr.is_inner_node;

    // 若子节点已健康（>=63），无需平衡
    if (child_count >= 63) return RC_OK;

    // ---------- 3. 确定邻居方向 ----------
    uint16_t neighbor_idx;  // 父节点中邻居的索引
    if (idx == 0) {
        // 头部：只能向右看
        if (parent_count == 1) {
            // 根节点收缩应由上层处理
            return RC_ERR_STATE;
        }
        neighbor_idx =  1;
    } else {
        // 非头部（含尾部）：向左看
        neighbor_idx = idx - 1;
    }
    uint64_t neighbor_blk = parent->hints[neighbor_idx].sub_ptr.ptr;
    mdata_piece_session neighbor_sess(mdata_piece_mgr, neighbor_blk);
    if (neighbor_sess.index_error()) return RC_ERR_STATE;
    if (!neighbor_sess) return RC_ERR_IO;
    Bptree_meta_t *neighbor_meta = (Bptree_meta_t *)neighbor_sess.data;
    if (!is_dentry_node(neighbor_meta)) return RC_ERR_STATE;
    if (neighbor_meta->parent_ptr.is_inner_node != is_inner) return RC_ERR_STATE;
    uint16_t neighbor_count = (uint16_t)neighbor_meta->parent_ptr.entry_count;

    uint16_t total = child_count + neighbor_count;

        // ---------- 4. 如果 total <= 126：合并 ----------
    if (total <= 126) {
        if (is_inner) {
            // ---- 内部节点合并 ----
            // 目标与邻居均为内部节点，无兄弟链表
            Bptree_dentry_inner_node_t *child = (Bptree_dentry_inner_node_t *)child_sess.data;
            Bptree_dentry_inner_node_t *neighbor = (Bptree_dentry_inner_node_t *)neighbor_sess.data;
            
            if (idx == 0) {
                // 目标左，邻居右：将邻居所有 hints 追加到目标末尾
                memcpy(&child->hints[child_count], neighbor->hints,
                       neighbor_count * sizeof(Dentry_hint_t));
                child->meta.parent_ptr.entry_count = total;
            } else {
                // 目标右，邻居左：将邻居 hints 插入目标开头，目标原 hints 后移
                memmove(&child->hints[neighbor_count], &child->hints[0],
                        child_count * sizeof(Dentry_hint_t));
                memcpy(&child->hints[0], neighbor->hints,
                       neighbor_count * sizeof(Dentry_hint_t));
                child->meta.parent_ptr.entry_count = total;
            }
            // ⚠ 合并后全部孩子归属 child：parent_ptr 收敛到 child_blk
            { result_code_t r = reparent_children(child, child_blk, total); if (r != RC_OK) return r; }
            // 写回目标节点（析构时）
            child_sess.dirty();
        } else {
            // ---- 叶子节点合并 ----
            Bptree_dentry_leaf_node_t *child = (Bptree_dentry_leaf_node_t *)child_sess.data;
            Bptree_dentry_leaf_node_t *neighbor = (Bptree_dentry_leaf_node_t *)neighbor_sess.data;
            
            if (idx == 0) {
                // 目标左，邻居右：追加邻居 entries
                memcpy(&child->entries[child_count], neighbor->entries,
                       neighbor_count * sizeof(Dentry_t));
                child->meta.parent_ptr.entry_count = total;
                // 更新兄弟链表：目标的下一个 = 邻居的下一个
                child->meta.ptrs[2].ptr = neighbor->meta.ptrs[2].ptr;
                // 若邻居有后继，更新后继的前驱指向目标
                uint64_t next_blk = neighbor->meta.ptrs[2].ptr;
                if (next_blk != PTR_INVALID_52) {
                    mdata_piece_session next_sess(mdata_piece_mgr, next_blk);
                    if (next_sess.index_error()) return RC_ERR_STATE;
                    if (!next_sess) return RC_ERR_IO;
                    Bptree_dentry_leaf_node_t *next_node = (Bptree_dentry_leaf_node_t *)next_sess.data;
                    next_node->meta.ptrs[1].ptr = child_blk;
                    next_sess.dirty();
                }
            } else {
                // 目标右，邻居左：将邻居 entries 插入目标开头
                memmove(&child->entries[neighbor_count], &child->entries[0],
                        child_count * sizeof(Dentry_t));
                memcpy(&child->entries[0], neighbor->entries,
                       neighbor_count * sizeof(Dentry_t));
                child->meta.parent_ptr.entry_count = total;
                // 更新兄弟链表：目标的前驱 = 邻居的前驱
                child->meta.ptrs[1].ptr = neighbor->meta.ptrs[1].ptr;
                // 若邻居有前驱，更新前驱的后继指向目标
                uint64_t prev_blk = neighbor->meta.ptrs[1].ptr;
                if (prev_blk != PTR_INVALID_52) {
                    mdata_piece_session prev_sess(mdata_piece_mgr, prev_blk);
                    if (prev_sess.index_error()) return RC_ERR_STATE;
                    if (!prev_sess) return RC_ERR_IO;
                    Bptree_dentry_leaf_node_t *prev_node = (Bptree_dentry_leaf_node_t *)prev_sess.data;
                    prev_node->meta.ptrs[2].ptr = child_blk;
                    prev_sess.dirty();
                }
            }
            // 写回目标节点（析构时）
            child_sess.dirty();
        }

        // ---- 释放邻居节点占用的块 ----
        block_free(neighbor_blk, 2);   // 每个节点占 2 个 FS_block

        // ---- 从父节点删除邻居对应的 hint ----
        uint16_t del_idx = (idx == 0) ? 1 : (idx - 1);
        remove_parent_hint(parent, del_idx);
        // 更新父节点中保留的 hint（目标节点）的最大 hash
        uint64_t new_max = get_node_max_hash(child_sess.data, is_inner);
        uint16_t kept_idx = (idx == 0) ? 0 : (idx - 1);  // 注意：删除后索引可能变化
        // 如果 del_idx == kept_idx? 但 kept_idx 是删除前保留的索引，删除后如果 del_idx < kept_idx，则 kept_idx 会前移，需要调整。
        // 因为 remove_parent_hint 左移了，所以保留的索引可能变化：
        //   - 若 del_idx == 0，则删除第一个，原来第二个变为索引0，保留的是原来的目标（如果目标在右则索引为 idx-1，大于0，不受影响）
        // 更安全的方式：直接通过 child_blk 在父节点中查找，或者记录保留索引。
        // 简单做法：在删除前记录保留的 hint 索引，删除后重新计算。
        // 我们记录保留的索引 kept_idx_before = (idx == 0) ? 0 : (idx - 1);
        // 删除后，如果 del_idx < kept_idx_before，则 kept_idx_after = kept_idx_before - 1；否则 kept_idx_after = kept_idx_before。
        uint16_t kept_idx_before = (idx == 0) ? 0 : (idx - 1);
        uint16_t kept_idx_after = kept_idx_before;
        if (del_idx < kept_idx_before) kept_idx_after--;
        // 更新该 hint 的 hash_max
        parent->hints[kept_idx_after].hash_max = new_max;

        // 判断父节点是否欠载（非根且 entry_count < 63）
        uint16_t new_parent_count = (uint16_t)parent->meta.parent_ptr.entry_count;
        bool parent_is_root = parent->meta.parent_ptr.is_root_node;
        if (!parent_is_root && new_parent_count < 63)
            is_parent_need_balance = true;
        else
            is_parent_need_balance = false;

        // 写回父节点（析构时）
        parent_sess.dirty();
        return RC_OK;
    } else {
        // ---------- 5. 抹匀（再分配） ----------
        // 总条目 > 126，两个节点合并后超阶，需要重新均匀分配
        // 计算分割点：左节点获得 total/2 个，右节点获得 total - total/2
        uint16_t left_count = total / 2;
        uint16_t right_count = total - left_count;

        if (is_inner) {
            Bptree_dentry_inner_node_t *child = (Bptree_dentry_inner_node_t *)child_sess.data;
            Bptree_dentry_inner_node_t *neighbor = (Bptree_dentry_inner_node_t *)neighbor_sess.data;
            // 将两个节点的 hints 合并到一个临时数组
            Dentry_hint_t merged[254]; // 最多 254
            if (idx == 0) {
                // 目标左，邻居右：目标在前，邻居在后
                memcpy(merged, child->hints, child_count * sizeof(Dentry_hint_t));
                memcpy(merged + child_count, neighbor->hints, neighbor_count * sizeof(Dentry_hint_t));
                // 分配：前 left_count 给目标，后 right_count 给邻居
                memcpy(child->hints, merged, left_count * sizeof(Dentry_hint_t));
                memcpy(neighbor->hints, merged + left_count, right_count * sizeof(Dentry_hint_t));
                child->meta.parent_ptr.entry_count = left_count;
                neighbor->meta.parent_ptr.entry_count = right_count;
            } else {
                // 目标右，邻居左：邻居在前，目标在后
                memcpy(merged, neighbor->hints, neighbor_count * sizeof(Dentry_hint_t));
                memcpy(merged + neighbor_count, child->hints, child_count * sizeof(Dentry_hint_t));
                // 分配：前 left_count 给邻居，后 right_count 给目标
                memcpy(neighbor->hints, merged, left_count * sizeof(Dentry_hint_t));
                memcpy(child->hints, merged + left_count, right_count * sizeof(Dentry_hint_t));
                neighbor->meta.parent_ptr.entry_count = left_count;
                child->meta.parent_ptr.entry_count = right_count;
            }
            // ⚠ 抹匀后孩子可能换父：parent_ptr 收敛（idx==0: child左邻右；否则 neighbor左child右）
            if (idx == 0) {
                { result_code_t r = reparent_children(child, child_blk, left_count);  if (r != RC_OK) return r; }
                { result_code_t r = reparent_children(neighbor, neighbor_blk, right_count); if (r != RC_OK) return r; }
            } else {
                { result_code_t r = reparent_children(neighbor, neighbor_blk, left_count); if (r != RC_OK) return r; }
                { result_code_t r = reparent_children(child, child_blk, right_count);  if (r != RC_OK) return r; }
            }
            // 写回两个节点（析构时）
            child_sess.dirty();
            neighbor_sess.dirty();
        } else {
            // ---- 叶子节点抹匀 ----
            Bptree_dentry_leaf_node_t *child = (Bptree_dentry_leaf_node_t *)child_sess.data;
            Bptree_dentry_leaf_node_t *neighbor = (Bptree_dentry_leaf_node_t *)neighbor_sess.data;
            Dentry_t merged[254]; // 最多 254
            if (idx == 0) {
                // 目标左，邻居右
                memcpy(merged, child->entries, child_count * sizeof(Dentry_t));
                memcpy(merged + child_count, neighbor->entries, neighbor_count * sizeof(Dentry_t));
                memcpy(child->entries, merged, left_count * sizeof(Dentry_t));
                memcpy(neighbor->entries, merged + left_count, right_count * sizeof(Dentry_t));
                child->meta.parent_ptr.entry_count = left_count;
                neighbor->meta.parent_ptr.entry_count = right_count;
            } else {
                // 目标右，邻居左
                memcpy(merged, neighbor->entries, neighbor_count * sizeof(Dentry_t));
                memcpy(merged + neighbor_count, child->entries, child_count * sizeof(Dentry_t));
                memcpy(neighbor->entries, merged, left_count * sizeof(Dentry_t));
                memcpy(child->entries, merged + left_count, right_count * sizeof(Dentry_t));
                neighbor->meta.parent_ptr.entry_count = left_count;
                child->meta.parent_ptr.entry_count = right_count;
            }
            // 写回两个节点（析构时；兄弟链表保持不变）
            child_sess.dirty();
            neighbor_sess.dirty();
        }

        // ---- 更新父节点中两个 hint 的 hash_max ----
        // 子节点和邻居的块号可能变化？不，块号不变，只是内容变了。
        // 更新目标节点对应的 hint（索引为 idx）和邻居对应的 hint（索引为 neighbor_idx）
        uint16_t neighbor_idx = (idx == 0) ? 1 : (idx - 1);
        uint64_t child_max = get_node_max_hash(child_sess.data, is_inner);
        uint64_t neighbor_max = get_node_max_hash(neighbor_sess.data, is_inner);
        parent->hints[idx].hash_max = child_max;
        parent->hints[neighbor_idx].hash_max = neighbor_max;

        // 抹匀不改变父节点 entry_count，父节点不会因此欠载
        is_parent_need_balance = false;

        // 写回父节点（析构时）
        parent_sess.dirty();
        return RC_OK;
    }
}
/* ═══════════════════════════════════════════════════════════════════
 * dentry_root_shrink — 根收缩：根内部节点只剩 1 个孩子时提升孩子为新根
 *
 * 场景：删除路径上根内部节点合并到 entry_count==1（或 dentry_node_balance
 * 对 idx==0 && parent_count==1 返回 RC_ERR_STATE 时），树退化为一层
 * 无意义的根内部节点。此时把唯一孩子提升为新根：
 *
 *   孩子 is_root_node=1, parent_ptr.ptr = root_inode_idx（"." 自指 inode）
 *   孩子是叶子 → ptrs[1] = root_inode_idx（".." 孤儿自指），ptrs[2] = INVALID
 *   更新 root inode.data_root → root_dentry_blk → 释放旧根 2 块
 *
 * 返回：
 *   RC_OK             成功（或无需收缩：非内部节点 / entry_count != 1）
 *   RC_ERR_STATE      root_blk 不是根节点（调用方用错）
 *   其他 RC_ERR_*      I/O 或结构错误
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dentry_root_shrink(uint64_t root_blk, uint64_t root_inode_idx)
{
    mdata_piece_session root_sess(mdata_piece_mgr, root_blk);
    if (root_sess.index_error()) return RC_ERR_STATE;
    if (!root_sess) return RC_ERR_IO;
    Bptree_dentry_inner_node_t *root = (Bptree_dentry_inner_node_t *)root_sess.data;
    if (!is_dentry_node(&root->meta)) return RC_ERR_STATE;
    if (!root->meta.parent_ptr.is_root_node) return RC_ERR_STATE;

    /* 非内部节点（根叶子）或孩子不止一个：无需收缩 */
    if (!root->meta.parent_ptr.is_inner_node) return RC_OK;
    uint16_t cnt = (uint16_t)root->meta.parent_ptr.entry_count;
    if (cnt != 1) return RC_OK;

    uint64_t child_blk = root->hints[0].sub_ptr.ptr;

    /* ── 读唯一孩子，提升为新根 ── */
    mdata_piece_session child_sess(mdata_piece_mgr, child_blk);
    if (child_sess.index_error()) return RC_ERR_STATE;
    if (!child_sess) return RC_ERR_IO;
    Bptree_meta_t *child_meta = (Bptree_meta_t *)child_sess.data;
    if (!is_dentry_node(child_meta)) return RC_ERR_STATE;
    if (child_meta->parent_ptr.is_root_node) return RC_ERR_STATE;   /* 孩子已是根？结构损坏 */

    child_meta->parent_ptr.is_root_node  = 1;
    child_meta->parent_ptr.ptr           = root_inode_idx;   /* "." 指向目录自身 inode */

    /* 叶子：".." 孤儿自指（与 dir_insert_dentry 空目录建根对齐）；
     * 根不做同级链表 → ptrs[2] 失效。内部节点 ptrs[1] 亦为 ".."。 */
    if (!child_meta->parent_ptr.is_inner_node) {
        auto *child_leaf = (Bptree_dentry_leaf_node_t *)child_sess.data;
        child_leaf->meta.ptrs[0].ptr        = PTR_INVALID;
        child_leaf->meta.ptrs[1].ptr        = root_inode_idx;   /* ".." */
        child_leaf->meta.ptrs[2].ptr        = PTR_INVALID;
    } else {
        auto *child_inner = (Bptree_dentry_inner_node_t *)child_sess.data;
        child_inner->meta.ptrs[1].ptr       = root_inode_idx;   /* ".." */
    }

    child_sess.dirty();

    /* ── 更新 root inode.data_root 与内存根句柄 ── */
    raven_Inode_t root_inode;
    inode_read(root_inode_idx, &root_inode);
    root_inode.data_root = child_blk;
    inode_write(root_inode_idx, &root_inode);
    root_dentry_blk = child_blk;

    /* ── 释放旧根（恒 2 块） ── */
    block_free(root_blk, 2);
    return RC_OK;
}

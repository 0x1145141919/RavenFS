#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>

/* ═══════════════════════════════════════════════════════════════════
 * root_dir.cpp — RavenFS v1.1 目录 dentry 顶层业务（调 dentry 树层）
 *
 * dir_insert_dentry / dir_rm_dentry / dir_lookup —— 任意目录（inode_idx 指定）
 * root_dir_* —— root 专属薄封装（内部转调 dir_*）
 * 零缓存原则：所有块/inode 访问直接 pread/pwrite，无缓存层。
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * dir_insert_dentry — 在 inode_idx 目录中插入名为 name 的条目
 *
 * 空目录（data_root=PTR_INVALID）首次插入时先建树根（根叶子，2 块）：
 *   根叶子 meta 与 mkfs build_root_dentry_node 对齐：
 *     parent_ptr = {cnt:0, inner:0, root:1, ptr=inode_idx}   (".", 自身 inode)
 *     ptrs[0]    = PTR_INVALID   （预留）
 *     ptrs[1]    = inode_idx     （".."，孤儿目录自指；mkdir 接入父链后由父提供）
 *     ptrs[2]    = PTR_INVALID   （预留，根叶子不参与同级链表）
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dir_insert_dentry(uint64_t inode_idx, const char *name)
{
    if (!tmp_name_len_limiter(name))
        return RC_ERR_NAME_LEN;
    if (!inode_test(inode_idx))
        return RC_ERR_STATE;

    name_cmlpx_t cmplx = {
        .name = (char*)name,
        .hash = name_hash(name)
    };

    raven_Inode_t ino;
    inode_read(inode_idx, &ino);
    if (ino.type != INODE_TYPE_DIR)
        return RC_ERR_STATE;

    /* 空目录：先建树根（根叶子） */
    if (ino.data_root == PTR_INVALID) {
        uint64_t root_blk = block_alloc_hard(2, 1);
        if (root_blk == PTR_INVALID) return RC_ERR_NO_BLOCK;

        /* 新建根叶子：mdata_piece_session 直接覆盖会话页（无栈缓冲/无复制），
         * 会话页整块清零 + 填 meta，dirty() 后析构写回（no_cache 写盘 /
         * offload 直改内核页缓存切片）。 */
        mdata_piece_session root_sess(mdata_piece_mgr, root_blk);
        if (root_sess.index_error()) return RC_ERR_STATE;
        if (!root_sess) return RC_ERR_IO;
        Bptree_dentry_leaf_node_t *rleaf = (Bptree_dentry_leaf_node_t*)root_sess.data;
        memset(root_sess.data, 0, 8192);
        rleaf->meta.magics[0] = DENTRY_MAGIC1;
        rleaf->meta.magics[1] = DENTRY_MAGIC2;
        rleaf->meta.magics[2] = DENTRY_MAGIC3;
        rleaf->meta.magics[3] = DENTRY_MAGIC4;
        rleaf->meta.parent_ptr.entry_count   = 0;
        rleaf->meta.parent_ptr.is_inner_node = 0;
        rleaf->meta.parent_ptr.is_root_node  = 1;
        rleaf->meta.parent_ptr.ptr           = inode_idx;   /* "." */
        rleaf->meta.ptrs[0].ptr = PTR_INVALID;
        rleaf->meta.ptrs[1].ptr = inode_idx;                /* ".." 孤儿自指 */
        rleaf->meta.ptrs[2].ptr = PTR_INVALID;
        root_sess.dirty();

        ino.data_root = root_blk;
        inode_write(inode_idx, &ino);
    }

    uint64_t dentry_root_idx = ino.data_root;

    dentry_bptree_path_t trace;
    result_code_t rc = dentry_Bptree_walk(dentry_root_idx, &trace, &cmplx);
    if (rc) return rc;
    if (trace.is_finally_hit) return RC_ERR_EXIST;

    uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    uint16_t glb = trace.child_idx[trace.depth - 1];
    uint16_t insert_idx = (glb == DENTRY_LEAF_GLB_NONE) ? 0 : (glb + 1);

    uint64_t flags = 0;
    rc = dentry_leaf_entry_insert(leaf_blk, &cmplx, insert_idx, flags);
    if (rc) return rc;

    /* ── 阶段一：若未分裂但 hash_max 变化，向上递归更新 ── */
    if ((flags & 2u) && !(flags & 1u)) {
        uint64_t cur_blk = leaf_blk;
        for (int i = trace.depth - 2; i >= 0; --i) {
            uint64_t parent_blk = trace.blocks[i];
            uint16_t child_idx_in_parent = trace.child_idx[i];

            /* 会话直读：parent 读改（dirty 写回），cur 只读；零栈缓冲零复制 */
            mdata_piece_session parent_sess(mdata_piece_mgr, parent_blk);
            if (parent_sess.index_error()) return RC_ERR_STATE;
            if (!parent_sess) return RC_ERR_IO;
            Bptree_dentry_inner_node_t *parent = (Bptree_dentry_inner_node_t*)parent_sess.data;

            mdata_piece_session cur_sess(mdata_piece_mgr, cur_blk);
            if (cur_sess.index_error()) return RC_ERR_STATE;
            if (!cur_sess) return RC_ERR_IO;
            uint64_t new_max = node_max_hash(cur_sess.data);

            uint64_t old_max = parent->hints[child_idx_in_parent].hash_max;
            if (new_max == old_max) break;
            parent->hints[child_idx_in_parent].hash_max = new_max;
            parent_sess.dirty();
            cur_blk = parent_blk;
        }
    }

    /* ── 阶段二：若叶子溢出，处理分裂 ── */
    if (flags & 1u) {
        split_result_t split_res = dentry_leaf_split(leaf_blk);
        if (split_res.rc != RC_OK) return split_res.rc;

        /* 情况 A：根是叶子，需创建新根（内部节点） */
        if (trace.depth == 1) {
            return dentry_new_root_inner(split_res.low_node_blkidx,
                                         split_res.low_node_maxhash,
                                         split_res.high_node_blkidx,
                                         inode_idx);
        }

        /* 情况 B：非根叶子，父节点存在 */
        uint64_t parent_blk = trace.blocks[trace.depth - 2];
        uint16_t child_idx  = trace.child_idx[trace.depth - 2];
        bool need_parent_split = false;
        rc = dentry_spilt_parent_apply(parent_blk, child_idx, split_res, need_parent_split);
        if (rc) return rc;

        /* 递归向上处理内部节点溢出 */
        uint64_t cur_parent_blk = parent_blk;
        while (need_parent_split) {
            mdata_piece_session parent_sess(mdata_piece_mgr, cur_parent_blk);
            if (parent_sess.index_error()) return RC_ERR_STATE;
            if (!parent_sess) return RC_ERR_IO;
            Bptree_dentry_inner_node_t *parent_node = (Bptree_dentry_inner_node_t*)parent_sess.data;   /* 只读 */

            bool parent_is_root = parent_node->meta.parent_ptr.is_root_node;
            split_result_t inner_split = dentry_inner_split(cur_parent_blk);
            if (inner_split.rc != RC_OK) return inner_split.rc;

            if (parent_is_root) {
                // 根内部节点溢出，创建新根
                rc = dentry_new_root_inner(inner_split.low_node_blkidx,
                                           inner_split.low_node_maxhash,
                                           inner_split.high_node_blkidx,
                                           inode_idx);
                if (rc) return rc;
                need_parent_split = false;
                break;
            } else {
                // 非根内部节点，向祖父节点应用分裂
                uint64_t grand_parent_blk = parent_node->meta.parent_ptr.ptr;
                // 查找 cur_parent_blk 在祖父节点中的索引
                mdata_piece_session grand_sess(mdata_piece_mgr, grand_parent_blk);
                if (grand_sess.index_error()) return RC_ERR_STATE;
                if (!grand_sess) return RC_ERR_IO;
                Bptree_dentry_inner_node_t *grand = (Bptree_dentry_inner_node_t*)grand_sess.data;   /* 只读 */
                uint16_t grand_idx = 0;
                bool found = false;
                for (uint16_t k = 0; k < grand->meta.parent_ptr.entry_count; ++k) {
                    if (grand->hints[k].sub_ptr.ptr == cur_parent_blk) {
                        grand_idx = k;
                        found = true;
                        break;
                    }
                }
                if (!found) return RC_ERR_STATE;

                rc = dentry_spilt_parent_apply(grand_parent_blk, grand_idx, inner_split, need_parent_split);
                if (rc) return rc;
                cur_parent_blk = grand_parent_blk;
                // 继续循环
            }
        }
    }

    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * dir_rm_dentry — 从 inode_idx 目录物理删除名为 name 的条目
 *
 * 流程：walk → 命中校验 → 叶子删除（memmove + 刷父 hash_max）→
 * 欠载平衡/合并（向上递归）→ inode_free（条目 inode 有效时）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dir_rm_dentry(uint64_t inode_idx, const char *name)
{
    if (!tmp_name_len_limiter(name))
        return RC_ERR_NAME_LEN;
    if (!inode_test(inode_idx))
        return RC_ERR_STATE;

    name_cmlpx_t cmplx = {
        .name = (char*)name,
        .hash = name_hash(name)
    };

    raven_Inode_t ino;
    inode_read(inode_idx, &ino);
    if (ino.type != INODE_TYPE_DIR)
        return RC_ERR_STATE;
    if (ino.data_root == PTR_INVALID)
        return RC_ERR_NOT_FOUND;   /* 空目录，无条目可删 */

    uint64_t dentry_root_idx = ino.data_root;

    dentry_bptree_path_t trace;
    result_code_t rc = dentry_Bptree_walk(dentry_root_idx, &trace, &cmplx);
    if (rc) return rc;
    if (!trace.is_finally_hit) return RC_ERR_NOT_FOUND;

    uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    uint16_t idx = trace.child_idx[trace.depth - 1];

    mdata_piece_session leaf_sess(mdata_piece_mgr, leaf_blk);
    if (leaf_sess.index_error()) return RC_ERR_STATE;
    if (!leaf_sess) return RC_ERR_IO;
    Bptree_dentry_leaf_node_t *leaf = (Bptree_dentry_leaf_node_t*)leaf_sess.data;   /* 只读（取条目 inum） */
    uint64_t inum = leaf->entries[idx].inode_ptr.ptr;
    /* ⚠ 52 位位域读出的 inum 与 PTR_INVALID 比较必须同样截断（64 位比较恒不相等）。
     * insert 尚未接 inode 分配时条目为悬空（PTR_INVALID）：此时只删树条目、
     * 跳过 inode_free —— 幼年期过渡态，insert 接 inode 后自然消除。 */
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    const bool has_inode = (inum != PTR_INVALID_52);

    uint64_t flags = 0;
    rc = dentry_leaf_entry_delete(leaf_blk, idx, flags);
    if (rc) return rc;

    /* ── 阶段一：若 hash_max 变化，向上递归更新 ── */
    if (flags & 2u) {
        uint64_t cur_blk = leaf_blk;
        for (int i = trace.depth - 2; i >= 0; --i) {
            uint64_t parent_blk = trace.blocks[i];
            uint16_t child_idx_in_parent = trace.child_idx[i];

            /* 会话直读：parent 读改（dirty 写回），cur 只读；零栈缓冲零复制 */
            mdata_piece_session parent_sess(mdata_piece_mgr, parent_blk);
            if (parent_sess.index_error()) return RC_ERR_STATE;
            if (!parent_sess) return RC_ERR_IO;
            Bptree_dentry_inner_node_t *parent = (Bptree_dentry_inner_node_t*)parent_sess.data;

            mdata_piece_session cur_sess(mdata_piece_mgr, cur_blk);
            if (cur_sess.index_error()) return RC_ERR_STATE;
            if (!cur_sess) return RC_ERR_IO;
            uint64_t new_max = node_max_hash(cur_sess.data);

            uint64_t old_max = parent->hints[child_idx_in_parent].hash_max;
            if (new_max == old_max) break;
            parent->hints[child_idx_in_parent].hash_max = new_max;
            parent_sess.dirty();
            cur_blk = parent_blk;
        }
    }

    /* ── 阶段二：若叶子欠载，进行平衡/合并（向上递归） ── */
    if (flags & 1u) {
        if (trace.depth > 1) {
            /* RC_ERR_STATE 捕获：验证是否为“根内部节点只剩 1 孩子”的
             * 收缩场景（dentry_node_balance 设计上把根收缩交给上层）。
             * 是则收缩并返回 true；否则返回 false（保留原错误）。 */
            auto try_root_shrink = [&](uint64_t blk) -> bool {
                mdata_piece_session tmp_sess(mdata_piece_mgr, blk);
                if (tmp_sess.index_error()) return false;
                if (!tmp_sess) return false;
                Bptree_meta_t *m = (Bptree_meta_t *)tmp_sess.data;
                if (!is_dentry_node(m)) return false;
                if (!m->parent_ptr.is_root_node) return false;
                if (!m->parent_ptr.is_inner_node) return false;
                if (m->parent_ptr.entry_count != 1) return false;
                return dentry_root_shrink(blk, inode_idx) == RC_OK;
            };

            uint64_t parent_blk = trace.blocks[trace.depth - 2];
            uint16_t child_idx = trace.child_idx[trace.depth - 2];
            bool need_parent_balance = false;
            rc = dentry_node_balance(parent_blk, child_idx, need_parent_balance);
            if (rc == RC_ERR_STATE) {
                /* 根收缩场景：父是根内部节点且只剩 1 孩子 */
                if (!try_root_shrink(parent_blk)) return RC_ERR_STATE;
                need_parent_balance = false;   /* 收缩后无欠载链 */
            } else if (rc) {
                return rc;
            }

            uint64_t cur_parent_blk = parent_blk;
            while (need_parent_balance) {
                mdata_piece_session parent_sess(mdata_piece_mgr, cur_parent_blk);
                if (parent_sess.index_error()) return RC_ERR_STATE;
                if (!parent_sess) return RC_ERR_IO;
                Bptree_dentry_inner_node_t *parent_node = (Bptree_dentry_inner_node_t*)parent_sess.data;   /* 只读 */

                if (parent_node->meta.parent_ptr.is_root_node) {
                    /* 根内部节点只剩 1 孩子（合并残留）→ 收缩 */
                    try_root_shrink(cur_parent_blk);
                    break;
                }

                uint64_t grand_parent_blk = parent_node->meta.parent_ptr.ptr;
                mdata_piece_session grand_sess(mdata_piece_mgr, grand_parent_blk);
                if (grand_sess.index_error()) return RC_ERR_STATE;
                if (!grand_sess) return RC_ERR_IO;
                Bptree_dentry_inner_node_t *grand = (Bptree_dentry_inner_node_t*)grand_sess.data;   /* 只读 */
                uint16_t grand_idx = 0;
                bool found = false;
                for (uint16_t k = 0; k < grand->meta.parent_ptr.entry_count; ++k) {
                    if (grand->hints[k].sub_ptr.ptr == cur_parent_blk) {
                        grand_idx = k;
                        found = true;
                        break;
                    }
                }
                if (!found) return RC_ERR_STATE;

                rc = dentry_node_balance(grand_parent_blk, grand_idx, need_parent_balance);
                if (rc == RC_ERR_STATE) {
                    /* 祖父是根且只剩 1 孩子 → 收缩后链结束 */
                    if (!try_root_shrink(grand_parent_blk)) return RC_ERR_STATE;
                    need_parent_balance = false;
                    break;
                } else if (rc) {
                    return rc;
                }
                cur_parent_blk = grand_parent_blk;
            }

            /* 阶段二收尾：合并路径可能留下“根内部节点 entry_count==1”
             * 的残留形态（根不置 need_balance），统一收缩。 */
            try_root_shrink(root_dentry_blk);
        }
    }

    if (has_inode) inode_free(inum);
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * dir_lookup — 在 inode_idx 目录中查找 name
 *
 * 命中 → RC_OK 且 *out_inum 输出条目 inode 编号；
 * 未命中 → RC_ERR_NOT_FOUND；目录无效/非目录 → RC_ERR_STATE。
 * （当前 insert 尚未分配 inode，命中的 out_inum 为 PTR_INVALID 52 位形态）
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dir_lookup(uint64_t inode_idx, const char *name, uint64_t *out_inum)
{
    if (!out_inum) return RC_ERR_STATE;
    if (!tmp_name_len_limiter(name)) return RC_ERR_NAME_LEN;
    if (!inode_test(inode_idx)) return RC_ERR_STATE;

    name_cmlpx_t cmplx = {
        .name = (char*)name,
        .hash = name_hash(name)
    };

    raven_Inode_t ino;
    inode_read(inode_idx, &ino);
    if (ino.type != INODE_TYPE_DIR) return RC_ERR_STATE;
    if (ino.data_root == PTR_INVALID) return RC_ERR_NOT_FOUND;   /* 空目录 */

    dentry_bptree_path_t trace;
    result_code_t rc = dentry_Bptree_walk(ino.data_root, &trace, &cmplx);
    if (rc) return rc;
    if (!trace.is_finally_hit) return RC_ERR_NOT_FOUND;

    uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    uint16_t idx = trace.child_idx[trace.depth - 1];

    mdata_piece_session leaf_sess(mdata_piece_mgr, leaf_blk);
    if (leaf_sess.index_error()) return RC_ERR_STATE;
    if (!leaf_sess) return RC_ERR_IO;
    Bptree_dentry_leaf_node_t *leaf = (Bptree_dentry_leaf_node_t*)leaf_sess.data;   /* 只读 */
    if (leaf->meta.parent_ptr.is_inner_node) return RC_ERR_STATE;

    *out_inum = leaf->entries[idx].inode_ptr.ptr;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * root_dir_* — root 专属薄封装（任意目录版见 dir_*）
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::root_dir_insert_dentry(const char *name)
{
    return dir_insert_dentry(sb.root_inode, name);
}
result_code_t RavenFS_juvenile::root_dir_rm_dentry(const char *name)
{
    return dir_rm_dentry(sb.root_inode, name);
}
result_code_t RavenFS_juvenile::root_dir_lookup(const char *name, uint64_t *out_inum)
{
    return dir_lookup(sb.root_inode, name, out_inum);
}

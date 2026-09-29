#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>

/* ═══════════════════════════════════════════════════════════════════
 * dentry_insert_delete.cpp — dentry 叶子条目增删 + 下游资产维护
 * （原 dentry_tree.cpp 拆出，2026-09-07）
 *
 * 本文件负责：
 *   dentry_leaf_entry_insert（插条目即建下游：inode/名字堆/树根，guard 回滚）
 *   dentry_leaf_entry_delete（物理删除 + 下游资产维护，DELETE_FULL）
 *
 * 与纯树机械（split/balance，见兄弟文件）的分界：本文件条目操作携带
 * 下游资产副作用（inode 槽 / 名字堆 / owner 目录计数 / 归零回收），
 * 失败 guard 全自动回滚；分裂/欠载不在本文件内处理——result_flags
 * 上抛，由上层（root_dir / dentry 调用链）驱动结构机械。
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * dentry_leaf_entry_insert — 叶子条目插入 + 下游资产创建（下沉版 2026-09-01）
 *
 * 前置条件：idx 由 dentry_Bptree_walk 的 GLB 语义推导：
 *   glb == DENTRY_LEAF_GLB_NONE → idx=0；否则 idx=glb+1。
 * 调用前须已查重（EXIST 由上层 walk 判，避免浪费 inode/堆/树根）。
 *
 * 下游资产创建（宗旨：插条目即建下游，与 delete 对称）：
 *   1. 分配 + 初始化目标 inode：DIR 急建空根叶子 + dir inode（size=0/
 *      link_count=2）；REG/SOFT_SYM 内联空 inode（link_count=1）
 *   2. 长名（>40B）：owner 目录名字堆分配外置存储 + 写尾部
 *   3. memmove 树插（消费 name->inum / full_char_remain_ptr）
 *   4. owner 目录计数维护：size++、link_count = size+2（§3.3）
 *   失败：inode_guard / block_guard / heap_guard 全自动回滚。
 *
 * result_flags：
 *   bit0 = 1 → 需分裂（count 入后 >126）
 *   bit1 = 1 → 叶子最大 hash 已变，父层 hint 需刷新
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dentry_leaf_entry_insert(
    uint64_t leaf_blk_base_idx,
    name_cmlpx_t *name,
    uint16_t idx,
    uint64_t &result_flags,
    uint8_t inode_type,
    uint64_t owner_dir_inode)
{
    result_flags = 0;
    if (!name || !name_len_limiter(name->name))
        return RC_ERR_NAME_LEN;
    if (inode_type != INODE_TYPE_REGULAR &&
        inode_type != INODE_TYPE_DIR &&
        inode_type != INODE_TYPE_SOFT_SYM)
        return RC_ERR_STATE;

    /* ── 1. 分配 + 初始化目标 inode（guard 兜底全回滚） ── */
    uint64_t new_inum = inode_alloc();
    if (new_inum == PTR_INVALID) return RC_ERR_NO_INODE;
    inode_guard_t inode_guard;
    inode_guard.arm(this, new_inum);
    block_guard_t dir_root_guard;   /* dir 型专用：新建树根 2 块回滚 */

    if (inode_type == INODE_TYPE_DIR) {
        /* dir 型：先建根叶子（2 块），再初始化 dir inode。
         * 会话页整块清零 + 填 meta，dirty() 后析构写回。 */
        uint64_t root_blk = block_alloc_hard(2, 1);
        if (root_blk == PTR_INVALID) return RC_ERR_NO_BLOCK;   /* guard 自动回滚 inode */
        dir_root_guard.arm(this, root_blk, 2);

        mdata_piece_session root_sess(mdata_piece_mgr, root_blk);
        if (root_sess.index_error()) return RC_ERR_STATE;
        if (!root_sess) return RC_ERR_IO;
        Bptree_ddentry_leaf_node_t *rleaf = (Bptree_ddentry_leaf_node_t*)root_sess.data;
        memset(root_sess.data, 0, 8192);
        rleaf->meta.magics[0] = DENTRY_MAGIC1;
        rleaf->meta.magics[1] = DENTRY_MAGIC2;
        rleaf->meta.magics[2] = DENTRY_MAGIC3;
        rleaf->meta.magics[3] = DENTRY_MAGIC4;
        RFS_PP_SET_COUNT(rleaf->meta.parent_ptr, 0);
        RFS_PP_SET_IS_INNER(rleaf->meta.parent_ptr, 0);
        RFS_PP_SET_IS_ROOT(rleaf->meta.parent_ptr, 1);
        RFS_PP_SET_PTR(rleaf->meta.parent_ptr, new_inum);   /* "." = 自身 inode */
        RFS_PTR_SET_ADDR(rleaf->meta.ptrs[0], PTR_INVALID);
        RFS_PTR_SET_ADDR(rleaf->meta.ptrs[1], owner_dir_inode);         /* ".." = 父目录 inode */
        RFS_PTR_SET_ADDR(rleaf->meta.ptrs[2], PTR_INVALID);
        root_sess.dirty();

        {
            InodeSession ino_sess(inode_cache_mgr, new_inum);
            if (!ino_sess) return RC_ERR_IO;   /* guard 自动回滚 */
            raven_dir_Inode_t *dino = (raven_dir_Inode_t*)ino_sess.inode;
            memset(dino, 0, sizeof(raven_dir_Inode_t));
            RFS_INO1_SET_TYPE(*dino, INODE_TYPE_DIR);
            dino->link_count      = 2;         /* = size + 2（size=0，§3.3） */
            RFS_MODE_XATTR_SET(dino->mode_xattr, 0755, RFS_XATTR_NONE);
            dino->size            = 0;         /* 新目录无条目 */
            dino->data_root       = root_blk;  /* 恒有效（不变量 11） */
            /* 名字堆 10 描述符：未激活哨兵（memset 0 是雷 —— 0 是合法块号，
             * dir_name_heap_release 会误当激活堆释放 block 0/1 根块） */
            for (int i = 0; i < 10; i++) {
                RFS_NHC_SET_BITMAP(((name_heap_controller_t *)&dino->heaps_8B_8k)[i], NAME_HEAP_INACTIVE);
                RFS_NHC_SET_DATA(((name_heap_controller_t *)&dino->heaps_8B_8k)[i], NAME_HEAP_INACTIVE);
            }
            ino_sess.dirty();
        }
    } else {
        /* REGULAR / SOFT_SYM：内联空 inode（全零 inline_data） */
        InodeSession ino_sess(inode_cache_mgr, new_inum);
        if (!ino_sess) return RC_ERR_IO;   /* guard 自动回滚 */
        raven_Inode_t *ino = ino_sess.inode;
        memset(ino, 0, sizeof(raven_Inode_t));   /* 全零：inline_data = 内联空文件 */
        RFS_INO1_SET_TYPE(*ino, inode_type);
        ino->link_count      = 1;
        RFS_MODE_XATTR_SET(ino->mode_xattr, 0644, RFS_XATTR_NONE);
        ino->data_root       = PTR_INVALID;   /* 内联空文件/软链，无数据块 */
        ino_sess.dirty();
    }
    name->inum = new_inum;   /* dentry 落真实 inum */

    /* ── 2. 长名（>40B）：owner 目录名字堆分配外置存储 + 写尾部 ── */
    name_heap_guard_t heap_guard;
    if (name->name_len > DENTRY_INLINE_NAME_MAX) {
        name_heap_loc_t loc;
        result_code_t rc = name_heap_alloc(owner_dir_inode,
                                           name->name_len - DENTRY_INLINE_NAME_MAX, &loc);
        if (rc) return rc;   /* guard 自动回滚 */
        name->full_char_remain_ptr = (loc.fblk << 12) | (uint64_t)loc.offset;
        heap_guard.arm(this, owner_dir_inode, name->name_len, name->full_char_remain_ptr);
        rc = name_heap_write(name->name_len, name->full_char_remain_ptr,
                             name->name + DENTRY_INLINE_NAME_MAX);
        if (rc) return rc;   /* guard 自动回滚 */
    }

    /* ── 3. 树插（memmove + 写条目，消费 name->inum / full_char_remain_ptr） ── */
    mdata_piece_session sess(mdata_piece_mgr, leaf_blk_base_idx);
    if (sess.index_error()) return RC_ERR_STATE;
    if (!sess) return RC_ERR_IO;
    Bptree_ddentry_leaf_node_t *leaf = (Bptree_ddentry_leaf_node_t *)sess.data;
    if (!is_dentry_node(&leaf->meta)) return RC_ERR_STATE;
    if (RFS_PP_IS_INNER(leaf->meta.parent_ptr)) return RC_ERR_STATE;

    uint16_t count = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
    if (idx > count) return RC_ERR_STATE;

    uint64_t old_max_hash = (count > 0) ? leaf->entries[count - 1].hash : 0;

    /* 挪出插入位置 */
    if (idx < count) {
        memmove(&leaf->entries[idx + 1], &leaf->entries[idx],
                (count - idx) * sizeof(ddentry_t));
    }

    /* 填充新条目 */
    ddentry_t *entry = &leaf->entries[idx];
    entry->hash = name->hash;
    RFS_PTR_SET_FREE(entry->inode_ptr, name->name_len);
    RFS_PTR_SET_ADDR(entry->inode_ptr, name->inum);
    /* 前 40B（长名尾部在堆）。按 name_len 拷（≤40），剩余 [name_len,40) 清零——
     * 防源端越界读（name 字面量/短名 buffer 可能不足 40）；比较逻辑按
     * dentry_name_cmp 的 e_inline=min(len,40) 用，但清零保确定性。 */
    const uint16_t ncopy = name->name_len < DENTRY_INLINE_NAME_MAX
                               ? name->name_len : DENTRY_INLINE_NAME_MAX;
    memcpy(entry->name, name->name, ncopy);
    if (ncopy < DENTRY_INLINE_NAME_MAX)
        memset(entry->name + ncopy, 0, DENTRY_INLINE_NAME_MAX - ncopy);
    entry->full_char_remain_ptr = name->full_char_remain_ptr;

    count++;
    RFS_PP_SET_COUNT(leaf->meta.parent_ptr, count);

    /* 判定分裂需求 */
    if (count > 126)
        result_flags |= 1u;

    /* 判定父层 hash_max 是否需要刷新：
     *   - 若插入到末尾（idx == count-1），新 max 一定变了
     *   - 若旧叶子非空且新 max != 旧 max（极少碰撞场景也刷，安全） */
    if (count > 1) {
        uint64_t new_max_hash = leaf->entries[count - 1].hash;
        if (idx == count - 1 || new_max_hash != old_max_hash)
            result_flags |= 2u;
    } else {
        result_flags |= 2u;   /* 从空到非空，父层必有新 max */
    }

    sess.dirty();

    /* ── 4. owner 目录计数维护：size++、link_count = size+2（§3.3） ── */
    {
        InodeSession dir_sess(inode_cache_mgr, owner_dir_inode);
        if (!dir_sess) return RC_ERR_IO;
        dir_sess.inode->size++;
        dir_sess.inode->link_count = (uint32_t)dir_sess.inode->size + 2;
        dir_sess.dirty();
    }

    /* ── 5. 成功：全部转正 ── */
    heap_guard.disarm();
    inode_guard.disarm();
    dir_root_guard.disarm();
    return RC_OK;
}
/* ═══════════════════════════════════════════════════════════════════
 * dentry_leaf_entry_delete — 叶子条目物理删除 + 下游资产维护（DELETE_FULL）
 *
 * idx 由 dentry_leaf_node_lookup 命中时返回的 greates_lower_bound_idx。
 * 物理删除 = memmove 左移 + 清空末槽。
 *
 * 下游资产维护（2026-09-01 下沉，宗旨：删条目即维护下游）：
 *   - dir 型目标：树删**前**校验空（NOT_EMPTY 零副作用），删后全量回收
 *     （名字堆 → 树根 2 块 → inode 槽）；
 *   - REG/SOFT_SYM 目标：link_count--，归零 → regular_file_reclaim 全量
 *     回收（2026-09-07 补票：inline/direct/extent 全形态释放数据 + inode 槽，
 *     零泄露；destroy_data=true 时数据块 free 前清零）。回收失败 →
 *     RC_ERR_NOT_SUPPORTED 哨兵（树删已生效，inode 暂挂孤儿，fsck 可回收）；
 *   - 悬空条目（inum = PTR_INVALID / 位图未亮）：只删树条目，跳过资产维护。
 *
 * result_flags：
 *   bit0 = 1 → 欠载（count < 63 且非根），需合并/再平衡
 *   bit1 = 1 → 叶子最大 hash 已变，父层 hint 需刷新
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dentry_leaf_entry_delete(
    uint64_t leaf_blk_base_idx,
    uint16_t idx,
    uint64_t &result_flags,
    bool destroy_data)
{
    result_flags = 0;

    /* 会话贯穿：读-改-写同一会话页，无栈缓冲无复制；dirty 后析构写回 */
    mdata_piece_session sess(mdata_piece_mgr, leaf_blk_base_idx);
    if (sess.index_error()) return RC_ERR_STATE;
    if (!sess) return RC_ERR_IO;
    Bptree_ddentry_leaf_node_t *leaf = (Bptree_ddentry_leaf_node_t *)sess.data;
    if (!is_dentry_node(&leaf->meta)) return RC_ERR_STATE;
    if (RFS_PP_IS_INNER(leaf->meta.parent_ptr)) return RC_ERR_STATE;

    uint16_t count = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
    if (idx >= count) return RC_ERR_STATE;

    /* ── 树删前：读下游资产信息（memmove 后条目移位不可再读） ──
     * ⚠ 52 位位域读出的 inum 与 PTR_INVALID 比较必须同样截断（64 位比较恒不相等）。
     * PTR_INVALID 悬空条目 = 历史过渡态/结构异常（只删树条目、跳过资产维护）。 */
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    const uint64_t inum = RFS_PTR_ADDR(leaf->entries[idx].inode_ptr);
    const bool has_inode = (inum != PTR_INVALID_52) && inode_test(inum);

    /* ── dir 型目标：删除前置校验（目录必须全空），零副作用 ──
     * 校验必须在树删**之前**：失败不产生任何树/位图变更（rmdir 语义）。
     * 空判定：树根是叶子 && entry_count==0（. / .. 由 parent_ptr/ptrs[1]
     * 合成，不在树内计条目）。树根为内部节点 ⇒ 至少 2 子树各有条目 ⇒ 非空。 */
    uint64_t dir_tree_root = PTR_INVALID;
    uint8_t tgt_type = 0xFF;   /* 0xFF = 无有效目标（跳过资产维护） */
    if (has_inode) {
        InodeSession tgt_sess(inode_cache_mgr, inum);
        if (!tgt_sess) return RC_ERR_IO;
        tgt_type = RFS_INO1_GET_TYPE(tgt_sess.inode->word1);
        if (tgt_type == INODE_TYPE_DIR) {
            dir_tree_root = tgt_sess.inode->data_root;
            if (dir_tree_root == PTR_INVALID) return RC_ERR_STATE;   /* 契约违反：dir 必有树根（不变量 11） */
            mdata_piece_session root_sess(mdata_piece_mgr, dir_tree_root);
            if (root_sess.index_error()) return RC_ERR_STATE;
            if (!root_sess) return RC_ERR_IO;
            Bptree_meta_t *m = (Bptree_meta_t *)root_sess.data;
            if (!is_dentry_node(m)) return RC_ERR_STATE;
            if (RFS_PP_IS_INNER(m->parent_ptr))  return RC_ERR_NOT_EMPTY;
            if (RFS_PP_COUNT(m->parent_ptr) != 0) return RC_ERR_NOT_EMPTY;
        }
    }

    bool is_last = (idx == count - 1);
    uint64_t old_max_hash = leaf->entries[count - 1].hash;

    /* 左移覆盖 */
    if (idx < count - 1) {
        memmove(&leaf->entries[idx], &leaf->entries[idx + 1],
                (count - idx - 1) * sizeof(ddentry_t));
    }
    memset(&leaf->entries[count - 1], 0, sizeof(ddentry_t));
    count--;
    RFS_PP_SET_COUNT(leaf->meta.parent_ptr, count);

    /* 欠载判定：非根且低于半满 63 */
    bool is_root = RFS_PP_IS_ROOT(leaf->meta.parent_ptr);
    if (!is_root && count < 63)
        result_flags |= 1u;

    /* 父层 hash_max 刷新判定：删的是末项，或叶子已空 */
    if (is_last || count == 0)
        result_flags |= 2u;

    sess.dirty();

    /* ── 树删后：下游 inode 资产维护（DELETE_FULL）──
     * 树删已落盘：此处失败仅上报错误，孤儿/计数偏差由 fsck 修正。 */
    bool need_reclaim = false;   /* REG/SOFT_SYM 归零：延后到 tgt_sess 析构后执行 */
    if (has_inode) {
        InodeSession tgt_sess(inode_cache_mgr, inum);
        if (!tgt_sess) return RC_ERR_IO;
        if (tgt_type == INODE_TYPE_DIR) {
            /* 空目录全量回收（rmdir 语义）：名字堆 → 树根 2 块 → inode 槽。
             * 尽力而为：失败不覆盖删除结果（树删已成功）。 */
            dir_name_heap_release(inum);
            if (dir_tree_root != PTR_INVALID)
                block_free(dir_tree_root, 2);
            inode_free(inum);
        } else {
            /* REGULAR / SOFT_SYM：link_count--；归零 → 全量空间回收 */
            uint32_t lc = tgt_sess.inode->link_count;
            if (lc == 0) return RC_ERR_STATE;   /* 契约违反：已分配 inode link_count 恒 ≥ 1（不变量 17） */
            lc--;
            tgt_sess.inode->link_count = lc;
            tgt_sess.dirty();
            if (lc == 0) need_reclaim = true;
        }
    }

    /* ── 归零全量回收（2026-09-07 补票，tgt_sess 析构后执行避免同 inode 双会话）──
     * regular_file_reclaim 按形态释放数据（extent 叶链遍历 + 自底向上释节点 /
     * direct 区间 / inline 内联）+ inode 槽，零泄露。失败 → 哨兵：树删 +
     * link_count=0 已落盘，inode 孤儿暂挂（位图亮），不静默泄漏、不悬挂，
     * fsck 可识别回收。 */
    if (need_reclaim) {
        if (regular_file_reclaim(inum, destroy_data) != RC_OK)
            return RC_ERR_NOT_SUPPORTED;
    }
    return RC_OK;
}

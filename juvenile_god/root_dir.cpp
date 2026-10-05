#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include "../include/ravenfs/inode_cache_radix.hpp"   /* InodeSession：inode 访问统一走缓存（offload 零拷贝指针） */
#include <cstring>

/* ═══════════════════════════════════════════════════════════════════
 * root_dir.cpp — 目录 dentry 顶层业务（调 dentry 树层 + 名字数据面）
 *
 * dir_insert_dentry / dir_rm_dentry / dir_lookup —— 任意目录（inode_idx 指定）
 * root_dir_* —— root 专属薄封装（内部转调 dir_*）
 * 零缓存原则：所有块/inode 访问直接 pread/pwrite，无缓存层（名字数据面
 * 块经 mdata_piece_caches 会话访问，见 name_data_plane.cpp）。
 *
 * v1.1 目录生命周期契约（disk_layout_v1.1.md §10 不变量 11）：
 *   DIR inode 的 data_root **恒 = dentry 树根块号，PTR_INVALID 非法**。
 *   根目录树根由 mkfs 建；子目录树根在创建时（本文件 dir 型初始化）急建。
 *   → 目录从诞生起树根即存在，"空目录懒建根"（旧两参版本行为）退役，
 *     任何时刻见到 dir 的 data_root==PTR_INVALID = 严重内部磁盘契约违反。
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * dir_insert_dentry — 在 inode_idx 目录中插入名为 name 的条目（按类型初始化）
 *
 * 2026-09-01 下沉重构：目标 inode 创建（DIR 急建树根 / REG/SOFT_SYM 内联）、
 * 长名外置分配、owner 计数维护全部下沉 dentry_leaf_entry_insert；本函数只留
 * 编排：校验 → walk 查重（EXIST 提前返回，不浪费 inode/堆/树根）→ 调 insert
 * → 刷父 hash_max → 分裂递归（leaf_split / new_root_inner / spilt_parent_apply）。
 *
 * 目标目录契约：inode_idx 的 data_root 恒有效（根由 mkfs、子目录由 insert
 * 建根）。因此**不做**空目录懒建根；data_root==PTR_INVALID → 严重内部磁盘
 * 契约违反 → RC_ERR_STATE。
 *
 * 新建 dir 根叶子细节见 dentry_leaf_entry_insert（与 mkfs build_root_dentry_node
 * 对齐，差异仅 ".."=父目录）；dir inode 初始化要点：
 *   link_count = 2（POSIX：2 + 直属 DIR 型子 inode 数，空目录即 2，§3.3）、
 *   permission=0755、ddentry_Bptree_root=新建
 *   树根、**名字数据面双树显式空态哨兵（RFS_ND_CTL_INACTIVE / RFS_ND_FR_CTL_INACTIVE）**
 *   —— memset 0 是雷：0 是合法块号，旧 dir_name_heap_release 曾误把 block 0/1
 *   （根 dentry 块）当激活堆释放。
 *
 * 名字把关（fs 级，2026-10-05 前置）：name_check_create 在**任何** inode/hash/树
 * 访问之前执行——空名（首字节 NUL）与含 '/' 的名字 → RC_ERR_NAME_INVALID，
 * > 552B → RC_ERR_NAME_LEN；失败零副作用（不分配 inode/数据面、不碰树）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dir_insert_dentry(uint64_t inode_idx, const char *name, uint8_t inode_type)
{
    result_code_t nrc = name_check_create(name);
    if (nrc) return nrc;
    if (!inode_test(inode_idx))
        return RC_ERR_STATE;
    if (inode_type != INODE_TYPE_REGULAR &&
        inode_type != INODE_TYPE_DIR &&
        inode_type != INODE_TYPE_SOFT_SYM)
        return RC_ERR_STATE;

    name_cmlpx_t cmplx = {
        .name = (char*)name,
        .hash = name_hash(name),
        .name_len = (uint16_t)strlen(name),
        .full_char_remain_offset = PTR_INVALID,
        .owner_dir_inum = inode_idx
    };

    uint64_t dentry_root_idx;
    {
        InodeSession ino_sess(inode_cache_mgr, inode_idx);
        if (!ino_sess) return RC_ERR_IO;
        raven_dir_Inode_t *ino = (raven_dir_Inode_t *)ino_sess.inode;
        if (RFS_INO1_GET_TYPE(ino->word1) != INODE_TYPE_DIR)
            return RC_ERR_STATE;

        /* 契约检查：目标目录 data_root 恒有效（v1.1 §10 不变量 11）。
         * 三参数版本**拒绝**空 data_root —— 出现即严重内部磁盘契约违反
         * （dir 从诞生起树根已建：根目录由 mkfs，子目录由本函数急建）。
         * 旧两参版本的"空目录懒建根"在此退役。 */
        if (ino->ddentry_Bptree_root == PTR_INVALID)
            return RC_ERR_STATE;
        dentry_root_idx = ino->ddentry_Bptree_root;
    }

    bptree_path_t trace;
    result_code_t rc = dentry_Bptree_walk(dentry_root_idx, &trace, &cmplx);
    if (rc) return rc;
    if (trace.is_leaf_hit) return RC_ERR_EXIST;

    /* ── 目标 inode 创建 + 长名分配 + owner 计数：全部下沉 dentry_leaf_entry_insert
     * （2026-09-01；失败 guard 全回滚）。 ── */
    uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    uint16_t glb = trace.child_idx[trace.depth - 1];
    uint16_t insert_idx = (glb == DENTRY_LEAF_GLB_NONE) ? 0 : (glb + 1);

    uint64_t flags = 0;
    rc = dentry_leaf_entry_insert(leaf_blk, &cmplx, insert_idx, flags, inode_type, inode_idx);
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
            Bptree_ddentry_inner_node_t *parent = (Bptree_ddentry_inner_node_t*)parent_sess.data;

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
            rc = dentry_new_root_inner(split_res.low_node_blkidx,
                                       split_res.low_node_maxhash,
                                       split_res.high_node_blkidx,
                                       inode_idx,
                                       split_res.old_root_ptrs1);
            if (rc) return rc;
            return RC_OK;
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
            Bptree_ddentry_inner_node_t *parent_node = (Bptree_ddentry_inner_node_t*)parent_sess.data;   /* 只读 */

            bool parent_is_root = RFS_PP_IS_ROOT(parent_node->meta.parent_ptr);
            split_result_t inner_split = dentry_inner_split(cur_parent_blk);
            if (inner_split.rc != RC_OK) return inner_split.rc;

            if (parent_is_root) {
                // 根内部节点溢出，创建新根
                rc = dentry_new_root_inner(inner_split.low_node_blkidx,
                                           inner_split.low_node_maxhash,
                                           inner_split.high_node_blkidx,
                                           inode_idx,
                                           inner_split.old_root_ptrs1);
                if (rc) return rc;
                need_parent_split = false;
                break;
            } else {
                // 非根内部节点，向祖父节点应用分裂
                uint64_t grand_parent_blk = RFS_PP_PTR(parent_node->meta.parent_ptr);
                // 查找 cur_parent_blk 在祖父节点中的索引
                mdata_piece_session grand_sess(mdata_piece_mgr, grand_parent_blk);
                if (grand_sess.index_error()) return RC_ERR_STATE;
                if (!grand_sess) return RC_ERR_IO;
                Bptree_ddentry_inner_node_t *grand = (Bptree_ddentry_inner_node_t*)grand_sess.data;   /* 只读 */
                uint16_t grand_idx = 0;
                bool found = false;
                for (uint16_t k = 0; k < RFS_PP_COUNT(grand->meta.parent_ptr); ++k) {
                    if (RFS_PTR_ADDR(grand->hints[k].sub_ptr) == cur_parent_blk) {
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
 * 流程：walk → 命中校验 → 叶子删除（dentry_leaf_entry_delete：树删 +
 * 下游资产维护——dir 型目标前置空校验 + 全量回收，REG/SOFT_SYM
 * link_count--/归零回收哨兵）→ 刷父 hash_max → 欠载平衡/合并（向上递归）
 * → owner 目录条目计数维护（size--；DIR 型条目 link_count--，§3.3）→ 长名堆释放。
 *
 * 目标 inode 侧资产全部下沉 dentry_leaf_entry_delete（2026-09-01）；本函数
 * 保留 owner 侧（当前目录）计数与长名释放——需 inode_idx，签名无 owner 参数。
 * RC_ERR_NOT_SUPPORTED = 归零全量空间回收未实现哨兵：树删 + link_count--
 * 已生效，inode 暂挂孤儿（位图亮 + 计数 0，fsck 可回收）；流程照常走完。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dir_rm_dentry(uint64_t inode_idx, const char *name, bool destroy_data)
{
    if (!name_len_limiter(name))
        return RC_ERR_NAME_LEN;
    if (!inode_test(inode_idx))
        return RC_ERR_STATE;

    name_cmlpx_t cmplx = {
        .name = (char*)name,
        .hash = name_hash(name),
        .name_len = (uint16_t)strlen(name),
        .full_char_remain_offset = PTR_INVALID,
        .owner_dir_inum = inode_idx
    };

    uint64_t dentry_root_idx;
    {
        InodeSession ino_sess(inode_cache_mgr, inode_idx);
        if (!ino_sess) return RC_ERR_IO;
        raven_dir_Inode_t *ino = (raven_dir_Inode_t *)ino_sess.inode;
        if (RFS_INO1_GET_TYPE(ino->word1) != INODE_TYPE_DIR)
            return RC_ERR_STATE;
        if (ino->ddentry_Bptree_root == PTR_INVALID)
            return RC_ERR_NOT_FOUND;   /* 空目录，无条目可删 */
        dentry_root_idx = ino->ddentry_Bptree_root;
    }

    bptree_path_t trace;
    result_code_t rc = dentry_Bptree_walk(dentry_root_idx, &trace, &cmplx);
    if (rc) return rc;
    if (!trace.is_leaf_hit) return RC_ERR_NOT_FOUND;

    uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    uint16_t idx = trace.child_idx[trace.depth - 1];

    mdata_piece_session leaf_sess(mdata_piece_mgr, leaf_blk);
    if (leaf_sess.index_error()) return RC_ERR_STATE;
    if (!leaf_sess) return RC_ERR_IO;
    Bptree_ddentry_leaf_node_t *leaf = (Bptree_ddentry_leaf_node_t*)leaf_sess.data;   /* 只读（取名字数据面信息） */

    /* 名字数据面信息必须在删除（memmove）**之前**取 */
    const uint16_t e_name_len = (uint16_t)RFS_PTR_FREE(leaf->entries[idx].inode_ptr);
    const uint64_t e_remain   = leaf->entries[idx].full_char_remain_offset;

    /* 目标 inode 类型也须在删除前读出（D-12）：仅 DIR 型条目才影响 owner
     * 目录 link_count（POSIX：link_count = 2 + 直属 DIR 型子 inode 数）。 */
    bool tgt_is_dir = false;
    {
        const uint64_t tgt_inum = RFS_PTR_ADDR(leaf->entries[idx].inode_ptr);
        if (tgt_inum != ((uint64_t)PTR_INVALID & ((1ULL << 52) - 1)) && inode_test(tgt_inum)) {
            InodeSession tgt_sess(inode_cache_mgr, tgt_inum);
            if (!tgt_sess) return RC_ERR_IO;
            tgt_is_dir = (RFS_INO1_GET_TYPE(tgt_sess.inode->word1) == INODE_TYPE_DIR);
        }
    }

    uint64_t flags = 0;
    rc = dentry_leaf_entry_delete(leaf_blk, idx, flags, destroy_data);
    /* RC_ERR_NOT_SUPPORTED = 归零全量回收未实现哨兵：树删 + link_count-- 已生效，
     * inode 暂挂孤儿。继续完成删除流程（平衡 + owner 计数维护），最后上报。 */
    const bool orphan_pending = (rc == RC_ERR_NOT_SUPPORTED);
    if (rc && !orphan_pending) return rc;

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
            Bptree_ddentry_inner_node_t *parent = (Bptree_ddentry_inner_node_t*)parent_sess.data;

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
                if (!RFS_PP_IS_ROOT(m->parent_ptr)) return false;
                if (!RFS_PP_IS_INNER(m->parent_ptr)) return false;
                if (RFS_PP_COUNT(m->parent_ptr) != 1) return false;
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
                Bptree_ddentry_inner_node_t *parent_node = (Bptree_ddentry_inner_node_t*)parent_sess.data;   /* 只读 */

                if (RFS_PP_IS_ROOT(parent_node->meta.parent_ptr)) {
                    /* 根内部节点只剩 1 孩子（合并残留）→ 收缩 */
                    try_root_shrink(cur_parent_blk);
                    break;
                }

                uint64_t grand_parent_blk = RFS_PP_PTR(parent_node->meta.parent_ptr);
                mdata_piece_session grand_sess(mdata_piece_mgr, grand_parent_blk);
                if (grand_sess.index_error()) return RC_ERR_STATE;
                if (!grand_sess) return RC_ERR_IO;
                Bptree_ddentry_inner_node_t *grand = (Bptree_ddentry_inner_node_t*)grand_sess.data;   /* 只读 */
                uint16_t grand_idx = 0;
                bool found = false;
                for (uint16_t k = 0; k < RFS_PP_COUNT(grand->meta.parent_ptr); ++k) {
                    if (RFS_PTR_ADDR(grand->hints[k].sub_ptr) == cur_parent_blk) {
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

    /* 长名：释放名字数据面外置区间（树删除已成功；释放失败尽力而为，不覆盖删除结果） */
    if (e_name_len > DENTRY_INLINE_NAME_MAX)
        name_data_free(inode_idx, e_remain, (uint16_t)(e_name_len - DENTRY_INLINE_NAME_MAX));

    /* 当前目录（owner）条目计数维护：size--（全部条目）；tgt_is_dir 时
     * link_count--（POSIX：link_count = 2 + 直属 DIR 型子 inode 数，§3.3）。
     * owner 侧留上层——签名无 owner 参数；目标 inode 侧资产已由
     * dentry_leaf_entry_delete 下沉处理。 */
    {
        InodeSession dir_sess(inode_cache_mgr, inode_idx);
        if (!dir_sess) return RC_ERR_IO;
        if (dir_sess.inode->size > 0)
            dir_sess.inode->size--;
        if (tgt_is_dir && dir_sess.inode->link_count > 2)
            dir_sess.inode->link_count--;
        dir_sess.dirty();
    }
    return orphan_pending ? RC_ERR_NOT_SUPPORTED : RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * dir_lookup — 在 inode_idx 目录中查找 name
 *
 * 命中 → RC_OK 且 *out_inum 输出条目 inode 编号；
 * 未命中 → RC_ERR_NOT_FOUND；目录无效/非目录 → RC_ERR_STATE。
 * （insert 已落真实 inum：命中输出即新文件 inode 槽）
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dir_lookup(uint64_t inode_idx, const char *name, uint64_t *out_inum)
{
    if (!out_inum) return RC_ERR_STATE;
    if (!name_len_limiter(name)) return RC_ERR_NAME_LEN;
    if (!inode_test(inode_idx)) return RC_ERR_STATE;

    name_cmlpx_t cmplx = {
        .name = (char*)name,
        .hash = name_hash(name),
        .name_len = (uint16_t)strlen(name),
        .full_char_remain_offset = PTR_INVALID,
        .owner_dir_inum = inode_idx
    };

    uint64_t dentry_root_idx = 0;
    {
        InodeSession ino_sess(inode_cache_mgr, inode_idx);
        if (!ino_sess) return RC_ERR_IO;
        raven_dir_Inode_t *ino = (raven_dir_Inode_t *)ino_sess.inode;
        if (RFS_INO1_GET_TYPE(ino->word1) != INODE_TYPE_DIR) return RC_ERR_STATE;
        if (ino->ddentry_Bptree_root == PTR_INVALID) return RC_ERR_NOT_FOUND;   /* 空目录 */
        dentry_root_idx = ino->ddentry_Bptree_root;
    }

    bptree_path_t trace;
    result_code_t rc = dentry_Bptree_walk(dentry_root_idx, &trace, &cmplx);
    if (rc) return rc;
    if (!trace.is_leaf_hit) return RC_ERR_NOT_FOUND;

    uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    uint16_t idx = trace.child_idx[trace.depth - 1];

    mdata_piece_session leaf_sess(mdata_piece_mgr, leaf_blk);
    if (leaf_sess.index_error()) return RC_ERR_STATE;
    if (!leaf_sess) return RC_ERR_IO;
    Bptree_ddentry_leaf_node_t *leaf = (Bptree_ddentry_leaf_node_t*)leaf_sess.data;   /* 只读 */
    if (RFS_PP_IS_INNER(leaf->meta.parent_ptr)) return RC_ERR_STATE;

    *out_inum = RFS_PTR_ADDR(leaf->entries[idx].inode_ptr);
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * root_dir_* — root 专属薄封装（任意目录版见 dir_*）
 * ═══════════════════════════════════════════════════════════════════ */
/* 正版：按 inode_type 初始化（REGULAR 内联文件 / DIR 建根 / SOFT_SYM 内联） */
result_code_t RavenFS_juvenile::root_dir_insert_dentry(const char *name, uint8_t inode_type)
{
    return dir_insert_dentry(sb.root_inode, name, inode_type);
}
result_code_t RavenFS_juvenile::root_dir_rm_dentry(const char *name, bool destroy_data)
{
    return dir_rm_dentry(sb.root_inode, name, destroy_data);
}
result_code_t RavenFS_juvenile::root_dir_lookup(const char *name, uint64_t *out_inum)
{
    return dir_lookup(sb.root_inode, name, out_inum);
}

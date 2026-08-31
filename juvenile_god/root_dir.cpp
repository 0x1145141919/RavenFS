#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include "../include/ravenfs/inode_cache_radix.hpp"   /* InodeSession：inode 访问统一走缓存（offload 零拷贝指针） */
#include <cstring>

/* ═══════════════════════════════════════════════════════════════════
 * root_dir.cpp — RavenFS v1.1 目录 dentry 顶层业务（调 dentry 树层 + 名字堆）
 *
 * dir_insert_dentry / dir_rm_dentry / dir_lookup —— 任意目录（inode_idx 指定）
 * root_dir_* —— root 专属薄封装（内部转调 dir_*）
 * 零缓存原则：所有块/inode 访问直接 pread/pwrite，无缓存层（名字堆块经
 * mdata_piece_caches 会话访问，见 name_heap.cpp）。
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
 * 三参数核心版本（v1.1 正式形态）：
 *   inode_type == INODE_TYPE_DIR     → 新建 dir inode + 急建其 dentry 树根
 *                                      （根叶子 2 块：magic + . / .. 指针齐活）
 *   inode_type == REGULAR/SOFT_SYM   → 内联空 inode（全零 inline_data）
 *
 * 目标目录契约：inode_idx 的 data_root 恒有效（根由 mkfs、子目录由本函数
 * 建根）。因此本版本**不做**空目录懒建根；data_root==PTR_INVALID → 严重
 * 内部磁盘契约违反 → RC_ERR_STATE。
 *
 * 新建 dir 根叶子（与 mkfs build_root_dentry_node 对齐，差异仅 ".."）：
 *   parent_ptr = {cnt:0, inner:0, root:1, ptr=新dir自身inode}   (".", 自身)
 *   ptrs[0]    = PTR_INVALID   （预留）
 *   ptrs[1]    = inode_idx     （".." = 父目录 inode；旧懒建路径的孤儿自指
 *                                由 mkdir 接入父链后显式提供，本版一步到位）
 *   ptrs[2]    = PTR_INVALID   （预留，根叶子不参与同级链表）
 *
 * dir inode 初始化要点（对照 mkfs build_root_inode）：
 *   link_count=2（父目录条目 + 自身 "."）、permission=0755、size=0、
 *   data_root=新建树根、**10 名字堆描述符显式未激活哨兵** —— memset 0 是雷：
 *   0 是合法块号，dir 关闭时 dir_name_heap_release 会误把 block 0/1（根
 *   dentry 块）当激活堆释放。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dir_insert_dentry(uint64_t inode_idx, const char *name, uint8_t inode_type)
{
    if (!name_len_limiter(name))
        return RC_ERR_NAME_LEN;
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
        .full_char_remain_ptr = PTR_INVALID
    };

    uint64_t dentry_root_idx;
    {
        InodeSession ino_sess(inode_cache_mgr, inode_idx);
        if (!ino_sess) return RC_ERR_IO;
        raven_Inode_t *ino = ino_sess.inode;
        if (ino->type != INODE_TYPE_DIR)
            return RC_ERR_STATE;

        /* 契约检查：目标目录 data_root 恒有效（v1.1 §10 不变量 11）。
         * 三参数版本**拒绝**空 data_root —— 出现即严重内部磁盘契约违反
         * （dir 从诞生起树根已建：根目录由 mkfs，子目录由本函数急建）。
         * 旧两参版本的"空目录懒建根"在此退役。 */
        if (ino->data_root == PTR_INVALID)
            return RC_ERR_STATE;
        dentry_root_idx = ino->data_root;
    }

    dentry_bptree_path_t trace;
    result_code_t rc = dentry_Bptree_walk(dentry_root_idx, &trace, &cmplx);
    if (rc) return rc;
    if (trace.is_finally_hit) return RC_ERR_EXIST;

    /* ── 真正 inode 操作：分配 inode 槽 + 按类型初始化 ──
     * 放在 EXIST 检查后（不浪费 inode 槽 / 树根块）。InodeSession 直改缓存页
     * （no_cache 写回 / offload 直映内核页缓存 / self_reliat 脏标记，
     * 三后端表面一致；读锁不跨会话，可放心与 mdata_piece_session 嵌套）。
     * inode_guard 兜底 insert/分裂链路任何失败自动 inode_free；
     * dir 型另配 block_guard 兜底新建树根 2 块；
     * **所有成功路径必须 disarm**（与 heap_guard 同纪律）。 */
    uint64_t new_inum = inode_alloc();
    if (new_inum == PTR_INVALID) return RC_ERR_NO_INODE;
    inode_guard_t inode_guard;
    inode_guard.arm(this, new_inum);
    block_guard_t dir_root_guard;   /* dir 型专用：新建树根 2 块回滚 */

    if (inode_type == INODE_TYPE_DIR) {
        /* ── dir 型：先建根叶子（2 块），再初始化 dir inode ──
         * mdata_piece_session 直接覆盖会话页（无栈缓冲/无复制），
         * 会话页整块清零 + 填 meta，dirty() 后析构写回。 */
        uint64_t root_blk = block_alloc_hard(2, 1);
        if (root_blk == PTR_INVALID) return RC_ERR_NO_BLOCK;   /* guard 自动回滚 inode */
        dir_root_guard.arm(this, root_blk, 2);

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
        rleaf->meta.parent_ptr.ptr           = new_inum;   /* "." = 自身 inode */
        rleaf->meta.ptrs[0].ptr = PTR_INVALID;
        rleaf->meta.ptrs[1].ptr = inode_idx;               /* ".." = 父目录 inode */
        rleaf->meta.ptrs[2].ptr = PTR_INVALID;
        root_sess.dirty();

        {
            InodeSession ino_sess(inode_cache_mgr, new_inum);
            if (!ino_sess) return RC_ERR_IO;   /* guard 自动回滚 */
            raven_dir_Inode_t *dino = (raven_dir_Inode_t*)ino_sess.inode;
            memset(dino, 0, sizeof(raven_dir_Inode_t));
            dino->type            = INODE_TYPE_DIR;
            dino->link_count      = 2;         /* 父目录条目 + 自身 "." */
            dino->permission_bits = 0755;
            dino->size            = 0;         /* 新目录无条目 */
            dino->data_root       = root_blk;  /* 恒有效（不变量 11） */
            /* 名字堆 10 描述符：未激活哨兵（memset 0 是雷 —— 0 是合法块号，
             * dir_name_heap_release 会误当激活堆释放 block 0/1 根块） */
            for (int i = 0; i < 10; i++) {
                ((name_heap_controller_t *)&dino->heaps_8B_8k)[i].bitmap_base_block_ptr = NAME_HEAP_INACTIVE;
                ((name_heap_controller_t *)&dino->heaps_8B_8k)[i].data_base_block_ptr   = NAME_HEAP_INACTIVE;
            }
            ino_sess.dirty();
        }
    } else {
        /* ── REGULAR / SOFT_SYM：内联空 inode（全零 inline_data） ── */
        InodeSession ino_sess(inode_cache_mgr, new_inum);
        if (!ino_sess) return RC_ERR_IO;   /* guard 自动回滚 */
        raven_Inode_t *ino = ino_sess.inode;
        memset(ino, 0, sizeof(raven_Inode_t));   /* 全零：inline_data = 内联空文件 */
        ino->type            = inode_type;
        ino->link_count      = 1;
        ino->permission_bits = 0644;
        ino->data_root       = PTR_INVALID;   /* 内联空文件/软链，无数据块 */
        ino_sess.dirty();
    }
    cmplx.inum = new_inum;   /* dentry 落真实 inum（dentry_leaf_entry_insert 消费） */

    /* ── 长名（>40B）：先分配名字堆外置存储 + 写尾部，再插入 ──
     * 分配置于 EXIST 检查后（不浪费堆空间）；guard 兜底 insert/分裂
     * 链路任何失败自动回滚；**所有成功路径必须 disarm**（分裂建新根的
     * return 路径曾绕过 disarm → 析构误触发 free → 位图被清）。 */
    name_heap_guard_t heap_guard;
    if (cmplx.name_len > DENTRY_INLINE_NAME_MAX) {
        name_heap_loc_t loc;
        rc = name_heap_alloc(inode_idx, cmplx.name_len - DENTRY_INLINE_NAME_MAX, &loc);
        if (rc) return rc;
        cmplx.full_char_remain_ptr = (loc.fblk << 12) | (uint64_t)loc.offset;
        heap_guard.arm(this, inode_idx, cmplx.name_len, cmplx.full_char_remain_ptr);
        rc = name_heap_write(cmplx.name_len, cmplx.full_char_remain_ptr,
                             name + DENTRY_INLINE_NAME_MAX);
        if (rc) return rc;   /* guard 自动回滚 */
    }

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
            rc = dentry_new_root_inner(split_res.low_node_blkidx,
                                       split_res.low_node_maxhash,
                                       split_res.high_node_blkidx,
                                       inode_idx);
            if (rc) return rc;   /* 失败：guard 自动回滚 */
            heap_guard.disarm();   /* 成功：分裂建新根路径必须转正 */
            inode_guard.disarm();  /* inode 同转正 */
            dir_root_guard.disarm();  /* dir 树根同转正 */
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

    heap_guard.disarm();   /* 树插入全链路成功，堆分配转正 */
    inode_guard.disarm();  /* inode 转正 */
    dir_root_guard.disarm();  /* dir 树根转正 */
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * dir_insert_dentry — 便捷封装（REGULAR 普通文件语义）
 *
 * 旧两参版本的空目录懒建根逻辑**退役**：v1.1 不变量 11 下 dir 的
 * data_root 恒有效（根目录 mkfs 建根、子目录三参版急建根），不再存在
 * "空目录等首次插入才建根"的合法窗口期。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dir_insert_dentry(uint64_t inode_idx, const char *name)
{
    return dir_insert_dentry(inode_idx, name, INODE_TYPE_REGULAR);
}

/* ═══════════════════════════════════════════════════════════════════
 * dir_rm_dentry — 从 inode_idx 目录物理删除名为 name 的条目
 *
 * 流程：walk → 命中校验 → **dir 型目标前置检查（必须全空）** → 叶子删除
 * （memmove + 刷父 hash_max）→ 欠载平衡/合并（向上递归）→ 清理（dir 型：
 * 名字堆回收 + 树根 2 块释放）→ inode_free（条目 inode 有效时）。
 *
 * dir 型空判定：树根是叶子 && entry_count==0（. / .. 由 parent_ptr/ptrs[1]
 * 合成，不在树内计条目；inode.size 当前未维护，不作判据）。树根为内部
 * 节点 ⇒ 至少 2 子树各有条目 ⇒ 非空。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dir_rm_dentry(uint64_t inode_idx, const char *name)
{
    if (!name_len_limiter(name))
        return RC_ERR_NAME_LEN;
    if (!inode_test(inode_idx))
        return RC_ERR_STATE;

    name_cmlpx_t cmplx = {
        .name = (char*)name,
        .hash = name_hash(name),
        .name_len = (uint16_t)strlen(name),
        .full_char_remain_ptr = PTR_INVALID
    };

    uint64_t dentry_root_idx;
    {
        InodeSession ino_sess(inode_cache_mgr, inode_idx);
        if (!ino_sess) return RC_ERR_IO;
        raven_Inode_t *ino = ino_sess.inode;
        if (ino->type != INODE_TYPE_DIR)
            return RC_ERR_STATE;
        if (ino->data_root == PTR_INVALID)
            return RC_ERR_NOT_FOUND;   /* 空目录，无条目可删 */
        dentry_root_idx = ino->data_root;
    }

    dentry_bptree_path_t trace;
    result_code_t rc = dentry_Bptree_walk(dentry_root_idx, &trace, &cmplx);
    if (rc) return rc;
    if (!trace.is_finally_hit) return RC_ERR_NOT_FOUND;

    uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    uint16_t idx = trace.child_idx[trace.depth - 1];

    mdata_piece_session leaf_sess(mdata_piece_mgr, leaf_blk);
    if (leaf_sess.index_error()) return RC_ERR_STATE;
    if (!leaf_sess) return RC_ERR_IO;
    Bptree_dentry_leaf_node_t *leaf = (Bptree_dentry_leaf_node_t*)leaf_sess.data;   /* 只读（取条目 inum / 名字堆信息） */
    uint64_t inum = leaf->entries[idx].inode_ptr.ptr;
    /* ⚠ 52 位位域读出的 inum 与 PTR_INVALID 比较必须同样截断（64 位比较恒不相等）。
     * insert 已落真实 inum；PTR_INVALID 悬空条目 = 历史过渡态/结构异常
     * （此时只删树条目、跳过 inode_free）。 */
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    const bool has_inode = (inum != PTR_INVALID_52);

    /* ── dir 型目标：删除前置检查（目录必须全空）──
     * 在树删除**之前**校验：非空目录拒绝删除（RC_ERR_NOT_EMPTY），
     * 不产生任何树/位图变更。空判定见函数头注释。 */
    bool target_is_dir = false;
    uint64_t dir_tree_root = PTR_INVALID;
    if (has_inode && inode_test(inum)) {
        InodeSession tgt_sess(inode_cache_mgr, inum);
        if (!tgt_sess) return RC_ERR_IO;
        if (tgt_sess.inode->type == INODE_TYPE_DIR) {
            target_is_dir = true;
            dir_tree_root = tgt_sess.inode->data_root;
            if (dir_tree_root == PTR_INVALID)
                return RC_ERR_STATE;   /* 契约违反：dir 必有树根（不变量 11） */
            mdata_piece_session root_sess(mdata_piece_mgr, dir_tree_root);
            if (root_sess.index_error()) return RC_ERR_STATE;
            if (!root_sess) return RC_ERR_IO;
            Bptree_meta_t *m = (Bptree_meta_t *)root_sess.data;
            if (!is_dentry_node(m)) return RC_ERR_STATE;
            if (m->parent_ptr.is_inner_node)  return RC_ERR_NOT_EMPTY;
            if (m->parent_ptr.entry_count != 0) return RC_ERR_NOT_EMPTY;
        }
    }

    /* 名字堆信息必须在删除（memmove）**之前**取 */
    const uint16_t e_name_len = (uint16_t)leaf->entries[idx].inode_ptr.free_to_use;
    const uint64_t e_remain   = leaf->entries[idx].full_char_remain_ptr;

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

    /* 真正 inode 操作：释放条目指向的 inode 槽（insert 已落真实 inum）。
     * inode_test 双保险：位图未亮灯的 inum 视为异常态，不再重复释放
     * （free_interval 本幂等，此守卫纯防御）。 */
    if (has_inode && inode_test(inum)) {
        if (target_is_dir) {
            /* 空目录清理（树删除已成功）：名字堆回收（尽力而为，失败不
             * 覆盖删除结果）→ 释放该目录树根 2 块 → inode 槽。 */
            dir_name_heap_release(inum);
            block_free(dir_tree_root, 2);
        }
        inode_free(inum);
    }
    /* 长名：释放名字堆外置存储（树删除已成功；释放失败尽力而为，不覆盖删除结果） */
    if (e_name_len > DENTRY_INLINE_NAME_MAX)
        name_heap_free(inode_idx, e_name_len, e_remain);
    return RC_OK;
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
        .full_char_remain_ptr = PTR_INVALID
    };

    uint64_t dentry_root_idx = 0;
    {
        InodeSession ino_sess(inode_cache_mgr, inode_idx);
        if (!ino_sess) return RC_ERR_IO;
        raven_Inode_t *ino = ino_sess.inode;
        if (ino->type != INODE_TYPE_DIR) return RC_ERR_STATE;
        if (ino->data_root == PTR_INVALID) return RC_ERR_NOT_FOUND;   /* 空目录 */
        dentry_root_idx = ino->data_root;
    }

    dentry_bptree_path_t trace;
    result_code_t rc = dentry_Bptree_walk(dentry_root_idx, &trace, &cmplx);
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
/* 正版：按 inode_type 初始化（REGULAR 内联文件 / DIR 建根 / SOFT_SYM 内联） */
result_code_t RavenFS_juvenile::root_dir_insert_dentry(const char *name, uint8_t inode_type)
{
    return dir_insert_dentry(sb.root_inode, name, inode_type);
}
/* 便捷封装：REGULAR 普通文件语义（兼容既有调用/测试） */
result_code_t RavenFS_juvenile::root_dir_insert_dentry(const char *name)
{
    return root_dir_insert_dentry(name, INODE_TYPE_REGULAR);
}
result_code_t RavenFS_juvenile::root_dir_rm_dentry(const char *name)
{
    return dir_rm_dentry(sb.root_inode, name);
}
result_code_t RavenFS_juvenile::root_dir_lookup(const char *name, uint64_t *out_inum)
{
    return dir_lookup(sb.root_inode, name, out_inum);
}

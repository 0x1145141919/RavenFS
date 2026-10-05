#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include "../include/ravenfs/inode_cache_radix.hpp"   /* InodeSession（v2.0） */
#include <cstring>

/* ═══════════════════════════════════════════════════════════════════
 * dentry_split.cpp — dentry B+tree 分裂域 + 节点基建（原 dentry_tree.cpp 拆出）
 *
 * 本文件负责：
 *   dentry_node_meta_init —— static 节点基建（调用点全在本文件：
 *     leaf_split / inner_split / new_root_inner，无跨文件需求）
 *   dentry_leaf_split / dentry_inner_split（126 阶：左 63 右 64）
 *   dentry_spilt_parent_apply / dentry_new_root_inner
 *
 * 126 阶机械契约：分裂不修改父层；调用者拿 split_result_t 后自行把
 * low_node_maxhash 提升到父层新增 hint（或根分裂时建新根）。
 * 与 extent 侧的差异：dentry 分裂函数自身 alloc（无预算池——2026-09-07
 * extent 预算化是 map 原子性改造，dentry 插入路径的失败面模型不同）。
 * ═══════════════════════════════════════════════════════════════════ */

/* ── 新建 dentry 节点通用初始化：整块清零 + 256-bit 魔数 + parent_ptr 四件套 ──
 * node_buf 为 8192B 节点缓冲（leaf/inner 通用，meta 在头部）；
 * ptrs[3] 按角色由调用方填充（分裂/建根各有语义）。 */
static void dentry_node_meta_init(void *node_buf, uint16_t entry_count,
                                  bool is_inner, bool is_root, uint64_t parent_ptr) {
    memset(node_buf, 0, 8192);
    Bptree_meta_t *meta = (Bptree_meta_t *)node_buf;
    meta->magics[0] = DENTRY_MAGIC1;
    meta->magics[1] = DENTRY_MAGIC2;
    meta->magics[2] = DENTRY_MAGIC3;
    meta->magics[3] = DENTRY_MAGIC4;
    RFS_PP_SET_COUNT(meta->parent_ptr, entry_count);
    RFS_PP_SET_IS_INNER(meta->parent_ptr, is_inner ? 1 : 0);
    RFS_PP_SET_IS_ROOT(meta->parent_ptr, is_root ? 1 : 0);
    RFS_PP_SET_PTR(meta->parent_ptr, parent_ptr);
}

/* ═══════════════════════════════════════════════════════════════════
 * dentry_leaf_split — 叶子分裂（126 阶，左 63 右 64）
 *
 * 不修改父层；调用者拿到 split_result_t 后自行把 low_node_maxhash
 * 提升到父层新增 hint 中（或根分裂时建新根）。
 *
 * 叶子链表维护：
 *    victim(左) → new(右) → old_next
 *   old_prev → victim 不变
 *   new.prev = victim, new.next = old_next
 *   old_next.prev = new (若存在)
 * ═══════════════════════════════════════════════════════════════════ */
RavenFS_juvenile::split_result_t RavenFS_juvenile::dentry_leaf_split(uint64_t victim_node_blkbase)
{
    split_result_t res;
    res.rc = RC_OK;
    res.low_node_blkidx = PTR_INVALID;
    res.high_node_blkidx = PTR_INVALID;
    res.low_node_maxhash = 0;

    mdata_piece_session victim_sess(mdata_piece_mgr, victim_node_blkbase);
    if (victim_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
    if (!victim_sess) { res.rc = RC_ERR_IO; return res; }
    Bptree_ddentry_leaf_node_t *victim = (Bptree_ddentry_leaf_node_t *)victim_sess.data;
    if (!is_dentry_node(&victim->meta)) { res.rc = RC_ERR_STATE; return res; }
    if (RFS_PP_IS_INNER(victim->meta.parent_ptr)) { res.rc = RC_ERR_STATE; return res; }

    uint16_t count = (uint16_t)RFS_PP_COUNT(victim->meta.parent_ptr);
    if (count <= 126) { res.rc = RC_ERR_STATE; return res; }   /* 只有 count=127 才裂 */

    uint64_t new_blk = block_alloc_hard(2, 1);   /* 2 块连续，2^1 对齐 */
    if (new_blk == PTR_INVALID) { res.rc = RC_ERR_NO_BLOCK; return res; }

    /* 根叶子不参与同级链表（mkfs：ptrs[1]=".." inode，ptrs[2]=预留）。
     * 分裂后 victim 变为非根叶子，前驱/后继必须落成真实链表语义：
     * 无前驱/无后继 → 老实写 PTR_INVALID。 */
    const bool victim_is_root = RFS_PP_IS_ROOT(victim->meta.parent_ptr);
    /* 根叶 ptrs[1] = '..'（下面会改写为前驱）→ 分裂前先捕获，供建新根继承 */
    res.old_root_ptrs1 = victim_is_root ? victim->meta.ptrs[1].v : PTR_INVALID;

    /* ⚠ 旧后继必须在改写 victim.ptrs[2] **之前**取，否则读到的是 new_blk
     * ⚠ 位域读出的值是 52 位形态，与 PTR_INVALID 比较必须同样截断 */
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    const uint64_t old_next = RFS_PTR_ADDR(victim->meta.ptrs[2]);

    /* ── 初始化新叶子节点（新建：直接覆盖会话页） ── */
    mdata_piece_session new_sess(mdata_piece_mgr, new_blk);
    if (new_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
    if (!new_sess) { res.rc = RC_ERR_IO; return res; }
    Bptree_ddentry_leaf_node_t *new_leaf = (Bptree_ddentry_leaf_node_t *)new_sess.data;
    dentry_node_meta_init(new_sess.data, count - 63, false, false,
                          RFS_PP_PTR(victim->meta.parent_ptr));   /* 同父 */

    RFS_PTR_SET_ADDR(new_leaf->meta.ptrs[0], PTR_INVALID);
    RFS_PTR_SET_FREE(new_leaf->meta.ptrs[0], 0);
    RFS_PTR_SET_ADDR(new_leaf->meta.ptrs[1], victim_node_blkbase);   /* prev = victim */
    RFS_PTR_SET_FREE(new_leaf->meta.ptrs[1], 0);
    /* 根叶子无真后继 → INVALID；非根叶子继承 victim 的旧后继 */
    RFS_PTR_SET_ADDR(new_leaf->meta.ptrs[2], victim_is_root ? PTR_INVALID : old_next);
    RFS_PTR_SET_FREE(new_leaf->meta.ptrs[2], 0);

    /* ── 收缩 victim（左半 63 项） ── */
    RFS_PP_SET_COUNT(victim->meta.parent_ptr, 63);
    RFS_PTR_SET_ADDR(victim->meta.ptrs[2], new_blk);
    RFS_PTR_SET_FREE(victim->meta.ptrs[2], 0);
    /* 根叶子过渡为链表成员：ptrs[1] 原为 ".." inode，无前驱 → 老实写 INVALID */
    if (victim_is_root) {
        RFS_PTR_SET_ADDR(victim->meta.ptrs[1], PTR_INVALID);
        RFS_PTR_SET_FREE(victim->meta.ptrs[1], 0);
    }

    /* 拷贝右半：entries[63 .. count-1] → new_leaf->entries[0 .. (count-63-1)] */
    size_t copy_count = count - 63;   /* 64 */
    memcpy(new_leaf->entries, &victim->entries[63], copy_count * sizeof(ddentry_t));

    /* ── 更新真旧后继的前驱指针（根叶子场景无真后继，跳过） ── */
    if (!victim_is_root && old_next != PTR_INVALID_52) {
        mdata_piece_session next_sess(mdata_piece_mgr, old_next);
        if (next_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
        if (!next_sess) { res.rc = RC_ERR_IO; return res; }
        Bptree_ddentry_leaf_node_t *next_node = (Bptree_ddentry_leaf_node_t *)next_sess.data;
        RFS_PTR_SET_ADDR(next_node->meta.ptrs[1], new_blk);
        RFS_PTR_SET_FREE(next_node->meta.ptrs[1], 0);
        next_sess.dirty();
    }

    /* ── 写回 victim 和 new（析构按会话写回） ── */
    victim_sess.dirty();
    new_sess.dirty();

    res.rc = RC_OK;
    res.low_node_blkidx  = victim_node_blkbase;
    res.high_node_blkidx = new_blk;
    res.low_node_maxhash = victim->entries[62].hash;   /* 左半最大 hash */
    return res;
}

/* ═══════════════════════════════════════════════════════════════════
 * dentry_inner_split — 内部节点分裂（126 阶，左 63 右 64）
 *
 * 内部节点无兄弟链表，仅分裂数据与 meta。
 * 返回 low_node_maxhash = 左半 hints[62].hash_max。
 * 父层新增 hint 的 hash_max 即为此值。
 * ═══════════════════════════════════════════════════════════════════ */
RavenFS_juvenile::split_result_t RavenFS_juvenile::dentry_inner_split(uint64_t victim_node_blkbase)
{
    split_result_t res;
    res.rc = RC_OK;
    res.low_node_blkidx = PTR_INVALID;
    res.high_node_blkidx = PTR_INVALID;
    res.low_node_maxhash = 0;

    mdata_piece_session victim_sess(mdata_piece_mgr, victim_node_blkbase);
    if (victim_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
    if (!victim_sess) { res.rc = RC_ERR_IO; return res; }
    Bptree_ddentry_inner_node_t *victim = (Bptree_ddentry_inner_node_t *)victim_sess.data;
    if (!is_dentry_node(&victim->meta)) { res.rc = RC_ERR_STATE; return res; }
    if (!RFS_PP_IS_INNER(victim->meta.parent_ptr)) { res.rc = RC_ERR_STATE; return res; }
    /* 根内部节点 ptrs[1] = '..' → 捕获供建新根继承（本函数不改 ptrs，仍先捕） */
    res.old_root_ptrs1 = RFS_PP_IS_ROOT(victim->meta.parent_ptr) ? victim->meta.ptrs[1].v : PTR_INVALID;

    uint16_t count = (uint16_t)RFS_PP_COUNT(victim->meta.parent_ptr);
    if (count <= 126) { res.rc = RC_ERR_STATE; return res; }

    uint64_t new_blk = block_alloc_hard(2, 1);
    if (new_blk == PTR_INVALID) { res.rc = RC_ERR_NO_BLOCK; return res; }

    /* ── 初始化新内部节点（新建：直接覆盖会话页） ── */
    mdata_piece_session new_sess(mdata_piece_mgr, new_blk);
    if (new_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
    if (!new_sess) { res.rc = RC_ERR_IO; return res; }
    Bptree_ddentry_inner_node_t *new_inner = (Bptree_ddentry_inner_node_t *)new_sess.data;
    dentry_node_meta_init(new_sess.data, count - 63, true, false,
                          RFS_PP_PTR(victim->meta.parent_ptr));   /* 同父 */
    RFS_PTR_SET_ADDR(new_inner->meta.ptrs[0], PTR_INVALID);
    RFS_PTR_SET_ADDR(new_inner->meta.ptrs[1], PTR_INVALID);
    RFS_PTR_SET_ADDR(new_inner->meta.ptrs[2], PTR_INVALID);

    /* ── 收缩 victim（左半 63 hint） ── */
    RFS_PP_SET_COUNT(victim->meta.parent_ptr, 63);

    /* 拷贝右半 hints[63 .. count-1] → new_inner->hints[0 .. count-64] */
    size_t copy_count = count - 63;
    memcpy(new_inner->hints, &victim->hints[63], copy_count * sizeof(ddentry_hint_t));

    /* ⚠ 新内部节点接管右半孩子：这些孩子的 parent_ptr 必须改指 new_blk，
     * 否则后续上溯（rm 平衡递归 / 分裂传播找祖父）会走错节点。 */
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    for (size_t i = 0; i < copy_count; i++) {
        uint64_t child_blk = RFS_PTR_ADDR(new_inner->hints[i].sub_ptr);
        if (child_blk == PTR_INVALID_52) { res.rc = RC_ERR_STATE; return res; }
        mdata_piece_session c_sess(mdata_piece_mgr, child_blk);
        if (c_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
        if (!c_sess) { res.rc = RC_ERR_IO; return res; }
        Bptree_meta_t *cmeta = (Bptree_meta_t *)c_sess.data;
        if (!is_dentry_node(cmeta)) { res.rc = RC_ERR_STATE; return res; }
        if (RFS_PP_IS_ROOT(cmeta->parent_ptr)) { res.rc = RC_ERR_STATE; return res; }
        RFS_PP_SET_PTR(cmeta->parent_ptr, new_blk);
        c_sess.dirty();
    }

    victim_sess.dirty();
    new_sess.dirty();

    res.rc = RC_OK;
    res.low_node_blkidx  = victim_node_blkbase;
    res.high_node_blkidx = new_blk;
    res.low_node_maxhash = victim->hints[62].hash_max;   /* 左半最大 hash_max */
    return res;
}


/* ═══════════════════════════════════════════════════════════════════
 * dentry_spilt_parent_apply — 将叶子/内部节点分裂结果应用到父层
 *
 * 入参：
 *   parent_node_blkbase  : 父内部节点块号
 *   victim_in_node_id    : 父 hints 中指向被分裂子节点（左半）的索引
 *   result               : dentry_leaf_split / dentry_inner_split 的返回值
 *   is_parent_spilt_acquire : 出参，true 表示父节点也溢出了（需递归分裂）
 *
 * 行为：
 *   1. 读父节点，校验为内部节点
 *   2. 校验父 hints[victim_in_node_id] 确实指向 result.low_node_blkidx
 *   3. 更新 hints[victim_in_node_id].hash_max = result.low_node_maxhash
 *   4. 读右子节点（result.high_node_blkidx），取其最大 hash
 *   5. 在 victim_in_node_id + 1 处插入新 hint，指向右子节点，
 *      hash_max = 右子节点最大值
 *   6. 若父 entry_count 超过 126，置 is_parent_spilt_acquire = true
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dentry_spilt_parent_apply(
    uint64_t parent_node_blkbase,
    uint16_t victim_in_node_id,
    split_result_t result,
    bool &is_parent_spilt_acquire)
{
    if (result.rc != RC_OK) return result.rc;
    is_parent_spilt_acquire = false;

    /* ── 1. 读父节点 ── */
    mdata_piece_session parent_sess(mdata_piece_mgr, parent_node_blkbase);
    if (parent_sess.index_error()) return RC_ERR_STATE;
    if (!parent_sess) return RC_ERR_IO;
    Bptree_ddentry_inner_node_t *parent = (Bptree_ddentry_inner_node_t *)parent_sess.data;
    if (!is_dentry_node(&parent->meta)) return RC_ERR_STATE;
    if (!RFS_PP_IS_INNER(parent->meta.parent_ptr)) return RC_ERR_STATE;

    uint16_t count = (uint16_t)RFS_PP_COUNT(parent->meta.parent_ptr);
    if (victim_in_node_id >= count) return RC_ERR_STATE;
    if (RFS_PTR_ADDR(parent->hints[victim_in_node_id].sub_ptr) != result.low_node_blkidx)
        return RC_ERR_STATE;

    /* ── 2. 读右子节点，取最大 hash ── */
    mdata_piece_session child_sess(mdata_piece_mgr, result.high_node_blkidx);
    if (child_sess.index_error()) return RC_ERR_STATE;
    if (!child_sess) return RC_ERR_IO;
    Bptree_meta_t *child_meta = (Bptree_meta_t *)child_sess.data;
    if (!is_dentry_node(child_meta)) return RC_ERR_STATE;

    uint64_t high_max = 0;
    if (RFS_PP_IS_INNER(child_meta->parent_ptr)) {
        Bptree_ddentry_inner_node_t *child_inner = (Bptree_ddentry_inner_node_t *)child_sess.data;
        uint16_t child_count = (uint16_t)RFS_PP_COUNT(child_inner->meta.parent_ptr);
        if (child_count == 0) return RC_ERR_STATE;
        high_max = child_inner->hints[child_count - 1].hash_max;
    } else {
        Bptree_ddentry_leaf_node_t *child_leaf = (Bptree_ddentry_leaf_node_t *)child_sess.data;
        uint16_t child_count = (uint16_t)RFS_PP_COUNT(child_leaf->meta.parent_ptr);
        if (child_count == 0) return RC_ERR_STATE;
        high_max = child_leaf->entries[child_count - 1].hash;
    }

    /* ── 3. 更新左 hint（左半最大 hash 已变） ── */
    parent->hints[victim_in_node_id].hash_max = result.low_node_maxhash;

    /* ── 4. 插入右 hint ── */
    uint16_t insert_pos = victim_in_node_id + 1;
    if (insert_pos < count) {
        memmove(&parent->hints[insert_pos + 1], &parent->hints[insert_pos],
                (count - insert_pos) * sizeof(ddentry_hint_t));
    }
    RFS_PTR_SET_ADDR(parent->hints[insert_pos].sub_ptr, result.high_node_blkidx);
    RFS_PTR_SET_FREE(parent->hints[insert_pos].sub_ptr, 0);
    parent->hints[insert_pos].hash_max          = high_max;
    RFS_PP_SET_COUNT(parent->meta.parent_ptr, count + 1);

    /* ── 5. 判断父是否溢出 ── */
    if (count + 1 > 126) {
        is_parent_spilt_acquire = true;
    }

    parent_sess.dirty();
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * dentry_new_root_inner — 分裂后新建根内部节点
 *
 * 场景：原根为叶子（depth==1）或根内部节点溢出，分裂出两个子节点后
 * 需要一层新根把它们提升为 hints[0]/[1]。
 *
 * 流程：分配 2 块 → dentry_node_meta_init（根内部节点，parent_ptr=自身
 * inode 即 "."）→ 填 hints → 读两子节点改父指针（指向新根、去 root 标志）
 * → 写回 → 更新 root inode.data_root 与 root_dentry_blk。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dentry_new_root_inner(uint64_t low_blk, uint64_t low_max,
                                                      uint64_t high_blk, uint64_t root_inode_idx,
                                                      uint64_t root_dotdot_v)
{
    uint64_t new_root_blk = block_alloc_hard(2, 1);
    if (new_root_blk == PTR_INVALID) return RC_ERR_NO_BLOCK;

    /* 新建根内部节点：直接覆盖会话页（meta_init 清零 + 填 meta） */
    mdata_piece_session root_sess(mdata_piece_mgr, new_root_blk);
    if (root_sess.index_error()) return RC_ERR_STATE;
    if (!root_sess) return RC_ERR_IO;
    Bptree_ddentry_inner_node_t *new_root = (Bptree_ddentry_inner_node_t *)root_sess.data;
    dentry_node_meta_init(root_sess.data, 2, true, true, root_inode_idx);   /* "." */

    /* 新根继承旧根的 '..'（dir 根节点 ptrs[1] = 上级 inode）。
     * 2026-09-30 修：根分裂建新根必须携带 '..'，否则根目录一分裂就丢（v1.2
     * root=16 下暴露；旧 v1.1 root=0 时丢成 0 恰好==root，被掩盖）→ 审计
     * dotdot_mismatch。值由 split 前由调用方捕获（叶分裂会把 root 叶 ptrs[1]
     * 改写成前驱，故不能在分裂后读）。 */
    new_root->meta.ptrs[1].v = root_dotdot_v;

    RFS_PTR_SET_ADDR(new_root->hints[0].sub_ptr, low_blk);
    new_root->hints[0].hash_max   = low_max;

    RFS_PTR_SET_ADDR(new_root->hints[1].sub_ptr, high_blk);
    mdata_piece_session right_sess(mdata_piece_mgr, high_blk);
    if (right_sess.index_error()) return RC_ERR_STATE;
    if (!right_sess) return RC_ERR_IO;
    new_root->hints[1].hash_max = node_max_hash(right_sess.data);

    /* 更新左右子节点的父指针 */
    mdata_piece_session left_sess(mdata_piece_mgr, low_blk);
    if (left_sess.index_error()) return RC_ERR_STATE;
    if (!left_sess) return RC_ERR_IO;
    Bptree_meta_t *left_meta = (Bptree_meta_t *)left_sess.data;
    RFS_PP_SET_PTR(left_meta->parent_ptr, new_root_blk);
    RFS_PP_SET_IS_ROOT(left_meta->parent_ptr, 0);
    left_sess.dirty();

    Bptree_meta_t *right_meta = (Bptree_meta_t *)right_sess.data;
    RFS_PP_SET_PTR(right_meta->parent_ptr, new_root_blk);
    RFS_PP_SET_IS_ROOT(right_meta->parent_ptr, 0);
    right_sess.dirty();

    root_sess.dirty();

    {
        InodeSession rs(inode_io, root_inode_idx);
        if (!rs) return RC_ERR_IO;
        raven_dir_Inode_t *ri = (raven_dir_Inode_t *)rs.inode;
        ri->ddentry_Bptree_root = new_root_blk;
        rs.dirty();
    }
    root_dentry_blk = new_root_blk;
    return RC_OK;
}

#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>

/* ═══════════════════════════════════════════════════════════════════
 * dentry_tree.cpp — RavenFS v1.1 dentry B+tree 全套操作
 *
 * 本文件负责（Sparrow 分工）：
 *   node_max_hash / dentry_Bptree_walk / dentry_leaf_node_lookup /
 *   dentry_inner_node_lookup / dentry_leaf_entry_insert / delete /
 *   dentry_leaf_split / dentry_inner_split / dentry_node_balance /
 *   dentry_spilt_parent_apply
 *
 * 零缓存原则：所有块/inode 访问直接 pread/pwrite，无缓存层。
 * ═══════════════════════════════════════════════════════════════════ */

uint64_t node_max_hash(const void *node_buf) {
    const Bptree_meta_t *meta = (const Bptree_meta_t *)node_buf;
    uint16_t cnt = (uint16_t)meta->parent_ptr.entry_count;
    if (cnt == 0) return 0;
    if (meta->parent_ptr.is_inner_node) {
        const Bptree_dentry_inner_node_t *inner = (const Bptree_dentry_inner_node_t *)node_buf;
        return inner->hints[cnt - 1].hash_max;
    } else {
        const Bptree_dentry_leaf_node_t *leaf = (const Bptree_dentry_leaf_node_t *)node_buf;
        return leaf->entries[cnt - 1].hash;
    }
}
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
    meta->parent_ptr.entry_count   = entry_count;
    meta->parent_ptr.is_inner_node = is_inner ? 1 : 0;
    meta->parent_ptr.is_root_node  = is_root ? 1 : 0;
    meta->parent_ptr.ptr           = parent_ptr;
}
/* ═══════════════════════════════════════════════════════════════
 *   open → 读 Primary SB → magic/version/crc32 三连验 → 几何推导
 *   → 双位图全量入内存 + 自描述校验 → 读 root inode（取树根块号）
 * 任一失败：清理资源、fd=-1、*res_code = RC_ERR_*
 * ═══════════════════════════════════════════════════════════════ */

result_code_t RavenFS_juvenile::dentry_Bptree_walk(uint64_t root_blk_idx, dentry_bptree_path_t *trace, name_cmlpx_t *name)
{
    if (!trace || !name) return RC_ERR_STATE;
    trace->depth = 0;
    trace->is_finally_hit = false;

    uint64_t cur_blk = root_blk_idx;

    for (int depth = 0; depth < 64; depth++) {   /* 深度上限=blocks[64]；超限即树有环 */
        /* 每层独立会话：缓冲随层释放；sess.data 直用，零复制 */
        mdata_piece_session sess(mdata_piece_mgr, cur_blk);
        if (sess.index_error()) return RC_ERR_STATE;
        if (!sess) return RC_ERR_IO;

        Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
        if (!is_dentry_node(meta)) return RC_ERR_STATE;
        /* 根校验：第 0 层必须是根，后续层必须非根（树损坏即报） */
        if ((depth == 0) != (bool)meta->parent_ptr.is_root_node)
            return RC_ERR_STATE;

        trace->blocks[depth] = cur_blk;

        if (!meta->parent_ptr.is_inner_node) {
            /* 叶子：复合序查找收尾 */
            auto *leaf = (Bptree_dentry_leaf_node_t *)sess.data;
            leaf_node_result_t res = dentry_leaf_node_lookup(leaf, name);
            trace->depth = depth + 1;
            trace->is_finally_hit = res.is_idx_hitted;
            trace->child_idx[depth] = res.greates_lower_bound_idx;
            return RC_OK;
        }

        /* 内部节点：上界路由（找第一个 hash_max ≥ target） */
        auto *node = (Bptree_dentry_inner_node_t *)sess.data;
        if (node->meta.parent_ptr.entry_count == 0) return RC_ERR_STATE;
        uint16_t hint_idx = dentry_inner_node_lookup(node, name->hash);
        if (hint_idx == 0xFFFF) {
            /* 击穿：target > 全部子树 hash_max。目标明确——新条目是
             * 全局最大，归宿 = 最右叶子末尾。特化：不再二分，每层
             * 直取最后一个 hint 下钻到最右叶子。 */
            hint_idx = (uint16_t)(node->meta.parent_ptr.entry_count - 1);
        }
        trace->child_idx[depth] = (uint8_t)hint_idx;
        cur_blk = node->hints[hint_idx].sub_ptr.ptr;
    }
    return RC_ERR_STATE;   /* 深度超限：树成环 / 指针损坏 */
}
/* ═══════════════════════════════════════════════════════════════
 * dentry_leaf_node_lookup — 叶子内复合序查找
 *
 * 输入：叶子节点 + 查询名（name_cmlpx_t：name + 预计算 hash）
 * 输出：leaf_node_result_t
 *   greates_lower_bound_idx：复合序 (hash, name) 下 ≤ target 的最大条目索引
 *     （即最大下界 GLB）；**DENTRY_LEAF_GLB_NONE(0xFFFF) = 无下界**
 *   is_idx_hitted：是否精准命中（hash 与名字均相等）
 *
 * 调用方插入位置契约：
 *   - glb != DENTRY_LEAF_GLB_NONE：有下界，insert_at = glb + 1（插 GLB 之后）
 *   - glb == DENTRY_LEAF_GLB_NONE：无下界（本叶子所有条目 > target），
 *     insert_at = 0（最左插入）。空叶子（count=0）同样返回此哨兵。
 *
 * 无下界为何不需要向左兄弟插入：上界路由取第一个满足
 * target ≤ hash_max[i] 的 hint（§6.2），walk 落到本叶子已保证
 * target > 前驱叶子最大条目（复合序），故 target 归宿 = 本叶子开头。
 * 若 walk 落点错误（无下界且 target 本应属于前驱），那是路由 bug，
 * 不是本函数职责。
 *
 * 算法：一次二分（entries 按 (hash, name) 复合序严格递增，§10 不变量 #1）：
 *   - hash 小则小；hash 相等降级为内联名字 strcmp 字典序（§6.2 名字可二分）
 *   - 因复合序严格递增，若存在完全相等的条目，它必是最后一个 ≤ target 的
 *     条目，即 GLB 本身——命中检测只需检查 GLB
 *
 * hash 冲突处理（§6.3）：同 hash 条目在叶内连续，二分在碰撞区间内自动
 * 落入名字比较，无需先定位 hash 区间再扫名字。
 *
 * 名字比较前提：tmp_name_len_limiter 把关（strlen ≤ 39）→ 全内联
 * name[40] NUL 结尾，full_char_remain_ptr 恒 PTR_INVALID；名字堆未实现，
 * 无需读外置部分。接入名字堆后此处需改为按 name_len 分流比较。
 * ═══════════════════════════════════════════════════════════════ */
RavenFS_juvenile::leaf_node_result_t RavenFS_juvenile::dentry_leaf_node_lookup(Bptree_dentry_leaf_node_t *leaf, name_cmlpx_t *name)
{
    leaf_node_result_t res;
    res.greates_lower_bound_idx = 0;
    res.is_idx_hitted = false;

    const uint16_t count = (uint16_t)leaf->meta.parent_ptr.entry_count; /* ≤126 */
    const uint64_t target_hash = name->hash;

    uint16_t lo = 0, hi = count;
    uint16_t glb = DENTRY_LEAF_GLB_NONE;   /* 哨兵：无下界（最左插入） */
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        const Dentry_t &e = leaf->entries[mid];
        bool e_le_target;
        if (e.hash < target_hash)
            e_le_target = true;
        else if (e.hash > target_hash)
            e_le_target = false;
        else
            e_le_target = strcmp(e.name, name->name) <= 0;   /* 冲突降级字符串比较 */

        if (e_le_target) {
            glb = mid;      /* 候选下界，继续向右找更大的 */
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    res.greates_lower_bound_idx = glb;
    if (glb != DENTRY_LEAF_GLB_NONE) {
        const Dentry_t &e = leaf->entries[glb];
        if (e.hash == target_hash && strcmp(e.name, name->name) == 0)
            res.is_idx_hitted = true;
    }
    return res;
}
/* 内部节点上界路由：hints 按 hash_max 递增（§10 不变量 #2），
 * 找第一个 hash_max ≥ target 的 hint（最小上界，等价于 lower_bound）。
 * 全部 hash_max < target（击穿）→ 0xFFFF（与叶子 GLB 哨兵同值）。
 * 空节点（entry_count=0）→ 0xFFFF。 */
uint16_t RavenFS_juvenile::dentry_inner_node_lookup(Bptree_dentry_inner_node_t *node, uint64_t hash)
{
    const uint16_t count = (uint16_t)node->meta.parent_ptr.entry_count;
    uint16_t lo = 0, hi = count;
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        if (node->hints[mid].hash_max < hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    /* lo = 第一个 hash_max ≥ hash 的位置；lo == count → 全部 < hash（击穿） */
    return lo < count ? lo : 0xFFFF;
}
/* ═══════════════════════════════════════════════════════════════════
 * dentry_leaf_entry_insert — 叶子条目插入（瞬时槽允许 count=127）
 *
 * 前置条件：idx 由 dentry_Bptree_walk 的 GLB 语义推导：
 *   glb == DENTRY_LEAF_GLB_NONE → idx=0
 *   否则 idx=glb+1
 * 此函数只做 memmove 插入，不关心 inode_ptr 的具体值（调用者须填充）。
 *
 * result_flags：
 *   bit0 = 1 → 需分裂（count 入后 >126）
 *   bit1 = 1 → 叶子最大 hash 已变，父层 hint 需刷新
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dentry_leaf_entry_insert(
    uint64_t leaf_blk_base_idx,
    name_cmlpx_t *name,
    uint16_t idx,
    uint64_t &result_flags)
{
    result_flags = 0;
    if (!name || !tmp_name_len_limiter(name->name))
        return RC_ERR_NAME_LEN;   /* 名字堆未实现 */

    /* 会话贯穿：读-改-写同一会话页，无栈缓冲无复制；dirty 后析构写回 */
    mdata_piece_session sess(mdata_piece_mgr, leaf_blk_base_idx);
    if (sess.index_error()) return RC_ERR_STATE;
    if (!sess) return RC_ERR_IO;
    Bptree_dentry_leaf_node_t *leaf = (Bptree_dentry_leaf_node_t *)sess.data;
    if (!is_dentry_node(&leaf->meta)) return RC_ERR_STATE;
    if (leaf->meta.parent_ptr.is_inner_node) return RC_ERR_STATE;

    uint16_t count = (uint16_t)leaf->meta.parent_ptr.entry_count;
    if (idx > count) return RC_ERR_STATE;

    uint64_t old_max_hash = (count > 0) ? leaf->entries[count - 1].hash : 0;

    /* 挪出插入位置 */
    if (idx < count) {
        memmove(&leaf->entries[idx + 1], &leaf->entries[idx],
                (count - idx) * sizeof(Dentry_t));
    }

    /* 填充新条目（inode_ptr.ptr 留 PTR_INVALID，由调用者赋真实 inum） */
    Dentry_t *entry = &leaf->entries[idx];
    entry->hash = name->hash;
    entry->inode_ptr.free_to_use = (uint64_t)strlen(name->name);
    entry->inode_ptr.ptr = PTR_INVALID;
    memcpy(entry->name, name->name, DENTRY_INLINE_NAME_MAX);
    entry->full_char_remain_ptr = PTR_INVALID;

    count++;
    leaf->meta.parent_ptr.entry_count = count;

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
    return RC_OK;
}
/* ═══════════════════════════════════════════════════════════════════
 * dentry_leaf_entry_delete — 叶子条目物理删除
 *
 * idx 由 dentry_leaf_node_lookup 命中时返回的 greates_lower_bound_idx。
 * 物理删除 = memmove 左移 + 清空末槽。
 *
 * result_flags：
 *   bit0 = 1 → 欠载（count < 63 且非根），需合并/再平衡
 *   bit1 = 1 → 叶子最大 hash 已变，父层 hint 需刷新
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dentry_leaf_entry_delete(
    uint64_t leaf_blk_base_idx,
    uint16_t idx,
    uint64_t &result_flags)
{
    result_flags = 0;

    /* 会话贯穿：读-改-写同一会话页，无栈缓冲无复制；dirty 后析构写回 */
    mdata_piece_session sess(mdata_piece_mgr, leaf_blk_base_idx);
    if (sess.index_error()) return RC_ERR_STATE;
    if (!sess) return RC_ERR_IO;
    Bptree_dentry_leaf_node_t *leaf = (Bptree_dentry_leaf_node_t *)sess.data;
    if (!is_dentry_node(&leaf->meta)) return RC_ERR_STATE;
    if (leaf->meta.parent_ptr.is_inner_node) return RC_ERR_STATE;

    uint16_t count = (uint16_t)leaf->meta.parent_ptr.entry_count;
    if (idx >= count) return RC_ERR_STATE;

    bool is_last = (idx == count - 1);
    uint64_t old_max_hash = leaf->entries[count - 1].hash;

    /* 左移覆盖 */
    if (idx < count - 1) {
        memmove(&leaf->entries[idx], &leaf->entries[idx + 1],
                (count - idx - 1) * sizeof(Dentry_t));
    }
    memset(&leaf->entries[count - 1], 0, sizeof(Dentry_t));
    count--;
    leaf->meta.parent_ptr.entry_count = count;

    /* 欠载判定：非根且低于半满 63 */
    bool is_root = leaf->meta.parent_ptr.is_root_node;
    if (!is_root && count < 63)
        result_flags |= 1u;

    /* 父层 hash_max 刷新判定：删的是末项，或叶子已空 */
    if (is_last || count == 0)
        result_flags |= 2u;

    sess.dirty();
    return RC_OK;
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
    Bptree_dentry_leaf_node_t *victim = (Bptree_dentry_leaf_node_t *)victim_sess.data;
    if (!is_dentry_node(&victim->meta)) { res.rc = RC_ERR_STATE; return res; }
    if (victim->meta.parent_ptr.is_inner_node) { res.rc = RC_ERR_STATE; return res; }

    uint16_t count = (uint16_t)victim->meta.parent_ptr.entry_count;
    if (count <= 126) { res.rc = RC_ERR_STATE; return res; }   /* 只有 count=127 才裂 */

    uint64_t new_blk = block_alloc_hard(2, 1);   /* 2 块连续，2^1 对齐 */
    if (new_blk == PTR_INVALID) { res.rc = RC_ERR_NO_BLOCK; return res; }

    /* 根叶子不参与同级链表（mkfs：ptrs[1]=".." inode，ptrs[2]=预留）。
     * 分裂后 victim 变为非根叶子，前驱/后继必须落成真实链表语义：
     * 无前驱/无后继 → 老实写 PTR_INVALID。 */
    const bool victim_is_root = victim->meta.parent_ptr.is_root_node;

    /* ⚠ 旧后继必须在改写 victim.ptrs[2] **之前**取，否则读到的是 new_blk
     * ⚠ 位域读出的值是 52 位形态，与 PTR_INVALID 比较必须同样截断 */
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    const uint64_t old_next = victim->meta.ptrs[2].ptr;

    /* ── 初始化新叶子节点（新建：直接覆盖会话页） ── */
    mdata_piece_session new_sess(mdata_piece_mgr, new_blk);
    if (new_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
    if (!new_sess) { res.rc = RC_ERR_IO; return res; }
    Bptree_dentry_leaf_node_t *new_leaf = (Bptree_dentry_leaf_node_t *)new_sess.data;
    dentry_node_meta_init(new_sess.data, count - 63, false, false,
                          victim->meta.parent_ptr.ptr);   /* 同父 */

    new_leaf->meta.ptrs[0].ptr        = PTR_INVALID;
    new_leaf->meta.ptrs[0].free_to_use = 0;
    new_leaf->meta.ptrs[1].ptr        = victim_node_blkbase;   /* prev = victim */
    new_leaf->meta.ptrs[1].free_to_use = 0;
    /* 根叶子无真后继 → INVALID；非根叶子继承 victim 的旧后继 */
    new_leaf->meta.ptrs[2].ptr        = victim_is_root ? PTR_INVALID : old_next;
    new_leaf->meta.ptrs[2].free_to_use = 0;

    /* ── 收缩 victim（左半 63 项） ── */
    victim->meta.parent_ptr.entry_count = 63;
    victim->meta.ptrs[2].ptr        = new_blk;
    victim->meta.ptrs[2].free_to_use = 0;
    /* 根叶子过渡为链表成员：ptrs[1] 原为 ".." inode，无前驱 → 老实写 INVALID */
    if (victim_is_root) {
        victim->meta.ptrs[1].ptr        = PTR_INVALID;
        victim->meta.ptrs[1].free_to_use = 0;
    }

    /* 拷贝右半：entries[63 .. count-1] → new_leaf->entries[0 .. (count-63-1)] */
    size_t copy_count = count - 63;   /* 64 */
    memcpy(new_leaf->entries, &victim->entries[63], copy_count * sizeof(Dentry_t));

    /* ── 更新真旧后继的前驱指针（根叶子场景无真后继，跳过） ── */
    if (!victim_is_root && old_next != PTR_INVALID_52) {
        mdata_piece_session next_sess(mdata_piece_mgr, old_next);
        if (next_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
        if (!next_sess) { res.rc = RC_ERR_IO; return res; }
        Bptree_dentry_leaf_node_t *next_node = (Bptree_dentry_leaf_node_t *)next_sess.data;
        next_node->meta.ptrs[1].ptr        = new_blk;
        next_node->meta.ptrs[1].free_to_use = 0;
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
    Bptree_dentry_inner_node_t *victim = (Bptree_dentry_inner_node_t *)victim_sess.data;
    if (!is_dentry_node(&victim->meta)) { res.rc = RC_ERR_STATE; return res; }
    if (!victim->meta.parent_ptr.is_inner_node) { res.rc = RC_ERR_STATE; return res; }

    uint16_t count = (uint16_t)victim->meta.parent_ptr.entry_count;
    if (count <= 126) { res.rc = RC_ERR_STATE; return res; }

    uint64_t new_blk = block_alloc_hard(2, 1);
    if (new_blk == PTR_INVALID) { res.rc = RC_ERR_NO_BLOCK; return res; }

    /* ── 初始化新内部节点（新建：直接覆盖会话页） ── */
    mdata_piece_session new_sess(mdata_piece_mgr, new_blk);
    if (new_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
    if (!new_sess) { res.rc = RC_ERR_IO; return res; }
    Bptree_dentry_inner_node_t *new_inner = (Bptree_dentry_inner_node_t *)new_sess.data;
    dentry_node_meta_init(new_sess.data, count - 63, true, false,
                          victim->meta.parent_ptr.ptr);   /* 同父 */
    new_inner->meta.ptrs[0].ptr = PTR_INVALID;
    new_inner->meta.ptrs[1].ptr = PTR_INVALID;
    new_inner->meta.ptrs[2].ptr = PTR_INVALID;

    /* ── 收缩 victim（左半 63 hint） ── */
    victim->meta.parent_ptr.entry_count = 63;

    /* 拷贝右半 hints[63 .. count-1] → new_inner->hints[0 .. count-64] */
    size_t copy_count = count - 63;
    memcpy(new_inner->hints, &victim->hints[63], copy_count * sizeof(Dentry_hint_t));

    /* ⚠ 新内部节点接管右半孩子：这些孩子的 parent_ptr 必须改指 new_blk，
     * 否则后续上溯（rm 平衡递归 / 分裂传播找祖父）会走错节点。 */
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    for (size_t i = 0; i < copy_count; i++) {
        uint64_t child_blk = new_inner->hints[i].sub_ptr.ptr;
        if (child_blk == PTR_INVALID_52) { res.rc = RC_ERR_STATE; return res; }
        mdata_piece_session c_sess(mdata_piece_mgr, child_blk);
        if (c_sess.index_error()) { res.rc = RC_ERR_STATE; return res; }
        if (!c_sess) { res.rc = RC_ERR_IO; return res; }
        Bptree_meta_t *cmeta = (Bptree_meta_t *)c_sess.data;
        if (!is_dentry_node(cmeta)) { res.rc = RC_ERR_STATE; return res; }
        if (cmeta->parent_ptr.is_root_node) { res.rc = RC_ERR_STATE; return res; }
        cmeta->parent_ptr.ptr = new_blk;
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
    Bptree_dentry_inner_node_t *parent = (Bptree_dentry_inner_node_t *)parent_sess.data;
    if (!is_dentry_node(&parent->meta)) return RC_ERR_STATE;
    if (!parent->meta.parent_ptr.is_inner_node) return RC_ERR_STATE;

    uint16_t count = (uint16_t)parent->meta.parent_ptr.entry_count;
    if (victim_in_node_id >= count) return RC_ERR_STATE;
    if (parent->hints[victim_in_node_id].sub_ptr.ptr != result.low_node_blkidx)
        return RC_ERR_STATE;

    /* ── 2. 读右子节点，取最大 hash ── */
    mdata_piece_session child_sess(mdata_piece_mgr, result.high_node_blkidx);
    if (child_sess.index_error()) return RC_ERR_STATE;
    if (!child_sess) return RC_ERR_IO;
    Bptree_meta_t *child_meta = (Bptree_meta_t *)child_sess.data;
    if (!is_dentry_node(child_meta)) return RC_ERR_STATE;

    uint64_t high_max = 0;
    if (child_meta->parent_ptr.is_inner_node) {
        Bptree_dentry_inner_node_t *child_inner = (Bptree_dentry_inner_node_t *)child_sess.data;
        uint16_t child_count = (uint16_t)child_inner->meta.parent_ptr.entry_count;
        if (child_count == 0) return RC_ERR_STATE;
        high_max = child_inner->hints[child_count - 1].hash_max;
    } else {
        Bptree_dentry_leaf_node_t *child_leaf = (Bptree_dentry_leaf_node_t *)child_sess.data;
        uint16_t child_count = (uint16_t)child_leaf->meta.parent_ptr.entry_count;
        if (child_count == 0) return RC_ERR_STATE;
        high_max = child_leaf->entries[child_count - 1].hash;
    }

    /* ── 3. 更新左 hint（左半最大 hash 已变） ── */
    parent->hints[victim_in_node_id].hash_max = result.low_node_maxhash;

    /* ── 4. 插入右 hint ── */
    uint16_t insert_pos = victim_in_node_id + 1;
    if (insert_pos < count) {
        memmove(&parent->hints[insert_pos + 1], &parent->hints[insert_pos],
                (count - insert_pos) * sizeof(Dentry_hint_t));
    }
    parent->hints[insert_pos].sub_ptr.ptr        = result.high_node_blkidx;
    parent->hints[insert_pos].sub_ptr.free_to_use = 0;
    parent->hints[insert_pos].hash_max          = high_max;
    parent->meta.parent_ptr.entry_count         = count + 1;

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
                                                      uint64_t high_blk, uint64_t root_inode_idx)
{
    uint64_t new_root_blk = block_alloc_hard(2, 1);
    if (new_root_blk == PTR_INVALID) return RC_ERR_NO_BLOCK;

    /* 新建根内部节点：直接覆盖会话页（meta_init 清零 + 填 meta） */
    mdata_piece_session root_sess(mdata_piece_mgr, new_root_blk);
    if (root_sess.index_error()) return RC_ERR_STATE;
    if (!root_sess) return RC_ERR_IO;
    Bptree_dentry_inner_node_t *new_root = (Bptree_dentry_inner_node_t *)root_sess.data;
    dentry_node_meta_init(root_sess.data, 2, true, true, root_inode_idx);   /* "." */

    new_root->hints[0].sub_ptr.ptr = low_blk;
    new_root->hints[0].hash_max   = low_max;

    new_root->hints[1].sub_ptr.ptr = high_blk;
    mdata_piece_session right_sess(mdata_piece_mgr, high_blk);
    if (right_sess.index_error()) return RC_ERR_STATE;
    if (!right_sess) return RC_ERR_IO;
    new_root->hints[1].hash_max = node_max_hash(right_sess.data);

    /* 更新左右子节点的父指针 */
    mdata_piece_session left_sess(mdata_piece_mgr, low_blk);
    if (left_sess.index_error()) return RC_ERR_STATE;
    if (!left_sess) return RC_ERR_IO;
    Bptree_meta_t *left_meta = (Bptree_meta_t *)left_sess.data;
    left_meta->parent_ptr.ptr = new_root_blk;
    left_meta->parent_ptr.is_root_node = 0;
    left_sess.dirty();

    Bptree_meta_t *right_meta = (Bptree_meta_t *)right_sess.data;
    right_meta->parent_ptr.ptr = new_root_blk;
    right_meta->parent_ptr.is_root_node = 0;
    right_sess.dirty();

    root_sess.dirty();

    raven_Inode_t root_inode;
    inode_read(root_inode_idx, &root_inode);
    root_inode.data_root = new_root_blk;
    inode_write(root_inode_idx, &root_inode);
    root_dentry_blk = new_root_blk;
    return RC_OK;
}

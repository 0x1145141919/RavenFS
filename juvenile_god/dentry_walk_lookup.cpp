#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>

/* ═══════════════════════════════════════════════════════════════════
 * dentry_walk_lookup.cpp — dentry B+tree 只读导航（原 dentry_tree.cpp 拆出，
 * 2026-09-07，Sparrow 手写代码按域分置）
 *
 * 本文件负责：
 *   node_max_hash —— 全局工具（root_dir.cpp 亦引用，hpp 542 声明）
 *   dentry_Bptree_walk / dentry_name_cmp / dentry_leaf_node_lookup /
 *   dentry_inner_node_lookup
 *
 * 与 extent 树的键语义差异（126 阶 dentry vs 337 阶 extent）：
 *   - 叶子序 = 复合序：hash 主序 + 冲突降级完整名字比较（name_cmp，
 *     >40B 自动读名字堆外置部分）——无"区间覆盖"概念，命中 = hash+name 全等；
 *   - 内部路由 = hash_max ≥ target（最小上界，lower_bound 语义）；
 *     extent 是 >（半开区间 key），不可互抄；
 *   - child_idx 为 uint8_t 直赋：126 阶 hint_idx 恒 ≤125，无截断问题
 *     （extent 337 阶必须 uint16_t，两树各自的特化点）。
 *
 * 零缓存原则：所有块/inode 访问直接 pread/pwrite，无缓存层。
 * ═══════════════════════════════════════════════════════════════════ */

uint64_t node_max_hash(const void *node_buf) {
    const Bptree_meta_t *meta = (const Bptree_meta_t *)node_buf;
    uint16_t cnt = (uint16_t)RFS_PP_COUNT(meta->parent_ptr);
    if (cnt == 0) return 0;
    if (RFS_PP_IS_INNER(meta->parent_ptr)) {
        const Bptree_ddentry_inner_node_t *inner = (const Bptree_ddentry_inner_node_t *)node_buf;
        return inner->hints[cnt - 1].hash_max;
    } else {
        const Bptree_ddentry_leaf_node_t *leaf = (const Bptree_ddentry_leaf_node_t *)node_buf;
        return leaf->entries[cnt - 1].hash;
    }
}

/* ═══════════════════════════════════════════════════════════════
 *   open → 读 Primary SB → magic/version/crc32 三连验 → 几何推导
 *   → 双位图全量入内存 + 自描述校验 → 读 root inode（取树根块号）
 * 任一失败：清理资源、fd=-1、*res_code = RC_ERR_*
 * ═══════════════════════════════════════════════════════════════ */
//等等，这个函数怎么处理最左边的边界条件的
result_code_t RavenFS_juvenile::dentry_Bptree_walk(uint64_t root_blk_idx, bptree_path_t *trace, name_cmlpx_t *name)
{
    if (!trace || !name) return RC_ERR_STATE;
    trace->depth = 0;
    trace->is_leaf_hit = false;

    uint64_t cur_blk = root_blk_idx;

    for (int depth = 0; depth < 64; depth++) {   /* 深度上限=blocks[64]；超限即树有环 */
        /* 每层独立会话：缓冲随层释放；sess.data 直用，零复制 */
        mdata_piece_session sess(mdata_piece_mgr, cur_blk);
        if (sess.index_error()) return RC_ERR_STATE;
        if (!sess) return RC_ERR_IO;

        Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
        if (!is_dentry_node(meta)) return RC_ERR_STATE;
        /* 根校验：第 0 层必须是根，后续层必须非根（树损坏即报） */
        if ((depth == 0) != (bool)RFS_PP_IS_ROOT(meta->parent_ptr))
            return RC_ERR_STATE;

        trace->blocks[depth] = cur_blk;

        if (!RFS_PP_IS_INNER(meta->parent_ptr)) {
            /* 叶子：复合序查找收尾 */
            auto *leaf = (Bptree_ddentry_leaf_node_t *)sess.data;
            leaf_node_result_t res = dentry_leaf_node_lookup(leaf, name);
            trace->depth = depth + 1;
            trace->is_leaf_hit = res.is_idx_hitted;
            trace->child_idx[depth] = res.greates_lower_bound_idx;
            return RC_OK;
        }

        /* 内部节点：上界路由（找第一个 hash_max ≥ target） */
        auto *node = (Bptree_ddentry_inner_node_t *)sess.data;
        if (RFS_PP_COUNT(node->meta.parent_ptr) == 0) return RC_ERR_STATE;
        uint16_t hint_idx = dentry_inner_node_lookup(node, name->hash);
        if (hint_idx == 0xFFFF) {
            /* 击穿：target > 全部子树 hash_max。目标明确——新条目是
             * 全局最大，归宿 = 最右叶子末尾。特化：不再二分，每层
             * 直取最后一个 hint 下钻到最右叶子。 */
            hint_idx = (uint16_t)(RFS_PP_COUNT(node->meta.parent_ptr) - 1);
        }
        trace->child_idx[depth] = (uint8_t)hint_idx;
        cur_blk = RFS_PTR_ADDR(node->hints[hint_idx].sub_ptr);
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
 * 名字比较：按 (hash, name) 复合序——hash 不等走 hash，相等降级
 * dentry_name_cmp 完整名字比较（>40B 自动读名字堆外置部分，§7）。
 * ═══════════════════════════════════════════════════════════════ */
/* ── dentry 完整名字比较 ──
 * 树内条目 e 的完整名字 = e.name[0..min(40,e_len)) +（e_len>40 时）堆外置尾部。
 * 与目标名 memcmp 公共前缀，长度裁决；e_len 取 inode_ptr.free_to_use。
 * 读堆失败保守返回 1（目标 < 条目，不会误判命中）。 */
int RavenFS_juvenile::dentry_name_cmp(const ddentry_t &e, const char *name, uint16_t name_len,
                                      uint64_t owner_dir_inum)
{
    const uint16_t e_len = (uint16_t)RFS_PTR_FREE(e.inode_ptr);
    char e_buf[RAVENFS_MAX_NAME_LEN];
    const uint16_t e_inline = (e_len < DENTRY_INLINE_NAME_MAX) ? e_len : DENTRY_INLINE_NAME_MAX;
    memcpy(e_buf, e.name, e_inline);
    if (e_len > DENTRY_INLINE_NAME_MAX) {
        if (name_data_read(owner_dir_inum, e.full_char_remain_offset,
                           (uint16_t)(e_len - DENTRY_INLINE_NAME_MAX),
                           e_buf + DENTRY_INLINE_NAME_MAX) != RC_OK)
            return 1;   /* 保守：视为大于目标 */
    }
    const uint16_t cmp_len = (e_len < name_len) ? e_len : name_len;
    int c = memcmp(e_buf, name, cmp_len);
    if (c != 0) return c;
    return (e_len < name_len) ? -1 : (e_len > name_len) ? 1 : 0;
}

RavenFS_juvenile::leaf_node_result_t RavenFS_juvenile::dentry_leaf_node_lookup(Bptree_ddentry_leaf_node_t *leaf, name_cmlpx_t *name)
{
    leaf_node_result_t res;
    res.greates_lower_bound_idx = 0;
    res.is_idx_hitted = false;

    const uint16_t count = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr); /* ≤126 */
    const uint64_t target_hash = name->hash;

    uint16_t lo = 0, hi = count;
    uint16_t glb = DENTRY_LEAF_GLB_NONE;   /* 哨兵：无下界（最左插入） */
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        const ddentry_t &e = leaf->entries[mid];
        bool e_le_target;
        if (e.hash < target_hash)
            e_le_target = true;
        else if (e.hash > target_hash)
            e_le_target = false;
        else
            e_le_target = dentry_name_cmp(e, name->name, name->name_len, name->owner_dir_inum) <= 0;   /* 冲突降级完整名字比较（>40B 读数据面） */

        if (e_le_target) {
            glb = mid;      /* 候选下界，继续向右找更大的 */
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    res.greates_lower_bound_idx = glb;
    if (glb != DENTRY_LEAF_GLB_NONE) {
        const ddentry_t &e = leaf->entries[glb];
        if (e.hash == target_hash && dentry_name_cmp(e, name->name, name->name_len, name->owner_dir_inum) == 0)
            res.is_idx_hitted = true;
    }
    return res;
}
/* 内部节点上界路由：hints 按 hash_max 递增（§10 不变量 #2），
 * 找第一个 hash_max ≥ target 的 hint（最小上界，等价于 lower_bound）。
 * 全部 hash_max < target（击穿）→ 0xFFFF（与叶子 GLB 哨兵同值）。
 * 空节点（entry_count=0）→ 0xFFFF。 */
uint16_t RavenFS_juvenile::dentry_inner_node_lookup(Bptree_ddentry_inner_node_t *node, uint64_t hash)
{
    const uint16_t count = (uint16_t)RFS_PP_COUNT(node->meta.parent_ptr);
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

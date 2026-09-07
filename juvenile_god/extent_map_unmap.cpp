#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 元数据节点会话（头里仅前置声明） */
#include <cstring>
#include <vector>
#include <algorithm>

/* ═══════════════════════════════════════════════════════════════════
 * extent_map_unmap.cpp — 区段树 map/unmap 轮子（原 extent_tree.cpp 拆出）
 *
 * 本文件是写路径（direct_file_* 四步管线 phase2）与打洞/截断的树操作
 * 落点：extent_map_range 逐段声明式映射（叶内融合 + 预算池原子分裂 + 欠载平衡链），
 * extent_unmap_range_loose 子集删除（unmap 家族唯一接口，2026-09-07
 * extent_unmap_range 退役并入其 full 分支）。
 * ═══════════════════════════════════════════════════════════════════ */


/* ═══════════════════════════════════════════════════════════════════
 * extent 原语对实现（2026-09-03 定稿；2026-09-07 换代：unmap 家族收敛 loose /
 * map 侧叶内融合四形态）——
 *   extent_map_range   ：声明式映射（叶内融合四形态：纯插 / 左并 / 右并 /
 *     双并三合一 + 预算池原子分裂 + 欠载平衡链）
 *   extent_unmap_range_loose：子集删除（full/head/tail/mid 三分支，
 *     前 extent_unmap_range 的整条删除语义 = 其 full 分支）
 *
 * 责任外显契约（对比 dentry 内敛）：
 *   - 数据块 alloc/free 全归调用方（map 的 ext.block_base 须已分配、
 *     unmap 成功后调用方自行 block_free）——本对只动树，测试可假性分配。
 *   - inline→extent 跃迁（建树根）归 direct_file_* 层：本对要求文件
 *     data_root ≠ PTR_INVALID 且 is_data_root_direct_bit==0。
 *   - 树节点块（2 块一组）内部分配/释放（分裂/平衡/根更替）。
 *   - 根更替（分裂建新根 / 根收缩）回写文件 inode.data_root。
 *
 * 与 dentry 树的结构差异：
 *   - key = 子树最大开边界 (fblkbase+length)，非 hash_max；
 *   - 合并义务：fblk 紧贴 && phys 连续的两条记录必须合一（canonical 形态）
 *     —— phys 是外部声明的事实，故合并判定在本对内部做；
 *   - 文件树根无 ".."：ptrs[1] 恒 PTR_INVALID，parent_ptr.ptr = 文件自身
 *     inode（"." 审计锚）；
 *   - 阶 337：欠载 <168（非根）、合并上界 total≤337、分裂左 168 右 count−168。
 *
 * 会话纪律：no_cache 下同块双会话各自持独立拷贝、析构写回——绝不跨会话
 * 互读修改；分裂/平衡/刷新一律先关掉本块旧会话再开新会话。
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * extent_map_range — 声明式映射原语（插侧 · 2026-09-07 叶内融合四形态完备）
 *
 * 演进史：
 *   - 2026-09-03 内建自动融合（左并/右并/跨界）→ F 组测试实证不完备
 *     （f1d 跨界左并盲区 / F2 融合致欠载叶不回收）→ 7bde35c 降级为纯插；
 *   - 2026-09-06 定案（演进史见 Docs/pwrite_pipeline.md §1/§2）：map 只左融合（同叶
 *     GLB 单对）+ 全量融合归激进重建（extent_rebuild_tree）；
 *   - 2026-09-07（晚）叶内四形态完备换代：左融合推广为同叶 L/R 双判定（纯插 /
 *     左融合 / 右融合 / 双融合三合一），叶内 canonical 合并义务全覆盖；跨界
 *     （entry[0] 前跨前叶 / 叶末越叶）仍显式放弃，f1d 残留待遇不变（交激进
 *     重建兑底）。四形态侦查在树改动前一次完成。
 *
 * 形态语义（同叶内；紧贴 = 半开区间端相接，融合 = 紧贴 && phys 连续）：
 *   L = GLB 紧贴 && GLB.phys_end == ext.block_base.ptr；
 *   R = ext.end == 右邻.fblkbase && ext.phys_end == 右邻.block_base.ptr；
 *   - 纯插（!L && !R）：GLB 后腾槽插入 → count+1。唯一可能超满的形态：满叶
 *     （337）需预算 depth+1 先分配（失败 = free + 返回，树零改动真原子，同
 *     unmap mid）。收尾 refresh 链（叶末插入 → 叶 max 变 → 上溯，内部短路）。
 *   - 左融合（L && !R）：GLB.length += 段长，count 不变零腾槽 → 无超满无欠载
 *     零分配。GLB 为叶末项 → 叶 max 变 → 收尾刷父链；否则零刷新。
 *   - 右融合（!L && R）：段并入右邻头（fblkbase/block_base 同步左移 cut，phys
 *     随逻辑线性平移，连续映射保持）。终点不变 → 叶 max 恒不变 → 父链零刷新。
 *     count 不变 → 无超满无欠载零分配。修旧"前插纯插致与右邻紧贴连续两条并
 *     存"的叶内残留（canonical 义务叶内完备化）。
 *   - 双融合（L && R）：三合一 —— GLB 扩长覆盖 [GLB.base, 右邻.end) + memmove
 *     删右邻 → count−1。唯一删记录的形态 → 非根 168→167 欠载 → 收尾走
 *     extent_balance_from（同 unmap full；detach：根收缩经 root_blk 带出）。
 *     叶 max 恒不变（右邻若为末项，新末项 = 扩长 GLB，终点 = 原末项终点）→
 *     无刷新义务。count−1 永不超满 → 零分配。
 *
 * 语义：声明 [ext.fblkbase, ext.fblkbase+ext.length) 由 ext.block_base 起
 * length 个连续 FS_block 背书（纯记录层，不碰数据侧位图）。重叠防御双向：
 *   - 落点 GLB 覆盖 ext.fblkbase（起点落入既有区间）→ RC_ERR_OVERLAP；
 *   - 区间终点盖右邻起点（ext.end > 右邻.fblkbase）→ RC_ERR_OVERLAP
 *     （四形态共用：防御先于融合判定，真重叠与融合判定（==）互斥）。
 * 紧贴合法（半开区间无交集）；紧贴+连续 = 融合义务，其余残留合法。
 *
 * 失败面汇总（对比 09-06 版新增两条结构路径）：纯插满叶分裂级联（预算池，
 * 无 ENOSPC）；双融合欠载平衡链（无分配无 ENOSPC，仅 IO/STATE）。左/右融合
 * 零失败面。所有树改动在单叶会话内一次 dirty；分裂/平衡/刷链前必须已关
 * 会话（同块双会话不互见修改）。
 *
 * 外显契约（detach，2026-09-06）：root_blk in/out（根裂建新根 / 根收缩经引用
 * 带出）；owner_inode_idx 纯审计锚（根 meta "."）；不碰 inode。调用方保证
 * root_blk 指向合法 extent 树根（inline→extent 跃迁归 direct_file_* 层）。
 */
result_code_t RavenFS_juvenile::extent_map_range(uint64_t &root_blk, uint64_t owner_inode_idx,
                                                 const extent_t &ext)
{
    if (ext.length == 0) return RC_OK;   /* 空映射：无操作 */

    bptree_path_t trace;
    result_code_t rc = extent_Bptree_walk(root_blk, &trace, ext.fblkbase);
    if (rc) return rc;
    const uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    const uint16_t glb = trace.child_idx[trace.depth - 1];
    const bool hit = trace.is_leaf_hit;

    /* 四形态（2026-09-07）：none=纯插 / left=扩 GLB 尾 / right=并入右邻头 /
     * both=GLB 扩长吞右邻（count−1）。跨界一律不做（L/R 均为叶内判定）。 */
    enum class merge_t { none, left, right, both } merge;
    bool underflow = false;         /* both：删右邻后非根欠载（count<168） */
    bool is_root_leaf = false;      /* both：根叶无欠载概念且无父 key */
    bool left_key_changed = false;  /* left：GLB 为叶末项 → 叶 max 变 */
    bool need_split = false;        /* none：纯插后超满（count>337） */
    std::vector<uint64_t> reserve_pool;   /* 分裂预算池（仅 none + 满叶需要） */
    size_t pool_used = 0;

    {
        mdata_piece_session sess(mdata_piece_mgr, leaf_blk);
        if (sess.index_error()) return RC_ERR_STATE;
        if (!sess) return RC_ERR_IO;
        Bptree_extent_leaf_node_t *leaf = (Bptree_extent_leaf_node_t *)sess.data;
        if (!is_extent_node(&leaf->meta) || leaf->meta.parent_ptr.is_inner_node)
            return RC_ERR_STATE;
        uint16_t count = (uint16_t)leaf->meta.parent_ptr.entry_count;

        if (hit) return RC_ERR_OVERLAP;   /* 起点被 GLB 覆盖（区间重叠） */

        /* 右邻重叠防御（四形态共用：扩长/并入/新插后终点都不得盖右邻起点） */
        const uint16_t pos = (glb == EXTENT_LEAF_GLB_NONE) ? 0 : (uint16_t)(glb + 1);
        if (pos < count) {
            const extent_t r = leaf->extents[pos];
            if (ext.fblkbase + ext.length > r.fblkbase)
                return RC_ERR_OVERLAP;
        }

        /* ── 形态判定（树改动前一次完成；packed 成员禁引用 → 局部拷贝）── */
        bool L = false, R = false;
        if (glb != EXTENT_LEAF_GLB_NONE) {
            const extent_t g = leaf->extents[glb];
            L = (g.fblkbase + g.length == ext.fblkbase &&
                 g.block_base.ptr + g.length == ext.block_base.ptr);
        }
        if (pos < count) {
            const extent_t r = leaf->extents[pos];
            R = (ext.fblkbase + ext.length == r.fblkbase &&
                 ext.block_base.ptr + ext.length == r.block_base.ptr);
        }
        merge = L ? (R ? merge_t::both : merge_t::left)
                  : (R ? merge_t::right : merge_t::none);

        if (merge == merge_t::left) {
            /* 左融合（2026-09-06 语义保留）：GLB 扩尾，count/槽位不动 */
            leaf->extents[glb].length += ext.length;
            left_key_changed = (glb == count - 1);    /* 叶末项 → 叶 max 变 */
            sess.dirty();
        } else if (merge == merge_t::right) {
            /* 右融合：段并入右邻头 —— 起点/phys 同步左移 cut（R 条件保证
             * r.fblkbase == ext.fblkbase + cut → 无下溢）。终点不动 → 叶 max
             * 不变 → 收尾零刷新。count 不变 → 零分配零失败面。 */
            const uint64_t cut = ext.length;
            extent_t nr = leaf->extents[pos];
            nr.fblkbase -= cut;
            nr.block_base.ptr -= cut;
            nr.length += cut;
            leaf->extents[pos] = nr;
            sess.dirty();
        } else if (merge == merge_t::both) {
            /* 双融合（三合一）：GLB 扩长覆盖到右邻终点（长度用 右邻.end − GLB
             * 起点 的防溢出写法）+ memmove 删右邻 → count−1。L&&R 已证 phys
             * 三连连续 → 合并单条连续映射合法。删记录 → 非根 168→167 欠载
             * → 收尾走平衡链；叶 max 恒不变（论证见注释）→ 零刷新。 */
            const extent_t g = leaf->extents[glb];
            const extent_t r = leaf->extents[pos];
            extent_t ng = g;
            ng.length = (r.fblkbase + r.length) - g.fblkbase;
            leaf->extents[glb] = ng;
            if (pos < count - 1) {
                memmove(&leaf->extents[pos], &leaf->extents[pos + 1],
                        (size_t)(count - pos - 1) * sizeof(extent_t));
            }
            memset(&leaf->extents[count - 1], 0, sizeof(extent_t));
            count--;
            leaf->meta.parent_ptr.entry_count = count;
            is_root_leaf = leaf->meta.parent_ptr.is_root_node;
            underflow = (!is_root_leaf && count < 168);
            sess.dirty();
        } else {
            /* ── 纯插（无融合命中）：GLB 后腾槽插入 ── */
            if (count == 337) {
                /* 分裂预算（2026-09-07 map 原子性）：满叶纯插必裂 → 按树高预分配
                 * 整条分裂链节点（depth+1 = 叶裂 1 + 每内层至多 1 + 根裂建新根 1）。
                 * alloc 在 memmove 之前 → 失败 = free 已分配 + 返回，树零改动、
                 * 真原子失败。不满叶插入零预算；融合分支恒零预算（count 不增 /
                 * 反减 → 永不超满，无 ENOSPC 面）。 ── */
                reserve_pool.reserve((size_t)trace.depth + 1);
                for (int i = 0; i <= trace.depth; i++) {
                    const uint64_t b = block_alloc_hard(2, 1);
                    if (b == PTR_INVALID) {
                        for (uint64_t x : reserve_pool) block_free(x, 2);
                        return RC_ERR_NO_BLOCK;   /* 未 dirty：会话析构不写回，树零改动 */
                    }
                    reserve_pool.push_back(b);
                }
            }
            if (pos < count) {
                memmove(&leaf->extents[pos + 1], &leaf->extents[pos],
                        (size_t)(count - pos) * sizeof(extent_t));
            }
            leaf->extents[pos] = ext;
            count++;
            leaf->meta.parent_ptr.entry_count = count;
            need_split = count > 337;
            sess.dirty();
        }
    }   /* 叶会话析构写回；分裂/平衡/刷链前必须关（同块双会话不互见修改） */

    if (merge == merge_t::left) {
        /* 左融合收尾：GLB 非叶末 → 叶 max 未变 → 父链零刷新；仅叶末项上溯。 */
        if (left_key_changed) {
            rc = extent_refresh_key_chain(leaf_blk);
            if (rc) return rc;
        }
        return RC_OK;
    }
    if (merge == merge_t::right)
        return RC_OK;   /* 终点不变 → 叶 max 不变 → 父链零刷新 */

    if (merge == merge_t::both) {
        /* 双融合收尾：仅"删记录致欠载"一条结构路径 → 平衡链（detach：根收缩
         * 经 root_blk 带出，inode.data_root 提交归调用方）；无欠载 → 零收尾
         * （叶 max 不变论证）。根叶恒合法（count≥1）。 */
        if (is_root_leaf) return RC_OK;
        if (underflow)
            return extent_balance_from(leaf_blk, owner_inode_idx, root_blk);
        return RC_OK;
    }

    /* ── none（纯插）收尾：分裂级联 + 父 key 刷新 ── */
    if (need_split)
        rc = extent_split_cascade(leaf_blk, trace, reserve_pool, pool_used,
                                  owner_inode_idx, root_blk);
    else
        rc = extent_refresh_key_chain(leaf_blk);   /* 纯插不裂：叶末插入 max 变时上溯（内部短路） */

    /* 预算归还（统一收尾）：级联实际消耗 pool_used 个（多数情况只裂一层 → 大部
     * 分预算归还；最坏全链分裂 depth+1 个全部消耗）。错误路径同样在此归还。 */
    for (; pool_used < reserve_pool.size(); pool_used++)
        block_free(reserve_pool[pool_used], 2);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_split_cascade — 分裂级联 + 父 key 刷新（map/loose 共用内核）
 *
 * 2026-09-07 从 extent_map_range 闭包抽取（原 split_and_refresh lambda）：
 * 语义与旧闭包逐行等价，仅作用域从 lambda 捕获改为参数传递。
 *
 * 前置：调用方已在本叶会话内完成插入/腾槽并 dirty（count>337 超满落盘）；
 * 本函数按树高预算池（depth+1 个节点，调用方预分配）自叶向上打完整条分裂链：
 *   叶裂（预算 1）→ 非根则逐级内层裂（每层至多 1）→ 根裂建新根（预算 1）
 * 链尾：从本叶（左半）与分裂高叶（右半）各自沿父链上溯刷新 max key。
 *
 * 失败面：预算池由调用方保证充足（depth+1 恰是满链消耗上界），本函数不再
 * alloc → 无 ENOSPC；仅剩 IO/STATE 校验错误（此时树形可能半裂——与调用方
 * 预算池契约一致，错误路径预算归还归调用方统一收尾）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_split_cascade(
    uint64_t leaf_blk, const bptree_path_t &trace,
    std::vector<uint64_t> &reserve_pool, size_t &pool_used,
    uint64_t owner_inode_idx, uint64_t &root_blk)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    bool did_split = false;
    uint64_t split_high_leaf = 0;
    result_code_t rc;

    split_result_t sr = extent_leaf_split(leaf_blk, reserve_pool[pool_used++]);
    if (sr.rc != RC_OK) return sr.rc;
    did_split = true;
    split_high_leaf = sr.high_node_blkidx;
    if (trace.depth == 1) {
        rc = extent_new_root_inner(sr.low_node_blkidx, sr.low_node_maxhash,
                                   sr.high_node_blkidx, owner_inode_idx, root_blk,
                                   reserve_pool[pool_used++]);
        if (rc) return rc;
    } else {
        uint64_t parent_blk = trace.blocks[trace.depth - 2];
        uint16_t child_idx = trace.child_idx[trace.depth - 2];
        bool parent_spilt = false;
        rc = extent_spilt_parent_apply(parent_blk, child_idx, sr, parent_spilt);
        if (rc) return rc;
        uint64_t cur_parent = parent_blk;
        int guard = 0;
        while (parent_spilt) {
            if (++guard > 64) return RC_ERR_STATE;
            mdata_piece_session ps(mdata_piece_mgr, cur_parent);
            if (ps.index_error()) return RC_ERR_STATE;
            if (!ps) return RC_ERR_IO;
            Bptree_extent_inner_node_t *pn = (Bptree_extent_inner_node_t *)ps.data;
            if (!is_extent_node(&pn->meta) || !pn->meta.parent_ptr.is_inner_node)
                return RC_ERR_STATE;
            const bool parent_is_root = pn->meta.parent_ptr.is_root_node;
            const uint64_t grand_blk = pn->meta.parent_ptr.ptr;
            split_result_t isr = extent_inner_split(cur_parent, reserve_pool[pool_used++]);
            if (isr.rc != RC_OK) return isr.rc;
            if (parent_is_root) {
                rc = extent_new_root_inner(isr.low_node_blkidx, isr.low_node_maxhash,
                                           isr.high_node_blkidx, owner_inode_idx,
                                           root_blk, reserve_pool[pool_used++]);
                if (rc) return rc;
                break;
            }
            if (grand_blk == PTR_INVALID_52) return RC_ERR_STATE;
            mdata_piece_session gs(mdata_piece_mgr, grand_blk);
            if (gs.index_error()) return RC_ERR_STATE;
            if (!gs) return RC_ERR_IO;
            Bptree_extent_inner_node_t *grand = (Bptree_extent_inner_node_t *)gs.data;
            if (!is_extent_node(&grand->meta) || !grand->meta.parent_ptr.is_inner_node)
                return RC_ERR_STATE;
            const uint16_t gcnt = (uint16_t)grand->meta.parent_ptr.entry_count;
            uint16_t grand_idx = 0;
            bool found = false;
            for (; grand_idx < gcnt; grand_idx++) {
                if (grand->extents[grand_idx].sub_node_ptr.ptr == cur_parent) {
                    found = true; break;
                }
            }
            if (!found) return RC_ERR_STATE;
            rc = extent_spilt_parent_apply(grand_blk, grand_idx, isr, parent_spilt);
            if (rc) return rc;
            cur_parent = grand_blk;
        }
    }

    /* 父 key 刷新：从本叶上溯；分裂后两半叶都刷。 */
    rc = extent_refresh_key_chain(leaf_blk);
    if (rc) return rc;
    if (did_split) {
        rc = extent_refresh_key_chain(split_high_leaf);
        if (rc) return rc;
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_unmap_range_loose — 子集撤销原语（unmap 家族，2026-09-07）
 *
 * 定位：unmap 家族唯一接口（2026-09-07 extent_unmap_range 退役并入——
 *   direct_file_pwrite/punch_hole/truncate 依赖分析：均不需三字段全等侦查式
 *   删除，loose 的 full 分支完整继承其防错删语义，调用面收敛到单接口）。
 *   full 分支 = "删掉树里这一整条记录"（原 unmap_range 语义：三字段全等）；其余
 *   分支 = "撤销/挖掉 [ext.fblkbase, +ext.length) 这个逻辑区间"——区间必须被
 *   树内**单条记录完全覆盖**（子集约束），树内部自行判定形态：整条删 /
 *   贴头贴尾缩水 / 中间拆两端。
 *
 * 前置契约（防错删，逻辑域 + phys 偏移自洽）：
 *   walk(ext.fblkbase) 未命中（起点空洞）→ RC_ERR_NOT_FOUND（兼作 phase2
 *   回滚的"此段未上树"信号：调用方仅 free 物理块）；命中但子区间终点越出
 *   记录（跨记录/越界）→ RC_ERR_STATE；ext.block_base 必须与记录 phys 按
 *   逻辑偏移自洽（rec.block_base + off == ext.block_base，off = 逻辑差）——
 *   防账本与树脱节。裁掉区间的 phys 由调用方按侦查/账本自算 free
 *   （缩水 = 线性映射，裁掉的逻辑段对应 phys 偏移段），loose 只动树形态。
 *
 * 三分支（侦查一次完成、树改动前全判定；会话纪律同 map_range）：
 *   full  = ext 三字段全等整条 → 内联删除机械（memmove + 欠载平衡 +
 *           根收缩链，原 unmap_range 尾段，2026-09-07 退役并入）；
 *   head  = 贴头挖 [rec.base, rec.base+ext.length)：原地改 rec ——
 *           fblkbase/length/block_base 同步右移 ext.length（phys 随逻辑
 *           线性平移，连续映射保持）。终点 (fblkbase+length) 不变 →
 *           子树 max 不变 → 父链零刷新（refresh 短路兜底）。零分配；
 *   tail  = 贴尾挖 [rec.end-ext.length, rec.end)：原地 length 减（剩头段）。
 *           count 不变 → 无欠载无分裂 → 零分配零失败面 —— phase2 左并
 *           回滚的落点（逆序账本保证每步裁切恒贴尾，见 pwrite_pipeline.md §6）；
 *   mid   = 真中间挖洞：左段留原槽缩尾（tail 式，零结构）+ 右段新插
 *           glb+1 位 → 唯此分支有插入 → 满叶（337）需预算 depth+1 先分配
 *           （失败 = 树零改动真原子，同 map 原子性）→ 缩尾+腾槽插入同
 *           一会话一次 dirty → 超满走 extent_split_cascade。右段终点 =
 *           原 rec 终点 ≤ 右邻起点（不重叠不变量）→ 天然无右邻防御需求。
 *           右段起点 > 左段新终点 → 无自重叠。中裂后记录数 +1：punch 的
 *           主形态（打洞区间命中记录中段时 1 拆 2）。
 *
 * 边界：ext.length==0 → RC_OK；缩水后记录恒非空（子集真包含）；
 * 记录被完全挖空 = full 分支（含 ext.length==rec.length 且 off==0）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_unmap_range_loose(uint64_t &root_blk,
                                                         uint64_t owner_inode_idx,
                                                         const extent_t &ext)
{
    if (ext.length == 0) return RC_OK;   /* 空区间：无操作 */

    bptree_path_t trace;
    result_code_t rc = extent_Bptree_walk(root_blk, &trace, ext.fblkbase);
    if (rc) return rc;
    if (!trace.is_leaf_hit) return RC_ERR_NOT_FOUND;   /* 起点空洞：目标不存在 */
    const uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    const uint16_t glb = trace.child_idx[trace.depth - 1];

    enum class kind_t { full, head, tail, mid } kind;
    bool need_split = false;
    bool is_root_leaf = false;      /* full 分支：删后是否根叶 */
    bool underflow = false;         /* full 分支：删后非根欠载 */
    std::vector<uint64_t> reserve_pool;   /* 分裂预算池（仅 mid + 满叶需要） */
    size_t pool_used = 0;

    {
        mdata_piece_session sess(mdata_piece_mgr, leaf_blk);
        if (sess.index_error()) return RC_ERR_STATE;
        if (!sess) return RC_ERR_IO;
        Bptree_extent_leaf_node_t *leaf = (Bptree_extent_leaf_node_t *)sess.data;
        if (!is_extent_node(&leaf->meta) || leaf->meta.parent_ptr.is_inner_node)
            return RC_ERR_STATE;
        const uint16_t count = (uint16_t)leaf->meta.parent_ptr.entry_count;
        if (glb == EXTENT_LEAF_GLB_NONE || glb >= count) return RC_ERR_STATE;

        const extent_t rec = leaf->extents[glb];   /* packed 成员禁引用绑定 → 拷贝 */

        /* ── 子集校验（相对偏移防 64 位溢出；is_leaf_hit 已保证 off < rec.length） ── */
        const uint64_t off = ext.fblkbase - rec.fblkbase;
        if (ext.length > rec.length - off) return RC_ERR_STATE;   /* 终点越出记录 */
        if (rec.block_base.ptr + off != ext.block_base.ptr)
            return RC_ERR_STATE;   /* phys 偏移自洽失败：账本与树脱节/错删 */

        /* ── 三分支判定 ── */
        const uint64_t rem_tail = rec.length - off - ext.length;   /* 挖后剩余尾长 */
        if (ext.length == rec.length) {
            kind = kind_t::full;   /* off 必为 0（子集校验下 ext.length==rec.length） */
        } else if (off == 0) {
            kind = kind_t::head;   /* 挖头 [rec.base, rec.base+ext.length) */
        } else if (rem_tail == 0) {
            kind = kind_t::tail;   /* 挖尾：剩头 [rec.base, rec.base+off) */
        } else {
            kind = kind_t::mid;
        }

        if (kind == kind_t::full) {
            /* full = 整条删（前 extent_unmap_range 机械，2026-09-07 退役内联）：
             * 三字段全等已由前置保证（off==0 && ext.length==rec.length && phys
             * 偏移自洽 → rec.ptr==ext.ptr）——侦查式删除的防错删语义完整继承，
             * 且单 walk 完成（不再二次下钻）。memmove 删 + 欠载判定。 */
            uint16_t cnt = count;
            if (glb < cnt - 1) {
                memmove(&leaf->extents[glb], &leaf->extents[glb + 1],
                        (size_t)(cnt - glb - 1) * sizeof(extent_t));
            }
            memset(&leaf->extents[cnt - 1], 0, sizeof(extent_t));
            cnt--;
            leaf->meta.parent_ptr.entry_count = cnt;
            sess.dirty();
            is_root_leaf = leaf->meta.parent_ptr.is_root_node;
            underflow = (!is_root_leaf && cnt < 168);
        } else if (kind == kind_t::head || kind == kind_t::tail) {
            /* 缩水：count/槽位不动 → 无欠载无分裂可能 → 零分配零失败面（仅 IO） */
            extent_t nr = rec;
            if (kind == kind_t::head) {
                const uint64_t cut = ext.length;
                nr.fblkbase += cut;
                nr.length -= cut;
                nr.block_base.ptr += cut;   /* phys 随逻辑线性平移（连续映射保持） */
            } else {
                nr.length = off;            /* 剩头 off 块；phys 起点未动 */
            }
            leaf->extents[glb] = nr;
            sess.dirty();
        } else {
            /* mid：拆两端 —— 左段 [rec.base, rec.base+off) 留原槽缩尾 + 右段新插。
             * 满叶（337）→ 预算 depth+1 先分配（失败 = 零改动返回）；右段终点 =
             * 原 rec 终点 ≤ 右邻起点 → 天然无右邻防御需求（不重叠不变量）。 */
            if (count == 337) {
                reserve_pool.reserve((size_t)trace.depth + 1);
                for (int i = 0; i <= trace.depth; i++) {
                    const uint64_t b = block_alloc_hard(2, 1);
                    if (b == PTR_INVALID) {
                        for (uint64_t x : reserve_pool) block_free(x, 2);
                        return RC_ERR_NO_BLOCK;   /* 未 dirty：树零改动真原子 */
                    }
                    reserve_pool.push_back(b);
                }
            }
            extent_t left = rec;
            left.length = off;                 /* 左段缩尾 */
            extent_t right = rec;
            right.fblkbase += off + ext.length;
            right.length = rem_tail;
            right.block_base.ptr += off + ext.length;
            leaf->extents[glb] = left;
            const uint16_t pos = (uint16_t)(glb + 1);
            if (pos < count) {
                memmove(&leaf->extents[pos + 1], &leaf->extents[pos],
                        (size_t)(count - pos) * sizeof(extent_t));
            }
            leaf->extents[pos] = right;
            const uint16_t ncount = (uint16_t)(count + 1);
            leaf->meta.parent_ptr.entry_count = ncount;
            need_split = ncount > 337;
            sess.dirty();
        }
    }   /* 叶会话析构写回；分裂/刷链前必须关（同块双会话不互见修改） */

    if (kind == kind_t::full) {
        /* 整删收尾（前 unmap_range 尾段）：根叶欠载/空均合法且无父 key；
         * 非根欠载 → 平衡链（含根收缩）；否则父 key 上溯刷新（删末项 max 变）。 */
        if (is_root_leaf) return RC_OK;
        if (underflow) return extent_balance_from(leaf_blk, owner_inode_idx, root_blk);
        return extent_refresh_key_chain(leaf_blk);
    }

    if (kind == kind_t::mid && need_split) {
        rc = extent_split_cascade(leaf_blk, trace, reserve_pool, pool_used,
                                  owner_inode_idx, root_blk);
        for (; pool_used < reserve_pool.size(); pool_used++)
            block_free(reserve_pool[pool_used], 2);
        return rc;
    }
    /* 缩水/不裂 mid：叶 max 变化已论证为零或已由缩尾/右段接管（mid 右段终点 =
     * 原 rec 终点）；refresh 短路兜底（head 终点不变 / tail 非叶末 / mid 均内部
     * 短路，仅 tail 叶末项真正上溯）。 */
    return extent_refresh_key_chain(leaf_blk);
}

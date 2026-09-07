#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"   /* Inode_cache_cfg_t_v2：编排构造（头里仅前置声明） */
#include "../include/ravenfs/mdata_piece_caches.hpp"  /* mdata_piece_caches_cfg_t：编排构造 */
#include "../include/ravenfs/bitmap_slices.hpp"  /* bitmap_slices_cfg_t / bitmap_slices_mgr_t：位图模块（2026-09-03 顶出） */
#include <vector>
#include <algorithm>
#include <cstring>
RavenFS_juvenile::regular_file_type_t RavenFS_juvenile::ftype(raven_Inode_t *inode)
{
    if(inode->size<=160){
        return regular_file_type_t::inline_file;
    }else{
        if(inode->is_data_root_direct_bit){
            return regular_file_type_t::direct;
        }else{
            return regular_file_type_t::extent;
        }
    }
}
result_code_t RavenFS_juvenile::direct_file_pwrite
    (uint64_t inode_idx, 
    uint64_t offset, 
    uint64_t len, 
    void *buf)
{
    //采取元数据规划
    regular_file_type_t pre_file_state;
    uint64_t data_root;
    {
        InodeSession f(this->inode_cache_mgr,inode_idx);
        //边界条件错误处理
        pre_file_state=ftype(f.inode);
        data_root=f.inode->data_root;
    }
    //接下来就是复杂的从pre_file_state状态转变了，虽然是状态转变但是是有副作用的状态转变，并且要考虑中间产生了位图资源用尽的回退这种边界条件，以及普通回滚
    //很明显初始条件是inline/direct/extent三种，我的推演发现在这个接口里面要考虑以下几个状态转变可能，但是很明显需要中间边分配边记录，最后成定论是什么状态
    //1.inline->inline态，最简单
    //2.inline->direct,扩容的时候优先尝试这个路径，hard_alloc
    //3.inline->extent,当inline->direct的升格超过2个FS_Block的时候才允许降级到这个，使用soft_alloc(笑死，一个块的时候没法撕成两个FS_block,若是一个块都没法hard_alloc出来只能说明满了)
    //4.direct->direct,不触发分配最好，触发了分配尽可能尝试调用block_occupy_soft尾部占领，尾部占领成功自然是皆大欢喜
    //5.direct->extent, 4的软占领有剩余降级这个路径
    //6.extent->extent, 待写区间落到extents上，若有空洞，尽可能从左侧区间尾巴上向右soft_occupy,能合并就合并
    //7.extent->direct,虽然极为罕见但是也有可能6的合并全文件合并到一个extent了，按照尽可能direct的宗旨就要坍缩到direct
    //不妨根据最复杂的extent初始条件的工作流程来写这个函数的五步骤：资源预分配并区间合并->根据老的extents编译一个原语操作序列->执行操作序列(map/unmap)->文件数据写入->元数据回写
    //很明显有些相对简单的边界条件可以适当简化省略对应的phase
    return result_code_t();
}

/* ═══════════════════════════════════════════════════════════════════
 * regular_file_reclaim — 归零回收（REG/SOFT_SYM link_count==0 全量空间回收）
 *
 * 2026-09-07 补票：替换旧 RC_ERR_NOT_SUPPORTED 哨兵（inode 孤儿暂挂）——
 * 接口契约（root_dir_rm_dentry 注释）本就承诺"物理删除 → inode_free"，
 * 此处把 REG 目标的数据资产按形态释放干净，零泄露。
 *
 * destroy_data=false（仅释放）：数据块 block_free，内容残留盘上（TRIM 语义）；
 * destroy_data=true（销毁）：数据块 free 前清零（防恢复，成本高）。
 *
 * 形态分流（ftype）：
 *   inline ：数据在 inode.inline_data（≤160B）——destroy 时随会话清零；槽回收。
 *   direct ：data_root 直指连续区间 [data_root, ceil(size/4096)) 数据块；
 *            destroy 先清零 → 整段 free。
 *   extent ：B+ 树（data_root = 树根）。用户指定路线（2026-09-07）：
 *            根沿 extents[0] 下钻取最左叶 → 叶链（ptrs[2]）顺序遍历：叶内逐
 *            记录释放数据块（destroy 先清零），叶块登记；链尾 → 叶块整体
 *            block_free(2)；父集合去重后自底向上逐层释放内节点，遇根止。
 *
 * 失败语义：中途失败不回滚已释放部分（位图释放幂等，重复 free 同一块是
 * 上层 bug 非本函数职责），返回错误码；调用方（dentry 删除链）按孤儿挂账
 * 上报（树删已成功，fsck 可兜）。inode 槽释放放最后（数据全清后才可回收）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::regular_file_reclaim(uint64_t inode_idx, bool destroy_data)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    constexpr uint64_t FS_BLOCK = 4096;

    /* 形态与字段：会话内判定，析构后执行释放（避免长会话占用缓存槽） */
    regular_file_type_t shape = inline_file;
    uint64_t data_root = PTR_INVALID;
    uint64_t fsize = 0;
    {
        InodeSession ino_sess(inode_cache_mgr, inode_idx);
        if (!ino_sess) return RC_ERR_IO;
        raven_Inode_t *ino = ino_sess.inode;
        if (ino->type != INODE_TYPE_REGULAR && ino->type != INODE_TYPE_SOFT_SYM)
            return RC_ERR_STATE;   /* 非 REG/SOFT_SYM：不归本回收器管 */
        shape = ftype(ino);
        data_root = ino->data_root;
        fsize = ino->size;
        if (shape == inline_file && destroy_data) {
            memset(ino->inline_data, 0, sizeof(ino->inline_data));   /* 销毁：内联数据清零 */
            ino_sess.dirty();
        }
    }

    /* 数据块清零（销毁模式）：分段批量写 0，1MB/段 */
    auto zero_range = [&](uint64_t blk, uint64_t count) -> result_code_t {
        constexpr uint64_t SEG = 256;              /* 256 × 4K = 1MB */
        std::vector<uint8_t> zeros((size_t)SEG * FS_BLOCK, 0);
        while (count > 0) {
            uint64_t n = count < SEG ? count : SEG;
            result_code_t r = write_buff(blk, n, zeros.data());
            if (r != RC_OK) return r;
            blk += n;
            count -= n;
        }
        return RC_OK;
    };

    switch (shape) {
    case inline_file:
        break;   /* 数据随 inode 槽；destroy 清零已在会话内完成 */

    case direct: {
        /* 直指区间：data_root 起 ceil(size/4096) 块（direct 前提 size>160 → ≥1 块） */
        if (data_root == PTR_INVALID_52) return RC_ERR_STATE;
        const uint64_t blocks = (fsize + FS_BLOCK - 1) / FS_BLOCK;
        if (destroy_data) {
            result_code_t r = zero_range(data_root, blocks);
            if (r != RC_OK) return r;
        }
        block_free(data_root, blocks);
        break;
    }

    case extent: {
        if (data_root == PTR_INVALID_52) return RC_ERR_STATE;
        std::vector<uint64_t> leaves;
        std::vector<uint64_t> parents;

        /* 最左叶：根沿 extents[0] 下钻 */
        uint64_t cur = data_root;
        bool found_leaf = false;
        for (int d = 0; d < 64; d++) {
            mdata_piece_session sess(mdata_piece_mgr, cur);
            if (sess.index_error()) return RC_ERR_STATE;
            if (!sess) return RC_ERR_IO;
            Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
            if (!is_extent_node(meta)) return RC_ERR_STATE;
            if (!meta->parent_ptr.is_inner_node) { found_leaf = true; break; }
            Bptree_extent_inner_node_t *inner = (Bptree_extent_inner_node_t *)sess.data;
            const uint16_t cnt = (uint16_t)inner->meta.parent_ptr.entry_count;
            if (cnt == 0) return RC_ERR_STATE;
            cur = inner->extents[0].sub_node_ptr.ptr;
        }
        if (!found_leaf) return RC_ERR_STATE;   /* 深度超限：树成环 */

        /* 叶链遍历：叶内逐记录释放数据（destroy 先清零），叶块 + 父集合登记 */
        uint64_t iter = cur;
        while (iter != PTR_INVALID_52) {
            mdata_piece_session sess(mdata_piece_mgr, iter);
            if (sess.index_error()) return RC_ERR_STATE;
            if (!sess) return RC_ERR_IO;
            Bptree_extent_leaf_node_t *leaf = (Bptree_extent_leaf_node_t *)sess.data;
            if (!is_extent_node(&leaf->meta) || leaf->meta.parent_ptr.is_inner_node)
                return RC_ERR_STATE;
            const uint16_t cnt = (uint16_t)leaf->meta.parent_ptr.entry_count;
            for (uint16_t i = 0; i < cnt; i++) {
                const extent_t e = leaf->extents[i];
                if (e.block_base.ptr == PTR_INVALID_52 || e.length == 0)
                    return RC_ERR_STATE;   /* 记录 phys 无效：结构异常，拒绝乱 free */
                if (destroy_data) {
                    result_code_t r = zero_range(e.block_base.ptr, e.length);
                    if (r != RC_OK) return r;
                }
                block_free(e.block_base.ptr, e.length);
            }
            if (!leaf->meta.parent_ptr.is_root_node)
                parents.push_back(leaf->meta.parent_ptr.ptr);
            leaves.push_back(iter);
            iter = leaf->meta.ptrs[2].ptr;
        }

        /* 叶块整体释放（数据已清，叶链遍历完） */
        for (uint64_t b : leaves) block_free(b, 2);

        /* 自底向上内层：父集合去重 → 逐层释放，遇根止（用户路线：递归向上删除） */
        std::sort(parents.begin(), parents.end());
        parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
        std::vector<uint64_t> layer = parents;
        while (!layer.empty()) {
            std::vector<uint64_t> next;
            bool saw_root = false;
            for (uint64_t b : layer) {
                mdata_piece_session sess(mdata_piece_mgr, b);
                if (sess.index_error()) return RC_ERR_STATE;
                if (!sess) return RC_ERR_IO;
                Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
                if (!is_extent_node(meta) || !meta->parent_ptr.is_inner_node)
                    return RC_ERR_STATE;
                if (meta->parent_ptr.is_root_node) saw_root = true;
                else next.push_back(meta->parent_ptr.ptr);
            }
            for (uint64_t b : layer) block_free(b, 2);
            if (saw_root) break;
            std::sort(next.begin(), next.end());
            next.erase(std::unique(next.begin(), next.end()), next.end());
            layer = next;
        }
        break;
    }
    }

    inode_free(inode_idx);   /* 数据资产清完后槽回收（最后一步，幂等位图操作） */
    return RC_OK;
}
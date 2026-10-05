#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session：8K 负载片会话 */
#include "../include/ravenfs/inode_cache_radix.hpp"   /* InodeSession：inode 访问统一走缓存 */
#include <cstring>
#include <vector>

/* ═══════════════════════════════════════════════════════════════════
 * name_data_plane.cpp — 目录名字数据面原语（v1.3；取代 v1.2 名字堆）
 *
 * 模型（disk_struct_v1.3.h raven_dir_Inode_t）：
 *   每目录一个**稠密、无空洞、8K 对齐**的逻辑字节空间：
 *     · 映射     name_data_ctl              direct(1)/tree(0) + 指针（51 位）
 *     · 片数     name_data_piece_count      实际 8K 数据片数原值
 *     · 空闲区间 name_data_free_ranges_ctl   fr B+树（inline 表尚未实现，恒 tree 态）
 *   长名（>40B）tail（≤512B）由本层分配逻辑区间；ddentry 存逻辑偏移
 *   full_char_remain_offset，经映射解析到物理。
 *
 * ── 分配粒度（2026-10-05 定：字节粒度）──
 *   逻辑空间 [0, pieces×8K) 内按**字节**分配：空闲区间 = 删长名释放 ∪ 新片未
 *   分配尾部，分配取 best-fit（最小 len ≥ 需求，见 name_data_freeranges.cpp）。
 *   一个 8K 数据片可容纳多个 tail（片内字节打包）；fr 无 len ≥ 需求才
 *   name_data_grow 增片。可能的优化：inline 空闲表
 *   （possible_inline_free_ranges[9]）当前未实现，小目录恒走 fr 树。
 *
 * ── 映射（复用通用 extent 树机械，kind = EXTENT_TREE_NAMEDATA）──
 *   direct(1) = 单连续物理段：物理块 = base + 逻辑块；逻辑块 = 偏移>>12。
 *   tree(0)   = 映射树根：逻辑块经 extent_Bptree_walk 定位 extent，
 *               物理块 = extent.phys + (逻辑块 − extent.fblkbase)。
 *   新片物理紧贴段尾 → 仍 direct；离散 → 物化映射树（direct→tree 升级）。
 * ═══════════════════════════════════════════════════════════════════ */

namespace {
inline bool nd_is_empty(const raven_dir_Inode_t *d)
{ return RFS_ND_CTL_PTR(d->name_data_ctl) == RFS_ND_CTL_PTR_INVALID; }
inline uint64_t nd_pieces(const raven_dir_Inode_t *d)
{ return RFS_ND_PIECE_COUNT(d->name_data_piece_count); }
}   /* namespace */

/* ═══════════════════════════════════════════════════════════════════
 * name_data_resolve — 逻辑偏移 → 物理块 + 块内偏移
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_resolve(raven_dir_Inode_t *dino, uint64_t offset,
                                                  uint64_t *out_fblk, uint16_t *out_off)
{
    const uint64_t ctl = dino->name_data_ctl;
    const uint64_t ptr = RFS_ND_CTL_PTR(ctl);
    if (ptr == RFS_ND_CTL_PTR_INVALID) return RC_ERR_STATE;   /* 空态 */

    /* 逻辑上界：数据面恒 pieces×8K，偏移越界 = 磁盘偏移字段损坏（防御性拒绝，
     * 防越域 read_block/write_block 读到无关块）。 */
    const uint64_t pieces = RFS_ND_PIECE_COUNT(dino->name_data_piece_count);
    if (offset >= pieces * 8192) return RC_ERR_STATE;

    if (RFS_ND_CTL_IS_DIRECT(ctl)) {   /* 单连续段直算 */
        *out_fblk = ptr + (offset >> 12);
        *out_off  = (uint16_t)(offset & 0xFFF);
        return RC_OK;
    }

    /* tree 态：走映射树定位 extent */
    const uint64_t lblock = offset >> 12;
    bptree_path_t trace;
    result_code_t rc = extent_Bptree_walk(ptr, &trace, lblock, EXTENT_TREE_NAMEDATA);
    if (rc) return rc;
    if (!trace.is_leaf_hit) return RC_ERR_STATE;

    const uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    const uint16_t idx = trace.child_idx[trace.depth - 1];
    mdata_piece_session sess(mdata_piece_mgr, leaf_blk);
    if (sess.index_error()) return RC_ERR_STATE;
    if (!sess) return RC_ERR_IO;
    const Bptree_extent_leaf_node_t *leaf = (const Bptree_extent_leaf_node_t *)sess.data;
    if (idx >= RFS_PP_COUNT(leaf->meta.parent_ptr)) return RC_ERR_STATE;
    const extent_t e = leaf->extents[idx];
    if (lblock < e.fblkbase || lblock >= e.fblkbase + e.length) return RC_ERR_STATE;
    *out_fblk = RFS_PTR_ADDR(e.block_base) + (lblock - e.fblkbase);
    *out_off  = (uint16_t)(offset & 0xFFF);
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * name_data_map_append — 追加一个 8K 数据片到逻辑尾
 *   前置：phys_base 为调用方已分配的 8K 对齐物理片起点（2 FS_block）。
 *   direct：先物化映射树根，把现有单段整条插入，再插新片，控制字转 tree。
 *   tree  ：直接在逻辑尾（pieces×2 块处）插入，根分裂经 root 引用带出。
 *   成功时同步 name_data_piece_count+1；失败零副作用（新建树节点已释放）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_map_append(uint64_t dir_inode_idx,
                                                     raven_dir_Inode_t *dino, uint64_t phys_base)
{
    const uint64_t ctl    = dino->name_data_ctl;
    const uint64_t pieces = nd_pieces(dino);

    extent_t nx;
    nx.length   = 2;                 /* 8K = 2 FS_block */
    nx.fblkbase = pieces * 2;        /* 逻辑尾 */
    RFS_PTR_SET_ADDR(nx.block_base, phys_base);

    uint64_t root;
    if (RFS_ND_CTL_IS_DIRECT(ctl)) {
        const uint64_t base = RFS_ND_CTL_PTR(ctl);
        result_code_t rc = extent_new_root_leaf(dir_inode_idx, root, EXTENT_TREE_NAMEDATA);
        if (rc) return rc;
        extent_t first;              /* 现有单连续段整条入树 */
        first.length = pieces * 2;
        first.fblkbase = 0;
        RFS_PTR_SET_ADDR(first.block_base, base);
        rc = extent_map_range(root, dir_inode_idx, first, EXTENT_TREE_NAMEDATA);
        if (rc) { extent_orphan_release(root, EXTENT_TREE_NAMEDATA); return rc; }
        rc = extent_map_range(root, dir_inode_idx, nx, EXTENT_TREE_NAMEDATA);
        if (rc) { extent_orphan_release(root, EXTENT_TREE_NAMEDATA); return rc; }
        RFS_ND_CTL_SET_PTR(dino->name_data_ctl, root);
        RFS_ND_CTL_SET_DIRECT(dino->name_data_ctl, 0);   /* 转 tree */
    } else {
        root = RFS_ND_CTL_PTR(ctl);
        result_code_t rc = extent_map_range(root, dir_inode_idx, nx, EXTENT_TREE_NAMEDATA);
        if (rc) return rc;
        RFS_ND_CTL_SET_PTR(dino->name_data_ctl, root);   /* 根分裂后写回 */
    }

    RFS_ND_PIECE_SET_COUNT(dino->name_data_piece_count, pieces + 1);
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * name_data_grow — 数据面增长一个 8K 片（direct 物理尾 soft_occupy 优先，
 * 投机失败则 hard_alloc(2,1) + 映射树注入）。成功 = piece_count+1。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_grow(uint64_t dir_inode_idx, raven_dir_Inode_t *dino)
{
    const uint64_t pieces = nd_pieces(dino);
    const uint64_t ctl = dino->name_data_ctl;

    if (RFS_ND_CTL_IS_DIRECT(ctl)) {
        const uint64_t base = RFS_ND_CTL_PTR(ctl);
        const uint64_t tail_blk = base + pieces * 2;
        uint64_t rest = 0;
        const uint64_t got = block_occupy_soft(tail_blk, 2, &rest);
        if (got == tail_blk && rest == 0) {   /* 段尾连续：保持 direct */
            RFS_ND_PIECE_SET_COUNT(dino->name_data_piece_count, pieces + 1);
            return RC_OK;
        }
        if (got != PTR_INVALID && rest < 2) block_free(got, 2 - rest);   /* 回滚部分占领 */
    }
    uint64_t new_base = block_alloc_hard(2, 1);
    if (new_base == PTR_INVALID) return RC_ERR_NO_BLOCK;
    result_code_t rc = name_data_map_append(dir_inode_idx, dino, new_base);
    if (rc) { block_free(new_base, 2); return rc; }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * name_data_alloc — 分配 tail_len（≤512B）逻辑区间，出参逻辑字节偏移
 *
 * 字节粒度（2026-10-05 定）：
 *   1. 空态 → 建首片 direct，整片 [0,8K) 作为空闲区间入 fr；
 *   2. fr best-fit：取最小 len ≥ tail_len 的区间（余段插回）→ 复用；
 *   3. 用尽（无 len ≥ 需求）→ grow 一个 8K 片，本 tail 取新片头部，
 *      余段 [L+tail, +8K) 入 fr。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_alloc(uint64_t dir_inode_idx,
                                                uint16_t tail_len, uint64_t *out_offset)
{
    if (!out_offset || tail_len == 0 || tail_len > 512) return RC_ERR_STATE;

    InodeSession ino_sess(inode_cache_mgr, dir_inode_idx);
    if (!ino_sess) return RC_ERR_IO;
    raven_dir_Inode_t *dino = (raven_dir_Inode_t *)ino_sess.inode;
    if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR) return RC_ERR_STATE;

    /* 1. 空态：建首片 direct + 整片入 fr */
    if (nd_is_empty(dino)) {
        uint64_t base = block_alloc_hard(2, 1);
        if (base == PTR_INVALID) return RC_ERR_NO_BLOCK;
        dino->name_data_ctl = RFS_ND_CTL_PACK(1, base);   /* direct(1) */
        RFS_ND_PIECE_SET_COUNT(dino->name_data_piece_count, 1);
        result_code_t rc0 = name_data_fr_insert(dir_inode_idx, dino, 0, 8192);
        if (rc0) {
            dino->name_data_ctl = RFS_ND_CTL_INACTIVE;
            RFS_ND_PIECE_SET_COUNT(dino->name_data_piece_count, 0);
            block_free(base, 2);
            return rc0;
        }
    }

    /* 2. best-fit 复用 */
    uint64_t off = 0;
    result_code_t rc = name_data_fr_take_fit(dir_inode_idx, dino, tail_len, &off);
    if (rc == RC_OK) { ino_sess.dirty(); *out_offset = off; return RC_OK; }
    if (rc != RC_ERR_NOT_FOUND) return rc;

    /* 3. 用尽：增长一片，本 tail 取新片头部 */
    const uint64_t pieces_before = nd_pieces(dino);
    rc = name_data_grow(dir_inode_idx, dino);
    if (rc) return rc;
    const uint64_t L = pieces_before * 8192;   /* 新片逻辑起点（映射稠密，恒在逻辑尾） */
    if (8192 > tail_len) {
        rc = name_data_fr_insert(dir_inode_idx, dino, L + tail_len, 8192 - tail_len);
        if (rc) return rc;   /* 余段入 fr 失败：已增长片，上报（空间记账偏差，不悬挂） */
    }
    ino_sess.dirty();
    *out_offset = L;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * name_data_free — 释放逻辑区间 [offset, offset+tail_len)（插入 fr 树）
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_free(uint64_t dir_inode_idx,
                                               uint64_t offset, uint16_t tail_len)
{
    if (tail_len == 0) return RC_OK;
    InodeSession ino_sess(inode_cache_mgr, dir_inode_idx);
    if (!ino_sess) return RC_ERR_IO;
    raven_dir_Inode_t *dino = (raven_dir_Inode_t *)ino_sess.inode;
    if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR) return RC_ERR_STATE;
    result_code_t rc = name_data_fr_insert(dir_inode_idx, dino, offset, tail_len);
    if (rc == RC_OK) ino_sess.dirty();
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════
 * name_data_read / name_data_write — 逻辑区间字节读写（≤512B，跨至多 2 块）
 * 按 4K 块 read-modify-write；direct/tree 由 name_data_resolve 统一解析。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_read(uint64_t dir_inode_idx, uint64_t offset,
                                               uint16_t tail_len, char *out_tail)
{
    if (!out_tail || tail_len == 0 || tail_len > 512) return RC_ERR_STATE;

    InodeSession ino_sess(inode_cache_mgr, dir_inode_idx);
    if (!ino_sess) return RC_ERR_IO;
    raven_dir_Inode_t *dino = (raven_dir_Inode_t *)ino_sess.inode;
    if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR) return RC_ERR_STATE;

    uint64_t fblk = 0; uint16_t off = 0;
    result_code_t rc = name_data_resolve(dino, offset, &fblk, &off);
    if (rc) return rc;

    uint8_t blk[4096];
    const uint16_t first = (off + tail_len <= 4096) ? tail_len : (uint16_t)(4096 - off);
    read_block(fblk, blk);
    memcpy(out_tail, blk + off, first);
    if (first < tail_len) {
        read_block(fblk + 1, blk);
        memcpy(out_tail + first, blk, (size_t)(tail_len - first));
    }
    return RC_OK;
}

result_code_t RavenFS_juvenile::name_data_write(uint64_t dir_inode_idx, uint64_t offset,
                                                uint16_t tail_len, const char *tail)
{
    if (!tail || tail_len == 0 || tail_len > 512) return RC_ERR_STATE;

    InodeSession ino_sess(inode_cache_mgr, dir_inode_idx);
    if (!ino_sess) return RC_ERR_IO;
    raven_dir_Inode_t *dino = (raven_dir_Inode_t *)ino_sess.inode;
    if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR) return RC_ERR_STATE;

    uint64_t fblk = 0; uint16_t off = 0;
    result_code_t rc = name_data_resolve(dino, offset, &fblk, &off);
    if (rc) return rc;

    uint8_t blk[4096];
    const uint16_t first = (off + tail_len <= 4096) ? tail_len : (uint16_t)(4096 - off);
    read_block(fblk, blk);
    memcpy(blk + off, tail, first);
    write_block(fblk, blk);
    if (first < tail_len) {
        read_block(fblk + 1, blk);
        memcpy(blk, tail + first, (size_t)(tail_len - first));
        write_block(fblk + 1, blk);
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * name_data_release — 目录销毁：全量回收（数据片 + 映射树节点 + 复位控制字）
 *
 * direct：整段归还（pieces×2 块）。
 * tree  ：叶链遍历收集每条映射（物理段）逐段归还 → extent_orphan_release
 *         释放全部映射树节点。
 * fr    ：name_data_fr_release 全量释放 fr 树节点。
 * 最后双控制字复位空态。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_release(uint64_t dir_inode_idx)
{
    InodeSession ino_sess(inode_cache_mgr, dir_inode_idx);
    if (!ino_sess) return RC_ERR_IO;
    raven_dir_Inode_t *dino = (raven_dir_Inode_t *)ino_sess.inode;
    if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR) return RC_ERR_STATE;

    /* fr 树节点先退役（无论数据面是否为空，防御性） */
    name_data_fr_release(dir_inode_idx, dino);

    const uint64_t ctl    = dino->name_data_ctl;
    const uint64_t pieces = nd_pieces(dino);
    const uint64_t ptr    = RFS_ND_CTL_PTR(ctl);

    if (ptr == RFS_ND_CTL_PTR_INVALID && pieces == 0) {
        ino_sess.dirty();   /* fr 复位落盘 */
        return RC_OK;       /* 空态：无数据资产 */
    }

    if (RFS_ND_CTL_IS_DIRECT(ctl)) {
        block_free(ptr, pieces * 2);
    } else {
        /* tree：叶链遍历逐段归还物理数据片 */
        const uint64_t PTR52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
        std::vector<extent_t> recs;
        uint64_t cur = ptr;
        for (int d = 0; d < 64 && cur != PTR52; d++) {
            mdata_piece_session s(mdata_piece_mgr, cur);
            if (s.index_error() || !s) break;
            Bptree_meta_t *m = (Bptree_meta_t *)s.data;
            if (!is_extent_node_kind(m, EXTENT_TREE_NAMEDATA)) break;
            if (!RFS_PP_IS_INNER(m->parent_ptr)) {
                const Bptree_extent_leaf_node_t *leaf = (const Bptree_extent_leaf_node_t *)s.data;
                const uint16_t cnt = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
                for (uint16_t i = 0; i < cnt; i++) recs.push_back(leaf->extents[i]);
                cur = RFS_PTR_ADDR(leaf->meta.ptrs[2]);
                continue;
            }
            const Bptree_extent_inner_node_t *inner = (const Bptree_extent_inner_node_t *)s.data;
            if (RFS_PP_COUNT(inner->meta.parent_ptr) == 0) break;
            cur = RFS_PTR_ADDR(inner->extents[0].sub_node_ptr);
        }
        for (const extent_t &e : recs)
            block_free(RFS_PTR_ADDR(e.block_base), e.length);
        extent_orphan_release(ptr, EXTENT_TREE_NAMEDATA);
    }

    /* 复位名字数据面控制字（空态） */
    dino->name_data_ctl               = RFS_ND_CTL_INACTIVE;
    dino->name_data_piece_count       = RFS_ND_PIECE_PACK(0);
    dino->name_data_free_ranges_ctl   = RFS_ND_FR_CTL_INACTIVE;
    dino->name_data_free_ranges_count = 0;
    memset(dino->possible_inline_free_ranges, 0, sizeof(dino->possible_inline_free_ranges));
    ino_sess.dirty();
    return RC_OK;
}

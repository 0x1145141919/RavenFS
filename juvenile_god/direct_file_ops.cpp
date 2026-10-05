#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"   /* Inode_cache_cfg_t_v2：编排构造（头里仅前置声明） */
#include "../include/ravenfs/mdata_piece_caches.hpp"  /* mdata_piece_caches_cfg_t：编排构造 */
#include <vector>
#include <algorithm>
#include <iterator>
#include <cstring>
RavenFS_juvenile::regular_file_type_t RavenFS_juvenile::ftype(raven_Inode_t *inode)
{
    if(inode->size<=160){
        return regular_file_type_t::inline_file;
    }else{
        if(RFS_INO1_GET_DIRECT(inode->word1)){
            return regular_file_type_t::direct;
        }else{
            return regular_file_type_t::extent;
        }
    }
}
/* ═══════════════════════════════════════════════════════════════════
 * dfprealloc_and_plan — pwrite 五段管线 phase1：资源预分配 + 区间规划
 *
 * 设计总纲见 Docs/pwrite_pipeline.md §5/§5.1（2026-09-08 定案 + 09-09 拍板）。
 * 一句话：为写域中「纯新段」（空洞/增长/物化块）预分配物理块，吐出升序
 * affect_extents 账本；inode 恒未动，失败逆序 free 整账本。
 *
 * 入参 inode = 调用方 InodeSession 作用域内的磁盘 inode 内存映像（纯读，
 * 不 dirty）。本函数自身不再开 inode 会话（09-09 拍板：避免 phase 间重复读）。
 *
 * 形态分支（判据 disk_layout §3.2，size 优先）：
 *   inline（size≤160，迁出 off+len>160）：
 *     direct 候选 = alloc_hard 整域 [0, ceil(newsize/4K)) 一次（单片连续原子）；
 *     失败降级 = 物化块 [0,1) ∪ 写域 [off>>12, newceil)，洞留空，soft 逐段。
 *   direct：
 *     覆盖写 / 增长未跨块界（newceil==oldceil）→ 零新段空返；
 *     增长段 [oldceil, newceil) = alloc_piece 锚定旧 run 物理尾
 *     （data_root+oldceil）→ 贴尾成功保 direct，部分/失败前缀保留 + soft 裂段。
 *   extent：
 *     D=[off>>12, ceil((off+len)/4K)) 与树记录求差 → 逐洞段；段起点 == 左邻
 *     记录终点 → 锚 = 左邻 phys 尾（phase2 map 左并前置），否则无锚 soft。
 *
 * 部分满足（§5.1 拍板）：锚定撞占用只认前缀 / soft 只给部分 → 缺口继续榨
 * soft 裂段（每轮 ≥1 块，有界）；唯一失败面 = 某轮一块都榨不出
 * （PTR_INVALID）→ RC_ERR_NO_BLOCK + 整体回滚。收尾合并相邻「逻辑紧贴 + phys
 * 连续」条目（等价 phase2 必发生的左并，纯减少记录数 + 让 phase1.5 判 direct
 * 不失真）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::dfprealloc_and_plan(raven_Inode_t *inode, uint64_t offset, uint64_t len,
                                                    std::list<extent_t> &affect_extents)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    constexpr uint64_t FS_BLOCK = 4096;
    constexpr uint64_t INLINE_MAX = 160;

    affect_extents.clear();
    if (!inode) return RC_ERR_STATE;
    if (RFS_INO1_GET_TYPE(inode->word1) != INODE_TYPE_REGULAR && RFS_INO1_GET_TYPE(inode->word1) != INODE_TYPE_SOFT_SYM)
        return RC_ERR_STATE;                       /* 非 REG/SOFT_SYM：不在受理范围 */
    if (len == 0) return RC_OK;                    /* 零长写无操作（POSIX：不扩文件） */
    if (offset > ~0ULL - len) return RC_ERR_STATE; /* offset+len 溢出：调用方参数错误 */

    const uint64_t fsize = inode->size;
    const uint64_t end_byte = offset + len;
    const uint64_t newsize = end_byte > fsize ? end_byte : fsize;

    /* 回滚账本：逆序 free（唯一失败出口前调用；位图 free 幂等） */
    auto rollback = [&]() {
        for (auto it = affect_extents.rbegin(); it != affect_extents.rend(); ++it)
            block_free(RFS_PTR_ADDR(it->block_base), it->length);
        affect_extents.clear();
    };

    /* 落实一段逻辑域 [a, a+need)：anchor 有效（= 左邻记录 phys 尾）时先锚定
     * occupy（物理紧贴 → phase2 左并）；撞占用只认前缀；缺口 + 无锚段走 soft
     * 榨取裂段。任何 alloc 完全失败（PTR_INVALID，一块都榨不出）→ false。
     * 成功段 append affect_extents（调用序保证 fblkbase 升序）。 */
    auto alloc_piece = [&](uint64_t a, uint64_t need, uint64_t anchor) -> bool {
        uint64_t cur = a;
        uint64_t rest = need;
        if (anchor != PTR_INVALID_52) {
            uint64_t r = rest;
            const uint64_t got = block_occupy_soft(anchor, rest, &r);
            if (got != PTR_INVALID) {              /* 锚定前缀 ≥1 块（occupy 契约） */
                const uint64_t claimed = rest - r;
                extent_t e;
                e.fblkbase = cur;
                e.length = claimed;
                RFS_PTR_SET_ADDR(e.block_base, got);
                RFS_PTR_SET_FREE(e.block_base, 0);
                affect_extents.push_back(e);
                cur += claimed;
                rest = r;
            }
        }
        while (rest > 0) {
            uint64_t r = rest;
            const uint64_t got = block_alloc_soft(rest, &r);
            if (got == PTR_INVALID) return false;  /* 唯一失败面：一块都榨不出 */
            const uint64_t claimed = rest - r;     /* ≥1（soft 契约：有返回必 ≥1） */
            extent_t e;
            e.fblkbase = cur;
            e.length = claimed;
            RFS_PTR_SET_ADDR(e.block_base, got);
            RFS_PTR_SET_FREE(e.block_base, 0);
            affect_extents.push_back(e);
            cur += claimed;
            rest = r;
        }
        return true;
    };

    const regular_file_type_t shape = ftype(inode);

    /* ── inline 迁出（size≤160 且 newsize>160）── */
    if (shape == inline_file) {
        if (newsize <= INLINE_MAX) return RC_OK;   /* inline→inline 豁免（防御：调用方本不进来） */
        const uint64_t newceil = (newsize + FS_BLOCK - 1) / FS_BLOCK;

        /* direct 候选：整域 [0, newceil) 一次 hard（单片连续 + 原子，无部分分配） */
        const uint64_t blk = block_alloc_hard(newceil, 0);
        if (blk != PTR_INVALID) {
            extent_t e;
            e.fblkbase = 0;
            e.length = newceil;
            RFS_PTR_SET_ADDR(e.block_base, blk);
            RFS_PTR_SET_FREE(e.block_base, 0);
            affect_extents.push_back(e);
            return RC_OK;
        }

        /* 降级：物化块 [0,1)（inline 内容迁出必须）∪ 写域 [off>>12, newceil)；
         * 中间洞留空不分配（extent 表示下读 0）。两段均无锚（逻辑不相邻，
         * phys 连续无收益）。off < 4096 时写域起点 == 0 → 单段整域 [0, newceil)
         * （物化块被写域吸收，勿漏写域主体！）；否则 [0,1) + [d_start, newceil)。 */
        const uint64_t d_start = offset >> 12;
        if (d_start == 0) {
            if (!alloc_piece(0, newceil, PTR_INVALID_52)) {
                rollback();
                return RC_ERR_NO_BLOCK;
            }
        } else {
            if (!alloc_piece(0, 1, PTR_INVALID_52)) {
                rollback();
                return RC_ERR_NO_BLOCK;
            }
            if (d_start < newceil) {
                if (!alloc_piece(d_start, newceil - d_start, PTR_INVALID_52)) {
                    rollback();
                    return RC_ERR_NO_BLOCK;
                }
            }
        }
        return RC_OK;
    }

    /* ── direct（size>160，直指连续区间 [data_root, +oldceil)）── */
    if (shape == direct) {
        const uint64_t oldceil = (fsize + FS_BLOCK - 1) / FS_BLOCK; /* 现有物理覆盖块数 */
        const uint64_t newceil = (newsize + FS_BLOCK - 1) / FS_BLOCK;
        if (newceil <= oldceil) return RC_OK;      /* 覆盖写 / 增长未跨块界：零新段 */
        if (inode->data_root == PTR_INVALID_52) return RC_ERR_STATE; /* direct 态必须有 base */
        /* 增长段 [oldceil, newceil)：锚 = 旧 run 物理尾 data_root+oldceil。
         * 贴尾成功 → 单段 → phase1.5 保 direct；部分/失败 → 前缀保留 +
         * soft 裂段（多段/非尾贴 → phase1.5 物化上树，物理事实不可 direct）。 */
        if (!alloc_piece(oldceil, newceil - oldceil, inode->data_root + oldceil)) {
            rollback();
            return RC_ERR_NO_BLOCK;
        }
        return RC_OK;
    }

    /* ── extent：树内逐洞规划（map-only 路径；无 direct 候选，判别留给 phase1.5）── */
    const uint64_t root = inode->data_root;
    if (root == PTR_INVALID_52) return RC_ERR_STATE;
    const uint64_t d_start = offset >> 12;
    const uint64_t d_end = (end_byte + FS_BLOCK - 1) >> 12;
    if (d_start >= d_end) return RC_OK;            /* 域空（len>0 时恒有 d_end>d_start，防御） */

    /* walk 定位 d_start：落点叶子 + GLB + 覆盖标志（零分配，失败零副作用） */
    bptree_path_t trace;
    result_code_t rc = extent_Bptree_walk(root, &trace, d_start);
    if (rc) return rc;
    const uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    const uint16_t glb = trace.child_idx[trace.depth - 1];
    const bool hit = trace.is_leaf_hit;

    extent_iterator it(this, leaf_blk, glb);
    uint64_t cur = d_start;              /* 扫描游标（逻辑块，升序推进） */
    bool have_prev = false;              /* 已消费左邻记录（锚候选） */
    uint64_t prev_end = 0;               /* 左邻记录逻辑终点 */
    uint64_t prev_pend = 0;              /* 左邻记录物理终点（block_base+length） */
    extent_t R;                          /* 当前扫描到的记录副本 */

    /* 起点预定位：把游标推进到「首个 ≥cur 的记录之前」，并备好左上下文 */
    if (hit) {
        /* GLB 覆盖 d_start：[d_start, R.end) 跳过；R 成为左上下文 */
        if (!it.current_extent(R)) return RC_ERR_STATE;   /* hit 必可读，防御 */
        prev_end = R.fblkbase + R.length;
        prev_pend = RFS_PTR_ADDR(R.block_base) + R.length;
        have_prev = true;
        cur = prev_end;
        if (cur >= d_end) return RC_OK;  /* 写域全被覆盖：零新段 */
        ++it;                            /* 前进到下一记录；树尾则 erro（循环内消化） */
    } else if (glb != EXTENT_LEAF_GLB_NONE) {
        /* 未命中但有 GLB（其终点 ≤ d_start）：作为左上下文；
         * 锚只在中段起点恰 == GLB 终点（d_start==prev_end）时成立，循环统一判定 */
        if (!it.current_extent(R)) return RC_ERR_STATE;
        prev_end = R.fblkbase + R.length;
        prev_pend = RFS_PTR_ADDR(R.block_base) + R.length;
        have_prev = true;
        ++it;
    } else {
        /* 虚拟前位（GLB==NONE）：文件头洞 / 空树，无左上下文 */
        ++it;                            /* → 首记录；空树则 erro → 循环消化为全洞段 */
    }

    /* 段发射：洞段 [a,b)（a<b，b≤d_end）；锚 ⇔ 左邻记录终点恰 == 段起点 */
    auto emit_piece = [&](uint64_t a, uint64_t b) -> bool {
        if (a >= b) return true;
        const uint64_t anchor = (have_prev && prev_end == a) ? prev_pend : PTR_INVALID_52;
        return alloc_piece(a, b - a, anchor);
    };

    /* erro 消化辅助：确认迭代器耗尽 = 真树尾（右兄弟链尽），非 IO/结构错误 */
    auto at_tree_end = [&]() -> bool {
        mdata_piece_session s(mdata_piece_mgr, it.leaf_blk());
        if (s.index_error() || !s) return false;
        auto *lf = (Bptree_extent_leaf_node_t *)s.data;
        if (!is_extent_node(&lf->meta) || RFS_PP_IS_INNER(lf->meta.parent_ptr)) return false;
        if (RFS_PP_IS_ROOT(lf->meta.parent_ptr)) return true;   /* 单叶树 */
        return RFS_PTR_ADDR(lf->meta.ptrs[2]) == PTR_INVALID_52;
    };

    for (;;) {
        if (cur >= d_end) break;
        if (!it.iterator_erro_flag && it.current_extent(R)) {
            /* it 指向下一记录 R（不变量：R.fblkbase ≥ cur） */
            if (R.fblkbase >= d_end) {
                /* 记录起点已越域：剩余 [cur, d_end) 为尾洞段，收尾 */
                if (!emit_piece(cur, d_end)) {
                    rollback();
                    return RC_ERR_NO_BLOCK;
                }
                break;
            }
            /* 洞段 [cur, R.fblkbase)（若紧邻记录则空段跳过） */
            if (!emit_piece(cur, R.fblkbase)) {
                rollback();
                return RC_ERR_NO_BLOCK;
            }
            /* 覆盖区 [R.fblkbase, R.end)：跳过，R 升级为左上下文 */
            cur = R.fblkbase + R.length;
            if (cur >= d_end) break;
            prev_end = cur;
            prev_pend = RFS_PTR_ADDR(R.block_base) + R.length;
            have_prev = true;
            ++it;
        } else {
            /* 迭代器耗尽（++ 越树尾）或 IO/结构错误：核验真树尾后收尾洞段 */
            if (it.iterator_erro_flag && at_tree_end()) {
                if (!emit_piece(cur, d_end)) {
                    rollback();
                    return RC_ERR_NO_BLOCK;
                }
                break;
            }
            return RC_ERR_STATE;         /* 树遍历异常：本路径尚无分配，零副作用 */
        }
    }

    /* 收尾：合并相邻「逻辑紧贴 + phys 连续」条目 —— 等价 phase2 map 必发生的
     * 左并（纯减少记录数；让 phase1.5 的 direct 判据（段数==1 等）不失真）。 */
    if (affect_extents.size() > 1) {
        auto prev = affect_extents.begin();
        for (auto it = std::next(prev); it != affect_extents.end();) {
            if (prev->fblkbase + prev->length == it->fblkbase &&
                RFS_PTR_ADDR(prev->block_base) + prev->length == RFS_PTR_ADDR(it->block_base)) {
                prev->length += it->length;
                it = affect_extents.erase(it);
            } else {
                prev = it;
                ++it;
            }
        }
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_new_root_leaf — 建空根叶（phase2 物化第一步，pwrite_pipeline.md §9 补票）
 *
 * alloc 2 块（8K 节点，align 2^1）→ extent_node_meta_init（根叶 count=0，
 * 审计锚 owner_inode_idx）→ ptrs[0..2] = PTR_INVALID（文件树根无链表无 ".."）。
 * root_blk 出参；失败（无块）→ RC_ERR_NO_BLOCK，root_blk 不变。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_new_root_leaf(uint64_t owner_inode_idx, uint64_t &root_blk,
                                                     extent_tree_kind_t kind)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    const uint64_t blk = block_alloc_hard(2, 1);
    if (blk == PTR_INVALID) return RC_ERR_NO_BLOCK;
    {
        mdata_piece_session s(mdata_piece_mgr, blk);
        if (s.index_error() || !s) {
            block_free(blk, 2);
            return RC_ERR_STATE;
        }
        extent_node_meta_init(s.data, 0, false, true, owner_inode_idx, kind);
        Bptree_meta_t *meta = (Bptree_meta_t *)s.data;
        RFS_PTR_SET_ADDR(meta->ptrs[0], PTR_INVALID_52);
        RFS_PTR_SET_ADDR(meta->ptrs[1], PTR_INVALID_52);
        RFS_PTR_SET_ADDR(meta->ptrs[2], PTR_INVALID_52);
        s.dirty();
    }
    root_blk = blk;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * direct_file_pwrite — pwrite 五段管线总装（pwrite_pipeline.md 全文）
 *
 * 形态决策内嵌（phase1.5 不独立成函数，09-09 拍板）：保 direct ⇔ 非树态
 * 且 affect 形状满足（inline 迁出 = 1 段 0 起始；direct = 空 或 1 段尾贴）；
 * extent 旧态免判别直出 map-only。
 *
 * 管线（含各 phase 失败回滚，inode 写 = 唯一提交点）：
 *   phase1  dfprealloc_and_plan（含 inline→inline 豁免前置判断）
 *   phase1.5 判别（本函数内嵌）
 *   phase2  [仅树路径] 物化建树（direct 旧区 map 一条零搬移）→ 逐段 map；
 *           失败：已 map 逆序 unmap_range_loose + 全段 free phys + 新建树
 *           无存活映射则 free 树根（extent 旧态树原样，零清理）
 *   phase3  数据写入（新段 zero+overlay / 旧块 read-modify-write / 整块直写）；
 *           inline 迁出 [0,pre_size) 物化拷贝 + 直接占位块批量清零
 *   phase4  一次 InodeSession 写 = 提交点；树路径提交前 O(1) 坍缩判定
 *           （根叶 && count==1 && 单条覆盖 [0,newceil)）→ 先 inode 后 free 旧根
 *
 * 写前必读：同 inode 写互斥由调用方保证（§11.5）；本函数不开锁。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::df_pipeline(uint64_t inode_idx, uint64_t offset, uint64_t len,
                                            const void *buf, bool data_is_zero)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    constexpr uint64_t FS_BLOCK = 4096;
    constexpr uint64_t INLINE_MAX = 160;

    if (len == 0) return RC_OK;                          /* POSIX：零长写无操作 */
    if (offset > ~0ULL - len) return RC_ERR_STATE;       /* offset+len 溢出 */
    const uint64_t end_byte = offset + len;

    /* ── 入口读态：一次 InodeSession（phase1 复用同映像指针；关后只用拷贝）── */
    uint64_t pre_size = 0, pre_data_root = 0;
    regular_file_type_t pre_shape = inline_file;
    uint8_t old_inline[INLINE_MAX] = {0};
    uint64_t old_inline_len = 0;
    uint64_t newsize = 0;
    std::list<extent_t> affect;
    {
        InodeSession s(inode_io, inode_idx);
        if (!s.inode) return RC_ERR_IO;
        raven_Inode_t *ino = s.inode;
        if (RFS_INO1_GET_TYPE(ino->word1) != INODE_TYPE_REGULAR && RFS_INO1_GET_TYPE(ino->word1) != INODE_TYPE_SOFT_SYM)
            return RC_ERR_STATE;
        pre_size = ino->size;
        pre_data_root = ino->data_root;
        pre_shape = ftype(ino);
        newsize = end_byte > pre_size ? end_byte : pre_size;

        /* ── inline→inline 豁免：不走管线，纯 inode 内写（零分配零树）── */
        if (pre_shape == inline_file && newsize <= INLINE_MAX) {
            if (offset > pre_size)   /* 中间未写洞清零（原内容可能越 size 有残留） */
                memset(ino->inline_data + pre_size, 0, offset - pre_size);
            memcpy(ino->inline_data + offset, buf, len);
            ino->size = newsize;
            s.dirty();
            return RC_OK;
        }
        if (pre_shape == inline_file) {
            old_inline_len = pre_size;
            memcpy(old_inline, ino->inline_data, sizeof(old_inline));   /* 物化拷贝源 */
        }

        /* ── phase1：资源预分配 + 区间规划 ── */
        result_code_t rc = dfprealloc_and_plan(ino, offset, len, affect);
        if (rc) return rc;
        if (pre_shape == inline_file && affect.empty())
            return RC_ERR_STATE;    /* 迁出必吐新段（防御：规划/豁免分派矛盾） */
    }

    const uint64_t oldceil = (pre_size + FS_BLOCK - 1) / FS_BLOCK;
    const uint64_t size_ceil = (newsize + FS_BLOCK - 1) / FS_BLOCK;   /* 提交后 size 块数（坍缩判定上界） */
    std::vector<extent_t> segs(affect.begin(), affect.end());   /* 升序账本（回滚/查找用） */

    /* ── phase1.5 形态判别（内嵌）：keep_direct 或上树 ── */
    bool keep_direct = false;
    if (pre_shape == extent) {
        keep_direct = false;                       /* 树已在 → map-only（判别只发生在非树态） */
    } else if (pre_shape == direct) {
        if (segs.empty()) {
            keep_direct = true;                    /* 覆盖写 / 增长未跨块界：base 不动 */
        } else if (segs.size() == 1) {
            const extent_t &e = segs[0];
            keep_direct = (e.fblkbase == oldceil &&
                           RFS_PTR_ADDR(e.block_base) == pre_data_root + oldceil);   /* 1 段尾贴 */
        }
    } else {   /* inline 迁出 */
        keep_direct = (segs.size() == 1 && segs[0].fblkbase == 0);
    }

    /* phase2 已开始后的统一失败清理。inode 恒未动（提交点是 phase4）；
     * mapped = 已成功 map 的段数。
     *   extent 旧态：树是原树 → 已 map 段逆序 loose 归位（零清理残留）；
     *   物化新建树：整体弃（节点块遍历释放；数据块 phys 零碰——seg 归账本、
     *     direct 旧区仍归 inode）——树可能已分裂多节点，只 free 根会泄漏。 */
    auto free_orphan_extent_tree = [&](uint64_t tree_root) {
        /* 叶链遍历登记 + 父集去重自底向上（同 regular_file_reclaim extent 分支，
         * 零数据释放；结构异常即弃疗 = 孤儿留盘 fsck 收）。 */
        std::vector<uint64_t> leaves, parents;
        uint64_t cur = tree_root;
        for (int d = 0; d < 64 && cur != PTR_INVALID_52; d++) {
            mdata_piece_session sess(mdata_piece_mgr, cur);
            if (sess.index_error() || !sess) return;
            Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
            if (!is_extent_node(meta)) return;
            if (!RFS_PP_IS_INNER(meta->parent_ptr)) {
                if (!RFS_PP_IS_ROOT(meta->parent_ptr)) parents.push_back(RFS_PP_PTR(meta->parent_ptr));
                leaves.push_back(cur);
                cur = RFS_PTR_ADDR(meta->ptrs[2]);
                continue;
            }
            auto *inner = (Bptree_extent_inner_node_t *)sess.data;
            if (RFS_PP_COUNT(inner->meta.parent_ptr) == 0) return;
            cur = RFS_PTR_ADDR(inner->extents[0].sub_node_ptr);
        }
        for (uint64_t b : leaves) block_free(b, 2);
        std::sort(parents.begin(), parents.end());
        parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
        std::vector<uint64_t> layer = parents;
        while (!layer.empty()) {
            std::vector<uint64_t> next;
            bool saw_root = false;
            for (uint64_t b : layer) {
                mdata_piece_session sess(mdata_piece_mgr, b);
                if (sess.index_error() || !sess) return;
                Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
                if (!is_extent_node(meta) || !RFS_PP_IS_INNER(meta->parent_ptr)) return;
                if (RFS_PP_IS_ROOT(meta->parent_ptr)) saw_root = true;
                else next.push_back(RFS_PP_PTR(meta->parent_ptr));
            }
            for (uint64_t b : layer) block_free(b, 2);
            if (saw_root) break;
            std::sort(next.begin(), next.end());
            next.erase(std::unique(next.begin(), next.end()), next.end());
            layer = next;
        }
    };
    uint64_t root_blk = pre_data_root;             /* extent 旧态：现树根（后续经引用更迭） */
    const bool tree_path = !keep_direct;
    auto cleanup_after_phase2 = [&](size_t mapped) {
        if (tree_path) {
            if (pre_shape == extent) {
                for (size_t i = mapped; i-- > 0;)
                    extent_unmap_range_loose(root_blk, inode_idx, segs[i]);   /* 逆序归位 */
            } else {
                free_orphan_extent_tree(root_blk);
            }
        }
        for (const extent_t &e : segs) block_free(RFS_PTR_ADDR(e.block_base), e.length);
    };

    /* ── phase2：逐段 map（仅树路径；保 direct 全程无树）── */
    if (tree_path && pre_shape != extent) {
        /* 物化：非树态跃迁 → 建空根叶（direct 旧区随后 map 一条，零搬移） */
        result_code_t rc = extent_new_root_leaf(inode_idx, root_blk);
        if (rc) {
            for (const extent_t &e : segs) block_free(RFS_PTR_ADDR(e.block_base), e.length);
            return rc;   /* 树未建成：仅 phase1 账本 */
        }
        if (pre_shape == direct) {
            extent_t oldrec;
            oldrec.fblkbase = 0;
            oldrec.length = oldceil;
            RFS_PTR_SET_ADDR(oldrec.block_base, pre_data_root);
            RFS_PTR_SET_FREE(oldrec.block_base, 0);
            rc = extent_map_range(root_blk, inode_idx, oldrec);   /* 空根叶纯插，无失败面 */
            if (rc) {
                block_free(root_blk, 2);
                for (const extent_t &e : segs) block_free(RFS_PTR_ADDR(e.block_base), e.length);
                return rc;
            }
        }
    }
    if (tree_path) {
        size_t mapped = 0;
        result_code_t rc = RC_OK;
        for (size_t i = 0; i < segs.size(); i++) {
            rc = extent_map_range(root_blk, inode_idx, segs[i]);
            if (rc) break;
            mapped++;
        }
        if (rc) {
            cleanup_after_phase2(mapped);
            return rc;
        }
    }

    /* ── phase4 前置：坍缩判定（树路径提交前 O(1) 读根；phase3 不碰树）──
     * 根叶 && count==1 && 单条覆盖 [0, newceil) → 可坍缩 direct。 */
    bool collapse = false;
    uint64_t collapse_base = 0;
    if (tree_path) {
        mdata_piece_session s(mdata_piece_mgr, root_blk);
        if (s.index_error() || !s || !is_extent_node((Bptree_meta_t *)s.data)) {
            /* 树结构异常：inode 未动，树 + 账本统一回滚（extent 旧态树归原位） */
            cleanup_after_phase2(segs.size());
            return RC_ERR_STATE;
        }
        Bptree_meta_t *meta = (Bptree_meta_t *)s.data;
        if (!RFS_PP_IS_INNER(meta->parent_ptr) &&
            (uint16_t)RFS_PP_COUNT(meta->parent_ptr) == 1) {
            const extent_t e0 = ((Bptree_extent_leaf_node_t *)s.data)->extents[0];
            if (e0.fblkbase == 0 && e0.fblkbase + e0.length == size_ceil) {
                collapse = true;
                collapse_base = RFS_PTR_ADDR(e0.block_base);
            }
        }
    }

    /* ── phase3：文件数据写入 ──
     * 新段块（affect 命中）：零垫底 + overlay 用户数据（新块未写区清零契约）；
     * 旧块：整块覆盖直写（免读），否则 read-modify-write 保周围内容。
     * inline 迁出：新块 0 额外叠加 [0, pre_size) 物化拷贝（在零垫底之上）。 */
    {
        const uint8_t *user = (const uint8_t *)buf;
        std::vector<uint8_t> tmp(FS_BLOCK);
        const bool mig = (pre_shape == inline_file);   /* inline 迁出（block 0 恒为新段） */
        const uint64_t d_start = offset >> 12;
        const uint64_t d_end = (end_byte + FS_BLOCK - 1) / FS_BLOCK;  /* 写域上界（≠newceil：end_byte<size 时勿碰旧块） */

        /* 段查找游标：segs 升序互不重叠；b 升序推进时同步前进。命中 = 新段 phys。 */
        size_t seg_i = 0;
        auto seg_phys_of = [&](uint64_t b) -> uint64_t {
            while (seg_i < segs.size() && b >= segs[seg_i].fblkbase + segs[seg_i].length)
                seg_i++;
            if (seg_i < segs.size() && b >= segs[seg_i].fblkbase)
                return RFS_PTR_ADDR(segs[seg_i].block_base) + (b - segs[seg_i].fblkbase);
            return PTR_INVALID_52;   /* 未命中 = 旧块 */
        };
        /* 旧块物理：树路径 → 树 walk；保 direct → 直指公式（仅 direct 旧态存在旧块） */
        auto old_phys_of = [&](uint64_t b, uint64_t &out) -> result_code_t {
            if (tree_path) {
                bptree_path_t tr;
                result_code_t rc = extent_Bptree_walk(root_blk, &tr, b);
                if (rc) return rc;
                if (!tr.is_leaf_hit) return RC_ERR_STATE;   /* phase1/2 覆盖不变量破坏：防御 */
                mdata_piece_session s(mdata_piece_mgr, tr.blocks[tr.depth - 1]);
                if (s.index_error() || !s) return RC_ERR_IO;
                auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
                const uint16_t glb = tr.child_idx[tr.depth - 1];
                if (glb >= (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr)) return RC_ERR_STATE;
                const extent_t &e = leaf->extents[glb];
                out = RFS_PTR_ADDR(e.block_base) + (b - e.fblkbase);
                return RC_OK;
            }
            if (b >= oldceil) return RC_ERR_STATE;   /* 保 direct：新块必在 seg 内（防御） */
            out = pre_data_root + b;
            return RC_OK;
        };
        /* 单块写入：ws/we = 本块内用户字节区间（半开，可为空）。新段块零垫底+
         * overlay（含 inline 物化拷贝）；旧块整块直写或 read-modify-write。 */
        auto write_blk = [&](uint64_t b, uint64_t ws, uint64_t we) -> result_code_t {
            const uint64_t phys = seg_phys_of(b);
            const uint64_t blk_off = b * FS_BLOCK;
            const size_t n = we > ws ? (size_t)(we - ws) : 0;
            if (phys != PTR_INVALID_52) {
                /* 新段块 */
                memset(tmp.data(), 0, FS_BLOCK);
                if (mig && b == 0 && old_inline_len > 0)
                    memcpy(tmp.data(), old_inline, old_inline_len);   /* inline 内容迁出 */
                if (n && !data_is_zero) memcpy(tmp.data() + (ws - blk_off), user + (ws - offset), n);
                write_block(phys, tmp.data());
                return RC_OK;
            }
            /* 旧块 */
            uint64_t ophys = 0;
            result_code_t rc = old_phys_of(b, ophys);
            if (rc) return rc;
            if (data_is_zero) {
                /* 增大空写：读-改-写，只清零写域字节（保块内周边） */
                read_block(ophys, tmp.data());
                if (n) memset(tmp.data() + (ws - blk_off), 0, n);
                write_block(ophys, tmp.data());
                return RC_OK;
            }
            if (ws == blk_off && we == blk_off + FS_BLOCK) {
                write_block(ophys, user + (ws - offset));   /* 整块覆盖直写，免读 */
                return RC_OK;
            }
            read_block(ophys, tmp.data());
            if (n) memcpy(tmp.data() + (ws - blk_off), user + (ws - offset), n);
            write_block(ophys, tmp.data());
            return RC_OK;
        };

        /* ① 保 direct 且 inline 迁出：占位块（[1,d_start) 与 [d_end, seg 尾)）清零 ——
         * direct 语义全覆盖，未写区必须读 0（§7；extent 路径洞无 phys 天然读 0）。 */
        if (keep_direct && mig) {
            std::vector<uint8_t> zeros(FS_BLOCK, 0);
            const uint64_t seg_base = RFS_PTR_ADDR(segs[0].block_base);
            const uint64_t seg_len = segs[0].length;
            for (uint64_t b = 1; b < d_start && b < seg_len; b++)
                write_block(seg_base + b, zeros.data());
            for (uint64_t b = d_end; b < seg_len; b++)
                write_block(seg_base + b, zeros.data());
        }

        /* ② 物化块 block 0（inline 迁出且 0 ∉ D：d_start>0，内容拷入 + 清零） */
        if (mig && d_start > 0) {
            result_code_t rc = write_blk(0, 0, 0);
            if (rc) {
                cleanup_after_phase2(tree_path ? segs.size() : 0);
                return rc;
            }
        }

        /* ③ D 逐块写（升序；seg 游标随 b 推进） */
        for (uint64_t b = d_start; b < d_end; b++) {
            const uint64_t ws = offset > b * FS_BLOCK ? offset : b * FS_BLOCK;
            const uint64_t we = end_byte < (b + 1) * FS_BLOCK ? end_byte : (b + 1) * FS_BLOCK;
            result_code_t rc = write_blk(b, ws, we);
            if (rc) {
                /* phase3 失败：已写数据不回滚（崩溃模型：③ 前 = 孤儿资源，fsck 收）；
                 * inode 未动 = 文件仍指旧态 → 树（若已建）+ 账本统一回滚。 */
                cleanup_after_phase2(tree_path ? segs.size() : 0);
                return rc;
            }
        }
    }


    /* ── phase4：inode 一次写 = 提交点（坍缩：先提交 direct 态再 free 旧根）── */
    {
        const uint64_t direct_base = keep_direct
            ? (pre_shape == inline_file ? RFS_PTR_ADDR(segs[0].block_base) : pre_data_root)
            : (collapse ? collapse_base : 0);
        InodeSession s(inode_io, inode_idx);
        if (!s.inode) {
            /* 提交前会话失败：inode 未动，树 + 账本统一回滚后报错 */
            cleanup_after_phase2(tree_path ? segs.size() : 0);
            return RC_ERR_IO;
        }
        s.inode->size = newsize;
        if (keep_direct || collapse) {
            RFS_INO1_SET_DIRECT(*s.inode, 1);
            s.inode->data_root = direct_base;
        } else {
            RFS_INO1_SET_DIRECT(*s.inode, 0);
            s.inode->data_root = root_blk;
        }
        s.dirty();
    }
    if (collapse) block_free(root_blk, 2);   /* ③ 后清扫：崩溃窗口 = 孤儿 2 块，无悬垂 */
    return RC_OK;
}

/* 薄封装：pwrite = df_pipeline(buf, data_is_zero=false)（覆盖写语义） */
result_code_t RavenFS_juvenile::direct_file_pwrite(uint64_t inode_idx, uint64_t offset, uint64_t len, void *buf)
{
    return df_pipeline(inode_idx, offset, len, buf, false);
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
        InodeSession ino_sess(inode_io, inode_idx);
        if (!ino_sess) return RC_ERR_IO;
        raven_Inode_t *ino = ino_sess.inode;
        if (RFS_INO1_GET_TYPE(ino->word1) != INODE_TYPE_REGULAR && RFS_INO1_GET_TYPE(ino->word1) != INODE_TYPE_SOFT_SYM)
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
            if (!RFS_PP_IS_INNER(meta->parent_ptr)) { found_leaf = true; break; }
            Bptree_extent_inner_node_t *inner = (Bptree_extent_inner_node_t *)sess.data;
            const uint16_t cnt = (uint16_t)RFS_PP_COUNT(inner->meta.parent_ptr);
            if (cnt == 0) return RC_ERR_STATE;
            cur = RFS_PTR_ADDR(inner->extents[0].sub_node_ptr);
        }
        if (!found_leaf) return RC_ERR_STATE;   /* 深度超限：树成环 */

        /* 叶链遍历：叶内逐记录释放数据（destroy 先清零），叶块 + 父集合登记 */
        uint64_t iter = cur;
        while (iter != PTR_INVALID_52) {
            mdata_piece_session sess(mdata_piece_mgr, iter);
            if (sess.index_error()) return RC_ERR_STATE;
            if (!sess) return RC_ERR_IO;
            Bptree_extent_leaf_node_t *leaf = (Bptree_extent_leaf_node_t *)sess.data;
            if (!is_extent_node(&leaf->meta) || RFS_PP_IS_INNER(leaf->meta.parent_ptr))
                return RC_ERR_STATE;
            const uint16_t cnt = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
            for (uint16_t i = 0; i < cnt; i++) {
                const extent_t e = leaf->extents[i];
                if (RFS_PTR_ADDR(e.block_base) == PTR_INVALID_52 || e.length == 0)
                    return RC_ERR_STATE;   /* 记录 phys 无效：结构异常，拒绝乱 free */
                if (destroy_data) {
                    result_code_t r = zero_range(RFS_PTR_ADDR(e.block_base), e.length);
                    if (r != RC_OK) return r;
                }
                block_free(RFS_PTR_ADDR(e.block_base), e.length);
            }
            if (!RFS_PP_IS_ROOT(leaf->meta.parent_ptr))
                parents.push_back(RFS_PP_PTR(leaf->meta.parent_ptr));
            leaves.push_back(iter);
            iter = RFS_PTR_ADDR(leaf->meta.ptrs[2]);
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
                if (!is_extent_node(meta) || !RFS_PP_IS_INNER(meta->parent_ptr))
                    return RC_ERR_STATE;
                if (RFS_PP_IS_ROOT(meta->parent_ptr)) saw_root = true;
                else next.push_back(RFS_PP_PTR(meta->parent_ptr));
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
/* ═══════════════════════════════════════════════════════════════════
 * direct_file_pread — 文件数据读（纯读零副作用，规格 punch_truncate_pread_spec.md §4）
 *
 * 契约（Linux filemap_read 对齐，R2/R3/R4/R6）：
 *   - len==0 → RC_OK，*out_bytes=0（不发 IO）
 *   - offset ≥ size → RC_OK，*out_bytes=0（越界读是正常结果）
 *   - offset+len 溢出 → RC_ERR_STATE
 *   - 成功：*out_bytes = min(len, size-offset)（EOF 短读）；只读 [0,size)，
 *     盘上 [size, 块上界) 残留永不拷出
 *   - 洞读 0；不分配、不 dirty、不扩文件、不维护 atime（v1.1 全系不维护时间戳）
 *   - 受理 = REGULAR only：DIR → RC_ERR_IS_DIR；SOFT_SYM/其它 → RC_ERR_STATE
 *
 * 形态分流（判据 ftype()，size 优先）：
 *   inline ：源 = inode.inline_data[offset..]（会话内 memcpy，零 IO）
 *   direct ：base+逻辑块号 直指公式，全覆盖无洞；逐块 read_block → 裁字节区间
 *   extent ：walk 定位 + iterator 游走；记录段拷 phys、洞段填 0（不读盘）
 *     树尾洞 / 记录间隙洞 = memset 0（与"从未写"不可区分，P9 语义）
 *
 * 性能注：extent 逐块 IO（每块 1 次 4K read + 节点读摊销）；连续记录段可
 * 后续换 read_buff 批量（正确性优先，先逐块）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::direct_file_pread(uint64_t inode_idx, uint64_t offset,
                                                  uint64_t len, void *buf,
                                                  uint64_t *out_bytes)
{
    constexpr uint64_t FS_BLOCK = 4096;
    constexpr uint64_t INLINE_MAX = 160;
    if (!out_bytes) return RC_ERR_STATE;
    *out_bytes = 0;
    if (len == 0) return RC_OK;                          /* R1：零长读无操作 */
    if (offset > ~0ULL - len) return RC_ERR_STATE;       /* offset+len 溢出 */
    if (!buf) return RC_ERR_STATE;

    uint8_t *dst = (uint8_t *)buf;

    /* ── 会话内：受理校验 + inline 直拷 + 非 inline 字段快照 ── */
    uint64_t fsize = 0, base = 0, root = 0;
    bool is_direct = false;
    {
        InodeSession s(inode_io, inode_idx);
        if (!s.inode) return RC_ERR_IO;
        raven_Inode_t *ino = s.inode;
        if (RFS_INO1_GET_TYPE(ino->word1) == INODE_TYPE_DIR) return RC_ERR_IS_DIR;
        if (RFS_INO1_GET_TYPE(ino->word1) != INODE_TYPE_REGULAR) return RC_ERR_STATE;  /* SOFT_SYM 拒 */
        fsize = ino->size;
        if (offset >= fsize) return RC_OK;               /* R2：越界读 = 0 字节 */
        if (fsize <= INLINE_MAX) {                       /* inline：会话内直拷 */
            const uint64_t n = len < fsize - offset ? len : fsize - offset;
            memcpy(dst, ino->inline_data + offset, n);
            *out_bytes = n;
            return RC_OK;
        }
        base = ino->data_root;
        is_direct = RFS_INO1_GET_DIRECT(ino->word1);
        root = ino->data_root;
    }   /* 会话析构（非 inline 走 IO 段），字段已快照 */

    const uint64_t clamp_end = offset + len < fsize ? offset + len : fsize; /* EOF clamp */
    const uint64_t n = clamp_end - offset;
    if (n == 0) { *out_bytes = 0; return RC_OK; }

    /* ── direct：base + 逻辑块号，全覆盖无洞 ── */
    if (is_direct) {
        if (base == PTR_INVALID) return RC_ERR_STATE;
        const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
        if (base == PTR_INVALID_52) return RC_ERR_STATE;
        std::vector<uint8_t> tmp(FS_BLOCK);
        const uint64_t first = offset >> 12;
        const uint64_t last = (clamp_end + FS_BLOCK - 1) >> 12;
        for (uint64_t b = first; b < last; b++) {
            const uint64_t blk_off = b * FS_BLOCK;
            const uint64_t lo = offset > blk_off ? offset - blk_off : 0;
            const uint64_t hi = clamp_end - blk_off < FS_BLOCK ? clamp_end - blk_off : FS_BLOCK;
            if (lo >= hi) continue;
            read_block(base + b, tmp.data());
            memcpy(dst + (blk_off + lo - offset), tmp.data() + lo, hi - lo);
        }
        *out_bytes = n;
        return RC_OK;
    }

    /* ── extent：walk + iterator；记录段拷 phys、洞段填 0 ── */
    if (root == PTR_INVALID) return RC_ERR_STATE;
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    if (root == PTR_INVALID_52) return RC_ERR_STATE;
    const uint64_t d_start = offset >> 12;
    const uint64_t d_end = (clamp_end + FS_BLOCK - 1) >> 12;

    bptree_path_t trace;
    result_code_t rc = extent_Bptree_walk(root, &trace, d_start);
    if (rc) return rc;
    const uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    const uint16_t glb = trace.child_idx[trace.depth - 1];
    const bool hit = trace.is_leaf_hit;

    extent_iterator it(this, leaf_blk, glb);
    /* 定位：hit → it 已在覆盖记录；!hit 且 glb 有效 → ++ 到首个可能覆盖记录 /
     * 首个 fblkbase > d_start 的记录；glb 虚拟前位 → ++ 进入首记录（头洞自理）。
     * 与 range_sample(测试) 同款定位，保证洞/记录边界语义一致。 */
    if (glb == EXTENT_LEAF_GLB_NONE) {
        if (it.iterator_erro_flag) return RC_ERR_STATE;
        ++it;
    } else if (!hit) {
        if (it.iterator_erro_flag) return RC_ERR_STATE;
        ++it;
    }

    std::vector<uint8_t> tmp(FS_BLOCK);
    uint64_t cur_b = d_start;
    extent_t R;

    /* 树尾核验：确认 iterator 耗尽是真树尾（读 it.leaf_blk 的 meta ptrs[2]），
     * 非 IO/结构错误（dfprealloc 同款模式）。 */
    auto at_tree_end = [&]() -> bool {
        mdata_piece_session s(mdata_piece_mgr, it.leaf_blk());
        if (s.index_error() || !s) return false;
        auto *lf = (Bptree_extent_leaf_node_t *)s.data;
        if (!is_extent_node(&lf->meta) || RFS_PP_IS_INNER(lf->meta.parent_ptr)) return false;
        if (RFS_PP_IS_ROOT(lf->meta.parent_ptr)) return true;   /* 单叶树 */
        return RFS_PTR_ADDR(lf->meta.ptrs[2]) == PTR_INVALID_52;
    };

    for (;;) {
        if (cur_b >= d_end) break;
        if (!it.iterator_erro_flag && it.current_extent(R)) {
            if (R.fblkbase + R.length <= cur_b) {
                /* GLB 已越过 cur_b（理论不达：GLB=最后 fblkbase≤target，覆盖则 hit）。
                 * 防御：本记录不覆盖 cur_b，视作洞间隙，推进 it 本记录后重判。 */
                ++it;
                continue;
            }
            if (R.fblkbase > cur_b) {
                /* 洞 [cur_b, R.fblkbase) 填 0（记录间隙洞 / 头洞） */
                const uint64_t s_lo = cur_b * FS_BLOCK;
                const uint64_t s_hi = R.fblkbase * FS_BLOCK;
                const uint64_t zlo = s_lo > offset ? s_lo : offset;
                const uint64_t zhi = s_hi < clamp_end ? s_hi : clamp_end;
                if (zhi > zlo) memset(dst + (zlo - offset), 0, zhi - zlo);
                cur_b = R.fblkbase;
                continue;   /* 下轮进入记录段 */
            }
            /* 记录段 [cur_b, min(R.end, d_end))：逐块拷 phys */
            const uint64_t last_b = R.fblkbase + R.length < d_end ? R.fblkbase + R.length : d_end;
            for (uint64_t b = cur_b; b < last_b; b++) {
                const uint64_t blk_off = b * FS_BLOCK;
                const uint64_t lo = offset > blk_off ? offset - blk_off : 0;
                const uint64_t hi = clamp_end - blk_off < FS_BLOCK ? clamp_end - blk_off : FS_BLOCK;
                if (lo >= hi) continue;
                read_block(RFS_PTR_ADDR(R.block_base) + (b - R.fblkbase), tmp.data());
                memcpy(dst + (blk_off + lo - offset), tmp.data() + lo, hi - lo);
            }
            cur_b = last_b;
            ++it;
        } else {
            /* iterator 耗尽（++ 越树尾）或 IO/结构错误：核验真树尾后收尾洞段 */
            if (it.iterator_erro_flag && at_tree_end()) {
                const uint64_t zlo = cur_b * FS_BLOCK > offset ? cur_b * FS_BLOCK : offset;
                const uint64_t zhi = clamp_end;
                if (zhi > zlo) memset(dst + (zlo - offset), 0, zhi - zlo);
                break;
            }
            return RC_ERR_STATE;   /* 树遍历异常（读路径无分配，零副作用） */
        }
    }
    *out_bytes = n;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_orphan_release — 孤儿树释放（新建树折弃，punch/truncate 物化回滚共用）
 *
 * 只释放节点 2 块（最左叶下钻 → 叶链 → 父集去重自底向上），数据块 phys 零碰
 * （归调用方账本 / 折弃时数据块归旧态仍在）。结构异常即弃疗（孤儿留盘 fsck 收）。
 * 与 direct_file_pwrite 内 free_orphan_extent_tree lambda 同机械。
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::extent_orphan_release(uint64_t tree_root, extent_tree_kind_t kind)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    if (tree_root == PTR_INVALID_52) return;
    std::vector<uint64_t> leaves, parents;
    uint64_t cur = tree_root;
    for (int d = 0; d < 64 && cur != PTR_INVALID_52; d++) {
        mdata_piece_session sess(mdata_piece_mgr, cur);
        if (sess.index_error() || !sess) return;
        Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
        if (!is_extent_node_kind(meta, kind)) return;
        if (!RFS_PP_IS_INNER(meta->parent_ptr)) {
            if (!RFS_PP_IS_ROOT(meta->parent_ptr)) parents.push_back(RFS_PP_PTR(meta->parent_ptr));
            leaves.push_back(cur);
            cur = RFS_PTR_ADDR(meta->ptrs[2]);
            continue;
        }
        auto *inner = (Bptree_extent_inner_node_t *)sess.data;
        if (RFS_PP_COUNT(inner->meta.parent_ptr) == 0) return;
        cur = RFS_PTR_ADDR(inner->extents[0].sub_node_ptr);
    }
    for (uint64_t b : leaves) block_free(b, 2);
    std::sort(parents.begin(), parents.end());
    parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
    std::vector<uint64_t> layer = parents;
    while (!layer.empty()) {
        std::vector<uint64_t> next;
        bool saw_root = false;
        for (uint64_t b : layer) {
            mdata_piece_session sess(mdata_piece_mgr, b);
            if (sess.index_error() || !sess) return;
            Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
            if (!is_extent_node_kind(meta, kind) || !RFS_PP_IS_INNER(meta->parent_ptr)) return;
            if (RFS_PP_IS_ROOT(meta->parent_ptr)) saw_root = true;
            else next.push_back(RFS_PP_PTR(meta->parent_ptr));
        }
        for (uint64_t b : layer) block_free(b, 2);
        if (saw_root) break;
        std::sort(next.begin(), next.end());
        next.erase(std::unique(next.begin(), next.end()), next.end());
        layer = next;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * file_data_release — 数据资产释放（不 free inode 槽）
 *
 * truncate 缩至 ≤160 强制回迁 inline 前，释放旧 direct run / extent 树+数据块。
 * 机械同 regular_file_reclaim 中枢（直接按 shape/data_root/fsize 快照释放），仅省
 * inode_free 一步（inode 槽留待回迁写）。inline 形 = no-op。destroy_data=true 时
 * 数据块 free 前清零（防恢复，成本高）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::file_data_release(regular_file_type_t shape, uint64_t droot,
                                                  uint64_t fsize, bool destroy_data)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    constexpr uint64_t FS_BLOCK = 4096;
    if (shape == inline_file) return RC_OK;

    auto zero_range = [&](uint64_t blk, uint64_t count) -> result_code_t {
        constexpr uint64_t SEG = 256;
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

    if (shape == direct) {
        if (droot == PTR_INVALID_52) return RC_ERR_STATE;
        const uint64_t blocks = (fsize + FS_BLOCK - 1) / FS_BLOCK;
        if (destroy_data) {
            result_code_t r = zero_range(droot, blocks);
            if (r != RC_OK) return r;
        }
        block_free(droot, blocks);
        return RC_OK;
    }

    /* extent */
    if (droot == PTR_INVALID_52) return RC_ERR_STATE;
    std::vector<uint64_t> leaves, parents;
    uint64_t cur = droot;
    bool found_leaf = false;
    for (int d = 0; d < 64; d++) {
        mdata_piece_session sess(mdata_piece_mgr, cur);
        if (sess.index_error()) return RC_ERR_STATE;
        if (!sess) return RC_ERR_IO;
        Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
        if (!is_extent_node(meta)) return RC_ERR_STATE;
        if (!RFS_PP_IS_INNER(meta->parent_ptr)) { found_leaf = true; break; }
        Bptree_extent_inner_node_t *inner = (Bptree_extent_inner_node_t *)sess.data;
        const uint16_t cnt = (uint16_t)RFS_PP_COUNT(inner->meta.parent_ptr);
        if (cnt == 0) return RC_ERR_STATE;
        cur = RFS_PTR_ADDR(inner->extents[0].sub_node_ptr);
    }
    if (!found_leaf) return RC_ERR_STATE;
    uint64_t iter = cur;
    while (iter != PTR_INVALID_52) {
        mdata_piece_session sess(mdata_piece_mgr, iter);
        if (sess.index_error()) return RC_ERR_STATE;
        if (!sess) return RC_ERR_IO;
        Bptree_extent_leaf_node_t *leaf = (Bptree_extent_leaf_node_t *)sess.data;
        if (!is_extent_node(&leaf->meta) || RFS_PP_IS_INNER(leaf->meta.parent_ptr))
            return RC_ERR_STATE;
        const uint16_t cnt = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
        for (uint16_t i = 0; i < cnt; i++) {
            const extent_t e = leaf->extents[i];
            if (RFS_PTR_ADDR(e.block_base) == PTR_INVALID_52 || e.length == 0) return RC_ERR_STATE;
            if (destroy_data) {
                result_code_t r = zero_range(RFS_PTR_ADDR(e.block_base), e.length);
                if (r != RC_OK) return r;
            }
            block_free(RFS_PTR_ADDR(e.block_base), e.length);
        }
        if (!RFS_PP_IS_ROOT(leaf->meta.parent_ptr))
            parents.push_back(RFS_PP_PTR(leaf->meta.parent_ptr));
        leaves.push_back(iter);
        iter = RFS_PTR_ADDR(leaf->meta.ptrs[2]);
    }
    for (uint64_t b : leaves) block_free(b, 2);
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
            if (!is_extent_node(meta) || !RFS_PP_IS_INNER(meta->parent_ptr)) return RC_ERR_STATE;
            if (RFS_PP_IS_ROOT(meta->parent_ptr)) saw_root = true;
            else next.push_back(RFS_PP_PTR(meta->parent_ptr));
        }
        for (uint64_t b : layer) block_free(b, 2);
        if (saw_root) break;
        std::sort(next.begin(), next.end());
        next.erase(std::unique(next.begin(), next.end()), next.end());
        layer = next;
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * direct_file_punch_hole — 打洞（KEEP_SIZE，恒不写 size；规格 punch_truncate_pread_spec.md §5）
 *
 * 契约（Linux ext4 punch_hole 对齐，P3/P4/P5/P6/P7）：
 *   - REG only：DIR → RC_ERR_IS_DIR；SOFT_SYM → RC_ERR_STATE
 *   - offset≥size → RC_OK（no-op，P3）；len==0 → RC_OK；溢出 → RC_ERR_STATE
 *   - clamp_end = min(off+len, round_up(size,4096))（P4）
 *   - 段化（P5/P6）：头/尾残块就地清零（保留）；整块域 [db_start,db_end) 释放
 *   - 形态分流（§5.2）：
 *       inline  ：就地 memset inline_data（无块可释，P7）
 *       direct  ：整块域空 → 纯字节清零保 direct；整块域非空 → 物化上树（§5.3）
 *       extent  ：残块清零；整块域非空 → 逐记录 loose（§5.4，部分生效可接受）
 *   - 无 phase4 坍缩出口（KEEP_SIZE；洞不可恢复 → 出口恒 extent 或空树/直保 direct）
 *
 * 残块清零采用"字节域零化"统一机械：整块域空 → zero [off,clamp_end)；非空 →
 * zero [off, db_start*4096)（头残块）+ [db_end*4096, clamp_end)（尾残块），中间
 * 整块域跳过（释放/已洞）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::direct_file_punch_hole(uint64_t inode_idx, uint64_t offset, uint64_t len)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    constexpr uint64_t FS_BLOCK = 4096;
    constexpr uint64_t INLINE_MAX = 160;

    uint64_t size = 0, droot = 0;
    bool is_direct = false;
    regular_file_type_t shape = inline_file;
    {
        InodeSession s(inode_io, inode_idx);
        if (!s.inode) return RC_ERR_IO;
        raven_Inode_t *ino = s.inode;
        if (RFS_INO1_GET_TYPE(ino->word1) == INODE_TYPE_DIR) return RC_ERR_IS_DIR;
        if (RFS_INO1_GET_TYPE(ino->word1) != INODE_TYPE_REGULAR) return RC_ERR_STATE;
        size = ino->size;
        if (len == 0) return RC_OK;                  /* P2：零长 → no-op */
        if (offset > ~0ULL - len) return RC_ERR_STATE;  /* 溢出防御在前（§4 同款 pread） */
        if (offset >= size) return RC_OK;            /* P3：洞越 EOF → no-op */
        shape = ftype(ino);
        droot = ino->data_root;
        is_direct = RFS_INO1_GET_DIRECT(ino->word1);

        if (shape == inline_file) {
            /* inline：洞域 ⊆ [0,size)，就地清零（无块级概念） */
            const uint64_t he = (offset + len < size) ? (offset + len) : size;
            if (he > offset) memset(ino->inline_data + offset, 0, he - offset);
            s.dirty();
            return RC_OK;
        }
    }

    const uint64_t roundup_size = ((size + FS_BLOCK - 1) / FS_BLOCK) * FS_BLOCK;
    const uint64_t clamp_end = (offset + len < roundup_size) ? (offset + len) : roundup_size;
    if (offset >= clamp_end) return RC_OK;   /* 防御 */
    const uint64_t db_start = (offset + FS_BLOCK - 1) >> 12;   /* ceil(off/4096) */
    const uint64_t db_end = clamp_end >> 12;
    const uint64_t oldceil = (size + FS_BLOCK - 1) >> 12;

    /* ── phys 解析 + 字节零化 ── */
    auto extent_phys_of = [&](uint64_t root_blk, uint64_t b, uint64_t &phys) -> bool {
        if (root_blk == PTR_INVALID_52) return false;
        bptree_path_t tr;
        if (extent_Bptree_walk(root_blk, &tr, b) != RC_OK) return false;
        if (!tr.is_leaf_hit) return false;
        mdata_piece_session s(mdata_piece_mgr, tr.blocks[tr.depth - 1]);
        if (s.index_error() || !s) return false;
        auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
        const uint16_t glb = tr.child_idx[tr.depth - 1];
        if (glb >= (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr)) return false;
        const extent_t &e = leaf->extents[glb];
        phys = RFS_PTR_ADDR(e.block_base) + (b - e.fblkbase);
        return true;
    };
    auto block_phys_of = [&](uint64_t b, uint64_t &phys) -> bool {
        if (is_direct) {
            if (droot == PTR_INVALID_52) return false;
            phys = droot + b;
            return true;
        }
        return extent_phys_of(droot, b, phys);
    };
    auto zero_block_range = [&](uint64_t b, uint64_t lo, uint64_t hi) {
        if (lo >= hi) return;
        uint64_t phys = 0;
        if (!block_phys_of(b, phys)) return;   /* 洞：已读 0，跳过 */
        std::vector<uint8_t> tmp(FS_BLOCK);
        read_block(phys, tmp.data());
        memset(tmp.data() + lo, 0, hi - lo);
        write_block(phys, tmp.data());
    };
    auto zero_byte_range = [&](uint64_t lo, uint64_t hi) {
        if (lo >= hi) return;
        const uint64_t b_first = lo >> 12, b_last = (hi - 1) >> 12;
        for (uint64_t b = b_first; b <= b_last; b++) {
            const uint64_t blk_off = b * FS_BLOCK;
            const uint64_t lo_in = (lo > blk_off) ? (lo - blk_off) : 0;
            const uint64_t hi_in = (hi < blk_off + FS_BLOCK) ? (hi - blk_off) : FS_BLOCK;
            zero_block_range(b, lo_in, hi_in);
        }
    };
    auto commit_root = [&](uint64_t new_root) {
        InodeSession s(inode_io, inode_idx);
        if (!s.inode) return;
        s.inode->data_root = new_root;
        s.dirty();
    };

    /* ── direct → extent 物化（整块域非空，§5.3）── */
    if (is_direct && db_start < db_end) {
        uint64_t new_root = 0;
        result_code_t rc = extent_new_root_leaf(inode_idx, new_root);
        if (rc) return rc;                       /* 建树前：零改动 */
        if (db_start > 0) {
            extent_t seg;
            seg.fblkbase = 0; seg.length = db_start;
            RFS_PTR_SET_ADDR(seg.block_base, droot); RFS_PTR_SET_FREE(seg.block_base, 0);
            rc = extent_map_range(new_root, inode_idx, seg);
            if (rc) { extent_orphan_release(new_root); return rc; }
        }
        if (db_end < oldceil) {
            extent_t seg;
            seg.fblkbase = db_end; seg.length = oldceil - db_end;
            RFS_PTR_SET_ADDR(seg.block_base, droot + db_end); RFS_PTR_SET_FREE(seg.block_base, 0);
            rc = extent_map_range(new_root, inode_idx, seg);
            if (rc) { extent_orphan_release(new_root); return rc; }
        }
        /* 残块字节清零（inode 未提交，现仍 direct → droot+块号） */
        zero_byte_range(offset, db_start * FS_BLOCK);
        zero_byte_range(db_end * FS_BLOCK, clamp_end);
        /* 提交：direct_bit=0、data_root=new_root、size 不变 */
        {
            InodeSession s(inode_io, inode_idx);
            if (!s.inode) { extent_orphan_release(new_root); return RC_ERR_IO; }
            RFS_INO1_SET_DIRECT(*s.inode, 0);
            s.inode->data_root = new_root;
            s.dirty();
        }
        /* 洞整块域 phys 释放（先 inode 后 free：孤儿窗口无悬垂） */
        block_free(droot + db_start, db_end - db_start);
        return RC_OK;
    }

    /* ── 残块字节清零（direct 保/ extent）── */
    if (db_start >= db_end) {
        zero_byte_range(offset, clamp_end);              /* 无整块域：纯字节零化 */
    } else {
        zero_byte_range(offset, db_start * FS_BLOCK);
        zero_byte_range(db_end * FS_BLOCK, clamp_end);
    }

    /* ── extent 逐记录 loose（整块域非空，§5.4）── */
    if (!is_direct && db_start < db_end) {
        uint64_t root_blk = droot;
        std::vector<std::pair<uint64_t, uint64_t>> freed;   /* phys,len 洞整块域释放（提交后 free） */
        for (int guard = 0; guard < 100000; guard++) {
            bptree_path_t trace;
            result_code_t rc = extent_Bptree_walk(root_blk, &trace, db_start);
            if (rc) { if (root_blk != droot) commit_root(root_blk); return rc; }
            const uint64_t leaf = trace.blocks[trace.depth - 1];
            const uint16_t glb = trace.child_idx[trace.depth - 1];
            const bool hit = trace.is_leaf_hit;
            extent_iterator it(this, leaf, glb);
            if (it.iterator_erro_flag) break;
            if (glb == EXTENT_LEAF_GLB_NONE) { ++it; if (it.iterator_erro_flag) break; }
            else if (!hit) { ++it; if (it.iterator_erro_flag) break; }
            extent_t e;
            bool found = false;
            uint64_t lo_ext = 0, len_ext = 0, phys_ext = 0;
            while (!it.iterator_erro_flag && it.current_extent(e)) {
                if (e.fblkbase >= db_end) break;             /* 已越洞域 */
                if (e.fblkbase + e.length > db_start) {
                    const uint64_t s = (e.fblkbase > db_start) ? e.fblkbase : db_start;
                    const uint64_t t = (e.fblkbase + e.length < db_end) ? (e.fblkbase + e.length) : db_end;
                    if (s < t) {
                        lo_ext = s; len_ext = t - s;
                        phys_ext = RFS_PTR_ADDR(e.block_base) + (s - e.fblkbase);
                        found = true; break;
                    }
                }
                ++it;
            }
            if (!found) break;
            extent_t lext;
            lext.fblkbase = lo_ext; lext.length = len_ext;
            RFS_PTR_SET_ADDR(lext.block_base, phys_ext); RFS_PTR_SET_FREE(lext.block_base, 0);
            rc = extent_unmap_range_loose(root_blk, inode_idx, lext);
            if (rc) {
                /* 部分 punch：已生效不回滚（§5.4）——root 若变先提交，报错 */
                if (root_blk != droot) commit_root(root_blk);
                return rc;
            }
            const uint64_t fptr = RFS_PTR_ADDR(lext.block_base);
            freed.push_back({fptr, lext.length});
        }
        if (root_blk != droot) commit_root(root_blk);
        for (auto &f : freed) block_free(f.first, f.second);   /* 洞整块域释放（loose 后无引用，无悬垂） */
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * direct_file_trunacate — 截断（命名 typo 保持现名，P8；规格 punch_truncate_pread_spec.md §6）
 *
 * 语义分流（§6.1）：== size → RC_OK no-op；> size → 增大（§6.5）；< size → 缩小（§6.2/6.3/6.4）。
 *
 * 缩小（tail-punch 编译）：
 *   - 域 = [len_new, size)；释放整块域 [newceil, oldceil)；EOF 残块（len_new 非对齐）
 *     保留 + [len_new, 4096*newceil) 清零到块上界（T3 stale 防护）
 *   - 出口（§6.3）：len_new≤160 → 强制回迁 inline（读回 + 释放外置资产 + 复位）；
 *     160<len_new 且 direct → 直接缩 run（释放尾块，保 direct）；
 *     160<len_new 且 extent → 尾 loose + 出口坍缩判定（根叶单条覆盖 [0,newceil) → direct）
 *
 * 增大（§6.5 P4）：
 *   - inline ≤160：补零推进（memset + size）
 *   - inline >160 / direct：df_pipeline 空写骨架（零垫底 + 无 overlay）
 *   - extent：纯 size 推进零分配（真稀疏）
 *
 * 统一纪律（§6.4）：inode 一次写（size+形态+data_root）= 唯一提交点，先于一切 free
 * （坍缩旧根 free、direct 尾块 free、回迁全资产 free 全部后置 → 孤儿窗口，无悬垂）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::direct_file_trunacate(uint64_t inode_idx, uint64_t len_new)
{
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    constexpr uint64_t FS_BLOCK = 4096;
    constexpr uint64_t INLINE_MAX = 160;

    uint64_t size = 0, droot = 0;
    bool is_direct = false;
    regular_file_type_t shape = inline_file;
    bool pipeline_grow = false;
    {
        InodeSession s(inode_io, inode_idx);
        if (!s.inode) return RC_ERR_IO;
        raven_Inode_t *ino = s.inode;
        if (RFS_INO1_GET_TYPE(ino->word1) == INODE_TYPE_DIR) return RC_ERR_IS_DIR;
        if (RFS_INO1_GET_TYPE(ino->word1) != INODE_TYPE_REGULAR) return RC_ERR_STATE;
        size = ino->size;
        shape = ftype(ino);
        droot = ino->data_root;
        is_direct = RFS_INO1_GET_DIRECT(ino->word1);

        if (len_new == size) return RC_OK;              /* T7 no-op */

        if (len_new > size) {
            /* ── 增大（§6.5）── */
            if (shape == inline_file && len_new <= INLINE_MAX) {
                memset(ino->inline_data + size, 0, len_new - size);   /* 补零推进 */
                ino->size = len_new;
                s.dirty();
                return RC_OK;
            }
            if (shape == extent) {
                ino->size = len_new;   /* 纯 size 推进零分配（真稀疏） */
                s.dirty();
                return RC_OK;
            }
            pipeline_grow = true;   /* inline>160 / direct → df_pipeline 空写 */
            /* 注：inline>160 与 direct 的增长段在 df_pipeline 内分配/零垫底 */
        } else {
            /* len_new < size → 缩小 */
            if (shape == inline_file) {
                /* size≤160 的 inline 截断：纯收 size */
                ino->size = len_new;
                s.dirty();
                return RC_OK;
            }
            /* size>160：缩小落到 scope 外处理（需多会话：pread 读回 / 释放 / 提交） */
        }
    }

    if (pipeline_grow)
        return df_pipeline(inode_idx, size, len_new - size, nullptr, true);

    /* ── 缩小（size>160，shape = direct / extent）── */
    const uint64_t oldceil = (size + FS_BLOCK - 1) >> 12;
    const uint64_t newceil = (len_new + FS_BLOCK - 1) >> 12;

    /* §6.3.1：len_new ≤ 160 → 强制回迁 inline（读回 + 释放外置资产 + 复位） */
    if (len_new <= INLINE_MAX) {
        uint8_t buf[INLINE_MAX];
        uint64_t got = 0;
        result_code_t rc = direct_file_pread(inode_idx, 0, len_new, buf, &got);
        if (rc) return rc;
        if (got != len_new) return RC_ERR_STATE;   /* 防御：读不足 */
        /* 提交点：inode 一次写（size + 形态 + data_root 同写）——先于一切 free */
        {
            InodeSession s(inode_io, inode_idx);
            if (!s.inode) return RC_ERR_IO;
            s.inode->size = len_new;
            RFS_INO1_SET_DIRECT(*s.inode, 0);
            s.inode->data_root = PTR_INVALID;
            memcpy(s.inode->inline_data, buf, len_new);
            s.dirty();
        }
        /* 后置：释放旧外置资产（direct run / extent 树+数据块；孤儿窗口 fsck 收） */
        rc = file_data_release(shape, droot, size, false);
        if (rc) return rc;
        return RC_OK;
    }

    /* §6.3.2：160 < len_new 且 direct → 直接缩 run（保 direct；零树操作） */
    if (is_direct) {
        /* EOF 残块清零到块上界（len_new 非对齐时） */
        if (len_new % FS_BLOCK != 0) {
            const uint64_t eof_blk = len_new >> 12;
            if (eof_blk < oldceil) {
                const uint64_t phys = droot + eof_blk;
                std::vector<uint8_t> tmp(FS_BLOCK);
                read_block(phys, tmp.data());
                memset(tmp.data() + (len_new - eof_blk * FS_BLOCK), 0, newceil * FS_BLOCK - len_new);
                write_block(phys, tmp.data());
            }
        }
        {
            InodeSession s(inode_io, inode_idx);
            if (!s.inode) return RC_ERR_IO;
            s.inode->size = len_new;   /* base/direct_bit 不动 */
            s.dirty();
        }
        /* 后置：释放尾块 [newceil, oldceil) */
        block_free(droot + newceil, oldceil - newceil);
        return RC_OK;
    }

    /* §6.3.3：160 < len_new 且 extent → 尾洞 loose + 出口坍缩判定 */
    {
        uint64_t root_blk = droot;
        std::vector<std::pair<uint64_t, uint64_t>> freed;
        auto extent_phys_of = [&](uint64_t rblk, uint64_t b, uint64_t &phys) -> bool {
            if (rblk == PTR_INVALID_52) return false;
            bptree_path_t tr;
            if (extent_Bptree_walk(rblk, &tr, b) != RC_OK) return false;
            if (!tr.is_leaf_hit) return false;
            mdata_piece_session s(mdata_piece_mgr, tr.blocks[tr.depth - 1]);
            if (s.index_error() || !s) return false;
            auto *leaf = (Bptree_extent_leaf_node_t *)s.data;
            const uint16_t glb = tr.child_idx[tr.depth - 1];
            if (glb >= (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr)) return false;
            const extent_t &e = leaf->extents[glb];
            phys = RFS_PTR_ADDR(e.block_base) + (b - e.fblkbase);
            return true;
        };
        for (int guard = 0; guard < 100000; guard++) {
            bptree_path_t trace;
            result_code_t rc = extent_Bptree_walk(root_blk, &trace, newceil);
            if (rc) {
                /* 部分 shrink：root 若变先提交，防悬挂 */
                if (root_blk != droot) {
                    InodeSession s(inode_io, inode_idx);
                    if (s.inode) { s.inode->data_root = root_blk; s.dirty(); }
                }
                return rc;
            }
            const uint64_t leaf = trace.blocks[trace.depth - 1];
            const uint16_t glb = trace.child_idx[trace.depth - 1];
            const bool hit = trace.is_leaf_hit;
            extent_iterator it(this, leaf, glb);
            if (it.iterator_erro_flag) break;
            if (glb == EXTENT_LEAF_GLB_NONE) { ++it; if (it.iterator_erro_flag) break; }
            else if (!hit) { ++it; if (it.iterator_erro_flag) break; }
            extent_t e;
            bool found = false;
            uint64_t lo_ext = 0, len_ext = 0, phys_ext = 0;
            while (!it.iterator_erro_flag && it.current_extent(e)) {
                if (e.fblkbase >= oldceil) break;
                if (e.fblkbase + e.length > newceil) {
                    const uint64_t s = (e.fblkbase > newceil) ? e.fblkbase : newceil;
                    const uint64_t t = (e.fblkbase + e.length < oldceil) ? (e.fblkbase + e.length) : oldceil;
                    if (s < t) {
                        lo_ext = s; len_ext = t - s;
                        phys_ext = RFS_PTR_ADDR(e.block_base) + (s - e.fblkbase);
                        found = true; break;
                    }
                }
                ++it;
            }
            if (!found) break;
            extent_t lext;
            lext.fblkbase = lo_ext; lext.length = len_ext;
            RFS_PTR_SET_ADDR(lext.block_base, phys_ext); RFS_PTR_SET_FREE(lext.block_base, 0);
            rc = extent_unmap_range_loose(root_blk, inode_idx, lext);
            if (rc) {
                /* 部分 shrink：已 loose 生效不回滚（合法形态）——root 若变先提交，防悬挂 */
                if (root_blk != droot) {
                    InodeSession s(inode_io, inode_idx);
                    if (s.inode) { s.inode->data_root = root_blk; s.dirty(); }
                }
                return rc;
            }
            const uint64_t fptr = RFS_PTR_ADDR(lext.block_base);
            freed.push_back({fptr, lext.length});
        }

        /* EOF 残块清零到块上界（len_new 非对齐；block newceil-1 保留，经树解析 phys） */
        if (len_new % FS_BLOCK != 0) {
            const uint64_t eof_blk = len_new >> 12;
            uint64_t phys = 0;
            if (extent_phys_of(root_blk, eof_blk, phys)) {
                std::vector<uint8_t> tmp(FS_BLOCK);
                read_block(phys, tmp.data());
                memset(tmp.data() + (len_new - eof_blk * FS_BLOCK), 0, newceil * FS_BLOCK - len_new);
                write_block(phys, tmp.data());
            }
        }

        /* 出口坍缩判定：根叶 && count==1 && 单条覆盖 [0,newceil) → direct */
        bool collapse = false;
        uint64_t collapse_base = 0;
        {
            mdata_piece_session s(mdata_piece_mgr, root_blk);
            if (s.index_error() || !s) return RC_ERR_STATE;
            Bptree_meta_t *meta = (Bptree_meta_t *)s.data;
            if (!is_extent_node(meta)) return RC_ERR_STATE;
            if (!RFS_PP_IS_INNER(meta->parent_ptr) &&
                (uint16_t)RFS_PP_COUNT(meta->parent_ptr) == 1) {
                const extent_t e0 = ((Bptree_extent_leaf_node_t *)s.data)->extents[0];
                if (e0.fblkbase == 0 && e0.fblkbase + e0.length == newceil) {
                    collapse = true;
                    collapse_base = RFS_PTR_ADDR(e0.block_base);
                }
            }
        }

        /* 提交点（inode 一次写）——先于一切 free */
        {
            InodeSession s(inode_io, inode_idx);
            if (!s.inode) return RC_ERR_IO;
            s.inode->size = len_new;
            if (collapse) {
                RFS_INO1_SET_DIRECT(*s.inode, 1);
                s.inode->data_root = collapse_base;
            } else {
                RFS_INO1_SET_DIRECT(*s.inode, 0);
                s.inode->data_root = root_blk;
            }
            s.dirty();
        }
        if (collapse) block_free(root_blk, 2);   /* 坍缩旧根：孤儿 2 块，无悬垂 */
        for (auto &f : freed) block_free(f.first, f.second);   /* 释放尾块数据 */
        return RC_OK;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * extent_tree_collapse_to_direct — 单 extent 树 → direct 降级（定制接口，2026-09-30）
 *
 * 传入 owner inode 索引：若其数据形态为 extent 树、且该树**内部只有单条记录**
 * （根叶 count==1）且该记录完整覆盖 [0, ceil(size/4K))（direct 语义所需：
 * 无前缀洞 / 无尾洞）→ 降级为 direct：
 *   · 提交点：一次 InodeSession 写 direct_bit=1、data_root=记录 phys 基址（size 不动）；
 *   · 后置：free 旧树节点（根叶 = 2 个 FS_block）。
 * 先 inode 后 free —— 崩溃窗口 = 孤儿 2 块，无悬垂。
 *
 * 返回：RC_OK（已检查；*out_collapsed 报是否发生降级）/ RC_ERR_IO（inode 载入失败）/
 *       RC_ERR_STATE（非 REG/LNK、data_root 悬空等结构异常）。
 * 不适用（inline/direct 态、树非单条、单条非整域覆盖）= RC_OK + 未降级，零副作用。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::extent_tree_collapse_to_direct(uint64_t owner_inode_idx, bool *out_collapsed)
{
    if (out_collapsed) *out_collapsed = false;
    constexpr uint64_t FS_BLOCK = 4096;
    const uint64_t PTR_INVALID_52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);

    uint64_t fsize = 0, root_blk = 0;
    {   /* 入口读态：受理校验 + 形态/树根快照（会话内判，关后只用拷贝） */
        InodeSession s(inode_io, owner_inode_idx);
        if (!s.inode) return RC_ERR_IO;
        raven_Inode_t *ino = s.inode;
        const uint32_t t = RFS_INO1_GET_TYPE(ino->word1);
        if (t != INODE_TYPE_REGULAR && t != INODE_TYPE_SOFT_SYM)
            return RC_ERR_STATE;                       /* 非 REG/LNK：不受理 */
        fsize = ino->size;
        if (fsize <= 160) return RC_OK;                /* inline 态：无树可降级 */
        if (RFS_INO1_GET_DIRECT(ino->word1)) return RC_OK;  /* 已是 direct */
        root_blk = ino->data_root;
    }
    if (root_blk == PTR_INVALID_52 || root_blk == PTR_INVALID) return RC_ERR_STATE;

    /* 结构判定：根必须是叶 + 单条记录 + 整域覆盖 [0, ceil(size/4K)) */
    const uint64_t size_ceil = (fsize + FS_BLOCK - 1) / FS_BLOCK;
    uint64_t direct_base = 0;
    {
        mdata_piece_session s(mdata_piece_mgr, root_blk);
        if (s.index_error()) return RC_ERR_STATE;
        if (!s) return RC_ERR_IO;
        Bptree_meta_t *meta = (Bptree_meta_t *)s.data;
        if (!is_extent_node(meta)) return RC_ERR_STATE;
        if (RFS_PP_IS_INNER(meta->parent_ptr)) return RC_OK;              /* 内部节点 → 非单条树 */
        if ((uint16_t)RFS_PP_COUNT(meta->parent_ptr) != 1) return RC_OK;  /* 叶多记录 */
        const extent_t e0 = ((Bptree_extent_leaf_node_t *)s.data)->extents[0];
        if (e0.fblkbase != 0 || e0.length != size_ceil) return RC_OK;     /* 非整域覆盖：不可 direct */
        if (RFS_PTR_ADDR(e0.block_base) == PTR_INVALID_52) return RC_ERR_STATE;
        direct_base = RFS_PTR_ADDR(e0.block_base);
    }

    /* 提交点：inode 一次写（direct 态，size 不动）——先于 free */
    {
        InodeSession s(inode_io, owner_inode_idx);
        if (!s.inode) return RC_ERR_IO;
        RFS_INO1_SET_DIRECT(*s.inode, 1);
        s.inode->data_root = direct_base;
        s.dirty();
    }
    block_free(root_blk, 2);   /* 释放旧树节点（根叶 2 块；孤儿窗口，无悬垂） */
    if (out_collapsed) *out_collapsed = true;
    return RC_OK;
}

#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"   /* Inode_cache_cfg_t_v2：编排构造（头里仅前置声明） */
#include "../include/ravenfs/mdata_piece_caches.hpp"  /* mdata_piece_caches_cfg_t：编排构造 */
#include "../include/ravenfs/bitmap_slices.hpp"  /* bitmap_slices_cfg_t / bitmap_slices_mgr_t：位图模块（2026-09-03 顶出） */
#include <vector>
#include <algorithm>
#include <iterator>
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
    if (inode->type != INODE_TYPE_REGULAR && inode->type != INODE_TYPE_SOFT_SYM)
        return RC_ERR_STATE;                       /* 非 REG/SOFT_SYM：不在受理范围 */
    if (len == 0) return RC_OK;                    /* 零长写无操作（POSIX：不扩文件） */
    if (offset > ~0ULL - len) return RC_ERR_STATE; /* offset+len 溢出：调用方参数错误 */

    const uint64_t fsize = inode->size;
    const uint64_t end_byte = offset + len;
    const uint64_t newsize = end_byte > fsize ? end_byte : fsize;

    /* 回滚账本：逆序 free（唯一失败出口前调用；位图 free 幂等） */
    auto rollback = [&]() {
        for (auto it = affect_extents.rbegin(); it != affect_extents.rend(); ++it)
            block_free(it->block_base.ptr, it->length);
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
                e.block_base.ptr = got;
                e.block_base.free_to_use = 0;
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
            e.block_base.ptr = got;
            e.block_base.free_to_use = 0;
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
            e.block_base.ptr = blk;
            e.block_base.free_to_use = 0;
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
        prev_pend = R.block_base.ptr + R.length;
        have_prev = true;
        cur = prev_end;
        if (cur >= d_end) return RC_OK;  /* 写域全被覆盖：零新段 */
        ++it;                            /* 前进到下一记录；树尾则 erro（循环内消化） */
    } else if (glb != EXTENT_LEAF_GLB_NONE) {
        /* 未命中但有 GLB（其终点 ≤ d_start）：作为左上下文；
         * 锚只在中段起点恰 == GLB 终点（d_start==prev_end）时成立，循环统一判定 */
        if (!it.current_extent(R)) return RC_ERR_STATE;
        prev_end = R.fblkbase + R.length;
        prev_pend = R.block_base.ptr + R.length;
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
        if (!is_extent_node(&lf->meta) || lf->meta.parent_ptr.is_inner_node) return false;
        if (lf->meta.parent_ptr.is_root_node) return true;   /* 单叶树 */
        return lf->meta.ptrs[2].ptr == PTR_INVALID_52;
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
            prev_pend = R.block_base.ptr + R.length;
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
                prev->block_base.ptr + prev->length == it->block_base.ptr) {
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
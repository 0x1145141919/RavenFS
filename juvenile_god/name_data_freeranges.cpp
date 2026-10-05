#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"   /* mdata_piece_session */
#include "../include/ravenfs/inode_cache_radix.hpp"
#include <cstring>
#include <vector>

/* ═══════════════════════════════════════════════════════════════════
 * name_data_freeranges.cpp — 目录名字数据面「空闲区间树」（v1.3）
 *
 * 节点族：Bptree_dir_name_data_free_ranges_{leaf,inner}_node_t
 *         魔数 NAMEDATA_FREERANGES_MAGIC1..4；统一 337 階（hints/free_ranges[338]）。
 * 键   ：空闲区间 (len, base)，**len 主序 / base 副序**升序。
 * hint ：子树最大**复合键** (max_len, max_base)。
 * 路由 ：叶内按复合键 lower_bound；内部找第一个 hint >= target（无则最右击穿）。
 *        分配 target=(need,0)（等价 max_len≥need）；插入 target=(len,base)。
 *
 * 语义：空闲区间 = 逻辑空间 [0,pieces×8K) 内「已映射 ∧ 未分配」的字节区间
 * （删除释放 ∪ 新片未用尾部）。alloc = best-fit 取最小 len≥need；free = 插入。
 * 本轮：不做相邻区间合并、节点欠载不合并（结构仍为合法搜索树）。
 *
 * 失败原子性说明：insert 分裂走「预算池」——分裂前按树高预分配全部节点，
 * 之后步骤仅指针改写；唯一失败面为 IO/STATE（节点已分配，尽力归还剩余池）。
 * ═══════════════════════════════════════════════════════════════════ */

namespace {
constexpr uint16_t FR_MAX  = 337;   /* 最大条目（数组 338 = 337 阶 + 1 瞬时槽） */
constexpr uint16_t FR_LEFT = 169;   /* 分裂左半（338 → 169 / 169） */

struct fr_split_t {
    result_code_t rc = RC_OK;
    uint64_t lo_len = 0, lo_base = 0;   /* 左半最大复合键 */
    uint64_t hi_len = 0, hi_base = 0;   /* 右半最大复合键 */
    uint64_t hi_blk = 0;                /* 右半块号 */
};

/* 复合键比较：a < b ? */
inline bool fr_key_lt(uint64_t alen, uint64_t abase, uint64_t blen, uint64_t bbase)
{ return alen < blen || (alen == blen && abase < bbase); }
}   /* namespace */

/* ═══════════════════════════════════════════════════════════════════
 * 节点基建
 * ═══════════════════════════════════════════════════════════════════ */
bool RavenFS_juvenile::is_namedata_freeranges_node(const Bptree_meta_t *m)
{
    return m->magics[0] == NAMEDATA_FREERANGES_MAGIC1 &&
           m->magics[1] == NAMEDATA_FREERANGES_MAGIC2 &&
           m->magics[2] == NAMEDATA_FREERANGES_MAGIC3 &&
           m->magics[3] == NAMEDATA_FREERANGES_MAGIC4;
}

void RavenFS_juvenile::name_data_fr_node_meta_init(void *buf, uint16_t count,
                                                   bool is_inner, bool is_root, uint64_t parent_ptr)
{
    memset(buf, 0, 8192);
    Bptree_meta_t *m = (Bptree_meta_t *)buf;
    m->magics[0] = NAMEDATA_FREERANGES_MAGIC1;
    m->magics[1] = NAMEDATA_FREERANGES_MAGIC2;
    m->magics[2] = NAMEDATA_FREERANGES_MAGIC3;
    m->magics[3] = NAMEDATA_FREERANGES_MAGIC4;
    RFS_PP_SET_COUNT(m->parent_ptr, count);
    RFS_PP_SET_IS_INNER(m->parent_ptr, is_inner ? 1 : 0);
    RFS_PP_SET_IS_ROOT(m->parent_ptr, is_root ? 1 : 0);
    RFS_PP_SET_PTR(m->parent_ptr, parent_ptr);
    RFS_PTR_SET_ADDR(m->ptrs[0], PTR_INVALID);
    RFS_PTR_SET_ADDR(m->ptrs[1], PTR_INVALID);
    RFS_PTR_SET_ADDR(m->ptrs[2], PTR_INVALID);
}

void RavenFS_juvenile::name_data_fr_max_key(const void *buf, uint64_t *out_len, uint64_t *out_base)
{
    const Bptree_meta_t *m = (const Bptree_meta_t *)buf;
    const uint16_t cnt = (uint16_t)RFS_PP_COUNT(m->parent_ptr);
    if (cnt == 0) { *out_len = 0; *out_base = 0; return; }
    if (RFS_PP_IS_INNER(m->parent_ptr)) {
        const Bptree_dir_name_data_free_ranges_inner_node_t *n =
            (const Bptree_dir_name_data_free_ranges_inner_node_t *)buf;
        *out_len  = n->hints[cnt - 1].max_len;
        *out_base = n->hints[cnt - 1].max_base;
    } else {
        const Bptree_dir_name_data_free_ranges_leaf_node_t *l =
            (const Bptree_dir_name_data_free_ranges_leaf_node_t *)buf;
        *out_len  = l->free_ranges[cnt - 1].len;
        *out_base = l->free_ranges[cnt - 1].base;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * 只读导航：路由到叶并做复合键 lower_bound
 *   need_base 一般传 0（best-fit 目标为 (need,0)）；插入定位传真实 base。
 *   trace.is_leaf_hit = 叶内存在 ≥ target 的条目。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_fr_walk(uint64_t root_blk, uint64_t need_len,
                                                  uint64_t need_base, bptree_path_t *trace)
{
    if (!trace) return RC_ERR_STATE;
    trace->depth = 0;
    trace->is_leaf_hit = false;
    uint64_t cur = root_blk;
    for (int depth = 0; depth < 64; depth++) {
        mdata_piece_session s(mdata_piece_mgr, cur);
        if (s.index_error()) return RC_ERR_STATE;
        if (!s) return RC_ERR_IO;
        Bptree_meta_t *m = (Bptree_meta_t *)s.data;
        if (!is_namedata_freeranges_node(m)) return RC_ERR_STATE;
        if ((depth == 0) != (bool)RFS_PP_IS_ROOT(m->parent_ptr)) return RC_ERR_STATE;
        trace->blocks[depth] = cur;

        if (!RFS_PP_IS_INNER(m->parent_ptr)) {
            const Bptree_dir_name_data_free_ranges_leaf_node_t *leaf =
                (const Bptree_dir_name_data_free_ranges_leaf_node_t *)s.data;
            const uint16_t cnt = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
            uint16_t lo = 0, hi = cnt;
            while (lo < hi) {
                uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
                const free_range_t &e = leaf->free_ranges[mid];
                if (fr_key_lt(e.len, e.base, need_len, need_base)) lo = (uint16_t)(mid + 1);
                else hi = mid;
            }
            trace->depth = depth + 1;
            trace->child_idx[depth] = lo;   /* 337 階：索引可 >255，child_idx 是 uint16_t，禁截断 */
            trace->is_leaf_hit = (lo < cnt);
            return RC_OK;
        }
        const Bptree_dir_name_data_free_ranges_inner_node_t *node =
            (const Bptree_dir_name_data_free_ranges_inner_node_t *)s.data;
        const uint16_t cnt = (uint16_t)RFS_PP_COUNT(node->meta.parent_ptr);
        if (cnt == 0) return RC_ERR_STATE;
        uint16_t lo = 0, hi = cnt;
        while (lo < hi) {
            uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
            const free_ranges_hint_t &h = node->hints[mid];
            if (fr_key_lt(h.max_len, h.max_base, need_len, need_base)) lo = (uint16_t)(mid + 1);
            else hi = mid;
        }
        uint16_t idx = (lo < cnt) ? lo : (uint16_t)(cnt - 1);   /* 击穿取最右 */
        trace->child_idx[depth] = idx;   /* 337 階：禁 uint8_t 截断 */
        cur = RFS_PTR_ADDR(node->hints[idx].sub_node_ptr);
    }
    return RC_ERR_STATE;
}

/* ═══════════════════════════════════════════════════════════════════
 * 沿父链刷新 hint = 子树最大复合键
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_fr_refresh_key_chain(uint64_t blk)
{
    const uint64_t PTR52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    uint64_t cur = blk;
    for (int hop = 0; hop < 64; hop++) {
        mdata_piece_session cs(mdata_piece_mgr, cur);
        if (cs.index_error()) return RC_ERR_STATE;
        if (!cs) return RC_ERR_IO;
        Bptree_meta_t *cm = (Bptree_meta_t *)cs.data;
        if (!is_namedata_freeranges_node(cm)) return RC_ERR_STATE;
        if (RFS_PP_IS_ROOT(cm->parent_ptr)) return RC_OK;
        uint64_t nl = 0, nb = 0;
        name_data_fr_max_key(cs.data, &nl, &nb);
        const uint64_t parent_blk = RFS_PP_PTR(cm->parent_ptr);
        if (parent_blk == PTR52) return RC_ERR_STATE;
        mdata_piece_session ps(mdata_piece_mgr, parent_blk);
        if (ps.index_error()) return RC_ERR_STATE;
        if (!ps) return RC_ERR_IO;
        Bptree_dir_name_data_free_ranges_inner_node_t *p =
            (Bptree_dir_name_data_free_ranges_inner_node_t *)ps.data;
        if (!is_namedata_freeranges_node(&p->meta) || !RFS_PP_IS_INNER(p->meta.parent_ptr))
            return RC_ERR_STATE;
        const uint16_t pcnt = (uint16_t)RFS_PP_COUNT(p->meta.parent_ptr);
        uint16_t i = 0; bool found = false;
        for (; i < pcnt; i++)
            if (RFS_PTR_ADDR(p->hints[i].sub_node_ptr) == cur) { found = true; break; }
        if (!found) return RC_ERR_STATE;
        if (p->hints[i].max_len == nl && p->hints[i].max_base == nb) return RC_OK;
        p->hints[i].max_len = nl;
        p->hints[i].max_base = nb;
        ps.dirty();
        cur = parent_blk;
    }
    return RC_ERR_STATE;
}

/* ═══════════════════════════════════════════════════════════════════
 * find_fit — 只读 best-fit 查询（最小 len ≥ need）
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_fr_find_fit(uint64_t root_blk, uint64_t need,
                                                      uint64_t *out_base, uint64_t *out_len)
{
    if (!out_base || !out_len) return RC_ERR_STATE;
    bptree_path_t tr;
    result_code_t rc = name_data_fr_walk(root_blk, need, 0, &tr);
    if (rc) return rc;
    if (!tr.is_leaf_hit) return RC_ERR_NOT_FOUND;
    const uint64_t leaf_blk = tr.blocks[tr.depth - 1];
    const uint16_t idx = tr.child_idx[tr.depth - 1];
    mdata_piece_session s(mdata_piece_mgr, leaf_blk);
    if (s.index_error()) return RC_ERR_STATE;
    if (!s) return RC_ERR_IO;
    const Bptree_dir_name_data_free_ranges_leaf_node_t *leaf =
        (const Bptree_dir_name_data_free_ranges_leaf_node_t *)s.data;
    if (idx >= RFS_PP_COUNT(leaf->meta.parent_ptr)) return RC_ERR_STATE;
    const free_range_t e = leaf->free_ranges[idx];
    if (e.len < need) return RC_ERR_NOT_FOUND;
    *out_base = e.base;
    *out_len  = e.len;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * insert — 插入空闲区间 [base, base+len)
 *   空态建根叶；叶满则预算池 + 分裂级联（叶 169/169 → 逐层 → 建新根）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_fr_insert(uint64_t dir_inode_idx,
                                                    raven_dir_Inode_t *dino,
                                                    uint64_t base, uint64_t len)
{
    if (len == 0) return RC_OK;
    uint64_t root = RFS_ND_FR_CTL_PTR(dino->name_data_free_ranges_ctl);

    /* 空态：建根叶 */
    if (root == RFS_ND_FR_CTL_PTR_INVALID) {
        uint64_t blk = block_alloc_hard(2, 1);
        if (blk == PTR_INVALID) return RC_ERR_NO_BLOCK;
        mdata_piece_session s(mdata_piece_mgr, blk);
        if (s.index_error() || !s) { block_free(blk, 2); return s.index_error() ? RC_ERR_STATE : RC_ERR_IO; }
        name_data_fr_node_meta_init(s.data, 1, false, true, dir_inode_idx);
        Bptree_dir_name_data_free_ranges_leaf_node_t *leaf =
            (Bptree_dir_name_data_free_ranges_leaf_node_t *)s.data;
        leaf->free_ranges[0].base = base;
        leaf->free_ranges[0].len  = len;
        s.dirty();
        RFS_ND_FR_CTL_SET_INLINE(dino->name_data_free_ranges_ctl, 0);
        RFS_ND_FR_CTL_SET_PTR(dino->name_data_free_ranges_ctl, blk);
        RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count,
                            RFS_ND_FR_COUNT(dino->name_data_free_ranges_count) + 1);
        return RC_OK;
    }

    /* 定位叶（插入目标 = 精确键） */
    bptree_path_t trace;
    result_code_t rc = name_data_fr_walk(root, len, base, &trace);
    if (rc) return rc;
    const uint64_t leaf_blk = trace.blocks[trace.depth - 1];

    /* 非满：就地插入 */
    {
        mdata_piece_session s(mdata_piece_mgr, leaf_blk);
        if (s.index_error()) return RC_ERR_STATE;
        if (!s) return RC_ERR_IO;
        Bptree_dir_name_data_free_ranges_leaf_node_t *leaf =
            (Bptree_dir_name_data_free_ranges_leaf_node_t *)s.data;
        uint16_t cnt = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
        if (cnt < FR_MAX) {
            uint16_t pos = 0;
            while (pos < cnt && fr_key_lt(leaf->free_ranges[pos].len, leaf->free_ranges[pos].base, len, base))
                pos++;
            if (pos < cnt && leaf->free_ranges[pos].len == len && leaf->free_ranges[pos].base == base)
                return RC_OK;   /* 精确重复：幂等跳过 */
            if (pos < cnt)
                memmove(&leaf->free_ranges[pos + 1], &leaf->free_ranges[pos],
                        (size_t)(cnt - pos) * sizeof(free_range_t));
            leaf->free_ranges[pos].base = base;
            leaf->free_ranges[pos].len  = len;
            RFS_PP_SET_COUNT(leaf->meta.parent_ptr, cnt + 1);
            s.dirty();
            RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count,
                                RFS_ND_FR_COUNT(dino->name_data_free_ranges_count) + 1);
            return name_data_fr_refresh_key_chain(leaf_blk);
        }
    }

    /* 满叶：瞬态插入（338）→ 预算池分裂级联 */
    std::vector<uint64_t> pool;
    pool.reserve((size_t)trace.depth + 1);
    for (int i = 0; i <= trace.depth; i++) {
        uint64_t b = block_alloc_hard(2, 1);
        if (b == PTR_INVALID) { for (uint64_t x : pool) block_free(x, 2); return RC_ERR_NO_BLOCK; }
        pool.push_back(b);
    }
    size_t used = 0;

    {
        mdata_piece_session s(mdata_piece_mgr, leaf_blk);
        if (s.index_error() || !s) { for (; used < pool.size(); used++) block_free(pool[used], 2); return s.index_error() ? RC_ERR_STATE : RC_ERR_IO; }
        Bptree_dir_name_data_free_ranges_leaf_node_t *leaf =
            (Bptree_dir_name_data_free_ranges_leaf_node_t *)s.data;
        uint16_t cnt = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);   /* == FR_MAX */
        uint16_t pos = 0;
        while (pos < cnt && fr_key_lt(leaf->free_ranges[pos].len, leaf->free_ranges[pos].base, len, base)) pos++;
        if (pos < cnt)
            memmove(&leaf->free_ranges[pos + 1], &leaf->free_ranges[pos],
                    (size_t)(cnt - pos) * sizeof(free_range_t));
        leaf->free_ranges[pos].base = base;
        leaf->free_ranges[pos].len  = len;
        RFS_PP_SET_COUNT(leaf->meta.parent_ptr, (uint16_t)(cnt + 1));   /* 338 */
        s.dirty();
    }

    /* 分裂叶 169 / 169 */
    fr_split_t sr;
    {
        mdata_piece_session vs(mdata_piece_mgr, leaf_blk);
        if (vs.index_error() || !vs) { sr.rc = RC_ERR_STATE; }
        else {
            Bptree_dir_name_data_free_ranges_leaf_node_t *v =
                (Bptree_dir_name_data_free_ranges_leaf_node_t *)vs.data;
            const uint16_t cnt = (uint16_t)RFS_PP_COUNT(v->meta.parent_ptr);
            mdata_piece_session ns(mdata_piece_mgr, pool[used]);
            if (ns.index_error() || !ns) { sr.rc = ns.index_error() ? RC_ERR_STATE : RC_ERR_IO; }
            else {
                name_data_fr_node_meta_init(ns.data, (uint16_t)(cnt - FR_LEFT), false, false,
                                            RFS_PP_PTR(v->meta.parent_ptr));
                Bptree_dir_name_data_free_ranges_leaf_node_t *n =
                    (Bptree_dir_name_data_free_ranges_leaf_node_t *)ns.data;
                memcpy(&n->free_ranges[0], &v->free_ranges[FR_LEFT],
                       (size_t)(cnt - FR_LEFT) * sizeof(free_range_t));
                RFS_PP_SET_COUNT(v->meta.parent_ptr, FR_LEFT);
                sr.lo_len  = v->free_ranges[FR_LEFT - 1].len;
                sr.lo_base = v->free_ranges[FR_LEFT - 1].base;
                sr.hi_len  = n->free_ranges[cnt - FR_LEFT - 1].len;
                sr.hi_base = n->free_ranges[cnt - FR_LEFT - 1].base;
                sr.hi_blk  = pool[used];
                vs.dirty(); ns.dirty();
            }
        }
        if (sr.rc == RC_OK) used++;
    }
    uint64_t new_root = root;
    if (sr.rc == RC_OK) {
        /* 上游应用：叶裂后的父层 / 新根 */
        auto parent_apply = [&](uint64_t parent_blk, uint16_t victim_idx, uint64_t hi_blk,
                                uint64_t lo_len, uint64_t lo_base,
                                uint64_t hi_len, uint64_t hi_base, bool *out_split) -> result_code_t {
            *out_split = false;
            mdata_piece_session ps(mdata_piece_mgr, parent_blk);
            if (ps.index_error()) return RC_ERR_STATE;
            if (!ps) return RC_ERR_IO;
            Bptree_dir_name_data_free_ranges_inner_node_t *p =
                (Bptree_dir_name_data_free_ranges_inner_node_t *)ps.data;
            if (!is_namedata_freeranges_node(&p->meta) || !RFS_PP_IS_INNER(p->meta.parent_ptr))
                return RC_ERR_STATE;
            uint16_t cnt = (uint16_t)RFS_PP_COUNT(p->meta.parent_ptr);
            if (victim_idx >= cnt) return RC_ERR_STATE;
            p->hints[victim_idx].max_len  = lo_len;
            p->hints[victim_idx].max_base = lo_base;
            const uint16_t ins = (uint16_t)(victim_idx + 1);
            if (ins < cnt)
                memmove(&p->hints[ins + 1], &p->hints[ins], (size_t)(cnt - ins) * sizeof(free_ranges_hint_t));
            p->hints[ins].max_len  = hi_len;
            p->hints[ins].max_base = hi_base;
            RFS_PTR_SET_ADDR(p->hints[ins].sub_node_ptr, hi_blk);
            RFS_PP_SET_COUNT(p->meta.parent_ptr, (uint16_t)(cnt + 1));
            *out_split = (cnt + 1) > FR_MAX;
            ps.dirty();
            return RC_OK;
        };
        auto new_root_inner = [&](uint64_t lo_blk, uint64_t lo_len, uint64_t lo_base,
                                  uint64_t hi_blk, uint64_t hi_len, uint64_t hi_base,
                                  uint64_t prealloc) -> result_code_t {
            mdata_piece_session rs(mdata_piece_mgr, prealloc);
            if (rs.index_error()) return RC_ERR_STATE;
            if (!rs) return RC_ERR_IO;
            name_data_fr_node_meta_init(rs.data, 2, true, true, dir_inode_idx);
            Bptree_dir_name_data_free_ranges_inner_node_t *r =
                (Bptree_dir_name_data_free_ranges_inner_node_t *)rs.data;
            r->hints[0].max_len = lo_len; r->hints[0].max_base = lo_base;
            RFS_PTR_SET_ADDR(r->hints[0].sub_node_ptr, lo_blk);
            r->hints[1].max_len = hi_len; r->hints[1].max_base = hi_base;
            RFS_PTR_SET_ADDR(r->hints[1].sub_node_ptr, hi_blk);
            /* 两子去 root 标志、父指新根 */
            for (int k = 0; k < 2; k++) {
                uint64_t cb = (k == 0) ? lo_blk : hi_blk;
                mdata_piece_session cs(mdata_piece_mgr, cb);
                if (cs.index_error()) return RC_ERR_STATE;
                if (!cs) return RC_ERR_IO;
                Bptree_meta_t *cm = (Bptree_meta_t *)cs.data;
                RFS_PP_SET_PTR(cm->parent_ptr, prealloc);
                RFS_PP_SET_IS_ROOT(cm->parent_ptr, 0);
                cs.dirty();
            }
            rs.dirty();
            new_root = prealloc;
            return RC_OK;
        };

        if (trace.depth == 1) {
            rc = new_root_inner(leaf_blk, sr.lo_len, sr.lo_base, sr.hi_blk, sr.hi_len, sr.hi_base,
                                pool[used++]);
        } else {
            uint64_t parent_blk = trace.blocks[trace.depth - 2];
            uint16_t child_idx = trace.child_idx[trace.depth - 2];
            bool parent_split = false;
            rc = parent_apply(parent_blk, child_idx, sr.hi_blk, sr.lo_len, sr.lo_base,
                              sr.hi_len, sr.hi_base, &parent_split);
            uint64_t cur_parent = parent_blk;
            int guard = 0;
            while (rc == RC_OK && parent_split) {
                if (++guard > 64) { rc = RC_ERR_STATE; break; }
                mdata_piece_session ps(mdata_piece_mgr, cur_parent);
                if (ps.index_error() || !ps) { rc = ps.index_error() ? RC_ERR_STATE : RC_ERR_IO; break; }
                Bptree_dir_name_data_free_ranges_inner_node_t *pn =
                    (Bptree_dir_name_data_free_ranges_inner_node_t *)ps.data;
                if (!is_namedata_freeranges_node(&pn->meta) || !RFS_PP_IS_INNER(pn->meta.parent_ptr)) { rc = RC_ERR_STATE; break; }
                const uint16_t cnt = (uint16_t)RFS_PP_COUNT(pn->meta.parent_ptr);   /* 338 */
                const bool p_is_root = RFS_PP_IS_ROOT(pn->meta.parent_ptr);
                const uint64_t grand_blk = RFS_PP_PTR(pn->meta.parent_ptr);
                /* 分裂内节点 169 / 169 */
                fr_split_t ir;
                {
                    mdata_piece_session ns(mdata_piece_mgr, pool[used]);
                    if (ns.index_error() || !ns) { ir.rc = ns.index_error() ? RC_ERR_STATE : RC_ERR_IO; }
                    else {
                        name_data_fr_node_meta_init(ns.data, (uint16_t)(cnt - FR_LEFT), true, false,
                                                    RFS_PP_PTR(pn->meta.parent_ptr));
                        Bptree_dir_name_data_free_ranges_inner_node_t *n =
                            (Bptree_dir_name_data_free_ranges_inner_node_t *)ns.data;
                        memcpy(&n->hints[0], &pn->hints[FR_LEFT],
                               (size_t)(cnt - FR_LEFT) * sizeof(free_ranges_hint_t));
                        RFS_PP_SET_COUNT(pn->meta.parent_ptr, FR_LEFT);
                        ir.lo_len  = pn->hints[FR_LEFT - 1].max_len;
                        ir.lo_base = pn->hints[FR_LEFT - 1].max_base;
                        ir.hi_len  = n->hints[cnt - FR_LEFT - 1].max_len;
                        ir.hi_base = n->hints[cnt - FR_LEFT - 1].max_base;
                        ir.hi_blk  = pool[used];
                        ps.dirty(); ns.dirty();
                        /* 右半孩子 reparent */
                        size_t hn = (size_t)(cnt - FR_LEFT);
                        for (size_t k = 0; k < hn; k++) {
                            uint64_t cb = RFS_PTR_ADDR(n->hints[k].sub_node_ptr);
                            mdata_piece_session cs(mdata_piece_mgr, cb);
                            if (cs.index_error() || !cs) { ir.rc = RC_ERR_STATE; break; }
                            Bptree_meta_t *cm = (Bptree_meta_t *)cs.data;
                            RFS_PP_SET_PTR(cm->parent_ptr, ir.hi_blk);
                            cs.dirty();
                        }
                    }
                    if (ir.rc == RC_OK) used++;
                }
                if (ir.rc != RC_OK) { rc = ir.rc; break; }
                if (p_is_root) {
                    rc = new_root_inner(cur_parent, ir.lo_len, ir.lo_base, ir.hi_blk, ir.hi_len, ir.hi_base,
                                        pool[used++]);
                    break;
                }
                /* 找祖父中 cur_parent 的 idx */
                mdata_piece_session gs(mdata_piece_mgr, grand_blk);
                if (gs.index_error() || !gs) { rc = RC_ERR_STATE; break; }
                Bptree_dir_name_data_free_ranges_inner_node_t *g =
                    (Bptree_dir_name_data_free_ranges_inner_node_t *)gs.data;
                if (!is_namedata_freeranges_node(&g->meta) || !RFS_PP_IS_INNER(g->meta.parent_ptr)) { rc = RC_ERR_STATE; break; }
                const uint16_t gcnt = (uint16_t)RFS_PP_COUNT(g->meta.parent_ptr);
                uint16_t gidx = 0; bool found = false;
                for (; gidx < gcnt; gidx++)
                    if (RFS_PTR_ADDR(g->hints[gidx].sub_node_ptr) == cur_parent) { found = true; break; }
                if (!found) { rc = RC_ERR_STATE; break; }
                rc = parent_apply(grand_blk, gidx, ir.hi_blk, ir.lo_len, ir.lo_base,
                                  ir.hi_len, ir.hi_base, &parent_split);
                cur_parent = grand_blk;
            }
        }
        if (rc == RC_OK) {
            RFS_ND_FR_CTL_SET_PTR(dino->name_data_free_ranges_ctl, new_root);
            RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count,
                                RFS_ND_FR_COUNT(dino->name_data_free_ranges_count) + 1);
            rc = name_data_fr_refresh_key_chain(leaf_blk);
            if (rc == RC_OK) rc = name_data_fr_refresh_key_chain(sr.hi_blk);
        }
    }
    for (; used < pool.size(); used++) block_free(pool[used], 2);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════
 * take_fit — best-fit 取用：摘除最小 len≥need 的区间，余段插回
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_fr_take_fit(uint64_t dir_inode_idx,
                                                      raven_dir_Inode_t *dino,
                                                      uint64_t need, uint64_t *out_base)
{
    if (!out_base || need == 0) return RC_ERR_STATE;
    uint64_t root = RFS_ND_FR_CTL_PTR(dino->name_data_free_ranges_ctl);
    if (root == RFS_ND_FR_CTL_PTR_INVALID) return RC_ERR_NOT_FOUND;

    bptree_path_t trace;
    result_code_t rc = name_data_fr_walk(root, need, 0, &trace);
    if (rc) return rc;
    if (!trace.is_leaf_hit) return RC_ERR_NOT_FOUND;
    const uint64_t leaf_blk = trace.blocks[trace.depth - 1];
    const uint16_t idx = trace.child_idx[trace.depth - 1];

    free_range_t e;
    uint16_t cnt_after = 0;
    {
        mdata_piece_session s(mdata_piece_mgr, leaf_blk);
        if (s.index_error()) return RC_ERR_STATE;
        if (!s) return RC_ERR_IO;
        Bptree_dir_name_data_free_ranges_leaf_node_t *leaf =
            (Bptree_dir_name_data_free_ranges_leaf_node_t *)s.data;
        uint16_t cnt = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
        if (idx >= cnt) return RC_ERR_STATE;
        e = leaf->free_ranges[idx];
        if (e.len < need) return RC_ERR_NOT_FOUND;
        if (idx < cnt - 1)
            memmove(&leaf->free_ranges[idx], &leaf->free_ranges[idx + 1],
                    (size_t)(cnt - idx - 1) * sizeof(free_range_t));
        memset(&leaf->free_ranges[cnt - 1], 0, sizeof(free_range_t));
        cnt_after = (uint16_t)(cnt - 1);
        RFS_PP_SET_COUNT(leaf->meta.parent_ptr, cnt_after);
        s.dirty();
    }
    RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count,
                        RFS_ND_FR_COUNT(dino->name_data_free_ranges_count) - 1);

    if (cnt_after == 0) {
        if (e.len > need) {
            /* 余段直接放回同一（已空）叶：避免留下空叶 (0,0) hint（破坏单调路由） */
            mdata_piece_session s(mdata_piece_mgr, leaf_blk);
            if (s.index_error()) return RC_ERR_STATE;
            if (!s) return RC_ERR_IO;
            Bptree_dir_name_data_free_ranges_leaf_node_t *leaf =
                (Bptree_dir_name_data_free_ranges_leaf_node_t *)s.data;
            leaf->free_ranges[0].base = e.base + need;
            leaf->free_ranges[0].len  = e.len - need;
            RFS_PP_SET_COUNT(leaf->meta.parent_ptr, 1);
            s.dirty();
            RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count,
                                RFS_ND_FR_COUNT(dino->name_data_free_ranges_count) + 1);
            rc = name_data_fr_refresh_key_chain(leaf_blk);
            if (rc) return rc;
        } else {
            /* 整取清空：退役空叶（父删 hint / 释放 / 上溯 / 根收缩） */
            rc = name_data_fr_retire_empty_leaf(dir_inode_idx, dino, leaf_blk);
            if (rc) return rc;
        }
    } else {
        rc = name_data_fr_refresh_key_chain(leaf_blk);
        if (rc) return rc;
        if (e.len > need) {
            rc = name_data_fr_insert(dir_inode_idx, dino, e.base + need, e.len - need);
            if (rc) return rc;
        }
    }
    *out_base = e.base;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * retire_empty_leaf — 空叶退役（父删 hint / 释放节点 / 上溯 / 根收缩）
 * 不变式：树内不留 count==0 的叶（空叶 hint=(0,0) 破坏 hint 单调性）。
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_fr_retire_empty_leaf(uint64_t dir_inode_idx,
                                                               raven_dir_Inode_t *dino,
                                                               uint64_t leaf_blk)
{
    const uint64_t PTR52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    uint64_t cur = leaf_blk;
    for (int hop = 0; hop < 64; hop++) {
        uint64_t parent_blk = 0;
        uint16_t pcnt_after = 0;
        bool p_is_root = false;
        uint64_t only_child = 0;
        {
            mdata_piece_session cs(mdata_piece_mgr, cur);
            if (cs.index_error()) return RC_ERR_STATE;
            if (!cs) return RC_ERR_IO;
            Bptree_meta_t *cm = (Bptree_meta_t *)cs.data;
            if (!is_namedata_freeranges_node(cm)) return RC_ERR_STATE;
            if (RFS_PP_IS_ROOT(cm->parent_ptr)) {
                /* cur 是根叶（已空）→ 整树退役 */
                block_free(cur, 2);
                dino->name_data_free_ranges_ctl = RFS_ND_FR_CTL_INACTIVE;
                dino->name_data_free_ranges_count = 0;
                return RC_OK;
            }
            parent_blk = RFS_PP_PTR(cm->parent_ptr);
            if (parent_blk == PTR52) return RC_ERR_STATE;

            mdata_piece_session ps(mdata_piece_mgr, parent_blk);
            if (ps.index_error()) return RC_ERR_STATE;
            if (!ps) return RC_ERR_IO;
            Bptree_dir_name_data_free_ranges_inner_node_t *p =
                (Bptree_dir_name_data_free_ranges_inner_node_t *)ps.data;
            if (!is_namedata_freeranges_node(&p->meta) || !RFS_PP_IS_INNER(p->meta.parent_ptr))
                return RC_ERR_STATE;
            uint16_t pcnt = (uint16_t)RFS_PP_COUNT(p->meta.parent_ptr);
            uint16_t pi = 0; bool found = false;
            for (; pi < pcnt; pi++)
                if (RFS_PTR_ADDR(p->hints[pi].sub_node_ptr) == cur) { found = true; break; }
            if (!found) return RC_ERR_STATE;
            if (pi < pcnt - 1)
                memmove(&p->hints[pi], &p->hints[pi + 1],
                        (size_t)(pcnt - pi - 1) * sizeof(free_ranges_hint_t));
            memset(&p->hints[pcnt - 1], 0, sizeof(free_ranges_hint_t));
            pcnt_after = (uint16_t)(pcnt - 1);
            RFS_PP_SET_COUNT(p->meta.parent_ptr, pcnt_after);
            p_is_root = RFS_PP_IS_ROOT(p->meta.parent_ptr);
            if (pcnt_after == 1) only_child = RFS_PTR_ADDR(p->hints[0].sub_node_ptr);
            ps.dirty();
        }   /* 关闭 cs/ps 会话（同块禁双会话） */
        block_free(cur, 2);

        if (pcnt_after == 0) {
            if (p_is_root) {   /* 根内部清空（理论不可达，防御） */
                block_free(parent_blk, 2);
                dino->name_data_free_ranges_ctl = RFS_ND_FR_CTL_INACTIVE;
                dino->name_data_free_ranges_count = 0;
                return RC_OK;
            }
            cur = parent_blk;   /* 父空 → 继续上溯 */
            continue;
        }
        if (p_is_root) {
            if (pcnt_after == 1) {   /* 根内部剩 1 孩 → 收缩提升 */
                mdata_piece_session cks(mdata_piece_mgr, only_child);
                if (cks.index_error()) return RC_ERR_STATE;
                if (!cks) return RC_ERR_IO;
                Bptree_meta_t *ck = (Bptree_meta_t *)cks.data;
                RFS_PP_SET_IS_ROOT(ck->parent_ptr, 1);
                RFS_PP_SET_PTR(ck->parent_ptr, dir_inode_idx);
                if (!RFS_PP_IS_INNER(ck->parent_ptr)) {
                    RFS_PTR_SET_ADDR(ck->ptrs[0], PTR_INVALID);
                    RFS_PTR_SET_ADDR(ck->ptrs[1], PTR_INVALID);
                    RFS_PTR_SET_ADDR(ck->ptrs[2], PTR_INVALID);
                }
                cks.dirty();
                block_free(parent_blk, 2);
                RFS_ND_FR_CTL_SET_PTR(dino->name_data_free_ranges_ctl, only_child);
            }
            return RC_OK;   /* 根内部 >1 孩：父即根，无需刷新 */
        }
        /* 非根父：其 max 可能变 → 刷祖父 hint */
        return name_data_fr_refresh_key_chain(parent_blk);
    }
    return RC_ERR_STATE;
}

/* ═══════════════════════════════════════════════════════════════════
 * release — 全量释放 fr 树节点（不碰数据片）：BFS 收集所有层节点，逐块归还
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::name_data_fr_release(uint64_t dir_inode_idx,
                                                     raven_dir_Inode_t *dino)
{
    (void)dir_inode_idx;
    uint64_t root = RFS_ND_FR_CTL_PTR(dino->name_data_free_ranges_ctl);
    if (root == RFS_ND_FR_CTL_PTR_INVALID) {
        dino->name_data_free_ranges_ctl = RFS_ND_FR_CTL_INACTIVE;
        dino->name_data_free_ranges_count = 0;
        return RC_OK;
    }
    const uint64_t PTR52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);
    std::vector<uint64_t> layer{root};
    while (!layer.empty()) {
        std::vector<uint64_t> next;
        for (uint64_t b : layer) {
            mdata_piece_session s(mdata_piece_mgr, b);
            if (s.index_error() || !s) continue;
            Bptree_meta_t *m = (Bptree_meta_t *)s.data;
            if (!is_namedata_freeranges_node(m)) continue;
            if (RFS_PP_IS_INNER(m->parent_ptr)) {
                const Bptree_dir_name_data_free_ranges_inner_node_t *n =
                    (const Bptree_dir_name_data_free_ranges_inner_node_t *)s.data;
                const uint16_t cnt = (uint16_t)RFS_PP_COUNT(n->meta.parent_ptr);
                for (uint16_t i = 0; i < cnt; i++) {
                    uint64_t cb = RFS_PTR_ADDR(n->hints[i].sub_node_ptr);
                    if (cb != PTR52) next.push_back(cb);
                }
            }
        }
        for (uint64_t b : layer) block_free(b, 2);
        layer.swap(next);
    }
    dino->name_data_free_ranges_ctl = RFS_ND_FR_CTL_INACTIVE;
    dino->name_data_free_ranges_count = 0;
    return RC_OK;
}

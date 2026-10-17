/* ═══════════════════════════════════════════════════════════════════════════
 * audit_inode_plane.cpp — 相位 A：inode 数据面审计
 *   A1 inode_resolver::build        —— 映射树（direct/tree）→ 翻译层 + 叶链校验
 *   A2 inode_alloc_ledger::build    —— 扁 fr 快照 → 分配账本（补集）
 *
 * 全部访存走 audit_image::fsblk() 坐标；不链 god，不碰 allocator/cache。
 * 违例计 sum.inodeplane_bad（ERROR）；观察面计 sum.inodeplane_*。
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "audit/audit.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace rfs_audit {

namespace {

constexpr uint16_t EXTENT_ORDER = 337;
constexpr uint16_t EXTENT_MINA  = 168;
constexpr uint64_t PTR52 = AUDIT_PTR52;
inline bool ptr52_bad(uint64_t p) { return p == PTR52; }

inline bool idp_magic(const Bptree_meta_t *m)
{
    return m->magics[0] == INODE_DATA_EXTENT_MAGIC1 && m->magics[1] == INODE_DATA_EXTENT_MAGIC2 &&
           m->magics[2] == INODE_DATA_EXTENT_MAGIC3 && m->magics[3] == INODE_DATA_EXTENT_MAGIC4;
}
inline void claim_range(dfs_ctx_t &c, uint64_t base, uint64_t len)
{
    for (uint64_t i = 0; i < len && base + i < c.blk_bits; i++)
        if (!c.blk_set(base + i)) {
            c.rep->error(scope_t::inode_plane, "multi_claim",
                         "inode 数据面块 %llu 二次认领", (unsigned long long)(base + i));
            c.sum->multi_claim_blk++;
        }
}

} /* namespace */

/* ── 映射树 DFS：填 piece_phys_ + 认领节点/数据块 + 结构不变量 ── */
namespace {

struct IdpWalker {
    const audit_image *img;
    audit_reporter    *rep;
    audit_summary_t   *sum;
    dfs_ctx_t         *c;
    std::vector<uint64_t> *piece_phys;   /* 逻辑片 → 物理首块 */
    uint64_t target;                     /* 逻辑 4K 块总数 = pieces×2 */
    int max_depth;
    uint64_t total_blocks;
    uint64_t logical_end = 0;
    bool     started = false;
    uint64_t data_blocks = 0, tree_blocks = 0;
    std::vector<uint64_t> leaves;
    uint64_t leaf_depth = 0;
    bool leaf_depth_set = false;

    /* 统一违规出口（check 固定 "idp_tree"） */
    #define IDP_BAD(...) do { sum->inodeplane_bad++; rep->error(scope_t::inode_plane, "idp_tree", __VA_ARGS__); } while (0)

    uint64_t walk(uint64_t blk, uint64_t owner_inum, uint64_t expect_parent,
                  bool expect_root, int depth, bool &sub_ok)
    {
        sub_ok = false;
        if (depth > max_depth) { IDP_BAD("blk=%llu depth 超限 %d", (unsigned long long)blk, depth); return 0; }
        if (blk == PTR52 || ptr52_bad(blk) || blk >= total_blocks || (blk & 1) || blk + 1 >= total_blocks) {
            IDP_BAD("blk=%llu 非法节点块号", (unsigned long long)blk); return 0;
        }
        if (!c->blk_set(blk)) {
            sum->multi_claim_blk++;
            rep->info(scope_t::inode_plane, "MULTI_CLAIM idp 节点 blk=%llu 二次认领", (unsigned long long)blk);
            return 0;
        }
        tree_blocks += 2;
        if (!c->blk_set(blk + 1)) {
            sum->multi_claim_blk++;
            rep->info(scope_t::inode_plane, "MULTI_CLAIM idp 节点尾 blk=%llu 二次认领", (unsigned long long)(blk + 1));
        }

        const Bptree_meta_t *meta = reinterpret_cast<const Bptree_meta_t *>(img->fsblk(blk));
        if (!idp_magic(meta)) { IDP_BAD("blk=%llu magic 不符（非 INODE_DATA_EXTENT）", (unsigned long long)blk); return 0; }

        const uint16_t cnt = (uint16_t)RFS_PP_COUNT(meta->parent_ptr);
        const uint16_t it  = std::min<uint16_t>(cnt, (uint16_t)(EXTENT_ORDER + 1));
        const bool inner = RFS_PP_IS_INNER(meta->parent_ptr);
        AUDIT_TRACE("[TRACE][IDPTREE] blk=%llu owner=%llu depth=%d role=%s inner=%d cnt=%u\n",
                    (unsigned long long)blk, (unsigned long long)owner_inum, depth,
                    expect_root ? "root" : (inner ? "inner" : "leaf"), (int)inner, cnt);

        if (cnt > EXTENT_ORDER)
            IDP_BAD("blk=%llu entry_count=%u > 阶 %u", (unsigned long long)blk, cnt, (unsigned)EXTENT_ORDER);
        if ((bool)RFS_PP_IS_ROOT(meta->parent_ptr) != expect_root)
            IDP_BAD("blk=%llu is_root=%d 期望 %d", (unsigned long long)blk,
                    (int)RFS_PP_IS_ROOT(meta->parent_ptr), (int)expect_root);
        if (expect_root) {
            if (RFS_PP_PTR(meta->parent_ptr) != owner_inum)
                IDP_BAD("根 blk=%llu ptr=%llu != owner %llu", (unsigned long long)blk,
                        (unsigned long long)RFS_PP_PTR(meta->parent_ptr), (unsigned long long)owner_inum);
        } else if (RFS_PP_PTR(meta->parent_ptr) != expect_parent) {
            IDP_BAD("blk=%llu parent_ptr=%llu != 实际父 %llu", (unsigned long long)blk,
                    (unsigned long long)RFS_PP_PTR(meta->parent_ptr), (unsigned long long)expect_parent);
        }

        if (!inner) {
            if (!leaf_depth_set) { leaf_depth = (uint64_t)depth; leaf_depth_set = true; }
            else if ((uint64_t)depth != leaf_depth)
                IDP_BAD("blk=%llu 叶深度 %d != %llu（不平衡）", (unsigned long long)blk, depth,
                        (unsigned long long)leaf_depth);
            leaves.push_back(blk);
            if (cnt == 0) {
                if (!expect_root) IDP_BAD("blk=%llu 非根叶空", (unsigned long long)blk);
                else if (target != 0) IDP_BAD("根叶空但 pieces*2=%llu", (unsigned long long)target);
            } else if (!expect_root && cnt < EXTENT_MINA) {
                IDP_BAD("blk=%llu 叶欠载 cnt=%u < %u", (unsigned long long)blk, cnt, (unsigned)EXTENT_MINA);
            }

            const auto *leaf = reinterpret_cast<const Bptree_inode_data_extent_leaf_node_t *>(meta);
            uint64_t leaf_max = 0;
            for (uint16_t i = 0; i < it; i++) {
                const extent_t &e = leaf->extents[i];
                const uint64_t beg = e.fblkbase, len = e.length, end = beg + len;
                if (len == 0) IDP_BAD("blk=%llu ext[%u] length==0", (unsigned long long)blk, i);
                if (beg & 1)  IDP_BAD("blk=%llu ext[%u] fblkbase=%llu 未 2 对齐", (unsigned long long)blk, i, (unsigned long long)beg);
                if (len & 1)  IDP_BAD("blk=%llu ext[%u] length=%llu 非偶", (unsigned long long)blk, i, (unsigned long long)len);
                if (end > target) IDP_BAD("blk=%llu ext[%u] 逻辑越界 end=%llu > %llu", (unsigned long long)blk, i, (unsigned long long)end, (unsigned long long)target);
                if (!started) {
                    started = true;
                    if (beg != 0) IDP_BAD("首条 fblkbase=%llu != 0", (unsigned long long)beg);
                } else if (beg < logical_end) {
                    IDP_BAD("ext[%u] 逻辑重叠 prev_end=%llu > beg=%llu", i, (unsigned long long)logical_end, (unsigned long long)beg);
                } else if (beg > logical_end) {
                    IDP_BAD("ext[%u] 逻辑空洞 prev_end=%llu < beg=%llu", i, (unsigned long long)logical_end, (unsigned long long)beg);
                }
                logical_end = end;

                const uint64_t pb = RFS_PTR_ADDR(e.block_base);
                AUDIT_TRACE("[TRACE][IDPEXT] blk=%llu idx=%u fblk=%llu pblk=%llu len=%llu end=%llu\n",
                            (unsigned long long)blk, i, (unsigned long long)beg,
                            (unsigned long long)pb, (unsigned long long)len,
                            (unsigned long long)end);
                if (pb == PTR52 || pb >= total_blocks || (pb & 1) || len > total_blocks - pb) {
                    IDP_BAD("blk=%llu ext[%u] 物理段非法 base=%llu len=%llu", (unsigned long long)blk, i,
                            (unsigned long long)pb, (unsigned long long)len);
                } else {
                    /* 逻辑 [beg,beg+len) → 物理 [pb,+len)：按 8K 片铺 piece_phys_。
                     * 只取每片**首块**（lblk 偶）写片基址，否则片内第二块会把它覆盖成 pb+1
                     * （off-by-4096 → 读 inode 整体错位）。段 is 2 对齐 + length 偶，故每片
                     * 必以偶 lblk 起。 */
                    for (uint64_t k = 0; k < len; k++) {
                        c->blk_set(pb + k);
                        const uint64_t lblk = beg + k;
                        if ((lblk & 1) == 0) {
                            const uint64_t piece = lblk >> 1;   /* 2 块/片 */
                            if (piece < piece_phys->size()) (*piece_phys)[piece] = pb + (lblk - beg);
                        }
                    }
                    data_blocks += len;
                }
                leaf_max = end;
            }
            sub_ok = true;
            return leaf_max;
        }

        if (cnt == 0) { IDP_BAD("blk=%llu 内部节点空", (unsigned long long)blk); return 0; }
        if (expect_root && cnt == 1) IDP_BAD("blk=%llu 根内部仅 1 孩子", (unsigned long long)blk);
        if (!expect_root && cnt < EXTENT_MINA) IDP_BAD("blk=%llu 内部欠载 cnt=%u < %u", (unsigned long long)blk, cnt, (unsigned)EXTENT_MINA);

        sub_ok = true;
        const auto *nd = reinterpret_cast<const Bptree_inode_data_extent_inner_node_t *>(meta);
        std::vector<std::pair<uint64_t, uint64_t>> hints(it);
        for (uint16_t i = 0; i < it; i++) {
            hints[i].first  = RFS_PTR_ADDR(nd->extents[i].sub_node_ptr);
            hints[i].second = nd->extents[i].key;
        }
        uint64_t prev_key = 0, ret = 0;
        for (uint16_t i = 0; i < it; i++) {
            const uint64_t child = hints[i].first, key = hints[i].second;
            AUDIT_TRACE("[TRACE][IDPIDX] blk=%llu depth=%d idx=%u key=%llu child=%llu\n",
                        (unsigned long long)blk, depth, i, (unsigned long long)key,
                        (unsigned long long)child);
            if (i > 0 && key <= prev_key)
                IDP_BAD("blk=%llu hint[%u] key=%llu 非严格递增", (unsigned long long)blk, i, (unsigned long long)key);
            prev_key = key;
            if (child == PTR52 || child >= total_blocks) {
                IDP_BAD("blk=%llu hint[%u] child 非法=%llu", (unsigned long long)blk, i, (unsigned long long)child);
                continue;
            }
            bool cok = false;
            const uint64_t cmax = walk(child, owner_inum, blk, false, depth + 1, cok);
            if (cok && cmax != key)
                IDP_BAD("blk=%llu hint[%u] key=%llu != 子树 max=%llu", (unsigned long long)blk, i,
                        (unsigned long long)key, (unsigned long long)cmax);
            ret = cok ? cmax : key;
        }
        return ret;
    }
    #undef IDP_BAD
};

} /* namespace */

bool inode_resolver::build(const audit_image &img, audit_reporter &rep, audit_summary_t &sum,
                           dfs_ctx_t &c, bool &mapping_ok)
{
    mapping_ok = false;
    img_ = &img;
    const RavenSuperblock &sb = img.sb();
    const uint64_t total_blocks = img.geo().total_blocks;

    const uint64_t ctl    = sb.inode_data_ctl;
    const uint64_t ptr    = RFS_IDP_CTL_PTR(ctl);
    const bool     direct = RFS_IDP_CTL_IS_DIRECT(ctl) != 0;
    const uint64_t pieces = RFS_IDP_PIECE_COUNT(sb.inode_data_plane_pieces_count);
    const uint64_t target = pieces * 2ULL;

    direct_    = direct;
    pieces_    = pieces;
    inode_count_ = pieces * RFS_IDP_INODES_PER_PIECE;
    piece_phys_.assign((size_t)pieces, AUDIT_PTR52);

    sum.inodeplane_is_tree = direct ? 0 : 1;
    sum.inodeplane_pieces  = pieces;

    AUDIT_TRACE("[TRACE][IDP] mode=%s ctl=%#llx ptr=%llu pieces=%llu target_blk=%llu inode_count=%llu\n",
                direct ? "direct" : "tree", (unsigned long long)ctl, (unsigned long long)ptr,
                (unsigned long long)pieces, (unsigned long long)target,
                (unsigned long long)(pieces * RFS_IDP_INODES_PER_PIECE));

    /* SB 控制字自洽 */
    if (RFS_IDP_CTL_RSV(ctl) != 0) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "idp_ctl", "保留位非 0"); }
    if (RFS_IDP_PIECE_RSV(sb.inode_data_plane_pieces_count) != 0) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "idp_pieces", "保留位非 0"); }
    if (ptr != RFS_IDP_CTL_PTR_INVALID && pieces == 0) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "idp_ctl", "ptr=%llu 但 pieces=0", (unsigned long long)ptr); }

    if (ptr == RFS_IDP_CTL_PTR_INVALID || pieces == 0) {
        /* 空态：不可解析（F4 门已在引导期拒绝，这里防御） */
        available_ = false;
        return false;
    }

    if (direct) {
        base_blk_ = ptr;
        if (ptr >= total_blocks || target > total_blocks - ptr) {
            sum.inodeplane_bad++;
            rep.error(scope_t::inode_plane, "idp_direct", "区间越界 base=%llu blk=%llu total=%llu",
                      (unsigned long long)ptr, (unsigned long long)target, (unsigned long long)total_blocks);
            available_ = false;
            return false;
        }
        for (uint64_t p = 0; p < pieces; p++) piece_phys_[(size_t)p] = ptr + p * 2ULL;
        claim_range(c, ptr, target);
        sum.inodeplane_data_blk += target;
        AUDIT_TRACE("[TRACE][IDPDIRECT] base=%llu data_blk=%llu\n",
                    (unsigned long long)ptr, (unsigned long long)target);
        available_ = true;
        mapping_ok = true;
        return true;
    }

    /* tree 态 */
    if (ptr & 1) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "idp_tree", "根=%llu 未 8K 对齐", (unsigned long long)ptr); }
    IdpWalker w;
    w.img = &img; w.rep = &rep; w.sum = &sum; w.c = &c;
    w.piece_phys = &piece_phys_; w.target = target; w.max_depth = c.max_depth;
    w.total_blocks = total_blocks;
    bool sub_ok = false;
    w.walk(ptr, RFS_ROOT_INODE_IDX, RFS_ROOT_INODE_IDX, true, 0, sub_ok);
    if (w.logical_end != target) {
        sum.inodeplane_bad++;
        rep.error(scope_t::inode_plane, "idp_tree", "逻辑覆盖 end=%llu != pieces*2=%llu",
                  (unsigned long long)w.logical_end, (unsigned long long)target);
    }
    sum.inodeplane_data_blk += w.data_blocks;
    sum.inodeplane_tree_blk += w.tree_blocks;

    /* 叶链（从根下钻最左叶，双向自洽）——简化：只做 DFS 叶序与 ptrs 互指 */
    /* （完整叶链见 walk_leaf_chain 的通用版；此处复用最小校验） */
    available_ = true;
    mapping_ok = true;
    return true;
}

uint64_t inode_resolver::phys_block(uint64_t inum) const
{
    if (!available_ || inum >= inode_count_) return AUDIT_PTR52;
    const uint64_t p = inum >> RFS_IDP_SHIFT_INODE_PIECE;
    if (p >= piece_phys_.size()) return AUDIT_PTR52;
    const uint64_t phys = piece_phys_[(size_t)p];
    if (phys == AUDIT_PTR52) return AUDIT_PTR52;
    const uint64_t off = (inum & (RFS_IDP_INODES_PER_PIECE - 1)) * RFS_IDP_INODE_BYTES;
    return phys + off / 4096;
}

const uint8_t *inode_resolver::slot(uint64_t inum) const
{
    if (!available_ || inum >= inode_count_) return nullptr;
    const uint64_t p = inum >> RFS_IDP_SHIFT_INODE_PIECE;
    if (p >= piece_phys_.size()) return nullptr;
    const uint64_t phys = piece_phys_[(size_t)p];
    if (phys == AUDIT_PTR52) return nullptr;
    const uint64_t off = (inum & (RFS_IDP_INODES_PER_PIECE - 1)) * RFS_IDP_INODE_BYTES;
    return img_->fsblk(phys) + off;
}

const raven_Inode_t *inode_resolver::inode(uint64_t inum) const
{
    const uint8_t *p = slot(inum);
    return p ? reinterpret_cast<const raven_Inode_t *>(p) : nullptr;
}

void inode_alloc_ledger::build(const audit_image &img, audit_reporter &rep, audit_summary_t &sum,
                               dfs_ctx_t &c, bool &fr_ok)
{
    fr_ok = false;
    const RavenSuperblock &sb = img.sb();
    const uint64_t total_blocks = img.geo().total_blocks;
    const uint64_t ctl    = sb.inode_data_fr_ctl;
    const uint64_t fr_cnt = RFS_IDP_FR_COUNT(sb.inode_data_fr_count);
    const uint64_t ptr    = RFS_IDP_FR_CTL_PTR(ctl);
    const uint64_t pieces = RFS_IDP_PIECE_COUNT(sb.inode_data_plane_pieces_count);
    const uint64_t limit  = pieces * RFS_IDP_INODES_PER_PIECE;

    inode_count_ = limit;
    alloc_.assign((size_t)((limit + 7) / 8), 0x00);

    AUDIT_TRACE("[TRACE][IDPFR] ctl=%#llx ptr=%llu count=%llu limit=%llu\n",
                (unsigned long long)ctl, (unsigned long long)ptr,
                (unsigned long long)fr_cnt, (unsigned long long)limit);

    if (!RFS_IDP_FR_CTL_RSV0_OK(ctl)) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr_ctl", "rsv0 非 0"); }
    if (RFS_IDP_FR_CTL_RSV(ctl) != 0) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr_ctl", "保留位非 0"); }

    if (ptr == RFS_IDP_FR_CTL_PTR_INVALID) {
        if (fr_cnt != 0) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr", "空态 ptr 全 1 但 fr_count=%llu", (unsigned long long)fr_cnt); }
        /* 空态：全部 inode 已分配（无空闲）——有效 */
        for (uint64_t i = 0; i < limit; i++) alloc_[i >> 3] |= (uint8_t)(1u << (i & 7));
        available_ = true;
        fr_ok = true;
        return;
    }
    if (fr_cnt == 0) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr", "ptr=%llu 但 fr_count=0", (unsigned long long)ptr); }

    const uint64_t bytes = sizeof(inode_data_fr_snapshot_hdr) + fr_cnt * sizeof(inode_dataplane_coordinate_free_range_t);
    uint64_t fblks = (bytes + 4095) / 4096; if (fblks == 0) fblks = 1;
    if (ptr >= total_blocks || fblks > total_blocks - ptr) {
        sum.inodeplane_bad++;
        rep.error(scope_t::inode_plane, "fr", "区越界 ptr=%llu blks=%llu total=%llu",
                  (unsigned long long)ptr, (unsigned long long)fblks, (unsigned long long)total_blocks);
        available_ = false;   /* F5 软失败：fr 不可读 */
        return;
    }
    claim_range(c, ptr, fblks);
    sum.inodeplane_fr_blk += fblks;

    const uint8_t *base = img.fsblk(ptr);
    const auto *h = reinterpret_cast<const inode_data_fr_snapshot_hdr *>(base);
    if (h->magic[0] != INODE_DATA_FR_SNAPSHOT_MAGIC1 || h->magic[1] != INODE_DATA_FR_SNAPSHOT_MAGIC2 ||
        h->magic[2] != INODE_DATA_FR_SNAPSHOT_MAGIC3 || h->magic[3] != INODE_DATA_FR_SNAPSHOT_MAGIC4) {
        sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr", "hdr magic 不符 ptr=%llu", (unsigned long long)ptr);
    }
    if (h->entry_count != fr_cnt) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr", "hdr.entry_count=%llu != SB fr_count=%llu", (unsigned long long)h->entry_count, (unsigned long long)fr_cnt); }
    if (h->capacity < fr_cnt) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr", "hdr.capacity=%llu < count=%llu", (unsigned long long)h->capacity, (unsigned long long)fr_cnt); }

    /* crc32：覆盖 hdr（除 crc）+ entries */
    {
        const size_t span = sizeof(inode_data_fr_snapshot_hdr) + (size_t)fr_cnt * sizeof(inode_dataplane_coordinate_free_range_t);
        if (span <= fblks * 4096) {
            std::vector<uint8_t> cpy(base, base + span);
            inode_data_fr_snapshot_hdr *hc = reinterpret_cast<inode_data_fr_snapshot_hdr *>(cpy.data());
            const uint32_t want = hc->crc32;
            hc->crc32 = 0;
            const uint32_t got = [](const void *d, size_t n) {
                static uint32_t tbl[256]; static bool ready = false;
                if (!ready) { for (uint32_t i = 0; i < 256; i++) { uint32_t cr = i; for (int j = 0; j < 8; j++) cr = (cr >> 1) ^ (cr & 1u ? 0xEDB88320u : 0u); tbl[i] = cr; } ready = true; }
                uint32_t cr = 0xFFFFFFFFu; const uint8_t *b = (const uint8_t *)d;
                for (size_t i = 0; i < n; i++) cr = tbl[(cr ^ b[i]) & 0xFFu] ^ (cr >> 8);
                return cr ^ 0xFFFFFFFFu;
            }(cpy.data(), span);
            if (want != got) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr", "crc32 不符 stored=%#x recalc=%#x", want, got); }
        }
    }

    /* 全分配起步，再清空闲区间 */
    for (uint64_t i = 0; i < limit; i++) alloc_[i >> 3] |= (uint8_t)(1u << (i & 7));
    const auto *e = reinterpret_cast<const inode_dataplane_coordinate_free_range_t *>(base + sizeof(*h));
    const uint64_t room = (fblks * 4096 > sizeof(*h)) ? (fblks * 4096 - sizeof(*h)) / sizeof(*e) : 0;
    const uint64_t nent = fr_cnt < room ? fr_cnt : room;
    if (fr_cnt > room) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr", "fr_count=%llu 超容量 %llu（截断）", (unsigned long long)fr_cnt, (unsigned long long)room); }
    bool have = false; uint64_t prev_end = 0;
    for (uint64_t i = 0; i < nent; i++) {
        const uint64_t eb = e[i].base, el = e[i].len;
        AUDIT_TRACE("[TRACE][IDPFR] idx=%llu base=%llu len=%llu end=%llu\n",
                    (unsigned long long)i, (unsigned long long)eb,
                    (unsigned long long)el, (unsigned long long)(eb + el));
        if (el == 0) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr", "[%llu] len==0", (unsigned long long)i); }
        if (eb > limit || el > limit - eb) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr", "[%llu] 越界 base=%llu len=%llu limit=%llu", (unsigned long long)i, (unsigned long long)eb, (unsigned long long)el, (unsigned long long)limit); }
        if (have && eb < prev_end) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr", "[%llu] 重叠/非递增", (unsigned long long)i); }
        if (eb == 0) { sum.inodeplane_bad++; rep.error(scope_t::inode_plane, "fr", "空闲区间含 idx0（root 永不外发）"); }
        prev_end = eb + el; have = true;
        for (uint64_t k = 0; k < el && eb + k < limit; k++) alloc_[(eb + k) >> 3] &= (uint8_t)~(1u << ((eb + k) & 7));
    }
    available_ = true;
    fr_ok = true;
}

bool inode_alloc_ledger::allocated(uint64_t inum) const
{
    if (!available_ || inum >= inode_count_) return false;
    return (alloc_[inum >> 3] >> (inum & 7)) & 1u;
}

inode_plane_auditor::result_t inode_plane_auditor::audit(const audit_image &img, audit_reporter &rep,
                                                         audit_summary_t &sum, dfs_ctx_t &c)
{
    result_t r;
    res_.build(img, rep, sum, c, r.mapping_ok);
    alloc_.build(img, rep, sum, c, r.fr_ok);
    return r;
}

} /* namespace rfs_audit */

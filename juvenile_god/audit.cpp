/* ═══════════════════════════════════════════════════════════════════
 * audit.cpp — RavenFS v1.1 静态合法性审计（audit_inode 模块）
 *
 * 设计基线：Docs/audit_inode_design.md（2026-09-10 定稿）
 *
 * 原则：
 *   1. 进上帝类的模块：实现在本 TU，是 RavenFS_juvenile 的成员方法；
 *      复用其位图（block_test/inode_test）+ 缓存（mdata_piece_session）。
 *   2. 复用存储设施，不复用导航逻辑：DFS / 路由校验自己重新推导，
 *      绝不调 dentry_Bptree_walk / extent_Bptree_walk（防共因失效）。
 *   3. audit_inode 粒度 = 单 inode（dir 只 DFS 自己的 dentry 树，不下钻子目录）。
 *   4. 只读、不修复；对账粒度只到计数；每 inode 只有一个标志位。
 *   5. 重点 = B+树数据结构一致性，错误详细打日志。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/ravenfs_audit.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

namespace {

/* 52 位无效指针（ptr_in_idx.ptr / parent_ptr_t.ptr 位域截断形态） */
constexpr uint64_t PTR52 = (1ULL << 52) - 1;
inline bool ptr52_bad(uint64_t p) { return p == PTR52; }

/* 阶 / 非根欠载下限（与 split/balance 实现挂钩；dentry 半满 63，extent 168） */
constexpr uint16_t DENTRY_ORDER = 126;
constexpr uint16_t EXTENT_ORDER = 337;
constexpr uint16_t DENTRY_MINA  = 63;
constexpr uint16_t EXTENT_MINA  = 168;

/* 名字堆静态几何（与 name_heap.cpp / raven_dir_Inode_t 字段序一致） */
constexpr uint8_t  HEAP_GRAN[10] = { 3,3,3, 6,6, 7,7, 8,8, 9 };
constexpr uint8_t  HEAP_BITS[10] = { 16,17,18, 16,17, 16,17, 16,17, 16 };

void audit_vlog(audit_ctx_t &ctx, const char *lvl, const char *fmt, va_list ap)
{
    char line[1200];
    int n = snprintf(line, sizeof(line), "[AUDIT][%s] ", lvl);
    if (n < 0) n = 0;
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    size_t len = strlen(line);
    if (len + 1 < sizeof(line)) { line[len] = '\n'; line[len + 1] = '\0'; }
    if (ctx.opts && ctx.opts->log.emit)
        ctx.opts->log.emit(ctx.opts->log.ud, line);
    else
        fputs(line, stderr);
}
void audit_log(audit_ctx_t &ctx, const char *lvl, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); audit_vlog(ctx, lvl, fmt, ap); va_end(ap);
}
/* B+树结构违例：计数（重点项）+ 详细日志 */
void audit_bad(audit_ctx_t &ctx, const char *fmt, ...)
{
    ctx.btree_bad++;
    va_list ap; va_start(ap, fmt); audit_vlog(ctx, "BAD", fmt, ap); va_end(ap);
}

/* 认领一个物理块：重入 = double_claim（红牌） */
void audit_claim(audit_ctx_t &ctx, uint64_t blk, uint64_t owner, const char *kind)
{
    if (blk >= ctx.total_blocks) return;
    if (ctx.claim_test(blk)) {
        ctx.double_claim++;
        audit_log(ctx, "BAD", "block %llu 双重认领 (owner=inode %llu kind=%s)",
                  (unsigned long long)blk, (unsigned long long)owner, kind);
    } else {
        ctx.claim_set(blk);
    }
}

bool is_backup_blk(const audit_ctx_t &ctx, uint64_t blk)
{
    for (uint64_t b : ctx.backup_blocks) if (b == blk) return true;
    return false;
}

}   /* namespace */

/* ═══════════════════════════════════════════════════════════════════
 * audit_begin — seed ctx
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::audit_begin(audit_ctx_t &ctx, const audit_opts_t &opts)
{
    if (fd < 0) return RC_ERR_STATE;
    ctx.fs = this;
    ctx.opts = &opts;
    ctx.total_blocks = total_blocks;
    ctx.inode_count  = inode_count;

    ctx.claim_bits.assign((total_blocks + 7) / 8, 0);
    ctx.ref_count.assign(inode_count, 0);
    ctx.parent_of.assign(inode_count, PTR_INVALID);
    ctx.dotdot_of.assign(inode_count, PTR_INVALID);
    ctx.inode_flag.assign((inode_count + 7) / 8, 0);
    ctx.visited_nodes.clear();
    ctx.heap_occ.clear();
    ctx.cur_heaps.clear();
    ctx.cur_dir = PTR_INVALID;

    ctx.nodes = ctx.inner = ctx.leaves = ctx.records = 0;
    ctx.btree_bad = ctx.inode_bad = 0;
    ctx.double_claim = ctx.leaked_blocks = ctx.orphan_blocks = 0;
    ctx.dangling_refs = ctx.orphan_inodes = 0;
    ctx.link_mismatch = ctx.dotdot_mismatch = ctx.heap_bad = 0;

    /* 结构块白名单：尾巴三段 [inode_bm_base, total_blocks) 常亮且无 owner */
    ctx.meta_tail_lo = inode_bm_base;
    ctx.meta_tail_hi = total_blocks;

    /* backup SB 块（链表遍历；不修改任何东西） */
    ctx.backup_blocks.clear();
    uint64_t blk = sb.backup_next;
    int guard = 0;
    while (blk != PTR_INVALID && !ptr52_bad(blk) && blk < total_blocks &&
           guard < (int)sb.backup_count + 8) {
        ctx.backup_blocks.push_back(blk);
        alignas(4096) uint8_t buf[4096];
        if (read_buff(blk, 1, buf) != RC_OK) break;
        RavenSuperblock bs; memcpy(&bs, buf, sizeof(bs));
        uint64_t nxt = bs.backup_next;
        if (nxt == PTR_INVALID || ptr52_bad(nxt) || nxt == blk || nxt >= total_blocks) break;
        blk = nxt; guard++;
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_inode — 单 inode 分派（dir 不下钻子目录）
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::audit_inode(uint64_t inum, audit_ctx_t &ctx)
{
    if (inum >= inode_count) return RC_ERR_STATE;
    if (!inode_test(inum)) return RC_OK;   /* 未分配：无审计对象（悬挂由引用方记） */

    raven_Inode_t raw;
    inode_read(inum, &raw);
    if (raw.type == INODE_TYPE_DIR) {
        audit_dir_inode(ctx, inum, (const raven_dir_Inode_t *)&raw);
    } else if (raw.type == INODE_TYPE_REGULAR || raw.type == INODE_TYPE_SOFT_SYM) {
        audit_file_inode(ctx, inum, &raw);
    } else {
        ctx.inode_bad++; ctx.flag_set(inum);
        audit_log(ctx, "BAD", "inode %llu 非法 type=%llu",
                  (unsigned long long)inum, (unsigned long long)raw.type);
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_dir_inode
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_dir_inode(audit_ctx_t &ctx, uint64_t inum, const raven_dir_Inode_t *d)
{
    /* link_count == size + 2（不变量 18，自足校验） */
    if ((uint64_t)d->link_count != d->size + 2) {
        ctx.link_mismatch++; ctx.flag_set(inum);
        audit_log(ctx, "BAD", "DIR inode %llu link_count=%u != size+2 (%llu)",
                  (unsigned long long)inum, d->link_count,
                  (unsigned long long)(d->size + 2));
    }

    uint64_t root = d->data_root;
    if (root == PTR_INVALID || root >= total_blocks || (root & 1)) {
        ctx.inode_bad++; ctx.flag_set(inum);
        audit_log(ctx, "BAD", "DIR inode %llu data_root 非法=%#llx",
                  (unsigned long long)inum, (unsigned long long)root);
        return;
    }

    /* 名字堆描述符 + 占用率（填充 ctx.cur_heaps 供长名指针归属校验） */
    if (!ctx.opts || ctx.opts->check_name_heap)
        audit_name_heaps(ctx, inum, d);

    /* dentry B+tree DFS */
    ctx.cur_dir = inum;
    audit_tree_ctx_t t;
    bool ok = true;
    audit_btree_node(ctx, t, root, inum, PTR_INVALID, true, true, 0, 0, ok);
    audit_leaf_chain(ctx, t, root, true, inum);
    ctx.cur_dir = PTR_INVALID;

    /* size == 实际 dentry 条目数 */
    if (t.records != d->size) {
        ctx.inode_bad++; ctx.flag_set(inum);
        audit_log(ctx, "BAD", "DIR inode %llu size=%llu != 实际 dentry 条目 %llu",
                  (unsigned long long)inum, (unsigned long long)d->size,
                  (unsigned long long)t.records);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_file_inode — REG / SOFT_SYM（size 优先解码；inline 忽视 direct_bit）
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_file_inode(audit_ctx_t &ctx, uint64_t inum, const raven_Inode_t *ino)
{
    if (ino->size <= 160) {
        /* inline：只依 size；direct_bit 显式忽视；data_root 残留 = 黄牌 */
        if (ino->data_root != PTR_INVALID) {
            ctx.flag_set(inum);
            audit_log(ctx, "WARN", "inode %llu size=%llu inline 但 data_root 残留=%#llx（黄牌）",
                      (unsigned long long)inum, (unsigned long long)ino->size,
                      (unsigned long long)ino->data_root);
        }
        return;
    }

    const uint64_t blocks = (ino->size + 4095) / 4096;   /* ceil(size/4096) */
    if (ino->is_data_root_direct_bit) {
        /* direct：连续区 [base, base+blocks) */
        uint64_t base = ino->data_root;
        if (base == PTR_INVALID || base >= total_blocks || base + blocks > total_blocks) {
            ctx.inode_bad++; ctx.flag_set(inum);
            audit_log(ctx, "BAD", "inode %llu direct data_root=%#llx blocks=%llu 越界",
                      (unsigned long long)inum, (unsigned long long)base,
                      (unsigned long long)blocks);
            return;
        }
        for (uint64_t i = 0; i < blocks; i++) audit_claim(ctx, base + i, inum, "direct");
    } else {
        /* extent：data_root = 区段树根 */
        uint64_t root = ino->data_root;
        if (root == PTR_INVALID || root >= total_blocks || (root & 1)) {
            ctx.inode_bad++; ctx.flag_set(inum);
            audit_log(ctx, "BAD", "inode %llu extent tree root 非法=%#llx",
                      (unsigned long long)inum, (unsigned long long)root);
            return;
        }
        audit_tree_ctx_t t;
        bool ok = true;
        audit_btree_node(ctx, t, root, inum, PTR_INVALID, true, false, blocks, 0, ok);
        audit_leaf_chain(ctx, t, root, false, inum);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_btree_node — B+树 DFS 递归核（重点）
 * ═══════════════════════════════════════════════════════════════════ */
uint64_t RavenFS_juvenile::audit_btree_node(audit_ctx_t &ctx, audit_tree_ctx_t &t, uint64_t blk,
                                            uint64_t owner_inum, uint64_t expect_parent,
                                            bool expect_root, bool is_dentry, uint64_t size_bound,
                                            int depth, bool &sub_ok)
{
    sub_ok = false;
    const char *tree = is_dentry ? "dentry" : "extent";
    const uint16_t order = is_dentry ? DENTRY_ORDER : EXTENT_ORDER;
    const uint16_t mina  = is_dentry ? DENTRY_MINA  : EXTENT_MINA;

    if (depth > (ctx.opts ? ctx.opts->max_tree_depth : 64)) {
        audit_bad(ctx, "tree=%s inum=%llu depth 超限 %d (blk=%llu)", tree,
                  (unsigned long long)owner_inum, depth, (unsigned long long)blk);
        return 0;
    }
    if (blk == PTR_INVALID || ptr52_bad(blk) || blk >= total_blocks || (blk & 1)) {
        audit_bad(ctx, "tree=%s inum=%llu blk=%llu 非法节点块号", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk);
        return 0;
    }
    if (ctx.visited_nodes.count(blk)) {
        audit_bad(ctx, "tree=%s inum=%llu blk=%llu 重入（环/共享子节点）", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk);
        return 0;
    }
    ctx.visited_nodes.insert(blk);
    audit_claim(ctx, blk, owner_inum, tree);
    if (blk + 1 < total_blocks) audit_claim(ctx, blk + 1, owner_inum, tree);

    mdata_piece_session sess(mdata_piece_mgr, blk);
    if (sess.index_error() || !sess) {
        audit_bad(ctx, "tree=%s inum=%llu blk=%llu 节点读取失败 (idx_err=%d)", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk, (int)sess.index_error());
        return 0;
    }
    Bptree_meta_t *meta = (Bptree_meta_t *)sess.data;
    const bool magic_ok = is_dentry ? is_dentry_node(meta) : is_extent_node(meta);
    if (!magic_ok) {
        audit_bad(ctx, "tree=%s inum=%llu blk=%llu magic 不符", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk);
        return 0;
    }

    ctx.nodes++; t.nodes++;
    const uint16_t cnt = (uint16_t)meta->parent_ptr.entry_count;
    const uint16_t it  = std::min<uint16_t>(cnt, (uint16_t)(order + 1));

    if (ctx.opts && ctx.opts->trace_nodes)
        audit_log(ctx, "TRACE", "tree=%s inum=%llu blk=%llu depth=%d inner=%d root=%d cnt=%u",
                  tree, (unsigned long long)owner_inum, (unsigned long long)blk, depth,
                  (int)meta->parent_ptr.is_inner_node, (int)meta->parent_ptr.is_root_node, cnt);

    /* 角色 / 父指针 */
    if ((bool)meta->parent_ptr.is_root_node != expect_root)
        audit_bad(ctx, "tree=%s inum=%llu blk=%llu is_root=%d 期望 %d", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk,
                  (int)meta->parent_ptr.is_root_node, (int)expect_root);
    if (expect_root) {
        if (meta->parent_ptr.ptr != owner_inum)
            audit_bad(ctx, "tree=%s inum=%llu blk=%llu root('.') ptr=%llu != owner %llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk,
                      (unsigned long long)meta->parent_ptr.ptr, (unsigned long long)owner_inum);
        if (is_dentry) ctx.dotdot_of[owner_inum] = meta->ptrs[1].ptr;   /* '..' inode */
    } else {
        if (meta->parent_ptr.ptr != expect_parent)
            audit_bad(ctx, "tree=%s inum=%llu blk=%llu parent_ptr=%llu != 实际父 %llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk,
                      (unsigned long long)meta->parent_ptr.ptr, (unsigned long long)expect_parent);
    }
    if (cnt > order)
        audit_bad(ctx, "tree=%s inum=%llu blk=%llu entry_count=%u > 阶 %u（瞬时槽落盘？）", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk, cnt, order);

    /* ══════ 叶子 ══════ */
    if (!meta->parent_ptr.is_inner_node) {
        ctx.leaves++; t.leaves++;
        t.dfs_leaves.push_back(blk);
        if (!t.leaf_depth_set) { t.leaf_depth = (uint64_t)depth; t.leaf_depth_set = true; }
        else if ((uint64_t)depth != t.leaf_depth)
            audit_bad(ctx, "tree=%s inum=%llu blk=%llu 叶深度 %d != %llu（不平衡）", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk, depth,
                      (unsigned long long)t.leaf_depth);

        if (cnt == 0) {
            if (!expect_root) audit_bad(ctx, "tree=%s inum=%llu blk=%llu 非根叶空", tree,
                                        (unsigned long long)owner_inum, (unsigned long long)blk);
            else if (!is_dentry) audit_bad(ctx, "tree=%s inum=%llu blk=%llu 根叶空（extent 空树）",
                                           tree, (unsigned long long)owner_inum, (unsigned long long)blk);
        } else if (!expect_root && cnt < mina) {
            audit_bad(ctx, "tree=%s inum=%llu blk=%llu 叶欠载 cnt=%u < %u", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk, cnt, mina);
        }

        sub_ok = true;   /* 读 + magic 通过（父层 key==max 比对可用） */

        if (is_dentry) {
            auto *leaf = (Bptree_dentry_leaf_node_t *)sess.data;
            uint64_t prev_hash = 0; char prev_name[DENTRY_INLINE_NAME_MAX];
            memset(prev_name, 0, sizeof(prev_name));
            for (uint16_t i = 0; i < it; i++) {
                const Dentry_t &e = leaf->entries[i];
                if (i > 0) {
                    const bool bad_order =
                        (e.hash < prev_hash) ||
                        (e.hash == prev_hash &&
                         memcmp(e.name, prev_name, DENTRY_INLINE_NAME_MAX) <= 0);
                    if (bad_order)
                        audit_bad(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] 复合序非严格递增",
                                  (unsigned long long)owner_inum, (unsigned long long)blk, i);
                }
                prev_hash = e.hash;
                memcpy(prev_name, e.name, DENTRY_INLINE_NAME_MAX);

                const uint16_t name_len = (uint16_t)e.inode_ptr.free_to_use;
                const uint64_t target   = e.inode_ptr.ptr;

                if (name_len > RAVENFS_MAX_NAME_LEN)
                    audit_bad(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] name_len=%u 超上限 %u",
                              (unsigned long long)owner_inum, (unsigned long long)blk, i,
                              name_len, (unsigned)RAVENFS_MAX_NAME_LEN);

                /* 名字重建（≤40 内联 / >40 读堆）→ hash 重算 */
                char full[RAVENFS_MAX_NAME_LEN + 1];
                const uint16_t ncopy = name_len < DENTRY_INLINE_NAME_MAX
                                           ? name_len : DENTRY_INLINE_NAME_MAX;
                memcpy(full, e.name, ncopy);
                bool name_ok = true;
                if (name_len > DENTRY_INLINE_NAME_MAX) {
                    if (e.full_char_remain_ptr == PTR_INVALID) {
                        ctx.heap_bad++; name_ok = false;
                        audit_log(ctx, "BAD", "tree=dentry inum=%llu blk=%llu entries[%u] 长名但 remain=PTR_INVALID",
                                  (unsigned long long)owner_inum, (unsigned long long)blk, i);
                    } else {
                        const uint16_t tail = name_len - DENTRY_INLINE_NAME_MAX;
                        const uint16_t units = (tail <= 40) ? (uint16_t)((tail + 7) / 8) : 1;
                        const uint64_t fblk = e.full_char_remain_ptr >> 12;
                        const uint16_t off  = (uint16_t)(e.full_char_remain_ptr & 0xFFF);
                        const audit_heap_region_t *hit = nullptr;
                        for (const auto &r : ctx.cur_heaps)
                            if (fblk >= r.data_lo && fblk < r.data_hi) { hit = &r; break; }
                        if (!hit) {
                            ctx.heap_bad++; name_ok = false;
                            audit_log(ctx, "BAD", "tree=dentry inum=%llu blk=%llu entries[%u] 长名 remain=%#llx 不落本目录任何激活堆数据区",
                                      (unsigned long long)owner_inum, (unsigned long long)blk, i,
                                      (unsigned long long)e.full_char_remain_ptr);
                        } else if (name_heap_read(name_len, e.full_char_remain_ptr,
                                                  full + DENTRY_INLINE_NAME_MAX) != RC_OK) {
                            ctx.heap_bad++; name_ok = false;
                            audit_log(ctx, "BAD", "tree=dentry inum=%llu blk=%llu entries[%u] 长名尾部读取失败",
                                      (unsigned long long)owner_inum, (unsigned long long)blk, i);
                        } else {
                            const uint64_t sbyte = (fblk - hit->data_lo) * 4096 + off;
                            const uint64_t bit0  = sbyte >> hit->gran_log2;
                            bool bits_ok = true;
                            for (uint16_t u = 0; u < units && bits_ok; u++) {
                                int b = audit_heap_bit(*hit, bit0 + u);
                                if (b != 1) bits_ok = false;
                            }
                            if (!bits_ok) {
                                ctx.heap_bad++; name_ok = false;
                                audit_log(ctx, "BAD", "tree=dentry inum=%llu blk=%llu entries[%u] 长名堆位图未置位 (fblk=%llu off=%u units=%u)",
                                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                                          (unsigned long long)fblk, off, units);
                            }
                        }
                    }
                } else {
                    if (e.full_char_remain_ptr != PTR_INVALID)
                        audit_log(ctx, "WARN", "tree=dentry inum=%llu blk=%llu entries[%u] name_len<=40 但 remain 非 INVALID（软）",
                                  (unsigned long long)owner_inum, (unsigned long long)blk, i);
                }
                if (name_ok) {
                    full[name_len] = '\0';
                    const uint64_t h = name_hash(full);
                    if (h != e.hash)
                        audit_bad(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] hash 不符 stored=%#llx recalc=%#llx name_len=%u",
                                  (unsigned long long)owner_inum, (unsigned long long)blk, i,
                                  (unsigned long long)e.hash, (unsigned long long)h, name_len);
                }

                /* 引用账本 + 悬挂检测 */
                if (target == PTR52 || target >= ctx.inode_count) {
                    ctx.dangling_refs++;
                    audit_bad(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] inode_ptr=%llu 越界",
                              (unsigned long long)owner_inum, (unsigned long long)blk, i,
                              (unsigned long long)target);
                } else {
                    if (!inode_test(target)) {
                        ctx.dangling_refs++;
                        audit_bad(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] → inode %llu 未分配（悬挂）",
                                  (unsigned long long)owner_inum, (unsigned long long)blk, i,
                                  (unsigned long long)target);
                    }
                    if (ctx.parent_of[target] == PTR_INVALID)
                        ctx.parent_of[target] = owner_inum;
                    ctx.ref_count[target]++;
                }
            }
            t.records += it;
            ctx.records += it;
            return (it > 0) ? leaf->entries[it - 1].hash : 0;
        }

        /* extent 叶 */
        auto *leaf = (Bptree_extent_leaf_node_t *)sess.data;
        uint64_t prev_end = 0;
        for (uint16_t i = 0; i < it; i++) {
            const extent_t &e = leaf->extents[i];
            if (e.length == 0)
                audit_bad(ctx, "tree=extent inum=%llu blk=%llu extents[%u] length==0", 
                          (unsigned long long)owner_inum, (unsigned long long)blk, i);
            if (i > 0 && prev_end > e.fblkbase)
                audit_bad(ctx, "tree=extent inum=%llu blk=%llu extents[%u] 重叠 (prev_end=%llu > fblkbase=%llu)",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                          (unsigned long long)prev_end, (unsigned long long)e.fblkbase);
            if (e.fblkbase + e.length > size_bound)
                audit_bad(ctx, "tree=extent inum=%llu blk=%llu extents[%u] 逻辑越界 end=%llu > size_bound=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                          (unsigned long long)(e.fblkbase + e.length), (unsigned long long)size_bound);
            const uint64_t pb = e.block_base.ptr;
            if (pb == PTR52 || pb >= total_blocks || pb + e.length > total_blocks) {
                audit_bad(ctx, "tree=extent inum=%llu blk=%llu extents[%u] 物理指针越界 base=%llu len=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                          (unsigned long long)pb, (unsigned long long)e.length);
            } else {
                for (uint64_t k = 0; k < e.length; k++) audit_claim(ctx, pb + k, owner_inum, "extent");
            }
            prev_end = e.fblkbase + e.length;
        }
        t.records += it;
        ctx.records += it;
        return prev_end;
    }

    /* ══════ 内部节点 ══════ */
    ctx.inner++; t.inner++;
    if (cnt == 0) {
        audit_bad(ctx, "tree=%s inum=%llu blk=%llu 内部节点空", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk);
        return 0;
    }
    if (expect_root && cnt == 1)
        audit_bad(ctx, "tree=%s inum=%llu blk=%llu 根内部仅 1 孩子（待收缩）", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk);
    if (!expect_root && cnt < mina)
        audit_bad(ctx, "tree=%s inum=%llu blk=%llu 内部欠载 cnt=%u < %u", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk, cnt, mina);

    sub_ok = true;
    uint64_t prev_key = 0, ret = 0;
    for (uint16_t i = 0; i < it; i++) {
        uint64_t child, key;
        if (is_dentry) {
            auto *nd = (Bptree_dentry_inner_node_t *)sess.data;
            child = nd->hints[i].sub_ptr.ptr;
            key   = nd->hints[i].hash_max;
        } else {
            auto *nd = (Bptree_extent_inner_node_t *)sess.data;
            child = nd->extents[i].sub_node_ptr.ptr;
            key   = nd->extents[i].key;
        }
        if (i > 0 && key <= prev_key)
            audit_bad(ctx, "tree=%s inum=%llu blk=%llu hint[%u] key=%llu 非严格递增", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk, i,
                      (unsigned long long)key);
        prev_key = key;
        if (child == PTR52 || child >= total_blocks) {
            audit_bad(ctx, "tree=%s inum=%llu blk=%llu hint[%u] child 指针非法=%llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk, i,
                      (unsigned long long)child);
            continue;
        }
        bool cok = false;
        const uint64_t cmax = audit_btree_node(ctx, t, child, owner_inum, blk, false,
                                               is_dentry, size_bound, depth + 1, cok);
        if (cok && cmax != key)
            audit_bad(ctx, "tree=%s inum=%llu blk=%llu hint[%u] key=%llu != 子树 max=%llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk, i,
                      (unsigned long long)key, (unsigned long long)cmax);
        ret = cok ? cmax : key;
    }
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_leaf_chain — 兄弟链双向自洽 + 链序 == DFS 叶序
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_leaf_chain(audit_ctx_t &ctx, audit_tree_ctx_t &t,
                                        uint64_t root_blk, bool is_dentry, uint64_t owner_inum)
{
    if (t.dfs_leaves.size() <= 1) return;   /* 单根叶：无链 */
    const char *tree = is_dentry ? "dentry" : "extent";

    /* 从根下钻最左叶 */
    uint64_t blk = root_blk;
    for (int d = 0; d < 64; d++) {
        mdata_piece_session sess(mdata_piece_mgr, blk);
        if (sess.index_error() || !sess) {
            audit_bad(ctx, "tree=%s inum=%llu 叶链下钻读取失败 blk=%llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk);
            return;
        }
        Bptree_meta_t *m = (Bptree_meta_t *)sess.data;
        if (!m->parent_ptr.is_inner_node) break;
        const uint16_t c = (uint16_t)m->parent_ptr.entry_count;
        if (c == 0) {
            audit_bad(ctx, "tree=%s inum=%llu 叶链下钻遇空内部节点 blk=%llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk);
            return;
        }
        blk = is_dentry ? ((Bptree_dentry_inner_node_t *)sess.data)->hints[0].sub_ptr.ptr
                        : ((Bptree_extent_inner_node_t *)sess.data)->extents[0].sub_node_ptr.ptr;
    }

    size_t idx = 0;
    uint64_t cur = blk;
    for (int guard = 0; guard < (int)t.dfs_leaves.size() + 2; guard++) {
        if (cur == PTR52) break;
        if (idx >= t.dfs_leaves.size()) {
            audit_bad(ctx, "tree=%s inum=%llu 兄弟链多出叶子 blk=%llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)cur);
            return;
        }
        if (cur != t.dfs_leaves[idx])
            audit_bad(ctx, "tree=%s inum=%llu 兄弟链序 != DFS 叶序 (cur=%llu expect=%llu idx=%zu)",
                      tree, (unsigned long long)owner_inum, (unsigned long long)cur,
                      (unsigned long long)t.dfs_leaves[idx], idx);
        mdata_piece_session sess(mdata_piece_mgr, cur);
        if (sess.index_error() || !sess) {
            audit_bad(ctx, "tree=%s inum=%llu 叶链节点读取失败 blk=%llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)cur);
            return;
        }
        Bptree_meta_t *m = (Bptree_meta_t *)sess.data;
        const uint64_t prev = m->ptrs[1].ptr;
        const uint64_t next = m->ptrs[2].ptr;
        if (prev != PTR52) {
            mdata_piece_session ps(mdata_piece_mgr, prev);
            if (ps.index_error() || !ps) {
                audit_bad(ctx, "tree=%s inum=%llu 前驱块读取失败 blk=%llu", tree,
                          (unsigned long long)owner_inum, (unsigned long long)prev);
            } else if (((Bptree_meta_t *)ps.data)->ptrs[2].ptr != cur) {
                audit_bad(ctx, "tree=%s inum=%llu 兄弟链断裂: blk=%llu.prev=%llu 但前驱.next != blk",
                          tree, (unsigned long long)owner_inum, (unsigned long long)cur,
                          (unsigned long long)prev);
            }
        }
        if (next != PTR52) {
            mdata_piece_session ns(mdata_piece_mgr, next);
            if (ns.index_error() || !ns) {
                audit_bad(ctx, "tree=%s inum=%llu 后继块读取失败 blk=%llu", tree,
                          (unsigned long long)owner_inum, (unsigned long long)next);
            } else if (((Bptree_meta_t *)ns.data)->ptrs[1].ptr != cur) {
                audit_bad(ctx, "tree=%s inum=%llu 兄弟链断裂: blk=%llu.next=%llu 但后继.prev != blk",
                          tree, (unsigned long long)owner_inum, (unsigned long long)cur,
                          (unsigned long long)next);
            }
        }
        cur = next; idx++;
    }
    if (idx != t.dfs_leaves.size())
        audit_bad(ctx, "tree=%s inum=%llu 兄弟链叶子数 %zu != DFS 叶数 %zu", tree,
                  (unsigned long long)owner_inum, idx, t.dfs_leaves.size());
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_name_heaps — 描述符自洽 + 占用率 + 归属区登记
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_name_heaps(audit_ctx_t &ctx, uint64_t inum, const raven_dir_Inode_t *d)
{
    ctx.cur_heaps.clear();
    const name_heap_controller_t *arr = (const name_heap_controller_t *)&d->heaps_8B_8k;
    for (int s = 0; s < 10; s++) {
        const name_heap_controller_t &c = arr[s];
        const uint64_t bmpb = c.bitmap_base_block_ptr;
        const uint64_t datab = c.data_base_block_ptr;
        const uint8_t gran = (uint8_t)c.bytes_per_bit_log2;
        const uint8_t bits = (uint8_t)c.total_bits_count_log2;
        const bool inactive = (bmpb == NAME_HEAP_INACTIVE && datab == NAME_HEAP_INACTIVE);
        if (inactive) continue;

        if (bmpb == NAME_HEAP_INACTIVE || datab == NAME_HEAP_INACTIVE) {
            ctx.heap_bad++;
            audit_log(ctx, "BAD", "DIR inode %llu heap[%d] 半激活 (bmp=%llu data=%llu)",
                      (unsigned long long)inum, s, (unsigned long long)bmpb, (unsigned long long)datab);
            continue;
        }
        if (gran != HEAP_GRAN[s] || bits != HEAP_BITS[s]) {
            ctx.heap_bad++;
            audit_log(ctx, "WARN", "DIR inode %llu heap[%d] log2 非常规 gran=%u bits=%u (期望 %u/%u)",
                      (unsigned long long)inum, s, gran, bits, (unsigned)HEAP_GRAN[s], (unsigned)HEAP_BITS[s]);
        }
        const uint64_t bmp_blks  = 1ULL << (bits - 15);
        const uint64_t data_blks = 1ULL << (bits + gran - 12);
        if (bmpb >= total_blocks || (bmpb & 1) || bmpb + bmp_blks > total_blocks ||
            datab >= total_blocks || (datab & 1) || datab + data_blks > total_blocks) {
            ctx.heap_bad++;
            audit_log(ctx, "BAD", "DIR inode %llu heap[%d] 区间越界 bmp[%llu,+%llu) data[%llu,+%llu)",
                      (unsigned long long)inum, s, (unsigned long long)bmpb, (unsigned long long)bmp_blks,
                      (unsigned long long)datab, (unsigned long long)data_blks);
            continue;
        }
        for (uint64_t k = 0; k < bmp_blks; k++)  audit_claim(ctx, bmpb + k, inum, "heap_bmp");
        for (uint64_t k = 0; k < data_blks; k++) audit_claim(ctx, datab + k, inum, "heap_data");

        std::vector<uint8_t> bm((size_t)bmp_blks * 4096);
        if (read_buff(bmpb, bmp_blks, bm.data()) != RC_OK) {
            ctx.heap_bad++;
            audit_log(ctx, "BAD", "DIR inode %llu heap[%d] 位图读取失败", (unsigned long long)inum, s);
            continue;
        }
        uint64_t used = 0;
        for (uint8_t v : bm) used += (uint64_t)__builtin_popcount(v);
        const uint64_t total_bits = 1ULL << bits;
        ctx.heap_occ.push_back(audit_heap_occ_t{
            inum, (uint8_t)s, gran, bits, bmpb, datab, used, total_bits });
        audit_log(ctx, "HEAP", "DIR inode %llu heap[%d] gran=%uB bits_log2=%u bmp=[%llu,+%llu) data=[%llu,+%llu) 占用 %llu/%llu (%.1f%%)",
                  (unsigned long long)inum, s, 1u << gran, bits,
                  (unsigned long long)bmpb, (unsigned long long)bmp_blks,
                  (unsigned long long)datab, (unsigned long long)data_blks,
                  (unsigned long long)used, (unsigned long long)total_bits,
                  total_bits ? 100.0 * (double)used / (double)total_bits : 0.0);

        audit_heap_region_t r;
        r.data_lo = datab; r.data_hi = datab + data_blks;
        r.gran_log2 = gran; r.bmp_base = bmpb; r.total_bits = total_bits;
        ctx.cur_heaps.push_back(r);
    }
}

int RavenFS_juvenile::audit_heap_bit(const audit_heap_region_t &r, uint64_t bit)
{
    if (bit >= r.total_bits) return -1;
    const uint64_t byte  = bit >> 3;
    const uint64_t blk   = r.bmp_base + byte / 4096;
    const uint64_t inblk = byte % 4096;
    if (blk >= total_blocks) return -1;
    alignas(4096) uint8_t buf[4096];
    if (read_buff(blk, 1, buf) != RC_OK) return -1;
    return (buf[inblk] >> (bit & 7)) & 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_filesystem — 驱动 + 收口对账
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::audit_filesystem(const audit_opts_t &opts, audit_report_t &rep)
{
    audit_ctx_t ctx;
    result_code_t brc = audit_begin(ctx, opts);
    if (brc != RC_OK) return brc;
    rep = audit_report_t{};

    /* ── 枚举全部已分配 inode，逐个审计 ── */
    for (uint64_t inum = 0; inum < inode_count; inum++) {
        if (!inode_test(inum)) continue;
        rep.inodes_scanned++;
        audit_inode(inum, ctx);
    }

    /* ── 块对账：claim_bits ↔ block bitmap ── */
    if (!ctx.opts || ctx.opts->check_bitmap) {
        for (uint64_t blk = 0; blk < total_blocks; blk++) {
            if (blk >= ctx.meta_tail_lo) continue;      /* 尾巴三段：结构块，跳过 */
            if (is_backup_blk(ctx, blk)) continue;      /* backup SB：亮且无 owner */
            const bool lit     = block_test(blk);
            const bool claimed = ctx.claim_test(blk);
            if (claimed && !lit) {
                ctx.leaked_blocks++;
                audit_log(ctx, "LEAK", "block %llu 被认领但 bitmap 未亮", (unsigned long long)blk);
            } else if (lit && !claimed) {
                ctx.orphan_blocks++;
            }
        }
    }

    /* ── inode 对账：可达性 + link_count ── */
    for (uint64_t inum = 0; inum < inode_count; inum++) {
        const bool lit = inode_test(inum);
        const uint32_t refs = ctx.ref_count[inum];
        if (lit && refs == 0 && inum != root_inum) {
            ctx.orphan_inodes++;
            audit_log(ctx, "LEAK", "inode %llu 已分配但不可达（孤儿）", (unsigned long long)inum);
        }
        if (lit) {
            raven_Inode_t ino; inode_read(inum, &ino);
            if (ino.type != INODE_TYPE_DIR) {   /* DIR 的 link_count==size+2 已在 audit_dir_inode 查 */
                if ((uint32_t)ino.link_count != refs) {
                    ctx.link_mismatch++; ctx.flag_set(inum);
                    audit_log(ctx, "BAD", "inode %llu link_count=%u != dentry 引用数 %u",
                              (unsigned long long)inum, ino.link_count, refs);
                }
            }
        }
    }

    /* ── dir '..' 对账：dotdot_of vs parent_of ── */
    for (uint64_t inum = 0; inum < inode_count; inum++) {
        const uint64_t dd = ctx.dotdot_of[inum];
        if (dd == PTR_INVALID) continue;   /* 非受审目录 */
        if (inum == root_inum) {
            if (dd != root_inum) {
                ctx.dotdot_mismatch++;
                audit_log(ctx, "BAD", "root dir '..' =%llu != 自身 %llu",
                          (unsigned long long)dd, (unsigned long long)root_inum);
            }
        } else {
            const uint64_t p = ctx.parent_of[inum];
            if (p != PTR_INVALID && dd != p) {
                ctx.dotdot_mismatch++;
                audit_log(ctx, "BAD", "DIR inode %llu '..' =%llu != 实际父 %llu",
                          (unsigned long long)inum, (unsigned long long)dd, (unsigned long long)p);
            }
        }
    }

    /* ── 填报表 ── */
    rep.nodes = ctx.nodes; rep.inner = ctx.inner; rep.leaves = ctx.leaves; rep.records = ctx.records;
    rep.btree_bad = ctx.btree_bad;
    rep.inode_bad = ctx.inode_bad;
    rep.double_claim = ctx.double_claim;
    rep.leaked_blocks = ctx.leaked_blocks;
    rep.orphan_blocks = ctx.orphan_blocks;
    rep.dangling_refs = ctx.dangling_refs;
    rep.orphan_inodes = ctx.orphan_inodes;
    rep.link_mismatch = ctx.link_mismatch;
    rep.dotdot_mismatch = ctx.dotdot_mismatch;
    rep.heap_bad = ctx.heap_bad;
    uint64_t flagged = 0;
    for (uint8_t b : ctx.inode_flag) flagged += (uint64_t)__builtin_popcount(b);
    rep.inodes_flagged = flagged;

    audit_log(ctx, "SUM",
              "inodes=%llu flagged=%llu | btree_bad=%llu inode_bad=%llu double=%llu "
              "leak_blk=%llu orphan_blk=%llu | dangling=%llu orphan_ino=%llu "
              "link=%llu dotdot=%llu heap=%llu | nodes=%llu inner=%llu leaves=%llu recs=%llu",
              (unsigned long long)rep.inodes_scanned, (unsigned long long)rep.inodes_flagged,
              (unsigned long long)rep.btree_bad, (unsigned long long)rep.inode_bad,
              (unsigned long long)rep.double_claim, (unsigned long long)rep.leaked_blocks,
              (unsigned long long)rep.orphan_blocks, (unsigned long long)rep.dangling_refs,
              (unsigned long long)rep.orphan_inodes, (unsigned long long)rep.link_mismatch,
              (unsigned long long)rep.dotdot_mismatch, (unsigned long long)rep.heap_bad,
              (unsigned long long)rep.nodes, (unsigned long long)rep.inner,
              (unsigned long long)rep.leaves, (unsigned long long)rep.records);
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * audit.cpp — RavenFS 可达性审计（新模型，2026-09-30 重写）
 *
 * 设计基线：本次会话口述口径（Raven 拟、Sparrow 确认）。
 * 取代：旧「枚举全 inode + 侧账本反推」模型（Docs/audit_inode_design.md 已淘汰）。
 *
 * 原则：
 *   1. 进上帝类的模块：实现在本 TU，是 RavenFS_juvenile 的成员方法；
 *      复用其位图（block_test/inode_test）+ 缓存（mdata_piece_session）+ 几何。
 *   2. 复用存储设施，不复用导航逻辑：DFS / 路由校验自己重新推导，
 *      绝不调 dentry_Bptree_walk / extent_Bptree_walk（防共因失效）。
 *   3. 统一坐标系：**从 root 先序 DFS 下钻**（可达性由递归携带）。
 *   4. 两张影子图（块 / inode）边走边重画；收口与真实位图 XOR 对账。
 *   5. 只读、不修复；全部经 ctx.out（默认 stdout）打印。
 *
 * 前提：mmap 后端（is_device_direct=false）——offload 缓存直映窗口，
 *       会话指针稳定（跨递归/嵌套不失效）。非 mmap → RC_ERR_STATE。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/ravenfs_audit.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <utility>
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

/* 名字数据面映射树节点魔数自识别（独立于 God is_extent_node_kind） */
inline bool nd_node_magic(const Bptree_meta_t *m)
{
    return m->magics[0] == NAMEDATA_EXTENT_MAGIC1 &&
           m->magics[1] == NAMEDATA_EXTENT_MAGIC2 &&
           m->magics[2] == NAMEDATA_EXTENT_MAGIC3 &&
           m->magics[3] == NAMEDATA_EXTENT_MAGIC4;
}

/* 名字数据面空闲区间树（fr）节点魔数自识别 */
inline bool fr_node_magic(const Bptree_meta_t *m)
{
    return m->magics[0] == NAMEDATA_FREERANGES_MAGIC1 &&
           m->magics[1] == NAMEDATA_FREERANGES_MAGIC2 &&
           m->magics[2] == NAMEDATA_FREERANGES_MAGIC3 &&
           m->magics[3] == NAMEDATA_FREERANGES_MAGIC4;
}

/* fr 复合键 (len 主序, base 副序) 比较：a < b / a > b */
inline bool fr_key_lt(uint64_t alen, uint64_t abase, uint64_t blen, uint64_t bbase)
{ return alen < blen || (alen == blen && abase < bbase); }
inline bool fr_key_gt(uint64_t alen, uint64_t abase, uint64_t blen, uint64_t bbase)
{ return blen < alen || (blen == alen && bbase < abase); }

constexpr uint16_t FR_ORDER = 337;   /* fr 树阶（数组 338 = 337 階 + 1 瞬时槽） */

constexpr uint32_t XATTR_MAGIC = 0x52465358u;   /* "RFSX"（v1.2 §5.2） */

/* ── 日志（全部写 ctx.out；默认 stdout）── */
void audit_vlog(audit_walk_ctx_t &ctx, const char *lvl, const char *fmt, va_list ap)
{
    FILE *out = ctx.out ? ctx.out : stdout;
    char line[1400];
    int n = snprintf(line, sizeof(line), "[AUDIT][%s] ", lvl);
    if (n < 0) n = 0;
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    fputs(line, out);
    fputc('\n', out);
}
void audit_logf(audit_walk_ctx_t &ctx, const char *lvl, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); audit_vlog(ctx, lvl, fmt, ap); va_end(ap);
}

/* 分类记账 + 打印 */
void bad_btree(audit_walk_ctx_t &ctx, const char *fmt, ...)
{
    ctx.rep.btree_bad++;
    va_list ap; va_start(ap, fmt); audit_vlog(ctx, "BAD", fmt, ap); va_end(ap);
}
void bad_inode(audit_walk_ctx_t &ctx, const char *fmt, ...)
{
    ctx.rep.inode_bad++;
    va_list ap; va_start(ap, fmt); audit_vlog(ctx, "BAD", fmt, ap); va_end(ap);
}
void bad_name_data(audit_walk_ctx_t &ctx, const char *fmt, ...)
{
    ctx.rep.heap_bad++;
    va_list ap; va_start(ap, fmt); audit_vlog(ctx, "BAD_NAMEDATA", fmt, ap); va_end(ap);
}
void bad_xattr(audit_walk_ctx_t &ctx, const char *fmt, ...)
{
    ctx.rep.xattr_bad++;
    va_list ap; va_start(ap, fmt); audit_vlog(ctx, "BAD_XATTR", fmt, ap); va_end(ap);
}

}   /* namespace */

/* ═══════════════════════════════════════════════════════════════════
 * audit_node_read — 读一个 8K 树节点到调用方缓冲（8192B，供递归期解耦会话）
 * ═══════════════════════════════════════════════════════════════════ */
bool RavenFS_juvenile::audit_node_read(uint64_t blk, void *dst)
{
    mdata_piece_session s(mdata_piece_mgr, blk);
    if (s.index_error() || !s) return false;
    std::memcpy(dst, s.data, 8192);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_dfs_root — 全盘可达性审计主入口（public）
 *   1. 白名单（尾巴三段 + backup SB 链）
 *   2. 从 root 先序 DFS（重画两张影子图）
 *   3. 收口 XOR（影子 ↔ 真实位图）
 * ═══════════════════════════════════════════════════════════════════ */
result_code_t RavenFS_juvenile::audit_dfs_root(audit_walk_report_t *rep, FILE *out, bool verbose)
{
    if (fd < 0) return RC_ERR_STATE;
    if (is_device_direct || !fsblk0_base_)
        return RC_ERR_STATE;   /* 前提：mmap 后端（offload 直映窗口） */
    if (!out) out = stdout;

    audit_walk_ctx_t ctx;
    ctx.fs = this;
    ctx.out = out;
    ctx.verbose = verbose;
    ctx.blk_bits = total_blocks;
    ctx.ino_bits = inode_count;

    /* 影子图尺寸 = 真实位图字节尺寸（便于收口按字节 XOR） */
    ctx.shadow_blk.assign((size_t)bitmap_blocks * 4096, 0);
    ctx.shadow_ino.assign((size_t)inode_bm_blocks * 4096, 0);

    /* 结构块白名单：尾巴三段 [inode_bm_base, total_blocks) 常亮且无 owner */
    ctx.meta_lo = inode_bm_base;
    ctx.meta_hi = total_blocks;

    /* backup SB 块链（只读遍历；不修改任何东西） */
    ctx.backup_blocks.clear();
    {
        uint64_t blk = sb.backup_next;
        int guard = 0;
        while (blk != PTR_INVALID && !ptr52_bad(blk) && blk < total_blocks &&
               guard < (int)sb.backup_count + 8) {
            ctx.backup_blocks.push_back(blk);
            alignas(4096) uint8_t buf[4096];
            if (read_buff(blk, 1, buf) != RC_OK) break;
            RavenSuperblock bs; std::memcpy(&bs, buf, sizeof(bs));
            uint64_t nxt = bs.backup_next;
            if (nxt == PTR_INVALID || ptr52_bad(nxt) || nxt == blk || nxt >= total_blocks) break;
            blk = nxt; guard++;
        }
    }

    audit_logf(ctx, "START",
               "root=%llu total_blocks=%llu inode_count=%llu | 白名单 meta[%llu,%llu) backup=%zu",
               (unsigned long long)root_inum, (unsigned long long)total_blocks,
               (unsigned long long)inode_count,
               (unsigned long long)ctx.meta_lo, (unsigned long long)ctx.meta_hi,
               ctx.backup_blocks.size());

    /* ── 从 root 先序 DFS（重画两张影子图）── */
    audit_walk_inode(ctx, root_inum, PTR_INVALID);

    /* ── 收口 XOR ── */
    {
        std::vector<std::pair<uint64_t, uint64_t>> run_unprot, run_leak;

        /* 块：只对 [0, meta_lo) 对账；按字节扫描（干净盘零差异 → 极快） */
        const uint64_t lim = std::min<uint64_t>(ctx.meta_lo, total_blocks);
        auto class_blk = [&](uint64_t blk) {
            if (ctx.is_backup_blk(blk)) return;
            const bool reach = ctx.blk_test(blk);
            const bool lit   = block_test(blk);
            if (reach == lit) return;
            if (reach) ctx.rep.unprotected_blk++; else ctx.rep.leaked_blk++;
            auto &runs = reach ? run_unprot : run_leak;
            if (!runs.empty() && runs.back().first + runs.back().second == blk)
                runs.back().second++;
            else
                runs.emplace_back(blk, 1);
        };
        uint64_t i = 0;
        for (; i + 8 <= lim; i += 8) {
            const uint64_t half = i >> 3;
            uint8_t x = (uint8_t)(ctx.shadow_blk[half] ^ block_bits[half]);
            while (x) {
                const int b = __builtin_ctz(x);
                x &= (uint8_t)(x - 1);
                class_blk(i + (uint64_t)b);
            }
        }
        for (; i < lim; i++)
            if (ctx.blk_test(i) != block_test(i)) class_blk(i);

        /* inode：全量 [0, inode_count) */
        for (uint64_t inum = 0; inum < inode_count; inum++) {
            const bool reach = ctx.ino_test(inum);
            const bool lit   = inode_test(inum);
            if (reach == lit) continue;
            if (reach) ctx.rep.unprotected_ino++; else ctx.rep.leaked_ino++;
            if (!reach) {  /* 泄露 inode 逐条打印（量通常极小） */
                /* 直接逐条打印 inode（量通常极小） */
                audit_logf(ctx, "LEAK_INO", "inode %llu 已分配但不可达（疑似泄露/孤儿）",
                           (unsigned long long)inum);
            } else {
                audit_logf(ctx, "UNPROT_INO", "inode %llu 可达但位图未亮（没有被保护）",
                           (unsigned long long)inum);
            }
        }

        auto print_runs = [&](const char *tag, const char *why,
                              const std::vector<std::pair<uint64_t, uint64_t>> &runs) {
            size_t shown = 0;
            for (const auto &r : runs) {
                if (shown++ >= 64) {
                    audit_logf(ctx, tag, "... 其余 %zu 段省略（共 %zu 段）", runs.size() - 64, runs.size());
                    break;
                }
                audit_logf(ctx, tag, "块区间 [%llu, +%llu)  %s",
                           (unsigned long long)r.first, (unsigned long long)r.second, why);
            }
        };
        print_runs("UNPROT_BLK", "没有被保护（可达 ∧ 位图暗）", run_unprot);
        print_runs("LEAK_BLK",   "疑似泄露（位图亮 ∧ 不可达）", run_leak);
    }

    audit_logf(ctx, "NAMEDATA_SUM",
               "active_dirs=%llu | data_blk=%llu tree_node_blk=%llu | pieces=%llu",
               (unsigned long long)ctx.rep.heap_active,
               (unsigned long long)ctx.rep.heap_active_data_blk,
               (unsigned long long)ctx.rep.heap_active_bitmap_blk,
               (unsigned long long)ctx.rep.heap_used_bits);

    audit_logf(ctx, "SUM",
               "inodes=%llu | nodes=%llu inner=%llu leaves=%llu recs=%llu | "
               "btree_bad=%llu inode_bad=%llu heap_bad=%llu xattr_bad=%llu "
               "multi_ref=%llu multi_claim=%llu link=%llu dotdot=%llu | "
               "没有被保护: blk=%llu ino=%llu | 疑似泄露: blk=%llu ino=%llu | bad_total=%llu",
               (unsigned long long)ctx.rep.inodes_walked,
               (unsigned long long)ctx.rep.nodes, (unsigned long long)ctx.rep.inner,
               (unsigned long long)ctx.rep.leaves, (unsigned long long)ctx.rep.records,
               (unsigned long long)ctx.rep.btree_bad, (unsigned long long)ctx.rep.inode_bad,
               (unsigned long long)ctx.rep.heap_bad, (unsigned long long)ctx.rep.xattr_bad,
               (unsigned long long)ctx.rep.multi_ref_ino, (unsigned long long)ctx.rep.multi_claim_blk,
               (unsigned long long)ctx.rep.link_mismatch, (unsigned long long)ctx.rep.dotdot_mismatch,
               (unsigned long long)ctx.rep.unprotected_blk, (unsigned long long)ctx.rep.unprotected_ino,
               (unsigned long long)ctx.rep.leaked_blk, (unsigned long long)ctx.rep.leaked_ino,
               (unsigned long long)ctx.rep.total_bad());

    if (rep) *rep = ctx.rep;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_walk_inode — 单 inode 访问（先序：置影子 → 校验 → 交给 dir/file → 下钻）
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_walk_inode(audit_walk_ctx_t &ctx, uint64_t inum, uint64_t parent_inum)
{
    if (ctx.depth > ctx.max_depth) {
        bad_inode(ctx, "inum=%llu 递归深度超限 %d（路径 %s）",
                  (unsigned long long)inum, ctx.depth, audit_path_str(ctx).c_str());
        return;
    }
    if (inum >= inode_count) {
        bad_inode(ctx, "引用越界 inode=%llu（父=%llu）", (unsigned long long)inum,
                  (unsigned long long)parent_inum);
        return;
    }
    /* 影子图 B：先置可达位（供 XOR 对账）；已置 → 二次触达（环/共享/双引用） */
    if (!ctx.ino_set(inum)) {
        ctx.rep.multi_ref_ino++;
        audit_logf(ctx, "MULTI_REF", "inode %llu 二次触达（环/共享/硬链接/双引用）父=%llu",
                   (unsigned long long)inum, (unsigned long long)parent_inum);
        return;
    }

    raven_Inode_t raw;
    inode_read(inum, &raw);
    const uint32_t type = RFS_INO1_GET_TYPE(raw.word1);

    ctx.path_inum.push_back(inum);
    ctx.depth++;
    ctx.rep.inodes_walked++;
    ctx.path_name.push_back(type == INODE_TYPE_DIR
                            ? (inum == root_inum ? std::string() : std::string("<dir>"))
                            : std::string("<file>"));

    {
        const uint64_t lc = raw.link_count;   /* 前 96B 同构 */
        if (ctx.verbose)
            audit_logf(ctx, "WALK", "%s inum=%llu type=%u size=%llu lc=%llu",
                       audit_path_str(ctx).c_str(), (unsigned long long)inum,
                       type, (unsigned long long)raw.size, (unsigned long long)lc);
    }

    switch (type) {
    case INODE_TYPE_DIR:
        audit_walk_dir(ctx, inum, parent_inum);
        break;
    case INODE_TYPE_REGULAR:
    case INODE_TYPE_SOFT_SYM:
        audit_walk_file(ctx, inum, &raw);
        break;
    case INODE_TYPE_FIFO:
    case INODE_TYPE_CHR:
    case INODE_TYPE_BLK:
    case INODE_TYPE_SOCK:
        /* 不变量 26：无数据类型 ⇒ size==0 ∧ data_root==PTR_INVALID ∧ 无树 */
        if (raw.size != 0 || raw.data_root != PTR_INVALID)
            bad_inode(ctx, "无数据类型 inode %llu type=%u 违反不变量 26 (size=%llu data_root=%#llx)",
                      (unsigned long long)inum, type, (unsigned long long)raw.size,
                      (unsigned long long)raw.data_root);
        break;
    default:
        bad_inode(ctx, "inode %llu 非法 type=%u", (unsigned long long)inum, type);
        break;
    }

    /* xattr（单块；v1.2 §5.5） */
    {
        const uint64_t xptr = (raw.mode_xattr >> 12) & PTR52;
        if (xptr != RFS_XATTR_NONE) {
            if (xptr >= total_blocks) {
                bad_xattr(ctx, "inode %llu xattr_ptr=%#llx 越界", (unsigned long long)inum,
                          (unsigned long long)xptr);
            } else {
                if (!ctx.blk_set(xptr)) {
                    ctx.rep.multi_claim_blk++;
                    audit_logf(ctx, "MULTI_CLAIM", "xattr 块 %llu 二次认领 (inode %llu)",
                               (unsigned long long)xptr, (unsigned long long)inum);
                }
                alignas(4096) uint8_t buf[4096];
                if (read_buff(xptr, 1, buf) == RC_OK) {
                    uint32_t magic; std::memcpy(&magic, buf, 4);
                    uint32_t rc1;   std::memcpy(&rc1, buf + 4, 4);
                    uint32_t hblk;  std::memcpy(&hblk, buf + 8, 4);
                    if (magic != XATTR_MAGIC)
                        bad_xattr(ctx, "inode %llu xattr 块 %llu magic=%#x 不符",
                                  (unsigned long long)inum, (unsigned long long)xptr, magic);
                    if (rc1 != 1 || hblk != 1)
                        bad_xattr(ctx, "inode %llu xattr 块 %llu refcount=%u blocks=%u（v1.2 应 1/1）",
                                  (unsigned long long)inum, (unsigned long long)xptr, rc1, hblk);
                }
            }
        }
    }

    ctx.depth--;
    ctx.path_inum.pop_back();
    ctx.path_name.pop_back();
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_walk_dir — DIR 字段校验 + 名字数据面 + dentry 树 DFS + 子目录下钻
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_walk_dir(audit_walk_ctx_t &ctx, uint64_t inum, uint64_t parent_inum)
{
    raven_Inode_t raw;
    inode_read(inum, &raw);
    const raven_dir_Inode_t *d = (const raven_dir_Inode_t *)&raw;

    /* 目录名字数据面（v1.3：映射 direct/tree + fr）独立审计 + 全物理块认领。
     * 先于 ddentry 根校验：即使 dentry 树根损坏也要认领数据面块，避免误报泄露。 */
    audit_name_data_plane(ctx, inum, d);

    uint64_t root = d->ddentry_Bptree_root;
    if (root == PTR_INVALID || root >= total_blocks || (root & 1)) {
        bad_inode(ctx, "DIR inode %llu ddentry_Bptree_root 非法=%#llx", (unsigned long long)inum,
                  (unsigned long long)root);
        return;
    }

    /* dentry 树 DFS（先序：本目录树先审完，再下钻子目录） */
    std::vector<uint64_t> children;
    audit_walk_tree_t t;
    bool ok = true;
    audit_walk_btree(ctx, children, t, root, inum, inum /*root parent=owner*/,
                     parent_inum == PTR_INVALID ? root_inum : parent_inum /*'..' 期望*/,
                     true, true, 0, 0, ok);
    audit_leaf_chain(ctx, t, root, true, inum);

    /* size == 实际 dentry 条目数 */
    if (t.records_total != d->size)
        bad_inode(ctx, "DIR inode %llu size=%llu != 实际 dentry 条目 %llu",
                  (unsigned long long)inum, (unsigned long long)d->size,
                  (unsigned long long)t.records_total);

    /* link_count 校验（D-12 新契约）：POSIX 语义 link_count = 2 + 直属 DIR 型
     * 子 inode 数（与 size 彻底解耦；size 仍 = 全部 dentry 条目数，见上）。
     * children 为 audit_walk_btree 收集的直属子 inode 列表（可能含重复，按条目
     * 出现次数计；重复本身由 MULTI_REF 另行捕获，本轮不改既有行为）。 */
    {
        uint64_t k = 0;
        for (uint64_t c : children) {
            if (c >= inode_count) continue;   /* 越界已由 audit_walk_btree 报 bad */
            raven_Inode_t craw;
            inode_read(c, &craw);
            if (RFS_INO1_GET_TYPE(craw.word1) == INODE_TYPE_DIR) k++;
        }
        const uint64_t expect_lc = k + 2;
        if ((uint64_t)d->link_count != expect_lc)
            bad_inode(ctx, "DIR inode %llu link_count=%u 期望=%llu (子目录数 %llu)",
                      (unsigned long long)inum, d->link_count,
                      (unsigned long long)expect_lc, (unsigned long long)k);
    }

    /* 先序下钻子目录（子目录 inode 由本目录 dentry 条目指向） */
    for (uint64_t c : children)
        audit_walk_inode(ctx, c, inum);
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_walk_file — REG / LNK（size 优先解码；inline 忽视 direct_bit）
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_walk_file(audit_walk_ctx_t &ctx, uint64_t inum, const raven_Inode_t *ino)
{
    /* link_count：非 DIR 通用契约 = 指向本 inode 的 dentry 条目数。
     * 幼年期无硬链接 ⇒ 可达即应恰 1（多次触达由 MULTI_REF 捕获）。 */
    if (ino->link_count == 0)
        ctx.rep.link_mismatch++, audit_logf(ctx, "WARN",
            "inode %llu link_count==0 但可达", (unsigned long long)inum);
    else if (ino->link_count > 1)
        audit_logf(ctx, "WARN", "inode %llu link_count=%u >1（幼年期疑多引用/硬链接）",
                   (unsigned long long)inum, ino->link_count);

    if (ino->size <= 160) {
        /* inline：只依 size；direct_bit 显式忽视；data_root 残留 = 黄牌 */
        if (ino->data_root != PTR_INVALID)
            audit_logf(ctx, "WARN", "inode %llu size=%llu inline 但 data_root 残留=%#llx（黄牌）",
                       (unsigned long long)inum, (unsigned long long)ino->size,
                       (unsigned long long)ino->data_root);
        return;
    }

    const uint64_t blocks = (ino->size + 4095) / 4096;
    if (RFS_INO1_GET_DIRECT(ino->word1)) {
        uint64_t base = ino->data_root;
        if (base == PTR_INVALID || base >= total_blocks || base + blocks > total_blocks) {
            bad_inode(ctx, "inode %llu direct data_root=%#llx blocks=%llu 越界",
                      (unsigned long long)inum, (unsigned long long)base,
                      (unsigned long long)blocks);
            return;
        }
        for (uint64_t i = 0; i < blocks; i++) {
            if (!ctx.blk_set(base + i)) {
                ctx.rep.multi_claim_blk++;
                audit_logf(ctx, "MULTI_CLAIM", "direct 块 %llu 二次认领 (inode %llu)",
                           (unsigned long long)(base + i), (unsigned long long)inum);
            }
        }
    } else {
        uint64_t root = ino->data_root;
        if (root == PTR_INVALID || root >= total_blocks || (root & 1)) {
            bad_inode(ctx, "inode %llu extent tree root 非法=%#llx",
                      (unsigned long long)inum, (unsigned long long)root);
            return;
        }
        std::vector<uint64_t> children;
        audit_walk_tree_t t;
        bool ok = true;
        audit_walk_btree(ctx, children, t, root, inum, inum, inum,
                         true, false, blocks, 0, ok);
        audit_leaf_chain(ctx, t, root, false, inum);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_walk_btree — B+树 DFS 递归核（认领节点块 + 结构不变量；重点）
 *   返回子树 max key（dentry=hash / extent=开边界）；
 *   sub_ok = 本节点读+身份校验通过（父层据此做 key==max 比对）。
 * ═══════════════════════════════════════════════════════════════════ */
uint64_t RavenFS_juvenile::audit_walk_btree(audit_walk_ctx_t &ctx,
        std::vector<uint64_t> &children, audit_walk_tree_t &t, uint64_t blk,
        uint64_t owner_inum, uint64_t expect_parent, uint64_t dotdot_expect,
        bool expect_root, bool is_dentry, uint64_t size_bound, int depth, bool &sub_ok)
{
    sub_ok = false;
    const char *tree = is_dentry ? "dentry" : "extent";
    const uint16_t order = is_dentry ? DENTRY_ORDER : EXTENT_ORDER;
    const uint16_t mina  = is_dentry ? DENTRY_MINA  : EXTENT_MINA;

    if (depth > ctx.max_depth) {
        bad_btree(ctx, "tree=%s inum=%llu depth 超限 %d (blk=%llu)", tree,
                  (unsigned long long)owner_inum, depth, (unsigned long long)blk);
        return 0;
    }
    if (blk == PTR_INVALID || ptr52_bad(blk) || blk >= total_blocks || (blk & 1)) {
        bad_btree(ctx, "tree=%s inum=%llu blk=%llu 非法节点块号", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk);
        return 0;
    }
    /* 节点认领（2 块）：重入 = 环 / 共享子节点 / 物理块双认领 */
    if (!ctx.blk_set(blk)) {
        ctx.rep.multi_claim_blk++;
        audit_logf(ctx, "MULTI_CLAIM", "tree=%s inum=%llu 节点 blk=%llu 二次认领（环/共享）",
                   tree, (unsigned long long)owner_inum, (unsigned long long)blk);
        return 0;
    }
    if (blk + 1 < total_blocks && !ctx.blk_set(blk + 1)) {
        ctx.rep.multi_claim_blk++;
        audit_logf(ctx, "MULTI_CLAIM", "tree=%s inum=%llu 节点尾巴 blk+1=%llu 二次认领",
                   tree, (unsigned long long)owner_inum, (unsigned long long)(blk + 1));
    }

    void *nb = node_buf_alloc();
    if (!nb) {
        bad_btree(ctx, "tree=%s inum=%llu blk=%llu 节点缓冲分配失败", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk);
        return 0;
    }
    if (!audit_node_read(blk, nb)) {
        bad_btree(ctx, "tree=%s inum=%llu blk=%llu 节点读取失败", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk);
        node_buf_free(nb);
        return 0;
    }
    Bptree_meta_t *meta = (Bptree_meta_t *)nb;
    const bool magic_ok = is_dentry ? is_dentry_node(meta) : is_extent_node(meta);
    if (!magic_ok) {
        bad_btree(ctx, "tree=%s inum=%llu blk=%llu magic 不符", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk);
        node_buf_free(nb);
        return 0;
    }

    ctx.rep.nodes++;
    const uint16_t cnt = (uint16_t)RFS_PP_COUNT(meta->parent_ptr);
    const uint16_t it  = std::min<uint16_t>(cnt, (uint16_t)(order + 1));
    const bool     inner = RFS_PP_IS_INNER(meta->parent_ptr);

    /* 角色 / 父指针四态 */
    if ((bool)RFS_PP_IS_ROOT(meta->parent_ptr) != expect_root)
        bad_btree(ctx, "tree=%s inum=%llu blk=%llu is_root=%d 期望 %d", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk,
                  (int)RFS_PP_IS_ROOT(meta->parent_ptr), (int)expect_root);
    if (expect_root) {
        if (RFS_PP_PTR(meta->parent_ptr) != owner_inum)
            bad_btree(ctx, "tree=%s inum=%llu blk=%llu root('.') ptr=%llu != owner %llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk,
                      (unsigned long long)RFS_PP_PTR(meta->parent_ptr), (unsigned long long)owner_inum);
        if (is_dentry) {   /* dir 根节点 ptrs[1] = '..' inode → 对账实际父 */
            const uint64_t dd = RFS_PTR_ADDR(meta->ptrs[1]);
            if (dd != dotdot_expect) {
                ctx.rep.dotdot_mismatch++;
                audit_logf(ctx, "BAD_DOTDOT", "DIR inode %llu '..' =%llu != 实际父 %llu",
                           (unsigned long long)owner_inum, (unsigned long long)dd,
                           (unsigned long long)dotdot_expect);
            }
        }
    } else {
        if (RFS_PP_PTR(meta->parent_ptr) != expect_parent)
            bad_btree(ctx, "tree=%s inum=%llu blk=%llu parent_ptr=%llu != 实际父 %llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk,
                      (unsigned long long)RFS_PP_PTR(meta->parent_ptr), (unsigned long long)expect_parent);
    }
    if (cnt > order)
        bad_btree(ctx, "tree=%s inum=%llu blk=%llu entry_count=%u > 阶 %u（瞬时槽落盘？）", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk, cnt, order);

    /* ══════ 叶子 ══════ */
    if (!inner) {
        ctx.rep.leaves++;
        t.leaves.push_back(blk);
        if (!t.leaf_depth_set) { t.leaf_depth = (uint64_t)depth; t.leaf_depth_set = true; }
        else if ((uint64_t)depth != t.leaf_depth)
            bad_btree(ctx, "tree=%s inum=%llu blk=%llu 叶深度 %d != %llu（不平衡）", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk, depth,
                      (unsigned long long)t.leaf_depth);

        if (cnt == 0) {
            if (!expect_root) bad_btree(ctx, "tree=%s inum=%llu blk=%llu 非根叶空", tree,
                                        (unsigned long long)owner_inum, (unsigned long long)blk);
            else if (!is_dentry) bad_btree(ctx, "tree=%s inum=%llu blk=%llu 根叶空（extent 空树）",
                                           tree, (unsigned long long)owner_inum, (unsigned long long)blk);
        } else if (!expect_root && cnt < mina) {
            bad_btree(ctx, "tree=%s inum=%llu blk=%llu 叶欠载 cnt=%u < %u", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk, cnt, mina);
        }
        sub_ok = true;

        if (is_dentry) {
            auto *leaf = (Bptree_ddentry_leaf_node_t *)nb;
            uint64_t prev_hash = 0; char prev_name[DENTRY_INLINE_NAME_MAX];
            std::memset(prev_name, 0, sizeof(prev_name));
            for (uint16_t i = 0; i < it; i++) {
                const ddentry_t &e = leaf->entries[i];
                if (i > 0) {
                    const bool bad_order = (e.hash < prev_hash) ||
                        (e.hash == prev_hash && std::memcmp(e.name, prev_name, DENTRY_INLINE_NAME_MAX) <= 0);
                    if (bad_order)
                        bad_btree(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] 复合序非严格递增",
                                  (unsigned long long)owner_inum, (unsigned long long)blk, i);
                }
                prev_hash = e.hash;
                std::memcpy(prev_name, e.name, DENTRY_INLINE_NAME_MAX);

                const uint16_t name_len = (uint16_t)RFS_PTR_FREE(e.inode_ptr);
                const uint64_t target   = RFS_PTR_ADDR(e.inode_ptr);

                if (name_len > RAVENFS_MAX_NAME_LEN)
                    bad_btree(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] name_len=%u 超上限 %u",
                              (unsigned long long)owner_inum, (unsigned long long)blk, i,
                              name_len, (unsigned)RAVENFS_MAX_NAME_LEN);

                /* 名字重建（≤40 内联 / >40 经目录名字数据面读）→ hash 重算。
                 * v1.3 名字堆已退役；映射树/fr 树完整校验见 audit_name_data_plane（独立轨）。 */
                char full[RAVENFS_MAX_NAME_LEN + 1];
                const uint16_t ncopy = name_len < DENTRY_INLINE_NAME_MAX ? name_len : DENTRY_INLINE_NAME_MAX;
                std::memcpy(full, e.name, ncopy);
                bool name_ok = true;
                if (name_len > DENTRY_INLINE_NAME_MAX) {
                    if (e.full_char_remain_offset == PTR_INVALID) {
                        name_ok = false;
                        bad_name_data(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] 长名但 offset=PTR_INVALID",
                                 (unsigned long long)owner_inum, (unsigned long long)blk, i);
                    } else if (name_data_read(owner_inum, e.full_char_remain_offset,
                                              (uint16_t)(name_len - DENTRY_INLINE_NAME_MAX),
                                              full + DENTRY_INLINE_NAME_MAX) != RC_OK) {
                        name_ok = false;
                        bad_name_data(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] 长名数据面读取失败 offset=%#llx",
                                 (unsigned long long)owner_inum, (unsigned long long)blk, i,
                                 (unsigned long long)e.full_char_remain_offset);
                    }
                } else if (e.full_char_remain_offset != PTR_INVALID) {
                    audit_logf(ctx, "WARN", "tree=dentry inum=%llu blk=%llu entries[%u] name_len<=40 但 offset 非 INVALID（软）",
                               (unsigned long long)owner_inum, (unsigned long long)blk, i);
                }
                if (name_ok) {
                    full[name_len] = '\0';
                    const uint64_t h = name_hash(full);
                    if (h != e.hash)
                        bad_btree(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] hash 不符 stored=%#llx recalc=%#llx name_len=%u",
                                  (unsigned long long)owner_inum, (unsigned long long)blk, i,
                                  (unsigned long long)e.hash, (unsigned long long)h, name_len);
                }

                /* 目标 inode：越界 = 硬错（悬挂由 XOR 的 unprotected_ino 收口） */
                if (target == PTR52 || target >= inode_count)
                    bad_btree(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] inode_ptr=%llu 越界",
                              (unsigned long long)owner_inum, (unsigned long long)blk, i,
                              (unsigned long long)target);
                else
                    children.push_back(target);
            }
            t.records_total += it;
            ctx.rep.records += it;
            node_buf_free(nb);
            return (it > 0) ? leaf->entries[it - 1].hash : 0;
        }

        /* extent 叶 */
        auto *leaf = (Bptree_extent_leaf_node_t *)nb;
        uint64_t prev_end = 0;
        for (uint16_t i = 0; i < it; i++) {
            const extent_t &e = leaf->extents[i];
            if (e.length == 0)
                bad_btree(ctx, "tree=extent inum=%llu blk=%llu extents[%u] length==0",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i);
            if (i > 0 && prev_end > e.fblkbase)
                bad_btree(ctx, "tree=extent inum=%llu blk=%llu extents[%u] 重叠 (prev_end=%llu > fblkbase=%llu)",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                          (unsigned long long)prev_end, (unsigned long long)e.fblkbase);
            if (e.fblkbase + e.length > size_bound)
                bad_btree(ctx, "tree=extent inum=%llu blk=%llu extents[%u] 逻辑越界 end=%llu > size_bound=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                          (unsigned long long)(e.fblkbase + e.length), (unsigned long long)size_bound);
            const uint64_t pb = RFS_PTR_ADDR(e.block_base);
            if (pb == PTR52 || pb >= total_blocks || pb + e.length > total_blocks) {
                bad_btree(ctx, "tree=extent inum=%llu blk=%llu extents[%u] 物理指针越界 base=%llu len=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                          (unsigned long long)pb, (unsigned long long)e.length);
            } else {
                for (uint64_t k = 0; k < e.length; k++) {
                    if (!ctx.blk_set(pb + k)) {
                        ctx.rep.multi_claim_blk++;
                        audit_logf(ctx, "MULTI_CLAIM", "extent 块 %llu 二次认领 (inode %llu)",
                                   (unsigned long long)(pb + k), (unsigned long long)owner_inum);
                    }
                }
            }
            prev_end = e.fblkbase + e.length;
        }
        t.records_total += it;
        ctx.rep.records += it;
        node_buf_free(nb);
        return prev_end;
    }

    /* ══════ 内部节点 ══════ */
    ctx.rep.inner++;
    if (cnt == 0) {
        bad_btree(ctx, "tree=%s inum=%llu blk=%llu 内部节点空", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk);
        node_buf_free(nb);
        return 0;
    }
    if (expect_root && cnt == 1)
        bad_btree(ctx, "tree=%s inum=%llu blk=%llu 根内部仅 1 孩子（待收缩）", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk);
    if (!expect_root && cnt < mina)
        bad_btree(ctx, "tree=%s inum=%llu blk=%llu 内部欠载 cnt=%u < %u", tree,
                  (unsigned long long)owner_inum, (unsigned long long)blk, cnt, mina);

    sub_ok = true;
    /* 先把 hint 拷出（递归解耦会话/缓冲），再逐个下钻 */
    std::vector<std::pair<uint64_t, uint64_t>> hints(it);   /* (child, key) */
    for (uint16_t i = 0; i < it; i++) {
        if (is_dentry) {
            auto *nd = (Bptree_ddentry_inner_node_t *)nb;
            hints[i].first  = RFS_PTR_ADDR(nd->hints[i].sub_ptr);
            hints[i].second = nd->hints[i].hash_max;
        } else {
            auto *nd = (Bptree_extent_inner_node_t *)nb;
            hints[i].first  = RFS_PTR_ADDR(nd->extents[i].sub_node_ptr);
            hints[i].second = nd->extents[i].key;
        }
    }
    node_buf_free(nb);

    uint64_t prev_key = 0, ret = 0;
    for (uint16_t i = 0; i < it; i++) {
        const uint64_t child = hints[i].first;
        const uint64_t key   = hints[i].second;
        if (i > 0 && key <= prev_key)
            bad_btree(ctx, "tree=%s inum=%llu blk=%llu hint[%u] key=%llu 非严格递增", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk, i,
                      (unsigned long long)key);
        prev_key = key;
        if (child == PTR52 || child >= total_blocks) {
            bad_btree(ctx, "tree=%s inum=%llu blk=%llu hint[%u] child 指针非法=%llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk, i,
                      (unsigned long long)child);
            continue;
        }
        bool cok = false;
        const uint64_t cmax = audit_walk_btree(ctx, children, t, child, owner_inum, blk,
                                               dotdot_expect, false, is_dentry, size_bound,
                                               depth + 1, cok);
        if (cok && cmax != key)
            bad_btree(ctx, "tree=%s inum=%llu blk=%llu hint[%u] key=%llu != 子树 max=%llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk, i,
                      (unsigned long long)key, (unsigned long long)cmax);
        ret = cok ? cmax : key;
    }
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_leaf_chain — 兄弟链双向自洽 + 链序 == DFS 叶序
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_leaf_chain(audit_walk_ctx_t &ctx, const audit_walk_tree_t &t,
                                        uint64_t root_blk, bool is_dentry, uint64_t owner_inum)
{
    if (t.leaves.size() <= 1) return;
    const char *tree = is_dentry ? "dentry" : "extent";

    /* 从根下钻最左叶 */
    uint64_t blk = root_blk;
    for (int d = 0; d < ctx.max_depth + 1; d++) {
        void *nb = node_buf_alloc();
        if (!nb) return;
        if (!audit_node_read(blk, nb)) { node_buf_free(nb); return; }
        Bptree_meta_t *m = (Bptree_meta_t *)nb;
        if (!RFS_PP_IS_INNER(m->parent_ptr)) { node_buf_free(nb); break; }
        const uint16_t c = (uint16_t)RFS_PP_COUNT(m->parent_ptr);
        if (c == 0) {
            bad_btree(ctx, "tree=%s inum=%llu 叶链下钻遇空内部节点 blk=%llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)blk);
            node_buf_free(nb);
            return;
        }
        blk = is_dentry ? RFS_PTR_ADDR(((Bptree_ddentry_inner_node_t *)nb)->hints[0].sub_ptr)
                        : RFS_PTR_ADDR(((Bptree_extent_inner_node_t *)nb)->extents[0].sub_node_ptr);
        node_buf_free(nb);
    }

    size_t idx = 0;
    uint64_t cur = blk;
    for (int guard = 0; guard < (int)t.leaves.size() + 2; guard++) {
        if (cur == PTR52) break;
        if (idx >= t.leaves.size()) {
            bad_btree(ctx, "tree=%s inum=%llu 兄弟链多出叶子 blk=%llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)cur);
            return;
        }
        if (cur != t.leaves[idx])
            bad_btree(ctx, "tree=%s inum=%llu 兄弟链序 != DFS 叶序 (cur=%llu expect=%llu idx=%zu)",
                      tree, (unsigned long long)owner_inum, (unsigned long long)cur,
                      (unsigned long long)t.leaves[idx], idx);
        void *nb = node_buf_alloc();
        if (!nb) return;
        if (!audit_node_read(cur, nb)) {
            bad_btree(ctx, "tree=%s inum=%llu 叶链节点读取失败 blk=%llu", tree,
                      (unsigned long long)owner_inum, (unsigned long long)cur);
            node_buf_free(nb);
            return;
        }
        Bptree_meta_t *m = (Bptree_meta_t *)nb;
        const uint64_t prev = RFS_PTR_ADDR(m->ptrs[1]);
        const uint64_t next = RFS_PTR_ADDR(m->ptrs[2]);
        node_buf_free(nb);
        if (prev != PTR52) {
            void *pb = node_buf_alloc();
            if (!pb || !audit_node_read(prev, pb)) {
                bad_btree(ctx, "tree=%s inum=%llu 前驱块读取失败 blk=%llu", tree,
                          (unsigned long long)owner_inum, (unsigned long long)prev);
                if (pb) node_buf_free(pb);
            } else {
                if (RFS_PTR_ADDR(((Bptree_meta_t *)pb)->ptrs[2]) != cur)
                    bad_btree(ctx, "tree=%s inum=%llu 兄弟链断裂: blk=%llu.prev=%llu 但前驱.next != blk",
                              tree, (unsigned long long)owner_inum, (unsigned long long)cur,
                              (unsigned long long)prev);
                node_buf_free(pb);
            }
        }
        if (next != PTR52) {
            void *nbb = node_buf_alloc();
            if (!nbb || !audit_node_read(next, nbb)) {
                bad_btree(ctx, "tree=%s inum=%llu 后继块读取失败 blk=%llu", tree,
                          (unsigned long long)owner_inum, (unsigned long long)next);
                if (nbb) node_buf_free(nbb);
            } else {
                if (RFS_PTR_ADDR(((Bptree_meta_t *)nbb)->ptrs[1]) != cur)
                    bad_btree(ctx, "tree=%s inum=%llu 兄弟链断裂: blk=%llu.next=%llu 但后继.prev != blk",
                              tree, (unsigned long long)owner_inum, (unsigned long long)cur,
                              (unsigned long long)next);
                node_buf_free(nbb);
            }
        }
        cur = next; idx++;
    }
    if (idx != t.leaves.size())
        bad_btree(ctx, "tree=%s inum=%llu 兄弟链叶子数 %zu != DFS 叶数 %zu", tree,
                  (unsigned long long)owner_inum, idx, t.leaves.size());
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_name_data_plane — 目录名字数据面（v1.3）独立审计
 *
 * 契约（仅据 include/disk_struct_v1.3.h 推导；不借 God 实现假设）：
 *   · name_data_ctl：[0]=direct(1)/tree(0)，[1:52)=指针（51 位）；
 *       空态 ptr 全 1（RFS_ND_CTL_PTR_INVALID）⇔ piece_count==0。
 *   · direct：ptr = 单连续物理段基块（8K 对齐），段长 = pieces×2 FS_block，
 *       逻辑空间 [0, pieces×2) 由 ptr 线性直算。
 *   · tree  ：ptr = 映射树根块号；节点族魔数 NAMEDATA_EXTENT_MAGIC1..4，
 *       布局/阶同文件 extent 树；key = 子树最大开边界 fblkbase+length。
 *       逻辑空间稠密无空洞、8K 对齐；每数据片 = length 2 的 extent。
 *   · fr（空闲区间树）：name_data_free_ranges_ctl inline(1)/tree(0) + 根块号；
 *       空态 ptr 全 1 ⇔ fr_count==0。tree 态节点族 NAMEDATA_FREERANGES_MAGIC1..4，
 *       复合键 (len 主序, base 副序)，hint=子树最大复合键。独立审计+认领。
 *
 * 认领：数据面占用的**全部物理块**（direct 段 / tree 各 extent + 映射树节点 +
 * fr 树节点）在收口 XOR 前登记进 ctx.blk_set；二次认领计 multi_claim_blk。
 * 所有不变量违例经 bad_name_data → rep.heap_bad。
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_name_data_plane(audit_walk_ctx_t &ctx, uint64_t inum,
                                             const raven_dir_Inode_t *d)
{
    const uint64_t ctl    = d->name_data_ctl;
    const uint64_t pieces = RFS_ND_PIECE_COUNT(d->name_data_piece_count);
    const uint64_t ptr    = RFS_ND_CTL_PTR(ctl);
    const uint64_t target = pieces * 2;   /* 逻辑块数（8K 片 = 2 FS_block） */

    /* fr（空闲区间树）：独立审计 + 节点块认领（先于映射树，防 XOR 误报泄露） */
    audit_name_data_free_ranges(ctx, inum, d);

    /* ── 空态自洽：ptr==INVALID ⇔ pieces==0 ── */
    if (ptr == RFS_ND_CTL_PTR_INVALID) {
        if (pieces != 0)
            bad_name_data(ctx, "DIR inode %llu 数据面空态（ptr 全 1）但 piece_count=%llu",
                          (unsigned long long)inum, (unsigned long long)pieces);
        return;
    }
    if (pieces == 0) {
        bad_name_data(ctx, "DIR inode %llu 数据面非空控制字 ptr=%llu 但 piece_count=0",
                      (unsigned long long)inum, (unsigned long long)ptr);
        /* 不 early return：继续认领（段/树），避免收口把活块误报 leaked */
    }

    ctx.rep.heap_active++;
    ctx.rep.heap_used_bits += pieces;

    /* ── direct 态 ── */
    if (RFS_ND_CTL_IS_DIRECT(ctl)) {
        if (ptr & 1)
            bad_name_data(ctx, "DIR inode %llu 数据面 direct base=%llu 未 8K 对齐",
                          (unsigned long long)inum, (unsigned long long)ptr);
        if (ptr >= total_blocks || ptr + target > total_blocks) {
            bad_name_data(ctx, "DIR inode %llu 数据面 direct 区间越界 base=%llu pieces=%llu total=%llu",
                          (unsigned long long)inum, (unsigned long long)ptr,
                          (unsigned long long)pieces, (unsigned long long)total_blocks);
            return;
        }
        for (uint64_t k = 0; k < target; k++) {
            if (!ctx.blk_set(ptr + k)) {
                ctx.rep.multi_claim_blk++;
                audit_logf(ctx, "MULTI_CLAIM", "name_data direct 块 %llu 二次认领 (inode %llu)",
                           (unsigned long long)(ptr + k), (unsigned long long)inum);
            }
        }
        ctx.rep.heap_active_data_blk += target;
        if (ctx.verbose)
            audit_logf(ctx, "NAMEDATA",
                       "DIR inode %llu direct base=%llu pieces=%llu (%llu blks)",
                       (unsigned long long)inum, (unsigned long long)ptr,
                       (unsigned long long)pieces, (unsigned long long)target);
        return;
    }

    /* ── tree 态：映射树 DFS ── */
    if (ptr & 1)
        bad_name_data(ctx, "DIR inode %llu 数据面 tree 根=%llu 未 8K 对齐",
                      (unsigned long long)inum, (unsigned long long)ptr);

    audit_name_data_tree_t w;
    bool sub_ok = false;
    (void)audit_name_data_tree(ctx, ptr, inum, inum, true, 0, w, sub_ok);
    (void)sub_ok;

    /* 逻辑域终态：无缝稠密覆盖 [0, pieces*2)（跨叶游标已逐条校验） */
    if (w.logical_end != target)
        bad_name_data(ctx, "DIR inode %llu 数据面逻辑覆盖 end=%llu != pieces*2=%llu",
                      (unsigned long long)inum, (unsigned long long)w.logical_end,
                      (unsigned long long)target);

    ctx.rep.heap_active_data_blk += w.data_blocks;
    ctx.rep.heap_active_bitmap_blk += w.tree_blocks;

    /* 叶链双向自洽 + 链序 == DFS 叶序（数据面专用：违例计 heap_bad） */
    audit_name_data_leaf_chain(ctx, w, ptr, inum);
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_name_data_tree — 名字数据面映射树 DFS（独立推导）
 *
 * 仅自识别 NAMEDATA_EXTENT_MAGIC1..4（不调用 God is_extent_node_kind）。
 * 校验：节点 magic/角色（根/内部/叶子）、父指针、叶深一致、叶/内欠载、
 *   hint key 严格递增且 == 子树 max（key=开边界）、每条 extent length≥2 且
 *   为偶、逻辑域跨叶无缝稠密递增、物理段 8K 对齐且不越界；认领节点块
 *   （2 块/节点）与各 extent 物理块（二次认领计 multi_claim_blk）。
 * 返回：本子树最大开边界（叶子末项 end / 内部末 hint key）。
 * ═══════════════════════════════════════════════════════════════════ */
uint64_t RavenFS_juvenile::audit_name_data_tree(audit_walk_ctx_t &ctx, uint64_t blk,
        uint64_t owner_inum, uint64_t expect_parent, bool expect_root, int depth,
        audit_name_data_tree_t &w, bool &sub_ok)
{
    sub_ok = false;
    if (depth > ctx.max_depth) {
        bad_name_data(ctx, "name_data tree inum=%llu depth 超限 %d (blk=%llu)",
                      (unsigned long long)owner_inum, depth, (unsigned long long)blk);
        return 0;
    }
    if (blk == PTR_INVALID || ptr52_bad(blk) || blk >= total_blocks ||
        (blk & 1) || blk + 1 >= total_blocks) {
        bad_name_data(ctx, "name_data tree inum=%llu blk=%llu 非法节点块号",
                      (unsigned long long)owner_inum, (unsigned long long)blk);
        return 0;
    }
    if (!ctx.blk_set(blk)) {
        ctx.rep.multi_claim_blk++;
        audit_logf(ctx, "MULTI_CLAIM",
                   "name_data tree inum=%llu 节点 blk=%llu 二次认领（环/共享）",
                   (unsigned long long)owner_inum, (unsigned long long)blk);
        return 0;
    }
    w.tree_blocks += 2;   /* 节点恒 8K = 2 FS_block */
    if (!ctx.blk_set(blk + 1)) {
        ctx.rep.multi_claim_blk++;
        audit_logf(ctx, "MULTI_CLAIM", "name_data tree inum=%llu 节点尾巴 blk+1=%llu 二次认领",
                   (unsigned long long)owner_inum, (unsigned long long)(blk + 1));
    }

    void *nb = node_buf_alloc();
    if (!nb) {
        bad_name_data(ctx, "name_data tree inum=%llu blk=%llu 节点缓冲分配失败",
                      (unsigned long long)owner_inum, (unsigned long long)blk);
        return 0;
    }
    if (!audit_node_read(blk, nb)) {
        bad_name_data(ctx, "name_data tree inum=%llu blk=%llu 节点读取失败",
                      (unsigned long long)owner_inum, (unsigned long long)blk);
        node_buf_free(nb);
        return 0;
    }
    Bptree_meta_t *meta = (Bptree_meta_t *)nb;
    if (!nd_node_magic(meta)) {
        bad_name_data(ctx, "name_data tree inum=%llu blk=%llu magic 不符（非 NAMEDATA_EXTENT）",
                      (unsigned long long)owner_inum, (unsigned long long)blk);
        node_buf_free(nb);
        return 0;
    }

    const uint16_t cnt   = (uint16_t)RFS_PP_COUNT(meta->parent_ptr);
    const uint16_t it    = std::min<uint16_t>(cnt, (uint16_t)(EXTENT_ORDER + 1));
    const bool     inner = RFS_PP_IS_INNER(meta->parent_ptr);

    if (cnt > EXTENT_ORDER)
        bad_name_data(ctx, "name_data tree inum=%llu blk=%llu entry_count=%u > 阶 %u",
                      (unsigned long long)owner_inum, (unsigned long long)blk,
                      cnt, (unsigned)EXTENT_ORDER);
    if ((bool)RFS_PP_IS_ROOT(meta->parent_ptr) != expect_root)
        bad_name_data(ctx, "name_data tree inum=%llu blk=%llu is_root=%d 期望 %d",
                      (unsigned long long)owner_inum, (unsigned long long)blk,
                      (int)RFS_PP_IS_ROOT(meta->parent_ptr), (int)expect_root);
    if (expect_root) {
        /* 映射树根 parent_ptr.ptr = owner dir inode（"." 审计锚，同文件 extent 树） */
        if (RFS_PP_PTR(meta->parent_ptr) != owner_inum)
            bad_name_data(ctx, "name_data tree inum=%llu 根 blk=%llu ptr=%llu != owner %llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk,
                          (unsigned long long)RFS_PP_PTR(meta->parent_ptr),
                          (unsigned long long)owner_inum);
    } else {
        if (RFS_PP_PTR(meta->parent_ptr) != expect_parent)
            bad_name_data(ctx, "name_data tree inum=%llu blk=%llu parent_ptr=%llu != 实际父 %llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk,
                          (unsigned long long)RFS_PP_PTR(meta->parent_ptr),
                          (unsigned long long)expect_parent);
    }

    /* ══════ 叶子 ══════ */
    if (!inner) {
        if (!w.leaf_depth_set) { w.leaf_depth = (uint64_t)depth; w.leaf_depth_set = true; }
        else if ((uint64_t)depth != w.leaf_depth)
            bad_name_data(ctx, "name_data tree inum=%llu blk=%llu 叶深度 %d != %llu（不平衡）",
                          (unsigned long long)owner_inum, (unsigned long long)blk, depth,
                          (unsigned long long)w.leaf_depth);
        w.leaves.push_back(blk);

        if (cnt == 0) {
            if (!expect_root)
                bad_name_data(ctx, "name_data tree inum=%llu blk=%llu 非根叶空",
                              (unsigned long long)owner_inum, (unsigned long long)blk);
            /* 根叶空：pieces 应为 0（覆盖收尾报） */
        } else if (!expect_root && cnt < EXTENT_MINA) {
            bad_name_data(ctx, "name_data tree inum=%llu blk=%llu 叶欠载 cnt=%u < %u",
                          (unsigned long long)owner_inum, (unsigned long long)blk,
                          cnt, (unsigned)EXTENT_MINA);
        }

        auto *leaf = (Bptree_extent_leaf_node_t *)nb;
        uint64_t leaf_max = 0;
        for (uint16_t i = 0; i < it; i++) {
            const extent_t &e = leaf->extents[i];
            const uint64_t beg = e.fblkbase;
            const uint64_t len = e.length;
            const uint64_t end = beg + len;

            if (len < 2 || (len & 1))
                bad_name_data(ctx, "name_data tree inum=%llu blk=%llu extents[%u] length=%llu（须 ≥2 且偶）",
                              (unsigned long long)owner_inum, (unsigned long long)blk, i,
                              (unsigned long long)len);
            if (beg & 1)
                bad_name_data(ctx, "name_data tree inum=%llu blk=%llu extents[%u] fblkbase=%llu 未 8K 对齐",
                              (unsigned long long)owner_inum, (unsigned long long)blk, i,
                              (unsigned long long)beg);

            if (!w.started) {
                w.started = true;
                if (beg != 0)
                    bad_name_data(ctx, "name_data tree inum=%llu 首条 fblkbase=%llu != 0（逻辑域不始于 0）",
                                  (unsigned long long)owner_inum, (unsigned long long)beg);
            } else if (beg < w.logical_end) {
                bad_name_data(ctx, "name_data tree inum=%llu extents[%u] 逻辑重叠 (prev_end=%llu > fblkbase=%llu)",
                              (unsigned long long)owner_inum, i,
                              (unsigned long long)w.logical_end, (unsigned long long)beg);
            } else if (beg > w.logical_end) {
                bad_name_data(ctx, "name_data tree inum=%llu extents[%u] 逻辑空洞 (prev_end=%llu < fblkbase=%llu)",
                              (unsigned long long)owner_inum, i,
                              (unsigned long long)w.logical_end, (unsigned long long)beg);
            }
            w.logical_end = end;
            w.records++;

            const uint64_t pb = RFS_PTR_ADDR(e.block_base);
            if (pb == PTR52 || pb >= total_blocks || (pb & 1) || pb + len > total_blocks) {
                bad_name_data(ctx, "name_data tree inum=%llu blk=%llu extents[%u] 物理段非法 base=%llu len=%llu",
                              (unsigned long long)owner_inum, (unsigned long long)blk, i,
                              (unsigned long long)pb, (unsigned long long)len);
            } else {
                for (uint64_t k = 0; k < len; k++) {
                    if (!ctx.blk_set(pb + k)) {
                        ctx.rep.multi_claim_blk++;
                        audit_logf(ctx, "MULTI_CLAIM", "name_data 数据块 %llu 二次认领 (inode %llu)",
                                   (unsigned long long)(pb + k), (unsigned long long)owner_inum);
                    }
                }
                w.data_blocks += len;
            }
            leaf_max = end;
        }
        sub_ok = true;
        node_buf_free(nb);
        return leaf_max;
    }

    /* ══════ 内部节点 ══════ */
    if (cnt == 0) {
        bad_name_data(ctx, "name_data tree inum=%llu blk=%llu 内部节点空",
                      (unsigned long long)owner_inum, (unsigned long long)blk);
        node_buf_free(nb);
        return 0;
    }
    if (expect_root && cnt == 1)
        bad_name_data(ctx, "name_data tree inum=%llu blk=%llu 根内部仅 1 孩子（待收缩）",
                      (unsigned long long)owner_inum, (unsigned long long)blk);
    if (!expect_root && cnt < EXTENT_MINA)
        bad_name_data(ctx, "name_data tree inum=%llu blk=%llu 内部欠载 cnt=%u < %u",
                      (unsigned long long)owner_inum, (unsigned long long)blk,
                      cnt, (unsigned)EXTENT_MINA);

    sub_ok = true;
    std::vector<std::pair<uint64_t, uint64_t>> hints(it);   /* (child, key) */
    for (uint16_t i = 0; i < it; i++) {
        auto *nd = (Bptree_extent_inner_node_t *)nb;
        hints[i].first  = RFS_PTR_ADDR(nd->extents[i].sub_node_ptr);
        hints[i].second = nd->extents[i].key;
    }
    node_buf_free(nb);

    uint64_t prev_key = 0, ret = 0;
    for (uint16_t i = 0; i < it; i++) {
        const uint64_t child = hints[i].first;
        const uint64_t key   = hints[i].second;
        if (i > 0 && key <= prev_key)
            bad_name_data(ctx, "name_data tree inum=%llu blk=%llu hint[%u] key=%llu 非严格递增",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                          (unsigned long long)key);
        prev_key = key;
        if (child == PTR52 || child >= total_blocks) {
            bad_name_data(ctx, "name_data tree inum=%llu blk=%llu hint[%u] child 指针非法=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                          (unsigned long long)child);
            continue;
        }
        bool cok = false;
        const uint64_t cmax = audit_name_data_tree(ctx, child, owner_inum, blk, false,
                                                   depth + 1, w, cok);
        if (cok && cmax != key)
            bad_name_data(ctx, "name_data tree inum=%llu blk=%llu hint[%u] key=%llu != 子树 max=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                          (unsigned long long)key, (unsigned long long)cmax);
        ret = cok ? cmax : key;
    }
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_name_data_leaf_chain — 名字数据面映射树叶链校验
 *   从根下钻最左叶 → 沿 ptrs[2] 走链，逐节点核对 ptrs[1]/ptrs[2] 双向自洽，
 *   且链序 == DFS 叶序；单叶则要求 prev/next 皆 INVALID。
 *   全部违例经 bad_name_data（heap_bad）；节点须为 NAMEDATA_EXTENT 魔数。
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_name_data_leaf_chain(audit_walk_ctx_t &ctx,
        const audit_name_data_tree_t &w, uint64_t root_blk, uint64_t owner_inum)
{
    auto node_ok = [&](uint64_t b) {
        return !(b == PTR_INVALID || ptr52_bad(b) || b >= total_blocks ||
                 (b & 1) || b + 1 >= total_blocks);
    };

    /* 空树（无叶）：无链可校 */
    if (w.leaves.empty()) return;

    /* 单叶：根叶不应有兄弟 */
    if (w.leaves.size() == 1) {
        void *nb = node_buf_alloc();
        if (!nb) return;
        if (!audit_node_read(w.leaves[0], nb)) {
            bad_name_data(ctx, "name_data 单叶 inum=%llu blk=%llu 读取失败",
                          (unsigned long long)owner_inum, (unsigned long long)w.leaves[0]);
            node_buf_free(nb);
            return;
        }
        Bptree_meta_t *m = (Bptree_meta_t *)nb;
        if (!nd_node_magic(m))
            bad_name_data(ctx, "name_data 单叶 inum=%llu blk=%llu magic 不符",
                          (unsigned long long)owner_inum, (unsigned long long)w.leaves[0]);
        const uint64_t pv = RFS_PTR_ADDR(m->ptrs[1]);
        const uint64_t nx = RFS_PTR_ADDR(m->ptrs[2]);
        if (pv != PTR52 || nx != PTR52)
            bad_name_data(ctx, "name_data 单叶 inum=%llu blk=%llu 兄弟指针非 INVALID (prev=%llu next=%llu)",
                          (unsigned long long)owner_inum, (unsigned long long)w.leaves[0],
                          (unsigned long long)pv, (unsigned long long)nx);
        node_buf_free(nb);
        return;
    }

    /* 从根下钻最左叶 */
    uint64_t blk = root_blk;
    for (int d = 0; d <= ctx.max_depth; d++) {
        if (!node_ok(blk)) return;
        void *nb = node_buf_alloc();
        if (!nb) return;
        if (!audit_node_read(blk, nb)) {
            bad_name_data(ctx, "name_data 叶链下钻读取失败 inum=%llu blk=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk);
            node_buf_free(nb);
            return;
        }
        Bptree_meta_t *m = (Bptree_meta_t *)nb;
        if (!nd_node_magic(m)) {
            bad_name_data(ctx, "name_data 叶链下钻 magic 不符 inum=%llu blk=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk);
            node_buf_free(nb);
            return;
        }
        if (!RFS_PP_IS_INNER(m->parent_ptr)) { node_buf_free(nb); break; }
        const uint16_t c = (uint16_t)RFS_PP_COUNT(m->parent_ptr);
        if (c == 0) {
            bad_name_data(ctx, "name_data 叶链下钻遇空内部节点 inum=%llu blk=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk);
            node_buf_free(nb);
            return;
        }
        blk = RFS_PTR_ADDR(((Bptree_extent_inner_node_t *)nb)->extents[0].sub_node_ptr);
        node_buf_free(nb);
    }

    size_t idx = 0;
    uint64_t cur = blk;
    for (int guard = 0; guard < (int)w.leaves.size() + 2; guard++) {
        if (cur == PTR52) break;
        if (idx >= w.leaves.size()) {
            bad_name_data(ctx, "name_data 兄弟链多出叶子 blk=%llu (owner inum=%llu)",
                          (unsigned long long)cur, (unsigned long long)owner_inum);
            return;
        }
        if (cur != w.leaves[idx])
            bad_name_data(ctx, "name_data 兄弟链序 != DFS 叶序 (cur=%llu expect=%llu idx=%zu inum=%llu)",
                          (unsigned long long)cur, (unsigned long long)w.leaves[idx], idx,
                          (unsigned long long)owner_inum);
        void *nb = node_buf_alloc();
        if (!nb) return;
        if (!audit_node_read(cur, nb)) {
            bad_name_data(ctx, "name_data 叶链节点读取失败 inum=%llu blk=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)cur);
            node_buf_free(nb);
            return;
        }
        Bptree_meta_t *m = (Bptree_meta_t *)nb;
        if (!nd_node_magic(m)) {
            bad_name_data(ctx, "name_data 叶链节点 magic 不符 inum=%llu blk=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)cur);
            node_buf_free(nb);
            return;
        }
        const uint64_t prev = RFS_PTR_ADDR(m->ptrs[1]);
        const uint64_t next = RFS_PTR_ADDR(m->ptrs[2]);
        node_buf_free(nb);

        if (idx == 0 && prev != PTR52)
            bad_name_data(ctx, "name_data 最左叶 inum=%llu blk=%llu prev=%llu 非 INVALID",
                          (unsigned long long)owner_inum, (unsigned long long)cur,
                          (unsigned long long)prev);

        if (prev != PTR52) {
            if (!node_ok(prev)) {
                bad_name_data(ctx, "name_data 前驱块非法 inum=%llu blk=%llu prev=%llu",
                              (unsigned long long)owner_inum, (unsigned long long)cur,
                              (unsigned long long)prev);
            } else {
                void *pb = node_buf_alloc();
                if (!pb || !audit_node_read(prev, pb)) {
                    bad_name_data(ctx, "name_data 前驱块读取失败 inum=%llu blk=%llu prev=%llu",
                                  (unsigned long long)owner_inum, (unsigned long long)cur,
                                  (unsigned long long)prev);
                    if (pb) node_buf_free(pb);
                } else {
                    Bptree_meta_t *pm = (Bptree_meta_t *)pb;
                    if (!nd_node_magic(pm) || RFS_PTR_ADDR(pm->ptrs[2]) != cur)
                        bad_name_data(ctx, "name_data 兄弟链断裂: blk=%llu.prev=%llu 但前驱.next != blk",
                                      (unsigned long long)cur, (unsigned long long)prev);
                    node_buf_free(pb);
                }
            }
        }
        if (next != PTR52) {
            if (!node_ok(next)) {
                bad_name_data(ctx, "name_data 后继块非法 inum=%llu blk=%llu next=%llu",
                              (unsigned long long)owner_inum, (unsigned long long)cur,
                              (unsigned long long)next);
            } else {
                void *nn = node_buf_alloc();
                if (!nn || !audit_node_read(next, nn)) {
                    bad_name_data(ctx, "name_data 后继块读取失败 inum=%llu blk=%llu next=%llu",
                                  (unsigned long long)owner_inum, (unsigned long long)cur,
                                  (unsigned long long)next);
                    if (nn) node_buf_free(nn);
                } else {
                    Bptree_meta_t *nm = (Bptree_meta_t *)nn;
                    if (!nd_node_magic(nm) || RFS_PTR_ADDR(nm->ptrs[1]) != cur)
                        bad_name_data(ctx, "name_data 兄弟链断裂: blk=%llu.next=%llu 但后继.prev != blk",
                                      (unsigned long long)cur, (unsigned long long)next);
                    node_buf_free(nn);
                }
            }
        }
        cur = next;
        idx++;
    }
    if (idx != w.leaves.size())
        bad_name_data(ctx, "name_data 兄弟链叶子数 %zu != DFS 叶数 %zu (inum=%llu)",
                      idx, w.leaves.size(), (unsigned long long)owner_inum);
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_name_data_free_ranges — 空闲区间树（fr）独立审计（形态分派 + 认领）
 *
 * 契约（仅据 disk_struct_v1.3.h）：
 *   · fr_ctl：[0]=inline(1)/tree(0)；[1:52)=树根块号。空态 ptr 全 1
 *     （RFS_ND_FR_CTL_INACTIVE）且 fr_count==0。
 *   · fr_count = 全域空闲区间总数 = fr 树叶条目总数。
 *   · inline 态表本轮未实现（恒空态）；tree 态优先生效。
 *   · tree：节点 NAMEDATA_FREERANGES_MAGIC1..4；hint=子树最大复合键。
 * 认领 fr 树全部节点块（2/节点）；违例经 bad_name_data 计 heap_bad。
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_name_data_free_ranges(audit_walk_ctx_t &ctx, uint64_t inum,
                                                   const raven_dir_Inode_t *d)
{
    const uint64_t fr_ctl = d->name_data_free_ranges_ctl;
    const uint64_t fr_cnt = RFS_ND_FR_COUNT(d->name_data_free_ranges_count);
    const uint64_t ptr    = RFS_ND_FR_CTL_PTR(fr_ctl);
    const uint64_t pieces = RFS_ND_PIECE_COUNT(d->name_data_piece_count);
    const uint64_t limit  = pieces * 8192;   /* 逻辑空间字节上界 = pieces×8K */

    /* ── 空态：ptr 全 1 ⇔ fr_count==0 ── */
    if (ptr == RFS_ND_FR_CTL_PTR_INVALID) {
        if (fr_cnt != 0)
            bad_name_data(ctx, "DIR inode %llu fr 空态（ptr 全 1）但 fr_count=%llu",
                          (unsigned long long)inum, (unsigned long long)fr_cnt);
        return;
    }

    /* ── inline 态（本轮未实现；如出现做表级校验，无节点认领）── */
    if (RFS_ND_FR_CTL_IS_INLINE(fr_ctl)) {
        if (fr_cnt > RFS_ND_FREE_INLINE_MAX)
            bad_name_data(ctx, "DIR inode %llu fr inline 态 fr_count=%llu > 上限 %u",
                          (unsigned long long)inum, (unsigned long long)fr_cnt,
                          (unsigned)RFS_ND_FREE_INLINE_MAX);
        bool have = false; uint64_t pl = 0, pb = 0;
        const uint64_t n = (fr_cnt < RFS_ND_FREE_INLINE_MAX) ? fr_cnt : RFS_ND_FREE_INLINE_MAX;
        for (uint64_t i = 0; i < n; i++) {
            const free_range_t &e = d->possible_inline_free_ranges[i];
            if (e.len == 0)
                bad_name_data(ctx, "DIR inode %llu fr inline[%llu] len==0",
                              (unsigned long long)inum, (unsigned long long)i);
            if (e.base > limit || e.len > limit - e.base)
                bad_name_data(ctx, "DIR inode %llu fr inline[%llu] 越界 base=%llu len=%llu limit=%llu",
                              (unsigned long long)inum, (unsigned long long)i,
                              (unsigned long long)e.base, (unsigned long long)e.len,
                              (unsigned long long)limit);
            if (have && !fr_key_gt(e.len, e.base, pl, pb))
                bad_name_data(ctx, "DIR inode %llu fr inline 复合键非严格递增 [%llu]",
                              (unsigned long long)inum, (unsigned long long)i);
            pl = e.len; pb = e.base; have = true;
        }
        return;
    }

    /* ── tree 态 ── */
    if (ptr & 1)
        bad_name_data(ctx, "DIR inode %llu fr 树根=%llu 未 8K 对齐",
                      (unsigned long long)inum, (unsigned long long)ptr);
    if (ptr >= total_blocks || ptr + 1 >= total_blocks) {
        bad_name_data(ctx, "DIR inode %llu fr 树根=%llu 越界 total=%llu",
                      (unsigned long long)inum, (unsigned long long)ptr,
                      (unsigned long long)total_blocks);
        return;
    }

    audit_fr_tree_t w;
    w.limit = limit;
    bool sub_ok = false;
    uint64_t mx_len = 0, mx_base = 0;
    audit_fr_tree(ctx, ptr, inum, inum, true, 0, w, sub_ok, &mx_len, &mx_base);
    (void)sub_ok; (void)mx_len; (void)mx_base;

    /* 全域空闲区间总数 == 叶条目总数 */
    if (w.records != fr_cnt)
        bad_name_data(ctx, "DIR inode %llu fr_count=%llu != fr 树叶条目 Σ=%llu",
                      (unsigned long long)inum, (unsigned long long)fr_cnt,
                      (unsigned long long)w.records);

    ctx.rep.heap_active_bitmap_blk += w.node_blocks;
}

/* ═══════════════════════════════════════════════════════════════════
 * audit_fr_tree — fr 树 DFS（独立推导）
 *
 * 仅自识别 NAMEDATA_FREERANGES_MAGIC1..4。校验：角色/父指针/叶等深、
 *   count≤337、leaf 复合键 (len,base) 全局严格递增且 len>0、区间
 *   [base,base+len)⊆[0,limit)、inner hint==子树最大复合键且严格递增。
 * 认领节点块（2/节点）；违例计 heap_bad，二次认领计 multi_claim_blk。
 * 不校验最小占用/合并（God 本轮不做）。出参 out_max_{len,base}=本子树最大复合键。
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_fr_tree(audit_walk_ctx_t &ctx, uint64_t blk,
        uint64_t owner_inum, uint64_t expect_parent, bool expect_root, int depth,
        audit_fr_tree_t &w, bool &sub_ok, uint64_t *out_max_len, uint64_t *out_max_base)
{
    sub_ok = false;
    if (out_max_len)  *out_max_len = 0;
    if (out_max_base) *out_max_base = 0;

    if (depth > ctx.max_depth) {
        bad_name_data(ctx, "fr tree inum=%llu depth 超限 %d (blk=%llu)",
                      (unsigned long long)owner_inum, depth, (unsigned long long)blk);
        return;
    }
    if (blk == PTR_INVALID || ptr52_bad(blk) || blk >= total_blocks ||
        (blk & 1) || blk + 1 >= total_blocks) {
        bad_name_data(ctx, "fr tree inum=%llu blk=%llu 非法节点块号",
                      (unsigned long long)owner_inum, (unsigned long long)blk);
        return;
    }
    if (!ctx.blk_set(blk)) {
        ctx.rep.multi_claim_blk++;
        audit_logf(ctx, "MULTI_CLAIM", "fr 树节点 blk=%llu 二次认领 (inode %llu)",
                   (unsigned long long)blk, (unsigned long long)owner_inum);
        return;
    }
    w.node_blocks += 2;   /* 节点恒 8K = 2 FS_block */
    if (!ctx.blk_set(blk + 1)) {
        ctx.rep.multi_claim_blk++;
        audit_logf(ctx, "MULTI_CLAIM", "fr 树节点尾巴 blk+1=%llu 二次认领 (inode %llu)",
                   (unsigned long long)(blk + 1), (unsigned long long)owner_inum);
    }

    void *nb = node_buf_alloc();
    if (!nb) {
        bad_name_data(ctx, "fr tree inum=%llu blk=%llu 节点缓冲分配失败",
                      (unsigned long long)owner_inum, (unsigned long long)blk);
        return;
    }
    if (!audit_node_read(blk, nb)) {
        bad_name_data(ctx, "fr tree inum=%llu blk=%llu 节点读取失败",
                      (unsigned long long)owner_inum, (unsigned long long)blk);
        node_buf_free(nb);
        return;
    }
    Bptree_meta_t *meta = (Bptree_meta_t *)nb;
    if (!fr_node_magic(meta)) {
        bad_name_data(ctx, "fr tree inum=%llu blk=%llu magic 不符（非 NAMEDATA_FREERANGES）",
                      (unsigned long long)owner_inum, (unsigned long long)blk);
        node_buf_free(nb);
        return;
    }

    const uint16_t cnt   = (uint16_t)RFS_PP_COUNT(meta->parent_ptr);
    const uint16_t it    = std::min<uint16_t>(cnt, (uint16_t)(FR_ORDER + 1));
    const bool     inner = RFS_PP_IS_INNER(meta->parent_ptr);

    if (cnt > FR_ORDER)
        bad_name_data(ctx, "fr tree inum=%llu blk=%llu entry_count=%u > 阶 %u",
                      (unsigned long long)owner_inum, (unsigned long long)blk,
                      cnt, (unsigned)FR_ORDER);
    if ((bool)RFS_PP_IS_ROOT(meta->parent_ptr) != expect_root)
        bad_name_data(ctx, "fr tree inum=%llu blk=%llu is_root=%d 期望 %d",
                      (unsigned long long)owner_inum, (unsigned long long)blk,
                      (int)RFS_PP_IS_ROOT(meta->parent_ptr), (int)expect_root);
    if (expect_root) {
        if (RFS_PP_PTR(meta->parent_ptr) != owner_inum)
            bad_name_data(ctx, "fr tree inum=%llu 根 blk=%llu ptr=%llu != owner %llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk,
                          (unsigned long long)RFS_PP_PTR(meta->parent_ptr),
                          (unsigned long long)owner_inum);
    } else {
        if (RFS_PP_PTR(meta->parent_ptr) != expect_parent)
            bad_name_data(ctx, "fr tree inum=%llu blk=%llu parent_ptr=%llu != 实际父 %llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk,
                          (unsigned long long)RFS_PP_PTR(meta->parent_ptr),
                          (unsigned long long)expect_parent);
    }

    /* ══════ 叶子 ══════ */
    if (!inner) {
        if (!w.leaf_depth_set) { w.leaf_depth = (uint64_t)depth; w.leaf_depth_set = true; }
        else if ((uint64_t)depth != w.leaf_depth)
            bad_name_data(ctx, "fr tree inum=%llu blk=%llu 叶深度 %d != %llu（不平衡）",
                          (unsigned long long)owner_inum, (unsigned long long)blk, depth,
                          (unsigned long long)w.leaf_depth);
        w.leaves.push_back(blk);

        const Bptree_dir_name_data_free_ranges_leaf_node_t *leaf =
            (const Bptree_dir_name_data_free_ranges_leaf_node_t *)nb;
        uint64_t last_len = 0, last_base = 0;
        for (uint16_t i = 0; i < it; i++) {
            const uint64_t elen  = leaf->free_ranges[i].len;
            const uint64_t ebase = leaf->free_ranges[i].base;
            if (elen == 0)
                bad_name_data(ctx, "fr tree inum=%llu blk=%llu free_ranges[%u] len==0",
                              (unsigned long long)owner_inum, (unsigned long long)blk, i);
            if (ebase > w.limit || elen > w.limit - ebase)
                bad_name_data(ctx, "fr tree inum=%llu blk=%llu free_ranges[%u] 越界 base=%llu len=%llu limit=%llu",
                              (unsigned long long)owner_inum, (unsigned long long)blk, i,
                              (unsigned long long)ebase, (unsigned long long)elen,
                              (unsigned long long)w.limit);
            if (w.have_last && !fr_key_gt(elen, ebase, w.last_len, w.last_base))
                bad_name_data(ctx, "fr tree inum=%llu blk=%llu free_ranges[%u] 复合键非严格递增 (len=%llu,base=%llu)",
                              (unsigned long long)owner_inum, (unsigned long long)blk, i,
                              (unsigned long long)elen, (unsigned long long)ebase);
            w.last_len = elen; w.last_base = ebase; w.have_last = true;
            w.records++;
            last_len = elen; last_base = ebase;
        }
        if (out_max_len)  *out_max_len  = last_len;
        if (out_max_base) *out_max_base = last_base;
        sub_ok = true;
        node_buf_free(nb);
        return;
    }

    /* ══════ 内部节点 ══════ */
    if (cnt == 0) {
        bad_name_data(ctx, "fr tree inum=%llu blk=%llu 内部节点空",
                      (unsigned long long)owner_inum, (unsigned long long)blk);
        node_buf_free(nb);
        return;
    }
    sub_ok = true;
    std::vector<uint64_t> child(it);
    std::vector<std::pair<uint64_t, uint64_t>> hint(it);   /* (max_len,max_base) */
    for (uint16_t i = 0; i < it; i++) {
        const Bptree_dir_name_data_free_ranges_inner_node_t *nd =
            (const Bptree_dir_name_data_free_ranges_inner_node_t *)nb;
        child[i]       = RFS_PTR_ADDR(nd->hints[i].sub_node_ptr);
        hint[i].first  = nd->hints[i].max_len;
        hint[i].second = nd->hints[i].max_base;
    }
    node_buf_free(nb);

    /* hint 严格递增：仅对**非空子树**参与比较。God 本轮不做欠载合并，一个非根叶
     * 可被 take_fit 逐个取空 → 其父 hint 被刷新为 max_key 空值 (0,0)（合法降级），
     * 这种「空子树占位」不参与复合键序，否则会误报。空/非空的真伪由下方
     * 「hint == 子树 max」等值校验兜底。 */
    bool prev_set = false;
    uint64_t prev_len = 0, prev_base = 0;
    for (uint16_t i = 0; i < it; i++) {
        const bool hint_empty = (hint[i].first == 0 && hint[i].second == 0);
        if (!hint_empty) {
            if (prev_set && !fr_key_gt(hint[i].first, hint[i].second, prev_len, prev_base))
                bad_name_data(ctx, "fr tree inum=%llu blk=%llu hint[%u] 复合键非严格递增 (len=%llu,base=%llu)",
                              (unsigned long long)owner_inum, (unsigned long long)blk, i,
                              (unsigned long long)hint[i].first, (unsigned long long)hint[i].second);
            prev_len = hint[i].first; prev_base = hint[i].second; prev_set = true;
        }

        const uint64_t c = child[i];
        if (c == PTR52 || c >= total_blocks) {
            bad_name_data(ctx, "fr tree inum=%llu blk=%llu hint[%u] child 指针非法=%llu",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                          (unsigned long long)c);
            continue;
        }
        bool cok = false;
        uint64_t clen = 0, cbase = 0;
        audit_fr_tree(ctx, c, owner_inum, blk, false, depth + 1, w, cok, &clen, &cbase);
        if (cok && (clen != hint[i].first || cbase != hint[i].second))
            bad_name_data(ctx, "fr tree inum=%llu blk=%llu hint[%u] key=(%llu,%llu) != 子树 max=(%llu,%llu)",
                          (unsigned long long)owner_inum, (unsigned long long)blk, i,
                          (unsigned long long)hint[i].first, (unsigned long long)hint[i].second,
                          (unsigned long long)clen, (unsigned long long)cbase);
        if (out_max_len)  *out_max_len  = cok ? clen : hint[i].first;
        if (out_max_base) *out_max_base = cok ? cbase : hint[i].second;
    }
}

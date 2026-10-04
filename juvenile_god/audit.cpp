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

/* 名字堆静态几何（与 name_heap.cpp / raven_dir_Inode_t 字段序一致） */
constexpr uint8_t  HEAP_GRAN[10] = { 3,3,3, 6,6, 7,7, 8,8, 9 };
constexpr uint8_t  HEAP_BITS[10] = { 16,17,18, 16,17, 16,17, 16,17, 16 };

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
void bad_heap(audit_walk_ctx_t &ctx, const char *fmt, ...)
{
    ctx.rep.heap_bad++;
    va_list ap; va_start(ap, fmt); audit_vlog(ctx, "BAD_HEAP", fmt, ap); va_end(ap);
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
 * audit_walk_dir — DIR 字段校验 + 名字堆 + dentry 树 DFS + 子目录下钻
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_walk_dir(audit_walk_ctx_t &ctx, uint64_t inum, uint64_t parent_inum)
{
    raven_Inode_t raw;
    inode_read(inum, &raw);
    const raven_dir_Inode_t *d = (const raven_dir_Inode_t *)&raw;

    uint64_t root = d->data_root;
    if (root == PTR_INVALID || root >= total_blocks || (root & 1)) {
        bad_inode(ctx, "DIR inode %llu data_root 非法=%#llx", (unsigned long long)inum,
                  (unsigned long long)root);
        return;
    }

    /* 名字堆描述符 + 占用率 + 归属区（供长名指针校验；随参数下传） */
    std::vector<audit_heap_region_t> heaps;
    audit_name_heaps(ctx, inum, d, heaps);

    /* dentry 树 DFS（先序：本目录树先审完，再下钻子目录） */
    std::vector<uint64_t> children;
    audit_walk_tree_t t;
    bool ok = true;
    audit_walk_btree(ctx, heaps, children, t, root, inum, inum /*root parent=owner*/,
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
        std::vector<audit_heap_region_t> none;
        std::vector<uint64_t> children;
        audit_walk_tree_t t;
        bool ok = true;
        audit_walk_btree(ctx, none, children, t, root, inum, inum, inum,
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
        const std::vector<audit_heap_region_t> &heaps, std::vector<uint64_t> &children,
        audit_walk_tree_t &t, uint64_t blk, uint64_t owner_inum, uint64_t expect_parent,
        uint64_t dotdot_expect, bool expect_root, bool is_dentry, uint64_t size_bound,
        int depth, bool &sub_ok)
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

                /* 名字重建（≤40 内联 / >40 读堆）→ hash 重算 */
                char full[RAVENFS_MAX_NAME_LEN + 1];
                const uint16_t ncopy = name_len < DENTRY_INLINE_NAME_MAX ? name_len : DENTRY_INLINE_NAME_MAX;
                std::memcpy(full, e.name, ncopy);
                bool name_ok = true;
                if (name_len > DENTRY_INLINE_NAME_MAX) {
                    if (e.full_char_remain_ptr == PTR_INVALID) {
                        name_ok = false;
                        bad_heap(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] 长名但 remain=PTR_INVALID",
                                 (unsigned long long)owner_inum, (unsigned long long)blk, i);
                    } else {
                        const uint16_t tail = name_len - DENTRY_INLINE_NAME_MAX;
                        const uint16_t units = (tail <= 40) ? (uint16_t)((tail + 7) / 8) : 1;
                        const uint64_t fblk = e.full_char_remain_ptr >> 12;
                        const uint16_t off  = (uint16_t)(e.full_char_remain_ptr & 0xFFF);
                        const audit_heap_region_t *hit = nullptr;
                        for (const auto &r : heaps)
                            if (fblk >= r.data_lo && fblk < r.data_hi) { hit = &r; break; }
                        if (!hit) {
                            name_ok = false;
                            bad_heap(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] 长名 remain=%#llx 不落本目录任何激活堆数据区",
                                     (unsigned long long)owner_inum, (unsigned long long)blk, i,
                                     (unsigned long long)e.full_char_remain_ptr);
                        } else if (name_heap_read(name_len, e.full_char_remain_ptr,
                                                  full + DENTRY_INLINE_NAME_MAX) != RC_OK) {
                            name_ok = false;
                            bad_heap(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] 长名尾部读取失败",
                                     (unsigned long long)owner_inum, (unsigned long long)blk, i);
                        } else {
                            const uint64_t sbyte = (fblk - hit->data_lo) * 4096 + off;
                            const uint64_t bit0  = sbyte >> hit->gran_log2;
                            bool bits_ok = true;
                            for (uint16_t u = 0; u < units && bits_ok; u++)
                                if (audit_heap_bit(*hit, bit0 + u) != 1) bits_ok = false;
                            if (!bits_ok) {
                                name_ok = false;
                                bad_heap(ctx, "tree=dentry inum=%llu blk=%llu entries[%u] 长名堆位图未置位 (fblk=%llu off=%u units=%u)",
                                         (unsigned long long)owner_inum, (unsigned long long)blk, i,
                                         (unsigned long long)fblk, off, units);
                            }
                        }
                    }
                } else if (e.full_char_remain_ptr != PTR_INVALID) {
                    audit_logf(ctx, "WARN", "tree=dentry inum=%llu blk=%llu entries[%u] name_len<=40 但 remain 非 INVALID（软）",
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
        const uint64_t cmax = audit_walk_btree(ctx, heaps, children, t, child, owner_inum, blk,
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
 * audit_name_heaps — 描述符自洽 + 占用率 + 归属区登记（out_heaps）
 * ═══════════════════════════════════════════════════════════════════ */
void RavenFS_juvenile::audit_name_heaps(audit_walk_ctx_t &ctx, uint64_t inum,
                                        const raven_dir_Inode_t *d,
                                        std::vector<audit_heap_region_t> &out_heaps)
{
    out_heaps.clear();
    const name_heap_controller_t *arr = (const name_heap_controller_t *)&d->heaps_8B_8k;
    for (int s = 0; s < 10; s++) {
        const name_heap_controller_t &c = arr[s];
        const uint64_t bmpb = RFS_NHC_BITMAP(c);
        const uint64_t datab = RFS_NHC_DATA(c);
        const uint8_t gran = (uint8_t)RFS_NHC_GRAN(c);
        const uint8_t bits = (uint8_t)RFS_NHC_BITS(c);
        const bool inactive = (bmpb == NAME_HEAP_INACTIVE && datab == NAME_HEAP_INACTIVE);
        if (inactive) continue;

        if (bmpb == NAME_HEAP_INACTIVE || datab == NAME_HEAP_INACTIVE) {
            bad_heap(ctx, "DIR inode %llu heap[%d] 半激活 (bmp=%llu data=%llu)",
                     (unsigned long long)inum, s, (unsigned long long)bmpb, (unsigned long long)datab);
            continue;
        }
        if (gran != HEAP_GRAN[s] || bits != HEAP_BITS[s])
            audit_logf(ctx, "WARN", "DIR inode %llu heap[%d] log2 非常规 gran=%u bits=%u (期望 %u/%u)",
                       (unsigned long long)inum, s, gran, bits, (unsigned)HEAP_GRAN[s], (unsigned)HEAP_BITS[s]);
        const uint64_t bmp_blks  = 1ULL << (bits - 15);
        const uint64_t data_blks = 1ULL << (bits + gran - 12);
        if (bmpb >= total_blocks || (bmpb & 1) || bmpb + bmp_blks > total_blocks ||
            datab >= total_blocks || (datab & 1) || datab + data_blks > total_blocks) {
            bad_heap(ctx, "DIR inode %llu heap[%d] 区间越界 bmp[%llu,+%llu) data[%llu,+%llu)",
                     (unsigned long long)inum, s, (unsigned long long)bmpb, (unsigned long long)bmp_blks,
                     (unsigned long long)datab, (unsigned long long)data_blks);
            continue;
        }
        for (uint64_t k = 0; k < bmp_blks; k++) {
            if (!ctx.blk_set(bmpb + k)) { ctx.rep.multi_claim_blk++;
                audit_logf(ctx, "MULTI_CLAIM", "heap bmp 块 %llu 二次认领 (inode %llu)",
                           (unsigned long long)(bmpb + k), (unsigned long long)inum); } }
        for (uint64_t k = 0; k < data_blks; k++) {
            if (!ctx.blk_set(datab + k)) { ctx.rep.multi_claim_blk++;
                audit_logf(ctx, "MULTI_CLAIM", "heap data 块 %llu 二次认领 (inode %llu)",
                           (unsigned long long)(datab + k), (unsigned long long)inum); } }

        std::vector<uint8_t> bm((size_t)bmp_blks * 4096);
        if (read_buff(bmpb, bmp_blks, bm.data()) != RC_OK) {
            bad_heap(ctx, "DIR inode %llu heap[%d] 位图读取失败", (unsigned long long)inum, s);
            continue;
        }
        uint64_t used = 0;
        for (uint8_t v : bm) used += (uint64_t)__builtin_popcount(v);
        const uint64_t total_bits = 1ULL << bits;
        audit_logf(ctx, "HEAP",
                   "DIR inode %llu heap[%d] gran=%uB bits_log2=%u bmp=[%llu,+%llu) data=[%llu,+%llu) 占用 %llu/%llu (%.1f%%)",
                   (unsigned long long)inum, s, 1u << gran, bits,
                   (unsigned long long)bmpb, (unsigned long long)bmp_blks,
                   (unsigned long long)datab, (unsigned long long)data_blks,
                   (unsigned long long)used, (unsigned long long)total_bits,
                   total_bits ? 100.0 * (double)used / (double)total_bits : 0.0);

        audit_heap_region_t r;
        r.data_lo = datab; r.data_hi = datab + data_blks;
        r.gran_log2 = gran; r.bmp_base = bmpb; r.total_bits = total_bits;
        out_heaps.push_back(r);
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

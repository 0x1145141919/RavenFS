#pragma once
/* ═══════════════════════════════════════════════════════════════════
 * ravenfs_audit.hpp — 可达性审计（新模型）类型
 *
 * 2026-09-30 重写（Raven）。**取代**旧「枚举全 inode + 侧账本反推」模型
 * （audit_ctx_t / audit_begin / audit_inode / audit_filesystem 等，已退役）。
 *
 * 新模型（一句话）：**从 root 先序 DFS 一路下钻 → 边走边重画两张影子图
 * （块 / inode）→ 收口与真实位图 XOR → 分类「没有被保护 / 疑似泄露」**。
 *   · 影子图 = 审计自己按可达性重画的"应该被占"；
 *   · 真实位图 = 分配器账本；
 *   · XOR 左差（可达 ∧ 位图暗）= 没有被保护（活结构指向空闲块，会被再分配）；
 *     右差（位图亮 ∧ 不可达）= 疑似泄露（分配了但捞不回）。
 * 可达性由递归本身携带 ⇒ 不再需要 ref_count / parent_of / dotdot_of /
 * visited_nodes / claim_bits 等跨 inode 平行数组。
 *
 * 前提：mmap 后端（is_device_direct=false，offload 缓存直映窗口）——
 *       审计据 fd 映射的窗口直接访问内存对账。只读、不修复；输出走 FILE*。
 *
 * 类型面与实现分离：本头只放数据结构；实现在 juvenile_god/audit.cpp，
 * 入口是 RavenFS_juvenile 的成员方法（复用其位图 + 缓存 + 几何）。
 * ═══════════════════════════════════════════════════════════════════ */
#include "../disk_struct_v1.2.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class RavenFS_juvenile;   /* 前置：ctx 只持指针 */

/* ── 名字堆激活区描述（长名指针归属校验用；每目录 transient）──
 * 由 audit_walk_dir 在审某目录时构建，随递归参数下传；用完即弃，
 * 不挂进 ctx（这是相对旧 cur_heaps 的一处减法）。 */
struct audit_heap_region_t {
    uint64_t data_lo, data_hi;   /* 数据面块区间（半开） */
    uint8_t  gran_log2;
    uint64_t bmp_base;
    uint64_t total_bits;
};

/* ── 每棵树的瞬态工作态（DFS 内累积；每棵树一份）──
 * 只留两件事：DFS 序叶子块号（对账兄弟链序）+ 首见叶深度（等深校验）。 */
struct audit_walk_tree_t {
    std::vector<uint64_t> leaves;      /* DFS 序叶子块号 */
    uint64_t leaf_depth = 0;
    bool     leaf_depth_set = false;
    uint64_t records_total = 0;        /* 本树叶条目累计（对 dir size） */
};

/* ── 审计汇总报表（收口后填满；对账粒度只到计数）── */
struct audit_walk_report_t {
    uint64_t inodes_walked = 0;
    uint64_t nodes = 0, inner = 0, leaves = 0, records = 0;

    /* 结构不变量违例（重点） */
    uint64_t btree_bad = 0;          /* B+树结构不变量违例 */
    uint64_t inode_bad = 0;          /* inode 字段硬矛盾 */
    uint64_t heap_bad  = 0;          /* 名字堆描述符 / 归属异常 */
    uint64_t xattr_bad = 0;          /* xattr 块描述符异常 */
    uint64_t multi_ref_ino  = 0;     /* inode 二次触达（环 / 共享 / 硬链接 / 双引用） */
    uint64_t multi_claim_blk = 0;    /* 物理块二次认领 */
    uint64_t link_mismatch = 0;      /* link_count 对不上 */
    uint64_t dotdot_mismatch = 0;    /* dir 根 '..' 对不上实际父 */

    /* 收口 XOR（核心输出） */
    uint64_t unprotected_blk = 0;    /* 可达 ∧ 块位图暗  → 没有被保护 */
    uint64_t unprotected_ino = 0;    /* 可达 ∧ inode 位图暗 → 没有被保护（悬挂引用） */
    uint64_t leaked_blk = 0;         /* 块位图亮 ∧ 不可达 → 疑似泄露 */
    uint64_t leaked_ino = 0;         /* inode 位图亮 ∧ 不可达 → 疑似泄露（孤儿） */

    uint64_t total_bad() const {
        return btree_bad + inode_bad + heap_bad + xattr_bad + multi_ref_ino +
               multi_claim_blk + link_mismatch + dotdot_mismatch +
               unprotected_blk + unprotected_ino + leaked_blk + leaked_ino;
    }

    /* ── fsck 修复计数（D-12；仅骨架，本轮不写盘）──
     * link_repaired = 本轮「拟修复」的 DIR link_count 偏差条数。语义是修复
     * 动作计数，**不是** bad 计数，故显式不纳入 total_bad()（保持 total_bad
     * 只反映"发现的不变量违例"这一既有口径）。 */
    uint64_t link_repaired = 0;
};

/* ── 可达性审计上下文（递归携带；瘦身 = 两张影子图 + 白名单 + 路径栈 + 计数）──
 * 对照旧 audit_ctx_t（约 20 字段）：删去 ref_count / parent_of / dotdot_of /
 * inode_flag / visited_nodes / claim_bits / heap_occ / cur_heaps / cur_dir，
 * 全部由「影子图 + 递归携带」替代。 */
struct audit_walk_ctx_t {
    RavenFS_juvenile *fs = nullptr;
    FILE *out = nullptr;              /* 输出 sink（默认 stdout） */

    /* 影子图 A：块可达位图（1 bit / FS_block）；兼作"已认领"→ 二次认领检测 */
    std::vector<uint8_t> shadow_blk;
    /* 影子图 B：inode 可达位图（1 bit / inode）；兼作"已访问"→ 环/共享检测 */
    std::vector<uint8_t> shadow_ino;
    uint64_t blk_bits = 0;
    uint64_t ino_bits = 0;

    /* 结构块白名单（收口 XOR 时排除）：尾巴三段 [meta_lo, meta_hi) + backup SB 链 */
    uint64_t meta_lo = 0, meta_hi = 0;
    std::vector<uint64_t> backup_blocks;

    /* DFS 路径栈（人话路径打印用；与 inum 平行） */
    std::vector<uint64_t>    path_inum;
    std::vector<std::string> path_name;
    int depth = 0;
    int max_depth = 64;
    bool verbose = false;          /* true: 逐 inode 打印 WALK（大盛盘日志巨大，默认关） */

    audit_walk_report_t rep;

    /* ── 影子图原语 ── */
    bool blk_test(uint64_t b) const {
        return b < blk_bits && (shadow_blk[b >> 3] & (uint8_t)(1u << (b & 7))); }
    bool blk_set(uint64_t b) {          /* true = 首次置位（未认领过） */
        if (b >= blk_bits) return false;
        const uint8_t m = (uint8_t)(1u << (b & 7));
        if (shadow_blk[b >> 3] & m) return false;
        shadow_blk[b >> 3] |= m; return true; }
    bool ino_test(uint64_t i) const {
        return i < ino_bits && (shadow_ino[i >> 3] & (uint8_t)(1u << (i & 7))); }
    bool ino_set(uint64_t i) {          /* true = 首次置位（未访问过） */
        if (i >= ino_bits) return false;
        const uint8_t m = (uint8_t)(1u << (i & 7));
        if (shadow_ino[i >> 3] & m) return false;
        shadow_ino[i >> 3] |= m; return true; }

    bool is_backup_blk(uint64_t b) const {
        for (uint64_t x : backup_blocks) if (x == b) return true;
        return false; }
};

/* ── 人话路径串（"a/b/c" 形态；审计打印用）── */
static inline std::string audit_path_str(const audit_walk_ctx_t &ctx)
{
    std::string s;
    for (size_t i = 0; i < ctx.path_name.size(); i++) {
        if (i) s += '/';
        s += ctx.path_name[i];
    }
    return s.empty() ? std::string("/") : s;
}

#pragma once
/* ═══════════════════════════════════════════════════════════════════
 * ravenfs_audit.hpp — 静态合法性审计类型（audit_inode 专用）
 *
 * 设计基线：Docs/audit_inode_design.md（2026-09-10 定稿）。
 * 定位：内部测试/调试手段；只读、不修复；重点 B+树数据结构一致性。
 *
 * 类型面与实现分离：本头只放数据结构；实现在 juvenile_god/audit.cpp，
 * 入口是 RavenFS_juvenile 的成员方法（复用其位图 + 缓存）。
 * ═══════════════════════════════════════════════════════════════════ */
#include "disk_struct_v1.2.h"
#include <cstdint>
#include <vector>
#include <unordered_set>

class RavenFS_juvenile;   /* 前置：ctx 只持指针 */

/* ── 日志 sink（默认 stderr；测试可接缓冲/文件） ── */
struct audit_log_t {
    void (*emit)(void *ud, const char *line) = nullptr;
    void  *ud = nullptr;
};

/* ── 审计选项 ── */
struct audit_opts_t {
    uint16_t max_tree_depth  = 64;    /* 树递归深度上限（防环底兜） */
    bool     trace_nodes     = false; /* true: 每进节点发 [AUDIT][TRACE] */
    bool     check_name_heap = true;  /* 名字堆描述符 + 占用率 */
    bool     check_bitmap    = true;  /* 收口跑 claims↔bitmap 对账 */
    audit_log_t log{};                /* 日志 sink（emit==nullptr → stderr） */
};

/* ── 名字堆占用率报告项 ── */
struct audit_heap_occ_t {
    uint64_t dir_inum;
    uint8_t  slot;
    uint8_t  gran_log2;
    uint8_t  bits_log2;
    uint64_t bitmap_base;
    uint64_t data_base;
    uint64_t used_bits;
    uint64_t total_bits;
};

/* ── 单棵树的工作态（DFS 内累积；每棵树一份） ── */
struct audit_tree_ctx_t {
    std::vector<uint64_t> dfs_leaves;   /* DFS 序叶子块号（对账兄弟链序） */
    uint64_t leaf_depth = 0;
    bool     leaf_depth_set = false;
    uint64_t nodes = 0, inner = 0, leaves = 0, records = 0;
};

/* ── 活动堆数据区描述（长名指针归属校验用；每目录 transient） ── */
struct audit_heap_region_t {
    uint64_t data_lo, data_hi;   /* 数据面块区间（半开） */
    uint8_t  gran_log2;
    uint64_t bmp_base;
    uint64_t total_bits;
};

/* ── 审计汇总报表（收口后填满；对账粒度只到计数） ── */
struct audit_report_t {
    uint64_t inodes_scanned = 0;
    uint64_t inodes_flagged = 0;   /* inode_flag 置位数（黄牌/异常） */
    uint64_t nodes = 0, inner = 0, leaves = 0, records = 0;
    uint64_t btree_bad = 0;        /* B+树结构不变量违例（重点） */
    uint64_t inode_bad = 0;        /* inode 字段硬矛盾 */
    uint64_t double_claim = 0;     /* 物理块双重认领 */
    uint64_t leaked_blocks = 0;    /* claim 但 bitmap 暗 */
    uint64_t orphan_blocks = 0;    /* bitmap 亮但无人认领（剔结构白名单） */
    uint64_t dangling_refs = 0;    /* dentry → 未分配/越界 inode */
    uint64_t orphan_inodes = 0;    /* 已分配但不可达（非 root） */
    uint64_t link_mismatch = 0;    /* link_count 对不上 */
    uint64_t dotdot_mismatch = 0;  /* dir 根 .. 对不上 parent_of */
    uint64_t heap_bad = 0;         /* 名字堆描述符/归属异常 */
};

/* ── 审计上下文：跨 inode 共享的认领账本 + 计数 + 每 inode 一 bit 标志位 ── */
struct audit_ctx_t {
    RavenFS_juvenile *fs = nullptr;
    uint64_t total_blocks = 0;
    uint64_t inode_count  = 0;

    /* 认领账本（1 bit/块）：claim 时置位；重入 → double_claim++ */
    std::vector<uint8_t> claim_bits;
    /* 引用账本（per inode） */
    std::vector<uint32_t> ref_count;    /* dentry 扫描累加（对 link_count） */
    std::vector<uint64_t> parent_of;    /* 首次引用者目录（对 dir 的 ..） */
    std::vector<uint64_t> dotdot_of;    /* dir 根 ptrs[1]（.. 值） */
    /* 遍历状态 */
    std::unordered_set<uint64_t> visited_nodes;   /* 树内环/共享子检测 */
    /* per-inode 标志位（1 bit/inode）：一个 inode 只有一个标志位 */
    std::vector<uint8_t> inode_flag;

    /* 全局计数 */
    uint64_t nodes = 0, inner = 0, leaves = 0, records = 0;
    uint64_t btree_bad = 0, inode_bad = 0;
    uint64_t double_claim = 0, leaked_blocks = 0, orphan_blocks = 0;
    uint64_t dangling_refs = 0, orphan_inodes = 0;
    uint64_t link_mismatch = 0, dotdot_mismatch = 0, heap_bad = 0;

    /* 结构块白名单 */
    uint64_t meta_tail_lo = 0, meta_tail_hi = 0;
    std::vector<uint64_t> backup_blocks;

    /* 名字堆占用率输出 */
    std::vector<audit_heap_occ_t> heap_occ;
    /* 当前受审目录的激活堆数据区（长名指针归属校验；每目录 transient） */
    std::vector<audit_heap_region_t> cur_heaps;
    uint64_t cur_dir = 0;

    const audit_opts_t *opts = nullptr;

    bool claim_test(uint64_t blk) const { return claim_bits[blk >> 3] & (1u << (blk & 7)); }
    void claim_set (uint64_t blk)       { claim_bits[blk >> 3] |= (1u << (blk & 7)); }
    bool flag_test (uint64_t inum) const{ return inode_flag[inum >> 3] & (1u << (inum & 7)); }
    void flag_set  (uint64_t inum)      { inode_flag[inum >> 3] |= (1u << (inum & 7)); }
};

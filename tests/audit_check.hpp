#pragma once
/* ═══════════════════════════════════════════════════════════════════
 * audit_check.hpp — 测试用 audit 便捷封装（横切检查）
 *
 * 在已挂载的 RavenFS_juvenile 上跑一次全盘可达性审计，返回总 bad 计数。
 * 用法（在 `#undef private` 之后 include）：
 *     #include "audit_check.hpp"
 *     CHECK(audit_clean(fs, "after-phase2") == 0, "audit clean");
 *
 * 2026-09-30 迁移：旧 audit_filesystem/audit_report_t 退役，
 * 改 audit_walk_report_t + audit_dfs_root（root 先序 DFS + 影子图 + XOR）。
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/ravenfs_audit.hpp"
#include <cstdio>
#include <cstdint>

inline uint64_t audit_clean(RavenFS_juvenile &fs, const char *tag)
{
    audit_walk_report_t r;
    fs.audit_dfs_root(&r, stderr);   /* 详细日志到 stderr（保持旧行为） */
    const uint64_t bad = r.total_bad();
    printf("[audit:%s] bad=%llu (btree=%llu inode=%llu heap=%llu xattr=%llu "
           "multi_ref=%llu multi_claim=%llu link=%llu dotdot=%llu | "
           "unprot b/i=%llu/%llu leak b/i=%llu/%llu) inodes=%llu nodes=%llu recs=%llu\n",
           tag, (unsigned long long)bad,
           (unsigned long long)r.btree_bad, (unsigned long long)r.inode_bad,
           (unsigned long long)r.heap_bad, (unsigned long long)r.xattr_bad,
           (unsigned long long)r.multi_ref_ino, (unsigned long long)r.multi_claim_blk,
           (unsigned long long)r.link_mismatch, (unsigned long long)r.dotdot_mismatch,
           (unsigned long long)r.unprotected_blk, (unsigned long long)r.unprotected_ino,
           (unsigned long long)r.leaked_blk, (unsigned long long)r.leaked_ino,
           (unsigned long long)r.inodes_walked, (unsigned long long)r.nodes,
           (unsigned long long)r.records);
    return bad;
}

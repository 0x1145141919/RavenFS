#pragma once
/* ═══════════════════════════════════════════════════════════════════
 * audit_check.hpp — 测试用 audit 便捷封装（横切检查）
 *
 * 在已挂载的 RavenFS_juvenile 上跑一次全盘审计，返回总 bad 计数。
 * 用法（在 `#undef private` 之后 include）：
 *     #include "audit_check.hpp"
 *     CHECK(audit_clean(fs, "after-phase2") == 0, "audit clean");
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/ravenfs_audit.hpp"
#include <cstdio>
#include <cstdint>

inline uint64_t audit_clean(RavenFS_juvenile &fs, const char *tag)
{
    audit_opts_t o;      /* 默认：详细日志到 stderr */
    audit_report_t r;
    fs.audit_filesystem(o, r);
    const uint64_t bad = r.btree_bad + r.inode_bad + r.double_claim + r.leaked_blocks +
                         r.orphan_blocks + r.dangling_refs + r.orphan_inodes +
                         r.link_mismatch + r.dotdot_mismatch + r.heap_bad;
    printf("[audit:%s] bad=%llu (btree=%llu inode=%llu dbl=%llu leak=%llu orphan=%llu "
           "dang=%llu orph_ino=%llu link=%llu dd=%llu heap=%llu) nodes=%llu recs=%llu\n",
           tag, (unsigned long long)bad,
           (unsigned long long)r.btree_bad, (unsigned long long)r.inode_bad,
           (unsigned long long)r.double_claim, (unsigned long long)r.leaked_blocks,
           (unsigned long long)r.orphan_blocks, (unsigned long long)r.dangling_refs,
           (unsigned long long)r.orphan_inodes, (unsigned long long)r.link_mismatch,
           (unsigned long long)r.dotdot_mismatch, (unsigned long long)r.heap_bad,
           (unsigned long long)r.nodes, (unsigned long long)r.records);
    return bad;
}

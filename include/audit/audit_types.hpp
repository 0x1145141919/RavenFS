#pragma once
/* ═══════════════════════════════════════════════════════════════════════════
 * audit_types.hpp — audit 顶层独立项目的「类型面」
 *
 * 纯枚举 / POD，无逻辑。对应审计框架 v1（audit/DISCIPLINE.md）：
 *   · 轴1 输出分层：severity_t（E/W/I）+ scope_t；verbosity 在 audit.hpp。
 *   · 轴2 执行相位：见 audit.hpp 的 inode_plane_auditor / audit_session。
 *   · 轴3 严重度：ERROR/WARN/INFO 三分；exit 由 audit_summary/verdict 决定。
 *   · 三空间对称账本：space_ledger_t（拥有 × 分配 对账）。
 *
 * 纪律：不变量定义见 audit/DISCIPLINE.md；本头不 include 任何实现。
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <cstdint>

namespace rfs_audit {

/* ── 严重度（轴3）：黄牌绝不掩盖红牌 ──
 * ERROR = 硬不变量违例；WARN = 纯策略/优化观察；INFO = 计数/占用率。 */
enum class severity_t : uint8_t {
    info  = 0,
    warn  = 1,
    error = 2,
};

/* ── 归属域（打日志用；每行 [AUDIT][E|W|I][SCOPE]）── */
enum class scope_t : uint8_t {
    boot,         /* SB / 几何 / 引导五门 */
    block,        /* 物理块空间 */
    inode_plane,  /* 全局 inode 数据面（映射树 + 扁 fr） */
    name_plane,   /* 目录名字数据面（映射树 + fr） */
    inode,        /* 单 inode 字段 */
    btree,        /* B+树通用结构不变量 */
    dentry,       /* 目录 dentry B+树 */
    extent,       /* 文件 extent B+树 */
    xattr,        /* xattr 块 */
    namedata,     /* 名字面细节（长名区间 / 占用） */
    recon,        /* 收口对账（xor 是 C++ 保留替代记号，故用 recon） */
};

/* ── 裁决 → 退出码（框架 v1 §exit）──
 *   2 usage/IO；3 fail-fast F0–F4（唯一提前退出口）；1 走完有 ERROR；0 走完无 ERROR。 */
enum class verdict_t : int {
    pass      = 0,   /* 走完，仅 WARN 或全净 */
    has_error = 1,   /* 走完，有 ERROR */
    io        = 2,   /* 用法 / IO（环境问题） */
    fail_fast = 3,   /* 引导期硬门 F0–F4 命中 */
};

/* ── 形态标签（raw 层打印用；事实：文件形态由 size/direct_bit 算出，非存储字段）── */
enum class file_form_t : uint8_t {
    inline_data = 0,  /* size ≤ 160（无视 direct_bit） */
    direct      = 1,  /* size > 160 ∧ direct_bit */
    extent      = 2,  /* size > 160 ∧ !direct_bit */
};
enum class plane_form_t : uint8_t {
    inactive = 0,  /* 空态（ptr 域全 1） */
    direct   = 1,  /* 单连续段 */
    tree     = 2,  /* 映射 B+树 */
};

/* ── 一个空间的对称账本：拥有账本 × 分配账本 的对账结果 ──
 *   owned     = 可达 / 被引用；
 *   allocated = 分配器的空闲管理账本标「占用」；
 *   unprotected = owned ∧ !allocated（会被再分配 → 踩踏，高危）；
 *   leaked      = allocated ∧ !owned（空间捞不回）；
 *   wild_free   = 名字面专属：分配账本（fr）说空闲，但仍被 ddentry 引用
 *                 （块位图可能还亮着 → 普通块 XOR 抓不到，必须独立一项）；
 *   overlap     = 名字面专属：两个 ddentry 引用区间交叠。 */
struct space_ledger_t {
    uint64_t owned       = 0;
    uint64_t allocated   = 0;
    uint64_t unprotected = 0;
    uint64_t leaked      = 0;
    uint64_t wild_free   = 0;
    uint64_t overlap     = 0;
};

/* ── 汇总报表（收口后填满；简单命名计数，不做枚举数组 ABI）──
 *   分三组：结构不变量（ERROR）/ 策略观察（WARN）/ 观察面（INFO）。 */
struct audit_summary_t {
    /* 三空间对账（核心输出） */
    space_ledger_t block;
    space_ledger_t namedata;   /* 全目录累计 */
    space_ledger_t inode;

    /* ── 结构不变量违例（ERROR）── */
    uint64_t sb_bad          = 0;   /* SB crc 等非门一致性（magic/version 走 F0 硬门） */
    uint64_t bitmap_bad      = 0;   /* 块位图自述段 [bitmap_start,total_blocks) 非全 1 */
    uint64_t btree_bad       = 0;   /* dentry/extent B+树结构不变量违例 */
    uint64_t inode_bad       = 0;   /* inode 字段硬矛盾 / type 非法 */
    uint64_t inodeplane_bad  = 0;   /* inode 数据面映射树 + 扁 fr 不变量 */
    uint64_t namedata_bad    = 0;   /* 名字面映射树 + fr 不变量 */
    uint64_t xattr_bad       = 0;   /* xattr 块描述符 */
    uint64_t multi_ref_ino   = 0;   /* inode 二次触达（环/共享/双引用） */
    uint64_t multi_claim_blk = 0;   /* 物理块二次认领 */
    uint64_t link_mismatch   = 0;   /* link_count 对不上 */
    uint64_t dotdot_mismatch = 0;   /* dir 根 '..' 对不上实际父 */
    uint64_t inline_residual = 0;   /* inline 残留 data_root（size≤160 却 data_root≠INVALID）→ ERROR */
    uint64_t dentry_xcheck_mismatch = 0; /* 两遍 ddentry 遍历（A 审计 / B 导航）条目集不一致 → ERROR（刻意冗余对拍） */

    /* ── 策略观察（WARN，纯优化；绝不影响 XOR 与 exit=0/1 的 ERROR 判定）── */
    uint64_t warn_frag         = 0; /* 碎片率超阈 */
    uint64_t warn_no_downgrade = 0; /* 可降级未降级（单 extent 可塌 direct 等） */

    /* ── 观察面（INFO）── */
    uint64_t inodes_walked = 0;
    uint64_t nodes = 0, inner = 0, leaves = 0, records = 0;

    uint64_t namedata_active_dirs = 0;  /* 非空名字面目录数（direct + tree） */
    uint64_t namedata_tree_blk    = 0;  /* 名字映射树 + fr 树节点块合计（2 块/节点） */
    uint64_t namedata_data_blk    = 0;  /* 名字数据片物理块合计 */
    uint64_t namedata_pieces      = 0;  /* 名字数据片总数（8K 片） */
    uint64_t namedata_xor_unavailable_dirs = 0; /* F5：某目录 fr 不可读 → 该目录名字 XOR 不可用 */

    uint64_t inodeplane_is_tree = 0;    /* 1 tree / 0 direct */
    uint64_t inodeplane_pieces  = 0;    /* 8K 数据面片数 */
    uint64_t inodeplane_data_blk = 0;   /* 数据片物理块合计 */
    uint64_t inodeplane_tree_blk = 0;   /* 映射树节点块合计 */
    uint64_t inodeplane_fr_blk   = 0;   /* 扁 fr 快照区占块 */
    bool     inode_xor_available = true;/* F5 软失败：inode fr 不可读 → false（inode XOR 不出结论） */

    /* 汇总（收口后 verdict 依据；ERROR 求和，WARN 只计数） */
    uint64_t total_error() const {
        return sb_bad + bitmap_bad + btree_bad + inode_bad + inodeplane_bad +
               namedata_bad + xattr_bad + multi_ref_ino + multi_claim_blk +
               link_mismatch + dotdot_mismatch + inline_residual + dentry_xcheck_mismatch +
               block.unprotected + block.leaked +
               namedata.wild_free + namedata.overlap +
               inode.unprotected + inode.leaked;
    }
    /* WARN：策略/优化观察。含名字面「已分配未引用」泄漏（空间未回收，非契约违例）。 */
    uint64_t total_warn() const { return warn_frag + warn_no_downgrade + namedata.leaked; }
};

const char *severity_str(severity_t s);
const char *scope_str(scope_t s);

} /* namespace rfs_audit */

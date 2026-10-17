#pragma once
/* ═══════════════════════════════════════════════════════════════════════════
 * audit.hpp — audit 顶层独立项目的「类 / 函数」总面
 *
 * 对应审计框架 v1（audit/DISCIPLINE.md）。相位 → 类/函数 映射：
 *
 *   引导          audit_image::open_bootstrap()            // include/audit/audit_image.hpp
 *                 └─ 五道 fail-fast F0–F4（唯一提前退出口，exit 3）
 *   相位 A1 翻译层 inode_plane_auditor → inode_resolver      // inum→piece→phys→slot
 *   相位 A2 分配账本 inode_plane_auditor → inode_alloc_ledger // 扁 fr 快照补集
 *   相位 B  根 DFS  audit_session::phase_b_dfs()            // 对象单位=inode，走到底
 *            ├─ 核心    audit_session::inode_audit(inum)     // 访问 + 类型分派；**不递归**
 *            │           └─ inode_audit_dir 走 ddentry 树     // traversal A（结构审计）
 *            └─ 驱动    audit_session::dfs_descend(inum)     // 唯一递归点
 *                        └─ enumerate_dir_children 再走一遍   // traversal B（导航枚举）
 *                        A/B 条目集对拍 → dentry_xcheck_mismatch（**刻意冗余**）
 *   相位 C  收口     audit_session::phase_c_reconcile()      // 三空间 XOR
 *   顶层入口        run_audit()                              // 路径→引导→A/B/C→verdict
 *
 * 输出：全部 → stdout 单流（含 fail-fast 原因）；stderr 仅用法/IO。
 *       行 = [AUDIT][E|W|I][SCOPE] …；详细度靠 verbosity（normal/walk/raw）。
 * 信任边界：只信裸盘字节 + disk_struct_v2.0.h 契约；不链 god。
 * 信任传播点：A1 必须完整；解析不到的 inode 报 ERROR，绝不静默读错。
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "audit/audit_types.hpp"
#include "audit/audit_image.hpp"
#include "magics.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace rfs_audit {

#if defined(__GNUC__)
#  define AUDIT_PRINTF_ATTR(fmt, arg) __attribute__((format(printf, fmt, arg)))
#else
#  define AUDIT_PRINTF_ATTR(fmt, arg)
#endif

/* ── 原始 trace 总开关 ─────────────────────────────────────────────────────
 * 逐 inode / 逐节点的 [TRACE] 明细体量与耗时巨大（2TB ≈ 1.8M 行 / ~250MB），
 * 默认关；由 CLI `--trace` 置位后打印到 stdout，外部自行重定向。
 * 用宏而非函数：关时连实参（path_str / sanitize 等）都不求值。 */
extern bool g_audit_trace;
#define AUDIT_TRACE(...) \
    do { if (rfs_audit::g_audit_trace) std::printf(__VA_ARGS__); } while (0)

/* 52 位无效指针（ptr_in_idx.ptr / parent_ptr_t.ptr 的位域截断形态） */
constexpr uint64_t AUDIT_PTR52 = (1ULL << 52) - 1;

/* ── 详细度（轴1 报告分层）── */
enum class verbosity_t : uint8_t { normal = 0, walk = 1, raw = 2 };

struct dfs_ctx_t;   /* 前向：相位 A 的账本/翻译层构建要往块影子图认领 */

/* ═══════════════════════════════════════════════════════════════════════════
 * audit_reporter — stdout 单流输出（唯一写 stdout 的类）
 * ═══════════════════════════════════════════════════════════════════════════ */
class audit_reporter {
public:
    explicit audit_reporter(FILE *out = stdout, verbosity_t v = verbosity_t::normal);

    void error(scope_t s, const char *check, const char *fmt, ...) AUDIT_PRINTF_ATTR(4, 5);
    void warn (scope_t s, const char *check, const char *fmt, ...) AUDIT_PRINTF_ATTR(4, 5);
    void info (scope_t s, const char *fmt, ...)                     AUDIT_PRINTF_ATTR(3, 4);
    void walk (scope_t s, const char *fmt, ...)                     AUDIT_PRINTF_ATTR(3, 4);
    void raw  (scope_t s, const char *fmt, ...)                     AUDIT_PRINTF_ATTR(3, 4);

    void summary(const audit_summary_t &sum, verdict_t v);

    verbosity_t verbosity() const { return v_; }
    FILE       *out()       const { return out_; }
    uint64_t    errors()    const { return err_; }
    uint64_t    warnings()  const { return warn_; }

private:
    void emit(severity_t sev, scope_t s, const char *check, const char *fmt, va_list ap);
    FILE       *out_;
    verbosity_t v_;
    uint64_t    err_ = 0, warn_ = 0, info_ = 0;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 相位 A1 — inode 面翻译层（DFS 的硬前置）
 *   direct 态：phys = ctl_ptr + (idx>>4)；
 *   tree  态：走映射树铺 piece_phys_（逻辑片 → 物理首块）。
 * ═══════════════════════════════════════════════════════════════════════════ */
class inode_resolver {
public:
    bool build(const audit_image &img, audit_reporter &rep, audit_summary_t &sum, dfs_ctx_t &c,
               bool &mapping_ok);

    bool     available()   const { return available_; }
    bool     is_direct()   const { return direct_; }
    uint64_t inode_count() const { return inode_count_; }
    uint64_t pieces()      const { return pieces_; }

    /* inum → 承载它的物理 FS_block（trace/布局挖掘用；不可解返回 AUDIT_PTR52） */
    uint64_t phys_block(uint64_t inum) const;

    const uint8_t       *slot (uint64_t inum) const;
    const raven_Inode_t *inode(uint64_t inum) const;

private:
    const audit_image *img_ = nullptr;
    bool     available_ = false;
    bool     direct_    = false;
    uint64_t pieces_    = 0;
    uint64_t inode_count_ = 0;
    uint64_t base_blk_  = 0;                /* direct 态段基块 */
    std::vector<uint64_t> piece_phys_;      /* 逻辑 8K 片 → 物理首块；AUDIT_PTR52=不可解 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 相位 A2 — inode 分配账本（扁 fr 快照补集）
 * ═══════════════════════════════════════════════════════════════════════════ */
class inode_alloc_ledger {
public:
    void build(const audit_image &img, audit_reporter &rep, audit_summary_t &sum, dfs_ctx_t &c,
               bool &fr_ok);

    bool     available()   const { return available_; }
    bool     allocated(uint64_t inum) const;
    uint64_t inode_count() const { return inode_count_; }

private:
    std::vector<uint8_t> alloc_;   /* 1 bit / inode（1=已分配） */
    uint64_t inode_count_ = 0;
    bool     available_   = false;
};

/* ── DIR 子项交接单位 ── */
struct child_ref_t {
    uint64_t    inum;
    std::string name;
};

/* ── 名字数据面解析（每目录瞬态）── */
struct name_extent_t { uint64_t lblk; uint64_t phys; uint64_t len; };   /* 4K 单位 */
struct name_ref_t    { uint64_t off;  uint64_t len; };                  /* 字节单位 */

/* ── 单目录名字数据面瞬态 ──────────────────────────────────────────────
 * 生命周期 = dfs_descend 的一次目录节点迭代（覆盖 traversal A → B）；
 * 由 dfs_descend 栈持有，**绝不进 dfs_ctx_t**。非 DIR inode 不触碰。 */
struct namedata_ctx_t {
    bool     active    = false;   /* name_data_ctl 非空态（数据面已激活） */
    bool     is_direct = false;   /* direct 段 / tree 映射 */
    uint64_t ptr       = 0;       /* direct 段基块 */
    uint64_t used_blk  = 0;       /* 映射覆盖的逻辑 4K 块数（= pieces×2） */
    std::vector<name_extent_t> map;                   /* tree 态逻辑→物理（4K 单位） */
    std::vector<name_ref_t>    refs;                  /* 长名引用画像（ddentry 采集，字节） */
    std::vector<std::pair<uint64_t, uint64_t>> free;  /* fr 空闲 (base,len) 字节 */
    uint64_t fr_count  = 0;
    bool     fr_ok     = false;   /* fr 可读（空态/inline/tree 均已解析） */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 相位 B — DFS 全局上下文（影子图 + 白名单 + 路径栈；不含名字面瞬态）
 * ═══════════════════════════════════════════════════════════════════════════ */
struct dfs_ctx_t {
    const audit_image        *img   = nullptr;
    audit_reporter           *rep   = nullptr;
    audit_summary_t          *sum   = nullptr;
    const inode_resolver     *res   = nullptr;
    const inode_alloc_ledger *alloc = nullptr;

    std::vector<uint8_t> shadow_blk;   /* 1 bit / FS_block；兼「已认领」 */
    std::vector<uint8_t> shadow_name;  /* 1 bit / FS_block；名字数据面占用子集（相位 C 归因用） */
    std::vector<uint8_t> shadow_ino;   /* 1 bit / inode；兼「已访问」 */
    uint64_t blk_bits = 0;
    uint64_t ino_bits = 0;

    uint64_t meta_lo = 0, meta_hi = 0;
    std::vector<uint64_t> backup_blocks;

    std::vector<uint64_t>    path_inum;
    std::vector<std::string> path_name;
    int depth = 0;
    int max_depth = 64;

    /* traversal A 结果（仅 DIR 有效）；dfs_descend 消费后与 B 对拍 */
    std::vector<child_ref_t> dir_entries_audited;

    /* 影子图原语 */
    bool blk_test(uint64_t b) const {
        return b < blk_bits && (shadow_blk[b >> 3] & (uint8_t)(1u << (b & 7)));
    }
    bool blk_set(uint64_t b) {
        if (b >= blk_bits) return false;
        const uint8_t m = (uint8_t)(1u << (b & 7));
        if (shadow_blk[b >> 3] & m) return false;
        shadow_blk[b >> 3] |= m; return true;
    }
    /* 名字数据面占用标记（与 blk_set 并行；不参与「已认领」去重，纯归因） */
    void name_set(uint64_t b) {
        if (b >= blk_bits) return;
        shadow_name[b >> 3] |= (uint8_t)(1u << (b & 7));
    }
    bool ino_test(uint64_t i) const {
        return i < ino_bits && (shadow_ino[i >> 3] & (uint8_t)(1u << (i & 7)));
    }
    bool ino_set(uint64_t i) {
        if (i >= ino_bits) return false;
        const uint8_t m = (uint8_t)(1u << (i & 7));
        if (shadow_ino[i >> 3] & m) return false;
        shadow_ino[i >> 3] |= m; return true;
    }
    bool is_backup_blk(uint64_t b) const {
        for (uint64_t x : backup_blocks) if (x == b) return true;
        return false;
    }
    bool in_range(uint64_t b) const {
        return img && b < img->geo().total_blocks;
    }
};

/* ── 一棵树 DFS 的瞬态工作态 ── */
enum class tree_kind_t : uint8_t {
    dentry, extent, name_extent, inode_extent, name_fr,
};
struct tree_walk_t {
    std::vector<uint64_t>  leaves;
    uint64_t leaf_depth = 0;
    bool     leaf_depth_set = false;
    uint64_t records = 0;
    uint64_t records_total = 0;              /* 同 records（对 dir size） */
    std::vector<child_ref_t> entries;        /* dentry 叶条目（traversal A） */
};
struct fr_walk_t {
    std::vector<uint64_t> leaves;
    uint64_t leaf_depth = 0;
    bool     leaf_depth_set = false;
    uint64_t records = 0;
    bool     have_last = false;
    uint64_t last_len = 0, last_base = 0;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 相位 A — inode 数据面审计器（A1 翻译层 + A2 分配账本）
 * ═══════════════════════════════════════════════════════════════════════════ */
class inode_plane_auditor {
public:
    struct result_t { bool mapping_ok = false; bool fr_ok = false; };

    result_t audit(const audit_image &img, audit_reporter &rep, audit_summary_t &sum, dfs_ctx_t &c);

    const inode_resolver     &resolver() const { return res_; }
    const inode_alloc_ledger &alloc()    const { return alloc_; }

private:
    inode_resolver     res_;
    inode_alloc_ledger alloc_;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * audit_session — 编排：A → B → C
 * ═══════════════════════════════════════════════════════════════════════════ */
class audit_session {
public:
    audit_session(const audit_image &img, audit_reporter &rep);

    bool phase_a_inode_plane();
    void phase_b_dfs();
    void phase_c_reconcile();

    /* 核心：单 inode 审计（访问 + 类型分派；**不递归**）。nd 仅 DIR 非空。 */
    void inode_audit(dfs_ctx_t &c, uint64_t inum, uint64_t parent_inum, namedata_ctx_t *nd);

    const audit_summary_t &summary() const { return sum_; }
    verdict_t verdict() const {
        return sum_.total_error() ? verdict_t::has_error : verdict_t::pass;
    }

private:
    /* 类型专属（inode_audit 分派目标；均不跨 inode 递归） */
    void inode_audit_dir    (dfs_ctx_t &c, uint64_t inum, uint64_t parent, const raven_Inode_t *ino,
                             namedata_ctx_t &nd);
    void inode_audit_reg    (dfs_ctx_t &c, uint64_t inum, const raven_Inode_t *ino);
    void inode_audit_lnk    (dfs_ctx_t &c, uint64_t inum, const raven_Inode_t *ino);
    void inode_audit_special(dfs_ctx_t &c, uint64_t inum, const raven_Inode_t *ino);
    void audit_xattr        (dfs_ctx_t &c, uint64_t inum, const raven_Inode_t *ino);

    /* 外部下钻驱动（唯一递归点）+ traversal B；name = 父目录给的条目名（根为 nullptr） */
    void dfs_descend(dfs_ctx_t &c, uint64_t inum, uint64_t parent_inum, const char *name);
    void enumerate_dir_children(dfs_ctx_t &c, const raven_dir_Inode_t *dino,
                                const namedata_ctx_t &nd, std::vector<child_ref_t> &out);

    /* B+树机械（dentry/extent 两族同构，kind 决定魔数+键语义）。
     * nd 仅 dentry 族非空（inode_audit_dir 提供），extent 族传 nullptr。 */
    uint64_t walk_btree(dfs_ctx_t &c, tree_kind_t kind, namedata_ctx_t *nd, uint64_t blk,
                        uint64_t owner_inum, uint64_t expect_parent, uint64_t dotdot_expect,
                        bool expect_root, uint64_t size_bound, int depth, bool &sub_ok,
                        tree_walk_t &w);
    void walk_leaf_chain(dfs_ctx_t &c, tree_kind_t kind, const tree_walk_t &w,
                         uint64_t root_blk, uint64_t owner_inum);

    /* dentry 叶记录处理（长名画像 + hash + 序），自 walk_btree 抽出 */
    uint64_t audit_dentry_leaf(dfs_ctx_t &c, namedata_ctx_t &nd, uint64_t owner_inum, uint64_t blk,
                               const Bptree_ddentry_leaf_node_t *leaf, uint16_t it, tree_walk_t &w);

    /* 名字数据面（映射树 + fr） */
    void     walk_name_plane(dfs_ctx_t &c, namedata_ctx_t &nd, uint64_t inum, const raven_dir_Inode_t *d);
    uint64_t walk_name_tree (dfs_ctx_t &c, namedata_ctx_t &nd, uint64_t owner, uint64_t blk,
                             uint64_t expect_parent, bool expect_root, int depth, uint64_t limit,
                             bool &sub_ok, uint64_t &tree_blocks, uint64_t &data_blocks);
    void     walk_name_fr   (dfs_ctx_t &c, namedata_ctx_t &nd, uint64_t inum, const raven_dir_Inode_t *d);
    void     walk_fr_tree   (dfs_ctx_t &c, namedata_ctx_t &nd, uint64_t owner, uint64_t blk,
                             uint64_t expect_parent, bool expect_root, int depth, uint64_t limit,
                             uint64_t &node_blocks, uint64_t &records);
    bool     name_data_read (const namedata_ctx_t &nd, uint64_t offset, uint16_t len, char *out);

    /* 收口 */
    void reconcile_block(dfs_ctx_t &c);
    void reconcile_namedata(dfs_ctx_t &c);       /* 名字面块占用归因（子集于 block 面） */
    void reconcile_namedata_dir(const namedata_ctx_t &nd, uint64_t inum);
    void reconcile_inode(dfs_ctx_t &c);

    void bad(scope_t s, const char *check, const char *fmt, ...) AUDIT_PRINTF_ATTR(4, 5);

    const audit_image *img_;
    audit_reporter    *rep_;
    audit_summary_t    sum_;
    inode_plane_auditor plane_;
    dfs_ctx_t           ctx_;
};

/* 顶层入口：路径 → 引导（五门）→ 相位 A/B/C → 裁决 */
verdict_t run_audit(const char *path, FILE *out = stdout,
                    verbosity_t v = verbosity_t::normal,
                    audit_summary_t *summary_out = nullptr);

/* 形态判据（size/direct_bit；事实源 disk_struct_v2.0.h） */
file_form_t form_of(const raven_Inode_t *ino);

} /* namespace rfs_audit */

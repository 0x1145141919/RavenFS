#include "audit/audit.hpp"

#include <cstdarg>
#include <cstring>

namespace rfs_audit {

audit_reporter::audit_reporter(FILE *out, verbosity_t v) : out_(out ? out : stdout), v_(v) {}

void audit_reporter::emit(severity_t sev, scope_t s, const char *check, const char *fmt, va_list ap)
{
    char body[1400];
    vsnprintf(body, sizeof(body), fmt, ap);
    if (sev == severity_t::error)      err_++;
    else if (sev == severity_t::warn)  warn_++;
    else                               info_++;
    /* [AUDIT][E|W|I][SCOPE] check: detail  —— 单流 stdout，可 grep */
    if (check && check[0])
        fprintf(out_, "[AUDIT][%s][%s] %s: %s\n", severity_str(sev), scope_str(s), check, body);
    else
        fprintf(out_, "[AUDIT][%s][%s] %s\n", severity_str(sev), scope_str(s), body);
}

void audit_reporter::error(scope_t s, const char *check, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); emit(severity_t::error, s, check, fmt, ap); va_end(ap);
}
void audit_reporter::warn(scope_t s, const char *check, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); emit(severity_t::warn, s, check, fmt, ap); va_end(ap);
}
void audit_reporter::info(scope_t s, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); emit(severity_t::info, s, nullptr, fmt, ap); va_end(ap);
}
void audit_reporter::walk(scope_t s, const char *fmt, ...)
{
    if (v_ < verbosity_t::walk) return;
    va_list ap; va_start(ap, fmt); emit(severity_t::info, s, "WALK", fmt, ap); va_end(ap);
}
void audit_reporter::raw(scope_t s, const char *fmt, ...)
{
    if (v_ < verbosity_t::raw) return;
    va_list ap; va_start(ap, fmt); emit(severity_t::info, s, "RAW", fmt, ap); va_end(ap);
}

void audit_reporter::summary(const audit_summary_t &sum, verdict_t v)
{
    const space_ledger_t &b = sum.block, &n = sum.namedata, &i = sum.inode;
    fprintf(out_, "[AUDIT][SUM] inodes=%llu nodes=%llu inner=%llu leaves=%llu recs=%llu\n",
            (unsigned long long)sum.inodes_walked, (unsigned long long)sum.nodes,
            (unsigned long long)sum.inner, (unsigned long long)sum.leaves,
            (unsigned long long)sum.records);
    fprintf(out_,
            "[AUDIT][SUM] errors=%llu warnings=%llu | "
            "btree=%llu inode=%llu namedata=%llu inodeplane=%llu xattr=%llu | "
            "multi_ref=%llu multi_claim=%llu link=%llu dotdot=%llu inline_resid=%llu xcheck=%llu\n",
            (unsigned long long)sum.total_error(), (unsigned long long)sum.total_warn(),
            (unsigned long long)sum.btree_bad, (unsigned long long)sum.inode_bad,
            (unsigned long long)sum.namedata_bad, (unsigned long long)sum.inodeplane_bad,
            (unsigned long long)sum.xattr_bad, (unsigned long long)sum.multi_ref_ino,
            (unsigned long long)sum.multi_claim_blk, (unsigned long long)sum.link_mismatch,
            (unsigned long long)sum.dotdot_mismatch, (unsigned long long)sum.inline_residual,
            (unsigned long long)sum.dentry_xcheck_mismatch);
    fprintf(out_,
            "[AUDIT][SUM][BLOCK] owned=%llu alloc=%llu unprotected=%llu leaked=%llu\n",
            (unsigned long long)b.owned, (unsigned long long)b.allocated,
            (unsigned long long)b.unprotected, (unsigned long long)b.leaked);
    /* owned/alloc/unprotected = 名字面块归因（子集于 BLOCK 面，检测仍由 BLOCK 面定夺）；
     * leaked_piece = 相位 B 的逻辑片级「已分配未引用」。 */
    fprintf(out_,
            "[AUDIT][SUM][NAMEDATA] owned=%llu alloc=%llu unprotected=%llu "
            "wild_free=%llu overlap=%llu leaked_piece=%llu\n",
            (unsigned long long)n.owned, (unsigned long long)n.allocated,
            (unsigned long long)n.unprotected, (unsigned long long)n.wild_free,
            (unsigned long long)n.overlap, (unsigned long long)n.leaked);
    fprintf(out_,
            "[AUDIT][SUM][INODE] owned=%llu alloc=%llu unprotected=%llu leaked=%llu "
            "xor_available=%d\n",
            (unsigned long long)i.owned, (unsigned long long)i.allocated,
            (unsigned long long)i.unprotected, (unsigned long long)i.leaked,
            (int)sum.inode_xor_available);
    fprintf(out_,
            "[AUDIT][SUM][NAMEDATA_OBS] active_dirs=%llu tree_blk=%llu data_blk=%llu pieces=%llu "
            "xor_unavail_dirs=%llu\n",
            (unsigned long long)sum.namedata_active_dirs, (unsigned long long)sum.namedata_tree_blk,
            (unsigned long long)sum.namedata_data_blk, (unsigned long long)sum.namedata_pieces,
            (unsigned long long)sum.namedata_xor_unavailable_dirs);
    fprintf(out_,
            "[AUDIT][SUM][INODEPLANE_OBS] mode=%s pieces=%llu data_blk=%llu tree_blk=%llu fr_blk=%llu\n",
            sum.inodeplane_is_tree ? "tree" : "direct", (unsigned long long)sum.inodeplane_pieces,
            (unsigned long long)sum.inodeplane_data_blk, (unsigned long long)sum.inodeplane_tree_blk,
            (unsigned long long)sum.inodeplane_fr_blk);
    const char *vs = (v == verdict_t::pass) ? "PASS"
                   : (v == verdict_t::has_error) ? "HAS_ERROR"
                   : (v == verdict_t::fail_fast) ? "FAIL_FAST" : "IO";
    fprintf(out_, "[AUDIT][VERDICT] %s exit=%d\n", vs, (int)v);
}

} /* namespace rfs_audit */

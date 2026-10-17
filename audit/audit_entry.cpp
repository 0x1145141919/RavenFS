/* ═══════════════════════════════════════════════════════════════════════════
 * audit_entry.cpp — 顶层入口 run_audit（路径 → 引导五门 → A/B/C → 裁决）
 *
 * 语义（框架 v1）：
 *   · fail-fast F0–F4 → verdict=fail_fast，**不进入 DFS**；原因打 stdout。
 *   · IO/用法 → verdict=io。
 *   · 引导非门 BAD（SB crc / 位图自述）只记账，继续 A/B/C。
 *   · DFS 一旦开始走到底；最终 exit 由 total_error 决定。
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "audit/audit.hpp"

#include <string>

namespace rfs_audit {

/* 原始 [TRACE] 明细总开关（定义；CLI --trace 置位，默认关） */
bool g_audit_trace = false;

verdict_t run_audit(const char *path, FILE *out, verbosity_t v, audit_summary_t *summary_out)
{
    if (!out) out = stdout;
    audit_reporter rep(out, v);

    audit_image img;
    std::string err;
    gate_t g = img.open_bootstrap(path, err);
    if (g != gate_t::OK) {
        if (g == gate_t::IO) {
            fprintf(stderr, "error: %s: %s\n", path, err.c_str());
            return verdict_t::io;
        }
        /* fail-fast：原因走 stdout（唯一提前退出口） */
        fprintf(out, "[AUDIT][E][BOOT] FAIL_FAST %s: %s\n", gate_str(g), err.c_str());
        if (summary_out) *summary_out = audit_summary_t{};
        return verdict_t::fail_fast;
    }

    rep.info(scope_t::boot, "=== RavenFS v2.0 audit (independent) : %s ===", path);
    img.dump(out);
    const bootstrap_report_t &br = img.report();
    if (!br.crc_ok)
        rep.error(scope_t::boot, "SB_CRC", "superblock crc32 不符（非门 BAD，继续审计）");
    if (!br.bitmap_tail_ok)
        rep.error(scope_t::boot, "BM_TAIL", "块位图自述段 [bitmap_start,total_blocks) 非全 1");

    audit_session sess(img, rep);
    sess.phase_a_inode_plane();
    sess.phase_b_dfs();
    sess.phase_c_reconcile();

    audit_summary_t s = sess.summary();
    if (!br.crc_ok)          s.sb_bad++;
    if (!br.bitmap_tail_ok)  s.bitmap_bad++;

    const verdict_t vd = s.total_error() ? verdict_t::has_error : verdict_t::pass;
    rep.summary(s, vd);
    if (summary_out) *summary_out = s;
    return vd;
}

} /* namespace rfs_audit */

/* ═══════════════════════════════════════════════════════════════════════════
 * audit_cli.cpp — audit 顶层独立项目 CLI（路径参数）
 *
 * 用法： ./audit [--trace] [--free-leaks] [-v|-vv] <image-or-device>
 *   -v      : 逐 inode WALK 摘要；-vv : 逐节点/记录 RAW（reporter 详细度）
 *   --trace : 打开逐 inode/逐节点原始 [TRACE] 明细（体量大，建议重定向）
 *   --free-leaks : [FORK/临时] 发现一个泄露→修复一个：就地清盘上位图分配位（写盘！）
 *                  仅用于未挂载的盘；默认关。
 *
 * 全部审计输出 → stdout 单流；stderr 仅用法/IO。
 * 退出码 = verdict：0 pass / 1 has_error / 2 io / 3 fail_fast。
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "audit/audit.hpp"
#include "leakscan_glue.hpp"

#include <cstdio>
#include <cstring>

using namespace rfs_audit;

int main(int argc, char **argv)
{
    verbosity_t v = verbosity_t::normal;
    const char *path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "-v"))       v = verbosity_t::walk;
        else if (!std::strcmp(argv[i], "-vv")) v = verbosity_t::raw;
        else if (!std::strcmp(argv[i], "--trace") || !std::strcmp(argv[i], "-t")) g_audit_trace = true;
        else if (!std::strcmp(argv[i], "--free-leaks")) g_free_leaks = true;
        else if (argv[i][0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        std::fprintf(stderr, "usage: %s [--trace] [--free-leaks] [-v|-vv] <image-or-device>\n", argv[0]);
        return 2;
    }

    verdict_t vd = run_audit(path, stdout, v, nullptr);
    return (int)vd;
}

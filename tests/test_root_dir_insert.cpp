/* ═══════════════════════════════════════════════════════════════════
 * test_root_dir_insert.cpp — 目录条目插入接口专项测试（v1.3）
 *
 * 覆盖（root_dir_insert_dentry / dir_insert_dentry）：
 *   1. 边界/异常输入：空名、39/40/41/552/553B、重复插入、大小写、
 *      空格、UTF-8、斜杠、控制字符。
 *      —— 空名 / 含斜杠 = fs 级非法名 → RC_ERR_NAME_INVALID（2026-10-05）。
 *   2. 批量插入：覆盖叶分裂 → 根分裂（建新根）。
 *   3. 长名批量（41..552B）：插入即 lookup（名字数据面映射树回读），
 *      重复长名 → EXIST（dentry_name_cmp 尾部比较）。
 *   4. 树完整性 DFS（共享 dir_tree_check.hpp）：复合序 / hint 单调 /
 *      父子一致 / 叶链双向；audit_dfs_root 全净。
 *   5. 持久化：析构 flush → 重挂载 → 树内容与长名回读不变。
 *
 * 只调用 insert / lookup（不碰 rm）；校验器直读内部状态（private public）。
 * 用法：ravenfs_juv_insert <device> [batch_count=1000] [stop_at=0]
 *   batch_count 默认 1000 —— 适配 64MiB 镜像（≈2048 inode）；更大规模
 *   请换更大镜像。64MiB 下最深到「根分裂（depth 2）」，内部节点分裂需
 *   ~8000 条目（超镜像 inode 预算），故不在默认覆盖内。
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "dir_tree_check.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>

/* ── 用例 ── */
struct Case { const char *name; result_code_t expect; const char *desc; };

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device> [batch_count=1000] [stop_at=0]\n", argv[0]); return 2; }
    int batch_n = 1000;
    if (argc >= 3) batch_n = atoi(argv[2]);
    if (batch_n < 1) batch_n = 1;
    int stop_at = 0;   /* >0：二分复播，只插前 stop_at 条后 verify 退出 */
    if (argc >= 4) stop_at = atoi(argv[3]);
    if (stop_at < 0) stop_at = 0;
    int insert_n = (stop_at > 0 && stop_at < batch_n) ? stop_at : batch_n;

    printf("════════════════════════════════════════════════\n"
           " RavenFS v1.3 dir_insert_dentry 专项测试\n"
           " device: %s   batch: %d   stop_at: %d%s\n"
           "════════════════════════════════════════════════\n",
           argv[1], batch_n, insert_n, stop_at > 0 ? "  (二分复播)" : "");

    uint64_t rc = 0;
    uint64_t inserted = 0;
    const int long_n = 300;   /* 长名批量条数（Phase 2b 与 Phase 3 回读共用） */

    linux_userspace_mount_t mnt = make_mount(argv[1]);   /* mmap 后端（审计前提） */
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }

    /* ══ Phase 0~2：挂载 → 边界 → 批量（作用域块，析构时位图全量写回） ══ */
    {
    RavenFS_juvenile fs(mnt, &rc);
    CHECK(fs.is_mounted(), "mount rc=%s", rc_name(rc));
    if (!fs.is_mounted()) { printf("挂载失败，测试中止（先确认已 mkfs）\n"); return 1; }
    fs.dump_info();

    /* ══ Phase 1：边界 / 异常输入 ══ */
    printf("\n--- Phase 1: 边界与异常 ---\n");
    char long39[40], long40[41], long41[42], long552[553], long553[554];
    memset(long39, 'x', 39); long39[39] = 0;
    memset(long40, 'y', 40); long40[40] = 0;
    memset(long41, 'z', 41); long41[41] = 0;
    memset(long552, 'w', 552); long552[552] = 0;   /* RAVENFS_MAX_NAME_LEN 上限 */
    memset(long553, 'v', 553); long553[553] = 0;
    static const char ctrl[] = { 0x01, 0x02, 0x03, 0 };

    const Case cases[] = {
        { "",            RC_ERR_NAME_INVALID, "空名（fs 级非法：首字节 NUL）" },
        { long40,        RC_OK,            "40B 边界（全内联上限，无外置）" },
        { long41,        RC_OK,            "41B 长名（字节粒度外置 1B）" },
        { "a",           RC_OK,            "正常单字符" },
        { "a",           RC_ERR_EXIST,     "重复插入同名字" },
        { "A",           RC_OK,            "大小写敏感（a/A 不同）" },
        { "a",           RC_ERR_EXIST,     "再次重复插入" },
        { long39,        RC_OK,            "39B 正好内联上限内" },
        { long552,       RC_OK,            "552B 上限长名（外置 512B）" },
        { long553,       RC_ERR_NAME_LEN,  "553B 超限（RAVENFS_MAX_NAME_LEN=552）" },
        { "foo bar",     RC_OK,            "含空格" },
        { "中文目录项",    RC_OK,            "UTF-8 多字节" },
        { "dir/sub",     RC_ERR_NAME_INVALID, "含斜杠（fs 级非法字符）" },
        { ctrl,          RC_OK,            "控制字符 0x01-0x03" },
    };
    for (const auto &c : cases) {
        result_code_t r = fs.root_dir_insert_dentry(c.name, INODE_TYPE_REGULAR);
        bool ok = (r == c.expect);
        if (ok) inserted += (c.expect == RC_OK);
        printf("  [%s] insert(\"%s\") → %-16s expect %-16s  %s\n",
               ok ? "PASS" : "FAIL", c.name, rc_name(r), rc_name(c.expect), c.desc);
        ok ? g_pass++ : g_fail++;
    }

    /* 空名再插一次：fs 级非法，在任何分配/树访问前拒绝（幂等，无副作用） */
    {
        result_code_t r = fs.root_dir_insert_dentry("", INODE_TYPE_REGULAR);
        bool ok = (r == RC_ERR_NAME_INVALID);
        printf("  [%s] insert(\"\") 再次 → %s（expect RC_ERR_NAME_INVALID）\n",
               ok ? "PASS" : "FAIL", rc_name(r));
        ok ? g_pass++ : g_fail++;
    }

    dir_verify_tree(fs, inserted, "Phase 1 后");

    /* 长名重复插入：hash 相同 → 叶内完整名字比较（dentry_name_cmp 读数据面尾部）→ EXIST */
    {
        result_code_t r = fs.root_dir_insert_dentry(long41, INODE_TYPE_REGULAR);
        bool ok = (r == RC_ERR_EXIST);
        printf("  [%s] insert(41B) 再次 → %s（expect RC_ERR_EXIST，尾部比较命中）\n",
               ok ? "PASS" : "FAIL", rc_name(r));
        ok ? g_pass++ : g_fail++;
    }

    /* ══ Phase 2：批量插入（叶分裂 → 根分裂） ══ */
    printf("\n--- Phase 2: 批量插入 %d 条目 ---\n", insert_n);
    char nb[64];
    bool batch_ok = true;
    uint64_t fail_at = 0;
    result_code_t fail_rc = RC_OK;
    int progress = insert_n >= 1000 ? insert_n / 8 : 1;
    for (int i = 0; i < insert_n; i++) {
        snprintf(nb, sizeof(nb), "f%05d", i);
        result_code_t r = fs.root_dir_insert_dentry(nb, INODE_TYPE_REGULAR);
        if (r != RC_OK) { batch_ok = false; fail_at = i; fail_rc = r; break; }
        inserted++;
        if (progress >= 1 && (i + 1) % progress == 0)
            printf("  ... %d/%d 已插入（累计条目 %llu）\n",
                   i + 1, insert_n, (unsigned long long)inserted);
    }
    if (batch_ok) printf("  批量插入全部 RC_OK\n");
    else printf("  批量插入在 f%05llu 失败: %s（树内条目 %llu）\n",
                (unsigned long long)fail_at, rc_name(fail_rc), (unsigned long long)inserted);
    CHECK(batch_ok, "批量 %d 全成功", insert_n);
    fs.dump_info();
    dir_verify_tree(fs, inserted, "Phase 2 后");

    /* ══ Phase 2b：长名批量（字节粒度外置 + 即时 lookup 回读） ══ */
    printf("\n--- Phase 2b: 长名批量 300 条（41..552B） ---\n");
    char lname[RAVENFS_MAX_NAME_LEN + 1];
    char l0[RAVENFS_MAX_NAME_LEN + 1];
    bool long_ok = true;
    uint64_t long_fail_at = 0;
    result_code_t long_fail_rc = RC_OK;
    for (int i = 0; i < long_n; i++) {
        dir_make_long_name(lname, i);
        if (i == 0) memcpy(l0, lname, sizeof(l0));
        result_code_t r = fs.root_dir_insert_dentry(lname, INODE_TYPE_REGULAR);
        if (r != RC_OK) { long_ok = false; long_fail_at = i; long_fail_rc = r; break; }
        inserted++;
        /* 即时抽查：插入后 lookup（完整名字比较读名字数据面） */
        uint64_t inum = 0;
        result_code_t lr = fs.root_dir_lookup(lname, &inum);
        if (lr != RC_OK) { long_ok = false; long_fail_at = i; long_fail_rc = lr; break; }
    }
    if (long_ok) printf("  长名批量 %d 条全部 RC_OK + lookup 命中\n", long_n);
    else printf("  长名批量在 #%llu 失败: %s（树内条目 %llu）\n",
                (unsigned long long)long_fail_at, rc_name(long_fail_rc), (unsigned long long)inserted);
    CHECK(long_ok, "长名批量 %d 全成功且可查", long_n);

    /* 长名重复插入 → EXIST（尾部比较） */
    {
        result_code_t r = fs.root_dir_insert_dentry(l0, INODE_TYPE_REGULAR);
        bool ok = (r == RC_ERR_EXIST);
        printf("  [%s] 重复长名 → %s（expect RC_ERR_EXIST）\n", ok ? "PASS" : "FAIL", rc_name(r));
        ok ? g_pass++ : g_fail++;
    }

    dir_verify_tree(fs, inserted, "Phase 2b 后");
    CHECK(audit_clean(fs, "insert-final") == 0, "audit_dfs_root 全净 (insert)");
    }   /* ← fs 析构：位图全量写回 */

    if (stop_at > 0) {
        printf("\n════════════════════════════════════════════════\n"
               " SUMMARY(bisect stop_at=%d): %d passed, %d failed\n"
               "════════════════════════════════════════════════\n",
               insert_n, g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    /* ══ Phase 3：持久化（重挂载 → 树 + 长名回读） ══ */
    printf("\n--- Phase 3: 持久化验证（重挂载） ---\n");
    {
        RavenFS_juvenile fs2(mnt, &rc);
        CHECK(fs2.is_mounted(), "重挂载 rc=%s", rc_name(rc));
        if (fs2.is_mounted()) {
            fs2.dump_info();
            dir_verify_tree(fs2, inserted, "重挂载后");
            /* 长名尾部经名字数据面映射树持久化回读：抽查 0/100/200/299 */
            const int probes[] = { 0, 100, 200, 299 };
            for (int k = 0; k < 4; k++) {
                int i = probes[k];
                if (i >= long_n) continue;
                char nm[RAVENFS_MAX_NAME_LEN + 1];
                dir_make_long_name(nm, i);
                uint64_t inum = 0;
                result_code_t lr = fs2.root_dir_lookup(nm, &inum);
                CHECK(lr == RC_OK, "重挂载长名 lookup #%d rc=%s", i, rc_name(lr));
            }
        }
    }
    ::close(mnt.dev.fd);

    printf("\n════════════════════════════════════════════════\n"
           " SUMMARY: %d passed, %d failed\n"
           "════════════════════════════════════════════════\n", g_pass, g_fail);
    printf("注：insert 已分配真实 inode（条目 inode_ptr.addr = 新 inode 槽）；\n"
           "    树验证统计 invalid_inode_ptr（应恒为 0）供确认。\n");
    return g_fail ? 1 : 0;
}

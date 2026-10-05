/* ═══════════════════════════════════════════════════════════════════
 * test_root_dir_delete.cpp — 目录条目删除接口专项测试（v1.3）
 *
 * 覆盖（root_dir_rm_dentry / dir_rm_dentry；insert 仅造数据）：
 *   1. 边界/异常：删不存在、553B 超限、40/41B 合法不存在、空名、
 *      常规删后 lookup 消失、二次删。
 *   2. 长名 phase：140 条（>126 → 叶分裂）插入 + lookup；全删（合并 +
 *      根收缩）；再插入 10 条（名字数据面 fr best-fit 复用）后删除。
 *   3. 批量删除：正向删（左→右，看右邻居）→ 反向删（右→左，看左邻居）
 *      → 删空收缩回根叶子；树完整性 DFS；audit_dfs_root 全净（offload）。
 *   4. 持久化：析构 flush → 重挂载 → 树内容不变 + 收缩为根叶。
 *
 * 后端矩阵：offload（默认，mmap）/ nocache（直写+双 no_cache）/
 *           selfreliat（直写+双 self_reliat）。审计仅在 offload 下执行
 *           （audit_dfs_root 前提 is_device_direct=false）。
 * 用法：ravenfs_juv_delete <device> [batch_count=1000] [del_count=0]
 *                              [mode=offload|nocache|selfreliat]
 *   del_count=0 → 全删；>0 → 只删前 del_count 条（二分复播）。
 *   batch_count 默认 1000 适配 64MiB 镜像（≈2048 inode）。
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "dir_tree_check.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>

using Clock = std::chrono::steady_clock;

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device> [batch_count=1000] [del_count=0] [mode=offload|nocache|selfreliat]\n", argv[0]); return 2; }
    int batch_n = 1000;
    if (argc >= 3) batch_n = atoi(argv[2]);
    if (batch_n < 1) batch_n = 1;
    int del_count = 0;   /* 0 = 删全部；>0 = 复播二分，只删前 del_count 条 */
    if (argc >= 4) del_count = atoi(argv[3]);
    if (del_count < 0) del_count = 0;
    /* 后端模式：offload（非直写 → mmap 全 offload）/ nocache（直写 → 双 no_cache）/
     * selfreliat（直写 → 双 self_reliat 用户态缓存） */
    bool mode_direct = false, mode_use_icache = false, mode_use_bcache = false;
    if (argc >= 5 && strcmp(argv[4], "nocache") == 0) {
        mode_direct = true;
    } else if (argc >= 5 && strcmp(argv[4], "selfreliat") == 0) {
        mode_direct = true; mode_use_icache = true; mode_use_bcache = true;
    }
    const bool audit_ok = !mode_direct;   /* 审计前提：mmap 后端 */

    printf("════════════════════════════════════════════════\n"
           " RavenFS v1.3 dir_rm_dentry 专项测试\n"
           " device: %s   batch: %d   del: %s   mode: %s\n"
           "════════════════════════════════════════════════\n",
           argv[1], batch_n,
           del_count > 0 ? "bisect（只删前若干条）" : "全部删除",
           mode_direct ? (mode_use_icache ? "selfreliat（直写+双self_reliat）"
                                          : "nocache（直写+双no_cache）")
                       : "offload（mmap后端）");

    uint64_t rc = 0;
    uint64_t inserted = 0;   /* 树内短名条目数（长名 phase 自带记账） */

    linux_userspace_mount_t mnt = make_mount(argv[1], mode_direct, mode_use_icache, mode_use_bcache);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }

    {
    RavenFS_juvenile fs(mnt, &rc);
    CHECK(fs.is_mounted(), "mount rc=%s", rc_name(rc));
    if (!fs.is_mounted()) { printf("挂载失败，测试中止（先确认已 mkfs）\n"); return 1; }
    fs.dump_info();

    /* ══ Phase 1：边界 / 异常输入 ══ */
    printf("\n--- Phase 1: 边界与异常 ---\n");
    char long39[40], long40[41], long41[42];
    memset(long39, 'x', 39); long39[39] = 0;
    memset(long40, 'y', 40); long40[40] = 0;
    memset(long41, 'z', 41); long41[41] = 0;

    {
        result_code_t r = fs.root_dir_rm_dentry("ghost", false);
        CHECK(r == RC_ERR_NOT_FOUND, "rm(\"ghost\") 空目录 → %s (expect NOT_FOUND)", rc_name(r));
    }
    {
        result_code_t ri = fs.root_dir_insert_dentry("alpha", INODE_TYPE_REGULAR);
        CHECK(ri == RC_OK, "insert(\"alpha\") → %s", rc_name(ri));
        if (ri == RC_OK) inserted++;
        result_code_t r = fs.root_dir_rm_dentry("alpha", false);
        CHECK(r == RC_OK, "rm(\"alpha\") → %s (expect RC_OK)", rc_name(r));
        if (r == RC_OK) inserted--;
        uint64_t inum = 0;
        result_code_t lr = fs.root_dir_lookup("alpha", &inum);
        CHECK(lr == RC_ERR_NOT_FOUND, "lookup(\"alpha\") → %s (expect NOT_FOUND)", rc_name(lr));
        result_code_t r2 = fs.root_dir_rm_dentry("alpha", false);
        CHECK(r2 == RC_ERR_NOT_FOUND, "rm(\"alpha\") 二次 → %s (expect NOT_FOUND)", rc_name(r2));
    }
    {
        char long553[554];
        memset(long553, 'v', 553); long553[553] = 0;
        result_code_t r = fs.root_dir_rm_dentry(long553, false);
        CHECK(r == RC_ERR_NAME_LEN, "rm(553B) → %s (expect NAME_LEN)", rc_name(r));
        result_code_t r2 = fs.root_dir_rm_dentry(long40, false);
        CHECK(r2 == RC_ERR_NOT_FOUND, "rm(40B) → %s (expect NOT_FOUND，合法长度不存在)", rc_name(r2));
        result_code_t r3 = fs.root_dir_rm_dentry(long41, false);
        CHECK(r3 == RC_ERR_NOT_FOUND, "rm(41B) → %s (expect NOT_FOUND)", rc_name(r3));
    }
    {
        result_code_t ri = fs.root_dir_insert_dentry(long39, INODE_TYPE_REGULAR);
        CHECK(ri == RC_OK, "insert(39B) → %s", rc_name(ri));
        if (ri == RC_OK) inserted++;
        result_code_t r = fs.root_dir_rm_dentry(long39, false);
        CHECK(r == RC_OK, "rm(39B) → %s (expect RC_OK)", rc_name(r));
        if (r == RC_OK) inserted--;
    }
    {
        result_code_t r = fs.root_dir_rm_dentry("", false);
        CHECK(r == RC_ERR_NOT_FOUND, "rm(\"\") 未插空名 → %s (expect NOT_FOUND)", rc_name(r));
    }
    dir_verify_tree(fs, inserted, "Phase 1 后");

    /* ══ Phase 1L：长名 split / merge / fr 复用 ══ */
    printf("\n--- Phase 1L: 长名 140 条（split）→ 全删（merge+收缩）→ 复用 ---\n");
    {
        const int long_n = 140;   /* >126 → 叶分裂 */
        char lname[RAVENFS_MAX_NAME_LEN + 1];
        uint64_t len_live = 0;
        bool lok = true;
        for (int i = 0; i < long_n; i++) {
            dir_make_long_name(lname, i);
            result_code_t r = fs.root_dir_insert_dentry(lname, INODE_TYPE_REGULAR);
            if (r != RC_OK) { lok = false; printf("  长名 insert #%d 失败: %s\n", i, rc_name(r)); break; }
            len_live++;
        }
        CHECK(lok, "长名插入 %d 全成功", long_n);
        for (int i = 0; i < long_n; i += 35) {
            char nm[RAVENFS_MAX_NAME_LEN + 1];
            dir_make_long_name(nm, i);
            uint64_t inum = 0;
            result_code_t lr = fs.root_dir_lookup(nm, &inum);
            CHECK(lr == RC_OK, "长名 lookup #%d rc=%s", i, rc_name(lr));
        }
        dir_verify_tree(fs, inserted + len_live, "长名插入后");

        /* 全删：触发叶合并 + 根收缩；长名外置区间回 fr */
        bool dok = true;
        for (int i = 0; i < long_n; i++) {
            dir_make_long_name(lname, i);
            result_code_t r = fs.root_dir_rm_dentry(lname, false);
            if (r != RC_OK) { dok = false; printf("  长名 rm #%d 失败: %s\n", i, rc_name(r)); break; }
            len_live--;
        }
        CHECK(dok && len_live == 0, "长名全删（剩余 %llu）", (unsigned long long)len_live);
        dir_verify_tree(fs, inserted, "长名删空后");

        /* 再插 10 条（fr best-fit 复用刚释放的字节区间）→ lookup → 再删 */
        uint64_t reuse_live = 0;
        for (int i = 0; i < 10; i++) {
            dir_make_long_name(lname, i);
            result_code_t r = fs.root_dir_insert_dentry(lname, INODE_TYPE_REGULAR);
            CHECK(r == RC_OK, "复用 insert #%d rc=%s", i, rc_name(r));
            if (r != RC_OK) break;
            reuse_live++;
            uint64_t inum = 0;
            result_code_t lr = fs.root_dir_lookup(lname, &inum);
            CHECK(lr == RC_OK, "复用 lookup #%d rc=%s", i, rc_name(lr));
        }
        for (int i = 0; i < 10; i++) {
            dir_make_long_name(lname, i);
            result_code_t r = fs.root_dir_rm_dentry(lname, false);
            CHECK(r == RC_OK, "复用 rm #%d rc=%s", i, rc_name(r));
            if (r == RC_OK) reuse_live--;
        }
        CHECK(reuse_live == 0, "复用后回收干净（剩余 %llu）", (unsigned long long)reuse_live);
        dir_verify_tree(fs, inserted, "长名复用后");
        if (audit_ok) CHECK(audit_clean(fs, "longname-phase") == 0, "audit_dfs_root 全净 (长名 phase)");
    }

    /* ══ Phase 2：批量插入 → 批量删除 ══ */
    printf("\n--- Phase 2: 插入 %d 条造数据 ---\n", batch_n);
    char nb[64];
    bool batch_ok = true;
    const auto t_insert0 = Clock::now();
    int progress = batch_n >= 1000 ? batch_n / 8 : 1;
    for (int i = 0; i < batch_n; i++) {
        snprintf(nb, sizeof(nb), "f%05d", i);
        result_code_t r = fs.root_dir_insert_dentry(nb, INODE_TYPE_REGULAR);
        if (r != RC_OK) { batch_ok = false; printf("  insert f%05d 失败: %s\n", i, rc_name(r)); break; }
        inserted++;
        if (progress >= 1 && (i + 1) % progress == 0)
            printf("  ... %d/%d 已插入（累计 %llu）\n", i + 1, batch_n, (unsigned long long)inserted);
    }
    const auto t_insert1 = Clock::now();
    const double insert_sec = std::chrono::duration<double>(t_insert1 - t_insert0).count();
    CHECK(batch_ok, "批量插入 %d 全成功", batch_n);
    printf("  ⏱ 插入阶段: %.3f s（%.0f ops/s）\n", insert_sec, insert_sec > 0 ? batch_n / insert_sec : 0);
    fs.dump_info();
    dir_verify_tree(fs, inserted, "批量插入后");

    int del_target = (del_count > 0) ? del_count : batch_n;
    if (del_target > batch_n) del_target = batch_n;
    /* 默认：正向删前半、反向删后半，左右邻居合并/抹匀都覆盖；单方向删光则
     * 树直接收缩回根叶子（收缩路径同样被验证）。 */
    int fwd_target = (del_count > 0) ? del_target : batch_n / 2;
    if (fwd_target < 0) fwd_target = 0;

    printf("\n--- Phase 2a: 正向删除 %d 条（左→右） ---\n", fwd_target);
    const auto t_fwd0 = Clock::now();
    progress = fwd_target >= 1000 ? fwd_target / 8 : 1;
    bool del_ok = true;
    int del_done = 0;
    for (int i = 0; i < fwd_target; i++) {
        snprintf(nb, sizeof(nb), "f%05d", i);
        result_code_t r = fs.root_dir_rm_dentry(nb, false);
        if (r != RC_OK) { del_ok = false; printf("  rm f%05d 失败: %s\n", i, rc_name(r)); break; }
        inserted--;
        del_done++;
        if (progress >= 1 && (del_done) % progress == 0)
            printf("  ... %d/%d 已删（剩余 %llu）\n", del_done, fwd_target, (unsigned long long)inserted);
    }
    const auto t_fwd1 = Clock::now();
    const double fwd_sec = std::chrono::duration<double>(t_fwd1 - t_fwd0).count();
    CHECK(del_ok, "正向删除 %d 全成功", del_done);
    printf("  ⏱ 正向删: %.3f s（%.0f ops/s）\n", fwd_sec, fwd_sec > 0 ? del_done / fwd_sec : 0);
    dir_verify_tree(fs, inserted, "正向删除后");

    if (del_count > 0) {
        printf("\n════════════════════════════════════════════════\n"
               " SUMMARY(bisect del=%d): %d passed, %d failed\n"
               "════════════════════════════════════════════════\n",
               del_done, g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    printf("\n--- Phase 2b: 反向删除 %d 条（右→左） ---\n", batch_n - del_done);
    int rev_target = batch_n - del_done;
    const auto t_rev0 = Clock::now();
    progress = rev_target >= 1000 ? rev_target / 8 : 1;
    bool rev_ok = true;
    int rev_done = 0;
    for (int i = batch_n - 1; i >= del_done; i--) {
        snprintf(nb, sizeof(nb), "f%05d", i);
        result_code_t r = fs.root_dir_rm_dentry(nb, false);
        if (r != RC_OK) { rev_ok = false; printf("  rm f%05d 失败: %s\n", i, rc_name(r)); break; }
        inserted--;
        rev_done++;
        if (progress >= 1 && rev_done % progress == 0)
            printf("  ... %d/%d 已删（剩余 %llu）\n", rev_done, rev_target, (unsigned long long)inserted);
    }
    const auto t_rev1 = Clock::now();
    const double rev_sec = std::chrono::duration<double>(t_rev1 - t_rev0).count();
    CHECK(rev_ok, "反向删除 %d 全成功", rev_done);
    printf("  ⏱ 反向删: %.3f s（%.0f ops/s）\n", rev_sec, rev_sec > 0 ? rev_done / rev_sec : 0);
    dir_verify_tree(fs, inserted, "反向删除后");
    printf("  删空后剩余条目: %llu\n", (unsigned long long)inserted);
    CHECK(inserted == 0, "全部删空（剩余 0）");
    fs.dump_info();
    if (audit_ok) CHECK(audit_clean(fs, "delete-empty") == 0, "audit_dfs_root 全净 (delete 删空)");

    {
        result_code_t r = fs.root_dir_rm_dentry("f00000", false);
        CHECK(r == RC_ERR_NOT_FOUND, "rm(\"f00000\") 删空后 → %s (expect NOT_FOUND)", rc_name(r));
    }
    }   /* ← fs 析构：位图全量写回 */

    /* ══ Phase 3：持久化（析构 flush → 重挂载） ══ */
    printf("\n--- Phase 3: 持久化验证（重挂载） ---\n");
    {
        RavenFS_juvenile fs2(mnt, &rc);
        CHECK(fs2.is_mounted(), "重挂载 rc=%s", rc_name(rc));
        if (fs2.is_mounted()) {
            fs2.dump_info();
            DirTreeStat st = dir_verify_tree(fs2, inserted, "重挂载后");
            /* 删空后树应收缩回根叶子：leaf=1 inner=0 */
            if (st.ok) {
                DirTreeStat st2;
                dir_verify_node(fs2, fs2.root_dentry_blk, PTR_INVALID, st2);
                if (st2.ok) {
                    CHECK(st2.leaf_count == 1 && st2.inner_count == 0,
                          "删空收缩: leaf=%llu inner=%llu (expect 1/0)",
                          (unsigned long long)st2.leaf_count,
                          (unsigned long long)st2.inner_count);
                }
            }
        }
    }
    ::close(mnt.dev.fd);

    printf("\n════════════════════════════════════════════════\n"
           " SUMMARY: %d passed, %d failed\n"
           "════════════════════════════════════════════════\n", g_pass, g_fail);
    printf("注：insert 已分配真实 inode，rm 时 inode_free 释放 inode 槽；\n"
           "    删除路径同时验证 inode/名字数据面随插删增减。\n");
    return g_fail ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * test_audit_dfs.cpp — 可达性审计（新模型）专项测试
 *
 * 覆盖（对齐 audit_dfs_root）：
 *   1. 干净盘（mkfs 后）：total_bad == 0，且至少扫到根叶节点
 *   2. 建内容（REG + 长名走名字堆 + 子目录）：再审全净
 *   3. 人工污染（翻 root 叶首条目 hash）→ btree_bad 命中；还原后归零
 *   4. XOR 对账：手工把 root 叶某块位图熄灭 → unprotected_blk 命中（没有被保护）
 *
 * 用法：ravenfs_juv_audit_dfs <device>    （已 mkfs 的 ravenfs v1.2 盘/镜像）
 * ═══════════════════════════════════════════════════════════════════ */
/* 重量级头（<chrono> 经 tsc.h）必须在 `#define private public` 之外 include。 */
#include "../include/ravenfs/ravenfs_audit.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "inode_compat.hpp"   /* v2.0 inode 读写兼容壳 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

static linux_userspace_mount_t make_mount(const char *path) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_UNKNOWN;
    mnt.dev.is_device_direct = false;   /* mmap 后端（offload 缓存）——审计前提 */
    mnt.param = {};
    mnt.param.if_will_use_inode_cache = true;
    mnt.param.if_will_use_mdata_piece_cache = true;
    mnt.logical_processor_count = 1;
    return mnt;
}

static void dump_rep(const char *tag, const audit_walk_report_t &r) {
    printf("  [audit:%s] inodes=%llu nodes=%llu inner=%llu leaves=%llu recs=%llu | "
           "btree=%llu inode=%llu heap=%llu xattr=%llu multi_ref=%llu multi_claim=%llu "
           "link=%llu dotdot=%llu | unprot[b=%llu i=%llu] leak[b=%llu i=%llu] bad=%llu\n",
           tag,
           (unsigned long long)r.inodes_walked, (unsigned long long)r.nodes,
           (unsigned long long)r.inner, (unsigned long long)r.leaves,
           (unsigned long long)r.records,
           (unsigned long long)r.btree_bad, (unsigned long long)r.inode_bad,
           (unsigned long long)r.heap_bad, (unsigned long long)r.xattr_bad,
           (unsigned long long)r.multi_ref_ino, (unsigned long long)r.multi_claim_blk,
           (unsigned long long)r.link_mismatch, (unsigned long long)r.dotdot_mismatch,
           (unsigned long long)r.unprotected_blk, (unsigned long long)r.unprotected_ino,
           (unsigned long long)r.leaked_blk, (unsigned long long)r.leaked_ino,
           (unsigned long long)r.total_bad());
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device>\n", argv[0]); return 2; }
    printf("════════════════════════════════════════════════\n"
           " RavenFS 可达性审计（audit_dfs_root）专项测试\n"
           " device: %s\n"
           "════════════════════════════════════════════════\n", argv[1]);

    uint64_t rc = 0;
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }

    {
        RavenFS_juvenile fs(mnt, &rc);
        if (!fs.is_mounted()) { printf("mount failed rc=%llu\n", (unsigned long long)rc); return 1; }

        printf("\n--- Phase 1: 干净盘审计 ---\n");
        audit_walk_report_t r1;
        result_code_t a1 = fs.audit_dfs_root(&r1, stdout);
        dump_rep("fresh", r1);
        CHECK(a1 == RC_OK, "audit_dfs_root rc=OK");
        CHECK(r1.total_bad() == 0, "干净盘 total_bad==0 (bad=%llu)", (unsigned long long)r1.total_bad());
        CHECK(r1.nodes >= 1 && r1.leaves >= 1, "至少扫到根叶节点");

        printf("\n--- Phase 2: 建内容后再审 ---\n");
        char lname[600];
        int ins_fail = 0, ins_ok = 0;
        uint64_t first_fail_rc = 0;
        for (int i = 0; i < 300; i++) {
            snprintf(lname, sizeof(lname), "f%04d_regular", i);
            result_code_t r = fs.root_dir_insert_dentry(lname, INODE_TYPE_REGULAR);
            if (r == RC_OK) ins_ok++;
            else { if (!ins_fail) { first_fail_rc = r; printf("  [insert-fail] name='%s\" rc=%llu\n", lname, (unsigned long long)r); } ins_fail++; }
        }
        for (int i = 0; i < 40; i++) {           /* 长名 → 名字堆 */
            int len = 41 + (i * 13) % 500;
            for (int j = 0; j < len; j++) lname[j] = 'a' + (char)((i + j) % 26);
            lname[len] = 0;
            result_code_t r = fs.root_dir_insert_dentry(lname, INODE_TYPE_REGULAR);
            if (r == RC_OK) ins_ok++;
            else { if (!ins_fail) { first_fail_rc = r; printf("  [insert-fail] len=%d rc=%llu\n", len, (unsigned long long)r); } ins_fail++; }
        }
        if (fs.root_dir_insert_dentry("subdir_a", INODE_TYPE_DIR) == RC_OK) ins_ok++; else ins_fail++;
        if (fs.root_dir_insert_dentry("subdir_b", INODE_TYPE_DIR) == RC_OK) ins_ok++; else ins_fail++;
        printf("  [insert] ok=%d fail=%d first_fail_rc=%llu\n", ins_ok, ins_fail,
               (unsigned long long)first_fail_rc);

        audit_walk_report_t r2;
        fs.audit_dfs_root(&r2, stdout);
        dump_rep("content", r2);
        CHECK(r2.btree_bad == 0 && r2.inode_bad == 0 && r2.heap_bad == 0 &&
              r2.multi_ref_ino == 0 && r2.multi_claim_blk == 0 &&
              r2.unprotected_blk == 0 && r2.unprotected_ino == 0 &&
              r2.leaked_blk == 0 && r2.leaked_ino == 0,
              "建内容后结构/XOR 全净 (btree=%llu heap=%llu unprot=%llu leak=%llu)",
              (unsigned long long)r2.btree_bad, (unsigned long long)r2.heap_bad,
              (unsigned long long)r2.unprotected_blk, (unsigned long long)r2.leaked_blk);
        CHECK(r2.records == (uint64_t)ins_ok, "审计条目数 == 成功插入数 (%llu)",
              (unsigned long long)r2.records);
        /* '..' 对账：根目录分裂后新根须携带 '..'（2026-09-30 修：
         * dentry_new_root_inner 未继承旧根 ptrs[1]，已修）。 */
        CHECK(r2.dotdot_mismatch == 0, "dir '..' 对账全净 (dotdot=%llu)",
              (unsigned long long)r2.dotdot_mismatch);

        printf("\n--- Phase 3: 人工污染 → 命中；还原 → 归零 ---\n");
        {
            mdata_piece_session s(fs.mdata_piece_mgr, fs.root_dentry_blk);
            auto *leaf = s ? (Bptree_ddentry_leaf_node_t *)s.data : nullptr;
            uint16_t c = leaf ? (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr) : 0;
            CHECK(leaf && !s.index_error() && c > 0, "打开根叶且非空");
            if (leaf && c > 0) {
                leaf->entries[0].hash ^= 0x1ULL;
                s.dirty();
            }
        }
        audit_walk_report_t r3;
        fs.audit_dfs_root(&r3, stdout);
        dump_rep("corrupt", r3);
        CHECK(r3.btree_bad >= 1, "污染后 btree_bad>=1 (bad=%llu)", (unsigned long long)r3.btree_bad);
        {
            mdata_piece_session s2(fs.mdata_piece_mgr, fs.root_dentry_blk);
            auto *l2 = s2 ? (Bptree_ddentry_leaf_node_t *)s2.data : nullptr;
            if (l2 && !s2.index_error()) { l2->entries[0].hash ^= 0x1ULL; s2.dirty(); }
        }
        audit_walk_report_t r4;
        fs.audit_dfs_root(&r4, stdout);
        CHECK(r4.btree_bad == 0, "还原后 btree_bad 归零 (btree=%llu)", (unsigned long long)r4.btree_bad);

        printf("\n--- Phase 4: XOR 没有被保护（灭 root 叶块位图）---\n");
        {
            const uint64_t blk = fs.root_dentry_blk;
            fs.block_free(blk, 1);   /* 故意把可达块在位图上标空 → 应报 unprotected_blk */
        }
        audit_walk_report_t r5;
        fs.audit_dfs_root(&r5, stdout);
        dump_rep("unprot", r5);
        CHECK(r5.unprotected_blk >= 1, "protected 命中 unprotected_blk>=1 (%llu)",
              (unsigned long long)r5.unprotected_blk);

        printf("\n--- Phase 5: XOR 疑似泄露（占空块不引用）---\n");
        {
            const uint64_t b = fs.block_alloc_hard(2, 1);   /* 2 块无人引用 → 位图亮 ∧ 不可达 */
            printf("  [alloc] leaked-candidate base=%llu\n", (unsigned long long)b);
        }
        audit_walk_report_t r6;
        fs.audit_dfs_root(&r6, stdout);
        dump_rep("leak", r6);
        CHECK(r6.leaked_blk >= 1, "泄露命中 leaked_blk>=1 (%llu)",
              (unsigned long long)r6.leaked_blk);

        printf("\n--- Phase 6: 子目录收缩后 '..' 仍指父 ---\n");
        {
            uint64_t sub = 0;
            result_code_t lr = fs.root_dir_lookup("subdir_a", &sub);
            CHECK(lr == RC_OK && sub != 0, "lookup subdir_a (=%llu)", (unsigned long long)sub);
            char nm[64];
            int ins = 0;
            for (int i = 0; i < 250; i++) {
                snprintf(nm, sizeof(nm), "s%04d", i);
                if (fs.dir_insert_dentry(sub, nm, INODE_TYPE_REGULAR) == RC_OK) ins++;
            }
            printf("  [subdir] inserted %d entries into subdir inum=%llu\n", ins, (unsigned long long)sub);
            for (int i = 0; i < 240; i++) {   /* 删到触发叶合并 + 根收缩 */
                snprintf(nm, sizeof(nm), "s%04d", i);
                fs.dir_rm_dentry(sub, nm, false);
            }
            raven_Inode_t sino; juv_inode_read(fs, sub, &sino);
            const uint64_t root_blk = sino.data_root;
            uint64_t dd = 0;
            {
                mdata_piece_session s(fs.mdata_piece_mgr, root_blk);
                if (s && !s.index_error())
                    dd = RFS_PTR_ADDR(((Bptree_meta_t *)s.data)->ptrs[1]);
            }
            printf("  [subdir] data_root=%llu  '..'=%llu  (期望父=%llu)\n",
                   (unsigned long long)root_blk, (unsigned long long)dd,
                   (unsigned long long)fs.root_inum);
            CHECK(dd == fs.root_inum, "子目录收缩后 '..' == 父 (dd=%llu)",
                  (unsigned long long)dd);
            audit_walk_report_t r7;
            fs.audit_dfs_root(&r7, stdout);
            CHECK(r7.dotdot_mismatch == 0 && r7.btree_bad == 0,
                  "收缩后 dotdot/btree 全净 (dotdot=%llu btree=%llu)",
                  (unsigned long long)r7.dotdot_mismatch, (unsigned long long)r7.btree_bad);
        }
    }
    ::close(mnt.dev.fd);

    printf("\n════════════════════════════════════════════════\n"
           " SUMMARY: %d passed, %d failed\n"
           "════════════════════════════════════════════════\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * test_audit_inode.cpp — audit_inode 静态审计专项测试
 *
 * 覆盖：
 *   1. 干净盘（mkfs 后）：audit_filesystem 全计数为 0
 *   2. 建 REG/长名条目 + dir 子目录：再审全 0（含 hash 重算 / 名字堆归属 / '..' 对账）
 *   3. 单 inode 入口 audit_inode(root) 不崩、账本可复算
 *   4. 人工污染（翻 root 叶首条目 hash）→ btree_bad 命中；还原后归零
 *
 * 用法：ravenfs_juv_audit <device>       （已 mkfs 的 ravenfs v1.1 盘/镜像）
 * ═══════════════════════════════════════════════════════════════════ */
/* 重量级头（<chrono> 经 tsc.h）必须在 `#define private public` 之外 include，
 * 否则 gcc16 下 sstream 可访问性检查直接报错（见 ravenfs_v1.1.hpp 头注释）。 */
#include "../include/ravenfs/ravenfs_audit.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private

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
    mnt.dev.is_device_direct = false;   /* mmap 后端（offload 缓存） */
    mnt.param = {};
    mnt.logical_processor_count = 1;
    return mnt;
}

static uint64_t total_bad(const audit_report_t &r) {
    return r.btree_bad + r.inode_bad + r.double_claim + r.leaked_blocks + r.orphan_blocks +
           r.dangling_refs + r.orphan_inodes + r.link_mismatch + r.dotdot_mismatch + r.heap_bad;
}

static void dump_rep(const char *tag, const audit_report_t &r) {
    printf("  [audit:%s] inodes=%llu flagged=%llu | nodes=%llu inner=%llu leaves=%llu recs=%llu\n"
           "              btree_bad=%llu inode_bad=%llu double=%llu leak_blk=%llu orphan_blk=%llu\n"
           "              dangling=%llu orphan_ino=%llu link=%llu dotdot=%llu heap=%llu\n",
           tag,
           (unsigned long long)r.inodes_scanned, (unsigned long long)r.inodes_flagged,
           (unsigned long long)r.nodes, (unsigned long long)r.inner,
           (unsigned long long)r.leaves, (unsigned long long)r.records,
           (unsigned long long)r.btree_bad, (unsigned long long)r.inode_bad,
           (unsigned long long)r.double_claim, (unsigned long long)r.leaked_blocks,
           (unsigned long long)r.orphan_blocks, (unsigned long long)r.dangling_refs,
           (unsigned long long)r.orphan_inodes, (unsigned long long)r.link_mismatch,
           (unsigned long long)r.dotdot_mismatch, (unsigned long long)r.heap_bad);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device>\n", argv[0]); return 2; }
    printf("════════════════════════════════════════════════\n"
           " RavenFS v1.1 audit_inode 静态审计专项测试\n"
           " device: %s\n"
           "════════════════════════════════════════════════\n", argv[1]);

    uint64_t rc = 0;
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }

    audit_opts_t opts;   /* 默认：详细日志到 stderr */

    {
        RavenFS_juvenile fs(mnt, &rc);
        if (!fs.is_mounted()) { printf("mount failed rc=%llu\n", (unsigned long long)rc); return 1; }
        fs.dump_info();

        printf("\n--- Phase 1: 干净盘审计 ---\n");
        audit_report_t r1;
        fs.audit_filesystem(opts, r1);
        dump_rep("fresh", r1);
        CHECK(total_bad(r1) == 0, "干净盘全计数为 0 (bad=%llu)", (unsigned long long)total_bad(r1));
        CHECK(r1.nodes >= 1 && r1.leaves >= 1, "至少扫到根叶节点");

        printf("\n--- Phase 2: 建内容后再审 ---\n");
        char lname[600];
        for (int i = 0; i < 300; i++) {
            snprintf(lname, sizeof(lname), "f%04d_regular", i);
            fs.root_dir_insert_dentry(lname, INODE_TYPE_REGULAR);
        }
        /* 长名（触发名字堆 8B/64B/128B/... 档） */
        for (int i = 0; i < 40; i++) {
            int len = 41 + (i * 13) % 500;
            for (int j = 0; j < len; j++) lname[j] = 'a' + (char)((i + j) % 26);
            lname[len] = 0;
            fs.root_dir_insert_dentry(lname, INODE_TYPE_REGULAR);
        }
        /* 子目录（覆盖 dir 树 root/.. 对账） */
        fs.root_dir_insert_dentry("subdir_a", INODE_TYPE_DIR);
        fs.root_dir_insert_dentry("subdir_b", INODE_TYPE_DIR);

        audit_report_t r2;
        fs.audit_filesystem(opts, r2);
        dump_rep("content", r2);
        CHECK(total_bad(r2) == 0, "建内容后全计数为 0 (bad=%llu)", (unsigned long long)total_bad(r2));
        CHECK(r2.records >= 342, "记录了 >=342 条目 (recs=%llu)", (unsigned long long)r2.records);

        printf("\n--- Phase 3: 单 inode 入口 ---\n");
        audit_ctx_t ctx;
        fs.audit_begin(ctx, opts);
        uint64_t b0 = ctx.btree_bad;
        result_code_t ar = fs.audit_inode(fs.root_inum, ctx);
        CHECK(ar == RC_OK, "audit_inode(root) rc=OK");
        CHECK(ctx.btree_bad == b0, "单 inode 审计 root 无 btree_bad");
        CHECK(!ctx.dotdot_of.empty() && ctx.dotdot_of[fs.root_inum] == fs.root_inum,
              "root '..' == root_inum");

        printf("\n--- Phase 4: 人工污染 → 命中 ---\n");
        {
            mdata_piece_session s(fs.mdata_piece_mgr, fs.root_dentry_blk);
            if (!s || s.index_error()) {
                CHECK(false, "打开根叶失败");
            } else {
                auto *leaf = (Bptree_dentry_leaf_node_t *)s.data;
                uint16_t c = (uint16_t)leaf->meta.parent_ptr.entry_count;
                if (c == 0) {
                    CHECK(false, "根叶为空，无法污染");
                } else {
                    uint64_t saved = leaf->entries[0].hash;
                    leaf->entries[0].hash ^= 0x1ULL;   /* 翻 bit → hash 重算不符 */
                    s.dirty();
                }
                audit_report_t r3;
                fs.audit_filesystem(opts, r3);
                dump_rep("corrupt", r3);
                CHECK(r3.btree_bad >= 1, "污染后 btree_bad>=1 (bad=%llu)", (unsigned long long)r3.btree_bad);

                /* 还原 */
                mdata_piece_session s2(fs.mdata_piece_mgr, fs.root_dentry_blk);
                if (s2 && !s2.index_error()) {
                    auto *l2 = (Bptree_dentry_leaf_node_t *)s2.data;
                    l2->entries[0].hash ^= 0x1ULL;
                    s2.dirty();
                }
                audit_report_t r4;
                fs.audit_filesystem(opts, r4);
                CHECK(total_bad(r4) == 0, "还原后归零 (bad=%llu)", (unsigned long long)total_bad(r4));
            }
        }
    }
    ::close(mnt.dev.fd);

    printf("\n════════════════════════════════════════════════\n"
           " SUMMARY: %d passed, %d failed\n"
           "════════════════════════════════════════════════\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

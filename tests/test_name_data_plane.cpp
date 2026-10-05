/* ═══════════════════════════════════════════════════════════════════
 * test_name_data_plane.cpp — 目录名字数据面（v1.3）专项测试
 *
 * 覆盖（字节粒度 + fr 树）：
 *   1. 字节打包：连续 alloc 100B → 偏移 0/100/200…，片数仍 1
 *   2. 空闲复用：free 一段 → 再 alloc 命中该段（best-fit）
 *   3. 用尽增长：占死 direct 段尾 → 填满首片 → grow 走 hard_alloc+映射树注入
 *      （direct→tree 升级）；tree 态映射解析读写回环
 *   4. release 全量回收（数据片 + 映射树 + fr 树）→ 控制字复位 + audit 全净
 *
 * 用法：ravenfs_juv_name_data <device>   （已 mkfs --v1.3 的镜像）
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/ravenfs_audit.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private

#include <cstdio>
#include <cstring>
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
    mnt.dev.is_device_direct = false;
    mnt.param = {};
    mnt.param.if_will_use_inode_cache = true;
    mnt.param.if_will_use_mdata_piece_cache = true;
    mnt.logical_processor_count = 1;
    return mnt;
}

static void read_ctl(RavenFS_juvenile &fs, uint64_t inum, uint64_t *ctl, uint64_t *pieces,
                     uint64_t *fr_count) {
    raven_dir_Inode_t d;
    fs.inode_read(inum, (raven_Inode_t *)&d);
    *ctl = d.name_data_ctl;
    *pieces = RFS_ND_PIECE_COUNT(d.name_data_piece_count);
    *fr_count = RFS_ND_FR_COUNT(d.name_data_free_ranges_count);
}
static void wl(RavenFS_juvenile &fs, uint64_t inum, uint64_t off, const char *s) {
    uint16_t n = (uint16_t)strlen(s);
    uint64_t rc = fs.name_data_write(inum, off, n, s);
    CHECK(rc == RC_OK, "write off=%llu len=%u", (unsigned long long)off, n);
}
static void rl(RavenFS_juvenile &fs, uint64_t inum, uint64_t off, const char *expect) {
    char buf[512]; memset(buf, 0, sizeof buf);
    uint16_t n = (uint16_t)strlen(expect);
    uint64_t rc = fs.name_data_read(inum, off, n, buf);
    CHECK(rc == RC_OK && memcmp(buf, expect, n) == 0,
          "read off=%llu rc=%llu match=%d", (unsigned long long)off, (unsigned long long)rc,
          buf[0] == expect[0]);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device>\n", argv[0]); return 2; }
    printf("══════════ 名字数据面专题  device=%s ══════════\n", argv[1]);

    uint64_t rc = 0;
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }

    {
        RavenFS_juvenile fs(mnt, &rc);
        if (!fs.is_mounted()) { printf("mount failed rc=%llu\n", (unsigned long long)rc); return 1; }
        const uint64_t root = fs.sb.root_inode;

        uint64_t ctl = 0, pieces = 0, frc = 0;
        read_ctl(fs, root, &ctl, &pieces, &frc);
        CHECK(RFS_ND_CTL_PTR(ctl) == RFS_ND_CTL_PTR_INVALID && pieces == 0, "初始空态");

        printf("\n--- Phase 1: 字节打包（100B × 5 同片） ---\n");
        uint64_t offs[5];
        for (int i = 0; i < 5; i++) {
            uint64_t r = fs.name_data_alloc(root, 100, &offs[i]);
            CHECK(r == RC_OK && offs[i] == (uint64_t)i * 100,
                  "alloc#%d rc=%llu off=%llu", i, (unsigned long long)r, (unsigned long long)offs[i]);
        }
        read_ctl(fs, root, &ctl, &pieces, &frc);
        CHECK(pieces == 1 && RFS_ND_CTL_IS_DIRECT(ctl) == 1, "打包后仍单片 direct (pieces=%llu)",
              (unsigned long long)pieces);
        char msg[128];
        for (int i = 0; i < 5; i++) { snprintf(msg, sizeof msg, "ND-pack-%d", i); wl(fs, root, offs[i], msg); }
        for (int i = 0; i < 5; i++) { snprintf(msg, sizeof msg, "ND-pack-%d", i); rl(fs, root, offs[i], msg); }

        printf("\n--- Phase 2: 空闲复用（free 中间段 → best-fit 命中） ---\n");
        fs.name_data_free(root, offs[1], 100);   /* 释放 offs[1]=100 的 100B */
        uint64_t reused = 0;
        uint64_t r2 = fs.name_data_alloc(root, 100, &reused);
        CHECK(r2 == RC_OK && reused == offs[1], "复用命中 off=%llu (期望 %llu)",
              (unsigned long long)reused, (unsigned long long)offs[1]);
        wl(fs, root, reused, "ND-reuse");
        rl(fs, root, reused, "ND-reuse");

        printf("\n--- Phase 3: 用尽 → 占死段尾 → direct→tree 升级 ---\n");
        read_ctl(fs, root, &ctl, &pieces, &frc);
        const uint64_t base = RFS_ND_CTL_PTR(ctl);
        const uint64_t tail_blk = base + pieces * 2;   /* 紧贴段尾的下一个 8K */
        uint64_t rest = 0;
        const uint64_t occ = fs.block_occupy_soft(tail_blk, 2, &rest);
        /* 段尾块可能已被别处占用（如 dentry 根叶就在块 4）——两种情形都保证
         * name_data 的 direct 尾延展失败，从而走 hard_alloc+映射树注入。 */
        printf("  [info] 段尾块=%llu occupy=%llu rest=%llu（占死或本已占用 → 升级 tree）\n",
               (unsigned long long)tail_blk, (unsigned long long)occ, (unsigned long long)rest);

        uint64_t last_off = 0;
        for (int i = 0; i < 20; i++) {
            uint64_t o = 0;
            uint64_t r = fs.name_data_alloc(root, 512, &o);
            if (r != RC_OK) { CHECK(false, "fill-alloc#%d rc=%llu", i, (unsigned long long)r); break; }
            last_off = o;
        }
        read_ctl(fs, root, &ctl, &pieces, &frc);
        CHECK(pieces >= 2, "已增长多片 pieces=%llu", (unsigned long long)pieces);
        CHECK(RFS_ND_CTL_IS_DIRECT(ctl) == 0, "已升级 tree 态 (direct=%u)",
              (unsigned)RFS_ND_CTL_IS_DIRECT(ctl));
        wl(fs, root, last_off, "ND-tree-last");
        rl(fs, root, last_off, "ND-tree-last");

        printf("\n--- Phase 4: tree 态全量回环 ---\n");
        for (int i = 0; i < 5; i++) {
            if (i == 1) continue;   /* offs[1] Phase2 已 free 并复用为 ND-reuse */
            snprintf(msg, sizeof msg, "ND-pack-%d", i); rl(fs, root, offs[i], msg);
        }
        rl(fs, root, reused, "ND-reuse");
        rl(fs, root, last_off, "ND-tree-last");
        printf("  [info] 终态 direct=%u pieces=%llu fr_count=%llu\n",
               (unsigned)RFS_ND_CTL_IS_DIRECT(ctl), (unsigned long long)pieces,
               (unsigned long long)frc);

        printf("\n--- Phase 5: release 全量回收 + audit ---\n");
        uint64_t r3 = fs.name_data_release(root);
        CHECK(r3 == RC_OK, "release rc=%llu", (unsigned long long)r3);
        read_ctl(fs, root, &ctl, &pieces, &frc);
        CHECK(RFS_ND_CTL_PTR(ctl) == RFS_ND_CTL_PTR_INVALID && pieces == 0 && frc == 0,
              "回收后复位空态");
        fs.block_free(occ, 2);   /* 归还测试占死的碎裂块 */

        audit_walk_report_t rep;
        uint64_t a = fs.audit_dfs_root(&rep, stdout);
        CHECK(a == RC_OK && rep.total_bad() == 0, "release 后 audit 全净 (bad=%llu leak_b=%llu)",
              (unsigned long long)rep.total_bad(), (unsigned long long)rep.leaked_blk);
    }

    printf("\n══════════ SUMMARY: %d passed, %d failed ══════════\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

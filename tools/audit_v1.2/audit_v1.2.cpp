/* ═══════════════════════════════════════════════════════════════════════════
 * audit_v1.2 — RavenFS v1.2 递归只读校验员（**全面使用上帝类**）
 *
 * 2026-09-30 重写：由「自带独立解析的 v1.2 审计器（~1230 行）」改为
 * **上帝类薄壳**——挂载 RavenFS_juvenile（readonly + mmap 后端）→ 调
 * `audit_dfs_root`。检查逻辑 = 可达性审计（root 先序 DFS + 两张影子图 +
 * 收口 XOR）；类型/几何/SB 四连验由上帝类 ctor 承担。
 *
 * 旧独立实现见 git 历史（本文件 2026-09-30 之前版本）。
 *
 * 纪律：
 *   · 只读（O_RDONLY + god readonly mmap 后端；god dtor 不 flush）
 *   · 日志只走 stdout（stderr 仅致命错误）
 *
 * 编译： make audit_v1.2        （链接 libravenfs_juvenile.a + cpu_id_stub + xxhash）
 * 运行： ./audit_v1.2 [-v] <image-or-device>
 *   -v / --verbose  逐 inode 打 WALK（大盛盘日志巨大，默认关）
 * 退出码： 0 = 无 BAD；1 = 有 BAD；2 = 用法 / 挂载 / IO 错误
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "ravenfs/ravenfs_v1.1.hpp"
#include "ravenfs/ravenfs_audit.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#ifndef BLKSSZGET
#  define BLKSSZGET _IO(0x12, 104)
#endif

int main(int argc, char **argv) {
    bool verbose = false;
    const char *path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) verbose = true;
        else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) verbose = false;
        else if (argv[i][0] == '-') { fprintf(stderr, "error: unknown option %s\n", argv[i]); return 2; }
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: %s [-v] <image-or-device>\n", argv[0]);
        return 2;
    }

    int fd = ::open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "error: open %s (O_RDONLY) failed\n", path); return 2; }

    linux_userspace_mount_t mnt{};
    mnt.dev.fd = fd;
    mnt.dev.type = DEV_UNKNOWN;
    mnt.dev.is_device_direct = false;        /* mmap 后端（审计前提） */
    int lba = 4096;
    if (::ioctl(fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;   /* 块设备探测，文件回退 4K */
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.param = {};
    mnt.param.is_readonly = true;            /* 只读挂载（rfs-audit 静态 mmap 审核） */
    mnt.param.if_will_use_inode_cache = true;
    mnt.param.if_will_use_mdata_piece_cache = true;
    mnt.logical_processor_count = 1;

    printf("=== RavenFS v1.2 audit (god class, readonly) : %s ===\n", path);

    uint64_t bad = 0;
    {
        uint64_t rc = 0;
        RavenFS_juvenile fs(mnt, &rc);
        if (!fs.is_mounted()) {
            fprintf(stderr, "error: mount failed rc=%llu\n", (unsigned long long)rc);
            ::close(fd);
            return 2;
        }

        audit_walk_report_t rep;
        result_code_t arc = fs.audit_dfs_root(&rep, stdout, verbose);
        if (arc != RC_OK) {
            fprintf(stderr, "error: audit rc=%llu\n", (unsigned long long)arc);
            ::close(fd);
            return 2;
        }
        bad = rep.total_bad();

        printf("bad_total=%llu (btree=%llu inode=%llu heap=%llu xattr=%llu "
               "multi_ref=%llu multi_claim=%llu link=%llu dotdot=%llu | "
               "unprot b/i=%llu/%llu leak b/i=%llu/%llu)\n",
               (unsigned long long)bad,
               (unsigned long long)rep.btree_bad, (unsigned long long)rep.inode_bad,
               (unsigned long long)rep.heap_bad, (unsigned long long)rep.xattr_bad,
               (unsigned long long)rep.multi_ref_ino, (unsigned long long)rep.multi_claim_blk,
               (unsigned long long)rep.link_mismatch, (unsigned long long)rep.dotdot_mismatch,
               (unsigned long long)rep.unprotected_blk, (unsigned long long)rep.unprotected_ino,
               (unsigned long long)rep.leaked_blk, (unsigned long long)rep.leaked_ino);
        printf("heap: active=%llu blocks=%llu (bmp=%llu data=%llu = %.1f MiB) "
               "used=%llu/%llu (%.4f%%) zero_used=%llu\n",
               (unsigned long long)rep.heap_active,
               (unsigned long long)(rep.heap_active_bitmap_blk + rep.heap_active_data_blk),
               (unsigned long long)rep.heap_active_bitmap_blk,
               (unsigned long long)rep.heap_active_data_blk,
               (double)(rep.heap_active_bitmap_blk + rep.heap_active_data_blk) * 4096.0 / 1048576.0,
               (unsigned long long)rep.heap_used_bits,
               (unsigned long long)rep.heap_total_bits,
               rep.heap_total_bits ? 100.0 * (double)rep.heap_used_bits / (double)rep.heap_total_bits : 0.0,
               (unsigned long long)rep.heap_zero_used);
        for (int s = 0; s < 10; s++) {
            if (rep.heap_slot_active[s] == 0) continue;
            printf("  heap_slot[%d] active=%llu data_blk=%llu used=%llu/%llu\n",
                   s, (unsigned long long)rep.heap_slot_active[s],
                   (unsigned long long)rep.heap_slot_data_blk[s],
                   (unsigned long long)rep.heap_slot_used_bits[s],
                   (unsigned long long)rep.heap_slot_total_bits[s]);
        }
    }
    ::close(fd);
    return bad == 0 ? 0 : 1;
}

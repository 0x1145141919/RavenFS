/* ═══════════════════════════════════════════════════════════════════
 * stress_name_data_fr.cpp — fr 空闲区间树百万级压测（开缓存）
 *
 * 目标：把 fr 树推到百万级记录（多层内节点），在**缓存开启**下压测
 *       插入 / best-fit 取用 / 全量审计 / 回收。
 *
 * 缓存模式（argv mode=）：
 *   offload   （默认）is_device_direct=false → 整盘 mmap + offload 缓存
 *             （inode/mdata 走内核页缓存直映）；可在原地直接 audit。
 *   selfreliat is_device_direct=true + 双 self_reliat 用户态缓存（O_DIRECT）；
 *             audit 需 mmap 后端，故压完卸载后**重挂 offload** 审计落盘树。
 *
 * 规模：N 条空闲区间（默认 1,000,000）；len_i = 1 + (i%8)（1..8 交替），base 递增，
 *       max 偏移 ≈ N×4.5B → 数据面约 N×4.5/8K 片（百万级 ~5MB 逻辑面）。
 *       建议镜像 ≥ 256MiB（fr 记录 16B/条 → 叶约 N/337 个，节点 8K/个）。
 *
 * 用法：ravenfs_juv_stress_name_data_fr <device> [N=1000000] [mode=offload|selfreliat]
 * ═══════════════════════════════════════════════════════════════════ */
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
#include <time.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

static double now_s() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}
static linux_userspace_mount_t make_mount(const char *path, bool selfreliat) {
    linux_userspace_mount_t mnt{};
    mnt.dev.fd = ::open(path, selfreliat ? O_RDWR | O_DIRECT : O_RDWR);
    int lba = 4096;
    if (::ioctl(mnt.dev.fd, BLKSSZGET, &lba) != 0 || lba <= 0) lba = 4096;
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_UNKNOWN;
    mnt.dev.is_device_direct = selfreliat;
    mnt.param = {};
    if (selfreliat) {
        mnt.param.if_will_use_inode_cache = true;
        mnt.param.if_will_use_mdata_piece_cache = true;   /* self_reliat 双档 */
    }
    mnt.logical_processor_count = 1;
    return mnt;
}
static uint64_t rd_fr_count(RavenFS_juvenile &fs, uint64_t inum) {
    raven_dir_Inode_t d; fs.inode_read(inum, (raven_Inode_t *)&d);
    return RFS_ND_FR_COUNT(d.name_data_free_ranges_count);
}

/* 期望：len_i = 1+(i%8)，base 递增 → 首个 len==L 的条目 base */
static uint64_t first_base_of_len(uint64_t L) {
    uint64_t base = 0;
    for (uint64_t i = 0; i + 1 < L; i++) base += 1 + (i % 8);   /* i0 = L-1 */
    return base;
}
static uint64_t total_bytes(uint64_t N) {
    uint64_t s = 0;
    for (uint64_t i = 0; i < N; i++) s += 1 + (i % 8);
    return s;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device> [N] [mode=offload|selfreliat]\n", argv[0]); return 2; }
    uint64_t N = (argc >= 3) ? strtoull(argv[2], nullptr, 10) : 1000000ULL;
    bool SELF = false;
    for (int i = 2; i < argc; i++) if (strstr(argv[i], "selfreliat")) SELF = true;
    printf("══════════ fr 百万级压测  device=%s  N=%llu  mode=%s ══════════\n",
           argv[1], (unsigned long long)N, SELF ? "selfreliat" : "offload");

    const uint64_t total = total_bytes(N);
    const uint64_t piece_need = total / 8192 + 4;

    /* ── Phase A：建面 + 灌 N 条 + best-fit 抽检（在所选缓存模式）── */
    {
        uint64_t rc = 0;
        linux_userspace_mount_t mnt = make_mount(argv[1], SELF);
        if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }
        {
            RavenFS_juvenile fs(mnt, &rc);
            if (!fs.is_mounted()) { printf("mount failed rc=%llu (mode=%s)\n",
                (unsigned long long)rc, SELF ? "selfreliat" : "offload"); return 1; }
            const uint64_t root = fs.sb.root_inode;

            double t0 = now_s();
            uint64_t got_pieces = 0;
            /* 建够片：alloc 512B× 直至 piece 数达标。读 ctl 片数。 */
            while (true) {
                raven_dir_Inode_t d; fs.inode_read(root, (raven_Inode_t *)&d);
                got_pieces = RFS_ND_PIECE_COUNT(d.name_data_piece_count);
                if (got_pieces >= piece_need) break;
                uint64_t o = 0;
                if (fs.name_data_alloc(root, 512, &o) != RC_OK) { CHECK(false, "建面 alloc 失败"); break; }
            }
            printf("  [info] 建面 %llu 片 (需 %llu)，耗时 %.1fs\n",
                   (unsigned long long)got_pieces, (unsigned long long)piece_need, now_s() - t0);

            uint64_t fr_before = rd_fr_count(fs, root);
            t0 = now_s();
            uint64_t cursor = 0;
            int ins_fail = 0;
            for (uint64_t i = 0; i < N; i++) {
                uint16_t len = (uint16_t)(1 + (i % 8));
                result_code_t r = fs.name_data_free(root, cursor, len);
                if (r != RC_OK) { if (!ins_fail) printf("  [warn] free #%llu rc=%llu\n",
                                      (unsigned long long)i, (unsigned long long)r); ins_fail++; }
                cursor += len;
            }
            double dt = now_s() - t0;
            uint64_t fr_after = rd_fr_count(fs, root);
            CHECK(ins_fail == 0 && fr_after == fr_before + N,
                  "灌入 %llu 条 (fr %llu→%llu, fail=%d) 耗时 %.1fs (%.2f M ops/s)",
                  (unsigned long long)N, (unsigned long long)fr_before,
                  (unsigned long long)fr_after, ins_fail, dt, N / dt / 1e6);

            /* best-fit 抽检：need=L 应命中 len=L、base=first_base_of_len(L) */
            raven_dir_Inode_t d; fs.inode_read(root, (raven_Inode_t *)&d);
            const uint64_t fr_root = RFS_ND_FR_CTL_PTR(d.name_data_free_ranges_ctl);
            bool spot_ok = (fr_root != RFS_ND_FR_CTL_PTR_INVALID);
            for (uint64_t L = 1; L <= 8 && spot_ok; L++) {
                uint64_t base = 0, len = 0;
                if (fs.name_data_fr_find_fit(fr_root, L, &base, &len) != RC_OK ||
                    len != L || base != first_base_of_len(L)) spot_ok = false;
            }
            CHECK(spot_ok, "best-fit 抽检 (need 1..8 → len==need, base 正确)");

            /* 取用若干（best-fit 摘除 + 余段插回） */
            t0 = now_s();
            int take_fail = 0;
            for (int k = 0; k < 10000; k++) {
                uint64_t o = 0;
                if (fs.name_data_alloc(root, (uint16_t)(1 + (k % 8)), &o) != RC_OK) take_fail++;
            }
            CHECK(take_fail == 0, "取用 10000 次 best-fit 无失败 耗时 %.1fs", now_s() - t0);

            /* offload 模式可在原地 audit（selfreliat 需重挂 offload） */
            if (!SELF) {
                audit_walk_report_t rep;
                uint64_t a = fs.audit_dfs_root(&rep, stdout);
                CHECK(a == RC_OK && rep.total_bad() == 0,
                      "百万级 fr 树 audit 全净 (bad=%llu heap=%llu leak=%llu mc=%llu)",
                      (unsigned long long)rep.total_bad(), (unsigned long long)rep.heap_bad,
                      (unsigned long long)rep.leaked_blk, (unsigned long long)rep.multi_claim_blk);
                uint64_t r3 = fs.name_data_release(root);
                audit_walk_report_t rep2;
                uint64_t a2 = fs.audit_dfs_root(&rep2, stdout);
                CHECK(r3 == RC_OK && a2 == RC_OK && rep2.total_bad() == 0,
                      "release 后 audit 全净 (bad=%llu)", (unsigned long long)rep2.total_bad());
            }
        }   /* fs 析构：selfreliat 脏页写回落盘 */
        if (SELF) printf("  [info] selfreliat 卸载（脏页回写）完成\n");
    }

    /* ── Phase B（selfreliat）：重挂 offload（mmap）审计落盘树 ── */
    if (SELF) {
        uint64_t rc = 0;
        linux_userspace_mount_t mnt = make_mount(argv[1], false);
        if (mnt.dev.fd < 0) { printf("remount open failed\n"); return 2; }
        {
            RavenFS_juvenile fs(mnt, &rc);
            CHECK(fs.is_mounted(), "重挂 offload rc=%llu", (unsigned long long)rc);
            if (fs.is_mounted()) {
                const uint64_t root = fs.sb.root_inode;
                uint64_t fr = rd_fr_count(fs, root);
                printf("  [info] 重挂后 fr_count=%llu（落盘树）\n", (unsigned long long)fr);
                audit_walk_report_t rep;
                uint64_t a = fs.audit_dfs_root(&rep, stdout);
                CHECK(a == RC_OK && rep.total_bad() == 0,
                      "selfreliat 落盘百万级 fr 树 audit 全净 (bad=%llu heap=%llu leak=%llu mc=%llu)",
                      (unsigned long long)rep.total_bad(), (unsigned long long)rep.heap_bad,
                      (unsigned long long)rep.leaked_blk, (unsigned long long)rep.multi_claim_blk);
                uint64_t r3 = fs.name_data_release(root);
                audit_walk_report_t rep2;
                uint64_t a2 = fs.audit_dfs_root(&rep2, stdout);
                CHECK(r3 == RC_OK && a2 == RC_OK && rep2.total_bad() == 0,
                      "release 后 audit 全净 (bad=%llu)", (unsigned long long)rep2.total_bad());
            }
        }
    }

    printf("\n══════════ SUMMARY: %d passed, %d failed ══════════\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

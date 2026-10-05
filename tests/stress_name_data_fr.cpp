/* ═══════════════════════════════════════════════════════════════════
 * stress_name_data_fr.cpp — fr 空闲区间树压力测试（随机灌入 + 乱序用尽 + 定长返还，开缓存）
 *
 * 覆盖（对齐"返还-分配"真实路径，非确定性模式）：
 *   Phase A 随机灌树：生成 N 条**随机 len**（1..512）、互不重叠的逻辑区间，
 *            **随机顺序**逐条 name_data_free 灌入 fr（压分裂/路由）。
 *   Phase B 乱序用尽：以**随机 need** 反复 best-fit 取用（take_fit），直到 fr
 *            无任何区间可满足（用尽）；校验 find_fit 与 take_fit 一致、
 *            取用区间两两不重叠、fr 清零。
 *   Phase C 定长乱序返还+分配：分配 K 条定长区间 → 随机挑子集 S、**乱序** free →
 *            再分配 |S| 次，校验返回恰为 S（且 best-fit = base 升序）。
 *   末尾：audit（映射树 + fr 树结构）全净；release 全净。
 *
 * 缓存模式（argv mode=）：
 *   offload   （默认）is_device_direct=false → mmap + offload 缓存，原地 audit。
 *   selfreliat is_device_direct=true + 双 self_reliat 用户态缓存（O_DIRECT）；
 *             audit 需 mmap，故卸载后**重挂 offload** 审计落盘树。
 *
 * 用法：ravenfs_juv_stress_name_data_fr <device> [N=1000000] [mode=offload|selfreliat]
 * 规模提示：随机 len 均值 ~256B ⇒ N 条约占 N×256B 逻辑面（1e6 ≈ 256MiB）；
 *           建议镜像 ≥ 1GiB。种子固定打印，可复现。
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/ravenfs_audit.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"   /* InodeSession（2026-10-05 显式化，原经 mdata 传递） */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "inode_compat.hpp"   /* v2.0 inode 读写兼容壳 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <numeric>
#include <random>
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
        mnt.param.if_will_use_mdata_piece_cache = true;
    }
    mnt.logical_processor_count = 1;
    return mnt;
}
static uint64_t rd_fr_count(RavenFS_juvenile &fs, uint64_t inum) {
    raven_dir_Inode_t d; juv_inode_read(fs, inum, (raven_Inode_t *)&d);
    return RFS_ND_FR_COUNT(d.name_data_free_ranges_count);
}

struct Iv { uint64_t base; uint16_t len; };

/* 三阶段主流程（cache 模式无关）；返回时 fr 已用尽、Phase C 已复原一批定长区间 */
static void run_phases(RavenFS_juvenile &fs, uint64_t N, uint64_t seed) {
    const uint64_t root = RFS_ROOT_INODE_IDX;
    std::mt19937_64 rng(seed);

    /* ── Phase A：随机 len + 随机插入序灌树 ── */
    printf("  [info] Phase A 随机灌树 N=%llu seed=%#llx\n",
           (unsigned long long)N, (unsigned long long)seed);
    std::vector<Iv> ivs; ivs.reserve((size_t)N);
    uint64_t cursor = 0;
    for (uint64_t i = 0; i < N; i++) {
        uint16_t len = (uint16_t)(1 + (rng() % 512));
        ivs.push_back({cursor, len});
        cursor += len;
    }
    const uint64_t total = cursor;
    const uint64_t piece_need = total / 8192 + 8;
    {
        double t0 = now_s(); uint64_t got = 0; bool ok = true;
        while (true) {
            raven_dir_Inode_t d; juv_inode_read(fs, root, (raven_Inode_t *)&d);
            got = RFS_ND_PIECE_COUNT(d.name_data_piece_count);
            if (got >= piece_need) break;
            uint64_t o = 0;
            if (fs.name_data_alloc(root, 512, &o) != RC_OK) { ok = false; break; }
        }
        CHECK(ok, "建面 %llu 片 (需 %llu, 逻辑 %llu MiB) %.1fs",
              (unsigned long long)got, (unsigned long long)piece_need,
              (unsigned long long)(total >> 20), now_s() - t0);
    }
    uint64_t fr_before = rd_fr_count(fs, root);
    std::vector<uint64_t> order((size_t)N);
    for (uint64_t i = 0; i < N; i++) order[(size_t)i] = i;
    std::shuffle(order.begin(), order.end(), rng);   /* 随机插入序 */
    double t0 = now_s();
    int ins_fail = 0;
    for (uint64_t k = 0; k < N; k++) {
        const Iv &v = ivs[(size_t)order[(size_t)k]];
        if (fs.name_data_free(root, v.base, v.len) != RC_OK) {
            if (!ins_fail) printf("  [warn] free #%llu 失败\n", (unsigned long long)k);
            ins_fail++;
        }
    }
    double dt = now_s() - t0;
    uint64_t fr_after = rd_fr_count(fs, root);
    CHECK(ins_fail == 0 && fr_after == fr_before + N,
          "随机灌入 %llu 条 (fr %llu→%llu) %.1fs (%.2f M ops/s)",
          (unsigned long long)N, (unsigned long long)fr_before,
          (unsigned long long)fr_after, dt, N / dt / 1e6);

    /* ── Phase B：随机 need 乱序 best-fit 取用，直至用尽 ── */
    printf("  [info] Phase B 乱序用尽\n");
    {
        InodeSession sess(fs.inode_io, root);
        if (!sess) { CHECK(false, "Phase B InodeSession 打开失败"); return; }
        raven_dir_Inode_t *dino = (raven_dir_Inode_t *)sess.inode;
        uint64_t frroot = RFS_ND_FR_CTL_PTR(dino->name_data_free_ranges_ctl);
        std::vector<Iv> taken; taken.reserve((size_t)N);
        bool take_ok = true, drained = false;
        int mismatch = 0;
        double tb = now_s();
        uint64_t guard = 8ULL * N + 1024;
        while (taken.size() + 1 < guard) {
            if (frroot == RFS_ND_FR_CTL_PTR_INVALID) { drained = true; break; }
            uint64_t need = 1 + (rng() % 512);
            uint64_t b = 0, l = 0;
            result_code_t rf = fs.name_data_fr_find_fit(frroot, need, &b, &l);
            if (rf == RC_ERR_NOT_FOUND) {           /* 该 need 无解：降为 need=1 榨干 */
                need = 1;
                rf = fs.name_data_fr_find_fit(frroot, 1, &b, &l);
                if (rf == RC_ERR_NOT_FOUND) { drained = true; break; }
            }
            if (rf != RC_OK) { take_ok = false; break; }
            uint64_t got = 0;
            result_code_t rt = fs.name_data_fr_take_fit(root, dino, need, &got);
            if (rt == RC_ERR_NOT_FOUND) { drained = true; break; }
            if (rt != RC_OK) { take_ok = false; break; }
            if (got != b) { mismatch++; if (mismatch <= 3)
                printf("  [warn] take base=%llu != find_fit base=%llu\n",
                       (unsigned long long)got, (unsigned long long)b); }
            taken.push_back({b, (uint16_t)need});
            frroot = RFS_ND_FR_CTL_PTR(dino->name_data_free_ranges_ctl);
        }
        uint64_t leftover = RFS_ND_FR_COUNT(dino->name_data_free_ranges_count);
        sess.dirty();
        double dtb = now_s() - tb;
        std::sort(taken.begin(), taken.end(),
                  [](const Iv &a, const Iv &b) { return a.base < b.base; });
        int overlap = 0;
        for (size_t i = 1; i < taken.size(); i++)
            if (taken[i-1].base + taken[i-1].len > taken[i].base) overlap++;
        CHECK(take_ok && drained && mismatch == 0 && overlap == 0 && leftover == 0,
              "乱序取用用尽: taken=%zu mismatch=%d overlap=%d leftover_fr=%llu %.1fs",
              taken.size(), mismatch, overlap, (unsigned long long)leftover, dtb);
    }

    /* ── Phase C：定长乱序返还 + 再分配 ── */
    printf("  [info] Phase C 定长乱序返还+分配\n");
    {
        CHECK(fs.name_data_release(root) == RC_OK, "Phase C 前置 release");
        const int K = 2000; const uint16_t FL = 64;
        std::vector<uint64_t> offs((size_t)K);
        bool ac_ok = true;
        for (int i = 0; i < K; i++) {
            if (fs.name_data_alloc(root, FL, &offs[(size_t)i]) != RC_OK) { ac_ok = false; break; }
        }
        CHECK(ac_ok, "分配 K=%d 个定长(%uB)区间", K, (unsigned)FL);

        std::vector<int> idx((size_t)K);
        std::iota(idx.begin(), idx.end(), 0);
        std::shuffle(idx.begin(), idx.end(), rng);
        const int S = K / 2;
        std::vector<uint64_t> freed;
        int fr_bad = 0;
        for (int t = 0; t < S; t++) {
            int i = idx[(size_t)t];
            if (fs.name_data_free(root, offs[(size_t)i], FL) != RC_OK) fr_bad++;
            freed.push_back(offs[(size_t)i]);
        }
        CHECK(fr_bad == 0, "乱序 free |S|=%d 个 (bad=%d)", S, fr_bad);

        std::sort(freed.begin(), freed.end());   /* 等 len：best-fit = base 升序 */
        int reuse_bad = 0;
        for (int t = 0; t < S; t++) {
            uint64_t o = 0;
            if (fs.name_data_alloc(root, FL, &o) != RC_OK || o != freed[(size_t)t]) {
                if (reuse_bad < 3) printf("  [warn] reuse t=%d got=%llu expect=%llu\n",
                                          t, (unsigned long long)o,
                                          (unsigned long long)freed[(size_t)t]);
                reuse_bad++;
            }
        }
        CHECK(reuse_bad == 0, "定长返还→分配：返回恰为 S 且 base 升序 (bad=%d, |S|=%d)",
              reuse_bad, S);
        /* 再 free 剩余 live（offs 中未在 S 的），保持盘面整洁 */
        for (int i = 0; i < K; i++) {
            bool inS = false;
            for (int t = 0; t < S; t++) if (idx[(size_t)t] == i) { inS = true; break; }
            if (!inS) fs.name_data_free(root, offs[(size_t)i], FL);
        }
    }
}

static bool audit_clean(RavenFS_juvenile &fs, const char *tag) {
    audit_walk_report_t rep;
    uint64_t a = fs.audit_dfs_root(&rep, stdout);
    bool ok = (a == RC_OK && rep.total_bad() == 0);
    printf("  [%s] audit bad=%llu heap=%llu leak=%llu mc=%llu\n", tag,
           (unsigned long long)rep.total_bad(), (unsigned long long)rep.heap_bad,
           (unsigned long long)rep.leaked_blk, (unsigned long long)rep.multi_claim_blk);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device> [N] [mode=offload|selfreliat]\n", argv[0]); return 2; }
    uint64_t N = (argc >= 3) ? strtoull(argv[2], nullptr, 10) : 1000000ULL;
    bool SELF = false;
    for (int i = 2; i < argc; i++) if (strstr(argv[i], "selfreliat")) SELF = true;
    const uint64_t seed = 0x9E3779B97F4A7C15ULL;
    printf("══════════ fr 随机+乱序压测  device=%s N=%llu mode=%s seed=%#llx ══════════\n",
           argv[1], (unsigned long long)N, SELF ? "selfreliat" : "offload",
           (unsigned long long)seed);

    {
        uint64_t rc = 0;
        linux_userspace_mount_t mnt = make_mount(argv[1], SELF);
        if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }
        {
            RavenFS_juvenile fs(mnt, &rc);
            if (!fs.is_mounted()) { printf("mount failed rc=%llu (mode=%s)\n",
                (unsigned long long)rc, SELF ? "selfreliat" : "offload"); return 1; }
            run_phases(fs, N, seed);
            if (!SELF) {
                CHECK(audit_clean(fs, "audit"), "offload 原地 audit 全净");
                CHECK(fs.name_data_release(RFS_ROOT_INODE_IDX) == RC_OK, "release");
                CHECK(audit_clean(fs, "after-release"), "release 后 audit 全净");
            }
        }   /* fs 析构：selfreliat 脏页回写 */
        if (SELF) printf("  [info] selfreliat 卸载（脏页回写）完成\n");
    }

    if (SELF) {
        uint64_t rc = 0;
        linux_userspace_mount_t mnt = make_mount(argv[1], false);   /* 重挂 offload 审计 */
        if (mnt.dev.fd < 0) { printf("remount open failed\n"); return 2; }
        {
            RavenFS_juvenile fs(mnt, &rc);
            CHECK(fs.is_mounted(), "重挂 offload rc=%llu", (unsigned long long)rc);
            if (fs.is_mounted()) {
                CHECK(audit_clean(fs, "persisted"), "selfreliat 落盘 audit 全净");
                CHECK(fs.name_data_release(RFS_ROOT_INODE_IDX) == RC_OK, "release");
                CHECK(audit_clean(fs, "after-release"), "release 后 audit 全净");
            }
        }
    }

    printf("\n══════════ SUMMARY: %d passed, %d failed ══════════\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

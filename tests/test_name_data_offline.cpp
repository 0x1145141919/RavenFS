/* ═══════════════════════════════════════════════════════════════════
 * test_name_data_offline.cpp — 名字数据面离线规整（下行迁移）专项（v1.3）
 *
 * 覆盖（供离线工具使用的两个函数）：
 *   A. name_data_rebuild：tree 碎片映射 → 单连续 direct 段
 *      · 触发 direct→tree（占死物理尾再增长）→ 写已知数据 → rebuild
 *      · 断言 direct=1 / pieces 不变 / 数据逻辑偏移回读一致
 *      · 幂等（之后再调 no-op）/ audit 全净（旧树+旧片零泄漏）
 *   B. name_data_fr_compact：fr dump + 全量相邻合并
 *      · 相邻 20 段 → 合并为 1，fr 21→2 → **降级 inline**
 *      · 分离 10 对 → 合并为 10，fr 21→11 → 保持 tree
 *      · best-fit 语义不变 / audit 全净
 *
 * 用法：ravenfs_juv_name_data_offline <device>   （已 mkfs --v1.3 镜像）
 * ═══════════════════════════════════════════════════════════════════ */
#include "../include/ravenfs/ravenfs_audit.hpp"
#include "../include/ravenfs/mdata_piece_caches.hpp"
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include "inode_compat.hpp"   /* v2.0 inode 读写兼容壳 */

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

static void read_dir_ctl(RavenFS_juvenile &fs, uint64_t inum, uint64_t *nd_ctl,
                         uint64_t *pieces, uint64_t *fr_ctl, uint64_t *fr_cnt) {
    raven_dir_Inode_t d;
    juv_inode_read(fs, inum, (raven_Inode_t *)&d);
    if (nd_ctl)  *nd_ctl  = d.name_data_ctl;
    if (pieces)  *pieces  = RFS_ND_PIECE_COUNT(d.name_data_piece_count);
    if (fr_ctl)  *fr_ctl  = d.name_data_free_ranges_ctl;
    if (fr_cnt)  *fr_cnt  = RFS_ND_FR_COUNT(d.name_data_free_ranges_count);
}

static uint64_t audit_clean(RavenFS_juvenile &fs, const char *tag) {
    audit_walk_report_t rep;
    uint64_t a = fs.audit_dfs_root(&rep, stdout);
    uint64_t bad = (a == RC_OK) ? rep.total_bad() : (uint64_t)-1;
    printf("  [%s] audit(%s) bad=%llu heap=%llu leak=%llu mc=%llu\n",
           bad == 0 ? "PASS" : "FAIL", tag, (unsigned long long)bad,
           (unsigned long long)rep.heap_bad, (unsigned long long)rep.leaked_blk,
           (unsigned long long)rep.multi_claim_blk);
    return bad;
}

/* 触发 name_data 映射 direct→tree：占死首片物理尾（2 块）后再增长，
 * soft 尾延展失败 → hard_alloc + 映射树注入。返回占用块（调用方归还）。 */
static uint64_t force_tree_plane(RavenFS_juvenile &fs, uint64_t root, int grow_n) {
    uint64_t o = 0;
    fs.name_data_alloc(root, 256, &o);
    uint64_t ctl = 0, pieces = 0;
    read_dir_ctl(fs, root, &ctl, &pieces, nullptr, nullptr);
    const uint64_t tail_blk = RFS_ND_CTL_PTR(ctl) + pieces * 2;
    uint64_t rest = 0;
    uint64_t occ = fs.block_occupy_soft(tail_blk, 2, &rest);
    for (int i = 0; i < grow_n; i++) {
        uint64_t t = 0;
        fs.name_data_alloc(root, 256, &t);
    }
    return occ;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device>\n", argv[0]); return 2; }
    printf("══════════ 名字数据面离线规整专题  device=%s ══════════\n", argv[1]);

    uint64_t rc = 0;
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }

    {
        RavenFS_juvenile fs(mnt, &rc);
        if (!fs.is_mounted()) { printf("mount failed rc=%llu\n", (unsigned long long)rc); return 1; }
        const uint64_t root = RFS_ROOT_INODE_IDX;

        /* ══ A. name_data_rebuild ══ */
        printf("\n--- A: name_data_rebuild（tree → direct 压实）---\n");
        {
            CHECK(fs.name_data_rebuild(root) == RC_OK, "空态 rebuild = no-op RC_OK");

            uint64_t occ = force_tree_plane(fs, root, 80);
            uint64_t nd_ctl = 0, pieces = 0;
            read_dir_ctl(fs, root, &nd_ctl, &pieces, nullptr, nullptr);
            CHECK(RFS_ND_CTL_IS_DIRECT(nd_ctl) == 0,
                  "已升 tree 态 (direct=%u pieces=%llu)",
                  (unsigned)RFS_ND_CTL_IS_DIRECT(nd_ctl), (unsigned long long)pieces);
            const uint64_t pieces_before = pieces;

            /* 前 5 个 256B 槽（alloc 顺序即逻辑偏移 0/256/512/768/1024）写 pattern */
            const int NS = 5;
            char wbuf[256], rbuf[256];
            int wr_ok = 1;
            for (int i = 0; i < NS; i++) {
                memset(wbuf, 'A' + i, sizeof wbuf);
                if (fs.name_data_write(root, (uint64_t)i * 256, 256, wbuf) != RC_OK) wr_ok = 0;
            }
            CHECK(wr_ok, "rebuild 前写入 5 槽 pattern");

            uint64_t rb = fs.name_data_rebuild(root);
            CHECK(rb == RC_OK, "rebuild rc=%llu", (unsigned long long)rb);
            read_dir_ctl(fs, root, &nd_ctl, &pieces, nullptr, nullptr);
            CHECK(RFS_ND_CTL_IS_DIRECT(nd_ctl) == 1 && pieces == pieces_before,
                  "rebuild 后 direct=1 pieces=%llu（不变）", (unsigned long long)pieces);

            int data_ok = 1;
            for (int i = 0; i < NS; i++) {
                memset(rbuf, 0, sizeof rbuf);
                uint64_t rd = fs.name_data_read(root, (uint64_t)i * 256, 256, rbuf);
                const char expect = (char)('A' + i);
                if (rd != RC_OK || rbuf[0] != expect || rbuf[255] != expect) data_ok = 0;
            }
            CHECK(data_ok, "rebuild 后数据按逻辑偏移回读一致");

            CHECK(fs.name_data_rebuild(root) == RC_OK, "rebuild 幂等（direct 态再调）= RC_OK");
            if (occ != PTR_INVALID && occ != RFS_ND_FR_CTL_PTR_INVALID) fs.block_free(occ, 2);
            CHECK(audit_clean(fs, "rebuild 后") == 0, "rebuild 后 audit 全净");
        }

        /* ══ B. name_data_fr_compact ══ */
        printf("\n--- B: name_data_fr_compact（dump + 相邻合并）---\n");
        fs.name_data_release(root);   /* 复位，重建干净 plane */

        const int N = 200;
        uint64_t slots[200];
        {
            bool ok = true;
            for (int i = 0; i < N; i++)
                if (fs.name_data_alloc(root, 256, &slots[i]) != RC_OK) { ok = false; break; }
            CHECK(ok, "建 200×256B 数据面");
        }

        /* B1: 相邻 20 段（slots[0..19]）→ 合并为 1 → 降级 inline */
        for (int i = 0; i < 20; i++) fs.name_data_free(root, slots[i], 256);
        {
            uint64_t fr_ctl = 0, fr_cnt = 0;
            read_dir_ctl(fs, root, nullptr, nullptr, &fr_ctl, &fr_cnt);
            CHECK(RFS_ND_FR_CTL_IS_INLINE(fr_ctl) == 0 && fr_cnt > 9,
                  "B1 上游 fr 已升 tree（inline=%u count=%llu）",
                  (unsigned)RFS_ND_FR_CTL_IS_INLINE(fr_ctl), (unsigned long long)fr_cnt);

            uint64_t c = fs.name_data_fr_compact(root);
            CHECK(c == RC_OK, "B1 compact rc=%llu", (unsigned long long)c);
            read_dir_ctl(fs, root, nullptr, nullptr, &fr_ctl, &fr_cnt);
            CHECK(RFS_ND_FR_CTL_IS_INLINE(fr_ctl) == 1 && fr_cnt == 2,
                  "B1 合并后 ≤9 → 降级 inline（inline=%u count=%llu，期望 1/2）",
                  (unsigned)RFS_ND_FR_CTL_IS_INLINE(fr_ctl), (unsigned long long)fr_cnt);

            uint64_t got = 0;
            uint64_t a = fs.name_data_alloc(root, 256, &got);
            CHECK(a == RC_OK && got == slots[0],
                  "B1 compact 后 best-fit alloc → off=%llu（期望 %llu）",
                  (unsigned long long)got, (unsigned long long)slots[0]);
        }
        CHECK(audit_clean(fs, "B1 compact 后") == 0, "B1 audit 全净");
        fs.name_data_release(root);

        /* B2: 10 对相邻段 → 合并为 10 → 仍 tree（>9 不降级） */
        {
            bool ok = true;
            for (int i = 0; i < N; i++)
                if (fs.name_data_alloc(root, 256, &slots[i]) != RC_OK) { ok = false; break; }
            CHECK(ok, "B2 重建 200×256B");
            for (int p = 0; p < 10; p++) {          /* (0,1) (4,5) (8,9) … 共 10 对 */
                const int b = p * 4;
                fs.name_data_free(root, slots[b],     256);
                fs.name_data_free(root, slots[b + 1], 256);
            }
            uint64_t fr_ctl = 0, fr_cnt = 0;
            read_dir_ctl(fs, root, nullptr, nullptr, &fr_ctl, &fr_cnt);
            CHECK(fr_cnt > 9, "B2 上游 fr tree（count=%llu）", (unsigned long long)fr_cnt);

            uint64_t c = fs.name_data_fr_compact(root);
            CHECK(c == RC_OK, "B2 compact rc=%llu", (unsigned long long)c);
            read_dir_ctl(fs, root, nullptr, nullptr, &fr_ctl, &fr_cnt);
            CHECK(RFS_ND_FR_CTL_IS_INLINE(fr_ctl) == 0 && fr_cnt == 11,
                  "B2 合并后 >9 → 仍 tree（inline=%u count=%llu，期望 0/11）",
                  (unsigned)RFS_ND_FR_CTL_IS_INLINE(fr_ctl), (unsigned long long)fr_cnt);
            CHECK(audit_clean(fs, "B2 compact 后") == 0, "B2 audit 全净");
        }

        fs.name_data_release(root);
        CHECK(audit_clean(fs, "release 后") == 0, "收尾 release 后 audit 全净");
    }

    printf("\n══════════ SUMMARY: %d passed, %d failed ══════════\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

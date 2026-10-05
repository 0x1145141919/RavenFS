/* ═══════════════════════════════════════════════════════════════════
 * test_name_data_freeranges.cpp — fr 空闲区间树（v1.3）专项测试
 *
 * 目的：把 fr 树的**结构路径**（单叶 337 满、>255 索引、叶分裂、内节点、
 *       复合键 (len,base) 序、best-fit 取用）纳入入库回归——这些路径
 *       此前只在一次性临时程序里验证过（child_idx uint8_t 截断 bug 正需
 *       单叶 >256 条才暴露）。
 *
 * 覆盖：
 *   0. fr inline bootstrap（≤9 条）与第 10 条**隐式升格 tree** 的状态断言
 *   1. 造 337 条 distinct-len 空闲区间（单根叶、count==337）
 *   2. best-fit 命中 index>255 的条目（截断回归）：find_fit(300)→len=300
 *   3. take_fit 精确取用（整取）+ 计数回退
 *   4. 再插 2 条 → 叶分裂 + 内节点（337 階）；audit 全净（fr 树独立校验）
 *   5. release 全量回收 → 复位空态 + audit 全净
 *
 * 用法：ravenfs_juv_name_data_fr <device>   （已 mkfs --v1.3 镜像）
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
static uint64_t rd_fr_count(RavenFS_juvenile &fs, uint64_t inum) {
    raven_dir_Inode_t d;
    juv_inode_read(fs, inum, (raven_Inode_t *)&d);
    return RFS_ND_FR_COUNT(d.name_data_free_ranges_count);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <device>\n", argv[0]); return 2; }
    printf("══════════ fr 空闲区间树专题  device=%s ══════════\n", argv[1]);

    uint64_t rc = 0;
    linux_userspace_mount_t mnt = make_mount(argv[1]);
    if (mnt.dev.fd < 0) { printf("open %s failed\n", argv[1]); return 2; }

    {
        RavenFS_juvenile fs(mnt, &rc);
        if (!fs.is_mounted()) { printf("mount failed rc=%llu\n", (unsigned long long)rc); return 1; }
        const uint64_t root = RFS_ROOT_INODE_IDX;
        const uint64_t SLOT = 512;

        printf("\n--- 造足够多的数据片（供 fr 逻辑空间） ---\n");
        int okall = 1;
        for (int i = 0; i < 360; i++) {
            uint64_t o = 0;
            if (fs.name_data_alloc(root, SLOT, &o) != RC_OK) { okall = 0; break; }
        }
        CHECK(okall, "alloc 360×512B 无失败");

        printf("\n--- Phase 1: 300 条 distinct-len 空闲区间（单根叶，含 >255 索引） ---\n");
        /* 释放 [i*512, i*512+i)：互不重叠（i<512），len = i ∈ [1,300]。
         * 另加 alloc 段尾遗留的一条空闲区间（~4K），合计 301 条 < 337 → 单根叶；
         * 叶内 len=300 的下标为 299 (>255)，正是 child_idx 截断 bug 的暴露点。 */
        uint64_t start_frc = rd_fr_count(fs, root);
        CHECK(start_frc == 1, "建面后 fr 遗留 1 条空闲区间（inline bootstrap）got=%llu",
              (unsigned long long)start_frc);
        for (uint64_t i = 1; i <= 300; i++) {
            uint64_t r = fs.name_data_free(root, i * SLOT, (uint16_t)i);
            if (r != RC_OK) { CHECK(false, "free #%llu rc=%llu", (unsigned long long)i, (unsigned long long)r); break; }
            /* 隐含升级回归：1（遗留）+8 条 = 9 条仍 inline；第 10 条升格 tree。 */
            if (i == 8) {
                raven_dir_Inode_t d; juv_inode_read(fs, root, (raven_Inode_t *)&d);
                CHECK(RFS_ND_FR_CTL_IS_INLINE(d.name_data_free_ranges_ctl) == 1 &&
                      RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 9 &&
                      RFS_ND_FR_CTL_PTR(d.name_data_free_ranges_ctl) != RFS_ND_FR_CTL_PTR_INVALID,
                      "累计 9 条仍 inline（inline=%u count=%llu）",
                      (unsigned)RFS_ND_FR_CTL_IS_INLINE(d.name_data_free_ranges_ctl),
                      (unsigned long long)RFS_ND_FR_COUNT(d.name_data_free_ranges_count));
            }
            if (i == 9) {
                raven_dir_Inode_t d; juv_inode_read(fs, root, (raven_Inode_t *)&d);
                CHECK(RFS_ND_FR_CTL_IS_INLINE(d.name_data_free_ranges_ctl) == 0 &&
                      RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 10 &&
                      RFS_ND_FR_CTL_PTR(d.name_data_free_ranges_ctl) != RFS_ND_FR_CTL_PTR_INVALID,
                      "第 10 条隐式升格 tree（inline=%u count=%llu root=%llu）",
                      (unsigned)RFS_ND_FR_CTL_IS_INLINE(d.name_data_free_ranges_ctl),
                      (unsigned long long)RFS_ND_FR_COUNT(d.name_data_free_ranges_count),
                      (unsigned long long)RFS_ND_FR_CTL_PTR(d.name_data_free_ranges_ctl));
                /* 升格后树立即可用：best-fit 命中 len=9 */
                uint64_t base = 0, len = 0;
                uint64_t rf = fs.name_data_fr_find_fit(
                    RFS_ND_FR_CTL_PTR(d.name_data_free_ranges_ctl), 9, &base, &len);
                CHECK(rf == RC_OK && len == 9 && base == 9 * SLOT,
                      "升格后 find_fit(9) → len=%llu base=%llu", (unsigned long long)len,
                      (unsigned long long)base);
            }
        }
        uint64_t frc = rd_fr_count(fs, root);
        CHECK(frc == 301, "fr_count==301（单根叶未裂；含段尾遗留 1 条）got=%llu", (unsigned long long)frc);

        printf("\n--- Phase 2: best-fit index>255 回归（截断 bug） ---\n");
        {
            uint64_t ctl = 0;
            raven_dir_Inode_t d; juv_inode_read(fs, root, (raven_Inode_t *)&d);
            ctl = d.name_data_free_ranges_ctl;
            uint64_t base = 0, len = 0;
            uint64_t r = fs.name_data_fr_find_fit(RFS_ND_FR_CTL_PTR(ctl), 300, &base, &len);
            CHECK(r == RC_OK && len == 300 && base == 300 * SLOT,
                  "find_fit(300) → len=%llu base=%llu（期望 len=300 base=%llu）",
                  (unsigned long long)len, (unsigned long long)base,
                  (unsigned long long)(300 * SLOT));
        }

        printf("\n--- Phase 3: take_fit 整取（经 name_data_alloc 走 best-fit） ---\n");
        /* name_data_alloc 内部即 take_fit：best-fit 应命中 len=300 的区间并整取 */
        {
            uint64_t got = 0;
            uint64_t r = fs.name_data_alloc(root, 300, &got);
            CHECK(r == RC_OK && got == 300 * SLOT,
                  "alloc(300) best-fit → off=%llu（期望 %llu）", (unsigned long long)got,
                  (unsigned long long)(300 * SLOT));
        }

        printf("\n--- Phase 4: 补插至 338 条 → 叶分裂 + 内节点；audit 校验 fr 树 ---\n");
        for (uint64_t i = 301; i <= 338; i++) {
            uint64_t r = fs.name_data_free(root, i * SLOT, (uint16_t)i);
            if (r != RC_OK) { CHECK(false, "fill free #%llu rc=%llu", (unsigned long long)i, (unsigned long long)r); break; }
        }
        frc = rd_fr_count(fs, root);
        printf("  [info] 分裂后 fr_count=%llu（338 触发叶裂）\n", (unsigned long long)frc);
        CHECK(frc == 338, "fr_count==338（叶裂后）got=%llu", (unsigned long long)frc);

        /* 再 best-fit 一次，分裂后仍正确 */
        {
            uint64_t ctl = 0; raven_dir_Inode_t d; juv_inode_read(fs, root, (raven_Inode_t *)&d);
            ctl = d.name_data_free_ranges_ctl;
            uint64_t base = 0, len = 0;
            uint64_t r = fs.name_data_fr_find_fit(RFS_ND_FR_CTL_PTR(ctl), 338, &base, &len);
            CHECK(r == RC_OK && len == 338, "分裂后 find_fit(338) → len=%llu", (unsigned long long)len);
        }

        audit_walk_report_t rep;
        uint64_t a = fs.audit_dfs_root(&rep, stdout);
        CHECK(a == RC_OK && rep.total_bad() == 0,
              "含分裂 fr 树的 audit 全净 (bad=%llu heap=%llu leak=%llu mc=%llu)",
              (unsigned long long)rep.total_bad(), (unsigned long long)rep.heap_bad,
              (unsigned long long)rep.leaked_blk, (unsigned long long)rep.multi_claim_blk);

        printf("\n--- Phase 5: release 全量回收 + audit ---\n");
        uint64_t r3 = fs.name_data_release(root);
        CHECK(r3 == RC_OK, "release rc=%llu", (unsigned long long)r3);
        CHECK(rd_fr_count(fs, root) == 0, "回收后 fr_count==0");
        audit_walk_report_t rep2;
        uint64_t a2 = fs.audit_dfs_root(&rep2, stdout);
        CHECK(a2 == RC_OK && rep2.total_bad() == 0, "release 后 audit 全净 (bad=%llu)",
              (unsigned long long)rep2.total_bad());
    }

    printf("\n══════════ SUMMARY: %d passed, %d failed ══════════\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

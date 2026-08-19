/* 临时验证：挂载/卸载 + 双位图分配器（B+tree 无关）
 * 位图接口组是 private（对外只暴露 root_dir_*），测试用
 * #define private public 绕过访问控制，同时可直接检查内部状态。
 *
 * 闭环：实例1 带分配状态析构（验证全量写回）→
 *       实例2 重挂载验证占用 → 释放后析构 →
 *       实例3 重挂载验证恢复 */
#define private public
#include "../include/ravenfs/ravenfs_v1.1.hpp"
#undef private
#include <cstdio>

static uint64_t count_used_blocks(RavenFS_juvenile &fs) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < fs.total_blocks; i++)
        if (fs.block_bits[i / 8] & (1u << (i % 8))) n++;
    return n;
}
static uint64_t count_used_inodes(RavenFS_juvenile &fs) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < fs.inode_count; i++)
        if (fs.inode_bits[i / 8] & (1u << (i % 8))) n++;
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <image>\n", argv[0]); return 2; }

    uint64_t rc = 0;
    uint64_t b1 = 0, i1 = 0;
    uint64_t base_blocks = 0, base_inodes = 0;

    /* ── 实例1：分配，不释放，析构（触发全量写回） ── */
    {
        RavenFS_juvenile fs(argv[1], &rc);
        if (!fs.is_mounted()) { printf("MOUNT1 FAILED rc=%llu\n", (unsigned long long)rc); return 1; }
        base_blocks = count_used_blocks(fs);
        base_inodes = count_used_inodes(fs);
        printf("=== mount1 OK (base used=%llu blk, %llu inode) ===\n",
               (unsigned long long)base_blocks,
               (unsigned long long)base_inodes);

        b1 = fs.block_alloc_hard(2, 1);
        uint64_t b2 = fs.block_alloc_hard(1, 0);
        uint64_t remain = 999;
        uint64_t b3 = fs.block_alloc_soft(100, &remain);
        printf("hard(2,align1)=%llu  hard(1)=%llu  soft(100,remain=%llu)=%llu\n",
               (unsigned long long)b1, (unsigned long long)b2,
               (unsigned long long)remain, (unsigned long long)b3);
        printf("in-mem test: b1=%d b2=%d b3=%d\n",
               fs.block_test(b1), fs.block_test(b2), fs.block_test(b3));
        i1 = fs.inode_alloc();
        printf("inode_alloc=%llu  test=%d  root_bit0=%d\n",
               (unsigned long long)i1, fs.inode_test(i1), fs.inode_test(0));
        printf("--- dtor: full flush ---\n");
    }

    /* ── 实例2：重挂载，必须看到占用；然后释放 ── */
    {
        RavenFS_juvenile fs(argv[1], &rc);
        if (!fs.is_mounted()) { printf("MOUNT2 FAILED rc=%llu\n", (unsigned long long)rc); return 1; }
        uint64_t ub = count_used_blocks(fs), ui = count_used_inodes(fs);
        printf("=== mount2 OK (used=%llu blk, %llu inode) ===\n",
               (unsigned long long)ub, (unsigned long long)ui);
        printf("persist check: block_test(b1=%llu)=%d  inode_test(i1=%llu)=%d\n",
               (unsigned long long)b1, fs.block_test(b1),
               (unsigned long long)i1, fs.inode_test(i1));
        bool ok = fs.block_test(b1) && fs.inode_test(i1)
               && ub == base_blocks + 103 && ui == base_inodes + 1;
        printf("expect used=%llu/%llu → %s\n",
               (unsigned long long)(base_blocks + 103),
               (unsigned long long)(base_inodes + 1),
               ok ? "PASS" : "FAIL");

        /* 释放全部 */
        fs.block_free(b1, 2);
        fs.block_free(b1 + 2, 1);
        fs.block_free(b1 + 3, 100);
        fs.inode_free(i1);
        printf("--- dtor: full flush (freed) ---\n");
    }

    /* ── 实例3：重挂载，必须恢复原状 ── */
    {
        RavenFS_juvenile fs(argv[1], &rc);
        if (!fs.is_mounted()) { printf("MOUNT3 FAILED rc=%llu\n", (unsigned long long)rc); return 1; }
        uint64_t ub = count_used_blocks(fs), ui = count_used_inodes(fs);
        printf("=== mount3 OK (used=%llu blk, %llu inode) ===\n",
               (unsigned long long)ub, (unsigned long long)ui);
        bool ok = !fs.block_test(b1) && !fs.inode_test(i1) && ub == base_blocks && ui == base_inodes;
        printf("expect used=%llu/%llu → %s\n",
               (unsigned long long)base_blocks,
               (unsigned long long)base_inodes,
               ok ? "PASS" : "FAIL");
        printf("ALL %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
}

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_inode_data_plane.c —— 全局 inode 数据面 struct rfs_inode_plane 独立单测
 *
 * 证明该单元**用户态可独立链接测试**：本测试只编 `../linux/inode_data_plane.c`
 * + `../linux/ravenfs_rbtree_intrusive.c`（**不编 router / slice / god**）。
 * grow 所需的 block 分配符号由本文件打桩（证明面不依赖 router 实现）。
 *
 * 覆盖：init 校验/自清理 · 翻译面(dump_extents/locate) · 空闲面 best-fit 头分配
 * · free 融合/幂等 · test · capacity/free_total · grow 失败路径 · set_direct · 池平衡。
 */
#include "ravenfs_allocator_top.h"	/* rfs_inode_plane + rfs_allocator 前向/声明 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

/* ══════════════════ grow 打桩（本测试不链 router：证明面自足）══════════════════ */
u64 rfs_allocator_block_alloc_hard(struct rfs_allocator *self, u64 n, u8 align)
{
	(void)self; (void)n; (void)align;
	return RFS_IV_NO_HIT;			/* 恒失败：grow 走 -ENOSPC 路径 */
}
int rfs_allocator_block_free(struct rfs_allocator *self, u64 base, u64 count)
{
	(void)self; (void)base; (void)count;
	return 0;
}

/* ══════════════════ 计数分配器 ══════════════════ */
static u64 g_allocs, g_frees, g_live;

static void *t_alloc(void *ctx, u64 size)
{
	void *p = calloc(1, (size_t)size);

	(void)ctx;
	if (p) { g_allocs++; g_live++; }
	return p;
}
static void t_free(void *ctx, void *p)
{
	(void)ctx;
	if (p) { g_frees++; g_live--; free(p); }
}
static struct rfs_mem_alloc g_mem = { t_alloc, t_free, NULL };

#define PIECES   8ULL
#define TOTAL_INO (PIECES * RFS_IDP_INODES_PER_PIECE)	/* 256 */

static struct rfs_inode_extent g_ext[1];
static struct inode_dataplane_coordinate_free_range_t g_fr[4];
static struct rfs_allocator g_owner;		/* 占位 owner（stat 钩子全 0 = no-op）*/

static void make_cfg(struct rfs_inode_plane_cfg *cfg, u64 frn)
{
	memset(cfg, 0, sizeof(*cfg));
	g_ext[0].lblk = 0;
	g_ext[0].phys_base = 0;
	g_ext[0].length = PIECES * 2ULL;
	cfg->pieces = PIECES;
	cfg->extents = g_ext;
	cfg->extent_count = 1;
	cfg->fr = g_fr;
	cfg->fr_count = frn;
	cfg->grow_batch_pieces = 1;
	cfg->mem = g_mem;
}

int main(void)
{
	struct rfs_inode_plane p;
	struct rfs_inode_plane_cfg cfg;
	struct inode_dataplane_coordinate_free_range_t out[8];
	struct rfs_inode_extent xe[4];
	u64 a, b, blkbase;
	u32 off;
	int i;

	printf("=== rfs_inode_plane standalone tests ===\n");
	memset(&g_owner, 0, sizeof(g_owner));

	/* [1] init 校验 / 自清理 */
	puts("[1] init 校验");
	memset(&cfg, 0, sizeof(cfg));
	memset(&p, 0, sizeof(p));
	CHECK(rfs_inode_plane_init(&p, &cfg, &g_owner) == -EINVAL, "null pieces -> -EINVAL");
	CHECK(rfs_inode_plane_init(NULL, &cfg, &g_owner) == -EINVAL, "null self -> -EINVAL");
	make_cfg(&cfg, 0);
	cfg.mem.alloc = NULL;
	CHECK(rfs_inode_plane_init(&p, &cfg, &g_owner) == -EINVAL, "null mem -> -EINVAL");

	/* [2] 正常 init：fr = {1,4} {100,2} {200,10} → free 16 */
	puts("[2] init + 几何");
	g_fr[0].base = 1;   g_fr[0].len = 4;
	g_fr[1].base = 100; g_fr[1].len = 2;
	g_fr[2].base = 200; g_fr[2].len = 10;
	make_cfg(&cfg, 3);
	CHECK(rfs_inode_plane_init(&p, &cfg, &g_owner) == 0, "init ok");
	CHECK(rfs_inode_plane_pieces(&p) == PIECES, "pieces==8");
	CHECK(rfs_inode_plane_free_total(&p) == 16, "free_total==16");
	CHECK(rfs_inode_plane_extent_count(&p) == 1, "extent_count==1");
	CHECK(rfs_inode_plane_test(&p, 0) == true, "idx0 root allocated");
	CHECK(rfs_inode_plane_test(&p, 1) == false, "idx1 free (in fr)");
	CHECK(rfs_inode_plane_test(&p, TOTAL_INO) == false, "OOB not allocated");

	/* [3] alloc best-fit：最小 run 头（len2 @100 → len1 @101 → len4 @1 ...） */
	puts("[3] alloc best-fit 头");
	CHECK(rfs_inode_plane_alloc(&p, &a) == 0 && a == 100, "alloc#1 == 100 (min run)");
	CHECK(rfs_inode_plane_alloc(&p, &b) == 0 && b == 101, "alloc#2 == 101 (残 len1)");
	CHECK(rfs_inode_plane_free_total(&p) == 14, "free 16-2==14");
	CHECK(rfs_inode_plane_test(&p, 100) == true, "100 allocated");
	CHECK(rfs_inode_plane_alloc(&p, &a) == 0 && a == 1, "alloc#3 == 1 (next min len4)");
	CHECK(rfs_inode_plane_alloc(&p, &b) == 0 && b == 2, "alloc#4 == 2");
	CHECK(rfs_inode_plane_alloc(&p, &a) == 0 && a == 3, "alloc#5 == 3");
	CHECK(rfs_inode_plane_alloc(&p, &b) == 0 && b == 4, "alloc#6 == 4");
	CHECK(rfs_inode_plane_free_total(&p) == 10, "free == 10");

	/* [4] free 融合 + 幂等 */
	puts("[4] free 融合 / 幂等");
	CHECK(rfs_inode_plane_dump_fr(&p, out, 8) == 0, "dump_fr ok");
	/* fr 现 = {200,10}（{1,4}{100,2} 已耗尽） */
	CHECK(out[0].base == 200 && out[0].len == 10, "fr = [200,210)");
	rfs_inode_plane_free(&p, 100);
	rfs_inode_plane_free(&p, 101);		/* 融合回 {100,2} */
	CHECK(rfs_inode_plane_free_total(&p) == 12, "free restored 12");
	rfs_inode_plane_free(&p, 100);		/* 幂等：重复释放忽略 */
	CHECK(rfs_inode_plane_free_total(&p) == 12, "idempotent free");
	rfs_inode_plane_free(&p, 0);		/* root 释放忽略 */
	CHECK(rfs_inode_plane_free_total(&p) == 12, "root free ignored");
	CHECK(rfs_inode_plane_test(&p, 100) == false, "100 freed");

	/* [5] dump_extents + set_direct + locate */
	puts("[5] 翻译面 dump_extents / set_direct / locate");
	CHECK(rfs_inode_plane_dump_extents(&p, xe, 4) == 0, "dump_extents ok");
	CHECK(xe[0].lblk == 0 && xe[0].length == PIECES * 2ULL, "extent dense");
	rfs_inode_plane_set_direct(&p, 1000, PIECES);
	CHECK(rfs_inode_plane_extent_count(&p) == 1, "set_direct -> 1 extent");
	CHECK(rfs_inode_plane_locate(&p, 0, &blkbase, &off) == 0 &&
	      blkbase == 1000 && off == 0, "locate idx0");
	CHECK(rfs_inode_plane_locate(&p, 31, &blkbase, &off) == 0 &&
	      blkbase == 1000 && off == 31 * RFS_IDP_INODE_BYTES, "locate idx31 in-piece");
	CHECK(rfs_inode_plane_locate(&p, 32, &blkbase, &off) == 0 &&
	      blkbase == 1002 && off == 0, "locate idx32 next piece");
	CHECK(rfs_inode_plane_dump_extents(&p, xe, 0) == -ENOSPC, "dump cap 0 -> -ENOSPC");

	/* [6] 耗尽 + grow 失败（打桩 block 恒 NO_HIT）*/
	puts("[6] 耗尽 -> grow 失败 -> -ENOSPC");
	/* fr 现 = {100,2} {200,10}（free 12）；best-fit 小 run 优先逐个吃 */
	for (i = 0; i < 4; i++)
		(void)rfs_inode_plane_alloc(&p, &a);	/* 100,101,200,201 */
	CHECK(rfs_inode_plane_free_total(&p) == 8, "free 12-4==8");
	while (rfs_inode_plane_free_total(&p) > 0)
		(void)rfs_inode_plane_alloc(&p, &a);
	CHECK(rfs_inode_plane_free_total(&p) == 0, "fr drained");
	CHECK(rfs_inode_plane_alloc(&p, &a) == -ENOSPC, "grow fail -> -ENOSPC");

	/* [7] fini + 池平衡 */
	puts("[7] fini / 池平衡");
	rfs_inode_plane_fini(&p);
	CHECK(!p.active, "fini inactive");
	rfs_inode_plane_fini(&p);		/* 幂等 */
	CHECK(g_live == 0 && g_allocs == g_frees, "pool balanced");
	printf("--- pool: allocs=%llu frees=%llu live=%llu\n",
	       (unsigned long long)g_allocs, (unsigned long long)g_frees,
	       (unsigned long long)g_live);
	printf("=== PASS %d  FAIL %d ===\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

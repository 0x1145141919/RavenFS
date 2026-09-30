// SPDX-License-Identifier: GPL-2.0
/*
 * sim/sim_prefix.c —— BUG-1「修前行为」对照（把预置占用下的失败 occupy 后接一次
 * hard(1) 的同址性在 MEM/oracle 与 PRE-FIX CHUNK 之间对比）。
 *
 * 链接 = bm_harness.c + linux/bitmap_core.c + linux/bitmap_mem.c + sim/bitmap_chunk_prefix.c
 * （**不链接冻结的 linux/bitmap_chunk.c**，故 rfs_bm_chunk_ops 来自 PRE-FIX 模拟）。
 *
 * 期望（见 regression_bug1）：
 *   预置 [64,128) 占用；occupy(base=64,len=1) → 0 认领失败；
 *   随后 hard(1,0)：MEM 与 oracle 均 0（hint 未动）；PRE-FIX CHUNK = 128（hint 被推到锚点 64）。
 * 退出码：0 = 复现了「修前与 MEM 分叉」；3 = 未复现（意外）。
 */
#include "bm_harness.h"

int main(void)
{
	env_t em, ec;
	oracle_t om;
	u64 rm = 99, rc = 99, om_hit, dm, dc, po;

	pin_to_cpu(first_allowed_cpu());
	if (env_prep_mem(&em, 8, 8, 1) != 0 || env_prep_chunk(&ec, 8, 8, 1, 0, 0, 0) != 0)
		return 2;
	words_set_range(em.blk_words, 64, 64);
	words_set_range(ec.blk_words, 64, 64);
	if (env_go(&em) != 0 || env_go(&ec) != 0)
		return 2;

	o_init(&om, em.bm.blk.nbits, em.bm.blk.per_bits, em.bm.blk.n_slices,
	       em.cpu_slots, (const u64 *)em.blk_words);	/* 须在任何分配之前 */

	om_hit = rfs_bm_block_occupy_soft(&em.bm, 64, 1, &rm);	/* MEM：失败 occupy */
	(void)om_hit;
	(void)rfs_bm_block_occupy_soft(&ec.bm, 64, 1, &rc);	/* PRE-FIX CHUNK：失败 occupy */

	dm = rfs_bm_block_alloc_hard(&em.bm, 1, 0);
	dc = rfs_bm_block_alloc_hard(&ec.bm, 1, 0);

	po = o_alloc_hard(&om, 1, 0);

	printf("MEM hard(1)      = %llu\n", (unsigned long long)dm);
	printf("oracle hard(1)   = %llu\n", (unsigned long long)po);
	printf("PREFIX CHUNK     = %llu  (pre-fix：hint 被推到锚点 64 → 命中 128)\n",
	       (unsigned long long)dc);
	printf("MEM/occupy rest  = %llu ; PRE-FIX rest = %llu (期望均 1)\n",
	       (unsigned long long)rm, (unsigned long long)rc);

	o_fini(&om);
	env_destroy(&em);
	env_destroy(&ec);
	return (dm == po && dc != po) ? 0 : 3;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * test_mdata_cache_core.c —— mdata 节点缓存核心（平台中立）用户态单测
 *
 * 直接编译 ../linux/mdata_cache_core.c（+ 兼容 TU ../linux/mdata_cache_self_contained.c），
 * 不链 god / ko 其它任何东西。
 *
 * 覆盖：
 *   1  两档几何：CTRL_ARRAY（bits<=13）/ RADIX（14..52）逐档 statey 校验；
 *      bits>52 拒绝；node-count-based bits（含奇数 total_blocks 尾部）。
 *   2  get miss→load→hit→put / ref 账目 / 重复 get-put / 越界 / 尾部 nblk=1。
 *   3  dirty→wb_dirty(ignore_age=false/true) / wb_node；内容与 nblk；DIRTY 清位。
 *   4  drop：clean+ref0 释放 / dirty+ref>0 清脏不释放 / dirty+ref0 丢弃不回写。
 *   5  reap：冷+净+ref0 回收；脏/ref>0/刚访问不回收；返回计数。
 *   6  flush_all：脏先落盘、全部行清空、幂等、fini 无泄漏。
 *   7  注入分配器计数（mem / pool allocs==frees，含失败回滚）。
 *   8  故障注入：mem.alloc / pool.alloc / read_block 第 N 次失败。
 *   9  多线程 get/put/dirty + wb + reap + drop + strong reap 并发烟测（ASan/TSan）。
 *  10  强淘汰：PRESSURE / LOWLOAD 两 gear、脏行写回/失败恢复、空表摘除与 get 重建
 *      （RADIX 单层/深树、CTRL_ARRAY 不摘表）、out==NULL / 非法 gear。
 *
 * 说明：本测试的 mem/pool 分配器均用 calloc，满足核心「零初始化」契约；
 * 测试侧直接读 row->ctrl 解位域（结构是公共头的一部分）。
 */
#include "ravenfs_mdata_cache_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, msg) do {						\
	if (cond) {							\
		g_pass++;						\
	} else {							\
		g_fail++;						\
		printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);	\
	}								\
} while (0)

/* ══════════════════════════ 注入分配器（计数 + 故障）══════════════════════════ */

struct alloc_stats {
	u64	allocs;
	u64	frees;
	u64	live;
	u64	calls;
	long	fail_at;	/* 第 N 次调用失败（1-based）；0 = 不失败 */
};

static u64 stat_load(u64 *p)
{
	return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static void *stats_alloc(struct alloc_stats *st, u64 size)
{
	void *p;
	u64 n = __atomic_add_fetch(&st->calls, 1, __ATOMIC_RELAXED);
	long fail = __atomic_load_n(&st->fail_at, __ATOMIC_RELAXED);

	if (fail && n == (u64)fail)
		return NULL;
	p = calloc(1, (size_t)size);
	if (!p)
		return NULL;
	__atomic_add_fetch(&st->allocs, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&st->live, 1, __ATOMIC_RELAXED);
	return p;
}

static void stats_free(struct alloc_stats *st, void *p)
{
	if (!p)
		return;
	__atomic_add_fetch(&st->frees, 1, __ATOMIC_RELAXED);
	__atomic_sub_fetch(&st->live, 1, __ATOMIC_RELAXED);
	free(p);
}

static void *t_mem_alloc(void *ctx, u64 size)
{
	return stats_alloc(ctx, size);
}

static void t_mem_free(void *ctx, void *p)
{
	stats_free(ctx, p);
}

static void *t_pool_alloc(void *ctx, u64 size)
{
	return stats_alloc(ctx, size);
}

static void t_pool_free(void *ctx, void *p, u64 size)
{
	(void)size;
	stats_free(ctx, p);
}

/* ══════════════════════════ 假盘（可计数 / 可内容校验）══════════════════════════ */

struct fake_disk {
	u64	total_blocks;
	u8	*mem;		/* NULL = 按块号生成只读图案（大几何用） */
	u64	read_calls;
	u64	write_calls;
	u64	reads;
	u64	writes;
	u64	last_write_blk;
	u64	last_write_nblk;
	u64	last_read_nblk;
	long	read_fail_at;
	long	write_fail_at;
};

static u8 disk_pattern(u64 blk, u64 off)
{
	return (u8)((blk * 131u + off * 17u + 0x5Au) & 0xFFu);
}

static void disk_fill(u8 *dst, u64 blk, u64 nblk)
{
	u64 b, j;

	for (b = 0; b < nblk; b++)
		for (j = 0; j < RAVENFS_MDATA_BLK_BYTES; j++)
			dst[b * RAVENFS_MDATA_BLK_BYTES + j] =
				disk_pattern(blk + b, j);
}

static int t_read_block(void *ctx, u64 blk, u64 nblk, void *buf)
{
	struct fake_disk *d = ctx;
	u64 call = __atomic_add_fetch(&d->read_calls, 1, __ATOMIC_RELAXED);

	if (d->read_fail_at && call == (u64)d->read_fail_at)
		return -EIO;
	if (blk + nblk > d->total_blocks)
		return -EIO;
	if (d->mem)
		memcpy(buf, d->mem + blk * RAVENFS_MDATA_BLK_BYTES,
		       (size_t)(nblk * RAVENFS_MDATA_BLK_BYTES));
	else
		disk_fill(buf, blk, nblk);
	__atomic_add_fetch(&d->reads, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&d->last_read_nblk, nblk, __ATOMIC_RELAXED);
	return 0;
}

static int t_write_block(void *ctx, u64 blk, u64 nblk, const void *buf)
{
	struct fake_disk *d = ctx;
	u64 call = __atomic_add_fetch(&d->write_calls, 1, __ATOMIC_RELAXED);

	if (d->write_fail_at && call == (u64)d->write_fail_at)
		return -EIO;
	if (blk + nblk > d->total_blocks)
		return -EIO;
	if (d->mem)
		memcpy(d->mem + blk * RAVENFS_MDATA_BLK_BYTES, buf,
		       (size_t)(nblk * RAVENFS_MDATA_BLK_BYTES));
	__atomic_add_fetch(&d->writes, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&d->last_write_blk, blk, __ATOMIC_RELAXED);
	__atomic_store_n(&d->last_write_nblk, nblk, __ATOMIC_RELAXED);
	return 0;
}

/* ══════════════════════════ 测试夹具 ══════════════════════════ */

struct md_stat_collector {
	u64	v[RAVENFS_MD_STAT_COUNT];
};

struct fixture {
	struct ravenfs_mdata_cache	c;
	struct ravenfs_mdata_cfg		cfg;
	struct alloc_stats		mem;
	struct alloc_stats		pool;
	struct fake_disk		disk;
	struct md_stat_collector	stat;
	int				ready;
};

static void t_stat_inc(void *ctx, u32 field, u64 delta)
{
	struct md_stat_collector *st = ctx;

	if (field < RAVENFS_MD_STAT_COUNT)
		__atomic_add_fetch(&st->v[field], delta, __ATOMIC_RELAXED);
}

static u64 t_stat_get(struct fixture *fx, u32 field)
{
	return __atomic_load_n(&fx->stat.v[field], __ATOMIC_RELAXED);
}

static int fx_storage_init(struct fixture *fx, u64 total_blocks)
{
	fx->disk.total_blocks = total_blocks;
	fx->disk.mem = calloc((size_t)total_blocks,
			      RAVENFS_MDATA_BLK_BYTES);
	if (!fx->disk.mem)
		return -1;
	disk_fill(fx->disk.mem, 0, total_blocks);
	return 0;
}

/* storage = 1：分配真实假盘（内容校验）；0：按块号生成图案（大几何/MT）。*/
static int fx_init(struct fixture *fx, u64 total_blocks, int storage)
{
	memset(fx, 0, sizeof(*fx));
	if (storage && fx_storage_init(fx, total_blocks) != 0)
		return -1;

	fx->disk.total_blocks = total_blocks;
	fx->cfg.total_blocks = total_blocks;
	fx->cfg.mem.alloc = t_mem_alloc;
	fx->cfg.mem.free = t_mem_free;
	fx->cfg.mem.ctx = &fx->mem;
	fx->cfg.pool.alloc = t_pool_alloc;
	fx->cfg.pool.free = t_pool_free;
	fx->cfg.pool.ctx = &fx->pool;
	fx->cfg.io.read_block = t_read_block;
	fx->cfg.io.write_block = t_write_block;
	fx->cfg.io.ctx = &fx->disk;
	fx->cfg.stat.inc = t_stat_inc;
	fx->cfg.stat.ctx = &fx->stat;

	fx->ready = (ravenfs_mdata_cache_init(&fx->c, &fx->cfg) == 0);
	return fx->ready ? 0 : -1;
}

static void fx_fini(struct fixture *fx)
{
	ravenfs_mdata_cache_fini(&fx->c);
	free(fx->disk.mem);
	fx->disk.mem = NULL;
}

static void fx_check_balanced(struct fixture *fx, const char *tag)
{
	u64 ma = stat_load(&fx->mem.allocs), mf = stat_load(&fx->mem.frees);
	u64 ml = stat_load(&fx->mem.live);
	u64 pa = stat_load(&fx->pool.allocs), pf = stat_load(&fx->pool.frees);
	u64 pl = stat_load(&fx->pool.live);

	if (ma != mf || ml != 0 || pa != pf || pl != 0)
		printf("  [%s] mem a=%llu f=%llu live=%llu | "
		       "pool a=%llu f=%llu live=%llu\n", tag,
		       (unsigned long long)ma, (unsigned long long)mf,
		       (unsigned long long)ml, (unsigned long long)pa,
		       (unsigned long long)pf, (unsigned long long)pl);
	CHECK(ma == mf, tag);	/* mem alloc/free 配平 */
	CHECK(ml == 0, tag);
	CHECK(pa == pf, tag);
	CHECK(pl == 0, tag);
}

/* ══════════════════════════ ctrl 位域读取 ══════════════════════════ */

static u64 row_ctrl(struct ravenfs_mrow_simple *row)
{
	return (u64)RFS_ATOMIC64_READ(&row->ctrl);
}

static u32 row_state(struct ravenfs_mrow_simple *row)
{
	return (u32)(row_ctrl(row) & RAVENFS_MROW_STATE_MASK);
}

static u32 row_ref(struct ravenfs_mrow_simple *row)
{
	return (u32)((row_ctrl(row) >> RAVENFS_MROW_REF_SHIFT) &
		     RAVENFS_MROW_REF_MASK);
}

static int row_dirty(struct ravenfs_mrow_simple *row)
{
	return !!(row_ctrl(row) & RAVENFS_MROW_FLAG(RAVENFS_MROW_F_DIRTY));
}

/* ══════════════════════════ 1. 几何 ══════════════════════════ */

static void test_geometry(void)
{
	struct ravenfs_mdata_geom g;
	u32 b, i;

	printf("[geometry]\n");

	CHECK(ravenfs_mdata_geometry(0, &g) == -EINVAL, "total_blocks=0 rejected");

	/* 小盘 → CTRL_ARRAY */
	CHECK(ravenfs_mdata_geometry(1024, &g) == 0, "1024 geom");
	CHECK(g.n_nodes == 512, "1024 n_nodes=512");
	CHECK(g.bits == 9, "1024 bits=9 (node-count based)");
	CHECK(g.mode == RAVENFS_MDATA_MODE_CTRL_ARRAY, "1024 CTRL_ARRAY");
	CHECK(g.n_slices == 1, "1024 one shard");
	CHECK(g.leaf_width == 512, "1024 leaf=n_nodes");

	/* 边界：bits 13/14 */
	CHECK(ravenfs_mdata_geometry(16384, &g) == 0, "16384 geom");
	CHECK(g.n_nodes == 8192 && g.bits == 13, "16384 -> bits 13");
	CHECK(g.mode == RAVENFS_MDATA_MODE_CTRL_ARRAY, "bits 13 CTRL_ARRAY");

	/* 32768 块 → 16384 节点 → bits 14 → statey_14 {7,7} → 128 片 */
	CHECK(ravenfs_mdata_geometry(32768, &g) == 0, "32768 geom");
	CHECK(g.n_nodes == 16384, "32768 n_nodes=16384");
	CHECK(g.bits == 14, "32768 bits=14");
	CHECK(g.mode == RAVENFS_MDATA_MODE_RADIX, "32768 RADIX");
	CHECK(g.n_slices == 128, "32768 128 shards");
	CHECK(g.w0 == 7 && g.shard_bits == 7, "32768 w0=7 shard_bits=7");
	CHECK(g.total_levels == 2 && g.tree_levels == 1, "32768 levels");
	CHECK(g.leaf_width == 128, "32768 leaf_width=128");

	/* 65536 块 → 32768 节点 → bits 15 → statey {8,7} → 256 片 */
	CHECK(ravenfs_mdata_geometry(65536, &g) == 0, "65536 geom");
	CHECK(g.bits == 15 && g.n_slices == 256, "65536 bits=15/256 shards");
	CHECK(g.w0 == 8 && g.shard_bits == 7, "65536 w0=8/shard=7");

	/* 524288 块 → 262144 节点 → bits 18 → statey {7,5,6} */
	CHECK(ravenfs_mdata_geometry(524288, &g) == 0, "524288 geom");
	CHECK(g.bits == 18 && g.mode == RAVENFS_MDATA_MODE_RADIX,
	      "524288 bits=18 RADIX");
	CHECK(g.n_slices == 128 && g.w0 == 7, "524288 128 shards");
	CHECK(g.total_levels == 3 && g.tree_levels == 2, "524288 3 levels");
	CHECK(g.leaf_width == 64, "524288 leaf_width=64");

	/* 逐档 14..52：sum(level_bits)==bits、层数/宽度自洽 */
	for (b = 14; b <= RAVENFS_MDATA_RADIX_MAX_BITS; b++) {
		u64 total = (1ULL << (b + 1)) - 1;	/* n_nodes = 2^b */
		u32 sum = 0, tree_sum = 0;

		CHECK(ravenfs_mdata_geometry(total, &g) == 0, "statey geom");
		CHECK(g.bits == b, "statey bits");
		CHECK(g.n_nodes == (1ULL << b), "statey n_nodes");
		CHECK(g.mode == RAVENFS_MDATA_MODE_RADIX, "statey RADIX");
		CHECK(g.total_levels >= 2 &&
		      g.total_levels <= RAVENFS_MDATA_MAX_LEVELS,
		      "statey levels range");
		CHECK(g.tree_levels == g.total_levels - 1, "statey tree_levels");
		CHECK(g.n_slices == (1u << g.w0), "statey n_slices");
		CHECK(g.n_slices <= (1u << RAVENFS_MDATA_SHARD_SEL_BITS),
		      "statey shard bound");
		for (i = 0; i < g.total_levels; i++)
			sum += g.level_bits[i];
		for (i = 0; i < g.tree_levels; i++)
			tree_sum += g.level_bits[1 + i];
		CHECK(sum == b, "statey sum(level_bits)==bits");
		CHECK(tree_sum == g.shard_bits, "statey tree sum==shard_bits");
		CHECK(g.leaf_width ==
		      (1u << g.level_bits[g.total_levels - 1]),
		      "statey leaf_width");
		CHECK(g.shift[g.tree_levels - 1] == 0,
		      "statey last shift==0");
	}

	/* 越档 */
	CHECK(ravenfs_mdata_geometry(1ULL << 54, &g) == -EINVAL,
	      "bits>52 rejected");
	CHECK(ravenfs_mdata_geometry(1ULL << 53, &g) == 0,
	      "bits=52 accepted");

	/* 退化 / 尾部 */
	CHECK(ravenfs_mdata_geometry(1, &g) == 0 &&
	      g.bits == 1 && g.leaf_width == 1, "1 block -> 1 node/bits 1");
	CHECK(ravenfs_mdata_geometry(5, &g) == 0 &&
	      g.n_nodes == 3 && g.bits == 2 && g.leaf_width == 3,
	      "5 blocks -> 3 nodes/bits 2");
}

/* ══════════════════════════ 2. get / put / read ══════════════════════════ */

/* RAII 探针：进入即 get，提前 return 时 cleanup 必须自动 put。*/
static int raii_probe(struct fixture *fx, u64 blk)
{
	RAVENFS_MDATA_SESSION(t, &fx->c, blk);

	if (!RAVENFS_MDATA_SESSION_VALID(t))
		return -1;
	if (RAVENFS_MDATA_SESSION_DATA(t) != ravenfs_msession_node(t))
		return -2;
	return 1;
}

static void test_get_put_read(void)
{
	struct fixture fx;
	struct ravenfs_msession *s, *s2;
	struct ravenfs_mrow_simple *row;
	u8 refbuf[RAVENFS_MDATA_NODE_BYTES];
	u64 node_idx = 3;		/* blk 6/7 */
	int rc;

	printf("[get/put/read]\n");
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "fixture init");
		return;
	}
	CHECK(fx.c.mode == RAVENFS_MDATA_MODE_CTRL_ARRAY, "fixture CTRL_ARRAY");
	CHECK(ravenfs_mdata_total_blocks(&fx.c) == 1024, "total_blocks audit");
	CHECK(ravenfs_mdata_shard_count(&fx.c) == 1, "shard_count audit");
	CHECK(ravenfs_mdata_cache_init(&fx.c, &fx.cfg) == -EINVAL,
	      "double init rejected");

	/* miss -> load */
	s = ravenfs_mdata_get(&fx.c, node_idx * 2);
	CHECK(!RFS_IS_ERR(s), "get miss ok");
	if (RFS_IS_ERR(s))
		goto out;
	row = s->row;
	CHECK(fx.disk.reads == 1, "one block read on miss");
	CHECK(fx.pool.live == 1, "one 8K buffer live");
	CHECK(row_state(row) == RAVENFS_MROW_READY, "row READY");
	CHECK(row_ref(row) == 1, "ref=1 after first get");
	CHECK(memcmp(ravenfs_msession_node(s), fx.disk.mem +
		     node_idx * 2 * RAVENFS_MDATA_BLK_BYTES,
		     RAVENFS_MDATA_NODE_BYTES) == 0, "loaded content");

	/* hit（同节点，偶数/奇数 blk 均映射同一 node_idx）*/
	s2 = ravenfs_mdata_get(&fx.c, node_idx * 2 + 1);
	CHECK(!RFS_IS_ERR(s2), "get hit ok");
	if (!RFS_IS_ERR(s2)) {
		CHECK(fx.disk.reads == 1, "no extra read on hit");
		CHECK(s2->row == row, "same row");
		CHECK(row_ref(row) == 2, "ref=2");
		ravenfs_mdata_put(s2);
	}
	CHECK(row_ref(row) == 1, "ref back to 1");

	/* read 路径 = get + memcpy + put */
	rc = ravenfs_mdata_read(&fx.c, node_idx * 2, refbuf);
	CHECK(rc == 0, "ravenfs_mdata_read ok");
	CHECK(memcmp(refbuf, ravenfs_msession_node(s),
		     RAVENFS_MDATA_NODE_BYTES) == 0, "read content");
	CHECK(fx.disk.reads == 1, "read used cache");
	ravenfs_mdata_put(s);
	CHECK(row_ref(row) == 0, "ref=0 after puts");

	/* 重复 get/put 账目 */
	{
		int i, ok = 1;

		for (i = 0; i < 100; i++) {
			struct ravenfs_msession *t =
				ravenfs_mdata_get(&fx.c, node_idx * 2);

			if (RFS_IS_ERR(t)) {
				ok = 0;
				break;
			}
			ravenfs_mdata_put(t);
		}
		CHECK(ok, "repeated get/put");
		CHECK(row_ref(row) == 0, "ref balanced after repeats");
	}

	/* RAII 会话宏：提前 return 也必须自动 put */
	CHECK(raii_probe(&fx, node_idx * 2) == 1, "RAII session probe");
	CHECK(row_ref(row) == 0, "RAII cleanup put on early return");

	/* 越界 */
	s = ravenfs_mdata_get(&fx.c, 1024);
	CHECK(RFS_IS_ERR(s) && RFS_PTR_ERR(s) == -EIO, "out-of-range -EIO");
	s = ravenfs_mdata_get(&fx.c, ~0ULL);
	CHECK(RFS_IS_ERR(s), "huge blk rejected");
	ravenfs_mdata_put(NULL);	/* NULL 容忍 */

out:
	fx_fini(&fx);
	fx_check_balanced(&fx, "get/put alloc balance");
}

/* ══════════════════════════ 3. 尾部节点 nblk=1 ══════════════════════════ */

static void test_tail_node(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	u8 refbuf[RAVENFS_MDATA_NODE_BYTES];
	int i, zero_tail = 1;

	printf("[tail node]\n");
	if (fx_init(&fx, 5, 1) != 0) {	/* 3 节点：2,2,1 块 */
		CHECK(0, "fixture init");
		return;
	}
	CHECK(fx.c.n_nodes == 3, "5 blocks -> 3 nodes");

	s = ravenfs_mdata_get(&fx.c, 4);	/* 尾节点：blk0=4，仅 1 块 */
	CHECK(!RFS_IS_ERR(s), "tail get");
	if (RFS_IS_ERR(s))
		goto out;
	CHECK(fx.disk.last_read_nblk == 1, "tail read nblk=1");
	CHECK(memcmp(ravenfs_msession_node(s),
		     fx.disk.mem + 4 * RAVENFS_MDATA_BLK_BYTES,
		     RAVENFS_MDATA_BLK_BYTES) == 0, "tail first block content");
	for (i = RAVENFS_MDATA_BLK_BYTES;
	     i < RAVENFS_MDATA_NODE_BYTES; i++) {
		if (((u8 *)ravenfs_msession_node(s))[i] != 0)
			zero_tail = 0;
	}
	CHECK(zero_tail, "tail second block zero");

	/* 尾部写回必须 nblk=1，且不越界 */
	memset(ravenfs_msession_node(s), 0x77, RAVENFS_MDATA_NODE_BYTES);
	RAVENFS_MDATA_DIRTY(s);
	CHECK(ravenfs_mdata_wb_node(&fx.c, 4) == 0, "tail wb_node");
	CHECK(fx.disk.last_write_nblk == 1, "tail write nblk=1");
	CHECK(fx.disk.mem[4 * RAVENFS_MDATA_BLK_BYTES] == 0x77,
	      "tail write content");
	ravenfs_mdata_put(s);

	CHECK(ravenfs_mdata_read(&fx.c, 4, refbuf) == 0, "tail read via cache");

out:
	fx_fini(&fx);
	fx_check_balanced(&fx, "tail alloc balance");
}

/* ══════════════════════════ 4. 脏回写 ══════════════════════════ */

static void test_dirty_wb(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	u64 blk = 10;
	int rc;

	printf("[dirty/wb]\n");
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "fixture init");
		return;
	}

	s = ravenfs_mdata_get(&fx.c, blk);
	CHECK(!RFS_IS_ERR(s), "get");
	if (RFS_IS_ERR(s))
		goto out;
	memset(ravenfs_msession_node(s), 0xAB, RAVENFS_MDATA_NODE_BYTES);
	RAVENFS_MDATA_DIRTY(s);
	CHECK(row_dirty(s->row), "DIRTY set");
	{
		u64 wb_before = fx.disk.writes;

		/* cutoff=0 且 ignore_age=false：wb_stamp >= 0 → 全部跳过 */
		rc = ravenfs_mdata_wb_dirty(&fx.c, 0, false);
		CHECK(rc == 0, "wb false ok");
		CHECK(fx.disk.writes == wb_before, "wb false skipped by age");
		CHECK(row_dirty(s->row), "still dirty after age skip");
	}
	rc = ravenfs_mdata_wb_dirty(&fx.c, ~0ULL, false);
	CHECK(rc == 0, "wb true ok");
	CHECK(fx.disk.writes == 1, "one write");
	CHECK(fx.disk.last_write_blk == blk, "write blk");
	CHECK(fx.disk.last_write_nblk == 2, "write nblk=2");
	CHECK(!row_dirty(s->row), "DIRTY cleared");
	CHECK(fx.disk.mem[blk * RAVENFS_MDATA_BLK_BYTES] == 0xAB,
	      "write content");

	/* 再脏 → writeback_all */
	memset(ravenfs_msession_node(s), 0xCD, RAVENFS_MDATA_NODE_BYTES);
	RAVENFS_MDATA_DIRTY(s);
	CHECK(ravenfs_mdata_writeback_all(&fx.c) == 0, "writeback_all ok");
	CHECK(fx.disk.writes == 2, "writeback_all wrote");
	CHECK(!row_dirty(s->row), "DIRTY cleared by writeback_all");

	/* wb_node：驻留脏行 */
	memset(ravenfs_msession_node(s), 0xEF, RAVENFS_MDATA_NODE_BYTES);
	RAVENFS_MDATA_DIRTY(s);
	CHECK(ravenfs_mdata_wb_node(&fx.c, blk) == 0, "wb_node ok");
	CHECK(fx.disk.writes == 3, "wb_node wrote");
	CHECK(!row_dirty(s->row), "wb_node cleared DIRTY");
	ravenfs_mdata_put(s);

	/* wb_node：未驻留视为已落盘（会先 load，但不写） */
	{
		u64 w = fx.disk.writes;

		CHECK(ravenfs_mdata_wb_node(&fx.c, 20) == 0,
		      "wb_node nonresident ok");
		CHECK(fx.disk.writes == w, "nonresident wb did not write");
	}

out:
	fx_fini(&fx);
	fx_check_balanced(&fx, "wb alloc balance");
}

/* ══════════════════════════ 5. drop ══════════════════════════ */

static void test_drop(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	struct ravenfs_mrow_simple *row;
	u64 blk = 30;

	printf("[drop]\n");
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "fixture init");
		return;
	}

	/* (a) clean + ref0：释放 + 回 IDLE */
	s = ravenfs_mdata_get(&fx.c, blk);
	CHECK(!RFS_IS_ERR(s), "get clean");
	if (RFS_IS_ERR(s))
		goto out;
	row = s->row;
	ravenfs_mdata_put(s);
	CHECK(fx.pool.live == 1, "clean resident");
	ravenfs_mdata_drop(&fx.c, blk);
	CHECK(fx.pool.live == 0, "drop clean freed data");
	CHECK(row_state(row) == RAVENFS_MROW_IDLE, "drop -> IDLE");
	CHECK(row->data == NULL, "drop data NULL");

	/* (b) dirty + ref>0：只清脏，不释放 */
	s = ravenfs_mdata_get(&fx.c, blk);
	CHECK(!RFS_IS_ERR(s), "get dirty");
	if (RFS_IS_ERR(s))
		goto out;
	row = s->row;
	RAVENFS_MDATA_DIRTY(s);
	ravenfs_mdata_drop(&fx.c, blk);
	CHECK(fx.pool.live == 1, "drop ref>0 keeps data");
	CHECK(!row_dirty(row), "drop ref>0 clears DIRTY");
	CHECK(row_state(row) == RAVENFS_MROW_READY, "drop ref>0 stays READY");
	ravenfs_mdata_put(s);

	/* (c) dirty + ref0：直接丢弃，不回写 */
	{
		u64 w = fx.disk.writes;

		ravenfs_mdata_drop(&fx.c, blk);
		CHECK(fx.pool.live == 0, "drop dirty ref0 frees data");
		CHECK(row_state(row) == RAVENFS_MROW_IDLE, "drop dirty -> IDLE");
		CHECK(fx.disk.writes == w, "drop dirty did not write back");
	}

	/* 越界 no-op */
	ravenfs_mdata_drop(&fx.c, 1024);
	ravenfs_mdata_drop(&fx.c, ~0ULL);
	CHECK(1, "drop out-of-range tolerated");

out:
	fx_fini(&fx);
	fx_check_balanced(&fx, "drop alloc balance");
}

/* ══════════════════════════ 6. reap ══════════════════════════ */

static void test_reap(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	struct ravenfs_mrow_simple *row;
	long n;

	printf("[reap]\n");
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "fixture init");
		return;
	}

	/* 冷 + 净 + ref0 → 回收 */
	s = ravenfs_mdata_get(&fx.c, 40);
	CHECK(!RFS_IS_ERR(s), "get cold");
	if (RFS_IS_ERR(s))
		goto out;
	row = s->row;
	RFS_ATOMIC64_SET(&row->acc_stamp, 1);
	ravenfs_mdata_put(s);
	n = ravenfs_mdata_reap_expired(&fx.c, 100);
	CHECK(n == 1, "cold clean ref0 reaped");
	CHECK(fx.pool.live == 0, "reap freed data");
	CHECK(row_state(row) == RAVENFS_MROW_IDLE, "reap -> IDLE");

	/* 脏 → 不回收 */
	s = ravenfs_mdata_get(&fx.c, 42);
	if (RFS_IS_ERR(s))
		goto out;
	row = s->row;
	RAVENFS_MDATA_DIRTY(s);
	RFS_ATOMIC64_SET(&row->acc_stamp, 1);
	ravenfs_mdata_put(s);
	n = ravenfs_mdata_reap_expired(&fx.c, 100);
	CHECK(n == 0, "dirty not reaped");
	CHECK(fx.pool.live == 1, "dirty still resident");
	ravenfs_mdata_drop(&fx.c, 42);

	/* ref>0 → 不回收 */
	s = ravenfs_mdata_get(&fx.c, 44);
	if (RFS_IS_ERR(s))
		goto out;
	row = s->row;
	RFS_ATOMIC64_SET(&row->acc_stamp, 1);
	n = ravenfs_mdata_reap_expired(&fx.c, 100);
	CHECK(n == 0, "ref>0 not reaped");
	CHECK(fx.pool.live == 1, "referenced still resident");
	ravenfs_mdata_put(s);
	ravenfs_mdata_drop(&fx.c, 44);

	/* 刚访问 → 不回收 */
	s = ravenfs_mdata_get(&fx.c, 46);
	if (RFS_IS_ERR(s))
		goto out;
	row = s->row;
	ravenfs_mdata_put(s);
	/* cutoff 取 1 秒前：行刚访问（acc_stamp > cutoff）→ 不回收 */
	n = ravenfs_mdata_reap_expired(&fx.c, RFS_NOW_NS() - 1000000000ULL);
	CHECK(n == 0, "hot not reaped");
	CHECK(fx.pool.live == 1, "hot still resident");

	/* cutoff 足够大 → 回收并返回计数 */
	n = ravenfs_mdata_reap_expired(&fx.c, ~0ULL);
	CHECK(n == 1, "future cutoff reaps cold-clean");
	CHECK(fx.pool.live == 0, "all clean reaped");

out:
	fx_fini(&fx);
	fx_check_balanced(&fx, "reap alloc balance");
}

/* ══════════════════════════ 7. flush_all ══════════════════════════ */

static void test_flush_all(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	struct ravenfs_mrow_simple *row;
	u64 w;

	printf("[flush_all]\n");
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "fixture init");
		return;
	}

	s = ravenfs_mdata_get(&fx.c, 60);
	if (RFS_IS_ERR(s))
		goto out;
	memset(ravenfs_msession_node(s), 0x11, RAVENFS_MDATA_NODE_BYTES);
	RAVENFS_MDATA_DIRTY(s);
	ravenfs_mdata_put(s);

	s = ravenfs_mdata_get(&fx.c, 62);
	if (RFS_IS_ERR(s))
		goto out;
	memset(ravenfs_msession_node(s), 0x22, RAVENFS_MDATA_NODE_BYTES);
	RAVENFS_MDATA_DIRTY(s);
	ravenfs_mdata_put(s);

	s = ravenfs_mdata_get(&fx.c, 64);	/* clean resident */
	if (RFS_IS_ERR(s))
		goto out;
	row = s->row;
	ravenfs_mdata_put(s);

	CHECK(fx.pool.live == 3, "3 resident rows");
	ravenfs_mdata_flush_all(&fx.c);
	CHECK(fx.pool.live == 0, "flush freed all data");
	CHECK(fx.disk.mem[60 * RAVENFS_MDATA_BLK_BYTES] == 0x11,
	      "flush wrote dirty 60");
	CHECK(fx.disk.mem[62 * RAVENFS_MDATA_BLK_BYTES] == 0x22,
	      "flush wrote dirty 62");
	CHECK(row_state(row) == RAVENFS_MROW_IDLE, "flush -> IDLE");

	/* 幂等 */
	w = fx.disk.writes;
	ravenfs_mdata_flush_all(&fx.c);
	CHECK(fx.disk.writes == w, "second flush no writes");
	CHECK(fx.pool.live == 0, "second flush no alloc");

	/* flush 后仍可重新装载 */
	s = ravenfs_mdata_get(&fx.c, 60);
	CHECK(!RFS_IS_ERR(s), "get after flush");
	if (!RFS_IS_ERR(s)) {
		CHECK(((u8 *)ravenfs_msession_node(s))[0] == 0x11,
		      "reload sees flushed content");
		ravenfs_mdata_put(s);
	}

out:
	fx_fini(&fx);		/* fini = flush + DFS free */
	fx_check_balanced(&fx, "flush alloc balance");
}

/* ══════════════════════════ 8. RADIX 路径 ══════════════════════════ */

static void test_radix_path(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	u64 i;

	printf("[radix path]\n");
	/* 16385 块 → 8193 节点 → bits 14 → 128 片；不分配 64MB 假盘 */
	if (fx_init(&fx, 16385, 0) != 0) {
		CHECK(0, "fixture init");
		return;
	}
	CHECK(fx.c.mode == RAVENFS_MDATA_MODE_RADIX, "RADIX mode");
	CHECK(fx.c.n_slices == 128, "128 shards");
	CHECK(fx.c.tree_levels == 1, "one tree level");
	CHECK(ravenfs_mdata_total_blocks(&fx.c) == 16385,
	      "radix total_blocks audit");
	CHECK(ravenfs_mdata_shard_count(&fx.c) == 128,
	      "radix shard_count audit");

	/* 不同分片的节点（key 0..8192；shard = key >> 7） */
	for (i = 0; i < 16; i++) {
		u64 node_idx = i * 512;	/* 跨 4 个分片 */
		u64 blk = node_idx * 2;

		if (blk >= fx.c.total_blocks)
			break;
		s = ravenfs_mdata_get(&fx.c, blk);
		CHECK(!RFS_IS_ERR(s), "radix get");
		if (RFS_IS_ERR(s))
			continue;
		CHECK(row_state(s->row) == RAVENFS_MROW_READY, "radix READY");
		CHECK(((u8 *)ravenfs_msession_node(s))[0] ==
		      disk_pattern(blk, 0), "radix loaded content");
		ravenfs_mdata_put(s);
	}
	CHECK(fx.pool.live == 16, "16 resident rows");
	CHECK(fx.mem.live > 1, "radix tables allocated");

	ravenfs_mdata_flush_all(&fx.c);
	CHECK(fx.pool.live == 0, "radix flush");
	fx_fini(&fx);
	fx_check_balanced(&fx, "radix alloc balance");
}

/* bits=18 → tree_levels=2：验证 ensure_path 内部表 + 叶表、跨 chunk 定位 */
static void test_radix_deep(void)
{
	struct fixture fx;
	static const u64 nodes[] = { 0, 63, 4096, 8191, 100000 };
	u32 i;

	printf("[radix deep tree]\n");
	if (fx_init(&fx, 524288, 0) != 0) {	/* n_nodes=262144 → bits=18 */
		CHECK(0, "fixture init");
		return;
	}
	CHECK(fx.c.mode == RAVENFS_MDATA_MODE_RADIX, "deep RADIX");
	CHECK(fx.c.bits_len == 18, "deep bits=18");
	CHECK(fx.c.n_slices == 128, "deep 128 shards");
	CHECK(fx.c.tree_levels == 2, "deep tree_levels=2");
	CHECK(fx.c.leaf_width == 64, "deep leaf_width=64");

	for (i = 0; i < RFS_ARRAY_SIZE(nodes); i++) {
		u64 node_idx = nodes[i];
		struct ravenfs_msession *s =
			ravenfs_mdata_get(&fx.c, node_idx * 2);

		CHECK(!RFS_IS_ERR(s), "deep get");
		if (RFS_IS_ERR(s))
			continue;
		CHECK(row_state(s->row) == RAVENFS_MROW_READY, "deep READY");
		CHECK(((u8 *)ravenfs_msession_node(s))[0] ==
		      disk_pattern(node_idx * 2, 0), "deep content");
		ravenfs_mdata_put(s);
	}
	CHECK(fx.pool.live == RFS_ARRAY_SIZE(nodes), "deep all resident");

	ravenfs_mdata_flush_all(&fx.c);
	CHECK(fx.pool.live == 0, "deep flush");
	fx_fini(&fx);
	fx_check_balanced(&fx, "deep alloc balance");
}

/* ══════════════════════════ 8b. discard_all（ro 卸载语义）══════════════════════════ */

static void test_discard_all(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	struct ravenfs_mrow_simple *row;
	u64 writes;

	printf("[discard_all]\n");
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "fixture init");
		return;
	}

	s = ravenfs_mdata_get(&fx.c, 400);
	CHECK(!RFS_IS_ERR(s), "discard get");
	if (RFS_IS_ERR(s)) {
		fx_fini(&fx);
		fx_check_balanced(&fx, "discard balance");
		return;
	}
	row = s->row;
	memset(ravenfs_msession_node(s), 0xD3, RAVENFS_MDATA_NODE_BYTES);
	RAVENFS_MDATA_DIRTY(s);
	ravenfs_mdata_put(s);

	writes = fx.disk.writes;
	ravenfs_mdata_discard_all(&fx.c);
	CHECK(fx.disk.writes == writes, "discard does not write back");
	CHECK(row_state(row) == RAVENFS_MROW_IDLE, "discard -> IDLE");
	CHECK(row->data == NULL, "discard frees data");
	CHECK(fx.pool.live == 0, "discard frees all pool data");

	ravenfs_mdata_discard_all(&fx.c);	/* 幂等 */
	ravenfs_mdata_discard_all(NULL);	/* NULL 容忍 */
	CHECK(1, "discard idempotent / NULL tolerated");

	fx_fini(&fx);
	fx_check_balanced(&fx, "discard balance");
}

/* ══════════════════════════ 9. 强淘汰 ══════════════════════════ */

static void test_strong_reap_pressure(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	struct ravenfs_mrow_simple *clean_row, *dirty_row, *held_row;
	struct ravenfs_mdata_squeeze_res res;
	int rc;

	printf("[strong reap] pressure\n");
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "fixture init");
		return;
	}

	/* 三行：干净 / 脏 / 被会话持有（ref>0） */
	s = ravenfs_mdata_get(&fx.c, 100);
	CHECK(!RFS_IS_ERR(s), "pressure get clean");
	if (RFS_IS_ERR(s))
		goto out;
	clean_row = s->row;
	ravenfs_mdata_put(s);

	s = ravenfs_mdata_get(&fx.c, 102);
	CHECK(!RFS_IS_ERR(s), "pressure get dirty");
	if (RFS_IS_ERR(s))
		goto out;
	dirty_row = s->row;
	memset(ravenfs_msession_node(s), 0xB1, RAVENFS_MDATA_NODE_BYTES);
	RAVENFS_MDATA_DIRTY(s);
	ravenfs_mdata_put(s);

	s = ravenfs_mdata_get(&fx.c, 104);
	CHECK(!RFS_IS_ERR(s), "pressure get held");
	if (RFS_IS_ERR(s))
		goto out;
	held_row = s->row;	/* 故意不 put：ref=1 */
	CHECK(fx.pool.live == 3, "three resident rows");

	/* PRESSURE：无视 cutoff（传一个很新的 cutoff 也必须全收）*/
	memset(&res, 0xFF, sizeof(res));	/* 验证入口清零 */
	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, RFS_NOW_NS(), &res);
	CHECK(rc == 0, "pressure rc=0");
	CHECK(res.rows_reaped == 2, "pressure reaped 2 (clean+dirty)");
	CHECK(res.write_errors == 0, "pressure no write errors");
	CHECK(res.tables_freed == 0, "CTRL_ARRAY no tables freed");
	CHECK(res.table_bytes_freed == 0, "CTRL_ARRAY no table bytes");
	CHECK(fx.pool.live == 1, "held row still resident");
	CHECK(row_state(clean_row) == RAVENFS_MROW_IDLE, "clean -> IDLE");
	CHECK(clean_row->data == NULL, "clean data freed");
	CHECK(row_state(dirty_row) == RAVENFS_MROW_IDLE, "dirty -> IDLE");
	CHECK(dirty_row->data == NULL, "dirty data freed");
	CHECK(fx.disk.mem[102 * RAVENFS_MDATA_BLK_BYTES] == 0xB1,
	      "dirty row written back before free");
	CHECK(row_state(held_row) == RAVENFS_MROW_READY, "held stays READY");
	CHECK(row_ref(held_row) == 1, "held ref still 1");
	CHECK(held_row->data != NULL, "held data retained");
	CHECK(fx.c.shards[0].root != NULL, "CTRL_ARRAY root retained");

	/* out==NULL 容忍；仍不动 ref>0 行 */
	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, 0, NULL);
	CHECK(rc == 0, "out==NULL tolerated");
	CHECK(fx.pool.live == 1, "out==NULL leaves held row");

	ravenfs_mdata_put(s);	/* ref=1 -> 0 */
	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, 1, &res);
	CHECK(rc == 0 && res.rows_reaped == 1, "released row reaped");
	CHECK(fx.pool.live == 0, "all reaped");

	/* 非法 gear / NULL self */
	memset(&res, 0xFF, sizeof(res));
	CHECK(ravenfs_mdata_reap_expired_strong(&fx.c, 2, 0, &res) == -EINVAL,
	      "invalid gear -> -EINVAL");
	CHECK(res.rows_reaped == 0 && res.tables_freed == 0 &&
	      res.table_bytes_freed == 0 && res.write_errors == 0,
	      "invalid gear out zeroed");
	CHECK(ravenfs_mdata_reap_expired_strong(NULL,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, 0, &res) == -EINVAL,
	      "NULL self -> -EINVAL");

out:
	fx_fini(&fx);
	fx_check_balanced(&fx, "strong pressure balance");
}

static void test_strong_reap_lowload(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	struct ravenfs_mrow_simple *cold_row, *hot_row, *cold_dirty;
	struct ravenfs_mdata_squeeze_res res;
	int rc;

	printf("[strong reap] lowload\n");
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "fixture init");
		return;
	}

	/* 冷干净 / 热干净 / 冷脏 */
	s = ravenfs_mdata_get(&fx.c, 200);
	CHECK(!RFS_IS_ERR(s), "lowload get cold");
	if (RFS_IS_ERR(s))
		goto out;
	cold_row = s->row;
	RFS_ATOMIC64_SET(&cold_row->acc_stamp, 100);
	ravenfs_mdata_put(s);

	s = ravenfs_mdata_get(&fx.c, 202);
	CHECK(!RFS_IS_ERR(s), "lowload get hot");
	if (RFS_IS_ERR(s))
		goto out;
	hot_row = s->row;
	RFS_ATOMIC64_SET(&hot_row->acc_stamp, (s64)~0ULL);
	ravenfs_mdata_put(s);

	s = ravenfs_mdata_get(&fx.c, 204);
	CHECK(!RFS_IS_ERR(s), "lowload get cold dirty");
	if (RFS_IS_ERR(s))
		goto out;
	cold_dirty = s->row;
	memset(ravenfs_msession_node(s), 0xC4, RAVENFS_MDATA_NODE_BYTES);
	RAVENFS_MDATA_DIRTY(s);
	RFS_ATOMIC64_SET(&cold_dirty->acc_stamp, 50);
	ravenfs_mdata_put(s);
	CHECK(fx.pool.live == 3, "lowload three rows");

	/* cutoff=1000：只收 acc_stamp < 1000 的冷行（含脏冷行）*/
	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_LOWLOAD, 1000, &res);
	CHECK(rc == 0, "lowload rc=0");
	CHECK(res.rows_reaped == 2, "lowload reaped 2 cold rows");
	CHECK(res.write_errors == 0, "lowload no write errors");
	CHECK(fx.pool.live == 1, "hot row retained");
	CHECK(row_state(cold_row) == RAVENFS_MROW_IDLE, "cold clean reaped");
	CHECK(row_state(cold_dirty) == RAVENFS_MROW_IDLE,
	      "cold dirty reaped");
	CHECK(fx.disk.mem[204 * RAVENFS_MDATA_BLK_BYTES] == 0xC4,
	      "cold dirty written before free");
	CHECK(row_state(hot_row) == RAVENFS_MROW_READY, "hot stays READY");
	CHECK(row_ref(hot_row) == 0, "hot ref=0");
	CHECK(hot_row->data != NULL, "hot data retained");

	/* ttl_cutoff_ns==0 = 无 TTL 过滤（文档语义）*/
	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_LOWLOAD, 0, &res);
	CHECK(rc == 0 && res.rows_reaped == 1, "lowload cutoff=0 reaps hot");
	CHECK(fx.pool.live == 0, "all lowload rows gone");

	/* PRESSURE 仍无视 cutoff（acc_stamp 极大也收）*/
	s = ravenfs_mdata_get(&fx.c, 206);
	CHECK(!RFS_IS_ERR(s), "pressure-ignore get");
	if (!RFS_IS_ERR(s)) {
		RFS_ATOMIC64_SET(&s->row->acc_stamp, (s64)~0ULL);
		ravenfs_mdata_put(s);
		rc = ravenfs_mdata_reap_expired_strong(&fx.c,
				RAVENFS_MDATA_SQUEEZE_PRESSURE, 1, &res);
		CHECK(rc == 0 && res.rows_reaped == 1,
		      "pressure ignores cutoff");
		CHECK(fx.pool.live == 0, "pressure-ignore freed");
	}

out:
	fx_fini(&fx);
	fx_check_balanced(&fx, "strong lowload balance");
}

static void test_strong_table_prune_radix(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	struct ravenfs_mdata_squeeze_res res;
	u32 sh0, sh4;
	int rc;

	printf("[strong reap] radix table prune\n");
	/* 16385 块 → 8193 节点 → bits 14 → 128 片、单层叶表 */
	if (fx_init(&fx, 16385, 0) != 0) {
		CHECK(0, "fixture init");
		return;
	}
	CHECK(fx.c.mode == RAVENFS_MDATA_MODE_RADIX, "prune RADIX mode");
	CHECK(fx.c.tree_levels == 1, "prune one tree level");
	sh0 = (u32)(0 >> fx.c.shard_bits);
	sh4 = (u32)(512 >> fx.c.shard_bits);
	CHECK(sh0 != sh4, "two distinct shards");

	s = ravenfs_mdata_get(&fx.c, 0 * 2);
	CHECK(!RFS_IS_ERR(s), "prune get shard0");
	if (!RFS_IS_ERR(s))
		ravenfs_mdata_put(s);
	s = ravenfs_mdata_get(&fx.c, 512 * 2);
	CHECK(!RFS_IS_ERR(s), "prune get shard4");
	if (!RFS_IS_ERR(s))
		ravenfs_mdata_put(s);
	CHECK(fx.pool.live == 2, "two resident rows");
	CHECK(fx.c.shards[sh0].root != NULL, "shard0 root built");
	CHECK(fx.c.shards[sh4].root != NULL, "shard4 root built");

	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, 0, &res);
	CHECK(rc == 0, "prune rc=0");
	CHECK(res.rows_reaped == 2, "prune reaped 2 rows");
	CHECK(res.write_errors == 0, "prune no write errors");
	CHECK(res.tables_freed >= 2, "at least two empty leaf tables freed");
	CHECK(res.table_bytes_freed > 0, "table bytes accounted");
	CHECK(fx.c.shards[sh0].root == NULL, "shard0 root released");
	CHECK(fx.c.shards[sh4].root == NULL, "shard4 root released");
	CHECK(fx.pool.live == 0, "row data all freed");

	/* H6：根被摘后 get 重建路径并读到正确内容 */
	s = ravenfs_mdata_get(&fx.c, 0 * 2);
	CHECK(!RFS_IS_ERR(s), "prune get after root free (shard0)");
	if (!RFS_IS_ERR(s)) {
		CHECK(((u8 *)ravenfs_msession_node(s))[0] ==
		      disk_pattern(0, 0), "rebuilt shard0 content");
		ravenfs_mdata_put(s);
	}
	s = ravenfs_mdata_get(&fx.c, 512 * 2);
	CHECK(!RFS_IS_ERR(s), "prune get after root free (shard4)");
	if (!RFS_IS_ERR(s)) {
		CHECK(((u8 *)ravenfs_msession_node(s))[0] ==
		      disk_pattern(512 * 2, 0), "rebuilt shard4 content");
		ravenfs_mdata_put(s);
	}
	CHECK(fx.c.shards[sh0].root != NULL, "shard0 root rebuilt");
	CHECK(fx.c.shards[sh4].root != NULL, "shard4 root rebuilt");

	/* 再强淘汰一次 → 表再次摘除；随后 fini 不得二次释放（ASan）*/
	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, 0, &res);
	CHECK(rc == 0 && res.rows_reaped == 2, "second prune reaped");
	CHECK(fx.c.shards[sh0].root == NULL, "shard0 root released again");
	CHECK(fx.c.shards[sh4].root == NULL, "shard4 root released again");

	fx_fini(&fx);
	fx_check_balanced(&fx, "strong radix prune balance");
}

static void test_strong_table_prune_deep(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	struct ravenfs_mdata_squeeze_res res;
	u32 sh;
	int rc;

	printf("[strong reap] deep table prune\n");
	/* 524288 块 → 262144 节点 → bits 18 → tree_levels=2 */
	if (fx_init(&fx, 524288, 0) != 0) {
		CHECK(0, "fixture init");
		return;
	}
	CHECK(fx.c.tree_levels == 2, "deep prune tree_levels=2");
	sh = (u32)(0 >> fx.c.shard_bits);

	/* 同一 shard、不同叶 chunk：node 0 与 node 64（chunk 0/1）*/
	s = ravenfs_mdata_get(&fx.c, 0 * 2);
	CHECK(!RFS_IS_ERR(s), "deep prune get node0");
	if (!RFS_IS_ERR(s))
		ravenfs_mdata_put(s);
	s = ravenfs_mdata_get(&fx.c, 64 * 2);
	CHECK(!RFS_IS_ERR(s), "deep prune get node64");
	if (!RFS_IS_ERR(s))
		ravenfs_mdata_put(s);
	CHECK(fx.pool.live == 2, "deep two resident rows");
	CHECK(fx.c.shards[sh].root != NULL, "deep root built");

	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, 0, &res);
	CHECK(rc == 0, "deep prune rc=0");
	CHECK(res.rows_reaped == 2, "deep prune reaped 2");
	CHECK(res.tables_freed >= 3, "two leaves + internal table freed");
	CHECK(res.table_bytes_freed > 0, "deep table bytes accounted");
	CHECK(fx.c.shards[sh].root == NULL, "deep root released");
	CHECK(fx.pool.live == 0, "deep data freed");

	/* 重建：get 一个此前未建过的叶，验证内部表也被补建 */
	s = ravenfs_mdata_get(&fx.c, 64 * 2);
	CHECK(!RFS_IS_ERR(s), "deep get after root free");
	if (!RFS_IS_ERR(s)) {
		CHECK(((u8 *)ravenfs_msession_node(s))[0] ==
		      disk_pattern(64 * 2, 0), "deep rebuilt content");
		ravenfs_mdata_put(s);
	}
	CHECK(fx.c.shards[sh].root != NULL, "deep root rebuilt");

	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, 0, &res);
	CHECK(rc == 0 && res.rows_reaped == 1, "deep second prune reaped");
	CHECK(res.tables_freed >= 2, "deep rebuilt leaf+root freed");
	CHECK(fx.c.shards[sh].root == NULL, "deep root released again");

	fx_fini(&fx);
	fx_check_balanced(&fx, "strong deep prune balance");
}

static void test_strong_write_fail(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	struct ravenfs_mrow_simple *row;
	struct ravenfs_mdata_squeeze_res res;
	int rc;

	printf("[strong reap] write failure\n");
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "fixture init");
		return;
	}

	s = ravenfs_mdata_get(&fx.c, 300);
	CHECK(!RFS_IS_ERR(s), "wf get dirty");
	if (RFS_IS_ERR(s))
		goto out;
	row = s->row;
	memset(ravenfs_msession_node(s), 0x5A, RAVENFS_MDATA_NODE_BYTES);
	RAVENFS_MDATA_DIRTY(s);
	ravenfs_mdata_put(s);

	/* 注入一次写失败 */
	fx.disk.write_fail_at = (long)fx.disk.write_calls + 1;
	memset(&res, 0, sizeof(res));
	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, 0, &res);
	CHECK(rc == -EIO, "write fail -> -EIO");
	CHECK(res.write_errors == 1, "one write error accounted");
	CHECK(res.rows_reaped == 0, "failed row not counted reaped");
	CHECK(row_state(row) == RAVENFS_MROW_READY, "row restored READY");
	CHECK(row_dirty(row), "row keeps DIRTY");
	CHECK(row->data != NULL, "row keeps data");
	CHECK(row_ref(row) == 0, "row ref stays 0");
	CHECK(fx.pool.live == 1, "data still live after failure");

	/* 解除失败：再次 strong 应成功回收 */
	fx.disk.write_fail_at = 0;
	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, 0, &res);
	CHECK(rc == 0, "retry after write fail rc=0");
	CHECK(res.rows_reaped == 1, "retry reaped row");
	CHECK(res.write_errors == 0, "retry no write errors");
	CHECK(row_state(row) == RAVENFS_MROW_IDLE, "retry -> IDLE");
	CHECK(fx.pool.live == 0, "retry frees data");
	CHECK(fx.disk.mem[300 * RAVENFS_MDATA_BLK_BYTES] == 0x5A,
	      "retry write content landed");

out:
	fx_fini(&fx);
	fx_check_balanced(&fx, "strong write fail balance");
}

/* ══════════════════════════ 10. 故障注入 ══════════════════════════ */

static void test_fault_injection(void)
{
	struct fixture fx;
	struct ravenfs_msession *s;
	int rc;

	printf("[fault injection]\n");

	/* mem.alloc #1 失败：CTRL_ARRAY init */
	memset(&fx, 0, sizeof(fx));
	fx.mem.fail_at = 1;
	fx.cfg.total_blocks = 1024;
	fx.cfg.mem.alloc = t_mem_alloc;
	fx.cfg.mem.free = t_mem_free;
	fx.cfg.mem.ctx = &fx.mem;
	fx.cfg.pool.alloc = t_pool_alloc;
	fx.cfg.pool.free = t_pool_free;
	fx.cfg.pool.ctx = &fx.pool;
	fx.cfg.io.read_block = t_read_block;
	fx.cfg.io.write_block = t_write_block;
	fx.cfg.io.ctx = &fx.disk;
	fx.disk.total_blocks = 1024;
	rc = ravenfs_mdata_cache_init(&fx.c, &fx.cfg);
	CHECK(rc == -ENOMEM, "init mem fail #1 -> -ENOMEM");
	CHECK(fx.c.shards == NULL, "init fail leaves shards NULL");
	ravenfs_mdata_cache_fini(&fx.c);	/* 幂等安全 */
	CHECK(fx.mem.live == 0, "init fail #1 no leak");

	/* mem.alloc #2 失败：CTRL_ARRAY 叶表分配失败 → 回滚 shards */
	memset(&fx, 0, sizeof(fx));
	fx.mem.fail_at = 2;
	fx.cfg.total_blocks = 1024;
	fx.cfg.mem.alloc = t_mem_alloc;
	fx.cfg.mem.free = t_mem_free;
	fx.cfg.mem.ctx = &fx.mem;
	fx.cfg.pool.alloc = t_pool_alloc;
	fx.cfg.pool.free = t_pool_free;
	fx.cfg.pool.ctx = &fx.pool;
	fx.cfg.io.read_block = t_read_block;
	fx.cfg.io.write_block = t_write_block;
	fx.cfg.io.ctx = &fx.disk;
	fx.disk.total_blocks = 1024;
	rc = ravenfs_mdata_cache_init(&fx.c, &fx.cfg);
	CHECK(rc == -ENOMEM, "init mem fail #2 -> -ENOMEM");
	CHECK(fx.c.shards == NULL, "init fail #2 rolls back shards");
	CHECK(fx.mem.live == 0, "init fail #2 no leak");

	/* bits > 52：不分配即拒 */
	memset(&fx, 0, sizeof(fx));
	fx.cfg.total_blocks = 1ULL << 54;
	fx.cfg.mem.alloc = t_mem_alloc;
	fx.cfg.mem.free = t_mem_free;
	fx.cfg.mem.ctx = &fx.mem;
	fx.cfg.pool.alloc = t_pool_alloc;
	fx.cfg.pool.free = t_pool_free;
	fx.cfg.pool.ctx = &fx.pool;
	fx.cfg.io.read_block = t_read_block;
	fx.cfg.io.write_block = t_write_block;
	fx.cfg.io.ctx = &fx.disk;
	fx.disk.total_blocks = 1ULL << 54;
	rc = ravenfs_mdata_cache_init(&fx.c, &fx.cfg);
	CHECK(rc == -EINVAL, "bits>52 -> -EINVAL");
	CHECK(fx.mem.calls == 0, "no alloc on bits>52");

	/* pool.alloc 失败：get 返回 -ENOMEM，行回 IDLE，无泄漏 */
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "fixture init");
		return;
	}
	{
		u64 base_mem = fx.mem.live;
		u64 base_pool = fx.pool.live;

		fx.pool.fail_at = 1;
		s = ravenfs_mdata_get(&fx.c, 70);
		CHECK(RFS_IS_ERR(s) && RFS_PTR_ERR(s) == -ENOMEM,
		      "pool alloc fail -> -ENOMEM");
		CHECK(fx.pool.live == base_pool, "pool fail no live buffer");
		CHECK(fx.mem.live == base_mem, "session freed on pool fail");
		CHECK(fx.disk.reads == 0, "no read after pool fail");
	}

	/* 重试成功（证明行已回 IDLE 可重新装载） */
	fx.pool.fail_at = 0;
	s = ravenfs_mdata_get(&fx.c, 70);
	CHECK(!RFS_IS_ERR(s), "retry after pool fail");
	if (!RFS_IS_ERR(s))
		ravenfs_mdata_put(s);

	/* read_block 失败：get 返回 -EIO，缓冲归还，行回 IDLE */
	{
		u64 base_pool = fx.pool.live;

		fx.disk.read_fail_at = fx.disk.read_calls + 1;
		s = ravenfs_mdata_get(&fx.c, 72);
		CHECK(RFS_IS_ERR(s) && RFS_PTR_ERR(s) == -EIO,
		      "read fail -> -EIO");
		CHECK(fx.pool.live == base_pool,
		      "read fail returns pool buffer");
		fx.disk.read_fail_at = 0;
	}
	s = ravenfs_mdata_get(&fx.c, 72);
	CHECK(!RFS_IS_ERR(s), "retry after read fail");
	if (!RFS_IS_ERR(s))
		ravenfs_mdata_put(s);
	fx_fini(&fx);
	fx_check_balanced(&fx, "fault alloc balance");

	/* session mem.alloc 失败 */
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "fixture init 2");
		return;
	}
	fx.mem.fail_at = fx.mem.calls + 1;
	s = ravenfs_mdata_get(&fx.c, 74);
	CHECK(RFS_IS_ERR(s) && RFS_PTR_ERR(s) == -ENOMEM,
	      "session alloc fail -> -ENOMEM");
	fx.mem.fail_at = 0;
	CHECK(fx.mem.live == 2, "only shards+leaf live");
	fx_fini(&fx);
	fx_check_balanced(&fx, "session fail alloc balance");

	/* RADIX ensure_path 表分配失败：session alloc 之后的下一次 mem.alloc 失败 */
	if (fx_init(&fx, 32768, 0) != 0) {
		CHECK(0, "radix fixture init");
		return;
	}
	fx.mem.fail_at = fx.mem.calls + 2;
	s = ravenfs_mdata_get(&fx.c, 0);
	CHECK(RFS_IS_ERR(s) && RFS_PTR_ERR(s) == -ENOMEM,
	      "ensure_path fail -> -ENOMEM");
	fx.mem.fail_at = 0;
	s = ravenfs_mdata_get(&fx.c, 0);
	CHECK(!RFS_IS_ERR(s), "radix retry after table fail");
	if (!RFS_IS_ERR(s))
		ravenfs_mdata_put(s);
	fx_fini(&fx);
	fx_check_balanced(&fx, "radix table fail balance");
}

/* ══════════════════════════ 11. 多线程烟测 ══════════════════════════ */

struct mt_arg {
	struct ravenfs_mdata_cache	*c;
	u64				n_nodes;
	int				tid;
	int				stop;
	long				errors;
	u64				iters;
};

static int mt_stop(struct mt_arg *a)
{
	return __atomic_load_n(&a->stop, __ATOMIC_RELAXED);
}

static void *mt_worker(void *p)
{
	struct mt_arg *a = p;
	u64 i;

	for (i = 0; i < a->iters && !mt_stop(a); i++) {
		/* 一半迭代打同一个热 key 集（0..7），强制多会话/读写竞争 */
		u64 key = (i & 1) ? ((u64)a->tid * 131 + i * 17) % a->n_nodes
				  : (u64)(i % 8);
		u64 blk = key * 2;
		struct ravenfs_msession *s =
			ravenfs_mdata_get(a->c, blk);

		if (RFS_IS_ERR(s)) {
			a->errors++;
			continue;
		}
		{
			u8 *d = ravenfs_msession_node(s);

			__atomic_store_n(&d[0], (u8)(a->tid ^ (int)i),
					 __ATOMIC_RELAXED);
			if ((i % 3) == 0)
				RAVENFS_MDATA_DIRTY(s);
			if ((i % 11) == 0) {
				RAVENFS_MDATA_DIRTY(s);
				if (ravenfs_mdata_wb_node(a->c, blk) != 0)
					a->errors++;
			}
		}
		ravenfs_mdata_put(s);

		/* 同 key 二次 get/put（嵌套/重入） */
		if ((i % 7) == 0) {
			struct ravenfs_msession *t =
				ravenfs_mdata_get(a->c, blk);

			if (RFS_IS_ERR(t))
				a->errors++;
			else
				ravenfs_mdata_put(t);
		}
		/* 跨 key drop */
		if ((i % 13) == 0) {
			u64 k2 = (key + 7) % a->n_nodes;

			ravenfs_mdata_drop(a->c, k2 * 2);
		}
	}
	return NULL;
}

static void *mt_reaper(void *p)
{
	struct mt_arg *a = p;

	while (!mt_stop(a)) {
		/* 大 cutoff：积极回收 clean && ref0 行，与 get 竞争 */
		ravenfs_mdata_reap_expired(a->c, RFS_NOW_NS() +
					   RAVENFS_MDATA_NODE_BYTES);
		RFS_CPU_RELAX();
	}
	return NULL;
}

static void *mt_dropper(void *p)
{
	struct mt_arg *a = p;
	u64 i;

	for (i = 0; i < a->iters && !mt_stop(a); i++) {
		u64 key = ((u64)a->tid * 977 + i * 31) % a->n_nodes;

		ravenfs_mdata_drop(a->c, key * 2);
		RFS_CPU_RELAX();
	}
	return NULL;
}

/* 强淘汰与 get/put/drop 并发：PRESSURE/LOWLOAD 交替，验证锁序/CAS 无 UAF。*/
static void *mt_strong_reaper(void *p)
{
	struct mt_arg *a = p;
	u32 n = 0;

	while (!mt_stop(a)) {
		struct ravenfs_mdata_squeeze_res res;
		u32 gear = (n++ & 1) ? RAVENFS_MDATA_SQUEEZE_LOWLOAD
				     : RAVENFS_MDATA_SQUEEZE_PRESSURE;
		u64 cutoff = (gear == RAVENFS_MDATA_SQUEEZE_LOWLOAD)
			   ? RFS_NOW_NS() - 1000000000ULL : 0;
		int rc = ravenfs_mdata_reap_expired_strong(a->c, gear, cutoff,
							   &res);

		if (rc != 0)
			a->errors++;
		RFS_CPU_RELAX();
	}
	return NULL;
}

#define MT_THREADS	8
#define MT_EXTRA	3	/* reaper + dropper + strong reaper */
#define MT_TOTAL	(MT_THREADS + MT_EXTRA)

static void test_multithread(void)
{
	struct fixture fx;
	struct mt_arg args[MT_TOTAL];
	pthread_t th[MT_TOTAL];
	int created[MT_TOTAL];
	int i, ncreated = 0, ok = 1;

	printf("[multithread]\n");
	/* 16385 块 → RADIX 128 片；无真实假盘（大几何） */
	if (fx_init(&fx, 16385, 0) != 0) {
		CHECK(0, "fixture init");
		return;
	}

	memset(args, 0, sizeof(args));
	memset(created, 0, sizeof(created));
	for (i = 0; i < MT_TOTAL; i++) {
		args[i].c = &fx.c;
		args[i].n_nodes = fx.c.n_nodes;
		args[i].tid = i;
		args[i].iters = 4000;
		args[i].stop = 0;
	}
	for (i = 0; i < MT_THREADS; i++) {
		if (pthread_create(&th[i], NULL, mt_worker, &args[i]) != 0) {
			ok = 0;
			break;
		}
		created[i] = 1;
		ncreated++;
	}
	if (ok && pthread_create(&th[MT_THREADS], NULL, mt_reaper,
				 &args[MT_THREADS]) == 0) {
		created[MT_THREADS] = 1;
		ncreated++;
	} else {
		ok = 0;
	}
	if (ok && pthread_create(&th[MT_THREADS + 1], NULL, mt_dropper,
				 &args[MT_THREADS + 1]) == 0) {
		created[MT_THREADS + 1] = 1;
		ncreated++;
	} else {
		ok = 0;
	}
	if (ok && pthread_create(&th[MT_THREADS + 2], NULL, mt_strong_reaper,
				 &args[MT_THREADS + 2]) == 0) {
		created[MT_THREADS + 2] = 1;
		ncreated++;
	} else {
		ok = 0;
	}
	CHECK(ok, "pthread_create all");
	if (!ok) {
		/* 停掉已建线程再收尾，避免 fini 与在飞线程竞争 */
		for (i = 0; i < MT_TOTAL; i++)
			__atomic_store_n(&args[i].stop, 1, __ATOMIC_RELAXED);
		for (i = 0; i < MT_TOTAL; i++)
			if (created[i])
				pthread_join(th[i], NULL);
		fx_fini(&fx);
		return;
	}

	for (i = 0; i < MT_THREADS; i++)
		pthread_join(th[i], NULL);

	/* 通知 reaper / dropper / strong reaper 停止并等待 */
	for (i = MT_THREADS; i < MT_TOTAL; i++)
		__atomic_store_n(&args[i].stop, 1, __ATOMIC_RELAXED);
	for (i = MT_THREADS; i < MT_TOTAL; i++)
		pthread_join(th[i], NULL);
	(void)ncreated;

	for (i = 0; i < MT_TOTAL; i++)
		CHECK(args[i].errors == 0, "MT worker errors");

	ravenfs_mdata_flush_all(&fx.c);
	CHECK(fx.pool.live == 0, "MT flush frees all");
	fx_fini(&fx);
	fx_check_balanced(&fx, "MT alloc balance");
}

/* ══════════════════════════ 11. 计数器钩子（⇄ mdp_stat 全量对位）══════════════════════════ */

static void test_stat_hook(void)
{
	struct fixture fx;
	struct ravenfs_mdata_squeeze_res res;
	u8 buf[RAVENFS_MDATA_NODE_BYTES];
	u64 blk = 12;			/* node 6 */
	int rc;

	printf("[stat hook]\n");

	/* ── CTRL_ARRAY：装配 / 会话 / 脏+flush / gauge ── */
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "stat fixture init");
		return;
	}
	CHECK(t_stat_get(&fx, RAVENFS_MD_INIT_OK) == 1, "stat init_ok");
	CHECK(t_stat_get(&fx, RAVENFS_MD_INIT_FAIL) == 0, "stat init_fail=0");
	CHECK(t_stat_get(&fx, RAVENFS_MD_INIT_CTRL_ARRAY) == 1,
	      "stat init_ctrl_array");
	CHECK(t_stat_get(&fx, RAVENFS_MD_SLICES) == 1, "stat slices");
	CHECK(t_stat_get(&fx, RAVENFS_MD_TABLE_ALLOC_LEAF) == 1,
	      "stat ctrl-array leaf alloc");
	CHECK(t_stat_get(&fx, RAVENFS_MD_TABLE_ADD_BYTES) > 0,
	      "stat table add bytes");

	rc = ravenfs_mdata_read(&fx.c, blk, buf);
	CHECK(rc == 0, "stat read");
	CHECK(t_stat_get(&fx, RAVENFS_MD_GET_MISS) == 1, "stat miss");
	CHECK(t_stat_get(&fx, RAVENFS_MD_GET_HIT) == 0, "stat hit=0");
	CHECK(t_stat_get(&fx, RAVENFS_MD_NODE_LOAD) == 1, "stat node_load");
	CHECK(t_stat_get(&fx, RAVENFS_MD_PUT) == 1, "stat put");
	CHECK(t_stat_get(&fx, RAVENFS_MD_RESIDENT_ADD_BYTES) ==
	      RAVENFS_MDATA_NODE_BYTES, "stat resident add");
	CHECK(t_stat_get(&fx, RAVENFS_MD_RESIDENT_DEL_BYTES) == 0,
	      "stat resident del=0");

	rc = ravenfs_mdata_read(&fx.c, blk, buf);
	CHECK(rc == 0, "stat read hit");
	CHECK(t_stat_get(&fx, RAVENFS_MD_GET_HIT) == 1, "stat hit");

	{
		RAVENFS_MDATA_SESSION(ms, &fx.c, blk);

		CHECK(RAVENFS_MDATA_SESSION_VALID(ms), "stat session");
		RAVENFS_MDATA_SESSION_DIRTY(ms);
	}
	CHECK(t_stat_get(&fx, RAVENFS_MD_DIRTY) == 1, "stat dirty");

	ravenfs_mdata_flush_all(&fx.c);
	CHECK(t_stat_get(&fx, RAVENFS_MD_NODE_WB) == 1, "stat node_wb");
	CHECK(t_stat_get(&fx, RAVENFS_MD_FLUSH_WB) == 1, "stat flush_wb");
	CHECK(t_stat_get(&fx, RAVENFS_MD_FLUSH_DROP) == 1, "stat flush_drop");
	CHECK(t_stat_get(&fx, RAVENFS_MD_RESIDENT_DEL_BYTES) ==
	      RAVENFS_MDATA_NODE_BYTES, "stat resident del");
	fx_fini(&fx);
	fx_check_balanced(&fx, "stat ctrl-array balance");

	/* ── 强淘汰：REAP_STRONG ── */
	if (fx_init(&fx, 1024, 1) != 0) {
		CHECK(0, "stat strong fixture init");
		return;
	}
	rc = ravenfs_mdata_read(&fx.c, blk, buf);
	CHECK(rc == 0, "stat strong read");
	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, 0, &res);
	CHECK(rc == 0 && res.rows_reaped == 1, "stat strong reap");
	CHECK(t_stat_get(&fx, RAVENFS_MD_REAP_STRONG) == 1,
	      "stat reap_strong");
	CHECK(t_stat_get(&fx, RAVENFS_MD_RESIDENT_DEL_BYTES) ==
	      RAVENFS_MDATA_NODE_BYTES, "stat strong resident del");
	fx_fini(&fx);
	fx_check_balanced(&fx, "stat strong balance");

	/* ── RADIX：内部表/叶表分配 + 空表回收 ── */
	if (fx_init(&fx, 524288, 0) != 0) {	/* n_nodes=262144 → bits=18 */
		CHECK(0, "stat radix fixture init");
		return;
	}
	CHECK(t_stat_get(&fx, RAVENFS_MD_INIT_RADIX) == 1, "stat init_radix");
	rc = ravenfs_mdata_read(&fx.c, blk, buf);
	CHECK(rc == 0, "stat radix read");
	CHECK(t_stat_get(&fx, RAVENFS_MD_TABLE_ALLOC_INTERNAL) >= 1,
	      "stat table alloc internal");
	CHECK(t_stat_get(&fx, RAVENFS_MD_TABLE_ALLOC_LEAF) >= 1,
	      "stat table alloc leaf");
	rc = ravenfs_mdata_reap_expired_strong(&fx.c,
			RAVENFS_MDATA_SQUEEZE_PRESSURE, 0, &res);
	CHECK(rc == 0 && res.rows_reaped == 1, "stat radix strong reap");
	CHECK(t_stat_get(&fx, RAVENFS_MD_TABLE_FREE) >= 2,
	      "stat table free");
	CHECK(t_stat_get(&fx, RAVENFS_MD_TABLE_DEL_BYTES) > 0,
	      "stat table del bytes");
	fx_fini(&fx);
	fx_check_balanced(&fx, "stat radix balance");
}

/* ══════════════════════════ main ══════════════════════════ */

int main(void)
{
	printf("ravenfs mdata cache core tests\n");

	test_geometry();
	test_get_put_read();
	test_tail_node();
	test_dirty_wb();
	test_drop();
	test_reap();
	test_flush_all();
	test_discard_all();
	test_radix_path();
	test_radix_deep();
	test_strong_reap_pressure();
	test_strong_reap_lowload();
	test_strong_table_prune_radix();
	test_strong_table_prune_deep();
	test_strong_write_fail();
	test_fault_injection();
	test_multithread();
	test_stat_hook();

	printf("----\npass=%d fail=%d\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

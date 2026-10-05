// SPDX-License-Identifier: GPL-2.0
/*
 * test_name_data_freeranges.c —— fr 空闲区间表（v1.3）平台中立 C 模块宿主单测
 *
 * 直接编译 ../linux/name_data_freeranges.c（不链 god/ko 其它任何东西）。
 * 用假宿主实现 rfs_ndfr_ops：
 *   · 块存储 = FS_block → 8K 节点缓冲（扁 arena，按块号寻址）；
 *   · bump 分配器 = 每次发 2 个连续 8K 对齐 FS_block，返回偶数块号；
 *   · node_pin 返回 arena 内节点首址；node_alloc 清零新节点；
 *   · 记 live/allocs/frees，release 后须 live==0 且 allocs==frees。
 *
 * 覆盖（PASS/FAIL 计数；末行 === PASS n  FAIL 0 ===）：
 *   1. 空态 INACTIVE → 首插 inline
 *   2. inline ≤9 全内联；第 10 条隐式升格 tree；find_fit 命中
 *   3. best-fit：多 (base,len) → 最小 len≥need 且 base 正确
 *   4. take_fit：摘除；余段回填且仍可 find_fit；整取清空复位
 *   5. 叶分裂：340 条 → 内节点；DFS 跨叶 (len,base) 严格递增；根 meta 一致
 *   6. retire_empty_leaf：取空一叶 → 无空叶残留；根收缩；路由仍正确
 *   7. release：归还全部节点；ctl 复位；live==0
 *   8. >255 下标回归：根内节点孩子数 >256，仍正确路由
 */
#include "ravenfs_name_data_freeranges.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do { \
	if (cond) { \
		g_pass++; \
	} else { \
		g_fail++; \
		printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* ══════════════════════════ 假宿主 ══════════════════════════ */

#define FAKE_BLK_BYTES	4096u
#define FAKE_NODE_BLKS	2u
#define FAKE_NODE_BYTES	(FAKE_BLK_BYTES * FAKE_NODE_BLKS)	/* 8K */
#define FAKE_MAX_BLKS	8192u			/* 2048 节点 = 32MiB arena */

struct fake_host {
	u8	*arena;
	u8	*alive;				/* 每节点活跃位 */
	u64	 next_blk;			/* bump（恒偶）*/
	int	 live, allocs, frees;
	int	 pins, dirties;
};

static void fake_init(struct fake_host *h)
{
	memset(h, 0, sizeof(*h));
	h->arena = (u8 *)calloc(1, (size_t)FAKE_MAX_BLKS * FAKE_BLK_BYTES);
	h->alive = (u8 *)calloc(1, FAKE_MAX_BLKS / FAKE_NODE_BLKS);
	h->next_blk = 2;				/* 跳过块 0 */
}

static void fake_fini(struct fake_host *h)
{
	free(h->arena);
	free(h->alive);
}

static void *fake_pin(void *host, u64 blk, bool *err)
{
	struct fake_host *h = (struct fake_host *)host;

	if (err)
		*err = false;
	if (blk % FAKE_NODE_BLKS != 0 ||
	    blk + FAKE_NODE_BLKS > FAKE_MAX_BLKS ||
	    !h->alive[blk / FAKE_NODE_BLKS]) {
		if (err)
			*err = true;
		return NULL;
	}
	h->pins++;
	return h->arena + blk * FAKE_BLK_BYTES;
}

static void fake_unpin(void *host, u64 blk, void *node, bool dirty)
{
	struct fake_host *h = (struct fake_host *)host;

	(void)blk;
	(void)node;
	if (dirty)
		h->dirties++;
}

static u64 fake_alloc(void *host, bool *err)
{
	struct fake_host *h = (struct fake_host *)host;
	u64 blk;

	if (err)
		*err = false;
	if (h->next_blk + FAKE_NODE_BLKS > FAKE_MAX_BLKS) {
		if (err)
			*err = true;
		return RFS_NDFR_NIL;
	}
	blk = h->next_blk;
	h->next_blk += FAKE_NODE_BLKS;
	/* 清零新节点（契约要求）*/
	memset(h->arena + blk * FAKE_BLK_BYTES, 0, FAKE_NODE_BYTES);
	h->alive[blk / FAKE_NODE_BLKS] = 1;
	h->live++;
	h->allocs++;
	return blk;
}

static int fake_free(void *host, u64 blk)
{
	struct fake_host *h = (struct fake_host *)host;

	if (blk % FAKE_NODE_BLKS != 0 ||
	    blk / FAKE_NODE_BLKS >= FAKE_MAX_BLKS / FAKE_NODE_BLKS ||
	    !h->alive[blk / FAKE_NODE_BLKS])
		return -1;
	h->alive[blk / FAKE_NODE_BLKS] = 0;
	h->live--;
	h->frees++;
	return 0;
}

static const struct rfs_ndfr_ops g_ops = {
	fake_pin, fake_unpin, fake_alloc, fake_free
};

#define DIR_INO	7u

/* ══════════════════════════ 小工具 ══════════════════════════ */

static bool klt(u64 al, u64 ab, u64 bl, u64 bb)
{
	return al < bl || (al == bl && ab < bb);
}

static void dino_reset(struct raven_dir_Inode_t *d)
{
	memset(d, 0, sizeof(*d));
	d->name_data_free_ranges_ctl = RFS_ND_FR_CTL_INACTIVE;
	RFS_ND_FR_SET_COUNT(d->name_data_free_ranges_count, 0);
}

/* DFS 全树校验：跨叶 (len,base) 严格递增；hint == 子树最大键且递增；
 * 统计叶条目 / 节点 / 空叶 / 最大内节点孩子数。返回 0 表示无结构错。*/
struct dfs_state {
	bool	first;
	u64	last_len, last_base;
	int	leaf_entries;
	int	nodes;
	int	empty_leaves;
	int	max_children;
	int	errors;
};

static int dfs_check(struct rfs_ndfr_ctx *ctx, u64 blk, bool expect_root,
		     int depth, struct dfs_state *st, u64 *out_len,
		     u64 *out_base)
{
	bool err = false;
	void *buf = ctx->ops->node_pin(ctx->host, blk, &err);
	{
		struct Bptree_meta_t *m;
		u16 cnt, i;

		if (!buf) {
			st->errors++;
			return -1;
		}
		m = (struct Bptree_meta_t *)buf;
		if (!rfs_ndfr_is_node(m)) {
			st->errors++;
			ctx->ops->node_unpin(ctx->host, blk, buf, false);
			return -1;
		}
		if ((bool)RFS_PP_IS_ROOT(m->parent_ptr) != expect_root)
			st->errors++;
		if (depth > 64)
			st->errors++;
		st->nodes++;
		cnt = (u16)RFS_PP_COUNT(m->parent_ptr);

		if (RFS_PP_IS_INNER(m->parent_ptr)) {
			struct Bptree_dir_name_data_free_ranges_inner_node_t *n =
				(struct Bptree_dir_name_data_free_ranges_inner_node_t *)buf;
			u64 pl = 0, pb = 0;
			bool have = false;

			if (cnt == 0)
				st->errors++;
			if ((int)cnt > st->max_children)
				st->max_children = (int)cnt;
			for (i = 0; i < cnt; i++) {
				u64 cl = 0, cb = 0;

				if (dfs_check(ctx,
					      RFS_PTR_ADDR(n->hints[i].sub_node_ptr),
					      false, depth + 1, st, &cl, &cb) != 0)
					st->errors++;
				if (n->hints[i].max_len != cl ||
				    n->hints[i].max_base != cb)
					st->errors++;
				if (have && !klt(pl, pb, cl, cb))
					st->errors++;
				pl = cl;
				pb = cb;
				have = true;
			}
			if (cnt > 0) {
				*out_len = n->hints[cnt - 1].max_len;
				*out_base = n->hints[cnt - 1].max_base;
			} else {
				*out_len = 0;
				*out_base = 0;
			}
		} else {
			struct Bptree_dir_name_data_free_ranges_leaf_node_t *l =
				(struct Bptree_dir_name_data_free_ranges_leaf_node_t *)buf;

			if (cnt == 0)
				st->empty_leaves++;
			for (i = 0; i < cnt; i++) {
				u64 el = l->free_ranges[i].len;
				u64 eb = l->free_ranges[i].base;

				if (!st->first && !klt(st->last_len, st->last_base,
						       el, eb))
					st->errors++;
				st->first = false;
				st->last_len = el;
				st->last_base = eb;
				st->leaf_entries++;
			}
			if (cnt > 0) {
				*out_len = l->free_ranges[cnt - 1].len;
				*out_base = l->free_ranges[cnt - 1].base;
			} else {
				*out_len = 0;
				*out_base = 0;
			}
		}
		ctx->ops->node_unpin(ctx->host, blk, buf, false);
	}
	return st->errors ? -1 : 0;
}

static void dfs_run(struct rfs_ndfr_ctx *ctx, u64 root, struct dfs_state *st)
{
	u64 ml = 0, mb = 0;

	memset(st, 0, sizeof(*st));
	st->first = true;
	dfs_check(ctx, root, true, 0, st, &ml, &mb);
}

static u64 tree_root(const struct raven_dir_Inode_t *d)
{
	return RFS_ND_FR_CTL_PTR(d->name_data_free_ranges_ctl);
}

/* ══════════════════════════ main ══════════════════════════ */
int main(void)
{
	struct fake_host h;
	struct rfs_ndfr_ctx ctx;
	struct raven_dir_Inode_t d;

	memset(&ctx, 0, sizeof(ctx));
	ctx.ops = &g_ops;
	ctx.host = &h;
	ctx.dir_inode_idx = DIR_INO;
	ctx.stat = RFS_STAT_HOOK_NULL;

	printf("=== ravenfs_name_data_freeranges host tests ===\n");

	/* ─────────── 1. 空态 / 首插 inline ─────────── */
	puts("[1] empty -> INACTIVE; first insert inline");
	fake_init(&h);
	dino_reset(&d);
	CHECK(RFS_ND_FR_CTL_PTR(d.name_data_free_ranges_ctl) ==
	      RFS_ND_FR_CTL_PTR_INVALID, "empty ctl ptr invalid");
	CHECK(RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 0,
	      "empty count 0");
	CHECK(rfs_ndfr_insert(&ctx, &d, 1000, 50) == 0, "insert rc");
	CHECK(RFS_ND_FR_CTL_IS_INLINE(d.name_data_free_ranges_ctl) == 1,
	      "inline bit set");
	CHECK(RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 1, "count 1");
	CHECK(d.possible_inline_free_ranges[0].base == 1000 &&
	      d.possible_inline_free_ranges[0].len == 50, "inline entry");
	CHECK(h.live == 0, "no tree nodes");
	fake_fini(&h);

	/* ─────────── 2. ≤9 inline；第 10 条升格 tree ─────────── */
	puts("[2] <=9 inline; 10th -> tree");
	fake_init(&h);
	dino_reset(&d);
	{
		int i;
		u64 base = 0, len = 0;
		u64 root = 0;

		for (i = 0; i < 9; i++) {
			CHECK(rfs_ndfr_insert(&ctx, &d, 100 + (u64)i,
					      10 + (u64)i) == 0,
			      "inline insert %d", i);
			CHECK(RFS_ND_FR_CTL_IS_INLINE(
				      d.name_data_free_ranges_ctl) == 1 &&
			      RFS_ND_FR_COUNT(d.name_data_free_ranges_count) ==
				      (u64)(i + 1),
			      "still inline at %d", i + 1);
		}
		CHECK(rfs_ndfr_insert(&ctx, &d, 5000, 999) == 0, "10th rc");
		CHECK(RFS_ND_FR_CTL_IS_INLINE(d.name_data_free_ranges_ctl) == 0,
		      "upgraded to tree");
		CHECK(RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 10,
		      "count 10");
		CHECK(h.live == 1, "one root leaf node");
		root = tree_root(&d);
		CHECK(rfs_ndfr_find_fit(&ctx, root, 18, &base, &len) == 0 &&
		      len == 18 && base == 108, "find_fit(18)");
		CHECK(rfs_ndfr_find_fit(&ctx, root, 19, &base, &len) == 0 &&
		      len == 999 && base == 5000, "find_fit(19)->999");
		CHECK(rfs_ndfr_find_fit(&ctx, root, 1000, &base, &len) ==
		      -ENOENT, "find_fit(1000) none");
	}
	fake_fini(&h);

	/* ─────────── 3. best-fit 正确性 ─────────── */
	puts("[3] best-fit smallest len>=need");
	fake_init(&h);
	dino_reset(&d);
	{
		int i;
		u64 base = 0, len = 0;
		u64 root;

		/* len = 10,13,...,67（20 条 → tree）；base = 20000 + i*7 */
		for (i = 0; i < 20; i++)
			CHECK(rfs_ndfr_insert(&ctx, &d,
					      20000 + (u64)i * 7,
					      10 + (u64)i * 3) == 0,
			      "insert %d", i);
		root = tree_root(&d);
		CHECK(rfs_ndfr_find_fit(&ctx, root, 1, &base, &len) == 0 &&
		      len == 10 && base == 20000, "fit(1)->10");
		CHECK(rfs_ndfr_find_fit(&ctx, root, 11, &base, &len) == 0 &&
		      len == 13 && base == 20007, "fit(11)->13");
		CHECK(rfs_ndfr_find_fit(&ctx, root, 13, &base, &len) == 0 &&
		      len == 13 && base == 20007, "fit(13)->13");
		CHECK(rfs_ndfr_find_fit(&ctx, root, 40, &base, &len) == 0 &&
		      len == 40 && base == 20000 + 10 * 7, "fit(40)->40");
		CHECK(rfs_ndfr_find_fit(&ctx, root, 67, &base, &len) == 0 &&
		      len == 67 && base == 20000 + 19 * 7, "fit(67)->67");
		CHECK(rfs_ndfr_find_fit(&ctx, root, 68, &base, &len) ==
		      -ENOENT, "fit(68) none");
	}
	fake_fini(&h);

	/* ─────────── 4. take_fit：摘除 + 余段回填 ─────────── */
	puts("[4] take_fit remove / remainder reinsert");
	fake_init(&h);
	dino_reset(&d);
	{
		u64 b = 0;
		int i;

		/* inline：取 400/1000，余段 [5400,600) 回填 */
		CHECK(rfs_ndfr_insert(&ctx, &d, 5000, 1000) == 0, "ins inline");
		CHECK(rfs_ndfr_take_fit(&ctx, &d, 400, &b) == 0 && b == 5000,
		      "take 400 -> base 5000");
		CHECK(RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 1 &&
		      d.possible_inline_free_ranges[0].base == 5400 &&
		      d.possible_inline_free_ranges[0].len == 600,
		      "inline remainder {5400,600}");

		/* 补到 10 条 → tree；含 {5400,600} + len 1000..1008 */
		for (i = 0; i < 9; i++)
			CHECK(rfs_ndfr_insert(&ctx, &d, 30000 + (u64)i * 100,
					      1000 + (u64)i) == 0,
			      "fill %d", i);
		CHECK(RFS_ND_FR_CTL_IS_INLINE(d.name_data_free_ranges_ctl) == 0 &&
		      RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 10,
		      "tree state 10");

		/* tree：取 need=100，best-fit 命中 len=600 → 余段 [5500,500) */
		CHECK(rfs_ndfr_take_fit(&ctx, &d, 100, &b) == 0 && b == 5400,
		      "take 100 -> base 5400");
		{
			u64 base = 0, len = 0;
			u64 root = tree_root(&d);

			CHECK(rfs_ndfr_find_fit(&ctx, root, 500, &base, &len) == 0 &&
			      len == 500 && base == 5500, "remainder findable");
			CHECK(rfs_ndfr_take_fit(&ctx, &d, 500, &b) == 0 &&
			      b == 5500, "take exact 500");
			/* 500 段已摘除：下一 best-fit 命中的是 len=1000 段 */
			CHECK(rfs_ndfr_find_fit(&ctx, root, 500, &base, &len) == 0 &&
			      len == 1000 && base == 30000,
			      "500 remainder gone (next fit 1000)");

			/* 大区间取小 need → 余段回填仍可查 */
			CHECK(rfs_ndfr_insert(&ctx, &d, 40000, 5000) == 0, "ins big");
			CHECK(rfs_ndfr_take_fit(&ctx, &d, 2000, &b) == 0 &&
			      b == 40000, "take 2000 -> 40000");
			CHECK(rfs_ndfr_find_fit(&ctx, root, 3000, &base, &len) == 0 &&
			      len == 3000 && base == 42000, "big remainder");
		}
	}
	fake_fini(&h);

	/* inline 整取清空 → 复位空态 */
	fake_init(&h);
	dino_reset(&d);
	{
		u64 b = 0;

		CHECK(rfs_ndfr_insert(&ctx, &d, 1, 1) == 0, "ins 1");
		CHECK(rfs_ndfr_take_fit(&ctx, &d, 1, &b) == 0 && b == 1,
		      "take exact -> base 1");
		CHECK(RFS_ND_FR_CTL_PTR(d.name_data_free_ranges_ctl) ==
			      RFS_ND_FR_CTL_PTR_INVALID &&
		      RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 0,
		      "inline drained -> INACTIVE");
	}
	fake_fini(&h);

	/* ─────────── 5. 叶分裂 ─────────── */
	puts("[5] leaf split -> inner node; ordered across leaves");
	fake_init(&h);
	dino_reset(&d);
	{
		u64 root, base = 0, len = 0;
		struct dfs_state st;
		int i;

		/* len = i, base = i*32（i=1..340）→ 键序即 i 序；第 338 条触发叶裂 */
		for (i = 1; i <= 340; i++)
			CHECK(rfs_ndfr_insert(&ctx, &d, (u64)i * 32, (u64)i) == 0,
			      "ins %d", i);
		CHECK(RFS_ND_FR_CTL_IS_INLINE(d.name_data_free_ranges_ctl) == 0,
		      "tree state");
		CHECK(RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 340,
		      "count 340");
		root = tree_root(&d);
		{
			bool err = false;
			void *rb = ctx.ops->node_pin(&h, root, &err);
			struct Bptree_meta_t *m = (struct Bptree_meta_t *)rb;

			CHECK(rb && RFS_PP_IS_INNER(m->parent_ptr) &&
			      RFS_PP_IS_ROOT(m->parent_ptr) &&
			      RFS_PP_COUNT(m->parent_ptr) == 2,
			      "root inner with 2 children");
			ctx.ops->node_unpin(&h, root, rb, false);
		}
		dfs_run(&ctx, root, &st);
		CHECK(st.errors == 0, "dfs no structural errors (%d)", st.errors);
		CHECK(st.leaf_entries == 340, "dfs leaf entries 340 (%d)",
		      st.leaf_entries);
		CHECK(st.empty_leaves == 0, "no empty leaves");
		CHECK(st.nodes >= 3, "at least 3 nodes (%d)", st.nodes);
		CHECK(rfs_ndfr_find_fit(&ctx, root, 340, &base, &len) == 0 &&
		      len == 340 && base == 340 * 32, "fit(340)");
		CHECK(rfs_ndfr_find_fit(&ctx, root, 1, &base, &len) == 0 &&
		      len == 1 && base == 32, "fit(1)");
		CHECK(rfs_ndfr_find_fit(&ctx, root, 341, &base, &len) == -ENOENT,
		      "fit(341) none");
	}
	fake_fini(&h);

	/* ─────────── 6. retire_empty_leaf ─────────── */
	puts("[6] retire empty leaf; root shrink; route intact");
	fake_init(&h);
	dino_reset(&d);
	{
		u64 root, b = 0;
		struct dfs_state st;
		u64 need;
		int i;

		for (i = 1; i <= 340; i++)
			CHECK(rfs_ndfr_insert(&ctx, &d, (u64)i * 32, (u64)i) == 0,
			      "ins %d", i);
		/* 取空前 169 条（左叶）→ 触发 retire + 根收缩 */
		for (need = 1; need <= 169; need++) {
			CHECK(rfs_ndfr_take_fit(&ctx, &d, need, &b) == 0 &&
			      b == need * 32, "take need=%llu",
			      (unsigned long long)need);
		}
		CHECK(RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 171,
		      "count 171");
		root = tree_root(&d);
		{
			bool err = false;
			void *rb = ctx.ops->node_pin(&h, root, &err);
			struct Bptree_meta_t *m = (struct Bptree_meta_t *)rb;

			CHECK(rb && !RFS_PP_IS_INNER(m->parent_ptr) &&
			      RFS_PP_IS_ROOT(m->parent_ptr) &&
			      RFS_PP_COUNT(m->parent_ptr) == 171,
			      "root shrunk to right leaf");
			ctx.ops->node_unpin(&h, root, rb, false);
		}
		dfs_run(&ctx, root, &st);
		CHECK(st.errors == 0, "dfs no errors (%d)", st.errors);
		CHECK(st.empty_leaves == 0, "no empty leaves remain");
		CHECK(st.leaf_entries == 171, "leaf entries 171 (%d)",
		      st.leaf_entries);
		{
			u64 base = 0, len = 0;

			CHECK(rfs_ndfr_find_fit(&ctx, root, 170, &base, &len) == 0 &&
			      len == 170 && base == 170 * 32, "fit(170)");
		}
		/* 取尽余下 → 根叶退役 → 全空、零节点 */
		for (need = 170; need <= 340; need++)
			CHECK(rfs_ndfr_take_fit(&ctx, &d, need, &b) == 0 &&
			      b == need * 32, "drain need=%llu",
			      (unsigned long long)need);
		CHECK(RFS_ND_FR_CTL_PTR(d.name_data_free_ranges_ctl) ==
			      RFS_ND_FR_CTL_PTR_INVALID &&
		      RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 0,
		      "drained -> INACTIVE");
		CHECK(h.live == 0, "no live nodes after drain (%d)", h.live);
	}
	fake_fini(&h);

	/* ─────────── 7. release ─────────── */
	puts("[7] release frees all nodes; ctl reset");
	fake_init(&h);
	dino_reset(&d);
	{
		int i, live_before;

		for (i = 1; i <= 340; i++)
			CHECK(rfs_ndfr_insert(&ctx, &d, (u64)i * 32, (u64)i) == 0,
			      "ins %d", i);
		live_before = h.live;
		CHECK(live_before > 0, "tree allocated nodes (%d)", live_before);
		CHECK(rfs_ndfr_release(&ctx, &d) == 0, "release rc");
		CHECK(RFS_ND_FR_CTL_PTR(d.name_data_free_ranges_ctl) ==
			      RFS_ND_FR_CTL_PTR_INVALID &&
		      RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == 0,
		      "ctl INACTIVE / count 0");
		CHECK(h.live == 0, "live 0 after release (%d)", h.live);
		CHECK(h.allocs == h.frees, "allocs==frees (%d/%d)", h.allocs,
		      h.frees);
	}
	/* inline 态 release 亦复位 */
	dino_reset(&d);
	{
		CHECK(rfs_ndfr_insert(&ctx, &d, 9, 9) == 0, "inline insert");
		CHECK(rfs_ndfr_release(&ctx, &d) == 0, "inline release");
		CHECK(RFS_ND_FR_CTL_PTR(d.name_data_free_ranges_ctl) ==
			      RFS_ND_FR_CTL_PTR_INVALID &&
		      d.possible_inline_free_ranges[0].len == 0,
		      "inline table cleared");
		CHECK(h.live == 0, "still no nodes");
	}
	fake_fini(&h);

	/* ─────────── 8. >255 下标回归 ─────────── */
	puts("[8] child index >255 regression");
	fake_init(&h);
	dino_reset(&d);
	{
		const u64 N = 44500;
		u64 base = 0, len = 0, root;
		struct dfs_state st;
		u64 i;

		for (i = 1; i <= N; i++)
			CHECK(rfs_ndfr_insert(&ctx, &d, i * 16, i) == 0,
			      "ins %llu", (unsigned long long)i);
		CHECK(RFS_ND_FR_COUNT(d.name_data_free_ranges_count) == N,
		      "count N");
		root = tree_root(&d);
		dfs_run(&ctx, root, &st);
		CHECK(st.errors == 0, "dfs no errors (%d)", st.errors);
		CHECK(st.max_children > 256,
		      "inner child count >256 (got %d)", st.max_children);
		CHECK(st.leaf_entries == (int)N, "entries %llu",
		      (unsigned long long)N);
		CHECK(rfs_ndfr_find_fit(&ctx, root, N, &base, &len) == 0 &&
		      len == N && base == N * 16, "fit(N) high child index");
		CHECK(rfs_ndfr_find_fit(&ctx, root, 44000, &base, &len) == 0 &&
		      len == 44000 && base == 44000 * 16, "fit(44000)");
		CHECK(rfs_ndfr_find_fit(&ctx, root, 1, &base, &len) == 0 &&
		      len == 1 && base == 16, "fit(1)");
		CHECK(rfs_ndfr_find_fit(&ctx, root, N + 1, &base, &len) ==
		      -ENOENT, "fit(N+1) none");
	}
	fake_fini(&h);

	printf("=== PASS %d  FAIL %d ===\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

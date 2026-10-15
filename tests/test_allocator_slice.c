// SPDX-License-Identifier: GPL-2.0
/*
 * test_allocator_slice.c —— struct rfs_slice（⇄ god mgr_slice_t）用户态冒烟单测
 *
 * 直接编译 ../linux/ravenfs_allocator_slice.c + ../linux/ravenfs_rbtree_intrusive.c
 * （不链 god/ko 其它任何东西）。
 *
 * 覆盖（冒烟级；对抗压测由测试组另做）：
 *   手工位图建片 / alloc_hard 命中与失败 / soft_alloc 多段 + 部分交付 /
 *   soft_occupy / free_interval 融合与重叠报错 / test_interval /
 *   all_dump 递增序 / shutdown + 重 enable。
 * 分配账：元素池 allocs == frees 且 live == 0；每步后 rfs_rb_check 双树 0。
 */
#include "ravenfs_allocator_slice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

/* ══════════════════════════ 元素池（计数）══════════════════════════ */
static u64 g_allocs, g_frees, g_live;

static void *test_alloc(void *ctx, u64 size)
{
	void *p = calloc(1, (size_t)size);

	(void)ctx;
	if (p) {
		g_allocs++;
		g_live++;
	}
	return p;
}

static void test_free(void *ctx, void *p)
{
	(void)ctx;
	if (p) {
		g_frees++;
		g_live--;
		free(p);
	}
}

static struct rfs_slice_alloc g_pool = { test_alloc, test_free, NULL };
static struct rfs_run_alloc g_ralloc = { test_alloc, test_free, NULL };

/* ══════════════════════════ 靶场位图 ══════════════════════════ */
struct bits {
	u8 *mem;
	u64 nbits;
};

static void bits_alloc(struct bits *b, u64 nbits)
{
	size_t bytes = (size_t)((nbits + 7) / 8);

	b->nbits = nbits;
	if (posix_memalign((void **)&b->mem, 4096, bytes) != 0) {
		printf("  FAIL posix_memalign\n");
		exit(1);
	}
	memset(b->mem, 0xFF, bytes);	/* 全 1 = 全部占用 */
}

static void bits_free(struct bits *b)
{
	free(b->mem);
	b->mem = NULL;
}

/* 片内偏移 off 起 n 位置空闲（清 0）。*/
static void bits_set_free(struct bits *b, u64 off, u64 n)
{
	u64 k;

	for (k = 0; k < n; k++)
		b->mem[(off + k) >> 3] &= (u8)~(1u << ((off + k) & 7));
}

/* ══════════════════════════ 校验小工具 ══════════════════════════ */

/* 测试侧比较器（仅用于 rfs_rb_check；与片内严格序在「两两不重叠」下等价于 base 序）。*/
static int t_cmp_pos(const struct rfs_rb_node *a, const struct rfs_rb_node *b,
		     void *ctx)
{
	const struct rfs_interval *x =
		container_of(a, struct rfs_interval, by_pos);
	const struct rfs_interval *y =
		container_of(b, struct rfs_interval, by_pos);

	(void)ctx;
	if (x->base != y->base)
		return (x->base < y->base) ? -1 : 1;
	return 0;
}

static int t_cmp_size(const struct rfs_rb_node *a, const struct rfs_rb_node *b,
		      void *ctx)
{
	const struct rfs_interval *x =
		container_of(a, struct rfs_interval, by_size);
	const struct rfs_interval *y =
		container_of(b, struct rfs_interval, by_size);

	(void)ctx;
	if (x->len != y->len)
		return (x->len < y->len) ? -1 : 1;
	if (x->base != y->base)
		return (x->base < y->base) ? -1 : 1;
	return 0;
}

static void check_trees(struct rfs_slice *s, const char *tag)
{
	int rp = rfs_rb_check(&s->pos, t_cmp_pos, NULL);
	int rs = rfs_rb_check(&s->size, t_cmp_size, NULL);

	if (rp != 0 || rs != 0)
		printf("  FAIL %s: rb_check pos=%d size=%d\n", tag, rp, rs);
	CHECK(rp == 0 && rs == 0, tag);
}

static void slice_zero(struct rfs_slice *s)
{
	memset(s, 0, sizeof(*s));
	RFS_MUTEX_INIT(&s->lock);
}

/* 取 all_dump 快照到数组，返回个数（同时可校验递增序）。*/
static int dump_slice(struct rfs_slice *s, struct rfs_run *out, int cap)
{
	struct rfs_run_dlist dl;
	struct rfs_run_node *n;
	int cnt = 0;

	rfs_run_dlist_init(&dl, g_ralloc);
	if (rfs_slice_all_dump(s, &dl) != 0) {
		printf("  FAIL all_dump rc\n");
		g_fail++;
		rfs_run_dlist_fini(&dl);
		return -1;
	}
	for (n = dl.head; n; n = n->next) {
		if (cnt < cap)
			out[cnt] = n->run;
		cnt++;
	}
	rfs_run_dlist_fini(&dl);
	return cnt;
}

/* 期望快照 = base 递增、且与给定 (base,len) 序列一致。*/
static void expect_runs(struct rfs_slice *s, const struct rfs_run *want, int wn,
			const char *tag)
{
	struct rfs_run got[64];
	int n, i;

	n = dump_slice(s, got, 64);
	if (n != wn) {
		printf("  FAIL %s: count got=%d want=%d\n", tag, n, wn);
		g_fail++;
		return;
	}
	for (i = 0; i < n; i++) {
		if (got[i].base != want[i].base || got[i].len != want[i].len) {
			printf("  FAIL %s: [%d] got {%llu,%llu} want {%llu,%llu}\n",
			       tag, i,
			       (unsigned long long)got[i].base,
			       (unsigned long long)got[i].len,
			       (unsigned long long)want[i].base,
			       (unsigned long long)want[i].len);
			g_fail++;
			return;
		}
		if (i > 0 && got[i].base <= got[i - 1].base) {
			printf("  FAIL %s: not strictly increasing\n", tag);
			g_fail++;
			return;
		}
	}
	CHECK(1, tag);
}

/* ══════════════════════════ main ══════════════════════════ */
int main(void)
{
	const u64 OFF = 500;	/* 全局起点：非 0，验证全局坐标 */
	const u64 NBITS = 64;
	struct rfs_slice s;
	struct bits b;
	struct rfs_slice_cfg cfg;
	struct rfs_run want[8];

	printf("=== ravenfs_allocator_slice smoke tests ===\n");

	/* ═══ 1. enable 校验 / 门闸 / 建树 ═══ */
	puts("[1] enable 校验 + 建树");
	slice_zero(&s);
	{
		struct rfs_slice_cfg bad;

		CHECK(rfs_slice_test_interval(&s, false, OFF, 1) == -EINVAL,
		      "gate: test before enable");
		CHECK(rfs_slice_free_interval(&s, OFF, 1) == -EINVAL,
		      "gate: free before enable");
		{
			struct rfs_run_dlist dl;

			rfs_run_dlist_init(&dl, g_ralloc);
			CHECK(rfs_slice_all_dump(&s, &dl) == -EINVAL,
			      "gate: dump before enable");
			rfs_run_dlist_fini(&dl);
		}
		{
			struct rfs_run_list fl;

			rfs_run_list_init(&fl, g_ralloc);
			CHECK(rfs_slice_free_intervals(&s, &fl) == -EINVAL,
			      "gate: free_intervals before enable");
			rfs_run_list_fini(&fl);
		}
		{
			u64 r = 0;

			CHECK(rfs_slice_soft_occupy(&s, OFF, 1, &r) == -EINVAL,
			      "gate: occ before enable");
		}

		memset(&bad, 0, sizeof(bad));
		bad.in_global_base_idx = OFF;
		bad.slice_bitscount = NBITS;
		bad.alloc = g_pool;
		bad.slice_base = NULL;
		CHECK(rfs_slice_enable(&s, &bad) == -EINVAL, "enable: null base");

		bits_alloc(&b, NBITS + 8);
		bad.slice_base = b.mem + 1;	/* 故意错位 */
		CHECK(rfs_slice_enable(&s, &bad) == -EINVAL, "enable: misaligned");
		bad.slice_base = b.mem;
		bad.slice_bitscount = 0;
		CHECK(rfs_slice_enable(&s, &bad) == -EINVAL, "enable: zero bits");
		bad.alloc.alloc = NULL;
		bad.slice_bitscount = NBITS;
		CHECK(rfs_slice_enable(&s, &bad) == -EINVAL, "enable: null pool");
		bits_free(&b);
	}

	bits_alloc(&b, NBITS);
	/* 空闲 run（片内偏移）：[0,10) [30,40) [60,64) → 全局 [500,510)[530,540)[560,564) */
	bits_set_free(&b, 0, 10);
	bits_set_free(&b, 30, 10);
	bits_set_free(&b, 60, 4);
	memset(&cfg, 0, sizeof(cfg));
	cfg.slice_base = b.mem;
	cfg.in_global_base_idx = OFF;
	cfg.slice_bitscount = NBITS;
	cfg.alloc = g_pool;
	CHECK(rfs_slice_enable(&s, &cfg) == 0, "enable ok");
	CHECK(rfs_slice_enable(&s, &cfg) == -EINVAL, "enable: repeat rejected");
	CHECK(rfs_slice_free_count(&s) == 24, "enable: free_count==24");
	check_trees(&s, "enable: rb_check");
	want[0].base = 500; want[0].len = 10;
	want[1].base = 530; want[1].len = 10;
	want[2].base = 560; want[2].len = 4;
	expect_runs(&s, want, 3, "enable: 3 maximal runs (increasing)");

	/* ═══ 2. test_interval ═══ */
	puts("[2] test_interval");
	CHECK(rfs_slice_test_interval(&s, false, 530, 10) == 0, "test free: exact");
	CHECK(rfs_slice_test_interval(&s, false, 530, 11) == -ENOENT, "test free: overrun");
	CHECK(rfs_slice_test_interval(&s, false, 535, 3) == 0, "test free: sub-run");
	CHECK(rfs_slice_test_interval(&s, false, 500, 11) == -ENOENT, "test free: cross");
	CHECK(rfs_slice_test_interval(&s, true, 510, 20) == 0, "test occ: gap");
	CHECK(rfs_slice_test_interval(&s, true, 525, 10) == -ENOENT, "test occ: touches free");
	CHECK(rfs_slice_test_interval(&s, true, 500, 1) == -ENOENT, "test occ: free bit");
	CHECK(rfs_slice_test_interval(&s, false, 530, 0) == 0, "test len==0");
	CHECK(rfs_slice_test_interval(&s, false, 499, 1) == -EINVAL, "test: below start");
	CHECK(rfs_slice_test_interval(&s, false, 564, 1) == -EINVAL, "test: above end");

	/* ═══ 3. free_interval：重叠报错 + 邻接融合 ═══ */
	puts("[3] free_interval 重叠/融合");
	CHECK(rfs_slice_free_interval(&s, 505, 3) == -EEXIST, "free: overlap (in free run)");
	CHECK(rfs_slice_free_count(&s) == 24, "free: overlap leaves count");
	CHECK(rfs_slice_free_interval(&s, 510, 20) == 0, "free: merge both sides");
	CHECK(rfs_slice_free_count(&s) == 44, "free: count 24+20");
	want[0].base = 500; want[0].len = 40;
	want[1].base = 560; want[1].len = 4;
	expect_runs(&s, want, 2, "free: [500,540)+[560,564)");
	check_trees(&s, "free: rb_check after merge");
	CHECK(rfs_slice_test_interval(&s, false, 510, 20) == 0, "free: now all free");
	CHECK(rfs_slice_free_interval(&s, 540, 20) == 0, "free: merge right again");
	want[0].base = 500; want[0].len = 64;
	expect_runs(&s, want, 1, "free: single [500,564)");
	CHECK(rfs_slice_free_count(&s) == 64, "free: count 64");

	/* ═══ 4. soft_occupy ═══ */
	puts("[4] soft_occupy");
	{
		u64 rest = 0;

		/* [500,564) 全空闲；认领 [500,510) */
		CHECK(rfs_slice_soft_occupy(&s, 500, 10, &rest) == 0, "occ: claim 10");
		CHECK(rest == 0, "occ: rest 0");
		CHECK(rfs_slice_free_count(&s) == 54, "occ: count 54");
		want[0].base = 510; want[0].len = 54;
		expect_runs(&s, want, 1, "occ: remainder [510,564)");

		/* 链式记账：入 rest=4（已满足 4），len=10 → real=6，认领 [510,516) */
		rest = 4;
		CHECK(rfs_slice_soft_occupy(&s, 510, 10, &rest) == 0, "occ: chain claim 6");
		CHECK(rest == 4, "occ: rest 10-6=4");
		CHECK(rfs_slice_free_count(&s) == 48, "occ: count 48");

		/* 锚点已在占用位 → -ENOSPC，rest 回填 len */
		rest = 0;
		CHECK(rfs_slice_soft_occupy(&s, 500, 5, &rest) == -ENOSPC, "occ: on occupied");
		CHECK(rest == 5, "occ: rest backfilled");
		/* 越片首 */
		rest = 0;
		CHECK(rfs_slice_soft_occupy(&s, 499, 1, &rest) == -EINVAL, "occ: below start");
	}
	check_trees(&s, "occ: rb_check");

	/* 复原为全空闲，便于 alloc_hard / soft_alloc 重来 */
	CHECK(rfs_slice_free_interval(&s, 500, 10) == 0, "occ: restitute low");
	CHECK(rfs_slice_free_interval(&s, 510, 6) == 0, "occ: restitute mid");
	CHECK(rfs_slice_free_count(&s) == 64, "occ: restored 64");

	/* ═══ 5. alloc_hard：best-fit + 头部上挖 + 对齐跳过 + 失败 ═══ */
	puts("[5] alloc_hard");
	CHECK(rfs_slice_shutdown(&s) == 0, "alloc_hard: shutdown");
	bits_free(&b);
	/* 新靶场：[500,516)(16) [524,532)(8，非 8 对齐) [540,564)(24) */
	bits_alloc(&b, NBITS);
	bits_set_free(&b, 0, 16);
	bits_set_free(&b, 24, 8);
	bits_set_free(&b, 40, 24);
	cfg.slice_base = b.mem;
	cfg.in_global_base_idx = OFF;
	cfg.slice_bitscount = NBITS;
	cfg.alloc = g_pool;
	CHECK(rfs_slice_enable(&s, &cfg) == 0, "alloc_hard: re-enable");
	{
		u64 o = 0;

		/* 需 10 → 最小 len≥10 的 [500,516) → 头部上挖 [500,510)，余 [510,6) */
		CHECK(rfs_slice_alloc_hard(&s, 0, 10, &o) == 0, "hard: rc");
		CHECK(o == 500, "hard: head-carve base 500");
		want[0].base = 510; want[0].len = 6;
		want[1].base = 524; want[1].len = 8;
		want[2].base = 540; want[2].len = 24;
		expect_runs(&s, want, 3, "hard: remainder");
		CHECK(rfs_slice_free_count(&s) == 38, "hard: count 48-10");
		check_trees(&s, "hard: rb_check");

		/* 需 8、align=2^3：跳过非 8 对齐的 [524,532)（头对齐 528 越出 532 → 容不下），
		 * 选 [540,564) → 头部对齐 (540+7)&~7 = 544 → [544,552)，余 [540,4)+[552,12) */
		CHECK(rfs_slice_alloc_hard(&s, 3, 8, &o) == 0, "hard: aligned rc");
		CHECK(o == 544, "hard: aligned base 544");
		want[0].base = 510; want[0].len = 6;
		want[1].base = 524; want[1].len = 8;
		want[2].base = 540; want[2].len = 4;
		want[3].base = 552; want[3].len = 12;
		expect_runs(&s, want, 4, "hard: aligned split");
		CHECK(rfs_slice_free_count(&s) == 30, "hard: count 38-8");

		/* 需 100 → 满片无命中，出参不动 */
		o = 0xDEADBEEFULL;
		CHECK(rfs_slice_alloc_hard(&s, 0, 100, &o) == -ENOSPC, "hard: no fit");
		CHECK(o == 0xDEADBEEFULL, "hard: out param untouched");
		CHECK(rfs_slice_alloc_hard(&s, 0, 0, &o) == -EINVAL, "hard: zero rejected");
		CHECK(rfs_slice_alloc_hard(&s, 64, 1, &o) == -EINVAL, "hard: align>63 rejected");
	}

	/* ═══ 5b. D-14 方向契约：头挖 → 连续分配物理升序（令锚定/融合可命中）═══ */
	puts("[5b] D-14 direction (head-dig)");
	CHECK(rfs_slice_shutdown(&s) == 0, "d14: shutdown");
	bits_free(&b);
	bits_alloc(&b, NBITS);
	bits_set_free(&b, 0, NBITS);		/* 整片空闲（片首起）*/
	cfg.slice_base = b.mem;
	cfg.in_global_base_idx = OFF;
	cfg.slice_bitscount = NBITS;
	cfg.alloc = g_pool;
	CHECK(rfs_slice_enable(&s, &cfg) == 0, "d14: enable whole-free");
	{
		u64 a0 = 0, a1 = 0, a2 = 0;

		/* 同一自由区间连续三次硬分配 → 落点必须严格升序（头挖）*/
		CHECK(rfs_slice_alloc_hard(&s, 0, 8, &a0) == 0, "d14: a0 rc");
		CHECK(rfs_slice_alloc_hard(&s, 0, 8, &a1) == 0, "d14: a1 rc");
		CHECK(rfs_slice_alloc_hard(&s, 0, 8, &a2) == 0, "d14: a2 rc");
		CHECK(a0 == OFF, "d14: a0 at slice head");
		CHECK(a1 == OFF + 8, "d14: a1 ascending (a0+8)");
		CHECK(a2 == OFF + 16, "d14: a2 ascending (a1+8)");
		CHECK(a0 < a1 && a1 < a2, "d14: physical order ascending");
		/* 对齐：align=3 需 8；余段 [524,564) → 向上对齐到 528（头挖 + 对齐）*/
		{
			u64 a3 = 0;

			CHECK(rfs_slice_alloc_hard(&s, 3, 8, &a3) == 0,
			      "d14: aligned head rc");
			CHECK(a3 == OFF + 28, "d14: aligned head-dig continues (528)");
		}
	}

	/* ═══ 6. soft_alloc：多段 + 部分交付 ═══ */
	puts("[6] soft_alloc");
	CHECK(rfs_slice_shutdown(&s) == 0, "soft: shutdown");
	bits_free(&b);
	bits_alloc(&b, NBITS);
	bits_set_free(&b, 0, 10);	/* [500,510) */
	bits_set_free(&b, 30, 10);	/* [530,540) */
	bits_set_free(&b, 60, 4);	/* [560,564) */
	cfg.slice_base = b.mem;
	CHECK(rfs_slice_enable(&s, &cfg) == 0, "soft: re-enable");
	{
		struct rfs_run_list rl;
		bool sat = false;
		u64 sum = 0;
		struct rfs_run_node *nd;

		/* 请求 12：碎片优先 → [560,4)(整段) + [500,10) 头挖 [500,8) */
		rfs_run_list_init(&rl, g_ralloc);
		CHECK(rfs_slice_soft_alloc(&s, 0, 12, &rl, &sat) == 0, "soft: rc");
		CHECK(sat == true, "soft: all_satisfied");
		for (nd = rl.head; nd; nd = nd->next)
			sum += nd->run.len;
		CHECK(sum == 12, "soft: delivered sum 12");
		CHECK(rfs_slice_free_count(&s) == 24 - 12, "soft: count 12");
		want[0].base = 508; want[0].len = 2;
		want[1].base = 530; want[1].len = 10;
		expect_runs(&s, want, 2, "soft: remainder");
		rfs_run_list_fini(&rl);
		check_trees(&s, "soft: rb_check");

		/* 请求 100 → 部分交付：耗尽剩余 12，all_satisfied=false */
		rfs_run_list_init(&rl, g_ralloc);
		CHECK(rfs_slice_soft_alloc(&s, 0, 100, &rl, &sat) == 0, "soft: partial rc");
		CHECK(sat == false, "soft: not satisfied");
		sum = 0;
		for (nd = rl.head; nd; nd = nd->next)
			sum += nd->run.len;
		CHECK(sum == 12, "soft: partial sum 12");
		CHECK(rfs_slice_free_count(&s) == 0, "soft: count 0");
		rfs_run_list_fini(&rl);

		/* 已空：一位未扣 → -ENOSPC */
		rfs_run_list_init(&rl, g_ralloc);
		CHECK(rfs_slice_soft_alloc(&s, 0, 1, &rl, &sat) == -ENOSPC, "soft: empty -ENOSPC");
		CHECK(sat == false, "soft: sat false on empty");
		rfs_run_list_fini(&rl);
		CHECK(rfs_slice_soft_alloc(&s, 0, 0, &rl, &sat) == -EINVAL, "soft: zero rejected");
	}

	/* ═══ 7. free_intervals：消费链 ═══ */
	puts("[7] free_intervals");
	{
		struct rfs_run_list rl;

		/* 释放 [500,510) 与 [530,540)（两段）；当前树空 */
		rfs_run_list_init(&rl, g_ralloc);
		CHECK(rfs_run_list_push_front(&rl, 530, 10) == 0, "fi: push 530");
		CHECK(rfs_run_list_push_front(&rl, 500, 10) == 0, "fi: push 500");
		CHECK(rfs_slice_free_intervals(&s, &rl) == 0, "fi: rc");
		CHECK(rl.head == NULL, "fi: chain drained");
		CHECK(rfs_slice_free_count(&s) == 20, "fi: count 20");
		want[0].base = 500; want[0].len = 10;
		want[1].base = 530; want[1].len = 10;
		expect_runs(&s, want, 2, "fi: two runs");
		/* 中途出错（重叠）→ 余段留链 */
		rfs_run_list_init(&rl, g_ralloc);
		CHECK(rfs_run_list_push_front(&rl, 540, 5) == 0, "fi2: push bad(first popped last)");
		CHECK(rfs_run_list_push_front(&rl, 505, 2) == 0, "fi2: push overlap");
		CHECK(rfs_slice_free_intervals(&s, &rl) == -EEXIST, "fi2: overlap -> -EEXIST");
		CHECK(rl.head != NULL, "fi2: remainder stays");
		rfs_run_list_fini(&rl);
	}

	/* ═══ 8. shutdown + 重 enable ═══ */
	puts("[8] shutdown + re-enable");
	check_trees(&s, "final: rb_check");
	CHECK(rfs_slice_shutdown(&s) == 0, "shutdown ok");
	CHECK(rfs_slice_shutdown(&s) == -EINVAL, "shutdown twice rejected");
	CHECK(rfs_slice_free_count(&s) == 0, "shutdown: count 0");
	CHECK(rfs_rb_empty(&s.pos) && rfs_rb_empty(&s.size), "shutdown: trees empty");
	CHECK(rfs_slice_enable(&s, &cfg) == 0, "re-enable ok");
	CHECK(rfs_slice_free_count(&s) == 24, "re-enable: count 24");
	CHECK(rfs_slice_shutdown(&s) == 0, "final shutdown");

	/* ═══ 9. 片内脏标志（D-15：分片按脏写回的隐状态）═══ */
	puts("[9] dirty flag（片内隐状态）");
	{
		struct rfs_slice s2;
		struct bits b2;
		struct rfs_slice_cfg cfg2;
		struct rfs_run_list rl;
		u64 rest = 0, out = 0;

		slice_zero(&s2);
		bits_alloc(&b2, NBITS);
		bits_set_free(&b2, 0, NBITS);	/* 全空闲 → 单 run */
		memset(&cfg2, 0, sizeof(cfg2));
		cfg2.slice_base = b2.mem;
		cfg2.in_global_base_idx = OFF;
		cfg2.slice_bitscount = NBITS;
		cfg2.alloc = g_pool;
		CHECK(rfs_slice_enable(&s2, &cfg2) == 0, "dirty: enable ok");
		CHECK(rfs_slice_dirty_take(&s2) == false, "dirty: enable -> clean（载入盘面=基线）");
		CHECK(rfs_slice_dirty_take(&s2) == false, "dirty: clean take idempotent");

		/* soft_occupy：变更 → 置脏；take 取走并清除 */
		CHECK(rfs_slice_soft_occupy(&s2, OFF, 4, &rest) == 0, "dirty: occupy ok");
		CHECK(rfs_slice_dirty_take(&s2) == true, "dirty: occupy -> dirty");
		CHECK(rfs_slice_dirty_take(&s2) == false, "dirty: take clears");

		/* alloc_hard：变更 → 置脏 */
		CHECK(rfs_slice_alloc_hard(&s2, 0, 8, &out) == 0, "dirty: alloc_hard ok");
		CHECK(rfs_slice_dirty_take(&s2) == true, "dirty: alloc_hard -> dirty");

		/* soft_alloc：变更 → 置脏 */
		rfs_run_list_init(&rl, g_ralloc);
		{
			bool sat = false;

			CHECK(rfs_slice_soft_alloc(&s2, 0, 8, &rl, &sat) == 0,
			      "dirty: soft_alloc ok");
			rfs_run_list_fini(&rl);
		}
		CHECK(rfs_slice_dirty_take(&s2) == true, "dirty: soft_alloc -> dirty");

		/* free_interval：变更 → 置脏 */
		CHECK(rfs_slice_free_interval(&s2, out, 8) == 0, "dirty: free ok");
		CHECK(rfs_slice_dirty_take(&s2) == true, "dirty: free -> dirty");

		/* re-enable：重建 = 载入盘面 → 归零 */
		CHECK(rfs_slice_shutdown(&s2) == 0, "dirty: shutdown ok");
		CHECK(rfs_slice_enable(&s2, &cfg2) == 0, "dirty: re-enable ok");
		CHECK(rfs_slice_dirty_take(&s2) == false, "dirty: re-enable -> clean");
		CHECK(rfs_slice_shutdown(&s2) == 0, "dirty: final shutdown");
		bits_free(&b2);
	}

	bits_free(&b);

	/* ═══ 分配账 ═══ */
	printf("--- element pool: allocs=%llu frees=%llu live=%llu\n",
	       (unsigned long long)g_allocs, (unsigned long long)g_frees,
	       (unsigned long long)g_live);
	CHECK(g_live == 0, "leak: live == 0");
	CHECK(g_allocs == g_frees, "leak: allocs == frees");

	printf("=== PASS %d  FAIL %d ===\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

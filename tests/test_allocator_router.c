// SPDX-License-Identifier: GPL-2.0
/*
 * test_allocator_router.c —— 顶层路由 struct rfs_allocator（⇄ god fs_allocator_t）冒烟单测
 *
 * 直接编 ../linux/ravenfs_allocator_router.c + ../linux/ravenfs_allocator_slice.c
 * + ../linux/ravenfs_rbtree_intrusive.c（不链 god/ko 其它任何东西）。
 *
 * 覆盖（冒烟级；对抗压测由测试组另做）：
 *   1. init 几何推导（2^n ∩ 满片数 / 末片非空 / cap 压片数）+ preferred 初值 +
 *      参数缺失自清理；fini 后分配账清
 *   2. block_alloc_hard：单片命中 / 全败哨兵 / 单片原子不跨片拼
 *   3. block_alloc_soft：跨片多段凑满 + 不够回滚（all-or-nothing）
 *   4. block_occupy_soft：跨片续圈 / 顶片尾记账 / 锚点占用 / 门闸
 *   5. block_free 跨片循环 + block_intervals_free 链式消费
 *   6. inode_alloc / inode_free / inode_test / 越界
 *   7. dump_slice：逐片纯快照与手工位图一致（block + inode，含跨片 run 切分）
 *
 * 分配账：allocs == frees 且 live == 0；ASan+UBSan 0 error。
 */
#include "ravenfs_allocator_top.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

/* ══════════════════════════ 计数分配器（全经注入）══════════════════════════ */
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

static struct rfs_mem_alloc g_mem = { test_alloc, test_free, NULL };
static struct rfs_run_alloc g_ralloc = { test_alloc, test_free, NULL };

/* ══════════════════════════ 靶场位图 ══════════════════════════ */
#define MSB 32768ULL
#define BITS (MSB * 4ULL)	/* 4 片靶场 = 131072 bit */

struct raw {
	u8 *mem;
	u64 nbits;
};

static void raw_init(struct raw *r, u64 nbits)
{
	size_t bytes = (size_t)((nbits + 7) / 8);

	r->nbits = nbits;
	if (posix_memalign((void **)&r->mem, 4096, bytes) != 0) {
		printf("  FAIL posix_memalign\n");
		exit(1);
	}
	memset(r->mem, 0xFF, bytes);	/* 全 1 = 全部占用 */
}

static void raw_free(struct raw *r)
{
	free(r->mem);
	r->mem = NULL;
}

/* 清 0 = 置空闲（LSB-first，与片 enable 扫描同口径）。*/
static void raw_clear(struct raw *r, u64 start, u64 n)
{
	u64 k;

	for (k = 0; k < n; k++)
		r->mem[(start + k) >> 3] &= (u8)~(1u << ((start + k) & 7));
}

static void make_cfg(struct rfs_slices_cfg *cfg, struct raw *b, struct raw *i,
		     u32 cpus)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->block_bits_raw = b->mem;
	cfg->block_bits_count = b->nbits;
	cfg->inode_bits_raw = i->mem;
	cfg->inode_bits_count = i->nbits;
	cfg->logical_processor_count = cpus;
	cfg->alloc = g_mem;
}

/* ══════════════════════════ 小工具 ══════════════════════════ */

static u64 sum_runs(const struct rfs_run_list *l)
{
	const struct rfs_run_node *n;
	u64 s = 0;

	for (n = l->head; n; n = n->next)
		s += n->run.len;
	return s;
}

static u64 count_runs(const struct rfs_run_list *l)
{
	const struct rfs_run_node *n;
	u64 c = 0;

	for (n = l->head; n; n = n->next)
		c++;
	return c;
}

/* dump_slice 快照到数组；返回 run 数（rc!=0 → -1）。*/
static int dump_slice(struct rfs_allocator *a, bool ino, u32 slice,
		      struct rfs_run *out, int cap)
{
	struct rfs_run_dlist dl;
	struct rfs_run_node *n;
	int cnt = 0;

	rfs_run_dlist_init(&dl, g_ralloc);
	if (rfs_allocator_dump_slice(a, ino, slice, &dl) != 0) {
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

static void expect_runs(const struct rfs_run *got, int n,
			const struct rfs_run *want, int wn, const char *tag)
{
	int i;

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

/* ══════════════════════════ 1. init 几何 / preferred / 自清理 ══════════════════════════ */
static void test_init_geometry(void)
{
	struct raw b, i;
	struct rfs_slices_cfg cfg;
	struct rfs_allocator a;
	u32 c;

	puts("[1] init 几何推导 / preferred / 参数自清理");

	raw_init(&b, BITS);
	raw_init(&i, BITS);
	make_cfg(&cfg, &b, &i, 4);
	CHECK(rfs_allocator_init(&a, &cfg) == 0, "ctor 4-slice ok");
	CHECK(rfs_allocator_slice_count(&a) == 4, "slice_count==4");
	CHECK(rfs_allocator_cpu_slots(&a) == 4, "cpu_slots==4");
	CHECK(rfs_allocator_block_per_slice_bits(&a) == MSB, "block per==32768");
	CHECK(rfs_allocator_inode_per_slice_bits(&a) == MSB, "inode per==32768");
	for (c = 0; c < 4; c++) {
		CHECK(rfs_allocator_block_preferred_of(&a, c) == (c % 4),
		      "block preferred init i%n");
		CHECK(rfs_allocator_inode_preferred_of(&a, c) == (c % 4),
		      "inode preferred init i%n");
	}
	CHECK(rfs_allocator_block_free_total(&a) == 0, "all-occupied block free==0");
	CHECK(rfs_allocator_inode_free_total(&a) == 0, "all-occupied inode free==0");
	CHECK(rfs_allocator_block_test(&a, 0) == true, "block 0 occupied");
	CHECK(rfs_allocator_inode_test(&a, 0) == true, "inode 0 (root) occupied");
	{
		struct rfs_run got[4];
		u32 s;

		for (s = 0; s < 4; s++) {
			CHECK(dump_slice(&a, false, s, got, 4) == 0,
			      "dump occupied block slice empty");
			CHECK(dump_slice(&a, true, s, got, 4) == 0,
			      "dump occupied inode slice empty");
		}
	}
	CHECK(rfs_allocator_cpu_slots(&a) == 4, "cpu_slots() const read");

	/* 装配回调缺失 → -EINVAL（模块零直接分配）*/
	{
		struct rfs_slices_cfg bad = cfg;

		bad.alloc.alloc = NULL;
		{
			struct rfs_allocator z;

			CHECK(rfs_allocator_init(&z, &bad) == -EINVAL,
			      "init: null mem alloc rejected");
			rfs_allocator_fini(&z);
		}
	}
	rfs_allocator_fini(&a);
	/* fini 后空态可读、幂等 */
	CHECK(rfs_allocator_slice_count(&a) == 0, "fini: cleared");
	CHECK(rfs_allocator_block_free_total(&a) == 0, "fini: free total 0");
	rfs_allocator_fini(&a);

	/* 参数缺失：空态 */
	{
		struct rfs_slices_cfg z;
		struct rfs_allocator a2;

		memset(&z, 0, sizeof(z));
		CHECK(rfs_allocator_init(&a2, &z) == -EINVAL, "init: null raw -> -EINVAL");
		CHECK(rfs_allocator_slice_count(&a2) == 0, "init fail cleared");
		CHECK(rfs_allocator_block_test(&a2, 0) == false, "empty test false");
		rfs_allocator_fini(&a2);
	}
	raw_free(&b);
	raw_free(&i);

	/* cap=3 把 2^n 上界 4 压到 3 */
	{
		struct raw b3, i3;
		struct rfs_slices_cfg c3;
		struct rfs_allocator a3;

		raw_init(&b3, MSB * 3);
		raw_init(&i3, MSB * 3);
		make_cfg(&c3, &b3, &i3, 4);
		CHECK(rfs_allocator_init(&a3, &c3) == 0, "ctor 3-slice ok");
		CHECK(rfs_allocator_slice_count(&a3) == 3, "cap=3 -> slice_count==3");
		rfs_allocator_fini(&a3);
		raw_free(&b3);
		raw_free(&i3);
	}
}

/* ══════════════════════════ 2. block_alloc_hard ══════════════════════════ */
static void test_block_alloc_hard(void)
{
	struct raw b, i;
	struct rfs_slices_cfg cfg;
	struct rfs_allocator a;
	u64 r;

	puts("[2] block_alloc_hard：单片命中 / 全败 / 不跨片");

	raw_init(&b, BITS);
	raw_init(&i, BITS);
	raw_clear(&b, 100, 8);		/* slice0：8 连续 */
	raw_clear(&b, 32768, 4);	/* slice1：4 连续 */
	make_cfg(&cfg, &b, &i, 4);
	CHECK(rfs_allocator_init(&a, &cfg) == 0, "ctor ok");
	CHECK(rfs_allocator_block_free_total(&a) == 12, "free 8+4==12");

	CHECK(rfs_allocator_block_alloc_hard(&a, 8, 0) == 100, "hard hit @100");
	{
		u64 k;

		for (k = 100; k < 108; k++)
			CHECK(rfs_allocator_block_test(&a, k) == true, "consumed bit occupied");
	}
	CHECK(rfs_allocator_block_free_total(&a) == 4, "free 12-8==4");
	/* 只剩 slice1 的 4；需 8 → 原子失败（不得跨片搬）*/
	CHECK(rfs_allocator_block_alloc_hard(&a, 8, 0) == RFS_IV_NO_HIT,
	      "hard all-fail -> NO_HIT");
	CHECK(rfs_allocator_block_alloc_hard(&a, 4, 0) == 32768, "hard slice1 4 ok");
	CHECK(rfs_allocator_block_free_total(&a) == 0, "free drained");
	CHECK(rfs_allocator_block_alloc_hard(&a, 1, 0) == RFS_IV_NO_HIT, "hard full -> NO_HIT");
	CHECK(rfs_allocator_block_alloc_hard(&a, 0, 0) == RFS_IV_NO_HIT, "hard zero -> NO_HIT");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);

	/* 相邻跨片空洞：硬分配必须单片完成，8 连续跨片 → 失败 */
	raw_init(&b, BITS);
	raw_init(&i, BITS);
	raw_clear(&b, 32764, 4);	/* slice0 尾 */
	raw_clear(&b, 32768, 4);	/* slice1 首（与上合计物理连续 8）*/
	make_cfg(&cfg, &b, &i, 4);
	CHECK(rfs_allocator_init(&a, &cfg) == 0, "ctor adjacent ok");
	CHECK(rfs_allocator_block_alloc_hard(&a, 8, 0) == RFS_IV_NO_HIT,
	      "hard never spans slices (8 across boundary)");
	r = rfs_allocator_block_alloc_hard(&a, 4, 0);
	CHECK(r == 32764 || r == 32768, "hard single-slice 4 ok");
	CHECK(rfs_allocator_block_test(&a, r) == true, "hard 4 occupied");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* ══════════════════════════ 3. block_alloc_soft ══════════════════════════ */
static void test_block_alloc_soft(void)
{
	struct raw b, i;
	struct rfs_slices_cfg cfg;
	struct rfs_allocator a;
	struct rfs_run_list runs;
	u64 segs;

	puts("[3] block_alloc_soft：跨片多段凑满 / 不够回滚");

	raw_init(&b, BITS);
	raw_init(&i, BITS);
	raw_clear(&b, 32760, 4);	/* slice0 */
	raw_clear(&b, 32768, 4);	/* slice1 */
	raw_clear(&b, 65536, 4);	/* slice2 */
	raw_clear(&b, 98304, 8);	/* slice3 */
	make_cfg(&cfg, &b, &i, 4);
	CHECK(rfs_allocator_init(&a, &cfg) == 0, "ctor ok");
	CHECK(rfs_allocator_block_free_total(&a) == 20, "free 4+4+4+8==20");

	rfs_run_list_init(&runs, g_ralloc);
	CHECK(rfs_allocator_block_alloc_soft(&a, 12, 0, &runs) == 0, "soft rc ok");
	CHECK(sum_runs(&runs) == 12, "soft delivered 12");
	segs = count_runs(&runs);
	CHECK(segs >= 2, "soft >=2 fragments (no single slice has 12)");
	{
		const struct rfs_run_node *n;

		for (n = runs.head; n; n = n->next) {
			u64 k;

			for (k = 0; k < n->run.len; k++)
				CHECK(rfs_allocator_block_test(&a, n->run.base + k) == true,
				      "soft run bits occupied");
		}
	}
	CHECK(rfs_allocator_block_free_total(&a) == 8, "free 20-12==8");
	rfs_run_list_fini(&runs);

	/* 只剩 8 却要 12 → 穷尽所有片仍不够 → 回滚，runs 清空 */
	rfs_run_list_init(&runs, g_ralloc);
	CHECK(rfs_allocator_block_alloc_soft(&a, 12, 0, &runs) == -ENOSPC,
	      "soft short -> -ENOSPC");
	CHECK(runs.head == NULL, "soft rollback: runs emptied");
	CHECK(rfs_allocator_block_free_total(&a) == 8, "soft rollback: 账复原==8");
	rfs_run_list_fini(&runs);

	/* 门闸 */
	rfs_run_list_init(&runs, g_ralloc);
	CHECK(rfs_allocator_block_alloc_soft(&a, 4, 0, NULL) == -EINVAL, "soft nullptr runs");
	CHECK(rfs_allocator_block_alloc_soft(&a, 0, 0, &runs) == -EINVAL, "soft zero count");
	rfs_run_list_fini(&runs);

	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* ══════════════════════════ 4. block_occupy_soft ══════════════════════════ */
static void test_block_occupy_soft(void)
{
	struct raw b, i;
	struct rfs_slices_cfg cfg;
	struct rfs_allocator a;
	u64 rest, k;

	puts("[4] block_occupy_soft：跨片续圈 / 顶片尾记账 / 锚点占用");

	/* 跨片连续空洞 [32760,32776) → 续圈占领 16 */
	raw_init(&b, BITS);
	raw_init(&i, BITS);
	raw_clear(&b, 32760, 16);
	make_cfg(&cfg, &b, &i, 4);
	CHECK(rfs_allocator_init(&a, &cfg) == 0, "ctor ok");
	CHECK(rfs_allocator_block_free_total(&a) == 16, "free 16");
	rest = 999;
	CHECK(rfs_allocator_block_occupy_soft(&a, 32760, 16, &rest) == 0, "occupy cross-slice ok");
	CHECK(rest == 0, "occupy fully satisfied");
	for (k = 0; k < 16; k++)
		CHECK(rfs_allocator_block_test(&a, 32760 + k) == true, "cross-slice bit occupied");
	CHECK(rfs_allocator_block_free_total(&a) == 0, "free drained");
	/* 同连续 run 逆向跨片释放 */
	CHECK(rfs_allocator_block_free(&a, 32760, 16) == 0, "cross-slice free ok");
	CHECK(rfs_allocator_block_free_total(&a) == 16, "free restored 16");
	for (k = 0; k < 16; k++)
		CHECK(rfs_allocator_block_test(&a, 32760 + k) == false, "freed bits free");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);

	/* 只到片尾：占领 8，rest 记账 8 */
	raw_init(&b, BITS);
	raw_init(&i, BITS);
	raw_clear(&b, 32760, 8);
	make_cfg(&cfg, &b, &i, 4);
	CHECK(rfs_allocator_init(&a, &cfg) == 0, "ctor ok");
	rest = 999;
	CHECK(rfs_allocator_block_occupy_soft(&a, 32760, 16, &rest) == 0, "occupy partial-prefix ok");
	CHECK(rest == 8, "rest==8 (stopped at occupied next slice)");
	for (k = 0; k < 8; k++)
		CHECK(rfs_allocator_block_test(&a, 32760 + k) == true, "claimed bits");
	CHECK(rfs_allocator_block_test(&a, 32768) == true, "next slice start untouched");
	CHECK(rfs_allocator_block_free_total(&a) == 0, "free drained");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);

	/* 锚点即占用 → -ENOSPC，rest==require */
	raw_init(&b, BITS);
	raw_init(&i, BITS);
	make_cfg(&cfg, &b, &i, 4);
	CHECK(rfs_allocator_init(&a, &cfg) == 0, "ctor ok");
	rest = 999;
	CHECK(rfs_allocator_block_occupy_soft(&a, 100, 4, &rest) == -ENOSPC,
	      "anchor occupied -> -ENOSPC");
	CHECK(rest == 4, "rest==require");
	CHECK(rfs_allocator_block_occupy_soft(&a, 100, 0, &rest) == -EINVAL, "occupy zero count");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* ══════════════════════════ 5. block_free / intervals_free ══════════════════════════ */
static void test_block_free_chains(void)
{
	struct raw b, i;
	struct rfs_slices_cfg cfg;
	struct rfs_allocator a;
	struct rfs_run_list runs;
	u64 rest, k;

	puts("[5] block_free 跨片 / block_intervals_free 链式");

	raw_init(&b, BITS);
	raw_init(&i, BITS);
	raw_clear(&b, 32760, 16);	/* 跨界连续 16 */
	make_cfg(&cfg, &b, &i, 4);
	CHECK(rfs_allocator_init(&a, &cfg) == 0, "ctor ok");
	CHECK(rfs_allocator_block_free_total(&a) == 16, "free 16");

	rest = 999;
	CHECK(rfs_allocator_block_occupy_soft(&a, 32760, 12, &rest) == 0, "occupy cross-slice 12");
	CHECK(rest == 0, "rest 0");
	CHECK(rfs_allocator_block_free_total(&a) == 4, "free 16-12==4");
	CHECK(rfs_allocator_block_free(&a, 32760, 12) == 0, "block_free cross-slice ok");
	CHECK(rfs_allocator_block_free_total(&a) == 16, "free restored 16");
	for (k = 0; k < 12; k++)
		CHECK(rfs_allocator_block_test(&a, 32760 + k) == false, "freed bits free");
	CHECK(rfs_allocator_block_free(&a, 40000, 0) == 0, "free count==0 no-op");

	/* soft 取一段 → 链式归还（跨片消费）*/
	rfs_run_list_init(&runs, g_ralloc);
	CHECK(rfs_allocator_block_alloc_soft(&a, 8, 0, &runs) == 0, "soft 8");
	CHECK(runs.head != NULL, "soft produced runs");
	CHECK(rfs_allocator_block_free_total(&a) == 8, "free 16-8==8");
	CHECK(rfs_allocator_block_intervals_free(&a, &runs) == 0, "intervals_free ok");
	CHECK(runs.head == NULL, "runs drained");
	CHECK(rfs_allocator_block_free_total(&a) == 16, "free restored via chain");
	CHECK(rfs_allocator_block_intervals_free(&a, NULL) == -EINVAL, "intervals_free nullptr");
	rfs_run_list_fini(&runs);

	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* ══════════════════════════ 6. inode 组 ══════════════════════════ */
static void test_inode(void)
{
	struct raw b, i;
	struct rfs_slices_cfg cfg;
	struct rfs_allocator a;
	u64 n1, n2, lo, hi;

	puts("[6] inode_alloc / inode_free / inode_test");

	raw_init(&b, BITS);
	raw_init(&i, BITS);
	raw_clear(&i, 32770, 1);	/* slice1 */
	raw_clear(&i, 98304, 1);	/* slice3 */
	make_cfg(&cfg, &b, &i, 4);
	CHECK(rfs_allocator_init(&a, &cfg) == 0, "ctor ok");
	CHECK(rfs_allocator_inode_free_total(&a) == 2, "inode free 1+1==2");
	CHECK(rfs_allocator_inode_test(&a, 0) == true, "inode 0 (root) occupied");

	n1 = 0;
	n2 = 0;
	CHECK(rfs_allocator_inode_alloc(&a, &n1) == 0, "inode_alloc #1");
	CHECK(rfs_allocator_inode_alloc(&a, &n2) == 0, "inode_alloc #2");
	lo = n1 < n2 ? n1 : n2;
	hi = n1 < n2 ? n2 : n1;
	CHECK(lo == 32770 && hi == 98304, "inode cross-slice set {32770,98304}");
	CHECK(rfs_allocator_inode_test(&a, 32770) == true, "32770 occupied");
	CHECK(rfs_allocator_inode_test(&a, 98304) == true, "98304 occupied");
	CHECK(rfs_allocator_inode_free_total(&a) == 0, "inode free drained");

	rfs_allocator_inode_free(&a, 32770);
	CHECK(rfs_allocator_inode_test(&a, 32770) == false, "32770 freed");
	CHECK(rfs_allocator_inode_free_total(&a) == 1, "inode free restored 1");
	CHECK(rfs_allocator_inode_alloc(&a, &n1) == 0 && n1 == 32770, "re-alloc 32770");
	rfs_allocator_inode_free(&a, 98304);
	rfs_allocator_inode_free(&a, 32770);
	CHECK(rfs_allocator_inode_free_total(&a) == 2, "inode free restored 2");

	/* 越界 test */
	CHECK(rfs_allocator_inode_test(&a, BITS + 5) == false, "inode_test OOB false");
	CHECK(rfs_allocator_block_test(&a, BITS + 5) == false, "block_test OOB false");
	CHECK(rfs_allocator_block_preferred_of(&a, 99) == 0, "preferred_of OOB -> 0");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);

	/* 全占用 → inode NO_BLOCK */
	raw_init(&b, BITS);
	raw_init(&i, BITS);
	make_cfg(&cfg, &b, &i, 4);
	CHECK(rfs_allocator_init(&a, &cfg) == 0, "ctor full ok");
	n1 = 0;
	CHECK(rfs_allocator_inode_alloc(&a, &n1) == -ENOSPC, "inode full -> -ENOSPC");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* ══════════════════════════ 7. dump_slice 快照 ══════════════════════════ */
static void test_dump_slice(void)
{
	struct raw b, i;
	struct rfs_slices_cfg cfg;
	struct rfs_allocator a;
	struct rfs_run got[8], want[4];

	puts("[7] dump_slice：逐片快照与手工位图一致");

	raw_init(&b, BITS);
	raw_init(&i, BITS);
	raw_clear(&b, 0, 10);		/* slice0 */
	raw_clear(&b, 32760, 16);	/* slice0 尾 + slice1 首（跨界）*/
	raw_clear(&b, 65536, 4);	/* slice2 */
	raw_clear(&b, 100000, 8);	/* slice3 */
	raw_clear(&i, 5, 1);		/* slice0 */
	make_cfg(&cfg, &b, &i, 4);
	CHECK(rfs_allocator_init(&a, &cfg) == 0, "ctor ok");
	CHECK(rfs_allocator_block_free_total(&a) == 38, "block free 10+16+4+8==38");
	CHECK(rfs_allocator_inode_free_total(&a) == 1, "inode free 1");

	/* slice0: [0,10) + [32760,32768) */
	want[0].base = 0; want[0].len = 10;
	want[1].base = 32760; want[1].len = 8;
	expect_runs(got, dump_slice(&a, false, 0, got, 8), want, 2, "dump block slice0");
	/* slice1: [32768,32776) */
	want[0].base = 32768; want[0].len = 8;
	expect_runs(got, dump_slice(&a, false, 1, got, 8), want, 1, "dump block slice1");
	/* slice2: [65536,65540) */
	want[0].base = 65536; want[0].len = 4;
	expect_runs(got, dump_slice(&a, false, 2, got, 8), want, 1, "dump block slice2");
	/* slice3: [100000,100008) */
	want[0].base = 100000; want[0].len = 8;
	expect_runs(got, dump_slice(&a, false, 3, got, 8), want, 1, "dump block slice3");

	/* inode: slice0 [5,6)，其余空 */
	want[0].base = 5; want[0].len = 1;
	expect_runs(got, dump_slice(&a, true, 0, got, 8), want, 1, "dump inode slice0");
	expect_runs(got, dump_slice(&a, true, 1, got, 8), want, 0, "dump inode slice1 empty");
	expect_runs(got, dump_slice(&a, true, 2, got, 8), want, 0, "dump inode slice2 empty");
	expect_runs(got, dump_slice(&a, true, 3, got, 8), want, 0, "dump inode slice3 empty");

	/* 门闸 */
	CHECK(rfs_allocator_dump_slice(&a, false, 4, NULL) == -EINVAL, "dump NULL out");
	{
		struct rfs_run_dlist dl;

		rfs_run_dlist_init(&dl, g_ralloc);
		CHECK(rfs_allocator_dump_slice(&a, false, 4, &dl) == -EINVAL, "dump OOB slice");
		CHECK(rfs_allocator_dump_slice(NULL, false, 0, &dl) == -EINVAL, "dump NULL self");
		rfs_run_dlist_fini(&dl);
	}

	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

int main(void)
{
	printf("=== ravenfs_allocator_router smoke tests ===\n");

	test_init_geometry();
	test_block_alloc_hard();
	test_block_alloc_soft();
	test_block_occupy_soft();
	test_block_free_chains();
	test_inode();
	test_dump_slice();

	printf("--- pool: allocs=%llu frees=%llu live=%llu\n",
	       (unsigned long long)g_allocs, (unsigned long long)g_frees,
	       (unsigned long long)g_live);
	CHECK(g_live == 0, "leak: live == 0");
	CHECK(g_allocs == g_frees, "leak: allocs == frees");

	printf("=== PASS %d  FAIL %d ===\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

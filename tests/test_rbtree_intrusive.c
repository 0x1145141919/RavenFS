/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_rbtree_intrusive.c —— ravenfs_rbtree_intrusive 用户态验证（差分 + 不变量 + 边界 + 双索引）
 *
 * 直接编译 ../linux/ravenfs_rbtree_intrusive.c（不链 god/ko 其它任何东西）。
 *   差分：轮子 vs「有序数组」参考实现，随机 insert/erase/find/lower_bound/
 *         upper_bound 逐操作对拍（≥2×10^5 操作），每 512 步跑 rfs_rb_check，
 *         每 4096 步全量中序比对。
 *   不变量：每步随机 erase 后仍 0；大量建树后逐个 erase 到空仍 0。
 *   边界：空树 first/last/next/prev/lower/upper；erase root/叶/单子/双子；
 *         重复插入（-EEXIST）；双索引（同一元素挂两棵树）。
 *   分配：验证「本头已无分配接口、轮子零直接分配」——元素块全由**测试自己**
 *         （test_alloc/test_free）管理，收尾时 live==0 且 allocs==frees；
 *         并以编译期负向断言坐实 rfs_rb_alloc / rfs_rb_alloc_node / rfs_rb_free_node
 *         已从本头消失（若复活则本测试编译失败）。
 */
#include "ravenfs_rbtree_intrusive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════ 计数 / 断言 ══════════════════════════ */
static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

/* ══════════════════════════ 元素 / 比较器 ══════════════════════════ */
struct elem {
	u64		 key;
	u64		 len;
	struct rfs_rb_node node;	/* 主索引：by key */
	struct rfs_rb_node node2;	/* 从索引：by (len,key) —— 同元素双挂 */
};

static int cmp_key(const struct rfs_rb_node *a, const struct rfs_rb_node *b,
		   void *ctx)
{
	const struct elem *x = container_of(a, struct elem, node);
	const struct elem *y = container_of(b, struct elem, node);

	(void)ctx;
	if (x->key < y->key)
		return -1;
	if (x->key > y->key)
		return 1;
	return 0;
}

static int cmp_size(const struct rfs_rb_node *a, const struct rfs_rb_node *b,
		    void *ctx)
{
	const struct elem *x = container_of(a, struct elem, node2);
	const struct elem *y = container_of(b, struct elem, node2);

	(void)ctx;
	if (x->len != y->len)
		return (x->len < y->len) ? -1 : 1;
	if (x->key != y->key)
		return (x->key < y->key) ? -1 : 1;
	return 0;
}

/* ══════════════════════════ 元素内存（调用方自管，轮子无分配接口） ══════════════════════════ */
/* 说明：intrusive 头已删除分配接口；元素块由测试自己取/还（演示"零分配接口 + 调用方管理"）。*/
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

/*
 * 编译期负向断言：本头必须已删除分配接口。
 *   · 若 rfs_rb_alloc_node / rfs_rb_free_node 仍在（函数），下面同名对象声明会
 *     与之冲突 → 重复声明/redeclared as different kind of symbol → 编译失败。
 *   · 若 struct rfs_rb_alloc 仍在，下面自建同名 struct 会重定义 → 编译失败。
 * 两者均编译通过 ⇒ 本头确无分配接口。
 */
struct rfs_rb_alloc { int __must_not_exist__; };
static int rfs_rb_alloc_node __attribute__((unused));
static int rfs_rb_free_node __attribute__((unused));

static void test_no_alloc_interface(void)
{
	CHECK((void *)&rfs_rb_alloc_node != NULL,
	      "no-alloc: rfs_rb_alloc_node absent (compile-time)");
	CHECK((void *)&rfs_rb_free_node != NULL,
	      "no-alloc: rfs_rb_free_node absent (compile-time)");
	CHECK(sizeof(struct rfs_rb_alloc) == sizeof(int),
	      "no-alloc: struct rfs_rb_alloc absent (compile-time)");
}

/* ══════════════════════════ PRNG ══════════════════════════ */
static u64 rnd(u64 *s)
{
	u64 x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*s = x;
	return x;
}

/* ══════════════════════════ 树工具 ══════════════════════════ */
static int tree_size(const struct rfs_rb_root *t)
{
	int n = 0;
	struct rfs_rb_node *it;

	for (it = rfs_rb_first(t); it; it = rfs_rb_next(it))
		n++;
	return n;
}

/* 中序收集 key 到 buf，返回个数。*/
static int tree_keys(const struct rfs_rb_root *t, u64 *buf, int cap)
{
	int n = 0;
	struct rfs_rb_node *it;

	for (it = rfs_rb_first(t); it; it = rfs_rb_next(it)) {
		if (n < cap)
			buf[n] = container_of(it, struct elem, node)->key;
		n++;
	}
	return n;
}

/* ══════════════════════════ 参考实现：有序数组 ══════════════════════════ */
#define MAXLIVE	4096
#define KEYSPACE 1000000u
static u64		ref_keys[MAXLIVE];
static struct elem     *ref_ptr[MAXLIVE];
static int		ref_n;

static int ref_lower(u64 k)
{
	int lo = 0, hi = ref_n;

	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (ref_keys[mid] < k)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static int ref_upper(u64 k)
{
	int lo = 0, hi = ref_n;

	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (ref_keys[mid] <= k)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static int ref_find(u64 k)
{
	int i = ref_lower(k);

	return (i < ref_n && ref_keys[i] == k) ? i : -1;
}

static void ref_insert(u64 key, struct elem *e)
{
	int pos = ref_lower(key);

	memmove(&ref_keys[pos + 1], &ref_keys[pos],
		(size_t)(ref_n - pos) * sizeof(u64));
	memmove(&ref_ptr[pos + 1], &ref_ptr[pos],
		(size_t)(ref_n - pos) * sizeof(struct elem *));
	ref_keys[pos] = key;
	ref_ptr[pos] = e;
	ref_n++;
}

static void ref_erase(int idx)
{
	memmove(&ref_keys[idx], &ref_keys[idx + 1],
		(size_t)(ref_n - idx - 1) * sizeof(u64));
	memmove(&ref_ptr[idx], &ref_ptr[idx + 1],
		(size_t)(ref_n - idx - 1) * sizeof(struct elem *));
	ref_n--;
}

/* ══════════════════════════ 1. 空树边界 ══════════════════════════ */
static void test_empty(void)
{
	struct rfs_rb_root t;
	struct elem probe;

	probe.key = 5;
	rfs_rb_init(&t);
	CHECK(rfs_rb_empty(&t), "empty: empty()");
	CHECK(rfs_rb_first(&t) == NULL, "empty: first NULL");
	CHECK(rfs_rb_last(&t) == NULL, "empty: last NULL");
	CHECK(rfs_rb_next(NULL) == NULL, "empty: next(NULL) NULL");
	CHECK(rfs_rb_prev(NULL) == NULL, "empty: prev(NULL) NULL");
	CHECK(rfs_rb_lower_bound(&t, &probe.node, cmp_key, NULL) == NULL, "empty: lower_bound");
	CHECK(rfs_rb_upper_bound(&t, &probe.node, cmp_key, NULL) == NULL, "empty: upper_bound");
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "empty: check == 0");
}

/* ══════════════════════════ 2. 升序建树 + 导航 ══════════════════════════ */
#define ORD_N 1000
static struct elem ord_e[ORD_N];

static void test_ordered(void)
{
	struct rfs_rb_root t;
	struct rfs_rb_node *it;
	int i, n;
	u64 prev;

	rfs_rb_init(&t);
	for (i = 0; i < ORD_N; i++) {
		ord_e[i].key = (u64)i * 3;
		int rc = rfs_rb_insert(&t, &ord_e[i].node, cmp_key, NULL);
		CHECK(rc == 0, "ordered: insert rc==0");
	}
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "ordered: check == 0");
	CHECK(tree_size(&t) == ORD_N, "ordered: size");
	CHECK(container_of(rfs_rb_first(&t), struct elem, node)->key == 0,
	      "ordered: first key 0");
	CHECK(container_of(rfs_rb_last(&t), struct elem, node)->key ==
	      (u64)(ORD_N - 1) * 3, "ordered: last key max");

	/* 中序严格递增 */
	n = 0;
	prev = 0;
	for (it = rfs_rb_first(&t); it; it = rfs_rb_next(it)) {
		u64 k = container_of(it, struct elem, node)->key;

		if (n > 0 && k <= prev) {
			CHECK(0, "ordered: strictly increasing");
			break;
		}
		prev = k;
		n++;
	}
	CHECK(n == ORD_N, "ordered: forward walk count");
	CHECK(rfs_rb_next(rfs_rb_last(&t)) == NULL, "ordered: next(last)==NULL");
	CHECK(rfs_rb_prev(rfs_rb_first(&t)) == NULL, "ordered: prev(first)==NULL");

	/* 反向遍历 */
	n = 0;
	for (it = rfs_rb_last(&t); it; it = rfs_rb_prev(it))
		n++;
	CHECK(n == ORD_N, "ordered: backward walk count");
}

/* ══════════════════════════ 3. lower/upper_bound 语义 ══════════════════════════ */
static struct elem bnd_e[3];

static void test_bounds(void)
{
	struct rfs_rb_root t;
	struct elem probe;
	u64 ks[3] = { 10, 20, 30 };
	int i;

	rfs_rb_init(&t);
	for (i = 0; i < 3; i++) {
		bnd_e[i].key = ks[i];
		rfs_rb_insert(&t, &bnd_e[i].node, cmp_key, NULL);
	}

	probe.key = 20;	/* 命中 */
	CHECK(container_of(rfs_rb_lower_bound(&t, &probe.node, cmp_key, NULL),
			   struct elem, node)->key == 20, "bounds: lb(20)==20");
	CHECK(container_of(rfs_rb_upper_bound(&t, &probe.node, cmp_key, NULL),
			   struct elem, node)->key == 30, "bounds: ub(20)==30");

	probe.key = 25;	/* 未命中 */
	CHECK(container_of(rfs_rb_lower_bound(&t, &probe.node, cmp_key, NULL),
			   struct elem, node)->key == 30, "bounds: lb(25)==30");
	CHECK(container_of(rfs_rb_upper_bound(&t, &probe.node, cmp_key, NULL),
			   struct elem, node)->key == 30, "bounds: ub(25)==30");

	probe.key = 5;	/* 全部更大 */
	CHECK(container_of(rfs_rb_lower_bound(&t, &probe.node, cmp_key, NULL),
			   struct elem, node)->key == 10, "bounds: lb(5)==10");
	CHECK(container_of(rfs_rb_upper_bound(&t, &probe.node, cmp_key, NULL),
			   struct elem, node)->key == 10, "bounds: ub(5)==10");

	probe.key = 35;	/* 全部更小 */
	CHECK(rfs_rb_lower_bound(&t, &probe.node, cmp_key, NULL) == NULL, "bounds: lb(35)==NULL");
	CHECK(rfs_rb_upper_bound(&t, &probe.node, cmp_key, NULL) == NULL, "bounds: ub(35)==NULL");
}

/* ══════════════════════════ 4. 重复插入（键唯一） ══════════════════════════ */
static struct elem dup_e[3];

static void test_duplicate(void)
{
	struct rfs_rb_root t;
	int rc;

	rfs_rb_init(&t);
	dup_e[0].key = 50;
	rc = rfs_rb_insert(&t, &dup_e[0].node, cmp_key, NULL);
	CHECK(rc == 0, "dup: first insert ok");

	dup_e[1].key = 50;	/* 等键 */
	rc = rfs_rb_insert(&t, &dup_e[1].node, cmp_key, NULL);
	CHECK(rc == -EEXIST, "dup: second insert -EEXIST");
	CHECK(tree_size(&t) == 1, "dup: size still 1");
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "dup: check == 0");

	dup_e[2].key = 51;
	rc = rfs_rb_insert(&t, &dup_e[2].node, cmp_key, NULL);
	CHECK(rc == 0, "dup: neighbor insert ok");
	CHECK(tree_size(&t) == 2, "dup: size 2");
}

/* ══════════════════════════ 5. erase 形态（根/叶/单子/双子） ══════════════════════════ */
static struct elem shp_e[7];

static int shp_insert(struct rfs_rb_root *t, u64 key)
{
	shp_e[key].key = key;
	return rfs_rb_insert(t, &shp_e[key].node, cmp_key, NULL);
}

static void test_erase_shapes(void)
{
	struct rfs_rb_root t;

	/* 单节点树 */
	rfs_rb_init(&t);
	CHECK(shp_insert(&t, 3) == 0, "shape: single insert");
	rfs_rb_erase(&t, &shp_e[3].node);
	CHECK(rfs_rb_empty(&t), "shape: single erase -> empty");
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "shape: single check");

	/* 根 + 两孩子（keys 2,1,3）：erase 双子根 */
	rfs_rb_init(&t);
	shp_insert(&t, 2);
	shp_insert(&t, 1);
	shp_insert(&t, 3);
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "shape: 3-node check");
	rfs_rb_erase(&t, &shp_e[2].node);	/* 根、双子 */
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "shape: erase 2-child root check");
	CHECK(tree_size(&t) == 2, "shape: size after root erase");

	/* 叶与单子：0..6 全插，erase 叶(0)、单子(1) */
	rfs_rb_init(&t);
	for (u64 k = 0; k < 7; k++)
		CHECK(shp_insert(&t, k) == 0, "shape: insert 0..6");
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "shape: 7-node check");
	rfs_rb_erase(&t, &shp_e[0].node);	/* 叶 */
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "shape: erase leaf check");
	rfs_rb_erase(&t, &shp_e[6].node);	/* 叶 */
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "shape: erase leaf2 check");
	rfs_rb_erase(&t, &shp_e[3].node);	/* 内部双子 */
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "shape: erase internal check");
	CHECK(tree_size(&t) == 4, "shape: size 4");

	/* 清空 */
	for (u64 k = 1; k <= 5; k++) {
		if (k == 3)
			continue;
		rfs_rb_erase(&t, &shp_e[k].node);
		CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "shape: drain check");
	}
	CHECK(rfs_rb_empty(&t), "shape: drained empty");
}

/* ══════════════════════════ 6. 双索引（同一元素挂两棵树） ══════════════════════════ */
static struct elem dual_e[4];

static void test_dual_index(void)
{
	struct rfs_rb_root tpos, tsize;
	struct rfs_rb_node *it;
	int i, n;
	u64 last;

	rfs_rb_init(&tpos);
	rfs_rb_init(&tsize);
	for (i = 0; i < 4; i++) {
		dual_e[i].key = (u64)(i + 1) * 10;	/* 10,20,30,40 */
		dual_e[i].len = (u64)(4 - i);		/* 4,3,2,1 */
		CHECK(rfs_rb_insert(&tpos, &dual_e[i].node, cmp_key, NULL) == 0,
		      "dual: insert by pos");
		CHECK(rfs_rb_insert(&tsize, &dual_e[i].node2, cmp_size, NULL) == 0,
		      "dual: insert by size");
	}
	CHECK(rfs_rb_check(&tpos, cmp_key, NULL) == 0, "dual: pos check");
	CHECK(rfs_rb_check(&tsize, cmp_size, NULL) == 0, "dual: size check");

	/* by_size 升序 = len 1,2,3,4 → key 40,30,20,10 */
	last = 0;
	n = 0;
	for (it = rfs_rb_first(&tsize); it; it = rfs_rb_next(it)) {
		u64 len = container_of(it, struct elem, node2)->len;

		CHECK(len > last, "dual: size order increasing");
		last = len;
		n++;
	}
	CHECK(n == 4, "dual: size tree count");

	/* 从从树摘、再从主树摘（模拟主从「先从后主」拆挂） */
	rfs_rb_erase(&tsize, &dual_e[1].node2);
	CHECK(rfs_rb_check(&tsize, cmp_size, NULL) == 0, "dual: size check after erase");
	CHECK(rfs_rb_check(&tpos, cmp_key, NULL) == 0, "dual: pos unaffected");
	rfs_rb_erase(&tpos, &dual_e[1].node);
	CHECK(rfs_rb_check(&tpos, cmp_key, NULL) == 0, "dual: pos check after erase");
	CHECK(tree_size(&tpos) == 3 && tree_size(&tsize) == 3, "dual: both size 3");
}

/* ══════════════════════════ 7. 大建树 → 随机逐个 erase 到空 ══════════════════════════ */
#define BIG_N 100000
static struct elem big_e[BIG_N];
static int big_perm[BIG_N];

static void test_big_drain(void)
{
	struct rfs_rb_root t;
	u64 rng = 0xdeadbeefcafe1234ULL;
	int i;

	rfs_rb_init(&t);
	for (i = 0; i < BIG_N; i++) {
		big_e[i].key = (u64)i;
		if (rfs_rb_insert(&t, &big_e[i].node, cmp_key, NULL) != 0) {
			CHECK(0, "big: insert");
			return;
		}
	}
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "big: check after build");
	CHECK(tree_size(&t) == BIG_N, "big: size after build");

	for (i = 0; i < BIG_N; i++)
		big_perm[i] = i;
	for (i = BIG_N - 1; i > 0; i--) {	/* Fisher-Yates */
		int j = (int)(rnd(&rng) % (u64)(i + 1));
		int tmp = big_perm[i];

		big_perm[i] = big_perm[j];
		big_perm[j] = tmp;
	}
	for (i = 0; i < BIG_N; i++) {
		rfs_rb_erase(&t, &big_e[big_perm[i]].node);
		if ((i & 1023) == 0) {
			if (rfs_rb_check(&t, cmp_key, NULL) != 0) {
				CHECK(0, "big: check during drain");
				return;
			}
		}
	}
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "big: check after drain");
	CHECK(rfs_rb_empty(&t), "big: empty after drain");
}

/* ══════════════════════════ 8. 随机差分（核心） ══════════════════════════ */

static void do_insert(struct rfs_rb_root *t, u64 key, u64 len)
{
	struct elem *e;
	int rc;

	if (ref_n >= MAXLIVE || ref_find(key) >= 0)
		return;
	e = (struct elem *)test_alloc(NULL, (u64)sizeof(*e));
	if (!e) {
		CHECK(0, "diff: alloc failed");
		return;
	}
	e->key = key;
	e->len = len;
	rc = rfs_rb_insert(t, &e->node, cmp_key, NULL);
	if (rc != 0) {
		test_free(NULL, e);
		CHECK(0, "diff: insert rc");
		return;
	}
	ref_insert(key, e);
}

static void do_erase(struct rfs_rb_root *t, int idx)
{
	struct elem *e = ref_ptr[idx];

	rfs_rb_erase(t, &e->node);
	ref_erase(idx);
	test_free(NULL, e);
}

static void cmp_lb(struct rfs_rb_root *t, u64 k)
{
	struct elem probe;
	struct rfs_rb_node *got;
	int idx;

	probe.key = k;
	got = rfs_rb_lower_bound(t, &probe.node, cmp_key, NULL);
	idx = ref_lower(k);
	if (idx < ref_n) {
		CHECK(got == &ref_ptr[idx]->node, "diff: lower_bound");
	} else {
		CHECK(got == NULL, "diff: lower_bound NULL");
	}
}

static void cmp_ub(struct rfs_rb_root *t, u64 k)
{
	struct elem probe;
	struct rfs_rb_node *got;
	int idx;

	probe.key = k;
	got = rfs_rb_upper_bound(t, &probe.node, cmp_key, NULL);
	idx = ref_upper(k);
	if (idx < ref_n) {
		CHECK(got == &ref_ptr[idx]->node, "diff: upper_bound");
	} else {
		CHECK(got == NULL, "diff: upper_bound NULL");
	}
}

static void cmp_find(struct rfs_rb_root *t, u64 k)
{
	struct elem probe;
	struct rfs_rb_node *got;
	int idx = ref_find(k);

	probe.key = k;
	got = rfs_rb_lower_bound(t, &probe.node, cmp_key, NULL);	/* lookup = lb + 相等判定 */
	if (idx >= 0)
		CHECK(got == &ref_ptr[idx]->node, "diff: find hit");
	else
		CHECK(got == NULL || container_of(got, struct elem, node)->key != k,
		      "diff: find miss");
}

static void cmp_full_traversal(struct rfs_rb_root *t)
{
	static u64 keys[MAXLIVE];
	int n = tree_keys(t, keys, MAXLIVE);
	int i;

	CHECK(n == ref_n, "diff: traversal count == ref");
	if (n != ref_n)
		return;
	for (i = 0; i < n; i++) {
		if (keys[i] != ref_keys[i]) {
			CHECK(0, "diff: traversal key mismatch");
			return;
		}
	}
	CHECK(1, "diff: traversal matches ref");
}

static void test_random_diff(void)
{
	struct rfs_rb_root t;
	const int OPS = 200000;
	u64 rng = 0x9e3779b97f4a7c15ULL;
	int i;

	rfs_rb_init(&t);
	ref_n = 0;

	for (i = 0; i < OPS; i++) {
		u64 r = rnd(&rng);
		u64 k = rnd(&rng) % KEYSPACE;
		int op = (int)(r % 100);

		if (op < 50) {
			do_insert(&t, k, k * 2654435761u);
		} else if (op < 78) {
			if (ref_n > 0)
				do_erase(&t, (int)(rnd(&rng) % (u64)ref_n));
		} else if (op < 88) {
			cmp_lb(&t, k);
		} else if (op < 95) {
			cmp_ub(&t, k);
		} else {
			cmp_find(&t, k);
		}

		if ((i & 511) == 0) {
			if (rfs_rb_check(&t, cmp_key, NULL) != 0) {
				CHECK(0, "diff: rb_check during ops");
				break;
			}
		}
		if ((i & 4095) == 0)
			cmp_full_traversal(&t);
	}

	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "diff: final rb_check");
	cmp_full_traversal(&t);

	/* 全清空 → 空树、无泄漏 */
	while (ref_n > 0)
		do_erase(&t, ref_n - 1);
	CHECK(rfs_rb_empty(&t), "diff: empty after drain");
	CHECK(rfs_rb_check(&t, cmp_key, NULL) == 0, "diff: check empty");
	CHECK(tree_size(&t) == 0, "diff: size 0");
}

/* ══════════════════════════ main ══════════════════════════ */
int main(void)
{
	printf("=== ravenfs_rbtree_intrusive user-space tests ===\n");

	test_no_alloc_interface();
	test_empty();
	test_ordered();
	test_bounds();
	test_duplicate();
	test_erase_shapes();
	test_dual_index();
	test_big_drain();
	test_random_diff();

	printf("--- allocator accounting: allocs=%llu frees=%llu live=%llu\n",
	       (unsigned long long)g_allocs, (unsigned long long)g_frees,
	       (unsigned long long)g_live);
	CHECK(g_live == 0, "leak: live == 0");
	CHECK(g_allocs == g_frees, "leak: allocs == frees");

	printf("=== PASS %d  FAIL %d ===\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

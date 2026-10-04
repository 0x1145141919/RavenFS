/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_rbtree_owning.c —— ravenfs_rbtree_owning 用户态验证
 *
 * 直接编译 ../linux/ravenfs_rbtree_owning.c + ../linux/ravenfs_rbtree_intrusive.c
 * （owning 复用 intrusive 核；不链 god/ko 其它任何东西）。
 *
 * 覆盖：
 *   差分：owning 轮子 vs「有序数组」参考实现，随机 insert/erase/find/遍历逐操作对拍
 *         （≥2×10^5 操作），每 512 步校验「size + 中序严格递增」，每 4096 步全量比对。
 *   拷贝语义：insert 拷贝调用方元素（改原元素不影响树内副本）。
 *   迭代：first/next 中序严格递增、计数 == size。
 *   重复插入：-EEXIST 且树不动。
 *   删除不存在：-ENOENT 且树不动。
 *   随机插入/删除到空：逐个 erase 到空，size 归零、first==NULL。
 *   分配账：owning 经堆回调分配，收尾 live==0 且 allocs==frees；fini 后仍平衡。
 */
#include "ravenfs_rbtree_owning.h"

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
struct oelem {
	u64 key;
	u64 tag;
};

static int oelem_cmp(const void *a, const void *b, void *ctx)
{
	const struct oelem *x = (const struct oelem *)a;
	const struct oelem *y = (const struct oelem *)b;

	(void)ctx;
	if (x->key < y->key)
		return -1;
	if (x->key > y->key)
		return 1;
	return 0;
}

/* ══════════════════════════ 堆回调（计数） ══════════════════════════ */
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

static struct rfs_rbo_alloc g_alloc = { test_alloc, test_free, NULL };

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

/* ══════════════════════════ 参考实现：有序数组（键） ══════════════════════════ */
#define MAXLIVE	4096
#define KEYSPACE 1000000u
static u64 ref_keys[MAXLIVE];
static int ref_n;

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

static int ref_find(u64 k)
{
	int i = ref_lower(k);

	return (i < ref_n && ref_keys[i] == k) ? i : -1;
}

static void ref_insert(u64 key)
{
	int pos = ref_lower(key);

	memmove(&ref_keys[pos + 1], &ref_keys[pos],
		(size_t)(ref_n - pos) * sizeof(u64));
	ref_keys[pos] = key;
	ref_n++;
}

static void ref_erase(int idx)
{
	memmove(&ref_keys[idx], &ref_keys[idx + 1],
		(size_t)(ref_n - idx - 1) * sizeof(u64));
	ref_n--;
}

/* ══════════════════════════ 遍历工具 ══════════════════════════ */

/* 中序遍历：校验严格递增，返回个数；keys 可选（cap 限制）。*/
static int walk_strict(const struct rfs_rbo_root *t, int *sorted_ok)
{
	const void *e;
	u64 prev = 0;
	int n = 0;

	*sorted_ok = 1;
	for (e = rfs_rbo_first(t); e; e = rfs_rbo_next(e)) {
		u64 k = ((const struct oelem *)e)->key;

		if (n > 0 && k <= prev)
			*sorted_ok = 0;
		prev = k;
		n++;
	}
	return n;
}

static int walk_keys(const struct rfs_rbo_root *t, u64 *buf, int cap)
{
	const void *e;
	int n = 0;

	for (e = rfs_rbo_first(t); e; e = rfs_rbo_next(e)) {
		if (n < cap)
			buf[n] = ((const struct oelem *)e)->key;
		n++;
	}
	return n;
}

/* ══════════════════════════ 1. 空树 / 参数契约 ══════════════════════════ */
static void test_empty_and_args(void)
{
	struct rfs_rbo_root t;
	struct oelem probe = { 5, 0 };
	int rc;

	/* 合法 init */
	rc = rfs_rbo_init(&t, g_alloc, sizeof(struct oelem), oelem_cmp, NULL);
	CHECK(rc == 0, "empty: init == 0");
	CHECK(rfs_rbo_size(&t) == 0, "empty: size 0");
	CHECK(rfs_rbo_first(&t) == NULL, "empty: first NULL");
	CHECK(rfs_rbo_next(NULL) == NULL, "empty: next(NULL) NULL");
	CHECK(rfs_rbo_find(&t, &probe) == NULL, "empty: find NULL");
	CHECK(rfs_rbo_erase(&t, &probe) == -ENOENT, "empty: erase -ENOENT");
	rfs_rbo_fini(&t);
	CHECK(rfs_rbo_size(&t) == 0, "empty: size 0 after fini");

	/* 非法参数 */
	{
		struct rfs_rbo_alloc bad = { NULL, test_free, NULL };

		CHECK(rfs_rbo_init(NULL, g_alloc, 8, oelem_cmp, NULL) == -EINVAL,
		      "args: t NULL -> -EINVAL");
		CHECK(rfs_rbo_init(&t, g_alloc, 0, oelem_cmp, NULL) == -EINVAL,
		      "args: elem_size 0 -> -EINVAL");
		CHECK(rfs_rbo_init(&t, g_alloc, 8, NULL, NULL) == -EINVAL,
		      "args: cmp NULL -> -EINVAL");
		CHECK(rfs_rbo_init(&t, bad, 8, oelem_cmp, NULL) == -EINVAL,
		      "args: alloc NULL -> -EINVAL");
	}
}

/* ══════════════════════════ 2. 升序建树 + 迭代 + 拷贝语义 ══════════════════════════ */
#define ORD_N 1000
static void test_ordered_and_copy(void)
{
	struct rfs_rbo_root t;
	struct oelem e;
	u64 k;
	int i, n, sorted;
	u64 seen[ORD_N];

	rfs_rbo_init(&t, g_alloc, sizeof(struct oelem), oelem_cmp, NULL);
	for (i = 0; i < ORD_N; i++) {
		e.key = (u64)i * 3;
		e.tag = (u64)i;
		CHECK(rfs_rbo_insert(&t, &e) == 0, "ordered: insert rc==0");
	}
	CHECK(rfs_rbo_size(&t) == ORD_N, "ordered: size");
	CHECK(((const struct oelem *)rfs_rbo_first(&t))->key == 0,
	      "ordered: first key 0");

	n = walk_strict(&t, &sorted);
	CHECK(sorted, "ordered: in-order strictly increasing");
	CHECK(n == ORD_N, "ordered: walk count");

	/* walk_keys 与期望一致 */
	n = walk_keys(&t, seen, ORD_N);
	CHECK(n == ORD_N, "ordered: keys count");
	for (i = 0; i < ORD_N; i++) {
		if (seen[i] != (u64)i * 3) {
			CHECK(0, "ordered: key mismatch");
			break;
		}
	}
	CHECK(1, "ordered: keys match");

	/* find 命中 / 未命中 */
	for (i = 0; i < ORD_N; i++) {
		struct oelem probe = { (u64)i * 3, 0 };

		if (!rfs_rbo_find(&t, &probe)) {
			CHECK(0, "ordered: find hit");
			break;
		}
	}
	CHECK(1, "ordered: all hits");
	k = 1;
	{
		struct oelem miss = { k, 0 };

		CHECK(rfs_rbo_find(&t, &miss) == NULL, "ordered: find miss");
	}

	/* 拷贝语义：insert 拷贝，改原元素不影响树内副本 */
	e.key = 7777;
	e.tag = 7777;
	CHECK(rfs_rbo_insert(&t, &e) == 0, "copy: insert 7777");
	e.key = 9999;			/* 改调用方元素 */
	{
		struct oelem p7777 = { 7777, 0 };
		struct oelem p9999 = { 9999, 0 };
		const struct oelem *got = (const struct oelem *)rfs_rbo_find(&t, &p7777);

		CHECK(got != NULL && got->key == 7777 && got->tag == 7777,
		      "copy: tree keeps inserted copy");
		CHECK(rfs_rbo_find(&t, &p9999) == NULL, "copy: mutation not leaked");
	}

	rfs_rbo_fini(&t);
	CHECK(rfs_rbo_size(&t) == 0, "ordered: empty after fini");
}

/* ══════════════════════════ 3. 重复插入 / 删除不存在 ══════════════════════════ */
static void test_dup_and_missing(void)
{
	struct rfs_rbo_root t;
	struct oelem a = { 50, 1 }, b = { 50, 2 }, c = { 51, 3 }, d = { 49, 4 };
	struct oelem miss = { 12345, 0 };
	int rc;

	rfs_rbo_init(&t, g_alloc, sizeof(struct oelem), oelem_cmp, NULL);

	CHECK(rfs_rbo_insert(&t, &a) == 0, "dup: first insert ok");
	rc = rfs_rbo_insert(&t, &b);	/* 等键 */
	CHECK(rc == -EEXIST, "dup: second insert -EEXIST");
	CHECK(rfs_rbo_size(&t) == 1, "dup: size still 1");
	/* 树内保留的是 a 的副本，不是 b */
	{
		const struct oelem *got = (const struct oelem *)rfs_rbo_find(&t, &b);

		CHECK(got != NULL && got->tag == 1, "dup: tree keeps first copy");
	}

	CHECK(rfs_rbo_insert(&t, &c) == 0, "dup: neighbor insert ok");
	CHECK(rfs_rbo_insert(&t, &d) == 0, "dup: neighbor insert ok");
	CHECK(rfs_rbo_size(&t) == 3, "dup: size 3");

	CHECK(rfs_rbo_erase(&t, &miss) == -ENOENT, "missing: erase -ENOENT");
	CHECK(rfs_rbo_size(&t) == 3, "missing: size unchanged");

	CHECK(rfs_rbo_erase(&t, &a) == 0, "missing: erase hit ok");
	CHECK(rfs_rbo_size(&t) == 2, "missing: size 2");
	CHECK(rfs_rbo_find(&t, &a) == NULL, "missing: gone after erase");

	rfs_rbo_fini(&t);
}

/* ══════════════════════════ 4. 随机差分（核心） ══════════════════════════ */
static void do_insert(struct rfs_rbo_root *t, u64 key, u64 tag)
{
	struct oelem e;
	int rc;

	if (ref_n >= MAXLIVE || ref_find(key) >= 0)
		return;
	e.key = key;
	e.tag = tag;
	rc = rfs_rbo_insert(t, &e);
	if (rc != 0) {
		CHECK(0, "diff: insert rc");
		return;
	}
	ref_insert(key);
}

static void do_erase(struct rfs_rbo_root *t, int idx)
{
	struct oelem probe;
	int rc;

	probe.key = ref_keys[idx];
	probe.tag = 0;
	rc = rfs_rbo_erase(t, &probe);
	if (rc != 0) {
		CHECK(0, "diff: erase rc");
		return;
	}
	ref_erase(idx);
}

static void cmp_find(struct rfs_rbo_root *t, u64 k)
{
	struct oelem probe;
	const struct oelem *got;
	int idx = ref_find(k);

	probe.key = k;
	probe.tag = 0;
	got = (const struct oelem *)rfs_rbo_find(t, &probe);
	if (idx >= 0)
		CHECK(got != NULL && got->key == k, "diff: find hit");
	else
		CHECK(got == NULL, "diff: find miss");
}

static void cmp_full_traversal(struct rfs_rbo_root *t)
{
	static u64 keys[MAXLIVE];
	int n = walk_keys(t, keys, MAXLIVE);
	int i;

	CHECK(n == ref_n, "diff: traversal count == ref");
	CHECK(rfs_rbo_size(t) == (u64)ref_n, "diff: size == ref");
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
	struct rfs_rbo_root t;
	const int OPS = 200000;
	u64 rng = 0x243f6a8885a308d3ULL;
	int i;

	rfs_rbo_init(&t, g_alloc, sizeof(struct oelem), oelem_cmp, NULL);
	ref_n = 0;

	for (i = 0; i < OPS; i++) {
		u64 r = rnd(&rng);
		u64 k = rnd(&rng) % KEYSPACE;
		int op = (int)(r % 100);

		if (op < 50) {
			do_insert(&t, k, k * 2654435761u);
		} else if (op < 80) {
			if (ref_n > 0)
				do_erase(&t, (int)(rnd(&rng) % (u64)ref_n));
		} else {
			cmp_find(&t, k);
		}

		if ((i & 511) == 0) {
			int sorted, n = walk_strict(&t, &sorted);

			if (!sorted || n != (int)rfs_rbo_size(&t) || rfs_rbo_size(&t) != (u64)ref_n) {
				CHECK(0, "diff: invariant during ops");
				break;
			}
		}
		if ((i & 4095) == 0)
			cmp_full_traversal(&t);
	}

	cmp_full_traversal(&t);

	/* 全清空 → 空树、无泄漏 */
	while (ref_n > 0)
		do_erase(&t, ref_n - 1);
	CHECK(rfs_rbo_size(&t) == 0, "diff: size 0 after drain");
	CHECK(rfs_rbo_first(&t) == NULL, "diff: first NULL after drain");
	CHECK(g_live == 0, "diff: live 0 after drain");

	rfs_rbo_fini(&t);
}

/* ══════════════════════════ 5. 随机插入/删除到空 ══════════════════════════ */
#define DRAIN_N 5000
static u64 drain_keys[DRAIN_N];
static unsigned char drain_ok[DRAIN_N];

static void test_random_drain(void)
{
	struct rfs_rbo_root t;
	u64 rng = 0x13198a2e03707344ULL;
	int i, j;

	rfs_rbo_init(&t, g_alloc, sizeof(struct oelem), oelem_cmp, NULL);

	for (i = 0; i < DRAIN_N; i++) {
		struct oelem e;
		int rc;

		e.key = rnd(&rng) % 1000000000u;
		e.tag = e.key;
		rc = rfs_rbo_insert(&t, &e);
		if (rc == 0) {
			drain_keys[i] = e.key;
			drain_ok[i] = 1;
		} else if (rc == -EEXIST) {
			drain_ok[i] = 0;	/* 重复：跳过 */
		} else {
			CHECK(0, "drain: insert rc");
			return;
		}
	}

	/* Fisher-Yates 打乱删除顺序 */
	for (i = DRAIN_N - 1; i > 0; i--) {
		u64 tmp;

		j = (int)(rnd(&rng) % (u64)(i + 1));
		tmp = drain_keys[i];
		drain_keys[i] = drain_keys[j];
		drain_keys[j] = tmp;
	}

	for (i = 0; i < DRAIN_N; i++) {
		struct oelem probe;
		int rc;

		if (!drain_ok[i])
			continue;		/* 未插入成功的哨兵 */
		probe.key = drain_keys[i];
		probe.tag = 0;
		rc = rfs_rbo_erase(&t, &probe);
		if (rc != 0) {
			CHECK(0, "drain: erase rc");
			break;
		}
	}
	CHECK(rfs_rbo_size(&t) == 0, "drain: empty");
	CHECK(rfs_rbo_first(&t) == NULL, "drain: first NULL");

	rfs_rbo_fini(&t);
}

/* ══════════════════════════ main ══════════════════════════ */
int main(void)
{
	printf("=== ravenfs_rbtree_owning user-space tests ===\n");

	test_empty_and_args();
	test_ordered_and_copy();
	test_dup_and_missing();
	test_random_diff();
	test_random_drain();

	printf("--- owning allocator accounting: allocs=%llu frees=%llu live=%llu\n",
	       (unsigned long long)g_allocs, (unsigned long long)g_frees,
	       (unsigned long long)g_live);
	CHECK(g_live == 0, "leak: live == 0");
	CHECK(g_allocs == g_frees, "leak: allocs == frees");

	printf("=== PASS %d  FAIL %d ===\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

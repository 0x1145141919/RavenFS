// SPDX-License-Identifier: GPL-2.0
/*
 * stress_allocator_slice.c —— 分配器片 struct rfs_slice（⇄ god mgr_slice_t）
 *                             对抗性压力验证（测试组，独立于实现）
 *
 * 直接编译 ../linux/ravenfs_allocator_slice.c + ../linux/ravenfs_rbtree_intrusive.c
 * （不链 god/ko 其它任何东西）。单线程：片锁由调用方持，片级无并发语义，故不跑 TSan。
 *
 * 验证层次：
 *   [A] 影子账本（位图 + 区间集，测试侧独立维护）—— 每类操作后对账：
 *         free_count == 账本空闲位数；all_dump 区间 == 账本空闲区间（逐段保序）；
 *         test_interval 与账本一致；每次 alloc/occupy 拿到的段在账本里确为空闲，
 *         返回后被确占。
 *   [B] 差分测试：另写「朴素参考实现」（有序区间数组 + 线性扫）同步跑同一随机
 *         操作序列，逐操作对拍片的返回值与效果（多种子，总操作数 ≫ 2e5）。
 *   [C] 不变量：主/从双树 rfs_rb_check；从 ⊆ 主（指针同一性）；主树两两不重叠、
 *         base 升序；元素池 live == 区间节点数；运行结束时 allocs==frees、live==0。
 *   [D] 对抗/边界：free_interval 重叠 → -EEXIST 且树不动；融合四态；对齐贴不到；
 *         片首/片尾半开；len==0 退化；越界 -EINVAL；alloc_hard best-fit+尾部下挖
 *         （god 单测同款：10→506；align 2^3 8→552 拆 [540,12)+[560,4)）；soft_alloc
 *         部分交付 + all_satisfied + push_front 失败回滚；soft_occupy rest_bits 入/出账；
 *         shutdown → 重 enable 干净。
 *
 * 用法： ./ravenfs_stress_allocator_slice [ops_per_seed] [seeds] [nbits]
 *        默认 100000 4 1024（= 4e5 操作）。参数 0 跳过差分压测（只跑定向用例）。
 */
#include "ravenfs_allocator_slice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════ 记账 / 宏 ══════════════════════════ */
static int g_pass, g_fail;
static int g_divergence;		/* 差分对拍首次分歧即停该种子 */
static u64 g_curseed;
static long g_curop;

#define ULL(x) ((unsigned long long)(x))

#define CHECK(cond, msg) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

#define FAILF(...) do { g_fail++; printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* ══════════════════════════ 分配器（计数 + 故障注入）══════════════════════════ */
struct pool {
	u64	allocs, frees, live;
	long long fail_in;	/* -1 = 从不失败；0 = 下次失败；k>0 = 放行 k 次后失败 */
};
static struct pool g_el = { 0, 0, 0, -1 };	/* 元素池（区间节点） */
static struct pool g_rl = { 0, 0, 0, -1 };	/* run 链节点池 */

static void *pool_alloc(void *ctx, u64 size)
{
	struct pool *p = (struct pool *)ctx;
	void *m;

	if (p->fail_in == 0) {
		p->fail_in = -1;	/* 只失败一次 */
		return NULL;
	}
	if (p->fail_in > 0)
		p->fail_in--;
	m = calloc(1, (size_t)size);
	if (m) {
		p->allocs++;
		p->live++;
	}
	return m;
}

static void pool_free(void *ctx, void *m)
{
	struct pool *p = (struct pool *)ctx;

	if (m) {
		p->frees++;
		p->live--;
		free(m);
	}
}

static struct rfs_slice_alloc el_alloc(void)
{
	struct rfs_slice_alloc a;

	a.alloc = pool_alloc;
	a.free = pool_free;
	a.ctx = &g_el;
	return a;
}

static struct rfs_run_alloc rl_alloc(void)
{
	struct rfs_run_alloc a;

	a.alloc = pool_alloc;
	a.free = pool_free;
	a.ctx = &g_rl;
	return a;
}

/* ══════════════════════════ 位图 / 靶场 ══════════════════════════ */
static u8 *bits_new(u64 nbits)
{
	size_t bytes = (size_t)((nbits + 7) / 8);
	void *p = NULL;

	if (posix_memalign(&p, 4096, bytes ? bytes : 1) != 0) {
		perror("posix_memalign");
		exit(2);
	}
	memset(p, 0xFF, bytes);		/* 1 = 占用 */
	return (u8 *)p;
}

static void bits_set_free(u8 *b, u64 off, u64 n)
{
	u64 k;

	for (k = 0; k < n; k++)
		b[(off + k) >> 3] &= (u8)~(1u << ((off + k) & 7));
}

/* ══════════════════════════ 被测片 + 坐标 ══════════════════════════ */
static struct rfs_slice g_s;
static u64 g_off, g_nbits;

static void slice_zero(struct rfs_slice *s)
{
	memset(s, 0, sizeof(*s));
	RFS_MUTEX_INIT(&s->lock);
}

/* 若已使能则 shutdown（释放全部区间节点），保证 target 复用不泄漏。*/
static void teardown(void)
{
	if (g_s.is_workable) {
		int rc = rfs_slice_shutdown(&g_s);

		CHECK(rc == 0, "teardown: shutdown");
	}
}

/* ══════════════════════════ 影子账本（独立位图）══════════════════════════ */
struct ledger {
	u8	*b;
	u64	 nbits;
	u64	 off;
};
static struct ledger g_led;

static void led_init(u8 *srcbits, u64 off, u64 nbits)
{
	size_t bytes = (size_t)((nbits + 7) / 8);

	free(g_led.b);
	g_led.b = (u8 *)malloc(bytes ? bytes : 1);
	memcpy(g_led.b, srcbits, bytes);
	g_led.nbits = nbits;
	g_led.off = off;
}

static void led_free(void)
{
	free(g_led.b);
	g_led.b = NULL;
}

/* 位（全局坐标）取值：1=占用。调用方保证在片内。*/
static int led_get(u64 base)
{
	u64 o = base - g_led.off;

	return (g_led.b[o >> 3] >> (o & 7)) & 1u;
}

static void led_set(u64 base, u64 len, int occ)
{
	u64 k;

	for (k = 0; k < len; k++) {
		u64 o = base - g_led.off + k;

		if (occ)
			g_led.b[o >> 3] |= (u8)(1u << (o & 7));
		else
			g_led.b[o >> 3] &= (u8)~(1u << (o & 7));
	}
}

static int led_all(u64 base, u64 len, int occ)
{
	u64 k;

	for (k = 0; k < len; k++)
		if (led_get(base + k) != occ)
			return 0;
	return 1;
}

static u64 led_free_bits(void)
{
	u64 i, c = 0;

	for (i = 0; i < g_led.nbits; i++)
		if (!led_get(g_led.off + i))
			c++;
	return c;
}

/* 从账本位图导出极大空闲 run（保序），返回个数。*/
#define REF_CAP 4096
static int led_runs(struct rfs_run *out, int cap)
{
	u64 off = g_led.off;
	u64 i = 0;
	int n = 0;

	while (i < g_led.nbits) {
		u64 st;

		if (led_get(off + i)) {
			i++;
			continue;
		}
		st = i;
		while (i < g_led.nbits && !led_get(off + i))
			i++;
		if (n < cap) {
			out[n].base = off + st;
			out[n].len = i - st;
		}
		n++;
	}
	return n;
}

/* ══════════════════════════ 朴素参考实现（有序区间数组 + 线性扫）══════════════════════════ */
struct refiv {
	u64 base, len;
};

struct ref {
	struct refiv v[REF_CAP];
	int n;
	u64 off, nbits, free_count;
};
static struct ref g_ref;

static void ref_insert(struct ref *r, u64 base, u64 len)
{
	int i;

	if (r->n >= REF_CAP) {
		printf("  FATAL ref overflow\n");
		exit(2);
	}
	for (i = r->n - 1; i >= 0 && r->v[i].base > base; i--)
		r->v[i + 1] = r->v[i];
	r->v[i + 1].base = base;
	r->v[i + 1].len = len;
	r->n++;
}

static void ref_erase(struct ref *r, int i)
{
	memmove(&r->v[i], &r->v[i + 1],
		(size_t)(r->n - i - 1) * sizeof(r->v[0]));
	r->n--;
}

static void ref_build(struct ref *r, u64 off, u64 nbits, const u8 *bits)
{
	u64 i = 0;

	r->off = off;
	r->nbits = nbits;
	r->n = 0;
	r->free_count = 0;
	while (i < nbits) {
		u64 st;

		if ((bits[i >> 3] >> (i & 7)) & 1u) {
			i++;
			continue;
		}
		st = i;
		while (i < nbits && (((bits[i >> 3] >> (i & 7)) & 1u) == 0))
			i++;
		ref_insert(r, off + st, i - st);
		r->free_count += i - st;
	}
}

/* 首个 end > base 的区间下标；无则 -1（= 片的 iv_covering_or_after）。*/
static int ref_cover_or_after(struct ref *r, u64 base)
{
	int i;

	for (i = 0; i < r->n; i++)
		if (r->v[i].base + r->v[i].len > base)
			return i;
	return -1;
}

static int ref_test(struct ref *r, bool val, u64 base, u64 len)
{
	u64 off;
	int iv;

	if (base < r->off)
		return -EINVAL;
	off = base - r->off;
	if (off > r->nbits || len > r->nbits - off)
		return -EINVAL;
	if (len == 0)
		return 0;
	iv = ref_cover_or_after(r, base);
	if (!val) {
		if (iv >= 0 && r->v[iv].base <= base &&
		    base + len <= r->v[iv].base + r->v[iv].len)
			return 0;
		return -ENOENT;
	}
	if (iv >= 0 && r->v[iv].base < base + len)
		return -ENOENT;
	return 0;
}

static int ref_free(struct ref *r, u64 base, u64 len)
{
	u64 off, nb, ne;
	int it, li;
	bool ml, mr;

	if (base < r->off)
		return -EINVAL;
	off = base - r->off;
	if (off > r->nbits || len > r->nbits - off)
		return -EINVAL;
	if (len == 0)
		return 0;
	it = ref_cover_or_after(r, base);
	li = (it >= 0) ? (it - 1) : (r->n - 1);
	if (li >= 0 && r->v[li].base + r->v[li].len > base)
		return -EEXIST;
	if (it >= 0 && r->v[it].base < base + len)
		return -EEXIST;
	ml = (li >= 0) && (r->v[li].base + r->v[li].len == base);
	mr = (it >= 0) && (r->v[it].base == base + len);
	nb = ml ? r->v[li].base : base;
	ne = mr ? (r->v[it].base + r->v[it].len) : (base + len);
	if (ml && mr) {
		ref_erase(r, it);	/* 先删高位下标 */
		ref_erase(r, li);
	} else if (ml) {
		ref_erase(r, li);
	} else if (mr) {
		ref_erase(r, it);
	}
	ref_insert(r, nb, ne - nb);
	r->free_count += len;
	return 0;
}

static int ref_min_size(struct ref *r)
{
	int b = -1, i;

	for (i = 0; i < r->n; i++) {
		if (b < 0 || r->v[i].len < r->v[b].len ||
		    (r->v[i].len == r->v[b].len && r->v[i].base < r->v[b].base))
			b = i;
	}
	return b;
}

static int ref_next_size(struct ref *r, int idx)
{
	u64 kl = r->v[idx].len, kb = r->v[idx].base;
	int b = -1, i;

	for (i = 0; i < r->n; i++) {
		if (!(r->v[i].len > kl || (r->v[i].len == kl && r->v[i].base > kb)))
			continue;
		if (b < 0 || r->v[i].len < r->v[b].len ||
		    (r->v[i].len == r->v[b].len && r->v[i].base < r->v[b].base))
			b = i;
	}
	return b;
}

static int ref_alloc_hard(struct ref *r, u8 align, u64 n, u64 *out)
{
	u64 mask, base, len, end, s, rb;
	int best = -1, i;

	if (n == 0)
		return -EINVAL;
	if (align > 63)
		return -EINVAL;
	mask = align ? ((1ULL << align) - 1ULL) : 0ULL;
	for (i = 0; i < r->n; i++) {
		if (r->v[i].len < n)
			continue;
		end = r->v[i].base + r->v[i].len;
		s = (end - n) & ~mask;
		if (s < r->v[i].base)
			continue;	/* 对齐贴不到 → 跳过 */
		if (best < 0) {
			best = i;
			continue;
		}
		if (r->v[i].len < r->v[best].len ||
		    (r->v[i].len == r->v[best].len && r->v[i].base < r->v[best].base))
			best = i;
	}
	if (best < 0)
		return -ENOSPC;
	base = r->v[best].base;
	len = r->v[best].len;
	end = base + len;
	s = (end - n) & ~mask;
	ref_erase(r, best);
	if (s > base)
		ref_insert(r, base, s - base);
	rb = s + n;
	if (rb < end)
		ref_insert(r, rb, end - rb);
	r->free_count -= n;
	*out = s;
	return 0;
}

#define REF_RUNS 512
struct softout {
	u64 b[REF_RUNS], l[REF_RUNS];
	int n;
};

static int ref_soft_alloc(struct ref *r, u8 align, u64 n,
			  struct softout *out, bool *sat)
{
	u64 mask, remain, taken = 0;
	u64 sb[REF_RUNS], sl[REF_RUNS];
	int sn = 0, idx;

	if (align > 63)
		return -EINVAL;
	*sat = false;
	if (n == 0)
		return -EINVAL;
	mask = align ? ((1ULL << align) - 1ULL) : 0ULL;
	remain = n;
	idx = ref_min_size(r);
	while (idx >= 0 && remain > 0) {
		u64 ivb = r->v[idx].base, ivl = r->v[idx].len, ive = ivb + ivl;
		u64 t = (ivl < remain) ? ivl : remain;
		u64 s = (ive - t) & ~mask;

		if (s < ivb) {
			s = (ivb + mask) & ~mask;
			if (s >= ive) {
				idx = ref_next_size(r, idx);
				continue;
			}
			{
				u64 avail = ive - s;

				t = (avail < remain) ? avail : remain;
			}
		}
		if (sn >= REF_RUNS) {
			printf("  FATAL soft ref overflow\n");
			exit(2);
		}
		sb[sn] = s;
		sl[sn] = t;
		sn++;
		ref_erase(r, idx);
		if (s > ivb)
			ref_insert(r, ivb, s - ivb);
		{
			u64 rb = s + t;

			if (rb < ive)
				ref_insert(r, rb, ive - rb);
		}
		r->free_count -= t;
		taken += t;
		remain -= t;
		idx = ref_min_size(r);
	}
	if (taken == 0)
		return -ENOSPC;
	*sat = (remain == 0);
	out->n = sn;
	for (int i = 0; i < sn; i++) {
		out->b[i] = sb[sn - 1 - i];
		out->l[i] = sl[sn - 1 - i];
	}
	return 0;
}

static int ref_soft_occupy(struct ref *r, u64 base, u64 len, u64 *rest)
{
	u64 real, end_slice, ivb, ive, claimed, rb;
	int ci, i;

	if (*rest > len)
		return -EINVAL;
	if (len == 0)
		return 0;
	real = len - *rest;
	if (real == 0)
		return 0;
	if (base < r->off)
		return -EINVAL;
	end_slice = r->off + r->nbits;
	if (base > end_slice)
		return -EINVAL;
	ci = -1;
	for (i = 0; i < r->n; i++)
		if (r->v[i].base <= base && base < r->v[i].base + r->v[i].len) {
			ci = i;
			break;
		}
	if (ci < 0) {
		*rest = len;
		return -ENOSPC;
	}
	ivb = r->v[ci].base;
	ive = ivb + r->v[ci].len;
	claimed = ive - base;
	if (claimed > real)
		claimed = real;
	if (base + claimed > end_slice)
		claimed = end_slice - base;
	if (claimed == 0) {
		*rest = len;
		return -ENOSPC;
	}
	ref_erase(r, ci);
	if (base > ivb)
		ref_insert(r, ivb, base - ivb);
	rb = base + claimed;
	if (rb < ive)
		ref_insert(r, rb, ive - rb);
	r->free_count -= claimed;
	*rest = len - claimed;
	return 0;
}

/* ══════════════════════════ 比较器 / 快照 ══════════════════════════ */
static int tcmp_pos(const struct rfs_rb_node *a, const struct rfs_rb_node *b,
		    void *ctx)
{
	const struct rfs_interval *x = container_of(a, struct rfs_interval, by_pos);
	const struct rfs_interval *y = container_of(b, struct rfs_interval, by_pos);

	(void)ctx;
	if (x->base != y->base)
		return (x->base < y->base) ? -1 : 1;
	return 0;
}

static int tcmp_size(const struct rfs_rb_node *a, const struct rfs_rb_node *b,
		     void *ctx)
{
	const struct rfs_interval *x = container_of(a, struct rfs_interval, by_size);
	const struct rfs_interval *y = container_of(b, struct rfs_interval, by_size);

	(void)ctx;
	if (x->len != y->len)
		return (x->len < y->len) ? -1 : 1;
	if (x->base != y->base)
		return (x->base < y->base) ? -1 : 1;
	return 0;
}

static int check_trees(const char *tag)
{
	int rp = rfs_rb_check(&g_s.pos, tcmp_pos, NULL);
	int rs = rfs_rb_check(&g_s.size, tcmp_size, NULL);

	if (rp != 0 || rs != 0)
		FAILF("%s: rb_check pos=%d size=%d", tag, rp, rs);
	else
		g_pass++;
	return (rp == 0 && rs == 0);
}

static int dump_arr(struct rfs_run *out, int cap)
{
	struct rfs_run_dlist dl;
	struct rfs_run_node *nd;
	int n = 0;

	rfs_run_dlist_init(&dl, rl_alloc());
	if (rfs_slice_all_dump(&g_s, &dl) != 0) {
		rfs_run_dlist_fini(&dl);
		return -1;
	}
	for (nd = dl.head; nd; nd = nd->next) {
		if (n < cap)
			out[n] = nd->run;
		n++;
	}
	rfs_run_dlist_fini(&dl);
	return n;
}

static void expect_dump(const struct rfs_run *want, int wn, const char *tag)
{
	struct rfs_run got[REF_CAP];
	int n = dump_arr(got, REF_CAP);
	int ok = (n == wn), i;

	if (ok) {
		for (i = 0; i < n; i++)
			if (got[i].base != want[i].base || got[i].len != want[i].len)
				ok = 0;
	}
	if (ok) {
		g_pass++;
		return;
	}
	printf("  FAIL %s: got", tag);
	for (i = 0; i < n && i < 12; i++)
		printf(" [%llu,%llu)", ULL(got[i].base), ULL(got[i].base + got[i].len));
	printf("  want");
	for (i = 0; i < wn && i < 12; i++)
		printf(" [%llu,%llu)", ULL(want[i].base), ULL(want[i].base + want[i].len));
	printf("\n");
	g_fail++;
}

/* 快照 / 校验「状态不变」（重叠报错类用例）。*/
static void snapshot(struct rfs_run *out, int *n, u64 *fc)
{
	*n = dump_arr(out, REF_CAP);
	*fc = rfs_slice_free_count(&g_s);
}

static void check_unchanged(const struct rfs_run *snap, int sn, u64 fc,
			    const char *tag)
{
	struct rfs_run got[REF_CAP];
	int n = dump_arr(got, REF_CAP);
	int ok = (n == sn) && (rfs_slice_free_count(&g_s) == fc), i;

	if (ok) {
		for (i = 0; i < n; i++)
			if (got[i].base != snap[i].base || got[i].len != snap[i].len)
				ok = 0;
	}
	if (ok)
		g_pass++;
	else
		FAILF("%s: state changed (fc=%llu vs %llu, n=%d vs %d)",
		      tag, ULL(rfs_slice_free_count(&g_s)), ULL(fc), n, sn);
}

/* ══════════════════════════ 全量不变量 ══════════════════════════ */
static void check_invariants(const char *tag)
{
	struct rfs_run pos[REF_CAP];
	const struct rfs_interval *posiv[REF_CAP], *sziv[REF_CAP];
	struct rfs_run d[REF_CAP], lr[REF_CAP];
	struct rfs_rb_node *n;
	int np = 0, ns = 0, i, j, dn, ln;
	int ok;

	check_trees(tag);

	for (n = rfs_rb_first(&g_s.pos); n; n = rfs_rb_next(n)) {
		struct rfs_interval *iv = container_of(n, struct rfs_interval, by_pos);

		if (np < REF_CAP) {
			pos[np].base = iv->base;
			pos[np].len = iv->len;
			posiv[np] = iv;
		}
		np++;
	}
	for (n = rfs_rb_first(&g_s.size); n; n = rfs_rb_next(n)) {
		struct rfs_interval *iv = container_of(n, struct rfs_interval, by_size);

		if (ns < REF_CAP) {
			sziv[ns] = iv;
		}
		ns++;
	}
	CHECK(np == g_ref.n, "inv: pos count == ref.n");
	CHECK(ns == g_ref.n, "inv: size count == ref.n");

	ok = 1;
	for (i = 1; i < np; i++)
		if (!(pos[i - 1].base + pos[i - 1].len <= pos[i].base))
			ok = 0;
	CHECK(ok, "inv: master base-ascending & disjoint");

	ok = 1;
	for (j = 0; j < ns; j++) {
		int found = 0;

		for (i = 0; i < np; i++)
			if (posiv[i] == sziv[j]) {
				found = 1;
				break;
			}
		if (!found) {
			ok = 0;
			break;
		}
	}
	CHECK(ok, "inv: slave node ⊆ master node (identity)");

	dn = dump_arr(d, REF_CAP);
	ok = (dn == g_ref.n);
	if (ok)
		for (i = 0; i < dn; i++)
			if (d[i].base != g_ref.v[i].base || d[i].len != g_ref.v[i].len)
				ok = 0;
	CHECK(ok, "inv: all_dump == ref intervals (ordered)");

	ln = led_runs(lr, REF_CAP);
	ok = (ln == g_ref.n);
	if (ok)
		for (i = 0; i < ln; i++)
			if (lr[i].base != g_ref.v[i].base || lr[i].len != g_ref.v[i].len)
				ok = 0;
	CHECK(ok, "inv: ledger runs == ref intervals (ordered)");

	CHECK(rfs_slice_free_count(&g_s) == g_ref.free_count,
	      "inv: free_count == ref.free_count");
	CHECK(rfs_slice_free_count(&g_s) == led_free_bits(),
	      "inv: free_count == ledger free bits");
	CHECK((g_el.allocs - g_el.frees) == (u64)g_ref.n,
	      "inv: element-pool live == interval count");
	CHECK(g_rl.live == 0, "inv: run-list pool live == 0 at checkpoint");
}

/* ══════════════════════════ RNG ══════════════════════════ */
static u64 g_rng = 0x9E3779B97F4A7C15ULL;

static u64 rnd_next(void)
{
	u64 x = g_rng;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	g_rng = x;
	return x;
}

/* ══════════════════════════ 靶场装配 ══════════════════════════ */
static void setup_free(const struct rfs_run *runs, int cnt, u64 off, u64 nbits)
{
	u8 *bits = bits_new(nbits);
	struct rfs_slice_cfg cfg;
	int i;

	teardown();
	for (i = 0; i < cnt; i++)
		bits_set_free(bits, runs[i].base - off, runs[i].len);
	slice_zero(&g_s);
	memset(&cfg, 0, sizeof(cfg));
	cfg.slice_base = bits;
	cfg.in_global_base_idx = off;
	cfg.slice_bitscount = nbits;
	cfg.alloc = el_alloc();
	CHECK(rfs_slice_enable(&g_s, &cfg) == 0, "setup: enable");
	g_off = off;
	g_nbits = nbits;
	ref_build(&g_ref, off, nbits, bits);
	led_init(bits, off, nbits);
	free(bits);
}

static void randomize_bitmap(u8 *bits, u64 nbits)
{
	u64 i = 0;

	while (i < nbits) {
		if ((rnd_next() & 3u) == 0) {
			u64 room = nbits - i;
			u64 ln = 1 + rnd_next() % (room < 40 ? room : 40);

			bits_set_free(bits, i, ln);
			i += ln;
		} else {
			i++;
		}
	}
}

/* ══════════════════════════ 定向 1：融合四态 ══════════════════════════ */
static void test_fusion(void)
{
	struct rfs_run init2[2] = { { 1010, 10 }, { 1030, 10 } };
	struct rfs_run init2b[2] = { { 1010, 10 }, { 1040, 10 } };
	struct rfs_run w[8];

	puts("[T1] free_interval 融合四态（邻接才并 / 不邻接不并）");

	/* 纯插（与两邻都隔开）*/
	setup_free(init2, 2, 1000, 64);
	CHECK(rfs_slice_free_interval(&g_s, 1050, 5) == 0, "fusion: pure insert rc");
	w[0] = (struct rfs_run){ 1010, 10 };
	w[1] = (struct rfs_run){ 1030, 10 };
	w[2] = (struct rfs_run){ 1050, 5 };
	expect_dump(w, 3, "fusion: pure insert");
	CHECK(rfs_slice_free_count(&g_s) == 25, "fusion: pure insert fc");
	check_trees("fusion: pure insert");

	/* 仅左并：free [1005,1010) 贴左区间头 */
	setup_free(init2, 2, 1000, 64);
	CHECK(rfs_slice_free_interval(&g_s, 1005, 5) == 0, "fusion: left rc");
	w[0] = (struct rfs_run){ 1005, 15 };
	w[1] = (struct rfs_run){ 1030, 10 };
	expect_dump(w, 2, "fusion: left-only merge");
	CHECK(rfs_slice_free_count(&g_s) == 25, "fusion: left fc");

	/* 仅右并：free [1030,1040) 贴右区间头，与左区间隔 10 */
	setup_free(init2b, 2, 1000, 64);
	CHECK(rfs_slice_free_interval(&g_s, 1030, 10) == 0, "fusion: right rc");
	w[0] = (struct rfs_run){ 1010, 10 };
	w[1] = (struct rfs_run){ 1030, 20 };
	expect_dump(w, 2, "fusion: right-only merge");
	CHECK(rfs_slice_free_count(&g_s) == 30, "fusion: right fc");

	/* 双侧并：free [1020,1040) 左右各贴一个 */
	setup_free(init2b, 2, 1000, 64);
	CHECK(rfs_slice_free_interval(&g_s, 1020, 20) == 0, "fusion: both rc");
	w[0] = (struct rfs_run){ 1010, 40 };
	expect_dump(w, 1, "fusion: both-side merge");
	CHECK(rfs_slice_free_count(&g_s) == 40, "fusion: both fc");
	check_trees("fusion: both merge");

	/* 邻接差 1 不并 */
	setup_free(init2, 2, 1000, 64);
	CHECK(rfs_slice_free_interval(&g_s, 1021, 5) == 0, "fusion: gap-1 rc");
	w[0] = (struct rfs_run){ 1010, 10 };
	w[1] = (struct rfs_run){ 1021, 5 };
	w[2] = (struct rfs_run){ 1030, 10 };
	expect_dump(w, 3, "fusion: gap-1 not merged");
}

/* ══════════════════════════ 定向 2：重叠 → -EEXIST 且树不动 ══════════════════════════ */
static void test_overlap(void)
{
	struct rfs_run init2[2] = { { 1010, 10 }, { 1030, 10 } };
	struct rfs_run snap[REF_CAP], w[4];
	int sn;
	u64 sfc;

	puts("[T2] free_interval 重叠检测（-EEXIST 且状态不动）");
	setup_free(init2, 2, 1000, 64);
	snapshot(snap, &sn, &sfc);
	CHECK(sn == 2 && sfc == 20, "overlap: init state");

	CHECK(rfs_slice_free_interval(&g_s, 1012, 3) == -EEXIST, "overlap: inside");
	check_unchanged(snap, sn, sfc, "overlap: inside unchanged");
	CHECK(rfs_slice_free_interval(&g_s, 1010, 10) == -EEXIST, "overlap: exact equal");
	check_unchanged(snap, sn, sfc, "overlap: exact unchanged");
	CHECK(rfs_slice_free_interval(&g_s, 1008, 4) == -EEXIST, "overlap: straddle left");
	check_unchanged(snap, sn, sfc, "overlap: straddle-left unchanged");
	CHECK(rfs_slice_free_interval(&g_s, 1018, 4) == -EEXIST, "overlap: straddle right");
	check_unchanged(snap, sn, sfc, "overlap: straddle-right unchanged");
	CHECK(rfs_slice_free_interval(&g_s, 1005, 20) == -EEXIST, "overlap: covering");
	check_unchanged(snap, sn, sfc, "overlap: covering unchanged");
	CHECK(rfs_slice_free_interval(&g_s, 1015, 20) == -EEXIST, "overlap: across both");
	check_unchanged(snap, sn, sfc, "overlap: across unchanged");
	check_trees("overlap: trees");

	/* 合法：填两区间之间的占用缺口 → 双侧并 [1010,1040) */
	CHECK(rfs_slice_free_interval(&g_s, 1020, 10) == 0, "overlap: legal gap fill");
	w[0] = (struct rfs_run){ 1010, 30 };
	expect_dump(w, 1, "overlap: merged by gap fill");
	CHECK(rfs_slice_free_count(&g_s) == 30, "overlap: fc 30");
}

/* ══════════════════════════ 定向 3：片首/片尾半开 + len==0 + 越界 ══════════════════════════ */
static void test_boundaries(void)
{
	struct rfs_run whole[1] = { { 1000, 64 } };
	struct rfs_run init2[2] = { { 1010, 10 }, { 1030, 10 } };
	u64 o;

	puts("[T6] 片首/片尾半开边界 + len==0 + 越界");
	setup_free(whole, 1, 1000, 64);
	CHECK(rfs_slice_test_interval(&g_s, false, 1000, 64) == 0, "bnd: whole free");
	CHECK(rfs_slice_test_interval(&g_s, false, 999, 1) == -EINVAL, "bnd: below start");
	CHECK(rfs_slice_test_interval(&g_s, false, 1064, 1) == -EINVAL, "bnd: end+1");
	CHECK(rfs_slice_test_interval(&g_s, false, 1064, 0) == 0, "bnd: end len0 ok");
	CHECK(rfs_slice_test_interval(&g_s, false, 1063, 1) == 0, "bnd: last bit free");
	CHECK(rfs_slice_test_interval(&g_s, true, 1063, 1) == -ENOENT, "bnd: last bit not occ");
	CHECK(rfs_slice_test_interval(&g_s, false, 1000, 65) == -EINVAL, "bnd: over end");
	CHECK(rfs_slice_free_interval(&g_s, 999, 1) == -EINVAL, "bnd: free below");
	CHECK(rfs_slice_free_interval(&g_s, 1064, 1) == -EINVAL, "bnd: free end+1");
	CHECK(rfs_slice_free_interval(&g_s, 1064, 0) == 0, "bnd: free end len0");
	CHECK(rfs_slice_free_interval(&g_s, 1000, 64) == -EEXIST, "bnd: free already-free whole");

	o = 0;
	CHECK(rfs_slice_alloc_hard(&g_s, 0, 10, &o) == 0, "bnd: alloc rc");
	CHECK(o == 1054, "bnd: tail-carve 1054");
	CHECK(rfs_slice_test_interval(&g_s, true, 1054, 10) == 0, "bnd: carved occupied");
	CHECK(rfs_slice_test_interval(&g_s, false, 1054, 1) == -ENOENT, "bnd: carved not free");
	CHECK(rfs_slice_free_interval(&g_s, 1054, 10) == 0, "bnd: restore");
	CHECK(rfs_slice_free_count(&g_s) == 64, "bnd: restored 64");

	/* len==0 no-op 不动状态 */
	{
		struct rfs_run snap[8];
		int sn;
		u64 sfc;

		snapshot(snap, &sn, &sfc);
		CHECK(rfs_slice_free_interval(&g_s, 1000, 0) == 0, "bnd: free len0");
		CHECK(rfs_slice_test_interval(&g_s, true, 1000, 0) == 0, "bnd: test occ len0");
		CHECK(rfs_slice_test_interval(&g_s, false, 1000, 0) == 0, "bnd: test free len0");
		check_unchanged(snap, sn, sfc, "bnd: len0 no-op");
	}

	/* test_interval 占用缺口 */
	setup_free(init2, 2, 1000, 64);
	CHECK(rfs_slice_test_interval(&g_s, true, 1020, 10) == 0, "bnd: occupied gap");
	CHECK(rfs_slice_test_interval(&g_s, true, 1010, 10) == -ENOENT, "bnd: gap touches free");
	CHECK(rfs_slice_test_interval(&g_s, true, 1005, 30) == -ENOENT, "bnd: gap spans free");
}

/* ══════════════════════════ 定向 4：alloc_hard ══════════════════════════ */
static void test_alloc_hard(void)
{
	struct rfs_run init[3] = { { 500, 16 }, { 524, 8 }, { 540, 24 } };
	struct rfs_run single[1] = { { 501, 10 } };
	struct rfs_run w[4];
	u64 o;

	puts("[T3] alloc_hard best-fit + 尾部下挖 + 对齐贴不到 + god 对拍");
	setup_free(init, 3, 500, 64);
	CHECK(rfs_slice_free_count(&g_s) == 48, "hard: init fc 48");
	o = 7;
	CHECK(rfs_slice_alloc_hard(&g_s, 0, 0, &o) == -EINVAL, "hard: zero");
	CHECK(rfs_slice_alloc_hard(&g_s, 64, 1, &o) == -EINVAL, "hard: align>63");
	o = 0;
	CHECK(rfs_slice_alloc_hard(&g_s, 0, 10, &o) == 0, "hard: best-fit rc");
	CHECK(o == 506, "hard: tail-carve 506");
	w[0] = (struct rfs_run){ 500, 6 };
	w[1] = (struct rfs_run){ 524, 8 };
	w[2] = (struct rfs_run){ 540, 24 };
	expect_dump(w, 3, "hard: remainder "
		"[500,6)+[524,8)+[540,24)");
	CHECK(rfs_slice_free_count(&g_s) == 38, "hard: fc 38");
	check_trees("hard: after 506");

	o = 0;
	CHECK(rfs_slice_alloc_hard(&g_s, 3, 8, &o) == 0, "hard: aligned rc");
	CHECK(o == 552, "hard: aligned base 552");
	w[0] = (struct rfs_run){ 500, 6 };
	w[1] = (struct rfs_run){ 524, 8 };
	w[2] = (struct rfs_run){ 540, 12 };
	w[3] = (struct rfs_run){ 560, 4 };
	expect_dump(w, 4, "hard: aligned split [540,12)+[560,4)");
	CHECK(rfs_slice_free_count(&g_s) == 30, "hard: fc 30");

	o = 0xABCDEFULL;
	CHECK(rfs_slice_alloc_hard(&g_s, 0, 100, &o) == -ENOSPC, "hard: no fit");
	CHECK(o == 0xABCDEFULL, "hard: out untouched on fail");
	o = 0;
	CHECK(rfs_slice_alloc_hard(&g_s, 0, 5, &o) == 0, "hard: extra rc");
	CHECK(o >= 500 && o + 5 <= 564, "hard: never cross slice");

	/* 对齐贴不到：区间 len≥n 但无对齐段 → 整体 -ENOSPC */
	setup_free(single, 1, 500, 64);
	o = 0x99ULL;
	CHECK(rfs_slice_alloc_hard(&g_s, 3, 8, &o) == -ENOSPC, "hard: align unreachable");
	CHECK(o == 0x99ULL, "hard: unreachable out untouched");
	CHECK(rfs_slice_free_count(&g_s) == 10, "hard: fc unchanged on unreachable");
	o = 0;
	CHECK(rfs_slice_alloc_hard(&g_s, 0, 8, &o) == 0, "hard: align0 rc");
	CHECK(o == 503, "hard: align0 base 503");
	CHECK(rfs_slice_free_count(&g_s) == 2, "hard: fc 2");
}

/* ══════════════════════════ 定向 5：soft_alloc ══════════════════════════ */
static void test_soft_alloc(void)
{
	struct rfs_run init[4] = { { 800, 3 }, { 820, 2 }, { 840, 10 }, { 880, 20 } };
	struct rfs_run_list rl;
	struct rfs_run_node *nd;
	struct rfs_run w[4];
	bool sat;
	u64 sum, eb[8], el_[8];
	int n;

	puts("[T4] soft_alloc 碎片优先 / 部分交付 / all_satisfied");
	setup_free(init, 4, 800, 128);
	CHECK(rfs_slice_free_count(&g_s) == 35, "soft: init fc 35");

	rfs_run_list_init(&rl, rl_alloc());
	sat = false;
	CHECK(rfs_slice_soft_alloc(&g_s, 0, 6, &rl, &sat) == 0, "soft: rc");
	CHECK(sat == true, "soft: satisfied");
	n = 0;
	sum = 0;
	for (nd = rl.head; nd; nd = nd->next) {
		if (n < 8) {
			eb[n] = nd->run.base;
			el_[n] = nd->run.len;
		}
		sum += nd->run.len;
		n++;
	}
	CHECK(n == 3, "soft: 3 fragments");
	CHECK(sum == 6, "soft: delivered 6");
	CHECK(n >= 1 && eb[0] == 849 && el_[0] == 1, "soft: head {849,1}");
	rfs_run_list_fini(&rl);
	w[0] = (struct rfs_run){ 840, 9 };
	w[1] = (struct rfs_run){ 880, 20 };
	expect_dump(w, 2, "soft: remainder {840,9},{880,20}");
	CHECK(rfs_slice_free_count(&g_s) == 29, "soft: fc 29");

	/* 100 位：耗尽剩余 29 → 部分交付 */
	rfs_run_list_init(&rl, rl_alloc());
	sat = true;
	CHECK(rfs_slice_soft_alloc(&g_s, 0, 100, &rl, &sat) == 0, "soft: partial rc");
	CHECK(sat == false, "soft: not satisfied");
	sum = 0;
	for (nd = rl.head; nd; nd = nd->next)
		sum += nd->run.len;
	CHECK(sum == 29, "soft: partial sum 29");
	rfs_run_list_fini(&rl);
	expect_dump(w, 0, "soft: drained empty");
	CHECK(rfs_slice_free_count(&g_s) == 0, "soft: fc 0");

	rfs_run_list_init(&rl, rl_alloc());
	CHECK(rfs_slice_soft_alloc(&g_s, 0, 1, &rl, &sat) == -ENOSPC, "soft: empty -ENOSPC");
	CHECK(sat == false, "soft: sat false on empty");
	CHECK(rl.head == NULL, "soft: no runs on empty");
	rfs_run_list_fini(&rl);
	CHECK(rfs_slice_soft_alloc(&g_s, 0, 0, &rl, &sat) == -EINVAL, "soft: zero rejected");
}

/* ══════════════════════════ 定向 6：soft_occupy rest_bits ══════════════════════════ */
static void test_soft_occupy(void)
{
	struct rfs_run init[1] = { { 500, 64 } };
	struct rfs_run w[4];
	u64 rest;

	puts("[T5] soft_occupy rest_bits 入/出账");
	setup_free(init, 1, 500, 64);
	CHECK(rfs_slice_free_count(&g_s) == 64, "occ: init fc 64");
	rest = 0;
	CHECK(rfs_slice_soft_occupy(&g_s, 500, 10, &rest) == 0, "occ: claim 10");
	CHECK(rest == 0, "occ: rest 0");
	w[0] = (struct rfs_run){ 510, 54 };
	expect_dump(w, 1, "occ: remainder {510,54}");
	CHECK(rfs_slice_free_count(&g_s) == 54, "occ: fc 54");

	rest = 4;
	CHECK(rfs_slice_soft_occupy(&g_s, 510, 10, &rest) == 0, "occ: chain real=6");
	CHECK(rest == 4, "occ: rest 4");
	CHECK(rfs_slice_free_count(&g_s) == 48, "occ: fc 48");

	rest = 0;
	CHECK(rfs_slice_soft_occupy(&g_s, 500, 5, &rest) == -ENOSPC, "occ: anchor occupied");
	CHECK(rest == 5, "occ: rest backfilled");
	rest = 0;
	CHECK(rfs_slice_soft_occupy(&g_s, 499, 1, &rest) == -EINVAL, "occ: below start");
	rest = 0;
	CHECK(rfs_slice_soft_occupy(&g_s, 564, 4, &rest) == -ENOSPC, "occ: base==slice_end");
	CHECK(rest == 4, "occ: rest=len at slice_end");
	rest = 0;
	CHECK(rfs_slice_soft_occupy(&g_s, 565, 1, &rest) == -EINVAL, "occ: base>slice_end");
	rest = 5;
	CHECK(rfs_slice_soft_occupy(&g_s, 516, 4, &rest) == -EINVAL, "occ: rest>len");
	rest = 0;
	CHECK(rfs_slice_soft_occupy(&g_s, 516, 0, &rest) == 0, "occ: len 0");
	CHECK(rest == 0, "occ: rest unchanged len0");
	rest = 4;
	CHECK(rfs_slice_soft_occupy(&g_s, 516, 4, &rest) == 0, "occ: real==0");
	CHECK(rest == 4, "occ: rest unchanged real0");
	rest = 0;
	CHECK(rfs_slice_soft_occupy(&g_s, 516, 100, &rest) == 0, "occ: partial to end");
	CHECK(rest == 52, "occ: rest 100-48");
	CHECK(rfs_slice_free_count(&g_s) == 0, "occ: fc 0");
	check_trees("occ: final");

	CHECK(rfs_slice_free_interval(&g_s, 516, 48) == 0, "occ: restore a");
	CHECK(rfs_slice_free_interval(&g_s, 510, 6) == 0, "occ: restore b");
	CHECK(rfs_slice_free_interval(&g_s, 500, 10) == 0, "occ: restore c");
	CHECK(rfs_slice_free_count(&g_s) == 64, "occ: restored 64");
}

/* ══════════════════════════ 定向 7：shutdown → 重 enable ══════════════════════════ */
static void test_shutdown_reinit(void)
{
	u8 *bits = bits_new(128);
	struct rfs_slice_cfg cfg;
	struct rfs_run w[4];

	puts("[T7] shutdown → 重 enable 状态干净");
	teardown();
	bits_set_free(bits, 0, 10);
	bits_set_free(bits, 20, 5);
	slice_zero(&g_s);
	memset(&cfg, 0, sizeof(cfg));
	cfg.slice_base = bits;
	cfg.in_global_base_idx = 700;
	cfg.slice_bitscount = 128;
	cfg.alloc = el_alloc();
	CHECK(rfs_slice_enable(&g_s, &cfg) == 0, "reinit: enable");
	CHECK(rfs_slice_free_count(&g_s) == 15, "reinit: fc 15");
	w[0] = (struct rfs_run){ 700, 10 };
	w[1] = (struct rfs_run){ 720, 5 };
	expect_dump(w, 2, "reinit: initial runs");
	CHECK(rfs_slice_shutdown(&g_s) == 0, "reinit: shutdown");
	CHECK(rfs_slice_free_count(&g_s) == 0, "reinit: fc 0");
	CHECK(rfs_rb_empty(&g_s.pos) && rfs_rb_empty(&g_s.size), "reinit: trees empty");
	CHECK(g_el.live == 0, "reinit: element pool drained");
	CHECK(rfs_slice_shutdown(&g_s) == -EINVAL, "reinit: double shutdown rejected");
	CHECK(rfs_slice_enable(&g_s, &cfg) == 0, "reinit: re-enable");
	CHECK(rfs_slice_free_count(&g_s) == 15, "reinit: fc 15 again");
	expect_dump(w, 2, "reinit: runs again");
	check_trees("reinit: after re-enable");
	CHECK(rfs_slice_shutdown(&g_s) == 0, "reinit: final shutdown");
	free(bits);
}

/* ══════════════════════════ 定向 8：free_intervals ══════════════════════════ */
static void test_free_intervals(void)
{
	struct rfs_run init[3] = { { 1000, 4 }, { 1008, 4 }, { 1016, 4 } };
	struct rfs_run w[4];
	struct rfs_run_list rl;

	puts("[T8] free_intervals 消费链 + 中途出错余段留链");
	setup_free(init, 3, 1000, 32);
	CHECK(rfs_slice_free_intervals(&g_s, NULL) == -EINVAL, "fi: nullptr");
	rfs_run_list_init(&rl, rl_alloc());
	CHECK(rfs_run_list_push_front(&rl, 1012, 4) == 0, "fi: push 1012");
	CHECK(rfs_run_list_push_front(&rl, 1004, 4) == 0, "fi: push 1004");
	CHECK(rfs_slice_free_intervals(&g_s, &rl) == 0, "fi: drain rc");
	CHECK(rl.head == NULL, "fi: drained");
	w[0] = (struct rfs_run){ 1000, 20 };
	expect_dump(w, 1, "fi: merged whole {1000,20}");
	CHECK(rfs_slice_free_count(&g_s) == 20, "fi: fc 20");
	rfs_run_list_fini(&rl);

	rfs_run_list_init(&rl, rl_alloc());
	CHECK(rfs_run_list_push_front(&rl, 9000, 4) == 0, "fi2: push 9000");
	CHECK(rfs_run_list_push_front(&rl, 1010, 4) == 0, "fi2: push overlap");
	CHECK(rfs_slice_free_intervals(&g_s, &rl) == -EEXIST, "fi2: overlap error");
	CHECK(rl.head != NULL && rl.head->run.base == 9000, "fi2: remainder kept");
	rfs_run_list_fini(&rl);
	CHECK(rfs_slice_free_count(&g_s) == 20, "fi2: fc unchanged");
}

/* ══════════════════════════ 定向 9：soft_alloc push_front 失败回滚 ══════════════════════════ */
static void test_rollback(void)
{
	struct rfs_run whole[1] = { { 2000, 64 } };
	struct rfs_run two[2] = { { 2000, 4 }, { 2008, 10 } };
	struct rfs_run snap[REF_CAP], got[8], w[2];
	struct rfs_run_list rl;
	bool sat;
	u64 fc;
	int sn, gn, rc;

	puts("[T9] soft_alloc push_front 失败回滚（账一致性）");

	/* 场景 A：首次 push_front 即失败 */
	setup_free(whole, 1, 2000, 64);
	snapshot(snap, &sn, &fc);
	g_rl.fail_in = 0;
	rfs_run_list_init(&rl, rl_alloc());
	sat = true;
	rc = rfs_slice_soft_alloc(&g_s, 0, 10, &rl, &sat);
	rfs_run_list_fini(&rl);
	g_rl.fail_in = -1;
	gn = dump_arr(got, 8);
	printf("    [probe A] rc=%d free_count=%llu (expect 64) dump_n=%d\n",
	       rc, ULL(rfs_slice_free_count(&g_s)), gn);
	CHECK(rc == -ENOMEM, "rollbackA: -ENOMEM");
	CHECK(rfs_slice_free_count(&g_s) == 64, "rollbackA: free_count unchanged");
	check_unchanged(snap, sn, fc, "rollbackA: state unchanged");

	/* 场景 B：第二次 push_front 失败（首段已交付，回滚第二段） */
	setup_free(two, 2, 2000, 64);
	snapshot(snap, &sn, &fc);
	g_rl.fail_in = 1;
	rfs_run_list_init(&rl, rl_alloc());
	sat = true;
	rc = rfs_slice_soft_alloc(&g_s, 0, 10, &rl, &sat);
	rfs_run_list_fini(&rl);
	g_rl.fail_in = -1;
	gn = dump_arr(got, 8);
	printf("    [probe B] rc=%d free_count=%llu (expect 10) dump_n=%d\n",
	       rc, ULL(rfs_slice_free_count(&g_s)), gn);
	CHECK(rc == -ENOMEM, "rollbackB: -ENOMEM");
	CHECK(rfs_slice_free_count(&g_s) == 10, "rollbackB: free_count consistent");
	w[0] = (struct rfs_run){ 2008, 10 };
	expect_dump(w, 1, "rollbackB: second segment restored");
	check_trees("rollbackB: trees");
}

/* ══════════════════════════ 定向 10：free_interval 元素池 OOM（次要观察）══════════════════════════ */
static void test_free_interval_oom(void)
{
	struct rfs_run init2[2] = { { 1010, 10 }, { 1030, 10 } };
	struct rfs_run got[8];
	int rc, n;

	puts("[T10] free_interval 元素池 OOM 错误路径（次要观察，不计判定）");
	setup_free(init2, 2, 1000, 64);
	g_el.fail_in = 0;
	rc = rfs_slice_free_interval(&g_s, 1020, 10);	/* 本应双侧融合 */
	g_el.fail_in = -1;
	n = dump_arr(got, 8);
	printf("    [probe C] rc=%d dump_n=%d free_count=%llu (init: dump_n=2 fc=20; "
	       "expected on success: dump_n=1 {1010,40} fc=40)\n",
	       rc, n, ULL(rfs_slice_free_count(&g_s)));
}

/* ══════════════════════════ 差分压测操作 ══════════════════════════ */
static int ledger_test(bool val, u64 base, u64 len)
{
	u64 o;

	if (base < g_off)
		return -EINVAL;
	o = base - g_off;
	if (o > g_nbits || len > g_nbits - o)
		return -EINVAL;
	if (len == 0)
		return 0;
	if (!val)
		return led_all(base, len, 0) ? 0 : -ENOENT;
	return led_all(base, len, 1) ? 0 : -ENOENT;
}

static void do_alloc_hard(u64 r)
{
	u8 align = (u8)(r % 6);
	u64 n = 1 + (r >> 8) % 48;
	const u64 sentinel = 0xDEADBEEFCAFEBABEULL;
	u64 co = sentinel, ro = sentinel;
	int rc = rfs_slice_alloc_hard(&g_s, align, n, &co);
	int rr = ref_alloc_hard(&g_ref, align, n, &ro);

	if (rc != rr || co != ro) {
		FAILF("DIFF seed=%llu op=%ld alloc_hard(align=%u,n=%llu): rc %d/%d out %llx/%llx",
		      ULL(g_curseed), g_curop, align, ULL(n), rc, rr, ULL(co), ULL(ro));
		g_divergence = 1;
		return;
	}
	if (rc == 0) {
		if (!led_all(co, n, 0)) {
			FAILF("DIFF seed=%llu op=%ld alloc_hard returned non-free [%llu,%llu)",
			      ULL(g_curseed), g_curop, ULL(co), ULL(co + n));
			g_divergence = 1;
			return;
		}
		led_set(co, n, 1);
	}
}

static void do_soft_alloc(u64 r)
{
	u8 align = (u8)(r % 4);
	u64 n = 1 + (r >> 8) % 64;
	struct rfs_run_list rl;
	struct softout so;
	struct rfs_run_node *nd;
	bool csat = true, rsat = false;
	u64 cb[REF_RUNS], cl[REF_RUNS];
	int rc, rr, cn = 0, i, ok;

	memset(&so, 0, sizeof(so));
	rfs_run_list_init(&rl, rl_alloc());
	rc = rfs_slice_soft_alloc(&g_s, align, n, &rl, &csat);
	rr = ref_soft_alloc(&g_ref, align, n, &so, &rsat);
	for (nd = rl.head; nd; nd = nd->next) {
		if (cn < REF_RUNS) {
			cb[cn] = nd->run.base;
			cl[cn] = nd->run.len;
		}
		cn++;
	}
	if (rc != rr || csat != rsat || cn != so.n) {
		FAILF("DIFF seed=%llu op=%ld soft_alloc(align=%u,n=%llu): rc %d/%d sat %d/%d nruns %d/%d",
		      ULL(g_curseed), g_curop, align, ULL(n), rc, rr, csat, rsat, cn, so.n);
		g_divergence = 1;
		rfs_run_list_fini(&rl);
		return;
	}
	ok = 1;
	for (i = 0; i < cn; i++)
		if (cb[i] != so.b[i] || cl[i] != so.l[i])
			ok = 0;
	if (!ok) {
		FAILF("DIFF seed=%llu op=%ld soft_alloc run mismatch", ULL(g_curseed), g_curop);
		g_divergence = 1;
		rfs_run_list_fini(&rl);
		return;
	}
	if (rc == 0) {
		u64 sum = 0;

		for (i = 0; i < cn; i++) {
			if (!led_all(cb[i], cl[i], 0)) {
				FAILF("DIFF seed=%llu op=%ld soft_alloc non-free run [%llu,%llu)",
				      ULL(g_curseed), g_curop, ULL(cb[i]), ULL(cb[i] + cl[i]));
				g_divergence = 1;
				break;
			}
			led_set(cb[i], cl[i], 1);
			sum += cl[i];
		}
		if (!g_divergence && csat && sum != n) {
			FAILF("DIFF seed=%llu op=%ld soft_alloc sat but sum %llu != n %llu",
			      ULL(g_curseed), g_curop, ULL(sum), ULL(n));
			g_divergence = 1;
		}
		if (!g_divergence && sum > n) {
			FAILF("DIFF seed=%llu op=%ld soft_alloc over-delivered", ULL(g_curseed), g_curop);
			g_divergence = 1;
		}
	}
	rfs_run_list_fini(&rl);
}

static void do_soft_occupy(u64 r)
{
	u64 base = g_off + (r % (g_nbits + 2));
	u64 len = 1 + (r >> 8) % 48;
	u64 rest0 = (r >> 16) % (len + 1);
	u64 crest = rest0, rrest = rest0;
	int rc = rfs_slice_soft_occupy(&g_s, base, len, &crest);
	int rr = ref_soft_occupy(&g_ref, base, len, &rrest);

	if (rc != rr || crest != rrest) {
		FAILF("DIFF seed=%llu op=%ld soft_occupy(base=%llu,len=%llu,rest0=%llu): rc %d/%d rest %llu/%llu",
		      ULL(g_curseed), g_curop, ULL(base), ULL(len), ULL(rest0), rc, rr, ULL(crest), ULL(rrest));
		g_divergence = 1;
		return;
	}
	if (rc == 0) {
		/* claimed==0 即 real==0（rest0==len）的合法 no-op，不占位。*/
		u64 claimed = len - crest;

		if (claimed > 0) {
			if (!led_all(base, claimed, 0)) {
				FAILF("DIFF seed=%llu op=%ld soft_occupy claimed %llu non-free",
				      ULL(g_curseed), g_curop, ULL(claimed));
				g_divergence = 1;
				return;
			}
			led_set(base, claimed, 1);
		}
	}
}

static void do_free_interval(u64 r)
{
	u64 base = g_off + (r % (g_nbits + 2));
	u64 len = 1 + (r >> 8) % 48;
	int rc = rfs_slice_free_interval(&g_s, base, len);
	int rr = ref_free(&g_ref, base, len);

	if (rc != rr) {
		FAILF("DIFF seed=%llu op=%ld free_interval(base=%llu,len=%llu): rc %d/%d",
		      ULL(g_curseed), g_curop, ULL(base), ULL(len), rc, rr);
		g_divergence = 1;
		return;
	}
	if (rc == 0) {
		if (!led_all(base, len, 1)) {
			FAILF("DIFF seed=%llu op=%ld free_interval success on non-occupied [%llu,%llu)",
			      ULL(g_curseed), g_curop, ULL(base), ULL(base + len));
			g_divergence = 1;
			return;
		}
		led_set(base, len, 0);
	}
}

static void do_test_interval(u64 r)
{
	bool val = ((r & 1u) != 0);
	u64 base = g_off + (r % (g_nbits + 2));
	u64 len = (r >> 8) % 48;
	int rc = rfs_slice_test_interval(&g_s, val, base, len);
	int rr = ref_test(&g_ref, val, base, len);
	int lr = ledger_test(val, base, len);

	if (rc != rr) {
		FAILF("DIFF seed=%llu op=%ld test_interval(val=%d,base=%llu,len=%llu): rc %d/%d",
		      ULL(g_curseed), g_curop, val, ULL(base), ULL(len), rc, rr);
		g_divergence = 1;
		return;
	}
	if (rc != lr) {
		FAILF("LEDGER seed=%llu op=%ld test_interval rc %d vs ledger %d",
		      ULL(g_curseed), g_curop, rc, lr);
		g_divergence = 1;
	}
}

/* ══════════════════════════ 差分压测主循环 ══════════════════════════ */
static void stress_seed(u64 seed, u64 nbits, long ops)
{
	u64 off = 1000 + (seed % 7) * 37;
	u8 *bits;
	struct rfs_slice_cfg cfg;
	long op;

	g_curseed = seed;
	g_rng = seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;
	bits = bits_new(nbits);
	randomize_bitmap(bits, nbits);
	slice_zero(&g_s);
	memset(&cfg, 0, sizeof(cfg));
	cfg.slice_base = bits;
	cfg.in_global_base_idx = off;
	cfg.slice_bitscount = nbits;
	cfg.alloc = el_alloc();
	if (rfs_slice_enable(&g_s, &cfg) != 0) {
		FAILF("seed %llu: enable failed", ULL(seed));
		free(bits);
		return;
	}
	g_off = off;
	g_nbits = nbits;
	ref_build(&g_ref, off, nbits, bits);
	led_init(bits, off, nbits);
	free(bits);
	check_invariants("seed-init");

	for (op = 0; op < ops && !g_divergence; op++) {
		u64 r;

		g_curop = op;
		if (op > 0 && (op % 4096) == 0)
			check_invariants("periodic");
		if (g_divergence)
			break;
		if (op > 0 && (op % 20000) == 0) {
			/* 偶发 shutdown → 重 enable（随机位图），验证状态干净 */
			if (rfs_slice_shutdown(&g_s) != 0) {
				FAILF("seed %llu op %ld: shutdown failed", ULL(seed), op);
				g_divergence = 1;
				break;
			}
			if (g_el.live != 0) {
				FAILF("seed %llu op %ld: element pool not drained after shutdown", ULL(seed), op);
				g_divergence = 1;
				break;
			}
			bits = bits_new(nbits);
			randomize_bitmap(bits, nbits);
			memset(&cfg, 0, sizeof(cfg));
			cfg.slice_base = bits;
			cfg.in_global_base_idx = off;
			cfg.slice_bitscount = nbits;
			cfg.alloc = el_alloc();
			if (rfs_slice_enable(&g_s, &cfg) != 0) {
				FAILF("seed %llu: re-enable failed", ULL(seed));
				g_divergence = 1;
				free(bits);
				break;
			}
			ref_build(&g_ref, off, nbits, bits);
			led_init(bits, off, nbits);
			free(bits);
			check_invariants("post-reinit");
			continue;
		}
		r = rnd_next();
		if (r % 100 < 25)
			do_alloc_hard(r);
		else if (r % 100 < 40)
			do_soft_alloc(r);
		else if (r % 100 < 55)
			do_soft_occupy(r);
		else if (r % 100 < 80)
			do_free_interval(r);
		else
			do_test_interval(r);
	}
	if (!g_divergence) {
		check_invariants("seed-final");
		CHECK(rfs_slice_shutdown(&g_s) == 0, "seed: final shutdown");
	} else if (g_s.is_workable) {
		(void)rfs_slice_shutdown(&g_s);
	}
}

/* ══════════════════════════ main ══════════════════════════ */
int main(int argc, char **argv)
{
	long ops = 100000;
	int seeds = 4;
	u64 nbits = 1024;
	long total = 0;
	int i;

	if (argc > 1)
		ops = atol(argv[1]);
	if (argc > 2)
		seeds = atoi(argv[2]);
	if (argc > 3)
		nbits = strtoull(argv[3], NULL, 10);
	if (nbits < 64)
		nbits = 64;
	if (seeds < 0)
		seeds = 0;

	printf("=== ravenfs_allocator_slice adversarial stress ===\n");

	test_fusion();
	test_overlap();
	test_alloc_hard();
	test_soft_alloc();
	test_soft_occupy();
	test_boundaries();
	test_shutdown_reinit();
	test_free_intervals();
	test_rollback();
	test_free_interval_oom();

	if (ops > 0 && seeds > 0) {
		for (i = 0; i < seeds && !g_divergence; i++) {
			printf("[S] differential seed %d/%d ops=%ld nbits=%llu\n",
			       i + 1, seeds, ops, ULL(nbits));
			stress_seed((u64)(0xC0FFEEu + (u32)i * 1013904223u), nbits, ops);
			total += ops;
		}
		printf("--- differential ops total = %ld\n", total);
	}

	led_free();

	printf("--- pools: el allocs=%llu frees=%llu live=%llu | rl allocs=%llu frees=%llu live=%llu\n",
	       ULL(g_el.allocs), ULL(g_el.frees), ULL(g_el.live),
	       ULL(g_rl.allocs), ULL(g_rl.frees), ULL(g_rl.live));
	CHECK(g_el.live == 0 && g_el.allocs == g_el.frees, "final: element pool balanced");
	CHECK(g_rl.live == 0 && g_rl.allocs == g_rl.frees, "final: run-list pool balanced");

	printf("=== PASS %d  FAIL %d%s ===\n", g_pass, g_fail,
	       g_divergence ? "  (DIFFERENTIAL DIVERGENCE)" : "");
	return g_fail ? 1 : 0;
}

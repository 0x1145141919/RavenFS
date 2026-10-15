// SPDX-License-Identifier: GPL-2.0
/*
 * stress_allocator_router.c —— 顶层路由 struct rfs_allocator（⇄ god fs_allocator_t）
 *                              集成对抗性压力验证（测试组，独立于实现）
 *
 * 直接编译 ../linux/ravenfs_allocator_router.c + ../linux/ravenfs_allocator_slice.c
 * + ../linux/ravenfs_rbtree_intrusive.c（不链 god/ko 其它任何东西）。
 *
 * 与片级压测（stress_allocator_slice.c）的关键区别：本文件打的是**跨片路由层**
 * 的不变量——preferred 旋转 / 跨片拼接 / 跨片续圈 / 跨片释放 / 全局账一致性。
 *
 * 线程模型：单线程差分。路由用 RFS_CPU_ID()（= sched_getcpu()）索引 preferred 槽，
 * 故测试把当前线程**钉在首个可用 CPU** 上，令 cpu 索引恒定可复现；参考实现用同一索引。
 *
 * 验证层次：
 *   [A] 全局影子账本（整组位图，测试侧独立维护）——每次操作后对账：
 *         block_free_total()/inode_free_total() == 账本空闲位数；
 *         block_test/inode_test == 账本位态；
 *         dump_slice 逐片拼起来 == 账本空闲区间（跨界 run 按片切分）；
 *         每次 alloc/occupy 拿到的 run/inum 在账本里确为空闲、结果后确占（无重复交付）。
 *   [B] 差分：朴素全组参考实现（有序区间数组 + 线性扫，逐片建模）跑同一随机序列，
 *         逐操作对拍路由返回码、返回块号与交付 run 链（含 push_front 顺序）。
 *   [C] 不变量：preferred 旋转不越界；hard 单片原子不跨片；all-or-nothing 回滚后账复原；
 *         跨片后全局账一致；分配账 allocs==frees 且 fini 后 live==0。
 *   [D] 定向对抗：hard 跨片必拒 / soft 跨片凑满+回滚 / occupy 跨片续圈+顶片尾记账+
 *         锚点占用 / 末片短边界 / free+intervals_free 跨片链式 / init 失败自清理 /
 *         dump_slice 跨界 split / inode 跨片 + root 占用。
 *
 * 用法： ./ravenfs_stress_allocator_router [ops_per_seed] [seeds]
 *        默认 ops=120000，seeds=4（总 ≥ 4.8e5 操作，多种几何）。
 */
#include "ravenfs_allocator_top.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════ 记账 / 宏 ══════════════════════════ */
static int g_pass, g_fail;
static int g_div;			/* 差分首次分歧即停该种子 */
static u64 g_curseed;
static long g_curop;
static long g_stat_hard, g_stat_hard_fail, g_stat_soft, g_stat_soft_fail,
	    g_stat_occ, g_stat_occ_partial, g_stat_occ_fail, g_stat_free,
	    g_stat_ifree, g_stat_test;

#define ULL(x) ((unsigned long long)(x))

#define CHECK(cond, msg) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } \
} while (0)

#define FAILF(...) do { g_fail++; printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* ══════════════════════════ 计数分配器（全经注入）══════════════════════════ */
struct pool {
	u64	allocs, frees, live;
	long long fail_in;	/* -1 = 从不失败；0 = 下次失败 */
};
static struct pool g_pool = { 0, 0, 0, -1 };

static void *pool_alloc(void *ctx, u64 size)
{
	struct pool *p = (struct pool *)ctx;
	void *m;

	if (p->fail_in == 0) {
		p->fail_in = -1;
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

static struct rfs_mem_alloc g_mem = { pool_alloc, pool_free, &g_pool };
static struct rfs_run_alloc g_ra = { pool_alloc, pool_free, &g_pool };

/* ══════════════════════════ RNG（xorshift64）══════════════════════════ */
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

/* ══════════════════════════ 原始位图（整组）══════════════════════════ */
struct raw {
	u8	*mem;
	u64	 nbits;
};

static void raw_init(struct raw *r, u64 nbits)
{
	size_t bytes = (size_t)((nbits + 7) / 8);
	void *p = NULL;

	if (posix_memalign(&p, 4096, bytes ? bytes : 1) != 0) {
		perror("posix_memalign");
		exit(2);
	}
	memset(p, 0xFF, bytes);		/* 1 = 占用 */
	r->mem = (u8 *)p;
	r->nbits = nbits;
}

static void raw_free(struct raw *r)
{
	free(r->mem);
	r->mem = NULL;
}

/* 置空闲（LSB-first，与片 enable 扫描同口径）。*/
static void raw_clear(struct raw *r, u64 start, u64 n)
{
	u64 k;

	for (k = 0; k < n; k++)
		r->mem[(start + k) >> 3] &= (u8)~(1u << ((start + k) & 7));
}

static void raw_set(struct raw *r, u64 start, u64 n)
{
	u64 k;

	for (k = 0; k < n; k++)
		r->mem[(start + k) >> 3] |= (u8)(1u << ((start + k) & 7));
}

/* 伪随机铺位图：交替「占用间隙 / 空闲 run」——控制 run 数，避免区间数组爆炸。
 * 空闲占比 ~80%（free run 1..256，间隙 1..64）。*/
static void raw_random(struct raw *r, u64 seed)
{
	u64 i = 0;
	u64 x = seed | 1ULL;

	while (i < r->nbits) {
		u64 gap, len;

		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		gap = 1 + x % 64;
		i += gap;
		if (i >= r->nbits)
			break;
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		len = 1 + x % 160;
		if (i + len > r->nbits)
			len = r->nbits - i;
		raw_clear(r, i, len);
		i += len;
	}
}

/* ══════════════════ v2.0 inode 数据面靶（由 inode 位图派生 fr）══════════════════
 * 复用「按位图摆放空闲」的既有测试意图：把 inode 位图的空闲 run 派生成 fr（inode
 * 单位，不含 idx0），片数 = nbits/32（1 片 = 32 inode）、翻译面单 direct extent。*/
#define STRESS_MAX_FR 400000
static struct inode_dataplane_coordinate_free_range_t g_ifr[STRESS_MAX_FR];
static struct rfs_inode_extent g_iext[1];

static u64 derive_inode_fr(const struct raw *i)
{
	u64 n = 0;

	for (u64 k = 0; k < i->nbits; ) {
		u64 start;

		if ((i->mem[k >> 3] >> (k & 7)) & 1u) {
			k++;
			continue;
		}
		start = k;
		while (k < i->nbits &&
		       (((i->mem[k >> 3] >> (k & 7)) & 1u) == 0))
			k++;
		if (n >= STRESS_MAX_FR) {
			printf("  FATAL inode fr overflow\n");
			exit(2);
		}
		g_ifr[n].base = start;
		g_ifr[n].len = k - start;
		n++;
	}
	return n;
}

static void cfg_set_inode_plane(struct rfs_slices_cfg *cfg, struct raw *i)
{
	u64 pieces = i->nbits / RFS_IDP_INODES_PER_PIECE;

	g_iext[0].lblk = 0;
	g_iext[0].phys_base = 0;
	g_iext[0].length = pieces * 2ULL;
	cfg->inode.pieces = pieces;
	cfg->inode.extents = g_iext;
	cfg->inode.extent_count = 1;
	cfg->inode.fr = g_ifr;
	cfg->inode.fr_count = derive_inode_fr(i);
	cfg->inode.grow_batch_pieces = 1;
	cfg->inode.mem = g_mem;
}

/* ══════════════════════════ 影子账本（整组位图）══════════════════════════ */
struct ledger {
	u8	*b;
	u64	 nbits;
	u64	 freebits;
};
static struct ledger g_lb;		/* block 组 */
static struct ledger g_li;		/* inode 组 */

static void led_init(struct ledger *L, const u8 *src, u64 nbits)
{
	size_t bytes = (size_t)((nbits + 7) / 8);
	u64 i, c = 0;

	free(L->b);
	L->b = (u8 *)malloc(bytes ? bytes : 1);
	memcpy(L->b, src, bytes);
	L->nbits = nbits;
	for (i = 0; i < nbits; i++)
		if (((L->b[i >> 3] >> (i & 7)) & 1u) == 0)
			c++;
	L->freebits = c;
}

static void led_fini(struct ledger *L)
{
	free(L->b);
	L->b = NULL;
}

static int led_get(const struct ledger *L, u64 i)
{
	return (L->b[i >> 3] >> (i & 7)) & 1u;
}

/* 设 [base,base+len) 为 occ（1=占用）。同步 freebits。*/
static void led_set(struct ledger *L, u64 base, u64 len, int occ)
{
	u64 k;

	for (k = 0; k < len; k++) {
		u64 i = base + k;
		int old = led_get(L, i);

		if (old == occ)
			continue;
		if (occ)
			L->b[i >> 3] |= (u8)(1u << (i & 7));
		else
			L->b[i >> 3] &= (u8)~(1u << (i & 7));
		L->freebits += occ ? (u64)-1 : (u64)1;
	}
}

static int led_range_free(const struct ledger *L, u64 base, u64 len)
{
	u64 k;

	for (k = 0; k < len; k++)
		if (led_get(L, base + k) != 0)
			return 0;
	return 1;
}

/* ══════════════════════════ 参考实现（逐片有序区间数组 + 线性扫）══════════════════════════ */
#define REF_RUNS 8192
#define MAXSLICE 16
#define MAXCPU 64

struct rrefiv {
	u64 base, len;
};

struct rrefslice {
	u64		 off, nbits, free_count;
	struct rrefiv	*v;
	int		 n, cap;
};

struct refside {
	struct rrefslice s[MAXSLICE];
	int	 ns;
	u64	 per, nbits;
	u32	 pref[MAXCPU];
};

/* 段序列（soft 消费顺序；router 用 push_front → 返回链 = 该序列逆序）。*/
static struct rrefiv g_seq[REF_RUNS];
static int g_seqn;

static void rr_insert(struct rrefslice *s, u64 base, u64 len)
{
	int i;

	if (s->n >= s->cap) {
		printf("  FATAL ref interval overflow cap=%d\n", s->cap);
		exit(2);
	}
	for (i = s->n - 1; i >= 0 && s->v[i].base > base; i--)
		s->v[i + 1] = s->v[i];
	s->v[i + 1].base = base;
	s->v[i + 1].len = len;
	s->n++;
}

/* 追加（仅当 base 严格递增时用；建表扫描天然递增）。*/
static void rr_append(struct rrefslice *s, u64 base, u64 len)
{
	if (s->n >= s->cap) {
		printf("  FATAL ref interval overflow cap=%d\n", s->cap);
		exit(2);
	}
	s->v[s->n].base = base;
	s->v[s->n].len = len;
	s->n++;
}

static void rr_erase(struct rrefslice *s, int i)
{
	memmove(&s->v[i], &s->v[i + 1],
		(size_t)(s->n - i - 1) * sizeof(s->v[0]));
	s->n--;
}

static int rr_cover_or_after(struct rrefslice *s, u64 base)
{
	int i;

	for (i = 0; i < s->n; i++)
		if (s->v[i].base + s->v[i].len > base)
			return i;
	return -1;
}

static int rr_test(struct rrefslice *s, bool val, u64 base, u64 len)
{
	u64 off;
	int iv;

	if (base < s->off)
		return -EINVAL;
	off = base - s->off;
	if (off > s->nbits || len > s->nbits - off)
		return -EINVAL;
	if (len == 0)
		return 0;
	iv = rr_cover_or_after(s, base);
	if (!val) {
		if (iv >= 0 && s->v[iv].base <= base &&
		    base + len <= s->v[iv].base + s->v[iv].len)
			return 0;
		return -ENOENT;
	}
	if (iv >= 0 && s->v[iv].base < base + len)
		return -ENOENT;
	return 0;
}

static int rr_free_interval(struct rrefslice *s, u64 base, u64 len)
{
	u64 off, nb, ne;
	int it, li;
	bool ml, mr;

	if (base < s->off)
		return -EINVAL;
	off = base - s->off;
	if (off > s->nbits || len > s->nbits - off)
		return -EINVAL;
	if (len == 0)
		return 0;
	it = rr_cover_or_after(s, base);
	li = (it >= 0) ? (it - 1) : (s->n - 1);
	if (li >= 0 && s->v[li].base + s->v[li].len > base)
		return -EEXIST;
	if (it >= 0 && s->v[it].base < base + len)
		return -EEXIST;
	ml = (li >= 0) && (s->v[li].base + s->v[li].len == base);
	mr = (it >= 0) && (s->v[it].base == base + len);
	nb = ml ? s->v[li].base : base;
	ne = mr ? (s->v[it].base + s->v[it].len) : (base + len);
	if (ml && mr) {
		rr_erase(s, it);
		rr_erase(s, li);
	} else if (ml) {
		rr_erase(s, li);
	} else if (mr) {
		rr_erase(s, it);
	}
	rr_insert(s, nb, ne - nb);
	s->free_count += len;
	return 0;
}

static int rr_min_size(struct rrefslice *s)
{
	int b = -1, i;

	for (i = 0; i < s->n; i++) {
		if (b < 0 || s->v[i].len < s->v[b].len ||
		    (s->v[i].len == s->v[b].len && s->v[i].base < s->v[b].base))
			b = i;
	}
	return b;
}

static int rr_next_size(struct rrefslice *s, int idx)
{
	u64 kl = s->v[idx].len, kb = s->v[idx].base;
	int b = -1, i;

	for (i = 0; i < s->n; i++) {
		if (!(s->v[i].len > kl || (s->v[i].len == kl && s->v[i].base > kb)))
			continue;
		if (b < 0 || s->v[i].len < s->v[b].len ||
		    (s->v[i].len == s->v[b].len && s->v[i].base < s->v[b].base))
			b = i;
	}
	return b;
}

static int rr_alloc_hard(struct rrefslice *s, u8 align, u64 n, u64 *out)
{
	u64 mask;
	int best = -1, i;

	if (n == 0)
		return -EINVAL;
	if (align > 63)
		return -EINVAL;
	mask = align ? ((1ULL << align) - 1ULL) : 0ULL;
	for (i = 0; i < s->n; i++) {
		u64 end, st;

		if (s->v[i].len < n)
			continue;
		end = s->v[i].base + s->v[i].len;
		st = (s->v[i].base + mask) & ~mask;	/* D-14：头挖 */
		if (st + n > end)
			continue;		/* 头对齐越出右沿 → 跳过 */
		if (best < 0 || s->v[i].len < s->v[best].len ||
		    (s->v[i].len == s->v[best].len && s->v[i].base < s->v[best].base))
			best = i;
	}
	if (best < 0)
		return -ENOSPC;
	{
		u64 base = s->v[best].base, len = s->v[best].len;
		u64 end = base + len, st = (base + mask) & ~mask, rb;

		rr_erase(s, best);
		if (st > base)
			rr_insert(s, base, st - base);
		rb = st + n;
		if (rb < end)
			rr_insert(s, rb, end - rb);
		s->free_count -= n;
		*out = st;
	}
	return 0;
}

static int rr_soft_alloc(struct rrefslice *s, u8 align, u64 n)
{
	u64 mask, remain, taken = 0;
	int idx;

	if (align > 63)
		return -EINVAL;
	if (n == 0)
		return -EINVAL;
	mask = align ? ((1ULL << align) - 1ULL) : 0ULL;
	remain = n;
	idx = rr_min_size(s);
	while (idx >= 0 && remain > 0) {
		u64 ivb = s->v[idx].base, ivl = s->v[idx].len, ive = ivb + ivl;
		u64 st = (ivb + mask) & ~mask;		/* D-14：头挖 */
		u64 t;

		if (st >= ive) {
			idx = rr_next_size(s, idx);
			continue;
		}
		t = ((ive - st) < remain) ? (ive - st) : remain;
		if (g_seqn >= REF_RUNS) {
			printf("  FATAL ref soft overflow\n");
			exit(2);
		}
		rr_erase(s, idx);
		if (st > ivb)
			rr_insert(s, ivb, st - ivb);
		{
			u64 rb = st + t;

			if (rb < ive)
				rr_insert(s, rb, ive - rb);
		}
		s->free_count -= t;
		g_seq[g_seqn].base = st;
		g_seq[g_seqn].len = t;
		g_seqn++;
		taken += t;
		remain -= t;
		idx = rr_min_size(s);
	}
	if (taken == 0)
		return -ENOSPC;
	return 0;
}

static int rr_soft_occupy(struct rrefslice *s, u64 base, u64 len, u64 *rest)
{
	u64 real, slice_end, ivb, ive, claimed, rb;
	int ci, i;

	if (*rest > len)
		return -EINVAL;
	if (len == 0)
		return 0;
	real = len - *rest;
	if (real == 0)
		return 0;
	if (base < s->off)
		return -EINVAL;
	slice_end = s->off + s->nbits;
	if (base > slice_end)
		return -EINVAL;
	ci = -1;
	for (i = 0; i < s->n; i++)
		if (s->v[i].base <= base && base < s->v[i].base + s->v[i].len) {
			ci = i;
			break;
		}
	if (ci < 0) {
		*rest = len;
		return -ENOSPC;
	}
	ivb = s->v[ci].base;
	ive = ivb + s->v[ci].len;
	claimed = ive - base;
	if (claimed > real)
		claimed = real;
	if (base + claimed > slice_end)
		claimed = slice_end - base;
	if (claimed == 0) {
		*rest = len;
		return -ENOSPC;
	}
	rr_erase(s, ci);
	if (base > ivb)
		rr_insert(s, ivb, base - ivb);
	rb = base + claimed;
	if (rb < ive)
		rr_insert(s, rb, ive - rb);
	s->free_count -= claimed;
	*rest = len - claimed;
	return 0;
}

/* ── refside ── */
static void refside_init(struct refside *R, u64 per, int ns, u64 nbits,
			 const u8 *raw, int ncpu)
{
	int i;

	R->ns = ns;
	R->per = per;
	R->nbits = nbits;
	for (i = 0; i < ncpu; i++)
		R->pref[i] = (u32)(i % ns);
	for (i = 0; i < ns; i++) {
		u64 begin = (u64)i * per;
		u64 cnt = nbits - begin;
		struct rrefslice *s = &R->s[i];
		u64 k;

		if (cnt > per)
			cnt = per;
		s->off = begin;
		s->nbits = cnt;
		s->free_count = 0;
		s->cap = (int)(cnt + 2);
		s->v = (struct rrefiv *)malloc((size_t)s->cap * sizeof(s->v[0]));
		s->n = 0;
		k = 0;
		while (k < cnt) {
			u64 st;

			if ((raw[(begin + k) >> 3] >> ((begin + k) & 7)) & 1u) {
				k++;
				continue;
			}
			st = k;
			while (k < cnt &&
			       (((raw[(begin + k) >> 3] >> ((begin + k) & 7)) & 1u) == 0))
				k++;
			rr_append(s, begin + st, k - st);
			s->free_count += k - st;
		}
	}
}

static void refside_fini(struct refside *R)
{
	int i;

	for (i = 0; i < R->ns; i++) {
		free(R->s[i].v);
		R->s[i].v = NULL;
	}
}

static u64 refside_free_total(struct refside *R)
{
	u64 sum = 0;
	int i;

	for (i = 0; i < R->ns; i++)
		sum += R->s[i].free_count;
	return sum;
}

static int ref_block_alloc_hard(struct refside *R, u32 cpu, u8 align, u64 n, u64 *out)
{
	int round;

	for (round = 0; round < R->ns; round++) {
		u64 idx = ((u64)R->pref[cpu] + (u64)round) % (u64)R->ns;

		if (rr_alloc_hard(&R->s[idx], align, n, out) == 0) {
			R->pref[cpu] = (u32)idx;
			return 0;
		}
	}
	return -ENOSPC;
}

static u64 seq_sum(void)
{
	u64 s = 0;
	int i;

	for (i = 0; i < g_seqn; i++)
		s += g_seq[i].len;
	return s;
}

static void ref_rollback(struct refside *R)
{
	int i;

	for (i = 0; i < g_seqn; i++) {
		u64 idx = g_seq[i].base / R->per;

		(void)rr_free_interval(&R->s[idx], g_seq[i].base, g_seq[i].len);
	}
	g_seqn = 0;
}

static int ref_block_alloc_soft(struct refside *R, u32 cpu, u8 align, u64 n)
{
	int round;
	u64 sum = 0;

	g_seqn = 0;
	for (round = 0; round < R->ns; round++) {
		u64 idx;
		u64 remain;
		int rc;

		if (sum >= n)
			break;
		idx = ((u64)R->pref[cpu] + (u64)round) % (u64)R->ns;
		remain = n - sum;
		rc = rr_soft_alloc(&R->s[idx], align, remain);
		if (rc == 0) {
			R->pref[cpu] = (u32)idx;
		} else if (rc == -ENOSPC) {
			continue;
		} else {
			ref_rollback(R);
			return rc;
		}
		sum = seq_sum();
	}
	if (sum >= n)
		return 0;
	ref_rollback(R);
	return -ENOSPC;
}

static int ref_block_occupy_soft(struct refside *R, u64 base, u64 n, u64 *rest)
{
	u64 remain = n;

	*rest = n;
	if (n == 0)
		return -EINVAL;
	while (remain > 0 && (base / R->per) < (u64)R->ns) {
		u64 idx = base / R->per;
		u64 left = 0;
		u64 claimed;
		int rc = rr_soft_occupy(&R->s[idx], base, remain, &left);

		if (rc != 0)
			break;
		claimed = remain - left;
		base += claimed;
		remain = left;
		if (remain == 0)
			break;
		if (base != (idx + 1) * R->per)
			break;
	}
	*rest = remain;
	return (n - remain > 0) ? 0 : -ENOSPC;
}

static int ref_block_free(struct refside *R, u64 base, u64 count)
{
	if (count == 0)
		return 0;
	while (count > 0) {
		u64 idx = base / R->per;
		u64 end, m;
		int rc;

		if (idx >= (u64)R->ns)
			return -EINVAL;
		end = (idx + 1) * R->per;
		m = count;
		if (base + m > end)
			m = end - base;
		if (m == 0)
			return -EINVAL;
		rc = rr_free_interval(&R->s[idx], base, m);
		if (rc)
			return rc;
		base += m;
		count -= m;
	}
	return 0;
}

static bool ref_block_test(struct refside *R, u64 blk)
{
	u64 idx = blk / R->per;

	if (idx >= (u64)R->ns)
		return false;
	return rr_test(&R->s[idx], true, blk, 1) == 0;
}

static int ref_inode_alloc(struct refside *R, u32 cpu, u64 *out)
{
	int round;

	for (round = 0; round < R->ns; round++) {
		u64 idx = ((u64)R->pref[cpu] + (u64)round) % (u64)R->ns;

		if (rr_alloc_hard(&R->s[idx], 0, 1, out) == 0) {
			R->pref[cpu] = (u32)idx;
			return 0;
		}
	}
	return -ENOSPC;
}

static void ref_inode_free(struct refside *R, u64 inum)
{
	u64 idx = inum / R->per;

	if (idx >= (u64)R->ns)
		return;
	(void)rr_free_interval(&R->s[idx], inum, 1);
}

/* ══════════════════════════ 小工具 ══════════════════════════ */
static u64 rl_sum(const struct rfs_run_list *l)
{
	const struct rfs_run_node *n;
	u64 s = 0;

	for (n = l->head; n; n = n->next)
		s += n->run.len;
	return s;
}

static int rl_count(const struct rfs_run_list *l)
{
	const struct rfs_run_node *n;
	int c = 0;

	for (n = l->head; n; n = n->next)
		c++;
	return c;
}

/* ══════════════════════════ 全局状态 ══════════════════════════ */
static struct rfs_allocator g_a;
static struct refside g_rsb, g_rsi;
static u32 g_cpu;			/* 钉住的物理 CPU */

/* 未归还的已分配区间（块组）。*/
#define MAXOUT 200000
struct outiv {
	u64 base, len;
};
static struct outiv *g_out;
static int g_outn;

/* 未归还 inode（LIFO 归还以抑制碎片）。*/
static u64 *g_iout;
static int g_ioutn;

static u32 cur_cpu(void)
{
	u32 cpu = (u32)RFS_CPU_ID();

	if (cpu != g_cpu) {
		FAILF("affinity unstable: cpu %u != pinned %u", cpu, g_cpu);
		g_div = 1;
	}
	return cpu % rfs_allocator_cpu_slots(&g_a);
}

/* ══════════════════════════ 对账 ══════════════════════════ */

/* block：dump_slice 逐片 → 与账本 [begin,end) 的空闲极大 run 流式对比。
 * inode：v2.0 数据面无分片 → 一次性 dump_fr 与账本 [0,nbits) 全量对比。*/
static void check_dump_side(const char *tag, int ino_sd)
{
	struct ledger *L = ino_sd ? &g_li : &g_lb;
	u64 per = rfs_allocator_block_per_slice_bits(&g_a);
	u32 ns = rfs_allocator_slice_count(&g_a);
	u32 i;

	if (ino_sd) {
		struct inode_dataplane_coordinate_free_range_t *fr;
		u64 n = rfs_inode_plane_fr_count(&g_a.ino_plane);
		u64 k = 0, p = 0;
		int ok = 1;

		fr = (struct inode_dataplane_coordinate_free_range_t *)
			malloc((size_t)(n ? n : 1) * sizeof(*fr));
		if (!fr) {
			FAILF("%s: fr malloc", tag);
			g_div = 1;
			return;
		}
		if (rfs_inode_plane_dump_fr(&g_a.ino_plane, fr, n) != 0) {
			FAILF("%s: idp dump_fr rc!=0", tag);
			g_div = 1;
			free(fr);
			return;
		}
		while (p < L->nbits) {
			u64 st;

			if (led_get(L, p) == 1) {
				p++;
				continue;
			}
			st = p;
			while (p < L->nbits && led_get(L, p) == 0)
				p++;
			if (k >= n || fr[k].base != st || fr[k].len != p - st) {
				ok = 0;
				break;
			}
			k++;
		}
		if (ok && k != n)
			ok = 0;			/* 路由多吐了 run */
		if (!ok)
			FAILF("%s: idp fr != ledger", tag);
		else
			g_pass++;
		free(fr);
		return;
	}

	for (i = 0; i < ns; i++) {
		u64 begin = (u64)i * per;
		u64 end = begin + per;
		struct rfs_run_dlist dl;
		struct rfs_run_node *nd;
		u64 p;
		int ok = 1;

		if (end > L->nbits)
			end = L->nbits;
		rfs_run_dlist_init(&dl, g_ra);
		if (rfs_allocator_dump_slice(&g_a, i, &dl) != 0) {
			FAILF("%s: dump_slice(blk,%u) rc!=0", tag, i);
			rfs_run_dlist_fini(&dl);
			continue;
		}
		nd = dl.head;
		p = begin;
		while (p < end) {
			u64 st;

			if (led_get(L, p) == 1) {
				p++;
				continue;
			}
			st = p;
			while (p < end && led_get(L, p) == 0)
				p++;
			if (!nd || nd->run.base != st || nd->run.len != p - st) {
				ok = 0;
				break;
			}
			nd = nd->next;
		}
		if (ok && nd)
			ok = 0;			/* 路由多吐了 run */
		if (!ok)
			FAILF("%s: dump_slice(blk,%u) != ledger (block %llu..%llu)",
			      tag, i, ULL(begin), ULL(end));
		else
			g_pass++;
		rfs_run_dlist_fini(&dl);
	}
}

static void reconcile(const char *tag)
{
	u32 ns = rfs_allocator_slice_count(&g_a);
	u32 c;

	CHECK(rfs_allocator_block_free_total(&g_a) == g_lb.freebits,
	      "reconcile: block_free_total == ledger free bits");
	CHECK(rfs_allocator_inode_free_total(&g_a) == g_li.freebits,
	      "reconcile: inode_free_total == ledger free bits");
	CHECK(refside_free_total(&g_rsb) == g_lb.freebits,
	      "reconcile: ref block free == ledger free bits");
	CHECK(refside_free_total(&g_rsi) == g_li.freebits,
	      "reconcile: ref inode free == ledger free bits");

	/* preferred 旋转不越界 */
	for (c = 0; c < rfs_allocator_cpu_slots(&g_a); c++) {
		if (rfs_allocator_block_preferred_of(&g_a, c) >= ns) {
			FAILF("%s: preferred out of range (cpu %u)", tag, c);
			g_div = 1;
			return;
		}
	}

	check_dump_side(tag, 0);
	check_dump_side(tag, 1);

	/* 抽测 test == 账本 */
	{
		u64 k;

		for (k = 0; k < 64; k++) {
			u64 b = (rnd_next() % g_lb.nbits);
			u64 in = (rnd_next() % g_li.nbits);

			if (rfs_allocator_block_test(&g_a, b) !=
			    (led_get(&g_lb, b) == 1)) {
				FAILF("%s: block_test(%llu) != ledger", tag, ULL(b));
				g_div = 1;
			}
			if (ref_block_test(&g_rsb, b) != (led_get(&g_lb, b) == 1)) {
				FAILF("%s: ref block_test(%llu) != ledger", tag, ULL(b));
				g_div = 1;
			}
			if (rfs_allocator_inode_test(&g_a, in) !=
			    (led_get(&g_li, in) == 1)) {
				FAILF("%s: inode_test(%llu) != ledger", tag, ULL(in));
				g_div = 1;
			}
		}
	}
}

/* 未归还区间表操作 */
static void out_push(u64 base, u64 len)
{
	if (g_outn >= MAXOUT) {
		printf("  FATAL out overflow\n");
		exit(2);
	}
	g_out[g_outn].base = base;
	g_out[g_outn].len = len;
	g_outn++;
}

static void out_remove(int i)
{
	g_out[i] = g_out[g_outn - 1];
	g_outn--;
}

/* ══════════════════════════ 差分操作 ══════════════════════════ */

static void do_hard(u64 r)
{
	u8 align = (u8)(r % 7);
	u64 n = 1 + (r >> 8) % 256;
	u64 ro = 0xDEADBEEFCAFEBABEULL;
	u64 cr;
	int rr;

	g_stat_hard++;
	cr = rfs_allocator_block_alloc_hard(&g_a, n, align);
	rr = ref_block_alloc_hard(&g_rsb, cur_cpu(), align, n, &ro);
	if (g_div)
		return;
	if ((cr != RFS_IV_NO_HIT) != (rr == 0) ||
	    (rr == 0 && cr != ro)) {
		FAILF("DIFF seed=%llu op=%ld alloc_hard(align=%u,n=%llu): router=%llx ref rc=%d out=%llx",
		      ULL(g_curseed), g_curop, align, ULL(n), ULL(cr), rr, ULL(ro));
		g_div = 1;
		return;
	}
	if (rr == 0) {
		u64 per = rfs_allocator_block_per_slice_bits(&g_a);
		if (!led_range_free(&g_lb, cr, n)) {
			FAILF("DIFF seed=%llu op=%ld alloc_hard delivered non-free [%llu,%llu)",
			      ULL(g_curseed), g_curop, ULL(cr), ULL(cr + n));
			g_div = 1;
			return;
		}
		if (cr / per != (cr + n - 1) / per) {
			FAILF("DIFF seed=%llu op=%ld alloc_hard spans slices [%llu,%llu)",
			      ULL(g_curseed), g_curop, ULL(cr), ULL(cr + n));
			g_div = 1;
			return;
		}
		if (cr + n > g_lb.nbits) {
			FAILF("DIFF seed=%llu op=%ld alloc_hard beyond nbits", ULL(g_curseed), g_curop);
			g_div = 1;
			return;
		}
		led_set(&g_lb, cr, n, 1);
		out_push(cr, n);
	} else {
		g_stat_hard_fail++;
	}
}

static void do_soft(u64 r)
{
	u8 align = (u8)(r % 5);
	u64 n = 1 + (r >> 8) % 256;
	struct rfs_run_list rl;
	const struct rfs_run_node *nd;
	u64 lfree0 = g_lb.freebits;
	int rr, cr, i, cn;
	int ok = 1;

	g_stat_soft++;
	rr = ref_block_alloc_soft(&g_rsb, cur_cpu(), align, n);
	rfs_run_list_init(&rl, g_ra);
	cr = rfs_allocator_block_alloc_soft(&g_a, n, align, &rl);
	if (g_div) {
		rfs_run_list_fini(&rl);
		return;
	}
	if (cr != rr) {
		FAILF("DIFF seed=%llu op=%ld alloc_soft(align=%u,n=%llu): rc %d/%d",
		      ULL(g_curseed), g_curop, align, ULL(n), cr, rr);
		g_div = 1;
		rfs_run_list_fini(&rl);
		return;
	}
	cn = rl_count(&rl);
	if (cr == 0) {
		/* 期望链 = g_seq 逆序（push_front）*/
		nd = rl.head;
		i = g_seqn - 1;
		while (nd) {
			if (i < 0 || nd->run.base != g_seq[i].base ||
			    nd->run.len != g_seq[i].len) {
				ok = 0;
				break;
			}
			nd = nd->next;
			i--;
		}
		if (ok && i != -1)
			ok = 0;
		if (!ok) {
			FAILF("DIFF seed=%llu op=%ld alloc_soft run chain mismatch (router n=%d ref n=%d)",
			      ULL(g_curseed), g_curop, cn, g_seqn);
			g_div = 1;
			rfs_run_list_fini(&rl);
			return;
		}
		if (rl_sum(&rl) < n) {
			FAILF("DIFF seed=%llu op=%ld alloc_soft rc=0 but delivered %llu < %llu",
			      ULL(g_curseed), g_curop, ULL(rl_sum(&rl)), ULL(n));
			g_div = 1;
			rfs_run_list_fini(&rl);
			return;
		}
		for (nd = rl.head; nd; nd = nd->next) {
			if (!led_range_free(&g_lb, nd->run.base, nd->run.len)) {
				FAILF("DIFF seed=%llu op=%ld alloc_soft non-free run",
				      ULL(g_curseed), g_curop);
				g_div = 1;
				break;
			}
			led_set(&g_lb, nd->run.base, nd->run.len, 1);
			out_push(nd->run.base, nd->run.len);
		}
	} else {
		/* 失败：链必须回空，账复原 */
		g_stat_soft_fail++;
		if (rl.head != NULL) {
			FAILF("DIFF seed=%llu op=%ld alloc_soft failed but runs not emptied",
			      ULL(g_curseed), g_curop);
			g_div = 1;
		}
		if (g_lb.freebits != lfree0) {
			FAILF("DIFF seed=%llu op=%ld alloc_soft rollback changed ledger %llu->%llu",
			      ULL(g_curseed), g_curop, ULL(lfree0), ULL(g_lb.freebits));
			g_div = 1;
		}
	}
	rfs_run_list_fini(&rl);
}

static void do_occupy(u64 r)
{
	u64 base = r % (g_lb.nbits + 2);
	u64 n = 1 + (r >> 8) % 256;
	u64 crest = 0, rrest = 0;
	int cr, rr;

	g_stat_occ++;
	cr = rfs_allocator_block_occupy_soft(&g_a, base, n, &crest);
	rr = ref_block_occupy_soft(&g_rsb, base, n, &rrest);
	if (g_div)
		return;
	if (cr != rr || crest != rrest) {
		FAILF("DIFF seed=%llu op=%ld occupy(base=%llu,n=%llu): rc %d/%d rest %llu/%llu",
		      ULL(g_curseed), g_curop, ULL(base), ULL(n), cr, rr,
		      ULL(crest), ULL(rrest));
		g_div = 1;
		return;
	}
	if (cr == 0) {
		u64 claimed = n - crest;

		if (crest > 0)
			g_stat_occ_partial++;
		if (claimed == 0) {
			FAILF("DIFF seed=%llu op=%ld occupy rc=0 claimed=0", ULL(g_curseed), g_curop);
			g_div = 1;
			return;
		}
		if (!led_range_free(&g_lb, base, claimed)) {
			FAILF("DIFF seed=%llu op=%ld occupy claimed non-free", ULL(g_curseed), g_curop);
			g_div = 1;
			return;
		}
		if (base + claimed > g_lb.nbits) {
			FAILF("DIFF seed=%llu op=%ld occupy beyond nbits", ULL(g_curseed), g_curop);
			g_div = 1;
			return;
		}
		led_set(&g_lb, base, claimed, 1);
		out_push(base, claimed);
	} else {
		g_stat_occ_fail++;
	}
}

static void do_free(u64 r)
{
	int i;
	u64 base, len;
	int cr, rr;

	g_stat_free++;
	if (g_outn == 0)
		return;
	i = ((r % 100) < 80) ? (g_outn - 1) : (int)(r % (u64)g_outn);
	base = g_out[i].base;
	len = g_out[i].len;
	out_remove(i);

	cr = rfs_allocator_block_free(&g_a, base, len);
	rr = ref_block_free(&g_rsb, base, len);
	if (g_div)
		return;
	if (cr != rr) {
		FAILF("DIFF seed=%llu op=%ld block_free(%llu,%llu): rc %d/%d",
		      ULL(g_curseed), g_curop, ULL(base), ULL(len), cr, rr);
		g_div = 1;
		return;
	}
	if (cr == 0) {
		/* 释放前必须全占 */
		u64 k;

		for (k = 0; k < len; k++)
			if (led_get(&g_lb, base + k) == 0) {
				FAILF("DIFF seed=%llu op=%ld block_free of non-occupied",
				      ULL(g_curseed), g_curop);
				g_div = 1;
				return;
			}
		led_set(&g_lb, base, len, 0);
	}
}

static void do_intervals_free(u64 r)
{
	struct rfs_run_list rl;
	int k, j;
	u64 bases[8], lens[8];
	int cr;

	g_stat_ifree++;
	if (g_outn == 0)
		return;
	k = 1 + (int)(r % 4u);
	if (k > g_outn)
		k = g_outn;
	if (k > 8)
		k = 8;
	rfs_run_list_init(&rl, g_ra);
	for (j = 0; j < k; j++) {
		int i = g_outn - 1;		/* 取末尾 */

		bases[j] = g_out[i].base;
		lens[j] = g_out[i].len;
		out_remove(i);
		if (rfs_run_list_push_front(&rl, bases[j], lens[j]) != 0) {
			FAILF("push_front failed");
			g_div = 1;
			rfs_run_list_fini(&rl);
			return;
		}
	}

	cr = rfs_allocator_block_intervals_free(&g_a, &rl);
	if (g_div) {
		rfs_run_list_fini(&rl);
		return;
	}
	if (cr != 0 || rl.head != NULL) {
		FAILF("DIFF seed=%llu op=%ld intervals_free rc=%d head=%p",
		      ULL(g_curseed), g_curop, cr, (void *)rl.head);
		g_div = 1;
		rfs_run_list_fini(&rl);
		return;
	}
	/* 参考模型按记录顺序归还（顺序不影响终态）*/
	for (j = 0; j < k; j++)
		(void)ref_block_free(&g_rsb, bases[j], lens[j]);
	for (j = 0; j < k; j++)
		led_set(&g_lb, bases[j], lens[j], 0);
	rfs_run_list_fini(&rl);
}

static void do_inode(u64 r)
{
	u64 co = 0xDEADBEEFCAFEBABEULL;
	u64 ro = 0xDEADBEEFCAFEBABEULL;
	u32 cpu = cur_cpu();
	int cr, rr;

	(void)r;
	g_stat_test++;
	cr = rfs_allocator_inode_alloc(&g_a, &co);
	rr = ref_inode_alloc(&g_rsi, cpu, &ro);
	if (g_div)
		return;
	if (cr != rr || (rr == 0 && co != ro)) {
		FAILF("DIFF seed=%llu op=%ld inode_alloc: rc %d/%d out %llx/%llx",
		      ULL(g_curseed), g_curop, cr, rr, ULL(co), ULL(ro));
		g_div = 1;
		return;
	}
	if (rr == 0) {
		if (led_get(&g_li, co) != 0) {
			FAILF("DIFF seed=%llu op=%ld inode_alloc duplicate %llu",
			      ULL(g_curseed), g_curop, ULL(co));
			g_div = 1;
			return;
		}
		led_set(&g_li, co, 1, 1);
		if (g_ioutn < MAXOUT)
			g_iout[g_ioutn++] = co;
	}
}

static void do_inode_free(u64 r)
{
	int i;
	u64 in;

	if (g_ioutn == 0)
		return;
	i = ((r % 100) < 80) ? (g_ioutn - 1) : (int)(r % (u64)g_ioutn);
	in = g_iout[i];
	g_iout[i] = g_iout[g_ioutn - 1];
	g_ioutn--;
	if (led_get(&g_li, in) != 1)
		return;
	rfs_allocator_inode_free(&g_a, in);
	ref_inode_free(&g_rsi, in);
	led_set(&g_li, in, 1, 0);
}

/* ══════════════════════════ 差分主循环 ══════════════════════════ */
static void stress_seed(u64 seed, u64 blk_nbits, u64 ino_nbits, u32 cpus, long ops)
{
	struct raw rb, ri;
	struct rfs_slices_cfg cfg;
	u64 msb = RFS_IV_SLICE_MIN_BITS;
	long op;

	/* v2.0：inode 数据面容量恒为 32 的倍数（1 片 = 32 inode）→ 靶场向下对齐，
	 * 使 ledger / 参考模型 / 数据面容量三者一致。*/
	ino_nbits = (ino_nbits / RFS_IDP_INODES_PER_PIECE) * RFS_IDP_INODES_PER_PIECE;

	g_curseed = seed;
	g_rng = seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;
	raw_init(&rb, blk_nbits);
	raw_init(&ri, ino_nbits);
	raw_random(&rb, seed ^ 0xABC);
	raw_random(&ri, seed ^ 0xDEF);
	raw_set(&ri, 0, 1);		/* inode 0 = root 预占 */

	memset(&cfg, 0, sizeof(cfg));
	cfg.block_bits_raw = rb.mem;
	cfg.block_bits_count = blk_nbits;
	cfg_set_inode_plane(&cfg, &ri);
	cfg.logical_processor_count = cpus;
	cfg.alloc = g_mem;
	if (rfs_allocator_init(&g_a, &cfg) != 0) {
		FAILF("seed %llu: init failed", ULL(seed));
		raw_free(&rb);
		raw_free(&ri);
		return;
	}

	/* 参考模型独立推导 per，并校验路由几何一致 */
	{
		u64 per_b = ((blk_nbits + rfs_allocator_slice_count(&g_a) - 1) /
			     rfs_allocator_slice_count(&g_a) + msb - 1) / msb * msb;

		CHECK(rfs_allocator_block_per_slice_bits(&g_a) == per_b,
		      "geometry: block per_slice_bits");
		CHECK(rfs_allocator_inode_capacity(&g_a) == ino_nbits,
		      "geometry: inode capacity == ino_nbits");
		refside_init(&g_rsb, rfs_allocator_block_per_slice_bits(&g_a),
			     (int)rfs_allocator_slice_count(&g_a), blk_nbits, rb.mem,
			     (int)cpus);
		/* inode 面 = 单一全局数据面：参考模型用「单段 per=nbits」承载
		 * （rr_alloc_hard(align=0,n=1) = best-fit 最小 run 头，与数据面同语义）。*/
		refside_init(&g_rsi, ino_nbits, 1, ino_nbits, ri.mem, (int)cpus);
	}

	led_init(&g_lb, rb.mem, blk_nbits);
	led_init(&g_li, ri.mem, ino_nbits);
	raw_free(&rb);
	raw_free(&ri);
	g_outn = 0;
	g_ioutn = 0;
	reconcile("seed-init");

	for (op = 0; op < ops && !g_div; op++) {
		u64 r;

		g_curop = op;
		if (op > 0 && (op % 4096) == 0)
			reconcile("periodic");
		if (g_div)
			break;
		r = rnd_next();
		switch (r % 100) {
		case 0 ... 21:
			do_hard(r);
			break;
		case 22 ... 39:
			do_soft(r);
			break;
		case 40 ... 49:
			do_occupy(r);
			break;
		case 50 ... 76:
			do_free(r);
			break;
		case 77 ... 83:
			do_intervals_free(r);
			break;
		case 84 ... 92:
			do_inode(r);
			break;
		default:
			do_inode_free(r);
			break;
		}
	}

	if (!g_div) {
		reconcile("seed-final");
		/* 归还全部未归还区间，账应回到「全占用位都被释放」的中性态 */
			while (g_outn > 0) {
			u64 base = g_out[g_outn - 1].base;
			u64 len = g_out[g_outn - 1].len;

			rfs_allocator_block_free(&g_a, base, len);
			ref_block_free(&g_rsb, base, len);
			led_set(&g_lb, base, len, 0);
			g_outn--;
		}
		while (g_ioutn > 0) {
			u64 in = g_iout[--g_ioutn];

			rfs_allocator_inode_free(&g_a, in);
			ref_inode_free(&g_rsi, in);
			led_set(&g_li, in, 1, 0);
		}
		reconcile("seed-drained");
	}

	rfs_allocator_fini(&g_a);
	if (g_pool.live != 0 || g_pool.allocs != g_pool.frees)
		FAILF("seed %llu: pool imbalance live=%llu a=%llu f=%llu",
		      ULL(seed), ULL(g_pool.live), ULL(g_pool.allocs), ULL(g_pool.frees));
	refside_fini(&g_rsb);
	refside_fini(&g_rsi);
	led_fini(&g_lb);
	led_fini(&g_li);
}

/* ══════════════════════════ 定向对抗用例 ══════════════════════════ */

static int router_setup(struct rfs_allocator *a, struct raw *b, struct raw *i, u32 cpus)
{
	struct rfs_slices_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.block_bits_raw = b->mem;
	cfg.block_bits_count = b->nbits;
	cfg_set_inode_plane(&cfg, i);
	cfg.logical_processor_count = cpus;
	cfg.alloc = g_mem;
	return rfs_allocator_init(a, &cfg);
}

/* 1. 几何 / 参数 / 自清理 */
static void test_geometry(void)
{
	u64 msb = RFS_IV_SLICE_MIN_BITS;
	struct raw b, i;
	struct rfs_allocator a;
	struct rfs_slices_cfg cfg;
	u32 c;

	puts("[1] 几何推导 / preferred / init 失败自清理");

	raw_init(&b, msb * 4);
	raw_init(&i, msb * 4);
	CHECK(router_setup(&a, &b, &i, 4) == 0, "4-slice init");
	CHECK(rfs_allocator_slice_count(&a) == 4, "4-slice count");
	CHECK(rfs_allocator_cpu_slots(&a) == 4, "cpu_slots 4");
	CHECK(rfs_allocator_block_per_slice_bits(&a) == msb, "block per==msb");
	CHECK(rfs_allocator_inode_capacity(&a) == msb * 4, "inode capacity==pieces*32");
	for (c = 0; c < 4; c++)
		CHECK(rfs_allocator_block_preferred_of(&a, c) == (c % 4), "pref init i%n");
	CHECK(rfs_allocator_block_free_total(&a) == 0, "all-occupied free 0");
	CHECK(rfs_allocator_block_test(&a, 0) == true, "blk0 occupied");
	CHECK(rfs_allocator_inode_test(&a, 0) == true, "inode0 root occupied");
	rfs_allocator_fini(&a);
	CHECK(g_pool.live == 0, "fini drained pool");
	raw_free(&b);
	raw_free(&i);

	/* cap 压片数：3msb, cpus 8 → 3 片 */
	raw_init(&b, msb * 3);
	raw_init(&i, msb * 3);
	CHECK(router_setup(&a, &b, &i, 8) == 0, "cap init");
	CHECK(rfs_allocator_slice_count(&a) == 3, "cap=3 -> 3 slices");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);

	/* 单片几何 */
	raw_init(&b, msb);
	raw_init(&i, msb);
	CHECK(router_setup(&a, &b, &i, 8) == 0, "single-slice init");
	CHECK(rfs_allocator_slice_count(&a) == 1, "single slice");
	CHECK(rfs_allocator_block_per_slice_bits(&a) == msb, "single per==msb");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);

	/* 末片短：block 2msb+100 / inode 2msb+777，cpus 2 */
	raw_init(&b, msb * 2 + 100);
	raw_init(&i, msb * 2 + 777);
	CHECK(router_setup(&a, &b, &i, 2) == 0, "short-last init");
	CHECK(rfs_allocator_slice_count(&a) == 2, "short-last 2 slices");
	CHECK(rfs_allocator_block_per_slice_bits(&a) == msb * 2, "block per 2*msb");
	/* 末片实际覆盖：block [2m,2m+100)，inode 面与分片无关 */
	{
		struct rfs_run got[4];
		struct rfs_run_dlist dl;
		struct rfs_run_node *nd;
		int n = 0;

		raw_clear(&b, msb * 2 + 5, 10);
		rfs_allocator_fini(&a);
		CHECK(router_setup(&a, &b, &i, 2) == 0, "short-last re-init");
		rfs_run_dlist_init(&dl, g_ra);
		CHECK(rfs_allocator_dump_slice(&a, 1, &dl) == 0, "dump short last");
		for (nd = dl.head; nd; nd = nd->next) {
			if (n < 4)
				got[n] = nd->run;
			n++;
		}
		CHECK(n == 1 && got[0].base == msb * 2 + 5 && got[0].len == 10,
		      "short-last slice1 dump correct");
		rfs_run_dlist_fini(&dl);
	}
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);

	/* init 失败自清理：inode 数据面缺失（pieces=0）→ -EINVAL */
	raw_init(&b, msb * 2);
	raw_init(&i, msb * 2);
	{
		struct rfs_allocator z;

		memset(&cfg, 0, sizeof(cfg));
		cfg.block_bits_raw = b.mem;
		cfg.block_bits_count = b.nbits;
		cfg.logical_processor_count = 4;
		cfg.alloc = g_mem;
		/* 不填 cfg.inode（pieces==0）→ 拒绝 */
		CHECK(rfs_allocator_init(&z, &cfg) == -EINVAL, "no inode plane -> -EINVAL");
		CHECK(rfs_allocator_slice_count(&z) == 0, "failed init -> empty");
		rfs_allocator_fini(&z);
		CHECK(g_pool.live == 0 && g_pool.allocs == g_pool.frees,
		      "failed init self-cleaned");
	}
	/* 参数缺失 */
	{
		struct rfs_allocator z;
		struct rfs_slices_cfg cz;

		memset(&cz, 0, sizeof(cz));
		CHECK(rfs_allocator_init(&z, &cz) == -EINVAL, "null cfg -> -EINVAL");
		CHECK(rfs_allocator_init(NULL, &cz) == -EINVAL, "null self -> -EINVAL");
		rfs_allocator_fini(&z);
	}
	raw_free(&b);
	raw_free(&i);

	/* reference 上界：block_test OOB false */
	raw_init(&b, msb);
	raw_init(&i, msb);
	CHECK(router_setup(&a, &b, &i, 1) == 0, "oob init");
	CHECK(rfs_allocator_block_test(&a, msb + 5) == false, "block_test OOB false");
	CHECK(rfs_allocator_inode_test(&a, msb + 5) == false, "inode_test OOB false");
	CHECK(rfs_allocator_block_preferred_of(&a, 99) == 0, "preferred_of OOB 0");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* 2. hard 单片原子不跨片 */
static void test_hard_atomic(void)
{
	u64 msb = RFS_IV_SLICE_MIN_BITS;
	struct raw b, i;
	struct rfs_allocator a;
	u64 r;

	puts("[2] block_alloc_hard 单片原子 / 全败哨兵");

	raw_init(&b, msb * 4);
	raw_init(&i, msb * 4);
	raw_clear(&b, msb - 4, 4);	/* slice0 尾 */
	raw_clear(&b, msb, 4);		/* slice1 首（物理连续 8，但跨片）*/
	CHECK(router_setup(&a, &b, &i, 4) == 0, "init");
	CHECK(rfs_allocator_block_alloc_hard(&a, 8, 0) == RFS_IV_NO_HIT,
	      "hard 8 across boundary must fail");
	r = rfs_allocator_block_alloc_hard(&a, 4, 0);
	CHECK(r == msb - 4 || r == msb, "hard 4 single-slice ok");
	CHECK(rfs_allocator_block_free_total(&a) == 4, "one 4 consumed");
	CHECK(rfs_allocator_block_alloc_hard(&a, 8, 0) == RFS_IV_NO_HIT, "full -> NO_HIT");
	CHECK(rfs_allocator_block_alloc_hard(&a, 0, 0) == RFS_IV_NO_HIT, "zero -> NO_HIT");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* 3. soft 跨片凑满 + 不够回滚 */
static void test_soft_cross(void)
{
	u64 msb = RFS_IV_SLICE_MIN_BITS;
	struct raw b, i;
	struct rfs_allocator a;
	struct rfs_run_list rl;

	puts("[3] block_alloc_soft 跨片多段 + all-or-nothing 回滚");

	raw_init(&b, msb * 4);
	raw_init(&i, msb * 4);
	raw_clear(&b, msb - 4, 4);
	raw_clear(&b, msb, 4);
	raw_clear(&b, msb * 2, 4);
	raw_clear(&b, msb * 3, 8);
	CHECK(router_setup(&a, &b, &i, 4) == 0, "init");
	CHECK(rfs_allocator_block_free_total(&a) == 20, "free 20");

	rfs_run_list_init(&rl, g_ra);
	CHECK(rfs_allocator_block_alloc_soft(&a, 12, 0, &rl) == 0, "soft 12 ok");
	CHECK(rl_sum(&rl) == 12, "soft delivered 12");
	CHECK(rl_count(&rl) >= 2, "soft multi-fragment");
	rfs_run_list_fini(&rl);
	CHECK(rfs_allocator_block_free_total(&a) == 8, "free 20-12==8");

	/* 只剩 8 要 12 → 回滚 */
	rfs_run_list_init(&rl, g_ra);
	CHECK(rfs_allocator_block_alloc_soft(&a, 12, 0, &rl) == -ENOSPC, "soft short -> -ENOSPC");
	CHECK(rl.head == NULL, "rollback emptied runs");
	CHECK(rfs_allocator_block_free_total(&a) == 8, "rollback restored ledger");
	rfs_run_list_fini(&rl);

	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* 4. occupy 跨片续圈 / 顶片尾记账 / 锚点占用 / 末片短 */
static void test_occupy_cross(void)
{
	u64 msb = RFS_IV_SLICE_MIN_BITS;
	struct raw b, i;
	struct rfs_allocator a;
	u64 rest, k;

	puts("[4] block_occupy_soft 跨片续圈 / rest 记账 / 末片短");

	raw_init(&b, msb * 4);
	raw_init(&i, msb * 4);
	raw_clear(&b, msb - 8, 16);	/* 跨界连续 [m-8, m+8) */
	CHECK(router_setup(&a, &b, &i, 4) == 0, "init");
	rest = 999;
	CHECK(rfs_allocator_block_occupy_soft(&a, msb - 8, 16, &rest) == 0, "occupy cross ok");
	CHECK(rest == 0, "occupy fully satisfied");
	for (k = 0; k < 16; k++)
		CHECK(rfs_allocator_block_test(&a, msb - 8 + k) == true, "cross bit occupied");
	CHECK(rfs_allocator_block_free(&a, msb - 8, 16) == 0, "reverse free cross ok");
	CHECK(rfs_allocator_block_free_total(&a) == 16, "free restored");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);

	/* 顶片尾：8 连续止于片尾，rest 记账 8 */
	raw_init(&b, msb * 4);
	raw_init(&i, msb * 4);
	raw_clear(&b, msb - 8, 8);
	CHECK(router_setup(&a, &b, &i, 4) == 0, "init");
	rest = 999;
	CHECK(rfs_allocator_block_occupy_soft(&a, msb - 8, 16, &rest) == 0, "partial-prefix ok");
	CHECK(rest == 8, "rest==8 stopped at slice end");
	CHECK(rfs_allocator_block_test(&a, msb) == true, "next slice start untouched");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);

	/* 锚点即占用 → -ENOSPC, rest==require */
	raw_init(&b, msb * 4);
	raw_init(&i, msb * 4);
	CHECK(router_setup(&a, &b, &i, 4) == 0, "init");
	rest = 999;
	CHECK(rfs_allocator_block_occupy_soft(&a, 100, 4, &rest) == -ENOSPC,
	      "anchor occupied -ENOSPC");
	CHECK(rest == 4, "rest==require");
	CHECK(rfs_allocator_block_occupy_soft(&a, 100, 0, &rest) == -EINVAL, "occupy zero count -EINVAL");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);

	/* 末片短：真实片尾夹断，rest 如实 */
	raw_init(&b, msb * 2 + 10);
	raw_init(&i, msb * 2 + 10);
	raw_clear(&b, msb * 2 - 4, 20);	/* 跨界到末片 [2m-4, 2m+10) 仅 10 真实可用 */
	CHECK(router_setup(&a, &b, &i, 2) == 0, "init");
	rest = 999;
	CHECK(rfs_allocator_block_occupy_soft(&a, msb * 2 - 4, 20, &rest) == 0,
	      "short-last occupy ok");
	CHECK(rest == 6, "short-last clamps at real end (14 claimed, rest 6)");
	CHECK(rfs_allocator_block_test(&a, msb * 2 + 9) == true, "last real bit occupied");
	CHECK(rfs_allocator_block_free_total(&a) == 0, "all real free consumed");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* 5. free / intervals_free 跨片链式 */
static void test_free_chains(void)
{
	u64 msb = RFS_IV_SLICE_MIN_BITS;
	struct raw b, i;
	struct rfs_allocator a;
	struct rfs_run_list rl;
	u64 rest;

	puts("[5] block_free 跨片 / block_intervals_free 链式");

	raw_init(&b, msb * 4);
	raw_init(&i, msb * 4);
	raw_clear(&b, msb - 8, 16);
	CHECK(router_setup(&a, &b, &i, 4) == 0, "init");
	rest = 999;
	CHECK(rfs_allocator_block_occupy_soft(&a, msb - 8, 12, &rest) == 0, "occupy 12");
	CHECK(rfs_allocator_block_free(&a, msb - 8, 12) == 0, "free cross 12");
	CHECK(rfs_allocator_block_free_total(&a) == 16, "restored");

	rfs_run_list_init(&rl, g_ra);
	CHECK(rfs_allocator_block_alloc_soft(&a, 8, 0, &rl) == 0, "soft 8");
	CHECK(rfs_allocator_block_free_total(&a) == 8, "free 8");
	CHECK(rfs_allocator_block_intervals_free(&a, &rl) == 0, "intervals_free ok");
	CHECK(rl.head == NULL, "chain drained");
	CHECK(rfs_allocator_block_free_total(&a) == 16, "restored via chain");
	CHECK(rfs_allocator_block_intervals_free(&a, NULL) == -EINVAL, "null chain -EINVAL");
	rfs_run_list_fini(&rl);

	CHECK(rfs_allocator_block_free(&a, 0, 0) == 0, "free count 0 no-op");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* 6. dump_slice 跨界 run 切分 */
static void test_dump_split(void)
{
	u64 msb = RFS_IV_SLICE_MIN_BITS;
	struct raw b, i;
	struct rfs_allocator a;
	struct rfs_run got[8];
	struct rfs_run_dlist dl;
	struct rfs_run_node *nd;
	int n;

	puts("[6] dump_slice 跨界 run 按片切分");

	raw_init(&b, msb * 4);
	raw_init(&i, msb * 4);
	raw_clear(&b, msb - 8, 16);	/* 物理连续 16，跨 slice0/1 → 应切为两段 8+8 */
	CHECK(router_setup(&a, &b, &i, 4) == 0, "init");

	rfs_run_dlist_init(&dl, g_ra);
	CHECK(rfs_allocator_dump_slice(&a, 0, &dl) == 0, "dump slice0");
	n = 0;
	for (nd = dl.head; nd; nd = nd->next) if (n < 8) got[n++] = nd->run;
	CHECK(n == 1 && got[0].base == msb - 8 && got[0].len == 8, "slice0 half");
	rfs_run_dlist_fini(&dl);

	rfs_run_dlist_init(&dl, g_ra);
	CHECK(rfs_allocator_dump_slice(&a, 1, &dl) == 0, "dump slice1");
	n = 0;
	for (nd = dl.head; nd; nd = nd->next) if (n < 8) got[n++] = nd->run;
	CHECK(n == 1 && got[0].base == msb && got[0].len == 8, "slice1 half");
	rfs_run_dlist_fini(&dl);

	CHECK(rfs_allocator_dump_slice(&a, 4, NULL) == -EINVAL, "dump OOB -EINVAL");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* 7. inode 组跨片 + root */
static void test_inode_group(void)
{
	u64 msb = RFS_IV_SLICE_MIN_BITS;
	struct raw b, i;
	struct rfs_allocator a;
	u64 n1, n2, lo, hi;

	puts("[7] inode 跨片 alloc/free/test + root 占用");

	raw_init(&b, msb * 4);
	raw_init(&i, msb * 4);
	raw_clear(&i, msb + 2, 1);
	raw_clear(&i, msb * 3 + 1, 1);
	CHECK(router_setup(&a, &b, &i, 4) == 0, "init");
	CHECK(rfs_allocator_inode_free_total(&a) == 2, "inode free 2");
	CHECK(rfs_allocator_inode_test(&a, 0) == true, "root occupied");

	n1 = n2 = 0;
	CHECK(rfs_allocator_inode_alloc(&a, &n1) == 0, "inode #1");
	CHECK(rfs_allocator_inode_alloc(&a, &n2) == 0, "inode #2");
	lo = n1 < n2 ? n1 : n2;
	hi = n1 < n2 ? n2 : n1;
	CHECK(lo == msb + 2 && hi == msb * 3 + 1, "inode cross-slice set");
	CHECK(rfs_allocator_inode_test(&a, msb + 2) == true, "occupied 1");
	CHECK(rfs_allocator_inode_test(&a, msb * 3 + 1) == true, "occupied 2");
	CHECK(rfs_allocator_inode_free_total(&a) == 0, "drained");
	rfs_allocator_inode_free(&a, msb + 2);
	CHECK(rfs_allocator_inode_test(&a, msb + 2) == false, "freed 1");
	CHECK(rfs_allocator_inode_free_total(&a) == 1, "restored 1");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);

	/* 全占 → inode -ENOSPC */
	raw_init(&b, msb * 2);
	raw_init(&i, msb * 2);
	CHECK(router_setup(&a, &b, &i, 2) == 0, "init");
	n1 = 0;
	CHECK(rfs_allocator_inode_alloc(&a, &n1) == -ENOSPC, "inode full -ENOSPC");
	rfs_allocator_fini(&a);
	raw_free(&b);
	raw_free(&i);
}

/* 8. 分配账（fini 后 allocs==frees, live==0） */
static void test_accounting(void)
{
	u64 msb = RFS_IV_SLICE_MIN_BITS;
	struct raw b, i;
	struct rfs_allocator a;
	struct rfs_run_list rl;

	puts("[8] 分配账 allocs==frees && live==0");

	raw_init(&b, msb * 4);
	raw_init(&i, msb * 4);
	raw_clear(&b, 0, 1000);
	raw_clear(&i, 10, 100);
	CHECK(router_setup(&a, &b, &i, 4) == 0, "init");
	rfs_run_list_init(&rl, g_ra);
	CHECK(rfs_allocator_block_alloc_soft(&a, 500, 0, &rl) == 0, "soft 500");
	rfs_run_list_fini(&rl);
	rfs_allocator_block_free(&a, 0, 500);
	rfs_allocator_fini(&a);
	CHECK(g_pool.live == 0, "live==0 after fini");
	CHECK(g_pool.allocs == g_pool.frees, "allocs==frees");
	raw_free(&b);
	raw_free(&i);
}

/* ══════════════════════════ main ══════════════════════════ */
static u32 pin_first_cpu(void)
{
	cpu_set_t set;
	int cpu = -1, i;

	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof(set), &set) != 0)
		return (u32)-1;
	for (i = 0; i < CPU_SETSIZE; i++)
		if (CPU_ISSET(i, &set)) {
			cpu = i;
			break;
		}
	if (cpu < 0)
		return (u32)-1;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	(void)sched_setaffinity(0, sizeof(set), &set);
	return (u32)sched_getcpu();
}

int main(int argc, char **argv)
{
	long ops = 60000;
	int seeds = 4;
	u64 msb = RFS_IV_SLICE_MIN_BITS;
	long total = 0;
	int i;

	if (argc > 1)
		ops = atol(argv[1]);
	if (argc > 2)
		seeds = atoi(argv[2]);
	if (seeds < 0)
		seeds = 0;

	g_cpu = pin_first_cpu();
	if (g_cpu == (u32)-1) {
		printf("  WARN: could not pin CPU; single-thread determinism not guaranteed\n");
		g_cpu = (u32)RFS_CPU_ID();
	}
	printf("=== ravenfs_allocator_router adversarial stress (pinned cpu %u) ===\n",
	       g_cpu);

	g_out = (struct outiv *)malloc((size_t)MAXOUT * sizeof(g_out[0]));
	g_iout = (u64 *)malloc((size_t)MAXOUT * sizeof(g_iout[0]));
	if (!g_out || !g_iout) {
		printf("FATAL out alloc\n");
		return 2;
	}

	test_geometry();
	test_hard_atomic();
	test_soft_cross();
	test_occupy_cross();
	test_free_chains();
	test_dump_split();
	test_inode_group();
	test_accounting();

	if (ops > 0 && seeds > 0) {
		/* 多种几何：正常/末片短/单片/6 片；cpus 与 nbits 组合出不同 per。*/
		struct geom {
			u64 blk, ino;
			u32 cpus;
		} geoms[4] = {
			{ msb * 4, msb * 4, 8 },
			{ msb * 3 + 12345, msb * 2 + 777, 8 },
			{ msb, msb, 4 },
			{ msb * 6, msb * 6, 8 },
		};

		for (i = 0; i < seeds && !g_div; i++) {
			struct geom gm = geoms[i % 4];

			printf("[S] diff seed %d/%d ops=%ld blk=%llu ino=%llu cpus=%u\n",
			       i + 1, seeds, ops, ULL(gm.blk), ULL(gm.ino), gm.cpus);
			fflush(stdout);
			stress_seed((u64)(0xC0FFEEu + (u32)i * 1013904223u),
				    gm.blk, gm.ino, gm.cpus, ops);
			total += ops;
		}
		printf("--- differential ops total = %ld\n", total);
	}

	printf("--- ops: hard=%ld(fail %ld) soft=%ld(fail %ld) occupy=%ld(partial %ld fail %ld)\n",
	       g_stat_hard, g_stat_hard_fail, g_stat_soft, g_stat_soft_fail,
	       g_stat_occ, g_stat_occ_partial, g_stat_occ_fail);
	printf("--- ops: free=%ld ifree=%ld inode=%ld\n",
	       g_stat_free, g_stat_ifree, g_stat_test);
	printf("--- pool: allocs=%llu frees=%llu live=%llu\n",
	       ULL(g_pool.allocs), ULL(g_pool.frees), ULL(g_pool.live));
	CHECK(g_pool.live == 0 && g_pool.allocs == g_pool.frees, "final: pool balanced");

	free(g_out);
	free(g_iout);
	printf("=== PASS %d  FAIL %d%s ===\n", g_pass, g_fail,
	       g_div ? "  (DIFFERENTIAL DIVERGENCE)" : "");
	return g_fail ? 1 : 0;
}

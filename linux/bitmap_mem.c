// SPDX-License-Identifier: GPL-2.0
/*
 * bitmap_mem.c —— RavenFS 双位图分片分配器 · **MEM 后端**（= god v1）
 *
 * 对应 god `juvenile_god/bitmap_slices.cpp`：`bitmap_slice_t`（片算法）+
 * `bitmap_slices_mgr_t`（与 v1 相关的部分）。位图**全量常驻内存**、零 I/O；
 * words 内存由拥有者提供（类不取所有权）。本文件只实现路由层 ops 接口；
 * 迭代/几何/preferred/跨片续圈都在 bitmap_core.c（禁止复制路由）。
 *
 * 坐标：**MEM 片即全局视角**（片自带 base_idx）⇒ 本文件 ops 直接吃/吐全局位号
 * （路由层 bm_to_local/to_global 对 MEM 恒等）。与 god v1 逐条对齐：
 *   · 搜索式分配（hard/soft）从 hint 起扫，段尾绕回 [片首, hint) 再扫一次；
 *   · hint **只进不退**（全局位号）；free_count 增量维护、重复释放不虚高；
 *   · soft_occupy 锚定贪心前缀、片内夹断（跨界续圈在路由层）；hint 只进不退；
 *   · alloc_soft 取「首个空闲段的对齐候选」，got = min(段长, count)。
 *
 * ── 与 god 的**刻意差异**（文档化）──
 *   1. 锁：god v1 片方法**不加锁**（临界区由 mgr 路由层用 try_lock_write 编排，
 *      忙则换片）；ko 口径把锁下移到**片内阻塞自旋**（`rfs_spin_t`）——与
 *      linux/bitmap.c ko 现状一致（2026-09-28 用户裁）。故本文件每个 ops 自持
 *      片锁（作用域守卫），路由层不再编排锁。
 *   2. 返回码：RC_* → 0 / 负 -errno；NO_HIT → RFS_BM_NO_HIT（仅路由层用哨兵）。
 *   3. inode 组**不做 `片0 hint=1` 的 root 保留**（god v2.1/ko 2026-09-26 裁：
 *      root inode = 16 由 mkfs 预占，无保留区）——与 god v1 的 `set_hint(1)` 差异，
 *      以冻结头 ravenfs_bitmap_core.h「inode 组…类不做保留区」为准。
 *   （坐标系差异：god slice 内部用片内偏移，本文件用全局位号——同一块内存、
 *    路由层与 ko 均以全局位号为准。）
 */
#include "ravenfs_bitmap_backend.h"

/* ═══════════════════════════ 位运算轮子（匿名语义，static）═══════════════════════════ */

/* 小端字内位原语 */
static inline bool mem_bit_get(const u64 *w, u64 i)
{
	return (w[i >> 6] >> (i & 63)) & 1ULL;
}

static inline void mem_bit_set(u64 *w, u64 i)
{
	w[i >> 6] |= 1ULL << (i & 63);
}

static inline void mem_bit_clear(u64 *w, u64 i)
{
	w[i >> 6] &= ~(1ULL << (i & 63));
}

/*
 * 从 from 起找下一个空闲段 [*seg_s, *seg_e)（字粒度，整字占用直接跳过）。
 * max_extend > 0：段长够 max_extend 即截断（防稀疏大位图 1-bit 分配 O(n²) 热点）。
 * 无 → false。移植自 linux/bitmap.c next_free_seg（v1 同构）。
 */
static bool mem_next_free_seg(const u64 *w, u64 nbits, u64 from, u64 max_extend,
			      u64 *seg_s, u64 *seg_e)
{
	u64 i = from;

	while (i < nbits) {
		u64 word = ~w[i >> 6];
		u64 bit, e;

		word &= ~0ULL << (i & 63);
		if (!word) {
			i = ((i >> 6) + 1) << 6;
			continue;
		}
		bit = (i & ~63ULL) + (u64)__builtin_ctzll(word);
		if (bit >= nbits)
			return false;
		e = bit;
		while (e < nbits) {
			if (mem_bit_get(w, e))
				break;
			e++;
			if (max_extend && (e - bit) >= max_extend)
				break;
		}
		*seg_s = bit;
		*seg_e = e;
		return true;
	}
	return false;
}

/* 找 [cand, cand+count) 全空闲且 cand % align == 0；无 → RFS_BM_NO_HIT */
static u64 mem_find_free_run(const u64 *w, u64 nbits, u64 from, u64 count, u64 align)
{
	u64 probe = from, seg_s, seg_e;

	if (count == 0 || count > nbits || align == 0)
		return RFS_BM_NO_HIT;
	while (mem_next_free_seg(w, nbits, probe, count + align, &seg_s, &seg_e)) {
		u64 cand = (seg_s + align - 1) & ~(align - 1);

		if (cand < seg_e && cand + count <= seg_e)
			return cand;
		probe = seg_e;
	}
	return RFS_BM_NO_HIT;
}

/* 扫 [base, base+bits) 空闲位数（enable / rescan 用） */
static u64 mem_scan_free(const struct rfs_bm_group *g, u64 base, u64 bits)
{
	u64 j, n = 0;

	for (j = 0; j < bits; j++)
		if (!mem_bit_get(g->words, base + j))
			n++;
	return n;
}

/* ═══════════════════════════ 生命周期 ops ═══════════════════════════ */

static int mem_group_setup(struct rfs_bm_group *g, const struct rfs_bm_cfg *cfg)
{
	u32 i;

	(void)cfg;			/* MEM 的 words/nbits 已由 core 填入组 */
	if (!g->words)
		return -EINVAL;
	g->slices = RFS_ZALLOC(sizeof(*g->slices) * g->n_slices);
	if (!g->slices)
		return -ENOMEM;
	for (i = 0; i < g->n_slices; i++) {
		struct rfs_bm_slice *s = &g->slices[i];
		u64 base = (u64)i * g->per_bits;

		s->base_idx = base;	/* 片首全局位号 */
		s->bits = (base >= g->nbits) ? 0 : min(g->per_bits, g->nbits - base);
		s->free_count = mem_scan_free(g, base, s->bits);
		s->hint = base;		/* 搜索游标：全局位号，只进不退 */
		RFS_SPIN_INIT(&s->lock);
	}
	return 0;
}

static void mem_group_fini(struct rfs_bm_group *g)
{
	RFS_FREE(g->slices);		/* RFS_FREE(NULL) 安全 → 幂等 */
	g->slices = NULL;
}

/* 拥有者装载/改写位图内存后重算各片 free_count（不取锁：装配期 / 维护期调用） */
static void mem_group_rescan_free(struct rfs_bm_group *g)
{
	u32 i;

	if (!g->slices)
		return;
	for (i = 0; i < g->n_slices; i++) {
		struct rfs_bm_slice *s = &g->slices[i];

		s->free_count = mem_scan_free(g, s->base_idx, s->bits);
	}
}

/* ═══════════════════════════ 分配 ops ═══════════════════════════ */

/*
 * 硬分配：片内 [lo,hi) 找 count 连续空闲位、起始 2^align_log2 对齐；
 * hint 起扫 → 段尾绕回 [lo, hint) 再扫一次；不部分分配。
 * 命中 → 0 且 *out = 全局起始位；free_count < count 或找不到 → -ENOSPC。
 */
static int mem_alloc_hard(struct rfs_bm_group *g, u32 idx, u64 count,
			  u8 align_log2, u64 *out)
{
	struct rfs_bm_slice *s = &g->slices[idx];
	u64 lo = s->base_idx, hi = lo + s->bits;
	u64 align, from, hit;

	if (!g->words)
		return -EINVAL;
	if (count == 0)
		return -EINVAL;
	if (count > s->bits)
		return -ENOSPC;
	align = 1ULL << (align_log2 > 16 ? 16 : align_log2);	/* 防御截断（god v1） */

	RFS_SPIN_GUARD(lk, &s->lock);
	if (s->free_count < count)
		return -ENOSPC;
	from = s->hint < lo ? lo : s->hint;
	if (from > hi)
		from = hi;
	hit = mem_find_free_run(g->words, hi, from, count, align);
	if (hit == RFS_BM_NO_HIT)
		hit = mem_find_free_run(g->words, from, lo, count, align);	/* 绕回 */
	if (hit == RFS_BM_NO_HIT)
		return -ENOSPC;
	{
		u64 j;

		for (j = 0; j < count; j++)
			mem_bit_set(g->words, hit + j);
	}
	s->free_count -= count;
	s->hint = hit + count;
	*out = hit;
	return 0;
}

/*
 * 软分配：首个空闲段取对齐候选，实际 got = min(段长, count)；缺口 *left 回传。
 * hint 起段 → 绕回 [lo, hint)；无空闲 → -ENOSPC（*left 预置 = count）。
 */
static int mem_alloc_soft(struct rfs_bm_group *g, u32 idx, u64 count,
			  u8 align_log2, u64 *out, u64 *left)
{
	struct rfs_bm_slice *s = &g->slices[idx];
	u64 lo = s->base_idx, hi = lo + s->bits;
	u64 align, cap, from, seg_s, seg_e, hit = RFS_BM_NO_HIT, got, j;

	*left = count;
	if (!g->words)
		return -EINVAL;
	if (count == 0)
		return -EINVAL;
	align = 1ULL << (align_log2 > 16 ? 16 : align_log2);

	RFS_SPIN_GUARD(lk, &s->lock);
	if (count > s->bits || s->free_count == 0)
		return -ENOSPC;
	cap = count > align ? count : align;
	from = s->hint < lo ? lo : s->hint;
	if (from > hi)
		from = hi;

	seg_s = 0;
	seg_e = 0;
	if (mem_next_free_seg(g->words, hi, from, cap, &seg_s, &seg_e)) {
		u64 cand = (seg_s + align - 1) & ~(align - 1);

		if (cand < seg_e)
			hit = cand;
	}
	if (hit == RFS_BM_NO_HIT &&
	    mem_next_free_seg(g->words, from, lo, cap, &seg_s, &seg_e)) {
		u64 cand = (seg_s + align - 1) & ~(align - 1);

		if (cand < seg_e)
			hit = cand;
	}
	if (hit == RFS_BM_NO_HIT)
		return -ENOSPC;

	got = seg_e - hit;
	if (got > count)
		got = count;
	for (j = 0; j < got; j++)
		mem_bit_set(g->words, hit + j);
	s->free_count -= got;
	s->hint = hit + got;
	*out = hit;
	*left = count - got;
	return 0;
}

/*
 * 锚定占领（定点对·对偶 free_interval）：从 base 起贪心认领连续空闲前缀，
 * 撞占用或到额度即停；**永不越片**（cap 片内夹断，跨界续圈在路由层）。
 * 口径：*rest 入 = 已由其它途径满足量 → real = len − *rest；出 = len − claimed。
 * 返回：认领 ≥1 → 0；锚点即占用 / 片内无空间 → -ENOSPC（*rest 出 = len）；
 * base 出片 → -EINVAL（*rest 不动）。hint 只进不退。
 */
static int mem_occupy_soft(struct rfs_bm_group *g, u32 idx, u64 base, u64 len, u64 *rest)
{
	struct rfs_bm_slice *s = &g->slices[idx];
	u64 lo = s->base_idx, hi = lo + s->bits;
	u64 real, cap, k = 0, j;

	if (!g->words || !rest)
		return -EINVAL;
	if (*rest > len)
		return -EINVAL;
	if (len == 0 || *rest == len)
		return 0;			/* real == 0：无事可做（rest 不动） */

	RFS_SPIN_GUARD(lk, &s->lock);
	if (base < lo || base >= hi)
		return -EINVAL;
	real = len - *rest;
	cap = min(real, hi - base);
	/* 贪心前缀（字粒度）：与段尾延伸同构、方向相反 */
	while (k < cap) {
		u64 pos = (base + k) & 63;
		u64 free_mask = ~g->words[(base + k) >> 6] >> pos;
		u64 run;

		if (!(free_mask & 1))
			break;			/* 当前位占用 → 前缀断 */
		run = (free_mask == ~0ULL) ? (64 - pos) : (u64)__builtin_ctzll(~free_mask);
		k += run;
		if (k > cap)
			k = cap;
	}
	if (!k) {
		*rest = len;
		return -ENOSPC;
	}
	for (j = 0; j < k; j++)
		mem_bit_set(g->words, base + j);
	s->free_count -= k;
	if (base + k > s->hint)
		s->hint = base + k;		/* 只进不退 */
	*rest = len - k;
	return 0;
}

/* ═══════════════════════════ 释放 / 测试 ops ═══════════════════════════ */

/* 区间释放：清位，free_count 按实际 1→0 位计数（重复释放不虚高）。越界 → -EINVAL */
static int mem_free_interval(struct rfs_bm_group *g, u32 idx, u64 base, u64 len)
{
	struct rfs_bm_slice *s = &g->slices[idx];
	u64 lo = s->base_idx, off, i, cleared = 0;

	if (!g->words)
		return -EINVAL;
	if (base < lo)
		return -EINVAL;
	off = base - lo;
	if (len > s->bits || off > s->bits - len)
		return -EINVAL;
	if (len == 0)
		return 0;

	RFS_SPIN_GUARD(lk, &s->lock);
	for (i = 0; i < len; i++) {
		if (mem_bit_get(g->words, base + i)) {
			mem_bit_clear(g->words, base + i);
			cleared++;
		}
	}
	s->free_count += cleared;
	rfs_bm_stat(&g->stat, RFS_BM_ST_FREE_BLOCKS, cleared);	/* 仅 1→0 实清计 */
	return 0;
}

/* 区间同值测试（片内；跨片切段在路由层）：异位 → -ENOENT；越界 → -EINVAL。
 * 取片锁（ko 口径：多比特读需一致快照）。 */
static int mem_test_interval(struct rfs_bm_group *g, u32 idx, bool val, u64 base, u64 len)
{
	struct rfs_bm_slice *s = &g->slices[idx];
	u64 lo = s->base_idx, off, i;

	if (!g->words)
		return -EINVAL;
	if (base < lo)
		return -EINVAL;
	off = base - lo;
	if (len > s->bits || off > s->bits - len)
		return -EINVAL;
	if (len == 0)
		return 0;

	RFS_SPIN_GUARD(lk, &s->lock);
	for (i = 0; i < len; i++) {
		if (mem_bit_get(g->words, base + i) != val)
			return -ENOENT;
	}
	return 0;
}

/* 单点测试：直读位图内存（无锁；调用方保证并发安全——冻结头注记） */
static bool mem_test_bit(struct rfs_bm_group *g, u32 idx, u64 bit)
{
	struct rfs_bm_slice *s = &g->slices[idx];

	if (!g->words)
		return false;
	if (bit < s->base_idx || bit - s->base_idx >= s->bits)
		return false;
	return mem_bit_get(g->words, bit);
}

static u64 mem_free_count(const struct rfs_bm_group *g, u32 idx)
{
	return g->slices[idx].free_count;
}

/* ═══════════════════════════ 持久化 ops ═══════════════════════════ */

/* 锁内快照「第 idx 片」位图字到 dst（bits/8 字节）；调用方锁外做 I/O。 */
static void mem_snapshot_slice(const struct rfs_bm_group *g, u32 idx, void *dst)
{
	struct rfs_bm_slice *s = (struct rfs_bm_slice *)&g->slices[idx];

	if (!g->words || !s->bits)
		return;
	RFS_SPIN_GUARD(lk, &s->lock);
	memcpy(dst, (const u8 *)g->words + (s->base_idx >> 3), (size_t)(s->bits >> 3));
}

/* ═══════════════════════════ ops 表 ═══════════════════════════ */

const struct rfs_bm_ops rfs_bm_mem_ops = {
	.name			= "mem",
	.group_setup		= mem_group_setup,
	.group_fini		= mem_group_fini,
	.group_rescan_free	= mem_group_rescan_free,
	.alloc_hard		= mem_alloc_hard,
	.alloc_soft		= mem_alloc_soft,
	.occupy_soft		= mem_occupy_soft,
	.free_interval		= mem_free_interval,
	.test_interval		= mem_test_interval,
	.bit_test		= mem_test_bit,
	.free_count		= mem_free_count,
	.snapshot_slice		= mem_snapshot_slice,
	.slice_sync		= NULL,		/* MEM 无 I/O：chunks_asyn → -ENOSYS */
};

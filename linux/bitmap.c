// SPDX-License-Identifier: GPL-2.0
/*
 * bitmap.c —— RavenFS ko：**双位图分片分配器**（全量常驻内存，学 god `bitmap_slices`）
 *
 * 决议（2026-09-26 深夜 + 2026-09-27 复裁，用户裁）：位图 = **全量常驻内存分片**，**不是缓存行**
 * （不纳入 inode/mdata 的缓存状态机与「架构无关回刷 driver」）；**保留分片设计 + 片级同步机制**；
 * 持久化 = 挂载装载 + **分片级同步/回写**（脏分片回写 + `sync_fs`/`put_super` 刷回）——
 * **不得挂载后仅驻留内存不回写**；仍**不做**「bh + mark_buffer_dirty 增量/页缓存推进」那套。
 * 移植规格：`Docs/linux/ravenfs-linux-ko-handover.md` §3.5（god `bitmap_slices` v1.1 → ko）。
 *
 * 设计：
 *   · 两组：block 组（块位图，1 bit/FS_block）与 inode 组（inode 位图，1 bit/槽）。
 *   · 分片：切片数由「2^n ≥ CPU 数 ∩ 满片数上限 ∩ 末片非空迭代」共享推导；
 *     片下限 = 1 个位图块 = 32768 位（4K 对齐）。每片 `spinlock_t`。
 *   · 锁纪律：**分配**只在片内做位运算（持自旋锁）；**释放**跨片逐段清位。
 *     临界区内不做 I/O / 不分配内存。
 *   · 片级同步（2026-09-27 加，**无脏标记**）：分片数按 CPU 切，逐片追踪脏位不划算 ⇒ 不做。
 *     同步 = **锁内 memcpy 快照整片字 → 锁外逐块 I/O**（I/O 绝不入自旋锁）；写者之间由
 *     组内 `wb_lock`(mutex) 串行（不挡分配/释放）。触发：① TTL 线程每 `bitmap_sync_seconds`
 *     （30s）把**所有分片**全同步一遍；② `sync_fs`/`put_super`/`fsync` 走同一 `ravenfs_bitmap_flush`。
 *     崩溃序无法通解，只能靠 ① 把「位图落后内存」的窗口收到 ≤30s 缓解。
 *   · 无保留区：只有 root inode（=16，mkfs 硬编码）被占用；0..15 与 17.. 全部可正常分配。
 *     （2026-09-26 更正：曾误写“inode 组 [0,16) 保留”，该说法不存在，已铲除。）
 *   · **D2/D3 对齐 god（2026-09-28 用户裁）**：补 god 三原语 `alloc_soft` / `soft_occupy` /
 *     `test_interval` + 软分配 / 锚定认领路由（语义 1:1 对齐 `juvenile_god/bitmap_slices.cpp`；
 *     仅坐标系差异：god 片内偏移，ko 全局位号）；preferred 初值 `cpu % slice_count` 铺开，
 *     **搜索式分配（硬/软）一律从 preferred 起扫**（锚定认领锚点片唯一、例外，god 同）。
 *   · alloc 锁维持**阻塞 spinlock**（不改 try —— 2026-09-28 用户裁：阻塞恰配合后续改制）。
 *   · 1 = 已用，0 = 空闲。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/timekeeping.h>
#include <linux/smp.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include "ravenfs.h"
#include "ravenfs_bitmap.h"

/* 片下限：1 个位图块 = 4096B × 8 = 32768 位（保证片基址 4K 对齐） */
#define RFS_BM_SLICE_MIN_BITS	32768ULL
#define RFS_BM_NO_HIT		(~0ULL)

/* ── 位原语（小端字内） ── */
static inline bool rfs_bm_get(const u64 *w, u64 i)
{
	return (w[i >> 6] >> (i & 63)) & 1ULL;
}
static inline void rfs_bm_set(u64 *w, u64 i)
{
	w[i >> 6] |= 1ULL << (i & 63);
}
static inline void rfs_bm_clear(u64 *w, u64 i)
{
	w[i >> 6] &= ~(1ULL << (i & 63));
}

/*
 * 从 `from` 起找下一个**空闲段** [seg_s, seg_e)（均 < nbits）。
 * max_extend>0：段长够 max_extend 即截断（防稀疏大位图 1-bit 分配热点）。
 */
static bool next_free_seg(const u64 *w, u64 nbits, u64 from, u64 max_extend,
			  u64 *seg_s, u64 *seg_e)
{
	u64 i = from;

	while (i < nbits) {
		u64 word = ~w[i >> 6];
		u64 bit;

		word &= ~0ULL << (i & 63);
		if (!word) {
			i = ((i >> 6) + 1) << 6;
			continue;
		}
		bit = (i & ~63ULL) + (u64)__builtin_ctzll(word);
		if (bit >= nbits)
			return false;
		{
			u64 e = bit;

			while (e < nbits) {
				if (rfs_bm_get(w, e))
					break;
				e++;
				if (max_extend && (e - bit) >= max_extend)
					break;
			}
			*seg_s = bit;
			*seg_e = e;
			return true;
		}
	}
	return false;
}

/* 找 [cand, cand+count) 全空闲且 cand % align == 0；无 → RFS_BM_NO_HIT */
static u64 find_free_run(const u64 *w, u64 nbits, u64 from, u64 count, u64 align)
{
	u64 probe = from, seg_s, seg_e;

	if (count == 0 || count > nbits || align == 0)
		return RFS_BM_NO_HIT;
	while (next_free_seg(w, nbits, probe, count + align, &seg_s, &seg_e)) {
		u64 cand = (seg_s + align - 1) & ~(align - 1);

		if (cand < seg_e && cand + count <= seg_e)
			return cand;
		probe = seg_e;
	}
	return RFS_BM_NO_HIT;
}

/* ── 组 / 片 ── */

struct ravenfs_bm_slice {
	u64		base_idx;	/* 全局位起 */
	u64		bits;		/* 本片位数 */
	u64		free_count;
	u64		hint;		/* 搜索起点（全局位号，片内） */
	spinlock_t	lock;
};

struct ravenfs_bm_group {
	const char	*name;
	u64		nbits;		/* 全局位数（= 位图块数 × 32768） */
	u64		words_n;	/* u64 字数 */
	u64		*words;		/* 位图原始内存（vzalloc，页对齐） */
	u32		n_slices;
	u64		per_bits;	/* 每片位数（32768 对齐） */
	struct ravenfs_bm_slice *slices;
	u32		cpu_slots;
	u32		*preferred;	/* CPU → 上次命中片 */

	/* 片级同步：写者之间串行（周期回写 vs sync_fs）；不挡分配/释放 */
	struct mutex	wb_lock;

	/* 装载/刷回映射 */
	u64		disk_start;	/* 起始 FS_block */
	u64		disk_blocks;	/* 块数 */
};

struct ravenfs_bitmap_mgr {
	struct ravenfs_bm_group	blk;
	struct ravenfs_bm_group	ino;
};

/* ── 几何 ── */

static u64 align_up_u64(u64 x, u64 a)
{
	return (x + a - 1) & ~(a - 1);
}

/* 共享切片数：2^n ≥ ncpu ∩ 满片上限 ∩ 末片非空迭代（规格 §A.1） */
static u32 rfs_bm_slice_count(u64 blk_bits, u64 ino_bits, u32 ncpu)
{
	u32 n = 1;
	u64 cap;

	while (n < ncpu && n < (1u << 30))
		n <<= 1;
	cap = min(blk_bits, ino_bits) / RFS_BM_SLICE_MIN_BITS;
	if (!cap)
		cap = 1;
	if ((u64)n > cap)
		n = cap ? (u32)cap : 1;
	for (;;) {
		u64 per_b, per_i;

		if (n <= 1) { n = 1; break; }
		per_b = align_up_u64(DIV_ROUND_UP_ULL(blk_bits, n), RFS_BM_SLICE_MIN_BITS);
		per_i = align_up_u64(DIV_ROUND_UP_ULL(ino_bits, n), RFS_BM_SLICE_MIN_BITS);
		if ((u64)(n - 1) * per_b >= blk_bits ||
		    (u64)(n - 1) * per_i >= ino_bits) {
			n >>= 1;
			continue;
		}
		break;
	}
	return n;
}

static int rfs_bm_group_setup(struct ravenfs_bm_group *g, const char *name,
			      u64 nbits, u32 n_slices, u32 cpu_slots)
{
	u32 i;

	g->name = name;
	g->nbits = nbits;
	g->words_n = DIV_ROUND_UP_ULL(nbits, 64);
	g->n_slices = n_slices;
	g->cpu_slots = cpu_slots;
	g->per_bits = align_up_u64(DIV_ROUND_UP_ULL(nbits, n_slices), RFS_BM_SLICE_MIN_BITS);

	g->words = vzalloc(g->words_n * sizeof(u64));
	if (!g->words)
		return -ENOMEM;
	g->slices = kcalloc(n_slices, sizeof(*g->slices), GFP_KERNEL);
	if (!g->slices)
		return -ENOMEM;
	g->preferred = kcalloc(cpu_slots, sizeof(u32), GFP_KERNEL);
	if (!g->preferred)
		return -ENOMEM;

	/* D3（对齐 god ctor）：preferred 按 cpu 片号铺开 —— 首次分配即分摊，
	 * 不同 CPU 不挤同一片（kzalloc 全 0 会让所有 CPU 首分配串行撞片 0） */
	for (i = 0; i < cpu_slots; i++)
		g->preferred[i] = i % n_slices;

	for (i = 0; i < n_slices; i++) {
		struct ravenfs_bm_slice *s = &g->slices[i];
		u64 base = (u64)i * g->per_bits;

		s->base_idx = base;
		s->bits = (base >= nbits) ? 0 : min(g->per_bits, nbits - base);
		s->free_count = 0;
		s->hint = base;
		spin_lock_init(&s->lock);
	}
	mutex_init(&g->wb_lock);
	return 0;
}

static void rfs_bm_group_scan_free(struct ravenfs_bm_group *g)
{
	u32 i;

	for (i = 0; i < g->n_slices; i++) {
		struct ravenfs_bm_slice *s = &g->slices[i];
		u64 j, n = 0;

		for (j = 0; j < s->bits; j++)
			if (!rfs_bm_get(g->words, s->base_idx + j))
				n++;
		s->free_count = n;
	}
}

/* ── 分配 / 释放（临界区只做位运算） ── */

static u64 rfs_bm_slice_alloc(struct ravenfs_bm_group *g, struct ravenfs_bm_slice *s,
			      u64 count, u64 align)
{
	u64 hi = s->base_idx + s->bits;
	u64 lo = s->base_idx;
	u64 hit = RFS_BM_NO_HIT;

	spin_lock(&s->lock);
	if (s->free_count >= count) {
		u64 from = s->hint < lo ? lo : s->hint;

		hit = find_free_run(g->words, hi, from, count, align);
		if (hit == RFS_BM_NO_HIT)
			hit = find_free_run(g->words, from, lo, count, align);
		if (hit != RFS_BM_NO_HIT) {
			u64 j;

			for (j = 0; j < count; j++)
				rfs_bm_set(g->words, hit + j);
			s->free_count -= count;
			s->hint = hit + count;
		}
	}
	spin_unlock(&s->lock);
	return hit;
}

/* 组内分配：CPU 亲和优先扫描；单片内完成（不跨片） */
static u64 rfs_bm_group_alloc(struct ravenfs_bm_group *g, u64 count, u64 align)
{
	u32 cpu = (u32)raw_smp_processor_id() % g->cpu_slots;
	u32 round;

	for (round = 0; round < g->n_slices; round++) {
		u32 idx = (g->preferred[cpu] + round) % g->n_slices;
		u64 hit = rfs_bm_slice_alloc(g, &g->slices[idx], count, align);

		if (hit != RFS_BM_NO_HIT) {
			g->preferred[cpu] = idx;
			return hit;
		}
	}
	return RFS_BM_NO_HIT;
}

/*
 * ── D2 对齐 god（2026-09-28）：软分配 / 锚定认领 / 区间测试 ──
 * 语义 1:1 对齐 juvenile_god/bitmap_slices.cpp（alloc_soft / soft_occupy /
 * test_interval + 路由 block_alloc_soft / block_occupy_soft）；差异仅坐标系
 * （god 片内偏移，ko 全局位号，同一块内存）与锁策略（ko 阻塞，见文件头）。
 */

/*
 * 软分配（god alloc_soft）：只看**单个段**（不走段链，对齐候选被拒不换段），
 * hint 起段 → 绕回「片内最早段」再试一次；got = min(段长, count)，缺口 *gap。
 * 返回命中起始全局位号 / RFS_BM_NO_HIT。
 * count > 本片位数 → NO_HIT（god RC_ERR_STATE：路由层换片 → 全片等价失败）。
 */
static u64 rfs_bm_slice_alloc_soft(struct ravenfs_bm_group *g, struct ravenfs_bm_slice *s,
				   u64 count, u64 align, u64 *gap)
{
	u64 lo = s->base_idx, hi = lo + s->bits;
	u64 cap = max(count, align);
	u64 from = s->hint < lo ? lo : s->hint;
	u64 seg_s, seg_e, hit = RFS_BM_NO_HIT;
	u64 got, j;

	*gap = count;
	if (!count || count > s->bits || !s->free_count)
		return RFS_BM_NO_HIT;

	if (next_free_seg(g->words, hi, from, cap, &seg_s, &seg_e)) {
		u64 cand = (seg_s + align - 1) & ~(align - 1);

		if (cand < seg_e)
			hit = cand;
	}
	if (hit == RFS_BM_NO_HIT &&
	    next_free_seg(g->words, from, lo, cap, &seg_s, &seg_e)) {
		u64 cand = (seg_s + align - 1) & ~(align - 1);

		if (cand < seg_e)
			hit = cand;
	}
	if (hit == RFS_BM_NO_HIT)
		return RFS_BM_NO_HIT;

	got = seg_e - hit;
	if (got > count)
		got = count;
	for (j = 0; j < got; j++)
		rfs_bm_set(g->words, hit + j);
	s->free_count -= got;
	s->hint = hit + got;
	*gap = count - got;
	return hit;
}

/*
 * 锚定认领（god soft_occupy）：从 base 起贪心认领**连续空闲前缀**，撞第一个
 * 占用位或到额度即停；**永不越片**（cap 片内夹断，跨界续圈在路由层）。
 * 口径：*rest 入 = 已由其它途径满足量（路由恒传 0）→ real = len - *rest；
 *       出 = len - claimed（恒等式与入值无关）。
 * 返回 0；-ENOSPC = 锚点即占用（*rest 出 = len）；-EINVAL = base 出片（rest 不动）。
 * hint 只进不退：回卷游标会打烂搜索分配的空间局部性（god 纪律）。
 */
static int rfs_bm_slice_occupy(struct ravenfs_bm_group *g, struct ravenfs_bm_slice *s,
			       u64 base, u64 len, u64 *rest)
{
	u64 lo = s->base_idx, hi = lo + s->bits;
	u64 real, cap, k = 0, j;

	if (*rest > len)
		return -EINVAL;
	if (!len || *rest == len)
		return 0;			/* real == 0：无事可做（rest 不动） */
	if (base < lo || base >= hi)
		return -EINVAL;
	real = len - *rest;
	cap = min(real, hi - base);

	/* 贪心前缀（字粒度）：与 next_free_seg 段尾延伸同构、方向相反 */
	while (k < cap) {
		u64 pos = (base + k) & 63;
		u64 free_mask = ~g->words[(base + k) >> 6] >> pos;
		u64 run;

		if (!(free_mask & 1))
			break;			/* 当前位占用 → 前缀断 */
		run = (free_mask == ~0ULL) ? (64 - pos) : __builtin_ctzll(~free_mask);
		k += run;
		if (k > cap)
			k = cap;
	}
	if (!k) {
		*rest = len;
		return -ENOSPC;
	}
	for (j = 0; j < k; j++)
		rfs_bm_set(g->words, base + j);
	s->free_count -= k;
	if (base + k > s->hint)
		s->hint = base + k;		/* 只进不退 */
	*rest = len - k;
	return 0;
}

/* 组内软分配路由（god block_alloc_soft）：preferred 起扫（**分配从 preferred 搞起**）；
 * **一片为止**（缺口回传，不跨片拼，调用方再决策）；锁忙阻塞等（ko 纪律） */
static u64 rfs_bm_group_alloc_soft(struct ravenfs_bm_group *g, u64 count, u64 align,
				   u64 *gap)
{
	u32 cpu = (u32)raw_smp_processor_id() % g->cpu_slots;
	u32 round;

	*gap = count;
	for (round = 0; round < g->n_slices; round++) {
		u32 idx = (g->preferred[cpu] + round) % g->n_slices;
		struct ravenfs_bm_slice *s = &g->slices[idx];
		u64 hit;

		spin_lock(&s->lock);
		hit = rfs_bm_slice_alloc_soft(g, s, count, align, gap);
		spin_unlock(&s->lock);
		if (hit != RFS_BM_NO_HIT) {
			g->preferred[cpu] = idx;
			return hit;
		}
	}
	return RFS_BM_NO_HIT;
}

/*
 * 锚定认领路由（god block_occupy_soft）：锚点片唯一 → 阻塞抢锁（不换片，
 * 纪律同 free）；片内前缀**顶到片尾才续圈**下一片（真实连续），中途撞占用即断。
 * 返回原址 base（认领 ≥1）/ PTR_INVALID；*rest = 缺口。
 */
static u64 rfs_bm_group_occupy_soft(struct ravenfs_bm_group *g, u64 base, u64 count,
				    u64 *rest)
{
	u64 nbits = g->nbits;
	u64 b = base, remain = count;

	while (remain && b < nbits) {
		u32 idx = (u32)(b / g->per_bits);
		struct ravenfs_bm_slice *s;
		u64 slice_end, left = 0;
		int rc;

		if (idx >= g->n_slices)
			break;			/* 越界剩余放弃（防御，同 free） */
		s = &g->slices[idx];
		spin_lock(&s->lock);
		rc = rfs_bm_slice_occupy(g, s, b, remain, &left);
		spin_unlock(&s->lock);
		if (rc)
			break;			/* 锚点即占用：前缀断，不跨 */
		b += remain - left;		/* claimed */
		remain = left;
		if (!remain)
			break;
		slice_end = min((u64)(idx + 1) * g->per_bits, nbits);
		if (b != slice_end)
			break;			/* 中途撞占用：跨界续圈无意义 */
		/* b == slice_end == 下一片 base 0 → 续圈 */
	}
	*rest = remain;
	return (count - remain) ? base : (u64)PTR_INVALID;
}

/* 区间同值测试（god test_interval 的组级版：跨片逐段；单片情形语义等价）：
 * [base, base+len) 全为 val → 0；异位 → -ENOENT；越界/溢出 → -EINVAL */
static int rfs_bm_group_test_interval(struct ravenfs_bm_group *g, u64 base, u64 len,
				      bool val)
{
	u64 b = base, end;

	if (len && base + len < base)
		return -EINVAL;			/* 溢出 */
	end = base + len;
	if (end > g->nbits)
		return -EINVAL;
	while (b < end) {
		u32 idx = (u32)(b / g->per_bits);
		struct ravenfs_bm_slice *s;
		u64 slice_end, j;

		if (idx >= g->n_slices)
			return -EINVAL;
		s = &g->slices[idx];
		spin_lock(&s->lock);
		slice_end = min((u64)(idx + 1) * g->per_bits, g->nbits);
		if (slice_end > end)
			slice_end = end;
		for (j = b; j < slice_end; j++) {
			if (rfs_bm_get(g->words, j) != val) {
				spin_unlock(&s->lock);
				return -ENOENT;
			}
		}
		spin_unlock(&s->lock);
		b = slice_end;
	}
	return 0;
}

/* 释放：跨片逐段清位（重复释放不虚高） */
static void rfs_bm_group_free(struct ravenfs_bm_group *g, u64 base, u64 count)
{
	u64 i = base;

	while (count) {
		u32 idx = (u32)(i / g->per_bits);
		struct ravenfs_bm_slice *s;
		u64 hi, n, j;

		if (idx >= g->n_slices)
			break;			/* 越界剩余放弃（防御） */
		s = &g->slices[idx];
		hi = s->base_idx + s->bits;
		if (i >= hi) {			/* 落入片间空洞 → 前进到下一片 */
			i = ((u64)idx + 1) * g->per_bits;
			continue;
		}
		n = min(count, hi - i);
		spin_lock(&s->lock);
		for (j = 0; j < n; j++) {
			if (rfs_bm_get(g->words, i + j)) {
				rfs_bm_clear(g->words, i + j);
				s->free_count++;
			}
		}
		spin_unlock(&s->lock);
		i += n;
		count -= n;
	}
}

/* ── 装载 / 全量刷回 ── */

static int rfs_bm_group_load(struct super_block *sb, struct ravenfs_bm_group *g)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 b;

	for (b = 0; b < g->disk_blocks; b++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, g->disk_start + b));

		if (!bh) {
			pr_err("ravenfs: bitmap(%s) load fail at blk %llu\n",
			       g->name, (unsigned long long)(g->disk_start + b));
			return -EIO;
		}
		memcpy(g->words + b * (RAVENFS_BLOCK_SIZE / sizeof(u64)), bh->b_data,
		       RAVENFS_BLOCK_SIZE);
		brelse(bh);
	}
	rfs_bm_group_scan_free(g);
	return 0;
}

/*
 * 片级同步（无脏标记）：把**全部片**与磁盘同步一遍。
 *   · 原子性：与 alloc/free 对齐的最小临界区 = 片自旋锁内「memcpy 快照整片字」的瞬间
 *     （拷贝完即可解锁，分配照旧只在同一自旋锁内做位运算，不会读到撕裂快照）。
 *   · I/O 全在锁外（绝不入自旋锁）；写者之间由组内 `wb_lock`(mutex) 串行（不挡分配/释放）。
 *   · 快照到 I/O 完成期间的变异不在本次内（下轮再同步）⇒ 只保证“某个一致时间点”落盘，
 *     崩溃序无法通解，窗口由调用周期（TTL 30s）限制。
 *   · ro 挂载不得调用。
 */
static int rfs_bm_group_sync(struct super_block *sb, struct ravenfs_bm_group *g)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 buf_bytes = g->per_bits / 8;
	void *buf;
	u32 i;
	int err = 0;

	if (!g->n_slices || !buf_bytes)
		return 0;
	buf = kvmalloc(buf_bytes, GFP_KERNEL);	/* 片上限缓冲；锁外分配 */
	if (!buf)
		return -ENOMEM;

	mutex_lock(&g->wb_lock);
	for (i = 0; i < g->n_slices; i++) {
		struct ravenfs_bm_slice *s = &g->slices[i];
		u64 nblk, b;

		/* 片 = 整块边界（per_bits/base_idx 均 32768 对齐）⇒ bits/8 为 4K 整数倍 */
		if (!s->bits)
			continue;
		spin_lock(&s->lock);
		memcpy(buf, g->words + s->base_idx / 64, s->bits / 8);
		spin_unlock(&s->lock);

		/* 锁外 I/O：逐块全覆盖写（与装载同源 sb_bread） */
		nblk = s->bits / RAVENFS_BITS_PER_BLOCK;
		for (b = 0; b < nblk; b++) {
			u64 dblk = g->disk_start + s->base_idx / RAVENFS_BITS_PER_BLOCK + b;
			struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, dblk));

			if (!bh) {
				err = -EIO;
				break;
			}
			memcpy(bh->b_data, buf + b * RAVENFS_BLOCK_SIZE, RAVENFS_BLOCK_SIZE);
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);	/* 确定性：返回即落盘 */
			brelse(bh);
		}
		if (err)
			break;		/* 一个片失败即停，剩余片未同步（下轮/下次重试） */
	}
	mutex_unlock(&g->wb_lock);
	kvfree(buf);
	return err;
}

/* ── mgr 生命周期 ── */

int ravenfs_bitmap_create(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_bitmap_mgr *m;
	u64 blk_bits = sbi->bitmap_blocks * RAVENFS_BITS_PER_BLOCK;
	u64 ino_bits = sbi->inode_bm_blocks * RAVENFS_BITS_PER_BLOCK;
	u32 ncpu = num_possible_cpus();
	u32 n_slices;
	int err;

	if (!blk_bits || !ino_bits)
		return -EINVAL;
	n_slices = rfs_bm_slice_count(blk_bits, ino_bits, ncpu);

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	err = rfs_bm_group_setup(&m->blk, "block", blk_bits, n_slices, max(ncpu, 1u));
	if (err)
		goto bad;
	err = rfs_bm_group_setup(&m->ino, "inode", ino_bits, n_slices, max(ncpu, 1u));
	if (err)
		goto bad;

	m->blk.disk_start = sbi->bitmap_start;
	m->blk.disk_blocks = sbi->bitmap_blocks;
	m->ino.disk_start = sbi->inode_bm_base;
	m->ino.disk_blocks = sbi->inode_bm_blocks;

	err = rfs_bm_group_load(sb, &m->blk);
	if (err)
		goto bad;
	err = rfs_bm_group_load(sb, &m->ino);
	if (err)
		goto bad;

	sbi->bitmap = m;
	pr_info("ravenfs: bitmap mgr: slices=%u block_bits=%llu(free=%llu) inode_bits=%llu(free=%llu)\n",
		n_slices, (unsigned long long)blk_bits, (unsigned long long)ravenfs_block_free_total(sb),
		(unsigned long long)ino_bits, (unsigned long long)ravenfs_inode_free_total(sb));
	return 0;
bad:
	sbi->bitmap = NULL;
	kfree(m->ino.preferred);
	kfree(m->ino.slices);
	vfree(m->ino.words);
	kfree(m->blk.preferred);
	kfree(m->blk.slices);
	vfree(m->blk.words);
	kfree(m);
	return err;
}

static void rfs_bm_group_release(struct ravenfs_bm_group *g)
{
	vfree(g->words);
	kfree(g->slices);
	kfree(g->preferred);
}

/* 全片同步（两组）：TTL 周期 / sync_fs / put_super / fsync 共用；ro 挂载不得调用 */
int ravenfs_bitmap_flush(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	int err = 0;

	if (!sbi || !sbi->bitmap)
		return 0;
	if (rfs_bm_group_sync(sb, &sbi->bitmap->blk))
		err = -EIO;
	if (rfs_bm_group_sync(sb, &sbi->bitmap->ino))
		err = -EIO;
	if (err)
		pr_err("ravenfs: bitmap flush failed\n");
	return err;
}

void ravenfs_bitmap_destroy(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;

	if (!m)
		return;
	rfs_bm_group_release(&m->blk);
	rfs_bm_group_release(&m->ino);
	kfree(m);
	sbi->bitmap = NULL;
}

/* ── 对外 API（R-NS/R-FD/R-LM 用） ── */

u64 ravenfs_block_alloc(struct super_block *sb, u64 count, u32 align_log2)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;
	u64 hit, align;

	if (!m || !count)
		return (u64)PTR_INVALID;
	align = 1ULL << min(align_log2, 16u);	/* 防御截断 16 */
	hit = rfs_bm_group_alloc(&m->blk, count, align);
	return (hit == RFS_BM_NO_HIT) ? (u64)PTR_INVALID : hit;
}

/*
 * 软分配（god block_alloc_soft）：单片取 min(片内可给, count)，**一片为止**；
 * preferred 起扫。*remain = 缺口；全片无 → PTR_INVALID 且 *remain = count。
 */
u64 ravenfs_block_alloc_soft(struct super_block *sb, u64 count, u64 *remain)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;
	u64 hit, gap = count;

	if (!m || !count) {
		if (remain)
			*remain = count;
		return (u64)PTR_INVALID;
	}
	hit = rfs_bm_group_alloc_soft(&m->blk, count, 1, &gap);
	if (remain)
		*remain = gap;
	return (hit == RFS_BM_NO_HIT) ? (u64)PTR_INVALID : hit;
}

/*
 * 锚定认领（god block_occupy_soft）：blkbase 起认领 ≤ count 连续空闲块；
 * 返回原址（认领 ≥1）/ PTR_INVALID；*rest = 缺口。
 */
u64 ravenfs_block_occupy_soft(struct super_block *sb, u64 base, u64 count, u64 *rest)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;
	u64 gap = count, ret;

	if (!m || !count) {
		if (rest)
			*rest = count;
		return (u64)PTR_INVALID;
	}
	ret = rfs_bm_group_occupy_soft(&m->blk, base, count, &gap);
	if (rest)
		*rest = gap;
	return ret;
}

/* 区间同值测试（god test_interval 组级版）：0=全同 / -ENOENT=异位 / -EINVAL=越界 */
int ravenfs_block_test_interval(struct super_block *sb, u64 base, u64 len, bool val)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m)
		return -EINVAL;
	return rfs_bm_group_test_interval(&m->blk, base, len, val);
}

void ravenfs_block_free(struct super_block *sb, u64 blk, u64 count)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m || !count)
		return;
	rfs_bm_group_free(&m->blk, blk, count);
}

bool ravenfs_block_test(struct super_block *sb, u64 blk)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m || blk >= m->blk.nbits)
		return false;
	return rfs_bm_get(m->blk.words, blk);
}

u64 ravenfs_inode_alloc(struct super_block *sb)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m)
		return (u64)PTR_INVALID;
	{
		u64 hit = rfs_bm_group_alloc(&m->ino, 1, 1);

		return (hit == RFS_BM_NO_HIT) ? (u64)PTR_INVALID : hit;
	}
}

void ravenfs_inode_free(struct super_block *sb, u64 inum)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m)
		return;
	rfs_bm_group_free(&m->ino, inum, 1);
}

bool ravenfs_inode_test(struct super_block *sb, u64 inum)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m || inum >= m->ino.nbits)
		return false;
	return rfs_bm_get(m->ino.words, inum);
}

u64 ravenfs_block_free_total(struct super_block *sb)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;
	u64 n = 0;
	u32 i;

	if (!m)
		return 0;
	for (i = 0; i < m->blk.n_slices; i++)
		n += m->blk.slices[i].free_count;
	return n;
}

u64 ravenfs_inode_free_total(struct super_block *sb)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;
	u64 n = 0;
	u32 i;

	if (!m)
		return 0;
	for (i = 0; i < m->ino.n_slices; i++)
		n += m->ino.slices[i].free_count;
	return n;
}

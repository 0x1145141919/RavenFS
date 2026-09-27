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
 *   · 无保留区：只有 root inode（=16，mkfs 硬编码）被占用；0..15 与 17.. 全部可正常分配。
 *     （2026-09-26 更正：曾误写“inode 组 [0,16) 保留”，该说法不存在，已铲除。）
 *   · 1 = 已用，0 = 空闲。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include "ravenfs.h"

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

	for (i = 0; i < n_slices; i++) {
		struct ravenfs_bm_slice *s = &g->slices[i];
		u64 base = (u64)i * g->per_bits;

		s->base_idx = base;
		s->bits = (base >= nbits) ? 0 : min(g->per_bits, nbits - base);
		s->free_count = 0;
		s->hint = base;
		spin_lock_init(&s->lock);
	}
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

/* 全量刷回（对齐持久化：sync_fs / put_super 调用；ro 挂载不得调用） */
static int rfs_bm_group_flush(struct super_block *sb, struct ravenfs_bm_group *g)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 b;

	for (b = 0; b < g->disk_blocks; b++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, g->disk_start + b));

		if (!bh)
			return -EIO;
		memcpy(bh->b_data, g->words + b * (RAVENFS_BLOCK_SIZE / sizeof(u64)),
		       RAVENFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);		/* 确定性：返回即落盘 */
		brelse(bh);
	}
	return 0;
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

void ravenfs_bitmap_flush(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;

	if (!sbi || !sbi->bitmap)
		return;
	if (rfs_bm_group_flush(sb, &sbi->bitmap->blk) ||
	    rfs_bm_group_flush(sb, &sbi->bitmap->ino))
		pr_err("ravenfs: bitmap flush failed\n");
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

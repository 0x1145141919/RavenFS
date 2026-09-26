// SPDX-License-Identifier: GPL-2.0
/*
 * inode_cache.c —— RavenFS ko 的 disk_inode_cache_mgr + inode_session
 *
 * 依据：Docs/linux/ravenfs-linux-ko-requirements-spec.md §4.11.1（基调修订）
 *   · 只管**盘 inode 热数据集**（256B 记录）；与 VFS logical inode 解耦。
 *   · 分片 xarray + 片级睡眠 rw_semaphore；分片几何 2^n inode/片（O(1) 位切）。
 *   · 驱逐三路解耦：冷集 → **自写 TTL 淘汰器**（`linux/cache_ttl.c`，主轴）；
 *     折构/卸载 → flush_all() 主动全量刷回。（压力 shrinker 已移除：2026-09-26 深夜）
 *
 * 【M2.2 行尺寸解耦（2026-09-26 定）】
 *   · 缓存行大小由 **mount option `row_size=`** 决定，支持 4K/8K/16K/32K/64K 五档；
 *     行 = 行尺寸/4096 个连续 FS_block（1/2/4/8/16 块）。
 *   · **尾部不完全行**：行覆盖 `[line*ipr, (line+1)*ipr)`；当该区间越过 inode array 末尾时，
 *     按实际存在的块数截断读取（其余零填充），**绝不越界读盘**；行内 inode 槽位仍以
 *     `inum < inode_count` 硬拦（上层 session_get 已保证）。
 *   · 行尺寸只是**内核侧装载粒度**，盘上仍是 256B/inode 连续布局 ⇒ 换行尺寸仍读同一盘。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/xarray.h>
#include <linux/rwsem.h>
#include <linux/refcount.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/timekeeping.h>

#include "ravenfs.h"

/* ── 分片/行几何（与 §4.11.1 + M2.2 逐式一致） ── */
static void ravenfs_geo_init(struct ravenfs_cache *c, u64 inode_count, u32 row_size)
{
	u32 bits = (inode_count <= 1) ? 1 : fls64(inode_count - 1);	/* ceil_log2 */

	c->row_size = row_size;
	c->inodes_per_row = row_size >> 8;		/* 256B/inode */
	c->row_blocks = row_size >> 12;			/* 4K/块 */
	c->row_bits = ilog2(c->inodes_per_row);		/* inodes_per_row 为 2 的幂 */
	c->line_mask = c->inodes_per_row - 1;

	c->bits_len = bits;

	/* ① 先定行：全盘 inode 比一行还少 → 收窄行（唯一残留的收窄情形） */
	if (c->row_bits > bits) {
		c->row_bits = bits;
		c->inodes_per_row = 1u << c->row_bits;
		c->row_size = c->inodes_per_row << 8;
		c->row_blocks = c->row_size >> 12;
		c->line_mask = c->inodes_per_row - 1;
		pr_warn("ravenfs: row_size 超全盘 inode 容量，收窄为 %uB\n", c->row_size);
	}
	/* ② 再定分片：w0 自适应，保证每片容量 ≥ 行容量（否则片内行键位宽为负） */
	c->w0 = RAVENFS_SHARD_SEL_BITS;
	if (c->w0 > bits - c->row_bits)
		c->w0 = bits - c->row_bits;	/* 可至 0 → 单片 */
	c->shard_bits = bits - c->w0;		/* 每片 inode 数 = 2^shard_bits */
	c->row_bits_shard = c->shard_bits - c->row_bits;	/* ≥0（由②保证） */
	c->row_mask = (c->row_bits_shard >= BITS_PER_LONG) ? ~0UL
							   : ((1UL << c->row_bits_shard) - 1);
	c->n_slices = 1u << c->w0;
}

int ravenfs_cache_create(struct super_block *sb, unsigned int row_size)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_cache *c;
	u32 i;

	if (!row_size)
		row_size = RAVENFS_ROW_DEFAULT;
	if (!RAVENFS_ROW_SIZE_OK(row_size))
		return -EINVAL;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	ravenfs_geo_init(c, sbi->inode_count, row_size);
	/* 行缓冲直接走伙伴系统 ⇒ 行字节数必须 ≥ 1 页（4K）。支持表已保证；
	 * 收窄仅见于 inode_count≤8 的病态盘（非良定义，显式拒绝）。 */
	if (c->row_size < PAGE_SIZE) {
		pr_err("ravenfs: row_size %u < PAGE_SIZE，伙伴分配不可用\n", c->row_size);
		kfree(c);
		return -EINVAL;
	}
	c->order = RAVENFS_ORDER_OF(c->row_size);
	c->shards = kcalloc(c->n_slices, sizeof(*c->shards), GFP_KERNEL);
	if (!c->shards) {
		kfree(c);
		return -ENOMEM;
	}
	for (i = 0; i < c->n_slices; i++) {
		xa_init(&c->shards[i].rows);
		init_rwsem(&c->shards[i].lock);
	}
	sbi->icache = c;
	pr_info("ravenfs: inode cache: row=%uB(order=%u, %u inode, %u blk) bits=%u w0=%u shards=%u(片容 %u inode) 片内行位=%u\n",
		c->row_size, c->order, c->inodes_per_row, c->row_blocks,
		c->bits_len, c->w0, c->n_slices, 1u << c->shard_bits, c->row_bits_shard);
	return 0;
}

static void ravenfs_row_free(struct ravenfs_cache *c, struct ravenfs_row *row)
{
	__free_pages(row->page, c->order);	/* 归还伙伴系统 */
	kfree(row);
}

/*
 * 慢路径：独占装载一行。**尾部行截断**：只读到 inode array 实际存在处，
 * 余下零填充（行缓冲恒为 row_size 字节，行内槽位寻址恒定）。
 *
 * ⚠️ 锁纪律（死锁根治）：**睡眠分配 + 块 I/O 一律在分片锁之外完成**，
 * 再进临界区双检插入。原因：① 不在持锁期间做可能睡眠的重活（块 I/O / 页分配）；
 * ② 免受「分配→direct reclaim→重入本缓存取锁」类自锁死（压力 shrinker 已在同轮移除，
 *    但纪律保留作护栏；见 spec §4.11.2 E）。
 */
static int ravenfs_row_load(struct super_block *sb, struct ravenfs_cache *c,
			    struct ravenfs_shard *sh, u32 row_key, u64 line,
			    struct ravenfs_row **out)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_row *row, *old;
	u64 blk0 = sbi->inode_array_base + line * c->row_blocks;
	u32 nblk = c->row_blocks, got;
	int err;

	/* 行首必须落在 inode array 内 */
	if (line * c->row_blocks >= sbi->inode_blocks)
		return -EIO;
	/* 尾部行：截断到 array 末尾 */
	if (blk0 + nblk > sbi->inode_array_base + sbi->inode_blocks)
		nblk = (u32)(sbi->inode_array_base + sbi->inode_blocks - blk0);

	/* —— 锁外：分配 + 读盘（可能睡眠/触发回收） —— */
	row = kzalloc(sizeof(*row), GFP_KERNEL);
	if (!row)
		return -ENOMEM;
	row->page = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, c->order);	/* 零填充，尾部行天然补齐 */
	if (!row->page) {
		kfree(row);
		return -ENOMEM;
	}
	row->data = page_address(row->page);	/* 直映射（物理内存宏访问） */
	for (got = 0; got < nblk; got++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, blk0 + got));

		if (!bh) {
			err = -EIO;
			goto fail_unlocked;
		}
		memcpy((char *)row->data + (u64)got * RAVENFS_BLOCK_SIZE,
		       bh->b_data, RAVENFS_BLOCK_SIZE);
		brelse(bh);
	}
	row->line = line;
	row->stamp = ktime_get_ns();
	refcount_set(&row->ref, 1);			/* 缓存基准引用 */
	row->dirty = false;

	/* —— 临界区：仅 xarray 双检 + 插入 —— */
	down_write(&sh->lock);
	old = xa_load(&sh->rows, row_key);
	if (old) {			/* 双检：他人已装载 → 丢弃自己的，复用他人的 */
		refcount_inc(&old->ref);
		up_write(&sh->lock);
		ravenfs_row_free(c, row);
		*out = old;
		return 0;
	}
	err = xa_insert(&sh->rows, row_key, row, GFP_KERNEL);
	if (err) {
		up_write(&sh->lock);
		goto fail_unlocked;
	}
	refcount_inc(&row->ref);			/* 给本次 session */
	up_write(&sh->lock);
	*out = row;
	return 0;
fail_unlocked:
	ravenfs_row_free(c, row);
	return err;
}

struct ravenfs_session *ravenfs_session_get(struct super_block *sb, u64 inum)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_cache *c = sbi ? sbi->icache : NULL;
	struct ravenfs_session *s;
	struct ravenfs_shard *sh;
	struct ravenfs_row *row;
	u32 sh_idx, row_key;
	u64 line;

	if (!c || inum >= sbi->inode_count)
		return ERR_PTR(-EIO);

	sh_idx = (u32)(inum >> c->shard_bits);
	line = inum >> c->row_bits;
	row_key = (u32)((unsigned long)line & c->row_mask);
	sh = &c->shards[sh_idx];

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return ERR_PTR(-ENOMEM);

	/* 快路径：读锁下查表 + 引用 */
	down_read(&sh->lock);
	row = xa_load(&sh->rows, row_key);
	if (row) {
		refcount_inc(&row->ref);
		row->stamp = ktime_get_ns();	/* 访问刷新（TTL，对齐 god accss_stamp） */
	}
	up_read(&sh->lock);

	if (!row) {			/* 慢路径：装载 */
		int err = ravenfs_row_load(sb, c, sh, row_key, line, &row);

		if (err) {
			kfree(s);
			return ERR_PTR(err);
		}
	}

	s->sbi = sbi;
	s->shard = sh;
	s->row = row;
	s->inum = inum;
	s->off_mask = c->line_mask;
	return s;
}

void ravenfs_session_put(struct ravenfs_session *s)
{
	if (!s)
		return;
	if (s->row)
		refcount_dec(&s->row->ref);
	kfree(s);
}

/* ── TTL 淘汰（cache_ttl.c 周期调用）：只收 refcount==1 ∧ !dirty ∧ stamp<cutoff ── */
long ravenfs_cache_reap_expired(struct ravenfs_cache *c, u64 cutoff_ns)
{
	long freed = 0;
	u32 i;

	for (i = 0; i < c->n_slices; i++) {
		struct ravenfs_shard *sh = &c->shards[i];
		unsigned long victim[64];
		unsigned int nv = 0, k;
		unsigned long key;
		void *e;

		down_write(&sh->lock);
		xa_for_each(&sh->rows, key, e) {
			struct ravenfs_row *row = e;

			if (nv >= ARRAY_SIZE(victim))
				break;
			if (row->dirty || refcount_read(&row->ref) != 1)
				continue;
			if (row->stamp >= cutoff_ns)
				continue;
			victim[nv++] = key;
		}
		for (k = 0; k < nv; k++) {
			struct ravenfs_row *row = xa_erase(&sh->rows, victim[k]);

			if (row) {
				ravenfs_row_free(c, row);
				freed++;
			}
		}
		up_write(&sh->lock);
	}
	return freed;
}

/* ── 驱逐：折构/卸载（主动全量刷回） ── */

void ravenfs_cache_flush_all(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_cache *c = sbi ? sbi->icache : NULL;
	u32 i;

	if (!c)
		return;
	for (i = 0; i < c->n_slices; i++) {
		struct ravenfs_shard *sh = &c->shards[i];
		unsigned long victim[64];
		unsigned int nv, k;
		unsigned long key;
		void *e;

		do {
			nv = 0;
			down_write(&sh->lock);
			xa_for_each(&sh->rows, key, e) {
				struct ravenfs_row *row = e;

				if (nv >= ARRAY_SIZE(victim))
					break;
				if (row->dirty)
					WARN_ON_ONCE(1);	/* M3：先回写 */
				if (refcount_read(&row->ref) != 1)
					pr_warn_ratelimited("ravenfs: inode row %llu 仍被引用(%u)\n",
							    (unsigned long long)row->line,
							    refcount_read(&row->ref));
				victim[nv++] = key;
			}
			for (k = 0; k < nv; k++) {
				struct ravenfs_row *row = xa_erase(&sh->rows, victim[k]);

				if (row)
					ravenfs_row_free(c, row);
			}
			up_write(&sh->lock);
		} while (nv == ARRAY_SIZE(victim));
	}
}

void ravenfs_cache_destroy(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_cache *c = sbi ? sbi->icache : NULL;
	u32 i;

	if (!c)
		return;
	ravenfs_cache_flush_all(sb);
	for (i = 0; i < c->n_slices; i++)
		xa_destroy(&c->shards[i].rows);
	kfree(c->shards);
	kfree(c);
	sbi->icache = NULL;
}

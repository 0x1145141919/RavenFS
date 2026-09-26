// SPDX-License-Identifier: GPL-2.0
/*
 * mdata_cache.c —— RavenFS ko 的 mdata_piece_cache_mgr + mdata_session
 *
 * 技术路线**完全仿照** disk_inode_cache_mgr（inode_cache.c）：
 *   · 分片 `xarray` + 片级睡眠 `rw_semaphore`；`__free()` session；flush_all。
 *   （压力 shrinker 已移除 2026-09-26 深夜；驱逐 = 自写 TTL + 折构/卸载 flush）
 *   · granule（缓存行）**固定 8K** = 1 个 B+tree 节点（= 2 个连续 4K FS_block）——
 *     与 inode 行可配 4K..64K 不同，**mdata 只能是 8K**（8K 是 B+tree 节点的盘面形状）。
 *   · 键 = 节点号 `blk >> 1`（节点首块必为偶块）。
 *   · 行缓冲直接走**伙伴系统**（`alloc_pages`/`__free_pages`，页块 + 直映射），淘汰/卸载归还伙伴系统。
 *   · 尾部节点按 total_blocks 截断读取（零填充），绝不越界读盘。
 *
 * 用法：原来的 `ravenfs_read_node()` 现在就是「mdata session 取节点 → 拷贝 → put」。
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

static void ravenfs_mgeo_init(struct ravenfs_mdata_cache *c, u64 total_blocks)
{
	u32 bits = (total_blocks <= 1) ? 1 : fls64(total_blocks - 1);	/* ceil_log2(块数) */

	c->total_blocks = total_blocks;
	c->bits_len = bits;			/* 以块为单位 */
	c->w0 = (bits <= RAVENFS_SHARD_SEL_BITS) ? 0 : RAVENFS_SHARD_SEL_BITS;
	c->shard_bits = bits - c->w0;		/* 每片容纳块数 = 2^shard_bits（≥1 节点） */
	if (c->shard_bits < 1)
		c->shard_bits = 1;
	c->node_mask = (c->shard_bits >= BITS_PER_LONG) ? ~0UL
							: ((1UL << c->shard_bits) - 1);
	c->n_slices = 1u << c->w0;
}

int ravenfs_mdata_cache_create(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_mdata_cache *c;
	u32 i;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	ravenfs_mgeo_init(c, sbi->total_blocks);
	c->order = RAVENFS_ORDER_OF(RAVENFS_MDATA_NODE_BYTES);	/* 8K → order 1 */
	c->shards = kcalloc(c->n_slices, sizeof(*c->shards), GFP_KERNEL);
	if (!c->shards) {
		kfree(c);
		return -ENOMEM;
	}
	for (i = 0; i < c->n_slices; i++) {
		xa_init(&c->shards[i].rows);
		init_rwsem(&c->shards[i].lock);
	}
	sbi->mcache = c;
	pr_info("ravenfs: mdata cache: node=8K(order=%u, 2 blk) bits=%u w0=%u shards=%u(片容 %lu 块)\n",
		c->order, c->bits_len, c->w0, c->n_slices, 1UL << c->shard_bits);
	return 0;
}

static void ravenfs_mrow_free(struct ravenfs_mdata_cache *c, struct ravenfs_mrow *row)
{
	__free_pages(row->page, c->order);	/* 归还伙伴系统 */
	kfree(row);
}

/* 脏节点落盘：节点 → 2 个连续 4K 块（尾部按 total_blocks 截断）。ro 挂载不得调用。 */
static int ravenfs_mrow_write(struct super_block *sb, struct ravenfs_mdata_cache *c,
			      struct ravenfs_mrow *row)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 blk0 = row->node_idx << 1;
	u32 nblk = RAVENFS_MDATA_BLK_PER_NODE, i;

	if (blk0 >= c->total_blocks)
		return -EIO;
	if (blk0 + nblk > c->total_blocks)
		nblk = (u32)(c->total_blocks - blk0);
	for (i = 0; i < nblk; i++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, blk0 + i));

		if (!bh)
			return -EIO;
		memcpy(bh->b_data, (char *)row->data + (u64)i * RAVENFS_BLOCK_SIZE,
		       RAVENFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}
	return 0;
}

int ravenfs_mdata_writeback_all(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_mdata_cache *c = sbi ? sbi->mcache : NULL;
	int err = 0;
	u32 i;

	if (!c || !ravenfs_sb_writable(sb))
		return 0;
	for (i = 0; i < c->n_slices; i++) {
		struct ravenfs_mdata_shard *sh = &c->shards[i];
		unsigned long key;
		void *e;

		down_write(&sh->lock);
		xa_for_each(&sh->rows, key, e) {
			struct ravenfs_mrow *row = e;

			if (!row->dirty)
				continue;
			if (ravenfs_mrow_write(sb, c, row))
				err = -EIO;
			else
				row->dirty = false;
		}
		up_write(&sh->lock);
	}
	return err;
}

/*
 * 慢路径：独占装载一个节点。
 *
 * ⚠️ 锁纪律（死锁根治）：**睡眠分配 + 块 I/O 一律在分片锁之外完成**，
 * 再进临界区双检插入。原因：① 不在持锁期间做可能睡眠的重活（块 I/O / 页分配）；
 * ② 免受「分配→direct reclaim→重入本缓存取锁」类自锁死（压力 shrinker 已在同轮移除，
 *    但纪律保留作护栏；见 spec §4.11.2 E）。
 */
static int ravenfs_mrow_load(struct super_block *sb, struct ravenfs_mdata_cache *c,
			     struct ravenfs_mdata_shard *sh, unsigned long key,
			     u64 node_idx, struct ravenfs_mrow **out)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_mrow *row, *old;
	u64 blk0 = node_idx << 1;			/* 节点首块（偶） */
	u32 nblk = RAVENFS_MDATA_BLK_PER_NODE, got;
	int err;

	if (blk0 >= c->total_blocks)
		return -EIO;
	if (blk0 + nblk > c->total_blocks)			/* 尾部节点截断 */
		nblk = (u32)(c->total_blocks - blk0);

	/* —— 锁外：分配 + 读盘（可能睡眠/触发回收） —— */
	row = kzalloc(sizeof(*row), GFP_KERNEL);
	if (!row)
		return -ENOMEM;
	row->page = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, c->order);
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
	row->node_idx = node_idx;
	row->stamp = ktime_get_ns();
	refcount_set(&row->ref, 1);
	row->dirty = false;

	/* —— 临界区：仅 xarray 双检 + 插入 —— */
	down_write(&sh->lock);
	old = xa_load(&sh->rows, key);
	if (old) {			/* 双检：他人已装载 → 丢弃自己的，复用他人的 */
		refcount_inc(&old->ref);
		up_write(&sh->lock);
		ravenfs_mrow_free(c, row);
		*out = old;
		return 0;
	}
	err = xa_insert(&sh->rows, key, row, GFP_KERNEL);
	if (err) {
		up_write(&sh->lock);
		goto fail_unlocked;
	}
	refcount_inc(&row->ref);
	up_write(&sh->lock);
	*out = row;
	return 0;
fail_unlocked:
	ravenfs_mrow_free(c, row);
	return err;
}

struct ravenfs_msession *ravenfs_mdata_get(struct super_block *sb, u64 blk)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_mdata_cache *c = sbi ? sbi->mcache : NULL;
	struct ravenfs_msession *s;
	struct ravenfs_mdata_shard *sh;
	struct ravenfs_mrow *row;
	u64 node_idx;
	unsigned long key;
	u32 sh_idx;

	if (!c)
		return ERR_PTR(-EIO);
	node_idx = blk >> 1;
	if ((node_idx << 1) >= c->total_blocks)
		return ERR_PTR(-EIO);
	sh_idx = (u32)(node_idx >> c->shard_bits);
	key = (unsigned long)node_idx & c->node_mask;
	sh = &c->shards[sh_idx];

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return ERR_PTR(-ENOMEM);

	down_read(&sh->lock);
	row = xa_load(&sh->rows, key);
	if (row) {
		refcount_inc(&row->ref);
		row->stamp = ktime_get_ns();	/* 访问刷新（TTL） */
	}
	up_read(&sh->lock);

	if (!row) {
		int err = ravenfs_mrow_load(sb, c, sh, key, node_idx, &row);

		if (err) {
			kfree(s);
			return ERR_PTR(err);
		}
	}
	s->sbi = sbi;
	s->shard = sh;
	s->row = row;
	s->node_idx = node_idx;
	return s;
}

void ravenfs_mdata_put(struct ravenfs_msession *s)
{
	if (!s)
		return;
	if (s->row)
		refcount_dec(&s->row->ref);
	kfree(s);
}

/* 兼容层：原 ravenfs_read_node() 现在走 mdata session（全 ko 节点读统一过大缓存） */
int ravenfs_read_node(struct super_block *sb, u64 blk, void *buf)
{
	struct ravenfs_msession *ms = ravenfs_mdata_get(sb, blk);

	if (IS_ERR(ms))
		return PTR_ERR(ms);
	memcpy(buf, ravenfs_msession_node(ms), RAVENFS_MDATA_NODE_BYTES);
	ravenfs_mdata_put(ms);
	return 0;
}

/* ── TTL 淘汰（cache_ttl.c 周期调用） ── */
long ravenfs_mdata_reap_expired(struct ravenfs_mdata_cache *c, u64 cutoff_ns)
{
	long freed = 0;
	u32 i;

	for (i = 0; i < c->n_slices; i++) {
		struct ravenfs_mdata_shard *sh = &c->shards[i];
		unsigned long victim[64];
		unsigned int nv = 0, k;
		unsigned long key;
		void *e;

		down_write(&sh->lock);
		xa_for_each(&sh->rows, key, e) {
			struct ravenfs_mrow *row = e;

			if (nv >= ARRAY_SIZE(victim))
				break;
			if (row->dirty || refcount_read(&row->ref) != 1)
				continue;
			if (row->stamp >= cutoff_ns)
				continue;
			victim[nv++] = key;
		}
		for (k = 0; k < nv; k++) {
			struct ravenfs_mrow *row = xa_erase(&sh->rows, victim[k]);

			if (row) {
				ravenfs_mrow_free(c, row);
				freed++;
			}
		}
		up_write(&sh->lock);
	}
	return freed;
}

void ravenfs_mdata_flush_all(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_mdata_cache *c = sbi ? sbi->mcache : NULL;
	u32 i;

	if (!c)
		return;
	ravenfs_mdata_writeback_all(sb);	/* 脏节点先落盘（ro 挂载 no-op） */
	for (i = 0; i < c->n_slices; i++) {
		struct ravenfs_mdata_shard *sh = &c->shards[i];
		unsigned long victim[64];
		unsigned int nv, k;
		unsigned long key;
		void *e;

		do {
			nv = 0;
			down_write(&sh->lock);
			xa_for_each(&sh->rows, key, e) {
				struct ravenfs_mrow *row = e;

				if (nv >= ARRAY_SIZE(victim))
					break;
				if (refcount_read(&row->ref) != 1)
					pr_warn_ratelimited("ravenfs: mdata node %llu 仍被引用(%u)\n",
							    (unsigned long long)row->node_idx,
							    refcount_read(&row->ref));
				victim[nv++] = key;
			}
			for (k = 0; k < nv; k++) {
				struct ravenfs_mrow *row = xa_erase(&sh->rows, victim[k]);

				if (row)
					ravenfs_mrow_free(c, row);
			}
			up_write(&sh->lock);
		} while (nv == ARRAY_SIZE(victim));
	}
}

void ravenfs_mdata_cache_destroy(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_mdata_cache *c = sbi ? sbi->mcache : NULL;
	u32 i;

	if (!c)
		return;
	ravenfs_mdata_flush_all(sb);
	for (i = 0; i < c->n_slices; i++)
		xa_destroy(&c->shards[i].rows);
	kfree(c->shards);
	kfree(c);
	sbi->mcache = NULL;
}

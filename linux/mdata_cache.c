// SPDX-License-Identifier: GPL-2.0
/*
 * mdata_cache.c —— RavenFS ko 的 mdata_piece_cache_mgr + mdata_session
 *
 * 技术路线**完全仿照** disk_inode_cache_mgr（inode_cache.c）：
 *   · 分片 `xarray` + 片级睡眠 `rw_semaphore`；`__free()` session；flush_all。
 *   · 状态机（2026-09-27 补票）与 inode_cache.c 同构：
 *     {ref, io_lock(内容+flags), state(READY/EVICTING), acc_stamp, wb_stamp, DIRTY/WB}；
 *     驻留回写 copy-out 排他 → 锁外 I/O（删 gen）；淘汰独占窗口原地写 + 释放。
 *     详见 Docs/linux/ravenfs-ko-cache-statemachine.md。
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
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/timekeeping.h>

#include "ravenfs.h"
#include "ravenfs_cache.h"
#include "ravenfs_mdata_cache.h"

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

/* 脏节点落盘：src → 2 个连续 4K 块（尾部按 total_blocks 截断）。ro 挂载不得调用。 */
static int ravenfs_mrow_store(struct super_block *sb, struct ravenfs_mdata_cache *c,
			      struct ravenfs_mrow *row, const void *src)
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
		memcpy(bh->b_data, (const char *)src + (u64)i * RAVENFS_BLOCK_SIZE,
		       RAVENFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}
	return 0;
}

/* 标 mdata 行脏（见 ravenfs.h 的 RFS_ROW_MARK_DIRTY 宏） */

/*
 * 慢路径：独占装载一个节点。
 *
 * ⚠️ 锁纪律（死锁根治）：**睡眠分配 + 块 I/O 一律在分片锁之外完成**，
 * 再进临界区双检插入。
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
	u64 now;

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
	init_rwsem(&row->io_lock);
	row->flags = 0;
	row->state = ROW_READY;
	now = ktime_get_ns();
	atomic64_set(&row->acc_stamp, now);
	row->wb_stamp = now;
	refcount_set(&row->ref, 1);

	/* —— 临界区：仅 xarray 双检 + 插入 —— */
	down_write(&sh->lock);
	old = xa_load(&sh->rows, key);
	if (old) {			/* 双检：他人已装载 → 丢弃自己的，复用他人的 */
		if (old->state == ROW_EVICTING) {
			up_write(&sh->lock);
			ravenfs_mrow_free(c, row);
			return -EAGAIN;
		}
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

retry:
	row = NULL;
	down_read(&sh->lock);
	row = xa_load(&sh->rows, key);
	if (row && row->state == ROW_EVICTING)
		row = NULL;			/* 淘汰接管中：置空 → 重试/重载 */
	else if (row)
		refcount_inc(&row->ref);
	up_read(&sh->lock);

	if (row) {
		atomic64_set(&row->acc_stamp, ktime_get_ns());	/* 访问刷新（TTL） */
	} else {
		int err = ravenfs_mrow_load(sb, c, sh, key, node_idx, &row);

		if (err == -EAGAIN) {
			cond_resched();
			goto retry;
		}
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

/*
 * 丢弃某节点块的缓存行（平衡/根收缩释放节点块时调用）。
 * 该块内容已被搬走/作废 → 若仍缓存且无人引用则直接丢弃；若被引用则清 dirty
 * （禁回写），防陈旧脏行在块被重新分配并写入新节点后回写污染。
 */
void ravenfs_mdata_drop(struct super_block *sb, u64 blk)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_mdata_cache *c = sbi ? sbi->mcache : NULL;
	struct ravenfs_mdata_shard *sh;
	struct ravenfs_mrow *row;
	u64 node_idx;
	unsigned long key;
	u32 sh_idx;

	if (!c)
		return;
	node_idx = blk >> 1;
	if ((node_idx << 1) >= c->total_blocks)
		return;
	sh_idx = (u32)(node_idx >> c->shard_bits);
	key = (unsigned long)node_idx & c->node_mask;
	sh = &c->shards[sh_idx];

	down_write(&sh->lock);
	row = xa_load(&sh->rows, key);
	if (row) {
		if (row->state == ROW_EVICTING) {
			/* 淘汰接管中：由 reap 负责 erase+free，此处**必须让位**
			 * （否则在 reap 的锁外 store 窗口内 free 该行 → UAF）。 */
		} else if (refcount_read(&row->ref) <= 1) {
			row = xa_erase(&sh->rows, key);
			if (row)
				ravenfs_mrow_free(c, row);
		} else {
			down_write(&row->io_lock);	/* shard→row 顺序一致 */
			row->flags &= ~ROWF_DIRTY;	/* 内容作废：禁回写 */
			up_write(&row->io_lock);
		}
	}
	up_write(&sh->lock);
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

/* ── 写回（2026-09-27）：脏行落盘；**持分片写锁**（与 drop/淘汰/get 串行，复刻旧安全锁序） ── */

int ravenfs_mdata_wb_dirty(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age)
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

			if (row->state != ROW_READY)
				continue;
			if (!(row->flags & ROWF_DIRTY) || (row->flags & ROWF_WB))
				continue;
			if (!ignore_age && row->wb_stamp >= age_cutoff_ns)
				continue;
			if (ravenfs_mrow_store(sb, c, row, row->data))
				err = -EIO;
			else {
				row->flags &= ~ROWF_DIRTY;
				row->wb_stamp = ktime_get_ns();
			}
		}
		up_write(&sh->lock);
	}
	return err;
}

int ravenfs_mdata_writeback_all(struct super_block *sb)
{
	return ravenfs_mdata_wb_dirty(sb, 0, true);
}

/*
 * 单节点回写（fsync 用）：只把「本 inode 树上的」脏节点落盘。
 * 锁序同 `ravenfs_mdata_wb_dirty`（持分片写锁）；节点不在缓存则视为已落盘（no-op）。
 */
int ravenfs_mdata_wb_node(struct super_block *sb, u64 blk)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_mdata_cache *c = sbi ? sbi->mcache : NULL;
	struct ravenfs_msession *ms;
	struct ravenfs_mrow *row;
	int err = 0;

	if (!c || !ravenfs_sb_writable(sb))
		return 0;
	ms = ravenfs_mdata_get(sb, blk);
	if (IS_ERR(ms))
		return PTR_ERR(ms);
	row = ms->row;
	down_write(&ms->shard->lock);
	if (row->state == ROW_READY && (row->flags & ROWF_DIRTY) &&
	    !(row->flags & ROWF_WB)) {
		if (ravenfs_mrow_store(sb, c, row, row->data))
			err = -EIO;
		else {
			row->flags &= ~ROWF_DIRTY;
			row->wb_stamp = ktime_get_ns();
		}
	}
	up_write(&ms->shard->lock);
	ravenfs_mdata_put(ms);
	return err;
}

/* ── TTL 淘汰（cache_ttl.c 周期调用）：超时且 ref==1 的行 → 原地回写 + 释放 ── */
long ravenfs_mdata_reap_expired(struct super_block *sb, u64 cutoff_ns)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_mdata_cache *c = sbi ? sbi->mcache : NULL;
	long freed = 0;
	u32 i;

	if (!c || !ravenfs_sb_writable(sb))
		return 0;
	for (i = 0; i < c->n_slices; i++) {
		struct ravenfs_mdata_shard *sh = &c->shards[i];

		for (;;) {
			unsigned long victim[64];
			unsigned int nv = 0, k;
			unsigned long key;
			void *e;

			down_write(&sh->lock);
			xa_for_each(&sh->rows, key, e) {
				struct ravenfs_mrow *row = e;
				bool pick;

				if (nv >= ARRAY_SIZE(victim))
					break;
				if (row->state != ROW_READY)
					continue;
				if (refcount_read(&row->ref) != 1)
					continue;
				pick = !(row->flags & ROWF_DIRTY) &&
				       !(row->flags & ROWF_WB) &&
				       atomic64_read(&row->acc_stamp) < cutoff_ns;
				if (pick)
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
			if (nv < ARRAY_SIZE(victim))
				break;
		}
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

/* 架构无关回刷 driver 的 mdata 后端 */
const struct ravenfs_wb_ops ravenfs_wb_mdata_ops = {
	.name     = "mdata",
	.wb_dirty = ravenfs_mdata_wb_dirty,
	.reap     = ravenfs_mdata_reap_expired,
};

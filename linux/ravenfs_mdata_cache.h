/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_mdata_cache.h —— mdata_piece_cache_mgr 接口
 * 实现：mdata_cache.c。仿 disk_inode_cache_mgr（granule 固定 8K = 1 个 B+tree 节点），
 *       行机制/标脏宏/回刷 driver 复用 inode 侧 → 依赖 ravenfs_cache.h。
 */
#ifndef _RAVENFS_MDATA_CACHE_H
#define _RAVENFS_MDATA_CACHE_H

#include "ravenfs_cache.h"	/* ROWF_* / ROW_* / RFS_ROW_MARK_DIRTY + wb driver 表 */

/* ── mdata_piece_cache_mgr（仿 disk_inode_cache_mgr；granule 固定 8K = 1 个 B+tree 节点） ── */
#define RAVENFS_MDATA_NODE_BYTES	8192
#define RAVENFS_MDATA_BLK_PER_NODE	2	/* 8K = 2×4K */

struct ravenfs_mrow {
	struct page	*page;		/* 伙伴系统页块（order=1，8K） */
	void		*data;		/* 直映射物理内存 = page_address(page) */
	refcount_t	ref;

	struct rw_semaphore io_lock;	/* 内容 + flags 守护 */
	unsigned int	flags;		/* ROWF_DIRTY | ROWF_WB */
	unsigned long	state;		/* ROW_READY | ROW_EVICTING */

	atomic64_t	acc_stamp;	/* 最近访问（ns，relaxed）→ TTL */
	u64		wb_stamp;	/* 回写 aging（io_lock 护） */
	u64		node_idx;
};

struct ravenfs_mdata_shard {
	struct xarray		rows;	/* 片内 node_idx → struct ravenfs_mrow* */
	struct rw_semaphore	lock;
};

struct ravenfs_mdata_cache {
	struct ravenfs_mdata_shard	*shards;
	u32	order;		/* 节点缓冲 order（8K → 1） */
	u32	n_slices;
	u32	bits_len;	/* ceil_log2(total_blocks) */
	u32	w0;
	u32	shard_bits;	/* 片内容纳的节点数 log2 */
	unsigned long node_mask;
	u64	total_blocks;
};

struct ravenfs_msession {
	struct ravenfs_sb_info		*sbi;
	struct ravenfs_mdata_shard	*shard;
	struct ravenfs_mrow		*row;
	u64				node_idx;
};

/* ── mdata 缓存主体 API（mdata_cache.c） ── */
int  ravenfs_mdata_cache_create(struct super_block *sb);
void ravenfs_mdata_cache_destroy(struct super_block *sb);
long ravenfs_mdata_reap_expired(struct super_block *sb, u64 cutoff_ns);
void ravenfs_mdata_flush_all(struct super_block *sb);
int  ravenfs_mdata_wb_dirty(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age);
int  ravenfs_mdata_writeback_all(struct super_block *sb);
int  ravenfs_mdata_wb_node(struct super_block *sb, u64 blk);	/* 单节点回写（fsync 用） */

/* ── msession（节点租借；mdata_cache.c） ── */
struct ravenfs_msession *ravenfs_mdata_get(struct super_block *sb, u64 blk);
void ravenfs_mdata_put(struct ravenfs_msession *s);
/* 释放节点块前丢弃其 mdata 缓存行（防陈旧脏行回写污染复用块） */
void ravenfs_mdata_drop(struct super_block *sb, u64 blk);

DEFINE_FREE(ravenfs_msession, struct ravenfs_msession *,
	    if (!IS_ERR_OR_NULL(_T)) ravenfs_mdata_put(_T))

#define RAVENFS_MSESSION(sb, blk, var)	\
	struct ravenfs_msession *var __free(ravenfs_msession) = ravenfs_mdata_get((sb), (blk))

static inline void *ravenfs_msession_node(struct ravenfs_msession *s)
{
	return s->row->data;
}

/* 标记 mdata 行脏（io_lock 写护；调用点需含 linux/timekeeping.h） */
#define NODE_DIRTY(ms) RFS_ROW_MARK_DIRTY((ms)->row)

#endif /* _RAVENFS_MDATA_CACHE_H */

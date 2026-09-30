/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_cache.h —— 磁盘 inode 行缓存（§4.11.1）接口
 * 实现：inode_cache.c（行/分片/session 主体）；回刷 driver 表跨 inode_cache.c
 *       与 mdata_cache.c 两个后端，接口统一归此头（架构无关，cache_ttl.c 消费）；
 *       TTL 淘汰器（cache_ttl.c）接口也并入 —— 它就是这套缓存的回收器。
 * 依赖：核心 ravenfs.h。mdata 侧的行机制复用（ROWF_* / RFS_ROW_MARK_DIRTY）
 *       由 ravenfs_mdata_cache.h 反向 include 本头表达。
 */
#ifndef _RAVENFS_CACHE_H
#define _RAVENFS_CACHE_H

#include "ravenfs.h"

/* ── disk_inode_cache_mgr（§4.11.1 + M2.2 行尺寸解耦）── */
#define RAVENFS_SHARD_SEL_BITS	7	/* 分片选择位宽：≤128 片（>4096 inode 时生效） */

/* 支持的缓存行尺寸（mount option `row_size=`，默认 4K） */
#define RAVENFS_ROW_DEFAULT	4096u
#define RAVENFS_ROW_SIZE_OK(sz)	((sz) == 4096u || (sz) == 8192u || (sz) == 16384u || \
				 (sz) == 32768u || (sz) == 65536u)

/*
 * 行缓冲直接走**伙伴系统**（不再经 slab）：order = log2(row_size/4K)。
 * 行尺寸均为 2 的幂（4K..64K）→ order 0..4，天然对齐伙伴分配阶。
 * 访问用**直映射物理内存指针**（page_address()；x86_64 无 highmem）。
 */
#define RAVENFS_ORDER_OF(bytes)	ilog2((bytes) >> PAGE_SHIFT)

/* 行生命周期状态（分片 rwsem 护） */
#define ROW_READY	0UL
#define ROW_EVICTING	1UL
/* 行标志（io_lock 护） */
#define ROWF_DIRTY	(1u << 0)	/* 内容脏，待回写 */
#define ROWF_WB		(1u << 1)	/* copy-out 快照在飞 */

/* 行标脏（io_lock 写护）——inode 行 / mdata 行**通用**（struct 字段同名）。
 * 外部写点统一走此宏（勿再裸写 row->dirty/stamp；调用点需含 linux/{rwsem,timekeeping}.h）。 */
#define RFS_ROW_MARK_DIRTY(row)					\
	do {							\
		down_write(&(row)->io_lock);			\
		(row)->flags |= ROWF_DIRTY;			\
		(row)->wb_stamp = ktime_get_ns();		\
		up_write(&(row)->io_lock);			\
	} while (0)

/*
 * 缓存行状态机（详见 Docs/linux/ravenfs-ko-cache-statemachine.md）：
 *   分片 rwsem —— xarray 成员 + ref pinning + state（get 读锁内 ref++；淘汰写锁内 ref==1 判）
 *   per-row io_lock —— 内容 + flags（读=session 读共享；写=session 变异 / copy-out 排他）
 *   acc_stamp(atomic relaxed) —— TTL；wb_stamp(io_lock) —— 回写 aging
 * 铁律：I/O 一律在锁外（copy-out 后 / 淘汰独占窗口）。
 */
struct ravenfs_row {
	struct page	*page;		/* 伙伴系统页块（order = cache->order，共 row_size 字节） */
	void		*data;		/* 直映射物理内存 = page_address(page) */
	refcount_t	ref;		/* 1=缓存基准；>1=session 持有 / 回写 pin */

	struct rw_semaphore io_lock;	/* 内容 + flags 守护 */
	unsigned int	flags;		/* ROWF_DIRTY | ROWF_WB */
	unsigned long	state;		/* ROW_READY | ROW_EVICTING */

	atomic64_t	acc_stamp;	/* 最近访问（ns，relaxed）→ TTL 淘汰 */
	u64		wb_stamp;	/* 最近落盘/最近 dirty（io_lock 护）→ 回写 aging */
	u64		line;		/* 行号 = inum>>row_bits（诊断用） */
};

struct ravenfs_shard {
	struct xarray		rows;	/* row_key → struct ravenfs_row* */
	struct rw_semaphore	lock;	/* 睡眠读写锁：行生命周期 + 内容 RMW */
};

struct ravenfs_cache {
	struct ravenfs_shard	*shards;
	u32	n_slices;
	u32	bits_len;	/* ceil_log2(inode_count) */
	u32	w0;		/* 分片选择位宽 */
	u32	shard_bits;	/* 每片 inode 数 = 2^shard_bits */

	/* M2.2：缓存行几何（由 mount 的 row_size 派生） */
	u32	row_size;	/* 行字节数（4K..64K） */
	u32	inodes_per_row;	/* = row_size / 256 */
	u32	row_blocks;	/* = row_size / 4096（连续块数） */
	u32	row_bits;	/* log2(inodes_per_row) */
	u32	row_bits_shard;	/* 片内行键位宽 = shard_bits - row_bits */
	unsigned long row_mask;	/* 片内行键掩码（行键为 unsigned long） */
	u32	line_mask;	/* inum & line_mask = 行内槽位 */

	u32	order;		/* 行缓冲的伙伴系统 order（4K→0 … 64K→4） */
};

struct ravenfs_session {
	struct ravenfs_sb_info	*sbi;
	struct ravenfs_shard	*shard;
	struct ravenfs_row	*row;
	u64			inum;
	u32			off_mask;	/* 行内槽位掩码（= inodes_per_row-1） */
};

/* ── 缓存主体 API（inode_cache.c） ── */
int  ravenfs_cache_create(struct super_block *sb, unsigned int row_size);
void ravenfs_cache_destroy(struct super_block *sb);
void ravenfs_cache_flush_all(struct super_block *sb);
/* TTL 淘汰：对超时（acc_stamp<cutoff）且 ref==1 的行做「脏原地回写 → 释放」 */
long ravenfs_cache_reap_expired(struct super_block *sb, u64 cutoff_ns);
/* 驻留回写（copy-out 排他 → 锁外 I/O）：ignore_age=false 时只写 wb_stamp<cutoff 的 */
int  ravenfs_cache_wb_dirty(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age);
int  ravenfs_cache_writeback_all(struct super_block *sb);
int  ravenfs_inode_flush(struct super_block *sb, u64 inum);
int  ravenfs_inode_sync_from_vfs(struct super_block *sb, struct inode *inode);
void ravenfs_inode_mark_dirty(struct super_block *sb, u64 inum);
void ravenfs_inode_reclaim(struct super_block *sb, u64 inum);

/* ── session（行租借；inode_cache.c） ── */
struct ravenfs_session *ravenfs_session_get(struct super_block *sb, u64 inum);
void ravenfs_session_put(struct ravenfs_session *s);

DEFINE_FREE(ravenfs_session, struct ravenfs_session *,
	    if (!IS_ERR_OR_NULL(_T)) ravenfs_session_put(_T))

/* 用法： struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, ino); */
#define RAVENFS_SESSION(sb, inum, var)	\
	struct ravenfs_session *var __free(ravenfs_session) = ravenfs_session_get((sb), (inum))

static inline struct raven_Inode_t *ravenfs_session_inode(struct ravenfs_session *s)
{
	return (struct raven_Inode_t *)((char *)s->row->data +
					(s->inum & s->off_mask) * RAVENFS_INODE_SIZE);
}

/* ── 架构无关回刷 driver（缓存后端：inode 行 / mdata 节点）──
 * 位图（bitmap.c）= 全量常驻内存分片，**非缓存行**、不接入本 driver（自有分片级同步）。 */
struct ravenfs_wb_ops {
	const char	*name;
	int		(*wb_dirty)(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age);
	long		(*reap)(struct super_block *sb, u64 cutoff_ns);
};
extern const struct ravenfs_wb_ops ravenfs_wb_inode_ops;	/* inode_cache.c */
extern const struct ravenfs_wb_ops ravenfs_wb_mdata_ops;	/* mdata_cache.c */

/* ── 自写 TTL 淘汰器（cache_ttl.c）── */
int  ravenfs_ttl_start(struct super_block *sb);
void ravenfs_ttl_stop(struct super_block *sb);

#endif /* _RAVENFS_CACHE_H */

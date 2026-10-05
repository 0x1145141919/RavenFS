/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_cache.h —— 缓存层公共接口（v2.0）
 *
 * 实现：inode_cache.c（inode 会话 = 薄 InodeSession，经 mdata 8K 片访问盘 inode）；
 *       mdata_cache.c（8K 元数据 / inode 片缓存本体）；cache_ttl.c（淘汰 + 周期回写）。
 *
 * v2.0 起 **不再有**「固定 inode 数组上的分片行缓存」：inode 存储 = 全局弹性数据面
 * （8K 片 + 映射树），inode 记录随所属 8K 片落在 **mdata 缓存**里。故：
 *   · `ravenfs_session` 只持「翻译出的物理 8K 片 mdata 会话 + 片内槽指针」；
 *   · 行标脏宏 / 回刷 driver 表继续服务 mdata 后端（inode 后端退化为 no-op）。
 *
 * 依赖：核心 ravenfs.h + mdata 行定义（mrow 需完整：会话持其指针供标脏宏）。
 */
#ifndef _RAVENFS_CACHE_H
#define _RAVENFS_CACHE_H

#include "ravenfs.h"
/* mdata 行定义（struct ravenfs_mrow）—— 会话 row 字段 / 标脏宏需其完整定义。
 * 反向包含由下方 include guard 兜住（mrow 仅按指针用于会话定义，顺序安全）。*/
#include "ravenfs_mdata_cache.h"

/* ── 分片选择位宽（mdata 缓存沿用）── */
#define RAVENFS_SHARD_SEL_BITS	7	/* 分片选择位宽：≤128 片 */

/* mount option `row_size=` 支持表（v2.0：inode 缓存不再按行装载，选项被接受但忽略；
 * 保留宏以维持 super.c 参数解析面）。 */
#define RAVENFS_ROW_DEFAULT	4096u
#define RAVENFS_ROW_SIZE_OK(sz)	((sz) == 4096u || (sz) == 8192u || (sz) == 16384u || \
				 (sz) == 32768u || (sz) == 65536u)

/* 行缓冲伙伴系统 order = log2(8K/4K)=1（mdata 节点 / inode 片恒 8K）*/
#define RAVENFS_ORDER_OF(bytes)	ilog2((bytes) >> PAGE_SHIFT)

/* 行生命周期状态 / 标志（分片 rwsem / io_lock 护；mdata 行沿用） */
#define ROW_READY	0UL
#define ROW_EVICTING	1UL
#define ROWF_DIRTY	(1u << 0)	/* 内容脏，待回写 */
#define ROWF_WB		(1u << 1)	/* copy-out 快照在飞 */

/* 行标脏（io_lock 写护）——inode 片 / mdata 节点**通用**（struct 字段同名）。
 * 外部写点统一走此宏（勿再裸写 row->dirty/stamp；调用点需含 linux/{rwsem,timekeeping}.h）。 */
#define RFS_ROW_MARK_DIRTY(row)					\
	do {							\
		down_write(&(row)->io_lock);			\
		(row)->flags |= ROWF_DIRTY;			\
		(row)->wb_stamp = ktime_get_ns();		\
		up_write(&(row)->io_lock);			\
	} while (0)

/*
 * inode 会话（v2.0 薄壳）：inum → 物理 8K 片（经 inode 数据面翻译）→ mdata 会话；
 *   inode 指针 = 片缓冲 + (inum & 31)×256。
 * `row` 直指 mdata 行（供 `RFS_ROW_MARK_DIRTY(s->row)` 外部标脏点无缝沿用）。
 * `piece_data` 为片缓冲直地址（ravenfs_session_inode 用它，不依赖 mrow 完整定义）。
 */
struct ravenfs_session {
	struct ravenfs_sb_info	*sbi;
	struct ravenfs_msession	*ms;		/* 物理 8K 片 mdata 会话（引用计数归它）*/
	struct ravenfs_mrow	*row;		/* == ms->row；外部标脏宏用 */
	void			*piece_data;	/* 片缓冲（8K）直地址 */
	u64			inum;
};

/* ── 缓存主体 API（inode_cache.c：v2.0 多数为薄壳 / no-op） ── */
int  ravenfs_cache_create(struct super_block *sb, unsigned int row_size);
void ravenfs_cache_destroy(struct super_block *sb);
void ravenfs_cache_flush_all(struct super_block *sb);
long ravenfs_cache_reap_expired(struct super_block *sb, u64 cutoff_ns);
int  ravenfs_cache_wb_dirty(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age);
int  ravenfs_cache_writeback_all(struct super_block *sb);
int  ravenfs_inode_flush(struct super_block *sb, u64 inum);
int  ravenfs_inode_sync_from_vfs(struct super_block *sb, struct inode *inode);
void ravenfs_inode_mark_dirty(struct super_block *sb, u64 inum);
void ravenfs_inode_reclaim(struct super_block *sb, u64 inum);

/* ── session（8K 片租借；inode_cache.c） ── */
struct ravenfs_session *ravenfs_session_get(struct super_block *sb, u64 inum);
void ravenfs_session_put(struct ravenfs_session *s);

DEFINE_FREE(ravenfs_session, struct ravenfs_session *,
	    if (!IS_ERR_OR_NULL(_T)) ravenfs_session_put(_T))

/* 用法： struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, ino); */
#define RAVENFS_SESSION(sb, inum, var)	\
	struct ravenfs_session *var __free(ravenfs_session) = ravenfs_session_get((sb), (inum))

static inline struct raven_Inode_t *ravenfs_session_inode(struct ravenfs_session *s)
{
	return (struct raven_Inode_t *)((char *)s->piece_data +
					(s->inum & (RFS_IDP_INODES_PER_PIECE - 1)) *
					RFS_IDP_INODE_BYTES);
}

/* ── 架构无关回刷 driver（缓存后端：inode 片 / mdata 节点）──
 * v2.0：inode 记录随 8K 片落 mdata → inode 后端退化为 no-op；mdata 后端实效。 */
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

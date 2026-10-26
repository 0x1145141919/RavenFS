/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_inode_session.h —— inode 会话 / inode 缓存公共接口（v2.0）
 *
 * 本头的第一类对象是 **inode 会话**（god `InodeSession` 的 ko 等价物）：
 *   · `ravenfs_inode_session_get/put`：inum → inode 数据面翻译 → 8K 片 mdata 会话；
 *   · `RAVENFS_INODE_SESSION` cleanup 包装（`__free(ravenfs_inode_session)`）；
 *   · `ravenfs_inode_session_record()`：片内 `raven_Inode_t` 记录地址。
 *
 * 注意与核心 `ravenfs_mdata_cache_core.h` 的 **块缓存会话**区分：
 *   · inode 会话：`struct ravenfs_inode_session`，按 **inum** 租借，内含
 *     `struct ravenfs_msession *mdata_session`（底层 8K 片会话）；
 *   · mdata 会话：`struct ravenfs_msession` / `RAVENFS_MDATA_SESSION*`，按
 *     **FS_block** 租借，归平台中立核心所有。
 *
 * v2.0 起 **不再有**「固定 inode 数组上的分片行缓存」：inode 存储 = 全局弹性数据面
 * （8K 片 + 映射树），inode 记录随所属 8K 片落在 mdata 缓存里。本层只做翻译 +
 * 片内取址 + 标脏转发，**不另计引用**（归内嵌 mdata 会话）。
 *
 * 本头不再保留任何 inode 缓存 no-op 壳：inode 记录随 8K 片落 mdata，
 * 淘汰 / 回写 / 生命周期全部归 mdata 核心。剩余非会话接口只有：
 *   · `ravenfs_inode_{flush,sync_from_vfs,mark_dirty,reclaim}`：inode 记录操作；
 *   · `ravenfs_wb_ops` / `ravenfs_ttl_*`：后台回写驱动（inode 后端已删除）。
 *
 * 依赖：核心 ravenfs.h + mdata 核心头（行类型/运行期 API）+ 装配层头（生命周期）。
 */
#ifndef _RAVENFS_INODE_SESSION_H
#define _RAVENFS_INODE_SESSION_H

#include <linux/cleanup.h>
#include <linux/err.h>

#include "ravenfs.h"
/* 平台中立核心：struct ravenfs_mrow_simple / struct ravenfs_msession 与全部
 * 运行期 API（get/put/drop/read/wb/reap/flush/mark_dirty）。缓存对象经
 * `ravenfs_mdata_cache_of(sb)` 取得；本头不再提供任何 sb 版调用薄壳。*/
#include "ravenfs_mdata_cache_core.h"
/* FS 环境装配层：仅 mount / put_super 的 attach / detach。*/
#include "ravenfs_mdata_cache_adapter.h"

/* 底层块缓存会话 RAII 归 core：`RAVENFS_MDATA_SESSION*` 按 FS_block 租借。
 * inode 会话不重复实现引用计数，只把一次 get 的所有权转交给 `mdata_session`。*/

/*
 * ravenfs_inode_session —— inode 访问会话（⇄ god `InodeSession`）
 *
 * 构造：inum →（inode 数据面翻译）物理 8K 片基 → 开一个 mdata 块缓存会话
 *       （引用计数归它）→ 记录片内槽信息。
 * 析构：`ravenfs_inode_session_put()` 归还内嵌 mdata 会话。
 *
 * 字段语义：
 *   · `mdata_session`：底层 8K 片会话（`struct ravenfs_msession`，核心类型不变）；
 *   · `mdata_row`：== `mdata_session->row`，供访问时间戳等只读/原子访问；
 *   · `piece_data`：8K 片缓冲直地址（等价 god `piece_.data`）；
 *   · `inum`：逻辑 inode 号（片内槽 = inum & 31）。
 *
 * 标脏统一转发到 mdata 会话：`RAVENFS_MDATA_DIRTY(s->mdata_session)`。
 */
struct ravenfs_inode_session {
	struct ravenfs_msession	*mdata_session;	/* 底层 8K 片会话（owning ref）*/
	struct ravenfs_mrow_simple *mdata_row;	/* == mdata_session->row */
	void			*piece_data;	/* 片缓冲（8K）直地址 */
	u64			inum;
};

/* ── inode 记录操作（inode_cache.c） ── */
int  ravenfs_inode_flush(struct super_block *sb, u64 inum);
int  ravenfs_inode_sync_from_vfs(struct super_block *sb, struct inode *inode);
void ravenfs_inode_mark_dirty(struct super_block *sb, u64 inum);
void ravenfs_inode_reclaim(struct super_block *sb, u64 inum);

/* ══════════════════════ inode 会话（按 inum 租借）══════════════════════
 * 与核心 `RAVENFS_MDATA_SESSION*`（按 FS_block 租借块缓存）严格分层：
 *   · 本组只负责 inum→物理片翻译、片内取址、标脏转发；
 *   · 引用计数 / 生命周期由内嵌 `mdata_session` 持有。
 */
struct ravenfs_inode_session *
ravenfs_inode_session_get(struct super_block *sb, u64 inum);
void ravenfs_inode_session_put(struct ravenfs_inode_session *s);

/* RAII（cleanup 属性）：作用域退出自动 put；ERR_PTR/NULL 安全跳过。*/
DEFINE_FREE(ravenfs_inode_session, struct ravenfs_inode_session *,
	    if (!IS_ERR_OR_NULL(_T)) ravenfs_inode_session_put(_T))

/* 声明 + 获取（⇄ core 的 RAVENFS_MDATA_SESSION；必须放在块内）。*/
#define RAVENFS_INODE_SESSION(var, sb, inum)				\
	struct ravenfs_inode_session *var				\
		__free(ravenfs_inode_session) =				\
		ravenfs_inode_session_get((sb), (inum))

/* 片内 inode 记录地址（⇄ god `InodeSession::inode` / `inode_slot_of`）。*/
static inline struct raven_Inode_t *
ravenfs_inode_session_record(struct ravenfs_inode_session *s)
{
	return (struct raven_Inode_t *)((char *)s->piece_data +
					(s->inum & (RFS_IDP_INODES_PER_PIECE - 1)) *
					RFS_IDP_INODE_BYTES);
}

/* ── 后台回写 driver（唯一实效后端：mdata 节点/片）──
 * v2.0：inode 记录随 8K 片落 mdata，inode 专用后端已删除。 */
struct ravenfs_wb_ops {
	int		(*wb_dirty)(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age);
	long		(*reap)(struct super_block *sb, u64 cutoff_ns);
	/* v0.2.3：空闲触发的强淘汰（gear/ttl 语义同 ioctl）。
	 * 0 = 已执行，或后端闸门（ro/shutdown/无缓存）直接跳过；
	 * -EAGAIN = 强淘汰互斥忙；其他负值 = 脏行写回失败等。*/
	int		(*squeeze)(struct super_block *sb, u32 gear, u64 ttl_cutoff_ns);
};
extern const struct ravenfs_wb_ops ravenfs_wb_mdata_ops;	/* mdata_cache_adapter.c */

/* ── 自写 TTL 淘汰器（cache_ttl.c）── */
int  ravenfs_ttl_start(struct super_block *sb);
void ravenfs_ttl_stop(struct super_block *sb);

#endif /* _RAVENFS_INODE_SESSION_H */

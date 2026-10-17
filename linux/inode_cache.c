// SPDX-License-Identifier: GPL-2.0
/*
 * inode_cache.c —— RavenFS ko：`ravenfs_inode_session`（⇄ god InodeSession）
 *                  + inode 缓存生命周期 + 回刷 driver 表
 *
 * v2.0（Docs/disk_layout_v2.0.md §12/§17.2）：inode 存储 = 全局弹性数据面
 * （8K 片 direct/tree + 映射树 + 扁 fr）。**固定 inode 数组 + inode 位图 + 分片行
 * 缓存整体退役** —— inode 记录随所属 8K 片落在 mdata 片缓存（mdata_cache_core.c）。
 *
 * 本文件职责：
 *   · ravenfs_inode_session_get/put —— inode 会话：inum →（inode 数据面翻译）
 *     物理 8K 片 → 开底层 `mdata_session`（`struct ravenfs_msession`）→
 *     inode 记录 = 片缓冲 + (inum & 31) × 256。引用计数归 mdata 块会话。
 *   · 标脏 / 落盘：inode 记录 = 所在 8K 片的 mdata 行；统一经 mdata 回写。
 *   · 回刷 driver 表：inode 后端退化为 no-op（真值全在 mdata 后端）。
 *
 * 会话命名分层：本文件的 `ravenfs_inode_session*` 只按 inum 租借；
 * `struct ravenfs_msession` / `RAVENFS_MDATA_SESSION*` 是按 FS_block 租借的
 * 块缓存会话（定义在 ravenfs_mdata_cache_core.h，语义不复用）。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/timekeeping.h>

#include "ravenfs.h"
#include "ravenfs_inode_session.h"
#include "ravenfs_allocator_top.h"

/* ── inode 会话（按 inum 租借；引用计数转发给底层 mdata 块会话） ── */

struct ravenfs_inode_session *ravenfs_inode_session_get(struct super_block *sb, u64 inum)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_inode_session *s;
	u64 blk = 0;
	u32 off = 0;
	int rc;

	if (!sbi || !sbi->alloc) {
		RFS_STAT_INC(sb, ic_get_err);
		return ERR_PTR(-EIO);
	}
	if (inum >= rfs_allocator_inode_capacity(sbi->alloc)) {
		RFS_STAT_INC(sb, ic_get_err);
		return ERR_PTR(-EIO);
	}

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		RFS_STAT_INC(sb, ic_get_err);
		return ERR_PTR(-ENOMEM);
	}

	/* inum → 物理 8K 片基（inode 数据面翻译）*/
	rc = rfs_allocator_inode_locate(sbi->alloc, inum, &blk, &off);
	if (rc) {
		kfree(s);
		RFS_STAT_INC(sb, ic_locate_err);
		return ERR_PTR(rc);
	}
	RAVENFS_MDATA_SESSION(ms, ravenfs_mdata_cache_of(sb), blk);
	if (!RAVENFS_MDATA_SESSION_VALID(ms)) {
		kfree(s);
		RFS_STAT_INC(sb, ic_err);
		return ERR_PTR(PTR_ERR(ms));
	}
	s->mdata_session = ms;
	s->mdata_row = ms->row;
	s->piece_data = RAVENFS_MDATA_SESSION_DATA(ms);
	s->inum = inum;
	RAVENFS_MDATA_SESSION_DISARM(ms);	/* 所有权转交 s->mdata_session */
	RFS_STAT_INC(sb, ic_get);
	return s;
}

void ravenfs_inode_session_put(struct ravenfs_inode_session *s)
{
	if (!s)
		return;
	RAVENFS_MDATA_SESSION_RELEASE(s->mdata_session);
	kfree(s);
}

/* ── 单 inode 落盘（fsync 用）：冲其所在 8K 片的 mdata 脏行 ── */
int ravenfs_inode_flush(struct super_block *sb, u64 inum)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 blk = 0;
	u32 off = 0;
	int rc;

	RFS_STAT_INC(sb, ic_flush);
	if (!sbi || !sbi->alloc || !ravenfs_sb_writable(sb) ||
	    ravenfs_is_shutdown(sb))
		return 0;
	rc = rfs_allocator_inode_locate(sbi->alloc, inum, &blk, &off);
	if (rc)
		return rc;
	return ravenfs_mdata_wb_node(sbi->mcache, blk);
}

/*
 * `write_inode` 的实现：**logical inode → 片内盘记录同步**（不落盘，标脏）。
 * 只推 VFS 拥有的字段；盘专有字段（data_root / inline_data / xattr_ptr / type /
 * direct）一律不动。内容写完后经 `RAVENFS_MDATA_DIRTY(s->mdata_session)` 标脏（核心片锁
 * 与回写互斥，无需 per-row io_lock）。
 */
int ravenfs_inode_sync_from_vfs(struct super_block *sb, struct inode *inode)
{
	struct ravenfs_inode_session *s __free(ravenfs_inode_session) =
		ravenfs_inode_session_get(sb, RFS_INO_TO_IDX(inode->i_ino));
	struct raven_Inode_t *r;
	struct timespec64 ts;

	if (IS_ERR(s))
		return PTR_ERR(s);
	r = ravenfs_inode_session_record(s);
	r->uid = i_uid_read(inode);
	r->gid = i_gid_read(inode);
	r->mode_xattr = (r->mode_xattr & ~RFS_MODE_MASK) | (inode->i_mode & 0o7777);
	r->link_count = inode->i_nlink;
	r->size = inode->i_size;
	ts = inode_get_mtime(inode);
	r->mtime = ts.tv_sec;
	r->m_ns_stamp = (u32)ts.tv_nsec;
	ts = inode_get_ctime(inode);
	r->ctime = ts.tv_sec;
	r->c_ns_stamp = (u32)ts.tv_nsec;
	ts = inode_get_atime(inode);
	r->atime = ts.tv_sec;
	r->a_ns_stamp = (u32)ts.tv_nsec;
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		__le32 rdev_le = cpu_to_le32(new_encode_dev(inode->i_rdev));

		memcpy(r->inline_data, &rdev_le, sizeof(rdev_le));
	}
	RAVENFS_MDATA_DIRTY(s->mdata_session);
	return 0;
}

/* FS 内部改完盘记录后：标所属 8K 片 dirty（落盘交 flush/周期回写） */
void ravenfs_inode_mark_dirty(struct super_block *sb, u64 inum)
{
	struct ravenfs_inode_session *s __free(ravenfs_inode_session) = ravenfs_inode_session_get(sb, inum);

	if (IS_ERR_OR_NULL(s))
		return;
	RAVENFS_MDATA_DIRTY(s->mdata_session);
	RFS_STAT_INC(sb, ic_dirty);
}

/*
 * nlink==0 的 inode 回收（evict_inode 调用）：记录清零（防脏片回写复活）+ inode
 * 索引回收。**只回收 inode 记录**；data 块 / 区段树由调用方随后释放。
 */
void ravenfs_inode_reclaim(struct super_block *sb, u64 inum)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_inode_session *s __free(ravenfs_inode_session) = ravenfs_inode_session_get(sb, inum);

	RFS_STAT_INC(sb, ic_evict);
	if (!IS_ERR_OR_NULL(s)) {
		memset(ravenfs_inode_session_record(s), 0, RAVENFS_INODE_SIZE);
		RAVENFS_MDATA_DIRTY(s->mdata_session);
	}
	rfs_allocator_inode_free(sbi->alloc, inum);	/* 数据面 fr 回收 */
}

/* ══════════════════════════ 回刷 / 淘汰归属 ══════════════════════════
 * v2.0：inode 记录 = 所属 8K 片内容，没有独立的 per-inode 行缓存。
 * 因此本文件**不提供** cache_create/destroy/wb_dirty/reap_expired/
 * writeback_all/flush_all 等空壳；脏回写、淘汰、生命周期全部由
 * mdata 核心（`sbi->mcache`）承担，后台驱动只剩 `ravenfs_wb_mdata_ops`。
 */

// SPDX-License-Identifier: GPL-2.0
/*
 * inode_cache.c —— RavenFS ko：inode 会话（v2.0 薄 InodeSession）+ 回刷 driver 表
 *
 * v2.0（Docs/disk_layout_v2.0.md §12/§17.2）：inode 存储 = 全局弹性数据面
 * （8K 片 direct/tree + 映射树 + 扁 fr）。**固定 inode 数组 + inode 位图 + 分片行
 * 缓存整体退役** —— inode 记录随所属 8K 片落在 mdata 片缓存（mdata_cache.c）。
 *
 * 本文件职责：
 *   · ravenfs_session_get/put —— 薄会话：inum →（inode 数据面翻译）物理 8K 片 →
 *     开 mdata 会话 → inode = 片缓冲 + (inum&31)×256。引用计数归 mdata 会话。
 *   · 标脏 / 落盘：inode 记录 = 所在 8K 片的 mdata 行；统一经 mdata 回写。
 *   · 回刷 driver 表：inode 后端退化为 no-op（真值全在 mdata 后端）。
 *
 * 外部 API（ravenfs_session_inode / ->row / ravenfs_inode_* / ravenfs_cache_*）逐字保留。
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
#include "ravenfs_cache.h"
#include "ravenfs_mdata_cache.h"
#include "ravenfs_allocator_top.h"

/* ── inode 会话（v2.0 薄壳；引用计数归 mdata 会话） ── */

struct ravenfs_session *ravenfs_session_get(struct super_block *sb, u64 inum)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_msession *ms;
	struct ravenfs_session *s;
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
	ms = ravenfs_mdata_get(sb, blk);
	if (IS_ERR(ms)) {
		kfree(s);
		RFS_STAT_INC(sb, ic_err);
		return ERR_PTR(PTR_ERR(ms));
	}
	s->ms = ms;
	s->row = ms->row;
	s->piece_data = ravenfs_msession_node(ms);
	s->inum = inum;
	RFS_STAT_INC(sb, ic_get);
	return s;
}

void ravenfs_session_put(struct ravenfs_session *s)
{
	if (!s)
		return;
	if (s->ms)
		ravenfs_mdata_put(s->ms);
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
	if (!sbi || !sbi->alloc || !ravenfs_sb_writable(sb))
		return 0;
	rc = rfs_allocator_inode_locate(sbi->alloc, inum, &blk, &off);
	if (rc)
		return rc;
	return ravenfs_mdata_wb_node(sb, blk);
}

/*
 * `write_inode` 的实现：**logical inode → 片内盘记录同步**（不落盘，标脏）。
 * 只推 VFS 拥有的字段；盘专有字段（data_root / inline_data / xattr_ptr / type /
 * direct）一律不动。内容改写在 mdata 行 io_lock 写锁内完成。
 */
int ravenfs_inode_sync_from_vfs(struct super_block *sb, struct inode *inode)
{
	struct ravenfs_session *s __free(ravenfs_session) =
		ravenfs_session_get(sb, RFS_INO_TO_IDX(inode->i_ino));
	struct raven_Inode_t *r;
	struct timespec64 ts;

	if (IS_ERR(s))
		return PTR_ERR(s);
	down_write(&s->row->io_lock);
	r = ravenfs_session_inode(s);
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
	s->row->flags |= ROWF_DIRTY;
	s->row->wb_stamp = ktime_get_ns();
	up_write(&s->row->io_lock);
	return 0;
}

/* FS 内部改完盘记录后：标所属 8K 片 dirty（落盘交 flush/周期回写） */
void ravenfs_inode_mark_dirty(struct super_block *sb, u64 inum)
{
	struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, inum);

	if (IS_ERR_OR_NULL(s))
		return;
	RFS_ROW_MARK_DIRTY(s->row);
	RFS_STAT_INC(sb, ic_dirty);
}

/*
 * nlink==0 的 inode 回收（evict_inode 调用）：记录清零（防脏片回写复活）+ inode
 * 索引回收。**只回收 inode 记录**；data 块 / 区段树由调用方随后释放。
 */
void ravenfs_inode_reclaim(struct super_block *sb, u64 inum)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, inum);

	RFS_STAT_INC(sb, ic_evict);
	if (!IS_ERR_OR_NULL(s)) {
		down_write(&s->row->io_lock);
		memset(ravenfs_session_inode(s), 0, RAVENFS_INODE_SIZE);
		s->row->flags |= ROWF_DIRTY;
		s->row->wb_stamp = ktime_get_ns();
		up_write(&s->row->io_lock);
	}
	rfs_allocator_inode_free(sbi->alloc, inum);	/* 数据面 fr 回收 */
}

/* ══════════════════════════ 回刷 driver（v2.0）══════════════════════════
 * inode 记录 = mdata 8K 片内容 → inode 后端 no-op；mdata 后端承担全部脏回写。 */

int ravenfs_cache_wb_dirty(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age)
{
	(void)sb;
	(void)age_cutoff_ns;
	(void)ignore_age;
	return 0;
}

int ravenfs_cache_writeback_all(struct super_block *sb)
{
	/* inode 记录随片落盘：统一走 mdata 全量回写 */
	return ravenfs_mdata_writeback_all(sb);
}

long ravenfs_cache_reap_expired(struct super_block *sb, u64 cutoff_ns)
{
	(void)sb;
	(void)cutoff_ns;
	return 0;		/* 淘汰归 mdata 后端 */
}

void ravenfs_cache_flush_all(struct super_block *sb)
{
	(void)ravenfs_mdata_writeback_all(sb);
}

/* v2.0：inode 缓存无独立实体（恒为空壳）；保留符号以维持 super.c 生命周期调用面。 */
int ravenfs_cache_create(struct super_block *sb, unsigned int row_size)
{
	(void)sb;
	(void)row_size;
	return 0;
}

void ravenfs_cache_destroy(struct super_block *sb)
{
	(void)sb;
}

/* 架构无关回刷 driver 的 inode 后端（v2.0：no-op；实效在 mdata 后端） */
const struct ravenfs_wb_ops ravenfs_wb_inode_ops = {
	.wb_dirty = ravenfs_cache_wb_dirty,
	.reap     = ravenfs_cache_reap_expired,
};

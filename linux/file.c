// SPDX-License-Identifier: GPL-2.0
/*
 * file.c —— RavenFS ko：文件数据路径的 a_ops / read_iter / write_iter 壳（M4.1 读 + M4.2 写）
 *
 * 依据：Docs/linux/ravenfs-linux-ko-M4-data-spec.md
 *   §2.1（file_operations 挂点）、§2.2（iomap 家族 a_ops）、§2.3（writepages/writeback_ops）、
 *   §5（mmap 前置 = read_folio）、§6（读粒度 = iomap 默认 D-M4-7）、§8（回写只查已分配映射）。
 * 惯例参照：fs/zonefs/file.c（最小 iomap 接线）、fs/xfs/xfs_aops.c（a_ops 家族 + writeback_range）。
 *
 * 纪律：
 *   · **绝不挂 write_begin/write_end**（iomap 不需要；spec §1.1/§2.2）。
 *   · read_folio 是 mmap 的硬前置（D-M4-1）——无它 generic_file_mmap_prepare 直接 -ENOEXEC。
 *   · writepages/writeback_range **只查「已分配映射」**（写时已分配，D-M4-6），回写期**不分配**。
 *   · 无 direct_IO（O_DIRECT 后置 D-M4-5：open(O_DIRECT) 直接 -EINVAL）。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/uio.h>

#include "ravenfs.h"

/* 缺页 / read(2) 的页填充 = iomap 默认 bio 读（走 iomap_bio_read_ops） */
static int ravenfs_read_folio(struct file *file, struct folio *folio)
{
	iomap_bio_read_folio(folio, &ravenfs_iomap_ops);
	return 0;
}

static void ravenfs_readahead(struct readahead_control *rac)
{
	iomap_bio_readahead(rac, &ravenfs_iomap_ops);
}

/*
 * 回写映射：只查「已分配映射」（wpc->iomap 复用；跨越边界才重查）。
 * 写时已分配（D-M4-6）→ 回写期**不分配**；洞返回 HOLE，iomap 核心跳过（不写）。
 */
static ssize_t ravenfs_writeback_range(struct iomap_writepage_ctx *wpc,
				       struct folio *folio, u64 pos,
				       unsigned int len, u64 end_pos)
{
	struct inode *inode = wpc->inode;
	struct iomap srcmap;

	if (pos < wpc->iomap.offset ||
	    pos >= wpc->iomap.offset + wpc->iomap.length) {
		int err = ravenfs_iomap_begin(inode, pos, len, 0,
					      &wpc->iomap, &srcmap);

		if (err)
			return err;
	}
	/* inline 态不产生脏页（走 write_end_inline）；若见之，拒绝而非静默错写 */
	if (wpc->iomap.type != IOMAP_MAPPED && wpc->iomap.type != IOMAP_HOLE)
		return -EIO;

	return iomap_add_to_ioend(wpc, folio, pos, end_pos, len);
}

static const struct iomap_writeback_ops ravenfs_writeback_ops = {
	.writeback_range	= ravenfs_writeback_range,
	.writeback_submit	= iomap_ioend_writeback_submit,
};

static int ravenfs_writepages(struct address_space *mapping,
			      struct writeback_control *wbc)
{
	struct iomap_writepage_ctx wpc = {
		.inode		= mapping->host,
		.wbc		= wbc,
		.ops		= &ravenfs_writeback_ops,
	};

	return iomap_writepages(&wpc);
}

/* iomap 家族 a_ops（spec §2.2，7.2.3 核实版）：无 write_begin/write_end */
const struct address_space_operations ravenfs_aops = {
	.read_folio		= ravenfs_read_folio,
	.readahead		= ravenfs_readahead,
	.writepages		= ravenfs_writepages,
	.dirty_folio		= iomap_dirty_folio,
	.release_folio		= iomap_release_folio,
	.invalidate_folio	= iomap_invalidate_folio,
	.migrate_folio		= filemap_migrate_folio,
	.is_partially_uptodate	= iomap_is_partially_uptodate,
	.error_remove_folio	= generic_error_remove_folio,
};

/*
 * buffered 读入口（spec §2.1：自写壳）。M4.1 直接委派 generic_file_read_iter
 * （页缓存 + iomap read_folio/readahead 全在框架内）；后续 atime/只读特判可在此加。
 */
ssize_t ravenfs_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	return generic_file_read_iter(iocb, iter);
}

/*
 * buffered 写入口（spec §2.1/§2.3：自写壳 → generic_write_checks → iomap_file_buffered_write）。
 * 立刻分配（D-M4-6）落在 ravenfs_iomap_begin(WRITE)；ENOSPC 经此直接暴露给 write(2)。
 */
ssize_t ravenfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	if (unlikely(IS_IMMUTABLE(inode) || IS_APPEND(inode)))
		return -EPERM;
	if (sb_rdonly(inode->i_sb))
		return -EROFS;			/* readonly 拒写 */
	if (!iov_iter_count(from))
		return 0;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out;
	ret = file_remove_privs(iocb->ki_filp);	/* 清 S_ISUID/S_ISGID（generic_write 惯例） */
	if (ret)
		goto out;

	ret = iomap_file_buffered_write(iocb, from, &ravenfs_iomap_ops, NULL, NULL);
out:
	inode_unlock(inode);
	return ret;
}

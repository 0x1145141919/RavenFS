// SPDX-License-Identifier: GPL-2.0
/*
 * file.c —— RavenFS ko：M4.1 只读文件数据路径的 a_ops / read_iter 壳
 *
 * 依据：Docs/linux/ravenfs-linux-ko-M4-data-spec.md
 *   §2.1（file_operations 挂点）、§2.2（iomap 家族 a_ops）、§5（mmap 前置 = read_folio）、
 *   §6（读粒度 = iomap 默认，D-M4-7）。
 * 惯例参照：fs/zonefs/file.c（最小 iomap 接线）、fs/xfs/xfs_aops.c（a_ops 家族）。
 *
 * 纪律：
 *   · **绝不挂 write_begin/write_end**（iomap 不需要；spec §1.1/§2.2）。
 *   · read_folio 是 mmap 的硬前置（D-M4-1）——无它 generic_file_mmap_prepare 直接 -ENOEXEC。
 *   · M4.1 只读：不挂 writepages / direct_IO（后续里程碑）。
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

/* iomap 家族 a_ops（spec §2.2，7.2.3 核实版）：无 write_begin/write_end */
const struct address_space_operations ravenfs_aops = {
	.read_folio		= ravenfs_read_folio,
	.readahead		= ravenfs_readahead,
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

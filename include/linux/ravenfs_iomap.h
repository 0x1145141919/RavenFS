/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_iomap.h —— iomap 数据映射接口（iomap.c 实现）
 * VFS 侧读写入口在 ravenfs_file.h；这里只管「块怎么映射」。
 */
#ifndef _RAVENFS_IOMAP_H
#define _RAVENFS_IOMAP_H

#include "ravenfs.h"

/* ── M4.1 只读数据路径（iomap.c） ── */
struct iomap_ops;
struct iomap;
extern const struct iomap_ops ravenfs_iomap_ops;

/* M4.2：回写映射复用同一 begin（file.c 的 writeback_range 使用） */
int ravenfs_iomap_begin(struct inode *inode, loff_t pos, loff_t length,
			unsigned int flags, struct iomap *iomap, struct iomap *srcmap);

#endif /* _RAVENFS_IOMAP_H */

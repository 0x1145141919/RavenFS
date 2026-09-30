/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_file.h —— 文件数据路径 VFS 接口（file.c 实现）
 * 读/写/mmap/truncate/punch/fallocate/setattr + a_ops + evict 数据回收。
 * iomap 机械本体在 ravenfs_iomap.h（iomap.c）。
 */
#ifndef _RAVENFS_FILE_H
#define _RAVENFS_FILE_H

#include "ravenfs.h"

/* ── M4.1 只读数据路径（file.c） ── */
extern const struct address_space_operations ravenfs_aops;
ssize_t ravenfs_file_read_iter(struct kiocb *iocb, struct iov_iter *iter);
/* M4.2：可写 mmap 立刻分配（D-M4-12）——挂自写 vm_ops（page_mkwrite→iomap 写向） */
struct vm_area_desc;
int ravenfs_file_mmap_prepare(struct vm_area_desc *desc);

/* ── M4.2 写数据路径（file.c） ── */
ssize_t ravenfs_file_write_iter(struct kiocb *iocb, struct iov_iter *iter);

/* ── M4.3 truncate / punch / fallocate（file.c） ── */
long ravenfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len);
int ravenfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr);

/* ── M4 收口：evict_inode 数据回收（file.c）──
 * nlink==0 的 REG/SOFT_SYM 外置数据释放（复用 truncate 出口 1 的下半段释放链）：
 *   direct → free 连续 run；extent → release 整棵区段树。
 * 前置纪律 = 先 inode 后 free（§10 不变量 25 / handover §6.1）：调用方须先
 * ravenfs_inode_reclaim 清零盘记录，再调用本函数；inline（size ≤160）由调用方跳过。 */
void ravenfs_release_inode_data(struct super_block *sb, u64 data_root, u64 size, bool direct);

#endif /* _RAVENFS_FILE_H */

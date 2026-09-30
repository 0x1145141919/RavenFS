/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_dir.h —— 目录 / 名字 VFS 操作接口
 * 实现：dentry.c（lookup/readdir/增删改名/链接族 + 类型→DT 转换）；
 *       symlink 双态 i_op 表定义在 super.c（与 getattr/setattr/fileattr
 *       回调同编译单元），namei 侧装配引用。
 */
#ifndef _RAVENFS_DIR_H
#define _RAVENFS_DIR_H

#include "ravenfs.h"

/* ── 跨文件接口（super.c ↔ dentry.c，M2） ── */
struct dentry *ravenfs_lookup(struct inode *dir, struct dentry *dentry,
			      unsigned int flags);
int ravenfs_iterate(struct file *file, struct dir_context *ctx);
int ravenfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
		   umode_t mode, bool excl);
struct dentry *ravenfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
			     umode_t mode);
int ravenfs_unlink(struct inode *dir, struct dentry *dentry);
int ravenfs_rmdir(struct inode *dir, struct dentry *dentry);
int ravenfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
		   struct inode *new_dir, struct dentry *new_dentry, unsigned int flags);

/* M5 残（R-NS-05/06/07/09）：硬链接 / 符号链接 / 特殊节点 / O_TMPFILE */
int ravenfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);
int ravenfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
		    const char *symname);
int ravenfs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
		  umode_t mode, dev_t rdev);
int ravenfs_tmpfile(struct mnt_idmap *idmap, struct inode *dir, struct file *file,
		    umode_t mode);

/* symlink 双态 i_op（R-NS-06）：fast = simple_get_link / slow = page_get_link。
 * 定义在 super.c（与 getattr/setattr/fileattr 回调同编译单元），namei 侧装配引用。 */
extern const struct inode_operations ravenfs_fast_symlink_inode_operations;
extern const struct inode_operations ravenfs_symlink_inode_operations;

/* 盘 inode 类型 → dirent d_type（readdir 用；dentry.c 实现） */
unsigned int ravenfs_type_dt(u32 type);

#endif /* _RAVENFS_DIR_H */

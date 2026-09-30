/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_xattr.h —— xattr / ACL 接口（xattr.c 实现）
 *
 * s_xattr 处理器表（user / security / system；**trusted 不注册 → 拒绝**）。
 * ACL（system.posix_acl_access/default）不进 s_xattr，由 VFS 特殊路径转发到
 * i_op->get_inode_acl/set_acl（见 fs/xattr.c is_posix_acl_xattr）。
 */
#ifndef _RAVENFS_XATTR_H
#define _RAVENFS_XATTR_H

#include "ravenfs.h"

/* ── M6：xattr / ACL（xattr.c）── */
struct xattr_handler;
extern const struct xattr_handler * const ravenfs_xattr_handlers[];

int  ravenfs_xattr_get(struct inode *inode, int name_index, const char *name,
		       void *buffer, size_t size);
int  ravenfs_xattr_set(struct inode *inode, int name_index, const char *name,
		       const void *value, size_t size, int flags);
ssize_t ravenfs_listxattr(struct dentry *dentry, char *buffer, size_t size);
/* ACL（R-XA-03）：值 opaque（内核通用 posix_acl_xattr v2 格式），默认 ACL 继承 */
struct posix_acl;
struct posix_acl *ravenfs_get_inode_acl(struct inode *inode, int type, bool rcu);
int  ravenfs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		     struct posix_acl *acl, int type);
int  ravenfs_init_acl(struct inode *inode, struct inode *dir);

#endif /* _RAVENFS_XATTR_H */

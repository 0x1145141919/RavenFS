/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_dentry_tree.h —— FS 顶层原语接口（dentry_tree.c 的导出面）
 * 三分拆分（2026-09-28）：原 ravenfs_dentry_write.h 改名演进 —— 接待层（dentry.c）
 * 对 dentry 树的全部操作入口。节点布局细节不在本头（经 ravenfs_dentry_node.h 下沉）。
 */
#ifndef _RAVENFS_DENTRY_TREE_H
#define _RAVENFS_DENTRY_TREE_H

#include "ravenfs.h"
#include "ravenfs_dentry_node.h"	/* rfs_dpath（walk API 签名依赖）+ 底层导出 */

/* 删除一个条目（含 balance/合并/抹匀/根收缩）；info 回传被删条目的下游资产信息 */
struct rfs_delinfo {
	u64	inum;		/* 目标 inode 编号（52 位形态） */
	u32	name_len;	/* 全名长（RFS_PTR_FREE） */
	u64	remain;		/* 长名尾段定位；短名 = PTR_INVALID */
};

/* ── dentry B+树 写入（dentry_tree.c）：多级 walk + 插入（含分裂/升根） ── */
int ravenfs_ddentry_walk(struct super_block *sb, u64 dir_ino, const char *name, u32 nlen,
			struct rfs_dpath *p);
int ravenfs_ddentry_insert(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p,
			  const char *name, u32 nlen, u64 hash, u64 inum, u64 remain_ptr);
int ravenfs_ddentry_delete(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p,
			   struct rfs_delinfo *info);

/* ── 读侧整树查询 / 叶链游标 / 目录级条目原语（三分拆分自 dentry.c）── */
int ravenfs_dir_find(struct super_block *sb, u64 dir_ino,
			const char *name, u32 nlen, u64 *ino);
int ravenfs_read_leaf(struct super_block *sb, u64 blk, void *buf, u32 *cnt);
/* 从 root 一路沿 hints[0] 下到最左叶子 */
int ravenfs_leftmost_leaf(struct super_block *sb, u64 blk, u64 *leaf);
/* 写目录的盘上父指针（根节点 ptrs[1] = ".."；叶/内根一致） */
int rfs_dir_set_dotdot(struct super_block *sb, u64 dir_ino, u64 parent_ino);
/* 交换两条目的 inode_ptr（EXCHANGE）；两条可能共叶 */
int rfs_swap_entries(struct super_block *sb, const struct rfs_dpath *op,
			    const struct rfs_dpath *np);
int ravenfs_dir_root_leaf_init(struct super_block *sb, u64 blk, u64 inum, u64 parent_ino);

#endif /* _RAVENFS_DENTRY_TREE_H */

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_extent.h —— 区段树（extent tree）机械接口（extent_write.c 实现）
 * 查找 / 插入（含叶裂升根）/ 计数 / 空根叶 / 子集撤销。
 * 调用方：file.c（截断/回收）、iomap.c（映射）、dentry 路径间接经 file。
 */
#ifndef _RAVENFS_EXTENT_H
#define _RAVENFS_EXTENT_H

#include "ravenfs.h"

/* 区段树写机械（extent_write.c）：查找 / 插入（含叶内四形态融合 + 前瞻预算 + 叶裂升根）/ 计数 */
int ravenfs_extent_lookup(struct super_block *sb, u64 root, u64 target,
			  struct extent_t *out, u64 *next);
/* 插入；D-5a/b：叶内四形态融合 + 预判法预算池。根更迭（both 致根收缩）旧根经
 * old_root 上抛（延迟 free，调用方在 inode.data_root 提交后 free）；不需要可传 NULL。*/
int ravenfs_extent_insert(struct super_block *sb, u64 ino, u64 *root_io,
			  const struct extent_t *ext, u64 *old_root);
int ravenfs_extent_count(struct super_block *sb, u64 root, u64 *data, u64 *nodes);

/* ── M4.3 区段树删除/平衡（extent_write.c）── */
/* 空根叶（direct→extent 物化 / 空树终态） */
int ravenfs_extent_new_root_leaf(struct super_block *sb, u64 ino, u64 *root_io);
/* 子集撤销（loose）；根更迭旧根经 old_root 上抛（延迟 free，调用方在 inode 提交后 free） */
int ravenfs_extent_unmap_loose(struct super_block *sb, u64 ino, u64 *root_io,
			       const struct extent_t *ext, u64 *old_root);

#endif /* _RAVENFS_EXTENT_H */

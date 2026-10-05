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
/*
 * ── 三族 extent 树（kind）──
 * 同一套节点写机械服务三族树：文件区段树 / 目录名字数据面映射树 / 全局 inode
 * 数据面映射树。三族节点布局逐字节同构（Bptree_extent_{leaf,inner}_node_t），
 * 仅 256-bit 魔数不同；kind 仅决定「认哪套魔数」（见 include/magics.h 与
 * god is_extent_node_kind）。FILE 为历史行为基线，与既有公开函数逐字节一致。
 */
enum rfs_extent_kind {
	RFS_EXT_KIND_FILE     = 0,	/* 文件区段树：BPTREE_MAGIC1..4 */
	RFS_EXT_KIND_NAMEDATA = 1,	/* 目录名字数据面映射树：NAMEDATA_EXTENT_MAGIC1..4 */
	RFS_EXT_KIND_INODE    = 2,	/* 全局 inode 数据面映射树：INODE_DATA_EXTENT_MAGIC1..4 */
};

int ravenfs_extent_lookup(struct super_block *sb, u64 root, u64 target,
			  struct extent_t *out, u64 *next);
/* 插入；D-5a/b：叶内四形态融合 + 预判法预算池。根更迭（both 致根收缩）旧根经
 * old_root 上抛（延迟 free，调用方在 inode.data_root 提交后 free）；不需要可传 NULL。*/
int ravenfs_extent_insert(struct super_block *sb, u64 ino, u64 *root_io,
			  const struct extent_t *ext, u64 *old_root);
int ravenfs_extent_count(struct super_block *sb, u64 root, u64 *data, u64 *nodes);

/* ── kind 参数化同名机械（名字数据面 / inode 数据面映射树）──
 * 语义与上列 FILE 版逐字节一致，仅节点魔数按 kind 选择。 */
int ravenfs_extent_lookup_kind(struct super_block *sb, u32 kind, u64 root,
			       u64 target, struct extent_t *out, u64 *next);
int ravenfs_extent_insert_kind(struct super_block *sb, u32 kind, u64 ino,
			       u64 *root_io, const struct extent_t *ext, u64 *old_root);
int ravenfs_extent_count_kind(struct super_block *sb, u32 kind, u64 root,
			      u64 *data, u64 *nodes);
int ravenfs_extent_new_root_leaf_kind(struct super_block *sb, u32 kind, u64 ino,
				      u64 *root_io);

/* 孤儿树释放（kind 参数化）：释放子树**全部节点**（每节点 2 块），
 * 数据块 phys 零碰（归调用方）。镜像 god extent_orphan_release —— 左叶下钻 +
 * 子树 DFS；结构异常即弃疗（孤儿留盘 fsck 收）。名字数据面 map_reclaim 复用。 */
void ravenfs_extent_orphan_release_kind(struct super_block *sb, u32 kind, u64 root);

/* ── M4.3 区段树删除/平衡（extent_write.c）── */
/* 空根叶（direct→extent 物化 / 空树终态） */
int ravenfs_extent_new_root_leaf(struct super_block *sb, u64 ino, u64 *root_io);
/* 子集撤销（loose）；根更迭旧根经 old_root 上抛（延迟 free，调用方在 inode 提交后 free） */
int ravenfs_extent_unmap_loose(struct super_block *sb, u64 ino, u64 *root_io,
			       const struct extent_t *ext, u64 *old_root);

#endif /* _RAVENFS_EXTENT_H */

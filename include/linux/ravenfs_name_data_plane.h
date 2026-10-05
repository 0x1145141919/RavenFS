/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_name_data_plane.h —— 目录名字数据面（name data plane）接口（v1.3）
 *
 * 语义出处：god `juvenile_god/name_data_plane.cpp`（逐接口移植，内核适配）。
 * 规格：Docs/disk_layout_v1.3_name_data_plane.md。
 *
 * 每目录一个**稠密、无空洞、8K 对齐**的逻辑字节空间：
 *   映射     name_data_ctl            direct(1)/tree(0) + 51 位指针
 *   片数     name_data_piece_count    实际 8K 数据片数原值
 *   空闲区间 name_data_free_ranges_*  fr 表（inline(≤9) / B+树，见 name_data_freeranges.c）
 * 长名（>40B）tail（≤512B）由本层分配逻辑区间；ddentry 存逻辑偏移
 * full_char_remain_offset，经映射树解析到物理。
 *
 * 映射树复用通用 extent 机械（kind = RFS_EXT_KIND_NAMEDATA）；
 * fr 复用平台中立模块 rfs_ndfr_*（linux/name_data_freeranges.c）。
 */
#ifndef _RAVENFS_NAME_DATA_PLANE_H
#define _RAVENFS_NAME_DATA_PLANE_H

#include "ravenfs.h"

/* ── 顶层操作（dir_idx = 目录盘 inode 编号；offset = 逻辑字节偏移）── */

/* 分配 tail_len（≤512B）逻辑区间；返回 0 时 *out_offset = 逻辑字节偏移 */
int ravenfs_nd_alloc(struct super_block *sb, u64 dir_idx, u16 tail_len,
		     u64 *out_offset);
/* 释放逻辑区间 [offset, offset+tail_len)（插入 fr） */
int ravenfs_nd_free(struct super_block *sb, u64 dir_idx, u64 offset, u16 tail_len);
/* 读写逻辑区间 [offset, offset+tail_len) 字节（≤512B，跨至多 2 个 4K 块） */
int ravenfs_nd_read(struct super_block *sb, u64 dir_idx, u64 offset, u16 tail_len,
		    char *out);
int ravenfs_nd_write(struct super_block *sb, u64 dir_idx, u64 offset, u16 tail_len,
		     const char *tail);
/* 目录销毁：全量回收数据片 + 映射树节点 + fr 树节点 + 双控制字复位 */
int ravenfs_nd_release(struct super_block *sb, u64 dir_idx);
/* 增长一个 8K 片（direct 物理尾 soft_occupy 优先，否则 hard_alloc + 映射注入） */
int ravenfs_nd_grow(struct super_block *sb, u64 dir_idx);
/* 逻辑偏移 → 物理块 + 块内偏移（dino 已由调用方持会话） */
int ravenfs_nd_resolve(struct super_block *sb, struct raven_dir_Inode_t *dino,
		       u64 offset, u64 *out_fblk, u16 *out_off);
/* 追加一个 8K 数据片（phys_base 为调用方已分配的 8K 对齐物理起点） */
int ravenfs_nd_map_append(struct super_block *sb, u64 dir_idx,
			  struct raven_dir_Inode_t *dino, u64 phys_base);
/* 归还一棵名字数据面映射树的数据片 + 节点（不碰 inode 控制字） */
void ravenfs_nd_map_reclaim(struct super_block *sb, u64 tree_root);

#endif /* _RAVENFS_NAME_DATA_PLANE_H */

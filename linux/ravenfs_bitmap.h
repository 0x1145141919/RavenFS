/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_bitmap.h —— 双位图分片分配器接口（bitmap.c 实现）
 * 全量常驻内存分片（非缓存行），自有分片级同步；不接入回刷 driver。
 */
#ifndef _RAVENFS_BITMAP_H
#define _RAVENFS_BITMAP_H

#include "ravenfs.h"

/* ── 双位图分片分配器（bitmap.c）── */
struct ravenfs_bitmap_mgr;
int  ravenfs_bitmap_create(struct super_block *sb);
void ravenfs_bitmap_destroy(struct super_block *sb);
/* 全片同步两组位图（锁内快照 → 锁外 I/O）；TTL 周期 / sync_fs/put_super/fsync 共用 */
int  ravenfs_bitmap_flush(struct super_block *sb);
u64  ravenfs_block_alloc(struct super_block *sb, u64 count, u32 align_log2);
void ravenfs_block_free(struct super_block *sb, u64 blk, u64 count);
bool ravenfs_block_test(struct super_block *sb, u64 blk);
u64  ravenfs_inode_alloc(struct super_block *sb);
void ravenfs_inode_free(struct super_block *sb, u64 inum);
bool ravenfs_inode_test(struct super_block *sb, u64 inum);
/* D2 对齐 god（2026-09-28）：软分配 / 锚定认领 / 区间同值测试（bitmap_slices 同款语义） */
u64  ravenfs_block_alloc_soft(struct super_block *sb, u64 count, u64 *remain);
u64  ravenfs_block_occupy_soft(struct super_block *sb, u64 base, u64 count, u64 *rest);
int  ravenfs_block_test_interval(struct super_block *sb, u64 base, u64 len, bool val);
u64  ravenfs_block_free_total(struct super_block *sb);
u64  ravenfs_inode_free_total(struct super_block *sb);

#endif /* _RAVENFS_BITMAP_H */

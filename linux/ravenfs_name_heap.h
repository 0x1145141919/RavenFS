/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_name_heap.h —— 名字堆接口（name_heap.c 实现）
 * 长名（>40B）外置部分的 分配/写/释放。
 */
#ifndef _RAVENFS_NAME_HEAP_H
#define _RAVENFS_NAME_HEAP_H

#include "ravenfs.h"

/* ── 名字堆（name_heap.c）：长名（>40B）外置部分的 分配/写/释放 ── */
int ravenfs_name_heap_alloc(struct super_block *sb, u64 dir_ino, u32 name_len,
			    u64 *out_remain_ptr);
int ravenfs_name_heap_write(struct super_block *sb, u32 name_len, u64 remain_ptr,
			   const char *tail);
int ravenfs_name_heap_free(struct super_block *sb, u64 dir_ino, u32 name_len,
			   u64 remain_ptr);
/* 释放目录的全部【已空】名字堆并重置描述符（rmdir / 目录析构） */
int ravenfs_dir_name_heap_release(struct super_block *sb, u64 dir_ino);

#endif /* _RAVENFS_NAME_HEAP_H */

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_allocator_adapter.h —— 运行时树分配器 struct rfs_allocator 的 FS 侧生命周期接线
 *                                （allocator_adapter.c ⇄ super.c / cache_ttl.c）
 *
 * 定位：把 `rfs_allocator`（运行时树分配器）挂到卷生命周期，并独占「位图镜像」的
 *   words 与块设备 IO（F5 裁决：落盘归 FS 适配层）：
 *   · 挂载   `ravenfs_allocator_create()`  —— 设备读位图镜像 → words → 建树（位图作 checkpoint 底座）
 *   · TTL/同步/卸载 `ravenfs_allocator_flush()` —— 逐片 dump 树 → 补集写回 words → **words 写回设备**
 *                                              （= 树快照 + 落盘，一步到位）
 *   · 卸载   `ravenfs_allocator_destroy()` —— 关闸 + 排空 + 拆树 + 释放 words
 *
 * ── 无回退（硬切）────────────────────────────────────────────────────────
 *   `rfs_allocator` 是**运行时唯一真值层**：FS 分配调用点全部直调 `rfs_allocator_*`
 *   （见 `rfs_allocator_top.h`），旧位图模块（`rfs_bm` 类 + bitmap_adapter）已整体删除。
 *   挂载**必建**树；位图几何取自 sbi（`bitmap_start/blocks`、`inode_bm_base/blocks`）→
 *   本层自持 words + IO。无迁移开关、无回退。位图仍作 checkpoint 底座 + 落盘载体（磁盘契约不变）。
 *
 * 边界：本头只暴露生命周期三函数，**不含**分配/释放接口（那些属 `rfs_allocator_top.h`，
 *   由上层直调）。`struct super_block` 前向声明。
 */
#ifndef _RAVENFS_ALLOCATOR_ADAPTER_H
#define _RAVENFS_ALLOCATOR_ADAPTER_H

#include "ravenfs_platform.h"		/* bool / u8 / u64（唯一平台收口）*/

#ifdef __cplusplus
extern "C" {
#endif

struct super_block;			/* 前向声明；定义属内核 <linux/fs.h> */

/* 挂载：设备读位图镜像 → words → 建树（必建、无回退）。
 * 幂等（已建则 0）；几何非法 / IO 失败 / 建树失败 → 负 -errno → 挂载失败。*/
int	ravenfs_allocator_create(struct super_block *sb);

/* 同步：逐片 dump 树 → 补集写回 words → **words 写回设备**（确定性，返回即落盘）。
 * 无管理器 → 0；ro 挂载只做内存 checkpoint、不写盘。TTL / sync_fs / fsync / put_super 共用。*/
int	ravenfs_allocator_flush(struct super_block *sb);

/* 卸载：关闸 + 在途排空（fail-safe：超时不释放）+ 拆树 + 释放管理器；幂等。*/
void	ravenfs_allocator_destroy(struct super_block *sb);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_ALLOCATOR_ADAPTER_H */

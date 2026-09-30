/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_bitmap_backend.h —— 双位图分片分配器「后端 ops」私有接口
 *                              (bitmap_core.c ⇄ bitmap_mem.c ⇄ bitmap_chunk.c)
 *
 * 定位：**唯一**的路由层（bitmap_core.c）与两后端之间的一层薄契约。
 *   · 路由层（bitmap_core.c）只有一份实现：共享几何/装配、per-CPU preferred
 *     亲和扫描、跨片/跨窗 occupy·free 续圈、global↔local 换算——全部经本 ops
 *     接口调用后端，**不把任何路由逻辑复制两份**。
 *   · 后端（bitmap_mem.c = god v1；bitmap_chunk.c = god v2）各自实现本 ops，
 *     并把 god 的私有方法一律保持 `static` 在各自 `.c` 内。
 *
 * ── 坐标契约（关键；与 ravenfs_bitmap_core.h 头注释一致）──
 *   公开方法（rfs_bm_*）**入参出参恒为全局位号**。
 *   后端 ops 在各自「**片自然坐标**」下工作：
 *     · MEM   （god v1）：片 = **全局视角**（片自带 base_idx）⇒ ops 直接吃/吐
 *       全局位号（换算恒等）；
 *     · CHUNK （god v2）：片 = **片内局部（0 起）**⇒ ops 吃/吐片内偏移。
 *   换算统一收在路由层 bitmap_core.c 的 bm_to_local()/bm_to_global()
 *   （CHUNK 经 `struct rfs_bm_cslice::in_global_base_idx`）。
 *
 * ── 视角（**语义判定前提**）──
 *   MEM 后端：位图在内存里**就是一等事实**，ops 的读写/计数即真值。
 *   CHUNK 后端：位态/计数是**合成视角**——由在线窗 + 挂账归还链 + 盘上块在合并点
 *     合成；挂账期读数可与真值暂不一致。**比对/判定必须基于合成后（提交态）视角**
 *     （先触发合并再比），不得拿逐步视图逐拍对齐 MEM。详见 ravenfs_bitmap_core.h ★★。
 *
 * ── 锁契约 ──
 *   每个 ops 自行取其片的锁（blocking）：
 *     · MEM   片锁 = `rfs_spin_t`（阻塞自旋；临界区内不 I/O、不分配内存）；
 *     · CHUNK 片锁 = `rfs_mutex_t`（可睡，允许持锁等盘 I/O）。
 *   路由层**不编排锁**（god v1 mgr 的 try_lock_write / god v2 的片内 try_lock
 *   统一收敛为片内阻塞锁——见 bitmap_core.c 文件顶「与 god 的刻意差异」）。
 *
 * ── 返回码 ──
 *   0 = 成功 / 负 -errno。分配类：0 命中（回填片自然坐标起始位）/ -ENOSPC
 *   （本片无货，路由换片）/ 其它负错误（同样换片，对齐 god v2 mgr 的 `continue`）。
 */
#ifndef _RAVENFS_BITMAP_BACKEND_H
#define _RAVENFS_BITMAP_BACKEND_H

#include "ravenfs_bitmap_core.h"

/* errno 值由 ravenfs_platform.h 统一提供（内核 <linux/errno.h> / 用户态 <errno.h>）；
 * 本头不再自带兜底——平台差异唯一收口在 ravenfs_platform.h。*/

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 后端 ops 表。arg0 恒为组（便于同时取到 words / cslices 与几何），
 * idx = 片下标（0..n_slices-1）；坐标见文件顶契约。
 */
struct rfs_bm_ops {
	const char *name;		/* "mem" / "chunk"（日志用）*/

	/* ── 装配 / 拆解 / 重扫（rfs_bm_init / fini / rescan_free 调）──
	 * group_setup：core 已把 g 的通用字段（mode/nbits/per_bits/n_slices/
	 *   cpu_slots）与模式专属字段（MEM 的 words；CHUNK 的 disk_base/disk_blocks/
	 *   chunk_order）填好，再据此建分片数组。失败须自清理本组数组并返回负。
	 * group_fini：幂等；释放本组自持数组（preferred 归 core，不在此动）。
	 * group_rescan_free：MEM 重算各片 free_count；CHUNK 为 NULL（enable 已实测）。*/
	int  (*group_setup)(struct rfs_bm_group *g, const struct rfs_bm_cfg *cfg);
	void (*group_fini)(struct rfs_bm_group *g);
	void (*group_rescan_free)(struct rfs_bm_group *g);

	/* ── 分配（片自然坐标；单窗语义在 CHUNK 内部）── */
	int  (*alloc_hard)(struct rfs_bm_group *g, u32 idx, u64 count,
			   u8 align_log2, u64 *out);
	int  (*alloc_soft)(struct rfs_bm_group *g, u32 idx, u64 count,
			   u8 align_log2, u64 *out, u64 *left);
	/* 锚定占领（片内前缀，永不越片）；rest 口径同 god */
	int  (*occupy_soft)(struct rfs_bm_group *g, u32 idx, u64 base,
			    u64 len, u64 *rest);

	/* ── 释放 / 测试（片自然坐标）── */
	int  (*free_interval)(struct rfs_bm_group *g, u32 idx, u64 base, u64 len);
	int  (*test_interval)(struct rfs_bm_group *g, u32 idx, bool val,
			      u64 base, u64 len);
	/* 注：成员名避开内核宏 `test_bit`（linux/bitops.h）——否则 `ops->test_bit(...)` 会被宏吞 */
	bool (*bit_test)(struct rfs_bm_group *g, u32 idx, u64 bit);

	/* ── 审计 ── */
	u64  (*free_count)(const struct rfs_bm_group *g, u32 idx);

	/* ── 持久化 ──
	 * snapshot_slice：MEM 锁内快照片位图字到 dst（bits/8 字节，类不碰盘）；
	 *   CHUNK 产出一份「整片一致视图」（读盘 + 覆盖在线窗 + 合并归还链）。
	 * slice_sync：CHUNK 整片周期同步（读-合并-写，类自走 io 回调）；MEM = NULL。*/
	void (*snapshot_slice)(const struct rfs_bm_group *g, u32 idx, void *dst);
	int  (*slice_sync)(struct rfs_bm_group *g, u32 idx);
};

/* 两后端的 ops 表（定义在各自 .c） */
extern const struct rfs_bm_ops rfs_bm_mem_ops;
extern const struct rfs_bm_ops rfs_bm_chunk_ops;

/* 按模式取 ops；未知模式返回 NULL（定义在 bitmap_core.c） */
const struct rfs_bm_ops *rfs_bm_ops_of(u8 mode);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_BITMAP_BACKEND_H */

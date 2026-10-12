/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_allocator_stats.h —— 运行时树分配器（rfs_allocator）计数器：**钩子 + 字段枚举**
 *
 * 背景（2026-10-04，D-11 连带；旧 `bm_` 组作废）：
 *   旧位图分配器（`struct rfs_bm`）整体退役，`bm_`（18 字段）**无任何埋点**（死组）。
 *   新分配器 = 多分片主从红黑树（`ravenfs_allocator_top.h` + `_slice.h`），**平台中立**、
 *   C(gnu11) 与 C++(c++17) 双编、**不得触碰内核**（无 `super_block`、无 `RFS_STAT_INC`）。
 *   故计数器经**注入了 `struct rfs_stat_hook`** 上报：算法模块只调钩子，宿主（FS 适配层）
 *   把 `(field, delta)` 累加进 `sbi->stats` 的 kernel-atomic64。
 *
 * 两条不变式（与全模块打点纪律 §7 同源）：
 *   P1 打点不改控制流：钩子只累加，返回值不参与分支；
 *   P2 只增不减、不存派生率：钩子只收 `delta ≥ 0` 的累计量。
 *
 * 字段命名（P6）：`al_<对象>_<动作/结果>`；枚举下标即 `ravenfs_stat_names[]` 中的 `al_`
 * 段下标（见 super.c），两处必须同序同数（BUILD_BUG_ON 守）。
 */
#ifndef _RAVENFS_ALLOCATOR_STATS_H
#define _RAVENFS_ALLOCATOR_STATS_H

#include "ravenfs_platform.h"	/* u32/u64（唯一平台收口） */

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════ 字段枚举（al_ 组）══════════════════════════
 * 分组：装配/生命周期 · block 顶层 · inode 顶层 · 片内双树/融合 · 路由 · checkpoint · 元素池 ·
 *       inode 数据面（H）。
 * **顺序即输出顺序**（super.c 名字表逐一对位）；增删须同改两处。
 */
enum rfs_al_stat {
	/* ── A 装配 / 生命周期（5）── */
	AL_INIT,		/* allocator_init 成功次数 */
	AL_INIT_FAIL,		/* allocator_init 失败次数 */
	AL_SLICES,		/* 装配出的片数（每次成功 init 累加 slice_count）*/
	AL_SHUTDOWN,		/* allocator_shutdown（关闸）次数 */
	AL_DRAIN_TIMEOUT,	/* 排空超时次数（fail-safe 拒绝释放）*/

	/* ── B block 组顶层（ops + 交付量；9）── */
	AL_BLK_ALLOC_HARD_OK,	/* block_alloc_hard 命中 */
	AL_BLK_ALLOC_HARD_FAIL,	/* block_alloc_hard 全片未命中（NO_HIT）*/
	AL_BLK_ALLOC_SOFT_OK,	/* block_alloc_soft 整体成功（凑满）*/
	AL_BLK_ALLOC_SOFT_FAIL,	/* block_alloc_soft 穷尽仍不足（all-or-nothing 回滚）*/
	AL_BLK_OCCUPY_OK,	/* block_occupy_soft 全满足 */
	AL_BLK_OCCUPY_PARTIAL,	/* block_occupy_soft 部分认领（前缀断）*/
	AL_BLK_OCCUPY_FAIL,	/* block_occupy_soft 一位未占 */
	AL_BLK_FREE_CALLS,	/* block_free 调用次数 */
	AL_BLK_FREE_BLOCKS,	/* block_free 释放的总块数（Σ count）*/

	/* ── C inode 组顶层（3）── */
	AL_INO_ALLOC_OK,	/* inode_alloc 命中 */
	AL_INO_ALLOC_FAIL,	/* inode_alloc 全片无空（ENOSPC）*/
	AL_INO_FREE_CALLS,	/* inode_free 调用次数 */

	/* ── D 片内双树 / 融合（诊断双树同步与碎片；8）── */
	AL_SLICE_ALLOC_HARD,	/* rfs_slice_alloc_hard 调用（=片级 best-fit 尝试）*/
	AL_SLICE_ALLOC_ENOSPC,	/* 片级硬分配未命中（满片）*/
	AL_SLICE_SOFT,		/* rfs_slice_soft_alloc 调用 */
	AL_SLICE_SOFT_ENOSPC,	/* 片级软分配一位未取（满片）*/
	AL_SLICE_FREE,		/* rfs_slice_free_interval 调用（含 free_intervals 逐段）*/
	AL_SLICE_FREE_EEXIST,	/* 释放区间与既有空闲区间重叠（-EEXIST 守卫命中）*/
	AL_TREE_INSERT,		/* 双挂插入次数（iv_link_both 成功）*/
	AL_TREE_ERASE,		/* 双摘删除次数（iv_erase_both）*/

	/* ── E 融合 / 碎片行为（3）── */
	AL_MERGE_LEFT,		/* 左邻接融合次数 */
	AL_MERGE_RIGHT,		/* 右邻接融合次数 */
	AL_MERGE_BOTH,		/* 左右同时融合（吞两邻）次数 */

	/* ── F 路由 / 跨片（3）── */
	AL_ROUTE_FIRST_HIT,	/* 首片（preferred）即命中：亲和性好 */
	AL_ROUTE_RESCAN,	/* 首选片未命中 → 换片重扫次数 */
	AL_ROUTE_CROSS_SLICE,	/* occupy 片尾跨界续圈次数 */

	/* ── G checkpoint / 元素池（5）── */
	AL_CHECKPOINT,		/* allocator_flush 快照成功次数 */
	AL_CHECKPOINT_FAIL,	/* 快照 / 落盘失败次数 */
	AL_CHECKPOINT_BLOCKS,	/* 写回的位图镜像块数 */
	AL_POOL_ALLOC,		/* 元素池取区间节点次数 */
	AL_POOL_FAIL,		/* 元素池取节点失败（-ENOMEM 路径）*/

	/* ── H 全局 inode 数据面（14；v2.0 内核）──
	 * plane 内 op（locate/free/grow/fr_merge）经 owner->stat 钩子累加；
	 * 盘面装载/落盘（扁 fr 快照 / 映射树 bulk）由 FS 适配层直接打点。*/
	AL_IDP_LOCATE_OK,	/* inode 数据面翻译命中 */
	AL_IDP_LOCATE_FAIL,	/* inode 数据面翻译未命中 / 非法 */
	AL_IDP_FREE,		/* inode 数据面释放一次 */
	AL_IDP_GROW,		/* inode 数据面增片次数 */
	AL_IDP_GROW_PIECES,	/* inode 数据面增片累计片数 */
	AL_IDP_FR_MERGE,	/* inode 数据面 fr 释放与邻接融合次数 */
	AL_IDP_TREE_DUMP,	/* inode 数据面翻译面 dump_extents 次数 */
	AL_IDP_TREE_BUILD,	/* 映射树 bulk build 次数 */
	AL_IDP_SNAP_WRITE,	/* 扁 fr 快照写次数 */
	AL_IDP_SNAP_READ,	/* 扁 fr 快照读次数 */
	AL_IDP_SNAP_BLOCKS,	/* 扁 fr 快照写累计块数 */
	AL_IDP_COMPACT_OK,	/* 挂载期压实 tree→direct 成功次数（挂载加速）*/
	AL_IDP_COMPACT_FAIL,	/* 挂载期压实无连续段回退（保持 tree）次数 */
	AL_IDP_COMPACT_PIECES,	/* 挂载期压实搬移的累计 8K 片数 */

	AL_STAT_COUNT		/* 哨兵：字段总数（勿作为计数器用）*/
};

/* ══════════════════════════ 注入钩子 ══════════════════════════
 * `inc(ctx, field, delta)`：把 field 计数器 +delta。**必须无阻塞、无 IO、可持片锁调用**
 * （片内埋点位于锁临界区）；实现侧只碰 atomic64。ctx = 宿主上下文（内核 = super_block*）。
 * 未注入（inc == NULL）→ 钩子 no-op（用户态压测 / 纯算法单测无需打点）。*/
struct rfs_stat_hook {
	void	(*inc)(void *ctx, u32 field, u64 delta);
	void	 *ctx;
};

/* 统一上报宏：未注入即 no-op；**不参与控制流**（P1）。*/
#define RFS_AL_STAT(h, field, delta)					\
	do {								\
		const struct rfs_stat_hook *__rfs_h = &(h);		\
		if (__rfs_h->inc)					\
			__rfs_h->inc(__rfs_h->ctx, (u32)(field),	\
				     (u64)(delta));			\
	} while (0)

#define RFS_AL_STAT1(h, field)	RFS_AL_STAT((h), (field), 1)

/* 空钩子（未注入）。*/
#define RFS_STAT_HOOK_NULL	((struct rfs_stat_hook){ NULL, NULL })

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_ALLOCATOR_STATS_H */

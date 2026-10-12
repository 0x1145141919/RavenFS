/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_inode_data_plane.h —— 全局 inode 数据面 struct rfs_inode_plane
 *                              （⇄ god `inode_data_plane_t`，fs_allocator.hpp/cpp）
 *
 * v2.0 范式（Docs/disk_layout_v2.0.md §12）：inode 存储 = 一段**弹性、逻辑紧密
 * 堆叠的 256B inode 数组**，物理以 **8K 片** 为单位从 block 分配器取块、经**映射
 * 面**（interval set，key = 逻辑 4K 块 lblk）定位；空闲用**扁 fr 快照**挂账（内存
 * 双红黑树），SB 四字段统辖。
 *
 * 两面 / 两锁（锁序硬）：
 *   翻译面 extent 集（key=lblk）   —— 翻译锁 extent_lock（rw_semaphore）
 *   空闲面 fr 主从红黑树（inode）  —— 空间锁 fr_lock（rw_semaphore）
 *   ★ 空间锁(fr) > 翻译锁(extent)；空间锁 > block 片锁；**禁止翻译→空间**。
 *   locate = 翻译读；alloc/free/test = 空间读写；grow = 空间写内再翻译写。
 *
 * 坐标（§12.1）：lblk = inum >> 4；片内偏移 = (inum & 31) × 256；
 *   1 片 = 32 inode = 8K = 2 块。extent 单位恒 4K 块，fblkbase/length 均 2 对齐。
 *
 * idx 0：契约级 root，**永不可分配**；init 拒绝含 idx0 的 fr，free(0) 忽略。
 *
 * 平台纪律：本头为 **ko 专用**（`struct rw_semaphore` / 内核块 I/O 语义）；由
 *   ravenfs_allocator_top.h 在 `__KERNEL__` 下 include。算法/几何纯内存。
 */
#ifndef _RAVENFS_INODE_DATA_PLANE_H
#define _RAVENFS_INODE_DATA_PLANE_H

#include "ravenfs_platform.h"		/* 类型 / 内存（唯一平台收口） */
#include "ravenfs_rbtree_intrusive.h"	/* 侵入式 RB 核（零分配） */
#include <linux/rwsem.h>		/* struct rw_semaphore */
#include <disk_struct_v2.0.h>		/* inode_dataplane_coordinate_free_range_t */

#ifdef __cplusplus
extern "C" {
#endif

struct rfs_allocator;			/* 前向：grow 借 block 组取片（本头不依赖 top.h） */

/* ── 翻译面一条 extent（单位 = 4K 块；lblk 偶、length 偶>0、phys_base 偶）──
 * ⇄ god `fs_inode_extent_t{lblk, phys_base, length}`。 */
struct rfs_inode_extent {
	u64	lblk;		/* 逻辑 4K 块起点（主序键） */
	u64	phys_base;	/* 物理 FS_block 起点（8K 片对齐） */
	u64	length;		/* 逻辑 4K 块数（偶） */
};

/* ── 无状态初始化配置（调用方组装；ctor 一次消费即弃，零 I/O）──
 * 读盘（SB + 映射 B+tree + 扁 fr 快照）归 FS 适配层；本模块只吃现成数组。 */
struct rfs_inode_plane_cfg {
	u64	pieces;			/* 8K 片数（= Σ extent.length / 2） */
	const struct rfs_inode_extent *extents;
	u64	extent_count;
	const struct inode_dataplane_coordinate_free_range_t *fr;
	u64	fr_count;		/* inode 单位 */
	u32	grow_batch_pieces;	/* 用尽增长批量（片）；0/1 = 单片 */
};

/* ── 空闲面区间（inode 单位；双挂主/从两棵侵入树）── */
struct rfs_ip_interval {
	struct rfs_rb_node	by_pos;	/* 主树：严格区间序（base 升序） */
	struct rfs_rb_node	by_size;/* 从树：(len,base) 序 */
	u64			base;
	u64			len;
};

/* ── 全局 inode 数据面（⇄ inode_data_plane_t）── */
struct rfs_inode_plane {
	bool			active;
	struct rfs_allocator	*owner;		/* grow：block_alloc_hard / block_free */
	u32			grow_batch;
	u64			pieces;

	/* 翻译面：有序动态数组 + 翻译锁（append-only；运行期从不改已有映射） */
	struct rw_semaphore	extent_lock;
	struct rfs_inode_extent	*ext;
	u64			ext_n;
	u64			ext_cap;

	/* 空闲面：主从双树 + 空间锁 */
	struct rw_semaphore	fr_lock;
	struct rfs_rb_root	fr_pos;
	struct rfs_rb_root	fr_size;
	u64			fr_free;
};

/* ══════════════════════════ 生命周期 ══════════════════════════ */

/* 装配：校验 cfg（extent 稠密无洞/2 对齐/Σ；fr 落 [0,pieces*32)、不含 idx0、不重叠）
 * + 建翻译数组 + fr 主从树。owner = 外层分配器（grow 用）。失败自清理返负 errno。*/
int	rfs_inode_plane_init(struct rfs_inode_plane *p,
			     const struct rfs_inode_plane_cfg *cfg,
			     struct rfs_allocator *owner);
/* 析构：拆 fr 双树 + 释放翻译数组。幂等。 */
void	rfs_inode_plane_fini(struct rfs_inode_plane *p);

/* ══════════════════════════ 翻译 / 分配 ══════════════════════════ */

/* inum → 物理 8K 片基（偶）+ 片内字节偏移。翻译锁读。0 / 负 -errno。 */
int	rfs_inode_plane_locate(struct rfs_inode_plane *p, u64 inum,
			       u64 *blkbase, u32 *blkoff);
/* 分配一个 inode（best-fit：最小空闲区间头）；用尽 → grow 增片。空间写。0 / -errno。 */
int	rfs_inode_plane_alloc(struct rfs_inode_plane *p, u64 *out_inum);
/* 释放一个 inode（自动融合；幂等；inum==0 忽略）。空间写。 */
void	rfs_inode_plane_free(struct rfs_inode_plane *p, u64 inum);
/* 是否已分配（在 [0,pieces*32) 且不在 fr）。空间读。 */
bool	rfs_inode_plane_test(struct rfs_inode_plane *p, u64 inum);

/* ══════════════════════════ 几何 / 快照 ══════════════════════════ */
u64	rfs_inode_plane_free_total(struct rfs_inode_plane *p);	/* 空闲 inode 数 */
u64	rfs_inode_plane_pieces(struct rfs_inode_plane *p);
u64	rfs_inode_plane_extent_count(struct rfs_inode_plane *p);
/* 翻译面全量（逻辑序）拷入 out（cap 不足 → -ENOSPC）。返回 0 / 负 errno。 */
int	rfs_inode_plane_dump_extents(struct rfs_inode_plane *p,
				     struct rfs_inode_extent *out, u64 cap);
u64	rfs_inode_plane_fr_count(struct rfs_inode_plane *p);
/* 空闲面全量（base 序）拷入 out（cap 不足 → -ENOSPC）。 */
int	rfs_inode_plane_dump_fr(struct rfs_inode_plane *p,
				struct inode_dataplane_coordinate_free_range_t *out,
				u64 cap);

/* 挂载期压实提交：把翻译面整体替换为单连续 direct 段 [0, pieces*2) → phys_base。
 * 逻辑空间/片数/fr 均不变（fr 坐标 = inode，与物理搬迁无关）。翻译写锁内一次性替换
 * （ext_cap 恒 ≥ 1；调用方保证数据已就位、旧树/旧片已由挂载压实流程处置）。幂等。 */
void	rfs_inode_plane_set_direct(struct rfs_inode_plane *p, u64 phys_base,
				   u64 pieces);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_INODE_DATA_PLANE_H */

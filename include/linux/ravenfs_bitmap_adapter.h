/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_bitmap_adapter.h —— 双位图分片分配器「ko 适配层」内部接口
 *                              (bitmap_adapter.c ⇄ super.c)
 *
 * 定位：`ravenfs_bitmap.h`（13 个公共 `ravenfs_*`，签名冻结）是**行为面**；
 *       本头是**挂载选项面**的补充：把「挂载参数锁定的后端选择」传进适配层、
 *       再从适配层回读（供 `show_options`）——两处共用同一 `struct ravenfs_bitmap_opts`，
 *       保证「回读集 ↔ 接受集」同构（D-9）。
 *
 * 为什么单开一头而不塞进 `ravenfs_bitmap.h`：`ravenfs_bitmap.h` 的 13 个签名受
 * 冻结红线保护，本头把「新增/可选」部分隔离出来，冻结面一字不动。
 *
 * ── 分层（勿混语义视角）──
 *   适配层内部持新核心类 `struct rfs_bm`（见 `ravenfs_bitmap_core.h`）：
 *     · MEM   ：内存即一等事实；flush = 锁内快照一片 → 锁外写（旧口径）。
 *     · CHUNK ：合成视角；flush = `rfs_bm_chunks_asyn`（合并后一致）。
 *   本头只描述**选项**，不含算法。算法/语义视角真值一律看 `ravenfs_bitmap_core.h`。
 *
 * 依赖：仅 `ravenfs_bitmap_core.h`（经 `ravenfs_platform.h` 收口平台差异），
 *       **不** include `ravenfs.h`（核心头）——使本头的纯校验函数可在用户态单测。
 *       `struct super_block` 用前向声明（调用方 super.c 已由 <linux/fs.h> 提供定义）。
 */
#ifndef _RAVENFS_BITMAP_ADAPTER_H
#define _RAVENFS_BITMAP_ADAPTER_H

#include "ravenfs_bitmap_core.h"	/* RFS_BM_MODE_* / u8 / u32 / bool（平台已收口）*/

#ifdef __cplusplus
extern "C" {
#endif

struct super_block;			/* 前向声明；定义属内核 <linux/fs.h> */

/* ═══════════════ 挂载参数：锁定值（传参 + 回读同构）═══════════════ */

/*
 * 一次挂载锁定的位图后端选择。字段即 `parse_param` 接受集，也是
 * `show_options` 回读集（同构，D-9 判据）。
 *   · mode            ：mem（0，默认）/ chunk（1）。
 *   · blk_chunk_order ：block 组 chunk 几何（仅 chunk 生效），0..6，默认 3；
 *                       回读用**短名** `bbco=`（全名 `block_bitmap_chunk_order=` 仅解析别名）。
 *   · ino_chunk_order ：inode 组 chunk 几何（仅 chunk 生效），0..6，默认 0；
 *                       回读用**短名** `ibco=`（全名 `inode_bitmap_chunk_order=` 仅解析别名）。
 *   · row_size        ：生效 row_size（0 → 默认 RAVENFS_ROW_DEFAULT；非默认才回读）。
 */
struct ravenfs_bitmap_opts {
	u8	mode;			/* enum rfs_bm_mode 取值 */
	u8	blk_chunk_order;	/* 0..6 */
	u8	ino_chunk_order;	/* 0..6 */
	u32	row_size;		/* 生效值（0 = 默认）*/
};

/* chunk order 值域 / 默认（与 super.c 的 parse_param 判定共用，单一真值）*/
#define RAVENFS_BM_CHUNK_ORDER_MIN		0u
#define RAVENFS_BM_CHUNK_ORDER_MAX		6u
#define RAVENFS_BM_DEFAULT_BLK_CHUNK_ORDER	3u
#define RAVENFS_BM_DEFAULT_INO_CHUNK_ORDER	0u

/* ═══════════════ 纯校验（user/kernel 双编 → 可单测）═══════════════ */

/* 字面量相等（不引 string.h，保持本头最小依赖）*/
static inline bool ravenfs_bm_str_eq_lit(const char *s, const char *lit)
{
	if (!s)
		return false;
	while (*lit) {
		if (*s != *lit)
			return false;
		s++;
		lit++;
	}
	return *s == '\0';
}

/* "mem" / "chunk" → mode；其余（含 NULL）→ -EINVAL（loud 拒挂）*/
static inline int ravenfs_bm_mode_from_str(const char *s, u8 *out)
{
	if (!out)
		return -EINVAL;
	if (ravenfs_bm_str_eq_lit(s, "mem")) {
		*out = RFS_BM_MODE_MEM;
		return 0;
	}
	if (ravenfs_bm_str_eq_lit(s, "chunk")) {
		*out = RFS_BM_MODE_CHUNK;
		return 0;
	}
	return -EINVAL;
}

/* chunk order 值域 0..6；越界 → -EINVAL。（v 为 unsigned：<0 不可能出现；
 * 用户写负数如 `bbco=-1` 在 kstrtouint 阶段即失败 → 同样 loud -EINVAL。）*/
static inline int ravenfs_bm_order_check(unsigned int v, u8 *out)
{
	if (!out)
		return -EINVAL;
	if (v > RAVENFS_BM_CHUNK_ORDER_MAX)
		return -EINVAL;
	*out = (u8)v;
	return 0;
}

/* ═══════════════ 适配层入口（ravenfs_bitmap.h 的行为面之外）═══════════════ */

/*
 * 按锁定选项装配位图管理器（= `ravenfs_bitmap_create` 的带选项版）。
 *   · opts == NULL → 全默认（mem，与旧行为一致）。
 *   · mode/orders 越界 → -EINVAL（不装配、不落 sbi->bitmap）。
 * 成功后 `sbi->bitmap` 持管理器（内部持 `struct rfs_bm` + 本选项副本，供回读）。
 */
int ravenfs_bitmap_create_mode(struct super_block *sb,
			       const struct ravenfs_bitmap_opts *opts);

/* 回读本次挂载锁定的选项（show_options 用）；无管理器 → -EINVAL */
int ravenfs_bitmap_opts_get(struct super_block *sb, struct ravenfs_bitmap_opts *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_BITMAP_ADAPTER_H */

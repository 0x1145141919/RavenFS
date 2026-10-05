/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_name_data_freeranges.h —— 目录名字数据面「空闲区间表」fr（v1.3）
 *
 * 定位：god 用户态原型 `juvenile_god/name_data_freeranges.cpp` 的 **C 语言逐接口
 *   语义翻译**（v1.3 契约）。fr 记录目录名字数据面逻辑空间 [0, pieces×8K) 内
 *   「已映射 ∧ 未分配」的字节区间，半开 [base, base+len)。
 *
 * 两态（同一 free_range_t 复用；键序 **len 主序 / base 副序** 升序）：
 *   · inline —— DIR inode 内 `possible_inline_free_ranges[9]`（≤9 条），
 *               `name_data_free_ranges_count` 记全域区间总数；
 *   · B+tree —— 节点族 Bptree_dir_name_data_free_ranges_{leaf,inner}_node_t，
 *               魔数 NAMEDATA_FREERANGES_MAGIC1..4；统一 337 階（+1 瞬时槽）。
 *               叶内复合键 lower_bound；内部找第一个 hint >= target（无则最右
 *               击穿）。hint = 子树最大复合键 (max_len, max_base)。
 *   第 10 条 insert 隐式升格 tree（建根叶，把 9 条就地把新条合并进去）。
 *
 * 平台纪律：只 include ravenfs_platform.h（唯一平台收口）+ disk_struct_v1.3.h
 *   + magics.h；**无** `#ifdef __KERNEL__`、无 `<linux/...>`、无 STL。
 *   I/O / 节点分配**全部经 ops vtable 注入**（零直接 I/O → 宿主可测）。
 *
 * 本轮范围：find_fit / insert / take_fit / retire_empty_leaf / release；
 *   不做相邻区间合并、欠载节点不合并（结构仍为合法搜索树）。
 *
 * 返回码：0 / 负 -errno（-EINVAL 状态、-EIO I/O、-ENOSPC 无块、-ENOENT 未命中）。
 */
#ifndef _RAVENFS_NAME_DATA_FREERANGES_H
#define _RAVENFS_NAME_DATA_FREERANGES_H

#include "ravenfs_platform.h"	/* 类型 u8..u64 / bool / errno（唯一平台收口）*/
/* v2.0 原子切换：v1.3/v2.0 盘头定义同名类型，不能同 TU 共存 → 名字数据面类型
 * （free_range_t / 节点族 / 控制字宏 / 魔数）逐字节同构地由 v2.0 头提供。 */
#include "disk_struct_v2.0.h"	/* free_range_t / 节点族 / 控制字宏 */
#include "magics.h"		/* NAMEDATA_FREERANGES_MAGIC1..4 */

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════ 哨兵 / 常量 ══════════════════════════ */

/* 节点块号无效哨兵（= PTR_INVALID 全 1）。node_alloc 失败返回它。*/
#define RFS_NDFR_NIL		(~0ULL)

/* 337 階 / 分裂左半 169（与 god name_data_freeranges.cpp 逐字一致）。*/
#define RFS_NDFR_MAX		337u
#define RFS_NDFR_LEFT		169u

/* ══════════════════════════ 注入 ops（零直接 I/O）══════════════════════════ */

/* 节点 I/O + 节点分配全外抛；模块内**绝不**直接 pin/alloc（宿主可测）。
 *   node_pin   —— 取回 blk 起 8K 节点缓冲；失败返 NULL（*err 置真）。
 *   node_unpin —— 归还节点；dirty=true 时写回（宿主自行落盘/缓存）。
 *   node_alloc —— 分配 2 个连续 8K 对齐 FS_block；失败返 RFS_NDFR_NIL。
 *   node_free  —— 归还一块 2-block 节点。
 * 纪律：同块禁双 pin；不同块可同时持有多枚 pin（宿主负责）。*/
struct rfs_ndfr_ops {
	void	*(*node_pin)(void *host, u64 blk, bool *err);
	void	 (*node_unpin)(void *host, u64 blk, void *node, bool dirty);
	u64	 (*node_alloc)(void *host, bool *err);
	int	 (*node_free)(void *host, u64 blk);
};

/* ══════════════════════════ 运行上下文 ══════════════════════════ */

/* host      —— 注入 ops 的宿主句柄；
 * dir_inode_idx —— 根节点 parent_ptr 指向的 DIR inode 编号（根收缩/新建根用）；
 * ops       —— 见上。*/
struct rfs_ndfr_ctx {
	const struct rfs_ndfr_ops	*ops;
	void				*host;
	u64				 dir_inode_idx;
};

/* ══════════════════════════ 寻路轨迹（⇄ god bptree_path_t）══════════════════════════
 * 模块不 include god 头，故在此自带同构定义（字段/次序与 god 逐字一致）。
 * child_idx 为 uint16_t —— 337 階下标可 >255，禁 uint8_t 截断（历史 bug）。*/
struct bptree_path_t {
	u64	blocks[64];	/* [depth] 各层块号（根在 [0]）*/
	u16	child_idx[64];	/* [depth] 各层选中的下标（叶 = 条目下标）*/
	int	depth;		/* 走过层数；叶所在 = depth-1 */
	bool	is_leaf_hit;	/* 叶内存在 ≥ target 的条目 */
};

/* ══════════════════════════ 节点基建 ══════════════════════════ */

/* 是否 fr 节点（四魔数全等）。*/
bool	rfs_ndfr_is_node(const struct Bptree_meta_t *m);

/* 初始化 8K 节点头：清零 → 四魔数 → count / is_inner / is_root / parent_ptr
 * → ptrs[3] 置无效。*/
void	rfs_ndfr_node_meta_init(void *buf, u16 count, bool is_inner, bool is_root,
				u64 parent_ptr);

/* 节点子树最大复合键：叶 = 末项 (len,base)；内部 = 末 hint 的 (max_len,max_base)；
 * 空节点 = (0,0)。*/
void	rfs_ndfr_max_key(const void *buf, u64 *out_len, u64 *out_base);

/* ══════════════════════════ 查询 / 变更 ══════════════════════════ */

/* 定位叶并做复合键 lower_bound。need_base 一般 0（best-fit 目标 (need,0)）；
 * 插入定位传真实 base。trace->is_leaf_hit = 叶内存在 ≥ target 的条目。*/
int	rfs_ndfr_walk(struct rfs_ndfr_ctx *ctx, u64 root, u64 need_len,
		      u64 need_base, struct bptree_path_t *trace);

/* best-fit（tree 态专用）：找最小 len ≥ need 的空闲区间；无 → -ENOENT。*/
int	rfs_ndfr_find_fit(struct rfs_ndfr_ctx *ctx, u64 root, u64 need,
			  u64 *out_base, u64 *out_len);

/* 插入一条空闲区间 [base, base+len)：空态建 inline；≤9 就地在留；
 * 第 10 条隐式升格 tree；tree 叶满则分裂级联（169/169，左半最大键提升）。*/
int	rfs_ndfr_insert(struct rfs_ndfr_ctx *ctx, struct raven_dir_Inode_t *dino,
			u64 base, u64 len);

/* best-fit 取用：摘除最小 len ≥ need 的区间；len>need 时余段原样插回。
 * 返回分配到的逻辑偏移 base；无 → -ENOENT。*/
int	rfs_ndfr_take_fit(struct rfs_ndfr_ctx *ctx, struct raven_dir_Inode_t *dino,
			  u64 need, u64 *out_base);

/* 空叶退役：父删 hint + 释放节点；父空继续上溯；根内部剩 1 孩收缩。
 * 不变式：树内**永不留下 count==0 的叶**（空叶 hint=(0,0) 破坏 hint 单调性）。*/
int	rfs_ndfr_retire_empty_leaf(struct rfs_ndfr_ctx *ctx,
				   struct raven_dir_Inode_t *dino, u64 leaf_blk);

/* 全量释放 fr（不碰数据片）：inline 清表复位；tree 归还全部节点。*/
int	rfs_ndfr_release(struct rfs_ndfr_ctx *ctx, struct raven_dir_Inode_t *dino);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_NAME_DATA_FREERANGES_H */

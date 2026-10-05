/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_dentry_node.h —— FS 底层原语接口（dentry_node.c 的导出面）
 * 三分拆分（2026-09-28）：底层机械对顶层（dentry_tree.c）+ 接待层 emit 的唯一通道。
 * rfs_dpath 住本头的依据：walk 产出、insert/delete 消费、balance_up（底层）复用 ——
 * 跨层共享类型按依赖方向下沉（tree.h include 本头即可见；禁止 node → tree 反向依赖）。
 */
#ifndef _RAVENFS_DENTRY_NODE_H
#define _RAVENFS_DENTRY_NODE_H

#include "ravenfs.h"

/* ── 多级 walk 的路径捕获（walk 产出 → insert/delete 消费 → balance_up 复用）── */
struct rfs_dpath {
	u32	depth;
	u64	blk[16];	/* 每层块号（叶在 depth-1） */
	u32	cidx[16];	/* 内层 = hint idx；叶层 = GLB（0xFFFF=NONE） */
	bool	hit;
	u32	ins_idx;
};

/* ── 树结构常量（原 dentry_write.c 文件级 define；跨 tree/node 两层 → 统一住此）── */
#define RFS_DENTRY_ORDER	126u
#define RFS_WALK_MAX_DEPTH	16u
#define GLB_NONE		0xFFFFu
#define RFS_PTR_INV_V		RFS_PTR_PACK(0, RFS_MASK52)
/* 叶欠载阈值：insert 侧（删除后借还）与 balance 侧共享的结构不变量（原 dentry_write.c） */
#define RFS_DENTRY_MIN		63u
/* 改写 parent_ptr 的 count 字段（保留 inner/root/ptr）；原 dentry_write.c，跨 tree/node 使用 */
#define PP_SET_COUNT(m, c) \
	do { (m)->parent_ptr.v = RFS_PP_PACK((u32)(c), RFS_PP_IS_INNER((m)->parent_ptr), \
					RFS_PP_IS_ROOT((m)->parent_ptr), RFS_PP_PTR((m)->parent_ptr)); } while (0)

/* ── 底层导出（三分拆分自 static；签名与设计注释随迁原样）── */
/* 物化 dentry 名字：≤40 内联；>40 从所属目录名字数据面读尾部（owner_dir_inum
 * 定位目录 inode；e->full_char_remain_offset 为数据面内**逻辑**偏移） */
int ravenfs_ddentry_name(struct super_block *sb, u64 owner_dir_inum,
			       const struct ddentry_t *e,
			       char *out, unsigned int *out_len);
int rfs_meta_ok(const struct Bptree_meta_t *m, u32 *cnt, u32 *inner, u32 *root, u64 *pptr);
/* 叶内 GLB：复合序 (hash,name) 的最大下界；无 → GLB_NONE；hit=精确命中 */
int rfs_leaf_glb(struct super_block *sb, u64 owner_dir_inum,
			const struct Bptree_ddentry_leaf_node_t *lf,
			u32 cnt, u64 hash, const char *name, u32 nlen, u32 *glb, bool *hit);
int ravenfs_leaf_lookup(struct super_block *sb, u64 owner_dir_inum,
			       const struct Bptree_ddentry_leaf_node_t *leaf,
			       u32 cnt, const char *name, u32 nlen, u64 hash, u64 *ino);
/* 自 child_level 起向上传播（child 子树 max 变为 cur_max）；父 max 未变则短路 */
int rfs_propagate(struct super_block *sb, const struct rfs_dpath *p, u32 child_level,
			 u64 cur_max);
/* 叶分裂：victim(count=127) → 左 63 / 右 64；维护叶链；返回 low/high max + 新叶块号
 * out_dotdot：旧根叶的 `..`（仅 was_root 有意义；升根时继承，非根 = RFS_MASK52） */
int rfs_leaf_split(struct super_block *sb, u64 victim_blk, u64 *out_new,
			  u64 *out_low, u64 *out_high, bool *out_was_root, u64 *out_dotdot);
/* 新建根内（根升长）；⭐ 显式写 ptrs[] + root ".." */
int rfs_new_root_inner(struct super_block *sb, u64 dir_ino, u64 left_blk, u64 left_max,
			      u64 right_blk, u64 right_max, u64 dotdot);
/* 父层应用（+ 内裂上溯）。cur_victim 是父层里被替换/插入的旧子块号 */
int rfs_apply_up(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p,
			u32 level, u32 child_idx, u64 cur_victim, u64 low_max,
			u64 new_blk, u64 high_max);
/* 欠载上溯：从叶的父层一路 balance；收尾统一尝试根收缩。返回 <0 错误 */
int rfs_balance_up(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p);

#endif /* _RAVENFS_DENTRY_NODE_H */

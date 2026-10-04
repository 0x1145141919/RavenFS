/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_rbtree_intrusive.h —— 平台中立 · 侵入式 · 零分配红黑树轮子（自造）
 *
 * 命名（名字即契约）：
 *   · **平台中立**：ko（内核）与用户态（差分 / 离线工具）**双编**，平台差异全部
 *     收口 ravenfs_platform.h —— 本头只 include 它，**不含任何 `#ifdef __KERNEL__`**。
 *   · **侵入式**：节点（struct rfs_rb_node）嵌在**调用方元素**里；同一元素可嵌
 *     **多个节点** → 同时挂多棵树（主/从双索引的基础）。
 *   · **零分配**：轮子内**不分配、不释放**任何内存，**且不提供任何分配接口**
 *     （无 rfs_rb_alloc / rfs_rb_alloc_node / rfs_rb_free_node）。元素内存的取/还
 *     全由**调用方**负责（自备池 / kmem_cache / 平台分配器，悉听尊便）。
 *     需要「容器拥有节点、insert 分配 / erase 释放」的语义，请用配套的
 *     owning 轮子（ravenfs_rbtree_owning.h，堆回调注入）。
 *
 * 为何**自造**：内核 `linux/rbtree.h` 是**侵入式 + 平台不可用**（只能在 ko 里编，
 *   用户态差分 / 离线工具用不了）。而运行时分配器接口层要的是「容器 / 挂载 +
 *   分配可注入」的可复用树（见 Docs/runtime_tech/04_sharded_master_slave_rbtree.md：
 *   主树 by_pos + 从树 by_size，同一区间双挂、从 ⊆ 主）。故手搓一个平台中立轮子，
 *   套本仓既有范式（同 ravenfs_run_list.h）：算法零内核依赖、平台差异收口
 *   ravenfs_platform.h。
 *
 * 设计要点（契约）：
 *   · **key 唯一**：insert 拒绝 cmp==0 的重复键（返回 -EEXIST，树不动），
 *     故中序严格递增 —— 与主从树「key = base / (len,base) 复合」的唯一性契约一致。
 *   · **比较器外抛**：cmp 由调用方给，内部用 container_of 取载荷比较；lookup 传
 *     「探针节点」（只填 key 字段即可，cmp 不得读 rb_node 链接字段）。
 *   · **零分配（无分配接口）**：轮子内零直接分配，也不包装分配；节点存储全由
 *     调用方管理（本头已**移除**曾经的 rfs_rb_alloc 回调接口）。
 *
 * 用法速览：
 *   struct my { u64 key; struct rfs_rb_node node; };
 *   static int my_cmp(const struct rfs_rb_node *a, const struct rfs_rb_node *b, void *ctx)
 *   {
 *       const struct my *x = container_of(a, struct my, node);
 *       const struct my *y = container_of(b, struct my, node);
 *       return (x->key < y->key) ? -1 : (x->key > y->key) ? 1 : 0;
 *   }
 *   struct rfs_rb_root t; rfs_rb_init(&t);
 *   struct my e;            （元素存储由调用方负责：栈 / 池 / 分配器皆可）
 *   e.key = 42;
 *   rfs_rb_insert(&t, &e.node, my_cmp, NULL);
 *   ...
 *   rfs_rb_erase(&t, &e.node);
 *
 * 平台纪律：本头**只** `#include "ravenfs_platform.h"`；**不得**出现
 *   `#ifdef __KERNEL__`、不得 include 任何内核头或额外 libc 头。C(gnu11) 与 C++
 *   (c++17) 双编均须通过（C++ 下 void* 需显式 cast，见下）。
 */
#ifndef _RAVENFS_RBTREE_INTRUSIVE_H
#define _RAVENFS_RBTREE_INTRUSIVE_H

#include "ravenfs_platform.h"	/* 类型 / errno（唯一平台收口） */

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════ 颜色 / 节点 / 树 ══════════════════════════ */

#define RFS_RB_RED	0u	/* 新插入节点恒为红 */
#define RFS_RB_BLACK	1u

/*
 * 侵入式节点：嵌在调用方元素里；同一元素可嵌多个 → 同挂多棵树。
 * 字段对调用方**不透明**（勿手改）；仅由本轮子维护。
 */
struct rfs_rb_node {
	struct rfs_rb_node	*left;
	struct rfs_rb_node	*right;
	struct rfs_rb_node	*parent;
	unsigned char		 color;	/* RFS_RB_RED / RFS_RB_BLACK */
};

/* 树根：仅持 `root`（最左/最右不缓存，靠 first/last 现算，保持结构最小）。 */
struct rfs_rb_root {
	struct rfs_rb_node	*root;	/* NULL = 空树 */
};

/*
 * 比较器：a<b 返负、a==b 返 0、a>b 返正。
 * 调用方经 container_of 取载荷比较；lookup 的 probe 只保证 key 字段有效。
 * ⚠ cmp **不得**读 a/b 的 left/right/parent/color（那是树内部字段）。
 */
typedef int (rfs_rb_cmp_fn)(const struct rfs_rb_node *a,
			    const struct rfs_rb_node *b, void *ctx);

/*
 * container_of：内核侧 `container_of` 已存在（本头只做兜底，勿重复定义）；
 * 用户态平台头未提供，此处补一份等价实现，使调用方两边通用。
 */
#ifndef container_of
#  define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* ══════════════════════════ 基本操作 ══════════════════════════ */

/* 初始化空树（等价于把 root 零初始化）。*/
static inline void rfs_rb_init(struct rfs_rb_root *t)
{
	t->root = NULL;
}

/* 空树判定。*/
static inline bool rfs_rb_empty(const struct rfs_rb_root *t)
{
	return t->root == NULL;
}

/*
 * 插入（**键唯一**）：node 为调用方元素内嵌的节点（其链接字段可未初始化，本函数会重置）。
 * 返回 0 成功；-EEXIST 表示 cmp==0 的等键节点已存在（**树不动、node 不入树**）。
 * 注意：本函数不分配内存，node 的存储由调用方负责（本头无分配接口）。
 */
int rfs_rb_insert(struct rfs_rb_root *t, struct rfs_rb_node *node,
		  rfs_rb_cmp_fn cmp, void *ctx);

/*
 * 删除（node 必须**确实在树内**且由同一 cmp 建树）。摘除后 node 的链接字段不保证有效。
 * 本函数不释放 node 存储（由调用方自行回收）。
 */
void rfs_rb_erase(struct rfs_rb_root *t, struct rfs_rb_node *node);

/* 最左 / 最右节点（即按 cmp 最小 / 最大）；空树返 NULL。*/
struct rfs_rb_node *rfs_rb_first(const struct rfs_rb_root *t);
struct rfs_rb_node *rfs_rb_last(const struct rfs_rb_root *t);

/* 中序后继 / 前驱；无则返 NULL（对 NULL 入参亦返 NULL，便于循环写法）。*/
struct rfs_rb_node *rfs_rb_next(struct rfs_rb_node *node);
struct rfs_rb_node *rfs_rb_prev(struct rfs_rb_node *node);

/* 首个 ≥ probe（lower_bound）/ 首个 > probe（upper_bound）的节点；无则 NULL。*/
struct rfs_rb_node *rfs_rb_lower_bound(const struct rfs_rb_root *t,
				       const struct rfs_rb_node *probe,
				       rfs_rb_cmp_fn cmp, void *ctx);
struct rfs_rb_node *rfs_rb_upper_bound(const struct rfs_rb_root *t,
				       const struct rfs_rb_node *probe,
				       rfs_rb_cmp_fn cmp, void *ctx);

/* ══════════════════════════ 不变量自检 ══════════════════════════ */

/*
 * 全树自检（O(n)）。校验：
 *   ① 父子指针自洽（含根 parent==NULL）；
 *   ② 红红不相邻（红节点的孩子不得为红）；
 *   ③ 每条根→叶路径黑高一致（含 NULL 叶，叶计 1）；
 *   ④ 中序严格递增（按 cmp，等键即视为破坏 —— 见「键唯一」契约）；
 *   ⑤ 颜色字段取值合法、根为黑。
 * 返回 0 通过，或负 -errno（-EINVAL）表示不变量被破坏。空树返 0。
 */
int rfs_rb_check(const struct rfs_rb_root *t, rfs_rb_cmp_fn cmp, void *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_RBTREE_INTRUSIVE_H */

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_run_list.h —— 分配器接口层「STL 式 run 链」简易轮子（自造）
 *
 * 为何**自造**：内核 list 家族（`list.h`/`llist.h`/`hlist.h`/`klist.h`/`list_nulls.h`）
 *   **全是侵入式**——节点嵌在业务对象里、链表**从不分配/释放**（已核实：这些头
 *   `kmalloc/kzalloc/kfree` 命中为 0）。而本层（分配器接口）契约要的是 **STL 式**：
 *   **容器拥有节点、push 即分配、析构即全释放** —— 内核无对等物，故自造。
 *
 * 分配绑定（⇄ STL allocator）：容器持 `struct rfs_run_alloc{alloc,free,ctx}`，
 *   节点的取/还全走它。宿主可绑 `kmem_cache`（推荐的 ko 形态）/ 固定池 / 平台默认。
 *   ⚠ 默认分配器用平台 `RFS_ZALLOC/RFS_FREE`（内核 = GFP_KERNEL）；**若容器在片锁
 *   临界区内取节点，宿主应改绑 GFP_NOFS 的 kmem_cache 分配器**（见 allocator_top 锁注）。
 *
 * 三个类型：
 *   struct rfs_run         ⇄ `fs_run_t`（**纯 payload**，无 next）
 *   struct rfs_run_list    ⇄ `fs_run_list_t` （前插单链；软分配产出 / 释放消费）
 *   struct rfs_run_dlist   ⇄ `fs_run_dlist_t`（尾插；仅 `all_dump` 出参）
 *
 * 生命周期（⇄ STL）：**对象 + 节点全归调用方**——init 即绑定分配器、fini 即释放全部
 *   节点；类只对已 init 的容器做 push、只对调用方链做 consume。`init` 后未 fini 会漏
 *   节点；`fini` 后容器需重新 `init` 才能再用。
 */
#ifndef _RAVENFS_RUN_LIST_H
#define _RAVENFS_RUN_LIST_H

#include "ravenfs_platform.h"	/* 类型 / 内存（唯一平台收口） */

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════ payload / 节点 / 分配器 ══════════════════════════ */

/* 运行时物理区段（⇄ `fs_run_t`）。分配器只懂「物理块世界」：base = 全局位号起点、
 * len = 段长（块数）；**不携带文件逻辑块**（逻辑↔物理归属由 pwrite 管线拿到 run 后决定）。*/
struct rfs_run {
	u64	base;		/* 全局位号起点 */
	u64	len;		/* 段长（块数 / 位数） */
};

/* 绑定分配器（⇄ STL allocator）：node 取/还全经此。ctx 自定（内核传 kmem_cache*）。*/
struct rfs_run_alloc {
	void	*(*alloc)(void *ctx, u64 size);	/* 取节点内存；失败返 NULL */
	void	 (*free)(void *ctx, void *p);	/* 还节点内存 */
	void	 *ctx;				/* 宿主上下文（可 NULL） */
};

/* 链节点（容器自持、自分配）。payload 内嵌，与 god `std::forward_list<fs_run_t>` 的
 * 「节点 = STL 内部节点、payload 独立」同构——故 `rfs_run` 保持纯 payload。*/
struct rfs_run_node {
	struct rfs_run		run;
	struct rfs_run_node	*next;
};

/* ── 前插单链（⇄ `fs_run_list_t`）──
 * 用法（软分配产出 / 释放消费）：`rfs_run_list_init` 绑定分配器 → 类内 push_front 构建
 * → 调用方遍历 / 类内 consume 逐节点取走（取走即还节点，链回空、可复用）→ `fini`。*/
struct rfs_run_list {
	struct rfs_run_node	*head;	/* NULL = 空链 */
	struct rfs_run_alloc	 alloc;	/* 绑定的分配器 */
};

/* ── 尾插链（⇄ `fs_run_dlist_t`，仅 `all_dump` 出参）──
 * 与单链分开命名：交付（软分配多段）与快照（全量区间）是两种用途，共用一个别名迟早误用。*/
struct rfs_run_dlist {
	struct rfs_run_node	*head;
	struct rfs_run_node	*tail;
	struct rfs_run_alloc	 alloc;
};

/* ══════════════════════════ 默认分配器（平台） ══════════════════════════ */

static inline void *rfs_run_alloc_platform_alloc(void *ctx, u64 size)
{
	(void)ctx;
	return RFS_ZALLOC(size);
}
static inline void rfs_run_alloc_platform_free(void *ctx, void *p)
{
	(void)ctx;
	RFS_FREE(p);
}
/* 平台默认分配器（内核 = GFP_KERNEL；片锁内取节点的宿主请**改绑 kmem_cache/GFP_NOFS**）。*/
static inline struct rfs_run_alloc rfs_run_alloc_platform(void)
{
	struct rfs_run_alloc a;
	a.alloc = rfs_run_alloc_platform_alloc;
	a.free  = rfs_run_alloc_platform_free;
	a.ctx   = NULL;
	return a;
}

/* ══════════════════════════ 前插单链 ops ══════════════════════════ */

static inline void rfs_run_list_init(struct rfs_run_list *l, struct rfs_run_alloc a)
{
	l->head = NULL;
	l->alloc = a;
}

static inline bool rfs_run_list_empty(const struct rfs_run_list *l)
{
	return l->head == NULL;
}

/* 前插一个 run；分配失败 → -ENOMEM（链不动）。⇄ `forward_list::push_front`。*/
static inline int rfs_run_list_push_front(struct rfs_run_list *l, u64 base, u64 len)
{
	/* 显式 cast：本头被 C++ 包含时 void*→T* 不隐式转（C 才隐式）。*/
	struct rfs_run_node *n =
		(struct rfs_run_node *)l->alloc.alloc(l->alloc.ctx, sizeof(*n));

	if (!n)
		return -ENOMEM;
	n->run.base = base;
	n->run.len  = len;
	n->next     = l->head;
	l->head     = n;
	return 0;
}

/* 首元素（只读）；空链返 NULL。*/
static inline const struct rfs_run *rfs_run_list_first(const struct rfs_run_list *l)
{
	return l->head ? &l->head->run : NULL;
}

/* 消费（drain）：逐个取走节点（先摘链 → 回调（可 NULL）→ 还节点）直至链空。
 * ⇄ god `free_intervals` 的「逐段取走节点」语义：**取走即还节点**，容器对象仍归调用方、
 * 消费后链为空可复用。回调 ctx 供消费方携带（如 slice 指针 + 备注账）。*/
static inline void rfs_run_list_consume(struct rfs_run_list *l,
					void (*cb)(void *ctx, const struct rfs_run *run),
					void *ctx)
{
	struct rfs_run_node *n = l->head;

	l->head = NULL;
	while (n) {
		struct rfs_run_node *next = n->next;

		if (cb)
			cb(ctx, &n->run);
		l->alloc.free(l->alloc.ctx, n);
		n = next;
	}
}

/* 析构：释放全部节点（⇄ STL 析构）。之后容器需重新 init 才能再用。*/
static inline void rfs_run_list_fini(struct rfs_run_list *l)
{
	rfs_run_list_consume(l, NULL, NULL);
}

/* ══════════════════════════ 尾插链 ops ══════════════════════════ */

static inline void rfs_run_dlist_init(struct rfs_run_dlist *l, struct rfs_run_alloc a)
{
	l->head = NULL;
	l->tail = NULL;
	l->alloc = a;
}

static inline bool rfs_run_dlist_empty(const struct rfs_run_dlist *l)
{
	return l->head == NULL;
}

/* 尾插一个 run（O(1)）；分配失败 → -ENOMEM（链不动）。⇄ `list::push_back`。*/
static inline int rfs_run_dlist_push_back(struct rfs_run_dlist *l, u64 base, u64 len)
{
	/* 显式 cast：同 push_front（C++ 包含兼容）。*/
	struct rfs_run_node *n =
		(struct rfs_run_node *)l->alloc.alloc(l->alloc.ctx, sizeof(*n));

	if (!n)
		return -ENOMEM;
	n->run.base = base;
	n->run.len  = len;
	n->next     = NULL;
	if (l->tail)
		l->tail->next = n;
	else
		l->head = n;
	l->tail = n;
	return 0;
}

/* 析构：释放全部节点。*/
static inline void rfs_run_dlist_fini(struct rfs_run_dlist *l)
{
	struct rfs_run_node *n = l->head;

	l->head = NULL;
	l->tail = NULL;
	while (n) {
		struct rfs_run_node *next = n->next;

		l->alloc.free(l->alloc.ctx, n);
		n = next;
	}
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_RUN_LIST_H */

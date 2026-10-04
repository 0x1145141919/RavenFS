/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_rbtree_owning.h —— 平台中立 · STL 式 owning 红黑树轮子（自造）
 *
 * 定位：与 ravenfs_rbtree_intrusive.h（侵入式 · 零分配 · 无分配接口）配套的一支。
 *   本支 **owning**：容器**拥有并管理节点** —— insert 时经堆回调分配节点块、把
 *   调用方元素**拷贝**进去；erase / fini 时释放节点块。这是它跟 intrusive 的
 *   本质区别（intrusive：元素内存全由调用方管，轮子零分配）。
 *
 * 实现策略：**复用 intrusive 核**。owning 节点 = `struct rfs_rb_node`（头部）+
 *   定长载荷；owning 容器 = `struct rfs_rb_root`（复用其全部 rebalance / 导航
 *   算法）+ 分配封装（堆回调 / elem_size / 比较器 / size 记账）。故本仓只有**一份**
 *   旋转 / 双黑修复实现，不会退化成两份 rebalance。本支**不改** intrusive 头/源的
 *   契约（仅 include 复用其符号）。
 *
 * 核心契约：
 *   · **堆回调必填**：struct rfs_rbo_alloc { alloc / free / ctx }；轮子内**零直接
 *     分配**（不出现 kmalloc/malloc 字面）—— 一切节点取/还全走回调。宿主可绑
 *     kmem_cache（ko）/ malloc 包装（用户态）/ 计数池（测试）。
 *   · **定长载荷**：init 时定下 elem_size；insert **拷贝**调用方元素进节点，
 *     返回给 find / first / next 的 `const void *` 指向树内副本（调用方只读）。
 *   · **比较器比元素**：typedef int (rfs_rbo_cmp_fn)(const void *elem_a,
 *     const void *elem_b, void *ctx)；比的是**元素**（载荷），不是节点。
 *   · **key 唯一**：insert 遇 cmp==0 返 -EEXIST、树不动（新块随即释放）。
 *   · **平台中立**：只依赖 ravenfs_platform.h 与 intrusive 头（二者皆平台中立，
 *     不含 `#ifdef __KERNEL__`）；C(gnu11) + C++(c++17) 双编、`extern "C"` 守卫。
 *
 * 用法速览：
 *   struct kv { u64 key; u64 val; };
 *   static int kv_cmp(const void *a, const void *b, void *ctx) {
 *       const struct kv *x = a, *y = b;
 *       return (x->key < y->key) ? -1 : (x->key > y->key) ? 1 : 0;
 *   }
 *   struct rfs_rbo_root t;
 *   struct rfs_rbo_alloc a = { my_alloc, my_free, NULL };
 *   rfs_rbo_init(&t, a, sizeof(struct kv), kv_cmp, NULL);
 *   struct kv e = { 42, 7 };
 *   rfs_rbo_insert(&t, &e);                 // 拷贝 e
 *   const struct kv *got = rfs_rbo_find(&t, &e);
 *   struct kv probe = { 42, 0 };
 *   rfs_rbo_erase(&t, &probe);
 *   rfs_rbo_fini(&t);                       // 释放全部节点
 */
#ifndef _RAVENFS_RBTREE_OWNING_H
#define _RAVENFS_RBTREE_OWNING_H

#include "ravenfs_platform.h"		/* 类型 / errno（唯一平台收口） */
#include "ravenfs_rbtree_intrusive.h"	/* 复用侵入式核：节点 / 根 / 算法 */

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════ 堆回调（必填） ══════════════════════════ */

/*
 * 绑定堆：轮子**从不**直接分配；凡取/还「节点块」全经此。
 * ctx 自定（内核传 kmem_cache* / 内存池对象；用户态可传计数器 / NULL）。
 * 语义同 malloc/free：alloc 失败返 NULL（insert 侧转 -ENOMEM）；free 收 alloc 出的指针。
 */
struct rfs_rbo_alloc {
	void	*(*alloc)(void *ctx, u64 size);	/* 取节点块；失败返 NULL */
	void	 (*free)(void *ctx, void *p);	/* 还节点块 */
	void	 *ctx;				/* 宿主上下文（可 NULL） */
};

/* 比较器：比**元素**（载荷），非节点。a<b 返负、a==b 返 0、a>b 返正。 */
typedef int (rfs_rbo_cmp_fn)(const void *elem_a, const void *elem_b, void *ctx);

/* ══════════════════════════ 容器 ══════════════════════════ */

/*
 * owning 树根：内嵌一份 intrusive 树（复用其算法），外加拥有语义所需的元数据。
 * 字段对调用方**不透明**（勿手改）；经 rfs_rbo_* 操作。
 */
struct rfs_rbo_root {
	struct rfs_rb_root	 rb;		/* 复用侵入式核（rebalance / 导航）*/
	struct rfs_rbo_alloc	 alloc;		/* 节点块的唯一来源 */
	rfs_rbo_cmp_fn		*cmp;		/* 比元素 */
	void			*cmp_ctx;	/* 透传给 cmp */
	u64			 elem_size;	/* 定长载荷（init 时定死）*/
	u64			 size;		/* 节点数（owning 自记账）*/
};

/* ══════════════════════════ 生命周期 ══════════════════════════ */

/*
 * 初始化：绑定堆回调、载荷尺寸、比较器。返回 0；参数非法（t/cmp 为 NULL、
 * elem_size==0、alloc/free 为 NULL、elem_size 导致块大小溢出）返 -EINVAL。
 * 初始化不分配任何内存（空树）。
 */
int  rfs_rbo_init(struct rfs_rbo_root *t, struct rfs_rbo_alloc a,
		  u64 elem_size, rfs_rbo_cmp_fn cmp, void *cmp_ctx);

/* 释放**全部**节点（遍历后逐个经 alloc.free 归还），树回到空；不释放 t 自身。*/
void rfs_rbo_fini(struct rfs_rbo_root *t);

/* ══════════════════════════ 增删查 ══════════════════════════ */

/*
 * 插入：分配节点块、**拷贝** *elem 进节点。返回 0 成功；-EEXIST（cmp==0 已存在，
 * 新块已释放、树不动）；-ENOMEM（堆回调返 NULL）；-EINVAL（参数非法）。
 */
int  rfs_rbo_insert(struct rfs_rbo_root *t, const void *elem);

/* 查找：返回树内元素只读指针（命中）或 NULL（未命中）。不分配。*/
const void *rfs_rbo_find(const struct rfs_rbo_root *t, const void *probe);

/*
 * 删除：按 probe 定位并摘除（比较元素）。返回 0；-ENOENT（未命中，树不动）；
 * -EINVAL（参数非法）。命中时释放该节点块。
 */
int  rfs_rbo_erase(struct rfs_rbo_root *t, const void *probe);

/* ══════════════════════════ 迭代 / 计数 ══════════════════════════ */

/* 最小元素（按 cmp）；空树返 NULL。*/
const void *rfs_rbo_first(const struct rfs_rbo_root *t);

/* 元素的中序后继；传 NULL 或末尾返 NULL（便于循环写法）。*/
const void *rfs_rbo_next(const void *elem);

/* 节点数。*/
u64  rfs_rbo_size(const struct rfs_rbo_root *t);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_RBTREE_OWNING_H */

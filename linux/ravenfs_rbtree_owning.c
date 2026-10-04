// SPDX-License-Identifier: GPL-2.0
/*
 * ravenfs_rbtree_owning.c —— 平台中立 · STL 式 owning 红黑树轮子（自造）· 实现
 *
 * 与 ravenfs_rbtree_owning.h 配套。实现策略 = **复用 intrusive 核**：
 *   本文件只做「分配封装 + 元素拷贝 + 元素比较适配 + size 记账」，所有旋转 /
 *   双黑修复 / 导航一律调 ravenfs_rbtree_intrusive.c 的 rfs_rb_*（不改其契约）。
 *
 * 节点布局（单块）：[ struct rfs_rb_node | 载荷 elem_size 字节 ]，rb 为首成员，
 *   block 首址 == 节点地址；载荷偏移 RFS_RBO_NODE_OFFSET 向上取 16 的对齐，
 *   保证常见标量载荷（≤16B 对齐）落在对齐地址上。块由堆回调分配（应返回最大
 *   对齐，如 kmalloc / malloc），轮子内**零直接分配**（无 kmalloc/malloc 字面）。
 *
 * 本文件 C(gnu11) 与 C++(c++17) 双编均通过（void*→T* 显式 cast、不用纯 C 扩展）。
 */
#include "ravenfs_rbtree_owning.h"

/* 载荷相对节点首址的字节偏移（16 对齐，恒 ≥ sizeof(struct rfs_rb_node)）。*/
#define RFS_RBO_NODE_OFFSET \
	(((u64)sizeof(struct rfs_rb_node) + 15u) & ~(u64)15u)

/* 编译期自检：偏移不得小于节点本身。*/
typedef char rfs_rbo_offset_sanity
	[(RFS_RBO_NODE_OFFSET >= (u64)sizeof(struct rfs_rb_node)) ? 1 : -1];

/* ══════════════════════════ 内部工具 ══════════════════════════ */

/* 节点的可变 / 只读载荷指针。*/
static void *rbo_elem_mut(struct rfs_rb_node *n)
{
	return (void *)((char *)n + RFS_RBO_NODE_OFFSET);
}

static const void *rbo_elem_const(const struct rfs_rb_node *n)
{
	return (const void *)((const char *)n + RFS_RBO_NODE_OFFSET);
}

/* 元素指针 → 其所属节点首址（与 rbo_elem_* 互逆）。*/
static struct rfs_rb_node *rbo_node_of(const void *elem)
{
	return (struct rfs_rb_node *)((char *)elem - RFS_RBO_NODE_OFFSET);
}

/* 逐字节拷贝（避免依赖具体 libc/内核 memcpy 头；定长且极小）。*/
static void rbo_copy(void *dst, const void *src, u64 n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	u64 i;

	for (i = 0; i < n; i++)
		d[i] = s[i];
}

/* 适配 intrusive 比较器：节点 → 载荷 → 用户 cmp。ctx = owning 根。*/
static int rbo_cmp_node(const struct rfs_rb_node *a,
			const struct rfs_rb_node *b, void *ctx)
{
	const struct rfs_rbo_root *t = (const struct rfs_rbo_root *)ctx;

	return t->cmp(rbo_elem_const(a), rbo_elem_const(b), t->cmp_ctx);
}

/* ══════════════════════════ 生命周期 ══════════════════════════ */

int rfs_rbo_init(struct rfs_rbo_root *t, struct rfs_rbo_alloc a,
		 u64 elem_size, rfs_rbo_cmp_fn cmp, void *cmp_ctx)
{
	if (!t || !cmp || !a.alloc || !a.free || elem_size == 0)
		return -EINVAL;
	if (elem_size > (u64)-1 - RFS_RBO_NODE_OFFSET)	/* 块大小溢出 */
		return -EINVAL;

	rfs_rb_init(&t->rb);
	t->alloc = a;
	t->cmp = cmp;
	t->cmp_ctx = cmp_ctx;
	t->elem_size = elem_size;
	t->size = 0;
	return 0;
}

/* 后序释放子树：先孩子后自身（释放后不再触碰 n）。深度受红黑树高约束（O(log n)）。*/
static void rbo_free_subtree(struct rfs_rbo_root *t, struct rfs_rb_node *n)
{
	if (!n)
		return;
	rbo_free_subtree(t, n->left);
	rbo_free_subtree(t, n->right);
	t->alloc.free(t->alloc.ctx, n);	/* n 即块首址（rb 为首成员）*/
}

void rfs_rbo_fini(struct rfs_rbo_root *t)
{
	if (!t)
		return;
	rbo_free_subtree(t, t->rb.root);
	rfs_rb_init(&t->rb);
	t->size = 0;
}

/* ══════════════════════════ 增删查 ══════════════════════════ */

int rfs_rbo_insert(struct rfs_rbo_root *t, const void *elem)
{
	struct rfs_rb_node *n;
	void *dst;
	int rc;

	if (!t || !elem)
		return -EINVAL;

	n = (struct rfs_rb_node *)t->alloc.alloc(
		t->alloc.ctx, RFS_RBO_NODE_OFFSET + t->elem_size);
	if (!n)
		return -ENOMEM;

	dst = rbo_elem_mut(n);
	rbo_copy(dst, elem, t->elem_size);

	rc = rfs_rb_insert(&t->rb, n, rbo_cmp_node, t);
	if (rc != 0) {			/* -EEXIST：等键拒绝，树不动 */
		t->alloc.free(t->alloc.ctx, n);
		return rc;
	}
	t->size++;
	return 0;
}

const void *rfs_rbo_find(const struct rfs_rbo_root *t, const void *probe)
{
	const struct rfs_rb_node *n;

	if (!t || !probe)
		return NULL;

	n = t->rb.root;
	while (n) {
		int c = t->cmp(probe, rbo_elem_const(n), t->cmp_ctx);

		if (c < 0)
			n = n->left;
		else if (c > 0)
			n = n->right;
		else
			return rbo_elem_const(n);
	}
	return NULL;
}

int rfs_rbo_erase(struct rfs_rbo_root *t, const void *probe)
{
	struct rfs_rb_node *n;

	if (!t || !probe)
		return -EINVAL;

	n = t->rb.root;
	while (n) {
		int c = t->cmp(probe, rbo_elem_const(n), t->cmp_ctx);

		if (c < 0)
			n = n->left;
		else if (c > 0)
			n = n->right;
		else
			break;
	}
	if (!n)				/* 未命中 */
		return -ENOENT;

	rfs_rb_erase(&t->rb, n);	/* 摘除（不改动本块）*/
	t->alloc.free(t->alloc.ctx, n);
	t->size--;
	return 0;
}

/* ══════════════════════════ 迭代 / 计数 ══════════════════════════ */

const void *rfs_rbo_first(const struct rfs_rbo_root *t)
{
	const struct rfs_rb_node *n;

	if (!t)
		return NULL;
	n = rfs_rb_first(&t->rb);
	return n ? rbo_elem_const(n) : NULL;
}

const void *rfs_rbo_next(const void *elem)
{
	struct rfs_rb_node *n;
	struct rfs_rb_node *nx;

	if (!elem)
		return NULL;
	n = rbo_node_of(elem);
	nx = rfs_rb_next(n);
	return nx ? rbo_elem_const(nx) : NULL;
}

u64 rfs_rbo_size(const struct rfs_rbo_root *t)
{
	return t ? t->size : 0;
}

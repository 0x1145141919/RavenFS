// SPDX-License-Identifier: GPL-2.0
/*
 * ravenfs_rbtree_intrusive.c —— 平台中立 · 侵入式 · 零分配红黑树轮子（自造）· 实现
 *
 * 与 ravenfs_rbtree_intrusive.h 配套：算法零内核依赖、零直接分配、**无分配接口**；
 * 平台差异（类型 / errno）
 * 全部经 ravenfs_platform.h 收口。**本文件 C(gnu11) 与 C++(c++17) 双编均通过**
 * （故所有 void*→T* 显式 cast、不用纯 C 扩展构造）。
 *
 * 算法取自经典 CLRS 红黑树（insert / delete + 双黑修复），以 parent 指针替代
 * 内核的 parent_color 打包：结构更直白，代价 = 节点多一个指针字段。
 * 不变量与自检语义详见头文件。
 */
#include "ravenfs_rbtree_intrusive.h"

/* ══════════════════════════ 旋转 ══════════════════════════ */

/* 以 node 为轴左旋（node 需有右孩子）。*/
static void rb_rotate_left(struct rfs_rb_root *t, struct rfs_rb_node *node)
{
	struct rfs_rb_node *right = node->right;
	struct rfs_rb_node *parent = node->parent;

	node->right = right->left;
	if (right->left)
		right->left->parent = node;
	right->parent = parent;
	if (!parent)
		t->root = right;
	else if (node == parent->left)
		parent->left = right;
	else
		parent->right = right;
	right->left = node;
	node->parent = right;
}

/* 以 node 为轴右旋（node 需有左孩子）。*/
static void rb_rotate_right(struct rfs_rb_root *t, struct rfs_rb_node *node)
{
	struct rfs_rb_node *left = node->left;
	struct rfs_rb_node *parent = node->parent;

	node->left = left->right;
	if (left->right)
		left->right->parent = node;
	left->parent = parent;
	if (!parent)
		t->root = left;
	else if (node == parent->right)
		parent->right = left;
	else
		parent->left = left;
	left->right = node;
	node->parent = left;
}

/* ══════════════════════════ 插入 ══════════════════════════ */

/* 插入后修复：自 node 上溯，处理红红冲突。*/
static void rb_insert_fixup(struct rfs_rb_root *t, struct rfs_rb_node *node)
{
	struct rfs_rb_node *parent, *gparent, *uncle;

	while ((parent = node->parent) && parent->color == RFS_RB_RED) {
		/* 父为红 → 父必非根 → 祖父存在。*/
		gparent = parent->parent;
		if (parent == gparent->left) {
			uncle = gparent->right;
			if (uncle && uncle->color == RFS_RB_RED) {	/* case 1 */
				parent->color = RFS_RB_BLACK;
				uncle->color = RFS_RB_BLACK;
				gparent->color = RFS_RB_RED;
				node = gparent;
				continue;
			}
			if (node == parent->right) {			/* case 2 */
				rb_rotate_left(t, parent);
				node = parent;
				parent = node->parent;
			}
			parent->color = RFS_RB_BLACK;			/* case 3 */
			gparent->color = RFS_RB_RED;
			rb_rotate_right(t, gparent);
		} else {						/* 镜像 */
			uncle = gparent->left;
			if (uncle && uncle->color == RFS_RB_RED) {
				parent->color = RFS_RB_BLACK;
				uncle->color = RFS_RB_BLACK;
				gparent->color = RFS_RB_RED;
				node = gparent;
				continue;
			}
			if (node == parent->left) {
				rb_rotate_right(t, parent);
				node = parent;
				parent = node->parent;
			}
			parent->color = RFS_RB_BLACK;
			gparent->color = RFS_RB_RED;
			rb_rotate_left(t, gparent);
		}
	}
	t->root->color = RFS_RB_BLACK;	/* 根恒黑 */
}

int rfs_rb_insert(struct rfs_rb_root *t, struct rfs_rb_node *node,
		  rfs_rb_cmp_fn cmp, void *ctx)
{
	struct rfs_rb_node **link = &t->root;
	struct rfs_rb_node *parent = NULL;

	while (*link) {
		int c;

		parent = *link;
		c = cmp(node, parent, ctx);
		if (c < 0)
			link = &parent->left;
		else if (c > 0)
			link = &parent->right;
		else
			return -EEXIST;	/* 键唯一：等键拒绝，树不动 */
	}
	node->parent = parent;
	node->left = NULL;
	node->right = NULL;
	node->color = RFS_RB_RED;
	*link = node;
	rb_insert_fixup(t, node);
	return 0;
}

/* ══════════════════════════ 删除 ══════════════════════════ */

/* 用 v（可 NULL）顶替 u 在树中的位置。*/
static void rb_transplant(struct rfs_rb_root *t,
			  struct rfs_rb_node *u, struct rfs_rb_node *v)
{
	if (!u->parent)
		t->root = v;
	else if (u == u->parent->left)
		u->parent->left = v;
	else
		u->parent->right = v;
	if (v)
		v->parent = u->parent;
}

/*
 * 双黑修复：x 处少一层黑（x 可 NULL），parent = x 的父（x 为 NULL 时仍有效）。
 * 与 CLRS RB-DELETE-FIXUP 同构；对合法红黑树 sibling 必存在，w==NULL 分支仅作防御。
 */
static void rb_erase_fixup(struct rfs_rb_root *t,
			   struct rfs_rb_node *x, struct rfs_rb_node *parent)
{
	while (x != t->root && (!x || x->color == RFS_RB_BLACK)) {
		if (parent->left == x) {			/* x 是左孩子 */
			struct rfs_rb_node *w = parent->right;

			if (!w) {	/* 防御：非法树才可能 */
				x = parent;
				parent = x->parent;
				continue;
			}
			if (w->color == RFS_RB_RED) {		/* case 1 */
				w->color = RFS_RB_BLACK;
				parent->color = RFS_RB_RED;
				rb_rotate_left(t, parent);
				w = parent->right;
			}
			if ((!w->left || w->left->color == RFS_RB_BLACK) &&
			    (!w->right || w->right->color == RFS_RB_BLACK)) {
				w->color = RFS_RB_RED;		/* case 2 */
				x = parent;
				parent = x->parent;
			} else {
				if (!w->right || w->right->color == RFS_RB_BLACK) {
					if (w->left)		/* case 3 */
						w->left->color = RFS_RB_BLACK;
					w->color = RFS_RB_RED;
					rb_rotate_right(t, w);
					w = parent->right;
				}
				w->color = parent->color;	/* case 4 */
				parent->color = RFS_RB_BLACK;
				if (w->right)
					w->right->color = RFS_RB_BLACK;
				rb_rotate_left(t, parent);
				x = t->root;
				break;
			}
		} else {					/* 镜像：x 是右孩子 */
			struct rfs_rb_node *w = parent->left;

			if (!w) {	/* 防御 */
				x = parent;
				parent = x->parent;
				continue;
			}
			if (w->color == RFS_RB_RED) {
				w->color = RFS_RB_BLACK;
				parent->color = RFS_RB_RED;
				rb_rotate_right(t, parent);
				w = parent->left;
			}
			if ((!w->right || w->right->color == RFS_RB_BLACK) &&
			    (!w->left || w->left->color == RFS_RB_BLACK)) {
				w->color = RFS_RB_RED;
				x = parent;
				parent = x->parent;
			} else {
				if (!w->left || w->left->color == RFS_RB_BLACK) {
					if (w->right)
						w->right->color = RFS_RB_BLACK;
					w->color = RFS_RB_RED;
					rb_rotate_left(t, w);
					w = parent->left;
				}
				w->color = parent->color;
				parent->color = RFS_RB_BLACK;
				if (w->left)
					w->left->color = RFS_RB_BLACK;
				rb_rotate_right(t, parent);
				x = t->root;
				break;
			}
		}
	}
	if (x)
		x->color = RFS_RB_BLACK;
}

void rfs_rb_erase(struct rfs_rb_root *t, struct rfs_rb_node *node)
{
	struct rfs_rb_node *y = node;
	struct rfs_rb_node *x;
	struct rfs_rb_node *xparent;
	unsigned char ycolor = node->color;

	if (!node->left) {				/* ≤1 孩子 */
		x = node->right;
		xparent = node->parent;
		rb_transplant(t, node, node->right);
	} else if (!node->right) {
		x = node->left;
		xparent = node->parent;
		rb_transplant(t, node, node->left);
	} else {					/* 两个孩子 → 用中序后继顶上 */
		y = node->right;
		while (y->left)
			y = y->left;
		ycolor = y->color;
		x = y->right;
		if (y->parent == node) {
			xparent = y;
		} else {
			xparent = y->parent;
			rb_transplant(t, y, y->right);
			y->right = node->right;
			y->right->parent = y;
		}
		rb_transplant(t, node, y);
		y->left = node->left;
		y->left->parent = y;
		y->color = node->color;
	}
	if (ycolor == RFS_RB_BLACK)
		rb_erase_fixup(t, x, xparent);
}

/* ══════════════════════════ 导航 ══════════════════════════ */

struct rfs_rb_node *rfs_rb_first(const struct rfs_rb_root *t)
{
	struct rfs_rb_node *n;

	if (!t || !t->root)
		return NULL;
	n = t->root;
	while (n->left)
		n = n->left;
	return n;
}

struct rfs_rb_node *rfs_rb_last(const struct rfs_rb_root *t)
{
	struct rfs_rb_node *n;

	if (!t || !t->root)
		return NULL;
	n = t->root;
	while (n->right)
		n = n->right;
	return n;
}

struct rfs_rb_node *rfs_rb_next(struct rfs_rb_node *node)
{
	struct rfs_rb_node *p;

	if (!node)
		return NULL;
	if (node->right) {
		node = node->right;
		while (node->left)
			node = node->left;
		return node;
	}
	while ((p = node->parent) && node == p->right)
		node = p;
	return node->parent;
}

struct rfs_rb_node *rfs_rb_prev(struct rfs_rb_node *node)
{
	struct rfs_rb_node *p;

	if (!node)
		return NULL;
	if (node->left) {
		node = node->left;
		while (node->right)
			node = node->right;
		return node;
	}
	while ((p = node->parent) && node == p->left)
		node = p;
	return node->parent;
}

struct rfs_rb_node *rfs_rb_lower_bound(const struct rfs_rb_root *t,
				       const struct rfs_rb_node *probe,
				       rfs_rb_cmp_fn cmp, void *ctx)
{
	const struct rfs_rb_node *n = t->root;
	const struct rfs_rb_node *best = NULL;

	while (n) {
		if (cmp(n, probe, ctx) < 0)		/* n < probe → 右寻 */
			n = n->right;
		else {					/* n ≥ probe → 记候选、左寻更小者 */
			best = n;
			n = n->left;
		}
	}
	return (struct rfs_rb_node *)best;
}

struct rfs_rb_node *rfs_rb_upper_bound(const struct rfs_rb_root *t,
				       const struct rfs_rb_node *probe,
				       rfs_rb_cmp_fn cmp, void *ctx)
{
	const struct rfs_rb_node *n = t->root;
	const struct rfs_rb_node *best = NULL;

	while (n) {
		if (cmp(n, probe, ctx) <= 0)		/* n ≤ probe → 右寻 */
			n = n->right;
		else {					/* n > probe → 记候选、左寻更小者 */
			best = n;
			n = n->left;
		}
	}
	return (struct rfs_rb_node *)best;
}

/* ══════════════════════════ 不变量自检 ══════════════════════════ */

/*
 * 递归自检：返回 0 或 -EINVAL。*bh 出参 = 以 n 为根的子树黑高（NULL 叶计 1）；
 * *prev 为中序游标（严格递增校验）。parent 为期望的父节点指针。
 */
static int rb_check_rec(const struct rfs_rb_node *n,
			const struct rfs_rb_node *parent,
			rfs_rb_cmp_fn cmp, void *ctx,
			const struct rfs_rb_node **prev, int *bh)
{
	int lh, rh, rc;

	if (!n) {
		*bh = 1;
		return 0;
	}
	if (n->parent != parent)			/* 父子自洽 */
		return -EINVAL;
	if (n->color != RFS_RB_RED && n->color != RFS_RB_BLACK)
		return -EINVAL;
	if (n->color == RFS_RB_RED && parent && parent->color == RFS_RB_RED)
		return -EINVAL;				/* 红红不相邻 */

	rc = rb_check_rec(n->left, n, cmp, ctx, prev, &lh);
	if (rc)
		return rc;
	if (*prev && cmp(*prev, n, ctx) >= 0)		/* 中序严格递增 */
		return -EINVAL;
	*prev = n;
	rc = rb_check_rec(n->right, n, cmp, ctx, prev, &rh);
	if (rc)
		return rc;
	if (lh != rh)					/* 黑高一致 */
		return -EINVAL;
	*bh = lh + (n->color == RFS_RB_BLACK ? 1 : 0);
	return 0;
}

int rfs_rb_check(const struct rfs_rb_root *t, rfs_rb_cmp_fn cmp, void *ctx)
{
	const struct rfs_rb_node *prev = NULL;
	int bh = 0;

	if (!t)
		return -EINVAL;
	if (!t->root)
		return 0;
	if (t->root->parent != NULL)
		return -EINVAL;
	if (t->root->color != RFS_RB_BLACK)
		return -EINVAL;
	return rb_check_rec(t->root, NULL, cmp, ctx, &prev, &bh);
}

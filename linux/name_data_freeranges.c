// SPDX-License-Identifier: GPL-2.0
/*
 * name_data_freeranges.c —— 目录名字数据面「空闲区间表」fr（v1.3）平台中立 C 实现
 *
 * 逐接口对照 `juvenile_god/name_data_freeranges.cpp` 翻译。契约与两态见
 * `include/linux/ravenfs_name_data_freeranges.h`。仅把 god 的 `mdata_piece_session`
 * （内部 pin/落盘）与 `block_alloc_hard/block_free` 换成注入 ops vtable，
 * `std::vector` 预算池换成栈上定长池（树高 ≤64），其余语义逐条对齐。
 *
 * 键序：空闲区间 (len, base)，**len 主序 / base 副序**升序；hint = 子树最大复合键。
 * 不变式：
 *   · 叶内条目严格按复合键升序；
 *   · hint 单调（分区路由）；
 *   · 树内**永不留下 count==0 的叶**（remainder refill / retire_empty_leaf）；
 *   · inline 恒 ≤ RFS_ND_FREE_INLINE_MAX(9)，>9 必 tree 态。
 *
 * 平台纪律：只 include ravenfs_name_data_freeranges.h；无 `#ifdef __KERNEL__`、
 * 无 `<linux/...>`、无 STL。mem* 用编译器内建，避免额外头依赖。
 * C(gnu11) 与内核双编应以 0 警告通过。
 */
#include "ravenfs_name_data_freeranges.h"

/* 337 階 / 分裂左半 169（= RFS_NDFR_*）。*/
#define FR_MAX		RFS_NDFR_MAX
#define FR_LEFT		RFS_NDFR_LEFT

/* 通用无效指针的 52 位实体（与 god name_data_freeranges.cpp 同）。*/
#define FR_PTR52	(((u64)PTR_INVALID) & RFS_MASK52)

/* inline 表取址：raven_dir_Inode_t 为 packed，直接 &dino->possible_inline_free_ranges
 * 触发 -Waddress-of-packed-member；经 u8* 偏移规避（offset 112 实为 8 对齐，安全）。*/
#define FR_INLINE_TAB(d) ((fr_t *)(void *)((u8 *)(d) + \
	__builtin_offsetof(struct raven_dir_Inode_t, possible_inline_free_ranges)))

typedef struct Bptree_meta_t					meta_t;
typedef struct Bptree_dir_name_data_free_ranges_leaf_node_t	leaf_t;
typedef struct Bptree_dir_name_data_free_ranges_inner_node_t	inner_t;
typedef struct free_range_t					fr_t;
typedef struct free_ranges_hint_t				hint_t;

/* ══════════════════════════ 内存内建（免 libc 头）══════════════════════════ */
static inline void fr_memset(void *d, int c, u64 n)
{
	__builtin_memset(d, c, (unsigned long)n);
}
static inline void fr_memcpy(void *d, const void *s, u64 n)
{
	__builtin_memcpy(d, s, (unsigned long)n);
}
static inline void fr_memmove(void *d, const void *s, u64 n)
{
	__builtin_memmove(d, s, (unsigned long)n);
}

/* 复合键比较：a < b ?（len 主序，base 副序）。*/
static inline bool fr_key_lt(u64 alen, u64 abase, u64 blen, u64 bbase)
{
	return alen < blen || (alen == blen && abase < bbase);
}

/* ══════════════════════════ inline 表（表按 len 升序）══════════════════════════ */

/* 首个 ≥ (len,base) 的下标（0..cnt）。*/
static u16 fr_inline_lower_bound(const fr_t *t, u16 cnt, u64 len, u64 base)
{
	u16 pos = 0;

	while (pos < cnt && fr_key_lt(t[pos].len, t[pos].base, len, base))
		pos++;
	return pos;
}

static void fr_inline_insert_raw(fr_t *t, u16 *cnt, u64 base, u64 len)
{
	u16 pos = fr_inline_lower_bound(t, *cnt, len, base);

	if (pos < *cnt)
		fr_memmove(&t[pos + 1], &t[pos],
			   (u64)(*cnt - pos) * sizeof(fr_t));
	t[pos].base = base;
	t[pos].len = len;
	(*cnt)++;
}

static void fr_inline_remove_raw(fr_t *t, u16 *cnt, u16 idx)
{
	if ((u32)idx + 1u < (u32)*cnt)
		fr_memmove(&t[idx], &t[idx + 1],
			   (u64)(*cnt - idx - 1) * sizeof(fr_t));
	fr_memset(&t[*cnt - 1], 0, sizeof(fr_t));
	(*cnt)--;
}

/* best-fit：表按 len 升序 ⇒ 首个 len≥need 即最小 len（同 len 内 base 升序）。*/
static u16 fr_inline_find_fit(const fr_t *t, u16 cnt, u64 need)
{
	u16 i;

	for (i = 0; i < cnt; i++)
		if (t[i].len >= need)
			return i;
	return 0xFFFFu;
}

/* ══════════════════════════ 节点基建 ══════════════════════════ */

bool rfs_ndfr_is_node(const struct Bptree_meta_t *m)
{
	return m->magics[0] == NAMEDATA_FREERANGES_MAGIC1 &&
	       m->magics[1] == NAMEDATA_FREERANGES_MAGIC2 &&
	       m->magics[2] == NAMEDATA_FREERANGES_MAGIC3 &&
	       m->magics[3] == NAMEDATA_FREERANGES_MAGIC4;
}

void rfs_ndfr_node_meta_init(void *buf, u16 count, bool is_inner, bool is_root,
			     u64 parent_ptr)
{
	meta_t *m = (meta_t *)buf;

	fr_memset(buf, 0, 8192);
	m->magics[0] = NAMEDATA_FREERANGES_MAGIC1;
	m->magics[1] = NAMEDATA_FREERANGES_MAGIC2;
	m->magics[2] = NAMEDATA_FREERANGES_MAGIC3;
	m->magics[3] = NAMEDATA_FREERANGES_MAGIC4;
	RFS_PP_SET_COUNT(m->parent_ptr, count);
	RFS_PP_SET_IS_INNER(m->parent_ptr, is_inner ? 1u : 0u);
	RFS_PP_SET_IS_ROOT(m->parent_ptr, is_root ? 1u : 0u);
	RFS_PP_SET_PTR(m->parent_ptr, parent_ptr);
	RFS_PTR_SET_ADDR(m->ptrs[0], PTR_INVALID);
	RFS_PTR_SET_ADDR(m->ptrs[1], PTR_INVALID);
	RFS_PTR_SET_ADDR(m->ptrs[2], PTR_INVALID);
}

void rfs_ndfr_max_key(const void *buf, u64 *out_len, u64 *out_base)
{
	const meta_t *m = (const meta_t *)buf;
	u16 cnt = (u16)RFS_PP_COUNT(m->parent_ptr);

	if (cnt == 0) {
		*out_len = 0;
		*out_base = 0;
		return;
	}
	if (RFS_PP_IS_INNER(m->parent_ptr)) {
		const inner_t *n = (const inner_t *)buf;

		*out_len = n->hints[cnt - 1].max_len;
		*out_base = n->hints[cnt - 1].max_base;
	} else {
		const leaf_t *l = (const leaf_t *)buf;

		*out_len = l->free_ranges[cnt - 1].len;
		*out_base = l->free_ranges[cnt - 1].base;
	}
}

/* ══════════════════════════ 只读导航 ══════════════════════════ */

int rfs_ndfr_walk(struct rfs_ndfr_ctx *ctx, u64 root, u64 need_len,
		  u64 need_base, struct bptree_path_t *trace)
{
	u64 cur = root;
	int depth;
	bool err = false;
	void *buf = NULL;
	meta_t *m;
	u16 cnt;

	if (!trace)
		return -EINVAL;
	trace->depth = 0;
	trace->is_leaf_hit = false;

	for (depth = 0; depth < 64; depth++) {
		err = false;
		buf = ctx->ops->node_pin(ctx->host, cur, &err);
		if (!buf)
			return -EIO;
		m = (meta_t *)buf;
		if (!rfs_ndfr_is_node(m))
			goto bad;
		if ((depth == 0) != (bool)RFS_PP_IS_ROOT(m->parent_ptr))
			goto bad;
		trace->blocks[depth] = cur;

		if (!RFS_PP_IS_INNER(m->parent_ptr)) {
			const leaf_t *leaf = (const leaf_t *)buf;
			u16 lo = 0, hi;

			cnt = (u16)RFS_PP_COUNT(leaf->meta.parent_ptr);
			hi = cnt;
			while (lo < hi) {
				u16 mid = (u16)(lo + (hi - lo) / 2);
				const fr_t *e = &leaf->free_ranges[mid];

				if (fr_key_lt(e->len, e->base, need_len, need_base))
					lo = (u16)(mid + 1);
				else
					hi = mid;
			}
			trace->depth = depth + 1;
			trace->child_idx[depth] = lo;	/* 可 >255：u16，禁截断 */
			trace->is_leaf_hit = (lo < cnt);
			ctx->ops->node_unpin(ctx->host, cur, buf, false);
			return 0;
		}

		{
			const inner_t *node = (const inner_t *)buf;
			u16 lo = 0, hi, idx;
			u64 next;

			cnt = (u16)RFS_PP_COUNT(node->meta.parent_ptr);
			if (cnt == 0)
				goto bad;
			hi = cnt;
			while (lo < hi) {
				u16 mid = (u16)(lo + (hi - lo) / 2);
				const hint_t *h = &node->hints[mid];

				if (fr_key_lt(h->max_len, h->max_base,
					      need_len, need_base))
					lo = (u16)(mid + 1);
				else
					hi = mid;
			}
			idx = (lo < cnt) ? lo : (u16)(cnt - 1);	/* 击穿取最右 */
			trace->child_idx[depth] = idx;		/* 可 >255：u16 */
			next = RFS_PTR_ADDR(node->hints[idx].sub_node_ptr);
			ctx->ops->node_unpin(ctx->host, cur, buf, false);
			cur = next;
		}
	}
	return -EINVAL;

bad:
	ctx->ops->node_unpin(ctx->host, cur, buf, false);
	return -EINVAL;
}

/* ══════════════════════════ 父链 hint 刷新 ══════════════════════════ */

static int ndfr_refresh_key_chain(struct rfs_ndfr_ctx *ctx, u64 blk)
{
	u64 cur = blk;
	int hop;

	for (hop = 0; hop < 64; hop++) {
		bool err = false;
		void *cb = ctx->ops->node_pin(ctx->host, cur, &err);
		meta_t *cm;
		u64 nl, nb, parent_blk;
		void *pb;
		inner_t *p;
		u16 pcnt, i;
		bool found = false;

		if (!cb)
			return -EIO;
		cm = (meta_t *)cb;
		if (!rfs_ndfr_is_node(cm)) {
			ctx->ops->node_unpin(ctx->host, cur, cb, false);
			return -EINVAL;
		}
		if (RFS_PP_IS_ROOT(cm->parent_ptr)) {
			ctx->ops->node_unpin(ctx->host, cur, cb, false);
			return 0;
		}
		rfs_ndfr_max_key(cb, &nl, &nb);
		parent_blk = RFS_PP_PTR(cm->parent_ptr);
		ctx->ops->node_unpin(ctx->host, cur, cb, false);
		if (parent_blk == FR_PTR52)
			return -EINVAL;

		pb = ctx->ops->node_pin(ctx->host, parent_blk, &err);
		if (!pb)
			return -EIO;
		p = (inner_t *)pb;
		if (!rfs_ndfr_is_node(&p->meta) ||
		    !RFS_PP_IS_INNER(p->meta.parent_ptr)) {
			ctx->ops->node_unpin(ctx->host, parent_blk, pb, false);
			return -EINVAL;
		}
		pcnt = (u16)RFS_PP_COUNT(p->meta.parent_ptr);
		for (i = 0; i < pcnt; i++) {
			if (RFS_PTR_ADDR(p->hints[i].sub_node_ptr) == cur) {
				found = true;
				break;
			}
		}
		if (!found) {
			ctx->ops->node_unpin(ctx->host, parent_blk, pb, false);
			return -EINVAL;
		}
		if (p->hints[i].max_len == nl && p->hints[i].max_base == nb) {
			ctx->ops->node_unpin(ctx->host, parent_blk, pb, false);
			return 0;
		}
		p->hints[i].max_len = nl;
		p->hints[i].max_base = nb;
		ctx->ops->node_unpin(ctx->host, parent_blk, pb, true);
		cur = parent_blk;
	}
	return -EINVAL;
}

/* ══════════════════════════ find_fit（只读 best-fit）══════════════════════════ */

int rfs_ndfr_find_fit(struct rfs_ndfr_ctx *ctx, u64 root, u64 need,
		      u64 *out_base, u64 *out_len)
{
	struct bptree_path_t tr;
	u64 leaf_blk;
	u16 idx;
	bool err = false;
	void *buf;
	leaf_t *leaf;
	fr_t e;
	int rc;

	if (!out_base || !out_len)
		return -EINVAL;
	rc = rfs_ndfr_walk(ctx, root, need, 0, &tr);
	if (rc)
		return rc;
	if (!tr.is_leaf_hit)
		return -ENOENT;
	leaf_blk = tr.blocks[tr.depth - 1];
	idx = tr.child_idx[tr.depth - 1];

	buf = ctx->ops->node_pin(ctx->host, leaf_blk, &err);
	if (!buf)
		return -EIO;
	leaf = (leaf_t *)buf;
	if (idx >= (u16)RFS_PP_COUNT(leaf->meta.parent_ptr)) {
		ctx->ops->node_unpin(ctx->host, leaf_blk, buf, false);
		return -EINVAL;
	}
	e = leaf->free_ranges[idx];
	ctx->ops->node_unpin(ctx->host, leaf_blk, buf, false);
	if (e.len < need)
		return -ENOENT;
	*out_base = e.base;
	*out_len = e.len;
	return 0;
}

/* ══════════════════════════ 分裂原语 ══════════════════════════ */

struct fr_split {
	int	rc;
	u64	lo_len, lo_base;	/* 左半最大复合键 */
	u64	hi_len, hi_base;	/* 右半最大复合键 */
	u64	hi_blk;			/* 右半块号 */
};

/* 父内部节点：victim 子树最大键改 lo；victim+1 处插入 hi hint。*/
static int ndfr_parent_apply(struct rfs_ndfr_ctx *ctx, u64 parent_blk,
			     u16 victim_idx, u64 hi_blk,
			     u64 lo_len, u64 lo_base,
			     u64 hi_len, u64 hi_base, bool *out_split)
{
	bool err = false;
	void *pb = ctx->ops->node_pin(ctx->host, parent_blk, &err);
	inner_t *p;
	u16 cnt, ins;

	*out_split = false;
	if (!pb)
		return -EIO;
	p = (inner_t *)pb;
	if (!rfs_ndfr_is_node(&p->meta) ||
	    !RFS_PP_IS_INNER(p->meta.parent_ptr)) {
		ctx->ops->node_unpin(ctx->host, parent_blk, pb, false);
		return -EINVAL;
	}
	cnt = (u16)RFS_PP_COUNT(p->meta.parent_ptr);
	if (victim_idx >= cnt) {
		ctx->ops->node_unpin(ctx->host, parent_blk, pb, false);
		return -EINVAL;
	}
	p->hints[victim_idx].max_len = lo_len;
	p->hints[victim_idx].max_base = lo_base;
	ins = (u16)(victim_idx + 1);
	if (ins < cnt)
		fr_memmove(&p->hints[ins + 1], &p->hints[ins],
			   (u64)(cnt - ins) * sizeof(hint_t));
	p->hints[ins].max_len = hi_len;
	p->hints[ins].max_base = hi_base;
	RFS_PTR_SET_ADDR(p->hints[ins].sub_node_ptr, hi_blk);
	RFS_PP_SET_COUNT(p->meta.parent_ptr, (u16)(cnt + 1));
	*out_split = ((u32)cnt + 1u > (u32)FR_MAX);
	ctx->ops->node_unpin(ctx->host, parent_blk, pb, true);
	return 0;
}

/* 新建根内部节点（2 孩）；两子去 root 标 + 父指新根。*new_root = prealloc。*/
static int ndfr_new_root_inner(struct rfs_ndfr_ctx *ctx,
			       u64 lo_blk, u64 lo_len, u64 lo_base,
			       u64 hi_blk, u64 hi_len, u64 hi_base,
			       u64 prealloc, u64 *new_root)
{
	bool err = false;
	void *rb = ctx->ops->node_pin(ctx->host, prealloc, &err);
	inner_t *r;
	int k;

	if (!rb)
		return -EIO;
	rfs_ndfr_node_meta_init(rb, 2, true, true, ctx->dir_inode_idx);
	r = (inner_t *)rb;
	r->hints[0].max_len = lo_len;
	r->hints[0].max_base = lo_base;
	RFS_PTR_SET_ADDR(r->hints[0].sub_node_ptr, lo_blk);
	r->hints[1].max_len = hi_len;
	r->hints[1].max_base = hi_base;
	RFS_PTR_SET_ADDR(r->hints[1].sub_node_ptr, hi_blk);
	ctx->ops->node_unpin(ctx->host, prealloc, rb, true);

	for (k = 0; k < 2; k++) {
		u64 cb = (k == 0) ? lo_blk : hi_blk;
		void *c = ctx->ops->node_pin(ctx->host, cb, &err);
		meta_t *cm;

		if (!c)
			return -EIO;
		cm = (meta_t *)c;
		RFS_PP_SET_PTR(cm->parent_ptr, prealloc);
		RFS_PP_SET_IS_ROOT(cm->parent_ptr, 0);
		ctx->ops->node_unpin(ctx->host, cb, c, true);
	}
	*new_root = prealloc;
	return 0;
}

/* ══════════════════════════ insert ══════════════════════════ */

int rfs_ndfr_insert(struct rfs_ndfr_ctx *ctx, struct raven_dir_Inode_t *dino,
		    u64 base, u64 len)
{
	u64 ctl, root, leaf_blk, new_root;
	struct bptree_path_t trace;
	u64 pool[65];
	int npool = 0, used = 0, i, rc;
	bool err = false;
	void *buf;
	leaf_t *leaf;
	u16 cnt, pos;
	struct fr_split sr;

	if (len == 0)
		return 0;
	ctl = dino->name_data_free_ranges_ctl;

	/* ── 空态 → inline bootstrap ── */
	if (RFS_ND_FR_CTL_PTR(ctl) == RFS_ND_FR_CTL_PTR_INVALID) {
		fr_t *t = FR_INLINE_TAB(dino);

		t[0].base = base;
		t[0].len = len;
		dino->name_data_free_ranges_ctl = RFS_ND_FR_CTL_PACK(1, 0);
		RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count, 1);
		return 0;
	}

	/* ── inline 态：就地插入；超 9 条隐式升格 tree ── */
	if (RFS_ND_FR_CTL_IS_INLINE(ctl)) {
		fr_t *t = FR_INLINE_TAB(dino);

		cnt = (u16)RFS_ND_FR_COUNT(dino->name_data_free_ranges_count);
		if (cnt > RFS_ND_FREE_INLINE_MAX)
			return -EINVAL;
		pos = fr_inline_lower_bound(t, cnt, len, base);
		if (pos < cnt && t[pos].len == len && t[pos].base == base)
			return 0;	/* 精确重复：幂等跳过 */
		if (cnt < RFS_ND_FREE_INLINE_MAX) {
			fr_inline_insert_raw(t, &cnt, base, len);
			RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count,
					    cnt);
			return 0;
		}

		/* cnt == 9：合计 10 条 → 建根叶承载 */
		{
			u64 blk = ctx->ops->node_alloc(ctx->host, &err);
			fr_t merged[RFS_ND_FREE_INLINE_MAX + 1];
			u16 m = 0, j;
			bool placed = false;
			leaf_t *nl;

			if (blk == RFS_NDFR_NIL || err)
				return -ENOSPC;
			for (j = 0; j < cnt; j++) {
				if (!placed &&
				    fr_key_lt(len, base, t[j].len, t[j].base)) {
					merged[m].base = base;
					merged[m].len = len;
					m++;
					placed = true;
				}
				merged[m++] = t[j];
			}
			if (!placed) {
				merged[m].base = base;
				merged[m].len = len;
				m++;
			}
			buf = ctx->ops->node_pin(ctx->host, blk, &err);
			if (!buf) {
				ctx->ops->node_free(ctx->host, blk);
				return -EIO;
			}
			rfs_ndfr_node_meta_init(buf, m, false, true,
						ctx->dir_inode_idx);
			nl = (leaf_t *)buf;
			fr_memcpy(nl->free_ranges, merged,
				  (u64)m * sizeof(fr_t));
			ctx->ops->node_unpin(ctx->host, blk, buf, true);
			RFS_ND_FR_CTL_SET_PTR(dino->name_data_free_ranges_ctl,
					      blk);
			RFS_ND_FR_CTL_SET_INLINE(dino->name_data_free_ranges_ctl,
						 0);
			RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count, m);
			fr_memset(t, 0, sizeof(fr_t) * RFS_ND_FREE_INLINE_MAX);
			return 0;
		}
	}

	/* ── tree 态（root 恒有效）── */
	root = RFS_ND_FR_CTL_PTR(ctl);
	rc = rfs_ndfr_walk(ctx, root, len, base, &trace);
	if (rc)
		return rc;
	leaf_blk = trace.blocks[trace.depth - 1];

	/* 非满：就地插入 */
	buf = ctx->ops->node_pin(ctx->host, leaf_blk, &err);
	if (!buf)
		return -EIO;
	leaf = (leaf_t *)buf;
	cnt = (u16)RFS_PP_COUNT(leaf->meta.parent_ptr);
	if (cnt < FR_MAX) {
		pos = 0;
		while (pos < cnt &&
		       fr_key_lt(leaf->free_ranges[pos].len,
				 leaf->free_ranges[pos].base, len, base))
			pos++;
		if (pos < cnt && leaf->free_ranges[pos].len == len &&
		    leaf->free_ranges[pos].base == base) {
			ctx->ops->node_unpin(ctx->host, leaf_blk, buf, false);
			return 0;
		}
		if (pos < cnt)
			fr_memmove(&leaf->free_ranges[pos + 1],
				   &leaf->free_ranges[pos],
				   (u64)(cnt - pos) * sizeof(fr_t));
		leaf->free_ranges[pos].base = base;
		leaf->free_ranges[pos].len = len;
		RFS_PP_SET_COUNT(leaf->meta.parent_ptr, (u16)(cnt + 1));
		ctx->ops->node_unpin(ctx->host, leaf_blk, buf, true);
		RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count,
				    RFS_ND_FR_COUNT(
					    dino->name_data_free_ranges_count) + 1);
		return ndfr_refresh_key_chain(ctx, leaf_blk);
	}
	ctx->ops->node_unpin(ctx->host, leaf_blk, buf, false);

	/* 满叶：预算池分裂级联（按树高预分配全部节点；之后仅指针改写）*/
	for (i = 0; i <= trace.depth; i++) {
		u64 b = ctx->ops->node_alloc(ctx->host, &err);

		if (b == RFS_NDFR_NIL || err) {
			int j;

			for (j = 0; j < npool; j++)
				ctx->ops->node_free(ctx->host, pool[j]);
			return -ENOSPC;
		}
		pool[npool++] = b;
	}

	/* 瞬态插入（338）*/
	buf = ctx->ops->node_pin(ctx->host, leaf_blk, &err);
	if (!buf) {
		int j;

		for (j = 0; j < npool; j++)
			ctx->ops->node_free(ctx->host, pool[j]);
		return -EIO;
	}
	leaf = (leaf_t *)buf;
	cnt = (u16)RFS_PP_COUNT(leaf->meta.parent_ptr);
	pos = 0;
	while (pos < cnt &&
	       fr_key_lt(leaf->free_ranges[pos].len, leaf->free_ranges[pos].base,
			 len, base))
		pos++;
	if (pos < cnt)
		fr_memmove(&leaf->free_ranges[pos + 1], &leaf->free_ranges[pos],
			   (u64)(cnt - pos) * sizeof(fr_t));
	leaf->free_ranges[pos].base = base;
	leaf->free_ranges[pos].len = len;
	RFS_PP_SET_COUNT(leaf->meta.parent_ptr, (u16)(cnt + 1));	/* 338 */
	ctx->ops->node_unpin(ctx->host, leaf_blk, buf, true);

	/* 分裂叶 169 / 169 */
	sr.rc = 0;
	{
		bool e2 = false;
		void *vs = ctx->ops->node_pin(ctx->host, leaf_blk, &e2);

		if (!vs) {
			sr.rc = -EIO;
		} else {
			leaf_t *v = (leaf_t *)vs;
			u16 vcnt = (u16)RFS_PP_COUNT(v->meta.parent_ptr);
			void *ns = ctx->ops->node_pin(ctx->host, pool[used], &e2);
			leaf_t *n;

			if (!ns) {
				ctx->ops->node_unpin(ctx->host, leaf_blk, vs,
						     false);
				sr.rc = -EIO;
			} else {
				n = (leaf_t *)ns;
				rfs_ndfr_node_meta_init(
					ns, (u16)(vcnt - FR_LEFT), false, false,
					RFS_PP_PTR(v->meta.parent_ptr));
				fr_memcpy(&n->free_ranges[0],
					  &v->free_ranges[FR_LEFT],
					  (u64)(vcnt - FR_LEFT) * sizeof(fr_t));
				RFS_PP_SET_COUNT(v->meta.parent_ptr, FR_LEFT);
				sr.lo_len = v->free_ranges[FR_LEFT - 1].len;
				sr.lo_base = v->free_ranges[FR_LEFT - 1].base;
				sr.hi_len = n->free_ranges[vcnt - FR_LEFT - 1].len;
				sr.hi_base =
					n->free_ranges[vcnt - FR_LEFT - 1].base;
				sr.hi_blk = pool[used];
				ctx->ops->node_unpin(ctx->host, leaf_blk, vs, true);
				ctx->ops->node_unpin(ctx->host, pool[used], ns,
						     true);
				used++;
			}
		}
	}

	new_root = root;
	if (sr.rc == 0) {
		if (trace.depth == 1) {
			rc = ndfr_new_root_inner(
				ctx, leaf_blk, sr.lo_len, sr.lo_base, sr.hi_blk,
				sr.hi_len, sr.hi_base, pool[used++], &new_root);
		} else {
			u64 parent_blk = trace.blocks[trace.depth - 2];
			u16 child_idx = trace.child_idx[trace.depth - 2];
			bool parent_split = false;
			u64 cur_parent = parent_blk;
			int guard = 0;

			rc = ndfr_parent_apply(ctx, parent_blk, child_idx,
					       sr.hi_blk, sr.lo_len, sr.lo_base,
					       sr.hi_len, sr.hi_base,
					       &parent_split);
			while (rc == 0 && parent_split) {
				bool e3 = false;
				void *pnb;
				inner_t *pn;
				u16 pcnt;
				bool p_is_root;
				u64 grand_blk;
				struct fr_split ir;

				if (++guard > 64) {
					rc = -EINVAL;
					break;
				}
				pnb = ctx->ops->node_pin(ctx->host, cur_parent,
							 &e3);
				if (!pnb) {
					rc = -EIO;
					break;
				}
				pn = (inner_t *)pnb;
				if (!rfs_ndfr_is_node(&pn->meta) ||
				    !RFS_PP_IS_INNER(pn->meta.parent_ptr)) {
					ctx->ops->node_unpin(ctx->host, cur_parent,
							     pnb, false);
					rc = -EINVAL;
					break;
				}
				pcnt = (u16)RFS_PP_COUNT(pn->meta.parent_ptr);
				p_is_root = (bool)RFS_PP_IS_ROOT(pn->meta.parent_ptr);
				grand_blk = RFS_PP_PTR(pn->meta.parent_ptr);

				ir.rc = 0;
				{
					void *ns = ctx->ops->node_pin(
						ctx->host, pool[used], &e3);

					if (!ns) {
						ctx->ops->node_unpin(
							ctx->host, cur_parent,
							pnb, false);
						ir.rc = -EIO;
					} else {
						inner_t *n = (inner_t *)ns;
						u16 hn = (u16)(pcnt - FR_LEFT);
						u16 k;

						rfs_ndfr_node_meta_init(
							ns, hn, true, false,
							RFS_PP_PTR(
								pn->meta.parent_ptr));
						fr_memcpy(&n->hints[0],
							  &pn->hints[FR_LEFT],
							  (u64)hn *
								  sizeof(hint_t));
						RFS_PP_SET_COUNT(
							pn->meta.parent_ptr,
							FR_LEFT);
						ir.lo_len =
							pn->hints[FR_LEFT - 1]
								.max_len;
						ir.lo_base =
							pn->hints[FR_LEFT - 1]
								.max_base;
						ir.hi_len =
							n->hints[hn - 1].max_len;
						ir.hi_base =
							n->hints[hn - 1].max_base;
						ir.hi_blk = pool[used];
						for (k = 0; k < hn; k++) {
							u64 cb = RFS_PTR_ADDR(
								n->hints[k]
									.sub_node_ptr);
							void *c =
								ctx->ops->node_pin(
									ctx->host,
									cb, &e3);
							meta_t *cm;

							if (!c) {
								ir.rc = -EIO;
								break;
							}
							cm = (meta_t *)c;
							RFS_PP_SET_PTR(
								cm->parent_ptr,
								ir.hi_blk);
							ctx->ops->node_unpin(
								ctx->host, cb,
								c, true);
						}
						if (ir.rc == 0) {
							ctx->ops->node_unpin(
								ctx->host,
								cur_parent,
								pnb, true);
							ctx->ops->node_unpin(
								ctx->host,
								pool[used], ns,
								true);
							used++;
						} else {
							ctx->ops->node_unpin(
								ctx->host,
								cur_parent,
								pnb, false);
							ctx->ops->node_unpin(
								ctx->host,
								pool[used], ns,
								false);
						}
					}
				}
				if (ir.rc != 0) {
					rc = ir.rc;
					break;
				}
				if (p_is_root) {
					rc = ndfr_new_root_inner(
						ctx, cur_parent, ir.lo_len,
						ir.lo_base, ir.hi_blk,
						ir.hi_len, ir.hi_base,
						pool[used++], &new_root);
					break;
				}
				/* 找祖父中 cur_parent 的 idx */
				{
					bool e4 = false;
					void *gb = ctx->ops->node_pin(
						ctx->host, grand_blk, &e4);
					inner_t *g;
					u16 gcnt, gidx;
					bool found = false;

					if (!gb) {
						rc = -EIO;
						break;
					}
					g = (inner_t *)gb;
					if (!rfs_ndfr_is_node(&g->meta) ||
					    !RFS_PP_IS_INNER(
						    g->meta.parent_ptr)) {
						ctx->ops->node_unpin(
							ctx->host, grand_blk,
							gb, false);
						rc = -EINVAL;
						break;
					}
					gcnt = (u16)RFS_PP_COUNT(
						g->meta.parent_ptr);
					gidx = 0;
					for (; gidx < gcnt; gidx++) {
						if (RFS_PTR_ADDR(
							    g->hints[gidx]
								    .sub_node_ptr) ==
						    cur_parent) {
							found = true;
							break;
						}
					}
					ctx->ops->node_unpin(ctx->host,
							     grand_blk, gb,
							     false);
					if (!found) {
						rc = -EINVAL;
						break;
					}
					rc = ndfr_parent_apply(
						ctx, grand_blk, gidx, ir.hi_blk,
						ir.lo_len, ir.lo_base,
						ir.hi_len, ir.hi_base,
						&parent_split);
					cur_parent = grand_blk;
				}
			}
		}
		if (rc == 0) {
			RFS_ND_FR_CTL_SET_PTR(dino->name_data_free_ranges_ctl,
					      new_root);
			RFS_ND_FR_SET_COUNT(
				dino->name_data_free_ranges_count,
				RFS_ND_FR_COUNT(
					dino->name_data_free_ranges_count) + 1);
			rc = ndfr_refresh_key_chain(ctx, leaf_blk);
			if (rc == 0)
				rc = ndfr_refresh_key_chain(ctx, sr.hi_blk);
		}
	}
	for (; used < npool; used++)
		ctx->ops->node_free(ctx->host, pool[used]);
	return rc;
}

/* ══════════════════════════ take_fit ══════════════════════════ */

int rfs_ndfr_take_fit(struct rfs_ndfr_ctx *ctx, struct raven_dir_Inode_t *dino,
		      u64 need, u64 *out_base)
{
	u64 ctl, ptr, root, leaf_blk;
	struct bptree_path_t trace;
	u16 idx, cnt_after;
	fr_t e;
	int rc;
	bool err = false;
	void *buf;
	leaf_t *leaf;

	if (!out_base || need == 0)
		return -EINVAL;
	ctl = dino->name_data_free_ranges_ctl;
	ptr = RFS_ND_FR_CTL_PTR(ctl);
	if (ptr == RFS_ND_FR_CTL_PTR_INVALID)
		return -ENOENT;

	/* ── inline 态：线性 best-fit 摘除；余段回填（净数不增）── */
	if (RFS_ND_FR_CTL_IS_INLINE(ctl)) {
		fr_t *t = FR_INLINE_TAB(dino);
		u16 cnt = (u16)RFS_ND_FR_COUNT(
			dino->name_data_free_ranges_count);
		u16 fi;

		if (cnt > RFS_ND_FREE_INLINE_MAX)
			return -EINVAL;
		fi = fr_inline_find_fit(t, cnt, need);
		if (fi == 0xFFFFu)
			return -ENOENT;
		e = t[fi];
		fr_inline_remove_raw(t, &cnt, fi);
		if (e.len > need)
			fr_inline_insert_raw(t, &cnt, e.base + need,
					     e.len - need);
		if (cnt == 0) {
			dino->name_data_free_ranges_ctl = RFS_ND_FR_CTL_INACTIVE;
			RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count,
					    0);
		} else {
			RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count,
					    cnt);
		}
		*out_base = e.base;
		return 0;
	}

	/* ── tree 态 ── */
	root = ptr;
	rc = rfs_ndfr_walk(ctx, root, need, 0, &trace);
	if (rc)
		return rc;
	if (!trace.is_leaf_hit)
		return -ENOENT;
	leaf_blk = trace.blocks[trace.depth - 1];
	idx = trace.child_idx[trace.depth - 1];

	buf = ctx->ops->node_pin(ctx->host, leaf_blk, &err);
	if (!buf)
		return -EIO;
	leaf = (leaf_t *)buf;
	{
		u16 cnt = (u16)RFS_PP_COUNT(leaf->meta.parent_ptr);

		if (idx >= cnt) {
			ctx->ops->node_unpin(ctx->host, leaf_blk, buf, false);
			return -EINVAL;
		}
		e = leaf->free_ranges[idx];
		if (e.len < need) {
			ctx->ops->node_unpin(ctx->host, leaf_blk, buf, false);
			return -ENOENT;
		}
		if (idx < cnt - 1)
			fr_memmove(&leaf->free_ranges[idx],
				   &leaf->free_ranges[idx + 1],
				   (u64)(cnt - idx - 1) * sizeof(fr_t));
		fr_memset(&leaf->free_ranges[cnt - 1], 0, sizeof(fr_t));
		cnt_after = (u16)(cnt - 1);
		RFS_PP_SET_COUNT(leaf->meta.parent_ptr, cnt_after);
		ctx->ops->node_unpin(ctx->host, leaf_blk, buf, true);
	}
	RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count,
			    RFS_ND_FR_COUNT(
				    dino->name_data_free_ranges_count) - 1);

	if (cnt_after == 0) {
		if (e.len > need) {
			/* 余段直接放回同一（已空）叶：避免留下空叶 (0,0) hint */
			buf = ctx->ops->node_pin(ctx->host, leaf_blk, &err);
			if (!buf)
				return -EIO;
			leaf = (leaf_t *)buf;
			leaf->free_ranges[0].base = e.base + need;
			leaf->free_ranges[0].len = e.len - need;
			RFS_PP_SET_COUNT(leaf->meta.parent_ptr, 1);
			ctx->ops->node_unpin(ctx->host, leaf_blk, buf, true);
			RFS_ND_FR_SET_COUNT(
				dino->name_data_free_ranges_count,
				RFS_ND_FR_COUNT(
					dino->name_data_free_ranges_count) + 1);
			rc = ndfr_refresh_key_chain(ctx, leaf_blk);
			if (rc)
				return rc;
		} else {
			rc = rfs_ndfr_retire_empty_leaf(ctx, dino, leaf_blk);
			if (rc)
				return rc;
		}
	} else {
		rc = ndfr_refresh_key_chain(ctx, leaf_blk);
		if (rc)
			return rc;
		if (e.len > need) {
			rc = rfs_ndfr_insert(ctx, dino, e.base + need,
					     e.len - need);
			if (rc)
				return rc;
		}
	}
	*out_base = e.base;
	return 0;
}

/* ══════════════════════════ retire_empty_leaf ══════════════════════════ */

int rfs_ndfr_retire_empty_leaf(struct rfs_ndfr_ctx *ctx,
			       struct raven_dir_Inode_t *dino, u64 leaf_blk)
{
	u64 cur = leaf_blk;
	int hop;

	for (hop = 0; hop < 64; hop++) {
		u64 parent_blk = 0, only_child = 0;
		u16 pcnt_after = 0;
		bool p_is_root = false;
		bool err = false;
		void *cb;
		meta_t *cm;

		cb = ctx->ops->node_pin(ctx->host, cur, &err);
		if (!cb)
			return -EIO;
		cm = (meta_t *)cb;
		if (!rfs_ndfr_is_node(cm)) {
			ctx->ops->node_unpin(ctx->host, cur, cb, false);
			return -EINVAL;
		}
		if (RFS_PP_IS_ROOT(cm->parent_ptr)) {
			/* cur 是根叶（已空）→ 整树退役 */
			ctx->ops->node_unpin(ctx->host, cur, cb, false);
			ctx->ops->node_free(ctx->host, cur);
			dino->name_data_free_ranges_ctl =
				RFS_ND_FR_CTL_INACTIVE;
			RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count, 0);
			return 0;
		}
		parent_blk = RFS_PP_PTR(cm->parent_ptr);
		ctx->ops->node_unpin(ctx->host, cur, cb, false);
		if (parent_blk == FR_PTR52)
			return -EINVAL;

		{
			void *pb = ctx->ops->node_pin(ctx->host, parent_blk,
						      &err);
			inner_t *p;
			u16 pcnt, pi;
			bool found = false;

			if (!pb)
				return -EIO;
			p = (inner_t *)pb;
			if (!rfs_ndfr_is_node(&p->meta) ||
			    !RFS_PP_IS_INNER(p->meta.parent_ptr)) {
				ctx->ops->node_unpin(ctx->host, parent_blk, pb,
						     false);
				return -EINVAL;
			}
			pcnt = (u16)RFS_PP_COUNT(p->meta.parent_ptr);
			for (pi = 0; pi < pcnt; pi++) {
				if (RFS_PTR_ADDR(p->hints[pi].sub_node_ptr) ==
				    cur) {
					found = true;
					break;
				}
			}
			if (!found) {
				ctx->ops->node_unpin(ctx->host, parent_blk, pb,
						     false);
				return -EINVAL;
			}
			if (pi < pcnt - 1)
				fr_memmove(&p->hints[pi], &p->hints[pi + 1],
					   (u64)(pcnt - pi - 1) *
						   sizeof(hint_t));
			fr_memset(&p->hints[pcnt - 1], 0, sizeof(hint_t));
			pcnt_after = (u16)(pcnt - 1);
			RFS_PP_SET_COUNT(p->meta.parent_ptr, pcnt_after);
			p_is_root = (bool)RFS_PP_IS_ROOT(p->meta.parent_ptr);
			if (pcnt_after == 1)
				only_child = RFS_PTR_ADDR(
					p->hints[0].sub_node_ptr);
			ctx->ops->node_unpin(ctx->host, parent_blk, pb, true);
		}
		ctx->ops->node_free(ctx->host, cur);

		if (pcnt_after == 0) {
			if (p_is_root) {	/* 根内部清空（防御）*/
				ctx->ops->node_free(ctx->host, parent_blk);
				dino->name_data_free_ranges_ctl =
					RFS_ND_FR_CTL_INACTIVE;
				RFS_ND_FR_SET_COUNT(
					dino->name_data_free_ranges_count, 0);
				return 0;
			}
			cur = parent_blk;	/* 父空 → 继续上溯 */
			continue;
		}
		if (p_is_root) {
			if (pcnt_after == 1) {	/* 根内部剩 1 孩 → 收缩提升 */
				bool e2 = false;
				void *cks = ctx->ops->node_pin(ctx->host,
							       only_child, &e2);
				meta_t *ck;

				if (!cks)
					return -EIO;
				ck = (meta_t *)cks;
				RFS_PP_SET_IS_ROOT(ck->parent_ptr, 1);
				RFS_PP_SET_PTR(ck->parent_ptr,
					       ctx->dir_inode_idx);
				if (!RFS_PP_IS_INNER(ck->parent_ptr)) {
					RFS_PTR_SET_ADDR(ck->ptrs[0], PTR_INVALID);
					RFS_PTR_SET_ADDR(ck->ptrs[1], PTR_INVALID);
					RFS_PTR_SET_ADDR(ck->ptrs[2], PTR_INVALID);
				}
				ctx->ops->node_unpin(ctx->host, only_child, cks,
						     true);
				ctx->ops->node_free(ctx->host, parent_blk);
				RFS_ND_FR_CTL_SET_PTR(
					dino->name_data_free_ranges_ctl,
					only_child);
			}
			return 0;
		}
		return ndfr_refresh_key_chain(ctx, parent_blk);
	}
	return -EINVAL;
}

/* ══════════════════════════ release ══════════════════════════ */

/* 递归归还整棵 fr 树节点（DFS；深度 ≤64，链上同时 pin 数 = 树高）。*/
static int ndfr_free_tree(struct rfs_ndfr_ctx *ctx, u64 root, int depth)
{
	bool err = false;
	void *buf;

	if (root == RFS_NDFR_NIL || root == RFS_ND_FR_CTL_PTR_INVALID ||
	    root == FR_PTR52 || depth > 64)
		return 0;
	buf = ctx->ops->node_pin(ctx->host, root, &err);
	if (buf) {
		meta_t *m = (meta_t *)buf;

		if (rfs_ndfr_is_node(m) &&
		    RFS_PP_IS_INNER(m->parent_ptr)) {
			inner_t *n = (inner_t *)buf;
			u16 cnt = (u16)RFS_PP_COUNT(n->meta.parent_ptr);
			u16 i;

			for (i = 0; i < cnt; i++) {
				u64 cb = RFS_PTR_ADDR(
					n->hints[i].sub_node_ptr);

				if (cb != FR_PTR52)
					ndfr_free_tree(ctx, cb, depth + 1);
			}
		}
		ctx->ops->node_unpin(ctx->host, root, buf, false);
	}
	return ctx->ops->node_free(ctx->host, root);
}

int rfs_ndfr_release(struct rfs_ndfr_ctx *ctx, struct raven_dir_Inode_t *dino)
{
	u64 ctl = dino->name_data_free_ranges_ctl;
	u64 ptr = RFS_ND_FR_CTL_PTR(ctl);

	if (ptr == RFS_ND_FR_CTL_PTR_INVALID ||
	    RFS_ND_FR_CTL_IS_INLINE(ctl)) {
		fr_memset(FR_INLINE_TAB(dino), 0,
			  sizeof(fr_t) * RFS_ND_FREE_INLINE_MAX);
		dino->name_data_free_ranges_ctl = RFS_ND_FR_CTL_INACTIVE;
		RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count, 0);
		return 0;
	}
	ndfr_free_tree(ctx, ptr, 0);
	dino->name_data_free_ranges_ctl = RFS_ND_FR_CTL_INACTIVE;
	RFS_ND_FR_SET_COUNT(dino->name_data_free_ranges_count, 0);
	return 0;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * ravenfs_allocator_slice.c —— 分片自治单元 struct rfs_slice 实现（⇄ god mgr_slice_t）
 *
 * 逐方法对照 `juvenile_god/fs_allocator.cpp` 的 mgr_slice_t 真实现翻译；契约注释见
 * `include/ravenfs/fs_allocator.hpp`。仅把 STL/Boost 两棵树换成 ko 自造的
 * `ravenfs_rbtree_intrusive.h` 侵入树、把树节点分配外抛 `struct rfs_slice_alloc`
 * （god 的 std::set 自持节点），其余语义逐条对齐。
 *
 * 双挂纪律（逐指令边界维持「从 ⊆ 主」）：
 *   挂：主树先 → 从树后（iv_link_both）；
 *   摘：从树先 → 主树后（iv_erase_both）。
 *
 * C(gnu11) 与 C++(c++17) 双编 -Wall -Wextra -Werror 均须 exit 0（void* 显式 cast）。
 */
#include "ravenfs_allocator_slice.h"

/* ══════════════════════════ 内部比较器（不对外暴露）══════════════════════════ */

/* 主树 by_pos：严格区间序（对两两不重叠区间集 = base 升序；相交 → cmp==0 → -EEXIST）。*/
static int iv_cmp_pos(const struct rfs_rb_node *a, const struct rfs_rb_node *b,
		      void *ctx)
{
	const struct rfs_interval *x =
		container_of(a, struct rfs_interval, by_pos);
	const struct rfs_interval *y =
		container_of(b, struct rfs_interval, by_pos);
	u64 x_end = x->base + x->len;
	u64 y_end = y->base + y->len;

	(void)ctx;
	if (x_end <= y->base)		/* x 整体在 y 之前（可相邻）*/
		return -1;
	if (y_end <= x->base)		/* y 整体在 x 之前 */
		return 1;
	return 0;			/* 相交非空 → 等键 */
}

/* 从树 by_size：(len, base) 字典序（严格）。*/
static int iv_cmp_size(const struct rfs_rb_node *a, const struct rfs_rb_node *b,
		       void *ctx)
{
	const struct rfs_interval *x =
		container_of(a, struct rfs_interval, by_size);
	const struct rfs_interval *y =
		container_of(b, struct rfs_interval, by_size);

	(void)ctx;
	if (x->len != y->len)
		return (x->len < y->len) ? -1 : 1;
	if (x->base != y->base)
		return (x->base < y->base) ? -1 : 1;
	return 0;
}

/* ══════════════════════════ 内部轮子：双树成对操作 ══════════════════════════ */

/* 把已填好 base/len 的区间挂进双树（主树先、从树后）。失败时不还节点（调用方负责）。*/
static int iv_link_both(struct rfs_slice *self, struct rfs_interval *iv)
{
	int rc = rfs_rb_insert(&self->pos, &iv->by_pos, iv_cmp_pos, NULL);

	if (rc)
		return rc;
	rc = rfs_rb_insert(&self->size, &iv->by_size, iv_cmp_size, NULL);
	if (rc) {			/* 主树已挂、从树失败 → 回滚主树 */
		rfs_rb_erase(&self->pos, &iv->by_pos);
		return rc;
	}
	RFS_AL_STAT1(self->stat, AL_TREE_INSERT);	/* 双挂成功（从 ⊆ 主）*/
	return 0;
}

/* 取一个新区间节点挂进双树（取池 + link；失败还节点）。*/
static int iv_insert_both(struct rfs_slice *self, u64 base, u64 len)
{
	struct rfs_interval *iv = (struct rfs_interval *)
		self->alloc.alloc(self->alloc.ctx, (u64)sizeof(*iv));
	int rc;

	if (!iv) {
		RFS_AL_STAT1(self->stat, AL_POOL_FAIL);	/* 元素池取节点失败 */
		return -ENOMEM;
	}
	RFS_AL_STAT1(self->stat, AL_POOL_ALLOC);
	iv->base = base;
	iv->len = len;
	rc = iv_link_both(self, iv);
	if (rc)
		self->alloc.free(self->alloc.ctx, iv);
	return rc;
}

/* 从双树摘除并归还节点（从树先、主树后）。*/
static void iv_erase_both(struct rfs_slice *self, struct rfs_interval *iv)
{
	rfs_rb_erase(&self->size, &iv->by_size);
	rfs_rb_erase(&self->pos, &iv->by_pos);
	self->alloc.free(self->alloc.ctx, iv);
	RFS_AL_STAT1(self->stat, AL_TREE_ERASE);
}

/* 拆空双树并归还全部节点（enable 清底 / 异常回滚 / shutdown 共用）。*/
static void iv_free_all(struct rfs_slice *self)
{
	while (!rfs_rb_empty(&self->pos)) {
		struct rfs_rb_node *n = rfs_rb_first(&self->pos);
		struct rfs_interval *iv =
			container_of(n, struct rfs_interval, by_pos);

		iv_erase_both(self, iv);
	}
}

/* 定位「可能包含 base 或其后第一个」的区间：首个 end > base 者。
 * 严格序下它 = 包含 base 的区间（base ∈ [iv.base, iv.end)），否则 = 第一个
 * base' ≥ base 的区间；无则 NULL。探针只填 base/len，比较器不读探针链接字段。*/
static struct rfs_interval *iv_covering_or_after(struct rfs_slice *self, u64 base)
{
	struct rfs_interval probe;
	struct rfs_rb_node *n;

	probe.base = base;
	probe.len = 0;
	n = rfs_rb_lower_bound(&self->pos, &probe.by_pos, iv_cmp_pos, NULL);
	return n ? container_of(n, struct rfs_interval, by_pos) : NULL;
}

/* ══════════════════════════ enable / shutdown ══════════════════════════ */

int rfs_slice_enable(struct rfs_slice *self, const struct rfs_slice_cfg *cfg)
{
	const u8 *bits;
	u64 i;

	if (!self || !cfg)
		return -EINVAL;
	if (self->is_workable)			/* 已使能：拒绝重复 enable */
		return -EINVAL;
	if (!cfg->slice_base)			/* base 非空 */
		return -EINVAL;
	/* 4K 对齐（用 unsigned long 做指针→整数转换：内核对 uintptr_t 无保证）*/
	if (((unsigned long)cfg->slice_base) & 4095UL)
		return -EINVAL;
	if (cfg->slice_bitscount == 0)		/* 空片无意义 */
		return -EINVAL;
	if (!cfg->alloc.alloc || !cfg->alloc.free)
		return -EINVAL;			/* 元素池必填 */

	self->alloc = cfg->alloc;		/* 内部化分配器（跨调用保留）*/
	self->stat = cfg->stat;			/* 内部化 al_ 计数钩子（跨调用保留）*/
	iv_free_all(self);			/* 清底：挡上次失败残留 */
	rfs_rb_init(&self->pos);
	rfs_rb_init(&self->size);
	self->in_global_base_idx = cfg->in_global_base_idx;
	self->slice_bitscount = cfg->slice_bitscount;
	self->free_count = 0;
	self->last_finish_hint = 0;

	bits = cfg->slice_base;
	/* 逐段扫出极大空闲 run（连续 0）；每个 run 双挂主从树。*/
	i = 0;
	while (i < cfg->slice_bitscount) {
		u64 start, run_len;
		int rc;

		if ((bits[i >> 3] >> (i & 7)) & 1u) {	/* 1 = 占用 → 跳过 */
			i++;
			continue;
		}
		start = i;
		while (i < cfg->slice_bitscount &&
		       (((bits[i >> 3] >> (i & 7)) & 1u) == 0))
			i++;				/* 吞掉连续 0 = 极大空闲 run */
		run_len = i - start;
		rc = iv_insert_both(self, self->in_global_base_idx + start,
				    run_len);
		if (rc) {				/* 异常回滚：退回未使能 + 空树 */
			iv_free_all(self);
			self->free_count = 0;
			self->last_finish_hint = 0;
			self->in_global_base_idx = 0;
			self->slice_bitscount = 0;
			return rc;
		}
		self->free_count += run_len;
	}

	/* bits 在此之后不再被引用（配置内存即刻可弃）。*/
	self->is_workable = true;
	return 0;
}

int rfs_slice_shutdown(struct rfs_slice *self)
{
	if (!self)
		return -EINVAL;
	if (!self->is_workable)			/* 初始化态：无可 shutdown */
		return -EINVAL;

	iv_free_all(self);			/* 拆双树 + 还全部区间节点 */
	self->free_count = 0;
	self->last_finish_hint = 0;
	self->in_global_base_idx = 0;		/* 对位：清内部化字段 */
	self->slice_bitscount = 0;
	self->is_workable = false;
	return 0;
}

/* ══════════════════════════ free_count ══════════════════════════ */

u64 rfs_slice_free_count(const struct rfs_slice *self)
{
	return self ? self->free_count : 0;
}

/* ══════════════════════════ 范围校验（test / free 共用）══════════════════════════ */

/* [base, base+len) 是否落在本片全局半开区间内；越界返 -EINVAL，合法返 0。*/
static int iv_check_range(const struct rfs_slice *self, u64 base, u64 len)
{
	u64 off;

	if (base < self->in_global_base_idx)
		return -EINVAL;
	off = base - self->in_global_base_idx;
	/* ⚠ 先判 off 上界：base 越过片尾时 `slice_bitscount - off` 无符号下溢会令检查失效 */
	if (off > self->slice_bitscount || len > self->slice_bitscount - off)
		return -EINVAL;
	return 0;
}

/* ══════════════════════════ test_interval ══════════════════════════ */

int rfs_slice_test_interval(struct rfs_slice *self, bool val, u64 base, u64 len)
{
	struct rfs_interval *iv;
	int rc;

	if (!self)
		return -EINVAL;
	if (!self->is_workable)
		return -EINVAL;
	rc = iv_check_range(self, base, len);
	if (rc)
		return rc;
	if (len == 0)				/* 空段恒真（退化）*/
		return 0;

	iv = iv_covering_or_after(self, base);

	if (!val) {
		/* 期望全空闲 ⟺ 整段被同一个空闲区间覆盖（区间两两不重叠且极大）。*/
		if (iv && iv->base <= base && base + len <= iv->base + iv->len)
			return 0;
		return -ENOENT;
	}
	/* 期望全占用 ⟺ 段内无任何空闲位：首个 end>base 的区间若左端落在段内即命中。*/
	if (iv && iv->base < base + len)
		return -ENOENT;
	return 0;
}

/* ══════════════════════════ free_interval ══════════════════════════ */

int rfs_slice_free_interval(struct rfs_slice *self, u64 base, u64 len)
{
	struct rfs_interval *it, *left = NULL;
	struct rfs_rb_node *leftn;
	bool merge_left, merge_right;
	u64 new_base, new_end;
	int rc;

	if (!self)
		return -EINVAL;
	if (!self->is_workable)
		return -EINVAL;
	rc = iv_check_range(self, base, len);
	if (rc)
		return rc;
	if (len == 0)				/* 空释放：无操作 */
		return 0;
	RFS_AL_STAT1(self->stat, AL_SLICE_FREE);

	it = iv_covering_or_after(self, base);	/* 首个 end>base 者 */
	if (it)
		leftn = rfs_rb_prev(&it->by_pos);	/* 左邻 = 中序前驱 */
	else
		leftn = rfs_rb_last(&self->pos);	/* 无后继 → 左邻 = 最右 */
	if (leftn)
		left = container_of(leftn, struct rfs_interval, by_pos);

	/* ── 重叠检测（守卫主树不变式）── */
	if (left && left->base + left->len > base) {
		RFS_AL_STAT1(self->stat, AL_SLICE_FREE_EEXIST);	/* 左邻伸进释放区间 */
		return -EEXIST;
	}
	if (it && it->base < base + len) {
		RFS_AL_STAT1(self->stat, AL_SLICE_FREE_EEXIST);	/* 右邻（或含 base 者）左端落在区间内 */
		return -EEXIST;
	}

	/* ── 融合边界（只并纯邻接）── */
	merge_left = left && (left->base + left->len == base);
	merge_right = it && (it->base == base + len);
	/* 融合行为计数（诊断碎片：邻接合并是否真发生；P1 只记不改流）。 */
	if (merge_left && merge_right)
		RFS_AL_STAT1(self->stat, AL_MERGE_BOTH);
	else if (merge_left)
		RFS_AL_STAT1(self->stat, AL_MERGE_LEFT);
	else if (merge_right)
		RFS_AL_STAT1(self->stat, AL_MERGE_RIGHT);
	new_base = merge_left ? left->base : base;
	new_end = merge_right ? (it->base + it->len) : (base + len);

	/* ── 先删后插（从树先主树后 → 主树先从树后）── */
	if (merge_left)
		iv_erase_both(self, left);
	if (merge_right)
		iv_erase_both(self, it);

	rc = iv_insert_both(self, new_base, new_end - new_base);
	if (rc)
		return rc;			/* 分配失败（god 抛 bad_alloc 对位）*/

	self->free_count += len;
	return 0;
}

/* ══════════════════════════ free_intervals ══════════════════════════ */

int rfs_slice_free_intervals(struct rfs_slice *self, struct rfs_run_list *runs)
{
	if (!self || !runs)
		return -EINVAL;
	if (!self->is_workable)
		return -EINVAL;

	/* 逐段消费：先摘链 + 还节点（取走即还），再 free_interval。
	 * 遇错：已消费段不回滚，余段留在链上供调用方续查。*/
	while (runs->head) {
		struct rfs_run_node *node = runs->head;
		u64 base = node->run.base;
		u64 len = node->run.len;
		int rc;

		runs->head = node->next;
		runs->alloc.free(runs->alloc.ctx, node);
		rc = rfs_slice_free_interval(self, base, len);
		if (rc)
			return rc;
	}
	return 0;
}

/* ══════════════════════════ all_dump ══════════════════════════ */

int rfs_slice_all_dump(struct rfs_slice *self, struct rfs_run_dlist *out_runs)
{
	struct rfs_rb_node *n;

	if (!self || !out_runs)
		return -EINVAL;
	if (!self->is_workable)
		return -EINVAL;

	/* 主树中序 = base 递增序（两两不重叠）。纯内存零 IO。*/
	for (n = rfs_rb_first(&self->pos); n; n = rfs_rb_next(n)) {
		struct rfs_interval *iv =
			container_of(n, struct rfs_interval, by_pos);
		int rc = rfs_run_dlist_push_back(out_runs, iv->base, iv->len);

		if (rc)
			return rc;		/* 节点分配失败（god 抛 bad_alloc 对位）*/
	}
	return 0;
}

/* ══════════════════════════ soft_occupy ══════════════════════════ */

int rfs_slice_soft_occupy(struct rfs_slice *self, u64 base, u64 len,
			  u64 *rest_bits)
{
	struct rfs_interval *iv;
	u64 real, slice_end, iv_end, claimed;
	u64 left_base, left_len, right_base, right_len;
	int rc;

	if (!self || !rest_bits)
		return -EINVAL;
	if (!self->is_workable)
		return -EINVAL;
	if (*rest_bits > len)			/* 口径防御：已满足量不能超需求 */
		return -EINVAL;
	if (len == 0)				/* 无事可做（rest 不动）*/
		return 0;
	real = len - *rest_bits;		/* 本锚点目标上限 */
	if (real == 0)				/* 已全满足 */
		return 0;
	if (base < self->in_global_base_idx)	/* 片首之下 */
		return -EINVAL;
	slice_end = self->in_global_base_idx + self->slice_bitscount;
	if (base > slice_end)			/* 片尾之上（base==片尾 → 无可占领）*/
		return -EINVAL;

	/* 定位「可能包含 base」的空闲区间；base 不在空闲区间内 → 锚点即占用。*/
	iv = iv_covering_or_after(self, base);
	if (!iv || iv->base > base) {
		*rest_bits = len;
		return -ENOSPC;
	}

	iv_end = iv->base + iv->len;		/* base ∈ [iv.base, iv.end) */
	claimed = iv_end - base;		/* 可用连续空闲前缀长（≥1）*/
	if (claimed > real)
		claimed = real;			/* 额度上限夹断 */
	if (base + claimed > slice_end)
		claimed = slice_end - base;	/* 片尾夹断（严格永不越片；base<slice_end ⇒ claimed≥1）*/

	left_base = iv->base;			/* 残余左段 [iv.base, base) */
	left_len = base - left_base;
	right_base = base + claimed;		/* 残余右段 [base+claimed, iv.end) */
	right_len = iv_end - right_base;

	/* 先删原区间（从树先主树后），再按残余插回（主树先从树后）。*/
	iv_erase_both(self, iv);
	if (left_len > 0) {
		rc = iv_insert_both(self, left_base, left_len);
		if (rc)
			return rc;
	}
	if (right_len > 0) {
		rc = iv_insert_both(self, right_base, right_len);
		if (rc)
			return rc;
	}

	self->free_count -= claimed;
	if (base + claimed > self->last_finish_hint)/* 只进不退 */
		self->last_finish_hint = base + claimed;
	*rest_bits = len - claimed;
	return 0;
}

/* ══════════════════════════ alloc_hard ══════════════════════════ */

int rfs_slice_alloc_hard(struct rfs_slice *self, u8 align_log2, u64 bitscount,
			 u64 *out_base_idx)
{
	struct rfs_interval probe;
	struct rfs_rb_node *n;
	u64 align_mask;

	if (!self)
		return -EINVAL;
	if (!self->is_workable)
		return -EINVAL;
	if (bitscount == 0)
		return -EINVAL;
	if (align_log2 > 63)			/* 对齐参数非法 */
		return -EINVAL;
	if (!out_base_idx)
		return -EINVAL;
	RFS_AL_STAT1(self->stat, AL_SLICE_ALLOC_HARD);

	align_mask = (align_log2 == 0) ? 0ULL : ((1ULL << align_log2) - 1ULL);

	/* best-fit：从树 (len,base) 升序，首个 len≥bitscount 且对齐可行的区间即选。
	 * 对齐可能让候选容不下 → 沿 len 非降序继续试，首个可行者即 best-fit。*/
	probe.base = 0;
	probe.len = bitscount;
	for (n = rfs_rb_lower_bound(&self->size, &probe.by_size, iv_cmp_size, NULL);
	     n; n = rfs_rb_next(n)) {
		struct rfs_interval *iv =
			container_of(n, struct rfs_interval, by_size);
		u64 iv_base = iv->base, iv_len = iv->len;
		u64 iv_end = iv_base + iv_len;
		u64 s, left_len, right_base, right_len;
		int rc;

		/* 树按 (len,base) 升序，lower_bound 起 len 恒 ≥ bitscount */

		/* 头部上挖起点（尽量贴头 + 对齐）；越出区间右沿 → 本区间容不下。
		 * D-14：头挖（非尾挖）→ 连续分配物理**升序**，令锚定（取左邻
		 * 上沿，iomap.c）与叶内四形态融合（升序相邻，extent_write.c）成立。*/
		s = (iv_base + align_mask) & ~align_mask;
		if (s + bitscount > iv_end)
			continue;

		iv_erase_both(self, iv);	/* 先删（从树先主树后）*/

		left_len = s - iv_base;		/* 头余（仅对齐留白）*/
		right_base = s + bitscount;	/* 右余 [s+bitscount, iv.end) */
		right_len = iv_end - right_base;
		if (left_len > 0) {
			rc = iv_insert_both(self, iv_base, left_len);
			if (rc)
				return rc;
		}
		if (right_len > 0) {
			rc = iv_insert_both(self, right_base, right_len);
			if (rc)
				return rc;
		}

		self->free_count -= bitscount;
		if (right_base > self->last_finish_hint)/* 只进不退 */
			self->last_finish_hint = right_base;
		*out_base_idx = s;
		return 0;
	}
	RFS_AL_STAT1(self->stat, AL_SLICE_ALLOC_ENOSPC);
	return -ENOSPC;			/* 满片：原子失败，不部分分配 */
}

/* ══════════════════════════ soft_alloc ══════════════════════════ */

int rfs_slice_soft_alloc(struct rfs_slice *self, u8 align_log2, u64 bitscount,
			 struct rfs_run_list *out_runs, bool *all_satisfied)
{
	u64 align_mask, remain, taken_total = 0;
	struct rfs_rb_node *n;

	if (!self || !out_runs || !all_satisfied)
		return -EINVAL;
	if (!self->is_workable)
		return -EINVAL;
	if (align_log2 > 63)
		return -EINVAL;
	*all_satisfied = false;
	if (bitscount == 0)			/* 0 请求视为非法 */
		return -EINVAL;
	RFS_AL_STAT1(self->stat, AL_SLICE_SOFT);

	align_mask = (align_log2 == 0) ? 0ULL : ((1ULL << align_log2) - 1ULL);
	remain = bitscount;

	/* 碎片优先：从树小端（最小碎片）起逐段消费；大块（尾巴）留给 alloc_hard。*/
	n = rfs_rb_first(&self->size);
	while (n && remain > 0) {
		struct rfs_interval *iv =
			container_of(n, struct rfs_interval, by_size);
		u64 iv_base = iv->base, iv_len = iv->len;
		u64 iv_end = iv_base + iv_len;
		u64 t, s, left_len, right_base, right_len;
		int rc;

		/* D-14：头挖（尽量贴头 + 对齐），与 alloc_hard 同向 → 连续分配升序。*/
		s = (iv_base + align_mask) & ~align_mask;
		if (s >= iv_end) {			/* 本段无可占 → 跳过 */
			n = rfs_rb_next(n);
			continue;
		}
		{
			u64 avail = iv_end - s;

			t = (avail < remain) ? avail : remain;
		}

		iv_erase_both(self, iv);	/* 先删（从树先主树后）*/
		left_len = s - iv_base;		/* 头余（仅对齐留白）*/
		right_base = s + t;		/* 右余 [s+t, iv.end) */
		right_len = iv_end - right_base;
		if (left_len > 0) {
			rc = iv_insert_both(self, iv_base, left_len);
			if (rc)
				return rc;
		}
		if (right_len > 0) {
			rc = iv_insert_both(self, right_base, right_len);
			if (rc)
				return rc;
		}

		/* 先扣账、再 push_front：挖段已从树摘除但此前未扣账，故**必须先扣**；
		 * 失败回滚 rfs_slice_free_interval 会 += t，与上式相抵（否则净多记 t）。*/
		self->free_count -= t;
		rc = rfs_run_list_push_front(out_runs, s, t);
		if (rc) {
			(void)rfs_slice_free_interval(self, s, t);	/* += t，与上式相抵 */
			return rc;
		}

		taken_total += t;
		remain -= t;
		if (right_base > self->last_finish_hint)/* 只进不退 */
			self->last_finish_hint = right_base;

		n = rfs_rb_first(&self->size);	/* 余段可能更小 → 重取最小 */
	}

	if (taken_total == 0) {			/* 一位未扣：原子空手 */
		RFS_AL_STAT1(self->stat, AL_SLICE_SOFT_ENOSPC);
		return -ENOSPC;
	}
	*all_satisfied = (remain == 0);		/* 分片级是否已满足 */
	return 0;				/* 允许部分交付 */
}

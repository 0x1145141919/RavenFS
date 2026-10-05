// SPDX-License-Identifier: GPL-2.0
/*
 * inode_data_plane.c —— 全局 inode 数据面 struct rfs_inode_plane 实现
 *                       （⇄ god `inode_data_plane_t`，juvenile_god/fs_allocator.cpp）
 *
 * 纯内存：翻译面（interval 数组，key=lblk）+ 空闲面（主从双侵入红黑树，inode 单位）。
 * 盘上装载 / 落盘（映射 B+tree / 扁 fr 快照 / SB 四字段）归 FS 适配层
 * （allocator_adapter.c）；本文件零块 I/O。
 *
 * 锁序（硬，见头注）：fr 写锁 > 翻译写锁 > block 片锁；禁止翻译→空间。
 *   · locate        = 翻译读
 *   · alloc         = 空间写（用尽 → grow：空间写内取翻译写 + block 片锁）
 *   · free / test   = 空间写 / 空间读
 * 分配节点走 kzalloc(GFP_NOFS)：临界区在可睡 rw_semaphore 内，无自旋内存分配。
 */
#include "ravenfs_allocator_top.h"	/* struct rfs_allocator + block_alloc_hard/free 声明 */
#include <linux/slab.h>
#include <linux/string.h>

/* ══════════════════════════ 比较器（不对外暴露）══════════════════════════ */

/* 主树 by_pos：严格区间序（两两不重叠 ⇒ base 升序；相交 → cmp==0）。 */
static int ip_cmp_pos(const struct rfs_rb_node *a, const struct rfs_rb_node *b,
		      void *ctx)
{
	const struct rfs_ip_interval *x =
		container_of(a, struct rfs_ip_interval, by_pos);
	const struct rfs_ip_interval *y =
		container_of(b, struct rfs_ip_interval, by_pos);
	u64 x_end = x->base + x->len;
	u64 y_end = y->base + y->len;

	(void)ctx;
	if (x_end <= y->base)
		return -1;
	if (y_end <= x->base)
		return 1;
	return 0;
}

/* 从树 by_size：(len, base) 字典序。 */
static int ip_cmp_size(const struct rfs_rb_node *a, const struct rfs_rb_node *b,
		       void *ctx)
{
	const struct rfs_ip_interval *x =
		container_of(a, struct rfs_ip_interval, by_size);
	const struct rfs_ip_interval *y =
		container_of(b, struct rfs_ip_interval, by_size);

	(void)ctx;
	if (x->len != y->len)
		return (x->len < y->len) ? -1 : 1;
	if (x->base != y->base)
		return (x->base < y->base) ? -1 : 1;
	return 0;
}

/* ══════════════════════════ 双树成对操作 ══════════════════════════ */

/* 挂：主树先 → 从树后；从树失败回滚主树。失败时不还节点（调用方负责）。 */
static int ip_link_both(struct rfs_inode_plane *p, struct rfs_ip_interval *iv)
{
	int rc = rfs_rb_insert(&p->fr_pos, &iv->by_pos, ip_cmp_pos, NULL);

	if (rc)
		return rc;
	rc = rfs_rb_insert(&p->fr_size, &iv->by_size, ip_cmp_size, NULL);
	if (rc) {
		rfs_rb_erase(&p->fr_pos, &iv->by_pos);
		return rc;
	}
	return 0;
}

/* 取一个新区间节点挂双树（取节点 + link；失败还节点）。 */
static int ip_insert_both(struct rfs_inode_plane *p, u64 base, u64 len)
{
	struct rfs_ip_interval *iv = kzalloc(sizeof(*iv), GFP_NOFS);
	int rc;

	if (!iv)
		return -ENOMEM;
	iv->base = base;
	iv->len = len;
	rc = ip_link_both(p, iv);
	if (rc)
		kfree(iv);
	return rc;
}

/* 摘：从树先 → 主树后；还节点。 */
static void ip_erase_both(struct rfs_inode_plane *p, struct rfs_ip_interval *iv)
{
	rfs_rb_erase(&p->fr_size, &iv->by_size);
	rfs_rb_erase(&p->fr_pos, &iv->by_pos);
	kfree(iv);
}

/* 拆空双树并归还全部节点。 */
static void ip_free_all(struct rfs_inode_plane *p)
{
	while (!rfs_rb_empty(&p->fr_pos)) {
		struct rfs_rb_node *n = rfs_rb_first(&p->fr_pos);
		struct rfs_ip_interval *iv =
			container_of(n, struct rfs_ip_interval, by_pos);

		ip_erase_both(p, iv);
	}
}

/* 首个 end > base 的区间（= 可能覆盖 base 者，否则其后第一个）；无则 NULL。
 * 探针只填 base/len，比较器不读侵入链接字段。 */
static struct rfs_ip_interval *ip_covering_or_after(struct rfs_inode_plane *p,
						    u64 base)
{
	struct rfs_ip_interval probe;
	struct rfs_rb_node *n;

	probe.base = base;
	probe.len = 0;
	n = rfs_rb_lower_bound(&p->fr_pos, &probe.by_pos, ip_cmp_pos, NULL);
	return n ? container_of(n, struct rfs_ip_interval, by_pos) : NULL;
}

/* 是否已含 inum（幂等判定 / test）。须持 fr 锁。 */
static bool ip_contains_locked(struct rfs_inode_plane *p, u64 inum)
{
	struct rfs_ip_interval *iv = ip_covering_or_after(p, inum);

	return iv && iv->base <= inum && inum < iv->base + iv->len;
}

/* ══════════════════════════ grow（须持空间写锁）══════════════════════════
 * 先 block 硬分配（2k 块、8K 对齐，k 逐级折半降至 1）→ 翻译写锁内注入 extent
 * （物理尾邻接则扩尾）→ 新片 32k 个 inode 入 fr。失败回滚退片。 */
static int ip_grow_locked(struct rfs_inode_plane *p)
{
	u32 k = p->grow_batch ? p->grow_batch : 1;
	u64 blk = (u64)PTR_INVALID;
	u64 new_lblk, old_pieces;
	bool merged = false;

	for (;;) {
		u64 b = rfs_allocator_block_alloc_hard(p->owner, (u64)k * 2ULL, 1);

		if (b != (u64)PTR_INVALID) {
			blk = b;
			break;
		}
		if (k <= 1)
			break;
		k >>= 1;
		if (k == 0)
			k = 1;
	}
	if (blk == (u64)PTR_INVALID)
		return -ENOSPC;

	new_lblk = p->pieces * 2ULL;
	down_write(&p->extent_lock);
	if (p->ext_n == p->ext_cap) {
		u64 ncap = p->ext_cap ? p->ext_cap * 2ULL : 8ULL;
		struct rfs_inode_extent *ne =
			krealloc(p->ext, (size_t)ncap * sizeof(*ne), GFP_NOFS);

		if (!ne) {
			up_write(&p->extent_lock);
			(void)rfs_allocator_block_free(p->owner, blk, (u64)k * 2ULL);
			return -ENOMEM;
		}
		p->ext = ne;
		p->ext_cap = ncap;
	}
	if (p->ext_n) {
		struct rfs_inode_extent *last = &p->ext[p->ext_n - 1];

		if (last->lblk + last->length == new_lblk &&
		    last->phys_base + last->length == blk) {
			last->length += (u64)k * 2ULL;
			merged = true;
		}
	}
	if (!merged) {
		p->ext[p->ext_n].lblk = new_lblk;
		p->ext[p->ext_n].phys_base = blk;
		p->ext[p->ext_n].length = (u64)k * 2ULL;
		p->ext_n++;
	}
	old_pieces = p->pieces;
	p->pieces += k;
	up_write(&p->extent_lock);

	if (ip_insert_both(p, old_pieces * RFS_IDP_INODES_PER_PIECE,
			   (u64)k * RFS_IDP_INODES_PER_PIECE) == 0)
		p->fr_free += (u64)k * RFS_IDP_INODES_PER_PIECE;
	else
		RFS_LOG("ravenfs: inode plane grow: fr 插入失败（inode 泄漏 k=%u）\n", k);
	RFS_AL_STAT1(p->owner->stat, AL_IDP_GROW);
	RFS_AL_STAT(p->owner->stat, AL_IDP_GROW_PIECES, k);
	return 0;
}

/* ══════════════════════════ 生命周期 ══════════════════════════ */

int rfs_inode_plane_init(struct rfs_inode_plane *p,
			 const struct rfs_inode_plane_cfg *cfg,
			 struct rfs_allocator *owner)
{
	u64 expect = 0, total, i;

	if (!p || !cfg || !owner)
		return -EINVAL;
	memset(p, 0, sizeof(*p));
	init_rwsem(&p->extent_lock);
	init_rwsem(&p->fr_lock);
	rfs_rb_init(&p->fr_pos);
	rfs_rb_init(&p->fr_size);
	p->owner = owner;
	p->grow_batch = cfg->grow_batch_pieces ? cfg->grow_batch_pieces : 1;

	if (cfg->pieces == 0 || !cfg->extents || cfg->extent_count == 0)
		return -EINVAL;

	/* ── 翻译面：稠密无洞无重叠 + 2 对齐 + Σ == pieces×2 ── */
	p->ext = kcalloc((size_t)cfg->extent_count, sizeof(*p->ext), GFP_KERNEL);
	if (!p->ext)
		return -ENOMEM;
	p->ext_cap = cfg->extent_count;
	for (i = 0; i < cfg->extent_count; i++) {
		const struct rfs_inode_extent *e = &cfg->extents[i];

		if (e->length == 0 || (e->length & 1ULL) ||
		    (e->lblk & 1ULL) || (e->phys_base & 1ULL))
			goto bad;
		if (e->lblk != expect)
			goto bad;
		p->ext[i] = *e;
		expect = e->lblk + e->length;
	}
	if (expect != cfg->pieces * 2ULL)
		goto bad;
	p->ext_n = cfg->extent_count;
	p->pieces = cfg->pieces;

	/* ── 空闲面：范围 / idx0 / 重叠校验 + 双挂 ── */
	total = p->pieces * RFS_IDP_INODES_PER_PIECE;
	for (i = 0; i < cfg->fr_count; i++) {
		const struct inode_dataplane_coordinate_free_range_t f = cfg->fr[i];
		struct rfs_ip_interval *it;
		struct rfs_rb_node *leftn;
		struct rfs_ip_interval *left;

		if (f.len == 0)
			continue;
		if (f.base == 0)			/* idx0 归 root */
			goto bad;
		if (f.base >= total || f.len > total - f.base)
			goto bad;
		it = ip_covering_or_after(p, f.base);
		if (it && it->base < f.base + f.len)
			goto bad;
		leftn = it ? rfs_rb_prev(&it->by_pos) : rfs_rb_last(&p->fr_pos);
		left = leftn
			? container_of(leftn, struct rfs_ip_interval, by_pos) : NULL;
		if (left && left->base + left->len > f.base)
			goto bad;
		if (ip_insert_both(p, f.base, f.len))
			goto bad;
		p->fr_free += f.len;
	}

	p->active = true;
	return 0;
bad:
	rfs_inode_plane_fini(p);
	return -EINVAL;
}

void rfs_inode_plane_fini(struct rfs_inode_plane *p)
{
	if (!p)
		return;
	ip_free_all(p);
	rfs_rb_init(&p->fr_pos);
	rfs_rb_init(&p->fr_size);
	kfree(p->ext);
	p->ext = NULL;
	p->ext_n = 0;
	p->ext_cap = 0;
	p->fr_free = 0;
	p->pieces = 0;
	p->active = false;
}

/* ══════════════════════════ 分配 / 释放 ══════════════════════════ */

int rfs_inode_plane_alloc(struct rfs_inode_plane *p, u64 *out_inum)
{
	struct rfs_rb_node *n;
	struct rfs_ip_interval *iv;
	u64 b;

	if (!p || !p->active || !out_inum)
		return -EINVAL;
	down_write(&p->fr_lock);
	if (rfs_rb_empty(&p->fr_size)) {
		int rc = ip_grow_locked(p);

		if (rc) {
			up_write(&p->fr_lock);
			return rc;
		}
	}
	n = rfs_rb_first(&p->fr_size);
	if (!n) {
		up_write(&p->fr_lock);
		return -ENOSPC;
	}
	iv = container_of(n, struct rfs_ip_interval, by_size);
	b = iv->base;
	rfs_rb_erase(&p->fr_size, &iv->by_size);
	rfs_rb_erase(&p->fr_pos, &iv->by_pos);
	if (iv->len > 1) {
		iv->base = b + 1;
		iv->len -= 1;
		if (ip_link_both(p, iv))
			kfree(iv);		/* 极端：余段丢失 */
	} else {
		kfree(iv);
	}
	p->fr_free -= 1;
	*out_inum = b;
	up_write(&p->fr_lock);
	return 0;
}

void rfs_inode_plane_free(struct rfs_inode_plane *p, u64 inum)
{
	struct rfs_ip_interval *it;
	struct rfs_rb_node *leftn;
	struct rfs_ip_interval *left;
	bool ml, mr;

	if (!p || !p->active || inum == 0)	/* root 永不释放 */
		return;
	down_write(&p->fr_lock);
	if (inum >= p->pieces * RFS_IDP_INODES_PER_PIECE) {
		up_write(&p->fr_lock);
		return;
	}
	if (ip_contains_locked(p, inum)) {	/* 幂等：重复释放忽略 */
		up_write(&p->fr_lock);
		return;
	}

	it = ip_covering_or_after(p, inum);
	leftn = it ? rfs_rb_prev(&it->by_pos) : rfs_rb_last(&p->fr_pos);
	left = leftn ? container_of(leftn, struct rfs_ip_interval, by_pos) : NULL;
	ml = left && (left->base + left->len == inum);
	mr = it && (it->base == inum + 1);

	RFS_AL_STAT1(p->owner->stat, AL_IDP_FREE);
	if (ml || mr)
		RFS_AL_STAT1(p->owner->stat, AL_IDP_FR_MERGE);

	if (ml && mr) {
		rfs_rb_erase(&p->fr_size, &left->by_size);
		rfs_rb_erase(&p->fr_pos, &left->by_pos);
		rfs_rb_erase(&p->fr_size, &it->by_size);
		rfs_rb_erase(&p->fr_pos, &it->by_pos);
		left->len = (it->base + it->len) - left->base;
		kfree(it);
		if (ip_link_both(p, left))
			kfree(left);
	} else if (ml) {
		rfs_rb_erase(&p->fr_size, &left->by_size);
		rfs_rb_erase(&p->fr_pos, &left->by_pos);
		left->len += 1;
		if (ip_link_both(p, left))
			kfree(left);
	} else if (mr) {
		rfs_rb_erase(&p->fr_size, &it->by_size);
		rfs_rb_erase(&p->fr_pos, &it->by_pos);
		it->base = inum;
		it->len += 1;
		if (ip_link_both(p, it))
			kfree(it);
	} else {
		(void)ip_insert_both(p, inum, 1);
	}
	p->fr_free += 1;
	up_write(&p->fr_lock);
}

bool rfs_inode_plane_test(struct rfs_inode_plane *p, u64 inum)
{
	bool alloc;

	if (!p || !p->active)
		return false;
	down_read(&p->fr_lock);
	alloc = inum < p->pieces * RFS_IDP_INODES_PER_PIECE &&
		!ip_contains_locked(p, inum);
	up_read(&p->fr_lock);
	return alloc;
}

/* ══════════════════════════ 翻译 ══════════════════════════ */

int rfs_inode_plane_locate(struct rfs_inode_plane *p, u64 inum,
			   u64 *blkbase, u32 *blkoff)
{
	u64 lblk, lo, hi;
	int rc = -ENOENT;

	if (!p || !p->active || !blkbase || !blkoff)
		return -EINVAL;
	lblk = inum >> RFS_IDP_SHIFT_INODE_BLOCK;
	down_read(&p->extent_lock);
	lo = 0;
	hi = p->ext_n;
	while (lo < hi) {			/* 首个 lblk > target */
		u64 mid = lo + (hi - lo) / 2;

		if (p->ext[mid].lblk <= lblk)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo > 0) {
		const struct rfs_inode_extent *e = &p->ext[lo - 1];

		if (lblk - e->lblk < e->length) {
			*blkbase = e->phys_base + ((lblk - e->lblk) & ~1ULL);
			*blkoff = (u32)((inum & (RFS_IDP_INODES_PER_PIECE - 1)) *
					RFS_IDP_INODE_BYTES);
			rc = 0;
		}
	}
	up_read(&p->extent_lock);
	if (rc == 0)
		RFS_AL_STAT1(p->owner->stat, AL_IDP_LOCATE_OK);
	else
		RFS_AL_STAT1(p->owner->stat, AL_IDP_LOCATE_FAIL);
	return rc;
}

/* ══════════════════════════ 几何 / 快照 ══════════════════════════ */

u64 rfs_inode_plane_free_total(struct rfs_inode_plane *p)
{
	u64 v;

	if (!p || !p->active)
		return 0;
	down_read(&p->fr_lock);
	v = p->fr_free;
	up_read(&p->fr_lock);
	return v;
}

u64 rfs_inode_plane_pieces(struct rfs_inode_plane *p)
{
	u64 v;

	if (!p || !p->active)
		return 0;
	down_read(&p->fr_lock);
	v = p->pieces;
	up_read(&p->fr_lock);
	return v;
}

u64 rfs_inode_plane_extent_count(struct rfs_inode_plane *p)
{
	u64 v;

	if (!p || !p->active)
		return 0;
	down_read(&p->extent_lock);
	v = p->ext_n;
	up_read(&p->extent_lock);
	return v;
}

int rfs_inode_plane_dump_extents(struct rfs_inode_plane *p,
				 struct rfs_inode_extent *out, u64 cap)
{
	int rc = 0;

	if (!p || !p->active || !out)
		return -EINVAL;
	RFS_AL_STAT1(p->owner->stat, AL_IDP_TREE_DUMP);
	down_read(&p->extent_lock);
	if (cap < p->ext_n)
		rc = -ENOSPC;
	else
		memcpy(out, p->ext, (size_t)p->ext_n * sizeof(*out));
	up_read(&p->extent_lock);
	return rc;
}

u64 rfs_inode_plane_fr_count(struct rfs_inode_plane *p)
{
	u64 n = 0;

	if (!p || !p->active)
		return 0;
	down_read(&p->fr_lock);
	{
		struct rfs_rb_node *n2;

		for (n2 = rfs_rb_first(&p->fr_pos); n2; n2 = rfs_rb_next(n2))
			n++;
	}
	up_read(&p->fr_lock);
	return n;
}

int rfs_inode_plane_dump_fr(struct rfs_inode_plane *p,
			    struct inode_dataplane_coordinate_free_range_t *out,
			    u64 cap)
{
	struct rfs_rb_node *n2;
	u64 i = 0, cnt = 0;
	int rc = 0;

	if (!p || !p->active || !out)
		return -EINVAL;
	down_read(&p->fr_lock);
	for (n2 = rfs_rb_first(&p->fr_pos); n2; n2 = rfs_rb_next(n2))
		cnt++;
	if (cap < cnt) {
		rc = -ENOSPC;
		goto out;
	}
	for (n2 = rfs_rb_first(&p->fr_pos); n2; n2 = rfs_rb_next(n2)) {
		const struct rfs_ip_interval *iv =
			container_of(n2, struct rfs_ip_interval, by_pos);

		out[i].base = iv->base;
		out[i].len = iv->len;
		i++;
	}
out:
	up_read(&p->fr_lock);
	return rc;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * dentry_write.c —— dentry B+树**写入**：多级 walk + 叶插入/分裂 + 根升长 + 内裂 + hint 精确维护
 *
 * 规格：`Docs/linux/m5-dentry-tree-notes.md`（god `dentry_walk_lookup/dentry_split/dentry_insert_delete` 移植）。
 * 验收（audit 硬校验）：count ≤ 126；is_root/parent_ptr.ptr 正确；叶条目按 (hash,name) 严格递增；
 *   内 hint `hash_max` **精确等于**子树 max 且严格递增；叶链 = 中序（prev/next，两端哨兵 MASK52）；
 *   根节点 ptr = 目录 inode；全节点显式写 ptrs[]（0 是合法块号，禁当空）。
 *
 * 移植要点（坑）：
 *   · 内路由 = lower_bound（`hash <= hash_max`），击穿 → 钻最右；叶 GLB 定插入位（NONE→0）。
 *   · 叶裂：左 63 / 右 64；**先读 victim.ptrs[2]（旧后继）再改写**。
 *   · 升根：**必须显式写** 新根 `ptrs[0]=INV, ptrs[1]=dir_ino(..), ptrs[2]=INV`。
 *   · 52 位哨兵：`RFS_PTR_PACK(0, RFS_MASK52)`。
 *   · 锁纪律：节点读改写 = mdata session get → 改 node → row->dirty → put；块分配（自旋）与会话（可睡眠）分步。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/timekeeping.h>
#include <linux/xxhash.h>

#include "ravenfs.h"

#define RFS_DENTRY_ORDER	126u
#define RFS_WALK_MAX_DEPTH	16u
#define GLB_NONE		0xFFFFu

#define RFS_PTR_INV_V		RFS_PTR_PACK(0, RFS_MASK52)

#define NODE_DIRTY(ms)		do { (ms)->row->dirty = true; (ms)->row->stamp = ktime_get_ns(); } while (0)

static inline void rfs_set_addr(struct ptr_in_idx *p, u64 addr)
{
	p->v = RFS_PTR_PACK(0, addr);
}

static int rfs_meta_ok(const struct Bptree_meta_t *m, u32 *cnt, u32 *inner, u32 *root, u64 *pptr)
{
	if (m->magics[0] != DENTRY_MAGIC1 || m->magics[1] != DENTRY_MAGIC2 ||
	    m->magics[2] != DENTRY_MAGIC3 || m->magics[3] != DENTRY_MAGIC4)
		return -EIO;
	*cnt = RFS_PP_COUNT(m->parent_ptr);
	*inner = RFS_PP_IS_INNER(m->parent_ptr);
	*root = RFS_PP_IS_ROOT(m->parent_ptr);
	*pptr = RFS_PP_PTR(m->parent_ptr);
	if (*cnt > RFS_DENTRY_ORDER)
		return -EIO;			/* 瞬时槽落盘？ */
	return 0;
}

static void rfs_node_init(void *node, u32 count, u32 inner, u32 root, u64 ptr)
{
	struct Bptree_meta_t *m = node;

	memset(node, 0, RAVENFS_NODE_BYTES);
	m->magics[0] = DENTRY_MAGIC1;
	m->magics[1] = DENTRY_MAGIC2;
	m->magics[2] = DENTRY_MAGIC3;
	m->magics[3] = DENTRY_MAGIC4;
	m->parent_ptr.v = RFS_PP_PACK(count, inner, root, ptr);
	m->ptrs[0].v = RFS_PTR_INV_V;
	m->ptrs[1].v = RFS_PTR_INV_V;
	m->ptrs[2].v = RFS_PTR_INV_V;
}

/* 节点子树 max hash（叶取末条目 hash；内取末 hint hash_max）；空 → 0 */
static u64 rfs_node_max(const void *node, u32 inner, u32 count)
{
	const struct Bptree_meta_t *m = node;

	if (count == 0)
		return 0;
	if (inner)
		return ((const struct Bptree_ddentry_inner_node_t *)m)->hints[count - 1].hash_max;
	return ((const struct Bptree_ddentry_leaf_node_t *)m)->entries[count - 1].hash;
}

/* 读节点（拷贝）取 max */
__maybe_unused static int rfs_read_max(struct super_block *sb, u64 blk, u32 *inner_out, u32 *count_out, u64 *max_out)
{
	void *buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	u32 cnt, inner, root;
	u64 pptr;
	int err;

	if (!buf)
		return -ENOMEM;
	err = ravenfs_read_node(sb, blk, buf);
	if (err)
		goto out;
	err = rfs_meta_ok(buf, &cnt, &inner, &root, &pptr);
	if (err)
		goto out;
	*inner_out = inner;
	*count_out = cnt;
	*max_out = rfs_node_max(buf, inner, cnt);
out:
	kfree(buf);
	return err;
}

/* 叶内 GLB：复合序 (hash,name) 的最大下界；无 → GLB_NONE；hit=精确命中 */
static int rfs_leaf_glb(struct super_block *sb, const struct Bptree_ddentry_leaf_node_t *lf,
			u32 cnt, u64 hash, const char *name, u32 nlen, u32 *glb, bool *hit)
{
	char *nm = kmalloc(RAVENFS_MAX_NAME_LEN, GFP_KERNEL);
	int best = -1;
	u32 i;
	int err = 0;

	if (!nm)
		return -ENOMEM;
	*hit = false;
	for (i = 0; i < cnt; i++) {
		const struct ddentry_t *e = &lf->entries[i];
		int c;
		unsigned int en;
		int cmplen;

		if (e->hash < hash) {
			best = (int)i;
			continue;
		}
		if (e->hash > hash)
			break;
		err = ravenfs_ddentry_name(sb, e, nm, &en);
		if (err)
			goto out;
		cmplen = min_t(int, en, (int)nlen);
		c = memcmp(nm, name, cmplen);
		if (c == 0)
			c = (en < nlen) ? -1 : (en > nlen ? 1 : 0);
		if (c == 0) {
			best = (int)i;
			*hit = true;
			break;
		}
		if (c < 0)
			best = (int)i;
		else
			break;
	}
	*glb = (best < 0) ? GLB_NONE : (u32)best;
out:
	kfree(nm);
	return err;
}

/* ══ 多级 walk：填 path（blk/cidx）、hit、插入位 ══ */
int ravenfs_ddentry_walk(struct super_block *sb, u64 dir_ino, const char *name, u32 nlen,
			struct rfs_dpath *p)
{
	struct ravenfs_session *ds __free(ravenfs_session) = ravenfs_session_get(sb, dir_ino);
	void *buf;
	u64 blk;
	u32 depth = 0;
	int err = 0;

	if (IS_ERR(ds))
		return PTR_ERR(ds);
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	blk = ravenfs_session_inode(ds)->data_root;
	p->hit = false;
	for (;;) {
		u32 cnt, inner, root;
		u64 pptr, hash = xxh64(name, nlen, 0);

		if (depth >= RFS_WALK_MAX_DEPTH) { err = -EIO; break; }
		err = ravenfs_read_node(sb, blk, buf);
		if (err)
			break;
		err = rfs_meta_ok(buf, &cnt, &inner, &root, &pptr);
		if (err)
			break;
		if ((depth == 0) != (root != 0)) { err = -EIO; break; }	/* 第 0 层必须 root */
		p->blk[depth] = blk;
		if (!inner) {
			u32 glb;

			err = rfs_leaf_glb(sb, buf, cnt, hash, name, nlen, &glb, &p->hit);
			if (err)
				break;
			p->cidx[depth] = glb;
			p->ins_idx = (glb == GLB_NONE) ? 0 : glb + 1;
			depth++;
			break;
		}
		{
			const struct Bptree_ddentry_inner_node_t *nd = buf;
			u32 i, sel = cnt;

			for (i = 0; i < cnt; i++)
				if (hash <= nd->hints[i].hash_max) { sel = i; break; }
			if (sel == cnt) {		/* 击穿 → 最右 */
				if (cnt == 0) { err = -EIO; break; }
				sel = cnt - 1;
			}
			p->cidx[depth] = sel;
			blk = RFS_PTR_ADDR(nd->hints[sel].sub_ptr);
			depth++;
		}
	}
	p->depth = depth;
	kfree(buf);
	return err;
}

/* 自 child_level 起向上传播（child 子树 max 变为 cur_max）；父 max 未变则短路 */
static int rfs_propagate(struct super_block *sb, const struct rfs_dpath *p, u32 child_level,
			 u64 cur_max)
{
	int lvl;

	for (lvl = (int)child_level - 1; lvl >= 0; lvl--) {
		struct ravenfs_msession *ms = ravenfs_mdata_get(sb, p->blk[lvl]);
		struct Bptree_ddentry_inner_node_t *nd;
		u32 cnt, idx = p->cidx[lvl];

		if (IS_ERR(ms))
			return PTR_ERR(ms);
		nd = ravenfs_msession_node(ms);
		if (!RFS_PP_IS_INNER(nd->meta.parent_ptr) ||
		    rfs_meta_ok(&nd->meta, &cnt, &(u32){0}, &(u32){0}, &(u64){0}) ||
		    idx >= cnt) {
			ravenfs_mdata_put(ms);
			return -EIO;
		}
		nd->hints[idx].hash_max = cur_max;
		NODE_DIRTY(ms);
		if (idx == cnt - 1) {		/* 父 max 亦变 → 继续上溯 */
			ravenfs_mdata_put(ms);
			continue;
		}
		ravenfs_mdata_put(ms);
		break;
	}
	return 0;
}

/* 叶分裂：victim(count=127) → 左 63 / 右 64；维护叶链；返回 low/high max + 新叶块号 */
static int rfs_leaf_split(struct super_block *sb, u64 victim_blk, u64 *out_new,
			  u64 *out_low, u64 *out_high, bool *out_was_root)
{
	struct ravenfs_msession *vms = ravenfs_mdata_get(sb, victim_blk);
	struct ravenfs_msession *nms = NULL;
	struct Bptree_ddentry_leaf_node_t *vl, *nl;
	u32 cnt, inner, root;
	u64 pptr, parent, old_next, new_blk = (u64)PTR_INVALID;
	bool was_root;
	int err;

	if (IS_ERR(vms))
		return PTR_ERR(vms);
	vl = ravenfs_msession_node(vms);
	err = rfs_meta_ok(&vl->meta, &cnt, &inner, &root, &pptr);
	if (err || inner || cnt != RFS_DENTRY_ORDER + 1) {
		ravenfs_mdata_put(vms);
		return err ? err : -EIO;
	}
	was_root = root != 0;
	parent = pptr;
	old_next = RFS_PTR_ADDR(vl->meta.ptrs[2]);	/* ⚠ 先读后写 */
	ravenfs_mdata_put(vms);

	new_blk = ravenfs_block_alloc(sb, 2, 1);	/* 2 块连续、2^1 对齐 */
	if (new_blk == (u64)PTR_INVALID)
		return -ENOSPC;

	vms = ravenfs_mdata_get(sb, victim_blk);
	nms = ravenfs_mdata_get(sb, new_blk);
	if (IS_ERR(vms) || IS_ERR(nms)) {
		err = IS_ERR(vms) ? PTR_ERR(vms) : PTR_ERR(nms);
		if (!IS_ERR(vms))
			ravenfs_mdata_put(vms);
		if (!IS_ERR(nms))
			ravenfs_mdata_put(nms);
		ravenfs_block_free(sb, new_blk, 2);
		return err;
	}
	vl = ravenfs_msession_node(vms);
	nl = ravenfs_msession_node(nms);

	rfs_node_init(nl, 64, 0, 0, parent);
	memcpy(&nl->entries[0], &vl->entries[63], 64 * sizeof(struct ddentry_t));
	rfs_set_addr(&nl->meta.ptrs[1], victim_blk);			/* prev */
	nl->meta.ptrs[2].v = was_root ? RFS_PTR_INV_V : RFS_PTR_PACK(0, old_next);

	vl->meta.parent_ptr.v = RFS_PP_PACK(63, 0, root, parent);	/* 左 63；root 位暂留，升根时清 */
	rfs_set_addr(&vl->meta.ptrs[2], new_blk);			/* next = new */
	if (was_root)
		vl->meta.ptrs[1].v = RFS_PTR_INV_V;			/* ".." 移走，左叶无前驱 */

	NODE_DIRTY(vms);
	NODE_DIRTY(nms);
	*out_low = vl->entries[62].hash;
	*out_high = nl->entries[63].hash;
	*out_was_root = was_root;
	ravenfs_mdata_put(vms);
	ravenfs_mdata_put(nms);

	/* 旧后继 prev = new */
	if (!was_root && old_next != (u64)RFS_MASK52) {
		struct ravenfs_msession *oms = ravenfs_mdata_get(sb, old_next);

		if (IS_ERR(oms))
			return PTR_ERR(oms);
		rfs_set_addr(&((struct Bptree_meta_t *)ravenfs_msession_node(oms))->ptrs[1], new_blk);
		NODE_DIRTY(oms);
		ravenfs_mdata_put(oms);
	}
	*out_new = new_blk;
	return 0;
}

/* 内分裂：victim(count=127) → 左 63 / 右 64；右半孩子 reparent → 新内块 */
static int rfs_inner_split(struct super_block *sb, u64 victim_blk, u64 *out_new,
			   u64 *out_low, u64 *out_high)
{
	struct ravenfs_msession *vms = ravenfs_mdata_get(sb, victim_blk);
	struct ravenfs_msession *nms = NULL;
	struct Bptree_ddentry_inner_node_t *vi, *ni;
	u32 cnt, inner, root, i;
	u64 pptr, parent, new_blk = (u64)PTR_INVALID;
	int err;

	if (IS_ERR(vms))
		return PTR_ERR(vms);
	vi = ravenfs_msession_node(vms);
	err = rfs_meta_ok(&vi->meta, &cnt, &inner, &root, &pptr);
	if (err || !inner || cnt != RFS_DENTRY_ORDER + 1) {
		ravenfs_mdata_put(vms);
		return err ? err : -EIO;
	}
	parent = pptr;
	ravenfs_mdata_put(vms);

	new_blk = ravenfs_block_alloc(sb, 2, 1);
	if (new_blk == (u64)PTR_INVALID)
		return -ENOSPC;

	vms = ravenfs_mdata_get(sb, victim_blk);
	nms = ravenfs_mdata_get(sb, new_blk);
	if (IS_ERR(vms) || IS_ERR(nms)) {
		err = IS_ERR(vms) ? PTR_ERR(vms) : PTR_ERR(nms);
		if (!IS_ERR(vms))
			ravenfs_mdata_put(vms);
		if (!IS_ERR(nms))
			ravenfs_mdata_put(nms);
		ravenfs_block_free(sb, new_blk, 2);
		return err;
	}
	vi = ravenfs_msession_node(vms);
	ni = ravenfs_msession_node(nms);

	rfs_node_init(ni, 64, 1, 0, parent);
	memcpy(&ni->hints[0], &vi->hints[63], 64 * sizeof(struct ddentry_hint_t));
	vi->meta.parent_ptr.v = RFS_PP_PACK(63, 1, root, parent);
	NODE_DIRTY(vms);
	NODE_DIRTY(nms);
	*out_low = vi->hints[62].hash_max;
	*out_high = ni->hints[63].hash_max;
	ravenfs_mdata_put(vms);
	ravenfs_mdata_put(nms);

	/* 右半孩子 reparent → new_blk */
	for (i = 0; i < 64; i++) {
		u64 child;
		struct ravenfs_msession *cms;
		struct Bptree_meta_t *cm;
		u32 cc, ci, cr;
		u64 cp;

		{
			struct ravenfs_msession *tms = ravenfs_mdata_get(sb, new_blk);
			const struct Bptree_ddentry_inner_node_t *tin;

			if (IS_ERR(tms))
				return PTR_ERR(tms);
			tin = ravenfs_msession_node(tms);
			child = RFS_PTR_ADDR(tin->hints[i].sub_ptr);
			ravenfs_mdata_put(tms);
		}
		cms = ravenfs_mdata_get(sb, child);
		if (IS_ERR(cms))
			return PTR_ERR(cms);
		cm = ravenfs_msession_node(cms);
		err = rfs_meta_ok(cm, &cc, &ci, &cr, &cp);
		if (err || cr) {
			ravenfs_mdata_put(cms);
			return err ? err : -EIO;
		}
		cm->parent_ptr.v = RFS_PP_PACK(cc, ci, 0, new_blk);
		NODE_DIRTY(cms);
		ravenfs_mdata_put(cms);
	}
	*out_new = new_blk;
	return 0;
}

/* 新建根内（根升长）；⭐ 显式写 ptrs[] + root ".." */
static int rfs_new_root_inner(struct super_block *sb, u64 dir_ino, u64 left_blk, u64 left_max,
			      u64 right_blk, u64 right_max)
{
	struct ravenfs_session *ds __free(ravenfs_session) = ravenfs_session_get(sb, dir_ino);
	struct ravenfs_msession *rms = NULL;
	struct Bptree_ddentry_inner_node_t *ri;
	u64 root_blk;
	int err = 0;

	if (IS_ERR(ds))
		return PTR_ERR(ds);
	root_blk = ravenfs_block_alloc(sb, 2, 1);
	if (root_blk == (u64)PTR_INVALID)
		return -ENOSPC;

	rms = ravenfs_mdata_get(sb, root_blk);
	if (IS_ERR(rms)) {
		err = PTR_ERR(rms);
		ravenfs_block_free(sb, root_blk, 2);
		return err;
	}
	ri = ravenfs_msession_node(rms);
	rfs_node_init(ri, 2, 1, 1, dir_ino);
	rfs_set_addr(&ri->meta.ptrs[0], RFS_MASK52);	/* 预留 */
	rfs_set_addr(&ri->meta.ptrs[1], dir_ino);	/* `..` = 目录自身 */
	rfs_set_addr(&ri->meta.ptrs[2], RFS_MASK52);
	rfs_set_addr(&ri->hints[0].sub_ptr, left_blk);
	ri->hints[0].hash_max = left_max;
	rfs_set_addr(&ri->hints[1].sub_ptr, right_blk);
	ri->hints[1].hash_max = right_max;
	NODE_DIRTY(rms);
	ravenfs_mdata_put(rms);

	/* 两子 reparent → new root；root 位清零；左叶清 ptrs[1]（原 ".." / prev） */
	{
		u64 kids[2] = { left_blk, right_blk };
		int k;

		for (k = 0; k < 2; k++) {
			struct ravenfs_msession *kms = ravenfs_mdata_get(sb, kids[k]);
			struct Bptree_meta_t *km;
			u32 cc, ci, cr;
			u64 cp;

			if (IS_ERR(kms))
				return PTR_ERR(kms);
			km = ravenfs_msession_node(kms);
			err = rfs_meta_ok(km, &cc, &ci, &cr, &cp);
			if (err) {
				ravenfs_mdata_put(kms);
				return err;
			}
			km->parent_ptr.v = RFS_PP_PACK(cc, ci, 0, root_blk);
			if (!ci) {			/* 叶：左叶 ptrs[1] 已是 INV；确保 */
				/* 左叶在分裂里已置 INV；右叶 ptrs[1]=victim，保留 */
			}
			NODE_DIRTY(kms);
			ravenfs_mdata_put(kms);
		}
	}

	ravenfs_session_inode(ds)->data_root = root_blk;
	ds->row->dirty = true;
	ds->row->stamp = ktime_get_ns();
	return 0;
}

/* 父层应用（+ 内裂上溯）。cur_victim 是父层里被替换/插入的旧子块号 */
static int rfs_apply_up(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p,
			u32 level, u32 child_idx, u64 cur_victim, u64 low_max,
			u64 new_blk, u64 high_max)
{
	for (;;) {
		struct ravenfs_msession *ms = ravenfs_mdata_get(sb, p->blk[level]);
		struct Bptree_ddentry_inner_node_t *nd;
		u32 pcnt, idx = child_idx, new_cnt;
		u32 is_root;
		int err = 0;

		if (IS_ERR(ms))
			return PTR_ERR(ms);
		nd = ravenfs_msession_node(ms);
		if (rfs_meta_ok(&nd->meta, &pcnt, &(u32){0}, &(u32){0}, &(u64){0}) ||
		    !RFS_PP_IS_INNER(nd->meta.parent_ptr) ||
		    idx >= pcnt || pcnt >= RFS_DENTRY_ORDER ||
		    RFS_PTR_ADDR(nd->hints[idx].sub_ptr) != cur_victim) {
			ravenfs_mdata_put(ms);
			return -EIO;
		}
		nd->hints[idx].hash_max = low_max;
		memmove(&nd->hints[idx + 2], &nd->hints[idx + 1],
			(size_t)(pcnt - idx - 1) * sizeof(struct ddentry_hint_t));
		rfs_set_addr(&nd->hints[idx + 1].sub_ptr, new_blk);
		nd->hints[idx + 1].hash_max = high_max;
		new_cnt = pcnt + 1;
		is_root = RFS_PP_IS_ROOT(nd->meta.parent_ptr);
		nd->meta.parent_ptr.v = RFS_PP_PACK(new_cnt, 1, is_root, RFS_PP_PTR(nd->meta.parent_ptr));
		NODE_DIRTY(ms);
		ravenfs_mdata_put(ms);

		if (new_cnt <= RFS_DENTRY_ORDER) {
			if (idx + 1 == new_cnt - 1)	/* 新 hint 是末位 → 父 max 变 */
				return rfs_propagate(sb, p, level, high_max);
			return 0;
		}
		/* 父溢出 → 内裂 */
		{
			u64 rblk, lmx, hmx;

			err = rfs_inner_split(sb, p->blk[level], &rblk, &lmx, &hmx);
			if (err)
				return err;
			if (is_root)
				return rfs_new_root_inner(sb, dir_ino, p->blk[level], lmx, rblk, hmx);
			if (level == 0)
				return -EIO;
			cur_victim = p->blk[level];
			low_max = lmx;
			new_blk = rblk;
			high_max = hmx;
			child_idx = p->cidx[level - 1];
			level--;
		}
	}
}

/* ══ 插入条目到叶（walk 定位后调用）；含分裂/升根/hint 维护 ══ */
int ravenfs_ddentry_insert(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p,
			  const char *name, u32 nlen, u64 hash, u64 inum, u64 remain_ptr)
{
	u64 leaf_blk = p->blk[p->depth - 1];
	u32 idx = p->ins_idx;
	u64 old_max, new_max, cnt;
	int err = 0;

	{
		struct ravenfs_msession *ms = ravenfs_mdata_get(sb, leaf_blk);
		struct Bptree_ddentry_leaf_node_t *lf;
		struct ddentry_t *e;
		u32 c, inner, root;
		u64 pptr;

		if (IS_ERR(ms))
			return PTR_ERR(ms);
		lf = ravenfs_msession_node(ms);
		if (rfs_meta_ok(&lf->meta, &c, &inner, &root, &pptr) || inner) {
			ravenfs_mdata_put(ms);
			return -EIO;
		}
		if (idx > c)
			idx = c;
		old_max = (c > 0) ? lf->entries[c - 1].hash : 0;
		memmove(&lf->entries[idx + 1], &lf->entries[idx],
			(size_t)(c - idx) * sizeof(struct ddentry_t));
		e = &lf->entries[idx];
		memset(e, 0, sizeof(*e));
		e->inode_ptr.v = RFS_PTR_PACK(nlen, inum);
		e->hash = hash;
		e->full_char_remain_ptr = remain_ptr;
		memcpy(e->name, name, min_t(u32, nlen, (u32)DENTRY_INLINE_NAME_MAX));
		lf->meta.parent_ptr.v = RFS_PP_PACK(c + 1, 0, root, pptr);
		cnt = c + 1;
		new_max = lf->entries[cnt - 1].hash;
		NODE_DIRTY(ms);
		ravenfs_mdata_put(ms);
	}

	if (cnt <= RFS_DENTRY_ORDER) {
		if (new_max != old_max)
			return rfs_propagate(sb, p, p->depth - 1, new_max);
		return 0;
	}
	/* 叶溢出 → 分裂 */
	{
		u64 new_blk, low_max, high_max;
		bool was_root;

		err = rfs_leaf_split(sb, leaf_blk, &new_blk, &low_max, &high_max, &was_root);
		if (err)
			return err;
		if (p->depth == 1)
			return rfs_new_root_inner(sb, dir_ino, leaf_blk, low_max, new_blk, high_max);
		return rfs_apply_up(sb, dir_ino, p, p->depth - 2, p->cidx[p->depth - 2],
				    leaf_blk, low_max, new_blk, high_max);
	}
}

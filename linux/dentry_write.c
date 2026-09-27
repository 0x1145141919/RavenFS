// SPDX-License-Identifier: GPL-2.0
/*
 * dentry_write.c —— dentry B+树**写入**：多级 walk + 叶插入/分裂 + 根升长 + 内裂 + hint 精确维护
 *
 * 规格：`Docs/linux/ravenfs-linux-ko-handover.md` §4（dentry B+树写入机械；原移植 notes 已折入该节）。
 *   god 参考：`juvenile_god/dentry_walk_lookup/dentry_split/dentry_insert_delete`（v1.1 语义）。
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

static inline void rfs_set_addr(struct ptr_in_idx *p, u64 addr)
{
	p->v = RFS_PTR_PACK(0, addr);
}

static int rfs_meta_ok_x(const struct Bptree_meta_t *m, u32 *cnt, u32 *inner, u32 *root,
			u64 *pptr, u32 maxc)
{
	if (m->magics[0] != DENTRY_MAGIC1 || m->magics[1] != DENTRY_MAGIC2 ||
	    m->magics[2] != DENTRY_MAGIC3 || m->magics[3] != DENTRY_MAGIC4)
		return -EIO;
	*cnt = RFS_PP_COUNT(m->parent_ptr);
	*inner = RFS_PP_IS_INNER(m->parent_ptr);
	*root = RFS_PP_IS_ROOT(m->parent_ptr);
	*pptr = RFS_PP_PTR(m->parent_ptr);
	if (*cnt > maxc)
		return -EIO;			/* 瞬时槽落盘？ */
	return 0;
}

static int rfs_meta_ok(const struct Bptree_meta_t *m, u32 *cnt, u32 *inner, u32 *root, u64 *pptr)
{
	return rfs_meta_ok_x(m, cnt, inner, root, pptr, RFS_DENTRY_ORDER);
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
	err = rfs_meta_ok_x(&vl->meta, &cnt, &inner, &root, &pptr, RFS_DENTRY_ORDER + 1);
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
	err = rfs_meta_ok_x(&vi->meta, &cnt, &inner, &root, &pptr, RFS_DENTRY_ORDER + 1);
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
		    idx >= pcnt || pcnt > RFS_DENTRY_ORDER ||
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

/* ══════════════════════════════════════════════════════════════════
 * 删除路径（M5b）：叶条目删除 + hash_max 上溯 + 欠载平衡/合并/抹匀 + 根收缩
 * 规格：`Docs/linux/ravenfs-linux-ko-handover.md` §4（god dentry_insert_delete / dentry_balance 移植）
 * ══════════════════════════════════════════════════════════════════ */

#define RFS_DENTRY_MIN	63u

#define PP_SET_COUNT(m, c) \
	do { (m)->parent_ptr.v = RFS_PP_PACK((u32)(c), RFS_PP_IS_INNER((m)->parent_ptr), \
					RFS_PP_IS_ROOT((m)->parent_ptr), RFS_PP_PTR((m)->parent_ptr)); } while (0)

/* 内部节点孩子 reparent → node_blk（仅非根；改的是孩子的 parent_ptr） */
static int rfs_reparent_children(struct super_block *sb,
				 const struct Bptree_ddentry_inner_node_t *nd,
				 u64 node_blk, u32 cnt)
{
	u32 i;

	for (i = 0; i < cnt; i++) {
		u64 cblk = RFS_PTR_ADDR(nd->hints[i].sub_ptr);
		struct ravenfs_msession *cms;
		struct Bptree_meta_t *cm;
		u32 cc, ci, cr;
		u64 cp;
		int err;

		if (cblk == (u64)RFS_MASK52)
			return -EIO;
		cms = ravenfs_mdata_get(sb, cblk);
		if (IS_ERR(cms))
			return PTR_ERR(cms);
		cm = ravenfs_msession_node(cms);
		err = rfs_meta_ok(cm, &cc, &ci, &cr, &cp);
		if (err || cr) {
			ravenfs_mdata_put(cms);
			return err ? err : -EIO;
		}
		if (RFS_PP_PTR(cm->parent_ptr) != node_blk) {
			cm->parent_ptr.v = RFS_PP_PACK(cc, ci, 0, node_blk);
			NODE_DIRTY(cms);
		}
		ravenfs_mdata_put(cms);
	}
	return 0;
}

/*
 * 欠载平衡：parent_blk 的 idx 孩子欠载 → 与邻居合并(total<=126)或抹匀。
 * 返回 0（*need = 父欠载需上溯）/ 1（父为根且只 1 孩 → 交上层根收缩）/ <0 错误。
 * 邻居方向：idx==0 只向右（idx+1）；idx>0 只向左（idx-1）。
 */
static int rfs_node_balance(struct super_block *sb, u64 parent_blk, u32 idx, bool *need)
{
	struct ravenfs_msession *pms, *cms = NULL, *nms = NULL;
	struct Bptree_ddentry_inner_node_t *pi;
	struct Bptree_meta_t *cm, *nm;
	u32 pcnt, pin, proot, cc = 0, ci, cr, nc = 0, ni, nr;
	u64 pptr, cp, np, child_blk, nblk, nidx, total;
	bool is_inner;
	int ret = 0;

	*need = false;
	pms = ravenfs_mdata_get(sb, parent_blk);
	if (IS_ERR(pms))
		return PTR_ERR(pms);
	pi = ravenfs_msession_node(pms);
	ret = rfs_meta_ok(&pi->meta, &pcnt, &pin, &proot, &pptr);
	if (ret || !pin || idx >= pcnt) {
		ret = ret ? ret : -EIO;
		goto out;
	}
	child_blk = RFS_PTR_ADDR(pi->hints[idx].sub_ptr);
	cms = ravenfs_mdata_get(sb, child_blk);
	if (IS_ERR(cms)) {
		ret = PTR_ERR(cms);
		cms = NULL;
		goto out;
	}
	cm = ravenfs_msession_node(cms);
	ret = rfs_meta_ok(cm, &cc, &ci, &cr, &cp);
	if (ret)
		goto out;
	is_inner = ci != 0;
	if (cc >= RFS_DENTRY_MIN)		/* 已健康，无需平衡 */
		goto out;

	if (idx == 0) {
		if (pcnt == 1) {		/* 根收缩信号（父为根且只 1 孩） */
			ret = 1;
			goto out;
		}
		nidx = 1;
	} else {
		nidx = idx - 1;
	}
	nblk = RFS_PTR_ADDR(pi->hints[nidx].sub_ptr);
	nms = ravenfs_mdata_get(sb, nblk);
	if (IS_ERR(nms)) {
		ret = PTR_ERR(nms);
		nms = NULL;
		goto out;
	}
	nm = ravenfs_msession_node(nms);
	ret = rfs_meta_ok(nm, &nc, &ni, &nr, &np);
	if (ret)
		goto out;
	if ((ni != 0) != is_inner) {
		ret = -EIO;
		goto out;
	}

	total = (u64)cc + nc;

	if (total <= RFS_DENTRY_ORDER) {
		/* ── 合并：吸收进 child ── */
		if (is_inner) {
			struct Bptree_ddentry_inner_node_t *cnd = ravenfs_msession_node(cms);
			struct Bptree_ddentry_inner_node_t *nnd = ravenfs_msession_node(nms);

			if (idx == 0) {
				memcpy(&cnd->hints[cc], nnd->hints,
				       nc * sizeof(struct ddentry_hint_t));
			} else {
				memmove(&cnd->hints[nc], &cnd->hints[0],
					cc * sizeof(struct ddentry_hint_t));
				memcpy(&cnd->hints[0], nnd->hints,
				       nc * sizeof(struct ddentry_hint_t));
			}
			PP_SET_COUNT(&cnd->meta, total);
			NODE_DIRTY(cms);
			ret = rfs_reparent_children(sb, cnd, child_blk, (u32)total);
			if (ret)
				goto out;
		} else {
			struct Bptree_ddentry_leaf_node_t *cnd = ravenfs_msession_node(cms);
			struct Bptree_ddentry_leaf_node_t *nnd = ravenfs_msession_node(nms);

			if (idx == 0) {
				u64 nnext;

				memcpy(&cnd->entries[cc], nnd->entries,
				       nc * sizeof(struct ddentry_t));
				nnext = RFS_PTR_ADDR(nnd->meta.ptrs[2]);
				rfs_set_addr(&cnd->meta.ptrs[2], nnext);
				if (nnext != (u64)RFS_MASK52) {
					struct ravenfs_msession *x = ravenfs_mdata_get(sb, nnext);

					if (IS_ERR(x)) { ret = PTR_ERR(x); goto out; }
					rfs_set_addr(&((struct Bptree_meta_t *)ravenfs_msession_node(x))->ptrs[1], child_blk);
					NODE_DIRTY(x);
					ravenfs_mdata_put(x);
				}
			} else {
				u64 nprev;

				memmove(&cnd->entries[nc], &cnd->entries[0],
					cc * sizeof(struct ddentry_t));
				memcpy(&cnd->entries[0], nnd->entries,
				       nc * sizeof(struct ddentry_t));
				nprev = RFS_PTR_ADDR(nnd->meta.ptrs[1]);
				rfs_set_addr(&cnd->meta.ptrs[1], nprev);
				if (nprev != (u64)RFS_MASK52) {
					struct ravenfs_msession *x = ravenfs_mdata_get(sb, nprev);

					if (IS_ERR(x)) { ret = PTR_ERR(x); goto out; }
					rfs_set_addr(&((struct Bptree_meta_t *)ravenfs_msession_node(x))->ptrs[2], child_blk);
					NODE_DIRTY(x);
					ravenfs_mdata_put(x);
				}
			}
			PP_SET_COUNT(&cnd->meta, total);
			memset(&cnd->entries[total], 0,
			       (RFS_DENTRY_ORDER + 1 - total) * sizeof(struct ddentry_t));
			NODE_DIRTY(cms);
		}
		/* 邻居内容已搬入 child → 丢弃其缓存行 + 释放块 */
		ravenfs_mdata_put(nms);
		nms = NULL;
		ravenfs_mdata_drop(sb, nblk);
		ravenfs_block_free(sb, nblk, 2);

		/* 父删 hint + 更新保留 hint 的 hash_max */
		{
			u32 del_idx = (idx == 0) ? 1 : (idx - 1);
			u32 kept_before = (idx == 0) ? 0 : (idx - 1);
			u32 kept_after = kept_before - (del_idx < kept_before ? 1u : 0u);

			if (del_idx < pcnt - 1)
				memmove(&pi->hints[del_idx], &pi->hints[del_idx + 1],
					(size_t)(pcnt - del_idx - 1) * sizeof(struct ddentry_hint_t));
			memset(&pi->hints[pcnt - 1], 0, sizeof(struct ddentry_hint_t));
			pcnt--;
			PP_SET_COUNT(&pi->meta, pcnt);
			pi->hints[kept_after].hash_max =
				rfs_node_max(ravenfs_msession_node(cms), is_inner, (u32)total);
			NODE_DIRTY(pms);
		}
		if (!proot && pcnt < RFS_DENTRY_MIN)
			*need = true;
		goto out;
	}

	/* ── 抹匀（再分配）：total > 126，两节点都在 ── */
	{
		u32 left = (u32)(total / 2), right = (u32)(total - left);
		bool child_left = (idx == 0);
		struct ravenfs_msession *lms = child_left ? cms : nms;
		struct ravenfs_msession *rms = child_left ? nms : cms;
		u64 lblk = child_left ? child_blk : nblk;
		u64 rblk = child_left ? nblk : child_blk;
		u32 lc = child_left ? cc : nc;
		u32 rc = child_left ? nc : cc;

		if (is_inner) {
			struct Bptree_ddentry_inner_node_t *l = ravenfs_msession_node(lms);
			struct Bptree_ddentry_inner_node_t *r = ravenfs_msession_node(rms);
			u32 k;

			if (lc > left) {
				k = lc - left;
				memmove(&r->hints[k], &r->hints[0],
					rc * sizeof(struct ddentry_hint_t));
				memcpy(&r->hints[0], &l->hints[left],
				       k * sizeof(struct ddentry_hint_t));
			} else {
				k = left - lc;
				memcpy(&l->hints[lc], &r->hints[0],
				       k * sizeof(struct ddentry_hint_t));
				memmove(&r->hints[0], &r->hints[k],
					(rc - k) * sizeof(struct ddentry_hint_t));
			}
			PP_SET_COUNT(&l->meta, left);
			PP_SET_COUNT(&r->meta, right);
			memset(&l->hints[left], 0, (RFS_DENTRY_ORDER + 1 - left) * sizeof(struct ddentry_hint_t));
			memset(&r->hints[right], 0, (RFS_DENTRY_ORDER + 1 - right) * sizeof(struct ddentry_hint_t));
			NODE_DIRTY(lms);
			NODE_DIRTY(rms);
			ret = rfs_reparent_children(sb, l, lblk, left);
			if (ret)
				goto out;
			ret = rfs_reparent_children(sb, r, rblk, right);
			if (ret)
				goto out;
		} else {
			struct Bptree_ddentry_leaf_node_t *l = ravenfs_msession_node(lms);
			struct Bptree_ddentry_leaf_node_t *r = ravenfs_msession_node(rms);
			u32 k;

			if (lc > left) {
				k = lc - left;
				memmove(&r->entries[k], &r->entries[0],
					rc * sizeof(struct ddentry_t));
				memcpy(&r->entries[0], &l->entries[left],
				       k * sizeof(struct ddentry_t));
			} else {
				k = left - lc;
				memcpy(&l->entries[lc], &r->entries[0],
				       k * sizeof(struct ddentry_t));
				memmove(&r->entries[0], &r->entries[k],
					(rc - k) * sizeof(struct ddentry_t));
			}
			PP_SET_COUNT(&l->meta, left);
			PP_SET_COUNT(&r->meta, right);
			memset(&l->entries[left], 0, (RFS_DENTRY_ORDER + 1 - left) * sizeof(struct ddentry_t));
			memset(&r->entries[right], 0, (RFS_DENTRY_ORDER + 1 - right) * sizeof(struct ddentry_t));
			NODE_DIRTY(lms);
			NODE_DIRTY(rms);
		}
		pi->hints[idx].hash_max =
			rfs_node_max(ravenfs_msession_node(cms), is_inner, child_left ? left : right);
		pi->hints[nidx].hash_max =
			rfs_node_max(ravenfs_msession_node(nms), is_inner, child_left ? right : left);
		NODE_DIRTY(pms);
		*need = false;
	}

out:
	if (nms)
		ravenfs_mdata_put(nms);
	if (cms)
		ravenfs_mdata_put(cms);
	ravenfs_mdata_put(pms);
	return ret;
}

/* 根收缩：根（内）只剩 1 孩时，把唯一孩子提升为新根（读 dir inode 的 data_root） */
static int rfs_root_shrink(struct super_block *sb, u64 dir_ino)
{
	struct ravenfs_session *ds __free(ravenfs_session) = ravenfs_session_get(sb, dir_ino);
	struct ravenfs_msession *rms, *cms = NULL;
	struct Bptree_meta_t *rm, *cm;
	u64 root_blk, child_blk;
	u32 rc, rin, rrt, cc, ci, crr;
	u64 rp, cp;
	int err;

	if (IS_ERR(ds))
		return PTR_ERR(ds);
	root_blk = ravenfs_session_inode(ds)->data_root;
	if (root_blk == (u64)PTR_INVALID)
		return -EIO;
	rms = ravenfs_mdata_get(sb, root_blk);
	if (IS_ERR(rms))
		return PTR_ERR(rms);
	rm = ravenfs_msession_node(rms);
	err = rfs_meta_ok(rm, &rc, &rin, &rrt, &rp);
	if (err)
		goto out;
	if (!rin || rc != 1)		/* 根叶 / 根内非单孩 → 无需收缩 */
		goto out;

	child_blk = RFS_PTR_ADDR(((struct Bptree_ddentry_inner_node_t *)rm)->hints[0].sub_ptr);
	cms = ravenfs_mdata_get(sb, child_blk);
	if (IS_ERR(cms)) {
		err = PTR_ERR(cms);
		cms = NULL;
		goto out;
	}
	cm = ravenfs_msession_node(cms);
	err = rfs_meta_ok(cm, &cc, &ci, &crr, &cp);
	if (err || crr) {
		err = err ? err : -EIO;
		goto out;
	}
	cm->parent_ptr.v = RFS_PP_PACK(cc, ci, 1, dir_ino);
	if (!ci) {
		struct Bptree_ddentry_leaf_node_t *cl = ravenfs_msession_node(cms);

		rfs_set_addr(&cl->meta.ptrs[0], RFS_MASK52);
		rfs_set_addr(&cl->meta.ptrs[1], dir_ino);	/* ".." */
		rfs_set_addr(&cl->meta.ptrs[2], RFS_MASK52);
	} else {
		struct Bptree_ddentry_inner_node_t *cin = ravenfs_msession_node(cms);

		rfs_set_addr(&cin->meta.ptrs[1], dir_ino);	/* ".." */
	}
	NODE_DIRTY(cms);

	ravenfs_session_inode(ds)->data_root = child_blk;
	ds->row->dirty = true;
	ds->row->stamp = ktime_get_ns();

	ravenfs_mdata_drop(sb, root_blk);
	ravenfs_block_free(sb, root_blk, 2);
out:
	if (cms)
		ravenfs_mdata_put(cms);
	ravenfs_mdata_put(rms);
	return err;
}

/* 欠载上溯：从叶的父层一路 balance；收尾统一尝试根收缩。返回 <0 错误 */
static int rfs_balance_up(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p)
{
	int level = (int)p->depth - 2;
	u32 idx;
	bool need = false;
	int err;

	if ((int)p->depth < 2)
		return 0;
	idx = p->cidx[level];
	for (;;) {
		err = rfs_node_balance(sb, p->blk[level], idx, &need);
		if (err < 0)
			return err;
		if (err == 1 || !need || level == 0)
			break;
		level--;
		idx = p->cidx[level];
	}
	return rfs_root_shrink(sb, dir_ino);
}

/* 删除一个条目：叶删 + hash_max 上溯 + 欠载平衡（含根收缩） */
int ravenfs_ddentry_delete(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p,
			   struct rfs_delinfo *info)
{
	u32 depth = p->depth;
	u64 leaf_blk = p->blk[depth - 1];
	u32 idx = p->cidx[depth - 1];
	struct ravenfs_msession *ms;
	struct Bptree_ddentry_leaf_node_t *lf;
	u32 cnt, inner, root;
	u64 pptr, new_max;
	bool is_last, is_root;
	int err;

	ms = ravenfs_mdata_get(sb, leaf_blk);
	if (IS_ERR(ms))
		return PTR_ERR(ms);
	lf = ravenfs_msession_node(ms);
	err = rfs_meta_ok(&lf->meta, &cnt, &inner, &root, &pptr);
	if (err || inner || idx >= cnt) {
		ravenfs_mdata_put(ms);
		return err ? err : -EIO;
	}
	if (info) {
		info->inum = RFS_PTR_ADDR(lf->entries[idx].inode_ptr);
		info->name_len = RFS_PTR_FREE(lf->entries[idx].inode_ptr);
		info->remain = lf->entries[idx].full_char_remain_ptr;
	}
	is_last = (idx == cnt - 1);
	is_root = root != 0;
	if (idx < cnt - 1)
		memmove(&lf->entries[idx], &lf->entries[idx + 1],
			(size_t)(cnt - idx - 1) * sizeof(struct ddentry_t));
	memset(&lf->entries[cnt - 1], 0, sizeof(struct ddentry_t));
	cnt--;
	lf->meta.parent_ptr.v = RFS_PP_PACK(cnt, 0, root, pptr);
	new_max = (cnt > 0) ? lf->entries[cnt - 1].hash : 0;
	NODE_DIRTY(ms);
	ravenfs_mdata_put(ms);

	if (is_last || cnt == 0)
		err = rfs_propagate(sb, p, depth - 1, new_max);
	if (!err && !is_root && cnt < RFS_DENTRY_MIN)
		err = rfs_balance_up(sb, dir_ino, p);
	return err;
}

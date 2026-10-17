// SPDX-License-Identifier: GPL-2.0
/*
 * dentry_node.c —— FS 底层原语：dentry B+树节点结构机械
 *
 * 【三分拆分（2026-09-28）】自 dentry_write.c 全量迁入（meta 校验 / 叶内定位 / 分裂 /
 * 传播 / 平衡 / 根收缩 / 升根）+ 自 dentry.c 迁入叶内读机械（read_bytes / ddentry_name /
 * leaf_lookup —— 叶内名字物化与查找属同一套「叶内机械」，与写侧 leaf_glb 同层）。
 * 职责：节点缓冲布局与树形不变量的 HOW；条目/路径语义归 dentry_tree.c。
 * 消费方：dentry_tree.c（walk/insert/delete/dir_find 经 ravenfs_dentry_node.h）
 *         + 接待层 emit 直取 ddentry_name（跨层向下只读工具，方案声明例外）。
 * 验收与移植要点总纲见 dentry_tree.c 文件头（原 dentry_write.c banner 原样保留）。
 * 规格：Docs/linux/ravenfs-linux-ko-handover.md §4；god 参考 juvenile_god/dentry_*。
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>

#include "ravenfs.h"
#include "ravenfs_dentry_node.h"
#include "ravenfs_inode_session.h"
#include "ravenfs_allocator_top.h"
#include "ravenfs_name_data_plane.h"

/* ── 8K 节点 / 跨块字节读 ── */
/* ── 8K 节点读取统一走 mdata 核心缓存（ravenfs_read_node → mdata_cache_core.c） ── */

/* 物化 dentry 名字：≤40 内联；>40 从所属目录名字数据面读尾部
 * （owner_dir_inum 定位目录 inode；full_char_remain_offset 为数据面内逻辑偏移） */
int ravenfs_ddentry_name(struct super_block *sb, u64 owner_dir_inum,
			       const struct ddentry_t *e,
			       char *out, unsigned int *out_len)
{
	u32 nlen = RFS_PTR_FREE(e->inode_ptr);
	u32 inl = min_t(u32, nlen, (u32)DENTRY_INLINE_NAME_MAX);

	if (nlen == 0 || nlen > RAVENFS_MAX_NAME_LEN)
		return -EIO;
	memcpy(out, e->name, inl);
	if (nlen > DENTRY_INLINE_NAME_MAX) {
		u64 off = e->full_char_remain_offset;
		int err;

		if (off == (u64)PTR_INVALID)
			return -EIO;
		err = ravenfs_nd_read(sb, owner_dir_inum, off,
				      (u16)(nlen - inl), out + inl);
		if (err)
			return err;
	}
	*out_len = nlen;
	return 0;
}

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

int rfs_meta_ok(const struct Bptree_meta_t *m, u32 *cnt, u32 *inner, u32 *root, u64 *pptr)
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

/* 叶内 GLB：复合序 (hash,name) 的最大下界；无 → GLB_NONE；hit=精确命中 */
int rfs_leaf_glb(struct super_block *sb, u64 owner_dir_inum,
			const struct Bptree_ddentry_leaf_node_t *lf,
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
		err = ravenfs_ddentry_name(sb, owner_dir_inum, e, nm, &en);
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

int ravenfs_leaf_lookup(struct super_block *sb, u64 owner_dir_inum,
			       const struct Bptree_ddentry_leaf_node_t *leaf,
			       u32 cnt, const char *name, u32 nlen, u64 hash, u64 *ino)
{
	char *nm = kmalloc(RAVENFS_MAX_NAME_LEN, GFP_KERNEL);
	u32 i;

	if (!nm)
		return -ENOMEM;
	*ino = (u64)PTR_INVALID;
	for (i = 0; i < cnt; i++) {
		const struct ddentry_t *e = &leaf->entries[i];
		unsigned int en;
		int c, cmplen;

		if (e->hash > hash)		/* 叶按 hash 严格递增 → 越过即 miss */
			break;
		if (e->hash != hash)
			continue;
		if (ravenfs_ddentry_name(sb, owner_dir_inum, e, nm, &en)) {
			kfree(nm);
			return -EIO;
		}
		cmplen = min_t(int, en, (int)nlen);
		c = memcmp(nm, name, cmplen);
		if (c == 0)
			c = (en < nlen) ? -1 : (en > nlen ? 1 : 0);
		if (c == 0) {
			*ino = RFS_PTR_ADDR(e->inode_ptr);
			break;
		}
		if (c > 0)			/* 已越过（同名 hash 区） */
			break;
	}
	kfree(nm);
	return 0;
}

/* 自 child_level 起向上传播（child 子树 max 变为 cur_max）；父 max 未变则短路 */
int rfs_propagate(struct super_block *sb, const struct rfs_dpath *p, u32 child_level,
			 u64 cur_max)
{
	int lvl;

	for (lvl = (int)child_level - 1; lvl >= 0; lvl--) {
		struct Bptree_ddentry_inner_node_t *nd;
		u32 cnt, idx = p->cidx[lvl];

		RAVENFS_MDATA_SESSION(ms, ravenfs_mdata_cache_of(sb),
				      p->blk[lvl]);
		if (!RAVENFS_MDATA_SESSION_VALID(ms))
			return PTR_ERR(ms);
		nd = RAVENFS_MDATA_SESSION_DATA(ms);
		if (!RFS_PP_IS_INNER(nd->meta.parent_ptr) ||
		    rfs_meta_ok(&nd->meta, &cnt, &(u32){0}, &(u32){0}, &(u64){0}) ||
		    idx >= cnt)
			return -EIO;
		nd->hints[idx].hash_max = cur_max;
		RAVENFS_MDATA_SESSION_DIRTY(ms);
		if (idx == cnt - 1)		/* 父 max 亦变 → 继续上溯 */
			continue;		/* ms 由 cleanup 自动 put */
		break;
	}
	return 0;
}

/* 叶分裂：victim(count=127) → 左 63 / 右 64；维护叶链；返回 low/high max + 新叶块号
 * out_dotdot：旧根叶的 `..`（仅 was_root 有意义；升根时继承，非根 = RFS_MASK52） */
int rfs_leaf_split(struct super_block *sb, u64 victim_blk, u64 *out_new,
			  u64 *out_low, u64 *out_high, bool *out_was_root, u64 *out_dotdot)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct Bptree_ddentry_leaf_node_t *vl, *nl;
	u32 cnt, inner, root;
	u64 pptr, parent, old_next, dotdot, new_blk = (u64)PTR_INVALID;
	bool was_root;
	int err;

	RAVENFS_MDATA_SESSION(vms, ravenfs_mdata_cache_of(sb), victim_blk);
	if (!RAVENFS_MDATA_SESSION_VALID(vms))
		return PTR_ERR(vms);
	vl = RAVENFS_MDATA_SESSION_DATA(vms);
	err = rfs_meta_ok_x(&vl->meta, &cnt, &inner, &root, &pptr, RFS_DENTRY_ORDER + 1);
	if (err || inner || cnt != RFS_DENTRY_ORDER + 1)
		return err ? err : -EIO;
	was_root = root != 0;
	parent = pptr;
	old_next = RFS_PTR_ADDR(vl->meta.ptrs[2]);	/* ⚠ 先读后写 */
	dotdot = was_root ? RFS_PTR_ADDR(vl->meta.ptrs[1]) : (u64)RFS_MASK52;	/* ⚠ ".." 先读后写 */
	*out_dotdot = dotdot;
	RAVENFS_MDATA_SESSION_RELEASE(vms);		/* 分配新块前先归还会话 */

	new_blk = rfs_allocator_block_alloc_hard(sbi->alloc, 2, 1);	/* 2 块连续、2^1 对齐 */
	if (new_blk == (u64)PTR_INVALID)
		return -ENOSPC;

	RAVENFS_MDATA_SESSION_RESET(vms, ravenfs_mdata_cache_of(sb), victim_blk);
	RAVENFS_MDATA_SESSION(nms, ravenfs_mdata_cache_of(sb), new_blk);
	if (!RAVENFS_MDATA_SESSION_VALID(vms) ||
	    !RAVENFS_MDATA_SESSION_VALID(nms)) {
		err = !RAVENFS_MDATA_SESSION_VALID(vms) ?
		      PTR_ERR(vms) : PTR_ERR(nms);
		(void)rfs_allocator_block_free(sbi->alloc, new_blk, 2);
		return err;
	}
	vl = RAVENFS_MDATA_SESSION_DATA(vms);
	nl = RAVENFS_MDATA_SESSION_DATA(nms);

	rfs_node_init(nl, 64, 0, 0, parent);
	memcpy(&nl->entries[0], &vl->entries[63], 64 * sizeof(struct ddentry_t));
	rfs_set_addr(&nl->meta.ptrs[1], victim_blk);			/* prev */
	nl->meta.ptrs[2].v = was_root ? RFS_PTR_INV_V : RFS_PTR_PACK(0, old_next);

	vl->meta.parent_ptr.v = RFS_PP_PACK(63, 0, root, parent);	/* 左 63；root 位暂留，升根时清 */
	rfs_set_addr(&vl->meta.ptrs[2], new_blk);			/* next = new */
	if (was_root)
		vl->meta.ptrs[1].v = RFS_PTR_INV_V;			/* ".." 移走，左叶无前驱 */

	RAVENFS_MDATA_SESSION_DIRTY(vms);
	RAVENFS_MDATA_SESSION_DIRTY(nms);
	*out_low = vl->entries[62].hash;
	*out_high = nl->entries[63].hash;
	*out_was_root = was_root;
	RAVENFS_MDATA_SESSION_RELEASE(nms);
	RAVENFS_MDATA_SESSION_RELEASE(vms);

	/* 旧后继 prev = new */
	if (!was_root && old_next != (u64)RFS_MASK52) {
		RAVENFS_MDATA_SESSION(oms, ravenfs_mdata_cache_of(sb),
				      old_next);

		if (!RAVENFS_MDATA_SESSION_VALID(oms))
			return PTR_ERR(oms);
		rfs_set_addr(&((struct Bptree_meta_t *)
				RAVENFS_MDATA_SESSION_DATA(oms))->ptrs[1],
			     new_blk);
		RAVENFS_MDATA_SESSION_DIRTY(oms);
	}
	*out_new = new_blk;
	return 0;
}

/* 内分裂：victim(count=127) → 左 63 / 右 64；右半孩子 reparent → 新内块 */
static int rfs_inner_split(struct super_block *sb, u64 victim_blk, u64 *out_new,
			   u64 *out_low, u64 *out_high, u64 *out_dotdot)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct Bptree_ddentry_inner_node_t *vi, *ni;
	u32 cnt, inner, root, i;
	u64 pptr, parent, new_blk = (u64)PTR_INVALID;
	int err;

	RAVENFS_MDATA_SESSION(vms, ravenfs_mdata_cache_of(sb), victim_blk);
	if (!RAVENFS_MDATA_SESSION_VALID(vms))
		return PTR_ERR(vms);
	vi = RAVENFS_MDATA_SESSION_DATA(vms);
	err = rfs_meta_ok_x(&vi->meta, &cnt, &inner, &root, &pptr, RFS_DENTRY_ORDER + 1);
	if (err || !inner || cnt != RFS_DENTRY_ORDER + 1)
		return err ? err : -EIO;
	parent = pptr;
	*out_dotdot = (root != 0) ? RFS_PTR_ADDR(vi->meta.ptrs[1]) : (u64)RFS_MASK52;	/* ⚠ 先读后写 */
	RAVENFS_MDATA_SESSION_RELEASE(vms);

	new_blk = rfs_allocator_block_alloc_hard(sbi->alloc, 2, 1);
	if (new_blk == (u64)PTR_INVALID)
		return -ENOSPC;

	RAVENFS_MDATA_SESSION_RESET(vms, ravenfs_mdata_cache_of(sb), victim_blk);
	RAVENFS_MDATA_SESSION(nms, ravenfs_mdata_cache_of(sb), new_blk);
	if (!RAVENFS_MDATA_SESSION_VALID(vms) ||
	    !RAVENFS_MDATA_SESSION_VALID(nms)) {
		err = !RAVENFS_MDATA_SESSION_VALID(vms) ?
		      PTR_ERR(vms) : PTR_ERR(nms);
		(void)rfs_allocator_block_free(sbi->alloc, new_blk, 2);
		return err;
	}
	vi = RAVENFS_MDATA_SESSION_DATA(vms);
	ni = RAVENFS_MDATA_SESSION_DATA(nms);

	rfs_node_init(ni, 64, 1, 0, parent);
	memcpy(&ni->hints[0], &vi->hints[63], 64 * sizeof(struct ddentry_hint_t));
	vi->meta.parent_ptr.v = RFS_PP_PACK(63, 1, root, parent);
	RAVENFS_MDATA_SESSION_DIRTY(vms);
	RAVENFS_MDATA_SESSION_DIRTY(nms);
	*out_low = vi->hints[62].hash_max;
	*out_high = ni->hints[63].hash_max;
	RAVENFS_MDATA_SESSION_RELEASE(nms);
	RAVENFS_MDATA_SESSION_RELEASE(vms);

	/* 右半孩子 reparent → new_blk */
	for (i = 0; i < 64; i++) {
		u64 child;
		struct Bptree_meta_t *cm;
		u32 cc, ci, cr;
		u64 cp;

		{
			const struct Bptree_ddentry_inner_node_t *tin;

			RAVENFS_MDATA_SESSION(tms, ravenfs_mdata_cache_of(sb),
					      new_blk);
			if (!RAVENFS_MDATA_SESSION_VALID(tms))
				return PTR_ERR(tms);
			tin = RAVENFS_MDATA_SESSION_DATA(tms);
			child = RFS_PTR_ADDR(tin->hints[i].sub_ptr);
		}	/* tms 由 cleanup 自动 put */

		RAVENFS_MDATA_SESSION(cms, ravenfs_mdata_cache_of(sb), child);
		if (!RAVENFS_MDATA_SESSION_VALID(cms))
			return PTR_ERR(cms);
		cm = RAVENFS_MDATA_SESSION_DATA(cms);
		err = rfs_meta_ok(cm, &cc, &ci, &cr, &cp);
		if (err || cr)
			return err ? err : -EIO;
		cm->parent_ptr.v = RFS_PP_PACK(cc, ci, 0, new_blk);
		RAVENFS_MDATA_SESSION_DIRTY(cms);
	}
	*out_new = new_blk;
	return 0;
}

/* 新建根内（根升长）；⭐ 显式写 ptrs[] + root ".." */
int rfs_new_root_inner(struct super_block *sb, u64 dir_ino, u64 left_blk, u64 left_max,
			      u64 right_blk, u64 right_max, u64 dotdot)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_inode_session *ds __free(ravenfs_inode_session) = ravenfs_inode_session_get(sb, dir_ino);
	struct Bptree_ddentry_inner_node_t *ri;
	u64 root_blk;

	if (IS_ERR(ds))
		return PTR_ERR(ds);
	root_blk = rfs_allocator_block_alloc_hard(sbi->alloc, 2, 1);
	if (root_blk == (u64)PTR_INVALID)
		return -ENOSPC;

	RAVENFS_MDATA_SESSION(rms, ravenfs_mdata_cache_of(sb), root_blk);
	if (!RAVENFS_MDATA_SESSION_VALID(rms)) {
		(void)rfs_allocator_block_free(sbi->alloc, root_blk, 2);
		return PTR_ERR(rms);
	}
	ri = RAVENFS_MDATA_SESSION_DATA(rms);
	rfs_node_init(ri, 2, 1, 1, dir_ino);
	rfs_set_addr(&ri->meta.ptrs[0], RFS_MASK52);	/* 预留 */
	rfs_set_addr(&ri->meta.ptrs[1], dotdot);	/* `..` 继承自旧根（非根目录 = 父目录 inum；根目录 = 自身） */
	rfs_set_addr(&ri->meta.ptrs[2], RFS_MASK52);
	rfs_set_addr(&ri->hints[0].sub_ptr, left_blk);
	ri->hints[0].hash_max = left_max;
	rfs_set_addr(&ri->hints[1].sub_ptr, right_blk);
	ri->hints[1].hash_max = right_max;
	RAVENFS_MDATA_SESSION_DIRTY(rms);
	RAVENFS_MDATA_SESSION_RELEASE(rms);

	/* 两子 reparent → new root；root 位清零；左叶清 ptrs[1]（原 ".." / prev） */
	{
		u64 kids[2] = { left_blk, right_blk };
		int k;

		for (k = 0; k < 2; k++) {
			struct Bptree_meta_t *km;
			u32 cc, ci, cr;
			u64 cp;
			int err;

			RAVENFS_MDATA_SESSION(kms, ravenfs_mdata_cache_of(sb),
					      kids[k]);
			if (!RAVENFS_MDATA_SESSION_VALID(kms))
				return PTR_ERR(kms);
			km = RAVENFS_MDATA_SESSION_DATA(kms);
			err = rfs_meta_ok(km, &cc, &ci, &cr, &cp);
			if (err)
				return err;
			km->parent_ptr.v = RFS_PP_PACK(cc, ci, 0, root_blk);
			RAVENFS_MDATA_SESSION_DIRTY(kms);
		}
	}

	ravenfs_inode_session_record(ds)->data_root = root_blk;
	RAVENFS_MDATA_DIRTY(ds->mdata_session);
	return 0;
}

/* 父层应用（+ 内裂上溯）。cur_victim 是父层里被替换/插入的旧子块号 */
int rfs_apply_up(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p,
			u32 level, u32 child_idx, u64 cur_victim, u64 low_max,
			u64 new_blk, u64 high_max)
{
	for (;;) {
		struct Bptree_ddentry_inner_node_t *nd;
		u32 pcnt, idx = child_idx, new_cnt;
		u32 is_root;
		int err = 0;

		{
			RAVENFS_MDATA_SESSION(ms, ravenfs_mdata_cache_of(sb),
					      p->blk[level]);
			if (!RAVENFS_MDATA_SESSION_VALID(ms))
				return PTR_ERR(ms);
			nd = RAVENFS_MDATA_SESSION_DATA(ms);
			if (rfs_meta_ok(&nd->meta, &pcnt, &(u32){0},
					&(u32){0}, &(u64){0}) ||
			    !RFS_PP_IS_INNER(nd->meta.parent_ptr) ||
			    idx >= pcnt || pcnt > RFS_DENTRY_ORDER ||
			    RFS_PTR_ADDR(nd->hints[idx].sub_ptr) != cur_victim)
				return -EIO;
			nd->hints[idx].hash_max = low_max;
			memmove(&nd->hints[idx + 2], &nd->hints[idx + 1],
				(size_t)(pcnt - idx - 1) *
				sizeof(struct ddentry_hint_t));
			rfs_set_addr(&nd->hints[idx + 1].sub_ptr, new_blk);
			nd->hints[idx + 1].hash_max = high_max;
			new_cnt = pcnt + 1;
			is_root = RFS_PP_IS_ROOT(nd->meta.parent_ptr);
			nd->meta.parent_ptr.v = RFS_PP_PACK(new_cnt, 1, is_root,
					RFS_PP_PTR(nd->meta.parent_ptr));
			RAVENFS_MDATA_SESSION_DIRTY(ms);
		}	/* 放锁后再裂变，避免同块嵌套会话 */

		if (new_cnt <= RFS_DENTRY_ORDER) {
			if (idx + 1 == new_cnt - 1)	/* 新 hint 是末位 → 父 max 变 */
				return rfs_propagate(sb, p, level, high_max);
			return 0;
		}
		/* 父溢出 → 内裂 */
		{
			u64 rblk, lmx, hmx, dotdot;

			err = rfs_inner_split(sb, p->blk[level], &rblk, &lmx, &hmx, &dotdot);
			if (err)
				return err;
			if (is_root)
				return rfs_new_root_inner(sb, dir_ino, p->blk[level], lmx, rblk, hmx, dotdot);
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

/* 内部节点孩子 reparent → node_blk（仅非根；改的是孩子的 parent_ptr） */
static int rfs_reparent_children(struct super_block *sb,
				 const struct Bptree_ddentry_inner_node_t *nd,
				 u64 node_blk, u32 cnt)
{
	u32 i;

	for (i = 0; i < cnt; i++) {
		u64 cblk = RFS_PTR_ADDR(nd->hints[i].sub_ptr);
		struct Bptree_meta_t *cm;
		u32 cc, ci, cr;
		u64 cp;
		int err;

		if (cblk == (u64)RFS_MASK52)
			return -EIO;
		RAVENFS_MDATA_SESSION(cms, ravenfs_mdata_cache_of(sb), cblk);
		if (!RAVENFS_MDATA_SESSION_VALID(cms))
			return PTR_ERR(cms);
		cm = RAVENFS_MDATA_SESSION_DATA(cms);
		err = rfs_meta_ok(cm, &cc, &ci, &cr, &cp);
		if (err || cr)
			return err ? err : -EIO;
		if (RFS_PP_PTR(cm->parent_ptr) != node_blk) {
			cm->parent_ptr.v = RFS_PP_PACK(cc, ci, 0, node_blk);
			RAVENFS_MDATA_SESSION_DIRTY(cms);
		}
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
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	RAVENFS_MDATA_SESSION_NULL(pms);
	RAVENFS_MDATA_SESSION_NULL(cms);
	RAVENFS_MDATA_SESSION_NULL(nms);
	struct Bptree_ddentry_inner_node_t *pi;
	struct Bptree_meta_t *cm, *nm;
	u32 pcnt, pin, proot, cc = 0, ci, cr, nc = 0, ni, nr;
	u64 pptr, cp, np, child_blk, nblk, nidx, total;
	bool is_inner;
	int ret = 0;

	*need = false;
	RAVENFS_MDATA_SESSION_OPEN(pms, ravenfs_mdata_cache_of(sb), parent_blk);
	if (!RAVENFS_MDATA_SESSION_VALID(pms))
		return PTR_ERR(pms);
	pi = RAVENFS_MDATA_SESSION_DATA(pms);
	ret = rfs_meta_ok(&pi->meta, &pcnt, &pin, &proot, &pptr);
	if (ret || !pin || idx >= pcnt) {
		ret = ret ? ret : -EIO;
		goto out;
	}
	child_blk = RFS_PTR_ADDR(pi->hints[idx].sub_ptr);
	RAVENFS_MDATA_SESSION_OPEN(cms, ravenfs_mdata_cache_of(sb), child_blk);
	if (!RAVENFS_MDATA_SESSION_VALID(cms)) {
		ret = PTR_ERR(cms);
		goto out;
	}
	cm = RAVENFS_MDATA_SESSION_DATA(cms);
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
	RAVENFS_MDATA_SESSION_OPEN(nms, ravenfs_mdata_cache_of(sb), nblk);
	if (!RAVENFS_MDATA_SESSION_VALID(nms)) {
		ret = PTR_ERR(nms);
		goto out;
	}
	nm = RAVENFS_MDATA_SESSION_DATA(nms);
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
			RAVENFS_MDATA_DIRTY(cms);
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
					RAVENFS_MDATA_SESSION(x,
						ravenfs_mdata_cache_of(sb), nnext);

					if (!RAVENFS_MDATA_SESSION_VALID(x)) {
						ret = PTR_ERR(x);
						goto out;
					}
					rfs_set_addr(&((struct Bptree_meta_t *)
						RAVENFS_MDATA_SESSION_DATA(x))->ptrs[1],
						child_blk);
					RAVENFS_MDATA_SESSION_DIRTY(x);
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
					RAVENFS_MDATA_SESSION(x,
						ravenfs_mdata_cache_of(sb), nprev);

					if (!RAVENFS_MDATA_SESSION_VALID(x)) {
						ret = PTR_ERR(x);
						goto out;
					}
					rfs_set_addr(&((struct Bptree_meta_t *)
						RAVENFS_MDATA_SESSION_DATA(x))->ptrs[2],
						child_blk);
					RAVENFS_MDATA_SESSION_DIRTY(x);
				}
			}
			PP_SET_COUNT(&cnd->meta, total);
			memset(&cnd->entries[total], 0,
			       (RFS_DENTRY_ORDER + 1 - total) * sizeof(struct ddentry_t));
			RAVENFS_MDATA_DIRTY(cms);
		}
		/* 邻居内容已搬入 child → 丢弃其缓存行 + 释放块 */
		RAVENFS_MDATA_SESSION_RELEASE(nms);
		ravenfs_mdata_drop(ravenfs_mdata_cache_of(sb), nblk);
		(void)rfs_allocator_block_free(sbi->alloc, nblk, 2);

		/* 父删 hint + 更新保留 hint 的 hash_max */
		{
			u32 del_idx = (idx == 0) ? 1 : (idx - 1);
			u32 kept_before = (idx == 0) ? 0 : (idx - 1);
			u32 kept_after = kept_before;	/* 恒等：del_idx 从不 < kept_before */

			if (del_idx < pcnt - 1)
				memmove(&pi->hints[del_idx], &pi->hints[del_idx + 1],
					(size_t)(pcnt - del_idx - 1) * sizeof(struct ddentry_hint_t));
			memset(&pi->hints[pcnt - 1], 0, sizeof(struct ddentry_hint_t));
			pcnt--;
			PP_SET_COUNT(&pi->meta, pcnt);
			pi->hints[kept_after].hash_max =
				rfs_node_max(ravenfs_msession_node(cms), is_inner, (u32)total);
			RAVENFS_MDATA_DIRTY(pms);
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
			RAVENFS_MDATA_DIRTY(lms);
			RAVENFS_MDATA_DIRTY(rms);
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
			RAVENFS_MDATA_DIRTY(lms);
			RAVENFS_MDATA_DIRTY(rms);
		}
		pi->hints[idx].hash_max =
			rfs_node_max(ravenfs_msession_node(cms), is_inner, child_left ? left : right);
		pi->hints[nidx].hash_max =
			rfs_node_max(ravenfs_msession_node(nms), is_inner, child_left ? right : left);
		RAVENFS_MDATA_DIRTY(pms);
		*need = false;
	}

out:
	return ret;	/* pms/cms/nms 由 cleanup 按 LIFO 自动 put */
}

/* 根收缩：根（内）只剩 1 孩时，把唯一孩子提升为新根（读 dir inode 的 data_root） */
static int rfs_root_shrink(struct super_block *sb, u64 dir_ino)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_inode_session *ds __free(ravenfs_inode_session) = ravenfs_inode_session_get(sb, dir_ino);
	RAVENFS_MDATA_SESSION_NULL(rms);
	RAVENFS_MDATA_SESSION_NULL(cms);
	struct Bptree_meta_t *rm, *cm;
	u64 root_blk, child_blk;
	u32 rc, rin, rrt, cc, ci, crr;
	u64 rp, cp;
	int err;

	if (IS_ERR(ds))
		return PTR_ERR(ds);
	root_blk = ravenfs_inode_session_record(ds)->data_root;
	if (root_blk == (u64)PTR_INVALID)
		return -EIO;
	RAVENFS_MDATA_SESSION_OPEN(rms, ravenfs_mdata_cache_of(sb), root_blk);
	if (!RAVENFS_MDATA_SESSION_VALID(rms))
		return PTR_ERR(rms);
	rm = RAVENFS_MDATA_SESSION_DATA(rms);
	err = rfs_meta_ok(rm, &rc, &rin, &rrt, &rp);
	if (err)
		goto out;
	if (!rin || rc != 1)		/* 根叶 / 根内非单孩 → 无需收缩 */
		goto out;

	child_blk = RFS_PTR_ADDR(((struct Bptree_ddentry_inner_node_t *)rm)->hints[0].sub_ptr);
	RAVENFS_MDATA_SESSION_OPEN(cms, ravenfs_mdata_cache_of(sb), child_blk);
	if (!RAVENFS_MDATA_SESSION_VALID(cms)) {
		err = PTR_ERR(cms);
		goto out;
	}
	cm = RAVENFS_MDATA_SESSION_DATA(cms);
	err = rfs_meta_ok(cm, &cc, &ci, &crr, &cp);
	if (err || crr) {
		err = err ? err : -EIO;
		goto out;
	}
	cm->parent_ptr.v = RFS_PP_PACK(cc, ci, 1, dir_ino);
	if (!ci) {
		struct Bptree_ddentry_leaf_node_t *cl = ravenfs_msession_node(cms);

		rfs_set_addr(&cl->meta.ptrs[0], RFS_MASK52);
		rfs_set_addr(&cl->meta.ptrs[1], RFS_PTR_ADDR(rm->ptrs[1]));	/* ".." 继承自旧根 */
		rfs_set_addr(&cl->meta.ptrs[2], RFS_MASK52);
	} else {
		struct Bptree_ddentry_inner_node_t *cin = ravenfs_msession_node(cms);

		rfs_set_addr(&cin->meta.ptrs[1], RFS_PTR_ADDR(rm->ptrs[1]));	/* ".." 继承自旧根 */
	}
	RAVENFS_MDATA_DIRTY(cms);

	ravenfs_inode_session_record(ds)->data_root = child_blk;
	RAVENFS_MDATA_DIRTY(ds->mdata_session);

	ravenfs_mdata_drop(ravenfs_mdata_cache_of(sb), root_blk);
	(void)rfs_allocator_block_free(sbi->alloc, root_blk, 2);
out:
	return err;	/* rms/cms 由 cleanup 按 LIFO 自动 put */
}

/* 欠载上溯：从叶的父层一路 balance；收尾统一尝试根收缩。返回 <0 错误 */
int rfs_balance_up(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p)
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

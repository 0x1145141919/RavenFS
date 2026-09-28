// SPDX-License-Identifier: GPL-2.0
/*
 * dentry_tree.c —— FS 顶层原语：dentry B+树的条目/路径级操作
 *
 * 【三分拆分（2026-09-28）】自 dentry.c 迁入 dir_find / read_leaf / leftmost_leaf /
 * set_dotdot / swap_entries + mkdir 空根叶段抽出的 root_leaf_init；自 dentry_write.c
 * 迁入 walk / insert / delete。职责：(dir_ino, name) → 路径 → 条目 的树级语义；
 * 结构变更（分裂/平衡/布局构造）全部委托 dentry_node.c。消费方 = dentry.c（接待层）。
 *
 * ★ 原 dentry_write.c 文件头（规格/验收/移植要点）原样保留在此 —— 随函数下迁的
 *   行内坑注在 dentry_node.c，维护时两头对照：
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
 *
 * 导出面收口在 ravenfs_dentry_tree.h；本层对底层的依赖经 ravenfs_dentry_node.h。
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xxhash.h>
#include <linux/timekeeping.h>

#include "ravenfs.h"
#include "ravenfs_dentry_tree.h"
#include "ravenfs_dentry_node.h"
#include "ravenfs_cache.h"
#include "ravenfs_mdata_cache.h"

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
		u64 new_blk, low_max, high_max, dotdot;
		bool was_root;

		err = rfs_leaf_split(sb, leaf_blk, &new_blk, &low_max, &high_max, &was_root, &dotdot);
		if (err)
			return err;
		if (p->depth == 1)
			return rfs_new_root_inner(sb, dir_ino, leaf_blk, low_max, new_blk, high_max, dotdot);
		return rfs_apply_up(sb, dir_ino, p, p->depth - 2, p->cidx[p->depth - 2],
				    leaf_blk, low_max, new_blk, high_max);
	}
}

/* ══════════════════════════════════════════════════════════════════
 * 删除路径（M5b）：叶条目删除 + hash_max 上溯 + 欠载平衡/合并/抹匀 + 根收缩
 * 规格：`Docs/linux/ravenfs-linux-ko-handover.md` §4（god dentry_insert_delete / dentry_balance 移植）
 * ══════════════════════════════════════════════════════════════════ */

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

int ravenfs_dir_find(struct super_block *sb, u64 dir_ino,
			const char *name, u32 nlen, u64 *ino)
{
	u64 hash = xxh64(name, nlen, 0);
	u64 blk;
	u32 depth = 0;
	void *buf;
	int err = 0;
	struct ravenfs_session *ses __free(ravenfs_session) = ravenfs_session_get(sb, dir_ino);

	*ino = (u64)PTR_INVALID;
	if (IS_ERR(ses))
		return PTR_ERR(ses);
	blk = ravenfs_session_inode(ses)->data_root;	/* 盘记录经 session 取（§4.11.1） */
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	for (;;) {
		const struct Bptree_meta_t *meta;
		u32 cnt, i, inner;

		if (++depth > 32 || blk == (u64)PTR_INVALID) {
			err = -EIO;
			break;
		}
		err = ravenfs_read_node(sb, blk, buf);
		if (err)
			break;
		meta = buf;
		if (meta->magics[0] != DENTRY_MAGIC1 || meta->magics[1] != DENTRY_MAGIC2 ||
		    meta->magics[2] != DENTRY_MAGIC3 || meta->magics[3] != DENTRY_MAGIC4) {
			err = -EIO;
			break;
		}
		cnt = RFS_PP_COUNT(meta->parent_ptr);
		inner = RFS_PP_IS_INNER(meta->parent_ptr);
		if (cnt > 126) {
			err = -EIO;
			break;
		}
		if (!inner) {
			err = ravenfs_leaf_lookup(sb, buf, cnt, name, nlen, hash, ino);
			break;
		}
		{
			const struct Bptree_ddentry_inner_node_t *nd = buf;
			u32 sel = cnt;		/* cnt = 无匹配 */

			for (i = 0; i < cnt; i++) {
				if (hash <= nd->hints[i].hash_max) {
					sel = i;
					break;
				}
			}
			if (sel == cnt)		/* 超出所有 hash_max → miss */
				break;
			blk = RFS_PTR_ADDR(nd->hints[sel].sub_ptr);
		}
	}
	kfree(buf);
	return err;
}

/* ── create / unlink（M5：多级 ddentry 树 walk / insert / delete） ── */

int ravenfs_read_leaf(struct super_block *sb, u64 blk, void *buf, u32 *cnt)
{
	const struct Bptree_meta_t *m;
	int err = ravenfs_read_node(sb, blk, buf);

	if (err)
		return err;
	RFS_STAT_INC(sb, dir_leaf_reads);	/* readdir/lookup 读叶子次数 */
	m = buf;
	if (m->magics[0] != DENTRY_MAGIC1 || m->magics[1] != DENTRY_MAGIC2 ||
	    m->magics[2] != DENTRY_MAGIC3 || m->magics[3] != DENTRY_MAGIC4) {
		RFS_STAT_INC(sb, err_other);	/* 兜底：未归类错误返回 */
		return -EIO;
	}
	if (RFS_PP_IS_INNER(m->parent_ptr))
		return -EIO;
	*cnt = RFS_PP_COUNT(m->parent_ptr);
	return (*cnt > 126) ? -EIO : 0;
}

/* 从 root 一路沿 hints[0] 下到最左叶子 */
int ravenfs_leftmost_leaf(struct super_block *sb, u64 blk, u64 *leaf)
{
	void *buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	u32 depth = 0;
	int err = 0;

	if (!buf)
		return -ENOMEM;
	for (;;) {
		const struct Bptree_meta_t *m;
		u32 n;

		if (++depth > 32) { err = -EIO; break; }
		err = ravenfs_read_node(sb, blk, buf);
		if (err)
			break;
		m = buf;
		if (m->magics[0] != DENTRY_MAGIC1 || m->magics[1] != DENTRY_MAGIC2 ||
		    m->magics[2] != DENTRY_MAGIC3 || m->magics[3] != DENTRY_MAGIC4) {
			err = -EIO;
			break;
		}
		n = RFS_PP_COUNT(m->parent_ptr);
		if (!RFS_PP_IS_INNER(m->parent_ptr)) {
			*leaf = blk;
			break;
		}
		if (n == 0 || n > 126) { err = -EIO; break; }
		blk = RFS_PTR_ADDR(((const struct Bptree_ddentry_inner_node_t *)buf)->hints[0].sub_ptr);
	}
	kfree(buf);
	return err;
}

/* 写目录的盘上父指针（根节点 ptrs[1] = ".."；叶/内根一致） */
int rfs_dir_set_dotdot(struct super_block *sb, u64 dir_ino, u64 parent_ino)
{
	struct ravenfs_session *ds __free(ravenfs_session) = ravenfs_session_get(sb, dir_ino);
	struct ravenfs_msession *ms __free(ravenfs_msession) = NULL;
	struct Bptree_meta_t *m;
	u64 root;

	if (IS_ERR(ds))
		return PTR_ERR(ds);
	root = ravenfs_session_inode(ds)->data_root;
	if (root == (u64)PTR_INVALID)
		return -EIO;
	ms = ravenfs_mdata_get(sb, root);
	if (IS_ERR(ms))
		return PTR_ERR(ms);
	m = ravenfs_msession_node(ms);
	m->ptrs[1].v = RFS_PTR_PACK(0, parent_ino);
	RFS_ROW_MARK_DIRTY(ms->row);
	return 0;
}

/* 交换两条目的 inode_ptr（EXCHANGE）；两条可能共叶 */
int rfs_swap_entries(struct super_block *sb, const struct rfs_dpath *op,
			    const struct rfs_dpath *np)
{
	u64 oblk = op->blk[op->depth - 1], nblk = np->blk[np->depth - 1];
	u32 oi = op->cidx[op->depth - 1], ni = np->cidx[np->depth - 1];
	struct ravenfs_msession *oms, *nms = NULL;
	struct Bptree_ddentry_leaf_node_t *ol, *nl;
	u64 tmp;

	oms = ravenfs_mdata_get(sb, oblk);
	if (IS_ERR(oms))
		return PTR_ERR(oms);
	ol = ravenfs_msession_node(oms);
	if (oblk == nblk) {
		tmp = ol->entries[oi].inode_ptr.v;
		ol->entries[oi].inode_ptr.v = ol->entries[ni].inode_ptr.v;
		ol->entries[ni].inode_ptr.v = tmp;
		NODE_DIRTY(oms);
		ravenfs_mdata_put(oms);
		return 0;
	}
	nms = ravenfs_mdata_get(sb, nblk);
	if (IS_ERR(nms)) {
		ravenfs_mdata_put(oms);
		return PTR_ERR(nms);
	}
	nl = ravenfs_msession_node(nms);
	tmp = ol->entries[oi].inode_ptr.v;
	ol->entries[oi].inode_ptr.v = nl->entries[ni].inode_ptr.v;
	nl->entries[ni].inode_ptr.v = tmp;
	NODE_DIRTY(oms);
	NODE_DIRTY(nms);
	ravenfs_mdata_put(nms);
	ravenfs_mdata_put(oms);
	return 0;
}

/*
 * 新目录空根叶初始化（自 ravenfs_mkdir 内联段抽出；2026-09-28 三分拆分）：
 * 接待层零结构变更红线 —— magics/parent_ptr/52 位哨兵的布局构造知识归顶层原语。
 * 语义（v1.1 §10 不变量 11）：count=0, root=1, ptr=自身 inum(".")，ptrs[1]=".."=父目录。
 */
int ravenfs_dir_root_leaf_init(struct super_block *sb, u64 blk, u64 inum, u64 parent_ino)
{
	struct ravenfs_msession *ms;
	struct Bptree_ddentry_leaf_node_t *lf;

	ms = ravenfs_mdata_get(sb, blk);
	if (IS_ERR(ms))
		return PTR_ERR(ms);
	lf = ravenfs_msession_node(ms);
	memset(lf, 0, RAVENFS_NODE_BYTES);
	lf->meta.magics[0] = DENTRY_MAGIC1;
	lf->meta.magics[1] = DENTRY_MAGIC2;
	lf->meta.magics[2] = DENTRY_MAGIC3;
	lf->meta.magics[3] = DENTRY_MAGIC4;
	lf->meta.parent_ptr.v = RFS_PP_PACK(0, 0, 1, inum);
	lf->meta.ptrs[0].v = RFS_PTR_PACK(0, RFS_MASK52);
	lf->meta.ptrs[1].v = RFS_PTR_PACK(0, parent_ino);	/* ".." */
	lf->meta.ptrs[2].v = RFS_PTR_PACK(0, RFS_MASK52);
	RFS_ROW_MARK_DIRTY(ms->row);
	ravenfs_mdata_put(ms);
	return 0;
}

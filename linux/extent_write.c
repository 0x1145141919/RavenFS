// SPDX-License-Identifier: GPL-2.0
/*
 * extent_write.c —— RavenFS ko：文件数据**区段树（extent B+tree）写机械**（M4.2 第一版）
 *
 * 依据：Docs/linux/ravenfs-linux-ko-M4-data-spec.md §1.2/§2.3/§3/§8（立刻分配 D-M4-6）；
 *       Docs/disk_layout_v1.1.md §5（区段树 entry/路由/分裂/平衡）+ §10 不变量 8/9/10/31–36
 *       （阶 337 / 左 168 / 欠载 <168 / 合并 ≤337 / 深度 64 / 节点 8K 对齐）；
 *       研究报告 m4-research/C-god-extent.md（god 机械伪规格，理念参考非照搬）。
 * 范式：linux/dentry_write.c（同一「树写机械」骨架：walk → 叶插 → 叶裂 → 升根 → 刷父 key）。
 *
 * 【M4.2 第一版范围（诚实边界）】
 *   · 只做 **插入**：多级 walk（严格上界路由）+ 叶插入 + 叶裂（左 168）+ 内裂 + 升根 + 父 key 上溯。
 *     **不做** 删除 / 合并 / 抹匀 / 根收缩 / 出口坍缩 / rebuild / iterator —— 属 M4.3/M4.5。
 *   · 无「四形态融合」：与既有 extent 紧贴承载 phys 连续时不自动合并（v1 允许碎片，交后续收敛）。
 *   · 路由：**严格上界**（`target < key → 左子树`；`key = 子树最大开边界 fblkbase+length`）——
 *     与 dentry 的 `≤ hash_max` 方向**相反**，勿照抄。
 *   · 阈值：阶 337（槽 338，[337] 瞬时槽仅内存）；叶/内裂左 168；深度上限 64；
 *     节点块 = `ravenfs_block_alloc(sb, 2, 1)`（2 块连续 + 8K 对齐）。
 *   · 三态迁移（inline→extent / direct→extent）的**盘面副作用**由 iomap.c 承担；本文件仅树机械。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/timekeeping.h>

#include "ravenfs.h"

#define RFS_EXT_ORDER		337u	/* 阶 */
#define RFS_EXT_MAX_SLOTS	338u	/* 槽 [0,337]；[337] 瞬时 */
#define RFS_EXT_SPLIT		168u	/* 分裂左半条数（左 168 / 右 count-168） */
#define RFS_EXT_DEPTH_MAX	64u	/* 深度上限（超限判成环） */
#define RFS_EXT_GLB_NONE	0xFFFFu	/* 叶内 GLB 无下界哨兵（u16） */

#define RFS_EXT_PTR_INV		RFS_PTR_PACK(0, RFS_MASK52)

#define RFS_EXT_SET_COUNT(m, c) \
	do { (m)->parent_ptr.v = RFS_PP_PACK((u32)(c), RFS_PP_IS_INNER((m)->parent_ptr), \
				RFS_PP_IS_ROOT((m)->parent_ptr), RFS_PP_PTR((m)->parent_ptr)); } while (0)

/* 多级路径（叶在 depth-1；cidx[内] = hint idx；cidx[叶] = GLB，NONE=插入最左） */
struct rfs_epath {
	u32	depth;
	u64	blk[RFS_EXT_DEPTH_MAX];
	u16	cidx[RFS_EXT_DEPTH_MAX];
	bool	hit;
	u16	ins_idx;
};

/* ══ 节点 / meta 基建 ══ */

static int rfs_ext_meta_ok_x(const struct Bptree_meta_t *m, u32 *cnt, u32 *inner,
			     u32 *root, u64 *pptr, u32 maxc)
{
	if (m->magics[0] != BPTREE_MAGIC1 || m->magics[1] != BPTREE_MAGIC2 ||
	    m->magics[2] != BPTREE_MAGIC3 || m->magics[3] != BPTREE_MAGIC4)
		return -EIO;
	*cnt = RFS_PP_COUNT(m->parent_ptr);
	*inner = RFS_PP_IS_INNER(m->parent_ptr);
	*root = RFS_PP_IS_ROOT(m->parent_ptr);
	*pptr = RFS_PP_PTR(m->parent_ptr);
	if (*cnt > maxc)
		return -EIO;
	return 0;
}

static int rfs_ext_meta_ok(const struct Bptree_meta_t *m, u32 *cnt, u32 *inner,
			   u32 *root, u64 *pptr)
{
	return rfs_ext_meta_ok_x(m, cnt, inner, root, pptr, RFS_EXT_MAX_SLOTS);
}

/* 节点子树最大开边界（叶取末项 fblkbase+length；内取末 hint key）；空 → 0 */
static u64 rfs_ext_node_max(const void *node, u32 inner, u32 cnt)
{
	const struct Bptree_meta_t *m = node;

	if (cnt == 0)
		return 0;
	if (inner)
		return ((const struct Bptree_extent_inner_node_t *)m)->extents[cnt - 1].key;
	return ((const struct Bptree_extent_leaf_node_t *)m)->extents[cnt - 1].fblkbase +
	       ((const struct Bptree_extent_leaf_node_t *)m)->extents[cnt - 1].length;
}

static void rfs_ext_node_init(void *node, u32 count, u32 inner, u32 root, u64 ptr)
{
	struct Bptree_meta_t *m = node;

	memset(node, 0, RAVENFS_NODE_BYTES);
	m->magics[0] = BPTREE_MAGIC1;
	m->magics[1] = BPTREE_MAGIC2;
	m->magics[2] = BPTREE_MAGIC3;
	m->magics[3] = BPTREE_MAGIC4;
	m->parent_ptr.v = RFS_PP_PACK(count, inner, root, ptr);
	m->ptrs[0].v = RFS_EXT_PTR_INV;
	m->ptrs[1].v = RFS_EXT_PTR_INV;
	m->ptrs[2].v = RFS_EXT_PTR_INV;
}

static inline void rfs_ext_set_addr(struct ptr_in_idx *p, u64 addr)
{
	p->v = RFS_PTR_PACK(0, addr);
}

/* ══ 查询：严格上界路由 + 叶内 GLB ══
 * 返回：0 = 命中（*out）；1 = 未命中（洞，*next=下一 extent 起点块号，0=无）；<0 = 错误。 */
int ravenfs_extent_lookup(struct super_block *sb, u64 root, u64 target,
			  struct extent_t *out, u64 *next)
{
	u64 blk = root;
	u32 depth = 0;
	void *buf;
	int err = -EIO;

	*next = 0;
	if (root == (u64)PTR_INVALID)
		return 1;			/* 空树 → 全洞 */

	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (;;) {
		const struct Bptree_meta_t *meta;
		u32 cnt, inner, rootf, i;
		u64 pptr;

		if (++depth > RFS_EXT_DEPTH_MAX || blk == (u64)PTR_INVALID) {
			err = -EIO;
			break;
		}
		err = ravenfs_read_node(sb, blk, buf);
		if (err)
			break;
		meta = buf;
		err = rfs_ext_meta_ok(meta, &cnt, &inner, &rootf, &pptr);
		if (err)
			break;

		if (inner) {
			const struct Bptree_extent_inner_node_t *nd = buf;
			u32 lo = 0, hi = cnt, sel;

			if (cnt == 0) {	/* 空内（非法） */
				err = -EIO;
				break;
			}
			/* 第一个 key > target；全 ≤ target → 最右（右击穿特化） */
			while (lo < hi) {
				u32 mid = lo + (hi - lo) / 2;

				if (nd->extents[mid].key <= target)
					lo = mid + 1;
				else
					hi = mid;
			}
			sel = (lo < cnt) ? lo : cnt - 1;
			blk = RFS_PTR_ADDR(nd->extents[sel].sub_node_ptr);
			continue;
		}

		/* 叶：二分求 GLB（最大 fblkbase ≤ target），并判覆盖 */
		{
			const struct Bptree_extent_leaf_node_t *lf = buf;
			u32 lo = 0, hi = cnt;
			int glb = -1;

			while (lo < hi) {
				u32 mid = lo + (hi - lo) / 2;

				if (lf->extents[mid].fblkbase <= target)
					lo = mid + 1;
				else
					hi = mid;
			}
			if (lo > 0)
				glb = (int)lo - 1;

			err = 1;
			if (glb >= 0) {
				const struct extent_t *e = &lf->extents[glb];

				/* 防 64 位加溢出：target - fblkbase < length */
				if (target - e->fblkbase < e->length) {
					*out = *e;
					err = 0;
				}
			}
			if (err == 1) {	/* 洞：下一 extent 起点 */
				i = (u32)(glb + 1);
				*next = (i < cnt) ? lf->extents[i].fblkbase : 0;
			}
			break;
		}
	}

	kfree(buf);
	return err;
}

/*
 * 计数（i_blocks 诚实化用）：递归遍历，累加数据块（叶 length 之和）与节点数（每节点 8K=2 块）。
 * 失败/超深时按已知部分返回（不致命）。
 */
/* 计数递归（depth 护栏防成环） */
static int rfs_ext_count_rec(struct super_block *sb, u64 root, u64 *data, u64 *nodes,
			     u32 depth)
{
	const struct Bptree_meta_t *m;
	void *buf;
	u32 cnt, inner, i;

	if (root == (u64)PTR_INVALID || depth > RFS_EXT_DEPTH_MAX)
		return 0;
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	if (ravenfs_read_node(sb, root, buf)) {
		kfree(buf);
		return 0;
	}
	m = buf;
	if (rfs_ext_meta_ok(m, &cnt, &inner, &(u32){0}, &(u64){0})) {
		kfree(buf);
		return 0;
	}
	(*nodes)++;
	if (inner) {
		const struct Bptree_extent_inner_node_t *nd = buf;

		for (i = 0; i < cnt; i++)
			rfs_ext_count_rec(sb, RFS_PTR_ADDR(nd->extents[i].sub_node_ptr),
					  data, nodes, depth + 1);
	} else {
		const struct Bptree_extent_leaf_node_t *lf = buf;

		for (i = 0; i < cnt; i++)
			*data += lf->extents[i].length;
	}
	kfree(buf);
	return 0;
}

/* 节点/数据块计数（i_blocks 诚实化用） */
int ravenfs_extent_count(struct super_block *sb, u64 root, u64 *data, u64 *nodes)
{
	return rfs_ext_count_rec(sb, root, data, nodes, 0);
}

/* ══ 多级 walk：填 path（blk/cidx）、hit、插入位 ══ */
static int rfs_ext_walk(struct super_block *sb, u64 root, u64 target, struct rfs_epath *p)
{
	void *buf;
	u64 blk = root;
	u32 depth = 0;
	int err = 0;

	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	p->hit = false;
	for (;;) {
		u32 cnt, inner, rootf;
		u64 pptr;

		if (depth >= RFS_EXT_DEPTH_MAX) { err = -EIO; break; }
		err = ravenfs_read_node(sb, blk, buf);
		if (err)
			break;
		err = rfs_ext_meta_ok(buf, &cnt, &inner, &rootf, &pptr);
		if (err)
			break;
		if ((depth == 0) != (rootf != 0)) { err = -EIO; break; }	/* 第 0 层必须 root */
		p->blk[depth] = blk;

		if (!inner) {
			const struct Bptree_extent_leaf_node_t *lf = buf;
			u32 lo = 0, hi = cnt;
			int glb = -1;

			while (lo < hi) {
				u32 mid = lo + (hi - lo) / 2;

				if (lf->extents[mid].fblkbase <= target)
					lo = mid + 1;
				else
					hi = mid;
			}
			if (lo > 0)
				glb = (int)lo - 1;
			if (glb >= 0) {
				const struct extent_t *e = &lf->extents[glb];

				if (target - e->fblkbase < e->length)
					p->hit = true;
			}
			p->cidx[depth] = (glb < 0) ? RFS_EXT_GLB_NONE : (u16)glb;
			p->ins_idx = (glb < 0) ? 0 : (u16)(glb + 1);
			depth++;
			break;
		} else {
			const struct Bptree_extent_inner_node_t *nd = buf;
			u32 lo = 0, hi = cnt, sel;

			if (cnt == 0) { err = -EIO; break; }
			while (lo < hi) {
				u32 mid = lo + (hi - lo) / 2;

				if (nd->extents[mid].key <= target)
					lo = mid + 1;
				else
					hi = mid;
			}
			sel = (lo < cnt) ? lo : cnt - 1;
			p->cidx[depth] = (u16)sel;
			blk = RFS_PTR_ADDR(nd->extents[sel].sub_node_ptr);
			depth++;
		}
	}
	p->depth = depth;
	kfree(buf);
	return err;
}

/* 自 child_level 起向上传播 max（父 hint key 变更）；父 max 未变则短路 */
static int rfs_ext_propagate(struct super_block *sb, const struct rfs_epath *p,
			     u32 child_level, u64 cur_max)
{
	int lvl;

	for (lvl = (int)child_level - 1; lvl >= 0; lvl--) {
		struct ravenfs_msession *ms = ravenfs_mdata_get(sb, p->blk[lvl]);
		struct Bptree_extent_inner_node_t *nd;
		u32 cnt, idx = p->cidx[lvl];

		if (IS_ERR(ms))
			return PTR_ERR(ms);
		nd = ravenfs_msession_node(ms);
		if (!RFS_PP_IS_INNER(nd->meta.parent_ptr) ||
		    rfs_ext_meta_ok(&nd->meta, &cnt, &(u32){0}, &(u32){0}, &(u64){0}) ||
		    idx >= cnt) {
			ravenfs_mdata_put(ms);
			return -EIO;
		}
		nd->extents[idx].key = cur_max;
		NODE_DIRTY(ms);
		if (idx == cnt - 1) {		/* 本节点 max 亦变 → 继续上溯 */
			ravenfs_mdata_put(ms);
			continue;
		}
		ravenfs_mdata_put(ms);
		break;
	}
	return 0;
}

/* 叶裂：victim(count=338) → 左 168 / 右 170；维护叶链；返回 low/high max + 新叶块号 */
static int rfs_ext_leaf_split(struct super_block *sb, u64 victim_blk, u64 *out_new,
			      u64 *out_low, u64 *out_high, bool *out_was_root)
{
	struct ravenfs_msession *vms = ravenfs_mdata_get(sb, victim_blk);
	struct ravenfs_msession *nms = NULL;
	struct Bptree_extent_leaf_node_t *vl, *nl;
	u32 cnt, inner, root;
	u64 pptr, parent, old_next, new_blk = (u64)PTR_INVALID;
	const u32 right = RFS_EXT_MAX_SLOTS - RFS_EXT_SPLIT;	/* 338-168 = 170 */
	bool was_root;
	int err;

	if (IS_ERR(vms))
		return PTR_ERR(vms);
	vl = ravenfs_msession_node(vms);
	err = rfs_ext_meta_ok_x(&vl->meta, &cnt, &inner, &root, &pptr, RFS_EXT_MAX_SLOTS);
	if (err || inner || cnt != RFS_EXT_MAX_SLOTS) {
		ravenfs_mdata_put(vms);
		return err ? err : -EIO;
	}
	was_root = root != 0;
	parent = pptr;
	old_next = RFS_PTR_ADDR(vl->meta.ptrs[2]);	/* ⚠ 先读后写 */
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
	vl = ravenfs_msession_node(vms);
	nl = ravenfs_msession_node(nms);

	rfs_ext_node_init(nl, right, 0, 0, parent);
	memcpy(&nl->extents[0], &vl->extents[RFS_EXT_SPLIT],
	       (size_t)right * sizeof(struct extent_t));
	rfs_ext_set_addr(&nl->meta.ptrs[1], victim_blk);			/* prev */
	nl->meta.ptrs[2].v = was_root ? RFS_EXT_PTR_INV : RFS_PTR_PACK(0, old_next);

	vl->meta.parent_ptr.v = RFS_PP_PACK(RFS_EXT_SPLIT, 0, root, parent);	/* 左 168 */
	rfs_ext_set_addr(&vl->meta.ptrs[2], new_blk);				/* next = new */
	if (was_root)
		vl->meta.ptrs[1].v = RFS_EXT_PTR_INV;				/* 根叶无 prev */

	NODE_DIRTY(vms);
	NODE_DIRTY(nms);
	*out_low = rfs_ext_node_max(vl, 0, RFS_EXT_SPLIT);
	*out_high = rfs_ext_node_max(nl, 0, right);
	*out_was_root = was_root;
	ravenfs_mdata_put(vms);
	ravenfs_mdata_put(nms);

	/* 旧后继 prev = new */
	if (!was_root && old_next != (u64)RFS_MASK52) {
		struct ravenfs_msession *oms = ravenfs_mdata_get(sb, old_next);

		if (IS_ERR(oms))
			return PTR_ERR(oms);
		rfs_ext_set_addr(&((struct Bptree_meta_t *)ravenfs_msession_node(oms))->ptrs[1], new_blk);
		NODE_DIRTY(oms);
		ravenfs_mdata_put(oms);
	}
	*out_new = new_blk;
	return 0;
}

/* 内裂：victim(count=338) → 左 168 / 右 170；右半孩子 reparent → 新内块 */
static int rfs_ext_inner_split(struct super_block *sb, u64 victim_blk, u64 *out_new,
			       u64 *out_low, u64 *out_high)
{
	struct ravenfs_msession *vms = ravenfs_mdata_get(sb, victim_blk);
	struct ravenfs_msession *nms = NULL;
	struct Bptree_extent_inner_node_t *vi, *ni;
	u32 cnt, inner, root, i;
	u64 pptr, parent, new_blk = (u64)PTR_INVALID;
	const u32 right = RFS_EXT_MAX_SLOTS - RFS_EXT_SPLIT;
	int err;

	if (IS_ERR(vms))
		return PTR_ERR(vms);
	vi = ravenfs_msession_node(vms);
	err = rfs_ext_meta_ok_x(&vi->meta, &cnt, &inner, &root, &pptr, RFS_EXT_MAX_SLOTS);
	if (err || !inner || cnt != RFS_EXT_MAX_SLOTS) {
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

	rfs_ext_node_init(ni, right, 1, 0, parent);
	memcpy(&ni->extents[0], &vi->extents[RFS_EXT_SPLIT],
	       (size_t)right * sizeof(struct extent_hint_t));
	vi->meta.parent_ptr.v = RFS_PP_PACK(RFS_EXT_SPLIT, 1, root, parent);
	NODE_DIRTY(vms);
	NODE_DIRTY(nms);
	*out_low = vi->extents[RFS_EXT_SPLIT - 1].key;
	*out_high = ni->extents[right - 1].key;
	ravenfs_mdata_put(vms);
	ravenfs_mdata_put(nms);

	/* 右半孩子 reparent → new_blk（逐个开子会话改 parent_ptr） */
	for (i = 0; i < right; i++) {
		struct ravenfs_msession *xms = ravenfs_mdata_get(sb, new_blk);
		const struct Bptree_extent_inner_node_t *xin;
		struct ravenfs_msession *cms;
		struct Bptree_meta_t *cm;
		u32 cc, ci, cr;
		u64 cp, child;

		if (IS_ERR(xms))
			return PTR_ERR(xms);
		xin = ravenfs_msession_node(xms);
		child = RFS_PTR_ADDR(xin->extents[i].sub_node_ptr);
		ravenfs_mdata_put(xms);

		cms = ravenfs_mdata_get(sb, child);
		if (IS_ERR(cms))
			return PTR_ERR(cms);
		cm = ravenfs_msession_node(cms);
		err = rfs_ext_meta_ok(cm, &cc, &ci, &cr, &cp);
		if (err) {
			ravenfs_mdata_put(cms);
			return err;
		}
		cm->parent_ptr.v = RFS_PP_PACK(cc, ci, 0, new_blk);
		NODE_DIRTY(cms);
		ravenfs_mdata_put(cms);
	}
	*out_new = new_blk;
	return 0;
}

/* 新建根内（根升长）；显式写 ptrs[]（文件树 ".." = PTR_INVALID） */
static int rfs_ext_new_root_inner(struct super_block *sb, u64 ino, u64 left_blk, u64 left_max,
				  u64 right_blk, u64 right_max, u64 *root_out)
{
	struct ravenfs_msession *rms;
	struct Bptree_extent_inner_node_t *ri;
	u64 root_blk, kids[2];
	int k, err = 0;

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
	rfs_ext_node_init(ri, 2, 1, 1, ino);
	rfs_ext_set_addr(&ri->meta.ptrs[1], RFS_MASK52);	/* 文件树无 ".." */
	rfs_ext_set_addr(&ri->extents[0].sub_node_ptr, left_blk);
	ri->extents[0].key = left_max;
	rfs_ext_set_addr(&ri->extents[1].sub_node_ptr, right_blk);
	ri->extents[1].key = right_max;
	NODE_DIRTY(rms);
	ravenfs_mdata_put(rms);

	kids[0] = left_blk;
	kids[1] = right_blk;
	for (k = 0; k < 2; k++) {
		struct ravenfs_msession *kms = ravenfs_mdata_get(sb, kids[k]);
		struct Bptree_meta_t *km;
		u32 cc, ci, cr;
		u64 cp;

		if (IS_ERR(kms))
			return PTR_ERR(kms);
		km = ravenfs_msession_node(kms);
		err = rfs_ext_meta_ok(km, &cc, &ci, &cr, &cp);
		if (err) {
			ravenfs_mdata_put(kms);
			return err;
		}
		km->parent_ptr.v = RFS_PP_PACK(cc, ci, 0, root_blk);
		NODE_DIRTY(kms);
		ravenfs_mdata_put(kms);
	}
	*root_out = root_blk;
	return 0;
}

/* 父层应用（+ 内裂上溯 + 升根）。cur_victim = 父层里被替换/插入的旧子块号 */
static int rfs_ext_apply_up(struct super_block *sb, u64 ino, const struct rfs_epath *p,
			    u32 level, u32 child_idx, u64 cur_victim, u64 low_max,
			    u64 new_blk, u64 high_max, u64 *root_io)
{
	for (;;) {
		struct ravenfs_msession *ms = ravenfs_mdata_get(sb, p->blk[level]);
		struct Bptree_extent_inner_node_t *nd;
		u32 pcnt, idx = child_idx, new_cnt, is_root;
		int err = 0;

		if (IS_ERR(ms))
			return PTR_ERR(ms);
		nd = ravenfs_msession_node(ms);
		if (rfs_ext_meta_ok(&nd->meta, &pcnt, &(u32){0}, &(u32){0}, &(u64){0}) ||
		    !RFS_PP_IS_INNER(nd->meta.parent_ptr) ||
		    idx >= pcnt || pcnt > RFS_EXT_ORDER ||
		    RFS_PTR_ADDR(nd->extents[idx].sub_node_ptr) != cur_victim) {
			ravenfs_mdata_put(ms);
			return -EIO;
		}
		nd->extents[idx].key = low_max;
		memmove(&nd->extents[idx + 2], &nd->extents[idx + 1],
			(size_t)(pcnt - idx - 1) * sizeof(struct extent_hint_t));
		rfs_ext_set_addr(&nd->extents[idx + 1].sub_node_ptr, new_blk);
		nd->extents[idx + 1].key = high_max;
		new_cnt = pcnt + 1;
		is_root = RFS_PP_IS_ROOT(nd->meta.parent_ptr);
		nd->meta.parent_ptr.v = RFS_PP_PACK(new_cnt, 1, is_root, RFS_PP_PTR(nd->meta.parent_ptr));
		NODE_DIRTY(ms);
		ravenfs_mdata_put(ms);

		if (new_cnt <= RFS_EXT_ORDER) {
			if (idx + 1 == new_cnt - 1)	/* 新 hint 是末位 → 父 max 变 */
				return rfs_ext_propagate(sb, p, level, high_max);
			return 0;
		}
		/* 父溢出 → 内裂 */
		{
			u64 rblk, lmx, hmx;

			err = rfs_ext_inner_split(sb, p->blk[level], &rblk, &lmx, &hmx);
			if (err)
				return err;
			if (is_root)
				return rfs_ext_new_root_inner(sb, ino, p->blk[level], lmx,
							      rblk, hmx, root_io);
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

/* ══ 插入一条 extent（walk 定位后调用）；含叶裂 + 升根 + 父 key 上溯 ══ */
int ravenfs_extent_insert(struct super_block *sb, u64 ino, u64 *root_io,
			  const struct extent_t *ext)
{
	struct rfs_epath p;
	u64 root = *root_io;
	u64 leaf_blk, old_max, new_max;
	u32 idx, cnt2;
	int err = 0;

	if (ext->length == 0)
		return 0;

	/* 空树 → 建根叶 */
	if (root == (u64)PTR_INVALID) {
		struct ravenfs_msession *rms;
		u64 rblk = ravenfs_block_alloc(sb, 2, 1);

		if (rblk == (u64)PTR_INVALID)
			return -ENOSPC;
		rms = ravenfs_mdata_get(sb, rblk);
		if (IS_ERR(rms)) {
			err = PTR_ERR(rms);
			ravenfs_block_free(sb, rblk, 2);
			return err;
		}
		rfs_ext_node_init(ravenfs_msession_node(rms), 0, 0, 1, ino);
		NODE_DIRTY(rms);
		ravenfs_mdata_put(rms);
		root = rblk;
		*root_io = root;
	}

	err = rfs_ext_walk(sb, root, ext->fblkbase, &p);
	if (err)
		return err;

	leaf_blk = p.blk[p.depth - 1];
	idx = p.ins_idx;
	{
		struct ravenfs_msession *ms = ravenfs_mdata_get(sb, leaf_blk);
		struct Bptree_extent_leaf_node_t *lf;
		u32 c, inner, rootf;
		u64 pptr;

		if (IS_ERR(ms))
			return PTR_ERR(ms);
		lf = ravenfs_msession_node(ms);
		if (rfs_ext_meta_ok(&lf->meta, &c, &inner, &rootf, &pptr) || inner) {
			ravenfs_mdata_put(ms);
			return -EIO;
		}
		if (p.hit) {			/* 起点已被覆盖（不应发生：仅洞插入） */
			ravenfs_mdata_put(ms);
			return -EEXIST;
		}
		if (idx > c)
			idx = c;
		/* 右邻重叠防御（半开：紧贴合法） */
		if (idx < c && ext->fblkbase + ext->length > lf->extents[idx].fblkbase) {
			ravenfs_mdata_put(ms);
			return -EEXIST;
		}
		/* 左邻重叠防御 */
		if (idx > 0 &&
		    lf->extents[idx - 1].fblkbase + lf->extents[idx - 1].length > ext->fblkbase) {
			ravenfs_mdata_put(ms);
			return -EEXIST;
		}
		old_max = (c > 0) ? (lf->extents[c - 1].fblkbase + lf->extents[c - 1].length) : 0;
		memmove(&lf->extents[idx + 1], &lf->extents[idx],
			(size_t)(c - idx) * sizeof(struct extent_t));
		lf->extents[idx] = *ext;
		cnt2 = c + 1;
		RFS_EXT_SET_COUNT(&lf->meta, cnt2);
		new_max = lf->extents[cnt2 - 1].fblkbase + lf->extents[cnt2 - 1].length;
		NODE_DIRTY(ms);
		ravenfs_mdata_put(ms);
	}

	if (cnt2 <= RFS_EXT_ORDER) {
		if (new_max != old_max)
			return rfs_ext_propagate(sb, &p, p.depth - 1, new_max);
		return 0;
	}
	/* 叶溢出 → 分裂 */
	{
		u64 nblk, low_max, high_max;
		bool was_root;

		err = rfs_ext_leaf_split(sb, leaf_blk, &nblk, &low_max, &high_max, &was_root);
		if (err)
			return err;
		if (p.depth == 1) {		/* 叶即根 → 升根 */
			return rfs_ext_new_root_inner(sb, ino, leaf_blk, low_max, nblk, high_max, root_io);
		}
		return rfs_ext_apply_up(sb, ino, &p, p.depth - 2, p.cidx[p.depth - 2],
					leaf_blk, low_max, nblk, high_max, root_io);
	}
}

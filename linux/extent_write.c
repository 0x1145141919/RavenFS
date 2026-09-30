// SPDX-License-Identifier: GPL-2.0
/*
 * extent_write.c —— RavenFS ko：文件数据**区段树（extent B+tree）写机械**（M4.2 第一版）
 *
 * 依据：Docs/linux/ravenfs-linux-ko-M4-data-spec.md §1.2/§2.3/§3/§8（立刻分配 D-M4-6）；
 *       Docs/disk_layout_v1.1.md §5（区段树 entry/路由/分裂/平衡）+ §10 不变量 8/9/10/31–36
 *       （阶 337 / 左 168 / 欠载 <168 / 合并 ≤337 / 深度 64 / 节点 8K 对齐）；
 *       研究报告 m4-research/C-god-extent.md（god 机械伪规格，理念参考非照搬）。
 * 范式：linux/dentry_tree.c + dentry_node.c（同一「树写机械」骨架：walk → 叶插 → 叶裂 → 升根 → 刷父 key）。
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
#include "ravenfs_extent.h"
#include "ravenfs_mdata_cache.h"
#include "ravenfs_bitmap.h"

#define RFS_EXT_ORDER		337u	/* 阶 */
#define RFS_EXT_MAX_SLOTS	338u	/* 槽 [0,337]；[337] 瞬时 */
#define RFS_EXT_SPLIT		168u	/* 分裂左半条数（左 168 / 右 count-168） */
#define RFS_EXT_DEPTH_MAX	64u	/* 深度上限（超限判成环） */
#define RFS_EXT_GLB_NONE	0xFFFFu	/* 叶内 GLB 无下界哨兵（u16） */
#define RFS_EXT_MIN		168u	/* 非根欠载阈值（count < 168 触发平衡） */

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

/* 叶内四形态融合（D-5a；对齐 god extent_map_range 融合段） */
enum rfs_ext_merge {
	RFS_EXT_MERGE_NONE,	/* 纯插 */
	RFS_EXT_MERGE_LEFT,	/* 左邻扩尾 */
	RFS_EXT_MERGE_RIGHT,	/* 并入右邻头 */
	RFS_EXT_MERGE_BOTH,	/* 左邻扩长吞右邻（count−1） */
};

/*
 * 分裂预算池（D-5b 预判法）：**修改叶之前**按树高一次性预分配整条分裂链节点
 * （叶裂 1 + 每内层至多 1 + 建新根 1 = depth+1，上界恒 depth+1）。级联过程中
 * 只从池取、不再 alloc → 修改一旦开始绝无「中途才缺节点」的 ENOSPC 半态。
 * 分配失败 = 树零改动返回（会话未 dirty）。整池容量 = 深度上限 + 1。
 */
#define RFS_EXT_RESV_MAX	(RFS_EXT_DEPTH_MAX + 1)
struct rfs_ext_resv {
	u64	blk[RFS_EXT_RESV_MAX];
	u32	n;		/* 已分配节点数 */
	u32	used;		/* 已被级联取用数 */
};

/* 预算池：前置分配 n 个节点块；失败即回滚已分配 → -ENOSPC（树零改动） */
static int rfs_ext_resv_alloc(struct super_block *sb, struct rfs_ext_resv *r, u32 n)
{
	u32 i;

	r->n = 0;
	r->used = 0;
	if (n > RFS_EXT_RESV_MAX)
		return -EIO;
	for (i = 0; i < n; i++) {
		u64 b = ravenfs_block_alloc(sb, 2, 1);

		if (b == (u64)PTR_INVALID) {
			while (r->n)
				ravenfs_block_free(sb, r->blk[--r->n], 2);
			return -ENOSPC;
		}
		r->blk[r->n++] = b;
	}
	return 0;
}

/* 预算池：归还未取用部分（正常收尾与错误路径统一收尾） */
static void rfs_ext_resv_release(struct super_block *sb, struct rfs_ext_resv *r)
{
	while (r->used < r->n)
		ravenfs_block_free(sb, r->blk[r->used++], 2);
	r->n = 0;
	r->used = 0;
}

/* 预算池：取一个节点块（池空/无池则现分配）；失败返回 PTR_INVALID */
static u64 rfs_ext_resv_take(struct super_block *sb, struct rfs_ext_resv *r)
{
	if (r && r->used < r->n)
		return r->blk[r->used++];
	return ravenfs_block_alloc(sb, 2, 1);
}

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

	RFS_STAT_INC(sb, ext_lookup);	/* extent 查找次数 */
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
		RFS_STAT_INC(sb, ext_node_reads);	/* 走树步数 */
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
		RFS_STAT_INC(sb, ext_node_reads);	/* 走树步数 */
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

/* 叶裂：victim(count=338) → 左 168 / 右 170；维护叶链；返回 low/high max + 新叶块号
 *
 * rsv：分裂预算池（D-5b）。非 NULL 时新叶块从池取（池空则现分配）；NULL = 旧口径
 *      （现分配，unmap 路径沿用）。*/
static int rfs_ext_leaf_split(struct super_block *sb, u64 victim_blk, u64 *out_new,
			      u64 *out_low, u64 *out_high, bool *out_was_root,
			      struct rfs_ext_resv *rsv)
{
	struct ravenfs_msession *vms = ravenfs_mdata_get(sb, victim_blk);
	struct ravenfs_msession *nms = NULL;
	struct Bptree_extent_leaf_node_t *vl, *nl;
	u32 cnt, inner, root;
	u64 pptr, parent, old_next, new_blk = (u64)PTR_INVALID;
	const u32 right = RFS_EXT_MAX_SLOTS - RFS_EXT_SPLIT;	/* 338-168 = 170 */
	bool was_root;
	int err;

	RFS_STAT_INC(sb, ext_split);	/* 分裂频率 → 树是否在抖 */
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

	new_blk = rfs_ext_resv_take(sb, rsv);
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

/* 内裂：victim(count=338) → 左 168 / 右 170；右半孩子 reparent → 新内块
 *
 * rsv：分裂预算池（D-5b，语义同 rfs_ext_leaf_split）。*/
static int rfs_ext_inner_split(struct super_block *sb, u64 victim_blk, u64 *out_new,
			       u64 *out_low, u64 *out_high, struct rfs_ext_resv *rsv)
{
	struct ravenfs_msession *vms = ravenfs_mdata_get(sb, victim_blk);
	struct ravenfs_msession *nms = NULL;
	struct Bptree_extent_inner_node_t *vi, *ni;
	u32 cnt, inner, root, i;
	u64 pptr, parent, new_blk = (u64)PTR_INVALID;
	const u32 right = RFS_EXT_MAX_SLOTS - RFS_EXT_SPLIT;
	int err;

	RFS_STAT_INC(sb, ext_split);
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

	new_blk = rfs_ext_resv_take(sb, rsv);
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

/* 新建根内（根升长）；显式写 ptrs[]（文件树 ".." = PTR_INVALID）
 *
 * rsv：分裂预算池（D-5b）；非 NULL 时新根块从池取。*/
static int rfs_ext_new_root_inner(struct super_block *sb, u64 ino, u64 left_blk, u64 left_max,
				  u64 right_blk, u64 right_max, u64 *root_out,
				  struct rfs_ext_resv *rsv)
{
	struct ravenfs_msession *rms;
	struct Bptree_extent_inner_node_t *ri;
	u64 root_blk, kids[2];
	int k, err = 0;

	RFS_STAT_INC(sb, ext_new_root);	/* 树长高的次数 */
	root_blk = rfs_ext_resv_take(sb, rsv);
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
			    u64 new_blk, u64 high_max, u64 *root_io, struct rfs_ext_resv *rsv)
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

			err = rfs_ext_inner_split(sb, p->blk[level], &rblk, &lmx, &hmx, rsv);
			if (err)
				return err;
			if (is_root)
				return rfs_ext_new_root_inner(sb, ino, p->blk[level], lmx,
							      rblk, hmx, root_io, rsv);
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

/* 欠载链修复（M4.3 定义于本文件后段；D-5a 双融合吞右邻致欠载时前向调用） */
static int rfs_ext_balance_from(struct super_block *sb, u64 ino, u64 child_blk,
				u64 *root_io, u64 *old_root);

/* ══ 插入一条 extent（walk 定位后调用）；叶内四形态融合 + 前瞻预算 + 叶裂 + 升根 + 父 key 上溯 ══
 *
 * D-5a：重叠防御之后按 merge = L ? (R ? both : left) : (R ? right : none) 做**叶内**
 *       四形态融合（对齐 god extent_map_range 融合段；god :105「跨界一律不做」）。
 * D-5b：**预判法（look-ahead）**——在修改叶之前先判定「本步是否需要新节点」（纯插致叶满
 *       → 叶裂；级联 → 内层裂；触顶 → 建新根），需要则**先**按树高预分配整条分裂链
 *       节点（预算池，同 god reserve_pool）。分配失败 = 树零改动返回 -ENOSPC，绝不会
 *       出现「叶已超满却裂不出来」的半态。left/right/both 恒零预算（count 不增或反减）。
 * P10：根更迭（both 致欠载 → 平衡链 → 根收缩）**旧根不就地 free**，经 *old_root 外抛，
 *       由调用方在 inode.data_root 提交**之后**统一 free。
 */
int ravenfs_extent_insert(struct super_block *sb, u64 ino, u64 *root_io,
			  const struct extent_t *ext, u64 *old_root)
{
	struct rfs_epath p;
	u64 root = *root_io;
	u64 leaf_blk, old_max, new_max = 0;
	u32 idx, cnt2 = 0;
	struct rfs_ext_resv rsv;
	enum rfs_ext_merge merge = RFS_EXT_MERGE_NONE;
	bool fused_both = false, leaf_is_root = false;
	int err = 0;

	RFS_STAT_INC(sb, ext_insert);	/* 映射插入次数 */
	if (old_root)
		*old_root = (u64)PTR_INVALID;
	rsv.n = 0;
	rsv.used = 0;
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
		bool L = false, R = false;

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

		/* ── D-5a 形态判定（树改动前一次完成；packed 成员禁引用 → 局部拷贝）──
		 * L/R 仅**叶内**判定（左邻 = GLB = ext[idx-1]；右邻 = ext[idx]）：
		 *   L = 左邻逻辑尾 == ext 起 ∧ 左邻物理尾 == ext 物理起
		 *   R = ext 逻辑尾 == 右邻起 ∧ ext 物理尾 == 右邻物理起
		 * 真重叠已被上列防御拦下（-EEXIST），== 判定与防御互斥 → 融合不误吞。*/
		if (idx > 0) {
			const struct extent_t *g = &lf->extents[idx - 1];

			L = (g->fblkbase + g->length == ext->fblkbase &&
			     RFS_PTR_ADDR(g->block_base) + g->length ==
			     RFS_PTR_ADDR(ext->block_base));
		}
		if (idx < c) {
			const struct extent_t *r = &lf->extents[idx];

			R = (ext->fblkbase + ext->length == r->fblkbase &&
			     RFS_PTR_ADDR(ext->block_base) + ext->length ==
			     RFS_PTR_ADDR(r->block_base));
		}
		merge = L ? (R ? RFS_EXT_MERGE_BOTH : RFS_EXT_MERGE_LEFT)
			  : (R ? RFS_EXT_MERGE_RIGHT : RFS_EXT_MERGE_NONE);

		/* ── D-5b 预判法：先判「本步是否需要新节点」，需要则**先备好** ── */
		if (merge == RFS_EXT_MERGE_NONE && c + 1 > RFS_EXT_ORDER) {
			/* 纯插致叶满（c==337）→ 必裂：按树高预分配整条分裂链
			 * （depth+1 = 叶裂 1 + 每内层至多 1 + 建新根 1）。alloc 在 memmove
			 * 之前 → 失败 = 回滚已分配 + 返回，会话未 dirty → 树零改动、真原子失败。*/
			err = rfs_ext_resv_alloc(sb, &rsv, p.depth + 1);
			if (err) {
				ravenfs_mdata_put(ms);
				return err;	/* -ENOSPC：未动树 */
			}
		}

		/* ── 执行形态（此刻起预算池已备好 → 无中途缺节点）── */
		switch (merge) {
		case RFS_EXT_MERGE_BOTH: {
			/* 左邻扩长吞右邻（右邻删除、count−1）；L&&R 已证 phys 三连连续
			 * → 合并单条连续映射合法。叶 max 恒不变（吞并终点 = 原右邻
			 * 终点）→ 零刷新。*/
			u64 gfb = lf->extents[idx - 1].fblkbase;
			struct extent_t r = lf->extents[idx];

			lf->extents[idx - 1].length = (r.fblkbase + r.length) - gfb;
			if (idx < c - 1)
				memmove(&lf->extents[idx], &lf->extents[idx + 1],
					(size_t)(c - idx - 1) * sizeof(struct extent_t));
			memset(&lf->extents[c - 1], 0, sizeof(struct extent_t));
			cnt2 = c - 1;
			RFS_EXT_SET_COUNT(&lf->meta, cnt2);
			fused_both = true;
			break;
		}
		case RFS_EXT_MERGE_LEFT:
			/* 左邻扩尾（count/槽位不动）*/
			lf->extents[idx - 1].length += ext->length;
			cnt2 = c;
			break;
		case RFS_EXT_MERGE_RIGHT: {
			/* 并入右邻头：起点/phys 同步左移 ext->length（R 条件保证
			 * r.fblkbase == ext.fblkbase + ext->length → 无下溢）；终点不动
			 * → 叶 max 不变。count 不变 → 零分配零失败面。*/
			struct extent_t nr = lf->extents[idx];

			nr.fblkbase -= ext->length;
			rfs_ext_set_addr(&nr.block_base,
					 RFS_PTR_ADDR(nr.block_base) - ext->length);
			nr.length += ext->length;
			lf->extents[idx] = nr;
			cnt2 = c;
			break;
		}
		default:	/* RFS_EXT_MERGE_NONE：纯插（GLB 后腾槽） */
			if (idx < c)
				memmove(&lf->extents[idx + 1], &lf->extents[idx],
					(size_t)(c - idx) * sizeof(struct extent_t));
			lf->extents[idx] = *ext;
			cnt2 = c + 1;
			RFS_EXT_SET_COUNT(&lf->meta, cnt2);
			break;
		}
		new_max = (cnt2 > 0) ? (lf->extents[cnt2 - 1].fblkbase +
					lf->extents[cnt2 - 1].length) : 0;
		leaf_is_root = (rootf != 0);
		NODE_DIRTY(ms);
		ravenfs_mdata_put(ms);
	}

	if (fused_both) {
		/* 双融合收尾：count 只减不增 → 永不超满（零分裂面）；叶 max 恒不变 →
		 * 零刷新。唯一结构路径 = 删记录致非根欠载 → 平衡链（复用 ko 既有
		 * rfs_ext_balance_from；根收缩旧根经 old_root 外抛，调用方提交后 free
		 * —— P10）。*/
		if (leaf_is_root || cnt2 >= RFS_EXT_MIN)
			return 0;
		return rfs_ext_balance_from(sb, ino, leaf_blk, root_io, old_root);
	}

	if (cnt2 <= RFS_EXT_ORDER) {
		if (new_max != old_max)
			return rfs_ext_propagate(sb, &p, p.depth - 1, new_max);
		return 0;
	}
	/* 叶溢出 → 分裂（D-5b：预算池已前置备好 → 级联零分配失败面） */
	{
		u64 nblk, low_max, high_max;
		bool was_root;

		err = rfs_ext_leaf_split(sb, leaf_blk, &nblk, &low_max, &high_max,
					 &was_root, &rsv);
		if (!err) {
			if (p.depth == 1) {	/* 叶即根 → 升根 */
				err = rfs_ext_new_root_inner(sb, ino, leaf_blk, low_max,
							     nblk, high_max, root_io, &rsv);
			} else {
				err = rfs_ext_apply_up(sb, ino, &p, p.depth - 2,
						       p.cidx[p.depth - 2], leaf_blk,
						       low_max, nblk, high_max, root_io, &rsv);
			}
		}
		rfs_ext_resv_release(sb, &rsv);	/* 归还未消耗预算（含错误路径） */
		return err;
	}
}

/* ══════════════════════════════════════════════════════════════════
 * M4.3：区段树**删除 / 欠载平衡 / 根收缩**（旧根延迟 free）
 *
 * 依据：Docs/punch_truncate_pread_spec.md §5/§6/§7（**P10✓ 旧根延迟 free**）；
 *       Docs/disk_layout_v1.1.md §5.3 + §10 不变量 31–36（阶 337 / 左 168 / 欠载 <168 /
 *       合并 ≤337 / 深度 64）；m4-research/C-god-extent.md §2.7/§2.8（god 机械伪规格）。
 * 范式：linux/dentry_node.c（node_balance / root_shrink / 上溯；同机械，阈值换 337/168）。
 * 纪律（P10）：`extent_root_shrink` 根更迭时**旧根不 free**，随 `old_root` 上抛；
 *       调用方在 inode.data_root 提交**之后**统一 free（严禁先 free 后翻 inode = 悬挂）。
 * ══════════════════════════════════════════════════════════════════ */

/* 空根叶：direct→extent 物化 / 整文件打洞的空树终态（size>160 时合法） */
int ravenfs_extent_new_root_leaf(struct super_block *sb, u64 ino, u64 *root_out)
{
	struct ravenfs_msession *ms;
	u64 blk;
	int err;

	RFS_STAT_INC(sb, ext_new_root);
	if (*root_out != (u64)PTR_INVALID)
		return 0;			/* 已有树 */
	blk = ravenfs_block_alloc(sb, 2, 1);
	if (blk == (u64)PTR_INVALID)
		return -ENOSPC;
	ms = ravenfs_mdata_get(sb, blk);
	if (IS_ERR(ms)) {
		err = PTR_ERR(ms);
		ravenfs_block_free(sb, blk, 2);
		return err;
	}
	rfs_ext_node_init(ravenfs_msession_node(ms), 0, 0, 1, ino);
	NODE_DIRTY(ms);
	ravenfs_mdata_put(ms);
	*root_out = blk;
	return 0;
}

/* 节点 max 变更后沿父链上溯刷新 hint key（短路：key 未变即停） */
static int rfs_ext_refresh_chain(struct super_block *sb, u64 blk)
{
	u64 cur = blk;
	u32 hop;

	for (hop = 0; hop < RFS_EXT_DEPTH_MAX; hop++) {
		struct ravenfs_msession *ms = ravenfs_mdata_get(sb, cur);
		struct Bptree_meta_t *m;
		struct ravenfs_msession *ps;
		struct Bptree_extent_inner_node_t *pn;
		u32 cnt, inner, root, pcnt, pin, proot, i, found = ~0u;
		u64 pptr, newkey, parent;

		if (IS_ERR(ms))
			return PTR_ERR(ms);
		m = ravenfs_msession_node(ms);
		if (rfs_ext_meta_ok(m, &cnt, &inner, &root, &pptr)) {
			ravenfs_mdata_put(ms);
			return -EIO;
		}
		if (root) {			/* 根无父，链止 */
			ravenfs_mdata_put(ms);
			return 0;
		}
		newkey = rfs_ext_node_max(m, inner, cnt);
		parent = pptr;
		ravenfs_mdata_put(ms);

		ps = ravenfs_mdata_get(sb, parent);
		if (IS_ERR(ps))
			return PTR_ERR(ps);
		pn = ravenfs_msession_node(ps);
		if (rfs_ext_meta_ok(&pn->meta, &pcnt, &pin, &proot, &(u64){0}) || !pin) {
			ravenfs_mdata_put(ps);
			return -EIO;
		}
		for (i = 0; i < pcnt; i++) {
			if (RFS_PTR_ADDR(pn->extents[i].sub_node_ptr) == cur) {
				found = i;
				break;
			}
		}
		if (found == ~0u) {
			ravenfs_mdata_put(ps);
			return -EIO;
		}
		if (pn->extents[found].key == newkey) {	/* key 未变 → 链止 */
			ravenfs_mdata_put(ps);
			return 0;
		}
		pn->extents[found].key = newkey;
		NODE_DIRTY(ps);
		ravenfs_mdata_put(ps);
		cur = parent;
	}
	return -EIO;			/* 超深 = 成环 */
}

/* 内层节点并入后：把孩子指针 parent_ptr 统一改指 node_blk（含从邻居搬来的） */
static int rfs_ext_reparent(struct super_block *sb, struct ravenfs_msession *node_ms,
			    u64 node_blk, u32 cnt)
{
	struct Bptree_extent_inner_node_t *nd = ravenfs_msession_node(node_ms);
	u32 i;

	for (i = 0; i < cnt; i++) {
		u64 cblk = RFS_PTR_ADDR(nd->extents[i].sub_node_ptr);
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
		err = rfs_ext_meta_ok(cm, &cc, &ci, &cr, &cp);
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
 * 欠载平衡：parent_blk 的 idx 孩子欠载（count<168）→ 与邻居合并（total≤337）或抹匀。
 * 返回 0（*need=父欠载需上溯）/ 1（父为根单孩 → 交 root_shrink）/ <0 错误。
 * 邻居方向：idx==0 向右（idx+1）；idx>0 向左（idx-1）。child 恒存活并吸收邻居。
 */
static int rfs_ext_node_balance(struct super_block *sb, u64 parent_blk, u32 idx, bool *need)
{
	struct ravenfs_msession *pms, *cms = NULL, *nms = NULL;
	struct Bptree_extent_inner_node_t *pi;
	struct Bptree_meta_t *cm, *nm;
	u32 pcnt, pin, proot, cc = 0, ci, cr, nc = 0, ni, nr;
	u64 pptr, cp, np, child_blk, nblk, nidx, total;
	bool is_inner;
	int ret = 0;

	RFS_STAT_INC(sb, ext_balance);	/* 平衡动作次数 */
	*need = false;
	pms = ravenfs_mdata_get(sb, parent_blk);
	if (IS_ERR(pms))
		return PTR_ERR(pms);
	pi = ravenfs_msession_node(pms);
	ret = rfs_ext_meta_ok(&pi->meta, &pcnt, &pin, &proot, &pptr);
	if (ret || !pin || idx >= pcnt) {
		ret = ret ? ret : -EIO;
		goto out;
	}
	child_blk = RFS_PTR_ADDR(pi->extents[idx].sub_node_ptr);
	cms = ravenfs_mdata_get(sb, child_blk);
	if (IS_ERR(cms)) {
		ret = PTR_ERR(cms);
		cms = NULL;
		goto out;
	}
	cm = ravenfs_msession_node(cms);
	ret = rfs_ext_meta_ok(cm, &cc, &ci, &cr, &cp);
	if (ret)
		goto out;
	is_inner = ci != 0;
	if (cc >= RFS_EXT_MIN)		/* 已健康 */
		goto out;

	if (idx == 0) {
		if (pcnt == 1) {	/* 根单孩 → 根收缩信号 */
			ret = 1;
			goto out;
		}
		nidx = 1;
	} else {
		nidx = idx - 1;
	}
	nblk = RFS_PTR_ADDR(pi->extents[nidx].sub_node_ptr);
	nms = ravenfs_mdata_get(sb, nblk);
	if (IS_ERR(nms)) {
		ret = PTR_ERR(nms);
		nms = NULL;
		goto out;
	}
	nm = ravenfs_msession_node(nms);
	ret = rfs_ext_meta_ok(nm, &nc, &ni, &nr, &np);
	if (ret)
		goto out;
	if ((ni != 0) != is_inner) {	/* 邻居不同层 → 结构异常 */
		ret = -EIO;
		goto out;
	}

	total = (u64)cc + nc;

	if (total <= RFS_EXT_ORDER) {
		/* ── 合并：child 吸收邻居（保 child，free 邻居） ── */
		if (is_inner) {
			struct Bptree_extent_inner_node_t *cnd = ravenfs_msession_node(cms);
			struct Bptree_extent_inner_node_t *nnd = ravenfs_msession_node(nms);

			if (idx == 0) {
				memcpy(&cnd->extents[cc], nnd->extents,
				       nc * sizeof(struct extent_hint_t));
			} else {
				memmove(&cnd->extents[nc], &cnd->extents[0],
					cc * sizeof(struct extent_hint_t));
				memcpy(&cnd->extents[0], nnd->extents,
				       nc * sizeof(struct extent_hint_t));
			}
			RFS_EXT_SET_COUNT(&cnd->meta, total);
			NODE_DIRTY(cms);
			ret = rfs_ext_reparent(sb, cms, child_blk, (u32)total);
			if (ret)
				goto out;
		} else {
			struct Bptree_extent_leaf_node_t *cnd = ravenfs_msession_node(cms);
			struct Bptree_extent_leaf_node_t *nnd = ravenfs_msession_node(nms);

			if (idx == 0) {
				u64 nnext;

				memcpy(&cnd->extents[cc], nnd->extents,
				       nc * sizeof(struct extent_t));
				nnext = RFS_PTR_ADDR(nnd->meta.ptrs[2]);
				rfs_ext_set_addr(&cnd->meta.ptrs[2], nnext);
				if (nnext != (u64)RFS_MASK52) {
					struct ravenfs_msession *x = ravenfs_mdata_get(sb, nnext);

					if (IS_ERR(x)) { ret = PTR_ERR(x); goto out; }
					rfs_ext_set_addr(&((struct Bptree_meta_t *)ravenfs_msession_node(x))->ptrs[1], child_blk);
					NODE_DIRTY(x);
					ravenfs_mdata_put(x);
				}
			} else {
				u64 nprev;

				memmove(&cnd->extents[nc], &cnd->extents[0],
					cc * sizeof(struct extent_t));
				memcpy(&cnd->extents[0], nnd->extents,
				       nc * sizeof(struct extent_t));
				nprev = RFS_PTR_ADDR(nnd->meta.ptrs[1]);
				rfs_ext_set_addr(&cnd->meta.ptrs[1], nprev);
				if (nprev != (u64)RFS_MASK52) {
					struct ravenfs_msession *x = ravenfs_mdata_get(sb, nprev);

					if (IS_ERR(x)) { ret = PTR_ERR(x); goto out; }
					rfs_ext_set_addr(&((struct Bptree_meta_t *)ravenfs_msession_node(x))->ptrs[2], child_blk);
					NODE_DIRTY(x);
					ravenfs_mdata_put(x);
				}
			}
			RFS_EXT_SET_COUNT(&cnd->meta, total);
			memset(&cnd->extents[total], 0,
			       (RFS_EXT_MAX_SLOTS - total) * sizeof(struct extent_t));
			NODE_DIRTY(cms);
		}
		/* 邻居内容已搬入 child → 丢弃其缓存行 + 释放块 */
		ravenfs_mdata_put(nms);
		nms = NULL;
		ravenfs_mdata_drop(sb, nblk);
		ravenfs_block_free(sb, nblk, 2);

		/* 父删邻居 hint + 刷保留 hint 的 key */
		{
			u32 del_idx = (idx == 0) ? 1 : (idx - 1);
			u32 kept = (idx == 0) ? 0 : (idx - 1);

			if (del_idx < pcnt - 1)
				memmove(&pi->extents[del_idx], &pi->extents[del_idx + 1],
					(size_t)(pcnt - del_idx - 1) * sizeof(struct extent_hint_t));
			memset(&pi->extents[pcnt - 1], 0, sizeof(struct extent_hint_t));
			pcnt--;
			RFS_EXT_SET_COUNT(&pi->meta, pcnt);
			pi->extents[kept].key =
				rfs_ext_node_max(ravenfs_msession_node(cms), is_inner, (u32)total);
			NODE_DIRTY(pms);
		}
		if (!proot && pcnt < RFS_EXT_MIN)
			*need = true;
		goto out;
	}

	/* ── 抹匀（再分配）：total > 337，两节点都在 ── */
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
			struct Bptree_extent_inner_node_t *l = ravenfs_msession_node(lms);
			struct Bptree_extent_inner_node_t *r = ravenfs_msession_node(rms);
			u32 k;

			if (lc > left) {
				k = lc - left;
				memmove(&r->extents[k], &r->extents[0],
					rc * sizeof(struct extent_hint_t));
				memcpy(&r->extents[0], &l->extents[left],
				       k * sizeof(struct extent_hint_t));
			} else {
				k = left - lc;
				memcpy(&l->extents[lc], &r->extents[0],
				       k * sizeof(struct extent_hint_t));
				memmove(&r->extents[0], &r->extents[k],
					(rc - k) * sizeof(struct extent_hint_t));
			}
			RFS_EXT_SET_COUNT(&l->meta, left);
			RFS_EXT_SET_COUNT(&r->meta, right);
			memset(&l->extents[left], 0, (RFS_EXT_MAX_SLOTS - left) * sizeof(struct extent_hint_t));
			memset(&r->extents[right], 0, (RFS_EXT_MAX_SLOTS - right) * sizeof(struct extent_hint_t));
			NODE_DIRTY(lms);
			NODE_DIRTY(rms);
			ret = rfs_ext_reparent(sb, lms, lblk, left);
			if (ret)
				goto out;
			ret = rfs_ext_reparent(sb, rms, rblk, right);
			if (ret)
				goto out;
		} else {
			struct Bptree_extent_leaf_node_t *l = ravenfs_msession_node(lms);
			struct Bptree_extent_leaf_node_t *r = ravenfs_msession_node(rms);
			u32 k;

			if (lc > left) {
				k = lc - left;
				memmove(&r->extents[k], &r->extents[0],
					rc * sizeof(struct extent_t));
				memcpy(&r->extents[0], &l->extents[left],
				       k * sizeof(struct extent_t));
			} else {
				k = left - lc;
				memcpy(&l->extents[lc], &r->extents[0],
				       k * sizeof(struct extent_t));
				memmove(&r->extents[0], &r->extents[k],
					(rc - k) * sizeof(struct extent_t));
			}
			RFS_EXT_SET_COUNT(&l->meta, left);
			RFS_EXT_SET_COUNT(&r->meta, right);
			memset(&l->extents[left], 0, (RFS_EXT_MAX_SLOTS - left) * sizeof(struct extent_t));
			memset(&r->extents[right], 0, (RFS_EXT_MAX_SLOTS - right) * sizeof(struct extent_t));
			NODE_DIRTY(lms);
			NODE_DIRTY(rms);
		}
		pi->extents[idx].key =
			rfs_ext_node_max(ravenfs_msession_node(cms), is_inner, child_left ? left : right);
		pi->extents[nidx].key =
			rfs_ext_node_max(ravenfs_msession_node(nms), is_inner, child_left ? right : left);
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

/*
 * 根收缩：根（内）只剩 1 孩 → 提升该孩为新根。
 * **旧根不 free**，经 *old_root 上抛（P10 延迟 free；调用方在 inode 提交后 free）。
 */
static int rfs_ext_root_shrink(struct super_block *sb, u64 ino, u64 *root_io, u64 *old_root)
{
	struct ravenfs_msession *rms, *cms = NULL;
	struct Bptree_meta_t *rm, *cm;
	u64 root_blk = *root_io, child_blk;
	u32 rc, rin, rrt, cc, ci, crr;
	u64 rp, cp;
	int err;

	if (old_root)
		*old_root = (u64)PTR_INVALID;
	if (root_blk == (u64)PTR_INVALID)
		return 0;

	rms = ravenfs_mdata_get(sb, root_blk);
	if (IS_ERR(rms))
		return PTR_ERR(rms);
	rm = ravenfs_msession_node(rms);
	err = rfs_ext_meta_ok(rm, &rc, &rin, &rrt, &rp);
	if (err)
		goto out;
	if (!rin || rc != 1)		/* 根叶 / 根内非单孩 → 无需收缩 */
		goto out;

	child_blk = RFS_PTR_ADDR(((struct Bptree_extent_inner_node_t *)rm)->extents[0].sub_node_ptr);
	cms = ravenfs_mdata_get(sb, child_blk);
	if (IS_ERR(cms)) {
		err = PTR_ERR(cms);
		cms = NULL;
		goto out;
	}
	cm = ravenfs_msession_node(cms);
	err = rfs_ext_meta_ok(cm, &cc, &ci, &crr, &cp);
	if (err || crr) {
		err = err ? err : -EIO;
		goto out;
	}
	cm->parent_ptr.v = RFS_PP_PACK(cc, ci, 1, ino);
	/* 新根无兄弟链、无 ".."（文件树恒 PTR_INVALID） */
	cm->ptrs[0].v = RFS_EXT_PTR_INV;
	cm->ptrs[1].v = RFS_EXT_PTR_INV;
	cm->ptrs[2].v = RFS_EXT_PTR_INV;
	NODE_DIRTY(cms);

	*root_io = child_blk;
	if (old_root)
		*old_root = root_blk;	/* **延迟 free**：此处不释放 */
out:
	if (cms)
		ravenfs_mdata_put(cms);
	ravenfs_mdata_put(rms);
	return err;
}

/* 欠载链修复（自下而上，父指针驱动）；收尾根收缩 */
static int rfs_ext_balance_from(struct super_block *sb, u64 ino, u64 child_blk,
				u64 *root_io, u64 *old_root)
{
	u64 cur = child_blk;
	u32 hop;

	RFS_STAT_INC(sb, ext_balance);
	for (hop = 0; hop < RFS_EXT_DEPTH_MAX; hop++) {
		struct ravenfs_msession *ms, *ps;
		struct Bptree_meta_t *m;
		struct Bptree_extent_inner_node_t *pn;
		u32 cnt, inner, root, pcnt, pin, proot, i, idx = ~0u;
		u64 pptr, parent;
		bool need = false;
		int r;

		ms = ravenfs_mdata_get(sb, cur);
		if (IS_ERR(ms))
			return PTR_ERR(ms);
		m = ravenfs_msession_node(ms);
		if (rfs_ext_meta_ok(m, &cnt, &inner, &root, &pptr)) {
			ravenfs_mdata_put(ms);
			return -EIO;
		}
		if (root) {
			ravenfs_mdata_put(ms);
			if (inner && cnt == 1)
				return rfs_ext_root_shrink(sb, ino, root_io, old_root);
			return 0;
		}
		if (cnt >= RFS_EXT_MIN) {	/* 已健康，链止 */
			ravenfs_mdata_put(ms);
			return 0;
		}
		parent = pptr;
		ravenfs_mdata_put(ms);

		ps = ravenfs_mdata_get(sb, parent);
		if (IS_ERR(ps))
			return PTR_ERR(ps);
		pn = ravenfs_msession_node(ps);
		if (rfs_ext_meta_ok(&pn->meta, &pcnt, &pin, &proot, &(u64){0}) || !pin) {
			ravenfs_mdata_put(ps);
			return -EIO;
		}
		for (i = 0; i < pcnt; i++) {
			if (RFS_PTR_ADDR(pn->extents[i].sub_node_ptr) == cur) {
				idx = i;
				break;
			}
		}
		ravenfs_mdata_put(ps);
		if (idx == ~0u)
			return -EIO;

		r = rfs_ext_node_balance(sb, parent, idx, &need);
		if (r < 0)
			return r;
		if (r == 1)			/* 父为根单孩 */
			return rfs_ext_root_shrink(sb, ino, root_io, old_root);
		if (rfs_ext_refresh_chain(sb, parent))
			return -EIO;
		if (!need) {
			/* 合并可能使根内残留 count==1 → 收尾收缩 */
			struct ravenfs_msession *rs = ravenfs_mdata_get(sb, parent);

			if (!IS_ERR(rs)) {
				struct Bptree_meta_t *rmm = ravenfs_msession_node(rs);
				u32 rc, rin, rrt;

				if (!rfs_ext_meta_ok(rmm, &rc, &rin, &rrt, &(u64){0}) &&
				    rrt && rin && rc == 1) {
					ravenfs_mdata_put(rs);
					return rfs_ext_root_shrink(sb, ino, root_io, old_root);
				}
				ravenfs_mdata_put(rs);
			}
			return 0;
		}
		cur = parent;
	}
	return -EIO;
}

/*
 * 子集撤销（loose）：删除逻辑区间 ext（须**被单条记录完全覆盖**）。
 * 三分支：full 整删 / head 缩水 / tail 缩水 / mid 拆两端；欠载走平衡链；
 * 根更迭由 root_shrink 上抛旧根（*old_root，延迟 free）。
 * 返回 0 / -ENOENT（起点空洞）/ -EIO（越记录 / phys 不自洽）/ -ENOSPC。
 */
int ravenfs_extent_unmap_loose(struct super_block *sb, u64 ino, u64 *root_io,
			       const struct extent_t *ext, u64 *old_root)
{
	struct rfs_epath p;
	u64 root = *root_io, leaf_blk;
	struct ravenfs_msession *ms;
	struct Bptree_extent_leaf_node_t *lf;
	u32 cnt, inner, rootf, glb;
	u64 pptr, off, rec_len, rec_fb, rec_addr, cut;
	int err, kind;

	RFS_STAT_INC(sb, ext_unmap);	/* unmap 强度 */
	if (old_root)
		*old_root = (u64)PTR_INVALID;
	if (ext->length == 0)
		return 0;
	if (root == (u64)PTR_INVALID)
		return -ENOENT;

	err = rfs_ext_walk(sb, root, ext->fblkbase, &p);
	if (err)
		return err;
	if (!p.hit)
		return -ENOENT;			/* 起点空洞 */
	leaf_blk = p.blk[p.depth - 1];
	glb = p.cidx[p.depth - 1];

	ms = ravenfs_mdata_get(sb, leaf_blk);
	if (IS_ERR(ms))
		return PTR_ERR(ms);
	lf = ravenfs_msession_node(ms);
	err = rfs_ext_meta_ok(&lf->meta, &cnt, &inner, &rootf, &pptr);
	if (err || inner || glb >= cnt) {
		ravenfs_mdata_put(ms);
		return err ? err : -EIO;
	}

	rec_fb = lf->extents[glb].fblkbase;
	rec_len = lf->extents[glb].length;
	rec_addr = RFS_PTR_ADDR(lf->extents[glb].block_base);
	cut = ext->length;
	off = ext->fblkbase - rec_fb;
	if (cut > rec_len - off) {		/* 终点越出记录 */
		ravenfs_mdata_put(ms);
		return -EIO;
	}
	if (rec_addr + off != RFS_PTR_ADDR(ext->block_base)) {	/* phys 不自洽 */
		ravenfs_mdata_put(ms);
		return -EIO;
	}

	if (cut == rec_len)
		kind = 0;			/* full 整删 */
	else if (off == 0)
		kind = 1;			/* head 贴头挖 */
	else if (cut == rec_len - off)
		kind = 2;			/* tail 贴尾挖 */
	else
		kind = 3;			/* mid 拆两端 */

	switch (kind) {
	case 0:	/* full */
		if (glb < cnt - 1)
			memmove(&lf->extents[glb], &lf->extents[glb + 1],
				(size_t)(cnt - glb - 1) * sizeof(struct extent_t));
		memset(&lf->extents[cnt - 1], 0, sizeof(struct extent_t));
		cnt--;
		RFS_EXT_SET_COUNT(&lf->meta, cnt);
		NODE_DIRTY(ms);
		ravenfs_mdata_put(ms);
		if (rootf)
			return 0;		/* 根叶可空（空树合法） */
		if (cnt < RFS_EXT_MIN)
			return rfs_ext_balance_from(sb, ino, leaf_blk, root_io, old_root);
		return rfs_ext_refresh_chain(sb, leaf_blk);

	case 1:	/* head：贴头挖，终点不变 → 零刷新 */
		lf->extents[glb].fblkbase = rec_fb + cut;
		lf->extents[glb].length = rec_len - cut;
		rfs_ext_set_addr(&lf->extents[glb].block_base, rec_addr + cut);
		NODE_DIRTY(ms);
		ravenfs_mdata_put(ms);
		return 0;

	case 2:	/* tail：贴尾挖，保留头 */
		lf->extents[glb].length = off;
		NODE_DIRTY(ms);
		ravenfs_mdata_put(ms);
		if (!rootf && glb == cnt - 1)	/* 末项 → 叶 max 变 */
			return rfs_ext_refresh_chain(sb, leaf_blk);
		return 0;

	default: /* mid：拆两端（左留原槽缩尾 + 右段新插 glb+1） */
	{
		struct extent_t right;

		right.fblkbase = rec_fb + off + cut;
		right.length = rec_len - off - cut;
		right.block_base.v = RFS_PTR_PACK(0, rec_addr + off + cut);
		lf->extents[glb].length = off;
		memmove(&lf->extents[glb + 2], &lf->extents[glb + 1],
			(size_t)(cnt - glb - 1) * sizeof(struct extent_t));
		lf->extents[glb + 1] = right;
		cnt++;
		RFS_EXT_SET_COUNT(&lf->meta, cnt);
		NODE_DIRTY(ms);
		ravenfs_mdata_put(ms);

		if (cnt > RFS_EXT_ORDER) {	/* 叶溢出 → 分裂 */
			u64 nblk, low_max, high_max;
			bool was_root;

			/* unmap 路径不预判预算（rsv=NULL → 现分配，旧口径）*/
			err = rfs_ext_leaf_split(sb, leaf_blk, &nblk, &low_max,
						 &high_max, &was_root, NULL);
			if (err)
				return err;
			if (p.depth == 1)
				return rfs_ext_new_root_inner(sb, ino, leaf_blk, low_max,
							      nblk, high_max, root_io, NULL);
			return rfs_ext_apply_up(sb, ino, &p, p.depth - 2,
						p.cidx[p.depth - 2], leaf_blk, low_max,
						nblk, high_max, root_io, NULL);
		}
		/* 右段终点 = 原 rec 终点 → 叶 max 不变，零刷新 */
		return 0;
	}
	}
}

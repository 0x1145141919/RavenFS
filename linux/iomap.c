// SPDX-License-Identifier: GPL-2.0
/*
 * iomap.c —— RavenFS ko：M4.1 只读数据路径（逻辑偏移 → iomap 映射）
 *
 * 依据：Docs/linux/ravenfs-linux-ko-M4-data-spec.md
 *   §1.2（7.2.3 iomap 真值）、§2.3（唯一自写核心 iomap_begin/end）、
 *   §3（三态→iomap type，判据 size 优先 = 不变量 22）、§6（读粒度 = iomap 默认）。
 * 机械真值：Docs/disk_layout_v1.1.md §5（区段树 entry/路由/半开区间）+ §10 不变量 22/35；
 *           Docs/disk_layout_v1.2_inode.md §2（inode 位域）。
 *
 * 纪律（spec §2.3）：
 *   · 页缓存 / folio / bio / readahead **全由 iomap 核心代劳**；
 *     本文件只回答「逻辑偏移 pos → 物理映射 / 空洞 / 内联」。
 *   · M4.1 = **只读**：IOMAP_WRITE / IOMAP_REPORT 一律 -EOPNOTSUPP（写路径 M4.2）。
 *   · srcmap = iomap（无 COW）。
 *
 * 三态解码（不变量 22，**判据 size 优先**，data_root 取值不参与判定）：
 *   size ≤ 160            → IOMAP_INLINE（内容在 inode.inline_data，offset=0）
 *   size > 160 ∧ direct=1 → IOMAP_MAPPED（连续区 data_root + pos/4096）
 *   size > 160 ∧ direct=0 → extent 树 walk（命中 MAPPED / 未命中 HOLE）
 *
 * 【地址换算】盘面 data_root / block_base 均为 **FS_block 号**；iomap->addr 须是
 * **块设备字节偏移**（iomap 直接向 iomap->bdev 提交 bio，iomap_sector = (addr+…)>>9）。
 * 故一律经 ravenfs_dev_blk()（+blk0_off，恒 4096）换算到设备字节，见 ravenfs_dev_byte()。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "ravenfs.h"

/* inline 态字节上限（不变量 22）：size ≤ 160 ⇒ 内容在 inode.inline_data */
#define RAVENFS_INLINE_BYTES		160U
/* 区段树深度上限（不变量 35；超限判成环） */
#define RAVENFS_EXTENT_DEPTH_MAX	64U
#define RAVENFS_BLK_SHIFT		12			/* 4096 = 1 << 12 */
/* size → 覆盖块数 ceil(size/4096)（不变量 22） */
#define RAVENFS_NBLOCKS(b)		(((b) + RAVENFS_BLOCK_SIZE - 1) >> RAVENFS_BLK_SHIFT)

/* FS_block 号 → 块设备字节偏移（含 blk0_off，恒 = 4096） */
static inline u64 ravenfs_dev_byte(struct ravenfs_sb_info *sbi, u64 fs_blk)
{
	return (u64)ravenfs_dev_blk(sbi, fs_blk) << RAVENFS_BLK_SHIFT;
}

/*
 * 区段树 walk（上界路由，不变量 9）：
 *   内部：target < key[i] → 子树 i（key = 子树最大**开边界** fblkbase+length）；
 *         无匹配（target ≥ 全部 key）→ 洞。
 *   叶子：相邻 extent 升序；`fblkbase ≤ target < fblkbase+length` 命中。
 *
 * 返回：0 = 命中（*out=extent）；1 = 未命中（洞，*next=下一 extent 起点块号，0=无）；
 *       <0 = 错误。
 */
static int ravenfs_extent_lookup(struct super_block *sb, u64 root, u64 target,
				 struct extent_t *out, u64 *next)
{
	u64 blk = root;
	u32 depth = 0;
	void *buf;
	int err = -EIO;

	*next = 0;
	if (root == (u64)PTR_INVALID)
		return 1;			/* 无树 → 全洞（size>160 但 direct=0 的空树） */

	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (;;) {
		const struct Bptree_meta_t *meta;
		u32 cnt, inner, i;

		if (++depth > RAVENFS_EXTENT_DEPTH_MAX || blk == (u64)PTR_INVALID) {
			err = -EIO;		/* 深度超限（成环）/ 悬挂指针 */
			break;
		}
		err = ravenfs_read_node(sb, blk, buf);
		if (err)
			break;
		meta = buf;
		if (meta->magics[0] != BPTREE_MAGIC1 || meta->magics[1] != BPTREE_MAGIC2 ||
		    meta->magics[2] != BPTREE_MAGIC3 || meta->magics[3] != BPTREE_MAGIC4) {
			err = -EIO;
			break;
		}
		cnt = RFS_PP_COUNT(meta->parent_ptr);
		inner = RFS_PP_IS_INNER(meta->parent_ptr);
		if (cnt > 338) {
			err = -EIO;
			break;
		}

		if (inner) {
			const struct Bptree_extent_inner_node_t *nd = buf;
			u32 sel = cnt;		/* cnt = 无匹配 */

			for (i = 0; i < cnt; i++) {
				if (target < nd->extents[i].key) {
					sel = i;
					break;
				}
			}
			if (sel == cnt) {	/* target ≥ 全部 key → 洞 */
				err = 1;
				*next = 0;
				break;
			}
			blk = RFS_PTR_ADDR(nd->extents[sel].sub_node_ptr);
			continue;
		}

		/* 叶子：线性扫描（≤337 项，半开区间，升序无重叠） */
		{
			const struct Bptree_extent_leaf_node_t *lf = buf;

			err = 1;		/* 默认未命中 */
			for (i = 0; i < cnt; i++) {
				const struct extent_t *e = &lf->extents[i];
				u64 b = e->fblkbase;

				if (target < b) {	/* 越过终点：b = 下一 extent 起点 */
					*next = b;
					break;
				}
				if (target < b + e->length) {	/* 命中 [b, b+len) */
					*out = *e;
					err = 0;
					break;
				}
			}
			break;
		}
	}

	kfree(buf);
	return err;
}

static int ravenfs_iomap_begin(struct inode *inode, loff_t pos, loff_t length,
			       unsigned int flags, struct iomap *iomap,
			       struct iomap *srcmap)
{
	struct super_block *sb = inode->i_sb;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;
	const struct raven_Inode_t *r;
	u64 size, off, blk, end_blk;
	int err;

	if (flags & IOMAP_WRITE)
		return -EOPNOTSUPP;		/* M4.1 只读：写路径 M4.2 */
	if (flags & IOMAP_REPORT)
		return -EOPNOTSUPP;		/* M4.1 不做 FIEMAP */

	memset(iomap, 0, sizeof(*iomap));
	iomap->bdev = sb->s_bdev;

	ses = ravenfs_session_get(sb, inode->i_ino);
	if (IS_ERR(ses))
		return PTR_ERR(ses);
	r = ravenfs_session_inode(ses);

	size = r->size;				/* 判据真值（不变量 22） */
	iomap->offset = ALIGN_DOWN(pos, RAVENFS_BLOCK_SIZE);
	iomap->addr = IOMAP_NULL_ADDR;
	iomap->type = IOMAP_HOLE;
	iomap->length = length;
	iomap->flags = 0;

	/* —— 越过 EOF：洞，零填至请求长度（对齐 zonefs/xfs 读越界口径） —— */
	if ((u64)pos >= size) {
		iomap->offset = pos;		/* 精确覆盖 pos（gfs2 同法） */
		goto out;
	}

	/* —— inline：size ≤ 160，整段在 inode.inline_data，恒在第一块 —— */
	if (size <= RAVENFS_INLINE_BYTES) {
		struct ravenfs_inode_info *ri = RAVENFS_I(inode);

		/*
		 * iomap->inline_data 须在 iomap_begin 返回后仍有效（iomap 核心稍后
		 * 在 iomap_read_inline_data 里读取）→ 不能指向会话缓存行（随会话释放）。
		 * 镜像一份到 inode 私有区（只读幂等拷贝）。
		 */
		memcpy(ri->inline_data, r->inline_data, sizeof(ri->inline_data));
		iomap->offset = 0;
		iomap->type = IOMAP_INLINE;
		iomap->addr = 0;
		iomap->inline_data = ri->inline_data;
		iomap->length = size;		/* 须 ≥ (i_size - offset) */
		goto out;
	}

	off  = (u64)ALIGN_DOWN(pos, RAVENFS_BLOCK_SIZE);
	blk  = off >> RAVENFS_BLK_SHIFT;		/* 目标逻辑块号 */
	end_blk = RAVENFS_NBLOCKS(size);		/* ceil(size/4096) */

	/* —— direct：连续区，逻辑块 → data_root + 块号 —— */
	if (RFS_INO1_GET_DIRECT(r->word1)) {
		iomap->type = IOMAP_MAPPED;
		iomap->addr = ravenfs_dev_byte(sbi, r->data_root + blk);
		iomap->length = (end_blk - blk) << RAVENFS_BLK_SHIFT;	/* 截到 run 尾 */
		goto out;
	}

	/* —— extent 树 —— */
	{
		struct extent_t ext;
		u64 next;

		err = ravenfs_extent_lookup(sb, r->data_root, blk, &ext, &next);
		if (err < 0)
			return err;
		if (err == 0) {			/* 命中：映射到 extent 尾 */
			u64 ext_phys = RFS_PTR_ADDR(ext.block_base) + (blk - ext.fblkbase);
			u64 run_end = ext.fblkbase + ext.length;

			iomap->type = IOMAP_MAPPED;
			iomap->addr = ravenfs_dev_byte(sbi, ext_phys);
			iomap->length = (run_end - blk) << RAVENFS_BLK_SHIFT;
		} else {			/* 洞：截到下一 extent 起点（或 EOF 块） */
			u64 hole_end = next ? min(next, end_blk) : end_blk;

			if (hole_end <= blk)
				hole_end = blk + 1;	/* 护栏：长度必 > 0 */
			iomap->type = IOMAP_HOLE;
			iomap->addr = IOMAP_NULL_ADDR;
			iomap->length = (hole_end - blk) << RAVENFS_BLK_SHIFT;
		}
	}

out:
	*srcmap = *iomap;			/* 无 COW：srcmap = iomap */
	return 0;
}

static int ravenfs_iomap_end(struct inode *inode, loff_t pos, loff_t length,
			     ssize_t written, unsigned int flags,
			     struct iomap *iomap)
{
	/* M4.1 只读：无分配需提交/回滚。写路径的短写尾清算见 M4.2（D-M4-10）。 */
	return 0;
}

const struct iomap_ops ravenfs_iomap_ops = {
	.iomap_begin	= ravenfs_iomap_begin,
	.iomap_end	= ravenfs_iomap_end,
};

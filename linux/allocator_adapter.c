// SPDX-License-Identifier: GPL-2.0
/*
 * allocator_adapter.c —— 运行时树分配器 struct rfs_allocator 的 FS 侧生命周期接线
 *                        （v2.0：block 位图镜像 + 全局 inode 数据面装载/落盘）
 *
 * 契约语义见 ravenfs_allocator_adapter.h。本文件是「树 ↔ 盘面」唯一缝合面：
 *   ① create ：block 位图镜像读入 words（vzalloc，4K 对齐）+ 从 SB 装载 inode
 *              数据面（direct 段 / 映射 B+tree bulk-load + 扁 fr 快照）→ rfs_allocator_init。
 *   ② flush  ：v2.0 硬顺序（⇄ god all_sycn）：
 *               (1) inode 数据面维护：extent 单连续段 → direct；否则整棵映射 B+tree
 *                   bulk build（RFS_EXT_KIND_INODE）；fr → 释放旧扁区 + hard_alloc 新区
 *                   整段重写；刷新 SB 四字段（sbi 内存）。
 *               (2) mdata 脏节点刷盘（新树节点 / inode 片内容）。
 *               (3) block 位图补集重建（全占用 → 逐片 dump 空闲位清零）→ 写回设备。
 *               (4) SB 最后写（crc + latest_syn_time；见 super.c ravenfs_sb_write_sync）。
 *               因 (1) 取块会改 block 分配器，(3) 在其后重建才能捕获。
 *   ③ destroy：rfs_allocator_fini（含 inode 数据面拆树）+ 释放 words；幂等。
 *
 * 无回退（硬切）：inode 组 = 全局数据面；旧 inode 位图镜像整体切除。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/buffer_head.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/crc32.h>

#include "ravenfs.h"
#include "ravenfs_allocator_adapter.h"
#include "ravenfs_allocator_top.h"
#include "ravenfs_run_list.h"
#include "ravenfs_mdata_cache.h"
#include "ravenfs_extent.h"

/* ═══════════════════════ 注入分配回调（GFP_NOFS）═══════════════════════ */

static void *ko_alloc(void *ctx, u64 size)
{
	(void)ctx;
	return kzalloc((size_t)size, GFP_NOFS);
}

static void ko_free(void *ctx, void *p)
{
	(void)ctx;
	kfree(p);
}

static const struct rfs_mem_alloc ko_mem_alloc = { ko_alloc, ko_free, NULL };
static const struct rfs_run_alloc ko_run_alloc = { ko_alloc, ko_free, NULL };

/* ═══════════════════════ al_ 计数器钩子（树分配器 → sbi->al）══════════════════════ */
static void ko_al_stat_inc(void *ctx, u32 field, u64 delta)
{
	struct super_block *sb = (struct super_block *)ctx;
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	atomic64_t *st;

	if (!sbi || field >= AL_STAT_COUNT)
		return;
	st = (atomic64_t *)&sbi->al;
	atomic64_add((long)delta, &st[field]);
}

/* ═══════════════════════ 管理器（对 sbi 不透明）═══════════════════════ */

struct ravenfs_alloc_mgr {
	struct rfs_allocator alloc;	/* 运行期真值（首成员；sbi->alloc 直指此处）*/
	struct super_block *sb;
	u8		*blk_words;	/* block 位图镜像（checkpoint 底座 + 落盘载体）*/
	u64		 blk_nbits;
};

/* ═══════════════════════ 位区间读写（LSB-first）═══════════════════════ */

static void bits_set_range(u8 *w, u64 s, u64 e)
{
	u64 b;

	for (b = s; b < e && (b & 7); b++)
		w[b >> 3] |= (u8)(1u << (b & 7));
	for (; b + 8 <= e; b += 8)
		w[b >> 3] = 0xFF;
	for (; b < e; b++)
		w[b >> 3] |= (u8)(1u << (b & 7));
}

static void bits_clear_range(u8 *w, u64 s, u64 e)
{
	u64 b;

	for (b = s; b < e && (b & 7); b++)
		w[b >> 3] &= (u8)~(1u << (b & 7));
	for (; b + 8 <= e; b += 8)
		w[b >> 3] = 0x00;
	for (; b < e; b++)
		w[b >> 3] &= (u8)~(1u << (b & 7));
}

/* ═══════════════════════ block 位图镜像：设备 ↔ words ═══════════════════════ */

static int bm_image_load(struct super_block *sb, u8 *words, u64 nblocks, u64 disk_start)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 b;

	for (b = 0; b < nblocks; b++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, disk_start + b));

		if (!bh) {
			RFS_STAT_INC(sb, err_meta_io);
			pr_err("ravenfs: bitmap image load fail at FS_blk %llu\n",
			       (unsigned long long)(disk_start + b));
			return -EIO;
		}
		memcpy(words + (b << 12), bh->b_data, RAVENFS_BLOCK_SIZE);
		brelse(bh);
		RFS_STAT_INC(sb, io_read_ops);
		RFS_STAT_ADD(sb, io_read_bytes, RAVENFS_BLOCK_SIZE);
	}
	return 0;
}

static int bm_image_save(struct super_block *sb, const u8 *words, u64 nblocks, u64 disk_start)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 b;

	for (b = 0; b < nblocks; b++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, disk_start + b));

		if (!bh) {
			RFS_STAT_INC(sb, err_meta_io);
			pr_err("ravenfs: bitmap image save fail at FS_blk %llu\n",
			       (unsigned long long)(disk_start + b));
			return -EIO;
		}
		memcpy(bh->b_data, words + (b << 12), RAVENFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
		RFS_STAT_INC(sb, io_write_ops);
		RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
	}
	return 0;
}

/* block 组补集重建：全片置占用 → 逐片 dump 空闲区间清零。 */
static int checkpoint_block(struct ravenfs_alloc_mgr *m)
{
	u64 per = rfs_allocator_block_per_slice_bits(&m->alloc);
	u32 ns = rfs_allocator_slice_count(&m->alloc);
	u32 i;
	u64 nbits = m->blk_nbits;

	if (!m->blk_words || !per || !ns)
		return -EINVAL;
	for (i = 0; i < ns; i++) {
		u64 base = (u64)i * per;
		u64 end = base + per;
		struct rfs_run_dlist dl;
		struct rfs_run_node *n;
		int rc;

		if (base >= nbits)
			break;
		if (end > nbits)
			end = nbits;
		bits_set_range(m->blk_words, base, end);	/* 先全置占用 */

		rfs_run_dlist_init(&dl, ko_run_alloc);
		rc = rfs_allocator_dump_slice(&m->alloc, false, i, &dl);
		if (rc) {
			rfs_run_dlist_fini(&dl);
			return rc;
		}
		for (n = dl.head; n; n = n->next) {
			u64 s = n->run.base;
			u64 e = s + n->run.len;

			if (s < base)
				s = base;
			if (e > end)
				e = end;
			if (s < e)
				bits_clear_range(m->blk_words, s, e);
		}
		rfs_run_dlist_fini(&dl);
	}
	return 0;
}

/* ═══════════════════ inode 数据面：装载（SB 四字段）═══════════════════════ */

static bool rfs_ip_magic_is(const struct Bptree_meta_t *m)
{
	return m->magics[0] == INODE_DATA_EXTENT_MAGIC1 &&
	       m->magics[1] == INODE_DATA_EXTENT_MAGIC2 &&
	       m->magics[2] == INODE_DATA_EXTENT_MAGIC3 &&
	       m->magics[3] == INODE_DATA_EXTENT_MAGIC4;
}

static int rfs_ip_extent_push(struct rfs_inode_extent **arr, u64 *n, u64 *cap,
			      u64 lblk, u64 phys, u64 len)
{
	if (*n == *cap) {
		u64 ncap = *cap ? *cap * 2ULL : 8ULL;
		struct rfs_inode_extent *ne =
			krealloc(*arr, (size_t)ncap * sizeof(**arr), GFP_KERNEL);

		if (!ne)
			return -ENOMEM;
		*arr = ne;
		*cap = ncap;
	}
	(*arr)[*n].lblk = lblk;
	(*arr)[*n].phys_base = phys;
	(*arr)[*n].length = len;
	(*n)++;
	return 0;
}

/* 栈式 DFS bulk-load 映射 B+tree（逻辑序）。 */
static int rfs_ip_fill_rec(struct super_block *sb, u64 blk, u32 depth,
			   struct rfs_inode_extent **arr, u64 *n, u64 *cap)
{
	const struct Bptree_meta_t *m;
	void *buf;
	u32 cnt, i;
	int err = 0;

	if (blk == (u64)PTR_INVALID || blk == (u64)RFS_MASK52 || depth > 64)
		return -EIO;
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	if (ravenfs_read_node(sb, blk, buf)) {
		kfree(buf);
		return -EIO;
	}
	m = buf;
	if (!rfs_ip_magic_is(m)) {
		kfree(buf);
		return -EIO;
	}
	cnt = RFS_PP_COUNT(m->parent_ptr);
	if (RFS_PP_IS_INNER(m->parent_ptr)) {
		const struct Bptree_inode_data_extent_inner_node_t *nd = buf;

		for (i = 0; i < cnt && i < 338 && !err; i++) {
			u64 cb = RFS_PTR_ADDR(nd->extents[i].sub_node_ptr);

			err = rfs_ip_fill_rec(sb, cb, depth + 1, arr, n, cap);
		}
	} else {
		const struct Bptree_inode_data_extent_leaf_node_t *lf = buf;

		for (i = 0; i < cnt && i < 338 && !err; i++) {
			err = rfs_ip_extent_push(arr, n, cap,
					lf->extents[i].fblkbase,
					RFS_PTR_ADDR(lf->extents[i].block_base),
					lf->extents[i].length);
		}
	}
	kfree(buf);
	return err;
}

/* 装载 inode 数据面：extent 数组 + fr 数组（均调用方 kfree）。 */
static int ravenfs_inode_plane_load(struct super_block *sb,
				    struct rfs_inode_extent **out_ext, u64 *out_en,
				    struct inode_dataplane_coordinate_free_range_t **out_fr,
				    u64 *out_fn, u64 *out_pieces)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 ctl = sbi->inode_data_ctl;
	u64 ptr = RFS_IDP_CTL_PTR(ctl);
	u64 pieces = RFS_IDP_PIECE_COUNT(sbi->inode_data_plane_pieces_count);
	struct rfs_inode_extent *ea = NULL;
	u64 en = 0, cap = 0;
	int rc = 0;

	*out_ext = NULL;
	*out_fr = NULL;
	*out_en = 0;
	*out_fn = 0;
	*out_pieces = 0;

	if (!pieces || ptr == RFS_IDP_CTL_PTR_INVALID)
		return -EINVAL;

	if (RFS_IDP_CTL_IS_DIRECT(ctl)) {
		rc = rfs_ip_extent_push(&ea, &en, &cap, 0, ptr, pieces * 2ULL);
		if (rc)
			return rc;
	} else {
		rc = rfs_ip_fill_rec(sb, ptr, 0, &ea, &en, &cap);
		if (rc) {
			kfree(ea);
			return rc;
		}
	}

	/* 扁 fr 快照：hdr 64B + n×16B；首块 1 块（4K）对齐。 */
	{
		u64 fptr = RFS_IDP_FR_CTL_PTR(sbi->inode_data_fr_ctl);
		u64 n = RFS_IDP_FR_COUNT(sbi->inode_data_fr_count);
		struct inode_dataplane_coordinate_free_range_t *fa = NULL;
		u64 fbytes, fblks, b;
		u8 *buf;
		const struct inode_data_fr_snapshot_hdr *h;

		if (fptr == RFS_IDP_FR_CTL_PTR_INVALID || n == 0)
			goto done;			/* 空 fr（防御） */

		fbytes = sizeof(*h) +
			 n * sizeof(struct inode_dataplane_coordinate_free_range_t);
		fblks = DIV_ROUND_UP_ULL(fbytes, RAVENFS_BLOCK_SIZE);
		if (fblks == 0)
			fblks = 1;
		buf = kzalloc((size_t)fblks * RAVENFS_BLOCK_SIZE, GFP_KERNEL);
		if (!buf) {
			kfree(ea);
			return -ENOMEM;
		}
		for (b = 0; b < fblks; b++) {
			struct buffer_head *bh =
				sb_bread(sb, ravenfs_dev_blk(sbi, fptr + b));

			if (!bh) {
				kfree(buf);
				kfree(ea);
				return -EIO;
			}
			memcpy(buf + (b << 12), bh->b_data, RAVENFS_BLOCK_SIZE);
			brelse(bh);
		}
		h = (const struct inode_data_fr_snapshot_hdr *)buf;
		if (h->magic[0] != INODE_DATA_FR_SNAPSHOT_MAGIC1 ||
		    h->magic[1] != INODE_DATA_FR_SNAPSHOT_MAGIC2 ||
		    h->magic[2] != INODE_DATA_FR_SNAPSHOT_MAGIC3 ||
		    h->magic[3] != INODE_DATA_FR_SNAPSHOT_MAGIC4 ||
		    h->entry_count != n) {
			kfree(buf);
			kfree(ea);
			return -EIO;
		}
		{
			u32 stored = h->crc32, calc;

			((struct inode_data_fr_snapshot_hdr *)buf)->crc32 = 0;
			calc = crc32_le(~0U, buf,
					sizeof(*h) + n * sizeof(struct
					inode_dataplane_coordinate_free_range_t)) ^ ~0U;
			if (stored != calc) {
				kfree(buf);
				kfree(ea);
				return -EIO;
			}
		}
		fa = kmalloc((size_t)n * sizeof(*fa), GFP_KERNEL);
		if (!fa) {
			kfree(buf);
			kfree(ea);
			return -ENOMEM;
		}
		memcpy(fa, buf + sizeof(*h),
		       (size_t)n * sizeof(*fa));
		kfree(buf);
		*out_fr = fa;
		*out_fn = n;
	}
done:
	*out_ext = ea;
	*out_en = en;
	*out_pieces = pieces;
	return 0;
}

/* ═══════════════════ inode 数据面：维护 / 落盘（⇄ all_sycn ①）═══════════════════════ */

static int ravenfs_inode_plane_sync(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct rfs_allocator *a = sbi->alloc;
	struct rfs_inode_plane *p;
	u64 en, pieces, old_ctl, old_ptr, i;
	bool old_is_tree;
	int rc = 0;

	if (!a || !a->ino_plane.active)
		return 0;
	p = &a->ino_plane;

	/* ── ①a 翻译面 extent：单连续段 → direct；否则整棵映射 B+tree bulk build ── */
	en = rfs_inode_plane_extent_count(p);
	pieces = rfs_inode_plane_pieces(p);
	old_ctl = sbi->inode_data_ctl;
	old_ptr = RFS_IDP_CTL_PTR(old_ctl);
	old_is_tree = (old_ptr != RFS_IDP_CTL_PTR_INVALID) &&
		      !RFS_IDP_CTL_IS_DIRECT(old_ctl);
	if (en) {
		struct rfs_inode_extent *ea =
			kmalloc((size_t)en * sizeof(*ea), GFP_KERNEL);
		bool direct_single;

		if (!ea)
			return -ENOMEM;
		if (rfs_inode_plane_dump_extents(p, ea, en)) {
			kfree(ea);
			return -EIO;
		}
		direct_single = (en == 1 && ea[0].lblk == 0 &&
				 ea[0].length == pieces * 2ULL);
		if (direct_single) {
			if (old_is_tree)
				ravenfs_extent_orphan_release_kind(sb,
						RFS_EXT_KIND_INODE, old_ptr);
			sbi->inode_data_ctl = RFS_IDP_CTL_PACK(1, ea[0].phys_base);
		} else {
			u64 new_root = (u64)PTR_INVALID;

			for (i = 0; i < en; i++) {
				struct extent_t x;

				memset(&x, 0, sizeof(x));
				x.length = ea[i].length;
				x.fblkbase = ea[i].lblk;
				RFS_PTR_SET_ADDR(x.block_base, ea[i].phys_base);
				RFS_PTR_SET_FREE(x.block_base, 0);
				rc = ravenfs_extent_insert_kind(sb,
						RFS_EXT_KIND_INODE,
						RFS_ROOT_INODE_IDX, &new_root,
						&x, NULL);
				if (rc) {
					if (new_root != (u64)PTR_INVALID)
						ravenfs_extent_orphan_release_kind(sb,
							RFS_EXT_KIND_INODE,
							new_root);
					kfree(ea);
					return rc;
				}
			}
			if (old_is_tree)
				ravenfs_extent_orphan_release_kind(sb,
						RFS_EXT_KIND_INODE, old_ptr);
			sbi->inode_data_ctl = RFS_IDP_CTL_PACK(0, new_root);
		}
		kfree(ea);
	}
	RFS_IDP_PIECE_SET_COUNT(sbi->inode_data_plane_pieces_count, pieces);
	sbi->ra_inode_count = pieces * RFS_IDP_INODES_PER_PIECE;

	/* ── ①b fr：释放旧扁区 → hard_alloc 新区（1 块对齐）→ 整段写 ── */
	{
		u64 fn = rfs_inode_plane_fr_count(p);
		struct inode_dataplane_coordinate_free_range_t *fa = NULL;
		u64 bytes, blks, new_fr, b;
		u8 *buf;
		struct inode_data_fr_snapshot_hdr *h;

		if (fn) {
			fa = kmalloc((size_t)fn * sizeof(*fa), GFP_KERNEL);
			if (!fa)
				return -ENOMEM;
			if (rfs_inode_plane_dump_fr(p, fa, fn)) {
				kfree(fa);
				return -EIO;
			}
		}
		{
			u64 ofr = RFS_IDP_FR_CTL_PTR(sbi->inode_data_fr_ctl);
			u64 ofn = RFS_IDP_FR_COUNT(sbi->inode_data_fr_count);

			if (ofr != RFS_IDP_FR_CTL_PTR_INVALID) {
				u64 ob = sizeof(*h) + ofn * sizeof(*fa);
				u64 obl = DIV_ROUND_UP_ULL(ob, RAVENFS_BLOCK_SIZE);

				if (obl == 0)
					obl = 1;
				(void)rfs_allocator_block_free(a, ofr, obl);
			}
		}
		bytes = sizeof(*h) + fn * sizeof(*fa);
		blks = DIV_ROUND_UP_ULL(bytes, RAVENFS_BLOCK_SIZE);
		if (blks == 0)
			blks = 1;
		new_fr = rfs_allocator_block_alloc_hard(a, blks, 0);
		if (new_fr == RFS_IV_NO_HIT) {
			kfree(fa);
			return -ENOSPC;
		}
		buf = kzalloc((size_t)blks * RAVENFS_BLOCK_SIZE, GFP_KERNEL);
		if (!buf) {
			(void)rfs_allocator_block_free(a, new_fr, blks);
			kfree(fa);
			return -ENOMEM;
		}
		h = (struct inode_data_fr_snapshot_hdr *)buf;
		h->magic[0] = INODE_DATA_FR_SNAPSHOT_MAGIC1;
		h->magic[1] = INODE_DATA_FR_SNAPSHOT_MAGIC2;
		h->magic[2] = INODE_DATA_FR_SNAPSHOT_MAGIC3;
		h->magic[3] = INODE_DATA_FR_SNAPSHOT_MAGIC4;
		h->entry_count = fn;
		h->capacity = fn;
		h->rsv0 = 0;
		h->rsv1[0] = 0;
		if (fn)
			memcpy(buf + sizeof(*h), fa,
			       (size_t)fn * sizeof(*fa));
		h->crc32 = 0;
		h->crc32 = crc32_le(~0U, buf, (size_t)bytes) ^ ~0U;
		for (b = 0; b < blks; b++) {
			struct buffer_head *bh =
				sb_bread(sb, ravenfs_dev_blk(sbi, new_fr + b));

			if (!bh) {
				(void)rfs_allocator_block_free(a, new_fr, blks);
				kfree(buf);
				kfree(fa);
				return -EIO;
			}
			memcpy(bh->b_data, buf + (b << 12), RAVENFS_BLOCK_SIZE);
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
			brelse(bh);
		}
		kfree(buf);
		kfree(fa);
		sbi->inode_data_fr_ctl = RFS_IDP_FR_CTL_PACK(new_fr);
		sbi->inode_data_fr_count = fn;
	}
	return 0;
}

/* ═══════════════════════ 生命周期三函数 ═══════════════════════ */

int ravenfs_allocator_create(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_alloc_mgr *m;
	struct rfs_slices_cfg cfg;
	struct rfs_inode_extent *ea = NULL;
	struct inode_dataplane_coordinate_free_range_t *fa = NULL;
	u64 en = 0, fn = 0, pieces = 0;
	u64 bn;
	int rc;

	if (!sbi)
		return -EINVAL;
	if (sbi->alloc)
		return 0;			/* 已建（幂等） */

	bn = sbi->bitmap_blocks * RAVENFS_BITS_PER_BLOCK;
	if (!bn)
		return -EINVAL;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->sb = sb;
	m->blk_nbits = bn;
	m->blk_words = vzalloc(bn >> 3);
	if (!m->blk_words) {
		kfree(m);
		return -ENOMEM;
	}

	rc = bm_image_load(sb, m->blk_words, sbi->bitmap_blocks, sbi->bitmap_start);
	if (rc) {
		vfree(m->blk_words);
		kfree(m);
		return rc;
	}

	/* inode 数据面装载（SB 四字段 → 现成数组；rfs_allocator_init 一次消费） */
	rc = ravenfs_inode_plane_load(sb, &ea, &en, &fa, &fn, &pieces);
	if (rc) {
		pr_err("ravenfs: inode plane load fail (%d)\n", rc);
		vfree(m->blk_words);
		kfree(m);
		return rc;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.block_bits_raw = m->blk_words;
	cfg.block_bits_count = bn;
	cfg.logical_processor_count = (u32)num_possible_cpus();
	cfg.min_slice_bits = 0;
	cfg.alloc = ko_mem_alloc;
	cfg.stat.inc = ko_al_stat_inc;
	cfg.stat.ctx = sb;
	cfg.inode.pieces = pieces;
	cfg.inode.extents = ea;
	cfg.inode.extent_count = en;
	cfg.inode.fr = fa;
	cfg.inode.fr_count = fn;
	cfg.inode.grow_batch_pieces = 1;

	rc = rfs_allocator_init(&m->alloc, &cfg);
	kfree(ea);
	kfree(fa);
	if (rc) {
		pr_err("ravenfs: allocator: 建树失败 (%d)\n", rc);
		RFS_AL_INC(sb, al_init_fail);
		vfree(m->blk_words);
		kfree(m);
		return rc;
	}

	sbi->alloc = &m->alloc;
	RFS_AL_INC(sb, al_init);
	RFS_AL_ADD(sb, al_slices, rfs_allocator_slice_count(&m->alloc));
	pr_info("ravenfs: allocator: tree online  blk_slices=%u blk_bits=%llu(free=%llu) inode_pieces=%llu(free=%llu)\n",
		rfs_allocator_slice_count(&m->alloc),
		(unsigned long long)bn,
		(unsigned long long)rfs_allocator_block_free_total(&m->alloc),
		(unsigned long long)pieces,
		(unsigned long long)rfs_allocator_inode_free_total(&m->alloc));
	return 0;
}

int ravenfs_allocator_flush(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_alloc_mgr *m = (sbi && sbi->alloc)
		? container_of(sbi->alloc, struct ravenfs_alloc_mgr, alloc) : NULL;
	int rc;

	if (!m)
		return 0;
	if (!ravenfs_sb_writable(sb))		/* ro 挂载：不落盘 */
		return 0;

	/* (1) inode 数据面维护（取块 → 建树 / 重写 fr；刷新 SB 四字段） */
	rc = ravenfs_inode_plane_sync(sb);
	if (rc) {
		RFS_AL_INC(sb, al_checkpoint_fail);
		pr_err("ravenfs: inode plane sync 失败 (%d)\n", rc);
		return rc;
	}
	/* (2) mdata 脏节点（新树节点 / inode 片内容）落盘 */
	rc = ravenfs_mdata_writeback_all(sb);
	if (rc) {
		RFS_AL_INC(sb, al_checkpoint_fail);
		return rc;
	}
	(void)ravenfs_issue_flush(sb);

	/* (3) block 位图补集重建 + 写回 */
	rc = checkpoint_block(m);
	if (rc) {
		RFS_AL_INC(sb, al_checkpoint_fail);
		return rc;
	}
	rc = bm_image_save(sb, m->blk_words, sbi->bitmap_blocks, sbi->bitmap_start);
	if (rc) {
		RFS_AL_INC(sb, al_checkpoint_fail);
		pr_err("ravenfs: block bitmap image save 失败 (%d)\n", rc);
		return rc;
	}
	RFS_AL_INC(sb, al_checkpoint);
	RFS_AL_ADD(sb, al_checkpoint_blocks, sbi->bitmap_blocks);
	(void)ravenfs_issue_flush(sb);

	/* (4) SB 最后写（crc + latest_syn_time + inode 四字段） */
	rc = ravenfs_sb_write_sync(sb);
	if (rc)
		RFS_AL_INC(sb, al_checkpoint_fail);
	return rc;
}

void ravenfs_allocator_destroy(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_alloc_mgr *m = (sbi && sbi->alloc)
		? container_of(sbi->alloc, struct ravenfs_alloc_mgr, alloc) : NULL;

	if (!m)
		return;
	RFS_AL_INC(sb, al_shutdown);
	if (rfs_allocator_shutdown(&m->alloc, 0) != 0)
		RFS_AL_INC(sb, al_drain_timeout);
	rfs_allocator_fini(&m->alloc);
	sbi->alloc = NULL;
	vfree(m->blk_words);
	kfree(m);
}

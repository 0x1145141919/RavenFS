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
#include <linux/bitmap.h>
#include <linux/mutex.h>

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

/*
 * 测试钩子（默认关；生产路径无副作用）：强制挂载期 inode 面压实走「无连续段」回退
 * 分支，用于验证回退语义（挂载不失败 / 保持 tree / 计数 / 数据完整），无需真把盘填到
 * 碎片化到无 pieces×2 连续段。置 1 → compact 跳过 block_alloc_hard、按 ENOSPC 处理。
 * 经 `/sys/module/ravenfs/parameters/idp_compact_force_fail` 或 insmod 参数设置。
 */
static bool idp_compact_force_fail;
module_param_named(idp_compact_force_fail, idp_compact_force_fail, bool, 0644);
MODULE_PARM_DESC(idp_compact_force_fail,
	"test-only: force mount-time inode-plane compaction to the no-contiguous-run fallback");

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
	u8		*blk_words;	/* block 位图镜像（checkpoint 底座 + 落盘载体）*/
	u64		 blk_nbits;
	struct mutex	 flush_lock;	/* flush 串行化（sync_fs/fsync/TTL/put_super 并发）*/
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

/* 片 i 覆盖的位图块区间 [*b0, *b1)（块下标，相对 bitmap_start）。
 * 片基址恒为 32768 位（= 1 位图块）倍数 → *b0 精确；末片可短 → *b1 向上取整并夹到
 * 位图总块数。*/
static void bm_slice_block_range(const struct ravenfs_alloc_mgr *m, u64 bitmap_blocks,
				 u32 i, u64 per, u64 *b0, u64 *b1)
{
	u64 base = (u64)i * per;
	u64 end = base + per;

	if (end > m->blk_nbits)
		end = m->blk_nbits;
	*b0 = base / RAVENFS_BITS_PER_BLOCK;
	*b1 = DIV_ROUND_UP(end, RAVENFS_BITS_PER_BLOCK);
	if (*b1 > bitmap_blocks)
		*b1 = bitmap_blocks;
}

/* 写回 block 位图镜像（D-15 按脏写回）。
 *   · `wr` 非空：只写「本次 checkpoint 收集到的待写片」覆盖的位图块；
 *   · `wr` 空（分配失败降级 / 无脏位图）：退化为全量写。
 * 返回 0 / -EIO；实际写出的块数写入 *out_blocks（供 `al_checkpoint_blocks` 记脏块数）。
 * 保留逐块 mark + `sync_dirty_buffer` 的确定性落盘语义（批量提交 = D-15 正交第二刀，
 * 本轮暂不引入，先只收敛「写多少」）。*/
static int bm_image_save(struct super_block *sb, struct ravenfs_alloc_mgr *m,
			 const unsigned long *wr, u64 *out_blocks)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 per = rfs_allocator_block_per_slice_bits(&m->alloc);
	u32 ns = rfs_allocator_slice_count(&m->alloc);
	u64 wrote = 0;
	u32 i;

	if (out_blocks)
		*out_blocks = 0;
	if (!m->blk_words || !per || !ns)
		return -EINVAL;
	for (i = 0; i < ns; i++) {
		u64 b0, b1, b;

		if ((u64)i * per >= m->blk_nbits)
			break;
		if (wr && !test_bit(i, wr))
			continue;			/* 非待写片：跳过 */
		bm_slice_block_range(m, sbi->bitmap_blocks, i, per, &b0, &b1);
		for (b = b0; b < b1; b++) {
			struct buffer_head *bh =
				sb_bread(sb, ravenfs_dev_blk(sbi, sbi->bitmap_start + b));

			if (!bh) {
				RFS_STAT_INC(sb, err_meta_io);
				pr_err("ravenfs: bitmap image save fail at FS_blk %llu\n",
				       (unsigned long long)(sbi->bitmap_start + b));
				if (out_blocks)
					*out_blocks = wrote;
				return -EIO;
			}
			memcpy(bh->b_data, m->blk_words + (b << 12), RAVENFS_BLOCK_SIZE);
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
			brelse(bh);
			RFS_STAT_INC(sb, io_write_ops);
			RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
			wrote++;
		}
	}
	if (out_blocks)
		*out_blocks = wrote;
	return 0;
}

/* 单片补集重建：全片置占用 → dump 该片空闲区间清零（树为唯一真值）。*/
static int bm_rebuild_slice(struct ravenfs_alloc_mgr *m, u32 i, u64 base, u64 end)
{
	struct rfs_run_dlist dl;
	struct rfs_run_node *n;
	int rc;

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
	return 0;
}

/* block 组补集重建（D-15 按脏重建）：
 *   · `wr` 非空：只重建「自上次落盘以来变过」的片——脏标志取自**片内隐状态**
 *     （`rfs_allocator_block_dirty_take`，读-改-清、持片锁），非外层注入钩子。
 *     取脏在重建**之前**：期间并发 alloc/free 会重新置脏 → 下轮再写，绝不丢更新。
 *   · `wr` 空（分配失败降级）：退化旧「全片重建」。
 * 重建出的待写片集合写入调用方本地 `wr`，供 bm_image_save 定写回范围。*/
static int checkpoint_block(struct ravenfs_alloc_mgr *m, bool full,
			    unsigned long *wr)
{
	u64 per = rfs_allocator_block_per_slice_bits(&m->alloc);
	u32 ns = rfs_allocator_slice_count(&m->alloc);
	u64 nbits = m->blk_nbits;
	bool selective = !full && wr;
	u32 i;

	if (!m->blk_words || !per || !ns)
		return -EINVAL;
	if (wr)
		bitmap_zero(wr, ns);
	for (i = 0; i < ns; i++) {
		u64 base = (u64)i * per;
		u64 end = base + per;
		int rc;

		if (base >= nbits)
			break;
		if (end > nbits)
			end = nbits;

		if (selective && !rfs_allocator_block_dirty_take(&m->alloc, i))
			continue;			/* 该片自上次落盘未变：跳过 */
		rc = bm_rebuild_slice(m, i, base, end);
		if (rc)
			return rc;
		if (wr)
			set_bit(i, wr);
	}
	return 0;
}

/* D-15 组合入口：重建（按脏）→ 写回（按待写片）。待写片集合**本调用本地化**
 * （栈外小位图，片数不过数百位），无共享可变态 → 并发 flush（TTL vs sync_fs）安全。
 * 位图分配失败 → wr=NULL → checkpoint/bm_image_save 自动全量（旧行为）。*/
static int bm_checkpoint_save(struct super_block *sb, struct ravenfs_alloc_mgr *m,
			      u64 *out_blocks)
{
	u32 ns = rfs_allocator_slice_count(&m->alloc);
	unsigned long *wr;
	int rc;

	if (out_blocks)
		*out_blocks = 0;
	wr = ns ? bitmap_zalloc(ns, GFP_NOFS) : NULL;
	rc = checkpoint_block(m, wr == NULL, wr);
	if (!rc)
		rc = bm_image_save(sb, m, wr, out_blocks);
	bitmap_free(wr);
	return rc;
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

		RFS_AL_INC(sb, al_idp_snap_read);
		fbytes = sizeof(*h) +
			 n * sizeof(struct inode_dataplane_coordinate_free_range_t);
		fblks = DIV_ROUND_UP_ULL(fbytes, RAVENFS_BLOCK_SIZE);
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
			/* 先发布新 ctl，再释放旧树：避免「ctl 仍指旧树而旧树已释放」的
			 * 内存窗口（旧树释放不可逆）。 */
			sbi->inode_data_ctl = RFS_IDP_CTL_PACK(1, ea[0].phys_base);
			if (old_is_tree)
				ravenfs_extent_orphan_release_kind(sb,
						RFS_EXT_KIND_INODE, old_ptr);
		} else {
			u64 new_root = (u64)PTR_INVALID;

			RFS_AL_INC(sb, al_idp_tree_build);
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
			/* 先发布新 ctl，再释放旧树（同上；new_root 已构建完成）。 */
			sbi->inode_data_ctl = RFS_IDP_CTL_PACK(0, new_root);
			if (old_is_tree)
				ravenfs_extent_orphan_release_kind(sb,
						RFS_EXT_KIND_INODE, old_ptr);
		}
		kfree(ea);
	}
	RFS_IDP_PIECE_SET_COUNT(sbi->inode_data_plane_pieces_count, pieces);
	sbi->ra_inode_count = pieces * RFS_IDP_INODES_PER_PIECE;

	/* ── ①b fr：hard_alloc 新区 → 整段写（逐块 sync 落盘）→ 发布新 ctl → 释放旧扁区 ──
	 * 加固：旧 fr 改为**最后**释放。原顺序「先释放旧 → 分配/写新」在 alloc 失败
	 * （-ENOSPC/-ENOMEM/-EIO）时已把旧 fr 释放，而 sbi->inode_data_fr_ctl 仍指旧
	 * → on-disk SB 指向已释放块。新顺序保证任何早退路径旧 fr 仍然完好。 */
	{
		u64 fn = rfs_inode_plane_fr_count(p);
		struct inode_dataplane_coordinate_free_range_t *fa = NULL;
		u64 bytes, blks, new_fr, b;
		u64 ofr = RFS_IDP_FR_CTL_PTR(sbi->inode_data_fr_ctl);
		u64 ofn = RFS_IDP_FR_COUNT(sbi->inode_data_fr_count);
		u64 obl = 0;
		u8 *buf;
		struct inode_data_fr_snapshot_hdr *h;

		if (ofr != RFS_IDP_FR_CTL_PTR_INVALID)
			obl = DIV_ROUND_UP_ULL(sizeof(*h) + ofn * sizeof(*fa),
					       RAVENFS_BLOCK_SIZE);
		if (fn) {
			fa = kmalloc((size_t)fn * sizeof(*fa), GFP_KERNEL);
			if (!fa)
				return -ENOMEM;
			if (rfs_inode_plane_dump_fr(p, fa, fn)) {
				kfree(fa);
				return -EIO;
			}
		}
		bytes = sizeof(*h) + fn * sizeof(*fa);
		blks = DIV_ROUND_UP_ULL(bytes, RAVENFS_BLOCK_SIZE);
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
		RFS_AL_INC(sb, al_idp_snap_write);
		RFS_AL_ADD(sb, al_idp_snap_blocks, blks);
		kfree(buf);
		kfree(fa);
		sbi->inode_data_fr_ctl = RFS_IDP_FR_CTL_PACK(new_fr);
		sbi->inode_data_fr_count = fn;
		/* 新 fr 已逐块 sync 落盘且 ctl 已切换 → 此时才回收旧扁区 */
		if (ofr != RFS_IDP_FR_CTL_PTR_INVALID)
			(void)rfs_allocator_block_free(a, ofr, obl);
	}
	return 0;
}

/* ═══════════════════ inode 数据面：挂载期压实（tree → direct）═══════════════════════
 *
 * 目标：挂载时若全局 inode 数据面处于 tree 态，把逻辑空间 [0, pieces×8K) 的物理承载
 * 整体搬迁到一段**新分配的、连续且 8K 对齐**的物理段（pieces×2 个 FS_block），随后把
 * SB 的 `inode_data_ctl` 切成 direct(ptr=新段基)。此后 inode 访问由「映射树查表 +
 * 物理块计算」降为「基块 + 逻辑块」直算（挂载后 inode 访问更快）。旧映射树 + 旧数据片
 * 回收。本特性 ⇄ god `name_data_rebuild`（juvenile_god/name_data_plane.cpp）在名字面
 * 的同形操作，此处抬升到**全局 inode 面**。
 *
 * 崩溃安全硬不变量（勿改！）——**盘上块位图任何时刻都不得把 SB/元数据仍引用的块标成
 * 空闲**（= 未保护 = 后续复用 = 损坏）。故写序只能是：
 *   ① 内存分配器取新连续段（新段亮灯）；
 *   ② 逐 8K 片拷贝旧数据 → 新段并落盘 + flush（提交前新段内容完整）；
 *   ③ 位图落盘：此刻新段**已亮**、旧树/旧片**仍亮**（保守；绝不先放旧）；
 *   ④ 写 SB：inode_data_ctl = direct(新段) —— **提交点**；
 *   ⑤ 提交成功后才在内存分配器释放旧树节点 + 旧数据片，再次位图落盘。
 * 崩溃点：
 *   · ④ 前崩 → SB 仍 = tree 指旧树；旧树/旧片在盘在位图亮（引用与被引用都在）→ 完好，
 *     新段虽亮无人引用 = 孤儿（fsck 收，无害）；
 *   · ④ 与 ⑤ 之间崩 → SB = direct 指新段（新段在、位图亮）→ 完好；旧树/旧片仍亮 = 孤儿；
 *   · 绝不出现「SB = direct 且新段位图暗」= 未保护（= 后续被复用 = 损坏）。
 * 注：此处**不复用** §17.4 `ravenfs_inode_plane_sync` 的顺序——那条为运行期增量同步设计
 * （先翻内存 ctl 再回收旧树），本压实是挂载期一次性搬迁，必须先把新段持久化再翻 SB。
 *
 * 前置：rw 挂载、单线程（fill_super 内、root iget 之前）。已 direct / 空态 / pieces==0
 * = no-op（幂等）。fr 快照（`inode_data_fr_ctl` 区）**不变**：fr 坐标 = inode 逻辑量，
 * 与物理搬迁无关。返回 0 = 成功或无需压实；负 errno = 压实失败（调用方忽略，照常挂载）。
 */

/* 逐 8K 片把旧物理承载抄入新连续段（旧段由 ea 的稠密 extent 描述；片恒 8K 对齐）。 */
static int ip_compact_copy_pieces(struct super_block *sb,
				  const struct rfs_inode_extent *ea, u64 en,
				  u64 new_base, u64 pieces)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	const u32 npb = RAVENFS_MDATA_BLK_PER_NODE;	/* 每 8K 片 = 2 个 4K 块 */
	u8 *buf;
	u64 j = 0, i;
	int rc = 0;

	buf = kmalloc(RAVENFS_MDATA_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	for (i = 0; i < pieces && !rc; i++) {
		u64 lblk = i * 2ULL;			/* 片逻辑起点（4K 块） */
		u64 old_blk = 0;
		u32 b;

		while (j < en && ea[j].lblk + ea[j].length <= lblk)
			j++;
		if (j >= en || lblk < ea[j].lblk ||
		    lblk >= ea[j].lblk + ea[j].length) {
			rc = -EIO;			/* 逻辑空间应稠密无洞 */
			break;
		}
		old_blk = ea[j].phys_base + (lblk - ea[j].lblk);
		for (b = 0; b < npb && !rc; b++) {	/* 读旧片 2 块 */
			struct buffer_head *bh = sb_bread(sb,
					ravenfs_dev_blk(sbi, old_blk + b));

			if (!bh) {
				rc = -EIO;
				break;
			}
			memcpy(buf + ((u64)b << 12), bh->b_data, RAVENFS_BLOCK_SIZE);
			brelse(bh);
			RFS_STAT_INC(sb, io_read_ops);
			RFS_STAT_ADD(sb, io_read_bytes, RAVENFS_BLOCK_SIZE);
		}
		for (b = 0; b < npb && !rc; b++) {	/* 写新片 2 块（同步落盘） */
			struct buffer_head *bh = sb_bread(sb,
					ravenfs_dev_blk(sbi, new_base + i * 2ULL + b));

			if (!bh) {
				rc = -EIO;
				break;
			}
			memcpy(bh->b_data, buf + ((u64)b << 12), RAVENFS_BLOCK_SIZE);
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
			brelse(bh);
			RFS_STAT_INC(sb, io_write_ops);
			RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
		}
	}
	kfree(buf);
	return rc;
}

int ravenfs_inode_plane_compact(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct rfs_allocator *a;
	struct ravenfs_alloc_mgr *m;
	struct rfs_inode_plane *p;
	struct rfs_inode_extent *ea = NULL;
	u64 en, pieces, old_ctl, old_ptr, need, new_base, i;
	bool moved = false;
	int rc;

	if (!sbi || !sbi->alloc)
		return 0;
	if (!ravenfs_sb_writable(sb))		/* ro 挂载：不写盘、不改面 */
		return 0;
	a = sbi->alloc;
	if (!a->ino_plane.active)
		return 0;
	p = &a->ino_plane;
	m = container_of(a, struct ravenfs_alloc_mgr, alloc);

	old_ctl = sbi->inode_data_ctl;
	old_ptr = RFS_IDP_CTL_PTR(old_ctl);
	if (RFS_IDP_CTL_IS_DIRECT(old_ctl) || old_ptr == RFS_IDP_CTL_PTR_INVALID)
		return 0;			/* 已 direct / 空态：幂等 no-op */

	pieces = rfs_inode_plane_pieces(p);
	en = rfs_inode_plane_extent_count(p);
	if (pieces == 0 || en == 0)
		return 0;

	/* 翻译面快照（旧映射）。提交前不改内存面 → 早退路径可无损回退。 */
	ea = kmalloc((size_t)en * sizeof(*ea), GFP_KERNEL);
	if (!ea)
		return -ENOMEM;
	if (rfs_inode_plane_dump_extents(p, ea, en)) {
		kfree(ea);
		return -EIO;
	}

	/* 内存翻译面已单连续段（tree 仅为历史盘面）→ 无需搬迁数据，直接提交。 */
	if (en == 1 && ea[0].lblk == 0 && ea[0].length == pieces * 2ULL) {
		new_base = ea[0].phys_base;
	} else {
		need = pieces * 2ULL;
		/* align_log2=1 → 2 块（8K）对齐，与片语义一致。 */
		new_base = idp_compact_force_fail ? (u64)RFS_IV_NO_HIT
			: rfs_allocator_block_alloc_hard(a, need, 1);
		if (new_base == (u64)RFS_IV_NO_HIT) {
			RFS_AL_INC(sb, al_idp_compact_fail);
			pr_warn("ravenfs: inode plane compact: no contiguous run (%llu blocks) — stay tree\n",
				(unsigned long long)need);
			kfree(ea);
			return -ENOSPC;
		}
		rc = ip_compact_copy_pieces(sb, ea, en, new_base, pieces);
		if (rc) {
			(void)rfs_allocator_block_free(a, new_base, need);
			RFS_AL_INC(sb, al_idp_compact_fail);
			pr_warn("ravenfs: inode plane compact: piece copy failed (%d) — stay tree\n",
				rc);
			kfree(ea);
			return rc;
		}
		(void)ravenfs_issue_flush(sb);	/* 新段数据先持久（提交前） */
		moved = true;
	}

	/* ③ 位图落盘：新段已亮、旧树/旧片仍亮（保守）。失败 = 不可提交 → 回退。 */
	rc = bm_checkpoint_save(sb, m, NULL);
	if (rc) {
		if (moved)
			(void)rfs_allocator_block_free(a, new_base, pieces * 2ULL);
		RFS_AL_INC(sb, al_idp_compact_fail);
		pr_warn("ravenfs: inode plane compact: bitmap persist failed (%d) — stay tree\n",
			rc);
		kfree(ea);
		return rc;
	}

	/* ④ 提交点：内存翻译面换向 + SB 写 direct（先新段持久、后翻 SB）。 */
	rfs_inode_plane_set_direct(p, new_base, pieces);
	sbi->inode_data_ctl = RFS_IDP_CTL_PACK(1, new_base);
	rc = ravenfs_sb_write_sync(sb);
	if (rc) {
		/* SB 写失败：**不释放新段**（盘上 SB 可能已 = direct 指它），保持 direct
		 * 内存态防 umount 误回收旧树 → 仅泄漏，绝不损坏。 */
		RFS_AL_INC(sb, al_idp_compact_fail);
		pr_err("ravenfs: inode plane compact: SB commit failed (%d) — direct left in memory, old leaked\n",
		       rc);
		kfree(ea);
		return rc;
	}

	/* ⑤ 提交成功：回收旧映射树 + 旧数据片（尽力而为），再次位图落盘。 */
	ravenfs_extent_orphan_release_kind(sb, RFS_EXT_KIND_INODE, old_ptr);
	if (moved) {
		for (i = 0; i < en; i++) {
			(void)rfs_allocator_block_free(a, ea[i].phys_base,
						       ea[i].length);
			ravenfs_mdata_drop(sb, ea[i].phys_base);
		}
	}
	(void)bm_checkpoint_save(sb, m, NULL);
	kfree(ea);

	RFS_AL_INC(sb, al_idp_compact_ok);
	RFS_AL_ADD(sb, al_idp_compact_pieces, pieces);
	pr_info("ravenfs: inode plane compact: tree→direct ok base=%llu pieces=%llu (old_tree=%llu old_extents=%llu)\n",
		(unsigned long long)new_base, (unsigned long long)pieces,
		(unsigned long long)old_ptr, (unsigned long long)en);
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
	mutex_init(&m->flush_lock);
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

/* flush 主体（**须持 m->flush_lock**）：inode 面维护 → mdata → block 位图（按脏）→ SB。*/
static int ravenfs_allocator_flush_locked(struct super_block *sb,
					  struct ravenfs_alloc_mgr *m)
{
	u64 wrote = 0;
	int rc;

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

	/* (3) block 位图补集重建 + 写回（D-15：只重建/回写脏片） */
	rc = bm_checkpoint_save(sb, m, &wrote);
	if (rc) {
		RFS_AL_INC(sb, al_checkpoint_fail);
		return rc;
	}
	RFS_AL_INC(sb, al_checkpoint);
	RFS_AL_ADD(sb, al_checkpoint_blocks, wrote);	/* 语义：实际写出的脏块数 */
	(void)ravenfs_issue_flush(sb);

	/* (4) SB 最后写（crc + latest_syn_time + inode 四字段） */
	rc = ravenfs_sb_write_sync(sb);
	if (rc)
		RFS_AL_INC(sb, al_checkpoint_fail);
	return rc;
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

	/* 串行化 flush：`sync_fs` / 每文件 `fsync` / TTL / `put_super` 可并发进入。
	 * 不加锁时 SB 块 0 缓冲是**共享可变态**（`ravenfs_sb_write_sync` 读-改-写 + crc），
	 * 两路并发会 crc 自撞 → -EIO → force_shutdown；inode 面 fr 重写亦会重复 alloc/free
	 * 抖动。此锁令整段 flush 独占（可睡锁，块 I/O 内持锁可接受）。*/
	mutex_lock(&m->flush_lock);
	rc = ravenfs_allocator_flush_locked(sb, m);
	mutex_unlock(&m->flush_lock);
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
	mutex_destroy(&m->flush_lock);
	vfree(m->blk_words);
	kfree(m);
}

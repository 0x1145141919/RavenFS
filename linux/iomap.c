// SPDX-License-Identifier: GPL-2.0
/*
 * iomap.c —— RavenFS ko：文件数据路径（逻辑偏移 → iomap 映射）；M4.1 读 + M4.2 写
 *
 * 依据：Docs/linux/ravenfs-linux-ko-M4-data-spec.md
 *   §1.2（7.2.3 iomap 真值）、§2.3（唯一自写核心 iomap_begin/end）、
 *   §3（三态→iomap type，判据 size 优先 = 不变量 22）、§6（读粒度 = iomap 默认）、
 *   §8（写序/崩溃：**立刻分配 D-M4-6**、IOMAP_F_NEW 零填、短写尾不清算 D-M4-10(b)）。
 * 机械真值：Docs/disk_layout_v1.1.md §5（区段树）+ §10 不变量 22/23/24/25；
 *           Docs/disk_layout_v1.2_inode.md §2（inode 位域）。
 *
 * 纪律（spec §2.3）：
 *   · 页缓存 / folio / bio / readahead / 回写 **全由 iomap 核心代劳**；
 *     本文件只回答「逻辑偏移 pos → 物理映射 / 空洞 / 内联」，并在写向**立刻真分配**。
 *   · **不返回 IOMAP_DELALLOC**（D-M4-6 立刻分配）；ENOSPC 在 write(2) 即暴露。
 *   · srcmap = iomap（无 COW）。
 *
 * 三态解码（不变量 22，**判据 size 优先**，data_root 取值不参与判定）：
 *   size ≤ 160            → IOMAP_INLINE（内容在 inode.inline_data，offset=0）
 *   size > 160 ∧ direct=1 → IOMAP_MAPPED（连续区 data_root + pos/4096）
 *   size > 160 ∧ direct=0 → extent 树 walk（命中 MAPPED / 未命中 HOLE）
 *
 * 写向（IOMAP_WRITE）：
 *   inline 不越 160 → 照旧 inline；越 160 → **inline→extent 迁移**（D-M4-8）；
 *   direct 覆盖 → MAPPED（不分配）；direct 增长（不可续）→ **direct→extent 迁移**；
 *   extent 命中 → MAPPED（不分配）；extent 洞 → **立刻分配 + 插树**，返 MAPPED|IOMAP_F_NEW。
 *   迁移的盘面物化在本文件完成（iomap.c 是唯一触碰 data_root/direct_bit 的提交点之一）。
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
#include <linux/timekeeping.h>

#include "ravenfs.h"
#include "ravenfs_iomap.h"
#include "ravenfs_extent.h"
#include "ravenfs_cache.h"
#include "ravenfs_bitmap.h"

/* inline 态字节上限（不变量 22）：size ≤ 160 ⇒ 内容在 inode.inline_data */
#define RAVENFS_INLINE_BYTES		160U
#define RAVENFS_BLK_SHIFT		12			/* 4096 = 1 << 12 */
/* size → 覆盖块数 ceil(size/4096)（不变量 22） */
#define RAVENFS_NBLOCKS(b)		(((b) + RAVENFS_BLOCK_SIZE - 1) >> RAVENFS_BLK_SHIFT)

/* FS_block 号 → 块设备字节偏移（含 blk0_off，恒 = 4096） */
static inline u64 ravenfs_dev_byte(struct ravenfs_sb_info *sbi, u64 fs_blk)
{
	return (u64)ravenfs_dev_blk(sbi, fs_blk) << RAVENFS_BLK_SHIFT;
}

/* 同步落一个 4K 块（迁移物化/零填用；与装载侧同源 sb_bread + sync_dirty_buffer） */
static int rfs_store_block(struct super_block *sb, u64 fs_blk, const void *buf)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, fs_blk));

	if (!bh)
		return -EIO;
	memcpy(bh->b_data, buf, RAVENFS_BLOCK_SIZE);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	RFS_STAT_INC(sb, io_write_ops);
	RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
	return 0;
}

/* 区域树 walk（上界路由）/ i_blocks 计数在 extent_write.c：
 *   ravenfs_extent_lookup() / ravenfs_extent_insert() / ravenfs_extent_count() */

/* ══════════════════════════════════════════════════════════════════
 * 写向（IOMAP_WRITE）：立刻分配（D-M4-6）
 * ══════════════════════════════════════════════════════════════════ */

/* inline→extent 迁移（D-M4-8：越 160 默认升格 extent）
 *
 * 分配策略（**只为真正需要的块分配**，缝隙保留为洞——与 god/验收模型一致）：
 *   · **D-6a：先试 direct**（整域连续分配成功即落 direct，零树节点，见下 helper）；
 *   · pos 块号 == 0（写从第 0 块起）：单条 extent 覆盖 [0, end_blk)，块 0 物化
 *     旧 inline + 零填，尾块（若 end 未块对齐）零填，返回映射**非 NEW**。
 *   · pos 块号 > 0：旧 inline 物化为单块 extent {0,1}（仅当 old_size>0），写域另
 *     一条 extent {pos_blk, cnt}，返回映射 **NEW**（单次调用，无重入丢标志问题）；
 *     缝隙 [1, pos_blk) 保留为洞（读 0）。
 */

/* D-6a：inline→外置迁移先试 direct。
 *
 * 口径对齐 god dfprealloc_and_plan 的「inline 迁出 direct 候选」
 * （juvenile_god/direct_file_ops.cpp:118-129）：整域 [0, end_blk) 一次连续分配
 * 成功 → 单条连续 run 覆盖整文件 → 落 direct（不变量 24：direct = 单条连续、
 * 零树节点最优路径），**不建 8K 区段树根叶**。分配失败（不连续 / 无块 →
 * PTR_INVALID）→ 返回 RFS_MIGRATE_INLINE_DEGRADE，调用方降级走原 extent 迁移。
 *
 * 返回：0 = 已落 direct（iomap 已填、inode 记录已提交）；
 *       RFS_MIGRATE_INLINE_DEGRADE = 不可 direct（分配失败/不连续）→ 降级；
 *       <0 = 硬错误（分配已回滚）。
 */
#define RFS_MIGRATE_INLINE_DEGRADE	1

static int ravenfs_migrate_inline_direct(struct super_block *sb, struct raven_Inode_t *r,
					 struct ravenfs_session *ses, loff_t pos, u64 end,
					 u64 end_blk, struct iomap *iomap)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 old_size = r->size;			/* ≤160 */
	u64 pos_blk = (u64)pos >> RAVENFS_BLK_SHIFT;
	u64 run, b;
	u8 *buf;
	int err;

	/* 整域 [0, end_blk) 一次连续分配（align=1 块，同既有 direct/坍缩口径）。
	 * 不连续/无块 → PTR_INVALID → 降级（不建树、不落 direct）。 */
	run = ravenfs_block_alloc(sb, end_blk, 0);
	if (run == (u64)PTR_INVALID)
		return RFS_MIGRATE_INLINE_DEGRADE;

	buf = kzalloc(RAVENFS_BLOCK_SIZE, GFP_KERNEL);
	if (!buf) {
		ravenfs_block_free(sb, run, end_blk);
		return -ENOMEM;
	}

	/* block 0 = 旧 inline 内容 + 零填（[old_size,4096) 读 0）。
	 * pos_blk==0 时写域覆盖 [pos,end)，但 block 0 的 [0,pos) 须保留 inline。 */
	memset(buf, 0, RAVENFS_BLOCK_SIZE);
	memcpy(buf, r->inline_data, min_t(u64, old_size, RAVENFS_INLINE_BYTES));
	err = rfs_store_block(sb, run, buf);
	if (err)
		goto err_free_run;

	/* 未写占位块 [1, pos_blk) 显式清零：写域之前、恒为洞，但 direct 全覆盖
	 * 语义下必须读 0（god phase3 ①「占位块批量清零」同机械）。写域首块前导
	 * [blkstart,pos) 与写尾块未写区由 iomap 零填兜底（block_start ≥ i_size
	 * 或 NEW；与既有 pos_blk>0 分支同口径）。 */
	if (pos_blk > 1) {
		memset(buf, 0, RAVENFS_BLOCK_SIZE);
		for (b = 1; b < pos_blk; b++) {
			err = rfs_store_block(sb, run + b, buf);
			if (err)
				goto err_free_run;
		}
	}
	/* 尾块零填（end 未块对齐，且非 block 0——block 0 已 inline+零填）。
	 * end_blk==1 时唯一块即 block 0，无需（且不可）二次零填。 */
	if (end_blk > 1 && ((u64)end & (RAVENFS_BLOCK_SIZE - 1))) {
		memset(buf, 0, RAVENFS_BLOCK_SIZE);
		err = rfs_store_block(sb, run + end_blk - 1, buf);
		if (err)
			goto err_free_run;
	}
	kfree(buf);

	/* 提交记录：direct=1；data_root=连续 run 首块；size≥end。 */
	RFS_INO1_SET_DIRECT(*r, 1);
	r->data_root = run;
	if (end > r->size)
		r->size = end;
	RFS_ROW_MARK_DIRTY(ses->row);

	iomap->offset = ALIGN_DOWN(pos, RAVENFS_BLOCK_SIZE);
	iomap->type = IOMAP_MAPPED;
	iomap->addr = ravenfs_dev_byte(sbi, run + pos_blk);
	iomap->length = (end_blk - pos_blk) << RAVENFS_BLK_SHIFT;
	/* pos_blk==0：非 NEW（保 block 0 内 [0,pos) 的 inline 内容；NEW 会清零
	 * mapping 前导）。pos_blk>0：NEW（零填写域首块前导之洞）。 */
	iomap->flags = pos_blk ? IOMAP_F_NEW : 0;
	return 0;

err_free_run:
	kfree(buf);
	ravenfs_block_free(sb, run, end_blk);
	return err;
}

static int ravenfs_write_migrate_inline(struct super_block *sb, struct inode *inode,
					struct raven_Inode_t *r, struct ravenfs_session *ses,
					loff_t pos, u64 end, struct iomap *iomap)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 old_size = r->size;			/* ≤160 */
	u64 pos_blk = (u64)pos >> RAVENFS_BLK_SHIFT;
	u64 end_blk = RAVENFS_NBLOCKS(end);
	u64 root = (u64)PTR_INVALID, tmp, run = (u64)PTR_INVALID;
	u8 *buf;
	int err;

	if (!end_blk)
		end_blk = 1;

	/* ── D-6a：先试 direct（整域连续分配 → 零树节点最优路径）── */
	err = ravenfs_migrate_inline_direct(sb, r, ses, pos, end, end_blk, iomap);
	if (err != RFS_MIGRATE_INLINE_DEGRADE)
		return err;			/* 0 = 已落 direct；<0 = 硬错误 */

	/* ── 降级：原 inline→extent 迁移（建 8K 根叶）── */
	buf = kzalloc(RAVENFS_BLOCK_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (pos_blk == 0) {
		struct extent_t e0;

		run = ravenfs_block_alloc(sb, end_blk, 0);
		if (run == (u64)PTR_INVALID) {
			kfree(buf);
			return -ENOSPC;
		}
		/* 块 0 = 旧 inline + 零填 */
		memset(buf, 0, RAVENFS_BLOCK_SIZE);
		memcpy(buf, r->inline_data, min_t(u64, old_size, RAVENFS_INLINE_BYTES));
		err = rfs_store_block(sb, run, buf);
		if (err)
			goto err_free_run;
		/* 尾块零填（若 end 未块对齐；中间块写全覆，无需落盘） */
		if (end_blk > 1 && ((u64)end & (RAVENFS_BLOCK_SIZE - 1))) {
			memset(buf, 0, RAVENFS_BLOCK_SIZE);
			err = rfs_store_block(sb, run + end_blk - 1, buf);
			if (err)
				goto err_free_run;
		}
		kfree(buf);
		e0.length = end_blk;
		e0.fblkbase = 0;
		e0.block_base.v = RFS_PTR_PACK(0, run);
		tmp = root;
		err = ravenfs_extent_insert(sb, inode->i_ino, &tmp, &e0);
		if (err)
			return err;		/* 孤儿块（inode 未动，§8） */
		root = tmp;
		iomap->offset = ALIGN_DOWN(pos, RAVENFS_BLOCK_SIZE);
		iomap->type = IOMAP_MAPPED;
		iomap->addr = ravenfs_dev_byte(sbi, run);
		iomap->length = (end_blk << RAVENFS_BLK_SHIFT) - (u64)pos;
		iomap->flags = 0;
	} else {
		u64 cnt = end_blk - pos_blk;
		struct extent_t e1;

		if (old_size > 0) {		/* 旧 inline 单块物化（非 NEW） */
			u64 run0 = ravenfs_block_alloc(sb, 1, 0);

			if (run0 == (u64)PTR_INVALID) {
				kfree(buf);
				return -ENOSPC;
			}
			memset(buf, 0, RAVENFS_BLOCK_SIZE);
			memcpy(buf, r->inline_data, min_t(u64, old_size, RAVENFS_INLINE_BYTES));
			err = rfs_store_block(sb, run0, buf);
			if (!err) {
				struct extent_t e0;

				e0.length = 1;
				e0.fblkbase = 0;
				e0.block_base.v = RFS_PTR_PACK(0, run0);
				tmp = root;
				err = ravenfs_extent_insert(sb, inode->i_ino, &tmp, &e0);
				root = tmp;
			}
			if (err) {
				ravenfs_block_free(sb, run0, 1);
				kfree(buf);
				return err;
			}
		}
		kfree(buf);

		run = ravenfs_block_alloc(sb, cnt, 0);
		if (run == (u64)PTR_INVALID)
			return -ENOSPC;
		e1.length = cnt;
		e1.fblkbase = pos_blk;
		e1.block_base.v = RFS_PTR_PACK(0, run);
		err = ravenfs_extent_insert(sb, inode->i_ino, &root, &e1);
		if (err) {
			ravenfs_block_free(sb, run, cnt);
			return err;
		}
		iomap->offset = ALIGN_DOWN(pos, RAVENFS_BLOCK_SIZE);
		iomap->type = IOMAP_MAPPED;
		iomap->addr = ravenfs_dev_byte(sbi, run);
		iomap->length = cnt << RAVENFS_BLK_SHIFT;
		iomap->flags = IOMAP_F_NEW;
	}

	/* 提交记录：direct=0；data_root=root；size≥end（强制 extent 解码，不变量 22） */
	RFS_INO1_SET_DIRECT(*r, 0);
	r->data_root = root;
	if (end > r->size)
		r->size = end;
	RFS_ROW_MARK_DIRTY(ses->row);
	return 0;

err_free_run:
	kfree(buf);
	ravenfs_block_free(sb, run, end_blk);
	return err;
}

/* direct→extent 迁移（增长不可续）：旧连续区物化一条 + 新增长区一条（零搬移） */
static int ravenfs_write_migrate_direct(struct super_block *sb, struct inode *inode,
					struct raven_Inode_t *r, struct ravenfs_session *ses,
					loff_t pos, u64 end, struct iomap *iomap)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 old_ceil = RAVENFS_NBLOCKS(r->size);
	u64 old_base = r->data_root;
	u64 new_ceil = RAVENFS_NBLOCKS(end);
	u64 grow = (new_ceil > old_ceil) ? (new_ceil - old_ceil) : 1;
	u64 run, root = (u64)PTR_INVALID, tmp;
	struct extent_t e0, e1;
	u8 *zbuf;
	u64 i, blk, phys, run_end_blk;
	int err;

	run = ravenfs_block_alloc(sb, grow, 0);
	if (run == (u64)PTR_INVALID)
		return -ENOSPC;

	/* 零填新增长块（非 NEW：部分块读需 0） */
	zbuf = kzalloc(RAVENFS_BLOCK_SIZE, GFP_KERNEL);
	if (!zbuf) {
		ravenfs_block_free(sb, run, grow);
		return -ENOMEM;
	}
	for (i = 0; i < grow; i++) {
		err = rfs_store_block(sb, run + i, zbuf);
		if (err) {
			kfree(zbuf);
			ravenfs_block_free(sb, run, grow);
			return err;
		}
	}
	kfree(zbuf);

	/* 建树：旧 direct 区（零搬移）+ 新增长区 */
	e0.length = old_ceil;
	e0.fblkbase = 0;
	e0.block_base.v = RFS_PTR_PACK(0, old_base);
	tmp = root;
	err = ravenfs_extent_insert(sb, inode->i_ino, &tmp, &e0);
	if (err)
		return err;
	root = tmp;

	e1.length = grow;
	e1.fblkbase = old_ceil;
	e1.block_base.v = RFS_PTR_PACK(0, run);
	err = ravenfs_extent_insert(sb, inode->i_ino, &root, &e1);
	if (err)
		return err;

	/* 提交记录：direct=0；data_root=root（size 不变，仍 >160 → extent 解码） */
	RFS_INO1_SET_DIRECT(*r, 0);
	r->data_root = root;
	RFS_ROW_MARK_DIRTY(ses->row);

	blk = (u64)pos >> RAVENFS_BLK_SHIFT;
	if (blk < old_ceil) {
		phys = old_base + blk;
		run_end_blk = old_ceil;
	} else {
		phys = run + (blk - old_ceil);
		run_end_blk = new_ceil;
	}
	iomap->offset = ALIGN_DOWN(pos, RAVENFS_BLOCK_SIZE);
	iomap->type = IOMAP_MAPPED;
	iomap->addr = ravenfs_dev_byte(sbi, phys);
	iomap->length = (run_end_blk - blk) << RAVENFS_BLK_SHIFT;
	iomap->flags = 0;
	return 0;
}

static int ravenfs_iomap_begin_write(struct inode *inode, loff_t pos, loff_t length,
				     unsigned int flags, struct iomap *iomap,
				     struct iomap *srcmap)
{
	struct super_block *sb = inode->i_sb;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;
	struct raven_Inode_t *r;
	u64 size, end, blk, end_blk;
	int err;

	if (flags & IOMAP_DIRECT)
		return -EOPNOTSUPP;		/* O_DIRECT 后置（D-M4-5） */
	ses = ravenfs_session_get(sb, inode->i_ino);
	if (IS_ERR(ses))
		return PTR_ERR(ses);
	r = ravenfs_session_inode(ses);

	memset(iomap, 0, sizeof(*iomap));
	iomap->bdev = sb->s_bdev;
	iomap->addr = IOMAP_NULL_ADDR;

	size = r->size;
	end = (u64)pos + (u64)length;

	/* ── inline 态（size ≤ 160） ── */
	if (size <= RAVENFS_INLINE_BYTES) {
		struct ravenfs_inode_info *ri = RAVENFS_I(inode);

		if (end <= RAVENFS_INLINE_BYTES) {
			/* 仍在内联区：镜像记录 → inode 私有稳定副本（iomap 稍后读取）
			 * 并清尾（i_size 之后到 160）→ 非尾部写（跳写/缝隙）读回 0。 */
			memcpy(ri->inline_data, r->inline_data, sizeof(ri->inline_data));
			if (size < RAVENFS_INLINE_BYTES)
				memset(ri->inline_data + size, 0, RAVENFS_INLINE_BYTES - size);
			iomap->offset = 0;
			iomap->type = IOMAP_INLINE;
			iomap->addr = 0;
			iomap->inline_data = ri->inline_data;
			iomap->length = RAVENFS_INLINE_BYTES;	/* 覆盖 [0,160) 全内联区 */
			goto out;
		}
		err = ravenfs_write_migrate_inline(sb, inode, r, ses, pos, end, iomap);
		if (err)
			return err;
		goto out;
	}

	/* ── size > 160 ── */
	blk = (u64)pos >> RAVENFS_BLK_SHIFT;
	end_blk = RAVENFS_NBLOCKS(end);

	if (RFS_INO1_GET_DIRECT(r->word1)) {
		u64 old_ceil = RAVENFS_NBLOCKS(size);

		if (end <= (old_ceil << RAVENFS_BLK_SHIFT)) {
			/* 直指覆盖：不分配 */
			iomap->offset = ALIGN_DOWN(pos, RAVENFS_BLOCK_SIZE);
			iomap->type = IOMAP_MAPPED;
			iomap->addr = ravenfs_dev_byte(sbi, r->data_root + blk);
			iomap->length = (old_ceil - blk) << RAVENFS_BLK_SHIFT;
			iomap->flags = 0;
			goto out;
		}
		err = ravenfs_write_migrate_direct(sb, inode, r, ses, pos, end, iomap);
		if (err)
			return err;
		goto out;
	}

	/* ── extent 树 ── */
	{
		struct extent_t ext;
		u64 next;

		err = ravenfs_extent_lookup(sb, r->data_root, blk, &ext, &next);
		if (err < 0)
			return err;

		if (err == 0) {			/* 命中：覆盖既有映射 */
			u64 ext_phys = RFS_PTR_ADDR(ext.block_base) + (blk - ext.fblkbase);
			u64 run_end = ext.fblkbase + ext.length;

			iomap->offset = ALIGN_DOWN(pos, RAVENFS_BLOCK_SIZE);
			iomap->type = IOMAP_MAPPED;
			iomap->addr = ravenfs_dev_byte(sbi, ext_phys);
			iomap->length = (run_end - blk) << RAVENFS_BLK_SHIFT;
			iomap->flags = 0;
		} else {			/* 洞 → 立刻分配 + 插树（D-M4-6） */
			u64 hole_end = next ? min(next, end_blk) : end_blk;
			u64 cnt, run, newroot;
			struct extent_t ne;

			if (hole_end <= blk)
				hole_end = blk + 1;	/* 护栏：长度必 > 0 */
			cnt = hole_end - blk;
			run = ravenfs_block_alloc(sb, cnt, 0);
			if (run == (u64)PTR_INVALID) {
				RFS_STAT_INC(sb, err_enospc);
				return -ENOSPC;		/* write(2) 即暴露 */
			}
			ne.length = cnt;
			ne.fblkbase = blk;
			ne.block_base.v = RFS_PTR_PACK(0, run);
			newroot = r->data_root;
			err = ravenfs_extent_insert(sb, inode->i_ino, &newroot, &ne);
			if (err) {
				ravenfs_block_free(sb, run, cnt);
				return err;
			}
			if (newroot != r->data_root) {
				r->data_root = newroot;	/* 叶裂升根 → 新根 */
				RFS_ROW_MARK_DIRTY(ses->row);
			}
			iomap->offset = ALIGN_DOWN(pos, RAVENFS_BLOCK_SIZE);
			iomap->type = IOMAP_MAPPED;
			iomap->addr = ravenfs_dev_byte(sbi, run);
			iomap->length = cnt << RAVENFS_BLK_SHIFT;
			iomap->flags = IOMAP_F_NEW;	/* 新分配块 → iomap 核心零填未写部分 */
		}
	}

out:
	*srcmap = *iomap;			/* 无 COW：srcmap = iomap */
	return 0;
}

int ravenfs_iomap_begin(struct inode *inode, loff_t pos, loff_t length,
			unsigned int flags, struct iomap *iomap,
			struct iomap *srcmap)
{
	struct super_block *sb = inode->i_sb;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;
	const struct raven_Inode_t *r;
	u64 size, off, blk, end_blk;
	int err;

	if (flags & IOMAP_REPORT)
		return -EOPNOTSUPP;		/* FIEMAP 后置 */
	/* IOMAP_WRITE 或 IOMAP_ZERO（iomap_zero_range = fallocate/ZERO_RANGE/残块清零）→ 写向 */
	if (flags & (IOMAP_WRITE | IOMAP_ZERO))
		return ravenfs_iomap_begin_write(inode, pos, length, flags, iomap, srcmap);

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
		 * 镜像一份到 inode 私有区。
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

/* i_blocks 诚实化（数据块 + extent 树节点×2；512B 单位） */
static void ravenfs_write_update_blocks(struct super_block *sb, struct inode *inode,
					const struct raven_Inode_t *r)
{
	u64 blks = 0;

	if (r->size > RAVENFS_INLINE_BYTES) {
		if (RFS_INO1_GET_DIRECT(r->word1)) {
			blks = RAVENFS_NBLOCKS(r->size);
		} else {
			u64 data = 0, nodes = 0;

			ravenfs_extent_count(sb, r->data_root, &data, &nodes);
			blks = data + nodes * 2;
		}
	}
	inode->i_blocks = blks * (RAVENFS_BLOCK_SIZE / 512);
}

static int ravenfs_iomap_end(struct inode *inode, loff_t pos, loff_t length,
			     ssize_t written, unsigned int flags, struct iomap *iomap)
{
	struct super_block *sb = inode->i_sb;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;
	struct raven_Inode_t *r;

	if (!(flags & IOMAP_WRITE))
		return 0;		/* 只读：无分配需提交/回滚 */

	ses = ravenfs_session_get(sb, inode->i_ino);
	if (IS_ERR(ses))
		return 0;		/* 提交失败不致命：内存 inode 已更新 */
	r = ravenfs_session_inode(ses);

	/* inline 写：把 inode 私有镜像回灌盘记录（iomap_write_end_inline 只写镜像） */
	if (iomap->type == IOMAP_INLINE) {
		struct ravenfs_inode_info *ri = RAVENFS_I(inode);

		memcpy(r->inline_data, ri->inline_data, sizeof(r->inline_data));
	}

	/* 大小提交（实际写入量）；短写尾**不清算**（D-M4-10(b)，evict 再算） */
	r->size = inode->i_size;

	/* i_blocks 诚实化 */
	ravenfs_write_update_blocks(sb, inode, r);

	RFS_ROW_MARK_DIRTY(ses->row);
	return 0;
}

const struct iomap_ops ravenfs_iomap_ops = {
	.iomap_begin	= ravenfs_iomap_begin,
	.iomap_end	= ravenfs_iomap_end,
};

// SPDX-License-Identifier: GPL-2.0
/*
 * allocator_adapter.c —— 运行时树分配器 struct rfs_allocator 的 FS 侧生命周期接线
 *
 * 契约语义见 ravenfs_allocator_adapter.h。本文件是「树 ↔ 位图镜像」唯一缝合面，
 * 承担三件事（挂载 / checkpoint + 落盘 / 卸载）：
 *   ① create ：从设备读「位图镜像」→ words（vzalloc，4K 对齐）→ `rfs_allocator_init`。
 *              位图 = checkpoint 底座（磁盘契约仍是位图，见 04_sharded_master_slave_rbtree.md §3）。
 *   ② flush  ：逐片 `rfs_allocator_dump_slice`（纯内存快照）→ 补集写回 words
 *              （全置占用 → 空闲区间清零）；再 words 逐块写回设备（sync_dirty_buffer，
 *              返回即落盘）。= 旧「allocator_flush + bitmap_flush」两步合一。
 *   ③ destroy：`rfs_allocator_fini`（关闸 + 排空 + 拆树；超时 fail-safe）+ 释放 words。
 *
 * 背景（2026-10-04 位图模块整体删除 / 硬切）：旧 `rfs_bm` 类与 bitmap_adapter 已退役；
 *   位图镜像的 words 归属 + 块设备 IO 收归本文件（F5 裁决：落盘归 FS 适配层）。
 *
 * ── 分配器内存 / 节点分配回调（注入，模块零直接分配纪律）──
 *   元素池与 run 链节点统一走 `kzalloc(GFP_NOFS)`：
 *     · 分配/释放路径在**片锁（可睡 mutex）内**取/还节点 → 用 GFP_NOFS 免 reclaim
 *       递归回本 fs（见 ravenfs_run_list.h 的“片锁内取节点”注）。
 *     · 生命周期短、频次低，暂不上 kmem_cache（够用即止；要提速再绑专用 cache）。
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

#include "ravenfs.h"
#include "ravenfs_allocator_adapter.h"
#include "ravenfs_allocator_top.h"
#include "ravenfs_run_list.h"

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

/* ═══════════════════════ al_ 计数器钩子（树分配器 → sbi->al）══════════════════════
 * 树分配器平台中立（不知 super_block），经 `struct rfs_stat_hook` 把 (field, delta) 上抛。
 * 本函数是**唯一落点**：按 `enum rfs_al_stat` 下标累加进 `sbi->al`（连续 atomic64 数组）。
 * 纪律（P1/P2）：只 atomic64_add，不改控制流；可持片锁调用（片内埋点），故**不睡眠、不 IO**。*/
static void ko_al_stat_inc(void *ctx, u32 field, u64 delta)
{
	struct super_block *sb = (struct super_block *)ctx;
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	atomic64_t *st;

	if (!sbi || field >= AL_STAT_COUNT)
		return;
	/* 结构体成员全为 atomic64_t 且无填充 → 当连续数组读（BUILD_BUG_ON 守）。*/
	st = (atomic64_t *)&sbi->al;
	atomic64_add((long)delta, &st[field]);
}

/* ═══════════════════════ 管理器（对 sbi 不透明）═══════════════════════ */

struct ravenfs_alloc_mgr {
	struct rfs_allocator alloc;	/* 运行期真值（首成员；sbi->alloc 直指此处）*/
	struct super_block *sb;
	u8		*blk_words;	/* 位图镜像（checkpoint 底座 + 落盘载体；vzalloc，4K 对齐）*/
	u8		*ino_words;
	u64		 blk_nbits;
	u64		 ino_nbits;
};

/* ═══════════════════════ 位区间读写（bit i = byte[i>>3] 的 bit(i&7)，LSB-first）═══════════ */

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

/* ═══════════════════════ 位图镜像：设备 ↔ words ═══════════════════════ */

/* 逐块装载位图镜像（每 4096B 块 = 32768 位）→ words。任一失败 → -EIO。*/
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

/* 逐块把 words 写回位图镜像（确定性：sync_dirty_buffer 返回即落盘）。失败 → -EIO。*/
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

/* ═══════════════════════ checkpoint：一组（block / inode）═══════════════════════ */

/* 把一组的树真值换算成位图字：**全片置占用 → 逐空闲区间清零 = 补集**。
 * 片范围 = [i*per, min(nbits,(i+1)*per))；dump 出的空闲区间是全局位号。*/
static int checkpoint_group(struct ravenfs_alloc_mgr *m, bool ino)
{
	u8 *w = ino ? m->ino_words : m->blk_words;
	u64 nbits = ino ? m->ino_nbits : m->blk_nbits;
	u64 per = ino ? rfs_allocator_inode_per_slice_bits(&m->alloc)
		      : rfs_allocator_block_per_slice_bits(&m->alloc);
	u32 ns = rfs_allocator_slice_count(&m->alloc);
	u32 i;

	if (!w || !per || !ns)
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

		bits_set_range(w, base, end);		/* 先全置占用 */

		rfs_run_dlist_init(&dl, ko_run_alloc);
		rc = rfs_allocator_dump_slice(&m->alloc, ino, i, &dl);
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
				bits_clear_range(w, s, e);	/* 空闲 → 0 */
		}
		rfs_run_dlist_fini(&dl);
	}
	return 0;
}

/* ═══════════════════════ 生命周期三函数 ═══════════════════════ */

int ravenfs_allocator_create(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_alloc_mgr *m;
	struct rfs_slices_cfg cfg;
	u64 bn, in;
	int rc;

	if (!sbi)
		return -EINVAL;
	if (sbi->alloc)
		return 0;			/* 已建（幂等） */

	/* 位图镜像几何（磁盘契约仍是位图；words = 块数 × 4096B）*/
	bn = sbi->bitmap_blocks * RAVENFS_BITS_PER_BLOCK;
	in = sbi->inode_bm_blocks * RAVENFS_BITS_PER_BLOCK;
	if (!bn || !in)
		return -EINVAL;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->sb = sb;
	m->blk_nbits = bn;
	m->ino_nbits = in;
	/* vzalloc 页对齐（≥4K 对齐；树 cfg 要求 words 4K 对齐）*/
	m->blk_words = vzalloc(bn >> 3);
	m->ino_words = vzalloc(in >> 3);
	if (!m->blk_words || !m->ino_words) {
		vfree(m->blk_words);
		vfree(m->ino_words);
		kfree(m);
		return -ENOMEM;
	}

	/* 装载位图镜像 → words，再建树（失败即自清理）*/
	rc = bm_image_load(sb, m->blk_words, sbi->bitmap_blocks, sbi->bitmap_start);
	if (!rc)
		rc = bm_image_load(sb, m->ino_words, sbi->inode_bm_blocks, sbi->inode_bm_base);
	if (rc) {
		vfree(m->blk_words);
		vfree(m->ino_words);
		kfree(m);
		return rc;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.block_bits_raw = m->blk_words;
	cfg.block_bits_count = bn;
	cfg.inode_bits_raw = m->ino_words;
	cfg.inode_bits_count = in;
	cfg.logical_processor_count = (u32)num_possible_cpus();
	cfg.min_slice_bits = 0;			/* 0 → RFS_IV_SLICE_MIN_BITS */
	cfg.alloc = ko_mem_alloc;
	/* al_ 计数钩子：ctx = sb（钩子内取 sbi->al，按 enum 下标累加）。*/
	cfg.stat.inc = ko_al_stat_inc;
	cfg.stat.ctx = sb;

	rc = rfs_allocator_init(&m->alloc, &cfg);
	if (rc) {
		pr_err("ravenfs: allocator: 建树失败 (%d)\n", rc);
		RFS_AL_INC(sb, al_init_fail);
		vfree(m->blk_words);
		vfree(m->ino_words);
		kfree(m);
		return rc;
	}

	sbi->alloc = &m->alloc;
	RFS_AL_INC(sb, al_init);
	RFS_AL_ADD(sb, al_slices, rfs_allocator_slice_count(&m->alloc));
	pr_info("ravenfs: allocator: tree online  slices=%u blk_bits=%llu(free=%llu) ino_bits=%llu(free=%llu)\n",
		rfs_allocator_slice_count(&m->alloc),
		(unsigned long long)bn,
		(unsigned long long)rfs_allocator_block_free_total(&m->alloc),
		(unsigned long long)in,
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
	/* ① 树真值 → words（逐片 snapshot 取补集）*/
	rc = checkpoint_group(m, false);
	if (!rc)
		rc = checkpoint_group(m, true);
	if (rc) {
		RFS_AL_INC(sb, al_checkpoint_fail);
		pr_err("ravenfs: allocator checkpoint 失败 (%d)\n", rc);
		return rc;
	}
	RFS_AL_INC(sb, al_checkpoint);
	/* ② words → 设备（落盘入口；ro 挂载不写）*/
	if (!ravenfs_sb_writable(sb))
		return 0;
	rc = bm_image_save(sb, m->blk_words, sbi->bitmap_blocks, sbi->bitmap_start);
	if (!rc)
		rc = bm_image_save(sb, m->ino_words, sbi->inode_bm_blocks, sbi->inode_bm_base);
	if (rc) {
		RFS_AL_INC(sb, al_checkpoint_fail);
		pr_err("ravenfs: bitmap image save 失败 (%d)\n", rc);
		return rc;
	}
	RFS_AL_ADD(sb, al_checkpoint_blocks,
		     sbi->bitmap_blocks + sbi->inode_bm_blocks);
	return rc;
}

void ravenfs_allocator_destroy(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_alloc_mgr *m = (sbi && sbi->alloc)
		? container_of(sbi->alloc, struct ravenfs_alloc_mgr, alloc) : NULL;

	if (!m)
		return;
	/* 关闸 + 排在途（超时 fail-safe 不释放）+ 拆树；destroy 后指针清空（幂等）。
	 * 计数：先显式 shutdown（拿到 drain 返回码）再 fini（幂等，二次 shutdown 无害）。*/
	RFS_AL_INC(sb, al_shutdown);
	if (rfs_allocator_shutdown(&m->alloc, 0) != 0)
		RFS_AL_INC(sb, al_drain_timeout);	/* 排空超时 → fail-safe 拒绝释放 */
	rfs_allocator_fini(&m->alloc);
	sbi->alloc = NULL;
	vfree(m->blk_words);
	vfree(m->ino_words);
	kfree(m);
}

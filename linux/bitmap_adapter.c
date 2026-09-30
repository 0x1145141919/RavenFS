// SPDX-License-Identifier: GPL-2.0
/*
 * bitmap_adapter.c —— RavenFS ko：双位图分片分配器「适配层」（旧 bitmap.c 的替代）
 *
 * 角色：`ravenfs_bitmap.h` 的 13 个公共 `ravenfs_*`（签名冻结，super.c/cache_ttl.c
 * 零改动）→ 落到新核心类 `struct rfs_bm`（bitmap_core.c 路由 + bitmap_mem.c /
 * bitmap_chunk.c 后端）。适配层只做三件事：
 *   ① `sb/sbi ↔ self` 取值；② `RC ↔ PTR_INVALID/RFS_BM_NO_HIT` 翻译；
 *   ③ 后端选择（MEM/CHUNK）与两个视角各自的 I/O 路径：
 *        · MEM  ：适配层拥有 words（vzalloc 4K 对齐）→ sb_bread 装载 → 类零 I/O；
 *                 flush = 锁内快照一片 → 锁外写（旧 bitmap.c 口径）。
 *        · CHUNK：适配层提供 read_block/write_block（sb_bread / mark_buffer_dirty +
 *                 sync_dirty_buffer + ravenfs_dev_blk 换算，返回 0 / 负 -errno）；
 *                 flush = `rfs_bm_chunks_asyn`（合成后一致视图）。
 *
 * 盘落点（来自 sbi，与 mkfs/mkfs.cpp:calc_geometry 同源）：
 *   block 位图：base = sbi->bitmap_start， blocks = sbi->bitmap_blocks；
 *   inode 位图：base = sbi->inode_bm_base，blocks = sbi->inode_bm_blocks。
 *   位数 = blocks × 32768（与旧 bitmap.c 同口径；不传位数给类，CHUNK 由块数推导）。
 *
 * ── 语义视角纪律（勿混，分不清即误判）──
 *   MEM   ：内存即一等事实 → `block_test` / `*_free_total` 随时可采信。
 *   CHUNK ：合成视角 → 挂账期读数可与真值暂不一致；`flush`/同步与任何"读回一致视图"
 *           一律走合成后口径（`chunks_asyn`）。详见 ravenfs_bitmap_core.h ★★。
 *
 * ── align_log2 集成点 ──
 *   公共 `ravenfs_block_alloc(sb,count,align_log2)` 收 align_log2，本层夹到 ≤16（旧口径）
 *   后把**指数**交给 `rfs_bm_block_alloc_hard`（核头语义："起始位 2^align 对齐"）。
 *   现有调用点为 0/1（dentry_node.c / extent_write.c / name_heap.c / iomap.c）。
 *   ⚠️ CHUNK 后端硬约束 align ≤15（>15 → -EINVAL → 该片换片 → 最终 NO_HIT）；0/1 不踩。
 *
 * ── 诚实边界 ──
 *   · io_ctx 传 `sb`（非 sbi）：sbi 无 sb 反向指针，而 sb_bread 需 sb；核头允许
 *     「io_ctx 传 sb/sbi」，取 sb 可同取二者（sb->s_fs_info），且不加 sbi 字段。
 *   · CHUNK 的 `chunks_asyn` 实现现状 = **逐片整片读+整片写**（见 bitmap_chunk.c 头注）：
 *     常驻内存 ↓，但"30s 全量回写"**未**随之收敛（如实标注，不夸大 D-2）。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/smp.h>
#include <linux/cpumask.h>

#include "ravenfs.h"
#include "ravenfs_bitmap.h"
#include "ravenfs_bitmap_adapter.h"

/* 适配层内部句柄（= sbi->bitmap；旧 bitmap.c 的 struct ravenfs_bitmap_mgr 平移） */
struct ravenfs_bitmap_mgr {
	struct super_block	*sb;
	struct rfs_bm		bm;		/* 新核心类本体（两模式合一）*/
	struct ravenfs_bitmap_opts opts;	/* 本次挂载锁定值（show_options 回读）*/

	/* MEM：适配层拥有位图内存（类只借 words 指针，不取所有权）*/
	u64			*blk_words;
	u64			*ino_words;

	u64			blk_nbits;
	u64			ino_nbits;

	/* MEM flush 写者串行（不挡分配/释放）；CHUNK 由片锁自串行，不用 */
	struct mutex		wb_lock;
};

/* ═══════════════════════ CHUNK I/O 回调（0 / 负 -errno）═══════════════════════ */

/* ── 计数出口：类事件 → sb 级 bm_* 计数器（设计 §3.F）──
 * 类本体 sb-free，只上报「事件 + 增量」；本函数把事件落到 sbi->stats.bm_*。
 * ctx = sb（与 CHUNK io_ctx 同源取法）。只做 atomic64_add，可持锁调用（P1）。
 * 模式契合：MEM 只会发来 ALLOC_*、FREE_*、SOFT_OK、OCCUPY（+adapter 侧 SYNC_SLICES）；
 * CHUNK 另发来 CHUNK_*、REC_*、POOL_EMPTY、SUMMARY_SKIP、SCAN_NS、TTL_*、SYNC_SLICES。
 * 未发出的字段在该后端恒 0（debugfs 读侧恒呈现全字段，非本后端为 0）。*/
static void bm_stat_emit(void *ctx, enum rfs_bm_stat ev, u64 val)
{
	struct super_block *sb = ctx;

	switch (ev) {
	case RFS_BM_ST_ALLOC_CALLS:	RFS_STAT_ADD(sb, bm_alloc_calls, val); break;
	case RFS_BM_ST_ALLOC_OK:	RFS_STAT_ADD(sb, bm_alloc_ok, val); break;
	case RFS_BM_ST_ALLOC_FAIL:	RFS_STAT_ADD(sb, bm_alloc_fail, val); break;
	case RFS_BM_ST_ALLOC_BLOCKS:	RFS_STAT_ADD(sb, bm_alloc_blocks, val); break;
	case RFS_BM_ST_FREE_CALLS:	RFS_STAT_ADD(sb, bm_free_calls, val); break;
	case RFS_BM_ST_FREE_BLOCKS:	RFS_STAT_ADD(sb, bm_free_blocks, val); break;
	case RFS_BM_ST_SOFT_OK:		RFS_STAT_ADD(sb, bm_soft_ok, val); break;
	case RFS_BM_ST_OCCUPY:		RFS_STAT_ADD(sb, bm_occupy, val); break;
	case RFS_BM_ST_SYNC_SLICES:	RFS_STAT_ADD(sb, bm_sync_slices, val); break;
	case RFS_BM_ST_CHUNK_SWITCH:	RFS_STAT_ADD(sb, bm_chunk_switch, val); break;
	case RFS_BM_ST_CHUNK_SWITCH_NS:	RFS_STAT_ADD(sb, bm_chunk_switch_ns, val); break;
	case RFS_BM_ST_REC_ATTACH:	RFS_STAT_ADD(sb, bm_rec_attach, val); break;
	case RFS_BM_ST_REC_DRAIN:	RFS_STAT_ADD(sb, bm_rec_drain, val); break;
	case RFS_BM_ST_POOL_EMPTY:	RFS_STAT_ADD(sb, bm_pool_empty, val); break;
	case RFS_BM_ST_SUMMARY_SKIP:	RFS_STAT_ADD(sb, bm_summary_skip, val); break;
	case RFS_BM_ST_SCAN_NS:		RFS_STAT_ADD(sb, bm_scan_ns, val); break;
	case RFS_BM_ST_TTL_ONLINE:	RFS_STAT_ADD(sb, bm_ttl_online, val); break;
	case RFS_BM_ST_TTL_DRAIN:	RFS_STAT_ADD(sb, bm_ttl_drain, val); break;
	default:			break;
	}
}

/* 读 blk_len 个 4096B 块（FS_block 视角）到 buf；任一失败 → -EIO */
static int bm_io_read(void *io_ctx, u64 blk, u64 blk_len, void *buf)
{
	struct super_block *sb = io_ctx;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 i;

	for (i = 0; i < blk_len; i++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, blk + i));

		if (!bh) {
			RFS_STAT_INC(sb, err_meta_io);
			pr_err("ravenfs: bm chunk read fail at FS_blk %llu\n",
			       (unsigned long long)(blk + i));
			return -EIO;
		}
		memcpy((u8 *)buf + (i << 12), bh->b_data, RAVENFS_BLOCK_SIZE);
		brelse(bh);
		RFS_STAT_INC(sb, io_read_ops);
		RFS_STAT_ADD(sb, io_read_bytes, RAVENFS_BLOCK_SIZE);
	}
	return 0;
}

/* 写 blk_len 个 4096B 块（确定性：sync_dirty_buffer 返回即落盘）；失败 → -EIO */
static int bm_io_write(void *io_ctx, u64 blk, u64 blk_len, const void *buf)
{
	struct super_block *sb = io_ctx;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 i;

	for (i = 0; i < blk_len; i++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, blk + i));

		if (!bh) {
			RFS_STAT_INC(sb, err_meta_io);
			pr_err("ravenfs: bm chunk write fail at FS_blk %llu\n",
			       (unsigned long long)(blk + i));
			return -EIO;
		}
		memcpy(bh->b_data, (const u8 *)buf + (i << 12), RAVENFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
		RFS_STAT_INC(sb, io_write_ops);
		RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
	}
	return 0;
}

/* ═══════════════════════ MEM 装载 / 全量刷回 ═══════════════════════ */

/* 逐块装载位图（与旧 bitmap.c rfs_bm_group_load 同源 sb_bread）*/
static int bm_mem_load(struct super_block *sb, u64 *words, u64 nblocks, u64 disk_start)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 b;

	for (b = 0; b < nblocks; b++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, disk_start + b));

		if (!bh) {
			RFS_STAT_INC(sb, err_meta_io);
			pr_err("ravenfs: bitmap load fail at FS_blk %llu\n",
			       (unsigned long long)(disk_start + b));
			return -EIO;
		}
		memcpy((u8 *)words + (b << 12), bh->b_data, RAVENFS_BLOCK_SIZE);
		brelse(bh);
		RFS_STAT_INC(sb, io_read_ops);
		RFS_STAT_ADD(sb, io_read_bytes, RAVENFS_BLOCK_SIZE);
	}
	return 0;
}

/* 每片位数（与 core bm_per_bits 同式：nbits/n_slices 上取整到 32768 倍数）——
 * 仅用于 MEM flush 的片→盘换算；n_slices 取自核心（不重复推导片数）。*/
static u64 bm_mem_per_bits(u64 nbits, u32 n_slices)
{
	u64 per;

	if (!n_slices)
		return RFS_BM_SLICE_MIN_BITS;
	per = DIV_ROUND_UP_ULL(nbits, n_slices);
	per = (per + RFS_BM_SLICE_MIN_BITS - 1) & ~(RFS_BM_SLICE_MIN_BITS - 1);
	return per ? per : RFS_BM_SLICE_MIN_BITS;
}

/* MEM 单组全量刷回：逐片「锁内快照一片 → 锁外逐块写」（与旧 bitmap.c 口径一致）。
 * 片 = 整块边界（per_bits/base_idx 均 32768 对齐）⇒ bits/8 为 4K 整数倍。*/
static int bm_mem_flush_group(struct ravenfs_bitmap_mgr *m, bool ino)
{
	struct super_block *sb = m->sb;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 nbits = ino ? m->ino_nbits : m->blk_nbits;
	u64 disk_start = ino ? sbi->inode_bm_base : sbi->bitmap_start;
	u32 n_slices = rfs_bm_slice_count(&m->bm);
	u64 per_bits, buf_bytes;
	void *buf;
	u32 i;
	int err = 0;

	if (!n_slices)
		return 0;
	per_bits = bm_mem_per_bits(nbits, n_slices);
	buf_bytes = per_bits >> 3;
	if (!buf_bytes)
		return 0;
	buf = kvmalloc(buf_bytes, GFP_KERNEL);	/* 片上限缓冲；锁外分配 */
	if (!buf)
		return -ENOMEM;

	mutex_lock(&m->wb_lock);
	for (i = 0; i < n_slices; i++) {
		u64 base = (u64)i * per_bits;
		u64 bits, nb, b;

		if (base >= nbits)
			break;
		bits = min(per_bits, nbits - base);
		/* 锁内快照该片（bits/8 字节，≤ buf_bytes）→ 锁外 I/O */
		rfs_bm_snapshot_slice(&m->bm, ino, i, buf);
		nb = bits >> 15;		/* /32768 = 4096B 块数 */
		for (b = 0; b < nb; b++) {
			u64 dblk = disk_start + (base >> 15) + b;
			struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, dblk));

			if (!bh) {
				err = -EIO;
				break;
			}
			memcpy(bh->b_data, (u8 *)buf + (b << 12), RAVENFS_BLOCK_SIZE);
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);	/* 确定性：返回即落盘 */
			brelse(bh);
			RFS_STAT_INC(sb, io_write_ops);
			RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
		}
		if (err)
			break;			/* 一片失败即停，余片下轮/下次重试 */
		RFS_STAT_INC(sb, bm_sync_slices);	/* FULL：逐片全量写回片数 */
	}
	mutex_unlock(&m->wb_lock);
	kvfree(buf);
	return err;
}

/* ═══════════════════════ 装配（带选项）═══════════════════════ */

int ravenfs_bitmap_create_mode(struct super_block *sb, const struct ravenfs_bitmap_opts *opts)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m;
	struct ravenfs_bitmap_opts o;
	u64 blk_bits, ino_bits;
	u32 ncpu;
	int err;

	if (!sbi)
		return -EINVAL;

	/* 选项默认 + 值域防御（正常在 parse_param/get_tree 已拦，此处兜底）*/
	o.mode = RFS_BM_MODE_MEM;
	o.blk_chunk_order = RAVENFS_BM_DEFAULT_BLK_CHUNK_ORDER;
	o.ino_chunk_order = RAVENFS_BM_DEFAULT_INO_CHUNK_ORDER;
	o.row_size = 0;
	if (opts) {
		if (opts->mode != RFS_BM_MODE_MEM && opts->mode != RFS_BM_MODE_CHUNK)
			return -EINVAL;
		if (ravenfs_bm_order_check(opts->blk_chunk_order, &o.blk_chunk_order) ||
		    ravenfs_bm_order_check(opts->ino_chunk_order, &o.ino_chunk_order))
			return -EINVAL;
		o.mode = opts->mode;
		o.row_size = opts->row_size;
	}

	blk_bits = sbi->bitmap_blocks * RAVENFS_BITS_PER_BLOCK;
	ino_bits = sbi->inode_bm_blocks * RAVENFS_BITS_PER_BLOCK;
	if (!blk_bits || !ino_bits)
		return -EINVAL;
	ncpu = num_possible_cpus();

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->sb = sb;
	mutex_init(&m->wb_lock);
	m->opts = o;
	m->blk_nbits = blk_bits;
	m->ino_nbits = ino_bits;

	if (o.mode == RFS_BM_MODE_MEM) {
		struct rfs_bm_cfg cfg;

		/* vzalloc 页对齐（≥4K 对齐；核头要求 words 4K 对齐）*/
		m->blk_words = vzalloc(blk_bits >> 3);
		m->ino_words = vzalloc(ino_bits >> 3);
		if (!m->blk_words || !m->ino_words) {
			err = -ENOMEM;
			goto bad;
		}
		err = bm_mem_load(sb, m->blk_words, sbi->bitmap_blocks, sbi->bitmap_start);
		if (err)
			goto bad;
		err = bm_mem_load(sb, m->ino_words, sbi->inode_bm_blocks, sbi->inode_bm_base);
		if (err)
			goto bad;

		memset(&cfg, 0, sizeof(cfg));
		cfg.choose = RFS_BM_MODE_MEM;
		cfg.stat.emit = bm_stat_emit;		/* 计数出口：类事件 → sbi->stats.bm_* */
		cfg.stat.ctx = sb;
		cfg.pure_mem_cfg.block_bits_raw = (u8 *)m->blk_words;
		cfg.pure_mem_cfg.block_bits_count = blk_bits;
		cfg.pure_mem_cfg.inode_bits_raw = (u8 *)m->ino_words;
		cfg.pure_mem_cfg.inode_bits_count = ino_bits;
		cfg.pure_mem_cfg.logical_processor_count = ncpu;
		cfg.pure_mem_cfg.min_slice_bits = 0;	/* 0 → RFS_BM_SLICE_MIN_BITS */
		err = rfs_bm_init(&m->bm, &cfg);
		if (err)
			goto bad;
		rfs_bm_rescan_free(&m->bm);		/* 装载后重扫（幂等；init 已扫）*/
	} else {
		struct rfs_bm_cfg cfg;

		memset(&cfg, 0, sizeof(cfg));
		cfg.choose = RFS_BM_MODE_CHUNK;
		cfg.stat.emit = bm_stat_emit;		/* 计数出口：类事件 → sbi->stats.bm_* */
		cfg.stat.ctx = sb;
		cfg.chunk_cfg.read_block = bm_io_read;
		cfg.chunk_cfg.write_block = bm_io_write;
		cfg.chunk_cfg.io_ctx = sb;		/* 核头允许 sb/sbi；取 sb（见文件头）*/
		cfg.chunk_cfg.block_bitmap_base_FS_block = sbi->bitmap_start;
		cfg.chunk_cfg.block_bitmap_blocks = sbi->bitmap_blocks;
		cfg.chunk_cfg.inode_bitmap_base_FS_block = sbi->inode_bm_base;
		cfg.chunk_cfg.inode_bitmap_blocks = sbi->inode_bm_blocks;
		cfg.chunk_cfg.blkbm_chunk_order = o.blk_chunk_order;
		cfg.chunk_cfg.inobm_chunk_order = o.ino_chunk_order;
		cfg.chunk_cfg.processor_count = ncpu;
		err = rfs_bm_init(&m->bm, &cfg);	/* enable 内一次整片读实测 free_count */
		if (err)
			goto bad;
	}

	sbi->bitmap = m;
	pr_info("ravenfs: bitmap mgr: mode=%s slices=%u blk_bits=%llu(free=%llu) ino_bits=%llu(free=%llu)\n",
		o.mode == RFS_BM_MODE_CHUNK ? "chunk" : "mem",
		rfs_bm_slice_count(&m->bm),
		(unsigned long long)blk_bits, (unsigned long long)ravenfs_block_free_total(sb),
		(unsigned long long)ino_bits, (unsigned long long)ravenfs_inode_free_total(sb));
	return 0;
bad:
	rfs_bm_fini(&m->bm);		/* 幂等（init 失败已自清理；全零 self 亦安全）*/
	vfree(m->blk_words);
	vfree(m->ino_words);
	kfree(m);
	return err;
}

/* 13 公共 API 之一的默认入口（无选项 → mem，与旧行为逐条一致）*/
int ravenfs_bitmap_create(struct super_block *sb)
{
	struct ravenfs_bitmap_opts o = {
		.mode = RFS_BM_MODE_MEM,
		.blk_chunk_order = RAVENFS_BM_DEFAULT_BLK_CHUNK_ORDER,
		.ino_chunk_order = RAVENFS_BM_DEFAULT_INO_CHUNK_ORDER,
		.row_size = 0,
	};

	return ravenfs_bitmap_create_mode(sb, &o);
}

/* ═══════════════════════ 拆解 / 刷回 ═══════════════════════ */

void ravenfs_bitmap_destroy(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;

	if (!m)
		return;
	rfs_bm_fini(&m->bm);		/* CHUNK：逐片 disable（脏/挂账才 I/O）；MEM：释放片/槽 */
	vfree(m->blk_words);		/* MEM 拥有者释放；CHUNK 为 NULL（vfree(NULL) 安全）*/
	vfree(m->ino_words);
	sbi->bitmap = NULL;
	kfree(m);
}

/* 全片同步（两组）：TTL 周期 / sync_fs / put_super / fsync 共用；ro 挂载不得调用 */
int ravenfs_bitmap_flush(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;
	int err = 0;

	if (!m)
		return 0;
	if (m->bm.mode == RFS_BM_MODE_CHUNK)
		return rfs_bm_chunks_asyn(&m->bm);	/* 合成后一致视图（读-合并-写）*/
	if (bm_mem_flush_group(m, false))
		err = -EIO;
	if (bm_mem_flush_group(m, true))
		err = -EIO;
	if (err)
		pr_err("ravenfs: bitmap flush failed\n");
	return err;
}

/* 回读本次挂载锁定的选项（show_options）*/
int ravenfs_bitmap_opts_get(struct super_block *sb, struct ravenfs_bitmap_opts *out)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;

	if (!m || !out)
		return -EINVAL;
	*out = m->opts;
	return 0;
}

/* ═══════════════════════ 13 公共 API（语义/签名不变）═══════════════════════ */

u64 ravenfs_block_alloc(struct super_block *sb, u64 count, u32 align_log2)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;
	u64 hit;

	if (!m || !count)
		return (u64)PTR_INVALID;
	/* 旧口径夹到 ≤16；核头第三参 = 对齐**指数**（起始位 2^align）*/
	hit = rfs_bm_block_alloc_hard(&m->bm, count, min_t(u32, align_log2, 16));
	return (hit == RFS_BM_NO_HIT) ? (u64)PTR_INVALID : hit;
}

u64 ravenfs_block_alloc_soft(struct super_block *sb, u64 count, u64 *remain)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;
	u64 hit, gap = count;

	if (!m || !count) {
		if (remain)
			*remain = count;
		return (u64)PTR_INVALID;
	}
	hit = rfs_bm_block_alloc_soft(&m->bm, count, &gap);
	if (remain)
		*remain = gap;
	return (hit == RFS_BM_NO_HIT) ? (u64)PTR_INVALID : hit;
}

u64 ravenfs_block_occupy_soft(struct super_block *sb, u64 base, u64 count, u64 *rest)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;
	u64 gap = count, ret;

	if (!m || !count) {
		if (rest)
			*rest = count;
		return (u64)PTR_INVALID;
	}
	ret = rfs_bm_block_occupy_soft(&m->bm, base, count, &gap);
	if (rest)
		*rest = gap;
	return (ret == RFS_BM_NO_HIT) ? (u64)PTR_INVALID : ret;
}

int ravenfs_block_test_interval(struct super_block *sb, u64 base, u64 len, bool val)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;

	if (!m)
		return -EINVAL;
	return rfs_bm_block_test_interval(&m->bm, base, len, val);
}

void ravenfs_block_free(struct super_block *sb, u64 blk, u64 count)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;

	if (!m || !count)
		return;
	rfs_bm_block_free(&m->bm, blk, count);
}

bool ravenfs_block_test(struct super_block *sb, u64 blk)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;

	if (!m)
		return false;
	return rfs_bm_block_test(&m->bm, blk);
}

u64 ravenfs_inode_alloc(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;
	u64 hit;

	if (!m)
		return (u64)PTR_INVALID;
	hit = rfs_bm_inode_alloc(&m->bm);
	return (hit == RFS_BM_NO_HIT) ? (u64)PTR_INVALID : hit;
}

void ravenfs_inode_free(struct super_block *sb, u64 inum)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;

	if (!m)
		return;
	rfs_bm_inode_free(&m->bm, inum);
}

bool ravenfs_inode_test(struct super_block *sb, u64 inum)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;

	if (!m)
		return false;
	return rfs_bm_inode_test(&m->bm, inum);
}

u64 ravenfs_block_free_total(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;

	if (!m)
		return 0;
	return rfs_bm_block_free_total(&m->bm);
}

u64 ravenfs_inode_free_total(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;

	if (!m)
		return 0;
	return rfs_bm_inode_free_total(&m->bm);
}

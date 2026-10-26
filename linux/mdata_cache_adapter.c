// SPDX-License-Identifier: GPL-2.0
/*
 * mdata_cache_adapter.c —— RavenFS mdata 8K 节点缓存的 FS 环境装配层
 *
 * 核心（`linux/mdata_cache_core.c`）只认识四个注入面：mem / pool / io / stat。
 * 本文件只做 FS 环境装配，运行期调用面直接走核心（见头文件分层说明）：
 *
 *   · mem  = kmalloc/kfree（核心对表结构显式 memset，零初始化契约由核心兜底）；
 *   · pool = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, order=1) + page_address；
 *            淘汰/卸载走 __free_pages（8K = 1 个 B+tree 节点 = 2×4K 连续块）；
 *   · io   = sb_bread + ravenfs_dev_blk + mark_buffer_dirty/sync_dirty_buffer；
 *            nblk 恒 1/2（尾部节点截断），并按旧实现打 io_{read,write}_{ops,bytes}；
 *   · stat = RAVENFS_MD_* → sbi->stats.md_*（与 god mdp_stat 全量同序对位）。
 *
 * 缓存本体（struct ravenfs_mdata_cache）由 kzalloc 取得并挂在 sbi->mcache；
 * attach 失败自清理，detach = 核心 fini（flush_all → DFS 释放表 → 释放 shards）
 * + kfree(self) + sbi->mcache = NULL。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/timekeeping.h>

#include "ravenfs.h"
#include "ravenfs_inode_session.h"
#include "ravenfs_mdata_cache_adapter.h"

/* ══════════════════════════ 注入面：mem ══════════════════════════ */

static void *ravenfs_md_mem_alloc(void *ctx, u64 size)
{
	(void)ctx;
	return kmalloc((size_t)size, GFP_KERNEL);
}

static void ravenfs_md_mem_free(void *ctx, void *p)
{
	(void)ctx;
	kfree(p);
}

/* ══════════════════════════ 注入面：pool（8K / order=1）══════════ */

static void *ravenfs_md_pool_alloc(void *ctx, u64 bytes)
{
	struct page *page;
	unsigned int order = get_order((unsigned int)bytes);

	(void)ctx;
	page = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, order);
	if (!page)
		return NULL;
	return page_address(page);	/* 直映射（物理内存宏访问）*/
}

static void ravenfs_md_pool_free(void *ctx, void *addr, u64 bytes)
{
	unsigned int order = get_order((unsigned int)bytes);

	(void)ctx;
	if (addr)
		__free_pages(virt_to_page(addr), order);
}

/* ══════════════════════════ 注入面：io ══════════════════════════
 * 块号单位 = 全局 FS_block；读须读满 nblk×4K，写「返回即可见」。
 * 尾部节点 nblk = 1 时余下 4K 由 pool 的 __GFP_ZERO 保证为零。
 */

static int ravenfs_md_io_read(void *ctx, u64 fs_blk, u64 nblk, void *buf)
{
	struct super_block *sb = ctx;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 i;

	for (i = 0; i < nblk; i++) {
		struct buffer_head *bh =
			sb_bread(sb, ravenfs_dev_blk(sbi, fs_blk + i));

		if (!bh) {
			RFS_STAT_INC(sb, err_meta_io);
			return -EIO;
		}
		memcpy((char *)buf + i * RAVENFS_BLOCK_SIZE, bh->b_data,
		       RAVENFS_BLOCK_SIZE);
		brelse(bh);
		RFS_STAT_INC(sb, io_read_ops);		/* mdata 节点盘读 */
		RFS_STAT_ADD(sb, io_read_bytes, RAVENFS_BLOCK_SIZE);
	}
	return 0;
}

static int ravenfs_md_io_write(void *ctx, u64 fs_blk, u64 nblk,
			       const void *buf)
{
	struct super_block *sb = ctx;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 i;

	for (i = 0; i < nblk; i++) {
		struct buffer_head *bh =
			sb_bread(sb, ravenfs_dev_blk(sbi, fs_blk + i));

		if (!bh)
			return -EIO;
		memcpy(bh->b_data, (const char *)buf + i * RAVENFS_BLOCK_SIZE,
		       RAVENFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
		RFS_STAT_INC(sb, io_write_ops);		/* mdata 节点写回 */
		RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
	}
	return 0;
}

/* ══════════════════════════ 注入面：stat ══════════════════════════
 * 核心 md_* 字段（与 god mdp_stat 全量同序对位）在 struct ravenfs_sb_stats
 * 中逐一存在。
 */

static void ravenfs_md_stat_inc(void *ctx, u32 field, u64 delta)
{
	struct super_block *sb = ctx;

	switch (field) {
	/* A 装配 / 生命周期 */
	case RAVENFS_MD_INIT_OK:
		RFS_STAT_ADD(sb, md_init_ok, delta);
		break;
	case RAVENFS_MD_INIT_FAIL:
		RFS_STAT_ADD(sb, md_init_fail, delta);
		break;
	case RAVENFS_MD_SLICES:
		RFS_STAT_ADD(sb, md_slices, delta);
		break;
	case RAVENFS_MD_INIT_CTRL_ARRAY:
		RFS_STAT_ADD(sb, md_init_ctrl_array, delta);
		break;
	case RAVENFS_MD_INIT_RADIX:
		RFS_STAT_ADD(sb, md_init_radix, delta);
		break;
	/* B 会话 */
	case RAVENFS_MD_GET_HIT:
		RFS_STAT_ADD(sb, md_hit, delta);
		break;
	case RAVENFS_MD_GET_MISS:
		RFS_STAT_ADD(sb, md_miss, delta);
		break;
	case RAVENFS_MD_NODE_LOAD:
		RFS_STAT_ADD(sb, md_node_load, delta);
		break;
	case RAVENFS_MD_RETRY:
		RFS_STAT_ADD(sb, md_retry, delta);
		break;
	case RAVENFS_MD_PUT:
		RFS_STAT_ADD(sb, md_put, delta);
		break;
	case RAVENFS_MD_DIRTY:
		RFS_STAT_ADD(sb, md_dirty, delta);
		break;
	case RAVENFS_MD_ERR:
		RFS_STAT_ADD(sb, md_err, delta);
		break;
	/* C 表结构 */
	case RAVENFS_MD_TABLE_ALLOC_INTERNAL:
		RFS_STAT_ADD(sb, md_table_alloc_internal, delta);
		break;
	case RAVENFS_MD_TABLE_ALLOC_LEAF:
		RFS_STAT_ADD(sb, md_table_alloc_leaf, delta);
		break;
	case RAVENFS_MD_TABLE_FREE:
		RFS_STAT_ADD(sb, md_table_free, delta);
		break;
	/* D 淘汰 / 回写 */
	case RAVENFS_MD_REAP:
		RFS_STAT_ADD(sb, md_reap, delta);
		break;
	case RAVENFS_MD_REAP_STRONG:
		RFS_STAT_ADD(sb, md_reap_strong, delta);
		break;
	case RAVENFS_MD_NODE_WB:
		RFS_STAT_ADD(sb, md_node_wb, delta);
		break;
	case RAVENFS_MD_FLUSH_WB:
		RFS_STAT_ADD(sb, md_flush_wb, delta);
		break;
	case RAVENFS_MD_FLUSH_DROP:
		RFS_STAT_ADD(sb, md_flush_drop, delta);
		break;
	/* E 驻留量 gauge */
	case RAVENFS_MD_RESIDENT_ADD_BYTES:
		RFS_STAT_ADD(sb, md_resident_add_bytes, delta);
		break;
	case RAVENFS_MD_RESIDENT_DEL_BYTES:
		RFS_STAT_ADD(sb, md_resident_del_bytes, delta);
		break;
	case RAVENFS_MD_TABLE_ADD_BYTES:
		RFS_STAT_ADD(sb, md_table_add_bytes, delta);
		break;
	case RAVENFS_MD_TABLE_DEL_BYTES:
		RFS_STAT_ADD(sb, md_table_del_bytes, delta);
		break;
	/* ko 扩展 */
	case RAVENFS_MD_DROP:
		RFS_STAT_ADD(sb, md_drop, delta);
		break;
	default:
		break;
	}
}

/* ══════════════════════════ 生命周期 ══════════════════════════ */

int ravenfs_mdata_cache_attach(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi;
	struct ravenfs_mdata_cache *c;
	struct ravenfs_mdata_cfg cfg = { };
	int rc;

	if (!sb || !sb->s_fs_info)
		return -EIO;
	sbi = sb->s_fs_info;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	cfg.total_blocks = sbi->total_blocks;
	cfg.mem.alloc = ravenfs_md_mem_alloc;
	cfg.mem.free = ravenfs_md_mem_free;
	cfg.mem.ctx = sb;
	cfg.pool.alloc = ravenfs_md_pool_alloc;
	cfg.pool.free = ravenfs_md_pool_free;
	cfg.pool.ctx = sb;
	cfg.io.read_block = ravenfs_md_io_read;
	cfg.io.write_block = ravenfs_md_io_write;
	cfg.io.ctx = sb;
	cfg.stat.inc = ravenfs_md_stat_inc;
	cfg.stat.ctx = sb;

	rc = ravenfs_mdata_cache_init(c, &cfg);
	if (rc) {
		kfree(c);
		return rc;
	}
	sbi->mcache = c;
	pr_info("ravenfs: mdata cache: node=8K(2 blk) mode=%s bits=%u shards=%u n_nodes=%llu\n",
		c->mode == RAVENFS_MDATA_MODE_CTRL_ARRAY ? "ctrl_array" : "radix",
		c->bits_len, c->n_slices, (unsigned long long)c->n_nodes);
	return 0;
}

void ravenfs_mdata_cache_detach(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct ravenfs_mdata_cache *c = sbi ? sbi->mcache : NULL;

	if (!c)
		return;
	if (!ravenfs_sb_writable(sb) || ravenfs_is_shutdown(sb))
		ravenfs_mdata_discard_all(c);	/* ro / 停机：丢弃脏行，绝不写设备 */
	ravenfs_mdata_cache_fini(c);	/* flush_all（已无脏行）+ DFS 释放表 + shards */
	kfree(c);
	sbi->mcache = NULL;
}

/* ══════════════════════════ 统一节点读入口 ══════════════════════════
 * 所有 B+tree 节点读一律直接走核心 8K 缓存；sb 仅用于取缓存 self。
 */
int ravenfs_read_node(struct super_block *sb, u64 blk, void *buf)
{
	return ravenfs_mdata_read(ravenfs_mdata_cache_of(sb), blk, buf);
}

/* ══════════════════════════ 回刷 driver 形状适配 ══════════════════════════
 * cache_ttl 的 `ravenfs_wb_ops` 以 sb 为参数；这里只做 sb → self 的取值与
 * ro/停机闸（不承载任何缓存算法）。周期动作本体仍直接调用核心。
 */
static int ravenfs_mdata_sb_wb_dirty(struct super_block *sb, u64 age_cutoff_ns,
				     bool ignore_age)
{
	struct ravenfs_mdata_cache *c = ravenfs_mdata_cache_of(sb);

	if (!c || !ravenfs_sb_writable(sb) || ravenfs_is_shutdown(sb))
		return 0;
	return ravenfs_mdata_wb_dirty(c, age_cutoff_ns, ignore_age);
}

static long ravenfs_mdata_sb_reap_expired(struct super_block *sb, u64 cutoff_ns)
{
	struct ravenfs_mdata_cache *c = ravenfs_mdata_cache_of(sb);

	if (!c || !ravenfs_sb_writable(sb) || ravenfs_is_shutdown(sb))
		return 0;
	return ravenfs_mdata_reap_expired(c, cutoff_ns);
}

/* v0.2.3：TTL 空闲触发的强淘汰后端。
 * 与用户 ioctl 共用 cache->squeeze_lock：TTL 用 trylock，抢不到返回 -EAGAIN，
 * 由调用方下轮再试（不阻塞、不做无用功）。*/
static int ravenfs_mdata_sb_squeeze(struct super_block *sb, u32 gear,
				    u64 ttl_cutoff_ns)
{
	struct ravenfs_mdata_cache *c = ravenfs_mdata_cache_of(sb);
	struct ravenfs_mdata_squeeze_res res;
	int ret;

	if (!c || !ravenfs_sb_writable(sb) || ravenfs_is_shutdown(sb))
		return 0;
	if (!RFS_MUTEX_TRYLOCK(&c->squeeze_lock))
		return -EAGAIN;

	ret = ravenfs_mdata_reap_expired_strong(c, gear, ttl_cutoff_ns, &res);
	RFS_MUTEX_UNLOCK(&c->squeeze_lock);
	return ret;
}

const struct ravenfs_wb_ops ravenfs_wb_mdata_ops = {
	.wb_dirty = ravenfs_mdata_sb_wb_dirty,
	.reap     = ravenfs_mdata_sb_reap_expired,
	.squeeze  = ravenfs_mdata_sb_squeeze,
};

// SPDX-License-Identifier: GPL-2.0
/*
 * cache_ttl.c —— RavenFS ko：**自写 TTL 淘汰 + 周期回写**驱动（2026-09-27 扩）
 *
 * 为什么自写：token 经济性 / 路线可靠性 —— 不指望内核压力回收做"冷集淘汰"，
 * 自己掌握 TTL 语义（**压力 shrinker 已移除**，TTL 为唯一冷工作集淘汰路径）。
 *
 * 每轮（delayed_work）三趟，I/O 全在锁外：
 *   趟1 淘汰  `ops->reap(sb, now - ttl)`              —— 超时冷行：脏原地回写 → 释放
 *   趟2 回写  `ops->wb_dirty(sb, now - wb_age, 0)`    —— 超龄脏行：copy-out 回写
 *   趟3 位图  `ravenfs_bitmap_flush(sb)` —— **每 `bitmap_sync_seconds`(30s)** 闸门到点才
 *             把两组位图**全片**同步一遍（锁内快照 → 锁外块 I/O）；无脏标记。
 *
 * 后端（架构无关 driver，仅缓存行）：`ravenfs_wb_inode_ops` / `ravenfs_wb_mdata_ops`。
 *   位图（bitmap.c）= 全量常驻内存分片，**非缓存行**、不接入本 driver：自有片级全片同步
 *   （组 wb_lock + 锁内快照/锁外 I/O；见 statemachine 文档 §6 / bitmap.c 首注）。
 *
 * 参数：
 *   ravenfs_ttl_seconds          默认 2   （0 = 关闭 TTL 淘汰）
 *   ravenfs_reap_ms              默认 4000（扫描周期；2026-09-27 由 1s 改 4s）
 *   ravenfs_wb_age_seconds       默认 60  （脏行回写 aging；0 = 关闭周期回写）
 *   ravenfs_bitmap_sync_seconds  默认 30  （位图全片同步闸门；0 = 关闭）
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/timekeeping.h>
#include <linux/kernel.h>

#include "ravenfs.h"
#include "ravenfs_cache.h"
#include "ravenfs_allocator_adapter.h"

static unsigned int ttl_seconds = 2;
module_param(ttl_seconds, uint, 0644);
MODULE_PARM_DESC(ttl_seconds, "缓存行 TTL（秒）；0 = 关闭 TTL 淘汰（对齐 god 默认 2s）");

static unsigned int reap_ms = 4000;
module_param(reap_ms, uint, 0644);
MODULE_PARM_DESC(reap_ms, "TTL 扫描周期（毫秒）");

static unsigned int wb_age_seconds = 60;
module_param(wb_age_seconds, uint, 0644);
MODULE_PARM_DESC(wb_age_seconds, "脏行回写 aging（秒）；0 = 关闭周期回写");

static unsigned int bitmap_sync_seconds = 30;
module_param(bitmap_sync_seconds, uint, 0644);
MODULE_PARM_DESC(bitmap_sync_seconds, "位图全片同步闸门（秒）；0 = 关闭周期同步");

static const struct ravenfs_wb_ops *const ravenfs_wb_backends[] = {
	&ravenfs_wb_inode_ops,
	&ravenfs_wb_mdata_ops,
};

struct ravenfs_ttl {
	struct delayed_work	work;
	struct super_block	*sb;
	unsigned long		reaped_total;
	unsigned long		wb_total;
	unsigned long		bm_total;
	unsigned long		rounds;
	u64			bm_last;	/* 上次位图全片同步（ktime_ns） */
};

static void ravenfs_ttl_work(struct work_struct *w)
{
	struct ravenfs_ttl *t = container_of(to_delayed_work(w), struct ravenfs_ttl, work);
	struct super_block *sb = t->sb;
	u64 now, ttl_cutoff, wb_cutoff;
	long freed = 0, wb = 0, bm = 0;
	unsigned int i;

	if (!sb->s_fs_info || (sb->s_flags & SB_RDONLY) || ravenfs_is_shutdown(sb)) {
		schedule_delayed_work(&t->work, msecs_to_jiffies(reap_ms ? reap_ms : 4000));
		return;
	}

	t->rounds++;
	now = ktime_get_ns();

	/* 趟1：淘汰超时冷行 */
	if (ttl_seconds) {
		ttl_cutoff = now - (u64)ttl_seconds * NSEC_PER_SEC;
		for (i = 0; i < ARRAY_SIZE(ravenfs_wb_backends); i++)
			freed += ravenfs_wb_backends[i]->reap(sb, ttl_cutoff);
	}

	/* 趟2：超龄脏行周期回写 */
	if (wb_age_seconds) {
		wb_cutoff = now - (u64)wb_age_seconds * NSEC_PER_SEC;
		for (i = 0; i < ARRAY_SIZE(ravenfs_wb_backends); i++)
			if (ravenfs_wb_backends[i]->wb_dirty(sb, wb_cutoff, false))
				wb = -1;
		if (wb)
			ravenfs_force_shutdown(sb, "ttl: periodic writeback failed");
		if (wb)
			RFS_STAT_INC(sb, ttl_round_errs);	/* 后台刷盘失败累积 */
	}

	/* 趟3：分配器同步（闸门 `bitmap_sync_seconds`；无脏标记，整轮全同步）
	 * `ravenfs_allocator_flush` = 树快照 → 位图 words → 写回设备（落盘一步到位）。*/
	if (bitmap_sync_seconds &&
	    now - t->bm_last >= (u64)bitmap_sync_seconds * NSEC_PER_SEC) {
		if (ravenfs_allocator_flush(sb))
			bm = -1;
		t->bm_last = now;
		if (bm)
			ravenfs_force_shutdown(sb, "ttl: bitmap flush failed");
		if (bm)
			RFS_STAT_INC(sb, ttl_round_errs);
	}

	if (freed)
		t->reaped_total += freed;
	if (wb)
		t->wb_total++;
	if (bm)
		t->bm_total++;
	if (freed || wb || bm)
		pr_info_ratelimited("ravenfs: ttl: reap=%ld(累计 %lu) wb_err=%ld(累计 %lu) bm_err=%ld(累计 %lu) (ttl=%us wb_age=%us bm_sync=%us round=%lu)\n",
				    freed, t->reaped_total, wb, t->wb_total, bm, t->bm_total,
				    ttl_seconds, wb_age_seconds, bitmap_sync_seconds, t->rounds);

	RFS_STAT_INC(sb, ttl_rounds);	/* 后台还活着吗（数不涨 = work 卡死） */
	schedule_delayed_work(&t->work, msecs_to_jiffies(reap_ms ? reap_ms : 4000));
}

int ravenfs_ttl_start(struct super_block *sb)
{
	struct ravenfs_ttl *t = kzalloc(sizeof(*t), GFP_KERNEL);

	if (!t)
		return -ENOMEM;
	t->sb = sb;
	t->bm_last = ktime_get_ns();	/* 位图首个同步闸门从启动起算 30s */
	INIT_DELAYED_WORK(&t->work, ravenfs_ttl_work);
	((struct ravenfs_sb_info *)sb->s_fs_info)->ttl = t;
	schedule_delayed_work(&t->work, msecs_to_jiffies(reap_ms ? reap_ms : 4000));
	pr_info("ravenfs: ttl evictor+wb started (ttl=%us, wb_age=%us, bm_sync=%us, period=%ums)\n",
		ttl_seconds, wb_age_seconds, bitmap_sync_seconds, reap_ms);
	return 0;
}

void ravenfs_ttl_stop(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_ttl *t = sbi ? sbi->ttl : NULL;

	if (!t)
		return;
	cancel_delayed_work_sync(&t->work);
	pr_info("ravenfs: ttl evictor+wb stopped (reaped_total=%lu)\n", t->reaped_total);
	sbi->ttl = NULL;
	kfree(t);
}

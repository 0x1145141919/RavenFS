// SPDX-License-Identifier: GPL-2.0
/*
 * cache_ttl.c —— RavenFS ko：**自写 TTL 淘汰器**（用户裁定 2026-09-26 晚）
 *
 * 为什么自写：token 经济性 / 路线可靠性 —— 不指望内核压力回收做"冷集淘汰"，
 * 自己掌握 TTL 语义（**压力 shrinker 已移除**，TTL 为唯一冷工作集淘汰路径）。
 *
 * 参数**暂时对齐用户态上帝类**（`inode_cache.cpp`）：
 *   · TTL 默认 **2s**（god：`ttl_bare_seconds==0 → 2s`）；
 *   · 每行 `stamp` 在**每次 get/put 访问**时刷新（god：`accss_stamp`）；
 *   · 淘汰条件 = `refcount==1（无 session 持有） ∧ !dirty ∧ now-stamp > ttl`（god：`accessor==0 && 超时`）；
 *   · 周期扫描（god 是专用线程周期调 `reap_expired`；内核侧用 **delayed_work**，等价策略、不另起线程）。
 *
 * 参数：
 *   ravenfs_ttl_seconds  默认 2（0 = 关闭 TTL 淘汰）
 *   ravenfs_reap_ms      默认 1000（扫描周期）
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/timekeeping.h>
#include <linux/kernel.h>

#include "ravenfs.h"

static unsigned int ttl_seconds = 2;
module_param(ttl_seconds, uint, 0644);
MODULE_PARM_DESC(ttl_seconds, "inode/mdata 缓存行 TTL（秒）；0 = 关闭（对齐 god 默认 2s）");

static unsigned int reap_ms = 1000;
module_param(reap_ms, uint, 0644);
MODULE_PARM_DESC(reap_ms, "TTL 扫描周期（毫秒）");

struct ravenfs_ttl {
	struct delayed_work	work;
	struct super_block	*sb;
	unsigned long		reaped_total;
	unsigned long		rounds;
};

static void ravenfs_ttl_work(struct work_struct *w)
{
	struct ravenfs_ttl *t = container_of(to_delayed_work(w), struct ravenfs_ttl, work);
	struct super_block *sb = t->sb;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 cutoff;
	long freed = 0, n;

	if (!sbi)
		return;

	t->rounds++;
	if (ttl_seconds) {			/* 0 = 关闭 TTL 淘汰 */
		cutoff = ktime_get_ns() - (u64)ttl_seconds * NSEC_PER_SEC;
		if (sbi->icache)
			freed += ravenfs_cache_reap_expired(sbi->icache, cutoff);
		if (sbi->mcache)
			freed += ravenfs_mdata_reap_expired(sbi->mcache, cutoff);
	}

	if (freed) {
		t->reaped_total += freed;
		n = t->reaped_total;
		pr_info_ratelimited("ravenfs: ttl reap: freed=%ld total=%ld (ttl=%us round=%lu)\n",
				    freed, n, ttl_seconds, t->rounds);
	}

	schedule_delayed_work(&t->work, msecs_to_jiffies(reap_ms ? reap_ms : 1000));
}

int ravenfs_ttl_start(struct super_block *sb)
{
	struct ravenfs_ttl *t = kzalloc(sizeof(*t), GFP_KERNEL);

	if (!t)
		return -ENOMEM;
	t->sb = sb;
	INIT_DELAYED_WORK(&t->work, ravenfs_ttl_work);
	((struct ravenfs_sb_info *)sb->s_fs_info)->ttl = t;
	schedule_delayed_work(&t->work, msecs_to_jiffies(reap_ms ? reap_ms : 1000));
	pr_info("ravenfs: ttl evictor started (ttl=%us, period=%ums)\n", ttl_seconds, reap_ms);
	return 0;
}

void ravenfs_ttl_stop(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_ttl *t = sbi ? sbi->ttl : NULL;

	if (!t)
		return;
	cancel_delayed_work_sync(&t->work);
	pr_info("ravenfs: ttl evictor stopped (reaped_total=%lu)\n", t->reaped_total);
	sbi->ttl = NULL;
	kfree(t);
}

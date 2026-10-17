/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_mdata_cache_adapter.h —— mdata 8K 节点缓存的 FS 环境装配层
 *
 * 平台中立的缓存算法/生命周期在 `ravenfs_mdata_cache_core.h` /
 * `linux/mdata_cache_core.c`；本层只做**一次性装配**：
 *   · 分配/释放核心 self（struct ravenfs_mdata_cache）；
 *   · 把内核 FS 环境注入核心：mem = kmalloc/kfree，pool = 伙伴系统 8K(order=1)
 *     零页，io = sb_bread + ravenfs_dev_blk + mark_buffer_dirty/sync_dirty_buffer，
 *     stat = RAVENFS_MD_* → sbi->stats.md_*（与 god mdp_stat 全量对位）；
 *   · mount / put_super 的 attach / detach 生命周期（核心 init / fini 的 FS 包装）。
 *
 * 运行期调用面**不在本头**：所有会话 / 回写 / 淘汰 / 标脏一律直接调用核心 API
 * （get/put/drop/read、wb_dirty/writeback_all/wb_node、reap_expired、
 *   flush_all/discard_all、mark_dirty），
 * 缓存对象经 `ravenfs_mdata_cache_of(sb)`（ravenfs.h）取得 = `sbi->mcache`。
 * 旧 sb 版 `ravenfs_mdata_adapter_*` 调用面已全面退役，勿再新增同类薄壳。
 *
 * 本头只给内核 FS 侧使用；平台中立核心/用户态测试不依赖它。
 */
#ifndef _RAVENFS_MDATA_CACHE_ADAPTER_H
#define _RAVENFS_MDATA_CACHE_ADAPTER_H

#include "ravenfs_mdata_cache_core.h"

struct super_block;

/* ── 生命周期（super.c mount / put_super）────────────────────────────── */
int  ravenfs_mdata_cache_attach(struct super_block *sb);
void ravenfs_mdata_cache_detach(struct super_block *sb);

#endif /* _RAVENFS_MDATA_CACHE_ADAPTER_H */

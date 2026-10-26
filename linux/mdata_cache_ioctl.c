// SPDX-License-Identifier: GPL-2.0
/*
 * mdata_cache_ioctl.c —— RavenFS mdata 缓存管理 ioctl 处理器
 *
 * 唯一命令：RAVENFS_IOC_CACHE_SQUEEZE —— 手动 / 守护进程触发 FS 级缓存强淘汰。
 *
 * **契约真值**：`Docs/linux/ravenfs-linux-ko-mdata-cache-ioctl-spec.md`（v1）。
 *   本文件只做 UAPI 边界守卫；语义/错误码/回填纪律以规格书为准。
 *
 * 分层纪律（不在本文件落缓存算法）：
 *   UAPI（ravenfs_mdata_ioctl.h）→ 本处理器（权限/参数/错误码/copy）
 *   → 核心 `ravenfs_mdata_reap_expired_strong(sbi->mcache, ...)`。
 * 本文件只做 UAPI 边界守卫与核心调用的 FS 级闸（ro / shutdown / 无缓存）；
 * 几何/锁序/IO 全部归 core。
 *
 * 语义（详见 UAPI 头）：
 *   · PRESSURE(0)：无视 TTL 回收全部 ref==0 就绪行（预留 shrinker/压力触发）；
 *   · LOWLOAD(1) ：只回收 acc_stamp < ttl_ns 的冷行；ttl_ns==0 不做 TTL 过滤。
 *   两档都可能回写脏行 → 只读挂载一律 -EROFS（判定先于实际动作，避免半途）。
 *
 * 结果回填：适配层返回非 0（如脏行写回失败 -EIO）时，仍把 out 中的 partial
 *   计数写回用户结构；仅 copy_to_user 本身失败时以 -EFAULT 覆盖返回值。
 *
 * 接线（由主审应用，本文件不改 super.c / Makefile）：
 *   · super.c 两个 fops：.unlocked_ioctl = ravenfs_mdata_ioctl;
 *                        .compat_ioctl   = compat_ptr_ioctl;
 *   · Makefile：ravenfs-y += mdata_cache_ioctl.o
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/errno.h>

#include "ravenfs.h"
#include "ravenfs_mdata_ioctl.h"
#include "ravenfs_mdata_cache_core.h"

long ravenfs_mdata_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode;
	struct super_block *sb;
	struct ravenfs_sb_info *sbi;
	struct ravenfs_mdata_cache *mc;
	struct ravenfs_cache_squeeze_arg uarg;
	struct ravenfs_mdata_squeeze_res res = { };
	int ret;

	inode = file_inode(file);
	if (!inode || !inode->i_sb)
		return -EINVAL;
	sb = inode->i_sb;
	sbi = sb->s_fs_info;
	/* 卸载窗口：s_fs_info 已释放，不能再碰缓存；按设备已消失返回 -EIO。*/
	if (!sbi)
		return -EIO;

	/* 低频维护操作，只允许特权用户（同 mount/remount 级别）。*/
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	/* R-LM-09：force-shutdown 后所有入口拒绝服务，避免在坏盘上继续写。*/
	if (ravenfs_is_shutdown(sb))
		return -EIO;

	if (cmd != RAVENFS_IOC_CACHE_SQUEEZE)
		return -ENOTTY;

	if (copy_from_user(&uarg, (void __user *)arg, sizeof(uarg)))
		return -EFAULT;

	/* gear 仅两档；flags 为未来扩展预留，当前必须 0（否则行为不明确）。*/
	if (uarg.gear > RAVENFS_CACHE_SQUEEZE_LOWLOAD || uarg.flags)
		return -EINVAL;

	/* 两种 gear 都可能回写脏行（脏行先落盘再释放）→ 只读挂载不可执行。
	 * 预判而非等核心撞错：ro 下写回必然失败，提前返回语义更清晰。
	 */
	if (sb_rdonly(sb))
		return -EROFS;

	/* 缓存未装配（卸载窗口 / mount 失败）→ 无对象可淘汰。*/
	mc = sbi->mcache;
	if (!mc)
		return -EIO;

	/* ttl_ns 原样下传：PRESSURE 由核心忽略；LOWLOAD 的 0 = 无 TTL 过滤。
	 * v0.2.3：与 TTL 空闲强淘汰串行化（TTL 侧用 trylock，抢不到跳过）。*/
	RFS_MUTEX_LOCK(&mc->squeeze_lock);
	ret = ravenfs_mdata_reap_expired_strong(mc, (u32)uarg.gear, uarg.ttl_ns,
						&res);
	RFS_MUTEX_UNLOCK(&mc->squeeze_lock);

	/* 无论 ret 是否 0 都回填结果（partial 可观测：例如部分脏行写回失败）。*/
	uarg.rows_reaped = res.rows_reaped;
	uarg.tables_freed = res.tables_freed;
	uarg.table_bytes_freed = res.table_bytes_freed;
	uarg.write_errors = res.write_errors;

	if (copy_to_user((void __user *)arg, &uarg, sizeof(uarg)))
		return -EFAULT;

	pr_debug_ratelimited("ravenfs: cache squeeze gear=%u ttl=%llu -> rows=%llu tables=%llu bytes=%llu wb_err=%llu ret=%d\n",
			     (u32)uarg.gear, uarg.ttl_ns, res.rows_reaped,
			     res.tables_freed, res.table_bytes_freed,
			     res.write_errors, ret);

	return ret;
}

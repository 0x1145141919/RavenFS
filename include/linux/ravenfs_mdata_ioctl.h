/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_mdata_ioctl.h —— RavenFS mdata 缓存管理 ioctl UAPI
 *
 * 用途：把「FS 级缓存强淘汰」（核心 `ravenfs_mdata_reap_expired_strong`，
 *   ioctl 处理器直接以 `sbi->mcache` 调用）暴露给用户态。
 *
 * **契约真值**：`Docs/linux/ravenfs-linux-ko-mdata-cache-ioctl-spec.md`（v1）。
 *   本头只固化 UAPI 形状；语义/错误码/并发/回填纪律以规格书为准。
 *
 * 两档语义（⇄ enum ravenfs_mdata_squeeze_gear）：
 *   · PRESSURE（0）：内存压力语义 —— 无视 TTL，回收所有 ref==0 的就绪行；
 *     预留为未来 shrinker / 压力守护进程的触发口，也允许 root 手动触发。
 *   · LOWLOAD （1）：低载主动语义 —— 由用户态 / 低载守护进程在系统空闲时调用，
 *     只回收 acc_stamp < ttl_ns 的冷行（热行保留）；ttl_ns == 0 表示不做 TTL
 *     过滤（等价 PRESSURE 的行选择，但仍走 LOWLOAD 档）。
 *
 * 调用纪律：
 *   · 仅 CAP_SYS_ADMIN（root）可调用；文件系统已 force-shutdown 时返回 -EIO；
 *   · 两种 gear 都可能回写脏行，故只读挂载（SB_RDONLY）返回 -EROFS；
 *   · ioctl 是低频重操作（含 DFS + 可能的元数据回写），勿放入周期热路径；
 *   · 失败（如部分脏行写回失败返回 -EIO）时，**adapter 已执行**的路径仍会回填
 *     输出字段，用户可据此观察 partial 结果；权限/参数/ro/shutdown/copy_from_user
 *     等前置错误不回填（详见规格书 §6）。
 *
 * 布局固定：字段全为 __u32/__u64 且自然对齐；32/64 位用户态同一布局，
 *   32 位兼容进程可直接走 compat_ptr_ioctl（无指针字段）。
 */
#ifndef _RAVENFS_MDATA_IOCTL_H
#define _RAVENFS_MDATA_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 强淘汰：输入 gear/flags/ttl_ns，输出四类计数。
 * 方向 _IOWR：内核先读入参，再把结果写回同一结构（失败也尽量回填 partial）。
 */
#define RAVENFS_IOC_CACHE_SQUEEZE \
	_IOWR('R', 0x20, struct ravenfs_cache_squeeze_arg)

/* gear 取值（与内核 enum ravenfs_mdata_squeeze_gear 数值一致）。*/
#define RAVENFS_CACHE_SQUEEZE_PRESSURE	0u	/* 压力档：无视 TTL */
#define RAVENFS_CACHE_SQUEEZE_LOWLOAD	1u	/* 低载档：按 ttl_ns 冷行过滤 */

/**
 * struct ravenfs_cache_squeeze_arg - RAVENFS_IOC_CACHE_SQUEEZE 参数/结果
 * @gear:             入参：RAVENFS_CACHE_SQUEEZE_{PRESSURE,LOWLOAD}，其余 -EINVAL
 * @flags:            入参：必须为 0（预留）；非 0 -EINVAL
 * @ttl_ns:           入参：LOWLOAD 的截止时间戳（CLOCK_MONOTONIC，ns）；
 *                    0 = 不做 TTL 过滤；PRESSURE 忽略此字段
 * @rows_reaped:      出参：成功回收（回到 IDLE）的行数
 * @tables_freed:     出参：摘除的空叶表/内部表/根表数（CTRL_ARRAY 档恒 0）
 * @table_bytes_freed:出参：摘除表结构释放的字节数
 * @write_errors:     出参：脏行回写失败数（>0 时 ioctl 返回 -EIO）
 *
 * 失败时出参仍回填（partial 结果）：仅限 adapter 已执行的路径；权限/参数/ro/
 * shutdown 等前置错误不回填。copy_to_user 失败则整体返回 -EFAULT。
 * **完整契约见 `Docs/linux/ravenfs-linux-ko-mdata-cache-ioctl-spec.md` §6。**
 */
struct ravenfs_cache_squeeze_arg {
	__u32	gear;			/* 0=PRESSURE, 1=LOWLOAD */
	__u32	flags;			/* 必须 0；预留 */
	__u64	ttl_ns;			/* LOWLOAD 的 cutoff；0 = 不做 TTL 过滤 */
	__u64	rows_reaped;
	__u64	tables_freed;
	__u64	table_bytes_freed;
	__u64	write_errors;
};

#ifdef __KERNEL__
struct file;

/*
 * fops 处理器：挂到 ravenfs_dir_operations / ravenfs_file_operations 的
 * .unlocked_ioctl（及 .compat_ioctl = compat_ptr_ioctl）。实现见
 * linux/mdata_cache_ioctl.c。
 *
 * 错误码：-EPERM（非 CAP_SYS_ADMIN）/ -EFAULT（用户内存）/
 *         -EINVAL（gear/flags）/ -EROFS（只读挂载）/ -ENOTTY（未知 cmd）/
 *         -EIO（已 shutdown 或脏行写回失败）。
 */
long ravenfs_mdata_ioctl(struct file *file, unsigned int cmd,
			 unsigned long arg);
#endif /* __KERNEL__ */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_MDATA_IOCTL_H */

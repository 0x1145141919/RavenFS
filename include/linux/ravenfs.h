/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs.h —— RavenFS Linux KO 核心头（全模块共享的最小集）
 *
 * 盘面真值：include/disk_struct_v1.3.h（v1.3，C/C++ 双兼容；内置 __KERNEL__ 分支）
 * 规格：Docs/linux/ravenfs-linux-ko-requirements-spec.md
 *
 * 【M1 范围声明（诚实边界，勿当成品）】
 *   本里程碑只做：模块注册 → bdev 绑定 → SB 读+校验 → 几何推导 → root inode(iget)
 *   → 只读 mount → 空目录 readdir/lookup/getattr/statfs。
 *   **非空目录的 dentry 树遍历未实现** —— lookup/iterate 对非空目录显式返回
 *   -EOPNOTSUPP（拒绝而非静默漏），M2 补。
 *   文件数据路径（a_ops / extent 树）完全未接 —— M2+。
 *
 * ─────────────────────────────────────────────────────────────
 * 【2026-09-28 头文件拆分（债务清算；用户裁：上帝头不可接受）】
 * 原 ravenfs.h 512 行把 15 个子系统的接口平铺一锅炖，维护时无法回答
 * 「这段声明归谁 / 谁依赖谁」。拆分规则：
 *
 *   1. **接口跟着实现文件走** —— 每个 .c 的对外声明进同名 ravenfs_*.h
 *      （如 bitmap.c 的分配器 API → ravenfs_bitmap.h）；
 *   2. 本文件只留「所有模块都要共享」的核心：常量、sbi、inode_info、
 *      几何推导、iget/read_node、停机/flush；
 *   3. 依赖方向单向：子系统头 → include 本文件，核心不回指任何子系统头
 *      （子系统间互用，如 mdata_cache 用 ROW 行机制，由 mdata 头自己
 *      include ravenfs_cache.h 表达）；
 *   4. .c 只 include 核心 + 实际用到的子系统头（IWYU，编译器背书）。
 *
 * 原各段注释（设计推理 / 踩坑 / 用户裁决）随声明原样搬迁，不因拆分丢
 * 对齐材料（DISCIPLINE.md：AI 思维可见性）。新增 .c 缺头由编译器点名。
 */
#ifndef _RAVENFS_H
#define _RAVENFS_H

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/types.h>
#include <linux/xarray.h>
#include <linux/rwsem.h>
#include <linux/refcount.h>
#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/cleanup.h>

/* v1.3 盘面结构（-I$(src)/../include） */
#include <disk_struct_v1.3.h>
#include <magics.h>	/* SUPERBLOCK_MAGIC */

/* al_ 分配器计数器：字段枚举（show 表对位 + 钩子下标） */
#include "ravenfs_allocator_stats.h"

/* ─────────────────────────── 常量 ─────────────────────────── */

/*
 * s_magic（statfs.f_type）—— 与盘上 SB 魔数 **同源**（mkfs 写的 `sb->magic` = SUPERBLOCK_MAGIC）。
 * 不自创 "RAVF"（2026-09-26 深夜，用户裁：对齐 mkfs）。
 */
#define RAVENFS_SUPER_MAGIC	((unsigned long)SUPERBLOCK_MAGIC)	/* "RAVN" = 0x5241564E */

#define RAVENFS_BLOCK_SIZE		4096
#define RAVENFS_BITS_PER_BLOCK		32768ULL	/* 4096 B × 8 */
#define RAVENFS_INODES_PER_BLOCK	16ULL		/* 4096 / 256 */
#define RAVENFS_INODE_SIZE		256ULL

/* E6：s_maxbytes = 1<<52（字节）≤ MAX_LFS_FILESIZE */
#define RAVENFS_MAX_FILESIZE		(1ULL << 52)
/* f_namelen 用盘面头自带的 RAVENFS_MAX_NAME_LEN（= DENTRY_INLINE_NAME_MAX + 512u = 552B，R-SP-01） */

/* B+tree 节点 = 2×4K（dentry 树 / 区段树通用；几何与 mdata 块粒度同源） */
#define RAVENFS_NODE_BYTES 8192

/*
 * ── inode 号契约（ABI，D-10；disk_layout_v1.2_inode.md §7 不变量 31）──
 *
 * `st_ino = inode_idx + 1`（盘格式 v1.2 内恒定）：
 *   · `st_ino` 恒 ≠ 0 → 解「0 = 用户态已删除哨兵」（glibc is_zero_ino 忽略）；
 *   · 无任何保留区（idx 0 与 1..15 及其后全部合法可分配）；
 *   · 同 FS 内 `st_ino` 唯一，配合 `s_dev` 满足 POSIX「(st_dev, st_ino) 唯一标识文件」；
 *   · 上限：inode 数 < 2^32。
 *
 * **运行时路径只认这两个访问器**：VFS 面（`inode->i_ino` / `->i_ino`）是 **ino**，
 * 一切内部数组/缓存/树接口的 inum 类是 **idx**。禁止散落 `±1`——新增代码若需在
 * 两界间穿越，一律经本对宏（grep `->i_ino` 直用应为零，D-10 验收判据）。
 *
 * 注：盘上 SB 的 `root_inode` 是 **idx**（mkfs 局部实现细节，不进入契约）；
 * `ravenfs_iget()` 入参为 idx，内部构造 ino 后交 `iget_locked()`。
 */
#define RFS_INO_TO_IDX(ino)	((u64)(ino) - 1)
#define RFS_IDX_TO_INO(idx)	((u64)(idx) + 1)

/* fast symlink target 内联上限（不变量 22：size ≤ 160 = 内联态）；
 * == REG 内联上限，便于三态判据复用。 */
#define RAVENFS_SYMLINK_INLINE_MAX	160U
/* slow symlink target 总上限（单块，对齐 ext2 sb_blocksize 口径） */
#define RAVENFS_SYMLINK_MAX		4096U

/* ─────────────── 前向声明（定义在各 ravenfs_*.h） ─────────────── */

struct ravenfs_cache;		/* inode_cache.c：磁盘 inode 行缓存（§4.11.1） */
struct ravenfs_row;
struct ravenfs_shard;
struct ravenfs_session;
struct ravenfs_ttl;		/* cache_ttl.c：自写 TTL 淘汰器，不透明句柄 */
struct rfs_allocator;	/* include/linux/ravenfs_allocator_top.h：运行时多分片主从红黑树分配器 */
struct ravenfs_mdata_cache;	/* mdata_cache.c：元数据节点缓存 */
struct ravenfs_mrow;
struct ravenfs_mdata_shard;
struct ravenfs_msession;

/* ─────────────────────────── 内存结构 ─────────────────────────── */

/*
 * ── superblock 级性能计数器（2026-09-28 部署；设计见
 *    `Docs/linux/ravenfs-linux-ko-sb-counters-design.md` §3/§6/§7）──
 * 纯 atomic64 累计量：随 mount 生（kzalloc(sbi) 天然清零 = S3）、随 umount 灭；
 * 打点只做 atomic64_add，**零分配零 I/O，绝不影响控制流**（P1）。
 * 98 字段 = §3 A–K 全清单；派生指标（命中率/写放大/换片率…，§4）**不存**，
 * 由 debugfs stats 文件现算（单一事实来源，P2）。
 * 口径：入口记 attempts、返回点记 outcomes（P5）；CHUNK 专属 9 个在 FULL 后端恒 0。
 */
struct ravenfs_sb_stats {
	/* A. VFS 操作面 op_（26）—— 负载画像 + 行为指纹 */
	atomic64_t	op_lookup;
	atomic64_t	op_create;
	atomic64_t	op_mkdir;
	atomic64_t	op_unlink;
	atomic64_t	op_rmdir;
	atomic64_t	op_rename;
	atomic64_t	op_link;
	atomic64_t	op_symlink;
	atomic64_t	op_mknod;
	atomic64_t	op_tmpfile;
	atomic64_t	op_readdir;
	atomic64_t	op_read_iter;
	atomic64_t	op_write_iter;
	atomic64_t	op_read_bytes;
	atomic64_t	op_write_bytes;
	atomic64_t	op_fallocate;
	atomic64_t	op_fsync;
	atomic64_t	op_syncfs;
	atomic64_t	op_getattr;
	atomic64_t	op_setattr;
	atomic64_t	op_mmap;
	atomic64_t	op_listxattr;
	atomic64_t	op_xattr_get;
	atomic64_t	op_xattr_set;
	atomic64_t	op_acl_get;
	atomic64_t	op_acl_set;

	/* B. inode 行缓存 ic_（9）—— 最核心调试价值 */
	atomic64_t	ic_hit;
	atomic64_t	ic_miss;
	atomic64_t	ic_row_load;
	atomic64_t	ic_retry;
	atomic64_t	ic_row_evict;
	atomic64_t	ic_row_reap;
	atomic64_t	ic_dirty_mark;
	atomic64_t	ic_wb_rows;
	atomic64_t	ic_err;

	/* C. mdata 节点缓存 md_（8） */
	atomic64_t	md_hit;
	atomic64_t	md_miss;
	atomic64_t	md_node_load;
	atomic64_t	md_retry;
	atomic64_t	md_drop;
	atomic64_t	md_reap;
	atomic64_t	md_node_wb;
	atomic64_t	md_err;

	/* D. TTL 后台 ttl_（2）—— “机制还活着”的证据 */
	atomic64_t	ttl_rounds;
	atomic64_t	ttl_round_errs;

	/* E. 块设备 IO io_（5） */
	atomic64_t	io_read_ops;
	atomic64_t	io_read_bytes;
	atomic64_t	io_write_ops;
	atomic64_t	io_write_bytes;
	atomic64_t	io_flush_ops;

	/* F. 运行时树分配器 al_（36）—— **独立子结构**（见 struct ravenfs_al_stats）。
	 * 旧 `bm_`（位图时代 18 字段）随位图模块退役而**删除**：新分配器是多分片主从红黑树
	 * （`ravenfs_allocator_top.h`），字段与埋点全变，见 `ravenfs_allocator_stats.h`。
	 * 单独成块（不进本数组）之理由：其下标须 = `enum rfs_al_stat`，以便注入钩子按
	 * 下标累加；混入本大数组会引入脆弱的绝对偏移。*/

	/* G. extent 树 ext_（7） */
	atomic64_t	ext_lookup;
	atomic64_t	ext_insert;
	atomic64_t	ext_split;
	atomic64_t	ext_new_root;
	atomic64_t	ext_balance;
	atomic64_t	ext_unmap;
	atomic64_t	ext_node_reads;

	/* H. 目录 dir_（3） */
	atomic64_t	dir_leaf_reads;
	atomic64_t	dir_emit;
	atomic64_t	dir_nlink_adjust;

	/* I. xattr / ACL xa_（10） */
	atomic64_t	xa_get;
	atomic64_t	xa_set;
	atomic64_t	xa_del;
	atomic64_t	xa_list;
	atomic64_t	xa_block_load;
	atomic64_t	xa_block_store;
	atomic64_t	xa_e2big;
	atomic64_t	xa_err;
	atomic64_t	acl_get;
	atomic64_t	acl_set;

	/* J. 错误与健康 err_（6）—— 少量计数器辅助判断，不搞复杂降级 */
	atomic64_t	err_shutdown_hits;
	atomic64_t	err_shutdown_trig;
	atomic64_t	err_meta_io;
	atomic64_t	err_sb_read;
	atomic64_t	err_enospc;
	atomic64_t	err_other;

	/* K. 生命周期 life_（4） */
	atomic64_t	life_mount;
	atomic64_t	life_umount;
	atomic64_t	life_iget_new;
	atomic64_t	life_evict;

	/* L. 名字堆 nh_（6；v0.1.2 新增）—— 大名字/长目录尾部名字存储的名字堆操作。
	 * 该模块不做直接磁盘 I/O（经 mdata 片，已并入 md_/io_）；本组只记"名字堆"这一层
	 * 自身的操作面，使「全模块」每模块都有自己的计数器。*/
	atomic64_t	nh_alloc;		/* 名字堆分配入口次数 */
	atomic64_t	nh_alloc_fail;		/* 名字堆分配失败（当前粒度档全满 -> -ENOSPC）*/
	atomic64_t	nh_write;		/* 尾部名字数据写入次数 */
	atomic64_t	nh_free;		/* 尾部名字存储释放次数 */
	atomic64_t	nh_activate;		/* 堆激活次数（首个大名字触发的晋升频率）*/
	atomic64_t	nh_release;		/* 空堆回收次数（目录折构时释放的堆数）*/
};

/*
 * ── F 组：运行时树分配器 al_（36，独立子结构）──
 * 字段顺序**必须**与 `enum rfs_al_stat`（ravenfs_allocator_stats.h）逐一对位：
 * 注入钩子按下标累加（`((atomic64_t *)&sbi->al)[field]`），show 表按序打印。
 * 增删字段 → 同改枚举 + show 名表 + BUILD_BUG_ON 三处。
 *
 * 说明：旧 `bm_`（位图时代）字段已删；本组由「平台中立的树分配器」经钩子注入填充
 * （见 allocator_adapter.c 的 `ko_al_stat_inc`）。
 */
struct ravenfs_al_stats {
	/* A 装配 / 生命周期（5）*/
	atomic64_t	al_init;
	atomic64_t	al_init_fail;
	atomic64_t	al_slices;
	atomic64_t	al_shutdown;
	atomic64_t	al_drain_timeout;
	/* B block 组顶层（9）*/
	atomic64_t	al_blk_alloc_hard_ok;
	atomic64_t	al_blk_alloc_hard_fail;
	atomic64_t	al_blk_alloc_soft_ok;
	atomic64_t	al_blk_alloc_soft_fail;
	atomic64_t	al_blk_occupy_ok;
	atomic64_t	al_blk_occupy_partial;
	atomic64_t	al_blk_occupy_fail;
	atomic64_t	al_blk_free_calls;
	atomic64_t	al_blk_free_blocks;
	/* C inode 组顶层（3）*/
	atomic64_t	al_ino_alloc_ok;
	atomic64_t	al_ino_alloc_fail;
	atomic64_t	al_ino_free_calls;
	/* D 片内双树 / 融合（8）*/
	atomic64_t	al_slice_alloc_hard;
	atomic64_t	al_slice_alloc_enospc;
	atomic64_t	al_slice_soft;
	atomic64_t	al_slice_soft_enospc;
	atomic64_t	al_slice_free;
	atomic64_t	al_slice_free_eexist;
	atomic64_t	al_tree_insert;
	atomic64_t	al_tree_erase;
	/* E 融合（3）*/
	atomic64_t	al_merge_left;
	atomic64_t	al_merge_right;
	atomic64_t	al_merge_both;
	/* F 路由 / 跨片（3）*/
	atomic64_t	al_route_first_hit;
	atomic64_t	al_route_rescan;
	atomic64_t	al_route_cross_slice;
	/* G checkpoint / 元素池（5）*/
	atomic64_t	al_checkpoint;
	atomic64_t	al_checkpoint_fail;
	atomic64_t	al_checkpoint_blocks;
	atomic64_t	al_pool_alloc;
	atomic64_t	al_pool_fail;
};

struct ravenfs_sb_info {
	u64	total_blocks;
	u64	inode_count;
	u64	root_ino;

	u64	bitmap_start;
	u64	bitmap_blocks;
	u64	inode_array_base;
	u64	inode_blocks;
	u64	inode_bm_base;
	u64	inode_bm_blocks;

	/* FS_block[i] 在设备上的字节偏移 = blk0_off + i*4096；blk0_off 恒 4096 */
	u64	blk0_off;

	struct ravenfs_cache *icache;	/* disk_inode_cache_mgr（§4.11.1） */
	struct ravenfs_mdata_cache *mcache;	/* mdata_piece_cache_mgr（8K 节点） */
	struct ravenfs_ttl *ttl;	/* 自写 TTL 淘汰器（cache_ttl.c） */
	struct rfs_allocator *alloc;	/* 运行时树分配器（挂载必建、无回退）；定义在 allocator_adapter.c */

	bool	shutdown;	/* RFS_SHUTDOWN（R-LM-09）：自检 / 元数据 I/O 失败 → 全局停机，入口返 -EIO */

	struct ravenfs_sb_stats stats;	/* §3 性能计数器：随 sbi 一起 kzalloc → mount 即清零（S3） */
	struct ravenfs_al_stats al;	/* F 组：运行时树分配器 al_（钩子注入；独立下标空间）*/
	struct dentry	*dbg_dir;	/* debugfs 挂载点目录（§8；内部探针，root-only） */
	struct proc_dir_entry *proc_dir;/* procfs 公开出口目录（D-11；`/proc/fs/ravenfs/<s_id>`，人人可读） */
	u64		mounted_at_ns;	/* stats 文件头行用（非计数器，§8.1） */
};

/*
 * ── 打点宏（§7.1；唯一入口）──
 * 必须判 `s_fs_info`：`parse_superblock` 失败 / `put_super` 尾段（kfree 之后）
 * 都可能出现 `sb` 在手但 sbi 已空的窗口 → 判空是本宏存在的唯一理由。
 * 只碰 atomic64，**零分配零 I/O，不睡眠，可安全持锁调用**（P1/§7.2）。
 */
#define RFS_STAT_INC(sb, field)		RFS_STAT_ADD((sb), field, 1)
#define RFS_STAT_ADD(sb, field, n)					\
	do {								\
		struct super_block *__rfs_sb = (sb);			\
		struct ravenfs_sb_info *__rfs_s = __rfs_sb ? __rfs_sb->s_fs_info : NULL;	\
		if (__rfs_s)						\
			atomic64_add((n), &__rfs_s->stats.field);	\
	} while (0)

/* F 组（al_）打点：字段在**独立子结构** `sbi->al`（下标空间 = enum rfs_al_stat）。
 * 与 RFS_STAT_* 同纪律（判空 / 只 atomic64 / 可持锁）。新增 al_ 字段用本对宏。*/
#define RFS_AL_INC(sb, field)		RFS_AL_ADD((sb), field, 1)
#define RFS_AL_ADD(sb, field, n)					\
	do {								\
		struct super_block *__rfs_sb = (sb);			\
		struct ravenfs_sb_info *__rfs_s = __rfs_sb ? __rfs_sb->s_fs_info : NULL;	\
		if (__rfs_s)						\
			atomic64_add((n), &__rfs_s->al.field);		\
	} while (0)

struct ravenfs_inode_info {
	struct inode		vfs_inode;	/* 骨架：盘记录归 mgr（§4.11.1）；此处仅备将来私有字段 */
	/* M4.1：REG/SYM inline 态（size ≤ 160）数据镜像。
	 * iomap->inline_data 须在 iomap_begin 返回后持续有效，而盘记录位于会随会话
	 * 释放的缓存行中 → 在此留稳定副本（iomap.c 在 inline 分支拷贝）。 */
	u8			inline_data[160];
	/* M5 残（R-NS-06）：fast symlink（≤160 内联）target 的内存稳定副本。
	 * 盘记录 inline_data 随缓存行可被淘汰 → 不能直接作 inode->i_link；
	 * read_inode 在此拷贝 target 并补 NUL，simple_get_link 返回此指针。 */
	char			symlink_target[RAVENFS_SYMLINK_INLINE_MAX + 1];
	/* M6（R-XA-01/03）：per-inode xattr 串行。
	 * set/ACL 走 xattr 块「读-改-写」，VFS 的 inode_lock 只覆盖 set 路径；
	 * get/list 无 inode_lock → 需自持锁防「并发 set 期间读到半改块 / 块被 free」。
	 * 锁序：inode_lock（VFS，set 路径持有）→ xattr_sem（本锁）；get/list 仅持本锁。 */
	struct rw_semaphore	xattr_sem;
};

static inline struct ravenfs_inode_info *RAVENFS_I(struct inode *inode)
{
	return container_of(inode, struct ravenfs_inode_info, vfs_inode);
}

/* ─────────────────────── 核心小工具 ─────────────────────── */

/* FS_block 号 → super_block 块号（bdev page cache 视角）；blk0_off/4096 恒 = 1 */
static inline sector_t ravenfs_dev_blk(const struct ravenfs_sb_info *sbi, u64 fs_blk)
{
	return (sbi->blk0_off / RAVENFS_BLOCK_SIZE) + fs_blk;
}

/* 类型值域校验（v1.2 不变量 27） */
static inline bool ravenfs_type_valid(u32 type)
{
	switch (type) {
	case INODE_TYPE_FIFO:
	case INODE_TYPE_CHR:
	case INODE_TYPE_DIR:
	case INODE_TYPE_BLK:
	case INODE_TYPE_REGULAR:
	case INODE_TYPE_SOFT_SYM:
	case INODE_TYPE_SOCK:
		return true;
	default:
		return false;
	}
}

/* 几何推导（与 mkfs/mkfs.cpp:calc_geometry 逐式对应） */
static inline u64 ravenfs_bitmap_blocks(u64 total_blocks)
{
	return (total_blocks + RAVENFS_BITS_PER_BLOCK - 1) / RAVENFS_BITS_PER_BLOCK;
}
static inline u64 ravenfs_inode_array_blocks(u64 inode_count)
{
	return (inode_count * RAVENFS_INODE_SIZE + RAVENFS_BLOCK_SIZE - 1) / RAVENFS_BLOCK_SIZE;
}
static inline u64 ravenfs_inode_bm_blocks(u64 inode_count)
{
	return (inode_count + RAVENFS_BITS_PER_BLOCK - 1) / RAVENFS_BITS_PER_BLOCK;
}

/* ─────────────── 跨模块核心入口（super.c 实现） ─────────────── */

struct inode *ravenfs_iget(struct super_block *sb, u64 inum);
/* 读一个 B+tree 节点（8K）到调用方缓冲；实现随 mdata 节点读路径（mdata_cache.c），
 * 但消费方横跨 dentry/extent/file/super → 归核心 */
int ravenfs_read_node(struct super_block *sb, u64 blk, void *buf);

/* ── 元数据可写？（ro 挂载不得写盘） ── */
static inline bool ravenfs_sb_writable(const struct super_block *sb)
{
	return !(sb->s_flags & SB_RDONLY);
}

/* ── R-LM-09：内部停机标志（xfs force-shutdown 惯例）──
 * 置位后所有入口返 -EIO；恢复 = 彻底 umount + 重新 mount（不做在线 remount-ro）。 */
static inline bool ravenfs_is_shutdown(const struct super_block *sb)
{
	const struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;

	return sbi && READ_ONCE(sbi->shutdown);
}
void ravenfs_force_shutdown(struct super_block *sb, const char *why);

/* ── R-SY-05：设备 flush 屏障（保证「返回即落盘」）──
 * 无 bdev / ro 挂载 → no-op 返 0。 */
int ravenfs_issue_flush(struct super_block *sb);

#endif /* _RAVENFS_H */

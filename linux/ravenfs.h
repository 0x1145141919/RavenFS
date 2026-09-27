/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs.h —— RavenFS Linux KO 内部头（M1：挂载骨架）
 *
 * 盘面真值：include/disk_struct_v1.2.h（v1.2，C/C++ 双兼容；内置 __KERNEL__ 分支）
 * 规格：Docs/linux/ravenfs-linux-ko-requirements-spec.md
 *
 * 【M1 范围声明（诚实边界，勿当成品）】
 *   本里程碑只做：模块注册 → bdev 绑定 → SB 读+校验 → 几何推导 → root inode(iget)
 *   → 只读 mount → 空目录 readdir/lookup/getattr/statfs。
 *   **非空目录的 dentry 树遍历未实现** —— lookup/iterate 对非空目录显式返回
 *   -EOPNOTSUPP（拒绝而非静默漏），M2 补。
 *   文件数据路径（a_ops / extent 树）完全未接 —— M2+。
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

/* v1.2 盘面结构（-I$(src)/../include） */
#include <disk_struct_v1.2.h>
#include <magics.h>	/* SUPERBLOCK_MAGIC */

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

/* ─────────────────────────── 内存结构 ─────────────────────────── */

/* ── 前向声明（定义在本文件末尾区块）── */
struct ravenfs_cache;
struct ravenfs_row;
struct ravenfs_shard;
struct ravenfs_session;
struct ravenfs_mdata_cache;
struct ravenfs_mrow;
struct ravenfs_mdata_shard;
struct ravenfs_msession;

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
	struct ravenfs_bitmap_mgr *bitmap;	/* 双位图分片分配器（bitmap.c；全量常驻内存分片、非缓存行，自有分片级同步） */

	bool	shutdown;	/* RFS_SHUTDOWN（R-LM-09）：自检 / 元数据 I/O 失败 → 全局停机，入口返 -EIO */
};

/* fast symlink target 内联上限（不变量 22：size ≤ 160 = 内联态）；
 * == REG 内联上限，便于三态判据复用。 */
#define RAVENFS_SYMLINK_INLINE_MAX	160U
/* slow symlink target 总上限（单块，对齐 ext2 sb_blocksize 口径） */
#define RAVENFS_SYMLINK_MAX		4096U

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
};

static inline struct ravenfs_inode_info *RAVENFS_I(struct inode *inode)
{
	return container_of(inode, struct ravenfs_inode_info, vfs_inode);
}

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

/* ── 跨文件接口（super.c ↔ dentry.c，M2） ── */
#define RAVENFS_NODE_BYTES 8192		/* B+tree 节点 = 2×4K */

struct dentry *ravenfs_lookup(struct inode *dir, struct dentry *dentry,
			      unsigned int flags);
int ravenfs_iterate(struct file *file, struct dir_context *ctx);
int ravenfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
		   umode_t mode, bool excl);
struct dentry *ravenfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
			     umode_t mode);
int ravenfs_unlink(struct inode *dir, struct dentry *dentry);
int ravenfs_rmdir(struct inode *dir, struct dentry *dentry);
int ravenfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
		   struct inode *new_dir, struct dentry *new_dentry, unsigned int flags);

/* M5 残（R-NS-05/06/07/09）：硬链接 / 符号链接 / 特殊节点 / O_TMPFILE */
int ravenfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry);
int ravenfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
		    const char *symname);
int ravenfs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
		  umode_t mode, dev_t rdev);
int ravenfs_tmpfile(struct mnt_idmap *idmap, struct inode *dir, struct file *file,
		    umode_t mode);

/* symlink 双态 i_op（R-NS-06）：fast = simple_get_link / slow = page_get_link。
 * 定义在 super.c（与 getattr/setattr/fileattr 回调同编译单元），namei 侧装配引用。 */
extern const struct inode_operations ravenfs_fast_symlink_inode_operations;
extern const struct inode_operations ravenfs_symlink_inode_operations;

/* 名字物化（≤40 内联 / >40 读名字堆尾段）—— 供 dentry_write.c 复合序比较用 */
int ravenfs_ddentry_name(struct super_block *sb, const struct ddentry_t *e,
		       char *out, unsigned int *out_len);

/* ── dentry B+树 写入（dentry_write.c）：多级 walk + 插入（含分裂/升根） ── */
struct rfs_dpath {
	u32	depth;
	u64	blk[16];	/* 每层块号（叶在 depth-1） */
	u32	cidx[16];	/* 内层 = hint idx；叶层 = GLB（0xFFFF=NONE） */
	bool	hit;
	u32	ins_idx;
};
int ravenfs_ddentry_walk(struct super_block *sb, u64 dir_ino, const char *name, u32 nlen,
			struct rfs_dpath *p);
int ravenfs_ddentry_insert(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p,
			  const char *name, u32 nlen, u64 hash, u64 inum, u64 remain_ptr);

/* 删除一个条目（含 balance/合并/抹匀/根收缩）；info 回传被删条目的下游资产信息 */
struct rfs_delinfo {
	u64	inum;		/* 目标 inode 编号（52 位形态） */
	u32	name_len;	/* 全名长（RFS_PTR_FREE） */
	u64	remain;		/* 长名尾段定位；短名 = PTR_INVALID */
};
int ravenfs_ddentry_delete(struct super_block *sb, u64 dir_ino, const struct rfs_dpath *p,
			   struct rfs_delinfo *info);

struct inode *ravenfs_iget(struct super_block *sb, u64 inum);
int ravenfs_read_node(struct super_block *sb, u64 blk, void *buf);

/* ── M4.1 只读数据路径（iomap.c / file.c） ── */
struct iomap_ops;
struct iomap;
extern const struct iomap_ops ravenfs_iomap_ops;
extern const struct address_space_operations ravenfs_aops;
ssize_t ravenfs_file_read_iter(struct kiocb *iocb, struct iov_iter *iter);
/* M4.2：可写 mmap 立刻分配（D-M4-12）——挂自写 vm_ops（page_mkwrite→iomap 写向） */
struct vm_area_desc;
int ravenfs_file_mmap_prepare(struct vm_area_desc *desc);

/* M4.2：回写映射复用同一 begin（file.c 的 writeback_range 使用） */
int ravenfs_iomap_begin(struct inode *inode, loff_t pos, loff_t length,
			unsigned int flags, struct iomap *iomap, struct iomap *srcmap);

/* ── M4.2 写数据路径（extent_write.c / iomap.c / file.c） ── */
ssize_t ravenfs_file_write_iter(struct kiocb *iocb, struct iov_iter *iter);

/* ── M4.3 truncate / punch / fallocate（file.c） ── */
long ravenfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len);
int ravenfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr);

/* 区段树写机械（extent_write.c）：查找 / 插入（含叶裂升根）/ 计数 */
int ravenfs_extent_lookup(struct super_block *sb, u64 root, u64 target,
			  struct extent_t *out, u64 *next);
int ravenfs_extent_insert(struct super_block *sb, u64 ino, u64 *root_io,
			  const struct extent_t *ext);
int ravenfs_extent_count(struct super_block *sb, u64 root, u64 *data, u64 *nodes);

/* ── M4.3 区段树删除/平衡（extent_write.c）── */
/* 空根叶（direct→extent 物化 / 空树终态） */
int ravenfs_extent_new_root_leaf(struct super_block *sb, u64 ino, u64 *root_io);
/* 子集撤销（loose）；根更迭旧根经 old_root 上抛（延迟 free，调用方在 inode 提交后 free） */
int ravenfs_extent_unmap_loose(struct super_block *sb, u64 ino, u64 *root_io,
			       const struct extent_t *ext, u64 *old_root);

/* ── disk_inode_cache_mgr（§4.11.1 + M2.2 行尺寸解耦）── */
#define RAVENFS_SHARD_SEL_BITS	7	/* 分片选择位宽：≤128 片（>4096 inode 时生效） */

/* 支持的缓存行尺寸（mount option `row_size=`，默认 4K） */
#define RAVENFS_ROW_DEFAULT	4096u
#define RAVENFS_ROW_SIZE_OK(sz)	((sz) == 4096u || (sz) == 8192u || (sz) == 16384u || \
				 (sz) == 32768u || (sz) == 65536u)

/*
 * 行缓冲直接走**伙伴系统**（不再经 slab）：order = log2(row_size/4K)。
 * 行尺寸均为 2 的幂（4K..64K）→ order 0..4，天然对齐伙伴分配阶。
 * 访问用**直映射物理内存指针**（page_address()；x86_64 无 highmem）。
 */
#define RAVENFS_ORDER_OF(bytes)	ilog2((bytes) >> PAGE_SHIFT)

/* 行生命周期状态（分片 rwsem 护） */
#define ROW_READY	0UL
#define ROW_EVICTING	1UL
/* 行标志（io_lock 护） */
#define ROWF_DIRTY	(1u << 0)	/* 内容脏，待回写 */
#define ROWF_WB		(1u << 1)	/* copy-out 快照在飞 */

/* 标行使脏（io_lock 写护）——inode 行 / mdata 行**通用**（struct 字段同名）。
 * 外部写点统一走此宏（勿再裸写 row->dirty/stamp；调用点需含 linux/{rwsem,timekeeping}.h）。 */
#define RFS_ROW_MARK_DIRTY(row)					\
	do {							\
		down_write(&(row)->io_lock);			\
		(row)->flags |= ROWF_DIRTY;			\
		(row)->wb_stamp = ktime_get_ns();		\
		up_write(&(row)->io_lock);			\
	} while (0)

/*
 * 缓存行状态机（详见 Docs/linux/ravenfs-ko-cache-statemachine.md）：
 *   分片 rwsem —— xarray 成员 + ref pinning + state（get 读锁内 ref++；淘汰写锁内 ref==1 判）
 *   per-row io_lock —— 内容 + flags（读=session 读共享；写=session 变异 / copy-out 排他）
 *   acc_stamp(atomic relaxed) —— TTL；wb_stamp(io_lock) —— 回写 aging
 * 铁律：I/O 一律在锁外（copy-out 后 / 淘汰独占窗口）。
 */
struct ravenfs_row {
	struct page	*page;		/* 伙伴系统页块（order = cache->order，共 row_size 字节） */
	void		*data;		/* 直映射物理内存 = page_address(page) */
	refcount_t	ref;		/* 1=缓存基准；>1=session 持有 / 回写 pin */

	struct rw_semaphore io_lock;	/* 内容 + flags 守护 */
	unsigned int	flags;		/* ROWF_DIRTY | ROWF_WB */
	unsigned long	state;		/* ROW_READY | ROW_EVICTING */

	atomic64_t	acc_stamp;	/* 最近访问（ns，relaxed）→ TTL 淘汰 */
	u64		wb_stamp;	/* 最近落盘/最近 dirty（io_lock 护）→ 回写 aging */
	u64		line;		/* 行号 = inum>>row_bits（诊断用） */
};

struct ravenfs_shard {
	struct xarray		rows;	/* row_key → struct ravenfs_row* */
	struct rw_semaphore	lock;	/* 睡眠读写锁：行生命周期 + 内容 RMW */
};

struct ravenfs_cache {
	struct ravenfs_shard	*shards;
	u32	n_slices;
	u32	bits_len;	/* ceil_log2(inode_count) */
	u32	w0;		/* 分片选择位宽 */
	u32	shard_bits;	/* 每片 inode 数 = 2^shard_bits */

	/* M2.2：缓存行几何（由 mount 的 row_size 派生） */
	u32	row_size;	/* 行字节数（4K..64K） */
	u32	inodes_per_row;	/* = row_size / 256 */
	u32	row_blocks;	/* = row_size / 4096（连续块数） */
	u32	row_bits;	/* log2(inodes_per_row) */
	u32	row_bits_shard;	/* 片内行键位宽 = shard_bits - row_bits */
	unsigned long row_mask;	/* 片内行键掩码（行键为 unsigned long） */
	u32	line_mask;	/* inum & line_mask = 行内槽位 */

	u32	order;		/* 行缓冲的伙伴系统 order（4K→0 … 64K→4） */
};

struct ravenfs_session {
	struct ravenfs_sb_info	*sbi;
	struct ravenfs_shard	*shard;
	struct ravenfs_row	*row;
	u64			inum;
	u32			off_mask;	/* 行内槽位掩码（= inodes_per_row-1） */
};

int  ravenfs_cache_create(struct super_block *sb, unsigned int row_size);
void ravenfs_cache_destroy(struct super_block *sb);
void ravenfs_cache_flush_all(struct super_block *sb);
/* TTL 淘汰：对超时（acc_stamp<cutoff）且 ref==1 的行做「脏原地回写 → 释放」 */
long ravenfs_cache_reap_expired(struct super_block *sb, u64 cutoff_ns);
/* 驻留回写（copy-out 排他 → 锁外 I/O）：ignore_age=false 时只写 wb_stamp<cutoff 的 */
int  ravenfs_cache_wb_dirty(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age);
int  ravenfs_cache_writeback_all(struct super_block *sb);
int  ravenfs_inode_flush(struct super_block *sb, u64 inum);
int  ravenfs_inode_sync_from_vfs(struct super_block *sb, struct inode *inode);
void ravenfs_inode_mark_dirty(struct super_block *sb, u64 inum);
void ravenfs_inode_reclaim(struct super_block *sb, u64 inum);

/* ── 名字堆（name_heap.c）：长名（>40B）外置部分的 分配/写/释放 ── */
int ravenfs_name_heap_alloc(struct super_block *sb, u64 dir_ino, u32 name_len,
			    u64 *out_remain_ptr);
int ravenfs_name_heap_write(struct super_block *sb, u32 name_len, u64 remain_ptr,
			    const char *tail);
int ravenfs_name_heap_free(struct super_block *sb, u64 dir_ino, u32 name_len,
			   u64 remain_ptr);
/* 释放目录的全部【已空】名字堆并重置描述符（rmdir / 目录析构） */
int ravenfs_dir_name_heap_release(struct super_block *sb, u64 dir_ino);

struct ravenfs_session *ravenfs_session_get(struct super_block *sb, u64 inum);
void ravenfs_session_put(struct ravenfs_session *s);

DEFINE_FREE(ravenfs_session, struct ravenfs_session *,
	    if (!IS_ERR_OR_NULL(_T)) ravenfs_session_put(_T))

/* 用法： struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, ino); */
#define RAVENFS_SESSION(sb, inum, var)	\
	struct ravenfs_session *var __free(ravenfs_session) = ravenfs_session_get((sb), (inum))

static inline struct raven_Inode_t *ravenfs_session_inode(struct ravenfs_session *s)
{
	return (struct raven_Inode_t *)((char *)s->row->data +
					(s->inum & s->off_mask) * RAVENFS_INODE_SIZE);
}

unsigned int ravenfs_type_dt(u32 type);

/* ── mdata_piece_cache_mgr（仿 disk_inode_cache_mgr；granule 固定 8K = 1 个 B+tree 节点） ── */
#define RAVENFS_MDATA_NODE_BYTES	8192
#define RAVENFS_MDATA_BLK_PER_NODE	2	/* 8K = 2×4K */

struct ravenfs_mrow {
	struct page	*page;		/* 伙伴系统页块（order=1，8K） */
	void		*data;		/* 直映射物理内存 = page_address(page) */
	refcount_t	ref;

	struct rw_semaphore io_lock;	/* 内容 + flags 守护 */
	unsigned int	flags;		/* ROWF_DIRTY | ROWF_WB */
	unsigned long	state;		/* ROW_READY | ROW_EVICTING */

	atomic64_t	acc_stamp;	/* 最近访问（ns，relaxed）→ TTL */
	u64		wb_stamp;	/* 回写 aging（io_lock 护） */
	u64		node_idx;
};

struct ravenfs_mdata_shard {
	struct xarray		rows;	/* 片内 node_idx → struct ravenfs_mrow* */
	struct rw_semaphore	lock;
};

struct ravenfs_mdata_cache {
	struct ravenfs_mdata_shard	*shards;
	u32	order;		/* 节点缓冲 order（8K → 1） */
	u32	n_slices;
	u32	bits_len;	/* ceil_log2(total_blocks) */
	u32	w0;
	u32	shard_bits;	/* 片内容纳的节点数 log2 */
	unsigned long node_mask;
	u64	total_blocks;
};

struct ravenfs_msession {
	struct ravenfs_sb_info		*sbi;
	struct ravenfs_mdata_shard	*shard;
	struct ravenfs_mrow		*row;
	u64				node_idx;
};

int  ravenfs_mdata_cache_create(struct super_block *sb);
void ravenfs_mdata_cache_destroy(struct super_block *sb);
long ravenfs_mdata_reap_expired(struct super_block *sb, u64 cutoff_ns);
void ravenfs_mdata_flush_all(struct super_block *sb);
int  ravenfs_mdata_wb_dirty(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age);
int  ravenfs_mdata_writeback_all(struct super_block *sb);
int  ravenfs_mdata_wb_node(struct super_block *sb, u64 blk);	/* 单节点回写（fsync 用） */

/* ── 架构无关回刷 driver（缓存后端：inode 行 / mdata 节点）──
 * 位图（bitmap.c）= 全量常驻内存分片，**非缓存行**、不接入本 driver（自有分片级同步）。 */
struct ravenfs_wb_ops {
	const char	*name;
	int		(*wb_dirty)(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age);
	long		(*reap)(struct super_block *sb, u64 cutoff_ns);
};
extern const struct ravenfs_wb_ops ravenfs_wb_inode_ops;
extern const struct ravenfs_wb_ops ravenfs_wb_mdata_ops;

struct ravenfs_msession *ravenfs_mdata_get(struct super_block *sb, u64 blk);
void ravenfs_mdata_put(struct ravenfs_msession *s);
/* 释放节点块前丢弃其 mdata 缓存行（防陈旧脏行回写污染复用块） */
void ravenfs_mdata_drop(struct super_block *sb, u64 blk);

DEFINE_FREE(ravenfs_msession, struct ravenfs_msession *,
	    if (!IS_ERR_OR_NULL(_T)) ravenfs_mdata_put(_T))

#define RAVENFS_MSESSION(sb, blk, var)	\
	struct ravenfs_msession *var __free(ravenfs_msession) = ravenfs_mdata_get((sb), (blk))

static inline void *ravenfs_msession_node(struct ravenfs_msession *s)
{
	return s->row->data;
}

/* 标记 mdata 行脏（io_lock 写护；调用点需含 linux/timekeeping.h） */
#define NODE_DIRTY(ms) RFS_ROW_MARK_DIRTY((ms)->row)

/* ── 自写 TTL 淘汰器（cache_ttl.c）── */
struct ravenfs_ttl;
int  ravenfs_ttl_start(struct super_block *sb);
void ravenfs_ttl_stop(struct super_block *sb);

/* 元数据可写？（ro 挂载不得写盘） */
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

/* ── 双位图分片分配器（bitmap.c）── */
struct ravenfs_bitmap_mgr;
int  ravenfs_bitmap_create(struct super_block *sb);
void ravenfs_bitmap_destroy(struct super_block *sb);
/* 全片同步两组位图（锁内快照 → 锁外 I/O）；TTL 周期 / sync_fs/put_super/fsync 共用 */
int  ravenfs_bitmap_flush(struct super_block *sb);
u64  ravenfs_block_alloc(struct super_block *sb, u64 count, u32 align_log2);
void ravenfs_block_free(struct super_block *sb, u64 blk, u64 count);
bool ravenfs_block_test(struct super_block *sb, u64 blk);
u64  ravenfs_inode_alloc(struct super_block *sb);
void ravenfs_inode_free(struct super_block *sb, u64 inum);
bool ravenfs_inode_test(struct super_block *sb, u64 inum);
u64  ravenfs_block_free_total(struct super_block *sb);
u64  ravenfs_inode_free_total(struct super_block *sb);

#endif /* _RAVENFS_H */

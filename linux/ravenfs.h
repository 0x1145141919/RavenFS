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
};

struct ravenfs_inode_info {
	struct inode		vfs_inode;	/* 骨架：盘记录归 mgr（§4.11.1）；此处仅备将来私有字段 */
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
struct inode *ravenfs_iget(struct super_block *sb, u64 inum);
int ravenfs_read_node(struct super_block *sb, u64 blk, void *buf);

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

struct ravenfs_row {
	struct page	*page;		/* 伙伴系统页块（order = cache->order，共 row_size 字节） */
	void		*data;		/* 直映射物理内存 = page_address(page) */
	refcount_t	ref;		/* 1 = 缓存基准；>1 = 有 session 持有 */
	bool		dirty;		/* M3：写回语义 */
	u64		stamp;		/* 最近访问时间（ns；TTL 淘汰用，对齐 god accss_stamp） */
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
long ravenfs_cache_reap_expired(struct ravenfs_cache *c, u64 cutoff_ns);

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
	bool		dirty;
	u64		stamp;		/* 最近访问时间（ns；TTL 淘汰用） */
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
long ravenfs_mdata_reap_expired(struct ravenfs_mdata_cache *c, u64 cutoff_ns);
void ravenfs_mdata_flush_all(struct super_block *sb);

struct ravenfs_msession *ravenfs_mdata_get(struct super_block *sb, u64 blk);
void ravenfs_mdata_put(struct ravenfs_msession *s);

DEFINE_FREE(ravenfs_msession, struct ravenfs_msession *,
	    if (!IS_ERR_OR_NULL(_T)) ravenfs_mdata_put(_T))

#define RAVENFS_MSESSION(sb, blk, var)	\
	struct ravenfs_msession *var __free(ravenfs_msession) = ravenfs_mdata_get((sb), (blk))

static inline void *ravenfs_msession_node(struct ravenfs_msession *s)
{
	return s->row->data;
}

/* ── 自写 TTL 淘汰器（cache_ttl.c）── */
struct ravenfs_ttl;
int  ravenfs_ttl_start(struct super_block *sb);
void ravenfs_ttl_stop(struct super_block *sb);

#endif /* _RAVENFS_H */

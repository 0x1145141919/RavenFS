// SPDX-License-Identifier: GPL-2.0
/*
 * super.c —— RavenFS Linux KO：模块注册 + 挂载骨架（M1）
 *
 * 里程碑 M1：register_filesystem → get_tree_bdev → SB 读+校验(magic/version/crc32)
 *            → 几何推导 → root inode(iget) → 只读挂载 + 空目录 readdir/lookup/getattr/statfs
 *
 * 依据：Docs/linux/ravenfs-linux-ko-requirements-spec.md
 *   R-KP-01/02/03 模块与 fs_type 注册、bdev 绑定、SB 校验
 *   R-LM-01/02/10 inode 分配 / write_inode / sb 关键字段
 *   R-NS-01/11    lookup / iterate_shared
 *   R-MD-01       getattr
 *   R-SP-01/06    statfs / s_maxbytes
 *
 * 【M1 诚实边界（勿当成品）】
 *   · 非空目录的 dentry 树遍历未实现 → lookup/iterate 显式 -EOPNOTSUPP（拒绝而非静默漏）
 *   · 文件数据路径（a_ops / extent 树）完全未接
 *   · 写路径（write_inode 落盘 / evict 回收 / 位图持久化）未接 → 强制只读挂载
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/pagemap.h>
#include <linux/fs_parser.h>
#include <linux/bitmap.h>
#include <linux/crc32.h>
#include <linux/string.h>
#include <linux/fileattr.h>

#include "ravenfs.h"
#include "ravenfs_dir.h"
#include "ravenfs_file.h"
#include "ravenfs_cache.h"
#include "ravenfs_mdata_cache.h"
#include "ravenfs_bitmap.h"
#include "ravenfs_xattr.h"

/* ── 前向声明（ops 表在 read_inode 里被引用） ── */
static const struct inode_operations ravenfs_dir_inode_operations;
static const struct inode_operations ravenfs_file_inode_operations;
static const struct file_operations ravenfs_dir_operations;
static const struct file_operations ravenfs_file_operations;
static const struct fs_context_operations ravenfs_context_ops;
static int ravenfs_fill_super(struct super_block *sb, struct fs_context *fc);
static int ravenfs_sync_fs(struct super_block *sb, int wait);
static int ravenfs_fsync(struct file *file, loff_t start, loff_t end, int datasync);

static struct kmem_cache *ravenfs_inode_cachep;
static struct file_system_type ravenfs_fs_type;

/* ═══════════════════════════════════════════════════════════════════
 * R-LM-09：内部停机（xfs force-shutdown 惯例） + R-SY-05：flush 屏障
 * ═══════════════════════════════════════════════════════════════════ */

/* 自检 / 元数据 I/O 失败 → 置全局停机；幂等。恢复 = umount + 重挂（不做在线 remount-ro）。 */
void ravenfs_force_shutdown(struct super_block *sb, const char *why)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;

	if (!sbi)
		return;
	if (!sbi->shutdown) {
		WRITE_ONCE(sbi->shutdown, true);
		pr_err("ravenfs: FORCED SHUTDOWN (%s) —— 后续操作返 -EIO；请 umount 后重新 mount\n",
		       why ? why : "?");
	}
}

/* 设备 flush 屏障（R-SY-05）：保证此前已提交的写全部落盘（返回即持久）。
 * 无 bdev / ro 挂载 → no-op。设备不支持 flush 时内核层返回 0。 */
int ravenfs_issue_flush(struct super_block *sb)
{
	if (!sb || !sb->s_bdev || (sb->s_flags & SB_RDONLY))
		return 0;
	return blkdev_issue_flush(sb->s_bdev);
}

/* ── fsync 定向回写：只落「本 inode 树」上的脏节点（非全 mdata 超集）──
 * 靠 data_root 递推走树；深度上限防成环。节点不在缓存 = 已落盘（no-op）。 */
#define RFS_FLUSH_DEPTH_MAX 64u

static int ravenfs_flush_extent_tree(struct super_block *sb, u64 root, u32 depth)
{
	const struct Bptree_meta_t *m;
	void *buf;
	u32 cnt, i;
	int err = 0;

	if (depth >= RFS_FLUSH_DEPTH_MAX || root == (u64)PTR_INVALID)
		return 0;
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	if (ravenfs_read_node(sb, root, buf)) {
		kfree(buf);
		return 0;
	}
	m = buf;
	if (m->magics[0] != BPTREE_MAGIC1 || m->magics[1] != BPTREE_MAGIC2 ||
	    m->magics[2] != BPTREE_MAGIC3 || m->magics[3] != BPTREE_MAGIC4) {
		kfree(buf);
		return 0;
	}
	if (ravenfs_mdata_wb_node(sb, root))
		err = -EIO;
	cnt = RFS_PP_COUNT(m->parent_ptr);
	if (RFS_PP_IS_INNER(m->parent_ptr)) {
		const struct Bptree_extent_inner_node_t *nd = buf;

		for (i = 0; i < cnt && i < 338 && !err; i++)
			err = ravenfs_flush_extent_tree(sb,
				RFS_PTR_ADDR(nd->extents[i].sub_node_ptr), depth + 1);
	}
	kfree(buf);
	return err;
}

static int ravenfs_flush_ddentry_tree(struct super_block *sb, u64 root, u32 depth)
{
	const struct Bptree_meta_t *m;
	void *buf;
	u32 cnt, i;
	int err = 0;

	if (depth >= RFS_FLUSH_DEPTH_MAX || root == (u64)PTR_INVALID)
		return 0;
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	if (ravenfs_read_node(sb, root, buf)) {
		kfree(buf);
		return 0;
	}
	m = buf;
	if (m->magics[0] != DENTRY_MAGIC1 || m->magics[1] != DENTRY_MAGIC2 ||
	    m->magics[2] != DENTRY_MAGIC3 || m->magics[3] != DENTRY_MAGIC4) {
		kfree(buf);
		return 0;
	}
	if (ravenfs_mdata_wb_node(sb, root))
		err = -EIO;
	cnt = RFS_PP_COUNT(m->parent_ptr);
	if (RFS_PP_IS_INNER(m->parent_ptr)) {
		const struct Bptree_ddentry_inner_node_t *nd = buf;

		for (i = 0; i < cnt && i < 127 && !err; i++)
			err = ravenfs_flush_ddentry_tree(sb,
				RFS_PTR_ADDR(nd->hints[i].sub_ptr), depth + 1);
	}
	kfree(buf);
	return err;
}

/* 该 inode 是否需要/是否有 mdata 树：DIR → dentry 树；REG/SYM（非 inline 非 direct）→ extent 树 */
static int ravenfs_flush_inode_trees(struct super_block *sb, struct inode *inode)
{
	struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, inode->i_ino);
	struct raven_Inode_t *r;

	if (IS_ERR_OR_NULL(s))
		return 0;		/* 行不在缓存 → 无脏元数据 */
	r = ravenfs_session_inode(s);
	if (S_ISDIR(inode->i_mode)) {
		if (r->data_root == (u64)PTR_INVALID)
			return 0;
		return ravenfs_flush_ddentry_tree(sb, r->data_root, 0);
	}
	if (S_ISREG(inode->i_mode) || S_ISLNK(inode->i_mode)) {
		if (r->size > 160 && !RFS_INO1_GET_DIRECT(r->word1) &&
		    r->data_root != (u64)PTR_INVALID)
			return ravenfs_flush_extent_tree(sb, r->data_root, 0);
	}
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * inode 生命周期（R-LM-01）
 * ═══════════════════════════════════════════════════════════════════ */

static struct inode *ravenfs_alloc_inode(struct super_block *sb)
{
	struct ravenfs_inode_info *ri;

	ri = alloc_inode_sb(sb, ravenfs_inode_cachep, GFP_KERNEL);
	if (!ri)
		return NULL;
	inode_init_once(&ri->vfs_inode);
	init_rwsem(&ri->xattr_sem);	/* M6：per-inode xattr 串行 */
	return &ri->vfs_inode;
}

static void ravenfs_free_inode(struct inode *inode)
{
	kmem_cache_free(ravenfs_inode_cachep, RAVENFS_I(inode));
}

static void ravenfs_destroy_inode(struct inode *inode)
{
	ravenfs_free_inode(inode);
}

/* ═══════════════════════════════════════════════════════════════════
 * 盘 inode → 内存 inode（R-LM-02 读侧）
 * ═══════════════════════════════════════════════════════════════════ */

/* 块统计（M2.1）见下方 ravenfs_inode_blocks4k */

/* 块统计（M2.1）：4K 块数。**诚实**：数据块 + 元数据块（8K 节点计 2 块）均计；
 * DIR 含 dentry 树节点 + name_heap（位图+数据面）；xattr 块也计。 */

static void ravenfs_count_extent(struct super_block *sb, u64 root, u32 depth,
				 u64 *data, u64 *nodes)
{
	const struct Bptree_meta_t *m;
	void *buf;
	u32 cnt, i;

	if (depth > 32 || root == (u64)PTR_INVALID)
		return;
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return;
	if (ravenfs_read_node(sb, root, buf)) {
		kfree(buf);
		return;
	}
	m = buf;
	if (m->magics[0] != BPTREE_MAGIC1 || m->magics[1] != BPTREE_MAGIC2 ||
	    m->magics[2] != BPTREE_MAGIC3 || m->magics[3] != BPTREE_MAGIC4) {
		kfree(buf);
		return;
	}
	(*nodes)++;					/* 每个 extent 节点 = 2 个 4K 块（调用方 ×2） */
	cnt = RFS_PP_COUNT(m->parent_ptr);
	if (RFS_PP_IS_INNER(m->parent_ptr)) {
		const struct Bptree_extent_inner_node_t *nd = buf;

		for (i = 0; i < cnt && i < 338; i++)
			ravenfs_count_extent(sb, RFS_PTR_ADDR(nd->extents[i].sub_node_ptr),
					     depth + 1, data, nodes);
	} else {
		const struct Bptree_extent_leaf_node_t *lf = buf;

		for (i = 0; i < cnt && i < 338; i++)
			*data += lf->extents[i].length;
	}
	kfree(buf);
}

static void ravenfs_count_ddentry_nodes(struct super_block *sb, u64 blk, u32 depth, u64 *nodes)
{
	const struct Bptree_meta_t *m;
	void *buf;
	u32 cnt, i;

	if (depth > 32)
		return;
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return;
	if (ravenfs_read_node(sb, blk, buf)) {
		kfree(buf);
		return;
	}
	m = buf;
	if (m->magics[0] != DENTRY_MAGIC1 || m->magics[1] != DENTRY_MAGIC2 ||
	    m->magics[2] != DENTRY_MAGIC3 || m->magics[3] != DENTRY_MAGIC4) {
		kfree(buf);
		return;
	}
	(*nodes)++;
	cnt = RFS_PP_COUNT(m->parent_ptr);
	if (RFS_PP_IS_INNER(m->parent_ptr)) {
		const struct Bptree_ddentry_inner_node_t *nd = buf;

		for (i = 0; i < cnt && i < 127; i++)
			ravenfs_count_ddentry_nodes(sb, RFS_PTR_ADDR(nd->hints[i].sub_ptr),
						   depth + 1, nodes);
	}
	kfree(buf);
}

static u64 ravenfs_inode_blocks4k(struct super_block *sb, const struct raven_Inode_t *r, u32 type)
{
	u64 blks = 0, i;

	if (RFS_XATTRPTR_GET(r->mode_xattr) != (u64)RFS_XATTR_NONE)
		blks += 1;				/* xattr 块（1 个 4K） */

	if (type == INODE_TYPE_DIR) {
		const struct raven_dir_Inode_t *d = (const struct raven_dir_Inode_t *)r;
		u64 nodes = 0;

		ravenfs_count_ddentry_nodes(sb, r->data_root, 0, &nodes);
		blks += nodes * 2;			/* dentry 树节点（8K/个） */
		for (i = 0; i < RFS_DIR_HEAP_COUNT; i++) {	/* name_heap：位图面 + 数据面 */
			const struct name_heap_controller_t *h = rfs_dir_heap_c(d, i);
			u64 bmp, dat;
			u32 bits, gran;

			if (!h)
				continue;
			bmp = RFS_NHC_BITMAP(*h);
			dat = RFS_NHC_DATA(*h);
			if (bmp == (u64)RFS_MASK52 && dat == (u64)RFS_MASK52)
				continue;		/* 未激活 */
			bits = RFS_NHC_BITS(*h);
			gran = RFS_NHC_GRAN(*h);
			blks += (1ULL << bits) / RAVENFS_BITS_PER_BLOCK;
			blks += 1ULL << (bits + gran - 12);
		}
	} else if (type == INODE_TYPE_REGULAR || type == INODE_TYPE_SOFT_SYM) {
		if (r->size > 160) {
			if (RFS_INO1_GET_DIRECT(r->word1)) {
				blks += (r->size + RAVENFS_BLOCK_SIZE - 1) /
					RAVENFS_BLOCK_SIZE;		/* direct：全覆盖 */
			} else {
				u64 data = 0, nodes = 0;

				ravenfs_count_extent(sb, r->data_root, 0, &data, &nodes);
				blks += data + nodes * 2;	/* 数据 + extent 树节点 */
			}
		}
	}
	return blks;
}

static int ravenfs_read_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;
	const struct raven_Inode_t *r;
	u64 inum = inode->i_ino;
	u32 type, perm;

	if (inum >= sbi->inode_count)
		return -EIO;

	ses = ravenfs_session_get(sb, inum);	/* 经 disk_inode_cache_mgr + session 取盘记录 */
	if (IS_ERR(ses))
		return PTR_ERR(ses);
	r = ravenfs_session_inode(ses);

	type = RFS_INO1_GET_TYPE(r->word1);
	perm = RFS_MODE_GET(r->mode_xattr);
	if (!ravenfs_type_valid(type)) {
		pr_err("ravenfs: inode %llu bad type %u\n", inum, type);
		return -EIO;
	}

	inode->i_mode = RFS_IMODE_BUILD(type, perm);
	i_uid_write(inode, r->uid);
	i_gid_write(inode, r->gid);
	set_nlink(inode, r->link_count ? r->link_count : 1);
	inode->i_size = r->size;
	inode_set_mtime(inode, r->mtime, r->m_ns_stamp);
	inode_set_ctime(inode, r->ctime, r->c_ns_stamp);
	inode_set_atime(inode, r->atime, r->a_ns_stamp);
	/* 已分配块数（512B 单位）：数据块 + 元数据块（见 ravenfs_inode_blocks4k） */
	inode->i_blocks = ravenfs_inode_blocks4k(sb, r, type) *
			  (RAVENFS_BLOCK_SIZE / 512);

	switch (type) {
	case INODE_TYPE_DIR:
		inode->i_op = &ravenfs_dir_inode_operations;
		inode->i_fop = &ravenfs_dir_operations;
		break;
	case INODE_TYPE_REGULAR:
		inode->i_op = &ravenfs_file_inode_operations;
		inode->i_fop = &ravenfs_file_operations;
		/* M4.1：REG 数据路径走 iomap a_ops（mmap/read_folio 前置，D-M4-1） */
		inode->i_data.a_ops = &ravenfs_aops;
		mapping_set_folio_order_range(inode->i_mapping, 0, 4);   /* ← 加这行：4K..64K */    
		break;
	case INODE_TYPE_SOFT_SYM:
		/* R-NS-06：fast（≤160 内联 → simple_get_link）/ slow（page_get_link）双态分派。
		 * a_ops 供 slow 态 read_mapping_folio 读 target（fast 态不用页缓存）。 */
		inode->i_data.a_ops = &ravenfs_aops;
		if (r->size <= RAVENFS_SYMLINK_INLINE_MAX) {
			struct ravenfs_inode_info *ri = RAVENFS_I(inode);
			u32 tl = min_t(u64, r->size, RAVENFS_SYMLINK_INLINE_MAX);

			memcpy(ri->symlink_target, r->inline_data, tl);
			ri->symlink_target[tl] = '\0';
			inode->i_link = ri->symlink_target;
			inode->i_op = &ravenfs_fast_symlink_inode_operations;
		} else {
			/* slow：page_get_link 直接映射 folio，而 __page_get_link 有
			 * `BUG_ON(mapping_gfp_mask(mapping) & __GFP_HIGHMEM)`（无 highmem
			 * 才能直映射）→ slow 态须清 highmem（照抄 ext2_symlink 的
			 * inode_nohighmem；否则 readlink 必在 __page_get_link 里 BUG）。 */
			inode_nohighmem(inode);
			inode->i_op = &ravenfs_symlink_inode_operations;
		}
		break;
	case INODE_TYPE_CHR:
	case INODE_TYPE_BLK: {
		/* rdev = inline_data[0:4] 小端 u32（v1.2 §4） */
		__le32 rdev_le;
		memcpy(&rdev_le, r->inline_data, sizeof(rdev_le));
		init_special_inode(inode, inode->i_mode,
				   new_decode_dev(le32_to_cpu(rdev_le)));
		break;
	}
	default:	/* FIFO / SOCK */
		init_special_inode(inode, inode->i_mode, 0);
		break;
	}
	return 0;
}

struct inode *ravenfs_iget(struct super_block *sb, u64 inum)
{
	struct inode *inode;
	int err;

	inode = iget_locked(sb, inum);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode_state_read_once(inode) & I_NEW))
		return inode;

	err = ravenfs_read_inode(inode);
	if (err) {
		iget_failed(inode);
		return ERR_PTR(err);
	}
	unlock_new_inode(inode);
	return inode;
}

/* ═══════════════════════════════════════════════════════════════════
 * 目录操作（M1：空目录正确；非空目录显式拒绝）
 * ═══════════════════════════════════════════════════════════════════ */

/* 目录操作（lookup / iterate）实现在 dentry.c；M2 起为真正的 dentry 树遍历 */

/* ═══════════════════════════════════════════════════════════════════
 * getattr / statfs
 * ═══════════════════════════════════════════════════════════════════ */

static int ravenfs_getattr(struct mnt_idmap *idmap, const struct path *path,
			   struct kstat *stat, u32 request_mask,
			   unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);

	/* R-MD-01：内存 inode 直填（rdev 已由 i_rdev 提供；dir nlink 即盘上 size+2） */
	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

/*
 * R-MD-09 / R-DG-03：`i_flags` 语义不实现（字段保留、写 0）。
 * 不挂默认 → `vfs_fileattr_get()` 返 `-ENOIOCTLCMD`，`lsattr/chattr` 只会得到
 * `-ENOTTY` 且**语义含糊**。显式返 `-EOPNOTSUPP` 才符合「降级」口径
 * （`chattr +i` 必须被拒且**绝不**出现「报成功却仍可写」的半开）。
 */
static int ravenfs_fileattr_get(struct dentry *dentry, struct file_kattr *fa)
{
	return -EOPNOTSUPP;
}

static int ravenfs_fileattr_set(struct mnt_idmap *idmap, struct dentry *dentry,
				struct file_kattr *fa)
{
	return -EOPNOTSUPP;
}

/* 位图区间内已亮灯比特数（bit = 已用）；读失败退化为 0 已用 */
static u64 ravenfs_bitmap_used(struct super_block *sb, u64 start, u64 nblocks,
			       u64 total_bits)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 used = 0, i, base = 0;
	struct buffer_head *bh;

	for (i = 0; i < nblocks && base < total_bits; i++, base += RAVENFS_BITS_PER_BLOCK) {
		u64 nbits = min_t(u64, RAVENFS_BITS_PER_BLOCK, total_bits - base);

		bh = sb_bread(sb, ravenfs_dev_blk(sbi, start + i));
		if (!bh)
			return 0;
		used += bitmap_weight((const unsigned long *)bh->b_data, nbits);
		brelse(bh);
	}
	return used;
}

static int ravenfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 used_blk, used_ino;

	used_blk = ravenfs_bitmap_used(sb, sbi->bitmap_start, sbi->bitmap_blocks,
				       sbi->total_blocks);
	used_ino = ravenfs_bitmap_used(sb, sbi->inode_bm_base, sbi->inode_bm_blocks,
				       sbi->inode_count);

	buf->f_type = RAVENFS_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_frsize = sb->s_blocksize;
	buf->f_blocks = sbi->total_blocks;
	buf->f_bfree = sbi->total_blocks > used_blk ? sbi->total_blocks - used_blk : 0;
	buf->f_bavail = buf->f_bfree;
	buf->f_files = sbi->inode_count;
	buf->f_ffree = sbi->inode_count > used_ino ? sbi->inode_count - used_ino : 0;
	buf->f_namelen = RAVENFS_MAX_NAME_LEN;
	buf->f_fsid.val[0] = RAVENFS_SUPER_MAGIC;
	buf->f_fsid.val[1] = (u32)(sbi->total_blocks ^ sbi->inode_count);
	return 0;
}

struct ravenfs_fs_context {
	unsigned int	row_size;	/* 0 = 默认 4K；mount option `row_size=` */
};

enum ravenfs_param {
	Opt_row_size,
};

static const struct fs_parameter_spec ravenfs_fs_parameters[] = {
	fsparam_string("row_size", Opt_row_size),
	{}
};

/* 接受 4k/8k/16k/32k/64k（大小写）与纯数字 4096..65536 */
static bool ravenfs_row_size_str(const char *s, unsigned int *out)
{
	static const unsigned int tbl[] = { 4096, 8192, 16384, 32768, 65536 };
	static const char *nm[] = { "4k", "8k", "16k", "32k", "64k" };
	unsigned int v;
	int i;

	if (!kstrtouint(s, 0, &v)) {
		for (i = 0; i < 5; i++)
			if (v == tbl[i]) { *out = v; return true; }
		return false;
	}
	for (i = 0; i < 5; i++) {
		if (strlen(s) != strlen(nm[i]))
			continue;
		if (!strncasecmp(s, nm[i], strlen(nm[i]))) {
			*out = tbl[i];
			return true;
		}
	}
	return false;
}

static int ravenfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct ravenfs_fs_context *ctx = fc->fs_private;
	struct fs_parse_result result;
	unsigned int sz;
	int opt = fs_parse(fc, ravenfs_fs_parameters, param, &result);

	if (opt < 0)
		return opt;
	switch (opt) {
	case Opt_row_size:
		if (!ravenfs_row_size_str(param->string, &sz)) {
			pr_err("ravenfs: invalid row_size= (支持 4k/8k/16k/32k/64k)\n");
			return -EINVAL;
		}
		ctx->row_size = sz;
		return 0;
	}
	return -EINVAL;
}

static int ravenfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	/* 不落盘：把 VFS 字段同步进 mgr 行 + 标 dirty（盘专有字段不动）；落盘交 flush_all */
	return ravenfs_inode_sync_from_vfs(inode->i_sb, inode);
}

static void ravenfs_evict_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	u64 data_root = (u64)PTR_INVALID;
	u64 data_size = 0;
	bool data_direct = false, has_data = false;

	/*
	 * nlink==0 → 回收，**先 inode 后 free**（§10 不变量 25；handover §6.1）：
	 *   ① 读盘记录抓外置资产定位（data_root / size / direct_bit；reclaim 会清零）；
	 *   ② ravenfs_inode_reclaim：记录清零 + inode 位图槽回收（引用先消失）；
	 *   ③ ravenfs_release_inode_data：释放 data 块 / 区段树（引用消失后才 free）。
	 * 仅 REG/SOFT_SYM（DIR 资产归 rmdir 自清；inline size ≤160 无外置资产 → 跳过 ③）。
	 */
	if (!inode->i_nlink) {
		struct ravenfs_session *s __free(ravenfs_session) =
			ravenfs_session_get(sb, inode->i_ino);
		u64 xptr = (u64)RFS_XATTR_NONE;

		if (!IS_ERR_OR_NULL(s)) {
			struct raven_Inode_t *r = ravenfs_session_inode(s);

			xptr = RFS_XATTRPTR_GET(r->mode_xattr);	/* M6：xattr 块指针 */
			if (r->size > 160 &&
			    (S_ISREG(inode->i_mode) || S_ISLNK(inode->i_mode))) {
				data_root = r->data_root;
				data_size = r->size;
				data_direct = RFS_INO1_GET_DIRECT(r->word1) != 0;
				has_data = true;
			}
		}
		ravenfs_inode_reclaim(sb, inode->i_ino);
		/* M6：释放 xattr 块（先 inode 后 free：记录已清零，引用先消失）*/
		if (xptr != (u64)RFS_XATTR_NONE)
			ravenfs_block_free(sb, xptr, 1);
		if (has_data)
			ravenfs_release_inode_data(sb, data_root, data_size, data_direct);
	}
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static void ravenfs_put_super(struct super_block *sb)
{
	ravenfs_ttl_stop(sb);		/* 先停 TTL 淘汰器（避免与折构竞争） */
	/* 干净卸载（R-SY-06/T3）：写序 mdata → inode → 位图，各阶段 + 末尾 flush 屏障 */
	ravenfs_mdata_cache_destroy(sb);	/* ① 折构：主动全量刷回 mdata 节点（§4.11.1 ②） */
	ravenfs_issue_flush(sb);
	ravenfs_cache_destroy(sb);		/* ② inode 记录行全量刷回 */
	ravenfs_issue_flush(sb);
	/* ③ 位图全量刷回（M3 持久化）；ro 挂载不写 */
	if (!(sb->s_flags & SB_RDONLY))
		ravenfs_bitmap_flush(sb);
	ravenfs_issue_flush(sb);
	ravenfs_bitmap_destroy(sb);
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

/*
 * 卷级同步（sync(1)/syncfs(2)）：脏元数据行 + 位图 全量刷回；ro 挂载 no-op。
 *
 * 写序（R-SY-04/05，D-C1；§6.1 修正）：**mdata 树 → inode 行 → 位图**。
 *   · mdata 先于 inode：inode 行内的 `data_root` 指向该 inode 的树根节点 → 引用者（inode 行）
 *     必须晚于被引用者（节点）落盘，否则重启见「inode.data_root → 未分配块」。
 *   · 每阶段后发 `blkdev_issue_flush`：设备可乱序写，无屏障则「按顺序调用」也保不住顺序（§6.1）。
 *   · 末尾再屏障：保证本函数返回即全卷落盘（syncfs(2) 语义，R-SY-02）。
 * 注：flush 中途崩溃仍可能留 dangling（位图最后 = 引用先于亮灯）—— §6.1 已裁为设计特性（不修）。
 */
static int ravenfs_sync_fs(struct super_block *sb, int wait)
{
	int err = 0, e;

	if (!ravenfs_sb_writable(sb))
		return 0;
	if (ravenfs_is_shutdown(sb))
		return -EIO;

	/* ① mdata 树节点（extent / dentry 树；被 inode 行引用者先落盘） */
	e = ravenfs_mdata_writeback_all(sb);
	if (e) {
		err = e;
		ravenfs_force_shutdown(sb, "sync_fs: mdata writeback failed");
	}
	e = ravenfs_issue_flush(sb);
	if (e && !err)
		err = e;

	/* ② inode 记录行（data_root / size / 时间戳翻转） */
	e = ravenfs_cache_writeback_all(sb);
	if (e) {
		if (!err)
			err = e;
		ravenfs_force_shutdown(sb, "sync_fs: inode writeback failed");
	}
	e = ravenfs_issue_flush(sb);
	if (e && !err)
		err = e;

	/* ③ 位图（分配落地） */
	e = ravenfs_bitmap_flush(sb);
	if (e) {
		if (!err)
			err = e;
		ravenfs_force_shutdown(sb, "sync_fs: bitmap flush failed");
	}
	e = ravenfs_issue_flush(sb);
	if (e && !err)
		err = e;

	return err;
}

static const struct super_operations ravenfs_sops = {
	.alloc_inode	= ravenfs_alloc_inode,
	.destroy_inode	= ravenfs_destroy_inode,
	.write_inode	= ravenfs_write_inode,
	.evict_inode	= ravenfs_evict_inode,
	.put_super	= ravenfs_put_super,
	.statfs		= ravenfs_statfs,
	.sync_fs	= ravenfs_sync_fs,
};

static const struct inode_operations ravenfs_dir_inode_operations = {
	.lookup		= ravenfs_lookup,
	.create		= ravenfs_create,
	.link		= ravenfs_link,		/* M5 残 R-NS-05 */
	.symlink	= ravenfs_symlink,		/* M5 残 R-NS-06 */
	.mkdir		= ravenfs_mkdir,
	.unlink		= ravenfs_unlink,
	.rmdir		= ravenfs_rmdir,
	.mknod		= ravenfs_mknod,		/* M5 残 R-NS-07 */
	.rename		= ravenfs_rename,
	.tmpfile	= ravenfs_tmpfile,		/* M5 残 R-NS-09 */
	.getattr	= ravenfs_getattr,
	.setattr	= ravenfs_setattr,		/* M6：chmod 同步 access ACL（同 file i_op）*/
	.listxattr	= ravenfs_listxattr,	/* M6 R-XA-01 */
	.get_inode_acl	= ravenfs_get_inode_acl,	/* M6 R-XA-03 */
	.set_acl	= ravenfs_set_acl,
	.fileattr_get	= ravenfs_fileattr_get,	/* R-MD-09：chattr/lsattr → -EOPNOTSUPP */
	.fileattr_set	= ravenfs_fileattr_set,
};

static const struct inode_operations ravenfs_file_inode_operations = {
	.getattr	= ravenfs_getattr,
	.setattr	= ravenfs_setattr,			/* M4.4：chmod/chown/utimes/truncate（全属性） */
	.listxattr	= ravenfs_listxattr,	/* M6 R-XA-01 */
	.get_inode_acl	= ravenfs_get_inode_acl,	/* M6 R-XA-03 */
	.set_acl	= ravenfs_set_acl,
	.fileattr_get	= ravenfs_fileattr_get,	/* R-MD-09：chattr/lsattr → -EOPNOTSUPP */
	.fileattr_set	= ravenfs_fileattr_set,
};

/* R-NS-06：符号链接双态 i_op（装配点在 ravenfs_read_inode）。
 * fast：target 内联在 inode 私有区（simple_get_link 直接返回 i_link）；
 * slow：target 为文件数据（page_get_link 经页缓存读）。
 * 非 static：namei（dentry.c）在 symlink 建完 slow 数据后需装配。 */
const struct inode_operations ravenfs_fast_symlink_inode_operations = {
	.get_link	= simple_get_link,
	.getattr	= ravenfs_getattr,
	.setattr	= ravenfs_setattr,
	.fileattr_get	= ravenfs_fileattr_get,
	.fileattr_set	= ravenfs_fileattr_set,
};

const struct inode_operations ravenfs_symlink_inode_operations = {
	.get_link	= page_get_link,
	.getattr	= ravenfs_getattr,
	.setattr	= ravenfs_setattr,
	.fileattr_get	= ravenfs_fileattr_get,
	.fileattr_set	= ravenfs_fileattr_set,
};

static const struct file_operations ravenfs_dir_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= ravenfs_iterate,
	.fsync		= ravenfs_fsync,		/* 目录 fsync（durable-create：创建后 fsync 父目录） */
};

static int ravenfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	int err = 0, e;

	if (ravenfs_is_shutdown(sb))
		return -EIO;

	/* ① 脏数据 → 盘：dirty folio 经 iomap `writepages` 落到已分配块（D-M4-6 立刻分配） */
	err = filemap_write_and_wait_range(inode->i_mapping, start, end);
	if (err)
		return err;
	if (!ravenfs_sb_writable(sb))
		return 0;

	/* ② 元数据落盘 + flush 屏障（R-FD-04/R-SY-01/05）。
	 * 写序（R-SY-04）：本 inode 树节点 → inode 行 → 位图（各阶段后屏障）。
	 * 树节点 = 定向回写（DIR 的 dentry 树 / REG 的 extent 树，非全 mdata 超集）
	 * —— 避免把“未 fsync 的父目录树”提前落盘而与其 inode 行（size/data_root）失配。
	 * datasync 语义同 fsync：inode 行是唯一元数据提交点（data_root/size 与数据可访问性同源）。 */
	e = ravenfs_flush_inode_trees(sb, inode);
	if (e) {
		err = e;
		ravenfs_force_shutdown(sb, "fsync: inode tree writeback failed");
	}
	e = ravenfs_issue_flush(sb);
	if (e && !err)
		err = e;

	e = ravenfs_inode_flush(sb, inode->i_ino);
	if (e) {
		if (!err)
			err = e;
		ravenfs_force_shutdown(sb, "fsync: inode flush failed");
	}
	e = ravenfs_issue_flush(sb);
	if (e && !err)
		err = e;

	e = ravenfs_bitmap_flush(sb);
	if (e) {
		if (!err)
			err = e;
		ravenfs_force_shutdown(sb, "fsync: bitmap flush failed");
	}
	e = ravenfs_issue_flush(sb);
	if (e && !err)
		err = e;

	return err;
}

static const struct file_operations ravenfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= ravenfs_file_read_iter,		/* M4.1（自写壳 → generic_file_read_iter） */
	.write_iter	= ravenfs_file_write_iter,		/* M4.2（自写壳 → iomap_file_buffered_write，立刻分配） */
	.mmap_prepare	= ravenfs_file_mmap_prepare,		/* M4.2：generic vm_ops 只标脏；分配在回写落点（2026-09-28 修订 D-M4-12）*/
	.fallocate	= ravenfs_fallocate,			/* M4.3（R-FD-07；D-M4-11 拒纯 KEEP_SIZE） */
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.fsync		= ravenfs_fsync,
};

/* ═══════════════════════════════════════════════════════════════════
 * SB 读 + 校验 + 几何（R-KP-03）
 * ═══════════════════════════════════════════════════════════════════ */

static int ravenfs_parse_superblock(struct super_block *sb, const void *buf,
				    unsigned int row_size)
{
	const struct RavenSuperblock *dsb = buf;
	struct ravenfs_sb_info *sbi;
	u32 crc_stored, crc_calc;
	void *tmp;

	if (le64_to_cpu(dsb->magic) != (u64)SUPERBLOCK_MAGIC) {
		pr_err("ravenfs: bad superblock magic\n");
		return -EINVAL;
	}
	if (le32_to_cpu(dsb->version) != RAVENFS_VERSION) {
		pr_err("ravenfs: version %u != expected %u\n",
		       le32_to_cpu(dsb->version), RAVENFS_VERSION);
		return -EINVAL;
	}

	/* crc32：覆盖整个 4096B，计算时 crc32 字段置 0（与 mkfs 一致） */
	crc_stored = le32_to_cpu(dsb->crc32);
	tmp = kmalloc(RAVENFS_BLOCK_SIZE, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	memcpy(tmp, buf, RAVENFS_BLOCK_SIZE);
	memset(tmp + offsetof(struct RavenSuperblock, crc32), 0, sizeof(u32));
	crc_calc = crc32_le(~0U, tmp, RAVENFS_BLOCK_SIZE) ^ ~0U;
	kfree(tmp);
	if (crc_stored != crc_calc) {
		pr_err("ravenfs: sb crc mismatch (stored %08x calc %08x)\n",
		       crc_stored, crc_calc);
		return -EINVAL;
	}

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sbi->total_blocks = le64_to_cpu(dsb->total_blocks);
	sbi->inode_count = le64_to_cpu(dsb->inode_array_count);
	sbi->root_ino = le64_to_cpu(dsb->root_inode);

	if (!sbi->total_blocks || !sbi->inode_count) {
		pr_err("ravenfs: zero geometry\n");
		goto bad;
	}

	/* blk0_off = world_base_lba × 设备逻辑块大小；约定恒 = 4096
	 * （mkfs 把 Primary SB 放在设备字节 0，FS_block[0] 紧随其后 4096） */
	sbi->blk0_off = (u64)le64_to_cpu(dsb->world_base_lba) *
			bdev_logical_block_size(sb->s_bdev);
	if (sbi->blk0_off != RAVENFS_BLOCK_SIZE) {
		pr_err("ravenfs: blk0_off=%llu != %u (lba/base mismatch)\n",
		       sbi->blk0_off, RAVENFS_BLOCK_SIZE);
		goto bad;
	}

	sbi->bitmap_blocks = ravenfs_bitmap_blocks(sbi->total_blocks);
	sbi->bitmap_start = sbi->total_blocks - sbi->bitmap_blocks;
	sbi->inode_blocks = ravenfs_inode_array_blocks(sbi->inode_count);
	sbi->inode_array_base = sbi->bitmap_start - sbi->inode_blocks;
	sbi->inode_bm_blocks = ravenfs_inode_bm_blocks(sbi->inode_count);
	sbi->inode_bm_base = sbi->inode_array_base - sbi->inode_bm_blocks;

	if (sbi->inode_bm_base <= 2) {
		pr_err("ravenfs: geometry underflow\n");
		goto bad;
	}

	sb->s_fs_info = sbi;
	if (ravenfs_cache_create(sb, row_size) ||	/* disk_inode_cache_mgr（§4.11.1/M2.2） */
	    ravenfs_mdata_cache_create(sb)) {	/* mdata_piece_cache_mgr（8K 节点） */
		ravenfs_cache_destroy(sb);
		sb->s_fs_info = NULL;
		kfree(sbi);
		return -ENOMEM;
	}
	if (ravenfs_ttl_start(sb)) {	/* 自写 TTL 淘汰器（cache_ttl.c） */
		ravenfs_mdata_cache_destroy(sb);
		ravenfs_cache_destroy(sb);
		sb->s_fs_info = NULL;
		kfree(sbi);
		return -ENOMEM;
	}
	if (ravenfs_bitmap_create(sb)) {	/* 双位图内存分片分配器（bitmap.c） */
		ravenfs_ttl_stop(sb);
		ravenfs_mdata_cache_destroy(sb);
		ravenfs_cache_destroy(sb);
		sb->s_fs_info = NULL;
		kfree(sbi);
		return -EIO;
	}
	return 0;
bad:
	kfree(sbi);
	return -EINVAL;
}

static int ravenfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct ravenfs_fs_context *ctx = fc->fs_private;
	struct ravenfs_sb_info *sbi;
	struct buffer_head *bh;
	struct inode *root;
	unsigned int row_size = (ctx && ctx->row_size) ? ctx->row_size : RAVENFS_ROW_DEFAULT;
	int err;

	sb_set_blocksize(sb, RAVENFS_BLOCK_SIZE);
	sb->s_magic = RAVENFS_SUPER_MAGIC;
	sb->s_maxbytes = RAVENFS_MAX_FILESIZE;
	sb->s_time_gran = 1;
	sb->s_op = &ravenfs_sops;

	/* SB 在设备起始 4K（FS_block 世界之外 → bdev 块 0） */
	bh = sb_bread(sb, 0);
	if (!bh) {
		pr_err("ravenfs: cannot read superblock\n");
		return -EIO;
	}
	err = ravenfs_parse_superblock(sb, bh->b_data, row_size);
	brelse(bh);
	if (err)
		return err;

	/* M6（R-XA-01/02）：s_xattr 处理器（user/security/system；trusted 不注册 → 拒）。
	 * 注册即置 IOP_XATTR（fs/inode.c），set 路径 inode_lock 已由 VFS 把持。 */
	sb->s_xattr = ravenfs_xattr_handlers;
	/* M6（R-XA-03）：POSIX ACL —— 置 SB_POSIXACL 后 VFS 把 umask 剥离推迟给
	 * posix_acl_create（namei.h mode_strip_umask），故 creat/mknod 路径必须调
	 * ravenfs_init_acl（否则 umask 失效）。 */
	sb->s_flags |= SB_POSIXACL;

	sbi = sb->s_fs_info;
	root = ravenfs_iget(sb, sbi->root_ino);
	if (IS_ERR(root))
		return PTR_ERR(root);

	sb->s_root = d_make_root(root);
	if (!sb->s_root)
		return -ENOMEM;

	pr_info("ravenfs: mounted blocks=%llu inodes=%llu root_ino=%llu\n",
		sbi->total_blocks, sbi->inode_count, sbi->root_ino);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * fs_context / fs_type（R-KP-01/02）
 * ═══════════════════════════════════════════════════════════════════ */

static int ravenfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, ravenfs_fill_super);
}

static int ravenfs_init_fs_context(struct fs_context *fc)
{
	struct ravenfs_fs_context *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	fc->fs_private = ctx;
	fc->ops = &ravenfs_context_ops;
	return 0;
}

static void ravenfs_fc_free(struct fs_context *fc)
{
	kfree(fc->fs_private);
	fc->fs_private = NULL;
}

static const struct fs_context_operations ravenfs_context_ops = {
	.free		= ravenfs_fc_free,
	.parse_param	= ravenfs_parse_param,
	.get_tree	= ravenfs_get_tree,
};

static struct file_system_type ravenfs_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "ravenfs",
	.fs_flags		= FS_REQUIRES_DEV,
	.init_fs_context	= ravenfs_init_fs_context,
	.kill_sb		= kill_block_super,
};

/* ═══════════════════════════════════════════════════════════════════
 * 模块 init/exit
 * ═══════════════════════════════════════════════════════════════════ */

static int __init ravenfs_init(void)
{
	int err;

	/* R-NS-06 fast symlink：`i_link` 指向本对象内的 `symlink_target`，readlink(2)
	 * 经 copy_to_user 从该 slab 对象导出 → 必须给该字段一条 usercopy 白名单，
	 * 否则 hardened usercopy 判「从无白名单 SLUB 对象导出」直接 BUG（照抄 ext2/ufs：
	 * fs/ext2/super.c kmem_cache_create_usercopy(…, offsetof(i_data), sizeof(i_data))）。 */
	ravenfs_inode_cachep = kmem_cache_create_usercopy("ravenfs_inode_cache",
					sizeof(struct ravenfs_inode_info), 0,
					SLAB_HWCACHE_ALIGN | SLAB_RECLAIM_ACCOUNT,
					offsetof(struct ravenfs_inode_info, symlink_target),
					sizeof_field(struct ravenfs_inode_info, symlink_target),
					NULL);
	if (!ravenfs_inode_cachep)
		return -ENOMEM;

	err = register_filesystem(&ravenfs_fs_type);
	if (err) {
		kmem_cache_destroy(ravenfs_inode_cachep);
		ravenfs_inode_cachep = NULL;
		return err;
	}
	pr_info("ravenfs: module loaded\n");
	return 0;
}

static void __exit ravenfs_exit(void)
{
	unregister_filesystem(&ravenfs_fs_type);
	rcu_barrier();
	kmem_cache_destroy(ravenfs_inode_cachep);
	ravenfs_inode_cachep = NULL;
	pr_info("ravenfs: module unloaded\n");
}

module_init(ravenfs_init);
module_exit(ravenfs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RavenFS - block filesystem (linux ko, M1 mount skeleton)");
MODULE_AUTHOR("Sparrow");
MODULE_ALIAS_FS("ravenfs");

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
#include <linux/debugfs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/timekeeping.h>

#include "ravenfs.h"
#include "ravenfs_dir.h"
#include "ravenfs_file.h"
#include "ravenfs_cache.h"
#include "ravenfs_mdata_cache.h"
#include "ravenfs_allocator_top.h"
#include "ravenfs_allocator_adapter.h"
#include "ravenfs_extent.h"
#include "ravenfs_xattr.h"

/* ── 前向声明（ops 表在 read_inode 里被引用） ── */
static const struct inode_operations ravenfs_dir_inode_operations;
static const struct inode_operations ravenfs_file_inode_operations;
static const struct file_operations ravenfs_dir_operations;
static const struct file_operations ravenfs_file_operations;
static const struct fs_context_operations ravenfs_context_ops;
static int ravenfs_fill_super(struct super_block *sb, struct fs_context *fc);
static int ravenfs_sb_write_timestamp(struct super_block *sb, bool is_mount);
static int ravenfs_sync_fs(struct super_block *sb, int wait);
static int ravenfs_fsync(struct file *file, loff_t start, loff_t end, int datasync);

static struct kmem_cache *ravenfs_inode_cachep;
static struct file_system_type ravenfs_fs_type;

/*
 * ── §8 性能计数器出口：一个 seq_file kv 文件**全吐**（不拆多文件）──
 * 一次 cat = 一次完整观测。**同一个 `ravenfs_stats_show` 注册两处**（数据源单一）：
 *
 *   · procfs（D-11，公开面）：`/proc/fs/ravenfs/<s_id>/stats`，0444，**人人可读**。
 *     fs stat 惯例（同形 `/proc/fs/xfs/stat`）——普通用户 / 监控脚本可读。
 *   · debugfs（内部探针）：`/sys/kernel/debug/ravenfs/<s_id>/stats`，0444。
 *     debugfs 挂载点 `/sys/kernel/debug` 默认为 0700 → 事实 root-only（D-11 的由来），
 *     保留作内核开发期内部探针，**不再当作对外出口**。
 *
 * 字段增删改名无兼容义务？——**公开面（procfs）一旦稳定即为事实 ABI**，须为字段名/
 * 分组的增删改名立兼容纪律（对齐 D-10「对外契约成员清单」教训；见 D-11）。
 * 生命周期 = UAF 红线（i_private/data 持 sb/sbi 指针）：**两处出口都必须早于
 * kfree(sbi) 摘除**，见 put_super。
 */
static struct dentry *ravenfs_dbg_root;
static struct proc_dir_entry *ravenfs_proc_root;	/* /proc/fs/ravenfs */

static const char *const ravenfs_stat_names[] = {
	/* A op_ */  "op_lookup", "op_create", "op_mkdir", "op_unlink", "op_rmdir",
	"op_rename", "op_link", "op_symlink", "op_mknod", "op_tmpfile",
	"op_readdir", "op_read_iter", "op_write_iter", "op_read_bytes",
	"op_write_bytes", "op_fallocate", "op_fsync", "op_syncfs", "op_getattr",
	"op_setattr", "op_mmap", "op_listxattr", "op_xattr_get", "op_xattr_set",
	"op_acl_get", "op_acl_set",
	/* B ic_ */  "ic_get", "ic_get_err", "ic_locate_err", "ic_dirty",
	"ic_evict", "ic_flush", "ic_err",
	/* C md_ */  "md_hit", "md_miss", "md_node_load", "md_retry", "md_drop",
	"md_reap", "md_node_wb", "md_err",
	/* D ttl_ */ "ttl_rounds", "ttl_round_errs",
	/* E io_ */  "io_read_ops", "io_read_bytes", "io_write_ops", "io_write_bytes",
	"io_flush_ops",
	/* G ext_ */ "ext_lookup", "ext_insert", "ext_split", "ext_new_root",
	"ext_balance", "ext_unmap", "ext_node_reads", "ext_nd_ops", "ext_idp_ops",
	/* H dir_ */ "dir_leaf_reads", "dir_emit", "dir_nlink_adjust",
	/* I xa_ */  "xa_get", "xa_set", "xa_del", "xa_list", "xa_block_load",
	"xa_block_store", "xa_e2big", "xa_err", "acl_get", "acl_set",
	/* J err_ */ "err_shutdown_hits", "err_shutdown_trig", "err_meta_io",
	"err_sb_read", "err_enospc", "err_other",
	/* K life_ */"life_mount", "life_umount", "life_iget_new", "life_evict",
	/* M nd_ */  "nd_resolve", "nd_map_append", "nd_grow", "nd_alloc",
	"nd_free", "nd_read", "nd_write", "nd_release", "nd_reclaim",
	"nd_fr_inline", "nd_fr_tree", "nd_fr_split", "nd_fr_retire",
};

/*
 * F 组：运行时树分配器 al_（36）—— **独立子结构 `sbi->al`**，
 * 下标空间 = `enum rfs_al_stat`（ravenfs_allocator_stats.h），与下面名字表逐一对位。
 * BUILD_BUG_ON 同时守「子结构是纯 atomic64 无填充」与「名字表数目 == 字段数」。
 */
static const char *const ravenfs_al_stat_names[] = {
	/* A 装配 / 生命周期 */ "al_init", "al_init_fail", "al_slices",
	"al_shutdown", "al_drain_timeout",
	/* B block 组顶层 */ "al_blk_alloc_hard_ok", "al_blk_alloc_hard_fail",
	"al_blk_alloc_soft_ok", "al_blk_alloc_soft_fail", "al_blk_occupy_ok",
	"al_blk_occupy_partial", "al_blk_occupy_fail", "al_blk_free_calls",
	"al_blk_free_blocks",
	/* C inode 组顶层 */ "al_ino_alloc_ok", "al_ino_alloc_fail",
	"al_ino_free_calls",
	/* D 片内双树 */ "al_slice_alloc_hard", "al_slice_alloc_enospc",
	"al_slice_soft", "al_slice_soft_enospc", "al_slice_free",
	"al_slice_free_eexist", "al_tree_insert", "al_tree_erase",
	/* E 融合 */ "al_merge_left", "al_merge_right", "al_merge_both",
	/* F 路由 / 跨片 */ "al_route_first_hit", "al_route_rescan",
	"al_route_cross_slice",
	/* G checkpoint / 元素池 */ "al_checkpoint", "al_checkpoint_fail",
	"al_checkpoint_blocks", "al_pool_alloc", "al_pool_fail",
	/* H inode 数据面 */ "al_idp_locate_ok", "al_idp_locate_fail",
	"al_idp_free", "al_idp_grow", "al_idp_grow_pieces", "al_idp_fr_merge",
	"al_idp_tree_dump", "al_idp_tree_build", "al_idp_snap_write",
	"al_idp_snap_read", "al_idp_snap_blocks",
};

static int ravenfs_stats_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	const atomic64_t *st;
	unsigned int i;
	const char *cur_grp = NULL;

	/* 结构体成员全为 atomic64_t 且无填充 → 当连续数组读（下面断言守着） */
	BUILD_BUG_ON(sizeof(struct ravenfs_sb_stats) !=
		     ARRAY_SIZE(ravenfs_stat_names) * sizeof(atomic64_t));
	BUILD_BUG_ON(sizeof(struct ravenfs_al_stats) !=
		     ARRAY_SIZE(ravenfs_al_stat_names) * sizeof(atomic64_t));
	BUILD_BUG_ON(AL_STAT_COUNT != ARRAY_SIZE(ravenfs_al_stat_names));
	if (!sbi)
		return 0;
	st = (const atomic64_t *)&sbi->stats;
	seq_printf(m, "# ravenfs stats  sb=%s  mounted_at=%llu\n",
		   sb->s_id, (unsigned long long)sbi->mounted_at_ns);
	for (i = 0; i < ARRAY_SIZE(ravenfs_stat_names); i++) {
		const char *nm = ravenfs_stat_names[i];
		const char *us = strchr(nm, '_');
		size_t gl = us ? (size_t)(us - nm) : strlen(nm);

		/* 分组小节头（`# [op]` …）：名字前缀即组名；便于 grep/人读，勿作为解析依据 */
		if (!cur_grp || strlen(cur_grp) != gl || memcmp(cur_grp, nm, gl)) {
			seq_printf(m, "# [%.*s]\n", (int)gl, nm);
			cur_grp = nm;
		}
		seq_printf(m, "%s\t%llu\n", nm,
			   (unsigned long long)atomic64_read(&st[i]));
	}
	/* F 组：运行时树分配器 al_（独立子结构；下标 = enum rfs_al_stat）。*/
	{
		const atomic64_t *ast = (const atomic64_t *)&sbi->al;

		seq_puts(m, "# [al]\n");
		for (i = 0; i < ARRAY_SIZE(ravenfs_al_stat_names); i++)
			seq_printf(m, "%s\t%llu\n", ravenfs_al_stat_names[i],
				   (unsigned long long)atomic64_read(&ast[i]));
	}
	/* §5：状态段（gauge）*/
	seq_printf(m, "# state\nshutdown\t%d\n", ravenfs_is_shutdown(sb) ? 1 : 0);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ravenfs_stats);

/* ═══════════════════════════════════════════════════════════════════
 * R-LM-09：内部停机（xfs force-shutdown 惯例） + R-SY-05：flush 屏障
 * ═══════════════════════════════════════════════════════════════════ */

/* 自检 / 元数据 I/O 失败 → 置全局停机；幂等。恢复 = umount + 重挂（不做在线 remount-ro）。 */
void ravenfs_force_shutdown(struct super_block *sb, const char *why)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;

	if (!sbi)
		return;
	RFS_STAT_INC(sb, err_shutdown_trig);	/* §7.3：每次调用都记（不只首次） */
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
	RFS_STAT_INC(sb, io_flush_ops);	/* R-SY-05 屏障次数 */
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
	struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, RFS_INO_TO_IDX(inode->i_ino));
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
 * DIR 含 dentry 树节点 + 名字数据面（映射树数据块/节点 + fr 树节点）；xattr 块也计。 */

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

/* 名字数据面 fr 树节点计数（tree 态；8K 节点/个，调用方 ×2） */
static void ravenfs_count_fr_nodes(struct super_block *sb, u64 blk, u32 depth,
				   u64 *nodes)
{
	const struct Bptree_meta_t *m;
	void *buf;
	u32 cnt, i;

	if (depth > 64 || blk == (u64)PTR_INVALID)
		return;
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return;
	if (ravenfs_read_node(sb, blk, buf)) {
		kfree(buf);
		return;
	}
	m = buf;
	if (m->magics[0] != NAMEDATA_FREERANGES_MAGIC1 ||
	    m->magics[1] != NAMEDATA_FREERANGES_MAGIC2 ||
	    m->magics[2] != NAMEDATA_FREERANGES_MAGIC3 ||
	    m->magics[3] != NAMEDATA_FREERANGES_MAGIC4) {
		kfree(buf);
		return;
	}
	(*nodes)++;
	cnt = RFS_PP_COUNT(m->parent_ptr);
	if (RFS_PP_IS_INNER(m->parent_ptr)) {
		const struct Bptree_dir_name_data_free_ranges_inner_node_t *nd = buf;

		for (i = 0; i < cnt && i < 338; i++)
			ravenfs_count_fr_nodes(sb, RFS_PTR_ADDR(nd->hints[i].sub_node_ptr),
					       depth + 1, nodes);
	}
	kfree(buf);
}

static u64 ravenfs_inode_blocks4k(struct super_block *sb, const struct raven_Inode_t *r, u32 type)
{
	u64 blks = 0;

	if (RFS_XATTRPTR_GET(r->mode_xattr) != (u64)RFS_XATTR_NONE)
		blks += 1;				/* xattr 块（1 个 4K） */

	if (type == INODE_TYPE_DIR) {
		const struct raven_dir_Inode_t *d = (const struct raven_dir_Inode_t *)r;
		u64 ndctl = d->name_data_ctl;
		u64 ndptr = RFS_ND_CTL_PTR(ndctl);
		u64 nodes = 0;

		ravenfs_count_ddentry_nodes(sb, r->data_root, 0, &nodes);
		blks += nodes * 2;			/* dentry 树节点（8K/个） */

		/* 名字数据面：direct = pieces×2；tree = 映射数据块 + 映射树节点 */
		if (ndptr != RFS_ND_CTL_PTR_INVALID) {
			if (RFS_ND_CTL_IS_DIRECT(ndctl)) {
				blks += RFS_ND_PIECE_COUNT(d->name_data_piece_count) * 2;
			} else {
				u64 data = 0, mnodes = 0;

				ravenfs_extent_count_kind(sb, RFS_EXT_KIND_NAMEDATA,
							  ndptr, &data, &mnodes);
				blks += data + mnodes * 2;
			}
		}
		/* fr 树节点（tree 态）*/
		if (RFS_ND_FR_CTL_PTR(d->name_data_free_ranges_ctl) !=
				RFS_ND_FR_CTL_PTR_INVALID &&
		    !RFS_ND_FR_CTL_IS_INLINE(d->name_data_free_ranges_ctl)) {
			u64 fnodes = 0;

			ravenfs_count_fr_nodes(sb,
					RFS_ND_FR_CTL_PTR(d->name_data_free_ranges_ctl),
					0, &fnodes);
			blks += fnodes * 2;
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
	/* D-10 契约：VFS 面 ino → 内部盘 idx（唯一穿越点之一，禁散落 ±1） */
	u64 inum = RFS_INO_TO_IDX(inode->i_ino);
	u32 type, perm;

	if (inum >= rfs_allocator_inode_capacity(sbi->alloc))
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

	/* 入参 inum = 盘 idx；VFS 面 ino = idx + 1（D-10 契约，唯一构造点） */
	inode = iget_locked(sb, RFS_IDX_TO_INO(inum));
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode_state_read_once(inode) & I_NEW))
		return inode;

	RFS_STAT_INC(sb, life_iget_new);	/* inode churn：新建 VFS inode */
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

	RFS_STAT_INC(inode->i_sb, op_getattr);
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
	u64 used_blk;
	u64 ino_cap;

	used_blk = ravenfs_bitmap_used(sb, sbi->bitmap_start, sbi->bitmap_blocks,
				       sbi->total_blocks);
	ino_cap = rfs_allocator_inode_capacity(sbi->alloc);

	buf->f_type = RAVENFS_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_frsize = sb->s_blocksize;
	buf->f_blocks = sbi->total_blocks;
	buf->f_bfree = sbi->total_blocks > used_blk ? sbi->total_blocks - used_blk : 0;
	buf->f_bavail = buf->f_bfree;
	buf->f_files = ino_cap;
	buf->f_ffree = rfs_allocator_inode_free_total(sbi->alloc);
	buf->f_namelen = RAVENFS_MAX_NAME_LEN;
	buf->f_fsid.val[0] = RAVENFS_SUPER_MAGIC;
	buf->f_fsid.val[1] = (u32)(sbi->total_blocks ^ ino_cap);
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
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 data_root = (u64)PTR_INVALID;
	u64 data_size = 0;
	bool data_direct = false, has_data = false;

	RFS_STAT_INC(sb, life_evict);

	/*
	 * nlink==0 → 回收，**先 inode 后 free**（§10 不变量 25；handover §6.1）：
	 *   ① 读盘记录抓外置资产定位（data_root / size / direct_bit；reclaim 会清零）；
	 *   ② ravenfs_inode_reclaim：记录清零 + inode 位图槽回收（引用先消失）；
	 *   ③ ravenfs_release_inode_data：释放 data 块 / 区段树（引用消失后才 free）。
	 * 仅 REG/SOFT_SYM（DIR 资产归 rmdir 自清；inline size ≤160 无外置资产 → 跳过 ③）。
	 */
	if (!inode->i_nlink) {
		struct ravenfs_session *s __free(ravenfs_session) =
			ravenfs_session_get(sb, RFS_INO_TO_IDX(inode->i_ino));
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
		ravenfs_inode_reclaim(sb, RFS_INO_TO_IDX(inode->i_ino));
		/* M6：释放 xattr 块（先 inode 后 free：记录已清零，引用先消失）*/
		if (xptr != (u64)RFS_XATTR_NONE)
			(void)rfs_allocator_block_free(sbi->alloc, xptr, 1);
		if (has_data)
			ravenfs_release_inode_data(sb, data_root, data_size, data_direct);
	}
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static void ravenfs_put_super(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;

	RFS_STAT_INC(sb, life_umount);
	ravenfs_ttl_stop(sb);		/* 先停 TTL 淘汰器（避免与折构竞争） */
	/* v2.0 干净卸载硬顺序（同 sync_fs；§17.4）：(1) inode 面维护 → (2) mdata 回写 →
	 * (3) block 位图补集重建 → (4) SB 最后写。require mdata/分配器尚存活。*/
	if (sbi && ravenfs_sb_writable(sb) && !ravenfs_is_shutdown(sb))
		(void)ravenfs_allocator_flush(sb);
	ravenfs_issue_flush(sb);
	ravenfs_mdata_cache_destroy(sb);	/* 折构：全量刷回（幂等）+ 释放 mdata 缓存 */
	ravenfs_cache_destroy(sb);		/* v2.0：inode 缓存空壳 no-op */
	ravenfs_allocator_destroy(sb);		/* 排空 + 拆 inode 面/block 树 + 释放 words */

	/*
	 * 干净卸载时间戳 umount_time（Docs/disk_layout_v1.1.md #15）：写 now。
	 *
	 * 【位置：全部元数据写回（mdata → inode → 位图）之后】umount_time 是
	 * 「本会话数据已完整落盘」的收据——先开收据再落数据 = 掉电时盘面声称干净
	 * 而数据没落，收据必须最后开。SB 自身写不依赖位图/mdata，放 bitmap_destroy
	 * 之后、debugfs 摘除之前即可。
	 *
	 * 【三重闸（少一重都可能报假干净）】
	 *   ① ravenfs_sb_writable：readonly 挂载纪律不写（helper 内第一道闸也拦，双保险）；
	 *   ② !ravenfs_is_shutdown：本会话发生过 force_shutdown → 写路径故障、数据一致性
	 *      无保证 → 不写，让盘上留 mount > umount，下次 sbinfo 照样报「非正常关机」；
	 *      写了就是给可能损坏的数据盖「干净」章；
	 *   ③ sbi->mounted_at_ns != 0：本次挂载的 mount_time 确实写成功过（见 fill_super
	 *      处哨兵耦合注释）——mount 写失败的回滚路径不许开「只有 umount 没有 mount」
	 *      的错序收据（sbinfo 会把 umount ≥ mount 读成「干净卸载历史」= 假干净）。
	 *
	 * 【失败只记日志】put_super 是 void，无处传错；写失败 → 盘上留旧时间戳 →
	 * 读侧判非正常关机（保守安全方向），折构照常进行，不能因时间戳写不进而
	 * 卡住卸载。【crc/flush 细节见 ravenfs_sb_write_timestamp 头注】。
	 */
	if (sbi && sbi->mounted_at_ns && ravenfs_sb_writable(sb) &&
	    !ravenfs_is_shutdown(sb)) {
		int e = ravenfs_sb_write_timestamp(sb, false);

		if (e)
			pr_err("ravenfs: umount_time not written (%d) — 盘上留旧值，读侧将判非正常关机\n",
			       e);
	}

	/* §8.2 UAF 红线：stats 文件 data 持 sb/sbi 指针 → 两处出口目录都必须
	 * **早于** kfree(sbi) 摘除；debugfs_remove_recursive / proc_remove 会等进行中的
	 * read 结束（proc_remove 对 NULL 安全；内部 remove_proc_subtree 会等在读）。 */
	debugfs_remove_recursive(sbi ? sbi->dbg_dir : NULL);
	proc_remove(sbi ? sbi->proc_dir : NULL);
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
	int err;
	(void)wait;

	RFS_STAT_INC(sb, op_syncfs);
	if (!ravenfs_sb_writable(sb))
		return 0;
	if (ravenfs_is_shutdown(sb)) {
		RFS_STAT_INC(sb, err_shutdown_hits);
		return -EIO;
	}

	/* v2.0 硬顺序（Docs/disk_layout_v2.0.md §17.4；⇄ god all_sycn）：
	 *   (1) inode 数据面维护（建 direct/映射树 + 重写扁 fr；刷新 SB 四字段）
	 *   (2) mdata 脏节点（新树节点 / inode 片内容）落盘
	 *   (3) block 位图补集重建 + 落盘
	 *   (4) SB 最后写（crc + latest_syn_time）
	 * 全部封装在 ravenfs_allocator_flush 内（cache_ttl 30s 周期同此入口）。 */
	err = ravenfs_allocator_flush(sb);
	if (err)
		ravenfs_force_shutdown(sb, "sync_fs: allocator/inode-plane flush failed");
	return err;
}

/*
 * 挂载选项回读（D-9）。位图模块退役后（2026-10-04 硬切）本驱动只剩 `row_size=`
 * 一个自有选项，且该值不落 sbi（仅 mount 期消费）→ 不再回读；VFS 级选项由 VFS 输出。
 * 保留本回调（s_op 需要）但不输出 driver 选项。
 */
static int ravenfs_show_options(struct seq_file *m, struct dentry *dentry)
{
	(void)m;
	(void)dentry;
	return 0;
}

static const struct super_operations ravenfs_sops = {
	.alloc_inode	= ravenfs_alloc_inode,
	.destroy_inode	= ravenfs_destroy_inode,
	.write_inode	= ravenfs_write_inode,
	.evict_inode	= ravenfs_evict_inode,
	.put_super	= ravenfs_put_super,
	.statfs		= ravenfs_statfs,
	.sync_fs	= ravenfs_sync_fs,
	.show_options	= ravenfs_show_options,	/* D-9：参数可回读 */
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

	RFS_STAT_INC(sb, op_fsync);
	if (ravenfs_is_shutdown(sb)) {
		RFS_STAT_INC(sb, err_shutdown_hits);
		return -EIO;
	}

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

	e = ravenfs_inode_flush(sb, RFS_INO_TO_IDX(inode->i_ino));
	if (e) {
		if (!err)
			err = e;
		ravenfs_force_shutdown(sb, "fsync: inode flush failed");
	}
	e = ravenfs_issue_flush(sb);
	if (e && !err)
		err = e;

	e = ravenfs_allocator_flush(sb);
	if (e) {
		if (!err)
			err = e;
		ravenfs_force_shutdown(sb, "fsync: allocator flush failed");
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

	if (le64_to_cpu(dsb->magic[0]) != (u64)RFS_SB_MAGIC_W0) {
		pr_err("ravenfs: bad superblock magic[0]\n");
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
	/* v2.0 SB 四字段（全局 inode 数据面）*/
	sbi->inode_data_ctl = le64_to_cpu(dsb->inode_data_ctl);
	sbi->inode_data_plane_pieces_count =
		le64_to_cpu(dsb->inode_data_plane_pieces_count);
	sbi->inode_data_fr_ctl = le64_to_cpu(dsb->inode_data_fr_ctl);
	sbi->inode_data_fr_count = le64_to_cpu(dsb->inode_data_fr_count);
	sbi->ra_inode_count = RFS_IDP_PIECE_COUNT(sbi->inode_data_plane_pieces_count) *
			      RFS_IDP_INODES_PER_PIECE;

	if (!sbi->total_blocks || !sbi->ra_inode_count) {
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

	/* v2.0 几何：**只有块位图尾巴**（无固定 inode 数组 / 无 inode 位图）*/
	sbi->bitmap_blocks = ravenfs_bitmap_blocks(sbi->total_blocks);
	sbi->bitmap_start = sbi->total_blocks - sbi->bitmap_blocks;

	if (sbi->bitmap_start <= 2) {
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
	/* 运行时树分配器：设备读位图镜像 → 建树（挂载必建、无回退）。
	 * 失败（几何 / IO / 内存）→ 非零 → 按挂载失败回滚。*/
	if (ravenfs_allocator_create(sb)) {
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

/*
 * ── Superblock 卷生命周期时间戳回写（Docs/disk_layout_v1.1.md 变更表 #15）──
 * 与上面的 ravenfs_parse_superblock（读侧）同段落：读改写同一块、同一 crc 口径。
 *
 * 纪律真值（该行原文要点）：mount_time 非 readonly 挂载写 now；umount_time 干净
 * 卸载写 now；readonly / 审计挂载不写（0 哨兵 = 从未挂载 / 从未干净卸载必须保留）；
 * born 恒为 mkfs 值、modify 只留扩容工具 → **ko 两字段一律不许动**；**只写 Primary
 * SB（设备字节 0），Backup SB 不随动**（救急副本保留 mkfs 时刻）；每次写必重算
 * crc32（原始 4096B、置零 crc 字段、全长重算）。
 *
 * 【为什么直写底层 bdev，不走 ravenfs 自己的 VFS】
 *   SB 在设备字节 0 = FS_block 世界之外；ravenfs 的数据/元数据写路径全在 FS_block
 *   世界内（先过位图分配），拿它写 SB 是鸡生蛋——SB 写不进，位图分配器本身也没了。
 *   读侧 fill_super 本就 `sb_bread(sb, 0)` 直读 bdev，写侧对称 sb_bread +
 *   mark_buffer_dirty + sync_dirty_buffer：同一块号口径、同一缓冲读改写，不会出现
 *   「读的是 A 写的是 B」的坐标系分叉。
 *
 * 【crc32 用内核 crc32_le，不在 ko 里再养一份 256 项表】
 *   mkfs/audit/sbinfo 三处同口径的表算法：init 0xFFFFFFFF、逐字节
 *   crc = tab[(crc ^ b) & 0xFF] ^ (crc >> 8)、末尾 ^ 0xFFFFFFFF，反射多项式
 *   0xEDB88320。内核 crc32_le(seed, buf, len) 正是同多项式的反射实现（lib/crc32.c），
 *   写成 `crc32_le(~0U, buf, n) ^ ~0U` 即与表算法逐位同式。交叉验证已经存在：
 *   parse_superblock 一直用这个表达式校验 mkfs 写出的盘且校验通过（本盘 crc OK、
 *   ko 能挂载）→ 两边逐位一致已被实盘证明。在 ko 内再抄一份表只会多一处真值
 *   （DISCIPLINE：两份真值必然分叉，且分叉时无从察觉）。
 *   附带记录（不动文档）：mkfs.cpp 第 23 行把 0xEDB88320 注释成 “Castagnoli”，
 *   实际它是 CRC-32/ISO-HDLC（zlib/Ethernet）；标签写错了但算法没错，ko 跟算法、
 *   不跟标签。
 *
 * 【写前重验 crc，而不是改完直接回填】
 *   mount 路径 parse_superblock 刚验过；但 umount 路径距上次校验隔了一整个会话，
 *   期间盘可能被外部工具改写、或发生介质位翻转。若不验就重算回填，等于把一份
 *   已损坏的 SB 重新盖章认证，把事故掩盖成「crc 又 OK 了」。验不过 → 拒写，
 *   盘上留旧时间戳（读侧照样报 mount > umount / 非正常关机），代价只是这次
 *   时间戳没记上——诚实报错优于盖章。
 *
 * 【耐久性：sync_dirty_buffer + 一次 flush 屏障】
 *   sync_dirty_buffer 只等 bio 完成（本仓写路径惯例「返回即落盘」），设备掉电
 *   写缓存里的内容仍可能丢。丢的后果方向是「读到旧值」→ umount 缺失或偏旧 →
 *   被判非正常关机——保守安全方向（宁可误报异常，不误报干净）。但 umount_time
 *   存在的全部意义就是「掉电之后仍可信」，所以再花一次 blkdev_issue_flush
 *   （复用 ravenfs_issue_flush；它对 ro 挂载自身 no-op，而本函数 ro 已在第一道闸
 *   就不进来）。总代价 = 每次挂载/卸载各 1×4K 写 + 1 屏障，正是 #15 写明的运行时
 *   代价（「每次挂载/卸载各 1×4K 写 + crc32 重算」）。
 *
 * 返回 0 = 已写成功，或按纪律本就不该写（readonly）；负值 = 想写没写成。
 * is_mount=false 时调用方必须自行保证「干净卸载」前提（见 put_super 处三重闸）。
 */
static int ravenfs_sb_write_timestamp(struct super_block *sb, bool is_mount)
{
	struct buffer_head *bh;
	struct RavenSuperblock *dsb;
	void *tmp;
	u32 stored, calc;
	int err = 0;

	/* 第一道闸（内置而非只靠调用点）：readonly 挂载一律不写 —— 审计/只读挂载
	 * 不得把 0 哨兵改成非 0，否则「从未挂载 / 从未干净卸载」的诊断信息当场丢失。 */
	if (!ravenfs_sb_writable(sb))
		return 0;

	/* Primary SB = 设备字节 0（与 fill_super 读侧同一块、同一 sb_bread 口径）。
	 * Backup SB 在别的块号上，本函数根本不碰 → 「Backup 不随动」自动成立。 */
	bh = sb_bread(sb, 0);
	if (!bh) {
		pr_err("ravenfs: cannot read superblock to write timestamp\n");
		return -EIO;
	}
	dsb = (struct RavenSuperblock *)bh->b_data;

	/* 正身核验：magic/version 不符 → 这不是（或已不再是）本 FS 的 Primary SB，
	 * 一个字节都不动。版本口径与 parse_superblock 相同：只认 RAVENFS_VERSION。 */
	if (le64_to_cpu(dsb->magic[0]) != (u64)RFS_SB_MAGIC_W0 ||
	    le32_to_cpu(dsb->version) != RAVENFS_VERSION) {
		pr_err("ravenfs: refusing timestamp write (sb magic/version mismatch)\n");
		err = -EINVAL;
		goto out;
	}

	/* 写前重验 crc（口径同 parse_superblock / mkfs / audit / sbinfo：原始 4096B
	 * 拷贝 → 置零 crc32 字段 → 全长重算）。验不过拒写，理由见头注。 */
	stored = le32_to_cpu(dsb->crc32);
	tmp = kmalloc(RAVENFS_BLOCK_SIZE, GFP_KERNEL);
	if (!tmp) {
		err = -ENOMEM;
		goto out;
	}
	memcpy(tmp, bh->b_data, RAVENFS_BLOCK_SIZE);
	memset(tmp + offsetof(struct RavenSuperblock, crc32), 0, sizeof(u32));
	calc = crc32_le(~0U, tmp, RAVENFS_BLOCK_SIZE) ^ ~0U;
	kfree(tmp);
	if (stored != calc) {
		pr_err("ravenfs: refusing timestamp write, sb crc mismatch (stored %08x calc %08x)\n",
		       stored, calc);
		err = -EIO;
		goto out;
	}

	/* 只动本次该动的那一个 u64：born/modify 恒不许 ko 碰，另一个时间戳原样保留
	 * （mount > umount 是「非正常关机可检出」的全部依据，绝不能顺手“修齐”）。 */
	if (is_mount)
		dsb->latest_mount_time = cpu_to_le64(ktime_get_real_seconds());
	else
		dsb->latest_umount_time = cpu_to_le64(ktime_get_real_seconds());

	/* crc32 重算回填：此刻 bh->b_data 已是「最终要落盘的 4096B」，置零 crc 字段
	 * 后全长计算再写回 = mkfs 的 `sb.crc32 = 0; crc32_compute(&sb, 4096)` 同式
	 * （mkfs 对整个 4096B 结构体算，此处对整个 4096B 缓冲算，逐位一致）。 */
	dsb->crc32 = 0;
	dsb->crc32 = cpu_to_le32(crc32_le(~0U, bh->b_data, RAVENFS_BLOCK_SIZE) ^ ~0U);

	mark_buffer_dirty(bh);
	if (sync_dirty_buffer(bh)) {	/* 等 bio 完成（本仓惯例：确定性落盘） */
		pr_err("ravenfs: timestamp write failed (bio)\n");
		err = -EIO;
		goto out;
	}
	RFS_STAT_INC(sb, io_write_ops);		/* SB 自身写（4K）*/
	RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
	/* 再一道屏障过掉电写缓存（头注耐久性段：umount_time 要在掉电后仍可信）。 */
	err = ravenfs_issue_flush(sb);
	if (err)
		pr_err("ravenfs: timestamp flush failed (%d)\n", err);
out:
	brelse(bh);
	return err;
}

/*
 * v2.0 周期同步的 SB 落定：把内存 sbi 里的 inode 数据面四字段刷入 Primary SB，
 * 更新 latest_syn_time，重算 crc32（**最后一步**）。
 *
 * 与 ravenfs_sb_write_timestamp 同口径（直写 bdev 块 0；置零 crc 全长重算；写前
 * 重验 crc；sync_dirty_buffer + flush）。仅动 5 个字段，其余原样保留。
 */
int ravenfs_sb_write_sync(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb ? sb->s_fs_info : NULL;
	struct buffer_head *bh;
	struct RavenSuperblock *dsb;
	void *tmp;
	u32 stored, calc;
	int err = 0;

	if (!sbi)
		return -EINVAL;
	if (!ravenfs_sb_writable(sb))
		return 0;

	bh = sb_bread(sb, 0);
	if (!bh) {
		pr_err("ravenfs: cannot read superblock for sync write\n");
		return -EIO;
	}
	dsb = (struct RavenSuperblock *)bh->b_data;
	if (le64_to_cpu(dsb->magic[0]) != (u64)RFS_SB_MAGIC_W0 ||
	    le32_to_cpu(dsb->version) != RAVENFS_VERSION) {
		pr_err("ravenfs: refusing sync SB write (magic/version mismatch)\n");
		err = -EINVAL;
		goto out;
	}
	stored = le32_to_cpu(dsb->crc32);
	tmp = kmalloc(RAVENFS_BLOCK_SIZE, GFP_KERNEL);
	if (!tmp) {
		err = -ENOMEM;
		goto out;
	}
	memcpy(tmp, bh->b_data, RAVENFS_BLOCK_SIZE);
	memset(tmp + offsetof(struct RavenSuperblock, crc32), 0, sizeof(u32));
	calc = crc32_le(~0U, tmp, RAVENFS_BLOCK_SIZE) ^ ~0U;
	kfree(tmp);
	if (stored != calc) {
		pr_err("ravenfs: refusing sync SB write, crc mismatch (stored %08x calc %08x)\n",
		       stored, calc);
		err = -EIO;
		goto out;
	}

	dsb->inode_data_ctl = cpu_to_le64(sbi->inode_data_ctl);
	dsb->inode_data_plane_pieces_count =
		cpu_to_le64(sbi->inode_data_plane_pieces_count);
	dsb->inode_data_fr_ctl = cpu_to_le64(sbi->inode_data_fr_ctl);
	dsb->inode_data_fr_count = cpu_to_le64(sbi->inode_data_fr_count);
	dsb->latest_syn_time = cpu_to_le64(ktime_get_real_seconds());

	dsb->crc32 = 0;
	dsb->crc32 = cpu_to_le32(crc32_le(~0U, bh->b_data, RAVENFS_BLOCK_SIZE) ^ ~0U);

	mark_buffer_dirty(bh);
	if (sync_dirty_buffer(bh)) {
		pr_err("ravenfs: sync SB write failed (bio)\n");
		err = -EIO;
		goto out;
	}
	RFS_STAT_INC(sb, io_write_ops);
	RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
	err = ravenfs_issue_flush(sb);
out:
	brelse(bh);
	return err;
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
		RFS_STAT_INC(sb, err_sb_read);	/* 盘/设备健康 */
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
	/* v2.0：根 inode 索引 = 契约级硬编码 RFS_ROOT_INODE_IDX(=0) */
	root = ravenfs_iget(sb, RFS_ROOT_INODE_IDX);
	if (IS_ERR(root))
		return PTR_ERR(root);

	sb->s_root = d_make_root(root);
	if (!sb->s_root)
		return -ENOMEM;

	/*
	 * 卷生命周期时间戳 mount_time（Docs/disk_layout_v1.1.md #15）：非 readonly
	 * 挂载写 now；readonly 由 helper 第一道闸跳过（审计挂载不污染 0 哨兵）。
	 *
	 * 【为什么卡在这个位置——d_make_root 之后、debugfs/pr_info 之前】
	 *   · 更早（iget/d_make_root 之前）：s_root 尚未建立时失败不走 put_super，
	 *     但「mount 写了 / 挂载却失败」与「mount 没写 / 挂载成功」会各留一半状态；
	 *   · 更晚（debugfs 之后）：对外宣布挂载成功（debugfs 目录 + pr_info）之后
	 *     再失败，回滚面更大，且 stats 里可能已有本次会话的计数；
	 *   · 本处写失败直接 return err = 挂载失败：rw 挂载连 4K SB 都写不进，
	 *     元数据写路径必然也坏 → 早失败符合「拒绝而非静默漏」（M1 诚实边界口径）。
	 *
	 * 【与 put_super 的哨兵耦合（不用往 ravenfs_sb_info 加字段的代价）】
	 *   本函数返错时 s_root 已置位 → VFS 仍会调 put_super。sbi->mounted_at_ns
	 *   在**本行写成功之后**才被置（kzalloc 清零 → 只有走到这里才非 0），put_super
	 *   用它非 0 作「本次挂载的 mount_time 已写成功」的判据：写失败的回滚路径
	 *   不会开出「只有 umount 没有 mount」的错序收据，改动也锁死在 super.c 单文件。
	 */
	err = ravenfs_sb_write_timestamp(sb, true);
	if (err)
		return err;

	/* §8.2：debugfs 出口 `<s_id>/stats`；kernel.debugfs=off 时 create 返回 ERR_PTR
	 * → 忽略（IS_ERR_OR_NULL 守卫，不挡挂载）。 */
	sbi->mounted_at_ns = ktime_get_ns();
	RFS_STAT_INC(sb, life_mount);
	sbi->dbg_dir = debugfs_create_dir(sb->s_id,
			IS_ERR_OR_NULL(ravenfs_dbg_root) ? NULL : ravenfs_dbg_root);
	if (!IS_ERR_OR_NULL(sbi->dbg_dir))
		debugfs_create_file("stats", 0444, sbi->dbg_dir, sb, &ravenfs_stats_fops);

	/* D-11：procfs 公开出口 `/proc/fs/ravenfs/<s_id>/stats`（0444，人人可读）。
	 * 与 debugfs 同一 `ravenfs_stats_show`（数据源单一；data=sb → m->private=sb）。
	 * 守卫：proc 根缺失（极罕见，init 已建）→ 只少公开面，不挡挂载（同 debugfs 口径）。*/
	if (ravenfs_proc_root) {
		sbi->proc_dir = proc_mkdir(sb->s_id, ravenfs_proc_root);
		if (sbi->proc_dir)
			proc_create_single_data("stats", 0444, sbi->proc_dir,
						ravenfs_stats_show, sb);
	}

	pr_info("ravenfs: mounted blocks=%llu inodes=%llu pieces=%llu root_idx=%llu\n",
		sbi->total_blocks, sbi->ra_inode_count,
		RFS_IDP_PIECE_COUNT(sbi->inode_data_plane_pieces_count),
		(unsigned long long)RFS_ROOT_INODE_IDX);
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
	/* §8.2：debugfs 根目录（`/sys/kernel/debug/ravenfs`）；失败忽略 */
	ravenfs_dbg_root = debugfs_create_dir("ravenfs", NULL);
	/* D-11：procfs 公开根 `/proc/fs/ravenfs`（`proc_mkdir("fs/ravenfs", NULL)`
	 * 会创建中间目录 fs）；失败忽略（仅少公开出口，不挡模块装载）。*/
	ravenfs_proc_root = proc_mkdir("fs/ravenfs", NULL);
	pr_info("ravenfs: module loaded\n");
	return 0;
}

static void __exit ravenfs_exit(void)
{
	debugfs_remove_recursive(ravenfs_dbg_root);
	ravenfs_dbg_root = NULL;
	proc_remove(ravenfs_proc_root);
	ravenfs_proc_root = NULL;
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

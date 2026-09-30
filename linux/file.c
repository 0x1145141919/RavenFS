// SPDX-License-Identifier: GPL-2.0
/*
 * file.c —— RavenFS ko：文件数据路径的 a_ops / read_iter / write_iter 壳（M4.1 读 + M4.2 写）
 *
 * 依据：Docs/linux/ravenfs-linux-ko-M4-data-spec.md
 *   §2.1（file_operations 挂点）、§2.2（iomap 家族 a_ops）、§2.3（writepages/writeback_ops）、
 *   §5（mmap 前置 = read_folio）、§6（读粒度 = iomap 默认 D-M4-7）、§8（回写 / 写序 / 崩溃分析）。
 * 惯例参照：fs/zonefs/file.c（最小 iomap 接线）、fs/xfs/xfs_aops.c（a_ops 家族 + writeback_range）。
 *
 * 纪律：
 *   · **绝不挂 write_begin/write_end**（iomap 不需要；spec §1.1/§2.2）。
 *   · read_folio 是 mmap 的硬前置（D-M4-1）——无它 generic_file_mmap_prepare 直接 -ENOEXEC。
 *   · `write(2)`/`fallocate` **写时立刻分配**（D-M4-6 不变：位图纯内存零 I/O，ENOSPC 即暴露）。
 *   · mmap 写 fault **不分配**：改用 generic_file_vm_ops（filemap_page_mkwrite 只标脏），
 *     分配移到**回写落点** —— writeback_range 查到 HOLE 即调 iomap_begin(IOMAP_WRITE)
 *     真分配（**2026-09-28 用户裁，修订 D-M4-12**；O-13 保护不丢：洞在 bio 提交前被分配）。
 *     接受的代价：回写期 ENOSPC → iomap 清脏位 + mapping_error（fsync 报错）丢数据；
 *     /mnt/huge_data 空间大，风险声明接受（裁决记录在 spec D-M4-12 行）。
 *   · 无 direct_IO（O_DIRECT 后置 D-M4-5：open(O_DIRECT) 直接 -EINVAL）。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/uio.h>
#include <linux/writeback.h>
#include <linux/timekeeping.h>
#include <linux/falloc.h>
#include <linux/posix_acl.h>	/* M6：posix_acl_chmod（chmod 同步 access ACL mask）*/

#include "ravenfs.h"
#include "ravenfs_file.h"
#include "ravenfs_iomap.h"
#include "ravenfs_extent.h"
#include "ravenfs_cache.h"
#include "ravenfs_mdata_cache.h"
#include "ravenfs_bitmap.h"

/* inline 态字节上限（不变量 22）：size ≤ 160 ⇒ 内容在 inode.inline_data */
#define RAVENFS_INLINE_BYTES	160U

/* 缺页 / read(2) 的页填充 = iomap 默认 bio 读（走 iomap_bio_read_ops） */
static int ravenfs_read_folio(struct file *file, struct folio *folio)
{
	iomap_bio_read_folio(folio, &ravenfs_iomap_ops);
	return 0;
}

static void ravenfs_readahead(struct readahead_control *rac)
{
	iomap_bio_readahead(rac, &ravenfs_iomap_ops);
}

/*
 * 回写映射：wpc->iomap 复用（跨越边界才重查）。
 * **2026-09-28 用户裁（修订 D-M4-12）：mmap 路径 = 回写落点分配** ——
 * 查到 HOLE 即调写向分支真分配（IOMAP_WRITE）。O-13「洞写静默丢数据」的
 * 保护由 mkwrite 挪到这里：分配发生在 bio 提交之前，不再跳过。
 * 副作用：回写期 ENOSPC → iomap 清脏位 + mapping_error（spec D-M4-12 修订注记）。
 */
static ssize_t ravenfs_writeback_range(struct iomap_writepage_ctx *wpc,
				       struct folio *folio, u64 pos,
				       unsigned int len, u64 end_pos)
{
	struct inode *inode = wpc->inode;
	struct iomap srcmap;

	if (pos < wpc->iomap.offset ||
	    pos >= wpc->iomap.offset + wpc->iomap.length) {
		int err = ravenfs_iomap_begin(inode, pos, len, IOMAP_WRITE,
					      &wpc->iomap, &srcmap);

		if (err)
			return err;
	}
	/* inline 态不产生脏页（走 write_end_inline）；若见之，拒绝而非静默错写 */
	if (wpc->iomap.type != IOMAP_MAPPED && wpc->iomap.type != IOMAP_HOLE)
		return -EIO;

	return iomap_add_to_ioend(wpc, folio, pos, end_pos, len);
}

static const struct iomap_writeback_ops ravenfs_writeback_ops = {
	.writeback_range	= ravenfs_writeback_range,
	.writeback_submit	= iomap_ioend_writeback_submit,
};

/*
 * inline 文件（size ≤ 160）专属回写路径（**D-M4-14 / O-14**）。
 *
 * iomap 核心**不支持** INLINE 态的回写：`iomap_writeback_range` 只认 MAPPED/HOLE，
 * 若此处落到 `ravenfs_writeback_range` 见 IOMAP_INLINE 会返 -EIO（`msync` 硬失败，O-14）。
 * 故对 inline 文件**绕开 iomap_writepages**：直接遍历脏 folio，把 `[0, min(size,160))`
 * 字节回灌盘记录 `inline_data` + 标行脏（inode_cache 会话写回）；**不改 size**、
 * **不升格**、超 size 的字节**静默丢弃**（Linux 常规 —— mmap 写落在 size 之外不落盘）。
 */
static int ravenfs_writepages_inline(struct inode *inode, struct writeback_control *wbc)
{
	struct address_space *mapping = inode->i_mapping;
	struct super_block *sb = inode->i_sb;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;
	struct raven_Inode_t *r;
	struct folio *folio = NULL;
	u64 limit = (u64)i_size_read(inode);
	bool dirtied = false;
	int error = 0;

	if (limit > RAVENFS_INLINE_BYTES)
		limit = RAVENFS_INLINE_BYTES;
	if (limit == 0)
		return 0;		/* 空文件：任何 mmap 写都在 size 之外 → 全部丢弃 */

	ses = ravenfs_session_get(sb, inode->i_ino);
	if (IS_ERR(ses))
		return PTR_ERR(ses);
	r = ravenfs_session_inode(ses);

	while ((folio = writeback_iter(mapping, wbc, folio, &error))) {
		u64 fpos = folio_pos(folio);
		u64 fsize = folio_size(folio);

		folio_start_writeback(folio);
		/* 仅持久化 [fpos, min(fpos+fsize, limit)) ∩ [0, size) */
		if (fpos < limit) {
			u64 end = min(fpos + fsize, limit);
			void *addr = kmap_local_folio(folio, 0);

			memcpy(r->inline_data + fpos, addr, end - fpos);
			kunmap_local(addr);
			dirtied = true;
		}
		folio_unlock(folio);
		folio_end_writeback(folio);
	}

	if (dirtied) {
		RFS_ROW_MARK_DIRTY(ses->row);
		atomic64_set(&ses->row->acc_stamp, ktime_get_ns());
	}
	return error;
}

static int ravenfs_writepages(struct address_space *mapping,
			      struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct iomap_writepage_ctx wpc = {
		.inode		= inode,
		.wbc		= wbc,
		.ops		= &ravenfs_writeback_ops,
	};

	/* inline 文件（size ≤ 160）走专属路径（D-M4-14）；extent/direct 走 iomap */
	if (i_size_read(inode) <= RAVENFS_INLINE_BYTES)
		return ravenfs_writepages_inline(inode, wbc);

	return iomap_writepages(&wpc);
}

/* iomap 家族 a_ops（spec §2.2，7.2.3 核实版）：无 write_begin/write_end */
const struct address_space_operations ravenfs_aops = {
	.read_folio		= ravenfs_read_folio,
	.readahead		= ravenfs_readahead,
	.writepages		= ravenfs_writepages,
	.dirty_folio		= iomap_dirty_folio,
	.release_folio		= iomap_release_folio,
	.invalidate_folio	= iomap_invalidate_folio,
	.migrate_folio		= filemap_migrate_folio,
	.is_partially_uptodate	= iomap_is_partially_uptodate,
	.error_remove_folio	= generic_error_remove_folio,
};

/*
 * mmap 前置：**改挂 generic_file_vm_ops**（2026-09-28 用户裁，修订/退役 D-M4-12；
 * f23b933 的自写 mkwrite 路径删除）。其 page_mkwrite = filemap_page_mkwrite
 * **只标脏不分配**；块分配推迟到回写落点（ravenfs_writeback_range 调
 * iomap_begin(IOMAP_WRITE)）。
 * 动机：/mnt/huge_data 空间大 → 晚 ENOSPC（回写期）风险声明接受；
 * O-13 保护不丢 —— 洞写在 bio 提交前被分配，不会走「HOLE 跳过」。
 * write(2)/fallocate 仍写时立刻分配（D-M4-6 不变）。
 * read_folio 仍是硬前置（D-M4-1）——由 a_ops 保证。
 */
int ravenfs_file_mmap_prepare(struct vm_area_desc *desc)
{
	RFS_STAT_INC(file_inode(desc->file)->i_sb, op_mmap);
	file_accessed(desc->file);
	return generic_file_mmap_prepare(desc);
}

/*
 * buffered 读入口（spec §2.1：自写壳）。M4.1 直接委派 generic_file_read_iter
 * （页缓存 + iomap read_folio/readahead 全在框架内）；后续 atime/只读特判可在此加。
 */
ssize_t ravenfs_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct super_block *sb = file_inode(iocb->ki_filp)->i_sb;
	ssize_t ret;

	RFS_STAT_INC(sb, op_read_iter);
	if (ravenfs_is_shutdown(sb)) {
		RFS_STAT_INC(sb, err_shutdown_hits);
		return -EIO;
	}
	ret = generic_file_read_iter(iocb, iter);
	if (ret > 0)
		RFS_STAT_ADD(sb, op_read_bytes, ret);	/* IO 放大分母 */
	return ret;
}

/*
 * buffered 写入口（spec §2.1/§2.3：自写壳 → generic_write_checks → iomap_file_buffered_write）。
 * 立刻分配（D-M4-6）落在 ravenfs_iomap_begin(WRITE)；ENOSPC 经此直接暴露给 write(2)。
 */
ssize_t ravenfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	RFS_STAT_INC(inode->i_sb, op_write_iter);
	if (ravenfs_is_shutdown(inode->i_sb)) {
		RFS_STAT_INC(inode->i_sb, err_shutdown_hits);
		return -EIO;
	}
	if (unlikely(IS_IMMUTABLE(inode) || IS_APPEND(inode)))
		return -EPERM;
	if (sb_rdonly(inode->i_sb))
		return -EROFS;			/* readonly 拒写 */
	if (!iov_iter_count(from))
		return 0;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out;
	ret = file_remove_privs(iocb->ki_filp);	/* 清 S_ISUID/S_ISGID（generic_write 惯例） */
	if (ret)
		goto out;

	/* 时间戳义务（R-MD-06）：自写壳绕开了 __generic_file_write_iter，
	 * 其 file_update_time（mm/filemap.c:4462）不再免费 ⇒ 必须自己打
	 * mtime/ctime（atime 走 file_accessed，read 路径免费）。 */
	ret = file_update_time(iocb->ki_filp);
	if (ret)
		goto out;

	ret = iomap_file_buffered_write(iocb, from, &ravenfs_iomap_ops, NULL, NULL);
	if (ret > 0)
		RFS_STAT_ADD(inode->i_sb, op_write_bytes, ret);	/* IO 放大分母 */
out:
	inode_unlock(inode);
	return ret;
}

/* ══════════════════════════════════════════════════════════════════
 * M4.3：truncate / punch_hole / fallocate（+ setattr 壳）
 *
 * 依据：Docs/punch_truncate_pread_spec.md §5（punch）/§6（truncate）/§7（旧根延迟 free）；
 *       Docs/linux/ravenfs-linux-ko-M4-data-spec.md §8（提交点/写序）、§11（M4.3）、
 *       D-M4-11（拒 `FALLOC_FL_KEEP_SIZE` 纯预留）、D-M4-14（inline 语义）；
 *       Docs/disk_layout_v1.1.md §10 不变量 22–25。
 * 纪律：inode **一次写** = 唯一提交点（size + data_root + direct_bit [+ inline_data]），
 *       **先 inode 后 free**（旧根 / 数据块）——严禁先 free 再翻 inode（悬挂）。
 * ══════════════════════════════════════════════════════════════════ */
#define RAVENFS_BLK_SHIFT	12
#define RFS_EXT_DEPTH_MAX_PUB	64u	/* 深度上限（防成环） */
#define RFS_NBLK(b)		(((u64)(b) + RAVENFS_BLOCK_SIZE - 1) >> RAVENFS_BLK_SHIFT)
#define RFS_IS_BLK_ALIGN(b)	((((u64)(b)) & (RAVENFS_BLOCK_SIZE - 1)) == 0)
#define RFS_SES_PUT(_s)	do { if (!IS_ERR_OR_NULL(_s)) { ravenfs_session_put(_s); (_s) = NULL; } } while (0)


/* 待释放物理块区间（延迟 free 账本：**先 inode 后 free**）。
 * 堆链表、无上限（truncate/punch_hole = 低频路径，容忍少量分配）；相邻区间自动合并。 */
struct rfs_frerange {
	struct list_head	link;
	u64		addr;
	u64		len;
};

struct rfs_freelist {
	struct list_head	ranges;
	int		n;
};

static struct rfs_freelist *rfs_fl_new(void)
{
	struct rfs_freelist *fl = kmalloc(sizeof(*fl), GFP_KERNEL);

	if (!fl)
		return NULL;
	INIT_LIST_HEAD(&fl->ranges);
	fl->n = 0;
	return fl;
}

static void rfs_fl_put(struct rfs_freelist *fl)
{
	struct rfs_frerange *r, *tmp;

	if (!fl)
		return;
	list_for_each_entry_safe(r, tmp, &fl->ranges, link) {
		list_del(&r->link);
		kfree(r);
	}
	kfree(fl);
}

/* 记一段待释放物理块 [addr, addr+len)；与前/后相邻区间自动合并 */
static void rfs_fl_add(struct rfs_freelist *fl, u64 addr, u64 len)
{
	struct rfs_frerange *r, *nr;

	if (len == 0)
		return;

	list_for_each_entry(r, &fl->ranges, link) {
		if (r->addr + r->len == addr) {		/* 新段接在 r 尾 */
			r->len += len;
			/* 桥接：接上后若又贴住后继 → 合并 */
			if (!list_is_last(&r->link, &fl->ranges)) {
				struct rfs_frerange *nx = list_next_entry(r, link);

				if (r->addr + r->len == nx->addr) {
					r->len += nx->len;
					list_del(&nx->link);
					kfree(nx);
					fl->n--;
				}
			}
			return;
		}
		if (addr + len == r->addr) {		/* 新段接在 r 头 */
			r->addr = addr;
			r->len += len;
			return;
		}
	}

	nr = kmalloc(sizeof(*nr), GFP_KERNEL);
	if (!nr) {
		/* 极端 OOM：宁可泄漏（fsck 可回收）也不破坏「先 inode 后 free」纪律 */
		WARN_ON_ONCE(1);
		return;
	}
	nr->addr = addr;
	nr->len = len;
	list_add_tail(&nr->link, &fl->ranges);
	fl->n++;
}

static void rfs_fl_free(struct super_block *sb, struct rfs_freelist *fl)
{
	struct rfs_frerange *r;

	list_for_each_entry(r, &fl->ranges, link)
		ravenfs_block_free(sb, r->addr, r->len);
}

/* 释放一棵区段树的全部节点块 + 收集数据块（commit 之后调用） */
static int rfs_release_tree_rec(struct super_block *sb, u64 root, struct rfs_freelist *fl,
				u32 depth)
{
	struct Bptree_meta_t *m;
	u8 *buf;
	u32 cnt, inner, i;
	int err;

	if (root == (u64)PTR_INVALID || depth > RFS_EXT_DEPTH_MAX_PUB)
		return 0;
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	err = ravenfs_read_node(sb, root, buf);
	if (err) {
		kfree(buf);
		return 0;			/* 读不到 → 交给 fsck */
	}
	m = (void *)buf;
	if (m->magics[0] != BPTREE_MAGIC1 || m->magics[1] != BPTREE_MAGIC2 ||
	    m->magics[2] != BPTREE_MAGIC3 || m->magics[3] != BPTREE_MAGIC4) {
		kfree(buf);
		return 0;
	}
	cnt = RFS_PP_COUNT(m->parent_ptr);
	inner = RFS_PP_IS_INNER(m->parent_ptr);
	if (cnt > 338) {
		kfree(buf);
		return 0;
	}
	if (inner) {
		struct Bptree_extent_inner_node_t *nd = (void *)buf;

		for (i = 0; i < cnt; i++)
			rfs_release_tree_rec(sb, RFS_PTR_ADDR(nd->extents[i].sub_node_ptr),
					     fl, depth + 1);
	} else {
		struct Bptree_extent_leaf_node_t *lf = (void *)buf;

		for (i = 0; i < cnt; i++)
			rfs_fl_add(fl, RFS_PTR_ADDR(lf->extents[i].block_base),
				   lf->extents[i].length);
	}
	kfree(buf);
	ravenfs_mdata_drop(sb, root);
	ravenfs_block_free(sb, root, 2);
	return 0;
}

/*
 * evict_inode 数据回收（M4 收口）：释放 nlink==0 的 REG/SOFT_SYM 全部外置数据资产。
 * 复用 truncate「缩到 inline」出口的下半段释放链：
 *   · direct（直接区）：释放连续 run [data_root, data_root + ceil(size/4K))；
 *   · extent（区段树）：释放整棵树（节点：逐个 ravenfs_mdata_drop + free；数据块：收集后 free）。
 * **先 inode 后 free**（§10 不变量 25 / handover §6.1）：调用方必须已先清零盘 inode 记录
 * （ravenfs_inode_reclaim）再调用本函数；inline（size ≤ 160）无外置资产，调用方直接跳过。
 * 逆向（先 free 后清 inode）会造出悬挂映射 —— 严禁。OOM 时宁可泄漏（fsck 可回收）。
 */
void ravenfs_release_inode_data(struct super_block *sb, u64 data_root, u64 size, bool direct)
{
	struct rfs_freelist *fl;

	if (data_root == (u64)PTR_INVALID)
		return;
	if (direct) {
		ravenfs_block_free(sb, data_root, RFS_NBLK(size));
		return;
	}
	fl = rfs_fl_new();
	if (!fl)
		return;		/* OOM：交 fsck 回收，绝不反向破坏「先 inode 后 free」 */
	rfs_release_tree_rec(sb, data_root, fl, 0);
	rfs_fl_free(sb, fl);
	rfs_fl_put(fl);
}

/* 提交盘 inode 新态（唯一提交点，**内存态**）：size / data_root / direct_bit [/ inline_data]。
 * 落盘 = inode_cache 的 TTL/周期回写 + sync()（**与 iomap_end 同款**，不在此强制 flush）。
 * “先 inode 后 free”纪律：调用方在此更新缓存行后，才可 free 旧根/数据块；
 * 落盘侧 sync_fs 里 inode 行先于 bitmap → 不悬挂。 */
static int rfs_commit_inode(struct inode *inode, u64 new_size, u64 new_root,
			    int new_direct, const void *inline_src)
{
	struct super_block *sb = inode->i_sb;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;
	struct raven_Inode_t *r;

	ses = ravenfs_session_get(sb, inode->i_ino);
	if (IS_ERR(ses))
		return PTR_ERR(ses);
	r = ravenfs_session_inode(ses);
	r->size = new_size;
	r->data_root = new_root;
	RFS_INO1_SET_DIRECT(*r, new_direct ? 1 : 0);
	if (inline_src)
		memcpy(r->inline_data, inline_src, RAVENFS_INLINE_BYTES);
	RFS_ROW_MARK_DIRTY(ses->row);
	atomic64_set(&ses->row->acc_stamp, ktime_get_ns());
	return 0;	/* 落盘交 TTL/周期回写 + sync（与 iomap_end 同款） */
}

/* 仅推进 size（data_root/direct_bit 由写路径/iomap_end 已置） */
static int rfs_commit_size(struct inode *inode, u64 new_size)
{
	struct super_block *sb = inode->i_sb;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;

	ses = ravenfs_session_get(sb, inode->i_ino);
	if (IS_ERR(ses))
		return PTR_ERR(ses);
	ravenfs_session_inode(ses)->size = new_size;
	RFS_ROW_MARK_DIRTY(ses->row);
	atomic64_set(&ses->row->acc_stamp, ktime_get_ns());
	return 0;
}

/* 读 inode 前 len 字节（页缓存 → iomap 读路径） */
static int rfs_read_inode_bytes(struct inode *inode, void *out, u64 len)
{
	struct address_space *mapping = inode->i_mapping;
	u64 done = 0;

	while (done < len) {
		pgoff_t idx = done >> PAGE_SHIFT;
		u64 off = done & (PAGE_SIZE - 1);
		u64 n = min_t(u64, len - done, PAGE_SIZE - off);
		struct page *pg = read_mapping_page(mapping, idx, NULL);
		void *kaddr;

		if (IS_ERR(pg))
			return PTR_ERR(pg);
		kaddr = kmap_local_page(pg);
		memcpy((char *)out + done, (char *)kaddr + off, n);
		kunmap_local(kaddr);
		put_page(pg);
		done += n;
	}
	return 0;
}

/* 逻辑块是否已映射（direct: < ceil(size)；extent: walk 命中；inline: 恒 true） */
static bool rfs_block_mapped(struct inode *inode, u64 blk)
{
	struct super_block *sb = inode->i_sb;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;
	const struct raven_Inode_t *r;
	struct extent_t ext;
	u64 next, size;
	bool ret;

	ses = ravenfs_session_get(sb, inode->i_ino);
	if (IS_ERR(ses))
		return false;
	r = ravenfs_session_inode(ses);
	size = r->size;
	if (size <= RAVENFS_INLINE_BYTES)
		return true;
	if (RFS_INO1_GET_DIRECT(r->word1))
		return blk < RFS_NBLK(size);
	ret = (ravenfs_extent_lookup(sb, r->data_root, blk, &ext, &next) == 0);
	return ret;
}

/* 就地清零**已映射**逻辑块内的字节区间（不分配洞；走 iomap zero → 页缓存 + 回写） */
static int rfs_zero_mapped_bytes(struct inode *inode, u64 off, u64 end)
{
	while (off < end) {
		u64 blk = off >> RAVENFS_BLK_SHIFT;
		u64 bend = min_t(u64, end, (blk + 1) << RAVENFS_BLK_SHIFT);
		bool did = false;
		int err;

		if (rfs_block_mapped(inode, blk)) {
			err = iomap_zero_range(inode, off, bend - off, &did,
					       &ravenfs_iomap_ops, NULL, NULL);
			if (err)
				return err;
		}
		off = bend;
	}
	return 0;
}

/* ── 区段树：删除逻辑块域 [fb0, fb1)（逐记录 loose）；返回可能更迭的新根 ── */
static int rfs_unmap_range(struct inode *inode, u64 *root_io, u64 fb0, u64 fb1,
			   struct rfs_freelist *fl, u64 *roots, int *nroots)
{
	struct super_block *sb = inode->i_sb;
	u64 root = *root_io, b = fb0;

	while (b < fb1) {
		struct extent_t ext, cut;
		u64 next, s, e, phys, old_root = (u64)PTR_INVALID;
		int err;

		err = ravenfs_extent_lookup(sb, root, b, &ext, &next);
		if (err < 0)
			return err;
		if (err == 1) {			/* 洞 */
			if (next == 0 || next >= fb1)
				break;
			b = next;
			continue;
		}
		s = max_t(u64, ext.fblkbase, b);
		e = min_t(u64, ext.fblkbase + ext.length, fb1);
		phys = RFS_PTR_ADDR(ext.block_base) + (s - ext.fblkbase);
		cut.length = e - s;
		cut.fblkbase = s;
		cut.block_base.v = RFS_PTR_PACK(0, phys);
		rfs_fl_add(fl, phys, e - s);
		err = ravenfs_extent_unmap_loose(sb, inode->i_ino, &root, &cut, &old_root);
		if (err && err != -ENOENT)
			return err;
		if (old_root != (u64)PTR_INVALID && *nroots < 64)
			roots[(*nroots)++] = old_root;
		b = e;
	}
	*root_io = root;
	return 0;
}

/* 出口坍缩判定：根叶 && count==1 && 单条覆盖 [0,ceil) → 可缩 direct（返回 base） */
static int rfs_extent_try_collapse(struct super_block *sb, u64 root, u64 ceil, u64 *base_out)
{
	struct Bptree_meta_t *m;
	struct Bptree_extent_leaf_node_t *lf;
	u8 *buf;
	u32 cnt, inner, rootf;
	int ret = 0;

	if (root == (u64)PTR_INVALID)
		return 0;
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return 0;
	if (ravenfs_read_node(sb, root, buf))
		goto out;
	m = (void *)buf;
	if (m->magics[0] != BPTREE_MAGIC1 || m->magics[1] != BPTREE_MAGIC2 ||
	    m->magics[2] != BPTREE_MAGIC3 || m->magics[3] != BPTREE_MAGIC4)
		goto out;
	cnt = RFS_PP_COUNT(m->parent_ptr);
	inner = RFS_PP_IS_INNER(m->parent_ptr);
	rootf = RFS_PP_IS_ROOT(m->parent_ptr);
	if (inner || !rootf || cnt != 1)
		goto out;
	lf = (void *)buf;
	if (lf->extents[0].fblkbase == 0 && lf->extents[0].length == ceil) {
		*base_out = RFS_PTR_ADDR(lf->extents[0].block_base);
		ret = 1;
	}
out:
	kfree(buf);
	return ret;
}

/* ── punch_hole：KEEP_SIZE，size 不变；整块域释放 + 边缘残块就地清零 ── */
static int rfs_punch_hole(struct inode *inode, loff_t off, loff_t len)
{
	struct super_block *sb = inode->i_sb;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;
	struct raven_Inode_t *r;
	u64 size, clamp_end, db_start, db_end, oldceil, head_end, tail_start;
	bool direct;
	int err = 0;

	ses = ravenfs_session_get(sb, inode->i_ino);
	if (IS_ERR(ses))
		return PTR_ERR(ses);
	r = ravenfs_session_inode(ses);
	size = r->size;
	if ((u64)off >= size)
		return 0;			/* P3：offset ≥ size → no-op */

	clamp_end = min_t(u64, (u64)off + (u64)len,
			  ((size + RAVENFS_BLOCK_SIZE - 1) & ~(u64)(RAVENFS_BLOCK_SIZE - 1)));
	if (clamp_end <= (u64)off)
		return 0;

	/* inline：size ≤ 160，纯内联字节清零（无块可释） */
	if (size <= RAVENFS_INLINE_BYTES) {
		memset(r->inline_data + off, 0, clamp_end - (u64)off);
		RFS_ROW_MARK_DIRTY(ses->row);
		atomic64_set(&ses->row->acc_stamp, ktime_get_ns());
		RFS_SES_PUT(ses);
		truncate_pagecache_range(inode, off, clamp_end - 1);	/* 失效陈旧页缓存 */
		return 0;
	}

	direct = RFS_INO1_GET_DIRECT(r->word1) != 0;
	oldceil = RFS_NBLK(size);
	db_start = ((u64)off + RAVENFS_BLOCK_SIZE - 1) >> RAVENFS_BLK_SHIFT;	/* ceil(off/4096) */
	db_end = clamp_end >> RAVENFS_BLK_SHIFT;				/* floor(clamp/4096) */
	head_end = min_t(u64, clamp_end, db_start << RAVENFS_BLK_SHIFT);
	tail_start = max_t(u64, (u64)off, db_end << RAVENFS_BLK_SHIFT);
	RFS_SES_PUT(ses);

	filemap_invalidate_lock(inode->i_mapping);

	if (direct && db_start < db_end) {
		/* direct + 整块域非空 → 物化上树（保 direct 不成立） */
		u64 root = (u64)PTR_INVALID, base;
		struct extent_t e0, e1;
		struct ravenfs_session *s2 = ravenfs_session_get(sb, inode->i_ino);
		struct rfs_freelist *fl;

		if (IS_ERR(s2)) {
			filemap_invalidate_unlock(inode->i_mapping);
			return PTR_ERR(s2);
		}
		base = ravenfs_session_inode(s2)->data_root;
		ravenfs_session_put(s2);
		fl = rfs_fl_new();
		if (!fl) {
			filemap_invalidate_unlock(inode->i_mapping);
			return -ENOMEM;
		}
		err = ravenfs_extent_new_root_leaf(sb, inode->i_ino, &root);
		if (err)
			goto out_free_fl;
		if (db_start > 0) {		/* 保留段 [0, db_start) */
			e0.length = db_start;
			e0.fblkbase = 0;
			e0.block_base.v = RFS_PTR_PACK(0, base);
			err = ravenfs_extent_insert(sb, inode->i_ino, &root, &e0, NULL);
			if (err)
				goto out_free_tree;
		}
		if (db_end < oldceil) {		/* 保留段 [db_end, oldceil) */
			e1.length = oldceil - db_end;
			e1.fblkbase = db_end;
			e1.block_base.v = RFS_PTR_PACK(0, base + db_end);
			err = ravenfs_extent_insert(sb, inode->i_ino, &root, &e1, NULL);
			if (err)
				goto out_free_tree;
		}
		/* 残留字节清零（保留段内就地）*/
		err = rfs_zero_mapped_bytes(inode, off, head_end);
		if (!err)
			err = rfs_zero_mapped_bytes(inode, tail_start, clamp_end);
		if (err)
			goto out_free_tree;
		truncate_pagecache_range(inode, off, clamp_end - 1);
		err = rfs_commit_inode(inode, size, root, 0, NULL);
		if (err)
			goto out_free_tree;
		/* 后置 free：被打洞的整块域物理块 */
		ravenfs_block_free(sb, base + db_start, db_end - db_start);
		rfs_fl_put(fl);
		filemap_invalidate_unlock(inode->i_mapping);
		return 0;

out_free_tree:
		rfs_release_tree_rec(sb, root, fl, 0);
		rfs_fl_free(sb, fl);
		rfs_fl_put(fl);
		filemap_invalidate_unlock(inode->i_mapping);
		return err;
out_free_fl:
		rfs_fl_put(fl);
		filemap_invalidate_unlock(inode->i_mapping);
		return err;
	}

	if (direct) {
		/* 整块域空 → 保 direct，纯字节清零 */
		err = rfs_zero_mapped_bytes(inode, off, head_end);
		if (!err)
			err = rfs_zero_mapped_bytes(inode, tail_start, clamp_end);
		if (!err)
			truncate_pagecache_range(inode, off, clamp_end - 1);
		filemap_invalidate_unlock(inode->i_mapping);
		return err;
	}

	/* extent：逐记录 loose 整块域 + 残块清零 */
	{
		u64 root, roots[64];
		int nroots = 0, i;
		struct ravenfs_session *s2;
		struct rfs_freelist *fl = rfs_fl_new();

		if (!fl) {
			filemap_invalidate_unlock(inode->i_mapping);
			return -ENOMEM;
		}
		s2 = ravenfs_session_get(sb, inode->i_ino);
		if (IS_ERR(s2)) {
			rfs_fl_put(fl);
			filemap_invalidate_unlock(inode->i_mapping);
			return PTR_ERR(s2);
		}
		root = ravenfs_session_inode(s2)->data_root;
		ravenfs_session_put(s2);

		err = rfs_zero_mapped_bytes(inode, off, head_end);
		if (err)
			goto out_inval;
		err = rfs_zero_mapped_bytes(inode, tail_start, clamp_end);
		if (err)
			goto out_inval;
		if (db_start < db_end) {
			err = rfs_unmap_range(inode, &root, db_start, db_end, fl, roots, &nroots);
			if (err)
				goto out_inval;
		}
		truncate_pagecache_range(inode, off, clamp_end - 1);
		err = rfs_commit_inode(inode, size, root, 0, NULL);
		if (err) {
			rfs_fl_put(fl);
			filemap_invalidate_unlock(inode->i_mapping);
			return err;
		}
		for (i = 0; i < nroots; i++) {
			ravenfs_mdata_drop(sb, roots[i]);
			ravenfs_block_free(sb, roots[i], 2);
		}
		rfs_fl_free(sb, fl);
		rfs_fl_put(fl);
		filemap_invalidate_unlock(inode->i_mapping);
		return 0;
out_inval:
		rfs_fl_put(fl);
		filemap_invalidate_unlock(inode->i_mapping);
		return err;
	}
}

/* ── truncate：缩小三出口 / 增大 ── */
static int rfs_truncate(struct inode *inode, loff_t new_size){
	struct super_block *sb = inode->i_sb;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;
	struct raven_Inode_t *r;
	u64 old, newceil, oldceil;
	bool direct;
	u8 tmp[RAVENFS_INLINE_BYTES];
	int err;

	if (new_size < 0)
		return -EINVAL;

	ses = ravenfs_session_get(sb, inode->i_ino);
	if (IS_ERR(ses))
		return PTR_ERR(ses);
	r = ravenfs_session_inode(ses);
	old = r->size;
	if ((u64)new_size == old)
		return 0;

	/* ══════════ 缩小 ══════════ */
	if ((u64)new_size < old) {
		if (old <= RAVENFS_INLINE_BYTES) {	/* inline 收 size */
			memcpy(tmp, r->inline_data, RAVENFS_INLINE_BYTES);
			memset(tmp + new_size, 0, RAVENFS_INLINE_BYTES - (u64)new_size);
			RFS_SES_PUT(ses);
			i_size_write(inode, new_size);
			truncate_pagecache(inode, new_size);
			return rfs_commit_inode(inode, new_size, (u64)PTR_INVALID, 0, tmp);
		}
		direct = RFS_INO1_GET_DIRECT(r->word1) != 0;
		oldceil = RFS_NBLK(old);
		newceil = RFS_NBLK(new_size);

		if ((u64)new_size <= RAVENFS_INLINE_BYTES) {
			/* 出口 1：≤160 **强制回迁 inline**（读回 [0,new) → 提交 inline → 后 free 全资产） */
			u64 data_root = r->data_root;
			bool was_direct = direct;

			RFS_SES_PUT(ses);
			memset(tmp, 0, sizeof(tmp));
			err = rfs_read_inode_bytes(inode, tmp, new_size);
			if (err)
				return err;
			i_size_write(inode, new_size);
			truncate_pagecache(inode, new_size);
			err = rfs_commit_inode(inode, new_size, (u64)PTR_INVALID, 0, tmp);
			if (err)
				return err;
			if (was_direct) {
				ravenfs_block_free(sb, data_root, oldceil);
			} else {
				struct rfs_freelist *fl = rfs_fl_new();

				if (fl) {
					rfs_release_tree_rec(sb, data_root, fl, 0);
					rfs_fl_free(sb, fl);
					rfs_fl_put(fl);
				}
			}
			return 0;
		}

		if (direct) {
			/* 出口 2：direct 缩 run（保 direct） */
			u64 base = r->data_root;

			RFS_SES_PUT(ses);
			filemap_invalidate_lock(inode->i_mapping);
			if (!RFS_IS_BLK_ALIGN(new_size)) {
				err = rfs_zero_mapped_bytes(inode, new_size,
							    newceil << RAVENFS_BLK_SHIFT);
				if (err) {
					filemap_invalidate_unlock(inode->i_mapping);
					return err;
				}
			}
			i_size_write(inode, new_size);
			truncate_pagecache(inode, new_size);
			err = rfs_commit_inode(inode, new_size, base, 1, NULL);
			if (err) {
				filemap_invalidate_unlock(inode->i_mapping);
				return err;
			}
			if (oldceil > newceil)
				ravenfs_block_free(sb, base + newceil, oldceil - newceil);
			filemap_invalidate_unlock(inode->i_mapping);
			return 0;
		}

		/* 出口 3：extent 尾 loose + 出口坍缩判定 */
		{
			u64 root = r->data_root, roots[64], base = 0;
			int nroots = 0, i, collapse;
			struct rfs_freelist *fl = rfs_fl_new();

			RFS_SES_PUT(ses);
			if (!fl)
				return -ENOMEM;
			filemap_invalidate_lock(inode->i_mapping);
			if (oldceil > newceil) {
				err = rfs_unmap_range(inode, &root, newceil, oldceil,
						      fl, roots, &nroots);
				if (err)
					goto out_inval;
			}
			if (!RFS_IS_BLK_ALIGN(new_size)) {
				err = rfs_zero_mapped_bytes(inode, new_size,
							    newceil << RAVENFS_BLK_SHIFT);
				if (err)
					goto out_inval;
			}
			collapse = rfs_extent_try_collapse(sb, root, newceil, &base);
			i_size_write(inode, new_size);
			truncate_pagecache(inode, new_size);
			err = rfs_commit_inode(inode, new_size,
					       collapse ? base : root,
					       collapse ? 1 : 0, NULL);
			if (err) {
				rfs_fl_put(fl);
				filemap_invalidate_unlock(inode->i_mapping);
				return err;
			}
			/* 后置 free：坍缩旧根 + 收缩产生的旧根 + 被删数据块 */
			if (collapse) {
				ravenfs_mdata_drop(sb, root);
				ravenfs_block_free(sb, root, 2);
			}
			for (i = 0; i < nroots; i++) {
				ravenfs_mdata_drop(sb, roots[i]);
				ravenfs_block_free(sb, roots[i], 2);
			}
			rfs_fl_free(sb, fl);
			rfs_fl_put(fl);
			filemap_invalidate_unlock(inode->i_mapping);
			return 0;
out_inval:
			rfs_fl_put(fl);
			filemap_invalidate_unlock(inode->i_mapping);
			return err;
		}
	}

	/* ══════════ 增大 ══════════ */
	if (old <= RAVENFS_INLINE_BYTES) {
		if ((u64)new_size <= RAVENFS_INLINE_BYTES) {
			/* inline 内补零推进 */
			memcpy(tmp, r->inline_data, RAVENFS_INLINE_BYTES);
			memset(tmp + old, 0, (u64)new_size - old);
			RFS_SES_PUT(ses);
			i_size_write(inode, new_size);
			return rfs_commit_inode(inode, new_size, (u64)PTR_INVALID, 0, tmp);
		}
		/* inline 跨 160 → 迁出（假写 [old,new)） */
		RFS_SES_PUT(ses);
		i_size_write(inode, new_size);
		{
			bool did = false;

			err = iomap_zero_range(inode, old, (u64)new_size - old, &did,
					       &ravenfs_iomap_ops, NULL, NULL);
			if (err)
				return err;
		}
		return rfs_commit_size(inode, new_size);
	}

	direct = RFS_INO1_GET_DIRECT(r->word1) != 0;
	RFS_SES_PUT(ses);
	i_size_write(inode, new_size);
	if (direct && RFS_NBLK(new_size) > RFS_NBLK(old)) {
		bool did = false;		/* 跨块 → 分配增长段（可能迁 extent） */

		err = iomap_zero_range(inode, old, (u64)new_size - old, &did,
				       &ravenfs_iomap_ops, NULL, NULL);
		if (err)
			return err;
	}
	/* extent（或 direct 未跨块）：纯 size 推进，零分配（真稀疏） */
	return rfs_commit_size(inode, new_size);
}

/* flags==0 普通预分配：仅为 [off,off+len) 内的**洞**分配块（不动既有数据），抬 size */
static int rfs_alloc_range(struct inode *inode, loff_t off, loff_t len)
{
	struct super_block *sb = inode->i_sb;
	struct ravenfs_session *ses __free(ravenfs_session) = NULL;
	struct raven_Inode_t *r;
	u64 size, end = (u64)off + (u64)len, b, b_end;
	int err = 0;

	ses = ravenfs_session_get(sb, inode->i_ino);
	if (IS_ERR(ses))
		return PTR_ERR(ses);
	r = ravenfs_session_inode(ses);
	size = r->size;

	if (size <= RAVENFS_INLINE_BYTES) {
		u8 tmp[RAVENFS_INLINE_BYTES];

		if (end <= RAVENFS_INLINE_BYTES) {	/* 纯内联：补零 + 抬 size */
			memcpy(tmp, r->inline_data, RAVENFS_INLINE_BYTES);
			if (end > size)
				memset(tmp + size, 0, end - size);
			RFS_SES_PUT(ses);
			return rfs_commit_inode(inode, max_t(u64, size, end),
						(u64)PTR_INVALID, 0, end > size ? tmp : NULL);
		}
		/* 跨 160 → 迁出（假写 [size, end)，保 [0,size) inline 内容） */
		RFS_SES_PUT(ses);
		i_size_write(inode, end);
		{
			bool did = false;

			err = iomap_zero_range(inode, size, end - size, &did,
					       &ravenfs_iomap_ops, NULL, NULL);
			if (err)
				return err;
		}
		return rfs_commit_size(inode, end);
	}

	RFS_SES_PUT(ses);
	i_size_write(inode, end);
	b = (u64)off >> RAVENFS_BLK_SHIFT;
	b_end = RFS_NBLK(end);
	for (; b < b_end; b++) {
		bool did = false;

		if (rfs_block_mapped(inode, b))
			continue;			/* 已映射 → 保留既有数据 */
		/* 洞块：整块分配 + 零填（块内不得泄旧数据） */
		err = iomap_zero_range(inode, b << RAVENFS_BLK_SHIFT,
				       RAVENFS_BLOCK_SIZE, &did,
				       &ravenfs_iomap_ops, NULL, NULL);
		if (err)
			return err;
	}
	return rfs_commit_size(inode, end);
}

/* ── file_operations.fallocate（R-FD-07；D-M4-11 拒纯 KEEP_SIZE） ── */
long ravenfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	bool keep;
	int err;

	RFS_STAT_INC(sb, op_fallocate);
	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE))
		return -EOPNOTSUPP;
	if (offset < 0 || len <= 0)
		return -EINVAL;
	if (offset + len < 0 || (u64)(offset + len) > RAVENFS_MAX_FILESIZE)
		return -EFBIG;
	if (ravenfs_is_shutdown(sb)) {
		RFS_STAT_INC(sb, err_shutdown_hits);
		return -EIO;
	}
	if (sb_rdonly(sb))
		return -EROFS;
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;
	/* PUNCH_HOLE 必须配 KEEP_SIZE（ABI；open.c 语义） */
	if ((mode & FALLOC_FL_PUNCH_HOLE) && !(mode & FALLOC_FL_KEEP_SIZE))
		return -EINVAL;
	if ((mode & FALLOC_FL_PUNCH_HOLE) && (mode & FALLOC_FL_ZERO_RANGE))
		return -EINVAL;
	/* D-M4-11：纯 KEEP_SIZE 预留（无 PUNCH/ZERO）→ EOPNOTSUPP */
	keep = (mode & FALLOC_FL_KEEP_SIZE) != 0;
	if (keep && !(mode & (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE)))
		return -EOPNOTSUPP;

	inode_lock(inode);
	if (mode & FALLOC_FL_PUNCH_HOLE) {
		err = rfs_punch_hole(inode, offset, len);
	} else if (mode & FALLOC_FL_ZERO_RANGE) {
		/* ZERO_RANGE：分配 + 零填（无 KEEP_SIZE 时抬 size） */
		bool did = false;

		err = iomap_zero_range(inode, offset, len, &did,
				       &ravenfs_iomap_ops, NULL, NULL);
		if (!err && !keep && (u64)(offset + len) > i_size_read(inode)) {
			i_size_write(inode, offset + len);
			err = rfs_commit_size(inode, offset + len);
		}
	} else {
		/* flags==0 普通预分配：仅分配洞块并抬 size（不动既有数据） */
		err = rfs_alloc_range(inode, offset, len);
	}
	if (!err)
		inode_set_ctime_current(inode);
	inode_unlock(inode);
	return err;
}

/* ── inode_operations.setattr（M4.3 先接 ATTR_SIZE=truncate；M4.4 补 chmod/chown/utimes） ── */
int ravenfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int err;

	RFS_STAT_INC(inode->i_sb, op_setattr);
	if (ravenfs_is_shutdown(inode->i_sb)) {
		RFS_STAT_INC(inode->i_sb, err_shutdown_hits);
		return -EIO;
	}
	err = setattr_prepare(idmap, dentry, attr);
	if (err)
		return err;

	if (attr->ia_valid & ATTR_SIZE) {
		if (!S_ISREG(inode->i_mode))
			return -EINVAL;
		err = rfs_truncate(inode, attr->ia_size);
		if (err)
			return err;
	}
	setattr_copy(idmap, inode, attr);
	/* M6（R-XA-03）：chmod 须同步 access ACL 的 mask/group_obj（posix_acl_chmod）。
	 * 无 access ACL 时它内部返 0（get_inode_acl → NULL）。*/
	if (attr->ia_valid & ATTR_MODE)
		err = posix_acl_chmod(idmap, dentry, inode->i_mode);
	mark_inode_dirty(inode);		/* VFS → 盘记录（mode/uid/gid/times/size）经 write_inode 同步 */
	return err;
}

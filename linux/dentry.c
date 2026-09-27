// SPDX-License-Identifier: GPL-2.0
/*
 * dentry.c —— RavenFS ko M2：dentry B+tree 走树 + 名字（含名字堆长名）
 *
 * 依据：Docs/linux/ravenfs-linux-ko-requirements-spec.md
 *   R-NS-01 lookup（hash 引导下行 + d_splice_alias）
 *   R-NS-11 iterate_shared（cookie = 平坦序号；`. `/`..` 由 VFS 合成）
 *
 * 结构真值：include/disk_struct_v1.2.h；参考 `tools/audit_v1.2/audit_v1.2.cpp`
 * （已验证的 v1.2 走树/名字堆读逻辑，本文件是其内核侧 C 移植）。
 *
 * 【M2 诚实边界】
 *   · readdir cookie = (叶块号, 槽) + 叶链（ptrs[1]/前驱, ptrs[2]/后继）续读：O(log n) 定位 + O(本次产出)。
 *     脏链护栏（magic/非内部/prev 指回+步数上限）；cookie 失效 → 静默结束。
 *   · d_type 逐条探测（icache 快路径 → inode 头读）。
 *   · 未加锁（M2 只读挂载，无并发写）；node latch 模型属 M7。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/dcache.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xxhash.h>
#include <linux/timekeeping.h>
#include <linux/kdev_t.h>		/* new_encode_dev（CHR/BLK rdev） */
#include <linux/iomap.h>		/* R-NS-06 slow symlink：复用 iomap 写路径 */

#include "ravenfs.h"

/* ── 8K 节点 / 跨块字节读 ── */

/* ── 8K 节点读取已移至 mdata_cache.c（走 mdata_piece_cache_mgr + session） ── */

/*
 * 跨块读任意字节（off 为块内偏移）—— **走 mdata 8K 片缓存**（与写路径同源）。
 * 为什么不直接 sb_bread：写路径改了 mdata 缓存但尚未落盘时，直接读盘会读到旧内容
 * （外置名/堆数据“刚写完读不到”）；统一走缓存保证同源可见。
 */
static int ravenfs_read_bytes(struct super_block *sb, u64 fs_blk, u32 off,
			      u32 len, void *out)
{
	u8 *p = out;

	while (len) {
		u64 piece = fs_blk & ~1ULL;
		u32 in_piece = (u32)((fs_blk - piece) * RAVENFS_BLOCK_SIZE + off);
		u32 n;
		struct ravenfs_msession *ms __free(ravenfs_msession) =
			ravenfs_mdata_get(sb, piece);

		if (IS_ERR(ms))
			return PTR_ERR(ms);
		n = min_t(u32, len, RAVENFS_MDATA_NODE_BYTES - in_piece);
		memcpy(p, (char *)ravenfs_msession_node(ms) + in_piece, n);
		p += n;
		len -= n;
		if (in_piece + n >= RAVENFS_MDATA_NODE_BYTES) {
			fs_blk = piece + 2;
			off = 0;
		} else {
			off = in_piece + n;
		}
	}
	return 0;
}

/* 物化 dentry 名字：≤40 内联；>40 从名字堆读尾部（full_char_remain_ptr 给物理位置） */
int ravenfs_ddentry_name(struct super_block *sb, const struct ddentry_t *e,
			       char *out, unsigned int *out_len)
{
	u32 nlen = RFS_PTR_FREE(e->inode_ptr);
	u32 inl = min_t(u32, nlen, (u32)DENTRY_INLINE_NAME_MAX);

	if (nlen == 0 || nlen > RAVENFS_MAX_NAME_LEN)
		return -EIO;
	memcpy(out, e->name, inl);
	if (nlen > DENTRY_INLINE_NAME_MAX) {
		u64 ptr = e->full_char_remain_ptr;
		int err;

		if (ptr == (u64)PTR_INVALID)
			return -EIO;
		err = ravenfs_read_bytes(sb, ptr >> 12, (u32)(ptr & 0xFFF),
					 nlen - inl, out + inl);
		if (err)
			return err;
	}
	*out_len = nlen;
	return 0;
}

unsigned int ravenfs_type_dt(u32 type)
{
	switch (type) {
	case INODE_TYPE_FIFO:		return DT_FIFO;
	case INODE_TYPE_CHR:		return DT_CHR;
	case INODE_TYPE_DIR:		return DT_DIR;
	case INODE_TYPE_BLK:		return DT_BLK;
	case INODE_TYPE_REGULAR:	return DT_REG;
	case INODE_TYPE_SOFT_SYM:	return DT_LNK;
	case INODE_TYPE_SOCK:		return DT_SOCK;
	default:			return DT_UNKNOWN;
	}
}

/* d_type 探测：经 disk_inode_cache_mgr 取盘记录（缓存命中即零盘读，§4.11.1） */
static unsigned int ravenfs_dt(struct super_block *sb, u64 inum)
{
	struct ravenfs_session *ses __free(ravenfs_session) = ravenfs_session_get(sb, inum);

	if (IS_ERR_OR_NULL(ses))
		return DT_UNKNOWN;
	return ravenfs_type_dt(RFS_INO1_GET_TYPE(ravenfs_session_inode(ses)->word1));
}

/* ── lookup：hash 引导下行到叶，再按 (hash,name) 精确匹配 ── */

static int ravenfs_leaf_lookup(struct super_block *sb,
			       const struct Bptree_ddentry_leaf_node_t *leaf,
			       u32 cnt, const char *name, u32 nlen, u64 hash, u64 *ino)
{
	char *nm = kmalloc(RAVENFS_MAX_NAME_LEN, GFP_KERNEL);
	u32 i;

	if (!nm)
		return -ENOMEM;
	*ino = (u64)PTR_INVALID;
	for (i = 0; i < cnt; i++) {
		const struct ddentry_t *e = &leaf->entries[i];
		unsigned int en;
		int c, cmplen;

		if (e->hash > hash)		/* 叶按 hash 严格递增 → 越过即 miss */
			break;
		if (e->hash != hash)
			continue;
		if (ravenfs_ddentry_name(sb, e, nm, &en)) {
			kfree(nm);
			return -EIO;
		}
		cmplen = min_t(int, en, (int)nlen);
		c = memcmp(nm, name, cmplen);
		if (c == 0)
			c = (en < nlen) ? -1 : (en > nlen ? 1 : 0);
		if (c == 0) {
			*ino = RFS_PTR_ADDR(e->inode_ptr);
			break;
		}
		if (c > 0)			/* 已越过（同名 hash 区） */
			break;
	}
	kfree(nm);
	return 0;
}

static int ravenfs_dir_find(struct inode *dir, const char *name, u32 nlen, u64 *ino)
{
	struct super_block *sb = dir->i_sb;
	u64 hash = xxh64(name, nlen, 0);
	u64 blk;
	u32 depth = 0;
	void *buf;
	int err = 0;
	struct ravenfs_session *ses __free(ravenfs_session) = ravenfs_session_get(sb, dir->i_ino);

	*ino = (u64)PTR_INVALID;
	if (IS_ERR(ses))
		return PTR_ERR(ses);
	blk = ravenfs_session_inode(ses)->data_root;	/* 盘记录经 session 取（§4.11.1） */
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	for (;;) {
		const struct Bptree_meta_t *meta;
		u32 cnt, i, inner;

		if (++depth > 32 || blk == (u64)PTR_INVALID) {
			err = -EIO;
			break;
		}
		err = ravenfs_read_node(sb, blk, buf);
		if (err)
			break;
		meta = buf;
		if (meta->magics[0] != DENTRY_MAGIC1 || meta->magics[1] != DENTRY_MAGIC2 ||
		    meta->magics[2] != DENTRY_MAGIC3 || meta->magics[3] != DENTRY_MAGIC4) {
			err = -EIO;
			break;
		}
		cnt = RFS_PP_COUNT(meta->parent_ptr);
		inner = RFS_PP_IS_INNER(meta->parent_ptr);
		if (cnt > 126) {
			err = -EIO;
			break;
		}
		if (!inner) {
			err = ravenfs_leaf_lookup(sb, buf, cnt, name, nlen, hash, ino);
			break;
		}
		{
			const struct Bptree_ddentry_inner_node_t *nd = buf;
			u32 sel = cnt;		/* cnt = 无匹配 */

			for (i = 0; i < cnt; i++) {
				if (hash <= nd->hints[i].hash_max) {
					sel = i;
					break;
				}
			}
			if (sel == cnt)		/* 超出所有 hash_max → miss */
				break;
			blk = RFS_PTR_ADDR(nd->hints[sel].sub_ptr);
		}
	}
	kfree(buf);
	return err;
}

/* ── create / unlink（M5：多级 ddentry 树 walk / insert / delete） ── */

int ravenfs_create(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, umode_t mode, bool excl)
{
	struct super_block *sb = dir->i_sb;
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct ravenfs_session *ts __free(ravenfs_session) = NULL;
	struct raven_Inode_t *dr, *tr;
	struct rfs_dpath path;
	struct timespec64 now;
	struct inode *inode;
	u64 inum, root_blk, hash;
	u64 remain_ptr = (u64)PTR_INVALID;
	u32 nlen = dentry->d_name.len;
	int err;

	if (nlen == 0 || nlen > RAVENFS_MAX_NAME_LEN)
		return -ENAMETOOLONG;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;

	ds = ravenfs_session_get(sb, dir->i_ino);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	dr = ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(dr->word1) != INODE_TYPE_DIR)
		return -ENOTDIR;
	root_blk = dr->data_root;
	if (root_blk == (u64)PTR_INVALID)
		return -EIO;

	/* 多级 walk：查重 + 定位插入位（内层 lower_bound / 叶层 GLB） */
	err = ravenfs_ddentry_walk(sb, dir->i_ino, dentry->d_name.name, nlen, &path);
	if (err)
		return err;
	if (path.hit)
		return -EEXIST;
	hash = xxh64(dentry->d_name.name, nlen, 0);

	inum = ravenfs_inode_alloc(sb);
	if (inum == (u64)PTR_INVALID)
		return -ENOSPC;
	ts = ravenfs_session_get(sb, inum);
	if (IS_ERR(ts)) {
		ravenfs_inode_free(sb, inum);
		return PTR_ERR(ts);
	}
	tr = ravenfs_session_inode(ts);
	now = current_time(dir);
	memset(tr, 0, RAVENFS_INODE_SIZE);
	tr->word1 = RFS_INO1_PACK(INODE_TYPE_REGULAR, 0, 0);
	tr->uid = i_uid_read(dir);
	tr->gid = i_gid_read(dir);
	tr->mode_xattr = RFS_MODE_XATTR_PACK(mode & 0o7777, RFS_XATTR_NONE);
	tr->link_count = 1;
	tr->size = 0;
	tr->data_root = (u64)PTR_INVALID;
	tr->mtime = now.tv_sec;
	tr->m_ns_stamp = (u32)now.tv_nsec;
	tr->ctime = now.tv_sec;
	tr->c_ns_stamp = (u32)now.tv_nsec;
	tr->atime = now.tv_sec;
	tr->a_ns_stamp = (u32)now.tv_nsec;
	RFS_ROW_MARK_DIRTY(ts->row);

	/* 长名（>40B）：尾段外置到目录名字堆 */
	if (nlen > DENTRY_INLINE_NAME_MAX) {
		err = ravenfs_name_heap_alloc(sb, dir->i_ino, nlen, &remain_ptr);
		if (err) {
			ravenfs_inode_reclaim(sb, inum);
			return err;
		}
		err = ravenfs_name_heap_write(sb, nlen, remain_ptr,
					      (const char *)dentry->d_name.name + DENTRY_INLINE_NAME_MAX);
		if (err) {
			ravenfs_name_heap_free(sb, dir->i_ino, nlen, remain_ptr);
			ravenfs_inode_reclaim(sb, inum);
			return err;
		}
	}

	/* 多级叶插入（含叶裂 / 根升长 / 内裂 / hint 维护） */
	err = ravenfs_ddentry_insert(sb, dir->i_ino, &path, dentry->d_name.name,
				     nlen, hash, inum, remain_ptr);
	if (err) {
		if (nlen > DENTRY_INLINE_NAME_MAX)
			ravenfs_name_heap_free(sb, dir->i_ino, nlen, remain_ptr);
		ravenfs_inode_reclaim(sb, inum);
		return err;
	}

	/* 父目录计数维护：size++ / link_count = size+2 */
	dr->size += 1;
	dr->link_count = (u32)dr->size + 2;
	dr->mtime = now.tv_sec;
	dr->m_ns_stamp = (u32)now.tv_nsec;
	dr->ctime = now.tv_sec;
	dr->c_ns_stamp = (u32)now.tv_nsec;
	RFS_ROW_MARK_DIRTY(ds->row);
	dir->i_size = dr->size;
	set_nlink(dir, dr->link_count);
	inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
	inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
	mark_inode_dirty(dir);

	inode = ravenfs_iget(sb, inum);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	return 0;
}

int ravenfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = d_inode(dentry);
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct raven_Inode_t *dr;
	struct rfs_dpath path;
	struct rfs_delinfo di;
	struct timespec64 now;
	u64 root_blk;
	u32 nlen = dentry->d_name.len;
	int err;

	if (nlen == 0 || nlen > RAVENFS_MAX_NAME_LEN)
		return -ENAMETOOLONG;
	ds = ravenfs_session_get(sb, dir->i_ino);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	dr = ravenfs_session_inode(ds);
	root_blk = dr->data_root;
	if (root_blk == (u64)PTR_INVALID)
		return -EIO;
	if (d_is_dir(dentry) && !simple_empty(dentry))
		return -ENOTEMPTY;

	err = ravenfs_ddentry_walk(sb, dir->i_ino, dentry->d_name.name, nlen, &path);
	if (err)
		return err;
	if (!path.hit)
		return -ENOENT;

	memset(&di, 0, sizeof(di));
	di.remain = (u64)PTR_INVALID;
	err = ravenfs_ddentry_delete(sb, dir->i_ino, &path, &di);
	if (err)
		return err;

	now = current_time(dir);
	dr->size -= 1;
	dr->link_count = (u32)dr->size + 2;
	dr->mtime = now.tv_sec;
	dr->m_ns_stamp = (u32)now.tv_nsec;
	dr->ctime = now.tv_sec;
	dr->c_ns_stamp = (u32)now.tv_nsec;
	RFS_ROW_MARK_DIRTY(ds->row);
	dir->i_size = dr->size;
	set_nlink(dir, dr->link_count);
	inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
	inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
	mark_inode_dirty(dir);

	/* 长名尾段：释放堆存储（失败仅告警：条目已移除；孤儿位 audit 不判损） */
	if (di.name_len > DENTRY_INLINE_NAME_MAX) {
		int herr = ravenfs_name_heap_free(sb, dir->i_ino, di.name_len, di.remain);

		if (herr)
			pr_warn("ravenfs: name_heap_free(%u) failed: %d\n", di.name_len, herr);
	}

	/* 目标 inode：nlink--（VFS 后续 evict → ravenfs_inode_reclaim 回收槽位） */
	if (inode) {
		inode_set_ctime(inode, now.tv_sec, now.tv_nsec);
		if (inode->i_nlink)
			inode_dec_link_count(inode);
	}

	/* 目录已变空 → 归还其已空名字堆（ko 版 dir_name_heap_release；大魔王 Phase5 堆回收） */
	if (dr->size == 0)
		ravenfs_dir_name_heap_release(sb, dir->i_ino);
	return 0;
}

/*
 * mkdir：在 dir 下建子目录。子目录**急建空根叶**（v1.1 §10 不变量 11：DIR 的
 * data_root 恒有效），dir inode 的 10 个名字堆描述符显式置未激活哨兵（memset 0 是雷）。
 * 复用与 create 同一条多级 ddentry 树插入路径。
 */
struct dentry *ravenfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			     struct dentry *dentry, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct ravenfs_session *ts __free(ravenfs_session) = NULL;
	struct ravenfs_msession *ms __free(ravenfs_msession) = NULL;
	struct raven_Inode_t *dr, *tr;
	struct Bptree_ddentry_leaf_node_t *lf;
	struct rfs_dpath path;
	struct timespec64 now;
	struct inode *inode;
	u64 inum, root_blk, new_root, hash;
	u64 remain_ptr = (u64)PTR_INVALID;
	u32 nlen = dentry->d_name.len, s;
	int err;

	if (nlen == 0 || nlen > RAVENFS_MAX_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);
	if (!S_ISDIR(dir->i_mode))
		return ERR_PTR(-ENOTDIR);

	ds = ravenfs_session_get(sb, dir->i_ino);
	if (IS_ERR(ds))
		return ERR_CAST(ds);
	dr = ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(dr->word1) != INODE_TYPE_DIR)
		return ERR_PTR(-ENOTDIR);
	root_blk = dr->data_root;
	if (root_blk == (u64)PTR_INVALID)
		return ERR_PTR(-EIO);

	err = ravenfs_ddentry_walk(sb, dir->i_ino, dentry->d_name.name, nlen, &path);
	if (err)
		return ERR_PTR(err);
	if (path.hit)
		return ERR_PTR(-EEXIST);
	hash = xxh64(dentry->d_name.name, nlen, 0);

	inum = ravenfs_inode_alloc(sb);
	if (inum == (u64)PTR_INVALID)
		return ERR_PTR(-ENOSPC);
	new_root = ravenfs_block_alloc(sb, 2, 1);
	if (new_root == (u64)PTR_INVALID) {
		ravenfs_inode_free(sb, inum);
		return ERR_PTR(-ENOSPC);
	}

	/* 子目录空根叶：count=0, root=1, ptr=自身 inum(".")，ptrs[1]=".."=父目录 */
	ms = ravenfs_mdata_get(sb, new_root);
	if (IS_ERR(ms)) {
		ravenfs_block_free(sb, new_root, 2);
		ravenfs_inode_free(sb, inum);
		return ERR_CAST(ms);
	}
	lf = ravenfs_msession_node(ms);
	memset(lf, 0, RAVENFS_NODE_BYTES);
	lf->meta.magics[0] = DENTRY_MAGIC1;
	lf->meta.magics[1] = DENTRY_MAGIC2;
	lf->meta.magics[2] = DENTRY_MAGIC3;
	lf->meta.magics[3] = DENTRY_MAGIC4;
	lf->meta.parent_ptr.v = RFS_PP_PACK(0, 0, 1, inum);
	lf->meta.ptrs[0].v = RFS_PTR_PACK(0, RFS_MASK52);
	lf->meta.ptrs[1].v = RFS_PTR_PACK(0, dir->i_ino);	/* ".." */
	lf->meta.ptrs[2].v = RFS_PTR_PACK(0, RFS_MASK52);
	RFS_ROW_MARK_DIRTY(ms->row);
	ravenfs_mdata_put(ms);
	ms = NULL;

	ts = ravenfs_session_get(sb, inum);
	if (IS_ERR(ts)) {
		ravenfs_block_free(sb, new_root, 2);
		ravenfs_inode_free(sb, inum);
		return ERR_CAST(ts);
	}
	tr = ravenfs_session_inode(ts);
	now = current_time(dir);
	memset(tr, 0, RAVENFS_INODE_SIZE);
	tr->word1 = RFS_INO1_PACK(INODE_TYPE_DIR, 0, 0);
	tr->uid = i_uid_read(dir);
	tr->gid = i_gid_read(dir);
	tr->mode_xattr = RFS_MODE_XATTR_PACK(mode & 0o7777, RFS_XATTR_NONE);
	tr->link_count = 2;		/* size(0) + 2 */
	tr->size = 0;
	tr->data_root = new_root;
	tr->mtime = now.tv_sec;
	tr->m_ns_stamp = (u32)now.tv_nsec;
	tr->ctime = now.tv_sec;
	tr->c_ns_stamp = (u32)now.tv_nsec;
	tr->atime = now.tv_sec;
	tr->a_ns_stamp = (u32)now.tv_nsec;
	for (s = 0; s < 10; s++)
		RFS_NHC_SET(*rfs_dir_heap((struct raven_dir_Inode_t *)tr, s), 0, 0,
			    (u64)NAME_HEAP_INACTIVE, 0, (u64)NAME_HEAP_INACTIVE);
	RFS_ROW_MARK_DIRTY(ts->row);

	if (nlen > DENTRY_INLINE_NAME_MAX) {
		err = ravenfs_name_heap_alloc(sb, dir->i_ino, nlen, &remain_ptr);
		if (err)
			goto fail_root;
		err = ravenfs_name_heap_write(sb, nlen, remain_ptr,
					      (const char *)dentry->d_name.name + DENTRY_INLINE_NAME_MAX);
		if (err) {
			ravenfs_name_heap_free(sb, dir->i_ino, nlen, remain_ptr);
			goto fail_root;
		}
	}

	err = ravenfs_ddentry_insert(sb, dir->i_ino, &path, dentry->d_name.name, nlen,
				     hash, inum, remain_ptr);
	if (err) {
		if (nlen > DENTRY_INLINE_NAME_MAX)
			ravenfs_name_heap_free(sb, dir->i_ino, nlen, remain_ptr);
		goto fail_root;
	}

	dr->size += 1;
	dr->link_count = (u32)dr->size + 2;
	dr->mtime = now.tv_sec;
	dr->m_ns_stamp = (u32)now.tv_nsec;
	dr->ctime = now.tv_sec;
	dr->c_ns_stamp = (u32)now.tv_nsec;
	RFS_ROW_MARK_DIRTY(ds->row);
	dir->i_size = dr->size;
	set_nlink(dir, dr->link_count);
	inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
	inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
	mark_inode_dirty(dir);

	inode = ravenfs_iget(sb, inum);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	d_instantiate(dentry, inode);
	return NULL;		/* 用原 dentry（VFS 约定：非 NULL 才替换） */

fail_root:
	if (nlen > DENTRY_INLINE_NAME_MAX)
		ravenfs_name_heap_free(sb, dir->i_ino, nlen, remain_ptr);
	ravenfs_mdata_drop(sb, new_root);
	ravenfs_block_free(sb, new_root, 2);
	ravenfs_inode_reclaim(sb, inum);
	return ERR_PTR(err);
}

/*
 * rmdir：删空目录。判空用**盘上 size==0**（不用 dcache 的 simple_empty —— 见 spec §4.11.3）。
 * 空校验在树删**之前**（失败零副作用）。删后回收目标目录资源：名字堆全归还 →
 * 树根 2 块 →（目标 inode 槽由 clear_nlink 后的 evict → ravenfs_inode_reclaim 回收）。
 */
int ravenfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct inode *target = d_inode(dentry);
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct ravenfs_session *ts __free(ravenfs_session) = NULL;
	struct raven_Inode_t *dr, *tr;
	struct rfs_dpath path;
	struct rfs_delinfo di;
	struct timespec64 now;
	u64 root_blk, t_root;
	u32 nlen = dentry->d_name.len;
	int err;

	if (nlen == 0 || nlen > RAVENFS_MAX_NAME_LEN)
		return -ENAMETOOLONG;
	if (!target || !S_ISDIR(target->i_mode))
		return -ENOTDIR;

	ds = ravenfs_session_get(sb, dir->i_ino);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	dr = ravenfs_session_inode(ds);
	root_blk = dr->data_root;
	if (root_blk == (u64)PTR_INVALID)
		return -EIO;

	/* 盘上判空（零副作用：失败前不碰树） */
	ts = ravenfs_session_get(sb, target->i_ino);
	if (IS_ERR(ts))
		return PTR_ERR(ts);
	tr = ravenfs_session_inode(ts);
	if (RFS_INO1_GET_TYPE(tr->word1) != INODE_TYPE_DIR)
		return -EINVAL;
	if (tr->size != 0)
		return -ENOTEMPTY;
	t_root = tr->data_root;
	if (t_root == (u64)PTR_INVALID)
		return -EIO;

	err = ravenfs_ddentry_walk(sb, dir->i_ino, dentry->d_name.name, nlen, &path);
	if (err)
		return err;
	if (!path.hit)
		return -ENOENT;

	memset(&di, 0, sizeof(di));
	di.remain = (u64)PTR_INVALID;
	err = ravenfs_ddentry_delete(sb, dir->i_ino, &path, &di);
	if (err)
		return err;

	/* 被删条目自身的名字堆尾段（长名）也要释放（同 unlink；否则堆位泄漏、堆永不归还） */
	if (di.name_len > DENTRY_INLINE_NAME_MAX)
		ravenfs_name_heap_free(sb, dir->i_ino, di.name_len, di.remain);

	/* 目标目录资源回收：名字堆（全归还）→ 树根 2 块 */
	ravenfs_dir_name_heap_release(sb, target->i_ino);
	ravenfs_mdata_drop(sb, t_root);
	ravenfs_block_free(sb, t_root, 2);

	now = current_time(dir);
	dr->size -= 1;
	dr->link_count = (u32)dr->size + 2;
	dr->mtime = now.tv_sec;
	dr->m_ns_stamp = (u32)now.tv_nsec;
	dr->ctime = now.tv_sec;
	dr->c_ns_stamp = (u32)now.tv_nsec;
	RFS_ROW_MARK_DIRTY(ds->row);
	dir->i_size = dr->size;
	set_nlink(dir, dr->link_count);
	inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
	inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
	mark_inode_dirty(dir);

	/* 目标 dir inode：nlink→0 → VFS evict → ravenfs_inode_reclaim 回收槽位+清零记录 */
	clear_nlink(target);
	inode_set_ctime(target, now.tv_sec, now.tv_nsec);

	/* 父目录已变空 → 归还其已空名字堆 */
	if (dr->size == 0)
		ravenfs_dir_name_heap_release(sb, dir->i_ino);
	return 0;
}

/* ── rename（VFS 自做 d_move/d_exchange 与 flag/祖先校验；ko 只需改盘上两条） ── */

/* 目录 size 增减 + link_count=size+2 同步（delta 可负） */
static int rfs_dir_adjust(struct super_block *sb, struct inode *dir, int delta)
{
	struct ravenfs_session *ds __free(ravenfs_session) = ravenfs_session_get(sb, dir->i_ino);
	struct raven_Inode_t *dr;
	struct timespec64 now;

	if (IS_ERR(ds))
		return PTR_ERR(ds);
	dr = ravenfs_session_inode(ds);
	dr->size = (u64)((s64)dr->size + delta);
	dr->link_count = (u32)dr->size + 2;
	now = current_time(dir);
	dr->mtime = now.tv_sec;
	dr->m_ns_stamp = (u32)now.tv_nsec;
	dr->ctime = now.tv_sec;
	dr->c_ns_stamp = (u32)now.tv_nsec;
	RFS_ROW_MARK_DIRTY(ds->row);
	dir->i_size = dr->size;
	set_nlink(dir, dr->link_count);
	inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
	inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
	mark_inode_dirty(dir);
	return 0;
}

/* 目录是否为空（盘上 size==0；不用 dcache） */
static int rfs_dir_is_empty(struct super_block *sb, struct inode *d)
{
	struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, d->i_ino);

	if (IS_ERR(s))
		return PTR_ERR(s);
	return ravenfs_session_inode(s)->size == 0;
}

/* 写目录的盘上父指针（根节点 ptrs[1] = ".."；叶/内根一致） */
static int rfs_dir_set_dotdot(struct super_block *sb, u64 dir_ino, u64 parent_ino)
{
	struct ravenfs_session *ds __free(ravenfs_session) = ravenfs_session_get(sb, dir_ino);
	struct ravenfs_msession *ms __free(ravenfs_msession) = NULL;
	struct Bptree_meta_t *m;
	u64 root;

	if (IS_ERR(ds))
		return PTR_ERR(ds);
	root = ravenfs_session_inode(ds)->data_root;
	if (root == (u64)PTR_INVALID)
		return -EIO;
	ms = ravenfs_mdata_get(sb, root);
	if (IS_ERR(ms))
		return PTR_ERR(ms);
	m = ravenfs_msession_node(ms);
	m->ptrs[1].v = RFS_PTR_PACK(0, parent_ino);
	RFS_ROW_MARK_DIRTY(ms->row);
	return 0;
}

/* 交换两条目的 inode_ptr（EXCHANGE）；两条可能共叶 */
static int rfs_swap_entries(struct super_block *sb, const struct rfs_dpath *op,
			    const struct rfs_dpath *np)
{
	u64 oblk = op->blk[op->depth - 1], nblk = np->blk[np->depth - 1];
	u32 oi = op->cidx[op->depth - 1], ni = np->cidx[np->depth - 1];
	struct ravenfs_msession *oms, *nms = NULL;
	struct Bptree_ddentry_leaf_node_t *ol, *nl;
	u64 tmp;

	oms = ravenfs_mdata_get(sb, oblk);
	if (IS_ERR(oms))
		return PTR_ERR(oms);
	ol = ravenfs_msession_node(oms);
	if (oblk == nblk) {
		tmp = ol->entries[oi].inode_ptr.v;
		ol->entries[oi].inode_ptr.v = ol->entries[ni].inode_ptr.v;
		ol->entries[ni].inode_ptr.v = tmp;
		NODE_DIRTY(oms);
		ravenfs_mdata_put(oms);
		return 0;
	}
	nms = ravenfs_mdata_get(sb, nblk);
	if (IS_ERR(nms)) {
		ravenfs_mdata_put(oms);
		return PTR_ERR(nms);
	}
	nl = ravenfs_msession_node(nms);
	tmp = ol->entries[oi].inode_ptr.v;
	ol->entries[oi].inode_ptr.v = nl->entries[ni].inode_ptr.v;
	nl->entries[ni].inode_ptr.v = tmp;
	NODE_DIRTY(oms);
	NODE_DIRTY(nms);
	ravenfs_mdata_put(nms);
	ravenfs_mdata_put(oms);
	return 0;
}

int ravenfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
		   struct inode *new_dir, struct dentry *new_dentry, unsigned int flags)
{
	struct super_block *sb = old_dir->i_sb;
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	struct rfs_dpath opath, npath;
	struct rfs_delinfo dt, dro;
	struct timespec64 now;
	u32 onlen = old_dentry->d_name.len, nnlen = new_dentry->d_name.len;
	u64 n_remain = (u64)PTR_INVALID;
	bool old_is_dir, new_is_dir = false;
	int err;

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
		return -EINVAL;
	if ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE))
		return -EINVAL;
	if (onlen == 0 || onlen > RAVENFS_MAX_NAME_LEN ||
	    nnlen == 0 || nnlen > RAVENFS_MAX_NAME_LEN)
		return -ENAMETOOLONG;
	if (!old_inode)
		return -ENOENT;
	old_is_dir = S_ISDIR(old_inode->i_mode);

	if (new_inode) {
		new_is_dir = S_ISDIR(new_inode->i_mode);
		if (flags & RENAME_NOREPLACE)
			return -EEXIST;
		if (!(flags & RENAME_EXCHANGE)) {
			if (old_is_dir && !new_is_dir)
				return -ENOTDIR;
			if (!old_is_dir && new_is_dir)
				return -EISDIR;
		}
	} else if (flags & RENAME_EXCHANGE) {
		return -ENOENT;	/* EXCHANGE 需两侧都存在（VFS 应早拦） */
	}

	/* ── EXCHANGE：两名都在，交换 inode_ptr ── */
	if (flags & RENAME_EXCHANGE) {
		err = ravenfs_ddentry_walk(sb, old_dir->i_ino, old_dentry->d_name.name, onlen, &opath);
		if (err)
			return err;
		if (!opath.hit)
			return -ENOENT;
		err = ravenfs_ddentry_walk(sb, new_dir->i_ino, new_dentry->d_name.name, nnlen, &npath);
		if (err)
			return err;
		if (!npath.hit)
			return -ENOENT;
		err = rfs_swap_entries(sb, &opath, &npath);
		if (err)
			return err;
		if (old_is_dir)
			rfs_dir_set_dotdot(sb, old_inode->i_ino, new_dir->i_ino);
		if (new_is_dir)
			rfs_dir_set_dotdot(sb, new_inode->i_ino, old_dir->i_ino);
		/* R-MD-06(b)：条目“改”（EXCHANGE）也要推父目录 mtime/ctime（delta=0 只走时间戳） */
		rfs_dir_adjust(sb, old_dir, 0);
		rfs_dir_adjust(sb, new_dir, 0);
		now = current_time(old_dir);
		inode_set_ctime(old_inode, now.tv_sec, now.tv_nsec);
		mark_inode_dirty(old_inode);
		inode_set_ctime(new_inode, now.tv_sec, now.tv_nsec);
		mark_inode_dirty(new_inode);
		return 0;
	}

	/* ── 非 EXCHANGE：目标先删 → 删旧名 → 插新名 ── */
	if (new_inode) {
		if (new_is_dir) {
			int e = rfs_dir_is_empty(sb, new_inode);

			if (e < 0)
				return e;
			if (!e)
				return -ENOTEMPTY;
		}
		err = ravenfs_ddentry_walk(sb, new_dir->i_ino, new_dentry->d_name.name, nnlen, &npath);
		if (err)
			return err;
		if (!npath.hit)
			return -ENOENT;
		memset(&dt, 0, sizeof(dt));
		dt.remain = (u64)PTR_INVALID;
		err = ravenfs_ddentry_delete(sb, new_dir->i_ino, &npath, &dt);
		if (err)
			return err;
		if (new_is_dir) {
			struct ravenfs_session *t __free(ravenfs_session) =
				ravenfs_session_get(sb, new_inode->i_ino);
			u64 troot = (u64)PTR_INVALID;

			if (!IS_ERR(t))
				troot = ravenfs_session_inode(t)->data_root;
			ravenfs_dir_name_heap_release(sb, new_inode->i_ino);
			if (troot != (u64)PTR_INVALID) {
				ravenfs_mdata_drop(sb, troot);
				ravenfs_block_free(sb, troot, 2);
			}
			clear_nlink(new_inode);
		} else {
			if (dt.name_len > DENTRY_INLINE_NAME_MAX)
				ravenfs_name_heap_free(sb, new_dir->i_ino, dt.name_len, dt.remain);
			if (new_inode->i_nlink)
				inode_dec_link_count(new_inode);
		}
		now = current_time(new_dir);
		inode_set_ctime(new_inode, now.tv_sec, now.tv_nsec);
	}

	err = ravenfs_ddentry_walk(sb, old_dir->i_ino, old_dentry->d_name.name, onlen, &opath);
	if (err)
		return err;
	if (!opath.hit)
		return -ENOENT;
	memset(&dro, 0, sizeof(dro));
	dro.remain = (u64)PTR_INVALID;
	err = ravenfs_ddentry_delete(sb, old_dir->i_ino, &opath, &dro);
	if (err)
		return err;
	if (dro.name_len > DENTRY_INLINE_NAME_MAX)
		ravenfs_name_heap_free(sb, old_dir->i_ino, dro.name_len, dro.remain);

	if (nnlen > DENTRY_INLINE_NAME_MAX) {
		err = ravenfs_name_heap_alloc(sb, new_dir->i_ino, nnlen, &n_remain);
		if (err)
			return err;
		err = ravenfs_name_heap_write(sb, nnlen, n_remain,
					      (const char *)new_dentry->d_name.name + DENTRY_INLINE_NAME_MAX);
		if (err) {
			ravenfs_name_heap_free(sb, new_dir->i_ino, nnlen, n_remain);
			return err;
		}
	}
	err = ravenfs_ddentry_walk(sb, new_dir->i_ino, new_dentry->d_name.name, nnlen, &npath);
	if (err)
		goto out_heap;
	err = ravenfs_ddentry_insert(sb, new_dir->i_ino, &npath, new_dentry->d_name.name, nnlen,
				     xxh64(new_dentry->d_name.name, nnlen, 0),
				     old_inode->i_ino, n_remain);
	if (err)
		goto out_heap;

	/* 计数：old_dir -1；new_dir = +1(新) - (目标?1:0) */
	rfs_dir_adjust(sb, old_dir, -1);
	rfs_dir_adjust(sb, new_dir, new_inode ? 0 : 1);

	/* 跨目录移动目录 → 改盘上父指针 */
	if (old_is_dir && old_dir != new_dir)
		rfs_dir_set_dotdot(sb, old_inode->i_ino, new_dir->i_ino);

	now = current_time(old_dir);
	inode_set_ctime(old_inode, now.tv_sec, now.tv_nsec);
	mark_inode_dirty(old_inode);
	return 0;

out_heap:
	if (nnlen > DENTRY_INLINE_NAME_MAX)
		ravenfs_name_heap_free(sb, new_dir->i_ino, nnlen, n_remain);
	return err;
}

/* ══════════════════════════════════════════════════════════════════
 * M5 残（R-NS-05/06/07/09）：link / symlink+get_link / mknod / tmpfile
 * 规格：不变量 17–21（link_count 契约、link(2) 前置、先删 dentry 后计数）；
 *   disk_layout_v1.2_inode.md §3/§4（type 值域、rdev@inline_data[0:4] 小端 u32）；
 *   ko-requirements-spec §4.3（get_link fast/slow 双态）。
 * ══════════════════════════════════════════════════════════════════ */

/* 在 dir 下插入「名字 → inum」条目，并维护父目录 size/link_count（=size+2）/时间戳。
 * 含长名（>40）名字堆分配；任何失败自回滚名字堆，零残留（对应 create/mkdir 的同款纪律）。 */
static int rfs_dir_link_inode(struct super_block *sb, struct inode *dir,
			      const struct dentry *dentry, u64 inum)
{
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct raven_Inode_t *dr;
	struct rfs_dpath path;
	struct timespec64 now;
	const char *name = dentry->d_name.name;
	u32 nlen = dentry->d_name.len;
	u64 hash, remain_ptr = (u64)PTR_INVALID;
	int err;

	if (nlen == 0 || nlen > RAVENFS_MAX_NAME_LEN)
		return -ENAMETOOLONG;
	ds = ravenfs_session_get(sb, dir->i_ino);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	dr = ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(dr->word1) != INODE_TYPE_DIR)
		return -ENOTDIR;

	err = ravenfs_ddentry_walk(sb, dir->i_ino, name, nlen, &path);
	if (err)
		return err;
	if (path.hit)
		return -EEXIST;		/* 不变量 19-4：目标名不存在 */
	hash = xxh64(name, nlen, 0);

	if (nlen > DENTRY_INLINE_NAME_MAX) {
		err = ravenfs_name_heap_alloc(sb, dir->i_ino, nlen, &remain_ptr);
		if (err)
			return err;
		err = ravenfs_name_heap_write(sb, nlen, remain_ptr,
					      name + DENTRY_INLINE_NAME_MAX);
		if (err) {
			ravenfs_name_heap_free(sb, dir->i_ino, nlen, remain_ptr);
			return err;
		}
	}
	err = ravenfs_ddentry_insert(sb, dir->i_ino, &path, name, nlen, hash, inum, remain_ptr);
	if (err) {
		if (nlen > DENTRY_INLINE_NAME_MAX)
			ravenfs_name_heap_free(sb, dir->i_ino, nlen, remain_ptr);
		return err;
	}

	now = current_time(dir);
	dr->size += 1;
	dr->link_count = (u32)dr->size + 2;
	dr->mtime = now.tv_sec;
	dr->m_ns_stamp = (u32)now.tv_nsec;
	dr->ctime = now.tv_sec;
	dr->c_ns_stamp = (u32)now.tv_nsec;
	RFS_ROW_MARK_DIRTY(ds->row);
	dir->i_size = dr->size;
	set_nlink(dir, dr->link_count);
	inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
	inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
	mark_inode_dirty(dir);
	return 0;
}

/* 分配 + 初始化一个子 inode 的盘记录（不含 dentry 插入、不含数据体）；
 * type/mode/rdev 由调用方给。成功后调方须 session 写回脏标记（本函数已标 dirty）。 */
static int rfs_new_inode_record(struct super_block *sb, struct inode *dir, u32 type,
				umode_t mode, dev_t rdev, u64 *inum_out)
{
	struct ravenfs_session *ts __free(ravenfs_session) = NULL;
	struct raven_Inode_t *tr;
	struct timespec64 now;
	u64 inum;

	inum = ravenfs_inode_alloc(sb);
	if (inum == (u64)PTR_INVALID)
		return -ENOSPC;
	ts = ravenfs_session_get(sb, inum);
	if (IS_ERR(ts)) {
		ravenfs_inode_free(sb, inum);
		return PTR_ERR(ts);
	}
	tr = ravenfs_session_inode(ts);
	now = current_time(dir);
	memset(tr, 0, RAVENFS_INODE_SIZE);
	tr->word1 = RFS_INO1_PACK(type, 0, 0);
	tr->uid = i_uid_read(dir);
	tr->gid = i_gid_read(dir);
	tr->mode_xattr = RFS_MODE_XATTR_PACK(mode & 0o7777, RFS_XATTR_NONE);
	tr->link_count = 1;
	tr->size = 0;
	tr->data_root = (u64)PTR_INVALID;
	tr->mtime = now.tv_sec;
	tr->m_ns_stamp = (u32)now.tv_nsec;
	tr->ctime = now.tv_sec;
	tr->c_ns_stamp = (u32)now.tv_nsec;
	tr->atime = now.tv_sec;
	tr->a_ns_stamp = (u32)now.tv_nsec;
	/* 不变量 26：rdev 仅 CHR/BLK 有效，存 inline_data[0:4] 小端 u32（new_encode_dev）；
	 * 其余类型该区置 0（memset 已零）——防 getattr 带出垃圾。 */
	if (type == INODE_TYPE_CHR || type == INODE_TYPE_BLK) {
		__le32 rdev_le = cpu_to_le32(new_encode_dev(rdev));

		memcpy(tr->inline_data + RFS_INODE_RDEV_OFF, &rdev_le, sizeof(rdev_le));
	}
	RFS_ROW_MARK_DIRTY(ts->row);
	*inum_out = inum;
	return 0;
}

/* R-NS-05：硬链接。前置全过才 ++（不变量 19）；目录禁链（防环）。
 * dcache 交 VFS（vfs_link 已做 S_ISDIR/EMLINK/may_create 预检 + 持源 inode 锁）。 */
int ravenfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	struct super_block *sb = dir->i_sb;
	struct ravenfs_session *ts __free(ravenfs_session) = NULL;
	struct raven_Inode_t *tr;
	struct timespec64 now;
	int err;

	if (ravenfs_is_shutdown(sb))
		return -EIO;
	if (sb_rdonly(sb))
		return -EROFS;
	if (!inode)
		return -ENOENT;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	if (S_ISDIR(inode->i_mode))
		return -EPERM;

	/* 不变量 19 前置：源已分配、type ≠ DIR、link_count < UINT32_MAX */
	ts = ravenfs_session_get(sb, inode->i_ino);
	if (IS_ERR(ts))
		return PTR_ERR(ts);
	tr = ravenfs_session_inode(ts);
	if (RFS_INO1_GET_TYPE(tr->word1) == INODE_TYPE_DIR)
		return -EPERM;
	if (tr->link_count >= (u32)-1)
		return -EMLINK;

	/* 目标名不存在（内部 walk 查重）→ 插 dentry + 父目录 size/link_count */
	err = rfs_dir_link_inode(sb, dir, dentry, inode->i_ino);
	if (err)
		return err;

	/* 盘上 link_count++（与 dentry 条目同步）；源 ctime 更新（POSIX link 改源 ctime） */
	tr->link_count += 1;
	now = current_time(inode);
	tr->ctime = now.tv_sec;
	tr->c_ns_stamp = (u32)now.tv_nsec;
	RFS_ROW_MARK_DIRTY(ts->row);

	inode_inc_link_count(inode);
	inode_set_ctime(inode, now.tv_sec, now.tv_nsec);
	mark_inode_dirty(inode);
	ihold(inode);
	d_instantiate(dentry, inode);
	return 0;
}

/* 把 [src, src+len) 按设备字节偏移 dev_byte 直接写入块设备（symlink slow 物化；len ≤ 1 块）。
 * 与 iomap.c 同源的 sb_bread + sync_dirty_buffer 路径（不绕页缓存）。 */
static int rfs_pwrite_dev(struct super_block *sb, u64 dev_byte, const void *src, u32 len)
{
	u32 done = 0;

	while (done < len) {
		u64 pos = dev_byte + done;
		u64 blk = pos >> 12;			/* sb_bread 以 sb_blocksize(4K) 为单位 */
		u32 off = (u32)(pos & (RAVENFS_BLOCK_SIZE - 1));
		u32 n = min_t(u32, len - done, RAVENFS_BLOCK_SIZE - off);
		struct buffer_head *bh = sb_bread(sb, blk);

		if (!bh)
			return -EIO;
		memcpy(bh->b_data + off, (const char *)src + done, n);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
		done += n;
	}
	return 0;
}

/* R-NS-06 slow：target 写为 LNK inode 的**文件数据** —— 复用既有 iomap 写路径：
 * ravenfs_iomap_begin(WRITE) 做 inline→extent 迁移并提交 data_root/direct/size，
 * 本处仅把 target 字节物化到已分配的映射块，再 iomap_end 提交 i_size/i_blocks。
 * len ∈ (160, 4096] → 单块。 */
static int rfs_write_symlink_target(struct inode *inode, const char *target, u32 len)
{
	struct super_block *sb = inode->i_sb;
	struct iomap iomap, srcmap;
	int err;

	memset(&iomap, 0, sizeof(iomap));
	err = ravenfs_iomap_ops.iomap_begin(inode, 0, (loff_t)len, IOMAP_WRITE,
					    &iomap, &srcmap);
	if (err)
		return err;
	if (iomap.type != IOMAP_MAPPED || iomap.addr == IOMAP_NULL_ADDR) {
		err = -EIO;			/* len>160 必落外置态；否则记录异常 */
		goto out;
	}
	err = rfs_pwrite_dev(sb, iomap.addr, target, len);
out:
	i_size_write(inode, len);
	ravenfs_iomap_ops.iomap_end(inode, 0, (loff_t)len, (ssize_t)len,
				    IOMAP_WRITE, &iomap);
	return err;
}

/* R-NS-06：symlink。≤160 → fast（target 内联盘记录，simple_get_link）；
 * >160 → slow（target 写为文件数据，page_get_link 读）。 */
int ravenfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
		    struct dentry *dentry, const char *symname)
{
	struct super_block *sb = dir->i_sb;
	struct ravenfs_session *ts __free(ravenfs_session) = NULL;
	struct raven_Inode_t *tr;
	struct inode *inode;
	u32 len = (u32)strlen(symname);
	u64 inum;
	int err;

	if (ravenfs_is_shutdown(sb))
		return -EIO;
	if (sb_rdonly(sb))
		return -EROFS;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	if (len > RAVENFS_SYMLINK_MAX)
		return -ENAMETOOLONG;

	err = rfs_new_inode_record(sb, dir, INODE_TYPE_SOFT_SYM, 0777, 0, &inum);
	if (err)
		return err;

	/* fast：target 内联入盘记录（不变量 22：size ≤ 160 ⇒ inline） */
	if (len <= RAVENFS_SYMLINK_INLINE_MAX) {
		ts = ravenfs_session_get(sb, inum);
		if (IS_ERR(ts)) {
			ravenfs_inode_reclaim(sb, inum);
			return PTR_ERR(ts);
		}
		tr = ravenfs_session_inode(ts);
		memcpy(tr->inline_data, symname, len);
		tr->size = len;
		RFS_ROW_MARK_DIRTY(ts->row);
		ravenfs_session_put(ts);
		ts = NULL;
	}

	inode = ravenfs_iget(sb, inum);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ravenfs_inode_reclaim(sb, inum);
		return err;
	}

	/* slow：写文件数据，再切 page_get_link 态（read_inode 此前按 size=0 装了 fast） */
	if (len > RAVENFS_SYMLINK_INLINE_MAX) {
		err = rfs_write_symlink_target(inode, symname, len);
		if (err) {
			clear_nlink(inode);	/* nlink=0 → evict → inode_reclaim */
			iput(inode);
			return err;
		}
		/* slow 态须清 `i_link`：iget 时盘记录 size 仍为 0 → read_inode 曾按
		 * **fast** 装了 i_link = symlink_target（空串）。若不清，vfs_readlink 的
		 * fast 快捷路径（`link = READ_ONCE(inode->i_link)`）会直接复制这个空串，
		 * 永远不走 page_get_link → readlink 返 0 字节（M5 残 slow 实测缺陷）。 */
		inode->i_link = NULL;
		/* slow 态还须清 mapping 的 __GFP_HIGHMEM：page_get_link 要直映射
		 * folio，而 __page_get_link 内 `BUG_ON(mapping_gfp_mask & __GFP_HIGHMEM)`
		 * （ext2_symlink 同款 inode_nohighmem）。iget 走的是 size=0 的 fast
		 * 分支，未设过，故此处补。 */
		inode_nohighmem(inode);
		inode->i_op = &ravenfs_symlink_inode_operations;
	}

	err = rfs_dir_link_inode(sb, dir, dentry, inum);
	if (err) {
		clear_nlink(inode);
		iput(inode);
		return err;
	}
	d_instantiate(dentry, inode);
	return 0;
}

/* R-NS-07：mknod。VFS 四型（FIFO/CHR/BLK/SOCK）都走此回调；CHR/BLK 写 rdev。 */
int ravenfs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
		  umode_t mode, dev_t rdev)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	u32 type = ((u32)mode & S_IFMT) >> 12;
	u64 inum;
	int err;

	if (ravenfs_is_shutdown(sb))
		return -EIO;
	if (sb_rdonly(sb))
		return -EROFS;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	if (!ravenfs_type_valid(type) || type == INODE_TYPE_DIR)
		return -EINVAL;

	err = rfs_new_inode_record(sb, dir, type, mode, rdev, &inum);
	if (err)
		return err;

	/* 先插名字后 iget（同 create：iget 失败极少且仅 I/O 级，此处不额外回滚） */
	err = rfs_dir_link_inode(sb, dir, dentry, inum);
	if (err) {
		ravenfs_inode_reclaim(sb, inum);
		return err;
	}
	inode = ravenfs_iget(sb, inum);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	return 0;
}

/* R-NS-09：tmpfile（O_TMPFILE）。建匿名 REG inode（nlink→0，d_tmpfile），依赖 M4 数据路径。 */
int ravenfs_tmpfile(struct mnt_idmap *idmap, struct inode *dir, struct file *file,
		    umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	u64 inum;
	int err;

	if (ravenfs_is_shutdown(sb))
		return -EIO;
	if (sb_rdonly(sb))
		return -EROFS;
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;

	err = rfs_new_inode_record(sb, dir, INODE_TYPE_REGULAR, mode, 0, &inum);
	if (err)
		return err;

	inode = ravenfs_iget(sb, inum);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ravenfs_inode_reclaim(sb, inum);
		return err;
	}
	d_tmpfile(file, inode);		/* nlink 1→0；匿名 dentry 接管 inode（final close → reclaim） */
	return finish_open_simple(file, 0);
}

struct dentry *ravenfs_lookup(struct inode *dir, struct dentry *dentry,
			      unsigned int flags)
{
	struct inode *inode;
	u64 ino;
	int err;

	if (dentry->d_name.len > RAVENFS_MAX_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);
	if (!S_ISDIR(dir->i_mode))
		return ERR_PTR(-ENOTDIR);

	err = ravenfs_dir_find(dir, dentry->d_name.name, dentry->d_name.len, &ino);
	if (err)
		return ERR_PTR(err);
	if (ino == (u64)PTR_INVALID)
		return NULL;			/* negative dentry（miss） */

	inode = ravenfs_iget(dir->i_sb, ino);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	return d_splice_alias(inode, dentry);
}

/* ── readdir：叶链顺序遍历；cookie = (叶块号, 槽) ──
 *
 * getdents 是**有状态、非幂等**接口：只保证**单次调用内**可靠；跨调用不承诺并发一致（E4）。
 * 顺手把「大目录性能」做实：叶有**双向链**（非根叶 ptrs[1]=前驱, ptrs[2]=后继，v1.1 §4.1）
 *   → cookie 给「叶块号+槽」；续读 = 直接读该叶、从 slot+1 起，再顺 ptrs[2] 走链
 *   ⇒ 每次调用 O(log n) 定位 + O(本次产出)，与目录规模解耦。
 * cookie 编码（loff_t）：0/1 = `.`/`..`（VFS 合成），2 = 起点，≥3 = 3 + ((叶块号<<8)|槽)。
 */
#define RFS_COOKIE_START 2

static inline u64 ravenfs_cookie(u64 blk, u32 slot)
{
	return (blk << 8) | (slot & 0xFF);
}

static int ravenfs_read_leaf(struct super_block *sb, u64 blk, void *buf, u32 *cnt)
{
	const struct Bptree_meta_t *m;
	int err = ravenfs_read_node(sb, blk, buf);

	if (err)
		return err;
	m = buf;
	if (m->magics[0] != DENTRY_MAGIC1 || m->magics[1] != DENTRY_MAGIC2 ||
	    m->magics[2] != DENTRY_MAGIC3 || m->magics[3] != DENTRY_MAGIC4)
		return -EIO;
	if (RFS_PP_IS_INNER(m->parent_ptr))
		return -EIO;
	*cnt = RFS_PP_COUNT(m->parent_ptr);
	return (*cnt > 126) ? -EIO : 0;
}

/* 从 root 一路沿 hints[0] 下到最左叶子 */
static int ravenfs_leftmost_leaf(struct super_block *sb, u64 blk, u64 *leaf)
{
	void *buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	u32 depth = 0;
	int err = 0;

	if (!buf)
		return -ENOMEM;
	for (;;) {
		const struct Bptree_meta_t *m;
		u32 n;

		if (++depth > 32) { err = -EIO; break; }
		err = ravenfs_read_node(sb, blk, buf);
		if (err)
			break;
		m = buf;
		if (m->magics[0] != DENTRY_MAGIC1 || m->magics[1] != DENTRY_MAGIC2 ||
		    m->magics[2] != DENTRY_MAGIC3 || m->magics[3] != DENTRY_MAGIC4) {
			err = -EIO;
			break;
		}
		n = RFS_PP_COUNT(m->parent_ptr);
		if (!RFS_PP_IS_INNER(m->parent_ptr)) {
			*leaf = blk;
			break;
		}
		if (n == 0 || n > 126) { err = -EIO; break; }
		blk = RFS_PTR_ADDR(((const struct Bptree_ddentry_inner_node_t *)buf)->hints[0].sub_ptr);
	}
	kfree(buf);
	return err;
}

/* 输出一个叶子的条目 [from, cnt)；成功一条就推进 cookie，缓冲满则停在「上一个已发条目」 */
static int ravenfs_emit_leaf(struct super_block *sb, const void *buf, u64 blk, u32 cnt,
			     u32 from, struct dir_context *ctx, bool *stopped)
{
	char *nm = kmalloc(RAVENFS_MAX_NAME_LEN, GFP_KERNEL);
	const struct Bptree_ddentry_leaf_node_t *lf = buf;
	u32 i;
	int err = 0;

	if (!nm)
		return -ENOMEM;
	for (i = from; i < cnt; i++) {
		const struct ddentry_t *e = &lf->entries[i];
		unsigned int en;
		u64 ino = RFS_PTR_ADDR(e->inode_ptr);

		err = ravenfs_ddentry_name(sb, e, nm, &en);
		if (err)
			break;
		if (!dir_emit(ctx, nm, en, ino, ravenfs_dt(sb, ino))) {
			*stopped = true;	/* 本条未发出：下次从同一槽重发（E4 允许重） */
			break;
		}
		ctx->pos = (loff_t)(3 + ravenfs_cookie(blk, i));
	}
	kfree(nm);
	return err;
}

int ravenfs_iterate(struct file *file, struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	struct super_block *sb = dir->i_sb;
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_session *ses __free(ravenfs_session) = ravenfs_session_get(sb, dir->i_ino);
	void *buf;
	u64 blk, nxt, guard;
	u32 cnt, from = 0;
	bool stopped = false;
	int err = 0;

	if (IS_ERR(ses))
		return PTR_ERR(ses);
	buf = kmalloc(RAVENFS_NODE_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	if (!dir_emit_dots(file, ctx)) {
		kfree(buf);
		return 0;
	}

	if (ctx->pos >= 3) {
		u64 v = (u64)ctx->pos - 3;

		blk = v >> 8;
		from = (u32)(v & 0xFF) + 1;
		if (ravenfs_read_leaf(sb, blk, buf, &cnt)) {
			kfree(buf);		/* cookie 失效 → 静默结束（不崩） */
			return 0;
		}
		if (from > cnt)
			from = cnt;
	} else {
		err = ravenfs_leftmost_leaf(sb, ravenfs_session_inode(ses)->data_root, &blk);
		if (err) { kfree(buf); return err; }
		if (ravenfs_read_leaf(sb, blk, buf, &cnt)) { kfree(buf); return -EIO; }
	}

	for (guard = 0; guard <= sbi->total_blocks / 2; guard++) {
		err = ravenfs_emit_leaf(sb, buf, blk, cnt, from, ctx, &stopped);
		if (err || stopped)
			break;
		/* 顺叶链前进：下一叶必须 magic 对、非内部、且 prev 指回当前 → 防脏指针死循环 */
		nxt = RFS_PTR_ADDR(((const struct Bptree_meta_t *)buf)->ptrs[2]);
		if (nxt == (u64)RFS_MASK52 || nxt >= sbi->total_blocks)
			break;
		if (ravenfs_read_leaf(sb, nxt, buf, &cnt))
			break;			/* 链断/损坏 → 静默结束 */
		if ((u64)RFS_PTR_ADDR(((const struct Bptree_meta_t *)buf)->ptrs[1]) != blk)
			break;			/* prev 不指回 → 链异常，停 */
		blk = nxt;
		from = 0;
	}
	kfree(buf);
	return err;
}

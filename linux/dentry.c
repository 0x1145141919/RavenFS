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

/* ── create / unlink（M3.4a：REG，单叶目录 + 内联名；多级树/外置名/其他类型待 M5） ── */

/* 目录项定位结果（名字 → 叶块号 + 位；found 区分命中/未命中） */
struct rfs_dloc {
	u64	blk;		/* 叶块号 */
	u32	idx;		/* 命中位 / 插入位 */
	u64	ino;
	bool	found;
};

/* 在**单叶**目录内定位（叶子必须 ≤126 条且非内部；多级树待 M5）。0 = 已定位/<0 错误。 */
static int ravenfs_leaf_locate(struct super_block *sb, u64 leaf_blk, const char *name,
			       u32 nlen, struct rfs_dloc *loc)
{
	struct ravenfs_msession *ms __free(ravenfs_msession) = ravenfs_mdata_get(sb, leaf_blk);
	const struct Bptree_ddentry_leaf_node_t *lf;
	const struct Bptree_meta_t *m;
	char *nm;
	u32 i, cnt;
	u64 hash;
	int err = 0;

	if (IS_ERR(ms))
		return PTR_ERR(ms);
	m = ravenfs_msession_node(ms);
	if (m->magics[0] != DENTRY_MAGIC1 || m->magics[1] != DENTRY_MAGIC2 ||
	    m->magics[2] != DENTRY_MAGIC3 || m->magics[3] != DENTRY_MAGIC4)
		return -EIO;
	if (RFS_PP_IS_INNER(m->parent_ptr))
		return -EOPNOTSUPP;	/* M3.4a：多级树叶定位待 M5 */
	cnt = RFS_PP_COUNT(m->parent_ptr);
	if (cnt > 126)
		return -EIO;
	lf = ravenfs_msession_node(ms);
	hash = xxh64(name, nlen, 0);
	nm = kmalloc(RAVENFS_MAX_NAME_LEN, GFP_KERNEL);
	if (!nm)
		return -ENOMEM;
	loc->blk = leaf_blk;
	loc->idx = cnt;
	loc->ino = (u64)PTR_INVALID;
	loc->found = false;
	for (i = 0; i < cnt; i++) {
		const struct ddentry_t *e = &lf->entries[i];
		unsigned int en;
		int c, cmplen;

		if (e->hash > hash) {		/* 越过 → 插在此位 */
			loc->idx = i;
			goto done;
		}
		if (e->hash != hash)
			continue;
		if (ravenfs_ddentry_name(sb, e, nm, &en)) {
			err = -EIO;
			goto done;
		}
		cmplen = min_t(int, en, (int)nlen);
		c = memcmp(nm, name, cmplen);
		if (c == 0)
			c = (en < nlen) ? -1 : (en > nlen ? 1 : 0);
		if (c == 0) {
			loc->found = true;
			loc->idx = i;
			loc->ino = RFS_PTR_ADDR(e->inode_ptr);
			goto done;
		}
		if (c > 0) {			/* 同名 hash 区，名字更大 → 插在此位 */
			loc->idx = i;
			goto done;
		}
	}
done:
	kfree(nm);
	return err;
}

int ravenfs_create(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, umode_t mode, bool excl)
{
	struct super_block *sb = dir->i_sb;
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct ravenfs_session *ts __free(ravenfs_session) = NULL;
	struct ravenfs_msession *ms __free(ravenfs_msession) = NULL;
	struct raven_Inode_t *dr, *tr;
	struct Bptree_ddentry_leaf_node_t *lf;
	struct Bptree_meta_t *meta;
	struct ddentry_t *e;
	struct rfs_dloc loc;
	struct timespec64 now;
	struct inode *inode;
	u64 inum, root_blk;
	u64 remain_ptr = (u64)PTR_INVALID;
	u32 nlen = dentry->d_name.len, cnt;
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

	err = ravenfs_leaf_locate(sb, root_blk, dentry->d_name.name, nlen, &loc);
	if (err)
		return err;
	if (loc.found)
		return -EEXIST;

	ms = ravenfs_mdata_get(sb, root_blk);
	if (IS_ERR(ms))
		return PTR_ERR(ms);
	lf = ravenfs_msession_node(ms);
	meta = &lf->meta;
	if (RFS_PP_IS_INNER(meta->parent_ptr))
		return -EOPNOTSUPP;		/* 多级树插入（hint 维护）待 M5 */
	cnt = RFS_PP_COUNT(meta->parent_ptr);
	if (cnt >= 126)
		return -ENOSPC;			/* 叶满 → 分裂待 M5 */

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
	ts->row->dirty = true;
	ts->row->stamp = ktime_get_ns();

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

	/* 叶插入 */
	if (loc.idx > cnt)
		loc.idx = cnt;
	memmove(&lf->entries[loc.idx + 1], &lf->entries[loc.idx],
		(size_t)(cnt - loc.idx) * sizeof(struct ddentry_t));
	e = &lf->entries[loc.idx];
	memset(e, 0, sizeof(*e));
	e->inode_ptr.v = RFS_PTR_PACK(nlen, inum);
	e->hash = xxh64(dentry->d_name.name, nlen, 0);
	e->full_char_remain_ptr = remain_ptr;
	memcpy(e->name, dentry->d_name.name, min_t(u32, nlen, (u32)DENTRY_INLINE_NAME_MAX));
	meta->parent_ptr.v = RFS_PP_PACK(cnt + 1, RFS_PP_IS_INNER(meta->parent_ptr),
					 RFS_PP_IS_ROOT(meta->parent_ptr), RFS_PP_PTR(meta->parent_ptr));
	ms->row->dirty = true;
	ms->row->stamp = ktime_get_ns();

	/* 父目录计数维护：size++ / link_count = size+2 */
	dr->size += 1;
	dr->link_count = (u32)dr->size + 2;
	dr->mtime = now.tv_sec;
	dr->m_ns_stamp = (u32)now.tv_nsec;
	dr->ctime = now.tv_sec;
	dr->c_ns_stamp = (u32)now.tv_nsec;
	ds->row->dirty = true;
	ds->row->stamp = ktime_get_ns();
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
	struct ravenfs_msession *ms __free(ravenfs_msession) = NULL;
	struct raven_Inode_t *dr;
	struct Bptree_ddentry_leaf_node_t *lf;
	struct Bptree_meta_t *meta;
	struct rfs_dloc loc;
	struct timespec64 now;
	u64 root_blk;
	u64 rem_remain = (u64)PTR_INVALID;
	u32 nlen = dentry->d_name.len, cnt;
	u32 rem_name_len = 0;
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

	err = ravenfs_leaf_locate(sb, root_blk, dentry->d_name.name, nlen, &loc);
	if (err)
		return err;
	if (!loc.found)
		return -ENOENT;

	ms = ravenfs_mdata_get(sb, root_blk);
	if (IS_ERR(ms))
		return PTR_ERR(ms);
	lf = ravenfs_msession_node(ms);
	meta = &lf->meta;
	cnt = RFS_PP_COUNT(meta->parent_ptr);
	if (loc.idx >= cnt)
		return -EIO;
	rem_name_len = RFS_PTR_FREE(lf->entries[loc.idx].inode_ptr);
	rem_remain = lf->entries[loc.idx].full_char_remain_ptr;
	memmove(&lf->entries[loc.idx], &lf->entries[loc.idx + 1],
		(size_t)(cnt - loc.idx - 1) * sizeof(struct ddentry_t));
	memset(&lf->entries[cnt - 1], 0, sizeof(struct ddentry_t));
	meta->parent_ptr.v = RFS_PP_PACK(cnt - 1, RFS_PP_IS_INNER(meta->parent_ptr),
					 RFS_PP_IS_ROOT(meta->parent_ptr), RFS_PP_PTR(meta->parent_ptr));
	ms->row->dirty = true;
	ms->row->stamp = ktime_get_ns();

	now = current_time(dir);
	dr->size -= 1;
	dr->link_count = (u32)dr->size + 2;
	dr->mtime = now.tv_sec;
	dr->m_ns_stamp = (u32)now.tv_nsec;
	dr->ctime = now.tv_sec;
	dr->c_ns_stamp = (u32)now.tv_nsec;
	ds->row->dirty = true;
	ds->row->stamp = ktime_get_ns();
	dir->i_size = dr->size;
	set_nlink(dir, dr->link_count);
	inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
	inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
	mark_inode_dirty(dir);

	/* 长名尾段：释放堆存储（失败仅告警：条目已移除；孤儿位 audit 不判损） */
	if (rem_name_len > DENTRY_INLINE_NAME_MAX) {
		int herr = ravenfs_name_heap_free(sb, dir->i_ino, rem_name_len, rem_remain);

		if (herr)
			pr_warn("ravenfs: name_heap_free(%u) failed: %d\n", rem_name_len, herr);
	}

	/* 目标 inode：nlink--（VFS 后续 evict → ravenfs_inode_reclaim 回收槽位） */
	if (inode) {
		inode_set_ctime(inode, now.tv_sec, now.tv_nsec);
		if (inode->i_nlink)
			inode_dec_link_count(inode);
	}
	return 0;
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

// SPDX-License-Identifier: GPL-2.0
/*
 * xattr.c —— RavenFS ko：扩展属性 / POSIX ACL / capability（M6，R-XA-01..04）
 *
 * 盘面真值：Docs/disk_layout_v1.2_inode.md §5（xattr 块）+ include/disk_struct_v1.2.h。
 * 规格：Docs/linux/ravenfs-linux-ko-requirements-spec.md §4.6（R-XA-01..04）/§4.5.1。
 * 参考真值（内核）：fs/xattr.c（VFS 分派）、fs/ext2/xattr.c（get/set 机械）、
 *   fs/posix_acl.c + include/linux/posix_acl*.h（ACL 通用格式/helper）、
 *   fs/ext4/acl.c（get_inode_acl/set_acl/init_acl 装配）、security/commoncap.c（cap）。
 *
 * ── 盘面（单块自描述；抄 ext4）─────────────────────────────────────────────
 *   inode.mode_xattr 高 52 位 = xattr_ptr（无 xattr = RFS_XATTR_NONE 全 1 哨兵）。
 *   块 = 1 个 FS_block(4096B)：32B 头（h_magic="RFSX" / h_refcount=1 / h_blocks=1 /
 *   h_hash=0）+ 条目区（自 [32) 向高增长，首个 u32==0 终止）+ 值区（自块尾向低堆叠，4B 对齐）。
 *   条目定长头 8B：e_name_len / e_name_index / e_value_offs / e_value_size（值**不**内联）。
 *
 * ── 定调（用户，2026-09-27）────────────────────────────────────────────────
 *   · trusted.* **不做**（不注册 handler → -EOPNOTSUPP）。
 *   · 值 > 1 FS_block(4K) → **干净拒绝 -E2BIG**（不发明分片）。
 *   · 范围必须做全：4 回调 + capability + POSIX ACL(access+default, 默认继承)。
 *
 * ── 并发 ───────────────────────────────────────────────────────────────────
 *   set/ACL 走 XATTR 块的「读-改-写」；VFS set 路径持 inode_lock；get/list 不持。
 *   故 per-inode `xattr_sem`（read for get/list，write for set）序列化；盘 inode 指针
 *   的 RMW 另持缓存行 io_lock（与 write_inode 的 sync_from_vfs 互斥）。
 *   写序（先 inode 后 free）：新建块「写块 → 置 inode 指针」；删空块「清 inode 指针 → free 块」。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/xattr.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/buffer_head.h>
#include <linux/timekeeping.h>
#include <linux/user_namespace.h>

#include "ravenfs.h"
#include "ravenfs_cache.h"
#include "ravenfs_bitmap.h"
#include "ravenfs_xattr.h"

#define RFS_XATTR_NAME_MAX	255
#define RFS_XATTR_SIZE_MAX	65536

/* ═══════════════════════════════════════════════════════════════════
 * 小块读写（4K，与装载侧同源 sb_bread + sync_dirty_buffer；不绕 mdata 缓存）
 * ═══════════════════════════════════════════════════════════════════ */

static int rfs_xb_load(struct super_block *sb, u64 blk, void **out)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct buffer_head *bh;
	void *buf;

	bh = sb_bread(sb, ravenfs_dev_blk(sbi, blk));
	if (!bh)
		return -EIO;
	buf = kmalloc(RFS_XATTR_BLOCK_SIZE, GFP_KERNEL);
	if (!buf) {
		brelse(bh);
		return -ENOMEM;
	}
	memcpy(buf, bh->b_data, RFS_XATTR_BLOCK_SIZE);
	brelse(bh);
	RFS_STAT_INC(sb, xa_block_load);
	RFS_STAT_INC(sb, io_read_ops);
	RFS_STAT_ADD(sb, io_read_bytes, RAVENFS_BLOCK_SIZE);
	*out = buf;
	return 0;
}

static int rfs_xb_store(struct super_block *sb, u64 blk, const void *buf)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct buffer_head *bh;

	bh = sb_bread(sb, ravenfs_dev_blk(sbi, blk));
	if (!bh)
		return -EIO;
	memcpy(bh->b_data, buf, RFS_XATTR_BLOCK_SIZE);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	RFS_STAT_INC(sb, xa_block_store);
	RFS_STAT_INC(sb, io_write_ops);
	RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * 盘 inode 指针读写（mode_xattr 高 52 位；RMW 持 io_lock 与 write_inode 互斥）
 * ═══════════════════════════════════════════════════════════════════ */

static int rfs_xattr_ptr(struct super_block *sb, u64 inum, u64 *out)
{
	struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, inum);

	if (IS_ERR(s))
		return PTR_ERR(s);
	*out = RFS_XATTRPTR_GET(ravenfs_session_inode(s)->mode_xattr);
	return 0;
}

static int rfs_xattr_set_ptr(struct super_block *sb, u64 inum, u64 newptr)
{
	struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, inum);
	struct raven_Inode_t *r;

	if (IS_ERR(s))
		return PTR_ERR(s);
	down_write(&s->row->io_lock);
	r = ravenfs_session_inode(s);
	r->mode_xattr = RFS_MODE_XATTR_PACK(RFS_MODE_GET(r->mode_xattr), newptr);
	up_write(&s->row->io_lock);
	RFS_ROW_MARK_DIRTY(s->row);
	atomic64_set(&s->row->acc_stamp, ktime_get_ns());
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * 条目定位 / 校验（抄 ext2 语义；8B 定长头，无 e_value_block/e_hash）
 * ═══════════════════════════════════════════════════════════════════ */

#define RFS_XI_HDR(b)   ((struct rfs_xattr_block_hdr *)(b))
#define RFS_XI_FIRST(b) ((struct rfs_xattr_entry *)((char *)(b) + RFS_XATTR_HDR_SIZE))
#define RFS_XI_END(b)   ((char *)(b) + RFS_XATTR_BLOCK_SIZE)

static inline bool rfs_xattr_index_valid(u8 idx)
{
	switch (idx) {
	case RFS_XATTR_INDEX_USER:
	case RFS_XATTR_INDEX_POSIX_ACL_ACCESS:
	case RFS_XATTR_INDEX_POSIX_ACL_DEFAULT:
	case RFS_XATTR_INDEX_TRUSTED:
	case RFS_XATTR_INDEX_SECURITY:
	case RFS_XATTR_INDEX_SYSTEM:
		return true;
	default:
		return false;
	}
}

static bool rfs_xattr_hdr_valid(const struct rfs_xattr_block_hdr *h)
{
	return h->h_magic == RFS_XATTR_H_MAGIC &&
	       h->h_blocks == 1 && h->h_refcount == 1 && h->h_hash == 0;
}

/* 遍历校验整块结构（头另校验）；*min_offs = 值区最低地址（无值则 = 块尾）。
 * 返回 0 合法，-EIO 非法（对齐 audit_v1.2 §5.5 口径）。 */
static int rfs_xb_check(const void *blk, u32 *min_offs, u32 *nent)
{
	const char *rend = RFS_XI_END(blk);
	struct rfs_xattr_entry *e = RFS_XI_FIRST(blk);
	u32 off = RFS_XATTR_HDR_SIZE, entry_end = RFS_XATTR_HDR_SIZE, n = 0;
	u32 vmin = RFS_XATTR_BLOCK_SIZE;
	bool terminated = false;

	while ((const char *)e + 4 <= rend) {
		u64 ent_len;

		if (RFS_XATTR_IS_LAST(e)) {
			terminated = true;
			break;
		}
		if ((const char *)e + RFS_XATTR_ENTRY_HDR_SIZE > rend)
			return -EIO;
		if (!rfs_xattr_index_valid(e->e_name_index))
			return -EIO;
		/* 空名规则（抄 ext4）：ACL(2/3) 后缀必空；其余后缀必非空 */
		if ((e->e_name_index == RFS_XATTR_INDEX_POSIX_ACL_ACCESS ||
		     e->e_name_index == RFS_XATTR_INDEX_POSIX_ACL_DEFAULT) !=
		    (e->e_name_len == 0))
			return -EIO;
		ent_len = RFS_XATTR_LEN(e->e_name_len);
		if ((char *)e + ent_len > rend)
			return -EIO;
		if (e->e_value_size) {
			u32 vo = e->e_value_offs, vs = e->e_value_size;

			if (vo < RFS_XATTR_HDR_SIZE ||
			    (u64)vo + vs > RFS_XATTR_BLOCK_SIZE || (vo & 3))
				return -EIO;
			if (vo < vmin)
				vmin = vo;
		}
		off += (u32)ent_len;
		entry_end = off;
		e = RFS_XATTR_NEXT(e);
		n++;
	}
	if (!terminated)
		return -EIO;
	/* 终止符 4B 必须落在值区之前：entry_end + 4 ≤ vmin（无值则 vmin=块尾）*/
	if (n && entry_end + 4 > vmin)
		return -EIO;
	*min_offs = n ? vmin : RFS_XATTR_BLOCK_SIZE;
	*nent = n;
	return 0;
}

static int rfs_xattr_cmp(int index, size_t name_len, const char *name,
			 const struct rfs_xattr_entry *e)
{
	int c = index - e->e_name_index;

	if (!c)
		c = (int)name_len - (int)e->e_name_len;
	if (!c)
		c = memcmp(name, (const char *)e + RFS_XATTR_ENTRY_HDR_SIZE, name_len);
	return c;
}

/* ═══════════════════════════════════════════════════════════════════
 * 核心：get / set（在给定 inum 的盘记录上，不取 VFS inode）
 * ═══════════════════════════════════════════════════════════════════ */

static int rfs_xattr_get_core(struct super_block *sb, u64 inum, int name_index,
			      const char *name, void *buffer, size_t size)
{
	u64 ptr;
	void *blk = NULL;
	struct rfs_xattr_entry *e;
	u32 min_offs, nent;
	size_t name_len;
	int err;

	if (!name)
		return -EINVAL;
	name_len = strlen(name);
	if (name_len > RFS_XATTR_NAME_MAX)
		return -ERANGE;

	err = rfs_xattr_ptr(sb, inum, &ptr);
	if (err)
		return err;
	if (ptr == (u64)RFS_XATTR_NONE)
		return -ENODATA;
	err = rfs_xb_load(sb, ptr, &blk);
	if (err)
		return err;
	if (!rfs_xattr_hdr_valid(blk)) {
		err = -EIO;
		goto out;
	}
	err = rfs_xb_check(blk, &min_offs, &nent);
	if (err)
		goto out;

	for (e = RFS_XI_FIRST(blk); !RFS_XATTR_IS_LAST(e); e = RFS_XATTR_NEXT(e)) {
		int c = rfs_xattr_cmp(name_index, name_len, name, e);

		if (c == 0) {
			u32 vs = e->e_value_size;

			if (buffer) {
				if (vs > size) {
					err = -ERANGE;
					goto out;
				}
				memcpy(buffer, (char *)blk + e->e_value_offs, vs);
			}
			err = (int)vs;
			goto out;
		}
		if (c < 0)
			break;
	}
	err = -ENODATA;
out:
	kfree(blk);
	return err;
}

static int rfs_xattr_set_core(struct super_block *sb, u64 inum, int name_index,
			      const char *name, const void *value,
			      size_t value_len, int flags)
{
	u64 ptr;
	void *blk = NULL;
	struct rfs_xattr_entry *here = NULL, *last, *it;
	size_t name_len, free_spc;
	u32 min_offs, nent;
	int need = 1, err = 0;

	if (value == NULL)
		value_len = 0;
	if (!name)
		return -EINVAL;
	name_len = strlen(name);
	if (name_len > RFS_XATTR_NAME_MAX)
		return -ERANGE;
	/* 定调：值 > 1 FS_block → 干净拒绝（不发明分片） */
	if (value_len > RFS_XATTR_BLOCK_SIZE || value_len > RFS_XATTR_SIZE_MAX) {
		RFS_STAT_INC(sb, xa_e2big);	/* 单块上限被撞的频率 */
		return -E2BIG;
	}

	err = rfs_xattr_ptr(sb, inum, &ptr);
	if (err)
		return err;

	if (ptr != (u64)RFS_XATTR_NONE) {
		err = rfs_xb_load(sb, ptr, &blk);
		if (err)
			return err;
		if (!rfs_xattr_hdr_valid(blk)) {
			kfree(blk);
			return -EIO;
		}
	} else {
		blk = kzalloc(RFS_XATTR_BLOCK_SIZE, GFP_KERNEL);
		if (!blk)
			return -ENOMEM;
		RFS_XI_HDR(blk)->h_magic = RFS_XATTR_H_MAGIC;
		RFS_XI_HDR(blk)->h_refcount = 1;
		RFS_XI_HDR(blk)->h_blocks = 1;
		RFS_XI_HDR(blk)->h_hash = 0;
	}
	err = rfs_xb_check(blk, &min_offs, &nent);
	if (err)
		goto out;

	/* 定位：here = 命中条目；未命中 = 首个 > 目标的插入位（保序）；末尾 = 终止位 */
	last = RFS_XI_FIRST(blk);
	while (!RFS_XATTR_IS_LAST(last)) {
		if (need > 0) {
			need = rfs_xattr_cmp(name_index, name_len, name, last);
			if (need <= 0)
				here = last;
		}
		last = RFS_XATTR_NEXT(last);
	}
	if (need > 0)
		here = last;

	/* 可用空间：free = 值区最低 − 终止位 − 4（留终止符） */
	free_spc = (size_t)(min_offs - ((char *)last - (char *)blk) - 4);

	if (need != 0) {			/* 未命中 */
		if (flags & XATTR_REPLACE) {
			err = -ENODATA;
			goto out;
		}
		if (value == NULL) {		/* 删不存在的 attr → no-op */
			err = 0;
			goto out;
		}
	} else {				/* 命中 */
		if (flags & XATTR_CREATE) {
			err = -EEXIST;
			goto out;
		}
		free_spc += RFS_XATTR_ALIGN(here->e_value_size);
		free_spc += RFS_XATTR_LEN(here->e_name_len);
	}
	if (free_spc < RFS_XATTR_LEN(name_len) + RFS_XATTR_ALIGN(value_len)) {
		RFS_STAT_INC(sb, xa_e2big);
		err = -E2BIG;			/* 放不进一块（定调：E2BIG，非 ENOSPC） */
		goto out;
	}

	if (need != 0) {
		/* ── 插入新条目（未命中且要写值）── */
		size_t sz = RFS_XATTR_LEN(name_len);
		size_t rest = (char *)last - (char *)here;

		memmove((char *)here + sz, here, rest);
		memset(here, 0, sz);
		here->e_name_index = name_index;
		here->e_name_len = name_len;
		memcpy((char *)here + RFS_XATTR_ENTRY_HDR_SIZE, name, name_len);
	} else {
		/* ── 命中：先清旧值（保 4B 紧凑；值向下堆叠）── */
		if (here->e_value_size) {
			char *first_val = (char *)blk + min_offs;
			u32 offs = here->e_value_offs;
			size_t sz = RFS_XATTR_ALIGN(here->e_value_size);

			if (sz == RFS_XATTR_ALIGN(value_len)) {
				/* 同长 → 就地覆盖（清尾 pad 防泄漏） */
				here->e_value_size = value_len;
				memset((char *)blk + offs + sz - 4, 0, 4);
				if (value_len)
					memcpy((char *)blk + offs, value, value_len);
				goto done_mutate;
			}
			/* 异长：把 [first_val, val) 上移 sz → 释放底部 sz，min_offs += sz */
			memmove(first_val + sz, first_val,
				(char *)blk + offs - first_val);
			memset(first_val, 0, sz);
			min_offs += sz;
			it = RFS_XI_FIRST(blk);
			while (!RFS_XATTR_IS_LAST(it)) {
				u32 o = it->e_value_offs;

				if (o && o < offs)
					it->e_value_offs = o + sz;
				it = RFS_XATTR_NEXT(it);
			}
			here->e_value_offs = 0;
		}
		if (value == NULL) {		/* 删该名 */
			size_t sz = RFS_XATTR_LEN(name_len);
			struct rfs_xattr_entry *new_last =
				(struct rfs_xattr_entry *)((char *)last - sz);

			memmove(here, (char *)here + sz, (char *)new_last - (char *)here);
			memset(new_last, 0, sz);
		}
	}

	if (value != NULL) {			/* 写入新值 */
		here->e_value_size = value_len;
		if (value_len) {
			size_t sz = RFS_XATTR_ALIGN(value_len);
			char *val = (char *)blk + min_offs - sz;

			here->e_value_offs = (u16)(val - (char *)blk);
			memset(val + sz - 4, 0, 4);
			memcpy(val, value, value_len);
		}
	}
done_mutate:
	/* ── 落块：空 → 清 inode 指针后 free；否则写块（新建先写块后置指针）── */
	if (RFS_XATTR_IS_LAST(RFS_XI_FIRST(blk))) {
		if (ptr != (u64)RFS_XATTR_NONE) {
			err = rfs_xattr_set_ptr(sb, inum, (u64)RFS_XATTR_NONE);
			if (err)
				goto out;
			ravenfs_block_free(sb, ptr, 1);		/* 先 inode 后 free */
		}
		err = 0;
		goto out;
	}
	if (ptr == (u64)RFS_XATTR_NONE) {
		u64 nb = ravenfs_block_alloc(sb, 1, 0);

		if (nb == (u64)PTR_INVALID) {
			err = -ENOSPC;
			goto out;
		}
		err = rfs_xb_store(sb, nb, blk);
		if (!err)
			err = rfs_xattr_set_ptr(sb, inum, nb);
		if (err) {
			ravenfs_block_free(sb, nb, 1);
			goto out;
		}
	} else {
		err = rfs_xb_store(sb, ptr, blk);
		if (err)
			err = -EIO;
	}
out:
	kfree(blk);
	return err;
}

/* ═══════════════════════════════════════════════════════════════════
 * 对外 get/set（持 xattr_sem）
 * ═══════════════════════════════════════════════════════════════════ */

int ravenfs_xattr_get(struct inode *inode, int name_index, const char *name,
		      void *buffer, size_t size)
{
	struct ravenfs_inode_info *ri = RAVENFS_I(inode);
	struct super_block *sb = inode->i_sb;
	int err;

	RFS_STAT_INC(sb, op_xattr_get);
	RFS_STAT_INC(sb, xa_get);
	if (ravenfs_is_shutdown(sb)) {
		RFS_STAT_INC(sb, err_shutdown_hits);
		return -EIO;
	}
	down_read(&ri->xattr_sem);
	err = rfs_xattr_get_core(sb, inode->i_ino, name_index, name,
				 buffer, size);
	up_read(&ri->xattr_sem);
	if (err < 0)
		RFS_STAT_INC(sb, xa_err);
	return err;
}

int ravenfs_xattr_set(struct inode *inode, int name_index, const char *name,
		      const void *value, size_t size, int flags)
{
	struct ravenfs_inode_info *ri = RAVENFS_I(inode);
	struct super_block *sb = inode->i_sb;
	int err;

	RFS_STAT_INC(sb, op_xattr_set);
	if (value == NULL)
		RFS_STAT_INC(sb, xa_del);	/* del = set 空值路径 */
	else
		RFS_STAT_INC(sb, xa_set);
	if (ravenfs_is_shutdown(sb)) {
		RFS_STAT_INC(sb, err_shutdown_hits);
		return -EIO;
	}
	if (sb_rdonly(sb))
		return -EROFS;
	down_write(&ri->xattr_sem);
	err = rfs_xattr_set_core(sb, inode->i_ino, name_index, name, value,
				 size, flags);
	up_write(&ri->xattr_sem);
	if (err)
		RFS_STAT_INC(sb, xa_err);
	if (!err) {
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
	}
	return err;
}

/* ═══════════════════════════════════════════════════════════════════
 * xattr_handler：user / security / system（trusted 不做 → 不注册）
 * ═══════════════════════════════════════════════════════════════════ */

static int ravenfs_xattr_user_get(const struct xattr_handler *handler,
				  struct dentry *unused, struct inode *inode,
				  const char *name, void *buffer, size_t size)
{
	return ravenfs_xattr_get(inode, RFS_XATTR_INDEX_USER, name, buffer, size);
}

static int ravenfs_xattr_user_set(const struct xattr_handler *handler,
				  struct mnt_idmap *idmap, struct dentry *unused,
				  struct inode *inode, const char *name,
				  const void *value, size_t size, int flags)
{
	return ravenfs_xattr_set(inode, RFS_XATTR_INDEX_USER, name, value, size, flags);
}

static int ravenfs_xattr_security_get(const struct xattr_handler *handler,
				      struct dentry *unused, struct inode *inode,
				      const char *name, void *buffer, size_t size)
{
	return ravenfs_xattr_get(inode, RFS_XATTR_INDEX_SECURITY, name, buffer, size);
}

static int ravenfs_xattr_security_set(const struct xattr_handler *handler,
				      struct mnt_idmap *idmap,
				      struct dentry *unused, struct inode *inode,
				      const char *name, const void *value,
				      size_t size, int flags)
{
	return ravenfs_xattr_set(inode, RFS_XATTR_INDEX_SECURITY, name, value,
				 size, flags);
}

static int ravenfs_xattr_system_get(const struct xattr_handler *handler,
				    struct dentry *unused, struct inode *inode,
				    const char *name, void *buffer, size_t size)
{
	/* 非 ACL 的 system.*（ACL 名由 VFS 特殊路径转发到 get_inode_acl，到不了这里） */
	return ravenfs_xattr_get(inode, RFS_XATTR_INDEX_SYSTEM, name, buffer, size);
}

static int ravenfs_xattr_system_set(const struct xattr_handler *handler,
				    struct mnt_idmap *idmap,
				    struct dentry *unused, struct inode *inode,
				    const char *name, const void *value,
				    size_t size, int flags)
{
	return ravenfs_xattr_set(inode, RFS_XATTR_INDEX_SYSTEM, name, value,
				 size, flags);
}

static const struct xattr_handler ravenfs_xattr_user_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.get	= ravenfs_xattr_user_get,
	.set	= ravenfs_xattr_user_set,
};

static const struct xattr_handler ravenfs_xattr_security_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.get	= ravenfs_xattr_security_get,
	.set	= ravenfs_xattr_security_set,
};

static const struct xattr_handler ravenfs_xattr_system_handler = {
	.prefix	= XATTR_SYSTEM_PREFIX,
	.get	= ravenfs_xattr_system_get,
	.set	= ravenfs_xattr_system_set,
};

const struct xattr_handler * const ravenfs_xattr_handlers[] = {
	&ravenfs_xattr_user_handler,
	&ravenfs_xattr_security_handler,
	&ravenfs_xattr_system_handler,
	NULL,
};

/* index → handler（listxattr 解析名字前缀；ACL 复用内核 nop handler，键即前缀） */
static const struct xattr_handler * const rfs_xattr_handler_map[] = {
	[RFS_XATTR_INDEX_USER]			= &ravenfs_xattr_user_handler,
	[RFS_XATTR_INDEX_POSIX_ACL_ACCESS]	= &nop_posix_acl_access,
	[RFS_XATTR_INDEX_POSIX_ACL_DEFAULT]	= &nop_posix_acl_default,
	/* [RFS_XATTR_INDEX_TRUSTED] 不注册 → 不识别/不列出 */
	[RFS_XATTR_INDEX_SECURITY]		= &ravenfs_xattr_security_handler,
	[RFS_XATTR_INDEX_SYSTEM]		= &ravenfs_xattr_system_handler,
};

/* ═══════════════════════════════════════════════════════════════════
 * listxattr（inode_operations）
 * ═══════════════════════════════════════════════════════════════════ */

ssize_t ravenfs_listxattr(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	struct inode *inode = d_inode(dentry);
	struct super_block *sb = inode->i_sb;
	struct ravenfs_inode_info *ri = RAVENFS_I(inode);
	void *blk = NULL;
	struct rfs_xattr_entry *e;
	u64 ptr;
	u32 min_offs, nent;
	size_t rest = buffer_size;
	char *p = buffer;
	int err;

	RFS_STAT_INC(sb, op_listxattr);
	RFS_STAT_INC(sb, xa_list);
	if (ravenfs_is_shutdown(sb)) {
		RFS_STAT_INC(sb, err_shutdown_hits);
		return -EIO;
	}

	down_read(&ri->xattr_sem);
	err = rfs_xattr_ptr(sb, inode->i_ino, &ptr);
	if (err)
		goto out;
	if (ptr == (u64)RFS_XATTR_NONE) {
		err = 0;
		goto out;
	}
	err = rfs_xb_load(sb, ptr, &blk);
	if (err)
		goto out;
	if (!rfs_xattr_hdr_valid(blk)) {
		err = -EIO;
		goto out;
	}
	err = rfs_xb_check(blk, &min_offs, &nent);
	if (err)
		goto out;

	err = 0;
	for (e = RFS_XI_FIRST(blk); !RFS_XATTR_IS_LAST(e); e = RFS_XATTR_NEXT(e)) {
		const struct xattr_handler *h = NULL;
		const char *prefix;
		size_t pl, sz;

		if (e->e_name_index < ARRAY_SIZE(rfs_xattr_handler_map))
			h = rfs_xattr_handler_map[e->e_name_index];
		if (!xattr_handler_can_list(h, dentry))
			continue;		/* 无 handler / 不可列 → 跳过 */
		prefix = xattr_prefix(h);
		pl = strlen(prefix);
		sz = pl + e->e_name_len + 1;
		if (p) {
			if (sz > rest) {
				err = -ERANGE;
				goto out;
			}
			memcpy(p, prefix, pl);
			p += pl;
			memcpy(p, (char *)e + RFS_XATTR_ENTRY_HDR_SIZE, e->e_name_len);
			p += e->e_name_len;
			*p++ = '\0';
		}
		rest -= sz;
	}
	err = (int)(buffer_size - rest);
out:
	kfree(blk);
	up_read(&ri->xattr_sem);
	return err;
}

/* ═══════════════════════════════════════════════════════════════════
 * POSIX ACL（R-XA-03）：值 opaque（内核通用 posix_acl_xattr v2 格式）
 * ═══════════════════════════════════════════════════════════════════ */

struct posix_acl *ravenfs_get_inode_acl(struct inode *inode, int type, bool rcu)
{
	int index;
	void *value = NULL;
	struct posix_acl *acl;
	int ret;

	RFS_STAT_INC(inode->i_sb, op_acl_get);
	RFS_STAT_INC(inode->i_sb, acl_get);
	if (rcu)
		return ERR_PTR(-ECHILD);
	switch (type) {
	case ACL_TYPE_ACCESS:
		index = RFS_XATTR_INDEX_POSIX_ACL_ACCESS;
		break;
	case ACL_TYPE_DEFAULT:
		index = RFS_XATTR_INDEX_POSIX_ACL_DEFAULT;
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	ret = ravenfs_xattr_get(inode, index, "", NULL, 0);
	if (ret > 0) {
		value = kmalloc(ret, GFP_NOFS);
		if (!value)
			return ERR_PTR(-ENOMEM);
		ret = ravenfs_xattr_get(inode, index, "", value, ret);
	}
	if (ret > 0)
		acl = posix_acl_from_xattr(&init_user_ns, value, ret);
	else if (ret == -ENODATA || ret == 0)
		acl = NULL;
	else
		acl = ERR_PTR(ret);
	kfree(value);
	return acl;
}

/* 内部 set（供 set_acl / init_acl 复用） */
static int rfs_set_acl_internal(struct inode *inode, struct posix_acl *acl, int type)
{
	int index;
	void *value = NULL;
	size_t size = 0;
	int err;

	RFS_STAT_INC(inode->i_sb, acl_set);	/* 真 ACL 写（含 init_acl 内部） */
	switch (type) {
	case ACL_TYPE_ACCESS:
		index = RFS_XATTR_INDEX_POSIX_ACL_ACCESS;
		break;
	case ACL_TYPE_DEFAULT:
		index = RFS_XATTR_INDEX_POSIX_ACL_DEFAULT;
		if (!S_ISDIR(inode->i_mode))
			return acl ? -EACCES : 0;
		break;
	default:
		return -EINVAL;
	}
	if (acl) {
		value = posix_acl_to_xattr(&init_user_ns, acl, &size, GFP_NOFS);
		if (!value)
			return -ENOMEM;
	}
	err = ravenfs_xattr_set(inode, index, "", value, size, 0);
	kfree(value);
	if (!err)
		set_cached_acl(inode, type, acl);
	return err;
}

int ravenfs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		    struct posix_acl *acl, int type)
{
	struct inode *inode = d_inode(dentry);
	umode_t mode = inode->i_mode;
	int update_mode = 0, err;

	RFS_STAT_INC(inode->i_sb, op_acl_set);
	if (type == ACL_TYPE_ACCESS && acl) {
		err = posix_acl_update_mode(idmap, inode, &mode, &acl);
		if (err)
			return err;
		if (mode != inode->i_mode)
			update_mode = 1;
	}
	err = rfs_set_acl_internal(inode, acl, type);
	if (!err && update_mode) {
		inode->i_mode = mode;
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
	}
	return err;
}

/* 新 inode 的 ACL 初始化：默认 ACL 继承 + umask（R-XA-03 / posix_acl_create） */
int ravenfs_init_acl(struct inode *inode, struct inode *dir)
{
	struct posix_acl *default_acl = NULL, *acl = NULL;
	struct super_block *sb = inode->i_sb;
	int err;

	err = posix_acl_create(dir, &inode->i_mode, &default_acl, &acl);
	if (err)
		return err;

	if (default_acl) {
		err = rfs_set_acl_internal(inode, default_acl, ACL_TYPE_DEFAULT);
		posix_acl_release(default_acl);
	}
	if (!err && acl)
		err = rfs_set_acl_internal(inode, acl, ACL_TYPE_ACCESS);
	posix_acl_release(acl);

	/* 把（可能被 umask / 默认 ACL 改过的）mode 落进盘记录（确定性，不依赖 write_inode 时机） */
	if (!err) {
		struct ravenfs_session *s __free(ravenfs_session) =
			ravenfs_session_get(sb, inode->i_ino);

		if (!IS_ERR(s)) {
			struct raven_Inode_t *r;

			down_write(&s->row->io_lock);
			r = ravenfs_session_inode(s);
			r->mode_xattr = RFS_MODE_XATTR_PACK(
				inode->i_mode & 0o7777,
				RFS_XATTRPTR_GET(r->mode_xattr));
			up_write(&s->row->io_lock);
			RFS_ROW_MARK_DIRTY(s->row);
			atomic64_set(&s->row->acc_stamp, ktime_get_ns());
		}
		mark_inode_dirty(inode);
	}
	return err;
}

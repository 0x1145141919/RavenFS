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

#include "ravenfs.h"

/* ── 8K 节点 / 跨块字节读 ── */

/* ── 8K 节点读取已移至 mdata_cache.c（走 mdata_piece_cache_mgr + session） ── */

/* 跨块读任意字节（off 为块内偏移） */
static int ravenfs_read_bytes(struct super_block *sb, u64 fs_blk, u32 off,
			      u32 len, void *out)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u8 *p = out;

	while (len) {
		struct buffer_head *bh;
		u32 n;

		if (fs_blk >= sbi->total_blocks)
			return -EIO;
		bh = sb_bread(sb, ravenfs_dev_blk(sbi, fs_blk));
		if (!bh)
			return -EIO;
		n = min_t(u32, len, RAVENFS_BLOCK_SIZE - off);
		memcpy(p, bh->b_data + off, n);
		brelse(bh);
		p += n;
		len -= n;
		fs_blk++;
		off = 0;
	}
	return 0;
}

/* 物化 dentry 名字：≤40 内联；>40 从名字堆读尾部（full_char_remain_ptr 给物理位置） */
static int ravenfs_dentry_name(struct super_block *sb, const struct Dentry_t *e,
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
			       const struct Bptree_dentry_leaf_node_t *leaf,
			       u32 cnt, const char *name, u32 nlen, u64 hash, u64 *ino)
{
	char *nm = kmalloc(RAVENFS_MAX_NAME_LEN, GFP_KERNEL);
	u32 i;

	if (!nm)
		return -ENOMEM;
	*ino = (u64)PTR_INVALID;
	for (i = 0; i < cnt; i++) {
		const struct Dentry_t *e = &leaf->entries[i];
		unsigned int en;
		int c, cmplen;

		if (e->hash > hash)		/* 叶按 hash 严格递增 → 越过即 miss */
			break;
		if (e->hash != hash)
			continue;
		if (ravenfs_dentry_name(sb, e, nm, &en)) {
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
			const struct Bptree_dentry_inner_node_t *nd = buf;
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
		blk = RFS_PTR_ADDR(((const struct Bptree_dentry_inner_node_t *)buf)->hints[0].sub_ptr);
	}
	kfree(buf);
	return err;
}

/* 输出一个叶子的条目 [from, cnt)；成功一条就推进 cookie，缓冲满则停在「上一个已发条目」 */
static int ravenfs_emit_leaf(struct super_block *sb, const void *buf, u64 blk, u32 cnt,
			     u32 from, struct dir_context *ctx, bool *stopped)
{
	char *nm = kmalloc(RAVENFS_MAX_NAME_LEN, GFP_KERNEL);
	const struct Bptree_dentry_leaf_node_t *lf = buf;
	u32 i;
	int err = 0;

	if (!nm)
		return -ENOMEM;
	for (i = from; i < cnt; i++) {
		const struct Dentry_t *e = &lf->entries[i];
		unsigned int en;
		u64 ino = RFS_PTR_ADDR(e->inode_ptr);

		err = ravenfs_dentry_name(sb, e, nm, &en);
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

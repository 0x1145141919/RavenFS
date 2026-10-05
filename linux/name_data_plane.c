// SPDX-License-Identifier: GPL-2.0
/*
 * name_data_plane.c —— 目录名字数据面（name data plane）内核实现（v1.3）
 *
 * 逐接口对照 god `juvenile_god/name_data_plane.cpp` 翻译（内核适配重写）。
 * 契约/模型见 `include/linux/ravenfs_name_data_plane.h` 与
 * `Docs/disk_layout_v1.3_name_data_plane.md`。
 *
 * 内核适配要点（⚠️ 与 god 的差异）：
 *   · god 直接开 `InodeSession` / `mdata_piece_session`；ko 统一走
 *     `ravenfs_session_get` / `ravenfs_mdata_get`。
 *   · fr 层（`rfs_ndfr_*`）零直接 I/O：节点 pin/alloc 经注入 ops vtable。
 *     本文件是**内核侧 ops shim**：node_pin/node_unpin 包 `ravenfs_mdata_get/put`
 *     （dirty 经 NODE_DIRTY 标脏），node_alloc/node_free 包运行时树分配器。
 *     fr 可能**同时** pin 多枚不同节点（叶裂级联 / free_tree 沿树高），故 host
 *     持一个按 blk 去重的定长并发 pin 表；超限或同块双 pin 干净失败。
 *   · god 的 4K 块读写 → 本文件用 mdata 8K 片 read-modify-write（字节粒度，
 *     tail ≤512B 至多跨 2 个 4K 块，且不会跨 8K 片边界，见 fr 分配不变量）。
 *   · fr / 映射树改写的 inode 控制字由本层统一标脏（god 在 InodeSession 内自标）。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/timekeeping.h>

#include "ravenfs.h"
#include "ravenfs_cache.h"
#include "ravenfs_mdata_cache.h"
#include "ravenfs_allocator_top.h"
#include "ravenfs_extent.h"
#include "ravenfs_name_data_freeranges.h"
#include "ravenfs_name_data_plane.h"

/* ══════════════════════ fr 内核 ops shim ══════════════════════ */

/*
 * 并发 pin 上限：free_tree 沿树高递归持 pin（深度 ≤64），插入级联/分裂瞬态另需
 * 数枚（叶 + 池节点 + 父 + 祖父…）→ 96 有充分裕量。
 */
#define RFS_ND_MAX_PINS		96

struct rfs_nd_host {
	struct super_block *sb;
	struct {
		u64				blk;
		struct ravenfs_msession		*ms;
	} pins[RFS_ND_MAX_PINS];
	int	nr;
};

static void nd_host_init(struct rfs_nd_host *h, struct super_block *sb)
{
	h->sb = sb;
	h->nr = 0;
}

/* 归还本层仍持有的全部 pin（正确路径应为 0；错误残留也在此兜底释放，防会话泄漏） */
static void nd_host_fini(struct rfs_nd_host *h)
{
	while (h->nr > 0) {
		h->nr--;
		ravenfs_mdata_put(h->pins[h->nr].ms);
	}
}

static struct ravenfs_msession *nd_host_find(struct rfs_nd_host *h, u64 blk,
					     int *idx_out)
{
	int i;

	for (i = 0; i < h->nr; i++) {
		if (h->pins[i].blk == blk) {
			if (idx_out)
				*idx_out = i;
			return h->pins[i].ms;
		}
	}
	return NULL;
}

static void *nd_node_pin(void *host, u64 blk, bool *err)
{
	struct rfs_nd_host *h = host;
	struct ravenfs_msession *ms;

	if (h->nr >= RFS_ND_MAX_PINS || nd_host_find(h, blk, NULL)) {
		*err = true;			/* 超限 / 同块双 pin：干净失败 */
		return NULL;
	}
	ms = ravenfs_mdata_get(h->sb, blk);
	if (IS_ERR(ms)) {
		*err = true;
		return NULL;
	}
	h->pins[h->nr].blk = blk;
	h->pins[h->nr].ms = ms;
	h->nr++;
	return ravenfs_msession_node(ms);
}

static void nd_node_unpin(void *host, u64 blk, void *node, bool dirty)
{
	struct rfs_nd_host *h = host;
	int idx = -1;
	struct ravenfs_msession *ms = nd_host_find(h, blk, &idx);

	(void)node;
	if (!ms)
		return;
	if (dirty)
		NODE_DIRTY(ms);
	if (idx != h->nr - 1)			/* 乱序 unpin：尾部补位 */
		h->pins[idx] = h->pins[h->nr - 1];
	h->nr--;
	ravenfs_mdata_put(ms);
}

static u64 nd_node_alloc(void *host, bool *err)
{
	struct rfs_nd_host *h = host;
	struct ravenfs_sb_info *sbi = h->sb->s_fs_info;
	u64 b = rfs_allocator_block_alloc_hard(sbi->alloc, 2, 1);

	if (b == (u64)PTR_INVALID) {
		*err = true;
		return RFS_NDFR_NIL;
	}
	return b;
}

static int nd_node_free(void *host, u64 blk)
{
	struct rfs_nd_host *h = host;
	struct ravenfs_sb_info *sbi = h->sb->s_fs_info;

	ravenfs_mdata_drop(h->sb, blk);		/* 防陈旧脏行回写污染复用块 */
	return rfs_allocator_block_free(sbi->alloc, blk, 2);
}

static const struct rfs_ndfr_ops rfs_nd_ndfr_ops = {
	.node_pin	= nd_node_pin,
	.node_unpin	= nd_node_unpin,
	.node_alloc	= nd_node_alloc,
	.node_free	= nd_node_free,
};

static void nd_fr_ctx_init(struct rfs_ndfr_ctx *ctx, struct rfs_nd_host *host,
			   u64 dir_idx)
{
	ctx->ops = &rfs_nd_ndfr_ops;
	ctx->host = host;
	ctx->dir_inode_idx = dir_idx;
}

/* ══════════════════════ 8K 片字节读写（mdata read-modify-write）══════════════════════ */

/* 跨 4K/8K 边界的任意字节读（走 mdata 8K 片缓存，与写路径同源可见） */
static int nd_read_bytes(struct super_block *sb, u64 fs_blk, u32 off,
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

/* 跨 4K/8K 边界的任意字节写（read-modify-write + 标脏） */
static int nd_write_bytes(struct super_block *sb, u64 fs_blk, u32 off,
			  u32 len, const void *in)
{
	const u8 *p = in;

	while (len) {
		u64 piece = fs_blk & ~1ULL;
		u32 in_piece = (u32)((fs_blk - piece) * RAVENFS_BLOCK_SIZE + off);
		u32 n;
		struct ravenfs_msession *ms __free(ravenfs_msession) =
			ravenfs_mdata_get(sb, piece);

		if (IS_ERR(ms))
			return PTR_ERR(ms);
		n = min_t(u32, len, RAVENFS_MDATA_NODE_BYTES - in_piece);
		memcpy((char *)ravenfs_msession_node(ms) + in_piece, p, n);
		NODE_DIRTY(ms);
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

static inline void nd_set_addr(struct ptr_in_idx *p, u64 addr)
{
	p->v = RFS_PTR_PACK(0, addr);
}

static inline bool nd_is_map_node(const struct Bptree_meta_t *m)
{
	return m->magics[0] == NAMEDATA_EXTENT_MAGIC1 &&
	       m->magics[1] == NAMEDATA_EXTENT_MAGIC2 &&
	       m->magics[2] == NAMEDATA_EXTENT_MAGIC3 &&
	       m->magics[3] == NAMEDATA_EXTENT_MAGIC4;
}

/* ══════════════════════ name_data_resolve ══════════════════════
 * 逻辑偏移 → 物理块 + 块内偏移。dino 由调用方持会话（本函数零 I/O 之外）。
 */
int ravenfs_nd_resolve(struct super_block *sb, struct raven_dir_Inode_t *dino,
		       u64 offset, u64 *out_fblk, u16 *out_off)
{
	const u64 ctl = dino->name_data_ctl;
	const u64 ptr = RFS_ND_CTL_PTR(ctl);
	u64 pieces;

	if (!out_fblk || !out_off)
		return -EINVAL;
	if (ptr == RFS_ND_CTL_PTR_INVALID)
		return -EIO;			/* 空态 */

	/* 逻辑上界：偏移越界 = 磁盘偏移字段损坏（防御性拒绝）。 */
	pieces = RFS_ND_PIECE_COUNT(dino->name_data_piece_count);
	if (offset >= pieces * 8192)
		return -EIO;

	if (RFS_ND_CTL_IS_DIRECT(ctl)) {	/* 单连续段直算 */
		*out_fblk = ptr + (offset >> 12);
		*out_off = (u16)(offset & 0xFFF);
		return 0;
	}

	{
		const u64 lblock = offset >> 12;
		struct extent_t e;
		u64 next = 0;
		int rc = ravenfs_extent_lookup_kind(sb, RFS_EXT_KIND_NAMEDATA,
						    ptr, lblock, &e, &next);

		if (rc != 0)
			return -EIO;
		if (lblock < e.fblkbase || lblock >= e.fblkbase + e.length)
			return -EIO;
		*out_fblk = RFS_PTR_ADDR(e.block_base) + (lblock - e.fblkbase);
		*out_off = (u16)(offset & 0xFFF);
		return 0;
	}
}

/* ══════════════════════ name_data_map_append ══════════════════════
 * 追加一个 8K 数据片：direct 先物化映射树根（把现有单段整条入树再插新片，
 * 控制字转 tree）；tree 直接在逻辑尾插入（根分裂写回）。成功 piece_count+1。
 */
int ravenfs_nd_map_append(struct super_block *sb, u64 dir_idx,
			  struct raven_dir_Inode_t *dino, u64 phys_base)
{
	const u64 ctl = dino->name_data_ctl;
	const u64 pieces = RFS_ND_PIECE_COUNT(dino->name_data_piece_count);
	struct extent_t nx;
	u64 root;

	nx.length = 2;			/* 8K = 2 FS_block */
	nx.fblkbase = pieces * 2;	/* 逻辑尾 */
	nd_set_addr(&nx.block_base, phys_base);

	if (RFS_ND_CTL_IS_DIRECT(ctl)) {
		const u64 base = RFS_ND_CTL_PTR(ctl);
		struct extent_t first;
		int rc;

		root = (u64)PTR_INVALID;
		rc = ravenfs_extent_new_root_leaf_kind(sb, RFS_EXT_KIND_NAMEDATA,
						       dir_idx, &root);
		if (rc)
			return rc;
		first.length = pieces * 2;	/* 现有单连续段整条入树 */
		first.fblkbase = 0;
		nd_set_addr(&first.block_base, base);
		rc = ravenfs_extent_insert_kind(sb, RFS_EXT_KIND_NAMEDATA, dir_idx,
						&root, &first, NULL);
		if (rc) {
			ravenfs_extent_orphan_release_kind(sb, RFS_EXT_KIND_NAMEDATA, root);
			return rc;
		}
		rc = ravenfs_extent_insert_kind(sb, RFS_EXT_KIND_NAMEDATA, dir_idx,
						&root, &nx, NULL);
		if (rc) {
			ravenfs_extent_orphan_release_kind(sb, RFS_EXT_KIND_NAMEDATA, root);
			return rc;
		}
		RFS_ND_CTL_SET_PTR(dino->name_data_ctl, root);
		RFS_ND_CTL_SET_DIRECT(dino->name_data_ctl, 0);	/* 转 tree */
	} else {
		int rc;

		root = RFS_ND_CTL_PTR(ctl);
		rc = ravenfs_extent_insert_kind(sb, RFS_EXT_KIND_NAMEDATA, dir_idx,
						&root, &nx, NULL);
		if (rc)
			return rc;
		RFS_ND_CTL_SET_PTR(dino->name_data_ctl, root);	/* 根分裂后写回 */
	}

	RFS_ND_PIECE_SET_COUNT(dino->name_data_piece_count, pieces + 1);
	return 0;
}

/* ══════════════════════ name_data_grow（dino 版，供 alloc 复用）══════════════════════ */
static int nd_grow_core(struct super_block *sb, u64 dir_idx,
			struct raven_dir_Inode_t *dino)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	const u64 pieces = RFS_ND_PIECE_COUNT(dino->name_data_piece_count);
	const u64 ctl = dino->name_data_ctl;
	u64 new_base;
	int rc;

	if (RFS_ND_CTL_PTR(ctl) == RFS_ND_CTL_PTR_INVALID)
		return -EIO;			/* 空态不应增片（先 bootstrap） */

	if (RFS_ND_CTL_IS_DIRECT(ctl)) {
		const u64 base = RFS_ND_CTL_PTR(ctl);
		const u64 tail_blk = base + pieces * 2;
		u64 rest = 0;

		if (rfs_allocator_block_occupy_soft(sbi->alloc, tail_blk, 2, &rest) == 0) {
			if (rest == 0) {	/* 段尾连续：保持 direct */
				RFS_ND_PIECE_SET_COUNT(dino->name_data_piece_count,
						       pieces + 1);
				return 0;
			}
			if (rest < 2)		/* 回滚部分占领 */
				(void)rfs_allocator_block_free(sbi->alloc,
							       tail_blk, 2 - rest);
		}
	}

	new_base = rfs_allocator_block_alloc_hard(sbi->alloc, 2, 1);
	if (new_base == (u64)PTR_INVALID)
		return -ENOSPC;
	rc = ravenfs_nd_map_append(sb, dir_idx, dino, new_base);
	if (rc) {
		(void)rfs_allocator_block_free(sbi->alloc, new_base, 2);
		return rc;
	}
	return 0;
}

int ravenfs_nd_grow(struct super_block *sb, u64 dir_idx)
{
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct raven_dir_Inode_t *dino;
	int rc;

	ds = ravenfs_session_get(sb, dir_idx);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	dino = (struct raven_dir_Inode_t *)ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR)
		return -EINVAL;

	rc = nd_grow_core(sb, dir_idx, dino);
	if (rc == 0)
		RFS_ROW_MARK_DIRTY(ds->row);
	return rc;
}

/* ══════════════════════ name_data_alloc ══════════════════════
 * 空态建首片 direct + 整片入 fr → fr best-fit 复用 → 用尽则增片取新片头部、
 * 余段入 fr。返回逻辑字节偏移。
 */
int ravenfs_nd_alloc(struct super_block *sb, u64 dir_idx, u16 tail_len,
		     u64 *out_offset)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct raven_dir_Inode_t *dino;
	struct rfs_nd_host host;
	struct rfs_ndfr_ctx ctx;
	int rc;

	if (!out_offset || tail_len == 0 || tail_len > 512)
		return -EINVAL;

	ds = ravenfs_session_get(sb, dir_idx);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	dino = (struct raven_dir_Inode_t *)ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR)
		return -EINVAL;

	nd_host_init(&host, sb);
	nd_fr_ctx_init(&ctx, &host, dir_idx);

	/* 1. 空态：建首片 direct + 整片 [0,8K) 入 fr */
	if (RFS_ND_CTL_PTR(dino->name_data_ctl) == RFS_ND_CTL_PTR_INVALID) {
		u64 base = rfs_allocator_block_alloc_hard(sbi->alloc, 2, 1);

		if (base == (u64)PTR_INVALID) {
			rc = -ENOSPC;
			goto out;
		}
		dino->name_data_ctl = RFS_ND_CTL_PACK(1, base);	/* direct(1) */
		RFS_ND_PIECE_SET_COUNT(dino->name_data_piece_count, 1);
		rc = rfs_ndfr_insert(&ctx, dino, 0, 8192);
		if (rc) {
			dino->name_data_ctl = RFS_ND_CTL_INACTIVE;
			RFS_ND_PIECE_SET_COUNT(dino->name_data_piece_count, 0);
			(void)rfs_allocator_block_free(sbi->alloc, base, 2);
			goto out;
		}
	}

	/* 2. best-fit 复用 */
	{
		u64 off = 0;

		rc = rfs_ndfr_take_fit(&ctx, dino, tail_len, &off);
		if (rc == 0) {
			RFS_ROW_MARK_DIRTY(ds->row);
			*out_offset = off;
			goto out;
		}
		if (rc != -ENOENT)
			goto out;
	}

	/* 3. 用尽：增片，本 tail 取新片头部，余段入 fr */
	{
		const u64 pieces_before = RFS_ND_PIECE_COUNT(dino->name_data_piece_count);
		const u64 L = pieces_before * 8192;	/* 新片逻辑起点 */

		rc = nd_grow_core(sb, dir_idx, dino);
		if (rc)
			goto out;
		if (8192 > tail_len) {
			rc = rfs_ndfr_insert(&ctx, dino, L + tail_len, 8192 - tail_len);
			if (rc)
				goto out;	/* 已增长片：上报（记账偏差，不悬挂） */
		}
		RFS_ROW_MARK_DIRTY(ds->row);
		*out_offset = L;
	}
out:
	nd_host_fini(&host);
	return rc;
}

/* ══════════════════════ name_data_free ══════════════════════ */
int ravenfs_nd_free(struct super_block *sb, u64 dir_idx, u64 offset, u16 tail_len)
{
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct raven_dir_Inode_t *dino;
	struct rfs_nd_host host;
	struct rfs_ndfr_ctx ctx;
	int rc;

	if (tail_len == 0)
		return 0;

	ds = ravenfs_session_get(sb, dir_idx);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	dino = (struct raven_dir_Inode_t *)ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR)
		return -EINVAL;

	nd_host_init(&host, sb);
	nd_fr_ctx_init(&ctx, &host, dir_idx);
	rc = rfs_ndfr_insert(&ctx, dino, offset, tail_len);
	if (rc == 0)
		RFS_ROW_MARK_DIRTY(ds->row);
	nd_host_fini(&host);
	return rc;
}

/* ══════════════════════ name_data_read / write ══════════════════════ */
int ravenfs_nd_read(struct super_block *sb, u64 dir_idx, u64 offset, u16 tail_len,
		    char *out)
{
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct raven_dir_Inode_t *dino;
	u64 fblk = 0;
	u16 off = 0;
	int rc;

	if (!out || tail_len == 0 || tail_len > 512)
		return -EINVAL;

	ds = ravenfs_session_get(sb, dir_idx);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	dino = (struct raven_dir_Inode_t *)ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR)
		return -EINVAL;

	rc = ravenfs_nd_resolve(sb, dino, offset, &fblk, &off);
	if (rc)
		return rc;
	return nd_read_bytes(sb, fblk, off, tail_len, out);
}

int ravenfs_nd_write(struct super_block *sb, u64 dir_idx, u64 offset, u16 tail_len,
		     const char *tail)
{
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct raven_dir_Inode_t *dino;
	u64 fblk = 0;
	u16 off = 0;
	int rc;

	if (!tail || tail_len == 0 || tail_len > 512)
		return -EINVAL;

	ds = ravenfs_session_get(sb, dir_idx);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	dino = (struct raven_dir_Inode_t *)ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR)
		return -EINVAL;

	rc = ravenfs_nd_resolve(sb, dino, offset, &fblk, &off);
	if (rc)
		return rc;
	return nd_write_bytes(sb, fblk, off, tail_len, tail);
}

/* ══════════════════════ name_data_map_reclaim ══════════════════════
 * 归还一棵名字数据面映射树的数据片 + 节点（不碰 inode 控制字）。
 * 左叶下钻 → 叶链 while 遍历逐条 extent block_free(phys,len) →
 * ravenfs_extent_orphan_release_kind 释放全部节点（只节点 2 块，不碰数据 phys）。
 */
void ravenfs_nd_map_reclaim(struct super_block *sb, u64 tree_root)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	const u64 PTR52 = (u64)RFS_MASK52;
	u64 cur = tree_root;
	u64 guard;
	int d;

	/* ⚠️ 0 是合法块号（v2.0 契约明令禁用 0 作哨兵）：映射树根可能恰为 block 0，
	 * 不得当无效早退（否则整棵映射树 + 数据片漏回收）。 */
	if (tree_root == (u64)PTR_INVALID || tree_root == PTR52)
		return;

	/* 1. 下钻到最左叶（深度受限，防环） */
	for (d = 0; d < 64; d++) {
		struct ravenfs_msession *ms = ravenfs_mdata_get(sb, cur);
		struct Bptree_meta_t *m;
		u64 next;

		if (IS_ERR(ms))
			return;
		m = ravenfs_msession_node(ms);
		if (!nd_is_map_node(m)) {
			ravenfs_mdata_put(ms);
			return;
		}
		if (!RFS_PP_IS_INNER(m->parent_ptr)) {
			ravenfs_mdata_put(ms);
			break;
		}
		if (RFS_PP_COUNT(m->parent_ptr) == 0) {
			ravenfs_mdata_put(ms);
			return;
		}
		next = RFS_PTR_ADDR(((struct Bptree_extent_inner_node_t *)m)
					->extents[0].sub_node_ptr);
		ravenfs_mdata_put(ms);
		cur = next;
	}

	/* 2. 叶链遍历收集物理段（while，链长无上限；护栏防脏指针死循环） */
	for (guard = 0; guard <= sbi->total_blocks / 2; guard++) {
		struct ravenfs_msession *ms = ravenfs_mdata_get(sb, cur);
		const struct Bptree_meta_t *m;
		const struct Bptree_extent_leaf_node_t *lf;
		u64 next;
		u32 cnt, i;

		if (IS_ERR(ms))
			break;
		m = ravenfs_msession_node(ms);
		if (!nd_is_map_node(m) || RFS_PP_IS_INNER(m->parent_ptr)) {
			ravenfs_mdata_put(ms);
			break;
		}
		lf = (const struct Bptree_extent_leaf_node_t *)m;
		cnt = RFS_PP_COUNT(m->parent_ptr);
		for (i = 0; i < cnt; i++)
			(void)rfs_allocator_block_free(sbi->alloc,
					RFS_PTR_ADDR(lf->extents[i].block_base),
					lf->extents[i].length);
		next = RFS_PTR_ADDR(m->ptrs[2]);
		ravenfs_mdata_put(ms);
		if (next == PTR52)
			break;
		cur = next;
	}

	/* 3. 释放全部映射树节点（只节点 2 块/个，不碰数据 phys） */
	ravenfs_extent_orphan_release_kind(sb, RFS_EXT_KIND_NAMEDATA, tree_root);
}

/* ══════════════════════ name_data_release ══════════════════════
 * 目录销毁：数据片 + 映射树节点 + fr 树节点全量回收 + 双控制字复位空态。
 */
int ravenfs_nd_release(struct super_block *sb, u64 dir_idx)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct raven_dir_Inode_t *dino;
	struct rfs_nd_host host;
	struct rfs_ndfr_ctx ctx;
	u64 ctl, pieces, ptr;

	ds = ravenfs_session_get(sb, dir_idx);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	dino = (struct raven_dir_Inode_t *)ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(dino->word1) != INODE_TYPE_DIR)
		return -EINVAL;

	/* fr 树节点先退役（无论数据面是否为空，防御性） */
	nd_host_init(&host, sb);
	nd_fr_ctx_init(&ctx, &host, dir_idx);
	(void)rfs_ndfr_release(&ctx, dino);
	nd_host_fini(&host);

	ctl = dino->name_data_ctl;
	pieces = RFS_ND_PIECE_COUNT(dino->name_data_piece_count);
	ptr = RFS_ND_CTL_PTR(ctl);

	if (ptr == RFS_ND_CTL_PTR_INVALID && pieces == 0) {
		RFS_ROW_MARK_DIRTY(ds->row);	/* fr 复位落盘 */
		return 0;
	}

	if (RFS_ND_CTL_IS_DIRECT(ctl))
		(void)rfs_allocator_block_free(sbi->alloc, ptr, pieces * 2);
	else
		ravenfs_nd_map_reclaim(sb, ptr);

	/* 复位名字数据面控制字（空态） */
	dino->name_data_ctl = RFS_ND_CTL_INACTIVE;
	dino->name_data_piece_count = RFS_ND_PIECE_PACK(0);
	dino->name_data_free_ranges_ctl = RFS_ND_FR_CTL_INACTIVE;
	dino->name_data_free_ranges_count = 0;
	memset(dino->possible_inline_free_ranges, 0,
	       sizeof(dino->possible_inline_free_ranges));
	RFS_ROW_MARK_DIRTY(ds->row);
	return 0;
}

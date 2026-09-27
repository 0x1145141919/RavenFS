// SPDX-License-Identifier: GPL-2.0
/*
 * name_heap.c —— 长名（>40B）外置部分的 分配/释放/读写（v1.2 盘面 §7）
 *
 * 语义出处：god `juvenile_god/name_heap.cpp`（**逐式移植**，内核适配重写）。
 * 迁移规格：`Docs/linux/ravenfs-linux-ko-handover.md` §3.6。
 *
 * 结构：每目录 inode 内联 10 个 `name_heap_controller_t`（见 raven_dir_Inode_t）；
 *   档位级懒激活：激活 = **位图面 + 数据面**各一次连续分配（2^1 对齐 ⇒ mdata 8K 片成立）+ 位图面清零。
 *   位图 bit[i] 管辖数据区字节 [i×2^gran, (i+1)×2^gran)；物理 = data_base + i×2^gran。
 *   分配 = 找 **N 个连续空闲 bit** 置位；释放 = 区间清位。数据面不收缩（回收见 dir_name_heap_release，M5）。
 *
 * ⚠️ 与 god 的差异（内核适配）：
 *   · god 把整个位图面（≤4 片 = 32KB）载进**栈缓冲**扫描；内核栈放不下 →
 *     改 `kvmalloc` 临时缓冲（更小档位也是；路径冷，可接受）。
 *   · god 的 `bitmap_base` 视图轮子 → 直接对 u64 字做位运算。
 *   · `mdata_piece_session` → ko `RAVENFS_MSESSION`；`block_alloc_hard/free` → ko bitmap.c。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/timekeeping.h>

#include "ravenfs.h"

/* 10 槽静态几何（与 raven_dir_Inode_t 字段序一一对应；disk_layout_v1.2 §7.4 容量表） */
static const u8 rfs_heap_gran_log2[10] = { 3, 3, 3, 6, 6, 7, 7, 8, 8, 9 };	/* 8B/64B/128B/256B/512B */
static const u8 rfs_heap_bits_log2[10] = { 16, 17, 18, 16, 17, 16, 17, 16, 17, 16 };	/* 8k/16k/32k */
/* 粒度组槽号区间 [lo, hi)：8B / 64B / 128B / 256B / 512B */
static const u8 rfs_heap_group_lo[5] = { 0, 3, 5, 7, 9 };
static const u8 rfs_heap_group_hi[5] = { 3, 5, 7, 9, 10 };

#define RFS_HEAP_BITS_PER_PIECE	65536ULL	/* 8K 片 × 8 */

static inline u64 rfs_heap_bmp_blocks(u8 bits_log2)
{
	return 1ULL << (bits_log2 - 15);	/* 2/4/8 */
}
static inline u64 rfs_heap_data_blocks(u8 bits_log2, u8 gran_log2)
{
	return 1ULL << (bits_log2 + gran_log2 - 12);
}
static inline u64 rfs_heap_total_bits(u8 bits_log2)
{
	return 1ULL << bits_log2;
}

/* 尾部长度（= name_len − 40，∈[1,512]）→ 粒度组 + 单元数 + 粒度 log2 */
static u32 rfs_heap_pick(u32 tail_len, u32 *units, u8 *gran_log2)
{
	if (tail_len <= 40)  { *units = (tail_len + 7) / 8; *gran_log2 = 3; return 0; }
	if (tail_len <= 64)  { *units = 1; *gran_log2 = 6; return 1; }
	if (tail_len <= 128) { *units = 1; *gran_log2 = 7; return 2; }
	if (tail_len <= 256) { *units = 1; *gran_log2 = 8; return 3; }
	*units = 1; *gran_log2 = 9; return 4;
}

/* 在 [0,total_bits) 找 [start,start+units) 全空闲的首个 start；无 → ~0ULL */
static u64 rfs_heap_find_free(const u64 *w, u64 total_bits, u32 units)
{
	u64 words = total_bits >> 6;
	u64 wi;

	for (wi = 0; wi < words; wi++) {
		u64 avail = ~w[wi];

		while (avail) {
			u64 z = (u64)__builtin_ctzll(avail);
			u64 cand = wi * 64 + z;

			if (cand + units <= total_bits) {
				bool ok = true;
				u32 u;

				for (u = 1; u < units; u++) {
					u64 b = cand + u;

					if ((w[b >> 6] >> (b & 63)) & 1ULL) {
						ok = false;
						break;
					}
				}
				if (ok)
					return cand;
			}
			avail &= avail - 1;	/* 清当前候选，找下一个 0 位 */
		}
	}
	return ~0ULL;
}

/* 激活槽：位图面 + 数据面各一次连续分配（2 块对齐）+ 位图面清零 */
static int rfs_heap_activate(struct super_block *sb, struct raven_Inode_t *r, u32 slot)
{
	struct name_heap_controller_t *ctrl =
		rfs_dir_heap((struct raven_dir_Inode_t *)r, slot);
	u8 gran = rfs_heap_gran_log2[slot];
	u8 bits = rfs_heap_bits_log2[slot];
	u64 bmp_blks = rfs_heap_bmp_blocks(bits);
	u64 data_blks = rfs_heap_data_blocks(bits, gran);
	u64 bmp_base, data_base, p;

	if (!ctrl)
		return -EINVAL;
	if (RFS_NHC_BITMAP(*ctrl) != (u64)NAME_HEAP_INACTIVE)
		return 0;			/* 已激活 */

	bmp_base = ravenfs_block_alloc(sb, bmp_blks, 1);
	if (bmp_base == (u64)PTR_INVALID)
		return -ENOSPC;
	data_base = ravenfs_block_alloc(sb, data_blks, 1);
	if (data_base == (u64)PTR_INVALID) {
		ravenfs_block_free(sb, bmp_base, bmp_blks);
		return -ENOSPC;
	}
	for (p = 0; p < bmp_blks / 2; p++) {
		struct ravenfs_msession *ms __free(ravenfs_msession) =
			ravenfs_mdata_get(sb, bmp_base + p * 2);

		if (IS_ERR(ms)) {
			ravenfs_block_free(sb, bmp_base, bmp_blks);
			ravenfs_block_free(sb, data_base, data_blks);
			return PTR_ERR(ms);
		}
		memset(ravenfs_msession_node(ms), 0, RAVENFS_MDATA_NODE_BYTES);
		RFS_ROW_MARK_DIRTY(ms->row);
	}
	RFS_NHC_SET(*ctrl, gran, bits, bmp_base, 0, data_base);
	return 0;
}

/* 读位图面（≤4 片 = ≤32KB）到临时缓冲 */
static int rfs_heap_load_bitmap(struct super_block *sb, u64 bmp_base, u64 bmp_blks,
				u8 **out)
{
	u64 bytes = bmp_blks * RAVENFS_BLOCK_SIZE;
	u8 *buf = kvmalloc(bytes, GFP_KERNEL);
	u64 p;

	if (!buf)
		return -ENOMEM;
	for (p = 0; p < bmp_blks / 2; p++) {
		struct ravenfs_msession *ms __free(ravenfs_msession) =
			ravenfs_mdata_get(sb, bmp_base + p * 2);

		if (IS_ERR(ms)) {
			kvfree(buf);
			return PTR_ERR(ms);
		}
		memcpy(buf + p * RAVENFS_MDATA_NODE_BYTES, ravenfs_msession_node(ms),
		       RAVENFS_MDATA_NODE_BYTES);
	}
	*out = buf;
	return 0;
}

/*
 * 分配尾部存储。返回 0 时 *out_remain_ptr = (fblk<<12)|offset（写进 Dentry.full_char_remain_ptr）。
 */
int ravenfs_name_heap_alloc(struct super_block *sb, u64 dir_ino, u32 name_len,
			    u64 *out_remain_ptr)
{
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct raven_Inode_t *r;
	u32 tail, units = 0, lo, hi, s;
	u8 gran = 0, group;
	int err = 0;

	if (name_len <= DENTRY_INLINE_NAME_MAX || name_len > RAVENFS_MAX_NAME_LEN)
		return -EINVAL;
	tail = name_len - DENTRY_INLINE_NAME_MAX;
	group = rfs_heap_pick(tail, &units, &gran);
	lo = rfs_heap_group_lo[group];
	hi = rfs_heap_group_hi[group];

	ds = ravenfs_session_get(sb, dir_ino);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	r = ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(r->word1) != INODE_TYPE_DIR)
		return -EINVAL;

	for (s = lo; s < hi; s++) {
		struct name_heap_controller_t *ctrl =
			rfs_dir_heap((struct raven_dir_Inode_t *)r, s);
		u64 bits_log2, total_bits, bmp_blks, bit_start, byte_off, p;
		u8 *buf = NULL;

		if (!ctrl)
			return -EINVAL;
		if (RFS_NHC_BITMAP(*ctrl) == (u64)NAME_HEAP_INACTIVE) {
			err = rfs_heap_activate(sb, r, s);	/* ctrl 指针仍有效（同 session 内存） */
			if (err)
				return err;
		}
		bits_log2 = RFS_NHC_BITS(*ctrl);
		total_bits = rfs_heap_total_bits((u8)bits_log2);
		bmp_blks = rfs_heap_bmp_blocks((u8)bits_log2);

		err = rfs_heap_load_bitmap(sb, RFS_NHC_BITMAP(*ctrl), bmp_blks, &buf);
		if (err)
			return err;
		bit_start = rfs_heap_find_free((const u64 *)buf, total_bits, units);
		if (bit_start == ~0ULL) {		/* 本槽满 → 同粒度下一档 */
			kvfree(buf);
			continue;
		}
		for (p = 0; p < units; p++) {		/* 置位 */
			u64 b = bit_start + p;

			((u64 *)buf)[b >> 6] |= 1ULL << (b & 63);
		}
		for (p = 0; p < bmp_blks / 2; p++) {	/* 回写覆盖到的片 */
			u64 plo = p * RFS_HEAP_BITS_PER_PIECE;
			u64 phi = plo + RFS_HEAP_BITS_PER_PIECE;

			if (bit_start < phi && bit_start + units > plo) {
				struct ravenfs_msession *ms __free(ravenfs_msession) =
					ravenfs_mdata_get(sb, RFS_NHC_BITMAP(*ctrl) + p * 2);

				if (IS_ERR(ms)) {
					kvfree(buf);
					return PTR_ERR(ms);
				}
				memcpy(ravenfs_msession_node(ms), buf + p * RAVENFS_MDATA_NODE_BYTES,
				       RAVENFS_MDATA_NODE_BYTES);
				RFS_ROW_MARK_DIRTY(ms->row);
			}
		}
		kvfree(buf);
		byte_off = bit_start << gran;		/* 物理定位（数据面单一连续区间） */
		*out_remain_ptr = ((RFS_NHC_DATA(*ctrl) + (byte_off >> 12)) << 12) |
				  (byte_off & 0xFFF);
		return 0;
	}
	return -ENOSPC;					/* 该粒度档全部耗尽 */
}

/* 写尾部数据（起点可能在片内第二块高偏移 → 跨 8K 片，≤2 片） */
int ravenfs_name_heap_write(struct super_block *sb, u32 name_len, u64 remain_ptr,
			    const char *tail)
{
	u32 len, offset, in_piece;
	u64 blk, piece;

	if (name_len <= DENTRY_INLINE_NAME_MAX || !tail)
		return -EINVAL;
	len = name_len - DENTRY_INLINE_NAME_MAX;
	blk = remain_ptr >> 12;
	offset = (u32)(remain_ptr & 0xFFF);
	piece = blk & ~1ULL;
	in_piece = (u32)(((blk & 1ULL) << 12) + offset);

	{
		struct ravenfs_msession *ms __free(ravenfs_msession) =
			ravenfs_mdata_get(sb, piece);

		if (IS_ERR(ms))
			return PTR_ERR(ms);
		if (in_piece + len <= RAVENFS_MDATA_NODE_BYTES) {
			memcpy((char *)ravenfs_msession_node(ms) + in_piece, tail, len);
			RFS_ROW_MARK_DIRTY(ms->row);
		} else {
			u32 first = (u32)(RAVENFS_MDATA_NODE_BYTES - in_piece);
			struct ravenfs_msession *ms2 __free(ravenfs_msession);

			memcpy((char *)ravenfs_msession_node(ms) + in_piece, tail, first);
			RFS_ROW_MARK_DIRTY(ms->row);
			ms2 = ravenfs_mdata_get(sb, piece + 2);
			if (IS_ERR(ms2))
				return PTR_ERR(ms2);
			memcpy(ravenfs_msession_node(ms2), tail + first, len - first);
			RFS_ROW_MARK_DIRTY(ms2->row);
		}
	}
	return 0;
}

/* 释放尾部存储（由 dentry 信息反推：尾长定档，块号∈组内某堆数据区=唯一命中） */
int ravenfs_name_heap_free(struct super_block *sb, u64 dir_ino, u32 name_len,
			   u64 remain_ptr)
{
	struct ravenfs_session *ds __free(ravenfs_session) = NULL;
	struct raven_Inode_t *r;
	u32 tail, units = 0, lo, hi, s;
	u8 gran = 0, group;

	if (name_len <= DENTRY_INLINE_NAME_MAX)
		return 0;
	if (remain_ptr == (u64)PTR_INVALID)
		return -EINVAL;
	tail = name_len - DENTRY_INLINE_NAME_MAX;
	group = rfs_heap_pick(tail, &units, &gran);
	lo = rfs_heap_group_lo[group];
	hi = rfs_heap_group_hi[group];

	ds = ravenfs_session_get(sb, dir_ino);
	if (IS_ERR(ds))
		return PTR_ERR(ds);
	r = ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(r->word1) != INODE_TYPE_DIR)
		return -EINVAL;

	for (s = lo; s < hi; s++) {
		struct name_heap_controller_t *ctrl =
			rfs_dir_heap((struct raven_dir_Inode_t *)r, s);
		u64 data_blks, blk = remain_ptr >> 12, byte_off, bit_start, bits_log2, bmp_blks, p;

		if (!ctrl || RFS_NHC_BITMAP(*ctrl) == (u64)NAME_HEAP_INACTIVE)
			continue;
		data_blks = rfs_heap_data_blocks((u8)RFS_NHC_BITS(*ctrl), (u8)RFS_NHC_GRAN(*ctrl));
		if (blk < RFS_NHC_DATA(*ctrl) || blk >= RFS_NHC_DATA(*ctrl) + data_blks)
			continue;			/* 不在本堆数据区 */
		byte_off = (blk - RFS_NHC_DATA(*ctrl)) * RAVENFS_BLOCK_SIZE + (remain_ptr & 0xFFF);
		bit_start = byte_off >> RFS_NHC_GRAN(*ctrl);
		bits_log2 = RFS_NHC_BITS(*ctrl);
		bmp_blks = rfs_heap_bmp_blocks((u8)bits_log2);

		for (p = 0; p < bmp_blks / 2; p++) {	/* 只改覆盖到的片 */
			u64 plo = p * RFS_HEAP_BITS_PER_PIECE;
			u64 phi = plo + RFS_HEAP_BITS_PER_PIECE;
			u32 u;

			if (bit_start >= phi || bit_start + units <= plo)
				continue;
			{
				struct ravenfs_msession *ms __free(ravenfs_msession) =
					ravenfs_mdata_get(sb, RFS_NHC_BITMAP(*ctrl) + p * 2);
				u64 *w;

				if (IS_ERR(ms))
					return PTR_ERR(ms);
				w = ravenfs_msession_node(ms);
				for (u = 0; u < units; u++) {
					u64 bit = bit_start + u;

					if (bit >= plo && bit < phi) {
						u64 local = bit - plo;

						w[local >> 6] &= ~(1ULL << (local & 63));
					}
				}
				RFS_ROW_MARK_DIRTY(ms->row);
			}
		}
		return 0;
	}
	return -EINVAL;					/* 块号不落任何堆数据区（调用方 bug） */
}

/*
 * 释放目录的全部【已空】名字堆（堆回收，god `dir_name_heap_release` 移植）。
 * 遍历 10 个控制器：已激活且位图面无任何置位（全空）→ 位图面 + 数据面归还位图 →
 * 描述符重置未激活哨兵；非空堆跳过（数据面不收缩）。有释放 → session dirty。
 * ⚠ 重置用 `RFS_NHC_SET`（gran/bits=0、bitmap/data=MASK52）；memset 0 是雷（0 是合法块号）。
 */
int ravenfs_dir_name_heap_release(struct super_block *sb, u64 dir_ino)
{
	struct ravenfs_session *ds __free(ravenfs_session) = ravenfs_session_get(sb, dir_ino);
	struct raven_Inode_t *r;
	u32 s;
	bool changed = false;

	if (IS_ERR(ds))
		return PTR_ERR(ds);
	r = ravenfs_session_inode(ds);
	if (RFS_INO1_GET_TYPE(r->word1) != INODE_TYPE_DIR)
		return -EINVAL;

	for (s = 0; s < 10; s++) {
		struct name_heap_controller_t *ctrl =
			rfs_dir_heap((struct raven_dir_Inode_t *)r, s);
		u64 bmp_blks, data_blks, p;
		bool empty = true;

		if (!ctrl || RFS_NHC_BITMAP(*ctrl) == (u64)NAME_HEAP_INACTIVE)
			continue; /* 未激活 */
		bmp_blks = rfs_heap_bmp_blocks((u8)RFS_NHC_BITS(*ctrl));
		data_blks = rfs_heap_data_blocks((u8)RFS_NHC_BITS(*ctrl),
						 (u8)RFS_NHC_GRAN(*ctrl));

		/* 全空判定：位图面全部字节 == 0 */
		for (p = 0; p < bmp_blks / 2 && empty; p++) {
			struct ravenfs_msession *ms __free(ravenfs_msession) =
				ravenfs_mdata_get(sb, RFS_NHC_BITMAP(*ctrl) + p * 2);
			const u8 *b;
			u32 i;

			if (IS_ERR(ms))
				return PTR_ERR(ms);
			b = ravenfs_msession_node(ms);
			for (i = 0; i < RAVENFS_MDATA_NODE_BYTES && empty; i++)
				if (b[i])
					empty = false;
		}
		if (!empty)
			continue;

		ravenfs_block_free(sb, RFS_NHC_BITMAP(*ctrl), bmp_blks);
		ravenfs_block_free(sb, RFS_NHC_DATA(*ctrl), data_blks);
		RFS_NHC_SET(*ctrl, 0, 0, (u64)NAME_HEAP_INACTIVE, 0, (u64)NAME_HEAP_INACTIVE);
		changed = true;
	}
	if (changed) {
		RFS_ROW_MARK_DIRTY(ds->row);
	}
	return 0;
}

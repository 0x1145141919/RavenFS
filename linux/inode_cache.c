// SPDX-License-Identifier: GPL-2.0
/*
 * inode_cache.c —— RavenFS ko 的 disk_inode_cache_mgr + inode_session
 *
 * 依据：Docs/linux/ravenfs-linux-ko-requirements-spec.md §4.11.1（基调修订）
 *      + Docs/linux/ravenfs-ko-cache-statemachine.md（缓存行状态机，2026-09-27）
 *   · 只管**盘 inode 热数据集**（256B 记录）；与 VFS logical inode 解耦。
 *   · 分片 xarray + 片级睡眠 rw_semaphore；分片几何 2^n inode/片（O(1) 位切）。
 *   · 驱逐三路解耦：冷集 → **自写 TTL 淘汰器**（`linux/cache_ttl.c`，主轴）；
 *     折构/卸载 → flush_all() 主动全量刷回。（压力 shrinker 已移除：2026-09-26 深夜）
 *
 * 【状态机（2026-09-27 补票）】
 *   行 = {ref(会话/pin), io_lock(内容+flags), state(READY/EVICTING),
 *          acc_stamp(访问→TTL), wb_stamp(回写 aging), flags(DIRTY/WB)}。
 *   铁律：**I/O 一律在锁外**——驻留回写 copy-out 排他后锁外落盘（删 gen）；
 *         淘汰独占窗口原地写 + 释放。详见 Docs/linux/ravenfs-ko-cache-statemachine.md。
 *
 * 【M2.2 行尺寸解耦（2026-09-26 定）】
 *   · 缓存行大小由 **mount option `row_size=`** 决定，支持 4K/8K/16K/32K/64K 五档；
 *     行 = 行尺寸/4096 个连续 FS_block（1/2/4/8/16 块）。
 *   · **尾部不完全行**：行覆盖 `[line*ipr, (line+1)*ipr)`；当该区间越过 inode array 末尾时，
 *     按实际存在的块数截断读取（其余零填充），**绝不越界读盘**；行内 inode 槽位仍以
 *     `inum < inode_count` 硬拦（上层 session_get 已保证）。
 *   · 行尺寸只是**内核侧装载粒度**，盘上仍是 256B/inode 连续布局 ⇒ 换行尺寸仍读同一盘。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/xarray.h>
#include <linux/rwsem.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/timekeeping.h>

#include "ravenfs.h"
#include "ravenfs_file.h"
#include "ravenfs_cache.h"
#include "ravenfs_bitmap.h"

/* ── 分片/行几何（与 §4.11.1 + M2.2 逐式一致） ── */
static void ravenfs_geo_init(struct ravenfs_cache *c, u64 inode_count, u32 row_size)
{
	u32 bits = (inode_count <= 1) ? 1 : fls64(inode_count - 1);	/* ceil_log2 */

	c->row_size = row_size;
	c->inodes_per_row = row_size >> 8;		/* 256B/inode */
	c->row_blocks = row_size >> 12;			/* 4K/块 */
	c->row_bits = ilog2(c->inodes_per_row);		/* inodes_per_row 为 2 的幂 */
	c->line_mask = c->inodes_per_row - 1;

	c->bits_len = bits;

	/* ① 先定行：全盘 inode 比一行还少 → 收窄行（唯一残留的收窄情形） */
	if (c->row_bits > bits) {
		c->row_bits = bits;
		c->inodes_per_row = 1u << c->row_bits;
		c->row_size = c->inodes_per_row << 8;
		c->row_blocks = c->row_size >> 12;
		c->line_mask = c->inodes_per_row - 1;
		pr_warn("ravenfs: row_size 超全盘 inode 容量，收窄为 %uB\n", c->row_size);
	}
	/* ② 再定分片：w0 自适应，保证每片容量 ≥ 行容量（否则片内行键位宽为负） */
	c->w0 = RAVENFS_SHARD_SEL_BITS;
	if (c->w0 > bits - c->row_bits)
		c->w0 = bits - c->row_bits;	/* 可至 0 → 单片 */
	c->shard_bits = bits - c->w0;		/* 每片 inode 数 = 2^shard_bits */
	c->row_bits_shard = c->shard_bits - c->row_bits;	/* ≥0（由②保证） */
	c->row_mask = (c->row_bits_shard >= BITS_PER_LONG) ? ~0UL
							   : ((1UL << c->row_bits_shard) - 1);
	c->n_slices = 1u << c->w0;
}

int ravenfs_cache_create(struct super_block *sb, unsigned int row_size)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_cache *c;
	u32 i;

	if (!row_size)
		row_size = RAVENFS_ROW_DEFAULT;
	if (!RAVENFS_ROW_SIZE_OK(row_size))
		return -EINVAL;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	ravenfs_geo_init(c, sbi->inode_count, row_size);
	/* 行缓冲直接走伙伴系统 ⇒ 行字节数必须 ≥ 1 页（4K）。支持表已保证；
	 * 收窄仅见于 inode_count≤8 的病态盘（非良定义，显式拒绝）。 */
	if (c->row_size < PAGE_SIZE) {
		pr_err("ravenfs: row_size %u < PAGE_SIZE，伙伴分配不可用\n", row_size);
		kfree(c);
		return -EINVAL;
	}
	c->order = RAVENFS_ORDER_OF(c->row_size);
	c->shards = kcalloc(c->n_slices, sizeof(*c->shards), GFP_KERNEL);
	if (!c->shards) {
		kfree(c);
		return -ENOMEM;
	}
	for (i = 0; i < c->n_slices; i++) {
		xa_init(&c->shards[i].rows);
		init_rwsem(&c->shards[i].lock);
	}
	sbi->icache = c;
	pr_info("ravenfs: inode cache: row=%uB(order=%u, %u inode, %u blk) bits=%u w0=%u shards=%u(片容 %u inode) 片内行位=%u\n",
		c->row_size, c->order, c->inodes_per_row, c->row_blocks,
		c->bits_len, c->w0, c->n_slices, 1u << c->shard_bits, c->row_bits_shard);
	return 0;
}

static void ravenfs_row_free(struct ravenfs_cache *c, struct ravenfs_row *row)
{
	__free_pages(row->page, c->order);	/* 归还伙伴系统 */
	kfree(row);
}

/* 脏行落盘：src → row_blocks 个连续 4K 块（尾部按 array 末尾截断）。ro 挂载不得调用。
 * sb_bread（读-改-写）+ mark_buffer_dirty + sync_dirty_buffer：与装载侧同源。 */
static int ravenfs_row_store(struct super_block *sb, struct ravenfs_cache *c,
			     struct ravenfs_row *row, const void *src)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 blk0 = sbi->inode_array_base + row->line * c->row_blocks;
	u32 nblk = c->row_blocks, i;

	if (row->line * c->row_blocks >= sbi->inode_blocks)
		return -EIO;
	if (blk0 + nblk > sbi->inode_array_base + sbi->inode_blocks)
		nblk = (u32)(sbi->inode_array_base + sbi->inode_blocks - blk0);
	for (i = 0; i < nblk; i++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, blk0 + i));

		if (!bh)
			return -EIO;
		memcpy(bh->b_data, (const char *)src + (u64)i * RAVENFS_BLOCK_SIZE,
		       RAVENFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);		/* 确定性：返回即落盘 */
		brelse(bh);
		RFS_STAT_INC(sb, io_write_ops);		/* inode 行写回（元数据写放大）*/
		RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
	}
	return 0;
}

/*
 * 慢路径：独占装载一行。**尾部行截断**：只读到 inode array 实际存在处，
 * 余下零填充（行缓冲恒为 row_size 字节，行内槽位寻址恒定）。
 *
 * ⚠️ 锁纪律（死锁根治）：**睡眠分配 + 块 I/O 一律在分片锁之外完成**，
 * 再进临界区双检插入。
 */
static int ravenfs_row_load(struct super_block *sb, struct ravenfs_cache *c,
			    struct ravenfs_shard *sh, u32 row_key, u64 line,
			    struct ravenfs_row **out)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_row *row, *old;
	u64 blk0 = sbi->inode_array_base + line * c->row_blocks;
	u32 nblk = c->row_blocks, got;
	int err;
	u64 now;

	/* 行首必须落在 inode array 内 */
	if (line * c->row_blocks >= sbi->inode_blocks)
		return -EIO;
	/* 尾部行：截断到 array 末尾 */
	if (blk0 + nblk > sbi->inode_array_base + sbi->inode_blocks)
		nblk = (u32)(sbi->inode_array_base + sbi->inode_blocks - blk0);

	/* —— 锁外：分配 + 读盘（可能睡眠/触发回收） —— */
	row = kzalloc(sizeof(*row), GFP_KERNEL);
	if (!row)
		return -ENOMEM;
	row->page = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, c->order);	/* 零填充，尾部行天然补齐 */
	if (!row->page) {
		kfree(row);
		return -ENOMEM;
	}
	row->data = page_address(row->page);	/* 直映射（物理内存宏访问） */
	for (got = 0; got < nblk; got++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, blk0 + got));

		if (!bh) {
			err = -EIO;
			RFS_STAT_INC(sb, err_meta_io);
			goto fail_unlocked;
		}
		memcpy((char *)row->data + (u64)got * RAVENFS_BLOCK_SIZE,
		       bh->b_data, RAVENFS_BLOCK_SIZE);
		brelse(bh);
		RFS_STAT_INC(sb, io_read_ops);
		RFS_STAT_ADD(sb, io_read_bytes, RAVENFS_BLOCK_SIZE);
	}
	row->line = line;
	init_rwsem(&row->io_lock);
	row->flags = 0;
	row->state = ROW_READY;
	now = ktime_get_ns();
	atomic64_set(&row->acc_stamp, now);
	row->wb_stamp = now;
	refcount_set(&row->ref, 1);			/* 缓存基准引用 */

	/* —— 临界区：仅 xarray 双检 + 插入 —— */
	down_write(&sh->lock);
	old = xa_load(&sh->rows, row_key);
	if (old) {			/* 双检：他人已装载 → 丢弃自己的，复用他人的 */
		if (old->state == ROW_EVICTING) {	/* 淘汰在飞：让上层重试 */
			up_write(&sh->lock);
			ravenfs_row_free(c, row);
			return -EAGAIN;
		}
		refcount_inc(&old->ref);
		up_write(&sh->lock);
		ravenfs_row_free(c, row);
		*out = old;
		RFS_STAT_INC(sb, ic_row_load);	/* 从盘装载行（成功返回） */
		return 0;
	}
	err = xa_insert(&sh->rows, row_key, row, GFP_KERNEL);
	if (err) {
		up_write(&sh->lock);
		goto fail_unlocked;
	}
	refcount_inc(&row->ref);			/* 给本次 session */
	up_write(&sh->lock);
	*out = row;
	RFS_STAT_INC(sb, ic_row_load);	/* 从盘装载行（成功返回） */
	return 0;
fail_unlocked:
	ravenfs_row_free(c, row);
	return err;
}

struct ravenfs_session *ravenfs_session_get(struct super_block *sb, u64 inum)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_cache *c = sbi ? sbi->icache : NULL;
	struct ravenfs_session *s;
	struct ravenfs_shard *sh;
	struct ravenfs_row *row;
	u32 sh_idx, row_key;
	u64 line;

	if (!c || inum >= sbi->inode_count) {
		RFS_STAT_INC(sb, ic_err);
		return ERR_PTR(-EIO);
	}

	sh_idx = (u32)(inum >> c->shard_bits);
	line = inum >> c->row_bits;
	row_key = (u32)((unsigned long)line & c->row_mask);
	sh = &c->shards[sh_idx];

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return ERR_PTR(-ENOMEM);

retry:
	row = NULL;
	/* 快路径：读锁下查表 + 引用（淘汰写锁内方置 EVICTING ⇒ 二者互斥） */
	down_read(&sh->lock);
	row = xa_load(&sh->rows, row_key);
	if (row && row->state == ROW_EVICTING)
		row = NULL;			/* 淘汰接管中：置空 → 重试/重载 */
	else if (row)
		refcount_inc(&row->ref);
	up_read(&sh->lock);

	if (row) {
		atomic64_set(&row->acc_stamp, ktime_get_ns());	/* 访问刷新（TTL） */
		RFS_STAT_INC(sb, ic_hit);	/* 快路径命中（§7.3：与 miss 互斥完备） */
	} else {
		int err = ravenfs_row_load(sb, c, sh, row_key, line, &row);

		if (err == -EAGAIN) {		/* 恰好撞上淘汰：重试 */
			RFS_STAT_INC(sb, ic_retry);	/* 淘汰竞争烈度 */
			cond_resched();
			goto retry;
		}
		if (err) {
			RFS_STAT_INC(sb, ic_miss);
			RFS_STAT_INC(sb, ic_err);
			kfree(s);
			return ERR_PTR(err);
		}
		RFS_STAT_INC(sb, ic_miss);	/* 走 row_load = miss（最终归宿记一次） */
	}

	s->sbi = sbi;
	s->shard = sh;
	s->row = row;
	s->inum = inum;
	s->off_mask = c->line_mask;
	return s;
}

void ravenfs_session_put(struct ravenfs_session *s)
{
	if (!s)
		return;
	if (s->row)
		refcount_dec(&s->row->ref);
	kfree(s);
}

/* ── 写回（M3+2026-09-27）：脏行落盘；失败恢复脏；I/O 一律在锁外 ── */

static inline bool ravenfs_writable(struct super_block *sb)
{
	return !(sb->s_flags & SB_RDONLY);
}

/* 行脏标记统一走 ravenfs.h 的 RFS_ROW_MARK_DIRTY 宏 */

/*
 * 脏行落盘（**持分片写锁**：与 drop/淘汰/get 全程串行——复刻旧 writeback_all 安全锁序；
 * 锁内直接以 row->data 落盘，无需拷贝/pin）。
 *   ignore_age=false → 只写 wb_stamp < age_cutoff 的行（TTL 周期回写）。
 */
int ravenfs_cache_wb_dirty(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_cache *c = sbi ? sbi->icache : NULL;
	int err = 0;
	u32 i;

	if (!c || !ravenfs_writable(sb))
		return 0;
	for (i = 0; i < c->n_slices; i++) {
		struct ravenfs_shard *sh = &c->shards[i];
		unsigned long key;
		void *e;

		down_write(&sh->lock);
		xa_for_each(&sh->rows, key, e) {
			struct ravenfs_row *row = e;

			if (row->state != ROW_READY)
				continue;
			if (!(row->flags & ROWF_DIRTY) || (row->flags & ROWF_WB))
				continue;
			if (!ignore_age && row->wb_stamp >= age_cutoff_ns)
				continue;
			if (ravenfs_row_store(sb, c, row, row->data))
				err = -EIO;
			else {
				row->flags &= ~ROWF_DIRTY;
				row->wb_stamp = ktime_get_ns();
				RFS_STAT_INC(sb, ic_wb_rows);	/* inode 行写回量 */
			}
		}
		up_write(&sh->lock);
	}
	return err;
}

int ravenfs_cache_writeback_all(struct super_block *sb)
{
	return ravenfs_cache_wb_dirty(sb, 0, true);
}

/* 单 inode 行落盘（fsync 用）：持分片写锁（与 drop/淘汰串行） */
int ravenfs_inode_flush(struct super_block *sb, u64 inum)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_cache *c = sbi ? sbi->icache : NULL;
	struct ravenfs_session *s __free(ravenfs_session) = NULL;
	int err = 0;

	if (!c || !ravenfs_writable(sb))
		return 0;
	s = ravenfs_session_get(sb, inum);
	if (IS_ERR(s))
		return PTR_ERR(s);
	down_write(&s->shard->lock);
	if (s->row->state == ROW_READY && (s->row->flags & ROWF_DIRTY)) {
		if (ravenfs_row_store(sb, c, s->row, s->row->data))
			err = -EIO;
		else {
			s->row->flags &= ~ROWF_DIRTY;
			s->row->wb_stamp = ktime_get_ns();
		}
	}
	up_write(&s->shard->lock);
	return err;
}

/*
 * `write_inode` 的实现：**logical inode → mgr 行同步**（不落盘）。
 * 只推 **VFS 拥有的字段**；**盘专有字段**（data_root / inline_data / heaps / xattr_ptr / type / direct）
 * 一律不动（logical inode 不持有它们，扫写会毁盘）。
 * 内容改写在 io_lock 写锁内完成（与 copy-out 回写互斥）。
 */
int ravenfs_inode_sync_from_vfs(struct super_block *sb, struct inode *inode)
{
	struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, inode->i_ino);
	struct raven_Inode_t *r;
	struct timespec64 ts;

	if (IS_ERR(s))
		return PTR_ERR(s);
	down_write(&s->row->io_lock);
	r = ravenfs_session_inode(s);
	r->uid = i_uid_read(inode);
	r->gid = i_gid_read(inode);
	r->mode_xattr = (r->mode_xattr & ~RFS_MODE_MASK) | (inode->i_mode & 0o7777);
	r->link_count = inode->i_nlink;
	r->size = inode->i_size;
	ts = inode_get_mtime(inode);
	r->mtime = ts.tv_sec;
	r->m_ns_stamp = (u32)ts.tv_nsec;
	ts = inode_get_ctime(inode);
	r->ctime = ts.tv_sec;
	r->c_ns_stamp = (u32)ts.tv_nsec;
	ts = inode_get_atime(inode);
	r->atime = ts.tv_sec;
	r->a_ns_stamp = (u32)ts.tv_nsec;
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		__le32 rdev_le = cpu_to_le32(new_encode_dev(inode->i_rdev));

		memcpy(r->inline_data, &rdev_le, sizeof(rdev_le));
	}
	s->row->flags |= ROWF_DIRTY;
	s->row->wb_stamp = ktime_get_ns();
	up_write(&s->row->io_lock);
	return 0;
}

/* FS 内部改完盘记录后：标行脏（落盘交 flush/周期回写） */
void ravenfs_inode_mark_dirty(struct super_block *sb, u64 inum)
{
	struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, inum);

	if (IS_ERR_OR_NULL(s))
		return;
	RFS_ROW_MARK_DIRTY(s->row);
	RFS_STAT_INC(sb, ic_dirty_mark);	/* 脏行产生速率 */
	atomic64_set(&s->row->acc_stamp, ktime_get_ns());
}

/*
 * nlink==0 的 inode 回收（evict_inode 调用）：记录清零（防脏行回写复活）+ 位图槽回收。
 * **只回收 inode 槽**；该 inode 的 data 块 / 区段树由调用方 evict_inode 随后调
 * ravenfs_release_inode_data 释放（**先 inode 后 free**，§10 不变量 25 / handover §6.1）。
 */
void ravenfs_inode_reclaim(struct super_block *sb, u64 inum)
{
	struct ravenfs_session *s __free(ravenfs_session) = ravenfs_session_get(sb, inum);

	RFS_STAT_INC(sb, ic_row_evict);	/* 主动逐出（§3.B 打点口径） */
	if (!IS_ERR_OR_NULL(s)) {
		down_write(&s->row->io_lock);
		memset(ravenfs_session_inode(s), 0, RAVENFS_INODE_SIZE);
		s->row->flags |= ROWF_DIRTY;
		s->row->wb_stamp = ktime_get_ns();
		up_write(&s->row->io_lock);
	}
	ravenfs_inode_free(sb, inum);	/* 位图槽回收（持久化交 sync/umount flush） */
}

/*
 * TTL 淘汰（cache_ttl.c 周期调用）：**单临界区，全程持分片写锁**（复刻旧 reap 安全锁序）：
 * 收 `state==READY ∧ ref==1 ∧ !DIRTY ∧ !WB ∧ 超时` 的行 → 在同一写锁内 erase + free（无 I/O）。
 */
long ravenfs_cache_reap_expired(struct super_block *sb, u64 cutoff_ns)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_cache *c = sbi ? sbi->icache : NULL;
	long freed = 0;
	u32 i;

	if (!c || !ravenfs_writable(sb))
		return 0;
	for (i = 0; i < c->n_slices; i++) {
		struct ravenfs_shard *sh = &c->shards[i];

		for (;;) {
			unsigned long victim[64];
			unsigned int nv = 0, k;
			unsigned long key;
			void *e;

			down_write(&sh->lock);
			xa_for_each(&sh->rows, key, e) {
				struct ravenfs_row *row = e;
				bool pick;

				if (nv >= ARRAY_SIZE(victim))
					break;
				if (row->state != ROW_READY)
					continue;
				if (refcount_read(&row->ref) != 1)
					continue;
				pick = !(row->flags & ROWF_DIRTY) &&
				       !(row->flags & ROWF_WB) &&
				       atomic64_read(&row->acc_stamp) < cutoff_ns;
				if (pick)
					victim[nv++] = key;
			}
			for (k = 0; k < nv; k++) {
				struct ravenfs_row *row = xa_erase(&sh->rows, victim[k]);

				if (row) {
					ravenfs_row_free(c, row);
					freed++;
				}
			}
			up_write(&sh->lock);
			if (nv < ARRAY_SIZE(victim))
				break;
		}
	}
	RFS_STAT_ADD(sb, ic_row_reap, freed);	/* TTL 过期淘汰行数 */
	return freed;
}

/* ── 驱逐：折构/卸载（主动全量刷回） ── */

void ravenfs_cache_flush_all(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_cache *c = sbi ? sbi->icache : NULL;
	u32 i;

	if (!c)
		return;
	ravenfs_cache_writeback_all(sb);	/* 脏行先落盘（ro 挂载 no-op） */
	for (i = 0; i < c->n_slices; i++) {
		struct ravenfs_shard *sh = &c->shards[i];
		unsigned long victim[64];
		unsigned int nv, k;
		unsigned long key;
		void *e;

		do {
			nv = 0;
			down_write(&sh->lock);
			xa_for_each(&sh->rows, key, e) {
				struct ravenfs_row *row = e;

				if (nv >= ARRAY_SIZE(victim))
					break;
				if (refcount_read(&row->ref) != 1)
					pr_warn_ratelimited("ravenfs: inode row %llu 仍被引用(%u)\n",
							    (unsigned long long)row->line,
							    refcount_read(&row->ref));
				victim[nv++] = key;
			}
			for (k = 0; k < nv; k++) {
				struct ravenfs_row *row = xa_erase(&sh->rows, victim[k]);

				if (row)
					ravenfs_row_free(c, row);
			}
			up_write(&sh->lock);
		} while (nv == ARRAY_SIZE(victim));
	}
}

void ravenfs_cache_destroy(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_cache *c = sbi ? sbi->icache : NULL;
	u32 i;

	if (!c)
		return;
	ravenfs_cache_flush_all(sb);
	for (i = 0; i < c->n_slices; i++)
		xa_destroy(&c->shards[i].rows);
	kfree(c->shards);
	kfree(c);
	sbi->icache = NULL;
}

/* 架构无关回刷 driver 的 inode 后端 */
const struct ravenfs_wb_ops ravenfs_wb_inode_ops = {
	.name     = "inode",
	.wb_dirty = ravenfs_cache_wb_dirty,
	.reap     = ravenfs_cache_reap_expired,
};

// SPDX-License-Identifier: GPL-2.0
/*
 * mdata_cache_core.c —— mdata 元数据节点缓存核心（平台中立 · 内核/用户态双编）
 *
 * 契约见 include/linux/ravenfs_mdata_cache_core.h；语义真值 =
 *   god `juvenile_god/mdata_piece_cache.cpp` self_reliat 后端。
 * ioctl 强淘汰对外契约见 Docs/linux/ravenfs-linux-ko-mdata-cache-ioctl-spec.md。
 *
 * 后端：多分片静态基数树（statey 14~52）+ 小盘 CTRL_ARRAY。
 *   · bits <= 13：单分片 + 预分配 row[n_nodes] 叶表；
 *   · 14..52     ：按 statey 切分，每片一棵「懒建 + 可摘空表」静态基数树：
 *                  内部表 = void *[]，叶表 = ravenfs_mrow_simple[]（行内嵌）；
 *   · > 52       ：init 拒绝（-EINVAL）。
 *   · 行 data（8K）仍由注入 pool 面懒分配，淘汰/丢弃/卸载归还。
 *   · 无按行 alloc-free、无内核专属符号：平台差异全在 ravenfs_platform.h。
 *
 * 并发不变量（勿破）：
 *   · 结构锁只有片锁 shard->lock；行内无锁，状态在 ctrl 单原子字上。
 *   · get 快路径短持读锁 locate + CAS 占坑；未命中走写锁 ensure_path +
 *     CAS IDLE→IO_ING，然后**放锁**做 pool.alloc / read_block，最后写锁发布。
 *   · 撞上 IO_ING/EVICTING 一律放锁 + RFS_CPU_RELAX/RFS_YIELD 重试。
 *   · mark_dirty 取片读锁置 F_DIRTY：与 wb/flush 的片写锁互斥，防回写清脏穿透。
 *   · 普通 reap 在**片读锁**内 CAS READY→EVICTING 并用行内 reserved[0] 串链表，
 *     放锁后归还 data、回 IDLE；不摘表。
 *   · 强淘汰两阶段：行回收用片读锁 + 行 CAS（锁外写回/归还 data）；表精简用片
 *     写锁后序 DFS，仅摘「所有槽 state==IDLE」的空表。行非 IDLE 期间叶表被钉住，
 *     因此 read-lock locate / 已占坑行指针始终有效；被摘路径由后续 get 重建。
 *   · wb_dirty / wb_node 持片写锁做 I/O（低频维护动作的取舍，见头注释）。
 *   · fini 统一 flush + DFS 释放残余表。
 */
#include "ravenfs_mdata_cache_core.h"

/* ═══════════════════════════════════════════════════════════════════
 * statey 位域策略表（RADIX 档 14~52，row_bits = 0 版）
 *
 * 逐字复制 god `juvenile_god/mdata_piece_cache.cpp` 的 mdp_statey_14..52：
 *   level_bits[0]     = w0（分片选择位宽，n_slices = 1 << w0）
 *   level_bits[1..]   = 片内静态基数树逐层位宽
 *   不变量：sum(level_bits) == bits（行 = 1 负载，无行内索引位）。
 * 命名加 rfs_ 前缀避免与其它 TU（若将来同时链接）的符号冲突。
 * ═══════════════════════════════════════════════════════════════════ */
static const u8 rfs_mdp_statey_14[] = { 7, 7 };
static const u8 rfs_mdp_statey_15[] = { 8, 7 };
static const u8 rfs_mdp_statey_16[] = { 8, 8 };
static const u8 rfs_mdp_statey_17[] = { 9, 8 };
static const u8 rfs_mdp_statey_18[] = { 7, 5, 6 };
static const u8 rfs_mdp_statey_19[] = { 7, 6, 6 };
static const u8 rfs_mdp_statey_20[] = { 8, 6, 6 };
static const u8 rfs_mdp_statey_21[] = { 8, 6, 7 };
static const u8 rfs_mdp_statey_22[] = { 7, 5, 5, 5 };
static const u8 rfs_mdp_statey_23[] = { 7, 5, 5, 6 };
static const u8 rfs_mdp_statey_24[] = { 7, 5, 6, 6 };
static const u8 rfs_mdp_statey_25[] = { 7, 6, 6, 6 };
static const u8 rfs_mdp_statey_26[] = { 7, 6, 6, 7 };
static const u8 rfs_mdp_statey_27[] = { 8, 6, 6, 7 };
static const u8 rfs_mdp_statey_28[] = { 8, 7, 6, 7 };
static const u8 rfs_mdp_statey_29[] = { 8, 6, 7, 8 };
static const u8 rfs_mdp_statey_30[] = { 8, 6, 8, 8 };
static const u8 rfs_mdp_statey_31[] = { 8, 7, 8, 8 };
static const u8 rfs_mdp_statey_32[] = { 9, 5, 6, 6, 6 };
static const u8 rfs_mdp_statey_33[] = { 9, 6, 6, 6, 6 };
static const u8 rfs_mdp_statey_34[] = { 9, 6, 6, 6, 7 };
static const u8 rfs_mdp_statey_35[] = { 9, 6, 6, 7, 7 };
static const u8 rfs_mdp_statey_36[] = { 9, 6, 7, 7, 7 };
static const u8 rfs_mdp_statey_37[] = { 9, 7, 7, 7, 7 };
static const u8 rfs_mdp_statey_38[] = { 10, 7, 7, 7, 7 };
static const u8 rfs_mdp_statey_39[] = { 10, 7, 7, 7, 8 };
static const u8 rfs_mdp_statey_40[] = { 10, 6, 6, 6, 6, 6 };
static const u8 rfs_mdp_statey_41[] = { 10, 6, 6, 6, 6, 7 };
static const u8 rfs_mdp_statey_42[] = { 10, 6, 6, 6, 7, 7 };
static const u8 rfs_mdp_statey_43[] = { 10, 6, 6, 7, 7, 7 };
static const u8 rfs_mdp_statey_44[] = { 10, 6, 7, 7, 7, 7 };
static const u8 rfs_mdp_statey_45[] = { 10, 7, 7, 7, 7, 7 };
static const u8 rfs_mdp_statey_46[] = { 10, 7, 7, 7, 7, 8 };
static const u8 rfs_mdp_statey_47[] = { 10, 8, 7, 7, 7, 8 };
static const u8 rfs_mdp_statey_48[] = { 10, 8, 7, 7, 8, 8 };
static const u8 rfs_mdp_statey_49[] = { 10, 8, 7, 8, 8, 8 };
static const u8 rfs_mdp_statey_50[] = { 10, 8, 8, 8, 8, 8 };
static const u8 rfs_mdp_statey_51[] = { 10, 8, 8, 8, 8, 9 };
static const u8 rfs_mdp_statey_52[] = { 10, 8, 8, 8, 9, 9 };

struct rfs_mdp_statey_ent {
	const u8	*bits;
	u32		 levels;
};

#define RFS_MDP_STATEY_COUNT \
	(RAVENFS_MDATA_RADIX_MAX_BITS - 14 + 1)
#define RFS_MDP_STATEY_ENT(b)	{ rfs_mdp_statey_##b, (u32)RFS_ARRAY_SIZE(rfs_mdp_statey_##b) }

static const struct rfs_mdp_statey_ent
rfs_mdp_statey[RFS_MDP_STATEY_COUNT] = {
	RFS_MDP_STATEY_ENT(14), RFS_MDP_STATEY_ENT(15),
	RFS_MDP_STATEY_ENT(16), RFS_MDP_STATEY_ENT(17),
	RFS_MDP_STATEY_ENT(18), RFS_MDP_STATEY_ENT(19),
	RFS_MDP_STATEY_ENT(20), RFS_MDP_STATEY_ENT(21),
	RFS_MDP_STATEY_ENT(22), RFS_MDP_STATEY_ENT(23),
	RFS_MDP_STATEY_ENT(24), RFS_MDP_STATEY_ENT(25),
	RFS_MDP_STATEY_ENT(26), RFS_MDP_STATEY_ENT(27),
	RFS_MDP_STATEY_ENT(28), RFS_MDP_STATEY_ENT(29),
	RFS_MDP_STATEY_ENT(30), RFS_MDP_STATEY_ENT(31),
	RFS_MDP_STATEY_ENT(32), RFS_MDP_STATEY_ENT(33),
	RFS_MDP_STATEY_ENT(34), RFS_MDP_STATEY_ENT(35),
	RFS_MDP_STATEY_ENT(36), RFS_MDP_STATEY_ENT(37),
	RFS_MDP_STATEY_ENT(38), RFS_MDP_STATEY_ENT(39),
	RFS_MDP_STATEY_ENT(40), RFS_MDP_STATEY_ENT(41),
	RFS_MDP_STATEY_ENT(42), RFS_MDP_STATEY_ENT(43),
	RFS_MDP_STATEY_ENT(44), RFS_MDP_STATEY_ENT(45),
	RFS_MDP_STATEY_ENT(46), RFS_MDP_STATEY_ENT(47),
	RFS_MDP_STATEY_ENT(48), RFS_MDP_STATEY_ENT(49),
	RFS_MDP_STATEY_ENT(50), RFS_MDP_STATEY_ENT(51),
	RFS_MDP_STATEY_ENT(52),
};

/* ══════════════════════════ 几何 ══════════════════════════ */

/* ceil(log2(n))，n >= 2。用折半递推避免 1ULL<<64 的 UB（不依赖内核位扫描助手）。*/
static u32 rfs_ceil_log2_u64(u64 n)
{
	u32 b = 0;

	while (n > 1) {
		n = (n >> 1) + (n & 1u);
		b++;
	}
	return b;
}

int ravenfs_mdata_geometry(u64 total_blocks, struct ravenfs_mdata_geom *out)
{
	const struct rfs_mdp_statey_ent *ent;
	const u8 *lv;
	u32 bits, total_levels = 0, sum = 0, i, cum = 0;
	u64 n_nodes;

	if (!out || !total_blocks)
		return -EINVAL;

	n_nodes = total_blocks / 2 + (total_blocks & 1u);
	bits = (n_nodes <= 1) ? 1 : rfs_ceil_log2_u64(n_nodes);

	RFS_MEMSET(out, 0, sizeof(*out));
	out->n_nodes = n_nodes;
	out->bits = bits;

	if (bits <= RAVENFS_MDATA_CTRL_ARRAY_MAX_BITS) {
		out->mode = RAVENFS_MDATA_MODE_CTRL_ARRAY;
		out->n_slices = 1;
		out->w0 = 0;
		out->shard_bits = bits;
		out->total_levels = 1;
		out->tree_levels = 0;
		out->leaf_width = (u32)n_nodes;
		out->level_bits[0] = 0;
		return 0;
	}
	if (bits > RAVENFS_MDATA_RADIX_MAX_BITS)
		return -EINVAL;		/* statey 表只到 52 */

	ent = &rfs_mdp_statey[bits - 14];
	lv = ent->bits;
	total_levels = ent->levels;
	if (!total_levels || total_levels > RAVENFS_MDATA_MAX_LEVELS)
		return -EINVAL;
	for (i = 0; i < total_levels; i++)
		sum += lv[i];
	if (sum != bits)		/* 表坏：row_bits != 0，拒绝 */
		return -EINVAL;

	out->mode = RAVENFS_MDATA_MODE_RADIX;
	out->w0 = lv[0];
	out->n_slices = 1u << out->w0;
	out->shard_bits = bits - out->w0;
	out->total_levels = total_levels;
	out->tree_levels = total_levels - 1;
	for (i = 0; i < total_levels; i++)
		out->level_bits[i] = lv[i];
	for (i = 0; i < out->tree_levels; i++) {
		cum += out->level_bits[1 + i];
		out->shift[i] = out->shard_bits - cum;
	}
	out->leaf_width = 1u << out->level_bits[total_levels - 1];

	return 0;
}

static void mcore_apply_geom(struct ravenfs_mdata_cache *c,
			     const struct ravenfs_mdata_geom *g)
{
	u32 i;

	c->n_slices = g->n_slices;
	c->w0 = g->w0;
	c->shard_bits = g->shard_bits;
	c->bits_len = g->bits;
	c->mode = g->mode;
	c->total_levels = g->total_levels;
	c->tree_levels = g->tree_levels;
	c->leaf_width = g->leaf_width;
	for (i = 0; i < RAVENFS_MDATA_MAX_LEVELS; i++) {
		c->level_bits[i] = g->level_bits[i];
		c->shift[i] = g->shift[i];
	}
	c->node_mask = (g->shard_bits >= 64) ? ~0ULL
					    : ((1ULL << g->shard_bits) - 1);
	c->total_blocks = 0;		/* 由 init 填 cfg->total_blocks */
	c->n_nodes = g->n_nodes;
}

/* ══════════════════════════ ctrl 字小助手 ══════════════════════════ */

static inline u32 mrow_state_of(u64 ctrl)
{
	return (u32)(ctrl & RAVENFS_MROW_STATE_MASK);
}

static inline u32 mrow_ref_of(u64 ctrl)
{
	return (u32)((ctrl >> RAVENFS_MROW_REF_SHIFT) & RAVENFS_MROW_REF_MASK);
}

enum { MROW_REF_FULL = -1, MROW_REF_BUSY = 0, MROW_REF_OK = 1 };

/* 占坑：state == READY 且 ref 未饱和时 ref+1（CAS，成功即占坑）。*/
static int mrow_try_ref_inc(struct ravenfs_mrow_simple *row)
{
	u64 old = (u64)RFS_ATOMIC64_READ(&row->ctrl);

	for (;;) {
		u64 new;

		if (mrow_state_of(old) != RAVENFS_MROW_READY)
			return MROW_REF_BUSY;
		if (mrow_ref_of(old) == RAVENFS_MROW_REF_MASK)
			return MROW_REF_FULL;
		new = old + RAVENFS_MROW_REF_ONE;
		if ((u64)RFS_ATOMIC64_CMPXCHG(&row->ctrl, (s64)old,
					      (s64)new) == old)
			return MROW_REF_OK;
		old = (u64)RFS_ATOMIC64_READ(&row->ctrl);
	}
}

/* put：ref--，带下溢保护（ref == 0 时告警并跳过，避免 ~2^32 永不淘汰）。*/
static void mrow_put_ref(struct ravenfs_mrow_simple *row)
{
	u64 old = (u64)RFS_ATOMIC64_READ(&row->ctrl);

	for (;;) {
		u64 new;

		if (mrow_ref_of(old) == 0) {
			RFS_LOG("mdata node %llu put underflow (ref=0)\n",
				(unsigned long long)row->node_idx);
			return;
		}
		new = old - RAVENFS_MROW_REF_ONE;
		if ((u64)RFS_ATOMIC64_CMPXCHG(&row->ctrl, (s64)old,
					      (s64)new) == old)
			return;
		old = (u64)RFS_ATOMIC64_READ(&row->ctrl);
	}
}

/* ══════════════════════════ 表 / 寻址 ══════════════════════════ */

/* 零初始化分配：mem.alloc 契约本来就是零初始化，这里再显式 memset 一次，
 * 使模块对「口头零初始化但实现忘清」的宿主也安全（表分配均在低频路径）。*/
static void *mcore_zalloc(struct ravenfs_mdata_cache *c, u64 bytes)
{
	void *p = c->mem.alloc(c->mem.ctx, bytes);

	if (p)
		RFS_MEMSET(p, 0, bytes);
	return p;
}

/* 片内键：node_idx 去掉分片选择位。*/
static inline u64 mrow_shard_key(const struct ravenfs_mdata_cache *c, u64 node_idx)
{
	return node_idx & c->node_mask;
}

/* 读路径寻址（读锁内）：路径未建返回 NULL；树只增不删，指针稳定。*/
static struct ravenfs_mrow_simple *
mrow_locate(const struct ravenfs_mdata_cache *c,
	    const struct ravenfs_mdata_shard *sh, u64 node_idx)
{
	u64 key, chunk;
	void *cur;
	u32 j;

	if (!sh->root)
		return NULL;

	if (c->mode == RAVENFS_MDATA_MODE_CTRL_ARRAY) {
		struct ravenfs_mrow_simple *leaf = sh->root;

		if (node_idx >= c->n_nodes)
			return NULL;
		return &leaf[node_idx];
	}

	key = mrow_shard_key(c, node_idx);
	cur = sh->root;
	for (j = 0; j < c->tree_levels; j++) {
		u64 width = 1ULL << c->level_bits[1 + j];

		chunk = (key >> c->shift[j]) & (width - 1);
		if (j == c->tree_levels - 1)
			return &((struct ravenfs_mrow_simple *)cur)[chunk];
		cur = ((void **)cur)[chunk];
		if (!cur)
			return NULL;
	}
	return NULL;
}

static u64 mrow_table_bytes(const struct ravenfs_mdata_cache *c, u32 level);

/* 写锁内补建缺失表（无 I/O）。返回 0 / -ENOMEM。*/
static int mrow_ensure_path(struct ravenfs_mdata_cache *c,
			    struct ravenfs_mdata_shard *sh, u64 node_idx)
{
	u64 key, chunk, width;
	void **slot;
	u32 j;

	if (c->mode == RAVENFS_MDATA_MODE_CTRL_ARRAY)
		return sh->root ? 0 : -ENOMEM;

	key = mrow_shard_key(c, node_idx);
	slot = &sh->root;
	for (j = 0; j < c->tree_levels; j++) {
		width = 1ULL << c->level_bits[1 + j];
		chunk = (key >> c->shift[j]) & (width - 1);
		if (!*slot) {
			if (j == c->tree_levels - 1)
				*slot = mcore_zalloc(c,
					width * sizeof(struct ravenfs_mrow_simple));
			else
				*slot = mcore_zalloc(c,
					width * sizeof(void *));
			if (!*slot)
				return -ENOMEM;
			if (j == c->tree_levels - 1)
				RAVENFS_MD_STAT1(c->stat,
						 RAVENFS_MD_TABLE_ALLOC_LEAF);
			else
				RAVENFS_MD_STAT1(c->stat,
						 RAVENFS_MD_TABLE_ALLOC_INTERNAL);
			RAVENFS_MD_STAT(c->stat, RAVENFS_MD_TABLE_ADD_BYTES,
					mrow_table_bytes(c, j));
		}
		if (j == c->tree_levels - 1)
			break;
		slot = &((void **)*slot)[chunk];
	}
	return 0;
}

/* 归还 8K data（调用方须持有该行独占态：IO_ING/EVICTING 或片写锁）。*/
static void mrow_release_data(struct ravenfs_mdata_cache *c,
			      struct ravenfs_mrow_simple *row)
{
	void *data = row->data;

	if (!data)
		return;
	row->data = NULL;
	c->pool.free(c->pool.ctx, data, RAVENFS_MDATA_NODE_BYTES);
	RAVENFS_MD_STAT(c->stat, RAVENFS_MD_RESIDENT_DEL_BYTES,
			RAVENFS_MDATA_NODE_BYTES);
}

/* 脏节点落盘：src → nblk 个连续 FS_block（尾部截断）。*/
static int mrow_store(struct ravenfs_mdata_cache *c,
		      const struct ravenfs_mrow_simple *row, const void *src)
{
	u64 blk0 = row->node_idx << 1;
	u32 nblk;

	if (blk0 >= c->total_blocks)
		return -EIO;
	/* 尾部截断（不写 blk0 + 2 形式，避免极端 total_blocks 下溢出）*/
	nblk = (c->total_blocks - blk0 < RAVENFS_MDATA_BLK_PER_NODE)
		? (u32)(c->total_blocks - blk0)
		: RAVENFS_MDATA_BLK_PER_NODE;
	return c->io.write_block(c->io.ctx, blk0, nblk, src);
}

/* ══════════════════════════ 遍历（调用方持片锁）══════════════════════════ */

typedef void (*mrow_visit_fn)(struct ravenfs_mdata_cache *c,
			      struct ravenfs_mrow_simple *row, void *arg);

static void mrow_visit_table(struct ravenfs_mdata_cache *c, void *table,
			     u32 level, mrow_visit_fn fn, void *arg)
{
	u32 i;

	if (!table)
		return;
	if (level == c->tree_levels - 1) {
		struct ravenfs_mrow_simple *leaf = table;

		for (i = 0; i < c->leaf_width; i++)
			fn(c, &leaf[i], arg);
		return;
	}
	{
		void **t = table;
		u32 width = 1u << c->level_bits[1 + level];

		for (i = 0; i < width; i++)
			if (t[i])
				mrow_visit_table(c, t[i], level + 1, fn, arg);
	}
}

static void mrow_for_each(struct ravenfs_mdata_cache *c,
			  struct ravenfs_mdata_shard *sh,
			  mrow_visit_fn fn, void *arg)
{
	if (!sh->root)
		return;
	if (c->mode == RAVENFS_MDATA_MODE_CTRL_ARRAY) {
		struct ravenfs_mrow_simple *leaf = sh->root;
		u64 i;

		for (i = 0; i < c->n_nodes; i++)
			fn(c, &leaf[i], arg);
		return;
	}
	mrow_visit_table(c, sh->root, 0, fn, arg);
}

/* ══════════════════════════ 生命周期 ══════════════════════════ */

int ravenfs_mdata_cache_init(struct ravenfs_mdata_cache *self,
			     const struct ravenfs_mdata_cfg *cfg)
{
	struct ravenfs_mdata_geom g;
	u32 i;
	int rc;

	if (!self || !cfg)
		return -EINVAL;
	if (self->shards) {			/* 已装配 */
		RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_INIT_FAIL);
		return -EINVAL;
	}
	/* 先快照 stat，保证后续所有失败/成功路径都能打点（P1：不参与控制流）。*/
	self->stat = cfg->stat;
	if (!cfg->total_blocks || !cfg->mem.alloc || !cfg->mem.free ||
	    !cfg->pool.alloc || !cfg->pool.free ||
	    !cfg->io.read_block || !cfg->io.write_block) {
		RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_INIT_FAIL);
		return -EINVAL;
	}

	rc = ravenfs_mdata_geometry(cfg->total_blocks, &g);
	if (rc) {
		RFS_LOG("mdata init rejected: total_blocks=%llu (bits > %u or invalid)\n",
			(unsigned long long)cfg->total_blocks,
			(u32)RAVENFS_MDATA_RADIX_MAX_BITS);
		RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_INIT_FAIL);
		return rc;
	}

	/* 注入面快照（运行期只读）；stat 已在入口快照。*/
	self->mem = cfg->mem;
	self->pool = cfg->pool;
	self->io = cfg->io;
	mcore_apply_geom(self, &g);
	self->total_blocks = cfg->total_blocks;

	self->shards = mcore_zalloc(self,
			(u64)self->n_slices * sizeof(*self->shards));
	if (!self->shards) {
		RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_INIT_FAIL);
		return -ENOMEM;
	}

	for (i = 0; i < self->n_slices; i++) {
		RFS_RWSEM_INIT(&self->shards[i].lock);
		self->shards[i].root = NULL;
	}

	if (self->mode == RAVENFS_MDATA_MODE_CTRL_ARRAY) {
		void *leaf = mcore_zalloc(self,
				self->n_nodes * sizeof(struct ravenfs_mrow_simple));

		if (!leaf) {
			self->mem.free(self->mem.ctx, self->shards);
			self->shards = NULL;
			self->n_slices = 0;
			RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_INIT_FAIL);
			return -ENOMEM;
		}
		self->shards[0].root = leaf;
		RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_TABLE_ALLOC_LEAF);
		RAVENFS_MD_STAT(self->stat, RAVENFS_MD_TABLE_ADD_BYTES,
				self->n_nodes *
				sizeof(struct ravenfs_mrow_simple));
	}

	RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_INIT_OK);
	RAVENFS_MD_STAT(self->stat, RAVENFS_MD_SLICES, self->n_slices);
	if (self->mode == RAVENFS_MDATA_MODE_CTRL_ARRAY)
		RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_INIT_CTRL_ARRAY);
	else
		RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_INIT_RADIX);
	return 0;
}

static void mrow_table_destroy(struct ravenfs_mdata_cache *c, void *table,
			       u32 level)
{
	u32 i;

	if (!table)
		return;
	if (level == c->tree_levels - 1) {
		struct ravenfs_mrow_simple *leaf = table;

		/* fini 是卸载静默点：flush_all 已归还 READY 行的 data；这里兜底
		 * 归还仍处 IO_ING/EVICTING 的 data（契约被破坏时不泄漏；表本身
		 * 仍按卸载语义释放）。*/
		for (i = 0; i < c->leaf_width; i++)
			mrow_release_data(c, &leaf[i]);
		c->mem.free(c->mem.ctx, table);
		RAVENFS_MD_STAT(c->stat, RAVENFS_MD_TABLE_DEL_BYTES,
				mrow_table_bytes(c, level));
		return;
	}
	{
		void **t = table;
		u32 width = 1u << c->level_bits[1 + level];

		for (i = 0; i < width; i++)
			if (t[i])
				mrow_table_destroy(c, t[i], level + 1);
	}
	c->mem.free(c->mem.ctx, table);
	RAVENFS_MD_STAT(c->stat, RAVENFS_MD_TABLE_DEL_BYTES,
			mrow_table_bytes(c, level));
}

void ravenfs_mdata_cache_fini(struct ravenfs_mdata_cache *self)
{
	u32 i;

	if (!self || !self->shards)
		return;

	ravenfs_mdata_flush_all(self);		/* 脏先落盘 → 再清空全部行 */

	for (i = 0; i < self->n_slices; i++) {
		struct ravenfs_mdata_shard *sh = &self->shards[i];

		if (!sh->root)
			continue;
		if (self->mode == RAVENFS_MDATA_MODE_CTRL_ARRAY) {
			struct ravenfs_mrow_simple *leaf = sh->root;
			u64 k;

			/* 同上：flush_all 已归还 READY 行，这里兜底在飞行行。*/
			for (k = 0; k < self->n_nodes; k++)
				mrow_release_data(self, &leaf[k]);
			self->mem.free(self->mem.ctx, sh->root);
			RAVENFS_MD_STAT(self->stat,
					RAVENFS_MD_TABLE_DEL_BYTES,
					self->n_nodes *
					sizeof(struct ravenfs_mrow_simple));
		} else {
			mrow_table_destroy(self, sh->root, 0);
		}
		sh->root = NULL;
	}
	self->mem.free(self->mem.ctx, self->shards);
	self->shards = NULL;
	self->n_slices = 0;
	self->n_nodes = 0;
	self->total_blocks = 0;
}

/* ══════════════════════════ 装载（慢路径）══════════════════════════ */

/* 已经以 IO_ING 抢到行独占权后：池分配 + 块 IO 全在片锁外；成功写锁发布。*/
static int mrow_load(struct ravenfs_mdata_cache *c,
		     struct ravenfs_mdata_shard *sh,
		     struct ravenfs_mrow_simple *row, u64 node_idx)
{
	u64 blk0 = node_idx << 1;
	u32 nblk;
	void *data;
	u64 now;
	int err;

	if (blk0 >= c->total_blocks) {
		RFS_ATOMIC64_SET(&row->ctrl, (s64)RAVENFS_MROW_IDLE);
		return -EIO;
	}
	/* 尾部节点截断（不写 blk0 + 2 形式，避免极端 total_blocks 下溢出）*/
	nblk = (c->total_blocks - blk0 < RAVENFS_MDATA_BLK_PER_NODE)
		? (u32)(c->total_blocks - blk0)
		: RAVENFS_MDATA_BLK_PER_NODE;

	/* —— 锁外：8K 缓冲（零初始化契约）+ 块读 —— */
	data = c->pool.alloc(c->pool.ctx, RAVENFS_MDATA_NODE_BYTES);
	if (!data) {
		RFS_RWSEM_WRITE_GUARD(wlk, &sh->lock);

		RFS_ATOMIC64_SET(&row->ctrl, (s64)RAVENFS_MROW_IDLE);
		return -ENOMEM;
	}
	err = c->io.read_block(c->io.ctx, blk0, nblk, data);
	if (err) {
		c->pool.free(c->pool.ctx, data, RAVENFS_MDATA_NODE_BYTES);
		RFS_RWSEM_WRITE_GUARD(wlk, &sh->lock);

		RFS_ATOMIC64_SET(&row->ctrl, (s64)RAVENFS_MROW_IDLE);
		return err;
	}

	/* —— 写锁发布：READY | ref=1（一次 store 同时发布并就位）—— */
	{
		RFS_RWSEM_WRITE_GUARD(wlk, &sh->lock);

		now = RFS_NOW_NS();
		row->node_idx = node_idx;
		row->data = data;
		RFS_ATOMIC64_SET(&row->acc_stamp, (s64)now);
		RFS_ATOMIC64_SET(&row->wb_stamp, (s64)now);
		RFS_ATOMIC64_SET(&row->ctrl,
			(s64)(RAVENFS_MROW_READY | RAVENFS_MROW_REF_ONE));
	}
	RAVENFS_MD_STAT(c->stat, RAVENFS_MD_RESIDENT_ADD_BYTES,
			RAVENFS_MDATA_NODE_BYTES);
	return 0;
}

/* ═══════════════════════ 会话（节点租借）═══════════════════════ */

struct ravenfs_msession *ravenfs_mdata_get(struct ravenfs_mdata_cache *self,
					   u64 blk)
{
	struct ravenfs_msession *s;
	struct ravenfs_mdata_shard *sh;
	struct ravenfs_mrow_simple *row;
	u64 node_idx;
	u32 sh_idx;

	if (!self || !self->shards)
		return RFS_ERR_PTR(-EIO);
	if (blk >= self->total_blocks) {
		RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_ERR);
		return RFS_ERR_PTR(-EIO);
	}
	node_idx = blk >> 1;
	sh_idx = (u32)(node_idx >> self->shard_bits);
	if (sh_idx >= self->n_slices) {
		RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_ERR);
		return RFS_ERR_PTR(-EIO);
	}
	sh = &self->shards[sh_idx];

	s = self->mem.alloc(self->mem.ctx, sizeof(*s));
	if (!s)
		return RFS_ERR_PTR(-ENOMEM);
	s->cache = self;
	s->shard = sh;
	s->row = NULL;

	for (;;) {
		struct ravenfs_mrow_simple *found = NULL;
		u32 found_state = RAVENFS_MROW_IDLE;
		int got = MROW_REF_BUSY;

		/* —— 快路径：短持读锁 locate + CAS 占坑 —— */
		{
			RFS_RWSEM_READ_GUARD(lk, &sh->lock);

			found = mrow_locate(self, sh, node_idx);
			if (found) {
				got = mrow_try_ref_inc(found);
				/* 读锁内取状态快照：放锁后叶表可能被强淘汰摘除，
				 * 不能再解引用 found（H6 / 表精简 UAF 防线）。*/
				if (got != MROW_REF_OK)
					found_state = mrow_state_of(
						(u64)RFS_ATOMIC64_READ(
							&found->ctrl));
			}
		}

		if (got == MROW_REF_OK) {
			RFS_ATOMIC64_SET(&found->acc_stamp, (s64)RFS_NOW_NS());
			RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_GET_HIT);
			s->row = found;
			return s;
		}
		if (got == MROW_REF_FULL) {
			RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_ERR);
			self->mem.free(self->mem.ctx, s);
			return RFS_ERR_PTR(-EOVERFLOW);
		}
		if (found) {
			/* 静态表里行恒存在：只有 IDLE 才是「未驻留」可认领；
			 * IO_ING（他人装载）/ EVICTING（淘汰接管）必须放锁让位重试。*/
			if (found_state != RAVENFS_MROW_IDLE) {
				RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_RETRY);
				RFS_CPU_RELAX();
				RFS_YIELD();
				continue;
			}
		}

		/* —— 未命中（路径未建或行 IDLE）：写锁 ensure_path + CAS IDLE→IO_ING —— */
		{
			bool claimed = false;
			int rc;

			{
				RFS_RWSEM_WRITE_GUARD(wlk, &sh->lock);

				rc = mrow_ensure_path(self, sh, node_idx);
				if (!rc) {
					row = mrow_locate(self, sh, node_idx);
					if (row) {
						u64 old = (u64)RFS_ATOMIC64_READ(
							&row->ctrl);

						if (mrow_state_of(old) ==
						    RAVENFS_MROW_IDLE &&
						    (u64)RFS_ATOMIC64_CMPXCHG(
							&row->ctrl, (s64)old,
							(s64)((old &
							~RAVENFS_MROW_STATE_MASK) |
							RAVENFS_MROW_IO_ING)) == old)
							claimed = true;
					} else {
						rc = -ENOMEM;
					}
				}
			}
			if (rc) {
				RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_ERR);
				self->mem.free(self->mem.ctx, s);
				return RFS_ERR_PTR(rc);
			}
			if (!claimed) {
				RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_RETRY);
				RFS_CPU_RELAX();
				RFS_YIELD();
				continue;
			}
			/* 拿到装载权 = MDP_GET_MISS（⇄ god「未命中走盘读」）。*/
			RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_GET_MISS);
		}

		/* —— 已独占：池分配 + 块 IO（锁外）—— */
		{
			int err = mrow_load(self, sh, row, node_idx);

			if (err) {
				RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_ERR);
				self->mem.free(self->mem.ctx, s);
				return RFS_ERR_PTR(err);
			}
			RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_NODE_LOAD);
			s->row = row;
			return s;
		}
	}
}

void ravenfs_mdata_put(struct ravenfs_msession *s)
{
	if (!s)
		return;
	if (s->row)
		mrow_put_ref(s->row);
	if (s->cache) {
		RAVENFS_MD_STAT1(s->cache->stat, RAVENFS_MD_PUT);
		s->cache->mem.free(s->cache->mem.ctx, s);
	}
}

/* 会话标脏：片读锁内 fetch_or（与 wb/flush 的片写锁互斥）。
 * 会话已持有 ref，行不会被 reap/drop 回收；读锁只用于与回写动作串行。*/
void ravenfs_mdata_mark_dirty(struct ravenfs_msession *s)
{
	if (!s || !s->row || !s->shard)
		return;
	{
		RFS_RWSEM_READ_GUARD(lk, &s->shard->lock);

		RFS_ATOMIC64_FETCH_OR(&s->row->ctrl,
			(s64)RAVENFS_MROW_FLAG(RAVENFS_MROW_F_DIRTY));
		RFS_ATOMIC64_SET(&s->row->wb_stamp, (s64)RFS_NOW_NS());
	}
	if (s->cache)
		RAVENFS_MD_STAT1(s->cache->stat, RAVENFS_MD_DIRTY);
}

/* ═══════════════════════ 丢弃 ═══════════════════════ */

void ravenfs_mdata_drop(struct ravenfs_mdata_cache *self, u64 blk)
{
	struct ravenfs_mrow_simple *victim = NULL;
	struct ravenfs_mdata_shard *sh;
	struct ravenfs_mrow_simple *row;
	u64 node_idx;
	u32 sh_idx;

	if (!self || !self->shards)
		return;
	if (blk >= self->total_blocks)
		return;
	node_idx = blk >> 1;
	sh_idx = (u32)(node_idx >> self->shard_bits);
	if (sh_idx >= self->n_slices)
		return;
	sh = &self->shards[sh_idx];

	RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_DROP);

	{
		/* 树只增不删：读锁足以保证 locate 期间表链稳定；接管靠 ctrl CAS。*/
		RFS_RWSEM_READ_GUARD(lk, &sh->lock);

		row = mrow_locate(self, sh, node_idx);
		if (row) {
			u64 ctrl = (u64)RFS_ATOMIC64_READ(&row->ctrl);

			if (mrow_state_of(ctrl) == RAVENFS_MROW_READY) {
				if (mrow_ref_of(ctrl) == 0) {
					u64 ev = (ctrl &
						~RAVENFS_MROW_STATE_MASK) |
						RAVENFS_MROW_EVICTING;

					if ((u64)RFS_ATOMIC64_CMPXCHG(
						&row->ctrl, (s64)ctrl,
						(s64)ev) == ctrl)
						victim = row;
				} else {
					/* 内容作废：禁回写，防块复用后陈旧脏行污染 */
					RFS_ATOMIC64_ANDNOT(&row->ctrl,
						(s64)RAVENFS_MROW_FLAG(
						RAVENFS_MROW_F_DIRTY));
				}
			}
			/* IO_ING / EVICTING / IDLE：跳过，由责任流程收口 */
		}
	}

	if (victim) {
		mrow_release_data(self, victim);
		RFS_ATOMIC64_SET(&victim->ctrl, (s64)RAVENFS_MROW_IDLE);
	}
}

int ravenfs_mdata_read(struct ravenfs_mdata_cache *self, u64 blk, void *buf)
{
	RAVENFS_MDATA_SESSION(ms, self, blk);

	if (!RAVENFS_MDATA_SESSION_VALID(ms))
		return RFS_IS_ERR(ms) ? (int)RFS_PTR_ERR(ms) : -EIO;
	RFS_MEMCPY(buf, RAVENFS_MDATA_SESSION_DATA(ms),
		   RAVENFS_MDATA_NODE_BYTES);
	return 0;	/* ms 由 cleanup 自动 put */
}

/* ═══════════════════════ 回写 / 淘汰 ═══════════════════════ */

struct mrow_wb_ctx {
	u64	cutoff;
	bool	ignore_age;
	int	err;
	u64	written;	/* 成功写回的行数（flush_all 用于 MDP_FLUSH_WB）*/
};

static void mrow_wb_visit(struct ravenfs_mdata_cache *c,
			  struct ravenfs_mrow_simple *row, void *arg)
{
	struct mrow_wb_ctx *ctx = arg;
	u64 ctrl = (u64)RFS_ATOMIC64_READ(&row->ctrl);

	if (mrow_state_of(ctrl) != RAVENFS_MROW_READY)
		return;
	if (!(ctrl & RAVENFS_MROW_FLAG(RAVENFS_MROW_F_DIRTY)))
		return;
	if (ctrl & RAVENFS_MROW_FLAG(RAVENFS_MROW_F_WB))
		return;
	if (!ctx->ignore_age &&
	    (u64)RFS_ATOMIC64_READ(&row->wb_stamp) >= ctx->cutoff)
		return;
	if (mrow_store(c, row, row->data)) {
		ctx->err = -EIO;
		RAVENFS_MD_STAT1(c->stat, RAVENFS_MD_ERR);
		return;
	}
	RFS_ATOMIC64_ANDNOT(&row->ctrl,
		(s64)RAVENFS_MROW_FLAG(RAVENFS_MROW_F_DIRTY));
	RFS_ATOMIC64_SET(&row->wb_stamp, (s64)RFS_NOW_NS());
	ctx->written++;
	RAVENFS_MD_STAT1(c->stat, RAVENFS_MD_NODE_WB);
}

static int mrow_wb_dirty_run(struct ravenfs_mdata_cache *self,
			     u64 age_cutoff_ns, bool ignore_age, u64 *written)
{
	struct mrow_wb_ctx ctx;
	u32 i;

	if (!self || !self->shards) {
		if (written)
			*written = 0;
		return 0;
	}
	ctx.cutoff = age_cutoff_ns;
	ctx.ignore_age = ignore_age;
	ctx.err = 0;
	ctx.written = 0;

	for (i = 0; i < self->n_slices; i++) {
		struct ravenfs_mdata_shard *sh = &self->shards[i];

		/* 低频维护动作：写锁内做 I/O，换取「回写期间行不被改写」的简单性 */
		RFS_RWSEM_WRITE_GUARD(wlk, &sh->lock);

		mrow_for_each(self, sh, mrow_wb_visit, &ctx);
	}
	if (written)
		*written = ctx.written;
	return ctx.err;
}

int ravenfs_mdata_wb_dirty(struct ravenfs_mdata_cache *self, u64 age_cutoff_ns,
			   bool ignore_age)
{
	return mrow_wb_dirty_run(self, age_cutoff_ns, ignore_age, NULL);
}

int ravenfs_mdata_writeback_all(struct ravenfs_mdata_cache *self)
{
	return mrow_wb_dirty_run(self, 0, true, NULL);
}

int ravenfs_mdata_wb_node(struct ravenfs_mdata_cache *self, u64 blk)
{
	struct ravenfs_mrow_simple *row;
	int err = 0;

	if (!self || !self->shards)
		return 0;
	RAVENFS_MDATA_SESSION(ms, self, blk);
	if (!RAVENFS_MDATA_SESSION_VALID(ms))
		return RFS_IS_ERR(ms) ? (int)RFS_PTR_ERR(ms) : -EIO;
	row = ms->row;

	{
		RFS_RWSEM_WRITE_GUARD(wlk, &ms->shard->lock);

		u64 ctrl = (u64)RFS_ATOMIC64_READ(&row->ctrl);

		if (mrow_state_of(ctrl) == RAVENFS_MROW_READY &&
		    (ctrl & RAVENFS_MROW_FLAG(RAVENFS_MROW_F_DIRTY)) &&
		    !(ctrl & RAVENFS_MROW_FLAG(RAVENFS_MROW_F_WB))) {
			if (mrow_store(self, row, row->data)) {
				err = -EIO;
				RAVENFS_MD_STAT1(self->stat, RAVENFS_MD_ERR);
			} else {
				RFS_ATOMIC64_ANDNOT(&row->ctrl,
					(s64)RAVENFS_MROW_FLAG(
					RAVENFS_MROW_F_DIRTY));
				RFS_ATOMIC64_SET(&row->wb_stamp,
						 (s64)RFS_NOW_NS());
				RAVENFS_MD_STAT1(self->stat,
						 RAVENFS_MD_NODE_WB);
			}
		}
	}
	return err;	/* ms 由 cleanup 自动 put */
}

struct mrow_reap_ctx {
	u64				cutoff;
	struct ravenfs_mrow_simple	*head;
};

static void mrow_reap_visit(struct ravenfs_mdata_cache *c,
			    struct ravenfs_mrow_simple *row, void *arg)
{
	struct mrow_reap_ctx *ctx = arg;
	u64 ctrl = (u64)RFS_ATOMIC64_READ(&row->ctrl);
	u64 ev;

	(void)c;
	if (mrow_state_of(ctrl) != RAVENFS_MROW_READY)
		return;
	if (mrow_ref_of(ctrl) != 0)
		return;				/* 有活跃会话：不回收 */
	if (ctrl & RAVENFS_MROW_FLAG(RAVENFS_MROW_F_DIRTY))
		return;				/* 普通 TTL 不回收脏行（靠 wb aging）*/
	if (ctrl & RAVENFS_MROW_FLAG(RAVENFS_MROW_F_WB))
		return;
	if ((u64)RFS_ATOMIC64_READ(&row->acc_stamp) >= ctx->cutoff)
		return;				/* 刚访问过：不回收 */

	ev = (ctrl & ~RAVENFS_MROW_STATE_MASK) | RAVENFS_MROW_EVICTING;
	if ((u64)RFS_ATOMIC64_CMPXCHG(&row->ctrl, (s64)ctrl, (s64)ev) != ctrl)
		return;				/* 被 get 占坑/他人接管 */

	/* EVICTING 独占态：reserved[0] 串临时链表，放锁后逐个归还 data。*/
	row->reserved[0] = (u64)(unsigned long)ctx->head;
	ctx->head = row;
}

/* 普通 TTL 淘汰 = 与 god `dfs_reap` 同构的「读锁 + 行 CAS」：
 *   · 表只在 strong 的表精简阶段被摘，而本函数只在片读锁内遍历、CAS 到的行
 *     立即变 EVICTING（钉住叶表），因此读锁遍历安全；
 *   · 不摘表（摘表要片写锁，会与 get 的写锁 ensure_path 串行化，且普通 TTL
 *     淘汰不需要收缩结构）。*/
long ravenfs_mdata_reap_expired(struct ravenfs_mdata_cache *self, u64 cutoff_ns)
{
	long freed = 0;
	u32 i;

	if (!self || !self->shards)
		return 0;

	for (i = 0; i < self->n_slices; i++) {
		struct ravenfs_mdata_shard *sh = &self->shards[i];
		struct mrow_reap_ctx ctx;

		ctx.cutoff = cutoff_ns;
		ctx.head = NULL;

		{
			RFS_RWSEM_READ_GUARD(lk, &sh->lock);

			mrow_for_each(self, sh, mrow_reap_visit, &ctx);
		}

		/* —— 放锁后归还 data / 回 IDLE（EVICTING 独占行）—— */
		while (ctx.head) {
			struct ravenfs_mrow_simple *row = ctx.head;
			struct ravenfs_mrow_simple *next =
				(struct ravenfs_mrow_simple *)(unsigned long)
				row->reserved[0];

			row->reserved[0] = 0;
			mrow_release_data(self, row);
			RFS_ATOMIC64_SET(&row->ctrl, (s64)RAVENFS_MROW_IDLE);
			freed++;
			ctx.head = next;
		}
	}
	RAVENFS_MD_STAT(self->stat, RAVENFS_MD_REAP, (u64)freed);
	return freed;
}

/* ══════════════════════ 强淘汰（两 gear + 空表回收）══════════════════════ */

struct mrow_squeeze_ctx {
	bool				respect_ttl;
	u64				ttl_cutoff_ns;
	struct ravenfs_mrow_simple	*head;
};

/* 行回收阶段（片读锁内）：CAS READY→EVICTING 并串链。
 * 候选 = READY && ref==0 && !F_WB；LOWLOAD 另要求 acc_stamp < cutoff
 * （cutoff==0 表示无 TTL 过滤）。脏行也在此接管（锁外写回）。*/
static void mrow_squeeze_visit(struct ravenfs_mdata_cache *c,
			       struct ravenfs_mrow_simple *row, void *arg)
{
	struct mrow_squeeze_ctx *ctx = arg;
	u64 ctrl = (u64)RFS_ATOMIC64_READ(&row->ctrl);
	u64 ev;

	(void)c;
	if (mrow_state_of(ctrl) != RAVENFS_MROW_READY)
		return;				/* IO_ING/EVICTING/IDLE 跳过 */
	if (mrow_ref_of(ctrl) != 0)
		return;				/* 会话持有：绝不释放（UAF）*/
	if (ctrl & RAVENFS_MROW_FLAG(RAVENFS_MROW_F_WB))
		return;				/* 回写中：由 wb 流程收口 */
	if (ctx->respect_ttl && ctx->ttl_cutoff_ns &&
	    (u64)RFS_ATOMIC64_READ(&row->acc_stamp) >= ctx->ttl_cutoff_ns)
		return;				/* 热行（剩余 TTL 多）：保留 */

	ev = (ctrl & ~RAVENFS_MROW_STATE_MASK) | RAVENFS_MROW_EVICTING;
	if ((u64)RFS_ATOMIC64_CMPXCHG(&row->ctrl, (s64)ctrl, (s64)ev) != ctrl)
		return;				/* 被 get 占坑/他人接管 */

	row->reserved[0] = (u64)(unsigned long)ctx->head;
	ctx->head = row;
}

/* 行处理阶段（锁外，行处于 EVICTING 独占态）：
 *   · 脏行先写回；成功才清 DIRTY/归还 data/回 IDLE，失败恢复 READY|DIRTY 并
 *     保留 data，write_errors++ 后继续处理下一行；
 *   · 非脏行直接归还 data → IDLE。*/
static void mrow_squeeze_process(struct ravenfs_mdata_cache *c,
				 struct ravenfs_mdata_squeeze_res *out,
				 struct ravenfs_mrow_simple **headp)
{
	while (*headp) {
		struct ravenfs_mrow_simple *row = *headp;
		struct ravenfs_mrow_simple *next =
			(struct ravenfs_mrow_simple *)(unsigned long)
			row->reserved[0];
		u64 ctrl;
		bool dirty;

		row->reserved[0] = 0;
		*headp = next;

		ctrl = (u64)RFS_ATOMIC64_READ(&row->ctrl);
		dirty = !!(ctrl & RAVENFS_MROW_FLAG(RAVENFS_MROW_F_DIRTY)) &&
			row->data != NULL;

		if (dirty) {
			if (mrow_store(c, row, row->data)) {
				/* 失败：恢复 READY|DIRTY（保留 data），下次再试 */
				u64 back = (ctrl & ~RAVENFS_MROW_STATE_MASK) |
					   RAVENFS_MROW_READY;

				RFS_ATOMIC64_CMPXCHG(&row->ctrl, (s64)ctrl,
						     (s64)back);
				out->write_errors++;
				RAVENFS_MD_STAT1(c->stat, RAVENFS_MD_ERR);
				continue;
			}
			RAVENFS_MD_STAT1(c->stat, RAVENFS_MD_NODE_WB);
		}

		mrow_release_data(c, row);
		RFS_ATOMIC64_SET(&row->ctrl, (s64)RAVENFS_MROW_IDLE);
		out->rows_reaped++;
	}
}

/* 表结构字节数（按当前几何）：level 从 0（根）到 tree_levels-1（叶）。*/
static u64 mrow_table_bytes(const struct ravenfs_mdata_cache *c, u32 level)
{
	if (level == c->tree_levels - 1)
		return (u64)c->leaf_width *
		       sizeof(struct ravenfs_mrow_simple);
	return (u64)(1u << c->level_bits[1 + level]) * sizeof(void *);
}

/* 表精简阶段（片写锁内）后序 DFS：返回 true = 本表已空且已释放。
 *   · 叶表：所有槽 state==IDLE → 空；防御性归还残留 data 后 mem.free；
 *   · 内部表：递归后所有子槽 NULL → 空；mem.free；
 *   · 非空表绝不释放（含 IO_ING/EVICTING/ref>0 行或仍有子表）。*/
static bool mrow_prune_table(struct ravenfs_mdata_cache *c, void *table,
			     u32 level, struct ravenfs_mdata_squeeze_res *out)
{
	u32 i;

	if (!table)
		return true;

	if (level == c->tree_levels - 1) {
		struct ravenfs_mrow_simple *leaf = table;
		bool empty = true;

		for (i = 0; i < c->leaf_width; i++) {
			if (mrow_state_of((u64)RFS_ATOMIC64_READ(
					&leaf[i].ctrl)) == RAVENFS_MROW_IDLE)
				continue;
			empty = false;
			break;
		}
		if (!empty)
			return false;
		/* 契约上空行 data==NULL；防御性收口残留缓冲 */
		for (i = 0; i < c->leaf_width; i++)
			mrow_release_data(c, &leaf[i]);
		c->mem.free(c->mem.ctx, table);
		RAVENFS_MD_STAT1(c->stat, RAVENFS_MD_TABLE_FREE);
		RAVENFS_MD_STAT(c->stat, RAVENFS_MD_TABLE_DEL_BYTES,
				mrow_table_bytes(c, level));
		out->tables_freed++;
		out->table_bytes_freed += mrow_table_bytes(c, level);
		return true;
	}

	{
		void **t = table;
		u32 width = 1u << c->level_bits[1 + level];
		bool empty = true;

		for (i = 0; i < width; i++) {
			if (!t[i])
				continue;
			if (mrow_prune_table(c, t[i], level + 1, out))
				t[i] = NULL;	/* 子表已释放 */
			else
				empty = false;
		}
		if (!empty)
			return false;
		c->mem.free(c->mem.ctx, table);
		RAVENFS_MD_STAT1(c->stat, RAVENFS_MD_TABLE_FREE);
		RAVENFS_MD_STAT(c->stat, RAVENFS_MD_TABLE_DEL_BYTES,
				mrow_table_bytes(c, level));
		out->tables_freed++;
		out->table_bytes_freed += mrow_table_bytes(c, level);
		return true;
	}
}

int ravenfs_mdata_reap_expired_strong(struct ravenfs_mdata_cache *self,
				      u32 gear,
				      u64 ttl_cutoff_ns,
				      struct ravenfs_mdata_squeeze_res *out)
{
	struct ravenfs_mdata_squeeze_res res;
	bool respect_ttl;
	u32 i;

	if (out)
		RFS_MEMSET(out, 0, sizeof(*out));
	if (!self)
		return -EINVAL;
	if (gear != RAVENFS_MDATA_SQUEEZE_PRESSURE &&
	    gear != RAVENFS_MDATA_SQUEEZE_LOWLOAD)
		return -EINVAL;

	RFS_MEMSET(&res, 0, sizeof(res));
	if (!self->shards) {
		if (out)
			*out = res;
		return 0;
	}

	respect_ttl = (gear == RAVENFS_MDATA_SQUEEZE_LOWLOAD);

	/* ── 阶段 ①：行回收（片读锁 + 行 CAS；写回/归还全在锁外）── */
	for (i = 0; i < self->n_slices; i++) {
		struct ravenfs_mdata_shard *sh = &self->shards[i];
		struct mrow_squeeze_ctx ctx;

		ctx.respect_ttl = respect_ttl;
		ctx.ttl_cutoff_ns = ttl_cutoff_ns;
		ctx.head = NULL;

		{
			RFS_RWSEM_READ_GUARD(lk, &sh->lock);

			mrow_for_each(self, sh, mrow_squeeze_visit, &ctx);
		}

		mrow_squeeze_process(self, &res, &ctx.head);
	}

	/* ── 阶段 ②：表精简（片写锁 + 后序 DFS；非空表绝不释放）── */
	if (self->mode == RAVENFS_MDATA_MODE_RADIX) {
		for (i = 0; i < self->n_slices; i++) {
			struct ravenfs_mdata_shard *sh = &self->shards[i];

			RFS_RWSEM_WRITE_GUARD(wlk, &sh->lock);

			if (sh->root &&
			    mrow_prune_table(self, sh->root, 0, &res))
				sh->root = NULL;	/* get 会 ensure_path 重建 */
		}
	}

	if (res.rows_reaped)
		RAVENFS_MD_STAT(self->stat, RAVENFS_MD_REAP_STRONG,
				res.rows_reaped);
	if (out)
		*out = res;
	return res.write_errors ? -EIO : 0;
}

/* ═══════════════════════ 卸载 / 同步点 ═══════════════════════ */

static void mrow_flush_visit(struct ravenfs_mdata_cache *c,
			     struct ravenfs_mrow_simple *row, void *arg)
{
	u64 ctrl = (u64)RFS_ATOMIC64_READ(&row->ctrl);
	u32 state = mrow_state_of(ctrl);

	(void)arg;
	if (state == RAVENFS_MROW_IDLE)
		return;
	if (state == RAVENFS_MROW_IO_ING ||
	    state == RAVENFS_MROW_EVICTING) {
		/* 卸载静默点契约之外：另一流程正独占该行，跳过以免 UAF */
		RFS_LOG("mdata flush skip in-flight row node=%llu state=%u\n",
			(unsigned long long)row->node_idx, state);
		return;
	}
	if (mrow_ref_of(ctrl) != 0)
		RFS_LOG("mdata node %llu still referenced(%u), reclaim anyway\n",
			(unsigned long long)row->node_idx, mrow_ref_of(ctrl));
	RAVENFS_MD_STAT1(c->stat, RAVENFS_MD_FLUSH_DROP);
	mrow_release_data(c, row);
	row->node_idx = 0;
	RFS_ATOMIC64_SET(&row->acc_stamp, 0);
	RFS_ATOMIC64_SET(&row->wb_stamp, 0);
	RFS_ATOMIC64_SET(&row->ctrl, (s64)RAVENFS_MROW_IDLE);
}

void ravenfs_mdata_discard_all(struct ravenfs_mdata_cache *self)
{
	u32 i;

	if (!self || !self->shards)
		return;
	for (i = 0; i < self->n_slices; i++) {
		struct ravenfs_mdata_shard *sh = &self->shards[i];

		RFS_RWSEM_WRITE_GUARD(wlk, &sh->lock);

		mrow_for_each(self, sh, mrow_flush_visit, NULL);
	}
}

void ravenfs_mdata_flush_all(struct ravenfs_mdata_cache *self)
{
	u64 written = 0;

	if (!self || !self->shards)
		return;
	(void)mrow_wb_dirty_run(self, 0, true, &written);
	if (written)
		RAVENFS_MD_STAT(self->stat, RAVENFS_MD_FLUSH_WB, written);
	ravenfs_mdata_discard_all(self);
}

/* ══════════════════════════ 几何 / 审计 ══════════════════════════ */

u64 ravenfs_mdata_total_blocks(const struct ravenfs_mdata_cache *self)
{
	return self ? self->total_blocks : 0;
}

u32 ravenfs_mdata_shard_count(const struct ravenfs_mdata_cache *self)
{
	return self ? self->n_slices : 0;
}

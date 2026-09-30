// SPDX-License-Identifier: GPL-2.0
/*
 * bitmap_core.c —— RavenFS 双位图分片分配器 · **路由层 / 共享逻辑**（ko + libc 双编）
 *
 * 定位：把 god 的「上帝类位图分配器」拆成 C 版三件套（core/mem/chunk）中的
 * **唯一路由器**——本文件是"公交换乘站"：对外只认全局位号，内部按 cfg.choose
 * 经后端 ops 分派，绝不把路由逻辑复制两份。
 *
 * 本文件负责：
 *   · 共享几何：slice_count 推导（2^n CPU 上界 ∩ 满片数 ∩ 末片非空迭代）、
 *     per_bits 推导、组通用字段填充、per-CPU preferred 铺开；
 *   · 通用路由：preferred 亲和扫描、跨片/跨窗 occupy·free 续圈、global↔local
 *     换算（CHUNK 经 in_global_base_idx）；
 *   · 按 cfg.choose 分派到后端 ops（MEM → bitmap_mem.c；CHUNK → bitmap_chunk.c）。
 *
 * 坐标契约：**公开方法恒为全局位号**；后端 ops 在「片自然坐标」下工作——
 *   MEM 片即全局视角（换算恒等），CHUNK 片为片内局部（0 起）。换算收在本文件的
 *   bm_to_local()/bm_to_global()（v2 的局部视角不外泄）。
 *
 * ── 与 god 的**刻意差异**（文档化，非语义差异）──
 *   1. 锁策略：god v1 mgr 路由层用 `try_lock_write`（忙则换片）、god v2 slice 分配
 *      用 `std::mutex::try_lock`（忙返 RC_ERR_BUSY）。ko 口径改为**片内阻塞锁**
 *      （MEM `rfs_spin_t` 阻塞自旋；CHUNK `rfs_mutex_t` 可睡阻塞）——与
 *      linux/bitmap.c ko 现状一致（2026-09-28 用户裁）。故路由循环**无「锁忙换片」
 *      分支**：ops 内阻塞取锁，无货返 -ENOSPC 才换片。
 *   2. 坐标外化：god v2 slice 吃/吐局部坐标、mgr 换算；本层承担同一角色。
 *   3. 返回码：god `result_code_t / RC_*` → ko `0 / 负 -errno`；`PTR_INVALID` →
 *      `RFS_BM_NO_HIT`。
 *   4. CHUNK 的 cpu 索引统一 `% cpu_slots`（god v2 头无 cpu_slots 成员、裸用
 *      raw cpu，宿主须自保；本类有 cpu_slots，故取模防 OOB）。
 *   其余语义（preferred 起扫 / hint 只进不退 / free_count 只进不退 / occupy 跨片
 *   续圈 / free 跨片逐段 / 摘要免载入 / 单窗语义 / align 夹断 / 尾片非空）逐条照 god。
 */
#include "ravenfs_bitmap_core.h"
#include "ravenfs_bitmap_backend.h"

/* ═══════════════════════════ 共享几何 ═══════════════════════════ */

static u64 bm_align_up(u64 x, u64 a)
{
	return (x + a - 1) & ~(a - 1);
}

/*
 * 共享切片数：2^n ≥ 逻辑 CPU 上界 ∩ 满片数上限（每片 ≥ 32768 位，4K 对齐下限）
 * ∩ 末片非空迭代。与 linux/bitmap.c rfs_bm_slice_count / god ctor 同源，
 * 两组共享一份（双实例会重复推导且几何可能分叉）。
 */
static u32 bm_derive_slice_count(u64 blk_bits, u64 ino_bits, u32 ncpu)
{
	u32 n = 1;
	u64 cap;

	while (n < ncpu && n < (1u << 30))
		n <<= 1;
	cap = min(blk_bits, ino_bits) / RFS_BM_SLICE_MIN_BITS;
	if (!cap)
		cap = 1;
	if ((u64)n > cap)
		n = (u32)cap;
	for (;;) {
		u64 per_b, per_i;

		if (n <= 1) {
			n = 1;
			break;
		}
		per_b = bm_align_up(DIV_ROUND_UP_ULL(blk_bits, n), RFS_BM_SLICE_MIN_BITS);
		per_i = bm_align_up(DIV_ROUND_UP_ULL(ino_bits, n), RFS_BM_SLICE_MIN_BITS);
		if ((u64)(n - 1) * per_b >= blk_bits ||
		    (u64)(n - 1) * per_i >= ino_bits) {
			n >>= 1;
			continue;
		}
		break;
	}
	return n;
}

/* 每片位数：nbits/n_slices 向上取整到 32768（片下限）倍数；0 → 下限 */
static u64 bm_per_bits(u64 nbits, u32 n_slices)
{
	u64 per = bm_align_up(DIV_ROUND_UP_ULL(nbits, n_slices), RFS_BM_SLICE_MIN_BITS);

	return per ? per : RFS_BM_SLICE_MIN_BITS;
}

/* 片 idx 的结束全局位号（开区间；末片夹到 nbits） */
static u64 bm_slice_end(const struct rfs_bm_group *g, u32 idx)
{
	u64 end = (u64)(idx + 1) * g->per_bits;

	return end < g->nbits ? end : g->nbits;
}

/*
 * global ↔ local（唯一换乘点）：
 *   CHUNK 片为片内局部（0 起）⇒ 经 in_global_base_idx 换算；
 *   MEM   片即全局视角（含 base_idx）⇒ 恒等（片算法直接吃/吐全局位号）。
 */
static u64 bm_to_local(const struct rfs_bm_group *g, u32 idx, u64 gpos)
{
	if (g->mode == RFS_BM_MODE_CHUNK)
		return gpos - g->cslices[idx].in_global_base_idx;
	return gpos;
}

static u64 bm_to_global(const struct rfs_bm_group *g, u32 idx, u64 lpos)
{
	if (g->mode == RFS_BM_MODE_CHUNK)
		return g->cslices[idx].in_global_base_idx + lpos;
	return lpos;
}

/* ═══════════════════════════ ops 选路 ═══════════════════════════ */

const struct rfs_bm_ops *rfs_bm_ops_of(u8 mode)
{
	switch (mode) {
	case RFS_BM_MODE_MEM:
		return &rfs_bm_mem_ops;
	case RFS_BM_MODE_CHUNK:
		return &rfs_bm_chunk_ops;
	default:
		return NULL;
	}
}

/* ═══════════════════════════ 生命周期 ═══════════════════════════ */

/*
 * init：按 cfg.choose 装配。失败时 self 已自清理（幂等 fini 可安全再调）。
 * 共享几何先落（slice_count / per_bits / preferred），模式专属字段（MEM words /
 * CHUNK disk_*）由本层填好后交后端 group_setup。
 */
int rfs_bm_init(struct rfs_bm *self, const struct rfs_bm_cfg *cfg)
{
	const struct rfs_bm_ops *ops;
	u64 blk_bits, ino_bits;
	u32 cpu_slots, sc, i;
	int err;

	if (!self || !cfg)
		return -EINVAL;
	memset(self, 0, sizeof(*self));	/* 幂等 fini 前提：先归零 */
	if (cfg->choose != RFS_BM_MODE_MEM && cfg->choose != RFS_BM_MODE_CHUNK)
		return -EINVAL;
	self->mode = cfg->choose;
	ops = rfs_bm_ops_of(self->mode);
	if (!ops)
		return -EINVAL;

	if (self->mode == RFS_BM_MODE_MEM) {
		const no_chunk_slices_cfg_t *mc = &cfg->pure_mem_cfg;

		if (!mc->block_bits_raw || !mc->inode_bits_raw ||
		    !mc->block_bits_count || !mc->inode_bits_count)
			return -EINVAL;
		blk_bits = mc->block_bits_count;
		ino_bits = mc->inode_bits_count;
		cpu_slots = mc->logical_processor_count ? mc->logical_processor_count : 1;
		self->blk.words = (u64 *)mc->block_bits_raw;
		self->ino.words = (u64 *)mc->inode_bits_raw;
	} else {
		const chunk_slices_cfg_t *cc = &cfg->chunk_cfg;

		if (!cc->read_block || !cc->write_block ||
		    !cc->block_bitmap_blocks || !cc->inode_bitmap_blocks)
			return -EINVAL;
		blk_bits = (u64)cc->block_bitmap_blocks << 15;
		ino_bits = (u64)cc->inode_bitmap_blocks << 15;
		cpu_slots = cc->processor_count ? cc->processor_count : 1;
		self->blk.disk_base = cc->block_bitmap_base_FS_block;
		self->blk.disk_blocks = cc->block_bitmap_blocks;
		self->blk.chunk_order = cc->blkbm_chunk_order;
		self->ino.disk_base = cc->inode_bitmap_base_FS_block;
		self->ino.disk_blocks = cc->inode_bitmap_blocks;
		self->ino.chunk_order = cc->inobm_chunk_order;
	}

	sc = bm_derive_slice_count(blk_bits, ino_bits, cpu_slots);
	self->slice_count = sc;
	self->cpu_slots = cpu_slots;

	self->blk.name = "block";
	self->blk.mode = self->mode;
	self->blk.nbits = blk_bits;
	self->blk.n_slices = sc;
	self->blk.cpu_slots = cpu_slots;
	self->blk.per_bits = bm_per_bits(blk_bits, sc);

	self->ino.name = "inode";
	self->ino.mode = self->mode;
	self->ino.nbits = ino_bits;
	self->ino.n_slices = sc;
	self->ino.cpu_slots = cpu_slots;
	self->ino.per_bits = bm_per_bits(ino_bits, sc);

	/* 计数出口：两组共用同一 sink（宿主注入；NULL → 不计数）*/
	self->blk.stat = cfg->stat;
	self->ino.stat = cfg->stat;

	/* per-CPU preferred：i % n_slices 铺开（仿 FPA；避免所有 CPU 首分配挤片 0） */
	self->blk.preferred = RFS_ZALLOC(sizeof(u32) * cpu_slots);
	self->ino.preferred = RFS_ZALLOC(sizeof(u32) * cpu_slots);
	if (!self->blk.preferred || !self->ino.preferred) {
		err = -ENOMEM;
		goto fail;
	}
	for (i = 0; i < cpu_slots; i++) {
		self->blk.preferred[i] = i % sc;
		self->ino.preferred[i] = i % sc;
	}

	err = ops->group_setup(&self->blk, cfg);
	if (err)
		goto fail;
	err = ops->group_setup(&self->ino, cfg);
	if (err)
		goto fail;

	return 0;
fail:
	rfs_bm_fini(self);
	return err;
}

/*
 * fini：释放本类自持资源（slices/cslices/preferred + CHUNK 的 buffer/mgrs/残留链）；
 * **不碰 MEM 的 words**（归拥有者）。幂等。
 */
void rfs_bm_fini(struct rfs_bm *self)
{
	const struct rfs_bm_ops *ops;

	if (!self)
		return;
	ops = rfs_bm_ops_of(self->mode);
	if (ops) {
		ops->group_fini(&self->blk);
		ops->group_fini(&self->ino);
	}
	RFS_FREE(self->blk.preferred);
	RFS_FREE(self->ino.preferred);
	self->blk.preferred = NULL;
	self->ino.preferred = NULL;
	self->slice_count = 0;
	self->cpu_slots = 0;
}

/* 拥有者装载 / 改写位图内存后重算 free_count（MEM）；CHUNK no-op */
void rfs_bm_rescan_free(struct rfs_bm *self)
{
	const struct rfs_bm_ops *ops;

	if (!self)
		return;
	ops = rfs_bm_ops_of(self->mode);
	if (!ops || !ops->group_rescan_free)
		return;
	ops->group_rescan_free(&self->blk);
	ops->group_rescan_free(&self->ino);
}

/* ═══════════════════════════ block 组路由 ═══════════════════════════ */

/*
 * 硬分配：count 连续 + 起始位 2^align 对齐，单片内完成（不跨片）。
 * 路由：本 CPU preferred 片起轮询；ops 内阻塞取锁，无货（-ENOSPC）换片；
 * 命中 → 写回 preferred，并把片自然坐标折成全局位号返出。全败 → RFS_BM_NO_HIT。
 */
u64 rfs_bm_block_alloc_hard(struct rfs_bm *self, u64 count, u64 align)
{
	struct rfs_bm_group *g = &self->blk;
	const struct rfs_bm_ops *ops = rfs_bm_ops_of(self->mode);
	u32 cpu, round;

	/* P5：入口记 attempt，出口必记 ok/fail 之一 ⇒ ok+fail==calls */
	rfs_bm_stat(&g->stat, RFS_BM_ST_ALLOC_CALLS, 1);
	if (!count || !ops || !g->preferred || !g->n_slices) {
		rfs_bm_stat(&g->stat, RFS_BM_ST_ALLOC_FAIL, 1);
		return RFS_BM_NO_HIT;
	}
	cpu = RFS_CPU_ID() % g->cpu_slots;
	for (round = 0; round < g->n_slices; round++) {
		u32 idx = (u32)((RFS_ATOMIC_LOAD_U32(&g->preferred[cpu]) + round) % g->n_slices);
		u64 out = 0;

		if (ops->alloc_hard(g, idx, count, (u8)align, &out) == 0) {
			RFS_ATOMIC_STORE_U32(&g->preferred[cpu], idx);
			rfs_bm_stat(&g->stat, RFS_BM_ST_ALLOC_OK, 1);
			rfs_bm_stat(&g->stat, RFS_BM_ST_ALLOC_BLOCKS, count);
			return bm_to_global(g, idx, out);
		}
	}
	rfs_bm_stat(&g->stat, RFS_BM_ST_ALLOC_FAIL, 1);	/* 全片扫毕 = ENOSPC/碎片 */
	return RFS_BM_NO_HIT;
}

/*
 * 软分配：一次只从单片取（preferred 起轮询；一片为止，不跨片拼），
 * 实际 = min(段长, count)；*remain 出 = 缺口；全片无 → NO_HIT 且 *remain = count。
 * 软分配 align 恒 0（本就是取首空闲段，对齐无意义——god v1/v2 同）。
 */
u64 rfs_bm_block_alloc_soft(struct rfs_bm *self, u64 count, u64 *remain)
{
	struct rfs_bm_group *g = &self->blk;
	const struct rfs_bm_ops *ops = rfs_bm_ops_of(self->mode);
	u32 cpu, round;

	if (remain)
		*remain = count;
	if (!count || !ops || !g->preferred || !g->n_slices)
		return RFS_BM_NO_HIT;
	cpu = RFS_CPU_ID() % g->cpu_slots;
	for (round = 0; round < g->n_slices; round++) {
		u32 idx = (u32)((RFS_ATOMIC_LOAD_U32(&g->preferred[cpu]) + round) % g->n_slices);
		u64 out = 0, left = count;

		if (ops->alloc_soft(g, idx, count, 0, &out, &left) == 0) {
			RFS_ATOMIC_STORE_U32(&g->preferred[cpu], idx);
			if (remain)
				*remain = left;
			rfs_bm_stat(&g->stat, RFS_BM_ST_SOFT_OK, 1);	/* 软分配成功（D2/D3 实测面）*/
			return bm_to_global(g, idx, out);
		}
	}
	return RFS_BM_NO_HIT;
}

/*
 * 软锚定占领（定点对·对偶 block_free）：base 起认领 ≤ count 连续空闲块。
 * 锚点片唯一（不换片）；片尾跨界在循环顶重定位续圈（下一片 base 0 继续，逐片单锁
 * 前缀链 = 真实连续，永不双持）。返回 base（认领 ≥1）/ NO_HIT；*rest 出 = 缺口。
 */
u64 rfs_bm_block_occupy_soft(struct rfs_bm *self, u64 base, u64 count, u64 *rest)
{
	struct rfs_bm_group *g = &self->blk;
	const struct rfs_bm_ops *ops = rfs_bm_ops_of(self->mode);
	u64 b = base, remain = count;

	if (rest)
		*rest = count;
	if (!count || !ops || !g->n_slices)
		return RFS_BM_NO_HIT;
	while (remain > 0) {
		u32 idx = (u32)(b / g->per_bits);
		u64 left = 0, slice_end;

		if (idx >= g->n_slices)
			break;			/* 越界：如实停 */
		if (ops->occupy_soft(g, idx, bm_to_local(g, idx, b), remain, &left) != 0)
			break;			/* 锚点即占用：前缀断，不跨 */
		b += remain - left;		/* claimed = remain − left ≥ 1 */
		remain = left;
		if (!remain)
			break;
		slice_end = bm_slice_end(g, idx);
		if (b != slice_end)
			break;			/* 中途撞占用：跨界续圈无意义 */
		/* 恰至片尾 → 循环顶重定位（下一片 base 0 续圈） */
	}
	if (rest)
		*rest = remain;
	if (count - remain > 0) {
		rfs_bm_stat(&g->stat, RFS_BM_ST_OCCUPY, 1);	/* 锚定认领（≥1 位）*/
		return base;
	}
	return RFS_BM_NO_HIT;
}

/*
 * 区间释放（跨片循环）：物理连续的 run 可能跨片（occupy 的跨界产物）——逐段定位
 * 所属片释放；释放无连续要求，无需跨片双持。
 */
void rfs_bm_block_free(struct rfs_bm *self, u64 base, u64 count)
{
	struct rfs_bm_group *g = &self->blk;
	const struct rfs_bm_ops *ops = rfs_bm_ops_of(self->mode);

	if (!count || !ops || !g->n_slices)
		return;
	rfs_bm_stat(&g->stat, RFS_BM_ST_FREE_CALLS, 1);
	while (count > 0) {
		u32 idx;
		u64 slice_end, n;

		if (base >= g->nbits)
			return;		/* 防御：越界剩余放弃 */
		idx = (u32)(base / g->per_bits);
		if (idx >= g->n_slices)
			return;
		slice_end = bm_slice_end(g, idx);
		n = (base + count <= slice_end) ? count : slice_end - base;
		if (!n)
			return;
		ops->free_interval(g, idx, bm_to_local(g, idx, base), n);
		base += n;
		count -= n;
	}
}

/* 单点测试：MEM 直读位图内存（无锁）；CHUNK 经片 test_interval（片内阻塞锁 +
 * 可能换窗 I/O）——见 ravenfs_bitmap_core.h 对 block_test 的注记。 */
bool rfs_bm_block_test(const struct rfs_bm *self, u64 blk)
{
	const struct rfs_bm_group *g = &self->blk;
	const struct rfs_bm_ops *ops = rfs_bm_ops_of(self->mode);
	u32 idx;

	if (!ops || !g->n_slices)
		return false;
	idx = (u32)(blk / g->per_bits);
	if (idx >= g->n_slices)
		return false;
	return ops->bit_test((struct rfs_bm_group *)g, idx, bm_to_local(g, idx, blk));
}

/* 区间同值测试：[base,base+len) 全 val → 0；异位 → -ENOENT；越界/溢出 → -EINVAL */
int rfs_bm_block_test_interval(const struct rfs_bm *self, u64 base, u64 len, bool val)
{
	const struct rfs_bm_group *g = &self->blk;
	const struct rfs_bm_ops *ops = rfs_bm_ops_of(self->mode);
	u64 end, b;

	if (!ops || !g->n_slices)
		return -EINVAL;
	if (len && base + len < base)
		return -EINVAL;			/* 溢出 */
	end = base + len;
	if (end > g->nbits)
		return -EINVAL;
	b = base;
	while (b < end) {
		u32 idx = (u32)(b / g->per_bits);
		u64 slice_end;
		int rc;

		if (idx >= g->n_slices)
			return -EINVAL;
		slice_end = bm_slice_end(g, idx);
		if (slice_end > end)
			slice_end = end;
		rc = ops->test_interval((struct rfs_bm_group *)g, idx, val,
				       bm_to_local(g, idx, b), slice_end - b);
		if (rc)
			return rc;
		b = slice_end;
	}
	return 0;
}

/* ═══════════════════════════ inode 组路由 ═══════════════════════════ */

/* inode 槽分配：alloc_soft(1)。root inode 由 mkfs 预占，类不做保留区（无 hint=1）。 */
u64 rfs_bm_inode_alloc(struct rfs_bm *self)
{
	struct rfs_bm_group *g = &self->ino;
	const struct rfs_bm_ops *ops = rfs_bm_ops_of(self->mode);
	u32 cpu, round;

	if (!ops || !g->preferred || !g->n_slices)
		return RFS_BM_NO_HIT;
	cpu = RFS_CPU_ID() % g->cpu_slots;
	for (round = 0; round < g->n_slices; round++) {
		u32 idx = (u32)((RFS_ATOMIC_LOAD_U32(&g->preferred[cpu]) + round) % g->n_slices);
		u64 out = 0, left = 0;

		if (ops->alloc_soft(g, idx, 1, 0, &out, &left) == 0) {
			RFS_ATOMIC_STORE_U32(&g->preferred[cpu], idx);
			return bm_to_global(g, idx, out);
		}
	}
	return RFS_BM_NO_HIT;
}

void rfs_bm_inode_free(struct rfs_bm *self, u64 inum)
{
	struct rfs_bm_group *g = &self->ino;
	const struct rfs_bm_ops *ops = rfs_bm_ops_of(self->mode);
	u32 idx;

	if (!ops || !g->n_slices)
		return;
	idx = (u32)(inum / g->per_bits);
	if (idx >= g->n_slices)
		return;
	ops->free_interval(g, idx, bm_to_local(g, idx, inum), 1);
}

bool rfs_bm_inode_test(const struct rfs_bm *self, u64 inum)
{
	const struct rfs_bm_group *g = &self->ino;
	const struct rfs_bm_ops *ops = rfs_bm_ops_of(self->mode);
	u32 idx;

	if (!ops || !g->n_slices)
		return false;
	idx = (u32)(inum / g->per_bits);
	if (idx >= g->n_slices)
		return false;
	return ops->bit_test((struct rfs_bm_group *)g, idx, bm_to_local(g, idx, inum));
}

/* ═══════════════════════════ 审计 / 几何 ═══════════════════════════ */

static u64 bm_group_free_total(const struct rfs_bm_group *g, const struct rfs_bm_ops *ops)
{
	u64 sum = 0;
	u32 i;

	if (!ops)
		return 0;
	for (i = 0; i < g->n_slices; i++)
		sum += ops->free_count(g, i);
	return sum;
}

u64 rfs_bm_block_free_total(const struct rfs_bm *self)
{
	return bm_group_free_total(&self->blk, rfs_bm_ops_of(self->mode));
}

u64 rfs_bm_inode_free_total(const struct rfs_bm *self)
{
	return bm_group_free_total(&self->ino, rfs_bm_ops_of(self->mode));
}

u32 rfs_bm_slice_count(const struct rfs_bm *self)
{
	return self ? self->slice_count : 0;
}

/* ═══════════════════════════ 持久化 ═══════════════════════════ */

/*
 * 快照第 i 片（ino=true 取 inode 组）到 dst。MEM：锁内快照位图字，锁外 I/O 归调用方；
 * CHUNK：产出一份整片一致视图（见后端 ops 语义）。dst 需 ≥ 该片 bits/8 字节。
 */
void rfs_bm_snapshot_slice(const struct rfs_bm *self, bool ino, u32 i, void *dst)
{
	const struct rfs_bm_group *g = ino ? &self->ino : &self->blk;
	const struct rfs_bm_ops *ops = rfs_bm_ops_of(self->mode);

	if (!ops || !dst || i >= g->n_slices)
		return;
	ops->snapshot_slice(g, i, dst);
}

/*
 * CHUNK：整片周期同步（逐片：读 → 合并归还链 → 写回）。MEM 不适用 → -ENOSYS。
 * 逐片循环由本路由层承担（后端 ops->slice_sync 只做单片的窗口机）。
 */
int rfs_bm_chunks_asyn(struct rfs_bm *self)
{
	const struct rfs_bm_ops *ops = rfs_bm_ops_of(self->mode);
	int err = 0;
	u32 i;

	if (!ops || !ops->slice_sync)
		return -ENOSYS;
	for (i = 0; i < self->blk.n_slices; i++) {
		int r = ops->slice_sync(&self->blk, i);

		if (r && !err)
			err = r;
	}
	for (i = 0; i < self->ino.n_slices; i++) {
		int r = ops->slice_sync(&self->ino, i);

		if (r && !err)
			err = r;
	}
	return err;
}

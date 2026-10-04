// SPDX-License-Identifier: GPL-2.0
/*
 * ravenfs_allocator_router.c —— 顶层路由 struct rfs_allocator 实现
 *                              （⇄ god `fs_allocator_t`，juvenile_god/fs_allocator_router.cpp）
 *
 * 逐接口翻译 god 路由层：几何推导 + 双组装配 + 跨片路由 + 审计。
 * 契约（已冻结）= include/linux/ravenfs_allocator_top.h；语义真值 = fs_allocator_router.cpp。
 *
 * 后端 = struct rfs_slice（分片自治单元，主从双侵入红黑树），本层不碰树内部：
 *   · 只做「全局位号 → 片号 = 位号 / per_slice_bits」的**定位**；
 *   · 片自持全局坐标（in_global_base_idx + 全局位号），本层不做局部↔全局换算；
 *   · 本层**不存 nbits**（顶头无该字段），一切按**名义片长 per_slice_bits**切段
 *     （god ctor 同款；合法调用下段内必落在真实片内，真实片尾由片自己把关）。
 *
 * 锁纪律（⇄ god，只换锁原语；详见报告「契约问题 #2」）：
 *   平台收口 ravenfs_platform.h 只暴露 RFS_MUTEX_LOCK（**无 try_lock**），故本实现
 *   对所有路径统一用**可睡互斥锁**（阻塞抢）。分配类 god 的「try_lock 忙则换片」
 *   退化为「阻塞后推进」，正确性不变；死锁面不变——本层**绝不同持两片锁**：
 *     · soft_alloc 逐片：锁 → 片内 soft_alloc → 解锁 → 下一片；
 *     · occupy 片尾跨界：解锁上一片 → 再锁下一片（逐片单锁 = 真实连续）；
 *     · free / test / dump / 审计：单次单锁。
 *
 * 平台纪律：只 include ravenfs_allocator_top.h（+ 其传递的 platform / run_list）
 *   与 ravenfs_allocator_slice.h（+ 传递的 rbtree / run_list）。**无** `#ifdef __KERNEL__`、
 *   无直接内核头；C(gnu11) 与 C++(c++17) 双编 -Wall -Wextra -Werror 均须 exit 0
 *   （void* 一律显式 cast）。
 *
 * 分配纪律：模块**零直接分配**（无 RFS_ZALLOC/RFS_FREE/malloc 字面），凡自持资源
 *   （slices / preferred 数组）一律经 cfg.alloc —— 与片元素池同源。
 */
#include "ravenfs_allocator_top.h"
#include "ravenfs_allocator_slice.h"

/* ══════════════════════════ 类型转换 ══════════════════════════
 * 顶层 struct rfs_mem_alloc（建装配的分配回调）与片 struct rfs_slice_alloc
 * （建树 / 融合取还区间节点）**布局相同**（alloc/free/ctx 同名同型），按字段拷贝。*/
static struct rfs_slice_alloc slice_alloc_of(const struct rfs_mem_alloc *m)
{
	struct rfs_slice_alloc a;

	a.alloc = m->alloc;
	a.free = m->free;
	a.ctx = m->ctx;
	return a;
}

/* ══════════════════════════ 生命周期：关闸 + 在途排空 ══════════════════════════
 * 语义真值见头文件 `struct rfs_allocator`（`is_workable`/`insession_count` 双字段）。
 * 协议（顺序是正确性的一部分）：
 *   入席 enter：**先** seq_cst +1 → **后** seq_cst 读闸；闸=0 → 立刻 -1 拒绝。
 *   打烊 shutdown：先 seq_cst 写闸=0 → 再轮询等 `insession_count` 归 0（有超时）。
 * 等待原语 = 平台收口的「自旋 + 时间轮询」（RFS_CPU_RELAX / RFS_DELAY_MS / RFS_NOW_NS），
 * **环境通用 + 架构通用**：不依赖 waitqueue（内核专属）/ condvar（POSIX 专属），且天然
 * 无丢失唤醒。*/

/* 读闸（seq_cst；顺序要求见头注）。*/
static inline bool rfs_allocator_online(const struct rfs_allocator *self)
{
	return __atomic_load_n((u32 *)&self->is_workable, __ATOMIC_SEQ_CST) != 0;
}

/* 退席：-1（seq_cst）。轮询式打烊无需唤醒。*/
static inline void rfs_allocator_leave(struct rfs_allocator *self)
{
	__atomic_sub_fetch(&self->insession_count, 1, __ATOMIC_SEQ_CST);
}

/* 入席：先 +1 再查闸；已关闸 → 回滚并拒绝（返回 false）。*/
static bool rfs_allocator_enter(struct rfs_allocator *self)
{
	__atomic_add_fetch(&self->insession_count, 1, __ATOMIC_SEQ_CST);
	if (!rfs_allocator_online(self)) {
		rfs_allocator_leave(self);
		return false;
	}
	return true;
}

/* const 接口入席：读访问器也计入在途（计数器是内部生存位，非逻辑读状态——
 * 有界去 const，仿 god 对从树钩子的 const_cast 先例）。*/
static bool rfs_allocator_enter_c(const struct rfs_allocator *self)
{
	return rfs_allocator_enter((struct rfs_allocator *)self);
}

/* RAII 退席守卫：出作用域（含任意提前 return）自动 -1。*/
static inline void rfs_allocator_session_release(struct rfs_allocator **p)
{
	if (p && *p)
		rfs_allocator_leave(*p);
}
#define RFS_IV_SESSION(var, self) \
	RFS_LOCK_GUARD(rfs_allocator_session_release, var, (self))

/* 排空：轮询等 `insession_count` 归 0，或超时。
 * 快路径先自旋若干次（期望排空是亚微秒级），再退化为 1ms 睡眠轮询，避免烧核。*/
static int rfs_allocator_drain(struct rfs_allocator *self, u64 timeout_ms)
{
	u64 deadline = RFS_NOW_NS() + timeout_ms * 1000000ULL;
	u64 spins = 0;

	for (;;) {
		if (__atomic_load_n(&self->insession_count, __ATOMIC_SEQ_CST) == 0)
			return 0;
		if (RFS_NOW_NS() >= deadline)
			return -ETIMEDOUT;
		if (spins < 1024) {
			RFS_CPU_RELAX();
			spins++;
		} else {
			RFS_DELAY_MS(1);
		}
	}
}

/* ══════════════════════════ 装配 / 拆解 ══════════════════════════ */

/* 装配一组：按 nbits 与 slice_count 推 per_slice_bits，逐片 rfs_slice_enable。
 *
 * per = ceil( ceil(nbits / n) / msb ) * msb
 *   msb 倍数 ⇒ 每片起点 begin = i*per 恒为 msb 倍数 ⇒ begin/8 恒 4K 对齐
 *   （满足片 enable 的 slice_base 4K 对齐校验；msb 默认 32768 = 1 位图块）。
 * 末片可短：cnt = min(per, nbits − begin)；ctor 的「末片非空」推导保证 begin < nbits。
 * 失败（数组分配 / 片 enable）→ 返回负值，**不**自行释放 slices/preferred 数组
 *   （由 init 的失败收敛统一回收；slices 已 enable 前缀由收敛路径 shutdown）。*/
static int setup_group(struct rfs_alloc_group *g, u8 *raw, u64 nbits, u64 msb,
		       u32 slice_count, const struct rfs_mem_alloc *mem,
		       const struct rfs_stat_hook *stat)
{
	struct rfs_slice_alloc sa;
	u64 per;
	u32 i;

	per = ((nbits + (u64)slice_count - 1) / (u64)slice_count + msb - 1) / msb * msb;
	if (per == 0)
		per = msb;
	g->per_slice_bits = per;

	g->slices = (struct rfs_slice *)mem->alloc(mem->ctx,
			(u64)slice_count * (u64)sizeof(struct rfs_slice));
	if (!g->slices)
		return -ENOMEM;
	memset(g->slices, 0, (size_t)slice_count * sizeof(struct rfs_slice));
	for (i = 0; i < slice_count; i++)
		RFS_MUTEX_INIT(&g->slices[i].lock);

	sa = slice_alloc_of(mem);
	for (i = 0; i < slice_count; i++) {
		struct rfs_slice_cfg cfg;
		u64 begin = (u64)i * per;
		u64 cnt;

		/* 防御：ctor 的「末片非空迭代」已保证 begin < nbits；真触发即参数不符。*/
		if (begin >= nbits)
			return -EINVAL;
		cnt = nbits - begin;
		if (cnt > per)
			cnt = per;

		memset(&cfg, 0, sizeof(cfg));
		cfg.slice_base = raw + begin / 8;	/* begin 恒 msb 倍数 → 4K 对齐 */
		cfg.in_global_base_idx = begin;
		cfg.slice_bitscount = cnt;
		cfg.alloc = sa;
		cfg.stat = *stat;		/* 下传 al_ 计数钩子（片内埋点）*/
		if (rfs_slice_enable(&g->slices[i], &cfg) != 0)
			return -EINVAL;
	}
	return 0;
}

/* 拆一组：逐片 shutdown（对位清账；未使能片 -EINVAL 忽略），再释放自持数组。
 * mem 由调用方给出（装配失败时是 cfg.alloc；fini 时从已 enable 片的 alloc 回收）。*/
static void teardown_group(struct rfs_alloc_group *g, u32 slice_count,
			   const struct rfs_mem_alloc *mem)
{
	u32 i;

	if (g->slices) {
		for (i = 0; i < slice_count; i++)
			(void)rfs_slice_shutdown(&g->slices[i]);
		if (mem->free)
			mem->free(mem->ctx, g->slices);
		g->slices = NULL;
	}
	if (g->preferred) {
		if (mem->free)
			mem->free(mem->ctx, g->preferred);
		g->preferred = NULL;
	}
	g->per_slice_bits = 0;
}

/* ══════════════════════════ 生命周期 ══════════════════════════ */

int rfs_allocator_init(struct rfs_allocator *self, const struct rfs_slices_cfg *cfg)
{
	struct rfs_mem_alloc mem;
	u64 msb, n, cap, per_b, per_i;
	u32 i;

	if (!self)
		return -EINVAL;
	memset(self, 0, sizeof(*self));		/* 空态：失败即自清理，fini 幂等 */

	if (!cfg || !cfg->block_bits_raw || !cfg->inode_bits_raw ||
	    cfg->block_bits_count == 0 || cfg->inode_bits_count == 0)
		return -EINVAL;
	if (!cfg->alloc.alloc || !cfg->alloc.free)
		return -EINVAL;			/* 模块零直接分配：装配回调必填 */

	/* 计数器钩子：先内部化，令装配路径（含失败）也能上报（P1）。
	 * 注：本函数若在装配中途失败会 `memset(self,0)` 清钩子——故**装配成功/失败/片数**
	 * 由 FS 适配层按其返回码上报（见 allocator_adapter.c），不在此处自报。*/
	self->stat = cfg->stat;
	mem = cfg->alloc;
	msb = cfg->min_slice_bits ? cfg->min_slice_bits : RFS_IV_SLICE_MIN_BITS;

	/* 2^n 上界（仿 FPA；0 → 由 cpu_slots 兜底单槽）*/
	n = 1;
	while (n < cfg->logical_processor_count && n < (1ULL << 30))
		n <<= 1;
	/* 满片数约束：两组各须容纳 ≥1 片（每片 ≥ msb 位）。*/
	{
		u64 blk_cap = cfg->block_bits_count / msb;
		u64 ino_cap = cfg->inode_bits_count / msb;

		cap = blk_cap < ino_cap ? blk_cap : ino_cap;
	}
	if (cap == 0)
		cap = 1;
	if (n > cap)
		n = cap;
	/* 末片非空迭代：per 取整到 msb 倍数后，两组末片都必须非空，否则 n 减半。*/
	for (;;) {
		per_b = ((cfg->block_bits_count + n - 1) / n + msb - 1) / msb * msb;
		per_i = ((cfg->inode_bits_count + n - 1) / n + msb - 1) / msb * msb;
		if (per_b == 0)
			per_b = msb;
		if (per_i == 0)
			per_i = msb;
		if ((n - 1) * per_b < cfg->block_bits_count &&
		    (n - 1) * per_i < cfg->inode_bits_count)
			break;
		if (n == 1)
			break;
		n >>= 1;
	}
	self->slice_count = (u32)n;
	self->cpu_slots = cfg->logical_processor_count ? cfg->logical_processor_count : 1;

	/* per-CPU preferred：每 CPU 记住上次命中的亲和片（初值 = CPU i 亲和片 i%n）。*/
	self->blk_.preferred = (u32 *)mem.alloc(mem.ctx,
			(u64)self->cpu_slots * (u64)sizeof(u32));
	if (!self->blk_.preferred)
		return -ENOMEM;
	self->ino_.preferred = (u32 *)mem.alloc(mem.ctx,
			(u64)self->cpu_slots * (u64)sizeof(u32));
	if (!self->ino_.preferred) {
		mem.free(mem.ctx, self->blk_.preferred);
		self->blk_.preferred = NULL;
		return -ENOMEM;
	}
	for (i = 0; i < self->cpu_slots; i++) {
		self->blk_.preferred[i] = (u32)(i % self->slice_count);
		self->ino_.preferred[i] = (u32)(i % self->slice_count);
	}

	/* 双组 enable：block 组先、inode 组后；任一失败收敛（自清理）。*/
	if (setup_group(&self->blk_, cfg->block_bits_raw, cfg->block_bits_count,
			msb, self->slice_count, &mem, &self->stat) != 0) {
		teardown_group(&self->blk_, self->slice_count, &mem);
		teardown_group(&self->ino_, self->slice_count, &mem);
		memset(self, 0, sizeof(*self));
		return -EINVAL;
	}
	if (setup_group(&self->ino_, cfg->inode_bits_raw, cfg->inode_bits_count,
			msb, self->slice_count, &mem, &self->stat) != 0) {
		teardown_group(&self->blk_, self->slice_count, &mem);
		teardown_group(&self->ino_, self->slice_count, &mem);
		memset(self, 0, sizeof(*self));
		return -EINVAL;
	}
	/* 使能：生命周期闸门（seq_cst 发布；双字段协议见头注）。*/
	__atomic_store_n(&self->is_workable, 1, __ATOMIC_SEQ_CST);
	return 0;
}

/* 回收装配分配器：struct rfs_allocator 未留存 cfg.alloc（契约缺口，见报告 #1），
 * 从任一已 enable 片的 alloc 字段回收（装配时由同源 mem 拷入，回调一致）。*/
static struct rfs_mem_alloc allocator_mem_of(const struct rfs_allocator *self)
{
	struct rfs_mem_alloc m;
	const struct rfs_slice *s = NULL;

	m.alloc = NULL;
	m.free = NULL;
	m.ctx = NULL;
	if (self->blk_.slices)
		s = &self->blk_.slices[0];
	else if (self->ino_.slices)
		s = &self->ino_.slices[0];
	if (s) {
		m.alloc = s->alloc.alloc;
		m.free = s->alloc.free;
		m.ctx = s->alloc.ctx;
	}
	return m;
}

void rfs_allocator_fini(struct rfs_allocator *self)
{
	struct rfs_mem_alloc mem;
	int rc;

	if (!self)
		return;
	/* 打烊：关闸 + 等在途退席。超时 → fail-safe：**拒绝释放**（留“关闸未释放”态，
	 * 宁可泄漏不 UAF），由调用方处置。*/
	rc = rfs_allocator_shutdown(self, 0);
	if (rc != 0) {
		RFS_LOG("ravenfs: allocator fini: drain timed out (%d) — refuse to free (fail-safe)\n",
			rc);
		return;
	}
	mem = allocator_mem_of(self);
	teardown_group(&self->blk_, self->slice_count, &mem);
	teardown_group(&self->ino_, self->slice_count, &mem);
	memset(self, 0, sizeof(*self));
}

int rfs_allocator_shutdown(struct rfs_allocator *self, u64 timeout_ms)
{
	if (!self)
		return -EINVAL;
	/* 关闸（seq_cst 发布）：此后新入席者读到 0 即退席拒绝；已在途者不被打断。*/
	__atomic_store_n(&self->is_workable, 0, __ATOMIC_SEQ_CST);
	if (timeout_ms == 0)
		timeout_ms = RFS_ALLOCATOR_DRAIN_TIMEOUT_MS;
	return rfs_allocator_drain(self, timeout_ms);
}

/* ══════════════════════════ 内部小轮子 ══════════════════════════ */

/* 统计一条 run 链总位数（Σ len）——soft 累计交付量的单源。*/
static u64 sum_runs(const struct rfs_run_list *runs)
{
	const struct rfs_run_node *n;
	u64 total = 0;

	for (n = runs->head; n; n = n->next)
		total += n->run.len;
	return total;
}

/* ══════════════════════════ block 组 ══════════════════════════ */

u64 rfs_allocator_block_alloc_hard(struct rfs_allocator *self, u64 require_count,
				   u8 align_log2)
{
	u32 cpu;
	u64 round;

	if (!self)
		return RFS_IV_NO_HIT;
	if (!rfs_allocator_enter(self))
		return RFS_IV_NO_HIT;
	RFS_IV_SESSION(rfs_iv_guard, self);
	if (require_count == 0 || !self->blk_.slices ||
	    self->slice_count == 0 || self->cpu_slots == 0 || !self->blk_.preferred)
		return RFS_IV_NO_HIT;

	cpu = (u32)RFS_CPU_ID() % self->cpu_slots;
	for (round = 0; round < self->slice_count; round++) {
		u64 idx = ((u64)RFS_ATOMIC_LOAD_U32(&self->blk_.preferred[cpu]) + round) % self->slice_count;
		struct rfs_slice *s = &self->blk_.slices[idx];
		u64 out = 0;
		int rc;

		RFS_MUTEX_LOCK(&s->lock);
		rc = rfs_slice_alloc_hard(s, align_log2, require_count, &out);
		RFS_MUTEX_UNLOCK(&s->lock);
		if (rc == 0) {
			RFS_AL_STAT1(self->stat,
				     round == 0 ? AL_ROUTE_FIRST_HIT : AL_ROUTE_RESCAN);
			RFS_ATOMIC_STORE_U32(&self->blk_.preferred[cpu], (u32)idx);	/* 回写亲和片 */
			RFS_AL_STAT1(self->stat, AL_BLK_ALLOC_HARD_OK);
			return out;
		}
	}
	RFS_AL_STAT1(self->stat, AL_BLK_ALLOC_HARD_FAIL);
	return RFS_IV_NO_HIT;		/* 全片失败：原子未命中（0 是合法块号）*/
}

int rfs_allocator_block_alloc_soft(struct rfs_allocator *self, u64 require_count,
				   u8 align_log2, struct rfs_run_list *runs)
{
	u32 cpu;
	u64 round;

	if (!self)
		return -EINVAL;
	if (!rfs_allocator_enter(self))
		return -EINVAL;
	RFS_IV_SESSION(rfs_iv_guard, self);
	if (!runs)
		return -EINVAL;
	if (require_count == 0)
		return -EINVAL;
	if (!self->blk_.slices || self->slice_count == 0 ||
	    self->cpu_slots == 0 || !self->blk_.preferred)
		return -EINVAL;

	cpu = (u32)RFS_CPU_ID() % self->cpu_slots;
	for (round = 0; round < self->slice_count; round++) {
		u64 idx;
		struct rfs_slice *s;
		u64 remaining;
		bool sat = false;
		int rc;

		if (sum_runs(runs) >= require_count)
			break;			/* 已凑满：提前收敛 */

		idx = ((u64)RFS_ATOMIC_LOAD_U32(&self->blk_.preferred[cpu]) + round) % self->slice_count;
		s = &self->blk_.slices[idx];
		remaining = require_count - sum_runs(runs);

		RFS_MUTEX_LOCK(&s->lock);
		rc = rfs_slice_soft_alloc(s, align_log2, remaining, runs, &sat);
		RFS_MUTEX_UNLOCK(&s->lock);

		if (rc == 0) {
			RFS_ATOMIC_STORE_U32(&self->blk_.preferred[cpu], (u32)idx);	/* 本片交付 ≥1 位 */
		} else if (rc == -ENOSPC) {
			continue;		/* 本片吃干：换下一片 */
		} else {
			/* 非空片 / 非法参数类错误：回滚已取段后透传（runs 回空）*/
			(void)rfs_allocator_block_intervals_free(self, runs);
			return rc;
		}
	}

	if (sum_runs(runs) >= require_count) {
		RFS_AL_STAT1(self->stat, AL_BLK_ALLOC_SOFT_OK);
		return 0;			/* 凑满 */
	}

	/* 穷尽所有片仍不够：回滚已取段（all-or-nothing），返回空间不足。*/
	RFS_AL_STAT1(self->stat, AL_BLK_ALLOC_SOFT_FAIL);
	(void)rfs_allocator_block_intervals_free(self, runs);
	return -ENOSPC;
}

int rfs_allocator_block_occupy_soft(struct rfs_allocator *self, u64 blkbase,
				    u64 require_count, u64 *rest_count)
{
	u64 base, remain;

	if (!self)
		return -EINVAL;
	if (!rfs_allocator_enter(self))
		return -EINVAL;
	RFS_IV_SESSION(rfs_iv_guard, self);
	if (rest_count)
		*rest_count = require_count;
	if (require_count == 0 || !self->blk_.slices ||
	    self->slice_count == 0 || self->blk_.per_slice_bits == 0)
		return -EINVAL;

	base = blkbase;
	remain = require_count;
	while (remain > 0 && (base / self->blk_.per_slice_bits) < self->slice_count) {
		u64 idx = base / self->blk_.per_slice_bits;	/* 锚点所在片 */
		struct rfs_slice *s = &self->blk_.slices[idx];
		u64 left = 0;			/* 入 = 本锚点无先验满足量 */
		u64 claimed;
		u64 nominal_slice_end;
		int rc;

		if (base != blkbase)		/* 已续圈跨片（非锚点原片）*/
			RFS_AL_STAT1(self->stat, AL_ROUTE_CROSS_SLICE);

		RFS_MUTEX_LOCK(&s->lock);	/* 定点对：阻塞抢，不换片 */
		rc = rfs_slice_soft_occupy(s, base, remain, &left);
		RFS_MUTEX_UNLOCK(&s->lock);
		if (rc != 0)
			break;			/* 锚点即占用 / 片内无可占：前缀断 */

		claimed = remain - left;
		base += claimed;
		remain = left;
		if (remain == 0)
			break;			/* 需求已满 */

		/* 只有恰好顶到片尾才值得续圈；中途撞占用（base < 片尾）→ 前缀断。*/
		nominal_slice_end = (idx + 1) * self->blk_.per_slice_bits;
		if (base != nominal_slice_end)
			break;
		/* base == 下一片首：循环顶重定位续圈；末片真实尾由片内夹断。*/
	}

	if (rest_count)
		*rest_count = remain;
	if (require_count - remain == 0) {
		RFS_AL_STAT1(self->stat, AL_BLK_OCCUPY_FAIL);	/* 一位未占 */
		return -ENOSPC;
	}
	if (remain == 0)
		RFS_AL_STAT1(self->stat, AL_BLK_OCCUPY_OK);	/* 全满足 */
	else
		RFS_AL_STAT1(self->stat, AL_BLK_OCCUPY_PARTIAL);/* 部分认领（前缀断）*/
	return 0;
}

/* 区间释放的内部实现：不含入席 / 计数（计数在公开入口做一次，防 rollback 重复计）。
 * 前置：已在席位、几何已校验、count > 0。跨片逐段定位所属片。*/
static int block_free_range(struct rfs_allocator *self, u64 blkbase, u64 count)
{
	while (count > 0) {
		u64 idx = blkbase / self->blk_.per_slice_bits;
		u64 slice_end, n;
		struct rfs_slice *s;
		int rc;

		if (idx >= self->slice_count)
			return -EINVAL;		/* 越出片数组：防御 */
		slice_end = (idx + 1) * self->blk_.per_slice_bits;
		n = count;
		if (blkbase + n > slice_end)
			n = slice_end - blkbase;	/* 本片内可释放量 */
		if (n == 0)
			return -EINVAL;		/* 防御：理论上不达 */

		s = &self->blk_.slices[idx];
		RFS_MUTEX_LOCK(&s->lock);	/* 释放不可丢：阻塞抢 */
		rc = rfs_slice_free_interval(s, blkbase, n);
		RFS_MUTEX_UNLOCK(&s->lock);
		if (rc != 0)
			return rc;		/* 透传（重叠 -EEXIST / 越界 -EINVAL）*/
		blkbase += n;
		count -= n;
	}
	return 0;
}

int rfs_allocator_block_free(struct rfs_allocator *self, u64 blkbase, u64 count)
{
	if (!self)
		return -EINVAL;
	if (!rfs_allocator_enter(self))
		return -EINVAL;
	RFS_IV_SESSION(rfs_iv_guard, self);
	if (count == 0)
		return 0;			/* 空释放：无操作 */
	if (!self->blk_.slices || self->slice_count == 0 ||
	    self->blk_.per_slice_bits == 0)
		return -EINVAL;
	RFS_AL_STAT1(self->stat, AL_BLK_FREE_CALLS);
	RFS_AL_STAT(self->stat, AL_BLK_FREE_BLOCKS, count);
	return block_free_range(self, blkbase, count);
}

int rfs_allocator_block_intervals_free(struct rfs_allocator *self,
				       struct rfs_run_list *runs)
{
	if (!self)
		return -EINVAL;
	if (!rfs_allocator_enter(self))
		return -EINVAL;
	RFS_IV_SESSION(rfs_iv_guard, self);
	if (!runs)
		return -EINVAL;
	/* 消费链：先取副本 + 摘链 + 还节点，再释放；遇错余段留链（已消费段不回滚）。
	 * 用内部 `block_free_range`（不再入席 / 不再计 AL_BLK_FREE_*）——本函数本身即一次
	 * 释放调用，rollback 路径（soft_alloc 失败）也不得把「回滚」记成用户释放。*/
	{
		u64 total = sum_runs(runs);

		if (total)
			RFS_AL_STAT(self->stat, AL_BLK_FREE_BLOCKS, total);
	}
	RFS_AL_STAT1(self->stat, AL_BLK_FREE_CALLS);
	while (runs->head) {
		struct rfs_run_node *node = runs->head;
		u64 base = node->run.base;
		u64 len = node->run.len;
		int rc;

		runs->head = node->next;
		runs->alloc.free(runs->alloc.ctx, node);
		rc = block_free_range(self, base, len);
		if (rc != 0)
			return rc;
	}
	return 0;
}

bool rfs_allocator_block_test(const struct rfs_allocator *self, u64 blk)
{
	u64 idx;
	struct rfs_slice *s;
	bool occupied;

	if (!self)
		return false;
	if (!rfs_allocator_enter_c(self))
		return false;
	RFS_IV_SESSION(rfs_iv_guard, (struct rfs_allocator *)self);
	if (!self->blk_.slices || self->slice_count == 0 ||
	    self->blk_.per_slice_bits == 0)
		return false;
	idx = blk / self->blk_.per_slice_bits;
	if (idx >= self->slice_count)
		return false;

	s = &self->blk_.slices[idx];
	RFS_MUTEX_LOCK(&s->lock);
	occupied = (rfs_slice_test_interval(s, true, blk, 1) == 0);
	RFS_MUTEX_UNLOCK(&s->lock);
	return occupied;
}

/* ══════════════════════════ inode 组 ══════════════════════════ */

int rfs_allocator_inode_alloc(struct rfs_allocator *self, u64 *out_inum)
{
	u32 cpu;
	u64 round;

	if (!self)
		return -EINVAL;
	if (!rfs_allocator_enter(self))
		return -EINVAL;
	RFS_IV_SESSION(rfs_iv_guard, self);
	if (!out_inum || !self->ino_.slices || self->slice_count == 0 ||
	    self->cpu_slots == 0 || !self->ino_.preferred)
		return -EINVAL;

	cpu = (u32)RFS_CPU_ID() % self->cpu_slots;
	for (round = 0; round < self->slice_count; round++) {
		u64 idx = ((u64)RFS_ATOMIC_LOAD_U32(&self->ino_.preferred[cpu]) + round) % self->slice_count;
		struct rfs_slice *s = &self->ino_.slices[idx];
		u64 out = 0;
		int rc;

		RFS_MUTEX_LOCK(&s->lock);
		rc = rfs_slice_alloc_hard(s, 0, 1, &out);	/* 单 bit：对齐 0 */
		RFS_MUTEX_UNLOCK(&s->lock);
		if (rc == 0) {
			RFS_AL_STAT1(self->stat,
				     round == 0 ? AL_ROUTE_FIRST_HIT : AL_ROUTE_RESCAN);
			RFS_ATOMIC_STORE_U32(&self->ino_.preferred[cpu], (u32)idx);	/* 回写亲和片 */
			*out_inum = out;
			RFS_AL_STAT1(self->stat, AL_INO_ALLOC_OK);
			return 0;
		}
	}
	RFS_AL_STAT1(self->stat, AL_INO_ALLOC_FAIL);
	return -ENOSPC;			/* 全片无空闲 inode */
}

void rfs_allocator_inode_free(struct rfs_allocator *self, u64 inum)
{
	u64 idx;
	struct rfs_slice *s;

	if (!self)
		return;
	if (!rfs_allocator_enter(self))
		return;
	RFS_IV_SESSION(rfs_iv_guard, self);
	if (!self->ino_.slices ||
	    self->slice_count == 0 || self->ino_.per_slice_bits == 0)
		return;
	idx = inum / self->ino_.per_slice_bits;
	if (idx >= self->slice_count)
		return;
	RFS_AL_STAT1(self->stat, AL_INO_FREE_CALLS);

	s = &self->ino_.slices[idx];
	RFS_MUTEX_LOCK(&s->lock);
	(void)rfs_slice_free_interval(s, inum, 1);
	RFS_MUTEX_UNLOCK(&s->lock);
}

bool rfs_allocator_inode_test(const struct rfs_allocator *self, u64 inum)
{
	u64 idx;
	struct rfs_slice *s;
	bool occupied;

	if (!self)
		return false;
	if (!rfs_allocator_enter_c(self))
		return false;
	RFS_IV_SESSION(rfs_iv_guard, (struct rfs_allocator *)self);
	if (!self->ino_.slices || self->slice_count == 0 ||
	    self->ino_.per_slice_bits == 0)
		return false;
	idx = inum / self->ino_.per_slice_bits;
	if (idx >= self->slice_count)
		return false;

	s = &self->ino_.slices[idx];
	RFS_MUTEX_LOCK(&s->lock);
	occupied = (rfs_slice_test_interval(s, true, inum, 1) == 0);
	RFS_MUTEX_UNLOCK(&s->lock);
	return occupied;
}

/* ══════════════════════════ 几何 / 审计 ══════════════════════════ */

u32 rfs_allocator_slice_count(const struct rfs_allocator *self)
{
	return self ? self->slice_count : 0;
}

u32 rfs_allocator_cpu_slots(const struct rfs_allocator *self)
{
	return self ? self->cpu_slots : 0;
}

u64 rfs_allocator_block_per_slice_bits(const struct rfs_allocator *self)
{
	return self ? self->blk_.per_slice_bits : 0;
}

u64 rfs_allocator_inode_per_slice_bits(const struct rfs_allocator *self)
{
	return self ? self->ino_.per_slice_bits : 0;
}

u64 rfs_allocator_block_free_total(const struct rfs_allocator *self)
{
	u64 sum = 0;
	u32 i;

	if (!self)
		return 0;
	if (!rfs_allocator_enter_c(self))
		return 0;
	RFS_IV_SESSION(rfs_iv_guard, (struct rfs_allocator *)self);
	if (!self->blk_.slices || self->slice_count == 0)
		return 0;
	for (i = 0; i < self->slice_count; i++) {
		struct rfs_slice *s = &self->blk_.slices[i];

		RFS_MUTEX_LOCK(&s->lock);
		sum += rfs_slice_free_count(s);
		RFS_MUTEX_UNLOCK(&s->lock);
	}
	return sum;
}

u64 rfs_allocator_inode_free_total(const struct rfs_allocator *self)
{
	u64 sum = 0;
	u32 i;

	if (!self)
		return 0;
	if (!rfs_allocator_enter_c(self))
		return 0;
	RFS_IV_SESSION(rfs_iv_guard, (struct rfs_allocator *)self);
	if (!self->ino_.slices || self->slice_count == 0)
		return 0;
	for (i = 0; i < self->slice_count; i++) {
		struct rfs_slice *s = &self->ino_.slices[i];

		RFS_MUTEX_LOCK(&s->lock);
		sum += rfs_slice_free_count(s);
		RFS_MUTEX_UNLOCK(&s->lock);
	}
	return sum;
}

u32 rfs_allocator_block_preferred_of(const struct rfs_allocator *self, u32 cpu)
{
	if (!self)
		return 0;
	if (!rfs_allocator_enter_c(self))
		return 0;
	RFS_IV_SESSION(rfs_iv_guard, (struct rfs_allocator *)self);
	if (!self->blk_.preferred || cpu >= self->cpu_slots)
		return 0;
	return RFS_ATOMIC_LOAD_U32(&self->blk_.preferred[cpu]);
}

u32 rfs_allocator_inode_preferred_of(const struct rfs_allocator *self, u32 cpu)
{
	if (!self)
		return 0;
	if (!rfs_allocator_enter_c(self))
		return 0;
	RFS_IV_SESSION(rfs_iv_guard, (struct rfs_allocator *)self);
	if (!self->ino_.preferred || cpu >= self->cpu_slots)
		return 0;
	return RFS_ATOMIC_LOAD_U32(&self->ino_.preferred[cpu]);
}

/* ══════════════════════════ 持久化出口（纯快照）══════════════════════════
 * 落盘归 FS 适配层；本模块只出**纯内存**逐片快照：持该片锁遍历其空闲区间
 * （base 升序）dump 进 out。绝不碰 IO。*/
int rfs_allocator_dump_slice(struct rfs_allocator *self, bool ino, u32 slice,
			     struct rfs_run_dlist *out)
{
	struct rfs_alloc_group *g;
	struct rfs_slice *s;

	if (!self || !out)
		return -EINVAL;
	if (!rfs_allocator_enter(self))
		return -EINVAL;
	RFS_IV_SESSION(rfs_iv_guard, self);
	if (slice >= self->slice_count)
		return -EINVAL;
	g = ino ? &self->ino_ : &self->blk_;
	if (!g->slices)
		return -EINVAL;

	s = &g->slices[slice];
	RFS_MUTEX_GUARD(rfs_dump_guard, &s->lock);
	return rfs_slice_all_dump(s, out);
}

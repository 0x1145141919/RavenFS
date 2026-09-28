// SPDX-License-Identifier: GPL-2.0
/*
 * bitmap.c —— RavenFS ko：**双位图分片分配器**（全量常驻内存，学 god `bitmap_slices`）
 *
 * 决议（2026-09-26 深夜 + 2026-09-27 复裁，用户裁）：位图 = **全量常驻内存分片**，**不是缓存行**
 * （不纳入 inode/mdata 的缓存状态机与「架构无关回刷 driver」）；**保留分片设计 + 片级同步机制**；
 * 持久化 = 挂载装载 + **分片级同步/回写**（脏分片回写 + `sync_fs`/`put_super` 刷回）——
 * **不得挂载后仅驻留内存不回写**；仍**不做**「bh + mark_buffer_dirty 增量/页缓存推进」那套。
 * 移植规格：`Docs/linux/ravenfs-linux-ko-handover.md` §3.5（god `bitmap_slices` v1.1 → ko）。
 *
 * 设计：
 *   · 两组：block 组（块位图，1 bit/FS_block）与 inode 组（inode 位图，1 bit/槽）。
 *   · 分片：切片数由「2^n ≥ CPU 数 ∩ 满片数上限 ∩ 末片非空迭代」共享推导；
 *     片下限 = 1 个位图块 = 32768 位（4K 对齐）。每片 `spinlock_t`。
 *   · 锁纪律：**分配**只在片内做位运算（持自旋锁）；**释放**跨片逐段清位。
 *     临界区内不做 I/O / 不分配内存。
 *   · 片级同步（2026-09-27 加，**无脏标记**）：分片数按 CPU 切，逐片追踪脏位不划算 ⇒ 不做。
 *     同步 = **锁内 memcpy 快照整片字 → 锁外逐块 I/O**（I/O 绝不入自旋锁）；写者之间由
 *     组内 `wb_lock`(mutex) 串行（不挡分配/释放）。触发：① TTL 线程每 `bitmap_sync_seconds`
 *     （30s）把**所有分片**全同步一遍；② `sync_fs`/`put_super`/`fsync` 走同一 `ravenfs_bitmap_flush`。
 *     崩溃序无法通解，只能靠 ① 把「位图落后内存」的窗口收到 ≤30s 缓解。
 *   · 无保留区：只有 root inode（=16，mkfs 硬编码）被占用；0..15 与 17.. 全部可正常分配。
 *     （2026-09-26 更正：曾误写“inode 组 [0,16) 保留”，该说法不存在，已铲除。）
 *   · **D2/D3 对齐 god（2026-09-28 用户裁）**：补 god 三原语 `alloc_soft` / `soft_occupy` /
 *     `test_interval` + 软分配 / 锚定认领路由（语义 1:1 对齐 `juvenile_god/bitmap_slices.cpp`；
 *     仅坐标系差异：god 片内偏移，ko 全局位号）；preferred 初值 `cpu % slice_count` 铺开，
 *     **搜索式分配（硬/软）一律从 preferred 起扫**（锚定认领锚点片唯一、例外，god 同）。
 *   · alloc 锁维持**阻塞 spinlock**（不改 try —— 2026-09-28 用户裁：阻塞恰配合后续改制）。
 *   · 1 = 已用，0 = 空闲。
 *
 *   ★ 2026-09-28 CHUNK 化改制移植（规格 `Docs/runtime_tech/03_chunk_backed_slices.md`,
 *     参照 `juvenile_god/bitmap_slices.cpp`；§12 = ko 移植注记）：
 *     · 新增 **backing 后端**：FULL（默认，本注前全部现行为逐位不变 = 红线）|
 *       CHUNK（懒加载：每片一个固定 chunk 缓冲 + 控制器数组 + 懒加载 + 返还记录）。
 *     · CHUNK 经**模块参数** `bitmap_backing=1` 显式开启（选最简机制：super.c/挂载项
 *       越界不可改；模块参数是写范围内唯一显式开关），`bitmap_chunk_order` 默认 4。
 *     · 移植映射（§12）：`chg_mu`→可睡 `struct mutex`（P0 复核调用点全进程上下文）；
 *       record 节点池→`kmem_cache` 预建（锁内零分配铁律）；`chunk_io`→`sb_bread`/
 *       `mark_buffer_dirty`（`disk_block_rel` 经 `ravenfs_dev_blk` 换算 FS block 号）；
 *       mount 扫描→改造 `rfs_bm_group_load`（计数+摘要，不再全量 memcpy 进 vzalloc）；
 *       TTL→`rfs_bm_group_sync` 三态分派。
 *     · **锁纪律维持 ko 现状：alloc 阻塞 spinlock**（2026-09-28 用户裁，勿改 try 换片）；
 *       盘 I/O 只在持本片 `chg_mu` 下发起、**零进 slice_lock**（§10.3 铁律）。
 *     · 坐标系：ko 全局位号；**片内窗口偏移 = 全局位号 − 片 base_idx**（god 片内坐标
 *       仅在其片基址重指技巧下等价）；差异清单见 `Docs/linux/03_port_progress.md`。
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/timekeeping.h>
#include <linux/smp.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/string.h>
#include <linux/bitops.h>

#include "ravenfs.h"
#include "ravenfs_bitmap.h"

/* 片下限：1 个位图块 = 4096B × 8 = 32768 位（保证片基址 4K 对齐） */
#define RFS_BM_SLICE_MIN_BITS	32768ULL
#define RFS_BM_NO_HIT		(~0ULL)

/* ── CHUNK 后端常量（§12/§2；FULL 默认不参与） ── */
#define RFS_BM_BACKING_FULL	0	/* 默认：全量常驻 = 本注前现行为（红线） */
#define RFS_BM_BACKING_CHUNK	1	/* 懒加载：每片一个 chunk 缓冲 + 控制器数组 */
#define RFS_BM_CHUNK_ORDER_DEF	4	/* 默认 chunk = 4K<<4 = 64K */
#define RFS_BM_CHUNK_ORDER_MAX	8	/* 4K<<8 = 1MB */
#define RFS_BM_CHUNK_NONE	0xFFFFFFFFu	/* online_chunk 哨兵（无窗上线） */
#define RFS_BM_REC_POOL_DEPTH	64	/* §6 record 节点池默认深度 */

/* 2bit 摘要（§7）：ALL_USED = 盘态无空闲（alloc 扫描免载入跳过）/ ALL_FREE = 整窗可判
 * 空闲 / MIXED = 须上线查。record 追加必降级到 MIXED（§6 摘要降级铁律，否则错误 ENOSPC）。 */
enum { RFS_BM_SUM_ALL_USED = 0, RFS_BM_SUM_ALL_FREE = 1, RFS_BM_SUM_MIXED = 2 };

/* 片内 chunk 快慢路径内部码（不外泄）：LOCKED = 已持 s->lock 且窗在线；
 * SKIP = 本窗暂不可用（摘要免载入 / 换片反复被抢）→ 路由跳过本窗。 */
enum { RFS_BM_WIN_LOCKED = 0, RFS_BM_WIN_SKIP = 1 };

/* ── CHUNK 模块参数（显式开关；装配期读取，默认 FULL = 现行为） ── */
static int bitmap_backing = RFS_BM_BACKING_FULL;
module_param(bitmap_backing, int, 0444);
MODULE_PARM_DESC(bitmap_backing, "位图 backing：0=FULL 全量常驻（默认）｜1=CHUNK 懒加载");

static uint bitmap_chunk_order = RFS_BM_CHUNK_ORDER_DEF;
module_param(bitmap_chunk_order, uint, 0444);
MODULE_PARM_DESC(bitmap_chunk_order, "CHUNK 模式 chunk = 4K<<order 字节；有效 0..8，默认 4 (64K)");

/* ── 位原语（小端字内） ── */
static inline bool rfs_bm_get(const u64 *w, u64 i)
{
	return (w[i >> 6] >> (i & 63)) & 1ULL;
}
static inline void rfs_bm_set(u64 *w, u64 i)
{
	w[i >> 6] |= 1ULL << (i & 63);
}
static inline void rfs_bm_clear(u64 *w, u64 i)
{
	w[i >> 6] &= ~(1ULL << (i & 63));
}

/*
 * 从 `from` 起找下一个**空闲段** [seg_s, seg_e)（均 < nbits）。
 * max_extend>0：段长够 max_extend 即截断（防稀疏大位图 1-bit 分配热点）。
 */
static bool next_free_seg(const u64 *w, u64 nbits, u64 from, u64 max_extend,
			  u64 *seg_s, u64 *seg_e)
{
	u64 i = from;

	while (i < nbits) {
		u64 word = ~w[i >> 6];
		u64 bit;

		word &= ~0ULL << (i & 63);
		if (!word) {
			i = ((i >> 6) + 1) << 6;
			continue;
		}
		bit = (i & ~63ULL) + (u64)__builtin_ctzll(word);
		if (bit >= nbits)
			return false;
		{
			u64 e = bit;

			while (e < nbits) {
				if (rfs_bm_get(w, e))
					break;
				e++;
				if (max_extend && (e - bit) >= max_extend)
					break;
			}
			*seg_s = bit;
			*seg_e = e;
			return true;
		}
	}
	return false;
}

/* 找 [cand, cand+count) 全空闲且 cand % align == 0；无 → RFS_BM_NO_HIT */
static u64 find_free_run(const u64 *w, u64 nbits, u64 from, u64 count, u64 align)
{
	u64 probe = from, seg_s, seg_e;

	if (count == 0 || count > nbits || align == 0)
		return RFS_BM_NO_HIT;
	while (next_free_seg(w, nbits, probe, count + align, &seg_s, &seg_e)) {
		u64 cand = (seg_s + align - 1) & ~(align - 1);

		if (cand < seg_e && cand + count <= seg_e)
			return cand;
		probe = seg_e;
	}
	return RFS_BM_NO_HIT;
}

/* ── 组 / 片 ── */

/* ═══════════════════════════════════════════════════════════════════
 * CHUNK 后端类型（§4/§6；FULL 模式全字段惰性不建）
 * ═══════════════════════════════════════════════════════════════════ */

/* §6 片级 record 链节点（池分配）。挂片级链、不按 chunk 切分——`block_free` 每片
 * 本就只产生单段（02 §5 逐段模式），单次 free 至多每片一个节点，长 run 也只 1
 * 节点，节点数天然有界（池深）。 */
struct rfs_bm_rec {
	u64			slice_off;	/* 片内偏移视角的连续区间 [slice_off, +len) */
	u64			len;
	struct rfs_bm_rec	*next;
};

/* §6 record 节点池：per-group 预建（`kmem_cache` + 空闲链），slice_lock 内取/还
 * 零堆分配零睡眠（§10.3“锁内零分配”）。空闲链由**微型自旋锁**护（锁序：片锁 →
 * 池锁，反向永不）——比 god 的无锁 tagged LIFO 更合内核惯用法，且天然免 ABA
 * （见进度文档「与 god 差异清单」）。 */
struct rfs_bm_rec_pool {
	struct kmem_cache	*cache;	/* rec 节点缓存（预建） */
	struct rfs_bm_rec	*free_head;	/* 空闲链（复用 node->next） */
	struct rfs_bm_rec	**all;		/* 全部节点（拆池用） */
	u32			depth;
	u32			nfree;
	spinlock_t		lock;
};

/* §4 chunk 控制器（上线前即构造好，靠它做一切定位/摘要/脏标/record 凭据） */
struct rfs_bm_chunk_ctl {
	u64	disk_block_rel;	/* 本 chunk 首位图块（相对本组盘起点，4K 块） */
	u64	tail_bits;	/* 尾 chunk 有效位数；非尾 = chunk_bits（§3） */
	u8	summary;	/* 2bit 摘要（mount 扫描初始 / evict 重算） */
	u8	dirty;		/* ONLINE 且被改（TTL 按它跳过干净 chunk，§9） */
	u8	has_recs;	/* 片级链上有覆盖本 chunk 的 record（降级凭据，§6） */
};

/* 片级 backing（§4；挂在片下。mode=FULL 时 backing 指针恒 NULL） */
struct ravenfs_bm_group;	/* 前向：backing 回指整组 */

struct rfs_bm_backing {
	struct ravenfs_bm_group	*grp;	/* 回指（取 is_inode / sb / disk_start） */
	u8			mode;	/* FULL | CHUNK（cfg 拷贝） */
	u8			chunk_order;	/* chunk = 4K<<order 字节；有效 0..8 */
	u32			chunks_per_slice;
	u32			online_chunk;	/* 当前上线窗；RFS_BM_CHUNK_NONE 哨兵。变更一律持 s->lock */
	u32			gen;	/* 快路径复检（发布/下线时递增，§4） */
	u8			*slice_base;	/* buffer − tlo/8；发布时按窗重指（§4 技巧） */
	void			*buffer;	/* 固定 chunk 字节，装配期一次分配 */
	void			*snapshot;	/* 换片 evict / TTL 快照（chg_mu 下独占） */
	struct mutex		chg_mu;	/* 换片互斥：evict/load/drain/TTL 写 串行（可睡，§12） */
	struct rfs_bm_chunk_ctl	*ctls;	/* 控制器数组，chunks_per_slice 个 */
	struct rfs_bm_rec	*rec_head;	/* 片级 record 链（池节点，不拥有） */
	struct rfs_bm_rec_pool	*pool;	/* per-group 共享池（不拥有） */
};

struct ravenfs_bm_slice {
	u64		base_idx;	/* 全局位起 */
	u64		bits;		/* 本片位数 */
	u64		free_count;
	u64		hint;		/* 搜索起点：FULL = 全局位号；CHUNK = 片内偏移 */
	spinlock_t	lock;
	struct rfs_bm_backing *backing;	/* CHUNK 后端；FULL 恒 NULL */
};

struct ravenfs_bm_group {
	const char	*name;
	u64		nbits;		/* 全局位数（= 位图块数 × 32768） */
	u64		words_n;	/* u64 字数 */
	u64		*words;		/* 位图原始内存（vzalloc，页对齐） */
	u32		n_slices;
	u64		per_bits;	/* 每片位数（32768 对齐） */
	struct ravenfs_bm_slice *slices;
	u32		cpu_slots;
	u32		*preferred;	/* CPU → 上次命中片 */

	/* 片级同步：写者之间串行（周期回写 vs sync_fs）；不挡分配/释放 */
	struct mutex	wb_lock;

	/* 装载/刷回映射 */
	u64		disk_start;	/* 起始 FS_block */
	u64		disk_blocks;	/* 块数 */

	/* ── CHUNK 后端装配参数（§2；backing_mode=FULL 时全字段不消费）── */
	struct super_block *sb;		/* chunk_io 后端（sb_bread/mark_buffer_dirty） */
	u8		backing_mode;
	u8		chunk_order;		/* 有效值（setup 夹断后） */
	bool		is_inode;
	struct rfs_bm_rec_pool *pool;	/* §6 per-group 共享 record 池 */
	u32		rec_pool_depth;
	/* §7.4 自描述三段校验：block 组尾段 [lo,hi) 全 1；inode 组 root 位亮灯 */
	u64		self_check_lo;
	u64		self_check_hi;
	u64		self_check_root_bit;
};

struct ravenfs_bitmap_mgr {
	struct ravenfs_bm_group	blk;
	struct ravenfs_bm_group	ino;
};

/* ── 几何 ── */

static u64 align_up_u64(u64 x, u64 a)
{
	return (x + a - 1) & ~(a - 1);
}

/* 共享切片数：2^n ≥ ncpu ∩ 满片上限 ∩ 末片非空迭代（规格 §A.1） */
static u32 rfs_bm_slice_count(u64 blk_bits, u64 ino_bits, u32 ncpu)
{
	u32 n = 1;
	u64 cap;

	while (n < ncpu && n < (1u << 30))
		n <<= 1;
	cap = min(blk_bits, ino_bits) / RFS_BM_SLICE_MIN_BITS;
	if (!cap)
		cap = 1;
	if ((u64)n > cap)
		n = cap ? (u32)cap : 1;
	for (;;) {
		u64 per_b, per_i;

		if (n <= 1) { n = 1; break; }
		per_b = align_up_u64(DIV_ROUND_UP_ULL(blk_bits, n), RFS_BM_SLICE_MIN_BITS);
		per_i = align_up_u64(DIV_ROUND_UP_ULL(ino_bits, n), RFS_BM_SLICE_MIN_BITS);
		if ((u64)(n - 1) * per_b >= blk_bits ||
		    (u64)(n - 1) * per_i >= ino_bits) {
			n >>= 1;
			continue;
		}
		break;
	}
	return n;
}

/* ── CHUNK 组级路由（实现在文件后半 CHUNK 节；此处前向声明 —— setup/alloc/free/
 * load/sync/test 各分叉点调用） ── */
static int  rfs_bm_group_chunk_setup(struct ravenfs_bm_group *g);
static int  rfs_bm_group_chunk_load(struct ravenfs_bm_group *g);
static int  rfs_bm_group_chunk_sync(struct ravenfs_bm_group *g);
static u64  rfs_bm_group_chunk_alloc(struct ravenfs_bm_group *g, u64 count, u64 align);
static u64  rfs_bm_group_chunk_alloc_soft(struct ravenfs_bm_group *g, u64 count, u64 align, u64 *gap);
static u64  rfs_bm_group_chunk_occupy_soft(struct ravenfs_bm_group *g, u64 base, u64 count, u64 *rest);
static void rfs_bm_group_chunk_free(struct ravenfs_bm_group *g, u64 base, u64 count);
static int  rfs_bm_group_chunk_test_interval(struct ravenfs_bm_group *g, u64 base, u64 len, bool val);
static bool rfs_bm_group_chunk_test(struct ravenfs_bm_group *g, u64 bit);
static void rfs_bm_group_release(struct ravenfs_bm_group *g);

static int rfs_bm_group_setup(struct ravenfs_bm_group *g, const char *name,
			      u64 nbits, u32 n_slices, u32 cpu_slots)
{
	u32 i;

	g->name = name;
	g->nbits = nbits;
	g->words_n = DIV_ROUND_UP_ULL(nbits, 64);
	g->n_slices = n_slices;
	g->cpu_slots = cpu_slots;
	g->per_bits = align_up_u64(DIV_ROUND_UP_ULL(nbits, n_slices), RFS_BM_SLICE_MIN_BITS);

	/* FULL：位图全量常驻（现行为）；CHUNK：不驻留全图（§0 动机，省 80MB） */
	if (g->backing_mode != RFS_BM_BACKING_CHUNK) {
		g->words = vzalloc(g->words_n * sizeof(u64));
		if (!g->words)
			return -ENOMEM;
	}
	g->slices = kcalloc(n_slices, sizeof(*g->slices), GFP_KERNEL);
	if (!g->slices)
		return -ENOMEM;
	g->preferred = kcalloc(cpu_slots, sizeof(u32), GFP_KERNEL);
	if (!g->preferred)
		return -ENOMEM;

	/* D3（对齐 god ctor）：preferred 按 cpu 片号铺开 —— 首次分配即分摊，
	 * 不同 CPU 不挤同一片（kzalloc 全 0 会让所有 CPU 首分配串行撞片 0） */
	for (i = 0; i < cpu_slots; i++)
		g->preferred[i] = i % n_slices;

	for (i = 0; i < n_slices; i++) {
		struct ravenfs_bm_slice *s = &g->slices[i];
		u64 base = (u64)i * g->per_bits;

		s->base_idx = base;
		s->bits = (base >= nbits) ? 0 : min(g->per_bits, nbits - base);
		s->free_count = 0;
		/* FULL：hint = 全局位号（现行为）；CHUNK：hint = 片内偏移（§8 窗口迭代） */
		s->hint = (g->backing_mode == RFS_BM_BACKING_CHUNK) ? 0 : base;
		s->backing = NULL;
		spin_lock_init(&s->lock);
	}
	mutex_init(&g->wb_lock);

	/* CHUNK：逐片建 backing（装配期一次分配；mount 扫描另走 chunk_load） */
	if (g->backing_mode == RFS_BM_BACKING_CHUNK)
		return rfs_bm_group_chunk_setup(g);
	return 0;
}

static void rfs_bm_group_scan_free(struct ravenfs_bm_group *g)
{
	u32 i;

	for (i = 0; i < g->n_slices; i++) {
		struct ravenfs_bm_slice *s = &g->slices[i];
		u64 j, n = 0;

		for (j = 0; j < s->bits; j++)
			if (!rfs_bm_get(g->words, s->base_idx + j))
				n++;
		s->free_count = n;
	}
}

/* ── 分配 / 释放（临界区只做位运算） ── */

static u64 rfs_bm_slice_alloc(struct ravenfs_bm_group *g, struct ravenfs_bm_slice *s,
			      u64 count, u64 align)
{
	u64 hi = s->base_idx + s->bits;
	u64 lo = s->base_idx;
	u64 hit = RFS_BM_NO_HIT;

	spin_lock(&s->lock);
	if (s->free_count >= count) {
		u64 from = s->hint < lo ? lo : s->hint;

		hit = find_free_run(g->words, hi, from, count, align);
		if (hit == RFS_BM_NO_HIT)
			hit = find_free_run(g->words, from, lo, count, align);
		if (hit != RFS_BM_NO_HIT) {
			u64 j;

			for (j = 0; j < count; j++)
				rfs_bm_set(g->words, hit + j);
			s->free_count -= count;
			s->hint = hit + count;
			RFS_STAT_ADD(g->sb, bm_alloc_blocks, count);	/* 分配块总量 */
		}
	}
	spin_unlock(&s->lock);
	return hit;
}

/* 组内分配：CPU 亲和优先扫描；单片内完成（不跨片） */
static u64 rfs_bm_group_alloc(struct ravenfs_bm_group *g, u64 count, u64 align)
{
	u32 cpu = (u32)raw_smp_processor_id() % g->cpu_slots;
	u32 round;
	u64 hit;

	RFS_STAT_INC(g->sb, bm_alloc_calls);	/* 组级入口（两模式同一计数，§3.F） */

	/* §12 分叉：CHUNK 走懒加载路由（FULL 旧码原样 = 红线） */
	if (g->backing_mode == RFS_BM_BACKING_CHUNK) {
		hit = rfs_bm_group_chunk_alloc(g, count, align);
		if (hit != RFS_BM_NO_HIT)
			RFS_STAT_INC(g->sb, bm_alloc_ok);
		else
			RFS_STAT_INC(g->sb, bm_alloc_fail);
		return hit;
	}

	for (round = 0; round < g->n_slices; round++) {
		u32 idx = (g->preferred[cpu] + round) % g->n_slices;

		hit = rfs_bm_slice_alloc(g, &g->slices[idx], count, align);
		if (hit != RFS_BM_NO_HIT) {
			g->preferred[cpu] = idx;
			RFS_STAT_INC(g->sb, bm_alloc_ok);
			return hit;
		}
	}
	RFS_STAT_INC(g->sb, bm_alloc_fail);	/* 全片扫毕 = ENOSPC/碎片 */
	return RFS_BM_NO_HIT;
}

/*
 * ── D2 对齐 god（2026-09-28）：软分配 / 锚定认领 / 区间测试 ──
 * 语义 1:1 对齐 juvenile_god/bitmap_slices.cpp（alloc_soft / soft_occupy /
 * test_interval + 路由 block_alloc_soft / block_occupy_soft）；差异仅坐标系
 * （god 片内偏移，ko 全局位号，同一块内存）与锁策略（ko 阻塞，见文件头）。
 */

/*
 * 软分配（god alloc_soft）：只看**单个段**（不走段链，对齐候选被拒不换段），
 * hint 起段 → 绕回「片内最早段」再试一次；got = min(段长, count)，缺口 *gap。
 * 返回命中起始全局位号 / RFS_BM_NO_HIT。
 * count > 本片位数 → NO_HIT（god RC_ERR_STATE：路由层换片 → 全片等价失败）。
 */
static u64 rfs_bm_slice_alloc_soft(struct ravenfs_bm_group *g, struct ravenfs_bm_slice *s,
				   u64 count, u64 align, u64 *gap)
{
	u64 lo = s->base_idx, hi = lo + s->bits;
	u64 cap = max(count, align);
	u64 from = s->hint < lo ? lo : s->hint;
	u64 seg_s, seg_e, hit = RFS_BM_NO_HIT;
	u64 got, j;

	*gap = count;
	if (!count || count > s->bits || !s->free_count)
		return RFS_BM_NO_HIT;

	if (next_free_seg(g->words, hi, from, cap, &seg_s, &seg_e)) {
		u64 cand = (seg_s + align - 1) & ~(align - 1);

		if (cand < seg_e)
			hit = cand;
	}
	if (hit == RFS_BM_NO_HIT &&
	    next_free_seg(g->words, from, lo, cap, &seg_s, &seg_e)) {
		u64 cand = (seg_s + align - 1) & ~(align - 1);

		if (cand < seg_e)
			hit = cand;
	}
	if (hit == RFS_BM_NO_HIT)
		return RFS_BM_NO_HIT;

	got = seg_e - hit;
	if (got > count)
		got = count;
	for (j = 0; j < got; j++)
		rfs_bm_set(g->words, hit + j);
	s->free_count -= got;
	s->hint = hit + got;
	*gap = count - got;
	RFS_STAT_INC(g->sb, bm_soft_ok);	/* 软分配成功（D2/D3 实测面） */
	return hit;
}

/*
 * 锚定认领（god soft_occupy）：从 base 起贪心认领**连续空闲前缀**，撞第一个
 * 占用位或到额度即停；**永不越片**（cap 片内夹断，跨界续圈在路由层）。
 * 口径：*rest 入 = 已由其它途径满足量（路由恒传 0）→ real = len - *rest；
 *       出 = len - claimed（恒等式与入值无关）。
 * 返回 0；-ENOSPC = 锚点即占用（*rest 出 = len）；-EINVAL = base 出片（rest 不动）。
 * hint 只进不退：回卷游标会打烂搜索分配的空间局部性（god 纪律）。
 */
static int rfs_bm_slice_occupy(struct ravenfs_bm_group *g, struct ravenfs_bm_slice *s,
			       u64 base, u64 len, u64 *rest)
{
	u64 lo = s->base_idx, hi = lo + s->bits;
	u64 real, cap, k = 0, j;

	if (*rest > len)
		return -EINVAL;
	if (!len || *rest == len)
		return 0;			/* real == 0：无事可做（rest 不动） */
	if (base < lo || base >= hi)
		return -EINVAL;
	real = len - *rest;
	cap = min(real, hi - base);

	/* 贪心前缀（字粒度）：与 next_free_seg 段尾延伸同构、方向相反 */
	while (k < cap) {
		u64 pos = (base + k) & 63;
		u64 free_mask = ~g->words[(base + k) >> 6] >> pos;
		u64 run;

		if (!(free_mask & 1))
			break;			/* 当前位占用 → 前缀断 */
		run = (free_mask == ~0ULL) ? (64 - pos) : __builtin_ctzll(~free_mask);
		k += run;
		if (k > cap)
			k = cap;
	}
	if (!k) {
		*rest = len;
		return -ENOSPC;
	}
	for (j = 0; j < k; j++)
		rfs_bm_set(g->words, base + j);
	s->free_count -= k;
	if (base + k > s->hint)
		s->hint = base + k;		/* 只进不退 */
	*rest = len - k;
	RFS_STAT_INC(g->sb, bm_occupy);	/* 锚定认领（认领 >=1 位） */
	return 0;
}

/* 组内软分配路由（god block_alloc_soft）：preferred 起扫（**分配从 preferred 搞起**）；
 * **一片为止**（缺口回传，不跨片拼，调用方再决策）；锁忙阻塞等（ko 纪律） */
static u64 rfs_bm_group_alloc_soft(struct ravenfs_bm_group *g, u64 count, u64 align,
				   u64 *gap)
{
	u32 cpu = (u32)raw_smp_processor_id() % g->cpu_slots;
	u32 round;

	*gap = count;
	/* §12 分叉：CHUNK 走懒加载路由（FULL 旧码原样） */
	if (g->backing_mode == RFS_BM_BACKING_CHUNK)
		return rfs_bm_group_chunk_alloc_soft(g, count, align, gap);

	for (round = 0; round < g->n_slices; round++) {
		u32 idx = (g->preferred[cpu] + round) % g->n_slices;
		struct ravenfs_bm_slice *s = &g->slices[idx];
		u64 hit;

		spin_lock(&s->lock);
		hit = rfs_bm_slice_alloc_soft(g, s, count, align, gap);
		spin_unlock(&s->lock);
		if (hit != RFS_BM_NO_HIT) {
			g->preferred[cpu] = idx;
			return hit;
		}
	}
	return RFS_BM_NO_HIT;
}

/*
 * 锚定认领路由（god block_occupy_soft）：锚点片唯一 → 阻塞抢锁（不换片，
 * 纪律同 free）；片内前缀**顶到片尾才续圈**下一片（真实连续），中途撞占用即断。
 * 返回原址 base（认领 ≥1）/ PTR_INVALID；*rest = 缺口。
 */
static u64 rfs_bm_group_occupy_soft(struct ravenfs_bm_group *g, u64 base, u64 count,
				    u64 *rest)
{
	u64 nbits = g->nbits;
	u64 b = base, remain = count;

	/* §12 分叉：CHUNK 走懒加载路由（FULL 旧码原样） */
	if (g->backing_mode == RFS_BM_BACKING_CHUNK)
		return rfs_bm_group_chunk_occupy_soft(g, base, count, rest);

	while (remain && b < nbits) {
		u32 idx = (u32)(b / g->per_bits);
		struct ravenfs_bm_slice *s;
		u64 slice_end, left = 0;
		int rc;

		if (idx >= g->n_slices)
			break;			/* 越界剩余放弃（防御，同 free） */
		s = &g->slices[idx];
		spin_lock(&s->lock);
		rc = rfs_bm_slice_occupy(g, s, b, remain, &left);
		spin_unlock(&s->lock);
		if (rc)
			break;			/* 锚点即占用：前缀断，不跨 */
		b += remain - left;		/* claimed */
		remain = left;
		if (!remain)
			break;
		slice_end = min((u64)(idx + 1) * g->per_bits, nbits);
		if (b != slice_end)
			break;			/* 中途撞占用：跨界续圈无意义 */
		/* b == slice_end == 下一片 base 0 → 续圈 */
	}
	*rest = remain;
	return (count - remain) ? base : (u64)PTR_INVALID;
}

/* 区间同值测试（god test_interval 的组级版：跨片逐段；单片情形语义等价）：
 * [base, base+len) 全为 val → 0；异位 → -ENOENT；越界/溢出 → -EINVAL */
static int rfs_bm_group_test_interval(struct ravenfs_bm_group *g, u64 base, u64 len,
				      bool val)
{
	u64 b = base, end;

	/* §12 分叉：CHUNK 逐位（按需换片）测试（FULL 旧码原样） */
	if (g->backing_mode == RFS_BM_BACKING_CHUNK)
		return rfs_bm_group_chunk_test_interval(g, base, len, val);

	if (len && base + len < base)
		return -EINVAL;			/* 溢出 */
	end = base + len;
	if (end > g->nbits)
		return -EINVAL;
	while (b < end) {
		u32 idx = (u32)(b / g->per_bits);
		struct ravenfs_bm_slice *s;
		u64 slice_end, j;

		if (idx >= g->n_slices)
			return -EINVAL;
		s = &g->slices[idx];
		spin_lock(&s->lock);
		slice_end = min((u64)(idx + 1) * g->per_bits, g->nbits);
		if (slice_end > end)
			slice_end = end;
		for (j = b; j < slice_end; j++) {
			if (rfs_bm_get(g->words, j) != val) {
				spin_unlock(&s->lock);
				return -ENOENT;
			}
		}
		spin_unlock(&s->lock);
		b = slice_end;
	}
	return 0;
}

/* 释放：跨片逐段清位（重复释放不虚高） */
static void rfs_bm_group_free(struct ravenfs_bm_group *g, u64 base, u64 count)
{
	u64 i = base;

	RFS_STAT_INC(g->sb, bm_free_calls);	/* 组级入口（两模式同一计数） */

	/* §12 分叉：CHUNK 走懒加载路由（在线直改/离线挂 record/池空兜底） */
	if (g->backing_mode == RFS_BM_BACKING_CHUNK) {
		rfs_bm_group_chunk_free(g, base, count);
		return;
	}

	while (count) {
		u32 idx = (u32)(i / g->per_bits);
		struct ravenfs_bm_slice *s;
		u64 hi, n, j;

		if (idx >= g->n_slices)
			break;			/* 越界剩余放弃（防御） */
		s = &g->slices[idx];
		hi = s->base_idx + s->bits;
		if (i >= hi) {			/* 落入片间空洞 → 前进到下一片 */
			i = ((u64)idx + 1) * g->per_bits;
			continue;
		}
		n = min(count, hi - i);
		spin_lock(&s->lock);
		for (j = 0; j < n; j++) {
			if (rfs_bm_get(g->words, i + j)) {
				rfs_bm_clear(g->words, i + j);
				s->free_count++;
				RFS_STAT_INC(g->sb, bm_free_blocks);	/* 仅 1→0 实清计 */
			}
		}
		spin_unlock(&s->lock);
		i += n;
		count -= n;
	}
}

/* ═══════════════════════════════════════════════════════════════════
 * CHUNK 后端实现（§12 移植；2026-09-28）
 *
 * 锁纪律（§10.3 铁律）：`slice_lock` 只护位操作/在线窗/record 链（短临界，零
 * I/O 零睡眠）；一切盘 I/O 与换片串行在片 `chg_mu`（可睡 mutex）下，**盘 I/O
 * 永不进 slice_lock**。锁序全局唯一：**chg_mu → slice_lock → 池锁**（反向永不）。
 * ko 纪律：alloc 阻塞 spinlock（2026-09-28 用户裁，勿改 try 换片）——快路径
 * 直接 spin_lock 复检在线窗；不在线则放锁走慢路径换片再重试。
 * ═══════════════════════════════════════════════════════════════════ */

/* §3 索引合成：chunk_bits = 32768<<order，位移 15+order；尾窗夹到片尾
 * （tail 恒 ≥1 整页 → 不存在 sub-page 尾块）。wlo/whi 为**片内偏移**。 */
static void rfs_bm_ctl_window(struct ravenfs_bm_slice *s, u32 ctl, u64 *wlo, u64 *whi)
{
	u64 lo = (u64)ctl << (15 + s->backing->chunk_order);
	u64 hi = lo + ((u64)32768 << s->backing->chunk_order);

	if (hi > s->bits)
		hi = s->bits;
	*wlo = lo;
	*whi = hi;
}

/* §7 摘要重算（须持 slice_lock）：链上有待还 record → 恒 MIXED（§10.5 防错误
 * ENOSPC）；src = buffer（evict）或 snapshot（TTL 态2 写回后），下标从窗首起。 */
static void rfs_bm_recompute_summary(struct ravenfs_bm_slice *s, u32 ctl, const void *src)
{
	struct rfs_bm_chunk_ctl *c = &s->backing->ctls[ctl];
	u64 wlo, whi, ones = 0, w, nbits;

	if (c->has_recs) {
		c->summary = RFS_BM_SUM_MIXED;
		return;
	}
	rfs_bm_ctl_window(s, ctl, &wlo, &whi);
	nbits = whi - wlo;
	for (w = 0; w < (nbits >> 6); w++)
		ones += (u64)hweight64(((const u64 *)src)[w]);
	c->summary = (ones == nbits) ? RFS_BM_SUM_ALL_USED :
		    (ones == 0)      ? RFS_BM_SUM_ALL_FREE :
				       RFS_BM_SUM_MIXED;
}

/* ── §6 record 节点池（kmem_cache 预建；锁内取/还零堆分配零睡眠） ── */
static struct rfs_bm_rec *rfs_bm_pool_acquire(struct rfs_bm_rec_pool *p)
{
	struct rfs_bm_rec *n;

	if (!p)
		return NULL;
	spin_lock(&p->lock);
	n = p->free_head;
	if (n) {
		p->free_head = n->next;
		p->nfree--;
	}
	spin_unlock(&p->lock);
	return n;
}

static void rfs_bm_pool_release(struct rfs_bm_rec_pool *p, struct rfs_bm_rec *n)
{
	if (!p || !n)
		return;
	spin_lock(&p->lock);
	n->next = p->free_head;
	p->free_head = n;
	p->nfree++;
	spin_unlock(&p->lock);
}

static struct rfs_bm_rec_pool *rfs_bm_pool_create(u32 depth, bool is_inode)
{
	struct rfs_bm_rec_pool *p;
	u32 i;

	if (!depth)
		depth = RFS_BM_REC_POOL_DEPTH;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return NULL;
	spin_lock_init(&p->lock);
	p->depth = depth;
	p->cache = kmem_cache_create(is_inode ? "ravenfs_bm_rec_ino" : "ravenfs_bm_rec_blk",
				     sizeof(struct rfs_bm_rec), 0, 0, NULL);
	if (!p->cache) {
		kfree(p);
		return NULL;
	}
	p->all = kvmalloc_array(depth, sizeof(struct rfs_bm_rec *), GFP_KERNEL);
	if (!p->all) {
		kmem_cache_destroy(p->cache);
		kfree(p);
		return NULL;
	}
	for (i = 0; i < depth; i++) {
		struct rfs_bm_rec *n = kmem_cache_alloc(p->cache, GFP_KERNEL);

		if (!n)
			goto fail;
		p->all[i] = n;
		n->next = p->free_head;
		p->free_head = n;
		p->nfree++;
	}
	return p;
fail:
	while (i > 0) {
		i--;
		kmem_cache_free(p->cache, p->all[i]);
	}
	kvfree(p->all);
	kmem_cache_destroy(p->cache);
	kfree(p);
	return NULL;
}

static void rfs_bm_pool_destroy(struct rfs_bm_rec_pool *p)
{
	u32 i;

	if (!p)
		return;
	if (p->all) {
		for (i = 0; i < p->depth; i++)
			if (p->all[i])
				kmem_cache_free(p->cache, p->all[i]);
		kvfree(p->all);
	}
	if (p->cache)
		kmem_cache_destroy(p->cache);
	kfree(p);
}

/* ── chunk 盘 I/O（§12：sb_bread/mark_buffer_dirty；disk_block_rel 经
 * ravenfs_dev_blk 换算 FS block 号）。调用方须持该片 chg_mu、零进 slice_lock。 ── */
static int rfs_bm_chunk_read(struct ravenfs_bm_group *g, u64 rel, void *buf, u64 nblocks)
{
	struct ravenfs_sb_info *sbi = g->sb->s_fs_info;
	u64 b;

	for (b = 0; b < nblocks; b++) {
		struct buffer_head *bh = sb_bread(g->sb,
			ravenfs_dev_blk(sbi, g->disk_start + rel + b));

		if (!bh) {
			RFS_STAT_INC(g->sb, err_meta_io);
			return -EIO;
		}
		memcpy((u8 *)buf + b * RAVENFS_BLOCK_SIZE, bh->b_data,
		       RAVENFS_BLOCK_SIZE);
		brelse(bh);
		RFS_STAT_INC(g->sb, io_read_ops);
		RFS_STAT_ADD(g->sb, io_read_bytes, RAVENFS_BLOCK_SIZE);
	}
	return 0;
}

static int rfs_bm_chunk_write(struct ravenfs_bm_group *g, u64 rel, const void *buf,
			      u64 nblocks)
{
	struct ravenfs_sb_info *sbi = g->sb->s_fs_info;
	u64 b;

	for (b = 0; b < nblocks; b++) {
		struct buffer_head *bh = sb_bread(g->sb,
			ravenfs_dev_blk(sbi, g->disk_start + rel + b));

		if (!bh) {
			RFS_STAT_INC(g->sb, err_meta_io);
			return -EIO;
		}
		memcpy(bh->b_data, (const u8 *)buf + b * RAVENFS_BLOCK_SIZE,
		       RAVENFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);	/* 确定性：返回即落盘（对齐 FULL 同步口径） */
		brelse(bh);
		RFS_STAT_INC(g->sb, io_write_ops);
		RFS_STAT_ADD(g->sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
	}
	return 0;
}

/* §2 有效 chunk_order：夹到「最小片字节数 ≥ chunk」（保可用性，可靠性优先） */
static u8 rfs_bm_fit_chunk_order(u8 order, u64 per_bytes)
{
	u8 o = (order > RFS_BM_CHUNK_ORDER_MAX) ? RFS_BM_CHUNK_ORDER_MAX : order;

	while (o > 0 && (4096ULL << o) > per_bytes)
		o--;
	return o;
}

/* ★ §5/§6 record 还账核心（须持 slice_lock；零 I/O）：链 ∩ 本窗 1→0 清位；
 * 整段覆盖 → 摘节点、单侧伸出 → 收缩留余、两侧都伸 → 尽力劈分（池空时保留
 * 整节点：清位已做且幂等，下次上线再缩/摘 → 不丢，§10.7）。target_words 下标
 * = slice_off − wlo（发布时 = buffer；TTL 态2 = snapshot，buffer 归线上窗所有）。
 * free_count 已在挂 record 时记账，此处不动。返回本窗实际 1→0 清位数（>0 ⇒
 * 发布须置脏，否则已还账的 free 在 evict 按 dirty==0 跳过 = 永不落盘，§10.6）。 */
static u64 rfs_bm_drain_window(struct ravenfs_bm_slice *s, u32 ctl, u64 *target_words)
{
	struct rfs_bm_backing *bk = s->backing;
	u64 wlo, whi, cleared = 0;
	struct rfs_bm_rec **pp = &bk->rec_head;
	bool kept = false;

	rfs_bm_ctl_window(s, ctl, &wlo, &whi);
	while (*pp) {
		struct rfs_bm_rec *n = *pp;
		u64 ns = n->slice_off, ne = n->slice_off + n->len, is, ie, b;

		if (ne <= wlo || ns >= whi) {
			pp = &n->next;
			continue;
		}
		is = (ns > wlo) ? ns : wlo;
		ie = (ne < whi) ? ne : whi;
		for (b = is; b < ie; b++)
			if (rfs_bm_get(target_words, b - wlo)) {
				rfs_bm_clear(target_words, b - wlo);
				cleared++;
			}
		if (ns >= wlo && ne <= whi) {	/* 整段覆盖 → 摘节点 */
			*pp = n->next;
			rfs_bm_pool_release(bk->pool, n);
			RFS_STAT_INC(bk->grp->sb, bm_rec_drain);	/* drain 消费量 */
			continue;		/* pp 已指向后继，不推进 */
		}
		if (ns >= wlo) {		/* 尾伸窗外 → 收缩留尾 */
			n->slice_off = whi;
			n->len = ne - whi;
			pp = &n->next;
		} else if (ne <= whi) {		/* 头伸窗外 → 收缩留头 */
			n->len = wlo - ns;
			pp = &n->next;
		} else {				/* 两侧伸窗外 → 劈分（池空则保留整节点） */
			struct rfs_bm_rec *tail = rfs_bm_pool_acquire(bk->pool);

			if (tail) {
				tail->slice_off = whi;
				tail->len = ne - whi;
				tail->next = n->next;
				n->len = wlo - ns;	/* 头余 [ns, wlo) */
				n->next = tail;
				pp = &tail->next;
			} else {
				kept = true;
				pp = &n->next;
			}
		}
	}
	bk->ctls[ctl].has_recs = kept ? 1 : 0;
	RFS_STAT_ADD(bk->grp->sb, bm_free_blocks, cleared);	/* drain 实清（1→0） */
	return cleared;
}

/* 在线窗直清 [from,to)（须持 slice_lock 且窗在线）：按实际 1→0 计入 free_count
 *（同 FULL 口径）+ 置 dirty（TTL 按脏回写，§9）。from/to 为片内偏移。 */
static void rfs_bm_clear_bits_locked(struct ravenfs_bm_slice *s, u64 from, u64 to)
{
	u64 *words = (u64 *)s->backing->slice_base;
	u64 cleared = 0, b;

	for (b = from; b < to; b++)
		if (rfs_bm_get(words, b)) {
			rfs_bm_clear(words, b);
			cleared++;
		}
	s->free_count += cleared;
	RFS_STAT_ADD(s->backing->grp->sb, bm_free_blocks, cleared);	/* 仅 1→0 实清计 */
	s->backing->ctls[from >> (15 + s->backing->chunk_order)].dirty = 1;
}

/* ══ §5 慢路径：换片状态机（盘 I/O 永不进 slice_lock）══
 * 全序列串行在 chg_mu 下：下线旧窗（锁内快照+清 dirty，online→NONE）→ evict 写
 *（锁外）→ 读目标窗（锁外）→ ★ 发布前先 drain record（§5/§6 顺序铁律）→ 发布。
 * 下线置 NONE：否则快路径在快照后继续改 buffer，随后的 read 覆盖 = 双分配方向。
 * 写失败 → 恢复旧窗在线 + 复脏（buffer 仍是真相，状态零损坏；失败方向 = 泄漏）。 */
static int rfs_bm_chunk_ensure_online(struct ravenfs_bm_slice *s, u32 ctl)
{
	struct rfs_bm_backing *bk = s->backing;
	u64 tlo, thi;
	u64 t0;
	u32 old;
	bool need_write = false;

	if (!bk || ctl >= bk->chunks_per_slice)
		return -EINVAL;
	t0 = ktime_get_ns();	/* bm_chunk_switch_ns：换片全程计时（§3.F） */
	mutex_lock(&bk->chg_mu);
	rfs_bm_ctl_window(s, ctl, &tlo, &thi);

	spin_lock(&s->lock);
	if (bk->online_chunk == ctl) {		/* 复检：别人已换上 ✓ */
		spin_unlock(&s->lock);
		mutex_unlock(&bk->chg_mu);
		return 0;
	}
	old = bk->online_chunk;
	if (old != RFS_BM_CHUNK_NONE) {
		struct rfs_bm_chunk_ctl *oc = &bk->ctls[old];

		if (oc->dirty) {
			u64 olo, ohi;

			rfs_bm_ctl_window(s, old, &olo, &ohi);
			memcpy(bk->snapshot, bk->buffer, (ohi - olo) / 8);
			oc->dirty = 0;
			need_write = true;
		}
		bk->online_chunk = RFS_BM_CHUNK_NONE;	/* 下线：buffer 冻结 */
		bk->gen++;
	}
	spin_unlock(&s->lock);

	if (need_write) {				/* evict 写（锁外、持 chg_mu） */
		u64 olo, ohi;

		rfs_bm_ctl_window(s, old, &olo, &ohi);
		if (rfs_bm_chunk_write(bk->grp, bk->ctls[old].disk_block_rel,
				       bk->snapshot, (ohi - olo) / 32768)) {
			spin_lock(&s->lock);
			if (bk->online_chunk == RFS_BM_CHUNK_NONE) {
				bk->online_chunk = old;
				bk->gen++;
				bk->ctls[old].dirty = 1;
				rfs_bm_recompute_summary(s, old, bk->buffer);
			}
			spin_unlock(&s->lock);
			mutex_unlock(&bk->chg_mu);
			return -EIO;
		}
		spin_lock(&s->lock);
		rfs_bm_recompute_summary(s, old, bk->buffer);	/* evict 时从 buffer 重算（§7） */
		spin_unlock(&s->lock);
	}

	/* 目标窗读入（锁外、持 chg_mu）；失败 → 旧窗已安全下线，保持 NONE 可重试。
	 * §7 摘要 ALL_FREE 且无 pending record → 盘态全 0，免读直置空窗；否则真读。 */
	if (bk->ctls[ctl].summary == RFS_BM_SUM_ALL_FREE && !bk->ctls[ctl].has_recs) {
		memset(bk->buffer, 0, (thi - tlo) / 8);
	} else if (rfs_bm_chunk_read(bk->grp, bk->ctls[ctl].disk_block_rel,
				    bk->buffer, (thi - tlo) / 32768)) {
		mutex_unlock(&bk->chg_mu);
		return -EIO;
	}

	spin_lock(&s->lock);
	{
		u64 drained = rfs_bm_drain_window(s, ctl, (u64 *)bk->buffer);

		bk->ctls[ctl].dirty = drained ? 1 : 0;
		bk->online_chunk = ctl;
		bk->gen++;
		bk->slice_base = (u8 *)bk->buffer - tlo / 8;	/* §4 重指 */
		RFS_STAT_INC(bk->grp->sb, bm_chunk_switch);	/* 换片频率 */
		RFS_STAT_ADD(bk->grp->sb, bm_chunk_switch_ns, ktime_get_ns() - t0);
	}
	spin_unlock(&s->lock);
	mutex_unlock(&bk->chg_mu);
	return 0;
}

/* 快/慢路径外壳（§5）：每窗一次调用。返回 RFS_BM_WIN_LOCKED = 已持 s->lock 且窗
 * 在线（调用方用完解锁）；RFS_BM_WIN_SKIP = 本窗暂不可用（摘要免载入 / 换片反复
 * 被抢）→ 路由跳过本窗；负值 = I/O 失败。 */
static int rfs_bm_chunk_lock_window(struct ravenfs_bm_slice *s, u32 ctl)
{
	struct rfs_bm_backing *bk = s->backing;
	int attempt;

	if (!bk || ctl >= bk->chunks_per_slice)
		return -EINVAL;
	for (attempt = 0; attempt < 8; attempt++) {
		spin_lock(&s->lock);
		if (bk->online_chunk == ctl)
			return RFS_BM_WIN_LOCKED;	/* 快路径 HIT（纯内存） */
		if (bk->ctls[ctl].summary == RFS_BM_SUM_ALL_USED &&
		    !bk->ctls[ctl].has_recs) {
			spin_unlock(&s->lock);
			RFS_STAT_INC(bk->grp->sb, bm_summary_skip);	/* 摘要免载入跳过 */
			return RFS_BM_WIN_SKIP;		/* 摘要免载入跳过（§7/§10.5） */
		}
		spin_unlock(&s->lock);
		if (rfs_bm_chunk_ensure_online(s, ctl))
			return -EIO;			/* 慢路径换片失败 */
	}
	return RFS_BM_WIN_SKIP;			/* 反复被抢 → 本窗跳过 */
}

/* ══ §8 单窗原语（调用方已持 slice_lock 且窗在线）：口径与 FULL 版完全一致，
 * 仅扫描区间夹到本窗。from/to 为片内偏移；*out 为全局位号。 ══ */
static bool rfs_bm_win_alloc_hard(struct ravenfs_bm_slice *s, u64 from, u64 to,
				  u64 count, u64 align, u64 *out)
{
	u64 *words = (u64 *)s->backing->slice_base;
	u64 hit, j;

	if (from >= to || count > to - from)
		return false;			/* 窗内放不下 */
	hit = find_free_run(words, to, from, count, align);
	if (hit == RFS_BM_NO_HIT)
		return false;
	for (j = 0; j < count; j++)
		rfs_bm_set(words, hit + j);
	s->free_count -= count;
	s->hint = hit + count;			/* 片内偏移（CHUNK 约定） */
	*out = s->base_idx + hit;		/* 全局位号（对外零换算） */
	RFS_STAT_ADD(s->backing->grp->sb, bm_alloc_blocks, count);	/* 分配块总量 */
	return true;
}

static bool rfs_bm_win_alloc_soft(struct ravenfs_bm_slice *s, u64 from, u64 to,
				  u64 count, u64 align, u64 *out, u64 *left)
{
	u64 *words = (u64 *)s->backing->slice_base;
	u64 cap = max(count, align), seg_s, seg_e, hit = RFS_BM_NO_HIT, got, j;

	*left = count;
	if (from >= to)
		return false;
	if (next_free_seg(words, to, from, cap, &seg_s, &seg_e)) {
		u64 cand = (seg_s + align - 1) & ~(align - 1);

		if (cand < seg_e)
			hit = cand;
	}
	if (hit == RFS_BM_NO_HIT)
		return false;
	got = seg_e - hit;
	if (got > count)
		got = count;
	for (j = 0; j < got; j++)
		rfs_bm_set(words, hit + j);
	s->free_count -= got;
	s->hint = hit + got;
	*out = s->base_idx + hit;
	*left = count - got;
	RFS_STAT_INC(s->backing->grp->sb, bm_soft_ok);	/* 软分配成功 */
	return true;
}

/* 贪心前缀（镜像 FULL soft_occupy，差异仅 cap 另受窗尾 whi 夹断）：口径不变 =
 * *rest 入=已满足量、出=len−claimed 总缺口；撞占用停、窗尾如实回传。 */
static int rfs_bm_win_soft_occupy(struct ravenfs_bm_slice *s, u64 wlo, u64 whi,
				  u64 base, u64 len, u64 *rest)
{
	u64 *words = (u64 *)s->backing->slice_base;
	u64 off, bound, cap, k = 0, real, nwords, j;

	if (*rest > len)
		return -EINVAL;
	if (len == 0)
		return 0;
	real = len - *rest;
	if (real == 0)
		return 0;
	if (base < s->base_idx)
		return -EINVAL;
	off = base - s->base_idx;
	if (off < wlo || off >= whi || off >= s->bits)
		return -EINVAL;			/* 窗口守卫（§4） */
	bound = min(whi, s->bits);
	cap = min(real, bound - off);
	if (!cap)
		return -ENOSPC;
	nwords = DIV_ROUND_UP(s->bits, 64);
	while (k < cap) {
		u64 wi = (off + k) >> 6, eword, pos, free_mask, run;

		if (wi >= nwords)
			break;
		eword = ~words[wi];
		if (wi == nwords - 1 && (s->bits & 63))
			eword &= (1ULL << (s->bits & 63)) - 1;
		pos = (off + k) & 63;
		free_mask = eword >> pos;
		if (!(free_mask & 1))
			break;			/* 当前位占用 → 前缀断 */
		run = (free_mask == ~0ULL) ? (64 - pos) : (u64)__builtin_ctzll(~free_mask);
		k += run;
		if (k > cap)
			k = cap;
	}
	if (!k) {
		*rest = len;
		return -ENOSPC;
	}
	for (j = 0; j < k; j++)
		rfs_bm_set(words, off + j);
	s->free_count -= k;
	if (off + k > s->hint)
		s->hint = off + k;		/* 只进不退 */
	*rest = len - k;
	RFS_STAT_INC(s->backing->grp->sb, bm_occupy);	/* 锚定认领 */
	return 0;
}

/* ══ §8 路由层窗口迭代器（alloc/occupy 外壳；每窗 = 快/慢路径 + 单窗原语）═══ */

/* 硬分配：hint 窗起顺序向片尾；扫到片尾未中 → 绕回 [0,hint) 窗序列再来一遍
 *（02 §4 绕回语义不变）。返回 0/命中、-ENOSPC（全片扫毕含绕回）/ -EIO。 */
static int rfs_bm_chunk_slice_alloc_hard(struct ravenfs_bm_slice *s, u64 align,
					 u64 count, u64 *out)
{
	struct rfs_bm_backing *bk = s->backing;
	u64 hint;
	u32 nwin, sh, hctl, c;

	if (!bk)
		return -EINVAL;
	if (count == 0 || count > s->bits || count > s->free_count || !s->free_count)
		return -ENOSPC;
	if (!align)
		align = 1;
	spin_lock(&s->lock);
	hint = s->hint;
	spin_unlock(&s->lock);

	nwin = bk->chunks_per_slice;
	sh = 15 + bk->chunk_order;
	hctl = (hint >= ((u64)nwin << sh)) ? nwin - 1 : (u32)(hint >> sh);

	for (c = hctl; c < nwin; c++) {		/* 正向：hint 窗 → 片尾 */
		u64 wlo, whi, from;
		int rc;

		rfs_bm_ctl_window(s, c, &wlo, &whi);
		from = (c == hctl && hint > wlo) ? hint : wlo;
		if (from >= whi)
			continue;
		rc = rfs_bm_chunk_lock_window(s, c);
		if (rc == RFS_BM_WIN_SKIP)
			continue;
		if (rc != RFS_BM_WIN_LOCKED)
			return -EIO;
		if (rfs_bm_win_alloc_hard(s, from, whi, count, align, out)) {
			bk->ctls[c].dirty = 1;	/* 置位同样是变更（§4/§9，丢 = 双分配） */
			spin_unlock(&s->lock);
			return 0;
		}
		spin_unlock(&s->lock);
	}
	for (c = 0; c <= hctl; c++) {		/* 绕回：[0, hint) 窗序列 */
		u64 wlo, whi, to;
		int rc;

		rfs_bm_ctl_window(s, c, &wlo, &whi);
		to = (c == hctl) ? min(hint, whi) : whi;
		if (wlo >= to)
			continue;
		rc = rfs_bm_chunk_lock_window(s, c);
		if (rc == RFS_BM_WIN_SKIP)
			continue;
		if (rc != RFS_BM_WIN_LOCKED)
			return -EIO;
		if (rfs_bm_win_alloc_hard(s, wlo, to, count, align, out)) {
			bk->ctls[c].dirty = 1;
			spin_unlock(&s->lock);
			return 0;
		}
		spin_unlock(&s->lock);
	}
	return -ENOSPC;				/* 全片扫毕（含绕回）= SLICE_EMPTY */
}

/* 软分配：同硬分配的两趟窗序列（02 §4：正向首段命中即中，绕回补 [0,hint)） */
static int rfs_bm_chunk_slice_alloc_soft(struct ravenfs_bm_slice *s, u64 align,
					 u64 count, u64 *out, u64 *left)
{
	struct rfs_bm_backing *bk = s->backing;
	u64 hint;
	u32 nwin, sh, hctl, c;

	*left = count;
	if (!bk)
		return -EINVAL;
	if (count == 0)
		return -EINVAL;
	if (!s->free_count)
		return -ENOSPC;
	if (!align)
		align = 1;
	spin_lock(&s->lock);
	hint = s->hint;
	spin_unlock(&s->lock);

	nwin = bk->chunks_per_slice;
	sh = 15 + bk->chunk_order;
	hctl = (hint >= ((u64)nwin << sh)) ? nwin - 1 : (u32)(hint >> sh);

	for (c = hctl; c < nwin; c++) {
		u64 wlo, whi, from;
		int rc;

		rfs_bm_ctl_window(s, c, &wlo, &whi);
		from = (c == hctl && hint > wlo) ? hint : wlo;
		if (from >= whi)
			continue;
		rc = rfs_bm_chunk_lock_window(s, c);
		if (rc == RFS_BM_WIN_SKIP)
			continue;
		if (rc != RFS_BM_WIN_LOCKED)
			return -EIO;
		if (rfs_bm_win_alloc_soft(s, from, whi, count, align, out, left)) {
			bk->ctls[c].dirty = 1;
			spin_unlock(&s->lock);
			return 0;
		}
		spin_unlock(&s->lock);
	}
	for (c = 0; c <= hctl; c++) {
		u64 wlo, whi, to;
		int rc;

		rfs_bm_ctl_window(s, c, &wlo, &whi);
		to = (c == hctl) ? min(hint, whi) : whi;
		if (wlo >= to)
			continue;
		rc = rfs_bm_chunk_lock_window(s, c);
		if (rc == RFS_BM_WIN_SKIP)
			continue;
		if (rc != RFS_BM_WIN_LOCKED)
			return -EIO;
		if (rfs_bm_win_alloc_soft(s, wlo, to, count, align, out, left)) {
			bk->ctls[c].dirty = 1;
			spin_unlock(&s->lock);
			return 0;
		}
		spin_unlock(&s->lock);
	}
	return -ENOSPC;
}

/* 锚定占领窗口迭代器（§8 occupy 路由）：锚点窗起逐窗贪心前缀，顶到窗尾换下一
 * 窗（跨 chunk 续圈 = 原跨片续圈同构）；撞占用（含摘要 ALL_USED）如实停手。
 * *rest 口径与 FULL soft_occupy 完全一致（出 = len − 本链累计认领）。 */
static int rfs_bm_chunk_slice_occupy(struct ravenfs_bm_slice *s, u64 base, u64 len,
				     u64 *rest)
{
	struct rfs_bm_backing *bk = s->backing;
	u64 start, rest0, gap, anchor;
	u32 sh, c;

	if (!bk)
		return -EINVAL;
	if (*rest > len)
		return -EINVAL;
	if (base < s->base_idx)
		return -EINVAL;
	if (len == 0 || *rest == len)
		return 0;
	start = base - s->base_idx;
	if (start >= s->bits)
		return -EINVAL;
	sh = 15 + bk->chunk_order;
	rest0 = *rest;
	c = (u32)(start >> sh);			/* 锚点窗起 */
	anchor = start;
	gap = len - rest0;
	while (c < bk->chunks_per_slice) {
		u64 wlo, whi, wr = 0;
		int rc;

		rfs_bm_ctl_window(s, c, &wlo, &whi);
		rc = rfs_bm_chunk_lock_window(s, c);
		if (rc == RFS_BM_WIN_SKIP)
			break;			/* 摘要 ALL_USED：锚点窗无货 → 前缀断 */
		if (rc != RFS_BM_WIN_LOCKED)
			return -EIO;
		rc = rfs_bm_win_soft_occupy(s, wlo, whi, s->base_idx + anchor, gap, &wr);
		if (rc != -EINVAL && gap > wr)	/* 认领了位 = 变更（§4/§9 dirty） */
			bk->ctls[c].dirty = 1;
		spin_unlock(&s->lock);
		if (rc == -EINVAL)
			return rc;
		{
			u64 claimed = gap - wr;

			gap = wr;
			*rest = rest0 + gap;	/* FULL 恒等式：出 = len − 累计认领 */
			if (!claimed)
				break;		/* 锚点即占用：前缀断 */
			if (!gap)
				return 0;	/* 全额认领，收工 */
			anchor += claimed;
			if (anchor < whi)
				return 0;	/* 撞占用停：不跨窗硬续 */
		}
		c++;				/* 顶到窗尾 → 下一窗续圈 */
	}
	return 0;				/* rest 如实（片尾/占用断） */
}

/* §6 区间释放：在线直改（dirty=1，零 record 零 I/O）/ 离线挂 record（零 I/O、
 * O(1)）/ 池空兜底强制上线直清（零第二批记录）。自管锁（兜底须解锁走慢路径），
 * 纪律同 FULL：阻塞抢锁、释放不可丢。base/len 为全局位号；区间须整落本片。 */
static void rfs_bm_chunk_slice_free(struct ravenfs_bm_slice *s, u64 base, u64 len)
{
	struct rfs_bm_backing *bk = s->backing;
	u64 pos, remaining;
	u32 sh;
	int guard = 0;

	if (!bk || base < s->base_idx)
		return;
	pos = base - s->base_idx;
	if (pos + len > s->bits || len == 0)
		return;
	sh = 15 + bk->chunk_order;
	remaining = len;
	while (remaining) {
		u32 c = (u32)(pos >> sh);
		u64 wlo, whi, nwin;

		rfs_bm_ctl_window(s, c, &wlo, &whi);
		nwin = (pos + remaining <= whi) ? remaining : whi - pos;

		spin_lock(&s->lock);
		if (bk->online_chunk == c) {
			rfs_bm_clear_bits_locked(s, pos, pos + nwin);
			spin_unlock(&s->lock);
			pos += nwin;
			remaining -= nwin;
			guard = 0;
			continue;
		}
		{
			u32 on = bk->online_chunk;
			u64 span_limit = s->bits, n;
			struct rfs_bm_rec *node;

			if (on != RFS_BM_CHUNK_NONE) {
				u64 olo, ohi;

				rfs_bm_ctl_window(s, on, &olo, &ohi);
				if (pos < ohi && pos + remaining > olo)
					span_limit = olo;
			}
			n = (pos + remaining <= span_limit) ? remaining
							    : span_limit - pos;
			node = rfs_bm_pool_acquire(bk->pool);
			if (node) {
				u32 c_last = (u32)((pos + n - 1) >> sh), t;

				node->slice_off = pos;
				node->len = n;
				node->next = bk->rec_head;
				bk->rec_head = node;
				RFS_STAT_INC(bk->grp->sb, bm_rec_attach);	/* 离线还账挂账 */
				/* 摘要降级（§6）：覆盖到的 ctl 一律 MIXED，
				 * 否则盘态 ALL_USED（其实有待还空闲）被免载入
				 * 跳过 → 错误 ENOSPC（§10.5） */
				for (t = c; t <= c_last; t++) {
					bk->ctls[t].has_recs = 1;
					bk->ctls[t].summary = RFS_BM_SUM_MIXED;
				}
				s->free_count += n;	/* 挂账时记账；drain 不再动 */
				spin_unlock(&s->lock);
				pos += n;
				remaining -= n;
				guard = 0;
				continue;
			}
		}
		RFS_STAT_INC(bk->grp->sb, bm_pool_empty);	/* 池空兜底（撞池深上限） */
		/* 池空兜底（§6/§10.9）：解锁 → 慢路径强制上线 → 重锁 → 直清；
		 * 此时盘态仍显示已分配（free 未记账），alloc 抢不走它 ⇒ 不越序。 */
		spin_unlock(&s->lock);
		if (rfs_bm_chunk_ensure_online(s, c)) {
			pos += nwin;		/* I/O 失败：泄漏方向（§1.5，fsck 可收） */
			remaining -= nwin;
			guard = 0;
			continue;
		}
		spin_lock(&s->lock);
		if (bk->online_chunk != c) {	/* 竞态被换片 → 重走本段 */
			spin_unlock(&s->lock);
			if (++guard > 64) {
				pos += nwin;
				remaining -= nwin;
				guard = 0;
			}
			continue;
		}
		rfs_bm_clear_bits_locked(s, pos, pos + nwin);
		spin_unlock(&s->lock);
		pos += nwin;
		remaining -= nwin;
		guard = 0;
	}
}

/* block_test / inode_test 的 CHUNK 版（§1.1 语义不变）：在线窗直读 / record
 * 覆盖 = 逻辑空闲 / 摘要裁决（ALL_USED/ALL_FREE 免载入）/ MIXED 离线 → 上线查；
 * I/O 失败保守按已用（泄漏方向，绝不误报 free）。base 为全局位号。 */
static bool rfs_bm_chunk_slice_test(struct ravenfs_bm_slice *s, u64 base)
{
	struct rfs_bm_backing *bk = s->backing;
	u64 off;
	u32 c;

	if (!bk || base < s->base_idx)
		return false;
	off = base - s->base_idx;
	if (off >= s->bits)
		return false;
	c = (u32)(off >> (15 + bk->chunk_order));

	spin_lock(&s->lock);
	if (bk->online_chunk == c) {		/* 在线：直读 buffer */
		bool v = rfs_bm_get((const u64 *)bk->slice_base, off);

		spin_unlock(&s->lock);
		return v;
	}
	{
		struct rfs_bm_rec *n;

		for (n = bk->rec_head; n; n = n->next)	/* 离线：record 覆盖 = 逻辑空闲 */
			if (off >= n->slice_off && off < n->slice_off + n->len) {
				spin_unlock(&s->lock);
				return false;
			}
	}
	{
		u8 sum = bk->ctls[c].summary;		/* 摘要裁决（免载入） */

		spin_unlock(&s->lock);
		if (sum == RFS_BM_SUM_ALL_USED) {
			RFS_STAT_INC(bk->grp->sb, bm_summary_skip);	/* 摘要裁决免载入 */
			return true;
		}
		if (sum == RFS_BM_SUM_ALL_FREE) {
			RFS_STAT_INC(bk->grp->sb, bm_summary_skip);	/* 摘要裁决免载入 */
			return false;
		}
		if (rfs_bm_chunk_ensure_online(s, c))
			return true;			/* MIXED 上线查；I/O 失败 → 保守按已用 */
		spin_lock(&s->lock);
		{
			bool v = (bk->online_chunk == c)
					 ? rfs_bm_get((const u64 *)bk->slice_base, off)
					 : true;

			spin_unlock(&s->lock);
			return v;
		}
	}
}

/* §7 mount 扫描单窗：chunk_io 顺序读入 → 一趟吃入 free_count + 2bit 摘要 +
 * 自描述三段校验（§7.4）内联同趟做掉，失败拒挂。构造期单线程；持 chg_mu 仅为
 * §10.3「I/O 只在持 chg_mu 下」字面成立（FULL ctor 零 I/O 不进本函数）。 */
static int rfs_bm_chunk_mount_scan(struct ravenfs_bm_slice *s, u32 ctl)
{
	struct rfs_bm_backing *bk = s->backing;
	struct ravenfs_bm_group *g;
	u64 wlo, whi, nbits, nblocks, w, ones, zeros, gwlo, gwhi;
	u64 t0;
	const u64 *words;
	int rc = 0;

	if (!bk || ctl >= bk->chunks_per_slice)
		return -EINVAL;
	g = bk->grp;
	rfs_bm_ctl_window(s, ctl, &wlo, &whi);
	nbits = whi - wlo;
	nblocks = nbits / RAVENFS_BITS_PER_BLOCK;	/* §3：恒整页，无 sub-page 尾块 */
	if (!nblocks)
		return -EINVAL;
	t0 = ktime_get_ns();	/* bm_scan_ns：mount 顺序扫描逐窗计时 */

	mutex_lock(&bk->chg_mu);
	if (rfs_bm_chunk_read(g, bk->ctls[ctl].disk_block_rel, bk->buffer, nblocks)) {
		mutex_unlock(&bk->chg_mu);
		RFS_STAT_ADD(g->sb, bm_scan_ns, ktime_get_ns() - t0);
		return -EIO;
	}
	words = (const u64 *)bk->buffer;
	ones = 0;
	for (w = 0; w < (nbits >> 6); w++)
		ones += (u64)hweight64(words[w]);
	zeros = nbits - ones;
	s->free_count += zeros;	/* 每片 free_count（02 §4 gate 保真） */
	bk->ctls[ctl].summary = (zeros == 0) ? RFS_BM_SUM_ALL_USED :
			       (ones == 0)  ? RFS_BM_SUM_ALL_FREE :
					      RFS_BM_SUM_MIXED;

	/* §7.4 自描述三段校验（02 §1）：block 组尾段全 1 + inode 组 root 亮灯。
	 * 区间用**全局位号**（§3.4 坐标双视角），否则每片 ctl0 误触/越界读。 */
	gwlo = s->base_idx + wlo;
	gwhi = s->base_idx + whi;
	if (!g->is_inode) {
		u64 lo = max(gwlo, g->self_check_lo), hi = min(gwhi, g->self_check_hi), b;

		for (b = lo; b < hi; b++)
			if (!rfs_bm_get(words, b - gwlo)) {
				rc = -EINVAL;
				break;
			}
	} else {
		u64 rb = g->self_check_root_bit;

		if (rb >= gwlo && rb < gwhi && !rfs_bm_get(words, rb - gwlo))
			rc = -EINVAL;
	}
	mutex_unlock(&bk->chg_mu);
	RFS_STAT_ADD(g->sb, bm_scan_ns, ktime_get_ns() - t0);
	return rc;
}

/* §9 单 chunk 三态分派（已持 chg_mu）：态1 ONLINE&&dirty → 锁内快照+清脏 → 锁外
 * 写 → 重算；态2 OFFLINE&&有账 → 读 → 应用+摘节点 → 写 → 重算；态3 其余跳过。
 * 写失败：态1 恢复脏标（buffer 是真相）；态2 摘要降 MIXED（盘态未知，保守）。 */
static void rfs_bm_chunk_sync_one(struct ravenfs_bm_slice *s, u32 ctl)
{
	struct rfs_bm_backing *bk = s->backing;
	struct ravenfs_bm_group *g;
	u64 wlo, whi, nbits, nblocks;
	bool offline_recs;

	if (!bk || ctl >= bk->chunks_per_slice)
		return;
	g = bk->grp;
	rfs_bm_ctl_window(s, ctl, &wlo, &whi);
	nbits = whi - wlo;
	nblocks = nbits / RAVENFS_BITS_PER_BLOCK;

	spin_lock(&s->lock);
	if (bk->online_chunk == ctl && bk->ctls[ctl].dirty) {	/* 态1 */
		memcpy(bk->snapshot, bk->buffer, nbits / 8);
		bk->ctls[ctl].dirty = 0;
		spin_unlock(&s->lock);
		if (rfs_bm_chunk_write(g, bk->ctls[ctl].disk_block_rel, bk->snapshot,
				       nblocks)) {
			spin_lock(&s->lock);
			bk->ctls[ctl].dirty = 1;	/* 写失败：buffer 是真相，回滚脏标 */
			spin_unlock(&s->lock);
			return;
		}
		spin_lock(&s->lock);
		rfs_bm_recompute_summary(s, ctl, bk->buffer);
		spin_unlock(&s->lock);
		RFS_STAT_INC(g->sb, bm_ttl_online);	/* TTL 态1：在线脏窗同步量 */
		RFS_STAT_INC(g->sb, bm_sync_slices);
		return;
	}
	offline_recs = (bk->online_chunk != ctl) && bk->ctls[ctl].has_recs;
	spin_unlock(&s->lock);
	if (!offline_recs)
		return;					/* 态3：其余跳过 */

	/* 态2：OFFLINE&&有账。用 snapshot 而非 buffer（buffer 归线上窗所有，§5）。
	 * 读失败 = 记录原样留待下次（不丢）；写失败 = 摘要降 MIXED 保守。 */
	if (rfs_bm_chunk_read(g, bk->ctls[ctl].disk_block_rel, bk->snapshot, nblocks))
		return;
	spin_lock(&s->lock);
	if (bk->online_chunk == ctl) {		/* 被换上线 → 记录已被那次发布 drain */
		spin_unlock(&s->lock);
		return;
	}
	(void)rfs_bm_drain_window(s, ctl, (u64 *)bk->snapshot);
	spin_unlock(&s->lock);
	if (rfs_bm_chunk_write(g, bk->ctls[ctl].disk_block_rel, bk->snapshot, nblocks)) {
		spin_lock(&s->lock);
		bk->ctls[ctl].summary = RFS_BM_SUM_MIXED;	/* 盘态未知 → 保守降级 */
		spin_unlock(&s->lock);
		return;
	}
	spin_lock(&s->lock);
	rfs_bm_recompute_summary(s, ctl, bk->snapshot);
	spin_unlock(&s->lock);
	RFS_STAT_INC(g->sb, bm_ttl_drain);	/* TTL 态2：离线还账窗同步量 */
	RFS_STAT_INC(g->sb, bm_sync_slices);
}

/* §9 单片 TTL：遍历本片 chunk 数组（不拿全片锁），逐 chunk 三态分派 */
static void rfs_bm_chunk_slice_sync(struct ravenfs_bm_slice *s)
{
	u32 c;

	if (!s->backing)
		return;					/* FULL：现全量行为归 rfs_bm_group_sync */
	for (c = 0; c < s->backing->chunks_per_slice; c++) {
		mutex_lock(&s->backing->chg_mu);	/* §5 反例依据：I/O 串行 chg_mu */
		rfs_bm_chunk_sync_one(s, c);
		mutex_unlock(&s->backing->chg_mu);
	}
}

/* mgr teardown 用：释放 buffer/snapshot/ctls/backing（幂等；FULL = 空操作）。
 * record 链上残留节点随组池整体释放（池拥有节点，无二次分配）。 */
static void rfs_bm_release_backing(struct ravenfs_bm_slice *s)
{
	struct rfs_bm_backing *bk = s->backing;

	if (!bk)
		return;
	kfree(bk->ctls);
	kvfree(bk->buffer);
	kvfree(bk->snapshot);
	mutex_destroy(&bk->chg_mu);
	kfree(bk);
	s->backing = NULL;
}

/* §2 装配：有效 order 夹断 → record 池 → 逐片 backing（buffer/snapshot/ctls）。
 * 失败 → 返回负值；调用方（create bad 路径）经 rfs_bm_group_release 全数安全拆除。 */
static int rfs_bm_group_chunk_setup(struct ravenfs_bm_group *g)
{
	u32 i, c;
	u64 chunk_bytes, chunk_bits;
	u8 want = g->chunk_order;

	g->chunk_order = rfs_bm_fit_chunk_order(want, g->per_bits >> 3);
	if (g->chunk_order != want)
		pr_warn("ravenfs: bitmap(%s) chunk_order %u clamped to %u (slice %llu B)\n",
			g->name, want, g->chunk_order,
			(unsigned long long)(g->per_bits >> 3));
	chunk_bytes = 4096ULL << g->chunk_order;
	chunk_bits = chunk_bytes * 8;

	g->pool = rfs_bm_pool_create(g->rec_pool_depth ? g->rec_pool_depth
						       : RFS_BM_REC_POOL_DEPTH, g->is_inode);
	if (!g->pool)
		return -ENOMEM;

	for (i = 0; i < g->n_slices; i++) {
		struct ravenfs_bm_slice *s = &g->slices[i];
		struct rfs_bm_backing *bk;
		u64 begin = s->base_idx, cnt = s->bits;

		bk = kzalloc(sizeof(*bk), GFP_KERNEL);
		if (!bk)
			return -ENOMEM;
		s->backing = bk;
		bk->grp = g;
		bk->mode = RFS_BM_BACKING_CHUNK;
		bk->chunk_order = g->chunk_order;
		bk->chunks_per_slice = (u32)DIV_ROUND_UP_ULL(cnt, chunk_bits);
		bk->online_chunk = RFS_BM_CHUNK_NONE;
		bk->pool = g->pool;
		mutex_init(&bk->chg_mu);
		bk->buffer = kvmalloc(chunk_bytes, GFP_KERNEL);
		bk->snapshot = kvmalloc(chunk_bytes, GFP_KERNEL);
		if (!bk->buffer || !bk->snapshot)
			return -ENOMEM;
		memset(bk->buffer, 0, chunk_bytes);
		memset(bk->snapshot, 0, chunk_bytes);
		bk->ctls = kcalloc(bk->chunks_per_slice, sizeof(*bk->ctls), GFP_KERNEL);
		if (!bk->ctls)
			return -ENOMEM;
		/* 控制器预构造（§4：上线前即构造好；摘要保守 MIXED，mount 扫描落真值） */
		for (c = 0; c < bk->chunks_per_slice; c++) {
			u64 wlo = (u64)c << (15 + g->chunk_order);
			u64 whi = min(wlo + chunk_bits, cnt);

			bk->ctls[c].disk_block_rel = (begin + wlo) / RAVENFS_BITS_PER_BLOCK;
			bk->ctls[c].tail_bits = whi - wlo;
			bk->ctls[c].summary = RFS_BM_SUM_MIXED;
		}
	}
	return 0;
}

/* ══ 组级 CHUNK 路由（循环形态与 FULL 同构；差异仅片内分叉 + 阻塞锁纪律）══ */

/* 硬分配路由：锁忙/片内无货/I/O 失败 → 换片，preferred 亲和与阻塞纪律不变 */
static u64 rfs_bm_group_chunk_alloc(struct ravenfs_bm_group *g, u64 count, u64 align)
{
	u32 cpu = (u32)raw_smp_processor_id() % g->cpu_slots;
	u32 round;

	for (round = 0; round < g->n_slices; round++) {
		u32 idx = (g->preferred[cpu] + round) % g->n_slices;
		u64 hit = 0;

		if (rfs_bm_chunk_slice_alloc_hard(&g->slices[idx], align, count, &hit) == 0) {
			g->preferred[cpu] = idx;
			return hit;
		}
	}
	return RFS_BM_NO_HIT;
}

/* 软分配路由：一次只从单片取，缺口回传，全片失败 NO_HIT */
static u64 rfs_bm_group_chunk_alloc_soft(struct ravenfs_bm_group *g, u64 count,
					 u64 align, u64 *gap)
{
	u32 cpu = (u32)raw_smp_processor_id() % g->cpu_slots;
	u32 round;

	*gap = count;
	for (round = 0; round < g->n_slices; round++) {
		u32 idx = (g->preferred[cpu] + round) % g->n_slices;
		u64 hit = 0, left = count;

		if (rfs_bm_chunk_slice_alloc_soft(&g->slices[idx], align, count, &hit, &left) == 0) {
			g->preferred[cpu] = idx;
			*gap = left;
			return hit;
		}
	}
	return RFS_BM_NO_HIT;
}

/* 锚定占领路由：锚点片唯一 → 片内窗迭代自消化；片尾跨界续圈归本循环
 *（逐片单锁前缀链 = 真实连续，同 FULL） */
static u64 rfs_bm_group_chunk_occupy_soft(struct ravenfs_bm_group *g, u64 base,
					  u64 count, u64 *rest)
{
	u64 nbits = g->nbits, b = base, remain = count;

	while (remain && b < nbits) {
		u32 idx = (u32)(b / g->per_bits);
		struct ravenfs_bm_slice *s;
		u64 slice_end, left = 0;
		int rc;

		if (idx >= g->n_slices)
			break;
		s = &g->slices[idx];
		rc = rfs_bm_chunk_slice_occupy(s, b, remain, &left);
		if (rc)
			break;			/* 锚点即占用：前缀断，不跨 */
		b += remain - left;
		remain = left;
		if (!remain)
			break;
		slice_end = min((u64)(idx + 1) * g->per_bits, nbits);
		if (b != slice_end)
			break;			/* 中途撞占用：跨界续圈无意义 */
	}
	*rest = remain;
	return (count - remain) ? base : (u64)PTR_INVALID;
}

/* 区间释放路由：逐片切段同 FULL；片内在线直改/离线兜底由 slice_free 自管锁 */
static void rfs_bm_group_chunk_free(struct ravenfs_bm_group *g, u64 base, u64 count)
{
	u64 i = base;

	while (count) {
		u32 idx = (u32)(i / g->per_bits);
		struct ravenfs_bm_slice *s;
		u64 hi, n;

		if (idx >= g->n_slices)
			break;			/* 越界剩余放弃（防御） */
		s = &g->slices[idx];
		hi = s->base_idx + s->bits;
		if (i >= hi) {			/* 落入片间空洞 → 前进到下一片 */
			i = ((u64)idx + 1) * g->per_bits;
			continue;
		}
		n = min(count, hi - i);
		rfs_bm_chunk_slice_free(s, i, n);
		i += n;
		count -= n;
	}
}

/* 单点测试路由（返回值语义与 FULL 逐位一致，§1.1） */
static bool rfs_bm_group_chunk_test(struct ravenfs_bm_group *g, u64 bit)
{
	if (bit >= g->nbits)
		return false;
	return rfs_bm_chunk_slice_test(&g->slices[bit / g->per_bits], bit);
}

/* 区间同值测试路由（CHUNK 逐位，按需换片）：语义同 FULL 组级版 */
static int rfs_bm_group_chunk_test_interval(struct ravenfs_bm_group *g, u64 base,
					    u64 len, bool val)
{
	u64 b = base, end;

	if (len && base + len < base)
		return -EINVAL;
	end = base + len;
	if (end > g->nbits)
		return -EINVAL;
	while (b < end) {
		u32 idx = (u32)(b / g->per_bits);
		struct ravenfs_bm_slice *s;
		u64 slice_end, stop, j;

		if (idx >= g->n_slices)
			return -EINVAL;
		s = &g->slices[idx];
		slice_end = min((u64)(idx + 1) * g->per_bits, g->nbits);
		stop = min(slice_end, end);
		for (j = b; j < stop; j++)
			if (rfs_bm_chunk_slice_test(s, j) != val)
				return -ENOENT;
		b = stop;
	}
	return 0;
}

/* §7 mount 顺序扫描（CHUNK ctor 唯一 I/O 源）：一趟产岀 每片 free_count / 每 ctl
 * 2bit 摘要 / 自描述校验。FULL 不进入（零 I/O 现行为不变）。 */
static int rfs_bm_group_chunk_load(struct ravenfs_bm_group *g)
{
	u32 i, c;

	for (i = 0; i < g->n_slices; i++) {
		struct ravenfs_bm_slice *s = &g->slices[i];

		for (c = 0; c < s->backing->chunks_per_slice; c++) {
			int rc = rfs_bm_chunk_mount_scan(s, c);

			if (rc)
				return rc;
		}
	}
	return 0;
}

/* §9 TTL 三态分派（组级）：只写脏/有待还 record 的 chunk（替代 02 §5 全量） */
static int rfs_bm_group_chunk_sync(struct ravenfs_bm_group *g)
{
	u32 i;

	for (i = 0; i < g->n_slices; i++)
		rfs_bm_chunk_slice_sync(&g->slices[i]);
	return 0;
}

/* ── 装载 / 全量刷回 ── */

static int rfs_bm_group_load(struct super_block *sb, struct ravenfs_bm_group *g)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 b;

	/* §12 分叉：CHUNK 计数+摘要扫描（不全量 memcpy 进 vzalloc）；FULL 旧码原样 */
	if (g->backing_mode == RFS_BM_BACKING_CHUNK)
		return rfs_bm_group_chunk_load(g);

	for (b = 0; b < g->disk_blocks; b++) {
		struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, g->disk_start + b));

		if (!bh) {
			pr_err("ravenfs: bitmap(%s) load fail at blk %llu\n",
			       g->name, (unsigned long long)(g->disk_start + b));
			return -EIO;
		}
		memcpy(g->words + b * (RAVENFS_BLOCK_SIZE / sizeof(u64)), bh->b_data,
		       RAVENFS_BLOCK_SIZE);
		brelse(bh);
		RFS_STAT_INC(g->sb, io_read_ops);
		RFS_STAT_ADD(g->sb, io_read_bytes, RAVENFS_BLOCK_SIZE);
	}
	rfs_bm_group_scan_free(g);
	return 0;
}

/*
 * 片级同步（无脏标记）：把**全部片**与磁盘同步一遍。
 *   · 原子性：与 alloc/free 对齐的最小临界区 = 片自旋锁内「memcpy 快照整片字」的瞬间
 *     （拷贝完即可解锁，分配照旧只在同一自旋锁内做位运算，不会读到撕裂快照）。
 *   · I/O 全在锁外（绝不入自旋锁）；写者之间由组内 `wb_lock`(mutex) 串行（不挡分配/释放）。
 *   · 快照到 I/O 完成期间的变异不在本次内（下轮再同步）⇒ 只保证“某个一致时间点”落盘，
 *     崩溃序无法通解，窗口由调用周期（TTL 30s）限制。
 *   · ro 挂载不得调用。
 */
static int rfs_bm_group_sync(struct super_block *sb, struct ravenfs_bm_group *g)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	u64 buf_bytes = g->per_bits / 8;
	void *buf;
	u32 i;
	int err = 0;

	/* §12 分叉：CHUNK 三态分派（只写脏/有待还 record 的 chunk）；FULL 全量行为原样 */
	if (g->backing_mode == RFS_BM_BACKING_CHUNK)
		return rfs_bm_group_chunk_sync(g);

	if (!g->n_slices || !buf_bytes)
		return 0;
	buf = kvmalloc(buf_bytes, GFP_KERNEL);	/* 片上限缓冲；锁外分配 */
	if (!buf)
		return -ENOMEM;

	mutex_lock(&g->wb_lock);
	for (i = 0; i < g->n_slices; i++) {
		struct ravenfs_bm_slice *s = &g->slices[i];
		u64 nblk, b;

		/* 片 = 整块边界（per_bits/base_idx 均 32768 对齐）⇒ bits/8 为 4K 整数倍 */
		if (!s->bits)
			continue;
		spin_lock(&s->lock);
		memcpy(buf, g->words + s->base_idx / 64, s->bits / 8);
		spin_unlock(&s->lock);

		/* 锁外 I/O：逐块全覆盖写（与装载同源 sb_bread） */
		nblk = s->bits / RAVENFS_BITS_PER_BLOCK;
		for (b = 0; b < nblk; b++) {
			u64 dblk = g->disk_start + s->base_idx / RAVENFS_BITS_PER_BLOCK + b;
			struct buffer_head *bh = sb_bread(sb, ravenfs_dev_blk(sbi, dblk));

			if (!bh) {
				err = -EIO;
				break;
			}
			memcpy(bh->b_data, buf + b * RAVENFS_BLOCK_SIZE, RAVENFS_BLOCK_SIZE);
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);	/* 确定性：返回即落盘 */
			brelse(bh);
			RFS_STAT_INC(sb, io_write_ops);
			RFS_STAT_ADD(sb, io_write_bytes, RAVENFS_BLOCK_SIZE);
		}
		if (err)
			break;		/* 一个片失败即停，剩余片未同步（下轮/下次重试） */
		RFS_STAT_INC(sb, bm_sync_slices);	/* FULL：逐片全量写回片数 */
	}
	mutex_unlock(&g->wb_lock);
	kvfree(buf);
	return err;
}

/* ── mgr 生命周期 ── */

int ravenfs_bitmap_create(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_bitmap_mgr *m;
	u64 blk_bits = sbi->bitmap_blocks * RAVENFS_BITS_PER_BLOCK;
	u64 ino_bits = sbi->inode_bm_blocks * RAVENFS_BITS_PER_BLOCK;
	u32 ncpu = num_possible_cpus();
	u8 mode = (bitmap_backing != RFS_BM_BACKING_FULL)
			  ? RFS_BM_BACKING_CHUNK : RFS_BM_BACKING_FULL;
	u32 n_slices;
	int err;

	if (!blk_bits || !ino_bits)
		return -EINVAL;
	n_slices = rfs_bm_slice_count(blk_bits, ino_bits, ncpu);

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	/* CHUNK 装配参数下放双组（§2；FULL 组全字段惰性不用）。
	 * 注：CHUNK 开关走**模块参数**（写范围不能碰 super.c/挂载项）；默认 FULL。 */
	m->blk.sb = m->ino.sb = sb;
	m->blk.backing_mode = m->ino.backing_mode = mode;
	m->blk.is_inode = false;
	m->ino.is_inode = true;
	m->blk.chunk_order = m->ino.chunk_order =
		(u8)min_t(uint, bitmap_chunk_order, RFS_BM_CHUNK_ORDER_MAX);
	m->blk.rec_pool_depth = m->ino.rec_pool_depth = RFS_BM_REC_POOL_DEPTH;
	/* §7.4 自描述三段校验区间：block 组尾段 [inode_bm_base, nbits) 全 1；
	 * inode 组 root 位（v1.2 = root_ino=16，非 0）亮灯。FULL 不查（现行为）。 */
	m->blk.self_check_lo = sbi->inode_bm_base;
	m->blk.self_check_hi = blk_bits;
	m->ino.self_check_root_bit = sbi->root_ino;

	err = rfs_bm_group_setup(&m->blk, "block", blk_bits, n_slices, max(ncpu, 1u));
	if (err)
		goto bad;
	err = rfs_bm_group_setup(&m->ino, "inode", ino_bits, n_slices, max(ncpu, 1u));
	if (err)
		goto bad;

	m->blk.disk_start = sbi->bitmap_start;
	m->blk.disk_blocks = sbi->bitmap_blocks;
	m->ino.disk_start = sbi->inode_bm_base;
	m->ino.disk_blocks = sbi->inode_bm_blocks;

	err = rfs_bm_group_load(sb, &m->blk);
	if (err)
		goto bad;
	err = rfs_bm_group_load(sb, &m->ino);
	if (err)
		goto bad;

	sbi->bitmap = m;
	pr_info("ravenfs: bitmap mgr: backing=%s slices=%u block_bits=%llu(free=%llu) inode_bits=%llu(free=%llu)\n",
		mode == RFS_BM_BACKING_CHUNK ? "CHUNK" : "FULL", n_slices,
		(unsigned long long)blk_bits, (unsigned long long)ravenfs_block_free_total(sb),
		(unsigned long long)ino_bits, (unsigned long long)ravenfs_inode_free_total(sb));
	return 0;
bad:
	sbi->bitmap = NULL;   /* 尚未发布，free_total 等不得经 sbi 看到半成品 */
	rfs_bm_group_release(&m->ino);
	rfs_bm_group_release(&m->blk);
	kfree(m);
	return err;
}

static void rfs_bm_group_release(struct ravenfs_bm_group *g)
{
	u32 i;

	if (g->slices) {
		for (i = 0; i < g->n_slices; i++)
			rfs_bm_release_backing(&g->slices[i]);
	}
	rfs_bm_pool_destroy(g->pool);
	g->pool = NULL;
	vfree(g->words);
	kfree(g->slices);
	kfree(g->preferred);
}

/* 全片同步（两组）：TTL 周期 / sync_fs / put_super / fsync 共用；ro 挂载不得调用 */
int ravenfs_bitmap_flush(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	int err = 0;

	if (!sbi || !sbi->bitmap)
		return 0;
	if (rfs_bm_group_sync(sb, &sbi->bitmap->blk))
		err = -EIO;
	if (rfs_bm_group_sync(sb, &sbi->bitmap->ino))
		err = -EIO;
	if (err)
		pr_err("ravenfs: bitmap flush failed\n");
	return err;
}

void ravenfs_bitmap_destroy(struct super_block *sb)
{
	struct ravenfs_sb_info *sbi = sb->s_fs_info;
	struct ravenfs_bitmap_mgr *m = sbi ? sbi->bitmap : NULL;

	if (!m)
		return;
	rfs_bm_group_release(&m->blk);
	rfs_bm_group_release(&m->ino);
	kfree(m);
	sbi->bitmap = NULL;
}

/* ── 对外 API（R-NS/R-FD/R-LM 用） ── */

u64 ravenfs_block_alloc(struct super_block *sb, u64 count, u32 align_log2)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;
	u64 hit, align;

	if (!m || !count)
		return (u64)PTR_INVALID;
	align = 1ULL << min(align_log2, 16u);	/* 防御截断 16 */
	hit = rfs_bm_group_alloc(&m->blk, count, align);
	return (hit == RFS_BM_NO_HIT) ? (u64)PTR_INVALID : hit;
}

/*
 * 软分配（god block_alloc_soft）：单片取 min(片内可给, count)，**一片为止**；
 * preferred 起扫。*remain = 缺口；全片无 → PTR_INVALID 且 *remain = count。
 */
u64 ravenfs_block_alloc_soft(struct super_block *sb, u64 count, u64 *remain)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;
	u64 hit, gap = count;

	if (!m || !count) {
		if (remain)
			*remain = count;
		return (u64)PTR_INVALID;
	}
	hit = rfs_bm_group_alloc_soft(&m->blk, count, 1, &gap);
	if (remain)
		*remain = gap;
	return (hit == RFS_BM_NO_HIT) ? (u64)PTR_INVALID : hit;
}

/*
 * 锚定认领（god block_occupy_soft）：blkbase 起认领 ≤ count 连续空闲块；
 * 返回原址（认领 ≥1）/ PTR_INVALID；*rest = 缺口。
 */
u64 ravenfs_block_occupy_soft(struct super_block *sb, u64 base, u64 count, u64 *rest)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;
	u64 gap = count, ret;

	if (!m || !count) {
		if (rest)
			*rest = count;
		return (u64)PTR_INVALID;
	}
	ret = rfs_bm_group_occupy_soft(&m->blk, base, count, &gap);
	if (rest)
		*rest = gap;
	return ret;
}

/* 区间同值测试（god test_interval 组级版）：0=全同 / -ENOENT=异位 / -EINVAL=越界 */
int ravenfs_block_test_interval(struct super_block *sb, u64 base, u64 len, bool val)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m)
		return -EINVAL;
	return rfs_bm_group_test_interval(&m->blk, base, len, val);
}

void ravenfs_block_free(struct super_block *sb, u64 blk, u64 count)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m || !count)
		return;
	rfs_bm_group_free(&m->blk, blk, count);
}

bool ravenfs_block_test(struct super_block *sb, u64 blk)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m || blk >= m->blk.nbits)
		return false;
	/* §1.1 语义不变：CHUNK 路由裁决（可能按需换片）；FULL 直读原样 */
	if (m->blk.backing_mode == RFS_BM_BACKING_CHUNK)
		return rfs_bm_group_chunk_test(&m->blk, blk);
	return rfs_bm_get(m->blk.words, blk);
}

u64 ravenfs_inode_alloc(struct super_block *sb)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m)
		return (u64)PTR_INVALID;
	{
		u64 hit = rfs_bm_group_alloc(&m->ino, 1, 1);

		return (hit == RFS_BM_NO_HIT) ? (u64)PTR_INVALID : hit;
	}
}

void ravenfs_inode_free(struct super_block *sb, u64 inum)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m)
		return;
	rfs_bm_group_free(&m->ino, inum, 1);
}

bool ravenfs_inode_test(struct super_block *sb, u64 inum)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;

	if (!m || inum >= m->ino.nbits)
		return false;
	/* §1.1 语义不变：CHUNK 路由裁决；FULL 直读原样 */
	if (m->ino.backing_mode == RFS_BM_BACKING_CHUNK)
		return rfs_bm_group_chunk_test(&m->ino, inum);
	return rfs_bm_get(m->ino.words, inum);
}

u64 ravenfs_block_free_total(struct super_block *sb)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;
	u64 n = 0;
	u32 i;

	if (!m)
		return 0;
	for (i = 0; i < m->blk.n_slices; i++)
		n += m->blk.slices[i].free_count;
	return n;
}

u64 ravenfs_inode_free_total(struct super_block *sb)
{
	struct ravenfs_bitmap_mgr *m = ((struct ravenfs_sb_info *)sb->s_fs_info)->bitmap;
	u64 n = 0;
	u32 i;

	if (!m)
		return 0;
	for (i = 0; i < m->ino.n_slices; i++)
		n += m->ino.slices[i].free_count;
	return n;
}

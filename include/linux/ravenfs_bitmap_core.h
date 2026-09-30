/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_bitmap_core.h —— 双位图分片分配器「类本体」（C 类）
 *                          RavenFS · linux-ko bm 模块（2026-09-30 拆分）
 *
 * 定位（对齐 god `include/ravenfs/bitmap_slices.hpp` v1 + `bitmap_slices_v2.hpp` v2）：
 *   · 纯算法类：**零 super_block 依赖**；arg0 恒为类的数据本体（`struct rfs_bm *self`）。
 *   · **两种后端，二选一（外部经 cfg.choose 指定）**：
 *       - RFS_BM_MODE_MEM   （= god v1 `bitmap_slices_mgr_t`）
 *           位图**全量常驻内存**，内存由「拥有者」提供，类不取所有权；零 I/O。
 *           内核：vzalloc + sb_bread 装载；用户态压测：aligned_alloc/mmap 直接喂。
 *       - RFS_BM_MODE_CHUNK （= god v2 `bitmap_slices_mgr_t_v2`）
 *           位图**懒加载窗口（CHUNK）**：类自持每片窗口缓冲 + 归还记录链；
 *           I/O 经**赋能式回调**（read_block/write_block + io_ctx，宿主给）；
 *           位数由「位图块数」推导（不传）。
 *   · 平台差异（类型 / 内存 / CPU / 日志 / 锁）全部下沉 `ravenfs_platform.h`；
 *     本头**不含任何 `#ifdef __KERNEL__`**（算法头保持平台中立）。
 *
 * 对象形态（god 投影）：
 *   struct rfs_bm        ⇄ god v1/v2 的 mgr（**按 mode 合并为一个类**）
 *   struct rfs_bm_group  ⇄ god v1/v2 的 group_t
 *   struct rfs_bm_slice  ⇄ god v1 bitmap_slice_t          （MEM 后端）
 *   struct rfs_bm_cslice ⇄ god v2 chunk_slice_mgr_t       （CHUNK 后端）
 *
 * ★ 核心取舍（合并 v1/v2 成一个类时定的，参考 god 两张头）：
 *   1. **一个类、两个分片结构**（非 union）：MEM/CHUNK 各一套 slice 结构体，
 *      `rfs_bm_group` 持两个指针，按 mode 取其一（另一为 NULL）。理由 = god 本就把两套
 *      分片拆成两个类；C 里用两个结构体比 union 更可读、各自字段不互相污染。
 *   2. **对外坐标恒为全局位号**（两模式一致）：MEM 片即全局视角（含 base_idx）；
 *      CHUNK 片内部走 god v2 的**局部（0 起）坐标系**，global↔local 换收在 group 路由。
 *      —— 这是"公交换乘站唯一"：外部只认全局位号，v2 的局部视角不外泄。
 *   3. **锁**：MEM 片 = `rfs_spin_t`（阻塞自旋，ko 现状口径）；
 *      CHUNK 片 = `rfs_mutex_t`（可睡；god v2 允许持锁等盘 I/O）。
 *   4. **I/O**：MEM 无 I/O（装载/回写归拥有者）；CHUNK 经 io 回调在类内做（god v2）。
 *   5. 公开方法集 = **与 ko 适配层 `ravenfs_bitmap.h` 的 13 个函数 1:1**，
 *      适配层只做 `sb/sbi ↔ self` 取值与 RC↔PTR_INVALID/errno 翻译。
 *
 * ★★ 语义视角（**关键，务必分清——混淆即误判**）：
 *   · MEM  ：位图**直接在内存里 = 一等事实（first-class truth）**。位态与 free_count
 *            即真值——`block_test` / `*_free_total` **随时可直接采信**；类不引入任何
 *            「待合成」中间态。逐步可观测 == 真值（与 god v1 同）。
 *   · CHUNK：**内存里没有全图的直接事实**。任何「当前位态 / 空闲计数」都只是
 *            **合成视角（synthesized view）**——由「在线窗 active_chunk_buffer +
 *            挂账归还链 rec_head + 盘上块」在**合并点**合成：
 *              ① 该窗被使用之前（alloc/occupy 进入时先 drain 在线窗记录）；
 *              ② `rfs_bm_chunks_asyn`（整片读→合并→写 + 计数重扫校准）。
 *            ⇒ 挂账期（未合并）读到的 `block_test` / `free_count` **可以与真值暂时
 *              不一致**（free 挂账即记账 → 计数可虚高；被释放的位在合并前仍为 1）。
 *            ⇒ **判定/比对必须以「合成后（提交态）」视角为准**：先触发合并（使用该窗
 *              或 `chunks_asyn`）再与 MEM 的结果比。**不得**拿 CHUNK 的逐步视图去与
 *              MEM 的逐步视图逐拍相等——那是把「合成视角」当成「直接事实」，是设计
 *              误读，**不是缺陷**。
 *
 * 命名：与 god 的 C++ `bitmap_slices*.hpp` 并列；本头是 ko + 用户态压测共用的 **C** 版。
 * 真值来源：god `bitmap_slices.hpp`（v1 抽取范式）、`bitmap_slices_v2.hpp`（v2 窗口版）。
 * 设计文档：Docs/runtime_tech/{02_bitmap_slices.md,03_chunk_backed_slices.md}。
 */
#ifndef _RAVENFS_BITMAP_CORE_H
#define _RAVENFS_BITMAP_CORE_H

#include "ravenfs_platform.h"     /* 类型 / 内存 / CPU / 日志 / 锁（唯一平台收口）*/
/* 返回码口径 = Linux 内核风：0 成功 / 负 -errno（不再引 god 的 result_code_t）。*/

#ifdef __cplusplus
extern "C" {
#endif

/* ═════════════════════════════ 常量 / 模式 ═════════════════════════════ */
/* 片下限：1 个位图块 = 4096B × 8 = 32768 位（保证片基址 4K 对齐；勿动） */
#define RFS_BM_SLICE_MIN_BITS	32768ULL
/* 分配未命中哨兵（= PTR_INVALID 之核；拥有者直接透传）*/
#define RFS_BM_NO_HIT		(~0ULL)

/* 后端模式（= rfs_bm_cfg.choose）*/
enum rfs_bm_mode {
	RFS_BM_MODE_MEM   = 0,	/* god v1：全量常驻内存，零 I/O */
	RFS_BM_MODE_CHUNK = 1,	/* god v2：CHUNK 懒加载窗口，I/O 经回调 */
};

/* ── CHUNK 后端常量（god v2；MEM 后端不参与）── */
#define RFS_BM_CHUNK_NONE	0xFFFFFFFFu	/* online_idx 哨兵（无窗上线）*/

/* chunk 控制器 compact_word 位域（god v2；[0,2) summary · [2,3) dirty · [3,4) has_recs）*/
#define RFS_BM_CHUNK_SUM_MASK		0x3ull
#define RFS_BM_CHUNK_SUM_ALL_USED	0ull	/* 盘态全 1（无空闲）*/
#define RFS_BM_CHUNK_SUM_ALL_FREE	1ull	/* 整窗可判空闲 */
#define RFS_BM_CHUNK_SUM_MIXED		2ull	/* 须上线查 */
#define RFS_BM_CHUNK_FLAG_DIRTY		(1ull << 2)	/* 在线且被改（未回写）*/
#define RFS_BM_CHUNK_FLAG_HAS_RECS	(1ull << 3)	/* 归还记录链非空 */

/* 分片状态机（god v2）：仅 WORKABLE 接待分配/释放/测试 */
enum rfs_bm_slice_state {
	RFS_BM_SLICE_UNWORKABLE = 0,	/* 未 enable / 已 shutdown */
	RFS_BM_SLICE_WORKABLE   = 1,	/* enable 后、shutdown 前 */
};

/* ═══════════════════════ 计数出口（可选注入；平台中立） ═══════════════════════ */
/*
 * 类本体**零 super_block 依赖**，计数只能以「事件 + 增量」上报：字段归属与累加
 * 语义由宿主决定（内核适配层映射到 `struct ravenfs_sb_stats.bm_*`；用户态压测不注入
 * → emit 为 NULL → 零开销、零依赖）。
 * 枚举顺序对齐设计 §3.F 的 F 组字段顺序（勿乱动；增删字段无兼容义务）。
 *
 * 模式契合：MEM 后端只会上报 ALLOC_*、FREE_*、SOFT_OK、OCCUPY、SYNC_SLICES 子集
 * （CHUNK 专属事件在 MEM 下永不发出 ⇒ 对应字段恒 0）；CHUNK 后端则覆盖全集。
 * 打点代码只写在对应后端，读侧（debugfs）恒呈现全字段、非本后端字段为 0。
 */
enum rfs_bm_stat {
	RFS_BM_ST_ALLOC_CALLS = 0,	/* 组级分配入口次数 */
	RFS_BM_ST_ALLOC_OK,		/* 分配成功次数 */
	RFS_BM_ST_ALLOC_FAIL,		/* 分配失败次数（ENOSPC/碎片；P5 完备：OK+FAIL==CALLS）*/
	RFS_BM_ST_ALLOC_BLOCKS,		/* 实际分配块总量 */
	RFS_BM_ST_FREE_CALLS,		/* 组级释放入口次数 */
	RFS_BM_ST_FREE_BLOCKS,		/* 实际 1→0 清位量（挂账未清不重复计）*/
	RFS_BM_ST_SOFT_OK,		/* 软分配成功次数 */
	RFS_BM_ST_OCCUPY,		/* 锚定认领次数（认领 ≥1 位）*/
	RFS_BM_ST_SYNC_SLICES,		/* 写回量：MEM=片数 / CHUNK=态1+态2 chunk 数 */
	RFS_BM_ST_CHUNK_SWITCH,		/* [CHUNK] 换片次数 */
	RFS_BM_ST_CHUNK_SWITCH_NS,	/* [CHUNK] 换片累计耗时 ns */
	RFS_BM_ST_REC_ATTACH,		/* [CHUNK] 离线还账挂账节点数 */
	RFS_BM_ST_REC_DRAIN,		/* [CHUNK] drain 消费节点数 */
	RFS_BM_ST_POOL_EMPTY,		/* [CHUNK] 归还记录资源耗尽兜底次数 */
	RFS_BM_ST_SUMMARY_SKIP,		/* [CHUNK] 摘要免载入跳过次数 */
	RFS_BM_ST_SCAN_NS,		/* [CHUNK] mount 扫描累计耗时 ns */
	RFS_BM_ST_TTL_ONLINE,		/* [CHUNK] TTL 态1：在线脏窗同步量 */
	RFS_BM_ST_TTL_DRAIN,		/* [CHUNK] TTL 态2：离线还账窗同步量 */
	RFS_BM_ST__NR,
};

typedef void (rfs_bm_stat_fn)(void *ctx, enum rfs_bm_stat ev, u64 val);

struct rfs_bm_stat_sink {
	rfs_bm_stat_fn	*emit;		/* NULL → 不计数（保持类零依赖）*/
	void		*ctx;		/* 宿主上下文（内核传 sb）*/
};

/* 事件上报（唯一入口；emit 为空则无操作——仅回调/原子，不睡眠，可持锁调用）*/
static inline void rfs_bm_stat(const struct rfs_bm_stat_sink *sink,
			       enum rfs_bm_stat ev, u64 val)
{
	if (sink && sink->emit)
		sink->emit(sink->ctx, ev, val);
}

/* ═══════════════════════ MEM 后端：分片 / 组 ═══════════════════════ */

/* ── 分片（纯内存，god v1 bitmap_slice_t）：片只吃自身状态，位图数据经 words 全局寻址 ── */
struct rfs_bm_slice {
	u64		base_idx;	/* 片首全局位号 */
	u64		bits;		/* 本片有效位数（末片可短） */
	u64		free_count;	/* 空闲位数（增量维护，勿手改） */
	u64		hint;		/* 搜索游标（全局位号；只进不退） */
	rfs_spin_t	lock;		/* 片锁（阻塞；临界区内不 I/O / 不分配内存） */
};

/* ═══════════════════════ CHUNK 后端：soul / 记录 / 控制器 / 分片 ═══════════════════════ */

/* ── 分片静态属性（god v2 slice_soul_t）：I/O 手段 + chunk 几何 + 本片位图区盘上落点 ── */
struct rfs_bm_soul {
	int	(*read_block)(void *io_ctx, u64 blk, u64 blk_len, void *buf);	/* 0 / -errno */
	int	(*write_block)(void *io_ctx, u64 blk, u64 blk_len, const void *buf);	/* 0 / -errno */
	void	*io_ctx;		/* 宿主上下文（内核传 sb/sbi；压测传 FILE 指针 / void 上下文）*/
	u64	 chunk_base_blk;	/* 本片位图区起始 FS_block（绝对）*/
	u64	 chunk_blk_length;	/* 本片位图区总块数 */
	u8	 chunk_order;		/* chunk = 4096 << order 字节 */
};

/* ── 归还记录（god v2 rec_seg_t；片内偏移视角的单链表节点，堆分配）── */
struct rfs_bm_rec {
	u64		off;		/* 片内偏移 */
	u64		len;
	struct rfs_bm_rec *next;
};

/* ── 单 chunk 控制器（god v2 chunk_mgr）：位域打包态 + 归还链头 ── */
struct rfs_bm_cmgr {
	u64			compact_word;	/* 见 RFS_BM_CHUNK_* 位域 */
	struct rfs_bm_rec	*rec_head;	/* 本 chunk 归还记录链头（可 NULL）*/
};

/* ── 窗口化分片（god v2 chunk_slice_mgr_t）：片内局部坐标（0 起）── */
struct rfs_bm_cslice {
	/* 静态属性 & 状态机 */
	struct rfs_bm_soul	soul;		/* enable 拷入（本片不变面）*/
	u8			state;		/* enum rfs_bm_slice_state */

	/* 几何（enable 派生，运行期只读）*/
	u64			slice_bits;	/* 本片位数（32768 倍数）*/
	u32			chunks_per_slice;
	u64			tail_bits;	/* 尾 chunk 有效位 */

	/* 唯一在线窗 */
	u32			online_idx;	/* 当前上线 chunk；RFS_BM_CHUNK_NONE 哨兵 */
	u32			gen;		/* 换片代号 */
	u8			*active_chunk_buffer;	/* 尺寸 = 4096<<order；唯一在线窗 */

	/* 控制器数组（位置即身份 ctl_idx）*/
	struct rfs_bm_cmgr	*mgrs;

	/* 记账 */
	u64			free_count;	/* 本片逻辑空闲位数 */
	u64			in_slice_hint;	/* 片内分配游标（局部；只进不退 + 绕回）*/

	/* 外化全局基址：本片在 group 全局位号系的基址；片内算法不消费，供路由换算 */
	u64			in_global_base_idx;

	/* 唯一分片锁（可睡；覆盖位运算 + 在线窗 + 归还链 + 盘 I/O）*/
	rfs_mutex_t		lock;

	/* 计数出口（组 setup 时由 core 拷入；片内 cs_* 事件经此上报，见 enum rfs_bm_stat）*/
	struct rfs_bm_stat_sink stat;
};

/* ═══════════════════════════ 组 / 类本体 ═══════════════════════════ */

/* ── 组：一组位图（block 组 或 inode 组）；按 mode 取用对应分片数组 ── */
struct rfs_bm_group {
	const char	*name;		/* "block" / "inode"（日志用）*/
	u8		 mode;		/* enum rfs_bm_mode */
	u64		 nbits;		/* 全局位数 */
	u64		 per_bits;	/* 每片位数（32768 对齐）*/
	u32		 n_slices;
	u32		 cpu_slots;
	u32		*preferred;	/* CPU → 上次命中片（类自持）*/

	/* ── MEM 后端（mode==MEM 时有效）── */
	u64			*words;		/* 位图内存：★ 拥有者提供，类不取所有权 */
	struct rfs_bm_slice	*slices;	/* 类自持（RFS_ZALLOC / RFS_FREE）*/

	/* ── CHUNK 后端（mode==CHUNK 时有效）── */
	struct rfs_bm_cslice	*cslices;	/* 类自持 */
	u64			 disk_base;	/* 本组位图起始 FS_block */
	u64			 disk_blocks;	/* 本组位图占块数 */
	u8			 chunk_order;

	/* ── 计数出口（两种模式共用；由 core 在 init 时从 cfg 拷入）── */
	struct rfs_bm_stat_sink stat;
};

/* ── ★ 类本体 ── */
struct rfs_bm {
	u8			mode;		/* enum rfs_bm_mode（两组合一）*/
	struct rfs_bm_group	blk;		/* 块位图组 */
	struct rfs_bm_group	ino;		/* inode 位图组 */
	u32			slice_count;	/* 两组共享（2^n ∩ 满片约束）*/
	u32			cpu_slots;
};

/* ═══════════════════════════ 初始化配置 ═══════════════════════════ */
/* 注：C 结构体不能带默认成员初始化（C++ 语法）——缺省语义写在下方注释，由类实现兜底。*/

/* ── MEM 后端配置（god v1 bitmap_slices_cfg_t 投影）── */
typedef struct no_chunk_slices_cfg_t {
	u8	*block_bits_raw;	/* 块位图原始内存（已加载/已映射，4K 对齐），生命周期归调用方 */
	u64	 block_bits_count;	/* 块组位数（= bitmap_blocks × 4096 × 8）*/
	u8	*inode_bits_raw;	/* inode 位图原始内存（同上）*/
	u64	 inode_bits_count;	/* inode 组位数 */
	u32	 logical_processor_count;	/* preferred 槽数 + 2^n 上界推导；0 → 退化单槽 */
	u64	 min_slice_bits;	/* 片下限（位数）；0 → RFS_BM_SLICE_MIN_BITS */
} no_chunk_slices_cfg_t;

/* ── CHUNK 后端配置（god v2 bitmap_slices_v2cfg_t 投影）── */
typedef struct chunk_slices_cfg_t {
	/* I/O 手段（赋能式；两组共用；返回 0 / -errno）*/
	int	(*read_block)(void *io_ctx, u64 blk, u64 blk_len, void *buf);
	int	(*write_block)(void *io_ctx, u64 blk, u64 blk_len, const void *buf);
	void	*io_ctx;			/* 宿主上下文（内核传 sb/sbi）*/

	/* 两段位图在盘上的落点（位数由块数推导，不传）*/
	u64	 block_bitmap_base_FS_block;	/* 块组位图起始 FS_block */
	u64	 block_bitmap_blocks;		/* 块组位图占块数（bit i ↔ FS_block[i]）*/
	u64	 inode_bitmap_base_FS_block;	/* inode 组位图起始 FS_block */
	u64	 inode_bitmap_blocks;		/* inode 组位图占块数（bit i ↔ inode[i]）*/
	u8	 blkbm_chunk_order;		/* 块组 chunk 几何（4K<<order）*/
	u8	 inobm_chunk_order;		/* inode 组 chunk 几何 */
	u32	 processor_count;		/* preferred 槽数；0 → 退化单槽 */
} chunk_slices_cfg_t;

/* ── 无状态初始化配置（调用方组装；类只消费不持有，纯栈上传递）── */
struct rfs_bm_cfg {
	u8	choose;			/* 0=MEM（纯内存）| 1=CHUNK；其它 → init 报错 */
	no_chunk_slices_cfg_t	pure_mem_cfg;	/* choose==0 时消费 */
	chunk_slices_cfg_t	chunk_cfg;	/* choose==1 时消费 */
	/* 计数出口注入（可选）。宿主把「事件 → 自有计数器」的映射经此传入；类只上报
	 * 事件。缺省（emit==NULL）表示不计数——用户态压测 memset(cfg,0) 即落到该态。*/
	struct rfs_bm_stat_sink stat;
};

/* ═════════════════════ 方法（arg0 恒为类的数据本体）═════════════════════ */

/* ── 生命周期 ──
 * init：按 cfg.choose 走 MEM 或 CHUNK 装配（推导 slice_count → 逐片建 → 扫/装载 free_count）。
 *   · MEM：cfg 只能许「已装载」的位图内存（init 内即扫 free_count，对齐 god v1 ctor）。
 *   · CHUNK：init 内 enable 每片（一次整片读入实测 free_count + 填 2bit 摘要）。
 *   失败返回负（RC/errno）；失败时 self 已自清理（幂等 fini 可安全再调）。
 * fini：释放本类自持资源（slices/cslices/preferred + CHUNK 的 buffer/mgrs/残留链）；
 *   **不碰 MEM 的 words**（归拥有者）。
 */
int	rfs_bm_init(struct rfs_bm *self, const struct rfs_bm_cfg *cfg);
void	rfs_bm_fini(struct rfs_bm *self);

/* 拥有者装载 / 改写位图内存后重算各片 free_count。
 *   MEM：装载路径用（幂等）；CHUNK：no-op（enable 已实测）。*/
void	rfs_bm_rescan_free(struct rfs_bm *self);

/* ── block 组（对外恒为全局位号）── */
/* 硬分配：count 连续 + 起始位 2^align 对齐；单片内完成（不跨片）；未命中 → RFS_BM_NO_HIT。
 *   CHUNK 附加约束（god v2）：请求须 ≤ 单窗容量 2^(15+chunk_order)，否则 NO_HIT。*/
u64	rfs_bm_block_alloc_hard(struct rfs_bm *self, u64 count, u64 align);
/* 软分配：单片内取 min(片内单段可给, count)；*remain 出缺口；未命中 → RFS_BM_NO_HIT */
u64	rfs_bm_block_alloc_soft(struct rfs_bm *self, u64 count, u64 *remain);
/* 锚定认领：base 起贪心认领 ≤ count 连续空闲前缀（可跨片续圈，CHUNK 可跨窗）；原址或 NO_HIT */
u64	rfs_bm_block_occupy_soft(struct rfs_bm *self, u64 base, u64 count, u64 *rest);
/* 区间释放：跨片逐段清位（重复释放不虚高 free_count）*/
void	rfs_bm_block_free(struct rfs_bm *self, u64 base, u64 count);
/* 单点测试：直读位图（无锁；调用方保证并发安全）*/
bool	rfs_bm_block_test(const struct rfs_bm *self, u64 blk);
/* 区间同值测试：[base,base+len) 全 val → 0；异位 → -ENOENT；越界/溢出 → -EINVAL */
int	rfs_bm_block_test_interval(const struct rfs_bm *self, u64 base, u64 len,
				   bool val);

/* ── inode 组（槽分配器；root inode 由 mkfs 预占，类不做保留区）── */
u64	rfs_bm_inode_alloc(struct rfs_bm *self);
void	rfs_bm_inode_free(struct rfs_bm *self, u64 inum);
bool	rfs_bm_inode_test(const struct rfs_bm *self, u64 inum);

/* ── 审计 / 几何 ── */
u64	rfs_bm_block_free_total(const struct rfs_bm *self);	/* 各片 free_count 求和 */
u64	rfs_bm_inode_free_total(const struct rfs_bm *self);
u32	rfs_bm_slice_count(const struct rfs_bm *self);

/* ── 持久化（两模式分工见文件顶取舍 4）──
 * MEM ：锁内快照「第 i 片」位图字 → 调用方锁外做 I/O（类不碰盘）。
 *       dst 需 ≥ slices[i].bits/8 字节；ino=true 取 inode 组。
 * CHUNK：整片周期同步（一次读入 → 合并归还链 → 一次写回；类自行走 io 回调）。
 *       MEM 模式调用 chunks_asyn 返回 -ENOSYS（不适用）。*/
void	rfs_bm_snapshot_slice(const struct rfs_bm *self, bool ino, u32 i, void *dst);
int	rfs_bm_chunks_asyn(struct rfs_bm *self);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_BITMAP_CORE_H */

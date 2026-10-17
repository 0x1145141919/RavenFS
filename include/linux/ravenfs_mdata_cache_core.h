/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_mdata_cache_core.h —— mdata 元数据节点缓存核心（平台中立 · 双编）
 *
 * 定位：god 用户态原型 `include/ravenfs/mdata_piece_caches.hpp` +
 *   `juvenile_god/mdata_piece_cache.cpp`（self_reliat 后端）的 C 语言逐接口语义
 *   翻译，落在 ko 运行时的**多分片静态基数树**后端。
 *
 *   本头只 include `ravenfs_platform.h` —— 内核 kbuild 与用户态 gcc 直链**同一份**
 *   代码：算法文件内不出现任何内核专属符号（容器 / 原子 / 误差指针 / 分配 /
 *   IO / 调度差异全部收口平台头）。
 *
 * ── 两档几何（严格对齐 god）───────────────────────────────────────────────
 *   键空间 = 元数据节点数，而不是 FS_block 数：
 *       n_nodes = DIV_ROUND_UP(total_blocks, 2)      （1 节点 = 2 个连续 4K 块）
 *       bits    = ceil_log2(n_nodes)                 （n_nodes <= 1 → bits = 1）
 *   这与旧实现「bits = ceil_log2(total_blocks)」**不同**：旧实现的键虽然是
 *   node_idx，但位宽按块数推导，导致分片数/树层数系统性偏大一档。本实现以节点数
 *   为唯一真相源；`bits_len` 即 node-count-based ceil_log2。
 *
 *   · bits <= 13  → RAVENFS_MDATA_MODE_CTRL_ARRAY
 *       单分片 + 一张**预分配**叶表：`ravenfs_mrow_simple[n_nodes]` 扁平数组，
 *       行内嵌于表中，片锁一把。小盘全量控制字常驻，数据页懒加载。
 *   · 14 <= bits <= 52 → RAVENFS_MDATA_MODE_RADIX
 *       按 god `mdp_statey_14..52` 策略表切分：
 *         level_bits[0] = w0（分片选择位宽），n_slices = 1 << w0
 *         shard_bits     = bits - w0（每片键空间）
 *         level_bits[1..] = 片内静态基数树逐层位宽
 *         total_levels   = statey 数组长度（1 个分片位 + 树层）
 *         tree_levels    = total_levels - 1
 *       每片一棵**懒建 + 可摘空表**的静态基数树：内部表 = 指针槽数组，叶表 =
 *       `ravenfs_mrow_simple[]`（行内嵌）。表在 ensure_path 时补建；强淘汰
 *       （`ravenfs_mdata_reap_expired_strong`）在片写锁内后序 DFS，把**所有行
 *       均已 IDLE** 的空叶表/空内部表/空根表摘下（非空表绝不释放）；fini 时
 *       DFS 释放残余表。被摘的路径由后续 get 的 ensure_path 重新补建（H6）。
 *   · bits > 52 → init 返回 -EINVAL（statey 表只到 52）。
 *
 * ── 行存储 / 生命周期 ────────────────────────────────────────────────────
 *   彻底去掉「按行 alloc/free + 容器 insert/erase」：
 *     · 行 = `struct ravenfs_mrow_simple`，**恰好 64B**，内嵌在叶表里；
 *     · 表由注入 mem 面分配（零初始化契约），行生命周期随表；
 *     · 8K data 仍由注入 pool 面在装载时分配，淘汰/丢弃/卸载时归还；
 *     · 行内无锁，所有状态在单原子字 `ctrl` 上（state | flags | ref）。
 *   尾部节点：blk0 + 2 > total_blocks 时读/写 nblk = 1；缓冲零初始化保证
 *   尾部余块恒 0。
 *
 * ── ctrl 单原子字位域（⇄ god `mdata_piece_ctrl_t.ctrl_word`）──────────────
 *     bit [ 0,  8) state   IDLE / IO_ING / READY(⇄god working) / EVICTING
 *     bit [ 8, 24) flags   F_DIRTY / F_WB
 *     bit [24, 56) ref     活跃会话计数（u32）
 *     bit [56, 64) reserved 恒 0
 *   占坑（get ref++）与接管（reap/drop/flush 置 EVICTING）是**同一原子字**上的
 *   CAS —— 无 TOCTOU。state 还承担「叶表钉住」职责：表只会在**整表所有行均
 *   IDLE** 时被强淘汰摘除，而 locate 在片读锁内完成、命中行以 ref>0 / IO_ING /
 *   EVICTING 钉住叶表，因此 locate 拿到的行指针在本次操作期间始终有效；行 data
 *   的清空只发生在 IO_ING/EVICTING 独占态下。
 *
 * ── 锁纪律（不变量）─────────────────────────────────────────────────────
 *   · 结构锁只有片锁 `shard->lock`（可睡 rwsem）；行内无锁。
 *   · get 快路径：短持读锁 locate + CAS 占坑 → 命中即 ref++ 返回并放锁。
 *   · get 未命中：放读锁 → 片写锁 ensure_path + CAS IDLE→IO_ING → 放写锁 →
 *     pool.alloc + read_block（均在锁外）→ 片写锁发布 READY|ref=1。
 *     撞上 IO_ING/EVICTING 必须放锁，RFS_CPU_RELAX/RFS_YIELD 后重试；禁止持
 *     读锁忙等（H5 教训）。
 *   · 表结构分配（mem.alloc）允许发生在片写锁内（低频、可睡眠）；行 data 分配
 *     与块 I/O 必须在片锁外。
 *   · mark_dirty：**片读锁内** fetch_or F_DIRTY + 原子更新 wb_stamp —— 与
 *     wb_dirty / wb_node / flush_all 的片写锁互斥，回写清 DIRTY 期间不会被并发
 *     置脏穿透（H2 丢更新的回写防线）；调用方须先写 data 再标脏。
 *   · reap（普通 TTL）：**片读锁内** DFS + 行 CAS（READY/ref==0/干净/!F_WB/
 *     acc_stamp < cutoff）→ EVICTING，用行内 reserved[0] 串临时链表；放锁后
 *     归还 data、回 IDLE。不摘表（摘表需片写锁）。与 god `dfs_reap` 同构。
 *   · reap_expired_strong：两阶段 ——
 *       ① 行回收：逐片**片读锁**内 DFS + 行 CAS READY→EVICTING（PRESSURE 无视
 *          TTL；LOWLOAD 只收 acc_stamp < ttl_cutoff 的冷行），放锁后逐行处理：
 *          脏行先 mrow_store 写回，成功才归还 data/回 IDLE；失败恢复
 *          READY|DIRTY、保留 data、记 write_errors；
 *       ② 表精简：逐片**片写锁**内后序 DFS，仅当整表所有槽 state==IDLE 时释放
 *          该叶表/内部表，最后由根表为空则 sh->root = NULL（get 会重建）。
 *     两阶段之间允许并发 get/put；非空表绝不释放。CTRL_ARRAY 档不摘扁平数组。
 *   · wb_dirty / wb_node：持片写锁做 I/O（取舍：回写是低频维护动作，锁内 I/O
 *     换来「回写期间行不被改写/释放」的简单不变量；周期路径请用 reap / 分批）。
 *   · flush_all：先 writeback_all，再逐片清空全部行、归还 data；幂等。fini =
 *     flush_all + DFS 释放全部表 + 释放 shards。
 *
 * ── 有意偏离 ────────────────────────────────────────────────────────────
 *   · 无 no_cache / offload 后端：只落 self_reliat 的分片缓存语义（F1）。
 *   · 无写闸：ro 判定与动作触发归 FS 适配层（缓存只管做）。
 *   · 强淘汰已实现（两 gear + 空表回收），但**不做** god 写锁内 I/O：本实现把
 *     行回收的 I/O 放到片读锁之外，避免写锁跨 I/O（表精简仍用写锁）。
 *   · F_WB 位保留但本 WBS 不做 copy-out；wb 期间以片写锁独占代替。
 *   · `RAVENFS_MDATA_SHARD_SEL_BITS` 由旧值 7 提为 10（statey 表 w0 最大 10），
 *     只作诊断上界；实际 w0 逐档取自 statey。
 */
#ifndef _RAVENFS_MDATA_CACHE_CORE_H
#define _RAVENFS_MDATA_CACHE_CORE_H

#include "ravenfs_platform.h"	/* 类型 / 原子 / 锁 / 时间 / errno / 误差指针收口 */

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════ 常量 / 哨兵 ══════════════════════════════ */

/* 负载片几何：1 个 mdata 节点 = 2 个连续 4K FS_block（= 8192B）。*/
#define RAVENFS_MDATA_BLK_BYTES		4096
#define RAVENFS_MDATA_BLK_PER_NODE	2
#define RAVENFS_MDATA_NODE_BYTES	(RAVENFS_MDATA_BLK_BYTES * RAVENFS_MDATA_BLK_PER_NODE)

/* 档位边界：<= 13 走 CTRL_ARRAY；14..52 走 RADIX；> 52 拒绝。*/
#define RAVENFS_MDATA_CTRL_ARRAY_MAX_BITS	13
#define RAVENFS_MDATA_RADIX_MAX_BITS		52

/* statey 表最大层数（现表最长 6 层：1 分片位 + 5 树层；留 2 层冗余）。*/
#define RAVENFS_MDATA_MAX_LEVELS		8

/* 分片选择位宽上界（statey level_bits[0] 最大 10 → 最多 1024 片）。*/
#define RAVENFS_MDATA_SHARD_SEL_BITS		10

/* 两档模式。*/
enum ravenfs_mdata_mode {
	RAVENFS_MDATA_MODE_CTRL_ARRAY	= 0,	/* bits <= 13：单分片预分配叶表 */
	RAVENFS_MDATA_MODE_RADIX	= 1,	/* bits 14..52：statey + 多级静态基数树 */
};

/* 强淘汰力度（⇄ god `mdp_squeeze_gear_t`）。
 *   PRESSURE：内存压力驱动 —— 无视 TTL，所有 READY && ref==0 && !F_WB 行都回收；
 *   LOWLOAD ：低载主动 —— 只回收 acc_stamp < ttl_cutoff_ns 的冷行（热行保留）；
 *             ttl_cutoff_ns == 0 表示「无 TTL 过滤」（等价 PRESSURE 的行选择）。
 * 两 gear 都只动 ref==0 的行；脏行先写回再释放。*/
enum ravenfs_mdata_squeeze_gear {
	RAVENFS_MDATA_SQUEEZE_PRESSURE = 0,
	RAVENFS_MDATA_SQUEEZE_LOWLOAD  = 1,
};

/* 强淘汰结果（非 NULL out 由入口清零、出口填写）。*/
struct ravenfs_mdata_squeeze_res {
	u64	rows_reaped;		/* 成功回收到 IDLE 的行数 */
	u64	tables_freed;		/* 摘除的空叶表/内部表/根表数（CTRL_ARRAY 恒 0）*/
	u64	table_bytes_freed;	/* 摘除表结构字节数 */
	u64	write_errors;		/* 脏行写回失败数 */
};

/* ── 行控制器打包字 `ctrl`（rfs_atomic64_t）位域 ─────────────────────────── */
#define RAVENFS_MROW_STATE_SHIFT	0
#define RAVENFS_MROW_STATE_MASK		0xFFULL
#define RAVENFS_MROW_FLAGS_SHIFT	8
#define RAVENFS_MROW_FLAGS_MASK		0xFFFFULL
#define RAVENFS_MROW_REF_SHIFT		24
#define RAVENFS_MROW_REF_MASK		0xFFFFFFFFULL
#define RAVENFS_MROW_REF_ONE		(1ULL << RAVENFS_MROW_REF_SHIFT)

#define RAVENFS_MROW_IDLE		0UL	/* 未驻留 / 已归零 */
#define RAVENFS_MROW_IO_ING		1UL	/* 装载接管中（本进程持有装载权） */
#define RAVENFS_MROW_READY		2UL	/* 已就绪（⇄ god working） */
#define RAVENFS_MROW_EVICTING		3UL	/* 淘汰接管中 */

#define RAVENFS_MROW_F_DIRTY		(1u << 0)
#define RAVENFS_MROW_F_WB		(1u << 1)

#define RAVENFS_MROW_FLAG(bit)		((u64)(bit) << RAVENFS_MROW_FLAGS_SHIFT)

/* ══════════════════════════ 注入面（唯一出站依赖）══════════════════════════ */

/* (1) 结构体内存：shards 数组 / 基数树表 / 行（内嵌叶表）/ session。
 *     契约：alloc 返回**零初始化**内存；free 与 alloc 配对。复用平台 rfs_mem_alloc。*/
/* struct rfs_mem_alloc 由 ravenfs_platform.h 提供。*/

/* (2) 8K 节点缓冲池：零初始化、连续、可直映射访问；bytes 恒
 *     RAVENFS_MDATA_NODE_BYTES（内核可据 bytes 反推 order）。*/
struct ravenfs_mdata_pool {
	void	*(*alloc)(void *ctx, u64 bytes);
	void	 (*free)(void *ctx, void *addr, u64 bytes);
	void	 *ctx;
};

/* (3) 节点块 IO：单位 = 全局 FS_block 号；read 须读满 nblk×4K，write 须
 *     「返回即可见」。nblk 恒 ∈ {1,2}（尾部节点截断）。返回 0 / 负 -errno。*/
struct ravenfs_mdata_io {
	int	(*read_block)(void *ctx, u64 fs_blk, u64 nblk, void *buf);
	int	(*write_block)(void *ctx, u64 fs_blk, u64 nblk, const void *buf);
	void	*ctx;
};

/* (4) md_* 计数器钩子：无阻塞、无 IO、可持锁；未注入（inc == NULL）→ no-op。
 *
 * 字段与 god `include/ravenfs/mdata_piece_caches_stats.hpp` 的 `enum mdp_stat`
 * **逐项、同序对位**（A 装配 / B 会话 / C 表结构 / D 淘汰回写 / E 驻留 gauge），
 * 末尾 `RAVENFS_MD_DROP` 是 ko 独有的块复用防污染 drop 计数（mdp 无此入口）。
 * 增删字段必须同步三处：本枚举、`ravenfs_sb_stats.md_*`、super.c 名字表。*/
enum ravenfs_md_stat {
	/* ── A 装配 / 生命周期（5）── */
	RAVENFS_MD_INIT_OK = 0,
	RAVENFS_MD_INIT_FAIL,
	RAVENFS_MD_SLICES,
	RAVENFS_MD_INIT_CTRL_ARRAY,
	RAVENFS_MD_INIT_RADIX,

	/* ── B 会话（ops + 命中率；7）── */
	RAVENFS_MD_GET_HIT,
	RAVENFS_MD_GET_MISS,
	RAVENFS_MD_NODE_LOAD,
	RAVENFS_MD_RETRY,
	RAVENFS_MD_PUT,
	RAVENFS_MD_DIRTY,
	RAVENFS_MD_ERR,

	/* ── C 表结构（RADIX 分片树；3）── */
	RAVENFS_MD_TABLE_ALLOC_INTERNAL,
	RAVENFS_MD_TABLE_ALLOC_LEAF,
	RAVENFS_MD_TABLE_FREE,

	/* ── D 淘汰 / 回写（5）── */
	RAVENFS_MD_REAP,
	RAVENFS_MD_REAP_STRONG,
	RAVENFS_MD_NODE_WB,
	RAVENFS_MD_FLUSH_WB,
	RAVENFS_MD_FLUSH_DROP,

	/* ── E 驻留量（gauge 上下拆分；宿主作差；4）── */
	RAVENFS_MD_RESIDENT_ADD_BYTES,
	RAVENFS_MD_RESIDENT_DEL_BYTES,
	RAVENFS_MD_TABLE_ADD_BYTES,
	RAVENFS_MD_TABLE_DEL_BYTES,

	/* ── ko 扩展（不在 mdp 组）── */
	RAVENFS_MD_DROP,

	RAVENFS_MD_STAT_COUNT
};

struct ravenfs_mdata_stat {
	void	(*inc)(void *ctx, u32 field, u64 delta);
	void	*ctx;
};

#define RAVENFS_MD_STAT(h, field, delta)				\
	do {								\
		const struct ravenfs_mdata_stat *__rfs_h = &(h);	\
		if (__rfs_h->inc)					\
			__rfs_h->inc(__rfs_h->ctx, (u32)(field),	\
				     (u64)(delta));			\
	} while (0)

#define RAVENFS_MD_STAT1(h, field)	RAVENFS_MD_STAT((h), (field), 1)

/* ══════════════════════════ 装配配置 ══════════════════════════ */
struct ravenfs_mdata_cfg {
	u64				total_blocks;
	struct rfs_mem_alloc		mem;
	struct ravenfs_mdata_pool	pool;
	struct ravenfs_mdata_io		io;
	struct ravenfs_mdata_stat	stat;
};

/* ══════════════════════════ 几何（纯计算，可单测）══════════════════════════ */
struct ravenfs_mdata_geom {
	u32	mode;		/* enum ravenfs_mdata_mode */
	u32	bits;		/* ceil_log2(n_nodes)，>= 1 */
	u64	n_nodes;	/* DIV_ROUND_UP(total_blocks, 2) */
	u32	n_slices;	/* CTRL_ARRAY = 1；RADIX = 1 << w0 */
	u32	w0;		/* 分片选择位宽（CTRL_ARRAY = 0） */
	u32	shard_bits;	/* bits - w0（片内键位宽） */
	u32	total_levels;	/* statey 层数（含分片选择位；CTRL_ARRAY = 1） */
	u32	tree_levels;	/* total_levels - 1（CTRL_ARRAY = 0） */
	u32	leaf_width;	/* 叶表槽数 */
	u8	level_bits[RAVENFS_MDATA_MAX_LEVELS];
	u32	shift[RAVENFS_MDATA_MAX_LEVELS];	/* 树层 j 的右移位数 */
};

/* 由 total_blocks 推导两档几何。0 / 负 -errno（bits > 52 → -EINVAL）。*/
int	ravenfs_mdata_geometry(u64 total_blocks,
			       struct ravenfs_mdata_geom *out);

/* ══════════════════════════════ 行 / 片 / 缓存 ══════════════════════════════ */

/* 一个 mdata 缓存行：恰好 64B（= 1 条 cache line），内嵌在叶表中。
 * 分配契约：所在叶表由注入 mem.alloc 取得且零初始化 → ctrl == IDLE(0)，
 * data == NULL。reserved[0] 仅在 EVICTING 态被 reap 临时用作链表 next。*/
struct ravenfs_mrow_simple {
	void		*data;		/* 8B：8K 节点缓冲（pool 面取得） */
	rfs_atomic64_t	ctrl;		/* 8B：state|flags|ref 单原子打包字 */
	rfs_atomic64_t	acc_stamp;	/* 8B：最近访问（ns）→ TTL 判据 */
	rfs_atomic64_t	wb_stamp;	/* 8B：回写 aging（原子：mark_dirty 与 wb 可并发） */
	u64		node_idx;	/* 8B：节点号 = fs_blk >> 1 */
	u64		reserved[3];	/* 24B：补齐 64B；[0] 为 reap 临时链表指针 */
};

#ifndef __cplusplus
_Static_assert(sizeof(struct ravenfs_mrow_simple) == 64,
	       "ravenfs_mrow_simple must be exactly 64B (one cache line)");
#else
static_assert(sizeof(struct ravenfs_mrow_simple) == 64,
	      "ravenfs_mrow_simple must be exactly 64B (one cache line)");
#endif

/* 一个分片：一把片锁 + 一棵静态基数树根（或 CTRL_ARRAY 的叶表）。*/
struct ravenfs_mdata_shard {
	rfs_rwsem_t	lock;	/* 片锁（可睡）；本模块唯一结构锁 */
	void		*root;	/* CTRL_ARRAY: row[]；RADIX: 根表（懒建） */
};

/* mdata 节点缓存本体（本头所有接口的 arg0）。*/
struct ravenfs_mdata_cache {
	struct ravenfs_mdata_shard	*shards;	/* [n_slices] */
	u32		n_slices;
	u32		w0;
	u32		shard_bits;
	u32		bits_len;	/* node-count-based ceil_log2(n_nodes) */
	u32		mode;		/* enum ravenfs_mdata_mode */
	u32		total_levels;
	u32		tree_levels;
	u32		leaf_width;	/* 叶表槽数 = 1 << level_bits[last] */
	u8		level_bits[RAVENFS_MDATA_MAX_LEVELS];
	u32		shift[RAVENFS_MDATA_MAX_LEVELS];
	u64		node_mask;	/* 片内键掩码 = 2^shard_bits - 1 */
	u64		total_blocks;
	u64		n_nodes;

	/* 注入面快照（init 从 cfg 拷入，运行期只读） */
	struct rfs_mem_alloc		mem;
	struct ravenfs_mdata_pool	pool;
	struct ravenfs_mdata_io		io;
	struct ravenfs_mdata_stat	stat;
};

/* 节点租借会话。*/
struct ravenfs_msession {
	struct ravenfs_mdata_cache	*cache;
	struct ravenfs_mdata_shard	*shard;
	struct ravenfs_mrow_simple	*row;
};

/* ══════════════════════════════ 生命周期 ══════════════════════════════ */

/* 装配：消费 cfg → 推导几何 → 建 shards 数组 + 逐片锁。CTRL_ARRAY 档同时
 * 预分配 n_nodes 行的叶表。self 由调用方提供且零初始化；本函数不释放 self。
 * 失败自清理返负 -errno；重复 init（已装配）→ -EINVAL；bits > 52 → -EINVAL。*/
int	ravenfs_mdata_cache_init(struct ravenfs_mdata_cache *self,
				 const struct ravenfs_mdata_cfg *cfg);

/* 析构：flush_all（脏先落盘）→ DFS 释放全部表 → 释放 shards。不释放 self。幂等。*/
void	ravenfs_mdata_cache_fini(struct ravenfs_mdata_cache *self);

/* ═══════════════════════ 会话（节点租借）═══════════════════════ */

/* 取节点：命中 ref++；未命中 IO_ING 占位后在锁外装载。失败 RFS_ERR_PTR(负 errno)。
 *
 * ⚠️ FS 调用点**禁止裸写 get/put**：一律用文件末尾的 RAII 宏
 *    RAVENFS_MDATA_SESSION()（或 _NULL() + _OPEN()），由 cleanup 自动 put。
 *    直接调用仅供核心内部实现 / RAII 宏展开使用。*/
struct ravenfs_msession *ravenfs_mdata_get(struct ravenfs_mdata_cache *self, u64 blk);

/* 还节点：ref--（下溢保护）；释放会话结构。NULL 容忍。
 * ⚠️ 同上：调用点用 RAII 宏 / _RELEASE()，不要裸写本函数。*/
void	ravenfs_mdata_put(struct ravenfs_msession *s);

/* 丢弃节点块缓存：ref==0 → 归还 data + 回 IDLE；ref>0 → 清 F_DIRTY；
 * IO_ING/EVICTING 跳过。越界容忍 no-op。*/
void	ravenfs_mdata_drop(struct ravenfs_mdata_cache *self, u64 blk);

/* 读一个 8K 节点到调用方缓冲。0 / 负 errno。*/
int	ravenfs_mdata_read(struct ravenfs_mdata_cache *self, u64 blk, void *buf);

/* 会话节点缓冲直址。*/
static inline void *ravenfs_msession_node(struct ravenfs_msession *s)
{
	return s->row->data;
}

/* ═══════════════════════ 回写 / 淘汰 ═══════════════════════ */

/* 回写脏行：READY && DIRTY && !WB && (ignore_age || wb_stamp < cutoff)。*/
int	ravenfs_mdata_wb_dirty(struct ravenfs_mdata_cache *self,
			       u64 age_cutoff_ns, bool ignore_age);

/* 全量回写 = wb_dirty(ignore_age = true)。*/
int	ravenfs_mdata_writeback_all(struct ravenfs_mdata_cache *self);

/* 单节点回写（fsync）：只把指定块所在行（若驻留且脏）落盘。*/
int	ravenfs_mdata_wb_node(struct ravenfs_mdata_cache *self, u64 blk);

/* TTL 淘汰：**片读锁**内 DFS + 行 CAS，回收 READY && ref==0 && !DIRTY && !WB &&
 * acc_stamp < cutoff 的行，返回回收行数。不摘表（表精简只走 strong）。*/
long	ravenfs_mdata_reap_expired(struct ravenfs_mdata_cache *self, u64 cutoff_ns);

/* 强淘汰（低频重操作，勿放周期路径）：两个 gear 语义见
 * enum ravenfs_mdata_squeeze_gear。流程 = 行回收（片读锁 + 行 CAS，锁外写回/
 * 归还 data）→ 表精简（片写锁 + 后序 DFS，仅摘全空表）。
 *   · out 可为 NULL；非 NULL 时入口清零、出口填结果；
 *   · 脏行写回失败：行恢复 READY|DIRTY 且保留 data，write_errors++，继续处理；
 *   · 有写回失败时返回 -EIO，否则 0；非法 gear / NULL self → -EINVAL。
 * 成功回收行累加 RAVENFS_MD_REAP_STRONG，脏行写回成功累加 RAVENFS_MD_NODE_WB。
 * 对外 ioctl 契约见 `Docs/linux/ravenfs-linux-ko-mdata-cache-ioctl-spec.md`。*/
int	ravenfs_mdata_reap_expired_strong(struct ravenfs_mdata_cache *self,
					  u32 gear,
					  u64 ttl_cutoff_ns,
					  struct ravenfs_mdata_squeeze_res *out);

/* 卸载 / 同步点：先 writeback_all，再逐片清空全部行、归还 data。幂等。*/
void	ravenfs_mdata_flush_all(struct ravenfs_mdata_cache *self);

/* 丢弃全部行（**不回写**）：ro 卸载 / 显式弃用。只清行 data 回 IDLE，不释放表；
 * 通常后接 fini（此时 writeback_all 已无脏行可写）。 */
void	ravenfs_mdata_discard_all(struct ravenfs_mdata_cache *self);

/* ══════════════════════════ 几何 / 审计 ══════════════════════════ */
u64	ravenfs_mdata_total_blocks(const struct ravenfs_mdata_cache *self);
u32	ravenfs_mdata_shard_count(const struct ravenfs_mdata_cache *self);

/* ══════════════════════════════ 标脏（外部写点）══════════════════════════════ */
/* 会话标脏：**片读锁内** fetch_or F_DIRTY + 更新 wb_stamp。
 * 与 wb_dirty / wb_node / flush_all 的片写锁互斥 —— 回写清 DIRTY 期间不会被
 * 并发置脏穿透（H2 丢更新在回写路径上的防线）。调用方须先把新内容写入
 * `ravenfs_msession_node(ms)`，再调用本函数（读锁释放提供 happens-before）。
 * 注意：本函数会获取可睡眠片读锁；不要在同片写锁内调用。*/
void	ravenfs_mdata_mark_dirty(struct ravenfs_msession *ms);

#define RAVENFS_MDATA_DIRTY(ms)	ravenfs_mdata_mark_dirty(ms)

/* ═══════════════════ 会话 RAII（⇄ god `mdata_piece_session`）═══════════════════
 * C++ 原型用「构造 get / 析构 put」把引用计数绑在会话对象生命周期上；本核心是
 * C，用 GCC/Clang 的 `__attribute__((cleanup))` 得到同一语义：
 *
 *   RAVENFS_MDATA_SESSION(ms, self, blk);
 *   if (!RAVENFS_MDATA_SESSION_VALID(ms))     // NULL 或 ERR_PTR
 *           return ...;                       // 出作用域自动 put（错误指针安全跳过）
 *   p = RAVENFS_MDATA_SESSION_DATA(ms);       // == ravenfs_msession_node(ms)
 *   ...
 *   RAVENFS_MDATA_SESSION_DIRTY(ms);          // 内容写完后标脏
 *
 * 语义与纪律：
 *   · 变量必须是 `struct ravenfs_msession *` 左值；作用域退出（**含 return /
 *     break / continue / goto**）自动 `ravenfs_mdata_put()`；
 *   · 宏展开为**声明**，必须放在块（`{}`）内，不可作 braceless if/for 的
 *     单语句；
 *   · `ravenfs_mdata_get()` 失败返回 ERR_PTR → cleanup 判错跳过，不会解引用；
 *   · 需要把会话所有权转交（如 inode/name_data 会话缓存）→ `..._DISARM(var)`
 *     清零指针后再赋给所有者，出作用域不再 put；
 *   · 需要提前归还（复用变量 / 先释放再重取）→ `..._RELEASE(var)`；重新取用
 *     `..._RESET(var, self, blk)`（先 release 再 get）；
 *   · 多会话 / 带 `goto out` 的函数：先用 `..._NULL(var)` 在函数开头声明空守卫，
 *     再在取用点 `..._OPEN(var, self, blk)` —— 保证任何跳转路径上的 cleanup
 *     都不会碰未初始化指针；
 *   · 禁止对同一变量重复 `RAVENFS_MDATA_SESSION()`（重复声明）；变量已存在时
 *     用 `..._RESET()`；
 *   · cleanup 只做 put，**不**做隐式回写：脏会话仍按核心回写/淘汰路径处理
 *     （对应 C++ session 析构里的 self_reliat_put 语义）。
 */

/* cleanup 回调：只释放「有效会话」；NULL / ERR_PTR 一律跳过。*/
static inline void ravenfs_msession_cleanup(struct ravenfs_msession **sp)
{
	if (!sp || RFS_IS_ERR_OR_NULL(*sp))
		return;
	ravenfs_mdata_put(*sp);
	*sp = NULL;
}

/* 声明 + 获取（作用域退出自动 put）。*/
#define RAVENFS_MDATA_SESSION(var, self, blk)				\
	struct ravenfs_msession *var					\
		__attribute__((__cleanup__(ravenfs_msession_cleanup),	\
			       __unused__)) =				\
		ravenfs_mdata_get((self), (blk))

/* 先声明空守卫、稍后再取（供多会话 / goto-out 函数）：初始 NULL 保证任何
 * 跳转路径上的 cleanup 都不会碰未初始化指针。*/
#define RAVENFS_MDATA_SESSION_NULL(var)					\
	struct ravenfs_msession *var					\
		__attribute__((__cleanup__(ravenfs_msession_cleanup),	\
			       __unused__)) = NULL

#define RAVENFS_MDATA_SESSION_OPEN(var, self, blk)			\
	((var) = ravenfs_mdata_get((self), (blk)))

/* 提前归还（幂等；NULL / ERR_PTR 安全）。*/
#define RAVENFS_MDATA_SESSION_RELEASE(var)				\
	do {								\
		struct ravenfs_msession *__rfs_ms__ = (var);		\
		(var) = NULL;						\
		if (!RFS_IS_ERR_OR_NULL(__rfs_ms__))			\
			ravenfs_mdata_put(__rfs_ms__);			\
	} while (0)

/* 重新取会话：先归还当前值，再 get 新块（不重新声明变量）。*/
#define RAVENFS_MDATA_SESSION_RESET(var, self, blk)			\
	do {								\
		RAVENFS_MDATA_SESSION_RELEASE(var);			\
		(var) = ravenfs_mdata_get((self), (blk));		\
	} while (0)

/* 所有权转交：清零指针，出作用域不再 put（调用方接管释放）。*/
#define RAVENFS_MDATA_SESSION_DISARM(var)	do { (var) = NULL; } while (0)

/* 会话可用性 / 数据直址 / 标脏（⇄ C++ `operator bool` / `data` / `dirty()`）。*/
#define RAVENFS_MDATA_SESSION_VALID(ms)	(!RFS_IS_ERR_OR_NULL(ms))
#define RAVENFS_MDATA_SESSION_DATA(ms)	ravenfs_msession_node(ms)
#define RAVENFS_MDATA_SESSION_DIRTY(ms)	ravenfs_mdata_mark_dirty(ms)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_MDATA_CACHE_CORE_H */

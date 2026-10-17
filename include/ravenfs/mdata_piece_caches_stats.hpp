#pragma once
/* ═══════════════════════════════════════════════════════════════════════════
 * mdata_piece_caches_stats.hpp — 8K 元数据负载片缓存（mdata_piece_caches_mgr）
 *                               计数器：**钩子 + 字段枚举**
 *
 * 设计对齐 `include/linux/ravenfs_allocator_stats.h`（al_ 组）：
 *   · 模块**平台中立**、不碰任何宿主设施（god 不碰 FS / 日志 / 全局计数；ko 不碰内核）；
 *   · 计数器经**注入的 `struct mdp_stat_hook`** 上报：本模块只调钩子，宿主（god 编排层 /
 *     FS 适配层）把 `(field, delta)` 累加进自己的统计面（⇄ ko 把 md_ 累加进 sbi->stats）。
 *   · 未注入（`inc == nullptr`）→ **no-op**（用户态算法单测 / 压测无需打点）。
 *
 * 两条不变式（与全模块打点纪律同源，实现必须守）：
 *   P1 打点**不改控制流**：钩子只累加，返回值不参与任何分支判定；
 *   P2 **只增不减**：钩子只收 `delta ≥ 0` 的累计量。gauge 类（驻留字节）拆成
 *      「上 / 下」两字段，宿主自行作差得实时量，模块不维护有符号增量。
 *
 * 并发契约：埋点位于片锁 / 读锁临界区（可能与 get/reap 并发）→ 实现的 `inc`
 *   **必须无阻塞、无 IO、可持锁调用**（只碰原子）；不得回调进本模块 / 不得睡眠。
 *
 * 字段命名 `mdp_<对象>_<动作/结果>`：本头为**唯一真相源**；枚举下标即输出顺序
 *   （名字表逐一对位），增删须同改名字表。ko 侧 `ravenfs_md_stat`（md_ 组）是本组的
 *   移植子集，两处**同源**（新增字段须两端同步）。
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 字段枚举（mdp_ 组）───────────────────────────────────────────────────────
 * 分组：A 装配/生命周期 · B 会话 · C 表结构 · D 淘汰/回写 · E 驻留量（gauge 上下）。
 * **顺序即输出顺序**（宿主名字表逐一对位）；增删须同改两处。 */
enum mdp_stat : uint32_t {
	/* ── A 装配 / 生命周期（5）── */
	MDP_INIT_OK = 0,	/* ctor_self_reliat 成功次数 */
	MDP_INIT_FAIL,		/* ctor 失败次数（退化配置 / statey 解析 / 装配 OOM）*/
	MDP_SLICES,		/* 装配出的分片数（每次成功 init 累加 n_slices_）*/
	MDP_INIT_CTRL_ARRAY,	/* 落到 CTRL_ARRAY 档（bits ≤ 13）次数 */
	MDP_INIT_RADIX,		/* 落到 RADIX 档（bits > 13）次数 */

	/* ── B 会话（ops + 命中率；7）── */
	MDP_GET_HIT,		/* get 快路径命中（working 占坑成功）*/
	MDP_GET_MISS,		/* get 未命中：拿到加载权（走盘读）*/
	MDP_NODE_LOAD,		/* 节点盘读成功次数（read_block 返回 OK）*/
	MDP_RETRY,		/* io_ing/evicting 让位重试（含 miss 建链后重定位）*/
	MDP_PUT,		/* put（accessor-1）调用次数 */
	MDP_DIRTY,		/* mark_dirty 置脏次数 */
	MDP_ERR,		/* 内部错误（OOM / read 失败 / 建链失败）*/

	/* ── C 表结构（RADIX 分片树；3）── */
	MDP_TABLE_ALLOC_INTERNAL,	/* 中间表（指针槽数组）分配次数 */
	MDP_TABLE_ALLOC_LEAF,		/* 叶表（ctrl_t 数组）分配次数 */
	MDP_TABLE_FREE,			/* 空表回收次数（strong reap 摘表）*/

	/* ── D 淘汰 / 回写（5）── */
	MDP_REAP,		/* TTL 淘汰回收的行数（reap_expired）*/
	MDP_REAP_STRONG,	/* 强淘汰回收的行数（reap_expired_strong）*/
	MDP_NODE_WB,		/* 脏行写回次数（节点数；reap/reap_strong 原地回写）*/
	MDP_FLUSH_WB,		/* flush_all 脏行写回次数 */
	MDP_FLUSH_DROP,		/* flush_all 非脏行丢弃次数 */

	/* ── E 驻留量（gauge 上下拆分；宿主作差；4）── */
	MDP_RESIDENT_ADD_BYTES,	/* 行驻留累计 +8192/次加载 */
	MDP_RESIDENT_DEL_BYTES,	/* 行释放累计 8192/次淘汰或丢弃 */
	MDP_TABLE_ADD_BYTES,	/* 表结构内存累计增长（字节）*/
	MDP_TABLE_DEL_BYTES,	/* 表结构内存累计回收（字节）*/

	MDP_STAT_COUNT		/* 哨兵：字段总数（勿作计数器用）*/
};

/* ── 注入钩子 ─────────────────────────────────────────────────────────────────
 * `inc(ctx, field, delta)`：把 `field` 计数器 +`delta`。**必须无阻塞、无 IO、
 * 可持锁调用**（埋点位于锁临界区），实现侧只碰原子。ctx = 宿主上下文
 * （god = 编排层句柄；ko = super_block*）。未注入（inc == NULL）→ no-op。*/
struct mdp_stat_hook {
	void	(*inc)(void *ctx, uint32_t field, uint64_t delta);
	void	*ctx;
};

#ifdef __cplusplus
} /* extern "C" */
#endif

/* 统一上报宏：未注入即 no-op；**不参与控制流**（P1）。C / C++ 通用。*/
#define MDP_STAT(h, field, delta)					\
	do {								\
		const struct mdp_stat_hook *__mdp_h = &(h);		\
		if (__mdp_h->inc)					\
			__mdp_h->inc(__mdp_h->ctx, (uint32_t)(field),	\
				     (uint64_t)(delta));		\
	} while (0)

#define MDP_STAT1(h, field)	MDP_STAT((h), (field), 1)

/* 空钩子（未注入）。C++ 侧可用 `{}` 值初始化替代。*/
#ifdef __cplusplus
namespace ravenfs {
inline constexpr mdp_stat_hook kMdpStatHookNull{ nullptr, nullptr };
}
#endif

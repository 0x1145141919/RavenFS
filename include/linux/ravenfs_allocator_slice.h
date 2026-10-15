/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_allocator_slice.h —— 分片自治单元 struct rfs_slice（⇄ god mgr_slice_t）
 *
 * 定位：god 用户态原型 `juvenile_god/fs_allocator.cpp` 里 `mgr_slice_t` 的 **C 语言
 *   逐接口语义翻译**。内部组合 = 主从双侵入红黑树：
 *     · 主树 by_pos ：严格区间序（见下），key = 区间自身；户籍 / 真相源
 *     · 从树 by_size： (len, base) 字典序；派生索引，供按尺寸查（best-fit / 碎片优先）
 *   同一 `struct rfs_interval` 双挂两棵树（by_pos / by_size 两个侵入钩子）——
 *   不变式：**从 ⊆ 主**（size 树的每个节点都在 pos 树内）。
 *
 * 平台纪律：只 include `ravenfs_platform.h` + `ravenfs_rbtree_intrusive.h` +
 *   `ravenfs_run_list.h`；**无** `#ifdef __KERNEL__`、无内核头。C(gnu11) 与
 *   C++(c++17) 双编 `-Wall -Wextra -Werror` 均须通过（故 void* 一律显式 cast）。
 *
 * ── 两棵树的一致性纪律（逐指令边界维持「从 ⊆ 主」）──────────────
 *   挂：主树先（拿到稳定元素地址）→ 从树后；
 *   摘：从树先（钩子归零）→ 主树后（析构元素）。
 *   本头元素内存全由 `struct rfs_slice_alloc` 元素池取/还，**轮子内零直接分配**。
 *
 * ── 主树比较器（严格区间序）────────────────────────────
 *   a < b  ⟺ a.base + a.len <= b.base
 *   a > b  ⟺ b.base + b.len <= a.base
 *   否则（相交非空）cmp == 0。
 *   性质（利用之）：对**两两不重叠**的区间集，它等价于 base 升序（中序序列一致）；
 *   额外效果 = 插入一个与既有区间相交的区间 → cmp==0 → `rfs_rb_insert` 返 -EEXIST
 *   —— 即 free_interval 的「重叠检测」天然获得（与 god 撞重叠报错等价）。
 *   ⚠ 相交关系不传递；仅当「树内区间两两不重叠」时才是合法全序 —— enable /
 *     free_interval 必须维护此不变量。
 *
 * ⚠ 契约缺口（实现必需，已在此补字段，**请设计方裁**）：
 *   任务给出的 `struct rfs_slice` 字段清单**未含元素池分配器**，但元素池既在
 *   enable 时绑定，后续 free_interval / free_intervals / soft_* / shutdown 都必须
 *   向池**还**节点（god 里树自持节点，无此问题；ko 把分配外抛后必须留存 alloc 才能
 *   归还）。故本头在 struct rfs_slice 里增列 `struct rfs_slice_alloc alloc;`（enable
 *   时从 cfg 拷入、跨调用保留）。若设计方不接受，替代方案 = 把这些方法的签名都补
 *   分配器入参（破坏既定签名）。详见交付报告「契约问题 #1」。
 *
 * 返回码（= ravenfs_bitmap_core.h 口径）：0 / 负 -errno
 *   失败 -ENOSPC · 重叠 -EEXIST · 未找到 -ENOENT · 越界/状态 -EINVAL。
 *
 * 锁纪律（⇄ god）：除 free_count 外**调用方须持 self->lock**（可睡互斥锁）；本文件
 *   所有函数内部**不加锁**、片内零主动 IO。
 */
#ifndef _RAVENFS_ALLOCATOR_SLICE_H
#define _RAVENFS_ALLOCATOR_SLICE_H

#include "ravenfs_platform.h"		/* 类型 / 内存 / 锁（唯一平台收口）*/
#include "ravenfs_rbtree_intrusive.h"	/* 侵入式 RB 核（零分配）*/
#include "ravenfs_run_list.h"		/* STL 式 run 链（容器持有 + 绑定分配器）*/
#include "ravenfs_allocator_stats.h"	/* al_ 计数器钩子（注入式；片内埋点）*/

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════ 区间元素 ══════════════════════════ */

/* 一个自由区间：双挂主 / 从两棵侵入树（同一对象两个钩子 → 从 ⊆ 主）。*/
struct rfs_interval {
	struct rfs_rb_node	by_pos;		/* 主树钩子（严格区间序）*/
	struct rfs_rb_node	by_size;	/* 从树钩子（(len,base) 序）*/
	u64			base;		/* 全局位号起点 */
	u64			len;		/* 长度（位）*/
};

/* ══════════════════════════ 元素池分配回调 ══════════════════════════ */

/* ko 把 god `std::set` 的内部分配外抛。轮子内零直接分配。*/
struct rfs_slice_alloc {
	void	*(*alloc)(void *ctx, u64 size);	/* 取区间节点；失败返 NULL */
	void	 (*free)(void *ctx, void *p);	/* 还区间节点 */
	void	 *ctx;				/* 宿主上下文（可 NULL）*/
};

/* ══════════════════════════ 片配置（⇄ slice_config_t）══════════════════════════ */

/* ⇄ god slice_config_t（ko 增补 alloc）。1 = 占用 / 0 = 空闲。*/
struct rfs_slice_cfg {
	u8	*slice_base;		/* 片位图内存（4K 对齐；enable 校验）*/
	u64	 in_global_base_idx;	/* 本片在全局位号系的起点 */
	u64	 slice_bitscount;	/* 本片位数 */
	struct rfs_slice_alloc alloc;	/* 建树 / 融合取还区间节点 */
	struct rfs_stat_hook stat;	/* al_ 计数器钩子（router 下传；未注入 = no-op）*/
};

/* ══════════════════════════ 片自治单元（⇄ mgr_slice_t）══════════════════════════ */

struct rfs_slice {
	bool			is_workable;	/* false→true: enable；true→false: shutdown */
	u64			in_global_base_idx;
	u64			slice_bitscount;
	struct rfs_slice_alloc	alloc;		/* 元素池（enable 时从 cfg 拷入，跨调用保留）*/
	struct rfs_stat_hook	stat;		/* al_ 计数器钩子（enable 时从 cfg 拷入）*/
	struct rfs_rb_root	pos;		/* 主树 by_pos（户籍 / 真相源）*/
	struct rfs_rb_root	size;		/* 从树 by_size（派生索引，从 ⊆ 主）*/
	u64			last_finish_hint;/* 分配游标（只进不退）*/
	u64			free_count;	/* 空闲位数（增量维护）*/
	bool			dirty;		/* D-15 片内隐状态：自上次落盘以来空闲区间集已变 */
	rfs_mutex_t		lock;		/* 片锁（可睡）；调用方持有 */
};

/* ══════════════════════════ 方法（⇄ mgr_slice_t）══════════════════════════
 * 除 free_count 外，调用方须持 self->lock；返回 0 / 负 -errno。
 */

/* enable：校验 → 扫位图建极大空闲 run → 每 run 双挂主从树；成功后 workable。*/
int	rfs_slice_enable(struct rfs_slice *self, const struct rfs_slice_cfg *cfg);

/* shutdown：true→false，拆双树 + 还全部区间节点 + 清账；初始化态调用 -EINVAL。*/
int	rfs_slice_shutdown(struct rfs_slice *self);

/* free_count：读空闲位数（唯一免锁接口）。*/
u64	rfs_slice_free_count(const struct rfs_slice *self);

/* dirty_take：取「自上次落盘以来空闲区间集已变」标志并清除（读-改-清，调用方持锁）。
 * 返回 true = 该片变过（checkpoint 应重建 / 回写其位图块）。
 * 「隐状态」语义（D-15）：`dirty` 由**片内**变更点（alloc_hard / soft_alloc /
 *   soft_occupy / free_interval 中 `free_count` 更新处）置位，`enable` 建树后归零
 *   （enable = 载入盘面 = 已持久基线）。故「是否需落盘」是片自己的属性，不靠外层
 *   逐接口枚举 / 注入钩子。清标志在**重建之前**取（见 FS 侧 checkpoint）：期间并发
 *   变更会重新置位 → 下轮再写，绝不丢更新。*/
bool	rfs_slice_dirty_take(struct rfs_slice *self);

/* alloc_hard：best-fit（从树 len≥bitscount 最小者）+ 头部上挖，不部分分配。*/
int	rfs_slice_alloc_hard(struct rfs_slice *self, u8 align_log2, u64 bitscount,
			     u64 *out_base_idx);

/* soft_alloc：从树小端逐段消费（碎片优先），允许部分交付；push_front 构建 out_runs。*/
int	rfs_slice_soft_alloc(struct rfs_slice *self, u8 align_log2, u64 bitscount,
			     struct rfs_run_list *out_runs, bool *all_satisfied);

/* soft_occupy：base 起贪心认领连续空闲前缀；*rest_bits 入=已满足量、出=缺口。*/
int	rfs_slice_soft_occupy(struct rfs_slice *self, u64 base, u64 len,
			      u64 *rest_bits);

/* free_interval：重叠检测（-EEXIST）→ 邻接融合 → 主从同步；free_count += len。*/
int	rfs_slice_free_interval(struct rfs_slice *self, u64 base, u64 len);

/* free_intervals：消费 run 链（逐段取走、取走即还节点），逐段 free_interval。*/
int	rfs_slice_free_intervals(struct rfs_slice *self, struct rfs_run_list *runs);

/* test_interval：断言 [base, base+len) 全空闲（val=false）/ 全占用（val=true）。*/
int	rfs_slice_test_interval(struct rfs_slice *self, bool val, u64 base, u64 len);

/* all_dump：全部空闲区间按 base 递增序 push_back 进 out_runs。*/
int	rfs_slice_all_dump(struct rfs_slice *self, struct rfs_run_dlist *out_runs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_ALLOCATOR_SLICE_H */

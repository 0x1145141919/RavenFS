/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ravenfs_allocator_top.h —— FS_block / inode 分配器「树版」顶层接口（C 版 OOP）
 *
 * 定位：god 用户态原型 `include/ravenfs/fs_allocator.hpp`（`fs_allocator_t` +
 *   `mgr_slice_t`）的 **C 语言逐接口语义翻译**，供 ravenfs.ko 运行时分配器 v2 用。
 *   —— 只落**接口/语义**；实现（.c）后补。本头是设计与辨析面，不是实现。
 *
 * ⚠ 边界（先钉死，决定「变什么、不变什么」）：
 *   · **磁盘契约 = 位图**（磁盘不放树；见 `Docs/runtime_tech/04_sharded_master_slave_rbtree.md` §3）。
 *   · **运行时 = 多分片主从红黑树**（易失、可重建、不承担持久化）。
 *   · 两层唯一缝合面：挂载 `位图 → 重建树`；写回 `树 → 补集成位图`。
 *   ⇒ 本头替换的是**运行时原语**（旧 `struct rfs_bm`，位图时代）；**磁盘格式不动**。
 *   ⇒ 与位图时代原语**不兼容**：真值表示、交付单位、内存/锁模型、持久化出口全变。
 *
 * ── god → ko 名字对照（逐接口）──────────────────────────────────────────────
 *   fs_allocator_t            → struct rfs_allocator
 *   mgr_slice_t               → struct rfs_slice（**本头只前向声明**；数据结构+接口另立头文件）
 *   fs_run_t                  → struct rfs_run
 *   fs_run_list_t             → struct rfs_run_list   （前插单链，调用方持有）
 *   fs_run_dlist_t            → struct rfs_run_dlist  （尾插，all_dump 出参）
 *   slice_config_t            → struct rfs_slice_cfg（随片头文件）
 *   slices_cfg_t              → struct rfs_slices_cfg
 *   group_t                   → struct rfs_alloc_group
 *   fs_interval_t / 双树别名  → **随片头文件**（不透明句柄；设施选型见 D2）
 *   result_code_t             → int（0 / 负 -errno，ko 口径）
 *   std::mutex                → rfs_mutex_t（平台收口；可睡）
 *
 * ── 与 god 的**有意偏离**（fork，待设计方逐条裁）────────────────────────────
 *   F1 树设施（归片头）：god 用 `std::set` + Boost.Intrusive；ko 无 STL/Boost。设施选型 =
 *      D2（内核 rbtree / maple / 自研），**随 `struct rfs_slice` 的独立头设计**，本头不展开。
 *   F2 树按值 vs 按指针：god 的 slice 按值内嵌两棵树 —— 随片头定，本头无关。
 *   F3 返回码：god `result_code_t`（RC_OK / RC_ERR_*）→ ko `int`（0 / 负 -errno），
 *      与 `ravenfs_bitmap_core.h` 同口径；映射见 §返回码。
 *   F4 run 链承载：god `std::forward_list`/`std::list`（容器拥有节点、push 即分配）→ ko
 *      **自造 STL 式轮子** `ravenfs_run_list.h`（内核 list 家族全侵入式、无对等物）。
 *      `rfs_run` 保持**纯 payload**，节点由 `rfs_run_node` 承载、分配绑定在容器上。
 *   F5 落盘面（**已裁决**）：god `all_syn()` 因 cfg 无 IO 面而 RC_ERR_NOT_SUPPORTED。
 *      ko 裁决：**落盘不属本模块** —— 本模块只出**纯快照**（逐片 dump 空闲区间）；
 *      「补集成位图字 + 盘上几何 + IO」归 **FS 适配层**（它已持位图几何与 IO）。见 §持久化出口。
 *   F6 装载粒度：god ctor 一次喂**全量**位图（峰值 = 全量位图 + 树）；ko 建议
 *      **逐片喂 → 建树 → 立即弃位图**（待 D8）。
 *   F7 `get_free_count`/`slice_count` 等 god 里混在 public 段的取值函数，C 里统一
 *      用 `const self` 只读。
 */
#ifndef _RAVENFS_ALLOCATOR_TOP_H
#define _RAVENFS_ALLOCATOR_TOP_H

#include "ravenfs_platform.h"	/* 类型 / 内存 / 锁 / RAII 守卫（唯一平台收口） */
#include "ravenfs_run_list.h"	/* run 三型 + 「STL 式自造链轮子」（容器拥有节点 + 绑定分配器）*/

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════ 常量 / 哨兵 ══════════════════════════════ */

/* 片下限：1 个位图块 = 4096B × 8 = 32768 位（保证片基址 4K 对齐；勿动）。*/
#define RFS_IV_SLICE_MIN_BITS	32768ULL

/* 硬分配未命中哨兵（⇄ god `PTR_INVALID`；拥有者直接透传）。*/
#define RFS_IV_NO_HIT		(~0ULL)

/* ══════════════════════════ 交付单位：run / 链 ══════════════════════════ */

/* run 三型（`rfs_run` / `rfs_run_list` / `rfs_run_dlist`）与「STL 式自造轮子」已定义在
 * `ravenfs_run_list.h` 并已 include：**容器拥有节点 + 分配绑定在容器上**（⇄ STL allocator）。
 * 内核 list 家族（`list`/`llist`/`hlist`/`klist`）**全侵入式**、无对等物，故自造（理由见该头）。
 * 片 / 顶层共用：顶层 `block_alloc_soft`/`block_intervals_free` 用单链，片 `all_dump` 用双链。*/

/* ═══════════════════════════ 分片自治单元 ═══════════════════════════ */

/* `struct rfs_slice`（⇄ mgr_slice_t）—— **本头刻意只前向声明**：
 *   片的数据结构（状态机 / 内部化几何 / 主从双树句柄+对象池 / 片锁 / 游标 / 计数）
 *   与**接口**（enable / alloc_hard / soft_alloc / soft_occupy / free_interval[s] /
 *   test_interval / all_dump / shutdown / free_count）**另立头文件设计**。
 *   随之另立的还有 `struct rfs_slice_cfg`（⇄ slice_config_t）与树/区间类型（D2）。
 *
 * 本头（顶层 = fs_allocator_t 翻译）只按**指针**引用片数组（`rfs_alloc_group.slices`），
 * 前向声明足够。分工：路由管几何推导 / 双组装配 / 跨片路由 / 审计；片管自己那一段
 * `[in_global_base_idx, +slice_bitscount)` 的主从树与原子性。
 * 不变量（片头必须守）：**从 ⊆ 主**；主树 base 升序且相邻区间严格不重叠。*/
struct rfs_slice;

/* ═══════════════════════════ 组 / 顶层路由 ═══════════════════════════ */

/* ⇄ `group_t`：一组位图（block 组或 inode 组）。*/
struct rfs_alloc_group {
	struct rfs_slice *slices;	/* [slice_count]：片自治单元数组 */
	u64		 per_slice_bits;	/* 每片名义位数（树版不存 nbits，按名义边界切段）*/
	u32		*preferred;	/* [cpu_slots]：每 CPU 亲和片游标（原子 relaxed 提示位）*/
};

/* ⇄ `fs_allocator_t`：顶层路由（几何推导 + 双组装配 + 路由 + 审计）。
 * 禁止拷贝（C 无拷贝构造，按约定：只按指针传；勿 memcpy 本结构）。*/
struct rfs_allocator {
	struct rfs_alloc_group blk_;	/* block 组 */
	struct rfs_alloc_group ino_;	/* inode 组 */
	u32 slice_count;
	u32 cpu_slots;
};

/* 顶层装配配置（⇄ `slices_cfg_t`）。两段位图原始内存**生命周期归调用方**、4K 对齐；
 * **出了 init 即可释放**（god：ctor 用一次即弃）。
 *
 * ⚠ F6 待裁（装载粒度）：god ctor 一次吃**全量**位图（峰值高）；ko 建议逐片喂。
 *   F5（落盘面）已裁决 
 *   —— 落盘归 FS 适配层，本模块不吃 IO/几何，故此处**不填 IO 面**。*/
struct rfs_slices_cfg {
	u8	*block_bits_raw;	/* 块组位图原始内存（已加载，4K 对齐）*/
	u64	 block_bits_count;	/* 块组位数（= bitmap_blocks × 4096 × 8）*/
	u8	*inode_bits_raw;	/* inode 组位图原始内存（同上）*/
	u64	 inode_bits_count;	/* inode 组位数 */
	u32	 logical_processor_count;	/* preferred 槽数 + 2^n 上界推导；0 → 退化单槽 */
	u64	 min_slice_bits;	/* 片下限（位数）；0 → RFS_IV_SLICE_MIN_BITS */
	/* 装配与元素池的内存来源（router 取 slices/preferred 数组 + 下传各片元素池）。
	 * 凡分配经此回调，模块零直接分配（同一 `rfs_mem_alloc` 类型，见 ravenfs_platform.h）。*/
	struct rfs_mem_alloc alloc;
};

/* ═════════════════════════════ 返回码（F3）═════════════════════════════
 * ko 统一 `int`：0 成功 / 负 -errno。god `result_code_t` 映射（**待裁**）：
 *   RC_OK                → 0
 *   RC_ERR_STATE         → -EINVAL
 *   RC_ERR_NO_BLOCK      → -ENOSPC
 *   RC_ERR_NOT_FOUND     → -ENOENT
 *   RC_ERR_OVERLAP       → -EEXIST
 *   RC_ERR_NOT_SUPPORTED → -EOPNOTSUPP
 * 硬分配未命中用哨兵 `RFS_IV_NO_HIT`（非负值返回），与 god 的 `PTR_INVALID` 同构。
 * ═════════════════════════════════════════════════════════════════════ */

/* ══════════════════════════ 生命周期 ══════════════════════════ */

/* ⇄ `fs_allocator_t(cfg, &rc)`：ctor 即装配（推导 slice_count → 双组 enable_slice →
 * preferred 初始化）。失败自清理（成员归空/未分配态），返回负 errno。
 * 位图原始内存**不取所有权**（cfg 只读消费）。*/
int	rfs_allocator_init(struct rfs_allocator *self, const struct rfs_slices_cfg *cfg);

/* ⇄ `~fs_allocator_t()`：逐片 shutdown + 释放自持资源。*/
void	rfs_allocator_fini(struct rfs_allocator *self);

/* ── 片自治单元接口（⇄ mgr_slice_t 各方法）**不在本头** ──
 * 见片头文件（`struct rfs_slice` 同处）：enable_slice / alloc_hard / soft_alloc /
 * soft_occupy / free_interval / free_intervals / test_interval / all_dump / shutdown /
 * free_count（⇄ get_free_count），及「除 free_count 外须持片锁」的锁纪律。*/

/* ── block 组（⇄ fs_allocator_t block 段；对外全局位号）── */

/* ⇄ `block_alloc_hard(require_count, align_log2)`：单片内完成、原子不跨片；
 * 全片失败 → RFS_IV_NO_HIT。*/
u64	rfs_allocator_block_alloc_hard(struct rfs_allocator *self, u64 require_count,
				       u8 align_log2);

/* ⇄ `block_alloc_soft(require_count, align_log2, *runs)`：跨片·碎片优先·穷尽所有分片；
 * **all-or-nothing**：不够 → 自动回滚已取段 + 返回 -ENOSPC（*runs 回空）。*/
int	rfs_allocator_block_alloc_soft(struct rfs_allocator *self, u64 require_count,
				       u8 align_log2, struct rfs_run_list *runs);

/* ⇄ `block_occupy_soft(blkbase, require_count, *rest_count)`：锚点片唯一 → 片锁不可换
 * （阻塞）；片尾跨界续圈内部消化（逐片单锁 = 真实连续）；结果即入参 blkbase 原址；
 * claimed<1 → -ENOSPC。可能产出跨片 run。*/
int	rfs_allocator_block_occupy_soft(struct rfs_allocator *self, u64 blkbase,
					u64 require_count, u64 *rest_count);

/* ⇄ `block_free(blkbase, count)`：区间释放（跨片逐段定位所属片）。*/
int	rfs_allocator_block_free(struct rfs_allocator *self, u64 blkbase, u64 count);

/* ⇄ `block_intervals_free(*runs)`：链式释放（消费 run 链，逐段定位所属片
 * free_interval；已消费段不回滚，余段留链）。*/
int	rfs_allocator_block_intervals_free(struct rfs_allocator *self,
					   struct rfs_run_list *runs);

/* ⇄ `block_test(blk)`。*/
bool	rfs_allocator_block_test(const struct rfs_allocator *self, u64 blk);

/* ── inode 组（⇄ fs_allocator_t inode 段）──
 * 注：god 的 inode 组复用同一 `mgr_slice_t` 片类（双树）；「inode 只用位置、不需尺寸树」
 * 的精简（doc §5.P4）**本版未采纳**，如需另行拍板。inode 0 = root（源位图预占）。*/

/* ⇄ `inode_alloc(out_inum)`：未命中 → -ENOSPC。*/
int	rfs_allocator_inode_alloc(struct rfs_allocator *self, u64 *out_inum);
/* ⇄ `inode_free(inum)`。*/
void	rfs_allocator_inode_free(struct rfs_allocator *self, u64 inum);
/* ⇄ `inode_test(inum)`。*/
bool	rfs_allocator_inode_test(const struct rfs_allocator *self, u64 inum);

/* ── 几何 / 审计（⇄ fs_allocator_t 只读取值段；F7 统一 const self）── */
u32	rfs_allocator_slice_count(const struct rfs_allocator *self);
u32	rfs_allocator_cpu_slots(const struct rfs_allocator *self);
u64	rfs_allocator_block_per_slice_bits(const struct rfs_allocator *self);
u64	rfs_allocator_inode_per_slice_bits(const struct rfs_allocator *self);
u64	rfs_allocator_block_free_total(const struct rfs_allocator *self);	/* 各片 free_count 求和 */
u64	rfs_allocator_inode_free_total(const struct rfs_allocator *self);
u32	rfs_allocator_block_preferred_of(const struct rfs_allocator *self, u32 cpu);
u32	rfs_allocator_inode_preferred_of(const struct rfs_allocator *self, u32 cpu);

/* ── 持久化出口（**裁决**：落盘归 FS 适配层，本模块只出**纯快照**）────────────
 * `all_syn` 是给 TTL 线程同步位图用的，但**其归属不属本模块**：
 *   · 本模块（运行时分配器）= 空闲区间集真值 + 分配/释放/测试，**零 IO、零盘上几何**；
 *   · 「树 → 补集 → 位图字 + 盘上落点 + 写回」= **FS 契约层**职责（它已持位图几何，
 *     见 `ravenfs_bitmap_*.h`）——否则会把 IO/几何拖进纯算法模块，破坏平台中立与
 *     用户态压测，且与位图几何成两份真值（几何双源）。
 * 故本头**不提供** `all_syn`；改提供纯内存**逐片快照**，供 FS 适配层组合：
 *   · `ino=false` 取 block 组、`ino=true` 取 inode 组；`slice` 为片下标；
 *   · 纯内存遍历（不碰 IO），把该片全部空闲区间按 base 升序 dump 进 `out`。
 * FS 适配层（TTL 线程调用点）据此：逐片 dump → 按全片范围取补集生成位图字 → 锁外写回。
 * （与 god 的差异：god `all_syn` 因 cfg 无 IO 面而 stub；本裁决把 IO 明确划给 FS 层。）*/
int	rfs_allocator_dump_slice(struct rfs_allocator *self, bool ino, u32 slice,
				 struct rfs_run_dlist *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _RAVENFS_ALLOCATOR_TOP_H */

# RavenFS → Linux KO：全模块性能计数器设计（v0.1.2）

> 2026-10-01 · Raven · **v0.1.2（定稿；2026-10-04 修订：F 组 `bm_`→`al_` 改制 + D-11 procfs 出口）**
> **定位**：`ravenfs.ko` 内置的、**superblock 级**性能/健康计数器 —— 覆盖**全部模块**（超块 / VFS 接待层 / 目录树 / inode 行缓存 / mdata 节点缓存 / TTL 后台 / 数据 IO / extent 树 / xattr&ACL / 名字堆 / **运行时树分配器**），打点在哪、记什么、为什么记、怎么给用户态看到。
> **前身**：`Docs/linux/ravenfs-linux-ko-sb-counters-design.md`（v0.1 草案，已退役；可从 `git show a358cc0:…` 取回）。本文 v0.1.2 = v0.1 的**全模块收口 + 覆盖补齐 + 分配器改制为 `al_`**，**以本文为唯一真值**。
> **地面真值**：`linux/` 下真实函数名逐个指认；本机内核 `7.2.6-1-vfio`（`CONFIG_DEBUG_INFO_BTF=y`、`FUNCTION_TRACER`、`KPROBES` 全开，debugfs 已挂载）；构建版本 `0.1.2`。
> **关联**：`ravenfs-linux-ko-requirements-spec.md`、`ravenfs-linux-ko-defects.md` §D-1、`ravenfs-linux-ko-v0.1.2-improvements.md`。

---

## 0. 一句话与决议总表

**只在 superblock 上挂一份 `struct ravenfs_sb_stats`（主表 86 字段）+ 独立子结构 `struct ravenfs_al_stats`（分配器 36 字段），合计 **122** 个 `atomic64` 计数器；挂载即清零，通过一个 seq_file 文件**全量吐出**；不做 per-inode、不做持久化、不做直方图。出口两处：**procfs 公开面**（`/proc/fs/ravenfs/<s_id>/stats`，人人可读，**2026-10-04 D-11 新增，事实 ABI**）+ debugfs 内部探针（非 ABI）。**

| # | 决议 | 一句话 |
|---|---|---|
| **S1** | 范围 = **仅 sb 级** | 不做 per-inode；单挂载 = 单份（理由见 §1） |
| **S2** | 暴露 = **procfs（公开） + debugfs（探针）** | 公开：`/proc/fs/ravenfs/<s_id>/stats`（`0444`，人人可读，**事实 ABI**）；探针：`/sys/kernel/debug/ravenfs/<s_id>/stats`（root-only、非 ABI）。一个 seq_file kv 文件，同一 show（**2026-10-04 D-11 修订**）|
| **S3** | 语义 = **挂载生命周期** | 计数器随 mount 生、随 umount 灭（`kzalloc(sbi)` 天然清零） |
| **S4** | 只做 **counter** | 累计原子量；gauge（当前脏行数等现读量）单列 §5，本版仍不做（`shutdown`/位图后端例外，见 §5） |
| **S5** | 打点 **不改控制流** | 计数是观测不是逻辑；打点失败/读到半旧值不影响正确性 |
| **S6** | 暴露 **只读** | 无 `clear`；reset = 重新 mount |
| **S7** | **不加编译开关** | 常开，开销用验收兜（§10） |
| **S8** | **全模块覆盖**（v0.1.2） | 每个 `.c` 模块都有自己的计数器组（含名字堆 `nh_`）；I/O 覆盖补齐到「所有显式下盘点」 |
| **S9** | **分配器注入式上报**（2026-10-04 改制） | `al_` 组经注入钩子 `struct rfs_stat_hook` 上报（分配器模块平台中立、不碰内核）；未注入 = no-op。旧 `bm_`「随模式」条款随位图退役失效 |

---

## 1. 范围裁决：为什么只做 sb 级

原问题是「要不要做 inode 级回调次数 / IO 次数 / miss 次数」。裁决为**不做**，三条理由：

1. **暴露成本不对称**：per-inode 必须「指名道姓」地读 → 要么加 `ioctl`（新 ABI 面），要么 debugfs dump-all（遍历 `sb->s_inodes` 并处理 `I_FREEING` 竞争）。sb 级一个文件读完，成本差一个量级。
2. **per-inode 有生命周期坑**：计数器住 `ravenfs_inode_info`，随 `ravenfs_evict_inode → ravenfs_destroy_inode` 一起死；且 cache miss 时目标 inode 往往**根本不在 core**（`iget_locked` 还没拿到对象），没地方挂 → 要么归父 inode、要么建 ino-keyed shadow 表。
3. **KISS**：调试期要回答的问题（缓存参数合不合理、写放大多少、谁在刷盘、错在哪）**在 sb 级就能答**；per-inode 是「看谁热」，而「谁热」ftrace/eBPF 也能临时拿（本机 `KPROBES+BTF` 全开），不必常驻内核。

**代价（明说）**：sb 级看不到「单个文件被打了多少次」。真需要时走观测式（ftrace/kprobe），不动内核结构。

---

## 2. 设计原则

- **P1 观测不干预**：打点只做 `atomic64_inc/add`，绝不影响返回值、锁序、控制流。
- **P2 单调累计**：只增不减、不重置（除重挂载）。派生率**不存**，由 counter 现算（§6）。
- **P3 撕裂读可接受**：读侧逐字段 `atomic64_read`，不做多字段一致性快照（x86_64 对齐 `u64` 读天然原子）。
- **P4 每个计数器必须答一个问题**：写不出问题的，不进清单。
- **P5 口径纪律**：**入口记 attempts、返回点记 outcomes**，两侧都记才能算失败率。
- **P6 命名**：`<子系统>_<对象>_<动作/结果>`，前缀即分组名（亦是 debugfs 小节头 `# [prefix]`）。
- **P7 全模块纪律**（v0.1.2）：新增模块 = 新增计数器组；I/O 下盘点「显式发起即记」，覆盖率缺口如实登记（§9）。

---

## 3. 全模块 → 计数器组映射（v0.1.2）

| # | 模块（文件） | 组前缀 | 字段数 | 该模块的计数器 |
|---|---|---|---|---|
| 1 | `super.c`（超块/挂载/VFS 根/同步） | `op_`·`life_`·`err_`·`io_flush_ops` | — | 见 §4 |
| 2 | `dentry.c`（VFS 目录接待层） | `op_`·`dir_`·`err_`·`io_` | — | §4.A/H |
| 3 | `dentry_tree.c`（目录 B+tree 原语） | `dir_leaf_reads`·`err_other` | — | §4.H |
| 4 | `dentry_node.c`（节点读写薄层 → 走 mdata） | （经 `md_`/`io_`） | — | — |
| 5 | `inode_cache.c`（inode 行缓存） | `ic_` | 9 | §4.B |
| 6 | `mdata_cache.c`（8K 元数据节点缓存） | `md_` | 8 | §4.C |
| 7 | `cache_ttl.c`（自写 TTL 淘汰器） | `ttl_` | 2 | §4.D |
| 8 | `iomap.c` + `file.c`（数据读写/mmap/落点） | `op_`·`io_`·`err_enospc` | — | §4.A/E |
| 9 | `extent_write.c`（extent 树） | `ext_` | 7 | §4.G |
| 10 | `xattr.c`（xattr/ACL） | `xa_`·`acl_`·`op_` | 10 | §4.I |
| 11 | `name_heap.c`（大名字/长目录尾部名字堆） | `nh_` | 6 | §4.L（v0.1.2 新增） |
| 12 | `ravenfs_allocator_router/slice.c`（运行时树分配器 v2） | `al_` | 36 | §4.F（2026-10-04 改制；旧 `bm_` 位图组退役） |
| — | 跨模块 | `err_`(6) | 6 | §4.J |

**字段总数 = A(26)+B(9)+C(8)+D(2)+E(5)+F(36)+G(7)+H(3)+I(10)+J(6)+K(4)+L(6) = 122。**
每 mount ≈ **976 B**（122 × 8B）——内存成本可忽略，「要不要」的唯一标准是 P4，不是内存。

> **F 组改制（2026-10-04）**：旧 `bm_`（位图时代 18 字段）随位图模块整体退役**删除**（死组）；
> 新分配器 = 多分片主从红黑树（`include/linux/ravenfs_allocator_top.h`），新增 **`al_` 组 36 字段**。
> `al_` 为**独立子结构** `sbi->al`（`enum rfs_al_stat` = 下标空间），经注入钩子
> `struct rfs_stat_hook`（`include/linux/ravenfs_allocator_stats.h`）从**平台中立**的分配器模块上报；
> 落点 `allocator_adapter.c: ko_al_stat_inc`。**为何不并进主 `ravenfs_sb_stats`**：钩子需按**下标**累加，
> 混入大数组会引入脆弱的绝对偏移；独立子结构使 `enum index == 子结构成员序号`。

---

## 4. 计数器总清单（逐字段）

> 读法：`字段` = `struct ravenfs_sb_stats` 成员（`include/linux/ravenfs.h`）；`打点位置` = 真实函数；`要回答` = P4 的问题。

### A. VFS 操作面（`op_`，26）—— 负载画像 + 行为指纹

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `op_lookup` | `ravenfs_lookup` | 基准分母；/`dir_leaf_reads`=每次 lookup 读几叶 |
| `op_create` / `op_mkdir` / `op_unlink` / `op_rmdir` / `op_rename` / `op_link` / `op_symlink` / `op_mknod` / `op_tmpfile` | `dentry.c` 对应回调入口 | 变更类配比；**行为指纹**（同 workload 次数变了=行为变了）|
| `op_readdir` | `ravenfs_iterate` | 大目录扫描强度 |
| `op_read_iter` / `op_write_iter` | `ravenfs_file_read_iter` / `ravenfs_file_write_iter` 完成点 | 读写次数（vs 字节=平均请求大小）|
| `op_read_bytes` / `op_write_bytes` | 同上，完成处 `请求−剩余` 差值累加 | **IO 放大分母**（§6）|
| `op_fallocate` | `ravenfs_fallocate` | 预分配占比 |
| `op_fsync` / `op_syncfs` | `ravenfs_fsync` / `ravenfs_sync_fs` | 落盘压力 / 卷级同步频次 |
| `op_getattr` / `op_setattr` | 对应回调 | 元数据操作强度（stat/write 类）|
| `op_mmap` | `ravenfs_file_mmap_prepare` | mmap 次数（回写落点分配入口强度）|
| `op_listxattr` / `op_xattr_get` / `op_xattr_set` | `xattr.c` 对应回调 | xattr 热点 |
| `op_acl_get` / `op_acl_set` | `ravenfs_get_inode_acl` / `ravenfs_set_acl` | **create 路径 `ravenfs_init_acl` 隐藏成本** |

### B. inode 行缓存 `icache`（`ic_`，9）—— 最核心调试价值

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `ic_hit` / `ic_miss` | `ravenfs_session_get` 快/慢路径 | 命中率分子/分母 → **`ttl_seconds`/`reap_ms` 调参判据** |
| `ic_row_load` | `ravenfs_row_load` 成功返回 | 真从盘装载行次数（miss 里有淘汰重试，≠ miss）|
| `ic_retry` | `session_get` 的 `-EAGAIN → goto retry` | **淘汰竞争烈度** |
| `ic_row_evict` | `ravenfs_inode_reclaim` | 主动逐出 |
| `ic_row_reap` | `ravenfs_cache_reap_expired` | TTL 过期淘汰 → TTL 是否在工作 |
| `ic_dirty_mark` | `ravenfs_inode_mark_dirty` | 脏行产生速率 |
| `ic_wb_rows` | `ravenfs_cache_wb_dirty` / `_wb_all` | **inode 行写回量**（写放大分子之一）|
| `ic_err` | `session_get` / `row_load` 错误返回 | 行装载失败（含 shutdown 后访问）|

### C. mdata 节点缓存（`md_`，8）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `md_hit` / `md_miss` | `ravenfs_mdata_get` 快/慢路径 | 8K 节点命中率（dentry+extent 树共用 → 分母大头）|
| `md_node_load` | `ravenfs_mrow_load` / `ravenfs_read_node` | 节点盘读次数 |
| `md_retry` | `mdata_get` 的 `-EAGAIN` 重试 | 同 `ic_retry`，mdata 侧 |
| `md_drop` | `ravenfs_mdata_drop` | 显式丢弃（节点被改/释放）|
| `md_reap` | `ravenfs_mdata_reap_expired` | TTL 淘汰节点数 |
| `md_node_wb` | `ravenfs_mdata_wb_node` / `_writeback_all` | **元数据节点写回量**（写放大大头）|
| `md_err` | `mdata_get` / `read_node` 错误返回 | 元数据读失败 |

### D. TTL 后台（`ttl_`，2）—— 「机制还活着」的证据

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `ttl_rounds` | `ravenfs_ttl_work` 正常完成 | **后台还活着吗**（数不涨=卡死）|
| `ttl_round_errs` | `ttl_work` 出错分支 | 后台刷盘失败累积 |

### E. 块设备 IO（`io_`，5）—— v0.1.2 **覆盖补齐**

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `io_read_ops` / `io_read_bytes` | 下盘点：`ravenfs_row_load`、`ravenfs_mrow_load`（= `ravenfs_read_node`）、`rfs_xb_load`（位图镜像经 `bm_image_load` 属挂载期，见 §9 盲区）| 物理读次数/字节 → /`op_lookup`=每操作 IO 数 |
| `io_write_ops` / `io_write_bytes` | 下盘点：`ravenfs_row_store`、`ravenfs_mrow_store`、`rfs_pwrite_dev`（目录树节点）、`rfs_store_block`（数据）、`rfs_xb_store`、`ravenfs_sb_write_timestamp`（SB）、`bm_image_save`（位图镜像 checkpoint）| **写放大 = `io_write_bytes / op_write_bytes`** |
| `io_flush_ops` | `ravenfs_issue_flush` | flush 屏障次数（每次 fsync/syncfs/umount ≥3 次）|

> **v0.1.2 变更**：把 **inode 行写回（`ravenfs_row_store`）**、**mdata 节点写回（`ravenfs_mrow_store`）**、**SB 写（`ravenfs_sb_write_timestamp`）** 纳入 `io_write_*`。此前只记 `rfs_pwrite_dev`/`rfs_store_block`/`rfs_xb_store`/位图 → §6「元数据写放大」**失真**（漏掉两条最大元数据写流）。**已知盲区**：`ravenfs_bitmap_used`（statfs 的整图直读）与挂载期 SB 初读（此时 `s_fsi` 未就绪）不入 io_，见 §9。

### F. 运行时树分配器（`al_`，36）—— 2026-10-04 改制（旧 `bm_` 位图组退役）

> **背景**：位图模块整体退役（`ec92d3c` 起引入树分配器 v2，本批完成 cutover）；旧 `bm_`（18 字段）**无任何埋点**（死组）→ 删除。
> 新分配器 = **多分片主从红黑树**（`include/linux/ravenfs_allocator_top.h` + `_slice.h`），**平台中立**、
> C(gnu11)/C++(c++17) 双编、**不得触碰内核**（无 `super_block`、无 `RFS_STAT_INC`）。
> 故计数器经**注入钩子** `struct rfs_stat_hook`（`include/linux/ravenfs_allocator_stats.h`）上报：
> `inc(ctx, field, delta)`；宿主 `allocator_adapter.c: ko_al_stat_inc` 按 `enum rfs_al_stat` 下标累加进
> **独立子结构** `sbi->al`（连续 `atomic64_t` 数组）。未注入 = no-op（用户态压测天然不打点）。
> **P1/P2**：钩子只累加、不改控制流；**可持片锁调用**（片内埋点 → 不睡眠、不 IO）。
> `BUILD_BUG_ON` 三重守：`sizeof(struct ravenfs_al_stats) == 36×8` ∧ 名字表 36 ∧ `AL_STAT_COUNT == 36`。

**A 装配 / 生命周期（5）**（落点 `allocator_adapter.c`，用 `RFS_AL_INC`）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `al_init` / `al_init_fail` | `ravenfs_allocator_create`（`rfs_allocator_init` 返码后）| 装配成功率（重建树是否稳）|
| `al_slices` | 同处 `+= rfs_allocator_slice_count()` | 装配片数（几何 `2^n ∩ 满片` 结果）|
| `al_shutdown` | `ravenfs_allocator_destroy`（关闸）| 卸载/失败回滚次数 |
| `al_drain_timeout` | 同处 `rfs_allocator_shutdown() != 0` | **排空超时 → fail-safe 拒绝释放**（须为 0）|

**B block 组顶层（9）**（落点 `ravenfs_allocator_router.c`，经钩子）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `al_blk_alloc_hard_ok` / `_fail` | `rfs_allocator_block_alloc_hard` 命中/全片未命中 | **硬分配失败率→ENOSPC/碎片** |
| `al_blk_alloc_soft_ok` / `_fail` | `rfs_allocator_block_alloc_soft` 凑满/穷尽 | 软分配路径占比（是否真在用）|
| `al_blk_occupy_ok` / `_partial` / `_fail` | `rfs_allocator_block_occupy_soft` 全满足/前缀断/一位未占 | 锚定认领使用面与断裂率 |
| `al_blk_free_calls` / `al_blk_free_blocks` | `rfs_allocator_block_free` + `_intervals_free` 入口 | 释放节奏 / 释放块总量（**rollback 不计**：走内部 `block_free_range`）|

**C inode 组顶层（3）**（`ravenfs_allocator_router.c`）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `al_ino_alloc_ok` / `_fail` | `rfs_allocator_inode_alloc` 命中 / 全片 ENOSPC | inode 号分配失败率 |
| `al_ino_free_calls` | `rfs_allocator_inode_free` | inode 号释放节奏 |

**D 片内双树 / 融合（8）**（落点 `ravenfs_allocator_slice.c`，经钩子；**诊断双树同步与碎片的核对心**）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `al_slice_alloc_hard` / `_enospc` | `rfs_slice_alloc_hard` 入口 / 满片 | 片级 best-fit 尝试与落空 |
| `al_slice_soft` / `_enospc` | `rfs_slice_soft_alloc` 入口 / 一位未取 | 片级碎片消费与吃干 |
| `al_slice_free` / `_eexist` | `rfs_slice_free_interval` 入口 / 重叠守卫命中 | 释放量 / **重叠 -EEXIST（应恒 0）** |
| `al_tree_insert` / `al_tree_erase` | `iv_link_both` 成功 / `iv_erase_both` | 双挂/双摘次数（**从 ⊆ 主不变式的活动量**）|

**E 融合（3）**（`rfs_slice_free_interval`；诊断碎片是否在自愈）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `al_merge_left` / `_right` / `_both` | 邻接判定处（记行为、不改流）| 邻接合并频率 → **碎片回收是否在工作** |

**F 路由 / 跨片（3）**（`ravenfs_allocator_router.c`）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `al_route_first_hit` / `_rescan` | `block_alloc_hard` / `inode_alloc` 命中时 `round==0 / >0` | **per-CPU 片亲和度**（首片命中 = 亲和好）|
| `al_route_cross_slice` | `block_occupy_soft` 逐片循环非锚点原片 | occupy 跨片续圈次数（真实连续性代价）|

**G checkpoint / 元素池（5）**（checkpoint 落点 `allocator_adapter.c`；池落点 `slice.c`）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `al_checkpoint` / `_fail` | `ravenfs_allocator_flush` 快照+落盘成功 / 失败 | 位图镜像落盘健康度 |
| `al_checkpoint_blocks` | 同处 `+= bitmap_blocks + inode_bm_blocks` | 写回镜像块量 |
| `al_pool_alloc` / `al_pool_fail` | `iv_insert_both` 元素池取节点成功 / `-ENOMEM` | 区间节点池压力（诊断内存兜底）|

### G. extent 树（`ext_`，7）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `ext_lookup` / `ext_insert` | `ravenfs_extent_lookup` / `_insert` | 查找/插入次数（direct inode 不走 → 比值=direct 占比）|
| `ext_split` | `rfs_ext_leaf_split` + `rfs_ext_inner_split` | **分裂频率→树是否在抖** |
| `ext_new_root` | `rfs_ext_new_root_inner` + `ravenfs_extent_new_root_leaf` | 树长高次数 |
| `ext_balance` | `rfs_ext_node_balance` + `rfs_ext_balance_from` | 平衡动作 |
| `ext_unmap` | `ravenfs_extent_unmap_loose` | truncate/punch/释放的 unmap 强度 |
| `ext_node_reads` | `rfs_ext_walk` 经 `mdata_get` 的节点取用 | **树高实测**（/ext_lookup=每次摸几节点）|

### H. 目录（`dir_`，3）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `dir_leaf_reads` | `ravenfs_read_leaf` | readdir/lookup 读叶次数 |
| `dir_emit` | `ravenfs_emit_leaf` | 实际吐条目批次（/leaf_reads=每叶产出）|
| `dir_nlink_adjust` | `rfs_dir_adjust` | nlink 维护频次（E3 `size+2` 恒过报策略实测面）|

### I. xattr / ACL（`xa_`，10）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `xa_get` / `xa_set` / `xa_del` / `xa_list` | `xattr.c` 对应路径 | xattr 读写配比 |
| `xa_block_load` / `xa_block_store` | `rfs_xb_load` / `rfs_xb_store` | xattr 块 IO 量（每次 set=读-改-写）|
| `xa_e2big` | 返回 `-E2BIG` 处 | **单块上限（E7）被撞频率** |
| `xa_err` | `ravenfs_xattr_get/set` 错误返回 | xattr 失败总量 |
| `acl_get` / `acl_set` | `ravenfs_get_inode_acl` / `ravenfs_set_acl`（含 `ravenfs_init_acl`）| create/mkdir 隐式 ACL 成本 |

> `op_acl_*`（VFS 面）与 `acl_*`（实现面）刻意分开：前者=VFS 找我要了几次，后者=我真读/写了几次盘上 ACL；差值=缓存/短路省下的量。

### J. 错误与健康（`err_`，6）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `err_shutdown_hits` | 各入口 `ravenfs_is_shutdown(sb)` 判真返 `-EIO` 处 | **停机影响面** |
| `err_shutdown_trig` | `ravenfs_force_shutdown` | 想停机次数（**每次调用都记**）|
| `err_meta_io` | 元数据路径（行/节点/xb/位图）IO 失败 | 元数据 IO 失败总量 |
| `err_sb_read` | `ravenfs_fill_super` SB 读失败 + 运行期 SB 重读失败 | 盘/设备健康 |
| `err_enospc` | 分配失败上抛（dentry/iomap）| 与 `al_blk_alloc_hard_fail` 配对（底层/对外）|
| `err_other` | 其余未归类错误返回 | 兜底；`err_*` 全家和不应有黑洞 |

### K. 生命周期（`life_`，4）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `life_mount` / `life_umount` | `ravenfs_fill_super` 成功末尾 / `ravenfs_put_super` 入口 | 挂载/卸载次数（配对校验；读到时应 `life_mount==1`）|
| `life_iget_new` | `ravenfs_iget` 的 `I_NEW` 分支 | **inode churn** |
| `life_evict` | `ravenfs_evict_inode` | 驱逐次数；`iget_new−evict`=当前在 core 净增 |

### L. 名字堆（`nh_`，6）—— v0.1.2 新增（`name_heap.c`）

> 名字堆**不做直接磁盘 I/O**（其尾部数据经 mdata 片，已并入 `md_`/`io_`、块位图并入 `al_`）→ 本组只记「名字堆」这一层自身的操作面，使「全模块」每模块皆有计数器。

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `nh_alloc` | `ravenfs_name_heap_alloc` 入口（大名字分配）| 长名/大目录消耗规模 |
| `nh_alloc_fail` | 同函数 `-ENOSPC`（当前粒度档全满）| 堆容量压力（配 `ext_`/`al_` 判碎）|
| `nh_write` | `ravenfs_name_heap_write` 入口 | 尾部名字写次数 |
| `nh_free` | `ravenfs_name_heap_free` 入口 | 尾部名字释放次数 |
| `nh_activate` | `rfs_heap_activate` 成功（`RFS_NHC_SET`）| 堆激活频率（长名晋升信号）|
| `nh_release` | `ravenfs_dir_name_heap_release` 释放空堆处 | 空堆回收（目录折构时可回收的堆数）|

---

## 5. Gauge（现读量，本版仅两项）

与 counter 不同，这些是「此刻的值」。**本版只放零成本、已就绪的两项**（在 stats 的 `# state` 段）：

| 量 | 来源 | 成本 |
|---|---|---|
| `shutdown` | `ravenfs_is_shutdown(sb)` 一个 `READ_ONCE` | 0 |
| `bitmap_mode`（+ chunk 时 `bitmap_chunk_order blk=.. ino=..`）| `ravenfs_bitmap_opts_get(sb)`（读本次挂载锁定值）| 0 |

其余 gauge（当前脏 inode 行数 / 脏 mdata 节点数）需「只读导出接口 + 拿分片锁」，成本非零 → **留后续**（§11）。gauge 与 counter 不混一个字段表：放 `# state` 独立段落。

---

## 6. 派生指标（**不存**，现算）

全部由 §4 counter 除出来（P2：存派生值=存两份还可能不一致）。用户态 `cat stats` 后一行 `awk` 即可：

| 指标 | 公式 | 用途 |
|---|---|---|
| icache 命中率 | `ic_hit/(ic_hit+ic_miss)` | TTL/`reap_ms` 调参主判据 |
| mdata 命中率 | `md_hit/(md_hit+md_miss)` | 同上（节点侧）|
| **元数据写放大** | `io_write_bytes / op_write_bytes` | 一个用户字节要写多少盘字节（v0.1.2 补齐后**可信**）|
| IO 放大（次数） | `io_write_ops / op_write_iter` | 每次写触发几次盘写 |
| 每 lookup 盘读 | `io_read_ops / op_lookup` | lookup 有多贵 |
| 树高（实测） | `ext_node_reads / ext_lookup` | extent 查找摸几个节点 |
| 分配失败率 | `al_blk_alloc_hard_fail / (al_blk_alloc_hard_ok + al_blk_alloc_hard_fail)` | ENOSPC/碎片 |
| 软分配占比 | `al_blk_alloc_soft_ok / (al_blk_alloc_soft_ok + al_blk_alloc_soft_fail)` | 软分配路径是否真在用 |
| 片亲和度 | `al_route_first_hit / (al_route_first_hit + al_route_rescan)` | per-CPU 首选片命中率（高=亲和好）|
| 双树活动 | `al_tree_insert − al_tree_erase` | 区间数净增（当前空闲区间族规模代理）|
| 融合强度 | `(al_merge_left+al_merge_right+al_merge_both) / al_slice_free` | 碎片自愈比例 |
| 重叠守卫命中 | `al_slice_free_eexist` | **应恒 0**（>0 = 双树/账本不一致）|
| 行淘汰竞争率 | `ic_retry/(ic_hit+ic_miss)` | 淘汰 vs 访问打架程度 |
| flush 强度 | `io_flush_ops/(op_fsync+op_syncfs+life_umount)` | 屏障是否过度 |
| inode churn | `life_iget_new/life_evict`（配 `ic_row_evict`）| 两层驱逐是否同步 |
| 长名压力 | `nh_activate/nh_alloc`、`nh_alloc_fail` | 名字堆晋升/容量 |

---

## 7. 暴露途径与格式

### 7.1 出口形状

```
/sys/kernel/debug/ravenfs/<s_id>/stats     # 0444，seq_file，kv 每行一个
```

格式（便于 `awk`，也是 §6 公式的输入）：

```
# ravenfs stats  sb=nvme0n1p1  mounted_at=<ns>
# [op]
op_lookup	184213
op_create	2211
...
# [ic]
ic_hit		171002
ic_miss		13211
...
# [al]
al_init		1
al_slices	4
al_blk_alloc_hard_ok	90112
...
# [nh]
nh_alloc	12
...
# state
shutdown	0
bitmap_mode	chunk
bitmap_chunk_order	blk=3 ino=0
```

- **一个文件全吐**（不拆多文件）：一次 `cat` = 一次完整观测；字段间非严格一致（P3）但足够。
- **分组小节头 `# [prefix]`**（v0.1.2）：`prefix` 取名字第一个 `_` 前（`op`/`ic`/`md`/`ttl`/`io`/`al`/`ext`/`dir`/`xa`/`acl`/`err`/`life`/`nh`）。**纯注释行**，解析用 `awk '!/^#/'` 仍得纯 kv；勿把小节头当数据。`al_` 组为**独立子结构**，在主循环之后单独打印 `# [al]`。
- **`# state` 段**：gauge（`shutdown` + 位图后端）。`bitmap_mode` 为**磁盘契约**（位图镜像）的模式，与运行时 `al_` 组无关（运行时恒为树分配器）。

### 7.2 生命周期（**顺序是重点**）

| 时机 | 动作 |
|---|---|
| `ravenfs_init` | debugfs：`ravenfs_dbg_root = debugfs_create_dir("ravenfs", NULL)`（`ERR_PTR` 忽略——`kernel.debugfs=off`）；**procfs（D-11）**：`ravenfs_proc_root = proc_mkdir("fs/ravenfs", NULL)` |
| `ravenfs_fill_super` 成功末尾 | debugfs：`sbi->dbg_dir = debugfs_create_dir(sb->s_id, ravenfs_dbg_root)` + `debugfs_create_file("stats", 0444, sbi->dbg_dir, sb, &fops)`；**procfs（D-11）**：`sbi->proc_dir = proc_mkdir(sb->s_id, ravenfs_proc_root)` + `proc_create_single_data("stats", 0444, sbi->proc_dir, ravenfs_stats_show, sb)` |
| **`ravenfs_put_super`，`kfree(sb->s_fs_info)` 之前** | **`debugfs_remove_recursive(sbi->dbg_dir)`** 与 **`proc_remove(sbi->proc_dir)`**（两处均早于 `kfree`）|
| `ravenfs_exit` | `debugfs_remove_recursive(ravenfs_dbg_root)` 与 `proc_remove(ravenfs_proc_root)` |

⚠️ **UAF 红线**：`stats` 文件 `i_private` 持 `sb` 指针 → `debugfs_remove_recursive` 必须**早于** `kfree(sbi)`；`debugfs_remove` 会等进行中的 read 结束（dentry active ref）→ 该顺序充分。
⚠️ **`kernel.debugfs=off`**：`debugfs_create_dir`/`_file` 返 `ERR_PTR` → **忽略不拒挂**（`IS_ERR_OR_NULL` 守卫）。

### 7.3 为什么只读、不给 `clear`（S6）

- reset 需并发读写语义（读者可能读到半清零表），为低频操作引入一致性问题不划算；
- 「重看一轮」的自然操作是 `umount && mount`，与 S3 语义一致（`life_mount` 顺带证明重挂过）。

### 7.4 出口权限（**2026-10-04 D-11 修订：原「root-only 是特性」口径作废**）

> **原口径（已作废）**：debugfs 挂载点 `0700`，普通用户读不到；宣称「这是特性：调试探针不是 ABI，字段增删改名无兼容义务」。
>
> **修订（D-11）**：该决策被推翻——出口**放错了位置**。数据面早已就绪（`atomic64` 计数），展示面却锁在 root 专属的 `/sys/kernel/debug`（`0700`），普通用户 / 监控脚本读不到 → D-1 的全部观测价值被白白锁死。故**新增人人可读的 procfs 公开出口**：

- **公开面（procfs）**：`/proc/fs/ravenfs/<s_id>/stats`，`0444`（同形惯例 `/proc/fs/xfs/stat`），**人人可读**；
- **内部探针（debugfs）**：`/sys/kernel/debug/ravenfs/<s_id>/stats`，`0444`，**保留**（root-only，内核开发期用）；
- 两处**同一 `ravenfs_stats_show`**（数据源单一，`m->private = sb`）。
- **兼容义务反转**：公开面一旦稳定即为**事实 ABI** → 字段名 / 分组的增删改名**须立兼容纪律**（对齐 D-10「对外契约成员清单」教训）；debugfs 面仍非 ABI。

---

## 8. 存储布局与成本

```c
/* ravenfs.h */
struct ravenfs_sb_stats {   /* 主表 86 × atomic64_t = 688 B */
    atomic64_t op_lookup;   /* A 组 */
    /* … 按 A–E、G–L 分段、段间空行 + 注释 … */
    atomic64_t nh_release;  /* L 组 */
};
struct ravenfs_al_stats {   /* 分配器 36 × atomic64_t = 288 B（独立下标空间 = enum rfs_al_stat）*/
    atomic64_t al_init;     /* A 组 */
    /* … */
    atomic64_t al_pool_fail;
};
struct ravenfs_sb_info {
    /* … */
    struct ravenfs_sb_stats stats;   /* 随 kzalloc(sbi) → mount 即清零（S3）*/
    struct ravenfs_al_stats al;      /* F 组：注入钩子落点（见 §4.F）*/
    struct dentry *dbg_dir;
    u64 mounted_at_ns;
};
```

- **内存**：86 × 8B + 36 × 8B = **976 B / mount**，多挂载线性叠加，可忽略。
- **零初始化免费**：`kzalloc(sbi)` 已清零 → 无需任何 init 代码，天然给出 S3 语义。
- **争用（已知代价，暂不优化）**：字段集中 → 高并发多核打同一 `atomic64` 有 cacheline bouncing。初版接受（调试指标非吞吐指标，每次 1–2 ns）。超预算 → 升级路径 = 字段 cacheline 对齐或 per-cpu，**不是**加锁/编译开关。

---

## 9. 覆盖率矩阵与已知盲区（P7 诚实登记）

| 模块 | 读 I/O 记 | 写 I/O 记 | 语义计数 | 备注 |
|---|---|---|---|---|
| inode 行 | ✅ `io_read`（`row_load`）| ✅ `io_write`（`row_store`，**v0.1.2 补**）| `ic_`（9）| |
| mdata 节点 | ✅ `io_read`（`mrow_load`）| ✅ `io_write`（`mrow_store`，**v0.1.2 补**）| `md_`（8）| 目录树/extent 树节点全走此路 |
| 目录树节点 | （经 mdata）| ✅ `io_write`（`rfs_pwrite_dev`）| `dir_`（3）| |
| 数据块 | iomap 默认 bio | ✅ `io_write`（`rfs_store_block`）| `op_*bytes` | 读走 iomap bio（内核层，不显式记）|
| xattr 块 | ✅ `io_read`（`rfs_xb_load`）| ✅ `io_write`（`rfs_xb_store`）| `xa_`（10）| |
| SB | ⚠️ **盲区**：挂载初读（`s_fsi` 未就绪）| ✅ `io_write`（`sb_write_timestamp`）| `err_sb_read`（读失败）| 初读一次性，可忽略 |
| 位图镜像 | ✅ `bm_image_load`（挂载期，§9 盲区）| ✅ `bm_image_save`（checkpoint）| `al_`（36）| 运行时树分配器；镜像=磁盘契约底座 |
| 名字堆 | （经 mdata）| （经 mdata）| `nh_`（6）| 无直接 I/O |
| **盲区** | `ravenfs_bitmap_used`（statfs 整图直读，O(bitmap)，未记以免 statfs 污染 io_read）| — | — | §11 |

**口径诚实**：`io_` 只记「我们代码显式发起的点」；iomap 默认 bio、statfs 整图直读、挂载期 SB 初读 **不计**，字段注释与本节均已标「覆盖率不完全」，**不假装全量**。

---

## 10. 验收判据

1. **形状**：mount 后 `cat …/stats` 返回**全部 122 字段**、初值全 0（`life_mount==1`），含 `# [group]` 小节头（含独立 `# [al]`）与 `# state` 段。
2. **单调性**：跑 `rfsfilechurn`/`rfsdirchurn` 一轮，相关计数器**只增不减**，且 `ic_hit+ic_miss>0`、`io_read_ops>0`、`io_write_ops>0`（证明真接上，不是摆设）。
3. **派生自洽**：§6 命中率落 (0,1)；`al_blk_alloc_hard_ok+al_blk_alloc_hard_fail` 与 `al_blk_free_*` 方向自洽（P5）；`al_slice_free_eexist == 0`；**v0.1.2 加**：`io_write_bytes ≥ ic_wb_rows×4096`（行写已被计入）之类方向性校验。
4. **生命周期安全**：`umount` 时 **procfs `/proc/fs/ravenfs/<s_id>` 与 debugfs 目录均消失**；**反复 mount/umount + 并发 `cat`（两处出口）** 无 crash / 无 KASAN UAF。
4b. **非 root 可读（D-11）**：`sudo -u nobody cat /proc/fs/ravenfs/<dev>/stats` **成功**且字段齐全（`life_mount==1`）。
5. **行为零变化**：现有验收（xfstests `-g quick` 等）**全绿**——计数器不许改变任何返回值与路径（P1）。
6. **开销**：同 workload 打点前后重编对比，吞吐差 **< 2%**。
7. **分配器上报（2026-10-04 改制）**：`# [al]` 36 字段齐全（`al_init≥1`、`al_slices==装配片数`）；空闲盘 `al_blk_alloc_hard_fail` 随负载增长；`al_slice_free_eexist==0`；`al_drain_timeout==0`（正常卸载）。旧 `bm_`/`bitmap_mode` 随模式条款已废。

---

## 11. 未决 / 待裁

1. **`err_shutdown_trig` 口径**：每次调用都记（§4.J 已定）。
2. **gauge 扩量**：当前脏 inode 行 / 脏 mdata 节点数（需只读导出 + 分片锁），本版未做。
3. **`io_` 盲区**：`ravenfs_bitmap_used`（statfs）与挂载 SB 初读是否计入（现不计）。
4. **`nh_alloc_fail` 口径**：仅记粒度档耗尽（`-ENOSPC`），参数/IO 错误归 `err_*`。
5. **命名前缀**：`ic_` vs `cache_`、`md_` vs `mdata_`（现取短前缀）。
6. **per-op 延迟**：仅位图 `*_ns`（换片/扫描）做了耗时累计；**热路径（lookup/read/write）刻意不加** `ktime`（每次打点开销 × 高频）。若确需，走 ftrace，不入常驻内核。
7. **开销超预算**：触发字段 cacheline 分组（P2 升级路径），不回退编译开关（S7）。

---

## 附：决策记录

- **为什么不是 per-inode**：§1 三条（暴露成本 / 生命周期与 miss 鸡生蛋 / KISS），本轮明确裁掉，不是遗漏。
- **为什么不是 tracepoint/eBPF**：eBPF 适合**临时、带维度**观测，依赖 root + 窗口内有数据 + 符号未内联；本需求是**常驻、单值、随挂载生灭** → debugfs + `atomic64` 最短路径。两者互补。
- **为什么不是 `statfs`**：`statfs` 是面向用户的语义 ABI（块数/inode 数），塞调试计数器=把调试面变 ABI；且一次只给十来个字段。
- **为什么不存派生率**：P2，公式给在 §6。
- **为什么常量开着**：S7 + §8 争用评估 + §10.6 兜底。
- **v0.1.2 为什么补 io_write**：§4.E/§6 元数据写放大此前漏两条最大写流 → 失真；补齐是**修正**而非新增口径。
- **v0.1.2 为什么加 `nh_`**：S8「全模块」——名字堆是唯一无自有计数器的模块；其 I/O 已被 `md_`/`al_` 覆盖，故只补该层操作面。
- **2026-10-04 为什么把 `bm_` 整组换成 `al_`**：位图模块（`struct rfs_bm`）整体退役、运行时改为多分片主从红黑树（`rfs_allocator`）→ 旧 `bm_` 事件出口（`rfs_bm_stat_sink`）连同类一起消失，18 字段成为**死组**（无埋点）。新 `al_` 组由**平台中立**的树分配器经注入钩子 `struct rfs_stat_hook` 上报——**保持算法模块不碰内核**（可 C/C++ 双编 + 用户态单测），落点 `ko_al_stat_inc`。旧「随模式」条款（`bitmap_mode` 消歧）随之失效：`bitmap_mode` 现仅指**磁盘契约镜像**的模式，运行时分配器恒为树。

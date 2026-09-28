# RavenFS → Linux KO：Superblock 级性能计数器设计草案

> 2026-09-28 · Raven · **v0.1（草案，待裁）**
> **定位**：`ravenfs.ko` 内置的、**仅 superblock 级**的性能/健康计数器——打点在哪、记什么、为什么记、怎么给用户态看到。本文是"先定清单再动手"的设计稿，**不含实现**。
> **关联**：`ravenfs-linux-ko-requirements-spec.md`（第一约束规格）、`disk_layout_v1.2_inode.md`、`ravenfs-linux-ko-acceptance-plan.md`（验收预言机）。
> **地面真值**：`linux/` 下各文件的真实函数名（打点位置逐个指认，不臆造）；本机内核 `7.2.3-1-vfio`（`CONFIG_DEBUG_INFO_BTF=y`、`FUNCTION_TRACER`、`KPROBES` 全开，debugfs 已挂载）。
> **本文性质**：草案 = 计数器清单 + 决策依据。**清单字段名、分期范围属"待裁"**，实现前请先过 §11 未决项。

---

## 0. 一句话与决议总表

**只在 superblock 上挂一份 `struct ravenfs_sb_stats`（纯 `atomic64` 计数器），挂载即清零，通过 debugfs 一个 seq_file 文件全量吐出；不做 per-inode、不做持久化、不做直方图、不做 ABI。**

| # | 决议 | 一句话 |
|---|---|---|
| **S1** | 范围 = **仅 sb 级** | 放弃 per-inode 计数（本轮已裁，理由见 §1）；单挂载 = 单份 |
| **S2** | 暴露 = **debugfs** | `/sys/kernel/debug/ravenfs/<s_id>/stats`，一个 seq_file kv 文件；root-only、非 ABI |
| **S3** | 语义 = **挂载生命周期** | 计数器随 mount 生、随 umount 灭（`kzalloc(sbi)` 天然清零）；不做跨挂载累计 |
| **S4** | 只做 **counter** | 原子递增的累计量；**gauge**（当前脏行数等现读量）单列 §5，初版不做 |
| **S5** | 打点 **不改控制流** | 计数器是观测，不是逻辑；打点失败/读到半旧值都不影响正确性 |
| **S6** | 暴露 **只读** | 初版无 `clear` 写入口（reset 语义 = 重新 mount，见 §8.3） |
| **S7** | **不加编译开关** | 不做 `#ifdef CONFIG_RAVENFS_STATS`；常开，开销用验收兜（§10） |

---

## 1. 范围裁决：为什么只做 sb 级

原问题是"要不要做 inode 级回调次数 / IO 次数 / miss 次数"。裁决为**不做**，三条理由：

1. **暴露成本不对称**：per-inode 必须"指名道姓"地读 → 要么加 `ioctl`（新 ABI 面），要么 debugfs dump-all（要遍历 `sb->s_inodes` 并处理 `I_FREEING` 竞争）。sb 级一个文件读完，成本差一个量级。
2. **per-inode 有生命周期坑**：计数器住在 `ravenfs_inode_info`，随 `ravenfs_evict_inode` → `ravenfs_destroy_inode` 一起死；且 cache **miss 发生时目标 inode 往往根本不在 core**（`iget_locked` 还没拿到对象），压根没有地方可挂 → 要么归父 inode、要么建 ino-keyed shadow 表，都是额外结构。
3. **KISS**：调试期想回答的问题（缓存参数合不合理、写放大多少、谁在刷盘、错在哪）**在 sb 级就能答**；per-inode 是"看谁热"，而"谁热"这一层 ftrace/eBPF 观测也能临时拿（本机 `KPROBES+BTF` 全开），不必常驻内核。

**代价（明说）**：sb 级看不到"单个文件被打了多少次"。真需要时走观测式（ftrace function profiler / kprobe），不动内核结构。

---

## 2. 设计原则

- **P1 观测不干预**：打点只做 `atomic64_inc/add`，绝不影响返回值、锁序、控制流。
- **P2 单调累计**：只增不减、不重置（除重挂载）。派生率**不存**，由两个 counter 现算（§4）。
- **P3 撕裂读可接受**：读侧逐字段 `atomic64_read`，**不做多字段一致性快照**（x86_64 对齐 `u64` 读天然原子，字段间不一致对调试无害）。换来的是一行锁都不用。
- **P4 每个计数器必须答一个问题**：清单里"要回答什么"一栏写不出问题的，不进清单（DISCIPLINE：设计意图可见，不为记而记）。
- **P5 口径纪律**：**入口记 attempts、返回点记 outcomes**，两侧都记才能算失败率；只记一侧的计数器是残废的。
- **P6 命名 = `<子系统>_<对象>_<动作/结果>`**：前缀分组，`hit/miss`、`ok/fail` 成对出现。

---

## 3. 计数器总清单

> **读法**：`字段` = `struct ravenfs_sb_stats` 成员；`打点位置` = 真实函数；`要回答` = P4 的问题。
> 全清单 **98 个字段**，每 mount ≈ **784 B**（98 × 8B）——内存成本可忽略，所以"要不要"的唯一标准是 P4，不是内存。
> （2026-09-28：位图分配组完成 chunk 化移植，F 组由 9 → 18 个字段，见 §3.F；总数 89 → 98。）

### A. VFS 操作面（`op_`，26 个）——负载画像 + 行为指纹

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `op_lookup` | `ravenfs_lookup` 入口 | 基准分母；和 `dir_leaf_reads` 比 = 每次 lookup 读几个叶子 |
| `op_create` / `op_mkdir` / `op_unlink` / `op_rmdir` / `op_rename` / `op_link` / `op_symlink` / `op_mknod` / `op_tmpfile` | `dentry.c` 对应回调入口 | 变更类操作配比；回归时**同一 workload 这些次数应稳定** → 行为指纹（改动后次数变了 = 行为变了） |
| `op_readdir` | `ravenfs_iterate` | 大目录扫描强度 |
| `op_read_iter` / `op_write_iter` | `ravenfs_file_read_iter` / `ravenfs_file_write_iter` 完成点 | 读写次数（次数 vs 字节 = 平均请求大小） |
| `op_read_bytes` / `op_write_bytes` | 同上，完成处 `读前 - 读后` 差值累加 | **IO 放大分母**（§4）；没有它一切放大率都算不出来 |
| `op_fallocate` | `ravenfs_fallocate` | 预分配路径占比 |
| `op_fsync` | `ravenfs_fsync` | 落盘压力（每次 fsync = 一轮三阶段写回 + flush） |
| `op_syncfs` | `ravenfs_sync_fs` | 卷级同步频次 |
| `op_getattr` / `op_setattr` | 对应回调 | stat/write 类元数据操作强度（stat 重负载下这两个极热，但 inc 成本可忽略） |
| `op_mmap` | `ravenfs_file_mmap_prepare` | mmap 映射次数（回写落点分配路径的入口强度） |
| `op_listxattr` / `op_xattr_get` / `op_xattr_set` | `ravenfs_listxattr` / `ravenfs_xattr_get` / `ravenfs_xattr_set` | xattr 是否成为热点（M6 刚接） |
| `op_acl_get` / `op_acl_set` | `ravenfs_get_inode_acl` / `ravenfs_set_acl` | **create 路径的 `ravenfs_init_acl` 隐藏成本**（E3/POSIX ACL 决议的实测验证） |

### B. inode 行缓存 `icache`（`ic_`，9 个）——最核心的调试价值

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `ic_hit` | `ravenfs_session_get` 快路径（`xa_load` 拿到行） | 缓存命中率分子 |
| `ic_miss` | 同函数 `else` 分支（进 `ravenfs_row_load`） | 分母；**`ttl_seconds`/`reap_ms` 调参的直接判据** |
| `ic_row_load` | `ravenfs_row_load` 成功返回 | 真正从盘装载行的次数（miss 里有淘汰竞争重试，不等同 miss） |
| `ic_retry` | `session_get` 的 `err == -EAGAIN → goto retry` | **淘汰竞争烈度**：这个数高说明行淘汰和访问在打架（`ROW_EVICTING` 接管） |
| `ic_row_evict` | `ravenfs_inode_reclaim` | 主动逐出次数 |
| `ic_row_reap` | `ravenfs_cache_reap_expired` | TTL 过期淘汰行数 → TTL 是否在正常工作 |
| `ic_dirty_mark` | `ravenfs_inode_mark_dirty` | 脏行产生速率 |
| `ic_wb_rows` | `ravenfs_cache_wb_dirty` / `ravenfs_cache_writeback_all` | **inode 行写回量**（写放大分子之一） |
| `ic_err` | `ravenfs_session_get` / `row_load` 错误返回 | 行装载失败（含 shutdown 后访问） |

### C. mdata 节点缓存（`md_`，8 个）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `md_hit` / `md_miss` | `ravenfs_mdata_get` 快/慢路径 | 8K 元数据节点命中率（dentry 树 + extent 树共用 → 分母大头） |
| `md_node_load` | `ravenfs_mrow_load` / `ravenfs_read_node` | 节点盘读次数（写放大 & 树遍历成本） |
| `md_retry` | `mdata_get` 的 `-EAGAIN` 重试 | 同 `ic_retry`，mdata 侧 |
| `md_drop` | `ravenfs_mdata_drop` | 显式丢弃（节点被改/释放）次数 |
| `md_reap` | `ravenfs_mdata_reap_expired` | TTL 淘汰节点数 |
| `md_node_wb` | `ravenfs_mdata_wb_node` / `ravenfs_mdata_writeback_all` | **元数据节点写回量**（写放大大头：一个逻辑写触发几次树节点回写） |
| `md_err` | `mdata_get` / `read_node` 错误返回 | 元数据读失败 |

### D. TTL 后台（`ttl_`，2 个）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `ttl_rounds` | `ravenfs_ttl_work` 正常完成 | **后台还活着吗**（数不涨 = work 卡死/被停），轮次 × `reap_ms` ≈ 实际调度间隔 |
| `ttl_round_errs` | `ttl_work` 出错分支 | 后台刷盘失败累积 |

> 这一组是"活着的证据"：计数器最重要的用途之一是**证明机制还在转**，而不是只在出问题时才有数。

### E. 块设备 IO（`io_`，5 个）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `io_read_ops` / `io_read_bytes` | 下盘点：`ravenfs_row_load`、`ravenfs_mrow_load`、`ravenfs_read_leaf`、`sb_bread`、`rfs_xb_load`、`rfs_bm_group_load`（FULL）/ `rfs_bm_chunk_read`（CHUNK，挂载扫描 + 换片读 + TTL 态2 回读） | 物理读次数/字节 → 与 `op_lookup` 等比 = 每操作 IO 数 |
| `io_write_ops` / `io_write_bytes` | 下盘点：`rfs_pwrite_dev`、`rfs_store_block`、`rfs_xb_store`、`rfs_bm_group_sync`（FULL）/ `rfs_bm_chunk_write`（CHUNK，换片 evict 写 + TTL 态1/态2 写回） | **写放大 = `io_write_bytes / op_write_bytes`** |
| （`io_flush_ops` 并入此组） | `ravenfs_issue_flush` | flush 屏障次数（R-SY-05 成本；每次 fsync/syncfs/umount 至少 3 次） |

**⚠️ 实现工作量集中点**：IO 下盘点散在 6+ 个文件，且部分经 buffer_head / iomap 核心间接发出。**第一批只覆盖能明确指认的直接点**，并把"覆盖率"本身当已知缺口写进验收（§10）。**不追求第一次就全**。

### F. 位图分配（`bm_`，18 个）—— FULL / CHUNK 双后端

> 位图分配器 2026-09-28 完成 **chunk 化移植**（`linux/bitmap.c`）：新增 **backing 后端**，由**模块参数** `bitmap_backing`（0=FULL 默认 / 1=CHUNK）与 `bitmap_chunk_order`（默认 4）显式选择。**FULL = 移植前逐位行为（红线基线）**；**CHUNK = 懒加载**（每片一个固定 chunk 缓冲 + 控制器数组 `rfs_bm_chunk_ctl` + 片级返还记录 `rfs_bm_rec` 池）。**打点必须两边都覆盖**：同一次 `ravenfs_block_*`/`ravenfs_inode_*` 调用在 FULL 走旧片内函数、在 CHUNK 走 `_chunk_` 路由 + 窗口原语，落点不同但**语义与粒度同一**（同一字段在两种模式下计同一个量）；组级分叉点在 `rfs_bm_group_load/_sync/_alloc*/_free` 入口。

**通用面**（两种模式共用语义；打点取各自真实落点）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `bm_alloc_calls` / `bm_alloc_ok` / `bm_alloc_fail` | 组级入口：`rfs_bm_group_alloc`（FULL）/ `rfs_bm_group_chunk_alloc`（CHUNK）；成败在片内：FULL `rfs_bm_slice_alloc`；CHUNK `rfs_bm_chunk_slice_alloc_hard` 返回 0 / `-ENOSPC` | **分配失败率 → ENOSPC/碎片判据**（P5：成败两侧都记）。CHUNK 的 fail 含"全片窗迭代扫毕（含绕回）= SLICE_EMPTY" |
| `bm_alloc_blocks` | 成功处累加块数：FULL `rfs_bm_slice_alloc` 命中；CHUNK `rfs_bm_win_alloc_hard` 命中（`s->free_count -= count` 处） | 分配的块总量 vs 用户请求量（元数据膨胀） |
| `bm_free_calls` / `bm_free_blocks` | 组级：`rfs_bm_group_free`（FULL）/ `rfs_bm_group_chunk_free`（CHUNK）；片内：FULL 逐段清位；CHUNK `rfs_bm_chunk_slice_free`（在线 `rfs_bm_clear_bits_locked` / 离线挂账） | 释放节奏；alloc/free 块数长期不平 = 泄漏线索。**块数只按 1→0 实清累计**（离线挂账未清位，不重复计） |
| `bm_soft_ok` | 软分配成功：FULL `rfs_bm_slice_alloc_soft`；CHUNK `rfs_bm_chunk_slice_alloc_soft` → `rfs_bm_win_alloc_soft` | **软分配路径占比**（D2/D3 对齐 god 的软分配是否真被走到） |
| `bm_occupy` | 锚定认领：FULL `rfs_bm_slice_occupy`；CHUNK `rfs_bm_chunk_slice_occupy` → `rfs_bm_win_soft_occupy` | 锚定认领次数（区间认领原语的使用面） |
| `bm_sync_slices` | 位图写回量：FULL `rfs_bm_group_sync` 逐片全量写回**片数**；CHUNK `rfs_bm_group_chunk_sync` 实际写回 **chunk 数**（= 态1 + 态2，见下） | 第三阶段写回量；**FULL↔CHUNK 直接 A/B 对照**（全量 80MB/30s ↔ 只写脏/有待还） |

**CHUNK 后端专属**（`backing=CHUNK` 才动；FULL 恒 0）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `bm_chunk_switch` | `rfs_bm_chunk_ensure_online` 发布目标窗处（`online_chunk = ctl; gen++`） | **换片频率**：hint 局部性是否成立（近零 = 常态命中在线窗；偏高 = 冷热混合，规格 §11 A/B 判据） |
| `bm_chunk_switch_ns` | 同上，`ensure_online` 全程 `ktime` 差累加（`atomic64_add`，口径同 `op_read_bytes` 的"累计量"记法） | **换片代价**（旧窗 evict 写 + 目标窗读）；`/ bm_chunk_switch` = 单次均耗时 |
| `bm_rec_attach` | `rfs_bm_chunk_slice_free` 离线挂账分支（`rfs_bm_pool_acquire` 成功、挂 `bk->rec_head`） | **离线返还压力**：挂账节点数（每片每次 free 至多 1 节点）→ 池深 `rec_pool_depth`(64) 是否够 |
| `bm_rec_drain` | `rfs_bm_drain_window` 摘节点处（`rfs_bm_pool_release`） | drain 消费量；与 `bm_rec_attach` 的净差 = 尚未还账的挂账（长期不归零 = 换片/TTL 没在回收） |
| `bm_pool_empty` | `rfs_bm_chunk_slice_free` 池空兜底分支（`rfs_bm_pool_acquire` 返回 NULL → `ensure_online` 强制上线直清） | **池空兜底触发次数**：>0 即 record 压力撞池深上限 → 调 `rec_pool_depth` 的信号（规格 §10.9 不变量） |
| `bm_summary_skip` | 摘要免载入跳过：`rfs_bm_chunk_lock_window` 的 `summary == ALL_USED` 跳过 + `rfs_bm_chunk_slice_test` 的摘要裁决 | **摘要是否挡住 ENOSPC 扫描悬崖**（全片失败轮询时避免逐 chunk 连锁载入；规格 §7/§10.5） |
| `bm_scan_ns` | `rfs_bm_chunk_mount_scan` 逐窗 `ktime` 差累加（mount 顺序扫描全程） | **mount 扫描耗时**（CHUNK ctor 唯一 I/O 源；与 FULL mount I/O 同价）→ 规格 §11 A/B「mount 耗时」列 |
| `bm_ttl_online` | `rfs_bm_chunk_sync_one` 态1（`online == ctl && dirty`）写回成功处 | TTL 三态之**在线脏窗**同步量（规格 §9 态1） |
| `bm_ttl_drain` | `rfs_bm_chunk_sync_one` 态2（`offline && has_recs`）写回成功处 | TTL 三态之**离线还账窗**同步量（规格 §9 态2） |

> 态3（其余 = 干净在线窗 / 无账离线窗）跳过、零 I/O → **不计数**；三态总量即 `bm_sync_slices`（= 态1 + 态2）。mount 扫描的**盘读次数/字节**已由 E 组 `io_read_ops`/`io_read_bytes` 在 `rfs_bm_chunk_read` 处覆盖（FULL 侧在 `rfs_bm_group_load`），此处不重复计。
> **部署时机**：本节（含通用面在 CHUNK 模式的落点）属设计定稿，**计数器代码部署 = 测试团队完成 chunk 化验收后的下一阶段**（见 §9）。

### G. extent 树（`ext_`，7 个）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `ext_lookup` | `ravenfs_extent_lookup` | extent 查找次数（直接寻址 `direct` 的 inode 不走 → 比值反映 direct 占比） |
| `ext_insert` | `ravenfs_extent_insert` | 映射插入次数 |
| `ext_split` | `rfs_ext_leaf_split` + `rfs_ext_inner_split` | **分裂频率 → 树是否在抖**（高频分裂 = 平衡策略有问题） |
| `ext_new_root` | `rfs_ext_new_root_inner` + `ravenfs_extent_new_root_leaf` | 树长高的次数 = 树高增长速率 |
| `ext_balance` | `rfs_ext_node_balance` + `rfs_ext_balance_from` | 平衡动作次数 |
| `ext_unmap` | `ravenfs_extent_unmap_loose` | 反映 truncate/punch/释放的 unmap 强度 |
| `ext_node_reads` | `rfs_ext_walk` 内经 `mdata_get` 的节点取用 | **走树步数总量 → 树高实测**（`ext_node_reads / ext_lookup` ≈ 平均每次查找摸几个节点） |

### H. 目录（`dir_`，3 个）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `dir_leaf_reads` | `ravenfs_read_leaf` | readdir/lookup 读叶子次数 |
| `dir_emit` | `ravenfs_emit_leaf` | 实际吐出条目批次数（与 `dir_leaf_reads` 比 = 每叶产出） |
| `dir_nlink_adjust` | `rfs_dir_adjust` | nlink 维护频次（E3 决议：`size+2` 恒过报策略的实测面） |

### I. xattr / ACL（`xa_`，10 个）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `xa_get` / `xa_set` / `xa_del` / `xa_list` | `ravenfs_xattr_get` / `ravenfs_xattr_set` / `ravenfs_listxattr`（del = set 空值路径） | xattr 读写配比 |
| `xa_block_load` / `xa_block_store` | `rfs_xb_load` / `rfs_xb_store` | xattr 块 IO 量（每次 set = 读-改-写 = 1 load + 1 store） |
| `xa_e2big` | `xattr.c` 返回 `-E2BIG` 处 | **单块上限（E7 决议）被撞的频率**——这个数高说明"单块自定义"要重新评估 |
| `xa_err` | `ravenfs_xattr_get/set` 错误返回（含 `-ENOSPC` 取块失败） | xattr 路径失败总量 |
| `acl_get` / `acl_set` | `ravenfs_get_inode_acl` / `ravenfs_set_acl`（**含 `ravenfs_init_acl` 内部调用**） | create/mkdir 隐式 ACL 成本（与 `op_create` 比 = 每次创建摸几次 ACL） |

> 注：`op_acl_*`（VFS 面）与 `xa_acl_*`（FS 内部实现面）**刻意分开**——前者是"VFS 找我要了几次"，后者是"我真的读/写了几次盘上 ACL"，差值 = 缓存/短路省下的量。

### J. 错误与健康（`err_`，6 个）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `err_shutdown_hits` | 各入口 `ravenfs_is_shutdown(sb)` 判真返 `-EIO` 的地方 | **停机后有多少操作撞墙**（停机影响面） |
| `err_shutdown_trig` | `ravenfs_force_shutdown` | 进入停机的次数（幂等置位，只在首次/每次？→ 定为**每次调用都记**，见 §7 口径） |
| `err_meta_io` | 元数据路径（行/节点/树节点/位图）IO 失败 | 元数据 IO 失败总量 |
| `err_sb_read` | `ravenfs_fill_super` 的 `sb_bread` 失败 + 运行期 SB 重读失败 | 盘/设备健康 |
| `err_enospc` | 位图分配失败导致的 `-ENOSPC` 上抛 | 与 `bm_alloc_fail` 配对（一个记底层、一个记对外） |
| `err_other` | 其余未归类错误返回 | 兜底；`err_*` 全家的和不应有"黑洞" |

> **这一组直接延续既定错误模型立场**：拒绝复刻 Linux AHE/复杂降级，只留**少量计数器辅助判断**（见 MEMORY：NVMe 错误模型 2026-09-22 定调）。`err_` 就是"少量"的具体化——6 个字段，不加状态机、不加降级逻辑。

### K. 生命周期（`life_`，4 个）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `life_mount` | `ravenfs_fill_super` 成功末尾 | 挂载次数（= 计数器被清零的次数；读到 stats 时应 == 1，>1 说明中间 umount 过） |
| `life_umount` | `ravenfs_put_super` 入口 | 卸载次数（配对校验） |
| `life_iget_new` | `ravenfs_iget` 中 `iget_locked` 返回 `I_NEW` 分支 | **inode churn**：新建 VFS inode 的速率（与 `life_evict` 配对看抖动） |
| `life_evict` | `ravenfs_evict_inode` | 驱逐次数；`iget_new - evict` = 当前在 core 的净增 |

---

## 4. 派生指标（**不存**，现算）

全部由 §3 的 counter 除出来，文档给出公式即可——**存派生值 = 存两份、还可能不一致**（P2）。

| 指标 | 公式 | 用途 |
|---|---|---|
| icache 命中率 | `ic_hit / (ic_hit + ic_miss)` | `ttl_seconds`/`reap_ms` 调参主判据 |
| mdata 命中率 | `md_hit / (md_hit + md_miss)` | 同上（节点侧） |
| **元数据写放大** | `io_write_bytes / op_write_bytes` | 一个用户字节要写多少盘字节 |
| IO 放大（次数） | `io_write_ops / op_write_iter` | 每次写操作触发几次盘写 |
| 每 lookup 盘读 | `io_read_ops / op_lookup` | lookup 有多贵 |
| 树高（实测） | `ext_node_reads / ext_lookup` | extent 查找摸几个节点 |
| 分配失败率 | `bm_alloc_fail / bm_alloc_calls` | ENOSPC/碎片 |
| 软分配占比 | `bm_soft_ok / bm_alloc_ok` | D2/D3 软分配路径是否真在用 |
| 换片率（CHUNK） | `bm_chunk_switch / bm_alloc_calls` | hint 局部性：每分配触发几次换片（A/B 判据） |
| 单次换片耗时（CHUNK） | `bm_chunk_switch_ns / bm_chunk_switch` | 换片成本（旧窗 evict 写 + 目标窗读） |
| record 净挂账（CHUNK） | `bm_rec_attach − bm_rec_drain` | 离线返还回收是否跟得上（长期 >0 = 还账滞后） |
| 行淘汰竞争率 | `ic_retry / (ic_hit + ic_miss)` | 淘汰 vs 访问打架程度 |
| flush 强度 | `io_flush_ops / (op_fsync + op_syncfs + life_umount)` | 屏障是否过度 |
| inode churn | `life_iget_new / life_evict`（配 `ic_row_evict`） | VFS inode 与缓存行两层驱逐是否同步 |

> **建议在文档/脚本里给一份 `awk` 一行式**，让用户态 `cat stats` 后直接算（P0 验收要用）。

---

## 5. Gauge（现读量，**初版不做**）

和 counter 不同，这些是"此刻的值"，只能现读、没有累计意义：

| 量 | 来源 | 初版态度 |
|---|---|---|
| 当前脏 inode 行数 / 当前在 core 行数 | `ravenfs_cache` 内部 | **P2**（要加只读导出接口 + 可能拿 shard 锁，成本非零） |
| 当前脏 mdata 节点数 | `ravenfs_mdata_cache` | 同上 |
| `shutdown` 布尔 | `ravenfs_is_shutdown(sb)` | 可以顺手放（一个 `READ_ONCE`，零成本）→ **建议进 P0** |
| `ttl_seconds` / `reap_ms` / `wb_age_seconds` / `bitmap_sync_seconds` / `bitmap_backing` / `bitmap_chunk_order` | 已由 `module_param` 暴露在 `/sys/module/ravenfs/parameters/` | **不重复**（已有出口）；`bitmap_backing`/`bitmap_chunk_order` = CHUNK 开关与 chunk 尺寸 |

> 决策：**gauge 与 counter 不混一个字段表**，避免读的人把"当前值"当"累计值"。若做，放 `stats` 文件的独立段落（`# state` 注释行分隔）。

---

## 6. 存储布局与成本

```c
/* ravenfs.h：紧邻 struct ravenfs_sb_info 之前 */
struct ravenfs_sb_stats {
	atomic64_t	op_lookup;	/* A 组 26 */
	/* ... 共 98 个字段，按 A–K 分段、段间空行 + 注释 ... */
};

struct ravenfs_sb_info {
	/* ... 现有字段 ... */
	struct ravenfs_sb_stats stats;	/* S3：随 sbi 一起 kzalloc → mount 即清零 */
	struct dentry		*dbg_dir;	/* debugfs 挂载点目录（§8） */
};
```

- **内存**：98 × 8B = **784 B / mount**。多个挂载线性叠加，可忽略。
- **零初始化免费**：`ravenfs_parse_superblock` 里 `kzalloc(sbi)` 已经清零 → **无需任何 init 代码**，且天然给出 S3 语义。
- **争用（已知代价，暂不优化）**：所有字段集中在 784B 内 → 高并发下多核打同一 `atomic64` 会有 cacheline bouncing。**初版接受**：这是调试指标不是吞吐指标，且每次打点 1–2 ns。若验收（§10）发现开销超预算，**升级路径是字段分组对齐（`____cacheline_aligned`）或 per-cpu**，不是加锁、不是加编译开关。

---

## 7. 打点规范

1. **宏统一入口**（放 `ravenfs.h`）：
   ```c
   #define RFS_STAT_INC(sb, field)		RFS_STAT_ADD(sb, field, 1)
   #define RFS_STAT_ADD(sb, field, n)					\
	do {								\
		struct ravenfs_sb_info *__s = (sb) ? (sb)->s_fs_info : NULL; \
		if (__s)						\
			atomic64_add((n), &__s->stats.field);		\
	} while (0)
   ```
   **必须判 `s_fs_info`**：`parse_superblock` 失败、`put_super` 尾段（`kfree` 之后）都可能出现 `sb` 在手但 sbi 已空的窗口。判空是这条宏存在的唯一理由，别在调用点重复判。
2. **位置**：成功/完成点记 outcome，入口记 attempt（P5）。**绝不在持锁临界区内做任何非原子操作**——`atomic64_add` 不睡眠，可安全持锁调用。
3. **口径先定死**（否则数字没意义）：
   - `ic_hit`/`ic_miss` **互斥且完备**：`session_get` 里 `row` 拿到 = hit，走 `row_load` = miss；`-EAGAIN` 重试**在最终归宿处记一次**，不在每次重试记（否则 retry 会重复污染 hit/miss）。
   - `err_shutdown_trig`：**每次调用都记**（不只首次）——它回答"有几次想停机"，首次与否另有 `shutdown` 布尔可查。
   - `op_read_bytes`：在 `read_iter` 完成点用 `原请求 - 剩余` 算，不逐块累加。
4. **错误路径也要记**：`err_*` 在错误返回前打，**不能因为"出错了"就跳过观测**——出错时的数据才是最值钱的。
5. **不引入新的锁/内存分配**：打点只碰 `atomic64`。

---

## 8. 暴露途径与生命周期

### 8.1 出口形状

```
/sys/kernel/debug/ravenfs/<s_id>/stats     # 0444，seq_file，kv 每行一个
```

格式（便于 `awk`，也是 §4 派生公式的输入）：

```
# ravenfs stats  sb=nvme0n1p1  mounted_at=<ns>
op_lookup	184213
op_read_iter	50211
ic_hit		171002
ic_miss		13211
...
```

**一个文件全吐**（不拆多文件）：一次 `cat` = 一次完整观测，字段间虽非严格一致（P3）但足够；拆文件反而让"读到一半配置"的组合更乱。

### 8.2 生命周期（**顺序是重点**）

| 时机 | 动作 |
|---|---|
| `ravenfs_init` | `ravenfs_dbg_root = debugfs_create_dir("ravenfs", NULL)`（返回 `ERR_PTR` 时忽略——`kernel.debugfs=off` 场景） |
| `ravenfs_fill_super` 成功末尾 | `sbi->dbg_dir = debugfs_create_dir(sb->s_id, ravenfs_dbg_root)` + `debugfs_create_file("stats", 0444, sbi->dbg_dir, sbi, &fops)` |
| **`ravenfs_put_super`，`kfree(sb->s_fs_info)` 之前** | **`debugfs_remove_recursive(sbi->dbg_dir)`** |
| `ravenfs_exit` | `debugfs_remove_recursive(ravenfs_dbg_root)` |

⚠️ **UAF 红线**：`stats` 文件的 `i_private` 持的是 `sbi` 指针 → `debugfs_remove_recursive` 必须**早于** `kfree(sbi)`。现有 `put_super` 结尾正是 `kfree(sb->s_fs_info); sb->s_fs_info = NULL;`，打点就插在它前面。`debugfs_remove` 会等进行中的 read 结束（dentry 有 active ref），所以这个顺序是充分的。

### 8.3 为什么初版不给 `clear`（S6）

- reset 需要并发读者与写者的语义（读者可能读到半清零的表），**为一个低频操作引入一致性问题不划算**；
- "重新看一轮"的自然操作是 `umount && mount`，成本就是一次挂载，且和 S3 语义一致（`life_mount` 还会顺带证明你确实重挂过）；
- 真有高频 reset 需求时，加一个 0222 文件 + `xa` 式"换指针"或按字段 `atomic64_set` 都行——**留到有真实需求再做**（不做过度工程化）。

### 8.4 root-only（明说不是 bug）

debugfs 挂载点 `0700`，普通用户读不到。**这是特性**：调试探针不是 ABI，我们不对普通用户承诺任何字段稳定（S2）。字段增删改名无兼容义务。

---

## 9. 分期落地

| 期 | 范围 | 字段数 | 交付判据 |
|---|---|---|---|
| **P0** | `struct` + 宏 + debugfs 骨架（§6/§7/§8 全套）+ **B `ic_`(9) + C `md_`(8) + E `io_`(5) + J `err_`(6) + D `ttl_`(2) + K `life_`(4)** + A 里挑 8 个（`op_lookup`/`op_read_iter`/`op_write_iter`/`op_read_bytes`/`op_write_bytes`/`op_fsync`/`op_create`/`op_unlink`） | **42** | §10 判据 1–4 |
| **P1** | A 其余 + F `bm_`(18) + G `ext_`(7) + H `dir_`(3) + I `xa_`(10)；`io_` 覆盖点补齐 | 98 | §10 判据 1–6 |
| **P2** | gauge 段（§5）；若开销超预算 → cacheline 分组 | — | §10 判据 6 |

> P0 已能回答**最值钱的四个问题**：缓存参数合不合理（`ic_/md_` 命中率）、写放大多少（`io_/op_`）、后台活着吗（`ttl_`）、错在哪（`err_`）。

**部署时机**：上表为**计数器代码**（`struct ravenfs_sb_stats` + 打点 + debugfs 出口）的落地顺序；**计数器代码部署 = 测试团队完成位图 chunk 化验收后的下一阶段**（本轮仅设计定稿，不动代码）。

---

## 10. 验收判据

1. **形状**：mount 后 `cat /sys/kernel/debug/ravenfs/<s_id>/stats` 返回全部 P0 字段、初值全 0（`life_mount` == 1）。
2. **单调性**：跑 `tools/rfsfilechurn` / `rfsdirchurn` 一轮，相关计数器**只增不减**，且 `ic_hit + ic_miss > 0`、`io_read_ops > 0`（证明打点真的接上了，不是摆设）。
3. **派生自洽**：§4 公式在 churn 后算出的命中率落在 (0,1)、`io_write_bytes >= 0`、`bm_alloc_ok + bm_alloc_fail == bm_alloc_calls`（P5 完备性校验）。
4. **生命周期安全**：`umount` 时 debugfs 目录消失；**反复 mount/umount + 并发 `cat`** 无 crash / 无 KASAN UAF（`put_super` 顺序正确性）。
5. **行为零变化**：现有验收（`ravenfs-linux-ko-*-acceptance.md` / xfstests `-g quick`）**全绿**——计数器不许改变任何返回值与路径（P1）。
6. **开销**：同一 workload（如 `rfsfilechurn` 固定轮次）打点前后重编对比，吞吐差 **< 2%**；超预算 → 触发 P2 的 cacheline 分组升级，**不回退到编译开关**（S7）。

---

## 11. 未决 / 待裁

1. **字段名单**：§3 是草案，98 个是否全要？尤其 A 组 26 个里 `op_getattr`/`op_setattr`/`op_mmap` 这类"大概率很热但可能用不上"的，要不要瘦身为 P1。
2. **`io_` 覆盖率口径**：buffer_head / iomap 核心间接发出的 IO 算不算？建议**只记我们代码显式发起的点**，并在字段注释里标"覆盖率不完全"，避免假装全量。
3. **`err_shutdown_trig` 口径**：每次调用都记 vs 仅首次（§7.3 已倾向"每次都记"，待确认）。
4. **`ic_hit/miss` 的 `-EAGAIN` 重试归宿**（§7.3 已定"最终归宿记一次"，待确认不会掩盖竞争烈度——竞争已有 `ic_retry` 专记）。
5. **是否要 gauge**（§5）：P0 先跳过是否可接受。
6. **命名前缀**：`ic_` vs `cache_`、`md_` vs `mdata_`（现取短前缀，表更窄）。
7. **`acl_*` 与 `op_acl_*` 是否过度**（§I 注）：两个都记才有"短路省了多少"，但确实是两处打点——若嫌重，P0 先只留 `op_acl_*`。

---

## 附：决策记录（DISCIPLINE 要求的推理留痕）

- **为什么不是 per-inode**：见 §1 三条（暴露成本 / 生命周期与 miss 鸡生蛋 / KISS）。这条是**本轮明确裁掉的**，不是遗漏。
- **为什么不是 tracepoint/eBPF**：eBPF（本机 `KPROBES+BTF` 全开）适合**临时、带维度（per-inode/per-pid）的观测**，但依赖 root + 窗口内有数据 + 符号未被内联；本需求是**常驻、单值、随挂载生灭** → debugfs + `atomic64` 是最短路径。两者互补，不互斥。
- **为什么不是 `statfs`**：`statfs` 是**面向用户的语义 ABI**（块数/inode 数），塞调试计数器等于把调试面变成要维护的 ABI；且 `statfs` 一次只能给十来个字段。
- **为什么不存派生率**：P2（一致性 + 单一事实来源），公式给在 §4，用户态一行 `awk`。
- **为什么只读不给 `clear`**：S6 + §8.3，避免为低频操作引入并发清零语义。
- **为什么敢常开不加编译开关**：S7 + §6 争用评估 + §10 判据 6 兜底。

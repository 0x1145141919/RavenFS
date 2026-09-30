# RavenFS → Linux KO：全模块性能计数器设计（v0.1.2）

> 2026-10-01 · Raven · **v0.1.2（定稿）**
> **定位**：`ravenfs.ko` 内置的、**superblock 级**性能/健康计数器 —— 覆盖**全部模块**（超块 / VFS 接待层 / 目录树 / inode 行缓存 / mdata 节点缓存 / TTL 后台 / 数据 IO / extent 树 / xattr&ACL / 名字堆 / 双位图分配器），打点在哪、记什么、为什么记、怎么给用户态看到。
> **前身**：`Docs/linux/ravenfs-linux-ko-sb-counters-design.md`（v0.1 草案，已退役；可从 `git show a358cc0:…` 取回）。本文 v0.1.2 = v0.1 的**全模块收口 + 覆盖补齐 + 位图随模式**，**以本文为唯一真值**。
> **地面真值**：`linux/` 下真实函数名逐个指认；本机内核 `7.2.6-1-vfio`（`CONFIG_DEBUG_INFO_BTF=y`、`FUNCTION_TRACER`、`KPROBES` 全开，debugfs 已挂载）；构建版本 `0.1.2`。
> **关联**：`ravenfs-linux-ko-requirements-spec.md`、`ravenfs-linux-ko-v0.1.1-defects.md` §D-1、`ravenfs-linux-ko-v0.1.2-improvements.md`。

---

## 0. 一句话与决议总表

**只在 superblock 上挂一份 `struct ravenfs_sb_stats`（纯 `atomic64` 计数器，**104** 字段），挂载即清零，通过 debugfs 一个 seq_file 文件**全量吐出**；不做 per-inode、不做持久化、不做直方图、不做 ABI。**

| # | 决议 | 一句话 |
|---|---|---|
| **S1** | 范围 = **仅 sb 级** | 不做 per-inode；单挂载 = 单份（理由见 §1） |
| **S2** | 暴露 = **debugfs** | `/sys/kernel/debug/ravenfs/<s_id>/stats`，一个 seq_file kv 文件；root-only、非 ABI |
| **S3** | 语义 = **挂载生命周期** | 计数器随 mount 生、随 umount 灭（`kzalloc(sbi)` 天然清零） |
| **S4** | 只做 **counter** | 累计原子量；gauge（当前脏行数等现读量）单列 §5，本版仍不做（`shutdown`/位图后端例外，见 §5） |
| **S5** | 打点 **不改控制流** | 计数是观测不是逻辑；打点失败/读到半旧值不影响正确性 |
| **S6** | 暴露 **只读** | 无 `clear`；reset = 重新 mount |
| **S7** | **不加编译开关** | 常开，开销用验收兜（§10） |
| **S8** | **全模块覆盖**（v0.1.2） | 每个 `.c` 模块都有自己的计数器组（含名字堆 `nh_`）；I/O 覆盖补齐到「所有显式下盘点」 |
| **S9** | **位图随模式**（v0.1.2） | `bm_` 组随位图后端（mem/chunk）；非本后端专属字段恒 0，打点只写在对应后端 |

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
| 12 | `bitmap_core/mem/chunk/adapter.c`（双位图分配器） | `bm_` | 18 | §4.F |
| — | 跨模块 | `err_`(6) | 6 | §4.J |

**字段总数 = A(26)+B(9)+C(8)+D(2)+E(5)+F(18)+G(7)+H(3)+I(10)+J(6)+K(4)+L(6) = 104。**
每 mount ≈ **832 B**（104 × 8B）——内存成本可忽略，「要不要」的唯一标准是 P4，不是内存。

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
| `io_read_ops` / `io_read_bytes` | 下盘点：`ravenfs_row_load`、`ravenfs_mrow_load`（= `ravenfs_read_node`）、`rfs_xb_load`、`bm_mem_load`(MEM) / `bm_io_read`(CHUNK) | 物理读次数/字节 → /`op_lookup`=每操作 IO 数 |
| `io_write_ops` / `io_write_bytes` | 下盘点：`ravenfs_row_store`、`ravenfs_mrow_store`、`rfs_pwrite_dev`（目录树节点）、`rfs_store_block`（数据）、`rfs_xb_store`、`ravenfs_sb_write_timestamp`（SB）、`bm_mem_flush_group`/`bm_io_write`（位图） | **写放大 = `io_write_bytes / op_write_bytes`** |
| `io_flush_ops` | `ravenfs_issue_flush` | flush 屏障次数（每次 fsync/syncfs/umount ≥3 次）|

> **v0.1.2 变更**：把 **inode 行写回（`ravenfs_row_store`）**、**mdata 节点写回（`ravenfs_mrow_store`）**、**SB 写（`ravenfs_sb_write_timestamp`）** 纳入 `io_write_*`。此前只记 `rfs_pwrite_dev`/`rfs_store_block`/`rfs_xb_store`/位图 → §6「元数据写放大」**失真**（漏掉两条最大元数据写流）。**已知盲区**：`ravenfs_bitmap_used`（statfs 的整图直读）与挂载期 SB 初读（此时 `s_fsi` 未就绪）不入 io_，见 §9。

### F. 位图分配（`bm_`，18）—— **随模式**（MEM / CHUNK）

> 位图已重构为 sb-free 纯算法类 `struct rfs_bm`（`ravenfs_bitmap_core.h`；core 路由 + mem/chunk 双后端 + adapter 适配）。类**不持有 sb**，故计数器经**事件出口注入**：`rfs_bm_cfg.stat`（`enum rfs_bm_stat` + `struct rfs_bm_stat_sink`）→ 适配层 `bm_stat_emit` 映射到 `sbi->stats.bm_*`。**打点代码只写在对应后端**：CHUNK 专属事件在 MEM 恒不发出（字段恒 0），反之亦然。**公共 `rfs_bm_*` 签名不变**。

**通用面**（两模式同一语义，落点各自）：

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `bm_alloc_calls` / `bm_alloc_ok` / `bm_alloc_fail` | 路由 `rfs_bm_block_alloc_hard` 入口/命中/全败 | **分配失败率→ENOSPC/碎片**（P5：ok+fail==calls）|
| `bm_alloc_blocks` | 同函数命中处 `+=count` | 分配块总量 vs 用户请求（元数据膨胀）|
| `bm_free_calls` | 路由 `rfs_bm_block_free` 入口 | 释放节奏 |
| `bm_free_blocks` | MEM `mem_free_interval`；CHUNK 实际清位点（drain/asyn 的 `chunk_apply_recs`）| **只按 1→0 实清累计**（挂账未清不重复计）|
| `bm_soft_ok` | 路由 `rfs_bm_block_alloc_soft` 命中 | 软分配路径占比（D2/D3 是否真在用）|
| `bm_occupy` | 路由 `rfs_bm_block_occupy_soft` 认领≥1 | 锚定认领使用面 |
| `bm_sync_slices` | MEM：adapter `bm_mem_flush_group` 逐片 / CHUNK：`cs_chunks_asyn` 态1+态2 | 第三阶段写回量；FULL↔CHUNK A/B 对照 |

**CHUNK 后端专属**（`mode=chunk` 才动；MEM 恒 0）：

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `bm_chunk_switch` / `bm_chunk_switch_ns` | `cs_ensure_online` 发布处 / 全程 `RFS_NOW_NS()` 差 | **换片频率/代价**（hint 局部性；÷switch=单次均耗时）|
| `bm_rec_attach` / `bm_rec_drain` | `cs_free_interval` 挂账 / `cs_drain_chunk`+`cs_chunks_asyn` 摘账 | **离线还账压力**；净差=尚未还账挂账 |
| `bm_pool_empty` | `cs_free_interval` 记录节点堆分配失败 | 归还记录**资源耗尽兜底**（旧固定池 → 现堆）|
| `bm_summary_skip` | `cs_alloc_hard/soft/occupy` ALL_USED 免载入 + `cs_test_interval` 摘要裁决 | **摘要是否挡住 ENOSPC 扫描悬崖** |
| `bm_scan_ns` | `cs_enable` 整片读计时 | mount 扫描耗时（CHUNK ctor 唯一 I/O）|
| `bm_ttl_online` / `bm_ttl_drain` | `cs_chunks_asyn` 态1（在线脏窗）/ 态2（离线还账窗）| TTL 三态之同步量；态3 跳过不计 |

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
| `err_enospc` | 位图分配失败上抛（dentry/iomap）| 与 `bm_alloc_fail` 配对（底层/对外）|
| `err_other` | 其余未归类错误返回 | 兜底；`err_*` 全家和不应有黑洞 |

### K. 生命周期（`life_`，4）

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `life_mount` / `life_umount` | `ravenfs_fill_super` 成功末尾 / `ravenfs_put_super` 入口 | 挂载/卸载次数（配对校验；读到时应 `life_mount==1`）|
| `life_iget_new` | `ravenfs_iget` 的 `I_NEW` 分支 | **inode churn** |
| `life_evict` | `ravenfs_evict_inode` | 驱逐次数；`iget_new−evict`=当前在 core 净增 |

### L. 名字堆（`nh_`，6）—— v0.1.2 新增（`name_heap.c`）

> 名字堆**不做直接磁盘 I/O**（其尾部数据经 mdata 片，已并入 `md_`/`io_`、块位图并入 `bm_`）→ 本组只记「名字堆」这一层自身的操作面，使「全模块」每模块皆有计数器。

| 字段 | 打点位置 | 要回答 |
|---|---|---|
| `nh_alloc` | `ravenfs_name_heap_alloc` 入口（大名字分配）| 长名/大目录消耗规模 |
| `nh_alloc_fail` | 同函数 `-ENOSPC`（当前粒度档全满）| 堆容量压力（配 `ext_`/`bm_` 判碎）|
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
| 分配失败率 | `bm_alloc_fail / bm_alloc_calls` | ENOSPC/碎片 |
| 软分配占比 | `bm_soft_ok / bm_alloc_ok` | 软分配路径是否真在用 |
| 换片率 / 单次换片耗时（CHUNK）| `bm_chunk_switch/bm_alloc_calls`；`bm_chunk_switch_ns/bm_chunk_switch` | hint 局部性 / 换片成本（A/B 判据）|
| record 净挂账（CHUNK）| `bm_rec_attach − bm_rec_drain` | 离线还账是否跟得上 |
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
# [bm]
bm_alloc_calls	90112
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
- **分组小节头 `# [prefix]`**（v0.1.2）：`prefix` 取名字第一个 `_` 前（`op`/`ic`/`md`/`ttl`/`io`/`bm`/`ext`/`dir`/`xa`/`acl`/`err`/`life`/`nh`）。**纯注释行**，解析用 `awk '!/^#/'` 仍得纯 kv；勿把小节头当数据。
- **`# state` 段**：gauge（`shutdown` + 位图后端）。`bitmap_mode` 让读侧知道该按哪套 `bm_` 子集解释（避免把「恒 0」误读成「没打点」）。

### 7.2 生命周期（**顺序是重点**）

| 时机 | 动作 |
|---|---|
| `ravenfs_init` | `ravenfs_dbg_root = debugfs_create_dir("ravenfs", NULL)`（`ERR_PTR` 忽略——`kernel.debugfs=off`）|
| `ravenfs_fill_super` 成功末尾 | `sbi->dbg_dir = debugfs_create_dir(sb->s_id, ravenfs_dbg_root)` + `debugfs_create_file("stats", 0444, sbi->dbg_dir, sb, &fops)` |
| **`ravenfs_put_super`，`kfree(sb->s_fs_info)` 之前** | **`debugfs_remove_recursive(sbi->dbg_dir)`** |
| `ravenfs_exit` | `debugfs_remove_recursive(ravenfs_dbg_root)` |

⚠️ **UAF 红线**：`stats` 文件 `i_private` 持 `sb` 指针 → `debugfs_remove_recursive` 必须**早于** `kfree(sbi)`；`debugfs_remove` 会等进行中的 read 结束（dentry active ref）→ 该顺序充分。
⚠️ **`kernel.debugfs=off`**：`debugfs_create_dir`/`_file` 返 `ERR_PTR` → **忽略不拒挂**（`IS_ERR_OR_NULL` 守卫）。

### 7.3 为什么只读、不给 `clear`（S6）

- reset 需并发读写语义（读者可能读到半清零表），为低频操作引入一致性问题不划算；
- 「重看一轮」的自然操作是 `umount && mount`，与 S3 语义一致（`life_mount` 顺带证明重挂过）。

### 7.4 root-only（明说不是 bug）

debugfs 挂载点 `0700`，普通用户读不到。**这是特性**：调试探针不是 ABI，字段增删改名**无兼容义务**（S2）。

---

## 8. 存储布局与成本

```c
/* ravenfs.h：紧邻 struct ravenfs_sb_info 之前 */
struct ravenfs_sb_stats {   /* 104 × atomic64_t = 832 B */
    atomic64_t op_lookup;   /* A 组 */
    /* … 按 A–L 分段、段间空行 + 注释 … */
    atomic64_t nh_release;  /* L 组 */
};
struct ravenfs_sb_info {
    /* … */
    struct ravenfs_sb_stats stats;   /* 随 kzalloc(sbi) → mount 即清零（S3）*/
    struct dentry *dbg_dir;
    u64 mounted_at_ns;
};
```

- **内存**：104 × 8B = **832 B / mount**，多挂载线性叠加，可忽略。
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
| 位图 | ✅（chunk read / mem load）| ✅（chunk write / mem flush）| `bm_`（18）| |
| 名字堆 | （经 mdata）| （经 mdata）| `nh_`（6）| 无直接 I/O |
| **盲区** | `ravenfs_bitmap_used`（statfs 整图直读，O(bitmap)，未记以免 statfs 污染 io_read）| — | — | §11 |

**口径诚实**：`io_` 只记「我们代码显式发起的点」；iomap 默认 bio、statfs 整图直读、挂载期 SB 初读 **不计**，字段注释与本节均已标「覆盖率不完全」，**不假装全量**。

---

## 10. 验收判据

1. **形状**：mount 后 `cat …/stats` 返回**全部 104 字段**、初值全 0（`life_mount==1`），含 `# [group]` 小节头与 `# state` 段。
2. **单调性**：跑 `rfsfilechurn`/`rfsdirchurn` 一轮，相关计数器**只增不减**，且 `ic_hit+ic_miss>0`、`io_read_ops>0`、`io_write_ops>0`（证明真接上，不是摆设）。
3. **派生自洽**：§6 命中率落 (0,1)；`bm_alloc_ok+bm_alloc_fail==bm_alloc_calls`（P5）；**v0.1.2 加**：`io_write_bytes ≥ ic_wb_rows×4096`（行写已被计入）之类方向性校验。
4. **生命周期安全**：`umount` 时 debugfs 目录消失；**反复 mount/umount + 并发 `cat`** 无 crash / 无 KASAN UAF。
5. **行为零变化**：现有验收（xfstests `-g quick` 等）**全绿**——计数器不许改变任何返回值与路径（P1）。
6. **开销**：同 workload 打点前后重编对比，吞吐差 **< 2%**。
7. **随模式（v0.1.2）**：MEM 挂载下 `# [bm]` 的 CHUNK 专属 9 字段恒 0；CHUNK 挂载下 `bm_chunk_switch>0` 等生效面正确（`bitmap_mode` 与实际一致）。

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
- **v0.1.2 为什么加 `nh_`**：S8「全模块」——名字堆是唯一无自有计数器的模块；其 I/O 已被 `md_`/`bm_` 覆盖，故只补该层操作面。
- **v0.1.2 为什么把 `bm_` 做成随模式**：位图重构成 sb-free 类（`rfs_bm`），类只用 sb-free 事件出口注入；非本后端专属字段恒 0（合法、有读侧 `bitmap_mode` 消歧）。

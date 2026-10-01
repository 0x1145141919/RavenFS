# RavenFS Linux KO · 缺陷名单（Defects）

> 2026-09-29 · Raven（2026-09-30 修订：D-2 部分落地 / D-9 修复；**2026-10-01 修订：去版本化 + 并入 D-10**）
> **定位**：本文件是 **linux-ko 的缺陷总名单**（跨版本累积），**不按版本归类**。
> **口径（2026-10-01 定，用户裁定）**：缺陷的组织轴是「**当前处理状态**」，**不是版本**。缺陷真正关心的是「**现在处理到哪一步**」；版本号只作「首次暴露 / 修复落点」的**附注**，不作分类依据。
> **为何去版本化**：有些缺陷是**设计期就被忽视的点**——把它归到 0.1.0 / 0.1.1 / 0.1.2 **都是错的**（例：D-10，`i_ino` 契约从第一天起就不存在）。故本名单**不设版本归属**。
> **首次成文方法（D-1~D-9）**：以 **v0.1.1-1 打包源**（`/usr/src/ravenfs-0.1.1/linux/`）为准，与 **repo mainline**（`PS_git/OS_pj_uefi/ravenfs/linux/`）逐条对照；`当前状态` 一栏标明修复是否已在 mainline 落地 / 部分 / 未做 / 决策已定。
> **前身**：原 `ravenfs-linux-ko-v0.1.1-defects.md`（当初以 v0.1.1-1 发布包为首次对照基线）。

---

## 总览

| 编号 | 缺陷 | 类别 | 严重度 | 当前状态 |
|---|---|---|---|---|
| **D-1** | **无 sb 级性能计数器 / 无 debugfs 出口** | 可观测性 | 中 | 代码已落（`a358cc0`），**未随包发布** |
| **D-2** | **位图分片器全量常驻（80MB），内存浪费 + 30s 全量回写** | 内存/写放大 | 中（对 SparrowOS 目标机为硬伤） | **ko 已落地 CHUNK 后端**（`rfs_bm` 类 + 挂载选项 `bitmap_mode=chunk`；运行时常驻内存 ↓）；「30s 全量回写」未收敛（**2026-09-30 用户接受**，NVMe SLC 写量小） |
| **D-3** | **mount / umount 时间戳不回写** | 正确性（元数据） | 低-中 | **已修 + 已提交**（`linux/super.c` 时间戳回写；随 `8b32497` 入仓）——仍未随包发布 |
| **D-4** | **O_DIRECT（`direct_IO`）故意留白 → `open(O_DIRECT)` 被拒** | 能力缺口（设计级留白） | 低（实验期）→ 中（长期通用） | **mainline 有意不做**（R-FD-10「可降级」） |
| **D-5** | **ko 移植时降级：丢掉 god 的叶内 extent 融合（四形态）→ 写请求粒度即碎片** | 功能缺失（移植降级） | 中（元数据膨胀 / 读放大 / 回收负担） | **ko 无对等实现**（仅「出口坍缩」单条特例；**god 侧已完备**） |
| **D-6** | **inline→extent 不试 direct：>160B 文件恒背 8K 区段树节点（单 extent 也背）** | 元数据膨胀（实现缺口） | 中 | ko 无（`iomap.c:195` 恒建树；direct 仅 truncate 坍缩） |
| **D-7** | **分配 hint 不持久化：每次 mount 重置到片首 → 低地址被反复复用** | 局部性 / 恢复友好度（实现缺口） | 中 | ko 无（hint 从不落盘） |
| **D-8** | **元数据与数据共用分配 hint：区段树叶插进数据流 → 自噬式碎片（2 块洞）** | 碎片放大（分配器耦合） | 中 | ko 无（同一 block 分配器/hint） |
| **D-9** | **挂载参数只进不出：无 `show_options` 回读出口（`row_size=` 静默）** | 接口完整性 / 可观测性 | 低（实验期）→ 中（长期通用） | **已修**（`ravenfs_show_options` 落地，回读集与接受集同构；**2026-09-30**） |
| **D-10** | **`i_ino` / `st_ino` 契约从未被定义（ABI「飞行」事故）** | **契约缺失（设计级 ABI）+ AI 协作事故** | **高（ABI 级）** | **决策已定（`i_ino = idx+1`）；实现待 v0.1.3** |

> **四类性质，勿混**：**D-1~D-3 = “发布落后 mainline”**（mainline 已有修复，未进包）；**D-4 = “有意留白 / 能力缺口”**（非发布落后）；**D-5 = 移植降级**（god 有完整实现，ko 移植时丢掉）；**D-6~D-8 = ko 实现/分配器层缺口**（D-6/7 迁移与持久化，D-8 为元数据/数据分配器耦合）；**D-9 = 两版共有的接口缺口**（发布版与 mainline 都缺，**非“发布落后”**）；**D-10 = 契约缺失（设计级 ABI）+ AI 协作事故**——与全部实现级缺陷**不同类**：不是“实现没跟上”，是“**该契约从第一天起就没被定义过**”。
> D-1 与 D-2 有交集（D-2 的 CHUNK 后端需要 D-1 的计数器做 A/B 判据，见 counters 设计 §3.F）。

---

## D-1 计数器缺失（含 debugfs 适配）

**现象**：v0.1.1-1 的模块**没有任何 sb 级性能/健康计数器**，也**没有 debugfs 出口**——挂载后无处观察缓存命中率、写放大、后台是否在转、错误分布。

**证据**
- 打包源 `/usr/src/ravenfs-0.1.1/linux/` 全树无 `debugfs` / `ravenfs_sb_stats` / `stats`（grep 零命中）。
- 包构建时间 `2026-09-28 17:51` **早于**计数器提交 `a358cc0`（`2026-09-28 20:44`）→ 发布版不含该功能。

**应有形态**（真值源：Superblock 级计数器设计草案，*原 `Docs/linux/ravenfs-linux-ko-sb-counters-design.md`*，已随工作文档退役，可从 git 取回：`git show a358cc0:Docs/linux/ravenfs-linux-ko-sb-counters-design.md`）
- 单挂一份 `struct ravenfs_sb_stats`（**98** 个 `atomic64` 字段，≈784 B/挂载），随 `kzalloc(sbi)` 自然清零（= 挂载即归零）。
- 出口：**debugfs** `/sys/kernel/debug/ravenfs/<s_id>/stats`（0444 seq_file，每行 `key\tvalue`，root-only、非 ABI）。
- 生命周期：`fill_super` 建目录 / `put_super` **先 `debugfs_remove_recursive` 再 `kfree(sbi)`**（UAF 红线） / `exit` 摘根。
- 口径纪律：入口记 attempts、返回点记 outcomes（P5）；只增不减、不存派生率（P2）。
- 分组：A `op_`(26) · B `ic_`(9) · C `md_`(8) · D `ttl_`(2) · E `io_`(5) · F `bm_`(18) · G `ext_`(7) · H `dir_`(3) · I `xa_`(10) · J `err_`(6) · K `life_`(4) = 98。

**影响**：调试与调参（TTL/`reap_ms`、写放大、ENOSPC、错误定位）全部失去常驻观测面；D-2 的 CHUNK A/B 判据也无数据来源。

**mainline 状态**
- 代码**已落**：`a358cc0`（98 字段 + debugfs `stats` + 位图 FULL/CHUNK 双后端打点），`super.c`/`ravenfs.h` 可见 `ravenfs_sb_stats`、`RFS_STAT_INC`、`debugfs_create_dir/file("stats", 0444)`。
- **未随包发布** → 修复动作 = **重打包发布**（版本纪律：`linux/PKGBUILD` 的 `pkgver` 单源）。

**验收判据**（设计 §10）：形状（字段全 / `life_mount==1`）、单调性、派生自洽（`bm_alloc_ok+bm_alloc_fail==bm_alloc_calls`）、生命周期安全（反复 mount/umount + 并发 cat 无 UAF）、行为零变化、开销 < 2%。

> ⚠️ **debugfs 适配点**：设计明确 `kernel.debugfs=off` 时 `debugfs_create_dir` 返回 `ERR_PTR` 要**忽略**（不得因此拒挂）；且字段增删**无兼容义务**（非 ABI）。发布前按此复核。

---

## D-2 位图分片器内存浪费（全量常驻）

**现象**：v0.1.1-1 的位图分配器是 **FULL 全量常驻内存**——整张 block 位图 + inode 位图一次性 `vzalloc` 进内核内存；且**周期回写无脏标记，每 30s 全量回写**。

**证据**
- 打包源 `bitmap.c` 文件头自述："双位图分片分配器（**全量常驻内存**，学 god `bitmap_slices`）"、"触发：**TTL 线程每 `bitmap_sync_seconds`（30s）把所有分片全同步一遍**"。
- 打包源无任何 CHUNK/backing 代码（grep `RFS_BM_BACKING|chunk` = 0）。

**量化**（设计 §0，2TB 盘）
- FS 块位图 **64 MB**（512M 位）+ inode 位图 **16 MB**（134M 槽）= **常驻 80 MB**。
- 有效工作集其实 ≈ 每片一个 chunk（分配集中在 hint 邻域、冷区不动）→ 80MB 是"站着茅坑不拉屎"。
- 附写放大：**每 30s 全量回写 80MB ≈ 2.7 MB/s 恒定**（即使无改动）。

**应有形态**（真值源：`Docs/runtime_tech/03_chunk_backed_slices.md`，含 `02_bitmap_slices.md` 母文档）
- `bitmap_slice_t` 底层改为可选 **backing**：`FULL`（现行为，红线基线）/ `CHUNK`（每片一个固定 chunk 缓冲 + 控制器数组 + 懒加载 + 返还记录）。
- **FULL 逐位/逐性能不变**（A/B 红线）；`mgr` 顶层接口签名零变化。
- 收益锚点：n=32 片 × 64K → 常驻 32×64K×2 组 ≈ **4 MB（40×↓）**；chunk=4K → ~256KB；TTL 改**按脏写**，2.7MB/s 写放大消失。
- 崩溃方向全 = **泄漏**（record 丢 / 在线未回写），不引入双分配方向（与 M3.5 同构）。

**影响**：host（32GB）无感；**对 SparrowOS 目标机（小内存）是硬收益**——这也是本次改制的动机。另：恒定 80MB/30s 写放大对闪存寿命与吞吐都是负担。

**mainline 状态（2026-09-30 修订）**
- **ko 已落地**：位图重构为独立 C 类 `rfs_bm`（`linux/bitmap_core.c` 路由 + `bitmap_mem.c` MEM 后端 + `bitmap_chunk.c` CHUNK 后端），驱动侧适配层 `linux/bitmap_adapter.c`（旧 `linux/bitmap.c` **已清除**）；CHUNK 经**挂载选项 `bitmap_mode=chunk`**（`bbco`/`ibco` 选 chunk 几何，默认 3/0）开启。
- **达成**：chunk 模式运行时常驻内存 ↓（懒加载窗口，不再全量 `vzalloc`）——**D-2 的“内存浪费”主诉已解**。
- **未收敛（用户 2026-09-30 明确接受）**：`chunks_asyn` 仍是逐片**整片读+整片写**，未改造为**按脏写** → “30s 全量回写”的写放大仍在；用户判断对 NVMe SLC 为毛毛雨，**接受**。故本项记**部分达成**。
- **旧口径作废**：早先 `linux/bitmap.c` 的 `bitmap_backing`/`bitmap_chunk_order` 模块参数方案已随该文件清除 → 改为**挂载选项**（与 D-9 回读同源）。
- **god（上帝类）**：仍为移植真值来源（`juvenile_god/bitmap_slices.cpp` v1 / `bitmap_slices_v2.cpp` v2）；A/B 判据（吞吐 ≥95% 且 RSS ↓≥10×）保留为参考。

**关联**：`Docs/runtime_tech/{02_bitmap_slices.md,03_chunk_backed_slices.md,03_progress.md}`；ko 侧落点见 counters 设计 §3.F（F 组 18 字段专为 FULL/CHUNK 双后端打点）。

---

## D-3 mount / umount 时间戳不回写

**现象**：挂载/卸载**不写回卷生命周期时间戳**——盘上 `mount_time` / `umount_time` 恒为 0 哨兵，`modify_time` 也不更新。用户态 `sbinfo` 读出来永远判"从未挂载"。

**证据（本次会话现场）**
```
$ sudo sbinfo /dev/nvme1n1
born   : 2026-09-28 00:38:29 (1790527109)
mount  : never (0)
umount : never (0)
modify : 2026-09-28 00:38:29 (1790527109)   ← 与 born 相同
state  : 从未挂载（或挂载路径未回写时间戳）
```
- 打包源 `/usr/src/ravenfs-0.1.1/linux/super.c` 中 `mount_time|umount_time` **0 次出现**（该功能整体不在发布版）。

**应有形态**（真值：`Docs/disk_layout_v1.1.md` §15 四时间戳语义；已退役规格 `ravenfs-linux-ko-requirements-spec.md` 承接）
- `mount_time`：**非 readonly** 挂载成功时写 `now`（`fill_super` 成功末尾）。
- `umount_time`：**干净卸载**时写 `now`（`put_super`，在**全部元数据写回之后**），并过一道 flush 屏障过掉电写缓存。
- `UMOUNT 0 哨兵`的读侧语义：`mount_time != 0 && umount_time == 0`（或 `mount > umount`）⇒ **上次非正常关机**——这是无 fsck 下判"脏卸载"的关键信号。

**影响**（低-中，但影响面明确）
- 元数据**自描述失效**：盘面无法回答"上次是否干净卸载 / 现在是否挂载中"；
- 连累**非正常关机检测**（panic 检测 / 恢复判据）——本应靠这对时间戳兜底。

**mainline 状态（2026-09-30 修订）**
- **已修 + 已入仓**：`linux/super.c` 实现 `ravenfs_sb_write_timestamp(sb, is_mount)`（`mount_time`/`umount_time` 二选一写 + crc 重验 + flush 屏障）；`fill_super` 写 mount（非 readonly；`sb_write_timestamp(sb,true)` @:1321）、`put_super` 写 umount（在**全部元数据写回之后**；`sb_write_timestamp(sb,false)` @:786），含 `mounted_at_ns` 哨兵耦合与失败日志。
- **入仓**：随 `8b32497`（位图模块拆分那次）一并提交。
- **仍缺**：**未随包发布** → v0.1.1-1 仍缺此功能（待下一版重打包）。

---

## D-4 O_DIRECT 故意留白（设计级留白 · 能力缺口）

**现象**：`open(O_DIRECT)` 被拒——`ravenfs_aops`（iomap 家族）**无 `.direct_IO`**，VFS 在 `fs/open.c` 直接返 `-EINVAL`（**不经过 write**）；另有 `linux/iomap.c:296` 的 `if (flags & IOMAP_DIRECT) return -EOPNOTSUPP` 作兜底。

**定性（关键：与 D-1~D-3 不同类）**
- **不是正确性缺陷**：loud 拒绝（**绝不**静默走 page cache）、有先例（**ext2 无 `direct_IO`**）、是已裁的「可降级」项（R-FD-10，优先级 C）——符合第二约束「顺位 2 降级功能」。
- **是能力缺口（capability gap）**：长期“通用使用”下 O_DIRECT 是**真实用户态用法**（PostgreSQL / MySQL `innodb_flush_method=O_DIRECT`、`dd oflag=direct`、qemu-img/libvirt 部分路径、备份/基准工具）。

**为什么“长期”就不再是合规留白**
1. **降级的合理性基础已消失**：留白原因为 M4 早期**未接 iomap**；现 `ravenfs_aops` **已是 iomap 家族**，而 iomap direct IO 是**内核成熟框架现成件**（`iomap_dio_rw` + `iomap_dio_ops`）→ 按第二约束「框架优先」，**关掉它才是“照抄框架”**，留着反成“逆框架”。
2. **成本不对称**：iomap 就位后，补 O_DIRECT ≈ 接线（对齐 / 短读 / 串行化为标准细节），**边际成本低、收益（用户态覆盖 = 第一约束广度）明确**。
3. **缺出口条件**：第二条边界要求“降级要有退出条件”，而本条**没有触发判据**——这是它真正的缺口。

**必须守的边界（否则会从“缺口”滑成“真缺陷”）**
1. **永远 loud**：不得改成“接受 O_DIRECT 但偷走 page cache”——那才是静默语义违反（真缺陷）。
2. **errno 口径统一**：现两处不一致——`fs/open.c` 给 `-EINVAL`，`iomap.c:296` 给 `-EOPNOTSUPP`；按 ext4/xfs 惯例**选一个**（用户态对两者反应不同）。

**方向 / 出口**
- 实现：挂 `iomap_dio_rw` + `iomap_dio_ops`（框架现成件，“照抄”路径，非“自创”）。
- 出口条件（待补）：如“声称通用 root FS / 出现真实 O_DIRECT workload 且其失败不可接受”时 → 必须实现。

**mainline 状态**：**有意不做**（D-M4-5 后置，R-FD-10 标 C/可降级）；**非**发布落后。

---

## D-5 ko 移植降级：丢掉 god 的**叶内 extent 融合**（四形态）→ 碎片化

**现象**：v0.1.1-1 的 ko **不做任何相邻 extent 融合**——每个写请求（及其覆盖的洞）独立成一条 extent，即使**逻辑紧贴 + phys 连续**也不合并。god（用户态）有「**叶内融合四形态**」。结果：一份 `cp` 拷入的 32.5 GiB 文件产生 **133351 条 64 块（256 KiB）extent**。

**证据（本次会话实测真实盘 `/mnt/huge_data`）**
- `ravenfs-vm-disks/root-ext4.qcow2`（ino **23003140**，size `34957164544`）：**133351 条 extent，条条 `len=64`（=256 KiB）**；逻辑零洞、物理 1042 段；段间 **775 个洞恰为 2 块（8 KiB）** —— 即该文件 **extent 树的叶节点被插进了数据流**（首叶 `blk=91866244` 正落在首个 2 块洞）。
- **256 KiB 写粒度来自 coreutils `cp`**（`IO_BUFSIZE = 256*1024`，`src/ioblksize.h`）；ko `ravenfs_iomap_begin_write` 洞分配 `cnt = hole_end - blk` → **extent 长 = 写请求长**。
- **打包源** `/usr/src/ravenfs-0.1.1/linux/extent_write.c:14`：「**无四形态融合：与既有 extent 紧贴承载 phys 连续时不自动合并（v1 允许碎片，交后续收敛）**」；`ravenfs_extent_insert`（636 行起）仅左右邻**重叠防御**，无合并分支。
- ko 唯一的写回期收敛 = `linux/file.c:568` **`rfs_extent_try_collapse`**（出口坍缩）：**根叶 && count==1 && 单条覆盖 [0,ceil)** → 缩 direct。多条文件（本例 133k 条）**永不触发**（`cnt != 1` 直接 return）。

**god 应有形态（真值源：`juvenile_god/extent_map_unmap.cpp:92` `RavenFS_juvenile::extent_map_range`）**
- **叶内融合四形态**（2026-09-07 完备）：`none` 纯插 / `left` 扩 GLB 尾 / `right` 并入右邻头 / `both` GLB 扩长吞右邻（count−1）。
- 判定：`L = (GLB.fblkbase+GLB.length == ext.fblkbase) && (GLB.block_base.ptr+GLB.length == ext.block_base.ptr)`；`R` 同构（**紧贴 + phys 连续**）。
- **刻意不跨节点**：`L/R` **均为叶内判定**，源码注释「**跨界一律不做（L/R 均为叶内判定）**」——融合只做「同叶」这一层，跨叶紧贴保持分离。
- 融合在 **pwrite 管线的统一写回点**（phase2 map）随插入自然发生（`Docs/pwrite_pipeline.md` §2；演进：`7bde35c` 降级为纯插 → `2bfda89` 左融合 + 激进重建 → `35f1158` 叶内四形态完备）。

**影响**：ko 同 workload 下**碎片数 ≈ 写请求数**（本例 133k 条，god 叶内融合后应大幅收敛）；**元数据膨胀**（本例 ≈ 794 叶 × 8K ≈ **6.2 MiB** 只为描述一份几乎连续的 32 GiB 文件）、**读放大**（每 256 KiB 跨一条树记录）、**回收/恢复的元数据负担**上升。
> 附注：对**数据恢复（内容雕复）**而言，细粒度映射反而更友好（局部覆盖最多丢 256 KiB 单元）——这是同一设计的双面。

**mainline 状态**：**ko 无对等实现** —— `linux/extent_write.c:14` 明确留白「**交后续收敛**」，后续只落了单条特例「出口坍缩」（`file.c:568`）；**god 侧已完备**。→ **修复 = 把 god 的叶内四形态（不跨界）移植进 ko 的 map 路径**（顺带：`hint` 持久化，避免每次 mount 从片首重开吃——见会话记录）。

**现场验证（同日 09-29/09-30 恢复实验）**

- **Lv1（元数据）= 几乎全灭**：删掉的 `GTAV Source.rar`（size `5965527705`）在根 dentry 无条目、inode `size` 精确指纹扫描 **0 命中** → inode 槽已被回收/清零。
- **Lv2（数据）= 基本全可雕复**：以 `~/Downloads` 完整件做 4K 块指纹，全盘 1.79 TiB 命中 **1456379/1456428 = 100.00%**；重建件与原件等长，`cmp` 差异 **200407 B = 0.0034%（仅 49 块）**。4K 落点：1 孤立 @128.5MB、**30 块 @逻辑块 395553–395611「严格隔块」（8K 周期簇，正是 D-5 的树–数据交错同源现象）**、18 块 @5.70–5.79GB 三段。
- **时间线（GMT+8，打捞自 OpenClaw 原始 session `5db354d8`/`477c1faf`）**：
  - `09-29 15:37:26` 用户**声明**把 gta5 源码/权重移到 ravenfs（“安全的地方”）；`15:38:20–15:39:08` 执行 `PS_git/GTAV Source.rar → /mnt/huge_data/GTAV Source.rar`。
  - `09-29 16:16:26 → ~16:37` **刻意在 ravenfs 上跑 `makepkg -si` / `make all -j22` 全量内核编译（linux-7.2.6，树在软链的 `/mnt/huge_data/custom-kernel/…`）**（压测 ravenfs；实测写 ~31 MB/s）。
  - `09-29 17:28:53` 打开解包源码 `/mnt/huge_data/GTAV Source/GTAV Source/`（19G）。
  - `09-29 23:30` 恢复实验启动；`09-30 00:02` 全量 100% 命中；`09-30 00:28` cmp 差异 200407 B。
- **因果**：删除后同日大量写入 ravenfs（19G 解包 + 16:16–16:37 全量内核编译）复用被归还块 → 造成上述 49 块丢失。

**关联**：`Docs/pwrite_pipeline.md` §2；`Docs/lesson_map_fusion_antikiss.md`（融合反 KISS 事故 + 降级史）；`juvenile_god/extent_map_unmap.cpp:92`（四形态真值）；`linux/extent_write.c:14` · `linux/file.c:568`（ko 现状）。

---

## D-6 inline→extent 迁移不试 direct：>160B 文件恒背 8K 区段树节点

**现象**：任何长过 160B 的文件（无论最终几条 extent）**都带一个 8K 区段树根叶节点**；即便它就是一条连续 extent、本该走「direct 零节点最优路径」。实测（`fs/ecryptfs/`，`stat`）：

| 文件 | size | 实占 | 拆解 |
|---|---|---|---|
| `dentry.c` | 2003B | **12K** | 1 块数据(4K) + 2 块树节点(8K) |
| `debug.c` | 2963B | **12K** | 1 + 2 |
| `kthread.c` | 4382B | **16K** | 2 + 2 |
| `super.c` | 5728B | **16K** | 2 + 2 |
| `read_write.c` | 8157B | **16K** | 2 + 2 |

> 规律：**+8K/文件**，与 extent 条数无关。4K 数据的文件 → 实占 12K（3×）。

**证据（打包源）**
- `iomap.c:195 ravenfs_write_migrate_inline` 结尾写死 `RFS_INO1_SET_DIRECT(*r, 0); r->data_root = root;`——**inline 跨 160 一律建树，从不建 direct**。
- 全 ko **唯一 `direct=1`** 在 `file.c:439`（`rfs_commit_inode`，`new_direct?1:0`）；其**唯一 `new_direct=1` 调用者** = `file.c:568 rfs_extent_try_collapse`（出口坍缩），只在 **truncate/setattr「出口 3」**（`file.c:888`）触发。
- 即 **extent→direct 提升只发生在 truncate**；cp/make 生成的「一路 grow、从不 truncate」文件**永远停在区段树**。

**影响**：小文件元数据 2–3×（4K 数据 → 12K 实占）；内核源码树 8 万文件量级 ≈ **数百 MB 纯树节点**；`du` 严重失真。

**应有形态**（不变量 24：direct = 单条连续、零树节点的最优路径）：inline→外置时若**可连续分配**就直接落 direct；否则至少给写路径补一个「单条覆盖 → 坍缩 direct」。

**mainline**：ko 无对等实现；修复 = 迁移/增长时先试 direct，或补写期单条坍缩。

---

## D-7 分配 hint 不持久化：每次 mount 重置到片首

**现象**：位图分配游标 `hint` **纯内存、从不落盘**；每次挂载按 slice 初始化 `hint = (CHUNK)?0:base` 归零 → **每个挂载周期分配从片首重开**。

**证据**
- `bitmap.c` 所有 hint 写点（352 初始化 / 401·490·1104·1133 alloc / 540·1190 occupy）**没有一个在 free 路径**；`hint` 在**整个 ko 只出现于 `bitmap.c`**（`super.c` 零出现）→ 无持久化入口。
- free 只清位 + `free_count++`（“只进不退”注释在 occupy，不在 free）。

**影响**
- **低地址区被反复优先复用**（跨挂载“热点”）→ 越靠片首的数据越先被回首（对“删除后残存”是负面）；本次 `GTAV` 的 18 块尾部丢失即此模型的实证方向。
- 跨挂载空间局部性意图落空（重启后从头开吃）。

**应有形态**：hint 持久化（SB/backup 专字段或 slice 表），挂载时从上次位置续。

**mainline**：无。

---

## D-8 元数据与数据共用分配 hint：区段树叶插进数据流（自噬式碎片）

**现象**：文件**数据**与它的**区段树节点**走**同一个 block 分配器 / 同一个 hint** → 树节点（8K = 2 块）被夹进数据流，把数据**打出一串 2 块洞**。

**证据**：`ravenfs-vm-disks/root-ext4.qcow2`（ino 23003140）区段统计：1042 个物理段、**775 个洞恰为 2 块（8K）**；其首叶 `blk=91866244` **正落在首个 2 块洞**（与 D-5 的 13.3 万条 64 块 extent 是同一全局现象的两面）。

**影响**：物理连续性被自身元数据破坏；碎片计数放大；**恢复时呈“隔块丢失”**（本次 `GTAV` 的 30 块 8K 周期簇即此）。

**应有形态**：元数据分配与数据分配**分离 hint**（或节点分配不与数据流共用前移游标）。

**mainline**：无。

---

## D-9 挂载参数只进不出：无 `show_options` 回读出口

**现象**：模块**接受** `row_size=` 挂载选项（解析后落 sbi），但 `super_operations` **无 `.show_options`** → VFS 读 `/proc/mounts` / `/proc/self/mountinfo`、`mount`/`findmnt` 回显时**拿不到任何 ravenfs 自有选项**；`mount -o remount,…` 与一切“复制/回读挂载选项”的工具链都看不到当前生效值。结果：`row_size=8k` 挂出来的盘与默认盘在**用户可见面完全无差别**——**参数只进不出**。

**证据**
- 打包源 `/usr/src/ravenfs-0.1.1/linux/super.c`：`show_options` **0 次出现**；`grep -rn show_options /usr/src/ravenfs-0.1.1/linux/`（含 `*.[ch]`、去 `.o`/`.cmd`）**= 0**（全树）。
- 同一打包源**已有 row_size 入口**：`ravenfs_fs_context{row_size}`（:522）、`ravenfs_fs_parameters[]`（:530）、`ravenfs_parse_param`（:559）、`ravenfs_fill_super` 消费（:937）——即“**吃选项有、回读选项无**”。
- mainline `linux/super.c` 同样：`ravenfs_sops`（:810）仅 `alloc_inode/destroy_inode/write_inode/evict_inode/put_super/statfs/sync_fs`，**无 `show_options`**；全仓 `grep -rn show_options linux/*.c *.h` = 0（`git log -S show_options` 的命中全是退役文档，无代码）。
- 需求真值（已退役规格，可 `git show 0860fad^:Docs/linux/ravenfs-linux-ko-requirements-spec.md` 取回）：**R-LM-08**「`show_options`：`/proc/mounts` 输出挂载选项（参照 `ext2_show_options`）」——优先级 **L**，状态 **❌**（立项以来未落）。

**定性**：**不是正确性缺陷**（挂载本身正常、loud 拒绝非法值、不静默改语义），而是**接口完整性 / 可观测性缺口**（R-LM-08 未落）。与 D-1 同类（观测面缺失），但 D-1 缺的是**性能计数器**，本条缺的是**挂载参数自描述**。

**与既有文档的口径冲突（须厘清）**：`Docs/linux_vfs_iface_spec.md:250` 把 `show_options` 标“**可选**”；但上游 `Documentation/filesystems/mount_api.rst` 为 **MUST**：「If a filesystem accepts mount options, it must define show_options() to show all the currently active options.」——**触发前提正是“吃了 mount option”**。ravenfs 已吃 `row_size=` → 本项在本仓语境下应从“可选”**升为“必须”**（该 spec 行建议一并订正）。

**应有形态**
- `static int ravenfs_show_options(struct seq_file *m, struct dentry *root)`（本树 `super_operations` 签名为 `struct dentry *dentry`，见 `linux-7.2.6/include/linux/fs/super_types.h:102`）；从 `root->d_sb->s_fs_info` 取生效值——`row_size` 存于 `sbi->icache->row_size`（`ravenfs.h:236` → `ravenfs_cache.h:81`）；非默认时 `seq_printf(m, ",row_size=%uk", sbi->icache->row_size >> 10)`。
- 挂接：`ravenfs_sops.show_options = ravenfs_show_options`。
- 纪律：**回读集必须与 `parse_param` 接受集同构**（将来加选项两处同步，防“吃了不回显”复发）；bool/flag 类选项用 `fsparam_flag` + `seq_show_option()` 统一；ro/rw 等 VFS 级选项由 VFS 经 `fc->sb_flags` 自动输出，driver 只管自有选项。

**影响**
- `mount`/`findmnt`/`/proc/mounts` 无法显示 ravenfs 挂载参数 → `mount -a`、fstab 复制、审计脚本、迁移/复现**取不到 `row_size`**（几何参数不可见，实占/容量口径对不上时难定位）。
- 与 D-3（时间戳）+ D-1（计数器）叠加效应：**盘面自描述（D-3）+ 运行时性能面（D-1）+ 挂载参数面（D-9）全线缺失**，ravenfs 挂载在用户态几乎“隐形”。

**mainline 状态（2026-09-30 修订）**：**已修**。`linux/super.c` 实现 `ravenfs_show_options(struct seq_file *, struct dentry *)` 并挂 `ravenfs_sops.show_options`；回读集与 `parse_param` 接受集**同构**：恒显 `bitmap_mode=mem|chunk`，chunk 时显**短名** `bbco=`/`ibco=`（remount 复制不需写全名），`row_size=` 仅非默认时显。v0.1.1-1 发布版仍缺（非 ABI，随下版打包）。R-LM-08 落地。

**验收判据**
- `mount -t ravenfs -o row_size=8k <dev> /mnt` 后，`cat /proc/mounts`（或 `findmnt -o OPTIONS`）**应出现 `row_size=8k`**。
- 默认 4K 挂载时的口径（不显 / 显 `row_size=4k`）**二选一并锁死**，避免同构歧义。
- 非法值仍走 loud 拒挂（`-EINVAL`），与 D-4 的 errno 口径纪律一致。

---

## D-10 `i_ino` / `st_ino` 契约从未被定义（ABI「飞行」事故）

> **类别**：契约缺失（**设计级 ABI**）+ AI 协作事故。**与 D-1~D-9 不同类**：不是“实现没跟上”，而是“**这个对外契约从第一天起就没被定义过**”——把它归到任何版本都是错的，故只按**状态**记。

### 定性（用户口径，2026-10-01）

> *“这个是属于一个 defect，我定性为与 AI 协作的时候没有以正确的方式**汇报并识别**，导致一个**严肃定义设计的 ABI 被飞行**的事故。那个幻觉本质是一个**表皮**。一方面是我当时没有意识到那个是 inode 的成员并且去调研；但是哪个 AI 能够牛逼读心出来用户实际上并没有脑子里面对于那个成员的建模？属于**大跃进的必要的结构性的代价**。当然今天识别出来并且做了选择，预计在接下来的 v0.1.3 进行修复。”*

- **表象**：一次幻觉传播事故（“inode [0,15] 保留区”，commit `94f7c6b` 铲除）。
- **本体**：`i_ino` 是 VFS 成员 / 用户态 ABI，**却从未被定义**。
- **责任两面（结构性，非归罪）**：用户当时**未对该成员建模**（不知其为 inode 属性 / ABI，无从调研）；**AI 无法读心**（无从主动识别）→ **大跃进的必要结构性代价**。缺口 = 流程缺“**对外契约成员清单**”这一环。

### 现象

- `linux/super.c:518` `iget_locked(sb, inum)`、`:434` `inum = inode->i_ino`（随后按磁盘 idx 解释）→ **`i_ino` ≡ 内部数组布局**；
- 全仓 **92 处 `->i_ino` 直接当磁盘 idx 用**（`dentry.c` 57 · `file.c` 13 · `iomap.c` 9 · `super.c` 5 · `xattr.c` 4 · `inode_cache.c` 1）；
- **无任何规范声明此事**（`disk_layout_v1.2_inode.md` §7 不变量 26–30 / §9 未决均无）。

### 为什么是 ABI 级

- `i_ino` → `st_ino` 是用户态契约：POSIX *“A file identity is uniquely determined by the combination of st_dev and st_ino…”*；
- **直接暴露数组下标 = 把内部布局当公开接口**：布局一变，`tar` / `rsync -H` / 备份去重 / `find -inum` / 容器层即破；
- 行业惯例：**有原生号就原生**（ext2/4、xfs、btrfs、ocfs2、isofs、udf 全暴露布局）；**无原生号才合成**（fat/exfat/smb `iunique`）。→ 暴露布局可接受，**但必须显式签“布局冻结为 ABI”**。
- 三个同洞子问题：① `i_ino=0` 用户态哨兵（`is_zero_ino`；glibc 忽略 zero i_ino）；② 32 位上限（`get_next_ino`/`iunique` 的 EOVERFLOW 注释）；③ root=16“保留区”传闻（本无）。

### 事故链（一手证据）

| 时刻 (+08) | 环节 | 事实 |
|---|---|---|
| 09-26 17:53:26 | 工具输出 | `stat / → Inode: 0`、`dmesg root_ino=0` |
| 09-26 17:53:52 | AI 汇报 | 「非良定义项 ②」；“POSIX 里 0 = 已删除”、“`find`/`tar` 可能发牢骚”；**建议 root 放 2**；**“一句话裁决即可，我先按现状继续”** |
| 09-26 18:02:46 | 用户 | 令 root idx = 16 |
| 09-26 18:03:19 | AI·edit | 幻觉：16 → “0..15 保留位”，**冠名“用户裁定”**，写入 `mkfs/mkfs.hpp` |
| 09-26 23:30 | git | `5b5e62d` sweep 入库（**物化**） |
| 09-26 23:35 | AI | 读自己旧输出当“查证结果”→ 写 `RAVENFS_FIRST_ALLOC_INODE=16` |
| 09-26 23:40:09 | 用户 | 追杀令；`23:41` `94f7c6b` 铲除（**仅表皮**） |
| 09-29 19:08 | DS 网页端 | **复现**同幻觉，当场掐灭 |
| 10-01 | 本次 | 溯源见**本体**（契约从未定义） |

### 根因（三层）

1. **协作层（AI）**：“`i_ino` 填什么”被当**自决小问题**，未识别为非良定义、未按第三约束上报；反把猜测包装成“用户裁定”。
2. **成员层（用户）**：`i_ino` 未建模 → 无从调研 / 定义。
3. **结构层（大跃进）**：AI 无法读心；“用户对某成员无建模”时无人能识别 → 速度换来“洞事后才暴露”。**流程缺“对外契约成员清单”**。

### 决策（2026-10-01 定）

**`i_ino = inode_idx + 1`**。
- 保“按号直取”（O(1) 双射）；**一举解 0 哨兵**（idx0→ino1，`st_ino` 恒≠ 0），**不引入保留区**；把“恒等”升级为“定义好的映射”（留接缝）。
- 被否：`i_ino = idx`（撞 0）；真不透明 map/生成器（丢掉直取）。

### 变更面（**不能一行了事**）

1. `super.c:518` → `iget_locked(sb, idx + 1)`；
2. `super.c:434` → `idx = inode->i_ino - 1`；
3. **92 处 `->i_ino`** 收敛为单一访问器（`RFS_INO_TO_IDX` / `RFS_IDX_TO_INO`，新头），**杜绝散落 ±1**；
4. 工具侧（`audit_v1.2` / `sbinfo`）随引；
5. **root 取值不外泄**：`16` 是 **mkfs 的局部硬编码实现**（已在 `mkfs/mkfs.hpp`/`.cpp` 加注 + 宏改名 `RAVENFS_ROOT_INODE` → `MKFS_ROOT_INODE` 以示“局部”）；**不进入任何契约**。消费者一律**读 SB 的 `root_inode`**（普通契约）——故契约里**不写死 root 的具体值**（见下方 31）。

### 契约文本（拟入 `disk_layout_v1.2_inode.md` §7 不变量 31）

> **31（新）· inode 号契约（ABI）**：`st_ino = inode_idx + 1`（盘格式 v1.2 内恒定）；同 FS 内 `st_ino` 唯一，配合 `s_dev` 满足 POSIX「`(st_dev, st_ino)` 唯一标识文件」。**无任何保留区**（idx `0` 与 `1..15` 及其后全部合法可分配）。**根 inode 按普通契约解析：读 superblock 的 `root_inode` 字段**（其具体取值是 mkfs 的局部实现细节，**不进入契约**）。上限：inode 数 < 2^32。**本条为 ABI，v1.2 内不得变更。**

### 状态与验收

- **状态**：**决策已定（2026-10-01）；实现待 v0.1.3**（变更面已盘，未动手）。
- **验收**：`stat /mnt/rfs` 的 `Inode` ≠ 0；首建文件 `st_ino = 1`；`st_ino != 0` 恒成立；`(dev,ino)` 唯一；**grep `->i_ino` 直用清零**；M1–M5 + `audit_v1.2` 全绿。

### 关联

- 一手：session `116be798` / `5cad25bb`；DS `4b847d18` / `82923745`；commit `5b5e62d` / `94f7c6b`。
- 内核：`include/linux/fs.h:782/2994/3007`、`fs/stat.c:89`、`fs/inode.c:1134/1553`、ext4 `special_inodes.rst`、btrfs `BTRFS_FIRST_FREE_OBJECTID=256`。
- 叙述还原（工作区外）：`~/Downloads/ds-research/agent幻觉多轮放大_犯罪现场还原_ravenfs保留区.md`。
- 纪律：`LINUX_KO_DISCIPLINE.md` §2 / §3.3。

---

## 发布建议（D-1~D-3 的统一出口；D-4~D-10 另论）

D-1~D-3 的共同点是：**mainline 已有部分/全部修复，但都没进发布包**。因此：

1. **D-3（已提交，2026-09-30）**：`super.c` 时间戳回写已随 `8b32497` 入仓。**遗留**：仍待**重打包发布**；发布后验收 = 挂载后 `sbinfo` 显示 `mount`=now、干净卸载后 `umount`=now。
2. **D-1**：随下一版**重打包发布**（版本升号按行为级变更），并跑 counters 设计 §10 的 6 条判据（尤其 debugfs UAF 与开销 <2%）。
3. **D-2（部分达成，2026-09-30）**：ko 已可经 `bitmap_mode=chunk` 用 CHUNK 后端（常驻内存 ↓）——内存主诉已解，用户接受；**遗留**：`chunks_asyn` 未改按脏写（30s 全量回写仍在，判断为 SLC 毛毛雨，缓行）。
4. **D-4（非发布动作）**：**backlog 立项** + 补**出口条件**（见该节）；实现走 iomap 现成件，同时统一 errno 口径并保持 loud。
5. **D-5（非发布动作 / 功能补齐）**：把 **god 的叶内四形态融合（刻意不跨界）** 移植进 ko 的 `ravenfs_iomap_begin`/`extent_insert` 路径；单测须覆盖「同叶紧贴 + phys 连续」四形态与「跨叶紧贴**不**融合」两条边界（后者正是 god 的既有语义）。
6. **D-6/D-7/D-8（非发布动作 / 结构改进）**：① inline→外置试 direct（消掉小文件 8K 税，D-6）；② hint 持久化（D-7）；③ 元数据与数据**分离分配游标**（D-8）。三项都可先在 god 侧原型 + A/B 再定 ko 移植。
7. **D-9（已修，2026-09-30）**：`ravenfs_show_options` 已落地并挂接，回读集与 `parse_param` 同构（`bitmap_mode` 恒显、chunk 时 `bbco/ibco` 短名）。遗留：订正 `Docs/linux_vfs_iface_spec.md:250` 的“可选”口径；随下版发布（属行为级变更 → 走第 8 条版本纪律）。
8. 版本纪律：以上任一**行为级**变更 → **`pkgver` 升号**（对齐 `acc9e20` 惯例），否则 DKMS 不重编、`pacman -U` 同版本不生效。
9. **D-10（决策已定 / 实现待 v0.1.3；非发布驱动）**：`i_ino = idx+1`——**跨切面变更**（92 处 `->i_ino`），须**先立访问器**再改；契约文本待入 `disk_layout_v1.2_inode.md` §7 不变量 31。**root 取值不泄露**：`16` 已定性为 mkfs 局部实现（已加注 + 改名 `MKFS_ROOT_INODE`），契约只认“读 SB `root_inode`”这条普通契约。

> 备注：D-1 的设计真值文档已随 linux-ko 工作文档退役（`0860fad`），需要时从 git 取回：`git show a358cc0:Docs/linux/ravenfs-linux-ko-sb-counters-design.md`。

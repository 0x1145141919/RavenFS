# RavenFS Linux KO · v0.1.1-1 缺陷集（Defects）

> 2026-09-29 · Raven
> **对象**：已发布的软件包 `ravenfs-dkms 0.1.1-1`（安装版 `Build Date 2026-09-28 17:51`；DKMS 源 `/usr/src/ravenfs-0.1.1/`）。
> **方法**：以**打包源**（`/usr/src/ravenfs-0.1.1/linux/`，即模块真正的编译输入）为准，与 **repo mainline**（`PS_git/OS_pj_uefi/ravenfs/linux/`）逐条对照。
> **口径**：本文件记录 **v0.1.1-1 发布版的缺陷**；"mainline 状态"一栏标明修复是否已在仓库落地 / 仅设计 / 仅原型。

---

## 总览

| 编号 | 缺陷 | 类别 | 严重度 | mainline 状态 |
|---|---|---|---|---|
| **D-1** | **无 sb 级性能计数器 / 无 debugfs 出口** | 可观测性 | 中 | 代码已落（`a358cc0`），**未随包发布** |
| **D-2** | **位图分片器全量常驻（80MB），内存浪费 + 30s 全量回写** | 内存/写放大 | 中（对 SparrowOS 目标机为硬伤） | 仅设计 + god 原型（`Docs/runtime_tech/03_*.md`） |
| **D-3** | **mount / umount 时间戳不回写** | 正确性（元数据） | 低-中 | WIP 未提交（`linux/super.c` 工作区 +180 行） |

> 三条互相独立；D-1 与 D-2 有交集（D-2 的 CHUNK 后端需要 D-1 的计数器做 A/B 判据，见 counters 设计 §3.F）。

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
- **未随包发布** → 修复动作 = **重打包发布**（版本纪律见 packaging）。

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

**mainline 状态**
- **ko**：`linux/bitmap.c` 已有 CHUNK 化（`bitmap_backing` / `bitmap_chunk_order` 模块参数，默认仍 `FULL`）；但**未随包发布**。
- **god（上帝类）**：`juvenile_god/bitmap_slices.cpp` 正在做 **CHUNK 原型验证**（施工拆刀 ①→④，见 03 文档 §13）。
- **决策口径（用户 2026-09-28）**：**以上帝类为改制锚点，先改 god、A/B 测完再定 ko 移植取舍** → 即 **ko 侧暂以 FULL 保底**，等 god 的 A/B 结论。
- **A/B 判据（预设）**：CHUNK 常态吞吐 ≥ FULL 的 **95%** 且 RSS ↓ ≥ **10×** → 选 CHUNK；否则 FULL 保底。

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

**mainline 状态**
- **WIP，未提交**：当前工作区 `linux/super.c`（相对 `a358cc0` **+180 行未提交**）已实现——`rfs_sb_timestamp_write()`（`mount_time`/`umount_time` 二选一写）、`fill_super` 写 mount、`put_super` 写 umount（含 `mounted_at_ns` 哨兵耦合与失败日志）。
- 即：**修复已在工作区，但既没提交、更没随包发布** → v0.1.1-1 缺此功能。

---

## 发布建议（三条的统一出口）

三条的共同点是：**mainline 已有部分/全部修复，但都没进发布包**。因此：

1. **D-3**：把工作区 super.c 的改动**提交**（小步快跑：一步一提交 + 验收——挂载后 `sbinfo` 应显示 `mount`=now、干净卸载后 `umount`=now）。
2. **D-1**：随下一版**重打包发布**（版本升号按行为级变更），并跑 counters 设计 §10 的 6 条判据（尤其 debugfs UAF 与开销 <2%）。
3. **D-2**：**跟 god 的 CHUNK A/B 结论**再决定 ko 是否默认切 CHUNK（当前保持 FULL 保底）；无论切不切，先把**脏标记**（去掉 30s 全量回写）这类低风险改进纳入考量。
4. 版本纪律：以上任一**行为级**变更 → **`pkgver` 升号**（对齐 `acc9e20` 惯例），否则 DKMS 不重编、`pacman -U` 同版本不生效。

> 备注：D-1 的设计真值文档已随 linux-ko 工作文档退役（`0860fad`），需要时从 git 取回：`git show a358cc0:Docs/linux/ravenfs-linux-ko-sb-counters-design.md`。

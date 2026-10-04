# RavenFS 运行时科技 ⑥ — 新原语的落地面：free_intervals / soft_alloc 及原语缺口

> 2026-10-04 · 状态：**待裁（分析）** · 上游：05_runtime_allocator_cutover.md（硬切已落，宿主编译过、guest 未验）
> 证据口径：**本仓源码实测**（grep 计数 + 逐函数读体），非推断。

## 0. 原语接线现状（全仓 grep 实测）

| 原语 | 外部调用数 | 现状 |
|---|---|---|
| block_alloc_hard | 17 | 主力（数据 + 节点 + 名字堆 + xattr） |
| block_free | 45 | 主力（逐段释放） |
| inode_alloc / inode_free | 3 / 6 | 主力 |
| block_occupy_soft | 1 | iomap 锚定左邻（D-5c） |
| dump_slice | 内部 | checkpoint（allocator_adapter） |
| *_free_total | 2 | **仅 create 的 pr_info 日志** |
| **block_intervals_free** | **0** | 未接 |
| **block_alloc_soft** | **0** | 未接 |
| **block_test / inode_test** | **0** | 未接 |

> 附带发现：sbi->stats.bm_*（18 个计数器）**发射点已归零**（bm_stat_emit 随位图模块删除）
> → debugfs 这 18 行**恒 0 = 静默坏**。见 §3 P0-2。

---

## 1. 假设一：block_intervals_free 用于文件删除「迭代返还」

### 1.1 真实调用面（取证）

删除路径已有一套自带收集的 rfs_freelist（file.c）：

- rfs_fl_add（file.c:310）——**线性扫一遍已有区间做首尾合并 + 相邻桥接**，每段 kmalloc 一个节点；
- 调用点：rfs_release_tree_rec（file.c:403，extent 叶物理段）、rfs_punch_hole（file.c:574，切割段）；
- 释放点：rfs_fl_free（file.c:369）——逐段 (void) rfs_allocator_block_free(...)；
- **树节点块**（2 块）在递归尾部边走边单段 free（file.c:408），未进链。

### 1.2 换 block_intervals_free 的收益（诚实量化）

| 维度 | 结论 |
|---|---|
| 结构 | **真赢**：链所有权转交原语（消费即还节点）、免掉单独 free 循环、**错误从 (void) 变可见**（契约「已消费段不回滚、余段留链」→ 可续排） |
| 去重复合并 | **真赢**：rfs_fl_add 的 O(n²) 合并是**树内 free_interval 的重复劳动**（树侧本有 pred/succ O(log n) 合并）→ raw 喂、树合并 |
| 会话原子 | **微不足道**：原语内部逐段仍各 enter/leave，净省约 3×(N−1) 个 seq_cst 原子（纳秒级） |
| 锁 | 无长持锁风险：逐段取片锁、段间不持锁 |
| 整体性能 | **别指望**：删除是 **I/O 主导**（每树节点 sb_bread + mdata 写回）。立项目标应为可维护性 + 报错可见，**性能需基准证明** |

### 1.3 语义分叉（待拍板）

- **F1 错误口径**：原语 **fail-fast**（首错即返、余段留链），现循环**忽略错误继续还**。
  - 甲（建议）：调用方**有界续排** while (链非空 && iter < cap) → 保持「尽量多还」，单个坏段不再吞其余。
  - 乙：接受 fail-fast（double-free 类 bug loud 化），余段记日志放弃 → 可能泄漏。
- **F2 OOM 口径**：现纪律是「宁可泄漏（WARN_ON）也不破坏『先 inode 后 free』」；
  换 run_list push（kzalloc 失败）须**保留同口径**。

### 1.4 建议（P1）

rfs_freelist → rfs_run_list + 末尾单次 block_intervals_free；同时删 rfs_fl_add 合并（raw 喂）；
树节点块并入同链。验收：audit_v1.2 不变量 + 删除 churn（alloc==free、live==0）+ 删除微基准
（若持平，如实报「结构收益」）。

---

## 2. 假设二：block_alloc_soft 作 hard 的降级

### 2.1 原语语义（读实现取证）

- **hard**：环扫全片找**单片内一段**连续对齐 → 失败 = 全片无一段 ≥ cnt。
- **soft**（slice.c:499）：
  - **碎片优先**：尺寸树最小者起逐段消费，**大块尾巴留给 alloc_hard**；
  - 整段优先 + 贴尾对齐；**片内 push_front、跨片后插 → 输出链无 base 序**；
  - 顶层 **all-or-nothing**：凑不齐即回滚返回 -ENOSPC，**不留半拉**。
- ⇒ soft = 「宁可碎片也要拿到 cnt」且**吃碎屑、留大块** —— 正是降级该有的样子。

### 2.2 唯一结构上直接成立的落点：inline→extent 整域迁移

取证 iomap.c:245 rfs_write_migrate_inline（pos_blk==0）：hard(end_blk) 失败 →
**直接 -ENOSPC**（iomap.c:248，有空间也失败）。消费方是**区段树**
（ravenfs_extent_insert 天然多 extent，随后 RFS_INO1_SET_DIRECT(*r, 0)）→ 降级合法：

    ① hard(end_blk)        → 单 extent（最优）
    ② 失败 → soft(end_blk) → N 段 →【按 base 排序】→ 逐段 insert 成 N extent
    ③ 仍失败 → -ENOSPC

- D-6a direct 路径失败本就降级到此 → 补②即闭环。
- **注意**：iomap 一次只描述 pos 所在一段 → 多段时 iomap->length 按首段裁剪，
  后续 iomap_begin 走区段树 lookup 续取。需写读回 + fsx 验证。

### 2.3 不成立的两处（这才是「通用降级」的真正门槛）

| 消费方 | 为什么 soft 帮不上 |
|---|---|
| **iomap 数据写**（主力） | 代码钉死「单次 iomap_begin 只描述**一条**连续 phys run」；且正常写**按 extent 分配**，单 extent 仍要硬连续 |
| **区段节点预算** rfs_ext_resv_alloc | 要 hard(2,1) = 8K 对齐 **2 连续**；soft 不保证**单段 ≥2**（可能两段各 1 位）→ 用不了 |

⇒ 「hard 失败就退 soft」在两个最大连续性消费者处**无处安放**。

### 2.4 三个原语缺口（待拍板）

- **G1 min_run_len**：soft 加单段最小长度（或「K 段 × 每段≥2 × 对齐」）→ 才能让 8K 节点预算碎片降级。
  **最热消费点，优先级最高。**
- **G2 尺寸降级原语缺失**：没有「最大连续 ≤ cap」（alloc_hard 是 best-fit 且**必须 ≥ cnt**）。
  「给多少写多少」（iomap 短映射/写短一块）是与碎片降级**正交**的第二种降级，需另立原语。
  设计方直觉可能真正指向它。
- **G3 顺序契约不一致**：soft 输出**无序**，dump_slice 输出**按 base 升序** → 喂区段树前**必须排序**，
  否则 D-5 叶内融合「左邻已存在」假设踩空（形态表只有 left/both）。要么调用方排序、要么原语补序，勿沉默。

### 2.5 建议（P2）

先做 §2.2（边界清晰、消费方明确支持多段），写读回 + fsx 验证；G1/G2 单独立项，不与 P1/P2 混。

---

## 3. 顺手的高价值点（比两假设更划算）

- **P0-1 statfs 换活数**：现在每次 statfs() 都 sb_bread 扫**整个位图区**
  （ravenfs_bitmap_used，super.c:561；1TiB = 2^28 块 ÷ 32768 → **8192 次块读**），且读的是 **checkpoint 旧值**。
  换 rfs_allocator_block_free_total / _inode_free_total = O(片数) 内存求和 + **活数**。
  **本轮最划算的一刀。**
- **P0-2 bm_* 计数器已死**：18 个零发射点 → debugfs 恒 0。二选一：调用点补打点，或删 F 组。
- **block_test / inode_test 零调用**：可作**护栏断言**（释放前校验「确为已分配」）或挂 audit，防双 free 静默。

---

## 4. 建议施工顺序（小步、每步可验收）

| # | 内容 | 规模 | 验收 |
|---|---|---|---|
| P0-1 | statfs 换 *_free_total 活数 | 极小 | df 数值对 + 无盘读 |
| P0-2 | bm_* 计数器处置 | 小 | debugfs 非恒 0 / 删净 |
| P1 | freelist → run_list + intervals_free（含 F1/F2） | 中 | 删除 churn + audit + 微基准 |
| P2 | inline→extent 迁移 soft 降级（排序 + iomap 裁剪） | 中 | 写读回 + fsx |
| G1/G2/G3 | 原语补缺口 | 拍板后立项 | 各自单测 + 压测 |

**全部宿主级提案；行为级仍须 guest 压测（纪律 §7）。**

---

## 5. 状态

初版 = **分析待裁**；2026-10-04 设计方已裁 + P1 已落（见 §6）。

---

## 6. 裁决与施工记录（2026-10-04）

### 6.1 裁决

| 项 | 裁决 | 理由（设计方口径） |
|---|---|---|
| **F1 错误口径** | **fail-fast（乙）** | 返还区间重叠只可能是上层作乱 / 原语一致性违例 → 该 loud 炸出，不做有界续排 |
| **F2 OOM 口径** | **保持** | 「宁可泄漏也不破坏『先 inode 后 free』」 |
| **G1 min_run_len** | **枪毙** | 文件数据可降级，但 **B+ 树节点块在磁盘契约头里定死**（必须 8K 连续），无降级余地 |
| **G2 尺寸降级原语** | **故意不做** | 属上层业务，且调用点很少 |
| **soft_alloc** | **暂不用** | §2 各落点暂不立项 |
| **P0-1 statfs** | 框定更正 | 磁盘位图在新模型下是**写回物、不是读源**；statfs 读盘违背「内存权威」→ 模型一致性必须修，**不算性能战果** |
| **P0-2 bm_* 计数器** | **挂起** | 遗老计数器等 guest 测试成功后再议 |

### 6.2 施工：P1 = free_intervals 落地（已实施，宿主编译过）

改动仅 `linux/file.c`（删除侧收账本整体换原语）：

- `struct rfs_freelist` / `struct rfs_frerange` + `rfs_fl_add` 的线性合并**整体删除**；
- 收账本改直用 `struct rfs_run_list`（分配器原语链，绑 `rfs_run_alloc_platform` = GFP_KERNEL；
  收集点**不持片锁** → 不违锁纪律）；
- `rfs_fl_add` → `rfs_run_list_push_front`：**raw 不预合并**（合并归树内 free_interval）；
  取节点失败走 F2 口径（WARN_ON + 放弃该段）；
- `rfs_fl_free` → 单次 `rfs_allocator_block_intervals_free`：**F1 fail-fast**，首错即
  `pr_err` + `WARN_ON_ONCE`，余段节点由 `rfs_fl_put` 回收、区间弃置（交 fsck）；
- `rfs_release_tree_rec` / `rfs_unmap_range` 签名 + 5 处声明
  `struct rfs_freelist *` → `struct rfs_run_list *`。

**行为保持核对**：成功路径链归空（原语逐段取走节点）→ `put` 只 kfree；错误路径 `put`
只回收节点、弃区间 —— 与旧口径一致。punch_hole 成功路径 `fl` 恒空（仅 `out_free_tree`
写入），**无新增泄漏面**（已逐 goto 核对）。

### 6.3 验证（宿主级）

- `make -C linux`（touch 全 .c 全量重编）**rc=0、warning/error = 0**，`ravenfs.ko` 产出。
- 陈旧符号 `rfs_freelist` / `rfs_frerange` 全仓 grep = **NONE**。
- userspace：`test_allocator_router` **207 PASS / 0**（allocs=98 frees=98 live=0）、
  `test_allocator_slice` **98 PASS / 0**。
- **未做（待 guest，纪律 §7）**：删除路径实测 + `audit_v1.2` 不变量对拍 + 删除微基准
  （若性能持平，如实记为「结构收益」）。

### 6.4 后续（按 §4 顺序，未裁不动）

P0-1（statfs 数据源切内存权威，非优化是修复）→ guest 通过后 P0-2 → P2（soft 迁移降级，等 soft 复活）。

# RavenFS — direct_file_* 直写管线设计（pwrite 五步管线 v0）

> 2026-09-04 定稿。代码未落地（`juvenile_god/direct_file_ops.cpp` 仅 ftype + pwrite 骨架
> 注释）；本文是 `direct_file_pwrite` / `direct_file_punch_hole` / `direct_file_trunacate`
> 的实现总纲与纪律，承载完整设计推理——供下一个工作时刻直接续写，不依赖会话记忆。
>
> 配套同批修订（本文的判据/状态机契约以这两份为准，不重复）：
> - `disk_layout_v1.md` §4.1：REG/SOFT_LINK 三态解码判据伪代码化（size 优先，`data_root`
>   取值退出判定）+ 三态不变量表 + 状态迁移 + 翻转写序 + SOFT_LINK 载荷约定（size 不含 NUL）
> - `disk_layout_v1.1.md` §3.2（判据同构收编）+ §10 不变量 22~25 + 修订日志 #14

---

## 1. 定位与总体决策

- `direct_file_*` 操纵文件数据三态（内联 / 直指区间 / 区段树）。一次写可能引发状态迁移
  （终态机七条，§3）；pwrite/punch_hole/truncate 共用同一套底层机制。
- **统一管线决策**（2026-09-04）：不做快速路径分级（L0/L1/L2 分层方案否决——分级是
  浪费脑细胞）。所有写路径走同一五步管线；平直条件（单 extent 内覆盖、纯增长）在管线里
  **自然退化**为"零 unmap + 至多一条 map + 直接写数据"。设计哲学：讨好最变态的边界
  （碎片一地、触及多 extent + 内部空洞，1% 场景），简单条件复用同一代码，正确性只维护一份。
- 五步管线形态（以最复杂的 extent 初始态为蓝本）：
  1. 资源预分配并区间合并（`dfprealloc_and_plan`）
  2. 根据老的 extents 编译一个原语操作序列（先绘制融合图景）
  3. 执行操作序列（map / unmap / 建树）
  4. 文件数据写入
  5. 元数据回写（inode 写入 = 提交点）

## 2. 各 phase 职责与副作用面（核心纪律表）

| phase | 职责 | 允许副作用 | 禁止 |
|-------|------|-----------|------|
| 1 | 规划写逻辑块域的新映射段 + 预分配物理块（含段合并） | 位图（alloc / occupy） | inode / 树结构 / 数据内容 |
| 2 | 读现映射 → 融合图景 → 编译 op 序列 | 无（纯计算） | 一切写 |
| 3 | 执行 op 序列（建树、map、树内分裂级联） | 位图（树节点现用现分）+ 树节点盘面 | inode |
| 4 | 用户数据 + 物化拷贝 + 边界清零落盘 | 数据块盘面 | inode / 树结构 |
| 5 | 一次写 inode（data_root / direct_bit / size / 时间戳） | inode | 其余 |

推论：

- **inode 写入 = 唯一提交点**。phase1~4 的位图亮灯、树节点、数据块全部是"未提交资源"；
  任何 phase 失败 → 逆序撤销（free 未 map 段 / unmap 已 map 段 + free 树根），
  inode 永远保持旧态自洽，永不指向半成品树。
- 与崩溃模型（disk_layout_v1.md §4.1 翻转写序）对齐：① 位图亮灯 → ② 数据/树节点落盘 →
  ③ inode 字段翻转。崩溃于 ③ 前 = 孤儿资源，fsck 回收。

## 3. 状态迁移全景（终态机七条）

（源注释归档：direct_file_ops.cpp 骨架，2026-09-04 转录，实现时注释可删、本文保留）

1. inline→inline：最简单（size 不越 160，纯 inode 内写，零分配零树）。
2. inline→direct：扩容优先试此路，hard_alloc。
3. inline→extent：仅当 inline→direct 升格需要 **>2 个 FS_block** 才允许降级到此，
   soft_alloc。1~2 块时无法"撕成两段"——树根自身 2 块 8K，≤2 数据块建树必亏；
   1 块 hard 都失败只能说明盘满。
4. direct→direct：不触发分配最好；触发则优先 `block_occupy_soft` 尾部占领
   （物理紧贴扩展 → phase3 保 direct）。
5. direct→extent：4 的软占领失败（相邻被占）→ 降级；旧 direct 区**数据零搬移**，
   上树后作为一条 extent（建树 + map 全文件旧区 + map 新段）。
6. extent→extent：待写区间落到 extents 上；遇空洞尽可能从左侧 extent 物理尾向右
   soft_occupy（物理紧贴 → phase3 map 时自动左并），能合并就合并。
7. extent→direct：极罕见——6 的合并使全文件坍缩为单 extent 时，按"尽可能 direct"宗旨
   坍缩。依赖整树释放轮子（§9 待办）；pwrite 第一版可暂缓坍缩（留在树上，语义等价）。

分配策略 = **phase1 内部单一事实源**（固化不注入、不散落）；phase2/3 只见
"已就绪的映射增量"，不重复决策。

## 4. phase1 `dfprealloc_and_plan` 规格

声明（hpp 已加，2026-09-04）：

```cpp
/* pwrite 的 phase1：预分配并区间规划。affect_extents = 有序单调递增链表 */
result_code_t dfprealloc_and_plan(uint64_t inode_idx, uint64_t offset, uint64_t len,
                                  std::list<extent_t> &affect_extents);
```

### 4.1 affect_extents 成员语义（关键决策：纯新段）

affect_extents = **纯新段**：只为写域中落在空洞/增长（旧映射覆盖不到）的逻辑块分配
物理块，输出"待生效的映射增量"列表。旧 extent 覆盖的部分**不出现在列表中**——保留，
phase4 直接往旧物理块写数据。

否决"写域最终分段"与"受影响旧段 + 新段"两案的推理：

- pwrite 本质上**不需要 unmap 任何旧 extent**：覆盖写不搬数据，旧映射保留即可。
  于是 op 序列 = phase1 输出 → map ops（+ 需要时建树/旧区一并上树），phase2 近乎零编译成本；
  会 unmap 的是 punch_hole / truncate，不是 pwrite；
- 回滚记账干净：列表本身就是账本——phase1 失败整体 free；phase3 中途失败 →
  已 map 的 unmap+free、未 map 的直接 free；
- 邻接合并是 `extent_map_range` 的事（左并/右并：逻辑紧贴 + 物理连续自动吞并）——
  phase1 不做树层面合并，occupy 成功的"物理紧贴扩展段"交给 phase3 自动并入旧 extent。

### 4.2 规划域（字节写域 → 逻辑块域，最容易漏的分叉）

- 非 inline 初始态：域 = `[off>>12, (off+len+4095)>>12)`（半开，len > 0）——首尾所在块
  即使只写 1 字节也是整块入域（不存在半块映射）。
- **inline 迁出**（`newsize = max(size, off+len) > 160`）：域扩为 `[0, ceil(newsize/4096))`——
  离开 inline 意味着全文件内容都要物理背书；**头部旧内容（inline_data）的物化分配是
  phase1 的职责**，[0,160) 的字节拷贝才归 phase4。此分叉漏掉 → phase3 建完树发现块 0
  无物理块。
- len == 0 → RC_OK + 空表；offset+len 溢出等参数防御在函数头。

### 4.3 输出不变量

按 `fblkbase` 升序；互不重叠；**与现映射零相交**（map_range 内部重叠即 RC_ERR_OVERLAP，
必须喂干净）；`block_base` 恒有效（52 位物理起点，禁 PTR_INVALID 占位）；
length = 块数（半开语义，对齐 v1.1 区段树）；段数上界 = 域内块数。

### 4.4 原子性与失败

失败 = 已分配段全部逆序 free，无副作用返回（RC_ERR_NO_BLOCK 为主）。phase1 是唯一
"提前花钱"的 phase，其原子性是整条管线可回滚的前提。

### 4.5 分配策略要点

- direct 初始态增长：优先 `block_occupy_soft`（物理尾相邻占领）→ 段物理紧贴旧区 →
  phase3 保 direct；失败 → 段落异处 → 终态上树（旧区一并 map，零搬移）。
- extent 初始态：空洞写优先从左侧 extent 物理尾向右 soft_occupy；文件尾增长同
  tail_infinite gap 处理。
- inline 迁出：≤2 块 hard_alloc 直指；>2 块 hard 优先（连续 → direct），失败 soft 多段
  → extent。
- 数据块分配 align_log2 = 0（8K 对齐只约束树节点/元数据片，数据块对齐是浪费）。

## 5. phase2 — 编译原语操作序列

- 输入：phase1 输出（新段列表）+ 现映射（extent_iterator 游走 / direct 公式）+ 初始态。
- 融合图景：对每条新段判定与旧映射的物理邻接（occupy 是否成功）→ 推出终态形态
  （保持 direct / 上树 / 树内追加）。
- pwrite 的 op 序列 = `[建空根叶?] + map × N`（**无 unmap**；unmap 是 punch_hole/truncate
  的主场）。建空根叶仅在终态为树的场合（inline/direct→extent）出现；树内 map 自带邻接合并。
- ENOSPC 只可能发生在 map 内部分裂（树节点现用现分：2 块、8K 对齐 hard_alloc）→
  phase3 的回滚路径承担，phase2 不预分配树节点。

## 6. phase3 — 执行 + 回滚

- **`root_blk` 引用必须贯穿执行与回滚全程**：第 k 条 map 触发根裂换根后，第 k+1 条失败
  回滚时要用的是最新根，不是 phase 开头那个。
- 失败（map 内部 ENOSPC / IO）→ 逆序：unmap 已生效段（用当前 root_blk）→ free 空根
  （若有）→ phase1 剩余未 map 段整体 free。
- 回滚完成后 inode 未动（旧态自洽）——可逆性由 §2 副作用面纪律保证。

## 7. phase4 — 数据写入

- 解析"写域逻辑块 → 物理块"：先查 affect_extents（新段，含物化块），miss 走旧树
  （walk/iterator）或 direct 公式（base + 逻辑块号）。同一逻辑块不会同时命中两者
  （零相交不变量）。
- 清零契约（disk_layout_v1.md §4.1）：块级空洞天然读 0；新分配块被**部分**写入时，
  块内未写区间必须清零（分配即清 / 首次部分写补零任选）——mmap 直写后端残留旧数据。
- inline 迁出的 [0,160) 内容拷贝在此 phase（物化字节写入新块）。

## 8. phase5 — 元数据回写（提交点）

一次 InodeSession 写：`data_root`（终态：直指基址或树根块号）、`is_data_root_direct_bit`、
`size = max(旧, off+len)`、时间戳。崩溃于此前 = 全部未提交资源成孤儿（位图亮灯块），
fsck 回收；inode 永远指向完整旧态。**禁止 phase1~4 任何一处写 inode**。

## 9. extent 原语 detach 重构（2026-09-04 决策，代码未动）

技术债定性：现 `extent_map_range / extent_unmap_range(uint64_t inode_idx, ext)` 把
"树的结构操作"与"inode.data_root 即时回写"焊死——内部 `extent_new_root_inner` /
`extent_root_shrink` / `extent_balance_from` 每次根更迭都开 InodeSession 写 data_root。
调用面只有"已上树文件的常规增删"时成立（当时测试即唯一调用方）；一旦宿主确定被推迟到
提交点（phase5 才写 data_root），"树一被操作就必须被 inode 引用"的假设即失效。
本质：抽象边界画错在"工具向宿主汇报根更迭"。

目标签名（待重构）：

```cpp
result_code_t extent_map_range(uint64_t &root_blk, uint64_t owner_inode_idx, const extent_t &ext);
result_code_t extent_unmap_range(uint64_t &root_blk, uint64_t owner_inode_idx, const extent_t &ext);
```

- `root_blk` **in/out**：入 = 当前树根，出 = 操作后根（map 根裂 / unmap 根收缩更迭由引用带出）；
- `owner_inode_idx` 降级为**纯审计锚**（根 meta `parent_ptr.ptr` = "." 锚，扫描恢复归属
  线索），**不触发任何 inode I/O**；
- 顶层 inode 态校验（type / direct_bit / data_root 检查）删除——调用方负责"这是文件树、
  root_blk 有效"（map/unmap 仍做 inode_test 防御可留 debug 变体）；
- RC 语义不变：OVERLAP（map 与现映射相交）/ 三字段全等 unmap / NOT_FOUND；
- 内部改动面：上述三个 helper 的 InodeSession 写删净，根更迭沿引用链上抛
  （如 `extent_balance_from(child_blk, owner, root_blk&)`）；`extent_Bptree_walk` 本就收
  root_blk 不收 inode，无需动。

连带缺口原语（inline/direct ↔ extent 转换缺失的拼图）：

- **建空根叶**：alloc 2 块（8K 对齐）+ init 空根叶（meta entry_count=0 / is_root / 叶子 /
  magics / owner 锚）→ 返回 root_blk。inline/direct→extent 的 phase3 第一步；
  现在 extent 侧只有分裂专用 `extent_new_root_inner`，无空树构造（dentry 侧 dir 急建
  树根可参照）。
- **整树节点收集释放**：extent→direct 坍缩、truncate ≤160 回迁 inline、未来 unlink 全量
  回收共用（dentry 删除侧现为 `RC_ERR_NOT_SUPPORTED` 哨兵——全量空间回收从未落地）。
  **单独立项，别塞进 pwrite**；坍缩在 pwrite 第一版暂缓（留在树上，语义等价、只少直指性）。

## 10. punch_hole / truncate 复用

同一五步管线：phase1 规划域 = 打洞区间；phase2 op 序列**含 unmap**（打洞/截断是 unmap
主场：整块释放、残块清零保留）。truncate 缩小内部编译为"尾巴打洞 + size 收口"
（hpp 注），但 **size ≤ 160 强制回迁 inline**（判据 size 优先的必然推论，
disk_layout_v1.md §4.1）：读回 inline_data + 释放全部数据块与树节点 +
data_root = PTR_INVALID——禁止只打尾洞不搬回（会造成"size ≤ 160 却外置映射"的不一致态）。

## 11. 待办 / 开放项

1. extent_map_range / unmap_range detach 重构（§9，含内部 helper 链穿透）
2. 建空根叶原语（§9）
3. 整树节点收集释放轮子立项（坍缩 / 回迁 / unlink 共用，§9）
4. dfprealloc_and_plan 实现（§4 规格）
5. phase3 ENOSPC 逆序回滚路径验证（树节点现用现分）
6. 并发前提：同 inode 写互斥由调用方保证（phase1 读态 ~ phase5 写之间无锁原子性）
7. ftype() 判据边界：**size ≤ 160 内联**（160B inline_data 恰好全用合法；stub 初版
   `<160` 已修为 `<=160`，与 disk_layout 判据一致）

---

> **一句话记**
>
> 直写 = 五步管线一条路走到底：phase1 预分配纯新段（副作用仅位图、失败原子）→
> phase2 画融合图景编 map 序列 → phase3 执行（root_blk 引用贯穿，逆序回滚）→
> phase4 写数据（新段 + 旧树双源解析）→ phase5 inode 一次写 = 唯一提交点。
> 为此 extent 原语必须 detach：arg1 换 `root_blk&`，inode.data_root 回写权收归 phase5。

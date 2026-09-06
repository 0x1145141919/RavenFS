# RavenFS — direct_file_* 直写管线设计（pwrite 五步管线 v0）

> 2026-09-04 定稿；**2026-09-06 修订：phase2/3 职责重排**（phase2 = 新区间逐段 map，
> 砍掉"画融合图景→编译 op 序列"预编译层；phase3 = 融合收敛）。代码未落地
> （`juvenile_god/direct_file_ops.cpp` 仅 ftype + pwrite 骨架注释）；本文是
> `direct_file_pwrite` / `direct_file_punch_hole` / `direct_file_trunacate`
> 的实现总纲与纪律，承载完整设计推理——供下一个工作时刻直接续写，不依赖会话记忆。
>
> 配套同批修订（本文的判据/状态机契约以这两份为准，不重复）：
> - `disk_layout_v1.md` §4.1：REG/SOFT_LINK 三态解码判据伪代码化（size 优先，`data_root`
>   取值退出判定）+ 三态不变量表 + 状态迁移 + 翻转写序 + SOFT_LINK 载荷约定（size 不含 NUL）
> - `disk_layout_v1.1.md` §3.2（判据同构收编）+ §10 不变量 22~25 + 修订日志 #14
> - `extent_merge_range.md`：phase3 融合原语完整设计（2026-09-06 立项，代码未落地）

---

## 1. 定位与总体决策

- `direct_file_*` 操纵文件数据三态（内联 / 直指区间 / 区段树）。一次写可能引发状态迁移
  （终态机七条，§3）；pwrite/punch_hole/truncate 共用同一套底层机制。
- **统一管线决策**（2026-09-04）：不做快速路径分级（L0/L1/L2 分层方案否决——分级是
  浪费脑细胞）。所有写路径走同一五步管线；平直条件（单 extent 内覆盖、纯增长）在管线里
  **自然退化**为"零 unmap + 至多一条 map + 直接写数据"，phase3 融合在此条件下
  自然空转（写域内本就无可并分离记录）。设计哲学：讨好最变态的边界
  （碎片一地、触及多 extent + 内部空洞，1% 场景），简单条件复用同一代码，正确性只维护一份。
- 五步管线形态（以最复杂的 extent 初始态为蓝本）：
  1. 资源预分配并区间规划（`dfprealloc_and_plan`）
  2. **新区间逐段 map**（形态判定一次 → 建树/空根叶（如需）→ map × N 机械落地）
  3. **融合收敛**（对写域/受影响域 `extent_merge_range`，并掉残余可并分离记录）
  4. 文件数据写入
  5. 元数据回写（inode 写入 = 提交点）

  **2026-09-06 重排说明**：旧版 phase2（纯计算画融合图景编 op 序列）+ phase3（执行）
  合并为一步半——预编译层是多余的：终态形态（保 direct / 上树）是全有全无判定、
  依据 phase1 输出段数/邻接即可在入口一次算完，不值得单独一层；"邻接合并"本就不在
  计划期做（map 原语插入时自动左并/右并）。于是 phase2 = 机械落地，原"融合"语义
  显式独立为 phase3 收敛（处理 map 自动合并管不到的历史分离残留 + 为坍缩准备），
  实现用新原语 `extent_merge_range`（设计见 `extent_merge_range.md`，代码未落地）。

## 2. 各 phase 职责与副作用面（核心纪律表）

| phase | 职责 | 允许副作用 | 禁止 |
|-------|------|-----------|------|
| 1 | 规划写逻辑块域的新映射段 + 预分配物理块（含段合并） | 位图（alloc / occupy） | inode / 树结构 / 数据内容 |
| 2 | 新区间逐段 map：入口形态判定（保 direct / 上树）→ 建树或空根叶（如需）→ map × N 机械落地 | 位图（树节点现用现分）+ 树节点盘面 | inode / 数据内容 / 融合动作 / 预编译（禁止"画图景"式计划） |
| 3 | 融合收敛：对写域/受影响域 merge_range，并掉残余可并分离记录（map 自动合并管不到的历史残留；为坍缩准备） | 树节点盘面 + 位图（空叶/旧根归还 free） | inode / 数据内容 / map / unmap |
| 4 | 用户数据 + 物化拷贝 + 边界清零落盘 | 数据块盘面 | inode / 树结构 |
| 5 | 一次写 inode（data_root / direct_bit / size / 时间戳） | inode | 其余 |

推论：

- **inode 写入 = 唯一提交点**。phase1~4 的位图亮灯、树节点、数据块全部是"未提交资源"；
  任何 phase 失败 → 逆序撤销（phase1 整体 free / phase2 逆序 unmap 已生效新段 + free 未
  map 段 / phase3 无孤儿无需撤 / phase4 free 数据块），inode 永远保持旧态自洽，永不指向
  半成品树。phase3 融合与账本无冲突（见 §6：能融的记录不是本次账本条目）。
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
   （物理紧贴扩展 → phase2 入口判定保 direct，零树动作）。
5. direct→extent：4 的软占领失败（相邻被占）→ 降级；旧 direct 区**数据零搬移**，
   上树后作为一条 extent（建树 + map 全文件旧区 + map 新段）。
6. extent→extent：待写区间落到 extents 上；遇空洞尽可能从左侧 extent 物理尾向右
   soft_occupy（物理紧贴 → phase2 map 时自动左并），能合并就合并；
   phase3 融合（merge_range）处理 map 自动合并之外的历史分离残留。
7. extent→direct：极罕见——6 的合并使全文件坍缩为单 extent 时，按"尽可能 direct"宗旨
   坍缩。依赖整树释放轮子（§9 待办）；pwrite 第一版可暂缓坍缩（留在树上，语义等价）。

分配策略 = **phase1 内部单一事实源**（固化不注入、不散落）；phase2/3 只见
"已就绪的映射增量"，不重复决策——phase2 机械落地（不预推演终态，落地时自然形成），
phase3 融合收口。

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
  于是 phase2 落地 = phase1 输出 → 逐段 map（+ 需要时建树/旧区一并上树），
  零预编译；会 unmap 的是 punch_hole / truncate，不是 pwrite；
- 回滚记账干净：列表本身就是账本——phase1 失败整体 free；phase2 中途失败 →
  已 map 的逆序 unmap+free（含并入旧记录的紧贴段：裁切回滚 = unmap 并入后整条 +
  map 回旧部，§5）、未 map 的直接 free；
- 邻接合并是 `extent_map_range` 的事（左并/右并：逻辑紧贴 + 物理连续自动吞并）——
  phase1 不做树层面合并，occupy 成功的"物理紧贴扩展段"由 phase2 map 自动并入旧
  extent（**并入发生在落地期而非计划期**）；map 合并管不到的历史分离残留 →
  phase3 融合收口（§6）。

### 4.2 规划域（字节写域 → 逻辑块域，最容易漏的分叉）

- 非 inline 初始态：域 = `[off>>12, (off+len+4095)>>12)`（半开，len > 0）——首尾所在块
  即使只写 1 字节也是整块入域（不存在半块映射）。
- **inline 迁出**（`newsize = max(size, off+len) > 160`）：域扩为 `[0, ceil(newsize/4096))`——
  离开 inline 意味着全文件内容都要物理背书；**头部旧内容（inline_data）的物化分配是
  phase1 的职责**，[0,160) 的字节拷贝才归 phase4。此分叉漏掉 → phase2 建完树发现块 0
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
  phase2 入口判定保 direct；失败 → 段落异处 → 终态上树（旧区一并 map，零搬移）。
- extent 初始态：空洞写优先从左侧 extent 物理尾向右 soft_occupy；文件尾增长同
  tail_infinite gap 处理。
- inline 迁出：≤2 块 hard_alloc 直指；>2 块 hard 优先（连续 → direct），失败 soft 多段
  → extent。
- 数据块分配 align_log2 = 0（8K 对齐只约束树节点/元数据片，数据块对齐是浪费）。

## 5. phase2 — 新区间逐段 map（机械落地）

- 输入：phase1 输出（纯新段列表）+ 初始态（inline/direct/extent，phase1 前快照）。
  现映射的精细游走不是 phase2 的职责——map 原语自己 walk；phase2 只见"已就绪的映射
  增量"，不重复决策（§3 末尾）。
- **入口一次形态判定（全有全无，2026-09-06 定）**：保 direct 条件 = phase1 输出整体
  满足直指区间连续覆盖（≤1 段且物理尾贴/连续），此时**零树动作**，只产生"扩 data_root
  区间"计划交 phase5 收口；不满足 → 整单上树：建空根叶（当前无树时）→ 旧区 map 一条
  （升格场景，旧区恒 1 条：direct 直指区间 / inline 迁出整域）→ 逐段 map 新段。
  不存在"部分直扩 + 部分树"的混合终态（直指区间必须全文件连续无洞，混合态违反
  三态判据）。判定后状态单调：一旦上树，state = extent，后面全是树内 map。
- 逐段 map = 机械循环：顺序无关（map 原语插入时自动左并/右并，物理紧贴段自动并入
  旧 extent）；**无 unmap**（pwrite 不搬数据；unmap 是 punch_hole/truncate 的主场）。
- **`root_blk` 引用必须贯穿全程**：第 k 条 map 触发根裂换根后，第 k+1 条用的、以及
  失败回滚用的都是最新根（引用带出），不是 phase 开头那个。
- **失败 → 逆序回滚**（账本 = phase1 输出列表）：未 map 段直接 free；已 map 段逆序
  unmap+free（用当前 root_blk）；**紧贴段已并入旧记录** → 裁切回滚：unmap 并入后整条
  + map 回旧部 + free 新物理块；建了空树且无存活映射 → free 树根。inode 未动，旧态自洽。
- ENOSPC 只可能发生在 map 内部分裂（树节点现用现分：2 块、8K 对齐 hard_alloc）→
  本条回滚路径承担；phase2 不预分配树节点。
- ⚠ 已知缺口（2026-09-06 C 决策，详见 `extent_merge_range.md` §7）：分裂级联
  ENOSPC 中间态（leaf_split 成功后 new_root_inner alloc 失败 / 内裂 reparent 中途
  失败）会留孤儿节点/半树——孤儿叶里的旧记录从根不可达，逆序 unmap 兜不住。
  决策：不修，孤儿交灾难恢复（fsck），与待办 #3 整树回收轮子一并接。

## 6. phase3 — 融合收敛（写域 merge_range）

- 时机：phase2 全部 map 成功之后、phase4 数据写之前；域 = 本次写域
  `[base, base+blk_len)`（逻辑块域），坍缩准备场景可扩到全域。
- 动作：对域调 `extent_merge_range(root_blk&, owner, base, blk_len)`（完整设计 /
  实现方案见 `extent_merge_range.md`，代码未落地）——把域内"逻辑紧贴 + 物理连续"
  的分离记录对并掉，收敛到无可并。**root_blk& 贯穿**（融合链可触发欠载合并/根收缩，
  新根经引用带出）。
- **map 已自动合并，为何还要融合**：map 的合并只在插入瞬间生效，管不到三类残留——
  ① 跨多次操作累积的历史分离记录（老树 / fsck 后 / 未来并发写者）；② 边界抹匀产物
  （balance 抹匀把记录分到两叶，边界偶发可并分离）；③ 为 extent→direct 坍缩铺路
  （全域融合收敛到单 extent 是坍缩前置，终态机 #7）。收敛是"紧凑性/直指性"诉求，
  非数据正确性必需。
- **与回滚账本无冲突（关键论证）**：融合条件要求物理连续，而本次账本条目（新段）
  要么物理不连续（hard_alloc 异处 → 独立记录，**融合条件不满足**，phase4/5 失败时
  可精确 unmap）；要么物理紧贴（occupy → 已在 phase2 map 时并入旧记录，**非融合
  造成**）。能融的都是非账本记录 → 融合不破坏任何新段的独立可撤销性，phase4 失败
  的逆序回滚不受影响。
- 失败模型：merge 只减记录、不产生孤儿数据（部分融合 + 欠载残留可自愈，见
  `extent_merge_range.md` §4.2）→ **尽力语义**：融合失败 RC 上报，树一致性不受损、
  与未收敛语义等价——推荐不阻塞管线（告警后继续 phase4/5；与 phase2 的"失败即
  中止回滚"严格区分）。
- 融合不改变物理映射 → phase4 的"逻辑块 → 物理块"解析（先 affect_extents 后旧树）
  无需任何适配。punch_hole / truncate 无融合必要（洞两侧物理不连续，天然不可并）。

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

## 9. extent 原语 detach 重构（2026-09-04 决策 → 2026-09-06 已落地）

技术债定性（已修）：原 `extent_map_range / extent_unmap_range(uint64_t inode_idx, ext)` 把
"树的结构操作"与"inode.data_root 即时回写"焊死——内部 `extent_new_root_inner` /
`extent_root_shrink` / `extent_balance_from` 每次根更迭都开 InodeSession 写 data_root。
2026-09-06 重构完成（hpp + extent_tree.cpp + test_extent_ops.cpp）：调用面已改，
串行 A/B/C/D 与并行 E 组（8 worker × 120k 记录打穿内部层）全绿，测试新增
"原语不碰 inode（stale 断言）+ phase5_commit 收口"双检查。

已落地签名：

```cpp
result_code_t extent_map_range(uint64_t &root_blk, uint64_t owner_inode_idx, const extent_t &ext);
result_code_t extent_unmap_range(uint64_t &root_blk, uint64_t owner_inode_idx, const extent_t &ext);
```

- `root_blk` **in/out**：入 = 当前树根，出 = 操作后根（map 根裂 / unmap 根收缩更迭由引用带出；
  单次调用根可更迭多次——根裂建新根后再被 balance 链收缩——引用天然覆盖）；
- `owner_inode_idx` 降级为**纯审计锚**（根 meta `parent_ptr.ptr` = "." 锚，扫描恢复归属
  线索），**不触发任何 inode I/O**（测试传假 owner 亦可，锚不影响原语正确性）；
- 顶层 inode 态校验（type / direct_bit / data_root 检查）删除——调用方负责"这是文件树、
  root_blk 有效"（map/unmap 仍做 inode_test 防御可留 debug 变体）；
- RC 语义不变：OVERLAP（map 与现映射相交）/ 三字段全等 unmap / NOT_FOUND；
- 内部改动面（已做）：上述三个 helper 的 InodeSession 写已删净，根更迭沿引用链上抛
  （`extent_balance_from(child_blk, owner, root_blk&)` 内三处收缩收归 `extent_root_shrink(root_blk&,...)`）；
  `extent_Bptree_walk` 本就收 root_blk 不收 inode，未动；顶层 inode_test/type/direct_bit 校验
  已删（调用方职责，debug 变体可后补）。

连带缺口原语（inline/direct ↔ extent 转换缺失的拼图）：

- **建空根叶**：alloc 2 块（8K 对齐）+ init 空根叶（meta entry_count=0 / is_root / 叶子 /
  magics / owner 锚）→ 返回 root_blk。inline/direct→extent 的 phase2 第一步；
  现在 extent 侧只有分裂专用 `extent_new_root_inner`，无空树构造（dentry 侧 dir 急建
  树根可参照）。
- **整树节点收集释放**：extent→direct 坍缩、truncate ≤160 回迁 inline、未来 unlink 全量
  回收共用（dentry 删除侧现为 `RC_ERR_NOT_SUPPORTED` 哨兵——全量空间回收从未落地）。
  **单独立项，别塞进 pwrite**；坍缩在 pwrite 第一版暂缓（留在树上，语义等价、只少直指性）。

## 10. punch_hole / truncate 复用

同一五步管线：phase1 规划域 = 打洞区间；phase2 落地序列**含 unmap**（打洞/截断是 unmap
主场：整块释放、残块清零保留），无融合必要。truncate 缩小内部编译为"尾巴打洞 + size 收口"
（hpp 注），但 **size ≤ 160 强制回迁 inline**（判据 size 优先的必然推论，
disk_layout_v1.md §4.1）：读回 inline_data + 释放全部数据块与树节点 +
data_root = PTR_INVALID——禁止只打尾洞不搬回（会造成"size ≤ 160 却外置映射"的不一致态）。

## 11. 待办 / 开放项

1. ~~extent_map_range / unmap_range detach 重构（§9）~~ → **2026-09-06 完成**（含内部
   helper 链穿透 + 测试改造，A/B/C/D + E 组全绿）
2. 建空根叶原语（§9）
3. 整树节点收集释放轮子立项（坍缩 / 回迁 / unlink 共用，§9）
4. dfprealloc_and_plan 实现（§4 规格）
5. **phase2 ENOSPC 逆序回滚路径验证**（树节点现用现分；含紧贴段并入旧记录后的
   裁切回滚：unmap 整条 + map 回旧部）
6. 并发前提：同 inode 写互斥由调用方保证（phase1 读态 ~ phase5 写之间无锁原子性）
7. ftype() 判据边界：**size ≤ 160 内联**（160B inline_data 恰好全用合法；stub 初版
   `<160` 已修为 `<=160`，与 disk_layout 判据一致）
8. **extent_merge_range 实现**（phase3 融合原语；设计/三问号见 `extent_merge_range.md`，
   代码未落地）
9. phase3 融合接线 + 尽力语义验证（§6）

---

> **一句话记**
>
> 直写 = 五步管线一条路走到底：phase1 预分配纯新段（副作用仅位图、失败原子）→
> **phase2 新区间逐段 map**（入口一次形态判定：保 direct 零树动作 / 上树建树+旧区+
> 逐段；root_blk 引用贯穿，失败逆序回滚）→ **phase3 融合收敛**（写域 merge_range，
> 处理 map 合并管不到的历史残留、为坍缩铺路；与账本无冲突、尽力语义）→
> phase4 写数据（新段 + 旧树双源解析）→ phase5 inode 一次写 = 唯一提交点。
> 为此 extent 原语必须 detach：arg1 换 `root_blk&`，inode.data_root 回写权收归 phase5。

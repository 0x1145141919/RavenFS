# RavenFS — direct_file_* 直写管线（pwrite 四步，当前版）

> 本文只承载**当前设计**，历史过程稿/决策演进已扬（git 历史可追溯）。
> 代码主体未落地：`direct_file_ops.cpp` 仅 ftype + `regular_file_reclaim` + 骨架注释；
> 底层 extent 原语已落地（§8）。本文是 `direct_file_pwrite` / `direct_file_punch_hole` /
> `direct_file_trunacate` 的实现总纲，供续写，不依赖会话记忆。
>
> 配套判据（不重复）：`disk_layout_v1.1.md` §3.2（三态解码判据：size 优先，
> data_root 取值退出）+ §10（不变量）+ 修订日志；三态：inline（size ≤ 160，
> 数据在 `inline_data`）/ direct（直指连续区间）/ extent（区段树，337 阶）。

## 1. 四步管线

所有写路径走同一条四步管线（不做快速路径分级）；平直条件（单 extent 覆盖、纯增长）
自然退化：零 unmap + 至多一条 map + 直接写数据。以最复杂的 extent 初始态为蓝本：

1. **资源预分配 + 区间规划**（`dfprealloc_and_plan`）：规划写域的纯新段 + 预分配物理块
2. **新区间逐段 map**：入口一次形态判定（保 direct / 上树）→ map × N 机械落地
3. **文件数据写入**
4. **inode 一次写 = 提交点**

纪律：inode 写入是唯一提交点——phase1~3 的位图亮灯/树节点/数据块全是未提交资源；
任何 phase 失败 → 逆序撤销（phase1 整体 free / phase2 逆序 unmap 已生效新段 / phase3
free 数据块），inode 恒指完整旧态。崩溃模型对齐翻转写序：① 位图亮灯 → ② 数据/树节点
落盘 → ③ inode 字段翻转；崩溃于 ③ 前 = 孤儿资源，fsck 回收。

## 2. 融合语义

**写路径内的融合 = map 左融合（phase2 落地期，不是独立 phase）**：

- phase1 对旧区尾部做物理尾延展（`block_occupy_soft`，见 §5）→ 新段与旧记录
  "逻辑紧贴 + phys 连续" → phase2 map 时 **GLB 扩长并入**（`GLB.length += 段长`，
  count 不变、无腾槽、无分裂 → 零新增失败路径）。顺序写文件记录数 ≈ phys run 数，
  **不随写次数涨**。
- 右并 / 跨界左并不做：可并残留是合法形态，交激进重建兜底。
- 反过来说：写路径**没有独立的融合 phase**——融合发生在 map 原语内部、随插入自然
  生效，pwrite 不为收敛付额外开销。

**低频兜底 = 激进重建 `extent_rebuild_tree`**（整树收集 → 可并对全并 → 均分重打包
168..337 → 自下而上建树 → free 旧树 → 换根；失败原子：新树建成前旧树只读）：
兑底 map 左并刻意不处理的残留（跨界对 / 随机写中间插入 / 历史遗留）。
触发点未接线：close 插桩 + 显式 `merge_extents`（len=0 全文件），三端透传。

## 3. phase 职责与副作用面

| phase | 职责 | 允许副作用 | 禁止 |
|-------|------|-----------|------|
| 1 | 规划写域纯新段 + 预分配物理块（尾延展优先） | 位图（alloc/occupy） | inode / 树结构 / 数据内容 |
| 2 | 形态判定 → 建树/空根叶（如需）→ map × N（左并 + 分裂预算） | 位图（预算池）+ 树盘面 | inode / 数据内容 / 独立融合动作 |
| 3 | 用户数据 + 物化拷贝 + 边界清零落盘 | 数据块盘面 | inode / 树结构 |
| 4 | 一次写 inode（data_root/direct_bit/size/时间戳） | inode | 其余 |

## 4. 状态迁移（七条）

1. inline→inline：size 不越 160，纯 inode 内写，零分配零树。
2. inline→direct：扩容优先此路，hard_alloc。
3. inline→extent：仅当升格需 >2 个 FS_block 才降级到此（≤2 块建树必亏——树根自身
   2 块 8K）；soft_alloc。
4. direct→direct：不触发分配最好；触发优先 `block_occupy_soft` 尾部占领 → 物理紧贴
   → phase2 判定保 direct，零树动作。
5. direct→extent：尾占领失败（相邻被占）→ 降级；旧 direct 区**零搬移**上树
   （建树 + map 全文件旧区 + map 新段）。
6. extent→extent：空洞写优先从左侧 extent 物理尾向右 soft_occupy → phase2 map 左并；
   文件尾增长同 tail gap。
7. extent→direct：全文件坍缩为单 extent 时按"尽可能 direct"坍缩（前置 = 全域融合，
   rebuild 天然产出单条）；接线未落地，暂留树上（语义等价）。

分配策略是 phase1 内部单一事实源；phase2 只见"已就绪的映射增量"，不重复决策。

## 5. phase1 `dfprealloc_and_plan` 规格

声明（hpp 已加）：

```cpp
result_code_t dfprealloc_and_plan(uint64_t inode_idx, uint64_t offset, uint64_t len,
                                  std::list<extent_t> &affect_extents);
```

- **affect_extents = 纯新段**：只为写域中空洞/增长部分分配物理块；旧 extent 覆盖部分
  不出现（保留，phase3 直接写旧物理块）。pwrite **不需要 unmap 任何旧 extent**
  （覆盖写不搬数据）；unmap 是 punch_hole/truncate 的主场。
- 列表即回滚账本：phase1 失败整体 free；phase2 中途失败 → 已 map 逆序 unmap+free、
  未 map 直接 free。
- **规划域**（字节 → 逻辑块域）：非 inline 初始态 = `[off>>12, (off+len+4095)>>12)`
  半开；**inline 迁出**（newsize>160）域扩为 `[0, ceil(newsize/4096))`——头部旧内容
  （inline_data）的物化分配是 phase1 职责，[0,160) 字节拷贝归 phase3。
- 输出不变量：fblkbase 升序、互不重叠、与现映射零相交、block_base 恒有效、length=块数。
- 原子性：失败 = 已分配段全部逆序 free，无副作用返回。
- 分配策略：direct 增长/extent 空洞写优先 `block_occupy_soft` 尾延展（物理紧贴 →
  phase2 map 左并）；inline 迁出 ≤2 块 hard 直指 / >2 块 hard 连续 → direct，失败
  soft 多段 → extent。数据块 align_log2=0（8K 对齐只约束树节点）。

## 6. phase2 — 逐段 map

- **入口一次形态判定（全有全无）**：保 direct = phase1 输出整体满足直指区间连续覆盖
  （≤1 段且物理尾贴/连续）→ 零树动作，只产"扩 data_root"计划交 phase4；否则整单上树
  （建空根叶 → 旧区 map 一条 → 逐段 map）。无"部分直扩 + 部分树"混合态；判定后状态
  单调（上树即 extent，后面全是树内 map）。
- 逐段 map = 机械循环，顺序无关；map 内建**左并**（GLB 紧贴+连续扩长）与
  **分裂预算**（见下）；**无 unmap**。
- **`root_blk` 引用贯穿全程**：第 k 条 map 根裂换根后，后续与回滚都用最新根。
- **map 原子性（预算池，2026-09-07）**：满叶（337）纯插必裂 → map 按树高预分配
  `depth+1` 个节点（叶裂 + 每内层 + 根裂建新根），分裂函数自身零 alloc → 级联无
  ENOSPC 面；预算 alloc 在树改动前 → 失败 = 树零改动真原子；用不完收尾归还。
  不满叶插入零预算。→ phase2 逆序回滚只需处理"map 成功后 unmap 账本"，无孤儿面。
- 失败回滚（账本 = phase1 列表）：未 map 段直接 free；已 map 逆序 unmap+free；
  左并已并入旧记录的紧贴段 → 裁切回滚（unmap 并入后整条 + map 回旧部 + free 新物理
  块）；建空树无存活映射 → free 树根。inode 未动。

## 7. phase3 — 数据写入

- 解析"写域逻辑块 → 物理块"：先查 affect_extents（新段，含物化块），miss 走旧树
  （walk/iterator）或 direct 公式（base + 逻辑块号）；同一逻辑块不双命中（零相交）。
- 清零契约：块级空洞天然读 0；新分配块被部分写入时，块内未写区间必须清零。
- inline 迁出的 [0,160) 内容拷贝在此 phase。

## 8. phase4 — 元数据回写（提交点）

一次 InodeSession 写：`data_root`（直指基址或树根块号）、`is_data_root_direct_bit`、
`size = max(旧, off+len)`、时间戳。禁止 phase1~3 任何一处写 inode。

## 9. 底层现状（已落地）

- **extent 原语 detach**：`extent_map_range / extent_unmap_range / extent_rebuild_tree`
  收 `root_blk&` in/out，owner 纯审计锚（根 meta "."），零 inode I/O；根更迭引用带出。
- **map 左并 + 分裂预算池**（2026-09-07）：见 §2/§6。测试：fusion F/L/E 组。
- **激进重建 `extent_rebuild_tree`**：见 §2。测试：fusion R 组（ENOSPC 旧树无损）。
- **归零回收 `regular_file_reclaim(inum, destroy_data)`**：REG/SOFT_SYM link_count 归零
  时全形态回收（inline 清数据域 / direct 直指区间 / extent 叶链遍历 + 自底向上释节点
  + inode 槽，零泄露）；destroy_data=true 数据块 free 前清零。已接 dentry 删除链。
- 缺口原语：**建空根叶**（inline/direct→extent 的 phase2 第一步）——rebuild 内建树
  覆盖批量场景，但单棵空根叶原语未独立（测试现手搓）。

## 10. punch_hole / truncate 复用

同一四步管线：phase1 域 = 打洞区间；phase2 落地序列**含 unmap**（打洞/截断是 unmap
主场：整块释放、残块清零保留）；无融合必要（洞两侧物理不连续，天然不可并）。
truncate 缩小编译为"尾巴打洞 + size 收口"，但 **size ≤ 160 强制回迁 inline**（判据
必然推论）：读回 inline_data + 释放全部数据块与树节点（机械同 reclaim）+
data_root = PTR_INVALID——禁止只打尾洞不搬回（会造成 size ≤ 160 却外置映射的不一致态）。

## 11. 待办（当前未完成）

1. 建空根叶原语（§9）
2. `dfprealloc_and_plan` 实现（§5 规格）
3. phase2 ENOSPC 逆序回滚路径验证（含左并裁切回滚：unmap 整条 + map 回旧部）
4. 并发前提：同 inode 写互斥由调用方保证（phase1 读态 ~ phase4 写之间无锁原子性）
5. 融合接线：close 插桩 + `merge_extents(fd,start,len)` 三端透传 + 碎片度阈值判定（§2）
6. 分配侧刷回批量连续分配（碎片预防第一性；occupy 尾延展已在 phase1 策略内）
7. 坍缩/回迁接线（extent→direct / truncate ≤160，§4.7/§10，机械已就绪）

---

> **一句话记**
>
> 直写 = 四步管线一条路：phase1 预分配纯新段（尾延展优先）→ phase2 形态判定 + 逐段
> map（**左并**让尾延展段扩长并入旧记录；**预算池**让分裂原子）→ phase3 写数据 →
> phase4 inode 一次写 = 提交点。融合不进独立 phase：map 左并随插入生效，激进重建
> close 级兜底。回滚：phase1 失败整体 free，phase2 失败逆序 unmap 账本——map 自身
> 已无孤儿面。

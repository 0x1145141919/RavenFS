# RavenFS — direct_file_* 直写管线（五段式 · 2026-09-08 定案）

> 本文只承载**当前设计**，历史过程稿/决策演进已扬（git 历史可追溯）。
> 代码主体未落地：`direct_file_ops.cpp` 仅 ftype + `regular_file_reclaim` + 骨架注释；
> 底层 extent 原语已落地（§9）。本文是 `direct_file_pwrite` / `direct_file_punch_hole` /
> `direct_file_trunacate` 的实现总纲，供续写，不依赖会话记忆。
>
> 配套判据（不重复）：`disk_layout_v1.1.md` §3.2（三态解码判据：size 优先，
> data_root 取值退出）+ §10（不变量）+ 修订日志；三态：inline（size ≤ 160，
> 数据在 `inline_data`）/ direct（直指连续区间）/ extent（区段树，337 阶）。
>
> 2026-09-08 定案（Sparrow 拍板）：**形态决策独立成 phase1.5**——不再为 7 条状态
> 转移各写流程，统一"入口态 → 判别 → 执行 → 提交出口"；phase4 出口坍缩替代
> rebuild 前置坍缩。设计动因与逐条论证见会话记录，本文只载结论。

## 1. 五段管线

写路径统一走五段管线（inline→inline 豁免，§4.1）；**phase2 仅上树路径执行**，
保 direct 路径自然跳过——平直条件退化不是快速路径分级，而是 phase1.5 判别的
输出分支：

```
phase1    资源预分配 + 区间规划（dfprealloc_and_plan）：规划纯新段 + 预分配物理块
phase1.5  形态判别（纯决策，零副作用零失败面）：保 direct / 物化+map / map-only
phase2   [仅上树路径] 物化（建空根叶 + map 旧区零搬移）→ 逐段 map × N
phase3    文件数据写入
phase4    inode 一次写 = 提交点 + 出口判定（坍缩 direct / 留树）
```

纪律：inode 写入是唯一提交点——phase1~3 的位图亮灯/树节点/数据块全是未提交资源；
任何 phase 失败 → 逆序撤销（phase1 整体 free / phase2 逆序 unmap 已生效新段 / phase3
free 数据块），inode 恒指完整旧态。崩溃模型对齐翻转写序：① 位图亮灯 → ② 数据/树节点
落盘 → ③ inode 字段翻转；崩溃于 ③ 前 = 孤儿资源，fsck 回收。**phase4 坍缩的旧根
free 发生在 ③ 之后**（先提交 direct 态再清扫树根）→ 崩溃窗口 = 孤儿 2 块，inode 已
不指它，无悬垂（§8）。

## 2. 融合与坍缩语义

**写路径内的融合 = map 叶内四形态（phase2 落地期，不是独立 phase）**：

- phase1 对旧区尾部做物理尾延展（`block_occupy_soft`，见 §5）→ 新段与旧记录
  "逻辑紧贴 + phys 连续" → phase2 map 时**左融合**（GLB 扩长并入，count 不变）；
  前插紧贴连续 → **右融合**（并入右邻头）；补洞三连 → **双融合**（GLB 扩长吞右邻，
  count−1，非根欠载走平衡链）。叶内 canonical 合并义务全覆盖。
- 残留 = 跨叶边界缝 + 渐进左并后新产生的叶内紧贴连续对——**合法形态**，不追严格
  融合（"不搞严格融合"定案），交激进重建低频兑底。
- 反过来说：写路径**没有独立的融合 phase**——融合发生在 map 原语内部、随插入自然
  生效，pwrite 不为收敛付额外开销。

**坍缩（extent→direct）= phase4 出口判定，不是独立 phase 也不是独立路径**：

- 树路径提交前 O(1) 读根：**根是叶 && count==1 && 该条覆盖 [0, 提交后 size)**
  （fblkbase==0 && end==ceil(size/4096)）→ 可坍缩。树操作机械已保证无"根内单孩"
  残留（root_shrink 收尾），判定只认根叶。
- 坍缩 = 先写 inode（data_root=单条 block_base、direct_bit=1、size）→ 再 free 旧根
  2 块（③ 后清扫，孤儿可回收，§8）。
- 触发面：truncate 缩到单条后 pwrite 增长（左并维持单条）、extent 双条中间洞填上
  （map 双融合 → 单条）、rebuild 后单条的下一次写。**不需要 rebuild 前置、不需要
  专门坍缩触发接线**（原 §4.7 待办消失）。
- 对照：direct 可表示性 = 整文件单 phys run 且 [0,size) 全覆盖——phase1.5 的
  "保 direct" 判据与 phase4 坍缩判据是**同一个检查**，一个事前（分配后）一个事后
  （提交前）；保 direct 路径直接写 direct inode，无树可坍缩。

**低频兜底 = 激进重建 `extent_rebuild_tree`**（整树收集 → 可并对全并 → 均分重打包
168..337 → 自下而上建树 → free 旧树 → 换根；失败原子：新树建成前旧树只读）：
兑底跨叶缝 / 渐进残留（phys 断的多 run 文件 rebuild 也合并不了——那是物理事实，
永居 extent）。触发点未接线：close 插桩 + 显式 `merge_extents`（len=0 全文件）。

## 3. phase 职责与副作用面

| phase | 职责 | 允许副作用 | 禁止 |
|-------|------|-----------|------|
| 1 | 规划写域纯新段 + 预分配物理块（direct 候选优先，见 §5） | 位图（alloc/occupy） | inode / 树结构 / 数据内容 |
| 1.5 | 形态判别（保 direct / 物化+map / map-only） | 无（纯读旧态 + phase1 输出） | 一切副作用 |
| 2 | [上树路径] 物化旧区（如需）→ 逐段 map（四形态 + 预算池 + 欠载平衡） | 位图（预算池）+ 树盘面 | inode / 数据内容 / 独立融合动作 |
| 3 | 用户数据 + 物化拷贝 + 边界清零落盘 | 数据块盘面 | inode / 树结构 |
| 4 | 一次写 inode（提交点）；坍缩时先提交 direct 再 free 旧根 | inode；③ 后 free 坍缩旧根 | 其余 |

## 4. 状态迁移（七条 → 逐条 phase 序列）

统一规则：**extent 旧态不需要判别**（树已在，phase1.5 直出 map-only）；
**判别只发生在非树态**（inline/direct），输出保 direct 或物化+map。保 direct ⇔
affect_extents **段数==1 && 首段 fblkbase==0**（0 起始单段 ⇒ 自动覆盖到尾）。
否定形式：多段 / 首段非 0（防御判据，实践几乎不出现——物化段恒 0 起始）→ 上树。

1. **inline→inline**（size 不越 160）：**不走管线**。纯 inode 内写，零分配零树。
2. **inline→direct**：phase1 整域 hard 连续候选成功（1 段 0 起始到尾）→ phase1.5
   判保 direct → **跳 phase2** → phase3（数据 + [0,160) 物化拷贝）→ phase4 写
   direct inode（data_root=段 base）。
3. **inline→extent**：phase1 hard 失败 → 降级裁剪必需段（物化块 ∪ 写域，洞留空）
   soft 多段 → phase1.5 见多段/首段非 0 → phase2 物化 = **建空根叶 + map 物化段 +
   map 新段**（inline 无旧 extent 记录，空树起）→ phase3 → phase4 出口（罕见兜底：
   若最终单条覆盖整文件 → 坍缩，等价于绕了一圈 direct）。
4. **direct→direct**：不触发分配最好（覆盖写零新段，affect_extents 空）；增长触发
   优先 `block_occupy_soft` 尾部占领 → 物理紧贴 → phase1.5 见 1 段尾贴 → 判保
   direct → **跳 phase2** → phase3 → phase4（size 推进，base 不动）。
5. **direct→extent**：尾占领失败（相邻被占）→ 新段 phys 不贴 → phase1.5 判物化 →
   phase2 = **建树 + map 旧 direct 区一条（零搬移，记录指向原 phys）+ map 新段** →
   phase3 → phase4 留树（2 run phys 断，永不单条，永居 extent——物理事实）。
6. **extent→extent**：空洞写优先从左侧 extent 物理尾向右 soft_occupy → phase2
   map 左并；文件尾增长同 tail gap。phase1.5 直出 **map-only**（无判别无物化）→
   phase2 现有树上逐段 map → phase3 → phase4 出口（双条洞填 → 双融合单条 →
   可能坍缩）。
7. **extent→direct**：**非独立路径** = 2/3/5/6 走树后 phase4 的出口分支（§2
   坍缩判定）。写前不可预知写后是否单条（取决于 map 收敛），不为它设计独立流程。

分配策略是 phase1 内部单一事实源；phase1.5 只见"已就绪的映射增量"做形态判别，
phase2 只见"已判定形态的执行"，不重复决策。

## 5. phase1 `dfprealloc_and_plan` 规格

声明（hpp 已加）：

```cpp
result_code_t dfprealloc_and_plan(uint64_t inode_idx, uint64_t offset, uint64_t len,
                                  std::list<extent_t> &affect_extents);
```

- **affect_extents = 纯新段**：只为写域中空洞/增长部分分配物理块；旧 extent 覆盖部分
  不出现（保留，phase3 直接写旧物理块）。pwrite **不需要 unmap 任何旧 extent**
  （覆盖写不搬数据）；unmap 是 punch_hole/truncate 的主场。
- 列表即回滚账本：phase1 失败整体 free；phase2 中途失败 → 已 map 逆序
  `unmap_range_loose` + free、未 map 直接 free（§6）。
- **规划域**（字节 → 逻辑块域）：非 inline 初始态 = `[off>>12, (off+len+4095)>>12)`
  半开；**inline 迁出**（newsize>160）域扩为 `[0, ceil(newsize/4096))`——头部旧内容
  （inline_data）的物化分配是 phase1 职责，[0,160) 字节拷贝归 phase3。
- 输出不变量：fblkbase 升序、互不重叠、与现映射零相交、block_base 恒有效、length=块数。
- 原子性：失败 = 已分配段全部逆序 free，无副作用返回。

**分配策略（2026-09-08 补：direct 候选 → 降级裁剪）**——phase1 不预判形态，但
分配倾向内嵌，保证 phase1.5 有 direct 可分、extent 不浪费：

1. **非树态（inline/direct）→ direct 候选**：整域（inline 迁出 = [0,ceil(newsize/
   4096)) 含 size 内未写区占位——direct 语义本就全覆盖，不浪费；direct 增长 = 尾段）
   一次 hard 连续尝试。成功 = 1 段 0 起始 → phase1.5 保 direct。
2. **失败降级 → extent 候选**：**裁剪为必需段再 soft 分配**——物化块（inline 块 0）
   ∪ 写域；size 内未写空洞**留空不分配**（extent 表示下洞无 phys，读 0）→ 天然多段
   → phase1.5 判物化。禁止把整域硬分 soft（会让 extent 态洞白占 phys = 浪费 + 账本脏）。
3. **extent 态**：空洞写/extent 尾增长按既有 occupy 策略（尾延展优先，
   `block_occupy_soft` 物理紧贴 → phase2 map 左并）。数据块 align_log2=0
   （8K 对齐只约束树节点）。

## 6. phase2 — 逐段 map（仅上树路径）

- **物化前段（仅非树态转树，路径 3/5）**：direct 旧区 = 建空根叶 + map 旧区一条
  （零搬移）；inline = 建空根叶（无旧区可物化）。树根经 `root_blk&` 引用贯穿全程。
- **map-only（路径 6）**：extent 旧态，跳过物化直接逐段 map。
- 逐段 map = 机械循环，顺序无关；map 内建**叶内四形态**（左并/右并/双并随插入
  自然生效）与**分裂预算池**（满叶纯插按树高预分配 depth+1 节点，级联零 alloc 无
  ENOSPC；双并删记录致非根欠载走 `extent_balance_from`，同样无 ENOSPC）——见 §9。
  **无 unmap**（pwrite 不删映射）。
- 单调性：phase1.5 判定后状态单调——保 direct 全程无树；上树即 extent 完整物化，
  无"部分直扩 + 部分树"混合态。
- **map 原子性（预算池，2026-09-07）**：满叶（337）纯插必裂 → map 按树高预分配
  `depth+1` 个节点（叶裂 + 每内层 + 根裂建新根），分裂函数自身零 alloc → 级联无
  ENOSPC 面；预算 alloc 在树改动前 → 失败 = 树零改动真原子；用不完收尾归还。
  不满叶插入零预算。→ phase2 逆序回滚只需处理"map 成功后 loose 账本"，无孤儿面。
- 失败回滚（2026-09-07 定案：账本驱动 + loose 一统，账本 = phase1 列表）：
  对每个已 map 段 X **逆序调 `unmap_range_loose(X)`**——独立记录 → full 分支整删；
  左并已并入旧记录的紧贴段 → tail 分支裁尾（count 不变、零分配、零欠载 → 回滚路径
  **无 ENOSPC 失败面**，恒成功）。**逆序贴尾不变量**：逆序时后并入的先裁，每步裁切
  恒贴当前记录尾，永不落中间（无拆两端）。未 map 段起点在空洞 → loose 返回
  NOT_FOUND → 仅 free 物理块（无需另记已 map/未 map 进度）。X 的 phys 由账本自算
  free，裁掉段的 phys = 线性映射偏移段。物化建的树无存活映射 → free 树根。inode 未动。

## 7. phase3 — 数据写入

- 解析"写域逻辑块 → 物理块"：先查 affect_extents（新段，含物化块），miss 后按
  **路径分**——保 direct 路径：direct 公式（base + 逻辑块号）直接命中旧区（无树）；
  上树路径：旧树 walk/iterator。同一逻辑块不双命中（零相交）。
- 清零契约：块级空洞天然读 0；新分配块被部分写入时，块内未写区间必须清零。
- inline 迁出的 [0,160) 内容拷贝在此 phase。

## 8. phase4 — 元数据回写（提交点）+ 出口判定

一次 InodeSession 写 + 出口判定，按提交时形态三分：

- **保 direct 路径**（phase1.5 已判，无树）：写 data_root=段 base、direct_bit=1、
  size=max(旧, off+len)、时间戳。
- **树路径坍缩**（根叶 && count==1 && 单条覆盖 [0, 新 size)）：先读单条 block_base
  → 写 inode（data_root=base、direct_bit=1、size）——**提交点** → 后置 free 旧根
  2 块（③ 后清扫；崩溃 = 孤儿 2 块，inode 已不指它，fsck 回收；**严禁先 free 再写
  inode**——会造 inode 指已 free 块的悬垂）。
- **留树**：写 inode（data_root=根块、direct_bit=0、size）。

禁止 phase1~3 任何一处写 inode。坍缩判定在 phase3 前 O(1) 读根完成、结果随行传
phase4（phase3 不碰树）。

## 9. 底层现状（已落地）

- **extent 原语 detach**：`extent_map_range / extent_unmap_range_loose / extent_rebuild_tree`
  收 `root_blk&` in/out，owner 纯审计锚（根 meta "."），零 inode I/O；根更迭引用带出。
  unmap 家族唯一接口 = loose（2026-09-07 extent_unmap_range 退役并入其 full 分支：
  三字段全等整删语义不变，多出 head/tail/mid 子集删除；回滚/punch/truncate 共用）。
- **map 叶内四形态 + 预算池 + 欠载平衡链**（2026-09-07 晚换代，本文件 §2/§6 依据）：
  同叶 L/R 双判定 → 纯插 / 左并（扩 GLB 尾）/ 右并（并入右邻头，终点不变零刷新）/
  双并（L&&R 三合一，count−1 → 非根欠载走 `extent_balance_from`）；满叶纯插预算池
  depth+1 原子分裂。跨界（entry[0] 前 / 叶末越叶）仍显式放弃。级联机械抽为
  `extent_split_cascade`（map 纯插与 loose 拆两端共用内核）。测试：fusion
  F1 四形态 + f1e 欠载链 + F2/L/R/D/E 组 120 断言全绿。
- **`unmap_range_loose` 三分支**（2026-09-07 落地）：full 整删（前 unmap_range 机械，
  单 walk）/ head+tail 缩水（零分配零欠载，phase2 左并回滚 = tail）/ mid 拆两端
  （满叶预算前移，同 map 原子性）。契约：逻辑域身份 + phys 偏移自洽防错删。
  测试：发烟 `ravenfs_juv_extent_unmap_loose` 62 断言（A~E 组：契约防御/三分支形态/
  左并回滚模拟/满叶分裂）；压测 `ravenfs_juv_extent_loose_stress` 47 断言
  （12 万宽 4 打底：A 规整组预期形态 + B 混沌 4×25904 乱序 + C 全删收敛空根叶 +
  S4 NO_BLOCK 注入失败零改动；验证支柱 = 账本模型差分）。
- **激进重建 `extent_rebuild_tree`**：见 §2。测试：fusion R 组（ENOSPC 旧树无损）。
- **归零回收 `regular_file_reclaim(inum, destroy_data)`**：REG/SOFT_SYM link_count 归零
  时全形态回收（inline 清数据域 / direct 直指区间 / extent 叶链遍历 + 自底向上释节点
  + inode 槽，零泄露）；destroy_data=true 数据块 free 前清零。已接 dentry 删除链。
- 缺口原语：**建空根叶**（phase2 物化的第一步，路径 3/5）——rebuild 内建树覆盖批量
  场景，但单棵空根叶原语未独立（测试现手搓）。

## 10. punch_hole / truncate 复用

同一五段管线：phase1 域 = 打洞区间；**phase1.5 对 punch/truncate 恒判物化/树路径**
（打洞是 unmap 主场，无保 direct 语义——洞两侧物理不连续天然不可并）；phase2 落地
序列**含 loose**（命中每条 extent 切段调 `unmap_range_loose`——整块释放走 full、
残块清零保留走 head/tail/mid，段化后每段必落单条记录内）；无 map 无融合。
truncate 缩小编译为"尾巴打洞 + size 收口"，但 **size ≤ 160 强制回迁 inline**（判据
必然推论）：读回 inline_data + 释放全部数据块与树节点（机械同 reclaim）+
data_root = PTR_INVALID——禁止只打尾洞不搬回（会造成 size ≤ 160 却外置映射的不一致态）。
**truncate 到 >160 且树收敛单条覆盖新 size → phase4 出口坍缩回 direct**（截断后
自然回 direct 的通道）。

## 11. 待办（当前未完成）

1. 建空根叶原语（§9）
2. `dfprealloc_and_plan` 实现（§5 规格 + direct 候选/降级裁剪）
3. phase1.5 形态判别实现（§4 判据：纯决策零副作用）
4. phase2 ENOSPC 逆序回滚集成验证（树侧机械已就绪：loose tail 裁切零分配恒成功，
   发烟 D 组 + stress 已覆盖 loose 全分支；待办 = dfprealloc_and_plan → 判别 → map →
   失败注入 → 逆序 loose 的管线级联测）
5. 并发前提：同 inode 写互斥由调用方保证（phase1 读态 ~ phase4 写之间无锁原子性）
6. 融合接线（低频兑底）：close 插桩 + `merge_extents(fd,start,len)` 三端透传 +
   碎片度阈值判定（§2）
7. 分配侧刷回批量连续分配（碎片预防第一性；occupy 尾延展已在 phase1 策略内）
8. phase4 出口坍缩接线（§8：判定 + 先 inode 后 free 根的顺序纪律——原 §4.7 待办
   的载体；truncate ≤160 回 inline 是 §10 既有回迁，机械就绪）

---

> **一句话记**
>
> 直写 = 五段一条路：phase1 预分配（direct 候选 → 降级裁剪必需段）→ phase1.5
> 形态判别（保 direct 跳 phase2 / 物化上树 / extent 免判别）→ phase2 仅上树路径
> 物化 + 逐段 map（**四形态**融合 + **预算池**原子分裂 + 欠载平衡）→ phase3 写数据
> → phase4 inode 一次写 = 提交点 + **出口坍缩**（根叶单条覆盖整文件 → 先 inode 后
> free 旧根，孤儿可回收）。融合不进独立 phase：map 随插入生效，坍缩免 rebuild 前置，
> 激进重建 close 级兑底。回滚：phase1 失败整体 free，phase2 失败逆序 `loose` 账本
> （full 整删 + tail 贴尾裁切，零分配恒成功）——map 自身已无孤儿面。

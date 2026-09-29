# 一个契约，多个工具 —— 修复一块「已经在运行」的数据盘

> 2026-09-30 会话历程 · Raven
> 副标题：一个 ROI 看着不高、但就是**上头**的故事。
> 关联：`Docs/disk_layout_v1.2_inode.md`（契约）· `Docs/linux/ravenfs-linux-ko-v0.1.1-defects.md`（D-5~D-8）· `tools/rfs_compact/`（刀）。

---

## 0. 一句话

从「给用户态上帝类（god）适配 `disk_struct v1.2`、顺带估个成本」出发，一路偏执往下挖，
最后落成一把**用户态离线修复刀** `tools/rfs_compact`，去治一块**已经在跑、装着内核源码树
和 VM 盘**的 ravenfs 数据盘 —— 治它上面由 ko `ravenfs-dkms 0.1.1-1` 留下的**元数据/数据失衡创伤**。

---

## 1. 契约的由来（不是这一天开始的）

`disk_struct v1.2` 不是新格式，而是**同一批字节的「语义重定义」**：
`type:6｜direct:1｜i_flags:57`、`mode:12｜xattr_ptr:52`、无数据类型把 `rdev` 塞进 `inline_data`、
xattr 单块自描述。**offset 与尺寸一字不变**，破坏性在两处：*语义*（v1.1 盘不兼容）；
*形式*（位域 bit-field → 掩码 + 宏，跨 C/C++ 二进制对接可靠）。

它像一份**地基契约**，于是自然长出**多把工具**：

| 工具 | 角色 |
|---|---|
| `mkfs/mkfs.ravenfs` | 按契约**写盘**（`--v1.1` 兼容旧盘） |
| `tools/audit_v1.2` | 按契约**递归只读校验**（+ 独立解析路径交叉验证） |
| `tools/sbinfo` | 按契约**轻查询**（SB / 几何 / 时间戳 / 备份链） |
| **`tools/rfs_compact`**（本日新增） | 按契约**离线修复**（治 D-5/D-6） |

> 真值源：`Docs/disk_layout_v1.2_inode.md`。

---

## 2. 会话历程（偏执狂的轨迹）

| # | 动作 | 产物 |
|---|---|---|
| 0 | 起点：god 适配 v1.2 的**成本评估**（~500 位域访问点 + 12 语义点 + mknod/xattr） | —— |
| 1 | 第一刀：先把 god **位图模块从 chunk 回退**到 `30ceb6a`（纯 FULL）—— 先减复杂度，再迁移 | `89a239a` |
| 2 | 契约**良定义收口** 3 处（link_count 全类型 / xattr 哨兵几何 / i_flags 读端）；顺跑「大魔王」两压测 | `c2bf310` |
| 3 | **迁移**：god 全量「位域→掩码」（~500 点 / 17 文件）+ 头切换 + 位图 root-bit 修复 | `1a941cc` |
| 4 | **定制接口**：`extent_tree_collapse_to_direct`（单 extent 树 → direct + free 节点） | `e93f5c0` |
| 5 | **工具**：`tools/rfs_compact`（DFS 全树 → rebuild + 坍缩） | `08dede6` |

每一步都不是计划好的；是「这一步做完，看到下一步该做什么」那种往下滚。

---

## 3. 为什么要修一块「已经在运行」的盘（契机）

前一夜（09-29）的**删除恢复实验**（`GTAV Source.rar`）意外盘出了 ko v0.1.1 的 D-5~D-8：

- **D-6**：`inline→extent` 不试 direct → **>160B 文件恒背一个 8K 区段树节点**。
  实测 `fs/ecryptfs/`：4K 数据文件实占 **12K**（1 数据块 + 2 节点块）。
  内核源码树 8 万文件量级 ≈ **数百 MB 纯节点税**，`du` 严重失真。
- **D-5**：ko 写路径**无叶内融合** → 一份 `cp` 进来的 32.5 GiB `root-ext4.qcow2` =
  **133351 条 64 块（256 KiB）extent**（≈794 叶 ≈ **6.2 MiB** 元数据，只为描述一份几乎连续的文件）。
- **D-8**：元数据与数据**共用分配 hint** → 区段树叶插进数据流，把数据打出一串 **2 块洞**。

这些**不是未来的 bug，是已经刻在盘上的伤**。ko 侧修复要重打包、只治将来；
**存量盘**只能靠用户态工具去救 —— 这就是上头的起点。

---

## 4. 刀法（`tools/rfs_compact`）

```
用户态挂载（mmap 后端 + inode/mdata_piece 缓存 = offload，非 O_DIRECT）
  └─ 从 sb.root_inode（v1.2=16）DFS 全目录树
       （dir_readdir 只有声明没实现 → 走底层 dentry 树下钻 hints[0] + 叶链 ptrs[2] 枚举）
       └─ 每个 REG 且非 inline/direct 的 extent inode：
            extent_rebuild_tree(root)            # 全量融合（治 D-5）
            → 回写 inode.data_root
            → extent_tree_collapse_to_direct(inum) # 单条覆盖 → direct + free 节点（治 D-6）
```

- **安全**：默认 **dry-run**（`O_RDONLY` + 只读挂载，只报告不写）；`--apply` 才写。
  只动**元数据（树节点 + inode `data_root/direct_bit`）与位图空闲**，**不碰数据字节**。
- **边界防护**：inode 索引 `< inode_count`（`inode_read` 无边界校验，外挂）；dir/reg 各自 visited 去重；深度/叶链 guard。
- **已知界**：物理不连续（D-8 的 2 块洞）**跨洞不融合**（工具不动数据）→ 大文件会**大幅降条数**但**未必到单条/坍缩**。

---

## 5. 验证

- **压测三档**（v1.2 盘）：`extent_ops` 默认 A/B/C/D **72/0**、`par 8` **66/0**；`name_heap_stress --full`（1,114,112）**24/0**。
- **新接口 smoke**：恶人树（手工 2 叶+根内、600 条跨叶未融合）→ rebuild 全量融合 records=1/nodes=1；collapse → direct + free；**18/0**。
- **工具**：造损伤盘（D-6 单 extent 背 8K 节点 + D-5 600 条残段）→ **DRY 与 APPLY 数字一致**、再跑**幂等**（extent 形态=0）。

---

## 6. 偏执狂的账（ROI 自嘲）

- **表面 ROI 不高**：一块已经能用的盘，「修不修都能跑」。
- **但**：D-6 的 8K 节点税是**每个文件**的常驻浪费；D-5 的 133k 条是**读放大 + 回收/恢复负担**——
  这些是压在盘上、**随时间复利放大**的账。
- **「上头」的正当性**（不是无脑上头）：
  1. **契约完备** → 工具可写；
  2. **可证** → dry-run 先报数（记录数前→后、可坍缩数）；
  3. **可逆** → 只动元数据、不碰数据；
  4. **幂等** → 修过再跑得 0。
- 顺带的收益：这趟把 **v1.2 迁移**做完了，也把「**一个契约，多个工具**」的拼图补上了最后一块。

---

## 7. 教训（写给未来的自己）

**迁移（位域 → 掩码）踩坑**：
1. 自造注释里写 `RFS_PTR_SET_*/...` 的 `*/` **提前关注释块**（09-28 同款，**又踩**）。
2. 对象前缀正则要 `.`/`->` **通吃**，且要能吞 **cast 前缀 `((T*)x)` 与 `()`**。
3. 宏定义里 `leaf_##sess_var->...` 的 **`##` 令牌拼接**会被切断。
4. `X.ptr += v` 复合赋值（lvalue）要改写成 `RFS_PTR_SET_ADDR(x, RFS_PTR_ADDR(x)+v)`。
5. 贪心前缀会把 `CHECK(` 吞进宏参数。

**世界观的坑**：
6. `extent_map_range` 是**四形态**（none/left/right/both）—— 有**右融合**，
   **任何插入顺序都会被 map 规范化成单条**；「恶人树」（跨叶/历史残留）只能**手工构**。

---

## 8. 交付物清单

- **commits**：`89a239a`（位图回退）· `c2bf310`（v1.2 契约收口）· `1a941cc`（v1.2 迁移）· `e93f5c0`（新接口）· `08dede6`（修复工具）。
- **接口**：`extent_tree_collapse_to_direct`（`include/ravenfs/ravenfs_v1.1.hpp` + `juvenile_god/direct_file_ops.cpp`）。
- **工具**：`tools/rfs_compact/{rfs_compact.cpp, Makefile, README.md}`。
- **文档**：`Docs/disk_layout_v1.2_inode.md`（收口）、`Docs/linux/ravenfs-linux-ko-v0.1.1-defects.md`（D-5~D-8）、**本故事**。

---

## 9. 尾注

> **一个契约，多个工具。** 契约是地基，工具是它的手。
> 下一位读者：如果你也在盘上撞见「8K 节点税」或「133k 条 extent」，
> `rfs_compact --apply` 在 umount 后的某个深夜等着你。

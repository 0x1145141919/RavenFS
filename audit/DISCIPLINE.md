# audit/DISCIPLINE.md — 顶层独立校验器局部纪律

> 状态：初版（2026-10-06）
> 定位：RavenFS 的 **audit 顶层子项目**（与 `juvenile_god/`、`linux/` 平级）的局部纪律。
> 上位：全局项目纪律 `../Docs/DISCIPLINE.md`；本文件只补本子项目特有约束。
> 关联：`../Docs/audit_dfs_design.md`（审计模型）、`../Docs/runtime_tech/01_mount_mode_contract.md`
> （挂载模式；本子项目**只取 mmap 只读后端**，不采纳其缓存分派）。

---

## 0. 一句话

**audit 是盘面契约的独立裁判，不是上帝类的一个方法。**
参数 = 路径；自己 `O_RDONLY` 打开 + 整盘 mmap；唯一地址语言 = `base + blk×4096`
（坐标偏移量）；自己解 SB / 几何 / 位图；**不链 `libravenfs_juvenile.a`**。

它要验的是**落盘后的静止字节**，与产生这些字节的实现（god / ko）**没有共享代码路径**。
这样它才能同时给两个写入方当裁判，且不与被审对象共因失效。

---

## 1. 信任边界（本子项目的宪法）

| 类别 | 信 / 不信 | 处理 |
|---|---|---|
| 裸盘字节、坐标数学 `base+blk×4096` | **信** | mmap 直读 |
| `disk_struct_v2.0.h`（掩码/魔数/size 契约） | **信**（契约 ≠ 实现） | include 共享；**并在 audit 内独立 offset/size/magic 自检**防漂移 |
| `crc32` / `XXH64` 原语 | **信** | 自持实现（不链 god） |
| god 的 SB 解析 / 几何推导 | **不信** | audit 重推 + 外部锚（设备尺寸/LBA）交叉验证 |
| `fs_allocator`（`block_test` / `inode_test` / `all_dump` / 区间重建） | **不信** | 改读**盘面位图字节**自行解码 |
| `mdata_piece_cache` / `InodeSession` / `read_buff` | **不信** | 坐标层直读 |
| `dentry/extent/name` 各 `*_Bptree_walk` 导航 | **不信** | 审计自行重推（既有纪律，保持） |

> **共享结构体定义 ≠ 共享存取实现**：审计校验的是这份盘面布局的**不变量**，不是实现
> 它的代码。故共享契约头不构成共因失效；但必须加独立自检（§4）。

---

## 2. 坐标语言（唯一访存途径）

```
fsblk(blk) = map_ + 4096 + blk×4096        // FS_block[n] → const uint8_t*
block_allocated(blk) = 盘面位图字节直接解码  // 位 i ↔ FS_block i
```

- `audit_image`（`include/audit/audit_image.hpp`）是唯一接入点；审计代码此后
  **禁止**出现第二套地址换算。
- mmap 契约：`PROT_READ | MAP_PRIVATE | MAP_NORESERVE`，64 位，页对齐。

---

## 3. ★ 引导期五道 fail-fast 硬门（F0–F4）

**判据（唯一原则）：fail-fast 只由「坐标空间是否有指称（referent）」决定，且只发生在
`audit_image` 引导阶段；DFS 一旦开始，永不因几何中止。**

理由：几何问题的唯一后果是「算出的地址没有对应的可读字节」。这种状态下的 DFS 结论
是噪声，没有枚举价值 → 该 fail-fast。反之只要坐标空间完整可寻址，剩下一切皆是**内容
发现（BAD）**，而「把所有 BAD 枚举出来」正是审计的存在意义 → 不能中止。

> 前提事实：**v2.0 的 SB 不存位图几何**。几何字段只有
> `world_base_lba / total_blocks / inode_data_ctl / inode_data_plane_pieces_count /
> inode_data_fr_ctl / inode_data_fr_count`；位图几何纯派生
> （`bitmap_blocks = ceil(total_blocks/32768)`，`bitmap_start = total_blocks − bitmap_blocks`）。
> 故「几何不一致」不是 SB 自相矛盾，而是**派生坐标空间 vs 外部事实**对不上，或派生结果退化。

| # | 门 | 判据 | 依据 |
|---|---|---|---|
| **F0** | 身份门 | `magic[0] != RFS_SB_MAGIC_W0` 或 `version != RAVENFS_VERSION` | 「一眼不是 v2.0 盘」= 无坐标系可言 |
| **F1** | 原点门 | `world_base_lba × lba != 4096`（文件模型即 `world_base_lba != 1`） | 整个坐标原点错；对齐 `linux/super.c` 的 `blk0_off != 4096` |
| **F2** | 退化/溢出门 | `total_blocks == 0`；或 `4096 + total_blocks×4096` 溢出 u64；或 `bitmap_start <= 2` | 退化/溢出；后两项对齐 ko 的 `zero geometry` / `geometry underflow` |
| **F3** | 落底门 | 字节源大小 `< 4096 + total_blocks×4096` | 声称存在的块读不到 → 空间未完全落底（**audit 比 ko 多出的外部锚**：文件镜像无自明尺寸） |
| **F4** | inode 面根门 | `inode_pieces == 0`；或 `ctl` 指针域全 1 / `>= total_blocks` / 非 2 对齐；或 direct 段 `ptr + pieces×2 > total_blocks` | 连一个 inode 都定位不了 = DFS 无起点 |

命中任一门 → `gate_t` 非 OK，CLI 退出码 `3`，**不进入 DFS**。

### 3.1 软失败（F5，**不属硬门**）

| 项 | 处理 | 理由 |
|---|---|---|
| **F5** inode 空闲面 `fr` 快照不可读（`fr_ctl.ptr` 越界 / magic / crc / 条目序错） | **软**：记 BAD，照常跑完 DFS 与块侧 XOR，仅把 inode 侧 XOR 标记 `unavailable` | 它**不影响块侧审计**（位图 + 结构）——那些发现独立有效。因一个可读性坏点丢掉整盘其余发现，与审计目的相悖。 |

---

## 4. 非门一致性项（BAD 类：报而不断）

坐标空间可寻址后，以下一律**计入 BAD 并继续**：

- `crc32` 不符（**不在 F0**：F0 只管「一眼身份」magic/version；crc 单点坏而几何完好时，报 `SB_CRC_BAD` 比中止更有价值，下游 F1–F4 门兜住真正不可寻址的情形）；
- 位图自述段 `[bitmap_start, total_blocks)` 非全 1（bitmap 自己的块没标占用）；
- inode 面映射树结构错（洞 / 重叠 / 越界）→ `inodeplane_bad`，受影响 inode 读取返回 BAD；
- fr 快照 magic / crc / 条目键序错 → `inodeplane_bad`；
- backup 链环 / 越界 → 报 BAD、停止扩展白名单，**并提示块侧 XOR 可能过报**（backup 块被当泄露）；
- root inode 槽非 DIR / 字段矛盾 → `inode_bad`。

明显不是错的观察项：**字节源大于 FS 覆盖范围**（分区/扩容合法）→ observe，不算 BAD。

---

## 5. 与本项目其它子项目的关系

- 不依赖 `juvenile_god/`：`tools/audit_v1.2/` 从链接 `libravenfs_juvenile.a` 改为链接
  `libravenfs_audit.a`；`include/ravenfs/ravenfs_audit.hpp` 的旧类型迁移进 `audit/`。
- 与 `linux/` 平级：ko 是**写入方之一**，audit 是**双方共同的裁判**。ko 已有的几何门
  （`linux/super.c`）在 audit 内**独立重推**（不复制），并额外加 F3 外部锚。

---

## 6. 已决 / 未决

**已决（2026-10-06）**：
1. 顶层独立项目，参数 = 路径，自己 open+mmap+坐标；
2. 不信任 `fs_allocator` / `mdata_piece_cache`（读盘面字节而非 allocator 重建）；
3. 契约头共享 + audit 内独立自检；
4. 只留盘面审计，测试改为 flush + 重开（活内存审计退役）；
5. fsck / repair **排除**在本只读项目外，另立工具；
6. 五道 fail-fast 硬门 F0–F4 落定（本文件 §3）。

**未决**：
1. 命中硬门时是否仍产出「部分报告」（当前：只报门 + 原因，不 DFS）；
2. `--force` 是否允许越过「已挂载」防呆（防呆尚未实现，见迁移步 6）；
3. F5 软失败下 inode-XOR `unavailable` 的报告字段命名。

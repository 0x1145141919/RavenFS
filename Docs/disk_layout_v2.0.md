# RavenFS — 磁盘布局 v2.0（**单一权威文档**）

> 2026-10-06 · 覆盖全盘格式契约（块模型 / 超级块 / B+树 / inode / 文件 / 目录 /
> 名字数据面 / 全局 inode 数据面 / xattr / 魔数 / 不变量 / mkfs / 运行时 / 工具）。
>
> 本文**自包含**：不再要求读者交叉阅读 `disk_layout_v1.1.md` / `v1.2_inode.md` /
> `v1.3_name_data_plane.md` 去互相补漏。那三份（及更早的 v1 / block_model）是
> **历史沿革**，其仍然有效的条款已**全部并入本文**；冲突之处以本文 + 盘面头
> （`include/disk_struct_v2.0.h` / `include/magics.h`）为准。
>
> 结构真值 = `include/disk_struct_v2.0.h`（C / C++ 双兼容，含 `static_assert`）；
> 魔数真值 = `include/magics.h`；结果码 = `include/ravenfs/rc.hpp`。

---

## 目录

1. [概述、版本、兼容性](#1-概述版本兼容性)
2. [地址空间与块模型](#2-地址空间与块模型)
3. [几何推导（块位图尾巴）](#3-几何推导块位图尾巴)
4. [超级块 RavenSuperblock](#4-超级块-ravensuperblock)
5. [契约级硬编码：根 inode 索引 ≡ 0](#5-契约级硬编码根-inode-索引--0)
6. [块位图 block bitmap](#6-块位图-block-bitmap)
7. [通用 B+树节点族](#7-通用-b树节点族)
8. [inode 结构 raven_Inode_t](#8-inode-结构-raven_inode_t)
9. [文件数据形态（inline / direct / extent）](#9-文件数据形态inline--direct--extent)
10. [目录与 dentry 树](#10-目录与-dentry-树)
11. [目录名字数据面（name data plane）](#11-目录名字数据面name-data-plane)
12. [全局 inode 数据面（inode data plane）](#12-全局-inode-数据面inode-data-plane)
13. [xattr 块](#13-xattr-块)
14. [魔数总表](#14-魔数总表)
15. [不变量清单](#15-不变量清单)
16. [mkfs v2.0 布局](#16-mkfs-v20-布局)
17. [运行时模型（内存 / 锁 / 刷回 / 装载）](#17-运行时模型内存--锁--刷回--装载)
18. [工具 / 端支持状态](#18-工具--端支持状态)
19. [已知未决 / TODO](#19-已知未决--todo)
20. [代码位置速查](#20-代码位置速查)

---

## 1. 概述、版本、兼容性

**一句话**：v2.0 把 inode 存储从「几何预分配的固定数组 + inode 位图」换成
**超级块统辖的、全局弹性 inode 数据面**（8K 片 direct 起步 + 映射树 + 扁 fr 快照）；
根 inode 索引**契约级硬编码 0**；超级块 `magic` 升 256-bit。**与 v1.x 不兼容 →
reformat，不提供迁移工具。**

| 项 | 值 |
|---|---|
| `RAVENFS_VERSION` | **0**（`disk_struct_v2.0.h`；v1.3 = 4，不兼容） |
| 盘兼容 | 与 v1.x **不兼容**（SB 布局变、magic 变、无 inode 数组/位图）→ reformat |
| SB magic | v1.x 单字 `"RAVN"` → v2.0 **256-bit**（`magic[0]` = `"RAVENFS\0"` 前缀） |
| 根 inode | v1.x 读 SB `root_inode` 字段 → v2.0 **硬编码 0**（SB 无该字段） |
| 长名外置 | v1.1「每目录 10 档名字堆」→ v1.3/v2.0「每目录名字数据面」（§11） |
| DIR `link_count` | v1.1 `size+2` → v1.2 起（D-12）**POSIX `2 + 直属子目录数`**（§8.6） |
| 文件类型值域 | v1.1 `type:2` → v1.2+ `type:6`（§8.2） |

**历史文档地位**：`disk_layout_v1.1.md`（B+树/区段树/dentry/位图/块模型基座）、
`disk_layout_v1.2_inode.md`（inode 语义重定义）、`disk_layout_v1.3_name_data_plane.md`
（名字数据面）、`block_model.md`（块模型）→ 仍有效的条款已在本文重述，**不再需要查阅**。

⚠️ `RAVENFS_VERSION = 0`（0 常被当「未初始化」）是否为最终值**待裁**；若换值须全端同步。

---

## 2. 地址空间与块模型

### 2.1 两个地址空间

- **设备 LBA 空间**：单位 = 设备逻辑块 `LBA_size`（NVMe Identify 获取，不落盘）。
- **FS_block 空间**：单位 = **固定 4096B**，RavenFS 内部**唯一**地址语言。

```c
LBA_size        = LBAF0.LBA_Data_Size;      // 512e→512, 4Kn→4096
multiplier      = 4096 / LBA_size;          // 必须为整数（512e→8, 4Kn→1）
total_dev_lba   = NSZE;
world_base_lba  = multiplier;               // 由 mkfs 写入 SB；FS_block[0] 的 LBA
```

### 2.2 唯一换算公式

```c
// FS_block[n] 在 fd 内的字节偏移：
fblock_to_byte(n) = world_base_lba * LBA_size + n * 4096
                  = 4096 + n * 4096          // ∵ world_base_lba*LBA_size ≡ 4096
```

### 2.3 Superblock 定位

- **Primary SB**：fd `byte[0, 4096)`，**不属于任何 FS_block**，是 FS_block 世界的入口。
- **Backup SB**：散落在 FS_block 世界内，被块位图管理（亮灯、常亮），Primary 的副本 +
  双向链（`backup_prev` / `backup_next` / `backup_count`）。
- `FS_block[0]` 从 fd `byte[4096]` 起；FS_block 世界占 `[4096, 4096 + total_blocks*4096)`。

### 2.4 节点粒度

- **所有 B+树节点恒 8192B = 2 个连续 FS_block**（依赖 `mdata_piece` 8K 片缓存）。
- 节点分配纪律：`block_alloc_hard(2, align_log2=1)` → 2 块连续 + **8K 对齐**（块号偶）。
- 节点内块指针一律指向**首块**。

---

## 3. 几何推导（块位图尾巴）

v2.0 **只有块位图一段尾巴**；无固定 inode 数组、无 inode 位图（inode 归数据面）。

```c
FS_BLOCK_BYTES   = 4096;
BITS_PER_FSBLOCK = 4096 * 8 = 32768;

bitmap_blocks = ceil(total_blocks / 32768);
bitmap_start  = total_blocks - bitmap_blocks;   // 块位图固定在 FS_block 世界尾巴
```

全部**运行期推导、不落盘**（唯一真值 = SB 的 `total_blocks`）。inode 数由数据面片数派生：

```c
inode_data_pieces = RFS_IDP_PIECE_COUNT(sb.inode_data_plane_pieces_count);
inode_count       = inode_data_pieces * RFS_IDP_INODES_PER_PIECE;   // ×32
```

---

## 4. 超级块 RavenSuperblock

Primary & Backup 均为 **4096B（packed）**，布局逐字节相同。

| offset | size | 字段 | 语义 |
|---|---|---|---|
| 0 | 32 | `magic[4]` | 256-bit 魔数：`magic[0]=RFS_SB_MAGIC_W0`（字节 `[0:7]="RAVENFS\0"`），`magic[1..3]` 每卷随机 |
| 32 | 4 | `version` | `= RAVENFS_VERSION`（v2.0 = 0） |
| 36 | 4 | `pad0` | — |
| 40 | 8 | `world_base_lba` | `FS_block[0]` 在设备 LBA 空间的起始 LBA |
| 48 | 8 | `total_blocks` | FS_block 世界总块数 |
| 56 | 8 | `inode_data_ctl` | 映射面控制：`RFS_IDP_CTL_*`（direct/tree + 段基/树根块） |
| 64 | 8 | `inode_data_plane_pieces_count` | 8K 数据面片数原值：`RFS_IDP_PIECE_*` |
| 72 | 8 | `inode_data_fr_ctl` | 空闲面控制：指向**扁 fr 快照区**首块（`RFS_IDP_FR_CTL_*`，非 B+树） |
| 80 | 8 | `inode_data_fr_count` | 扁 fr 快照条目数：`RFS_IDP_FR_*`（条目坐标 = inode） |
| 88 | 16 | `uuid` | 卷 UUID |
| 104 | 48 | `volume_name` | 卷标 |
| 152 | 4 | `crc32` | 覆盖整块（**crc 字段自身置零后**计算；poly `0xEDB88320` 反射，与 mkfs/god 同口径） |
| 156 | 8 | `backup_prev` | Primary: `PTR_INVALID`；Backup: 前驱 FS_block |
| 164 | 8 | `backup_next` | Primary: 链头；Backup: 后继 FS_block（`PTR_INVALID`=链尾） |
| 172 | 4 | `backup_count` | 备份总数 |
| 176 | 8 | `born_time` | mkfs 时刻（秒） |
| 184 | 8 | `latest_mount_time` | 最近挂载（0 = 从未） |
| 192 | 8 | `latest_syn_time` | 最近一次周期同步（sb + 映射面 + 空闲面刷回）时刻 |
| 200 | 8 | `latest_umount_time` | 最近**干净卸载**（0 = 从未） |
| 208 | 8 | `system_mount_time_accumulate` | 系统挂载累计秒 |
| 216 | 8 | `system_mount_counter` | 系统挂载次数 |
| 224 | 8 | `tools_mount_time_accumulate` | 工具挂载累计秒 |
| 232 | 8 | `tools_mount_counter` | 工具挂载次数 |
| 240 | 8 | `abnormal_shutdown_counts` | 非正常关机累计 |
| 248 | 3848 | `pad[3848]` | 248(实字段) + 3848 = 4096；**加字段必须同步减 pad**（`static_assert` 兜底） |

**识别**：只比对 `magic[0] == RFS_SB_MAGIC_W0`（前缀匹配），`magic[1..3]` 随机部分由 crc32 兜底。
**断电判据**：`latest_syn_time > latest_umount_time` ⇒ 上次非正常关机（或仍挂载中）。
**Backup SB 不随动**（保留 mkfs/最后写时刻；时间戳只 Primary 维护）。

---

## 5. 契约级硬编码：根 inode 索引 ≡ 0

```c
#define RFS_ROOT_INODE_IDX  0ULL
```

- `root_dir` 的 inode 恒为**逻辑索引 0** ⇒ 数据面逻辑空间第 0 槽（piece 0 第 0 个 256B）。
- 配合 §8.7 `st_ino = idx + 1` ⇒ **根 st_ino ≡ 1**。
- 这是**格式契约 / 不变量**（v2.0 推翻 v1.2「root = SB.root_inode 局部值」）：
  **idx 0 永久归 root、分配器永不外发**；mkfs 初始 fr 必从 idx 1 起。
- 纪律：mkfs / god / ko / 工具**只准引用 `RFS_ROOT_INODE_IDX`**，禁止散落字面量 0。

---

## 6. 块位图 block bitmap

- v2.0 **唯一**的 FS_block 分配追踪结构；自身也在 FS_block 世界内且自描述。
- **bit 语义（精确）**：`bit[i] = 1 → FS_block[i] 已分配`；`0 → 空闲可分配`。
  覆盖 `[0, total_blocks)`；`bit[i]` 对应 FS_block[i]。
- **放置**：`[bitmap_start, total_blocks)`，该区间每个 bit 在 mkfs 即置 1（不可分配）。
- **常亮者（mkfs / 运营）**：块位图自身、根目录 dentry 树根节点、Backup SB（N 个随机块）、
  **全局 inode 数据面的 physical 资产**（direct 段 / 映射树节点 / fr 快照区）；
  文件/目录数据、名字数据面数据片与树节点按需分配。
- **无隐藏预留**：不存在位图不亮灯却被占用的块。
- **校验（挂载）**：`[bitmap_start, total_blocks)` 的 bit 全 1；失败拒绝挂载。
- 位图落盘：v2.0 运行时分配器**不原地改内存位图**（树版），刷回时由树**反向重建**位图再写（§17）。

---

## 7. 通用 B+树节点族

文件区段树、dentry 树、名字数据面映射树/fr 树、全局 inode 数据面映射树，**共用同一套节点元数据与机械**，
仅**魔数**与**阶/条目类型**不同。

### 7.1 节点元数据 `Bptree_meta_t`（独立 64B 头）

```c
struct Bptree_meta_t {          // 64B
    uint64_t            magics[4];   // 256-bit 扫描签名（全盘恢复识别节点类型）
    struct parent_ptr_t parent_ptr;  // 8B
    struct ptr_in_idx   ptrs[3];     // 24B
};
```

**`parent_ptr_t`**（8B）：`entry_count:10 | is_inner_node:1 | is_root_node:1 | ptr:52`
- `entry_count` = 有效条目数（节点计数唯一来源；10 bit 够 337）。
- 四态解码：根节点 `ptr` = **自身 inode 编号**（即 `.`）；非根 = 父节点块号。

**`ptr_in_idx`**（8B）：`free_to_use:12 | ptr:52`（`free_to_use` 语义由上下文定义；
dentry 里 = `name_len`）。哨兵 `PTR_INVALID = ~0`（52 位全 1）；**0 是合法块号，禁用 0 作哨兵**。

**`ptrs[3]` 角色解码**：

| 节点角色 | ptrs[0] | ptrs[1] | ptrs[2] |
|---|---|---|---|
| 非根叶子 | 预留 | 前驱叶子块号 | 后继叶子块号 |
| 非根内部 | 预留 | 预留 | 预留 |
| 根叶子 | 预留 | `..` inode | 预留 |
| 根内部 | 预留 | `..` inode | 预留 |

### 7.2 阶与尺寸链（恒 8192B；数组 = 阶数 + 1，`[阶数]` 为插入瞬时槽）

```
区段树 / 名字面映射树 / inode 面映射树：337 阶（338 槽）
  inner: 64 + 338×16(hint) + 340×8        = 8192   // [337] 瞬时
  leaf : 64 + 338×24(extent) + 2×8        = 8192   // [337] 瞬时
dentry 树：126 阶（127 槽）
  inner: 64 + 127×16(hint) + 762×8        = 8192   // [126] 瞬时
  leaf : 64 + 127×64(ddentry)             = 8192   // [126] 瞬时
```

- **容量 + 1 瞬时槽（内外统一）**：插入先占瞬时槽 → `count > 阶数` → 分裂；单趟「插→查→裂」。
- **瞬时槽仅存内存，禁止落盘**（误落盘破坏 `count` 语义）。
- **内部节点无最左子树**：上界路由下 N hints = N children，无孤儿子树。

### 7.3 路由 / 分裂 / 平衡

- **上界路由**：
  - 区段族（文件/名字面/inode 面映射树）：`extent_hint_t.key = 子树最大开边界 max(fblkbase+length)`；查找 `target < key[i] → sub_node_ptr[i]`（`target == key` 不入该子树）。
  - dentry 树：`ddentry_hint_t.hash_max = 子树最大 hash`；查找 `target_hash ≤ hash_max[i] → sub_ptr[i]`。
- **区段树机械**：阶 337；非根 `count ∈ [168,337]`，根 `count ∈ [1,337]`；
  分裂 `count > 337` → **左 168 / 右 count−168**（左半最大开边界提升为父 key）；
  欠载 `count < 168` → 平衡链（父指针驱动）；`child+neighbor ≤ 337 → 合并`，否则**抹匀（左 total/2）**；
  根内 `count==1 → root_shrink`；删叶子最大开边界项 → **必刷父 key**。
- **dentry 树机械**：阶 126；分裂（瞬时槽占用后 count=127）→ **右半 64 拷新块 / 左半 63**，
  左半末位 hash 提升；非根半满/欠载阈值 **63**；删除叶子最大项 → 刷父 `hash_max`；**物理删除**（memmove，无 tombstone）。
- **深度上限 64**（超限判成环）。

### 7.4 叶链

非根叶子 `ptrs[1]` = 前驱、`ptrs[2]` = 后继（首尾 `PTR_INVALID`）；兄弟链序 = 层内块序。
根叶子的 `ptrs[1]` = `..`（父目录 inode 编号）。

---

## 8. inode 结构 raven_Inode_t

### 8.1 通用 inode（REG / LNK / CHR / BLK / FIFO / SOCK 共用，256B packed）

| offset | size | 字段 | 说明 |
|---|---|---|---|
| 0 | 8 | `word1` | `type:6 | is_data_root_direct:1 | i_flags:57` |
| 8 | 4 | `uid` | |
| 12 | 4 | `gid` | |
| 16 | 4 | `m_ns_stamp` | mtime 纳秒余数 |
| 20 | 4 | `c_ns_stamp` | ctime 纳秒余数 |
| 24 | 4 | `a_ns_stamp` | atime 纳秒余数 |
| 28 | 4 | `link_count` | 见 §8.6 |
| 32 | 8 | `mode_xattr` | `mode:12 | xattr_ptr:52` |
| 40 | 8 | `size` | 文件字节数 / DIR 条目数 |
| 48 | 8 | `mtime` | 秒 |
| 56 | 8 | `ctime` | 秒 |
| 64 | 8 | `atime` | 秒 |
| 72 | 8 | `data_root` | `PTR_INVALID` = inline；否则按 §9 三态 |
| 80 | 16 | `padding[2]` | 预留 |
| 96 | 160 | `inline_data[160]` | 内容 / 无数据类型 rdev@`[0:4]` |

### 8.2 word1（`RFS_INO1_*`）

`type:6 | is_data_root_direct:1 | i_flags:57`
- `type` = `(i_mode & S_IFMT) >> 12`，值域（其余含 0 非法）：
  `FIFO=1, CHR=2, DIR=4, BLK=6, REG=8, LNK=10, SOCK=12`。
- `is_data_root_direct` 仅 REG/LNK 有意义（1=直指 / 0=extent 树）；DIR/无数据类型忽略（DIR 该位恒 0）。
- `i_flags` 镜像 Linux `S_*`（存值不移位），v1.2+ 恒 0（chattr 降级）。

### 8.3 mode_xattr（`RFS_MODE_*` / `RFS_XATTRPTR_*`）

`mode:12 | xattr_ptr:52`
- `mode = i_mode & 0o7777`（rwxrwxrwx + suid/sgid/sticky）；
- `xattr_ptr` = 单块 xattr 块指针；无 xattr = `RFS_XATTR_NONE`（52 位全 1，**禁用 0**）。
- 恒等式：`i_mode == (RFS_INO1_GET_TYPE(word1) << 12) | RFS_MODE_GET(mode_xattr)`（两切片不相交）。

### 8.4 无数据类型（CHR/BLK/FIFO/SOCK）

`size = 0`、`data_root = PTR_INVALID`、无 extent 树 / 无名字面。
`rdev`（仅 CHR/BLK）存 `inline_data[0:4]`：**小端 u32**，编码 = Linux `new_encode_dev()`
（12 位 major + 20 位 minor）；升级方案 = `inline_data[0:8]` 小端 u64。非设备文件 / FIFO / SOCK 置 0。

### 8.5 时间戳

秒（`mtime`/`ctime`/`atime`）+ 纳秒余数（`m_`/`c_`/`a_ns_stamp`）。

### 8.6 link_count 规则

- **REG / LNK / 其它非 DIR**：`link_count` = 指向该 inode 的 **dentry 条目总数**（含创建者）；
  创建初始 1；`link(2)` 前置全过才 `++`；unlink `--`，归 0 → 回收 inode + 数据。
  崩溃顺序：**先删 dentry（落盘）→ 后 `link_count--`**（窗口内计数虚高，fsck 以 dentry 扫描为准）。
- **DIR（D-12，POSIX）**：`link_count = 2 + 直属 DIR 型子 inode 数`（空目录 = 2）；
  `size` 保持「条目数」原义，与 `link_count` **脱钩**。
  （v1.1 的 `size+2` 语义已废除；存量盘需离线从 dentry 树重建。）

### 8.7 inode 号 ABI

`st_ino = inode_idx + 1`（idx = 数据面逻辑索引 `[0, total_inodes)`）；无保留区；`st_ino` 恒 ≠ 0；
同 FS 内唯一；上限 < 2^32。实现只认 `RFS_INO_TO_IDX()` / `RFS_IDX_TO_INO()`，禁散落 `±1`。

---

## 9. 文件数据形态（inline / direct / extent）

**解码判据只依 `size` 与 `is_data_root_direct`（size 优先，`data_root` 取值不参与判定）**：

```c
if (i.size <= 160)                  // inline：内容在 inline_data[0..size-1]
                                    // data_root 规范 = PTR_INVALID；direct 位无意义
else if (RFS_INO1_GET_DIRECT(i.word1))
                                    // direct：data_root = base_blk，物理覆盖 [base, base+ceil(size/4096))
                                    // 无块级空洞
else                                // extent：data_root = 区段 B+tree 根块号；允许空洞，洞读 0
```

- **inline ≤ 160B**：`inline_data` 承载。
- **direct**：`size > 160 ∧ direct=1`；`data_root` = 连续区首块；物理占块 = `ceil(size/4096)`。
- **extent**：`size > 160 ∧ direct=0`；`data_root` = 树根。

**`extent_t`**（区段树叶条目，24B）：`length:u64 | fblkbase:u64 | block_base:ptr_in_idx`
- **左闭右开 `[fblkbase, fblkbase+length)`，`length` = 块数**；`block_base` 是 `ptr_in_idx`（物理 FS_block）。
- **SOFT_SYM 载荷**：`size` = target 字节数（**不含结尾 NUL**），读回以 size 定界。

**状态迁移 / 写序契约**：
- `size ≤ 160` 一律按内联解读；`size ≤ 160` 而 `data_root ≠ PTR_INVALID` = 不一致（fsck 以 size 为真值清理）。
- inline → direct/extent：写跨 160，内容拷入首块。
- direct 增长需相邻块可续，不可得 → 整文件迁 extent（**数据零搬移**，旧连续区 = 单条 extent）。
- 写洞 / 打洞产生块级洞 → extent。
- truncate 缩小保持 >160 态（direct 释放尾块 / extent 删尾段）；truncate 缩至 ≤160 → **强制回迁 inline**（读回 + 释放数据块与树 + 复位 data_root）。
- extent → direct：不做主动迁移，但**提交期允许「出口缩水」**：根叶 && 严格单条 && 覆盖 `[0, ceil(size))` → 缩 direct（先 inode 后 free 旧根，孤儿语义）。
- **翻转写序（统一）**：位图亮灯 → 数据块/树节点落盘 → **inode 字段翻转**（`data_root`/`direct`/`size` 同一次写 = 提交点）；崩溃窗口 = 孤儿块（fsck 回收）；**禁止先翻 inode**。

**区段树内部节点**：`extent_hint_t { key:u64（子树最大开边界）; sub_node_ptr:ptr_in_idx }`（16B）。

---

## 10. 目录与 dentry 树

### 10.1 raven_dir_Inode_t（DIR，256B packed）

前 80B 与通用 inode 同构（word1 的 direct 位恒 0）；`[72:80)` 名为 `ddentry_Bptree_root`。

| offset | size | 字段 | 语义 |
|---|---|---|---|
| 72 | 8 | `ddentry_Bptree_root` | dentry 树根块号；dir 恒有效（`PTR_INVALID` = 内部契约违反） |
| 80 | 8 | `name_data_ctl` | 名字数据面映射控制（direct/tree + 指针）｜§11 |
| 88 | 8 | `name_data_piece_count` | 8K 数据片数原值 |
| 96 | 8 | `name_data_free_ranges_ctl` | 名字面 fr 控制（inline/tree + 指针） |
| 104 | 8 | `name_data_free_ranges_count` | 名字面 fr 区间总数（>9 ⇒ tree） |
| 112 | 144 | `possible_inline_free_ranges[9]` | inline fr 表（9×16B） |

`DIR size` = 非 `.`/`..` 的 dentry 条目数；`DIR link_count` = `2 + 直属子目录数`（§8.6）。DIR 无 rdev、无内联数据。

### 10.2 ddentry_t（叶条目，64B）

```c
struct ddentry_t {                       // 64B
    struct ptr_in_idx inode_ptr;         // free_to_use = name_len（全名长度，≤552）
    uint64_t          hash;              // 名字 64-bit hash = XXH64(seed=0)
    uint64_t          full_char_remain_offset; // name_len>40: 外置 tail 在名字面内的逻辑字节偏移；否则 PTR_INVALID
    char              name[40];          // 内联名（name_len ≤ 40）
};
struct ddentry_hint_t {                  // 16B
    struct ptr_in_idx sub_ptr;
    uint64_t          hash_max;          // 子树最大 hash
};
```

- 叶内按 **`(hash, name)` 复合严格递增**：hash 定位，碰撞区间内按名字字典序。
- 路由只看 hash（上界 `hash_max`）；碰撞区间内比较**完整名字**（>40B 读名字数据面 tail）。
- `.`/`..` **不在树内**：根节点 `parent_ptr.ptr` = `.`、`ptrs[1]` = `..`；readdir 合成。
- **readdir 按 hash 乱序**输出。
- 删除：物理删除（memmove + 欠载处理）；删叶子最大项必刷父 `hash_max`。

### 10.3 名字规则

- **内联上限 40B**（`DENTRY_INLINE_NAME_MAX`）：`name_len ≤ 40` 全内联。
- **长名**：`name_len > 40`，tail = `name[40..]`（≤512B）存名字数据面；`ddentry.full_char_remain_offset` 存逻辑偏移。
- **名字上限 `RAVENFS_MAX_NAME_LEN = 552 = 40 + 512`**；`name_len` 不含 NUL。
- 校验（fs 级 `name_check_create`，在任何 inode/hash/树访问前，零副作用）：
  空名 → `RC_ERR_NAME_INVALID`；含 `/` → `RC_ERR_NAME_INVALID`；>552B → `RC_ERR_NAME_LEN`。

### 10.4 dentry 节点

`Bptree_ddentry_leaf_node_t`：`meta + ddentry_t entries[127]`（8192B）；
`Bptree_ddentry_inner_node_t`：`meta + ddentry_hint_t hints[127] + padding[762]`（8192B）；
魔数 `DENTRY_MAGIC1..4`。

---

## 11. 目录名字数据面（name data plane）

每目录一个**稠密、8K 对齐的逻辑字节空间** `[0, pieces×8K)`：映射树（逻辑→物理）+
空闲区间表 fr（已映射但内容空闲的字节区间）。长名 tail 按**字节粒度**分配。

### 11.1 映射控制 `name_data_ctl`（8K 对齐偷 bit0）

`bit0 = direct(1)/tree(0)`；`[1:52) = 指针(51)`；`[52:64) = 保留`。
- **direct(1)**：数据面为**单连续物理段**，ptr = 段基块（8K 对齐）；物理块 = base + 逻辑块。bootstrap 态，无映射树。
- **tree(0)**：ptr = 映射树根块。
- **空态** = `RFS_ND_CTL_INACTIVE`（整字全 `0xF`；ptr 域全 1 优先判空）。
- 借位依据：树根 / 段基恒 2 FS_block（8K）对齐 ⇒ 块号偶 ⇒ bit0 恒 0。

### 11.2 映射树 `Bptree_dir_name_data_extent_{leaf,inner}_node_t`

魔数 `NAMEDATA_EXTENT_MAGIC1..4`；与文件区段树**同布局同阶**（337 阶、`extent_t`/`extent_hint_t`）。
**不变量**：逻辑空间稠密无洞、8K 对齐；每条 extent `length` 为**偶数块**（每 8K 片 `length=2`）；物理段 8K 对齐。

### 11.3 空闲区间表 fr（字节单位；两态）

`name_data_free_ranges_ctl`：`bit0 = inline(1)/tree(0)`；`[1:52) = fr 树根`；空态 = ptr 全 1。
- **inline(1)**：内联表 `possible_inline_free_ranges[9]`，键序同叶，`fr_count ≤ 9`；bootstrap 态；
  **插入第 10 条隐式升格 tree**（不回溯、不降级）。
- **tree(0)**：B+树，魔数 `NAMEDATA_FREERANGES_MAGIC1..4`，统一 337 阶。
- `free_range_t { base:u64, len:u64 }`（16B，**字节单位**，半开）：`= 删长名释放 ∪ 新片未分配尾部`；
  「用尽」⇔ 无 `len ≥ need`。inline 表与叶复用同型。
- 叶按**复合键 `(len 主序, base 副序)` 严格递增**；hint = 子树最大复合键 `(max_len,max_base)`；
  路由 = 第一个 `(max_len,max_base) ≥ target` 的子树，无则最右击穿。
  分配 best-fit（target = `(need,0)`）；插入定位 target = 新键 `(len,base)`。
- 运行态**不做相邻/欠载合并**（碎片靠 best-fit 复用）；但**绝不留空叶**（空叶 hint=(0,0) 破坏单调路由 → 余段回填同叶 / `retire_empty_leaf`）。

### 11.4 grow / 离线工具

- **grow（增片）**：direct 物理尾 `block_occupy_soft` 优先；投机失败 `hard_alloc(2,1)` +
  `name_data_map_append`（映射树尾插 / direct→tree 升级）。新增 8K 片，其未用尾部成为空闲区间。
- **`name_data_rebuild`（离线压实）**：tree 碎片映射 → 单连续 direct（`hard_alloc(pieces*2, 8K)` → 逐片迁数 → 提交 ctl=direct → reclaim 旧树+旧片）；direct/空态 no-op。
- **`name_data_fr_compact`（离线压缩）**：dump 全部区间 → 按 base 全量相邻合并 → ≤9 条降级 inline / >9 升格 tree → 释放旧树。

---

## 12. 全局 inode 数据面（inode data plane）

v2.0 **新范式**：inode 存储 = 一段**弹性、逻辑紧密堆叠的 256B inode 数组**，物理以
**8K 片**为单位从块分配器取块、经**映射树**定位；空闲用**扁 fr 快照**挂账。
（v1.x 的固定 inode 数组 + inode 位图**整体切除**。）

### 12.1 坐标语言（★ 唯一换算入口）

```
inode 索引  inum / idx   单位 1 inode = 256B   [0, pieces×32)   ← 主坐标
   │ idx<<8
逻辑字节    loff          单位 1B              [0, pieces<<13)
   │ loff>>12
逻辑 4K 块  lblk = idx>>4 单位 4096B           [0, pieces<<1)    ← 映射树 extent_t 坐标
   └ 等价：逻辑片 pidx = idx>>5（8K）
```

几何常量（`RFS_IDP_*`）：`INODE_BYTES=256`、`PIECE_BYTES=8192`、`BLOCKS_PER_PIECE=2`、
`INODES_PER_BLOCK=16`、`INODES_PER_PIECE=32`；`SHIFT inode→byte=8 / →block=4 / →piece=5`。
换算宏 `RFS_IDP_INODE_TO_BYTE/_BLOCK/_PIECE`、`_BYTE/_BLOCK/_PIECE_TO_INODE`。

**两面各持坐标（禁止串用）**：
- **映射面**（盘上 B+树 `Bptree_inode_data_extent_*`）：键 = **lblk**；记录 `[fblkbase, fblkbase+length)` 块 → 物理 FS_block。`extent_t` 单位**恒 4K**；纪律 `fblkbase`/`length` **均 2 对齐**（每片 length=2、段尾亦 2 对齐）。
- **空闲面**（内存 RBT / 盘上扁快照）：键 = **inum**；专用类型 `inode_dataplane_coordinate_free_range_t{base,len}`（单位 = inode = 256B；字节量 `<<8`）。

**resolve(idx)**（规范路径）：
```c
lblk = idx >> 4;
ext  = mapping_tree_lookup(lblk);            // lblk ∈ ext 覆盖域
phys_blk = RFS_PTR_ADDR(ext.block_base) + (lblk - ext.fblkbase);
phys_off = (idx & 15) * 256;
dst      = fblock_to_byte(phys_blk) + phys_off;
```

### 12.2 映射面（B+树快照）

`Bptree_inode_data_extent_{inner,leaf}_node_t`：与文件区段树**逐字节同构**（`Bptree_meta_t` /
`extent_t` / `extent_hint_t`，337 阶），魔数独立 `INODE_DATA_EXTENT_MAGIC1..4`。
- 路由：内部 `key` = 子树最大开边界 `fblkbase+length`。
- **盘上形态 = 快照**：由内存映射 RBT **整棵 bulk build**（非增量补丁）；挂载 bulk-load 回 RBT。
- `inode_data_ctl`：`direct(1)` ptr=单连续段基块（8K 对齐）/ `tree(0)` ptr=映射树根块；空态 ptr 全 1。

### 12.3 空闲面（扁 fr 快照）

```c
struct inode_dataplane_coordinate_free_range_t { uint64_t base; uint64_t len; };  // 16B，inode 单位
```
```c
struct inode_data_fr_snapshot_hdr {             // 64B
    uint64_t magic[4];      // INODE_DATA_FR_SNAPSHOT_MAGIC1..4（非 B+树魔数）
    uint64_t entry_count;   // = SB.inode_data_fr_count（交叉校验）
    uint64_t capacity;      // 本区条目容量
    uint32_t crc32;         // 覆盖 hdr(除 crc) + 全部 entries
    uint32_t rsv0;
    uint64_t rsv1[1];       // 补齐 64B（水位线不落盘）
};
```
区布局：`[0:64) hdr` + `entry_count × 16B`；区块数 = `ceil((64 + count×16)/4096)`；
**首块 1 块（4K）对齐**（2026-10-06 定；bit0 为纯保留位恒 0，与对齐无关）。
SB `inode_data_fr_ctl` = 区首块指针；`inode_data_fr_count` = entry_count。

### 12.4 分配器水位线（运行时，不落盘）

全局 fr 的**分配水位线**唯一归属 = 运行时 `include/ravenfs/fs_allocator.hpp` 的
`mgr_slice_t.last_finish_hint_`（只进不退游标）。盘面只存「空闲区间事实」。

### 12.5 四控制字（与名字数据面同源同构）

| SB 字段 | ↔ 名字面 | 含义 |
|---|---|---|
| `inode_data_ctl` | `name_data_ctl` | 映射面 direct/tree + 指针 |
| `inode_data_plane_pieces_count` | `name_data_piece_count` | 8K 片数原值 |
| `inode_data_fr_ctl` | `name_data_free_ranges_ctl` | 扁 fr 快照区指针（无 inline 态，bit0 纯保留） |
| `inode_data_fr_count` | `name_data_free_ranges_count` | fr 条目数 |

**唯一语义分叉**：`inode_data_fr_ctl` 指向**扁快照区**（非 B+树、无 inline 态）——全局 fr 载体是
SB + 整刷扁数组，无「省块」收益，故不搞 inline。

**A/B 混合（2026-10-05 定）**：
```
内存（全量常驻 + 两把互斥锁，热路径零盘上节点 I/O）
  映射 RBT（lblk→phys）  +  空闲 RBT（best-fit, inode 单位） + 水位线
        │ 周期 / 卸载刷回
        ▼
盘上快照：映射面 = 新 B+树（整棵 bulk build）；空闲面 = 扁 fr 快照（整段重写）
SB：两控制字同处一块 ⇒ 单块写即原子换新 ⇒ 「映射/空闲」成对一致
```

---

## 13. xattr 块

inode 侧只有 `mode_xattr.xattr_ptr`（52 位块指针）；块**单块自描述**（1 个 FS_block = 4096B，
非 8K piece）；放不下 → `-E2BIG`。净容量 `RFS_XATTR_USABLE = 4096 − 32(hdr) − 4(终止零) = 4060B`。

```c
struct rfs_xattr_block_hdr {           // 32B
    uint32_t h_magic;                  // "RFSX" = 0x52465358（轻识别，非校验和）
    uint32_t h_refcount;               // v1.2 恒 1
    uint32_t h_blocks;                 // 恒 1
    uint32_t h_hash;                   // v1.2 置 0
    uint64_t h_reserved[2];
};
struct rfs_xattr_entry {               // 8B 定长头；名字紧随（4B 对齐）；值在 e_value_offs 处
    uint8_t  e_name_len;               // 名字后缀长（不含命名空间前缀）
    uint8_t  e_name_index;             // 命名空间索引
    uint16_t e_value_offs;             // 值在块内偏移
    uint32_t e_value_size;
};
```
- 两区相向生长：条目区从 `[32)` 向高地址增长，以**首个 u32 == 0** 终止；值区从块尾向低地址、每值 4B 对齐。
- `RFS_XATTR_LEN(nl) = ((nl+3+8) & ~3)`；命名空间索引抄 ext4：`user=1, acl_access=2, acl_default=3, trusted=4, security=6, system=7`。
- 生命周期：重复 set 覆盖；删最后一条 → 释放块 + 指针回 `RFS_XATTR_NONE`；**空 xattr 块禁止留盘**；不要求条目排序。

---

## 14. 魔数总表

| 用途 | 宏 | 值 |
|---|---|---|
| SB（v2.0） | `RFS_SB_MAGIC_W0` / `SUPERBLOCK_MAGIC_V20_W0` | `0x0053464E45564152` |
| SB（v1.x，对比） | `SUPERBLOCK_MAGIC` | `0x5241564E`（"RAVN"） |
| 文件区段树 | `BPTREE_MAGIC1..4` | `0xE2C21AFE1C35CFF8 / 0xBF483E2462E9EA83 / 0x38F45A47363D9F3D / 0x66EC3C500307DA82` |
| dentry 树 | `DENTRY_MAGIC1..4` | `0x1407B23B4E3119EF / 0x09429E79C450AF5D / 0xD9C792AE76308B5B / 0x0586CE7C1EDF3B9C` |
| 名字面映射树 | `NAMEDATA_EXTENT_MAGIC1..4` | `0xC372D1FC567D65CF / 0x6E702B0D21436D8E / 0x0CCD38A459D96B13 / 0xD5F8B50F3ED12C4D` |
| 名字面 fr 树 | `NAMEDATA_FREERANGES_MAGIC1..4` | `0xFFD7291C71147B5E / 0xDC68716E675F2409 / 0x3994B6520C33B94F / 0xC49BE2A5D4F3C1A1` |
| inode 面映射树 | `INODE_DATA_EXTENT_MAGIC1..4` | `0xCBF11C1152260768 / 0x402E275272ECE079 / 0xE9FE8A9551304699 / 0xA051B9AFF19DBF8A` |
| inode 面 fr 快照 | `INODE_DATA_FR_SNAPSHOT_MAGIC1..4` | `0x6C591CA8002381E5 / 0x7C4D62EC982323E4 / 0x064D499E99983279 / 0xB335E5A62E4F1586` |
| xattr 块 | `RFS_XATTR_H_MAGIC` | `0x52465358`（"RFSX"，非校验和） |

**哨兵统一**：`PTR_INVALID = ~0`；指针域全 1 = 无效；`RFS_XATTR_NONE = RFS_MASK52`；
`RFS_ND_CTL_INACTIVE` / `RFS_ND_FR_CTL_INACTIVE` = `~0`；`RFS_IDP_CTL_INACTIVE` = `~0`。
**0 是合法块号/inode 索引，禁用 0 作哨兵**。

---

## 15. 不变量清单

**B+树通用**
1. 叶条目按各自复合键严格递增；`count ≤ 阶数`（`parent_ptr.entry_count`）。
2. 内部 hint 按 key/hash_max 递增，恒 = 对应子树最大键。
3. 上界路由：区段族 `target < key[i] → sub[i]`；dentry `target ≤ hash_max[i] → sub[i]`。
4. 分裂提升左半最大键；删叶最大项必刷父 key。
5. 瞬时槽（区段族 `[337]`、dentry `[126]`）**永不落盘**。
6. 内部节点无最左子树；节点块恒 2 连续 + 8K 对齐。
7. 非根叶 `ptrs[1]/ptrs[2]` = 前驱/后继；根 `ptrs[1]` = `..`，`parent_ptr.ptr` = `.`。
8. 深度 ≤ 64。

**inode**
9. DIR `ddentry_Bptree_root` 恒有效（`PTR_INVALID` 非法）。
10. 文件三态由 `size` 与 direct 位唯一定；`size ≤ 160` 而 `data_root ≠ PTR_INVALID` = 不一致（fsck 以 size 清）。
11. `type ∈ {1,2,4,6,8,10,12}`；其余含 0 = 硬错。
12. 无数据类型：`size==0 ∧ data_root==PTR_INVALID ∧ 无树`；`rdev` 仅 CHR/BLK 有效。
13. `i_mode == (type<<12) | mode` 恒成立；`xattr_ptr` 非 0（无 xattr 用全 1 哨兵）。
14. REG/LNK `link_count` = dentry 引用数，已分配恒 ≥1（归 0 = 孤儿）；DIR `link_count = 2 + 直属子目录数`。
15. 翻转写序：位图亮灯 → 数据/节点落盘 → inode 字段一次写（提交点）；禁止先翻 inode。
16. `st_ino = idx + 1`，无保留区，恒 ≠0，唯一，上限 < 2^32；根 idx ≡ 0。

**名字数据面**
17. 逻辑空间 `[0, pieces×8K)` 稠密无洞、8K 对齐；每片 = `length 2` 的 extent。
18. `name_data_ctl` 空态 ⇔ `piece_count==0`；非空必为 direct 或 tree。
19. fr 两态：`inline ⇔ fr_count ≤ 9`；`tree ⇔ fr_count > 9`；区间互不重叠、落 `[0,pieces×8K)`；`fr_count == Σ 区间`。
20. fr 叶复合键 `(len,base)` 全局严格递增；hint = 子树最大复合键；树内无空叶；根内不得只剩 1 孩。

**全局 inode 数据面**
21. 逻辑空间 `[0, pieces×32)` 稠密无洞；fr ⊆ 其中、互不重叠、**不含 idx0**、单位 = inode。
22. 映射树 `extent_t` 单位恒 4K；`fblkbase`/`length` 均 2 对齐（每片 length=2）。
23. fr 快照 `entry_count == SB.inode_data_fr_count`；`crc32` 覆盖 hdr(除 crc) + 全部条目。
24. 句柄两指针（`inode_data_ctl` / `inode_data_fr_ctl`）同处 SB ⇒ 快照成对原子换新。
25. `RavenSuperblock` 恒 4096B；加字段必须同步减 `pad[]`。
26. `magic[0]` 恒 = `RFS_SB_MAGIC_W0`；识别只比对 `magic[0]`。

**xattr**
27. `xattr_ptr ≠ 哨兵` ⇒ 块亮灯且被恰一个 inode 认领；`h_magic/h_refcount==1/h_blocks==1/h_hash==0`；
    条目索引合法、终止符存在、`e_value_offs` 合法且 4B 对齐、值区间不重叠、总长 ≤ 4096。

---

## 16. mkfs v2.0 布局

几何：`bitmap_blocks = ceil(total_blocks/32768)`；`bitmap_start = total_blocks - bitmap_blocks`；
（无 inode array / 无 inode bitmap）。

**低块确定性布局（FS_block 号）**：
```
[0, 2)     根 dentry 根叶（8K）
[2, 66)    inode 数据面 direct 段：256KB = 64 块 = 32 个 8K 片 = 1024 inode（起点 8K 对齐）
[66, 67)   扁 fr 快照区（1 块；hdr 64B + 1×free_range）
[67, bitmap_start)  Backup SB × N（随机散布，避让）
```

**SB 写入**：
```
magic[0] = RFS_SB_MAGIC_W0 ; magic[1..3] = uuid 派生随机
version  = RAVENFS_VERSION
inode_data_ctl = direct(1) | ptr=2
inode_data_plane_pieces_count = 32
inode_data_fr_ctl = ptr=66 ; inode_data_fr_count = 1
root inode @ idx0：DIR / 0755 / xattr=NONE / link_count=2 / ddentry_Bptree_root=0
                 名字数据面空态哨兵（RFS_ND_CTL_INACTIVE / RFS_ND_FR_CTL_INACTIVE）
fr 条目 = { base=1, len=1023 }   // idx0 归 root
```
根 dentry 根叶：`parent_ptr.ptr = "." = RFS_ROOT_INODE_IDX(0)`；`ptrs[1] = ".." = 0`。
Primary SB **最后写**（崩溃安全）；Backup SB 同 `magic[0..3]`、改 prev/next、重算 crc。
CLI：`mkfs.ravenfs --v2.0 [-f]`；`--inode-ratio/--inode-count` 在 v2.0 下被忽略（警告）。

---

## 17. 运行时模型（内存 / 锁 / 刷回 / 装载）

### 17.1 缓存层 `mdata_piece_caches_mgr`（8K 片）

- 键 = **物理 FS_block**（8K 片基，偶）；覆盖 `[0, bitmap_start)`；三后端
  `no_cache` / `offload_cache`（mmap 窗口直映）/ `self_reliat_cache`（TTL 懒加载）。
- **B+tree 节点与 inode 数据面片共用同一缓存**（都 8K、8K 对齐）。
- 会话 `mdata_piece_session`（RAII，accessor 引用计数）。

### 17.2 inode 会话 `InodeSession`

- `inode_io_t { fs_allocator_t *plane; mdata_piece_caches_mgr *cache; }`。
- 构造：`plane->inode_locate(inum)` 翻译出物理 8K 片基 → 内嵌 `mdata_piece_session` →
  `inode = &data[inum & 31]`；`dirty()` 片粒度脏标记；析构归还。
- **`inode_read`/`inode_write` 已废除**：inode 访问统一走 `InodeSession`。

### 17.3 分配器 `fs_allocator_t`（双组）

- **block 组**：分片 `mgr_slice_t`（主从红黑树，位/块粒度），`block_alloc_hard/soft/occupy_soft/free/test`。
- **inode 组** = `inode_data_plane_t`：
  - 翻译面 extent 树（key=lblk）+ **翻译锁**；空闲面 fr 主从红黑树（inode 单位）+ **空间锁**。
  - API：`inode_locate`（翻译读锁）/ `inode_alloc`（空间写；用尽 → 空间写→翻译写 grow）/
    `free`（空间写，自动融合）/ `test`（空间读）。
  - grow：`block_alloc_hard(2k, 1)` 逐级折半降至 1；先注入 extent（物理尾邻接则合并）→ 再把
    新片 32k 个 inode 入 fr。
  - **锁序（硬）**：`空间锁 > 翻译锁`；`空间锁 > block 片锁`；**禁止翻译→空间**。
- `dump_extents` / `dump_fr`（fr 出参 = `inode_dataplane_coordinate_free_range_t`）供上层取快照。

### 17.4 同步落盘 `all_sycn`（上帝类；友元访问私有树）

顺序（硬）：
1. **inode 数据面维护**：extent —— 单连续段写 direct，否则**整棵映射 B+tree bulk build**；
   fr —— **先全量释放老区 → `hard_alloc`（1 块对齐）新扁数组 → 写入**；刷新 SB 四字段。
2. 刷 mdata_piece 脏页（新树节点/片内容落盘）。
3. **block 位图回写**：全占用起步 → 各 block 片树 `all_dump` 清空闲位 → pwrite。
4. **SB 最后写**（crc 重算；`latest_syn_time`）。因 ① 取块改 block 分配器，② 在其后重建才能捕获。

### 17.5 挂载装载

- block 位图入内存/窗口切片 + 尾巴白名单校验。
- 建 mdata_piece 缓存（tree 态 bulk-load 经它读映射树）。
- inode 数据面：direct → 单 extent；tree → **栈式 DFS bulk-load** 映射 B+tree；
  fr → 按条目数读扁快照。
- 构造 `fs_allocator_t`（block 位图源 + inode 面）→ `inode_io` → 经 `InodeSession` 读 root inode。

### 17.6 断电判据 / 周期

`latest_syn_time > latest_umount_time` ⇒ 非正常关机（或仍挂载）。30s 周期刷回（sb + 映射面 + 空闲面）为设计目标。

---

## 18. 工具 / 端支持状态

| 端 | 状态 |
|---|---|
| `mkfs --v2.0`（`mkfs/mkfs_v20.cpp`，独立 TU） | ✅ 落地并实测 |
| `tools/sbinfo`（按 magic 分派多 TU；解读数据面/扁 fr/root inode） | ✅ 落地并实测 |
| `tests/header_selfcheck_v20.cpp`（尺寸/offset/契约常量/坐标恒等/位域断言） | ✅ |
| god `RavenFS_juvenile`（挂载/读写/分配/落盘/tree 重挂载/audit v2.0） | ✅ 落地并全测全绿 |
| ko（`super.c` / `inode_cache.c` / `allocator_adapter.c`） | ⬜ 待办 |
| `tools/rfs_trim` / `rfs_compact` / `bitmap_hotspot` / `check_super.py` | ⬜ 待办（`rfs_compact` 仍用废接口） |
| `bitmap_slices.{hpp,cpp}` / `_v2` | 已从构建移出（待删文件本体） |

---

## 19. 已知未决 / TODO

1. **`RAVENFS_VERSION = 0`** 是否最终值（0 常被当「未初始化」）；若换值须全端同步。
2. **分配器水位线不落盘**：崩溃窗口内如何保证「不重复分配」——当前归运行时 `fs_allocator`
   纪律（单调 / 分配前持久化待定）。
3. **30s 定时刷回**：当前仅在卸载 `all_sycn` 全量刷回；周期线程 + 断电判据待接线。
4. **迁移**：v1.x → v2.0 无迁移工具（破坏性 reformat）。
5. **fr 区对齐**：现为 1 块（4K）对齐（§12.3）；盘头旧注释「8K」已同步修正。

---

## 20. 代码位置速查

- **契约**：`include/disk_struct_v2.0.h`、`include/magics.h`、`include/ravenfs/rc.hpp`。
- **运行时**：`include/ravenfs/fs_allocator.hpp`（分配器双组 + inode 数据面）、
  `include/ravenfs/inode_cache_radix.hpp`（`InodeSession` / `inode_io_t`）、
  `include/ravenfs/mdata_piece_caches.hpp`（8K 片缓存）、`include/ravenfs/cache_wheel.hpp`（共享轮子）、
  `include/ravenfs/ravenfs_v1.1.hpp`（上帝类接口）。
- **实现**：`juvenile_god/ravenfs_juvenile.cpp`（挂载/分配转发/`all_sycn`/dump）、
  `juvenile_god/fs_allocator.cpp` + `fs_allocator_router.cpp`（分配器）、
  `juvenile_god/inode_cache.cpp`（InodeSession）、`juvenile_god/mdata_piece_cache.cpp`、
  `juvenile_god/name_data_plane.cpp` + `name_data_freeranges.cpp`、
  `juvenile_god/extent_*.cpp` / `dentry_*.cpp` / `direct_file_ops.cpp` / `root_dir.cpp` / `audit.cpp`。
- **测试**：`tests/header_selfcheck_v20.cpp`、`test_fs_allocator{,_router,_inode_plane}.cpp`、
  `stress_fs_allocator{_slice,_mt}.cpp`、`test_{root_dir_insert,root_dir_delete,name_data_plane,
  name_data_freeranges,name_data_offline,audit_dfs,extent_ops}.cpp`、`stress_name_data_fr.cpp`。

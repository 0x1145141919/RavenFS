# RavenFS Block Model

> 2026-07-19 定稿（块模型本体：坐标系 / 几何推导 / 挂载校验）。
> 2026-08-29 v1.1 对齐：坐标系与块模型**全部继承 v1，无变化**（`disk_layout_v1.1.md` §0），
> 本文指针类型标注（§5）、bitmap 亮灯表（§6.3）、示例（§8）与引用更新至 v1.1 时代
> （对齐 `include/disk_struct_v1.1.hpp`）。
> 定义 RavenFS 的两层块坐标系：设备 LBA 空间 与 FS_block 空间。
> 本文件是 `disk_layout_v1.1.md` 的底层契约，**所有涉及"块号"的字段必须先查此坐标系定义**。

---

## 1. 坐标系总览

RavenFS 中存在两个独立的地址空间：

| 坐标系 | 单位 | 谁定义 | 使用方 |
|--------|------|--------|--------|
| 设备 LBA | `LBA_size`（NVMe Identify Namespace 获取） | 硬件/驱动 | Superblock 的物理定位、I/O 请求提交 |
| FS_block | 4096 字节（固定） | `Superblock.world_base_lba` + 运行期推导 | Bitmap、Inode、B+tree 全部内部地址 |

FS_block 空间的地址是 RavenFS **内部唯一地址语言**。设备 LBA 空间只在 mount 入口和 I/O 下发时参与换算。

---

## 2. 设备 LBA 空间

设备 LBA 空间由两个常量定义，**不存于 superblock，驱动初始化时必然已知**：

```
LBA_size           ← NVMe Identify Namespace LBAF0.LBA_Data_Size（单位：字节）
                    例：512e → 512，4Kn → 4096，FDP → 可查

total_dev_lba      ← NVMe Identify Namespace NSZE（单位：LBA）
                    例：1TB 512e 盘 → 1953525168
```

**设备模型统一**：不论整盘（`/dev/nvme0n1`）、分区（`/dev/nvme0n1p1`）还是普通文件，用户态打开后均视为一个从 byte 0 开始的连续地址空间。
内核已在块设备层自动翻译分区偏移，用户态无需感知 `partition_start_lba`。

换算到字节一律用 `LBA_size`：

```
byte_offset = lba_number × LBA_size
```

---

## 3. FS_block 空间 — 世界定义

### 3.1 锚点与换算

FS_block 世界由 `RavenSuperblock` 中字段以及相关隐式常量联合定义：

```
world_base_lba    ← FS_block[0] 的 LBA 号（单位：device LBA）。
                    恒等于 multiplier = 4096 / LBA_size。
                    Mkfs 时写入，之后只由离线扩容工具修改。

total_blocks      ← FS_block 世界中的总块数（单位：FS_block）

multiplier        ← FS_block_size / LBA_size = 4096 / LBA_size
                    不落盘，运行时推导。必须为整数。
                    512e → 8，4Kn → 1
```

**核心换算公式（唯一路径）：**

```
FS_block[n] 在打开设备/文件 fd 内的字节偏移:

   字节偏移 = world_base_lba × LBA_size + n × 4096
            = 4096 + n × 4096                       ← 因 world_base_lba × LBA_size ≡ 4096

   即 Primary Superblock 占据 fd byte[0, 4096)，FS_block[0] 从 byte 4096 开始。
```

### 3.2 几何推导（零冗余）

以下值**全部不落盘**，运行时从 `total_blocks` 和 `inode_array_count` 链式推导。
**此节是唯一推导源，disk_layout_v1.1.md 中所有区间定义以此为准。**

```
// 一、常量
FS_BLOCK_BYTES   = 4096
BITS_PER_FSBLOCK = FS_BLOCK_BYTES × 8          // = 32768
INODE_BYTES      = 256                         // v1.1 固定（尺寸继承 v1，disk_layout_v1.1.md §3）

// 二、从 total_blocks 出发
bitmap_blocks       = ceil(total_blocks / BITS_PER_FSBLOCK)
                     = ceil(total_blocks / 32768)

bitmap_start        = total_blocks - bitmap_blocks
                     // Bitmap 固定在 FS_block 世界尾巴

// 三、从 inode_array_count 出发
inode_blocks        = ceil(inode_array_count × INODE_BYTES / FS_BLOCK_BYTES)
                     = ceil(inode_array_count × 256 / 4096)

inode_bitmap_blocks = ceil(inode_array_count / BITS_PER_FSBLOCK)
                     = ceil(inode_array_count / 32768)

// 四、尾巴连续布局（自右向左: Bitmap → Inode Array → Inode Bitmap）
inode_array_base          = bitmap_start - inode_blocks
inode_array_bitmap_base   = inode_array_base - inode_bitmap_blocks

// 五、等价校验
// bitmap_start + bitmap_blocks == total_blocks
// inode_array_base + inode_blocks == bitmap_start
// inode_array_bitmap_base + inode_bitmap_blocks == inode_array_base
```

**Superblock 只需存 `total_blocks` 和 `inode_array_count`，零冗余。**
`world_base_lba` 由 `multiplier` 恒等推导，不携带额外信息。

---

## 4. Superblock — 双角色设计

RavenFS 中存在两个角色的 Superblock：**Primary（上岗）**和 **Backup（备胎）**。

### 4.1 Primary Superblock（上岗）

**不在 FS_block 世界内。**

物理位置是**打开设备/文件 fd 的 byte offset 0**。
指向该位置的 `RavenSuperblock.magic` 必须为 `"RAVN"`。

```
打开 fd:
  byte[0, 4096) ─────────────────→ Primary Superblock
                                    ↑
                                不在 FS_block 区间内
```

**Primary 与 FS_block 世界的关系：**

- Primary 是 **FS_block 世界的定义者和入口**
- Primary 本身不占用任何 FS_block 编号
- `bitmap` 覆盖 `[0, total_blocks)`，不包含 Primary
- `world_base_lba` 恒等于 `multiplier = 4096 / LBA_size`。
  Primary 占据 fd byte[0, 4096)，**FS_block[0] 从 byte 4096 开始**。

### 4.2 Backup Superblock（备胎）

**在 FS_block 世界内，随机散落。**

Backup 是 Primary 的完整冗余副本，通过双向链表串联，用于容灾。

```
FS_block 世界:
  ┌──────┬──────┬──────┬──────┬──────┬──────┐
  │Bkup#0│ Data │Bkup#1│ Data │ Data │Bkup#2│  ← 随机散布
  │SB    │      │SB    │      │      │SB    │
  └──┼───┘     └──┼───┘              └──┼───┘
     └──────┬──────┘                    │
            └──────────────┬────────────┘
                     双向链表
```

**字段语义（统一结构体，角色双解码）：**

| 字段 | 指针类型 | Primary | Backup |
|------|---------|---------|--------|
| `backup_prev` | 块指针 | PTR_INVALID（哨兵） | 前驱块号（PTR_INVALID=链头） |
| `backup_next` | 块指针 | 链头块号 | 后继块号（PTR_INVALID=链尾） |
| `backup_count` | — | 备份总数 | 备份总数（交叉校验） |

**Backup 与 FS_block 世界的关系：**

- Backup 在 FS_block 世界内，`bitmap` 管理它
- Backup 占用的 FS_block 在 mkfs 时亮灯（`bit[i] = 1`），运营期间常亮
- 换算时 Backup 的 FS_block 地址也走同一换算公式（block_model.md §3）

### 4.3 挂载校验链

```
1. 读 Primary（partition offset 0）→ 取 backup_next, backup_count
2. 校验 Primary 自身 crc32
3. 从 backup_next 开始遍历 Backup 链表：
   a. 每个 Backup 校验 magic + crc32
   b. 校验 common 字段与 Primary 全同
   c. 校验自身的 backup_count == Primary.backup_count
4. 遍历计数必须 == Primary.backup_count

若 Primary 损坏 → 遍历 backup_next... 但备胎的 backup_next 也必须读 Primary 才知道。
v1/v1.1 容灾恢复均须辅助工具先魔数扫描找到所有 Backup，再选举主用。
```

### 4.4 扩容对 Backup 的影响

离线扩容只搬元数据（bitmap, inode array），Backup 原地不动。
Backup 的 `total_blocks` 不会自动更新——
v1.1 继承 v1：禁用 online expand 场景下 Backup 与 Primary 不一致的可能。
扩容工具必须一并更新所有 Backup 的 `total_blocks` 和 bitmap 相关字段。

---

## 5. 字段指针类型标注（v1.1）

v1.1 指针类型化（`disk_layout_v1.1.md` §2）：所有"地址/索引"字段经位域结构体
`ptr_in_idx` / `parent_ptr_t` 承载，替代 v1 的裸 u64 + MASK 宏。三类指针语义不变：

| 指针类型 | 载体 | 含义 | 取值空间 |
|---------|------|------|---------|
| **块指针（Block Ptr）** | `ptr_in_idx.ptr` / `parent_ptr_t.ptr` | FS_block 索引 | `[0, total_blocks)` |
| **Inode 指针（Inode Ptr）** | `ptr_in_idx.ptr` / `parent_ptr_t.ptr` | inode array 索引 | `[0, inode_array_count)` |
| **联合指针（Union Ptr）** | `ptr_in_idx` / `parent_ptr_t` 整体 | 位域复用，由上下文位域解码为块指针或 Inode 指针 | 见各字段说明 |

```
ptr_in_idx:    free_to_use:12 | ptr:52
parent_ptr_t:  entry_count:10 | is_inner_node:1 | is_root_node:1 | ptr:52
```

- `free_to_use` 语义由上下文定义（当前唯一实例：Dentry_t: name_len；名字堆控制器用裸 52 位指针，不走 `ptr_in_idx`）
- `parent_ptr_t` 双位解码四态（根/非根 × 内/叶）；根节点 `ptr` = 自身 inode 编号（`.`），非根 = 父节点块号
- 三个取值空间彼此独立。联合指针的当前语义由上下文位域决定，不得脱离位域单独解读裸值

**哨兵值统一约定（`include/disk_struct_v1.1.hpp`，禁止裸写 `0` 或 `~0ULL`）：**

| 宏 | 值 | 语义 |
|----|----|------|
| `PTR_INVALID` | `~0ULL` | 通用无效指针（v1 的 `FS_BLOCK_PTR_INVALID` / `INODE_PTR_INVALID` 合并；`DENTRY_TOMBSTONE_PTR` 随 v1.1 物理删除弃用） |

`0` 在两类指针取值空间内都是**合法指针**（根目录 inode 编号为 0，v1.1 继承 v1 惯例），
因此"无指针/已删除"一律用 `PTR_INVALID` 哨兵，不得复用 `0`。

**B+tree 节点块数**：v1.1 四种节点（区段内/叶、dentry 内/叶）恒为 8192B =
2 个连续 FS_block，块号字段一律指向**节点首块**（`disk_layout_v1.1.md` §4）。

---

### RavenSuperblock

| 字段 | 指针类型 | 说明 |
|------|---------|------|
| `magic` | — | 固定魔数 |
| `version` | — | 格式版本号（v1.1 起 bump = 2） |
| `pad0` | — | 对齐填充（替代 v1 落盘的 LBA_size；LBA_size 由 NVMe Identify 已知，不落盘） |
| `world_base_lba` | device LBA | 非指针。FS_block 世界锚点，单位与设备 LBA_size 一致 |
| `total_blocks` | — | 非指针。定义块指针取值上限 |
| `inode_array_count` | — | 非指针。定义 Inode 指针取值上限 |
| `root_inode` | Inode 指针 | 根目录 inode 索引（恒 0） |
| `uuid` | — | — |
| `volume_name` | — | — |
| `crc32` | — | — |
| `backup_prev` | 块指针 | Primary: PTR_INVALID（哨兵）\| Backup: 前驱块号，PTR_INVALID=链头 |
| `backup_next` | 块指针 | Primary: 链头块号 \| Backup: 后继块号，PTR_INVALID=链尾 |
| `backup_count` | — | 非指针。备份总数 |

---

### 区段树节点（`Bptree_extent_leaf_node_t` / `Bptree_extent_inner_node_t`，337 阶）

| 字段 | 指针类型 | 说明 |
|------|---------|------|
| `meta.parent_ptr.ptr` | 联合指针 | 根 → Inode 指针（自身 inode / `.`）；非根 → 块指针（父节点块号） |
| `meta.ptrs[1]` | 块指针 | 非根叶子：前驱兄弟块号；`ptrs[2]` = 后继兄弟块号 |
| `meta.ptrs[1]` | Inode 指针 | 根节点（叶/内部）：`..` inode 编号（由 readdir 合成，不在树内） |
| `extents[i].block_base` | 块指针 | 叶子：extent 物理起始 FS_block（`free_to_use` 预留） |
| `extents[i].sub_node_ptr` | 块指针 | 内部：子节点块号 |

非根内部节点 `ptrs[3]` 全预留——上界路由下 N hints = N children，无最左子树（v1.1 §4.1）。

---

### Dentry 树节点（`Bptree_dentry_leaf_node_t` / `Bptree_dentry_inner_node_t`，126 阶）

| 字段 | 指针类型 | 说明 |
|------|---------|------|
| `meta.parent_ptr.ptr` | 联合指针 | 根 → Inode 指针（自身 inode / `.`）；非根 → 块指针（父节点块号） |
| `meta.ptrs[1]` | 块指针 | 非根叶子：前驱兄弟块号；`ptrs[2]` = 后继兄弟块号 |
| `meta.ptrs[1]` | Inode 指针 | 根节点（叶/内部）：`..` inode 编号（由 readdir 合成，不在树内） |
| `entries[i].inode_ptr` | 联合指针 | 叶子 → Inode 指针（目标 inode 编号）；`free_to_use` = name_len |
| `hints[i].sub_ptr` | 块指针 | 内部 → 子节点块号 |
| `entries[i].full_char_remain_ptr` | 物理定位 | name_len>40：块号[12:63] \| 块内偏移[0:11]，指向 name[40..]（名字堆分配，块号部分 ∈ FS_block 世界）；否则 PTR_INVALID |

---

### Inode（256B，尺寸继承 v1）

| 字段 | 指针类型 | 说明 |
|------|---------|------|
| 首 64 位 | — | `raven_Inode_t`（REGULAR/SOFT_SYM）：`type:2 | is_data_root_direct_bit:1 | other:61`；`raven_dir_Inode_t`（DIR）：`type:2 | other:62`（无 direct bit，见 `disk_layout_v1.1.md` §3.1） |
| `data_root` | 联合指针（块指针，带 PTR_INVALID 哨兵） | REGULAR/SOFT_SYM：PTR_INVALID=内联（内容在 inline_data[0..size-1]）；≠INVALID 按 `is_data_root_direct_bit` 解码：=1 直指区间起始（连续 FS_block）；=0 区段 B+tree 根。DIR：恒 = dentry B+tree 根块号（无内联/直指目录，PTR_INVALID 非法） |
| `inline_data` | — | 按 type 分型：REGULAR/SOFT_SYM=文件内联数据；DIR=10 个 `name_heap_controller_t`（名字堆描述符，固定数组，见 `disk_layout_v1.1.md` §7.4） |

---

## 6. Bitmap — FS_block 世界的分配图

Bitmap 是 FS_block 世界**唯一的、完整的**全局分配追踪结构。

### 6.1 核心约束

**Bitmap 自身必须在 FS_block 世界内，且必须自描述。**

即：

- Bitmap 占用的 FS_block 区间 ∈ `[0, total_blocks)`
- Bitmap 覆盖的 FS_block 区间 = `[0, total_blocks)`
- Bitmap 自身占用的 FS_block，其 bit 已置为 1

这是 bitmap 的**位置依据是一切**——不必在 superblock 之外单独记录 bitmap 的物理位置，因为它在 FS_block 世界里本身就是亮灯的，和其他 allocated 块没有区别。

```
bitmap_blocks = ceil(total_blocks / (4096 × 8))    // 运行时推导，不落盘
bitmap_start  = total_blocks - bitmap_blocks        // 运行时推导，不落盘
bitmap 占用的 FS_block 区间:
  [bitmap_start, total_blocks)                      // 尾巴连续
  → 此区间内的每个 bit 在 mkfs 时已置为 1

bitmap 覆盖的 FS_block 区间:
  [0, total_blocks)
  → bit[i] 对应 FS_block[i]
```

### 6.2 bit 语义

```
bit[i] = 1 → FS_block[i] 已分配（有主）
bit[i] = 0 → FS_block[i] 空闲（可分配）
```

### 6.3 谁在该亮灯

以下 FS_block 在 mkfs 后必须亮灯，运营期间一直亮着：

| 占用者 | FS_block 区间 | 亮灯时机 |
|--------|--------------|----------|
| Bitmap 自身 | `[bitmap_start, total_blocks)` | mkfs |
| Inode array | `[inode_array_base, bitmap_start)` | mkfs |
| Inode bitmap | `[inode_array_bitmap_base, inode_array_base)` | mkfs |
| 根目录 dentry B+tree 根节点 | 2 FS_block（8K 节点 = 2 连续块，v1.1 §4） | mkfs |
| 根 inode | 不占 FS_block（inode array 内） | mkfs |
| **Backup Superblocks** | **N 个随机 FS_block** | **mkfs** |
| 文件/目录数据 | 按需分配 | 运行时 |
| 名字堆位图/数据块（每目录） | 按需分配（位图面+数据面各自连续、起点 8K 对齐，v1.1 §7.4） | 运行时（目录首次出现落入该档的名字时） |

**无隐藏预留**：FS_block 世界中不存在在 bitmap 中不亮灯却被占用的块。

### 6.4 Bitmap 与 Superblock 的关系

**Primary Superblock** 不在 FS_block 世界内（见 §4.1），bitmap 不对其负责。

**Backup Superblock** 在 FS_block 世界内，bitmap 正常管理——
mkfs 时其占用的 FS_block 亮灯，运营期间常亮。

### 6.5 自描述循环的破局

Bitmap 自描述看似循环依赖（bitmap 在 FS_block 世界里，自己描述自己），破局方式是：**mkfs 先算位置，再写 bitmap，最后标记自身为 allocated**。

```
mkfs 流程:
  1. 确定 total_blocks, backup_count
  2. 推导 bitmap_blocks = ceil(total_blocks / 32768)     // 4096×8
  3. 推导 bitmap_start = total_blocks - bitmap_blocks    // 尾巴固定
  4. 计算 inode 元数据位置
  5. 随机选取 N 个 FS_block 作为 Backup Superblock 散布位置
  6. 写满 bitmap 区域（全零 → 再置位自身、inode、Backup 的 bits）
  7. 写 Backup Superblocks（先清空，再写入全同副本）
  8. 写 Primary Superblock（含 backup_next, backup_count）
  9. 后续运营，bitmap 正常管理
```

注意顺序：先写 Backup 再写 Primary——
即使写 Primary 时掉电，Backup 在 FS_block 世界里已亮灯 + 已写入，
恢复工具可以扫描魔数找到它们。

运营期间 bitmap 加载到内存后，其自身占用的 bits 保持 1 不变，不参与分配回收。

### 6.6 Bitmap 的归属校验

任何 mount 操作必须验证 bitmap 自描述一致性：

```
1. 读 superblock → 取 total_blocks
2. 推导 bitmap_blocks = ceil(total_blocks / 32768)
3. 推导 bitmap_start  = total_blocks - bitmap_blocks
4. 读 bitmap 占据的 FS_block 区间 [bitmap_start, total_blocks)
5. 校验: bit[bitmap_start] ... bit[total_blocks - 1] 全部为 1
6. 校验: inode 元数据区域的 bits 全部为 1
7. 若校验失败 → 拒绝挂载
```

### 6.7 与 disk_layout 文档的关系

`disk_layout_v1.1.md` §0 声明：坐标系与块模型（FS_block 世界、Primary/Backup Superblock、
bitmap 尾巴布局、inode array/inode bitmap 推导）**全部继承 v1，无变化**，以本文件为契约。
v1 文档（`disk_layout_v1.md` §3）中 "bitmap 自身占用的 block 在 mkfs 时预先标记为已分配，
运营期间不进入自管理。无自指问题"的描述，在 block model 世界观下重新表述为：

- Bitmap 在 FS_block 世界内，不是局外人
- 自描述不是问题，是规则——mkfs 破循环（§6.5）
- "运营期间不进入自管理"仍然成立：bitmap 自身 bits 保持 1，读写都跳过

---

## 7. 统一挂载验证

RavenFS 统一块设备模型下，挂载验证只有一条路径——不论整盘、分区还是文件：

```
设备: 任意块设备或文件，LBA_size = S

读 fd byte[0, 4096) → Primary Superblock
校验 magic = "RAVN"

读 world_base_lba   ← 从 Superblock
读 total_blocks     ← 从 Superblock

验证 world_base_lba × S == 4096          ← Primary 占据 byte[0, 4096)，FS_block[0] 从 byte 4096 开始
验证 total_blocks > 0

multiplier = 4096 / S                    ← 运行时推导

FS_block 世界占据 fd byte[4096, 4096 + total_blocks × 4096)，Primary 在其前，无重叠

Backup 校验:
  从 Primary.backup_next 遍历链表:
     每个 Backup 校验 magic + crc32 + backup_count == Primary.backup_count
  遍历计数 == Primary.backup_count
```

### 迁移保护

```
同一块设备在不同机器上：
  LBA_size 不变 → world_base_lba 不需更新

手动 dd 到另一分区 / 文件 image：
  world_base_lba 不变（恒 = multiplier = 4096 / LBA_size）
  Backup 的 world_base_lba 也同样不变
  → v1/v1.1 均不做在线迁移（v1.1 继承 v1），仅通过离线扩容工具修改备份计数
```

---

## 8. 示例换算

### 场景：读 B+tree 叶子节点的 extent

```
B+tree 叶子 entry 记录（v1.1 extent_t，指针标注见 §5）:
  block_base.ptr = 1024  ← FS_block 号（物理起点）
  length = 3             ← 闭区间，覆盖 4 个 FS_block
  fblkbase = 0           ← 文件逻辑块起点（本例不参与换算）

设备 LBA_size = 512
multiplier = 8
world_base_lba = 8   (= multiplier)

换算:
  FS_block[1024] 的 fd 字节偏移:
    4096 + 1024 × 4096 = 4198400
    对应 LBA: 4198400 / 512 = 8200

  FS_block[1024..1027] (4块) 的 fd 字节区间:
    [4198400, 4198400 + 4 × 4096) = [4198400, 4214784)
```

### 场景：读 inode array

Bitmap 固定在 FS_block 世界尾巴，Inode array 紧挨 Bitmap 向左：

```
bitmap_start  = total_blocks - bitmap_blocks            // = 100000 - 4 = 99996
inode_array_base = bitmap_start - inode_blocks           // = 99996 - 16 = 99980

假设 total_blocks = 100000, bitmap_blocks = 4, inode_blocks = 16
bitmap_start  = 99996
inode_array_base = 99980  ← FS_block 号

bitmap 区间:  [99996, 100000)
inode 区间:   [99980, 99996)

fd 字节偏移:
  start_byte = 4096 + 99980 × 4096 = 4096 + 409518080 = 409522176
  end_byte   = 409522176 + 16 × 4096 - 1 = 409587711
```

### 场景：读 B+tree 节点（v1.1 恒 8K = 2 FS_block）

四种节点统一 8K：读节点 = 读 2 个连续 FS_block（read_buff(blk, blk_len=2)），
块号字段一律指向首块。节点首块 FS_block[5000] → fd 字节区间:

```
4096 + 5000 × 4096 = 20496384       ← 节点首块字节偏移
[20496384, 20496384 + 8192)          ← 8K 节点区间
```

---

## 9. 与 disk_layout 文档的关系

- `disk_layout_v1.1.md` 中所有 "block" 均指 **FS_block**（除非特别标注 "LBA"）
- 分区布局图中 `block 0 = Superblock` 是物理视图，不是 FS_block 世界内的编号
- v1.1 继承 v1 的坐标系与块模型（disk_layout_v1.1.md §0），本文即其底层契约
- 扩容策略中的块号均为 FS_block 号，扩容后 `world_base_lba` 不变（恒 = multiplier），只更新 `total_blocks`

---

> **一句话记**
>
> Primary 是世界的创造者，**不在**世界内。Backup 是世界的备胎居民。
> FS_block[n] byte offset = `4096 + n × 4096`，仅此一条公式贯穿全局。

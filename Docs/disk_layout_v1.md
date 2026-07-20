# RavenFS — 磁盘布局 v1

> 2026-06-25 推倒 v0 草案从头设计。
> 基于 B+tree 的文件系统，NVMe 驱动验收场景驱动。
> 设计哲学：跑起来第一，用户自己规划上限。

---

## 1. 分区布局

```
物理视图（设备 LBA 空间）:
  partition_offset_0:  Primary Superblock ← 不在 FS_block 世界内

FS_block 世界视图（4096B blocks）:
  block 0..:            Data Blocks / Backup Superblocks ← 随机散落
  block 1..:            Data Blocks               ← 向前增长
                        [空闲空间 / 未来扩容]
  block T-B-I-IB..T-B-I-1:  Inode Bitmap (IB 块)     ← 从尾巴连续分配
  block T-B-I..T-B-1:       Inode Array (I 块)        ← 从尾巴连续分配
  block T-B..T-1:           Bitmap (B 块)              ← 固定在尾巴

T = total_blocks
B = ceil(T / (4096 × 8))          ← 运行时推导，不落盘
I  = ceil(inode_count × 256 / 4096)
IB = ceil(inode_count / (4096 × 8))
```

元数据从分区**尾巴**开始连续分配，自右向左：**Bitmap → Inode Array → Inode Bitmap**。
数据区从 block 1 向前增长，中间留空作为扩容余量。

Backup Superblock 在 FS_block 世界内随机散落，被 bitmap 标记为 allocated，通过双向链表连接。详见 §2.1。

### 扩容策略

增加分区大小 Δ 块（T′ = T + Δ）：

```
旧: inode [T-B-I, T-B), bitmap [T-B, T)
新: inode [T′-B-I, T′-B), bitmap [T′-B, T′)
```

1. 读旧 bitmap + 旧 inode array
2. 写到新尾巴位置
3. 新 bitmap 中：标记新元数据块为 allocated，旧位置释放为 free
4. 更新 superblock.total_blocks = T′（Backup 的 total_blocks 也需一并更新）
5. 数据块原地不动

最怂但最稳妥——只搬元数据，不动数据。

**注意：** Bitmap 固定在尾巴，扩容后新 bitmap_start = T′ - B 自动重新推导，不需额外计算。

---

## 2. Primary Superblock（上岗）

Primary Superblock 是 RavenFS 的唯一入口和定义者。

**位置：**
- 裸盘：LBA[0] offset=0
- GPT 分区：`gpt_entry.starting_lba` offset=0

**FS_block 世界关系：** 不在 FS_block 世界内。bitmap 不管理它。

### 结构

```c
struct RavenSuperblock {
    uint64_t  magic;                // "RAVN" = 0x5241564E
    uint32_t  version;              // 格式版本号
    uint32_t  pad0;                 // 对齐，替代 LBA_size（NVMe Identify 已知，不落盘）
    uint64_t  world_base_lba;       // FS_block[0] 在设备 LBA 空间的起始 LBA
    uint64_t  total_blocks;         // FS_block 世界总块数

    uint64_t  inode_array_count;         // Inode 上限（推导出 inode_blocks, inode_bitmap_blocks）
    uint64_t  root_inode;                // 根目录 inode 索引
    uint8_t   uuid[16];
    uint8_t   volume_name[48];

    uint32_t  crc32;

    /* ── 备份链表 ── */
    uint64_t  backup_prev;          // Primary: 0（哨兵）
    uint64_t  backup_next;          // Primary: 链表头（第一个 Backup FS_block 号）
    uint32_t  backup_count;         // 备份总数

    uint8_t   pad[3960];            // 删了 bitmap_start，调整此值使 sizeof == 4096
} __attribute__((packed));
```

### Primary 角色下的字段语义

| 字段 | 值 | 说明 |
|------|----|------|
| `backup_prev` | 0 | 哨兵，Primary 无前驱 |
| `backup_next` | FS_block# | 第一个 Backup Superblock 的块号 |
| `backup_count` | N | 备份总数。mount 时遍历链表计数校验 |

关键字段由用户 mkfs 时指定：
```
mkfs.ravenfs /dev/nvme0n1 --inodes=65536 --inode-ratio=16 --backups=4
```

`--backups=N` 控制 mkfs 时散布的 Backup Superblock 数量。默认建议 3。

---

## 2.1 Backup Superblock（备胎）

Backup Superblock 是 Primary 的完整冗余副本，散落在 FS_block 世界内用于容灾。

**位置：** FS_block 世界内，mkfs 时由随机种子选择散布位置，
bitmap 标记为 allocated。

**内容与 Primary 全同：** `magic` ~ `crc32` 所有字段值一致。
仅 `backup_prev` / `backup_next` 的角色语义不同。

### Backup 角色下的字段语义

| 字段 | 值 | 说明 |
|------|----|------|
| `backup_prev` | FS_block# / 0 | 前驱 Backup 块号（0=链头，即 Primary 后的第一个） |
| `backup_next` | FS_block# / 0 | 后继 Backup 块号（0=链尾） |
| `backup_count` | N | 与 Primary 交叉验证。挂载时遍历链表，计数必须一致 |

### 链表拓扑

```
Primary (不在世界内)
  └─ backup_next → Backup#0 (FS_block[P0])
                      ├─ backup_prev = 0  (链头，前驱为 Primary)
                      ├─ backup_next → Backup#1 (FS_block[P1])
                      └─ backup_count = 3
                  Backup#1 (FS_block[P1])
                      ├─ backup_prev = P0
                      ├─ backup_next → Backup#2 (FS_block[P2])
                      └─ backup_count = 3
                  Backup#2 (FS_block[P2])
                      ├─ backup_prev = P1
                      ├─ backup_next = 0  (链尾)
                      └─ backup_count = 3
```

链头（Backup#0）的 `backup_prev = 0`。
链尾（Backup#last）的 `backup_next = 0`。

### 挂载校验

```
1. 读 Primary（partition offset 0）
2. 遍历 backup_next 链表：
   a. 对每个节点校验 magic + crc32
   b. 校验 backup_count == Primary.backup_count
   c. 校验 common 字段（total_blocks, inode_array_count, ...）== Primary
3. 若 Primary 校验失败 → 尝试扫描各 Backup 选举主用
   （v1 暂不实现自动选举，人工工具辅助）
```

### mkfs 散布策略

Backup Superblock 散布位置由 `--backups=N` 指定，
mkfs 在 `[0, total_blocks)` 范围内均匀/随机选取 N 个 FS_block。
mkfs 后立即被 bitmap 标记为 allocated，运营期间保持亮灯。

---

## 3. Bitmap（FS_block 分配图）

### 3.1 位置

Bitmap 固定在 FS_block 世界**最末尾**连续分配，区间由 `total_blocks` 唯一推导（见 block_model.md §3.2）：

```
bitmap_blocks = ceil(total_blocks / 32768)      ← 不落盘，运行时推导
bitmap_start  = total_blocks - bitmap_blocks     ← 不落盘，运行时推导
bitmap 区间:  FS_block[bitmap_start, total_blocks)
```

### 3.2 内部布局

Bitmap 是**连续的 bit 数组**，从区间首 FS_block 的 byte offset 0 开始排列。

```
FS_block[bitmap_start]:
  byte[0]      byte[1]      ...  byte[4095]
  [b7 b6..b0]  [b7 b6..b0]  ...  [b7 b6..b0]
    ↑                          ↑
  bit[0]                    bit[32767]

FS_block[bitmap_start + 1]:
  bit[32768] ... bit[65535]

...

FS_block[total_blocks - 1] (最后一个 bitmap block):
  bit[(B-1)×32768] ... bit[B×32768 - 1]
```

### 3.3 bit 语义

```
bit[i] = 0  → FS_block[i] 空闲（可分配）
bit[i] = 1  → FS_block[i] 已分配（有主）
```

即 bit 编号 = FS_block 编号，一一对应。

### 3.4 尾巴填充

Bitmap 总位数 = `bitmap_blocks × 32768`，但 FS_block 世界只有 `total_blocks` 个块。
超出部分（`bitmap_blocks × 32768 - total_blocks` 位）**必须全为 1**：

```
tail_bits = bitmap_blocks × 32768 - total_blocks   // 尾部冗余位数
条件: 1 ≤ tail_bits < 32768                        // 最多差一个 FS_block
要求: bit[total_blocks] ... bit[bitmap_blocks × 32768 - 1] = 全部 1
```

原因：这些 bit 不对应任何实际 FS_block，永远不会被分配。
置为 1 使分配器不会误认为它们是空闲块。

### 3.5 自描述规则

Bitmap 自身占用的 FS_block 区间 `[bitmap_start, total_blocks)` 在 mkfs 时亮灯（全部置 1），
运营期间保持常亮，不参与分配回收。

挂载校验（见 block_model.md §6.6）：

```
1. 推导 bitmap_blocks, bitmap_start
2. 校验 bitmap 区间内所有 bit 非零
3. 校验 tail_bits 全部为 1
4. 校验 inode 元数据区域的 bits 全部为 1
```

---

## 4. Inode 元数据

### 4.1 Inode 结构

每个 inode 256B 固定，v1 不支持其他尺寸。

#### 类型常量

```c
constexpr uint8_t INODE_TYPE_REGULAR = 0;   // 普通文件
constexpr uint8_t INODE_TYPE_DIR     = 1;   // 目录
constexpr uint8_t INODE_TYPE_SOFT_SYM = 2;  // 软链接
```

`type` 2-bit 字段编码，存储于 inode 首 64-bit 的 bits[1:0]。

#### 结构体定义

```c
struct raven_Inode_t {
    uint64_t  type:2;                  // INODE_TYPE_REGULAR/DIR/SOFT_LINK
    uint64_t  is_data_root_direct_bit:1; // 1=direct extent, 0=B+tree root (dir always 0)
    uint64_t  other:61;                // 预留
    uint32_t  uid;
    uint32_t  gid;
    uint32_t  m_ns_stamp;            // mtime 纳秒余数
    uint32_t  c_ns_stamp;            // ctime 纳秒余数
    uint32_t  a_ns_stamp;            // atime 纳秒余数
    uint64_t  permission_bits;       // 编码后权限位
    uint64_t  size;
    uint64_t  mtime;                  // seconds
    uint64_t  ctime;                  // seconds
    uint64_t  atime;                  // seconds
    uint64_t  data_root;             // 0 = inline, else FS_block#
    uint32_t  link_count;
    uint64_t  padding[2];            // 元数据扩展预留
    uint8_t   inline_data[160];
} __attribute__((packed));
static_assert(sizeof(raven_Inode_t) == 256, "");
```

#### 位域布局

```
byte[0..7] = { type:2 | is_data_root_direct_bit:1 | other:61 }
              bits[1:0] = type
              bits[2]   = is_data_root_direct_bit
              bits[63:3] = 预留
```

### data_root 三态语义

| `size` | `data_root` | `is_data_root_direct_bit` | 含义 | I/O 次数 |
|---------|-------------|---------------------------|------|---------|
| 0~160 | = 0 | don't care | **内联**，内容在 inline_data[] | 0 |
| — | ≠ 0 | = 1 | **直指 extent**：data_root → 单个数据块 | 1 |
| — | ≠ 0 | = 0 | **B+tree**：data_root → btree 根节点 | ≥ 2 |

前两行由 size 判定。`data_root ≠ 0` 后用 `is_data_root_direct_bit` 区分 direct / B+tree。

**各类型默认行为：**

| 类型 | is_data_root_direct_bit | 说明 |
|------|------------------------|------|
| `INODE_TYPE_REGULAR` | 0 / 1 | size ≤ 4K 可直指；跨 8K 翻转为 0，创建 B+tree |
| `INODE_TYPE_DIR` | 始终 0 | 目录永远走 dentry B+tree |
| `INODE_TYPE_SOFT_LINK` | 0 / 1 | 同 regular，target 路径超过 inline 时分配数据 |

### 4.2 Inode 数组

Inode 数组是**连续的 inode 记录序列**，紧挨 Bitmap 向左排列。

```
inode_blocks = ceil(inode_array_count × 256 / 4096)    ← 不落盘

inode 区间:  FS_block[inode_array_base, bitmap_start)

内部布局（从区间首 FS_block 的 byte offset 0 开始连续排列）:
  byte[0..255]     → inode[0]
  byte[256..511]   → inode[1]
  ...
  byte[(i×256)..(i×256+255)] → inode[i]

inode_index 即 inode 编号，0 ≤ i < inode_array_count。
```

inode 数组之后（`inode_array_base` 到 `bitmap_start` 之间）不跨块外填充：
数组尾部可能在一个 FS_block 内有未用空间（`FS_block_bytes - inode_array_count × 256 % FS_block_bytes` 字节），
**这些未用字节全零**，写入时保留即可。

所有位置由 `total_blocks` + `inode_array_count` 链式推导（见 block_model.md §3.2），
离线扩容工具见 §1。

### 4.3 Inode Bitmap（inode 分配图）

Inode Bitmap 跟踪每个 inode 的分配状态，紧挨 Inode 数组向左排列。

```
inode_bitmap_blocks = ceil(inode_array_count / 32768)   ← 不落盘
inode_bitmap_base   = inode_array_base - inode_bitmap_blocks  ← 不落盘

Inode bitmap 区间:  FS_block[inode_bitmap_base, inode_array_base)
```

#### 内部布局

与 FS_block Bitmap（§3）同结构——连续的 bit 数组，从区间首 FS_block 的 byte offset 0 开始：

```
FS_block[inode_bitmap_base]:
  byte[0]      byte[1]      ...  byte[4095]
  [b7 b6..b0]  [b7 b6..b0]  ...  [b7 b6..b0]
    ↑                          ↑
  bit[0]                    bit[32767]

FS_block[inode_bitmap_base + 1]:
  bit[32768] ... bit[65535]

...

FS_block[inode_array_base - 1] (最后一个 inode bitmap block):
  bit[(IB-1)×32768] ... bit[IB×32768 - 1]
```

#### bit 语义

```
bit[j] = 0  → inode[j] 空闲（可分配）
bit[j] = 1  → inode[j] 已分配（有主）
```

bit 编号 = inode 编号，一一对应。

#### 尾巴填充

Inode bitmap 总位数 = `inode_bitmap_blocks × 32768`，
但 inode 只有 `inode_array_count` 个。超出部分**必须全为 1**：

```
tail_bits = inode_bitmap_blocks × 32768 - inode_array_count
条件: 1 ≤ tail_bits < 32768
要求: bit[inode_array_count] ... bit[IB × 32768 - 1] = 全部 1
```

原因同 §3.4：这些 bit 不对应任何实际 inode，置 1 防止被误分配。

---

## 5. B+tree 节点（8K = 2 × 4K pages）

### 内存结构

```c
struct btree_node_entry_t {
    uint64_t words[4];               // 32B
    // getter/setter 封装语义
};

struct Bptree_node_t {
    btree_node_entry_t entries[256]; // 256 × 32B = 8K
};
```

### Entry 位域（通用）

```
words[0]  bits [63]   = VALID
          bits [62:52] = type
          bits [51:0]  = key

words[1]  bits [63:52] = flags
          bits [51:0]  = value (叶子: len=末端偏移, 闭区间; 内部: unused)

words[2]  bits [63:52] = addr flags (含 ADDR_BTREE)
          bits [51:0]  = addr

words[3]  自由使用，节点类型特定含义见下文
```

32B 裸结构（`words[0..3]`）本身无类型语义——**同一个 `btree_node_entry_t` 按节点类型解码为两个不同的视图**，由 `node_meta_t.is_internal_node` 决定。

---

### f_blkitv 区间比较器具

```c
/* fblk_cmp: f_blkitv 严格比较器具
 *   1  = left 完全在 right 之前
 *   -1 = right 完全在 left 之前
 *   0  = 重叠 (invariant 违反)
 *
 * 区间序：I < J  ⇔  I.R + 1 ≤ J.L
 * 即 fblkbase_i + len_i + 1 ≤ fblkbase_{i+1}
 * 保证无重叠、无逆序 */
int inline fblk_cmp(f_blkitv left, f_blkitv right) {
    if (left.fblkbase + left.len < right.fblkbase) return 1;
    if (right.fblkbase + right.len < left.fblkbase) return -1;
    return 0;
}

/* 区间右端点 fblkbase + len */
uint64_t inline f_blkitv_end(f_blkitv itv) {
    return itv.fblkbase + itv.len;
}
```

叶子 invariant：同一节点内相邻 extent 满足 `fblk_cmp(entries[i], entries[i+1]) == 1`。

---

### 节点元数据 (entry[0].words[3])

```c
union node_meta_t {
    struct {
        uint64_t entry_count_minus1 : 8;   /* 有效 entry 数 - 1 */
        uint64_t is_internal_node   : 1;   /* 0=leaf, 1=internal */
        uint64_t is_root_node       : 1;   /* 1=根节点 */
        uint64_t reserved           : 2;
        uint64_t parent_ptr         : 52;  /* 非根→父节点块号 | 根→所属 inode 编号 */
    } fields;
    uint64_t raw;
} __attribute__((packed));
```

仅 entry[0].words[3] 做 node meta。其他 entry 的 words[3] 含义见各节点类型。

---

### 两个解码视角

`btree_node_entry_t.words[0..3]` 按 `is_internal_node` 解码：

#### 叶子节点解码

| Field | words[0] | words[1] | words[2] | words[3] |
|-------|---------|---------|---------|---------|
| 语义  | key = fblkbase (区间左端) | len (末端偏移, 闭区间) | pblkbase | 附加元数据 |

words[3] 分配：
- entry[0] → `node_meta_t`（entry_count_minus1, is_internal_node, is_root_node, parent_ptr）
- entry[1] → `prev_sibling`（同级链表前驱）
- entry[2] → `next_sibling`（同级链表后驱）
- entry[3..6] → `magic1..magic4`（256-bit 扫描签名）
- entries[7..] → 自由使用

布局示意：
```
entry[0]: { key_0  |  len_0  |  pblkbase_0  |  node_meta_t     }
entry[1]: { key_1  |  len_1  |  pblkbase_1  |  prev_sibling    }
entry[2]: { key_2  |  len_2  |  pblkbase_2  |  next_sibling    }
entry[3]: { key_3  |  len_3  |  pblkbase_3  |  magic1          }
entry[4]: { key_4  |  len_4  |  pblkbase_4  |  magic2          }
entry[5]: { key_5  |  len_5  |  pblkbase_5  |  magic3          }
entry[6]: { key_6  |  len_6  |  pblkbase_6  |  magic4          }
entry[7]: { key_7  |  len_7  |  pblkbase_7  |  free            }
...
```

最大 256 个 extent (entries[0..255] 的 words[0..2] 都存 extent)。
entries[3..6] 的 words[3] 被签名占用，但其 words[0..2] 仍可存 extent —— 不影响有效 entry 容量。

#### 内部节点解码

| Field | words[0] | words[1] | words[2] | words[3] |
|-------|---------|---------|---------|---------|
| 语义  | key = max_R_subtree (子树最大右端点=fblkbase+len) | 未使用 | subptr | 自由 |

内部节点是 256 阶 B+tree（方案 A）：所有 entry 统一，key = max_R_subtree。

**key 语义**：`entry[i].key = max_R(child_i)`，即该子树中所有 extent 的最大右端点 `max(fblkbase+len)`。路由判定 `target ≤ max_R_subtree` 保证任何落入该子树覆盖范围的逻辑块号都能正确路由。

```
entry[0]: { R_0      |  —  |  child_0       |  node_meta_t    }
          ← R_0 = max_R(child_0)
entry[1]: { R_1      |  —  |  child_1       |  free           }
          ← R_1 = max_R(child_1)
entry[2]: { R_2      |  —  |  child_2       |  free           }
          ← R_2 = max_R(child_2)
entry[3]: { R_3      |  —  |  child_3       |  magic1         }
          ← R_3 = max_R(child_3)
entry[4]: { R_4      |  —  |  child_4       |  magic2         }
          ← R_4 = max_R(child_4)
entry[5]: { R_5      |  —  |  child_5       |  magic3         }
          ← R_5 = max_R(child_5)
entry[6]: { R_6      |  —  |  child_6       |  magic4         }
          ← R_6 = max_R(child_6)
...
entry[n]: { R_n      |  —  |  child_n       |  free           }
          ← R_n = max_R(child_n)
```

entry[0].words[3] 存 node_meta_t，entry[3..6].words[3] 存 256-bit 扫描签名，其余自由。

查找：`target ≤ entry[i].key (= max_R(child_i))` 即命中 child_i 区间。

```cpp
for (i = 0; i < get_entry_count(); i++) {
    if (target <= entries[i].get_key())
        return entries[i].get_subptr();   // → child_i
}
return 0;  // 不应到达
```

最大 256 child + 256 key（每个 entry 都带 key）。

---

### 同级链表

所有 B+tree 节点（叶子和内部均可用，但 v1 仅在叶子层使用）通过 `entry[1].words[3]`（prev）和 `entry[2].words[3]`（next）连接为双向链表。

范围遍历无需回溯父节点：先沿链表找到起始位置再顺序向前扫描。

---

## 6. Dentry B+tree

目录项专用 B+tree，与文件区段 B+tree（`Bptree_node_t`）使用不同的 entry 格式。
所有 inode 类型为 `RAVEN_DIR` 的数据内容由此树组织。

### 6.1 节点布局

```
Dentry_Bptree_node_t (8K = 8192B)
+────────────────────────────────────────────┐
│ Dentry_head_t head                   128B  │
+────────────────────────────────────────────┤
│ Dentry_common_t entries[0]           128B  │
│ Dentry_common_t entries[1]           128B  │
│ ...                                        │
│ Dentry_common_t entries[62]          128B  │
+────────────────────────────────────────────┤
│ ← 63 × 128 = 8064B                         │
+────────────────────────────────────────────┘
```

最大 entry 数：**63**（根节点最少 1 个，非根节点分裂时 ≥ 31）。

```c
struct Dentry_head_t {
    dentry_node_meta_t  head;        // 8B: 节点元数据
    uint64_t            ptr1;        // 8B: 内部→最左子树 | 叶子→前驱
    uint64_t            ptr2;        // 8B: 内部→空闲     | 叶子→后继
    uint64_t            reserved0;   // 8B: 预留
    uint64_t            magic1;      // 8B: 256-bit 扫描签名
    uint64_t            magic2;      // 8B: 256-bit 扫描签名
    uint64_t            magic3;      // 8B: 256-bit 扫描签名
    uint64_t            magic4;      // 8B: 256-bit 扫描签名
    uint64_t            reserved2[8]; // 64B: 扩展预留
};

union dentry_node_meta_t {
    struct {
        uint64_t entry_count    : 6;  // 有效 entry 数（0~63，根节点最少 1）
        uint64_t reserved0      : 2;
        uint64_t is_internal_node : 1; // 0=leaf, 1=internal
        uint64_t is_root_node   : 1;  // 1=根节点
        uint64_t reserved       : 2;
        uint64_t parent_ptr     : 52; // 非根→父节点块号 | 根→所属 inode 编号
    } fields;
    uint64_t raw;
};

struct Dentry_common_t {
    uint64_t ptr;        // 8B: 叶子→inode编号 | 内部→child块号
    char     name[120];  // 120B: 文件名（字典序 key）
};
```

### 6.2 双解码视角

同一 `Dentry_common_t` 按 `head.is_internal_node` 解码：

**叶子：**

```
entries[i].ptr   = target inode 编号
entries[i].name  = 文件名（排序 key）
ptr1 → 前驱叶子块号（同级链表左）
ptr2 → 后继叶子块号（同级链表右）
```

叶子节点 entry 按文件名字典序排列，满足 `strcmp(name[i], name[i+1]) < 0`。

**内部：**

```
                  ┌─── ptr1 ──→ child_0（最左子树）
entry[0].name  =  child_1 子树最小文件名
entry[0].ptr   →  child_1
entry[1].name  =  child_2 子树最小文件名
entry[1].ptr   →  child_2
...
entry[n].name  =  child_{n+1} 子树最小文件名
entry[n].ptr   →  child_{n+1}
```

共 `entry_count + 1` 个子树（ptr1 + 每个 entry 的 ptr）。

路由规则：

```
target < entry[0].name                  →  ptr1（child_0）
entry[0].name ≤ target < entry[1].name  →  entry[0].ptr（child_1）
...
entry[n].name ≤ target                  →  entry[n].ptr（child_{n+1}）
```

### 6.3 扫描签名

`head.magic1`–`magic4` 共 256-bit（K=4）固定魔数。假阳性分析详见 `Docs/scan_fp_analysis.md`，K=4 时 512GB 盘期望误报 < 2^-256。

魔数值定义见 `include/magics.h`（`DENTRY_MAGIC1`–`DENTRY_MAGIC4`）。

### 6.4 同级链表

叶子节点通过 `ptr1` / `ptr2` 连成双向链表：

```
leaf_A.ptr2 = B
leaf_B.ptr1 = A, leaf_B.ptr2 = C
leaf_C.ptr1 = B, leaf_C.ptr2 = 0 (tail)
```

范围遍历（`readdir`/`ls`）沿链表顺序扫描，无需回溯父节点。

### 6.5 基础操作

**查找：**
1. 从 root inode 读取根节点
2. 内部节点 → `find_child()` → 读 child → 递归
3. 到达叶子后二分/线性查找 `entries[]`

**插入（分裂）：**
1. 下溯到目标叶子
2. 未满（entry_count < 63）：字典序插入
3. 已满：右半 32 entry 分到新节点，最小 name 提升到父节点
4. 父节点满 → 递归分裂 → 根满则创建新根

**删除：** v1 标记 tombstone，不主动合并。

### 6.6 与文件区段 B+tree 的对比

| 维度 | 文件区段 `Bptree_node_t` | Dentry `Dentry_Bptree_node_t` |
|:---|:---|:---|
| Entry 大小 | 32B | 128B |
| 每节点 entry 数 | 256 | 63 |
| Key 类型 | `uint64_t`（fblkbase / max_R） | `char[120]`（字典序） |
| 扫描签名 | entry[3..6].words[3] | head.magic1..magic4 |
| 叶子 ptr | pblkbase | inode 编号 |
| 内部 ptr | sub_block_num | child 块号 |
| 内部最左子树 | entry[0].words[2]（统一格式） | head.ptr1（独立字段） |
| 同级链表 | entry[1/2].words[3] | head.ptr1 / head.ptr2 |
| 排序 | fblk_cmp（区间序） | strcmp（字典序） |
| 查找 | ≤ 右端点 | ≥ 最小文件名 |

### 6.7 文件类型与 Dentry 的关系

| 文件类型 | Dentry 的 inode 指向 | 备注 |
|:---|:---|:---|
| `RAVEN_REG` | 普通文件 inode | 文件数据走 `raven_Inode_t.data_root` |
| `RAVEN_DIR` | 子目录 inode | 子目录自己的 Dentry B+tree root |
| `RAVEN_SYMLNK` | 软链接 inode | target 路径在 `inline_data` 或扩展块 |

Dentry 本身不区分类型——类型信息在目标 inode 的 `flags` 中。

---

## 7. 灾难恢复设计

### 7.1 魔数纪律

两种 B+tree 节点在 on-disk 布局中均引入了 4 × uint64_t（256-bit）固定魔数签名：

| B+tree 类型 | 魔数位置 | 定义 |
|:---|:---|:---|
| `Bptree_node_t`（文件区段） | `entry[3..6].words[3]` | `BPTREE_MAGIC1`–`BPTREE_MAGIC4` |
| `Dentry_Bptree_node_t`（目录项） | `head.magic1`–`magic4` | `DENTRY_MAGIC1`–`DENTRY_MAGIC4` |

**写盘纪律：** 任何一个 B+tree 节点写回磁盘时，必须保证四个魔数字段都写入正确的值。

**内存缓存推荐：** 即使节点加载到内存（buffer cache），也建议保持魔数字段不变。这允许在内存中同样使用魔数校验判断节点类型。

### 7.2 全盘扫描与节点识别

在元数据严重损坏（如 superblock 损毁、inode array 丢失）的场景下，可通过全盘扫描恢复 B+tree 结构。

扫描算法：

```
for each 8K-aligned block in partition:
    read block
    match = count of magic1..magic4 that match
    if match ≥ 3:
        → 该块是 B+tree 节点（高可靠）
        if match == 4:
            → 完整签名，节点+魔数区域均无损坏
        elif match == 3:
            → 一个魔数字段损坏，其余数据可能也受损
        read node_meta_t 获取 entry_count, is_internal, is_root, parent_ptr
        if is_root:
            parent_ptr = owner inode → 定位到文件/目录
        if is_leaf:
            entries[0..n].key/ptr → 恢复 extent 或 dentry
```

### 7.3 按 3/4 容错匹配

| 命中魔数 | 判定 | 含义 |
|:---:|:---|:---|
| 4/4 | ✅ 确信节点 | 魔数区域完整无损 |
| 3/4 | ⚠️ 高概率节点 | 一个魔数字段损坏，其他数据可能也有坏区 |
| 2/4 | ❌ 不确定 | 误报风险过高，丢弃 |
| < 2 | ❌ 非节点 | 不做处理 |

假阳性分析（详见 `Docs/scan_fp_analysis.md`）：

- K=4（全部匹配）：512GB 盘期望误报 ≈ 2^-256，物理不可能级别
- K=3（3/4 容错）：假阳性率 ≈ 4 × 2^-192，512GB 盘期望误报仍 < 2^-165
- **结论：3/4 容错在工程上假阳性仍然为零**

### 7.4 恢复后的索引重建

在扫描汇总所有 B+tree 节点后：

1. 遍历所有根节点（`is_root_node=1`），通过 `parent_ptr` 获取 owner inode
2. 对每个根节点递归向下遍历，重建完整 B+tree 拓扑
3. 汇总所有叶子节点中的 dentry / extent 数据，恢复文件系统目录树
4. 重新计算并重建 bitmap

此流程供下一代容灾工具实现参考，v1 不做自动恢复。

---

## 8. mkfs 参数化

```
mkfs.ravenfs <device> [options]

Options:
  --inode-ratio=N    每 N block 分配 1 个 inode (默认 16)
  --inode-count=N    直接指定 inode 上限
  --label=NAME       卷标 (最长 48B)
```

默认 `--inode-ratio=8` = 1 inode / 32KB。inode 数组空间开销约 0.4%。

`--inode-ratio=4` 用于大量小文件场景（约 0.8% 空间开销）。

---

## 9. 测试方案

```
host:  mkfs.ravenfs /dev/nvme0n1pX   → 确定性盘面布局（元数据尾部）
guest: KVM + VFIO passthrough         → 真实 NVMe 驱动路径
guest: 驱动读盘 → 解析 superblock → 验证布局正确
       → 查目录 → 读文件 → 跨 extent 大文件
       → 检查位图一致性
```

不依赖 mock，不用模拟器，全真 I/O 验证 NVMe 驱动。

扩容测试：mkfs 小分区 → 模拟扩容（离线工具）→ guest 交叉验证元数据搬迁后的正确性。

---

## 10. v1 不做

| 特性 | 原因 |
|------|------|
| 动态 inode 分配 | mkfs 静态分配 + 离线扩容工具 |
| 在线碎片整理 | v1 跑通为先 |
| 事务/日志 | NVMe 验收场景不需要 |
| 在线扩容 | 离线工具补 |



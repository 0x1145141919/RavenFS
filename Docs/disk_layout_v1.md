# RavenFS — 磁盘布局 v1

> 2026-06-25 推倒 v0 草案从头设计。
> 基于 B+tree 的文件系统，NVMe 驱动验收场景驱动。
> 设计哲学：跑起来第一，用户自己规划上限。

---

## 1. 分区布局

```
block 0:              Superblock               ← 固定
block 1..:            Data Blocks               ← 向前增长
                      [空闲空间 / 未来扩容]   
block T-B-I..T-I-1:   Bitmap (B 块)            ← 从尾巴向后分配
block T-I..T-1:       Inode Array (I 块)        ← 从尾巴向后分配

T = total_blocks
B = ceil(T / (4096 × 8))
I = ceil(inode_count × 256 / 4096)
```

元数据（bitmap、inode array）从分区**尾巴**开始向后分配，数据区从 block 1 向前增长。中间留空作为扩容余量。

### 扩容策略

增加分区大小 Δ 块（T′ = T + Δ）：

```
旧: bitmap [T-B-I, T-I), inode [T-I, T)
新: bitmap [T′-B-I, T′-I), inode [T′-I, T′)
```

1. 读旧 bitmap + 旧 inode array
2. 写到新尾巴位置
3. 新 bitmap 中：标记新元数据块为 allocated，旧位置释放为 free
4. 更新 superblock.total_blocks = T′
5. 数据块原地不动

最怂但最稳妥——只搬元数据，不动数据。

---

## 2. Superblock（1 block = 4096B）

```c
struct RavenSuperblock {
    uint64_t  magic;                // "RAVN" = 0x5241564E
    uint32_t  version;              // 格式版本号,[0:7]编码LBA_size到文件系统实际块的乘数
    uint32_t  LBA_size;             // NVMe LBA 尺寸

    uint64_t  total_blocks;         // 分区总块数
    uint64_t  bitmap_start;         // 位图起始块号
    uint64_t  bitmap_blocks;        // 位图占用块数

    uint64_t  inode_array_base;     // inode 数组起始块号
    uint64_t  inode_array_bitmap_base; // inode 位图起始块号
    uint64_t  inode_array_count;    // 用户选定的 inode 上限
    uint64_t  root_inode;           // 根目录 inode 编号
    uint8_t   uuid[16];
    uint8_t   volume_name[48];

    uint32_t  crc32;
    uint8_t   pad[3956];            // 凑满 4096
} __attribute__((packed));
```

关键字段由用户 mkfs 时指定：
```
mkfs.ravenfs /dev/nvme0n1 --inodes=65536 --inode-ratio=16
```

---

## 3. Bitmap

尺寸由 mkfs 计算：
```
bitmap_blocks = ceil(total_blocks / (4096 × 8))
```

每个 bit 代表一个 block：
- 0 = 空闲
- 1 = 已分配

bitmap 自身占用的 block 在 mkfs 时预先标记为已分配，运营期间不进入自管理。无自指问题。

---

## 4. Inode（256B）

```c
struct raven_Inode_t {
    uint64_t  flags;                  // type + tree_height + other flags
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
    uint64_t  data_root;             // 0 = inline, else data address
    uint32_t  link_count;
    uint64_t  padding[2];            // 元数据扩展预留
    uint8_t   inline_data[160];
} __attribute__((packed));
static_assert(sizeof(raven_Inode_t) == 256, "");
```

### data_root 三态语义

| `size` | `data_root` | `flags.tree_height` | 含义 | I/O 次数 |
|---------|-------------|---------------------|------|---------|
| 0~160B | = 0 | 任意 | **内联**，内容在 inline_data[] | 0 |
| 161~8K | ≠ 0 | = 0 | **直指块**：data_root → 单个 8K 数据块 | 1 |
| > 8K | ≠ 0 | ≥ 1 | **B+tree**：data_root → btree 根节点 | ≥ 2 |

树高度 0 的语义：不创建 btree 节点，data_root 直接是文件数据块地址。8K 以内的小文件省去一次 btree 根节点 I/O。

### Inode 数组

mkfs 在分区尾部静态分配连续区域：
```
inode_blocks = ceil(inode_array_count × 256 / 4096)
inode_size   = 256  (fixed in v1)
inode_array_base = total_blocks - bitmap_blocks - inode_blocks
```

离线扩容工具见 §1。

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
        uint64_t entry_count    : 6;  // 有效 entry 数 - 1（最大 63）
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

## 7. mkfs 参数化

```
mkfs.ravenfs <device> [options]

Options:
  --inode-ratio=N    每 N block 分配 1 个 inode (默认 16)
  --inode-count=N    直接指定 inode 上限
  --label=NAME       卷标 (最长 48B)
```

默认 `--inode-ratio=16` = 1 inode / 64KB。inode 数组空间开销约 0.2%。

`--inode-ratio=4` 用于大量小文件场景（约 0.8% 空间开销）。

---

## 8. 测试方案

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

## 9. v1 不做

| 特性 | 原因 |
|------|------|
| 动态 inode 分配 | mkfs 静态分配 + 离线扩容工具 |
| 在线碎片整理 | v1 跑通为先 |
| 事务/日志 | NVMe 验收场景不需要 |
| 在线扩容 | 离线工具补 |



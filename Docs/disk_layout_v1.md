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
    uint32_t  version;              // 格式版本号
    uint32_t  block_size;           // 固定 4096

    uint64_t  total_blocks;         // 分区总块数
    uint64_t  bitmap_start;         // 位图起始块号
    uint64_t  bitmap_blocks;        // 位图占用块数

    uint64_t  inode_array_base;     // inode 数组起始块号
    uint64_t  inode_array_count;    // 用户选定的 inode 上限
    uint16_t  inode_size;           // 256
    uint16_t  reserved0;

    uint32_t  root_inode;           // 根目录 inode 编号
    uint8_t   uuid[16];
    uint8_t   volume_name[48];

    uint32_t  crc32;
    uint8_t   pad[];                // 凑满 4096
};
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
    uint32_t  link_count;
    uint32_t  uid;
    uint32_t  gid;
    uint32_t  m_ns_stamp;            // mtime 纳秒余数
    uint32_t  c_ns_stamp;            // ctime 纳秒余数
    uint32_t  a_ns_stamp;            // atime 纳秒余数
    uint32_t  user_permission_bits;
    uint32_t  group_permission_bits;
    uint32_t  other_permission_bits;
    uint64_t  size;
    uint64_t  mtime;                  // seconds
    uint64_t  ctime;                  // seconds
    uint64_t  atime;                  // seconds
    uint64_t  data_root;             // 0 = inline, else data address
    uint32_t  parent_inode;
    uint32_t  generation;
    uint8_t   inline_data[160];
};
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
        uint64_t entry_count_minus1 : 8;   // 有效 entry 数-1
        uint64_t is_internal_node   : 1;   // 0=leaf, 1=internal
        uint64_t reserved           : 3;
        uint64_t parent_ptr         : 52;  // 父节点(0=root)
    };
    uint64_t raw;
};
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
- entry[0] → `node_meta_t`（entry_count_minus1, is_internal_node, parent_ptr）
- entry[1] → `prev_sibling`（同级链表前驱）
- entry[2] → `next_sibling`（同级链表后驱）
- entries[3..] → 自由使用

布局示意：
```
entry[0]: { key_0  |  len_0  |  pblkbase_0  |  node_meta_t     }
entry[1]: { key_1  |  len_1  |  pblkbase_1  |  prev_sibling    }
entry[2]: { key_2  |  len_2  |  pblkbase_2  |  next_sibling    }
entry[3]: { key_3  |  len_3  |  pblkbase_3  |  free            }
...
```

最大 256 个 extent (entries[0..255] 的 words[0..2] 都存 extent)。

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
...
entry[n]: { R_n      |  —  |  child_n       |  free           }
          ← R_n = max_R(child_n)
```

entry[0].words[3] 存 node_meta_t，其余 entry 的 words[3] 自由。

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

## 6. Dentry（分离路线）

inode 与 dentry 分离。目录的 data B+tree 存储目录项：

```c
struct RavenDentry {
    uint64_t  hash;              // key = filename hash
    uint64_t  inode;             // inode 编号
    uint32_t  group;             // inode 所在块组 (保留)
    uint8_t   name_len;
    uint8_t   file_type;
    uint16_t  _pad;
    char      name[];            // 变长
};
```

目录作为特殊文件，其 `data_root` 指向目录项 B+tree（与文件数据 B+tree 同结构，但 entry 语义不同）。

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

---

## 10. 未实现：In-memory 借用指针缓存优化

### 思路

B+tree 节点加载到私有内存缓存后，B+tree 遍历变成纯内存操作（不需要 hash 表或 radix tree 做 block# → 线性地址的映射），
可以在现有磁盘结构上直接借用空闲字段缓存线性地址指针：

**内部节点（磁盘上 `words[1]` 未使用）：**
```
words[1] 借用为 child 线性地址
         bits [63:13] = 8K 对齐虚拟地址
         bits [12:0]  = 自由标志
刷回前清零 words[1] → 写盘
```

**叶子节点（磁盘上 `entry[3..5].words[3]` 自由）：**
```
entry[3].words[3] → 父节点线性地址
entry[4].words[3] → 左邻居线性地址
entry[5].words[3] → 右邻居线性地址
刷回前清零 → 写盘
```

注意：`entry[1/2].words[3]`（sibling block#）保留磁盘语义不变，不作为内存指针槽位，避免刷回前做 block# ↔ 线性地址转换。

### 落地难点（2026-06-27 记录）

| 难点 | 说明 |
|------|------|
| **缓存生命周期** | 节点加载/驱逐时，借用字段的填充和清理需要统一的 entry/exit 钩子 |
| **双字段同步** | 树结构变更（分裂、合并）时，必须同时更新借用指针（线性地址）和对应磁盘字段（block#），否则刷回后盘面损坏 |
| **写回竞赛** | 刷回前清零借用字段 → 提交 BIO → 等待 IO 完成。清零后到 IO 完成前这段时间，节点数据对并发读者是损坏状态 |
| **异步写回的恢复时机** | 如果采用异步 flush，必须在 IO completion callback 中恢复借用字段。飞行中的节点需要标记 BUSY 防止被读到损坏数据 |
| **多线程一致性** | B+tree 查找/插入/分裂可能是并发的（写时加锁、读时共享）。借用字段跟磁盘字段混在同一个 struct 内，多线程下的数据一致性和可见性语义需要明确 |
| **调试复杂度** | 借用指针和磁盘字段在 struct 中无视觉区分，容易漏清零或错恢复。此类 bug 跟写回时机相关，极难复现 |
| **Linux 移植** | 该设计不能走 Linux 通用 page cache（page cache 假设 page 内容 = 磁盘内容），需要实现私有 buffer cache（类似 XFS xfs_buf / btrfs extent_buffer），直发 block layer BIO |

### 结论

设计已在 `disk_struct.h` 注释中留底。待 v1 跑通基础功能后，作为独立优化主题评估投入产出比。

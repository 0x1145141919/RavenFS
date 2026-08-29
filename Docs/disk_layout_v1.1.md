# RavenFS — 磁盘布局 v1.1

> 2026-08-19 对齐定稿；2026-08-29 修订（dir inode 直指 dentry 树 + 固定 10 堆描述符，据 `disk_struct_v1.1.hpp` 反向维护）。
> v1（`disk_layout_v1.md` + `block_model.md`）的继承与修正。
> 结构定义见 `include/disk_struct_v1.1.hpp`，本文是它的设计意图与不变量契约。

---

## 0. v1 → v1.1 变更动机

v1 的两个结构级教训（详见 `bug_bplus_separator_corruption.md`）：

1. **元数据借用 entry 槽位**：v1 把 node_meta / 兄弟指针 / magic 塞进 entry 的 words[3]，
   名义 256 槽、实际多个槽被挪用 → entry_count 语义要靠纪律维护，
   Bug B（root 满判断失准 → 第 64 个 separator 越界写）本质是"塞满 + 隐式槽"的组合拳。
2. **Dentry 树下界路由**：内部节点存"子树最小文件名"，删除叶子首项必须级联刷父
   separator（衍生 bug 3.3）；分裂全排序破坏 ptr1 不变量（Bug A）。

v1.1 的结构级修正：

| # | 修正 | 效果 |
|---|------|------|
| 1 | 元数据独立成 `Bptree_meta_t`（64B 头） | entry_count 语义纯净，off-by-one 温床灭绝 |
| 2 | 指针类型化 `ptr_in_idx` / `parent_ptr_t` | 消灭裸 u64 + MASK 宏 |
| 3 | Dentry 树 hash 路由 + **上界**（hash_max） | 查找单规则、删最小项无感 |
| 4 | 容量 + 1 瞬时槽（内外统一） | 插入单趟"插→查→裂"，消灭两套满判断 |
| 5 | 长名外置 → 每目录名字堆（位图分配） | 替代散装块内偏移，分配/回收统一 |
| 6 | readdir 按 hash 乱序 | 路径解析高频、ls 低频，接受 |

### 0.1 v1.1 内修订（2026-08-29，据 `disk_struct_v1.1.hpp` 反向维护）

| # | 修订 | 效果 |
|---|------|------|
| 7 | dir 型 inode 独立成 `raven_dir_Inode_t`：`data_root` **直指 dentry B+tree 根节点**（单态，无内联/直指/level 多态） | 消除 dir 侧冗余：`is_data_root_direct_bit` / `dir_inode_name_heap_level` 对目录无意义 |
| 8 | 160B 内联区固定为 **10 个 `name_heap_controller_t`**（5 粒度 × 位图容量档） | 消灭三档 level 升级路径，描述符结构恒定、按堆独立激活 |
| 9 | 名字堆**懒加载懒分配（档位级）**：未激活档位零开销（mkfs 不预留）；激活 = 位图面 + 数据面按位图容量一次分配（各自 block_alloc_hard 连续区间）；某档位位图全部分配干净才激活下一档（2026-08-29 决策，alloc_at 定点认领驳回） | 激活成本 = 该档容量（如 8B_8k 共 130 块），数据面连续性由单次分配保证 |
| 10 | 名字上限定 `RAVENFS_MAX_NAME_LEN = 552`（=40 内联 + 512，对齐 512B 档单单元）；名字堆位图面/数据面统一走 mdata_piece_caches（8K 负载片，位图≥8K 即为此上调）；`tmp_name_len_limiter` → `name_len_limiter`（2026-08-29 决策） | POSIX NAME_MAX=255 是实际约束，552 为向上拓展一层的兜底；堆块访问复用元数据缓存 |

坐标系与块模型（FS_block 世界、Primary/Backup Superblock、bitmap 尾巴布局、
inode array/inode bitmap 推导）**全部继承 v1，无变化**，见 `block_model.md`。

---

## 1. 格式版本

`RavenSuperblock.version` 在 v1.1 起 bump。v1 盘与 v1.1 结构不兼容
（节点格式全变），v1.1 不承诺读旧盘。

---

## 2. 指针类型

### 2.1 `ptr_in_idx`（8B）

```
free_to_use:12 | ptr:52
```

- 52 位地址空间：块号 `[0, total_blocks)` 或 inode 索引 `[0, inode_array_count)`
- `free_to_use` 语义由上下文定义（当前唯一实例：Dentry_t: name_len；名字堆控制器用裸 52 位指针，不走 `ptr_in_idx`）
- 哨兵：`PTR_INVALID = ~0ULL`（v1 惯例沿用，禁止裸写 0——0 是合法指针）

### 2.2 `parent_ptr_t`（8B）

```
entry_count:10 | is_inner_node:1 | is_root_node:1 | ptr:52
```

- `entry_count` = 节点有效 entry 数（0~阶数上限，10 bit 够 337；根/非根通用，
  替代 v1 的 node_meta 字段，v1.1 节点计数唯一来源）
- 双位解码四态；根节点 `ptr` = 自身 inode 编号（即 `.`），非根 = 父节点块号。

---

## 3. Inode（256B，尺寸不变）

### 3.1 结构分型与首 64 位

inode 槽按 `type` 选择解码结构（两结构均 256B packed，前 96B 布局一致）：

```
raven_Inode_t     (REGULAR / SOFT_SYM):  type:2 | is_data_root_direct_bit:1 | other:61
raven_dir_Inode_t (DIR):                 type:2 | other:62
```

- `raven_Inode_t`：`is_data_root_direct_bit` 有效（三态解码见 §3.2）；`inline_data[160]` = 文件内联数据
- `raven_dir_Inode_t`：**无** `is_data_root_direct_bit`——dir 的 `data_root` 语义单一（§3.2）；
  160B 内联区 = **10 个 `name_heap_controller_t`**（名字堆描述符，固定数组，§7.4）

⚠ 同一 256B inode 槽：前 96B（`type` ~ `padding[2]`）两结构逐字段一致；
后 160B 按 type 分型 = REGULAR/SOFT_SYM 文件内联数据 / DIR 堆描述符。

### 3.2 `data_root` 解码

REGULAR / SOFT_SYM（`raven_Inode_t`，`is_data_root_direct_bit` 有效，三态继承 v1）：

| `data_root` | `is_data_root_direct_bit` | 含义 |
|-------------|---------------------------|------|
| PTR_INVALID | don't care | 内联：内容在 inline_data[0..size-1] |
| ≠ INVALID | = 1 | 直指区间：连续 FS_block |
| ≠ INVALID | = 0 | 区段 B+tree 根节点 |

DIR（`raven_dir_Inode_t`，单态）：

- `data_root` **恒 = 块指针，直指 dentry B+tree 根节点**（目录数据唯一组织方式）
- 无内联目录、无直指区间目录；`PTR_INVALID` 是非法状态（mkfs 后不应出现）
- `size` = 非 `.` `..` 的 dentry 条目数（继承 v1）

---

## 4. B+tree 节点（8K = 8192B，全部 static_assert 锁死）

### 4.1 节点元数据 `Bptree_meta_t`（独立 64B 头）

```
magics[4] (32B) | parent_ptr_t (8B) | ptrs[3] (24B)
```

`ptrs[3]` 按角色解码（四态）：

| 角色 | ptrs[0] | ptrs[1] | ptrs[2] |
|------|---------|---------|---------|
| 非根叶子 | 预留 | 前驱叶子块号 | 后继叶子块号 |
| 非根内部 | 预留 | 预留 | 预留 |
| 根叶子 | 预留 | `..` inode | 预留 |
| 根内部 | 预留 | `..` inode | 预留 |

**内部节点无最左子树**：上界路由下 N hints = N children，无孤儿子树。

### 4.2 容量 + 1 瞬时槽

所有节点数组 = **阶数 + 1**，`[阶数]` 槽是插入瞬时产物：

- 插入：先占瞬时槽 → 检查 `count > 阶数` → 超则分裂
- 单趟处理、内外统一（叶子/内部同一套"插→查→裂"逻辑，v1 两套满判断的教训）
- ⚠ 瞬时槽仅存内存，**禁止落盘**——误落盘会破坏 count 语义

### 4.3 四种节点尺寸链

```
meta = 32 + 8 + 24 = 64B

区段树（337 阶）:
  inner: 64 + 338×16 + 340×8 = 8192 ✓   (hints 338 槽, [337] 瞬时)
  leaf:  64 + 338×24 + 2×8   = 8192 ✓   (extents 338 槽, [337] 瞬时)

Dentry 树（126 阶）:
  inner: 64 + 127×16 + 762×8 = 8192 ✓   (hints 127 槽, [126] 瞬时)
  leaf:  64 + 127×64         = 8192 ✓   (entries 127 槽, [126] 瞬时)
```

---

## 5. 区段树（文件逻辑块 → 物理块）

### 5.1 Entry

```
extent_t:        length:64 | fblkbase:64 | block_base: ptr_in_idx
                 闭区间 [fblkbase, fblkbase+length]，块数 = length+1（继承 v1）
extent_hint_t:   key:64 | sub_node_ptr: ptr_in_idx
```

### 5.2 路由（上界，v1 max_R 继承）

- 叶子 invariant：相邻 extent 满足 `fblk_cmp(entries[i], entries[i+1]) == 1`
  （无重叠、无逆序，fblk_cmp 定义继承 v1）
- 内部：`key[i] = 子树 i 最大右端点 max(fblkbase+length)`
- 查找：`target ≤ key[i] → sub_node_ptr[i]`

### 5.3 分裂 / 删除

- 分裂：叶子满（count > 337）→ 新块装右半，**左半最大右端点提升**为父 separator
- 删除：删叶子最大右端点项 → 刷父 `key`（上界路由对称代价，必走路径）

---

## 6. Dentry 树（目录项）

### 6.1 Entry

```
Dentry_t:  inode_ptr: ptr_in_idx   (free_to_use = name_len，≤ RAVENFS_MAX_NAME_LEN=552)
           hash: 64                (名字 64 位 hash，算法待定)
           full_char_remain_ptr: 64 (name_len>40: 物理定位 块号[12:63]|偏移[0:11] → name[40..]；否则 PTR_INVALID)
           name[40]
Dentry_hint_t:  sub_ptr: ptr_in_idx | hash_max: 64
```

### 6.2 路由（hash 上界）

- 查找：`target_hash ≤ hash_max[i] → sub_ptr[i]`
- 分裂：左半最大 hash 提升为父 separator
- 删除：**删叶子最大项 → 必刷父 hash_max**（写死路径；下界路由删最小项
  要刷的坑见 v1 bug 3.3，v1.1 换方向后仍在删除路径上）
- 叶子内 **(hash, name) 复合排序**：hash 定位后碰撞区间内按名字字典序，
  名字可二分；路由只看 hash（同 hash 必在同路径），碰撞区间内比较完整名字

### 6.3 碰撞

64 位 hash 碰撞概率极低（65536 条目期望碰撞 ~6e-8），但必须处理：
同 hash 条目在叶子内连续、插入定位同 hash 区间、比较走完整名字
（>40B 时读名字堆外置部分）。

### 6.4 readdir

按 hash 乱序输出（路径解析高频、ls 低频，接受）。`. / ..` 不在树内
（根节点 parent_ptr = `.`、ptrs[1] = `..`），由 readdir 路径合成。

### 6.5 删除策略

**物理删除**（v1 tombstone 弃用）：叶子内 memmove + 欠载处理。
min occupancy / 合并阈值：**待定**（分裂后半满 = 63/126，合并阈值建议挂钩）。

---

## 7. 名字堆（name heap）— 长名外置

### 7.1 动机

名字 ≤40B 内联；>40B 的外置部分（≤ 512B = RAVENFS_MAX_NAME_LEN−40）由每目录名字堆分配。
堆化替代"每长名散装块内偏移"：分配/回收统一走位图、名字数据聚簇（IO 局部性）。

### 7.2 硬约束

```
文件名上限 RAVENFS_MAX_NAME_LEN = 552B（40 内联 + 512 外置；2026-08-29 定，
对齐 512B 档单单元上限；POSIX VFS 层 NAME_MAX=255 是实际约束，552 为向上
拓展一层的兜底）
外置部分 ≤ 512B；从 8B 对齐起点起可能跨 4K 块边界，极端偏移（片内第二块
高偏移）下跨 8K 片（≤2 片）→ name_heap_read/write 按两段拷贝
free_to_use 12 位存 name_len（0~4095）够
```

### 7.3 控制器 `name_heap_controller_t`（16B）

```
bytes_per_bit_log2:6 | total_bits_count_log2:6 | bitmap_base_block_ptr:52   (u64[0])
reserved:12           | data_base_block_ptr:52                               (u64[1])
```

- `bit[i]` 管辖数据区 `[i×2^bytes_per_bit_log2, (i+1)×2^bytes_per_bit_log2)`
- 位图块 / 数据块在 FS_block 世界内，全局 bitmap 亮灯
- 位图占用 FS_block 数 = `2^total_bits_count_log2 / 32768` 向上取整

### 7.4 固定 10 描述符（dir inode 160B 内联区）

`raven_dir_Inode_t` 内联区 = `name_heap_controller_t[10]`：5 粒度 × 位图容量档，
**结构恒定、无 level 升级路径**；每堆按需独立激活（§7.6）。

| 字段 | 粒度 | 位图 | total_bits_count_log2 | 承接尾部 (L−40) | 对应全名 L |
|------|------|------|----------------------|-----------------|-----------|
| heaps_8B_8k    | 8B   | 8KB  | 16 | 0~40B    | 41~80  |
| heaps_8B_16k   | 8B   | 16KB | 17 | 0~40B    | 41~80  |
| heaps_8B_32k   | 8B   | 32KB | 18 | 0~40B    | 41~80  |
| heaps_64B_8k   | 64B  | 8KB  | 16 | 41~64B   | 81~104 |
| heaps_64B_16k  | 64B  | 16KB | 17 | 41~64B   | 81~104 |
| heaps_128B_8k  | 128B | 8KB  | 16 | 65~128B  | 105~168 |
| heaps_128B_16k | 128B | 16KB | 17 | 65~128B  | 105~168 |
| heaps_256B_8k  | 256B | 8KB  | 16 | 129~256B | 169~296 |
| heaps_256B_16k | 256B | 16KB | 17 | 129~256B | 169~296 |
| heaps_512B_8k  | 512B | 8KB  | 16 | 257~512B | 297~552 |

- 分档 = 尾部长度上取整到最小匹配粒度；`bytes_per_bit_log2` 实际取值 8B→3 / 64B→6 / 128B→7 / 256B→8 / 512B→9
- 8B 档下限 0 为退化情形（L=40 全内联，不产生分配）；单元数 = ceil(尾部/粒度)：8B 档 ≤5 单元，其余档恒 1 单元（RAVENFS_MAX_NAME_LEN=552 → 尾部 ≤512B，无跨档续分配）
- 长度均指**有效载荷**，不含 NUL 结尾符——dentry 的 `name_len`（`inode_ptr.free_to_use`）已描述载荷，堆内不存结尾符

激活即分配容量（= 2^total_bits_count_log2 × 粒度，位图满配 = 数据面用尽）：

| 堆 | 位图块数 | 数据区容量 | 数据区块数 |
|----|---------|-----------|-----------|
| 8B_8k   | 2  | 512 KiB | 128  |
| 8B_16k  | 4  | 1 MiB   | 256  |
| 8B_32k  | 8  | 2 MiB   | 512  |
| 64B_8k  | 2  | 4 MiB   | 1024 |
| 64B_16k | 4  | 8 MiB   | 2048 |
| 128B_8k | 2  | 8 MiB   | 2048 |
| 128B_16k| 4  | 16 MiB  | 4096 |
| 256B_8k | 2  | 16 MiB  | 4096 |
| 256B_16k| 4  | 32 MiB  | 8192 |
| 512B_8k | 2  | 32 MiB  | 8192 |

⚠ **mdata_piece 约束 + 档位级懒分配**：每堆的位图面与数据面**各自保持连续区间**、起点 8K 对齐
（块号 ≡ 0 mod 2，8K = 2 个 FS_block），均为标准 8K 负载片，访问统一走 mdata_piece_caches
（mdata_piece_session；位图 8K/16K/32K = 1/2/4 个负载片）。**激活 = 位图面 + 数据面
按位图容量一次分配**（各自 block_alloc_hard 连续区间，单片内完成、失败走下一片，全局
bitmap 亮灯；数据面容量 = 2^total_bits_count_log2 × 粒度，见下表）；未激活档位零开销。
释放只清位、不收缩数据面（回收策略待定，§9）。

### 7.5 分配规则

- **分流**：按尾部长度 (L−40) 落到粒度匹配的堆（§7.4 表），短尾 → 细粒度、长尾 → 粗粒度，
  目标：各堆位图 bit 占用率尽量高（位图块不空转）
- ⚠ **名字数据必须连续**：分配 = 找 N 个连续空闲 bit 并置位（非单 bit），
  释放 = 连续区间清位；`full_char_remain_ptr` 只给起点，长度由 name_len−40 隐含
- 单元数 = ceil(尾部/粒度)：8B 档 ≤5 单元，64B/128B/256B/512B 档恒 1 单元
  （RAVENFS_MAX_NAME_LEN=552 → 尾部 ≤512B，无跨档续分配）
- 同粒度多档 = **容量扩容关系**（不是分流）：位图**无连续空闲单元**（全部分配干净）才激活
  下一档（8k → 16k → 32k），旧档数据不迁移；512B 档仅 8k 一档，满 = 该目录长名空间
  耗尽（返回 RC_ERR_NO_BLOCK）

### 7.6 生命周期（按堆独立激活）

- **按需独立激活**：目录首次出现落入该档的名字时创建该堆——位图面 + 数据面
  按位图容量一次分配（§7.4），未激活档位零开销
- **档位扩容**：当前档位位图**全部分配干净**（无连续空闲单元）→ 激活同粒度下一档
  （8k → 16k → 32k），新分配进新档，旧档数据不迁移
- 未激活堆：`bitmap_base_block_ptr` / `data_base_block_ptr` 双指针 = 52 位全 1
  （PTR_INVALID 惯例，兼作激活哨兵；此时 log2 字段为 63 = 非法值，不产生歧义）
- 回收：目录清空长名后是否释放堆（位图/数据区归还全局 bitmap），**待定**（§9）

---

## 8. 与 v1 的差异表

| 维度 | v1 | v1.1 |
|------|----|------|
| 节点元数据 | 借用 entry[0].words[3] | 独立 Bptree_meta_t（64B） |
| 指针 | 裸 u64 + MASK 宏 | ptr_in_idx / parent_ptr_t 位域 |
| 节点数组 | 恰好容量（256/63） | 容量 + 1 瞬时槽（338/127） |
| Dentry 排序 | strcmp 字典序 | (hash, name) 复合序 |
| 内部路由 | 下界（子树最小名） | 上界（hash_max / max_R） |
| Dentry 大小 | 128B（name[120]） | 64B（name[40] + hash + 外置指针） |
| 长名 | 无策略（120B 截断风险） | 名字堆 + 物理定位外置（上限 RAVENFS_MAX_NAME_LEN=552B） |
| 删除 | tombstone | 物理删除（欠载策略待定） |
| readdir | 字典序 | hash 乱序（合成 . / ..） |
| 扫描签名 | 借位（entry[3..6].words[3]） | 独立 magics[4] |
| 目录 inode | 复用通用结构（data_root 多态 + 忽略位） | 独立 `raven_dir_Inode_t`：data_root 直指 dentry 树，160B = 固定 10 堆描述符 |

---

## 9. 待定决策

| 项 | 现状 | 影响 |
|----|------|------|
| hash 算法 | **已定：XXH64（seed=0）**，2026-08-20 决策（弃 crc32：32 位熵 + 可线性构造；弃挂载随机 key 方案：hash 落盘需跨挂载稳定） | 抗 DoS 升级：SipHash-2-4 + key 存 SB（mkfs 生成一次），接口签名不变 |
| min occupancy / 合并阈值 | 待定 | 物理删除后叶子欠载策略（建议挂钩半满 63） |
| 堆回收时机 | 待定（目录清空长名后是否释放位图/数据区、数据面是否收缩） | 空间归还策略 |

---

## 10. 不变量清单（实现契约）

### Dentry 树

1. 叶子 entries 按 `(hash, name)` 复合序严格递增；count ≤ 126（count = `parent_ptr.entry_count`）
2. 内部 hints 按 hash_max 递增；`hints[i].hash_max = 子树 i 最大 hash`
3. 查找：`target ≤ hash_max[i]` → `sub_ptr[i]`（单规则，无最左子树特判）
4. 分裂：瞬时槽占用后 count=127 → 右半 64 拷新块，左半 63，左半末位 hash 提升
5. 删除叶子最大项 → 刷父 hash_max；删最小项无感（上界路由收益）
6. 瞬时槽 `[126]` 永不落盘
7. 碰撞区间内名字比较用完整名字（>40B 读堆）

### 区段树

8. 叶子相邻 extent `fblk_cmp == 1`（无重叠无逆序）
9. `hints[i].key = 子树 i 最大右端点`；`target ≤ key[i]` → 路由
10. 分裂提升左半最大右端点；删最大项刷父 key

### Inode

11. DIR inode（`raven_dir_Inode_t`）`data_root` 恒 = dentry 树根块号；PTR_INVALID 非法
12. 160B 内联区按 type 分型：REGULAR/SOFT_SYM = 文件数据；DIR = 10 个堆描述符（固定）

### 名字堆

13. `full_char_remain_ptr` 指向 name[40..] 起点，长度 = name_len−40 隐含（不含 NUL）
14. 一个名字的粒度单元连续（找连续空闲 bit 串）；8B 档 ≤5 单元，其余档单单元
15. 堆的位图块/数据块全局 bitmap 常亮；删名字清位、删目录回收
16. 未激活堆双指针全 1 哨兵；激活堆位图面/数据面各为连续区间、起点 8K 对齐、按容量一次分配；
    位图满才激活同粒度下一档；堆块访问走 mdata_piece_caches

---

> **一句话记**
>
> v1.1 = 元数据独立 + 指针类型化 + hash 上界路由 + 容量+1 瞬时槽 + 名字堆
> （dir inode 直指 dentry 树 + 160B 内联区固定 10 堆描述符）。
> 每一条都对着 v1 的一个真实痛点，改动方向由 bug 记录驱动，不是重写癖。

# RavenFS — 磁盘布局 v1.1

> 2026-08-19 对齐定稿。
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
- `free_to_use` 语义由上下文定义（Dentry_t: name_len；level 1 名字堆: 粒度 log2）
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

### 3.1 首 64 位

```
type:2 | is_data_root_direct_bit:1 | dir_inode_name_heap_level:2 | other:59
```

`dir_inode_name_heap_level` 仅 dir 型 inode 有效（REGULAR/SOFT_SYM 恒 0）：

| level | 语义 |
|-------|------|
| 0 | 目录所有名字 ≤40B 全内联（Dentry_t.name），无名字堆，零开销 |
| 1 | `inline_data[160]` 充当位图；`padding[1]` 复用为 `ptr_in_idx`（ptr=数据区块基址，free_to_use=粒度 log2） |
| 2 | `padding[2]`(16B) + `inline_data[160]` = 11 个 `name_heap_controller_t`，位图/数据全外置 |
| 3 | 预留，未设计 |

⚠ **`inline_data` 语义按 type 分档**：REGULAR = 文件内联数据；
DIR level 1 = 名字堆位图；DIR level 2 = 控制器数组。

### 3.2 `data_root` 三态解码（继承 v1）

| `data_root` | 指针类型 | `is_data_root_direct_bit` | 含义 |
|-------------|---------|---------------------------|------|
| PTR_INVALID | — | don't care | 内联：内容在 inline_data[0..size-1] |
| ≠ INVALID | 块指针 | = 1 | 直指区间：连续 FS_block |
| ≠ INVALID | 块指针 | = 0 | B+tree 根节点（REGULAR: 区段树；DIR: dentry 树） |

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
Dentry_t:  inode_ptr: ptr_in_idx   (free_to_use = name_len, 0~4095)
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

名字 ≤40B 内联；>40B 的外置部分（≤ 2047-40 = 2007B）由每目录名字堆分配。
堆化替代"每长名散装块内偏移"：分配/回收统一走位图、名字数据聚簇（IO 局部性）。

### 7.2 硬约束

```
文件名上限 2047B（ravenfs 文件系统级）
外置部分 ≤ 2007B < 4096B → full_char_remain_ptr 永不跨块
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

### 7.4 三档结构（inode 字段控制）

```
level 0: 全内联，无堆                                            （零开销）
level 1: inline_data[160] = 位图 (1280 bit)
         padding[1] 复用 ptr_in_idx = {粒度 log2, 数据区块基址}
         位图不装满（留升级余量）
level 2: padding[2] + inline_data = 11 个控制器                   （异构堆）
         位图/数据全外置
```

### 7.5 分配规则

- **异构堆**：各堆粒度（bytes_per_bit_log2 实际取值建议 3~11 = 8B~2KB）与
  位图容量独立，按需扩容（新名字长度分布 → 新建堆）
- **名字按长度分流**到粒度匹配的堆（短名 → 细粒度堆、长名 → 粗粒度堆），
  目标：各堆位图 bit 占用率尽量高（位图块不空转）
- ⚠ **名字数据必须连续**：分配 = 找 N 个连续空闲 bit 并置位（非单 bit），
  释放 = 连续区间清位；full_char_remain_ptr 只给起点，长度由 name_len-40 隐含

### 7.6 升级路径（level 0 → 1 → 2）

- 0 → 1：目录出现第一个 >40B 名字。位图内联（inline_data），数据区外置
- 1 → 2：位图容量不足（1280 bit 按当前粒度管满）。**新建控制器承接**：
  旧堆位图/数据原样保留（level 1 已预留升级余量，位图外置搬移），
  后续新名字进新堆；数据不迁移
- 降级：目录清空长名后是否回收堆 / 降回 level 0，**待定**

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
| 长名 | 无策略（120B 截断风险） | 名字堆 + 物理定位外置（上限 2047B） |
| 删除 | tombstone | 物理删除（欠载策略待定） |
| readdir | 字典序 | hash 乱序（合成 . / ..） |
| 扫描签名 | 借位（entry[3..6].words[3]） | 独立 magics[4] |

---

## 9. 待定决策

| 项 | 现状 | 影响 |
|----|------|------|
| hash 算法 | **已定：XXH64（seed=0）**，2026-08-20 决策（弃 crc32：32 位熵 + 可线性构造；弃挂载随机 key 方案：hash 落盘需跨挂载稳定） | 抗 DoS 升级：SipHash-2-4 + key 存 SB（mkfs 生成一次），接口签名不变 |
| min occupancy / 合并阈值 | 待定 | 物理删除后叶子欠载策略（建议挂钩半满 63） |
| 名字堆分档阈值 | 待定（40~128 / 129~512 / 513~2047？） | 异构堆粒度选择 |
| level 1 升级余量 | 待定（位图留多少） | 0→1→2 迁移平滑度 |
| 降级策略 | 待定 | 堆回收时机 |

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

### 名字堆

11. `full_char_remain_ptr` 指向 name[40..] 起点，长度 = name_len-40 隐含
12. 一个名字的粒度单元连续（找连续空闲 bit 串）
13. 堆的位图块/数据块全局 bitmap 常亮；删名字清位、删目录回收
14. level 1 位图不装满（升级余量）；level 2 控制器 = 旧堆承接 + 新堆扩容

---

> **一句话记**
>
> v1.1 = 元数据独立 + 指针类型化 + hash 上界路由 + 容量+1 瞬时槽 + 名字堆。
> 每一条都对着 v1 的一个真实痛点，改动方向由 bug 记录驱动，不是重写癖。

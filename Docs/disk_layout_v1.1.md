# RavenFS — 磁盘布局 v1.1

> 2026-08-19 对齐定稿；2026-08-29 修订（dir inode 直指 dentry 树 + 固定 10 堆描述符，据 `disk_struct_v1.1.hpp` 反向维护）；2026-09-01 修订（区段树区间语义半开化 + link_count 契约补全）；2026-09-04 修订（REG/SYM 数据区解码判据伪代码化、弃哨兵主判；文件数据区状态机契约落 v1 §4.1，v1.1 收编 §10 不变量 22~25）；2026-09-09 修订（不变量 24：extent→direct 出口缩水通道 + truncate 增大语义，据 `punch_truncate_pread_spec.md` 拍板）。
> v1（`disk_layout_v1.md` + `block_model.md`）的继承与修正。
> 结构定义见 `include/disk_struct_v1.1.hpp`，本文是它的设计意图与不变量契约。

---

## 0. v1 → v1.1 变更动机

v1 的两个结构级教训（v1 排障记录已随 v1 文档归档，git history 可查）：

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

### 0.1 v1.1 内修订（2026-08-29 起，据 `disk_struct_v1.1.hpp` 反向维护）

| # | 修订 | 效果 |
|---|------|------|
| 7 | dir 型 inode 独立成 `raven_dir_Inode_t`：`data_root` **直指 dentry B+tree 根节点**（单态，无内联/直指/level 多态） | 消除 dir 侧冗余：`is_data_root_direct_bit` / `dir_inode_name_heap_level` 对目录无意义 |
| 8 | 160B 内联区固定为 **10 个 `name_heap_controller_t`**（5 粒度 × 位图容量档） | 消灭三档 level 升级路径，描述符结构恒定、按堆独立激活 |
| 9 | 名字堆**懒加载懒分配（档位级）**：未激活档位零开销（mkfs 不预留）；激活 = 位图面 + 数据面按位图容量一次分配（各自 block_alloc_hard 连续区间）；某档位位图全部分配干净才激活下一档（2026-08-29 决策，alloc_at 定点认领驳回） | 激活成本 = 该档容量（如 8B_8k 共 130 块），数据面连续性由单次分配保证 |
| 10 | 名字上限定 `RAVENFS_MAX_NAME_LEN = 552`（=40 内联 + 512，对齐 512B 档单单元）；名字堆位图面/数据面统一走 mdata_piece_caches（8K 负载片，位图≥8K 即为此上调）；`tmp_name_len_limiter` → `name_len_limiter`（2026-08-29 决策） | POSIX NAME_MAX=255 是实际约束，552 为向上拓展一层的兜底；堆块访问复用元数据缓存 |
| 11 | **区段树区间语义半开化**（2026-09-01 决策，代码未动、零迁移成本窗口内拍板）：extent_t 左闭右开 `[fblkbase, fblkbase+length)`，`length` = 块数（弃 v1 闭区间 `len=末端偏移/块数=len+1`）；路由 `key` = 子树最大**开边界**、查找 `target < key`；`fblk_cmp` 相邻判定同步半开化（完全在前 ⇔ `fblkbase_i+length_i ≤ fblkbase_{i+1}`） | 消灭全部 off-by-one 转换点（长度换算/相邻合并/分裂/截断），对齐 ext4/XFS 的 start+len 惯例，LLM 生成代码正确性优先；字段结构零变化，仅语义解读 |
| 12 | **link_count 契约补全**（2026-09-01 决策）：字段自 v1 就在（uint32_t，前 96B 一致）但 v1/v1.1 均无语义解释——REG/SOFT_SYM = 指向该 inode 的 dentry 总数（含创建者，硬链接计数，归零即回收，软链接允许被硬链接）；DIR 初定恒 = 1（见 #13 当日修订）；link(2)/unlink 前置与崩溃顺序、孤儿检测（§3.3 + 不变量 17~21） | 多 dentry 同 inode（硬链接）+ SOFT_SYM 已设计，无契约则 unlink/link/回收语义悬空，LLM 生成代码无依据 |
| 13 | **DIR link_count 语义再修订**（2026-09-01 当日，弃恒 1）：改 `link_count = size + 2`（2 = `.`/`..` 合成引用约定），**以 size 为真值随写**。动机：恒 1 反 POSIX 直觉（LLM 本能写 2+子目录数，mkdir 代码 `link_count=2` 即实锤），对抗本能的注释不可靠；`size+2` 把维护降为同 inode 内代数关系——无类型判断、无跨目录维护（POSIX 2+子目录数需 mkdir/rmdir/rename 维护父目录，是 LLM 最大漏写点），犯错窗口最小；size 从摆设转为真值，插入/删除/rename 路径开始实际维护 | 代价：ls 显示目录 nlink = 2+条目数（非 POSIX 2+子目录数），自研 FS 接受；fsck 校验升级为 `link_count == size+2` 三重 |
| 14 | **REG/SYM 数据区解码判据伪代码化**（2026-09-04）：弃表格式“data_root 哨兵主判”，改 v1 §4.1 伪代码判据——**size 优先、`data_root` 取值退出判定**（哨兵退为内联态规范标记）；三态不变量 / 状态迁移 / 翻转写序契约补齐落 v1 §4.1（v1.1 继承未改动），要点收编 §10 不变量 22~25 | 消除“哨兵判据 vs size 判据”双轨歧义（读路径 / fsck 一律以 size 为真值，外置残留 ≠ PTR_INVALID 而 size≤160 = 不一致）；与 v1 判据文本同构，ftype() 与读写路径实现无二义 |
| 15 | **Superblock 卷生命周期时间戳**（2026-09-09）：`RavenSuperblock` 追加 4×u64（born/mount/umount/modify，epoch 秒，置于 backup_count 后 pad 前，结构体仍 4096B）——born = mkfs 时刻恒不变；mount/umount 由挂载/卸载路径运行时维护（非 readonly 才写；readonly 审计挂载不写）；modify 预留给未来扩容/缩容工具，mkfs 时 == born。**Backup SB 不随动**（保留 mkfs 时刻；救急副本语义）。0 哨兵 = 从未挂载/从未干净卸载；崩溃后 mount 新而 umount 旧 → 非正常关机可检出。字段布局细节见 disk_struct_v1.1.hpp 结构体头注释 | 长时间使用的运维信息：卷龄 / 最近使用 / 干净卸载历史 / 几何变更史；运行时代价 = 每次挂载/卸载各 1×4K 写 + crc32 重算（static crc32_compute 现成） |

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

> ⚠️ **增量已出（2026-09-26）**：inode 布局有 **v1.2 改造**（第一字 `type:6｜direct:1｜i_flags:57`；`permission_bits → mode:12｜xattr_ptr:52`；无数据类型 + 设备号存 `inline_data`；新增 xattr 块）→ 见 **`disk_layout_v1.2_inode.md`**。
> 本文 §3 为 **v1.1 现行格式**；v1.2 冻结后由其替代（**v1.1 盘不兼容**，需 reformat）。

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

解码判据 **只依赖 `size` 与 `is_data_root_direct_bit`**（2026-09-04 起用 v1 伪代码形式，弃表格式“data_root 哨兵主判”——`data_root` 取值不参与判定，哨兵退为内联态规范标记；size 优先，判定顺序敏感）：

```c
/* 判据：假设受审对象是正常 inode（§10 不变量 22~23），以引用访问 i */
if (i.size <= 160) {                    // 内联：内容在 inline_data[0..size-1]
    // inline —— data_root 规范 = PTR_INVALID；is_data_root_direct_bit 无解码意义（规范置 0）
} else if (i.is_data_root_direct_bit) { // 直指区间
    // i.data_root = base_blk（连续区首 FS_block）
    // 物理覆盖 = 以 base 起始的 ceil(size/4096) 个连续 FS_block；
    // 逻辑字节 o → FS_block[base + o/4096] 内偏移 o % 4096（无块级空洞）
} else {                                // 区段树
    // i.data_root = 区段 B+tree 根节点块号
    // size = 逻辑占用大小（含空洞；未映射逻辑块读 0）
}
```

- size ≤ 160 一律按内联解读：data_root / direct_bit 无解码意义；外置残留（data_root ≠ PTR_INVALID 而 size ≤ 160）= 不一致，fsck 以 size 为真值清理（不变量 23）
- 直指 = 全文件无块级空洞（逻辑块 [0, ceil(size/4096)) 全映射且物理连续）的最优路径：一次间接、零树节点 I/O；任何块级空洞 / 碎片 → 整文件迁区段树（数据零搬移）
- 三态不变量表、状态迁移（inline 跨 160 升级、direct ↔ extent 零搬移、truncate ≤160 强制回迁内联）、翻转写序、SOFT_SYM 载荷约定（**size = target 字节数，不含结尾 NUL**，读回以 size 定界）：**完整契约 = v1 §4.1**（v1.1 继承未改动），要点收编为 §10 不变量 22~25

DIR（`raven_dir_Inode_t`，单态）：

- `data_root` **恒 = 块指针，直指 dentry B+tree 根节点**（目录数据唯一组织方式）
- 无内联目录、无直指区间目录；`PTR_INVALID` 是非法状态（mkfs 后不应出现）
- `size` = 非 `.` `..` 的 dentry 条目数（继承 v1）
- `link_count` = `size` + 2（2 = `.` `..` 合成引用约定；§3.3）

---

### 3.3 link_count（链接计数）—— 2026-09-01 补契约

字段：`uint32_t`；REGULAR/SOFT_SYM/DIR 三型 inode 同一 offset（`a_ns_stamp` 后、`permission_bits` 前，前 96B 一致）。

**REGULAR / SOFT_SYM（`raven_Inode_t`）**

- `link_count` = 指向该 inode 的 **dentry 条目总数（含创建者第一个）**，即硬链接总数
- 创建（create / symlink）：初始 = 1
- 硬链接 `link(2)`：`link_count++`；前置检查全过才生效（失败无副作用）：
  1. 源 inode 存在且已分配
  2. 源 type ≠ DIR（目录禁硬链接，防目录环）
  3. `link_count < UINT32_MAX`（溢出防护）
  4. 目标名在目标目录不存在（不覆盖，返回 EEXIST）
  5. 目标目录可插入（dentry 树分裂空间）
  跨目录硬链接允许（同 FS 内）；成功 = 插 dentry + `link_count++`
- unlink：`link_count--`；归 0 → 回收 inode（清 inode bitmap）+ 数据（extent 树 / 直指区间 / 内联；软链接 target 存储同按 data_root 三态释放）
- **崩溃顺序：先删 dentry（落盘）→ 后 `link_count--`**。窗口内 = 计数虚高泄漏，fsck 以 dentry 扫描为准修正；永不产生悬挂引用（反序会先出现 `link_count=0` 而 dentry 仍在的悬挂窗口）
- SOFT_SYM **允许被硬链接**（结构统一零特判）；软链接 target 文本不参与计数；解析失败（ENOENT/ELOOP/EACCES）为纯读错误，零副作用
- 磁盘计数**不感知打开句柄**：物理回收延迟到内存 inode 引用归零（VFS 层 refcount），磁盘只定义"归零可回收"状态

**DIR（`raven_dir_Inode_t`）**

- 禁止硬链接（`link(2)` 对 DIR 报错，防目录环）
- `link_count` = **`size` + 2**（2026-09-01 决策，弃恒 1）：2 = `.` / `..` 合成引用约定（dentry 树根 `parent_ptr.ptr` = "."、`ptrs[1]` = ".."，不在树内计条目）；`size` = 非 `.` `..` 的实际 dentry 条目数
- **以 size 为真值，link_count 恒按 size+2 写入**（两个字段同 inode 同写，防漂移）；无类型判断、无跨目录维护
- 创建（mkdir）：size = 0、link_count = 2；根目录同（约定，无父引用仍按 2）
- 维护点（全在同 inode 内，插入/删除/rename 条目时 size ±1、link_count ±1）：
  - 插入条目 → `size++`、`link_count++`
  - 删除条目 → `size--`、`link_count--`
  - rename 条目跨目录 → 源目录 size--/lc--，目标目录 size++/lc++
  - rmdir 空目录 → size=0、link_count=2 约定归零，直接全量回收
- 溢出：size 受 52 位块空间约束恒远小于 2^32，link_count = size+2 溢出不可达，不设防护

**与 size 的分工（DIR）**

| 字段 | 数什么 | 维护点 |
|------|--------|--------|
| `size` | 目录内实际 dentry 条目数 | 插入/删除/rename 条目时 ±1（真值） |
| `link_count` | size + 2（含 `.` `..` 合成引用约定） | 随 size 同步写（无独立维护） |

fsck 三重校验：`link_count == size + 2` 恒成立；`size == 0` ⇔ `link_count == 2`（空目录，rmdir 前提）；dentry 树扫描条目数核对 size。

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
                 左闭右开 [fblkbase, fblkbase+length)，块数 = length
                 （2026-09-01 半开化，弃 v1 闭区间 len=末端偏移/块数=len+1）
extent_hint_t:   key:64 | sub_node_ptr: ptr_in_idx
```

### 5.2 路由（上界）

- 叶子 invariant：相邻 extent 满足 `fblk_cmp(entries[i], entries[i+1]) == 1`（无重叠、无逆序）
  fblk_cmp 半开化定义（2026-09-01，v1 闭区间版为 `I.R+1 ≤ J.L`，此处 `<` 同步为 `≤`）：
  `I 完全在 J 前 ⇔ fblkbase_i + length_i ≤ fblkbase_{i+1}`（length = 块数，开边界 = fblkbase+length）
- 内部：`key[i] = 子树 i 最大开边界 max(fblkbase+length)`
- 查找：`target < key[i] → sub_node_ptr[i]`（key 是开边界，`target == key` 时不属于该子树）

### 5.3 分裂 / 删除 / 平衡（2026-09-27 固化精确值，见 §10-31..36）

- **阶 = 337**（槽 338，`[337]` 瞬时槽仅内存、禁落盘）；非根 count ∈ [168,337]，根 count ∈ [1,337]
- 分裂：叶子满（count > 337；纯插先判 ==337）→ **左 168 / 右 count−168**（叶/内同切法）；**左半最大开边界提升**为父 separator
- 删除：删叶子最大开边界项 → 刷父 `key`（上界路由对称代价，必走路径）
- **欠载平衡**：非根 **count < 168** → 触发平衡链（自下而上，父指针驱动）；**child+neighbor ≤ 337 → 合并**，否则**抹匀**（左 total/2）；根内 count==1 → root_shrink
- **深度上限 64**（`blocks[64]/child_idx[64]`，超限判成环）；节点块 = `block_alloc(sb, 2, align_log2=1)` → 2 块连续 + **8K 对齐**

---

## 6. Dentry 树（目录项）

### 6.1 Entry

```
Dentry_t:  inode_ptr: ptr_in_idx   (free_to_use = name_len，≤ RAVENFS_MAX_NAME_LEN=552)
           hash: 64                (名字 64 位 hash，算法 = XXH64(seed=0)，见 §9)
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
| 内部路由 | 下界（子树最小名） | 上界（hash_max / max 开边界） |
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
| min occupancy / 合并阈值 | **已定（2026-09-27，O-7）**：**dentry** 半满 63（分裂 127→左 63/右 64、欠载 <63）；**extent** 半满 168（分裂 337→左 168/右 169、欠载 <168、合并 ≤337、深度上限 64） | 见 §5.3 / §10-31..36（dentry 见 §10-4） |
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

8. 叶子相邻 extent `fblk_cmp == 1`（无重叠无逆序；半开语义：`fblkbase_i + length_i ≤ fblkbase_{i+1}`，length = 块数）
9. `hints[i].key = 子树 i 最大开边界 (fblkbase+length)`；`target < key[i]` → 路由（key 为开边界，等于时不入）
10. 分裂提升左半最大开边界；删最大项刷父 key

### Inode

11. DIR inode（`raven_dir_Inode_t`）`data_root` 恒 = dentry 树根块号；PTR_INVALID 非法
12. 160B 内联区按 type 分型：REGULAR/SOFT_SYM = 文件数据；DIR = 10 个堆描述符（固定）

### 名字堆

13. `full_char_remain_ptr` 指向 name[40..] 起点，长度 = name_len−40 隐含（不含 NUL）
14. 一个名字的粒度单元连续（找连续空闲 bit 串）；8B 档 ≤5 单元，其余档单单元
15. 堆的位图块/数据块全局 bitmap 常亮；删名字清位、删目录回收
16. 未激活堆双指针全 1 哨兵；激活堆位图面/数据面各为连续区间、起点 8K 对齐、按容量一次分配；
    位图满才激活同粒度下一档；堆块访问走 mdata_piece_caches

### link_count

17. REGULAR/SOFT_SYM：`link_count` = 指向该 inode 的 dentry 总数（含创建者）；已分配 inode 恒 ≥ 1；归 0 = 孤儿，fsck 回收（inode + 数据）
18. DIR：`link_count` = `size` + 2（2 = `.` `..` 合成引用约定）；size = 实际 dentry 条目数；违反 = 不一致
19. `link(2)` 前置：源已分配、type ≠ DIR、`link_count < UINT32_MAX`、目标名不存在；全过才 `++`，失败无副作用
20. unlink 顺序：先删 dentry（落盘）→ 后 `link_count--`；磁盘回收延迟到 VFS 内存引用归零
21. fsck：以 dentry 树扫描引用数为准重建 REG/SOFT_SYM 计数；dentry 指向的 inode 必须已分配（禁悬挂）；DIR 校验 `link_count == size+2` 与 `size==0 ⇔ link_count==2`（空目录）

### 文件数据区（REGULAR / SOFT_SYM）

22. 解码判据只依 `size` 与 `is_data_root_direct_bit`（size 优先；`data_root` 取值不参与判定）：size ≤ 160 → 内联（内容 inline_data[0..size-1]，规范 data_root = PTR_INVALID）；size > 160 ∧ direct_bit = 1 → 直指（data_root = 连续区首块，物理占块 = ceil(size/4096)，逻辑字节 o → data_root + o/4096 块内 o%4096，无块级空洞）；size > 160 ∧ direct_bit = 0 → 区段树（data_root = 根块号，空洞读 0）
23. 三态互斥由 size 保证：外置两态只在 size > 160 出现；size ≤ 160 而 data_root ≠ PTR_INVALID = 不一致（fsck 以 size 为准清理残留并告警）
24. 迁移：inline → direct/extent（写跨 160，内容拷入首块）；direct 增长需相邻块可续，不可得 → 整文件迁 extent（数据零搬移，旧连续区 = 单条 extent）；写洞 / 打洞（size 不变，整块释放、残块清零）产生块级洞 → extent；truncate 缩至 >160 态不变（direct 释放尾块 / extent 删尾部 extent）；truncate 增大 = 上调 size（inline ≤160 补零推进；inline 跨 160 迁出 / direct 跨块增长与 pwrite 同机械——占位分配是 direct 全覆盖表示的物理义务；extent 纯 size 推进零分配，真稀疏）；truncate 缩至 ≤160 → 强制回迁内联（读回 inline_data + 释放数据块与树 + 复位 data_root）；extent → direct **不做主动/重建式迁移**（不追严格融合），但**尾部元数据写回提交期允许出口缩水**：根叶 && 严格单条 && 覆盖 [0, ceil(size))（fblkbase==0 且 end==ceil(size/4096)）→ 缩 direct（先 inode 后 free 旧根，孤儿语义）——pwrite 增长 / truncate 缩小后收敛单条时触发，判定见 `pwrite_pipeline.md` §8 与 `punch_truncate_pread_spec.md` §6.3
25. 翻转写序（外置分配 / 迁移 / 回迁统一）：位图亮灯 → 数据块 / 树节点落盘 → inode 字段翻转（data_root / direct_bit / size 同一次写 = 提交点）；崩溃窗口 = 孤儿块，fsck 回收；禁止先翻 inode（会造出悬挂映射）

### 区段树机械参数（31–36，2026-09-27 固化）

> 来源：god 实参（`juvenile_god/extent_*.cpp`）——此前只存在于参考实现代码，现固化进盘面契约（ko 移植硬依据）。
> （编号 **26–30** 是 `disk_layout_v1.2_inode.md` §7 的 v1.2 增补，不在此处。）

31. 阶 = **337**（槽 338，`[337]` 瞬时槽仅内存、禁落盘）；非根 count ∈ [168,337]，根 count ∈ [1,337]
32. 分裂：count > 337 → **左 168 / 右 count−168**（叶/内同）；左半最大开边界提升为父 separator
33. 欠载：非根 **count < 168** → 平衡链（自下而上，父指针驱动）
34. 平衡：**child+neighbor ≤ 337 → 合并**；否则**抹匀**（左 total/2）；根内 count==1 → root_shrink
35. 深度上限 **64**（超限判成环）
36. 节点块：`block_alloc(sb, 2, align_log2=1)` → 2 块连续、**8K 对齐**

---

> **一句话记**
>
> v1.1 = 元数据独立 + 指针类型化 + hash 上界路由 + 容量+1 瞬时槽 + 名字堆
> （dir inode 直指 dentry 树 + 160B 内联区固定 10 堆描述符）。
> 每一条都对着 v1 的一个真实痛点，改动方向由 bug 记录驱动，不是重写癖。

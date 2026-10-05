# v1.3 目录名字数据面（name data plane）—— 设计 + 交接文档

> 2026-10-05 · RavenFS god（`RavenFS_juvenile`）
> 覆盖：`dir_inode` v1.3 改制、长名外置从「名字堆」迁移到「字节粒度名字数据面」
> （映射树 + 空闲区间树 fr）、通用 extent 机械 kind 泛化、独立审计。
> 本文是**冻结契约 + 实现导览 + 已知边界**的交接件。

## 0. 一句话

v1.2：每目录 10 个固定粒度**名字堆**（8B/64B/128B/256B/512B，位图面+数据面）。
v1.3：每目录一个**稠密、8K 对齐的逻辑字节空间** = 一棵**映射树**（逻辑→物理）
+ 一棵**空闲区间树 fr**（记录空闲字节区间）。
长名（>40B）tail（≤512B）按**字节粒度**分配逻辑区间；`ddentry` 只存**逻辑偏移**
`full_char_remain_offset`，物理经映射树解析。
顶层接口 `dir_insert_dentry` / `dir_rm_dentry` / `dir_lookup` **签名与语义不变**。

---

## 1. 磁盘契约（`include/disk_struct_v1.3.h`，权威）

### 1.1 `raven_dir_Inode_t`（256B；前 80B 与 `raven_Inode_t` 同构）

| offset | 字段 | 语义 |
|---|---|---|
| 72 | `ddentry_Bptree_root` | dentry 树根（原 `data_root`；dir 恒有效）|
| 80 | `name_data_ctl` | bit0 = **direct(1)/tree(0)**；[1:52)=指针(51 位, 8K 对齐偷 bit0)；[52:64)=保留 |
| 88 | `name_data_piece_count` | **8K 数据片数原值**（`RFS_ND_PIECE_COUNT`；早期 count/2 编码已废）|
| 96 | `name_data_free_ranges_ctl` | bit0 = inline(1)/tree(0)；[1:52)=fr 树根 |
| 104 | `name_data_free_ranges_count` | 全域空闲区间总数（= fr 树叶条目数）|
| 112 | `possible_inline_free_ranges[9]` | inline 空闲表（**本轮未实现**，恒空）|

- 空态哨兵：`RFS_ND_CTL_INACTIVE` / `RFS_ND_FR_CTL_INACTIVE` = `~0`（`ptr` 域全 1 = 无效）。
- 映射控制字：
  - `direct`：数据面为**单连续物理段**，ptr = 段基块（8K 对齐）；物理块 = base + 逻辑块。
  - `tree`：ptr = 映射树根块；逻辑块经映射树定位 extent 后算物理。
- `ddentry_t.full_char_remain_offset` = 长名 tail 在数据面内的**逻辑字节偏移**（非物理）。

### 1.2 映射树（`Bptree_dir_name_data_extent_{leaf,inner}_node_t`）

- 魔数 `NAMEDATA_EXTENT_MAGIC1..4`（`include/magics.h`）。
- 与文件 extent 树**同布局同阶**（`extent_t` / `extent_hint_t`，337 阶）；
  key = 子树最大开边界 `fblkbase+length`。
- **不变量**：逻辑空间 `[0, pieces×8K)` **稠密无洞**；每条 extent `length` 为偶数块
  （每 8K 数据片 = `length 2`）；物理段 8K 对齐。

### 1.3 空闲区间树 fr（`Bptree_dir_name_data_free_ranges_{leaf,inner}_node_t`）

- 魔数 `NAMEDATA_FREERANGES_MAGIC1..4`；**统一 337 阶**（`free_ranges[338]` /
  `hints[338]` + padding）。
- `free_range_t{ base, len }`（16B）：**空闲 = 删长名释放 ∪ 新片未分配尾部**
  （故无需"已用前沿"字段；分配用尽 ⇔ fr 内无 `len ≥ need`）。
- 叶子按 **复合键 `(len 主序, base 副序)`** 升序。
- 内部 hint = 子树**最大复合键 `(max_len, max_base)`**；路由 = 第一个
  `(max_len,max_base) ≥ target` 的子树，无则最右击穿。
  - 分配 best-fit：target=`(need, 0)`（等价 `max_len ≥ need`）；
  - 插入定位：target=新键 `(len, base)`。
- 本轮**不做相邻区间合并、不做欠载合并**（碎片靠 best-fit 复用），但**绝不留空叶**（见 §5）。

---

## 2. God 侧实现导览

### 2.1 `juvenile_god/name_data_plane.cpp`
- `name_data_alloc(dir, need, *off)`：空态建首片 direct + 整片入 fr → fr `take_fit`
  best-fit 复用 → 用尽则 `name_data_grow` 增片、本 tail 取新片头部、余段入 fr。
- `name_data_grow`：direct 物理尾 `block_occupy_soft` 优先；投机失败 `hard_alloc(2,1)`
  + `name_data_map_append`（映射树注入 / direct→tree 升级）。
- `name_data_free(dir, off, len)`：fr 插入。
- `name_data_read/write(dir, off, len, buf)`：`name_data_resolve` 解析物理（含逻辑上界校验）。
- `name_data_release(dir)`：数据片 + 映射树节点 + fr 树节点全量回收 + 双控制字复位。
- `name_data_resolve` / `name_data_map_append`：映射解析 / 映射尾插。

### 2.2 `juvenile_god/name_data_freeranges.cpp`（fr B+ 树）
- `name_data_fr_walk` / `find_fit` / `insert`（预算池 + 169/169 分裂级联 + 建新根）/
  `take_fit`（best-fit 摘除 + 余段回填）/ `retire_empty_leaf` / `refresh_key_chain` /
  `node_meta_init` / `max_key` / `release`。

### 2.3 通用 extent 机械 kind 泛化（复用）
- `ravenfs_v1.1.hpp`：`enum extent_tree_kind_t { EXTENT_TREE_FILE, EXTENT_TREE_NAMEDATA }`。
- 下列函数加**默认参数** `kind = EXTENT_TREE_FILE`（文件侧零改动）：
  `extent_Bptree_walk / leaf_split / inner_split / spilt_parent_apply / new_root_inner /
  node_meta_init / refresh_key_chain / map_range / split_cascade / node_balance /
  root_shrink / balance_from / new_root_leaf / orphan_release`；
  判谓词 `is_extent_node_kind(meta, kind)`。

### 2.4 顶层 / dentry 侧
- `root_dir.cpp`：`dir_*` 用 `raven_dir_Inode_t*` + `ddentry_Bptree_root`；
  `name_cmlpx_t` 填 `owner_dir_inum`；长名释放走 `name_data_free`。
- `dentry_walk_lookup.cpp`：`dentry_name_cmp(e, name, len, owner_dir_inum)`。
- `dentry_insert_delete.cpp`：dir 初始化双树空态；长名 `name_data_alloc/write` +
  `name_data_guard_t`；rmdir 走 `name_data_release`。
- `ravenfs_juvenile.cpp`：`is_extent_node_kind`。

### 2.5 审计（`juvenile_god/audit.cpp`，独立轨）
- `audit_name_data_plane` / `audit_name_data_tree`（映射树 DFS）/ `audit_name_data_leaf_chain`。
- `audit_name_data_free_ranges` / `audit_fr_tree`（fr 树 DFS + 全节点认领）。
- 不依赖 God 的 `is_extent_node_kind`，自认两套魔数；fr 盲区见 §6。

---

## 3. 关键不变量

- 逻辑空间 `[0, pieces×8K)` 稠密无洞、8K 对齐；每片 = `length 2` 的 extent。
- `name_data_ctl` 空态 ⇔ `piece_count==0`；非空必为 direct 或 tree。
- fr 区间互不重叠、落 `[0, pieces×8K)`；`fr_count == Σ 叶条目`。
- fr 叶复合键 `(len,base)` **全局严格递增**；hint = 子树最大复合键。
- **树内无 `count==0` 的叶**（空叶 hint=(0,0) 会破坏 hint 单调性 → 二分路由错位）；
  根内部不得只剩 1 孩（收缩提升）。

---

## 4. 测试与运行

| 目标 | 文件 | 覆盖 |
|---|---|---|
| `ravenfs_juv_name_data` | `tests/test_name_data_plane.cpp` | 字节打包 / free 复用 / 用尽增片 direct→tree / tree 回环 / release+audit |
| `ravenfs_juv_name_data_fr` | `tests/test_name_data_freeranges.cpp` | 301 满叶 / **>255 索引回归** / 叶裂+内节点 / 复合序 / release |
| `ravenfs_stress_name_data_fr` | `tests/stress_name_data_fr.cpp` | **百万级 + 随机/乱序**：随机 len、随机插入序灌入 → 随机 need 乱序 best-fit **取用至用尽**（校验 find_fit==take_fit、区间不重叠、fr 清零）→ **定长乱序返还+再分配**（返回恰为返还集、base 升序）；`mode=offload|selfreliat`（开缓存）|
| `ravenfs_juv_insert` | `tests/test_root_dir_insert.cpp` | 插入边界（空/40/41/552/553B 等）/ 批量叶·根分裂 / **长名插入+即时 lookup+重复 EXIST** / 树 DFS / 审计 / 重挂载长名回读 |
| `ravenfs_juv_delete` | `tests/test_root_dir_delete.cpp` | 删除边界 / **长名 140 条 split→全删 merge+收缩→fr best-fit 复用** / 正反向批量删+删空收缩 / 树 DFS / 审计 / 重挂载；`mode=offload\|nocache\|selfreliat` |
| `ravenfs_juv_audit_dfs` | `tests/test_audit_dfs.cpp` | 全盘可达性审计（含 fr 认领/校验）|

> 顶层三接口（`dir_insert_dentry` / `dir_lookup` / `dir_rm_dentry`）共用测试装置
> `tests/dir_tree_check.hpp`（树 DFS 校验器 + fixture；v1.3 掩码访问器）。

```bash
cmake --build build --target ravenfs_juvenile ravenfs_juv_audit_dfs ravenfs_juv_name_data \
                              ravenfs_juv_name_data_fr ravenfs_stress_name_data_fr \
                              ravenfs_juv_insert ravenfs_juv_delete
truncate -s 64M /tmp/x.img && ./mkfs/mkfs.ravenfs --v1.3 -f /tmp/x.img
./build/tests/ravenfs_juv_audit_dfs        /tmp/x.img   # 14/14
./build/tests/ravenfs_juv_name_data        /tmp/x.img   # 33/33
./build/tests/ravenfs_juv_name_data_fr     /tmp/x.img   # 10/10
./build/tests/ravenfs_juv_insert           /tmp/x.img   # 34/34（batch=1000）
./build/tests/ravenfs_juv_delete           /tmp/x.img   # 74/74（batch=1000，offload）
./build/tests/ravenfs_juv_delete           /tmp/x.img 1000 0 nocache    # 72/72
./build/tests/ravenfs_juv_delete           /tmp/x.img 1000 0 selfreliat # 72/72
# 百万级（建议 ≥512MiB 镜像）
truncate -s 512M /tmp/big.img && ./mkfs/mkfs.ravenfs --v1.3 -f /tmp/big.img
./build/tests/ravenfs_stress_name_data_fr /tmp/big.img 1000000 mode=offload    # 5/5
./build/tests/ravenfs_stress_name_data_fr /tmp/big.img 1000000 mode=selfreliat # 6/6
```

实测（随机灌入 1,000,000 条、随机插入序）：fr_count=1,000,001；
Phase B 乱序取用 **1,162,931** 次至 fr 用尽（`mismatch=0 overlap=0 leftover_fr=0`）；
Phase C 定长乱序返还 1000 条再分配**逐一命中且 base 升序**；
audit/回收全净（offload 10/10、selfreliat 11/11）。

---

## 5. 已修缺陷（规模测试与独立审计抓出）

1. **`child_idx` `uint8_t` 截断**（`name_data_fr_walk`）：fr 是 337 阶，索引可 >255；
   截断致 >256 条目时选错条目/子树。已改 `uint16_t` 直赋。回归：`test_name_data_freeranges`。
2. **空叶 `(0,0)` hint 破坏单调路由**：`take_fit` 删除致空叶后残留 `count==0` 叶，
   其父 hint=(0,0) 落在中间 → 二分路由跳过合法左子树 → best-fit/插入落错叶、复合序级联崩坏。
   修：余段回填同叶 / `retire_empty_leaf`（父删 hint + 释放 + 上溯 + 根收缩）。
   回归：`stress_name_data_fr`（M≥2000 即复现，修复后百万级全净）。

---

## 6. 已知限制 / TODO（按优先级）

1. **`possible_inline_free_ranges[9]` inline 空闲表未实现**（当前 fr 恒 tree 态）：
   小目录可省一棵树；需实现 inline↔tree 边界条件（用户明示「fr_tree 稳定后再加」）。
2. **fr 不做相邻/欠载合并**：长期 churn 会碎片（best-fit 仍可用）；如需回收度可选。
3. **审计未强制 fr 非根叶 min-occupancy**（当前只查「合法搜索树」）；可加「非根叶 count≥1」
   以让此类错误被 audit 直接逮住（而非只靠压测）。
4. **`name_data_resolve` direct 路径**已有逻辑上界校验；tree 路径靠 extent 覆盖校验。
5. **readdir（`root_dir_readdir`/`dir_readdir_interval`）历史未实现**（与本次无关）。
6. **旧测试 stale**：`test_root_dir_{insert,delete}` 已于 2026-10-05 迁移至 v1.3
   （新增 `tests/dir_tree_check.hpp` 共享校验器 + 长名回环，`ninja` 恢复可编）。
   其余 `test_pwrite_{smoke,phase1,big}`、`test_pread_smoke`、`test_punch_truncate_smoke`、
   `test_extent_{walk,fusion,loose_stress,unmap_loose}`、`benchmark_{radix,session_vs_nvme}`
   仍按 v1.1 位域 API 编写，在 v1.2 掩码迁移时即已编不过 —— `ninja all` 仍卡在这些
   TU（`test_extent_ops` 不受影响）；`test_name_heap*` 测的是已退役名字堆，已从构建移除。
7. **mkfs 空目录**：`--v1.3` 写双树空态（`name_data_ctl=RFS_ND_CTL_INACTIVE` 等），已就绪。

---

## 7. 代码位置速查

- 契约：`include/disk_struct_v1.3.h`、`include/magics.h`（两套 NAMEDATA 魔数）、
  `tests/header_selfcheck_v13.cpp`（布局 static_assert）。
- 声明：`include/ravenfs/ravenfs_v1.1.hpp`（name_data 原语 / fr 机械 / kind 枚举）；
  `include/ravenfs/ravenfs_audit.hpp`（审计类型）。
- 实现：`juvenile_god/name_data_plane.cpp`、`name_data_freeranges.cpp`、`audit.cpp`；
  通用树：`extent_{walk_lookup,split,balance,map_unmap}.cpp`、`direct_file_ops.cpp`。
- 测试：`tests/test_name_data_plane.cpp`、`test_name_data_freeranges.cpp`、
  `stress_name_data_fr.cpp`、`test_audit_dfs.cpp`、`test_root_dir_insert.cpp`、
  `test_root_dir_delete.cpp`；共享装置 `tests/dir_tree_check.hpp`。
- 退役：`juvenile_god/name_heap.cpp`（已删）。

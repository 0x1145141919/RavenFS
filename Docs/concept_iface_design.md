# RavenFS — 概念接口设计（移植缺口补齐批次）

> 2026-09-10 设计方 Raven，待 Sparrow 审计后实现。
> 定位：**上帝类原型的概念层补完**——把「Linux 文件系统必须存在、而当前原型缺失的概念」
> 落成平台无关的接口契约（参数 / 行为 / 失败路径 / 不变量）。
>
> **纪律边界（重要）**：
> - 本文只写**概念层 + 接口层**。接口用 RavenFS 自有词汇（`inode_idx` / `result_code_t` /
>   `dir_*` / 名字堆 / mdata_piece_session），**禁止 Linux 词汇进入签名**（`dentry` /
>   `inode*` / `iattr` / `kstat` / `errno` / `d_rwsem` 一律不出现）。
> - 每个接口末尾的「移植落点」仅为**参照注释**（说明为何要有这个概念），不构成本端契约。
> - 与现有约定对齐：`disk_layout_v1.1.md` §3.3/§6/§7/§10、`Docs/DISCIPLINE.md`（思维可见）。
>
> 关联：`Docs/linux_vfs_iface_spec.md`（Linux 侧回调规格，移植时查）、
> `Docs/disk_layout_v1.1.md`（磁盘不变量）、`Docs/punch_truncate_pread_spec.md`（数据面语义）。

---

## 0. 三层切分（本文的坐标）

| 层 | 内容 | 本文是否涉及 |
|---|---|---|
| 概念层 | 「文件系统必须具备的能力/对象关系」——平台无关 | **是**（每接口的"概念"段） |
| 接口层 | 上帝类上暴露的方法签名 + 语义 + 失败路径 | **是**（每接口主体） |
| 平台层 | Linux 回调 / `d_*` helper / `i_rwsem` / errno | **否**（只在"移植落点"里点一句） |

判据：**概念是"能力存在与否"，接口是"你怎么调"，平台是"Linux 怎么调你"。**
移植 = 把接口层机械映射到平台层；因此**接口层必须概念完备、且不被平台绑架**。

---

## 1. 本批接口一览

| # | 接口 | 概念缺口 | 状态（现状） |
|---|---|---|---|
| I1 | `dir_link` | 名字可挂到**既有 inode**（引用计数真正落地） | 新概念，机制缺 |
| I2 | `dir_rename` | 名字**重绑定**（换父/换名/覆盖/目录 `..`+计数） | 新概念，机制缺（最大） |
| I3 | `dir_get_parent`（+内部 `dir_set_parent`） | 目录**唯一父**的可读可写 | 半有（存了 `..`，无原语） |
| I4 | `dir_enum_open` / `dir_enum_next` | 目录枚举 +**可续游标**（cookie） | 新概念，机制缺 |
| I5 | `symlink_set_target` / `symlink_get_target` | symlink = **内容是路径**的对象 | 新概念，机制缺 |
| I6 | `inode_getattr` / `inode_setattr` | 属性**读**；属性**写**（元数据 vs size 分家） | 导出缺 / 元数据写缺 |
| I7 | `fs_statfs` | 卷**空闲统计** | 导出缺 |
| I8 | `fs_sync` | **持久化/同步边界**（独立于 unmount） | 概念缺（仅析构 flush） |

> 注：`dir_unlink` 概念已由现有 `dir_rm_dentry(inode_idx,name,destroy_data)` 覆盖，本批不改。

---

## 2. 公共约定（所有接口共享）

- **返回**：一律 `result_code_t`（`RC_OK` / `RC_ERR_*`）。枚举类接口用 `int64_t`
  （`>0` 有条目 / `0` 结束 / `<0` 负错误码），与现有 `root_dir_readdir` 同风格。
- **名字**：`const char*`。创建路径（`dir_insert_dentry`）用 `name_check_create()`
  把关：空名 / 含 `/` → `RC_ERR_NAME_INVALID`；`strlen > RAVENFS_MAX_NAME_LEN=552`
  → `RC_ERR_NAME_LEN`；均在任何 inode/树访问前完成（零副作用）。
  读路径（`dir_lookup` / `dir_rm_dentry`）沿用 `name_len_limiter()`（仅长度，≤552）。
- **inode 定位**：全部用 `uint64_t inode_idx`（52 位有效；`PTR_INVALID` 哨兵永不合法）。
  有效性用 `inode_test()` 判定。
- **"已分配 inode 恒 ≥1 引用"**（不变量 17）：任何接口遇到 `link_count==0` 的已分配
  inode = 内部契约违反 → `RC_ERR_STATE`。
- **破坏性参数**：`destroy_data`（数据块 free 前清零）语义同现有 `dir_rm_dentry`。
- **并发假设**：接口**自身不保证线程安全**（与现状一致）。同一目录的命名空间改操作
  （insert/link/rename/unlink）调用方须串行化；枚举（I4）与 lookup 可并发（读路径）。
  本条为**契约声明**，非实现义务（见 I8 与 §5-D4）。
- **写序总原则**（复用 §10 不变量 20/25 的既有精神）：
  1. 分配类：**位图亮灯 → 数据/节点落盘 → inode 字段翻转（提交点）**；
  2. 删除类：**先删引用（dentry/名）落盘 → 后计数/回收**；永不先制造"计数归零而引用仍在"。
  3. rename/link 这类"多步"操作在 v1（无 journal）下**不保证原子**；写序以"任何中断点
     都落在 fsck 可判定的可恢复态"为准则，具体逐接口给。

---

## I1. `dir_link` — 把名字挂到既有 inode（硬链接概念）

### 概念
一个 inode 可被**多个名字**引用（引用计数 `link_count` 的真正含义）。现有
`dir_insert_dentry` 把"造 inode"和"挂名字"焊死，导致"挂既有 inode"这一能力不存在。
硬链接 = 只做"挂名字"、不动 inode。

### 签名
```cpp
result_code_t dir_link(uint64_t dir_idx, const char *name, uint64_t target_inum);
/* root 薄封装（后续）：root_dir_link(name, target_inum) */

/* 内部建议共用原语（从 dentry_leaf_entry_insert 拆出"挂条目"尾段）：
 *   dentry_leaf_entry_link(leaf_blk, name_cmlpx_t*, idx, result_flags, inum, owner_dir)
 *   —— 长名堆分配 + 树插 + owner 计数；不做 inode_alloc / 不建 dir 树根。
 */
```

### 参数
- `dir_idx`：目标目录 inode（必须 type==DIR、已分配）。
- `name`：新名字。
- `target_inum`：**既有**目标 inode（必须已分配）。

### 前置校验（全部零副作用，按序，任一失败即返回）
1. `name_check_create(name)`：空名 / 含 `/` → `RC_ERR_NAME_INVALID`；超长 → `RC_ERR_NAME_LEN`。
2. `inode_test(dir_idx)` 否 → `RC_ERR_STATE`；读 dir inode，`type!=DIR` → `RC_ERR_STATE`。
3. `inode_test(target_inum)` 否 → `RC_ERR_NOT_FOUND`。
4. 读 target inode：`type==DIR` → `RC_ERR_STATE`（**目录禁硬链接**，§3.3 / 不变量 19）
   〔移植语义：Linux `link(2)` 对目录 `EPERM`〕。
5. `target.link_count < UINT32_MAX` 否 → `RC_ERR_STATE`（溢出防护，§3.3；正常不可达）
   〔待裁决：是否新增 `RC_ERR_OVERFLOW`，见 §4〕。
6. `dentry_Bptree_walk` 查重：命中 → `RC_ERR_EXIST`（不覆盖，§3.3 前置 4）。

### 行为（成功路径）
1. 取 dir 的 dentry 树根（`data_root`；`PTR_INVALID` → `RC_ERR_STATE`，不变量 11）。
2. `dentry_leaf_entry_link(...)`：长名堆（>40B，由 **dir** 分配）→ memmove 树插
   （`inode_ptr.ptr = target_inum`，`free_to_use = name_len`）→ `dir.size++`、
   `dir.link_count = size+2`（§3.3）。
3. 刷新父层 `hash_max`（同 `dir_insert_dentry` 阶段一）。
4. 若叶子溢出 → 分裂递归（同 `dir_insert_dentry` 阶段二）。
5. `target.link_count++`（**引用计数真正的落点**）。
6. 返回 `RC_OK`。

### 失败路径
- **前置校验失败**：零副作用返回对应码。
- **步骤 2–4 中途失败**（分配失败 `RC_ERR_NO_BLOCK` / IO `RC_ERR_IO` / 结构 `RC_ERR_STATE`）：
  RAII 回滚 `heap_guard`（释放本目录名字堆）+ 树插回滚（未提交则 splice 回去）；
  **`target.link_count` 尚未改动**（放在最后一步），故无需回滚计数。
  若回滚不完全（IO 中断）→ 沿用现有哨兵策略：`RC_ERR_NOT_SUPPORTED`，残留由 fsck 收。
- **步骤 5 失败**（inode 写回 IO）：树条目已落盘、计数未增 → 计数虚低；返回 `RC_ERR_IO`，
  fsck 以 dentry 扫描为准重建计数（不变量 21）。**永不返回 `RC_ERR_STATE` 静默**。

### 不变量
- 成功后：`dir.size` 与树内条目数一致；`target.link_count` == 指向它的 dentry 总数。
- 不产生新 inode、不动 `target` 除 `link_count` 外的字段。
- 目录永不被 link 命中（步骤 4 挡住）。

### 移植落点（参照）
概念 → Linux `inode_operations.link`（`vfs_link`）。失败码映射：`EXIST→-EEXIST`、
`NOT_FOUND→-ENOENT`、目录→`-EPERM`。

---

## I2. `dir_rename` — 名字重绑定（换名 / 换父 / 覆盖）

### 概念
一个**既有 inode 的名字**可以在目录树里改变位置：换名（同父）、换父（跨目录）、
覆盖已存在目标；**目录被移动时，它的"父"引用（`..`）必须改写**，且源/目标目录的
条目计数随之变化。Linux VFS 只负责取锁 + 路由，**这些全是 FS 逻辑**。

### 签名
```cpp
namespace ravenfs_rename_flags {          // 自定概念位（勿直接抄 Linux 宏）
    constexpr uint32_t NOREPLACE = 1u;    // 目标存在 → RC_ERR_EXIST，不覆盖
    constexpr uint32_t EXCHANGE  = 2u;    // 互换源/目标两个条目（两者都必须存在）
}
result_code_t dir_rename(uint64_t old_dir, const char *old_name,
                         uint64_t new_dir, const char *new_name,
                         uint32_t flags);
/* root 薄封装（后续）：root_dir_rename(...) —— 单层场景 old_dir=new_dir=root */
```

### 参数
- `old_dir` / `new_dir`：源/目标父目录（各自必须 type==DIR、已分配）。
- `old_name` / `new_name`：源/目标名字。
- `flags`：`NOREPLACE` / `EXCHANGE`（可 0 = 普通可覆盖 rename）。

### v1 受理范围（**先划清，省得实现时发散**）
| 形态 | v1 | 说明 |
|---|---|---|
| 同目录 改名（含覆盖目标） | ✅ | 主场景 |
| 跨目录 移动 普通文件 / 软链 | ✅ | |
| 跨目录 移动 目录（含 `..` 改写 + 两父计数） | ✅ | |
| 覆盖一个**空目录**（源为目录） | ✅ | 非空 → `NOT_EMPTY` |
| 覆盖一个**非空目录** | ❌ | `RC_ERR_NOT_EMPTY` |
| 类型不匹配覆盖（dir↔非 dir） | ❌ | 见错误码；`RC_ERR_TYPE`（拟新增） |
| `EXCHANGE` 同目录 | ⚠️ 可选 | 建议 v1 实现（简单）；跨目录 exchange 延后 |
| `EXCHANGE` 跨目录 / `WHITEOUT` | ❌ | `RC_ERR_NOT_SUPPORTED` |
| 未知 flag 位 | ❌ | `RC_ERR_STATE`（调用方错误，零副作用） |

### 前置校验（全部零副作用，按序）
1. `name_len_limiter(old_name)` / `name_len_limiter(new_name)` → `RC_ERR_NAME_LEN`。
2. `flags` 含未知位 → `RC_ERR_STATE`；含 `WHITEOUT` 概念 → `RC_ERR_NOT_SUPPORTED`。
3. `old_dir`/`new_dir` 合法且 type==DIR → 否 `RC_ERR_STATE`。
4. 源条目存在（walk `old_name`）→ 否 `RC_ERR_NOT_FOUND`。
5. 读源 inode `src_inum` 与其 `src_type`。
6. **同目录同名字（`old_dir==new_dir && 同名`）**：
   - 无 flag → `RC_OK`（no-op，Linux 语义）；
   - `EXCHANGE` → `RC_ERR_STATE`（自己与自己交换，非法）。
7. 目标条目存在性（walk `new_name`）→ `tgt_inum` / `tgt_type`（或不存在）。
8. 按 flag 分流校验：
   - `NOREPLACE` ∧ 目标存在 → `RC_ERR_EXIST`。
   - `EXCHANGE` ∧ 目标不存在 → `RC_ERR_NOT_FOUND`。
   - 目标存在 ∧ 非 `EXCHANGE`：
     - `src_type==DIR ∧ tgt_type!=DIR` → `RC_ERR_TYPE`（拟新增）
     - `src_type!=DIR ∧ tgt_type==DIR` → `RC_ERR_IS_DIR`
     - `tgt_type==DIR`（源亦 dir）∧ 目标目录非空 → `RC_ERR_NOT_EMPTY`
       （空判定：目标 dir 树根为叶子且 `entry_count==0`，同 `dentry_leaf_entry_delete`）。

### 行为（成功路径，非 EXCHANGE）
记 `src` 为源条目承载的 inode（**`link_count` 全程不变**——只是名字搬位置）。

1. **覆盖目标（若存在且非 EXCHANGE）**：等价于对目标做 unlink/rmdir
   （`dentry_leaf_entry_delete` 语义）：
   - 目标 REG/SOFT_SYM：`link_count--`，归零 → `regular_file_reclaim`；
   - 目标空目录：`dir_name_heap_release` → 释放树根 2 块 → `inode_free`；
   - 目标为长名：释放其名字堆（在**目标父目录** `new_dir` 上）。
2. **插入**：在 `new_dir` 以 `new_name` 挂 `src_inum`（复用 I1 的 `dentry_leaf_entry_link`
   机械，长名堆由 `new_dir` 分配）。树结构维护（父 `hash_max` 刷新 / 叶子分裂递归）。
3. **删除**：从 `old_dir` 删 `old_name` 条目（树删 + `old_dir` 名字堆释放 + 树结构维护
   `hash_max`/欠载平衡递归）。
4. **目录 `..` 改写**：若 `src_type==DIR ∧ old_dir!=new_dir` → `dir_set_parent(src_inum, new_dir)`
   （内部原语，见 I3）。
5. **计数**：
   - `old_dir.size--`、`old_dir.link_count = size+2`（删条目的后果）；
   - `new_dir.size++`、`new_dir.link_count = size+2`（插条目的后果；若覆盖则先后抵消）；
   - `src_inum`: **不变**（名字数不变）。
6. 返回 `RC_OK`。

### 行为（EXCHANGE，v1 若实现）
两条目必须存在；**交换两条目承载的 inode 指针**（名字/长名堆归属**不动**，因为它们随
(dir,name) 绑定而非随 inode）：
- `entryA.inode_ptr.ptr ↔ entryB.inode_ptr.ptr`（`free_to_use=name_len` 各自保留）；
- `link_count` 两边均不变；
- 若 `old_dir!=new_dir` 且其中一方为目录：改写该目录的 `..`（`dir_set_parent`），
  两父 `size` 不变（条目数不变）；
- 类型不限（EXCHANGE 允许异型）。

### 失败路径与写序（无 journal，v1 准则 = 任何中断点可 fsck 判定）
**推荐写序**（非 EXCHANGE）：
```
(0) 全部前置校验         —— 零副作用，失败即返回
(1) 覆盖：删目标条目      —— 与 unlink 同序（先删引用）
(2) 插入：new_dir 挂 src  —— 
(3) 删除：old_dir 摘源    —— 
(4) 目录：src 的 .. 改写
(5) 计数维护（old/new size、link_count）
```
各中断窗的可恢复性：
| 中断点 | 盘上状态 | fsck 判定/修复 |
|---|---|---|
| (1) 后 / (2) 前 | 目标已消失，源仍在旧名 | 等价"目标被删"；合法，无需修 |
| (2) 后 / (3) 前 | **两名指向同一 inode**（暂态） | 等价硬链接；合法；rename 未完成但无害 |
| (3) 后 / (4) 前 | 目录已在新名，其 `..` 仍指旧父 | `..` 与树拓扑不符 → fsck 依 `parent_of` 重建 `..` |
| (4) 后 / (5) 前 | 结构对，计数偏 | fsck 以 dentry 扫描重建 `link_count`（不变量 21） |

**内存失败（分配/IO 中途）**：尽力回滚（步骤可逆：删除类用守卫、插入类用 splice 回滚），
回滚不完全 → `RC_ERR_NOT_SUPPORTED` 哨兵 + 残留孤儿/重复由 fsck 收（与 I1 同策略）。
**不承诺**多步原子；这是 v1 无 journal 的既定边界（§5 待裁决 B）。

### 不变量
- 成功后两目录 `size`/`link_count` 与各自树条目数一致；
- 每个目录仍恰有一个父（`..` 自洽）；
- 不产生新 inode、不产生悬挂引用（`src_inum` 始终已分配）。

### 移植落点（参照）
概念 → Linux `inode_operations.rename`（`vfs_rename` / `struct renamedata`）。
`NOREPLACE/EXCHANGE` 与 Linux `RENAME_NOREPLACE/RENAME_EXCHANGE` 语义对应（概念一致，
命名自有）；`WHITEOUT` 不支持 → `-EINVAL`。

---

## I3. `dir_get_parent`（+ 内部 `dir_set_parent`）

### 概念
目录的**唯一父**是结构不变量（树的骨架）。它存于目录自身：dentry 树根叶子
`meta.ptrs[1]`（`..`）；根目录自指。已有存储、**无原语**——rename 需要写它，测试/审计需要读它。

### 签名
```cpp
result_code_t dir_get_parent(uint64_t dir_idx, uint64_t *out_parent);
/* 内部（不对外，rename 等使用）：
 *   result_code_t dir_set_parent(uint64_t dir_idx, uint64_t new_parent);
 */
```

### 行为 `dir_get_parent`
1. `inode_test(dir_idx)` ∧ `type==DIR` → 否 `RC_ERR_STATE`。
2. 取 `data_root`（树根块号）；`PTR_INVALID` → `RC_ERR_STATE`（不变量 11）。
3. 读树根节点 `meta`（mdata_piece_session），取 `ptrs[1].ptr` → `*out_parent`。
   - 根目录：`ptrs[1].ptr == 自身 inum`（约定，见 `dentry_leaf_entry_insert` mkfs 对齐）；
     直接输出自身。
4. `RC_OK`。

### 行为 `dir_set_parent`（内部）
1. 同上定位树根。
2. 写 `ptrs[1].ptr = new_parent`；`dirty()`。
3. 失败：`RC_ERR_IO`/`RC_ERR_STATE`。
（注：`parent_ptr.ptr` 是 `.` 自指，**不随 rename 改**；只有 `..` 改。）

### 失败路径
- 非目录 / 未分配 → `RC_ERR_STATE`；读 IO 失败 → `RC_ERR_IO`；树根 magic 不符 → `RC_ERR_STATE`。

### 不变量
- 非根目录的 `ptrs[1]` 指一个**已分配且为 DIR** 的 inode（rename 后校验）；
  根目录 `ptrs[1] == 自身`。

### 移植落点（参照）
Linux 用 dcache `d_parent` 解析 `..`，通常**不需要** FS 提供读父；但**写** `..` 是
rename 的 FS 义务。`dir_set_parent` = rename 内部步骤 (4)。

---

## I4. `dir_enum_open` / `dir_enum_next` — 可续目录游标

### 概念
目录枚举的本质是**可分页续读**（用户态 `readdir` 靠 cookie 续读），不是一次性快照数组。
当前 `(base,count)` 是快照切片，缺"位置 token"。`. / ..` 是**合成**的（不落盘，§6.4）。

### 签名
```cpp
/* 打开一次枚举：校验目录 + 初始化游标（不持有任何状态；游标即全部上下文） */
result_code_t dir_enum_open(uint64_t dir_idx, uint64_t *out_cursor);

/* 取下一个条目：1=有条目(out 已填, cursor 前进)；0=枚举结束；<0=负错误码 */
int64_t dir_enum_next(uint64_t dir_idx, uint64_t *cursor, dentry_out *out);
```

### 游标模型
- **不透明** `uint64_t`；调用方不得解释（**参考编码**：`bits[0..1]` tag 0=起点 /
  1=已发"." / 2=已发".." / 3=树内；tag==3 时 `bits[2..8]=槽号(0..126)`、
  `bits[9..]=叶子块号`）。实现可自由改编码，只要满足行为。
- **无状态**：`dir_enum_next` 每次凭游标重新定位叶子（读路径），跨调用不持锁/不持内存。
- **稳定性**：目录并发修改下游标行为**未定义**（允许跳/重，同 Linux readdir 语义）；
  实现可校验叶子 magic，失效 → `RC_ERR_STATE`（或按"重扫该叶"容错，二选一，见 §5-D5）。

### 行为
1. `dir_enum_open`：校验目录（同 lookup 的目录检查）→ `*out_cursor = 0` → `RC_OK`。
2. `dir_enum_next`：
   - tag==起点 → 发 `.`（`inum=dir_idx`, `name="."`）→ 游标进 tag1；
   - tag==1 → 发 `..`（`inum=dir_get_parent(dir_idx)`, `name=".."`）→ 游标进 tag2；
   - tag==2 → 定位**最左叶子** slot0，开始发树内条目（hash 序 = DFS 叶链序）；
   - tag==3 → 从游标槽续发；到叶尾 → 沿 `ptrs[2]`（后继叶）跳，直到最后一叶末 → 返回 0；
   - 每发一条：填 `dentry_out{ inum=inode_ptr.ptr, name_len=free_to_use, character=名字 }`
     （长名读名字堆拼全名），游标前进，返回 1。
   - 空目录：发完 `.`/`..` 即返回 0。

### 失败路径
- 非目录 / 未分配 → 负码 `RC_ERR_*`（`dir_enum_open` 同）。
- 叶 magic / 兄弟链损坏 → `RC_ERR_STATE`（负）；IO 失败 → `RC_ERR_IO`（负）。
- 结束时返回 `0`（**不是错误**）。

### 不变量
- 枚举序 == DFS 叶链序 == hash 序（与 §6.4「hash 乱序输出」一致，接受）。
- `.`/`..` 永不从树内读出（树里本就没有）；恒在最前两条，且各恰一次。

### 移植落点（参照）
概念 → Linux `file_operations.iterate_shared` + `dir_context.pos`（游标）+ `dir_emit` /
`dir_emit_dots`。`dir_enum_next` 的 1/0 与 `filldir` 的 true/false 同构；
`dir_enum_open` 无需对应（VFS 初始化 pos=0）。

---

## I5. `symlink_set_target` / `symlink_get_target`

### 概念
symlink 是一种 inode，其**内容是"另一个路径字符串"**，解析时被替换。当前只有空
`SOFT_SYM` inode，没有"写/读/解析 target"。载荷存储**复用文件数据三态**（§3.2）：
`size = target 字节数（不含 NUL）`；≤160 内联，>160 直指/区段树。

### 签名
```cpp
result_code_t symlink_set_target(uint64_t inum, const char *target, uint64_t len);
result_code_t symlink_get_target(uint64_t inum, char *buf, uint64_t cap, uint64_t *out_len);
```

### 参数
- `inum`：必须已分配且 `type==SOFT_SYM`。
- `target`/`len`：目标路径串与字节数（**不含 NUL**）。`len==0` 允许（空 target；受理性
  由上层决定，linux 侧 `symlink("")` 通常是 `-ENOENT`，属平台层）。
- `buf`/`cap`/`out_len`：读缓冲 / 容量 / 实际读回长度。

### 行为 `symlink_set_target`
1. 校验 `inum` 已分配 ∧ `type==SOFT_SYM` → 否 `RC_ERR_STATE`。
2. 长度上限：`len > RAVENFS_MAX_SYMLINK_LEN` → `RC_ERR_NAME_LEN`（**待裁决**上限取值，见 §5-D4b；
   建议 4095，或暂定"受数据三态能力约束，无额外上限"）。
3. 覆写语义 = **truncate 到 0 + pwrite(target, 0, len)**：
   - 直接复用 `direct_file_trunacate(inum, 0)` + `direct_file_pwrite(inum, 0, len, target)`
     （数据面机械已具备三态迁移/物化）；
   - `size` 由 pwrite 维护 = `len`（§3.2 软链载荷约定）。
4. `RC_OK`。

### 行为 `symlink_get_target`
1. 校验 type==SOFT_SYM → 否 `RC_ERR_STATE`。
2. `direct_file_pread(inum, 0, min(cap, size), buf, out_len)`（EOF 短读语义）。
3. `RC_OK`；`*out_len` = 实际字节数。`buf` 不含 NUL（调用方按 `out_len` 定界）。

### 失败路径
- 非软链 → `RC_ERR_STATE`；超长 → `RC_ERR_NAME_LEN`；
- 数据面失败（`RC_ERR_NO_BLOCK`/`RC_ERR_IO`/`RC_ERR_STATE`）原样上传。

### 不变量
- 软链载荷与普通文件共用 `size`+三态不变量（§3.2 / 不变量 22–25）；
- `symlink_get_target` 读回字节 == `symlink_set_target` 写入字节（round-trip）。

### 移植落点（参照）
概念 → Linux `inode_operations.symlink`（建）+ `get_link`（解）。fast/slow 之分 =
"载荷在 `inline_data` 还是数据区"——**由 len 自然决定**，上帝类无需区分；移植时再按
Linux 惯例拆 fast（`i_link`）/slow（`page_symlink`）。

---

## I6. `inode_getattr` / `inode_setattr`

### 概念
- **属性读**：从 inode 读出一组标准属性（纯函数，不改盘）。
- **属性写**：**元数据**（权限/属主/时间）与 **size（数据面）是两个概念**，必须分家。
  size 走既有 `direct_file_trunacate`；本接口只做元数据。

### 结构体
```cpp
struct inode_attr_t {          /* 属性快照（读） */
    uint8_t  type;             /* INODE_TYPE_* */
    uint32_t uid, gid;
    uint64_t permission_bits;
    uint64_t size;
    uint32_t link_count;
    uint64_t blocks;           /* 占用 FS_block 数（可由形态+size 现算） */
    uint64_t atime, mtime, ctime;   /* 秒（epoch） */
    uint32_t a_ns, m_ns, c_ns;      /* 纳秒（对应 *_ns_stamp 字段；语义待裁决 §5-D2） */
};
struct inode_setattr_t {       /* 属性变更（写；同时给出"改哪些"的位图） */
    uint32_t valid;            /* 位图：MODE|UID|GID|ATIME|MTIME|CTIME */
    uint64_t permission_bits;
    uint32_t uid, gid;
    uint64_t atime, mtime, ctime;
    uint32_t a_ns, m_ns, c_ns;
};
```

### 签名
```cpp
result_code_t inode_getattr(uint64_t inum, inode_attr_t *out);
result_code_t inode_setattr(uint64_t inum, const inode_setattr_t *in);
```

### 行为 `inode_getattr`
1. `inode_test(inum)` → 否 `RC_ERR_NOT_FOUND`。
2. 按 type 读字段，填 `out`（`size`/`link_count`/`uid`/`gid`/`permission_bits`/时间）。
3. `blocks` 现算（inline=0；direct=`ceil(size/4096)`；extent=叶链记录 length 求和；
   dir=树节点块数）。**只读不 dirty**。

### 行为 `inode_setattr`
1. `inode_test(inum)` → 否 `RC_ERR_NOT_FOUND`。
2. **拒绝 `valid` 含 SIZE**（size 属数据面，走 `direct_file_trunacate`）→ `RC_ERR_STATE`。
3. 按 `valid` 逐字段覆盖（MODE→`permission_bits`；UID/GID；ATIME/MTIME/CTIME→秒+纳秒）。
4. 元数据变更是否触发 **ctime 自动推进** = 语义决策（§5-D1）：建议"任何元数据写 →
   ctime 推进"，与 Unix 一致。
5. `dirty()`；`RC_OK`。

### 失败路径
- 未分配 → `RC_ERR_NOT_FOUND`；`valid` 非法/含 SIZE → `RC_ERR_STATE`；IO → `RC_ERR_IO`。

### 不变量
- 读不改变盘上任何状态；
- 写只碰元数据字段，**永不碰 `size`/`data_root`/形态位**。

### 移植落点（参照）
概念 → Linux `inode_operations.getattr`（配 `generic_fillattr`）/ `setattr`（配
`setattr_prepare`+`setattr_copy`）；`valid` 位图 ↔ `iattr.ia_valid`/`ATTR_*`。

---

## I7. `fs_statfs`

### 概念
卷级空间统计（空闲块 / 空闲 inode）。位图已在内存（`bitmap_mgr` 每片 `free_count`），纯汇总。

### 签名
```cpp
struct fs_statfs_t {
    uint64_t total_blocks;    /* FS_block 总数（含元数据区） */
    uint64_t free_blocks;     /* 空闲 FS_block（block bitmap 0 位计数） */
    uint64_t total_inodes;
    uint64_t free_inodes;     /* inode bitmap 0 位计数（含保留槽） */
    uint32_t block_size;      /* 4096 */
    uint32_t name_len_max;    /* RAVENFS_MAX_NAME_LEN = 552 */
};
result_code_t fs_statfs(fs_statfs_t *out);
```

### 行为
1. 未挂载（`fd<0`/`!is_mounted()`）→ `RC_ERR_OPEN`。
2. `total_blocks = sb.total_blocks`；`free_blocks = Σ 各片 free_count`（需 `bitmap_mgr`
   暴露汇总，**新增一个只读汇总方法**）。
3. inode 侧同理（`total_inodes = sb.inode_array_count`；`free_inodes = Σ`）。
4. `block_size=4096`、`name_len_max=552`。
5. `RC_OK`。

> 语义：元数据区在 bitmap 里恒为 1（已占），故 `free_blocks` 天然不含结构性占块——
> 与 ext2 `f_bfree` 语义一致（`f_blocks` 含全部，`f_bfree` 排除元数据）。

### 失败路径
- 未挂载 → `RC_ERR_OPEN`；`out==nullptr` → `RC_ERR_STATE`；`bitmap_mgr==nullptr` → `RC_ERR_STATE`。

### 移植落点（参照）
概念 → Linux `super_operations.statfs`（`struct kstatfs`）。`f_bfree/f_ffree` 直接来自本接口。

---

## I8. `fs_sync` — 持久化/同步边界

### 概念
独立于 unmount 的"把内存态落到盘"的**同步点**。当前只有析构 flush → 挂载态掉电丢
位图更新。`fs_sync` 让"此刻起已落盘"成为**可声明的承诺**。

### 签名
```cpp
result_code_t fs_sync(uint32_t flags);   /* flags 预留（暂 0） */
```

### 行为（按后端模式分派，模式由 ctor 决策树定）
1. 未挂载 → `RC_ERR_OPEN`。
2. 缓存层：`self_reliat` → `flush_all()`（写回脏页）；`no_cache` → 无需（session 直写）
   → 跳过；`offload`（mmap）→ `msync(canvas)`。
3. **位图**：本批语义 = `fs_sync` **包含位图写回**（否则"已落盘"不成立）：
   - 直写模式：全量 `pwrite` 位图（同析构路径）；**顺序：先元数据/数据，后位图**
     （避免"块标 free 而仍被引用"的危险窗口）。
   - mmap 后端：由内核页缓存管，no-op。
4. **写序承诺**：返回 `RC_OK` 时，截至调用点已完成的所有元数据/数据修改 + 分配位图
   **均已落盘**（崩溃在其后不再丢这些修改）。
5. `RC_OK`；任一 IO 失败 → `RC_ERR_IO`（部分成功已落盘，不承诺回滚）。

### 失败路径
- 未挂载 → `RC_ERR_OPEN`；IO → `RC_ERR_IO`。

### 移植落点（参照）
概念 → Linux `super_operations.sync_fs` + `file_operations.fsync`（inode 级）。
本接口是"卷级同步"；inode 级 fsync 后续可另加（本批不做）。

---

## 4. 返回码：复用 / 拟新增

**复用（既有）**：`RC_OK`、`RC_ERR_NOT_FOUND`、`RC_ERR_EXIST`、`RC_ERR_NAME_LEN`、
`RC_ERR_NO_BLOCK`、`RC_ERR_NO_INODE`、`RC_ERR_IO`、`RC_ERR_STATE`、
`RC_ERR_NOT_EMPTY`、`RC_ERR_IS_DIR`、`RC_ERR_NOT_SUPPORTED`。

**拟新增（待审计批准）**：
| 码 | 用途 | 备注 |
|---|---|---|
| `RC_ERR_TYPE` | 类型不匹配（rename 覆盖 dir↔非 dir） | 现有 `IS_DIR`/`NOT_EMPTY` 表达不全 |
| `RC_ERR_OVERFLOW`（可选） | `link_count` 溢出 | 正常不可达；也可用 `RC_ERR_STATE` |

> 若不加新码，退化方案：类型不匹配用 `RC_ERR_IS_DIR`/`RC_ERR_STATE`，溢出用 `RC_ERR_STATE`。
> **请审计时拍板**。

---

## 5. 待裁决项（实现前必须定）

| | 项 | 影响 | Raven 建议 |
|---|---|---|---|
| **D1** | 时间戳**更新触发点**（哪些操作动 mtime/ctime/atime；`setattr` 是否推 ctime） | I6 + 全网语义 | 先只做"C1 读 / C2 写"接口，**触发点**单独一表后补；本批 setattr 只做"显式写" |
| **D2** | 时间戳**字段语义**：`{mtime:u64, m_ns_stamp:u32}` 是否为 `{秒,纳秒}`？ | I6 结构体 | 认定为 `{秒, 纳秒}`；请确认 |
| **D3⚠️** | **目录 `link_count` 语义冲突**：RavenFS 用 `size+2`（=**条目数**+2）；Linux 惯例 dir nlink = `2 + 直接子目录数`（不含普通文件） | rename/unlink/link + getattr + `ls -l`/`find -noleaf` | 见下方专段 |
| **D4** | `dir_rename` **原子性边界**：接受"尽力回滚 + fsck 收"（无 journal） | I2 | 接受（与现有 unlink 同策略），但写序按 §I2 表 |
| **D4b** | `symlink` target **长度上限** | I5 | 暂定 4095，或"无额外上限" |
| **D5** | readdir 游标**稳定性**：失效时"报错" vs "容错重扫" | I4 | 选"容错重扫该叶"（更接近 readdir 直觉） |
| **D6** | rename 是否**预算化分裂节点**（避免中途 ENOSPC 破坏原子性） | I2 | 建议借鉴 extent 侧预算池：new_dir 插入前预分配 `depth+1` 节点 |

### D3 详述：目录 nlink 语义冲突
- **现状**（§3.3）：`link_count = size + 2`，`size` = 全部条目数（**文件+子目录**）。
- **Linux 惯例**：目录 `i_nlink = 2 + 直接子目录数`（`. `+ 父中名字 = 2，每个子目录的
  `..` 再加 1；**普通文件不计**）。用户态 `ls -l` 显示、`find` 的 `-noleaf` 优化都依赖它。
- **两条路**：
  - **(a) 保持盘上 `size+2`**，`inode_getattr` 在报告时**另算** Linux 风格 nlink（需扫
    该目录 dentry 树统计子目录数——O(树)，仅在 stat 时付代价）。盘面不动，fsck 规则不动。
  - **(b) 改盘上语义**为 `2 + 子目录数`——影响 §3.3 契约、mkfs、所有计数点，**代价大**。
- **倾向 (a)**：盘面 `size+2` 是"目录项计数"的自然选法（它同时服务 `size` 与 rmdir 判定），
  不必为满足 Linux 显示而扭曲；把"Linux 风格 nlink"降级为 **getattr 的报告层换算**。
- ⚠️ 若选 (a)，需明确：`inode_getattr` 返回的 `link_count` 是**盘上值**还是**报告值**——
  建议结构体加区分（或 `inode_getattr` 返回报告值、审计另走 `audit_inode` 看盘上值）。

---

## 6. 内部机械分解建议（供实现，非契约）

- **`dentry_leaf_entry_insert` 拆两段**：
  - `dentry_leaf_entry_new`（现有：造 inode + 建 dir 树根 + 挂名）——供 `dir_insert_dentry`；
  - `dentry_leaf_entry_link`（新：只挂名到给定 inum）——供 I1 `dir_link` 与 I2 的"插入"步。
  两段共用尾段（名字堆 + 树插 + owner 计数 + flags）。
- **`dir_rename` 组合现有机械**：`dentry_leaf_entry_delete`（覆盖目标）+ 新 link 尾段（插入）
  + 现有树结构维护（hash_max 刷新 / split / balance，均已在 insert/rm 里成对存在）
  + 新 `dir_set_parent`。
- **枚举**：借 dentry 树叶子链（`ptrs[1]` 前驱 / `ptrs[2]` 后继）游走；最左叶下钻同
  `extent` 侧"最左叶"套路（现有代码有现成写法）。

---

## 7. 测试建议（每接口正/负/边界，实现时落 tests/）

| 接口 | 正 | 负 | 边界 |
|---|---|---|---|
| I1 `dir_link` | 同/跨目录 link，别名可 lookup，`link_count` 递增 | 名字已存在→EXIST；目标未分配→NOT_FOUND；目标为 dir→STATE；溢出 | 长名 link；多次 link 后 unlink 非归零不回收 |
| I2 `dir_rename` | 同目录改名；跨目录移动文件；移动目录后 `..` 正确；覆盖空目录 | 源不存在→NOT_FOUND；NOREPLACE 撞目标→EXIST；覆盖非空目录→NOT_EMPTY；未知 flag→STATE | 同目录同名 no-op；移到自身子目录（成环）→ 应拒绝（**新增校验**：new_dir 是 src 子树判定，见 §8） |
| I3 `dir_get_parent` | 根自指；子目录指向父 | 非目录→STATE | rename 后重读 |
| I4 `dir_enum_*` | 空目录（`.`/`..` 两条）；非空全枚举；续读游标 | 非目录→负码 | 长名条目；分裂后重开；逐条 `readdir` 与全量一致 |
| I5 `symlink_*` | ≤160 内联 / >160 跨态 target round-trip | 非软链→STATE；超长→NAME_LEN | 空 target；恰好 160 边界 |
| I6 `inode_*attr` | 读写 mode/uid/gid/times；读为纯函数 | 未分配→NOT_FOUND；valid 含 SIZE→STATE | 三型 inode 的 blocks 现算 |
| I7 `fs_statfs` | 空闲数随 alloc/free 变化 | 未挂载→OPEN | 充满后 free=0 |
| I8 `fs_sync` | 调用后重挂载可见修改；位图落盘 | 未挂载→OPEN | 各后端模式（no_cache/self_reliat/offload）分别验 |

### §8 补：rename 成环防护（新增校验，容易忘）
移动目录时若 `new_dir` 位于 `src` 子树内 → 产生环（目录图不连通/自引用）。
**必须前置拒绝**：从 `new_dir` 沿 `..`（`dir_get_parent`）上溯，若遇 `src_inum` → 拒
（`RC_ERR_STATE`，或拟新增 `RC_ERR_INVAL`）。Linux VFS 在 `lock_rename` 层做了一部分，
但 **FS 侧仍应自防**（尤其跨目录 rename 的 `..` 改写前提）。这是 I2 实现里**最易漏**的一步。

---

## 9. 冻结清单（审计通过后视为本端契约）

- [ ] I1–I8 签名与语义
- [ ] §4 新增返回码（`RC_ERR_TYPE` / 可选 `RC_ERR_OVERFLOW`）
- [ ] §5-D1 时间戳触发点（是否本批做）
- [ ] §5-D2 时间戳字段语义确认
- [ ] §5-D3 目录 nlink：选 (a) 报告层换算 / (b) 改盘
- [ ] §5-D4b symlink 长度上限
- [ ] §5-D5 readdir 游标失效策略
- [ ] §5-D6 rename 预算化分裂
- [ ] §8 rename 成环防护

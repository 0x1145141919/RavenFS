# M5 dentry B+树「写入」god→ko 移植规格（插入 / 分裂 / 再平衡 / 删除）

> 2026-09-27 · 为 **M5（命名空间写：dentry 树插入/分裂/再平衡/删除）** 产出的精确移植规格。
> **只读 god（v1.1 盘面）产「行为契约」，不是代码**；ko 用 v1.2 盘面（`include/disk_struct_v1.2.h`），语义同构。
> 真值源：`juvenile_god/dentry_split.cpp`、`dentry_balance.cpp`、`dentry_insert_delete.cpp`、
> `dentry_walk_lookup.cpp`、`root_dir.cpp`（驱动循环）、`audit.cpp`（不变量校验）；盘面 `Docs/disk_layout_v1.1.md` §4/§6/§10。
> 行号引用格式 `文件:NN`（缩写：`walk`=dentry_walk_lookup.cpp `split`=dentry_split.cpp `bal`=dentry_balance.cpp
> `id`=dentry_insert_delete.cpp `rd`=root_dir.cpp `aud`=audit.cpp）。
> **生殖隔离**：god 的 `mdata_piece_session`/`InodeSession`/RAII guard ↔ ko 的 `RAVENFS_MSESSION`/`RAVENFS_SESSION`/`__free`+goto（§E）。

---

## A. 节点元数据契约

### A.1 `Bptree_meta_t`（64B 头，叶/内通用）

```
magics[4] (32B) | parent_ptr_t (8B) | ptrs[3] (24B)
```

v1.2 形式：`uint64_t magics[4]` + `struct parent_ptr_t{uint64_t v}` + `struct ptr_in_idx ptrs[3]{uint64_t v}`
（位域→掩码+宏 `RFS_PP_*` / `RFS_PTR_*`；`include/disk_struct_v1.2.h`）。尺寸 `RFS_STATIC_ASSERT == 64`。

### A.2 `parent_ptr_t` 四态（`layout_v1.1 §2.2`）

位域：`entry_count:10 (bit0..9) | is_inner_node:1 (bit10) | is_root_node:1 (bit11) | ptr:52 (bit12..63)`。

| 状态 | entry_count | is_inner_node | is_root_node | ptr |
|---|---|---|---|---|
| 叶 · 根 | 0..126（可 0=空目录） | 0 | 1 | **自身 inode 编号**（`.`） |
| 叶 · 非根 | 0..126（**≥63**，见 §C 阈值） | 0 | 0 | **父节点块号** |
| 内 · 根 | 1..126（**恒 ≥2**，==1 待收缩） | 1 | 1 | **自身 inode 编号**（`.`，非块号！） |
| 内 · 非根 | 63..126 | 1 | 0 | **父节点块号** |

- **根的 `ptr` 恒 = 该目录 inode 编号**（`.`），叶/内一致；**非根 `ptr` = 父节点块号**（是块号，不是 inode）。
- `entry_count` 是节点有效 entry 数**唯一真值**（替代 v1 node_meta）。上限 126。
- god 位域读出的 `ptr` 是 **52 位形态**；与 `PTR_INVALID`（64 位全 1）比较**必须同截断**
  （`PTR_INVALID_52 = PTR_INVALID & ((1ULL<<52)-1)`，如 `split:77`、`bal:34`、`id:242`）；v1.2 用 `RFS_PTR_ADDR/RFS_PP_PTR` 直接给 52 位值。

### A.3 `ptrs[3]` 按角色解码（`layout_v1.1 §4.1`，四态）

| 角色 | ptrs[0] | ptrs[1] | ptrs[2] |
|---|---|---|---|
| 非根叶子 | 预留 = PTR_INVALID | **前驱叶块号**（prev；首叶 = INVALID） | **后继叶块号**（next；尾叶 = INVALID） |
| 非根内部 | 预留 = INVALID | 预留 = INVALID | 预留 = INVALID |
| 根叶子 | 预留 = INVALID | **`..` inode** | 预留 = INVALID |
| 根内部 | 预留 = INVALID | **`..` inode** | 预留 = INVALID |

- 哨兵 = `PTR_INVALID`（= `~0ULL`，落盘 52 位全 1 = `RFS_MASK52`）。**0 是合法块号/inode，禁止当「空」用**。
- 这些 ptrs 的 `free_to_use`（ptr_in_idx 低 12 位）god 恒显式写 0（`split:88-94` 等）。
- 内部节点**无最左子树**：上界路由下 `N hints = N children`，无孤儿子树（`layout §4.1`）。
- 建根/升根时 `..` 的来源：`mkfs build_root_dentry_node`（`mkfs/mkfs.cpp:305`）根叶 `ptrs[1]=root_ino`；
  子目录建根 `id:82` `ptrs[1]=owner_dir_inode`。**降根**（root_shrink）`bal:379` 把 `..` 写回叶子 `ptrs[1]`。⚠ **升根路径 miss（见 B.6 坑）**。

### A.4 hint 语义 + 数组阶

- `Dentry_hint_t`：`sub_ptr: ptr_in_idx` + `hash_max: u64`。`hints[i].hash_max = 子树 i 的最大 hash`（上界）。
- 内部 `hints` 按 `hash_max` **严格递增**（`aud:507` 校验 `i>0 && key<=prev_key → BAD`）。
- 数组阶 = **126**，数组声明 = 阶+1：叶 `entries[127]`、内 `hints[127]`；索引 `[126]` = **插入瞬时槽，仅内存、禁落盘**
  （误落盘破坏 count 语义；`layout §4.2`、`aud:316` 校验 `entry_count>126 → BAD`）。
- `Dentry_t`（64B）= `inode_ptr: ptr_in_idx`（`free_to_use = name_len`，全名长）+ `hash: u64` +
  `full_char_remain_ptr: u64`（>40B 外置定位，否则 INVALID）+ `name[40]`。
  叶 64 + 127×64 = 8192；内 64 + 127×16 + 762×8 = 8192。

---

## B. 插入路径（含分裂）

### B.1 walk（`walk:44` `dentry_Bptree_walk`）— 下行 + 定位推导

- 从根（`root_inode.data_root`）逐层下行，独立会话每层。校验：第 0 层必 `is_root=1`，后续层必 `is_root=0`。
- **内部路由 = 最小上界（lower_bound）**：找**第一个 `hash_max[i] >= target`** 的 hint（`walk:184` `dentry_inner_node_lookup`，`target ≤ hash_max[i] → sub_ptr[i]`，`layout §6.2`）。
- **击穿特化**（`walk:80`）：若 `target > 全部 hash_max`（返回 0xFFFF），**不是错误** → `hint_idx = entry_count-1`（最右 hint）下钻到最右叶。语义：新条目是全局最大，归宿 = 最右叶末尾。
- 记录 `trace.blocks[depth]`、`trace.child_idx[depth]`（内层 = hint idx；叶层 = GLB）；`trace.depth` = 层数；`trace.is_leaf_hit`。
- ⚠ **dentry 是 `≥`（含等号），extent 是 `>`（半开区间 key）——不可互抄**（`walk:8-20`）。
- ⚠ ko 现有只读 `ravenfs_dir_find`（`linux/dentry.c:155`）把击穿当 **miss**（只读正确）→ **M5 插入必须改成 god 的「击穿续钻最右叶」**。

### B.2 叶内 GLB（`walk:142` `dentry_leaf_node_lookup`）

- 复合序 `(hash, name)` 下一次二分，返回 `greates_lower_bound_idx` = **最大下界 GLB**（最后一个 ≤ target 的条目）；
  `DENTRY_LEAF_GLB_NONE = 0xFFFF` = **无下界**（空叶 / 所有条目 > target）。
- 命中判定：仅 GLB 条目 `hash==target && dentry_name_cmp==0`。
- **插入位 `idx`**：`glb == NONE → idx = 0`（最左）；否则 `idx = glb + 1`（插 GLB 之后）— `rd:88`。
- hash 冲突：同 hash 条目叶内连续；比较降级 `dentry_name_cmp`（`walk:126`；>40B 自动读名字堆外置尾段，读堆失败保守返 1）。

### B.3 叶条目插入（`id:38` `dentry_leaf_entry_insert`）

前置：`idx` 已由 walk 推导；**调用前已查重**（`trace.is_leaf_hit → RC_ERR_EXIST`，`rd:81`，省 inode/堆/树根）。
下游副作用（**「插条目即建下游」**）：① 分配+初始化目标 inode（DIR 急建空根叶+dir inode `size=0/lc=2/10 堆哨兵`；REG/SOFT_SYM 内联空 `lc=1/data_root=INVALID`）；
② 长名 >40B 走 owner 名字堆；③ memmove 插入 `entries[idx]`（`hash`、`inode_ptr.free_to_use=name_len`、`inode_ptr.ptr=inum`、name 拷 `min(len,40)` 余零填、`full_char_remain_ptr`）；
④ owner 目录计数 `size++`、`link_count = size+2`。

**result_flags**：
- `bit0 = 1` → 需分裂（插入后 `count > 126`，即 `count == 127`）。
- `bit1 = 1` → 叶子最大 hash 已变、父层 hint 需刷新：`count>1` 时 `(idx==count-1 || new_max != old_max)`；`count==1`（0→1）恒置。
  ⚠ 判定用的是 **++ 后**的 `count`（`id:176`）。

### B.4 驱动阶段一：父层 hash_max 刷新（`rd:90`，无分裂时）

沿 `trace` **自底向上**：读 parent（读-改-写）+ cur（只读）取 `node_max_hash`；若 `new_max==old_max` **立即 break**（短路，上溯无感）；
否则写 `parent->hints[child_idx].hash_max = new_max`、`dirty()`，`cur=parent` 继续。

### B.5 叶分裂（`split:49` `dentry_leaf_split`）

- **触发**：`count == 127`（`count <= 126 → RC_ERR_STATE`）。
- **均分**：左（victim）**63**，右（new）**64**（`copy_count = count - 63 = 64`）。
- **新叶分配**：`block_alloc_hard(2, 1)` → **2 个 FS_block（8K），`2^1` 对齐**（8K 对齐）。
- **叶链维护**（`split:88-120`）：
  - `new.ptrs[1] = victim_blk`（prev）；`new.ptrs[2] = victim_is_root ? INVALID : old_next`；
  - `victim.ptrs[2] = new_blk`；若 `old_next` 存在 → `old_next.ptrs[1] = new_blk`。
  - **根叶分裂**：victim 原为根（`ptrs[1]=".."`）→ 过渡为链表成员，`victim.ptrs[1] = INVALID`（无前驱）。
  - ⚠ **`old_next` 必须在改写 `victim.ptrs[2]` 之前读**（否则读到 `new_blk`；`split:78`）。
- **`low_node_maxhash = victim->entries[62].hash`**（左半 63 项的最大 hash；`split:128`）。
- **不动父层**；`memcpy` 右半 `entries[63..126]` → 新叶 `[0..63]`；写回 victim+new（`dirty()`）。

### B.6 根升长 / 内分裂 / 父层应用

- **情况 A（`rd:120`）**：`trace.depth == 1`（根就是被裂的叶）→ `dentry_new_root_inner(low, low_max, high, inode_idx)`。
- **情况 B（`rd:133`）**：非根叶 → `dentry_spilt_parent_apply(parent, child_idx, res, &need)`，再 `while(need)` 递归向上。

**`dentry_spilt_parent_apply`（`split:223`）**：
1. 读父（校验内 + `hints[victim_id].sub_ptr.ptr == low_blk`）；
2. 读右子取 `high_max`（内→`hints[cnt-1].hash_max`，叶→`entries[cnt-1].hash`）；
3. `parent.hints[victim_id].hash_max = low_max`；
4. `insert_pos = victim_id+1`，memmove 后插新 hint（`sub_ptr=high_blk`，`hash_max=high_max`），`count++`；
5. `count+1 > 126 → is_parent_spilt_acquire = true`。

**`dentry_inner_split`（`split:139`）**：
- 触发 `count == 127`；左 63 hint / 右 64 hint；`block_alloc_hard(2,1)`；新内 `ptrs[0..2]=INVALID`；
- 拷 `hints[63..126]` → 新内 `[0..63]`；
- **⚠ reparent 右半孩子**：逐个读 `new_inner.hints[i].sub_ptr.ptr` 所指孩子，校验非根，`parent_ptr.ptr = new_blk`、`dirty()`（`split:181`）；漏了这条 → 后续上溯（rm 平衡/分裂传播找祖父）走错节点。
- `low_node_maxhash = victim->hints[62].hash_max`。

**递归向上（`rd:143`）**：读 `cur_parent`（只读，先取 `parent_is_root`）；
`inner_split(cur)` 后：若 `parent_is_root` → `new_root_inner`（结束）；否则按 `parent_ptr.ptr` 找祖父，**线性扫** `grand->hints[k].sub_ptr.ptr == cur` 得 `grand_idx`，
`spilt_parent_apply(grand, grand_idx, inner_split, &need)`；`cur = grand` 继续。

**`dentry_new_root_inner`（`split:298`）**：
- `block_alloc_hard(2,1)`；`dentry_node_meta_init(2, true, true, root_inode_idx)`；`hints[0]={low_blk,low_max}`；
  `hints[1]={high_blk, node_max_hash(right)}`；
- 两子 `parent_ptr.ptr = new_root_blk`、`is_root_node = 0`、`dirty()`；
- `inode_read/write(root_inode_idx)`：`data_root = new_root_blk`；内存 `root_dentry_blk = new_root_blk`。
- ⚠⚠ **god 此函数不清 `new_root->meta.ptrs[]`，也不写 root `..`**：`dentry_node_meta_init` 先 `memset(8192,0)`，故新根 `ptrs[0] = ptrs[1] = ptrs[2] = 0`。而 mkfs/audit 期望新根（根内部）为 `ptrs[0]=INVALID, ptrs[1]=root_inode(..), ptrs[2]=INVALID`。**0 是合法块号**，会把「预留」误成真指针；且 `aud:746` 会判 root `..`(=0) != 自身 → `dotdot_mismatch`。**ko 必须补写**（见 §E 坑 3）。

### B.7 结果标志 / 失败回滚

- `result_flags` 只由 insert/delete 产生，分裂机械不看它——由 `rd` 驱动（bit0→分裂，bit1→刷父）。
- **insert 层回滚**：`inode_guard_t` / `block_guard_t`（dir 树根 2 块）/ `name_heap_guard_t`
  （RAII：arm 后任何失败返回自动释放，成功路径 `disarm`；`id:38-215`、hpp `:397-437`）。
- **分裂/父应用失败：god 无回滚** —— 已 alloc 的块、已插入的子树都不回收（注释 `split:5-8`：dentry 插入路径**无预算池**，失败面上抛靠 fsck）。ko 必须自定失败面（若沿用「无回滚」需在文档声明；否则补 `__free` 守卫）。

---

## C. 删除路径（含再平衡）

### C.1 叶条目删除（`id:220` `dentry_leaf_entry_delete`）

- `idx` = 命中 GLB。物理删：`memmove(&entries[idx], &entries[idx+1], (count-idx-1)*64)` + `memset(&entries[count-1],0,64)` + `count--`。
- **result_flags**：`bit0 = (!is_root && count < 63)`（欠载，需合并/再平衡）；`bit1 = (is_last || count == 0)`（父层 hash_max 刷新）。
- **树删前**（零副作用）读下游资产：`inum = entries[idx].inode_ptr.ptr`（52 位截断比较）；`has_inode = (inum != PTR_INVALID_52) && inode_test(inum)`。
  - DIR 型目标：先校验**空**（树根是叶 && `entry_count==0`；`. / ..` 由 meta 合成不计）→ 非空 `RC_ERR_NOT_EMPTY`（校验必须在树删前）。
- **树删后**下游维护：DIR → `dir_name_heap_release` + `block_free(data_root,2)` + `inode_free`；REG/SOFT_SYM → `link_count--`，归零 → `regular_file_reclaim`（`tgt_sess` 析构后执行；失败 → `RC_ERR_NOT_SUPPORTED` 哨兵）。
- owner 目录 `size--`/`lc=size+2` 由 **driver** 维护（`rd:250`，本文件不动 owner）。

### C.2 驱动阶段一（`rd:251`）：刷父 hash_max，同 B.4（沿 trace 自底向上、短路 break）。

### C.3 驱动阶段二（`rd:277`）：欠载平衡（`bit0 && trace.depth > 1`）

- `dentry_node_balance(parent_blk, child_idx, &need)`。
- 若返回 `RC_ERR_STATE` → `try_root_shrink(parent_blk)`（仅当 parent 是根内 && `count==1` 才成功，否则原错误上抛）。
- `while(need)`：读 `cur_parent`；若 `is_root` → `try_root_shrink(cur)`、break；否则找祖父 → `balance(grand, grand_idx, &need)`；`RC_ERR_STATE` → `try_root_shrink(grand)`、break。
- **收尾**：`try_root_shrink(root_dentry_blk)`（统一收缩合并残留的「根内 count==1」）。

### C.4 `dentry_node_balance`（`bal:31`）

- 读父（校验内）、读目标子 `child`。**子已健康（`child_count >= 63`）→ 直接 `RC_OK`**（delete 后目标必 <63）。
- **邻居方向**：`idx == 0` → 只向右（`neighbor = 1`）；`parent_count == 1 → RC_ERR_STATE`（根收缩交上层）；`idx > 0` → 只向左（`neighbor = idx-1`）。
- 读邻居（校验同 `is_inner`）；`total = child_count + neighbor_count`。
- **`total <= 126` → 合并**：
  - 内：按方向拼接 hints（`idx==0`：child 原 + neighbor；否则 neighbor + child 原），`entry_count=total`；
    **reparent 全部孩子 → `child_blk`**（`bal:163` 调用 / `bal:74` 定义 `reparent_children`）。
  - 叶：按方向拼接 entries；**修叶链**：`idx==0`（child 左 / neighbor 右）→ `child.ptrs[2] = neighbor.ptrs[2]`，若有后继 → `后继.ptrs[1] = child_blk`；
    `idx>0`（child 右 / neighbor 左）→ `child.ptrs[1] = neighbor.ptrs[1]`，若有前驱 → `前驱.ptrs[2] = child_blk`。
  - `block_free(neighbor_blk, 2)`；`remove_parent_hint(parent, del_idx = idx==0 ? 1 : idx-1)`（memmove 左移）。
  - 更新保留 hint 的 `hash_max`（`kept_idx_after` 需按 `del_idx < kept_idx_before` 平移，`bal:229`）。
  - 父欠载判定：`!parent_is_root && new_parent_count < 63 → is_parent_need_balance = true`。
- **`total > 126` → 抹匀（再分配）**：`left = total/2`、`right = total - left`；
  `idx==0`：child 得左半、neighbor 得右半；否则 neighbor 得左半、child 得右半。内需 reparent 两侧；
  **叶链不变**（两叶都在）；更新两个 hint 的 `hash_max`；父 count 不变 → `need = false`。

### C.5 根收缩（`bal:347` `dentry_root_shrink`）

- 仅当根**为内**且 `count == 1`（`is_root` 校验；根叶 / `count!=1` → `RC_OK` 无操作）。
- 唯一孩子升为新根：`is_root_node = 1`、`parent_ptr.ptr = root_inode_idx`（`.`）；
  - 孩子是**叶**：`ptrs[0]=INVALID`、`ptrs[1]=root_inode_idx`（`..`，**从 prev 改回**）、`ptrs[2]=INVALID`；
  - 孩子是**内**：`ptrs[1]=root_inode_idx`（`..`）。
- `inode_read/write`：`data_root = child_blk`；`root_dentry_blk = child_blk`；`block_free(old_root, 2)`。

### C.6 欠载/合并阈值小结

- **叶与内统一下限 = 63**（=`126/2` 半满；`aud:35` `DENTRY_MINA=63`，`aud:334`(叶) / `aud:490`(内) `非根 && cnt<63 → BAD`）。仅**非根**适用；根可 <63 甚至 0（空目录根叶 count=0）。
- **合并条件** `total <= 126`；否则抹匀。根内 `count==1` 由上层收缩（本文件不处理，返 `RC_ERR_STATE`）。
- 空叶（非根 count→0）→ `bit0` → 合并进邻居 → 邻居块 `block_free` + 父 hint 删。

---

## D. 精确常量

| 项 | 值 | 说明 |
|---|---|---|
| 阶 `DENTRY_ORDER` | **126** | 叶 `entries[127]` / 内 `hints[127]`；索引 `[126]`=瞬时槽（禁落盘） |
| 分裂触发 | `count == 127` | 插入后 `count>126` |
| 分裂均分 | **左 63 / 右 64** | `copy_count = count - 63 = 64`；`low_node_maxhash = victim[62]` |
| 欠载下限 `DENTRY_MINA` | **63** | 非根叶/内；`entry_count < 63 → 欠载` |
| 合并条件 | `child+neighbor <= 126` | 否则抹匀（`left=total/2`） |
| 节点字节/块 | **8192B = 2 FS_block** | `block_alloc_hard(2, 1)`：2 块连续、`2^1` 对齐 |
| `PTR_INVALID` | `~0ULL`；52 位形态 `RFS_MASK52=0xFFFFFFFFFFFFF` | 0 合法，禁当空 |
| `DENTRY_INLINE_NAME_MAX` | 40 | 内联名上限 |
| `RAVENFS_MAX_NAME_LEN` | 552（=40+512） | 全名上限 |
| GLB 哨兵 `DENTRY_LEAF_GLB_NONE` | `0xFFFF` | 无下界（最左插入 idx=0） |
| 魔数 | `DENTRY_MAGIC1..4`（`include/magics.h`） | 256-bit 节点签名 |
| root inode | **v1.2 = 16**（god v1.1 = 0） | `parent_ptr.ptr` 写「自身 inode」用 |

---

## E. ko 移植要点

### E.1 session / RAII 映射

| god | ko | 备注 |
|---|---|---|
| `mdata_piece_session(mgr, blk)` | `RAVENFS_MSESSION(sb, blk, var)` = `ravenfs_mdata_get(sb,blk)` | 节点 = `ravenfs_msession_node(ms)` |
| `sess.dirty()` | `ms->row->dirty = true`（+ `row->stamp = ktime_get_ns()`） | 无 `dirty()` 方法，直写行标志 |
| `InodeSession(mgr, inum)` | `RAVENFS_SESSION(sb, inum, var)` | inode = `ravenfs_session_inode(s)` |
| `inode_guard_t` / `block_guard_t` / `name_heap_guard_t` | `goto cleanup` / `DEFINE_FREE` 或显式 `ravenfs_inode_free` / `ravenfs_block_free(sb,blk,2)` / `ravenfs_name_heap_free` | god RAII 在 C 内无对应，逐失败点手写 |
| `block_alloc_hard(2,1)` | `ravenfs_block_alloc(sb, 2, 1)` | align_log2=1 |
| `block_free(b,2)` | `ravenfs_block_free(sb, b, 2)` | |
| `inode_alloc/free/test` | `ravenfs_inode_alloc/free/test(sb,…)` | |
| `name_hash(name)` | `xxh64(name, nlen, 0)`（`linux/dentry.c`） | 已核：god `seed=0` XXH64（`ravenfs_juvenile.cpp:107`）；**两实现必须同算法同种子**（盘上存 hash） |

### E.2 锁纪律（关键）

- `ravenfs_mdata_get` **会睡眠**（`kzalloc(GFP_KERNEL)` + `down_read(&sh->lock)` + 可能 I/O 装载，`linux/mdata_cache.c:207`）→ **绝不可在 spinlock 内调用**。
- 分片锁（`rw_semaphore`）只在 get/put 内部短暂持有；**节点读改写 = 「get → 改 node 内存 → `row->dirty=true` → put」**，落盘由 flush 线程统一负责。
- 分层：**睡眠分配 + 块 I/O（session/alloc）在分片锁外**；只有 bitmap 的位运算在 spinlock 内（`ravens_block_alloc/free` 内部自旋，锁内零 I/O）。
  → dentry 树机械里 `ravenfs_block_alloc`（自旋，不可睡眠）与 `RAVENFS_MSESSION`（可睡眠）**不可同处一个不可睡眠临界区**；顺序化（先分配块、再开会话）。
- 同一 inode 双会话避免：delete 的 `tgt_sess` 必须析构后再 `regular_file_reclaim`（ko 同：先释放 session 再回收）。
- 分裂时 victim/new/next 三会话并存，但改的是**不同节点**（不同 row），安全；同一 8K 片（`blk & ~1`）的两个 4K 半块共用一 row 时注意不要双写冲突。
- M5 写路径整段在 VFS `i_rwsem`（dir）写锁下 → 并发顺序大半被 VFS 掩盖；node latch 是 M7 范畴。**但**回写/evict/sync 后台线程在 `i_rwsem` 之外 → `dirty` 行可能被 TTL evict（仅 refcount==1 时），需注意。

### E.3 逻辑移植差异（必改）

1. **击穿语义**：ko 只读 `ravenfs_dir_find` 把 `hash > 全部 hash_max` 当 miss；**插入**必须改成 god「击穿 → 钻最右叶」（`walk:80`）。
2. **多级叶定位**：ko `ravenfs_leaf_locate`（`linux/dentry.c:228`）现只支持单叶（`inner → -EOPNOTSUPP`）→ M5 需扩成多级 walk，返回 `depth + path[]`（对应 god `bptree_path_t`），插入位 = GLB 语义。
3. **上界 vs 严格上界**：dentry 内路由 `≥`；extent 是 `>`；不可互抄。
4. **52 位截断**：v1.1 god 需 `PTR_INVALID_52`；ko v1.2 直接用 `RFS_PTR_ADDR`/`RFS_PP_PTR`（自动 52 位），比较与哨兵判定不写错。
5. **`..` / 预留 ptrs 哨兵**：新建节点后必须**显式**写 `ptrs[]`（`0` 落盘会被当合法指针）——尤其升根路径（见坑 3）。
6. TSC/CPU 亲和：dentry 树机械本身不依赖；仅 bitmap alloc 的 `preferred[cpu]` 路由依赖 `raw_smp_processor_id()`（god 的 extern `get_processor_id()` 合约）。alloc 自旋上下文取 CPU id 需保证不睡眠。

### E.4 最容易踩的坑（Top 3）

1. **新根/升根没有初始化 `ptrs[]` 与 root `..`**（`dentry_new_root_inner` 只 memset，`ptrs[]` 全 0，`ptrs[1]` 未写 root inode）——0 是合法块号，且 audit 会判 root `..` 缺失（`dotdot_mismatch`）。ko **必须显式写** `ptrs[0]=INVALID, ptrs[1]=root_inode, ptrs[2]=INVALID`。
2. **`old_next` 读序**：叶分裂必须先读 `victim.ptrs[2]`（旧后继）再改写为 `new_blk`，否则拿到的「旧后继」是新叶自己 → 叶链自指/断链（`split:74`）。
3. **52 位截断 + 内路由方向**：v1.1 位域 ptr 是 52 位，与 `PTR_INVALID`（64 位）比较恒不等 → 悬空判定/哨兵失效；且 dentry 用 `≥`、extent 用 `>`，抄错路由会选错子树。

### E.5 ko 落地实测新增坑（2026-09-27，guest 实测）

1. **分裂时「瞬时 count=127」不能被通用 meta 校验拒掉**：ko `rfs_meta_ok` 原把 `count>126` 当落盘污染 → 返回 `-EIO`；但 `rfs_leaf_split`/`rfs_inner_split` 正是要在 `count==127` 上工作。修法：加 `rfs_meta_ok_x(..., maxc)`，分裂传 `maxc = ORDER+1`（127），即时槽允许、其他调用点仍传 126。症状：第 127 条 create 返 `-EIO`，且**条目已插入但未分裂** → 叶 count=127、新 inode 已分配（create 错误路径又 reclaim 了它）→ audit 报 `count>126` + 悬挂引用。
2. **释放节点块前必须丢弃其 mdata 缓存行**（`ravenfs_mdata_drop`）：合并/根收缩把节点块 `block_free` 后，该块的缓存行若仍 `dirty`（早前写路径留下的），会在后续 `flush_all` 回写到「可能已被重新分配」的块 → 污染新节点。drop 语义：无人引用→直接丢弃；被引用→清 `dirty`（禁回写）。合并/根收缩释放邻居块/旧根前都调用。
3. **create 的 owner 计数在驱动层维护**：ko `ravenfs_ddentry_insert` 只做树机械（与 god `dentry_leaf_entry_insert` 不同——god 把 inode/堆/计数下沉）。故 ko `create/unlink` 在 walk/insert/delete 前后自维护目录 `size++/--`、`link_count=size+2`。

### E.6 内核版本/回调约定坑（2026-09-27）

1. **`.mkdir` 在本内核（linux-7.2.3）返回 `struct dentry *`**（ext2 约定：成功 `d_instantiate` 后**返 `NULL`** 用原 dentry；失败 `ERR_PTR`）。**不是** `int`。（`.create`/`.rmdir`/`.unlink`/`.rename` 仍返 `int`。）
2. **rename 的 VFS 分工**：VFS 已做 flag 合法性（`~NOREPLACE|EXCHANGE|WHITEOUT` → `-EINVAL`；NOREPLACE+EXCHANGE → `-EINVAL`）、祖先陷阱（`lock_rename` 的 trap → `-EINVAL`/`-ENOTEMPTY`）、EXCHANGE 目标必须存在、目标覆盖权限。ko `->rename` 只做盘上工作（+ 文件 ctime；目录跨父改 `..`）；**dcache 的 `d_move`/`d_exchange` 由 VFS 自做**（见 spec §4.11.3）。
3. **EXCHANGE 语义**：两名都保留，仅**交换 `inode_ptr`**（name/hash/堆不动）；目录交换时两侧 `..` 互换。
4. **测试注意**：M4（文件数据）未接 → **写文件内容 `-EINVAL`**；命名空间测试用 `touch`/`mv`/`stat`，勿用 `echo > f`。`mv -T` 才测「覆盖目录」；`mv`（无 -T）是「移入目录」。

---

## 附：god 写入路径调用链（一眼图）

```
dir_insert_dentry (rd:45)
  └ walk 定位 + 查重
  └ dentry_leaf_entry_insert (id:38)  ── inode/堆/树根 + memmove + owner 计数；出 flags
  ├ bit1: 沿 trace 自底向上刷 hash_max（短路）
  └ bit0: dentry_leaf_split (split:49) ── 裂叶 + 叶链
        ├ depth==1: dentry_new_root_inner (split:298)
        └ else    : dentry_spilt_parent_apply (split:223)
                     └ while(need): dentry_inner_split (split:139) → new_root / 祖父 apply

dir_rm_dentry (rd:187)
  └ walk 命中
  └ dentry_leaf_entry_delete (id:220) ── 物理删 + 下游资产；出 flags
  ├ bit1: 沿 trace 自底向上刷 hash_max
  └ bit0: dentry_node_balance (bal:31) ── 合并/抹匀（叶链维护 + reparent）
           └ while(need): 向上 balance / 根内 count==1 → dentry_root_shrink (bal:347)
```

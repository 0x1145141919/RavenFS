# M3 写路径 god→ko 移植规格（分配器 + dentry 条目增删）

> 2026-09-26 · 面向 **M3（元数据写路径）** 的精确移植规格。**只读 god（v1.1）**，产的是「行为契约」不是代码。
> 真值源：`juvenile_god/bitmap_slices.cpp`、`include/ravenfs/bitmap_slices.hpp`、`juvenile_god/ravenfs_juvenile.cpp`、
> `juvenile_god/dentry_insert_delete.cpp`、`juvenile_god/name_heap.cpp`；盘面真值 `include/disk_struct_v1.2.h`。
> **生殖隔离原则**：god = v1.1 语义参考；ko = v1.2 盘面 + 内核设施（mdata/inode 行缓存、伙伴页、spinlock/bio）。
> 行号引用格式 `文件:NN`，均指上述 god 文件。

---

## A. 位图分片分配器（bitmap_slices）

模块 = 纯内存、零 I/O、零设备知识；调用方负责位图原始内存的加载/写回与 `get_processor_id()` 外部合约。
两个独立组：**block 组**（块位图）+ **inode 组**（inode 位图），几何推导**共享** slice_count（故合成单 mgr 而非双实例）。

### A.1 几何推导（`bitmap_slices.cpp:363` ctor / `:330` setup_group）

输入（`bitmap_slices_cfg_t`）：两组「已加载位图原始内存 + 位数」+ `logical_processor_count`。
ko 侧喂入（`ravenfs_juvenile.cpp:319` 附近同式）：`block_bits_count = bitmap_blocks × 4096 × 8`、`inode_bits_count = inode_bm_blocks × 4096 × 8`。

- **片下限 `msb = 32768` 位**（= 1 个位图块 = 4K 对齐切片约束，硬编码；cfg.min_slice_bits **实际未被消费**，`bitmap_slices.cpp:331` 写死 32768 —— 移植时注意）。
- **slice_count 推导**（三段，ctor 内 ≈`bitmap_slices.cpp:372`–`:402`）：
  1. `n` = ≥ `logical_processor_count` 的最小 2 的幂（上界 `1<<30`）；
  2. 满片数约束：`cap = min(block_bits_count, inode_bits_count) / 32768`；`cap==0 → 1`；`n = min(n, cap)`；
  3. **末片非空迭代**：每片位数 `per = align_up(ceil(bits_count / n), 32768)`（两组各算 `per_b`/`per_i`）；若 `(n-1)*per_b ≥ block_bits_count` 或 `(n-1)*per_i ≥ inode_bits_count` 则 `n >>= 1` 重试（`n==1` 兜底）。
- `slice_count_ = n`；`cpu_slots_ = max(logical_processor_count, 1)`。
- **per-slice 位数**（`setup_group`，两组各一）：`per = align_up(ceil(nbits / slice_count_), 32768)`。
  片 `i` 覆盖全局位区间 `[i*per, min((i+1)*per, nbits))`；`slice_base = raw + (i*per)/8`（`i*per` 恒为 32768 倍数 → **4K 对齐**）。
- **两组几何**：block 组片基址派生自 block 位图 raw；inode 组派生自 inode 位图 raw。片内坐标 = 全局位号 − `base_idx`（`base_idx = i*per`）。
- **ko 移植**：位图 raw 必须 4K 对齐（`alloc_pages`/`vmalloc` 后校验），slice 数组按缓存行对齐分配；锁换成内核 `spinlock_t`（见 A.2），**不可睡眠**。

### A.2 路由规则 + 锁纪律（`bitmap_slices.cpp:430`–`:590`）

**锁纪律（全文件铁律）**：slice 层方法**一律不加锁**；临界区由 mgr 路由层编排。
- 分配（`block_alloc_hard/soft`、`inode_alloc`）：`try_lock_write()`，忙则**换片**（preferred 亲和扫描，仿 FPA::alloc）；成功→更新 `preferred[cpu]=idx`。
- 释放 / 定点占领（`block_free`、`block_occupy_soft`、`inode_free`）：**自旋强制抢锁**（定点不可换片 / 释放不可丢）。
- 测试读（`block_test`/`inode_test`/`get_free_count`/`get_hint`/`set_hint`）：无锁，调用方保证并发安全。
- **ko 移植**：`rw_try_spinlock_t`（`include/util/lock.h`）→ 内核 `spinlock_t` + `spin_trylock()`/`spin_unlock()`。临界区内**只做位运算**（无 I/O、无内存分配）→ 可用 spinlock；点解：`test_interval/free_interval/soft_occupy` 的锁全由调用方持（mgr 层已持）。

**路由（块号/inode 号 → 片）**：
- **按 CPU 亲和扫描**（alloc 家族）：`cpu = get_processor_id() % cpu_slots_`；`for round in [0, slice_count_)`：`idx = (preferred[cpu] + round) % slice_count_`，`try` 锁失败继续下一片；全片失败 → 哨兵 `PTR_INVALID`。
- **定点路由**（free / occupy / test）：`idx = 位号 / per_slice_bits`；越界直接丢弃（防御）。
- **跨片循环**：`block_free` 把物理连续区间按片边界切段逐段释放（释放无连续要求，逐段清位，无需跨片双持）；`block_occupy_soft` 在片尾**内部续圈**到下一片 base 0（逐片单锁前缀链 = 真实连续，永不双持）。
- **`get_processor_id()`**：god cpp 顶部 `extern "C"`；ko 必须提供强定义 —— 用 `raw_smp_processor_id()`（禁抢占语义下稳定即可；勿用会睡眠/迁移的 id）。

### A.3 位操作原语（`bitmap_slices.cpp:28`–`:107`）

- **粒度**：`uint64_t words[]`，`bit idx → words[idx>>6] >> (idx&63)`；**1 = 已用/已分配，0 = 空闲**。`free_count` = 片内 0 位计数，`enable_slice` 时 O(片) 扫出后增量维护。
- `bit_get/bit_set/bit_clear`（`:32`–`:40`）：小端字内位操作，纯内存。
- **`next_free_seg(words, nbits, from, max_extend, &start, &end)`**（`:44`）：从 `from` 起找下一个**空闲段** `[start, end)`；整字全占用直接跳过，末字按 `nbits` 截位；`max_extend>0` 时段长够 `max_extend` 即截断（防稀疏大位图 1-bit 分配 O(n²) 热点）；无空闲 → `false`。
- **`find_free_run(words, nbits, from, count, align)`**（`:89`）：找 `[start, start+count)` 全空闲且 `start % align == 0` 的起始位（片内偏移）；内部用 `next_free_seg` 以 `cap = count+align` 界，`cand = align_up(seg_s, align)`，接受条件 `cand<seg_e && cand+count<=seg_e`，否则 `probe=seg_e` 续扫；无 → `SLICE_NO_HIT = ~0ULL`。`count==0 || count>nbits → SLICE_NO_HIT`。
- 对齐参数：`align = 1ULL << min(align_log2, 16)`（**防御截断 16**）。

### A.4 slice 层成员语义（返回值/错误码）

| 函数 | 语义 | 返回 / 错误码 |
|---|---|---|
| `enable_slice(gdesc, cfg)` `:110` | 绑定位图子段 + 扫 `free_count`。校验：`slice_base≥g_base`、字节偏移 4K 对齐、`base<bits_count`、片尾不越界；`last_finish_hint=0` | `RC_OK` / `RC_ERR_STATE` |
| `disable_slice()` `:136` | 全字段清零 | — |
| `get_free_count()` / `get_hint()` `:145/:150` | 读数 | `uint64_t` |
| `set_hint(h)` `:154` | 写 hint；`h>slice_bitscount` **忽略**（越界保护） | — |
| `alloc_hard(al, cnt, &out)` `:160` | 片内找 `cnt` 连续空闲、`2^al` 对齐；**从 hint 起扫，段尾绕回 `[0,hint)` 再扫**；不部分分配 | `RC_OK`（`out=base_idx+hit` 全局位号）/ `RC_ERR_NO_BLOCK` / `RC_ERR_STATE`。成功：置位、`free_count-=cnt`、`hint=hit+cnt` |
| `alloc_soft(al, cnt, &out, &left)` `:181` | 第一个空闲段取对齐候选，实际 `got=min(段长,cnt)`；缺口回传 | `RC_OK`（`left=cnt-got`）/ `RC_ERR_NO_BLOCK`（`left=cnt`）/ `RC_ERR_STATE`。成功：`hint=hit+got` |
| `soft_occupy(base, len, &rest)` `:236` | **锚定式**（对偶 `free_interval`）：从全局位 `base` 起贪心认领**连续空闲前缀**，撞占用/额度上限即停；`real=len-*rest` 为上限；**cap 片内夹断，永不越片**；hint 只进不退（`off+k>hint` 才推进） | `claimed≥1 → RC_OK`（`*rest=len-claimed`）；`claimed==0 → RC_ERR_NO_BLOCK`（`*rest=len`）；`base<base_idx`/`off≥slice_bitscount`/`*rest>len` → `RC_ERR_STATE`；`len==0` 或 `real==0 → RC_OK`（rest 不动） |
| `test_interval(val, base, len)` `:285` | `[base,base+len)` 全同 `val` | `RC_OK` / 有异位 `RC_ERR_NOT_FOUND` / 越界 `RC_ERR_STATE` / `len==0 → RC_OK` |
| `free_interval(base, len)` `:300` | 清 `[base,base+len)`；`free_count` 按**实际清掉的 1→0 位**计数（重复释放不虚高） | `RC_OK` / `RC_ERR_STATE` |

**hint 维护规则（绕回）**：分配成功后 `hint = hit + 实际占用数`；`soft_occupy` 只进不退；`set_hint` 只接受 `≤ slice_bitscount`。绕回只发生**搜索起点**（`alloc_hard` 用 `find_free_run(words, hint, 0, …)` 把上界夹到 hint；`alloc_soft` 从 0 再找一段），**不改变 hint 语义**。

### A.5 mgr 对外 API 一览（精确签名 + 语义 + 错误码，`bitmap_slices.hpp:112`–`:168`）

哨兵统一 `PTR_INVALID`（0 是合法块号/槽号）；`align_log2` 为「2 的幂指数」。

| 签名 | 语义 | 返回 / 错误码 |
|---|---|---|
| `u64 block_alloc_hard(u64 require_count, u8 align_log2)` | count 连续 + 起始块号对齐；**单片内完成不跨片**；preferred 优先 try 扫描 | 块号 / `PTR_INVALID`（原子，不部分分配） |
| `u64 block_alloc_soft(u64 require_count, u64 *remain_count)` | 一次只从单片取，`got=min(区间长,require)`；`remain=缺口`；内部 `alloc_soft(0,…)`（**align=1**） | 块号 / `PTR_INVALID` |
| `u64 block_occupy_soft(u64 blkbase, u64 require_count, u64 *rest_count)` | 从 `blkbase` 起锚定占领 ≤ require 连续空闲块；片尾跨界内部续圈 | `blkbase`（占领≥1）/ `PTR_INVALID`；`*rest_count=缺口`（⚠ 可能产跨片 run） |
| `void block_free(u64 blkbase, u64 count)` | 跨片循环清位；越界剩余放弃 | — |
| `bool block_test(u64 blk) const` | 直读位图内存（无锁） | bool |
| `u64 inode_alloc()` | inode 组 `alloc_soft(0,1,…)`；preferred 优先 | inum / `PTR_INVALID` |
| `void inode_free(u64 inum)` | 自旋抢锁 → `free_interval(inum,1)` | — |
| `bool inode_test(u64 inum) const` | 直读（无锁） | bool |
| `u64 slice_count() const` / `u32 cpu_slots() const` | 几何 | — |
| `u64 block_per_slice_bits() const` / `u64 inode_per_slice_bits() const` | 每组片位数 | — |
| `u64 block_free_total() const` / `u64 inode_free_total() const` | 各片 `free_count` 求和（空闲总数，**审计用**） | — |
| `u32 block_preferred_of(u32 cpu) const` / `u32 inode_preferred_of(u32 cpu) const` | dump_info 用 | — |

**RavenFS_juvenile 转发壳**（调用点零改动，`ravenfs_juvenile.cpp:632` 起）：同名 8 个薄转发（`block_alloc_hard/soft`、`block_occupy_soft`、`block_free`、`block_test`、`inode_alloc`、`inode_free`、`inode_test`），`bitmap_mgr==nullptr` 时哨兵返回。

### A.6 自身描述 / 尾巴（tail）标记 + inode 0 保留

- **置**：由 **mkfs** 写死（不在运行时置）。三段不变量：
  1. block 位图自身区 `[bitmap_start, total_blocks)` 全 **1**；
  2. inode 元数据区（inode array + inode 位图）`[inode_bm_base, bitmap_start)` 在 **block 位图**中全 **1**；
  3. **inode 位图 bit0（root inode）亮**。
- **校验**：挂载期 `RavenFS_juvenile::bitmaps_load()`（`ravenfs_juvenile.cpp:575`）逐位校验，任一不满足 → `goto fail` → `RC_ERR_BITMAP_SELF`（ctor `:313`）。
- **`inode 0 = root 保留`**：ctor 末尾 `ino_.slices[0].set_hint(1)`（`bitmap_slices.cpp:417`），让 inode_alloc 跳过 0。
  ⚠ **v1.2 更正（2026-09-26，用户澄清）**：v1.2 `root_inode = 16`。**不存在任何保留区** —— 0..15 与 17.. 全部可正常分配（mkfs 只占用了 16 这一个 inode）。god 的 `set_hint(1)`（v1.1 root=0 时代）在 v1.2 **不需要、也不应**复刻。
  （本文初稿曾写成“ko 需保留 0..15”，系误采 mkfs.hpp 一条错误注释；已铲除。）

---

## B. inode 读写（`ravenfs_juvenile.cpp:511`–`:560`）

### B.1 偏移换算（几何）
- `inode_blocks = ceil(inode_count × 256 / 4096)`；`inode_array_base = bitmap_start − inode_blocks`；`inode_bm_base = inode_array_base − inode_bm_blocks`。
- inode array 从 `inode_array_base` 块起线性排布，**16 inode/块**（256B × 16 = 4K）。第 `inum` 个 inode：块 `inode_array_base + (inum>>4)`，槽 `inum & 15`。
- `FS_block[n]` 字节偏移 = `blk0_off + n×4096`（`blk0_off` 恒 4096；Primary SB 占 byte[0,4096)）。

### B.2 语义（v1.1 零缓存，每次直接落盘/映射）
`inode_read(inum, raven_Inode_t *out)`：三后端分流
1. `self_reliat` 缓存后端（god 专有）→ `self_reliat_get/put`；**ko 无此模式**（ko inode 记录在 `disk_inode_cache_mgr` 行里，走 `RAVENFS_SESSION`）。
2. **mmap 后端**：直接 `memcpy` 自窗口切片 `fsblk0_base_ + inode_array_base*4096 + inum*256`（零拷贝）。
3. **直写（O_DIRECT）后端**：整块 4K `pread`（长度/缓冲须 4K 对齐）→ 取槽内 256B。
`inode_write(inum, const raven_Inode_t *in)`：对称；直写后端 **读-改-写整块 4K**（pwrite 前先 pread 整块），避免 256B 非对齐写。

**ko 移植要点**：不存在 god 的两后端分歧；ko 统一走 inode 行缓存 `ravenfs_session_get(sb, inum)`（行尺寸 mount option 4K..64K，槽位 = `inum & off_mask`），session 内 `ravenfs_session_inode(s)` 得 `struct raven_Inode_t *`；写 = 改缓存副本 + `row->dirty=true`（等价 god 的 `dirty()`），落盘由 `flush_all` 统一负责（见 handover §3.3）。

### B.3 写路径对 superblock 的触碰
god **inode 写路径不碰 superblock**。SB 字段只在挂载/卸载症结点更新：
- `mount_time`：ctor 中 SB 校验通过后**立即 pwrite**（`ravenfs_juvenile.cpp:248`，readonly 不写）；写前 `crc32=0` 重算 `sb.crc32`，整 4K 落盘。
- `umount_time`：dtor 中 `caches_teardown()` + `bitmaps_flush()` **之后** pwrite（`ravenfs_juvenile.cpp:361` 附近）。
- **不变量**：非正常关机 → `umount_time` 停留在上次干净卸载值（与 `mount_time` 对照可检非正常关机）。v1.2 另有 `born_time`（mkfs 后恒不变）与 `modify_time`（几何变更，当前无在线路径）。
- **SB 内无空闲计数字段**（`total_blocks`/`inode_array_count` 静态）→ 分配器不需要回写 SB。**ko 注意**：位图持久化是 M3 硬约束（全量内存分片 + `sync_fs`/`put_super` 全量刷回，spec R-SY-03），与 SB 时间戳是两条独立写。

---

## C. dentry 叶条目 插入/删除（`dentry_insert_delete.cpp`）

常量：`DENTRY_INLINE_NAME_MAX = 40`；`RAVENFS_MAX_NAME_LEN = 552`（= 40+512）；叶容量 **126**（`Bptree_dentry_leaf_node_t.entries[127]`，`[126]` 是插入瞬时槽）；节点 8K；`name_len_limiter` 判 `strlen ≤ 552`。

### C.1 `dentry_leaf_entry_insert(leaf_blk_base_idx, name, idx, &result_flags, inode_type, owner_dir_inode)`（`:38`）

前置：`idx` 由 `dentry_Bptree_walk` 的 GLB 语义推导（`glb==NONE → idx=0`，否则 `idx=glb+1`）；**调用前已查重**（EXIST 由上层 walk 判，省 inode/堆/树根）。

逐步：
1. **入口校验**：`!name || !name_len_limiter` → `RC_ERR_NAME_LEN`；`inode_type ∉ {REGULAR, DIR, SOFT_SYM}` → `RC_ERR_STATE`。
2. **分配新 inode**：`new_inum = inode_alloc()`；`PTR_INVALID → RC_ERR_NO_INODE`；`inode_guard.arm(this, new_inum)`（RAII）。
3. **按类型初始化 inode**：
   - **DIR**：`block_alloc_hard(2, 1)` 建树根（2 块，8K）；失败 `RC_ERR_NO_BLOCK`（guard 自动回滚 inode）；`dir_root_guard.arm(this, root_blk, 2)`。`mdata_piece_session(root_blk)`：`memset 8192` → 写 `magics[0..3]=DENTRY_MAGIC1..4`、`parent_ptr{entry_count=0,is_inner_node=0,is_root_node=1,ptr=new_inum("." )}`、`ptrs[0]=PTR_INVALID`、`ptrs[1]=owner_dir_inode("..")`、`ptrs[2]=PTR_INVALID`、`dirty()`。`InodeSession(new_inum)`：`memset`、`type=DIR`、`link_count=2`、`permission_bits=0755`、`size=0`、`data_root=root_blk`、**10 个堆描述符双指针 = `NAME_HEAP_INACTIVE`**（`memset 0` 是雷：0 是合法块号，释放会误回收树根块）、`dirty()`。
   - **REG / SOFT_SYM**：`InodeSession(new_inum)`：`memset(sizeof(raven_Inode_t))`（inline_data 全零 = 内联空文件）、`type=inode_type`、`link_count=1`、`permission_bits=0644`、`data_root=PTR_INVALID`、`dirty()`。
   - `name->inum = new_inum`。
4. **长名分支**（`name_len > 40`）：`name_heap_alloc(owner_dir_inode, name_len−40, &loc)`；失败即返（guard 回滚）。`name->full_char_remain_ptr = (loc.fblk<<12) | loc.offset`；`heap_guard.arm(this, owner_dir_inode, name_len, remain_ptr)`；`name_heap_write(name_len, remain_ptr, name+40)`；失败即返。
   - **≤40**：无堆操作，`full_char_remain_ptr` 保持入口值（通常 `PTR_INVALID`）。
5. **树插**（`mdata_piece_session(leaf_blk_base_idx)`）：校验 `is_dentry_node && !is_inner_node`；`count=entry_count`；`idx>count → RC_ERR_STATE`；记 `old_max_hash`；`memmove(&entries[idx+1], &entries[idx], (count−idx)*sizeof(Dentry_t))`；填 `entries[idx]`：`hash=name->hash`、`inode_ptr.free_to_use=name_len`、`inode_ptr.ptr=name->inum`、`name` 拷 `min(name_len,40)` 并**零填剩余**、`full_char_remain_ptr`；`entry_count=count+1`；`sess.dirty()`。
6. **result_flags**：`count>126 → bit0`（需分裂）；`count>1` 时 `(idx==count-1 || new_max_hash!=old_max_hash) → bit1`（父层 hash_max 刷新）；`count==1 → bit1`。
7. **父目录计数维护**（`InodeSession(owner_dir_inode)`）：`size++`；`link_count = (u32)size + 2`；`dirty()`。
8. **成功转正**：`heap_guard.disarm(); inode_guard.disarm(); dir_root_guard.disarm(); return RC_OK`。

**回滚守卫（RAII，`ravenfs_v1.1.hpp:397`–`:437`）**：`inode_guard_t`（析构 `inode_free(inum)`）、`block_guard_t`（析构 `block_free(blk,count)`）、`name_heap_guard_t`（析构 `name_heap_free(dir_ino,name_len,remain)`）。语义：**arm 后任何失败返回自动回滚；成功路径显式 disarm**。移植时 ko 用 `__free(...)`/goto-cleanup 等价（ko 无 C++ RAII，需 `DEFINE_FREE` 或 goto 标签；注意 god 守卫覆盖「insert/分裂链路全部失败点」）。

### C.2 `dentry_leaf_entry_delete(leaf_blk_base_idx, idx, &result_flags, bool destroy_data)`（`:220`）

前置：`idx` = `dentry_leaf_node_lookup` 命中的 GLB idx。物理删除 = 左移 + 清末槽。

逐步：
1. `mdata_piece_session(leaf_blk_base_idx)`；校验 `is_dentry_node && !is_inner_node`；`count=entry_count`；`idx>=count → RC_ERR_STATE`。
2. **树删前读下游资产**：`PTR_INVALID_52 = PTR_INVALID & ((1<<52)-1)`；`inum = entries[idx].inode_ptr.ptr`（52 位域）；`has_inode = (inum != PTR_INVALID_52) && inode_test(inum)`（悬空条目跳过资产维护）。
   ⚠ 52 位域读出的 inum 与 `PTR_INVALID` 比较**必须同样截断**（64 位比较恒不等）。
3. **dir 型删除前置校验（零副作用，必须在树删前）**：读目标 inode → `tgt_type=type`；若 DIR：`dir_tree_root=data_root`（`PTR_INVALID → RC_ERR_STATE`，不变量 11）；读树根 → 非叶节点（`is_inner_node`）或 `entry_count!=0` → **`RC_ERR_NOT_EMPTY`**（`. / ..` 由 parent_ptr/ptrs[1] 合成，不计树内条目）。校验失败不产生任何树/位图变更。
4. **物理删除**：`is_last=(idx==count-1)`；`old_max_hash=entries[count-1].hash`；`memmove(&entries[idx], &entries[idx+1], (count-idx-1)*sizeof)`；`memset(&entries[count-1],0,sizeof)`；`entry_count=count-1`；`sess.dirty()`。
5. **result_flags**：`!is_root && count<63 → bit0`（欠载，需合并/再平衡）；`is_last || count==0 → bit1`（父层 hash_max 刷新）。
6. **树删后下游维护（`DELETE_FULL`；失败仅上报，孤儿/计数偏差交 fsck）**：
   - **DIR**：`dir_name_heap_release(inum)` → `block_free(dir_tree_root, 2)` → `inode_free(inum)`（空目录全量回收）。
   - **REG / SOFT_SYM**：`lc=link_count`；`lc==0 → RC_ERR_STATE`（不变量 17：已分配 inode `link_count ≥ 1`）；`lc--` 写回 `dirty()`；归零 → `need_reclaim=true`；**在 `tgt_sess` 析构后**调 `regular_file_reclaim(inum, destroy_data)`（按 inline/direct/extent 形态释放数据 + inode 槽）；失败 → **`RC_ERR_NOT_SUPPORTED` 哨兵**（树删已生效，inode 孤儿暂挂，fsck 可回收）。
7. 返回 `RC_OK`。

**父目录计数**：删除路径**不动 owner dir 的 size/link_count**（由上层 `dir_rm_dentry`/`root_dir_rm_dentry` 驱动；本文件只管叶条目 + 目标资产）。插入路径的 owner 维护见 C.1 第 7 步。

### C.3 名字堆（`name_heap.cpp`，insert/delete 触达部分）

- **分档**（尾部 = `name_len − 40`，≤512B；`name_heap_pick` `:44`）：
  `≤40B→8B 粒度/≤5 单元/槽0–2`；`41–64→64B/1 单元/槽3–4`；`65–128→128B/槽5–6`；`129–256→256B/槽7–8`；`257–512→512B/槽9（仅 8k）`。
  表：`kGranLog2={3,3,3,6,6,7,7,8,8,9}`、`kBitsLog2={16,17,18,16,17,16,17,16,17,16}`、组边界 `kGroupSlotLo/Hi`（`:36`–`:41`）。
- **几何**：`heap_bmp_blocks = 1<<(bits_log2−15)`（2/4/8 块）；`heap_data_blocks = 1<<(bits_log2+gran_log2−12)`；`heap_total_bits = 1<<bits_log2`；`BITS_PER_PIECE=65536`（8K×8）。
- **`name_heap_alloc(dir_inode_idx, tail_len, &out)`**（`:148`）：读 dir inode → `pick` 定组 → 组内**从低档起**：未激活 → `name_heap_activate`（懒激活：位图面 + 数据面各 `block_alloc_hard` 连续 + **2^1 对齐**；位图面清零；填控制器；失败回滚已分配）；已激活 → 位图分片载入栈缓冲 → `bitmap_find_free` 找连续空闲 → 置位 → 分片写回 → 物理定位 `out->fblk=data_base+(byte_off>>12)`、`out->offset=byte_off&0xFFF`。组内全满 → `RC_ERR_NO_BLOCK`。返回 `RC_ERR_STATE`（参数/dir 类型）/`RC_ERR_IO`/`RC_ERR_NO_BLOCK`。
- **`name_heap_write(name_len, remain_ptr, tail)`**（`:313`）：定位片 `piece=blk&~1`，片内偏移 `in_piece=((blk&1)<<12)+offset`；`≤512B` 可能跨 8K 片（≤2 片）→ 两段拷贝，各 `dirty()`。`name_len≤40` 或 `!tail → RC_ERR_STATE`。
- **`name_heap_free(dir_inode_idx, name_len, remain_ptr)`**（`:225`，由 guard 调用）：`tail=name_len−40` → 组；遍历组内槽按数据区区间**唯一命中** → 反推 `bit_start` → 分片载入 → 区间清位 → 写回。数据面/位图**不收缩**。`name_len≤40 → RC_OK`；`remain_ptr==PTR_INVALID → RC_ERR_STATE`；无命中 → `RC_ERR_STATE`（调用方 bug）。
- **`dir_name_heap_release(inode_idx)`**（`:350`，删除 DIR 时调用）：遍历 10 控制器，位图面**全空**（全字节 0）→ `block_free` 位图面 + 数据面 + 控制器重置 `NAME_HEAP_INACTIVE`（4 字段）；非空跳过；`changed → dirty()`。
- **ko 移植**：位图面/数据面读写 god 走 `mdata_piece_session`（8K 片）→ ko 走 `RAVENFS_MSESSION`；`bitmap_base` 视图（`include/util/bitmap_base.hpp`，god 自造）→ ko 可换成直接 `u64` 字扫描（同 A.3 的 `find_free_run`/`next_free_seg` 轮子足够）。

---

## D. 盘面字段触碰清单 + v1.2 改名标注

### D.1 上述流程**会写到**的结构 / 字段（god v1.1 名）

| 结构 | 字段（v1.1） | 谁写 |
|---|---|---|
| `Dentry_t` | `inode_ptr.free_to_use`（=name_len）、`inode_ptr.ptr`（=inum）、`hash`、`full_char_remain_ptr`、`name[40]` | insert（第5步）、delete（memmove/清末槽） |
| `Bptree_meta_t` | `magics[0..3]`、`parent_ptr.entry_count`、`parent_ptr.is_inner_node`、`parent_ptr.is_root_node`、`parent_ptr.ptr`、`ptrs[0]/ptrs[1]/ptrs[2]` | insert（建根叶）、insert/delete（entry_count） |
| `raven_Inode_t` | `type`、`link_count`、`permission_bits`、`size`、`data_root`、`inline_data` | insert（REG/SYM）、delete（link_count） |
| `raven_dir_Inode_t` | `type`、`link_count`、`permission_bits`、`size`、`data_root`、`heaps_8B_8k … heaps_512B_8k`（10 控制器） | insert（DIR）、insert/delete owner 计数、name_heap |
| `name_heap_controller_t` | `bytes_per_bit_log2`、`total_bits_count_log2`、`bitmap_base_block_ptr`、`data_base_block_ptr` | name_heap_activate/release |
| `RavenSuperblock` | `mount_time`、`umount_time`、`crc32` | ctor / dtor |

### D.2 v1.2 改名 / 换形式对照（`include/disk_struct_v1.2.h`）

**主线一（内容）**：inode 语义重定义（offset/尺寸不变，仍 256B）
- 第一字：`type:2 | is_data_root_direct_bit:1 | other:61` → **`type:6 | direct:1 | i_flags:57`**（`word1`，宏 `RFS_INO1_GET_TYPE/DIRECT/FLAGS`、`RFS_INO1_PACK`）。
- `permission_bits` → **`mode_xattr`**（`mode:12 | xattr_ptr:52`，宏 `RFS_MODE_XATTR_PACK(mode,xptr)`；`0755`→`RFS_MODE_XATTR_PACK(0755, RFS_XATTR_NONE)`；`0644` 同理）。
- type 值域：`REG=0/DIR=1/SYM=2` → **`{FIFO1,CHR2,DIR4,BLK6,REG8,LNK10,SOCK12}`**（枚举 `INODE_TYPE_*`，值 = `S_IFMT>>12`）。
- `inline_data`：追加语义 —— CHR/BLK 存 rdev @`inline_data[0:4]`（小端 u32）；非设备置 0。insert 建 REG 时其 `inline_data` 仍全零（无影响）。
- 新增 `xattr_ptr`（`RFS_XATTR_NONE` = 52 位全 1）。
- `version 2 → 3`；v1.1 盘不兼容。

**主线二（形式）**：位域 → **掩码 + 宏**
- `ptr_in_idx.free_to_use/ptr` → `struct ptr_in_idx{ uint64_t v; }` + `RFS_PTR_FREE/RFS_PTR_ADDR/RFS_PTR_PACK/RFS_PTR_SET_*`。
- `parent_ptr_t.entry_count/is_inner_node/is_root_node/ptr` → `{ uint64_t v; }` + `RFS_PP_COUNT/IS_INNER/IS_ROOT/PTR(PACK)`。
- `name_heap_controller_t.{bytes_per_bit_log2, total_bits_count_log2, bitmap_base_block_ptr, data_base_block_ptr}` → `{ word0, word1 }` + `RFS_NHC_GRAN/BITS/BITMAP/DATA`、`RFS_NHC_SET`。
- 常量：`DENTRY_INLINE_NAME_MAX=40`、`RAVENFS_MAX_NAME_LEN=552`、`PTR_INVALID=~0ULL` 不变；`NAME_HEAP_INACTIVE = RFS_MASK52`（值同 v1.1 `(1<<52)-1`）；`RAVENFS_VERSION=3`；`RFS_SB_MAGIC=0x5241564E`。
- 魔数：`DENTRY_MAGIC1..4` / `BPTREE_MAGIC1..4`（`include/magics.h`，两版同源）。
- **root inode：0 → 16**（见 A.6 警告）。
- 结构尺寸断言不变：inode 256B、节点 8K、SB 4096B、叶 `entries[127]`（阶 126）、内节点 `hints[127]`。

### D.3 移植风险清单（务必在 ko 侧处理）
1. ~~`inode 0 保留`~~ → **更正（2026-09-26，用户澄清）：v1.2 无保留区**。root=16 是 mkfs 唯一占用的 inode；ko 的 inode 分配器从 0 起正常扫（**无需跳过任何槽**）。
2. `get_processor_id()` 必须强定义（ko 用 `raw_smp_processor_id()`）；god 里它是 extern 合约。
3. 位图 raw 4K 对齐（`enable_slice` 校验 `byte_off & 4095 == 0`）；slice 数组按缓存行对齐。
4. slice 锁内**只做位运算**（不可睡眠）；ko 用 `spinlock_t`，`free` 路径自旋、`alloc` 路径 try。
5. `cfg.min_slice_bits` god **未消费**（硬编码 32768）；ko 若想参数化需改几何函数。
6. 52 位位域比较必须截断（`delete` 的 `PTR_INVALID_52`）；ko v1.2 宏已封装，直接用 `RFS_PTR_ADDR`。
7. god 的 `mdata_piece_session`/`InodeSession` → ko 的 `RAVENFS_MSESSION`/`RAVENFS_SESSION`；god RAII guard → ko `__free()`/`goto` cleanup。
8. `regular_file_reclaim` / `dir_name_heap_release` 属**下游资产维护**，M3 最小写集（create REG / unlink）只需要：REG 的空 inode 初始化（`link_count=1`/`data_root=PTR_INVALID`）+ owner 计数维护；extent/direct 数据回收完整实现归 M4/M5，M3 可先覆盖 inline 形态；dir 急建树根 + rmdir 空判定是 M5 范畴。

# bitmap_slice chunk 化 —— Linux ko 移植进度 + 报告（防失落盘）

> 规格（权威）：`Docs/runtime_tech/03_chunk_backed_slices.md`（§12 = ko 移植注记映射表）
> 参照实现（只读，125 测试全绿）：`juvenile_god/bitmap_slices.cpp` + `include/ravenfs/bitmap_slices.hpp`
> 移植落点：`linux/bitmap.c`（+ `linux/ravenfs_bitmap.h` 注释）
> 日期：2026-09-28 ｜ 施工：本文件每完成一阶段必写（前任 run 死过三个，纪律铁律）
> 红线：**默认 FULL 逐位不变**、**盘 I/O 永不进 slice_lock**、**alloc 阻塞 spinlock（ko 现状，勿改 try）**

## 状态总览（全部完成）

| 阶段 | 内容 | 状态 | 证据 |
|---|---|---|---|
| P0 | 复核 `ravenfs_block_*` 调用点无原子上下文（chg_mu→mutex 前提） | ✅ | 「P0 复核证据」 |
| P1 | 类型/常量/模块参数/结构扩展 + record 池（kmem_cache 预建） | ✅ | 编译零警告 |
| P2 | chunk I/O（sb_bread/mark_buffer_dirty）+ 索引/尾块（§3） | ✅ | 编译零警告 |
| P3 | 换片状态机 + 单窗原语 + 窗口迭代器（alloc/soft/occupy/free/test） | ✅ | 编译零警告 + 走查 |
| P4 | mount 扫描（计数+摘要+三段校验）改造 `rfs_bm_group_load` | ✅ | 编译零警告 + 实测镜像佐证 |
| P5 | TTL 三态分派改造 `rfs_bm_group_sync` | ✅ | 编译零警告 + 走查 |
| P6 | 构建零警告 `make -C linux`（clean 全量） | ✅ | 「验证证据」 |
| P7 | 审查证据（FULL diff 等价 / CHUNK 走查 / god 差异清单）| ✅ | 本文件后半 |

---

## P0 复核证据（chg_mu → 可睡 mutex 前提：无原子上下文）

`ravenfs_block_*` / `ravenfs_inode_*` 全部调用点在**进程上下文**（可睡）：

- `grep -rn "spin_lock\|spin_trylock\|spin_unlock" --include=*.c`（除 `bitmap.c`）→ **零命中**；
  模块内仅有的 `atomic*` 是 inode 行 `acc_stamp`（与位图无关）。
- iomap.c 无自旋锁 → `iomap_begin/iomap_end` 可睡（持 inode_lock 亦可睡）。
- 调用点归属：dentry.c（create/mkdir/unlink/rename → VFS 目录 op，可睡）、dentry_node.c
  （树机械，被 dentry.c 调）、iomap.c、file.c（truncate/punch）、extent_write.c、name_heap.c、
  xattr.c（setxattr）、inode_cache.c（evict → `ravenfs_inode_free`）、super.c（evict_inode/
  put_super/sync_fs/fsync）、cache_ttl.c（delayed_work → 进程上下文，仅调 `ravenfs_bitmap_flush`）。
- 结论：**chg_mu 用可睡 `struct mutex` 安全**（§12 前提成立）。命令留档：
  `grep -rn "ravenfs_block_alloc\|ravenfs_block_free\|ravenfs_block_occupy_soft\|ravenfs_block_alloc_soft\|ravenfs_inode_alloc\|ravenfs_inode_free\|ravenfs_bitmap_flush" --include=*.c`（逐点人工确认，无自旋锁包裹）。

---

## 改动文件（一句/文件）

| 文件 | 一句 |
|---|---|
| `linux/bitmap.c` | +1406/−15 行：文件头补 CHUNK 说明；新增 6 段（常量/模块参数、CHUNK 类型块、record 池、chunk I/O、片级换片状态机+单窗原语+窗口迭代器、mount 扫描+TTL 三态、组级路由）；既有函数仅加**装配期常量分叉**（FULL 旧码逐字不动）。 |
| `linux/ravenfs_bitmap.h` | 仅加注释块：说明 FULL/CHUNK 两后端 + 模块参数开关（接口签名零变化）。 |
| `linux/Makefile` | **未改**（无新增 .c 编译单元，CHUNK 全在 bitmap.c；见「移植完成度」理由）。 |
| `Docs/linux/03_port_progress.md` | 本文件：阶段进度 + 移植报告。 |

---

## 移植完成度

| §12 条目 | 落地 | 说明 |
|---|---|---|
| `chg_mu` → 可睡 mutex | ✅ | `struct mutex chg_mu` 挂片 backing；P0 已证调用点可睡 |
| record 节点池 → `kmem_cache` 预建 | ✅ | `rfs_bm_pool_create()` 用 `kmem_cache_create`+逐节点预建；锁内 `rfs_bm_pool_acquire/release` 零堆分配 |
| `chunk_io` → `sb_bread`/`mark_buffer_dirty` | ✅ | `rfs_bm_chunk_read/write()`；`disk_block_rel` 经 `ravenfs_dev_blk` 换算 FS block 号 |
| mount 扫描改造 `rfs_bm_group_load` | ✅ | CHUNK 走 `rfs_bm_group_chunk_load`（计数+摘要+三段校验，**不 memcpy 进 vzalloc**）；FULL 原样 |
| TTL 改 `rfs_bm_group_sync` 三态分派 | ✅ | CHUNK 走 `rfs_bm_group_chunk_sync`（ONLINE&&dirty / OFFLINE&&recs / 其余跳过）；FULL 原全量 |
| 锁纪律维持 ko 现状（alloc 阻塞 spinlock） | ✅ | 快路径 `spin_lock` 阻塞复检；**不做 try 换片** |
| 默认 FULL；CHUNK 显式开启 | ✅ | **模块参数** `bitmap_backing`（0=FULL 默认）/`bitmap_chunk_order`（默认 4） |
| 索引/尾块严格 §3 公式 | ✅ | `ctl_window`：`lo=ctl<<(15+order)`，`hi=min(lo+chunk_bits,片尾)`；尾块恒 ≥1 整页 |
| 顶层 8 接口签名/语义不变 | ✅ | `ravenfs_block_*`/`ravenfs_inode_*` 头文件零改（仅注释） |
| §10 十条不变量（linux 语义） | ✅ | 见「§10 不变量逐条对照」 |

**为什么 CHUNK 开关选模块参数（记录理由）**：写范围**禁碰 super.c**（挂载项解析在 super.c），
故挂载选项不可行；模块参数是写范围内**唯一**的显式开关，且默认 FULL（不传即零变化）。
代价：全局参数（影响所有挂载）——对「dev/灰测期 A/B」足够，生产化再走挂载项（遗留项）。

**为什么不新增 `linux/bitmap_chunk.c`（记录理由）**：CHUNK 与 FULL 共享 `struct ravenfs_bm_group/
slice`（现为 bitmap.c 私有静态）；拆文件须把这些内部结构搬进头文件 → 破坏封装、扩大暴露面。
任务纪律「不搞过度工程化 + 移植落点现状 bitmap.c」⇒ 单文件内以独立「CHUNK 后端」大节组织
（与 god 把改制层全放 `bitmap_slices.cpp` 的做法一致）。故 `Makefile` 无需改。

---

## 验证证据

### V1. 构建零警告（P6）—— `make -C linux`
```
$ make -C /lib/modules/$(uname -r)/build M=$PWD clean
$ make -C /lib/modules/$(uname -r)/build M=$PWD modules        # 全量 15 个 TU
  CC [M]  bitmap.o
  ...
  CC [M]  .module-common.o
  LD [M]  ravenfs.ko
  BTF [M] ravenfs.ko
# grep -iE '\bwarning\b|\berror\b'（排除工具链 pahole 版本提示行）→ 0 命中
```
- 共 **15 个 TU** 全编，**零 warning 零 error**（`pahole version differs` 是 BTF 工具版本提示，
  基线即有、非代码告警）。
- 加强告警复核：`make KCFLAGS="-Wextra -Wsign-compare -Wtype-limits -Wshadow" … bitmap.o`
  → **bitmap.c 零告警**。
- 模块参数已落盘：`modinfo ravenfs.ko` 可见 `bitmap_backing (int)` / `bitmap_chunk_order (uint)`。
- 链接产物 `ravenfs.ko`（4.7MB）生成成功。

### V2. FULL 默认路径 —— diff 级「与移植前行为一致」佐证
FULL 下 `backing_mode==RFS_BM_BACKING_FULL`，所有 `if (mode==CHUNK)` 分叉**恒假**，其后旧码逐字未动。
`git diff` 中对**既有函数的改动**仅下列（其余全是新增块）：

1. `rfs_bm_group_setup`：`words = vzalloc(...)` 外包一层 `if (mode != CHUNK)`（FULL 恒真→同旧）；
   `s->hint = (mode==CHUNK)?0:base`（FULL 取 `base`，**同旧**）；新增 `s->backing=NULL`（无行为）。
2. `rfs_bm_group_alloc / _alloc_soft / _occupy_soft / test_interval / free`：各加一条
   `if (mode==CHUNK) return chunk_xxx;`（FULL 不进入）；旧循环体**未动**。
3. `rfs_bm_group_load / _sync`：各加 `if (mode==CHUNK) return chunk_xxx;`；旧体未动。
4. `ravenfs_bitmap_create`：新增 11 行**字段赋值**（mode/sb/is_inode/order/pool_depth/self_check）
   + `pr_info` 串加 `backing=FULL` 前缀；`bad:` 路径改用 `rfs_bm_group_release`（FULL 下 =
   原先的 vfree/kfree 顺序，**等价**）。
5. `ravenfs_block_test/inode_test`：加一条恒假的 CHUNK 分叉；直读行未动。
6. 结构体新增字段（`slice.backing`、group 的 CHUNK 字段）——不影响 FULL 布局语义。

⇒ **FULL 位操作、几何推导、片级同步、装载逐位不变**；唯一可见差异 = `pr_info` 文案多
`backing=FULL`（非功能）。`ravenfs_bitmap.h` 仅加注释（签名零变）。

### V3. CHUNK 路径 —— 代码走查（装配 → 分配 → 释放 → TTL → 重装盘读回一致）
> guest 实跑由**测试团队 harness** 负责（见「Harness 怎么跑我这份代码」）；此处为逐环节代码走查。

- **装配**：`create` 读模块参数 → 双组 `backing_mode=CHUNK`；`group_setup` **跳过 vzalloc(words)**
  （省 80MB），逐片置 `backing=NULL`、`hint=0`；`group_chunk_setup` 夹断 order →
  `pool_create`（kmem_cache 预建）→ 逐片 `kzalloc(backing)` + `kvmalloc(buffer/snapshot)`
  + `kcalloc(ctls)` + 预构造每控（`disk_block_rel=(begin+wlo)/32768`、`tail_bits=whi-wlo`、
  `summary=MIXED`）。→ `group_chunk_load`：逐片逐控 `mount_scan`：`sb_bread` 读该窗 → 数 1/0 →
  `free_count += zeros` + 摘要真值 + **三段校验**（block 尾段全 1 / inode root 位亮灯，失败拒挂）。
- **分配（硬/软）**：`group_chunk_alloc[_soft]` 按 `preferred[cpu]` 轮询片 → `slice_alloc_*`：
  读 hint → 定 hctl → 正向窗序列 [hctl,nwin) → 绕回 [0,hctl]；每窗 `chunk_lock_window`
  （快路径：`spin_lock` 复检 online==ctl，纯内存；不在线 → 摘要 ALL_USED 免载入跳过 → 放锁
  → `ensure_online` 慢路径 → 重试）→ 单窗 `win_alloc_*`（`find_free_run` 夹 [from,to)）→ 命中置位 +
  `free_count-=n` + `ctls[c].dirty=1` + 返回**全局位号** `base_idx+hit`。
- **释放**：`group_chunk_free` 逐片切段 → `slice_free`：在线窗 → `clear_bits_locked`（1→0 计数 + dirty，
  零 record 零 I/O）；离线窗 → `pool_acquire` 取节点挂片级链 + **降级覆盖 ctl 摘要为 MIXED** +
  `free_count+=n`（零 I/O、O(1)）；**池空兜底** → 解锁 → `ensure_online` 强制上线 → 重锁直清。
- **TTL（`ravenfs_bitmap_flush` → `group_sync` → CHUNK 三态）**：逐片逐控持 `chg_mu`：
  态1 `online==ctl && dirty` → 锁内快照+清 dirty → 锁外 `chunk_write` → 重算摘要；
  态2 `offline && has_recs` → 读盘入 snapshot → `drain_window`（应用 record+摘节点）→ 写回 → 重算；
  态3 其余跳过。→ 只写脏/有待还 chunk（消除 80MB/30s 写放大）。
- **换片（`ensure_online`，§5 核心）**：`mutex_lock(chg_mu)` → 锁内：若 online==ctl 早退；否则
  若旧窗 dirty → 快照+清 dirty（old 窗 buffer 内容）→ `online=NONE`+gen++ → 解锁 →
  （锁外）旧窗 `chunk_write(snapshot)`（失败→恢复旧窗在线+复脏+重算，状态零损坏）→ 重算旧窗摘要
  → （锁外）目标窗 `chunk_read`（摘要 `ALL_FREE` 且无 recs → 免读 memset 0）→ 锁内 **先 drain**
  record（清位数>0 → 置 dirty）→ 发布 `online=ctl`+gen++ + `slice_base=buffer-tlo/8` → 解锁。
- **重装盘读回一致**：TTL/同步把脏窗/还账窗写回盘；重装时 `mount_scan` 重新从盘数 free_count +
  摘要 + 校验，得到与落盘态一致的位图。若卸载前有 unclean 窗口残留 = **泄漏方向**（§1.5，
  fsck 可收），**不引入双分配方向**（§10.5/§10.7 保证）。

### V4. mount 自描述校验 —— 用真实 mkfs 镜像实测佐证（离线验证）
用仓库 `mkfs/mkfs.ravenfs` 生成 v1.2 镜像（256MB，total_blocks=65535、inode_count=8192、
bitmap=[65533,65535)）后按 ko 几何直接读盘校验：

```
block self-check [inode_bm_base=65020, blk_bits=65536) ：0 个 0 位（全 1）✅
block tail bit 65535 = 1                                   ✅
inode bit16(root) = 1 ；inode bit0 = 0                     ✅（★ v1.2 root=16，非 0）
inode tail [8192, 32768) 全 1                              ✅
```
⇒ ko CHUNK 三段校验对本镜像**可通过**；同时实证 **root 校验位必须是 `root_ino(16)` 而非 god 的
bit0**（若照抄 god 的 bit0，ko v1.2 100% 拒挂）——已按 `sbi->root_ino` 实现。

---

## §10 不变量逐条对照（linux 语义）

1. FULL 逐位不变 —— ✅（V2）。
2. 每片 `online_chunk ∈ {NONE,唯一}`；buffer ≡ 盘态⊕未回写⊕已 drain —— ✅（`ensure_online` 发布时置 NONE→读→drain→发布；下线置 NONE 冻 buffer）。
3. 盘 I/O 只在持 chg_mu 下；slice_lock 内零 I/O/零睡眠/零阻塞分配 —— ✅（`chunk_read/write` 全部在 chg_mu 段且**在 slice_lock 之外**；池取还为原子操作）。
4. 发布前 record 必已 drain —— ✅（`ensure_online` 锁内先 `drain_window` 再 `online=ctl`）。
5. `summary==ALL_USED ⇒ 无 pending record` —— ✅（挂 record 必把覆盖 ctl 降级 MIXED；`ALL_USED` 与 `has_recs` 互斥）。
6. free_count/全局计数与驻留无关，gate/statfs 保真 —— ✅（mount 扫描填初值；在线 free 实清计数、离线 free 挂账计数）。
7. record 只被 drain 消费、不丢；crash 丢 = 泄漏方向 —— ✅（drain 幂等；池空保留整节点）。
8. tail==0 或 ≥1 整页；buffer/ctl/盘块 4K 对齐 —— ✅（ko 片位数恒 32768 倍数 → 尾恒 ≥1 页；buffer 页/8 对齐）。
9. 池空兜底 = 强制上线直清，不阻塞/不越序/不产生第二批记录 —— ✅（`slice_free` 兜底分支）。
10. preferred 起扫纪律 + 锁语义各保持现行为（ko 阻塞） —— ✅（`group_chunk_alloc*` 沿用 `preferred[cpu]` 轮询；全 CHUNK 路径用阻塞 `spin_lock`）。

---

## 与 god 实现差异清单

> 必列三点（坐标系 / 锁 / I/O 后端）+ 其余实现差异。

| # | 维度 | god（`bitmap_slices.cpp`） | ko（`linux/bitmap.c`） | 理由 |
|---|---|---|---|---|
| 1 | **坐标系** | 片内偏移（`slice_base=buffer−tlo/8` 重指 + 片内 index），口径全片内 | **全局位号**对外，窗口用「片内偏移=全局−base_idx」；CHUNK 下 `hint` 存**片内偏移**、输出 `base_idx+hit` 全局 | ko 既有 FULL 全用全局位号（`g->words` 全局索引）；对外零换算契约（02 §3.4） |
| 2 | **锁** | slice_lock = `rw_try_spinlock_t`（**try**）→ 忙返 `RC_CHUNK_BUSY` 交路由**换片** | slice_lock = `spinlock_t`（**阻塞**，2026-09-28 用户裁）；无 BUSY→换片，换片竞争以有限重试+跳过窗处理 | ko 现状纪律 = 阻塞；§10.10「两边各保持现行为」 |
| 3 | **I/O 后端** | 抽象 `bitmap_chunk_io_t` 回调（用户态 fd/mmap，保持模块零设备知识） | 直接 `sb_bread`/`mark_buffer_dirty`/`sync_dirty_buffer`（经 `ravenfs_dev_blk`） | ko 侧位图天然有 sb，规格 §12 明示 |
| 4 | record 池并发原语 | 无锁 tagged(32\|32) 原子 LIFO（CAS 抗 ABA） | **微型 spinlock 护空闲链**（node->next），锁序 片锁→池锁 | 内核惯用法更简、天然免 ABA 推理；保持「锁内零分配」等价保证 |
| 5 | record 节点存储 | `new rec_node[depth]` 连续数组 | `kmem_cache_alloc` 逐节点预建 + `all[]` 拆池 | 规格 §12 明示 kmem_cache |
| 6 | 尾块/摘要 | 同 §3/§7 | 同 §3/§7（数学一致） | — |
| 7 | 自描述校验 root 位 | 硬编码 inode bit0（v1.1 root=0） | `sbi->root_ino`（v1.2=**16**） | **代码生殖隔离**：v1.2 root=16（实测 bit0=0），照抄 bit0 会拒挂 |
| 8 | 装配期 chunk_order 越界 | `order>8 → RC_ERR_STATE`；`chunk_bytes>片字节数 → 失败` | `order>8 → 夹到 8`；`chunk_bytes>片字节数 → **下调 order** 并 `pr_warn` | 「可用性/可靠性优先」：失败=拒挂（不可用）；夹断保挂载 |
| 9 | `test_interval` CHUNK 版 | 未单列（mgr 无区间测试接口） | 实现为逐位 `chunk_test_bit`（按需换片） | ko 有 `ravenfs_block_test_interval`（D2/D3），补齐语义 |
| 10 | TTL 触发 | 上帝类 30s 定时器（接口 `chunk_ttl_sync`） | 复用 ko 既有 `cache_ttl.c` 30s 闸门 → `ravenfs_bitmap_flush` → `group_sync` 分派 | ko 已有 TTL 基建，零新定时器 |

> 一致点：§3 索引公式、§5 快慢路径状态机、§6 record 语义（在线直改/离线挂账/上线前 drain/池空兜底/摘要降级）、§7 mount 计数+摘要、§9 三态分派——**逐条对齐**。

---

## 遗留事项（含 ko 特有风险）

1. **【ko 特有·中】`block_test/inode_test` 的睡眠语义变化**：FULL 下是无锁直读（任何上下文可调）；
   CHUNK 下 `chunk_test_bit` 可能 `ensure_online`→`mutex_lock`+盘 I/O（**可睡**）。当前模块内
   **无人调用** `ravenfs_block_test/inode_test`（grep 零命中），故无现实影响；**未来调用方必须
   在可睡上下文**。连同 `ravenfs_block_test_interval` 同理。已在文件注释声明。
2. **【ko 特有·低】CHUNK 下 `alloc_hard` 不跨 chunk**：单片同时仅一个 chunk 上线 ⇒ 单次硬分配上限 =
   一个 chunk（order 4 = 524288 位 = 16384 块）。FULL 无此界。当前最大硬分配 = 2 块（节点）×小量，
   远低于此界；记为 A/B 记账项。
3. **【ko 特有·低】mount 自描述校验仅在 CHUNK 生效**：FULL 沿用现状（无校验）。若用**旧 mkfs**
   （未写 tail 全 1）产盘 + `bitmap_backing=1` → CHUNK 拒挂（FULL 可挂）。v1.2 现行 mkfs 已实测通过（V4）。
4. **【差异·低】record 挂账 `free_count += n` 口径**：同一区间重复 free 会虚高 `free_count`
   （FULL 按实清 1→0 不会）。正确调用方语义下无差（god 同款已知差异）。
5. **【低】align 语义**：CHUNK 单窗 `find_free_run` 在**片内偏移**上取对齐；因片首 32768 对齐，
   对 `align ≤ 32768` 与全局对齐等价。当前最大 `align_log2=1`（align=2），无影响；仅当未来
   `align_log2>15` 才需专门处理（FULL 走全局索引不受影响）。
6. **【低】crash 窗口**：与 M3.5 同构——flush 外不损坏；flush 中途 dangling = 泄漏方向（fsck 可收）。
7. **【遗留·后续任务】挂载项化 + A/B 压测接线**：生产化建议把 `bitmap_backing/chunk_order` 升级为
   `mount -o`（需改 super.c，本任务禁改）；§11 A/B（FULL/CHUNK/mmap）压测由专项任务做。
8. **【遗留】`pool->nfree` 仅审计用**：可在 `chunk_ttl_sync`/debugfs 暴露剩余池深，本轮未接线。

---

## Harness 怎么跑我这份代码（给测试团队）

本移植**不改顶层 API**，测试团队既有 harness（`tests/`+`vm/`）可按以下方式驱动 CHUNK：

1. **加载带参数**（或在 `modprobe.d` 里写 `options ravenfs bitmap_backing=1 bitmap_chunk_order=4`）：
   ```
   insmod ravenfs.ko bitmap_backing=1 bitmap_chunk_order=4
   # 或： modprobe ravenfs bitmap_backing=1 bitmap_chunk_order=4
   ```
   默认（不传）= FULL，行为与移植前逐位一致（**A/B 基线**）。
2. **挂载**：`mount -t ravenfs -o loop <img> /mnt`（CHUNK 的 mount 扫描 = 顺序 `sb_bread` 全位图，
   与 FULL 装载同价 I/O，只不 memcpy 驻留）。
3. **A/B 对测**：同镜像分别以 `bitmap_backing=0/1` 挂载跑同一负载（create/unlink/大目录/文件 IO），
   比对：分配/释放 p50/p99、ENOSPC 扫描耗时、mount 耗时、常驻 RSS、TTL 写放大（字节/30s）。
   判据（§11 预设）：CHUNK 常态吞吐 ≥ FULL 的 95% 且 RSS ↓ ≥10×。
4. **一致性收口**（§10 端到端）：`sync` 后 `umount`，**重装**再用 `audit_v1.2` / `statfs free` 校验
   盘态 == 预期；`echo b > /proc/sysrq-trigger` 崩测后 audit（泄漏方向可 fsck 收，不应见双分配）。
5. **回归**：FULL 路径跑全部既有用例（M3/M5 命名空间 + T3），应**全绿且零行为差**。

> 注意：`bitmap_backing` 是**全局**模块参数（写范围不能改 super.c 做挂载项）；改值需重新
> `rmmod/insmod`（或对已挂载 fs 用已生效值）。见「遗留 7」。

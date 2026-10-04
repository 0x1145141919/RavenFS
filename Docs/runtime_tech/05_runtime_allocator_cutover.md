# RavenFS 运行时科技 ⑤ — 位图分配器 → 树分配器：调用点切换（施工备忘）

> 2026-10-04
> 状态：**待裁（施工备忘 / cutover memo）** —— 记录「把 FS 分配调用从位图面切到
> `rfs_allocator_top.h` 直调」的 ABI 差、调用面、地雷与待拍板项。**未施工**。
> 设计母体：`04_sharded_master_slave_rbtree.md`（运行时树 = 多分片主从红黑树；磁盘契约 = 位图）。
> 接口契约（已冻结）：`include/linux/ravenfs_allocator_top.h`（router）/ `ravenfs_allocator_slice.h`（片）。
> 生命周期接线（已就位）：`linux/allocator_adapter.c` + `ravenfs_allocator_adapter.h`（`allocator_takeover` 闸，默认 off）。

---

## 0. 一句话

> **运行时真值从「`rfs_bm` 位图」切到「`rfs_allocator` 主从树」：FS 调用点直调
> `rfs_allocator_*`，绕过位图适配面（`ravenfs_bitmap.h` / `ravenfs_bitmap_adapter.h`）；
> 磁盘契约仍是位图（不动），位图降级为 checkpoint 底座 + 落盘载体。**

- 「保守替换」= **只用保行为的基础子集**（hard alloc / free / occupy_soft / inode alloc·free），
  暂不碰高级面（soft 多段 run、双树 best-fit 调优、亲和/跨片策略……）。
- 基线：树分配器用户态冒烟 `tests/test_allocator_router` **207 PASS / 0 FAIL**（2026-10-04 复核）。

---

## 1. ABI 对照（旧 sb 面 ↔ 新 self 面）

| 旧（`ravenfs_bitmap.h`，`bitmap_adapter.c`） | 新（`ravenfs_allocator_top.h`） | 形状 / 语义差 |
|---|---|---|
| `u64 ravenfs_block_alloc(sb, count, align_log2)` | `u64 rfs_allocator_block_alloc_hard(self, count, align_log2)` | 同形；**未命中哨兵同值** `PTR_INVALID == RFS_IV_NO_HIT == ~0ULL` → 调用点判空**零改** |
| `void ravenfs_block_free(sb, blk, count)` | `int rfs_allocator_block_free(self, blkbase, count)` | 多返 int（调用点可忽略） |
| `u64 ravenfs_block_occupy_soft(sb, base, count, &rest)` | `int rfs_allocator_block_occupy_soft(self, blkbase, count, &rest_count)` | **返回语义变**：旧 = 命中 base 或 `PTR_INVALID`；新 = `0`/`-ENOSPC`，**结果即入参 blkbase 原址** |
| `u64 ravenfs_inode_alloc(sb)` | `int rfs_allocator_inode_alloc(self, &out_inum)` | **出参化**（`0`/`-ENOSPC`） |
| `void ravenfs_inode_free(sb, inum)` | `void rfs_allocator_inode_free(self, inum)` | 同形 |
| `bool ravenfs_block_test(sb,blk)` / `ravenfs_inode_test(sb,inum)` | 同名 `rfs_allocator_*` | 同形（**零调用者**，暂不映射） |
| `u64 ravenfs_block_free_total(sb)` / `_inode_free_total` | `u64 rfs_allocator_*_free_total(self)` | 同形（**零调用者**） |
| `u64 ravenfs_block_alloc_soft(sb,count,&remain)` | `int rfs_allocator_block_alloc_soft(self,count,align,runs)` | **语义不同**：旧 = 单 base + 缺口；新 = all-or-nothing 多段 run 链。**零调用者**，不在保守路径 |
| `int ravenfs_block_test_interval(sb,base,len,val)` | （新头无对等物） | **零调用者**，不在保守路径 |

**结构对拍（已核源码，✓）**：旧 `rfs_bm_block_alloc_hard` 与新 `rfs_allocator_block_alloc_hard`
同为「per-CPU `preferred` 起、环扫全片、**不跨片**」。→ hard 路径行为同构。

---

## 2. 调用面（真改范围 = 72 处 / 9 文件）

| 旧 API | 处数 | 主要文件 |
|---|---|---|
| `ravenfs_block_alloc` | 17 | dentry / dentry_node / iomap / name_heap / xattr / extent_write |
| `ravenfs_block_free` | 45 | super / dentry / dentry_node / iomap / name_heap / xattr / extent_write / file |
| `ravenfs_block_occupy_soft` | 1 | iomap（`ravenfs_data_alloc_anchored` 锚定左邻） |
| `ravenfs_inode_alloc` | 3 | dentry |
| `ravenfs_inode_free` | 6 | inode_cache / dentry |

**零调用者（保守步不碰）**：`block_alloc_soft` / `block_test_interval` / `block_test` /
`inode_test` / `block_free_total` / `inode_free_total`。

---

## 3. 关键结论与地雷

### 3.1 ✓ 落盘链天然兼容（最重要的好消息）

`rfs_bm_snapshot_slice`（MEM）实现 = `memcpy(dst, g->words + …, bits>>3)` —— **只拷贝
words 缓冲，不重算**（`linux/bitmap_mem.c:386`）。故：
- `ravenfs_allocator_flush`（树快照 → 补集写 words）+ `ravenfs_bitmap_flush`（words → 盘）**链成立**；
- **顺序**（allocator_flush 先、bitmap_flush 后）在 `super.c`（put_super / sync_fs）与
  `cache_ttl.c` 已满足 → **落盘/契约层零改**。

### 3.2 地雷

| # | 地雷 | 影响 | 处置 |
|---|---|---|---|
| L1 | **双真值**：切换后 `rfs_bm` 不再收 alloc/free，其 words/自由计数只在 checkpoint 被回填 | 混用（部分调用点走位图、部分走树）= 撕裂 | **必须全切**，不留混用；或保留闸门使二选一 |
| L2 | **句柄不可达**：调用点只有 `sb`，`sbi->alloc` 是不透明 `struct ravenfs_alloc_mgr *` | 无法直调 `rfs_allocator_*` | 见 §4 Fork-1 |
| L3 | **闸门 / 后端**：`allocator_takeover=off` 或 CHUNK 后端（无全量 words）时树为空 | 直调空句柄 = NPE | 见 §4 Fork-2 |
| L4 | **物理落点不同**：树 = 片内 best-fit（从树取 `len≥N` 最小者）；位图 = 片内 first-fit | 字节级落点不同，**语义级一致**（不影响正确性） | 记录；压测只验不变量 |
| L5 | **statfs / 审计口径滞后**：`ravenfs_bitmap_used` 走**磁盘读**；`bm_*` 计数器源自 `rfs_bm` | 树时代这些读数陈旧（非正确性） | 后续改读 `rfs_allocator_*_free_total` / 树统计（高级阶段） |
| L6 | **锁**：树片锁 = 可睡 mutex；旧位图 MEM 片锁亦为 mutex | 一致（均可在 FS 路径可睡上下文持） | ✓ 无差 |

---

## 4. 待裁（fork，请设计方逐条裁）

### Fork-1 调用形状：门面 vs 直调

- **甲（保守·建议）**：在 **allocator 层**（`allocator_adapter.c` / `ravenfs_allocator_adapter.h`，
  **非** `bitmap_adapter`）加 5 个 sb 门面，集中做「哨兵/errno/NULL/闸门」翻译；调用点只换函数名。
  → ~65 处机械改；**默认 off = 旧路径逐字节不变**；翻译单点可测。
- **乙（贴合字面「直调」）**：`sbi->alloc` 改为 `struct rfs_allocator *`（`ravenfs.h` 前向声明），
  调用点 `#include "ravenfs_allocator_top.h"` 直调 `rfs_allocator_*`。
  → 每站点自处理空句柄 / 出参 / errno；~65 处展开；`allocator_adapter` 内部改 `container_of` 回收 mgr。

> 甲仍是"直呼 `rfs_allocator_*`"（只是住在本层门面），且**已绕过 `bitmap_adapter`**；
> 乙更贴字面，但把空句柄/后端取舍摊到每个站点。**AI 建议：保守期取甲**。

### Fork-2 闸门与后端

- **α（保守双轨）**：保留 `allocator_takeover`；树未接管（off / CHUNK）时调用点回退位图。
  → 旧路径默认不变、新路径可灰度；代价 = 调用点带分支（甲天然支持）。
- **β（单轨切换）**：树为唯一运行时分配器，取消闸门；**MEM 强制**、CHUNK 后端需另定
  （下线 or 补「经 CHUNK 读 → 重建树」的装载路径）。

> **AI 建议：保守期取 α**（默认 off，guest 压测通过后再翻默认/删旧路径）。

---

## 5. 建议施工步骤（小步快跑；每步可编译/可验收）

1. **Step1（本步·可编译）**：`ravenfs_allocator_adapter.h` 声明 + `allocator_adapter.c` 实现 5 门面
   （`_hard` / `_free` / `_occupy_soft` / `_inode_alloc` / `_inode_free`），**不改调用点** → 宿主编译过。
2. **Step2**：逐文件切调用点（建议 order：iomap → dentry_node → dentry → extent_write → name_heap
   → xattr → file → inode_cache → super），每文件一提交、每步编译。
3. **Step3（真闸门）**：guest 挂载（takeover=on）+ `audit_v1.2` + churn 压测对拍，**过闸**。
4. **Step4**：翻默认 /（乙）改直调 / 清旧位图运行时路径。

> 纪律 §7：宿主只编译不 insmod；guest 压测是实体机灰测硬前置。

---

## 6. 验证清单（Step1 起）

- [ ] 宿主：`make -C linux`（只编译）+ `tests/test_allocator_router` 冒烟 207/0。
- [ ] 门面翻译对拍：alloc 哨兵、occupy 返回、inode 出参三处与旧面逐条等价。
- [ ] guest：挂载 → `audit_v1.2` 不变量 → churn（alloc==free，live==0）→ statfs 合理性。
- [ ] 落盘：`allocator_flush → bitmap_flush` 顺序在 TTL / sync_fs / put_super 三处均保持。

---

## 7. 状态

- **已裁（2026-10-04，设计方）**：**Fork-1 = 乙（直调）**、**Fork-2 = β（单轨硬切）**——不妥协、不留回退。
- **Step A 已落地（2026-10-04，未 git commit，工作树）**，见 §8。
- 下一步 = **Step B（guest 压测，真闸门）** + **Step C（清 CHUNK / 死符号）**。

---

## 8. 施工记录（Step A：调用点硬切，2026-10-04）

**裁定**：乙（FS 调用点直调 `rfs_allocator_*`）+ β（树为唯一运行时分配器，删 `allocator_takeover` 闸，CHUNK 退役）。

**落地内容**
- `include/linux/ravenfs.h`：前向声明 `struct rfs_allocator`；`sbi->alloc` 字段类型 `struct rfs_allocator *`（挂载必建、无回退）。
- `linux/allocator_adapter.c`：mgr 首成员内嵌 `struct rfs_allocator alloc`；`sbi->alloc = &m->alloc`；flush/destroy `container_of` 回收；**删 `allocator_takeover`**；非 MEM 后端（无 words）→ `-EOPNOTSUPP` 建树失败（挂载失败）。
- `include/linux/ravenfs_allocator_adapter.h`：头注改「无回退（硬切）」。
- 9 个调用点文件（dentry / dentry_node / extent_write / file / inode_cache / iomap / name_heap / super / xattr）：`#include "ravenfs_allocator_top.h"` + 直调 `rfs_allocator_*`；occupy_soft / inode_alloc 形状翻译见 §1。
- `include/linux/ravenfs_bitmap.h` + `linux/bitmap_adapter.c`：删 11 个旧分配/测试/统计门面（-151 行）；**保留** create/create_mode/destroy/flush/words_raw/opts_get + `rfs_bm` create/fini/snapshot/几何（checkpoint 底座 + 落盘载体）。
- `linux/super.c`：put_super / sync_fs 保持 `allocator_flush → bitmap_flush` 顺序；put_super 加 `allocator_destroy`。

**独立复核（Raven，非子代理自评）**
- 冻结头（allocator_top/slice/platform/rbtree×2/run_list/disk_struct_v1.2）**未触碰**（mtime 早于改动）。
- `make -C linux`（touch 全 .c 强制全量重编）**EXIT=0、零 warning/error**。
- userspace：`test_allocator_router` **207 PASS / 0 FAIL**；`test_allocator_slice` **98 PASS / 0 FAIL**。
- grep：旧调用名残留 = **NONE**；`takeover` 残留 = **NONE**。
- 逐文件 diff 复核：11 处形状翻译（哨兵 / occupy 返回 / inode 出参）正确；错误路径 free 未漏。

**Step A 遗留（待后续）**
1. **CHUNK 运行期不可达**：挂载在建树处失败；但 CHUNK 符号仍被 `rfs_bm` 分派 + `bitmap_adapter.c` flush 分支静态引用。
2. **死符号**（零调用者，仍编译）：`rfs_bm_block_alloc_hard/_soft/_occupy_soft/_free/_test/_test_interval`、`rfs_bm_inode_alloc/_free/_test`（定义在 `bitmap_core.c`）。→ Step C 清。
3. **`-EOPNOTSUPP` 被挂载路径折叠为 `-EIO`**：`ravenfs_allocator_create` 返 -EOPNOTSUPP，但 super.c fill_super 统一按 -EIO 收；仅错误码观感，无正确性影响。
4. **无 NULL 句柄护栏**（硬切使然）：分配调用点假设「挂载成功 ⇒ `sbi->alloc` 非空」；若存在卸载后仍触发分配的路径需另加断言。
5. **statfs/审计口径滞后**（L5 已知）：`ravenfs_bitmap_used` 仍走磁盘读。
6. **陈旧头注**：`ravenfs_bitmap_adapter.h` 仍写「13 个公共 `ravenfs_*` 签名冻结」，与现状不符（未改，待顺）。

**未验收声明**：以上均为**宿主编译级**。**行为级变更未过 guest 压测**（纪律 §7：guest 是唯一无损试验场，宿主 dev build 不算验收）→ Step B 待办。

---

## 9. 位图模块整体删除后的细修（fine prune，2026-10-04）

**事件**：设计方**粗修剪**——删除 `include/linux/ravenfs_bitmap{.h,_adapter.h,_backend.h,_core.h}` +
`linux/bitmap_{adapter,chunk,core,mem}.c` + `linux/allocator_adapter.c`（未跟踪，故不可 git 恢复），
要求「细修过编译」。

**关键影响**：粗修剪连**checkpoint 底座 + 落盘载体**（位图镜像 words + 块设备 IO）一并删掉；
而树分配器建树/checkpoint 依赖它。→ 细修把该层**收归 allocator_adapter**（符合 F5：落盘归 FS 适配层）。

**细修内容（Raven）**
1. `linux/Makefile`：移 `bitmap_adapter.o bitmap_core.o bitmap_mem.o bitmap_chunk.o`。
2. **重建 `linux/allocator_adapter.c`（bitmap-free）**：mgr 自持 `blk_words/ino_words`（vzalloc，4K 对齐）
   + 设备读写（`sb_bread` 装 / `mark_buffer_dirty`+`sync_dirty_buffer` 写）；`flush` = 树快照 → 补集写 words
   → words 写设备（= 旧 `allocator_flush + bitmap_flush` 合一）；ro 挂载只 checkpoint 不写盘。
3. `include/linux/ravenfs.h`：删 `struct ravenfs_bitmap_mgr` 前向声明 + `sbi->bitmap` 字段（几何 `ravenfs_bitmap_blocks()` 保留）。
4. `linux/super.c`：删 bitmap include / `bm_opts` 管线 / `bitmap_mode|bbco|ibco` 挂载选项 / debugfs+show_options
   的位图段；create/destroy/flush 改走 `ravenfs_allocator_*`；fsync 改 `ravenfs_allocator_flush`。
5. `linux/cache_ttl.c`：趟3 只调 `ravenfs_allocator_flush`。
6. 10 个 `.c` 删 `#include "ravenfs_bitmap.h"`。

**验证（宿主）**：`make -C linux`（touch 全 .c 全量重编）**EXIT=0、零 warning/error**；`ravenfs.ko` 5.1MB；
残留符号 grep = **仅注释**（无存活引用）。

**遗留（cosmetic / Step C）**
- `sbi->stats.bm_*` 18 字段 + `ravenfs_stat_names[]` 名字串仍在（位图主题但无害）；
- `cache_ttl.c:11` 注释、冻结头（platform/slice/top）注释仍提位图；
- `tests/CMakeLists.txt` + `tests/bitmap_core_v2/` 等仍引已删 `../linux/bitmap_core.c` → **cmake 用户态测试会断（非 ko）**；
- `linux/README.md` / `PKGBUILD` 注释提 `bitmap.c`。

**状态**：宿主编译级通过；**guest 压测（Step B）仍未做**。

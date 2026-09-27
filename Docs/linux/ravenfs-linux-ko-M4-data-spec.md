# RavenFS → Linux KO：M4 文件数据需求规格（定调版）

> 2026-09-27 · Raven · 用户定调（三条）+ 路线裁决 = **iomap**
> 上位文档：`ravenfs-linux-ko-requirements-spec.md`（第一约束实体化，**冲突以 spec 为准**）；
> 本文是 **M4（文件数据 / G1·G2）的第二约束实体化**：把「走哪个内核框架 / 怎么接线」钉死。
> 机械参考（god，谱系非代码）：`Docs/pwrite_pipeline.md`（五段管线）、`Docs/punch_truncate_pread_spec.md`、
> `juvenile_god/direct_file_ops.cpp`（`df_pipeline`）。
> **一句话**：M4 的文件必须 **mmap-able**、必须走 **read_iter/write_iter**（iomap 路线）、
> **写时立刻分配（强同步，非 delalloc）**；读粒度用 **iomap 默认**（不做自研 64K 单元）。
> 〔2026-09-27 晚更新：用户裁决 **O-4 撤销 D-M4-4**、**O-5 撤销 D-M4-3**。〕

---

## 0. 定调（用户裁决，2026-09-27）

| ID | 定调 | 性质 | 说明 |
|---|---|---|---|
| **D-M4-1** | **文件必须能被 mmap** | **硬约束（不可降级）** | 含可读 / 可写 / 可执行（动态加载器 mmap 全部 `.so`）。→ `a_ops->read_folio` 是前置（无它 `generic_file_mmap_prepare` 直接 `-ENOEXEC`，`mm/filemap.c`） |
| **D-M4-2** | **必须走 `read_iter`/`write_iter` 路线** | **硬约束** | 不再手搓 `direct_file_pwrite` 式同步总装；缓冲读写全部经 VFS 迭代器 |
| ~~**D-M4-3**~~ | ~~buffered cache_miss 全量加载（64K 单元）~~ | **撤销（2026-09-27 晚，O-5）** | 见 **D-M4-7** |
| ~~**D-M4-4**~~ | ~~分配时机 = 写回才分配（delalloc）~~ | **撤销（2026-09-27 晚，O-4）** | 见 **D-M4-6** |
| **D-M4-6** | **分配时机 = 立刻分配（强同步）** | **已决（2026-09-27，O-4）** | `iomap_begin(WRITE)` **立刻真分配**并返 **`IOMAP_MAPPED`**（**非** `IOMAP_DELALLOC`）；依据 = 位图分配器**纯内存多分片**（分配零 I/O、极廉价，无推迟动机）→ **ENOSPC 在 `write(2)` 即暴露**。必须记录 `f_off→blkidx`（逻辑→物理映射）——extent 树 / direct 表示即为此而设 |
| **D-M4-7** | **读粒度 = iomap 默认** | **已决（2026-09-27，O-5）** | 不做自研「64K 单元 / 大 folio」；用 iomap 默认（`read_folio` + readahead）。**后续有时间/token 再研究** |
| **D-M4-8** | **三态迁移基调 = inline 默认升格 extent** | **已决（2026-09-27，O-1）** | inline 写越 160 → **默认升格为 extent**；**direct 仅作出口坍缩的收敛态**（末尾发现单 extent 覆盖 `[0,size)` 再缩 direct），**不作初始分配态** |
| **D-M4-9** | **不做 `iomap_write_ops.iomap_valid`** | **已决（2026-09-27，O-10）** | 因 O-4 立刻分配 → 映射**自创建起稳定**，无 stale 窗口；单挂载 + VFS `inode_lock` 串行化已足 |
| **D-M4-10** | **短写尾巴：保留 + `evict_inode` 清算** | **已决（2026-09-27，O-11=(b)）** | `iomap_end` 对「已分配但未写完」区间 **不 free**（短期浪费可接受）；改在 **VFS `evict_inode`（inode 驱逐）时「秋后算账」** trim 超出 `size` 的意外尾块。⇒ 常驻文件的尾巴挂到 **unlink / umount** 才清算（可接受，见 §8）。因 **KEEP_SIZE 已拒（D-M4-11）**，「> `size` 分配」只剩短写尾一类，**无 fallocate 误伤** |
| **D-M4-11** | **`fallocate` 拒绝 `FALLOC_FL_KEEP_SIZE`（纯预留）** | **已决（2026-09-27，O-12）** | `.fallocate` **只受理 `flags==0` 的普通预分配**（会抬 `size`）；**`FALLOC_FL_KEEP_SIZE`（纯预留、不抬 size）→ `-EOPNOTSUPP`**（第二约束「降级」路径）⇒ 盘上**永无「已分配但 > `size`」的 fallocate 预留** → **O-12 消解**。**例外**：`FALLOC_FL_PUNCH_HOLE\|FALLOC_FL_KEEP_SIZE`（打洞，R-FD-08，**该位是 ABI 强制**）与 `FALLOC_FL_ZERO_RANGE` **照常受理** |
| **D-M4-12** | **`mmap` 写必须立刻分配（`page_mkwrite`）** | **已决+已落（2026-09-27，O-13=(a)）** | `vm_ops.page_mkwrite = ravenfs_filemap_page_mkwrite → iomap_page_mkwrite(vmf, &ravenfs_iomap_ops, NULL)`；`iomap_begin` 收 `IOMAP_WRITE\|IOMAP_FAULT` 时**立刻分配**（与 `write()` 同源）。堵住「mmap 洞写静默丢数据」。**M4.2 内补**（**不**推迟到 M4.3；**不**设拒写护栏）。**✅ 已落（提交 `f23b933`；guest mmap 洞写→msync→逐出读回→remount 实测 PASS）** |
| **D-M4-13** | **碎片化：暂接受为特性** | **已决（2026-09-27，用户）** | 「**能跑是第一要务**」；M4.2 不做合并/删除 → 随机写会碎片化，**暂当 feature**。**后续学习方向**：在 Linux 规则（分配器/位图/写回）下压低碎片（occupy 尾延展 / 批量连续分配 / 融合 / 重建） |
| **D-M4-14** | **inline 可写 mmap：回灌 `inline_data`（不升格、不静默丢）** | **已决（2026-09-27，O-14=(a)；M4.3 首项）** | 行为规范：mmap 写**不改 size**；`[0,size)` 内字节**持久化到 `inline_data`**；**超出 size 的字节按 Linux 常规刻意静默丢弃**（不报错、**不升格** inline→extent）；**不得**再让 `msync` 返 `EIO` |
| **D-M4-5** | **O_DIRECT（C 真兑现）后置** | 范围（**已决**，见 §7.4） | god 的 `pwrite_pipeline` 复杂度已致恶心；M4 不碰直写，放最后/独立里程碑 |
| **路线** | **走 iomap** | 框架选择 | 取代 buffer_head/`generic_perform_write` 路线（E1「缓存/IO 下沉 Linux」的现代落地） |

> 第二约束对齐：iomap 是内核**成熟框架**（xfs/ext4/btrfs/zones 现行主线），符合「能用内核成熟框架就用」。
> 第三约束（非良定义即上报）触发面见 §10。

---

## 1. 路线裁决：iomap（**不是** buffer_head 路）

两条路都「走内核框架」，但**互斥**（`generic_file_write_iter` 的 buffered 分支调 `a_ops->write_begin/write_end` = buffer_head 型；iomap 有自己的 `iomap_write_begin/end` 内部件，**不经 `a_ops->write_begin`**）。本裁决选 iomap：

| 维度 | buffer_head 路 | **iomap 路（选定）** |
|---|---|---|
| buffered 写 | `generic_file_write_iter` → `generic_perform_write` → `a_ops->write_begin/end` | 自写壳 → `iomap_file_buffered_write` |
| 映射契约 | `get_block`（每块） | **`iomap_begin/iomap_end`（每区间）** |
| a_ops 需求 | 需 `write_begin/write_end` | **不需要**（见 §2 勘误） |
| O_DIRECT | `__blockdev_direct_IO` | `iomap_dio_rw` |

### 1.1 ⚠️ 对既有文档的勘误（必须同步）

- **handover §10.2 的 `a_ops` 清单**列了 `write_begin/write_end`（buffer_head 路的件）→ **iomap 路线应删除这两项**。证据：`fs/xfs/xfs_aops.c::xfs_address_space_operations` 无 `write_begin/write_end`，buffered 写走 iomap 内部 `iomap_write_begin/end`。
- **spec §4.4 R-FD-01** 现写「框架落点 = `generic_file_write_iter`」→ **iomap 路线须改为**「自写 `write_iter` 壳 + `iomap_file_buffered_write`」。照旧挂 `generic_file_write_iter` 会**编译过、运行时对不上 iomap a_ops**。
- **spec §4.4 R-FD-10** 现写「`direct_IO` 可降级，落点 `-EOPNOTSUPP`」→ **口径错**。真实机制：点 **`a_ops->direct_IO` 存废决定 `open(O_DIRECT)` 成败**；无它 → `open` 直接 `-EINVAL`（`fs/open.c:960/968`），**不经过 write**，谈不上 `EOPNOTSUPP`。本 M4 选 **C 真兑现**（§7.4）。

### 1.2 内核真值核对（对 `linux-7.2.3` 源码逐符号核实，2026-09-27）

> 目的：本 spec §2 原按「主线老 iomap」写；**7.2.x 的 iomap 已重构**。以下为对着本机
> `PS_git/custom-kernel/linux-vfio/src/linux-7.2.3/`（`include/linux/iomap.h`、`fs/iomap/buffered-io.c`、
> 各家落地 `fs/zonefs/file.c`、`fs/xfs/xfs_aops.c`、`fs/gfs2/aops.c`）核实的**接口真值**，§2/§6.2 已据此改写。

| 符号 | **7.2.3 真值** | 本 spec 旧文（错） |
|---|---|---|
| `iomap_read_folio` | `void iomap_read_folio(const struct iomap_ops*, struct iomap_read_folio_ctx*, void *private)` | 老 `(ops, folio, priv)` |
| `iomap_readahead` | `void iomap_readahead(const struct iomap_ops*, struct iomap_read_folio_ctx*, void*)` | — |
| 简易读封装 | `iomap_bio_read_folio(folio, ops)` / `iomap_bio_readahead(rac, ops)`（走 `iomap_bio_read_ops`） | 无 |
| 自定义读 | `struct iomap_read_ops { read_folio_range, submit_read, bio_set }`（经 `ctx.ops`） | 无 |
| `iomap_writepages` | `int iomap_writepages(struct iomap_writepage_ctx *wpc)` | 老 `(mapping, wbc, ops)` |
| 回写 ops | `struct iomap_writeback_ops { writeback_range, writeback_submit }`（**不是 `map_blocks`**） | 无 |
| `iomap_file_buffered_write` | `(iocb, iter, ops, write_ops, private)` **五参** | 三参 |
| `struct iomap_write_ops` | `{ get_folio, put_folio, iomap_valid, read_folio_range }` | 「可选」未展开 |
| `migrate_folio` | **`filemap_migrate_folio`**（xfs/zonefs 同；**无 `iomap_migrate_folio`**） | 旧文写 `iomap_migrate_folio` |
| 其他必挂 a_ops | `.is_partially_uptodate = iomap_is_partially_uptodate`、`.error_remove_folio = generic_error_remove_folio` | 旧文缺 |
| `dirty_folio`/`invalidate_folio`/`release_folio` | `iomap_dirty_folio` / `iomap_invalidate_folio` / `iomap_release_folio` | ✅ |
| `IOMAP_INLINE` | 存在（`type=4`；`iomap_read_inline_data`/`iomap_write_begin_inline`/`iomap_write_end_inline`） | ✅ 结论成立 |
| `IOMAP_DELALLOC` | 存在（`type=1`） | ✅ |
| 新 flag | `IOMAP_F_SIZE_CHANGED` / `IOMAP_F_STALE` / `IOMAP_F_FOLIO_BATCH` | 旧文无 |
| `.mmap_prepare` | 存在（`int (*mmap_prepare)(struct vm_area_desc*)`；`generic_file_mmap_prepare(desc)`） | ✅ 已对 |

> **结论**：三态→iomap type（§3）、D-M4-1（`read_folio` 是 mmap 硬前置）、D-M4-5（O_DIRECT 后置）**成立**；
> **D-M4-3 / D-M4-4 已撤销**（→ D-M4-7 / D-M4-6，见 §0）。
> 但 **§2 的接线签名**与 **§6.2 的「包装层放大粒度」** 必须按上表改写（下文已改）。

---

## 2. 接口清单（ko 侧最终挂点）

### 2.1 `file_operations`（regular）

| 成员 | 落点 | 依据 | 优先 |
|---|---|---|---|
| `read_iter` | `ravenfs_file_read_iter`（自写壳；inline 可走 `IOMAP_INLINE`，无需入口特判） | D-M4-2 | **M** |
| `write_iter` | `ravenfs_file_write_iter`（自写壳：readonly 拒写 → `generic_write_checks` → `iomap_file_buffered_write`/`iomap_dio_rw`） | D-M4-2 | **M** |
| `mmap_prepare` | `generic_file_mmap_prepare`（7.2.3 已把 `.mmap` 换为 `.mmap_prepare`；自带 `generic_file_vm_ops = {filemap_fault, filemap_map_pages, filemap_page_mkwrite}`） | **D-M4-1** | **M** |
| `llseek` | `generic_file_llseek` | R-FD-02 | M |
| `fsync` | `ravenfs_fsync`（`filemap_write_and_wait_range` + 元数据落盘） | R-FD-04 | M |
| `fallocate` | `ravenfs_fallocate`（`KEEP_SIZE` 预分配） | R-FD-07 | M |
| `splice_read`/`splice_write` | `filemap_splice_read` / `iter_file_splice_write` | R-FD-05 | M |
| `fop_flags` | `FOP_MMAP_SYNC \| FOP_BUFFER_RASYNC \| FOP_BUFFER_WASYNC \| FOP_DIO_PARALLEL_WRITE`（对齐 xfs） | — | M |

### 2.2 `address_space_operations`（**iomap 家族**）

> **7.2.3 核实版**（签名见 §1.2）。最小落地参照 `fs/zonefs/file.c`。

| 成员 | 落点（真实签名） |
|---|---|
| `read_folio` | `ravenfs_read_folio(file, folio)` → `iomap_bio_read_folio(folio, &ravenfs_iomap_ops)`（**D-M4-1 硬前置**）。需自定义读（large-folio 校验 / endio 卸载到 workqueue）时改用 `iomap_read_folio(ops, &ctx, NULL)` + 自写 `iomap_read_ops` |
| `readahead` | `ravenfs_readahead(rac)` → `iomap_bio_readahead(rac, &ravenfs_iomap_ops)` |
| `writepages` | `ravenfs_writepages(mapping, wbc)` → 组 `struct iomap_writepage_ctx wpc = {.inode=mapping->host, .wbc=wbc, .ops=&ravenfs_writeback_ops}` → `iomap_writepages(&wpc)` |
| `dirty_folio` | `iomap_dirty_folio` |
| `invalidate_folio` | `iomap_invalidate_folio` |
| `release_folio` | `iomap_release_folio` |
| `migrate_folio` | **`filemap_migrate_folio`**（**不是** `iomap_migrate_folio`） |
| `is_partially_uptodate` | `iomap_is_partially_uptodate` |
| `error_remove_folio` | `generic_error_remove_folio` |
| ~~`write_begin`/`write_end`~~ | **不需要**（§1.1；iomap 内部 `iomap_write_begin/end`） |
| `direct_IO` | 未来 C：`ravenfs_direct_IO` → `iomap_dio_rw(iocb, iter, &ravenfs_iomap_ops, &ravenfs_dio_ops, …)`（§7.4） |

### 2.3 核心自写件：`iomap_ops`

```c
/* 逻辑偏移 → 物理映射 / 空洞 / 内联 —— 唯一真正触碰数据语义的自写件 */
static const struct iomap_ops ravenfs_iomap_ops = {
	.iomap_begin = ravenfs_iomap_begin,   /* (inode,pos,len,flags,iomap,srcmap) */
	.iomap_end   = ravenfs_iomap_end,
};

/* buffered 写可选钩子（M4 先空；需要防 stale 时再填 iomap_valid，见 §12 O-10） */
static const struct iomap_write_ops ravenfs_write_ops = {
	/* .get_folio / .put_folio / .iomap_valid / .read_folio_range */
};

/* ★回写：只查「已分配映射」（写时已分配）+ 挂 ioend —— **不在回写期分配**（D-M4-6） */
static ssize_t ravenfs_writeback_range(struct iomap_writepage_ctx *wpc,
		struct folio *folio, u64 pos, unsigned len, u64 end_pos);
static const struct iomap_writeback_ops ravenfs_writeback_ops = {
	.writeback_range  = ravenfs_writeback_range,
	.writeback_submit = iomap_ioend_writeback_submit,  /* 默认 ioend 提交即可 */
};

/* 仅当读需 workqueue/校验/自定义 bio_set 时才需要；否则用 iomap_bio_read_ops */
/* 未来 C：static const struct iomap_dio_ops ravenfs_dio_ops = {.end_io, .submit_io}; */
```

> **纪律**：页缓存、folio、bio、回写、readahead **全由 iomap 核心代劳**；
> fs 在 `iomap_begin` 回答「逻辑偏移 → 物理映射/空洞/内联」（**写向 = 立刻分配**），在 `writeback_range` 只负责「查已分配映射 + 挂 ioend」（**不再分配**，见 D-M4-6）。
> 最小参照：`fs/zonefs/file.c`（读 = `iomap_bio_read_*`；回写 = `zonefs_writeback_range` + `iomap_ioend_writeback_submit`）。

---

## 3. 三态 → iomap type 映射（判据 size 优先，不变量 22）

| god 态 | 判据 | `iomap_begin` 返回 | 读 | 写 |
|---|---|---|---|---|
| **inline** | `size ≤ 160` | **`IOMAP_INLINE`**，`inline_data = inode->inline_data`，`offset=0`，`length ≥ size` | `iomap_read_inline_data` 填 folio | `iomap_write_begin/end_inline` |
| **direct** | `size > 160 ∧ direct_bit=1` | `IOMAP_MAPPED`，`addr = data_root + (pos>>12)`，`len` 截到 run 尾 | bio | bio |
| **extent**（命中） | `size > 160 ∧ direct_bit=0` 且树命中 | `IOMAP_MAPPED`（walk 树取覆盖 pos 的 extent） | bio | bio |
| **extent**（洞） | 树未命中 | 读向 `IOMAP_HOLE`（`len` 截到下一映射起点）；**写向 → 立刻分配**（D-M4-6）后返 `IOMAP_MAPPED` | `folio_zero_range`（读 0） | **立刻分配 + 物化**，返 `IOMAP_MAPPED`（**非 `IOMAP_DELALLOC`**） |

> **惊喜点（收编 inline）**：iomap **自带 `IOMAP_INLINE`**（`include/linux/iomap.h:34`，`inline_data` 见 `:137`），
> 所以 ≤160 的 inline 数据**也走 iomap**，读/写/直读/直写四路统一，**不需要在 `read_iter/write_iter` 入口特判 inline**。
> （此结论修正前几轮口头推测的「inline 进不了 iomap」。）

---

## 4. 需求项（R-M4-*）

| ID | 需求 | 落点 | 优先 | 现状 |
|---|---|---|---|---|
| **R-M4-01** | **mmap（读/写/执行）** | `.mmap_prepare=generic_file_mmap_prepare` + `a_ops->read_folio` | **M（硬）** | ❌ |
| **R-M4-02** | `read_iter`（buffered 读，含 atime 免推） | `ravenfs_file_read_iter` | **M（硬）** | ❌ |
| **R-M4-03** | `write_iter`（buffered 写，含 mtime/ctime 免推） | `ravenfs_file_write_iter` | **M（硬）** | ❌ |
| **R-M4-04** | `a_ops` iomap 家族（§2.2） | 见 §2.2 | **M** | ❌ |
| **R-M4-05** | `iomap_begin/end`（extent 对接） | `ravenfs_iomap_ops` | **M** | ❌ |
| ~~**R-M4-06**~~ | ~~cache_miss 全量加载（64K 单元）~~ | **撤销**（D-M4-7：用 iomap 默认） | — | 撤销 |
| **R-M4-07** | 三态迁移（inline↔direct↔extent、出口坍缩） | `iomap_begin`(WRITE) + `iomap_end` | **M** | ⚠️ god 有（`df_pipeline`） |
| **R-M4-08** | inline 回迁（`size ≤ 160` 强制） | `iomap_begin`/truncate | M | ⚠️ god 有 |
| **R-M4-09** | **O_DIRECT 真兑现（C）** | `a_ops->direct_IO = iomap_direct_IO` + 对齐 | C | ❌ |
| **R-M4-10** | truncate / punch_hole / fallocate | 见 `punch_truncate_pread_spec.md` | M | ⚠️ god 有 |
| **R-M4-11** | `i_blocks` 诚实化（数据块 + 元数据块） | 随写更新 | M | ❌ |
| **R-M4-12** | readonly 拒写 / 写序 / `fsync` 落数据 | 入口检查 + §8 | M | ❌ |

---

## 5. mmap 专项（D-M4-1）

- **前置**：`generic_file_mmap_prepare` 检查 `a_ops->read_folio` → 无则 `-ENOEXEC`。故 **`read_folio` 是 mmap 的硬前置**（M4.1 先做）。
- **vm_ops**（内核代劳）：`filemap_fault`（缺页→`read_folio`）、`filemap_map_pages`（多页映射）、`filemap_page_mkwrite`（写 fault）。
- **写 fault**：`filemap_page_mkwrite` → `a_ops->dirty_folio`（= `iomap_dirty_folio`）标脏 + **顺带推 mtime/ctime**（spec §2.2「mmap 写免费」）。
- **可执行**：动态加载器 `mmap(PROT_EXEC)` 全走 `filemap_fault` → 数据必须能被 `read_folio` 正确解析（含 inline/洞/跨态）。
- **一致性**：`mmap` 写与 buffered 写共享页缓存 → 天然一致；与 **O_DIRECT 混用**需失效协调（§7.4）。

---

## 6. buffered 读粒度（**D-M4-3 撤销 → D-M4-7：用 iomap 默认**）

> **2026-09-27 晚：D-M4-3（自研「64K 单元 / 大 folio 全量加载」）撤销**（O-5 = (c)）。
> 原因：真正落点 `mapping_set_folio_min_order(4)` 受 `CONFIG_TRANSPARENT_HUGEPAGE` 门控
> （`include/linux/pagemap.h:469`），且 `iomap_read_folio` 只处理传入的那一个 folio —— **「在 fs 包装层放大粒度」做不到**，
> 只能改 folio order，收益/风险比不划算。

**决定（D-M4-7）**：读路径 **全用 iomap 默认** —— `read_folio`（`iomap_bio_read_folio`）+ `readahead`（`iomap_bio_readahead`），
4K folio 按需 + 内核 readahead 启发式。**不设自研单元上限、不做大 folio min order**。

- inline（≤160）整段直拷；direct/extent 命中由 iomap 提交 bio；洞 `folio_zero_range` 零填 —— **全部 iomap 核心代劳**。
- 后续若确需 SSD 批量读，**再单独立项研究**（不阻塞 M4）。

（历史：原 V3·64K 方案 + 大 folio 机制 + THP 依赖，见 git `181bae9`。）

---

## 7. 四路收敛：一个 `ravenfs_iomap_ops`

```
read(2)   → read_iter ─┐
pread(2)                ├─ buffered ── → iomap_file_buffered_write / iomap_read_folio ─┐
write(2)  → write_iter ─┘                                                              ├─→ ravenfs_iomap_begin/end
O_DIRECT read/write → a_ops->direct_IO → iomap_dio_rw ────────────────────────────────┘
mmap      → filemap_fault → a_ops->read_folio ─────────────────────────────────────────┘
```

**读、写、直读、直写、mmap 五条全部收敛到同一个 `iomap_begin`**（用 `iomap_iter.flags` 的 `IOMAP_WRITE` 有无分向）。

### 7.4 O_DIRECT = C 真兑现（R-M4-09）

- `a_ops->direct_IO = iomap_direct_IO`（包 `iomap_dio_rw`），**读写共用**（`iov_iter_rw()` 分向）。
- **对齐**：`(pos | len) & (alignment-1)` → `-EINVAL`（`iomap_dio_bio_iter`，`alignment = i_blocksize`）。
- **一致性**：读前 `kiocb_write_and_wait`；写后 invalidate（iomap 代劳）。
- **短读/短写** 均退回 buffered（`direct_write_fallback` / `generic_file_read_iter` 尾段）——**保留**，与真直写不冲突。
- 〔**已决 D-M4-5**〕**后置**：M4 全程不做（M4.1–M4.5 不含 C），放最后一个独立里程碑——用户口径：「god 写 `pwrite_pipeline` 时已被复杂度恶心过」。M4 只需保证 `a_ops->direct_IO` 的**存废决策**明确（无它 → `open(O_DIRECT)` `EINVAL`，即 §9 的 R-FD-10 勘误）。

---

## 8. 一致性 / 崩溃 / 写序（**D-M4-4 撤销后重述**）

**分配时机 = 立刻（D-M4-6）** —— 写时 `iomap_begin(WRITE)` 直接真分配并返 `IOMAP_MAPPED`；**不再有 delalloc 预留**。于是：

| 项 | 要求 | 状态 |
|---|---|---|
| **分配时机** | **写时立刻分配**（D-M4-6）；`write(2)` 即知 ENOSPC | **已决** |
| 写序（不变量 25） | 位图亮灯 → 数据/节点落盘 → inode 翻转 —— **恢复经典形态**：①亮灯在 `write(2)`（内存位图）；②数据落盘在回写（dirty folio → `writeback_range` bio）；③inode 字段翻转在 `flush_all`/`sync` | 必落 |
| 崩溃模型 | 写后未 flush 崩 = 内存分配 + 脏页全丢（**等价「没写」，无损坏**）；flush 中途崩 = 同 G11/M3.5（dangling = 设计特性，不修） | 与 M3.5 一致 |
| `extent_root_shrink` 旧根 | 延迟 free（随 `root_blk&` 上抛，inode 提交**后** free） | **必修** |
| 收缩/坍缩 | 先 inode 后 free 旧根（孤儿 2 块可 fsck；**严禁反向**） | 必落 |
| readonly | 写入口 `RAVENFS_SB(inode->i_sb)` 检查 | 必落 |
| `i_blocks` | 数据块 + extent 节点×2 + … 诚实化 | 必落 |
| 部分块写清零 | 新分配块被部分写 → 依 iomap `IOMAP_F_NEW` **零填**未写部分（不得读出旧块内容） | `iomap_begin` 写向新分配时置 `IOMAP_F_NEW` |
| 短写尾巴 | `iomap_end` **不** free 未写完区间（D-M4-10(b)）；**VFS `evict_inode` 时清算** trim 超越 `size` 的意外分配。⇒ 常驻文件挂到 unlink/umount（短命文件/umount 全清）；又因 **KEEP_SIZE 已拒（D-M4-11）**、`> size` 分配只剩短写尾，**无 fallocate 误伤** | 必落（O-11/O-12） |
| **mmap 写（洞）** | **已修（D-M4-12，提交 `f23b933`）**：`page_mkwrite`→`iomap_page_mkwrite`→立刻分配；实测 mmap 洞写落盘 PASS。残留：**inline 态可写 mmap 未闭环**（见 §12 O-14） | ✅ 已修 |

> **撤销记录（D-M4-4 原定案后果，作废）**：原「写时 `IOMAP_DELALLOC` / 回写期真分配 / 提交点搬到回写 / `writepages` 承担真分配」**全部作废**——O-4 选择强同步立刻分配（位图纯内存 → 无推迟动机），`write(2)` 即可返 ENOSPC。**O-2/O-3（回写期形态、提交点搬家窗口）随之消解**。
> **新增待定（立即分配的岔口）**：`iomap_end` 里「已分配但未写完」区间（短写/失败）如何处理 → 见 §12 **O-11**。

---

## 9. 与既有文档的接口 & 勘误清单

| 文档 | 动作 |
|---|---|
| `ravenfs-linux-ko-requirements-spec.md` §4.4 R-FD-01 | **勘误（已改 2026-09-27）**：框架落点 `generic_file_write_iter` → 「自写壳 + `iomap_file_buffered_write`」 |
| 同上 R-FD-10 | **勘误（已改 2026-09-27）**：`-EOPNOTSUPP` → 「无 `direct_IO` → `open` `EINVAL`」；**C 真兑现后置**（D-M4-5，见 §7.4） |
| 同上 §4.4 R-FD-03 | 保持；`mmap_prepare` 口径正确（7.2.3） |
| `ravenfs-linux-ko-handover.md` §10.2 | **勘误**：a_ops 去 `write_begin/write_end`；补「路线 = iomap」 |
| 同上 §10.3/§10.6 | 保持；M4.1 只读优先不变 |
| `Docs/pwrite_pipeline.md` | 保持为**盘布局机械真值**；「五段」是设计真相，但**宿主改为 `iomap_begin/end`**（见溶解说明） |
| `Docs/punch_truncate_pread_spec.md` | 保持；truncate/punch/fallocate 契约不变 |
| `ravenfs-linux-ko-acceptance-plan.md` §2.0 M4 | 保持；本文件为其细化 |
| `ravenfs-linux-ko-handover.md` §10.2 | **勘误 v2**：`read_folio`/`writepages` 签名按 7.2.3 改（§1.2）；`migrate_folio=filemap_migrate_folio`；补 `is_partially_uptodate`/`error_remove_folio`/`iomap_writeback_ops` |
| 本文件 §2.1/§2.2/§2.3/§6.2 | **已改写**为 7.2.3 核实版（§1.2） |

---

## 10. 裁决记录（第三约束上报 → 已决）

| 项 | 裁决（2026-09-27 用户） | 影响步骤 |
|---|---|---|
| ~~D-M4-3 口径~~ | ~~V3 + 单元上限 64K~~ → **撤销**，改 **D-M4-7 iomap 默认**（O-5） | §6；M4.1 |
| ~~分配时机（delalloc）~~ | ~~写回才分配~~ → **撤销**，改 **D-M4-6 立刻分配（强同步）**（O-4） | §8；M4.2 |
| **O_DIRECT（C）** | **后置**（M4 不做） | §7.4；M4.5 不含 |
| **O-1（直指态）** | **(b)**：inline 默认升格 extent；direct 仅出口坍缩收敛态 → **D-M4-8** | §3/§8 |
| **O-6** | 保留走 `mdata_piece_cache`；**否掉 node_latch**（M2.4 已裁） | handover §10.2 |
| **O-7** | god 实参固化进盘面不变量（阶 337 / 左 168 / 欠载 <168 / 合并 ≤337 / 深度 64） | 盘面文档（待落） |
| **O-8** | `s_maxbytes = 1<<52`（R-SP-06 已有） | — |
| **O-9** | hash 统一 XXH64（非 M4，顺带清） | dentry 文档（待落） |
| **O-10** | 不做 `iomap_valid` → **D-M4-9** | §2.3 |
| **O-11** | 短写尾巴 (b) + `evict_inode` 清算 → **D-M4-10** | §8 |
| **O-12** | 拒绝 `FALLOC_FL_KEEP_SIZE`（纯预留）→ **D-M4-11**（O-12 消解） | §4/§8；R-FD-07 |

> **连带未清项**：**O-4 裁决（立刻分配）后，原 delalloc 连带的 O-2/O-3 消解**；O-4 自身已闭环；新暴露 **O-11**（`iomap_end` 未写完区间的块处置，见 §12）。

---

## 11. M4 切分（对齐 handover §10.3，补 iomap 口径）

| 步 | 内容 | 判据 |
|---|---|---|
| **M4.1 只读** | `a_ops.read_folio/readahead`（**iomap 默认**，D-M4-7）+ `read_iter` + `mmap` | guest `dd/cat/head` 往返一致（三态 / >4K / 稀疏洞）；mmap 读/执行通 —— ✅ **2026-09-27 通过**（提交 `791ad02`；验收 `Docs/linux/ravenfs-linux-ko-M4.1-acceptance.md`） |
| **M4.2 写数据** | `write_iter` + `iomap_begin(WRITE)` **立刻分配（D-M4-6）** + extent 树移植 + 三态迁移（inline→extent 默认，D-M4-8）+ `i_blocks` | `dd` 写→读往返；`stat.blocks` 吻合；audit 全绿 —— ✅ **2026-09-27 通过**（提交 `e61c398`；验收 `Docs/linux/ravenfs-linux-ko-M4.2-acceptance.md`；已知缺口见 §12 O-13） |
| **M4.3 truncate/punch/fallocate** | 按 `punch_truncate_pread_spec.md` | spec §8 矩阵 |
| **M4.4 setattr + 时间戳** | `inode_operations.setattr`；时间戳走 generic | `chmod/chown/utimes/truncate` |
| **M4.5 收尾** | `fsync` 落数据 / 写序（**经典形态**，§8）/ `root_shrink` 延迟 free | `sync` 往返 + 崩溃窗口（§8 / handover §6.1 设计特性） |
| ~~O_DIRECT（C）~~ | **后置独立里程碑**（D-M4-5，M4 不做） | — |

---

## 12. 裁决闭环（第三约束上报 → 用户已裁，2026-09-27 晚）

> 用户于 `git diff`（基线 `181bae9`）直接裁决；下表末列 = 最终裁决，已回填 §0/§6/§8/§10。

| # | 问题 | 现状冲突 | 选项 | **裁决（2026-09-27 用户）** |
|---|---|---|---|---|
| **O-1** | **delalloc 下 direct 态的「物理全覆盖义务」** | 不变量 24 要求 direct 增长时**占位分配**（写时亮灯）；D-M4-4 写时不亮灯 → **写时无法确定能否维持 direct** | (a) 写时预留连续 run（soft-reserve 不亮灯），回写兑付，兑付失败降级 extent（需搬数据）；(b) **delalloc 下非 inline 一律走 extent**，direct 只作**回写期出口坍缩的收敛态**（不再是初始分配态）；(c) 退回同步分配（违背 D-M4-4） | **(b)** 确实可以inline默认升格->extent,最后尾巴处发现确实可以shrink到一个extent再direct → **D-M4-8** |
| **O-2** | **不变量 25 的回写期形态** | 原三步（亮灯→落盘→inode 翻转）在 delalloc 下**全压缩到回写**；`write(2)` 返回时零落盘 | (a) 单阶段：回写内「分配→bio→inode 翻转」顺序推进，中途崩 = dangling（**同 M3.5，判为设计特性**）；(b) 分阶段迷你 journal（重） | **消解**（O-4 立刻分配后不再有「全压缩到回写」）→ 见 §8 |
| **O-3** | **`data_root` 提交点搬家后的崩溃窗口** | 提交点从 write 期搬到回写期 | write 返回后、回写前崩 = 数据全丢但 inode 未动（安全，等价「没写」）；回写中途 = O-2 | **消解**（提交点不再搬到回写期） |
| **O-4** | **回写期 ENOSPC 暴露面** | 真分配在回写 → `write(2)` 已成功返回后才可能 ENOSPC（Arch 安装期致命） | (a) **写时做块数预留**（reserve 计数不亮灯）→ `write(2)` 即返 ENOSPC（ext4 式）；需 sb 级 reserve 新件；(b) 不预留 → ENOSPC 只能 `sync`/`fsync` 暴露 | 另一条路，强同步，数据块一个需求立马上分配，我的位图分配器纯内存多分片不怕，但是要记录f_off->blkidx,就是设计来解决这个的|
| **O-5** | **64K 单元的硬依赖** | `mapping_set_folio_min_order(4)` 受 `CONFIG_TRANSPARENT_HUGEPAGE` 门控 | (a) 依赖 THP（宿主 =y，**guest 未核实**）；(b) guest THP=n 则退 4K + 靠 readahead 窗口；(c) 放弃 64K 用 iomap 默认 | 统一回退到 （C）使用iomap的先进框架，后续有时间有token再研究 |
| **O-6** | **extent 树节点的内核侧承载** | M4 spec §10.2 说走现有 `mdata_piece_cache`（8K piece）；研究报告 C 曾提「node latch + bh」 | 保留走 `mdata_piece_cache`（**M2.4 已裁不做 node_latch**，片级 rwsem 已足）；C 的 node latch 建议系误读 | **保留 `mdata_piece_cache`**；在 spec 明确否掉 node_latch |
| **O-7** | **extent 树分裂/欠载阈值未固化** | 盘面文档**未给**：区段树分裂左右半条数、min occupancy、合并阈值（dentry 有 127→64/63、MINA=63） | 把 god 实参**固化进盘面文档**：阶 337；分裂左 168/右 rest；欠载 `count<168`；合并 `total≤337`；深度上限 64 | **✅ 已落（2026-09-27）**：`disk_layout_v1.1.md` §10 不变量 **31–36** + §5.3 + §9；§10“其余条目不变”合并链接 |
| **O-8** | **最大文件尺寸未给** | 三份盘面文档均无显式值；spec R-SP-06 定 `s_maxbytes = 1<<52` | 由 extent 树容量反推（337^depth × 4K，深度上限 64 → 极大）；或直接采用 `1<<52` + `static_assert` | **采用 `1<<52`**（R-SP-06 已有） |
| **O-9** | **hash 算法自相矛盾** | v1.1 §9 写「XXH64（已定）」；头文件/§6.1 写「待定」 | 统一为 XXH64(seed=0)，或明确 SipHash-2-4 升级路径 | **✅ 已落（2026-09-27）**：统一 **XXH64(seed=0)**（v1.1 §6.1 + header 注释改）；SipHash 升级路径保留在 §9 |
| **O-10** | **`iomap_write_ops.iomap_valid` 是否实现** | 长写期间 extent 映射可能变（并发/恢复）→ 需 validity_cookie 检测 stale | (a) M4 不做（单挂载 + VFS `inode_lock` 串行化写）；(b) 做（存 data_root 版本号） | **不做**（O-4 立刻分配 → 映射稳定）→ D-M4-9 |
| **O-11** | **（新）`iomap_end` 里「已分配但未写完」区间（短写/失败）如何处置** | 立刻分配引出：写时已真分配，但 `iomap_end` 的 `written` 可能 < length | (a) 对 `[pos+written, pos+length)` 已分配块**即刻 free**（干净，需拆 extent）；(b) 保留（浪费但简单）；(c) free 整段并回滚 size | **(b) + evict 清算**：短期保留浪费；**evict 时秋后算账** trim 尾块 → **D-M4-10** |
| **O-12** | **（新）`evict_inode` 清算如何区分「意外短写尾」vs「fallocate 有意预留」** | 两者都是「已分配但超出 `size`」；若一律 trim，会误删 `FALLOC_FL_KEEP_SIZE` 预分配（R-FD-07） | (a) **盘面标记**（预留记 `unwritten`，加第四态——改动大）；(b) **内存标记**（write 路径短写时给 inode 记「待清算尾」，`evict_inode` 见标记才 trim；不动盘面；删除路径本就整 inode 释放）；(c) `fallocate` 放弃 KEEP_SIZE（违背 R-FD-07） | **消解**：`FALLOC_FL_KEEP_SIZE` 拒绝受理（保留 `PUNCH_HOLE\|KEEP_SIZE` 例外）→ 盘上无「已分配 > `size`」的 fallocate 预留 → evict trim 无歧义 → **D-M4-11** |
| **O-13** | **（新，M4.2 实测暴露）可写 mmap 落在「未分配洞」上 → 静默丢数据** | `mmap` 写 → `page_mkwrite` 脏页 → 回写期 `iomap_begin` 见 HOLE → iomap 跳过（不写）→ 数据丢（`write()` 走立刻分配故无此问题） | (a) 现在做 `page_mkwrite` → 立刻分配（正解）；(b) 过渡护栏：`mmap_prepare` 拒 `VM_MAYWRITE`；(c) 暂接受 | **已落**：`page_mkwrite` 立刻分配 → **D-M4-12**（提交 `f23b933`，实测 PASS） |
| **O-14** | **（新，page_mkwrite 实测暴露）inline 态（size≤160）的可写 mmap 未闭环** | mmap 写 inline 文件 → iomap 不物化 `IOMAP_INLINE` → 写只进页缓存；回写期 `writeback_range` 见非 MAPPED/HOLE 返 `-EIO` → `msync` 报 EIO（**硬失败，未静默丢**；但用户态 write 到页缓存看似成功） | (a) `page_mkwrite`/`iomap_end(FAULT)` 把页回灌 `inline_data`；(b) `mmap_prepare` 对 inline 文件拒可写映射；(c) 接受当前硬失败 | **(a) 现在做（M4.3 首项）→ D-M4-14**：回灌 inline_data；**不升格**；超 size 静默丢弃（Linux 常规） |

> **一句话**：O-1/O-4/O-5 已裁并回填（**D-M4-6/7/8**）；O-2/O-3 随 O-4 消解；O-6/O-7/O-8/O-9 按建议采纳；O-11 = **D-M4-10**（b + `evict_inode` 清算）；O-12 = **D-M4-11**（拒 `FALLOC_FL_KEEP_SIZE`）；O-13 = **D-M4-12**（已落 `f23b933`）；**碎片化暂接受为特性（D-M4-13）**；**O-14 = D-M4-14**（M4.3 首项，inline 可写 mmap 回灌）→ 闭环。

---

> **一句话**：M4 文件数据 = **iomap 一条路**；`ravenfs_iomap_begin/end` 是唯一自写核心；
> mmap（D-M4-1）与 read_iter/write_iter（D-M4-2）是硬约束；**写时立刻分配（D-M4-6）**、读用 **iomap 默认（D-M4-7）**、
> 三态迁移 = inline 默认升格 extent（D-M4-8）；O_DIRECT（C）后置（D-M4-5）；先读（M4.1）后写。

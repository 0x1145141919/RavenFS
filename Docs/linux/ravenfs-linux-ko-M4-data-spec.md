# RavenFS → Linux KO：M4 文件数据需求规格（定调版）

> 2026-09-27 · Raven · 用户定调（三条）+ 路线裁决 = **iomap**
> 上位文档：`ravenfs-linux-ko-requirements-spec.md`（第一约束实体化，**冲突以 spec 为准**）；
> 本文是 **M4（文件数据 / G1·G2）的第二约束实体化**：把「走哪个内核框架 / 怎么接线」钉死。
> 机械参考（god，谱系非代码）：`Docs/pwrite_pipeline.md`（五段管线）、`Docs/punch_truncate_pread_spec.md`、
> `juvenile_god/direct_file_ops.cpp`（`df_pipeline`）。
> **一句话**：M4 的文件必须 **mmap-able**、必须走 **read_iter/write_iter**（iomap 路线）、
> buffered 的 **cache_miss 全量加载**进页缓存。

---

## 0. 定调（用户裁决，2026-09-27）

| ID | 定调 | 性质 | 说明 |
|---|---|---|---|
| **D-M4-1** | **文件必须能被 mmap** | **硬约束（不可降级）** | 含可读 / 可写 / 可执行（动态加载器 mmap 全部 `.so`）。→ `a_ops->read_folio` 是前置（无它 `generic_file_mmap_prepare` 直接 `-ENOEXEC`，`mm/filemap.c`） |
| **D-M4-2** | **必须走 `read_iter`/`write_iter` 路线** | **硬约束** | 不再手搓 `direct_file_pwrite` 式同步总装；缓冲读写全部经 VFS 迭代器 |
| **D-M4-3** | **buffered 路径 cache_miss → 全量加载进页缓存** | 策略（**已定：V3 · 单元上限 64K**，见 §6） | 缺 folio 时**整单元**读入，单元 **≤64K**（SSD 导向），**无子块/子 folio partial**。非 iomap 默认的逐 4K 页按需 + readahead 启发式 |
| **D-M4-4** | **分配时机 = 写回才分配（delalloc）** | 策略（**已决**，见 §8） | `iomap_begin@写` 只预留（`IOMAP_DELALLOC`），**真分配在写回**（`writepages`→`iomap_begin@wb`→`IOMAP_MAPPED`） |
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

| 成员 | 落点 |
|---|---|
| `read_folio` | `iomap_read_folio(&ravenfs_iomap_ops, ...)` 包装（**D-M4-1 的前置**） |
| `readahead` | `iomap_readahead(&ravenfs_iomap_ops, ...)` |
| `writepages` | `ravenfs_writepages` → `iomap_writepages(&ravenfs_iomap_ops)` |
| `dirty_folio` | `iomap_dirty_folio` |
| `invalidate_folio` | `iomap_invalidate_folio` |
| `release_folio` | `iomap_release_folio` |
| `migrate_folio` | `iomap_migrate_folio` |
| ~~`write_begin`/`write_end`~~ | **不需要**（§1.1 勘误） |
| `direct_IO` | `iomap_direct_IO`（未来 C，见 §7.4） |

### 2.3 核心自写件：`iomap_ops`

```c
static const struct iomap_ops ravenfs_iomap_ops = {
    .iomap_begin = ravenfs_iomap_begin,   // ← 唯一真正触碰数据语义的自写件
    .iomap_end   = ravenfs_iomap_end,
};
static const struct iomap_write_ops ravenfs_iomap_write_ops = { /* 可选：iomap_valid 等 */ };
static const struct iomap_dio_ops   ravenfs_dio_ops        = { /* 未来 C：end_io/submit_io */ };
```

> **纪律**：页缓存、folio、bio、回写、readahead **全由 iomap 核心代劳**；
> fs 在 `iomap_begin` 只回答「逻辑偏移 → 物理映射/空洞/内联」这一件事。

---

## 3. 三态 → iomap type 映射（判据 size 优先，不变量 22）

| god 态 | 判据 | `iomap_begin` 返回 | 读 | 写 |
|---|---|---|---|---|
| **inline** | `size ≤ 160` | **`IOMAP_INLINE`**，`inline_data = inode->inline_data`，`offset=0`，`length ≥ size` | `iomap_read_inline_data` 填 folio | `iomap_write_begin/end_inline` |
| **direct** | `size > 160 ∧ direct_bit=1` | `IOMAP_MAPPED`，`addr = data_root + (pos>>12)`，`len` 截到 run 尾 | bio | bio |
| **extent**（命中） | `size > 160 ∧ direct_bit=0` 且树命中 | `IOMAP_MAPPED`（walk 树取覆盖 pos 的 extent） | bio | bio |
| **extent**（洞） | 树未命中 | `IOMAP_HOLE`（`len` 截到下一映射起点） | `folio_zero_range`（读 0） | 触发分配/物化（`IOMAP_WRITE` 向） |

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
| **R-M4-06** | **cache_miss 全量加载**（D-M4-3） | `read_folio`/`readahead` 包装层 | **M** | ❌ |
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

## 6. buffered 路径 & D-M4-3「cache_miss 全量加载」

### 6.1 定义（**已定：V3 · 单元上限 64K**）

> **cache 未命中（缺 folio）→ 一次性把一个对齐单元读入页缓存**；
> 单元 = **最大 64K**（= 16×4K 页），**无子块/子 folio partial**，且**不跨映射边界**（洞 / run 尾）。

- **粒度上限 64K**：单元 = `min(64K 对齐窗口, 当前映射剩余长度)`。绝不越过 extent/run 边界去读未分配区（不制造越界 IO，也不把洞读成数据）。
- **不做部分**：命中即整单元 uptodate，绕开 iomap per-block `ifs` 子块 partial 的语义负担。
- **实现二选一**：
  - ① **大 folio**：`mapping_set_folio_min_order(mapping, 4)`（order 4 = 16 页 = 64K）——iomap 原生支持（`mapping_min_folio_order`，`include/linux/pagemap.h:438`）。
  - ② **批量**：4K folio × 16，一次 `filemap_get_pages` / readahead 窗口。
- **为何 64K**：本 fs **只为 SSD 设计**（无 HDD 寻道顾虑）；64K 是 SSD 友好的批量读上限——再大则读放大不划算，再小则退化成 iomap 默认。
- **三态一致**：inline（≤160）整段直拷；direct 单 run 一次读到 run 尾（≤64K）；extent 命中段同理；洞段直接零填不 IO。

> 与 V1（整文件）/ V2（单 run）的区别：**不做全文件预载**，避免 `cat` 大文件读放大；**以 64K 为硬上限**。

### 6.2 实现落点

- **读**：`ravenfs_read_folio` / `ravenfs_readahead` 包装层——缺页时按 **64K 单元** 预取，其余经 `iomap_read_folio`。
  - 落地：`iomap_read_folio` 前先 `filemap_get_folio`/`filemap_add_folio` 把 64K 单元（≤16 页 / 大 folio）纳入，再触发回填（不破坏 iomap 契约）。
- **写**：首次写未缓存区同样按 **64K 单元** 预载，避免 read-modify-write 落差（delalloc 下更顺，见 §8）。

> ⚠️ 该策略**不改 iomap 契约**（iomap 仍按 folio 请求），只是**在 fs 包装层把 miss 的加载粒度放大**。属「框架优先 + 参数化」的合规做法。

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

## 8. 一致性 / 崩溃 / 写序（承接 handover §10.4）

| 项 | 要求 | 状态 |
|---|---|---|
| 写序 | 位图亮灯 → 节点/数据落盘 → inode 翻转（不变量 25） | 必落 |
| `extent_root_shrink` 旧根 | 延迟 free（随 `root_blk&` 上抛，inode 提交**后** free） | **必修** |
| 收缩/坍缩 | 先 inode 后 free 旧根（孤儿 2 块可 fsck；**严禁反向**） | 必落 |
| readonly | 写入口 `RAVENFS_SB(inode->i_sb)` 检查 | 必落 |
| `i_blocks` | 数据块 + extent 节点×2 + … 诚实化 | 必落 |
| **分配时机** | **写回才分配（delalloc）**〔已决 D-M4-4〕 | **已定** |

> **D-M4-4 定案后果（必须在 M4.2 处理）**：
> - 写时 `iomap_begin(WRITE)` 返回 **`IOMAP_DELALLOC`**（只预留，位图**不亮灯**）；**真分配在写回**：`writepages` → `iomap_begin(WRITE+回写)` → `IOMAP_MAPPED`（位图亮灯）→ bio 落数据。
> - god 的「**inode 一次写 = 唯一提交点**」**失效**：`data_root`/size 落到写回后才定。崩溃模型须**重述**为「脏页 + delalloc 预留 → 回写（分配+落数据）→ 元数据落盘」。
> - **写序不变量 25 的新形态**：亮灯/落盘/inode 翻转**全压缩到回写阶段**；`write(2)` 返回时**什么都没落盘**（仅页缓存脏 + 预留）。→ 与 handover §10.4「写序」条**冲突**，需同步改写。
> - **必须的新件**：`writepages` 里承担真分配（`iomap_begin` 的写回分支）+ 「预留→实分配」状态收敛（fs 侧要能对同 pos 二次 `iomap_begin` 给予真实映射）。

---

## 9. 与既有文档的接口 & 勘误清单

| 文档 | 动作 |
|---|---|
| `ravenfs-linux-ko-requirements-spec.md` §4.4 R-FD-01 | **勘误**：框架落点 `generic_file_write_iter` → 「自写壳 + `iomap_file_buffered_write`」 |
| 同上 R-FD-10 | **勘误**：`-EOPNOTSUPP` → 「无 `direct_IO` → `open` `EINVAL`」；**C 真兑现后置**（D-M4-5，见 §7.4） |
| 同上 §4.4 R-FD-03 | 保持；`mmap_prepare` 口径正确（7.2.3） |
| `ravenfs-linux-ko-handover.md` §10.2 | **勘误**：a_ops 去 `write_begin/write_end`；补「路线 = iomap」 |
| 同上 §10.3/§10.6 | 保持；M4.1 只读优先不变 |
| `Docs/pwrite_pipeline.md` | 保持为**盘布局机械真值**；「五段」是设计真相，但**宿主改为 `iomap_begin/end`**（见溶解说明） |
| `Docs/punch_truncate_pread_spec.md` | 保持；truncate/punch/fallocate 契约不变 |
| `ravenfs-linux-ko-acceptance-plan.md` §2.0 M4 | 保持；本文件为其细化 |

---

## 10. 裁决记录（第三约束上报 → 已决）

| 项 | 裁决（2026-09-27 用户） | 影响步骤 |
|---|---|---|
| **D-M4-3 口径** | **V3 + 单元上限 64K**（SSD 导向，无 partial） | §6；M4.1 read_folio/readahead |
| **分配时机** | **写回才分配（delalloc）** | §8；**M4.2 必须新增 `writepages` 真分配 + 重述崩溃模型** |
| **O_DIRECT（C）** | **后置**（M4 不做） | §7.4；M4.5 不含 |

> **连带未清项**（delalloc 引出，M4.2 前必须定）：写序不变量 25 的**回写期形态**、`data_root` 提交点搬家后的崩溃窗口、`writepages` 分配失败（ENOSPC）在**回写路径**的暴露面（`write(2)` 已返回成功）。
> 此连带项若在 M4.2 开工时仍未定义 → 触发第三约束，**上报**。

---

## 11. M4 切分（对齐 handover §10.3，补 iomap 口径）

| 步 | 内容 | 判据 |
|---|---|---|
| **M4.1 只读** | `a_ops.read_folio/readahead`（**64K 单元**，D-M4-3）+ `read_iter` + `mmap` | guest `dd/cat/head` 往返一致（三态 / >4K / 稀疏洞）；mmap 读/执行通 |
| **M4.2 写数据** | `write_iter` + `iomap_begin(WRITE)` + **`writepages` 延迟分配（D-M4-4）** + extent 树移植 + 三态迁移 + `i_blocks` | `dd` 写→读往返；`stat.blocks` 吻合；audit 全绿 |
| **M4.3 truncate/punch/fallocate** | 按 `punch_truncate_pread_spec.md` | spec §8 矩阵 |
| **M4.4 setattr + 时间戳** | `inode_operations.setattr`；时间戳走 generic | `chmod/chown/utimes/truncate` |
| **M4.5 收尾** | `fsync` 落数据 / 写序（**delalloc 回写形态**）/ `root_shrink` 延迟 free | `sync` 往返 + 崩溃窗口（§6.1 设计特性） |
| ~~O_DIRECT（C）~~ | **后置独立里程碑**（D-M4-5，M4 不做） | — |

---

> **一句话**：M4 文件数据 = **iomap 一条路**；`ravenfs_iomap_begin/end` 是唯一自写核心；
> mmap（D-M4-1）与 read_iter/write_iter（D-M4-2）是硬约束；cache_miss 以 **64K 单元**全量加载（D-M4-3）；
> **delalloc 写回才分配**（D-M4-4）→ 崩溃模型须重述；O_DIRECT（C）后置（D-M4-5）；先读（M4.1）后写。

# RavenFS ko：缓存行状态机 + 周期回写（设计）

> 2026-09-27 · Raven · 用户裁决多轮收敛（见 §7 决策记录）
> 上位：`ravenfs-linux-ko-requirements-spec.md`（第一约束）；本文 = **R-IO-03**（新增）的实体化。
> 对象：**两类缓存行**共用同一套状态机与回刷机制——
> ① `inode_cache.c`（盘 inode 行，4K..64K）/ ② `mdata_cache.c`（8K 节点）。
> ⚠️ **位图（`bitmap.c`）不是缓存行**：全量常驻内存分片，保留分片设计 + 片级同步，**不纳入**本状态机与回刷 driver。
> 谱系真值：god `include/ravenfs/inode_cache_radix.hpp`（`ctrl_word` 状态机）+ `juvenile_god/{inode_cache,mdata_piece_cache}.cpp`。
> **一句话**：**copy-out 排他挡全 session、锁外 I/O（删 gen）；淘汰独占窗口原地 0 拷贝 I/O + 释放控制器**；
> 双时间戳（`acc_stamp`/`wb_stamp`）；TTL 4s 轮次同时做「淘汰 + 超龄回写」。

---

## 0. 背景与缺陷（当前 ko 实现）

ko 两个缓存的现状（同构）：行 = `{node: ref(refcount_t), dirty(bool), stamp(u64)}`，**无显式状态、无 per-row 保护**。

| # | 缺陷 | 证据（现实现） | 严重性 |
|---|---|---|---|
| **①** | **`dirty` 无锁写 ⟷ 回写清 → 丢更新** | `ravenfs_inode_mark_dirty`：`session_get`（读锁**已释放**）后裸写 `s->row->dirty = true`；`ravenfs_cache_writeback_all` 在 `down_write(sh->lock)` 内读/清 `dirty` → **锁护不住** | **活 bug（正确性）** |
| **②** | **dirty 行永不淘汰、也永不周期回写** | `reap_expired` 条件含 `!dirty`；`writeback_all` 仅 `sync_fs`/`umount` 调 → dirty 集**无界增长**（内存 + 崩溃窗口） | 高 |
| **③** | **回写持分片写锁做 I/O** | `writeback_all` 在 `down_write` 内调 `ravenfs_row_write`（`sb_bread`+`sync_dirty_buffer` 会睡）→ 整分片被 I/O 阻塞；且 `sb_bread`+`memcpy→bh` = **多余读 + 双重缓冲** | 高（性能 + 违反自定纪律） |

> ko 移植时把 god 的 `ctrl_word` 状态机**精简掉了** → 重新引入了 god 当年专门修掉的竞态。本设计 = **补回 god 状态纪律 + 扩出 dirty 回写维度**。

---

## 1. 真值来源：god 的 `ctrl_word` 状态机

```
state ∈ { idle → io_ing → working → evicting }        // CAS 状态机
flags:    CTRL_FLAG_DIRTY
accessor: 32 位会话占坑计数
—— 三者打包进单个 atomic<uint64_t> ctrl_word
```

- **打包的原因（god 注释）**：`reap` 要 `accessor==0` 才能接管淘汰、`get` 要 `accessor+1` 占坑——**必须落在同一原子操作**，否则 TOCTOU（get 复核 `working` 后 reap 已 CAS `working→evicting` 并 free → 悬垂）。
- god 淘汰语义：**超时且无访问者 → 脏写回 → 释放 → 回 idle**（god 的脏行**是会写回的**）。
- god 有 `accss_stamp`（atomic relaxed，TSC 戳）→ ko 应保留同义。

> **ko 落地取舍**：ko 用分片 `rw_semaphore` 已能把「`get` 取引用 vs 淘汰擦除」互斥（见 §4.1），**无需 god 那套原子打包**；但**语义状态（LOADING/READY/EVICTING + DIRTY/WB）必须显式化**，这是本设计的核心。

---

## 2. 状态机

### 2.1 状态与标志（语义层）

```
enum { ROW_LOADING, ROW_READY, ROW_EVICTING }   // 生命周期状态
flags: ROW_DIRTY（内容脏，待回写） · ROW_WB（copy-out 快照在飞）
```

| 状态 | 含义 | 在 xarray? |
|---|---|---|
| `ROW_LOADING` | 磁盘读入中（或占位） | 是（占位，避免同址重复装载） |
| `ROW_READY` | 内容有效，可被 session 使用 | 是 |
| `ROW_EVICTING` | 淘汰接管中（回写/释放进行时）；`get` 遇之**重试** | 是（**保留**，防同址立刻重载出第二份） |

### 2.2 行字段（推荐布局）

```c
struct ravenfs_row {
    struct page *page;      // buddy 页块（order=缓存->order），页对齐、直映射 → 可 DMA
    void        *data;      // page_address(page)
    refcount_t   ref;       // 1=缓存基准；>1=有 session 持有 / 回写 pin
    u64          line;      // 行号（诊断）

    /* ── 状态机 + 内容 ── */
    struct rw_semaphore io_lock;   // 读=session 读(共享)；写=session 变异 / copy-out / 淘汰转换
    unsigned long state;           // ROW_*（见 §4.1 归属哪把锁）
    unsigned int  flags;           // ROW_DIRTY | ROW_WB

    /* ── 双时间戳 ── */
    atomic64_t   acc_stamp;        // 最近访问（get）→ TTL 淘汰（atomic relaxed，尽力而为）
    u64          wb_stamp;         // 最近一次落盘完成 / 数据最后一次被改 → 回写 aging（io_lock 护）
};
```

### 2.3 转换

```
        get(miss)
 EMPTY ──────────► LOADING ──load ok──► READY(clean)
                     │                    │  ▲
                     │                    │  └── sync/周期回写：copy-out 排他 → 锁外 I/O → 清 DIRTY
                     │               mutate│
                     │                    ▼
                     │                READY(dirty)
                     │                    │
                     │           淘汰(独占)▼
                     └──────────────── EVICTING ──原地 0 拷贝 I/O──► [free page + free 控制器] ─► EMPTY
                          （get 撞 EVICTING → 重试；重试时 xa_load 已 NULL → 慢路径重载）
```

---

## 3. 两味写回（核心）

| | **驻留回写**（sync / 周期） | **淘汰回写**（行即将销毁） |
|---|---|---|
| 触发 | `sync_fs` / `fsync` / TTL 周期超龄 | TTL 淘汰 / 卸载（`put_super`） |
| 行去向 | **保留**（继续被 session 用） | **销毁** |
| 与 session | **全并发**（copy 临界区除外，见下） | 无 session（已独占） |
| I/O 方式 | **copy-out 快照 → 锁外 I/O** | **0 拷贝原地 I/O**（DMA `row->page`） |
| dirty 提交 | copy 时清 DIRTY（见下） | 写完即 `free(page)` + `free(控制器)` |
| 崩溃安全 | 快照 = 一致时间点 | 独占窗口内写 |

### 3.1 驻留回写：copy-out **排他挡全 session** → 锁外 I/O（**无 gen**）

```
┌── io_lock 写锁（排他：挡住所有 session 读 + 写）──┐
│  if !DIRTY: 解锁 return                          │
│  copy-out 快照（到暂存缓冲）                      │
│  DIRTY = 0;  WB = 1                              │   ← 快照一致 ⇒ 无需 gen/seqlock
└──────────────────────────────────────────────────┘
   pin（ref++，防淘汰；见 §4.3）
   锁外 I/O（session 恢复并发）
┌── io_lock 写锁 ──┐
│  WB = 0           │
│  if I/O 失败: DIRTY = 1（恢复）                 │
└───────────────────┘
   unpin（ref--）
```

- **为何无需 gen**：copy 在排他区内完成 ⇒ 快照必为一致时间点；`DIRTY` 在 copy 时清零 ⇒ copy 之后任何 session 变异**自然重新置 DIRTY**（下轮再写）；失败则恢复。**不存在"撕裂快照"**。
- **`fsync` 语义**：若要求"返回时该行已落盘"，需在 copy 后有新变异时**循环重试到收敛**（对齐内核对 `I_SYNC` 的处理）；**后台 sweep 则 best-effort**（留脏下轮再写）。二者共用同一函数，靠 `bool must_flush` 区分。

### 3.2 淘汰回写：独占窗口内 **原地 0 拷贝 I/O**

> **实现注（2026-09-27）**：本次落地用 **bh**（`sb_getblk` 全覆盖写）实现本例的“原地”味；真正的 **0 拷贝 bio 未落地**（见 **§11**）。

淘汰时行**已互斥**（`EVICTING` + `ref==1`）⇒ 内容不会被任何 session 改 ⇒ **无需拷贝**：

```
分片写锁: 选 victim（READY ∧ !DIRTY? 或 DIRTY 都可 ∧ ref==1 ∧ 超龄）
          置 EVICTING（留在 xarray）；解锁
（若 DIRTY）直接 submit_bio from row->page（原地，无 bounce、不读盘）
          等 I/O 完成
分片写锁: xa_erase → free(page) + free(控制器)
```

- **就地释放三件套**：`__free_pages(row->page)` + `kfree(row)` + 销毁其锁（`rwlock`），即「**释放缓存行控制器**」。
- **原地 I/O 优于现状**：现 `ravenfs_row_write` = `sb_bread`(多读) + `memcpy`(双重缓冲) + `mark_buffer_dirty/sync_dirty_buffer`（走 bdev 页缓存）。原地 = **一次 bio 从 `row->page` 直接发**（inode 行 ≤64K=16 连续块 / mdata 8K=2 块），**跳过 bdev 页缓存**。

### 3.3 两味对比一句话

**驻留要"留行给并发访问" → 必须拷（且拷贝时排他）；淘汰"行已独占" → 不拷（原地写 + 释放）。**

---

## 4. 并发矩阵与锁纪律

### 4.1 锁的分工

| 锁 | 护什么 | 说明 |
|---|---|---|
| **分片 `rw_semaphore`** | xarray 成员 + `ref` pinning + `state` 的装载/淘汰转换 | `get` 在**读锁内** `xa_load` + `ref++`；淘汰在**写锁内**判 `ref==1` 置 `EVICTING`/擦除 → 二者互斥，封住 get-vs-淘汰 TOCTOU |
| **per-row `io_lock`（rw_semaphore）** | 行内容 + `{DIRTY, WB, wb_stamp}` | 读=session 读（共享）；写=session 变异 / copy-out / 淘汰转换 |
| **`acc_stamp`（atomic64 relaxed）** | TTL 时间戳 | `get` 直写，不参与排他（尽力而为，对齐 god） |

> **铁律：I/O 绝不在任何锁内发生**（copy-out 后锁外 I/O；淘汰原地 I/O 也在分片锁释放后）。这条**修掉缺陷 ③**，也是本设计相对现实现的关键纪律。

### 4.2 并发矩阵

| 两两 | 关系 | 机制 |
|---|---|---|
| **copy-out 临界区 ↔ session** | **互斥（全部：读 + 写）** | `io_lock` 写锁 |
| 锁外 I/O ↔ session | **并发** | I/O 不持锁 |
| session 读 ↔ session 读 | 并发 | `io_lock` 读锁共享 |
| session 变异 ↔ 任何 | 互斥 | `io_lock` 写锁 |
| 会话 ↔ 淘汰 | 互斥 | 分片写锁 + `ref==1` |
| 回写 ↔ 淘汰 | 不重叠 | 淘汰只收 `READY`；回写期间 pin（§4.3） |

### 4.3 回写必须 **pin**（防淘汰 UAF）

淘汰条件是 `ref==1`。驻留回写在**锁外 I/O 期间**若不 pin，该行可能被淘汰方 free（`ref==1` 成立）→ I/O 回来 commit 时**行已没 → UAF**。
⇒ **驻留回写在 I/O 期间持一个额外 ref（pin）**，使 `ref≥2`，淘汰取不到它。**淘汰味不需要 pin**（它自己就是 `ref==1` 的支配者）。

---

## 5. 周期回写（TTL 线程扩展）

TTL 线程（`linux/cache_ttl.c`，现为 `delayed_work`）每轮做**两趟**，都在锁外 I/O：

```
每 reap_ms（= 4s）：
  趟1 淘汰  ：READY ∧ ref==1 ∧ now-acc_stamp > ttl(2s) → 原地回写(若脏)+释放
  趟2 回写  ：READY(dirty) ∧ now-wb_stamp > wb_age(60s) → copy-out 回写
```

- **效果**：dirty 集**有界**（最坏 = `wb_age` 窗口内的修改量）→ 崩溃窗口从「自上次 sync 起」收缩到「≤60s」。**修掉缺陷 ②**。
- **TTL 轮次 1s → 4s**（用户裁决）：淘汰判定粒度变粗（2s TTL 最长 ~6s 才被收）——可接受（TTL 是冷工作集近似）；换来**唤醒次数 ÷4**（省 CPU / xa 抖动）。`ttl` 与 `wb_age` 独立。

---

## 6. 架构无关回刷 driver

把"回写"抽成 **vtable**，`sync_fs` / `umount` / TTL 线程共用一张 driver；**两类缓存后端**注册进来：

```c
struct ravenfs_wb_backend {
    const char *name;
    const void *geo;   /* 行尺寸 / 块映射 / 几何（每种后端自有） */
    /* 锁内：收集「该回写」的行 → 快照内容 + 记句柄（pinned 批） */
    int  (*collect)(void *cache, u64 age_cutoff_ns, bool ignore_age, struct wb_batch *b);
    /* 锁外：把快照写盘（后端自选 bh / bio / page-cache） */
    int  (*write)(void *cache, struct wb_item *it);
    /* 锁内：清 DIRTY + 更新 wb_stamp（驻留味）；淘汰味则 erase+free */
    void (*commit)(void *cache, struct wb_item *it);
};

void ravenfs_wb_sweep(struct super_block *sb, u64 age_cutoff_ns, bool ignore_age);
```

- **TTL 线程**：`ravenfs_wb_sweep(sb, now - wb_age, false)`（按龄）。
- **sync/umount**：`ravenfs_wb_sweep(sb, 0, true)`（全量，忽略龄）。
- **位图（`bitmap.c`）＝非缓存后端，不接入本 driver**：位图 = 全量常驻内存分片（无行状态机）；其同步由 bitmap 模块**自有的分片级同步**承担——分片级（脏分片）回写 + `sync_fs`/`put_super` 刷回；**不得挂载后仅驻留内存**。
  → 与缓存行的"行级 copy-out / 淘汰"是两码事，driver 只覆盖 inode/mdata 两类缓存后端 ⇒ **架构无关回刷**。

> 位图 = 全量常驻内存分片 + 分片级同步（脏分片回写 + `sync_fs`/`put_super` 刷回）；**该机制归 bitmap 模块自身，不并入缓存回刷 driver**。

---

## 7. 决策记录（2026-09-27）

| ID | 裁决 | 理由 |
|---|---|---|
| **C-1** | 驻留回写 **copy-out，且 copy 临界区排他挡全 session** | 排他 ⇒ 一致快照 ⇒ **可删 gen** |
| **C-2** | 锁外 I/O（copy-out 后 / 淘汰原地） | 修缺陷③；不阻塞 session/分片 |
| **C-3** | 淘汰 **独占窗口内 0 拷贝原地 I/O + 释放控制器**（实现以 bh 替代 bio，见 §11） | 行已互斥，拷贝白费；顺带甩掉 bdev 页缓存双重缓冲 |
| **C-4** | 淘汰与 session **互斥** | 防 `s->row` 悬垂 |
| **C-5** | 双时间戳：`acc_stamp`（淘汰）/ `wb_stamp`（回写 aging） | 冷集淘汰 vs 脏集老化，两码事 |
| **C-6** | TTL 轮次 **4s** | 唤醒 ÷4；TTL 非精确计时 |
| **C-7** | 回写/淘汰 **架构无关 driver**（**缓存后端**：inode/mdata） | 一套机制；位图非缓存、另有分片同步 |
| **C-8** | 淘汰状态 `EVICTING`（保留在 xarray，get 重试） | 防同址重载出第二份 |

---

## 8. 落地顺序（**M4.2 的前置**，非仅 M7）

> ⚠️ M4.2 的 extent 树**写节点走 `mdata_cache`**——缓存行状态机/回写若有缺陷①，**extent 写入继承丢更新 bug**。

1. **修缺陷①**（dirty 锁纪律：`mark_dirty` / `sync_from_vfs` / `reclaim` 全部改为经 `io_lock`）——**最小且正确性必需，可立刻做**。
2. 补回状态机 + 双时间戳 + `WB`/`EVICTING`（§2）。
3. TTL 线程扩展周期回写（§5）+ wb driver（§6）。
4. 位图**分片级同步**落地（§6；bitmap 模块自有，**非缓存后端**）。
5. **顺带**：`ravenfs_row_write`/`ravenfs_mrow_write` 从"sb_bread+memcpy+bh"改为**原地 bio**（淘汰味标准路径；驻留味用 copy-out）。

**建议 1+2 与 M4.2 同批或先行**；3+4 可归 M7 加固。

---

## 9. 参数表

| 参数 | 默认 | 说明 | 现状 |
|---|---|---|---|
| `ravenfs_ttl_seconds` | 2 | 冷行 TTL（`acc_stamp` 距now） | 2（不变） |
| `ravenfs_reap_ms` | **4000** | TTL 线程轮次 | ~~1000~~ **改** |
| `ravenfs_wb_age_seconds` | 60 | 脏行回写 aging（`wb_stamp` 距now） | 新增 |
| `ravenfs_ttl_seconds=0` | — | 关闭 TTL 淘汰（既有语义保留） | 不变 |

---

## 10. 与既有文档的接口 / 勘误

| 文档 | 动作 |
|---|---|
| `linux/cache_ttl.c` 注释 | **勘误**：`TTL 默认 2s / 扫描周期 1000ms` → 扫描 **4s**；补「轮次含超龄回写」 |
| `ravenfs-linux-ko-requirements-spec.md` §4.11 | **新增** `R-IO-03 缓存行状态机与周期回写`（本文为其实体化） |
| `ravenfs-linux-ko-handover.md` §3.3（位图）/§3.5（分配器） | **挂锚点** → 本文 |
| 同上 §8（缺口） | 补：dirty 无锁写（活 bug①）/ dirty 无周期回写（②）/ 回写持锁 I/O（③） |
| `Docs/linux/ravenfs-linux-ko-M4-data-spec.md` §10 | 连带未清项之一（delalloc 回写）**引用本文** |

---

## 11. 实现状态（2026-09-27 落地）

✅ **已落地**（`linux/`，host 编译通过 `ravenfs.ko`，`make` 无 error）：

| 文件 | 改动 |
|---|---|
| `ravenfs.h` | 行结构（`io_lock`/`flags`/`state`/`acc_stamp`/`wb_stamp`）+ `ROW_*`/`ROWF_*` + `RFS_ROW_MARK_DIRTY` 宏 + `ravenfs_wb_ops` + 声明 |
| `inode_cache.c` | 状态机 + `ravenfs_cache_wb_dirty`（copy-out 排他 → 锁外 I/O，删 gen）+ `reap_expired`（淘汰独占窗口写+释放）+ EVICTING 重试 + I/O 全出锁 |
| `mdata_cache.c` | 同构镜像 + `mdata_drop` 走 `io_lock` + 回刷 ops |
| `cache_ttl.c` | 两趟（淘汰 + 超龄回写）+ `reap_ms` **1s→4s** + `wb_age_seconds=60` + 走 `ravenfs_wb_ops` driver |
| `dentry.c`/`dentry_write.c`/`name_heap.c` | 18 处裸写 dirty/stamp → `RFS_ROW_MARK_DIRTY`（**修缺陷①**） |

**⚠️ 一处偏离设计（已上报用户）**：§3.2 的 **「0 拷贝原地 bio」未落地**——当前用 **bh**（`sb_getblk` 全覆盖写 / `sb_bread` 读），因为：
1. 与 spec **R-IO-01b（元数据节点 = b1 buffer head）** 一致；
2. 免 raw bio（sector 数学 / 完成回调）风险——**宿主无法 guest 实测**时不宜引入。

「seam」已备：`ravenfs_row_store` / `ravenfs_mrow_store` 是唯一 I/O 函数，换 bio 只动这两处；⚠️ **读写必须同源**（都用 bio 或都用 bh），否则 bdev 页缓存陈旧 → 静默损坏。淘汰味当前也走 bh（多一次 memcpy，非 0 拷贝）。

**未落地**：位图**分片级周期同步**（§6，归 M7；bitmap 模块自身，**非缓存后端**）；独立 `WRITING` 态（用 `ROWF_WB` 标志替代，语义等价）；`wb_dirty` 已加**防活锁上限**（每片 ≤256 轮）。

**guest 测试步骤**（交接，手测）：
1. 按 serial `deadbeef` 认盘 → `insmod ravenfs.ko` → 挂载。
2. 造 dirty（`mkdir`/`dd`）；`dmesg` 看 `ravenfs: ttl: reap=… wb_err=…`（4s 一轮）。
3. **不 sync** 等 >60s → 应见周期回写；`sysrq-b` 掉电重启 → `audit_v1.2` 验证。
4. 核验 `ravenfs_reap_ms`/`ravenfs_wb_age_seconds`/`ravenfs_ttl_seconds` 参数生效。

---

## 12. 实测收口（2026-09-27 晚 · guest：rfsdirchurn + rfsdirparallel）

**关键教训（回归 → 修复）**：首版实现把落盘从 `sb_bread`（读-改-写）改成 **`sb_getblk`（免读全覆盖写）**
→ **rfsdirchurn 出现非确定块泄漏**（旧 ko 10/10 PASS；新 ko ~30–50% FAIL，Δ-8196/4100，落 r1/r2 的 G2/G3）。
回退 **`sb_getblk → sb_bread`** **并**把 wb/reap 收敛为**单临界区（持分片写锁）**后 → **rfsdirchurn 12/12 PASS**。
（未逐字节定位成因；经验判据：**读写同源 + 与 drop/free 严格互斥**。已被单个样本误导多次——**非确定性 bug 必须多轮取样**。）

**最终落地形态（比设计更保守，性能换正确——用户：“浪费点鸡毛性能可接受”）**：

| 项 | 设计意图 | **实际落地** |
|---|---|---|
| 淘汰 reap | 独占窗口 0 拷贝原地回写 | **clean-only、单临界区 `erase+free`（无 I/O、无脏回写）** |
| 驻留回写 wb | copy-out 排他 + 锁外 I/O | **持分片写锁整段回写（复刻旧 `writeback_all` 锁序）** |
| 落盘 | in-place bio | **`sb_bread` 读-改-写**（与装载同源） |
| 周期 | TTL 4s 两趟 | ✅ 落地（淘汰 + 超龄回写，`reap_ms=4000`/`wb_age=60s`） |
| 缺陷① | io_lock 标脏 | ✅ 落地（`RFS_ROW_MARK_DIRTY`，18 处外部裸写收敛） |
| 位图分片级同步 | bitmap 模块自有（**非缓存后端**） | 未落地（M7） |

**实测结果**：
- `rfsdirchurn <mnt> 2`（5 组×2 轮，747109 条长名）：**12/12 PASS**（bfree/ffree 精确回基线）。
- `rfsdirparallel`（6 目录并发乱序 mkdir/rmdir，各持 512 存活名）：
  - 1 worker ×40000：wall **0.50s**、**80k ops/s**；
  - 6 worker ×40000：wall **0.64s**、**377k ops/s（≈4.7× 提升）**，各 worker 自身耗时均 ≈**0.63s**；
  - 3 worker：499k ops/s。
  → **目录改操作跨目录并行良好**（不同目录不互相串行；§4.2 的「同目录 i_rwsem 串行」推断得证）。

---

> **一句话**：两类缓存行一套状态机（`LOADING/READY/EVICTING` + `DIRTY/WB`）；位图非缓存行、另有分片级同步；
> **驻留回写 = copy-out 排他挡全 session、锁外 I/O（删 gen）、I/O 期间 pin**；
> **淘汰回写 = 独占窗口原地 0 拷贝 I/O + 释放页与控制器**；
> 双时间戳 + TTL **4s** 轮次（淘汰 + 超龄回写）+ 架构无关回刷 driver；
> **先修缺陷①，随 M4.2 落地**。

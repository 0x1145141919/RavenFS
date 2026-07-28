# RavenFS Bitmap Cache — 设计契约（最终版）

> 2026-07-23 定稿。
> 定义位图元数据的分片管理模式：全量 bitmap 元数据在 FS_block 世界内按地址区间切分成 N 个 controller，
> 每个处理器同一时间只服务一个 controller（一个桶），通过上下线切换实现多段覆盖。
>
> `BitmapCacheController` 是自包含模块，自己分配管理所有内部内存。
> 上帝类只传入 `BitmapCacheConfig`，析构时自动刷盘清理。

---

## 1. 核心架构

```
BitmapCacheController（自包含模块）
├── all_controller[]           ← 内部 new/delete
├── all_proc_state[]           ← 内部 new/delete
├── raw_buckets[][]            ← 内部 new/delete（processor_max × bucket_sz）
├── sweeper_bucket             ← 专职 buffer
└── sweeper (std::thread)

上帝类只传:
  BitmapCacheConfig { dev, bitmap_base, bitmap_block_count, processor_max, N }
```

每个 controller 管理一段 bitmap 元数据。同一时间每个处理器手里只有一个桶的数据（write-back），
未被任何处理器临幸的 controller → `state = OFFLINE`。

---

## 2. 数据结构（最终版）

### 2.1 返还记录

```c
struct bitmap_return_record_sll {
    bitmap_return_record_sll *next;
    uint64_t  base;           // 全局 FS_block 起始（不是段内偏移）
    uint64_t  entry_count;    // 区间长度：[base, base + entry_count)
};
```

跨 controller 释放时生成延迟记录，挂在目标 controller 的 `head` 链表上。
`base` 使用全局 FS_block 号而非段内偏移，保证 release 记录不依赖上下文。

### 2.2 控制器

```c
/* ─── 常量 ─── */
static constexpr uint8_t BITMAP_CTRL_OFFLINE     = 0;
static constexpr uint8_t BITMAP_CTRL_ONLINE      = 1;
static constexpr uint8_t BITMAP_CTRL_INTEGRATING = 2;   /* sweeper 专用 */

struct BitmapBlockController {
    spinlock_cpp_t            lock;        // 保护 state/free_count/head
    uint8_t                   state;       // 0=OFFLINE, 1=ONLINE, 2=INTEGRATING
    uint16_t                  flags;       // 扩展预留
    uint32_t                  free_count;  // 本段空闲 FS_block 计数（近似值）
    uint64_t                  scan_hint;   // 上次成功分配的 bit 偏移
    bitmap_return_record_sll *head;        // 外来归还链表
};
```

| 状态 | 含义 | 谁可使用 |
|------|------|---------|
| OFFLINE | 不在任何处理器桶中，可被 switch_to 或 sweeper 接管 | 任何 processor |
| ONLINE | 在某处理器桶中，热路径可用 | 绑定的 processor 独占 |
| INTEGRATING | sweeper 正在整合，不能上盘 | 仅 sweeper |

**`free_count` 语义：** 近似值，只作快速否定条件（`free_count < count` → 肯定不够）。
跨核归还会 `+count`，热路径分配会 `-count`。head 链表消费后 bit 清掉但不减 `free_count`
（已加过的不再重复减）。`free_count` 偏大不崩，偏乐观不影响正确性。

**`scan_hint` 语义：** 上次成功分配的 bit 偏移。下次分配从 `hint + 1` 开始扫描卷绕。
随 controller 上下线持久化——切走时保留，切回来继承。只在一个 processor 在 `ONLINE` 时写入，
不做多写者竞争。

### 2.3 每处理器分配状态

```c
struct ProcessorBitmapState {
    spinlock_cpp_t   alloc_lock;     // 保护分配状态机
    uint64_t         current_cid;    // 当前桶的 controller_id（~0ULL = 无）
    uint64_t         last_hint_cid;  // 上次分配成功的 cid（慢路径扫描起点）
    uint8_t         *bucket;         // 桶数据指针
};
```

`current_cid == ~0ULL` 表示此槽未指向任何 controller。

### 2.4 配置包

```c
struct BitmapCacheConfig {
    const BlockDevice     *dev;                     // I/O 通道
    uint64_t               bitmap_base;             // Bitmap 数组基址（FS_block 号）
    uint64_t               bitmap_block_count;      // Bitmap FS_block 总数上限
    uint16_t               processor_max;           // 最大处理器数
    uint8_t                num_of_blocks_per_bucket_log2; // N
};
```

不再包含指针字段（`all_controller`、状态表、`local_scan_buf` 全部删除），
模块内部自己管理全部内存。

---

## 3. BitmapCacheController 类接口

```cpp
class BitmapCacheController {
public:
    explicit BitmapCacheController(const BitmapCacheConfig &cfg);
    ~BitmapCacheController();

    // 禁止拷贝/移动
    BitmapCacheController(const BitmapCacheController &) = delete;
    BitmapCacheController &operator=(const BitmapCacheController &) = delete;

    // ── Core API ──
    uint64_t hard_alloc(uint64_t count);       // 硬性连续，~0ULL = 全盘满
    uint64_t soft_alloc(uint64_t max_count);   // 尽力分配，~0ULL = 全盘满
    void     free(uint64_t fblock, uint64_t count);

    // ── 上帝类驱动的关闭 ──
    void shutdown_all();

    bool is_valid() const;
};
```

---

## 4. 两层锁协议

### 锁序（单向，无死锁）

```
alloc_lock → controller.lock
```

永不反向。跨核归还只握 `controller.lock`，不握 `alloc_lock`。

### 各操作持锁范围

| 操作 | alloc_lock | controller.lock | 持锁时间 |
|------|-----------|----------------|---------|
| 热路径分配 | 持有 | 仅更新 free_count/scan_hint | 短 |
| consume_head 重试 | 持有 | 持有（消费链表） | 短 |
| switch_to Phase 1 | 持有 | 持握（改 state） | 极短 |
| switch_to Phase 2 | 无锁 | 无锁（纯 I/O） | — |
| switch_to Phase 3 | 持有 | 持有（消费 head） | 短 |
| 跨核归还 | 不持有 | 持有（挂链表） | 极短 |
| sweeper | 不持有 | 持有（三临界区） | 可控 |

### 关键原则

- **I/O 不在锁内。** flush_bucket / load_bucket 在 Phase 2 无锁区间执行。
- **controller.lock 只保护几行 field update。** 热路径上 bitmap 扫描（可能几百字节）完全在 alloc_lock 保护下进行，不占 controller.lock。
- **跨核归还无锁依赖。** 提交方不需要知道目标 controller 的状态是 OFFLINE/ONLINE/INTEGRATING。

---

## 5. 三层分配协议

### 5.1 hard_alloc(count)

```
Tier 1（本地桶）
  alloc_lock
  └─ try_alloc(): scan_run(hint) → 找到 → set_bits → update fields → return
  └─ 空仓 → consume_head → retry → 还空 → unlock → mark[TRIED]

Tier 1.5（借别人的桶）
  for each other processor:
    lock peer->alloc_lock
    └─ peer->current_cid → try_alloc() → 找到 → return
    unlock
  mark[TRIED]

Tier 2（找 OFFLINE controller，I/O 切换）
  for cid in ring_scan(last_hint_cid):
    if state != OFFLINE || free_count < count → skip
    switch_to(current_cid, cid)
    try_alloc() → 找到 → return
    mark[TRIED]

return ~0ULL  // 全盘满
```

### 5.2 soft_alloc(max_count)

与 hard_alloc 同三层结构，区别在于：

| | hard_alloc | soft_alloc |
|---|---|---|
| 扫描 | `scan_run(need)` 找第一个 ≥need 的 run | `find_longest(max)` 找最长 ≤max 的 run |
| 契约 | 要么 count 个连续，要么 0 | 实际返回 1~max_count |
| 用途 | 元数据、小文件、对齐要求 | 大文件连续写入 |

### 5.3 `try_alloc` 内部逻辑

```c
uint64_t try_alloc(ProcessorBitmapState *ps, uint64_t cid, uint64_t count) {
    // ① 从 scan_hint 开始扫
    pos = scan_run(bucket, bucket_bits, ctrl->scan_hint, count);
    if (found) goto alloc;

    // ② 空仓 → consume_head 重试一次
    lock(controller.lock)
      consume_head(bucket, ctrl)
    unlock(controller.lock)

    pos = scan_run(bucket, bucket_bits, 0, count);
    if (not found) return ~0ULL;

alloc:
    set_bits(bucket, pos, count);
    lock(controller.lock)
      free_count -= count;
      scan_hint = pos + count - 1;
    unlock(controller.lock)
    return encode(cid, pos);
}
```

先 `consume_head` 再重扫，保证在放弃当前 cid 之前不会漏掉刚归还的块。

---

## 6. switch_to — 三阶段切换

```c
void switch_to(uint64_t old_cid, uint64_t new_cid) {
    // ── Phase 1: 持锁，无 I/O ──
    alloc_lock.lock();
    if (old_cid != ~0ULL) {
        controller[old_cid].lock.lock();
        controller[old_cid].state = OFFLINE;
        controller[old_cid].lock.unlock();
    }
    controller[new_cid].lock.lock();
    controller[new_cid].state = ONLINE;
    controller[new_cid].lock.unlock();
    current_cid = ~0ULL;
    alloc_lock.unlock();

    // ── Phase 2: 无锁 I/O ──
    if (old_cid != ~0ULL) flush_bucket(bucket, old_cid);
    load_bucket(bucket, new_cid);

    // ── Phase 3: 持锁收尾 ──
    alloc_lock.lock();
    controller[new_cid].lock.lock();
    consume_head(bucket, &controller[new_cid]);
    controller[new_cid].lock.unlock();
    current_cid = new_cid;
    last_hint_cid = new_cid;
    alloc_lock.unlock();
}
```

- Phase 1 将 new_cid 标记为 ONLINE，防止其他 processor 或 sweeper 抢走。
- Phase 1→3 之间的缝隙中，跨核归还的 record 推到 head 链表，Phase 3 一次性消费。
- `old_cid = ~0ULL` 表示首次加载（无旧桶要刷）。

---

## 7. free — 返还逻辑

```c
void free(uint64_t fblock, uint64_t count) {
    if (count == 0) return;

    f = decode(fblock);

    // ── 归本桶管：直清 bit ──
    if (ps->current_cid == f.controller_id) {
        alloc_lock.lock();
        clear_bits(bucket, segment_relative, count);
        controller.lock.lock();
        free_count += count;
        controller.lock.unlock();
        alloc_lock.unlock();
        return;
    }

    // ── 跨桶：挂 record ──
    rec = malloc(sizeof(bitmap_return_record_sll));
    rec->base = fblock;       // 全局 FS_block 号
    rec->entry_count = count;
    controller[f.controller_id].lock.lock();
    rec->next = controller[f.controller_id].head;
    controller[f.controller_id].head = rec;
    controller[f.controller_id].free_count += count;
    controller[f.controller_id].lock.unlock();
}
```

跨桶归还不握 `alloc_lock`、不知道 bucket、不知道 state。只握 `controller.lock`，
推链表 + 更新 `free_count`，无论目标 controller 当前在谁手上。

---

## 8. Sweeper（专职线程）

### 设计

- 专职线程，绑定一个专用 bucket buffer（不属于任何 processor）。
- 几秒~几十秒循环一次。
- 只处理 `state == OFFLINE && head != nullptr` 的 controller。
- 通过 `INTEGRATING` 状态防止被 `switch_to` 抢走。

### 骨干逻辑

```c
for cid in all_controllers:
    if head == nullptr || state != OFFLINE: continue

    // ── 临界区①: 标记 INTEGRATING，摘空 head ──
    controller.lock.lock()
      if state != OFFLINE || head == nullptr: unlock; continue
      state = INTEGRATING
      consume_head(sweeper_bucket, ctrl)  // 摘链清 bit
    controller.lock.unlock()

    // ── 临界区外①: 读盘到专职 buffer ──
    load_bucket(sweeper_bucket, cid)

    // ── 临界区②: 消费在线期间积压的 head ──
    controller.lock.lock()
      if head != nullptr: consume_head(sweeper_bucket, ctrl)
    controller.lock.unlock()

    // ── 临界区外②: 写盘 ──
    flush_bucket(sweeper_bucket, cid)

    // ── 临界区③: 恢复 OFFLINE ──
    controller.lock.lock()
      state = OFFLINE
    controller.lock.unlock()
```

**不碰 ProcessorBitmapState。** 不读 `current_cid`，不改 `alloc_lock`。

---

## 9. 析构 / shutdown 流程

```c
void shutdown_all() {
    // 1. 停 sweeper
    stop_sweeper();

    // 2. 逐核下野刷桶
    for each processor:
        flush_processor(pid);   // 下线 → flush_bucket

    // 3. 逐 controller 清偿 head 链表
    for each controller:
        flush_controller(cid);  // 读盘 → clear bits → 写盘
}

void flush_processor(uint64_t pid) {
    if current_cid == ~0ULL: return;
    alloc_lock.lock()
      controller.lock.lock()  → state = OFFLINE
      controller.lock.unlock()
      current_cid = ~0ULL
    alloc_lock.unlock()
    flush_bucket(bucket, cid);
}

void flush_controller(uint64_t cid) {
    controller.lock.lock()
      head = ctrl->head; ctrl->head = nullptr; state = OFFLINE
    controller.lock.unlock()
    if head == nullptr: return

    // 按 bitmap FS_block 分组：read → clear → write
    for each record in head:
        for f in [base, base + entry_count):
            block = bitmap_base + (f / 32768)
            read_block(block, tmp)
            clear bit = f % 32768
            write_block(block, tmp)
    free all records
}
```

析构函数：`shutdown_all()` + `delete[]` 全部内部数组。
**不 I/O 在析构里执行，但 shutdown_all 负责清账，析构只清理堆内存。**

---

## 10. 构造阶段

```c
BitmapCacheController(const BitmapCacheConfig &cfg) {
    // ① 推导常量
    ctrl_count     = ceil(bitmap_block_count / (1 << N));
    bucket_sz      = (1 << N) * 4096;
    bucket_bits    = (1 << N) * 32768;
    ctrl_seg_bits  = 1ULL << (N + 15);

    // ② 分配 all_controller[]
    all_controller = new BitmapBlockController[ctrl_count]();

    // ③ 校准 free_count：遍历每个 controller 的所有 bitmap 块
    for each cid:
        read all bitmap blocks in range
        count zero bits → free_count
        state = OFFLINE, scan_hint = 0, head = nullptr

    // ④ 按 free_count 降序排序 cid
    //    选取最富的 min(processor_max, ctrl_count) 个

    // ⑤ 分配 per-processor 状态 + bucket
    all_proc_state = new ProcessorBitmapState[processor_max]();
    raw_buckets    = new uint8_t[processor_max * bucket_sz]();
    sweeper_bucket = new uint8_t[bucket_sz]();

    // ⑥ 预填充：每个处理器分配最富的 controller
    for p = 0..assign_cnt:
        load_bucket(bucket[p], richest_cid[p])
        current_cid = richest_cid[p], state = ONLINE
        // 未分配的 processor 保持 current_cid = ~0ULL

    // ⑦ 启动 sweeper 线程
    sweeper = std::thread(sweeper_loop);
}
```

构造阶段通过扫描磁盘校准 `free_count`，确保初始分配不依赖任何外部元数据一致性校验。

---

## 11. 推导公式汇总

```c
// 给定全局 FS_block X:
uint64_t controller_id     = X >> (N + 15);
uint64_t segment_relative  = X & ((1ULL << (N + 15)) - 1);

// 编码（段内分配完成后）:
uint64_t global_fs_block   = (cid << (N + 15)) + segment_relative;

// 桶内操作（已知 segment_relative）:
uint64_t bitmap_blk_idx    = segment_relative >> 15;
uint64_t bit_in_block      = segment_relative & 0x7FFF;
uint64_t bucket_byte_off   = bitmap_blk_idx * 4096 + (bit_in_block >> 3);
uint8_t  bit_mask          = 1 << (bit_in_block & 7);
```

---

## 12. 内存占用示例

| N | bucket_bytes | ctrl_count（4TB盘） | 16核 DRAM |
|---|------------|-------------------|-----------|
| 8 | 1MB | 128 | 16MB（桶）+ 1MB（sweeper）= 17MB |
| 10 | 4MB | 32 | 64MB（桶）+ 4MB（sweeper）= 68MB |
| 12 | 16MB | 8 | 128MB（桶）+ 16MB（sweeper）= 144MB |

相较于全量加载 bitmap 的 ~128MB（4TB盘），N=8 时节省约 7.5×。

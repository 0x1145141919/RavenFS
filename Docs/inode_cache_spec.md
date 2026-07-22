# RavenFS Inode Cache — 设计契约

## 1. 核心思想

数据面与控制面分离。data line 地址由 `(set, way)` 坐标直接计算——全寄存器运算，零额外 load。

```
数据面：cache_lines[0..total_slots-1]   每 line = Inode_cache_line = 16 inode = 4096B
控制面：all_entries[0..sets-1]          每 set = controller_set = 8 × controller = 192B

slot_idx = set × 8 + way               ← 寄存器运算
data     = &cache_lines[slot_idx]       ← 零 load
```

## 2. 数据结构

### 2.1 数据行 — `Inode_cache_line` (4096B)

```c
struct Inode_cache_line {
    raven_Inode_t entries[16];  // 16 × 256B = 4096B
};
```

对应磁盘 inode array 中一个 FS_block。

### 2.2 控制器 — `Inode_cache_line_controller` (24B)

```c
struct Inode_cache_line_controller {
    uint16_t           flags;               // [V:1 | D:1 | reserved:14]
    spintrylock_cpp_t  lock;                // lock / try_lock / unlock
    uint8_t            reserved0;
    uint32_t           num_of_accessor;     // 引用计数，>0 时不可淘汰
    uint64_t           last_access_stamp;   // LRU 时间戳
    uint64_t           tag;                 // inum 高比特 [4+N:63]
    // 不存 data 指针——地址由 cache_lines[set × 8 + way] 计算
} __attribute__((packed));
static_assert(sizeof(Inode_cache_line_controller) == 24);
```

flags 位域：
- `V` — valid：该 slot 已载入有效数据
- `D` — dirty：数据已被修改，需回写磁盘
- `T` — transfer：数据传输中（I/O 进行中）。仅 `num_of_accessor == 0` 方可进入 T 态

### 2.3 组 — `controller_set` (192B)

```c
static constexpr uint8_t WAYS_PER_SET = 8;

struct controller_set {
    Inode_cache_line_controller controllers[WAYS_PER_SET];
};
static_assert(sizeof(controller_set) == 192);  // 24B × 8
```

### 2.4 控制器 — `Inode_cache_controller`

```c
class Inode_cache_controller {
    RavenFS          *fs;                  // 上帝类回指——唯一冗余路径
    controller_set   *all_entries;         // [sets]，控制面
    Inode_cache_line *cache_lines;         // [total_slots]，数据面
    uint8_t           num_of_entries_log2; // N: sets = 1 << N
    uint64_t          access_stamp_counter;

    // (set, way) → data line，全寄存器运算
    Inode_cache_line *slot_data(uint64_t set, int way) const {
        return cache_lines + (set * WAYS_PER_SET + way);
    }
};
```

## 3. 缓存行状态机

每个 slot 三个状态位：`V` (valid)、`D` (dirty)、`T` (transfer)。
**只有 V=1 T=0 的 slot 可以响应 cache_get 命中**。T=1 表示 I/O 进行中，不得访问数据。

状态定义、转换图、并发排除规则见独立文档：
`Docs/inode_cache_state_machine.md`

```
规则：T=0 → T=1 必须在 num_of_accessor == 0 下进行（锁内确认）
      任何 T=1 → T=0 的出口都由 I/O 完成触发

get(hit):
  V=1 T=0 tag match → accessor++ → stamp++ → 返回 data

get(miss → load_slot):
  lock → accessor==0 → T=1 → unlock         ──→ 进入 FETCHING
  read_block(data)                           ──→ I/O（不持锁）
  lock → tag=t, V=1, T=0, accessor=1 → unlock  ──→ 进入 VALID

eviction(dirty):
  lock → accessor==0 → T=1 → unlock         ──→ 进入 WRITEBACK
  write_block(data)                          ──→ I/O（不持锁）
  read_block(data)                           ──→ I/O（不持锁，紧接加载）
  lock → tag=t, V=1, T=0, accessor=1 → unlock  ──→ 进入 VALID

flush_inum(dirty → writeback):
  lock → accessor==0 → flags &= ~D, T=1 → unlock ──→ 进入 WRITEBACK
  write_block(data)                          ──→ I/O（不持锁）
  lock → T=0 → unlock                       ──→ 回到 VALID（V=1 不变）
```

### 并发排除

```
cache_get 扫描时:
  T=1   → 跳过该 slot（数据未就绪或正回写）
  V=0 T=0 → 跳过（空槽，继续扫

  V=1 T=0 tag match → 命中，开 session

select_victim 扫描时:
  accessor>0 → 跳过（有人用）
  T=1 → 跳过（I/O 中）
  V=0 T=0 → 优先选（空槽）
  否则选 accessor==0 T=0 中 stamp 最小者
```

## 4. inum 位域编码

inum 本身就是缓存索引，不哈希不取模。位域三段：

```
inum bits:
  ┌─────────┬──────────────────┬──────────────────────┐
  │ [0:3]   │ [4:4+N-1]        │ [4+N:63]             │
  │ row     │ set_idx          │ tag                  │
  │ 4 bits  │ N bits           │ remaining bits       │
  │ 行内引索│ all_entries 索引  │ 高比特验证           │
  └─────────┴──────────────────┴──────────────────────┘

N = num_of_entries_log2
sets        = 1 << N
total_slots = sets × 8
```

### 推导公式

```c
row     = inum & 0xF
set_idx = (inum >> 4) & ((1 << N) - 1)
tag     = inum >> (4 + N)
```

### 示例

`N = 10`（1024 sets，8-way，8192 slots）：

```
inum = 0x00000000_0001A34F

拆分 (N=10):
  [0:3]   = 0xF = 15           ← 行内第 15 个 inode
  [4:13]  = 0x1A34 & 0x3FF = 52  ← set 52
  [14:63] = 0x00068D            ← tag
```

## 5. 访问路径

### 命中路径

```
cache_get(inum) — hit:
  row     = inum & 0xF
  set     = (inum >> 4) & ((1 << N) - 1)
  tag     = inum >> (4 + N)
  cs      = all_entries[set]

  for way = 0..7:
    ctrl = &cs.controllers[way]
    if ctrl.flags.V && !ctrl.flags.T && ctrl.tag == tag:  // V=1 T=0
      ctrl.lock.lock()
      ctrl.num_of_accessor++
      ctrl.last_access_stamp = now++
      ctrl.lock.unlock()
      data = cache_lines + (set * 8 + way)
      return &data->entries[row]
```

### Miss 加载 — T 位解耦 I/O

```
load_slot(cs, way, inum, {row, set, tag}):
  ctrl = &cs.controllers[way]
  data = cache_lines + (set * 8 + way)

  // ─── Phase 1: lock → set T → unlock（只维护状态） ───
  ctrl.lock.lock()
  if ctrl.num_of_accessor != 0:
    ctrl.lock.unlock()
    return retry
  ctrl.flags |= T                                      // I/O 开始
  ctrl.lock.unlock()

  // ─── Phase 2: I/O（不持锁） ───
  if ctrl.flags.D:
    fs->write_block(inum_to_block(inum), data)
  fs->read_block(inum_to_block(inum), data)

  // ─── Phase 3: lock → clear T, set V → unlock ───
  ctrl.lock.lock()
  ctrl.tag   = tag
  ctrl.flags = V                                       // T 自动清零
  ctrl.num_of_accessor = 1
  ctrl.last_access_stamp = now++
  ctrl.lock.unlock()
  return &data->entries[row]
```

`inum_to_block` 走 `fs->geo.inode_array_base`，零冗余：

```c
uint64_t inum_to_block(uint64_t inum) const {
    return fs->geo.inode_array_base + (inum / 16);
}
```

### cache_put

```
cache_put(inum):
  set = (inum >> 4) & ((1 << N) - 1)
  tag = inum >> (4 + N)
  cs  = all_entries[set]

  for way = 0..7:
    ctrl = &cs.controllers[way]
    if ctrl.flags.V && ctrl.tag == tag:
      ctrl.lock.lock()
      ctrl.num_of_accessor--
      ctrl.lock.unlock()
      return
```

## 6. 淘汰策略 — `select_victim`

每个 controller_set 内 8 路：

1. 优先选 `flags.V == 0`（空槽）
2. 其次选 `num_of_accessor == 0` 中 `last_access_stamp` 最小的（LRU）
3. `ctrl.lock.try_lock()` — 抢锁失败则选下一路
4. 锁内确认 `num_of_accessor == 0`，否则解锁重选

被选中后淘汰流程在 `load_slot` 中执行（脏回写 + 加载新数据）。

## 7. 内存上限

cache_lines 固定上限，不随文件系统规模增长：

```
total_slots = sets × 8
cache_lines 占用 = total_slots × 4096

典型值:
  sets = 1024  → total_slots = 8192  → 32MB
  sets = 512   → total_slots = 4096  → 16MB
  sets = 256   → total_slots = 2048  → 8MB

all_entries 占用 = sets × 192 (24B × 8)
                = < 200KB for all cases
```

超出容量的 inode 走**准入淘汰**，不额外分配。

## 8. Session 模型

### 生命周期

**关键原则：锁只维护状态原子性，绝不持锁做 I/O。**

```
cache_get(hit):
  lock() → num_of_accessor++ → stamp++ → unlock()
  ─── 不持锁 ───
  调用者读写 inode（num_of_accessor > 0 保证不被淘汰）

cache_get(miss → load_slot):
  lock() → flags |= T → unlock()           ← Phase 1: 状态
  write_block / read_block                  ← Phase 2: I/O（不持锁）
  lock() → tag=t, flags=V, accessor=1 → unlock()  ← Phase 3: 状态

cache_put:
  lock() → num_of_accessor-- → unlock()
```

### RAII — `InodeSession`

```cpp
class InodeSession {
    Inode_cache_controller *cache;
    uint64_t                inum;
public:
    raven_Inode_t          *inode;   // data line 内原位指针

    InodeSession(Inode_cache_controller *cache, uint64_t inum);
    ~InodeSession();                  // 自动 cache_put

    void dirty();                     // 标记 flag.D
    // move 语义，禁止拷贝
};
```

使用：

```cpp
{
    InodeSession s(&fs.inode_cache, inum);
    if (!s) return error;

    if (s.inode->type == INODE_TYPE_DIR) { ... }
    s.inode->size = new_size;
    s.dirty();
}
```

## 9. 与上帝类的关系

```cpp
// Userspace_God.hpp
#include "Inode_cache_system.hpp"   // 完整类型

class RavenFS {
    ...
    Inode_cache_controller inode_cache;  // 直接成员
    ...
};
```

`Inode_cache_system.hpp` 只前向声明 `RavenFS`，不包含 `Userspace_God.hpp`。

## 10. 移植路径

| 组件 | 用户态 | Linux 模块 | 你内核 |
|------|--------|-----------|--------|
| cache_lines 分配 | `mmap(N×4096)` | `vmalloc` / `alloc_page` | `FreePagesAllocator` |
| 锁 | `spintrylock_cpp_t` | `spinlock_t` | 已有 |
| I/O | `fs->read_block` → `pread` | `submit_bio` | 你的 I/O 层 |
| 核心算法 | **不改** | **不改** | **不改** |

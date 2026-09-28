# RavenFS v1.1 运行时科技 ③ — bitmap_slice 分片后端 chunk 化（懒加载）

> 2026-09-28
> 主题：`bitmap_slice_t` 的**底层存储**从「全量常驻」改为可选 backing（FULL | CHUNK）；
> CHUNK = 每片一个固定 chunk 缓冲 + chunk 控制器数组 + 懒加载 + 返还记录（record）。
> **顶层 `bitmap_slices_mgr_t` 接口零变化；FULL 模式行为/性能逐位不变（A/B 基线红线）。**
> 决策（用户裁 2026-09-28）：**以上帝类为改制锚点，先改 god、测完再移植 ko**；本文为两端共同规格。
> 代码映射：`include/ravenfs/bitmap_slices.hpp`（cfg/backing 契约）＋
> `juvenile_god/bitmap_slices.cpp`（改制层）＋ 02 文档（分片思想母文档，本文只写增量）。
> A/B（用户裁）：`backing_mode` 与 `chunk_order` 均为**装配期自由选择**，两条路线必须能对测性能。

---

## 0. 动机（数字锚点）

- nvme0n1 盘面实测：FS 块位图 **64MB**（512M 位 ≈ 2TB 盘）、inode 位图 **16MB**（134M 槽）→ 全量常驻 80MB。
- 热度分布：分配推进集中在**每片 hint 邻域**（顺序局部性），冷区"站着茅坑不拉屎"——
  常驻 80MB 换来的有效工作集 ≈ 每片一个 chunk。
- n=32 片 × 4MB/片；chunk=64K → 常驻 32×64K×2组 ≈ **4MB（40×↓）**；chunk=4K → ~256KB。
- 副产物：现状 TTL **无脏标记、每 30s 全量回写 80MB**（≈2.7MB/s 恒定写放大，02 §1 承认）；
  chunk 化后按脏写，该放大自动消失。
- 对 host（32GB）无感；**对 SparrowOS 目标机是硬收益** ⇒ 先在 god 上把懒加载的性能代价
  测清楚，再定移植取舍。

## 1. 决策与不变边界

1. **改制层 = `bitmap_slice_t` 内部 + backing 子结构**；mgr 顶层 8 个块/inode 接口
   （`block_alloc_hard/soft`、`block_occupy_soft`、`block_free`、`block_test`、
   `inode_alloc/free/test`、`*_free_total`）**签名与语义完全不变**。
2. **FULL 模式 = 现行为逐位、逐性能不变**（基线红线；现有测试必须原样通过）。
3. 锁纪律继承 02 §5/§6：alloc try 换片（god）/ free·occupy 自旋强抢 —— **各自现行为不动**。
4. CHUNK 模式三条用户裁语义（2026-09-28）：
   - 任一时刻**每片至多一个 chunk 上线**；
   - **离线 free → 挂 record；该 chunk 下次上线时先把 record drain 还给位图，才准参与工作**；
     **在线 free → 直接改内存**；
   - TTL 改走 **chunk 数组层**定时同步，不再拿全片锁阻塞分配。
5. 崩溃方向全 = **泄漏**（record 丢 / 在线未回写），与 M3.5 裁决同构
   （flush 外不损坏；泄漏 fsck 可收），**不引入双分配方向**（§9 不变量 5/7 保证）。
6. 改制顺序：god 落地（本文 §13 拆刀）→ 压测 A/B → ko 移植（§12 注记）。

## 2. backing 模式与 cfg 扩展（A/B 开关）

```cpp
/* include/ravenfs/bitmap_slices.hpp 增量 */
enum bitmap_backing_mode : uint8_t {
    BITMAP_BACKING_FULL  = 0,   /* 默认：全量常驻 = 02 文档现行为 */
    BITMAP_BACKING_CHUNK = 1,   /* 懒加载：每片一个 chunk 缓冲 + 控制器数组 */
};

/* CHUNK 模式的盘后端回调（保持本模块"零设备知识"契约：
 * 模块只认"某组的位图块区间"，fd/sb_bread 全由 ctx 解释）。 */
struct bitmap_chunk_io_t {
    /* 读/写 is_inode 组的 [disk_block_rel, +nblocks) 到/自 buf（4K 块粒度、4K 对齐）。
     * 返回 0 = 成功。CHUNK 模式必填；FULL 模式忽略。 */
    int (*read)(void *ctx, bool is_inode, uint64_t disk_block_rel,
                void *buf, uint64_t nblocks);
    int (*write)(void *ctx, bool is_inode, uint64_t disk_block_rel,
                 const void *buf, uint64_t nblocks);
};

struct bitmap_slices_cfg_t {
    ... /* 现有字段不动 */
    uint8_t  backing_mode  = BITMAP_BACKING_FULL;
    uint8_t  chunk_order   = 4;      /* 4K << order 字节；有效 0..8（4K..1MB），默认 64K */
    const bitmap_chunk_io_t *chunk_io = nullptr;   /* CHUNK 必填 */
    void    *chunk_io_ctx  = nullptr;
    bool     skip_self_check = false; /* 跳过 mount 自描述校验（单测合成位图用） */
};
```

约束（装配期校验，违反 → ctor 返 `RC_ERR_STATE`）：
- `chunk_bits_bytes = 4096 << chunk_order`；须 ≤ 最小片字节数；order 超 0..8 夹断/报错。
- CHUNK 必须配 `chunk_io`；**CHUNK 要求原始 fd 后端**——mmap 后端与 CHUNK 不叠用
  （mmap = 内核页缓存代管，另作 A/B 第三对照组，§11）。
- 默认 `backing=FULL`：**不传任何新字段的现有调用方行为零变化**。

## 3. 索引合成与尾块边界（数学，先钉死）

```
chunk_bits  = 32768 << order          （4K×2^order 字节 = 32768×2^order 位）
slice_bits  = 32768 × k               （02 §2：片恒 32768 倍数，不变量）

slice_off = 全局位号 − base_idx       （片内偏移）
ctl_idx   = slice_off >> (15 + order)  （位移即除法）
within    = slice_off & (chunk_bits − 1)
全局位号  = base_idx + slice_off       ← 对外坐标零换算（02 §3.4 继承）

尾块：tail_bits = slice_bits − (chunks_per_slice − 1) × chunk_bits
              = 32768 × (k mod 2^order)
     → tail == 0，或 tail ≥ 32768 位 = ≥1 整页（4KB）
     ⇒ **不存在 sub-page 尾块**，位移/掩码全链路安全（前提由现有 32768 对齐保证，
        无需任何运行期分支特判——只在控制器数组里记 tail_bits 供载入用）
```

## 4. chunk 控制器结构

```cpp
/* 片级 backing（挂在 bitmap_slice_t 下；mode=FULL 时全字段惰性不建） */
struct slice_backing_t {
    uint8_t  mode;                 /* FULL | CHUNK（cfg 拷贝） */
    uint8_t  chunk_order;
    uint32_t chunks_per_slice;
    uint32_t online_chunk;         /* 当前上线片；NONE 哨兵。变更一律持 slice_lock */
    uint32_t gen;                  /* 快路径复检（发布/下线时递增） */
    uint8_t *buffer;               /* 固定 chunk_bits_bytes，装配期一次分配（4K 对齐） */
    mutex_t  chg_mu;               /* 换片互斥：evict/load/record回灌/TTL写 串行（可睡眠） */
    chunk_ctl_t *ctls;             /* 控制器数组，chunks_per_slice 个，装配期构造好 */
    /* record 节点池：per-group 共享 LIFO（原子栈），锁内零分配的保证（§6） */
};

struct chunk_ctl_t {               /* 上线前即构造好，靠它做一切定位 */
    uint64_t disk_block_rel;       /* 本 chunk 首位图块（相对本组盘起点） */
    uint64_t tail_bits;            /* 尾 chunk 有效位数；非尾 = chunk_bits */
    uint8_t  summary;              /* 2bit：ALL_USED / ALL_FREE / MIXED（语义见 §7） */
    uint8_t  dirty;                /* ONLINE 且被改（TTL 按它跳过干净 chunk） */
    uint8_t  has_recs;             /* 片级链上有覆盖本 chunk 的 record（摘要降级凭据） */
};

struct rec_node {                  /* 片级 record 链节点（池分配） */
    uint64_t slice_off, len;       /* 片内偏移视角的连续区间 */
    rec_node *next;
};
```

要点：
- **record 挂片级链、不按 chunk 切分**——`block_free` 每片本就只产生单段（02 §5 逐段模式），
  单次 free 至多每片一个节点；长 run（TB 级 truncate）也只 1 节点，节点数天然有界（§6）。
- `slice_base` 重指向技巧（存量代码零改动的关键）：
  `slice_base = buffer − ctl_idx × chunk_bytes` → `slice_base + slice_off` 自动落进 buffer；
  片内既有算式（`bit_get(words, off)` 等）**一行不改**。
  唯一增补：入口**区间守卫**（`off+len` 须落在 `[chunk_lo, chunk_hi]`，越右界 = WINDOW_EMPTY
  触发换片，**不返错**；越左界同理由迭代器防）。

## 5. 快/慢路径状态机（核心；盘 I/O 永不入 slice_lock）

```
锁分工：
  slice_lock   —— 位操作、online_chunk/gen/dirty/record链 变更（短临界，零 I/O 零睡眠）
  backing.chg_mu —— 一切针对本片 chunk 的盘 I/O 与换片（可睡眠；TTL 亦持它）
  池           —— 原子 LIFO，slice_lock 内取还（零睡眠）
  铁律：盘 I/O 永不进 slice_lock。

快路径（常态 = hint 局部性命中在线 chunk，纯内存，与 FULL 同速）：
    ctl = slice_off >> (15+order)                    // 无锁读
    slice_lock.lock()
    if (online_chunk != ctl) { unlock; goto slow; }  // gen/指针复检兜住并发换片
    位操作（alloc/free/occupy/test 同款；free 撞在线 → 此处直改 + dirty=1）
    unlock; return HIT

慢路径（换片，全部串行在 chg_mu 下）：
    slice_lock.unlock(); chg_mu.lock()
      slice_lock.lock()
      if (online_chunk == ctl) { unlock; chg_mu.unlock(); goto fast; }  // 别人已换上 ✓
      if (online_chunk != NONE && dirty) {
          snapshot = copy(buffer); dirty = 0;         // 锁内：快照+清 dirty 原子
          slice_lock.unlock();
          chunk_io.write(snapshot);                   // 锁外写（仍持 chg_mu）
      } else slice_lock.unlock();
      chunk_io.read(buffer, 目标 ctl);                // 锁外读（仍持 chg_mu）
      slice_lock.lock()
        drain(片级链 ∩ [chunk_lo, chunk_hi])          // ★ 你定的顺序：上线前先还账
        online_chunk = ctl; gen++; dirty = 0;         // 发布
      slice_lock.unlock()
    chg_mu.unlock(); goto fast

record drain（在发布前，slice_lock 内）：
    遍历片级链，与本 chunk 窗口求交：
      整段覆盖 → 摘节点；部分 → 收缩 slice_off/len 留余
      交集内 1→0 清位（free_count 已在挂 record 时记过账，此处不再动计数）

为什么 TTL 的 online 写回必须持 chg_mu（反例）：
    若 TTL 锁外裸写：snapshot(c) → 写在途 → 另一 CPU 换片又把 c 换上（load 读盘）
    → 读到旧数据（写未落）→ 随后写落盘 → buffer 旧 / 盘新，后续 evict 干净判定错乱
    → 可能丢置位 = 双分配方向。持 chg_mu 则 load 必等写完成 ⇒ read-your-writes ✓
    （烧的代价：写盘 ~100µs 内同片换片等待；快路径不碰 chg_mu，分配不被阻塞 ✓）
```

补充：
- `soft_occupy` / `test_interval` / 跨 chunk 扫描在 CHUNK 下 = **窗口迭代**（§8），
  每段窗口各自走快慢路径；段间可换片，连续性由算法层保证（同 `block_free` 跨片逐段模式）。
- 同片并发换片被 chg_mu 串行；hint 单调推进 ⇒ 顺序迁移、无乒乓（并发落同片不同 chunk
  才乒乓，靠 hint 局部性压制——A/B 实测项 §11）。

## 6. 返还记录（record）与节点池

- 语义（用户裁，正确性支柱）：
  - **在线 free**：快路径直改内存（dirty=1），零 record；
  - **离线 free**：`slice_lock` 内从组池取节点 → 片级链追加 `{slice_off, len}` →
    `free_count/全局计数` 记账 → 解锁。**零 I/O、O(1)**；
  - **上线顺序**：load 完 → drain → 发布（§5 ★），否则 alloc 会把"盘上还是 1"的块
    判成已用 → 永久泄漏。
- 节点池：per-group 原子 LIFO（预建数组，cfg 可调，**默认 64**）。
  - `slice_lock` 内取/还（原子栈，零睡眠）→ 满足"锁内零分配"；
  - **池空兜底**：解锁 → 慢路径强制上线该 chunk → 重锁 → 直接清位。
    安全性：此时盘态仍显示已分配（free 未记账），alloc 不可能抢先拿走它 ✓；
    记录数因此**天然有界 = 池深**，"不上线的 chunk 挂一串记录无限涨"被结构性封死。
- **摘要降级（关键正确性）**：追加 record 覆盖到的 ctl，`summary` 一律置 MIXED。
  若不降级：chunk 盘态 ALL_USED（其实有待还空闲）被 alloc 扫描无载入跳过
  → **错误 ENOSPC**（不变量 §10.5）。

## 7. mount 顺序扫描（计数不驻留）+ 2bit 摘要

- CHUNK 模式 ctor：经 `chunk_io` **顺序读一遍全位图**（80MB ≈ 40–80ms，与 FULL 模式的
  mount I/O **同价**，只是不 memcpy 驻留），一趟产出：
  1. 每片 `free_count` → 02 §4 的 `free_count >= count` gate 保真；
  2. 组全局空闲计数 → `block_free_total`/statfs 保真；
  3. 每 ctl **2bit 摘要**（全 0 = ALL_FREE / 全 1 = ALL_USED / 否 MIXED）；
  4. **自描述三段校验**（02 §1：尾巴全 1 ×2 段 + root inode 亮灯）内联同趟做掉，
     失败返 `RC_ERR_STATE` 拒挂；`skip_self_check=true` 供单测合成位图。
- FULL 模式 ctor **零 I/O**（现行为不动）。
- 摘要语义与维护点：
  | 摘要 | alloc 扫描可否无载入跳过 | 维护 |
  |---|---|---|
  | ALL_USED | ✅ 跳过（无空闲） | evict 时从 buffer 重算；mount 扫描初始 |
  | ALL_FREE | ✅ 整段可判空闲 | 同上 |
  | MIXED | ❌ 须上线查 | record 追加必降级到 MIXED；evict 重算 |
  - 摘要是**运行期缓存**（内存 ctl 内），零盘格式改动；crash 后重建 = 重扫。
- 收益：ENOSPC 级全片扫描（`group_alloc` 全片失败轮询）先查摘要 → 不产生
  "逐 chunk 连锁载入全图"悬崖（首扫后摘要常驻，之后免 I/O）。

## 8. 片内算法的窗口化（改制层机械）

- 片内函数（`find_free_run`/`next_free_seg`/`alloc_hard`/`alloc_soft`/`soft_occupy`/
  `test_interval`/`free_interval`）**签名不动**，CHUNK 下有效窗口 =
  `[max(片首, chunk_lo), min(片尾, chunk_hi)]`，返回三态：
  - `HIT` —— 命中，收工；
  - `WINDOW_EMPTY` —— 本窗口无货（≠ 片完），路由迭代器推进到下一窗（走慢路径换片）；
  - `SLICE_EMPTY` —— 已扫完全片（含绕回）。
- 路由层窗口迭代器（CHUNK 专属外壳，FULL 直走旧码）：
  ```
  alloc:  从 hint 窗起，顺序向片尾推进；每窗 = 快/慢路径 + 单窗 find
          扫到片尾仍未中 → 绕回 [0, hint) 窗序列再来一遍（02 §4 语义不变）
  occupy: 锚点窗起，逐窗贪心前缀，顶到窗尾换下一窗（跨 chunk 续圈 = 原跨片续圈同构）
  test:   逐窗比对；ALL_USED/ALL_FREE 摘要命 val 直接跳过该窗（免载入）
  ```
- FULL 模式：`mode` 开关在入口分叉，旧码原样，逐位不变（红线）。

## 9. TTL 三态分派（替代 02 §5 的全量回写）

```
遍历 chunk 数组（不拿全片锁、不拿 chg_mu 逐片快进）：
  ONLINE && dirty  → chg_mu → 锁内快照+清 dirty → 锁外写 → chg_mu 放（§5 反例依据）
  OFFLINE && 链上有覆盖记录 → chg_mu → 读该 chunk → 应用记录 → 写回 → 摘节点
  其余             → 跳过
FULL 模式维持现全量行为（是否顺手加脏标 = 后置可选，不在本次范围）
```
- 对比现状：`80MB/30s 全量 + 全片锁` → `只写脏/有待还的 chunk`，锁阻塞面降为零
  （online 写盘期间仅同片换片等待，快路径分配不受扰）。
- crash 窗口与 M3.5 同构：写途中 dangling = 泄漏方向，feature 接受。

## 10. 不变量契约（编号给测试用）

1. FULL 模式行为与性能 = 现状逐位不变（A/B 基线红线）。
2. 任一时刻每片 `online_chunk ∈ {NONE, 唯一 c}`；buffer 内容 ≡ 该 c 盘态 ⊕ 未回写变更 ⊕ 已 drain record。
3. 盘 I/O 只在持该片 `chg_mu` 下发起；`slice_lock` 内零 I/O、零睡眠、零阻塞分配（快路径不碰 chg_mu）。
4. chunk 发布（online 可见）前，其覆盖范围的 record 链必已 drain 完（用户裁顺序）。
5. `summary == ALL_USED` ⇒ 该 chunk 盘态无可分配位且无 pending record（防**错误 ENOSPC**）。
6. `free_count`/全局计数在锁内与置/清位同步记账，与驻留与否无关（statfs/gate 保真）。
7. record 只被 drain 消费、不丢；crash 丢 record = 泄漏方向（fsck 收），无双分配方向。
8. tail == 0 或 ≥1 整页（§3 推论）；buffer、ctl 窗口、盘块全 4K 对齐。
9. 池空兜底走"强制上线直清"，不阻塞、不越序、不产生第二批记录（§6）。
10. preferred 起扫纪律继承（02 §5 + 2026-09-28 D3）；锁语义两边各保持现行为（god try / ko 阻塞）。

## 11. A/B 计划

- 三路线：**FULL / CHUNK（chunk_order 枚举 0..4：4K..64K）/ mmap 后端**
  （mmap = "内核代管 4K 懒加载"的现成免费对照，不叠用 CHUNK）。
- 指标：alloc/free p50/p99（hint 局部命中 vs 冷区随机混合）、ENOSPC 扫描耗时、
  mount 耗时、常驻 RSS、TTL 写放大（字节/30s）。
- 负载形态：贴近真实盘面的「顺序推进 + 少量冷区随机」混合；复用 tests 压测轮子。
- 判据（预设）：CHUNK 常态吞吐 ≥ FULL 的 **95%** 且 RSS ↓ ≥ **10×** → 选 CHUNK；
  否则 FULL 保底、CHUNK 数据留给 SparrowOS 目标机复测（内存收益在小内存机上才是主收益）。

## 12. ko 移植注记（先记档，本次不做）

- `chg_mu` → `mutex`（`ravenfs_block_*` 调用点均睡眠上下文；移植时复核无原子路径——
  iomap_begin/dentry/extent 均已持有可睡锁 ✓）。
- record 池 → `kmem_cache` 预建；`chunk_io` → `sb_bread`（`disk_block_rel` → FS block 号）。
- god try 换片 ↔ ko 阻塞：**两边各自保持现纪律**（2026-09-28 用户裁，勿"顺手统一"）。
- TTL 分派对应重写 `rfs_bm_group_sync`（现为无脏标全量）；mount 扫描对应
  `rfs_bm_group_load` 改造（不再全量 memcpy 进 vzalloc）。
- `mapping_set_folio_order_range` 等页缓存话题与本改制正交（不同层）。

## 13. 实施拆刀（施工顺序，每刀过测再进下一刀）

1. **① backing 开关 + cfg/io 契约 + FULL 分叉**：CHUNK 先走简化——
   "强制上线单 chunk、离线 free 兜底=上线直清"（暂无 record），跑通换片状态机。
2. **② record 池 + drain + 摘要降级**：补齐用户裁语义。
3. **③ mount 扫描（counts/summary/三段校验）+ TTL 三态分派**：收口运行时。
4. **④ A/B 压测接线**（§11 指标）。
- 每刀验收：`ravenfs_juv_bitmap_slice`、`ravenfs_juv_block_occupy`（FULL 红线，原样通过）
  + 新增 `ravenfs_juv_bitmap_slice_chunk`（合成盘 + mock chunk_io：换片/drain/池空兜底/
  摘要跳过/绕回/尾块/占位跨界）。

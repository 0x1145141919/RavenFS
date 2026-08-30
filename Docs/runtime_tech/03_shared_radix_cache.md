# RavenFS v1.1 运行时科技 ③ — 双缓存器共用的多分片静态基数树 + 动态位域切分

> 2026-08-30
> 主题：inode / mdata_piece 双缓存器在 self_reliat（自力更生）模式下共用的两套轮子——
> ① 动态位域切分（cut_high / decode / statey 策略表）② 多分片静态基数树
> （slice_radix_tree）；行生命周期 ctrl_word 单原子字状态机；读锁不跨会话纪律。
> 代码映射：`juvenile_god/inode_cache.cpp`（inode 版，RADIX 档 [19,52]）＋
> `juvenile_god/mdata_piece_cache.cpp`（mdata 版，RADIX 档 (13,52]）＋
> 声明在 `include/ravenfs/inode_cache_radix.hpp` / `mdata_piece_caches.hpp`。

---

## 0. 要解决的问题

self_reliat 缓存要面对超大容量：inode 数可达 2^19~2^52，mdata 负载数 2^14~2^52。
全量建控制器数组（CTRL_ARRAY）内存爆炸；普通哈希表要么动态扩容（并发复杂），
要么固定桶数（容量上限和内存浪费不可兼得）。

答案一句话：**索引整数按位域从高位往下动态切层（level_bits 由 statey 策略表按
容量档固化），最高位切出"分片选择"（多片并行），树内逐层落到"静态基数树"
（层数/表宽构造时定死、按需懒建、只增不删），叶表槽即缓存行控制器——
行生命周期由 ctrl_word 单原子字状态机（idle→io_ing→working→evicting）接管。**

---

## 1. 动态位域切分轮子（cut_high / decode / plan）

### 1.1 cut_high — 左对齐切割原语

```
raw 左对齐（位域最高位 = bit 63）→ 从顶端向下切 bits_to_cut 位:
    cut_out_section = 切下的高位段（右对齐到低位）
    main            = 剩余位域（保持左对齐，供递归继续从 bit 63 切）
```

### 1.2 decode — 按 plan 逐层切分

```
bitfield_decode_plan_t:               bitfield_decoded_result_t:
    bits_len      总索引位宽               chunks[0]              = mgr 层分片选择
    total_levels  使用层数                 chunks[1..total_levels-1] = 树内逐层索引
    level_bits[]  每层位宽                 chunks[total_levels]   = 行内索引

raw 左对齐 → 逐层 cut_high → 剩余低位 = 行内索引（row_bits = bits_len − Σlevel_bits 派生）
```

**plan 是唯一真相源**：decode 不依赖 mgr 任何状态，纯函数；row_bits 由 plan
累积派生，不做独立常量（25/33 档 row_bits=5 行 = 32 entries = 8KB = 2 块，
就是派生出来的，禁止用别处的 ROW_BITS=4 常量）。

### 1.3 statey 策略表 — "动态"的真相

每档 bits_len 固化一层 level_bits 数组（Sparrow 亲笔调过的平衡点）：

```
inode 版:  statey_19={8,7} 2层 ... statey_26={7,9,6} 3层 ...
           statey_34={7,9,9,5} 4层 ... statey_45={7,9,9,9,7} 5层 ... statey_52={9,10,11,11,7}
mdata 版:  mdp_statey_14={7,7} ... mdp_statey_21={8,6,7} 3层 ...
           mdp_statey_44={8,7,7,7,7,8} 6层 ... mdp_statey_52={10,8,8,8,9,9}
```

**为什么分层不固定**：层数 ↔ 表宽互为代价。层数多 → 每层表窄 → 内存稀疏省，
但寻址深；层数少 → 表宽大 → 内存涨。策略表按容量档位调好"哪一层多宽"，
把内存峰值和寻址深度同时压住。mdata 版是 row_bits=0 特化：`Σlevel_bits = bits`，
无行内索引位（叶表槽 = 负载控制器，行 = 1 负载恒 8K）。

---

## 2. 多分片：level_bits[0] 即并行度

```
n_slices_ = 2^level_bits[0]        // mgr 层分片选择位宽 → 片数
slices_ = 每片 placement new 一棵 slice_radix_tree（ctor 即建根表）
寻址: chunks[0] 选片 → 片内树逐层 → 叶表 ctrl_t → 行内索引
```

与位图分片（文档 ②）同哲学：**片与片零共享，锁粒度 = 片**。但切法不同——
位图是地址区间线性切，缓存是位域高位切（索引哈希式分布）→ 同一 inode 号的
并发访问自然散列到不同片，热点天然打散。

---

## 3. 静态基数树（slice_radix_tree）

```
tree_levels_ = plan.total_levels − 1        // level_bits[0] 是 mgr 层，不进树

中间层表 = 指针槽数组（8B/槽，nullptr = 未建）
叶子层表 = Inode_cache_ctrl_t 数组（槽即控制器实体）
表类型由层深决定（无运行时标记）

根表 ctor 建好（tree_levels_==1 → 根即叶表，19~25 档全属此类）
中间/叶表按需懒建（ensure_path，写锁内，无 I/O）
树只增不删：表结构建好即永久（直到 dtor）
```

**为什么"静态"**：层数和每层表宽构造时由 plan 定死，不是动态插入的二叉
搜索树——零旋转零重平衡，寻址就是"数组下标套数组下标"。

### 表内存代价（写锁内累计，供审计）

```
alloc_leaf:     叶表 = 2^level_bits[last] × sizeof(ctrl_t)，ctrl 全 idle
alloc_internal: 中间表 = 2^level_bits[1+level] × 8B，槽清零
table_size_all_accumulate 原子累计 —— 缓存实际驻留内存的审计口
```

---

## 4. ctrl_word 单原子字状态机（行生命周期）

### 4.1 为什么打包成一个原子字

```
[0,8)   state    —— idle / io_ing / working / evicting（CAS 状态机）
[8,24)  flags    —— DIRTY
[24,56) accessor —— 会话占坑计数（32 位）
```

get 的"占坑（accessor+1）"与 reap 的"接管（要求 working && accessor==0）"
必须落在同一原子操作上，否则存在 TOCTOU：旧两字段方案 get 复核通过后返回
指针、reap 才 CAS 成功 → 页被 free → 已返回的指针悬垂。打包后：
**占坑 = CAS(word, word+INC)（成功即占坑，无需复核）；接管 = CAS(word, evicting)
（前提含 working && accessor==0）——两个窗口同时关闭。**

### 4.2 ctrl_acquire — 懒加载状态机（CTRL_ARRAY / RADIX 共用）

```
loop:
  working → CAS 占坑（accessor+1，同时刷 accss_stamp）→ 返回 cache_data
  idle    → CAS io_ing（拿加载权，独占）→ alloc + read_block（I/O 不持锁）
            → cache_data 发布（release）→ working（acquire 观察方可见）→ 回顶部占坑
  io_ing / evicting → cpu_relax 忙等（他人加载中 / 淘汰接管中）
OOM/读失败 → 回 idle（release），返回 nullptr（上层走错误路径）
```

### 4.3 ctrl_reap_one — 单行 TTL 淘汰（reap 线程周期调）

```
前提（原子打包）: working && accessor==0 && now − accss_stamp > ttl
CAS → evicting（接管成功）→ 脏则写回（无锁 I/O）→ free 页 → 回 idle
返回 true → 调用方记账（real_cache_accumulate 减）
```

### 4.4 ctrl_flush_one — 同步点（flush_all，挂载/卸载）

```
无 TTL 前提；脏行写回后驻留（清 DIRTY），非脏行丢弃回 idle
（卸载路径 dtor 统一释放写回后的残留行）
```

---

## 5. 读锁不跨会话（2026-08-30 血泪修复）

```
旧设计: get 持 shared_mutex 读锁 → put 才释放
  → 业务嵌套 InodeSession（victim+new、位图多负载片）时:
    内层 miss 建链要写锁（ensure_path），外层读锁未还 → 单线程自死锁（futex 实锤）

新设计: 读锁只在 get 内部建链窗口
  → 树只增不删（表建好即永久）→ 会话内 locate/mark_dirty 无锁安全
  → 页生命周期由 ctrl_word accessor 计数保护（与树锁无关）
  → 屏障职责从读锁移交 accessor；嵌套建链不再自死锁
```

**这是双缓存共用的同一条纪律**：inode 版先踩（inode_cache.cpp），mdata 版
实装时同款 bug 复现后对齐修复（2026-08-30 提交"读锁不跨会话"）。

---

## 6. 双缓存共用同一轮子的差异点

| | inode 版 | mdata 版 |
|---|---|---|
| 索引 | inum | piece_idx |
| RADIX 档 | [19,52]（≤18 走 FULL_RAM/CTRL_ARRAY） | (13,52]（≤13 走 CTRL_ARRAY） |
| 行 | 2^row_bits inode（25/33 档 = 32 = 8KB = 2 块） | 恒 1 负载 = 8192B（blk_len 恒 2） |
| 末行 | 收窄：row_blk_len = ceil(有效 inode / 16) ≥1 | 无收窄：blkidx_valid 已保证负载完整 |
| 行内索引 | chunks[total_levels]（row_bits ≥4） | 恒 0（row_bits=0，叶表槽即控制器） |
| blk 计算 | base + ((inum>>row_bits) << (row_bits−4)) | base + piece_idx×2（偶数对齐契约） |
| 策略表 | statey_XX | mdp_statey_XX（前缀区分，防跨 TU 重定义） |

**mdata 的 B 路线否决记录**（设计决策，防止后人重走）：行 = 2 负载 →
一行涉及多个控制器 → 淘汰/写回可能同时锁多行 → 死锁风险不可忍。
A 路线（行 = 1 负载）控制器与缓存行一一对应，锁粒度最小。

---

## 7. 不变量契约（踩坑总结）

1. **plan 是唯一真相源**：decode 纯函数，不依赖 mgr 状态；row_bits 由 plan
   派生，禁引外部常量（ROW_BITS=4 仅 FULL_RAM/CTRL_ARRAY 可用）
2. **树只增不删**：表结构建好即永久；ensure_path 只在写锁内、且无 I/O
3. **读锁不跨会话**：get 内建链窗口持锁，返回即释；页生命周期由 accessor 管
4. **状态机原子打包**：占坑/接管互斥在同一 CAS 上，无 TOCTOU 窗口
5. **I/O 不持锁**：io_ing 加载阶段无锁（他人 cpu_relax 忙等），锁内只有内存操作
6. **行 I/O 恒 = 有效数据**：inode 末行收窄不越出 array（不碰 bitmap 首块）；
   mdata 负载完整（blkidx_valid 前置阻断）
7. **OOM 原子回退**：加载失败回 idle（不残留 io_ing 卡死），上层收 nullptr
8. **淘汰线程纪律**：宿主须先停 reap 再析构（dtor 与 reap 不并发）
9. **两缓存策略表独立命名**（statey_XX vs mdp_statey_XX），跨 TU 链接防重定义

---

## 8. 移植注记

| 接地层 | host（现状） | SparrowOS kmod | Linux ko |
|--------|-------------|----------------|----------|
| 分配器 | host_alloc/mdp_alloc（aligned_alloc 4K/8K） | 内核分配器 | **不进 Linux 端**（缓存全用内核轮子） |
| 树锁 | std::shared_mutex | 内核 rwsem | — |
| TTL 时间戳 | rdtsc_timestamp（Invariant TSC） | arch 计数器 | — |
| 淘汰 | 专用线程 reap_expired | 同（或内核 shrinker） | — |

**定位重申**：这套 self_reliat 轮子是 SparrowOS 的嫁妆（无内核页缓存的端
才需要自力更生缓存）；Linux 端缓存层由 VFS/页缓存接管（文档 ① §8），
但这套**位域切分 + 静态基数树 + ctrl_word 状态机**的设计思路在 Linux 端
仍有价值——如果你将来做自己的元数据缓冲，直接照抄这个骨架。

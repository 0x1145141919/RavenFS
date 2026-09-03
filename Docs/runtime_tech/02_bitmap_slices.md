# RavenFS v1.1 运行时科技 ② — 双位图全量进内存的运行时分片思想

> 2026-08-30
> 主题：block / inode 双位图 mount 全量进内存；按 CPU 数切分成自治片；
> 片内字粒度位操作 + hint 游标；顶层仿 FPA 亲和路由（分配 try / 释放自旋）。
> 代码映射：`juvenile_god/bitmap_slices.cpp`（slice 自治单元 + mgr 几何推导/路由）＋
> `include/ravenfs/bitmap_slices.hpp`（模块契约：类型顶出 + bitmap_slices_mgr_t）＋
> `juvenile_god/ravenfs_juvenile.cpp`（bitmaps_load/flush、位图原始内存、薄转发壳）。
> 2026-09-03 修订：位图模块从上帝类顶出（对齐 inode_cache/mdata_piece 的 cfg 大结构体
> 范式；原 bitmap_slice.cpp + 上帝类内嵌类型退役），本文明中的"顶层路由"现为
> bitmap_slices_mgr_t 成员，上帝类只留签名不变的转发。

---

## 0. 要解决的问题

位图是"元数据的元数据"——每次分配/释放都要碰它，多核并发下它是天然的争抢热点。
朴素做法（整图一把锁）在 SMP 下就是串行瓶颈；按地址段预分锁又引入跨段拼接复杂度。

答案一句话：**位图全量进内存（分配=纯内存操作），按 CPU 数切成 N 个自治片，
片内自管（锁 + 游标 + free_count），顶层 preferred 亲和路由。片与片零共享 → 
并行度 = 片数，无跨片锁。**

---

## 1. 全量进内存（bitmaps_load）

```
直写模式（O_DIRECT）:
    block_bits = posix_memalign(4096, bitmap_blocks * 4096)   // 4K 对齐
    inode_bits = posix_memalign(4096, inode_bm_blocks * 4096)
    pread 全量（fblock_to_byte(bitmap_start) 起 / inode_bm_base 起）

mmap 后端:
    block_bits = fsblk0_base_ + bitmap_start * 4096            // 窗口切片
    inode_bits = fsblk0_base_ + inode_bm_base * 4096
    不预取（内核按需调页），不 free（munmap 统一负责）
```

**为什么全量进内存**：位图访问频率最高，全量驻留后 alloc/free 是纯内存操作，
无 I/O 路径；幼年期接受"掉电丢位图更新"（析构才写回，不追求事务安全），
mmap 后端由内核页缓存兜底。

### 自描述校验（两模式共用，四连验的第四验）

```
[bitmap_start, total_blocks)        全 1   ← block 位图区自身（含 backup 链等元数据）
[inode_bm_base, bitmap_start)       全 1   ← inode 元数据区
inode_bits[0] & 1                   必 1   ← root inode 亮灯
```

校验通过后填 descriptor：`bit i ↔ FS_block[i] / inode[i]`（FS_blk_base=0），
位序与磁盘原样映射（不反转、不重排）——这是"分片切片"能直接按地址切的前提。

---

## 2. 分片参数推导（slices_setup）

```
n = 2^ceil(log2(cpu_count))                      // 2^n 上界(cpu)
cap = min(blk_nbits, ino_nbits) / 32768          // 满片数约束：每片 ≥ 1 位图块
n = min(n, cap)，n ≥ 1

末片非空迭代:
    per = ceil(nbits / n) 向上取整到 32768 倍数
    while ((n-1) * per >= nbits) n >>= 1;         // 末片空 → 减半，保持 2 幂
```

**32768 的来历**：32768 bit = 4096 B = 1 个 FS_block。位图区以块为原子单位，
一片至少管一个位图块 → 片起点天然 2^15 对齐（对齐语义见 §4）。

约束链：`slice_count` 是 block 组和 inode 组**共用的**（两个 slices 数组同长度），
所以取两者满片数的最小值。每片 `slice_base` 必须 4K 对齐（`begin/8` 是 4096
倍数，enable_slice 里校验）。inode 组片 0 的 hint 预置 1（跳过 root inode）。

---

## 3. bitmap_slice_t —— 分片自治单元

```cpp
class bitmap_slice_t {
    uint8_t  *slice_base;          // 片位图基址（必须 4K 对齐）
    uint64_t  base_idx;            // 片首的全局位号（= 相对 parent 位图字节偏移 ×8）
    uint64_t  slice_bitscount;     // 片位数（非末片 32768 倍数，末片可短）
    fs_bitmap_descriptor_t *parent_descriptor;  // 全局位图描述符
    uint64_t  free_count;          // 片内空闲计数（enable 时 O(片) 扫出，增量维护）
    uint64_t  last_finish_hint;    // 分配游标（片内偏移，分配连续性来源）
    rw_try_spinlock_t slice_lock;  // 片锁（读共享/写独占，全 try）
};
```

**自治纪律**：

1. **片与片零共享**：无跨片状态、无跨片锁 → 并行度 = 片数
2. **本文件所有函数一律不加锁**：临界区由外部用 `slice_lock` 编排
   （try_lock_write → 操作 → unlock_write），片内函数是纯位操作
3. **刻意不搞构造/析构函数**：往 64B 缓存行对齐设计（内存规整，数组化分配）
4. **坐标双视角**：对外全部用全局位号（parent_descriptor 视角），内部换算
   `片内偏移 = 全局位号 − base_idx`；alloc 输出 `out_base_idx = base_idx + hit`
   直接是全局位号，调用方零换算

---

## 4. 片内分配算法（字粒度位操作）

### 基础轮子（匿名 namespace，纯函数）

```
next_free_seg(words, nbits, from, &seg_s, &seg_e)
    —— 找从 from 起的下一个空闲段 [start, end)
       字粒度：整字占用直接跳过（~word==0），__builtin_ctzll 快速定位
       段尾延伸：free_mask 找第一个占用位（ctzll(~free_mask)），
       到字边界且整字空闲续下一字

find_free_run(words, nbits, from, count, align)
    —— 从 from 起找 count 连续空闲位、起始位 2^align 对齐（片内偏移）
       段粒度：段内取对齐候选，段不足直接跳过段尾继续
       复杂度：O(段数 + count)，避免 O(n×count) 逐位扫
```

### alloc_hard —— 硬分配（连续 + 对齐，不部分分配）

```
从 last_finish_hint 起扫 → 段尾绕回 [0, hint) 再扫一次
命中: 置位 count 位 → free_count -= count → hint = hit + count → 输出全局位号
失败: RC_ERR_NO_BLOCK（片内无满足段）
```

### alloc_soft —— 软分配（有多少给多少）

```
第一个空闲段取对齐候选，实际 got = min(段长, count)
缺口 bits_left_to_be_allocated 回传（调用层可据此再调一次补足）
```

### free_interval / test_interval

```
free_interval: 按实际清掉的 1→0 位计数（重复释放不虚高 free_count）
test_interval: 区间 [base, base+len) 全同 val → RC_OK / 有异位 → RC_ERR_NOT_FOUND
               （自描述校验、extent 区间认领预检用）
```

**hint 游标为什么重要**：分配从上次结束处继续 → 空间局部性（邻近分配落邻近块，
extent 合并友好）；绕回保证不会因碎片"前半段空着却报满"。

---

## 5. 顶层路由（仿 FPA 亲和分配）

```
preferred_slice（block_preferred_slice / inode_preferred_slice）
    —— 亲和片：分配成功回写片号，下次从它开始（cache 亲和 + 片内连续性）

分配（block_alloc_hard/soft、inode_alloc）:
    for round in 0..slice_count:
        idx = (preferred + round) % slice_count
        if !slice_lock.try_lock_write(): continue     ← 锁忙换片，不阻塞
        rc = slice.alloc_*(...)
        unlock_write()
        if rc == RC_OK: preferred = idx; return out
    全部片失败 → PTR_INVALID（硬分配原子：不部分分配）

释放（block_free / inode_free）:
    idx = blkbase / 名义片长（block_per_slice_bits / inode_per_slice_bits）
    while !try_lock_write(): ;      ← 释放不可丢，自旋等
    free_interval(...)
    unlock_write()
```

**为什么分配 try / 释放自旋**：分配是可放弃的操作（这片忙就换一片，反正
片与片零共享）；释放是不可放弃的（必须落到正确的片，否则泄漏）——语义
不对称直接映射到两种锁获取策略。`inode_alloc` = `alloc_soft(0, 1)` 的单槽
特化（片 0 hint 从 1 起扫，root 永不分配）。

---

## 6. 锁与并发模型

```
rw_try_spinlock_t：读共享 / 写独占，全 try 语义（拿不到立即 false，不阻塞自旋）
   —— 高 1 位 = 写锁，低 31 位 = 读者计数，std::atomic CAS 实现
无公平性保证：饥饿由调用方路由策略承担（preferred 旋转 = 轮询式公平）
```

位图临界区是短临界（字粒度位操作，几十条指令），自旋锁足够；try 语义让
"锁忙"从阻塞变成"换片"，是分片并行度的兑现方式。

---

## 7. 设计溯源：FPA BCB 对齐

用户态这套锁轮子（`util/lock.h`）就是从 Sparrow kernel 移植的（spinlock_cpp_t
等，剥掉了中断关闭/重入部分）；分片 + preferred 亲和 + try 路由直接对齐
FPA（Fast Physical Allocation）的 BCB（bitmap cache block）语义——分配器
设计不是新发明，是把内核验证过的分配器思想搬到用户态上帝类，再为
SparrowOS 预留回去的路。

---

## 8. 不变量契约（踩坑总结）

1. **全量进内存，分配/释放只改内存态**；析构（直写模式）pwrite 全量写回；
   mmap 后端由内核页缓存管，不显式写回
2. **片与片零共享**：无跨片状态、无跨片锁；片内函数不加锁（锁由外部编排）
3. **硬分配原子性**：单片内完成（不跨片拼接）、不部分分配；全片失败
   PTR_INVALID（0 是合法块号，哨兵必须用 PTR_INVALID）
4. **软分配一次一片**：缺口回传，调用层决定是否补足
5. **释放定位**：`idx = base / 名义片长`（片长固定，释放不扫片）
6. **slice_base 4K 对齐**（enable_slice 校验，违反 RC_ERR_STATE）
7. **root inode 保留**：片 0 hint 从 1 起；inode 分配从 1 起扫
8. **自描述校验是挂载门槛**：尾巴三段全 1 + root 亮灯，缺一不可
9. **位序与磁盘原样映射**（不反转不重排）——切片直接按地址切的前提

---

## 9. 移植注记

| 端 | 位图落地 | 锁 | 备注 |
|----|---------|-----|------|
| host（现状） | posix_memalign 全量 / mmap 切片 | rw_try_spinlock_t（atomic） | 上帝类主场 |
| SparrowOS kmod | 自有分配器（kmalloc 对应物） | arch 原子原语（xchg/cmpxchg） | **分片思想的直接继承者** |
| Linux ko | kmalloc/kvzalloc 全量进内存 | spinlock_t（位图临界短，自旋够用） | 分片思想照搬；释放可睡版本用 mutex |

`rw_try_spinlock_t` 接口语义不变（try_lock_read/write + unlock），换端只换
底层实现；分片参数推导（2^n 上界 + 满片数约束 + 末片非空）与片内算法
（next_free_seg/find_free_run/hint 绕回）纯算法，零平台依赖。

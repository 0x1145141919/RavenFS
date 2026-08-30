# RavenFS v1.1 运行时科技 ① — 挂载参数契约：上帝类工作模式的唯一真相源

> 2026-08-30
> 主题：挂载参数 → 上帝类全机工作模式（两层决策树）；大结构体无状态初始化子模块；
> 下游双缓存（inode / mdata_piece）三工作模式（no_cache / self_reliat / offload）。
> 代码映射：`include/ravenfs/ravenfs_v1.1.hpp`（契约）＋ `juvenile_god/ravenfs_juvenile.cpp`（编排）。

---

## 0. 本文要回答的问题

上帝类（`RavenFS_juvenile`）一个构造即挂载，内部却没有"模式枚举"散落各处——
工作模式是怎么定的？为什么缓存子模块的初始化用大结构体整体传入而不是逐参？
下游两个缓存（inode / mdata_piece）的"三个工作模式"到底是什么、各自管什么？

答案一句话：**挂载参数结构体是唯一真相源，构造时走两层决策树把参数翻译成
`God_CTRL_WORD`（控制字）+ 两个子模块 cfg（大结构体），子模块拿到 cfg 后
自包含初始化、自管生命周期，宿主不再隐式依赖任何内部状态。**

---

## 1. 挂载参数：三个结构体，无状态消费

```
linux_userspace_mount_t           ← 顶层挂载参数束（构造唯一入参）
├── dev: ravenfs_mount_device_t   ← 设备描述
│   ├── fd                  ← 外部打开并负责关闭；类只借不关
│   ├── lba_size_log2       ← 设备逻辑块大小 log2（512e→9，4Kn→12）
│   ├── type                ← DEV_LOOP_IMG / DEV_NVME / DEV_SATA_* / DEV_UNKNOWN
│   └── is_device_direct    ← 外部是否以 O_DIRECT 打开（决策树第一层的关键位）
├── param: ravenfs_mount_param_t  ← 行为开关
│   ├── if_will_use_inode_cache
│   ├── if_will_use_mdata_piece_cache
│   └── is_readonly          ← true 必须配 is_device_direct=false（rfs-audit 专用）
└── logical_processor_count  ← 位图分片数推导用（真机拿真实值，测试可填任意数）
```

**无状态消费纪律**（构造体顶部注释，2026-08-21 起）：

- 类不做设备几何的内部推导与校验——设备探测是外部职责
- 全部参数来自调用者结构体；类不维护"上次挂载的残留状态"
- 失败时 `*res_code` 给出 RC_ERR_*，对象处于未挂载态（fd=-1）
- fd 生命周期归外部：析构只解自己的资源，不关 fd

> 为什么这么设计：上帝类探索期要能被测试程序任意摆布（loop 镜像 / 真盘 /
> O_DIRECT / mmap / readonly），参数束让"同一份代码、任意组合"成为可能，
> 也把设备探测的脏活（几何、对齐、类型）全部留在外部。

---

## 2. 决策树第一层：I/O 后端（is_device_direct）

```
is_device_direct == true  → 直写模式
is_device_direct == false → mmap 后端（整盘从 offset 0 映射）
```

### 直写模式

- 块 I/O 走 pread/pwrite，O_DIRECT 契约：长度/缓冲须对齐逻辑块（4K）
- 256B inode 直读/直写非法 → `inode_read/write` 整块 4K 读-改-写
- 位图全量 pread 入内存；析构 pwrite 全量写回

### mmap 后端

- 整盘从 offset 0 mmap（含 Primary SB 区 byte[0,4096)）
- readonly 配 `MAP_PRIVATE + PROT_READ`（写不落盘）；否则 `MAP_SHARED` 读写
- `fsblk0_base_ = mmap_base_ + 4096` → FS_block[n] 虚拟地址 = `fsblk0_base_ + n×4096`
- **所有切片（位图 ×2、两个缓存 canvas）一律从 fsblk0_base_ 派生，禁止散写**
- 不刻意预取：内核按需调页

> 第一层的本质：**决定"数据住哪"**。直写模式数据在用户态内存（要自己缓存）；
> mmap 模式数据在内核页缓存（用户态零拷贝直映，缓存子模块整体 offload）。

---

## 3. 决策树第二层：缓存子模块模式（caches_setup）

```
第一层已定（mmap / 直写）↓

mmap 后端（非直写）:
    inode 缓存     → offload_cache   canvas = inode array 起始 FS_block 的窗口切片
    mdata_piece 缓存 → offload_cache   canvas = FS_block[0] 的窗口切片（数据区 base=0）
    数据不落用户态，全由内核页缓存管

直写模式:
    inode 缓存     → param.if_will_use_inode_cache ? self_reliat : no_cache
    mdata_piece 缓存 → param.if_will_use_mdata_piece_cache ? self_reliat : no_cache
```

第二层的本质：**决定"谁缓存"**。mmap 后端 = 内核页缓存代劳（offload）；
直写模式要么用户态自力更生（self_reliat，SparrowOS 嫁妆），要么完全不缓存
（no_cache，session 自管缓冲直读盘）。

### God_CTRL_WORD —— 模式位域的压缩表示

```cpp
namespace ravenfs_juvenile_god_ctrl_words_bits {
    constexpr uint64_t READ_ONLY_MASK          = 1;  // 由 read_only_bit 控制
    constexpr uint64_t MMAP_BACKEND_CONTROL_MASK = 2;  // 由 is_device_direct 取反无条件设置
    constexpr uint64_t BLOCK_CACHE_ENABLE_MASK = 4;  // mmap 后端强制翻 1；直写模式=手动用户态缓存开关
    constexpr uint64_t INODE_CACHE_ENABLE_MASK = 8;  // 同上
};
```

控制字是两层决策树结果的位域压缩（`get_ctrl_word()` 对外暴露），供测试/审计
快速判断"当前实例跑在什么模式组合上"，不参与运行时分派（分派走子模块的
`backed_mode`，见 §5）。

---

## 4. 去隐状态依赖：大结构体初始化子模块

### 4.1 为什么不用逐参

子模块（`Inode_cache_mgr_t` / `mdata_piece_caches_mgr`）的构造参数分四类：

| 类别 | 内容 |
|------|------|
| 模式 | `mode`（三选一，**显式指定，不再由 bits 隐式推导**） |
| I/O 回调 | `read_line/write_line`（或 read_block/write_block）+ `io_ctx` |
| 几何 | `Inode_count` / `Inode_base_FS_block_idx`（或 `Piece_count` / `Piece_base_FS_block_idx`） |
| per-mode 特化 | self_reliat 的 `ttl_bare_seconds`（0=默认 2s）/ offload 的 `virtual_canvas_base` |

逐参传递的毛病：参数顺序易错、per-mode 特化字段互相污染、新增模式要改签名、
宿主和子模块的隐式耦合（子模块摸宿主内部状态推导几何）。

**大结构体方案**：cfg 整体传入，子模块自己从 cfg 派生内部快照（`inode_cache_rt_t`），
构造后 `rc_out` 收结果码；失败路径由**宿主**负责 delete 已建子模块（caches_setup
里的清理链）。

### 4.2 回调适配器（宿主私有 I/O → 子模块函数指针）

```cpp
// ravenfs_juvenile.cpp —— 适配器只做一件事：把私有 read_buff/write_buff 包成函数指针
static result_code_t fs_io_read(void *io_ctx, uint64_t blk, uint64_t blk_len, void *buf) {
    return static_cast<RavenFS_juvenile *>(io_ctx)->read_buff(blk, blk_len, buf);
}
static result_code_t fs_io_write(void *io_ctx, uint64_t blk, uint64_t blk_len, const void *buf) {
    return static_cast<RavenFS_juvenile *>(io_ctx)->write_buff(blk, blk_len, buf);
}
```

子模块不摸宿主、不 include 上帝类头——只知道"给我 blk 我就读"。
**这是为 SparrowOS / Linux 移植预留的缝**：换端只改这两个适配器的实现
（内核 bio / NVMe 函数指针），子模块算法零改动。

### 4.3 派生快照（inode_cache_rt_t）：消灭共享指针悬垂

- `Inode_cache_cfg_t_v2` 的 I/O 回调 + io_ctx + 几何 → 派生成 `inode_cache_rt_t` 值快照
- RADIX 分片树**每片持一份值拷贝（40B）**，无共享指针
- mgr 自身持一份 `rt_` 供 CTRL_ARRAY 分支与原语调用
- 为什么：分片是自治单元（各片独立锁/独立树），若共享同一份快照指针，
  分片生命周期与 mgr 生命周期耦合，拆片/重建时会悬垂；值拷贝让分片完全自治

### 4.4 分配器适配

self_reliat 内部统一走 libc（`host_alloc`/`host_free`，cpp 顶部适配器）。
cfg 里**没有 alloc/free 字段**——移植（sparrowos/linux 内核）时改
`inode_cache.cpp` / `mdata_piece_cache.cpp` 顶部的两个适配器即可。

---

## 5. 下游双缓存的三个工作模式（cache_work_mode_t）

```cpp
enum class cache_work_mode_t {
    no_cache,          // 不缓存：session 自管缓冲直读盘
    self_reliat_cache, // 自力更生用户态缓存（SparrowOS 嫁妆）
    offload_cache      // 背后有 mmap 内核页缓存直映
};
```

| 模式 | 数据住哪 | 生命周期谁管 | 会话（session）行为 | 移植去向 |
|------|---------|-------------|--------------------|---------|
| `no_cache` | session 自管 8K 对齐缓冲 | session 构造/析构 | 每次直读盘，无缓存语义 | 内核端等价"裸 I/O" |
| `self_reliat_cache` | 用户态私有缓存页 | mgr（占坑 token + TTL 淘汰） | 占坑（accessor++）→ 用 → 还坑 | **SparrowOS kmod** |
| `offload_cache` | 内核页缓存（mmap canvas） | 内核（用户态零拷贝直映） | 无 token/accessor，直映指针 | Linux 端天然形态 |

**会话（InodeSession / mdata_piece_session）第一件事是拿 `backed_mode`**，然后
三路分派；对业务代码（dentry_tree / root_dir / name_heap）来说三种模式表面一致
（都是 get → 用指针 → put），这是"模式切换不动业务层"的关键。

### self_reliat 内部再分层（仅 inode 版）

inode 缓存 self_reliat 模式内部按 `bits_len = ceil(log2(inode_count))` 再推导：

```
[1,12]   CACHE_MODE_FULL_RAM    全量上内存：数据常驻，构造即载入
[13,18]  CACHE_MODE_CTRL_ARRAY  全量控制器数组：页按需加载，加载后常驻不淘汰
[19,52]  CACHE_MODE_RADIX_SHARD 静态基数树分片：控制器按需建，有容量上限要淘汰
```

mdata_piece 版同构双档：`CTRL_ARRAY ≤13 / RADIX >13`（详见文档 ③）。

### flush_all —— 模式自适应的同步点

```
no_cache      ：无缓存可同步，丢弃（RC_OK）
self_reliat   ：丢弃非脏页 + 脏页写回（写回后驻留，卸载由 dtor 统一释放）
offload_cache ：msync 整片 canvas 回写内核页缓存
```

---

## 6. 构造时序（构造即挂载）

```
ctor(linux_userspace_mount_t, res_code)
│
├─ 1. 无状态消费：拷贝 dev/param 全部字段（含 is_device_direct 快照）
├─ 2. 契约校验：readonly && is_device_direct → RC_ERR_STATE
├─ 3. 读 Primary SB（4K 对齐缓冲 pread）→ 四连验
│     magic → version → crc32(Castagnoli, 置零后全结构计算) → bitmap 自描述
├─ 4. 几何推导（block_model §3.2 尾巴三段链式，与 mkfs calc_geometry 同源）
├─ 5. 决策树第一层：mmap 整盘映射（或直写模式不动作）
├─ 6. bitmaps_load()：直写=全量 pread 入内存 / mmap=窗口切片不预取
├─ 7. slices_setup(logical_processor_count)：位图分片（文档 ②）
├─ 8. caches_setup()：决策树第二层 → 两个子模块 cfg → new 子模块
├─ 9. root inode 校验：type==DIR && data_root!=PTR_INVALID → root_dentry_blk
└─ 10. *res_code = RC_OK

失败路径统一 fail lambda（幂等清理，任意步失败都能安全退出）：
    caches_teardown() → slices_teardown() → munmap / free 位图 → fd=-1
```

### 析构即卸载（顺序与构造严格镜像）

```
~RavenFS_juvenile()
├─ 1. caches_teardown()：flush_all（同步点）→ delete 子模块 → 指针置空
├─ 2. bitmaps_flush()：仅直写模式 pwrite 全量写回（mmap 由内核页缓存管）
├─ 3. slices_teardown()：delete[] 两个 slices 数组
├─ 4. 释放后端：munmap / free 位图
└─ 5. fd 归外部，不关
```

readonly 契约：**不 flush**（mmap MAP_PRIVATE/PROT_READ 下写回本就禁止，
msync 无意义）——rfs-audit 静态审核专用路径。

---

## 7. 不变量契约（踩坑总结）

1. **模式只能由挂载参数决定**：子模块 `backed_mode` 是 const，构造时写死，
   运行期不可变。模式切换 = 重新挂载，不存在热切换。
2. **子模块不摸宿主**：通过函数指针 + io_ctx 通信；宿主类型对子模块不可见。
3. **决策树两层顺序固定**：先 I/O 后端（mmap/直写），再缓存模式
   （offload/self_reliat/no_cache）。第二层是第二层的唯一输入，没有交叉。
4. **所有 mmap 切片从 fsblk0_base_ 派生**，禁止散写地址——否则跨后端
   换挂载方式时地址语义就烂了。
5. **self_reliat 的脏数据必须走缓存读写**：`inode_read/inode_write` 在
   self_reliat 后端统一走缓存（2026-08-30 真 bug：分裂建新根 data_root 只落盘
   不落缓存 → 删除 walk 旧根 RC_ERR_STATE）。offload/no_cache 无此问题。
6. **卸载顺序 = 构造逆序**：先同步（flush_all）再拆，保证脏数据在资源
   释放前落盘；readonly 例外（不写回）。

---

## 8. 移植注记

| 端 | I/O 回调适配器 | 缓存策略 | 备注 |
|----|--------------|---------|------|
| host（现状） | pread/pwrite 或 mmap | 三模式全 | 上帝类，探索期主场 |
| SparrowOS kmod | NVMe 驱动函数指针 | **self_reliat 移植**（无内核页缓存，正是这套轮子的归宿） | TSC 换 arch 原子原语 |
| Linux ko | bio/sb_bread | 全 offload 思路（内核 VFS/页缓存接管） | self_reliat 不进场 |

分配器适配器（`host_alloc/host_free`）与锁原语（`rw_try_spinlock_t` →
arch 原子）是移植时唯二需要动的"接地层"，算法层零改动。

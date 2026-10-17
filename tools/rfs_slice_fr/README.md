# rfs_slice_fr — 分片空闲区间（per-slice free-range）trace dump

只读小工具。按**上帝类 / ko 通用的分片算法**把 v2.0 block 位图切成 `slice_count`
个分片，**逐片**枚举该片内的极大连续空闲区间（`bit=0` 的 run，已按片边界夹断），
以**结构化 trace** 输出到 stdout，供外部重定向与事后挖掘。

## 为什么需要它

分片算法**不落盘**：盘上只有整张 block 位图，分片是挂载时按 CPU 数重建的运行时结构。
但它**持久地影响**每一次分配（片内 best-fit、跨片仅靠 preferred 轮转、释放按
`base / per_slice_bits` 定位到唯一片）。整张位图或整盘 free-run 都看不出「分片后每片
长什么样」——本工具补的就是这一层，把「分片视图」变成可 diff / 可回归 / 可结构化分析的日志。

## 契约依据（与 god / ko 逐式一致）

- **分片数** n（`juvenile_god/fs_allocator_router.cpp:166` `fs_allocator_t` ctor
  ≡ `linux/ravenfs_allocator_router.c:196` `rfs_allocator_init`；v2.0 仅 block 组参与）：
  1. `n = 1; while (n < cpu && n < 2^30) n <<= 1;`
  2. `blk_cap = nbits / msb; cap = blk_cap ?: 1; if (n > cap) n = cap;`
  3. 末片非空迭代：`per_b = ceil(ceil(nbits/n)/msb)*msb`；若 `(n-1)*per_b < nbits` 停，否则 `n >>= 1`。
- **每片几何**（god `setup_slices` `:79` ≡ ko `setup_group` `:126`）：
  `per = ceil(ceil(nbits/n)/msb)*msb`；片 i：`begin = i*per`，`cnt = min(per, nbits-begin)`。
- `SLICES_MODE_DEFRAG`（god ctor）在③之后**强制 n = 1**（全片视角）→ `--defrag`。
- 常量：`msb = min_slice_bits` 默认 **32768**（`RFS_IV_SLICE_MIN_BITS`
  `include/linux/ravenfs_allocator_top.h:68`）；`nbits = bitmap_blocks × 32768`
  （`RAVENFS_BITS_PER_BLOCK` `include/linux/ravenfs.h:62`）；**1 bit = 1 FS_block**。

> ⚠ 本工具走的是 **v2.0 block 单组**几何。v1.1 的 `bitmap_slices_mgr_t` 用「block+inode
> 双组共享 slice_count」，v2.0 inode 面已成弹性数据面、不参与切片——旧几何请勿用。

## 磁盘契约（v2.0）

与 `mkfs_v20.cpp` / `audit` 逐式一致：FS_block=4096B；`Primary SB @ byte 0`；
`bitmap_blocks = ceil(total/32768)`、`bitmap_start = total − bitmap_blocks`；
位序 LSB-first；**1=占用，0=空闲**。

## 构建 / 运行

```sh
make
./rfs_slice_fr <image-or-device> --cpu N [--msb N] [--defrag] [--top N] [--quiet] [--json]
```

- `--cpu N`   喂给共享几何的逻辑 CPU 数。**给定 = 无状态可复现（几何 A/B 的基准）**；
  **省略 = 取机器全局可配置 CPU 数**（`sysconf(_SC_NPROCESSORS_CONF)`，语义对齐 ko
  `num_possible_cpus()`；再退 online 数、再退 1）。来源恒记于 `cpu_src`。
- `--msb N`   片下限（位；须为 2 的幂；默认 32768）。
- `--defrag`  单分片（离线全片视角，对齐 `SLICES_MODE_DEFRAG`）。
- `--top N`   每片 FR 行数上限（0=全列；默认 0）。
- `--quiet`   只出 `GEO` + 每片摘要 + `SUM`（不出 `BSLICE-FR`）。
- `--json`    机读流（替代 `[TRACE]` 行）。

块设备需 `root`/`disk` 组（`/dev/nvme0n1`）；普通镜像文件无需提权。

## trace schema（stdout，`[TRACE][<tag>] k=v ...`）

`cpu_src` 取值：`param`（显式 `--cpu`）/ `host-conf`（`_SC_NPROCESSORS_CONF`，对齐
`num_possible_cpus()`）/ `host-onln`（`_SC_NPROCESSORS_ONLN` 兜底）/ `fallback1`。

| tag | 字段 |
|---|---|
| `BSLICE-GEO` | `cpu cpu_src msb nbits total_blocks bitmap_start bitmap_blocks slices per_slice_bits defrag` |
| `BSLICE`     | `idx begin bits end free used fr max_len min_len` |
| `BSLICE-FR`  | `idx ord base len end`（`base/len` = 全局块号/块数；`end=base+len`） |
| `BSLICE-SUM` | `cpu cpu_src msb slices per_slice_bits fr_total free_total max_len covered_bits covered_blocks` |

示例（2 TB 空盘，显式 `--cpu 32`）：

```
[TRACE][BSLICE-GEO] cpu=32 cpu_src=param msb=32768 nbits=488407040 total_blocks=488378645 bitmap_start=488363740 bitmap_blocks=14905 slices=32 per_slice_bits=15269888 defrag=0
[TRACE][BSLICE] idx=0 begin=0 bits=15269888 end=15269888 free=15269821 used=67 fr=1 max_len=15269821 min_len=15269821
[TRACE][BSLICE-FR] idx=0 ord=0 base=67 len=15269821 end=15269888
...
[TRACE][BSLICE-SUM] cpu=32 cpu_src=param msb=32768 slices=32 per_slice_bits=15269888 fr_total=35 free_total=488363670 max_len=15269888 covered_bits=488407040 covered_blocks=488407040
```

> 注意 `fr_total` 会**随分片数增大**：一条跨片界的整段空闲，在片视图里被夹断成两条
> （片内各一条）——这正是本工具要暴露的东西。上例空盘 1 片时 `fr_total=4`，32 片时 `fr_total=35`。

## 退出码

`0` 正常 · `2` 用法 / IO / 契约错误（magic 不符 / 非 v2.0）。

## 典型用法

```sh
# 真盘：给同一镜像换 cpu 参数，做几何 A/B（结构化、可 diff）
sudo ./rfs_slice_fr /dev/nvme0n1 --cpu 32  > slice_fr_cpu32.log
sudo ./rfs_slice_fr /dev/nvme0n1 --cpu 64  > slice_fr_cpu64.log
diff <(grep BSLICE-GEO slice_fr_cpu32.log) <(grep BSLICE-GEO slice_fr_cpu64.log)

# 只看每片摘要（片段热点一眼可见）
sudo ./rfs_slice_fr /dev/nvme0n1 --cpu 32 --quiet
```

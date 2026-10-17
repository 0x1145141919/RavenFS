# rfs_freeruns — RavenFS v2.0 空闲区间（free-run）dump

只读小工具：按磁盘契约解析 **v2.0 超级块 + 块位图**，枚举**连续空闲块区间**
（bitmap 里 bit=0 的极大 run），给出总量 / 区间数 / 最大区间 / Top-N / 长度直方图。
用途：调研「盘上是否存在超大连续空闲空间，可用于回收/消化碎片」。

## 依据（与 mkfs_v20.cpp / audit 逐式一致）

- `FS_block = 4096 B`；`FS_block[b]` 字节偏移 `= (world_base_lba + b) * lba_size`
  （`world_base_lba = 4096 / lba_size` ⇒ 恒 `= 4096*(b+1)`）。
- Primary SB @ 字节 `0`，`4096 B`，`magic[0]=RFS_SB_MAGIC_W0`，`version=RAVENFS_VERSION(=0)`。
- 几何推导（`mkfs_v20.cpp:129 calc_geometry_v20`）：
  - `bitmap_blocks = ceil(total_blocks / 32768)`
  - `bitmap_start  = total_blocks - bitmap_blocks`（块位图在**尾部**）
- 位序 LSB-first：block `i` → `byte[i/8]` 的 `bit[i%8]`；**`1` = 占用，`0` = 空闲**
  （`bm_mark_range`；元数据 `[0,68)`、位图自身、backup、尾部 pad 全置 1）。

## 编译 / 运行

```sh
make
./rfs_freeruns <image-or-device> [--top N] [--min-blocks N] [--json] [--quiet] [--all]
```

- `--top N`  只列最长 N 条（默认 20；`0` = 全列）
- `--min-blocks N`  只列长度 ≥ N 块的 run（默认 1）
- `--all`  全列（= `--top 0`）
- `--json`  机读输出
- `--quiet` 只出汇总

块设备需 `root`（`/dev/nvme0n1`）；普通镜像文件无需提权。

## 退出码

`0` 正常 · `2` 用法 / IO / 契约错误（magic 不符 / 非 v2.0）。

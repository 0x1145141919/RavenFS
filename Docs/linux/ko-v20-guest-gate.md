# KO v2.0 全局 inode 数据面 · guest 验收闸门（第二跳 · 目标版本）

> 2026-10-06 · RavenFS Linux KO `pkgver=0.2.0`（盘面 v2.0）。
> 第一跳（v1.3 名字数据面）见 `ko-v13-guest-gate.md`；本文件记第二跳（v2.0）与终检。
> 验证尺 = `audit_v20`（master god v2.0 的 `audit_dfs_root` 薄壳）。

## 范围

KO 盘面 v1.3 → v2.0（第二跳）：SB 256-bit magic、无 `root_inode`（root idx 硬编码 0，
`st_ino=1`）；固定 inode 数组 + inode 位图 → **全局弹性 inode 数据面**（8K 片 direct /
映射 B+树 + 扁 fr 快照）；`InodeSession` 走「翻译→物理 8K 片→mdata」。与 v1.x **不兼容**
（reformat，无迁移工具）。

## guest 流程

同第一跳：9p 拷 `linux/*.c` + `Makefile` + `PKGBUILD` + `include/` 到 `~/rfs`；guest 内
就地 `make -C /usr/lib/modules/$(uname -r)/build M=~/rfs/linux modules`；`insmod`；
`mkfs.ravenfs --v2.0 -f /dev/nvme2n1`；mount → churn → remount → umount → `audit_v20`。

## 结果

**顺序压力（`v20-stress.sh`）**
- 挂载 OK；`stat /` → `st_ino=1`（root idx0+1）；初始 `f_files=1024`（32 片 ×32）。
- 2000 文件 → **inode 数据面 grow**（`f_files` 1024→2016）；800 长名（名字数据面）；
  删半 + 重建（fr best-fit 复用）；umount → 重挂载回读一致。
- 6/6 轮（1500 文件）+ 1 轮 3000 文件：`audit_v20 bad_total=0`、零 `LEAK_BLK`、零 ravenfs
  BUG/Oops/panic。

**并发 + sync 风暴（`v20-mt.sh`）**
- 4 并发 writer × 4000 建删 + 40× `sync` 风暴 + 10× mount/umount 风暴；
  终态 8000 文件；`audit_v20 bad_total=0`、零泄漏、零 BUG。
- 注：`sync_fs` 的「inode 面维护（建映射树在先）→ 块位图补集重建」顺序**刻意不完全锁死**
  ——只需保证「建树在先 ⇒ 位图覆盖上」的不变量，不求完全同步（否则性能宕机）。
  审计结果为该不变量的实测背书。

## 期间抓出并修复的缺陷

**非确定性 `[LEAK_BLK] 块区间 [0,+2)`（已修，提交 `0ea72b3`）**
- 现象：同数据同代码，部分轮次 audit 报 block 0 泄漏、部分干净。
- 取证（`v20-diag.py`）：block 0 内容魔数 = `INODE_DATA_EXTENT_MAGIC*`（旧 inode 映射树
  根叶），位图亮但当前映射树不可达 → 旧树孤儿未释放。
- 根因：**v2.0 契约明令「0 是合法块号，禁用 0 作哨兵」**，而
  `ravenfs_extent_orphan_release_kind` 顶层 `root==0`、`ravenfs_nd_map_reclaim` 顶层
  `tree_root==0` 早退 → 旧树根恰为 block 0 时整棵漏释放。
- 修：两处仅认 `PTR_INVALID` / ptr 域全一为无效。
- 验证：6/6 + 3000 + MT 全 `bad_total=0`。

## 包内容

- `PKGBUILD pkgver=0.2.0`；修包内容：`disk_struct_v2.0.h #include "ravenfs/rc.hpp"`，
  原先只装 `include/*.h` + `include/linux/*.h` → 现安装 `include/ravenfs/`（否则 DKMS 缺头）。
- 模拟 DKMS 安装布局 + 宿主内核头编译通过，`modinfo -F version = 0.2.0`。

## 遗留（非本目标范围）

- `linux/tools/rfsdirchurn.c`（名字堆几何）与 `linux/pkg/` 旧快照仍残 v1.2 引用——非构建路径、
  非包内容，未清理。
- `nh_*`/`row_size=` 等旧语义字段/选项保留但已惰性。
- `name_data_rebuild` / `name_data_fr_compact`（离线压实/压缩）未移植。
- 未跑 lockdep/KASAN（stock 内核）；未做 ENOSPC 往返与大文件 O_DIRECT 矩阵。
- 仅契约移植：不随包审计/fsck 工具（审计尺为宿主 god 构建，非包内容）。

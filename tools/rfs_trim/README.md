# tools/rfs_trim — RavenFS 离线裁剪（trim/discard）工具

Host 侧、**自包含**（不复用 god 库、不挂载）的离线裁剪器：FS **卸载后**，按
**block 位图**机械地把空闲 FS_block 区间裁掉还给底层——

- **块设备** → `ioctl(BLKDISCARD)`（内核 TRIM/discard，thin-provisioning 还空间 / SSD TRIM）
- **镜像文件** → `fallocate(PUNCH_HOLE|KEEP_SIZE)`（真打洞 → 稀疏文件）

> 位图与文件树是否一致属 `tools/audit_v1.2` 辖区，本工具**假定 audit 通过**，
> 只做机械执行 + 一道便宜的安全闸（见下）。SB 解析与派生布局公式照抄
> `tools/sbinfo`（mkfs `calc_geometry` 同式），位序同 mkfs `BitmapBuf::mark`
> （LSB-first：bit i = `bmp[i/8] >> (i%8) & 1`）。

## 构建 / 自测

```sh
make                    # g++ -std=c++17 -Wall -Wextra -O2 -g -I../../include（0 error 0 warn）
make check              # = ./rfs_trim --selftest-plan && ./selftest.sh
./selftest.sh           # 纯函数单测 + 文件模式端到端（16MiB 稀疏镜像，30 项断言）
```

## 用法

```sh
./rfs_trim <image-or-device>            # DRY-RUN（默认，O_RDONLY 只报告，零写入）
./rfs_trim <image-or-device> --apply    # 真裁剪（需可写；FS 必须先 umount）
./rfs_trim --selftest-plan              # 纯函数离线单测（不碰盘）
```

退出码：`0` 成功 · `1` 校验拒跑 / 裁剪失败 · `2` 用法 / IO 错误。

### 报告字段

```
FS 覆盖字节      : total_blocks × 4096（FS 世界总容量）
逻辑空闲字节     : block 位图中 0 的个数 × 4096（[0,total_blocks) 的 0 游程合计）
对齐后可裁字节   : 只保留**完整 discard 单元**后真正能裁的字节
损耗比           : (逻辑空闲 − 对齐后可裁) / 逻辑空闲 —— 被对齐吃掉、裁不动的部分
段数             : 原始 0 游程段数 → 对齐后实际裁剪段数
```

## 流程与安全

1. **判型**：`S_ISBLK` → 块设备（`BLKGETSIZE64` + `BLKSSZGET`，失败回退 LBA=4096 并 WARN）；
   `S_ISREG` → 镜像文件（LBA 恒 4096）。`world_base_lba*lba ≠ 4096` 违 mkfs 契约 → WARN。
   world 末端超设备尺寸 → 拒跑（几何与盘不符）。
2. **SB（LBA0 = 字节 0，4096B）**：magic 必须 `"RAVN"`（0x438 检出 ext magic 会直说
   「这盘是 ext2/3/4」，同 sbinfo）；version ∈ {2(v1.1), 3(v1.2)}，否则拒跑。
3. **派生布局**（mkfs `calc_geometry` 同式）：
   `bitmap_blocks = ceil(total/32768)`、`bitmap_start = total − bitmap_blocks`、
   `inode 数组 = ceil(ic*256/4096)` 块、`inode 位图 = ceil(ic/32768)` 块，依次向下推
   `inode_array_base` / `inode_bm_base`（`inode_bm_base ≤ 1` → 拒跑）。
4. **读 block 位图**：`byte = world_base_lba*lba + bitmap_start*4096`，长 `bitmap_blocks*4096`。
5. **安全闸（不过就拒）**：bit `[inode_bm_base, total_blocks)` 必须**全 1**——自描述三段
   （inode 位图 / inode 数组 / block 位图自身；mkfs `format_fs` 恒 `mark_range`）。
   出现 0 → 位图损坏或几何不可信（同 mkfs `RC_ERR_BITMAP_SELF`）→ `exit 1`，**一个字节都不动**。
6. **裁剪对齐**：
   - 块设备：`/sys/dev/block/<maj>:<min>` → `queue/{discard_granularity,discard_alignment}`
     （分区退父目录；拿不到保守 **4096/0**），按 `queue/max_discard_bytes`（缺省 **1MiB**，
     且校正为 gran 整数倍）切批；只发**两端都对齐**的完整 discard 单元。
   - 镜像文件：恒 gran=4096 / align=0，punch 不切批。
7. **`--apply` 前置**：路径出现在 `/proc/mounts`（块设备按 maj:min 比对，防 by-uuid 别名；
   文件按 st_dev+st_ino）→ 拒跑（双写者风险）。执行后 `fsync`；失败批逐条报 stderr 并计数，
   `exit 1`。

纯计算（位图 → 按 granularity 对齐的段列表）抽成 `plan_trim()` 纯函数：
不碰盘、不碰全局状态，`--selftest-plan` 里 5 例离线断言（含粒度放大收窄、
游程跨不进单元整段拒裁、world 基址 512 不对齐两头裁齐）。

## 自测覆盖与未决点

`selftest.sh`（30 项断言，退出 0 = 全过）：

- **文件模式端到端**：构造 16MiB 稀疏镜像（合法 SB：magic/version/world_base_lba=1/
  total_blocks=4095/ic=64 自洽 + block 位图空闲游程 [1000,3000)、尾巴全 1）：
  - dry-run：报告数字与构造一致（FS 覆盖 16773120 / 逻辑空闲 8192000 / 对齐后可裁 8192000 /
    损耗比 0.00% / 段数 1→1）、`st_blocks` 不变、镜像字节纹丝不动；
  - `--apply`：`st_blocks` 32768 → **16768**（= 8192000/512，真打洞）、空闲区读回全 0、
    SB/占用区/位图字节不变；再 `--apply` 幂等（退出 0，`st_blocks` 不再变）；
  - **拒跑**：尾巴（inode 数组区）挖一个 0 → `--apply` exit 1 且 `st_blocks` 不变；
  - 边角：无参数 exit 2、`--help` exit 0、零块非 RavenFS → 魔数诊断 exit 1。
- **未决点**：真实块设备 `BLKDISCARD` 路径**无可用盘不测**（需 root + 可丢弃的块设备；
  本环境不 sudo）——文件模式已覆盖主逻辑（同一套 `plan_trim` 对齐/段切分 +
  同一份报告），块设备仅差异在 sysfs 参数读取与 ioctl 发射，留待有盘环境用
  `rfs_trim /dev/<disk>` dry-run 先核对 `discard: gran/align/max_batch` 行后再 `--apply`。

## 安全

- `--apply` **就地把底层空间裁掉**（块设备上 discard 不可逆）。务必：① 先 `umount`
  （工具会查 `/proc/mounts`，但别只靠它）；② 先 dry-run 复核报告与段列表；③ 位图必须是
  最近一次 audit 通过后的盘面（本工具不复验一致性）。
- 只动**空闲块**，不写任何数据/元数据字节；幂等（重跑只是重发空裁剪）。

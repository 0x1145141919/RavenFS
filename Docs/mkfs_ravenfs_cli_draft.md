# mkfs.ravenfs — CLI 参数企划

> 草案 — 2026-07-19
> Linux 用户态命令行格式化工具设计。

---

## 1. 命令形态

```
mkfs.ravenfs [options] <device>
```

`<device>` 是必选位置参数，可以是块设备（`/dev/nvme0n1pX`）或普通文件（`/tmp/test.img`）。

---

## 2. 参数清单

| 参数 | 必选 | 默认值 | 说明 |
|------|------|--------|------|
| `--inode-ratio=N` | 否 | 8 | 每 inode 平均管辖多少 FS_block，16 向上取整 |
| `--backups=N` | 否 | 3 | Backup Superblock 散布数量 |
| `--label=NAME` | 否 | 空 | 卷标，最长 48 字节 |
| `--uuid=UUID` | 否 | 自动生成 | 显式指定 UUID，否则随机生成 |
| `--force` / `-f` | 否 | 无 | 不询问直接覆写已有文件系统 |
| `--verbose` / `-v` | 否 | 无 | 打印布局详情（元数据区间、块号等） |
| `--help` / `-h` | 否 | 无 | 打印帮助信息并退出 |

### 2.1 `--inode-ratio=N` — inode 密度控制

**语义：** 期望每个 inode 平均管辖 N 个 FS_block。N 越小 → inode 越多，适合海量小文件；N 越大 → inode 越少，元数据开销更低。

**默认：** 8（即每 32 KB 数据一个 inode，兼顾多数场景）。

**换算公式：**

```
raw_count    = ceil(total_blocks / inode_ratio)
inode_count  = ceil(raw_count / 16) × 16
```

16 向上取整的理由：每个 FS_block（4096B）恰好容纳 16 个 inode 条目（256B/个），使 inode 数组尾部不浪费部分 FS_block。

**行为：**
- N 必须是正整数
- 推导：`inode_blocks = ceil(inode_count × 256 / 4096)`（block_model.md §3.2）

### 2.2 `--backups=N` — Backup Superblock 数量

- N = 0 时 **不创建** Backup Superblock（分区不可容灾）
- N ≥ 1 时 mkfs 在 `[0, total_blocks)` 范围内选择 N 个 FS_block 散布
- 散布策略：hash-based 伪随机，确定性可重复（v1 不需要可重复性保证，但为后续调试留口）
- N 上限：不超过 `total_blocks / 1024`（粗略保护，不占太多元数据）

默认 3 的理由：block_model.md §4.4 指出 v1 禁用 online expand，3 个备份足以应付单盘失效。

### 2.3 `--uuid=UUID` — 显式 UUID

不指定时自动生成（Linux `uuid_generate`）。
指定时格式：标准 UUID 字符串 `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`。

### 2.4 `--force` / `-f`

**行为：**

| 场景 | 无 `-f` | `-f` |
|------|---------|------|
| 已有有效 FS（ext4/btrfs/xfs/RavenFS 等） | ❌ 报错退出 | ✅ 覆写 |
| 设备已挂载 | ❌ 报错退出 | ❌ 仍然拒绝 |
| 魔数全空（未格式化） | ✅ 直接执行 | ✅ 直接执行 |

检测到挂载的设备无论如何不写——`-f` 不覆盖挂载保护。

### 2.5 `--verbose` / `-v`

默认输出一行摘要：
```
mkfs.ravenfs: done  device=/dev/nvme0n1p3  blocks=100000  inodes=65536  backups=3  uuid=...
```

`-v` 展开输出：

```
device:           /dev/nvme0n1p3
total_blocks:     100000
inode_count:      65536

metadata layout (FS_block world):
  bitmap:          [99996, 100000)     (4 blocks)
  inode array:     [99980, 99996)      (16 blocks)
  inode bitmap:    [99976, 99980)      (4 blocks)

backup locations:
  backup #0:       block 42
  backup #1:       block 8731
  backup #2:       block 65535
```

---

## 3. 设备检测

### 3.1 块设备与普通文件

`stat()` 判断设备类型：

- **块设备（`S_ISBLK`）：** 检查 `partition_start_lba`（GPT/裸盘逻辑），检测是否已被系统挂载
- **普通文件（`S_ISREG`）：** 无条件接受，用于快速迭代测试

### 3.2 已有文件系统检测

检测到任何有效魔数 → **报错退出**，除非 `-f` 显式指定。

顺序检查：
1. **LBA 0** 的 `magic = "RAVN"` → 已有 RavenFS，报错退出
2. 常见魔数扫描（ext4 `0xEF53`、btrfs `_BHRfS_M`、xfs `XFSB`、ntfs `NTFS`）→ 报错退出
3. 以上均未命中 → 继续执行

### 3.3 分区大小确定

- 块设备：`ioctl(BLKGETSIZE64)` 获取字节数，或通过 GPT 边界
- 普通文件：`fstat()` 获取大小
- **对齐约束：** 分区大小必须是 4096 的整数倍（即 FS_block_size），否则报错退出

---

## 4. 参数校验顺序

```
1. 解析参数
2. stat() 检查 device 是否存在、是否为普通文件或块设备
3. 块设备 → 检查是否已挂载（已挂载直接拒，`-f` 也不放过）
4. 检测已有文件系统（有则拒，`-f` 跳过此项）
5. `--inode-ratio` 合法性检查（正整数）
6. `--backups` 合法性检查（≤ total_blocks/1024）
7. `--label` 长度检查（≤ 48B）
8. `--uuid` 格式检查（如指定）
9. 校验通过 → 执行 mkfs
```

---

## 5. 错误码

| 退出码 | 含义 |
|--------|------|
| 0 | 成功 |
| 1 | 参数错误 |
| 2 | 设备已挂载 |
| 3 | 已有文件系统（需 `-f` 覆写） |
| 4 | 分区大小不对齐或过小 |
| 5 | 权限不足 / 设备访问失败 |

---

## 6. 未决事项（v1 不做）

| 事项 | 原因 |
|------|------|
| 交互式引导（`--interactive`） | v1 追求简洁，CLI 参数化不欠设计 |
| json/yaml 输出（`--json`） | phase 0 不需要程序化输出 |
| 进度条 | 小分区秒写，不必要 |
| `--dry-run` | 可用 `-v` 预览布局，本质上等价 |

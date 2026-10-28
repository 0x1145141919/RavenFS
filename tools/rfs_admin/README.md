# rfs_admin —— RavenFS 离线镜像运维工具

`rfs_admin` 直接对 **RavenFS 镜像文件**（普通 file，必要时也可只读块设备）做浏览、
读写、递归复制、删除、空间统计、全盘审计和 `.vmcore` 检查。

**不需要 `sudo`，不需要 `mount`，不需要 `losetup`。** 工具用与内核模块同一份
上帝类（`RavenFS_juvenile`）以文件 fd 挂载，普通文件走 mmap 后端。

---

## 1. 为什么需要它

SparrowOS 的 TCG / 真盘 `mem_dump` 测试会往 RavenFS 根目录写一个约等于 RAM 大小的
文件：

```
/sparrowOS_full_mem_<14 hex>.vmcore
```

以前要查看 / 搬运 / 删除它，只能：

```bash
sudo mount /mnt/huge_data          # 需要 root，且要求镜像离线
sudo ls -l /mnt/huge_data
sudo rm /mnt/huge_data/xxx.vmcore
sudo umount /mnt/huge_data
```

这条路径有两个问题：

1. 需要 root；CI / 开发机 / 嵌入式现场未必有。
2. 8 GiB 的 dump 光查看 ELF 头就要先想办法挂载；想校验 digest 更得整份读一遍。

`rfs_admin` 把这件事变成：

```bash
rfs_admin ravenfs-data.img df
rfs_admin ravenfs-data.img ls -l /
rfs_admin ravenfs-data.img vmcore /sparrowOS_full_mem_xxx.vmcore --verify
rfs_admin ravenfs-data.img get /sparrowOS_full_mem_xxx.vmcore /tmp/dump.vmcore
rfs_admin ravenfs-data.img rm /sparrowOS_full_mem_old.vmcore --shred
```

**离线**（镜像不能同时被内核挂载或被其它写入者使用）是硬前提。

---

## 2. 构建

out-of-tree，不污染仓库：

```bash
cmake -S /home/PS/PS_git/OS_pj_uefi/ravenfs \
      -B /tmp/rfs_admin_build \
      -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/rfs_admin_build -j"$(nproc)" \
      --target rfs_admin mkfs.ravenfs
```

产物：

```
/tmp/rfs_admin_build/tools/rfs_admin/rfs_admin
/tmp/rfs_admin_build/mkfs/mkfs.ravenfs
```

仅需 C++20 + 仓库现有依赖；`tests/cpu_id_stub.cpp` 提供用户态
`get_processor_id()` weak 桩（与其它宿主目标同一约定）。

---

## 3. 安全前提（务必读）

- **offline only**：镜像必须处于未挂载、无其它写者状态。
  - 上帝类构造时会做 `device_is_kernel_mounted()` 防呆；已挂载设备直接
    `RC_ERR_BUSY`，工具打印“请先 umount 或改用副本”。
  - 工具**不会**传 `allow_mounted_device=true`，也不提供强制写开关。
- **没有 journal / 事务**：写操作不是崩溃安全的。`put --force` 是
  “先 truncate(0) 再写”，中途失败可能留下半成品（新建目标会尽力回滚目录项，
  覆盖目标无法还原旧内容）。重要镜像先备份。
- **写命令退出时提交**：让 `RavenFS_juvenile` 正常析构 = `all_sycn` +
  block 位图回写 + SB 时间戳。工具不会 `_exit`，正常/错误路径都会走析构。
- 读命令以 `is_readonly=true` + mmap（`MAP_PRIVATE`）挂载，**不写盘**。
- 工具不使用 `mount(2)` / `losetup` / `ioctl` 改设备状态；普通文件仅
  `open/read/write/pread/pwrite/mmap/fsync`。
- 工具不做 fsck 修复；发现异常请保留镜像并用 `audit` 定位。

---

## 4. 命令表

```
rfs_admin <image> <command> [args...]
```

公共选项（可出现在命令后任意位置）：

| 选项 | 说明 |
|------|------|
| `--readonly` | 强制只读挂载；写命令与之组合会直接拒绝 |
| `--json` | 机器可读输出（`df/info/ls/stat/hash/audit/vmcore/find`） |
| `--help` | 帮助 |

读命令会自动按只读挂载；写命令自动按读写挂载。

### 只读

| 命令 | 说明 |
|------|------|
| `info` | SB / 几何 / 位图 / 空闲块 / inode；含 `dump_info()` + df |
| `df` | 空闲块 / 总块 / 空闲 inode / 使用率 |
| `ls [-l] [-R] [path]` | 列目录，默认 `/`；`-l` 打类型/权限/大小/inum；`-R` 递归；按名字排序 |
| `stat <path>` | type / size / link_count / mode / uid / gid / form / data_root / inum；目录额外打 ddentry_root 与名字数据面 |
| `tree [path] [--depth N]` | 打印 dentry B+tree 每个节点（blk/level/leaf\|inner/count/hash 范围/parent）与叶条目；文件打印 inline/direct/extent 形态、extent 逐条记录、direct 前 256B hex |
| `cat <path> [--offset N] [--length N]` | 原始字节到 stdout |
| `get <path> <hostfile> [--offset N] [--length N] [--force]` | 1 MiB 块流式拷出；支持 8 GiB+ |
| `hash <path> [--offset N] [--length N]` | `xxh64=<16hex> len=<n>`（XXH64 seed=0） |
| `audit [--verbose]` | `audit_dfs_root` 全盘可达性审计；`total_bad>0` 退出 1 |
| `vmcore <path> [--verify]` | ELF64 / PT_LOAD / PT_NOTE / SPARROW note / digest |
| `find [path] [--name SUBSTR]` | 递归按子串找名字 |

### 写

| 命令 | 说明 |
|------|------|
| `mkdir [-p] <path>` | 逐级创建；已存在且是目录 = 成功；同名文件 = 错 |
| `put <hostfile> <fspath> [--force] [--mkdirs]` | 1 MiB 块流式写；目标已存在默认报错；`--force` 先 truncate(0) |
| `rm [-r] [--shred] <path>` | 文件直接删；目录必须 `-r`（后序递归删空）；`--shred` 释放前清零；打印空闲块差值 |
| `cpout <fspath> <hostdir> [--force]` | 递归导出目录**内容**到 hostdir；文件导出为 `<hostdir>/<basename>` |
| `cpin <hostdir> <fspath> [--force]` | 递归导入 hostdir 的**内容**；symlink / 特殊文件跳过并告警；只处理普通文件与目录 |
| `truncate <path> <size>` | 截断/扩展普通文件（扩展为稀疏零） |

安全拒绝：

- `rm /`、`rm -r /` 一律拒绝。
- 所有路径分量中的 `..` 一律拒绝（含 `cpin` 目标）。
- 镜像已被内核挂载 → `RC_ERR_BUSY`，明确提示先 umount / 换副本。
- 失败操作打印具体 path + `RC_*` 名称，不静默跳过。

退出码：`0` 成功；`1` = `audit` 有 bad，或 `vmcore --verify` 校验失败；
`2` = 用法 / I/O / 挂载 / 路径错误。

---

## 5. 例子

```bash
# 空间概览
rfs_admin ravenfs-data.img df
rfs_admin ravenfs-data.img info

# 看目录（长名会完整显示，不截断）
rfs_admin ravenfs-data.img ls -l /
rfs_admin ravenfs-data.img ls -lR /src

# 看一个文件的 inode 与物理形态
rfs_admin ravenfs-data.img stat /src/sub1/f1.bin
# → form=direct data_root=...
#   form=extent 时 data_root 为区段树根

# 直接看内部结构
rfs_admin ravenfs-data.img tree /src --depth 2
rfs_admin ravenfs-data.img tree /src/sub1/f1.bin

# 校验一个文件
rfs_admin ravenfs-data.img hash /src/sub1/f1.bin
xxh64sum /tmp/f1.bin        # 与 get 出来的文件对拍

# 拷进/拷出
rfs_admin ravenfs-data.img put ./firmware.bin /images/firmware.bin --mkdirs
rfs_admin ravenfs-data.img get /images/firmware.bin /tmp/firmware.bin
rfs_admin ravenfs-data.img cpout /images /tmp/images_out
rfs_admin ravenfs-data.img cpin /tmp/new_images /images --force

# 删除并确认空间回收
rfs_admin ravenfs-data.img rm /images/firmware.bin
rfs_admin ravenfs-data.img rm -r /old_tree --shred
rfs_admin ravenfs-data.img df
rfs_admin ravenfs-data.img audit
```

---

## 6. TCG mem_dump 运维流程（重点）

```bash
# 1) 看根目录和空间
rfs_admin ravenfs-data.img df
rfs_admin ravenfs-data.img ls -l /

# 2) 找 dump（名字形如 sparrowOS_full_mem_<14 hex>.vmcore）
rfs_admin ravenfs-data.img ls -l / | grep sparrowOS_full_mem

# 3) 只检查 ELF 头 / PT_LOAD / SPARROW note + 两个 digest，不拷贝 8GiB
rfs_admin ravenfs-data.img vmcore /sparrowOS_full_mem_xxx.vmcore --verify

# 4) 只取头部 / notes 看（例如给 readelf 看 PT_NOTE）
rfs_admin ravenfs-data.img get /sparrowOS_full_mem_xxx.vmcore /tmp/dump.hdr --length 4096
readelf -h -l -n /tmp/dump.hdr

# 5) 整份拷出给 gdb / vmcore-mkcore.py
rfs_admin ravenfs-data.img get /sparrowOS_full_mem_xxx.vmcore /tmp/dump.vmcore

# 6) 删除旧 dump 释放空间，并确认没有块/inode 泄漏
rfs_admin ravenfs-data.img rm /sparrowOS_full_mem_old.vmcore --shred
rfs_admin ravenfs-data.img df
rfs_admin ravenfs-data.img audit          # 必须 total_bad=0
```

### 6.1 `vmcore` 做了什么

- 读 4 KiB ELF 头，按 `e_phoff/e_phnum/e_phentsize` 读程序头；
  **不严格校验** `e_ehsize` / section header（QEMU 样本可能 `e_ehsize=8`、
  带 2 个 section header）。
- 校验 `ELFCLASS64` / `ELFDATA2LSB` / `ET_CORE(4)` / `EM_X86_64(62)`。
- 打印每个 `PT_LOAD` 的 `p_offset / p_vaddr / p_paddr / p_filesz / p_memsz`。
- 解析 `PT_NOTE`：统计 `CORE/NT_PRSTATUS` 与 `QEMU` note；
  查找厂商 note `owner="SPARROW\0"`、`type=0x53504D44`（"SPMD"）。
- 没有 SPARROW note 的 QEMU 原生 vmcore 正常打印，跳过 digest 校验，不报错。

SPARROW note desc（64B，kernel `fullmemdump` 契约）：

| 字段 | 含义 |
|------|------|
| `magic` | `"SPWFMD01"`（0x3130444D46575053） |
| `version` | 1 |
| `flags` | bit0 数据完整 / bit1 写端已自验 / bit2 notes 带寄存器 |
| `total_bytes` | 整个 ELF 文件字节数 |
| `dram_top` | dump 覆盖的物理上界 |
| `segments` | PT_LOAD 数量 |
| `layout_xxh64` | `[0, SPARROW note 起始偏移)` 的 XXH64 |
| `data_xxh64` | 按 phdr 顺序拼接所有 PT_LOAD `p_filesz` 字节的 XXH64 |
| `data_offset` | 第一个 PT_LOAD 的文件偏移 |

`--verify` 额外校验：

- `total_bytes == 文件大小`
- `segments == PT_LOAD 数量`
- `data_offset == 第一个 PT_LOAD.p_offset`
- `data_xxh64`、`layout_xxh64` 重算比对

`layout_xxh64` **以解析到的 SPARROW note 头偏移为界**（不是 PT_NOTE 段起点）——
kernel 会把调用方 notes 与 4B 对齐垫片一起算进 layout，SPARROW note 本身不参与。

8 GiB 文件只有 `--verify` 会流式全读，进度每 256 MiB 打到 stderr；普通查看只读
头部与 notes 的几 KiB。

---

## 7. 内部结构视图

```bash
$ rfs_admin img tree /src
[目录] /src  inum=1 entries=2 ddentry_root=13292
  blk=13292 level=0 leaf count=2 root=1 parent=1 hash=[...]
    entry[0] hash=4952... name="abc" inum=8 type=file nlen=3
    entry[1] hash=8712... name="sub1" inum=2 type=dir nlen=4

[目录] /src/sub1 ...
  name_data: ctl=0x69e5 direct ptr=13554 pieces=1 fr_ctl=0x1 fr_count=1
```

- 目录树按 **dentry B+tree** DFS 打印：每节点 `blk / level / leaf|inner /
  count / hash 范围 / parent`，叶条目 `hash name inum type`。
- 条目 >126 会分裂出 inner 节点；`ls` 的枚举自行沿「最左叶 → `meta.ptrs[2]`
  兄弟链」遍历，根叶不跟 next（根叶 ptrs 是 `.`/`..` 语义）。
- 名字 >40B 由目录名字数据面承载；`ls/tree/stat` 都通过 `name_data_read`
  拼回完整名字（上限 552B，不截断）。
- 文件：`inline`（≤160B）/ `direct`（单连续物理段）/ `extent`（区段 B+tree，
  逐条 `fblkbase length phys`）。
- `stat` 目录额外给出 `ddentry_root` 与名字数据面 ctl / pieces / fr。

`audit` 输出示例：

```
[audit] total_bad=0 (btree=0 inode=0 namedata=0 inodeplane=0 xattr=0 ... )
[audit] VERDICT: PASS
```

`total_bad` 汇总 btree / inode / 名字数据面 / 全局 inode 数据面 / xattr /
二次引用 / link_count / `..` / 未保护块 / 泄漏块。任何一项非 0 都退出 1。

---

## 8. 实现边界与已知限制

- **离线无并发**：不能与内核挂载、写入者同时使用；工具没有锁协议。
- **没有 journal**：写操作中途崩溃/断电可能留下半成品或孤儿块；需要时重跑
  `audit` 定位（本工具不做修复）。
- **symlink 不复制**：`cpout` 遇到 `SOFT_SYM` 报错；`cpin` 遇到 host symlink
  跳过并告警（不跟随，避免逃出源目录）。`cpin` 只处理普通文件和目录。
- **mode / uid / gid 不保留**：god 创建文件固定 0644、目录 0755；导出按宿主
  umask 落 0644。host → FS 的 uid/gid 不映射。
- **host 文件名长度**：受宿主 FS 限制（ext4 通常 255B），超过会失败；
  RavenFS 自身支持 552B，`cpin` 会把 host 名原样写入。
- `cpout/cpin` 的语义是 **目录内容的镜像**：`cpout /a /out` 得到
  `/out/<a 的子项>`；`cpin /in /a` 得到 `/a/<in 的子项>`。
- `put --force` 非原子；覆盖前不备份旧内容。
- 未实现 fsck 修复；`audit_fsck_root` 仍是核心里的骨架（本工具不调用）。
- `--json` 覆盖 `df/info/ls/stat/hash/audit/vmcore/find`；其余命令保持文本输出。
- 镜像 LBA 大小：普通文件默认 4 KiB；块设备用 `BLKSSZGET` 探测。

---

## 9. 与仓库核心的边界

- 未修改 `juvenile_god/*.cpp` 或 `include/ravenfs/ravenfs_juvenile.hpp`。
- 私有接口（`dir_lookup/dir_insert_dentry/dir_rm_dentry`、`direct_file_*`、
  `read_buff`、`name_data_read`、`allocator` 等）通过测试同款
  `#define private public` 访问。
- 目录枚举没有调用只有声明、没有实现的 `root_dir_readdir` /
  `dir_readdir_interval`，而是按盘面结构独立 DFS。

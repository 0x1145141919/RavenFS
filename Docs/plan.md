# RavenFS 开发路线规划

> 对 `raven_toolchain.md` 的补充与修正。
> 2026-06-30

---

## 总体阶段

```
Phase 0: mkfs.ravenfs
   裸格式化工具，写盘验证布局
      ↓
Phase 1: 上帝类 + 测试集
   全部算法内聚，面向测试
   挂载 01:00.0 NVMe 实验盘，增删读写
      ↓
Phase 2: ravendsrv
   用户态守护进程，io_uring 驱动
   IPC = NVMe SQ/CQ 模型
   打磨运行时文件系统理解
      ↓
Phase 3: ravenfs.ko + SparrowOS kmod
   各端独立适配，算法可互相参考但不过早抽象
```

---

## Phase 0 — mkfs.ravenfs

**目标**：在 NVMe 实验盘（01:00.0）上写出正确的磁盘布局。

### 职责

- 裸 `pread`/`pwrite` 操作分区
- 写 Superblock、Bitmap、Inode Array、根目录 B+tree
- 无需缓存，单次顺序写完
- 不实现任何文件操作（create/read/write）

### 验证

1. `mkfs.ravenfs /dev/nvme0n1pX --inodes=65536`
2. `hexdump -C` 检查 superblock magic、inode array 位置、bitmap 覆盖
3. 读回 superblock 校验字段一致性
4. 验证 bitmap：元数据块标记为已分配，其余空闲

### 依赖

- 直接使用 `core/include/ravenfs/disk_struct.h`
- I/O 走 `pread`/`pwrite`（暂不引入 io_uring）

### 里程碑

```
mkfs.ravenfs /dev/nvme0n1pX
→ superblock magic 正确
→ bitmap: 元数据块 = 1，其余 = 0
→ inode[0] (root) 有效，inline_data 空目录项 B+tree
```

---

## Phase 1 — 上帝类 + 测试集

**目标**：在 Phase 0 格式化的分区上，实现完整的文件操作，通过测试集验证。

### 策略

- 所有算法内聚在 `RavenFS` 一个类中
- 不拆分任何层，不做通用抽象
- `io_backend.h` 仅用于区分 I/O 后端（host = pread, sparrowos = NVMe 驱动），不是算法抽象

### 测试集（init_fs_t 模式）

继承 initfs 的历史经验——面向测试的上帝类，CLI 驱动测试：

```
ravenfs_cli /dev/nvme0n1pX mkfs
ravenfs_cli /dev/nvme0n1pX mkdir /mydir
ravenfs_cli /dev/nvme0n1pX create /mydir/hello.txt
ravenfs_cli /dev/nvme0n1pX write /mydir/hello.txt "world"
ravenfs_cli /dev/nvme0n1pX read /mydir/hello.txt
ravenfs_cli /dev/nvme0n1pX ls /mydir
ravenfs_cli /dev/nvme0n1pX stat /mydir/hello.txt
ravenfs_cli /dev/nvme0n1pX rm /mydir/hello.txt
```

测试步骤：

| # | 操作 | 验证点 |
|---|------|--------|
| 1 | mkfs | 布局正确 |
| 2 | 创建单文件 | inode 分配、dentry 写入 |
| 3 | 读写小文件 | inline data 路径 |
| 4 | 读写跨 extent 大文件 | B+tree 多节点遍历 |
| 5 | 创建/删除目录 | dentry B+tree 增删 |
| 6 | 重命名 | dentry 更新 |
| 7 | 空洞文件 | 未分配 extent 返回零 |
| 8 | 碎片化写入 | B+tree 分裂/合并 |

### 测试战场

- **主战场**：`/dev/nvme0n1pX`（01:00.0 实验盘）
- 双用途：既是 RavenFS 测试盘，也是 SparrowOS NVMe 驱动的压力测试场
- 备选：`/tmp/test.img` loop 文件（快速迭代时用）

### 里程碑

```
RavenFS 上帝类通过全部 8 步测试
→ 文件系统操作完备
→ 布局与 Phase 0 mkfs 一致
→ 可进入 Phase 2
```

---

## Phase 2 — ravendsrv

**目标**：用户态守护进程，打磨运行时文件系统理解和并发控制。

### 架构要点

- **IPC 模型**：NVMe SQ/CQ 模型
  - Submission Queue：客户端提交请求（read/write/create/delete）
  - Completion Queue：服务器完成通知
  - 设计动机：为 SparrowOS kmod 的 NVMe SQ/CQ 交互积累工程经验
  - 不是自定义 socket 协议，而是借用 NVMe 队列语义

- **I/O 引擎**：`io_uring`
  - 替换 Phase 0/1 的 `pread`/`pwrite`
  - 异步提交 + 完成轮询
  - 为 `ravenfs.ko` 的 `bio_alloc`/`submit_bio_wait` 路径做铺垫

- **FUSE**：可实现但不优先
  - FUSE 不是第一等目标，是锦上添花
  - 先保证 SQ/CQ + io_uring 路径正确，有余力再加 FUSE 导出

- **并发模型**：待定（Phase 1 测试集给出压力数据后决定）
  - 单线程 io_uring？（B+tree 不需要复杂并发）
  - 多 SQ 多 CQ？（模仿 NVMe 多队列）

### 与 Phase 1 的关系

上帝类的全部文件操作直接复用，ravendsrv 只是替换 I/O 后端和添加 IPC 层：

```
God class (algorithm)  ← Phase 1 产物，不做算法抽象
     ↕
PosixIO    → 替换 →   IoUringIO     ← Phase 2 新后端
     ↕                    ↕
pread/pwrite          io_uring          ← I/O 引擎
     ↕                    ↕
loop file / 分区       NVMe 分区
```

### 里程碑

```
ravendsrv 启动 → 接收 SQ 请求 → io_uring 读写 → CQ 完成通知
→ 多客户端并发访问 01:00.0
→ 长时间运行无数据损坏
```

---

## Phase 3 — ravenfs.ko + SparrowOS kmod

**目标**：各端独立适配。

### 原则

- **不要提前设计通用算法抽象**
- Phase 1 的 god class 就是"标准实现"
- 各端开发者直接参考 god class 的算法逻辑，按自己端的 API 翻译
- B+tree 分裂/合并、inode 分配、bitmap 管理等核心逻辑完全一致
- 差异只在 I/O 后端和外部接口（VFS 回调 vs kmod 函数指针表）

```
God class (reference implementation)
    ↓ 参考
ravenfs.ko          SparrowOS kmod
├─ I/O: bio_alloc   ├─ I/O: NVMe 驱动函数指针
├─ VFS: file_op     ├─ 接口: kmod 符号表
└─ 缓存: xfs_buf    └─ 缓存: 自有分配器
```

### 里程碑

```
ravenfs.ko mount -t ravenfs /dev/nvme0n1pX /mnt
SparrowOS kernel 加载 ravenfs.kmod → 挂载读写
```

---

## 不要做的事

| 事项 | 原因 |
|------|------|
| 提前设计算法通用抽象层 | Phase 1 上帝类就是标准实现，各端直接参考移植 |
| FUSE 作为一等目标 | 可实现但不优先，先打好 SQ/CQ + io_uring 基础 |
| 自定义 socket 协议 | IPC 走 NVMe SQ/CQ 模型，为 kmod 积累经验 |
| 多端共享的 I/O 层抽象 | `io_backend.h` 已经够薄，算法层不需要再包一层 |
| 在线事务/日志 | v1 不做 |
| 动态 inode 分配 | mkfs 静态分配 + 离线扩容 |
| 在线碎片整理 | v1 不做 |

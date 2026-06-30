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

## Phase 1 — 上帝类（静态库）+ 外部测试程序

**目标**：交付 `libravenfs.a`（静态链接的上帝类库），外部测试程序链接后即可挂载、操作文件系统。

### 交付物

| 产物 | 说明 |
|------|------|
| `libravenfs.a` | 上帝类静态库，内嵌极简 VFS + 多线程安全 |
| 外部测试程序 | 任何外部 `main()` cpp 链接 `libravenfs.a`，`mount()` 后即可操作 |

### 策略

- **所有算法内聚**在 `RavenFS` 一个类中，不拆分任何抽象层
- **极简 VFS 内嵌**在 god class 中：`open`/`close`/`read`/`write`/`create`/`delete` 是类的方法，不是外部层
- **多线程安全**从一开始就设计：
  - 文件句柄表（`FileID` → 上下文）必须支持并发访问
  - B+tree 操作需要明确的锁粒度决策（inode 级锁？btree node 级锁？）
  - 缓存一致性：私有 buffer cache 的并发读写
- `io_backend.h` 仅用于切换 I/O 后端（host = pread, sparrowos = NVMe 驱动），不是算法抽象
- 研究目标不只是文件系统，更是 **VFS 的并发设计**（句柄管理、路径解析的锁、并发 open/close 的语义）

### 测试集

外部测试程序（任意 `main()`，链接 `libravenfs.a`）：

```cpp
#include "ravenfs/ravenfs.h"

int main() {
    PosixIOBackend io("/dev/nvme0n1pX");
    RavenFS fs;
    fs.mount(&io);

    // 基础操作
    fs.create_file({0,0}, "/hello.txt");
    FileID f = fs.open({0,0}, "/hello.txt");
    fs.write(f, "hello world", 11, 0);
    char buf[32];
    fs.read(f, buf, 11, 0);
    fs.close(f);

    // 目录
    fs.mkdir({0,0}, "/subdir");
    fs.create_file({0,0}, "/subdir/a.txt");
    RavenFS::DirEntry entries[16];
    int n = fs.readdir("/subdir", entries, 16);

    // 随机读写
    FileID f2 = fs.open({0,0}, "/bigfile.bin");
    fs.read(f2, buf, 512, 1048576);  // offset 1MB
    fs.write(f2, buf, 512, 2097152); // offset 2MB
    fs.close(f2);
}
```

覆盖场景：

| # | 场景 | 验证点 |
|---|------|--------|
| 1 | mkfs + 挂载 | 布局正确，superblock 校验 |
| 2 | 创建单文件，写 inline data | inode 分配、dentry、inline 路径 |
| 3 | 跨 extent 大文件读/写 | B+tree 多节点遍历 |
| 4 | 创建/删除目录 | dentry B+tree 增删 |
| 5 | 目录枚举 | readdir 正确性 |
| 6 | 随机偏移读写 | extent 查找 + 部分块 I/O |
| 7 | 空洞文件 | 未分配 extent 返回零 |
| 8 | 碎片化写入 | B+tree 分裂/合并 |
| 9 | 重命名 | dentry 更新 |
| 10 | **多线程并发** | 多线程同时 open/read/write/close |
| 11 | **压力测试** | 大量小文件创建/删除，句柄表耗尽/回收 |

### 测试战场

- **主战场**：`/dev/nvme0n1pX`（01:00.0 实验盘）
  - 双用途：既是 RavenFS 的测试盘，也是 SparrowOS NVMe 驱动的压力测试场
- **备选**：`/tmp/test.img` loop 文件（快速迭代算法时用）

### 里程碑

```
libravenfs.a 编译通过
外部测试程序链接成功
单线程全部操作正确
多线程并发无数据损坏
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

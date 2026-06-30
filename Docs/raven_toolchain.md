# RavenFS Toolchain

> 五个编译目标，一套磁盘布局。
> 配套阅读: `disk_layout_v1.md`, `disk_struct.h`
>
> 所谓 toolchain 并非编译工具链——是让 RavenFS 接入各端 VFS 的**实现手段集合**。
> 与项目 CMake / 链接器等构建工具链无关。

## 全端概况

| 端 | 运行环境 | 语言 | 用途 | 缓存策略 |
|:---|:---------|:-----|:-----|:---------|
| **mkfs.ravenfs** | Linux userspace | C++ | 格式化分区 | 无需持久缓存，单次顺序操作 |
| **expandravenfs** | Linux userspace | C++ | 离线扩容 + 附只读诊断 | 调试状态可复现 1:1 |
| **ravendsrv** | Linux userspace | C++ | 用户态 VFS 守护进程；直接读写分区镜像，对外暴露文件操作接口 | 私有 buffer cache |
| **ravenfs.ko** | Linux kernel | C | 挂载读写 | 私有 buffer cache（不走 page cache） |
| **SparrowOS** | 自有 UEFI 内核 | C++ | 挂载读写 | 私有 buffer cache |

### 用户空间三端的递进关系

```
mkfs.ravenfs       — 写盘布局，只是格式化
     ↓
expandravenfs      — 离线扩容（元数据搬迁：bitmap + inode array）
                    附只读诊断功能
     ↓
ravendsrv          — 完整读写，用户态开发内核态文件系统逻辑
                    内核端的前体开发平台（算法在此验证）
```

---

## 依赖顺序

```
                    ┌──────────────┐
                    │  disk_struct │
                    │  (全端共用)   │
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │   Superblock  │
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │    Bitmap     │
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │  Inode Array  │
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │  B+tree 操作   │
                    │  (按协议各自实现)│
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │  Inode 操作   │
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │   Dentry    │
                    └──────┬───────┘
                           ↓
          ┌────────────────┼────────────────┐
          ↓                ↓                ↓
   ┌────────────┐   ┌────────────┐   ┌────────────┐
   │ ravendsrv  │   │ ravenfs.ko │   │ SparrowOS  │
   │ 用户态 VFS  │   │ Linux  VFS │   │ 自有 VFS   │
   └────────────┘   └────────────┘   └────────────┘
   ↑            ↑
   │            │
   │     ┌──────────────┐
   │     │ expandravenfs │
   │     │ (离线扩容+诊断)│
   │     └──────────────┘
   │
   ┌──────────────┐
   │  mkfs.ravenfs │
   └──────────────┘
```

---

## 起步策略

**先做 mkfs → ravendsrv → expandravenfs → ko / SparrowOS**

### 1. mkfs.ravenfs（最轻量，最先做）

- `pread`/`pwrite` 裸分区或 loopback 文件
- 只写，不需要缓存
- B+tree 只需顺序构建能力（写根 inode + 初始化空目录）
- 不需要实现 insert/delete 全套
- 测试: `sfdisk` + `losetup`

### 2. ravendsrv（核心开发平台）

- 文件 + 分区直读模式的用户态进程
- **开发运行时**，不依赖内核模块
- 实现完整的 B+tree、私有缓存、inode/dentry 操作
- 借用指针式缓存在这里做验证
- 对外暴露接口的方案待定：
  - 自定义 socket / unix domain socket 协议
  - FUSE 挂载
  - TLV 协议 + 本地端口供 SparrowOS mock 测试用

### 3. expandravenfs（扩容工具 + 只读诊断）

- 主功能: 离线扩容 — 搬移 bitmap 和 inode array 到扩容后的尾巴位置
- 附功能: 只读诊断 — 检查磁盘布局一致性，展示 superblock/inode/B+tree/bitmap 统计
- 用于调试 mkfs 输出和 ravendsrv 的修改是否破坏布局
- 类似 ext4 的 `resize2fs` + `debugfs` 二合一

### 4. ravenfs.ko（Linux 内核端）

- 注册 `file_system_type` → `fill_super`
- 私有 buffer cache（`kmalloc` + `bio_alloc` + `submit_bio_wait`）
- B+tree 算法从 ravendsrv 移植（替换 I/O 层）
- VFS 回调接入

### 5. SparrowOS（自有内核端）

- 复用 `disk_struct.h`，布局同 ravenfs.ko
- 缓存走自有分配器 + 自有 NVMe 驱动
- B+tree 算法逻辑同 ravenfs.ko（替换 load_node / mark_dirty / submit_io 的实现）
- VFS 层按 SparrowOS 自有接口实现

---

## 目录结构

```
src/include/ravenfs/
  disk_struct.h              ✓ 已有 — 全端共用

Docs/ravenfs/
  disk_layout_v1.md          ✓ 已有
  raven_toolchain.md         ← 本文

raid/                        — 将来可能迁移到独立仓库
  mkfs.ravenfs/               Linux userspace 格式化工具
  expandravenfs/              Linux userspace 离线扩容 + 诊断工具
  ravendsrv/                  Linux userspace 守护进程（用户态 VFS）
  ravenfs.ko/                 Linux kernel module
  sparrowos/                  SparrowOS 端（或内联进 kernel 镜像）
```

每端实现内部建议模块划分（各端按需调整）：

```
    ├── main.c         入口 / CLI 解析
    ├── super.c        superblock 读/写/校验
    ├── bitmap.c       block allocator
    ├── inode.c        inode array + inode 操作
    ├── btree.c        B+tree 查找/插入/删除/分裂/合并
    ├── cache.c        私有 buffer cache（mkfs/expandravenfs 可无）
    ├── dentry.c       dentry B+tree 操作
    ├── vfs_ops.c      VFS 回调 / 对外接口（ko→file_operations, server→socket, OS→自有）
    └── disk_struct.h  软链接或头文件副本
```

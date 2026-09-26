# RavenFS — 多端用户态文件系统

RavenFS — 基于 B+tree 的 block-level 文件系统，面向三端适配：

| 端 | 路径 | 用途 |
|----|------|------|
| **host** | `host/` | Linux 用户态：上帝类开发 + FUSE 守护进程 |
| **sparrowos** | `sparrowos/` | SparrowOS 内核模块（首个 kmod 候选） |
| **linux** | `linux/` | Linux kernel module（远期） |

核心算法（B+tree、inode、bitmap、dentry）全在 `core/`，三端共享。

---

## 开发路线

```
Phase 0: mkfs.ravenfs
Phase 1: libravenfs.a — 上帝类静态库 + 外部测试程序
Phase 2: ravendsrv — io_uring + NVMe SQ/CQ IPC
Phase 3: ravenfs.ko + SparrowOS kmod
```

详见 `Docs/linux/ravenfs-linux-ko-requirements-spec.md`（现行需求规格；原 `Docs/plan.md` 已删）。

## 构建

```bash
# core 静态库
cd host && mkdir build && cd build
cmake .. && make
# libravenfs.a → 外部测试链接

# SparrowOS kmod (TODO)
# Linux ko (TODO)
```

## 设计参考

- `Docs/disk_layout_v1.md` — 磁盘布局
- `Docs/disk_layout_v1.2_inode.md` — v1.2 inode 盘面（权威）
- `src/include/ravenfs/disk_struct.h` — 全端共用数据结构

## 设计参考

- `Docs/ravenfs/disk_layout_v1.md` — 磁盘布局
- `src/include/ravenfs/disk_struct.h` — 全端共用数据结构
- 设计哲学习自 SparrowOS init_fs_t 上帝类过渡路线

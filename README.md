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
Phase 1: 上帝类 (core/ + host/cli)
   裸 CLI 测试，直接 new RavenFS → mkfs → create → read → assert
   FUSE 不介入

Phase 2: FUSE 导出
   ravendsrv = 薄映射层挂到 /mnt/raven
   用标准工具 (ls/cp/cat) 压算法

Phase 3: 各端拆分
   core 算法不变，各端换 I/O 后端
```

## 构建

```bash
# host 端
cd host && mkdir build && cd build
cmake .. && make
./ravenfs_cli /tmp/test.img    # 裸 CLI 测试

# SparrowOS kmod (TODO)
# Linux ko (TODO)
```

## 设计参考

- `Docs/ravenfs/disk_layout_v1.md` — 磁盘布局
- `src/include/ravenfs/disk_struct.h` — 全端共用数据结构
- 设计哲学习自 SparrowOS init_fs_t 上帝类过渡路线

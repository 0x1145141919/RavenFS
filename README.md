# RavenFS — 多端用户态文件系统

RavenFS — 基于 B+tree 的 block-level 文件系统，面向三端适配：

| 端 | 路径 | 用途 |
|----|------|------|
| **host** | `host/` | Linux 用户态：上帝类开发 + FUSE 守护进程 |
| **sparrowos** | `sparrowos/` | SparrowOS 内核模块（首个 kmod 候选） |
| **linux** | `linux/` | **Linux 内核模块（当前焦点）**——见 `linux/README.md` |

核心算法（B+tree、inode、bitmap、dentry）全在 `core/`，三端共享。

---

## 开发路线

```
Phase 0: mkfs.ravenfs
Phase 1: libravenfs.a — 上帝类静态库 + 外部测试程序
Phase 2: ravendsrv — io_uring + NVMe SQ/CQ IPC
Phase 3: ravenfs.ko + SparrowOS kmod
```

> **2026-09-29 质变**：Linux KO 子项目确立**存在形式 = pacman 包锚点**（包只部署“源代码 + 构建脚本”，运行期钩子/工具均为外部配置）——详见 **`linux/README.md`**（门面）；局部纪律（三层约束 / 小步快跑 / 上线闸门）见 `Docs/linux/LINUX_KO_DISCIPLINE.md`。

详见 `linux/README.md`（Linux KO 子项目门面：存在形式 + 构建/安装）与 `Docs/linux/LINUX_KO_DISCIPLINE.md`。

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

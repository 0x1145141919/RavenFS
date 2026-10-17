# RavenFS Linux KO · mdata 缓存强淘汰 ioctl 契约（v1）

> **状态**：已实现（2026-10-17）；本文件是该 ioctl 的**唯一对外契约**。
> **代码入口**：`include/linux/ravenfs_mdata_ioctl.h`（UAPI）+ `linux/mdata_cache_ioctl.c`（处理器）
> → `linux/mdata_cache_core.c`（强淘汰算法；处理器直接以 `sbi->mcache` 调用核心）。
> **规范用语**：**必须**（MUST）/ **禁止**（MUST NOT）/ **应当**（SHOULD）/ **可以**（MAY）。
> **适用范围**：RavenFS Linux KO 模块（`ravenfs.ko`）的 mdata 8K 节点缓存；
> 不适用于用户态 god 原型，也不适用于旧 xarray 实现（已退役）。

---

## 0. 一页速览

| 项 | 值 |
|---|---|
| 命令 | `RAVENFS_IOC_CACHE_SQUEEZE = _IOWR('R', 0x20, struct ravenfs_cache_squeeze_arg)` |
| 作用域 | **整个挂载实例**（FS-wide），不是单文件/单 inode |
| 作用对象 | mdata 8K 节点缓存行 + RADIX 分片的空翻译表 |
| 权限 | 全局 `CAP_SYS_ADMIN`（`capable()`；通常 root） |
| 句柄 | 挂载点内任意已打开的文件或目录 fd |
| 档位 | `PRESSURE=0`（无视 TTL）/ `LOWLOAD=1`（按 TTL 截止时间筛冷行） |
| 时间基准 | `ttl_ns` 是 **CLOCK_MONOTONIC 绝对截止时间戳（ns）**，不是“空闲时长” |
| 只读挂载 | `-EROFS`，不做任何动作 |
| 典型错误 | `-EPERM` / `-EFAULT` / `-EINVAL` / `-EROFS` / `-ENOTTY` / `-EIO` |
| 结果 | `rows_reaped` / `tables_freed` / `table_bytes_freed` / `write_errors` |
| 幂等 | 是；可重复调用，无累积状态 |
| 自动触发 | 无；`PRESSURE`/`LOWLOAD` 均由用户态或未来内核编排显式调用 |

---

## 1. 目的与非目标

### 1.1 目的

在系统低载或内存压力时，对 FS 级 mdata 节点缓存执行一次**强淘汰（squeeze）**：

- 回收 `ref==0` 的缓存行（8K 节点缓冲）；
- 脏行先写回再释放，避免丢更新；
- RADIX 档在行回收后**精简翻译结构**：释放所有行均为 `IDLE` 的空叶表/空内部表/空根表；
- 通过 ioctl 把结果计数返回用户态，便于守护进程/运维决策。

### 1.2 非目标

- **不是 `syncfs`**：不负责 inode 数据面、block 位图、Superblock 的全量落盘；
- **不是 shrinker**：本版本不注册内核内存回收器，也不自动触发；
- **不是 per-inode/per-key 操作**：不能指定文件、inode、分片或 key 范围；
- **不保证回收全部行**：`ref>0`、`IO_ING`、`EVICTING`、`F_WB` 的行必须跳过；
- **不提供 `NOWRITE` 档**：只读挂载直接拒绝，不回收干净行；
- **不保证时延**：强淘汰是低频重操作，可能遍历所有分片并回写脏行；
- **不是 ABI 之外的性能计数器查询口**：结果只含本次调用的四个计数。

---

## 2. 分层与实现映射

```
用户态
  ioctl(fd, RAVENFS_IOC_CACHE_SQUEEZE, &arg)
      │
      ▼
linux/super.c  fops: .unlocked_ioctl = ravenfs_mdata_ioctl
      │            .compat_ioctl   = compat_ptr_ioctl
      ▼
linux/mdata_cache_ioctl.c  ravenfs_mdata_ioctl()
      │  权限 / cmd / copy_from_user / 参数 / ro / shutdown / 取 sbi->mcache
      ▼
linux/mdata_cache_core.c  ravenfs_mdata_reap_expired_strong(c, gear, ttl_ns, out)
      │  两阶段：行回收（片读锁 + CAS）→ 表精简（片写锁 + 后序 DFS）
      ▼
mdata 8K 缓存行 / RADIX 静态基数树
```

**禁止**绕过本契约直接调用 `ravenfs_mdata_reap_expired_strong()` 之外的内部函数；
内核内其他编排点（未来 shrinker / 空闲 workqueue）**必须**直接复用核心
`ravenfs_mdata_reap_expired_strong()`（缓存对象经 `sbi->mcache` 取得），
不得自行解析用户结构。

---

## 3. UAPI 定义（规范）

### 3.1 命令

```c
#include <linux/ioctl.h>
#include <linux/types.h>

#define RAVENFS_IOC_CACHE_SQUEEZE \
        _IOWR('R', 0x20, struct ravenfs_cache_squeeze_arg)
```

- 方向：`_IOWR`（内核先读入参，再把结果写回同一结构）。
- 魔数：`'R'`；序号：`0x20`。
- 大小：`sizeof(struct ravenfs_cache_squeeze_arg) == 48`。
- **禁止**在用户态硬编码命令数值；必须使用宏。命令数值随 `_IOC` 编码生成，
  不应作为 ABI 常量传播。

### 3.2 参数/结果结构

```c
struct ravenfs_cache_squeeze_arg {
    __u32 gear;               /* in  */
    __u32 flags;              /* in  */
    __u64 ttl_ns;             /* in  */
    __u64 rows_reaped;        /* out */
    __u64 tables_freed;       /* out */
    __u64 table_bytes_freed;  /* out */
    __u64 write_errors;       /* out */
};
```

固定布局（**v1 冻结**）：

| 偏移 | 大小 | 字段 | 方向 | 含义 |
|---:|---:|---|---|---|
| 0 | 4 | `gear` | in | `0=PRESSURE`, `1=LOWLOAD`，其他 `-EINVAL` |
| 4 | 4 | `flags` | in | **必须为 0**；非 0 `-EINVAL`（v1 无定义位） |
| 8 | 8 | `ttl_ns` | in | `LOWLOAD` 的 CLOCK_MONOTONIC 绝对截止时间戳；`0` = 不做 TTL 过滤；`PRESSURE` 忽略 |
| 16 | 8 | `rows_reaped` | out | 本次成功回收到 `IDLE` 的行数 |
| 24 | 8 | `tables_freed` | out | 本次摘除的空叶表/空内部表/空根表数；CTRL_ARRAY 恒 0 |
| 32 | 8 | `table_bytes_freed` | out | 本次摘除的**表结构**字节数；不含 8K 数据缓冲 |
| 40 | 8 | `write_errors` | out | 脏行写回失败数；`>0` 时返回 `-EIO` |

- 所有字段使用本机字节序；结构无指针、无填充洞。
- 32/64 位布局一致（字段均为 `__u32/__u64`，偏移如上），
  可走 `.compat_ioctl = compat_ptr_ioctl`；**不要求**第二套 compat 结构。
- **禁止**在 v1 命令上扩展字段或改变偏移；新增能力必须：
  1. 使用新的 ioctl 命令/魔数，或
  2. 使用 `flags` 的新定义位（并在语义上保持旧 `flags=0` 行为不变）。
- v1 的 `flags` **必须为 0**；未知位返回 `-EINVAL`。

### 3.3 档位常量

| 值 | 名称 | 语义 |
|---:|---|---|
| 0 | `PRESSURE` | 内存压力：**无视 TTL**，回收所有可回收行 |
| 1 | `LOWLOAD` | 低载主动：只回收 `acc_stamp < ttl_ns` 的冷行 |

用户态应把它们视为不透明整数；禁止把内核 `enum ravenfs_mdata_squeeze_gear`
直接暴露到 UAPI 之外。

---

## 4. 语义（规范）

### 4.1 统一行选择条件

对每个 mdata 缓存行，**必须**同时满足：

1. `state == READY`（⇄ 用户态 god 的 `working`）；
2. `ref == 0`（无活跃会话持有）；
3. `!(flags & F_WB)`（无在飞回写快照）；
4. 若 `gear == LOWLOAD` 且 `ttl_ns != 0`：`acc_stamp < ttl_ns`；
5. 若 `gear == PRESSURE`：不检查 `acc_stamp`。

不满足的行**必须**保持原状（不释放、不写回、不改变 `ctrl`）。

> `acc_stamp` 语义 = 最近一次 `get` 命中或装载的时间（ns），
> `mark_dirty` **不刷新**它。因此 `LOWLOAD` 的“冷”是**访问/装载冷**，
> 不是“最近写入冷”。脏冷行会被写回后释放，这是允许的。

### 4.2 `PRESSURE`（0）

- `ttl_ns` **必须被忽略**；
- 所有满足 4.1 条件 1~3 的行都**必须**被回收（含脏行、热行）；
- 目的：模拟内存压力下的“全踹”。

### 4.3 `LOWLOAD`（1）

- `ttl_ns != 0`：只回收 `acc_stamp < ttl_ns` 的行；
- `ttl_ns == 0`：**不做 TTL 过滤**，行选择等价 `PRESSURE`（但仍走 LOWLOAD 档；
  该语义便于“现在清一次”而无需计算时钟）；
- 热行（`acc_stamp >= ttl_ns`）**必须**保留。

`ttl_ns` 是**绝对截止时间戳**，不是年龄。用户态计算方式：

```c
/* 回收“空闲超过 age_ns”的行： */
u64 now = clock_gettime_ns(CLOCK_MONOTONIC);
u64 cutoff = now - age_ns;
```

### 4.4 脏行处理

对每个被选中行：

1. 行状态必须先 CAS `READY → EVICTING`（保留 `flags/ref` 位）；
2. 若 `F_DIRTY` 置位：
   - **必须**先把 8K 内容写回盘（`io.write_block`，尾部节点按 `total_blocks` 截断）；
   - 写回成功：清 `F_DIRTY`、归还 8K 缓冲、回 `IDLE`，`rows_reaped++`；
   - 写回失败：恢复 `READY|F_DIRTY`、**保留** 8K 缓冲、`write_errors++`，
     继续处理其他行；本次调用最终返回 `-EIO`；
3. 若非脏：直接归还 8K 缓冲、回 `IDLE`，`rows_reaped++`。

**禁止**在写回失败时丢弃脏行，也**禁止**把失败行计为 `rows_reaped`。
写回成功的每个块按适配层现有语义 `mark_buffer_dirty + sync_dirty_buffer`，
即“返回即该块已提交”；但本 ioctl **不**负责 FS 其他元数据/位图/SB 的同步。

### 4.5 表精简（翻译结构收缩）

仅 `RAVENFS_MDATA_MODE_RADIX`（节点数 > 8192，bits 14~52）执行：

1. 行回收阶段结束后，逐分片取**片写锁**；
2. 后序 DFS：
   - 叶表：所有槽 `state == IDLE` → 空；释放叶表；
   - 内部表：所有子槽 `NULL` → 空；释放内部表；
   - 根表为空 → 释放根表并置 `shard->root = NULL`；
3. **非空表绝对禁止释放**：只要还有一个 `READY/IO_ING/EVICTING` 行或非空子表，
   该表及其祖先必须保留；
4. 被摘除的路径由后续 `get` 的 `ensure_path` 重新补建；这是预期行为（H6），
   不视为错误，也不保证 `get` 不重试；
5. `CTRL_ARRAY` 档（bits ≤ 13）的扁平 row 数组**不参与**表精简；
   `tables_freed` 与 `table_bytes_freed` 恒为 0。

`table_bytes_freed` 按当前几何现算：

```
叶表   = leaf_width × sizeof(struct ravenfs_mrow_simple)   /* 64B/槽 */
内部表 = 2^level_bits[1+level] × sizeof(void *)
```

它是**信息性计数**，不是分配器账本；不含行内 8K 数据缓冲，也不含 `shards` 数组。

### 4.6 结果计数

| 字段 | 精确含义 |
|---|---|
| `rows_reaped` | 本次**成功**从 `READY` 走到 `IDLE` 的行数；写回失败/跳过的不计 |
| `tables_freed` | 本次释放的空表总数（叶+内部+根）；CTRL_ARRAY 恒 0 |
| `table_bytes_freed` | 上述表结构的几何字节数；CTRL_ARRAY 恒 0 |
| `write_errors` | 本次脏行写回失败次数；`>0` ⇒ 返回 `-EIO` |

- `rows_reaped == 0` **不表示**调用失败；可能所有行都被 `ref>0/IO_ING/EVICTING/F_WB`
  或 TTL 条件跳过。
- `tables_freed > 0` 可以在 `rows_reaped == 0` 时发生：此前已经 `IDLE` 的空表被回收。
- 结果字段在 **adapter 被调用后**（含 `-EIO`）必须回填；前置校验错误不回填（见 §6）。

### 4.7 幂等与重复调用

- 本 ioctl **没有**跨调用累积状态；同一参数重复调用是安全的；
- 第二次调用可能回收第一次之后新装载/新变冷的行，也可能什么都不做；
- 表被摘除后再次 `get` 会重建，重复调用不会破坏缓存一致性。

---

## 5. 前置条件、权限与生命周期

### 5.1 句柄

- `fd` **必须**是挂载点内任意已打开的**普通文件或目录**；
  两种 fops 均挂载 `.unlocked_ioctl`。
- 内核通过 `file_inode(file)->i_sb` 取得 FS-wide `super_block`；
  ioctl 作用域是**整个挂载实例**。
- 打开 fd 本身会持有 superblock 引用，因此用户态 ioctl **不会**与
  `put_super`/`ravenfs_mdata_cache_detach()` 并发（VFS 保证卸载发生在所有
  fd 关闭之后）。

### 5.2 权限

- **必须**具备全局 `CAP_SYS_ADMIN`（`capable()`，即 init user namespace 的
  capability；不是 `ns_capable(sb->s_user_ns, ...)`）。
- 不满足 → `-EPERM`。
- 权限检查在 `cmd` 分发之前：非 root 对未知命令也返回 `-EPERM`；
  root 对未知命令返回 `-ENOTTY`（见 §6 顺序）。

### 5.3 状态闸

按以下顺序检查（与实现逐条对应）：

1. `file_inode(file)` 或其 `i_sb` 为空 → `-EINVAL`；
2. `sb->s_fs_info` 为空（卸载窗口） → `-EIO`；
3. 非 `CAP_SYS_ADMIN` → `-EPERM`；
4. `ravenfs_is_shutdown(sb)`（R-LM-09 force-shutdown） → `-EIO`；
5. 未知命令 → `-ENOTTY`；
6. `copy_from_user` 失败 → `-EFAULT`；
7. `gear` 非法或 `flags != 0` → `-EINVAL`；
8. `sb_rdonly(sb)` → `-EROFS`（**即使**当前没有脏行；v1 保守拒绝）；
9. `sbi->mcache` 为空 → `-EIO`；否则调核心
   `ravenfs_mdata_reap_expired_strong()`;
10. 回填结果并 `copy_to_user`，返回核心的返回值。

### 5.4 只读/停机

- 只读挂载（含 remount-ro）：**必须**返回 `-EROFS`，**禁止**进入行回收/表精简；
- force-shutdown：**必须**返回 `-EIO`，**禁止**写盘；
- 正常读写卸载：由 `put_super → ravenfs_mdata_cache_detach()` 负责最终
  `flush_all`；本 ioctl **不**是卸载同步点。

---

## 6. 错误码与出参回填矩阵

### 6.1 错误码

| 返回 | 条件 | 出参 `rows_reaped/...` 是否写回 | 是否可能已改动缓存 |
|---|---|---|---|
| `-EINVAL` | `file_inode`/`i_sb` 空；`gear>1`；`flags!=0` | **否** | 否 |
| `-EIO` | `s_fs_info` 空；shutdown；adapter 无 mcache；adapter 写回失败 | 前两者**否**；adapter 失败**是** | adapter 失败时可能已部分回收 |
| `-EPERM` | 非 `CAP_SYS_ADMIN` | **否** | 否 |
| `-ENOTTY` | root 且 `cmd` 未知 | **否** | 否 |
| `-EFAULT` | `copy_from_user` 失败；或 `copy_to_user` 失败 | 前者**否**；后者可能部分写回 | copy_to_user 失败时缓存动作已发生 |
| `-EROFS` | `sb_rdonly`；或 adapter 在竞态 remount-ro 后返回 | 前置**否**；adapter 竞态**是** | 前置否；adapter 竞态否（adapter 在动作前拒绝） |
| `0` | 成功（可能回收 0 行） | **是** | 可能已改动缓存 |

> **规范**：只有 **adapter 已被调用** 的路径才保证回填四个 out 字段。
> 前置校验错误（权限/参数/ro/shutdown/未知命令/`copy_from_user`）**不保证**
> 写用户结构；调用方应把 out 字段初始化为 0，并在失败时按错误码解释。
> `copy_to_user` 失败时内核返回 `-EFAULT`，用户内存可能部分更新。

### 6.2 错误优先级

按 §5.3 顺序；例如：

- 非 root + `gear=2` → `-EPERM`（不是 `-EINVAL`）；
- root + 未知命令 → `-ENOTTY`（不是 `-EINVAL`）；
- 只读 + `flags=1` → `-EINVAL`（参数检查先于 ro）；
- 只读 + 合法参数 → `-EROFS`（在 adapter 前拒绝，out 不回填）。

---

## 7. 并发契约

### 7.1 可并发的对象

本 ioctl **可以**与以下路径并发，实现必须保证不 UAF/不破坏行状态：

- `ravenfs_mdata_get/put/read`（会话装载/释放/读）；
- `RAVENFS_MDATA_DIRTY`（会话标脏；片读锁与回写片写锁互斥）；
- `ravenfs_mdata_wb_dirty`（周期回写；片写锁）；
- `ravenfs_mdata_reap_expired`（普通 TTL；片读锁 + 行 CAS）；
- `ravenfs_mdata_drop`（块复用前丢弃；片读锁 + CAS）。

### 7.2 锁序

- 行回收阶段：**片读锁**内 DFS + 行 CAS `READY → EVICTING`；8K 写回/归还
  **必须在片锁外**完成；
- 表精简阶段：**片写锁**内后序 DFS；只有整表全 `IDLE` 才释放；
- 行回收阶段 CAS 到 `EVICTING` 的行会钉住其叶表，表精简看到非 `IDLE`
  必须保留该表及其祖先；
- `get` 的 `locate` 在片读锁内完成；命中行以 `ref>0` / `IO_ING` / `EVICTING`
  钉住叶表；表被摘后 `get` 走 `ensure_path` 重建（可能重试，H6）。

### 7.3 禁止的并发

- **禁止**与 `ravenfs_mdata_cache_fini()` / `ravenfs_mdata_cache_detach()`
  并发。用户态 ioctl 由 fd 生命周期天然排除；内核侧调用方（未来 shrinker/
  workqueue）**必须**在卸载前停止。
- **禁止**从同一分片的片写锁内部再调用本 ioctl 的同步入口（会自死锁）；
  正常的用户态 ioctl 不持有任何缓存锁。
- **禁止**把本 ioctl 当实时接口；`wb_dirty/wb_node` 仍持片写锁做 I/O，
  强淘汰本身也可能回写脏行。

---

## 8. 统计与可观测

| 事件 | 统计钩子 | 说明 |
|---|---|---|
| 成功回收行 | `RAVENFS_MD_REAP_STRONG`（`sbi->stats.md_reap_strong`） | strong 专用，与普通 TTL 的 `md_reap` 分离 |
| 脏行写回成功 | `RAVENFS_MD_NODE_WB`（`md_node_wb`） | 每行一次 |
| 脏行写回失败 | `RAVENFS_MD_ERR`（`md_err`） | 每失败一次 |
| 表释放 | `RAVENFS_MD_TABLE_FREE`（`md_table_free`） | 强淘汰摘除的空表数 |
| 表字节回收 | `RAVENFS_MD_TABLE_DEL_BYTES`（`md_table_del_bytes`） | 与 `md_table_add_bytes` 作差得驻留量 |

- `md_reap_strong` 与 `md_reap` 分列；统计面已与 god `mdp_stat` 全量对位。
- `table_bytes_freed` 为几何估算，不是分配器账本；不要用它做精确内存对账。
- dmesg：写回失败由核心 `RAVENFS_MD_ERR`/适配层错误路径记录；
  ioctl 本身以 `pr_debug_ratelimited` 记录结果（默认不刷 dmesg）。

---

## 9. 调用示例

### 9.1 C 用户态

```c
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "ravenfs_mdata_ioctl.h"

static unsigned long long mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(int argc, char **argv)
{
    struct ravenfs_cache_squeeze_arg a = { 0 };
    int fd, ret;

    if (argc < 3)
        return 2;
    a.gear = !strcmp(argv[2], "pressure")
             ? RAVENFS_CACHE_SQUEEZE_PRESSURE
             : RAVENFS_CACHE_SQUEEZE_LOWLOAD;
    /* LOWLOAD：回收空闲超过 2s 的行；PRESSURE 忽略 */
    a.ttl_ns = mono_ns() - 2ULL * 1000000000ULL;

    fd = open(argv[1], O_RDONLY);
    if (fd < 0)
        return 1;
    ret = ioctl(fd, RAVENFS_IOC_CACHE_SQUEEZE, &a);
    printf("ret=%d rows=%llu tables=%llu bytes=%llu wb_err=%llu\n",
           ret, (unsigned long long)a.rows_reaped,
           (unsigned long long)a.tables_freed,
           (unsigned long long)a.table_bytes_freed,
           (unsigned long long)a.write_errors);
    close(fd);
    return ret < 0 ? 1 : 0;
}
```

编译（仓库内）：

```sh
gcc -std=gnu11 -Wall -Wextra -Werror -I include/linux \
    tools/rfs_cachectl.c -o rfs_cachectl
```

### 9.2 现成 CLI

```sh
# 压力档：回收所有可回收行（无视 TTL）
rfs_cachectl /mnt/ravenfs pressure

# 低载档：只回收空闲超过 2s 的冷行（ttl 是绝对 cutoff）
cutoff=$(python3 -c 'import time; print(time.clock_gettime_ns(time.CLOCK_MONOTONIC)-2_000_000_000)')
rfs_cachectl /mnt/ravenfs lowload "$cutoff"

# 低载档且不按 TTL 过滤（会回收热行，谨慎）
rfs_cachectl /mnt/ravenfs lowload 0
```

---

## 10. 验收矩阵

### 10.1 已完成的构建/单测

| 项 | 结果 |
|---|---|
| 核心单测 `tests/test_mdata_cache_core.c` | 874 pass / 0 fail |
| ASan + UBSan（leak detect） | 874 pass / 0 fail |
| TSan（并发烟测） | 874 pass / 0 fail，无 race |
| UAPI 头用户态编译（`sizeof == 48`） | 通过 |
| `tools/rfs_cachectl.c` `-Werror` 编译 | 通过 |
| 独立 kbuild 烟测 | `rfs_mdata_probe.ko` 构建成功 |
| 生产模块 `make -C linux` | `ravenfs.ko` 构建成功，无 warning/undefined/duplicate |

### 10.2 运行时验收（待执行）

在 QEMU guest 内（`linux/vm`）执行：

1. `insmod ravenfs.ko` → `/proc/filesystems` 出现 `ravenfs`；
2. `mkfs.ravenfs` + `mount -t ravenfs`；
3. 产生冷/热/脏/被引用行（文件读写 + 目录操作）；
4. `rfs_cachectl <mnt> lowload <cutoff>` → 冷行回收、热行保留、脏行落盘；
5. `rfs_cachectl <mnt> pressure` → 0-ref 行回收、`tables_freed > 0`（RADIX）；
6. 非 root 调用 → `-EPERM`；ro 挂载 → `-EROFS`；未知命令 → `-ENOTTY`；
7. `dmesg` 无 UAF/警告；卸载后重挂载，数据/审计一致。

**在运行时验收完成前，本契约只能声明“构建/单测已通过”，不能声明“端到端已验证”。**

---

## 11. 已知限制（v1）

1. **只读挂载一律 `-EROFS`**：即使只回收干净行也拒绝；未来若需要“ro 只收缩干净行”，
   必须新增 flag/命令，不能改变 v1 语义。
2. **没有 `NOWRITE`**：不能只回收干净行、跳过写回。
3. **没有自动触发**：不注册 shrinker；`PRESSURE` 也需显式调用。
4. **`LOWLOAD` 的冷热按 `acc_stamp`**：`mark_dirty` 不刷新它；写多读少的行可能被判冷。
5. **`get` 在表被摘后可能重试/重建（H6）**：没有重试上界；持续强淘汰压力下
   读者可能短暂饥饿。
6. **`wb_dirty/wb_node` 仍持片写锁做 I/O**：强淘汰与周期回写可能造成分片级阻塞；
   不是实时接口。
7. **`table_bytes_freed` 非分配器账本**：只按几何计算表结构字节。
8. **`md_reap_strong` 已与普通 TTL 的 `md_reap` 分离**。
9. **compat 未 runtime 验证**：结构无指针、32/64 布局一致，理论上
   `compat_ptr_ioctl` 足够，但未跑 32 位用户态。
10. **运行时验收未执行**：见 §10.2。

---

## 12. 版本与变更纪律

| 版本 | 日期 | 变更 |
|---|---|---|
| v1 | 2026-10-17 | 首版：`RAVENFS_IOC_CACHE_SQUEEZE`；PRESSURE/LOWLOAD；结果四字段；表精简；ro/shutdown 闸 |

**变更纪律**：

- 任何改变 §3.2 结构布局/偏移/命令数值的改动都是 **ABI 破坏**，必须新命令；
- 任何改变 §4 行选择/写回/表精简语义的改动都必须更新本规格并提升版本；
- 用户态工具 **必须** 以本规格为准，不得依赖实现细节（例如 `reserved[0]` 链表、
  片锁顺序、state 数值）。
- 规格与代码冲突时：**以本规格为契约**，同时立即修代码或改规格，禁止二者长期漂移。

---

## 附录 A · 代码入口

| 层 | 文件 |
|---|---|
| UAPI | `include/linux/ravenfs_mdata_ioctl.h` |
| ioctl 处理器 | `linux/mdata_cache_ioctl.c` |
| fops 接线 | `linux/super.c`（`ravenfs_dir_operations` / `ravenfs_file_operations`） |
| FS 装配 | `linux/mdata_cache_adapter.c`（mount/umount 注入面，无运行期调用薄壳） |
| 核心算法 | `linux/mdata_cache_core.c`（`ravenfs_mdata_reap_expired_strong` / `ravenfs_mdata_discard_all`） |
| 核心契约 | `include/linux/ravenfs_mdata_cache_core.h` |
| 用户态工具 | `tools/rfs_cachectl.c` |
| 单测 | `tests/test_mdata_cache_core.c` |

## 附录 B · 与 god 的对应

| god 用户态 | ko v1 |
|---|---|
| `mdp_squeeze_gear_t`（PRESSURE/LOWLOAD） | `enum ravenfs_mdata_squeeze_gear` / UAPI 0/1 |
| `self_reliat_reap_expired_strong(gear)` | `ravenfs_mdata_reap_expired_strong(self, gear, ttl_ns, out)` |
| `ttl_ticks_`（TSC tick） | `ttl_ns`（CLOCK_MONOTONIC ns 绝对 cutoff） |
| 返回 void | 返回 0/-errno + `struct ravenfs_mdata_squeeze_res` |
| 写锁内整树挤水 | 行回收读锁 + CAS / 表精简写锁；I/O 在片锁外 |
| 空表回收 | 同；CTRL_ARRAY 不摘扁平数组 |
| 无 ioctl | `RAVENFS_IOC_CACHE_SQUEEZE` |

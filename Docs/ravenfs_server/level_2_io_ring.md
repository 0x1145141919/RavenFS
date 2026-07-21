# Level 2 — IO Ring 命令表

> 协议骨架见 `protocol.md`。本文档定义 IO Ring 的操作码与参数语义。
>
> opcode 从 0 起始，独立命名空间——IO 环只处理文件系统操作。

---

## 1. 环标识

```
shm_path: /tmp/ravenfsd_sessions/{session_id}/ring_{rid}
参数关联: 同一 session 下已创建的 canvas（数据缓冲区）
```

IO 命令不能直接寻址文件数据——所有数据引用通过**数据指针编码**间接指向 canvas。

**路径解析：** 所有涉及路径的 IO 命令遵循以下规则：
- 路径以 `'/'` 开头 → **绝对路径**，从根开始
- 路径不以 `'/'` 开头 → **相对路径**，基于当前 session 的 cwd 拼接
- cwd 可通过 Level 1 `CMD_SESSION_CHDIR` 设置，初始值为 `"/"`

---

## 2. 数据指针编码（重申）

```c
param[n] = encode_data_ptr(canvas_id, offset):
  bits [0:20]   = offset     // 21 bit, 最大 2MB-1 偏移
  bits [21:31]  = canvas_id  // 11 bit, 最大 2047 张画布
  bits [32:63]  = 0          // 上层不占用低 32 bit 之外

辅助宏：
  encode_data_ptr(canvas_id, offset)
  data_ptr_canvas(ptr)     → 提取 canvas_id
  data_ptr_offset(ptr)     → 提取 offset
```

当 param 不引用画布数据时，直接使用 64 bit 整数值（如 uid、ino、size）。
本文档用 `dp(canvas_id, offset)` 或 `dp(...)` 标记数据指针参数。

---

## 3. 命令槽结构

```c
struct command_common_t {
    uint32_t opcode;         // Level 2 专用 opcode
    uint32_t res_code;       // 见各命令；≥0 = 成功值，<0 = -errno
    uint64_t param[6];       // 6 × 8 = 48B 参数
    uint64_t reserved;       // 扩展预留
};
```

---

## 4. 操作码枚举

```c
enum IORingOpcode : uint32_t {
    CMD_NOP        = 0,
    CMD_MKDIR      = 1,
    CMD_RMDIR      = 2,
    CMD_CREATE     = 3,
    CMD_UNLINK     = 4,
    CMD_OPEN       = 5,
    CMD_CLOSE      = 6,
    CMD_READ       = 7,
    CMD_WRITE      = 8,
    CMD_STAT       = 9,
    CMD_FSTAT      = 10,
    CMD_READDIR    = 11,
    CMD_RENAME     = 12,
    CMD_TRUNCATE   = 13,
    CMD_FSYNC      = 14,
};
```

---

## 5. 参数语义

### `CMD_NOP = 0`

无操作。仅用于驱动冒烟测试 / 环状态验证。

| param | 方向 | 语义 |
|-------|------|------|
| 全部 | — | 忽略 |
| `res_code` | 出 | 0 |

---

### `CMD_MKDIR = 1`

创建目录。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | `dp(路径)` — 路径字符串所在的 canvas 指针 |
| `param[1]` | 入 | `uint64_t` — 路径长度（字节） |
| `param[2]` | 入 | `uint64_t` — mode 权限位（0 = 默认 0755） |
| `param[3..5]` | — | 未使用（0） |
| `res_code` | 出 | 0 / -errno |

错误码：`-EEXIST`, `-ENOENT`, `-ENOTDIR`, `-ENAMETOOLONG`, `-EACCES`。

---

### `CMD_RMDIR = 2`

删除空目录。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | `dp(路径)` |
| `param[1]` | 入 | 路径长度 |
| `param[2..5]` | — | 未使用 |
| `res_code` | 出 | 0 / -errno |

错误码：`-ENOTEMPTY`, `-ENOENT`, `-ENOTDIR`, `-EBUSY`。

---

### `CMD_CREATE = 3`

创建普通文件。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | `dp(路径)` |
| `param[1]` | 入 | 路径长度 |
| `param[2]` | 入 | flags — `O_CREAT=0x40`, `O_EXCL=0x80`（与 Linux 一致） |
| `param[3]` | 入 | mode 权限位（0 = 默认 0644） |
| `param[4..5]` | — | 未使用 |
| `res_code` | 出 | 0 / -errno |

错误码：`-EEXIST`, `-ENOENT`, `-ENOTDIR`, `-ENAMETOOLONG`, `-ENOSPC`。

**`CMD_CREATE` 与 `CMD_OPEN` 的关系：** CMD_CREATE 只创建不打开。创建后需要 CMD_OPEN 才能读写。这与其他文件系统的 creat-with-open 行为不同，简化为两步——创建和打开解耦。

---

### `CMD_UNLINK = 4`

删除文件（或空目录，类似 POSIX unlink）。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | `dp(路径)` |
| `param[1]` | 入 | 路径长度 |
| `param[2..5]` | — | 未使用 |
| `res_code` | 出 | 0 / -errno |

错误码：`-ENOENT`, `-EISDIR`, `-EBUSY`。

---

### `CMD_OPEN = 5`

打开文件，返回 inode 编号。打开后才能 READ / WRITE / FSTAT / FSYNC。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | `dp(路径)` |
| `param[1]` | 入 | 路径长度 |
| `param[2]` | 入 | flags — `O_RDONLY=0`, `O_WRONLY=1`, `O_RDWR=2` |
| `param[3..5]` | — | 未使用 |
| `res_code` | 出 | ino / -errno | 成功时返回 inode 编号 |

错误码：`-ENOENT`, `-EACCES`, `-EIO`。

---

### `CMD_CLOSE = 6`

关闭已打开的文件。关闭后该 ino 在本 ring 中失效（server 端引用计数管理）。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | `uint64_t` — ino（CMD_OPEN 返回的 inode 编号） |
| `param[1..5]` | — | 未使用 |
| `res_code` | 出 | 0 / -errno |

错误码：`-EBADF`。

---

### `CMD_READ = 7`

从文件中读取数据到 canvas 缓冲区。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | ino |
| `param[1]` | 入 | `dp(接收缓冲区)` — 数据写入目标 canvas |
| `param[2]` | 入 | `uint64_t` — 请求读取的字节数 |
| `param[3]` | 入 | `uint64_t` — 文件偏移（字节） |
| `param[4..5]` | — | 未使用 |
| `res_code` | 出 | ≥0 = 实际读取字节数 / <0 = -errno |

`res_code` 双重语义：非负值 = 已读取字节数（0 表示 EOF），负值 = -errno。

错误码：`-EBADF`, `-EINVAL`, `-EIO`。

---

### `CMD_WRITE = 8`

将 canvas 缓冲区中的数据写入文件。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | ino |
| `param[1]` | 入 | `dp(数据源)` — 待写入数据的 canvas 指针 |
| `param[2]` | 入 | `uint64_t` — 写入字节数 |
| `param[3]` | 入 | `uint64_t` — 文件偏移（字节） |
| `param[4]` | 入 | `uint64_t` — flags：0 = 按偏移写入，1 = 追加（忽略 param[3]） |
| `param[5]` | — | 未使用 |
| `res_code` | 出 | ≥0 = 实际写入字节数 / <0 = -errno |

`res_code` 双重语义同 READ。

错误码：`-EBADF`, `-ENOSPC`, `-EIO`, `-EFBIG`。

---

### `CMD_STAT = 9`

通过路径查询文件/目录/软链接的元数据。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | `dp(路径)` |
| `param[1]` | 入 | 路径长度 |
| `param[2]` | 入 | `dp(stat 输出缓冲区)` — 结果写入目标 canvas |
| `param[3]` | 入 | `uint64_t` — 输出缓冲区大小（应 ≥ `sizeof(ravenfs_stat_t)`） |
| `param[4..5]` | — | 未使用 |
| `res_code` | 出 | 0 / -errno |

`ravenfs_stat_t` 结构（写 入 dp(param[2])）：

```c
struct ravenfs_stat_t {
    uint64_t  st_ino;          // inode 编号
    uint64_t  st_size;         // 文件大小（字节）
    uint32_t  st_uid;
    uint32_t  st_gid;
    uint32_t  st_mode;         // 文件类型 + 权限（sys/stat.h 风格）
    uint32_t  st_nlink;        // 硬链接计数
    uint64_t  st_mtime;        // 最后修改时间（seconds）
    uint64_t  st_ctime;        // 元数据变更时间
    uint64_t  st_atime;        // 最后访问时间
    uint32_t  st_blksize;      // 首选 IO 块大小
    uint32_t  pad;             // 对齐
} __attribute__((packed));
static_assert(sizeof(ravenfs_stat_t) == 64, "stat size");
```

错误码：`-ENOENT`, `-ENOTDIR`, `-EACCES`, `-ENAMETOOLONG`。

---

### `CMD_FSTAT = 10`

通过已打开的 inode 查询文件元数据。参数语义同 CMD_STAT，但省去路径。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | ino |
| `param[1]` | 入 | `dp(stat 输出缓冲区)` |
| `param[2]` | 入 | 缓冲区大小 |
| `param[3..5]` | — | 未使用 |
| `res_code` | 出 | 0 / -errno |

输出格式同 `ravenfs_stat_t`（见 CMD_STAT）。

错误码：`-EBADF`。

---

### `CMD_READDIR = 11`

读取目录项。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | ino — 目录 inode |
| `param[1]` | 入 | `dp(输出缓冲区)` — 目录项写入目标 canvas |
| `param[2]` | 入 | `uint64_t` — 输出缓冲区最大字节数 |
| `param[3]` | 入 | `uint64_t` — readdir cookie（0 = 从头开始） |
| `→ param[4]` | 出 | `uint64_t` — next cookie（0 = 无更多条目） |
| `→ param[5]` | 出 | `uint64_t` — 写入缓冲区的总字节数 |
| `res_code` | 出 | 0 / -errno |

输出格式（写入 dp(param[1])）：

```c
struct ravenfs_dirent_t {
    uint64_t  d_ino;           // inode 编号
    uint64_t  d_cookie;        // 本条目的 cookie（传给下次 param[3]）
    uint32_t  d_name_len;      // 文件名长度（字节）
    uint8_t   d_type;          // 文件类型：0=regular, 1=dir, 2=symlink
    char      d_name[255];     // 文件名（无 null 终止，用 d_name_len 判定长度）
} __attribute__((packed));

// 条目连续排列，不留填充。
// server 在填入下一个条目会超出 max_bytes 时停止。
// 通过 param[5] 获取实际写入字节数，遍历时按 d_name_len 跳转到下一条。
// 当 param[4] == 0 时表示无后续条目。
```

遍历模式：

```
cookie = 0;
do {
    cmd.param[0] = dir_ino;
    cmd.param[1] = dp(buf);
    cmd.param[2] = BUF_SIZE;
    cmd.param[3] = cookie;
    // submit → wait → complete
    cookie = cmd->param[4];
    bytes  = cmd->param[5];
    // 解析 buf 中的 ravenfs_dirent_t[]
} while (cookie != 0 && cookie_从未见过);
```

错误码：`-ENOTDIR`, `-EBADF`, `-EIO`。

---

### `CMD_RENAME = 12`

重命名或移动文件/目录。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | `dp(旧路径)` |
| `param[1]` | 入 | 旧路径长度 |
| `param[2]` | 入 | `dp(新路径)` |
| `param[3]` | 入 | 新路径长度 |
| `param[4..5]` | — | 未使用 |
| `res_code` | 出 | 0 / -errno |

错误码：`-ENOENT`, `-ENOTDIR`, `-EEXIST`, `-EXDEV`, `-EBUSY`, `-ENOTEMPTY`, `-EIO`。

v1 要求源与目标在同一个 RavenFS 挂载内（`-EXDEV` 表示跨设备尝试）。

---

### `CMD_TRUNCATE = 13`

按 ino 截断文件到指定大小。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | ino |
| `param[1]` | 入 | `uint64_t` — 新文件大小（字节） |
| `param[2]` | 入 | `uint64_t` — flags：0 = 截断，1 = 清零区间（保留大小） |
| `param[3..5]` | — | 未使用 |
| `res_code` | 出 | 0 / -errno |

`new_size < current_size` → 释放超出部分的 extent。
`new_size > current_size` → v1 不做 sparse 扩展（返回 `-ENOTSUP`），由 guest 自行 WRITE 补齐。

错误码：`-EBADF`, `-ENOTSUP`, `-EIO`。

---

### `CMD_FSYNC = 14`

将 ino 对应的文件数据刷回磁盘。

| param | 方向 | 语义 |
|-------|------|------|
| `param[0]` | 入 | ino |
| `param[1]` | 入 | `uint64_t` — flags：0 = 数据+元数据，1 = 仅数据 |
| `param[2..5]` | — | 未使用 |
| `res_code` | 出 | 0 / -errno |

错误码：`-EBADF`, `-EIO`。

---

## 6. res_code 返回值约定

遵循 `protocol.md §5` 的约定：

| int32_t 视角 | 含义 |
|-------------|------|
| `-1`（`0xFFFFFFFF`） | `CMD_NOT_PROCESSED`，提交前状态 |
| `< 0` | `-errno` |
| `≥ 0` | 命令自身的语义返回值（见各命令定义） |

---

## 7. 与 `protocol.md` 的引用关系

本文档定义的是 IO 环的命令集，不重复协议骨架。读者应结合以下文档阅读：

- `protocol.md §2` — 命令环内存布局
- `protocol.md §3` — 数据指针编码
- `protocol.md §5` — res_code 语义约定
- `protocol.md §7` — 同步/提交流程

---

## 8. v1 不做（IO 层面）

| 特性 | 原因 |
|------|------|
| `CMD_LINK` / `CMD_SYMLINK` | v1 只跑通常规文件+目录 |
| `CMD_CHMOD` / `CMD_CHOWN` | 权限模型 v1 简化处理 |
| `CMD_IOCTL` | 无块设备级控制需要 |
| 异步 IO / 轮询模式 | 全部通道同步 futex 等待 |
| inline 数据路径上的 READ/WRITE | 内联走 CMD_STAT 的 inline_data 域 |

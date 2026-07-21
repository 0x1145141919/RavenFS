# Level 1 — Per-Session Admin Ring 命令表

> 协议骨架见 `protocol.md`。本文档定义 Per-Session Admin Ring 的操作码与参数语义。
>
> opcode 从 0 起始，独立命名空间——该环只处理 per-session 资源管理命令。
>
> session_id 由 Level 0 `CMD_SESSION_REGISTER` 分配。
> 本文档中的 `{session_id}` 指代此 ID 对应的 shm 路径。

---

## 1. 环标识

```
shm_path: /tmp/ravenfsd_sessions/{session_id}/admin_ring
桥接 ID:  session_id（由 Level 0 `SESSION_REGISTER` 返回，信号附带）
```

每个已注册 session 拥有一个独立的管理环。server 通过 `session_id` 索引该 session 的资源状态。

---

## 2. 命令槽结构

```c
struct command_common_t {
    uint32_t opcode;         // Level 1 专用 opcode
    uint32_t res_code;       // CMD_NOT_PROCESSED → 语义返回值 / -errno
    uint64_t param[6];       // 6 × 8 = 48B 参数
    uint64_t reserved;       // 扩展预留
};
```

---

## 3. 操作码枚举

```c
enum SessionAdminOpcode : uint32_t {
    CMD_CANVAS_CREATE   = 0,
    CMD_CANVAS_DESTROY  = 1,
    CMD_RING_CREATE     = 2,
    CMD_RING_DESTROY    = 3,
    CMD_SESSION_CHDIR   = 4,
};
```

如需扩充（例如 QUOTA_SET / QUERY_STATE）应在末尾追加。

---

## 4. 参数语义

### `CMD_CANVAS_CREATE = 0`

创建一块数据画布。server 创建 `/tmp/ravenfsd_sessions/{session_id}/canvas_{cid}` 文件并 mmap。

| param | 方向 | 类型 | 语义 |
|-------|------|------|------|
| `param[0]` | 入 | `uint64_t` | 画布大小（字节），0 = 默认（2MB） |
| `param[1..5]` | — | 0 | 未使用 |
| `res_code` | 出 | canvas_id / -errno | 成功时返回分配的 canvas_id |

错误码：`-ENOMEM`, `-ENOSPC`, `-EMFILE`（达到 MAX_CANVAS_PER_USER）。
canvas_id 范围 `[0, MAX_CANVAS_PER_USER)`，由 server 内部 allocator 分配。

---

### `CMD_CANVAS_DESTROY = 1`

无条件销毁指定画布。释放 mmap，unlink 文件。

**注意：** server 不做引用计数检查。若仍有 IO ring 引用该 canvas 上的数据指针，
后续访问导致段错误由 guest 自行承担。

| param | 方向 | 类型 | 语义 |
|-------|------|------|------|
| `param[0]` | 入 | `uint32_t` | canvas_id |
| `param[1..5]` | — | 0 | 未使用 |
| `res_code` | 出 | 0 / -errno | 0 = 成功 |

错误码：`-ENOENT`（无此 canvas_id）。

---

### `CMD_RING_CREATE = 2`

创建一条 IO 命令环。server 创建 `/tmp/ravenfsd_sessions/{session_id}/ring_{rid}` 文件并 mmap。

| param | 方向 | 类型 | 语义 |
|-------|------|------|------|
| `param[0..5]` | — | 0 | 未使用 |
| `res_code` | 出 | ring_id / -errno | 成功时返回分配的 ring_id |

错误码：`-EMFILE`（达到 MAX_RING_PER_USER）。
ring_id 范围 `[0, MAX_RING_PER_USER)`，由 server 内部 allocator 分配。

---

### `CMD_RING_DESTROY = 3`

销毁指定 IO 命令环。释放 mmap，unlink 文件。

**前置条件：** 该 ring 上所有命令必须已处理完毕（`head == tail`）。
server 在销毁前检查 ring 的 drain 状态。

| param | 方向 | 类型 | 语义 |
|-------|------|------|------|
| `param[0]` | 入 | `uint32_t` | ring_id |
| `param[1..5]` | — | 0 | 未使用 |
| `res_code` | 出 | 0 / -errno | 0 = 成功 |

错误码：`-ENOENT`（无此 ring_id），`-EBUSY`（仍有未处理命令）。

---

### `CMD_SESSION_CHDIR = 4`

设置 session 的当前工作目录。影响 Level 2 IO 命令中相对路径的解析。
初始 cwd 在 SESSION_REGISTER 时设为 `"/"`。

| param | 方向 | 类型 | 语义 |
|-------|------|------|------|
| `param[0]` | 入 | `uint64_t` | `dp(路径)` — 路径字符串所在的 canvas 指针 |
| `param[1]` | 入 | `uint64_t` | 路径长度 |
| `param[2..5]` | — | 0 | 未使用 |
| `res_code` | 出 | 0 / -errno | 0 = 成功 |

server 不归一化路径（不解析 `..`、`.`），仅按字面存储。
路径必须以 `/` 开头（绝对路径）。

错误码：`-ENOENT`（路径指向不存在的目录——用于校验），`-ENOTDIR`。

---

## 5. 资源约束

```
MAX_SESSIONS            = 256
MAX_CANVAS_PER_SESSION  = 2048
MAX_RING_PER_SESSION   = 64
```

创建操作在达到上限时返回 `-EMFILE`。

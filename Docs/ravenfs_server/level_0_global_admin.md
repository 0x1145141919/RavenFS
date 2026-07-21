# Level 0 — Global Admin Ring 命令表（会话管理）

> 协议骨架见 `protocol.md`。本文档仅定义 Global Admin Ring 的操作码与参数语义。
>
> opcode 从 0 起始，独立命名空间——不同 level 的命令环是物理隔离的，不需要全局编号对齐。
>
> Level 0 管理的是 ravenfsd 的 **guest session**（IPC 连接会话），
**不是文件系统的 uid/gid**。文件所有权由 inode 字段管理（Level 2 操作）。

---

## 1. 环标识

```
shm_path: /dev/shm/ravenfsd-admin
桥接 ID:  GLOBAL_ADMIN_ID (= 0)
```

该环只能由 ravenfsd 自身（或管理工具）写入，对外仅暴露 `SESSION_REGISTER` / `SESSION_UNREGISTER` 的提交语义。

---

## 2. 命令槽结构（重申）

见 `protocol.md §2.2`。每个命令槽 64B：

```c
struct command_common_t {
    uint32_t opcode;         // Level 0 专用 opcode
    uint32_t res_code;       // CMD_NOT_PROCESSED → 语义返回值 / -errno
    uint64_t param[6];       // 6 × 8 = 48B 参数
    uint64_t reserved;       // 扩展预留
};
```

---

## 3. 操作码枚举

```c
enum GlobalAdminOpcode : uint32_t {
    CMD_SESSION_REGISTER   = 0,
    CMD_SESSION_UNREGISTER = 1,
    CMD_SERVER_SHUTDOWN    = 2,
};
```

如需扩充应在该枚举末尾追加，不与其他 level 冲突。

---

## 4. 参数语义

### `CMD_SESSION_REGISTER = 0`

注册一个新的 guest session，携带文件系统 uid/gid 凭据。
server 分配 session_id，创建 `/tmp/ravenfsd_sessions/{session_id}/` 目录及其中的 `admin_ring`。

这些 uid/gid 成为该 session 的隐式状态——后续 Level 2 IO 操作
（CREATE、MKDIR 等）创建 inode 时自动以此 uid/gid 填充文件所有者，
无需在每个 IO 命令中重复携带。

cwd 初始为 `"/"`，可通过 Level 1 `CMD_SESSION_CHDIR` 修改。

| param | 方向 | 类型 | 语义 |
|-------|------|------|------|
| `param[0]` | 入 | `uint32_t` | fs_uid（文件系统 uid） |
| `param[1]` | 入 | `uint64_t` | 编码：`((uint64_t)fs_gid << 32) \| flags`。低 32 bit 为 fs_gid，高 32 bit 为 flags 扩展 |
| `param[2..5]` | — | 0 | 未使用 |
| `res_code` | 出 | session_id / -errno | 成功时返回 server 分配的 ID |

错误码：`-ENOMEM`, `-ENOSPC`。
session_id 范围 `[0, MAX_SESSIONS)`（见 protocol.md §6）。

---

### `CMD_SESSION_UNREGISTER = 1`

注销指定 session。server 销毁该 session_id 下的所有 canvas、ring、admin_ring 目录。

**前置条件：** 该 session_id 下不得有任何存活的 IO ring 或 canvas。
UNREGISTER 前 session 必须先自行 DESTROY 所有 ring 和 canvas。

| param | 方向 | 类型 | 语义 |
|-------|------|------|------|
| `param[0]` | 入 | `uint32_t` | session_id（由 SESSION_REGISTER 返回） |
| `param[1..5]` | — | 0 | 未使用 |
| `res_code` | 出 | 0 / -errno | 0 = 成功 |

错误码：`-ENOENT`（无此 session_id），`-EBUSY`（仍有 ring/canvas 存活）。

Server 侧行为：

1. 检查该 session 是否仍有分配的 ring 或 canvas → 有则 `-EBUSY`
2. 检查 admin_ring 的 `saved_head == saved_tail`（无飞行中命令）→ 不相等则 `-EBUSY`
3. 通过后：`munmap` + `shm_unlink` admin_ring、`rmdir` session 目录
4. 回收 session_id

---

### `CMD_SERVER_SHUTDOWN = 2`

请求 server 优雅停机。server 刷盘后退出。

**前置条件：** 所有 session 必须已注销（`SESSION_UNREGISTER`），
即 `allocated_session_count == 0`。

| param | 方向 | 类型 | 语义 |
|-------|------|------|------|
| `param[0..5]` | — | 0 | 未使用 |
| `res_code` | 出 | 0 / -errno | 0 = 成功，server 即将退出 |

错误码：`-EBUSY`（仍有存活的 session）。

Server 侧行为：

1. 检查 `allocated_session_count == 0` → 否则 `-EBUSY`
2. `sync()` / `fsync()` 底层文件系统（确保所有数据落盘）
3. 销毁 Global Admin Ring（`munmap` + `shm_unlink`）
4. 移除 `/tmp/ravenfsd_sessions/` 目录
5. 退出进程

---

## 5. 批处理语义

saved_head → saved_tail 快照内可能包含多个命令。
server 在处理 Global Admin 环时按序遍历：

```
for each cmd in [saved_head, saved_tail):
    switch cmd.opcode:
        case CMD_SESSION_REGISTER:
            sid = alloc_session_id();
            fs_uid  = cmd->param[0];                     // 文件系统 uid
            fs_gid  = (uint32_t)cmd->param[1];           // 文件系统 gid
            session_set_cred(sid, fs_uid, fs_gid);        // 存入 session 状态
            // 创建目录 + admin_ring + mmap + init
            cmd.res_code = sid;                           // ≥0 = session_id
        case CMD_SESSION_UNREGISTER:
            sid = (uint32_t)cmd->param[0];
            if (session_has_live_resources(sid))
                cmd.res_code = -EBUSY;  // 仍有 ring/canvas
            else if (!admin_ring_is_drained(sid))
                cmd.res_code = -EBUSY;  // admin_ring 有飞行命令
            else {
                release_session(sid);   // munmap + unlink + rmdir
                cmd.res_code = 0;
            }
        case CMD_SERVER_SHUTDOWN:
            if (allocated_session_count > 0)
                cmd.res_code = -EBUSY;  // 仍有 session 存活
            else {
                sync();                 // 刷盘
                destroy_global_ring();
                rmdir("/tmp/ravenfsd_sessions/");
                exit(0);
            }
```

所有命令都刷完 result 后再推进 `head_idx` 并唤醒 guest。
`CMD_SERVER_SHUTDOWN` 除外——推进 head 后应直接 `exit(0)`，不返回唤醒循环。

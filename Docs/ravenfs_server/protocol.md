# RavenFS 守护进程 & 客户端协议 v1

> 2026-07-21
> 协议头文件: `include/ravenfsd/bridge_protocol.hpp`

---

## 1. 概述

三层命令环架构。全通信走共享内存 + SIGRTMIN 门铃，不依赖 UNIX socket。

### 1.1 三层模型

```
Level 0:  Global Admin Ring (1 个全局, /dev/shm/ravenfsd-admin)
            用户管理: USER_REGISTER / USER_UNREGISTER

Level 1:  Per-User Admin Ring (每用户 1 个)
            资源管理: CANVAS_CREATE / DESTROY, RING_CREATE / DESTROY

Level 2:  IO Rings (每用户 N 个)
            FS 操作: MKDIR / READ / WRITE ...
```

### 1.2 文件系统路径

```
Global:
  /dev/shm/ravenfsd-admin                      ← 全局管理环

Per-User:
  /tmp/ravenfsd_guests/{uid}/
    ├── admin_ring     (8KB)                    ← 用户管理环
    ├── canvas_0       (size 字节, 默认 2MB)    ← 数据画布
    ├── canvas_1       ...
    ├── ring_1         (8KB)                    ← IO 命令环
    └── ring_2         (8KB)
```

用户目录及其所有文件由 server 在 USER_REGISTER/UNREGISTER 时创建/销毁。

---

## 2. 命令环结构

### 2.1 布局

三种环 (Global Admin / Per-User Admin / IO) 使用相同的 8KB 布局：

```
偏移量      大小       字段         说明
───────────────────────────────────────────
0x0000     64B        meta         元数据 (lock/block_token/head/tail)
0x0040     64B        ring[0]      命令槽 0
0x0080     64B        ring[1]      命令槽 1
...                   ...
0x1FC0     64B        ring[126]    命令槽 126

总计:     8192B = 8KB
```

画布 (canvas) 是独立的文件，没有元数据头——纯数据缓冲区。

### 2.2 命令槽 (64B)

```c
struct command_common_t {
    uint32_t  opcode;         // RavenOpcode
    uint32_t  res_code;       // CMD_NOT_PROCESSED → 0 / errno
    uint64_t  param[6];       // 6 × 8 = 48B 自由参数
    uint64_t  reserved;
};
```

### 2.3 元数据 (64B)

```c
struct bridge_meta_t {
    uint32_t         block_token;   // Global Admin: server_pid
                                    // Per-User Admin / IO: BRIDGE_HAS_WAITER
    uint8_t          head_idx;      // server 写入, 写锁保护
    uint8_t          tail_idx;      // guest 写入, 写锁保护
    spinrwlock_cpp_t rwlock;        // 保护 head/tail/cmd
    uint8_t          reserved[56];
};
```

---

## 3. 数据指针编码

IO 命令中所有涉及数据引用的 param 使用统一编码格式：

```
param[n] = 64 bit:
  bits [0:20]   = offset     (21 bit, 最大 2MB-1 偏移)
  bits [21:31]  = canvas_id  (11 bit, 最大 2047 张画布)
  bits [32:63]  = opcode 专用语义 (flags/length 等, 放其他 param)
```

辅助宏:

```c
static inline uint64_t encode_data_ptr(uint32_t canvas_id, uint32_t offset);
static inline uint32_t data_ptr_canvas(uint64_t ptr);
static inline uint32_t data_ptr_offset(uint64_t ptr);
```

例: `CMD_MKDIR` 的路径字符串放在 `canvas_0` 偏移 128 处:

```
param[0] = encode_data_ptr(0, 128)   // canvas_id=0, offset=128
param[1] = strlen("/foo/bar")         // 路径长度
```

server 收到后：

```c
uint32_t cid = data_ptr_canvas(cmd->param[0]);
uint32_t off = data_ptr_offset(cmd->param[0]);
char *path = (char*)(user->canvases[cid].mmap_addr + off);
```

解耦效果：一个 ring 可以引用任意 canvas 的数据，不绑定。

---

## 4. 操作码

```c
enum RavenOpcode : uint32_t {
    /* Level 0 */
    CMD_USER_REGISTER   = 0,   // param[0]=uid
    CMD_USER_UNREGISTER = 1,   // param[0]=uid

    /* Level 1 */
    CMD_CANVAS_CREATE   = 2,   // param[0]=size → param[1]=canvas_id
    CMD_CANVAS_DESTROY  = 3,   // param[0]=canvas_id
    CMD_RING_CREATE     = 4,   // → param[0]=ring_id
    CMD_RING_DESTROY    = 5,   // param[0]=ring_id

    /* Level 2 */
    CMD_NOP             = 6,
    CMD_MKDIR           = 7,
    CMD_RMDIR           = 8,
    CMD_CREATE          = 9,
    CMD_UNLINK          = 10,
    CMD_OPEN            = 11,
    CMD_CLOSE           = 12,
    CMD_READ            = 13,
    CMD_WRITE           = 14,
    CMD_STAT            = 15,
    CMD_READDIR         = 16,
    CMD_RENAME          = 17,
};
```

---

## 5. 参数语义表

记号: `dp` = `encode_data_ptr(canvas_id, offset)` 编码的数据指针

### Level 0 — Global Admin

| opcode | param[0] | 完成→ |
|--------|---------|------|
| `CMD_USER_REGISTER` | uid | res_code = 0 |
| `CMD_USER_UNREGISTER` | uid | — |

### Level 1 — Per-User Admin

| opcode | param[0] | param[1] | 完成→ |
|--------|---------|---------|------|
| `CMD_CANVAS_CREATE` | size | — | param[1]=canvas_id |
| `CMD_CANVAS_DESTROY` | canvas_id | — | — |
| `CMD_RING_CREATE` | — | — | param[0]=ring_id |
| `CMD_RING_DESTROY` | ring_id | — | — |

### Level 2 — IO

| opcode | param[0] | param[1] | param[2] | param[3] |
|--------|---------|---------|---------|---------|
| `CMD_MKDIR` | dp(路径) | 路径长度 | — | — |
| `CMD_RMDIR` | dp(路径) | 路径长度 | — | — |
| `CMD_CREATE` | dp(路径) | 路径长度 | flags | — |
| `CMD_UNLINK` | dp(路径) | 路径长度 | — | — |
| `CMD_OPEN` | dp(路径) | 路径长度 | flags | *(完成→ino)* |
| `CMD_CLOSE` | ino | — | — | — |
| `CMD_READ` | ino | dp(接收缓冲区) | 长度 | 文件偏移 |
| `CMD_WRITE` | ino | dp(数据源) | 长度 | 文件偏移 |
| `CMD_STAT` | dp(路径) | 路径长度 | — | — |
| `CMD_READDIR` | ino | dp(接收区) | 最大条数 | — |
| `CMD_RENAME` | dp(旧路径) | 旧长度 | dp(新路径) | 新长度 |

---

## 6. 常量

```c
constexpr uint32_t CMD_NOT_PROCESSED = 0xFFFF'FFFF;
constexpr uint32_t BRIDGE_HAS_WAITER = 0xFFFF'FFFF;
constexpr uint32_t GLOBAL_ADMIN_ID   = 0;

constexpr int MAX_USERS          = 256;
constexpr int MAX_CANVAS_PER_USER = 2048;
constexpr int MAX_RING_PER_USER  = 64;

constexpr uint32_t CMD_RING_BYTES     = 8192;
constexpr uint32_t DEFAULT_CANVAS_BYTES = 2097152;
constexpr int      CMD_RING_SLOTS     = 127;
```

---

## 7. 同步协议

### 7.1 提交流程 (各层通用)

```
1. 写 ring[tail_idx].opcode + param[], res_code = CMD_NOT_PROCESSED

2. 写锁保护 tail:
   meta.rwlock.write_lock();
   meta.tail_idx++;
   meta.rwlock.write_unlock();

3. sigqueue(server_pid, SIGRTMIN + 0,
            (union sigval){ .sival_int = ring_bridge_id })

4. 若需等待完成:
   meta.block_token = BRIDGE_HAS_WAITER;
   wmb();
   if (cmd.res_code == CMD_NOT_PROCESSED)
       futex(&meta.block_token, FUTEX_WAIT,
             BRIDGE_HAS_WAITER, NULL, NULL, 0);
```

### 7.2 Server 处理流程

```
while (1):
  sigsuspend / sigwaitinfo → ring_id

  ring.meta.rwlock.read_lock();
  saved_tail = ring.meta.tail_idx;
  saved_head = ring.meta.head_idx;
  ring.meta.rwlock.read_unlock();

  if saved_head == saved_tail: continue

  无锁处理各 cmd (解析 param 中的 canvas 数据指针)

  ring.meta.rwlock.write_lock();
  刷 result → ring.meta.head_idx = saved_tail
  ring.meta.rwlock.write_unlock();

  // 唤醒
  uint32_t exp = BRIDGE_HAS_WAITER;
  if (cmpxchg(&ring.meta.block_token, exp, 0))
      futex(&ring.meta.block_token, FUTEX_WAKE, 1);
```

---

## 8. 守护进程骨架

```
ravenfsd /dev/nvme0n1pX [options]

  1. mount FS

  2. Global Admin Ring:
     shm_open / ravenfsd-admin, mmap
     meta.block_token = getpid()

  3. /tmp/ravenfsd_guests/ 目录 (若不存在则创建)

  4. sigaction(SIGRTMIN + 0, handler)
     handler: 存 ring_id + 设 flag

  5. main loop:
     sigsuspend(mask)
     if flag:
       switch ring_id:
         GLOBAL_ADMIN_ID → process_global_admin()
         per-user admin   → process_user_admin(uid)
         IO ring          → process_io(uid, ring_id)
```

### 8.1 Global Admin 处理

```c
void process_global_admin() {
    // 读 ring, 取 [saved_head, saved_tail) 快照
    for (cmd in batch) {
        if (cmd->opcode == CMD_USER_REGISTER) {
            uid_t uid = cmd->param[0];
            // 创建 /tmp/ravenfsd_guests/{uid}/
            // 创建 admin_ring (8KB), mmap
            // 初始化: block_token=0, head=tail=0, 所有 res_code=NOT_PROCESSED
            cmd->res_code = 0;  // 成功
        }
        if (cmd->opcode == CMD_USER_UNREGISTER) {
            uid_t uid = cmd->param[0];
            // 遍历销毁所有 canvas/ring 文件
            // 销毁 admin_ring, munmap + shm_unlink
            // rmdir /tmp/ravenfsd_guests/{uid}/
            cmd->res_code = 0;
        }
    }
    // 推进 head, 唤醒
}
```

### 8.2 Per-User Admin 处理

```c
void process_user_admin(uid_t uid) {
    // 读 ring 快照
    for (cmd in batch) {
        if (cmd->opcode == CMD_CANVAS_CREATE) {
            uint64_t size = cmd->param[0];
            int cid = alloc_canvas_id(uid);
            // shm_open /tmp/ravenfsd_guests/{uid}/canvas_{cid}
            // ftruncate + mmap
            cmd->param[1] = cid;
            cmd->res_code = 0;
        }
        if (cmd->opcode == CMD_RING_CREATE) {
            int rid = alloc_ring_id(uid);
            // 同上, 创建 ring_{rid} 文件, mmap
            cmd->param[0] = rid;
            cmd->res_code = 0;
        }
        // CANVAS_DESTROY / RING_DESTROY: 逆操作
    }
    // 推进 head, 唤醒
}
```

### 8.3 IO 处理

```c
void process_io(uid_t uid, int ring_id) {
    user_t *u = &users[uid];
    command_ring_t *ring = u->io_rings[ring_id];

    // 读 ring 快照
    for (cmd in batch) {
        if (cmd->res_code != CMD_NOT_PROCESSED) continue;

        // 解析数据指针 → canvas + offset
        uint64_t dp = cmd->param[0];
        uint32_t canvas_id = data_ptr_canvas(dp);
        uint32_t offset    = data_ptr_offset(dp);
        char *path = (char*)(u->canvases[canvas_id].addr + offset);

        switch (cmd->opcode) {
        case CMD_MKDIR: ret = fs_mkdir(path); break;
        case CMD_RMDIR: ret = fs_rmdir(path); break;
        // ...
        }
        cmd->res_code = (uint32_t)(ret < 0 ? -ret : 0);
    }
    // 推进 head, 唤醒
}
```

---

## 9. 关键约束

| | |
|---|---|
| 命令槽 | 127 `ring[127]`, idx 模 127 |
| 数据指针 | `(canvas_id << 21) | offset`, canvas_id 11 bit, offset 21 bit |
| `CMD_NOT_PROCESSED` | `0xFFFF'FFFF` |
| `BRIDGE_HAS_WAITER` | `0xFFFF'FFFF` |
| 锁保护 | head/tail/cmd, **不含 block_token** |
| 信令 | Guest → Server, `SIGRTMIN + ring_bridge_id` |
| 断连检测 | server 扫描 `/proc/{guest_tid}`, 回收资源 |

---

## 10. 设计演进

| 阶段 | Bridge 结构 | 数据区 |
|------|-----------|--------|
| v1.0 | 单层, 命令环绑定数据区 | 2MB 内嵌 |
| v1.1 | 双层: Admin + IO | 同上 |
| v1.2 | **三层: Global Admin + Per-User Admin + IO** | **画布与环解耦, 数据指针编码** |

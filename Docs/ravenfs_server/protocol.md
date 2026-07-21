# RavenFS 守护进程 & 客户端协议 v1

> 2026-07-21
> 协议头文件: `include/ravenfsd/bridge_protocol.hpp`

---

## 1. 概述

三层命令环架构。全通信走共享内存 + SIGRTMIN 门铃，不依赖 UNIX socket。

### 1.1 三层模型

每个 level 的 opcode 独立从 0 起始——不同环物理隔离，不需全局编号对齐。

```
Level 0:  Global Admin Ring (1 个全局, /dev/shm/ravenfsd-admin)
            命令表 → level_0_global_admin.md

Level 1:  Per-Session Admin Ring (每 session 1 个)
            命令表 → level_1_user_admin.md

Level 2:  IO Rings (每 session N 个)
            命令表 → level_2_io_ring.md
```

### 1.2 文件系统路径

```
Global:
  /dev/shm/ravenfsd-admin                      ← 全局管理环

Per-Session:
  /tmp/ravenfsd_sessions/{session_id}/
    ├── admin_ring     (8KB)                    ← session 管理环
    ├── canvas_0       (size 字节, 默认 2MB)    ← 数据画布
    ├── canvas_1       ...
    ├── ring_1         (8KB)                    ← IO 命令环
    └── ring_2         (8KB)
```

session 目录及其所有文件由 server 在 SESSION_REGISTER/UNREGISTER 时创建/销毁。

---

## 2. 命令环结构

### 2.1 布局

三种环 (Global Admin / Per-Session Admin / IO) 使用相同的 8KB 布局：

```
偏移量      大小       字段         说明
───────────────────────────────────────────
0x0000     64B        meta         元数据 (server_pid/ring_bridge_id/block_token/head/tail)
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
    uint32_t  res_code;       // CMD_NOT_PROCESSED → 语义返回值 / -errno
    uint64_t  param[6];       // 6 × 8 = 48B 自由参数
    uint64_t  reserved;       // 扩展预留
};
```

### 2.3 门铃信息与元数据 (64B)

每个环的元数据是门铃的完整描述——guest mmap 后直接读出 server_pid
和 ring_bridge_id 即可 `sigqueue`，无需额外握手。

```c
struct bridge_meta_t {
    uint32_t         block_token;       // 0:  futex word (BRIDGE_HAS_WAITER / 0)
    int32_t          server_pid;        // 4:  sigqueue 目标 PID
    uint32_t         ring_bridge_id;    // 8:  sigqueue sival_int 值
    uint8_t          head_idx;          // 12: server 写入, 写锁保护
    uint8_t          tail_idx;          // 13: guest 写入, 写锁保护
    spinrwlock_cpp_t rwlock;            // 14: 保护 head/tail/cmd
    uint8_t          reserved[40];      // 24: 扩展预留
};
// static_assert(sizeof(bridge_meta_t) == 64) 已验证于头文件
```

### 2.4 ring_bridge_id 分配方案

`ring_bridge_id` 是信号中 `sival_int` 的值，server 据此识别触发哪个环。

| 范围 | 用途 | 分配时机 |
|------|------|---------|
| 0 (`GLOBAL_ADMIN_ID`) | Global Admin Ring | 固定 |
| `[1, MAX_SESSIONS]` | Per-Session Admin Rings | SESSION_REGISTER 时分配，`bridge_id = session_id + 1` |
| `MAX_SESSIONS + 1 ..` | IO Rings（全局池） | RING_CREATE 时从全局 ID pool 分配 |

**信令格式（统一，不因环类型而异）：**

```c
union sigval sv;
sv.sival_int = ring_bridge_id;          // 从 meta 读取
sigqueue(meta.server_pid, SIGRTMIN + 0, sv);
```

**初始化时序：**

1. server 启动 → 创建 Global Admin ring：`server_pid=getpid()`, `ring_bridge_id=0`, `block_token=0`
2. SESSION_REGISTER → 创建 admin_ring：`server_pid=getpid()`（全进程共享一个接待员线程 PID），`ring_bridge_id=session_id+1`, `block_token=0`
3. RING_CREATE → 创建 IO ring：`server_pid=getpid()`, `ring_bridge_id` 从全局池分配, `block_token=0`

Guest 端从 mmap 的 `meta` 中读取后直接使用，不依赖任何带外信息。

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
char *path = (char*)(session->canvases[cid].mmap_addr + off);
```

解耦效果：一个 ring 可以引用任意 canvas 的数据，不绑定。

---

## 4. 命令表分文档

opcode 按所在环独立编号，各 level 文档自包含：

| Level | 文档 | 环类型 |
|-------|------|--------|
| 0 | `level_0_global_admin.md` | Global Admin Ring |
| 1 | `level_1_user_admin.md` | Per-Session Admin Ring |
| 2 | `level_2_io_ring.md` | IO Rings |

各文档定义了：opcode 枚举、param[0..5] 逐字段语义、res_code 返回值约定、
输出参数位置、错误码列表。

本文档 §3 的数据指针编码约定对所有 Level 2 命令有效（Level 0/1 直接使用
param 中的整数值，不引用 canvas）。

---

## 5. res_code 语义约定

`res_code` 是命令的唯一返回值槽，同时承载错误码和语义返回值。
以 `int32_t` 视角解读 `uint32_t`：

```
  -1 (0xFFFFFFFF)   → CMD_NOT_PROCESSED（未处理）
  < 0               → -errno（错误码）
  ≥ 0               → 命令自身的语义返回值
```

各命令返回的语义值类型由各 level 文档定义：

| res_code ≥ 0 时 | 含义 |
|-----------------|------|
| 0 | 仅表示成功，无额外语义 |
| session_id | 注册 session 时分配的 ID |
| canvas_id | 创建画布时分配的 ID |
| ring_id | 创建 IO 环时分配的 ID |
| ino | 打开文件返回的 inode 编号 |
| bytes_read | 实际读取的字节数 |
| bytes_written | 实际写入的字节数 |

部分命令通过 `param[4]/param[5]` 携带额外输出，
这些输出只有在 `res_code ≥ 0` 时才有效。

---

## 6. 常量

```c
constexpr uint32_t CMD_NOT_PROCESSED = 0xFFFF'FFFF;
constexpr uint32_t BRIDGE_HAS_WAITER = 0xFFFF'FFFF;
constexpr uint32_t GLOBAL_ADMIN_ID   = 0;

constexpr int MAX_SESSIONS       = 256;
constexpr int MAX_CANVAS_PER_SESSION = 2048;
constexpr int MAX_RING_PER_SESSION  = 64;

constexpr uint32_t CMD_RING_BYTES     = 8192;
constexpr uint32_t DEFAULT_CANVAS_BYTES = 2097152;
constexpr int      CMD_RING_SLOTS     = 127;
```

---

## 7. 同步协议

### 7.1 提交流程 (各层通用)

```
1. 写 ring[tail_idx].opcode + param[], res_code = CMD_NOT_PROCESSED

2. wmb();  // 确保命令数据已提交, server 读到新 tail 时必见完整 cmd

3. 写锁保护 tail:
   meta.rwlock.write_lock();
   meta.tail_idx++;
   meta.rwlock.write_unlock();

4. union sigval sv;
   sv.sival_int = meta.ring_bridge_id;   // 从 meta 读取
   sigqueue(meta.server_pid, SIGRTMIN + 0, sv);

5. 若需等待完成:
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

Server 多线程架构:
  Thread R (接待员): sigwaitinfo 接收全部信号
                     Level 0/1: 亲自同步处理 (内存操作, 无 I/O)
                     Level 2:   甩给工作者线程池
  Thread W0..Wn (IO 工作者): 只处理 IO 环命令 (可能涉及 io_uring)
  Thread F (文件系统): io_uring 等 I/O 密集型路径

工作者线程启动后必须 `pthread_sigmask(SIG_BLOCK, ...)` 屏蔽 SIGRTMIN。
接待员在处理 Level 0/1 期间不响应新信号——但这类 batch 处理极快
(纯内存操作)，不会造成 guest 端明显阻塞。

初始化:
  1. mount FS

  2. Global Admin Ring:
     shm_open / ravenfsd-admin, mmap
     meta.server_pid    = getpid();
     meta.ring_bridge_id = GLOBAL_ADMIN_ID;
     meta.block_token    = 0;

  3. /tmp/ravenfsd_sessions/ 目录 (若不存在则创建)

  4. 启动 IO 工作者线程池

  5. 接待员线程:
     while (true):
       // 先扫工单, 推进已完成 batch 的 head (见 §8.2 drain_work_orders)
       drain_work_orders();

       info = sigwaitinfo(mask)             // 阻塞等信号
       ring_bridge_id = info.si_value.sival_int

       // 快照
       ring = lookup_ring(ring_bridge_id)
       ring.meta.rwlock.read_lock()
       saved_tail = ring.meta.tail_idx
       saved_head = ring.meta.head_idx
       ring.meta.rwlock.read_unlock()
       if saved_head == saved_tail: continue

       if ring_bridge_id <= MAX_SESSIONS:    // Level 0/1 亲自处理
           process_admin(ring, saved_head, saved_tail)
           // process_admin 内已推进 head + futex_wake
       else {
           // Level 2: 填写工单, 唤醒线程池
           order = alloc_work_order();
           order.ring    = ring;
           order.head    = saved_head;
           order.tail    = saved_tail;
           order.session = lookup_session_by_ring(ring_bridge_id);
           order.state   = ORDER_PENDING;
           thread_pool_wake();
         }

     // 不会到达此处 (CMD_SERVER_SHUTDOWN 直接 exit)
```

### 8.1 Admin 处理（接待员内联）

Level 0 (Global Admin) 和 Level 1 (Per-Session Admin) 由接待员线程
同步处理，不进工作队列。所有命令都是内存操作，无阻塞 I/O。

```c
void process_admin(command_ring_t *ring, uint8_t head, uint8_t tail) {
    for each cmd in ring->ring[head .. tail):
        switch (cmd.opcode) {
        case CMD_SESSION_REGISTER:
            fs_uid = (uint32_t)cmd.param[0];
            fs_gid = (uint32_t)cmd.param[1];
            sid = alloc_session_id(fs_uid, fs_gid);
            cmd.res_code = sid;
        case CMD_SESSION_UNREGISTER:
            sid = (uint32_t)cmd.param[0];
            cmd.res_code = session_has_live_resources(sid) ? -EBUSY :
                           (release_session(sid), 0);
        case CMD_SERVER_SHUTDOWN:
            if (allocated_session_count > 0)
                cmd.res_code = -EBUSY;
            else {
                cmd.res_code = 0;
                sync();
                destroy_global_ring();
                rmdir("/tmp/ravenfsd_sessions/");
                exit(0);
            }
        case CMD_CANVAS_CREATE:
            cid = alloc_canvas_id(session_of(ring));
            cmd.res_code = cid;
        case CMD_CANVAS_DESTROY:
            destroy_canvas(session_of(ring), cmd.param[0]);
            cmd.res_code = 0;
        case CMD_RING_CREATE:
            rid = alloc_ring_id(session_of(ring));
            bridge_id = alloc_io_bridge_id();
            cmd.res_code = rid;
        case CMD_RING_DESTROY:
            cmd.res_code = destroy_ring(session_of(ring), cmd.param[0]);
        case CMD_SESSION_CHDIR:
            set_cwd(session_of(ring), cmd.param[0..1]);
            cmd.res_code = 0;
        }
    // 推进 head (sigblock_guard + write_lock)
    // futex_wake(block_token)
}
```

### 8.2 IO 处理（线程池工作者）

```cpp
#include <thread>
#include <mutex>
#include <condition_variable>

/* ─── 工单 ─── */
struct work_order_t {
    command_ring_t *ring;
    session_t      *session;
    uint8_t         head;
    uint8_t         tail;        // 完成后即新 head
    std::atomic<int> state;      // FREE / PENDING / DONE
};
constexpr int MAX_ORDERS = 128;
work_order_t g_orders[MAX_ORDERS];

/* ─── 线程池 ─── */
std::mutex              g_pool_mtx;
std::condition_variable g_pool_cv;
int                     g_pending_count = 0;

/* 接待员: 填好工单后通知工作者 */
void wake_io_worker(work_order_t *order) {
    order->state.store(ORDER_PENDING, std::memory_order_release);
    {
        std::lock_guard lk(g_pool_mtx);
        g_pending_count++;
    }
    g_pool_cv.notify_one();
}

/* 工作者: 处理工单 → 标记完成 (不碰 head) */
void io_worker() {
    while (true) {
        work_order_t *order = nullptr;
        {
            std::unique_lock lk(g_pool_mtx);
            g_pool_cv.wait(lk, []{ return g_pending_count > 0; });
            for (auto &o : g_orders)
                if (cmpxchg(o.state, ORDER_PENDING, ORDER_INPROGRESS))
                    { order = &o; break; }
            g_pending_count--;
        }
        if (!order) continue;

        // 处理 batch
        for each cmd in order->ring->ring[order->head .. order->tail):
            dp = cmd.param[0];
            path = order->session->canvases[
                data_ptr_canvas(dp)].addr + data_ptr_offset(dp);
            switch (cmd.opcode) {
            case CMD_MKDIR:  ret = fs_mkdir(path, cmd.param[2]); break;
            case CMD_RMDIR:  ret = fs_rmdir(path); break;
            case CMD_CREATE: ret = fs_create(path, cmd.param[2], cmd.param[3]); break;
            case CMD_OPEN:   ret = fs_open(path); cmd.res_code = ret; continue;
            case CMD_READ:   ret = fs_read(cmd.param[0], cmd.param[1..3]); break;
            case CMD_WRITE:  ret = fs_write(cmd.param[0], cmd.param[1..4]); break;
            case CMD_STAT:   ret = fs_stat(path, cmd.param[2..3]); break;
            case CMD_READDIR: ret = fs_readdir(cmd.param[0..5]); break;
            }
            cmd.res_code = (uint32_t)(ret < 0 ? -ret : ret);

        order->state.store(ORDER_DONE, std::memory_order_release);
    }
}

/* 接待员: 每次 sigwaitinfo 返回前扫工单 */
void drain_work_orders() {
    for (auto &order : g_orders) {
        int exp = ORDER_DONE;
        if (order.state.compare_exchange_strong(exp, ORDER_FREE)) {
            sigblock_guard_t guard(SIGRTMIN);
            order.ring->meta.rwlock.write_lock();
            order.ring->meta.head_idx = order.tail;  // 推进 head
            order.ring->meta.rwlock.write_unlock();
            // ~guard: 恢复 SIGRTMIN
            uint32_t exp2 = BRIDGE_HAS_WAITER;
            if (cmpxchg(&order.ring->meta.block_token, exp2, 0))
                futex(&order.ring->meta.block_token, FUTEX_WAKE, 1);
        }
    }
}

/* 初始化: 启动 N 个工作者线程 */
void start_io_pool(int n) {
    for (int i = 0; i < n; i++)
        new std::thread(io_worker);
}
```
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
| 门铃目标 | `meta.server_pid`（Global Admin init 时写入 `getpid()`） |
| 门铃载荷 | `meta.ring_bridge_id`（见 §2.4 分配方案） |
| 信令格式 | `sigqueue(meta.server_pid, SIGRTMIN+0, {.sival_int = meta.ring_bridge_id})` |
| 断连检测 | server 扫描 `/proc/{guest_tid}`, 回收资源 |

---

## 10. 设计演进

| 阶段 | Bridge 结构 | 数据区 |
|------|-----------|--------|
| v1.0 | 单层, 命令环绑定数据区 | 2MB 内嵌 |
| v1.1 | 双层: Admin + IO | 同上 |
| v1.2 | **三层: Global Admin + Per-User Admin + IO** | **画布与环解耦, 数据指针编码** |

# RavenFS 守护进程 — 初始化时序

> 2026-07-21
> 
> 承接 `protocol.md §8`（守护进程骨架），细化从进程启动到接待员线程
> 就绪的完整时序。本文件描述的是「单次 mount + 单进程」生命周期。

---

## 1. 总览

```
ravenfsd /dev/nvme0n1pX [--io-workers=N]

启动阶段:
  1. 命令行解析 & 前置校验
  2. 挂载文件系统 (mount)
  3. 创建全局 IPC 基础设施
  4. 启动 IO 工作者线程池
  5. 接待员线程就绪 → 开始服务

停机阶段:
  CMD_SERVER_SHUTDOWN → sync() → 清理 → exit(0)
```

---

## 2. 阶段一：命令行解析

```
ravenfsd <device> [options]

位置参数:
  device         块设备路径 (如 /dev/nvme0n1p3)

Options:
  --io-workers=N     IO 工作者线程数 (默认 1, 最大 64)
  --daemon           后台化
  --pidfile=PATH     PID 文件路径 (仅 --daemon 时生效)
```

解析后校验：

| 检查 | 不通过时 |
|------|---------|
| `device` 存在 | 退出 |
| `device` 可读 | 退出 |
| `device` 首 4096 字节 magic = `"RAVN"` | 退出（不是 RavenFS） |
| `--io-workers` ≤ 64 | 退出 |

是否 `--daemon`：fork + setsid + `exit(parent)`，子进程写 PID 文件。

---

## 3. 阶段二：挂载文件系统

```
1. 读 Primary Superblock (device offset 0)
   → 校验 magic、crc32、version
   → 读 world_base_lba, total_blocks, inode_array_count

2. 遍历验证 Backup Superblock 链表
   → 每个节点校验 magic + crc32 + common 字段一致性
   → 计数 == Primary.backup_count

3. 校验 Bitmap 自描述一致性
   → 推导 bitmap_blocks, bitmap_start
   → 校验 bitmap 区间的 bits 全部为 1
   → 校验 inode 元数据区域的 bits 全部为 1

4. 加载根目录 inode (root_inode)
   → 读 inode array → 定位 root_inode
   → 初始化为文件系统操作入口
```

挂载成功后 `block_token` 等内部结构初始化，后续所有命令直接操作内存中的 FS 状态。

---

## 4. 阶段三：创建全局 IPC 基础设施

### 4.1 Global Admin Ring

```
1. shm_open("/ravenfsd-admin", O_CREAT | O_RDWR, 0600)
2. ftruncate → CMD_RING_BYTES (8KB)
3. mmap → command_ring_t *global_admin_ring
4. 初始化 meta:
     block_token    = 0
     server_pid     = getpid()
     ring_bridge_id = GLOBAL_ADMIN_ID (0)
     head_idx       = 0
     tail_idx       = 0
     rwlock         = 初始状态
5. 所有 slot 的 res_code = CMD_NOT_PROCESSED
6. fchmod / shm_open 权限 → 0666
   (任何进程可 mmap, 由 ravenfsd 管理)
```

### 4.2 /tmp/ravenfsd_sessions/ 目录

```
1. stat("/tmp/ravenfsd_sessions/")
   → 若不存在: mkdir("/tmp/ravenfsd_sessions/", 0755)
   → 若存在且非目录: 退出 (状态冲突)
```

此后 `SESSION_REGISTER` 在该目录下创建每个 session 的子目录。

---

## 5. 阶段四：启动 IO 工作者线程池

```
1. 分配全局工单数组
     work_order_t g_orders[MAX_ORDERS] = {0};
     // state = ORDER_FREE, 其余字段置零

2. 按 --io-workers=N 启动 N 个工作者线程
     for (int i = 0; i < N; i++)
         new std::thread(io_worker);

3. 工作者线程启动后立即:
     pthread_sigmask(SIG_BLOCK, {SIGRTMIN})
     // 之后不再处理任何信号, 仅等 condvar
```

线程池启动后，`wake_io_worker()` 即可正常调用——虽然此时接待员还没开始收信号，但不影响。先有工作者等着，再有接待员派活。

---

## 6. 阶段五：接待员线程就绪

接待员就是主线程本身（`main()` 的最后一段），不分叉。

```
1. sigset_t mask;
   sigemptyset(&mask);
   sigaddset(&mask, SIGRTMIN);
   // 只接收 SIGRTMIN, 其他信号按默认处理

2. loop:
   while (true):
     drain_work_orders();           // 扫已完成工单
     sigwaitinfo(&mask, &info);     // 等信号
     // ... dispatch ...
```

到这一步 `ravenfsd` 正式上线，可以接受 guest 发信号了。

---

## 7. 完整的启动时序图

```
main()
├── parse_args()                         // 命令行
├── if --daemon: daemonize()
├── mount_fs(device)                     // 挂载
│   ├── read_primary_superblock()
│   ├── validate_backup_chain()
│   └── validate_bitmap()
├── init_global_admin_ring()             // /dev/shm/ravenfsd-admin
│   ├── shm_open + mmap
│   └── meta.server_pid = getpid()
├── init_sessions_dir()                  // /tmp/ravenfsd_sessions/
├── start_io_pool(n_workers)             // 线程池
│   ├── alloc g_orders[MAX_ORDERS]
│   └── pthread_create × N
├── receptionist_loop()                  // 主线程, 不返回
│   ├── sigprocmask(SIG_BLOCK, ...)
│   ├── while (true):
│   │   ├── drain_work_orders()
│   │   └── sigwaitinfo()
```

---

## 8. 状态检查

接待员上线前，guest 可能已经发信号？不会——guest 必须先 `SESSION_REGISTER` 才能拿到 `session_id` 和 `server_pid`，而 `SESSION_REGISTER` 必须通过 Global Admin ring —— 接待员循环启动前，Global Admin ring 虽然存在但没有线程在 `sigwaitinfo`，信号没人收，guest 的 `futex_wait` 会超时或永远等下去。

所以 guest 端必须实现超时重试：

```
while (res_code == CMD_NOT_PROCESSED):
    sigqueue(server_pid, SIGRTMIN+0, ...)
    futex_wait(block_token, BRIDGE_HAS_WAITER, &timeout)
    if 超时:
        retry ...        // server 可能还没就绪
```

但更优雅的做法是 server 启动后广播一个就绪标记——比如 Global Admin Ring 的 `block_token` 初始为 0，启动完成后设为 `getpid()`（或某个非零标记）。guest 轮询 `block_token != 0` 再发第一条信号。不过这是实现细节，不强制约定。

---

## 9. 冻结说明

本文档描述的初始化时序与 `protocol.md §8` 骨架一致，无协议结构层面的新增约定。全部时序细节均为 server 实现策略，不涉及 guest-visible 接口变更。

#pragma once
#include "stdint.h"
#include "../util/lock.h"

/* ═══════════════════════════════════════════════════════════════
 * RavenFS 守护进程 — Guest-Server 协议头 v1
 *
 * 三层命令环模型 (opcode 每层独立从 0 起始):
 *   Level 0: Global Admin Ring  (1 个全局) — session 管理
 *     → level_0_global_admin.md
 *
 *   Level 1: Per-Session Admin Ring  (每 session 1 个) — 资源管理
 *     → level_1_user_admin.md
 *
 *   Level 2: IO Rings  (每 session N 个) — 文件系统操作
 *     → level_2_io_ring.md
 *
 * 通信: 共享内存 + SIGRTMIN 门铃, 不依赖 UNIX socket
 *   门铃格式: sigqueue(meta.server_pid, SIGRTMIN+0,
 *                      {.sival_int = meta.ring_bridge_id})
 * ═══════════════════════════════════════════════════════════════ */

/* ─── 常量 ─── */
constexpr uint32_t CMD_NOT_PROCESSED = ~0u;
constexpr uint32_t BRIDGE_HAS_WAITER = ~0u;

constexpr uint32_t GLOBAL_ADMIN_ID    = 0;
constexpr int      MAX_SESSIONS       = 256;
constexpr int      MAX_CANVAS_PER_SESSION = 2048;   // 11 bit
constexpr int      MAX_RING_PER_SESSION  = 64;

constexpr uint32_t CMD_RING_BYTES        = 8192;      // 8KB
constexpr uint32_t DEFAULT_CANVAS_BYTES  = 2097152;   // 2MB 默认画布
constexpr int      CMD_RING_SLOTS        = 127;       // ring[127]

/* ─── 数据指针编码 ───
 *   param[n].low[31:21] = canvas_id (0..2047)
 *   param[n].low[20:0]  = offset  (0..2MB-1)
 *   param[n].high[31:0] = opcode 专用语义 */
constexpr uint64_t CANVAS_ID_SHIFT = 21;
constexpr uint64_t CANVAS_ID_MASK  = 0x7FFull << CANVAS_ID_SHIFT;
constexpr uint64_t OFFSET_MASK     = (1ull << CANVAS_ID_SHIFT) - 1;

static inline uint64_t encode_data_ptr(uint32_t canvas_id, uint32_t offset) {
    return ((uint64_t)canvas_id << CANVAS_ID_SHIFT) | (offset & OFFSET_MASK);
}
static inline uint32_t data_ptr_canvas(uint64_t ptr) {
    return (ptr >> CANVAS_ID_SHIFT) & 0x7FF;
}
static inline uint32_t data_ptr_offset(uint64_t ptr) {
    return ptr & OFFSET_MASK;
}

/* ═══════════════════════════════════════════════════════════════
 * 命令槽 — 64B (一个缓存行)
 *
 *   offset  size   field
 *   0        4     opcode
 *   4        4     res_code     CMD_NOT_PROCESSED → 语义返回值 / -errno
 *                               以 int32_t 解读: ≥0 = 返回值, <0 = -errno
 *   8        48    param[6]    自由参数 (按 opcode 定语义)
 *   56       8     reserved
 * ═══════════════════════════════════════════════════════════════ */
struct command_common_t {
    uint32_t opcode;
    uint32_t res_code;
    uint64_t param[6];
    uint64_t reserved;
};
struct sparrowOS_command_common_t {
    union 
    {
        uint64_t opcode;
        uint64_t result_code;
    }rax;
    uint64_t param[6];
    uint64_t tag;
};
static_assert(sizeof(command_common_t) == 64,
              "command_common_t must be 64B (one cache line)");

/* ═══════════════════════════════════════════════════════════════
 * 桥元数据 — 64B (一个缓存行)
 *
 *   每环自描述: guest mmap 后直接读出 server_pid 和 ring_bridge_id
 *   即可 sigqueue, 无需带外握手。
 *
 *   server_pid      门铃目标: sigqueue() 的 pid 参数
 *   ring_bridge_id  门铃载荷: sigqueue() 的 sival_int
 *                    分配方案: 0=Global Admin,
 *                              [1, MAX_SESSIONS]=session admin,
 *                              >MAX_SESSIONS=IO ring
 *   block_token     futex 字: guest 置 BRIDGE_HAS_WAITER,
 *                              server 处理完后 cmpxchg 回 0 并 FUTEX_WAKE
 *   head_idx        server 写入, 写锁保护
 *   tail_idx        guest 写入, 写锁保护
 *   rwlock          保护 head/tail/cmd 批处理一致性
 * ═══════════════════════════════════════════════════════════════ */
struct bridge_meta_t {
    uint32_t         block_token;       // 0
    int32_t          server_pid;        // 4
    uint32_t         ring_bridge_id;    // 8
    uint8_t          head_idx;          // 12
    uint8_t          tail_idx;          // 13
    spinrwlock_cpp_t rwlock;            // 14
    uint8_t          reserved[40];      // 24
};
static_assert(sizeof(bridge_meta_t) == 64,
              "bridge_meta_t must be 64B (one cache line)");

/* ═══════════════════════════════════════════════════════════════
 * 命令环 — 8KB = meta(64B) + ring[127](8128B)
 * ═══════════════════════════════════════════════════════════════ */
struct command_ring_t {
    bridge_meta_t    meta;
    command_common_t ring[CMD_RING_SLOTS];
};
static_assert(sizeof(command_ring_t) == CMD_RING_BYTES,
              "command_ring_t must be exactly 8KB");

/* ═══════════════════════════════════════════════════════════════
 * 文件系统路径布局
 *
 *   Global Admin:
 *     /dev/shm/ravenfsd-admin
 *
 *   每 session:
 *     /tmp/ravenfsd_sessions/{session_id}/
 *       ├── admin_ring    (8KB)         session 管理环
 *       ├── canvas_{id}   (size 字节)   数据画布
 *       ├── ...
 *       └── ring_{id}     (8KB)         IO 命令环
 * ═══════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════
 * 操作码 (每层独立编号, 因环类型不同不冲突)
 *
 * Level 0 — Global Admin Ring:
 *   通常入参直接使用 param 中的整数值, 不引用 canvas
 *
 * CMD_SESSION_REGISTER   = 0
 *   param[0] = fs_uid
 *   param[1] = (fs_gid << 32) | flags
 *   res_code = session_id / -errno
 *
 * CMD_SESSION_UNREGISTER = 1
 *   param[0] = session_id
 *   res_code = 0 / -EBUSY
 *
 * CMD_SERVER_SHUTDOWN    = 2
 *   param 不复用
 *   res_code = 0 / -EBUSY
 *   前置: 所有 session 已注销
 *   通过后: sync() + exit(0)
 *
 * Level 1 — Per-Session Admin Ring:
 *   资源创建命令通过 res_code 返回 ID
 *
 * CMD_CANVAS_CREATE   = 0
 *   param[0] = size (0 = 默认 2MB)
 *   res_code = canvas_id / -errno
 *
 * CMD_CANVAS_DESTROY  = 1
 *   param[0] = canvas_id
 *   res_code = 0 / -errno
 *   !! 无条件销毁, 后果自负 !!
 *
 * CMD_RING_CREATE     = 2
 *   param 不复用
 *   res_code = ring_id / -errno
 *
 * CMD_RING_DESTROY    = 3
 *   param[0] = ring_id
 *   res_code = 0 / -EBUSY
 *   前置: ring 已 drain (head == tail)
 *
 * CMD_SESSION_CHDIR   = 4
 *   param[0] = dp(路径)
 *   param[1] = 路径长度
 *   res_code = 0 / -errno
 *   cwd 初始为 "/", 影响 Level 2 相对路径解析
 *
 * Level 2 — IO Ring:
 *   数据指针 = encode_data_ptr(canvas_id, offset)
 *   路径: 以 '/' 开头=绝对, 否则基于 session cwd 拼接
 *
 * CMD_NOP        = 0   无操作, 冒烟测试
 * CMD_MKDIR      = 1   param[0..1]=dp(path)+len  param[2]=mode
 * CMD_RMDIR      = 2   param[0..1]=dp(path)+len
 * CMD_CREATE     = 3   param[0..1]=dp(path)+len  param[2]=flags  param[3]=mode
 * CMD_UNLINK     = 4   param[0..1]=dp(path)+len
 * CMD_OPEN       = 5   param[0..1]=dp(path)+len  param[2]=flags
 *                       res_code = ino / -errno
 * CMD_CLOSE      = 6   param[0]=ino
 * CMD_READ       = 7   param[0]=ino  param[1]=dp(buf)  param[2]=len
 *                       param[3]=file_off  res_code = bytes_read / -errno
 * CMD_WRITE      = 8   param[0]=ino  param[1]=dp(data)  param[2]=len
 *                       param[3]=file_off  param[4]=flags(0=write,1=append)
 *                       res_code = bytes_written / -errno
 * CMD_STAT       = 9   param[0..1]=dp(path)+len  param[2]=dp(stat_buf)
 *                       param[3]=stat_buf_len  res_code=0
 * CMD_FSTAT      = 10  param[0]=ino  param[1]=dp(stat_buf)
 *                       param[2]=stat_buf_len  res_code=0
 * CMD_READDIR    = 11  param[0]=ino  param[1]=dp(buf)  param[2]=max_bytes
 *                       param[3]=cookie  → param[4]=next_cookie
 *                       → param[5]=bytes_written  res_code=0
 * CMD_RENAME     = 12  param[0..1]=dp(old)+old_len
 *                       param[2..3]=dp(new)+new_len
 * CMD_TRUNCATE   = 13  param[0]=ino  param[1]=new_size
 * CMD_FSYNC      = 14  param[0]=ino  param[1]=flags
 * ═══════════════════════════════════════════════════════════════ */

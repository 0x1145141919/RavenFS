#pragma once
#include "stdint.h"
#include "../util/lock.h"

/* ═══════════════════════════════════════════════════════════════
 * RavenFS 守护进程 — Guest-Server 协议头
 *
 * 三层命令环模型:
 *   Level 0: Global Admin Ring  (1 个全局)
 *     CMD_USER_REGISTER / UNREGISTER
 *
 *   Level 1: Per-User Admin Ring  (每用户 1 个)
 *     CMD_CANVAS_CREATE / DESTROY
 *     CMD_RING_CREATE / DESTROY
 *
 *   Level 2: IO Rings  (每用户 N 个)
 *     CMD_MKDIR / READ / WRITE ...
 *
 * 数据指针: param[n] 高 11 bit = canvas_id, 低 21 bit = offset
 * ═══════════════════════════════════════════════════════════════ */

/* ─── 常量 ─── */
constexpr uint32_t CMD_NOT_PROCESSED = ~0u;
constexpr uint32_t BRIDGE_HAS_WAITER = ~0u;

constexpr uint32_t GLOBAL_ADMIN_ID = 0;
constexpr int      MAX_USERS       = 256;
constexpr int      MAX_CANVAS_PER_USER = 2048;  // 11 bit
constexpr int      MAX_RING_PER_USER    = 64;

constexpr uint32_t CMD_RING_BYTES    = 8192;     // 8KB
constexpr uint32_t DEFAULT_CANVAS_BYTES = 2097152; // 2MB 默认画布
constexpr int      CMD_RING_SLOTS    = 127;      // ring[127]

/* ─── 数据指针编码 ───
 *   param[n]:  [31:21] = canvas_id (0..2047)
 *              [20:0]  = offset  (0..2MB-1)
 *   参数字段本身是 uint64_t, 高 32 bit 为 opcode 专用语义 */
constexpr uint64_t CANVAS_ID_SHIFT = 21;
constexpr uint64_t CANVAS_ID_MASK  = 0x7FFull << CANVAS_ID_SHIFT;  // 11 bit
constexpr uint64_t OFFSET_MASK     = (1ull << CANVAS_ID_SHIFT) - 1; // 21 bit

static inline uint64_t encode_data_ptr(uint32_t canvas_id, uint32_t offset) {
    return ((uint64_t)canvas_id << CANVAS_ID_SHIFT) | (offset & OFFSET_MASK);
}
static inline uint32_t data_ptr_canvas(uint64_t ptr) {
    return (ptr >> CANVAS_ID_SHIFT) & 0x7FF;
}
static inline uint32_t data_ptr_offset(uint64_t ptr) {
    return ptr & OFFSET_MASK;
}

/* ─── 操作码 ─── */
enum RavenOpcode : uint32_t {
    /* Level 0: Global Admin */
    CMD_USER_REGISTER   = 0,   // param[0]=uid
    CMD_USER_UNREGISTER = 1,   // param[0]=uid

    /* Level 1: Per-User Admin */
    CMD_CANVAS_CREATE   = 2,   // param[0]=size → 完成→param[1]=canvas_id
    CMD_CANVAS_DESTROY  = 3,   // param[0]=canvas_id
    CMD_RING_CREATE     = 4,   // → 完成→param[0]=ring_id
    CMD_RING_DESTROY    = 5,   // param[0]=ring_id

    /* Level 2: IO */
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

/* ═══════════════════════════════════════════════════════════════
 * 命令槽 — 64B (一个缓存行)
 *
 *   offset  size   field
 *   0        4     opcode
 *   4        4     res_code     CMD_NOT_PROCESSED → 0/errno
 *   8        48    param[6]    自由参数 (按 opcode 定语义)
 *   56       8     reserved
 * ═══════════════════════════════════════════════════════════════ */
struct command_common_t {
    uint32_t opcode;
    uint32_t res_code;
    uint64_t param[6];
    uint64_t reserved;
};
static_assert(sizeof(command_common_t) == 64,
              "command_common_t must be 64B (one cache line)");

/* ═══════════════════════════════════════════════════════════════
 * 桥元数据 — 64B (一个缓存行)
 *
 *   block_token  futex 字, 不受锁保护
 *                Global Admin: 存放 server_pid
 *                Per-User Admin / IO: BRIDGE_HAS_WAITER
 *   head_idx     server 写入, 写锁保护
 *   tail_idx     guest 写入, 写锁保护
 *   rwlock       保护 head/tail/cmd 批处理一致性
 * ═══════════════════════════════════════════════════════════════ */
struct bridge_meta_t {
    uint32_t         block_token;
    uint8_t          head_idx;
    uint8_t          tail_idx;
    spinrwlock_cpp_t rwlock;
    uint8_t          reserved[48];
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
 *   每用户:
 *     /tmp/ravenfsd_guests/{uid}/
 *       ├── admin_ring    (8KB)         用户管理环
 *       ├── canvas_{id}   (size 字节)   数据画布
 *       ├── ...
 *       └── ring_{id}     (8KB)         IO 命令环
 * ═══════════════════════════════════════════════════════════════ */

/* ─── param[6] 语义表 ───

   Level 0 — Global Admin:
     CMD_USER_REGISTER:   param[0]=uid   完成→res_code=0
     CMD_USER_UNREGISTER: param[0]=uid

   Level 1 — Per-User Admin:
     CMD_CANVAS_CREATE:   param[0]=size  完成→param[1]=canvas_id
     CMD_CANVAS_DESTROY:  param[0]=canvas_id
     CMD_RING_CREATE:     —              完成→param[0]=ring_id
     CMD_RING_DESTROY:    param[0]=ring_id

   Level 2 — IO (数据指针 = encode_data_ptr(canvas_id, offset)):
     CMD_MKDIR:    param[0]=路径数据指针  param[1]=路径长度
     CMD_RMDIR:    param[0]=路径数据指针  param[1]=路径长度
     CMD_CREATE:   param[0]=路径数据指针  param[1]=路径长度  param[2]=flags
     CMD_UNLINK:   param[0]=路径数据指针  param[1]=路径长度
     CMD_OPEN:     param[0]=路径数据指针  param[1]=路径长度  param[2]=flags
                   完成→param[3]=ino
     CMD_CLOSE:    param[0]=ino
     CMD_READ:     param[0]=ino  param[1]=数据指针(接收)  param[2]=长度  param[3]=文件偏移
     CMD_WRITE:    param[0]=ino  param[1]=数据指针(源)    param[2]=长度  param[3]=文件偏移
     CMD_STAT:     param[0]=路径数据指针  param[1]=路径长度
     CMD_READDIR:  param[0]=ino  param[1]=数据指针(接收)  param[2]=最大条数
     CMD_RENAME:   param[0]=旧路径指针   param[1]=旧长度  param[2]=新路径指针  param[3]=新长度
 */

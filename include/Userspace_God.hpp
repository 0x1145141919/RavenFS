#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include "disk_struct.hpp"
#include "magics.h"
#include "util/lock.h"

/* ═══════════════════════════════════════════════════════════════
 * 挂载错误码
 * ═══════════════════════════════════════════════════════════════ */
enum RavenFSError : uint64_t {
    RAVENFS_OK              = 0,   // 成功
    RAVENFS_ERR_OPEN        = 1,   // 打开设备失败
    RAVENFS_ERR_LBA         = 2,   // LBA_size 不支持
    RAVENFS_ERR_NOMAGIC     = 3,   // 非 RAVN 魔数
    RAVENFS_ERR_VERSION     = 4,   // 不支持的版本号
    RAVENFS_ERR_WORLD_BASE  = 5,   // world_base_lba * lba_size != 4096
    RAVENFS_ERR_CRC         = 6,   // Superblock CRC32 校验失败
    RAVENFS_ERR_BACKUP      = 7,   // Backup Superblock 链表校验失败
    RAVENFS_ERR_BITMAP      = 8,   // Bitmap 自描述一致性校验失败
};

/* ═══════════════════════════════════════════════════════════════
 * FsGeometry — 布局几何（同 mkfs.cpp calc_geometry）
 * ═══════════════════════════════════════════════════════════════ */
struct FsGeometry {
    uint64_t total_blocks;
    uint64_t bitmap_blocks;
    uint64_t bitmap_start;
    uint64_t inode_blocks;
    uint64_t inode_array_base;
    uint64_t inode_bitmap_blocks;
    uint64_t inode_array_bitmap_base;
};

/* ═══════════════════════════════════════════════════════════════
 * BlockDevice — 块设备抽象（用户态具体类）
 *
 * 持有 fd + LBA 参数，read_block/write_block 直接 pread/pwrite。
 * 移植到内核时重写此 struct，方法签名不变。
 * ═══════════════════════════════════════════════════════════════ */

struct BlockDevice {
    int      fd;
    uint64_t lba_size;
    uint64_t multiplier;

    void read_block(uint64_t fblock, void *buf) const {
        ::pread(fd, buf, 4096, 4096 + fblock * 4096);
    }
    void write_block(uint64_t fblock, const void *buf) const {
        ::pwrite(fd, buf, 4096, 4096 + fblock * 4096);
    }
};

/* ═══════════════════════════════════════════════════════════════
 * Inode Cache — 文件作用域数据结构
 *
 * Inode_cache_line：     数据行（4096B = 16 inode）
 * Inode_cache_line_controller：每 slot 控制块（24B）
 * controller_set：      1 组（8 way × 24B = 192B）
 *
 * inum 位域编码:
 *   [0:3]     → row（行内引索）
 *   [4:4+N-1] → set_idx（组索引）
 *   [4+N:63]  → tag（高比特验证）
 *   N = num_of_entries_log2
 * ═══════════════════════════════════════════════════════════════ */

struct Inode_cache_line {
    raven_Inode_t entries[16];
};
static_assert(sizeof(Inode_cache_line) == 4096, "Inode cache line must be 4096B");

struct Inode_cache_line_controller {
    uint16_t           flags;               // [V:1 | D:1 | T:1 | reserved:13]
    spintrylock_cpp_t  lock;
    uint8_t            reserved0;
    uint32_t           num_of_accessor;
    uint64_t           last_access_stamp;
    uint64_t           tag;
};
static_assert(sizeof(Inode_cache_line_controller) == 24,
              "Controller must be 24B");

static constexpr uint16_t INODE_CACHE_V = 1 << 0;
static constexpr uint16_t INODE_CACHE_D = 1 << 1;
static constexpr uint16_t INODE_CACHE_T = 1 << 2;

static constexpr uint8_t WAYS_PER_SET = 8;

struct controller_set {
    Inode_cache_line_controller controllers[WAYS_PER_SET];
};
static_assert(sizeof(controller_set) == 192,
              "controller_set must be 192B (8 × 24B)");

/* ─── InodeCacheConfig — 初始化信息包 ───
 *
 * 控制器构造时一次性传入，之后不可变。
 * 所有内存由外部（RavenFS）分配，控制器只使用不拥有。
 */
struct InodeCacheConfig {
    const BlockDevice  *dev;                 // I/O 通道
    uint64_t            inode_array_base;    // inode 数组基址（FS_block 号）
    uint64_t            inode_array_count;   // inode 总数上限（超限请求被拒绝）
    controller_set     *all_entries;         // [sets] 控制面
    Inode_cache_line   *cache_lines;         // [total_slots] 数据面
    uint8_t             num_of_entries_log2; // N: sets = 1 << N
};

/* ─── Inode_cache_controller — 附生式缓存控制器 ───
 *
 * 纯使用者，不分配不释放任何资源。
 * 构造时接收 InodeCacheConfig，之后所有操作基于配置指针。
 * 无 init/destroy，无业务返回值。
 */
class Inode_cache_controller {
    InodeCacheConfig cfg;
    uint64_t         access_stamp_counter;

    Inode_cache_line *slot_data(uint64_t set, int way) const {
        return cfg.cache_lines + (set * WAYS_PER_SET + way);
    }

public:
    Inode_cache_controller(const InodeCacheConfig &cfg)
        : cfg(cfg), access_stamp_counter(0) {}

    raven_Inode_t *cache_get(uint64_t inum);
    void           cache_put(uint64_t inum);
    void           mark_dirty(uint64_t inum);
    void           flush_all();
    void           flush_inum(uint64_t inum);

    uint64_t sets()            const { return 1ULL << cfg.num_of_entries_log2; }
    uint64_t total_slots()     const { return sets() * WAYS_PER_SET; }
    uint64_t bytes()           const { return total_slots() * 4096; }
    bool     is_valid()        const { return cfg.all_entries != nullptr && cfg.cache_lines != nullptr; }

    const InodeCacheConfig &config() const { return cfg; }

private:
    struct InumFields {
        uint64_t row;
        uint64_t set_idx;
        uint64_t tag;
    };

    InumFields decode_inum(uint64_t inum) const {
        uint64_t set_mask = (1ULL << cfg.num_of_entries_log2) - 1;
        return {
            .row     = inum & 0xF,
            .set_idx = (inum >> 4) & set_mask,
            .tag     = inum >> (4 + cfg.num_of_entries_log2)
        };
    }

    uint64_t inum_to_block(uint64_t inum) const {
        return cfg.inode_array_base + (inum / 16);
    }
    int  select_victim_and_lock(controller_set *cs);

    raven_Inode_t *load_slot_locked(controller_set *cs, int way,
                              uint64_t inum, const InumFields &f);
};

/* ─── Controller state ─── */
static constexpr uint8_t BITMAP_CTRL_OFFLINE     = 0;
static constexpr uint8_t BITMAP_CTRL_ONLINE      = 1;
static constexpr uint8_t BITMAP_CTRL_INTEGRATING = 2;

/* ─── Forward declarations ─── */
struct BitmapFields;
struct BitmapBlockController;
struct ProcessorBitmapState;
struct BitmapCacheConfig;

/* ═══════════════════════════════════════════════════════════════
 * BitmapCacheController — 位图缓存控制器
 *
 * 自包含模块：构造时分配所有内存，析构时通过 shutdown_all 刷盘清理。
 * 上帝类只传建参数，内存生命周期在模块内部管理。
 *
 * 锁序: alloc_lock → controller.lock (单向，无死环)
 * ═══════════════════════════════════════════════════════════════ */
class BitmapCacheController {
public:
    explicit BitmapCacheController(const BitmapCacheConfig &cfg);
    ~BitmapCacheController();

    BitmapCacheController(const BitmapCacheController &) = delete;
    BitmapCacheController &operator=(const BitmapCacheController &) = delete;

    /* ─── Core API ─── */
    uint64_t hard_alloc(uint64_t count);       // 返回 ~0ULL = 全盘满
    uint64_t soft_alloc(uint64_t max_count);   // 返回实际分配数，~0ULL = 全盘满
    void     free(uint64_t fblock, uint64_t count);

    /* ─── 上帝类驱动的关闭流程 ─── */
    void shutdown_all();

    bool is_valid() const { return all_controller != nullptr; }

private:
    /* ─── 构造参数副本 ─── */
    const BlockDevice  *dev;
    uint64_t            bitmap_base;
    uint64_t            bitmap_block_count;
    uint16_t            processor_max;
    uint8_t             N;

    /* ─── 推导值 ─── */
    uint64_t            ctrl_count;
    uint64_t            bucket_sz;        // (1<<N) * 4096
    uint64_t            bucket_bits;      // (1<<N) * 32768
    uint64_t            ctrl_seg_bits;    // 1ULL << (N + 15)

    /* ─── 内部表 ─── */
    BitmapBlockController *all_controller;      // [ctrl_count]
    ProcessorBitmapState  *all_proc_state;      // [processor_max]
    uint8_t               *raw_buckets;         // [processor_max * bucket_sz]

    /* ─── Sweeper ─── */
    std::thread            sweeper;
    std::atomic<bool>      sweeper_stop{false};
    uint8_t               *sweeper_bucket;      // 专职 buffer

    /* ─── 位图运算 (静态，操作裸 buffer) ─── */
    static uint64_t scan_run(const uint8_t *buf, uint64_t buf_bits,
                              uint64_t start, uint64_t need);
    static uint64_t find_longest(const uint8_t *buf, uint64_t buf_bits,
                                  uint64_t max_count);
    static void     set_bits(uint8_t *buf, uint64_t start, uint64_t count);
    static void     clear_bits(uint8_t *buf, uint64_t start, uint64_t count);

    /* ─── I/O ─── */
    void load_bucket(uint8_t *bucket, uint64_t cid) const;
    void flush_bucket(const uint8_t *bucket, uint64_t cid) const;

    /* ─── Decode / encode ─── */
    BitmapFields decode(uint64_t bit_idx) const;
    uint64_t     encode(uint64_t cid, uint64_t bucket_pos) const;

    /* ─── 分配器内部逻辑 ─── */
    uint64_t try_alloc(ProcessorBitmapState *ps, uint64_t cid,
                        uint64_t count);
    uint64_t try_alloc_soft(ProcessorBitmapState *ps, uint64_t cid,
                             uint64_t max_count);
    void     consume_head(uint8_t *bucket, BitmapBlockController *ctrl);
    void     switch_to(uint64_t old_cid, uint64_t new_cid);

    /* ─── 清理 ─── */
    void flush_processor(uint64_t pid);
    void flush_controller(uint64_t cid);
    void stop_sweeper();
    void sweeper_loop();
};

/* ─── BitmapFields — 全局 bit 引索三级分解 ───
 *
 * 全局 bit 引索（对应 FS_block 编号或 inode 编号）拆为三段:
 *   [0:14]     → bit_in_block（bitmap 块内 bit 偏移）
 *   [15:15+N-1]→ block_idx（controller 段内的 bitmap 块索引）
 *   [15+N:63]  → controller_id
 *
 * N = num_of_blocks_per_bucket_log2
 * 32768 = 1<<15（每 bitmap FS_block 管理的 bit 数）
 */
struct BitmapFields {
    uint64_t controller_id;     // 哪个 controller
    uint64_t block_idx;         // controller 段内的 bitmap 块索引
    uint64_t bit_in_block;      // bitmap 块内的 bit 偏移 (0..32767)
};

static inline BitmapFields decode_bitmap_idx(uint64_t bit_idx, uint8_t N) {
    uint64_t ctrl_mask = (1ULL << N) - 1;
    return {
        .controller_id = bit_idx >> (N + 15),
        .block_idx     = (bit_idx >> 15) & ctrl_mask,
        .bit_in_block  = bit_idx & 0x7FFF
    };
}

struct bitmap_return_record_sll{
    bitmap_return_record_sll*next;
    uint32_t base;
    uint32_t entry_count;
};//[base,base+entry_count]的区间进行归还
struct BitmapBlockController{
    spinlock_cpp_t lock;             // 保护 state 转换、free_count、head 链表的原子性
    uint8_t state;                   // 0=offline, 1=online
    uint16_t flags;                  // 扩展预留
    uint32_t free_count;             // 本段空闲 FS_block 计数（近似值）
    uint64_t scan_hint;              // 位图桶隐式状态：上次成功分配的 bit 偏移
    bitmap_return_record_sll*head;   // 外来归还链表
};
struct ProcessorBitmapState {
    spinlock_cpp_t   alloc_lock;    // 保护本处理器分配状态机（current_cid + hint 变更是原子的）
    uint64_t         current_cid;   // 当前临幸的 controller_id
    uint64_t         last_hint_cid; // 上次成功分配的 controller_id（慢路径扫描起点）
    uint8_t         *bucket;        // 本处理器桶数据指针（上帝类分配，指向 raw_work_sets[p]）
};
struct BitmapCacheConfig {
    const BlockDevice     *dev;                  // I/O 通道
    uint64_t               bitmap_base;          // Bitmap 数组基址（FS_block 号）
    uint64_t               bitmap_block_count;   // Bitmap FS_block 总数上限
    uint16_t               processor_max;        // 最大处理器数
    uint8_t                num_of_blocks_per_bucket_log2; // N: 每桶对应 1<<N 个 bitmap FS_block
};

/* ═══════════════════════════════════════════════════════════════
 * InodeBitmapCacheConfig — Inode Bitmap 缓存控制器的构造参数
 * ═══════════════════════════════════════════════════════════════ */
struct InodeBitmapCacheConfig {
    const BlockDevice     *dev;                  // I/O 通道
    uint64_t               bitmap_base;          // geoinode_array_bitmap_base
    uint64_t               bitmap_block_count;   // geo.inode_bitmap_blocks
    uint64_t               inode_array_count;    // sb.inode_array_count（分配上限）
    uint8_t                num_of_blocks_per_bucket_log2; // N
};

class InodeBitmapCacheController {
public:
    explicit InodeBitmapCacheController(const InodeBitmapCacheConfig &cfg);
    ~InodeBitmapCacheController();

    InodeBitmapCacheController(const InodeBitmapCacheController &) = delete;
    InodeBitmapCacheController &operator=(const InodeBitmapCacheController &) = delete;

    uint64_t alloc_inode();   // ~0ULL = full
    void     free_inode(uint64_t inum);
    bool     test_inode(uint64_t inum);

    void     shutdown_all();
    bool     is_valid() const { return all_controller != nullptr; }

private:
    const BlockDevice   *dev;
    uint64_t             bitmap_base;
    uint64_t             bitmap_block_count;
    uint64_t             inode_array_upper;
    uint8_t              N;

    uint64_t             ctrl_count;
    uint64_t             bucket_sz;
    uint64_t             bucket_bits;
    uint64_t             ctrl_seg_bits;

    BitmapBlockController *all_controller;       // [ctrl_count]
    ProcessorBitmapState   proc;                 // single processor
    uint8_t               *bucket;               // single bucket

    void load_bucket(uint64_t cid);
    void flush_bucket(uint64_t cid);
    void consume_head(BitmapBlockController *ctrl);

    uint64_t try_alloc();
    void     switch_to(uint64_t new_cid);

    static uint64_t scan_run(const uint8_t *buf, uint64_t buf_bits, uint64_t start);
    static void     set_bits(uint8_t *buf, uint64_t start, uint64_t count);
    static void     clear_bits(uint8_t *buf, uint64_t start, uint64_t count);

    BitmapFields decode(uint64_t inum) const;
    uint64_t     encode(uint64_t cid, uint64_t bucket_pos) const;
};

/* ═════════════════════════════════════════════════W══════════════
 * RavenFS — 上帝类 v1 (Phase 1)
 *
 * RAII 风格：构造即挂载，析构即卸载
 * 错误码通过 res_code 输出，0 = 成功
 *
 * 使用方式：
 *   uint64_t rc;
 *   RavenFS fs("/dev/nvme1n1", &rc);
 *   if (rc) { ... }
 *   // 作用域结束自动 unmount
 * ═══════════════════════════════════════════════════════════════ */
class RavenFS {
public:
    struct DirEntryInfo {
        uint64_t inode;
        char     name[120];
    };

    /* ─── 构造即挂载（设备路径 + 错误码输出） ─── */
    RavenFS(const char *device_path, uint64_t *res_code);
    ~RavenFS();

    /* 禁止拷贝/移动（RAII 有打开的文件描述符） */
    RavenFS(const RavenFS &) = delete;
    RavenFS &operator=(const RavenFS &) = delete;

    /* ─── 手动卸载（可选，否则析构自动调） ─── */
    void unmount();
    bool is_mounted() const { return bdev.fd >= 0; }

    /* ─── 目录操作（0=成功，负值=-errno） ─── */
    int  mkdir(const char *path);
    int  rmdir(const char *path);
    int  readdir(const char *path, DirEntryInfo *entries, int max_entries);
    int  readdir_inum(uint64_t inum, DirEntryInfo *entries, int max_entries);

    /* ─── 路径解析（公开供测试验证） ─── */
    int  path_resolve(const char *path, uint64_t *out_inum);

    /* ─── 查询 inode 类型（return INODE_TYPE_* / -errno） ─── */
    int  inode_type(uint64_t inum);

    /* ─── 调试 ─── */
    void dump_info() const;

    /* ─── Dentry B+tree 全量校验（从根递归，走块缓存） ─── */
    int validate_dentry_tree();

    /* ─── InodeSession — RAII inode session ─── */
    class InodeSession {
        RavenFS  *fs;
        uint64_t  inum_;
    public:
        raven_Inode_t *inode;

        InodeSession(RavenFS *fs, uint64_t inum)
            : fs(fs), inum_(inum), inode(nullptr) {
            if (fs && fs->icache)
                inode = fs->icache->cache_get(inum);
        }

        ~InodeSession() {
            if (fs && fs->icache && inode)
                fs->icache->cache_put(inum_);
        }

        InodeSession(const InodeSession &) = delete;
        InodeSession &operator=(const InodeSession &) = delete;

        InodeSession(InodeSession &&other) noexcept
            : fs(other.fs), inum_(other.inum_), inode(other.inode) {
            other.fs    = nullptr;
            other.inode = nullptr;
        }

        InodeSession &operator=(InodeSession &&other) noexcept {
            if (this != &other) {
                if (fs && fs->icache && inode) fs->icache->cache_put(inum_);
                fs    = other.fs;
                inum_ = other.inum_;
                inode = other.inode;
                other.fs    = nullptr;
                other.inode = nullptr;
            }
            return *this;
        }

        void dirty() {
            if (fs && fs->icache && inode)
                fs->icache->mark_dirty(inum_);
        }

        uint64_t inum()    const { return inum_; }
        bool     ok()      const { return inode != nullptr; }
        explicit operator bool() const { return ok(); }
    };

private:
    /* ═══════════════════════════════════════════════════════════
     * CRC32
     * ═══════════════════════════════════════════════════════════ */
    static uint32_t crc32_table[256];
    static bool     crc32_table_init;
    static void     crc32_init();
    static uint32_t crc32_compute(const void *data, size_t len);

    /* ═══════════════════════════════════════════════════════════
     * I/O 底层
     * ═══════════════════════════════════════════════════════════ */
    uint64_t fblock_to_byte(uint64_t fblock) const;

    void read_raw(uint64_t byte_off, void *buf, size_t len);
    void write_raw(uint64_t byte_off, const void *buf, size_t len);

    void read_block(uint64_t fblock, void *buf);
    void write_block(uint64_t fblock, const void *buf);

    /* B+tree 节点 = 8K = 2 个连续 FS_block */
    void read_btnode(uint64_t fblock, void *node, size_t node_size);
    void write_btnode(uint64_t fblock, const void *node, size_t node_size);

    /* ═══════════════════════════════════════════════════════════
     * 超级块
     * ═══════════════════════════════════════════════════════════ */
    uint64_t init_mount(const char *device_path);

    /* ═══════════════════════════════════════════════════════════
     * 位图校验直读（仅 validate 使用，不走 bcache/ibcache）
     * ═══════════════════════════════════════════════════════════ */
    bool   bitmap_test(uint64_t fblock);
    bool   ibitmap_test(uint64_t inum);

    /* ═══════════════════════════════════════════════════════════
     * Inode 数组
     * ═══════════════════════════════════════════════════════════ */
    void inode_read(uint64_t inum, raven_Inode_t *inode);
    void inode_write(uint64_t inum, const raven_Inode_t *inode);

    /* ═══════════════════════════════════════════════════════════
     * Dentry B+tree
     * ═══════════════════════════════════════════════════════════ */
    bool dentry_lookup(uint64_t dir_inode, const char *name, uint64_t *out_inum);
    bool dentry_insert(uint64_t dir_inode, const char *name, uint64_t target_inum);
    bool dentry_remove(uint64_t dir_inode, const char *name);
    int  dentry_enumerate(uint64_t dir_inode, DirEntryInfo *entries, int max_entries);
    int  dentry_node_find(const Dentry_Bptree_node_t *node, const char *name);
    void dentry_node_shift_entries(Dentry_Bptree_node_t *node, int start, int dir);

    /* ═══════════════════════════════════════════════════════════
     * PathCursor — 路径解析状态机
     *
     * 构造即从根开始，step() 逐组件前进，记录 pos/cur_inode。
     * walk_all() = 走完全部 → 目标 inode
     * walk_parent() = 走到倒数第二个 → 父目录 inode, leaf() 返回末组件名
     *
     * 用法:
     *   PathCursor cur(*this, "/a/b/c");
     *   if (!cur.walk_parent()) return false;
     *   // cur.inode() = inode of /a/b
     *   // cur.leaf() = "c"
     * ═══════════════════════════════════════════════════════════ */
    class PathCursor {
    public:
        PathCursor(RavenFS &fs, const char *path);

        /* 前进一个组件，返回 false 表示该组件不存在 */
        bool step();

        /* 走完全部组件 → cur_inode = 目标 inode */
        bool walk_all();

        /* 走到倒数第二个组件 → cur_inode = 父目录, leaf() 有值 */
        bool walk_parent();

        /* 状态查询 */
        uint64_t    inode() const { return cur_inode; }
        const char *leaf()  const { return leaf_buf; }
        int         depth() const { return ncomps; }
        int         pos()   const { return consumed; }
        bool        ok()    const { return good; }
        int         err()   const { return path_err; }  /* 最近一次失败时的 -errno */

    private:
        RavenFS    &fs_ref;
        uint64_t    cur_inode = 0;       // 当前所在的 inode（从 root 开始）
        uint64_t    root_inum = 0;
        const char *comps[64];           // 组件指针（指向 path_copy 内）
        size_t      comp_lens[64];
        int         ncomps      = 0;     // 总组件数
        int         consumed    = 0;     // 已走过的组件数
        bool        good        = false; // 路径语法正确
        bool        finished    = false;
        int         path_err    = 0;      // 最近失败时的 -errno

        char        path_copy[512];
        char        leaf_buf[120];       // walk_parent 时存末组件
    };



    void validate_subtree(uint64_t block, uint64_t expected_parent,
                          int depth, std::set<uint64_t> &visited,
                          std::map<uint64_t, Dentry_Bptree_node_t> &pin);

    /* ═══════════════════════════════════════════════════════════
     * 挂载校验
     * ═══════════════════════════════════════════════════════════ */
    uint64_t validate_world_base();
    uint64_t validate_sb_crc32();
    uint64_t validate_backup_chain();
    uint64_t validate_bitmap_self_consistency();

    /* v2 inode cache — 内存由上帝类分配，控制器只使用 */
    static const uint64_t INODE_CACHE_SETS = 256;
    static const uint64_t INODE_CACHE_WAYS = 8;
    controller_set          *cache_entries = nullptr;  // [sets]
    Inode_cache_line        *cache_data    = nullptr;   // [total_slots]
    Inode_cache_controller  *icache        = nullptr;

    /* ═══════════════════════════════════════════════════════════
     * 缓存 — Bitmap Cache（分片 write-back, per-processor 单桶）
     * ═══════════════════════════════════════════════════════════ */
    BitmapCacheController      *bcache        = nullptr;

    /* ═══════════════════════════════════════════════════════════
     * 缓存 — Inode Bitmap Cache（分片 write-back，单核简化）
     * ═══════════════════════════════════════════════════════════ */
    InodeBitmapCacheController *ibcache       = nullptr;

    /* ═══════════════════════════════════════════════════════════
     * 缓存 — 通用 block cache（4K 块，直写直读）
     *
     * 4096 entries × 4KB = 16MB, 直映射 (fblock % N)
     * 读命中直接返回；写命中则更新并标记无效；
     * 覆盖所有 read_block / write_block。
     * ═══════════════════════════════════════════════════════════ */
    static const int BLOCK_CACHE_ENTRIES = 4096;
    struct BlockCacheEntry {
        uint64_t block;
        uint8_t  data[4096];
        bool     valid;
    };
    BlockCacheEntry *block_cache = nullptr;   // [BLOCK_CACHE_ENTRIES], heap

    /* ═══════════════════════════════════════════════════════════
     * 状态
     * ═══════════════════════════════════════════════════════════ */
    BlockDevice bdev = { -1, 0, 0 };
    uint64_t    root_inum = 0;

    FsGeometry geo;
    RavenSuperblock sb;
};

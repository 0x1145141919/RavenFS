#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <map>
#include <set>
#include <string>
#include <vector>

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

/* ═══════════════════════════════════════════════════════════════
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
     * FS_block 位图
     * ═══════════════════════════════════════════════════════════ */
    bool   bitmap_test(uint64_t fblock);
    void   bitmap_set(uint64_t fblock);
    void   bitmap_clear(uint64_t fblock);
    uint64_t bitmap_alloc_block();
    uint64_t bitmap_alloc_blocks(uint64_t count);

    /* ═══════════════════════════════════════════════════════════
     * Inode 位图
     * ═══════════════════════════════════════════════════════════ */
    bool   ibitmap_test(uint64_t inum);
    void   ibitmap_set(uint64_t inum);
    void   ibitmap_clear(uint64_t inum);
    uint64_t ibitmap_alloc_inode();

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

    /* ═══════════════════════════════════════════════════════════
     * 缓存 — Bitmap（全量 DRAM）
     * ═══════════════════════════════════════════════════════════ */
    uint8_t *bitmap_cache   = nullptr;
    uint8_t *ibitmap_cache  = nullptr;
    bool     bitmap_dirty   = false;
    bool     ibitmap_dirty  = false;
    uint64_t bitmap_alloc_hint  = 0;   // 上次成功分配 FS_block 的下一位
    uint64_t ibitmap_alloc_hint = 1;   // 上次成功分配 inode 的下一位

    /* ═══════════════════════════════════════════════════════════
     * 缓存 — Inode Table（组相联写回，堆分配）
     *
     * 线路 = 4KB（16 × 256B inodes）
     * 256 sets × 8 ways = 2048 lines, 总计 8MB
     * ═══════════════════════════════════════════════════════════ */
    static const uint64_t INODE_CACHE_SETS = 256;
    static const uint64_t INODE_CACHE_WAYS = 8;

    struct InodeCacheLine {
        uint8_t  data[4096];
        uint64_t tag;
        bool     valid;
        bool     dirty;
    };
    InodeCacheLine *inode_cache = nullptr;  // [sets][ways], heap
    uint8_t        *inode_evict_next = nullptr;  // [sets]

    void cache_load();          // 装载 bitmaps + 重置 inode cache
    void cache_flush();         // 回写所有脏数据
    void inode_cache_load(uint64_t abs_fblock, uint8_t *buf);
    void inode_cache_commit(uint64_t abs_fblock, const uint8_t *buf);

    // v2 inode cache — 内存由上帝类分配，控制器只使用
    controller_set          *cache_entries = nullptr;  // [sets]
    Inode_cache_line        *cache_data    = nullptr;   // [total_slots]
    Inode_cache_controller  *icache        = nullptr;

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

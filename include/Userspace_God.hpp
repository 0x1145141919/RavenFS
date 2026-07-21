#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "disk_struct.hpp"
#include "magics.h"

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
    bool is_mounted() const { return dev_fd >= 0; }

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
    int     dev_fd = -1;

    uint64_t lba_size       = 0;
    uint64_t multiplier     = 0;
    uint64_t world_base_lba = 0;
    uint64_t root_inum      = 0;

    FsGeometry geo;
    RavenSuperblock sb;
};

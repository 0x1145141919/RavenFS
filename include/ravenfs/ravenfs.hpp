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

#include "error.hpp"
#include "geometry.hpp"
#include "block_device.hpp"
#include "inode_cache.hpp"
#include "bitmap_types.hpp"
#include "bitmap_cache.hpp"
#include "inode_bitmap_cache.hpp"
#include "../disk_struct.hpp"
#include "../magics.h"

class RavenFS {
public:
    struct DirEntryInfo {
        uint64_t inode;
        char     name[120];
    };

    RavenFS(const char *device_path, uint64_t *res_code);
    ~RavenFS();

    RavenFS(const RavenFS &) = delete;
    RavenFS &operator=(const RavenFS &) = delete;

    void unmount();
    bool is_mounted() const { return bdev.fd >= 0; }

    int  mkdir(const char *path);
    int  rmdir(const char *path);
    int  readdir(const char *path, DirEntryInfo *entries, int max_entries);
    int  readdir_inum(uint64_t inum, DirEntryInfo *entries, int max_entries);

    int  path_resolve(const char *path, uint64_t *out_inum);

    int  inode_type(uint64_t inum);

    void dump_info() const;

    int validate_dentry_tree();

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
    static uint32_t crc32_table[256];
    static bool     crc32_table_init;
    static void     crc32_init();
    static uint32_t crc32_compute(const void *data, size_t len);

    uint64_t fblock_to_byte(uint64_t fblock) const;

    void read_raw(uint64_t byte_off, void *buf, size_t len);
    void write_raw(uint64_t byte_off, const void *buf, size_t len);

    void read_block(uint64_t fblock, void *buf);
    void write_block(uint64_t fblock, const void *buf);

    void read_btnode(uint64_t fblock, void *node, size_t node_size);
    void write_btnode(uint64_t fblock, const void *node, size_t node_size);

    uint64_t init_mount(const char *device_path);

    bool   bitmap_test(uint64_t fblock);
    bool   ibitmap_test(uint64_t inum);

    void inode_read(uint64_t inum, raven_Inode_t *inode);
    void inode_write(uint64_t inum, const raven_Inode_t *inode);

    bool dentry_lookup(uint64_t dir_inode, const char *name, uint64_t *out_inum);
    bool dentry_insert(uint64_t dir_inode, const char *name, uint64_t target_inum);
    bool dentry_remove(uint64_t dir_inode, const char *name);
    int  dentry_enumerate(uint64_t dir_inode, DirEntryInfo *entries, int max_entries);
    int  dentry_node_find(const Dentry_Bptree_node_t *node, const char *name);
    void dentry_node_shift_entries(Dentry_Bptree_node_t *node, int start, int dir);

    class PathCursor {
    public:
        PathCursor(RavenFS &fs, const char *path);

        bool step();

        bool walk_all();

        bool walk_parent();

        uint64_t    inode() const { return cur_inode; }
        const char *leaf()  const { return leaf_buf; }
        int         depth() const { return ncomps; }
        int         pos()   const { return consumed; }
        bool        ok()    const { return good; }
        int         err()   const { return path_err; }

    private:
        RavenFS    &fs_ref;
        uint64_t    cur_inode = 0;
        uint64_t    root_inum = 0;
        const char *comps[64];
        size_t      comp_lens[64];
        int         ncomps      = 0;
        int         consumed    = 0;
        bool        good        = false;
        bool        finished    = false;
        int         path_err    = 0;

        char        path_copy[512];
        char        leaf_buf[120];
    };

    void validate_subtree(uint64_t block, uint64_t expected_parent,
                          int depth, std::set<uint64_t> &visited,
                          std::map<uint64_t, Dentry_Bptree_node_t> &pin);

    uint64_t validate_world_base();
    uint64_t validate_sb_crc32();
    uint64_t validate_backup_chain();
    uint64_t validate_bitmap_self_consistency();

    static const uint64_t INODE_CACHE_SETS = 256;
    static const uint64_t INODE_CACHE_WAYS = 8;
    controller_set          *cache_entries = nullptr;
    Inode_cache_line        *cache_data    = nullptr;
    Inode_cache_controller  *icache        = nullptr;

    BitmapCacheController      *bcache        = nullptr;

    InodeBitmapCacheController *ibcache       = nullptr;

    static const int BLOCK_CACHE_ENTRIES = 4096;
    struct BlockCacheEntry {
        uint64_t block;
        uint8_t  data[4096];
        bool     valid;
    };
    BlockCacheEntry *block_cache = nullptr;

    BlockDevice bdev = { -1, 0, 0 };
    uint64_t    root_inum = 0;

    FsGeometry geo;
    RavenSuperblock sb;
};

#pragma once

#include <cstdint>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <mutex>

#include <boost/intrusive/unordered_set.hpp>
#include <boost/intrusive/list.hpp>

#include "../util/lock.h"

namespace bi = boost::intrusive;

/* ─── Forward declarations ─── */
struct DentryEntryCache;

/* ═══════════════════════════════════════════════════════════════
 * hash_mix — Thomas Wang 64-bit hash mix
 * ═══════════════════════════════════════════════════════════════ */
static inline uint64_t hash_mix(uint64_t key) {
    key = ~key + (key << 21);
    key = key ^ (key >> 24);
    key = (key + (key << 3)) + (key << 8);
    key = key ^ (key >> 14);
    key = (key + (key << 2)) + (key << 4);
    key = key ^ (key >> 28);
    key = key + (key << 31);
    return key;
}

static inline uint32_t name_hash(const char *name) {
    uint32_t h = 0;
    while (*name) {
        h = h * 31 + (unsigned char)*name++;
    }
    return h;
}

/* ═══════════════════════════════════════════════════════════════
 * LogicalInode — 运行时元数据，不持 raven_Inode_t 物理数据
 * ═══════════════════════════════════════════════════════════════ */
struct LogicalInode {
    /* ── 侵入式容器 Hook ── */
    bi::unordered_set_member_hook<>    inum_hash_hook;
    bi::list_member_hook<>             lru_hook;
    bi::list_member_hook<>             dirty_hook;

    /* ── 运行时元数据 ── */
    uint64_t             inum;
    std::atomic<int>     refcnt{0};
    bool                 dirty  : 1 = false;
    bool                 loaded : 1 = false;
    std::mutex           mtx;

    /* ── 目录专用（未来 per-dir dentry cache 优化） ── */
    DentryEntryCache    *dentry_cache = nullptr;
};

/* ═══════════════════════════════════════════════════════════════
 * DentryEntry — name → inum 映射
 * ═══════════════════════════════════════════════════════════════ */
struct DentryEntry {
    /* ── 侵入式容器 Hook ── */
    bi::unordered_set_member_hook<>    hash_hook;
    bi::list_member_hook<>             lru_hook;

    /* ── 键 ── */
    uint64_t    parent_inum;
    uint32_t    hash_cache;
    uint64_t    target_inum;

    /* ── 名字存储 ── */
    char        name[48];
    char       *long_name = nullptr;
};

/* ── dentry 查找用的临时 key（避免创建完整 DentryEntry） ── */
struct DentryEntryKey {
    uint64_t    parent_inum;
    uint32_t    hash_cache;
    const char *name;
};

/* ═══════════════════════════════════════════════════════════════
 * 哈希 / 相等 函数对象
 * ═══════════════════════════════════════════════════════════════ */

/* ── inum_hashtable ── */
struct InumHash {
    std::size_t operator()(const LogicalInode &v) const { return hash_mix(v.inum); }
    std::size_t operator()(uint64_t inum) const         { return hash_mix(inum); }
};

struct InumEqual {
    bool operator()(const LogicalInode &a, const LogicalInode &b) const { return a.inum == b.inum; }
    bool operator()(uint64_t inum, const LogicalInode &b) const         { return inum == b.inum; }
    bool operator()(const LogicalInode &a, uint64_t inum) const         { return a.inum == inum; }
};

/* ── dentry_hashtable ── */
struct DentryEntryHash {
    std::size_t operator()(const DentryEntry &v) const {
        return hash_mix((uint64_t)v.hash_cache ^ hash_mix(v.parent_inum));
    }
    std::size_t operator()(const DentryEntryKey &k) const {
        return hash_mix((uint64_t)k.hash_cache ^ hash_mix(k.parent_inum));
    }
};

struct DentryEntryEqual {
    bool operator()(const DentryEntry &a, const DentryEntry &b) const {
        return a.parent_inum == b.parent_inum
            && a.hash_cache == b.hash_cache
            && strcmp(a.long_name ? a.long_name : a.name,
                      b.long_name ? b.long_name : b.name) == 0;
    }
    bool operator()(const DentryEntryKey &k, const DentryEntry &b) const {
        return k.parent_inum == b.parent_inum
            && k.hash_cache == b.hash_cache
            && strcmp(k.name, b.long_name ? b.long_name : b.name) == 0;
    }
    bool operator()(const DentryEntry &a, const DentryEntryKey &k) const {
        return a.parent_inum == k.parent_inum
            && a.hash_cache == k.hash_cache
            && strcmp(a.long_name ? a.long_name : a.name, k.name) == 0;
    }
};

/* ═══════════════════════════════════════════════════════════════
 * 容器 typedef
 * ═══════════════════════════════════════════════════════════════ */

/* ── inum_hashtable ── */
typedef bi::unordered_set<
    LogicalInode,
    bi::member_hook<
        LogicalInode,
        bi::unordered_set_member_hook<>,
        &LogicalInode::inum_hash_hook
    >,
    bi::hash<InumHash>,
    bi::equal<InumEqual>,
    bi::constant_time_size<false>
> InumHashSet;

/* ── inode LRU 列表 ── */
typedef bi::list<
    LogicalInode,
    bi::member_hook<
        LogicalInode,
        bi::list_member_hook<>,
        &LogicalInode::lru_hook
    >,
    bi::constant_time_size<false>
> InodeLRUList;

/* ── dirty 列表 ── */
typedef bi::list<
    LogicalInode,
    bi::member_hook<
        LogicalInode,
        bi::list_member_hook<>,
        &LogicalInode::dirty_hook
    >,
    bi::constant_time_size<false>
> DirtyList;

/* ── dentry_hashtable ── */
typedef bi::unordered_set<
    DentryEntry,
    bi::member_hook<
        DentryEntry,
        bi::unordered_set_member_hook<>,
        &DentryEntry::hash_hook
    >,
    bi::hash<DentryEntryHash>,
    bi::equal<DentryEntryEqual>,
    bi::constant_time_size<false>
> DentryHashSet;

/* ── dentry LRU 列表 ── */
typedef bi::list<
    DentryEntry,
    bi::member_hook<
        DentryEntry,
        bi::list_member_hook<>,
        &DentryEntry::lru_hook
    >,
    bi::constant_time_size<false>
> DentryLRUList;

/* ═══════════════════════════════════════════════════════════════
 * VFSCache — 聚合缓存状态，便于管理
 * ═══════════════════════════════════════════════════════════════ */
struct VFSCache {
    /* ── 配置 ── */
    uint8_t      inum_hash_shift   = 18;
    uint8_t      dentry_hash_shift = 20;

    /* ── 哈希表 ── */
    InumHashSet::bucket_type  *inum_buckets    = nullptr;
    uint64_t                   inum_bucket_cnt = 0;
    InumHashSet               *inum_set        = nullptr;

    DentryHashSet::bucket_type *dentry_buckets    = nullptr;
    uint64_t                    dentry_bucket_cnt = 0;
    DentryHashSet              *dentry_set        = nullptr;

    /* ── LRU 列表 ── */
    InodeLRUList    inode_lru;
    DirtyList       dirty_list;
    DentryLRUList   dentry_lru;

    /* ── 自旋锁：保护 VFS cache 全局结构（dentry_set / dentry_lru / inum_set / inode_lru / dirty_list） ── */
    spinlock_cpp_t      cache_lock;

    /* ── 统计 ── */
    std::atomic<long> nr_logical_inodes{0};
    std::atomic<long> nr_logical_unused{0};
    std::atomic<long> nr_dentries{0};

    bool init(uint64_t estimated_inodes, uint64_t estimated_dentries);
    void destroy();
};

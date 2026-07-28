#include "../include/ravenfs/ravenfs.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>

/* ═══════════════════════════════════════════════════════════════
 * VFSCache::init — 分配 hashtable bucket 数组
 * ═══════════════════════════════════════════════════════════════ */
bool VFSCache::init(uint64_t estimated_inodes, uint64_t estimated_dentries) {
    /* 计算 bucket 数：上取整到 2 的幂，负载因子 ~1/8 */
    auto calc_shift = [](uint64_t n) -> uint8_t {
        uint64_t target = n / 8;
        if (target == 0) target = 64;
        uint8_t shift = 6;
        while ((1ULL << shift) < target) shift++;
        return shift;
    };

    inum_hash_shift   = calc_shift(estimated_inodes);
    dentry_hash_shift = calc_shift(estimated_dentries);

    inum_bucket_cnt   = 1ULL << inum_hash_shift;
    dentry_bucket_cnt = 1ULL << dentry_hash_shift;

    /* 分配 bucket 数组（零初始化） */
    inum_buckets = new InumHashSet::bucket_type[inum_bucket_cnt]();
    if (!inum_buckets) return false;

    dentry_buckets = new DentryHashSet::bucket_type[dentry_bucket_cnt]();
    if (!dentry_buckets) {
        delete[] inum_buckets;
        inum_buckets = nullptr;
        return false;
    }

    /* 创建哈希表（hash/equal 已通过模板参数注入） */
    inum_set = new InumHashSet(
        InumHashSet::bucket_traits(inum_buckets, inum_bucket_cnt)
    );

    dentry_set = new DentryHashSet(
        DentryHashSet::bucket_traits(dentry_buckets, dentry_bucket_cnt)
    );

    return true;
}

/* ═══════════════════════════════════════════════════════════════
 * VFSCache::destroy — 回收所有节点，释放 bucket 数组
 * ═══════════════════════════════════════════════════════════════ */
void VFSCache::destroy() {
    /* 回收所有 LogicalInode */
    while (!inode_lru.empty()) {
        LogicalInode *li = &inode_lru.front();
        inode_lru.pop_front();
        if (li->inum_hash_hook.is_linked())
            inum_set->erase(*li);
        delete li;
    }
    while (!dirty_list.empty()) {
        LogicalInode *li = &dirty_list.front();
        dirty_list.pop_front();
        if (li->inum_hash_hook.is_linked())
            inum_set->erase(*li);
        delete li;
    }
    /* inum_set 中残余（refcnt > 0）泄漏警告 */
    if (inum_set && !inum_set->empty()) {
        fprintf(stderr, "RavenFS VFSCache: %zu LogicalInodes leaked (refcnt > 0)\n",
                inum_set->size());
        auto it = inum_set->begin();
        while (it != inum_set->end()) {
            LogicalInode *li = &*it;
            ++it;
            inum_set->erase(*li);
            delete li;
        }
    }

    /* 回收所有 DentryEntry */
    while (!dentry_lru.empty()) {
        DentryEntry *de = &dentry_lru.front();
        dentry_lru.pop_front();
        if (de->hash_hook.is_linked())
            dentry_set->erase(*de);
        if (de->long_name)
            free(de->long_name);
        delete de;
    }
    if (dentry_set && !dentry_set->empty()) {
        fprintf(stderr, "RavenFS VFSCache: %zu DentryEntries leaked\n",
                dentry_set->size());
        auto it = dentry_set->begin();
        while (it != dentry_set->end()) {
            DentryEntry *de = &*it;
            ++it;
            dentry_set->erase(*de);
            if (de->long_name) free(de->long_name);
            delete de;
        }
    }

    delete inum_set;
    inum_set = nullptr;
    delete[] inum_buckets;
    inum_buckets = nullptr;

    delete dentry_set;
    dentry_set = nullptr;
    delete[] dentry_buckets;
    dentry_buckets = nullptr;
}

/* ═══════════════════════════════════════════════════════════════
 * RavenFS::vfs_cache_init / destroy — 薄 wrapper
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS::vfs_cache_init() {
    vcache.init(sb.inode_array_count, sb.inode_array_count * 4);
}

void RavenFS::vfs_cache_destroy() {
    /* 先清 dirty_list：保证物理缓存已刷 */
    while (!vcache.dirty_list.empty()) {
        LogicalInode *li = &vcache.dirty_list.front();
        vcache.dirty_list.pop_front();
        li->dirty = false;
        if (li->refcnt == 0) {
            if (li->inum_hash_hook.is_linked())
                vcache.inum_set->erase(*li);
            delete li;
        }
    }
    vcache.destroy();
}

/* ═══════════════════════════════════════════════════════════════
 * logical_inode_get — 查找 or 创建 LogicalInode，refcnt++
 *
 * 自旋锁（vcache.cache_lock）保护 inum_set / inode_lru 访问。
 * 内存分配在锁外进行。双检锁 pattern。
 * ═══════════════════════════════════════════════════════════════ */
LogicalInode *RavenFS::logical_inode_get(uint64_t inum) {
    /* Phase 1: 快速查表（自旋锁） */
    {
        spinlock_guard g(vcache.cache_lock);
        auto it = vcache.inum_set->find(inum, InumHash(), InumEqual());
        if (it != vcache.inum_set->end()) {
            LogicalInode *li = &*it;
            li->refcnt++;
            if (li->lru_hook.is_linked()) {
                vcache.inode_lru.erase(vcache.inode_lru.iterator_to(*li));
                vcache.nr_logical_unused--;
            }
            return li;
        }
    }

    /* Phase 2: 未命中 → 构造新节点（不持锁，new 可能阻塞） */
    LogicalInode *li = new LogicalInode();
    li->inum   = inum;
    li->refcnt = 1;
    li->dirty  = false;
    li->loaded = false;

    /* Phase 3: 重新查 + 插入（自旋锁，双检） */
    {
        spinlock_guard g(vcache.cache_lock);

        /* 双检：释放锁期间可能已被他人插入 */
        auto it = vcache.inum_set->find(inum, InumHash(), InumEqual());
        if (it != vcache.inum_set->end()) {
            delete li;
            LogicalInode *existing = &*it;
            existing->refcnt++;
            if (existing->lru_hook.is_linked()) {
                vcache.inode_lru.erase(vcache.inode_lru.iterator_to(*existing));
                vcache.nr_logical_unused--;
            }
            return existing;
        }

        auto commit_data = InumHashSet::insert_commit_data();
        auto res = vcache.inum_set->insert_check(inum, InumHash(), InumEqual(), commit_data);
        if (!res.second) {
            delete li;
            li = &*res.first;
            li->refcnt++;
            if (li->lru_hook.is_linked()) {
                vcache.inode_lru.erase(vcache.inode_lru.iterator_to(*li));
                vcache.nr_logical_unused--;
            }
            return li;
        }
        vcache.inum_set->insert_commit(*li, commit_data);
        vcache.nr_logical_inodes++;
    }

    return li;
}

/* ═══════════════════════════════════════════════════════════════
 * logical_inode_put — refcnt--，refcnt==0 时挂 LRU 或 dirty_list
 *
 * refcnt 是 atomic，减到 0 后挂链表需要自旋锁保护。
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS::logical_inode_put(LogicalInode *li) {
    int prev = li->refcnt--;
    if (prev <= 0) {
        fprintf(stderr, "RavenFS: logical_inode_put refcnt underflow (inum=%lu)\n",
                (unsigned long)li->inum);
        li->refcnt = 0;
        return;
    }
    if (li->refcnt > 0)
        return;

    /* refcnt == 0 → 可 evict */
    spinlock_guard g(vcache.cache_lock);
    vcache.nr_logical_unused++;
    if (li->dirty) {
        if (!li->dirty_hook.is_linked())
            vcache.dirty_list.push_back(*li);
    } else {
        if (!li->lru_hook.is_linked())
            vcache.inode_lru.push_front(*li);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * dentry_cache_lookup — 查 dentry 缓存，miss 则回退 B+tree
 *
 * 两阶段锁：
 *   1) 自旋锁（vcache.cache_lock）查缓存 —— 命中即回
 *   2) cache miss → IO（caller 应持 per-inode mutex）→ 自旋锁写缓存
 *
 * 返回 target_inum（≠ ~0ULL 表示命中）
 * ═══════════════════════════════════════════════════════════════ */
uint64_t RavenFS::dentry_cache_lookup(uint64_t parent_inum, const char *name) {
    uint32_t h = name_hash(name);

    /* Phase 1: 查缓存（自旋锁，无 IO） */
    {
        spinlock_guard g(vcache.cache_lock);
        DentryEntryKey key;
        key.parent_inum = parent_inum;
        key.hash_cache  = h;
        key.name        = name;

        auto it = vcache.dentry_set->find(key, DentryEntryHash(), DentryEntryEqual());
        if (it != vcache.dentry_set->end()) {
            DentryEntry *de = &*it;
            if (de->lru_hook.is_linked())
                vcache.dentry_lru.erase(vcache.dentry_lru.iterator_to(*de));
            vcache.dentry_lru.push_front(*de);
            return de->target_inum;
        }
    }

    /* Phase 2: cache miss → B+tree 读盘（IO，caller 应持 per-inode mutex） */
    uint64_t target_inum;
    if (!dentry_lookup(parent_inum, name, &target_inum))
        return ~0ULL;

    /* Phase 3: 填充缓存（自旋锁） */
    DentryEntry *de = new DentryEntry();
    de->parent_inum = parent_inum;
    de->hash_cache  = h;
    de->target_inum = target_inum;

    size_t nlen = strlen(name);
    if (nlen < 48) {
        memcpy(de->name, name, nlen + 1);
        de->long_name = nullptr;
    } else {
        de->name[0] = '\0';
        de->long_name = (char *)malloc(nlen + 1);
        if (de->long_name)
            memcpy(de->long_name, name, nlen + 1);
    }

    {
        spinlock_guard g(vcache.cache_lock);
        DentryEntryKey key;
        key.parent_inum = parent_inum;
        key.hash_cache  = h;
        key.name        = name;

        auto commit_data = DentryHashSet::insert_commit_data();
        auto res = vcache.dentry_set->insert_check(key, DentryEntryHash(),
                                                     DentryEntryEqual(), commit_data);
        if (res.second) {
            vcache.dentry_set->insert_commit(*de, commit_data);
            vcache.dentry_lru.push_front(*de);
            vcache.nr_dentries++;
        } else {
            if (de->long_name) free(de->long_name);
            delete de;
            DentryEntry *existing = &*res.first;
            if (existing->lru_hook.is_linked())
                vcache.dentry_lru.erase(vcache.dentry_lru.iterator_to(*existing));
            vcache.dentry_lru.push_front(*existing);
        }
    }

    return target_inum;
}

/* ═══════════════════════════════════════════════════════════════
 * dentry_cache_insert — 插入 dentry cache + B+tree
 *
 * 两阶段锁：
 *   1) B+tree 写盘（IO，caller 应持 per-inode mutex）
 *   2) 自旋锁（vcache.cache_lock）更新缓存
 *
 * 返回 false 表示 B+tree 层插入失败（磁盘满等）
 * ═══════════════════════════════════════════════════════════════ */
bool RavenFS::dentry_cache_insert(uint64_t parent_inum, const char *name,
                                   uint64_t target_inum) {
    uint32_t h = name_hash(name);

    /* Phase 1: B+tree 写盘（IO，caller 应持 per-inode mutex） */
    if (!dentry_insert(parent_inum, name, target_inum))
        return false;

    /* Phase 2: 更新缓存（自旋锁） */
    DentryEntry *de = new DentryEntry();
    de->parent_inum = parent_inum;
    de->hash_cache  = h;
    de->target_inum = target_inum;

    size_t nlen = strlen(name);
    if (nlen < 48) {
        memcpy(de->name, name, nlen + 1);
        de->long_name = nullptr;
    } else {
        de->name[0] = '\0';
        de->long_name = (char *)malloc(nlen + 1);
        if (de->long_name)
            memcpy(de->long_name, name, nlen + 1);
    }

    {
        spinlock_guard g(vcache.cache_lock);

        DentryEntryKey key;
        key.parent_inum = parent_inum;
        key.hash_cache  = h;
        key.name        = name;

        auto it = vcache.dentry_set->find(key, DentryEntryHash(), DentryEntryEqual());
        if (it != vcache.dentry_set->end()) {
            DentryEntry *existing = &*it;
            existing->target_inum = target_inum;
            if (existing->lru_hook.is_linked())
                vcache.dentry_lru.erase(vcache.dentry_lru.iterator_to(*existing));
            vcache.dentry_lru.push_front(*existing);
            if (de->long_name) free(de->long_name);
            delete de;
            return true;
        }

        auto commit_data = DentryHashSet::insert_commit_data();
        auto res = vcache.dentry_set->insert_check(key, DentryEntryHash(),
                                                     DentryEntryEqual(), commit_data);
        if (res.second) {
            vcache.dentry_set->insert_commit(*de, commit_data);
            vcache.dentry_lru.push_front(*de);
            vcache.nr_dentries++;
        } else {
            DentryEntry *existing = &*res.first;
            existing->target_inum = target_inum;
            if (existing->lru_hook.is_linked())
                vcache.dentry_lru.erase(vcache.dentry_lru.iterator_to(*existing));
            vcache.dentry_lru.push_front(*existing);
            if (de->long_name) free(de->long_name);
            delete de;
        }
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════
 * dentry_cache_remove — 从缓存 + B+tree 删除 dentry
 *
 * 两阶段锁：
 *   1) 自旋锁（vcache.cache_lock）摘缓存
 *   2) B+tree 写墓碑（IO，caller 应持 per-inode mutex）
 *
 * 返回 false 表示 B+tree 层删除失败（如条目不存在）
 * ═══════════════════════════════════════════════════════════════ */
bool RavenFS::dentry_cache_remove(uint64_t parent_inum, const char *name) {
    uint32_t h = name_hash(name);

    /* Phase 1: 摘缓存（自旋锁，无 IO） */
    {
        spinlock_guard g(vcache.cache_lock);

        DentryEntryKey key;
        key.parent_inum = parent_inum;
        key.hash_cache  = h;
        key.name        = name;

        auto it = vcache.dentry_set->find(key, DentryEntryHash(), DentryEntryEqual());
        if (it != vcache.dentry_set->end()) {
            DentryEntry *de = &*it;
            if (de->lru_hook.is_linked())
                vcache.dentry_lru.erase(vcache.dentry_lru.iterator_to(*de));
            vcache.dentry_set->erase(*de);
            if (de->long_name) free(de->long_name);
            delete de;
            vcache.nr_dentries--;
        }
    }

    /* Phase 2: B+tree 写墓碑（IO，caller 应持 per-inode mutex） */
    return dentry_remove(parent_inum, name);
}

/* ═══════════════════════════════════════════════════════════════
 * dentry_cache_warm — 批量预热 dentry（用于 readdir 后填充）
 *
 * 纯自旋锁保护（无 IO）。
 * ═══════════════════════════════════════════════════════════════ */
void RavenFS::dentry_cache_warm(uint64_t parent_inum, const char *name,
                                 uint64_t target_inum) {
    uint32_t h = name_hash(name);

    /* Phase 1: 查重（自旋锁） */
    {
        spinlock_guard g(vcache.cache_lock);
        DentryEntryKey key;
        key.parent_inum = parent_inum;
        key.hash_cache  = h;
        key.name        = name;

        auto it = vcache.dentry_set->find(key, DentryEntryHash(), DentryEntryEqual());
        if (it != vcache.dentry_set->end())
            return;
    }

    /* Phase 2: 构造条目（不持锁，内存分配可能阻塞） */
    DentryEntry *de = new DentryEntry();
    de->parent_inum = parent_inum;
    de->hash_cache  = h;
    de->target_inum = target_inum;

    size_t nlen = strlen(name);
    if (nlen < 48) {
        memcpy(de->name, name, nlen + 1);
        de->long_name = nullptr;
    } else {
        de->name[0] = '\0';
        de->long_name = (char *)malloc(nlen + 1);
        if (de->long_name)
            memcpy(de->long_name, name, nlen + 1);
    }

    /* Phase 3: 插入缓存（自旋锁） */
    {
        spinlock_guard g(vcache.cache_lock);
        DentryEntryKey key;
        key.parent_inum = parent_inum;
        key.hash_cache  = h;
        key.name        = name;

        auto it = vcache.dentry_set->find(key, DentryEntryHash(), DentryEntryEqual());
        if (it != vcache.dentry_set->end()) {
            if (de->long_name) free(de->long_name);
            delete de;
            return;
        }

        auto commit_data = DentryHashSet::insert_commit_data();
        auto res = vcache.dentry_set->insert_check(key, DentryEntryHash(),
                                                     DentryEntryEqual(), commit_data);
        if (res.second) {
            vcache.dentry_set->insert_commit(*de, commit_data);
            vcache.dentry_lru.push_front(*de);
            vcache.nr_dentries++;
        } else {
            if (de->long_name) free(de->long_name);
            delete de;
        }
    }
}

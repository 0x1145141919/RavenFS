# RavenFS VFS Cache — LogicalInode + Dentry Cache 设计契约

> 2026-07-26
> 前置阅读：`Docs/vfs_research_report.md`（设计推演）、`Docs/inode_cache_spec.md`（物理缓存层）
> 本规范定义 God Class 内 VFS 级运行时缓存的完整设计。

---

## 1. 总揽：三层缓存体系

```
VFS 逻辑层（本文定义）:
  ┌─────────────────────┐  ┌───────────────────────┐
  │  inum_hashtable      │  │  dentry_hashtable      │
  │  LogicalInode 查找   │  │  name → inum 查找       │
  │  + inode_lru         │  │  + dentry_lru           │
  │  + dirty_list        │  │                         │
  └─────────┬───────────┘  └───────────┬───────────┘
            │                           │
            │ inum 桥接                  │ inum
            ▼                           ▼
  ┌─────────────────────────────────────────────┐
  │        InodeSession (RAII)                  │
  │        cache_get / cache_put                │
  └──────────────────┬──────────────────────────┘
                     │
物理缓存层（inode_cache_spec.md 定义）:
  ┌─────────────────────────────────────────────┐
  │  Inode_cache_controller                     │
  │  组相联 cache lines × controller_set        │
  │  LRU 淘汰, V/D/T 状态机                     │
  │  raven_Inode_t data[16] per cache line       │
  └─────────────────────────────────────────────┘
```

**核心原则：LogicalInode 不持 raw inode 数据**，通过 `InodeSession` 从物理缓存层按需获取 `raven_Inode_t*`。写回时机全权交给物理缓存 LRU。

---

## 2. 数据结构

### 2.1 LogicalInode — 运行时元数据

```cpp
struct LogicalInode {
    /* ── 侵入式容器 Hook ── */
    bi::unordered_set_member_hook<>    inum_hash_hook;   // → inum_hashtable
    bi::list_member_hook<>             lru_hook;          // → inode_lru
    bi::list_member_hook<>             dirty_hook;        // → dirty_list

    /* ── 运行时元数据（全部纯内存，不占磁盘空间） ── */
    uint64_t             inum;                            // inode 编号
    std::atomic<int>     refcnt;                          // == 0 时可 evict
    bool                 dirty      : 1;                  // 数据被修改
    bool                 loaded     : 1;                  // 已通过 InodeSession 获取过物理数据
    std::mutex           mtx;                             // per-inode 细粒度锁

    /* ── 目录专用 ── */
    DentryEntryCache    *dentry_cache = nullptr;           // 目录的 dentry B+tree 缓存

    /* ── 绝不存 raven_Inode_t ──
     * 物理数据通过 InodeSession→物理缓存获取
     */
};

/* LogicalInode 内存开销:
 *   hook[3] = 3 × 2 ptr ≈ 48B
 *   inum + refcnt + flags ≈ 24B
 *   mtx ≈ 40B (pthread_mutex)
 *   dentry_cache ptr ≈ 8B
 *   ────────────────
 *   约 120B per LogicalInode
 */
```

### 2.2 DentryEntry — 名字→inum 映射

```cpp
struct DentryEntry {
    /* ── 侵入式容器 Hook ── */
    bi::unordered_set_member_hook<>    hash_hook;    // → dentry_hashtable
    bi::list_member_hook<>             lru_hook;     // → dentry_lru

    /* ── 键 ── */
    uint64_t    parent_inum;                          // 父目录 inum
    uint32_t    hash_cache;                           // name hash，快速筛

    /* ── 值 ── */
    uint64_t    target_inum;                          // 指向的 inode（不是指针！）

    /* ── 名字存储 ── */
    char        name[48];                             // 短名内联（≤47 字符 + NUL）
    char       *long_name = nullptr;                  // 长名单独分配
};

/* DentryEntry 内存开销:
 *   2×hook + 2×uint64 + uint32 + name[48] = 16 + 16 + 4 + 48 ≈ 84B
 *   长名额外 malloc
 */
```

### 2.3 容器体系

```cpp
/* ── 全局容器：两个哈希表 + LRU 列表 ── */

struct RavenFS {
    // 文件系统全局参数
    uint64_t     inum_count;            // 总 inode 数（mkfs 静态分配）
    uint64_t     dentry_entries_max;    // dentry 上限（内存决定）

    // ─── inum_hashtable ───
    uint8_t      inum_hash_shift;       // bucket 数 = 1 << x, ~(inum_count / 8)
    bi::unordered_set<LogicalInode,
        bi::member_hook<LogicalInode,
            bi::unordered_set_member_hook<>,
            &LogicalInode::inum_hash_hook>
        >          *inum_hashtable;     // key: inum, 大数组预分配

    // ─── inode LRU 列表 ───
    bi::list<LogicalInode,
        bi::member_hook<LogicalInode,
            bi::list_member_hook<>,
            &LogicalInode::lru_hook>,
        bi::constant_time_size<false>
        >           inode_lru;          // refcnt==0 的 LogicalInode

    // ─── dirty 列表 ───
    bi::list<LogicalInode,
        bi::member_hook<LogicalInode,
            bi::list_member_hook<>,
            &LogicalInode::dirty_hook>,
        bi::constant_time_size<false>
        >           dirty_list;         // dirty==true 的 LogicalInode

    // ─── dentry_hashtable ───
    uint8_t      dentry_hash_shift;     // bucket 数 = 1 << y
    bi::unordered_set<DentryEntry,
        bi::member_hook<DentryEntry,
            bi::unordered_set_member_hook<>,
            &DentryEntry::hash_hook>
        >          *dentry_hashtable;   // key: hash(parent_inum, name)

    // ─── dentry LRU 列表 ───
    bi::list<DentryEntry,
        bi::member_hook<DentryEntry,
            bi::list_member_hook<>,
            &DentryEntry::lru_hook>,
        bi::constant_time_size<false>
        >           dentry_lru;

    // ─── 物理缓存（引用） ───
    Inode_cache_controller  *phys_cache;

    // ─── 统计 ───
    std::atomic<long>    nr_logical_inodes;
    std::atomic<long>    nr_logical_unused;
    std::atomic<long>    nr_dentries;
    std::atomic<long>    nr_dentries_unused;
};
```

---

## 3. 初始分配策略（仿 Linux 内核）

```cpp
// Phase 1 初始化：一次性分配大数组，FS 生命周期内不缩容
void RavenFS::vfs_cache_init() {
    /* inum_hashtable: bucket 数组
     *   Linux: alloc_large_system_hash("Inode-cache", sizeof(hlist_head), ...)
     *   目标: bucket_count ~ inum_count / 8（负载因子 ~0.125 保性能）
     *   bucket_count 向上取整到 2 的幂
     */
    uint64_t inum_buckets = 1ULL << inum_hash_shift;
    inum_hashtable = new bi::unordered_set<...>[inum_buckets];

    /* dentry_hashtable: bucket 数组
     *   Linux: alloc_large_system_hash("Dentry cache", sizeof(hlist_bl_head), ...)
     *   dentry 数量由内存压力决定, 初始 bucket_count ≈ 预期峰值 / 8
     */
    uint64_t dentry_buckets = 1ULL << dentry_hash_shift;
    dentry_hashtable = new bi::unordered_set<...>[dentry_buckets];

    /* 两个哈希表的 bucket 数组在初始化时分配并置零
     * 后续插入只链入节点，不额外分配容器元数据
     * （Boost.Intrusive 的 unordered_set 内部用单链表/hook 链）
     */
}
```

### 3.1 哈希函数

```cpp
/* ── inum_hashtable 的哈希 ── */
// 直接用 inum 做哈希，和 Linux 的 hash(sb, ino) 同思路
// 不需要 sb 做盐值，因为 RavenFS 是单实例
struct InumHash {
    uint64_t operator()(const LogicalInode &inode) const {
        return hash_mix(inode.inum);
    }
};

// 使用 Thomas Wang 的 hash mix（简单、快、分布好）
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

/* ── dentry_hashtable 的哈希 ── */
// 用 parent_inum 做盐值混合 name hash
// 和 Linux 用 parent 指针做盐值同原理
struct DentryEntryHash {
    uint64_t operator()(const DentryEntry &de) const {
        return hash_mix(
            de.hash_cache ^
            hash_mix(de.parent_inum)
        );
    }
};

// 比较（同 Linux d_lookup 四级过滤的 hash + parent + name 检查）
struct DentryEntryEq {
    bool operator()(const DentryEntry &a, const DentryEntry &b) const {
        return a.parent_inum == b.parent_inum
            && a.hash_cache == b.hash_cache
            && strcmp(
                a.long_name ? a.long_name : a.name,
                b.long_name ? b.long_name : b.name
            ) == 0;
    }
};
```

---

## 4. 查找路径

### 4.1 dentry_hashtable 查找（路径解析主路径）

```cpp
// 路径解析 walk 到某级目录时，查子文件 dentry：
uint64_t dentry_lookup(uint64_t parent_inum, const char *name,
                        uint32_t name_hash)
{
    DentryEntry probe;
    probe.parent_inum = parent_inum;
    probe.hash_cache  = name_hash;
    strncpy(probe.name, name, 47);
    probe.name[47] = '\0';

    auto &bucket = dentry_hashtable[hash_mix(name_hash ^ hash_mix(parent_inum))
                                    & (dentry_buckets - 1)];

    // bucket 内的侵入式查找
    auto it = bucket.find(probe, DentryEntryHash(), DentryEntryEq());
    if (it == bucket.end())
        return ~0ULL;  // cache miss

    // 更新 LRU
    dentry_lru.erase(*it);
    dentry_lru.push_front(*it);

    return it->target_inum;
}
```

**cache miss → FS 回退：**

```
dentry_hashtable miss
  → 走 dentry B+tree 读盘
  → alloc DentryEntry + 填充字段
  → dentry_hashtable[hash].insert(*new_entry)
  → dentry_lru.push_front(*new_entry)  // 置 MRU 端
  → return inum
```

### 4.2 inum_hashtable 查找（dentry hit 后继步骤）

```cpp
LogicalInode *logical_inode_get(uint64_t inum) {
    auto &bucket = inum_hashtable[hash_mix(inum) & (inum_buckets - 1)];

    // 查找
    auto it = bucket.find(LogicalInode{inum}, InumHash(),
                          [](auto &a, auto &b){ return a.inum == b.inum; });
    if (it != bucket.end()) {
        it->refcnt++;                    // pin
        inode_lru.erase(*it);            // 从 LRU 摘除（如果在上）
        return &*it;
    }

    // 未命中 → 分配新 LogicalInode
    LogicalInode *li = new LogicalInode();
    li->inum   = inum;
    li->refcnt = 1;                     // 初始 1（调用者持有）
    li->dirty  = false;
    li->loaded = false;

    bucket.insert(*li);
    nr_logical_inodes++;

    // loaded==false, 物理数据延迟获取
    return li;
}
```

### 4.3 物理数据懒加载

```cpp
// 路径解析拿到 inum 后，需要读写 inode 实际数据时：
raven_Inode_t *get_physical_inode(uint64_t inum) {
    // 走 InodeSession（RAII）从物理缓存拿数据
    InodeSession sess(phys_cache, inum);
    return sess.inode;                    // raven_Inode_t*
}

// 或者合并到 LogicalInode 的访问：
raven_Inode_t *LogicalInode::get_data(Inode_cache_controller *cache) {
    if (!loaded) {
        session = new InodeSession(cache, inum);
        loaded = true;
    }
    return session->inode;
}
```

---

## 5. 引用规则

```cpp
/* ─── 引用计数传导 ───

 * path_resolve step():
 *   上一级 LogicalInode 持有 → refcnt--
 *   本级的 LogicalInode → refcnt++ (pin, 瞬态)
 *   进入下一级前 → 释放上一级

 * open():
 *   target LogicalInode → refcnt++ (持久 pin)
 *   → 返回 handle（handle 析构时 refcnt--）

 * close():
 *   LogicalInode → refcnt--
 *   → refcnt == 0 → dirty? 挂 dirty_list : 挂 inode_lru

 * 根 inode:
 *   神之 ref++（永不归零）
 *   显式 mark_dirty() 触发 flush
 */
```

### 5.1 关键区别：与 Linux 的 dentry→inode 反 pin

```
Linux:
  dentry→d_inode = inode*        ← 直接指针
  iput(inode) 发生在 dentry 释放时
  → dentry pin 住了 inode 的 i_count

RavenFS:
  DentryEntry→target_inum = inum  ← 整数桥接
  dentry hit 后: inum_hashtable.find(inum) → ref++
  → dentry 不 pin LogicalInode 的引用
  → ref 只在路径解析/操作中自增，不被 dentry 绑定
```

---

## 6. 写回策略

### 6.1 写路径

```cpp
void modify_inode_field(uint64_t inum) {
    // 1. 获取 LogicalInode（运行时元数据）
    LogicalInode *li = logical_inode_get(inum);
    li->mtx.lock();

    // 2. 获取物理数据（InodeSession RAII）
    InodeSession sess(phys_cache, inum);
    // sess.inode → raven_Inode_t*

    // 3. 修改
    sess.inode->size = new_size;
    sess.dirty();          // 标记物理 cache line DIRTY
    li->dirty = true;      // 标记 LogicalInode 脏
    if (!li->lru_hook.is_linked()) {
        dirty_list.push_back(*li);  // 挂脏列表
    }

    li->mtx.unlock();
    logical_inode_put(li);  // refcnt--
}
```

### 6.2 写盘时机（谁写盘？——物理缓存层）

```
modify → InodeSession::dirty()
  → Inode_cache_controller::mark_dirty()
    → cache line controller.flags |= INODE_CACHE_D
    ← 完成。不刷盘。

什么时候真的 write_block()？
  ① LRU 淘汰选中这个 cache line
     → 检查 D 位 → 刷盘 → 覆盖新数据
  ② umount / flush_all()
     → 遍历所有 cache line controller
     → D 位 = 1 的刷盘
  ③ 析构 ~Inode_cache_controller()
     → 同上

LogicalInode 的 dirty_list 只用来：
  - 标记"这个 inode 被改过"
  - umount 时确保至少遍历一次
  - 不直接触发 write_block()
```

### 6.3 两条独立 LRU

```
LogicalInode LRU:
  refcnt == 0 & !dirty → 挂 inode_lru
  淘汰 = delete LogicalInode
  不碰磁盘

物理 Cache Line LRU:
  accessor == 0 & cache 满 → select victim
  淘汰 = 检查 D → 刷盘 → 读新数据
  需要访问磁盘

两者互不依赖：
  LogicalInode 释放了，物理数据还在 cache line 里
  物理 cache line 被逐出了，LogicalInode 还在 (loaded=false)
```

---

## 7. Dentry 淘汰

```cpp
// dentry_lru 管理
void dentry_put(DentryEntry *de) {
    // ref 逻辑简化：一次路径解析 = 一次引用
    // 路径完整解析完毕后进行 LRU 管理

    // 场景 1: 被 open/readdir pin
    if (de->is_pinned) {
        de->is_pinned = false;
        return;
    }

    // 场景 2: 正常放到 LRU MRU 端
    dentry_lru.push_front(*de);
}

// 用 LRU 尾端作为 evict 候选
void shrink_dentry_cache(uint64_t target) {
    while (nr_dentries > target) {
        DentryEntry *victim = &dentry_lru.back();
        dentry_lru.pop_back();

        // 从 dentry_hashtable 摘除
        uint64_t bucket_idx = hash_mix(
            victim->hash_cache ^ hash_mix(victim->parent_inum)
        ) & (dentry_buckets - 1);
        dentry_hashtable[bucket_idx].erase(*victim);

        if (victim->long_name)
            free(victim->long_name);
        delete victim;
        nr_dentries--;
    }
}
```

---

## 8. VFS 视图和 I/O 全链路

```
用户请求: open("/home/user/foo.txt", O_RDONLY)

RavenFS::open(path):
  │
  ├─ 1. path_resolve()
  │     ├─ 当前路径上下文 (cwd = inum)
  │     ├─ for each component:
  │     │   ├─ dentry_hashtable.lookup(parent_inum, name)  ← 查 dentry cache
  │     │   │  hit → target_inum
  │     │   │  miss → FS dentry B+tree → new DentryEntry
  │     │   │
  │     │   ├─ inum_hashtable.find(target_inum)            ← 查 inode cache
  │     │   │  hit → LogicalInode.refcnt++
  │     │   │  miss → new LogicalInode
  │     │   │
  │     │   └─ parent->refcnt--  (离开上一级)
  │     │
  │     └─ 拿到 target inum + LogicalInode
  │
  ├─ 2. handle_table.alloc(target_inum)                    ← 分配文件句柄
  │     └─ target LogicalInode.refcnt++ (持久 pin)
  │
  ├─ 3. 路径上的中间 LogicalInode refcnt-- (瞬态释放)
  │
  └─ 返回 handle_id

用户读写: handle_table[handle_id]
  │
  ├─ InodeSession sess(phys_cache, inum)                   ← 获取 raven_Inode_t*
  ├─ 通过 raven_Inode_t->extents 定位数据块
  ├─ 读写
  └─ 改数据后 sess.dirty()

用户 close(handle_id):
  ├─ handle_table[handle_id].LogicalInode.refcnt--
  │  refcnt==0 → 挂 dirty_list (如果脏) 或 inode_lru
  └─ handle 释放
```

---

## 9. Phase 2 弑神边界

依据当前设计，Phase 2 与 Phase 1 之间的拆分线：

```
Phase 1 (上帝类):
┌────────────────────────────────────────────────────┐
│                   RavenFS                          │
│                                                    │
│  VFS 逻辑缓存:                                      │
│    ● inum_hashtable + LogicalInode                 │
│    ● dentry_hashtable + DentryEntry                │
│    ● inode_lru / dentry_lru / dirty_list           │
│    ● per-inode mtx / refcnt                        │
│                                                    │
│  VFS 操作:                                         │
│    ● path_resolve / dentry_lookup                  │
│    ● handle_table (open/close)                     │
│                                                    │
│  FS 算法:                                          │
│    ● dentry B+tree                                 │
│    ● inode alloc/free (bitmap)                     │
│    ● extent 管理                                   │
│                                                    │
│  物理缓存:                                         │
│    ● Inode_cache_controller (组相联)                │
│    ● BitmapCacheController                         │
│                                                    │
│  I/O:                                              │
│    ● BlockDevice (pread/pwrite)                    │
└────────────────────────────────────────────────────┘

Phase 2 (弑神后):
┌──────────────────┐  ┌───────────────────────────┐
│ RavenVFS          │  │ RavenFSImpl              │
│                   │  │                           │
│  path_resolve     │  │  dentry B+tree            │
│  dentry_hashtable │  │  inode alloc/free         │
│  inum_hashtable   │  │  extent lookup            │
│  handle_table     │  │  bitmap                   │
│  LogicalInode     │  └───────────────────────────┘
│  inode_lru        │              ↕
│  dentry_lru       │  ┌───────────────────────────┐
│  dirty_list       │  │ BlockCache (组相联)        │
│  per-inode lock   │  │ + LRU + V/D/T             │
└──────────────────┘  └───────────────────────────┘
                             ↕
                       ┌────────────┐
                       │ IoBackend  │
                       │ (io_uring) │
                       └────────────┘
```

---

## 10. 与 InodeBitmap 的 locality 耦合

```
InodeBitmapCache::scan_hint
  │  确保相邻 inum 连续分配
  │
  ▼
inum 0xA0, 0xA1, 0xA2, ... 0xAF
  │
  ▼
Inode_cache_controller::decode_inum:
  set   = (inum >> 4) & mask    → 同一个 set
  row   = inum & 0xF             → 同一 cache line 内偏移
  tag   = inum >> (4 + N)
  │
  ▼
分配 16 个 inode → 只有第一次 cache miss → 1 次 read_block
剩下的 15 次 cache_get → 全命中 → 零 I/O
```

---

## 11. 配置与编译期常量

```cpp
// 在 ravenfs.hpp 或独立 config 中
struct VFSCacheConfig {
    uint64_t   estimated_inodes;       // mkfs 传入，决定 inum_hashtable bucket 数
    uint64_t   estimated_dentries;     // 预期最大 dentry 数
    uint8_t    inum_hash_shift;       // 推导: 2^shift = buckets >= estimated_inodes / 8
    uint8_t    dentry_hash_shift;      // 推导: 2^shift = buckets >= estimated_dentries / 8
    bool       enable_dentry_shrinker; // 是否启用内存压力淘汰
};

// 默认值
constexpr uint8_t DEFAULT_INUM_HASH_SHIFT   = 18;  // 256K buckets
constexpr uint8_t DEFAULT_DENTRY_HASH_SHIFT = 20;  // 1M buckets
```

---

## 12. 实现顺序（Phase 1 内）

| # | 组件 | 前置依赖 | 说明 |
|---|------|---------|------|
| 1 | `LogicalInode` struct 定义 | 无 | 纯数据定义 |
| 2 | `DentryEntry` struct 定义 | 无 | 纯数据定义 |
| 3 | `inum_hashtable` + 初始化 | 1 | 大数组分配 + hash mix |
| 4 | `dentry_hashtable` + 初始化 | 2 | 大数组分配 + hash mix |
| 5 | `logical_inode_get`/`put` | 1, 3 | 引用计数 + LRU 管理 |
| 6 | `dentry_lookup`/`dentry_insert` | 2, 4 | 哈希查找 + 插入 |
| 7 | `path_resolve` 改为走新缓存 | 5, 6 | 替换现有 PathCursor |
| 8 | 从 InodeSession 获取物理数据 | 5, `inode_cache_spec.md` | loaded flag 控制 |
| 9 | `dirty_list` + umount flush | 5, 8 | 担保脏数据写回 |
| 10 | dentry shrinker（LRU evict） | 6 | debug 阶段可选 |
| 11 | 多线程并发测试 | 全部 | 压力测试 |
| 12 | Phase 1 验收 | 全部 | plan.md 验收清单 |

---

> **本文与 Docs 其他文档的关系：**
>
> - `inode_cache_spec.md` — 物理缓存层（V/D/T 状态机、组相联、`InodeSession`）
> - `vfs_cache_spec.md`  — **本文** VFS 逻辑缓存层（LogicalInode、dentry、侵入式容器）
> - `vfs_research_report.md` — 设计推演笔记，包含 Linux 内核对比分析和决策链
> - `bitmap_cache_spec.md` — InodeBitmap 的时空局部性设计（为本文的 locality 链路供前端）
> - `plan.md` — 全局路线图

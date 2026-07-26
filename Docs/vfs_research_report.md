# RavenFS VFS 层研究报告

> 2026-07-25
> 主题：Linux VFS 缓存体系分析 + RavenFS 上帝类 VFS 架构设计方向

---

## 一、缘起

RavenFS 开发进入 Phase 1（上帝类），计划.md 明确此阶段不只是文件系统实现，更是 **VFS 并发设计的研究战场**。盘间布局已在 `block_model.md` / `disk_layout_v1.md` 中定稿，Phase 1 的核心是搞清楚：

- VFS 和 FS 的边界在哪
- 缓存、锁定、路径解析的并发模型怎么设计
- Phase 2 弑神时按哪条线切开

---

## 二、Linux VFS 缓存体系分析

### 2.1 全局哈希表，全部侵入式

Linux VFS 的两个核心缓存都是**全局、侵入式、哈希表**，不存在"中间表项"设计。

#### inode_hashtable — inode 查找

```c
static struct hlist_head *inode_hashtable;    // bucket 数组

struct inode {
    struct hlist_node i_hash;     // ← 嵌在 inode 内部，链入哈希表
    struct list_head  i_sb_list;  // → sb->s_inodes
    struct list_head  i_lru;      // → sb->s_inode_lru（淘汰队列）
    struct list_head  i_io_list;  // → writeback 脏队列
    struct hlist_head i_dentry;   // → dentry 别名（硬链接）
};
```

key = `hash(sb, ino)`，值为 `struct inode *`。查找时：

```c
struct hlist_head *head = inode_hashtable + hash(sb, ino);
hlist_for_each_entry(inode, head, i_hash) {
    if (inode->i_ino == ino && inode->i_sb == sb)
        return inode;    // 已有 → ref++
}
if (!found) {
    inode = alloc_inode(sb);   // slab 分配
    inode->i_ino = ino;
    hlist_add_head_rcu(&inode->i_hash, head);  // 侵入式链入
}
```

#### dentry_hashtable — 路径解析

```c
static struct hlist_bl_head *dentry_hashtable; // bucket 数组

struct dentry {
    struct hlist_bl_node d_hash;   // → dentry_hashtable
    struct dentry       *d_parent; // 父目录
    struct hlist_node    d_sib;    // → 父的 d_children
    struct hlist_head    d_children; // 自己的孩子
    struct inode        *d_inode;  // 名字指向的 inode
    struct list_head     d_lru;    // LRU 淘汰
    struct qstr          d_name;   // 名字（hash + len + 字符串指针）
};
```

key = `hash(parent_dentry_ptr, name)`。盐值用父 dentry 指针而不是 inode 指针，因为一个目录 inode 可能被多个 dentry 引用（bind mount），不同路径需要独立 hash。

查找四级过滤：

1. `dentry->d_name.hash != hash` — 快速筛
2. `dentry->d_parent != parent` — 同名不同父筛掉
3. `d_unhashed(dentry)` — 已删除
4. `!d_same_name(...)` — 字符串最终确认

#### 名字字符串存储

```
短名 (≤39 字节 x86-64): d_shortname 内联，零额外分配
长名 (>39 字节):        struct external_name 单独 kmalloc
```

### 2.2 一个对象同时链入多个容器

`struct inode` 同时链入六个容器：

| 容器 | node 字段 | 用途 |
|------|----------|------|
| `inode_hashtable` | `i_hash`（hlist_node） | **查找**：inum → inode |
| `sb->s_inodes` | `i_sb_list`（list_head） | **遍历**：一个 sb 的所有 inode |
| `sb->s_inode_lru` | `i_lru`（list_head） | **淘汰**：ref==0 后等 evict |
| `bdi->wb.b_dirty` | `i_io_list`（list_head） | **写回**：脏 inode |
| `in_lookup_hashtable` | `i_in_lookup_hash`（hlist_bl_node） | **并发**：正在 lookup 中的 |
| 自身 | `i_dentry`（hlist_head） | **别名**：硬链接列表（自己是 head） |

全都是侵入式。没有 `struct inode_cache_entry` 这种中间人。**同一个 slab 对象，六个 node 字段，六个不同容器。**

### 2.3 引用规则

```
i_count:
  > 0: 有人用，不能 evict
  == 0: iput_final() 挂入 LRU，等内存压力淘汰

根 inode 无免死金牌，靠 dentry 链传导 refcount：
  sb->s_root（dentry）→ d_inode
  → path walk 从 / 开始每步 dget/dput 传导
  → 没有全局"根 inode 不死"标志位

实际上根是 per-process 的：
  current->fs->root (struct path)
  fork 时共享 fs_struct，unshare 后可独立改
```

### 2.4 淘汰策略

```
iput() → i_count == 0
  → drop_inode() ? (强制删) : (挂入 LRU)
  → LRU 不主动赶人，等内存压力 shrinker

shrinker (prune_icache_sb):
  → walk LRU:
    i_count > 0? → 摘掉（又被用了）
    I_REFERENCED? → 旋转一圈再给次机会
    page cache 还在? → 尝试清理 page
    全清了? → I_FREEING → evict
```

### 2.5 Dentry cache miss 后的文件系统回调

```
全局哈希表 miss
  → __lookup_slow()
    → dir->i_op->lookup(dir, dentry, flags)
      → ext4_lookup() 读盘
        → iget_locked(ino) → 统一 inode 指针
    → d_add(dentry, inode) 塞回全局哈希表
```

FS 给 VFS 的是 inum，`iget_locked` 保证所有 dentry 的 `d_inode` 指向同一个 `struct inode` 对象。

---

## 三、RavenFS 上帝类 VFS 架构设计

### 3.1 核心结构

```cpp
// ─── 逻辑 Inode ───
struct LogicalInode {
    /* 侵入式容器 Hook */
    bi::list_member_hook<>             lru_hook;         // → sb->inode_lru
    bi::list_member_hook<>             dirty_hook;       // → sb->dirty_list
    bi::unordered_set_member_hook<>    inum_hash_hook;   // → inum_hashtable

    /* 运行时元数据 */
    uint64_t             inum;
    std::atomic<int>     refcnt;
    bool                 dirty : 1;
    bool                 loaded : 1;
    std::mutex           mtx;

    /* 物理数据（lazy load） */
    raven_Inode_t        disk_data;

    /* 目录专用 */
    DentryEntryCache    *dentry_cache = nullptr;
};

// ─── Dentry 条目 ───
struct DentryEntry {
    bi::unordered_set_member_hook<>    hash_hook;   // → dentry_hashtable
    bi::list_member_hook<>             lru_hook;    // → dentry_lru

    uint64_t    parent_inum;
    uint64_t    target_inum;
    uint32_t    hash_cache;            // 快速筛
    char        name[48];              // 短名内联
    char       *long_name = nullptr;   // 长名额外分配
};
```

### 3.2 容器体系

| 容器 | 类型 | key/value | 节点来源 |
|------|------|-----------|---------|
| `inum_hashtable` | `bi::unordered_set` | `inum` → `LogicalInode*` | 嵌在 `LogicalInode` 内 |
| `inode_lru` | `bi::list` | N/A | 嵌在 `LogicalInode` 内 |
| `dirty_list` | `bi::list` | N/A | 嵌在 `LogicalInode` 内 |
| `dentry_hashtable` | `bi::unordered_set` | `hash(parent_inum,name)` → `DentryEntry*` | 嵌在 `DentryEntry` 内 |
| `dentry_lru` | `bi::list` | N/A | 嵌在 `DentryEntry` 内 |

五个容器，全部 Boost.Intrusive 侵入式。分配只有两个原生操作：

```
new LogicalInode     — 直接是 inum_hashtable / lru / dirty 的节点
new DentryEntry      — 直接是 dentry_hashtable / dentry_lru 的节点
```

容器本身只分配一次 bucket 数组（`new hlist_head[n]`），无额外节点元数据。

### 3.3 引用规则

```
路径解析 step():
  LogicalInode->refcnt++   // 瞬态 pin
  // 用完/step()下一级前
  pref_logical->refcnt--   // 释放上级

open():
  LogicalInode->refcnt++   // 持久 pin

close():
  LogicalInode->refcnt--   // → 0 时可 evict

根 inode:
  神之 ref++（永不归零）
  显式 mark_dirty() + umount 时 flush
```

### 3.4 关键设计决策

#### A. inum→LogicalInode 不走数组下标（Option A）

- 全局 `bi::unordered_set`，key = inum
- 不允许硬编码数组大小
- 与 Linux 的 `sb->s_inodes` + `inode_hashtable` 双容器同思路

#### B. Dentry cache 为全局哈希表（仿 Linux）

- 不是 per-directory 附属，是全局独立结构
- key = `hash(parent_inum, name)`，跟 Linux 用父 dentry 指针做盐值同原理
- 生命周期：DentryEntry 独立于 LogicalInode，LRU 各自独立淘汰
- Phase 2 弑神时直接拆成独立层

#### C. DentryEntry 指向 inum（不直接指向 LogicalInode*）

- dentry 不持有 LogicalInode 的引用
- hit 后 `inum_hashtable.find(target_inum)` → `LogicalInode::refcnt++`
- 解决父目录 dentry 链 pin 死子 inode 的问题

#### D. 每 inode 一把 `std::mutex`（细粒度锁）

- 不搞全局大锁
- `step()` 时锁目录 inode → 查 dentry → 锁 child inode → 解锁目录 → 继续

### 3.5 LogicalInode 生命周期

```
new LogicalInode
  → refcnt = 0
  → 链入 inum_hashtable（未被 pin，可 evict）
  → ref++（step() 或 open()）
  → 使用中（dirty 时会挂 dirty_list）
  → ref--（用完）
  → ref == 0 && dirty → flush → 可 evict
  → ref == 0 && !dirty → LRU 等淘汰
  → LRU evict → 从 inum_hashtable 摘除 → delete

根 inode:
  神之 ref 保持 > 0
  dirty 时走显式 flush（定时/umount）
```

### 3.6 写回策略（Phase 1 简化版）

```
ref == 0 && dirty → 写入 dirty_list
umount 时：
  遍历 dirty_list → flush → 清空
  遍历 inode_lru → ref==0 → evict

Phase 1 不做后台写回线程，不做 shrinker。
```

---

## 四、与 block_model.md / disk_layout_v1.md 之间的关系

### 物理层 vs 逻辑层

```
物理层（已有，已定稿）:
  ┌─────────────────────────┐
  │ 组相联 Inode Cache      │ ← 存 raw raven_Inode_t
  │ (Inode_cache_controller)│    按 FS_block 粒度
  │                         │    物理 I/O 缓存
  └─────────────────────────┘

逻辑层（本文定义的新设计）:
  ┌─────────────────────────┐
  │ inum_hashtable          │ ← LogicalInode 的查找索引
  │ dentry_hashtable        │ ← name→inum 的路径解析索引
  │ inode_lru               │ ← 淘汰队列
  │ dirty_list              │ ← 写回队列
  │ 每个 LogicalInode       │ ← refcnt + lock + 运行时元数据
  └─────────────────────────┘
```

物理层是磁盘块缓存，受 `block_model.md` 的 FS_block 坐标系支配。逻辑层是运行时对象池，与盘面布局解耦，属于 VFS 范畴。

### 关系

```
路径解析：
  dentry_hashtable (name→inum)
    miss → dentry B+tree 读盘
         → 经过物理缓存（组相联/block cache）
         → 得到 inum
         → new DentryEntry + 链入 dentry_hashtable

  inum_hashtable (inum→LogicalInode)
    miss → new LogicalInode
         → 物理 cache 加载 raven_Inode_t
         → ref++ 返回

写回：
  LogicalInode.dirty == true → flush
    → 物理 cache mark_dirty
    → 写入组相联 cache line
    → 后续刷盘
```

---

## 五、Phase 2 弑神的边界

根据 plan.md，Phase 2 应沿以下边界拆分：

```
Phase 1 上帝类:
┌────────────────────────────────────────────┐
│           RavenFS (god class)              │
│  VFS (path_resolve, handle_table, locks)   │
│  FS  (B+tree, bitmap, inode allocation)    │
│  LogicalInode + dentry_hashtable           │
│  Inode_cache_controller (物理块缓存)        │
│  io_backend (pread/pwrite)                 │
└────────────────────────────────────────────┘

Phase 2 弑神后的拆分（方向）:
┌──────────────┐  ┌──────────────────────┐
│ RavenVFS     │  │  RavenFSImpl         │
│              │  │                      │
│ path_resolve │  │  B+tree              │
│ handle_table │  │  bitmap              │
│ lock mgmt    │  │  inode alloc/free    │
│              │  │  dentry tree ops     │
│ LogicalInode │  └──────────────────────┘
│ dentry hash  │             ↕
│ inode LRU    │  ┌──────────────────────┐
│ dirty list   │  │  BlockCache          │
└──────────────┘  │  (组相联物理缓存)     │
         ↕        └──────────────────────┘
  ┌────────────┐
  │ IoBackend  │
  │ (io_uring) │
  └────────────┘
```

本文讨论的 dentry_hashtable、inum_hashtable、LRU、dirty_list 属于 **RavenVFS** 层。
组相联 inode cache + B+tree + bitmap 属于 **RavenFSImpl** 和 **BlockCache** 层。
LogicalInode 是两层的交界——VFS 管它的生命周期，FS 管它 `disk_data` 的填充和写回。

---

## 六、待解决 / Phase 1 实验内容

| 项目 | 状态 |
|------|------|
| `dentry_insert` 的 `insert_at` 计算 bug | 未修复（永远 append） |
| LogicalInode 结构定义 + inum_hashtable | 待实现 |
| DentryEntry + dentry_hashtable | 待实现 |
| 路径解析更新为走 dentry_hashtable + inum_hashtable | 待实现 |
| 现有 PathCursor 迁移为新缓存体系 | 待实现 |
| 引用规则实现（step/pin/unpin） | 待实现 |
| 写回机制（脏列表 + umount flush） | 待实现 |
| 多线程并发压力测试 | plan.md Phase 1 验收项 |
| 写回线程 | Phase 1 不做 |
| shriker / 主动淘汰 | Phase 1 不做 |
| 组相联 inode cache (物理层) 与 LogicalInode 的同步关系 | 待设计 |

---

## 七、Phase 1 设计修正 (2026-07-26)

### 7.1 修正：LogicalInode 不再持有原始磁盘数据

原设计（第一节报告）的 `LogicalInode` 包含 `raven_Inode_t disk_data` 字段，这导致两个问题：

1. **数据冗余：** 物理缓存（组相联 `Inode_cache_controller`）已经持有一份 `raven_Inode_t` 的内存副本，LogicalInode 再持有一份就是双份
2. **一致性问题：** 谁写了还得同步，多次拷贝浪费时间
3. **内存浪费：** 每个 LogicalInode（可能几万到上百万）都带一份 inode 数据，物理缓存里也有一份

**新设计：**

```cpp
struct LogicalInode {
    /* 侵入式容器 Hook（完全不碰磁盘数据） */
    bi::list_member_hook<>             lru_hook;           // → sb->inode_lru
    bi::list_member_hook<>             dirty_hook;         // → sb->dirty_list
    bi::unordered_set_member_hook<>    inum_hash_hook;     // → inum_hashtable

    /* 运行时元数据（全部在内存，不占磁盘） */
    uint64_t             inum;
    std::atomic<int>     refcnt;
    bool                 dirty : 1;
    bool                 loaded : 1;          // 物理数据是否已从缓存获取
    std::mutex           mtx;

    /* 目录专用 */
    DentryEntryCache    *dentry_cache = nullptr;

    /* ❌ 删掉：raven_Inode_t disk_data — 物理数据交给 InodeSession */
};
```

### 7.2 数据获取流程

```
路径解析命中 dentry_hashtable
  → 拿到 target_inum
  → inum_hashtable.find(target_inum) → LogicalInode* 或创建
  → 需要读盘上数据时：
      → InodeSession sess(fs, inum)        ← RAII 构造
          → Inode_cache_controller::cache_get(inum)
              → 组相联缓存命中 → 直接返回 raven_Inode_t*
              → 缓存 miss → 从磁盘加载进 cache line → 返回指针
      → 直接用 raven_Inode_t* 读写
      → 修改后 sess.dirty()
          → Inode_cache_controller::mark_dirty(inum)
              → 只设 cache line controller 的 DIRTY 位
              → 不刷盘
  → InodeSession 析构
      → cache_put(inum) → 释放访问计数
      → 不主动刷盘！
```

### 7.3 写回策略

```
mark_dirty() 只做一件事：
  对应 cache line controller 的 flags |= INODE_CACHE_D
之前以为只需要手动调用 mark_dirty，不需要手动 flush

什么时候真正写盘：
  ① LRU 淘汰选中这个 cache line 时
      → evict 前检查 DIRTY 位
      → 脏就 write_block() 刷盘
  ② umount / flush_all()
      → 遍历所有 cache line controller
      → DIRTY 的全部刷盘
  ③ 析构
      → 同上
```

**这么做的好处：**

```cpp
// 场景：一个物理 cache line 包含 16 个 inode (4096B / 256B-per-inode)
// 修改其中任意一个 inode
InodeSession sess1(fs, inum_5);
sess1.inode->nlink = 2;  // 改 inum_5
sess1.dirty();           // 整条 cache line 标记脏

// 紧接着改同一 cache line 的另一个 inode
InodeSession sess2(fs, inum_7);
sess2.inode->size = 1024;  // 改 inum_7
sess2.dirty();              // 同一 cache line 已脏，无事可做

// 直到 LRU 淘汰这个 cache line 时，一次 write_block 刷 16 个 inode
// 磁盘 I/O 从 2 次合并为 1 次
```

### 7.4 LogicalInode 生命周期（修正后）

```
new LogicalInode
  → refcnt = 0
  → 链入 inum_hashtable（未被 pin，可 evict）
  → loaded = false（物理数据未获取）

step() 或 open()
  → LogicalInode.refcnt++
  → 需要修改数据时：
      InodeSession sess(fs, inum);     // 触发 cache_get
      // raven_Inode_t* 直接可用
      sess.inode->xyz = val;
      sess.dirty();                    // 标脏 cache line
  → sess 析构 → cache_put（物理缓存访问计数归零，但不一定 evict）

close() / 路径离开
  → LogicalInode.refcnt--

ref == 0 && dirty → 挂 dirty_list（标记，不刷盘）
ref == 0 && !dirty → LRU 等淘汰

LRU evict LogicalInode
  → 从 inum_hashtable 摘除
  → delete LogicalInode
  → 不刷盘！物理数据还在组相联缓存里
```

**关键：LogicalInode 的 LRU 和物理 cache line 的 LRU 是两条独立淘汰线。**

```
LogicalInode LRU          → 运行时元数据淘汰（refcnt、锁、目录缓存）
物理 cache line LRU       → 磁盘数据淘汰（raven_Inode_t 生命周期）
```

两个互不依赖。LogicalInode 释放了，物理数据还在组相联缓存里，下次另一个路径解析进来直接复用。反之亦然——物理 cache line 被逐出了，LogicalInode 还在 (`loaded = false`)，下次访问时重新 `cache_get` 就重新加载了。

### 7.5 初始分配策略

```cpp
// 借鉴 Linux 内核策略：
// 初始化时分配一大块 hash table bucket 数组（连续内存）

// dentry_hashtable
//   Linux: alloc_large_system_hash("Dentry cache", sizeof(hlist_bl_head), ..., HASH_ZERO)
//   Raven: new hlist_head[1 << dentry_hash_shift]  // bucket 数组，一次性分配

// inum_hashtable
//   Linux: alloc_large_system_hash("Inode-cache", sizeof(hlist_head), ..., HASH_ZERO)
//   Raven: new hlist_head[1 << inum_hash_shift]     // bucket 数组，一次性分配

// 两个数组生命周期 = FS 生命周期，不做缩容
```

### 7.6 修正后的容关系

| 层 | 容器 | 成员 | 生命周期 | 写回触发 |
|----|------|------|----------|----------|
| VFS 逻辑层 | `inum_hashtable` (global hash) | `LogicalInode*` | LRU evict | N/A（不持盘上数据）|
| VFS 逻辑层 | `dentry_hashtable` (global hash) | `DentryEntry*` | LRU evict | N/A |
| VFS 逻辑层 | `inode_lru` | LogicalInode | 内存压力 | N/A |
| VFS 逻辑层 | `dirty_list` | LogicalInode（仅标记） | umount | 不直接刷 |
| 物理层 | 组相联 cache lines | `raven_Inode_t[16]` per line | LRU evict/析构 | **真正的写盘点** |
| 物理层 | cache line controllers | flags(V/D/T) + tag | 与 cache lines 同生 | — |

### 7.7 关键设计原则

> **专业的事交给专业的缓存层。**
>
> InodeSession 负责 raven_Inode_t 的生命周期管理。
> LogicalInode 只关心运行时元数据。
> dentry cache 只管 name→inum 映射。
> 三个层次各管各的淘汰，不互相绑架。
>
> 磁盘 I/O 次数降到最低——同一个物理 cache line 上的 16 个 inode，
> 不管哪个被改，最终一次 write_block 写完。

---

> **本文结论**
>
> Linux VFS 的 dentry/inode 两层缓存都是全局侵入式哈希表。RavenFS 上帝类在用户态用 Boost.Intrusive 可以实现相同的设计，同时借助 C++ 的类型安全和 RAII 避免内核那套宏操作。dentry_hashtable 和 inum_hashtable 是两个独立的侵入式容器，中间通过 inum 整数桥接——dentry 不持有 inode 引用，解决了 Linux 的 dentry→inode 循环 pin 问题。
> LogicalInode 不持原始磁盘数据，通过 InodeSession 从组相联缓存获取 raven_Inode_t* 指针，让物理缓存层全权管理数据生命周期和写回时机。
